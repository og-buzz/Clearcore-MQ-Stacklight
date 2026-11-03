// Light.cpp — see Light.h.
#include "Light.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace sl {

namespace {

const uint8_t R = 1 << kRed, G = 1 << kGreen, B = 1 << kBlue, A = 1 << kAlarm;

struct Name {
  const char* name;
  uint8_t channels;
};
const Name kNames[] = {
    {"RED", R},        {"GREEN", G},       {"BLUE", B},          {"YELLOW", R | G}, {"ORANGE", R | G},
    {"MAGENTA", R | B}, {"CYAN", G | B},   {"WHITE", R | G | B}, {"ALL", R | G | B}, {"ALARM", A},
};

const char* const kModeNames[] = {"OFF", "ON", "SLOW", "FAST"};

void copy_err(char* err, size_t cap, const char* msg, const char* token) {
  if (err && cap) snprintf(err, cap, "%s '%s'", msg, token);
}

// Copies [begin, end) into buf upper-cased and trimmed. Returns false if it doesn't fit.
bool token(const char* begin, const char* end, char* buf, size_t cap) {
  while (begin < end && isspace((unsigned char)*begin)) begin++;
  while (end > begin && isspace((unsigned char)end[-1])) end--;
  size_t n = (size_t)(end - begin);
  if (n >= cap) return false;
  for (size_t i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)begin[i]);
  buf[n] = '\0';
  return true;
}

bool parse_mode(const char* m, Mode* out) {
  for (uint8_t i = 0; i < 4; i++) {
    if (strcmp(m, kModeNames[i]) == 0) {
      *out = (Mode)i;
      return true;
    }
  }
  return false;
}

// "123" (digits only) into *out, at most max.
bool parse_index(const char* begin, const char* end, int max, int* out) {
  if (begin == end || end - begin > 4) return false;
  int v = 0;
  for (const char* p = begin; p < end; p++) {
    if (!isdigit((unsigned char)*p)) return false;
    v = v * 10 + (*p - '0');
  }
  if (v > max) return false;
  *out = v;
  return true;
}

// Splits "v1,v2,...[,MODE]" (already trimmed and upper-cased) into levels and a mode. Returns the level count,
// or -1 with err set.
int parse_values(char* values, uint16_t* levels, int max_levels, Mode* mode, char* err, size_t err_cap) {
  int n = 0;
  *mode = kOn;
  bool have_mode = false;
  for (char* v = values; v;) {
    char* comma = strchr(v, ',');
    if (comma) *comma = '\0';
    while (*v == ' ') v++;
    for (char* e = v + strlen(v); e > v && e[-1] == ' ';) *--e = '\0';
    if (have_mode) {
      copy_err(err, err_cap, "nothing may follow the mode, got", v);
      return -1;
    }
    uint16_t level;
    if (parse_level(v, &level)) {
      if (n == max_levels) {
        copy_err(err, err_cap, "too many values at", v);
        return -1;
      }
      levels[n++] = level;
    } else if (parse_mode(v, mode)) {
      have_mode = true;
    } else {
      copy_err(err, err_cap, "not a percent (0..100) or mode:", v);
      return -1;
    }
    v = comma ? comma + 1 : nullptr;
  }
  return n;
}

// An item with '=': IOn=... or STRIP[...]=... (text trimmed and upper-cased). Returns false with err set.
bool parse_assignment(char* item, State* s, char* err, size_t err_cap) {
  char* eq = strchr(item, '=');
  *eq = '\0';
  char* key = item;
  char* values = eq + 1;
  for (char* e = key + strlen(key); e > key && e[-1] == ' ';) *--e = '\0';
  uint16_t levels[kPixelChannels];
  Mode mode;

  if (key[0] == 'I' && key[1] == 'O') {
    int pin;
    if (!parse_index(key + 2, key + strlen(key), kIoPins - 1, &pin)) {
      copy_err(err, err_cap, "IO pins are IO0..IO5, got", key);
      return false;
    }
    int n = parse_values(values, levels, 1, &mode, err, err_cap);
    if (n < 0) return false;
    if (n != 1) {
      copy_err(err, err_cap, "needs one percent, e.g. IO5=26:", key);
      return false;
    }
    if (mode == kOff) levels[0] = 0, mode = kOn;  // off is off, whatever the level
    s->io_set[pin] = true;
    s->io[pin].level = levels[0];
    s->io[pin].mode = mode;
    return true;
  }

  if (!strncmp(key, "STRIP", 5)) {
    int first = 0, last = kLightMaxPixels - 1;
    const char* k = key + 5;
    if (*k == '[') {
      const char* close = strchr(k, ']');
      if (!close || close[1] != '\0') {
        copy_err(err, err_cap, "pixels are STRIP[n] or STRIP[a-b], got", key);
        return false;
      }
      const char* dash = (const char*)memchr(k + 1, '-', (size_t)(close - k - 1));
      if (!parse_index(k + 1, dash ? dash : close, kLightMaxPixels - 1, &first) ||
          (dash && !parse_index(dash + 1, close, kLightMaxPixels - 1, &last))) {
        copy_err(err, err_cap, "pixel numbers are 0..299, got", key);
        return false;
      }
      if (!dash) last = first;
      if (last < first) {
        copy_err(err, err_cap, "pixel range runs backwards:", key);
        return false;
      }
    } else if (*k != '\0') {
      copy_err(err, err_cap, "unknown item", key);
      return false;
    }
    // STRIP...=NAME[,SPEED][,COLOUR][,COLOUR]: an effect instead of levels.
    while (*values == ' ') values++;
    char name[16];
    size_t nl = 0;
    while (values[nl] && values[nl] != ',' && values[nl] != ' ' && nl < sizeof name - 1) name[nl] = values[nl], nl++;
    name[nl] = '\0';
    EffectId id = effect_by_name(name);
    if (id != kNoEffect) {
      if (s->fx_count == kMaxEffects) {
        copy_err(err, err_cap, "at most 8 effects in one command, at", key);
        return false;
      }
      EffectSpec& e = s->fx[s->fx_count];
      memset(&e, 0, sizeof e);
      e.id = id;
      e.speed = 1;
      e.level = 1000;
      e.first = (int16_t)first;
      e.last = (int16_t)last;
      for (char* tok = values + nl; *tok;) {
        while (*tok == ',' || *tok == ' ') tok++;
        if (!*tok) break;
        char* end = tok;
        while (*end && *end != ',') end++;
        char saved = *end;
        *end = '\0';
        for (char* e2 = end; e2 > tok && e2[-1] == ' ';) *--e2 = '\0';
        Mode m;
        uint16_t lvl;
        size_t tl = strlen(tok);
        int num = 0;
        bool digits = tl > 0;
        for (size_t q = 0; q < tl && digits; q++) digits = (tok[q] >= '0' && tok[q] <= '9') || (q && tok[q] == 'K') ||
                                                           (q && (tok[q] == 'H' || tok[q] == 'Z'));
        if (parse_mode(tok, &m)) {
          if (m == kOff) {
            copy_err(err, err_cap, "effects take SLOW, ON or FAST, got", tok);
            return false;
          }
          e.speed = m == kSlow ? 0 : m == kFast ? 2 : 1;
        } else if (parse_level(tok, &lvl)) {
          e.level = lvl;  // dimmer
        } else if (digits && tl >= 2 && tok[tl - 1] == 'K' && sscanf(tok, "%d", &num) == 1 && num >= 1000 && num <= 10000) {
          e.kelvin = (uint16_t)num;
        } else if (digits && tl >= 3 && !strcmp(tok + tl - 2, "HZ") && sscanf(tok, "%d", &num) == 1 && num >= 1 && num <= 60) {
          e.hz = (uint8_t)num;
        } else if (e.ncolors < 2 && parse_effect_color(tok, e.color[e.ncolors])) {
          e.ncolors++;
        } else {
          copy_err(err, err_cap, "not a speed, colour, dimmer (%), kelvin (4000K) or rate (25HZ):", tok);
          return false;
        }
        *end = saved;
        tok = end;
      }
      s->fx_count++;
      s->strip_set = true;
      for (int p = first; p <= last; p++) {
        memset(s->pixel[p], 0, sizeof s->pixel[p]);
        s->pixel_mode[p] = kOn;
        s->pixel_fx[p] = s->fx_count;
      }
      return true;
    }
    int n = parse_values(values, levels, kPixelChannels, &mode, err, err_cap);
    if (n < 0) return false;
    if (n != 3 && n != 5) {
      copy_err(err, err_cap, "needs red,green,blue or red,green,blue,warm,cool percents:", key);
      return false;
    }
    if (n == 3) levels[3] = levels[4] = 0;
    if (mode == kOff) memset(levels, 0, sizeof levels), mode = kOn;
    bool dark = true;
    for (int c = 0; c < kPixelChannels; c++) dark = dark && levels[c] == 0;
    if (dark) mode = kOn;  // a dark pixel is the same in any mode
    s->strip_set = true;
    for (int p = first; p <= last; p++) {
      memcpy(s->pixel[p], levels, sizeof levels);
      s->pixel_mode[p] = mode;
      s->pixel_fx[p] = 0;
    }
    return true;
  }

  copy_err(err, err_cap, "unknown item", key);
  return false;
}

// Appends to a bounded string; on overflow the text is cut off (and stays terminated).
struct Out {
  char* buf;
  size_t cap, used;
  void add(const char* fmt, const char* a = "", const char* b = "") {
    if (!cap || used >= cap - 1) return;
    int n = snprintf(buf + used, cap - used, fmt, a, b);
    if (n > 0) used += (size_t)n < cap - used ? (size_t)n : cap - used - 1;
  }
  void sep() { add(used ? ";" : ""); }
};

void add_levels(Out& o, const uint16_t* levels, int n, Mode mode) {
  for (int c = 0; c < n; c++) {
    char v[8];
    format_level(levels[c], v, sizeof v);
    o.add(c ? ",%s" : "%s", v);
  }
  if (mode != kOn) o.add(",%s", kModeNames[mode]);
}

}  // namespace

void clear(State* s) {
  memset(s, 0, sizeof *s);  // everything off; also zeroes padding, so equal states compare equal
  for (int i = 0; i < kIoPins; i++) s->io[i].mode = kOn;
  for (int p = 0; p < kLightMaxPixels; p++) s->pixel_mode[p] = kOn;
}

bool parse_command(const char* text, size_t len, State* out, char* err, size_t err_cap) {
  static State s;  // large: not on the stack
  clear(&s);
  const char* p = text;
  const char* end = text + len;
  bool any = false;
  while (p < end) {
    const char* semi = (const char*)memchr(p, ';', (size_t)(end - p));
    const char* item_end = semi ? semi : end;
    char item[96];
    if (!token(p, item_end, item, sizeof item)) {
      copy_err(err, err_cap, "item too long:", "");
      return false;
    }
    p = semi ? semi + 1 : end;
    if (item[0] == '\0') continue;  // empty item, e.g. a trailing ';'
    any = true;
    if (strchr(item, '=')) {
      if (!parse_assignment(item, &s, err, err_cap)) return false;
      continue;
    }
    char* comma = strchr(item, ',');
    const char* mode_txt = "ON";
    if (comma) {
      *comma = '\0';
      mode_txt = comma + 1;
      while (*mode_txt == ' ') mode_txt++;
      for (char* e = comma; e > item && e[-1] == ' ';) *--e = '\0';
    }
    if (item[0] == '\0') {
      copy_err(err, err_cap, "missing name before", mode_txt);
      return false;
    }
    Mode mode;
    if (!parse_mode(mode_txt, &mode)) {
      copy_err(err, err_cap, "unknown mode", mode_txt);
      return false;
    }
    if (strcmp(item, "OFF") == 0) {
      if (comma) {
        copy_err(err, err_cap, "OFF takes no mode, got", mode_txt);
        return false;
      }
      clear(&s);
      continue;
    }
    if (strcmp(item, "FIRE") == 0) {
      s.fire = (mode != kOff);
      continue;
    }
    bool found = false;
    for (const Name& n : kNames) {
      if (strcmp(item, n.name) == 0) {
        for (uint8_t c = 0; c < kChannels; c++)
          if (n.channels & (1 << c)) s.mode[c] = mode;
        found = true;
        break;
      }
    }
    if (!found) {
      copy_err(err, err_cap, "unknown name", item);
      return false;
    }
  }
  if (!any) {
    copy_err(err, err_cap, "empty command", "");
    return false;
  }
  *out = s;
  return true;
}

bool blink(Mode m, const Timing& t, uint32_t now) {
  switch (m) {
    case kOn: return true;
    case kSlow: return (now % ((uint32_t)t.slow_on + t.slow_off)) < t.slow_on;
    case kFast: return (now % ((uint32_t)t.fast_on + t.fast_off)) < t.fast_on;
    default: return false;
  }
}

void lit(const State& s, const Timing& t, uint32_t now, bool out[kChannels]) {
  for (uint8_t c = 0; c < kChannels; c++) out[c] = blink(s.mode[c], t, now);
  if (s.fire) {
    // Alternate red and red + green every slow period; alarm fast. Blue is left as commanded.
    uint32_t period = (uint32_t)t.slow_on + t.slow_off;
    bool second_half = (now % (2 * period)) >= period;
    out[kRed] = true;
    out[kGreen] = second_half;
    out[kAlarm] = blink(kFast, t, now);
  }
}

void io_duty(const State& s, const Timing& t, uint32_t now, const int8_t pin[kChannels], uint8_t duty[kIoPins]) {
  bool on[kChannels];
  lit(s, t, now, on);
  memset(duty, 0, kIoPins);
  for (uint8_t c = 0; c < kChannels; c++)
    if (pin[c] >= 0 && pin[c] < kIoPins && on[c]) duty[pin[c]] = 255;
  for (int p = 0; p < kIoPins; p++)
    if (s.io_set[p]) duty[p] = blink(s.io[p].mode, t, now) ? level_duty(s.io[p].level) : 0;
}

uint8_t level_duty(uint16_t tenths) {
  if (tenths >= 1000) return 255;
  return (uint8_t)((tenths * 255u + 500u) / 1000u);
}

bool parse_level(const char* text, uint16_t* tenths) {
  unsigned whole = 0, frac = 0;
  const char* p = text;
  if (!isdigit((unsigned char)*p)) return false;
  for (; isdigit((unsigned char)*p); p++) {
    whole = whole * 10 + (unsigned)(*p - '0');
    if (whole > 100) return false;
  }
  if (*p == '.') {
    p++;
    if (!isdigit((unsigned char)*p)) return false;
    frac = (unsigned)(*p++ - '0');
  }
  if (*p != '\0') return false;  // more than one decimal, or junk
  unsigned t = whole * 10 + frac;
  if (t > 1000) return false;
  *tenths = (uint16_t)t;
  return true;
}

void format_level(uint16_t tenths, char* out, size_t cap) {
  if (tenths % 10) snprintf(out, cap, "%u.%u", tenths / 10u, tenths % 10u);
  else snprintf(out, cap, "%u", tenths / 10u);
}

void describe(const State& s, char* buf, size_t cap) {
  if (!cap) return;
  buf[0] = '\0';
  Out o{buf, cap, 0};
  auto named = [&](const char* name, Mode m) {
    o.sep();
    o.add("%s,%s", name, kModeNames[m]);
  };
  // Group the lamp channels into the colour names they make, so the text reads naturally.
  Mode r = s.mode[kRed], g = s.mode[kGreen], b = s.mode[kBlue];
  if (r != kOff && r == g && g == b) {
    named("WHITE", r);
  } else {
    if (r != kOff && r == g) {
      named("YELLOW", r);
    } else {
      if (r != kOff) named("RED", r);
      if (g != kOff) named("GREEN", g);
    }
    if (b != kOff) named("BLUE", b);
  }
  if (s.mode[kAlarm] != kOff) named("ALARM", s.mode[kAlarm]);
  if (s.fire) {
    o.sep();
    o.add("FIRE");
  }
  for (int p = 0; p < kIoPins; p++) {
    if (!s.io_set[p]) continue;
    char key[16];
    snprintf(key, sizeof key, "IO%d", p);
    o.sep();
    o.add("%s=", key);
    add_levels(o, &s.io[p].level, 1, s.io[p].mode);
  }
  if (s.strip_set) {
    // Runs of identical pixels; dark runs are left out (a STRIP item starts from a dark strip).
    bool wrote = false;
    for (int first = 0; first < kLightMaxPixels;) {
      int last = first;
      while (last + 1 < kLightMaxPixels && s.pixel_mode[last + 1] == s.pixel_mode[first] &&
             s.pixel_fx[last + 1] == s.pixel_fx[first] &&
             !memcmp(s.pixel[last + 1], s.pixel[first], sizeof s.pixel[first]))
        last++;
      bool dark = s.pixel_fx[first] == 0;
      for (int c = 0; c < kPixelChannels; c++) dark = dark && s.pixel[first][c] == 0;
      if (!dark) {
        char a[12], z[12];
        snprintf(a, sizeof a, "%d", first);
        snprintf(z, sizeof z, "%d", last);
        o.sep();
        if (first == 0 && last == kLightMaxPixels - 1) o.add("STRIP=");
        else if (first == last) o.add("STRIP[%s]=", a);
        else o.add("STRIP[%s-%s]=", a, z);
        if (s.pixel_fx[first]) {
          const EffectSpec& e = s.fx[s.pixel_fx[first] - 1];
          o.add("%s", effect_name(e.id));
          if (e.speed != 1) o.add(",%s", e.speed == 0 ? "SLOW" : "FAST");
          char num[16];
          if (e.level != 1000) {
            format_level(e.level, num, sizeof num);
            o.add(",%s", num);
          }
          if (e.kelvin) {
            snprintf(num, sizeof num, "%uK", e.kelvin);
            o.add(",%s", num);
          }
          if (e.hz) {
            snprintf(num, sizeof num, "%uHZ", e.hz);
            o.add(",%s", num);
          }
          for (int k = 0; k < e.ncolors; k++) {
            for (int c = 0; c < kEffectChannels; c++) {
              char v[8];
              format_level(e.color[k][c], v, sizeof v);
              o.add(c ? "/%s" : ",%s", v);
            }
          }
        } else {
          add_levels(o, s.pixel[first], kPixelChannels, s.pixel_mode[first]);
        }
        wrote = true;
      }
      first = last + 1;
    }
    if (!wrote) {
      o.sep();
      o.add("STRIP=0,0,0,0,0");
    }
  }
  if (o.used == 0) snprintf(buf, cap, "OFF");
}

}  // namespace sl
