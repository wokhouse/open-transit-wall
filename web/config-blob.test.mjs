// Self-check for the config blob the flasher writes to the ESP32 `cfg`
// partition. Run with:  node web/config-blob.test.mjs
//
// Deliberately uses an independent bit-at-a-time CRC so a typo in the shared
// table-driven implementation can't hide. tools/make_config.py and
// firmware/src/device_config.cpp must agree with this layout.

import assert from "node:assert/strict";
import { buildConfigBlob, crc32, utf8Length, CFG } from "./config-blob.js";

// Reference CRC32 without the lookup table (same polynomial, different code).
function referenceCrc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let k = 0; k < 8; k++) {
      crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// 1. CRC agrees with the independent implementation across sizes/values.
for (const bytes of [
  new Uint8Array(0),
  new Uint8Array([0]),
  new Uint8Array([0xff, 0x00, 0x7f, 0x80]),
  Uint8Array.from({ length: 166 }, (_, i) => (i * 37) & 0xff),
]) {
  assert.equal(crc32(bytes), referenceCrc32(bytes), "crc32 mismatch");
}
assert.equal(crc32(new TextEncoder().encode("123456789")), 0xcbf43926);

// 2. Layout matches firmware/include/device_config.h.
const input = {
  ssid: "MyHomeWiFi",
  password: "correct horse battery",
  host: "192.168.1.50",
  port: 8000,
};
const blob = buildConfigBlob(input);
assert.equal(blob.length, CFG.headerLen + CFG.ssidLen + CFG.passLen + CFG.hostLen + 4);
assert.equal(blob.length, 182);

const view = new DataView(blob.buffer);
assert.equal(view.getUint32(0, true), CFG.magic);
assert.equal(view.getUint16(4, true), 1);
assert.equal(view.getUint16(6, true), 166);
assert.equal(view.getUint32(12, true), 0);

const payload = blob.subarray(CFG.headerLen);
assert.equal(view.getUint32(8, true), referenceCrc32(payload));

// 3. Fields decode back, NUL-terminated in their fixed slots.
const text = (start, len) => {
  const field = payload.subarray(start, start + len);
  assert.equal(field[field.length - 1], 0, "field must be NUL-terminated");
  return new TextDecoder().decode(field.subarray(0, field.indexOf(0)));
};
assert.equal(text(0, CFG.ssidLen), input.ssid);
assert.equal(text(CFG.ssidLen, CFG.passLen), input.password);
assert.equal(text(CFG.ssidLen + CFG.passLen, CFG.hostLen), input.host);
assert.equal(
  new DataView(payload.buffer, payload.byteOffset).getUint16(CFG.ssidLen + CFG.passLen + CFG.hostLen, true),
  input.port,
);

// 4. Boundary conditions: full-size fields fit, one byte over throws.
buildConfigBlob({ ssid: "a".repeat(32), password: "b".repeat(64), host: "c".repeat(63), port: 1 });
assert.throws(() => buildConfigBlob({ ...input, ssid: "a".repeat(33) }), /SSID is too long/);
assert.throws(() => buildConfigBlob({ ...input, password: "b".repeat(65) }), /password is too long/);
assert.throws(() => buildConfigBlob({ ...input, host: "c".repeat(64) }), /host is too long/);
assert.equal(utf8Length("é"), 2, "lengths must be UTF-8 bytes, not code points");

// 5. Open networks (empty password) are valid.
assert.doesNotThrow(() => buildConfigBlob({ ...input, password: "" }));

console.log("config-blob: all checks passed");