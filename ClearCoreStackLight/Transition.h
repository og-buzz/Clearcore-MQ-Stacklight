// Transition.h — the change from one look to the next: what was showing fades into the new look.
//
// Plain C++ with no Arduino dependencies, so it can be unit-tested on a PC (see test/).
//
// When a command changes the light, the frame that was on the strip and the duty on each light output are kept,
// and for `transition` ms (settings.ini, default 2000) every frame is a blend of them and the new look, rendered
// live (so a pulsing or blinking new look moves while it fades in):
//   - light outputs (IOn): an eased crossfade from the old duty to the new one;
//   - the strip: a soft front sweeps along the strip_layout from position 0 to the end, each pixel easing from old
//     to new as the front passes, and sparkles flash and fade out in the band the front is crossing. Sparkles take
//     the colour of the new look's brightest pixel (the old look's when the new one is dark), with their brightest
//     channel at `transition_sparkle` % (default 20, 0 = none) or that pixel's own level if it is brighter.
// A new change during a transition starts from the blended frame that is showing, so nothing jumps.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Strip.h"

namespace sl {

const uint16_t kTransitionMaxMs = 10000;
const uint16_t kTransitionDefaultMs = 2000;
const uint16_t kTransitionDefaultSparkle = 200;  // tenths of a percent

struct Transition {
  bool active;
  uint32_t start, ms, last;
  uint8_t sparkle;  // brightest channel of a sparkle, 0..255; 0 = no sparkles
  uint8_t from_duty[kIoPins];
  uint8_t from_pix[kStripMaxPixels * kStripChannels];  // wire order, like strip_render's output
  uint8_t spark[kStripMaxPixels];                      // each pixel's sparkle, 255 when it flashes, fading to 0
  uint32_t rng;
};

// Starts a transition at now from what is showing: duty per light output, and `pixels` pixels of strip bytes (wire
// order; nullptr for none, which counts as dark). ms 0 makes no transition. sparkle: 0..255.
void transition_start(Transition* tr, uint32_t now, uint32_t ms, uint8_t sparkle, const uint8_t from_duty[kIoPins],
                      const uint8_t* from_pix, int pixels, uint32_t seed);

// Whether a transition is still running at now (it ends by itself once its time is up).
bool transition_running(Transition* tr, uint32_t now);

// Blends the new look's light-output duty (in duty[], from io_duty) with the old one, in place.
void transition_io(const Transition& tr, uint32_t now, uint8_t duty[kIoPins]);

// Blends the new look's strip bytes (pix, from strip_render) with the old frame, in place, and draws the sparkles.
// Call once per frame sent: sparkles start and fade with the time between calls.
void transition_strip(Transition* tr, uint32_t now, const Layout& layout, int pixels, uint8_t* pix);

}  // namespace sl
