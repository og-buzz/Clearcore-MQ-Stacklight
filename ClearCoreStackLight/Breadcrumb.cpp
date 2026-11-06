// Breadcrumb.cpp — see Breadcrumb.h.
#include "Breadcrumb.h"

#include <stdio.h>

namespace sl {

namespace {

const char* const kStepNames[kStepCount] = {"boot",      "console",  "network", "dhcp",    "mqtt-connect",
                                            "mqtt-loop", "button",   "strip",   "light",   "sd-read",
                                            "sd-write",  "reboot",   "watchdog-test"};

// Mixes the fields so a record of random bytes (backup RAM after a power-on) almost never passes.
uint32_t crumb_check(uint32_t step, uint32_t uptime_s) {
  return kCrumbMagic ^ (step * 0x9E3779B1u) ^ ((uptime_s << 13) | (uptime_s >> 19)) ^ 0xA5C3F00Fu;
}

}  // namespace

const char* step_name(uint8_t step) { return step < kStepCount ? kStepNames[step] : "unknown"; }

const char* reset_cause_name(uint8_t cause) {
  if (cause & kCauseWdt) return "watchdog";
  if (cause & kCausePor) return "power-on";
  if (cause & kCauseBodCore) return "brown-out (core)";
  if (cause & kCauseBodVdd) return "brown-out (supply)";
  if (cause & kCauseNvm) return "nvm";
  if (cause & kCauseExt) return "reset pin";
  if (cause & kCauseSyst) return "software";
  if (cause & kCauseBackup) return "backup";
  return "unknown";
}

void crumb_write(volatile Crumb* c, uint8_t step, uint32_t uptime_s) {
  c->magic = 0;  // invalid until the last store
  c->step = step;
  c->uptime_s = uptime_s;
  c->check = crumb_check(step, uptime_s);
  c->magic = kCrumbMagic;
}

bool crumb_read(const volatile Crumb* c, uint8_t* step, uint32_t* uptime_s) {
  const uint32_t s = c->step, up = c->uptime_s;
  if (c->magic != kCrumbMagic || s > 0xFF || c->check != crumb_check(s, up)) return false;
  *step = (uint8_t)s;
  *uptime_s = up;
  return true;
}

void crumb_clear(volatile Crumb* c) { c->magic = 0; }

void reset_report(char* out, size_t cap, uint8_t cause, bool have_crumb, uint8_t step, uint32_t uptime_s) {
  if (!cap) return;
  if (!(cause & kCauseWdt)) snprintf(out, cap, "%s", reset_cause_name(cause));
  else if (!have_crumb) snprintf(out, cap, "watchdog (no breadcrumb)");
  else snprintf(out, cap, "watchdog in %s after %lu s", step_name(step), (unsigned long)uptime_s);
}

uint32_t wdt_code_ms(uint8_t code) {
  if (code > 0xB) return 0;
  return ((uint32_t)8 << code) * 1000 / 1024;  // exact from code 7 (1 s) up; 7 ms for code 0 (7.8 ms)
}

void duration_text(char* out, size_t cap, uint32_t ms) {
  if (!cap) return;
  if (ms >= 1000 && ms % 1000 == 0) snprintf(out, cap, "%lu s", (unsigned long)(ms / 1000));
  else snprintf(out, cap, "%lu ms", (unsigned long)ms);
}

}  // namespace sl
