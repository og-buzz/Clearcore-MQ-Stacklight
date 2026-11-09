# ClearCore MQTT Stack Light

Drive a stack light from MQTT with a [Teknic ClearCore](https://www.teknic.com/products/io-motion-controller-clearcore/). The ClearCore joins the network over Ethernet, subscribes to a command topic and sets its outputs, so anything that can publish MQTT (a script, Node-RED, a monitoring system) can turn the light red, green or blinking.

- One retained command describes the whole state of the light, so it comes back right after a reboot or a dropped connection.
- Settings live in `settings.ini` on the ClearCore's microSD card: one build serves every unit.
- Nothing is driven until `settings.ini` names the output pins.

## Hardware

- A Teknic ClearCore with a microSD card and an Ethernet connection to the broker's network.
- A 24 V stack light wired to the ClearCore's IO-0..IO-5 outputs, one output per colour (and optionally one for the buzzer).
- Optional: a push button on any IO or DI input.

## Build and flash

The sketch uses the `ClearCore:sam` Arduino core, the [PubSubClient](https://github.com/knolleary/pubsubclient)
library and [ClearCoreWatchdog](https://github.com/og-buzz/ClearCoreWatchdog) 1.1.0 or later (see
[Watchdog](#watchdog)):

```bash
arduino-cli lib install PubSubClient
arduino-cli config set library.enable_unsafe_install true   # needed for --git-url
arduino-cli lib install --git-url https://github.com/og-buzz/ClearCoreWatchdog.git#v1.1.0
arduino-cli compile --fqbn ClearCore:sam:clearcore ClearCoreStackLight
```

With a checkout of ClearCoreWatchdog instead, add `--library /path/to/ClearCoreWatchdog` to the compile.

To install the toolchain and flash the board from Linux, WSL2 or Windows, use [teknic-clearcore-cli](https://github.com/og-buzz/teknic-clearcore-cli):

```bash
scripts/flash.sh /path/to/ClearCoreStackLight          # Linux, WSL2
powershell -File scripts\flash.ps1 C:\path\to\ClearCoreStackLight   # Windows
```

## Settings

Copy [`settings.example.ini`](settings.example.ini) to the root of the microSD card as `settings.ini` and edit it. The main keys:

| Key | Meaning |
|---|---|
| `broker`, `port`, `user`, `password` | MQTT broker; leave `user` empty for an anonymous broker |
| `project`, `client`, `site`, `role` | Build the topic path `{project}/{client}/{site}/{role}/{kind}` |
| `asset` | The unit's asset tag (e.g. `cc-0001`); used as the MQTT client ID |
| `ip` | `dhcp` (default) or a static address, with `gateway`, `dns` and `subnet` |
| `red`, `green`, `blue`, `alarm` | Output for each channel: `IO0`..`IO5` or `none` |
| `button` | Input for a push button: `IO0`..`IO5`, `DI6`..`DI8` or `none` |
| `fast_on`, `fast_off`, `slow_on`, `slow_off` | Blink timing in milliseconds |
| `allow_reset` | `true` to allow a reboot over MQTT (default `false`) |
| `transition`, `transition_sparkle` | How a change of look fades in: over `transition` ms (default `2000`, `0` = at once), with sparkles at least `transition_sparkle` % bright (default `20`, `0` = none); see [Transitions](#transitions) |
| `scene_<name>` | A named light command (at most 16, 240 characters each), e.g. `scene_busy=IO4=100;STRIP=0,0,0,3,0;PIX[12]=5,0,0`. The Teams states `available`, `busy`, `call`, `dnd`, `away`, `brb`, `offline`, `unknown` and `stale` have built-in looks (`GREEN`, `RED`, `RED`, `RED,SLOW`, `YELLOW`, `YELLOW,SLOW`, `OFF`, `OFF`, `OFF`) that a scene of the same name replaces |
| `owner`, `presence` | Follow a person: with both set, the light subscribes to `<presence>/<owner>` and shows the scene named by each state published there, and the `stale` scene while `<presence>/publisher` is `offline` |
| `allow_config` | `true` to accept `scene_*` settings on `…/config` (default `false`) |
| `strip`, `strip_pixels` | A WS2805 strip on `COM0` or `COM1` with this many pixels (default `none`) |
| `strip_order` | Colour order on the wire: `RGB` (default, WS2805 datasheet), `GRB` (BTF's WS2805 strip), or any other order of R, G and B |
| `strip_timing` | Signal timing `0`..`5` (default `4`, see below) |
| `strip_layout` | How the strip is laid out; pixel numbers in commands are positions along it: `line` (default), `ring` (moving effects go round), `fold` (folded at its middle and run back, both legs side by side), or a map of pixel ranges in position order, `|` starting a leg beside the first: `0-24|49-25` is `fold` for 50 pixels, `24-0,25-49` runs centre-out, `0-16|33-17|34-49` is three legs |

Problems in the file (unknown keys, bad values, two channels on one pin) are printed on the USB serial port at boot. A bad line is ignored; two channels on one pin disables all outputs.

## Topics

| Topic | Direction | Payload |
|---|---|---|
| `…/command` | to the light | A light command (below). Publish it retained, so the light gets it again after a reconnect. |
| `…/status` | from the light | `online`, or `offline` (retained Last Will) |
| `…/event` | from the light | `applied <state>`, `rejected <reason>`, `button`, `resetting`, `reset watchdog in <step> after <n> s` (or `reset watchdog (no breadcrumb)` when the early warning couldn't run), once after a watchdog reset |
| `…/reset` | to the light | `RESET` reboots the board, only with `allow_reset=true` |
| `…/scene` | to the light | A scene name: shows that scene. Publish it not retained |
| `…/config` | to the light | Only with `allow_config=true`: `scene_<name>=<command>` saves a scene to `settings.ini` and uses it at once, `scene_<name>=` removes it; anything else is rejected. Reported on `…/event` as `config saved …`, `config removed …` or `config rejected …`. Publish it not retained |
| `<presence>/<owner>` | to the light | With `owner` and `presence` set: a Teams state (`busy`, `dnd`, …), shown as the scene of that name |
| `<presence>/publisher` | to the light | `online`, or `offline` (the publisher's Last Will): shows the `stale` scene until it is back |

A retained `RESET` would reboot the board on every reconnect, so the light ignores a reset that arrives right after it subscribes and clears it from the broker. Retained `…/scene` and `…/config` messages are ignored the same way, and a light that follows presence ignores the retained `…/command` too, taking its state from the presence topic instead.

## Light commands

A command describes the whole light, starting from all off: items separated by `;`. Brightness comes with the
command, so the publisher decides the colours.

| Item | Does |
|---|---|
| `NAME[,MODE]` | `RED`, `GREEN`, `BLUE`, `YELLOW` and `ORANGE` (red + green), `MAGENTA`, `CYAN`, `WHITE` and `ALL` (red + green + blue), `ALARM`, `FIRE` (red and yellow alternating, alarm fast), `OFF` (clears everything). Lights the pins `settings.ini` names for those channels at full brightness, and the whole strip unless a `STRIP` item is given |
| `IOn=PERCENT[,MODE]` | Light output `IO0`..`IO5` at `0`..`100` % (one decimal), e.g. `IO5=26`. Overrides the names for that pin; only pins named in `settings.ini` (`red=`, `green=`, `blue=`, `alarm=`) are accepted |
| `IOn=RAMP,[FROM,]TO[,SPEED][,NNNMS]` | The output rises evenly from `FROM` % (default 0) to `TO` %, then drops straight back: 2 s a rise (`SLOW` 4, `FAST` 1), or a period in ms, e.g. `IO4=RAMP,50,100,1250MS`. In step with `RAMP` strip effects of the same period |
| `IOn=PULSE,[FROM,]TO[,SPEED][,NNNMS]` | The output eases up from `FROM` % to `TO` % and back down on a cosine, no hard edges; the period is the whole cycle, e.g. `IO4=PULSE,50,100,6000MS`. In step with `PULSE` strip effects of the same period |
| `STRIP=R,G,B[,WW,CW][,MODE]` | Every pixel: red, green, blue, warm white, cool white in percent |
| `STRIP[n]=…`, `STRIP[a-b]=…` | Pixel `n`, or pixels `a`..`b` (0 = the end at the ClearCore). With any `STRIP` item the strip starts dark and the items paint it in order |
| `PIX=…`, `PIX[n]=…`, `PIX[a-b]=…` | The same values as `STRIP` (levels and a mode, or an effect), on **physical** pixels (0 = the first after the ClearCore) whatever `strip_layout` says, over the `STRIP` items. A range may run backwards, `PIX[38-20]=FIRE`, so an effect runs either way along the strand. `strip_layout` shapes how effects flow; `PIX` picks out pixels |
| `STRIP=EFFECT[,options]`, `STRIP[a-b]=EFFECT…` | An effect over the range (see [Strip effects](#strip-effects)). Options, in any order: a speed `SLOW`, `ON` (normal) or `FAST`; up to two colours, names (`RED`, `ORANGE`, `AMBER`, `YELLOW`, `GREEN`, `CYAN`, `BLUE`, `PURPLE`, `MAGENTA`, `PINK`, `WARM`, `COOL`, `WHITE`) or percents `r/g/b[/ww/cw]`; a dimmer, a plain percent (`40`); a colour temperature for the white lights (`4000K`); a rate for the flashers and the strobe (`25HZ`); a period for `RAMP` and `PULSE` (`1250MS`, 100..60000). Up to 8 effects in one command. Without `STRIP` items, the `FIRE` name burns the whole strip |

Modes: `ON` (default), `OFF`, `SLOW`, `FAST`; everything in the same mode blinks in step. Case doesn't matter.
An invalid command is rejected (reported on the event topic) and the light keeps its state.

```bash
T=stacklight/acme/main-plant/cc-01/command
mosquitto_pub -h <broker> -t $T -r -m 'GREEN'
mosquitto_pub -h <broker> -t $T -r -m 'YELLOW,SLOW;ALARM,FAST'
mosquitto_pub -h <broker> -t $T -r -m 'IO4=100;IO5=26;STRIP=100,40,0'          # balanced yellow
mosquitto_pub -h <broker> -t $T -r -m 'STRIP=0,0,0,0,30;STRIP[0-9]=100,0,0,0,0,FAST'
mosquitto_pub -h <broker> -t $T -r -m 'STRIP=FIRE,FAST'
mosquitto_pub -h <broker> -t $T -r -m 'OFF'
```

The 2024 payloads such as `RED,ON` and `ALL,OFF` still work, with one change: each command now sets the whole light, so `GREEN,ON` turns red off.

## Transitions

When a command, scene or presence state changes the look, the light fades from what it was showing into the new
look over `transition` ms (default 2000) instead of switching at once:

- the light outputs crossfade, eased, from their old brightness to the new;
- on the strip a soft front sweeps from position 0 to the end of the `strip_layout` (on a `fold`, up both legs
  together), each pixel easing into the new look as the front passes, and sparkles flash and fade out where the
  front is crossing. They take the colour of the new look's brightest pixel (the old look's when the new one is
  dark), at `transition_sparkle` % (default 20) or that pixel's own level if it is brighter.

The new look is live during the fade, so a pulsing or blinking look already moves. A change during a transition
starts from what is showing, and the same command again (a retained resend after a reconnect) doesn't fade.
`transition=0` switches at once, as before 2.7.

## Strip effects

| Group | Effect | Look | Colours (defaults) |
|---|---|---|---|
| Fire and earth | `FIRE` | Flames from the start of the range: ember red, orange, amber, yellow, white-hot with both whites, hues drifting | – |
| | `CANDLE` | Soft warm-white flicker, each position on its own | – |
| | `LAVA` | Slow molten blobs of red and orange | – |
| | `SUNRISE` | Night, dawn red, orange, warm white, daylight and back, 60 s a cycle (SLOW 120, FAST 30), brightest at the start | – |
| Water and sky | `WATER` | Rolling blue and cyan waves with white caps and glints | – |
| | `RAIN` | Drops running from the far end with short trails | light blue |
| | `ICE` | Cool white and cyan shimmer with glints | – |
| | `AURORA` | Green, teal and violet curtains waving slowly | – |
| | `LIGHTNING` | Dark storm, bursts of white flashes over part of the range | – |
| Space | `SPACE` | Deep blue night, twinkling stars, a shooting star now and then | – |
| | `NEBULA` | Slow purple, magenta and blue clouds with faint stars | – |
| | `METEOR` | Meteors with trails that break up as they fade | blue-white |
| Lasers and party | `LASER` | One fast beam bouncing end to end, hot core and glow | red |
| | `LASERS` | Three beams crossing at different speeds | red, green (+ blue) |
| | `SCANNER` | A bouncing eye | red |
| | `DISCO` | Blocks of random colours on the beat (120 bpm), mirror-ball sparkles | – |
| | `STROBE` | White flashes, 10 Hz (SLOW 4, FAST 20, or any rate up to 60 with `NNHZ`), 30 ms each, shorter above 13 Hz | white |
| | `CONFETTI` | Random coloured sparks fading out | – |
| | `SPARKLE` | Random pixels flash for a single frame (about 2 ms on a short strip) over the second colour, random along the whole strand whatever the layout (not mirrored on its legs): 25 a second over the range (SLOW 10, FAST 50, or `NNHZ`); `STRIP=SPARKLE,0/0/0/0/40` | cool white, off |
| Colour motion | `RAINBOW` | A rainbow scrolling along the range | – |
| | `PLASMA` | Psychedelic interference colours | – |
| | `COMET` | A comet with a fading tail, again and again (on a ring, round) | cyan-white |
| | `THEATER` | Marching marquee lights | amber, off |
| | `WIPE` | Fill from the start, hold, empty, hold | blue, off |
| | `MATRIX` | Green code rain with white-hot heads | green |
| Calm | `BREATHE` | A slow fade in and out, 4 s | soft blue |
| | `HEARTBEAT` | Lub-dub double pulse | red |
| | `TWINKLE` | Soft twinkles on a dim background | warm white, dim warm |
| | `FIREPLACE` | `FIRE`, calm: slow low embers at half brightness over a warm glow, never white-hot | – |
| | `LAKE` | `WATER`, calm: a slow swell, deeper and dimmer, the odd glint | – |
| | `RAMP` | Rises evenly from the first colour to the second (from dark with one colour), then drops straight back: 2 s (SLOW 4, FAST 1) or `NNNMS`. The phase follows the clock, so ramps with the same period, on the strip and on `IOn=RAMP` outputs, rise and drop together | warm white |
| | `PULSE` | Eases from the first colour to the second and back on a cosine (from dark with one colour), the whole cycle in 2 s (SLOW 4, FAST 1) or `NNNMS`; in step with `IOn=PULSE` outputs of the same period | warm white |
| Warning lights | `POLICE` | Sides alternating, a double flash each: 120 flashes a minute per side (SLOW 60, FAST 240) | red, blue |
| | `WIGWAG` | Sides alternating, one flash each: 75 a minute per side (SLOW 60, FAST 120) | red, blue |
| | `QUAD` | Sides alternating, four quick flashes each, 60 cycles a minute (SLOW 30, FAST 120) | red, blue |
| | `BEACON` | One flash for the range: 55 a minute (SLOW 50, FAST 60), lit 60 % of each cycle | amber |
| | `HAZARD` | Sides alternating, two quick flashes each, 75 sequences a minute | amber, amber |
| White light | `LIGHT` | Steady white, 4000 K, from the strip's 3000 K and 6500 K LEDs (below 3000 K, down to 2200 K, some red warms it); `LIGHT,2700K,30` | – |
| | `DAYLIGHT`, `NEUTRAL`, `WARMLIGHT` | `LIGHT` at 6500, 4500 and 3000 K | – |

```bash
mosquitto_pub -h <broker> -t $T -r -m 'STRIP=AURORA,SLOW'
mosquitto_pub -h <broker> -t $T -r -m 'STRIP[0-12]=WATER;STRIP[13-24]=FIRE'
mosquitto_pub -h <broker> -t $T -r -m 'STRIP=WARMLIGHT,20;STRIP[24-25]=LASER,FAST,GREEN'
mosquitto_pub -h <broker> -t $T -r -m 'STRIP=LIGHT,4000K,60'                       # desk light, dimmed
mosquitto_pub -h <broker> -t $T -r -m 'STRIP=QUAD,RED,WHITE'                       # fire-engine colours
mosquitto_pub -h <broker> -t $T -r -m 'IO4=RAMP,50,100,1250MS;STRIP[12]=RAMP,2/0/0,6/0/0,1250MS'  # in step
```

The flash rates follow published guidance where there is some: `BEACON` keeps to the MUTCD's flashing beacon
rule (50-60 flashes a minute, lit for 1/2 to 2/3 of each cycle), `HAZARD` uses the MUTCD's 75 sequences a
minute for rapid-flashing beacons (the pattern itself is simplified), and `POLICE`, `WIGWAG` and `QUAD` stay
within 60-240 flashes a minute, the range commonly cited for SAE J595 warning lamps. None of this makes the
strip a certified warning device. Flashing between about 3 and 30 Hz (`STROBE`, fast flashers) can trigger
seizures in people with photosensitive epilepsy.

For laying out a new installation, `strip_index` on the USB console lights the physical pixels in wiring order in blocks of 5 (red, orange, yellow, green, cyan, blue, purple, pink, warm white, cool white), each block fading 100, 32, 10, 3, 1 %, so every pixel can be named ("the 2nd green"). `strip_index off` ends it.

## Addressable strip

A WS2805 RGBCCT strip on COM-0 or COM-1 shows the `STRIP` items of each command, or, without them, the colour
names on every pixel. The ClearCore has no fast logic-level output, so the WS2805 signal is made
on the COM port's SPI data line by DMA. With the default timing 4, every data bit is 13 SPI bits at 12 MHz:
`0` = 333 ns high / 750 ns low, `1` = 750 / 333 ns (NeoPixelBus/WLED use 300/790 and 790/300 ns for the
WS2805), then a 300 µs reset. 12 MHz needs the port's SPI clocked from the ClearCore's 48 MHz generator
instead of the library's 10 MHz one; the sketch does that. It also turns off the port's hardware slave
select, which the ClearCore library enables for SPI and which pauses about three clock periods after every
byte: enough to stretch pulses at every byte boundary and make the strip misread its data. Each frame also
starts with a few bytes of low line: the first bit an SPI transfer puts out can come out clipped, and the
strip then loses it and shifts every pixel by one bit.

Each pixel is five bytes: three colours, then warm white and cool white. The colour order varies by strip;
BTF's WS2805 strip wants `strip_order=GRB`. A frame of 50 pixels takes 2.5 ms, so the strip can change up to
about 400 times a second.

| RJ-45 pin (COM port) | Strip |
|---|---|
| 5 (data) | DIN, and BIN (backup data) on dual-data strips: BTF's WS2805 manual joins both to the data output |
| 4 (GND) | GND, also to the 24 V supply's negative |
| 2, 6 (5 V), 1, 3, 7, 8 | not connected |

Power the strip from the 24 V supply, not from the ClearCore, and connect to the end where the arrows on
the strip point away from the connection.

## USB serial console

At 115200 baud the USB port takes the same light commands, plus `status` (settings, network, MQTT, watchdog and light state; never the password), `set`, `unset`, `scene`, `scenes`, `reboot`, `watchdog_test` and `help`. `set <key> <value>` checks a setting the way `settings.ini` is read and writes it into the file, keeping every other line (the previous version goes to `settings.bak`), e.g. `set strip COM0`; `unset <key>` removes a key's lines; `reboot` applies them. Scenes (`set scene_busy …`, `unset scene_busy`) apply at once. `scene <name>` shows a scene; `scenes` lists the ones on the SD card.

For bringing up a new strip: `strip_index` (above); `strip_timing [n]` lists or switches the signal timing until a reboot; `strip_raw <hex>` sends the same raw bytes to every pixel (e.g. `strip_raw 000000ff00` lights only the first white) until the light changes; `strip_loopback`, with a wire from the COM port's pin 5 to pin 3, reads the data line back and times a frame to check the bytes go out without pauses. That makes it possible to test the wiring before the network is set up.

`watchdog_test` hangs the firmware on purpose (it says so first) so the watchdog reset can be tried on the bench:
the board resets after about 16 s and `status` then shows `last reset watchdog in watchdog-test after <n> s`. It
exists only on the USB console, never over MQTT.

## Watchdog

The hardware watchdog resets the board when the firmware hangs: 16 s without a kick, in normal mode (not
Always-On, so flashing and the bootloader are never raced). Every call that can block is bounded below that:
the MQTT connect (TCP connect 2 s, CONNACK and each stalled packet 3 s, plus 2 s DNS for a broker host name),
DHCP (5 tries of 1.5 s), settings writes to the SD card (kicked every 512 bytes; a whole write takes 4-6 s), and
USB console output (a host that keeps the port open but doesn't read loses lines instead of stopping the
light). The console runs one command per loop pass, and `reboot` and `RESET` no longer wait for the broker
connection to drain; the broker's Last Will says `offline` as before.

The loop records which step it is in (`console`, `network`, `dhcp`, `mqtt-connect`, `mqtt-loop`, `button`,
`strip`, `light`, `sd-read`, `sd-write`, `reboot`, `watchdog-test`). The watchdog's early warning, 8 s after the
last kick, saves that step and the uptime in the SAME53's backup RAM, which a reset keeps. After a watchdog
reset the light logs it on the USB port, publishes `reset watchdog in mqtt-connect after 3600 s` (for example)
once on `…/event` when MQTT connects, and `status` shows it on its `watchdog:` line, e.g.
`watchdog: 16 s, last reset watchdog in mqtt-connect after 3600 s, 0 early warning(s)`. A hang with interrupts
off gives `watchdog (no breadcrumb)`; other resets show their cause (`power-on`, `software`, `reset pin`, ...).
An early warning without a reset (a step that took 8-16 s) is logged and counted, and its breadcrumb dropped.

Flashing doesn't clear Always-On: a board that last ran firmware which set it (such as ClearCoreWatchdog's
`AlwaysOnEarlyWarning` example) keeps that firmware's period and early warning until a power cycle. The light
then reads them back, kicks from the start of `setup()`, and shows them in `status`, e.g. `watchdog: Always-On
8 s, early warning at 4 s, locked by earlier firmware until a power cycle; ...`. A period under 12 s leaves no
margin for DHCP and the MQTT connect, so the light keeps the network off and says to power-cycle it.

The watchdog comes from [ClearCoreWatchdog](https://github.com/og-buzz/ClearCoreWatchdog), which needs its
early-warning support: [1.1.0](https://github.com/og-buzz/ClearCoreWatchdog/releases/tag/v1.1.0) or later.

## Tests

The command parser, the blink timing, the settings parser and the watchdog breadcrumb are plain C++ and run on
a PC:

```bash
make -C test
```

## Roadmap

- ~~Owner and look on the SD card~~: done in 2.5 (`owner`, `presence`, `scene_<name>`, `PIX`, `…/scene`,
  `…/config`). status-master still has to publish state names to `<presence>/<owner>` (step 2 of
  [its roadmap](https://github.com/og-buzz/status-master#roadmap)).
- **Broker credentials and ACLs** once presence for everyone is on the broker: lights read presence and their own
  command topic, nothing else.

## History

This is a rewrite of the 2024 `WorkingComplete.ino` sketch (still in the git history). It keeps the stack-light commands and the settings file, and adds DHCP, configurable pins, correct blink timing, a non-blocking reconnect, retained commands with a Last Will, and tests. The analog inputs and the serial output of the 2024 sketch were dropped.

## License

MIT, see [LICENSE](LICENSE).
