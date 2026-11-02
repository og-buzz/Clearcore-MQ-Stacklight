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
//
// The USB serial port (115200 baud) takes the same commands, plus "status" and "help".
//
// FQBN: ClearCore:sam:clearcore. Needs the PubSubClient library (arduino-cli lib install PubSubClient).

#include <ClearCore.h>
#include <Ethernet.h>
#include <PubSubClient.h>
#include <SD.h>
#include <stdarg.h>

#include "Light.h"
#include "Settings.h"

namespace {

const char* const kVersion = "2.0.0";
const char* const kSettingsFile = "settings.ini";
const uint32_t kNetRetryMs = 10000;
const uint32_t kMqttBackoffMinMs = 2000, kMqttBackoffMaxMs = 60000;
const uint32_t kStaleResetWindowMs = 3000;  // retained reset messages arrive right after subscribing
const uint32_t kDebounceMs = 50;

const int kArduinoPin[sl::kPinCount] = {IO0, IO1, IO2, IO3, IO4, IO5, DI6, DI7, DI8};

sl::Settings cfg;
sl::State light;
int settings_errors = 0;
bool settings_loaded = false;
bool mqtt_enabled = false;

char t_command[160], t_status[160], t_event[160], t_reset[160];

EthernetClient eth;
PubSubClient mqtt(eth);
bool net_up = false;
uint32_t next_net_try = 0, next_mqtt_try = 0, mqtt_backoff = kMqttBackoffMinMs, subscribed_at = 0;

void log_line(const char* fmt, ...) {
  char buf[200];
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
  char line[160];
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
}

// --- light -----------------------------------------------------------------------------------

void setup_pins() {
  for (uint8_t c = 0; c < sl::kChannels; c++) {
    if (cfg.pin[c] == sl::kNoPin) continue;
    pinMode(kArduinoPin[cfg.pin[c]], OUTPUT);
    digitalWrite(kArduinoPin[cfg.pin[c]], LOW);
  }
  if (cfg.button != sl::kNoPin) pinMode(kArduinoPin[cfg.button], INPUT);
}

void update_light() {
  bool on[sl::kChannels];
  sl::lit(light, cfg.timing, millis(), on);
  for (uint8_t c = 0; c < sl::kChannels; c++) {
    if (cfg.pin[c] != sl::kNoPin) digitalWrite(kArduinoPin[cfg.pin[c]], on[c] ? HIGH : LOW);
  }
}

void publish_event(const char* text) {
  if (mqtt.connected()) mqtt.publish(t_event, text, false);
}

// Applies a command from MQTT or the console. Returns false if it was rejected.
bool apply_command(const char* text, size_t len, const char* source) {
  char err[64], desc[64], msg[128];
  sl::State next;
  if (!sl::parse_command(text, len, &next, err, sizeof err)) {
    log_line("%s: rejected (%s)", source, err);
    snprintf(msg, sizeof msg, "rejected %s", err);
    publish_event(msg);
    return false;
  }
  light = next;
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

void on_message(char* topic, byte* payload, unsigned int length) {
  char text[256];
  if (length >= sizeof text) {
    log_line("mqtt: message on %s too long (%u bytes); ignored", topic, length);
    return;
  }
  memcpy(text, payload, length);
  text[length] = '\0';

  if (strcmp(topic, t_command) == 0) {
    apply_command(text, length, "mqtt");
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
    if (cfg.allow_reset) mqtt.subscribe(t_reset, 1);
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

void network_tick() {
  if (!mqtt_enabled) return;
  Ethernet.maintain();
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
  char desc[64];
  sl::describe(light, desc, sizeof desc);
  log_line("ClearCoreStackLight %s", kVersion);
  log_line("settings: %s, %d error(s)", settings_loaded ? "loaded" : "not loaded", settings_errors);
  log_line("asset %s, role %s/%s/%s/%s", cfg.asset[0] ? cfg.asset : "(none)", cfg.project, cfg.client, cfg.site,
           cfg.role);
  log_line("pins: red %s, green %s, blue %s, alarm %s, button %s", sl::pin_name(cfg.pin[sl::kRed]),
           sl::pin_name(cfg.pin[sl::kGreen]), sl::pin_name(cfg.pin[sl::kBlue]), sl::pin_name(cfg.pin[sl::kAlarm]),
           sl::pin_name(cfg.button));
  if (mqtt_enabled) {
    IPAddress ip = Ethernet.localIP();
    log_line("network: link %s, %s %u.%u.%u.%u", Ethernet.linkStatus() == LinkON ? "up" : "down",
             net_up ? "ip" : "no ip yet", ip[0], ip[1], ip[2], ip[3]);
    log_line("mqtt: %s %s:%u, user %s, reset %s", mqtt.connected() ? "connected to" : "not connected to", cfg.broker,
             cfg.port, cfg.user[0] ? "set" : "none", cfg.allow_reset ? "allowed" : "disabled");
    log_line("topic: %s", t_command);
  } else {
    log_line("mqtt: disabled");
  }
  log_line("light: %s", desc);
}

void console_tick() {
  static char line[160];
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
    } else if (!strcmp(line, "help")) {
      log_line("commands: a light command (e.g. GREEN, RED,FAST, YELLOW,SLOW;ALARM,FAST, FIRE, OFF), status, help");
    } else {
      apply_command(line, n, "console");
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  sl::clear(&light);
  load_settings();
  setup_pins();
  if (mqtt_enabled) {
    mqtt.setServer(cfg.broker, cfg.port);
    mqtt.setCallback(on_message);
    mqtt.setBufferSize(512);
    start_network();
    next_net_try = millis() + kNetRetryMs;
  }
  log_line("ClearCoreStackLight %s ready; type 'help' on this port for commands", kVersion);
}

void loop() {
  console_tick();
  network_tick();
  button_tick();
  update_light();
}
