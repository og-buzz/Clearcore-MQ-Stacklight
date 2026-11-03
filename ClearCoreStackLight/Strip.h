// Strip.h — an addressable WS2805 LED strip on a COM port, mirroring the stack light.
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

const int kStripChannels = 5;  // R G B W1 W2
const uint32_t kStripResetUs = 300;
const int kStripMaxPixels = 300;

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

const size_t kStripMaxFrameBytes =
    (size_t)kStripMaxPixels * kStripChannels * kStripMaxBits + (kStripResetUs * 12 + 7) / 8;

// Bytes of the low tail that resets (latches) the strip, and of one whole frame of `pixels` pixels.
size_t strip_reset_bytes(const StripTiming& t);
size_t strip_frame_bytes(int pixels, const StripTiming& t);

// Pulse widths in ns: high and low of a '0' and of a '1'.
void strip_pulses(const StripTiming& t, int ns[4]);

// The bytes a pixel gets for the light's outputs right now: each lit colour channel (red, green, blue) at its
// strip level in tenths of a percent (0..1000), in the strip's colour order. order[i] is the colour sent in
// position i: 0 = red, 1 = green, 2 = blue (the WS2805 datasheet says R G B; BTF's WS2805 strip wants G R B).
// The whites (bytes 4 and 5) stay off; the alarm isn't shown.
void strip_color(const bool on[kChannels], const uint16_t level[3], const uint8_t order[3],
                 uint8_t out[kStripChannels]);

// Writes one frame with every pixel set to `color` into out (strip_frame_bytes(pixels, t) bytes).
void strip_encode_solid(const uint8_t color[kStripChannels], int pixels, const StripTiming& t, uint8_t* out);

// For testing a strip: every pixel gets the same `n` raw bytes (1..8), so other frame layouts can be tried.
// Returns the frame length, or 0 if it wouldn't fit in `cap` bytes.
size_t strip_encode_raw(const uint8_t* bytes, int n, int pixels, const StripTiming& t, uint8_t* out, size_t cap);

}  // namespace sl
