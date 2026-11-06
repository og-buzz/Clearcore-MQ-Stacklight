// Effects.h — animated strip effects: STRIP=NAME[,SPEED][,COLOUR][,COLOUR] (see Light.h).
//
// Plain C++ with no Arduino dependencies, so it can be unit-tested on a PC (see test/).
//
// Options after the name, in any order: a speed (SLOW, ON, FAST), up to two colours (names or r/g/b[/ww/cw]
// percents), a dimmer (a plain percent, e.g. 40), a colour temperature for the white lights (e.g. 4000K), a
// rate for the flashers and the strobe (e.g. 25HZ) and a period for RAMP and PULSE (e.g. 1250MS).
//
// An effect paints a range of strip positions (0 = the start of its range); the strip's layout maps
// positions to physical pixels (Strip.h). Some effects are pure functions of time, others simulate
// (flames, raindrops, sparks) and keep their state in an EffectState, one per effect in the command.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sl {

const int kEffectChannels = 5;  // red, green, blue, warm white, cool white (0..255)
const int kEffectMaxPositions = 300;

enum EffectId : uint8_t {
  kNoEffect,
  // fire and earth
  kFireFx, kCandleFx, kLavaFx, kSunriseFx,
  // water and sky
  kWaterFx, kRainFx, kIceFx, kAuroraFx, kLightningFx,
  // space
  kSpaceFx, kNebulaFx, kMeteorFx,
  // lasers and party
  kLaserFx, kLasersFx, kScannerFx, kDiscoFx, kStrobeFx, kPoliceFx, kConfettiFx, kSparkleFx,
  // colour motion
  kRainbowFx, kPlasmaFx, kCometFx, kTheaterFx, kWipeFx, kMatrixFx,
  // calm
  kBreatheFx, kHeartbeatFx, kTwinkleFx, kFireplaceFx, kLakeFx, kRampFx, kPulseFx,
  // warning lights
  kWigwagFx, kQuadFx, kBeaconFx, kHazardFx,
  // white light
  kLightFx, kDaylightFx, kNeutralFx, kWarmLightFx,
  kEffectCount
};

const char* effect_name(EffectId id);
EffectId effect_by_name(const char* upper_name);  // kNoEffect if unknown

// What an effect item asked for. Colours in tenths of a percent per channel; ncolors says how many were given
// (effects use their own defaults otherwise). speed: 0 = SLOW, 1 = normal, 2 = FAST.
struct EffectSpec {
  EffectId id;
  uint8_t speed;
  uint8_t ncolors;
  uint16_t color[2][kEffectChannels];
  uint16_t level;   // dimmer, tenths of a percent (1000 = full)
  uint16_t kelvin;  // white lights' colour temperature, 0 = the effect's own
  uint8_t hz;       // flash rate for flashers and the strobe, 0 = from the speed
  uint16_t ms;      // RAMP's and PULSE's period in ms, 0 = from the speed
  int16_t first, last;  // the positions the item named; physical pixels for PIX items, where last may be below first
  bool physical;        // a PIX item: drawn on physical pixels from first to last, over the layout
};

// RAMP: rises evenly from one level to another over a period, then drops straight back. The phase follows
// absolute time (now % period), so ramps with the same period rise and drop together, strip and IO pins alike.
const uint16_t kRampMinMs = 100, kRampMaxMs = 60000;
// The period: ms when given, otherwise 2 s at the normal speed (SLOW 4 s, FAST 1 s).
uint32_t ramp_period(uint8_t speed, uint16_t ms);
// The level at time now between from and to (any units), e.g. tenths of a percent.
uint16_t ramp_level(uint16_t from, uint16_t to, uint32_t period, uint32_t now);

// PULSE: eases from one level to the other and back on a cosine, no hard edges; the whole cycle (up and down)
// takes one ramp_period, and the phase follows absolute time like RAMP's. The fraction (0..1) of the way from
// `from` to `to` at time now, and the level itself.
float pulse_fraction(uint32_t period, uint32_t now);
uint16_t pulse_level(uint16_t from, uint16_t to, uint32_t period, uint32_t now);

struct EffectState {
  uint8_t a[kEffectMaxPositions];  // per-position values: heat, brightness, trail...
  uint8_t b[kEffectMaxPositions];
  int8_t t[kEffectMaxPositions];   // per-position tint
  uint32_t next_ms;                // next simulation step
  uint32_t rng;
  int32_t v[8];                    // effect-specific: particle positions, timers
  bool started;
};

void effect_reset(EffectState* st, uint32_t seed);

// Paints `count` positions (relative to the effect's range) for time now_ms into out. wrap: the positions form
// a closed ring (moving things go round instead of starting over).
void effect_render(const EffectSpec& spec, EffectState* st, int count, bool wrap, uint32_t now_ms,
                   uint8_t (*out)[kEffectChannels]);

// A named colour (RED, ORANGE, AMBER, YELLOW, GREEN, CYAN, BLUE, PURPLE, MAGENTA, PINK, WARM, COOL, WHITE) or
// percents "r/g/b[/ww/cw]" into tenths. Returns false if it's neither.
bool parse_effect_color(const char* upper_text, uint16_t out[kEffectChannels]);

// Fire's palette: black, ember red, orange, amber, yellow, then warm and finally cool white for the hottest
// heat. tint shifts the hue (-100 redder .. 100 golden).
void fire_color(uint8_t heat, int8_t tint, uint8_t out[kEffectChannels]);

}  // namespace sl
