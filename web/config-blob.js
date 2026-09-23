// Fixed-layout device config blob written to the ESP32 `cfg` partition.
//
// This is the JS half of a three-way contract — keep all three in sync:
//   firmware/src/device_config.cpp  (reader + CRC check)
//   web/config-blob.js              (this file — the flasher's writer)
//   tools/make_config.py            (writer for scripted/manual flashing)
//
// Blob layout, little-endian, packed:
//   [0]  u32 magic    ("OTWC")     [4]  u16 version  [6]  u16 payload length
//   [8]  u32 crc32    (of payload) [12] u32 reserved (0)
//   [16] payload:  ssid[33] password[65] proxy_host[64] u16 port u16 flags

export const CFG = {
  magic: 0x4f545743, // "OTWC" little-endian
  version: 1,
  headerLen: 16,
  ssidLen: 33, // 32 bytes + NUL, IEEE 802.11 max
  passLen: 65, // 64 bytes + NUL, WPA2 max
  hostLen: 64,
};

// Standard IEEE 802.3 CRC32 (reflected, poly 0xEDB88320) — matches crc32Of()
// in firmware/src/device_config.cpp.
const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

export function crc32(bytes) {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    crc = CRC_TABLE[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export function utf8Length(value) {
  return new TextEncoder().encode(value).length;
}

// NUL-terminated UTF-8 in a fixed-width field; throws if it won't fit.
function fixedString(value, length, field) {
  const out = new Uint8Array(length);
  const bytes = new TextEncoder().encode(value);
  if (bytes.length > length - 1) {
    throw new Error(`${field} is too long (max ${length - 1} bytes as UTF-8).`);
  }
  out.set(bytes);
  return out;
}

export function buildConfigBlob({ ssid, password, host, port }) {
  const payload = new Uint8Array(
    CFG.ssidLen + CFG.passLen + CFG.hostLen + 2 + 2,
  );
  let offset = 0;
  const put = (bytes) => {
    payload.set(bytes, offset);
    offset += bytes.length;
  };
  put(fixedString(ssid, CFG.ssidLen, "SSID"));
  put(fixedString(password, CFG.passLen, "WiFi password"));
  put(fixedString(host, CFG.hostLen, "Proxy host"));

  const payloadView = new DataView(payload.buffer);
  payloadView.setUint16(offset, port, true);
  payloadView.setUint16(offset + 2, 0, true); // flags (reserved)

  const blob = new Uint8Array(CFG.headerLen + payload.length);
  const header = new DataView(blob.buffer);
  header.setUint32(0, CFG.magic, true);
  header.setUint16(4, CFG.version, true);
  header.setUint16(6, payload.length, true);
  header.setUint32(8, crc32(payload), true);
  header.setUint32(12, 0, true); // reserved
  blob.set(payload, CFG.headerLen);
  return blob;
}