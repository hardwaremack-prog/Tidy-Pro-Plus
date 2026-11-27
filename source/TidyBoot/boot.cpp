// TidyBoot - the boot menu for a dual-boot Heltec V4: The Shelf and TidyOS Pro on one board.
//
// It lives in the small "factory" slot, so the board always starts here. It:
//   1. installs an update waiting on the microSD card (or in The Shelf's internal storage):
//        /tidyos/update.bin + update.sha   -> the TidyOS Pro slot   (TidyOS Pro's own updater;
//                                              on the card, or in the board's storage with no card)
//        /.shelf/update-app.bin            -> The Shelf's slot       (The Shelf's own updater)
//   2. shows the menu on the OLED for 5 seconds, with the one used last already picked:
//        PRG short press = switch between The Shelf and TidyOS Pro
//        PRG held for 1 second = start the one picked now
//      and then starts the one picked (the last one used, unless you change it). Whatever
//      starts is remembered, so it's the one picked next time.
//   3. With only one of them installed, it starts that one straight away.
//
// Each system sets the board back to start here, so every restart (or power-on) comes back to
// this menu. Waking from deep sleep skips the countdown and goes straight back to the last one.
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <SD.h>
#include <FFat.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include "mbedtls/sha256.h"
#include <string>

#if defined(TIDYOS_XIAO)       // Seeed XIAO ESP32S3 + Wio-SX1262 (build.sh passes -DTIDYOS_XIAO)
  #define OLED_SDA 5            // a plug-in OLED on D4 / D5, if there is one
  #define OLED_SCL 6
  #define OLED_RST U8X8_PIN_NONE
  #define PIN_PRG  21           // the Wio-SX1262's button
  #define PIN_LED  -1           // (no LED to spare)
  #define PIN_VEXT -1           // (no Vext switch)
#else
#define OLED_SDA 17
#define OLED_SCL 18
#define OLED_RST 21
#define PIN_PRG  0
#define PIN_LED  35
#endif
#if defined(TIDYOS_XIAO)
#elif CONFIG_SPIRAM_MODE_OCT
  #define PIN_VEXT 40         // V4-R8
#else
  #define PIN_VEXT 36
#endif
#define COUNTDOWN_S 5

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA);
static SPIClass sdSpi(FSPI);
static bool s_card = false, s_ffat = false;
static const esp_partition_t * s_tidy = nullptr, * s_shelf = nullptr;

static bool hasApp(const esp_partition_t * p)
{
  if (!p) return false;
  uint8_t m = 0;
  esp_partition_read(p, 0, &m, 1);
  return m == 0xE9;               // an app image starts with 0xE9
}

static void screen(const char * a, const char * b = "", const char * c = "", const char * d = "")
{
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 11, a); oled.drawStr(0, 26, b); oled.drawStr(0, 41, c); oled.drawStr(0, 56, d);
  oled.sendBuffer();
}

static void startApp(const esp_partition_t * p, const char * name)
{
  Serial.printf("[TidyBoot] starting %s\n", name);
  Preferences pr;
  if (pr.begin("tidyboot", false)) { pr.putString("last", p == s_shelf ? "shelf" : "tidyos"); pr.end(); }
  screen("Starting", name);
  esp_ota_set_boot_partition(p);
  delay(300);
  ESP.restart();
}

static bool mountCard(int cs, int miso, int clk, int mosi)
{
  sdSpi.begin(clk, miso, mosi, cs);
  for (int pin : { miso, mosi, cs }) gpio_pullup_en((gpio_num_t)pin);
  for (uint32_t hz : { 10000000u, 4000000u }) {
    if (SD.begin(cs, sdSpi, hz, "/sd", 2)) return true;
    SD.end();
  }
  sdSpi.end();
  return false;
}

static std::string hexOf(const uint8_t * h)
{
  char b[65];
  for (int i = 0; i < 32; ++i) sprintf(b + i * 2, "%02x", h[i]);
  return std::string(b, 64);
}

// copies `f` into partition `p` with the OTA functions (checks the image as it goes)
static bool install(fs::File & f, const esp_partition_t * p, const char * name)
{
  size_t size = f.size();
  if (size < 65536 || size > p->size) { Serial.println("[TidyBoot] wrong size"); return false; }
  esp_ota_handle_t h;
  if (esp_ota_begin(p, size, &h) != ESP_OK) return false;
  static uint8_t buf[4096];
  size_t done = 0;
  int lastPct = -1;
  while (done < size) {
    int n = f.read(buf, std::min(sizeof(buf), size - done));
    if (n <= 0 || esp_ota_write(h, buf, n) != ESP_OK) { esp_ota_abort(h); return false; }
    done += n;
    if (PIN_LED >= 0) digitalWrite(PIN_LED, (millis() / 80) & 1);
    int pct = done * 100 / size;
    if (pct != lastPct) { lastPct = pct; char b[24]; snprintf(b, sizeof(b), "%d%%", pct); screen("Installing", name, b, "Don't unplug it"); }
  }
  bool ok = esp_ota_end(h) == ESP_OK;
  if (PIN_LED >= 0) digitalWrite(PIN_LED, LOW);
  return ok;
}

static void note(const char * why)       // for TidyOS Pro to show after it starts (same as its old loader)
{
  if (s_card) { File l = SD.open("/tidyos/update.log", FILE_WRITE); if (l) { l.print(why); l.close(); return; } }
  Preferences p; if (p.begin("tidyload", false)) { p.putString("note", why); p.end(); }
}

static void tidyUpdate()
{
  if (!s_tidy) return;
  fs::FS * where = nullptr;
  if (s_card && SD.exists("/tidyos/update.bin")) where = &SD;
  else if (s_ffat && FFat.exists("/tidyos/update.bin")) where = &FFat;
  if (!where) return;
  File f = where->open("/tidyos/update.bin", FILE_READ);
  if (!f) return;
  if (f.size() == 0) { f.close(); return; }           // emptied leftover
  std::string want;
  File s = where->open("/tidyos/update.sha", FILE_READ);
  if (s) { want = s.readString().c_str(); s.close(); }
  while (!want.empty() && (want.back() == '\n' || want.back() == '\r' || want.back() == ' ')) want.pop_back();
  screen("TidyOS Pro update", "Checking the file...");
  mbedtls_sha256_context c; uint8_t h[32]; static uint8_t buf[4096];
  mbedtls_sha256_init(&c); mbedtls_sha256_starts_ret(&c, 0);
  size_t n; while ((n = f.read(buf, sizeof(buf))) > 0) mbedtls_sha256_update_ret(&c, buf, n);
  mbedtls_sha256_finish_ret(&c, h);
  if (want.size() != 64 || hexOf(h) != want) {
    f.close();
    if (where == &SD) SD.rename("/tidyos/update.bin", "/tidyos/update.bad"); else FFat.remove("/tidyos/update.bin");
    note("the update file was damaged - nothing changed");
    return;
  }
  f.seek(0);
  bool ok = install(f, s_tidy, "TidyOS Pro");
  f.close();
  where->remove("/tidyos/update.bin"); where->remove("/tidyos/update.sha"); where->remove("/tidyos/update.ver");
  note(ok ? "update installed" : "the update didn't go in - the old version is still there");
  if (ok || hasApp(s_tidy)) startApp(s_tidy, "TidyOS Pro");
}

static void shelfUpdate()
{
  if (!s_shelf) return;
  fs::FS * where = nullptr;
  if (s_card && SD.exists("/.shelf/update-app.bin")) where = &SD;
  else if (s_ffat && FFat.exists("/.shelf/update-app.bin")) where = &FFat;
  if (!where) return;
  File f = where->open("/.shelf/update-app.bin", FILE_READ);
  uint8_t m = 0; f.read(&m, 1); f.seek(0);
  bool ok = m == 0xE9 && install(f, s_shelf, "The Shelf");
  f.close();
  where->remove("/.shelf/update-app.bin");
  if (ok || hasApp(s_shelf)) startApp(s_shelf, "The Shelf");
}

static void drawMenu(int sel, int secsLeft)
{
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 10, "Start which one?");
  const char * names[2] = { "TidyOS Pro", "The Shelf" };
  for (int i = 0; i < 2; ++i) {
    int y = 16 + i * 16;
    if (i == sel) { oled.drawBox(0, y, 128, 15); oled.setDrawColor(0); }
    oled.setFont(u8g2_font_7x14B_tr);
    oled.drawStr(6, y + 12, names[i]);
    oled.setDrawColor(1);
  }
  oled.setFont(u8g2_font_5x8_tr);
  char b[40]; snprintf(b, sizeof(b), "PRG: switch  hold: go  %2ds", secsLeft);
  oled.drawStr(0, 58, b);
  oled.drawFrame(0, 60, 128, 4);
  oled.drawBox(0, 60, 128 * secsLeft / COUNTDOWN_S, 4);
  oled.sendBuffer();
}

void setup()
{
  Serial.begin(115200);
  delay(50);
  Serial.println("\n[TidyBoot] boot menu");
  if (PIN_LED >= 0) { pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW); }
  pinMode(PIN_PRG, INPUT_PULLUP);
  if (PIN_VEXT >= 0) { pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW); }   // screen power on
  delay(60);
  oled.begin(); oled.setContrast(200);

  s_tidy  = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "tidyos");
  s_shelf = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "shelf");

  // updates first (the card in either wiring; The Shelf may keep its update in internal storage)
#if defined(TIDYOS_XIAO)
  pinMode(41, OUTPUT); digitalWrite(41, HIGH);                       // the LoRa radio's chip select: off the shared bus
  s_card = mountCard(44, 8, 7, 9);                                    // CS D7, MISO D9, CLK D8, MOSI D10
#else
  s_card = mountCard(4, 6, 47, 48) || mountCard(42, 39, 40, 41);
#endif
  s_ffat = FFat.begin(false, "/ffat", 2, "ffat");
  tidyUpdate();
  shelfUpdate();

  bool haveT = hasApp(s_tidy), haveS = hasApp(s_shelf);
  if (!haveT && !haveS) { screen("No system installed", "", "Install from the", "TidyOS Pro web page"); for (;;) delay(1000); }
  if (haveT != haveS) startApp(haveT ? s_tidy : s_shelf, haveT ? "TidyOS Pro" : "The Shelf");

  Preferences pr;
  String last = "tidyos", once = "";
  if (pr.begin("tidyboot", false)) { last = pr.getString("last", "tidyos"); once = pr.getString("once", ""); if (once.length()) pr.remove("once"); pr.end(); }
  if (once == "shelf") startApp(s_shelf, "The Shelf");               // asked for from the other system ("Switch to ...")
  if (once == "tidyos") startApp(s_tidy, "TidyOS Pro");
  int sel = last == "shelf" ? 1 : 0;
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED)     // woken from deep sleep: back to where it was
    startApp(sel ? s_shelf : s_tidy, sel ? "The Shelf" : "TidyOS Pro");

  uint32_t t0 = millis(), downAt = 0;
  bool wasDown = digitalRead(PIN_PRG) == LOW;                        // still held from waking it: ignore until let go
  int shown = -1;
  for (;;) {
    int left = COUNTDOWN_S - (int)((millis() - t0) / 1000);
    if (left <= 0) break;
    bool down = digitalRead(PIN_PRG) == LOW;
    if (down && !wasDown) downAt = millis();
    if (down && downAt && millis() - downAt > 1000) break;            // held: start now
    if (!down && wasDown && downAt && millis() - downAt < 1000) { sel ^= 1; t0 = millis(); shown = -1; }   // pressed: switch, restart the count
    if (!down) downAt = 0;
    wasDown = down;
    if (left * 10 + sel != shown) { shown = left * 10 + sel; drawMenu(sel, left); }
    delay(15);
  }
  startApp(sel ? s_shelf : s_tidy, sel ? "The Shelf" : "TidyOS Pro");
}

void loop() {}
