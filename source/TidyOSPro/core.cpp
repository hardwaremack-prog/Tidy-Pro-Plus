#include <driver/rtc_io.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
// TidyOS core: display start-up, theme, settings, storage, desktop, main app
#include <Preferences.h>
#include <FFat.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include "screen.h"
#include "led.h"
#include <algorithm>
#include "esp_sleep.h"
#include <WiFi.h>
#include "crashlog.h"
#include "update.h"
#include "driver/spi_master.h"
#include <SPI.h>
#include <SD.h>
#include "driver/gpio.h"
#include "esp_bt.h"
#include "core.h"
#include "stream.h"
#include "input.h"
#include "net.h"
#include "style.h"
#include "spell.h"
#include "shelfmsg.h"
#include "fileserver.h"

using namespace fabgl;

Theme    T;
int      gColors      = 4;
bool     gSDMounted   = false;
bool     gFlashMounted= false;
Settings gSet;
uiApp *  gApp = nullptr;
volatile uint64_t gNetRx = 0, gNetTx = 0;
volatile bool gOpenTaskMgr = false;
volatile bool gRestartReq = false;
static RTC_NOINIT_ATTR uint32_t s_waitViewer;   // set by Restart: the start-up screen waits for the browser to come back
volatile bool gSleepReq = false;     // the page's Sleep button   // the page's Restart button

static Preferences s_prefs;
static BitmappedDisplayController * s_dc = nullptr;
static fabgl::Keyboard s_keyboard;     // never begun: only used to keep key state for FabGL widgets

// ====================================================================== settings

void settingsLoad()
{
  s_prefs.begin("tidyos", false);
  gSet.ssid      = s_prefs.getString("ssid", "");
  gSet.pass      = s_prefs.getString("pass", "");
  gSet.proxy     = s_prefs.getString("proxy", "");
  gSet.home      = s_prefs.getString("home", "http://frogfind.com");
  gSet.tzHours10 = s_prefs.getInt("tz", -50);
  gSet.dst       = s_prefs.getBool("dst", true);
  gSet.colors    = s_prefs.getInt("colors", 0);
  gSet.btNames   = s_prefs.getString("bt", "");
  gSet.printer   = s_prefs.getString("printer", "");
  gSet.fileShare = s_prefs.getBool("share", false);
  gSet.sdOn      = s_prefs.getBool("sdon", false);
  gSet.meshOn    = s_prefs.getBool("mesh", false);
  gSet.mesh900   = s_prefs.getBool("m900", true);
  gSet.ledOn     = s_prefs.getBool("led", false);
  gSet.shelfOn   = s_prefs.getBool("shelf", false);
  gSet.gpsOn     = s_prefs.getBool("gps", true);
  gSet.oledSleep = s_prefs.getBool("oledslp", true);
  gSet.oledOn    = s_prefs.getBool("oledon", true);
#ifdef TIDYOS_VGA_OUT
  gSet.vgaOn     = s_prefs.getBool("vga", true);
#endif
  gSet.tidyBands = s_prefs.getBool("tbands", false);
  gSet.tidyFreeze = s_prefs.getBool("tfreeze", false);
  gSet.hotspot = s_prefs.getBool("hotspot", false);
  gSet.hotspotCell = s_prefs.getBool("hscell", false);
  gSet.alarmOn   = s_prefs.getBool("alarm", false);
  gSet.alarmMin  = s_prefs.getInt("alarmm", 420);
  gSet.worldCity = s_prefs.getInt("wcity", 0);
  gSet.storeUrl  = s_prefs.getString("surl", "");
  if (s_prefs.getBytesLength("apre") == sizeof(gSet.alarmPresets)) s_prefs.getBytes("apre", gSet.alarmPresets, sizeof(gSet.alarmPresets));
  gSet.neatFont  = s_prefs.getInt("nfont", 0);
  gSet.neatSize  = s_prefs.getInt("nsize", 1);
}

void settingsReset()                // every setting back to how it came (PRG held 12 s)
{
  s_prefs.clear();
}

void settingsSave()
{
  s_prefs.putString("ssid", gSet.ssid);
  s_prefs.putString("pass", gSet.pass);
  s_prefs.putString("proxy", gSet.proxy);
  s_prefs.putString("home", gSet.home);
  s_prefs.putInt("tz", gSet.tzHours10);
  s_prefs.putBool("dst", gSet.dst);
  s_prefs.putInt("colors", gSet.colors);
  s_prefs.putString("bt", gSet.btNames);
  s_prefs.putString("printer", gSet.printer);
  s_prefs.putBool("share", gSet.fileShare);
  s_prefs.putBool("sdon", gSet.sdOn);
  s_prefs.putBool("mesh", gSet.meshOn);
  s_prefs.putBool("m900", gSet.mesh900);
  s_prefs.putBool("led", gSet.ledOn);
  s_prefs.putBool("shelf", gSet.shelfOn);
  s_prefs.putBool("gps", gSet.gpsOn);
  s_prefs.putBool("oledslp", gSet.oledSleep);
  s_prefs.putBool("oledon", gSet.oledOn);
#ifdef TIDYOS_VGA_OUT
  s_prefs.putBool("vga", gSet.vgaOn);
#endif
  s_prefs.putBool("tbands", gSet.tidyBands);
  s_prefs.putBool("tfreeze", gSet.tidyFreeze);
  s_prefs.putBool("hotspot", gSet.hotspot);
  s_prefs.putBool("hscell", gSet.hotspotCell);
  s_prefs.putBool("alarm", gSet.alarmOn);
  s_prefs.putInt("alarmm", gSet.alarmMin);
  s_prefs.putInt("wcity", gSet.worldCity);
  s_prefs.putString("surl", gSet.storeUrl);
  s_prefs.putBytes("apre", gSet.alarmPresets, sizeof(gSet.alarmPresets));
  s_prefs.putInt("nfont", gSet.neatFont);
  s_prefs.putInt("nsize", gSet.neatSize);
}

// ====================================================================== storage

std::string dataPath(const char * name)
{
  if (gSDMounted) return std::string(SD_MOUNT "/tidyos/") + name;
  return std::string(FLASH_HOME "/") + name;
}

std::string documentsDir()
{
  if (gSDMounted) return SD_MOUNT "/Documents";
  return FLASH_HOME;
}

// ---- the board's own storage (no SD card)
// V4: the 7.4 MB "ffat" area (FAT), shared with The Shelf, mounted at /flash; TidyOS Pro's files in /flash/TidyPro.
// The first time, anything in the old 1 MB "tidyfs" area is copied over. If the FAT area won't open, the old
// 1 MB area is used as before.
static bool s_flashIsFat = false;
#ifdef TIDYOS_V4
#include "esp_spiffs.h"
static void copyTree(const std::string & from, const std::string & to, int & files, int depth = 0)
{
  DIR * d = opendir(from.c_str());
  if (!d) return;
  mkdir(to.c_str(), 0777);
  struct dirent * e;
  while ((e = readdir(d)) != nullptr) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    std::string src = from + "/" + n, dst = to + "/" + n;
    if (e->d_type == DT_DIR) { if (depth < 6) copyTree(src, dst, files, depth + 1); continue; }
    for (size_t k = to.size() + 1; (k = dst.find('/', k)) != std::string::npos; ++k) mkdir(dst.substr(0, k).c_str(), 0777);   // SPIFFS names can hold "/"
    struct stat st;
    if (stat(dst.c_str(), &st) == 0) continue;                // already there: keep the newer one
    FILE * a = fopen(src.c_str(), "rb"), * b = a ? fopen(dst.c_str(), "wb") : nullptr;
    if (a && b) { static uint8_t buf[2048]; size_t r; while ((r = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, r, b); ++files; }
    if (b) fclose(b);
    if (a) fclose(a);
  }
  closedir(d);
}
static void moveOldFlash()
{
  Preferences p;
  if (!p.begin("tidyplus", false)) return;
  if (p.getBool("moved", false)) { p.end(); return; }
  esp_vfs_spiffs_conf_t c = { "/oldfl", "tidyfs", 4, false };
  int files = 0;
  if (esp_vfs_spiffs_register(&c) == ESP_OK) {
    copyTree("/oldfl", FLASH_HOME, files);
    esp_vfs_spiffs_unregister("tidyfs");
  }
  Serial.printf("[TidyOS] moved %d file(s) from the old 1 MB area to the board's storage\n", files);
  p.putBool("moved", true);
  p.end();
}
#endif
bool mountInternal()
{
#ifdef TIDYOS_V4
  bool ok = FFat.begin(false, FLASH_MOUNT, 4, "ffat");
  if (!ok) {                                                  // never set up (nothing on it to lose)
    Serial.println("[TidyOS] setting up the board's storage (first time)");
    ok = FFat.format(false, (char *)"ffat") && FFat.begin(false, FLASH_MOUNT, 4, "ffat");
  }
  if (ok) {
    s_flashIsFat = true;
    mkdir(FLASH_HOME, 0777);
    mkdir(FLASH_HOME "/Music", 0777);       // ready for songs dropped on the desktop (the Music app looks here
    mkdir(FLASH_HOME "/Pictures", 0777);    // when there's no SD card) and photos
    moveOldFlash();
    Serial.printf("[TidyOS] board storage: %u KB free of %u KB (%s)\n", (unsigned)((FFat.totalBytes() - FFat.usedBytes()) / 1024), (unsigned)(FFat.totalBytes() / 1024),
                  esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "shelf") ? "shared with The Shelf" : "TidyOS Pro only");
    return true;
  }
  Serial.println("[TidyOS] the board's storage won't open - using the old 1 MB area");
#endif
  s_flashIsFat = false;
  return FileBrowser::mountSPIFFS(true, FLASH_MOUNT, 2);
}
// size of the board's storage (whichever is in use)
bool internalFSInfo(int64_t & total, int64_t & used)
{
  if (!gFlashMounted) return false;
#ifdef TIDYOS_V4
  if (s_flashIsFat) { total = FFat.totalBytes(); used = FFat.usedBytes(); return true; }
#endif
  return FileBrowser::getFSInfo(fabgl::DriveType::SPIFFS, 0, &total, &used);
}
bool internalIsShared() { return s_flashIsFat; }

bool readFileStr(const char * path, std::string & out, size_t maxLen)
{
  FILE * f = fopen(path, "rb");
  if (!f) return false;
  out.clear();
  char buf[256];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (out.size() + n > maxLen) { n = maxLen - out.size(); out.append(buf, n); break; }
    out.append(buf, n);
  }
  fclose(f);
  return true;
}

bool writeFileStr(const char * path, const std::string & data)
{
  FILE * f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  fclose(f);
  return ok;
}

bool writeFileWith(const char * path, const std::function<void(RSink &)> & fill)
{
  std::string tmp = std::string(path) + ".tmp";
  FILE * f = fopen(tmp.c_str(), "wb");
  bool direct = !f;                             // e.g. name too long for the flash filesystem
  if (direct) f = fopen(path, "wb");
  if (!f) return false;
  FileSink o(f);
  fill(o);
  bool ok = o.flush();
  if (fclose(f) != 0) ok = false;
  if (direct) return ok;
  if (!ok) { remove(tmp.c_str()); return false; }
  remove(path);
  return rename(tmp.c_str(), path) == 0;
}

long fileSize(const char * path)
{
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long)st.st_size;
}

const char * fileExt(const char * path)
{
  static char ext[8];
  const char * dot = strrchr(path, '.');
  const char * sl  = strrchr(path, '/');
  if (!dot || (sl && dot < sl) || strlen(dot + 1) >= sizeof(ext)) return "";
  int i = 0;
  for (const char * p = dot + 1; *p; ++p) ext[i++] = tolower(*p);
  ext[i] = 0;
  return ext;
}

std::string fmtMoney(double v)
{
  char b[32];
  snprintf(b, sizeof(b), "$%.2f", v);
  return b;
}

// ====================================================================== theme

struct V8  : public VGA8Controller  { void lut() { updateRGB2PaletteLUT(); } };
struct V2  : public VGA2Controller  { void lut() { updateRGB2PaletteLUT(); } };
struct V4  : public VGA4Controller  { void lut() { updateRGB2PaletteLUT(); } };

RGB888 themePal(int i);
static const RGB888 PAL[16] = {
  RGB888(0, 0, 0),       RGB888(255, 255, 255), RGB888(170, 170, 170), RGB888(0, 85, 170),     // 4-colour set
  RGB888(85, 85, 85),    RGB888(0, 170, 170),   RGB888(170, 0, 0),     RGB888(0, 170, 0),      // 8-colour set
  RGB888(255, 255, 85),  RGB888(85, 170, 255),  RGB888(0, 0, 85),      RGB888(255, 170, 0),
  RGB888(255, 85, 170),  RGB888(85, 255, 85),   RGB888(85, 0, 170),    RGB888(170, 85, 0),
};

void themeSetup(BitmappedDisplayController * dc, int colors)
{
  gColors = colors;
  if (colors == 8)  { auto c = (V8*)dc;  for (int i = 0; i < 8; ++i)  c->setPaletteItem(i, PAL[i]); c->lut(); }
  if (colors == 4)  { auto c = (V4*)dc;  for (int i = 0; i < 4; ++i)  c->setPaletteItem(i, PAL[i]); c->lut(); }
  if (colors == 2)  { auto c = (V2*)dc;  for (int i = 0; i < 2; ++i)  c->setPaletteItem(i, PAL[i]); c->lut(); }
  vgaS3Palette(PAL, colors);        // (XIAO: the monitor's copy of the palette)

  T.win        = PAL[1];
  T.text       = PAL[0];
  T.face       = PAL[2];
  T.accent     = PAL[3];
  T.accentText = PAL[1];
  if (colors == 2) {           // black and white, like the first Macs
    T.face = PAL[1]; T.accent = PAL[0]; T.accentText = PAL[1];
    T.desk = PAL[1]; T.deskText = PAL[0]; T.shadow = PAL[0]; T.good = PAL[0]; T.warn = PAL[0];
  } else if (colors == 4) {
    T.desk = PAL[3]; T.deskText = PAL[1]; T.shadow = PAL[0]; T.good = PAL[3]; T.warn = PAL[0];
  } else {
    T.desk = PAL[5]; T.deskText = PAL[1]; T.shadow = PAL[4]; T.good = PAL[7]; T.warn = PAL[6];
  }
}

// ====================================================================== shared UI helpers

Point clientOrigin(uiFrame * f)
{
  Rect r = f->clientRect(uiOrigin::Window);
  return Point(r.X1, r.Y1);
}

uiLabel * makeSavedLight(uiWindow * parent, int x, int y)
{
  auto l = S(new uiLabel(parent, "  Saved  ", Point(x, y), Size(70, 18)));
  l->labelStyle().textAlign = uiHAlign::Center;
  setSavedLight(l, true);
  return l;
}

void setSavedLight(uiLabel * l, bool saved)
{
  if (!l) return;
  l->labelStyle().backgroundColor = saved ? T.good : T.face;
  l->labelStyle().textColor       = saved ? T.accentText : T.text;
  l->setText(saved ? "Saved" : "Not saved");
  l->repaint();
}

// minimized windows wait on the taskbar (between RAM and the clock)
static std::vector<uiFrame *> s_minimized;
static uiControl * s_deskCtl = nullptr;
static int s_ramX = 330;          // where the RAM text starts on the taskbar
// free space where files go: the SD card when it's on, otherwise TidyOS Pro's own flash (bar and OLED)
extern volatile bool gStorageChanged;
bool storageFree(int64_t & freeB, bool & sd)
{
  int64_t total = 0, used = 0;
  if (gSDMounted && FileBrowser::getFSInfo(fabgl::DriveType::SDCard, 0, &total, &used)) { freeB = total - used; sd = true; return true; }
  if (internalFSInfo(total, used)) { freeB = total - used; sd = false; return true; }
  return false;
}

static int s_clockX = 0;                  // where the bar's clock starts (it moves with the seconds)
static Rect s_optBtn;             // the Optimize button (before the RAM text)
static int s_ramEnd = 0;          // where the RAM text ends
static std::string s_optMsg;      // what Optimize did (shown for a few seconds)
static uint32_t s_optUntil = 0;
static void optimizeRam()
{
  size_t before = ramFree();
  WiFi.scanDelete();              // old network lists
  screenTrim();                   // idle web connections; the setup hotspot when it's not needed
  vTaskDelay(pdMS_TO_TICKS(400)); // closed connections hand their memory back
  int freed = ((int)ramFree() - (int)before) / 1024;
  char b[64];
  if (freed > 0) snprintf(b, sizeof(b), "Optimized: %dK freed - RAM %uK", freed, (unsigned)(ramFree() / 1024));
  else snprintf(b, sizeof(b), "Already tidy - RAM %uK (close apps to free more)", (unsigned)(ramFree() / 1024));
  s_optMsg = b;
  s_optUntil = millis() + 5000;
}
static void taskbarRepaint() { if (s_deskCtl) s_deskCtl->repaint(Rect(0, SCREEN_H - 20, SCREEN_W - 1, SCREEN_H - 1)); }
volatile bool gStorageChanged = false;
void storageChanged() { gStorageChanged = true; taskbarRepaint(); }   // a file was deleted or added: the bar's free space now
void taskbarMinimize(uiFrame * f)
{
  if (std::find(s_minimized.begin(), s_minimized.end(), f) == s_minimized.end()) s_minimized.push_back(f);
  gApp->showWindow(f, false);
  taskbarRepaint();
}
void taskbarForget(uiFrame * f)
{
  auto it = std::find(s_minimized.begin(), s_minimized.end(), f);
  if (it != s_minimized.end()) { s_minimized.erase(it); taskbarRepaint(); }
}

void bringToFront(uiFrame * f)
{
  taskbarForget(f);                       // (back from the taskbar)
  gApp->showWindow(f, true);
  gApp->setActiveWindow(f);
}

// ====================================================================== desktop

static const char * ART_CALC[16] = {
  "..############..", "..#wwwwwwwwww#..", "..#waaaaaaaaw#..", "..#waaaaaaaaw#..",
  "..#wwwwwwwwww#..", "..#w##w##w##w#..", "..#w##w##w##w#..", "..#wwwwwwwwww#..",
  "..#w##w##w##w#..", "..#w##w##w##w#..", "..#wwwwwwwwww#..", "..#w##w##waaw#..",
  "..#w##w##waaw#..", "..#wwwwwwwwww#..", "..############..", "................" };
static const char * ART_NEAT[16] = {
  "..#########.....", "..#wwwwwww##....", "..#wwwwwww#w#...", "..#wwwwwww####..",
  "..#wwwwwwwwwww#.", "..#wa#######aw#.", "..#wwwwwwwwwww#.", "..#w########ww#.",
  "..#wwwwwwwwwww#.", "..#w#########w#.", "..#wwwwwwwwwww#.", "..#w######wwww#.",
  "..#wwwwwwwwwww#.", "..#wwwwwwwwwww#.", "..#############.", "................" };
static const char * ART_TIDY[16] = {
  "################", "#aaaa#aaaa#aaaa#", "################", "#wwww#wwww#wwww#",
  "#wwww#wwww#wwww#", "################", "#wwww#wwww#wwww#", "#wwww#wwww#wwww#",
  "################", "#wwww#wwww#wwww#", "#wwww#wwww#wwww#", "################",
  "#wwww#wwww#wwww#", "#wwww#wwww#wwww#", "################", "................" };
static const char * ART_READY[16] = {
  "################", "#wwwwwwwwwwwwww#", "#w###wwwwwwwwww#", "#w#a#w########w#",
  "#w###wwwwwwwwww#", "#wwwwwwwwwwwwww#", "#w###wwwwwwwwww#", "#w#a#w########w#",
  "#w###wwwwwwwwww#", "#wwwwwwwwwwwwww#", "#w###wwwwwwwwww#", "#w#w#w########w#",
  "#w###wwwwwwwwww#", "#wwwwwwwwwwwwww#", "################", "................" };
static const char * ART_FILES[16] = {
  "................", "................", ".#####..........", "#ggggg#.........",
  "#gggggg#########", "#gggggggggggggg#", "################", "#wwwwwwwwwwwwww#",
  "#gggggggggggggg#", "#gggggggggggggg#", "#gggggggggggggg#", "#gggggggggggggg#",
  "#gggggggggggggg#", "################", "................", "................" };
static const char * ART_GEAR[16] = {
  "......####......", "......#gg#......", "..##.#gggg#.##..", "..#g#gggggg#g#..",
  "...#gggggggg#...", "..#ggg####ggg#..", "###gg#wwww#gg###", "#ggg#wwwwww#ggg#",
  "#ggg#wwwwww#ggg#", "###gg#wwww#gg###", "..#ggg####ggg#..", "...#gggggggg#...",
  "..#g#gggggg#g#..", "..##.#gggg#.##..", "......#gg#......", "......####......" };
static const char * ART_SYS[16] = {
  "################", "#aaaaaaaaaaaaaa#", "#awwwaaaaaaaaaa#", "#aaaaaaaaaaaaaa#",
  "#awwwwwwaaaaaaa#", "#aaaaaaaaaaaaaa#", "#awwwwaaaaaaaaa#", "#aaaaaaaaaaaaaa#",
  "#aaaaaaaaaaaaaa#", "################", "......####......", "......#gg#......",
  "....########....", "....#gggggg#....", "....########....", "................" };

static const char * ART_MSG[16] = {
  "................", ".##############.", "#wwwwwwwwwwwwww#", "#wwwwwwwwwwwwww#",
  "#ww#########www#", "#wwwwwwwwwwwwww#", "#ww######wwwwww#", "#wwwwwwwwwwwwww#",
  "#wwwwwwwwwwwwww#", ".######ww######.", "......#ww#......", ".......#w#......",
  "........##......", "................", "................", "................" };

static const char * ART_PING[16] = {
  "................", "..w..........w..", ".w..w......w..w.", "w..w...##...w..w",
  "w.w...#ww#...w.w", "w.w...#ww#...w.w", "w..w...##...w..w", ".w..w..##..w..w.",
  "..w....##....w..", ".......##.......", "......#gg#......", ".....#gggg#.....",
  "....#gggggg#....", "...##########...", "................", "................" };


// ---- TidyOS Pro field-tool icons ('#' outline, 'w' white, 'g' grey, 'a' accent)
static const char * ART_RFSWEEP[16] = {
  "################", "#wwwwwwwwwwwwww#", "#wwwwwwwawwwwww#", "#wwwwwwwawwwwww#", "#wwwwwwaaawwwww#", "#wwwwawaaawwwww#",
  "#wwwwawaaawawww#", "#wwwaaaaaaawaww#", "#wawaaaaaaaaaaw#", "#aaaaaaaaaaaaaa#", "#gggggggggggggg#", "#wwwwwwwwwwwwww#",
  "#w#ww#ww#ww#ww##", "################", "................", "................" };
static const char * ART_AMI[16] = {
  "....########....", "...#wwwwwwww#...", "..#ww######ww#..", "..#w#aaaaaa#w#..", "..#w#a#aa#a#w#..", "..#w#aaaaaa#w#..",
  "..#ww######ww#..", "..#wwwwwwwwww#..", "..#ww#gggg#ww#..", "..#wwwwwwwwww#..", "...#wwwwwwww#...", "....########....",
  "..w...w..w...w..", ".w...w....w...w.", "................", "................" };
static const char * ART_SURVEY[16] = {
  "................", "......####......", ".....#aaaa#.....", "....#aa##aa#....", "....#a#ww#a#....", "....#aa##aa#....",
  ".....#aaaa#.....", "......#aa#......", ".......##.......", "..gggg....gggg..", ".g#gg#gggg#gg#g.", "gggggg#gg#gggggg",
  ".gggggggggggggg.", "..gggggggggggg..", "................", "................" };
static const char * ART_EXPO[16] = {
  "................", ".......##.......", "......#aa#......", "......#aa#......", ".....#aaaa#.....", ".....#a##a#.....",
  "....#aa##aa#....", "....#aa##aa#....", "...#aaa##aaa#...", "...#aaaaaaaa#...", "..#aaaa##aaaa#..", "..#aaaa##aaaa#..",
  ".#aaaaaaaaaaaa#.", ".##############.", "................", "................" };
static const char * ART_GPS[16] = {
  "................", ".......#........", "......#w#.......", ".....#www#......", "....#wwwww#.....", "...#www#www#....",
  "..#wwww#wwww#...", ".#######a#####..", "..#wwww#wwww#...", "...#www#www#....", "....#wwwww#.....", ".....#www#......",
  "......#w#.......", ".......#........", "................", "................" };
static const char * ART_CLONE[16] = {
  "................", ".#######........", ".#wwwww#........", ".#waaaw#........", ".#wwwww#.....#..", ".#wwwww#.....##.",
  ".#######aaaaa###", "........aaaaa###", ".............##.", ".........#######", ".........#wwwww#", ".........#waaaw#",
  ".........#wwwww#", ".........#######", "................", "................" };

static const char * ART_DOODLE[16] = {
  "................", "..############..", ".#wwwwwwwwwww#..", ".#ww##wwwwww##..", ".#w#ww#wwww#a#..", ".#w#ww#www#a#w..",
  ".#ww##wwww#a#w..", ".#wwwwwww#a#ww..", ".#wwwwww#a#www..", ".#wgggw##a#www..", ".#wgggw#g#wwww..", ".#wwwww##wwwww..",
  ".#wwwwwwwwwwww..", "..############..", "................", "................" };

static const char * ART_PICS[16] = {
  "................", ".##############.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwaaww#.", ".#wwwwwwwwaaww#.", ".#wwwwwwwwwwww#.",
  ".#wwww#wwwwwww#.", ".#www###wwwwww#.", ".#ww#####w#www#.", ".#w#######g#ww#.", ".##########gg##.", ".#gggggggggggg#.",
  ".##############.", "................", "................", "................" };

static const char * ART_MUSIC[16] = {
  "................", "......#######...", "......#aaaaa#...", "......#######...", "......#.....#...", "......#.....#...",
  "......#.....#...", "......#.....#...", "....###...###...", "...####..####...", "...####..####...", "....##....##....",
  "................", "................", "................", "................" };

static const char * ART_SHOT[16] = {
  "................",
  "................",
  "....#####.......",
  "..############..",
  ".#gggggggggggg#.",
  ".#gggg####gaag#.",
  ".#ggg#wwww#ggg#.",
  ".#gg#wwaaww#gg#.",
  ".#gg#wwaaww#gg#.",
  ".#ggg#wwww#ggg#.",
  ".#gggg####gggg#.",
  ".#gggggggggggg#.",
  "..############..",
  "................",
  "................",
  "................",
};

static const char * ART_CLOCK[16] = {
  ".......##.......",
  "....########....",
  "...##wwwwww##...",
  "..##www#wwww##..",
  ".##wwww#wwwww##.",
  ".#wwwww#wwwwww#.",
  ".#wwwww#wwwwww#.",
  "##wwwww#aaaaww##",
  "##wwwwwwwwwwww##",
  ".#wwwwwwwwwwww#.",
  ".#wwwwwwwwwwww#.",
  ".##wwwwwwwwww##.",
  "..##wwwwwwww##..",
  "...##wwwwww##...",
  "....########....",
  ".......##.......",
};

static const char * ART_LOCK[16] = {
  "................",
  "......####......",
  ".....#....#.....",
  "....#......#....",
  "....#......#....",
  "....#......#....",
  "..############..",
  "..#aaaaaaaaaa#..",
  "..#aaaaaaaaaa#..",
  "..#aaaa##aaaa#..",
  "..#aaaa##aaaa#..",
  "..#aaaaa#aaaa#..",
  "..#aaaaaaaaaa#..",
  "..#aaaaaaaaaa#..",
  "..############..",
  "................",
};

static const char * ART_HTMLG[16] = {
  "################",
  "#aaaaaaaaaaaa#w#",
  "################",
  "#wwwwwwwwwwwwww#",
  "#wwwwwwwwwwwwww#",
  "#www########www#",
  "#ww#gggggggg#ww#",
  "#ww#g#gggg#g#ww#",
  "#ww####gg#g##ww#",
  "#ww#g#gggggg#ww#",
  "#ww#gggggggg#ww#",
  "#www########www#",
  "#wwwwwwwwwwwwww#",
  "#wwwwwwwwwwwwww#",
  "################",
  "................",
};

static const char * ART_HTMLP[16] = {
  "################",
  "#aaaaaaaaaaaa#w#",
  "################",
  "#wwwwwwwwwwwwww#",
  "#wwwwwwwwwwwwww#",
  "#ww#wwwwww#wwww#",
  "#w#wwwwww#wwwww#",
  "##wwwwww#wwwwww#",
  "#w#wwwww#wwwwww#",
  "#ww#wwww#ww#www#",
  "#wwwwww#wwww#ww#",
  "#wwwwww#wwwww#w#",
  "#wwwwww#wwww#ww#",
  "#wwwwwwwwww#www#",
  "################",
  "................",
};

static const char * ART_CLOUD[16] = {
  "................",
  "................",
  "......####......",
  "....##wwww##....",
  "...#wwwwwwww#...",
  "..#wwwwwwwwww##.",
  ".##wwwwwwwwwwww#",
  "#wwwwwwaawwwwww#",
  "#wwwwwaaaawwwww#",
  "#wwwwaaaaaawwww#",
  "#wwwwwwaawwwwww#",
  ".##wwwwaawwwww#.",
  "...####aa#####..",
  ".......aa.......",
  "....aaaaaaaa....",
  "................",
};

static const char * ART_NIC[16] = {
  "................", ".##############.", ".#gggggggggggg#.", ".#g####ggwwwwg#.", ".#g#aa#ggw##wg#.", ".#g#aa#ggw##wg#.",
  ".#g####ggwwwwg#.", ".#gggggggggggg#.", ".#gg#g#g#g#ggg#.", ".##############.", "..a.a.a.a.a.a...", "..a.a.a.a.a.a...",
  "................", "...#...#...#....", "..#.#.#.#.#.#...", ".#...#...#...#.." };

static const char * ART_SCAN[16] = {
  "................", "......#####.....", "....##wwwww##...", "...#wwwww#www#..", "..#wwwww#wwwww#.", "..#wwww#wwwwww#.",
  ".#wwwww#waawww#.", ".#wwwwwwwaawww#.", ".#wwwwwwwwwwww#.", "..#wawwwwwwww#..", "..#wwwwwwwaw#...", "...#wwwwwwww#...",
  "....##wwwww##...", "......#####.....", "................", "................" };

static const char * ART_SD[16] = {
  "................", "...#########....", "...#a#a#a#a##...", "...#a#a#a#a#w#..",
  "...#wwwwwwwwww#.", "...#wwwwwwwwww#.", "...#wggggggggw#.", "...#wgwwwwwwgw#.",
  "...#wgw##wwwgw#.", "...#wgwwwwwwgw#.", "...#wggggggggw#.", "...#wwwwwwwwww#.",
  "...#wwwwwwwwww#.", "...############.", "................", "................" };

static const char * ART_GAMES[16] = {
  "................", "................", "................", "...##########...",
  "..#gggggggggg#..", ".#gg#gggggggag#.", ".#g###ggggagga#.", ".#gg#ggggggaag#.",
  ".#ggggg##gggggg#", ".#gggg#..#ggggg#", "..####....#####.", "................",
  "................", "................", "................", "................" };

static const char * ART_RUGGED[16] = {
  ".......##.......", "......#gg#......", ".....#gggg#.....", "....#gg##gg#....",
  "...#gg#aa#gg#...", "..#gg#aaaa#gg#..", ".#gg#aaaaaa#gg#.", "#ggg#aaaaaa#ggg#",
  "#ggg#aaaaaa#ggg#", "#gggg######gggg#", "#gggggggggggggg#", "#gggggg##gggggg#",
  "#gggggg##gggggg#", "#gggggg##gggggg#", "################", "................" };

struct DeskIcon {
  const char *  label;
  const char ** art;      // pixel art, or nullptr for a glyph tile
  const char *  glyph;
  void (*open)();
  uint8_t       bit;      // which bit of the app list turns it on (fixed: the web flasher uses these numbers)
};

static void openNeat0()    { openNeat(nullptr); }
static void openTidy0()    { openTidy(nullptr); }
#if TIDY_BROWSER
static void openBrowser0() { openBrowser(nullptr); }
#endif
#if TIDY_PICTURES
static void openPictures0() { openPictures(nullptr); }
#endif

// roughly how much memory each app takes while open (measured on the board), in KB
static int appNeedKB(const char * label)
{
  static const struct { const char * n; int kb; } T_[] = {
    { "Task Manager", 18 }, { "Files", 14 }, { "Neat", 22 }, { "Tidy", 20 }, { "Doodle", 18 }, { "Browser", 30 },
    { "Rugged", 18 }, { "Pictures", 24 }, { "Music", 14 }, { "Nic Sweep", 12 }, { "Screenshot", 6 }, { "Clock", 10 }, { "Latch", 24 }, { "HTML Games", 8 }, { "HTML Programs", 8 }, { "Git-Store", 12 }, { "Settings", 16 }, { "Messages", 16 }, { "Ping", 14 }, { "Games", 6 }, { "Steady", 14 }, { "RF Sweep", 16 }, { "Meter Traffic", 14 }, { "Site Survey", 16 }, { "RF Exposure", 10 }, { "GPS", 10 }, { "Clone", 10 }, { "Wi-Fi Scan", 10 }, { "The Shelf", 1 },
  };
  for (auto & e : T_) if (!strcmp(e.n, label)) return e.kb;
  return 10;
}
static bool appIsOpen(const char * label)
{
  for (uiWindow * w = gApp->rootWindow()->firstChild(); w; w = w->next())
    if (w->objectType().uiFrame && w->state().visible && ((uiFrame *)w)->title() && !strncmp(((uiFrame *)w)->title(), label, strlen(label))) return true;
  return false;
}

// Tidy Pro Plus: every TidyOS app, plus the field tools. Which ones are on the desktop is
// chosen in the web flasher: it writes the list into TIDY_APP_LIST below (and then the board
// keeps it in its settings, so an update doesn't undo it). The bit numbers must not change.
// Builds without TIDY_ALL_APPS (the smaller "field" build) leave the TidyOS-ESP32 extras out.
#ifndef TIDY_ALL_APPS
#define TIDY_ALL_APPS 0
#endif
static const DeskIcon ALL_ICONS[] = {
  { "RF Sweep",   ART_RFSWEEP, nullptr, openRfSweep, 0 },
  { "Meter Traffic", ART_AMI,  nullptr, openMeterTraffic, 1 },
  { "Site Survey", ART_SURVEY, nullptr, openSiteSurvey, 2 },
  { "RF Exposure", ART_EXPO,  nullptr, openRfExposure, 3 },
  { "GPS",        ART_GPS,   nullptr, openGps, 4 },
  { "Clone",      ART_CLONE, nullptr, openClone, 5 },
  { "Wi-Fi Scan", nullptr,   "WiFi",  openWifiScan, 6 },
  { "Nic Sweep",  ART_NIC,   nullptr, openScan, 7 },
  { "Ping",       ART_PING,  nullptr, openPing, 8 },
  { "Tidy",       ART_TIDY,  nullptr, openTidy0, 9 },
  { "Neat",       ART_NEAT,  nullptr, openNeat0, 10 },
#if TIDY_ALL_APPS
  { "Ready",      ART_READY, nullptr, openReady, 22 },
  { "Steady",     nullptr,   "$",     openSteady, 23 },
  { "Rugged",     ART_RUGGED, nullptr, openRugged, 24 },
#endif
  { "Files",      ART_FILES, nullptr, openFiles, 11 },
  { "Calculator", ART_CALC,  nullptr, openCalculator, 12 },
#if TIDY_BROWSER
  { "Browser",    nullptr,   "www",   openBrowser0, 13 },
#endif
  { "Messages",   ART_MSG,   nullptr, openMessages, 14 },
  { "Clock",      ART_CLOCK, nullptr, openClock, 15 },
#if TIDY_ALL_APPS
  { "Games",      ART_GAMES, nullptr, openGames, 25 },
  { "Doodle",     ART_DOODLE, nullptr, openDoodle, 26 },
#if TIDY_PICTURES
  { "Pictures",   ART_PICS,  nullptr, openPictures0, 27 },
#endif
  { "Music",      ART_MUSIC, nullptr, openMusic, 28 },
  { "Latch",      ART_LOCK,  nullptr, openLatch, 29 },
#if TIDY_HTML_APPS
  { "HTML Games", ART_HTMLG, nullptr, openHtmlGames, 30 },
  { "HTML Programs", ART_HTMLP, nullptr, openHtmlPrograms, 31 },
#endif
#endif
  { "Screenshot", ART_SHOT,  nullptr, openShot, 16 },
  { "Git-Store",  ART_CLOUD, nullptr, openGitStore, 17 },
  { "Settings",   ART_GEAR,  nullptr, openSettings, 18 },
  { "Task Manager", ART_SYS, nullptr, openSystem, 19 },
  { "Switches",   ART_SD,    nullptr, openSwitches, 20 },
  { "The Shelf",  nullptr,   "Shelf", openShelfSwitch, 21 },
};

// The web flasher finds this by its name and writes the ticked apps into it (then fixes the
// image's checksum and hash). "set" is 1 only in a copy the flasher wrote.
struct TidyAppList { char magic[16]; uint32_t apps; uint32_t set; };
extern "C" __attribute__((used)) const volatile TidyAppList TIDY_APP_LIST = { "TIDYPLUS-APPS-1", 0xFFFFFFFFu, 0 };

static DeskIcon ICONS[sizeof(ALL_ICONS) / sizeof(ALL_ICONS[0])];
static int NICONS = 0;
static uint32_t s_appBits = 0xFFFFFFFFu;

// Which apps go on the desktop: the flasher's list when it just wrote one (kept in the settings
// from then on), otherwise the kept list, otherwise all of them. Settings is always there.
static void chooseDeskApps()
{
  uint32_t bits = 0xFFFFFFFFu;
  Preferences p;
  if (p.begin("tidyplus", false)) {
    if (TIDY_APP_LIST.set == 1) {
      bits = TIDY_APP_LIST.apps;
      if (p.getUInt("apps", 0) != bits || !p.isKey("apps")) p.putUInt("apps", bits);
    } else bits = p.getUInt("apps", 0xFFFFFFFFu);
    p.end();
  } else if (TIDY_APP_LIST.set == 1) bits = TIDY_APP_LIST.apps;
  bits |= 1u << 18;
  s_appBits = bits;
  NICONS = 0;
  for (auto & ic : ALL_ICONS)
    if (bits & (1u << ic.bit)) ICONS[NICONS++] = ic;
  Serial.printf("[TidyOS] %d apps on the desktop (list %08x%s)\n", NICONS, (unsigned)bits, TIDY_APP_LIST.set == 1 ? ", from the flasher" : "");
}

static std::string s_bootNote;              // shown once on the desktop (update result)

#include "rf_engine.h"
void oledBegin();   // oled.cpp
void oledOff();
#include "desk_panel.h"
std::string musicQuickToggle(bool midi);   // media_apps.h: the panel's MP3 and volume buttons
std::string musicQuickVol(int d);
std::string musicQuickNext();
void musicQuickTick();
extern volatile bool gMusicNextReq;   // screen.cpp
#ifdef TIDYOS_PRO
// a window (other than the desktop itself) is over the news line
static bool tickerCovered()
{
  Rect k = newsTickerRect();
  for (uiWindow * w = gApp->rootWindow()->firstChild(); w; w = w->next())
    if (w != s_deskCtl && w->state().visible && w->rect(uiOrigin::Screen).intersects(k)) return true;
  return false;
}
#endif

class Desktop : public uiControl {
public:
  static const int BAR_H = 20;
  static const int CELL_W = 80, X0 = 6, COLS = 4;   // 4 columns: 24 icons fit, up to 32 with shorter rows
  int CELL_H = 72, Y0 = 6;

  Desktop(uiWindow * parent) : uiControl(parent, Point(0, 0), Size(SCREEN_W, SCREEN_H), true, 0) {
    windowStyle().borderSize = 0;
    windowStyle().focusedBorderSize = 0;
    setFocusIndex(-1);
  }

  void processEvent(uiEvent * event) override {
    uiControl::processEvent(event);
    switch (event->id) {
      case UIEVT_PAINT:
        beginPaint(event, Rect(0, 0, SCREEN_W - 1, SCREEN_H - 1));
        paint(Rect(event->params.rect.X1, event->params.rect.Y1, event->params.rect.X2, event->params.rect.Y2));
        break;
      case UIEVT_MOUSEBUTTONDOWN:
        if (event->params.mouse.changedButton == 1) {
          int mx = event->params.mouse.status.X, my = event->params.mouse.status.Y;
          if (my >= SCREEN_H - BAR_H && mx >= s_optBtn.X1 && mx <= s_optBtn.X2 && s_optBtn.X2 > 0) {
            optimizeRam();
            repaint(Rect(0, SCREEN_H - BAR_H, SCREEN_W - 1, SCREEN_H - 1));
            break;
          }
          if (my >= SCREEN_H - BAR_H && !s_minimized.empty()) {          // a minimized window's button
            Canvas cv(s_dc);
            int x1, x2; std::vector<Rect> slots;
            taskSlots(&cv, ramText().c_str(), x1, x2, slots);
            for (size_t i = 0; i < slots.size(); ++i)
              if (mx >= slots[i].X1 && mx <= slots[i].X2) { uiFrame * f = s_minimized[i]; bringToFront(f); break; }
            break;
          }
#ifdef TIDYOS_HTML5
          if (!showingHint()) {                                     // the panel's music buttons: MP3, volume - / +
            auto in = [&](const Rect & q) { return q.X2 > q.X1 && mx >= q.X1 && mx <= q.X2 && my >= q.Y1 && my <= q.Y2; };
            if (in(s_newsTag)) {                                     // the NEWS button: headlines the other way
              s_newsRev = !s_newsRev;
              repaint(newsTickerRect());
              break;
            }
            int what = in(s_mp3Btn) ? 0 : in(s_volDnBtn) ? 1 : in(s_volUpBtn) ? 2 : in(s_nextBtn) ? 3 : -1;
            if (what >= 0) {
              std::string m = what == 0 ? musicQuickToggle(false) : what == 3 ? musicQuickNext() : musicQuickVol(what == 1 ? -1 : 1);
              if ((what == 0 || what == 3) && m.rfind("Music: ", 0) == 0) deskNote("Playing", m.substr(7));
              else if (what == 1 || what == 2) deskNote("Volume", m.substr(m.find(' ') + 1));
              else deskNote("Music", m.empty() ? "Nothing playing" : m);
              repaint(deskPanelRect());
              break;
            }
          }
#endif
          if (!showingHint() && mx >= s_affBtn.X1 && mx <= s_affBtn.X2 && my >= s_affBtn.Y1 && my <= s_affBtn.Y2) {
            s_affShown = s_affShown % 3 + 1;                      // the affirmation >> button
            repaint(deskPanelRect());
            break;
          }
          int i = hit(mx, my);
          if (i >= 0) { m_pressed = i; repaintIcon(i); }
        }
        break;
      case UIEVT_MOUSEMOVE: {
        int mx = event->params.mouse.status.X, my = event->params.mouse.status.Y, h = -1;
        if (!showingHint() && s_graphRect.X2 > s_graphRect.X1 && mx >= s_graphRect.X1 && mx <= s_graphRect.X2 && my >= s_graphRect.Y1 && my <= s_graphRect.Y2 && s_tempsN > 1)
          h = ((mx - s_graphRect.X1) * (s_tempsN - 1) + (s_graphRect.width() / 2)) / imax(1, s_graphRect.width() - 1);
        if (h != s_graphHover) { s_graphHover = h; Rect g = s_graphRect; repaint(Rect(g.X1 - 2, g.Y1 - 2, g.X2 + 2, g.Y2 + 16)); }
        break;
      }
      case UIEVT_MOUSEBUTTONUP:
        if (event->params.mouse.changedButton == 1 && m_pressed >= 0) {
          int i = m_pressed;
          m_pressed = -1;
          repaintIcon(i);
          if (hit(event->params.mouse.status.X, event->params.mouse.status.Y) == i) {
            // open a moment later: the second click of a double-click then lands on the icon,
            // not on a button (like Save) in the window that just opened
            if (m_pendingOpen < 0) { m_pendingOpen = i; m_pendingAt = millis(); }
          }
        }
        break;
      case UIEVT_TIMER:
        if (m_pendingOpen >= 0 && millis() - m_pendingAt >= 350) {
          int i = m_pendingOpen;
          m_pendingOpen = -1;
          {
            Serial.printf("[TidyOS] open %s, free RAM %u (biggest %u)\n", ICONS[i].label, (unsigned)ramFree(), (unsigned)ramBiggest());
            int need = appNeedKB(ICONS[i].label);
            if (!appIsOpen(ICONS[i].label) && (int)(ramFree() / 1024) < need + 10) {
              char m[100];
              snprintf(m, sizeof(m), "%s needs ~%d KB, %u KB free. Close an app, or pick Black & white at start-up.",
                       ICONS[i].label, need, (unsigned)(ramFree() / 1024));
              gApp->messageBox("Not enough memory", m, "OK", nullptr, nullptr, uiMessageBoxIcon::Warning);
            } else
              ICONS[i].open();
          }
        }
        if (gOpenTaskMgr) { gOpenTaskMgr = false; openSystem(); }
        alarmTick();                                // Clock alarm + Print Screen
        deskDropTick();                             // files dropped on the page: where do they go?
#ifdef TIDYOS_HTML5
        if (gMusicNextReq) {                        // Next from the phone's lock screen / the computer's media keys
          gMusicNextReq = false;
          std::string m = musicQuickNext();
          if (m.rfind("Music: ", 0) == 0) { deskNote("Playing", m.substr(7)); repaint(deskPanelRect()); }
        }
        if (m_tick % 10 == 0) musicQuickTick();     // the panel's MP3: on to the next song when one ends
#endif
#ifdef TIDYOS_HTML5                                 // (the monitor edition has no IRAM to spare for sleep code)
        if (gSleepReq) {
          gSleepReq = false;
          Serial.println("[TidyOS] deep sleep - press BOOT (or EN) to wake");
          ledSetOn(false);
          delay(400);                               // let the page get the last frame
          WiFi.disconnect(true);
#ifdef TIDYOS_V4
          rf::powerDown();                          // radio, its amplifier, GPS and the OLED's power off too
          rtc_gpio_pullup_en(GPIO_NUM_0);           // PRG reads high until pressed
#endif
          esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);   // the BOOT (PRG) button wakes it
          esp_deep_sleep_start();
        }
#endif
        if (gRestartReq) {
          gRestartReq = false;
          Serial.println("[TidyOS] restart asked for from the browser");
          s_waitViewer = 0xB0B0;
          delay(300);
          ESP.restart();
        }
        if (!s_bootNote.empty() && (m_tick > 20 || screenViewer())) {     // show it once, when someone can see it
          std::string n = s_bootNote; s_bootNote.clear();
          gApp->messageBox("Update", n.c_str(), "OK", nullptr, nullptr, uiMessageBoxIcon::Info);
        }
        {                                           // the clock: repaint as soon as the second changes, so it blinks evenly
          time_t ns = time(nullptr);
          if (ns != m_lastSec) { m_lastSec = ns; repaint(Rect(imax(0, s_clockX - 4), SCREEN_H - BAR_H, SCREEN_W - 1, SCREEN_H - 1)); }   // the seconds tick every second
        }
#ifdef TIDYOS_PRO
        if (s_newsCount && !showingHint() && !tickerCovered()) { newsTickerStep(canvas()); repaint(newsTickerRect()); }   // rolling headlines, every 100 ms
        // (not while a window is over it: drawing it unseen 10 times a second took drawing time and browser
        // bandwidth from the window in front, a game most of all)
#endif
        if (++m_tick % 10) break;                   // the rest once a second (timer: 100 ms)
        netTick();
        gHeapSnap = ramFree(); gMinHeapSnap = ramLowest();
        if (m_tick % 300 == 0) Serial.printf("[TidyOS] still running: up %us, free RAM %uK\n", (unsigned)(millis() / 1000), (unsigned)(gHeapSnap / 1024));
        if (m_tick % 50 == 0) repaint(Rect(0, SCREEN_H - BAR_H, SCREEN_W - 1, SCREEN_H - 1));   // the whole bar every 5 s
        {
          // refresh the welcome / pairing hint only when something changed
          std::string sig = inputStatus() + char('0' + inputKeyboards()) + char('0' + inputMice()) + (gSet.ssid.length() ? "w" : "-") + (netConnected() ? "c" : "-");
          int unread = msgUnreadTotal() + comsUnread();
          if (unread != m_unread) { m_unread = unread; for (int k = 0; k < NICONS; ++k) if (ICONS[k].open == openMessages) repaintIcon(k); }
          if (sig != m_hintSig) {
            m_hintSig = sig;
            repaint(Rect(CELL_W * COLS + 16, 0, SCREEN_W - 1, SCREEN_H - BAR_H - 1));   // hint or panel
            m_panelSig = deskPanelSig();
          } else if (!showingHint() && deskPanelSig() != m_panelSig) {
            m_panelSig = deskPanelSig();
            repaint(deskPanelRect());
          }
        }
        break;
      default:
        break;
    }
  }

private:
  int m_pressed = -1;
  int m_unread = 0;
  int m_tick = 0;
  time_t m_lastSec = 0;
  std::string m_hintSig;

  Rect iconRect(int i) {
    if (NICONS > 24) { CELL_H = 57; Y0 = 4; } else { CELL_H = 72; Y0 = 6; }
    int col = i % COLS, row = i / COLS;
    int x = X0 + col * CELL_W, y = Y0 + row * CELL_H;
    return Rect(x, y, x + CELL_W - 1, y + CELL_H - 1);
  }
  int hit(int x, int y) {
    for (int i = 0; i < NICONS; ++i) {
      Rect r = iconRect(i);
      if (x >= r.X1 && x <= r.X2 && y >= r.Y1 && y <= r.Y2) return i;
    }
    return -1;
  }
  void repaintIcon(int i) { repaint(iconRect(i)); }

  void drawIcon(Canvas * cv, int i) {
    Rect r = iconRect(i);
    int ix = r.X1 + (CELL_W - 36) / 2, iy = r.Y1 + 4;
    bool down = (i == m_pressed);
    if (down) {
      cv->setBrushColor(T.shadow);
      cv->fillRectangle(r.X1 + 4, r.Y1, r.X2 - 4, r.Y2 - 6);
    }
    const DeskIcon & ic = ICONS[i];
    if (ic.art) {
      for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
          char ch = ic.art[y][x];
          if (ch == '.') continue;
          cv->setBrushColor(ch == '#' ? T.text : ch == 'a' ? T.accent : ch == 'g' ? T.face : T.win);
          cv->fillRectangle(ix + 2 + x * 2, iy + 2 + y * 2, ix + 3 + x * 2, iy + 3 + y * 2);
        }
    } else {
      cv->setBrushColor(T.text);
      cv->fillRectangle(ix + 2, iy + 2, ix + 33, iy + 33);
      cv->setBrushColor(!strcmp(ic.glyph, "$") ? T.good : T.accent);
      cv->fillRectangle(ix + 4, iy + 4, ix + 31, iy + 31);
      auto f = strlen(ic.glyph) > 1 ? &FONT_std_14 : &FONT_std_24;
      int w = cv->textExtent(f, ic.glyph);
      cv->setPenColor(T.accentText);
      cv->setBrushColor(!strcmp(ic.glyph, "$") ? T.good : T.accent);
      cv->drawText(f, ix + 18 - w / 2, iy + 18 - f->height / 2, ic.glyph);
    }
    if (ic.open == openMessages) {
      int unread = msgUnreadTotal() + comsUnread();
      if (unread) {
        char nb[8];
        snprintf(nb, sizeof(nb), "%d", unread > 99 ? 99 : unread);
        cv->setBrushColor(T.warn == T.text ? T.accent : T.warn);
        cv->fillEllipse(ix + 32, iy + 4, 18, 18);
        cv->setPenColor(T.accentText);
        int nw = cv->textExtent(&FONT_std_12, nb);
        cv->drawText(&FONT_std_12, ix + 32 - nw / 2, iy - 2, nb);
      }
    }
    const fabgl::FontInfo * lf = &FONT_std_14;
    int w = cv->textExtent(lf, ic.label);
    if (w > CELL_W - 4) { lf = &FONT_std_12; w = cv->textExtent(lf, ic.label); }   // long names: smaller text
    cv->setPenColor(T.deskText);
    cv->setBrushColor(down ? T.shadow : T.desk);
    cv->drawText(lf, r.X1 + CELL_W / 2 - w / 2, iy + 40, ic.label);
  }

  bool showingHint() {
#ifdef TIDYOS_HTML5
    return false;            // the panel always shows; without Wi-Fi it says what needs Wi-Fi
#else
    return inputKeyboards() == 0 || inputMice() == 0 || !gSet.ssid.length();
#endif
  }
  int m_panelSig = -1;
  int m_pendingOpen = -1;          // icon clicked, opening shortly
  uint32_t m_pendingAt = 0;

  // where the minimized windows' buttons go: after the RAM text, before the clock
  void taskSlots(Canvas * cv, const char * ramText, int & x1, int & x2, std::vector<Rect> & out) {
    int by = SCREEN_H - BAR_H;
    x1 = s_ramX + cv->textExtent(&FONT_std_12, ramText) + 10;
    x2 = (s_clockX > 0 ? s_clockX : SCREEN_W - cv->textExtent(&FONT_std_14, "12:00 PM")) - 16;
    int n = s_minimized.size();
    if (!n || x2 - x1 < 30) return;
    int w = imin(80, (x2 - x1) / n);
    for (int i = 0; i < n && w >= 24; ++i) out.push_back(Rect(x1 + i * w, by + 3, x1 + i * w + w - 3, SCREEN_H - 3));
  }
  std::string ramText() {
    char b[96];
    if (s_optUntil && (int32_t)(millis() - s_optUntil) < 0) return s_optMsg;
    std::string notice = msgLastNotice();
    if (!notice.empty()) snprintf(b, sizeof(b), "%s", notice.substr(0, 40).c_str());
#ifdef TIDYOS_HTML5
    else {                                                                    // (you're looking at it in the browser)
      // RAM figures every 20 s (they jump about); free storage every 5 s (the bar is redrawn every 5 s)
      static size_t ps = 0, in = 0; static uint32_t ramAt = 0;
      if (!ramAt || millis() - ramAt >= 20000) { ramAt = millis() | 1; ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM); in = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
      // free storage: the SD card when it's on, otherwise TidyOS's own flash area
      static char st[28] = ""; static uint32_t stAt = 0;
      if (gStorageChanged) { gStorageChanged = false; stAt = 0; st[0] = 0; }
      if (!st[0] || millis() - stAt >= 4900) {
        stAt = millis();
        int64_t fb = 0; bool onSd = false;
        auto human = [](int64_t n, char * o, size_t m) {
          if (n >= (1LL << 30)) snprintf(o, m, "%u.%uG", (unsigned)(n >> 30), (unsigned)((n % (1LL << 30)) * 10 >> 30));
          else if (n >= (1LL << 20)) snprintf(o, m, "%u.%02uM", (unsigned)(n >> 20), (unsigned)((n % (1LL << 20)) * 100 >> 20));   // (to 10 KB: a deleted photo shows)
          else snprintf(o, m, "%uK", (unsigned)(n >> 10));
        };
        char h[12];
        if (storageFree(fb, onSd)) { human(fb, h, sizeof(h)); snprintf(st, sizeof(st), onSd ? ", SD %s" : ", flash %s", h); }
        else st[0] = 0;
      }
      if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM)) snprintf(b, sizeof(b), "RAM %uK + PSRAM %u.%uM%s free", (unsigned)(in / 1024), (unsigned)(ps / 1048576), (unsigned)(ps % 1048576 * 10 / 1048576), st);
      else snprintf(b, sizeof(b), "RAM %uK%s free", (unsigned)(in / 1024), st);
    }
#else
    else snprintf(b, sizeof(b), "Keyboard: %s  Mouse: %s   RAM %uK", inputKeyboards() ? "on" : "--", inputMice() ? "on" : "--", (unsigned)(ramFree() / 1024));
#endif
    return b;
  }

  void paint(Rect const & clip) {
    auto cv = canvas();
#ifdef TIDYOS_PRO
    { Rect k = newsTickerRect();                    // just the news line moving on: draw only that
      if (!showingHint() && clip.X1 >= k.X1 && clip.X2 <= k.X2 && clip.Y1 >= k.Y1 && clip.Y2 <= k.Y2) { drawNewsTicker(cv); return; } }
#endif
    cv->setBrushColor(T.desk);
    {
      // the desktop colour everywhere in the clip except under the panel: the panel paints all of its own area,
      // and filling it first left the map a plain box for a moment, which the browser could catch (every 20 s)
      Rect f(clip.X1, clip.Y1, clip.X2, imin(clip.Y2, SCREEN_H - BAR_H - 1)), p = deskPanelRect();
      if (showingHint() || !f.intersects(p)) cv->fillRectangle(f.X1, f.Y1, f.X2, f.Y2);
      else {
        if (f.Y1 < p.Y1) cv->fillRectangle(f.X1, f.Y1, f.X2, p.Y1 - 1);                    // above
        if (f.Y2 > p.Y2) cv->fillRectangle(f.X1, p.Y2 + 1, f.X2, f.Y2);                    // below
        int y1 = imax(f.Y1, p.Y1), y2 = imin(f.Y2, p.Y2);
        if (f.X1 < p.X1) cv->fillRectangle(f.X1, y1, p.X1 - 1, y2);                         // left
        if (f.X2 > p.X2) cv->fillRectangle(p.X2 + 1, y1, f.X2, y2);                         // right
      }
    }
    for (int i = 0; i < NICONS; ++i) {
      Rect r = iconRect(i);
      if (r.X2 >= clip.X1 && r.X1 <= clip.X2 && r.Y2 >= clip.Y1 && r.Y1 <= clip.Y2)
        drawIcon(cv, i);
    }
    // welcome / pairing hint - or, once everything is set up, the panel on the right
    if (!showingHint()) { Rect p = deskPanelRect(); if (p.intersects(clip)) drawDeskPanel(cv); }
#ifdef TIDYOS_HTML5
    if (false) {
      int x = CELL_W * COLS + 24, y = 150;
      cv->setPenColor(T.deskText);
      cv->setBrushColor(T.desk);
      cv->drawText(&FONT_std_22, x, y, "Welcome to TidyOS");
      y += 34;
      cv->drawText(&FONT_std_14, x, y, "This desktop runs on the ESP32 and");
      y += 20;
      cv->drawText(&FONT_std_14, x, y, "shows up in your web browser.");
      y += 28;
      cv->drawText(&FONT_std_14, x, y, gSet.ssid.length() ? "Joining your Wi-Fi..." : "Open Settings to join your home Wi-Fi.");
      y += 20;
      cv->drawText(&FONT_std_14, x, y, ("Then open http://" + boardHost() + ".local").c_str());
      y += 28;
      cv->setBrushColor(T.desk);
      cv->fillRectangle(x, y, SCREEN_W - 1, y + 16);
      std::string st = inputStatus();
      cv->drawText(&FONT_std_12, x, y, st.c_str());
    }
#else
    bool needKbd = inputKeyboards() == 0, needMouse = inputMice() == 0;
    if (needKbd || needMouse || !gSet.ssid.length()) {
      int x = CELL_W * COLS + 24, y = 150;
      cv->setPenColor(T.deskText);
      cv->setBrushColor(T.desk);
      cv->drawText(&FONT_std_22, x, y, "Welcome to TidyOS");
      y += 34;
      if (needKbd)
        cv->drawText(&FONT_std_14, x, y, "1. Put your keyboard in pairing mode.");
      else
        cv->drawText(&FONT_std_14, x, y, "1. Keyboard connected!");
      y += 20;
      if (needMouse)
        cv->drawText(&FONT_std_14, x, y, "2. Then put your mouse in pairing mode.");
      else
        cv->drawText(&FONT_std_14, x, y, "2. Mouse connected!");
      y += 20;
      cv->drawText(&FONT_std_14, x, y, gSet.ssid.length() ? "3. Wi-Fi is set up." : "3. Open Settings to join your Wi-Fi.");
      y += 28;
      std::string st = inputStatus();
      cv->setBrushColor(T.desk);
      cv->fillRectangle(x, y, SCREEN_W - 1, y + 16);
      cv->drawText(&FONT_std_14, x, y, st.c_str());
      y += 24;
      if (needKbd && needMouse)
        cv->drawText(&FONT_std_12, x, y, "No Bluetooth? Type in a serial monitor (115200).");
    }
#endif
    // status bar
    int by = SCREEN_H - BAR_H;
    if (clip.Y2 >= by) {
      cv->setBrushColor(T.face);
      cv->fillRectangle(0, by, SCREEN_W - 1, SCREEN_H - 1);
      cv->setPenColor(T.shadow);
      cv->drawLine(0, by, SCREEN_W - 1, by);
      cv->setPenColor(T.text);
      cv->setBrushColor(T.face);
      // groups, each centred on the bar, with a thin divider between them
      auto ty = [&](const fabgl::FontInfo * f) { return by + 1 + (BAR_H - 1 - (int)f->height) / 2; };
      auto divider = [&](int x) { cv->setPenColor(T.shadow); cv->drawLine(x, by + 4, x, SCREEN_H - 4); cv->setPenColor(T.text); };
      int x = 4;
      {                                                   // the logo: "TidyOS" and a blue PRO badge in a rounded box
        const fabgl::FontInfo * fT = &FONT_std_14, * fP = &FONT_std_14;   // PRO the same size as TidyOS
        int wT = cv->textExtent(fT, "TidyOS"), wP = cv->textExtent(fP, "PRO");
        // a snug box: 3 px each side; PRO right after "TidyOS", its blue badge just around the letters
        int x0 = x, y0 = by + 2, y1 = SCREEN_H - 3;
        int tx = x0 + 3, px1 = tx + wT, px2 = px1 + wP, x1 = px2 + 2;   // PRO touches "TidyOS"
        cv->setPenColor(T.text);
        cv->drawLine(x0 + 2, y0, x1 - 2, y0); cv->drawLine(x0 + 2, y1, x1 - 2, y1);
        cv->drawLine(x0, y0 + 2, x0, y1 - 2); cv->drawLine(x1, y0 + 2, x1, y1 - 2);
        cv->setPixel(x0 + 1, y0 + 1); cv->setPixel(x1 - 1, y0 + 1); cv->setPixel(x0 + 1, y1 - 1); cv->setPixel(x1 - 1, y1 - 1);
        cv->drawText(fT, tx, ty(fT), "TidyOS");
        int py1 = y0 + 1, py2 = y1 - 1;                  // the badge fills the box's height (room for the 14 px PRO)
        cv->setBrushColor(T.accent);
        cv->fillRectangle(px1, py1, px2, py2);
        cv->setPenColor(T.face);                          // the badge: straight left edge at the P, rounded on the right
        cv->setPixel(px2, py1); cv->setPixel(px2, py2);
        cv->setPenColor(T.accentText);
        cv->drawText(fP, px1, py1 + (py2 - py1 + 1 - (int)fP->height) / 2, "PRO");
        cv->setPenColor(T.text); cv->setBrushColor(T.face);
        x = x1 + 8;
      }
      cv->drawText(&FONT_std_12, x, ty(&FONT_std_12), "v" TIDYOS_VERSION);
      x += cv->textExtent(&FONT_std_12, "v" TIDYOS_VERSION) + 8;
      std::string bid = "#" + boardId().substr(boardId().size() - 4);    // this board: the last 4 of its ID
      cv->setPenColor(T.shadow == T.face ? T.text : T.shadow);
      cv->drawText(&FONT_std_12, x, ty(&FONT_std_12), bid.c_str());
      cv->setPenColor(T.text);
      x += cv->textExtent(&FONT_std_12, bid.c_str()) + 8;
      divider(x); x += 8;
      char b[96];
      std::string ns = netStatus();
      if (fileServerOn()) ns += " - sharing";
      snprintf(b, sizeof(b), "%s", ns.c_str());
      cv->drawText(&FONT_std_12, x, ty(&FONT_std_12), b);
      x += cv->textExtent(&FONT_std_12, b) + 8;
      divider(x); x += 8;
      s_optBtn = Rect(0, 0, 0, 0);                        // (no Optimize button on the bar any more)
      s_ramX = x;
      strlcpy(b, ramText().c_str(), sizeof(b));
      cv->drawText(&FONT_std_12, x, ty(&FONT_std_12), b);
      s_ramEnd = x + cv->textExtent(&FONT_std_12, b);
      // minimized windows
      {
        int x1, x2; std::vector<Rect> slots;
        taskSlots(cv, b, x1, x2, slots);
        for (size_t i = 0; i < slots.size(); ++i) {
          Rect s = slots[i];
          cv->setBrushColor(T.win);
          cv->fillRectangle(s.X1, s.Y1, s.X2, s.Y2);
          cv->setPenColor(T.text);
          cv->drawRectangle(s.X1, s.Y1, s.X2, s.Y2);
          char t[40];
          strlcpy(t, s_minimized[i]->title() ? s_minimized[i]->title() : "?", sizeof(t));
          while (strlen(t) > 1 && cv->textExtent(&FONT_std_12, t) > s.width() - 6) t[strlen(t) - 1] = 0;
          cv->setBrushColor(T.win);
          cv->drawText(&FONT_std_12, s.X1 + 3, s.Y1 + 1, t);
        }
        cv->setBrushColor(T.face);
      }
      time_t now = time(nullptr);
      struct tm tmv;
      localtime_r(&now, &tmv);
      // the clock: with seconds when the bar has room for them
      if (tmv.tm_year > 100) {
        const fabgl::FontInfo * f = &FONT_std_14;
        int h = tmv.tm_hour % 12; if (h == 0) h = 12;
        char hs[4], ms[4], ss[4], ap[4];
        snprintf(hs, sizeof(hs), "%d", h); snprintf(ms, sizeof(ms), "%02d", tmv.tm_min);
        snprintf(ss, sizeof(ss), "%02d", tmv.tm_sec); snprintf(ap, sizeof(ap), " %s", tmv.tm_hour < 12 ? "AM" : "PM");
        int cw = cv->textExtent(f, ":"), dw = cv->textExtent(f, "00");
        int left = s_ramEnd > 0 ? s_ramEnd + 10 : SCREEN_W / 2;
        int wSec = cv->textExtent(f, "12") + cw + dw + cw + dw + cv->textExtent(f, " PM");
        // the battery, just left of the clock (on USB with no battery: nothing)
        int bp = batteryPct();
        char bt[8]; snprintf(bt, sizeof(bt), "%d%%", bp < 0 ? 0 : bp);
        int bw = bp >= 0 ? 20 + cv->textExtent(&FONT_std_12, bt) + 10 : 0;
        bool secs = SCREEN_W - 8 - wSec - bw >= left + (int)s_minimized.size() * 30;
        int w = cv->textExtent(f, hs) + cw + dw + (secs ? cw + dw : 0) + cv->textExtent(f, ap);
        int cx = SCREEN_W - w - 8, cy = by + 3;
        s_clockX = cx - bw;
        if (bp >= 0) {
          int bx = cx - bw, iy = by + (BAR_H - 10) / 2;
          cv->setPenColor(T.text);
          cv->drawRectangle(bx, iy, bx + 15, iy + 9);                  // the body
          cv->setBrushColor(T.text);
          cv->fillRectangle(bx + 16, iy + 3, bx + 17, iy + 6);         // the nub
          cv->setBrushColor(bp <= 15 ? RGB888(220, 60, 50) : T.text);
          int fw = (bp * 14 + 50) / 100;
          if (fw > 0) cv->fillRectangle(bx + 1, iy + 1, bx + fw, iy + 8);
          cv->setBrushColor(T.face);
          cv->drawText(&FONT_std_12, bx + 21, by + 4, bt);
        }
        bool on = true;                                  // steady colon (no blinking); the seconds tick every second
        cv->drawText(f, cx, cy, hs); cx += cv->textExtent(f, hs);
        if (on) cv->drawText(f, cx, cy, ":"); cx += cw;
        cv->drawText(f, cx, cy, ms); cx += dw;
        if (secs) { if (on) cv->drawText(f, cx, cy, ":"); cx += cw; cv->drawText(f, cx, cy, ss); cx += dw; }
        cv->drawText(f, cx, cy, ap);
      } else {
        snprintf(b, sizeof(b), "--:--");
        int w = cv->textExtent(&FONT_std_14, b);
        cv->drawText(&FONT_std_14, SCREEN_W - w - 8, by + 3, b);
      }
    }
  }
};

// ====================================================================== application

class TidyApp : public uiApp {
  void init() override {
    gApp = this;
    rootWindow()->frameStyle().backgroundColor = T.desk;
    auto desk = new Desktop(rootWindow());
    s_deskCtl = desk;
    setTimer(desk, 100);
    displayController()->setMouseCursor(CursorName::CursorPointerSimpleReduced);
    inputAttach(this, &s_keyboard, s_dc);
  }
};
static TidyApp s_app;

// ====================================================================== start-up

template <class C>
static BitmappedDisplayController * tryMode(size_t fbBytes, size_t margin = 60000)
{
  size_t freeMem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#ifdef TIDYOS_V4
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 200000) { freeMem = heap_caps_get_free_size(MALLOC_CAP_SPIRAM); margin = 0; }   // screen goes in PSRAM
#endif
  Serial.printf("[TidyOS] free %u, need %u\n", (unsigned)freeMem, (unsigned)(fbBytes + margin));
  if (freeMem < fbBytes + margin) return nullptr;
  C * c = new C;
#ifdef TIDYOS_HTML5
  c->setHeadless(true);     // no monitor: keep the picture in RAM, skip the VGA signal (~16 KB + CPU time)
#endif
  c->begin(VGA_RED1, VGA_RED0, VGA_GREEN1, VGA_GREEN0, VGA_BLUE1, VGA_BLUE0, VGA_HSYNC, VGA_VSYNC);
  c->setResolution(VGA_640x480_60Hz);
  if (c->getViewPortHeight() >= SCREEN_H && c->getViewPortWidth() >= SCREEN_W) return c;
  Serial.printf("[TidyOS] only got %d lines, trying fewer colours\n", c->getViewPortHeight());
  c->end();
  delete c;
  return nullptr;
}

namespace fabgl { extern bool fabglNoMouseSprite; }
static void startDisplay()
{
#ifdef TIDYOS_HTML5
  fabgl::fabglNoMouseSprite = true;   // the browser shows its own mouse pointer: none drawn into the picture
#endif
  // FabGL queues drawing commands; its default 1024-entry queue is ~15 KB of RAM. 256 is plenty
  // (when it fills, the app just waits a moment for drawing to catch up).
  BitmappedDisplayController::queueSize = 256;
  int want = gSet.colors;   // 0 = auto
#ifdef TIDYOS_V3
  if (want == 0) want = 2;  // V3 (no PSRAM): black & white unless asked - the 4-colour screen leaves too little for the hotspot's web page
#endif
#ifdef TIDYOS_HTML5
#ifdef TIDYOS_V4
  if (want == 0) want = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 200000 ? 8 : 4;   // V4: PSRAM holds the screen, so 8 colours cost nothing
#endif
  if (want == 0) want = 4;  // HTML5: 4 colours unless asked for 8 - leaves ~38 KB more for Wi-Fi and apps
#endif
  BitmappedDisplayController * dc = nullptr;
  int got = 0;
  // 8 colours only when it still leaves plenty for Wi-Fi and apps (asked for explicitly = smaller margin)
  if (want == 2) { dc = tryMode<V2>(640 * 480 / 8, 0); got = 2; }      // black & white: half the screen memory
  if (!dc && (want == 0 || want >= 8))  { dc = tryMode<V8>(640 * 480 * 3 / 8, want >= 8 ? 60000 : 100000); got = 8; }
  if (!dc)                              { dc = tryMode<V4>(640 * 480 / 4);     got = 4; }
  if (!dc) {
    // last resort: 4 colours without the safety margin
    auto c = new V4;
#ifdef TIDYOS_HTML5
    c->setHeadless(true);
#endif
    c->begin(VGA_RED1, VGA_RED0, VGA_GREEN1, VGA_GREEN0, VGA_BLUE1, VGA_BLUE0, VGA_HSYNC, VGA_VSYNC);
    c->setResolution(VGA_640x480_60Hz);
    dc = c; got = 4;
  }
  s_dc = dc;
  themeSetup(dc, got);
  Serial.printf("[TidyOS] display 640x480, %d colours, free heap %u\n", got, (unsigned)ramFree());
}

const char * gSDWiring = "none";

// ---- battery (the same reading as The Shelf): VBAT on GPIO 1 through a 390k / 100k divider, switched on
// by ADC_CTRL (GPIO 37) on the V4. (The V4-R8's GPIO 37 belongs to its PSRAM: just read GPIO 1.)
static volatile int s_batPct = -1;           // -1 = no battery (on USB)
static float s_battV = -1;
int batteryPct() { return s_batPct; }
void batteryRead()                           // every ~10 s (the OLED task)
{
#if defined(TIDYOS_XIAO)
  s_batPct = -1;                             // the XIAO has no battery divider on a pin
#elif defined(TIDYOS_V4)
  uint32_t mv = 0;
#if defined(TIDYOS_V3)
  // V3.0/3.1 connect the divider with ADC_CTRL LOW, V3.2 with HIGH (as The Shelf does it): read both, keep the higher
  for (int lvl = 0; lvl < 2; lvl++) {
    pinMode(PIN_ADC_CTRL, OUTPUT); digitalWrite(PIN_ADC_CTRL, lvl ? HIGH : LOW); delay(5);
    uint32_t s = 0;
    for (int i = 0; i < 8; i++) s += analogReadMilliVolts(PIN_VBAT);
    if (s > mv) mv = s;
  }
  pinMode(PIN_ADC_CTRL, INPUT);
#else
#if !CONFIG_SPIRAM_MODE_OCT
  pinMode(PIN_ADC_CTRL, OUTPUT); digitalWrite(PIN_ADC_CTRL, HIGH); delay(5);
#endif
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_VBAT);
#if !CONFIG_SPIRAM_MODE_OCT
  digitalWrite(PIN_ADC_CTRL, LOW);
#endif
#endif
  float v = (mv / 8.0f) * 4.9f * 1.045f / 1000.0f;
  s_battV = (s_battV < 0 || fabsf(v - s_battV) > 0.5f) ? v : s_battV * 0.7f + v * 0.3f;
  int pct = -1;
  if (s_battV >= 2.8f && s_battV <= 4.45f) {
    static const float V[] = { 4.20f, 4.10f, 4.00f, 3.90f, 3.80f, 3.70f, 3.60f, 3.50f, 3.40f, 3.30f };
    static const int P[] = { 100, 90, 80, 65, 50, 35, 20, 10, 5, 0 };
    pct = 0;
    if (s_battV >= V[0]) pct = 100;
    else for (int i = 1; i < 10; i++) if (s_battV >= V[i]) { pct = P[i] + (int)((s_battV - V[i]) / (V[i - 1] - V[i]) * (P[i - 1] - P[i])); break; }
  }
  s_batPct = pct;
#endif
}
bool gpsSwitchOn() { return gSet.gpsOn; }

// Same method as The Shelf on an ESP32 (Arduino SD library, tried at 10, 4 and 1 MHz):
// it reads cards that ESP-IDF's own SD-over-SPI driver rejects.
#if defined(TIDYOS_XIAO)
SPIClass gSharedSpi(FSPI);          // XIAO: the card and the LoRa radio share D8 / D9 / D10 (each has its own chip select)
#define s_sdSpi gSharedSpi
#define SD_SPI_END()                // (never stopped: the radio uses it too)
#elif defined(TIDYOS_V4)
static SPIClass s_sdSpi(FSPI);      // the LoRa radio has HSPI
#else
static SPIClass s_sdSpi(VSPI);
#endif
#ifndef SD_SPI_END
#define SD_SPI_END() s_sdSpi.end()
#endif

static bool mountSD(int miso, int mosi, int clk, int cs, const char * name)
{
  s_sdSpi.begin(clk, miso, mosi, cs);
  for (int pin : { miso, mosi, cs }) gpio_pullup_en((gpio_num_t)pin);      // SD lines need pull-ups
  for (uint32_t hz : { 10000000u, 4000000u, 1000000u }) {
    if (SD.begin(cs, s_sdSpi, hz, SD_MOUNT, 2)) {     // 2 open files at once: each one reserves a 4 KB buffer up front
      gSDWiring = name;
      Serial.printf("[TidyOS] SD card found on %s wiring at %u MHz (%llu MB)\n", name, (unsigned)(hz / 1000000), SD.cardSize() >> 20);
      return true;
    }
    SD.end();
  }
  SD_SPI_END();
  Serial.printf("[TidyOS] no SD card on %s wiring\n", name);
  return false;
}

bool gnssAlreadyFound();   // rf_engine.cpp: a GPS is already running on 39-42

static bool mountAnySD()
{
#if defined(TIDYOS_XIAO)
  pinMode(LORA_NSS, OUTPUT); digitalWrite(LORA_NSS, HIGH);    // the radio keeps off the shared bus while the card is looked for
  return mountSD(SD_MISO, SD_MOSI, SD_CLK, SD_CS, "XIAO (CS D7, shares D8 / D9 / D10 with the radio)");
#elif defined(TIDYOS_V3)
  // V3: no GNSS header, so The Shelf's V3 in-line layout comes first, then the six-wire 4/6/47/48 layout
  if (mountSD(SD_INLINE_MISO, SD_INLINE_MOSI, SD_INLINE_CLK, SD_INLINE_CS, "Shelf in-line (CS 42, MOSI 41, CLK 40, MISO 39)")) return true;
  if (mountSD(SD_MISO, SD_MOSI, SD_CLK, SD_CS, "V3 six-wire (CS 4, MISO 6, CLK 47, MOSI 48)")) return true;
  return mountSD(42, 40, 41, 39, "in-line, module turned round (CS 39, MOSI 40, CLK 41, MISO 42)");
#elif defined(TIDYOS_V4)
  if (mountSD(SD_MISO, SD_MOSI, SD_CLK, SD_CS, "V4 (CS 4, MISO 6, CLK 47, MOSI 48)")) return true;
  if (gnssAlreadyFound()) return false;     // 39-42 belong to a GPS that is already running
  // (the card is tried before looking for a GPS, like The Shelf does: probing for a GPS pokes 40 and 42)
  if (mountSD(SD_INLINE_MISO, SD_INLINE_MOSI, SD_INLINE_CLK, SD_INLINE_CS, "Shelf in-line (CS 42, MOSI 41, CLK 40, MISO 39)")) return true;
  // the in-line module the other way round (easy to do when it's fitted from the bottom of the board, and
  // often soldered before anyone notices): the four pins are ordinary GPIOs, so they work reversed too
  return mountSD(42, 40, 41, 39, "in-line, module turned round (CS 39, MOSI 40, CLK 41, MISO 42)");
#elif defined(TIDYOS_HTML5)
  return mountSD(SD_INLINE_MISO, SD_INLINE_MOSI, SD_INLINE_CLK, SD_INLINE_CS, "in-line (19/18/5/17)") ||
         mountSD(SD_MISO, SD_MOSI, SD_CLK, SD_CS, "classic (16/17/14/13)");
#else
  return mountSD(SD_MISO, SD_MOSI, SD_CLK, SD_CS, "16/17/14/13");
#endif
}

static void makeDirs();

bool sdMountForUpdate()
{
  if (!gSDMounted) gSDMounted = mountAnySD();
  return gSDMounted;
}

bool sdTurnOn()
{
  gSet.sdOn = true;
  settingsSave();
  if (gSDMounted) return true;
  gSDMounted = mountAnySD();
  if (gSDMounted) makeDirs();
  return gSDMounted;
}

void sdTurnOff()
{
  gSet.sdOn = false;
  settingsSave();
  if (gSDMounted) {
    SD.end();
    SD_SPI_END();
    gSDMounted = false;
    gSDWiring = "none";
    Serial.println("[TidyOS] SD card turned off");
  }
  if (!gFlashMounted) gFlashMounted = mountInternal();
}

#ifdef TIDYOS_HTML5
// The screen memory keeps working (the browser reads it); only the pins stop being VGA outputs.
static void releaseVgaPins()
{
#ifdef TIDYOS_V4
  return;
#endif
  for (gpio_num_t p : { VGA_RED1, VGA_RED0, VGA_GREEN1, VGA_GREEN0, VGA_BLUE1, VGA_BLUE0, VGA_HSYNC, VGA_VSYNC })
    gpio_reset_pin(p);
}
#endif

static void makeDirs()
{
  if (gSDMounted) {
    mkdir(SD_MOUNT "/tidyos", 0777);
    mkdir(SD_MOUNT "/Documents", 0777);
  }
}

// ---------------------------------------------------------------- boot logo
// big text: draws each font pixel as a scale x scale square (runs merged per row)
static void drawBig(Canvas & cv, FontInfo const * f, int x, int y, const char * t, int scale, RGB888 col)
{
  cv.setBrushColor(col);
  for (; *t; ++t) {
    uint8_t c = *t;
    int gw;
    const uint8_t * g;
    if (f->flags & FONTINFOFLAGS_VARWIDTH) { g = f->data + f->chptr[c]; gw = *g++; }
    else { gw = f->width; g = f->data + c * f->height * ((gw + 7) / 8); }
    int bpr = (gw + 7) / 8;
    for (int r = 0; r < f->height; ++r) {
      const uint8_t * row = g + r * bpr;
      int run = -1;
      for (int i = 0; i <= gw; ++i) {
        bool on = i < gw && (row[i >> 3] & (0x80 >> (i & 7)));
        if (on && run < 0) run = i;
        if (!on && run >= 0) {
          cv.fillRectangle(x + run * scale, y + r * scale, x + i * scale - 1, y + (r + 1) * scale - 1);
          run = -1;
        }
      }
    }
    x += gw * scale;
  }
}

static int bigWidth(FontInfo const * f, const char * t, int scale)
{
  int w = 0;
  for (; *t; ++t) w += (f->flags & FONTINFOFLAGS_VARWIDTH) ? f->data[f->chptr[(uint8_t)*t]] : f->width;
  return w * scale;
}

// one block of the logo "T" (same style as the Blocks game)
static void logoBlock(Canvas & cv, int x, int y, int s, RGB888 fill)
{
  cv.setBrushColor(T.text);
  cv.fillRectangle(x, y, x + s - 1, y + s - 1);
  cv.setBrushColor(fill);
  cv.fillRectangle(x + 3, y + 3, x + s - 4, y + s - 4);
  cv.setBrushColor(T.win);
  cv.fillRectangle(x + 3, y + 3, x + s - 4, y + 6);
  cv.fillRectangle(x + 3, y + 3, x + 6, y + s - 4);
}

static const int BAR_X = 170, BAR_Y = 300, BAR_W = 300, BAR_H = 14;

static void splashLogo()
{
  Canvas cv(s_dc);
  cv.setBrushColor(T.desk);
  cv.clear();
  // the TidyOS Pro logo: the five-block "T", now a mast sending out radio waves, and a PRO badge
  int bs = 36, lx = 120, ly = 104;
  logoBlock(cv, lx,          ly,      bs, T.face);
  logoBlock(cv, lx + bs,     ly,      bs, T.accent);      // the top of the mast
  logoBlock(cv, lx + bs * 2, ly,      bs, T.face);
  logoBlock(cv, lx + bs,     ly + bs, bs, T.face);
  logoBlock(cv, lx + bs,     ly + bs * 2, bs, T.face);
  // radio waves off the top block, both sides
  cv.setPenColor(T.win);
  int cxw = lx + bs + bs / 2, cyw = ly + bs / 2;
  for (int r = 1; r <= 3; ++r) {
    int rr = bs / 2 + 8 + r * 10;
    for (int a = -35; a <= 35; a += 3) {           // little arcs, drawn as dots so any colour depth works
      float ra = a * 0.0174533f;
      int dx = (int)(rr * cosf(ra)), dy = (int)(rr * sinf(ra));
      cv.setPixel(cxw + dx + bs, cyw + dy);  cv.setPixel(cxw + dx + bs + 1, cyw + dy);
      cv.setPixel(cxw - dx - bs, cyw + dy);  cv.setPixel(cxw - dx - bs - 1, cyw + dy);
    }
  }
  // wordmark
  int tx = lx + bs * 3 + 60;
  drawBig(cv, &FONT_std_24, tx + 3, ly + 9, "TidyOS", 3, T.text);        // shadow
  drawBig(cv, &FONT_std_24, tx, ly + 6, "TidyOS", 3, T.win);
  // PRO badge
  int bx = tx + 4, by2 = ly + 80;
  cv.setBrushColor(T.accent);
  cv.fillRectangle(bx, by2, bx + 58, by2 + 24);
  cv.setPenColor(T.accentText);
  cv.setBrushColor(T.accent);
  cv.drawText(&FONT_std_22, bx + 8, by2 + 1, "PRO");
  cv.setPenColor(T.deskText);
  cv.setBrushColor(T.desk);
  cv.drawText(&FONT_std_14, bx + 68, by2 + 5, "900 MHz field computer");
  cv.drawText(&FONT_std_12, bx + 68, by2 + 24, "AMI / RF survey tools on the " TIDYOS_BOARD);
  // progress bar frame
  cv.setPenColor(T.win);
  cv.drawRectangle(BAR_X - 2, BAR_Y - 2, BAR_X + BAR_W + 1, BAR_Y + BAR_H + 1);
  cv.setPenColor(T.deskText);
  cv.drawText(&FONT_std_12, 8, 6, TIDYOS_NAME " " TIDYOS_VERSION);
  cv.waitCompletion();
}

// The progress bar keeps creeping forward on its own while a step works (joining Wi-Fi,
// connecting to GitHub...), so it never looks stuck; each splash() sets the real value.
static SemaphoreHandle_t s_barMux = nullptr;
static volatile int  s_barShown = 0, s_barCeil = 0;
static volatile bool s_barRun = false, s_barAlive = false;
static bool s_barExact = false;     // update mode: the bar shows the real download progress and its percent (no creeping, no icons)

static void drawBar(Canvas & cv, int percent)
{
  cv.setBrushColor(T.desk);
  cv.fillRectangle(BAR_X, BAR_Y, BAR_X + BAR_W - 1, BAR_Y + BAR_H - 1);
  cv.setBrushColor(T.win);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  int w = BAR_W * percent / 100;
  if (w > 0) cv.fillRectangle(BAR_X, BAR_Y, BAR_X + w - 1, BAR_Y + BAR_H - 1);
}

static void barTaskBody(void * arg)
{
  while (s_barRun) {
    vTaskDelay(pdMS_TO_TICKS(300));
    if (xSemaphoreTake(s_barMux, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    if (s_barRun && s_barShown < s_barCeil) {
      s_barShown = s_barShown + 1;
      Canvas cv(s_dc);
      drawBar(cv, s_barShown);
      cv.waitCompletion();
    }
    xSemaphoreGive(s_barMux);
  }
  s_barAlive = false;
  return;
}

// runs the task, then ends it - after the body's locals are destroyed (vTaskDelete
// inside the body would skip their destructors and leak their memory)
static void barTask(void * arg) { barTaskBody(arg); vTaskDelete(nullptr); }

static void barStart()
{
  if (!s_barMux) s_barMux = xSemaphoreCreateMutex();
  if (s_barAlive) return;
  s_barRun = s_barAlive = true;
  xTaskCreatePinnedToCore(barTask, "bar", 4096, nullptr, 1, nullptr, 1);
}

static void barStop()
{
  s_barRun = false;
  for (int i = 0; i < 20 && s_barAlive; ++i) delay(20);
}

// percent > 0: the bar moves up to it (never back) and then creeps on; 0 = stop creeping
// Like a classic Mac starting up: the icons of what's loading march in one by one along the
// bottom of the screen (wrapping to a second row), while the bar fills.
static int s_parade = 0;
static void paradeIcon(Canvas & cv, int i)
{
  const int PITCH = 36, PER_ROW = (SCREEN_W - 16) / PITCH;
  int ix = 10 + (i % PER_ROW) * PITCH, iy = SCREEN_H - 40 - (i / PER_ROW) * 36;
  const DeskIcon & ic = ICONS[i];
  if (ic.art) {
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) {
        char ch = ic.art[y][x];
        if (ch == '.') continue;
        cv.setBrushColor(ch == '#' ? T.text : ch == 'a' ? T.accent : ch == 'g' ? T.face : T.win);
        cv.fillRectangle(ix + x * 2, iy + y * 2, ix + 1 + x * 2, iy + 1 + y * 2);
      }
  } else {
    cv.setBrushColor(T.text);
    cv.fillRectangle(ix, iy, ix + 31, iy + 31);
    cv.setPenColor(T.win);
    cv.setBrushColor(T.text);
    if (ic.glyph) cv.drawText(&FONT_std_14, ix + 16 - cv.textExtent(&FONT_std_14, ic.glyph) / 2, iy + 8, ic.glyph);
  }
}

void oledBoot(const char * msg, int pct);       // oled.cpp: the same steps on the board's own screen
static void splash(const char * msg, int percent)
{
  oledBoot(msg, percent);
  if (s_barMux) xSemaphoreTake(s_barMux, portMAX_DELAY);
  Canvas cv(s_dc);
  cv.setBrushColor(T.desk);
  cv.fillRectangle(0, BAR_Y + 20, SCREEN_W - 1, BAR_Y + 40);
  // icons up to this step's share march in, and the bar slides up with them
  if (!s_barExact) {
    int target = NICONS * (percent > 100 ? 100 : percent) / 100;
    if (target <= s_parade) target = imin(NICONS, s_parade + 1);      // at least one new icon per step
    int from = s_barShown, to = percent > from ? percent : from, n = target - s_parade;
    for (int k = 0; s_parade < target; ++k) {
      paradeIcon(cv, s_parade++);
      drawBar(cv, from + (to - from) * (k + 1) / imax(1, n));
      cv.waitCompletion();
      delay(percent >= 100 ? 60 : 110);
    }
    if (to > s_barShown) s_barShown = to;
  }
  int shown = s_barShown;
  if (percent > 0) {
    if (percent > 100) percent = 100;
    if (percent > shown) shown = percent;
    s_barShown = shown;
    s_barCeil  = percent >= 100 ? 100 : (shown + 15 > 97 ? 97 : shown + 15);
  } else {
    s_barCeil = shown;
  }
  if (s_barExact) s_barCeil = shown;                  // the real figure: no creeping ahead of the download
  drawBar(cv, shown);
  if (s_barExact) {                                    // the percent, right of the bar
    char pc[8]; snprintf(pc, sizeof(pc), "%d%%", shown);
    cv.setBrushColor(T.desk);
    cv.fillRectangle(BAR_X + BAR_W + 6, BAR_Y - 2, BAR_X + BAR_W + 60, BAR_Y + BAR_H + 2);
    cv.setPenColor(T.deskText);
    cv.drawText(&FONT_std_14, BAR_X + BAR_W + 8, BAR_Y + BAR_H / 2 - 7, pc);
  }
  int tw = cv.textExtent(&FONT_std_14, msg);
  cv.setPenColor(T.deskText);
  cv.drawText(&FONT_std_14, BAR_X + BAR_W / 2 - tw / 2, BAR_Y + 22, msg);
  // the checklist: every step so far, ticked once the next one starts
  static char steps[8][48];
  static int nSteps = 0;
  if (nSteps > 0 && !strncmp(steps[nSteps - 1], msg, 12) && !strncmp(msg, "Downloading", 11))
    strlcpy(steps[nSteps - 1], msg, sizeof(steps[0]));             // the download's KB count: one line, kept up to date
  else if (strcmp(msg, "Ready") != 0 && (nSteps == 0 || strcmp(steps[(nSteps - 1) % 8], msg) != 0) && nSteps < 8)
    strlcpy(steps[nSteps++], msg, sizeof(steps[0]));
  bool allDone = percent >= 100;
  cv.setBrushColor(T.desk);
  cv.fillRectangle(0, BAR_Y + 44, SCREEN_W - 1, BAR_Y + 103);   // (the icons below stay)
  for (int i = 0; i < nSteps; ++i) {
    int x = i < 4 ? 120 : 360, y = BAR_Y + 46 + (i % 4) * 15;
    bool done = allDone || i < nSteps - 1;
    cv.setPenColor(T.deskText);
    if (done) { cv.drawLine(x, y + 6, x + 3, y + 9); cv.drawLine(x + 3, y + 9, x + 9, y + 2); cv.drawLine(x + 1, y + 6, x + 4, y + 9); }
    else cv.drawText(&FONT_std_12, x, y, "...");
    char t[48]; strlcpy(t, steps[i], sizeof(t));
    while (strlen(t) > 4 && cv.textExtent(&FONT_std_12, t) > 230) t[strlen(t) - 1] = 0;
    cv.drawText(&FONT_std_12, x + 16, y, t);
  }
  cv.waitCompletion();
  if (s_barMux) xSemaphoreGive(s_barMux);
}

RGB888 themePal(int i) { return PAL[i & 15]; }

// If memory runs out for a moment (e.g. a phone opening several connections at once),
// wait and retry instead of crashing. Gives up after 5 s of no memory at all.
#include <new>
static void lowMemoryWait()
{
  static uint32_t since = 0;
  uint32_t now = millis();
  if (!since || now - since > 6000) since = now;
  if (now - since > 5000) { since = 0; std::set_new_handler(nullptr); Serial.println("[TidyOS] out of memory"); return; }
  vTaskDelay(pdMS_TO_TICKS(25));
}

static void memLog(const char * step)
{
  Serial.printf("[TidyOS] mem after %-18s free %6u  biggest %6u  (32-bit-only extra %u)\n", step, (unsigned)ramFree(), (unsigned)ramBiggest(),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - ramFree()));
}

#ifdef TIDYOS_HTML5
// ---------------------------------------------------------------- start-up switches
// Shown in the browser before anything big starts, so memory-hungry parts can be left off
// for this boot. Boots by itself after a countdown, so a restart with nobody watching still
// comes up. Click anything (or press a key) to stop the countdown.
static const int BW_X = 130, BW_Y = 12, BW_W = 380, BW_H = 452;

static void bootRow(Canvas & cv, int i, const char * name, const char * note, bool on, const char * valText = nullptr)
{
  int y = BW_Y + 40 + i * 50;
  cv.setBrushColor(T.win);
  cv.fillRectangle(BW_X + 1, y - 4, BW_X + BW_W - 2, y + 44);
  cv.setPenColor(T.text);
  char b[48];
  snprintf(b, sizeof(b), "%d  %s", i + 1, name);
  cv.drawText(&FONT_std_16, BW_X + 16, y, b);
  cv.setPenColor(T.shadow);
  cv.drawText(&FONT_std_12, BW_X + 34, y + 21, note);
  int sx = BW_X + BW_W - 96;
  cv.setBrushColor(on ? T.accent : T.face);
  cv.fillRectangle(sx, y, sx + 76, y + 26);
  cv.setPenColor(T.shadow);
  cv.drawRectangle(sx, y, sx + 76, y + 26);
  cv.setPenColor(on ? T.accentText : T.text);
  const char * t = valText ? valText : on ? "ON" : "OFF";
  cv.drawText(&FONT_std_16, sx + 38 - cv.textExtent(&FONT_std_16, t) / 2, y + 5, t);
}

static void bootStatus(Canvas & cv, const char * msg)
{
  int y = BW_Y + BW_H - 28;
  cv.setBrushColor(T.win);
  cv.fillRectangle(BW_X + 1, y, BW_X + BW_W - 2, y + 18);
  cv.setPenColor(T.text);
  cv.drawText(&FONT_std_12, BW_X + BW_W / 2 - cv.textExtent(&FONT_std_12, msg) / 2, y + 2, msg);
}

static void bootScreenRow(Canvas & cv)
{
  int c = gSet.colors == 0 ? 4 : gSet.colors;
  bootRow(cv, 3, "Screen", c == 8 ? "8 colours: ~38 KB less free memory" : c == 2 ? "Black & white: ~37 KB more free memory" : "4 colours (restarts TidyOS to change)",
          c != 4, c == 8 ? "8 col" : c == 2 ? "B&W" : "4 col");
}

// the sixth start-up switch: the GPS search on a V4, the board's OLED screen on a V3 (no GPS header there, and
// every KB counts without PSRAM)
static void bootRow6(Canvas & cv)
{
#if defined(TIDYOS_V3) || defined(TIDYOS_XIAO)
  bootRow(cv, 5, "OLED screen", "~10 KB. Off: the board's own screen stays dark", gSet.oledOn);
#else
  bootRow(cv, 5, "GPS module", "Off: no GPS search (pins 39-42 left alone)", gSet.gpsOn);
#endif
}

static void bootDraw(Canvas & cv)
{
  cv.setBrushColor(T.desk);
  cv.clear();
  cv.setBrushColor(T.shadow);
  cv.fillRectangle(BW_X + 4, BW_Y + 4, BW_X + BW_W + 3, BW_Y + BW_H + 3);
  cv.setBrushColor(T.win);
  cv.fillRectangle(BW_X, BW_Y, BW_X + BW_W - 1, BW_Y + BW_H - 1);
  cv.setPenColor(T.shadow);
  cv.drawRectangle(BW_X, BW_Y, BW_X + BW_W - 1, BW_Y + BW_H - 1);
  cv.setBrushColor(T.accent);
  cv.fillRectangle(BW_X + 1, BW_Y + 1, BW_X + BW_W - 2, BW_Y + 28);
  cv.setPenColor(T.accentText);
  cv.drawText(&FONT_std_16, BW_X + 10, BW_Y + 7, "Start-up switches - save memory");
  bootRow(cv, 0, "SD card", "~15 KB. Off: files go to internal storage", gSet.sdOn);
  bootRow(cv, 1, "Wireless mesh", "A few KB. Coms boards nearby", gSet.meshOn);
#ifdef TIDYOS_XIAO
  bootRow(cv, 2, "VGA monitor", "D0-D3 + D6. Off: the browser only", gSet.vgaOn);
#else
  bootRow(cv, 2, "Status light", "~2 KB. Breathes when TidyOS is ready", gSet.ledOn);
#endif
  bootScreenRow(cv);
  bootRow(cv, 4, "Shelf messages", "~9 KB. Chat with your Shelves on Wi-Fi", gSet.shelfOn);
#ifdef TIDYOS_V4
  bootRow6(cv);
#endif
  // Boot button, and Update next to it
  int bx = BW_X + BW_W / 2 - 80, by = BW_Y + BW_H - 70;
  cv.setBrushColor(T.face);
  cv.fillRectangle(bx, by, bx + 160, by + 32);
  cv.setPenColor(T.shadow);
  cv.drawRectangle(bx, by, bx + 160, by + 32);
  cv.setPenColor(T.text);
  const char * t = "Boot TidyOS Pro";
  cv.drawText(&FONT_std_16, bx + 80 - cv.textExtent(&FONT_std_16, t) / 2, by + 8, t);
  int ux = BW_X + BW_W - 102;
  cv.setBrushColor(T.face);
  cv.fillRectangle(ux, by + 4, ux + 96, by + 28);
  cv.setPenColor(T.shadow);
  cv.drawRectangle(ux, by + 4, ux + 96, by + 28);
  cv.setPenColor(T.text);
  cv.drawText(&FONT_std_14, ux + 48 - cv.textExtent(&FONT_std_14, "Update") / 2, by + 9, "Update");
  cv.setPenColor(T.deskText);
  cv.setBrushColor(T.desk);
  cv.drawText(&FONT_std_12, 8, SCREEN_H - 16, TIDYOS_NAME " " TIDYOS_VERSION);
}

static RTC_NOINIT_ATTR uint32_t s_skipChooser;    // set before a restart the chooser itself asked for

static void bootChooser()
{
  if (s_skipChooser == 0x5EED) { s_skipChooser = 0; return; }
  const int startColors = gSet.colors;
  inputBootGrab(true);
  screenBegin();                         // the browser can show this window right away
  Canvas cv(s_dc);
  bootDraw(cv);
  bootStatus(cv, "Waiting for Wi-Fi...");
  cv.waitCompletion();
  const int COUNT = 12;
  uint32_t t0 = millis(), readyAt = 0;
  bool waitView = s_waitViewer == 0xB0B0;          // after the page's Restart: count down only once the browser is back
  s_waitViewer = 0;
  bool timer = true, boot = false;
  int shown = -2;
  while (!boot) {
    bool netUp = WiFi.status() == WL_CONNECTED || screenApOn();
    if (!readyAt && ((netUp && (!waitView || screenViewer())) || millis() - t0 > (waitView ? 60000 : 35000))) readyAt = millis();
    int left = readyAt ? COUNT - (int)((millis() - readyAt) / 1000) : COUNT;
    if (timer && readyAt && left <= 0) break;
    int toggled = -1;
    int x, y;
    if (inputBootClick(x, y)) {
      timer = false;
      for (int i = 0; i < 6; ++i) {
        int ry = BW_Y + 40 + i * 50;
        if (x >= BW_X && x < BW_X + BW_W && y >= ry - 4 && y < ry + 44) toggled = i;
      }
      int bx = BW_X + BW_W / 2 - 80, by = BW_Y + BW_H - 70;
      if (x >= bx && x <= bx + 160 && y >= by && y <= by + 32) boot = true;
      int ux = BW_X + BW_W - 102;
      if (x >= ux && x <= ux + 96 && y >= by + 4 && y <= by + 28) {     // Update: restart into update mode
        if (updateLoaderPresent()) {
          bootStatus(cv, "Restarting to update...");
          cv.waitCompletion();
          delay(400);
          updateRequest();
        } else bootStatus(cv, "Install once from the web flasher to enable updates.");
      }
    }
    int k;
    while ((k = inputBootKey()) >= 0) {
      timer = false;
      if (k >= 0x1E && k <= 0x23) toggled = k - 0x1E;        // keys 1 to 6
      if (k == 0x28 || k == 0x58) boot = true;                // Enter
    }
    if (toggled == 3) {                          // 4 colours -> 8 colours -> black & white -> 4 colours
      int c = gSet.colors == 0 ? 4 : gSet.colors;
      gSet.colors = c == 4 ? 8 : c == 8 ? 2 : 0;
      settingsSave();
      bootScreenRow(cv);
      shown = -2;
      toggled = -1;
    }
    if (toggled == 5) {
#if defined(TIDYOS_V3) || defined(TIDYOS_XIAO)
      gSet.oledOn = !gSet.oledOn;
      settingsSave();
      bootRow6(cv);
      shown = -2;
#elif defined(TIDYOS_V4)
      gSet.gpsOn = !gSet.gpsOn;
      settingsSave();
      bootRow6(cv);
      shown = -2;
#endif
      toggled = -1;
    }
    if (toggled == 4) {
      gSet.shelfOn = !gSet.shelfOn;
      settingsSave();
      bootRow(cv, 4, "Shelf messages", "~9 KB. Chat with your Shelves on Wi-Fi", gSet.shelfOn);
      shown = -2;
      toggled = -1;
    }
#ifdef TIDYOS_XIAO
    if (toggled == 2) {
      gSet.vgaOn = !gSet.vgaOn;
      settingsSave();
      vgaS3SetOn(gSet.vgaOn);
      bootRow(cv, 2, "VGA monitor", "D0-D3 + D6. Off: the browser only", gSet.vgaOn);
      shown = -2;
      toggled = -1;
    }
#endif
    if (toggled >= 0) {
      bool * v = toggled == 0 ? &gSet.sdOn : toggled == 1 ? &gSet.meshOn : &gSet.ledOn;
      *v = !*v;
      settingsSave();
      if (toggled == 2) ledSetOn(gSet.ledOn);
      bootRow(cv, toggled, toggled == 0 ? "SD card" : toggled == 1 ? "Wireless mesh" : "Status light",
              toggled == 0 ? "~15 KB. Off: files go to internal storage" : toggled == 1 ? "A few KB. Coms boards nearby" : "~2 KB. Breathes when TidyOS is ready", *v);
      shown = -2;
    }
    int want = !timer ? -1 : readyAt ? left : 99;
    if (want != shown) {
      shown = want;
      char b[80];
      if (want == -1) snprintf(b, sizeof(b), "Click Boot TidyOS Pro (or press Enter) when you're ready");
      else if (want == 99) snprintf(b, sizeof(b), "Waiting for Wi-Fi...   click anything to stop the timer");
      else snprintf(b, sizeof(b), "Booting in %d s   click anything to stop the timer", want);
      bootStatus(cv, b);
      char o[40];
      if (want == -1) strlcpy(o, "Start-up switches: waiting for you", sizeof(o));
      else if (want == 99) strlcpy(o, "Waiting for Wi-Fi...", sizeof(o));
      else snprintf(o, sizeof(o), "Start-up switches: booting in %d s", want);
      oledBoot(o, want == 99 || want == -1 ? 10 : 10 + (COUNT - want) * 15 / COUNT);
    }
    cv.waitCompletion();
    delay(50);
  }
  inputBootGrab(false);
  if (gSet.colors != startColors && (gSet.colors == 0 ? 4 : gSet.colors) != gColors) {   // screen mode changed here: set up at power-on
    s_skipChooser = 0x5EED;
    bootStatus(cv, "Restarting with the new screen...");
    cv.waitCompletion();
    delay(600);
    ESP.restart();
  }
  Serial.printf("[TidyOS] start-up switches: SD %s, mesh %s, light %s, shelf %s, GPS %s, OLED %s, colours %d\n", gSet.sdOn ? "on" : "off", gSet.meshOn ? "on" : "off", gSet.ledOn ? "on" : "off", gSet.shelfOn ? "on" : "off", gSet.gpsOn ? "on" : "off", gSet.oledOn ? "on" : "off", gSet.colors);
}
#endif

// what happened to the last update (the installer leaves tidyos/update.log on the card)
static void checkUpdateNote()
{
  {                                              // the installer couldn't use the card: its note is in flash
    Preferences p;
    if (p.begin("tidyload", false)) {
      String n = p.getString("note", "");
      if (n.length()) {
        p.remove("note");
        Serial.printf("[TidyOS] update installer said: %s\n", n.c_str());
        if (n == "update installed") s_bootNote = "TidyOS was updated to " TIDYOS_VERSION ".";
        else if (n != "no update waiting") s_bootNote = std::string("The update wasn't installed: ") + n.substring(0, 70).c_str();
      }
      p.end();
    }
  }
#ifdef TIDYOS_V4
  {                                              // an update downloaded to the board's own storage (no SD card)
    Preferences p;
    if (p.begin("tidyupd", false) && p.getBool("inffat", false)) {
      p.remove("inffat");
      bool already = s_flashIsFat && gFlashMounted;         // TidyOS's own storage is this same area
      if (already || FFat.begin(false, "/ffat", 2, "ffat")) {
        std::string base = already ? FLASH_MOUNT : "/ffat";
        if (FFat.exists("/tidyos/update.bin")) {          // still there: the boot menu didn't install it
          std::string ver;
          readFileStr((base + "/tidyos/update.ver").c_str(), ver, 40);
          while (!ver.empty() && (ver.back() == '\n' || ver.back() == ' ')) ver.pop_back();
          if (ver != TIDYOS_VERSION && s_bootNote.empty())
            s_bootNote = "An update was downloaded but the boot menu didn't install it. Install the boot menu from the web flasher once, then try again.";
          FFat.remove("/tidyos/update.bin");
        }
        FFat.remove("/tidyos/update.sha"); FFat.remove("/tidyos/update.ver");
        if (!already) FFat.end();
      }
    }
    p.end();
  }
#endif
  if (!gSDMounted) return;
  std::string l;
  if (readFileStr(SD_MOUNT "/tidyos/update.log", l, 200)) {
    remove(SD_MOUNT "/tidyos/update.log");
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    Serial.printf("[TidyOS] update installer said: %s\n", l.c_str());
    if (l.find("installed") != std::string::npos) s_bootNote = "TidyOS was updated to " TIDYOS_VERSION ".";
    else s_bootNote = "The update wasn't installed: " + l.substr(0, 70);
  } else if (fileSize(SD_MOUNT "/tidyos/update.bin") > 0) {
    std::string ver;
    readFileStr(SD_MOUNT "/tidyos/update.ver", ver, 40);
    while (!ver.empty() && (ver.back() == '\n' || ver.back() == ' ')) ver.pop_back();
    auto vnum = [](const std::string & v) { int a = 0, b = 0, c = 0; sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c); return a * 1000000 + b * 1000 + c; };
    if (ver.empty()) { }                  // a leftover from an earlier update (its note already shown): just tidy up
    else if (ver == TIDYOS_VERSION) s_bootNote = "TidyOS was updated to " TIDYOS_VERSION ".";   // it went in; the installer just couldn't tidy up
    else if (vnum(TIDYOS_VERSION) >= vnum(ver)) { }                         // already as new or newer (e.g. from the web flasher)
    else if (s_bootNote.empty())          // (the installer's own note from flash, if any, says more - keep that)
      s_bootNote = "An update was downloaded but the installer didn't run. Try again.";
    if (remove(SD_MOUNT "/tidyos/update.bin") != 0) {       // can't delete it? empty it, so it's never seen as waiting again
      FILE * z = fopen(SD_MOUNT "/tidyos/update.bin", "wb");
      if (z) fclose(z);
      Serial.println("[TidyOS] couldn't delete update.bin - emptied it");
    }
  }
  remove(SD_MOUNT "/tidyos/update.ver");
}

// Stack watch (Tidy Pro Plus): every few seconds, look at how much stack each task has never used.
// Prints a warning on the serial port when a task gets close to the end of its stack, so a crash
// like "Stack canary watchpoint triggered" can be traced to the task. Also a full table once,
// half a minute after start-up. Runs on core 1 with a small stack of its own.
#include "freertos/task_snapshot.h"
static void stackWatchTask(void *)
{
  uint32_t t0 = millis(); bool told = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    UBaseType_t n = uxTaskGetNumberOfTasks(), tcb = 0;
    TaskSnapshot_t * snap = (TaskSnapshot_t *)malloc(sizeof(TaskSnapshot_t) * (n + 4));
    if (!snap) continue;
    n = uxTaskGetSnapshotAll(snap, n + 4, &tcb);
    bool all = !told && millis() - t0 > 30000;
    if (all) { told = true; Serial.println("[TidyOS] stack headroom (bytes never used) per task:"); }
    for (UBaseType_t i = 0; i < n; ++i) {
      TaskHandle_t h = (TaskHandle_t)snap[i].pxTCB;
      uint32_t free_ = (uint32_t)uxTaskGetStackHighWaterMark(h);
      if (all) Serial.printf("  %-16s %6u\n", pcTaskGetName(h)[0] ? pcTaskGetName(h) : "(no name)", (unsigned)free_);
      else if (free_ < 600) {
        const char * nm = pcTaskGetName(h);                 // ESP-IDF's own idle / ipc tasks always run this tight
        if (strncmp(nm, "IDLE", 4) && strncmp(nm, "ipc", 3))
          Serial.printf("[TidyOS] stack low: task %s has only %u bytes spare\n", nm[0] ? nm : "(no name)", (unsigned)free_);
      }
    }
    free(snap);
  }
}

void oledStatus(const char * msg, int pct);     // oled.cpp
static void updStatus(const char * msg, int pct)  // update mode: the browser screen, the OLED and the serial port
{
  Serial.printf("[update] %s (%d%%)\n", msg, pct);
  oledStatus(msg, pct);
  splash(msg, pct);
}

SET_LOOP_TASK_STACK_SIZE(16 * 1024);   // the desktop and every app run in the loop task: 8 KB was the classic ESP32's size

void setup()
{
#if defined(TIDYOS_HTML5) && !defined(TIDYOS_XIAO)
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);   // no Bluetooth in this edition: give its RAM to apps
#endif                                               // (XIAO: Bluetooth keyboard + mouse for its VGA monitor)
  Serial.setRxBufferSize(6144);   // room for a whole line from the flasher's "Copy files over USB" (256 drops bytes)
  Serial.begin(115200);
  delay(200);
  crashBegin();     // did the last run crash? (Task Manager shows it)
  ledBegin();       // white LED: slow blink while starting
  Serial.println("\n[TidyOS Pro] " TIDYOS_VERSION " (" TIDYOS_BOARD ") starting");
#ifdef TIDYOS_V4
  // dual boot: the next restart goes back to the boot menu (TidyBoot), which starts the last
  // choice after its 5 s countdown. Without a boot menu this does nothing.
  if (const esp_partition_t * menu = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "menu"))
    esp_ota_set_boot_partition(menu);
#endif

  settingsLoad();
  chooseDeskApps();       // which apps go on the desktop (picked in the web flasher)
#ifndef TIDYOS_V3                       // (a log line for developers: a V3 can't spare the 3 KB)
  xTaskCreatePinnedToCore(stackWatchTask, "stackwatch", 3072, nullptr, 1, nullptr, 1);
#endif
  ledSetOn(gSet.ledOn);   // ledBegin ran before the settings were read, so the switch looked "off" to it

  bool updMode = updateModeRequested();
  if (!updMode) updateRestoreColors();    // back from an update: the screen mode it ran in was only for the update
  if (updMode) {
    // update mode: screen + Wi-Fi only, so there's room for HTTPS
    Serial.println("[TidyOS] update mode");
#if !defined(TIDYOS_HTML5) || defined(TIDYOS_XIAO)   // (the HTML5 edition already gave it back at the top of setup(): a second
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);   // release fails and aborts, which made every update reboot)
#endif
    netBegin();
    startDisplay();
#ifdef TIDYOS_HTML5
    releaseVgaPins();
#ifdef TIDYOS_VGA_OUT
    if (gSet.vgaOn) vgaS3SetOn(true);    // XIAO: the update's progress on the monitor too
#endif
    screenBegin();
#endif
    splashLogo();
    s_barExact = true;
    barStart();
    // the update (HTTPS, certificate checks, writing to the card or the board's storage) needs much more
    // stack than the loop task has, so it runs in a task of its own; it restarts the board when done
    static TaskHandle_t upd = nullptr;
    if (xTaskCreatePinnedToCore([](void *) { updateRun(updStatus); vTaskDelete(nullptr); }, "update", 24576, nullptr, 2, &upd, 1) != pdPASS)
      updateRun(updStatus);                      // (no room for the task: try it here)
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  oledBoot("Getting ready...", 2);
  inputBegin();     // Bluetooth first: it needs a big block of RAM
  std::set_new_handler(lowMemoryWait);
  memLog("start");
  netBegin();       // Wi-Fi radio
  memLog("Wi-Fi");

#ifdef TIDYOS_HTML5
  startDisplay();
  releaseVgaPins();     // no monitor in this edition: free GPIO 19/18/5/... (the in-line SD wiring uses them)
#ifdef TIDYOS_VGA_OUT
  if (gSet.vgaOn) vgaS3SetOn(true);   // XIAO: its own VGA output
#endif
  bootChooser();        // start-up switches (SD card, mesh, status light) before they take any RAM
  if (gSet.sdOn) oledBoot("Looking for the SD card...", 27);
  if (gSet.sdOn) gSDMounted = mountAnySD();
#else
  if (gSet.sdOn) gSDMounted = mountAnySD();
  startDisplay();
#endif
  Serial.printf("[TidyOS] SD card %s\n", gSDMounted ? "mounted" : gSet.sdOn ? "not found - using internal flash" : "off (SD Card icon turns it on) - using internal flash");

  splashLogo();
  barStart();
  splash(gSDMounted ? "SD card found" : gSet.sdOn ? "No SD card - preparing internal storage..." : "Preparing internal storage...", 30);

  if (!gSDMounted) gFlashMounted = mountInternal();
  memLog("display + storage");
  checkUpdateNote();
  {                                              // why the last update stopped (it said so on the OLED too)
    Preferences p;
    if (p.begin("tidyupd", false)) {
      String r = p.getString("res", "");
      if (r.length()) { p.remove("res"); if (s_bootNote.empty()) s_bootNote = std::string("Update: ") + r.c_str(); }
      p.end();
    }
  }
  makeDirs();
  crashSave(dataPath("crash.txt"));
#ifdef TIDYOS_V4
  splash("Starting the 900 MHz radio and GPS", 45);
#ifdef TIDYOS_V3
  if (gSet.meshOn && gSet.mesh900) rf::begin();     // V3 (no PSRAM): otherwise the radio starts when an app needs it (~25 KB)
#else
  rf::begin();
#endif
  rf::gnssStart();
  if (gSet.oledOn) oledBegin();   // the board's own screen: where to find the desktop
  else oledOff();                 // (switched off at start-up: dark, and no task or memory for it)
  memLog("radio + GNSS");
#endif
  splash("Loading the dictionary", 50);
  spellBegin();
  memLog("dictionary");
  splash("Looking for your Shelves", 70);
#ifdef TIDYOS_HTML5
  if (gSet.ssid.length() && msgWanted()) msgBegin();   // otherwise started once on home Wi-Fi (saves RAM for the setup hotspot)
#else
  if (msgWanted()) msgBegin();       // The Shelf messaging + Coms mesh
#endif
  memLog("Shelf + mesh");
  if (gSet.fileShare) fileServerSet(true);
  splash("Starting the web screen", 80);
  screenBegin();    // HTML5 edition: the desktop in a web browser
  memLog("web screen");
#ifdef TIDYOS_HTML5
  splash(gSet.ssid.length() ? "Joining Wi-Fi" : ("Starting the " + boardId() + " hotspot").c_str(), 90);
#else
  splash(gSet.ssid.length() ? "Joining Wi-Fi and Bluetooth" : "Starting Bluetooth", 90);
#endif
  delay(400);
#ifdef TIDYOS_HTML5
  // keep the bar going until the page has looked up the weather and headlines, so the desktop opens filled in
  if (screenViewer() && WiFi.status() == WL_CONNECTED) {
    splash("Getting the weather and headlines", 94);
    screenMedia("w");
    uint32_t t0 = millis();
    while (millis() - t0 < 10000 && screenViewer() && !((s_wx.code >= 0 || s_wx.err[0]) && s_newsCount > 0)) delay(100);
  }
#endif
  splash("Ready", 100);
  delay(700);           // a moment to see it all come up
  barStop();
  ledReady();       // white LED: breathing
}

void loop()
{
  s_app.run(s_dc, &s_keyboard, nullptr);
}

// ====================================================================== widget styling

void applyStyle(uiObject * o)
{
  auto & t = o->objectType();
  if (t.uiWindow) {
    auto w = (uiWindow*)o;
    w->windowStyle().borderColor        = T.shadow;
    w->windowStyle().activeBorderColor  = T.accent;
    w->windowStyle().focusedBorderColor = T.accent;
  }
  if (t.uiFrame) {
    auto f = (uiFrame*)o;
    f->frameStyle().backgroundColor                = T.win;
    f->frameStyle().titleBackgroundColor           = T.face;
    f->frameStyle().activeTitleBackgroundColor     = T.accent;
    f->frameStyle().titleColor                     = T.text;
    f->frameStyle().activeTitleColor               = T.accentText;
    f->frameStyle().titleFont                      = &FONT_std_14;
    f->frameStyle().buttonColor                    = T.text;
    f->frameStyle().activeButtonColor              = T.accentText;
    f->frameStyle().mouseOverBackgroundButtonColor = T.win;
    f->frameStyle().mouseOverButtonColor           = T.text;
    f->windowStyle().borderColor                   = T.face;
    f->windowStyle().borderSize                    = 2;
  }
  if (t.uiScrollableControl) {
    auto c = (uiScrollableControl*)o;
    c->scrollableControlStyle().scrollBarBackgroundColor          = T.face;
    c->scrollableControlStyle().scrollBarForegroundColor          = T.shadow;
    c->scrollableControlStyle().mouseOverScrollBarForegroundColor = T.accent;
  }
  if (t.uiButton) {
    auto b = (uiButton*)o;
    b->buttonStyle().backgroundColor          = T.face;
    b->buttonStyle().downBackgroundColor      = T.accent;
    b->buttonStyle().mouseOverBackgroundColor = T.win;
    b->buttonStyle().mouseDownBackgroundColor = T.accent;
    b->buttonStyle().mouseOverTextColor       = T.text;
    b->buttonStyle().textColor                = T.text;
    b->buttonStyle().downTextColor            = T.accentText;
  }
  if (t.uiTextEdit) {
    auto e = (uiTextEdit*)o;
    e->textEditStyle().backgroundColor          = T.win;
    e->textEditStyle().mouseOverBackgroundColor = T.win;
    e->textEditStyle().focusedBackgroundColor   = T.win;
    e->textEditStyle().textColor                = T.text;
    e->windowStyle().borderSize                 = 1;
  }
  if (t.uiLabel) {
    auto l = (uiLabel*)o;
    l->labelStyle().backgroundColor = T.win;
    l->labelStyle().textColor       = T.text;
  }
  if (t.uiCustomListBox) {
    auto l = (uiCustomListBox*)o;
    l->listBoxStyle().backgroundColor                = T.win;
    l->listBoxStyle().focusedBackgroundColor         = T.win;
    l->listBoxStyle().selectedBackgroundColor        = T.accent;
    l->listBoxStyle().focusedSelectedBackgroundColor = T.accent;
    l->listBoxStyle().textColor                      = T.text;
    l->listBoxStyle().selectedTextColor              = T.accentText;
    l->windowStyle().borderSize                      = 1;
  }
  if (t.uiCheckBox) {
    auto c = (uiCheckBox*)o;
    c->checkBoxStyle().backgroundColor          = T.win;
    c->checkBoxStyle().checkedBackgroundColor   = T.win;
    c->checkBoxStyle().mouseOverBackgroundColor = T.face;
    c->checkBoxStyle().foregroundColor          = T.text;
    c->checkBoxStyle().mouseOverForegroundColor = T.text;
  }
}
