// Host tests for the parts of ClearCoreStackLight that don't touch hardware.
// Build and run: make -C test
#include <stdio.h>
#include <string.h>

#include "../ClearCoreStackLight/Light.h"
#include "../ClearCoreStackLight/Settings.h"

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

int main() {
  test_commands();
  test_blink();
  test_settings();
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
