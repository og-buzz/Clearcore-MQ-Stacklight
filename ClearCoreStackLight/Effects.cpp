// Effects.cpp — see Effects.h.
#include "Effects.h"

#include <math.h>
#include <string.h>

#include "Light.h"

namespace sl {

namespace {

struct Named {
  const char* name;
  EffectId id;
};
const Named kEffects[] = {
    {"FIRE", kFireFx},         {"CANDLE", kCandleFx},     {"LAVA", kLavaFx},       {"SUNRISE", kSunriseFx},
    {"WATER", kWaterFx},       {"RAIN", kRainFx},         {"ICE", kIceFx},         {"AURORA", kAuroraFx},
    {"LIGHTNING", kLightningFx}, {"SPACE", kSpaceFx},     {"NEBULA", kNebulaFx},   {"METEOR", kMeteorFx},
    {"LASER", kLaserFx},       {"LASERS", kLasersFx},     {"SCANNER", kScannerFx}, {"DISCO", kDiscoFx},
    {"STROBE", kStrobeFx},     {"POLICE", kPoliceFx},     {"CONFETTI", kConfettiFx}, {"RAINBOW", kRainbowFx},
    {"PLASMA", kPlasmaFx},     {"COMET", kCometFx},       {"THEATER", kTheaterFx}, {"WIPE", kWipeFx},
    {"MATRIX", kMatrixFx},     {"BREATHE", kBreatheFx},   {"HEARTBEAT", kHeartbeatFx}, {"TWINKLE", kTwinkleFx},
    {"FIREPLACE", kFireplaceFx}, {"LAKE", kLakeFx},       {"WIGWAG", kWigwagFx},   {"QUAD", kQuadFx},
    {"BEACON", kBeaconFx},     {"HAZARD", kHazardFx},     {"LIGHT", kLightFx},     {"DAYLIGHT", kDaylightFx},
    {"NEUTRAL", kNeutralFx},   {"WARMLIGHT", kWarmLightFx},
};

struct NamedColor {
  const char* name;
  uint16_t c[kEffectChannels];  // tenths of a percent
};
const NamedColor kColors[] = {
    {"RED", {1000, 0, 0, 0, 0}},      {"ORANGE", {1000, 250, 0, 0, 0}}, {"AMBER", {1000, 450, 0, 0, 0}},
    {"YELLOW", {1000, 600, 0, 0, 0}}, {"GREEN", {0, 1000, 0, 0, 0}},    {"CYAN", {0, 1000, 1000, 0, 0}},
    {"BLUE", {0, 0, 1000, 0, 0}},     {"PURPLE", {500, 0, 1000, 0, 0}}, {"MAGENTA", {1000, 0, 1000, 0, 0}},
    {"PINK", {1000, 200, 500, 0, 0}}, {"WARM", {0, 0, 0, 1000, 0}},     {"COOL", {0, 0, 0, 0, 1000}},
    {"WHITE", {0, 0, 0, 1000, 1000}},
};

const float kPi = 3.14159265f;

// --- small helpers ------------------------------------------------------------------------------------------

uint32_t rnd(EffectState* s) {  // xorshift32
  s->rng ^= s->rng << 13;
  s->rng ^= s->rng >> 17;
  s->rng ^= s->rng << 5;
  return s->rng;
}
uint8_t rnd8(EffectState* s) { return (uint8_t)(rnd(s) >> 24); }
float frand(EffectState* s) { return (float)(rnd(s) >> 8) / 16777216.0f; }

float speed_of(const EffectSpec& sp) { return sp.speed == 0 ? 0.5f : sp.speed == 2 ? 2.0f : 1.0f; }

float hash1(int32_t x) {
  uint32_t h = (uint32_t)x * 374761393u + 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
float hash2(int32_t x, int32_t y) { return hash1(x * 7919 + y * 104729); }

// Smooth 1-D value noise, 0..1.
float noise1(float x) {
  float fl = floorf(x);
  int32_t i = (int32_t)fl;
  float f = x - fl;
  float u = f * f * (3.0f - 2.0f * f);
  return hash1(i) * (1.0f - u) + hash1(i + 1) * u;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

uint8_t to8(float v) { return v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t)(v + 0.5f); }

// out (0..255 floats) from hue 0..1 (wraps), saturation and value 0..1.
void hsv(float h, float s, float v, float* out) {
  h -= floorf(h);
  float i = floorf(h * 6.0f), f = h * 6.0f - i;
  float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  float r, g, b;
  switch ((int)i % 6) {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    default: r = v, g = p, b = q; break;
  }
  out[0] = r * 255, out[1] = g * 255, out[2] = b * 255, out[3] = 0, out[4] = 0;
}

// The spec's colour k (0 or 1) as 0..255 floats, or the default (tenths) if it wasn't given.
void color_of(const EffectSpec& sp, int k, const uint16_t def[kEffectChannels], float out[kEffectChannels]) {
  const uint16_t* c = sp.ncolors > k ? sp.color[k] : def;
  for (int ch = 0; ch < kEffectChannels; ch++) out[ch] = (float)c[ch] * 255.0f / 1000.0f;
}

struct Px {  // accumulates one position, then clamps
  float c[kEffectChannels] = {0, 0, 0, 0, 0};
  void add(const float* col, float k) {
    for (int ch = 0; ch < kEffectChannels; ch++) c[ch] += col[ch] * k;
  }
  void add5(float r, float g, float b, float ww, float cw) {
    c[0] += r, c[1] += g, c[2] += b, c[3] += ww, c[4] += cw;
  }
  void store(uint8_t* out) const {
    for (int ch = 0; ch < kEffectChannels; ch++) out[ch] = to8(c[ch]);
  }
};

// Fixed-step simulation: how many steps of step_ms are due (at most 10 after a pause).
int due_steps(EffectState* st, uint32_t now, uint32_t step_ms) {
  if (!st->started || (int32_t)(now - st->next_ms) > (int32_t)(10 * step_ms)) {
    st->next_ms = now;
    st->started = true;
  }
  int n = 0;
  while ((int32_t)(now - st->next_ms) >= 0 && n < 10) {
    st->next_ms += step_ms;
    n++;
  }
  return n;
}

// A beam at position x (float) moving in direction dir: full at the head, fading tail behind it.
float beam(float p, float x, float dir, float tail) {
  float d = (x - p) * dir;  // > 0: behind the head
  if (d >= 0.0f) return d > tail * 4 ? 0.0f : expf(-d / tail);
  return d > -0.8f ? 1.0f + d / 0.8f : 0.0f;
}

// Ping-pong (or, on a ring, round and round) position along count positions at `rate` positions per second.
float sweep(uint32_t now, float rate, int count, bool wrap, float phase, float* dir) {
  if (count <= 1) {
    *dir = 1;
    return 0;
  }
  float travel = (float)now / 1000.0f * rate + phase;
  if (wrap) {
    *dir = 1;
    return fmodf(travel, (float)count);
  }
  float span = (float)(count - 1), m = fmodf(travel, 2 * span);
  *dir = m < span ? 1.0f : -1.0f;
  return m < span ? m : 2 * span - m;
}

// --- fire ------------------------------------------------------------------------------------------------------

void burn(EffectState* st, int n, uint8_t cooling, uint8_t sparking) {
  uint8_t* h = st->a;
  const unsigned cool_max = (unsigned)cooling * 10u / (unsigned)n + 2u;
  for (int k = 0; k < n; k++) {
    uint8_t c = (uint8_t)(rnd8(st) % cool_max);
    h[k] = c >= h[k] ? 0 : (uint8_t)(h[k] - c);
  }
  for (int k = n - 1; k >= 2; k--) h[k] = (uint8_t)((h[k - 1] + h[k - 2] + h[k - 2]) / 3);
  if (rnd8(st) < sparking) {
    int y = rnd8(st) % (n < 7 ? n : 7);
    unsigned v = h[y] + 160u + (unsigned)(rnd8(st) % 96u);
    h[y] = (uint8_t)(v > 255 ? 255 : v);
  }
  for (int k = 0; k < n; k++) {  // tints wander a little each step
    int v = st->t[k] + (int)(rnd8(st) % 9) - 4;
    st->t[k] = (int8_t)(v < -100 ? -100 : v > 100 ? 100 : v);
  }
}

void fire(const EffectSpec& sp, EffectState* st, int n, uint32_t now, uint8_t (*out)[kEffectChannels], bool calm) {
  uint32_t step_ms = sp.speed == 0 ? 30 : sp.speed == 2 ? 8 : 16;
  uint8_t cooling = sp.speed == 2 ? 70 : 55, sparking = sp.speed == 2 ? 190 : sp.speed == 0 ? 90 : 120;
  if (calm) step_ms *= 3, cooling = 45, sparking = 70;  // a fireplace: slow, low embers
  for (int s = due_steps(st, now, step_ms); s > 0; s--) burn(st, n, cooling, sparking);
  for (int p = 0; p < n; p++) {
    // The fireplace stays below white heat and burns at half brightness, with a warm-white glow underneath.
    uint8_t heat = calm ? (uint8_t)(st->a[p] * 200 / 255) : st->a[p];
    fire_color(heat, st->t[p], out[p]);
    if (calm) {
      for (int c = 0; c < kEffectChannels; c++) out[p][c] = (uint8_t)(out[p][c] / 2);
      out[p][3] = (uint8_t)(out[p][3] + 12 < 255 ? out[p][3] + 12 : 255);
    }
  }
}

// Whites for a colour temperature: the strip's 3000 K (warm) and 6500 K (cool) LEDs mixed; below 3000 K some
// red and a little green warm it further.
void white_light(float kelvin, float level, float* c) {
  kelvin = clampf(kelvin, 2200.0f, 6500.0f);
  float w = clampf((kelvin - 3000.0f) / 3500.0f, 0.0f, 1.0f);
  float extra = kelvin < 3000.0f ? (3000.0f - kelvin) / 800.0f : 0.0f;  // 0..1 between 3000 and 2200 K
  c[0] = 255 * level * 0.35f * extra, c[1] = 255 * level * 0.08f * extra, c[2] = 0;
  c[3] = 255 * level * (1.0f - w), c[4] = 255 * level * w;
}

// Two halves flashing: first half on during [0, on) of each pulse in the left half-cycle, etc. Returns which
// half is lit (0 none, 1 first, 2 second) for a cycle phase 0..1 with `pulses` flashes per side.
int flasher(float phase, int pulses, float duty) {
  bool second = phase >= 0.5f;
  float sub = (second ? phase - 0.5f : phase) * 2.0f * (float)pulses;  // 0..pulses
  float within = sub - floorf(sub);
  return within < duty ? (second ? 2 : 1) : 0;
}

// --- particles: rain, matrix, meteors ---------------------------------------------------------------------------

// Drops falling from the far end to the start, leaving trails in st->a. v[0..5]: drop positions x 256 (-1: none).
void drops(EffectState* st, int n, uint32_t now, float rate, float spawn, float decay) {
  if (!st->started)
    for (int d = 0; d < 6; d++) st->v[d] = -1;
  for (int s = due_steps(st, now, 16); s > 0; s--) {
    for (int p = 0; p < n; p++) st->a[p] = (uint8_t)(st->a[p] * decay);
    for (int d = 0; d < 6; d++) {
      if (st->v[d] < 0) continue;
      st->v[d] -= (int32_t)(rate * 256.0f * 0.016f);
      if (st->v[d] < 0) st->v[d] = -1;
      else st->a[st->v[d] >> 8] = 255;
    }
    if (frand(st) < spawn) {
      for (int d = 0; d < 6; d++) {
        if (st->v[d] < 0) {
          st->v[d] = ((n - 1) << 8) + 255;
          break;
        }
      }
    }
  }
}

const uint16_t kColWarm[] = {0, 0, 0, 1000, 200}, kColRed[] = {1000, 0, 0, 0, 0}, kColBlue[] = {0, 0, 1000, 0, 0},
               kColRainBlue[] = {0, 300, 1000, 0, 300}, kColMatrixGreen[] = {0, 1000, 150, 0, 0},
               kColMeteorWhite[] = {0, 150, 400, 400, 1000}, kColCometCyan[] = {0, 600, 1000, 0, 300},
               kColAmber[] = {1000, 450, 0, 300, 0}, kColBreatheBlue[] = {0, 300, 1000, 0, 0},
               kColWhite[] = {1000, 1000, 1000, 1000, 1000}, kColOff[] = {0, 0, 0, 0, 0}, kColDimWarm[] = {0, 0, 0, 40, 0};

}  // namespace

const char* effect_name(EffectId id) {
  for (const Named& e : kEffects)
    if (e.id == id) return e.name;
  return "NONE";
}

EffectId effect_by_name(const char* name) {
  for (const Named& e : kEffects)
    if (!strcmp(name, e.name)) return e.id;
  return kNoEffect;
}

bool parse_effect_color(const char* text, uint16_t out[kEffectChannels]) {
  for (const NamedColor& c : kColors) {
    if (!strcmp(text, c.name)) {
      memcpy(out, c.c, sizeof c.c);
      return true;
    }
  }
  // r/g/b[/ww/cw] percents
  char buf[48];
  size_t len = strlen(text);
  if (len >= sizeof buf || !strchr(text, '/')) return false;
  memcpy(buf, text, len + 1);
  uint16_t v[kEffectChannels] = {0, 0, 0, 0, 0};
  int n = 0;
  for (char* part = buf; part;) {
    char* slash = strchr(part, '/');
    if (slash) *slash = '\0';
    if (n == kEffectChannels || !parse_level(part, &v[n])) return false;
    n++;
    part = slash ? slash + 1 : nullptr;
  }
  if (n != 3 && n != 5) return false;
  memcpy(out, v, sizeof v);
  return true;
}

void fire_color(uint8_t heat, int8_t tint, uint8_t out[kEffectChannels]) {
  // Stops: heat, then red, green, blue, warm white, cool white. Linear between stops.
  static const uint8_t kStops[][6] = {
      {0, 0, 0, 0, 0, 0},           {35, 45, 0, 0, 0, 0},         {80, 170, 6, 0, 0, 0},  // ember red
      {120, 255, 35, 0, 0, 0},      {155, 255, 75, 0, 10, 0},     {190, 255, 130, 5, 70, 0},  // orange, amber
      {220, 255, 175, 20, 170, 15}, {240, 255, 200, 45, 230, 80}, {255, 255, 215, 70, 255, 170},  // white hot
  };
  const int n = (int)(sizeof kStops / sizeof kStops[0]);
  int k = 1;
  while (k < n - 1 && heat > kStops[k][0]) k++;
  const uint8_t* lo = kStops[k - 1];
  const uint8_t* hi = kStops[k];
  const int span = hi[0] - lo[0] > 0 ? hi[0] - lo[0] : 1;
  const int f = heat <= lo[0] ? 0 : heat >= hi[0] ? span : heat - lo[0];
  for (int c = 0; c < kEffectChannels; c++) out[c] = (uint8_t)(lo[1 + c] + (hi[1 + c] - lo[1 + c]) * f / span);
  // Hue: a positive tint adds green (golden), a negative one takes it away (redder orange), up to 40 %.
  int g = out[1] + out[1] * tint * 40 / 10000;
  out[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
}

void effect_reset(EffectState* st, uint32_t seed) {
  memset(st, 0, sizeof *st);
  st->rng = seed ? seed : 0x9E3779B9u;
}

void effect_render(const EffectSpec& sp, EffectState* st, int n, bool wrap, uint32_t now,
                   uint8_t (*out)[kEffectChannels]) {
  if (n <= 0) return;
  if (n > kEffectMaxPositions) n = kEffectMaxPositions;
  const float sf = speed_of(sp);
  const float t = (float)now / 1000.0f;
  float c1[kEffectChannels], c2[kEffectChannels];

  switch (sp.id) {
    case kFireFx: fire(sp, st, n, now, out, false); break;
    case kFireplaceFx: fire(sp, st, n, now, out, true); break;

    case kCandleFx:
      for (int p = 0; p < n; p++) {
        float v = 0.72f + 0.28f * noise1(t * 4.0f * sf + p * 3.1f);
        if (noise1(t * 9.0f * sf + p * 1.7f + 40) > 0.86f) v *= 0.65f;  // the odd gutter
        Px x;
        x.add5(95 * v, 22 * v, 0, 255 * v, 0);
        x.store(out[p]);
      }
      break;

    case kLavaFx:
      for (int p = 0; p < n; p++) {
        float n1 = noise1(p * 0.18f + t * 0.35f * sf), n2 = noise1(p * 0.07f - t * 0.2f * sf + 50);
        float heat = 0.12f + 0.88f * powf(n1 * 0.6f + n2 * 0.4f, 1.5f);
        fire_color(to8(heat * 205), (int8_t)((n2 - 0.5f) * 120), out[p]);
      }
      break;

    case kSunriseFx: {
      // Night, dawn red, orange, warm white, daylight, then sunset back to night: 60 s a cycle (SLOW 120, FAST 30).
      static const float kKeys[][6] = {
          {0.00f, 0, 0, 12, 0, 0},     {0.10f, 0, 0, 12, 0, 0},       {0.20f, 120, 5, 8, 0, 0},
          {0.28f, 255, 60, 0, 0, 0},   {0.36f, 255, 130, 0, 120, 0},  {0.44f, 200, 120, 30, 255, 60},
          {0.50f, 120, 120, 90, 255, 255}, {0.58f, 200, 120, 30, 255, 60}, {0.66f, 255, 120, 0, 110, 0},
          {0.74f, 255, 50, 0, 0, 0},   {0.82f, 120, 5, 10, 0, 0},     {0.90f, 0, 0, 12, 0, 0},
          {1.00f, 0, 0, 12, 0, 0}};
      float phase = fmodf(t * sf / 60.0f, 1.0f);
      int k = 1;
      while (k < 12 && phase > kKeys[k][0]) k++;
      float f = (phase - kKeys[k - 1][0]) / (kKeys[k][0] - kKeys[k - 1][0]);
      for (int p = 0; p < n; p++) {
        float glow = 1.0f - 0.55f * (float)p / (float)(n > 1 ? n - 1 : 1);  // the sun rises at the start
        Px x;
        for (int ch = 0; ch < kEffectChannels; ch++)
          x.c[ch] = (kKeys[k - 1][1 + ch] + (kKeys[k][1 + ch] - kKeys[k - 1][1 + ch]) * f) * glow;
        x.store(out[p]);
      }
      break;
    }

    case kWaterFx:
    case kLakeFx: {
      const bool lake = sp.id == kLakeFx;  // calm water: slow swell, deeper and dimmer, the odd glint
      const float ws = lake ? 0.35f * sf : sf, bright = lake ? 0.5f : 1.0f;
      for (int s = due_steps(st, now, 30); s > 0; s--) {
        for (int p = 0; p < n; p++) st->a[p] = (uint8_t)(st->a[p] * (lake ? 0.9f : 0.8f));
        if (frand(st) < (lake ? 0.04f : 0.25f) * sf) st->a[rnd(st) % n] = 255;  // a glint on the water
      }
      for (int p = 0; p < n; p++) {
        float w1 = sinf(p * (lake ? 0.3f : 0.55f) - t * 2.1f * ws), w2 = sinf(p * 0.21f + t * 1.3f * ws + 1.7f);
        float w = (w1 * 0.6f + w2 * 0.4f) * 0.5f + 0.5f;
        Px x;
        x.add5(0, (20 + 110 * w * w) * bright * (lake ? 0.6f : 1.0f), (70 + 185 * w) * bright, 0,
               (w > 0.86f ? (w - 0.86f) / 0.14f * 200 : 0) * bright);
        x.c[4] += st->a[p] * (lake ? 0.35f : 0.7f);
        x.store(out[p]);
      }
      break;
    }

    case kRainFx:
    case kMatrixFx: {
      bool matrix = sp.id == kMatrixFx;
      drops(st, n, now, (matrix ? 22.0f : 14.0f) * sf, (matrix ? 0.18f : 0.10f) * sf, matrix ? 0.82f : 0.62f);
      color_of(sp, 0, matrix ? kColMatrixGreen : kColRainBlue, c1);
      for (int p = 0; p < n; p++) {
        Px x;
        x.add5(0, matrix ? 4 : 2, matrix ? 0 : 10, 0, 0);  // background
        x.add(c1, st->a[p] / 255.0f);
        if (matrix && st->a[p] == 255) x.c[4] += 120;  // white-hot heads
        x.store(out[p]);
      }
      break;
    }

    case kIceFx:
      for (int s = due_steps(st, now, 30); s > 0; s--) {
        for (int p = 0; p < n; p++) st->a[p] = (uint8_t)(st->a[p] * 0.85f);
        if (frand(st) < 0.2f * sf) st->a[rnd(st) % n] = 255;
      }
      for (int p = 0; p < n; p++) {
        float v = 0.55f + 0.45f * noise1(p * 0.45f + t * 0.6f * sf);
        Px x;
        x.add5(0, 70 * v, 120 * v, 0, 60 + 150 * v);
        x.c[4] += st->a[p] * 0.4f, x.c[2] += st->a[p] * 0.3f;
        x.store(out[p]);
      }
      break;

    case kAuroraFx:
      for (int p = 0; p < n; p++) {
        float h = 0.30f + 0.45f * noise1(p * 0.06f + t * 0.08f * sf);
        float c = 0.5f + 0.5f * sinf(p * 0.33f + t * 0.9f * sf + 3.0f * noise1(p * 0.05f - t * 0.1f * sf + 9));
        float col[kEffectChannels];
        hsv(h, 0.85f, 0.08f + 0.85f * c * c, col);
        Px x;
        x.add(col, 1.0f);
        x.c[4] += 35 * c * c * c * c;
        x.store(out[p]);
      }
      break;

    case kLightningFx: {
      // v[0] flashes left in this strike, v[1] next change (ms), v[2] flash on, v[3..4] range, v[5] intensity
      if (!st->started) {
        st->started = true;
        st->v[1] = (int32_t)now + 500;
      }
      if ((int32_t)(now - (uint32_t)st->v[1]) >= 0) {
        if (st->v[2]) {
          st->v[2] = 0;
          st->v[1] = (int32_t)now + 40 + (int32_t)(frand(st) * 110);
        } else if (st->v[0] > 0) {
          st->v[0]--;
          st->v[2] = 1;
          st->v[5] = 120 + rnd8(st) % 136;
          st->v[1] = (int32_t)now + 30 + (int32_t)(frand(st) * 50);
        } else {  // next strike
          st->v[0] = 2 + rnd8(st) % 4;
          int len = n * (30 + rnd8(st) % 71) / 100;
          if (len < 1) len = 1;
          st->v[3] = (int32_t)(rnd(st) % (uint32_t)(n - len + 1));
          st->v[4] = st->v[3] + len - 1;
          st->v[1] = (int32_t)now + (int32_t)((1500 + frand(st) * 4500) / sf);
        }
      }
      for (int p = 0; p < n; p++) {
        Px x;
        x.add5(2, 3, 14, 0, 3);  // storm clouds
        if (st->v[2] && p >= st->v[3] && p <= st->v[4]) {
          float k = st->v[5] / 255.0f;
          x.add5(60 * k, 70 * k, 140 * k, 180 * k, 255 * k);
        }
        x.store(out[p]);
      }
      break;
    }

    case kSpaceFx:
    case kNebulaFx: {
      bool nebula = sp.id == kNebulaFx;
      // twinkling stars in a; for SPACE a shooting star: v[0] position x 256 (-1 none), v[1] next one (ms)
      if (!st->started) {
        st->v[0] = -1;
        st->v[1] = (int32_t)now + 2000;
      }
      for (int s = due_steps(st, now, 30); s > 0; s--) {
        for (int p = 0; p < n; p++) st->a[p] = st->a[p] > 5 ? (uint8_t)(st->a[p] - 5) : 0;
        if (frand(st) < (nebula ? 0.08f : 0.3f) * sf) st->a[rnd(st) % n] = (uint8_t)(150 + rnd8(st) % 106);
        if (!nebula) {
          if (st->v[0] >= 0) {
            st->v[0] -= (int32_t)(45.0f * sf * 256 * 0.030f);
            if (st->v[0] < -4 * 256) st->v[0] = -1, st->v[1] = (int32_t)now + (int32_t)((3000 + frand(st) * 6000) / sf);
          } else if ((int32_t)(now - (uint32_t)st->v[1]) >= 0) {
            st->v[0] = (n - 1) << 8;
          }
        }
      }
      for (int p = 0; p < n; p++) {
        Px x;
        if (nebula) {
          float n1 = noise1(p * 0.12f + t * 0.15f * sf), n2 = noise1(p * 0.25f - t * 0.1f * sf + 20);
          float col[kEffectChannels];
          hsv(0.72f + 0.2f * (n1 - 0.5f) + 0.1f * n2, 0.75f, 0.12f + 0.7f * powf(n2, 1.3f), col);
          x.add(col, 1.0f);
        } else {
          x.add5(1, 1, 8 + 8 * noise1(p * 0.2f + t * 0.05f), 0, 0);  // deep blue sky
        }
        float s = st->a[p] / 255.0f;
        x.add5(0, 0, 20 * s, 90 * s, (nebula ? 120 : 220) * s);
        if (!nebula && st->v[0] >= -4 * 256) {
          float k = beam((float)p, st->v[0] / 256.0f, -1.0f, 1.2f);
          x.add5(40 * k, 60 * k, 120 * k, 160 * k, 255 * k);
        }
        x.store(out[p]);
      }
      break;
    }

    case kMeteorFx: {
      // v[0] head x 256 (-1 none), v[1] next meteor (ms); a = trail that breaks up as it fades
      if (!st->started) st->v[0] = -1;
      color_of(sp, 0, kColMeteorWhite, c1);
      for (int s = due_steps(st, now, 16); s > 0; s--) {
        for (int p = 0; p < n; p++)
          if (rnd8(st) > 90) st->a[p] = (uint8_t)(st->a[p] * 0.72f);
        if (st->v[0] >= 0) {
          st->a[st->v[0] >> 8] = 255;
          st->v[0] -= (int32_t)(26.0f * sf * 256 * 0.016f);
          if (st->v[0] < 0) st->v[0] = -1, st->v[1] = (int32_t)now + (int32_t)((400 + frand(st) * 1600) / sf);
        } else if ((int32_t)(now - (uint32_t)st->v[1]) >= 0) {
          st->v[0] = ((n - 1) << 8) + 255;
        }
      }
      for (int p = 0; p < n; p++) {
        Px x;
        x.add(c1, st->a[p] / 255.0f);
        x.store(out[p]);
      }
      break;
    }

    case kLaserFx:
    case kScannerFx: {
      bool laser = sp.id == kLaserFx;
      color_of(sp, 0, kColRed, c1);
      float dir, x0 = sweep(now, (laser ? 32.0f : 14.0f) * sf, n, wrap, 0, &dir);
      for (int p = 0; p < n; p++) {
        float k = beam((float)p, x0, dir, laser ? 1.6f : 2.2f);
        if (!laser) k = fmaxf(k, 1.0f - fabsf((float)p - x0) / 1.6f);  // a wider eye
        Px x;
        x.add(c1, k);
        if (laser && fabsf((float)p - x0) < 0.6f) x.c[4] += 70;  // hot core
        x.store(out[p]);
      }
      break;
    }

    case kLasersFx: {
      // three beams at different speeds, additive: red, green, blue (or the given colours, then blue)
      color_of(sp, 0, kColRed, c1);
      static const uint16_t kColGreenT[] = {0, 1000, 0, 0, 0};
      color_of(sp, 1, kColGreenT, c2);
      float c3[kEffectChannels];
      for (int ch = 0; ch < kEffectChannels; ch++) c3[ch] = (float)kColBlue[ch] * 255.0f / 1000.0f;
      const float rates[3] = {31.0f, 19.0f, 43.0f}, phases[3] = {0, 7.3f, 15.1f};
      float* cols[3] = {c1, c2, c3};
      float xs[3], dirs[3];
      for (int b = 0; b < 3; b++) xs[b] = sweep(now, rates[b] * sf, n, wrap, phases[b], &dirs[b]);
      for (int p = 0; p < n; p++) {
        Px x;
        for (int b = 0; b < 3; b++) x.add(cols[b], beam((float)p, xs[b], dirs[b], 1.4f));
        x.store(out[p]);
      }
      break;
    }

    case kDiscoFx: {
      const float beat_s = 0.5f / sf;  // 120 bpm (SLOW 60, FAST 240)
      float beats = t / beat_s;
      int32_t beat = (int32_t)floorf(beats);
      float phase = beats - (float)beat;
      int block = n / 8 > 2 ? n / 8 : 2;
      for (int p = 0; p < n; p++) {
        int b = p / block;
        Px x;
        if (hash2(beat, b) > 0.25f) {
          float col[kEffectChannels];
          hsv(hash2(beat * 31 + 7, b), 1.0f, 1.0f - 0.65f * phase, col);
          x.add(col, 1.0f);
        }
        if (phase < 0.3f && hash2(beat * 17 + 3, p) > 0.93f) x.add5(80, 80, 80, 200, 255);  // mirror ball
        x.store(out[p]);
      }
      break;
    }

    case kStrobeFx: {
      color_of(sp, 0, kColWhite, c1);
      const float hz = sp.hz ? (float)sp.hz : sp.speed == 0 ? 4.0f : sp.speed == 2 ? 20.0f : 10.0f;
      const float flash_ms = fminf(30.0f, 400.0f / hz);  // 30 ms flashes, shorter above 13 Hz
      bool on = fmodf(t * hz, 1.0f) * (1000.0f / hz) < flash_ms;
      for (int p = 0; p < n; p++) {
        Px x;
        if (on) x.add(c1, 1.0f);
        x.store(out[p]);
      }
      break;
    }

    case kPoliceFx: {
      color_of(sp, 0, kColRed, c1);
      color_of(sp, 1, kColBlue, c2);
      // A double flash per side, sides alternating: 120 flashes a minute per side at normal speed (SAE J595's
      // range for warning lamps is 60-240 a minute).
      float ph = fmodf(t * (sp.hz ? (float)sp.hz : sf), 1.0f);
      bool first = ph < 0.5f;
      float sub = (first ? ph : ph - 0.5f) * 2.0f;
      bool on = sub < 0.18f || (sub > 0.3f && sub < 0.48f);
      for (int p = 0; p < n; p++) {
        bool lower = p < (n + 1) / 2;
        Px x;
        if (on && lower == first) x.add(lower ? c1 : c2, 1.0f);
        x.store(out[p]);
      }
      break;
    }

    case kConfettiFx:
      for (int s = due_steps(st, now, 20); s > 0; s--) {
        for (int p = 0; p < n; p++) st->a[p] = (uint8_t)(st->a[p] * 0.92f);
        if (frand(st) < 0.35f * sf) {
          int p = (int)(rnd(st) % (uint32_t)n);
          st->a[p] = 255;
          st->b[p] = rnd8(st);
        }
      }
      for (int p = 0; p < n; p++) {
        float col[kEffectChannels];
        hsv(st->b[p] / 255.0f, 0.9f, st->a[p] / 255.0f, col);
        Px x;
        x.add(col, 1.0f);
        x.store(out[p]);
      }
      break;

    case kRainbowFx:
      for (int p = 0; p < n; p++) {
        float col[kEffectChannels];
        hsv((float)p / (float)n - t * 0.25f * sf, 1.0f, 1.0f, col);
        Px x;
        x.add(col, 1.0f);
        x.store(out[p]);
      }
      break;

    case kPlasmaFx:
      for (int p = 0; p < n; p++) {
        float xp = p * 0.25f;
        float v = sinf(xp + t * 0.9f * sf) + sinf(xp * 0.53f - t * 1.3f * sf + 2) + sinf(xp * 0.29f + t * 0.6f * sf + 4);
        float col[kEffectChannels];
        hsv(v / 6.0f + 0.5f + t * 0.03f * sf, 0.95f, 0.6f + 0.4f * sinf(v * 1.7f), col);
        Px x;
        x.add(col, 1.0f);
        x.store(out[p]);
      }
      break;

    case kCometFx: {
      color_of(sp, 0, kColCometCyan, c1);
      float tail = n / 4 > 3 ? (float)(n / 4) : 3.0f;
      float span = (float)n + (wrap ? 0.0f : tail * 2.5f);
      float head = fmodf(t * 20.0f * sf, span);
      for (int p = 0; p < n; p++) {
        float k = beam((float)p, head, 1.0f, tail / 2.5f);
        if (wrap) k = fmaxf(k, beam((float)p + n, head, 1.0f, tail / 2.5f));
        Px x;
        x.add(c1, k);
        if (fabsf((float)p - head) < 0.6f) x.c[4] += 90;
        x.store(out[p]);
      }
      break;
    }

    case kTheaterFx: {
      color_of(sp, 0, kColAmber, c1);
      color_of(sp, 1, kColOff, c2);
      int32_t step = (int32_t)(t * 6.0f * sf);
      for (int p = 0; p < n; p++) {
        Px x;
        x.add(((p + step) % 3 == 0) ? c1 : c2, 1.0f);
        x.store(out[p]);
      }
      break;
    }

    case kWipeFx: {
      static const uint16_t kColWipeBlue[] = {0, 200, 1000, 0, 0};
      color_of(sp, 0, kColWipeBlue, c1);
      color_of(sp, 1, kColOff, c2);
      // fill over 2 s, hold 0.5 s, empty over 2 s, hold 0.5 s
      float ph = fmodf(t * sf, 5.0f);
      float edge = ph < 2.0f ? ph / 2.0f : ph < 2.5f ? 1.0f : ph < 4.5f ? 1.0f - (ph - 2.5f) / 2.0f : 0.0f;
      float front = edge * (float)n;
      for (int p = 0; p < n; p++) {
        float k = clampf(front - (float)p, 0.0f, 1.0f);
        Px x;
        x.add(c1, k);
        x.add(c2, 1.0f - k);
        x.store(out[p]);
      }
      break;
    }

    case kBreatheFx: {
      color_of(sp, 0, kColBreatheBlue, c1);
      float b = 0.5f - 0.5f * cosf(2 * kPi * t * sf / 4.0f);
      for (int p = 0; p < n; p++) {
        Px x;
        x.add(c1, 0.04f + 0.96f * b * b);
        x.store(out[p]);
      }
      break;
    }

    case kHeartbeatFx: {
      color_of(sp, 0, kColRed, c1);
      float ph = fmodf(t * sf / 1.2f, 1.0f), e = 0.05f;
      if (ph < 0.12f) e = fmaxf(e, sinf(ph / 0.12f * kPi));
      else if (ph > 0.20f && ph < 0.34f) e = fmaxf(e, 0.7f * sinf((ph - 0.20f) / 0.14f * kPi));
      for (int p = 0; p < n; p++) {
        Px x;
        x.add(c1, e);
        x.store(out[p]);
      }
      break;
    }

    case kTwinkleFx:
      color_of(sp, 0, kColWarm, c1);
      color_of(sp, 1, kColDimWarm, c2);
      for (int s = due_steps(st, now, 20); s > 0; s--) {
        for (int p = 0; p < n; p++) {  // b: 0 idle, 1 brightening, 2 fading
          if (st->b[p] == 1) {
            if (st->a[p] >= 243) st->a[p] = 255, st->b[p] = 2;
            else st->a[p] = (uint8_t)(st->a[p] + 12);
          } else if (st->b[p] == 2) {
            if (st->a[p] <= 6) st->a[p] = 0, st->b[p] = 0;
            else st->a[p] = (uint8_t)(st->a[p] - 6);
          }
        }
        if (frand(st) < n * 0.012f * sf) {
          int p = (int)(rnd(st) % (uint32_t)n);
          if (st->b[p] == 0) st->b[p] = 1;
        }
      }
      for (int p = 0; p < n; p++) {
        Px x;
        x.add(c2, 1.0f);
        x.add(c1, st->a[p] / 255.0f);
        x.store(out[p]);
      }
      break;

    case kWigwagFx:
    case kQuadFx:
    case kHazardFx: {
      // WIGWAG: one flash per side, sides alternating, 75 a minute per side (SLOW 60, FAST 120).
      // QUAD: four quick flashes per side, 60 cycles a minute (SLOW 30, FAST 120).
      // HAZARD: amber, two quick flashes per side, 75 sequences a minute (the MUTCD's rate for rapid-flashing
      //         beacons).
      static const uint16_t kAmberT[] = {1000, 450, 0, 0, 0};
      const bool hazard = sp.id == kHazardFx, quad = sp.id == kQuadFx;
      color_of(sp, 0, hazard ? kAmberT : kColRed, c1);
      color_of(sp, 1, hazard ? kAmberT : kColBlue, c2);
      float per_min = hazard ? 75.0f : quad ? 60.0f : 75.0f;
      if (!hazard) per_min *= sp.speed == 0 ? (quad ? 0.5f : 0.8f) : sp.speed == 2 ? (quad ? 2.0f : 1.6f) : 1.0f;
      const float hz = sp.hz ? (float)sp.hz : per_min / 60.0f;
      const int lit_half = flasher(fmodf(t * hz, 1.0f), quad ? 4 : hazard ? 2 : 1, quad ? 0.5f : hazard ? 0.55f : 0.8f);
      for (int p = 0; p < n; p++) {
        int half = p < (n + 1) / 2 ? 1 : 2;
        Px x;
        if (lit_half == half) x.add(half == 1 ? c1 : c2, 1.0f);
        x.store(out[p]);
      }
      break;
    }

    case kBeaconFx: {
      // MUTCD 4S.01: 50-60 flashes a minute, lit for 1/2 to 2/3 of each cycle. Normal 55 a minute (SLOW 50, FAST
      // 60), lit 60 %; amber unless a colour is given.
      static const uint16_t kAmberT[] = {1000, 450, 0, 0, 0};
      color_of(sp, 0, kAmberT, c1);
      const float hz = sp.hz ? (float)sp.hz : (sp.speed == 0 ? 50.0f : sp.speed == 2 ? 60.0f : 55.0f) / 60.0f;
      const bool on = fmodf(t * hz, 1.0f) < 0.6f;
      for (int p = 0; p < n; p++) {
        Px x;
        if (on) x.add(c1, 1.0f);
        x.store(out[p]);
      }
      break;
    }

    case kLightFx:
    case kDaylightFx:
    case kNeutralFx:
    case kWarmLightFx: {
      const float preset = sp.id == kDaylightFx ? 6500.0f : sp.id == kNeutralFx ? 4500.0f
                         : sp.id == kWarmLightFx ? 3000.0f : 4000.0f;
      float w[kEffectChannels];
      white_light(sp.kelvin ? (float)sp.kelvin : preset, 1.0f, w);
      for (int p = 0; p < n; p++) {
        Px x;
        x.add(w, 1.0f);
        x.store(out[p]);
      }
      break;
    }

    default:
      for (int p = 0; p < n; p++) memset(out[p], 0, kEffectChannels);
      break;
  }
  // The dimmer, last: a plain percent in the item scales whatever the effect drew.
  if (sp.level < 1000) {
    for (int p = 0; p < n; p++)
      for (int c = 0; c < kEffectChannels; c++) out[p][c] = (uint8_t)((out[p][c] * (uint32_t)sp.level + 500) / 1000);
  }
}

}  // namespace sl
