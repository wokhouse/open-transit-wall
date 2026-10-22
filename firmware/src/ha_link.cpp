// Optional Home Assistant link (see ha_link.h for the contract).
//
// Transport: PubSubClient over the plain WiFiClient — the pinned
// espressif32@7.1.3 platform ships arduino core 2.0.17, which has no
// built-in esp-mqtt (MQTT.h arrived in core 3.x). The broker is a LAN
// address on the network the wall already joined, so plain TCP/1883 is
// correct; no TLS, no credentials unless the broker asks for them.
//
// Home Assistant needs zero configuration: the wall publishes one retained
// MQTT-discovery config (light, default schema, brightness 0-255) under
// homeassistant/light/otw-<id>/config, plus retained state and an LWT
// availability topic. <id> is the last 3 bytes of the WiFi MAC in hex, so
// every wall is unique without any extra provisioning.
//
// Commands (subscribed):
//   otw/<id>/set             "ON" / "OFF"
//   otw/<id>/set_brightness  "0".."255"
// State (published, retained):
//   otw/<id>/state           "ON" / "OFF"
//   otw/<id>/brightness      "0".."255"
//   otw/<id>/available       "online" / "offline" (LWT: offline)
// Discovery:
//   homeassistant/light/otw-<id>/config
//
// Brightness persistence: NVS namespace "otw" (same store the provisioner
// uses), key "bright". `erase` wipes it along with the config — that is a
// factory reset.

#include "ha_link.h"

#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "provisioner.h"

#define HA_BRIGHT_KEY   "bright"
#define HA_RETRY_MS     10000   // broker reconnect backoff
#define HA_BUF          1024    // PubSubClient buffer: discovery JSON is ~600 B
#define HA_ID_HEX       6       // last 3 MAC bytes, lowercase hex

static Preferences haPrefs;
static WiFiClient haNet;
static PubSubClient mqtt(haNet);

static void (*applyBrightness)(uint8_t) = nullptr;
static uint8_t curBrightness = DISPLAY_BRIGHTNESS;
static bool powered = true;

static char idHex[HA_ID_HEX + 1];
static char topicState[48];
static char topicBright[48];
static char topicSet[48];
static char topicSetBright[48];
static char topicAvail[48];
static char topicDisc[64];
static bool idsBuilt = false;

static char lastBroker[128];  // broker we are (or last tried) connected to
static uint32_t nextTryMs = 0;

// Build the per-wall id and all six topics. The MAC is fixed, so this runs
// once; WiFi.mode() has already run by the time we are called.
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
           idHex, OTW_FW_VERSION);
  mqtt.publish(topicDisc, buf, true);
}

static void mqttMsg(char *topic, byte *payload, unsigned int len) {
  if (len == 0) {
    return;
  }
  if (strcmp(topic, topicSet) == 0) {
    // PubSubClient does not NUL-terminate; compare against the length.
    powered = len == 2 && memcmp(payload, "ON", 2) == 0;
    applyBrightness(powered ? (curBrightness ? curBrightness : DISPLAY_BRIGHTNESS) : 0);
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
    applyBrightness((uint8_t)v);
    haPrefs.putInt(HA_BRIGHT_KEY, v);  // survive the next boot without HA
    publishState();
  }
}

static bool connectBroker() {
  const char *broker = provMqttHost();
  const char *user = provMqttUser();
  const char *pass = provMqttPass();
  char clientId[16];
  snprintf(clientId, sizeof(clientId), "otw-%s", idHex);
  mqtt.setServer(broker, provMqttPort());
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

uint8_t haBrightness() { return curBrightness; }
bool haConnected() { return mqtt.connected(); }

void haService() {
  const char *broker = provMqttHost();
  if (!broker[0] || WiFi.status() != WL_CONNECTED) {
    if (mqtt.connected()) {
      mqtt.disconnect();
    }
    lastBroker[0] = '\0';
    return;
  }
  if (!idsBuilt) {
    buildIds();
    idsBuilt = true;
  }
  if (!mqtt.connected() || strcmp(lastBroker, broker) != 0) {
    if ((int32_t)(millis() - nextTryMs) < 0) {
      return;  // backoff between attempts
    }
    nextTryMs = millis() + HA_RETRY_MS;
    if (!connectBroker()) {
      // PubSubClient state(): -1 no TCP, -2 connect fail, 4/5/6 broker
      // refused (unauth / id / credentials).
      Serial.printf("[ha] broker %s:%u unreachable rc=%d\n", broker,
                    provMqttPort(), mqtt.state());
      return;
    }
    snprintf(lastBroker, sizeof(lastBroker), "%s", broker);
    mqtt.subscribe(topicSet);
    mqtt.subscribe(topicSetBright);
    mqtt.publish(topicAvail, "online", true);
    publishDiscovery();
    publishState();
    Serial.printf("[ha] mqtt broker %s:%u — light entity announced\n",
                  broker, provMqttPort());
    return;
  }
  mqtt.loop();  // keepalive + command delivery
}
