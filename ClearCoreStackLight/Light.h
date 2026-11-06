// Light.h — stack light state, command parsing and blink timing.
//
// Plain C++ with no Arduino dependencies, so it can be unit-tested on a PC (see test/).
//
// A command describes the whole desired state of the light, starting from all off: items separated by ';'.
//   Colour names:  NAME[,MODE]
//     NAME: RED GREEN BLUE YELLOW ORANGE MAGENTA CYAN WHITE ALL ALARM FIRE OFF
//     They drive the pins settings.ini names for red, green, blue and alarm, at full brightness, and the
//     whole strip (unless a STRIP item is given). YELLOW and ORANGE light red + green; WHITE and ALL light
//     red + green + blue; FIRE alternates red and yellow with the alarm fast. OFF clears everything.
//   IO pins:       IOn=PERCENT[,MODE]           n = 0..5, e.g. IO5=26 or IO4=100,SLOW
//     Sets one light output's brightness (0..100 %, one decimal), overriding the colour names for that pin.
//                  IOn=RAMP,[FROM,]TO[,SPEED][,NNNMS]   e.g. IO4=RAMP,50,100,1250MS
//     Rises evenly from FROM (default 0) to TO, then drops straight back: 2 s a rise (SLOW 4, FAST 1), or
//     NNN ms. In step with STRIP RAMP effects of the same period.
//                  IOn=PULSE,[FROM,]TO[,SPEED][,NNNMS]  e.g. IO4=PULSE,50,100,6000MS
//     Eases up from FROM to TO and back down on a cosine, the whole cycle in the period.
//   Strip pixels:  STRIP=R,G,B[,WW,CW][,MODE]   every pixel
//                  STRIP[n]=...                 position n along the strip_layout (0 = the start)
//                  STRIP[a-b]=...               pixels a..b
//     Percent per channel: red, green, blue, warm white, cool white. With any STRIP item the strip starts
//     all off and the items paint it in order, later items over earlier ones.
//   Strip effects: STRIP=NAME[,SPEED][,COLOUR][,COLOUR] or STRIP[a-b]=NAME...: an animation over the range
//     (Effects.h lists them: FIRE, WATER, SPACE, LASER, DISCO, ...). SPEED is SLOW, ON (normal) or FAST;
//     COLOURs are names (RED, AMBER, COOL, ...) or percents r/g/b[/ww/cw]. At most 8 effects per command.
//     RAMP rises from the first colour to the second (or from dark to the only one) and drops back; a period
//     like 1250MS sets its length.
//     Without STRIP items, the FIRE name burns the whole strip.
//   Physical pixels: PIX=..., PIX[n]=..., PIX[a-b]=...   the same values as STRIP (levels or an effect), but on
//     physical pixels (0 = the first after the ClearCore), whatever strip_layout says, and over the STRIP items.
//     A range may run backwards (PIX[38-20]), so an effect can run either way along the strand.
//   MODE: ON (default) OFF SLOW FAST
// Examples: "GREEN"  "RED,FAST"  "YELLOW,SLOW;ALARM,FAST"  "IO4=100;IO5=26;STRIP=100,40,0"
//           "STRIP=0,0,0,0,30;STRIP[0-9]=100,0,0,0,0,FAST"  "STRIP=FIRE,FAST"  "STRIP[0-12]=LASER,FAST,GREEN"
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Effects.h"

namespace sl {

enum Mode : uint8_t { kOff, kOn, kSlow, kFast };
enum Channel : uint8_t { kRed, kGreen, kBlue, kAlarm, kChannels };

const int kIoPins = 6;             // IO0..IO5
const int kLightMaxPixels = 300;   // pixels a command can address
const int kPixelChannels = 5;      // red, green, blue, warm white, cool white

const int kMaxEffects = 8;  // effect items in one command

enum Wave : uint8_t { kSteady, kWaveRamp, kWavePulse };

// Brightness in tenths of a percent (0..1000) and a blink mode, or a wave (see Effects.h) between `from` and
// `level` every ramp_period(speed, ms): RAMP rises and drops straight back, PULSE eases up and down.
struct Output {
  uint16_t level;
  Mode mode;
  Wave wave;
  uint8_t speed;  // wave: 0 = SLOW, 1 = normal, 2 = FAST
  uint16_t from;  // wave: the level it starts each period from
  uint16_t ms;    // wave: the period, 0 = from the speed
};

struct State {
  Mode mode[kChannels];  // colour names, per channel
  bool fire;             // FIRE pattern: drives red, green and alarm itself
  bool io_set[kIoPins];  // an IOn item was given for this pin
  Output io[kIoPins];
  bool strip_set;        // a STRIP item was given: the strip shows pixel[], not the colour names
  uint16_t pixel[kLightMaxPixels][kPixelChannels];  // tenths of a percent
  Mode pixel_mode[kLightMaxPixels];
  uint8_t pixel_fx[kLightMaxPixels];  // 0: the levels above; k: effect fx[k - 1] paints this position
  // PIX items, per physical pixel, drawn over what the layout maps there.
  bool px_set[kLightMaxPixels];
  uint16_t px[kLightMaxPixels][kPixelChannels];  // tenths of a percent
  Mode px_mode[kLightMaxPixels];
  uint8_t px_fx[kLightMaxPixels];  // 0: the levels in px; k: effect fx[k - 1] paints this pixel
  EffectSpec fx[kMaxEffects];
  uint8_t fx_count;
};

struct Timing {
  uint16_t fast_on, fast_off, slow_on, slow_off;  // ms
};

void clear(State* s);

// Parses a command into *out. On error returns false, leaves *out untouched and writes a reason.
// Not reentrant (it parses into a static scratch state, which is large).
bool parse_command(const char* text, size_t len, State* out, char* err, size_t err_cap);
// Only checks a command, in the same scratch state (no State of the caller's needed).
bool check_command(const char* text, size_t len, char* err, size_t err_cap);

// Whether a mode is lit at time now_ms. Blinking follows absolute time, so everything in the same mode blinks
// together.
bool blink(Mode m, const Timing& t, uint32_t now_ms);

// Which colour-name channels are lit at time now_ms (red + green for yellow stay in step).
void lit(const State& s, const Timing& t, uint32_t now_ms, bool out[kChannels]);

// PWM duty (0..255) for each IO pin at time now_ms. pin[c] is the IO pin of channel c (or -1): it follows
// its colour name at full brightness unless an IOn item set that pin.
void io_duty(const State& s, const Timing& t, uint32_t now_ms, const int8_t pin[kChannels], uint8_t duty[kIoPins]);

// PWM duty (0..255) for a brightness level in tenths of a percent (0..1000; more counts as 1000).
uint8_t level_duty(uint16_t tenths);

// Brightness levels are percentages with at most one decimal, e.g. "25" or "27.5", kept in tenths (0..1000).
bool parse_level(const char* text, uint16_t* tenths);
// "25", "27.5" or "100" for a level in tenths.
void format_level(uint16_t tenths, char* out, size_t cap);

// Canonical text for a state, e.g. "YELLOW,SLOW;ALARM,FAST", "IO5=26;STRIP[0-9]=100,0,0,0,0;PIX[12]=0,5,0,0,0".
// A PIX effect over a backwards range is written backwards.
// Parses back to the same state (when it fits in cap; longer descriptions are cut off).
void describe(const State& s, char* out, size_t cap);

}  // namespace sl
