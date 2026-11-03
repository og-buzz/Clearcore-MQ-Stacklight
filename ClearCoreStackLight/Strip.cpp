// Strip.cpp — see Strip.h.
#include "Strip.h"

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
  return (size_t)clamp_pixels(pixels) * kStripChannels * t.bits + strip_reset_bytes(t);
}

void strip_pulses(const StripTiming& t, int ns[4]) {
  auto to_ns = [&](int spi_bits) { return (int)(((uint64_t)spi_bits * 1000000000u + t.spi_hz / 2) / t.spi_hz); };
  int h0 = high_bits(t.zero, t.bits), h1 = high_bits(t.one, t.bits);
  ns[0] = to_ns(h0);
  ns[1] = to_ns(t.bits) - ns[0];
  ns[2] = to_ns(h1);
  ns[3] = to_ns(t.bits) - ns[2];
}

void strip_color(const bool on[kChannels], const uint16_t level[3], const uint8_t order[3],
                 uint8_t out[kStripChannels]) {
  const Channel colours[3] = {kRed, kGreen, kBlue};
  for (int i = 0; i < 3; i++) {
    int c = order[i] < 3 ? order[i] : i;
    out[i] = on[colours[c]] ? level_duty(level[c]) : 0;
  }
  out[3] = out[4] = 0;
}

void strip_encode_solid(const uint8_t color[kStripChannels], int pixels, const StripTiming& t, uint8_t* out) {
  pixels = clamp_pixels(pixels);
  const size_t per_pixel = (size_t)kStripChannels * t.bits;
  uint8_t pixel[kStripChannels * kStripMaxBits];
  for (int c = 0; c < kStripChannels; c++) encode_byte(color[c], t, pixel + c * t.bits);
  for (int p = 0; p < pixels; p++) memcpy(out + (size_t)p * per_pixel, pixel, per_pixel);
  memset(out + (size_t)pixels * per_pixel, 0, strip_reset_bytes(t));  // reset: line low
}

size_t strip_encode_raw(const uint8_t* bytes, int n, int pixels, const StripTiming& t, uint8_t* out, size_t cap) {
  if (n < 1 || n > 8) return 0;
  pixels = clamp_pixels(pixels);
  const size_t per_pixel = (size_t)n * t.bits, len = (size_t)pixels * per_pixel + strip_reset_bytes(t);
  if (len > cap) return 0;
  uint8_t pixel[8 * kStripMaxBits];
  for (int c = 0; c < n; c++) encode_byte(bytes[c], t, pixel + c * t.bits);
  for (int p = 0; p < pixels; p++) memcpy(out + (size_t)p * per_pixel, pixel, per_pixel);
  memset(out + (size_t)pixels * per_pixel, 0, strip_reset_bytes(t));
  return len;
}

}  // namespace sl
