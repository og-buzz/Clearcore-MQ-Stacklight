// Settings.cpp — see Settings.h.
#include "Settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace sl {

namespace {

const char* const kPinNames[kPinCount] = {"IO0", "IO1", "IO2", "IO3", "IO4", "IO5", "DI6", "DI7", "DI8"};

void fail(char* err, size_t cap, const char* what, const char* key) {
  if (err && cap) snprintf(err, cap, "%s: %s", key, what);
}

bool set_text(char* dst, size_t cap, const char* value, const char* key, char* err, size_t err_cap) {
  size_t n = strlen(value);
  if (n >= cap) {
    char msg[48];
    snprintf(msg, sizeof msg, "longer than %u characters", (unsigned)(cap - 1));
    fail(err, err_cap, msg, key);
    return false;
  }
  memcpy(dst, value, n + 1);
  return true;
}

bool parse_uint(const char* v, unsigned long max, unsigned long* out) {
  if (!*v) return false;
  for (const char* p = v; *p; p++)
    if (!isdigit((unsigned char)*p)) return false;
  unsigned long n = strtoul(v, nullptr, 10);
  if (n > max) return false;
  *out = n;
  return true;
}

bool parse_ip(const char* v, uint8_t out[4]) {
  unsigned a, b, c, d;
  char tail;
  if (sscanf(v, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  out[0] = (uint8_t)a, out[1] = (uint8_t)b, out[2] = (uint8_t)c, out[3] = (uint8_t)d;
  return true;
}

bool parse_pin(const char* v, bool output, int8_t* out) {
  char up[8];
  size_t n = strlen(v);
  if (n >= sizeof up) return false;
  for (size_t i = 0; i <= n; i++) up[i] = (char)toupper((unsigned char)v[i]);
  if (strcmp(up, "NONE") == 0 || up[0] == '\0') {
    *out = kNoPin;
    return true;
  }
  for (int8_t p = 0; p < kPinCount; p++) {
    if (strcmp(up, kPinNames[p]) == 0) {
      if (output && p > kIO5) return false;  // only IO0..IO5 can drive outputs
      *out = p;
      return true;
    }
  }
  return false;
}

bool parse_bool(const char* v, bool* out) {
  if (!strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "1")) return *out = true, true;
  if (!strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "0")) return *out = false, true;
  return false;
}

}  // namespace

void defaults(Settings* s) {
  memset(s, 0, sizeof *s);
  s->port = 1883;
  s->dhcp = true;
  for (uint8_t c = 0; c < kChannels; c++) s->pin[c] = kNoPin;  // nothing driven until configured
  s->button = kNoPin;
  s->timing = {350, 150, 750, 250};
  s->allow_reset = false;
}

bool apply_line(Settings* s, const char* line, char* err, size_t err_cap) {
  char buf[160];
  size_t n = strlen(line);
  if (n >= sizeof buf) {
    fail(err, err_cap, "line too long", "settings.ini");
    return false;
  }
  memcpy(buf, line, n + 1);
  // Trim, strip comments and a trailing CR.
  char* p = buf;
  while (isspace((unsigned char)*p)) p++;
  if (*p == '\0' || *p == '#' || *p == ';') return true;
  char* eq = strchr(p, '=');
  if (!eq) {
    fail(err, err_cap, "expected key=value", p);
    return false;
  }
  *eq = '\0';
  char* key = p;
  char* value = eq + 1;
  for (char* e = eq - 1; e >= key && isspace((unsigned char)*e); e--) *e = '\0';
  while (isspace((unsigned char)*value)) value++;
  char* hash = strstr(value, " #");
  if (hash) *hash = '\0';
  for (char* e = value + strlen(value) - 1; e >= value && isspace((unsigned char)*e); e--) *e = '\0';

  unsigned long u;
  if (!strcmp(key, "broker")) return set_text(s->broker, sizeof s->broker, value, key, err, err_cap);
  if (!strcmp(key, "user")) return set_text(s->user, sizeof s->user, value, key, err, err_cap);
  if (!strcmp(key, "password")) return set_text(s->password, sizeof s->password, value, key, err, err_cap);
  if (!strcmp(key, "project")) return set_text(s->project, sizeof s->project, value, key, err, err_cap);
  if (!strcmp(key, "client")) return set_text(s->client, sizeof s->client, value, key, err, err_cap);
  if (!strcmp(key, "site")) return set_text(s->site, sizeof s->site, value, key, err, err_cap);
  if (!strcmp(key, "role")) return set_text(s->role, sizeof s->role, value, key, err, err_cap);
  if (!strcmp(key, "asset")) return set_text(s->asset, sizeof s->asset, value, key, err, err_cap);
  if (!strcmp(key, "port")) {
    if (!parse_uint(value, 65535, &u) || u == 0) return fail(err, err_cap, "not a port number", key), false;
    s->port = (uint16_t)u;
    return true;
  }
  if (!strcmp(key, "ip")) {
    if (!strcmp(value, "dhcp")) return s->dhcp = true, true;
    if (!parse_ip(value, s->ip)) return fail(err, err_cap, "not an IPv4 address or 'dhcp'", key), false;
    s->dhcp = false;
    return true;
  }
  uint8_t* ip_field = !strcmp(key, "gateway") ? s->gateway
                      : !strcmp(key, "dns")   ? s->dns
                      : !strcmp(key, "subnet") ? s->subnet
                                               : nullptr;
  if (ip_field) {
    if (!parse_ip(value, ip_field)) return fail(err, err_cap, "not an IPv4 address", key), false;
    return true;
  }
  const char* const channel_keys[kChannels] = {"red", "green", "blue", "alarm"};
  for (uint8_t c = 0; c < kChannels; c++) {
    if (!strcmp(key, channel_keys[c])) {
      if (!parse_pin(value, true, &s->pin[c])) return fail(err, err_cap, "must be IO0..IO5 or none", key), false;
      return true;
    }
  }
  if (!strcmp(key, "button")) {
    if (!parse_pin(value, false, &s->button)) return fail(err, err_cap, "must be IO0..IO5, DI6..DI8 or none", key), false;
    return true;
  }
  uint16_t* ms = !strcmp(key, "fast_on")    ? &s->timing.fast_on
                 : !strcmp(key, "fast_off") ? &s->timing.fast_off
                 : !strcmp(key, "slow_on")  ? &s->timing.slow_on
                 : !strcmp(key, "slow_off") ? &s->timing.slow_off
                                            : nullptr;
  if (ms) {
    if (!parse_uint(value, 60000, &u) || u == 0) return fail(err, err_cap, "must be 1..60000 ms", key), false;
    *ms = (uint16_t)u;
    return true;
  }
  if (!strcmp(key, "allow_reset")) {
    if (!parse_bool(value, &s->allow_reset)) return fail(err, err_cap, "must be true or false", key), false;
    return true;
  }
  fail(err, err_cap, "unknown setting", key);
  return false;
}

bool mqtt_ready(const Settings& s, char* err, size_t err_cap) {
  const char* missing = !s.broker[0]    ? "broker"
                        : !s.project[0] ? "project"
                        : !s.client[0]  ? "client"
                        : !s.site[0]    ? "site"
                        : !s.role[0]    ? "role"
                                        : nullptr;
  if (missing) {
    fail(err, err_cap, "not set", missing);
    return false;
  }
  return true;
}

bool pins_ok(const Settings& s, char* err, size_t err_cap) {
  const char* const names[kChannels + 1] = {"red", "green", "blue", "alarm", "button"};
  int8_t pins[kChannels + 1];
  for (uint8_t c = 0; c < kChannels; c++) pins[c] = s.pin[c];
  pins[kChannels] = s.button;
  for (uint8_t i = 0; i <= kChannels; i++) {
    for (uint8_t j = i + 1; j <= kChannels; j++) {
      if (pins[i] != kNoPin && pins[i] == pins[j]) {
        if (err && err_cap) snprintf(err, err_cap, "%s and %s both use %s", names[i], names[j], pin_name(pins[i]));
        return false;
      }
    }
  }
  return true;
}

bool topic(const Settings& s, const char* kind, char* out, size_t cap) {
  int n = snprintf(out, cap, "%s/%s/%s/%s/%s", s.project, s.client, s.site, s.role, kind);
  return n > 0 && (size_t)n < cap;
}

const char* pin_name(int8_t pin) {
  return (pin >= 0 && pin < kPinCount) ? kPinNames[pin] : "none";
}

}  // namespace sl
