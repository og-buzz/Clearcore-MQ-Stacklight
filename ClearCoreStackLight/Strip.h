// Strip.h — an addressable WS2805 LED strip on a COM port: layout, rendering and the SPI signal.
//
// Plain C++ with no Arduino dependencies, so it can be unit-tested on a PC (see test/).
//
// The ClearCore has no fast logic-level GPIO, but each COM port is a 5 V SPI master with DMA. The WS2805
// waveform is made on the port's MOSI line (RJ-45 pin 5; GND on pin 4). By default every data bit becomes
// 7 SPI bits at 5 MHz, '0' = 1100000 (400 ns high, 1000 ns low) and '1' = 1111000 (800 ns high, 600 ns low);
// strip_timing() lists the alternatives.
// WS2805 datasheet: T0H 220-380 ns, T1H/T0L/T1L 580-1000 ns, reset >= 280 us, up to 800 kbps. NeoPixelBus
// (WLED) drives it at 300/790 ns and 790/300 ns, 1.09 us per bit. The default, timing 4, is close to that and
// is the one verified on BTF's 24 V WS2805 RGBCCT strip.
// Each pixel is 40 bits, three colours then W1 W2, most significant bit first; the colour order is a setting
// (datasheet R G B, BTF's strip G R B). Each frame ends with 300 us low (reset).
// The bytes must go out back to back: the sketch turns off the SERCOM's hardware slave select (MSSEN),
// which the ClearCore library enables and which pauses ~3 SPI clocks after every byte.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Light.h"

namespace sl {

const int kStripChannels = kPixelChannels;  // three colours, then W1 (warm) and W2 (cool)
const uint32_t kStripResetUs = 300;
const int kStripMaxPixels = kLightMaxPixels;

// How a data bit is drawn: `bits` SPI bits at spi_hz per data bit, `zero` and `one` the patterns (MSB first).
// A data byte takes `bits` SPI bytes. The COM ports' SPI clock source is 10 MHz (SPI at 5 or 2.5 MHz);
// timings with fast_clock need it switched to the 48 MHz generator (SPI at 12 or 8 MHz) by the sketch.
struct StripTiming {
  uint32_t spi_hz;
  uint8_t bits;
  uint16_t zero, one;
  bool fast_clock;
};
const int kStripTimings = 6;
const int kStripMaxBits = 13;
// 0: 400/1000 ns and 800/600 ns, 1.4 us     1: 200/1000 and 600/600, 1.2 us
// 2: 400/800 and 800/400, 1.2 us             3: 200/1200 and 800/600, 1.4 us
// 4: 333/750 and 750/333, 1.083 us (48 MHz clock; NeoPixelBus/WLED's WS2805: 300/790 and 790/300, 1.09 us)
// 5: 375/750 and 750/375, 1.125 us (48 MHz clock)
const StripTiming& strip_timing(int index);  // out-of-range indexes give timing 0

// Every frame starts with this many low bytes before the first pixel: the first bit an SPI transfer puts
// out can come out clipped, and BTF's WS2805 strip then loses it, shifting every pixel by one bit.
const size_t kStripLeadBytes = 8;

const size_t kStripMaxFrameBytes =
    kStripLeadBytes + (size_t)kStripMaxPixels * kStripChannels * kStripMaxBits + (kStripResetUs * 12 + 7) / 8;

// Bytes of the low tail that resets (latches) the strip, and of one whole frame of `pixels` pixels (lead-in,
// pixels and reset).
size_t strip_reset_bytes(const StripTiming& t);
size_t strip_frame_bytes(int pixels, const StripTiming& t);

// Pulse widths in ns: high and low of a '0' and of a '1'.
void strip_pulses(const StripTiming& t, int ns[4]);

// How the strip is laid out (strip_layout): each physical pixel shows one position (or none), so effects and
// pixel numbers in commands follow the shape, not the wiring.
//   line   straight, pixel i = position i (the default)
//   ring   straight, and the positions form a closed loop (moving effects go round)
//   fold   folded at its middle and run back: both legs show positions 0..half side by side
//   a map: physical ranges in position order, e.g. "0-24|49-25" (= fold for 50 pixels), "24-0,25-49" (centre
//          out), "0-16|33-17|34-49" (three legs). ',' continues the positions; '|' starts a leg that lies
//          beside the first, from position 0 again. Pixels not in the map stay dark.
struct Layout {
  int16_t pos[kStripMaxPixels];  // position of each physical pixel, -1 = none
  int positions;
  bool wrap;
};
bool layout_parse(const char* text, int pixels, Layout* out, char* err, size_t err_cap);

// Effect state for a command's effects, plus one for the FIRE name without STRIP items.
struct StripEffects {
  EffectState st[kMaxEffects + 1];
};
void strip_effects_reset(StripEffects* fx, uint32_t seed);

// Renders the light state at now_ms into per-pixel bytes (kStripChannels per physical pixel, wire order):
// positions get the STRIP items' levels and effects (or, without STRIP items, the colour names at full
// brightness, or FIRE), then the layout maps them to pixels and order[] to the strip's colour order
// (order[k] = the colour sent k-th: 0 red, 1 green, 2 blue; BTF's WS2805 strip wants G R B), whites last.
void strip_render(const State& s, const Timing& t, uint32_t now_ms, const Layout& layout, const uint8_t order[3],
                  StripEffects* fx, int pixels, uint8_t* pixel_bytes);

// Writes one frame from per-pixel bytes (kStripChannels per pixel, wire order) into out
// (strip_frame_bytes(pixels, t) bytes).
void strip_encode(const uint8_t* pixel_bytes, int pixels, const StripTiming& t, uint8_t* out);

// Writes one frame with every pixel set to `color` into out (strip_frame_bytes(pixels, t) bytes).
void strip_encode_solid(const uint8_t color[kStripChannels], int pixels, const StripTiming& t, uint8_t* out);

// For testing a strip: every pixel gets the same `n` raw bytes (1..8), so other frame layouts can be tried.
// Returns the frame length, or 0 if it wouldn't fit in `cap` bytes.
size_t strip_encode_raw(const uint8_t* bytes, int n, int pixels, const StripTiming& t, uint8_t* out, size_t cap);

}  // namespace sl
