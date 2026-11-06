// Strip.cpp — see Strip.h.
#include "Strip.h"

#include <stdio.h>
#include <string.h>

namespace sl {

namespace {

const StripTiming kTimings[kStripTimings] = {
    {5000000, 7, 0x60, 0x78, false},      // 1100000 / 1111000
    {5000000, 6, 0x20, 0x38, false},      // 100000 / 111000
    {2500000, 3, 0x04, 0x06, false},      // 100 / 110
    {5000000, 7, 0x40, 0x78, false},      // 1000000 / 1111000
    {12000000, 13, 0x1E00, 0x1FF0, true},  // 1111000000000 / 1111111110000
    {8000000, 9, 0x1C0, 0x1F8, true},      // 111000000 / 111111000
};

int high_bits(uint16_t pattern, uint8_t bits) {
  int n = 0;
  for (int b = bits - 1; b >= 0 && ((pattern >> b) & 1); b--) n++;
  return n;
}

// One data byte as t.bits SPI bytes (8 x t.bits bits), most significant bit first.
void encode_byte(uint8_t v, const StripTiming& t, uint8_t* out) {
  memset(out, 0, t.bits);
  int pos = 0;  // bit position in out, 0 = MSB of out[0]
  for (int i = 7; i >= 0; i--) {
    uint16_t pattern = ((v >> i) & 1) ? t.one : t.zero;
    for (int b = t.bits - 1; b >= 0; b--, pos++)
      if ((pattern >> b) & 1) out[pos / 8] |= (uint8_t)(0x80 >> (pos % 8));
  }
}

int clamp_pixels(int pixels) { return pixels < 0 ? 0 : pixels > kStripMaxPixels ? kStripMaxPixels : pixels; }

}  // namespace

const StripTiming& strip_timing(int index) { return kTimings[(index >= 0 && index < kStripTimings) ? index : 0]; }

size_t strip_reset_bytes(const StripTiming& t) {
  return (size_t)(((uint64_t)kStripResetUs * t.spi_hz / 1000000u + 7) / 8);
}

size_t strip_frame_bytes(int pixels, const StripTiming& t) {
  return kStripLeadBytes + (size_t)clamp_pixels(pixels) * kStripChannels * t.bits + strip_reset_bytes(t);
}

void strip_pulses(const StripTiming& t, int ns[4]) {
  auto to_ns = [&](int spi_bits) { return (int)(((uint64_t)spi_bits * 1000000000u + t.spi_hz / 2) / t.spi_hz); };
  int h0 = high_bits(t.zero, t.bits), h1 = high_bits(t.one, t.bits);
  ns[0] = to_ns(h0);
  ns[1] = to_ns(t.bits) - ns[0];
  ns[2] = to_ns(h1);
  ns[3] = to_ns(t.bits) - ns[2];
}

namespace {

bool word(const char* a, const char* b) {  // case-insensitive equality
  for (; *a && *b; a++, b++)
    if ((*a | 0x20) != (*b | 0x20)) return false;
  return *a == *b;
}

bool read_index(const char*& p, int* out) {
  if (*p < '0' || *p > '9') return false;
  int v = 0;
  while (*p >= '0' && *p <= '9' && v < 10000) v = v * 10 + (*p++ - '0');
  *out = v;
  return true;
}

}  // namespace

bool layout_parse(const char* text, int pixels, Layout* L, char* err, size_t cap) {
  if (pixels < 0) pixels = 0;
  if (pixels > kStripMaxPixels) pixels = kStripMaxPixels;
  for (int i = 0; i < kStripMaxPixels; i++) L->pos[i] = -1;
  L->positions = 0;
  L->wrap = false;
  if (!*text || word(text, "line") || word(text, "ring")) {
    for (int i = 0; i < pixels; i++) L->pos[i] = (int16_t)i;
    L->positions = pixels;
    L->wrap = word(text, "ring");
    return true;
  }
  if (word(text, "fold")) {
    const int half = (pixels + 1) / 2;
    for (int i = 0; i < pixels; i++) L->pos[i] = (int16_t)(i < half ? i : pixels - 1 - i);
    L->positions = half;
    return true;
  }
  // A map: legs separated by '|', each a ','-separated list of ranges a-b (either direction) or single pixels.
  const char* p = text;
  int position = 0;
  while (true) {
    while (*p == ' ') p++;
    int a, b;
    if (!read_index(p, &a)) {
      if (err && cap) snprintf(err, cap, "strip_layout: expected line, ring, fold or ranges like 0-24|49-25");
      return false;
    }
    b = a;
    if (*p == '-') {
      p++;
      if (!read_index(p, &b)) {
        if (err && cap) snprintf(err, cap, "strip_layout: a range needs two pixel numbers, e.g. 49-25");
        return false;
      }
    }
    if (a >= pixels || b >= pixels) {
      if (err && cap) snprintf(err, cap, "strip_layout: pixel %d is past the strip's %d pixels", a > b ? a : b, pixels);
      return false;
    }
    for (int k = a;; k += (b >= a ? 1 : -1)) {
      if (L->pos[k] >= 0) {
        if (err && cap) snprintf(err, cap, "strip_layout: pixel %d is in the map twice", k);
        return false;
      }
      L->pos[k] = (int16_t)position++;
      if (position > L->positions) L->positions = position;
      if (k == b) break;
    }
    while (*p == ' ') p++;
    if (*p == ',') {
      p++;
    } else if (*p == '|') {
      p++;
      position = 0;  // the next leg lies beside the first
    } else if (*p == '\0') {
      return true;
    } else {
      if (err && cap) snprintf(err, cap, "strip_layout: unexpected '%c'", *p);
      return false;
    }
  }
}

void strip_effects_reset(StripEffects* fx, uint32_t seed) {
  for (int k = 0; k <= kMaxEffects; k++) effect_reset(&fx->st[k], seed * 2654435761u + (uint32_t)k * 40503u + 1u);
}

namespace {

// Effects drawn on the physical pixels instead of the layout's positions, so they don't repeat on every leg:
// SPARKLE's flashes are random along the whole strand.
bool per_pixel(EffectId id) { return id == kSparkleFx; }

// One pixel's channels (red, green, blue, warm, cool) into wire order.
void put(uint8_t* o, const uint8_t* c, const uint8_t order[3]) {
  for (int k = 0; k < 3; k++) o[k] = c[order[k] < 3 ? order[k] : k];
  o[3] = c[3];
  o[4] = c[4];
}

}  // namespace

void strip_render(const State& s, const Timing& t, uint32_t now, const Layout& L, const uint8_t order[3],
                  StripEffects* fx, int pixels, uint8_t* out) {
  static uint8_t pos_col[kStripMaxPixels][kStripChannels];
  static uint8_t tmp[kStripMaxPixels][kStripChannels];
  const int P = L.positions < kStripMaxPixels ? L.positions : kStripMaxPixels;
  bool lit_now[kChannels];
  lit(s, t, now, lit_now);
  for (int p = 0; p < P; p++) {
    uint8_t* c = pos_col[p];
    if (!s.strip_set) {  // the colour names, full brightness
      c[0] = lit_now[kRed] ? 255 : 0;
      c[1] = lit_now[kGreen] ? 255 : 0;
      c[2] = lit_now[kBlue] ? 255 : 0;
      c[3] = c[4] = 0;
    } else if (!s.pixel_fx[p]) {
      bool on = blink(s.pixel_mode[p], t, now);
      for (int ch = 0; ch < kStripChannels; ch++) c[ch] = on ? level_duty(s.pixel[p][ch]) : 0;
    }
  }
  if (!s.strip_set && s.fire) {  // the FIRE name burns the whole strip
    EffectSpec spec;
    memset(&spec, 0, sizeof spec);
    spec.id = kFireFx;
    spec.speed = 1;
    spec.level = 1000;
    spec.last = (int16_t)(P - 1);
    effect_render(spec, &fx->st[kMaxEffects], P, L.wrap, now, pos_col);
  } else if (s.strip_set) {
    for (int k = 0; k < s.fx_count; k++) {
      const EffectSpec& e = s.fx[k];
      const int first = e.first, last = e.last < P - 1 ? e.last : P - 1;
      if (e.physical || first > last || per_pixel(e.id)) continue;
      effect_render(e, &fx->st[k], last - first + 1, L.wrap && first == 0 && last == P - 1, now, tmp);
      for (int p = first; p <= last; p++)
        if (s.pixel_fx[p] == k + 1) memcpy(pos_col[p], tmp[p - first], kStripChannels);
    }
  }
  if (pixels > kStripMaxPixels) pixels = kStripMaxPixels;
  for (int i = 0; i < pixels; i++) {
    uint8_t* o = out + (size_t)i * kStripChannels;
    const int p = L.pos[i];
    if (p < 0 || p >= P) {
      memset(o, 0, kStripChannels);
      continue;
    }
    put(o, pos_col[p], order);
  }
  // Per-pixel effects: over every physical pixel whose position the effect still owns (later items keep theirs).
  for (int k = 0; s.strip_set && k < s.fx_count; k++) {
    const EffectSpec& e = s.fx[k];
    if (e.physical || !per_pixel(e.id)) continue;
    static int16_t idx[kStripMaxPixels];
    int m = 0;
    for (int i = 0; i < pixels; i++) {
      const int p = L.pos[i];
      if (p >= e.first && p <= e.last && p < P && s.pixel_fx[p] == k + 1) idx[m++] = (int16_t)i;
    }
    if (!m) continue;
    effect_render(e, &fx->st[k], m, false, now, tmp);
    for (int j = 0; j < m; j++) put(out + (size_t)idx[j] * kStripChannels, tmp[j], order);
  }
  if (!s.strip_set) return;
  // PIX items: physical pixels, over everything above. Levels first, then effects from their first pixel to their
  // last (either way along the strand), over the pixels they still own.
  for (int i = 0; i < pixels; i++) {
    if (!s.px_set[i] || s.px_fx[i]) continue;
    uint8_t c[kStripChannels];
    const bool on = blink(s.px_mode[i], t, now);
    for (int ch = 0; ch < kStripChannels; ch++) c[ch] = on ? level_duty(s.px[i][ch]) : 0;
    put(out + (size_t)i * kStripChannels, c, order);
  }
  for (int k = 0; k < s.fx_count; k++) {
    const EffectSpec& e = s.fx[k];
    if (!e.physical) continue;
    static int16_t idx[kStripMaxPixels];
    int m = 0;
    const int step = e.first <= e.last ? 1 : -1;
    for (int p = e.first;; p += step) {
      if (p < pixels && s.px_fx[p] == k + 1) idx[m++] = (int16_t)p;
      if (p == e.last) break;
    }
    if (!m) continue;
    effect_render(e, &fx->st[k], m, false, now, tmp);
    for (int j = 0; j < m; j++) put(out + (size_t)idx[j] * kStripChannels, tmp[j], order);
  }
}

void strip_encode(const uint8_t* pixel_bytes, int pixels, const StripTiming& t, uint8_t* out) {
  pixels = clamp_pixels(pixels);
  memset(out, 0, kStripLeadBytes);  // lead-in: line low
  out += kStripLeadBytes;
  for (int b = 0; b < pixels * kStripChannels; b++) encode_byte(pixel_bytes[b], t, out + (size_t)b * t.bits);
  memset(out + (size_t)pixels * kStripChannels * t.bits, 0, strip_reset_bytes(t));  // reset: line low
}

void strip_encode_solid(const uint8_t color[kStripChannels], int pixels, const StripTiming& t, uint8_t* out) {
  pixels = clamp_pixels(pixels);
  const size_t per_pixel = (size_t)kStripChannels * t.bits;
  uint8_t pixel[kStripChannels * kStripMaxBits];
  for (int c = 0; c < kStripChannels; c++) encode_byte(color[c], t, pixel + c * t.bits);
  memset(out, 0, kStripLeadBytes);  // lead-in: line low
  out += kStripLeadBytes;
  for (int p = 0; p < pixels; p++) memcpy(out + (size_t)p * per_pixel, pixel, per_pixel);
  memset(out + (size_t)pixels * per_pixel, 0, strip_reset_bytes(t));  // reset: line low
}

size_t strip_encode_raw(const uint8_t* bytes, int n, int pixels, const StripTiming& t, uint8_t* out, size_t cap) {
  if (n < 1 || n > 8) return 0;
  pixels = clamp_pixels(pixels);
  const size_t per_pixel = (size_t)n * t.bits;
  const size_t len = kStripLeadBytes + (size_t)pixels * per_pixel + strip_reset_bytes(t);
  if (len > cap) return 0;
  uint8_t pixel[8 * kStripMaxBits];
  for (int c = 0; c < n; c++) encode_byte(bytes[c], t, pixel + c * t.bits);
  memset(out, 0, kStripLeadBytes);
  uint8_t* data = out + kStripLeadBytes;
  for (int p = 0; p < pixels; p++) memcpy(data + (size_t)p * per_pixel, pixel, per_pixel);
  memset(data + (size_t)pixels * per_pixel, 0, strip_reset_bytes(t));
  return len;
}

}  // namespace sl
