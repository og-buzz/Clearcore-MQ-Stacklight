// Light.h — stack light state, command parsing and blink timing.
//
// Plain C++ with no Arduino dependencies, so it can be unit-tested on a PC (see test/).
//
// A command describes the whole desired state of the light, starting from all off:
//   one or more NAME[,MODE] pairs separated by ';'
//   NAME: RED GREEN BLUE YELLOW ORANGE MAGENTA CYAN WHITE ALL ALARM FIRE OFF
//   MODE: ON (default) OFF SLOW FAST
// Examples: "GREEN"  "RED,FAST"  "YELLOW,SLOW;ALARM,FAST"  "FIRE"  "OFF"
// YELLOW and ORANGE both light red + green (what that looks like depends on the light).
// WHITE and ALL light red + green + blue. FIRE alternates red and yellow with the alarm fast.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sl {

enum Mode : uint8_t { kOff, kOn, kSlow, kFast };
enum Channel : uint8_t { kRed, kGreen, kBlue, kAlarm, kChannels };

struct State {
  Mode mode[kChannels];
  bool fire;  // FIRE pattern: drives red, green and alarm itself
};

struct Timing {
  uint16_t fast_on, fast_off, slow_on, slow_off;  // ms
};

void clear(State* s);

// Parses a command into *out. On error returns false, leaves *out untouched and writes a reason.
bool parse_command(const char* text, size_t len, State* out, char* err, size_t err_cap);

// Which channels are lit at time now_ms. Blinking follows absolute time, so channels in the
// same mode blink together (red + green for yellow stay in step).
void lit(const State& s, const Timing& t, uint32_t now_ms, bool out[kChannels]);

// Canonical text for a state, e.g. "YELLOW,SLOW;ALARM,FAST" or "OFF". Parses back to the same state.
void describe(const State& s, char* out, size_t cap);

}  // namespace sl
