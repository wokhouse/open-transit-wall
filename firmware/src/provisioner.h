#pragma once
// Runtime-provisioned settings (WiFi credentials + proxy endpoint), stored in
// NVS and editable over the serial console. This is the ONLY way the device
// ever learns network configuration: the binary carries no secrets, no
// endpoint, no defaults. One firmware binary is flashed to every wall; the
// browser tool writes these values via WebSerial. The full line protocol is
// specified in docs/provisioning-protocol.md.

#include <Arduino.h>

#define OTW_FW_VERSION 3

// Load the stored values into RAM (call once, before any WiFi use).
void provLoad();
// True until BOTH a WiFi SSID and a proxy host are set — the wall stays in
// provisioning mode (backdrop shown, no WiFi, no polling) until then.
bool provNeeded();
// Consume stdin; answer on stdout. Non-blocking except for a partial line.
void provPumpSerial();

// Live values for the running session (compiled defaults until provisioned).
const char *provSsid();
const char *provPass();
const char *provScheme();
const char *provPollHost();  // host without scheme or port
uint16_t provPort();
// Registry id of the agency this wall displays ("" = no agency param, which
// works against single-agency servers).
const char *provAgency();
// Optional Home Assistant MQTT broker (see ha_link.h). Empty host means the
// HA link is off — the wall works exactly as before without one.
const char *provMqttHost();
uint16_t provMqttPort();  // 1883 when unset
const char *provMqttUser();
const char *provMqttPass();
