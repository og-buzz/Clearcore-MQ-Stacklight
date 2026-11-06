// ClearCoreStackLight — drive a stack light from MQTT on a Teknic ClearCore.
//
// The ClearCore joins the network over Ethernet, connects to an MQTT broker and subscribes to a
// command topic. Each command describes the whole state of the light, e.g. "GREEN", "RED,FAST" or
// "YELLOW,SLOW;ALARM,FAST" (see Light.h). Settings come from settings.ini on the SD card (see
// settings.example.ini); nothing is driven until that file names the output pins.
//
// Topics, following {project}/{client}/{site}/{role}/{kind}:
//   .../command  subscribed; retained, so the light shows the current state after any reconnect
//   .../status   "online", or "offline" as the retained Last Will
//   .../event    "applied <state>", "rejected <reason>", "button"
//   .../reset    subscribed only with allow_reset=true; "RESET" reboots the board
//   .../scene    subscribed; a scene name: shows that scene (scene_<name> in settings.ini, or a built-in look)
//   .../config   subscribed only with allow_config=true; "scene_<name>=<command>" saves a scene to settings.ini and
//                uses it at once, "scene_<name>=" removes it; nothing else can be set this way
//   <presence>/<owner>     with owner= and presence= set: a Teams state (busy, dnd, ...) shows the scene of that name
//   <presence>/publisher   "offline" (the publisher's Last Will) shows the stale scene until it is back
//
// Scenes are named light commands on the SD card (scene_busy=IO4=100;STRIP=0,0,0,3,0;PIX[12]=5,0,0); the Teams
// states have built-in looks a scene of the same name replaces. PIX items address physical pixels (see Light.h).
//
// Commands carry the brightness: colour names light their pins at full brightness, IOn=PERCENT sets one light
// output (PWM), and STRIP items set the pixels of an addressable WS2805 strip on COM-0 or COM-1 (strip= and
// strip_pixels= in settings.ini); without STRIP items the strip shows the colour names (see Light.h, Strip.h).
//
// The USB serial port (115200 baud) takes the same commands, plus "status", "set", "unset", "reboot", "help"
// and the strip tests "strip_timing", "strip_raw" and "strip_loopback".
//
// FQBN: ClearCore:sam:clearcore. Needs the PubSubClient library (arduino-cli lib install PubSubClient).

#include <ClearCore.h>
#include <Ethernet.h>
#include <PubSubClient.h>
#include <SD.h>
#include <stdarg.h>
#include <strings.h>

#include "Light.h"
#include "Settings.h"
#include "Strip.h"

namespace {

const char* const kVersion = "2.5.0";
const char* const kSettingsFile = "settings.ini";
const char* const kBackupFile = "settings.bak";  // the previous settings.ini, written by "set" and "unset"
const uint32_t kNetRetryMs = 10000;
const uint32_t kMqttBackoffMinMs = 2000, kMqttBackoffMaxMs = 60000;
const uint32_t kStaleResetWindowMs = 3000;  // retained reset messages arrive right after subscribing
const uint32_t kDebounceMs = 50;

const int kArduinoPin[sl::kPinCount] = {IO0, IO1, IO2, IO3, IO4, IO5, DI6, DI7, DI8};
// Light outputs by pin (IO0..IO5), driven through the ClearCore API so they can run as PWM.
DigitalInOut* const kOutput[6] = {&ConnectorIO0, &ConnectorIO1, &ConnectorIO2,
                                  &ConnectorIO3, &ConnectorIO4, &ConnectorIO5};
sl::Settings cfg;
sl::State light;
int16_t written[sl::kIoPins];  // last PWM duty written per IO pin; -1 = not a light output

// Light outputs are the IO pins settings.ini names for red, green, blue and alarm.
bool is_light_output(int pin) {
  for (uint8_t c = 0; c < sl::kChannels; c++)
    if (cfg.pin[c] == pin) return true;
  return false;
}

// Strip: one DMA buffer, refilled only while the previous frame isn't on the wire.
const uint32_t kStripRefreshMs = 1000;  // resend the frame this often even when nothing changed
SerialDriver* strip_com = nullptr;       // set when a strip is configured
DmaChannels strip_dma;
uint8_t strip_buf[sl::kStripMaxFrameBytes] __attribute__((aligned(4)));
sl::Layout strip_map;                                          // pixel -> position (strip_layout)
sl::StripEffects strip_fx;                                     // the running effects' state
uint8_t strip_pix[sl::kStripMaxPixels * sl::kStripChannels];   // this loop's pixel bytes, wire order
uint8_t strip_last[sl::kStripMaxPixels * sl::kStripChannels];  // the frame last sent
bool strip_sent_once = false;
uint32_t strip_last_send = 0, strip_frames = 0;
// "strip_raw" test pattern: sent instead of the light's colour until the light changes or "strip_raw" alone.
uint8_t strip_raw_bytes[8];
int strip_raw_len = 0;
bool strip_raw_sent = false;
// "strip_index": every physical pixel in wiring order, blocks of 5 in rainbow order, each block fading from
// 100 % to 1 % (100, 32, 10, 3, 1), for working out how the strip is laid out. Stays on (through light changes) until "strip_index off".
bool strip_index_on = false;
const uint8_t kIndexColors[10][sl::kStripChannels] = {
    {255, 0, 0, 0, 0},   {255, 45, 0, 0, 0},  {255, 120, 0, 0, 0}, {0, 255, 0, 0, 0}, {0, 170, 255, 0, 0},
    {0, 0, 255, 0, 0},   {130, 0, 255, 0, 0}, {255, 0, 110, 0, 0}, {0, 0, 0, 255, 0}, {0, 0, 0, 0, 255}};
const char* const kIndexNames[10] = {"red",  "orange", "yellow", "green",      "cyan",
                                     "blue", "purple", "pink",   "warm white", "cool white"};
// Starts sending len bytes of strip_buf.
void strip_send(size_t len) {
  if (!strip_com->SpiTransferDataAsync(strip_buf, nullptr, (int32_t)len)) {
    strip_com->SpiTransferData(strip_buf, nullptr, (int32_t)len);  // no DMA on this port: send blocking
  }
  strip_last_send = millis();
  strip_frames++;
}
int settings_errors = 0;
bool settings_loaded = false;
bool mqtt_enabled = false;

char t_command[160], t_status[160], t_event[160], t_reset[160], t_scene[160], t_config[160];
char t_presence[160], t_publisher[160];  // with owner= and presence=
char showing_scene[sl::kSceneNameMax + 1];  // the scene on the light now; empty after a plain command
char presence_state[sl::kSceneNameMax + 1];  // the owner's last state
bool publisher_offline = false;
sl::Settings check_cfg;  // a copy to try a setting on before saving it (set, config): large, so shared

EthernetClient eth;
PubSubClient mqtt(eth);
bool net_up = false;
uint32_t next_net_try = 0, next_mqtt_try = 0, mqtt_backoff = kMqttBackoffMinMs, subscribed_at = 0;

void log_line(const char* fmt, ...) {
  static char buf[640];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);
  Serial.println(buf);
}

// --- settings --------------------------------------------------------------------------------

void load_settings() {
  sl::defaults(&cfg);
  if (!SD.begin()) {
    log_line("settings: no SD card; outputs stay off and MQTT is disabled");
    return;
  }
  File f = SD.open(kSettingsFile);
  if (!f) {
    log_line("settings: %s not found on the SD card; outputs stay off and MQTT is disabled", kSettingsFile);
    return;
  }
  char line[320];
  size_t len = 0;
  int line_no = 0;
  bool overflow = false;
  while (true) {
    int c = f.read();
    if (c == '\r') continue;
    if (c == '\n' || c < 0) {
      if (len > 0 || overflow) {
        line_no++;
        line[len] = '\0';
        char err[96];
        if (overflow) {
          log_line("settings: line %d is too long; ignored", line_no);
          settings_errors++;
        } else if (!sl::apply_line(&cfg, line, err, sizeof err)) {
          log_line("settings: line %d: %s; ignored", line_no, err);
          settings_errors++;
        }
      } else {
        line_no++;
      }
      len = 0;
      overflow = false;
      if (c < 0) break;
      continue;
    }
    if (len < sizeof line - 1) line[len++] = (char)c;
    else overflow = true;
  }
  f.close();
  settings_loaded = true;

  char err[96];
  if (!sl::pins_ok(cfg, err, sizeof err)) {
    log_line("settings: %s; all outputs disabled", err);
    for (uint8_t c = 0; c < sl::kChannels; c++) cfg.pin[c] = sl::kNoPin;
    cfg.button = sl::kNoPin;
    settings_errors++;
  }
  mqtt_enabled = sl::mqtt_ready(cfg, err, sizeof err);
  if (!mqtt_enabled) log_line("settings: MQTT disabled (%s)", err);
  sl::topic(cfg, "command", t_command, sizeof t_command);
  sl::topic(cfg, "status", t_status, sizeof t_status);
  sl::topic(cfg, "event", t_event, sizeof t_event);
  sl::topic(cfg, "reset", t_reset, sizeof t_reset);
  sl::topic(cfg, "scene", t_scene, sizeof t_scene);
  sl::topic(cfg, "config", t_config, sizeof t_config);
  if (sl::follows_presence(cfg)) {
    snprintf(t_presence, sizeof t_presence, "%s/%s", cfg.presence, cfg.owner);
    snprintf(t_publisher, sizeof t_publisher, "%s/publisher", cfg.presence);
  }
  log_line("settings: %u scene(s)%s%s", cfg.scene_count, sl::follows_presence(cfg) ? ", following " : "",
           sl::follows_presence(cfg) ? t_presence : "");
}

// --- light -----------------------------------------------------------------------------------

// Puts every light output in PWM mode, switched off. Brightness comes with each command.
void configure_outputs() {
  for (int p = 0; p < sl::kIoPins; p++) {
    written[p] = -1;
    if (!is_light_output(p)) continue;
    kOutput[p]->Mode(Connector::OUTPUT_PWM);
    kOutput[p]->PwmDuty(0);
    written[p] = 0;
  }
}

void setup_pins() {
  configure_outputs();
  if (cfg.button != sl::kNoPin) pinMode(kArduinoPin[cfg.button], INPUT);
}

void strip_clock(const sl::StripTiming& t);

// The library turns on the SERCOM's hardware slave select (CTRLB.MSSEN), which spaces characters apart. The
// strip has no SS and needs bytes back to back, so switch it off. CTRLB can only be written while disabled.
void strip_no_hw_ss() {
  Sercom* sercom = cfg.strip_port == 0 ? SERCOM7 : SERCOM0;
  sercom->SPI.CTRLA.bit.ENABLE = 0;
  while (sercom->SPI.SYNCBUSY.bit.ENABLE) continue;
  sercom->SPI.CTRLB.bit.MSSEN = 0;
  while (sercom->SPI.SYNCBUSY.bit.CTRLB) continue;
  sercom->SPI.CTRLA.bit.ENABLE = 1;
  while (sercom->SPI.SYNCBUSY.bit.ENABLE) continue;
}

void setup_strip() {
  char err[96];
  if (!sl::strip_ready(cfg, err, sizeof err)) {
    if (err[0]) {
      log_line("settings: %s; strip disabled", err);
      settings_errors++;
    }
    return;
  }
  strip_com = cfg.strip_port == 0 ? &ConnectorCOM0 : &ConnectorCOM1;
  strip_dma = cfg.strip_port == 0 ? DMA_SERCOM7_SPI_RX : DMA_SERCOM0_SPI_RX;  // COM-0 = SERCOM7, COM-1 = SERCOM0
  strip_com->Mode(Connector::SPI);
  strip_com->Speed(sl::strip_timing(cfg.strip_timing).spi_hz);
  strip_com->DataOrder(SerialDriver::COM_MSB_FIRST);
  strip_com->SpiClock(SerialDriver::SCK_LOW, SerialDriver::LEAD_CHANGE);
  strip_com->PortOpen();
  strip_com->SpiSsMode(SerialDriver::LINE_OFF);
  if (sl::strip_timing(cfg.strip_timing).fast_clock) strip_clock(sl::strip_timing(cfg.strip_timing));
  strip_no_hw_ss();
  size_t reset = sl::strip_reset_bytes(sl::strip_timing(cfg.strip_timing));
  sl::strip_effects_reset(&strip_fx, micros());
  char why[96];
  if (!sl::layout_parse(cfg.strip_layout, cfg.strip_pixels, &strip_map, why, sizeof why)) {
    log_line("settings: %s; using a straight line", why);
    settings_errors++;
    sl::layout_parse("line", cfg.strip_pixels, &strip_map, why, sizeof why);
  }
  memset(strip_buf, 0, reset);
  strip_com->SpiTransferData(strip_buf, nullptr, (int32_t)reset);  // start from a reset (line low)
}

bool strip_busy() { return DmaManager::Channel(strip_dma)->CHCTRLA.bit.ENABLE != 0; }

// Clocks the strip port's SPI for a timing. The library clocks COM-port SPI from GCLK7 (10 MHz, so SPI at
// 10 / (2 (BAUD + 1)) MHz); fast_clock timings switch the port's SERCOM to GCLK4 (48 MHz, the USB clock) and
// set BAUD directly. The library only re-selects GCLK7 when the port's mode changes.
void strip_clock(const sl::StripTiming& t) {
  Sercom* sercom = cfg.strip_port == 0 ? SERCOM7 : SERCOM0;
  const uint32_t gclk_id = cfg.strip_port == 0 ? SERCOM7_GCLK_ID_CORE : SERCOM0_GCLK_ID_CORE;
  strip_com->SpiAsyncWaitComplete();
  sercom->SPI.CTRLA.bit.ENABLE = 0;
  while (sercom->SPI.SYNCBUSY.bit.ENABLE) continue;
  GCLK->PCHCTRL[gclk_id].bit.CHEN = 0;
  while (GCLK->PCHCTRL[gclk_id].bit.CHEN) continue;
  GCLK->PCHCTRL[gclk_id].reg = GCLK_PCHCTRL_GEN(t.fast_clock ? 4 : 7) | GCLK_PCHCTRL_CHEN;
  while (!GCLK->PCHCTRL[gclk_id].bit.CHEN) continue;
  if (t.fast_clock) {
    sercom->SPI.BAUD.reg = (uint8_t)(48000000UL / (2 * t.spi_hz) - 1);
    sercom->SPI.CTRLA.bit.ENABLE = 1;
    while (sercom->SPI.SYNCBUSY.bit.ENABLE) continue;
  } else {
    sercom->SPI.CTRLA.bit.ENABLE = 1;
    while (sercom->SPI.SYNCBUSY.bit.ENABLE) continue;
    strip_com->Speed(t.spi_hz);  // the library's own calculation for the 10 MHz clock
  }
  strip_no_hw_ss();
}

// Renders every pixel for this moment and sends the frame when anything changed, and once a second regardless.
void strip_tick() {
  if (!strip_com) return;
  if (strip_raw_len) {  // a test pattern from "strip_raw" is showing
    if (strip_raw_sent && millis() - strip_last_send < kStripRefreshMs) return;
    if (strip_busy()) return;
    const sl::StripTiming& t = sl::strip_timing(cfg.strip_timing);
    size_t len = sl::strip_encode_raw(strip_raw_bytes, strip_raw_len, cfg.strip_pixels, t, strip_buf, sizeof strip_buf);
    if (len) strip_send(len);
    strip_raw_sent = true;
    return;
  }
  // Render only when a frame can go out, so each render is one frame: an effect's single-frame flash (SPARKLE)
  // is never drawn and then dropped.
  if (strip_busy()) return;
  const uint32_t now = millis();
  const int n = cfg.strip_pixels;
  const size_t bytes = (size_t)n * sl::kStripChannels;
  if (strip_index_on) {
    for (int i = 0; i < n; i++) {
      const uint8_t* c = kIndexColors[(i / 5) % 10];
      static const uint16_t kStep[5] = {255, 81, 26, 8, 3};  // 100, 32, 10, 3, 1 %: even steps to the eye
      const uint16_t k = kStep[i % 5];
      uint8_t* o = strip_pix + i * sl::kStripChannels;
      for (int j = 0; j < 3; j++) o[j] = (uint8_t)(c[cfg.strip_order[j]] * k / 255);
      o[3] = (uint8_t)(c[3] * k / 255);
      o[4] = (uint8_t)(c[4] * k / 255);
    }
  } else {
    sl::strip_render(light, cfg.timing, now, strip_map, cfg.strip_order, &strip_fx, n, strip_pix);
  }
  bool changed = !strip_sent_once || memcmp(strip_pix, strip_last, bytes) != 0;
  if (!changed && now - strip_last_send < kStripRefreshMs) return;
  const sl::StripTiming& t = sl::strip_timing(cfg.strip_timing);
  sl::strip_encode(strip_pix, n, t, strip_buf);
  strip_send(sl::strip_frame_bytes(n, t));
  memcpy(strip_last, strip_pix, bytes);
  strip_sent_once = true;
}

void update_light() {
  strip_tick();
  uint8_t duty[sl::kIoPins];
  sl::io_duty(light, cfg.timing, millis(), cfg.pin, duty);
  for (int p = 0; p < sl::kIoPins; p++) {
    if (written[p] < 0 || duty[p] == written[p]) continue;  // only touch the hardware on a change
    written[p] = duty[p];
    kOutput[p]->PwmDuty(duty[p]);
  }
}

void publish_event(const char* text) {
  if (mqtt.connected()) mqtt.publish(t_event, text, false);
}

// Applies a command from MQTT or the console. Returns false if it was rejected.
bool apply_command(const char* text, size_t len, const char* source) {
  static sl::State next;  // large: not on the stack
  static char desc[480], msg[520];
  char err[96];
  bool ok = sl::parse_command(text, len, &next, err, sizeof err);
  for (int p = 0; ok && p < sl::kIoPins; p++) {
    if (next.io_set[p] && !is_light_output(p)) {
      snprintf(err, sizeof err, "IO%d is not a light output (name it in %s: red=, green=, blue= or alarm=)", p,
               kSettingsFile);
      ok = false;
    }
  }
  if (!ok) {
    log_line("%s: rejected (%s)", source, err);
    snprintf(msg, sizeof msg, "rejected %s", err);
    publish_event(msg);
    return false;
  }
  light = next;
  strip_raw_len = 0;  // a new light state ends a strip_raw test pattern (strip_index stays until "strip_index off")
  sl::strip_effects_reset(&strip_fx, micros());  // effects start fresh
  sl::describe(light, desc, sizeof desc);
  log_line("%s: %s", source, desc);
  snprintf(msg, sizeof msg, "applied %s", desc);
  publish_event(msg);
  return true;
}

// --- network and MQTT ------------------------------------------------------------------------

void start_network() {
  uint8_t unused_mac[6] = {0};  // the ClearCore always uses its own MAC address
  if (cfg.dhcp) {
    net_up = Ethernet.begin(unused_mac) != 0;
  } else {
    Ethernet.begin(unused_mac, IPAddress(cfg.ip), IPAddress(cfg.dns), IPAddress(cfg.gateway), IPAddress(cfg.subnet));
    net_up = true;
  }
  if (net_up) {
    IPAddress ip = Ethernet.localIP();
    log_line("network: up, %u.%u.%u.%u (%s)", ip[0], ip[1], ip[2], ip[3], cfg.dhcp ? "DHCP" : "static");
  } else {
    log_line("network: no DHCP lease (cable connected?); retrying in %lu s", (unsigned long)(kNetRetryMs / 1000));
  }
}

bool update_settings_file(const char* const keys[], const char* const values[], size_t n, const char* who);

// Shows a scene: its scene_<name> command, or a Teams state's built-in look. Returns false if there is none.
bool play_scene(const char* name, const char* source) {
  const char* command = sl::scene_command(cfg, name);
  char label[48];
  snprintf(label, sizeof label, "%s %s", source, name);
  if (!command) {
    log_line("%s: no such scene", label);
    char msg[64];
    snprintf(msg, sizeof msg, "rejected unknown scene %s", name);
    publish_event(msg);
    return false;
  }
  if (!apply_command(command, strlen(command), label)) return false;
  snprintf(showing_scene, sizeof showing_scene, "%s", name);
  return true;
}

// The state name in a presence or scene payload: trimmed and lower-cased, or empty if it isn't a scene name.
void scene_name_of(const char* text, char* out, size_t cap) {
  while (*text == ' ') text++;
  size_t n = 0;
  while (text[n] && text[n] != ' ' && text[n] != '\r' && text[n] != '\n' && n < cap - 1) {
    out[n] = (char)tolower((unsigned char)text[n]);
    n++;
  }
  out[n] = '\0';
  if (!sl::scene_name_ok(out)) out[0] = '\0';
}

// "scene_<name>=<command>" saves a scene (and uses it now), "scene_<name>=" removes it. Only scene keys.
void config_message(const char* text) {
  static char line[400];
  sl::Settings& check = check_cfg;
  char msg[160], err[96];
  snprintf(line, sizeof line, "%s", text);
  for (char* e = line + strlen(line); e > line && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n');) *--e = '\0';
  char* eq = strchr(line, '=');
  char key[40];
  size_t kl = eq ? (size_t)(eq - line) : 0;
  if (!eq || kl >= sizeof key || strncmp(line, "scene_", 6) != 0) {
    log_line("config: rejected (only scene_<name>=<command>)");
    publish_event("config rejected: only scene_<name>=<command> can be set");
    return;
  }
  memcpy(key, line, kl);
  key[kl] = '\0';
  const char* value = eq + 1;
  const char* name = key + 6;
  if (!sl::scene_name_ok(name)) {
    publish_event("config rejected: scene names are 1..23 of a..z, 0..9, _ and -");
    return;
  }
  const char* const keys[1] = {key};
  if (!*value) {
    const char* const vals[1] = {nullptr};
    if (!update_settings_file(keys, vals, 1, "config")) return publish_event("config failed: settings.ini not changed");
    sl::remove_scene(&cfg, name);
    log_line("config: %s removed", key);
    snprintf(msg, sizeof msg, "config removed %s", key);
    publish_event(msg);
  } else {
    check = cfg;
    if (!sl::apply_line(&check, line, err, sizeof err)) {
      log_line("config: rejected (%s)", err);
      snprintf(msg, sizeof msg, "config rejected %s", err);
      return publish_event(msg);
    }
    const char* const vals[1] = {value};
    if (!update_settings_file(keys, vals, 1, "config")) return publish_event("config failed: settings.ini not changed");
    sl::apply_line(&cfg, line, err, sizeof err);
    log_line("config: %s saved", key);
    snprintf(msg, sizeof msg, "config saved %s", key);
    publish_event(msg);
  }
  if (!strcmp(showing_scene, name)) play_scene(name, "config");  // the scene on the light changed: show the new one
}

void on_message(char* topic, byte* payload, unsigned int length) {
  static char text[1024];
  if (length >= sizeof text) {
    log_line("mqtt: message on %s too long (%u bytes); ignored", topic, length);
    return;
  }
  memcpy(text, payload, length);
  text[length] = '\0';

  // Retained messages arrive right after subscribing: ones from before this connection.
  const bool just_subscribed = millis() - subscribed_at < kStaleResetWindowMs;
  if (strcmp(topic, t_command) == 0) {
    // A light that follows presence takes its state from the presence topic, not an old retained command.
    if (sl::follows_presence(cfg) && just_subscribed) {
      log_line("mqtt: ignored the retained command (following %s)", t_presence);
      return;
    }
    if (apply_command(text, length, "mqtt")) showing_scene[0] = '\0';
  } else if (strcmp(topic, t_scene) == 0) {
    char name[sl::kSceneNameMax + 1];
    scene_name_of(text, name, sizeof name);
    if (!just_subscribed) play_scene(name[0] ? name : "?", "scene");
  } else if (sl::follows_presence(cfg) && strcmp(topic, t_presence) == 0) {
    scene_name_of(text, presence_state, sizeof presence_state);
    if (!presence_state[0]) {
      log_line("presence: ignored '%s'", text);
      return;
    }
    if (!publisher_offline) play_scene(presence_state, "presence");
  } else if (sl::follows_presence(cfg) && strcmp(topic, t_publisher) == 0) {
    const bool offline = strcmp(text, "offline") == 0;
    if (offline == publisher_offline) return;
    publisher_offline = offline;
    if (offline) play_scene("stale", "presence");
    else if (presence_state[0]) play_scene(presence_state, "presence");
  } else if (cfg.allow_config && strcmp(topic, t_config) == 0) {
    if (length == 0) return;
    if (just_subscribed) {
      log_line("config: ignored a retained config message");
      return;
    }
    config_message(text);
  } else if (cfg.allow_reset && strcmp(topic, t_reset) == 0) {
    if (length == 0) return;  // our own clearing of a retained reset
    // A reset message delivered right after subscribing is a retained one from the past: clear it.
    bool stale = millis() - subscribed_at < kStaleResetWindowMs;
    mqtt.publish(t_reset, "", true);
    if (stale || strcmp(text, "RESET") != 0) {
      log_line("mqtt: ignored %s reset message '%s'", stale ? "retained" : "unknown", text);
      return;
    }
    log_line("mqtt: RESET received; rebooting");
    publish_event("resetting");
    mqtt.publish(t_status, "offline", true);
    mqtt.disconnect();
    delay(200);
    SysMgr.ResetBoard();
  }
}

void try_mqtt() {
  char id[132];  // four 31-character fields, three dots and the terminator
  if (cfg.asset[0]) snprintf(id, sizeof id, "%s", cfg.asset);
  else snprintf(id, sizeof id, "%s.%s.%s.%s", cfg.project, cfg.client, cfg.site, cfg.role);
  const char* user = cfg.user[0] ? cfg.user : nullptr;
  const char* pass = cfg.password[0] ? cfg.password : nullptr;
  if (mqtt.connect(id, user, pass, t_status, 1, true, "offline")) {
    mqtt.publish(t_status, "online", true);
    mqtt.subscribe(t_command, 1);
    mqtt.subscribe(t_scene, 1);
    if (cfg.allow_reset) mqtt.subscribe(t_reset, 1);
    if (cfg.allow_config) mqtt.subscribe(t_config, 1);
    if (sl::follows_presence(cfg)) {
      mqtt.subscribe(t_presence, 1);
      mqtt.subscribe(t_publisher, 1);
    }
    subscribed_at = millis();
    mqtt_backoff = kMqttBackoffMinMs;
    log_line("mqtt: connected to %s:%u as %s; listening on %s", cfg.broker, cfg.port, id, t_command);
  } else {
    log_line("mqtt: connect to %s:%u failed (state %d); retrying in %lu s", cfg.broker, cfg.port, mqtt.state(),
             (unsigned long)(mqtt_backoff / 1000));
    next_mqtt_try = millis() + mqtt_backoff;
    mqtt_backoff = min(mqtt_backoff * 2, kMqttBackoffMaxMs);
  }
}

bool link_up() { return EthernetMgr.PhyLinkActive(); }

void network_tick() {
  static bool reported_link = true;
  if (!mqtt_enabled) return;
  Ethernet.maintain();
  // DHCP and MQTT connects block while they time out, so only try them with an Ethernet link.
  if (!link_up()) {
    if (reported_link) log_line("network: no Ethernet link; waiting for a cable");
    reported_link = false;
    return;
  }
  if (!reported_link) log_line("network: Ethernet link up");
  reported_link = true;
  if (!net_up) {
    if ((int32_t)(millis() - next_net_try) >= 0) {
      start_network();
      next_net_try = millis() + kNetRetryMs;
    }
    return;
  }
  if (mqtt.connected()) {
    mqtt.loop();
  } else if ((int32_t)(millis() - next_mqtt_try) >= 0) {
    try_mqtt();
  }
}

// --- button ----------------------------------------------------------------------------------

void button_tick() {
  static bool stable = false, last_read = false;
  static uint32_t changed_at = 0;
  if (cfg.button == sl::kNoPin) return;
  bool now = digitalRead(kArduinoPin[cfg.button]) == HIGH;
  if (now != last_read) {
    last_read = now;
    changed_at = millis();
  }
  if (now != stable && millis() - changed_at >= kDebounceMs) {
    stable = now;
    if (stable) {
      log_line("button: pressed");
      publish_event("button");
    }
  }
}

// --- USB console -----------------------------------------------------------------------------

void print_status() {
  static char desc[480];
  sl::describe(light, desc, sizeof desc);
  log_line("ClearCoreStackLight %s", kVersion);
  log_line("settings: %s, %d error(s)", settings_loaded ? "loaded" : "not loaded", settings_errors);
  log_line("asset %s, role %s/%s/%s/%s", cfg.asset[0] ? cfg.asset : "(none)", cfg.project, cfg.client, cfg.site,
           cfg.role);
  log_line("pins: red %s, green %s, blue %s, alarm %s, button %s", sl::pin_name(cfg.pin[sl::kRed]),
           sl::pin_name(cfg.pin[sl::kGreen]), sl::pin_name(cfg.pin[sl::kBlue]), sl::pin_name(cfg.pin[sl::kAlarm]),
           sl::pin_name(cfg.button));
  if (strip_com) {
    const char rgb[] = "RGB";
    log_line("strip: COM%d, %u WS2805 pixels, layout %s (%d positions), order %c%c%c, timing %u, pixel 0 %02x %02x %02x, "
             "%lu frames sent",
             cfg.strip_port, cfg.strip_pixels, cfg.strip_layout, strip_map.positions, rgb[cfg.strip_order[0]], rgb[cfg.strip_order[1]],
             rgb[cfg.strip_order[2]], cfg.strip_timing, strip_last[0], strip_last[1], strip_last[2],
             (unsigned long)strip_frames);
  } else {
    log_line("strip: none");
  }
  if (mqtt_enabled) {
    IPAddress ip = Ethernet.localIP();
    log_line("network: link %s, %s %u.%u.%u.%u", Ethernet.linkStatus() == LinkON ? "up" : "down",
             net_up ? "ip" : "no ip yet", ip[0], ip[1], ip[2], ip[3]);
    log_line("mqtt: %s %s:%u, user %s, reset %s", mqtt.connected() ? "connected to" : "not connected to", cfg.broker,
             cfg.port, cfg.user[0] ? "set" : "none", cfg.allow_reset ? "allowed" : "disabled");
    log_line("topic: %s", t_command);
    if (sl::follows_presence(cfg)) {
      log_line("presence: following %s, state %s, publisher %s", t_presence, presence_state[0] ? presence_state : "(none yet)",
               publisher_offline ? "offline" : "online");
    }
    log_line("config: %s", cfg.allow_config ? t_config : "disabled");
  } else {
    log_line("mqtt: disabled");
  }
  log_line("scenes: %u on the SD card%s%s", cfg.scene_count, showing_scene[0] ? ", showing " : "", showing_scene);
  log_line("light: %s", desc);
}

void print_strip_timing(int index, const char* prefix) {
  int ns[4];
  sl::strip_pulses(sl::strip_timing(index), ns);
  log_line("%s%d: '0' %d ns high / %d ns low, '1' %d / %d ns, bit %d ns", prefix, index, ns[0], ns[1], ns[2], ns[3],
           ns[0] + ns[1]);
}

// "strip_timing" lists the signal timings; "strip_timing <n>" switches to one now (until reboot; "set
// strip_timing <n>" keeps it) and resends the current colour.
void strip_timing_command(const char* args) {
  int n;
  if (sscanf(args, " %d", &n) != 1) {
    for (int i = 0; i < sl::kStripTimings; i++) print_strip_timing(i, i == cfg.strip_timing ? "* timing " : "  timing ");
    return;
  }
  if (n < 0 || n >= sl::kStripTimings) {
    log_line("strip_timing: use 0..%d", sl::kStripTimings - 1);
    return;
  }
  if (!strip_com) {
    log_line("strip_timing: no strip configured");
    return;
  }
  cfg.strip_timing = (uint8_t)n;
  strip_clock(sl::strip_timing(n));  // waits for the frame on the wire to finish first
  strip_sent_once = false;  // resend now
  print_strip_timing(n, "strip_timing: now timing ");
}

// "strip_raw <hex>" sends the same raw bytes (1..8, e.g. 0000000000 or 00:00:00:ff:ff) to every pixel until the
// light changes; "strip_raw" alone goes back to the light's colour. For finding out how a strip reads its data.
void strip_raw_command(const char* args) {
  if (!strip_com) {
    log_line("strip_raw: no strip configured");
    return;
  }
  uint8_t bytes[8];
  int n = 0, nibbles = 0, acc = 0;
  for (const char* p = args; *p; p++) {
    int v = isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) >= 'a' && tolower((unsigned char)*p) <= 'f') ? tolower((unsigned char)*p) - 'a' + 10 : -1;
    if (v < 0) {
      if (*p == ' ' || *p == ':' || *p == ',') continue;
      log_line("strip_raw: use hex bytes, e.g. 'strip_raw 0000000000' (R G B W1 W2)");
      return;
    }
    acc = acc * 16 + v;
    if (++nibbles == 2) {
      if (n == 8) {
        log_line("strip_raw: at most 8 bytes per pixel");
        return;
      }
      bytes[n++] = (uint8_t)acc;
      nibbles = acc = 0;
    }
  }
  if (nibbles) {
    log_line("strip_raw: odd number of hex digits");
    return;
  }
  if (n == 0) {
    strip_raw_len = 0;
    strip_sent_once = false;  // resend the light's colour now
    log_line("strip_raw: off, showing the light's colour");
    return;
  }
  memcpy(strip_raw_bytes, bytes, n);
  strip_raw_len = n;
  strip_raw_sent = false;
  char hex[3 * 8 + 1];
  for (int i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02x ", bytes[i]);
  log_line("strip_raw: every pixel gets %d byte(s): %s(until the light changes)", n, hex);
}

// Runs of equal bits in a buffer, MSB first: fills up to max (level, length) pairs, returns how many.
int bit_runs(const uint8_t* buf, size_t len, uint8_t* level, uint16_t* run, int max) {
  int n = 0;
  int cur = -1;
  for (size_t i = 0; i < len * 8; i++) {
    int b = (buf[i / 8] >> (7 - i % 8)) & 1;
    if (b == cur && run[n - 1] < 65535) {
      run[n - 1]++;
    } else {
      if (n == max) break;
      cur = b;
      level[n] = (uint8_t)b;
      run[n++] = 1;
    }
  }
  return n;
}

// "strip_loopback": with a wire from the COM port's pin 5 (data out) to pin 3 (data in), sends a short test
// frame and reads the line back at the same time, then compares bit by bit and pulse by pulse. Shows what the
// line really carries at the connector, strip load included.
void strip_loopback_command() {
  static uint8_t rx[1024];
  const sl::StripTiming& t = sl::strip_timing(cfg.strip_timing);
  const uint8_t pattern[5] = {0x00, 0xFF, 0xA5, 0x5A, 0x0F};
  size_t len = sl::strip_encode_raw(pattern, 5, 4, t, strip_buf, sizeof rx);  // 4 pixels
  if (!len) {
    log_line("strip_loopback: test frame too big");
    return;
  }
  strip_com->SpiAsyncWaitComplete();
  memset(rx, 0x55, len);
  strip_com->SpiTransferData(strip_buf, rx, (int32_t)len);
  strip_sent_once = false;  // the strip got the test frame; resend the light's colour next loop
  size_t diff = 0, ones_tx = 0, ones_rx = 0, first = len;
  for (size_t i = 0; i < len; i++) {
    uint8_t x = strip_buf[i] ^ rx[i];
    if (x && first == len) first = i;
    diff += (size_t)__builtin_popcount(x);
    ones_tx += (size_t)__builtin_popcount(strip_buf[i]);
    ones_rx += (size_t)__builtin_popcount(rx[i]);
  }
  int ns_per_bit = (int)(1000000000UL / t.spi_hz);
  log_line("strip_loopback: timing %u, %u bytes, %u of %u bits differ; ones sent %u, read %u", cfg.strip_timing,
           (unsigned)len, (unsigned)diff, (unsigned)(len * 8), (unsigned)ones_tx, (unsigned)ones_rx);
  if (first < len) {
    char a[3 * 12 + 1], b[3 * 12 + 1];
    size_t from = first > 2 ? first - 2 : 0;
    for (size_t k = 0; k < 12 && from + k < len; k++) {
      snprintf(a + 3 * k, 4, "%02x ", strip_buf[from + k]);
      snprintf(b + 3 * k, 4, "%02x ", rx[from + k]);
    }
    log_line("  first difference at byte %u, from byte %u: sent %s", (unsigned)first, (unsigned)from, a);
    log_line("  %*sread %s", (int)strlen("first difference at byte , from byte : ") + (first > 9 ? 2 : 1) +
                                  (from > 9 ? 2 : 1) - 4, "", b);
  }
  // The first pulses, in SPI bits (x ns): sent vs read.
  uint8_t lv_tx[16], lv_rx[16];
  uint16_t rn_tx[16], rn_rx[16];
  int n_tx = bit_runs(strip_buf, len, lv_tx, rn_tx, 16), n_rx = bit_runs(rx, len, lv_rx, rn_rx, 16);
  char line_tx[120] = "", line_rx[120] = "";
  for (int i = 0; i < n_tx; i++)
    snprintf(line_tx + strlen(line_tx), sizeof line_tx - strlen(line_tx), "%c%u ", lv_tx[i] ? 'H' : 'L', rn_tx[i]);
  for (int i = 0; i < n_rx; i++)
    snprintf(line_rx + strlen(line_rx), sizeof line_rx - strlen(line_rx), "%c%u ", lv_rx[i] ? 'H' : 'L', rn_rx[i]);
  log_line("  pulses sent (x %d ns): %s", ns_per_bit, line_tx);
  log_line("  pulses read (x %d ns): %s", ns_per_bit, line_rx);

  // Gaps between bytes don't show above (the read is clocked by the same SPI clock), so time a real DMA frame
  // with the CPU cycle counter: any pause between bytes makes it longer than bytes x 8 / SPI clock.
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  size_t frame = sl::strip_encode_raw(pattern, 5, cfg.strip_pixels, t, strip_buf, sizeof strip_buf);
  uint32_t ideal_ns = (uint32_t)((uint64_t)frame * 8 * 1000000000ULL / t.spi_hz);
  uint32_t start = DWT->CYCCNT;
  if (!strip_com->SpiTransferDataAsync(strip_buf, nullptr, (int32_t)frame)) {
    log_line("  dma: not available on this port");
    return;
  }
  while (strip_busy()) continue;
  uint32_t took_ns = (uint32_t)((uint64_t)(DWT->CYCCNT - start) * 1000000000ULL / SystemCoreClock);
  log_line("  dma: %u-byte frame took %lu us, ideal %lu us (%+ld ns per byte)", (unsigned)frame,
           (unsigned long)(took_ns / 1000), (unsigned long)(ideal_ns / 1000),
           (long)(((int64_t)took_ns - ideal_ns) / (int64_t)frame));
}

void strip_index_command(const char* args) {
  while (*args == ' ') args++;
  if (!strcmp(args, "off")) {
    strip_index_on = false;
    strip_sent_once = false;
    log_line("strip_index: off, showing the light again");
    return;
  }
  strip_raw_len = 0;
  strip_index_on = true;
  strip_sent_once = false;
  log_line("strip_index: physical pixels in wiring order (layout ignored), blocks of 5 fading 100/32/10/3/1 %%:");
  for (int b = 0; b * 5 < cfg.strip_pixels; b++) {
    int last = b * 5 + 4 < cfg.strip_pixels - 1 ? b * 5 + 4 : cfg.strip_pixels - 1;
    log_line("  pixels %d-%d %s", b * 5, last, kIndexNames[b % 10]);
  }
}

// Writes data to a file on the SD card, replacing it, then reads it back to check.
bool write_file(const char* name, const char* data, size_t len) {
  File f = SD.open(name, O_WRITE | O_CREAT | O_TRUNC);
  if (!f) return false;
  size_t written = f.write((const uint8_t*)data, len);
  f.close();
  if (written != len) return false;
  File r = SD.open(name);
  if (!r) return false;
  size_t i = 0;
  bool same = true;
  while (same && r.available() > 0) {
    int c = r.read();
    same = i < len && c == (uint8_t)data[i];
    i++;
  }
  r.close();
  return same && i == len;
}

// Sets keys in settings.ini, keeping every other line. The old file is kept as settings.bak first, so a failed
// write never loses the settings. `who` prefixes the messages. Returns true on success.
bool update_settings_file(const char* const keys[], const char* const values[], size_t n, const char* who) {
  static char before[8192], after[8704];
  if (!settings_loaded) {
    log_line("%s: no %s was loaded from the SD card; nothing to update", who, kSettingsFile);
    return false;
  }
  File f = SD.open(kSettingsFile);
  if (!f) {
    log_line("%s: can't open %s", who, kSettingsFile);
    return false;
  }
  size_t len = 0;
  while (f.available() > 0 && len < sizeof before) before[len++] = (char)f.read();
  bool too_big = f.available() > 0;
  f.close();
  if (too_big) {
    log_line("%s: %s is over %u bytes; not changed", who, kSettingsFile, (unsigned)sizeof before);
    return false;
  }
  long out = sl::set_keys(before, len, keys, values, n, after, sizeof after);
  if (out < 0) {
    log_line("%s: the updated %s would be too big; not changed", who, kSettingsFile);
    return false;
  }
  if (!write_file(kBackupFile, before, len)) {
    log_line("%s: couldn't write %s; %s not changed", who, kBackupFile, kSettingsFile);
    return false;
  }
  if (!write_file(kSettingsFile, after, (size_t)out)) {
    log_line("%s: writing %s failed; the previous version is in %s", who, kSettingsFile, kBackupFile);
    return false;
  }
  return true;
}

// "unset <key>": removes a key's lines from settings.ini (e.g. old ones), so its default applies after a reboot.
void unset_command(const char* args) {
  while (*args == ' ') args++;
  char key[32];
  size_t k = 0;
  while (args[k] && args[k] != ' ' && k < sizeof key - 1) {
    key[k] = args[k];
    k++;
  }
  key[k] = '\0';
  if (!key[0]) {
    log_line("unset: use 'unset <key>', e.g. 'unset green_level'");
    return;
  }
  const char* const keys[1] = {key};
  const char* const vals[1] = {nullptr};
  if (!update_settings_file(keys, vals, 1, "unset")) return;
  if (!strncmp(key, "scene_", 6) && sl::remove_scene(&cfg, key + 6)) {
    log_line("unset: %s removed from %s (previous copy: %s) and no longer in use", key, kSettingsFile, kBackupFile);
    return;
  }
  log_line("unset: %s removed from %s (previous copy: %s); type 'reboot' to apply", key, kSettingsFile, kBackupFile);
}

// "set <key> <value>": checks one setting the way settings.ini is read, then writes it into settings.ini.
// It takes effect after a reboot.
void set_command(const char* args) {
  while (*args == ' ') args++;
  char key[32];
  size_t k = 0;
  while (args[k] && args[k] != ' ' && args[k] != '=' && k < sizeof key - 1) {
    key[k] = args[k];
    k++;
  }
  key[k] = '\0';
  const char* value = args + k;
  while (*value == ' ' || *value == '=') value++;
  if (!key[0] || !*value) {
    log_line("set: use 'set <key> <value>', e.g. 'set strip COM0'; see settings.example.ini for the keys");
    return;
  }
  static char line[400];
  sl::Settings& check = check_cfg;
  char err[96];
  snprintf(line, sizeof line, "%s=%s", key, value);
  check = cfg;
  if (!sl::apply_line(&check, line, err, sizeof err)) {
    log_line("set: %s; not saved", err);
    return;
  }
  const char* const keys[1] = {key};
  const char* const vals[1] = {value};
  if (!update_settings_file(keys, vals, 1, "set")) return;
  if (!strncmp(key, "scene_", 6)) {  // scenes are used at once; everything else after a reboot
    sl::apply_line(&cfg, line, err, sizeof err);
    log_line("set: %s saved to %s (previous copy: %s) and in use", key, kSettingsFile, kBackupFile);
    if (!strcmp(showing_scene, key + 6)) play_scene(key + 6, "set");
    return;
  }
  log_line("set: %s saved to %s (previous copy: %s); type 'reboot' to apply", key, kSettingsFile, kBackupFile);
}

void console_tick() {
  static char line[640];
  static size_t len = 0;
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (len < sizeof line - 1) line[len++] = (char)c;
      continue;
    }
    line[len] = '\0';
    size_t n = len;
    len = 0;
    if (n == 0) continue;
    if (!strcmp(line, "status") || !strcmp(line, "?")) {
      print_status();
    } else if (!strcmp(line, "scenes")) {
      for (int i = 0; i < cfg.scene_count; i++) log_line("scene %s: %s", cfg.scenes[i].name, cfg.scenes[i].command);
      log_line("scenes: %u on the SD card; the Teams states without one use their built-in looks", cfg.scene_count);
    } else if (!strncmp(line, "scene ", 6)) {
      char name[sl::kSceneNameMax + 1];
      scene_name_of(line + 6, name, sizeof name);
      play_scene(name[0] ? name : "?", "console scene");
    } else if (!strncmp(line, "unset ", 6)) {
      unset_command(line + 6);
    } else if (!strncmp(line, "set ", 4)) {
      set_command(line + 4);
    } else if (!strcmp(line, "strip_loopback") && strip_com) {
      strip_loopback_command();
    } else if (!strncmp(line, "strip_index", 11) && (line[11] == '\0' || line[11] == ' ') && strip_com) {
      strip_index_command(line + 11);
    } else if (!strncmp(line, "strip_raw", 9) && (line[9] == '\0' || line[9] == ' ')) {
      strip_raw_command(line + 9);
    } else if (!strncmp(line, "strip_timing", 12) && (line[12] == '\0' || line[12] == ' ')) {
      strip_timing_command(line + 12);
    } else if (!strcmp(line, "reboot")) {
      log_line("reboot: restarting");
      if (mqtt.connected()) {
        mqtt.publish(t_status, "offline", true);  // a clean disconnect cancels the Last Will, so say it
        mqtt.disconnect();
      }
      delay(100);
      SysMgr.ResetBoard();
    } else if (!strcmp(line, "help")) {
      log_line("commands: a light command (e.g. GREEN, RED,FAST, IO4=100;IO5=26, STRIP[0-9]=100,0,0, OFF), status,");
      log_line("  set <key> <value> and unset <key> (edit %s), scene <name>, scenes, reboot, help", kSettingsFile);
      log_line("  strip tests: strip_timing [n], strip_raw <hex bytes per pixel>, strip_loopback (pin 5 to 3)");
    } else {
      if (apply_command(line, n, "console")) showing_scene[0] = '\0';
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  sl::clear(&light);
  load_settings();
  setup_pins();
  setup_strip();
  if (mqtt_enabled) {
    mqtt.setServer(cfg.broker, cfg.port);
    mqtt.setCallback(on_message);
    mqtt.setBufferSize(1200);  // long STRIP commands
    EthernetMgr.Setup();  // brings up the PHY so the link can be checked; DHCP waits for a link
    next_net_try = millis();
  }
  log_line("ClearCoreStackLight %s ready; type 'help' on this port for commands", kVersion);
}

void loop() {
  console_tick();
  network_tick();
  button_tick();
  update_light();
}
