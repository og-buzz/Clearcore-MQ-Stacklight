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
  int8_t button;          // kNoPin = no button
  Timing timing;
  bool allow_reset;
};

void defaults(Settings* s);

// Applies one line of settings.ini. Blank lines and lines starting with '#' or ';' are ignored.
// Returns false and writes a reason for an unknown key or a bad value.
bool apply_line(Settings* s, const char* line, char* err, size_t err_cap);

// True when the settings are enough to connect to MQTT; otherwise writes what's missing.
bool mqtt_ready(const Settings& s, char* err, size_t err_cap);

// False if two settings name the same pin; writes which.
bool pins_ok(const Settings& s, char* err, size_t err_cap);

// "{project}/{client}/{site}/{role}/{kind}"; returns false if it doesn't fit.
bool topic(const Settings& s, const char* kind, char* out, size_t cap);

const char* pin_name(int8_t pin);

}  // namespace sl
