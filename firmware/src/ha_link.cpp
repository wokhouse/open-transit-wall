// Optional Home Assistant link (see ha_link.h for the contract).
//
// Transport: PubSubClient over the plain WiFiClient — the pinned
// espressif32@7.1.3 platform ships arduino core 2.0.17, which has no
// built-in esp-mqtt (MQTT.h arrived in core 3.x). The broker is a LAN
// address on the network the wall already joined, so plain TCP/1883 is
// correct; no TLS. The host is found via mDNS (ha_discover.h) unless
// provisioning overrides it; credentials are used when set.
//
// Home Assistant needs zero configuration: the wall publishes retained
// MQTT-discovery configs (light, default schema, brightness 0-255; and a
// select listing the proxy's agencies — docs/agency-select.md) under
// homeassistant/{light,select}/otw-<id>/config, plus retained state and an
// LWT availability topic. <id> is the last 3 bytes of the WiFi MAC in hex,
// so every wall is unique without any extra provisioning.
//
// Commands (subscribed):
//   otw/<id>/set             "ON" / "OFF"
//   otw/<id>/set_brightness  "0".."255"
//   otw/<id>/set_agency      an agency option label (haSetAgencies roster)
// State (published, retained):
//   otw/<id>/state           "ON" / "OFF"
//   otw/<id>/brightness      "0".."255"
//   otw/<id>/agency          the current agency's option label
//   otw/<id>/available       "online" / "offline" (LWT: offline)
// Discovery:
//   homeassistant/light/otw-<id>/config
//   homeassistant/select/otw-<id>/config
//
// Brightness persistence: NVS namespace "otw" (same store the provisioner
// uses), key "bright". `erase` wipes it along with the config — that is a
// factory reset.

#include "ha_link.h"

#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "ha_discover.h"
#include "provisioner.h"
#include "vehicle_client.h"  // AgencyEntry, AGENCY_*

#define HA_BRIGHT_KEY   "bright"
#define HA_RETRY_MS     10000   // broker reconnect backoff
// mDNS discovery blocks the render loop for up to ~10 s (four 2.5 s listen
// windows), so a miss backs off far longer than a failed TCP connect: a
// wall whose broker is gone must keep drawing its map instead of stalling
// on the LAN every cycle.
#define HA_DISCOVER_RETRY_MS 60000
// PubSubClient buffer and the largest discovery document. The agency
// select is the big one: fixed ~500 B plus ~50 B per option, and the
// roster bound (AGENCY_MAX) keeps that ceiling firm.
#define HA_BUF          1536
#define HA_ID_HEX       6       // last 3 MAC bytes, lowercase hex

static Preferences haPrefs;
static WiFiClient haNet;
static PubSubClient mqtt(haNet);

static void (*applyBrightness)(uint8_t) = nullptr;
static uint8_t curBrightness = DISPLAY_BRIGHTNESS;
static bool powered = true;

// Home Assistant levels (0-255) are remapped into the provisioned
// brightness window before touching the panel: HA 0% lands on bmin, HA
// 100% on bmax. The state topics keep the raw HA level, so the slider in
// HA always shows what HA sent. Unset bounds (0/255) = identity.
static uint8_t mapBrightness(uint8_t v) {
  uint8_t lo = provBmin(), hi = provBmax();
  if (lo == 0 && hi == 255) {
    return v;
  }
  if (hi < lo) {
    hi = lo;  // defensive: a broken window collapses to the floor
  }
  return (uint8_t)(lo + (uint32_t)v * (hi - lo) / 255);
}

static char idHex[HA_ID_HEX + 1];
static char topicState[48];
static char topicBright[48];
static char topicSet[48];
static char topicSetBright[48];
static char topicAvail[48];
static char topicDisc[64];
static char topicAgency[48];
static char topicSetAgency[48];
static char topicDiscAgency[64];
static bool idsBuilt = false;

// The proxy's agency roster (main.cpp fetches it), the wall's option list
// for the select entity and its label<->id map.
static AgencyEntry agencyRoster[AGENCY_MAX];
static int agencyCount = 0;
static bool agencyRepoll = false;

static char lastBroker[128];  // broker we are (or last tried) connected to
static uint32_t nextTryMs = 0;

// When no mhost is provisioned, this holds the host found via mDNS.
static char discHost[192];
static uint16_t discPort = 1883;

static const char *brokerHost() {
  const char *m = provMqttHost();
  return m[0] ? m : discHost;
}

static uint16_t brokerPort() {
  return provMqttHost()[0] ? provMqttPort() : discPort;
}

// Build the per-wall id and all nine topics. The MAC is fixed, so this
// runs once; WiFi.mode() has already run by the time we are called.
static void buildIds() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(idHex, sizeof(idHex), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(topicState, sizeof(topicState), "otw/%s/state", idHex);
  snprintf(topicBright, sizeof(topicBright), "otw/%s/brightness", idHex);
  snprintf(topicSet, sizeof(topicSet), "otw/%s/set", idHex);
  snprintf(topicSetBright, sizeof(topicSetBright), "otw/%s/set_brightness", idHex);
  snprintf(topicAvail, sizeof(topicAvail), "otw/%s/available", idHex);
  snprintf(topicDisc, sizeof(topicDisc), "homeassistant/light/otw-%s/config", idHex);
  snprintf(topicAgency, sizeof(topicAgency), "otw/%s/agency", idHex);
  snprintf(topicSetAgency, sizeof(topicSetAgency), "otw/%s/set_agency", idHex);
  snprintf(topicDiscAgency, sizeof(topicDiscAgency), "homeassistant/select/otw-%s/config", idHex);
}

static void publishState() {
  char num[4];
  mqtt.publish(topicState, powered ? "ON" : "OFF", true);
  snprintf(num, sizeof(num), "%u", (unsigned)curBrightness);
  mqtt.publish(topicBright, num, true);
}

// One retained discovery config. HA creates a native light entity with a
// brightness slider and groups everything under one device entry. Default
// schema: plain ON/OFF and 0-255 payloads, no templates.
static void publishDiscovery() {
  char buf[HA_BUF];
  snprintf(buf, sizeof(buf),
           "{\"name\":\"Transit Wall %s\",\"unique_id\":\"otw-%s\","
           "\"schema\":\"basic\","
           "\"state_topic\":\"otw/%s/state\",\"command_topic\":\"otw/%s/set\","
           "\"brightness_state_topic\":\"otw/%s/brightness\","
           "\"brightness_command_topic\":\"otw/%s/set_brightness\","
           "\"brightness_scale\":255,"
           "\"availability_topic\":\"otw/%s/available\","
           "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
           "\"icon\":\"mdi:led-on\","
           "\"device\":{\"identifiers\":\"otw-%s\",\"name\":\"Open Transit Wall\","
           "\"model\":\"ESP32 LED matrix wall\",\"manufacturer\":\"open-transit-wall\","
           "\"sw_version\":\"otw-fw-%d\"}}",
           idHex, idHex, idHex, idHex, idHex, idHex, idHex, idHex,
           OTW_FW_VERSION);
  mqtt.publish(topicDisc, buf, true);
}

// --- agency select entity -------------------------------------------------
// Options are the proxy's /agencies roster keyed by its spoken names
// (docs/agency-select.md). A voice "change the transit wall to X" or the
// select card lands on topicSetAgency; the id is applied and persisted via
// provSetAgency.

// Copy `in` into `out` escaping for JSON: " and \ get a backslash, bytes
// below 0x20 become \u00xx. The server rejects control characters in
// labels, but a malformed feed must never publish a config HA cannot
// parse, so escape them anyway. Each escape is written whole or not at
// all, so the output is always valid JSON.
static void jsonEscape(const char *in, char *out, size_t outSize) {
  size_t o = 0;
  for (size_t i = 0; in[i]; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c < 0x20) {
      if (o + 6 >= outSize) {
        break;
      }
      o += (size_t)snprintf(out + o, outSize - o, "\\u%04x", c);
      continue;
    }
    size_t need = (c == '"' || c == '\\') ? 2 : 1;
    if (o + need >= outSize) {
      break;
    }
    if (need == 2) {
      out[o++] = '\\';
    }
    out[o++] = (char)c;
  }
  out[o] = '\0';
}

// Roster index whose label matches `want`. Exact match first: HA hands
// back the option string unchanged, so exact is unambiguous. Case-
// insensitive only as a fallback for a voice pipeline that drifted case
// (the server rejects case-folded duplicate labels). -1 = no match.
static int agencyByLabel(const char *want) {
  for (int i = 0; i < agencyCount; i++) {
    if (strcmp(agencyRoster[i].label, want) == 0) {
      return i;
    }
  }
  for (int i = 0; i < agencyCount; i++) {
    if (strcasecmp(agencyRoster[i].label, want) == 0) {
      return i;
    }
  }
  return -1;
}

static int agencyById(const char *id) {
  for (int i = 0; i < agencyCount; i++) {
    if (strcmp(agencyRoster[i].id, id) == 0) {
      return i;
    }
  }
  return -1;
}

// Bounds-checked append for a discovery document: if any fragment fails to
// fit, the whole config is dropped rather than publishing half a JSON
// document.
static bool appendFrag(char *buf, size_t cap, size_t *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static bool appendFrag(char *buf, size_t cap, size_t *len, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= cap - *len) {
    return false;
  }
  *len += (size_t)n;
  return true;
}

// One retained discovery config for the select entity. The device block is
// identical to the light's, so HA merges both entities into the same
// device entry instead of showing a second device. Entity name "Agency
// <wall-id>" matches the light's per-wall naming: HA prefixes it with the
// device name, and two walls on one broker stay distinguishable.
static void publishAgencyDiscovery() {
  if (agencyCount == 0) {
    return;
  }
  static char buf[HA_BUF];  // not on the render loop's stack
  size_t len = 0;
  bool ok = appendFrag(buf, sizeof(buf), &len,
                       "{\"name\":\"Agency %s\","
                       "\"unique_id\":\"otw-%s-agency\",\"icon\":\"mdi:bus\","
                       "\"options\":[",
                       idHex, idHex);
  for (int i = 0; ok && i < agencyCount; i++) {
    char esc[2 * AGENCY_LABEL_MAX + 2];
    jsonEscape(agencyRoster[i].label, esc, sizeof(esc));
    ok = appendFrag(buf, sizeof(buf), &len, "%s\"%s\"", i ? "," : "", esc);
  }
  ok = ok && appendFrag(buf, sizeof(buf), &len,
                        "],\"state_topic\":\"otw/%s/agency\","
                        "\"command_topic\":\"otw/%s/set_agency\","
                        "\"availability_topic\":\"otw/%s/available\","
                        "\"payload_available\":\"online\","
                        "\"payload_not_available\":\"offline\","
                        "\"device\":{\"identifiers\":\"otw-%s\","
                        "\"name\":\"Open Transit Wall\","
                        "\"model\":\"ESP32 LED matrix wall\","
                        "\"manufacturer\":\"open-transit-wall\","
                        "\"sw_version\":\"otw-fw-%d\"}}",
                        idHex, idHex, idHex, idHex, OTW_FW_VERSION);
  if (!ok) {
    Serial.println("[ha] agency config exceeded the buffer; not published");
    return;
  }
  mqtt.publish(topicDiscAgency, buf, true);
}

// Retained selection state. HA's select rejects a state outside its
// options (it logs an error and keeps the last option), so anything the
// wall cannot name — no agency set, or a stored id the server no longer
// serves — publishes "None", the payload documented to reset the entity
// to unknown. The serial log shows why.
static void publishAgencyState() {
  if (agencyCount == 0) {
    return;
  }
  const char *cur = provAgency();
  int idx = cur[0] ? agencyById(cur) : -1;
  if (idx < 0 && cur[0]) {
    Serial.printf("[ha] stored agency \"%s\" is not in the roster; select shows unknown\n",
                  cur);
  }
  mqtt.publish(topicAgency, idx >= 0 ? agencyRoster[idx].label : "None", true);
}

static void mqttMsg(char *topic, byte *payload, unsigned int len) {
  if (len == 0) {
    return;
  }
  if (strcmp(topic, topicSet) == 0) {
    // PubSubClient does not NUL-terminate; compare against the length.
    powered = len == 2 && memcmp(payload, "ON", 2) == 0;
    applyBrightness(powered ? mapBrightness(curBrightness ? curBrightness
                                                         : DISPLAY_BRIGHTNESS)
                            : 0);
    publishState();
  } else if (strcmp(topic, topicSetBright) == 0) {
    char num[8];
    size_t n = len < sizeof(num) - 1 ? len : sizeof(num) - 1;
    memcpy(num, payload, n);
    num[n] = '\0';
    int v = atoi(num);
    if (v < 0) {
      v = 0;
    }
    if (v > 255) {
      v = 255;
    }
    curBrightness = (uint8_t)v;
    if (v > 0) {
      powered = true;  // a brightness command is also a turn-on
    }
    applyBrightness(mapBrightness((uint8_t)v));
    haPrefs.putInt(HA_BRIGHT_KEY, v);  // survive the next boot without HA
    publishState();
  } else if (strcmp(topic, topicSetAgency) == 0) {
    // Same NUL-termination rule as above.
    char want[AGENCY_LABEL_MAX + 1];
    size_t n = len < sizeof(want) - 1 ? len : sizeof(want) - 1;
    memcpy(want, payload, n);
    want[n] = '\0';
    int idx = agencyByLabel(want);
    if (idx < 0 || !provSetAgency(agencyRoster[idx].id)) {
      // A stale card after a roster change, or a bad id: republish the
      // real state so the card snaps back instead of lying.
      Serial.printf("[ha] agency command \"%s\" not in roster\n", want);
      publishAgencyState();
    } else {
      agencyRepoll = true;  // the loop polls the new agency right away
      publishAgencyState();
      Serial.printf("[ha] agency -> %s\n", agencyRoster[idx].id);
    }
  }
}

static bool connectBroker() {
  const char *user = provMqttUser();
  const char *pass = provMqttPass();
  char clientId[16];
  snprintf(clientId, sizeof(clientId), "otw-%s", idHex);
  mqtt.setServer(brokerHost(), brokerPort());
  mqtt.setKeepAlive(30);
  // LWT: HA marks the wall unavailable the moment it drops off the broker.
  if (user[0]) {
    return mqtt.connect(clientId, user, pass, topicAvail, 1, true, "offline");
  }
  return mqtt.connect(clientId, nullptr, nullptr, topicAvail, 1, true, "offline");
}

void haBegin(void (*apply)(uint8_t)) {
  applyBrightness = apply;
  haPrefs.begin("otw", false);
  int v = haPrefs.getInt(HA_BRIGHT_KEY, -1);
  if (v >= 0 && v <= 255) {
    curBrightness = (uint8_t)v;
  }
  mqtt.setBufferSize(HA_BUF);
  mqtt.setCallback(mqttMsg);
}

// Already window-mapped: main.cpp can hand this straight to the panel.
uint8_t haBrightness() { return mapBrightness(curBrightness); }
bool haConnected() { return mqtt.connected(); }
bool haEnabled() { return provMqttHost()[0] || provMqttUser()[0]; }
const char *haBroker() { return provMqttHost()[0] ? lastBroker : discHost; }

void haService() {
  // The link is on when a broker host or credentials are provisioned.
  if ((!provMqttHost()[0] && !provMqttUser()[0]) ||
      WiFi.status() != WL_CONNECTED) {
    if (mqtt.connected()) {
      mqtt.disconnect();
    }
    lastBroker[0] = '\0';
    discHost[0] = '\0';
    return;
  }
  if (!idsBuilt) {
    buildIds();
    idsBuilt = true;
  }
  if (!mqtt.connected() || strcmp(lastBroker, brokerHost()) != 0) {
    if ((int32_t)(millis() - nextTryMs) < 0) {
      return;  // backoff between attempts
    }
    nextTryMs = millis() + HA_RETRY_MS;
    if (!provMqttHost()[0] && !discHost[0]) {
      // No broker given: ask the LAN. HA advertises itself via mDNS and
      // the add-on broker shares its host; a broker advertising _mqtt._tcp
      // itself wins with its real port.
      uint16_t p = 0;
      if (!haDiscover(discHost, sizeof(discHost), &p)) {
        nextTryMs = millis() + HA_DISCOVER_RETRY_MS;  // it blocked; wait long
        Serial.println("[ha] no mqtt broker found via mDNS (set mhost= to override)");
        return;
      }
      discPort = p;
      Serial.printf("[ha] mDNS found broker %s:%u\n", discHost, p);
    }
    if (!connectBroker()) {
      // PubSubClient state(): -1 no TCP, -2 connect fail, 4/5/6 broker
      // refused (unauth / id / credentials).
      Serial.printf("[ha] broker %s:%u unreachable rc=%d\n", brokerHost(),
                    brokerPort(), mqtt.state());
      return;
    }
    snprintf(lastBroker, sizeof(lastBroker), "%s", brokerHost());
    mqtt.subscribe(topicSet);
    mqtt.subscribe(topicSetBright);
    mqtt.subscribe(topicSetAgency);
    mqtt.publish(topicAvail, "online", true);
    publishDiscovery();
    publishState();
    if (agencyCount > 0) {
      // The roster arrived while the broker was down: announce it now.
      publishAgencyDiscovery();
      publishAgencyState();
    } else {
      // No roster this run (the proxy was unreachable): drop any select
      // config retained from a previous boot. An empty retained config
      // payload removes the entity — better a missing select than one
      // whose commands all fall.
      mqtt.publish(topicDiscAgency, "", true);
    }
    Serial.printf("[ha] mqtt broker %s:%u — light entity announced%s\n",
                  brokerHost(), brokerPort(),
                  agencyCount > 0 ? " (+ agency select)" : "");
    return;
  }
  mqtt.loop();  // keepalive + command delivery
}

// Fetch the agency roster from the proxy (docs/agency-select.md) and
// announce the select entity if the broker is already up. The roster
// lives here only: main.cpp calls this and keeps no copy. Returns the
// entry count, 0 on any failure (the caller retries).
int haFetchRoster(const char *scheme, const char *host, uint16_t port) {
  if (agencyCount > 0) {
    return agencyCount;
  }
  int n = fetchAgencies(scheme, host, port, agencyRoster, AGENCY_MAX);
  if (n <= 0) {
    return 0;
  }
  agencyCount = n;
  // Announce now if the broker is up; the connect path publishes if not.
  if (mqtt.connected()) {
    publishAgencyDiscovery();
    publishAgencyState();
  }
  return agencyCount;
}

bool haTakeAgencyRepoll() {
  bool v = agencyRepoll;
  agencyRepoll = false;
  return v;
}
