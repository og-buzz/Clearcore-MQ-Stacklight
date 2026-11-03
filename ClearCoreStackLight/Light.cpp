// Light.cpp — see Light.h.
#include "Light.h"

#include <ctype.h>
#include <stdio.h>
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

bool blink(Mode m, const Timing& t, uint32_t now) {
  switch (m) {
    case kOn: return true;
    case kSlow: return (now % ((uint32_t)t.slow_on + t.slow_off)) < t.slow_on;
    case kFast: return (now % ((uint32_t)t.fast_on + t.fast_off)) < t.fast_on;
    default: return false;
  }
}

}  // namespace

void clear(State* s) {
  for (uint8_t c = 0; c < kChannels; c++) s->mode[c] = kOff;
  s->fire = false;
}

bool parse_command(const char* text, size_t len, State* out, char* err, size_t err_cap) {
  State s;
  clear(&s);
  const char* p = text;
  const char* end = text + len;
  bool any = false;
  while (p < end) {
    const char* semi = (const char*)memchr(p, ';', (size_t)(end - p));
    const char* item_end = semi ? semi : end;
    const char* comma = (const char*)memchr(p, ',', (size_t)(item_end - p));
    char name[16], mode_txt[8] = "ON";
    if (!token(p, comma ? comma : item_end, name, sizeof name)) {
      copy_err(err, err_cap, "name too long:", "");
      return false;
    }
    if (comma && !token(comma + 1, item_end, mode_txt, sizeof mode_txt)) {
      copy_err(err, err_cap, "mode too long:", "");
      return false;
    }
    p = semi ? semi + 1 : end;
    if (name[0] == '\0') {
      if (comma) {
        copy_err(err, err_cap, "missing name before", mode_txt);
        return false;
      }
      continue;  // empty item, e.g. a trailing ';'
    }
    Mode mode;
    if (!parse_mode(mode_txt, &mode)) {
      copy_err(err, err_cap, "unknown mode", mode_txt);
      return false;
    }
    any = true;
    if (strcmp(name, "OFF") == 0) {
      if (comma) {
        copy_err(err, err_cap, "OFF takes no mode, got", mode_txt);
        return false;
      }
      clear(&s);
      continue;
    }
    if (strcmp(name, "FIRE") == 0) {
      s.fire = (mode != kOff);
      continue;
    }
    bool found = false;
    for (const Name& n : kNames) {
      if (strcmp(name, n.name) == 0) {
        for (uint8_t c = 0; c < kChannels; c++)
          if (n.channels & (1 << c)) s.mode[c] = mode;
        found = true;
        break;
      }
    }
    if (!found) {
      copy_err(err, err_cap, "unknown name", name);
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

uint8_t level_duty(uint16_t tenths) {
  if (tenths >= 1000) return 255;
  return (uint8_t)((tenths * 255u + 500u) / 1000u);
}

void describe(const State& s, char* out, size_t cap) {
  if (!cap) return;
  out[0] = '\0';
  size_t used = 0;
  auto add = [&](const char* name, Mode m) {
    int n = snprintf(out + used, cap - used, "%s%s,%s", used ? ";" : "", name, kModeNames[m]);
    if (n > 0) used += (size_t)n < cap - used ? (size_t)n : cap - used - 1;
  };
  // Group the lamp channels into the colour names they make, so the text reads naturally.
  Mode r = s.mode[kRed], g = s.mode[kGreen], b = s.mode[kBlue];
  if (r != kOff && r == g && g == b) {
    add("WHITE", r);
  } else {
    if (r != kOff && r == g) {
      add("YELLOW", r);
    } else {
      if (r != kOff) add("RED", r);
      if (g != kOff) add("GREEN", g);
    }
    if (b != kOff) add("BLUE", b);
  }
  if (s.mode[kAlarm] != kOff) add("ALARM", s.mode[kAlarm]);
  if (s.fire) {
    int n = snprintf(out + used, cap - used, "%sFIRE", used ? ";" : "");
    if (n > 0) used += (size_t)n < cap - used ? (size_t)n : cap - used - 1;
  }
  if (used == 0) snprintf(out, cap, "OFF");
}

}  // namespace sl
