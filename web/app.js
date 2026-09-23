// Browser flasher for open-transit-wall.
//
// One Web Serial session does both jobs: esptool-js writes the merged firmware
// image, then a small fixed-layout config blob (WiFi credentials + proxy
// endpoint) goes into the dedicated `cfg` partition. The firmware prefers that
// blob over its compiled secrets.h fallback — see firmware/include/device_config.h.
//
// Served statically; nothing leaves the machine except the firmware fetch from
// this same origin.

import { ESPLoader, Transport } from "./vendor/esptool-js.js";
import { buildConfigBlob, utf8Length } from "./config-blob.js";

const MANIFEST_URL = "./firmware/manifest.json";

// High speed first; the CH34x bridges on many clone boards choke on it, so we
// transparently fall back to the classic 115200.
const BAUDRATES = [921600, 115200];

const els = {
  form: document.getElementById("flash-form"),
  unsupported: document.getElementById("unsupported"),
  manifestError: document.getElementById("manifest-error"),
  ssid: document.getElementById("ssid"),
  password: document.getElementById("password"),
  host: document.getElementById("proxy-host"),
  port: document.getElementById("proxy-port"),
  configureOnly: document.getElementById("configure-only"),
  togglePassword: document.getElementById("toggle-password"),
  install: document.getElementById("install-btn"),
  progressWrap: document.getElementById("progress-wrap"),
  progressBar: document.getElementById("progress-bar"),
  progressLabel: document.getElementById("progress-label"),
  status: document.getElementById("status"),
  log: document.getElementById("log"),
  manifestInfo: document.getElementById("manifest-info"),
};

let manifest = null;
let busy = false;

// --- small UI helpers -------------------------------------------------------

function appendLog(text) {
  const atBottom =
    els.log.scrollTop + els.log.clientHeight >= els.log.scrollHeight - 8;
  els.log.textContent += (els.log.textContent ? "\n" : "") + text;
  // Trim to the last ~400 lines so a long flash doesn't grow the DOM forever.
  const lines = els.log.textContent.split("\n");
  if (lines.length > 400) els.log.textContent = lines.slice(-400).join("\n");
  if (atBottom) els.log.scrollTop = els.log.scrollHeight;
}

function setStatus(text, state = "") {
  els.status.textContent = text;
  els.status.dataset.state = state;
}

function setProgress(fraction) {
  const pct = Math.max(0, Math.min(1, fraction));
  els.progressWrap.hidden = false;
  els.progressBar.style.width = `${(pct * 100).toFixed(1)}%`;
  els.progressLabel.textContent = `${Math.round(pct * 100)}%`;
}

function resetProgress() {
  els.progressWrap.hidden = true;
  els.progressBar.style.width = "0%";
  els.progressLabel.textContent = "0%";
}

function setBusy(value) {
  busy = value;
  els.install.disabled = value || !manifest;
  els.install.textContent = value ? "Working…" : "Flash & configure";
  for (const el of [els.ssid, els.password, els.host, els.port, els.configureOnly]) {
    el.disabled = value;
  }
}

// --- config blob ------------------------------------------------------------

// buildConfigBlob/utf8Length live in config-blob.js, which is unit-tested
// (node web/config-blob.test.mjs) and mirrors firmware/include/device_config.h.

// --- hashing / fetching -----------------------------------------------------

async function sha256Hex(bytes) {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)]
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

async function loadManifest() {
  const res = await fetch(MANIFEST_URL, { cache: "no-store" });
  if (!res.ok) throw new Error(`HTTP ${res.status} for ${MANIFEST_URL}`);
  const data = await res.json();
  if (!data.firmware?.path || !data.config?.flashOffset) {
    throw new Error("manifest.json is missing firmware.path or config.flashOffset");
  }
  return data;
}

// --- flashing ---------------------------------------------------------------

// reportProgress gives per-file offsets; turn that into one overall bar.
function makeReporter(files) {
  const sizes = files.map((f) => f.data.length);
  const total = sizes.reduce((a, b) => a + b, 0) || 1;
  return (fileIndex, written) => {
    const base = sizes.slice(0, fileIndex).reduce((a, b) => a + b, 0);
    setProgress((base + written) / total);
  };
}

async function runSession(port, baud, fileArray, eraseAll) {
  const transport = new Transport(port, true);
  const loader = new ESPLoader({
    transport,
    baudrate: baud,
    terminal: {
      clean: () => {
        els.log.textContent = "";
      },
      writeLine: (data) => appendLog(String(data)),
      write: (data) => appendLog(String(data)),
    },
  });
  try {
    const chip = await loader.main();
    appendLog(`Detected chip: ${chip}`);
    await loader.writeFlash({
      fileArray,
      // "keep" leaves the image headers' flash mode/freq/size untouched — the
      // merged binary already carries PlatformIO's build-time values.
      flashSize: "keep",
      eraseAll,
      compress: true,
      reportProgress: makeReporter(fileArray),
    });
    await loader.after("hard_reset");
    return chip;
  } finally {
    try {
      await transport.disconnect();
    } catch (err) {
      appendLog(`port close: ${err.message}`);
    }
  }
}

function validate(form) {
  if (!form.ssid) throw new Error("Enter the WiFi network name.");
  if (utf8Length(form.ssid) > 32) throw new Error("SSID is too long (max 32 bytes).");
  if (utf8Length(form.password) > 64) {
    throw new Error("WiFi password is too long (max 64 bytes).");
  }
  if (!form.host) throw new Error("Enter the proxy host.");
  if (/[\s/:]/.test(form.host)) {
    throw new Error("Proxy host must be a bare hostname or IP (no scheme, port, or slash).");
  }
  if (utf8Length(form.host) > 63) throw new Error("Proxy host is too long (max 63 bytes).");
  if (!Number.isInteger(form.port) || form.port < 1 || form.port > 65535) {
    throw new Error("Proxy port must be between 1 and 65535.");
  }
}

async function flash() {
  const form = {
    ssid: els.ssid.value.trim(),
    password: els.password.value,
    host: els.host.value.trim(),
    port: Number.parseInt(els.port.value, 10),
  };
  validate(form);

  const configureOnly = els.configureOnly.checked;
  const configOffset = manifest.config.flashOffset;

  let firmwareBytes = null;
  if (!configureOnly) {
    setStatus("Downloading firmware…", "busy");
    const res = await fetch(manifest.firmware.path, { cache: "no-store" });
    if (!res.ok) throw new Error(`HTTP ${res.status} for ${manifest.firmware.path}`);
    firmwareBytes = new Uint8Array(await res.arrayBuffer());
    if (manifest.firmware.sha256) {
      const got = await sha256Hex(firmwareBytes);
      if (got !== manifest.firmware.sha256) {
        throw new Error(
          `Firmware checksum mismatch (expected ${manifest.firmware.sha256.slice(0, 12)}…, got ${got.slice(0, 12)}…).`,
        );
      }
      appendLog(`firmware sha256 ok (${firmwareBytes.length} bytes)`);
    }
  }

  const configBlob = buildConfigBlob(form);
  appendLog(`config blob ${configBlob.length} bytes @ 0x${configOffset.toString(16)}`);

  const fileArray = [];
  if (firmwareBytes) {
    fileArray.push({
      data: firmwareBytes,
      address: manifest.firmware.flashOffset ?? 0,
    });
  }
  fileArray.push({ data: configBlob, address: configOffset });

  const port = await navigator.serial.requestPort();
  let lastError = null;
  for (const baud of BAUDRATES) {
    try {
      setStatus(`Connecting at ${baud} baud…`, "busy");
      setProgress(0);
      const chip = await runSession(port, baud, fileArray, !configureOnly);
      setProgress(1);
      setStatus(
        `Done — ${chip} flashed and configured. It will now reboot and join "${form.ssid}".`,
        "ok",
      );
      return;
    } catch (err) {
      lastError = err;
      appendLog(`attempt at ${baud} baud failed: ${err.message}`);
      if (baud !== BAUDRATES[BAUDRATES.length - 1]) {
        appendLog("retrying at a lower baud rate…");
      }
    }
  }
  throw lastError ?? new Error("Flashing failed.");
}

// --- wiring -----------------------------------------------------------------

async function init() {
  if (!("serial" in navigator)) {
    els.unsupported.hidden = false;
    setStatus("Unsupported browser.", "error");
    return;
  }

  els.togglePassword.addEventListener("click", () => {
    const showing = els.password.type === "text";
    els.password.type = showing ? "password" : "text";
    els.togglePassword.textContent = showing ? "Show" : "Hide";
    els.togglePassword.setAttribute("aria-label", showing ? "Show password" : "Hide password");
  });

  els.form.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (busy) return;
    els.log.textContent = "";
    resetProgress();
    setBusy(true);
    try {
      await flash();
    } catch (err) {
      setStatus(err.message, "error");
      appendLog(`ERROR: ${err.message}`);
    } finally {
      setBusy(false);
    }
  });

  try {
    manifest = await loadManifest();
    els.manifestInfo.textContent = manifest.version ? `firmware ${manifest.version}` : "";
    setStatus("Ready. Enter your network details and click Flash & configure.", "");
  } catch (err) {
    els.manifestError.hidden = false;
    els.manifestError.innerHTML =
      `<strong>No firmware manifest found.</strong> ` +
      `The firmware build step hasn't run for this deployment (<code>${MANIFEST_URL}</code>).`;
    setStatus("Firmware manifest unavailable.", "error");
  }
  setBusy(false);
}

init();