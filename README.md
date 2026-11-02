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

The sketch uses the `ClearCore:sam` Arduino core and the [PubSubClient](https://github.com/knolleary/pubsubclient) library:

```bash
arduino-cli lib install PubSubClient
arduino-cli compile --fqbn ClearCore:sam:clearcore ClearCoreStackLight
```

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

Problems in the file (unknown keys, bad values, two channels on one pin) are printed on the USB serial port at boot. A bad line is ignored; two channels on one pin disables all outputs.

## Topics

| Topic | Direction | Payload |
|---|---|---|
| `…/command` | to the light | A light command (below). Publish it retained, so the light gets it again after a reconnect. |
| `…/status` | from the light | `online`, or `offline` (retained Last Will) |
| `…/event` | from the light | `applied <state>`, `rejected <reason>`, `button`, `resetting` |
| `…/reset` | to the light | `RESET` reboots the board, only with `allow_reset=true` |

A retained `RESET` would reboot the board on every reconnect, so the light ignores a reset that arrives right after it subscribes and clears it from the broker.

## Light commands

A command is one or more `NAME[,MODE]` pairs separated by `;`, applied to a light that starts all off.

- Names: `RED`, `GREEN`, `BLUE`, `YELLOW` and `ORANGE` (both red + green), `MAGENTA`, `CYAN`, `WHITE` and `ALL` (red + green + blue), `ALARM` (the buzzer channel), `FIRE` (alternates red and yellow with the alarm fast), `OFF`
- Modes: `ON` (default), `OFF`, `SLOW`, `FAST`
- Case doesn't matter. An invalid command is rejected and the light keeps its state.

```bash
mosquitto_pub -h <broker> -t stacklight/acme/main-plant/cc-01/command -r -m 'GREEN'
mosquitto_pub -h <broker> -t stacklight/acme/main-plant/cc-01/command -r -m 'YELLOW,SLOW;ALARM,FAST'
mosquitto_pub -h <broker> -t stacklight/acme/main-plant/cc-01/command -r -m 'OFF'
```

The 2024 payloads such as `RED,ON` and `ALL,OFF` still work, with one change: each command now sets the whole light, so `GREEN,ON` turns red off.

## USB serial console

At 115200 baud the USB port takes the same light commands, plus `status` (settings, network, MQTT and light state; never the password) and `help`. That makes it possible to test the wiring before the network is set up.

## Tests

The command parser, the blink timing and the settings parser are plain C++ and run on a PC:

```bash
make -C test
```

## History

This is a rewrite of the 2024 `WorkingComplete.ino` sketch (still in the git history). It keeps the stack-light commands and the settings file, and adds DHCP, configurable pins, correct blink timing, a non-blocking reconnect, retained commands with a Last Will, and tests. The analog inputs and the serial output of the 2024 sketch were dropped.

## License

MIT, see [LICENSE](LICENSE).
