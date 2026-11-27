// TidyOS Pro - a real VGA monitor on the Seeed XIAO ESP32S3 (built with -DTIDYOS_XIAO).
//
// The desktop is drawn by FabGL into its screen memory in PSRAM ("headless"), which the browser reads.
// This sends the same picture to a VGA monitor at 640x480, 60 Hz, using the ESP32-S3's LCD peripheral
// as an 8-bit parallel port: every pixel clock it puts out one byte, and five of its bits go to pins:
//
//   bit 0  red     D0 (GPIO 1)  -> 270 ohm -> VGA pin 1
//   bit 1  green   D1 (GPIO 2)  -> 270 ohm -> VGA pin 2
//   bit 2  blue    D2 (GPIO 3)  -> 270 ohm -> VGA pin 3
//   bit 3  H sync  D3 (GPIO 4)  -> 100 ohm -> VGA pin 13
//   bit 4  V sync  D6 (GPIO 43) -> 100 ohm -> VGA pin 14
//   GND -> VGA pins 5, 6, 7, 8 and 10
//
// The sync pulses are part of the data, so the timing never drifts. DMA plays a ring of 8 line buffers
// (internal RAM, ~6.4 KB) over and over; each time one has been sent, an interrupt (on core 1, the
// desktop's core: the radio, Wi-Fi and the browser screen are on core 0) fills it with the line 8 ahead.
//
// One bit per colour gives 8 colours. TidyOS's palette has in-between shades (grey, dark blue, orange...),
// so those are mixed from a 2x2 pattern of the 8 (a light dither) and still look like their colour.
// The mouse pointer is drawn on top of each line here, so it shows on the monitor but never in the
// browser's copy of the screen.
#include "config.h"
#ifdef TIDYOS_VGA_OUT
#include <Arduino.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_private/gdma.h"
#include "hal/dma_types.h"
#include "hal/lcd_ll.h"
#include "soc/lcd_cam_struct.h"
#include "soc/gpio_sig_map.h"
#include "driver/periph_ctrl.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "fabgl.h"
#include "core.h"

// ---- 640x480 at 60 Hz (VESA): 800 x 525 pixel clocks, both syncs negative.
// Pixel clock: PLL 240 MHz / 9.5 = 25.26 MHz (the standard is 25.175: every monitor takes this).
static const int H_VIS = 640, H_FRONT = 16, H_SYNC = 96, H_BACK = 48, H_TOTAL = 800;
static const int V_VIS = 480, V_FRONT = 10, V_SYNC = 2, V_BACK = 33, V_TOTAL = 525;
static const int NBUF = 8;                       // line buffers in the DMA ring

static const uint8_t HS = 1 << 3, VS = 1 << 4;   // sync bits: high = idle, low = pulse

static gdma_channel_handle_t s_dma = nullptr;
static dma_descriptor_t * s_desc = nullptr;      // NBUF descriptors, each one line, linked in a ring
static uint8_t * s_buf[NBUF];
static volatile bool s_on = false;
static volatile int s_fillIdx = 0;               // the next buffer the DMA will finish
static volatile int s_fillLine = 0;              // the line that goes into it
static volatile int s_mouseX = -100, s_mouseY = -100;
static volatile int s_mode = 8;                  // 2, 4 or 8 colours (how the screen memory is packed)

// palette -> pin byte. [y & 1] picks the dither row, [x & 1] is folded into each table.
static uint16_t s_lut8[2][64];                   // 8 colours: two 3-bit pixels -> two bytes
static uint32_t s_lut4[2][256];                  // 4 colours: one byte (4 pixels) -> four bytes
static uint32_t s_lut2[2][16];                   // 2 colours: half a byte (4 pixels) -> four bytes
static uint8_t s_pal[8][2][2];                   // [index][y & 1][x & 1] -> RGB bits

// one colour channel (0-255) as on / off at this spot of a 2x2 pattern: 0 = off, 85 = 1 of 4,
// 170 = 3 of 4, 255 = on
static inline int ditherBit(int v, int xo, int yo)
{
  static const uint8_t B[2][2] = { { 0, 2 }, { 3, 1 } };
  int level = (v + 42) / 85;                     // 0..3
  return level * 4 > (B[yo][xo] * 3 + 1) ? 1 : 0;
}

void vgaS3Palette(const RGB888 * pal, int colors)
{
  s_mode = colors;
  for (int i = 0; i < 8; ++i)
    for (int yo = 0; yo < 2; ++yo)
      for (int xo = 0; xo < 2; ++xo) {
        RGB888 c = i < colors ? pal[i] : RGB888(0, 0, 0);
        s_pal[i][yo][xo] = ditherBit(c.R, xo, yo) | ditherBit(c.G, xo, yo) << 1 | ditherBit(c.B, xo, yo) << 2;
      }
  for (int yo = 0; yo < 2; ++yo) {
    for (int a = 0; a < 8; ++a)
      for (int b = 0; b < 8; ++b)          // first pixel at an even x, second at an odd x
        s_lut8[yo][a << 3 | b] = (s_pal[a][yo][0] | HS | VS) | (s_pal[b][yo][1] | HS | VS) << 8;
    for (int v = 0; v < 256; ++v) {        // 4 colours: pixel 0 in bits 7-6
      uint32_t w = 0;
      for (int p = 0; p < 4; ++p) w |= (uint32_t)(s_pal[(v >> (6 - p * 2)) & 3][yo][p & 1] | HS | VS) << (p * 8);
      s_lut4[yo][v] = w;
    }
    for (int v = 0; v < 16; ++v) {         // 2 colours: pixel 0 in bit 3 of the nibble
      uint32_t w = 0;
      for (int p = 0; p < 4; ++p) w |= (uint32_t)(s_pal[(v >> (3 - p)) & 1][yo][p & 1] | HS | VS) << (p * 8);
      s_lut2[yo][v] = w;
    }
  }
}

void vgaS3Mouse(int x, int y) { s_mouseX = x; s_mouseY = y; }

// the pointer: 12 x 19, '#' = black outline, '.' = white
static const char * const ARROW[19] = {
  "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ", "#....#      ",
  "#.....#     ", "#......#    ", "#.......#   ", "#........#  ", "#.........# ", "#......#####",
  "#...#..#    ", "#..# #..#   ", "#.#  #..#   ", "##    #..#  ", "#     #..#  ", "       #..# ", "        ##  ",
};

static void IRAM_ATTR fillLine(uint8_t * d, int line)
{
  const uint8_t vs = (line >= V_VIS + V_FRONT && line < V_VIS + V_FRONT + V_SYNC) ? 0 : VS;
  // blanking: front porch, H sync pulse, back porch (the visible part comes first in each line)
  memset(d + H_VIS, HS | vs, H_FRONT);
  memset(d + H_VIS + H_FRONT, vs, H_SYNC);
  memset(d + H_VIS + H_FRONT + H_SYNC, HS | vs, H_BACK);
  if (line >= V_VIS) { memset(d, HS | vs, H_VIS); return; }

  const uint8_t * src = fabgl::VGAPalettedController::sgetScanline(line);
  const int yo = line & 1;
  uint32_t * o = (uint32_t *)d;
  if (!src) { memset(d, HS | VS, H_VIS); return; }
  if (s_mode == 8) {
    // 8 pixels in 3 bytes (FabGL's VGA8 order): read as 24 bits, pixel 0 at bits 21-23
    const uint16_t * lut = s_lut8[yo];
    for (int x = 0; x < H_VIS; x += 8, src += 3) {
      uint32_t v = src[0] | src[1] << 8 | src[2] << 16;
      o[0] = lut[(v >> 18) & 0x3f] | (uint32_t)lut[(v >> 12) & 0x3f] << 16;
      o[1] = lut[(v >> 6) & 0x3f]  | (uint32_t)lut[v & 0x3f] << 16;
      o += 2;
    }
  } else if (s_mode == 4) {
    const uint32_t * lut = s_lut4[yo];
    for (int x = 0; x < H_VIS; x += 4) *o++ = lut[*src++];
  } else {
    const uint32_t * lut = s_lut2[yo];
    for (int x = 0; x < H_VIS; x += 8, ++src) { *o++ = lut[*src >> 4]; *o++ = lut[*src & 15]; }
  }
  // the pointer on top
  int r = line - s_mouseY, mx = s_mouseX;
  if (r >= 0 && r < 19 && mx > -12 && mx < H_VIS) {
    const char * a = ARROW[r];
    for (int i = 0; i < 12; ++i) {
      int x = mx + i;
      if (x < 0 || x >= H_VIS || a[i] == ' ') continue;
      d[x] = (a[i] == '.' ? 7 : 0) | HS | VS;
    }
  }
}

// a line buffer has been sent: refill it (and any the interrupt missed) with the line NBUF ahead
static bool IRAM_ATTR onEof(gdma_channel_handle_t, gdma_event_data_t * ev, void *)
{
  int done = ((dma_descriptor_t *)ev->tx_eof_desc_addr) - s_desc;
  if (done < 0 || done >= NBUF) return false;
  for (int guard = 0; guard < NBUF; ++guard) {
    int i = s_fillIdx;
    fillLine(s_buf[i], s_fillLine);
    s_fillLine = s_fillLine + 1 == V_TOTAL ? 0 : s_fillLine + 1;
    s_fillIdx = i + 1 == NBUF ? 0 : i + 1;
    if (i == done) break;
  }
  return false;
}

static const int PINS[5] = { XVGA_R, XVGA_G, XVGA_B, XVGA_HS, XVGA_VS };

static bool start()
{
  if (!s_desc) {
    s_desc = (dma_descriptor_t *)heap_caps_calloc(NBUF, sizeof(dma_descriptor_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    for (int i = 0; i < NBUF; ++i) s_buf[i] = (uint8_t *)heap_caps_aligned_alloc(4, H_TOTAL, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    for (int i = 0; s_desc && i < NBUF; ++i) if (!s_buf[i]) { heap_caps_free(s_desc); s_desc = nullptr; }
    if (!s_desc) { Serial.println("[VGA] no memory for the line buffers"); return false; }
  }
  for (int i = 0; i < NBUF; ++i) {
    fillLine(s_buf[i], i);
    s_desc[i].dw0.size = H_TOTAL;
    s_desc[i].dw0.length = H_TOTAL;
    s_desc[i].dw0.suc_eof = 1;                   // an interrupt after every line
    s_desc[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    s_desc[i].buffer = s_buf[i];
    s_desc[i].next = &s_desc[(i + 1) % NBUF];
  }
  s_fillIdx = 0;
  s_fillLine = NBUF;

  periph_module_enable(PERIPH_LCD_CAM_MODULE);
  periph_module_reset(PERIPH_LCD_CAM_MODULE);
  lcd_cam_dev_t * dev = &LCD_CAM;
  dev->lcd_user.lcd_reset = 1;
  dev->lcd_user.lcd_reset = 0;
  lcd_ll_enable_clock(dev, true);
  lcd_ll_select_clk_src(dev, LCD_CLK_SRC_PLL240M);
  lcd_ll_set_group_clock_coeff(dev, 9, 2, 1);    // 240 MHz / (9 + 1/2) = 25.26 MHz
  lcd_ll_set_pixel_clock_prescale(dev, 1);       // pixel clock = that
  lcd_ll_set_clock_idle_level(dev, false);
  lcd_ll_set_pixel_clock_edge(dev, false);
  dev->lcd_ctrl.lcd_rgb_mode_en = 0;             // i8080 mode: just bytes out, no timing generator
  dev->lcd_rgb_yuv.lcd_conv_bypass = 0;          // no colour conversion
  dev->lcd_misc.lcd_bk_en = 0;
  dev->lcd_misc.lcd_next_frame_en = 0;
  dev->lcd_user.lcd_2byte_en = 0;                // 8-bit bus
  dev->lcd_user.lcd_cmd = 0;
  dev->lcd_user.lcd_dummy = 0;
  dev->lcd_user.lcd_dout = 1;
  dev->lcd_user.lcd_dout_cyclelen = 0;
  dev->lcd_user.lcd_always_out_en = 1;           // keep going for as long as the DMA feeds it
  dev->lcd_user.lcd_8bits_order = 0;
  dev->lcd_user.lcd_bit_order = 0;
  dev->lcd_user.lcd_byte_order = 0;
  dev->lcd_dly_mode.val = 0;
  dev->lcd_data_dout_mode.val = 0;

  for (int b = 0; b < 5; ++b) {
    gpio_reset_pin((gpio_num_t)PINS[b]);
    gpio_set_direction((gpio_num_t)PINS[b], GPIO_MODE_OUTPUT);
    gpio_set_drive_capability((gpio_num_t)PINS[b], GPIO_DRIVE_CAP_3);
    esp_rom_gpio_connect_out_signal(PINS[b], LCD_DATA_OUT0_IDX + b, false, false);
  }

  if (!s_dma) {
    gdma_channel_alloc_config_t ac = {};
    ac.direction = GDMA_CHANNEL_DIRECTION_TX;
    if (gdma_new_channel(&ac, &s_dma) != ESP_OK) { Serial.println("[VGA] no DMA channel"); s_dma = nullptr; return false; }
    gdma_connect(s_dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));
    gdma_strategy_config_t sc = { .owner_check = false, .auto_update_desc = false };
    gdma_apply_strategy(s_dma, &sc);
    gdma_transfer_ability_t ab = { .sram_trans_align = 4, .psram_trans_align = 64 };
    gdma_set_transfer_ability(s_dma, &ab);
    gdma_tx_event_callbacks_t cb = { .on_trans_eof = onEof };
    gdma_register_tx_event_callbacks(s_dma, &cb, nullptr);   // (its interrupt lands on this core: core 1)
  }
  gdma_reset(s_dma);
  lcd_ll_fifo_reset(dev);
  gdma_start(s_dma, (intptr_t)&s_desc[0]);
  esp_rom_delay_us(1);
  dev->lcd_user.lcd_update = 1;
  dev->lcd_user.lcd_start = 1;
  Serial.println("[VGA] 640x480 60 Hz on D0 D1 D2 (R G B), D3 (H sync), D6 (V sync)");
  return true;
}

static void stop()
{
  LCD_CAM.lcd_user.lcd_start = 0;
  LCD_CAM.lcd_user.lcd_update = 1;
  if (s_dma) gdma_stop(s_dma);
  for (int b = 0; b < 5; ++b) gpio_reset_pin((gpio_num_t)PINS[b]);   // the pins go back to plain inputs
  Serial.println("[VGA] off");
}

void vgaS3SetOn(bool on)
{
  if (on == s_on) return;
  if (on) s_on = start();
  else { stop(); s_on = false; }
}

bool vgaS3On() { return s_on; }

#endif  // TIDYOS_VGA_OUT
