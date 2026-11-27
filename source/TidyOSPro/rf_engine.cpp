#include "sdkconfig.h"
// TidyOS Pro - 900 MHz engine (see rf_engine.h)
#include "config.h"
#include "rf_engine.h"
#include <esp_timer.h>
#ifdef TIDYOS_V4
#include <SPI.h>
#include <RadioLib.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <sys/time.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

extern const char * gSDWiring;   // core.cpp: which wiring the SD card was found on
#ifdef TIDYOS_XIAO
#include <SPI.h>
extern SPIClass gSharedSpi;      // core.cpp: the XIAO's radio and SD card share one SPI bus
#endif
bool gpsSwitchOn();              // core.cpp: the GPS start-up switch

namespace rf {

Settings   gSettings;
Spectrum * gSpec = nullptr;
Listen   * gListen = nullptr;
Gnss       gGnss;
std::mutex gMx;

#ifdef TIDYOS_XIAO
#define s_spi ::gSharedSpi            // core.cpp: the radio and the SD card share one bus (D8 / D9 / D10)
#else
static SPIClass s_spi(HSPI);          // the SD card has FSPI
#endif
static SX1262 * s_radio = nullptr;
static volatile Mode s_mode = Mode::Off, s_want = Mode::Off;
static volatile int  s_wantCh = 64;
static volatile bool s_reset = false;
static char s_err[64] = "";
static bool s_ok = false;
static TaskHandle_t s_task = nullptr;

const char * lastError() { return s_err; }
bool radioOk() { return s_ok; }
Mode mode() { return s_mode == Mode::Mesh ? Mode::Off : s_mode; }   // (the mesh is background: to the RF apps the radio is free)

float powerDensity(float dbm, float gDbi, float fMHz)
{
  // S = Pr / Ae, with the antenna's effective aperture Ae = G * lambda^2 / (4 pi)
  double lambdaCm = 29979.2458 / fMHz;                       // c / f, in cm
  double ae = pow(10.0, gDbi / 10.0) * lambdaCm * lambdaCm / (4.0 * M_PI);   // cm^2
  double mw = pow(10.0, dbm / 10.0);
  return (float)(mw / ae);
}

void loadSettings()
{
  Preferences p;
  if (!p.begin("rfpro", true)) return;
  gSettings.antGainDbi = p.getFloat("ant", gSettings.antGainDbi);
  gSettings.frontEndDb = p.getFloat("fe", gSettings.frontEndDb);
  gSettings.attenDb    = p.getFloat("att", gSettings.attenDb);
  gSettings.threshDb   = p.getFloat("thr", gSettings.threshDb);
  gSettings.dwellUs    = p.getInt("dwell", gSettings.dwellUs);
  p.end();
}

void saveSettings()
{
  Preferences p;
  if (!p.begin("rfpro", false)) return;
  p.putFloat("ant", gSettings.antGainDbi);
  p.putFloat("fe", gSettings.frontEndDb);
  p.putFloat("att", gSettings.attenDb);
  p.putFloat("thr", gSettings.threshDb);
  p.putInt("dwell", gSettings.dwellUs);
  p.end();
}

// ---- front-end module (GC1109 on V4.2, KCT8103L on V4.3): powered, enabled, in receive
static void femReceive(bool on)
{
#ifndef HAS_FEM
  (void)on; return;                    // V3 / XIAO: no front-end module
#else
  pinMode(FEM_POWER, OUTPUT); pinMode(FEM_EN, OUTPUT);
  pinMode(FEM_CPS, OUTPUT);   pinMode(FEM_CTX, OUTPUT);
  digitalWrite(FEM_POWER, on ? HIGH : LOW);
  digitalWrite(FEM_EN, on ? HIGH : LOW);
  digitalWrite(FEM_CPS, LOW);          // never transmitting
  digitalWrite(FEM_CTX, LOW);          // receive through the LNA
#endif
}

static void clearStats()
{
  std::lock_guard<std::mutex> lk(gMx);
  for (int c = 0; c < NCH; ++c) {
    gSpec->avg[c] = gSpec->peak[c] = gSpec->hold[c] = -130;
    gSpec->busy[c] = gSpec->samples[c] = 0;
    gSpec->linSum[c] = 0;
  }
  memset(gSpec->wf, -128, sizeof(gSpec->wf));
  gSpec->wfHead = 0; gSpec->sweeps = 0; gSpec->resetAt = millis();
  gListen->bursts = gListen->busyMs = 0; gListen->startedAt = millis();
  memset(gListen->hist, 0, sizeof(gListen->hist));
  gListen->recentN = gListen->recentHead = 0;
  for (auto & t : gListen->trace) t = -130;
  gListen->traceHead = 0;
}

void resetStats() { s_reset = true; if (s_mode == Mode::Off && gSpec) clearStats(); }

static bool tune(float mhz)
{
  s_radio->standby();
  if (s_radio->setFrequency(mhz, true) != RADIOLIB_ERR_NONE) return false;   // image calibration done once in begin()
  return s_radio->startReceive() == RADIOLIB_ERR_NONE;
}

static inline float rssiNow() { return s_radio->getRSSI(false); }   // instantaneous RSSI, dBm

static void doSweep()
{
  float avg[NCH], pk[NCH];
  uint16_t busyN[NCH], nS[NCH];
  double lin[NCH];
  uint32_t t0 = millis();
  float floorPrev = gSpec->noiseFloor;
  float thr = floorPrev + gSettings.threshDb;
  bool over = false;
  int each = imax(2, gSettings.dwellUs / 250);
  int64_t work = 0;
  for (int c = 0; c < NCH && s_want == Mode::Sweep; ++c) {
    int64_t w0 = esp_timer_get_time();
    tune(chFreq(c));
    delayMicroseconds(250);                       // RSSI settles
    double sum = 0, l = 0; float m = -200; int b = 0;
    for (int k = 0; k < each; ++k) {
      float raw = rssiNow();
      if (raw > -30) over = true;                 // near the top of what the SX1262 + LNA can read
      float v = corrected(raw);
      sum += v; l += pow(10.0, v / 10.0);
      if (v > m) m = v;
      if (v > thr) ++b;
      delayMicroseconds(200);
    }
    avg[c] = (float)(sum / each); pk[c] = m; busyN[c] = b; nS[c] = each; lin[c] = l;
    work += esp_timer_get_time() - w0;           // (includes time taken by other tasks mid-channel; counted below)
    if ((c & 15) == 15) vTaskDelay(1);           // let Wi-Fi and the desktop in
  }
  if (s_want != Mode::Sweep) return;
  // noise floor: mean of the quietest quarter of channels
  float srt[NCH]; memcpy(srt, avg, sizeof(srt));
  std::sort(srt, srt + NCH);
  double nf = 0; for (int i = 0; i < NCH / 4; ++i) nf += srt[i];
  nf /= NCH / 4;
  std::lock_guard<std::mutex> lk(gMx);
  uint32_t busyAll = 0, allN = 0;
  float bp = -200; int bpc = 0;
  for (int c = 0; c < NCH; ++c) {
    gSpec->avg[c] = avg[c]; gSpec->peak[c] = pk[c];
    if (pk[c] > gSpec->hold[c]) gSpec->hold[c] = pk[c];
    gSpec->busy[c] += busyN[c]; gSpec->samples[c] += nS[c]; gSpec->linSum[c] += lin[c];
    busyAll += busyN[c]; allN += nS[c];
    if (pk[c] > bp) { bp = pk[c]; bpc = c; }
    gSpec->wf[gSpec->wfHead][c] = (int8_t)fmaxf(-127, fminf(0, avg[c]));
  }
  gSpec->wfHead = (gSpec->wfHead + 1) % WF_ROWS;
  gSpec->noiseFloor = (float)nf;
  gSpec->bandPeak = bp; gSpec->bandPeakCh = bpc;
  gSpec->occupancy = allN ? (float)busyAll / allN : 0;
  gSpec->overload = over;
  gSpec->sweeps++;
  gSpec->sweepMs = millis() - t0;
  gSpec->workMs = (uint32_t)(work / 1000);
}

static void doListen()
{
  int ch = s_wantCh;
  {
    std::lock_guard<std::mutex> lk(gMx);
    if (gListen->ch != ch) {
      gListen->ch = ch; gListen->bursts = gListen->busyMs = 0; gListen->startedAt = millis();
      memset(gListen->hist, 0, sizeof(gListen->hist)); gListen->recentN = gListen->recentHead = 0;
    }
  }
  tune(chFreq(ch));
  delayMicroseconds(300);
  float noise = gListen->noise > -125 ? gListen->noise : -115;
  bool in = false; uint32_t bStart = 0, lastHigh = 0; float bPeak = -200;
  uint32_t winStart = millis(); float winMax = -200;
  uint32_t yieldAt = millis();
  while (s_want == Mode::Listen && s_wantCh == ch && !s_reset) {
    float raw = rssiNow();
    float v = corrected(raw);
    uint32_t now = millis();
    float thr = noise + gSettings.threshDb;
    if (v > thr) {
      if (!in) { in = true; bStart = now; bPeak = v; }
      lastHigh = now; if (v > bPeak) bPeak = v;
    } else {
      noise = noise * 0.995f + v * 0.005f;      // only quiet samples move the noise estimate
      if (in && now - lastHigh > 3) {            // 3 ms quiet ends a burst
        in = false;
        uint32_t len = lastHigh - bStart + 1;
        std::lock_guard<std::mutex> lk(gMx);
        gListen->bursts++; gListen->busyMs += len;
        static const uint16_t edges[7] = { 2, 5, 10, 20, 50, 100, 250 };
        int h = 7; for (int i = 0; i < 7; ++i) if (len < edges[i]) { h = i; break; }
        gListen->hist[h]++;
        Burst & b = gListen->recent[gListen->recentHead];
        b.atMs = bStart; b.lenMs = (uint16_t)imin(65535, (int)len); b.peakDbm = (int8_t)fmaxf(-127, fminf(127, bPeak));
        gListen->recentHead = (gListen->recentHead + 1) % 32;
        if (gListen->recentN < 32) gListen->recentN++;
      }
    }
    if (v > winMax) winMax = v;
    if (now - winStart >= 5) {
      std::lock_guard<std::mutex> lk(gMx);
      gListen->trace[gListen->traceHead] = winMax;
      gListen->traceHead = (gListen->traceHead + 1) % 200;
      gListen->noise = noise; gListen->now = v;
      winStart = now; winMax = -200;
    }
    if (now - yieldAt >= 8) { vTaskDelay(1); yieldAt = millis(); }
    else delayMicroseconds(150);
  }
}

// ---- 900 MHz Coms mesh (LoRa). The radio is an FSK receiver for the RF apps; for the mesh it's set up for
// LoRa instead, and set back to FSK when an RF app wants it again.
static volatile bool s_meshWanted = false;
static bool s_loRa = false;                                   // the radio is set up for LoRa now
static void (*s_meshRx)(const uint8_t *, size_t) = nullptr;
static std::mutex s_meshMx;
static std::vector<std::vector<uint8_t>> s_meshTx;
static volatile int s_meshSent = 0, s_meshHeard = 0;
#ifdef TIDYOS_XIAO
static const int8_t MESH_CHIP_DBM = 20;                       // Wio-SX1262: no front end, so the chip itself gives ~20 dBm
#else
static const int8_t MESH_CHIP_DBM = 9;                        // + ~11 dB from the V4's front end: about 20 dBm out
#endif

void meshEnable(bool on) {
  s_meshWanted = on;
#if defined(TIDYOS_V3) || defined(TIDYOS_XIAO)
  if (on && !s_radio) begin();                    // (the V3 starts its radio only when it's wanted)
#endif
}
bool wake() { if (!s_radio) begin(); return s_ok; }
void meshOnReceive(void (*fn)(const uint8_t *, size_t)) { s_meshRx = fn; }
bool meshListening() { return s_mode == Mode::Mesh; }
void meshCounts(int & sent, int & heard) { sent = s_meshSent; heard = s_meshHeard; }
void meshSend(const uint8_t * b, size_t n)
{
  if (!s_ok || !s_meshWanted || n == 0 || n > 255) return;
  std::lock_guard<std::mutex> lk(s_meshMx);
  if (s_meshTx.size() < 16) s_meshTx.emplace_back(b, b + n);
}

static bool toLoRa()
{
  s_radio->standby();
  int st = s_radio->begin(915.0, 125.0, 9, 5, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, MESH_CHIP_DBM, 16, 1.8);
  if (st != RADIOLIB_ERR_NONE) { snprintf(s_err, sizeof(s_err), "Mesh: LoRa didn't start (RadioLib error %d)", st); return false; }
  s_radio->setDio2AsRfSwitch(true);
  s_radio->setCRC(2);
  s_radio->setCurrentLimit(140);
  s_radio->setRxBoostedGainMode(true);
  s_loRa = true;
  return s_radio->startReceive() == RADIOLIB_ERR_NONE;
}
static void toFSK()
{
  s_radio->standby();
  s_radio->beginFSK(915.0, 50.0, 25.0, 234.3, 0, 16, 1.8);
  s_radio->setDio2AsRfSwitch(true);
  s_radio->setRxBoostedGainMode(true);
  s_loRa = false;
}
static void doMesh()
{
  for (int i = 0; i < 10 && s_want == Mode::Off && s_meshWanted; ++i) {
    if (digitalRead(LORA_DIO1) == HIGH) {                     // a packet (or a bad one) arrived
      uint8_t b[256];
      size_t n = s_radio->getPacketLength();
      if (n > 0 && n <= 255 && s_radio->readData(b, n) == RADIOLIB_ERR_NONE && n >= 3 && b[0] == 'S' && b[1] == '9') {
        s_meshHeard++;
        if (s_meshRx) s_meshRx(b, n);
      }
      s_radio->startReceive();
    }
    std::vector<uint8_t> t;
    { std::lock_guard<std::mutex> lk(s_meshMx); if (!s_meshTx.empty()) { t.swap(s_meshTx.front()); s_meshTx.erase(s_meshTx.begin()); } }
    if (!t.empty()) {
      if (s_radio->scanChannel() == RADIOLIB_LORA_DETECTED) {  // someone's talking: try again shortly
        std::lock_guard<std::mutex> lk(s_meshMx);
        s_meshTx.push_back(std::move(t));
        vTaskDelay(pdMS_TO_TICKS(300 + esp_random() % 600));
      } else {
#ifdef HAS_FEM
        digitalWrite(FEM_CPS, HIGH); digitalWrite(FEM_CTX, HIGH);   // front end to transmit (V4.2 / V4.3)
#endif
        if (s_radio->transmit(t.data(), t.size()) == RADIOLIB_ERR_NONE) s_meshSent++;
#ifdef HAS_FEM
        digitalWrite(FEM_CPS, LOW); digitalWrite(FEM_CTX, LOW);
#endif
      }
      s_radio->startReceive();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void radioTask(void *)
{
  for (;;) {
    if (s_reset) { clearStats(); s_reset = false; }
    Mode w = s_want;
    if (w == Mode::Off && s_meshWanted) w = Mode::Mesh;          // no RF app on the radio: the Coms mesh gets it
    if (w != s_mode) {
      if (w == Mode::Off) { s_radio->standby(); s_radio->sleep(); femReceive(false); }
      else {
        if (s_mode == Mode::Off) { femReceive(true); s_radio->standby(); }
        if (w == Mode::Mesh) { if (!toLoRa()) { s_meshWanted = false; w = Mode::Off; s_radio->standby(); s_radio->sleep(); femReceive(false); } }
        else if (s_loRa) toFSK();                                  // back to the RF apps' receiver
      }
      s_mode = w;
    }
    if (s_mode == Mode::Sweep) doSweep();
    else if (s_mode == Mode::Listen) doListen();
    else if (s_mode == Mode::Mesh) doMesh();
    else vTaskDelay(pdMS_TO_TICKS(100));
  }
}

bool begin()
{
  if (s_radio) return s_ok;
  loadSettings();
  gSpec = (Spectrum *)heap_caps_calloc(1, sizeof(Spectrum), MALLOC_CAP_SPIRAM);
  if (!gSpec) gSpec = (Spectrum *)calloc(1, sizeof(Spectrum));
  gListen = new Listen();
  femReceive(true);
#ifdef TIDYOS_XIAO
  if (!strcmp(::gSDWiring, "none")) { pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH); }   // no card mounted: keep a fitted one quiet
#endif
  s_spi.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  s_radio = new SX1262(new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, s_spi));
#ifdef LORA_RXEN
  s_radio->setRfSwitchPins(LORA_RXEN, RADIOLIB_NC);     // Wio-SX1262: RXEN high to receive, DIO2 for transmit
#endif
  // FSK receiver, 234 kHz wide so each 200 kHz channel is covered; TCXO at 1.8 V on the V4
  int st = s_radio->beginFSK(915.0, 50.0, 25.0, 234.3, 0, 16, 1.8);
  if (st != RADIOLIB_ERR_NONE) {
    snprintf(s_err, sizeof(s_err), "SX1262 didn't start (RadioLib error %d)", st);
    femReceive(false);
    s_ok = false;
  } else {
    s_radio->setDio2AsRfSwitch(true);
    s_radio->setRxBoostedGainMode(true);
    s_radio->calibrateImage(915.0);          // covers 902-928 MHz
    s_radio->standby(); s_radio->sleep();
    femReceive(false);
    s_ok = true;
  }
  clearStats();
  gSpec->noiseFloor = -110;
  // priority 2, the same as the desktop streaming to the browser ("screen"): the two share core 0 a tick at a
  // time. At 1 the sweep only ran when the screen task was idle, and a sweep took ~1.4 s instead of ~0.4 s.
  if (s_ok) xTaskCreatePinnedToCore(radioTask, "rf", 10240, nullptr, 2, &s_task, 0);
  return s_ok;
}

void powerDown()
{
  s_meshWanted = false;
  s_want = Mode::Off;
  for (int i = 0; i < 40 && s_mode != Mode::Off; ++i) delay(10);
  if (s_radio && s_ok) { s_radio->standby(); s_radio->sleep(); }
  femReceive(false);
#ifdef HAS_FEM
  digitalWrite(FEM_POWER, LOW);
#endif
#if defined(TIDYOS_XIAO)
  // (no Vext: GPIO 40 is the radio's BUSY line on the XIAO)
#elif CONFIG_SPIRAM_MODE_OCT
  pinMode(PIN_VEXT_R8, OUTPUT); digitalWrite(PIN_VEXT_R8, HIGH);  // Vext (OLED) off
#else
#ifdef HAS_GNSS
  pinMode(GNSS_EN, OUTPUT); digitalWrite(GNSS_EN, HIGH);          // GNSS header power off
#endif
  pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, HIGH);        // Vext (OLED) off
#endif
}

void setMode(Mode m, int ch)
{
  if (!s_ok) return;
  if (ch >= 0 && ch < NCH) s_wantCh = ch;
  s_want = m;
}

// ====================================================================== GNSS
// L76K-type module on the V4's GNSS header: NMEA at 9600 (some modules: 115200).
static HardwareSerial & s_gps = Serial1;
static int s_gnssState = 0;           // 0 not looked, 1 found, -1 not there
static TaskHandle_t s_gTask = nullptr;

static bool nmeaOk(const char * s)
{
  if (s[0] != '$') return false;
  const char * star = strchr(s, '*');
  if (!star || !star[1] || !star[2]) return false;
  uint8_t c = 0;
  for (const char * p = s + 1; p < star; ++p) c ^= (uint8_t)*p;
  return c == (uint8_t)strtol(star + 1, nullptr, 16);
}

static int fields(char * s, char ** f, int max)
{
  int n = 0; f[n++] = s;
  for (char * p = s; *p && n < max; ++p) {
    if (*p == ',' || *p == '*') { *p = 0; f[n++] = p + 1; }
  }
  return n;
}

static double nmeaDeg(const char * v, const char * hemi)
{
  if (!*v) return 0;
  double x = atof(v);
  int d = (int)(x / 100);
  double deg = d + (x - d * 100) / 60.0;
  if (*hemi == 'S' || *hemi == 'W') deg = -deg;
  return deg;
}

static void parseLine(char * line)
{
  if (!nmeaOk(line)) return;
  char * f[24];
  int n = fields(line, f, 24);
  const char * t = f[0] + 3;            // skip $GP / $GN / $GL / $BD
  std::lock_guard<std::mutex> lk(gMx);
  gGnss.sentences++; gGnss.lastSentenceMs = millis();
  if (!strncmp(t, "GGA", 3) && n > 9) {
    int q = atoi(f[6]);
    gGnss.fix = q > 0;
    if (gGnss.fix) {
      gGnss.lat = nmeaDeg(f[2], f[3]); gGnss.lon = nmeaDeg(f[4], f[5]);
      gGnss.altM = atof(f[9]); gGnss.fixAtMs = millis();
    }
    gGnss.sats = atoi(f[7]); gGnss.hdop = *f[8] ? atof(f[8]) : 99;
  } else if (!strncmp(t, "RMC", 3) && n > 9) {
    if (*f[2] == 'A') {
      gGnss.lat = nmeaDeg(f[3], f[4]); gGnss.lon = nmeaDeg(f[5], f[6]);
      gGnss.speedKmh = atof(f[7]) * 1.852f; gGnss.courseDeg = atof(f[8]);
    }
    if (strlen(f[1]) >= 6 && strlen(f[9]) == 6) {
      gGnss.hour = (f[1][0] - '0') * 10 + f[1][1] - '0';
      gGnss.minute = (f[1][2] - '0') * 10 + f[1][3] - '0';
      gGnss.second = (f[1][4] - '0') * 10 + f[1][5] - '0';
      gGnss.day = (f[9][0] - '0') * 10 + f[9][1] - '0';
      gGnss.month = (f[9][2] - '0') * 10 + f[9][3] - '0';
      gGnss.year = 2000 + (f[9][4] - '0') * 10 + f[9][5] - '0';
      // no internet clock yet? set it from the satellites
      if (*f[2] == 'A' && time(nullptr) < 1700000000) {
        struct tm tmv = {}; tmv.tm_year = gGnss.year - 1900; tmv.tm_mon = gGnss.month - 1; tmv.tm_mday = gGnss.day;
        tmv.tm_hour = gGnss.hour; tmv.tm_min = gGnss.minute; tmv.tm_sec = gGnss.second;
        char * tz = getenv("TZ"); std::string keep = tz ? tz : "";
        setenv("TZ", "UTC0", 1); tzset();
        time_t tt = mktime(&tmv);
        if (keep.size()) setenv("TZ", keep.c_str(), 1); else unsetenv("TZ");
        tzset();
        struct timeval tv = { tt, 0 }; settimeofday(&tv, nullptr);
      }
    }
  } else if (!strncmp(t, "GSV", 3) && n > 3) {
    if (!strncmp(f[0] + 1, "GP", 2) || !strncmp(f[0] + 1, "GN", 2)) gGnss.satsInView = atoi(f[3]);
  }
}

static void gnssTask(void *)
{
  char line[100]; int n = 0;
  for (;;) {
    while (s_gps.available()) {
      char c = s_gps.read();
      if (c == '\n' || c == '\r') { if (n) { line[n] = 0; parseLine(line); n = 0; } }
      else if (n < (int)sizeof(line) - 1) line[n++] = c;
      else n = 0;
    }
    {
      std::lock_guard<std::mutex> lk(gMx);
      if (gGnss.fix && millis() - gGnss.fixAtMs > 5000) gGnss.fix = false;   // stale
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// a GNSS module only counts once it sends two whole NMEA sentences with good checksums: a few stray
// characters (noise, or an in-line SD card's MISO line on GPIO 39) can look like "$GP" by chance
static bool listenFor(int baud, int rx, int tx, uint32_t ms)
{
  s_gps.end();
  s_gps.begin(baud, SERIAL_8N1, rx, tx);
  uint32_t t0 = millis();
  char buf[100]; int n = -1; int good = 0;
  while (millis() - t0 < ms) {
    while (s_gps.available()) {
      char c = s_gps.read();
      if (c == '$') { n = 0; continue; }
      if (n < 0) continue;
      if (c == '\r' || c == '\n') {
        buf[n] = 0;
        char * star = strchr(buf, '*');
        if (star && star - buf >= 5 && buf[0] == 'G' && hexv(star[1]) >= 0 && hexv(star[2]) >= 0) {
          uint8_t x = 0; for (char * q = buf; q < star; q++) x ^= (uint8_t)*q;
          if (x == (uint8_t)(hexv(star[1]) * 16 + hexv(star[2])) && ++good >= 2) return true;
        }
        n = -1;
      } else if (n < (int)sizeof(buf) - 1) buf[n++] = c;
      else n = -1;
    }
    delay(10);
  }
  return false;
}

bool gnssRunning() { return s_gnssState > 0; }

bool gnssStart()
{
  if (s_gnssState) return s_gnssState > 0;
#ifndef HAS_GNSS
  s_gnssState = -1;
  Serial.println("[TidyOS Pro] " TIDYOS_BOARD ": no GNSS header, not looking for a GPS");
  return false;
#else
  if (!gpsSwitchOn()) {
    s_gnssState = -1;
    Serial.println("[TidyOS Pro] GPS switched off (Switches): not looking for one");
    return false;
  }
  if (strstr(::gSDWiring, "in-line")) {     // the SD card is on 39-42: those are the GNSS pins, so no GPS (and hands off them)
    s_gnssState = -1;
    Serial.println("[TidyOS Pro] GNSS skipped: the in-line SD card uses its pins");
    return false;
  }
#if !CONFIG_SPIRAM_MODE_OCT     // (V4-R8: GPIO 33-37 belong to its PSRAM and 40 to the screen: leave them alone)
  pinMode(GNSS_EN, OUTPUT);  digitalWrite(GNSS_EN, LOW);      // power on
  pinMode(GNSS_WAKE, OUTPUT); digitalWrite(GNSS_WAKE, HIGH);
  pinMode(GNSS_RST, OUTPUT); digitalWrite(GNSS_RST, LOW); delay(10); digitalWrite(GNSS_RST, HIGH);
#endif
  delay(300);
  static const int combos[4][3] = { { 9600, GNSS_RX, GNSS_TX }, { 9600, GNSS_TX, GNSS_RX }, { 115200, GNSS_RX, GNSS_TX }, { 115200, GNSS_TX, GNSS_RX } };
  for (auto & c : combos) {
    if (listenFor(c[0], c[1], c[2], 2500)) {
      gGnss.present = true; gGnss.baud = c[0]; gGnss.rxPin = c[1];
      s_gnssState = 1;
      Serial.printf("[TidyOS Pro] GNSS found: %d baud, RX on GPIO %d\n", c[0], c[1]);
      xTaskCreatePinnedToCore(gnssTask, "gnss", 6144, nullptr, 1, &s_gTask, 0);
      return true;
    }
  }
  s_gps.end();
#if !CONFIG_SPIRAM_MODE_OCT
  digitalWrite(GNSS_EN, HIGH);              // nothing there: switch the header's power off again
  pinMode(GNSS_EN, INPUT); pinMode(GNSS_WAKE, INPUT); pinMode(GNSS_RST, INPUT);
#endif
  s_gnssState = -1;
  Serial.println("[TidyOS Pro] no GNSS module on the header");
  return false;
#endif
}

bool gnssFixed() { std::lock_guard<std::mutex> lk(gMx); return gGnss.fix; }

}  // namespace rf

bool gnssPresent() { return rf::gnssStart(); }
bool gnssAlreadyFound() { return rf::gnssRunning(); }

#else   // not a V4: no radio
namespace rf {
Settings gSettings; Spectrum * gSpec = nullptr; Listen * gListen = nullptr; Gnss gGnss; std::mutex gMx;
bool begin() { return false; }
const char * lastError() { return "This board has no 900 MHz radio"; }
bool radioOk() { return false; }
bool wake() { return false; }
void setMode(Mode, int) {}
void powerDown() {}
Mode mode() { return Mode::Off; }
void resetStats() {}
void loadSettings() {}
void saveSettings() {}
float powerDensity(float, float, float) { return 0; }
bool gnssStart() { return false; }
bool gnssFixed() { return false; }
bool gnssRunning() { return false; }
void meshEnable(bool) {}
void meshOnReceive(void (*)(const uint8_t *, size_t)) {}
void meshSend(const uint8_t *, size_t) {}
bool meshListening() { return false; }
void meshCounts(int & s, int & h) { s = h = 0; }
}
bool gnssPresent() { return false; }
#endif
