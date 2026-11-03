// Host tests for the parts of ClearCoreStackLight that don't touch hardware.
// Build and run: make -C test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ClearCoreStackLight/Light.h"
#include "../ClearCoreStackLight/Settings.h"
#include "../ClearCoreStackLight/Strip.h"

#include <algorithm>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                      \
    }                                                                  \
  } while (0)

using namespace sl;

static bool parse(const char* text, State* s) {
  char err[64];
  return parse_command(text, strlen(text), s, err, sizeof err);
}

static void desc_is(const char* text, const char* want) {
  State s;
  char got[64];
  CHECK(parse(text, &s));
  describe(s, got, sizeof got);
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "describe(%s) = %s, want %s\n", text, got, want);
    failures++;
  }
  State back;
  CHECK(parse(got, &back));  // the description parses back to the same state
  CHECK(memcmp(&back, &s, sizeof s) == 0);
}

static void test_commands() {
  State s;
  CHECK(parse("GREEN", &s) && s.mode[kGreen] == kOn && s.mode[kRed] == kOff);
  CHECK(parse("red,fast", &s) && s.mode[kRed] == kFast);
  CHECK(parse(" Yellow , Slow ", &s) && s.mode[kRed] == kSlow && s.mode[kGreen] == kSlow && s.mode[kBlue] == kOff);
  CHECK(parse("ORANGE", &s) && s.mode[kRed] == kOn && s.mode[kGreen] == kOn);
  CHECK(parse("ALL,ON;ALARM,FAST", &s) && s.mode[kBlue] == kOn && s.mode[kAlarm] == kFast);
  CHECK(parse("ALL,OFF", &s) && s.mode[kRed] == kOff && !s.fire);  // the 2024 sketch's "all off"
  CHECK(parse("OFF", &s) && s.mode[kGreen] == kOff);
  CHECK(parse("RED;OFF;GREEN", &s) && s.mode[kRed] == kOff && s.mode[kGreen] == kOn);  // OFF resets
  CHECK(parse("FIRE", &s) && s.fire);
  CHECK(parse("ALL,FIRE", &s) == false);  // FIRE is a name, not a mode
  CHECK(parse("GREEN;", &s) && s.mode[kGreen] == kOn);

  // Errors leave the previous state alone.
  State before;
  parse("BLUE,SLOW", &before);
  s = before;
  char err[64];
  CHECK(!parse_command("PURPLE", 6, &s, err, sizeof err) && strstr(err, "PURPLE"));
  CHECK(!parse_command("RED,BLINK", 9, &s, err, sizeof err) && strstr(err, "BLINK"));
  CHECK(!parse_command("", 0, &s, err, sizeof err));
  CHECK(!parse_command(";;", 2, &s, err, sizeof err));
  CHECK(!parse_command("OFF,ON", 6, &s, err, sizeof err));
  CHECK(memcmp(&s, &before, sizeof s) == 0);

  desc_is("GREEN", "GREEN,ON");
  desc_is("OFF", "OFF");
  desc_is("YELLOW,SLOW;ALARM,FAST", "YELLOW,SLOW;ALARM,FAST");
  desc_is("RED,FAST;GREEN,SLOW", "RED,FAST;GREEN,SLOW");
  desc_is("WHITE", "WHITE,ON");
  desc_is("CYAN", "GREEN,ON;BLUE,ON");
  desc_is("FIRE", "FIRE");
}

static void test_blink() {
  Timing t = {350, 150, 750, 250};
  State s;
  bool on[kChannels];
  parse("RED,FAST;GREEN,SLOW", &s);
  lit(s, t, 0, on);
  CHECK(on[kRed] && on[kGreen]);
  lit(s, t, 349, on);
  CHECK(on[kRed]);
  lit(s, t, 350, on);
  CHECK(!on[kRed]);
  lit(s, t, 500, on);
  CHECK(on[kRed]);  // fast period 500 ms
  lit(s, t, 749, on);
  CHECK(on[kGreen]);
  lit(s, t, 750, on);
  CHECK(!on[kGreen]);
  lit(s, t, 1000, on);
  CHECK(on[kGreen]);  // slow period 1000 ms
  parse("YELLOW,SLOW", &s);
  for (uint32_t ms = 0; ms < 3000; ms += 37) {
    lit(s, t, ms, on);
    CHECK(on[kRed] == on[kGreen]);  // the two halves of yellow stay in step
  }
  parse("FIRE", &s);
  lit(s, t, 0, on);
  CHECK(on[kRed] && !on[kGreen] && on[kAlarm]);
  lit(s, t, 1000, on);
  CHECK(on[kRed] && on[kGreen]);
}

static bool line(Settings* s, const char* text) {
  char err[96];
  bool ok = apply_line(s, text, err, sizeof err);
  if (!ok) printf("  (expected) %s -> %s\n", text, err);
  return ok;
}

static void test_settings() {
  Settings s;
  defaults(&s);
  CHECK(s.port == 1883 && s.dhcp && s.pin[kRed] == kNoPin && !s.allow_reset);
  char err[96];
  CHECK(!mqtt_ready(s, err, sizeof err) && strstr(err, "broker"));

  CHECK(line(&s, "broker = 192.168.1.10"));
  CHECK(line(&s, "# a comment"));
  CHECK(line(&s, "   "));
  CHECK(line(&s, "project=stacklight"));
  CHECK(line(&s, "client=acme"));
  CHECK(line(&s, "site=main-plant"));
  CHECK(line(&s, "role=cc-01   # trailing comment"));
  CHECK(line(&s, "asset=cc-0001\r"));
  CHECK(line(&s, "red=IO4"));
  CHECK(line(&s, "green=io5"));
  CHECK(line(&s, "blue=none"));
  CHECK(line(&s, "button=DI6"));
  CHECK(line(&s, "slow_on=600"));
  CHECK(line(&s, "allow_reset=true"));
  CHECK(line(&s, "password=a#b"));  // '#' only starts a comment after a space
  CHECK(strcmp(s.password, "a#b") == 0);
  CHECK(strcmp(s.role, "cc-01") == 0 && strcmp(s.asset, "cc-0001") == 0);
  CHECK(s.pin[kRed] == kIO4 && s.pin[kGreen] == kIO5 && s.button == kDI6 && s.timing.slow_on == 600);
  CHECK(mqtt_ready(s, err, sizeof err));
  CHECK(pins_ok(s, err, sizeof err));
  char t[160];
  CHECK(topic(s, "command", t, sizeof t) && strcmp(t, "stacklight/acme/main-plant/cc-01/command") == 0);

  CHECK(line(&s, "ip=192.168.1.50") && !s.dhcp && s.ip[3] == 50);
  CHECK(line(&s, "ip=dhcp") && s.dhcp);

  // Bad values are rejected and leave the old value.
  CHECK(!line(&s, "red=DI6"));  // inputs can't drive the light
  CHECK(!line(&s, "red=IO9"));
  CHECK(s.pin[kRed] == kIO4);
  CHECK(!line(&s, "port=70000"));
  CHECK(!line(&s, "port=abc"));
  CHECK(!line(&s, "ip=10.0.0"));
  CHECK(!line(&s, "fast_on=0"));
  CHECK(!line(&s, "colour=red"));
  CHECK(!line(&s, "no equals sign"));
  CHECK(!line(&s, "site=a-site-name-that-is-far-too-long-to-fit"));
  CHECK(strcmp(s.site, "main-plant") == 0);

  CHECK(line(&s, "alarm=IO4"));  // same pin as red
  CHECK(!pins_ok(s, err, sizeof err) && strstr(err, "IO4"));
}

static void test_levels() {
  Settings s;
  defaults(&s);
  CHECK(s.level[kRed] == 1000 && s.level[kGreen] == 1000 && s.level[kBlue] == 1000);
  CHECK(line(&s, "green_level=35") && s.level[kGreen] == 350 && s.level[kRed] == 1000);
  CHECK(line(&s, "green_level=27.5") && s.level[kGreen] == 275);
  CHECK(line(&s, "red_level = 0") && s.level[kRed] == 0);
  CHECK(line(&s, "blue_level=100.0") && s.level[kBlue] == 1000);
  CHECK(!line(&s, "green_level=101"));
  CHECK(!line(&s, "green_level=100.5"));
  CHECK(!line(&s, "green_level=27.55"));  // one decimal at most
  CHECK(!line(&s, "green_level=27."));
  CHECK(!line(&s, "green_level=.5"));
  CHECK(!line(&s, "green_level=-5"));
  CHECK(!line(&s, "green_level=half"));
  CHECK(!line(&s, "alarm_level=50"));  // the alarm is on/off only
  CHECK(s.level[kGreen] == 275);

  char text[8];
  format_level(275, text, sizeof text);
  CHECK(strcmp(text, "27.5") == 0);
  format_level(250, text, sizeof text);
  CHECK(strcmp(text, "25") == 0);
  format_level(1000, text, sizeof text);
  CHECK(strcmp(text, "100") == 0);
  format_level(5, text, sizeof text);
  CHECK(strcmp(text, "0.5") == 0);

  CHECK(level_duty(0) == 0);
  CHECK(level_duty(1000) == 255);
  CHECK(level_duty(2000) == 255);
  CHECK(level_duty(500) == 128);
  CHECK(level_duty(270) == 69 && level_duty(275) == 70 && level_duty(280) == 71);  // 27.5 % is its own step
}

static void ini_is(const char* in, const char* want) {
  const char* const keys[2] = {"green_level", "red_level"};
  const char* const vals[2] = {"25", "100"};
  char out[512];
  long n = set_keys(in, strlen(in), keys, vals, 2, out, sizeof out);
  if (n < 0 || strcmp(out, want) != 0 || (size_t)n != strlen(want)) {
    fprintf(stderr, "set_keys(%s) = %s, want %s\n", in, n < 0 ? "(error)" : out, want);
    failures++;
  }
}

static void test_set_keys() {
  // Replaced in place, comments and other lines untouched, missing keys appended.
  ini_is("# c\nred=IO4\ngreen_level=100\nport=1883\n", "# c\nred=IO4\ngreen_level=25\nport=1883\nred_level=100\n");
  // Spacing around '=' and an inline comment: the whole line is rewritten.
  ini_is("  green_level = 40   # was 40\n", "green_level=25\nred_level=100\n");
  // Commented-out keys and keys that only start the same are not touched.
  ini_is("#green_level=10\ngreen_level_x=1\n", "#green_level=10\ngreen_level_x=1\ngreen_level=25\nred_level=100\n");
  // CRLF files stay CRLF; a last line without a newline gets one before the appended keys.
  ini_is("broker=x\r\ngreen_level=1\r\nsite=y", "broker=x\r\ngreen_level=25\r\nsite=y\r\nred_level=100\r\n");
  // Every line setting a key is replaced, so a later duplicate can't win.
  ini_is("green_level=1\ngreen_level=2\nred_level=3\n", "green_level=25\ngreen_level=25\nred_level=100\n");
  ini_is("", "green_level=25\nred_level=100\n");

  // Too small an output buffer is an error, not a truncated file.
  const char* const keys[1] = {"green_level"};
  const char* const vals[1] = {"25"};
  char small[8];
  CHECK(set_keys("port=1883\n", 10, keys, vals, 1, small, sizeof small) == -1);

  // The result parses: apply each line.
  const char* in = "red=IO4\ngreen=IO5\ngreen_level=100\n";
  char out[256];
  CHECK(set_keys(in, strlen(in), keys, vals, 1, out, sizeof out) > 0);
  Settings s;
  defaults(&s);
  char err[96];
  for (char* l = strtok(out, "\n"); l; l = strtok(nullptr, "\n")) CHECK(apply_line(&s, l, err, sizeof err));
  CHECK(s.level[kGreen] == 250 && s.pin[kGreen] == kIO5);
}

// Decodes SPI bytes as a WS2805 would: each data bit starts with a high pulse; a long high is a '1'.
// Also measures every pulse (in SPI bit periods of 200 ns) and the trailing low (the reset).
struct Decoded {
  std::vector<uint8_t> bytes;
  int min_high0 = 1 << 30, max_high0 = 0, min_high1 = 1 << 30, max_high1 = 0;
  bool periods_ok = true;
  int reset_bits = 0;
};
static Decoded ws2805_decode(const uint8_t* spi, size_t len, int bits_per_bit) {
  std::vector<int> bits;
  for (size_t i = 0; i < len; i++)
    for (int b = 7; b >= 0; b--) bits.push_back((spi[i] >> b) & 1);
  Decoded d;
  std::vector<int> data;
  size_t i = 0;
  while (i < bits.size()) {
    size_t h = i;
    while (h < bits.size() && bits[h] == 1) h++;
    size_t l = h;
    while (l < bits.size() && bits[l] == 0) l++;
    int high = (int)(h - i), low = (int)(l - h);
    if (high == 0) {  // only the trailing low is left
      d.reset_bits = low;
      break;
    }
    bool one = high * 2 >= bits_per_bit;
    data.push_back(one);
    if (one) d.min_high1 = std::min(d.min_high1, high), d.max_high1 = std::max(d.max_high1, high);
    else d.min_high0 = std::min(d.min_high0, high), d.max_high0 = std::max(d.max_high0, high);
    if (l == bits.size()) {  // the last bit's low runs into the reset
      d.reset_bits = low - (bits_per_bit - high);
      break;
    }
    if (high + low != bits_per_bit) d.periods_ok = false;
    i = l;
  }
  for (size_t k = 0; k + 7 < data.size(); k += 8) {
    uint8_t v = 0;
    for (int j = 0; j < 8; j++) v = (uint8_t)((v << 1) | data[k + j]);
    d.bytes.push_back(v);
  }
  return d;
}

static void test_strip() {
  // Encoding: for every timing, what a WS2805 decodes is exactly the colour, for every pixel.
  const uint8_t color[kStripChannels] = {0xA5, 0x00, 0xFF, 0x01, 0x80};
  const int pixels = 4;
  // Expected pulses per timing, in SPI bits: high of a '0', high of a '1'.
  const int expect_h0[kStripTimings] = {2, 1, 1, 1, 4, 3}, expect_h1[kStripTimings] = {4, 3, 2, 4, 9, 6};
  for (int ti = 0; ti < kStripTimings; ti++) {
    const StripTiming& t = strip_timing(ti);
    std::vector<uint8_t> spi(strip_frame_bytes(pixels, t), 0xEE);
    CHECK(spi.size() == (size_t)pixels * kStripChannels * t.bits + strip_reset_bytes(t));
    strip_encode_solid(color, pixels, t, spi.data());
    Decoded d = ws2805_decode(spi.data(), spi.size(), t.bits);
    CHECK(d.bytes.size() == (size_t)pixels * kStripChannels);
    for (size_t k = 0; k < d.bytes.size(); k++) CHECK(d.bytes[k] == color[k % kStripChannels]);
    CHECK(d.periods_ok);
    CHECK(d.min_high0 == expect_h0[ti] && d.max_high0 == expect_h0[ti]);
    CHECK(d.min_high1 == expect_h1[ti] && d.max_high1 == expect_h1[ti]);
    double bit_ns = 1e9 / t.spi_hz;
    CHECK(d.reset_bits * bit_ns >= 280000);  // reset >= 280 us
    CHECK(spi[0] & 0x80);                    // the first bit starts at once
    int ns[4];
    strip_pulses(t, ns);
    CHECK(abs(ns[0] - (int)(expect_h0[ti] * bit_ns + 0.5)) <= 1 && abs(ns[2] - (int)(expect_h1[ti] * bit_ns + 0.5)) <= 1);
    CHECK(abs(ns[0] + ns[1] - (int)(t.bits * bit_ns + 0.5)) <= 1 && ns[2] + ns[3] == ns[0] + ns[1]);
    CHECK(t.bits <= kStripMaxBits);
  }
  // Raw test patterns: any byte count per pixel decodes back exactly; too big for the buffer is refused.
  {
    const uint8_t raw[6] = {0x00, 0x00, 0x00, 0xFF, 0xFF, 0x5A};
    const StripTiming& t = strip_timing(4);
    std::vector<uint8_t> buf(4096);
    size_t len = strip_encode_raw(raw, 6, 3, t, buf.data(), buf.size());
    CHECK(len == 3 * 6 * t.bits + strip_reset_bytes(t));
    Decoded d = ws2805_decode(buf.data(), len, t.bits);
    CHECK(d.bytes.size() == 18);
    for (size_t k = 0; k < d.bytes.size(); k++) CHECK(d.bytes[k] == raw[k % 6]);
    CHECK(strip_encode_raw(raw, 6, 3, t, buf.data(), 100) == 0);
    CHECK(strip_encode_raw(raw, 0, 3, t, buf.data(), buf.size()) == 0);
    CHECK(strip_encode_raw(raw, 9, 3, t, buf.data(), buf.size()) == 0);
  }
  // Timing 0 against the WS2805 datasheet: T0H 400 ns (20 ns over 380, see Strip.h), T1H 800, T0L 1000,
  // T1L 600, period 1.4 us (>= 1.25).
  int ns[4];
  strip_pulses(strip_timing(0), ns);
  CHECK(ns[0] == 400 && ns[1] == 1000 && ns[2] == 800 && ns[3] == 600);
  CHECK(&strip_timing(-1) == &strip_timing(0) && &strip_timing(9) == &strip_timing(0));
  // Timing 4, the NeoPixelBus-like one: 333/750 ns and 750/333 ns, 1.083 us per bit, on the 48 MHz clock.
  strip_pulses(strip_timing(4), ns);
  CHECK(ns[0] == 333 && ns[1] == 750 && ns[2] == 750 && ns[3] == 333);
  CHECK(strip_timing(4).fast_clock && 48000000 / (2 * strip_timing(4).spi_hz) - 1 == 1);  // BAUD 1
  CHECK(strip_timing(5).fast_clock && 48000000 / (2 * strip_timing(5).spi_hz) - 1 == 2);  // BAUD 2
  CHECK(strip_frame_bytes(0, strip_timing(0)) == strip_reset_bytes(strip_timing(0)));
  CHECK(strip_frame_bytes(1000, strip_timing(0)) == strip_frame_bytes(kStripMaxPixels, strip_timing(0)));
  CHECK(strip_frame_bytes(kStripMaxPixels, strip_timing(0)) <= kStripMaxFrameBytes);

  // Colour from the light's outputs and the strip levels.
  bool on[kChannels] = {true, true, false, true};
  const uint16_t full[3] = {1000, 1000, 1000}, trimmed[3] = {1000, 250, 500};
  const uint8_t rgb[3] = {0, 1, 2}, grb[3] = {1, 0, 2};
  uint8_t c[kStripChannels];
  strip_color(on, full, rgb, c);
  CHECK(c[0] == 255 && c[1] == 255 && c[2] == 0 && c[3] == 0 && c[4] == 0);  // yellow; alarm not shown
  strip_color(on, trimmed, rgb, c);
  CHECK(c[0] == 255 && c[1] == level_duty(250) && c[2] == 0);
  bool blue_only[kChannels] = {false, false, true, false};
  strip_color(blue_only, trimmed, rgb, c);
  CHECK(c[0] == 0 && c[1] == 0 && c[2] == level_duty(500));
  // G R B strips: green goes first.
  bool green_only[kChannels] = {false, true, false, false};
  strip_color(green_only, trimmed, grb, c);
  CHECK(c[0] == level_duty(250) && c[1] == 0 && c[2] == 0 && c[3] == 0 && c[4] == 0);

  // Settings.
  Settings s;
  defaults(&s);
  char err[96];
  CHECK(s.strip_port == kNoStrip && s.strip_pixels == 0 && s.strip_level[1] == 1000);
  CHECK(!strip_ready(s, err, sizeof err) && err[0] == '\0');  // not configured: no complaint
  CHECK(line(&s, "strip_pixels=50"));
  CHECK(!strip_ready(s, err, sizeof err) && strstr(err, "strip"));
  CHECK(line(&s, "strip=com0") && s.strip_port == 0);
  CHECK(strip_ready(s, err, sizeof err));
  CHECK(line(&s, "strip=COM1") && s.strip_port == 1);
  CHECK(line(&s, "strip=none") && s.strip_port == kNoStrip);
  CHECK(!line(&s, "strip=COM2"));
  CHECK(!line(&s, "strip_pixels=0"));
  CHECK(!line(&s, "strip_pixels=301"));
  CHECK(line(&s, "strip_green_level=62.5") && s.strip_level[1] == 625);
  CHECK(!line(&s, "strip_green_level=120"));
  CHECK(s.strip_timing == 4 && line(&s, "strip_timing=2") && s.strip_timing == 2);
  CHECK(s.strip_order[0] == 0 && s.strip_order[1] == 1 && s.strip_order[2] == 2);
  CHECK(line(&s, "strip_order=grb") && s.strip_order[0] == 1 && s.strip_order[1] == 0 && s.strip_order[2] == 2);
  CHECK(!line(&s, "strip_order=RRB"));
  CHECK(!line(&s, "strip_order=RGBW"));
  CHECK(!line(&s, "strip_order=RGX"));
  CHECK(s.strip_order[0] == 1);
  CHECK(line(&s, "strip_timing=5") && s.strip_timing == 5);
  CHECK(!line(&s, "strip_timing=6"));
}

int main() {
  test_commands();
  test_blink();
  test_settings();
  test_levels();
  test_set_keys();
  test_strip();
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
