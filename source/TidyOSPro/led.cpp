// TidyOS: the board's LED as a status light, like The Shelf.
// Heltec V4: the white LED on GPIO 35. (GPIO 2 - the NodeMCU's blue LED - is the V4's radio
// front-end enable, so it must not be touched here.) V4-R8: GPIO 35 belongs to its PSRAM, so no light.
//   starting up           slow blink (half a second on, half off)
//   ready                 breathing: a slow, smooth fade in and out (~4 s a breath)
//   microSD card busy     a quick flicker while the card is read or written
//   low on memory         fast flash (close an app)
#include <Arduino.h>
#include <math.h>
#include "led.h"
#include "crashlog.h"
#include "core.h"
#include "config.h"
#if defined(TIDYOS_XIAO)
  #define LED_PIN -1              // XIAO: its LED (GPIO 21) is wired to the Wio-SX1262's button: used as a button
#elif defined(TIDYOS_V4) && CONFIG_SPIRAM_MODE_OCT
  #define LED_PIN -1              // V4-R8: no LED the program may use
#elif defined(TIDYOS_V4)
  #define LED_PIN PIN_LED         // V4: white LED, GPIO 35
#else
  #define LED_PIN 2
#endif
#define LED_CH  7                 // LEDC (PWM) channel 7: nothing else in TidyOS uses LEDC

static bool s_pwm = false;
static inline void ledLevel(uint32_t v)      // 0 = off .. 255 = full
{
  if (LED_PIN < 0) return;
#ifdef TIDYOS_HTML5
  if (s_pwm) ledcWrite(LED_CH, v);
  else
#endif
  digitalWrite(LED_PIN, v >= 128 ? HIGH : LOW);   // no PWM: plain on / off
}

volatile uint32_t gSdActivityMs = 0;
static volatile bool s_ready = false;

// Card reads/writes are stamped here. The build links these in place of FatFs's disk functions
// (-Wl,--wrap=ff_disk_read,--wrap=ff_disk_write); without that flag the light never flickers.
// (Internal flash uses SPIFFS, not FatFs, so only the SD card is counted.)
extern "C" {
  int __real_ff_disk_read(uint8_t pdrv, uint8_t * buff, uint32_t sector, unsigned count);
  int __real_ff_disk_write(uint8_t pdrv, const uint8_t * buff, uint32_t sector, unsigned count);
  int __wrap_ff_disk_read(uint8_t pdrv, uint8_t * buff, uint32_t sector, unsigned count)
  { gSdActivityMs = millis(); return __real_ff_disk_read(pdrv, buff, sector, count); }
  int __wrap_ff_disk_write(uint8_t pdrv, const uint8_t * buff, uint32_t sector, unsigned count)
  { gSdActivityMs = millis(); return __real_ff_disk_write(pdrv, buff, sector, count); }
}

static volatile bool s_wanted = false, s_running = false;

static void ledTaskBody(void * arg)
{
  for (;;) {
    if (!s_wanted) { ledLevel(0); s_running = false; return; }
    uint32_t now = millis();
    uint32_t duty;
    uint32_t sd = gSdActivityMs;
    if (!s_ready) duty = (now / 500) % 2 ? 255 : 0;                                   // starting: slow blink
    else if (sd && now - sd < 90) duty = (now / 35) % 2 ? 255 : 0;                    // card busy: flicker
    else if (ramFree() < 10000) duty = (now / 70) % 2 ? 255 : 0;              // low memory: fast flash
    else {                                                                            // ready: breathing
      float t = (now % 4000) / 4000.0f;
      float v = 0.5f - 0.5f * cosf(t * 2 * PI);
      duty = (uint32_t)(4 + v * v * 251);                                             // squared: looks even to the eye
    }
    ledLevel(duty);
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// runs the task, then ends it - after the body's locals are destroyed (vTaskDelete
// inside the body would skip their destructors and leak their memory)
static void ledTask(void * arg) { ledTaskBody(arg); vTaskDelete(nullptr); }

void ledBegin()
{
  if (LED_PIN < 0) { Serial.println("[TidyOS] status light: none on this board"); return; }
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
#ifdef TIDYOS_HTML5
  s_pwm = ledcSetup(LED_CH, 5000, 8) != 0;        // fading
  if (s_pwm) ledcAttachPin(LED_PIN, LED_CH);
#endif                                            // the monitor edition has no IRAM to spare for it: on / off
  ledLevel(0);
  Serial.printf("[TidyOS] status light on GPIO %d (%s), switch %s\n", LED_PIN, s_pwm ? "PWM" : "on/off", gSet.ledOn ? "on" : "off");
  ledSetOn(gSet.ledOn);
}

// the light's little task only exists while it's on (~2 KB of RAM)
void ledSetOn(bool on)
{
  s_wanted = on;
  if (LED_PIN < 0) return;
  if (on && !s_running) {
    s_running = true;
    if (xTaskCreatePinnedToCore(ledTask, "led", 4096, nullptr, 1, nullptr, 0) != pdPASS) s_running = false;
  }
  if (!on && !s_running) ledLevel(0);
}

void ledReady() { s_ready = true; }
