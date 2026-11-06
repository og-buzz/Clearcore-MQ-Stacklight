// Breadcrumb.h — what the main loop was doing when the watchdog fired, and the reset report built from it.
//
// The loop records which step it is in. The watchdog's early-warning interrupt copies that step and the uptime
// into a Crumb in RAM that survives the reset (the sketch maps it onto the SAME53's backup RAM); the next boot
// reads it back and reports e.g. "watchdog in mqtt-connect after 3600 s".
//
// Plain C++ with no Arduino dependencies, so it can be unit-tested on a PC (see test/). Nothing here is named
// Watchdog: the watchdog library owns that global.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sl {

// Steps of the main loop and of the calls that can take long. Stored as one byte; add new ones at the end so
// a crumb written by an older build still names the right step.
enum Step : uint8_t {
  kStepBoot,
  kStepConsole,
  kStepNetwork,
  kStepDhcp,
  kStepMqttConnect,
  kStepMqttLoop,
  kStepButton,
  kStepStrip,
  kStepLight,
  kStepSdRead,
  kStepSdWrite,
  kStepReboot,
  kStepWatchdogTest,
  kStepCount
};

// The step's name for messages ("mqtt-connect"), or "unknown" for a number out of range.
const char* step_name(uint8_t step);

// RSTC.RCAUSE bits (SAME53). Only the latest reset's bit is set.
const uint8_t kCausePor = 0x01, kCauseBodCore = 0x02, kCauseBodVdd = 0x04, kCauseNvm = 0x08, kCauseExt = 0x10,
              kCauseWdt = 0x20, kCauseSyst = 0x40, kCauseBackup = 0x80;

// The reset cause in words ("power-on", "watchdog", "software", ...), or "unknown".
const char* reset_cause_name(uint8_t cause);

// The record that survives the reset. All words, so each store from the interrupt is a single aligned write.
// magic is written last and check covers the rest, so a record torn by the reset, or the random contents of
// the RAM after a power-on, is rejected.
struct Crumb {
  uint32_t magic;
  uint32_t step;
  uint32_t uptime_s;
  uint32_t check;
};
const uint32_t kCrumbMagic = 0x57445442;  // "WDTB"

// Records a step and the uptime. Plain stores only: safe in the early-warning interrupt.
void crumb_write(volatile Crumb* c, uint8_t step, uint32_t uptime_s);
// Reads a record back. Returns false if there is none or it is damaged.
bool crumb_read(const volatile Crumb* c, uint8_t* step, uint32_t* uptime_s);
// Invalidates the record, so it is reported once and a stale one is never reported against a later reset.
void crumb_clear(volatile Crumb* c);

// The last reset in words: "watchdog in <step> after <n> s" after a watchdog reset with a valid crumb,
// "watchdog (no breadcrumb)" without one (the early warning could not run, e.g. a hang with interrupts off),
// otherwise the cause's name. Writes at most cap bytes, always terminated.
void reset_report(char* out, size_t cap, uint8_t cause, bool have_crumb, uint8_t step, uint32_t uptime_s);

// Whether a kick is due: at least gap ms since the last one. Kicking waits for the watchdog's slow clock domain
// to sync, so the loop kicks at most this often instead of on every pass. Correct across millis() wrapping.
inline bool kick_due(uint32_t now_ms, uint32_t last_kick_ms, uint32_t gap_ms) {
  return now_ms - last_kick_ms >= gap_ms;
}

// The time a WDT period or early-warning code stands for, in ms: CONFIG.PER and EWCTRL.EWOFFSET both count
// 8 << code cycles of the 1.024 kHz clock (code 0xB: 16384 cycles, 16 s). 0 for a reserved code (above 0xB).
uint32_t wdt_code_ms(uint8_t code);

// Whether a watchdog period leaves room for the longest call that runs between two kicks (DHCP, an MQTT
// connect). Half the call again as margin: the watchdog's clock, OSCULP32K, isn't precise, and the call has
// some overhead around its own time limits. 16 s fits an 8 s call; an 8 s period, locked by Always-On, doesn't.
inline bool wdt_period_fits(uint32_t period_ms, uint32_t longest_call_ms) {
  return period_ms >= longest_call_ms + longest_call_ms / 2;
}

// A duration for messages: "16 s" for whole seconds, otherwise "500 ms". Always terminated.
void duration_text(char* out, size_t cap, uint32_t ms);

}  // namespace sl
