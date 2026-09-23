#pragma once
// Runtime device configuration stored in the `cfg` flash partition.
//
// The web flasher writes a fixed-layout blob here right after flashing the
// application, so one firmware binary works for every user: WiFi credentials
// and the proxy endpoint live in flash, not in the build. A plain
// `pio run -t upload` (no blob present) falls back to compiled secrets.h.
//
// The on-flash format is shared three ways — keep them in sync:
//   - firmware/src/device_config.cpp  (reader + CRC check, authoritative)
//   - web/app.js                      (buildConfigBlob, used by the flasher)
//   - tools/make_config.py            (builds a blob for manual esptool writes)

#include <Arduino.h>

#define CFG_MAGIC 0x4F545743u  // "OTWC" little-endian
#define CFG_VERSION 1
#define CFG_PARTITION_LABEL "cfg"
#define CFG_PARTITION_SUBTYPE 0x40  // custom data subtype, see partitions.csv

#define CFG_SSID_LEN 33  // 32 bytes (IEEE 802.11 max) + NUL
#define CFG_PASS_LEN 65  // 64 bytes (WPA2 max) + NUL
#define CFG_HOST_LEN 64  // hostname or dotted-quad + NUL

// Blob layout, little-endian, packed:
//   [0]  u32 magic   [4]  u16 version  [6]  u16 length (payload bytes)
//   [8]  u32 crc32   [12] u32 reserved (0)
//   [16] payload
typedef struct __attribute__((packed)) DeviceConfigPayload {
  char ssid[CFG_SSID_LEN];
  char password[CFG_PASS_LEN];
  char proxy_host[CFG_HOST_LEN];
  uint16_t proxy_port;
  uint16_t flags;  // reserved, 0
} DeviceConfigPayload;

typedef struct __attribute__((packed)) DeviceConfigHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t length;
  uint32_t crc32;
  uint32_t reserved;
} DeviceConfigHeader;

#define CFG_PAYLOAD_LEN ((uint16_t)sizeof(DeviceConfigPayload))
#define CFG_BLOB_LEN (sizeof(DeviceConfigHeader) + CFG_PAYLOAD_LEN)

// Effective settings the firmware runs on (flash blob or compiled fallback).
struct RuntimeConfig {
  char ssid[CFG_SSID_LEN];
  char password[CFG_PASS_LEN];
  char proxyHost[CFG_HOST_LEN];
  uint16_t proxyPort;
  bool fromFlash;  // true = loaded from the cfg partition
};

// Loads and validates the cfg partition into `out`. Returns false (leaving
// `out` untouched) when the partition or blob is missing, mis-sized, or fails
// its CRC, so the caller can fall back to compiled defaults.
bool loadDeviceConfig(RuntimeConfig *out);