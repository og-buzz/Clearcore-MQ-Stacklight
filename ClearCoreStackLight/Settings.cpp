// Settings.cpp — see Settings.h.
#include "Settings.h"

#include "Strip.h"

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
  s->strip_port = kNoStrip;  // no strip until configured: a COM port may have other hardware on it
  s->strip_pixels = 0;
  s->strip_timing = 4;  // verified on BTF's WS2805 strip (see Strip.h)
  strcpy(s->strip_layout, "line");
  for (uint8_t i = 0; i < 3; i++) s->strip_order[i] = i;  // R G B, as the WS2805 datasheet says
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
  const char* const old_level_keys[] = {"red_level",       "green_level",       "blue_level",
                                        "strip_red_level", "strip_green_level", "strip_blue_level"};
  for (const char* old : old_level_keys) {
    if (!strcmp(key, old)) {
      return fail(err, err_cap, "no longer used: brightness comes with each command, e.g. IO5=26 (remove the line, "
                                "or 'unset' it on the USB console)", key), false;
    }
  }
  if (!strcmp(key, "strip")) {
    char up[8];
    size_t n = strlen(value);
    if (n >= sizeof up) return fail(err, err_cap, "must be COM0, COM1 or none", key), false;
    for (size_t i = 0; i <= n; i++) up[i] = (char)toupper((unsigned char)value[i]);
    if (!strcmp(up, "NONE") || up[0] == '\0') s->strip_port = kNoStrip;
    else if (!strcmp(up, "COM0")) s->strip_port = 0;
    else if (!strcmp(up, "COM1")) s->strip_port = 1;
    else return fail(err, err_cap, "must be COM0, COM1 or none", key), false;
    return true;
  }
  if (!strcmp(key, "strip_pixels")) {
    if (!parse_uint(value, kStripMaxPixels, &u) || u == 0) return fail(err, err_cap, "must be 1..300", key), false;
    s->strip_pixels = (uint16_t)u;
    return true;
  }
  if (!strcmp(key, "strip_layout")) {
    static Layout check;  // large: not on the stack
    char why[80];
    if (!layout_parse(value, kStripMaxPixels, &check, why, sizeof why)) return fail(err, err_cap, why, key), false;
    return set_text(s->strip_layout, sizeof s->strip_layout, value, key, err, err_cap);
  }
  if (!strcmp(key, "strip_mirror")) {
    return fail(err, err_cap, "replaced by strip_layout=fold (remove the line, or 'unset' it)", key), false;
  }
  if (!strcmp(key, "strip_order")) {
    uint8_t order[3];
    bool seen[3] = {false, false, false};
    if (strlen(value) != 3) return fail(err, err_cap, "must be RGB, GRB or another order of R, G and B", key), false;
    for (int i = 0; i < 3; i++) {
      char ch = (char)toupper((unsigned char)value[i]);
      int c = ch == 'R' ? 0 : ch == 'G' ? 1 : ch == 'B' ? 2 : -1;
      if (c < 0 || seen[c]) return fail(err, err_cap, "must be RGB, GRB or another order of R, G and B", key), false;
      seen[c] = true;
      order[i] = (uint8_t)c;
    }
    memcpy(s->strip_order, order, 3);
    return true;
  }
  if (!strcmp(key, "strip_timing")) {
    if (!parse_uint(value, kStripTimings - 1, &u)) return fail(err, err_cap, "must be 0..5", key), false;
    s->strip_timing = (uint8_t)u;
    return true;
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

bool strip_ready(const Settings& s, char* err, size_t err_cap) {
  if (err && err_cap) err[0] = '\0';
  if (s.strip_port == kNoStrip && s.strip_pixels == 0) return false;  // not configured: nothing to report
  if (s.strip_port == kNoStrip) return fail(err, err_cap, "not set (COM0 or COM1), but strip_pixels is", "strip"), false;
  if (s.strip_pixels == 0) return fail(err, err_cap, "not set", "strip_pixels"), false;
  return true;
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

// Index in keys[] of the key this settings line sets, or -1 for comments, blank lines and other keys.
static int line_key(const char* line, size_t len, const char* const keys[], size_t n) {
  size_t i = 0;
  while (i < len && isspace((unsigned char)line[i])) i++;
  if (i == len || line[i] == '#' || line[i] == ';') return -1;
  const char* eq = (const char*)memchr(line + i, '=', len - i);
  if (!eq) return -1;
  size_t end = (size_t)(eq - line);
  while (end > i && isspace((unsigned char)line[end - 1])) end--;
  for (size_t k = 0; k < n; k++) {
    if (strlen(keys[k]) == end - i && memcmp(line + i, keys[k], end - i) == 0) return (int)k;
  }
  return -1;
}

long set_keys(const char* in, size_t len, const char* const keys[], const char* const values[], size_t n, char* out,
              size_t cap) {
  if (n > 8 || cap == 0) return -1;
  bool found[8] = {false};
  bool crlf = false;
  size_t used = 0;
  auto put = [&](const char* p, size_t k) {
    if (used + k >= cap) return false;  // keep room for the terminator
    memcpy(out + used, p, k);
    used += k;
    return true;
  };
  auto put_setting = [&](size_t k) {
    return put(keys[k], strlen(keys[k])) && put("=", 1) && put(values[k], strlen(values[k]));
  };
  size_t i = 0;
  while (i < len) {
    size_t start = i;
    while (i < len && in[i] != '\n') i++;
    size_t text_end = i;
    if (i < len) i++;  // the line now spans [start, i), terminator included
    if (text_end > start && in[text_end - 1] == '\r') {
      text_end--;
      crlf = true;
    }
    int k = line_key(in + start, text_end - start, keys, n);
    if (k < 0) {
      if (!put(in + start, i - start)) return -1;
      continue;
    }
    found[k] = true;
    if (!values[k]) continue;  // removing this key: drop the line
    if (!put_setting((size_t)k) || !put(in + text_end, i - text_end)) return -1;
  }
  const char* eol = crlf ? "\r\n" : "\n";
  bool need_eol = len > 0 && in[len - 1] != '\n';
  for (size_t k = 0; k < n; k++) {
    if (found[k] || !values[k]) continue;
    if (need_eol && !put(eol, strlen(eol))) return -1;
    need_eol = false;
    if (!put_setting(k) || !put(eol, strlen(eol))) return -1;
  }
  out[used] = '\0';
  return (long)used;
}

const char* pin_name(int8_t pin) {
  return (pin >= 0 && pin < kPinCount) ? kPinNames[pin] : "none";
}

}  // namespace sl
