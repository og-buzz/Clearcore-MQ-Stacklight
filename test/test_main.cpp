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
  char err[96];
  return parse_command(text, strlen(text), s, err, sizeof err);
}

static void desc_is(const char* text, const char* want) {
  static State s, back;
  char got[256];
  CHECK(parse(text, &s));
  describe(s, got, sizeof got);
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "describe(%s) = %s, want %s\n", text, got, want);
    failures++;
  }
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
  // Levels are no longer settings: they come with each command. Old keys are refused with a hint.
  Settings s;
  defaults(&s);
  char err[160];
  for (const char* old : {"green_level=35", "red_level=100", "strip_green_level=50"}) {
    CHECK(!apply_line(&s, old, err, sizeof err) && strstr(err, "no longer used") && strstr(err, "IO5=26"));
  }

  uint16_t v;
  CHECK(parse_level("35", &v) && v == 350);
  CHECK(parse_level("27.5", &v) && v == 275);
  CHECK(parse_level("0", &v) && v == 0);
  CHECK(parse_level("100.0", &v) && v == 1000);
  CHECK(!parse_level("101", &v));
  CHECK(!parse_level("100.5", &v));
  CHECK(!parse_level("27.55", &v));  // one decimal at most
  CHECK(!parse_level("27.", &v));
  CHECK(!parse_level(".5", &v));
  CHECK(!parse_level("-5", &v));
  CHECK(!parse_level("half", &v));

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

// IO and STRIP items: brightness per output and per pixel.
static void test_brightness_commands() {
  static State s;
  CHECK(parse("IO5=26", &s) && s.io_set[5] && s.io[5].level == 260 && s.io[5].mode == kOn && !s.strip_set);
  CHECK(parse("io4 = 100 , slow", &s) && s.io[4].level == 1000 && s.io[4].mode == kSlow);
  CHECK(parse("IO0=27.5,FAST", &s) && s.io[0].level == 275 && s.io[0].mode == kFast && !s.io_set[1]);
  CHECK(parse("IO5=40,OFF", &s) && s.io_set[5] && s.io[5].level == 0 && s.io[5].mode == kOn);  // off is level 0
  CHECK(parse("GREEN;IO5=26", &s) && s.mode[kGreen] == kOn && s.io[5].level == 260);  // names and pins together

  CHECK(parse("STRIP=100,40,0", &s) && s.strip_set);
  CHECK(s.pixel[0][0] == 1000 && s.pixel[0][1] == 400 && s.pixel[0][2] == 0 && s.pixel[0][3] == 0);
  CHECK(s.pixel[kLightMaxPixels - 1][1] == 400 && s.pixel_mode[17] == kOn);
  CHECK(parse("STRIP=0,0,0,0,30;STRIP[0-9]=100,0,0,0,0,FAST;STRIP[49]=0,0,100", &s));
  CHECK(s.pixel[0][0] == 1000 && s.pixel_mode[0] == kFast && s.pixel[9][0] == 1000 && s.pixel[9][4] == 0);
  CHECK(s.pixel[10][0] == 0 && s.pixel[10][4] == 300 && s.pixel_mode[10] == kOn);
  CHECK(s.pixel[49][2] == 1000 && s.pixel[49][4] == 0 && s.pixel[50][4] == 300);
  CHECK(parse("STRIP[3]=0,0,0,0,0,SLOW", &s) && s.pixel_mode[3] == kOn);  // a dark pixel is dark in any mode
  CHECK(parse("STRIP=10,20,30;OFF", &s) && !s.strip_set);  // OFF clears pixels too

  // Errors, each leaving the state alone.
  static State before;
  parse("BLUE,SLOW;IO5=26", &before);
  s = before;
  char err[96];
  const char* bad[] = {"IO6=50",        "IO=50",           "IOX=50",         "IO5=",          "IO5=101",
                       "IO5=50,60",     "IO5=50,BLINK",    "IO5=SLOW,50",    "STRIP=1,2",     "STRIP=1,2,3,4",
                       "STRIP=1,2,3,4,5,6", "STRIP[300]=1,2,3", "STRIP[5-2]=1,2,3", "STRIP[1-]=1,2,3",
                       "STRIP[2]x=1,2,3", "STRIPE=1,2,3",  "PIXEL=1,2,3",    "STRIP=1,2,3,FAST,ON"};
  for (const char* b : bad) {
    bool rejected = !parse_command(b, strlen(b), &s, err, sizeof err);
    if (!rejected) fprintf(stderr, "accepted bad command %s\n", b);
    CHECK(rejected);
  }
  CHECK(memcmp(&s, &before, sizeof s) == 0);

  // Descriptions: canonical, and they parse back to the same state.
  desc_is("IO5=26", "IO5=26");
  desc_is("io4=100,slow;io5=27.5", "IO4=100,SLOW;IO5=27.5");
  desc_is("GREEN;IO5=26", "GREEN,ON;IO5=26");
  desc_is("STRIP=100,40,0", "STRIP=100,40,0,0,0");
  desc_is("STRIP[0-9]=100,0,0,0,0,FAST", "STRIP[0-9]=100,0,0,0,0,FAST");
  desc_is("STRIP=0,0,0,0,30;STRIP[0-9]=100,0,0,0,0;STRIP[49]=0,0,100",
          "STRIP[0-9]=100,0,0,0,0;STRIP[10-48]=0,0,0,0,30;STRIP[49]=0,0,100,0,0;STRIP[50-299]=0,0,0,0,30");
  desc_is("STRIP=0,0,0", "STRIP=0,0,0,0,0");  // a dark strip still overrides the colour names
  desc_is("RED;STRIP[7]=0,100,0", "RED,ON;STRIP[7]=0,100,0,0,0");

  // PWM duty per IO pin: colour names light their pin at full, IO items override their pin.
  const Timing t = {350, 150, 750, 250};
  const int8_t pin[kChannels] = {kIO4, kIO5, kNoPin, kNoPin};
  uint8_t duty[kIoPins];
  parse("YELLOW", &s);
  io_duty(s, t, 0, pin, duty);
  CHECK(duty[4] == 255 && duty[5] == 255 && duty[0] == 0);
  parse("YELLOW;IO5=26", &s);
  io_duty(s, t, 0, pin, duty);
  CHECK(duty[4] == 255 && duty[5] == level_duty(260));
  parse("IO4=100,SLOW;IO5=0", &s);
  io_duty(s, t, 100, pin, duty);
  CHECK(duty[4] == 255 && duty[5] == 0);
  io_duty(s, t, 800, pin, duty);  // slow is off from 750 to 1000 ms
  CHECK(duty[4] == 0);
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

  // A null value removes the key's lines.
  {
    const char* const keys[1] = {"green_level"};
    const char* const vals[1] = {nullptr};
    char out[128];
    const char* in = "red=IO4\ngreen_level=26\r\nport=1883\ngreen_level=30";
    CHECK(set_keys(in, strlen(in), keys, vals, 1, out, sizeof out) > 0 && strcmp(out, "red=IO4\nport=1883\n") == 0);
    CHECK(set_keys("port=1883\n", 10, keys, vals, 1, out, sizeof out) > 0 && strcmp(out, "port=1883\n") == 0);
  }

  // Too small an output buffer is an error, not a truncated file.
  const char* const keys[1] = {"strip_pixels"};
  const char* const vals[1] = {"25"};
  char small[8];
  CHECK(set_keys("port=1883\n", 10, keys, vals, 1, small, sizeof small) == -1);

  // The result parses: apply each line.
  const char* in = "red=IO4\ngreen=IO5\nstrip_pixels=100\n";
  char out[256];
  CHECK(set_keys(in, strlen(in), keys, vals, 1, out, sizeof out) > 0);
  Settings s;
  defaults(&s);
  char err[96];
  for (char* l = strtok(out, "\n"); l; l = strtok(nullptr, "\n")) CHECK(apply_line(&s, l, err, sizeof err));
  CHECK(s.strip_pixels == 25 && s.pin[kGreen] == kIO5);
}

// Decodes SPI bytes as a WS2805 would: each data bit starts with a high pulse; a long high is a '1'.
// Also measures every pulse (in SPI bit periods of 200 ns) and the trailing low (the reset).
struct Decoded {
  std::vector<uint8_t> bytes;
  int min_high0 = 1 << 30, max_high0 = 0, min_high1 = 1 << 30, max_high1 = 0;
  bool periods_ok = true;
  int reset_bits = 0;
  int lead_bits = 0;
};
static Decoded ws2805_decode(const uint8_t* spi, size_t len, int bits_per_bit) {
  std::vector<int> bits;
  for (size_t i = 0; i < len; i++)
    for (int b = 7; b >= 0; b--) bits.push_back((spi[i] >> b) & 1);
  Decoded d;
  std::vector<int> data;
  size_t i = 0;
  while (i < bits.size() && bits[i] == 0) i++;  // skip the lead-in (a long low reads as idle, not data)
  d.lead_bits = (int)i;
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
    CHECK(spi.size() == kStripLeadBytes + (size_t)pixels * kStripChannels * t.bits + strip_reset_bytes(t));
    strip_encode_solid(color, pixels, t, spi.data());
    Decoded d = ws2805_decode(spi.data(), spi.size(), t.bits);
    CHECK(d.bytes.size() == (size_t)pixels * kStripChannels);
    for (size_t k = 0; k < d.bytes.size(); k++) CHECK(d.bytes[k] == color[k % kStripChannels]);
    CHECK(d.periods_ok);
    CHECK(d.min_high0 == expect_h0[ti] && d.max_high0 == expect_h0[ti]);
    CHECK(d.min_high1 == expect_h1[ti] && d.max_high1 == expect_h1[ti]);
    double bit_ns = 1e9 / t.spi_hz;
    CHECK(d.reset_bits * bit_ns >= 280000);  // reset >= 280 us
    CHECK(d.lead_bits == (int)kStripLeadBytes * 8);  // low lead-in, then the first pixel's first bit
    CHECK(spi[kStripLeadBytes] & 0x80);
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
    CHECK(len == kStripLeadBytes + 3 * 6 * t.bits + strip_reset_bytes(t));
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
  CHECK(strip_frame_bytes(0, strip_timing(0)) == kStripLeadBytes + strip_reset_bytes(strip_timing(0)));
  CHECK(strip_frame_bytes(1000, strip_timing(0)) == strip_frame_bytes(kStripMaxPixels, strip_timing(0)));
  CHECK(strip_frame_bytes(kStripMaxPixels, strip_timing(0)) <= kStripMaxFrameBytes);

  // Rendering: the colour names on every pixel at full brightness, unless STRIP items paint the positions.
  {
    static State st;
    static StripEffects fx;
    static Layout line, fold;
    char lerr[96];
    CHECK(layout_parse("line", 50, &line, lerr, sizeof lerr) && line.positions == 50 && !line.wrap);
    CHECK(layout_parse("fold", 50, &fold, lerr, sizeof lerr) && fold.positions == 25);
    const Timing t = {350, 150, 750, 250};
    const uint8_t rgb[3] = {0, 1, 2}, grb[3] = {1, 0, 2};
    uint8_t px[50 * kStripChannels];
    auto pix = [&](int i) { return px + i * kStripChannels; };
    strip_effects_reset(&fx, 1);

    parse("YELLOW;ALARM", &st);
    strip_render(st, t, 0, line, rgb, &fx, 50, px);
    CHECK(pix(5)[0] == 255 && pix(5)[1] == 255 && pix(5)[2] == 0 && pix(5)[3] == 0 && pix(5)[4] == 0);
    parse("GREEN", &st);
    strip_render(st, t, 0, line, grb, &fx, 50, px);
    CHECK(pix(5)[0] == 255 && pix(5)[1] == 0 && pix(5)[2] == 0);  // G R B strips: green goes first
    parse("STRIP=100,40,0,10,20;STRIP[3]=0,0,100,0,0,SLOW", &st);
    strip_render(st, t, 0, line, grb, &fx, 50, px);
    CHECK(pix(5)[0] == level_duty(400) && pix(5)[1] == 255 && pix(5)[3] == level_duty(100) && pix(5)[4] == level_duty(200));
    CHECK(pix(3)[2] == 255);
    strip_render(st, t, 800, line, grb, &fx, 50, px);  // slow blink is off from 750 to 1000 ms
    CHECK(pix(3)[2] == 0);
    parse("RED;STRIP[0]=0,100,0", &st);  // with a STRIP item, unpainted positions are dark, not red
    strip_render(st, t, 0, line, rgb, &fx, 50, px);
    CHECK(pix(1)[0] == 0 && pix(1)[1] == 0 && pix(1)[2] == 0 && pix(0)[1] == 255);
    // Warm white with status pixels at the ends.
    parse("IO5=26;STRIP=0,0,0,100,0;STRIP[0]=0,100,0;STRIP[49]=0,100,0", &st);
    strip_render(st, t, 0, line, rgb, &fx, 50, px);
    CHECK(pix(0)[1] == 255 && pix(0)[3] == 0 && pix(49)[1] == 255 && pix(25)[3] == 255 && pix(25)[1] == 0);

    // Folded: both legs show the same position side by side.
    parse("STRIP[0]=100,0,0;STRIP[24]=0,0,100", &st);
    strip_render(st, t, 0, fold, rgb, &fx, 50, px);
    CHECK(pix(0)[0] == 255 && pix(49)[0] == 255 && pix(24)[2] == 255 && pix(25)[2] == 255 && pix(10)[0] == 0);

    // Layout maps.
    static Layout L;
    CHECK(layout_parse("0-24|49-25", 50, &L, lerr, sizeof lerr) && L.positions == 25 && L.pos[49] == 0 && L.pos[25] == 24);
    CHECK(layout_parse("24-0,25-49", 50, &L, lerr, sizeof lerr) && L.positions == 50 && L.pos[24] == 0 && L.pos[0] == 24 && L.pos[25] == 25);
    CHECK(layout_parse("0-16|33-17|34-49", 50, &L, lerr, sizeof lerr) && L.positions == 17 && L.pos[33] == 0 && L.pos[34] == 0 && L.pos[49] == 15);
    CHECK(layout_parse("0-9", 50, &L, lerr, sizeof lerr) && L.positions == 10 && L.pos[10] == -1);  // the rest dark
    CHECK(layout_parse("RING", 50, &L, lerr, sizeof lerr) && L.wrap && L.positions == 50);
    CHECK(!layout_parse("0-50", 50, &L, lerr, sizeof lerr) && strstr(lerr, "past"));
    CHECK(!layout_parse("0-9,5-12", 50, &L, lerr, sizeof lerr) && strstr(lerr, "twice"));
    CHECK(!layout_parse("zigzag", 50, &L, lerr, sizeof lerr));
    CHECK(!layout_parse("0-9;10-20", 50, &L, lerr, sizeof lerr));
    CHECK(!layout_parse("0-", 50, &L, lerr, sizeof lerr));

    // A frame from per-pixel bytes decodes back to those bytes.
    uint8_t bytes[3 * kStripChannels] = {1, 2, 3, 4, 5, 0x80, 0x40, 0x01, 0xFF, 0x00, 9, 8, 7, 6, 5};
    std::vector<uint8_t> spi(strip_frame_bytes(3, strip_timing(4)));
    strip_encode(bytes, 3, strip_timing(4), spi.data());
    Decoded d = ws2805_decode(spi.data(), spi.size(), strip_timing(4).bits);
    CHECK(d.bytes.size() == sizeof bytes && memcmp(d.bytes.data(), bytes, sizeof bytes) == 0);
    CHECK(d.lead_bits == (int)kStripLeadBytes * 8);
  }

  // Settings.
  Settings s;
  defaults(&s);
  char err[96];
  CHECK(s.strip_port == kNoStrip && s.strip_pixels == 0);
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
  CHECK(s.strip_timing == 4 && line(&s, "strip_timing=2") && s.strip_timing == 2);
  CHECK(s.strip_order[0] == 0 && s.strip_order[1] == 1 && s.strip_order[2] == 2);
  CHECK(line(&s, "strip_order=grb") && s.strip_order[0] == 1 && s.strip_order[1] == 0 && s.strip_order[2] == 2);
  CHECK(!line(&s, "strip_order=RRB"));
  CHECK(!line(&s, "strip_order=RGBW"));
  CHECK(!line(&s, "strip_order=RGX"));
  CHECK(s.strip_order[0] == 1);
  CHECK(line(&s, "strip_timing=5") && s.strip_timing == 5);
  CHECK(!strcmp(s.strip_layout, "line") && line(&s, "strip_layout=0-24|49-25") && !strcmp(s.strip_layout, "0-24|49-25"));
  CHECK(line(&s, "strip_layout=fold") && !line(&s, "strip_layout=zigzag") && !strcmp(s.strip_layout, "fold"));
  char err2[160];
  CHECK(!apply_line(&s, "strip_mirror=true", err2, sizeof err2) && strstr(err2, "strip_layout=fold"));
  CHECK(!line(&s, "strip_timing=6"));
}

// Effects: every one parses, renders in range, and does something over time; named colours and speeds work.
static void test_effects() {
  static State st;
  static EffectState es;
  static uint8_t out[60][kEffectChannels];
  char err[96];
  for (int id = 1; id < kEffectCount; id++) {
    const char* name = effect_name((EffectId)id);
    CHECK(effect_by_name(name) == (EffectId)id);
    char cmd[64];
    snprintf(cmd, sizeof cmd, "STRIP[0-29]=%s,FAST", name);
    bool ok = parse_command(cmd, strlen(cmd), &st, err, sizeof err);
    if (!ok) fprintf(stderr, "%s: %s\n", cmd, err);
    CHECK(ok && st.fx_count == 1 && st.fx[0].id == id && st.fx[0].speed == 2 && st.pixel_fx[29] == 1 && st.pixel_fx[30] == 0);
    effect_reset(&es, 99);
    long lit = 0, changed = 0;
    uint8_t prev[60][kEffectChannels] = {};
    for (uint32_t ms = 0; ms <= 12000; ms += 40) {  // 12 s: long enough for lightning and meteors to appear
      effect_render(st.fx[0], &es, 30, false, ms, out);
      for (int p = 0; p < 30; p++)
        for (int c = 0; c < kEffectChannels; c++) {
          lit += out[p][c] > 0;
          changed += out[p][c] != prev[p][c];
        }
      memcpy(prev, out, sizeof prev);
    }
    if (!lit || !changed) fprintf(stderr, "effect %s: lit %ld, changed %ld\n", name, lit, changed);
    CHECK(lit > 0);
    CHECK(changed > 0);
  }
  // Colours and speeds; describe round-trips.
  CHECK(parse_command("STRIP=LASER,GREEN", 17, &st, err, sizeof err) && st.fx[0].ncolors == 1 && st.fx[0].color[0][1] == 1000);
  const char* two = "STRIP[0-9]=TWINKLE,SLOW,WARM,0/0/10";
  CHECK(parse_command(two, strlen(two), &st, err, sizeof err) && st.fx[0].speed == 0 && st.fx[0].ncolors == 2 &&
        st.fx[0].color[0][3] == 1000 && st.fx[0].color[1][2] == 100);
  desc_is("STRIP=FIRE,FAST", "STRIP=FIRE,FAST");
  desc_is("strip[10-19]=fire", "STRIP[10-19]=FIRE");
  desc_is("STRIP[0-9]=LASER,FAST,GREEN", "STRIP[0-9]=LASER,FAST,0/100/0/0/0");
  desc_is("STRIP[0-11]=WATER;STRIP[12-24]=FIRE,SLOW", "STRIP[0-11]=WATER;STRIP[12-24]=FIRE,SLOW");
  desc_is("STRIP=0,0,0,0,20;STRIP[0-24]=FIRE", "STRIP[0-24]=FIRE;STRIP[25-299]=0,0,0,0,20");
  for (const char* bad : {"STRIP=FIRE,OFF", "STRIP=FIRE,HOT", "STRIP=LASER,1/2", "STRIP=LASER,RED,BLUE,GREEN",
                          "STRIP[0]=FIRE;STRIP[1]=FIRE;STRIP[2]=FIRE;STRIP[3]=FIRE;STRIP[4]=FIRE;STRIP[5]=FIRE;"
                          "STRIP[6]=FIRE;STRIP[7]=FIRE;STRIP[8]=FIRE"}) {
    CHECK(!parse_command(bad, strlen(bad), &st, err, sizeof err));
  }
  // Dimmer, colour temperature and rate options.
  const char* opts = "STRIP=LIGHT,3000K,20";
  CHECK(parse_command(opts, strlen(opts), &st, err, sizeof err) && st.fx[0].kelvin == 3000 && st.fx[0].level == 200);
  desc_is("STRIP=LIGHT,4000K,20", "STRIP=LIGHT,20,4000K");
  desc_is("STRIP=STROBE,FAST,25HZ", "STRIP=STROBE,FAST,25HZ");
  desc_is("STRIP[0-9]=FIRE,40", "STRIP[0-9]=FIRE,40");
  CHECK(!parse_command("STRIP=LIGHT,90000K", 18, &st, err, sizeof err));
  CHECK(!parse_command("STRIP=STROBE,99HZ", 17, &st, err, sizeof err));
  {
    // LIGHT at 3000 K is the warm LEDs only, at 6500 K the cool ones; the dimmer scales it.
    EffectSpec w;
    memset(&w, 0, sizeof w);
    w.id = kLightFx, w.speed = 1, w.level = 1000, w.kelvin = 3000;
    effect_reset(&es, 1);
    effect_render(w, &es, 3, false, 0, out);
    CHECK(out[1][3] == 255 && out[1][4] == 0 && out[1][0] == 0);
    w.kelvin = 6500;
    effect_render(w, &es, 3, false, 0, out);
    CHECK(out[1][3] == 0 && out[1][4] == 255);
    w.kelvin = 4750, w.level = 200;
    effect_render(w, &es, 3, false, 0, out);
    CHECK(out[1][3] > 20 && out[1][3] < 30 && out[1][4] > 20 && out[1][4] < 30);  // half each, at 20 %
    w.kelvin = 2400, w.level = 1000;
    effect_render(w, &es, 3, false, 0, out);
    CHECK(out[1][3] == 255 && out[1][0] > 0);  // below 3000 K some red warms it
    // BEACON (MUTCD 50-60 a minute, lit 1/2 to 2/3): at normal speed 55 a minute, lit 60 % of each cycle.
    w.id = kBeaconFx, w.kelvin = 0;
    int on = 0, total = 0;
    for (uint32_t ms = 0; ms < 60000; ms += 10, total++) {
      effect_render(w, &es, 3, false, ms, out);
      on += out[0][0] > 0;
    }
    CHECK(on * 100 / total >= 55 && on * 100 / total <= 65);
    // STROBE at 25 HZ: 25 flashes a second.
    w.id = kStrobeFx, w.hz = 25;
    int flashes = 0;
    bool was = false;
    for (uint32_t ms = 0; ms < 1000; ms++) {
      effect_render(w, &es, 1, false, ms, out);
      bool lit_now = out[0][3] > 0;
      flashes += lit_now && !was;
      was = lit_now;
    }
    CHECK(flashes >= 24 && flashes <= 26);
  }
  uint16_t c[kEffectChannels];
  CHECK(parse_effect_color("AMBER", c) && c[0] == 1000 && c[1] == 450);
  CHECK(parse_effect_color("10/20/30/40/50", c) && c[4] == 500);
  CHECK(!parse_effect_color("10/20", c) && !parse_effect_color("PLAID", c));
  // Fire's palette: ember red, orange, yellow with warm white, white hot with cool white; tint moves the hue.
  uint8_t f[kEffectChannels];
  fire_color(0, 0, f);
  CHECK(f[0] == 0 && f[1] == 0 && f[3] == 0);
  fire_color(60, 0, f);
  CHECK(f[0] > 60 && f[1] < 20 && f[3] == 0);
  fire_color(140, 0, f);
  CHECK(f[0] == 255 && f[1] > 35 && f[1] < 90);
  fire_color(230, 0, f);
  CHECK(f[1] > 150 && f[3] > 150);
  fire_color(255, 0, f);
  CHECK(f[3] == 255 && f[4] > 150);
  uint8_t gold[kEffectChannels], red[kEffectChannels];
  fire_color(140, 100, gold);
  fire_color(140, -100, red);
  CHECK(gold[1] > red[1] && gold[0] == red[0]);
}

int main() {
  test_commands();
  test_blink();
  test_settings();
  test_levels();
  test_brightness_commands();
  test_set_keys();
  test_strip();
  test_effects();
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
