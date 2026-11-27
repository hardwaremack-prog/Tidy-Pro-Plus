// Tidy Pro Plus - the Heltec V4's little OLED (128 x 64): rolling pages of the desktop's live data.
//
// Pages: address, clock, weather, wind + humidity, sun + HF bands, storms + alerts, 900 MHz, system, stopwatch.
// It moves to the next page every 30 seconds. The PRG button:
//   tap once        hold this page (tap once more to roll again)
//   tap twice       next page
//   hold 1 s        stopwatch page: start / stop
//   hold 3 s        stopwatch page: reset
// After 30 minutes without a press it dims; any press brightens it again (that press does nothing else).
#include "config.h"
#ifdef TIDYOS_V4
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <U8g2lib.h>
#include <mutex>
#include <math.h>
#include "rf_engine.h"
#include "net.h"
#include "screen.h"
#include "core.h"
#include "oled_data.h"
using fabgl::imin; using fabgl::imax;

bool musicQuickPlaying();      // media_apps.h
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_oled(U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA);
static const uint32_t PAGE_MS = 30000, DIM_MS = 30UL * 60 * 1000, SLEEP_MS = 5UL * 60 * 1000, TAP_GAP_MS = 400, LONG_MS = 1000, RESET_MS = 3000;
static const uint8_t BRIGHT = 160, DIM = 8;

enum Page { P_ADDRESS, P_CLOCK, P_WEATHER, P_WIND, P_SUN, P_STORMS, P_NEWS, P_RADIO, P_SYSTEM, P_STOPWATCH, P_COUNT };
static const char * TITLE[P_COUNT] = { "ADDRESS", "CLOCK", "WEATHER", "WIND + HUMIDITY", "SUN + HF BANDS", "STORMS + ALERTS", "NEWS", "900 MHz", "SYSTEM", "STOPWATCH" };

// stopwatch
static bool s_swRun = false;
static uint32_t s_swStart = 0, s_swAcc = 0;                  // ms counted before the current run
static uint32_t swMs() { return s_swAcc + (s_swRun ? millis() - s_swStart : 0); }
static void swFmt(char * b, size_t n, uint32_t ms, bool tenths)
{
  uint32_t s = ms / 1000, h = s / 3600, m = (s / 60) % 60;
  if (h) snprintf(b, n, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)(s % 60));
  else if (tenths) snprintf(b, n, "%02u:%02u.%u", (unsigned)m, (unsigned)(s % 60), (unsigned)(ms / 100 % 10));
  else snprintf(b, n, "%u:%02u", (unsigned)m, (unsigned)(s % 60));
}

static DeskSnap s_d;                                         // the desktop's data (refreshed every page draw)
static float s_spec[rf::NCH]; static bool s_specOk = false; static float s_nf = -120, s_occ = 0; static bool s_over = false;

static void centre(const char * t, int y) { s_oled.drawStr((128 - s_oled.getStrWidth(t)) / 2, y, t); }
// draws a line of text, cut short so it fits on the screen
static void fit(int x, int y, const char * t)
{
  char b[64]; strlcpy(b, t, sizeof(b));
  size_t n = strlen(b);
  while (n > 1 && x + s_oled.getStrWidth(b) > 128) b[--n] = 0;
  s_oled.drawStr(x, y, b);
}

// a little line graph of a 3-hourly series; bars = draw it as bars
static void graph(const DeskSeries & f, int x, int y, int w, int h, int lo, int hi, bool bars)
{
  s_oled.drawFrame(x, y, w, h);
  if (f.n < 2 || hi <= lo) return;
  auto X = [&](int i) { return x + 1 + i * (w - 3) / (f.n - 1); };
  auto Y = [&](int v) { v = v < lo ? lo : v > hi ? hi : v; return y + h - 2 - (v - lo) * (h - 3) / (hi - lo); };
  for (int i = 0; i < f.n; ++i) {
    if (!f.ok[i]) continue;
    if (bars) s_oled.drawVLine(X(i), Y(f.v[i]), y + h - 1 - Y(f.v[i]));
    else if (i && f.ok[i - 1]) s_oled.drawLine(X(i - 1), Y(f.v[i - 1]), X(i), Y(f.v[i]));
  }
}
static int smax(const DeskSeries & f, int d) { int m = -99999; for (int i = 0; i < f.n; ++i) if (f.ok[i] && f.v[i] > m) m = f.v[i]; return m == -99999 ? d : m; }
static int slast(const DeskSeries & f) { for (int i = f.n - 1; i >= 0; --i) if (f.ok[i]) return f.v[i]; return -99999; }
static const char * compass(int deg) { static const char * C[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" }; return C[((deg + 22) / 45) % 8]; }

static void pageClock()
{
  time_t now = time(nullptr);
  char b[40];
  if (now < 1600000000) { s_oled.setFont(u8g2_font_6x12_tr); centre("Waiting for the clock", 34); }
  else {
    struct tm t; localtime_r(&now, &t);
    int h = t.tm_hour % 12; if (!h) h = 12;
    snprintf(b, sizeof(b), "%d:%02d:%02d", h, t.tm_min, t.tm_sec);
    s_oled.setFont(u8g2_font_logisoso22_tn);
    int w = s_oled.getStrWidth(b);
    s_oled.drawStr((116 - w) / 2, 37, b);
    s_oled.setFont(u8g2_font_6x12_tr);
    s_oled.drawStr((116 - w) / 2 + w + 2, 37, t.tm_hour < 12 ? "AM" : "PM");
    static const char * D[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char * M[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    snprintf(b, sizeof(b), "%s %s %d, %d", D[t.tm_wday % 7], M[t.tm_mon % 12], t.tm_mday, t.tm_year + 1900);
    centre(b, 50);
  }
  s_oled.setFont(u8g2_font_5x8_tr);
  if (netConnected()) snprintf(b, sizeof(b), "http://%s", WiFi.localIP().toString().c_str());
  else snprintf(b, sizeof(b), "Hotspot: http://10.1.1.250");
  centre(b, 62);
}

// how to reach the desktop: the address on the home Wi-Fi, or how to join the board's own hotspot
static void pageAddress()
{
  char b[48];
  if (netConnected()) {
    s_oled.setFont(u8g2_font_5x8_tr); centre("Open in a browser:", 19);
    snprintf(b, sizeof(b), "%s", WiFi.localIP().toString().c_str());
    s_oled.setFont(u8g2_font_7x14B_tr);
    if (s_oled.getStrWidth(b) > 128) s_oled.setFont(u8g2_font_6x12_tr);
    centre(b, 35);
    s_oled.setFont(u8g2_font_5x8_tr);
    snprintf(b, sizeof(b), "%s.local", boardHost().c_str()); fit(imax(0, (128 - s_oled.getStrWidth(b)) / 2), 47, b);
    snprintf(b, sizeof(b), "on %s", WiFi.SSID().c_str()); fit(imax(0, (128 - s_oled.getStrWidth(b)) / 2), 58, b);
  } else {
    s_oled.setFont(u8g2_font_5x8_tr); centre("Join the Wi-Fi:", 19);
    s_oled.setFont(u8g2_font_7x14B_tr);
    snprintf(b, sizeof(b), "%s", boardId().c_str());
    if (s_oled.getStrWidth(b) > 128) s_oled.setFont(u8g2_font_6x12_tr);
    centre(b, 34);
    s_oled.setFont(u8g2_font_5x8_tr);
    snprintf(b, sizeof(b), "password %s", SCREEN_AP_PASS); centre(b, 46);
    centre("then open 10.1.1.250", 57);
  }
}

static void pageWeather()
{
  char b[48];
  if (!s_d.wxOk) {
    s_oled.setFont(u8g2_font_6x12_tr);
    centre("Weather shows when", 30); centre("the desktop is open", 42); centre("in a browser", 54);
    return;
  }
  snprintf(b, sizeof(b), "%d", s_d.temp);
  s_oled.setFont(u8g2_font_logisoso22_tn);
  s_oled.drawStr(0, 35, b);
  int w = s_oled.getStrWidth(b);
  s_oled.drawCircle(w + 3, 15, 2);                           // the degree sign
  s_oled.setFont(u8g2_font_6x12_tr);
  s_oled.drawStr(w + 7, 22, "F");
  fit(w + 16, 22, s_d.cond);
  s_oled.setFont(u8g2_font_5x8_tr);
  snprintf(b, sizeof(b), "H%d L%d %s", s_d.hi, s_d.lo, s_d.city);
  fit(w + 16, 34, b);
  // the last 4 days of temperatures, with a dotted freezing line
  if (s_d.tempsN > 1) {
    int lo = 999, hi = -999;
    for (int i = 0; i < s_d.tempsN; ++i) { lo = imin(lo, s_d.temps[i]); hi = imax(hi, s_d.temps[i]); }
    lo -= 2; hi += 2;
    const int gx = 0, gy = 40, gw = 128, gh = 24;
    s_oled.drawFrame(gx, gy, gw, gh);
    auto Y = [&](int v) { return gy + gh - 2 - (v - lo) * (gh - 3) / imax(1, hi - lo); };
    if (32 > lo && 32 < hi) for (int k = gx + 1; k < gx + gw - 1; k += 3) s_oled.drawPixel(k, Y(32));
    for (int i = 1; i < s_d.tempsN; ++i)
      s_oled.drawLine(gx + 1 + (i - 1) * (gw - 3) / (s_d.tempsN - 1), Y(s_d.temps[i - 1]), gx + 1 + i * (gw - 3) / (s_d.tempsN - 1), Y(s_d.temps[i]));
  }
}

static void pageWind()
{
  char b[48];
  s_oled.setFont(u8g2_font_5x8_tr);
  if (s_d.haveNow) snprintf(b, sizeof(b), "Wind %d mph %s  gust %d", s_d.windNow, compass(s_d.windDir), s_d.gustNow);
  else snprintf(b, sizeof(b), "Wind (mph): looking it up");
  fit(0, 17, b);
  int wmax = imax(15, imax(smax(s_d.gust, 15), smax(s_d.wind, 15)) + 2);
  graph(s_d.gust, 0, 19, 128, 18, 0, wmax, true);
  graph(s_d.wind, 0, 19, 128, 18, 0, wmax, false);
  if (s_d.haveNow) snprintf(b, sizeof(b), "Hum %d%%  dew %dF  %.2fin", s_d.rh, s_d.dew, s_d.inHg);
  else snprintf(b, sizeof(b), "Humidity (%%)");
  fit(0, 45, b);
  graph(s_d.hum, 0, 47, 128, 17, 0, 100, false);
}

static void pageSun()
{
  char b[48];
  s_oled.setFont(u8g2_font_5x8_tr);
  int kp = slast(s_d.kp);
  std::string l = "";
  if (s_d.sfi > 0) { snprintf(b, sizeof(b), "SFI %d", s_d.sfi); l += b; }
  if (kp != -99999) { snprintf(b, sizeof(b), "  Kp %.1f", kp / 10.0); l += b; }
  if (s_d.xray[0]) { l += "  "; l += s_d.xray; }
  if (l.empty()) l = "Sun: looking it up";
  fit(0, 17, l.c_str());
  if (s_d.hfOk) {                                          // the six bands; Good ones in reverse
    static const char * BAND[6] = { "160", "80", "40", "20", "15", "10" };
    for (int i = 0; i < 6; ++i) {
      int x = (i % 3) * 43, y = 26 + (i / 3) * 8;
      snprintf(b, sizeof(b), "%s %s", BAND[i], s_d.hf[i]);
      bool good = !strcmp(s_d.hf[i], "Good");
      if (good) { s_oled.drawBox(x, y - 7, 42, 8); s_oled.setDrawColor(0); }
      fit(x + 1, y, b);
      s_oled.setDrawColor(1);
    }
  } else { s_oled.setFont(u8g2_font_6x12_tr); centre("HF: waiting for data", 36); s_oled.setFont(u8g2_font_5x8_tr); }
  // Kp over the last 4 days (one bar per 3 hours, newest on the right): "5" on the left marks
  // the dotted storm line (Kp 5); a dotted line at each midnight with the day under it, and "now" at the end
  s_oled.setFont(u8g2_font_4x6_tr);
  s_oled.drawStr(0, 41, "Kp last 4 days, dots = storm");
  const int gx = 6, gy = 43, gw = 122, gh = 15;
  graph(s_d.kp, gx, gy, gw, gh, 0, 90, true);
  auto KY = [&](int kp10) { return gy + gh - 2 - kp10 * (gh - 3) / 90; };
  s_oled.drawStr(0, KY(50) + 3, "5");                  // the storm line's level
  for (int k = gx + 1; k < gx + gw - 1; k += 3) s_oled.drawPixel(k, KY(50));
  int n = s_d.kp.n;
  if (n > 1) {
    time_t now = time(nullptr);
    auto X = [&](int i) { return gx + 1 + i * (gw - 3) / (n - 1); };
    static const char * DN[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    int prevDay = -1, lastLabelEnd = -10;
    for (int i = 0; i < n; ++i) {
      time_t t = now - (time_t)(n - 1 - i) * 3 * 3600;
      struct tm lt; localtime_r(&t, &lt);
      if (prevDay != -1 && lt.tm_mday != prevDay) {                 // midnight: a dotted line and the new day's name
        int lx = X(i) - 1;
        for (int y = gy + 1; y < gy + gh - 1; y += 2) s_oled.drawPixel(lx, y);
        const char * d = DN[lt.tm_wday % 7];
        int tw = s_oled.getStrWidth(d), tx = lx + 2;
        if (tx > lastLabelEnd + 2 && tx + tw < 128 - 14) { s_oled.drawStr(tx, 64, d); lastLabelEnd = tx + tw; }
      }
      prevDay = lt.tm_mday;
    }
    s_oled.drawStr(128 - s_oled.getStrWidth("now"), 64, "now");
  }
}

static void pageStorms()
{
  char b[64];
  s_oled.setFont(u8g2_font_5x8_tr);
  int capeMax = smax(s_d.cape, 0), thun = 0;
  for (int i = 0; i < s_d.thun.n; ++i) thun += s_d.thun.ok[i] && s_d.thun.v[i];
  if (thun) snprintf(b, sizeof(b), "Storms: %d thunder spells", thun);
  else snprintf(b, sizeof(b), capeMax >= 1000 ? "Storms: unstable air" : "Storms: none in 4 days");
  fit(0, 17, b);
  int dl = slast(s_d.duct);
  if (dl == -99999) snprintf(b, sizeof(b), "Tropo: -");
  else snprintf(b, sizeof(b), "Tropo: %s %d N/km", dl <= -157 ? "DUCTING" : dl <= -79 ? "enhanced" : dl >= 0 ? "sub-refr." : "normal", dl);
  fit(0, 26, b);
  // alerts, wrapped
  int y = 36;
  if (s_d.alertN == -1) s_oled.drawStr(0, y, "Alerts: looking them up");
  else if (s_d.alertN == -2) s_oled.drawStr(0, y, "Alerts: NWS not reachable");
  else if (s_d.alertN == 0) s_oled.drawStr(0, y, "No NWS alerts");
  else {
    for (int i = 0; i < s_d.alertN && y <= 63; ++i) {
      const char * a = s_d.alert[i];
      while (*a && y <= 63) {
        int n = strlen(a); if (n > 25) { n = 25; while (n > 10 && a[n] != ' ') --n; }
        snprintf(b, sizeof(b), "%s%.*s", a == s_d.alert[i] ? "! " : "  ", n, a);
        s_oled.drawStr(0, y, b); y += 9;
        a += n; while (*a == ' ') ++a;
      }
    }
  }
}

// headlines: one at a time, word-wrapped, a new one every 5 s (all six in the page's 30 s)
static void pageNews(uint32_t pageAt)
{
  char b[40];
  s_oled.setFont(u8g2_font_5x8_tr);
  if (s_d.newsN <= 0) {
    centre("Headlines show once", 30);
    centre("the desktop is open", 40);
    centre("in a browser", 50);
    return;
  }
  int i = (int)((millis() - pageAt) / 5000) % s_d.newsN;
  snprintf(b, sizeof(b), "%d/%d", i + 1, s_d.newsN);
  s_oled.drawStr(128 - s_oled.getStrWidth(b), 63, b);
  const char * a = s_d.news[i];
  int y = 19;
  while (*a && y <= 63) {
    int n = strlen(a), lim = y >= 55 ? 20 : 25;               // (the last line leaves room for "3/6")
    if (n > lim) { n = lim; while (n > 8 && a[n] != ' ') --n; if (a[n] != ' ') n = lim; }
    bool more = y >= 55 && (int)strlen(a) > n;
    snprintf(b, sizeof(b), "%.*s%s", n, a, more ? ".." : "");
    s_oled.drawStr(0, y, b); y += 9;
    a += n; while (*a == ' ') ++a;
  }
}

static void pageRadio()
{
  char b[48];
  s_oled.setFont(u8g2_font_5x8_tr);
  if (!rf::radioOk()) { s_oled.setFont(u8g2_font_6x12_tr); centre("No 900 MHz radio", 34); }
  else if (!s_specOk) { s_oled.setFont(u8g2_font_6x12_tr); centre("Radio idle", 30); s_oled.setFont(u8g2_font_5x8_tr); centre("open RF Sweep on the desktop", 44); }
  else {
    snprintf(b, sizeof(b), "NF %.0f dBm  busy %.1f%%%s", s_nf, s_occ * 100, s_over ? " OVL" : "");
    fit(0, 17, b);
    // one column per channel (902.2 .. 927.8 MHz), -130 .. -50 dBm
    const int gy = 19, gh = 36;
    for (int c = 0; c < rf::NCH && c < 128; ++c) {
      float v = s_spec[c]; int hgt = (int)((v + 130) * gh / 80); hgt = hgt < 0 ? 0 : hgt > gh ? gh : hgt;
      if (hgt) s_oled.drawVLine(c, gy + gh - hgt, hgt);
    }
    int nfy = gy + gh - (int)((s_nf + 130) * gh / 80);
    if (nfy > gy && nfy < gy + gh) for (int k = 0; k < 128; k += 4) s_oled.drawPixel(k, nfy);
  }
  // GPS on the bottom line
  {
    std::lock_guard<std::mutex> lk(rf::gMx);
    if (rf::gGnss.present) {
      if (rf::gGnss.fix) snprintf(b, sizeof(b), "GPS %.4f %.4f %ds", rf::gGnss.lat, rf::gGnss.lon, rf::gGnss.sats);
      else snprintf(b, sizeof(b), "GPS: no fix yet, %d sats", rf::gGnss.satsInView);
    } else snprintf(b, sizeof(b), "902        915        928");
  }
  s_oled.setFont(u8g2_font_5x8_tr);
  fit(0, 63, b);
}

bool storageFree(int64_t & freeB, bool & sd);   // core.cpp
static void pageSystem()
{
  char b[48];
  s_oled.setFont(u8g2_font_5x8_tr);
  size_t in = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  snprintf(b, sizeof(b), "RAM   %uK free", (unsigned)(in / 1024)); s_oled.drawStr(0, 18, b);
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) { snprintf(b, sizeof(b), "PSRAM %u.%uM free", (unsigned)(ps >> 20), (unsigned)((ps & 0xFFFFF) * 10 >> 20)); s_oled.drawStr(0, 27, b); }
  int64_t fb = 0; bool sd = false;
  if (storageFree(fb, sd)) {
    if (fb >= (1LL << 30)) snprintf(b, sizeof(b), "%s %u.%uG free", sd ? "SD   " : "Flash", (unsigned)(fb >> 30), (unsigned)((fb % (1LL << 30)) * 10 >> 30));
    else if (fb >= (1LL << 20)) snprintf(b, sizeof(b), "%s %u.%uM free", sd ? "SD   " : "Flash", (unsigned)(fb >> 20), (unsigned)((fb & 0xFFFFF) * 10 >> 20));
    else snprintf(b, sizeof(b), "%s %uK free", sd ? "SD   " : "Flash", (unsigned)(fb >> 10));
    s_oled.drawStr(0, 36, b);
  }
  if (netConnected()) snprintf(b, sizeof(b), "Wi-Fi %d dBm  %.12s", (int)WiFi.RSSI(), WiFi.SSID().c_str());
  else snprintf(b, sizeof(b), "Wi-Fi: hotspot only");
  fit(0, 45, b);
  uint32_t up = millis() / 1000;
  snprintf(b, sizeof(b), "Up %ud %uh %02um   %.0fC", (unsigned)(up / 86400), (unsigned)(up / 3600 % 24), (unsigned)(up / 60 % 60), temperatureRead());
  s_oled.drawStr(0, 54, b);
  if (s_d.unread) { snprintf(b, sizeof(b), "%d new message%s", s_d.unread, s_d.unread == 1 ? "" : "s"); s_oled.drawStr(0, 63, b); }
  else { snprintf(b, sizeof(b), "TidyOS Pro %s", TIDYOS_VERSION); s_oled.drawStr(0, 63, b); }
}

static void pageStopwatch()
{
  char b[24];
  swFmt(b, sizeof(b), swMs(), true);
  s_oled.setFont(u8g2_font_logisoso22_tn);
  centre(b, 40);
  s_oled.setFont(u8g2_font_5x8_tr);
  centre(s_swRun ? "running" : swMs() ? "stopped" : "ready", 50);
  centre("hold: 1s go/stop  3s reset", 62);
}

static void draw(int page, bool held, uint32_t pageAt, bool resetHint)
{
  // the data the pages show
  deskSnapshot(s_d);
  {
    std::lock_guard<std::mutex> lk(rf::gMx);
    s_specOk = rf::gSpec && rf::mode() == rf::Mode::Sweep;
    if (s_specOk) { memcpy(s_spec, rf::gSpec->avg, sizeof(s_spec)); s_nf = rf::gSpec->noiseFloor; s_occ = rf::gSpec->occupancy; s_over = rf::gSpec->overload; }
  }
  s_oled.clearBuffer();
  // header: page name, then the stopwatch (if running), HOLD and the page number
  s_oled.setFont(u8g2_font_5x8_tr);
  s_oled.drawStr(0, 7, TITLE[page]);
  char r[24]; r[0] = 0;
  if (s_swRun && page != P_STOPWATCH) { swFmt(r, sizeof(r), swMs(), false); strcat(r, " "); }
  char pg[12]; snprintf(pg, sizeof(pg), "%s%d/%d", held ? "HOLD " : "", page + 1, P_COUNT);
  strlcat(r, pg, sizeof(r));
  int rx = 128 - s_oled.getStrWidth(r);
  s_oled.drawStr(rx, 7, r);
  {                                                            // battery: a little gauge, its % beside it
    int pct = batteryPct();
    int left = s_oled.getStrWidth(TITLE[page]) + 3;
    s_oled.setFont(u8g2_font_4x6_tr);
    char t[8];
    if (pct < 0) strlcpy(t, "USB", sizeof(t)); else snprintf(t, sizeof(t), "%d%%", pct);
    int tw = s_oled.getStrWidth(t), w = pct < 0 ? tw : 14 + 1 + tw;
    int bx = rx - 4 - w;
    if (bx >= left) {
      if (pct >= 0) {
        s_oled.drawFrame(bx, 1, 12, 7);
        s_oled.drawBox(bx + 12, 3, 2, 3);
        s_oled.drawBox(bx + 1, 2, (pct * 10 + 50) / 100, 5);
        s_oled.drawStr(bx + 15, 7, t);
      } else s_oled.drawStr(bx, 7, t);
    }
    s_oled.setFont(u8g2_font_5x8_tr);
  }
  s_oled.drawHLine(0, 9, 128);
  switch (page) {
    case P_ADDRESS: pageAddress(); break;
    case P_CLOCK: pageClock(); break;
    case P_WEATHER: pageWeather(); break;
    case P_WIND: pageWind(); break;
    case P_SUN: pageSun(); break;
    case P_STORMS: pageStorms(); break;
    case P_NEWS: pageNews(pageAt); break;
    case P_RADIO: pageRadio(); break;
    case P_SYSTEM: pageSystem(); break;
    case P_STOPWATCH: pageStopwatch(); break;
  }
  if (resetHint) { s_oled.setDrawColor(0); s_oled.drawBox(14, 22, 100, 20); s_oled.setDrawColor(1); s_oled.drawFrame(14, 22, 100, 20); s_oled.setFont(u8g2_font_6x12_tr); centre("Reset", 36); }
  // time to the next page: a short bar under the header
  if (!held) { int w = (int)((millis() - pageAt) * 128 / PAGE_MS); s_oled.setDrawColor(0); s_oled.drawHLine(w, 9, 128 - w); s_oled.setDrawColor(1); }
  s_oled.sendBuffer();
}

static void oledTask(void *)
{
  int page = P_ADDRESS;                                    // start-up: show where to find the desktop first
  bool held = false, dimmed = false;
  uint32_t pageAt = millis(), lastPress = millis(), lastDraw = 0;
  bool prgWas = false, swallow = false, longDone = false, resetDone = false;
  uint32_t downAt = 0, tapAt = 0; int taps = 0;
  uint32_t resetShowUntil = 0;
  uint32_t battAt = 0;
  for (;;) {
    uint32_t now = millis();
    if (!battAt || now - battAt > 10000) { battAt = now; batteryRead(); }   // the battery, every 10 s
#ifdef PIN_PRG2
    bool prg = digitalRead(PIN_PRG) == LOW || digitalRead(PIN_PRG2) == LOW;
#else
    bool prg = digitalRead(PIN_PRG) == LOW;
#endif
    bool redraw = false;
    if (prg && !prgWas) {                                    // pressed
      downAt = now; longDone = resetDone = false;
      lastPress = now;
      if (dimmed) { s_oled.setPowerSave(0); s_oled.setContrast(BRIGHT); dimmed = false; swallow = true; }   // (a press only wakes it)
    }
    if (prg && !swallow && page == P_STOPWATCH) {           // held on the stopwatch page
      if (!longDone && now - downAt >= LONG_MS) {
        longDone = true;
        if (s_swRun) { s_swAcc += now - s_swStart; s_swRun = false; } else { s_swStart = now; s_swRun = true; }
        redraw = true;
      }
      if (!resetDone && now - downAt >= RESET_MS) {
        resetDone = true; s_swRun = false; s_swAcc = 0; resetShowUntil = now + 800; redraw = true;
      }
    }
    if (!prg && prgWas) {                                    // let go
      if (swallow) swallow = false;
      else if (now - downAt < LONG_MS && !longDone) { taps++; tapAt = now; }
    }
    // hold PRG (on any page but the stopwatch): 6 s then let go = forget Wi-Fi,
    // keep holding to 12 s then let go = every TidyOS setting back to how it came (like The Shelf)
    bool holding = prg && !swallow && page != P_STOPWATCH && now - downAt >= 2000;
    if (holding) {
      uint32_t h = now - downAt;
      longDone = true;                                       // (no tap when it's let go)
      if (now - lastDraw >= 200) {
        char b[32];
        s_oled.clearBuffer();
        s_oled.setFont(u8g2_font_6x10_tr);
        if (h < 6000) {
          s_oled.drawStr(0, 12, "Forget Wi-Fi?");
          snprintf(b, sizeof(b), "Keep holding %d s", (int)((6999 - h) / 1000)); s_oled.drawStr(0, 30, b);
          s_oled.drawStr(0, 46, "Let go to cancel");
        } else if (h < 12000) {
          s_oled.drawStr(0, 12, "Let go: forget Wi-Fi");
          snprintf(b, sizeof(b), "Hold %d s more:", (int)((12999 - h) / 1000)); s_oled.drawStr(0, 30, b);
          s_oled.drawStr(0, 46, "all settings reset");
        } else {
          s_oled.drawStr(0, 12, "Let go: reset ALL");
          s_oled.drawStr(0, 30, "settings to default");
          s_oled.drawStr(0, 46, "(files are kept)");
        }
        s_oled.sendBuffer();
        lastDraw = now;
      }
    }
    if (!prg && prgWas && !swallow && page != P_STOPWATCH && now - downAt >= 6000) {
      bool all = now - downAt >= 12000;
      s_oled.clearBuffer(); s_oled.setFont(u8g2_font_6x10_tr);
      s_oled.drawStr(0, 12, all ? "Settings reset" : "Wi-Fi forgotten");
      s_oled.drawStr(0, 30, "Restarting in");
      s_oled.drawStr(0, 46, "setup mode...");
      s_oled.sendBuffer();
      Serial.printf("[TidyOS] PRG held: %s\n", all ? "every setting back to default" : "forgetting Wi-Fi");
      if (all) settingsReset();
      else { gSet.ssid = ""; gSet.pass = ""; settingsSave(); }
      WiFi.disconnect(true, true);                           // (the Wi-Fi driver's own copy too)
      delay(1200);
      ESP.restart();
    }
    prgWas = prg;
    if (holding) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    if (taps >= 2) {                                         // double tap: next page
      taps = 0; page = (page + 1) % P_COUNT; pageAt = now; redraw = true;
    } else if (taps == 1 && !prg && now - tapAt > TAP_GAP_MS) {   // single tap: hold / roll
      taps = 0; held = !held; pageAt = now; redraw = true;
    }
    if (!held && now - pageAt >= PAGE_MS) { page = (page + 1) % P_COUNT; pageAt = now; redraw = true; }
    if (musicQuickPlaying()) lastPress = now;                // music playing: stay awake (as The Shelf does)
    // 5 minutes without a press: off (Switches > OLED sleep), or with that off, dimmed after 30 minutes
    if (!dimmed && now - lastPress > (gSet.oledSleep ? SLEEP_MS : DIM_MS)) {
      if (gSet.oledSleep) s_oled.setPowerSave(1); else s_oled.setContrast(DIM);
      dimmed = true;
    }
    if (dimmed) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }   // (nothing to draw while it's off)
    uint32_t every = page == P_STOPWATCH && s_swRun ? 100 : page == P_RADIO ? 300 : 250;
    if (redraw || now - lastDraw >= every) { draw(page, held, pageAt, now < resetShowUntil); lastDraw = now; }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// update mode: no pages, just what the updater is doing (so it can be seen without a browser)
static bool s_statusInit = false;
static volatile bool s_pagesOn = false;     // the page task has the screen
static void oledProgress(const char * title, const char * msg, int pct);
static bool s_updating = false;
void oledStatus(const char * msg, int pct) { s_updating = true; oledProgress("UPDATE  TidyOS Pro " TIDYOS_VERSION, msg, pct); }
// start-up: what TidyOS Pro is doing, with a progress bar, until the OLED's own pages take over
void oledBoot(const char * msg, int pct) { if (!s_pagesOn && !s_updating) oledProgress("Starting TidyOS Pro " TIDYOS_VERSION, msg, pct); }
static void oledProgress(const char * title, const char * msg, int pct)
{
  if (!s_statusInit) {
#if defined(TIDYOS_XIAO)
    // (no Vext switch: a plug-in OLED is powered from 3V3)
#elif CONFIG_SPIRAM_MODE_OCT
    pinMode(PIN_VEXT_R8, OUTPUT); digitalWrite(PIN_VEXT_R8, LOW);
#else
    pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW);
#endif
    delay(50);
    s_oled.begin(); s_oled.setContrast(BRIGHT);
    s_statusInit = true;
  }
  s_oled.clearBuffer();
  s_oled.setFont(u8g2_font_5x8_tr);
  s_oled.drawStr(0, 7, title);
  s_oled.drawHLine(0, 9, 128);
  // the message, wrapped over up to 4 lines
  const char * m = msg; int y = 20;
  while (*m && y <= 50) {
    int n = strlen(m); if (n > 25) { n = 25; while (n > 8 && m[n] != ' ') --n; }
    char line[32]; snprintf(line, sizeof(line), "%.*s", n, m);
    s_oled.drawStr(0, y, line); y += 10;
    m += n; while (*m == ' ') ++m;
  }
  if (pct >= 0) { s_oled.drawFrame(0, 56, 128, 8); if (pct > 0) s_oled.drawBox(2, 58, (pct > 100 ? 100 : pct) * 124 / 100, 4); }
  s_oled.sendBuffer();
}

void oledOff()            // switched off: the panel dark (it may still show the start-up progress)
{
  if (s_statusInit) { s_oled.setPowerSave(1); s_pagesOn = true; }   // (and no more start-up progress on it)
}

void oledBegin()
{
#if defined(TIDYOS_XIAO)
  pinMode(PIN_PRG2, INPUT_PULLUP);
#elif CONFIG_SPIRAM_MODE_OCT
  pinMode(PIN_VEXT_R8, OUTPUT); digitalWrite(PIN_VEXT_R8, LOW);
#else
  pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW);       // screen power on
#endif
  delay(50);
  pinMode(PIN_PRG, INPUT_PULLUP);
  if (!s_statusInit) s_oled.begin();
  s_statusInit = true;
  s_oled.setContrast(BRIGHT);
  s_pagesOn = true;
  xTaskCreatePinnedToCore(oledTask, "oled", 8192, nullptr, 1, nullptr, 0);
}
#else
void oledBegin() {}
void oledStatus(const char *, int) {}
void oledBoot(const char *, int) {}
void oledOff() {}
#endif
