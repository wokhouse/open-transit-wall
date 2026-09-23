// Reads the `cfg` partition written by the web flasher. See device_config.h
// for the blob format. Any failure is non-fatal: the caller falls back to the
// compiled secrets.h values, so a board flashed without a blob still runs.

#include "device_config.h"

#include <esp_partition.h>
#include <string.h>

// Standard IEEE 802.3 CRC32 (reflected, poly 0xEDB88320), matching the JS
// implementation in web/app.js and tools/make_config.py.
static uint32_t crc32Of(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (~((crc & 1) - 1)));
    }
  }
  return ~crc;
}

bool loadDeviceConfig(RuntimeConfig *out) {
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)CFG_PARTITION_SUBTYPE,
      CFG_PARTITION_LABEL);
  if (part == nullptr) {
    Serial.println("[cfg] no cfg partition — using compiled defaults");
    return false;
  }

  DeviceConfigHeader header;
  if (esp_partition_read(part, 0, &header, sizeof(header)) != ESP_OK) {
    Serial.println("[cfg] partition read failed");
    return false;
  }
  if (header.magic != CFG_MAGIC) {
    Serial.println("[cfg] empty — using compiled defaults");
    return false;
  }
  if (header.version != CFG_VERSION || header.length != CFG_PAYLOAD_LEN) {
    Serial.printf("[cfg] version/size mismatch (%u/%u) — ignoring\n",
                  header.version, header.length);
    return false;
  }

  DeviceConfigPayload payload;
  if (esp_partition_read(part, sizeof(header), &payload, sizeof(payload)) !=
      ESP_OK) {
    Serial.println("[cfg] payload read failed");
    return false;
  }
  if (crc32Of((const uint8_t *)&payload, sizeof(payload)) != header.crc32) {
    Serial.println("[cfg] CRC mismatch — ignoring");
    return false;
  }

  // A writer could have filled a field without a terminator; force one.
  payload.ssid[CFG_SSID_LEN - 1] = '\0';
  payload.password[CFG_PASS_LEN - 1] = '\0';
  payload.proxy_host[CFG_HOST_LEN - 1] = '\0';

  memset(out, 0, sizeof(*out));
  memcpy(out->ssid, payload.ssid, CFG_SSID_LEN);
  memcpy(out->password, payload.password, CFG_PASS_LEN);
  memcpy(out->proxyHost, payload.proxy_host, CFG_HOST_LEN);
  out->proxyPort = payload.proxy_port;
  out->fromFlash = true;
  return true;
}