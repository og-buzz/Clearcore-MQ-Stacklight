// Settings.h — the device settings read from settings.ini on the SD card.
//
// Plain C++ with no Arduino dependencies, so it can be unit-tested on a PC (see test/).
// See settings.example.ini for every key.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Light.h"

namespace sl {

// Inputs and outputs a setting can name. Outputs (light channels) must be IO0..IO5.
enum Pin : int8_t { kNoPin = -1, kIO0, kIO1, kIO2, kIO3, kIO4, kIO5, kDI6, kDI7, kDI8, kPinCount };

struct Settings {
  // MQTT broker
  char broker[64];
  uint16_t port;
  char user[64];
  char password[64];
  // Topic path: {project}/{client}/{site}/{role}/{kind}
  char project[32];
  char client[32];
  char site[32];
  char role[32];
  char asset[32];  // physical asset tag, e.g. cc-0001; used as the MQTT client ID
  // Network: DHCP unless ip is set
  bool dhcp;
  uint8_t ip[4], gateway[4], dns[4], subnet[4];
  // Light
  int8_t pin[kChannels];  // per channel, kNoPin = not wired
  uint16_t level[kChannels];  // brightness when lit, in tenths of a percent (0..1000); below 1000 the output runs
                              // as PWM. Alarm is always 1000.
  int8_t button;          // kNoPin = no button
  // Addressable WS2805 strip mirroring the light (see Strip.h)
  int8_t strip_port;           // kNoStrip, 0 = COM-0, 1 = COM-1
  uint16_t strip_pixels;       // 1..kStripMaxPixels
  uint16_t strip_level[3];     // red, green, blue brightness on the strip, tenths of a percent (0..1000)
  uint8_t strip_timing;        // signal timing, 0..kStripTimings-1 (see Strip.h)
  uint8_t strip_order[3];      // colour sent in each of the first three byte positions: 0 red, 1 green, 2 blue
  Timing timing;
  bool allow_reset;
};

const int8_t kNoStrip = -1;

void defaults(Settings* s);

// Applies one line of settings.ini. Blank lines and lines starting with '#' or ';' are ignored.
// Returns false and writes a reason for an unknown key or a bad value.
bool apply_line(Settings* s, const char* line, char* err, size_t err_cap);

// True when a strip is configured (port and pixel count); otherwise writes what's missing, if anything was set.
bool strip_ready(const Settings& s, char* err, size_t err_cap);

// True when the settings are enough to connect to MQTT; otherwise writes what's missing.
bool mqtt_ready(const Settings& s, char* err, size_t err_cap);

// False if two settings name the same pin; writes which.
bool pins_ok(const Settings& s, char* err, size_t err_cap);

// "{project}/{client}/{site}/{role}/{kind}"; returns false if it doesn't fit.
bool topic(const Settings& s, const char* kind, char* out, size_t cap);

const char* pin_name(int8_t pin);

// Brightness levels are percentages with at most one decimal, e.g. "25" or "27.5", kept in tenths (0..1000).
bool parse_level(const char* text, uint16_t* tenths);
// "25", "27.5" or "100" for a level in tenths.
void format_level(uint16_t tenths, char* out, size_t cap);

// Rewrites settings.ini text so each keys[i] is set to values[i]: lines setting that key are replaced (keeping their
// line ending), keys not in the file are appended, and every other line is copied unchanged. At most 8 keys.
// Returns the new length (out is NUL-terminated), or -1 if it doesn't fit in cap.
long set_keys(const char* in, size_t len, const char* const keys[], const char* const values[], size_t n, char* out,
              size_t cap);

}  // namespace sl
