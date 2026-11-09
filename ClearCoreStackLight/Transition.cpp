// Transition.cpp — see Transition.h.
#include "Transition.h"

#include <math.h>
#include <string.h>

namespace sl {

namespace {

const float kFrontWidth = 0.4f;      // the fade band, as a fraction of the strip
const float kFrontDone = 0.85f;      // the front has crossed the strip by this fraction of the transition...
const float kSparksPerSecond = 6.f;  // ...so the last sparkles fade out before it ends; per pixel in the band
const float kSparkFadeMs = 90.f;     // a sparkle's brightness falls to 1/e in this long

float ease(float x) { return x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x); }

float progress(const Transition& tr, uint32_t now) {
  if (!tr.active || tr.ms == 0) return 1;
  const uint32_t t = now - tr.start;
  return t >= tr.ms ? 1 : (float)t / (float)tr.ms;
}

uint8_t mix(uint8_t from, uint8_t to, float e) { return (uint8_t)(from + (to - from) * e + 0.5f); }

uint32_t next_random(uint32_t* s) {  // xorshift32
  uint32_t x = *s ? *s : 0x9e3779b9u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *s = x;
}

// The brightest pixel's colour (by its channel sum), scaled so its brightest channel is `peak`, or kept as it is when
// it's brighter than that, so sparkles always glint over the look. False if all dark.
bool sparkle_colour(const uint8_t* pix, int pixels, uint8_t peak, uint8_t out[kStripChannels]) {
  int best = -1, best_sum = 0;
  for (int i = 0; i < pixels; i++) {
    int sum = 0;
    for (int c = 0; c < kStripChannels; c++) sum += pix[i * kStripChannels + c];
    if (sum > best_sum) best_sum = sum, best = i;
  }
  if (best < 0) return false;
  const uint8_t* p = pix + best * kStripChannels;
  uint8_t top = 0;
  for (int c = 0; c < kStripChannels; c++) top = p[c] > top ? p[c] : top;
  if (top >= peak) {
    memcpy(out, p, kStripChannels);
    return true;
  }
  for (int c = 0; c < kStripChannels; c++) out[c] = (uint8_t)((p[c] * peak + top / 2) / top);
  return true;
}

}  // namespace

void transition_start(Transition* tr, uint32_t now, uint32_t ms, uint8_t sparkle, const uint8_t from_duty[kIoPins],
                      const uint8_t* from_pix, int pixels, uint32_t seed) {
  if (pixels > kStripMaxPixels) pixels = kStripMaxPixels;
  if (pixels < 0) pixels = 0;
  tr->active = ms > 0;
  tr->start = tr->last = now;
  tr->ms = ms > kTransitionMaxMs ? kTransitionMaxMs : ms;
  tr->sparkle = sparkle;
  memcpy(tr->from_duty, from_duty, sizeof tr->from_duty);
  memset(tr->from_pix, 0, sizeof tr->from_pix);
  if (from_pix) memcpy(tr->from_pix, from_pix, (size_t)pixels * kStripChannels);
  memset(tr->spark, 0, sizeof tr->spark);
  tr->rng = seed;
}

bool transition_running(Transition* tr, uint32_t now) {
  if (tr->active && now - tr->start >= tr->ms) tr->active = false;
  return tr->active;
}

void transition_io(const Transition& tr, uint32_t now, uint8_t duty[kIoPins]) {
  const float e = ease(progress(tr, now));
  for (int p = 0; p < kIoPins; p++) duty[p] = mix(tr.from_duty[p], duty[p], e);
}

void transition_strip(Transition* tr, uint32_t now, const Layout& L, int pixels, uint8_t* pix) {
  if (pixels > kStripMaxPixels) pixels = kStripMaxPixels;
  const float u = progress(*tr, now);
  const float front = (1 + kFrontWidth) * ease(u / kFrontDone);
  uint32_t dt = now - tr->last;
  tr->last = now;
  if (dt > 100) dt = 100;
  const float fade = expf(-(float)dt / kSparkFadeMs);
  const uint32_t chance = (uint32_t)(kSparksPerSecond * (float)dt / 1000.f * 65536.f);  // per pixel in the band

  uint8_t colour[kStripChannels];
  const bool sparkles = tr->sparkle && (sparkle_colour(pix, pixels, tr->sparkle, colour) ||
                                        sparkle_colour(tr->from_pix, pixels, tr->sparkle, colour));
  for (int i = 0; i < pixels; i++) {
    // Where the pixel lies along the layout (physical order for pixels the layout leaves out), 0..1.
    const int pos = L.pos[i];
    const float x = pos >= 0 && L.positions > 1 ? (float)pos / (float)(L.positions - 1)
                    : pos < 0 && pixels > 1      ? (float)i / (float)(pixels - 1)
                                                 : 0.f;
    const float local = (front - x) / kFrontWidth;
    const float e = ease(local);
    uint8_t* o = pix + i * kStripChannels;
    const uint8_t* f = tr->from_pix + i * kStripChannels;
    for (int c = 0; c < kStripChannels; c++) o[c] = mix(f[c], o[c], e);

    tr->spark[i] = (uint8_t)(tr->spark[i] * fade);
    if (local > 0 && local < 1 && (next_random(&tr->rng) & 0xffff) < chance) tr->spark[i] = 255;
    if (!sparkles || !tr->spark[i]) continue;
    for (int c = 0; c < kStripChannels; c++) {
      const uint8_t v = (uint8_t)((colour[c] * tr->spark[i] + 127) / 255);
      if (v > o[c]) o[c] = v;
    }
  }
}

}  // namespace sl
