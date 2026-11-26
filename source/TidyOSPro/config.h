// TidyOS Pro - TidyOS for the Heltec WiFi LoRa 32 V4 (ESP32-S3), with 900 MHz / AMI field tools.
// Pin map and build-time settings.
#pragma once
#include "sdkconfig.h"

#define TIDYOS_PRO 1
#define TIDYOS_NAME "TidyOS Pro"
#if CONFIG_IDF_TARGET_ESP32S3
  #define TIDYOS_V4 1          // Heltec V4 / V4-R8 (and V3: same radio, no FEM, no GNSS header)
  // Heltec V3 (ESP32-S3FN8: 8 MB flash, no PSRAM): built with -DTIDYOS_V3. Same pins as the V4 for the
  // OLED, radio, PRG, LED, Vext and battery; no front-end module and no GNSS header. TidyOS Pro only (no Shelf).
  #if defined(TIDYOS_V3)
    #define TIDYOS_BOARD "Heltec V3"
  #elif CONFIG_SPIRAM_MODE_OCT
    #define TIDYOS_BOARD "Heltec V4-R8"
  #else
    #define TIDYOS_BOARD "Heltec V4"
  #endif
  #ifndef TIDYOS_HTML5
    #define TIDYOS_HTML5       // no VGA on the S3: the desktop is always in the browser
  #endif
#endif

// Git-Store: the Cloudflare relay every board uses unless one is typed in (see gitstore/README.md)
#define TIDYOS_STORE_URL ""

#define TIDYOS_VERSION "3.4.41"

// Apps left out of every build to make room for the RF tools (build with -DTIDY_BROWSER=1 etc. to put one back)
#ifndef TIDY_BROWSER
#define TIDY_BROWSER 0
#endif
#ifndef TIDY_PICTURES
#define TIDY_PICTURES 0
#endif
#ifndef TIDY_HTML_APPS
#define TIDY_HTML_APPS 0
#endif

// Edition. The VGA edition draws on a monitor and uses Bluetooth keyboard + mouse.
// The HTML5 edition shows the desktop in a web browser (keyboard + mouse come from the browser).
// The web flasher has both; when building yourself, uncomment the next line for HTML5.
// #define TIDYOS_HTML5
#ifdef TIDYOS_HTML5
  #define TIDYOS_EDITION "HTML5"
#else
  #define TIDYOS_EDITION "VGA"
#endif

#ifdef TIDYOS_V4
// No VGA: the screen memory only feeds the browser. FabGL never drives these (headless).
#define VGA_RED1    GPIO_NUM_NC
#define VGA_RED0    GPIO_NUM_NC
#define VGA_GREEN1  GPIO_NUM_NC
#define VGA_GREEN0  GPIO_NUM_NC
#define VGA_BLUE1   GPIO_NUM_NC
#define VGA_BLUE0   GPIO_NUM_NC
#define VGA_HSYNC   GPIO_NUM_NC
#define VGA_VSYNC   GPIO_NUM_NC

// ---------------- microSD card (SPI) on the V4 ----------------
// TidyOS Pro uses the GNSS module, which sits on GPIO 38-42 (and 34), so the card goes on
// GPIO 4 / 6 / 47 / 48 (the V4-R8 layout of The Shelf) on every V4. If no GNSS is fitted,
// the Shelf's in-line layout (42/41/40/39) is tried too, so a board wired for The Shelf works.
#define SD_CS   4
#define SD_MISO 6
#define SD_CLK  47
#define SD_MOSI 48
#define SD_INLINE_CS   42
#define SD_INLINE_MOSI 41
#define SD_INLINE_CLK  40
#define SD_INLINE_MISO 39

// ---------------- Built in to the Heltec V4 ----------------
#define OLED_SDA   17
#define OLED_SCL   18
#define OLED_RST   21
#define PIN_PRG    0       // PRG button
#define PIN_VEXT   36      // LOW = OLED / Vext power on (V4-R8: GPIO 40)
#define PIN_VEXT_R8 40
#define PIN_VBAT   1       // battery voltage, 390k / 100k divider
#define PIN_ADC_CTRL 37
#define PIN_LED    35

// SX1262 LoRa radio
#define LORA_NSS   8
#define LORA_SCK   9
#define LORA_MOSI  10
#define LORA_MISO  11
#define LORA_RST   12
#define LORA_BUSY  13
#define LORA_DIO1  14
// Front-end module (GC1109 on V4.2, KCT8103L on V4.3). The V3 has none: these pins are left alone there.
#ifndef TIDYOS_V3
  #define HAS_FEM 1
  #define HAS_GNSS 1
#endif
#define FEM_POWER  7
#define FEM_EN     2
#define FEM_CPS    46
#define FEM_CTX    5

// GNSS header (L76K-type module). RX/TX are tried both ways round at start-up.
#define GNSS_RX    39      // ESP32 receives here (module TX)
#define GNSS_TX    38
#define GNSS_EN    34      // LOW = module powered
#define GNSS_RST   42
#define GNSS_WAKE  40
#define GNSS_PPS   41

#else
// ---------------- VGA (8 GPIOs, 2 bits per colour + sync) ----------------
// Same pinout as the LilyGO/TTGO "FabGL VGA32" board, so FabGL demos work too.
#define VGA_RED1    GPIO_NUM_22   // red   MSB  -> 270 ohm -> VGA pin 1
#define VGA_RED0    GPIO_NUM_21   // red   LSB  -> 820 ohm -> VGA pin 1
#define VGA_GREEN1  GPIO_NUM_19   // green MSB  -> 270 ohm -> VGA pin 2
#define VGA_GREEN0  GPIO_NUM_18   // green LSB  -> 820 ohm -> VGA pin 2
#define VGA_BLUE1   GPIO_NUM_5    // blue  MSB  -> 270 ohm -> VGA pin 3
#define VGA_BLUE0   GPIO_NUM_4    // blue  LSB  -> 820 ohm -> VGA pin 3
#define VGA_HSYNC   GPIO_NUM_23   // -> 100 ohm -> VGA pin 13
#define VGA_VSYNC   GPIO_NUM_15   // -> 100 ohm -> VGA pin 14

// ---------------- microSD card (SPI) ----------------
#define SD_MISO 16
#define SD_MOSI 17
#define SD_CLK  14
#define SD_CS   13

// HTML5 edition only: the microSD module plugged straight onto GND, 19, 18, 5, 17 (the same
// "in-line" wiring as The Shelf on an ESP32). The VGA edition can't use it: 19, 18 and 5 are
// VGA colour pins there. The HTML5 edition tries this wiring first, then the one above.
#define SD_INLINE_MISO 19
#define SD_INLINE_CLK  18
#define SD_INLINE_MOSI 5
#define SD_INLINE_CS   17
#endif  // TIDYOS_V4

#define SD_MOUNT     "/SD"
#define FLASH_MOUNT  "/flash"
// Tidy Pro Plus (V4): the board's storage is the 7.4 MB "ffat" area, shared with The Shelf; TidyOS Pro's
// files live in its own folder there. (Classic ESP32: the SPIFFS area itself.)
#ifdef TIDYOS_V4
  #define FLASH_HOME FLASH_MOUNT "/TidyPro"
#else
  #define FLASH_HOME FLASH_MOUNT
#endif

// Screen
#define SCREEN_W 640
#define SCREEN_H 480
