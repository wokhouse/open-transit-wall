// open-transit-wall web flasher.
// Speaks the line protocol in docs/provisioning-protocol.md (OTW-FW 5) and
// flashes via the vendored esptool-js bundle. No framework, no build step.

import { ESPLoader, Transport } from './vendor/esptool-js/bundle.js';

const $ = (id) => document.getElementById(id);
const els = {
  fwVersion: $('fw-version'), browserWarn: $('browser-warn'),
  btnConnect: $('btn-connect'), connState: $('conn-state'),
  btnFlash: $('btn-flash'), boardPreset: $('board-preset'), flashLog: $('flash-log'),
  serverUrl: $('server-url'), serverPreset: $('server-preset'),
  btnDiscover: $('btn-discover'), discoverState: $('discover-state'),
  agency: $('agency'), agencyManual: $('agency-manual'),
  ssid: $('ssid'), pass: $('pass'),
  mqttSection: $('mqtt-section'),
  mhost: $('mhost'), mport: $('mport'), muser: $('muser'), mpass: $('mpass'),
  bmin: $('bmin'), bmax: $('bmax'), bwindow: $('bwindow'),
  btnSave: $('btn-save'), btnErase: $('btn-erase'), provLog: $('prov-log'),
  console: $('console'), cmd: $('cmd'), btnSendCmd: $('btn-send-cmd'),
  btnClearConsole: $('btn-clear-console'),
};

if (!('serial' in navigator)) els.browserWarn.hidden = false;

let manifest = null;   // firmware/manifest.json
let port = null;       // SerialPort picked by the user
let session = null;    // { reader, writer, buf, lines, lastLineAt }
const enc = new TextEncoder();

function logTo(pre, msg) {
  pre.hidden = false;
  pre.textContent += msg + '\n';
  pre.scrollTop = pre.scrollHeight;
}
const flashLog = (m) => logTo(els.flashLog, m);
const provLog = (m) => logTo(els.provLog, m);
const boardLog = (m) => logTo(els.console, m);

function setBusy(busy) {
  for (const b of [els.btnConnect, els.btnFlash, els.btnDiscover,
                   els.btnSave, els.btnErase, els.btnSendCmd]) {
    b.disabled = busy || b.dataset.wantDisabled === '1';
  }
}

// ---------- manifest ----------

async function loadManifest() {
  try {
    const r = await fetch('firmware/manifest.json');
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    manifest = await r.json();
    els.fwVersion.textContent =
      `built ${manifest.built}, version ${manifest.version}, provisioning protocol v${manifest.fw_version}`;
  } catch (e) {
    els.fwVersion.textContent = `firmware manifest unavailable (${e.message}) — ` +
      'the Pages build stages it; a bare flasher/ checkout has none.';
  }
}
loadManifest();

// The image set for the board picked in the dropdown.
function selectedBoard() {
  if (!manifest) return null;
  return manifest.boards.find((b) => b.id === els.boardPreset.value) || null;
}

// ---------- serial line session ----------
// One reader loop feeds both the console view and command replies. Replies
// can be multi-line (get all, status, help), so callers wait for the line
// stream to go idle rather than assuming one command = one line.

async function startSession() {
  if (session && !session.dead) await endSession();
  await port.open({ baudRate: 115200 });
  session = { reader: port.readable.getReader(),
              writer: port.writable.getWriter(),
              buf: '', lines: [], lastLineAt: 0, dead: false };
  readLoop();
}

// Release the Web Serial port cleanly: cancel the reader (unlocks it),
// drop both locks, then close. Without this the port stays locked and the
// next open() fails with "The port is already open".
async function endSession() {
  if (!session) return;
  const s = session;
  session = null;
  s.dead = true;
  try { await s.reader.cancel(); } catch { /* already gone */ }
  try { s.reader.releaseLock(); } catch { /* not locked */ }
  try { s.writer.releaseLock(); } catch { /* not locked */ }
  try { await port.close(); } catch { /* already closed */ }
}

async function readLoop() {
  const dec = new TextDecoder();
  const s = session;
  try {
    while (true) {
      const { value, done } = await s.reader.read();
      if (done) break;
      s.buf += dec.decode(value, { stream: true });
      let nl;
      while ((nl = s.buf.indexOf('\n')) >= 0) {
        const line = s.buf.slice(0, nl).replace(/\r$/, '');
        s.buf = s.buf.slice(nl + 1);
        if (line.length) {
          boardLog(line);
          s.lines.push(line);
          s.lastLineAt = Date.now();
        }
      }
    }
  } catch (e) {
    if (session === s) boardLog(`!! serial read error: ${e.message}`);
  }
  if (session === s) {
    s.dead = true;
    els.connState.textContent = 'disconnected';
    boardLog('!! serial port closed');
  }
}

async function sendLine(line) {
  await session.writer.write(enc.encode(line + '\n'));
}

// Collect reply lines: wait until nothing arrives for idleMs (capped at
// maxMs total). Returns the lines seen since the last drain.
async function awaitLines(idleMs = 700, maxMs = 8000) {
  const s = session;
  if (!s) return [];
  const start = Date.now();
  s.lines = [];
  s.lastLineAt = Date.now();
  while (Date.now() - s.lastLineAt < idleMs && Date.now() - start < maxMs) {
    if (s.dead) break;
    await new Promise((r) => setTimeout(r, 50));
  }
  return s.lines.slice();
}

async function ask(cmd, idleMs, maxMs) {
  await sendLine(cmd);
  return awaitLines(idleMs, maxMs);
}

// ---------- banner / board state ----------

let board = null;  // { fw, stored: {ssid, passSet, host, scheme, port, agency, mhost, mport, muser} | null }

async function readBanner() {
  await sendLine('');  // any serial activity makes the firmware print the banner
  const lines = await awaitLines(1200, 6000);
  board = { fw: 0, stored: null };
  let sawBanner = false;
  for (const line of lines) {
    let m = line.match(/^OTW-FW (\d+)/);
    if (m) { board.fw = Number(m[1]); sawBanner = true; }
    if (line.startsWith('UNPROVISIONED') || line === 'STORED none') board.stored = null;
    // scheme/port/agency can be empty strings in the firmware's printf.
    // The m*/b* tail only appears on protocol v3+ boards (bmin/bmax on v4).
    m = line.match(/^STORED ssid=(.*?) pass=(set|empty) host=(\S*) scheme=(\S*) port=(\d*) agency=(\S*)(?: mhost=(\S*) mport=(\d*) muser=(\S*) mpass=(set|empty)(?: bmin=(\d*) bmax=(\d*))?)?$/);
    if (m) {
      board.stored = {
        ssid: m[1], passSet: m[2] === 'set', host: m[3],
        scheme: m[4], port: Number(m[5]), agency: m[6],
        mhost: m[7] ?? '', mport: Number(m[8] || 0), muser: m[9] ?? '',
        bmin: m[11] ?? '', bmax: m[12] ?? '',
      };
    }
  }
  if (!sawBanner) {
    // The banner is printed once per boot; if it was already consumed
    // (e.g. a serial monitor saw it first), read the live state instead.
    boardLog('(no banner — falling back to `status`)');
    for (const line of await ask('status', 1200, 8000)) {
      let m = line.match(/^STATUS fw=(\d+) provisioned=(yes|no)/);
      if (m) board.fw = Number(m[1]);
      m = line.match(/^STATUS ssid=(.*) pass=(set|\(unset\))$/);
      if (m) board.stored = board.stored || {};
      if (m && board.stored) {
        board.stored.ssid = m[1] === '(unset)' ? '' : m[1];
        board.stored.passSet = m[2] === 'set';
      }
      m = line.match(/^STATUS proxy=(https?:\/\/[^:]+):(\d+)\/vehicles(?:\?agency=([a-z0-9]*))?$/);
      if (m && board.stored) {
        board.stored.scheme = m[1];
        board.stored.host = m[2];
        board.stored.port = Number(m[3]);
      }
      m = line.match(/^STATUS agency=([a-z0-9]+|\(unset\))$/);
      if (m && board.stored) board.stored.agency = m[1] === '(unset)' ? '' : m[1];
      m = line.match(/^STATUS mqtt=(\S+):(\d+) (connected|down)$/);
      if (m && board.stored) {
        board.stored.mhost = m[1];
        board.stored.mport = Number(m[2]);
        board.stored.muser = board.stored.muser ?? '';
      }
    }
    if (board.stored && !board.stored.ssid) board.stored = null;
  }
  if (!board.fw) {
    boardLog('!! no OTW-FW banner and no `status` reply — is the board sitting in the ROM bootloader? Flash it (step 2).');
    return;
  }
  if (manifest && board.fw < manifest.fw_version) {
    provLog(`board speaks protocol v${board.fw}, this page expects v${manifest.fw_version} — flash it first (step 2).`);
  } else {
    provLog(`board speaks protocol v${board.fw}.`);
  }
  if (board.stored) {
    els.ssid.value = board.stored.ssid;
    els.agencyManual.value = board.stored.agency;
    // Point the Server URL at whatever server the board already uses.
    if (board.stored.host) {
      const scheme = board.stored.scheme || 'https://';
      const portPart = board.stored.port && board.stored.port !== 443 ? `:${board.stored.port}` : '';
      els.serverUrl.value = `${scheme}${board.stored.host}${portPart}`;
      syncServerPreset();
    }
    if (board.stored.mhost) {
      els.mhost.value = board.stored.mhost;
      if (board.stored.mport && board.stored.mport !== 1883) els.mport.value = String(board.stored.mport);
      els.muser.value = board.stored.muser || '';
    }
    // Stored bounds are 0-255; the form speaks percent like Home Assistant.
    if (board.stored.bmin && Number(board.stored.bmin) !== 0) {
      els.bmin.value = String(Math.round((Number(board.stored.bmin) * 100) / 255));
    }
    if (board.stored.bmax && Number(board.stored.bmax) !== 255) {
      els.bmax.value = String(Math.round((Number(board.stored.bmax) * 100) / 255));
    }
    provLog('prefilled from the board — change anything and send.');
  } else {
    provLog('board is unprovisioned — fill the form and send.');
  }
  // The MQTT fields only exist on protocol v3+ boards; older firmware would
  // answer ERR unknown key, so keep the section closed for them. The
  // brightness window needs v4.
  els.mqttSection.hidden = board.fw < 3;
  els.bwindow.hidden = board.fw < 4;
  enableProvisioning();
}

function enableProvisioning() {
  els.btnSave.disabled = false;
  els.btnErase.disabled = false;
  els.cmd.disabled = false;
  els.btnSendCmd.disabled = false;
  els.btnClearConsole.disabled = false;
  els.btnDiscover.dataset.wantDisabled = '0';
  setBusy(false);
}

// ---------- server URL presets ----------
// The dropdown is a convenience for the URL input: picking a preset fills
// the input, typing a URL that matches no preset switches it to
// "Custom…". The input stays the single source of truth.
function syncServerPreset() {
  const v = els.serverUrl.value.trim();
  const match = [...els.serverPreset.options].some(o => o.value && o.value === v);
  els.serverPreset.value = match ? v : '';
}
els.serverPreset.onchange = () => {
  if (els.serverPreset.value) els.serverUrl.value = els.serverPreset.value;
};
els.serverUrl.addEventListener('input', syncServerPreset);

// ---------- agency discovery ----------

els.btnDiscover.onclick = async () => {
  const url = els.serverUrl.value.trim().replace(/\/+$/, '');
  els.discoverState.textContent = '';
  if (!/^https?:\/\/[^ ]+$/.test(url)) {
    els.discoverState.textContent = 'enter a full http(s):// URL first';
    return;
  }
  els.btnDiscover.disabled = true;
  els.discoverState.textContent = 'asking the server…';
  try {
    const r = await fetch(`${url}/agencies`);
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const data = await r.json();
    const agencies = data.agencies || [];
    els.agency.innerHTML = '';
    for (const a of agencies) {
      const opt = document.createElement('option');
      opt.value = a.id;
      opt.textContent = `${a.id} — ${a.name}`;
      els.agency.appendChild(opt);
    }
    if (board?.stored?.agency) els.agency.value = board.stored.agency;
    els.agency.disabled = agencies.length === 0;
    els.agencyManual.hidden = agencies.length > 0;
    els.discoverState.textContent = `${agencies.length} agencies`;
  } catch (e) {
    els.discoverState.textContent = `discovery failed (${e.message}) — type the agency id by hand`;
    els.agency.disabled = true;
    els.agencyManual.hidden = false;
  } finally {
    els.btnDiscover.disabled = false;
  }
};

// ---------- provisioning ----------

function chosenAgency() {
  return els.agency.disabled ? els.agencyManual.value.trim() : els.agency.value;
}

// Split the Server URL into the three values the firmware stores separately
// (set host / set scheme / set port). Returns null unless it parses to a
// plain http(s) origin with a valid host.
function serverParts(raw) {
  let u;
  try { u = new URL(raw.trim()); } catch { return null; }
  if (u.protocol !== 'https:' && u.protocol !== 'http:') return null;
  if (!/^[a-z0-9.-]{1,127}$/i.test(u.hostname)) return null;
  const port = u.port ? Number(u.port) : (u.protocol === 'https:' ? 443 : 80);
  return { host: u.hostname, scheme: u.protocol + '//', port };
}

els.btnSave.onclick = async () => {
  if (!session || session.dead) { provLog('not connected.'); return; }
  setBusy(true);
  const cmds = [];
  if (els.ssid.value) cmds.push(`set ssid=${els.ssid.value}`);
  if (els.pass.value) cmds.push(`set pass=${els.pass.value}`);
  // One Server URL field serves both discovery and the board: split it into
  // the scheme/host/port the provisioning protocol stores.
  const parts = serverParts(els.serverUrl.value);
  if (!parts) {
    provLog('a server URL is required — the wall needs a server to poll.');
    setBusy(false); return;
  }
  cmds.push(`set host=${parts.host}`, `set scheme=${parts.scheme}`, `set port=${parts.port}`);
  const agency = chosenAgency();
  if (agency) {
    if (!/^[a-z0-9]{1,8}$/.test(agency)) {
      provLog(`agency "${agency}" is not [a-z0-9]{1,8} — fix it before sending.`);
      setBusy(false); return;
    }
    cmds.push(`set agency=${agency}`);
  }
  // Optional Home Assistant link (protocol v3+; v5 finds the broker via
  // mDNS when mhost is empty). Only send what differs from what the
  // board reported; an empty mhost clears a stored one and re-enables
  // auto-discovery.
  if (board && board.fw >= 3) {
    const mhost = els.mhost.value.trim();
    if (mhost) {
      if (!/^[^\s/:]{1,127}$/.test(mhost)) {
        provLog(`broker host "${mhost}" has a space, ':' or '/' — host only, no port.`);
        setBusy(false); return;
      }
      cmds.push(`set mhost=${mhost}`);
    } else if (board.stored?.mhost) {
      cmds.push('set mhost=');
    }
    const mport = els.mport.value.trim();
    if (mport) {
      if (!/^\d{1,5}$/.test(mport) || Number(mport) < 1 || Number(mport) > 65535) {
        provLog(`broker port "${mport}" is not 1-65535 — leave it empty for 1883.`);
        setBusy(false); return;
      }
      cmds.push(`set mport=${mport}`);
    }
    if (els.muser.value) cmds.push(`set muser=${els.muser.value}`);
    if (els.mpass.value) cmds.push(`set mpass=${els.mpass.value}`);
    // Brightness window (protocol v4). The form speaks percent like the
    // Home Assistant slider; the wire carries 0-255 like every other
    // brightness value on the wall. Reset the window to defaults first so
    // the firmware's cross-bound checks cannot be tripped by command order.
    const bmin = els.bmin.value.trim(), bmax = els.bmax.value.trim();
    if (board.fw >= 4 && (bmin || bmax || board.stored?.bmin || board.stored?.bmax)) {
      if (bmin && (!/^\d{1,3}$/.test(bmin) || Number(bmin) > 100)) {
        provLog(`brightness floor "${bmin}" is not 0-100 — leave it empty for 0.`);
        setBusy(false); return;
      }
      if (bmax && (!/^\d{1,3}$/.test(bmax) || Number(bmax) > 100)) {
        provLog(`brightness ceiling "${bmax}" is not 0-100 — leave it empty for 100.`);
        setBusy(false); return;
      }
      if (bmin && bmax && Number(bmin) > Number(bmax)) {
        provLog(`brightness floor ${bmin}% is above the ceiling ${bmax}% — fix it before sending.`);
        setBusy(false); return;
      }
      cmds.push('set bmin=', 'set bmax=');
      if (bmin) cmds.push(`set bmin=${Math.round((Number(bmin) * 255) / 100)}`);
      if (bmax) cmds.push(`set bmax=${Math.round((Number(bmax) * 255) / 100)}`);
    }
  }
  try {
    for (const c of cmds) {
      const replies = await ask(c);
      for (const r of replies) provLog(`  ${r}`);
      if (replies.some((r) => r.startsWith('ERR'))) {
        provLog('the board rejected a value — fix it and send again.');
        setBusy(false); return;
      }
    }
    for (const r of await ask('save')) provLog(`  ${r}`);
    els.pass.value = '';
    els.mpass.value = '';
    provLog('saved. rebooting; watch the console for the connection attempt.');
    for (const r of await ask('reboot')) provLog(`  ${r}`);
  } catch (e) {
    provLog(`send failed: ${e.message}`);
  }
  setBusy(false);
};

els.btnErase.onclick = async () => {
  if (!session || session.dead) return;
  if (!confirm('Erase wipes the stored WiFi/proxy/agency/MQTT settings and drops the board back to provisioning mode. Continue?')) return;
  for (const r of await ask('erase')) provLog(`  ${r}`);
  board = null;
};

els.btnSendCmd.onclick = async () => {
  const c = els.cmd.value.trim();
  if (!c || !session || session.dead) return;
  els.cmd.value = '';
  await ask(c, 900, 10000);
};
els.cmd.addEventListener('keydown', (e) => { if (e.key === 'Enter') els.btnSendCmd.onclick(); });
els.btnClearConsole.onclick = () => { els.console.textContent = ''; };

// ---------- md5 (for esptool-js's calculateMD5Hash hook) ----------
// esptool-js hands calculateMD5Hash the FINAL bytes it writes (the
// bootloader header may have been patched for the detected flash size),
// then reads the region back and compares. Computing md5 here verifies
// every image's transport regardless of header patching.
function md5hex(bytes) {
  const K = [
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
  ];
  const S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  const len = bytes.length;
  const withPad = new Uint8Array((((len + 8) >> 6) + 1) << 6);
  withPad.set(bytes);
  withPad[len] = 0x80;
  new DataView(withPad.buffer).setUint32(withPad.length - 8, len << 3, true);
  new DataView(withPad.buffer).setUint32(withPad.length - 4, Math.floor(len / 0x100000000), true);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const dv = new DataView(withPad.buffer);
  for (let off = 0; off < withPad.length; off += 64) {
    const M = new Array(16);
    for (let i = 0; i < 16; i++) M[i] = dv.getUint32(off + i * 4, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
      else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) & 15; }
      else { F = C ^ (B | ~D); g = (7 * i) & 15; }
      F = (F + A + K[i] + M[g]) >>> 0;
      A = D; D = C; C = B;
      const s = S[(i >> 4) * 4 + (i % 4)];
      B = (B + ((F << s) | (F >>> (32 - s)))) >>> 0;
    }
    a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
  }
  const out = new Uint8Array(16);
  const odv = new DataView(out.buffer);
  for (const [i, v] of [[0, a0], [1, b0], [2, c0], [3, d0]]) odv.setUint32(i * 4, v, true);
  return [...out].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// ---------- connect / flash ----------

els.btnConnect.onclick = async () => {
  try {
    port = await navigator.serial.requestPort();
  } catch {
    els.connState.textContent = 'no port picked';
    return;
  }
  els.connState.textContent = 'port selected';
  els.btnFlash.disabled = false;
  flashForced = false;  // a new port is a new board: check currency again
};
els.boardPreset.onchange = () => { flashForced = false; };

// Flash first checks whether the board already runs current firmware: if
// the app answers with the manifest's provisioning protocol version, there
// is nothing to flash. A second click re-flashes anyway (same protocol,
// older build). A board in the ROM bootloader never answers, so it falls
// through to flashing.
let flashForced = false;

els.btnFlash.onclick = async () => {
  if (!manifest) { flashLog('no firmware manifest — cannot flash.'); return; }
  const fwBoard = selectedBoard();
  if (!fwBoard) { flashLog('no staged images for the selected board.'); return; }
  setBusy(true);
  els.flashLog.hidden = false;
  if (!flashForced) {
    try {
      await startSession();
      await readBanner();
      if (board && board.fw >= manifest.fw_version) {
        flashLog(`board already runs current firmware (provisioning protocol v${board.fw}) — nothing to flash.`);
        flashLog('click Flash again to re-flash anyway.');
        flashForced = true;
        setBusy(false);
        return;  // leave the session open: provisioning can continue
      }
      await endSession();  // not current (or no app): the loader needs the port
    } catch (e) {
      boardLog(`!! pre-flash check skipped: ${e.message}`);
      await endSession();
    }
  }
  flashForced = false;
  flashLog(`flashing ${fwBoard.images.length} images for ${fwBoard.name} (version ${manifest.version})…`);
  let transport = null;
  try {
    await endSession();  // free the port from any console session first
    // Fit check: with flashSize 'keep' esptool-js skips its own check.
    for (const img of fwBoard.images) {
      if (img.offset + img.size > fwBoard.flash_bytes) {
        throw new Error(`${img.name} at 0x${img.offset.toString(16)} exceeds the ${fwBoard.flash_bytes} byte flash`);
      }
    }
    transport = new Transport(port);
    // terminal adapter: esptool-js info lines into the page's flash log
    const terminal = { clean() {}, write(m) { flashLog(m); }, writeLine(m) { flashLog(m); } };
    const device = new ESPLoader({ transport, terminal });
    // The currency check (or a console session) just closed this port; USB
    // CDC devices — especially the S3 ROM USB-Serial-JTAG — can take a
    // moment to accept open() again. Retry the connect instead of failing
    // the whole flash.
    for (let attempt = 1; ; attempt++) {
      try {
        await device.detectChip();
        break;
      } catch (e) {
        if (attempt >= 3) throw e;
        flashLog(`port not ready (${e.message}) — retrying connect ${attempt}/3…`);
        try { await transport.disconnect(); } catch { /* not open */ }
        await new Promise((r) => setTimeout(r, 600 * attempt));
      }
    }
    // Run the RAM stub like esptool/PlatformIO do. Pure-ROM flashing erases
    // inside FLASH_BEGIN, which this board's flash link answers with status
    // 1,5; the stub defers erase into the write pass and just works.
    await device.runStub();
    const fileArray = [];
    for (const img of fwBoard.images) {
      const data = new Uint8Array(await (await fetch(`firmware/${fwBoard.id}/${img.name}`)).arrayBuffer());
      if (data.length !== img.size) throw new Error(`${img.name}: got ${data.length} bytes, manifest says ${img.size}`);
      fileArray.push({ data, address: img.offset });
    }
    // calculateMD5Hash gets the final bytes esptool-js writes; it then reads
    // each region back and compares. This is a real transport verification.
    // flashSize 'keep': PlatformIO built these images for this board's
    // flash, so the bootloader header is already correct — and 'detect'
    // needs a flash-RDID probe the boot bridge sometimes drops.
    await device.writeFlash({
      fileArray,
      flashSize: fwBoard.flash_size,
      flashMode: 'keep',
      flashFreq: 'keep',
      calculateMD5Hash: md5hex,
      reportProgress: (i, written, total) => {
        if (written === 0 || written === total) {
          flashLog(`${fwBoard.images[i].name}: ${written}/${total}`);
        }
      },
    });
    await device.after();  // hard reset into the new app
    // The Web Serial transport's close API is disconnect(), not close().
    try { await transport.disconnect(); } catch { /* port may already be gone */ }
    transport = null;
    flashLog('flash complete, md5 verified, board reset.');
    // The app's USB CDC re-enumerates a moment after the reset; retry the
    // reopen rather than reporting a failure on a flash that succeeded.
    try {
      for (let attempt = 1; ; attempt++) {
        try { await startSession(); break; }
        catch (e) {
          if (attempt >= 4) throw e;
          await new Promise((r) => setTimeout(r, 800 * attempt));
        }
      }
      await readBanner();
    } catch {
      flashLog('flashed, but the port did not come back on its own — click Connect, then provision in step 3.');
    }
  } catch (e) {
    flashLog(`flash failed: ${e.message}`);
    flashLog('if the board is mid-boot, hold its BOOT button, click Flash again, and release once writing starts.');
    try { await transport?.disconnect(); } catch { /* ignore */ }
  }
  setBusy(false);
};
