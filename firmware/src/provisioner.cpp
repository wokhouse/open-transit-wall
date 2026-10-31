// Serial provisioning protocol + NVS persistence.
//
// Transport: USB serial, 115200 baud, LF-terminated lines (WebSerial's
// writer.send(line + "\n")). Every command gets exactly one reply line:
// "OK ..." on success, "ERR ..." on failure. docs/provisioning-protocol.md
// is the contract the browser tool is built against.
//
// Values live in two places: RAM (the live session — a `set` takes effect
// immediately, no reflash, no reboot) and NVS namespace "otw" (survives
// power cycles, written only by `save`). An empty SSID means unprovisioned.

#include "provisioner.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>

#include "config.h"
#include "ha_link.h"

#define PROV_LINE_MAX   256  // longest command line accepted
#define PROV_SSID_MAX   32   // 802.11 SSID cap
#define PROV_PASS_MAX   63   // 802.11 passphrase cap
#define PROV_HOST_MAX   127
#define PROV_SCHEME_MAX 9    // "https://" / "http://"
#define PROV_AGENCY_MAX 8    // registry id: [a-z0-9]{1,8}
#define PROV_USER_MAX   63   // MQTT username cap

static Preferences prefs;

// Live session values. Everything starts empty: the binary carries no
// network configuration at all — WebSerial provisioning is the only path.
static char curSsid[PROV_SSID_MAX + 1] = "";
static char curPass[PROV_PASS_MAX + 1] = "";
static char curHost[PROV_HOST_MAX + 1] = "";
static char curScheme[PROV_SCHEME_MAX + 1] = "";
static char curAgency[PROV_AGENCY_MAX + 1] = "";
static char curPort[8] = "";
// Optional Home Assistant MQTT broker. Empty host = HA link off; the wall
// needs nothing extra to run as before.
static char curMhost[PROV_HOST_MAX + 1] = "";
static char curMuser[PROV_USER_MAX + 1] = "";
static char curMpass[PROV_PASS_MAX + 1] = "";
static char curMport[8] = "";
// Optional brightness floor/ceiling (0-255). Empty = default full range
// (0/255). The panel maps any Home Assistant level into this window; see
// src/ha_link.cpp and docs/brightness-plan.md.
static char curBmin[4] = "";
static char curBmax[4] = "";

// A line buffer with no complete line yet.
static String partial;

static bool storedSsidSet = false;  // an SSID exists in NVS

static uint16_t portOf(const char *p) {
  if (p[0] == '\0') {
    return 0;  // unset — provNeeded() keeps the poll loop away from it
  }
  int v = String(p).toInt();
  if (v <= 0 || v > 65535) {
    return 0;
  }
  return (uint16_t)v;
}

// Same parse as portOf, but an unset MQTT port means the IANA default.
static uint16_t mqttPortOf(const char *p) {
  uint16_t v = portOf(p);
  return v ? v : 1883;
}

// Parse a stored/live brightness bound; empty or junk means the default.
static uint8_t bvalOf(const char *b, uint8_t dflt) {
  if (b[0] == '\0') {
    return dflt;
  }
  int v = String(b).toInt();
  if (v < 0 || v > 255) {
    return dflt;
  }
  return (uint8_t)v;
}

static void printStored() {
  String ssid = prefs.getString("ssid", "");
  if (ssid.length() == 0) {
    Serial.println("STORED none");
    return;
  }
  Serial.printf("STORED ssid=%s pass=%s host=%s scheme=%s port=%s agency=%s"
                " mhost=%s mport=%s muser=%s mpass=%s bmin=%s bmax=%s\n",
                ssid.c_str(),
                prefs.getString("pass", "").length() ? "set" : "empty",
                prefs.getString("host", "").c_str(),
                prefs.getString("scheme", "").c_str(),
                prefs.getString("port", "").c_str(),
                prefs.getString("agency", "").c_str(),
                prefs.getString("mhost", "").c_str(),
                prefs.getString("mport", "").c_str(),
                prefs.getString("muser", "").c_str(),
                prefs.getString("mpass", "").length() ? "set" : "empty",
                prefs.getString("bmin", "").c_str(),
                prefs.getString("bmax", "").c_str());
}

static void printHelp() {
  Serial.println("CMD set ssid=<wifi network name>");
  Serial.println("CMD set pass=<wifi password>");
  Serial.println("CMD set host=<proxy host, no scheme or port>");
  Serial.println("CMD set scheme=<https://|http://>");
  Serial.println("CMD set port=<proxy port>");
  Serial.println("CMD set agency=<registry id, lowercase a-z0-9 up to 8>");
  Serial.println("CMD set mhost=<mqtt broker host, empty = find it via mDNS>");
  Serial.println("CMD set mport=<mqtt broker port, empty means 1883>");
  Serial.println("CMD set muser=<mqtt username, enables the HA link>");
  Serial.println("CMD set mpass=<mqtt password>");
  Serial.println("CMD set bmin=<brightness floor 0-255, empty means 0>");
  Serial.println("CMD set bmax=<brightness ceiling 0-255, empty means 255>");
  Serial.println("CMD get ssid|pass|host|scheme|port|agency|mhost|mport|muser|mpass|bmin|bmax|all");
  Serial.println("CMD status | save | erase | reboot | help");
}

static void printStatus() {
  Serial.printf("STATUS fw=%d provisioned=%s\n", OTW_FW_VERSION,
                provNeeded() ? "no" : "yes");
  Serial.printf("STATUS ssid=%s pass=%s\n",
                curSsid[0] ? curSsid : "(unset)",
                curPass[0] ? "set" : "(unset)");
  if (curHost[0]) {
    Serial.printf("STATUS proxy=%s%s:%u/vehicles%s%s\n",
                  curScheme[0] ? curScheme : "https://", curHost,
                  portOf(curPort), curAgency[0] ? "?agency=" : "",
                  curAgency);
  } else {
    Serial.println("STATUS proxy=(unset)");
  }
  Serial.printf("STATUS agency=%s\n", curAgency[0] ? curAgency : "(unset)");
  if (curMhost[0]) {
    Serial.printf("STATUS mqtt=%s:%u %s\n", curMhost, mqttPortOf(curMport),
                  haConnected() ? "connected" : "down");
  } else if (curMuser[0]) {
    const char *found = haBroker();
    Serial.printf("STATUS mqtt=auto(%s) %s\n",
                  found[0] ? found : "not-found",
                  haConnected() ? "connected" : "down");
  } else {
    Serial.println("STATUS mqtt=(unset)");
  }
  Serial.printf("STATUS wifi=%s ip=%s\n",
                WiFi.status() == WL_CONNECTED ? "connected" : "down",
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str()
                                              : "-");
  Serial.printf("STATUS heap=%u\n", ESP.getFreeHeap());
}

static void cmdSet(const String &arg) {
  int eq = arg.indexOf('=');
  if (eq <= 0) {
    Serial.println("ERR usage: set <key>=<value>");
    return;
  }
  String key = arg.substring(0, eq);
  String val = arg.substring(eq + 1);
  size_t n = val.length();

  if (key == "ssid") {
    if (n == 0 || n > PROV_SSID_MAX) {
      Serial.println("ERR ssid must be 1-32 chars");
      return;
    }
    val.toCharArray(curSsid, sizeof(curSsid));
    Serial.printf("OK ssid=%s (send save to persist)\n", curSsid);
  } else if (key == "pass") {
    if (n > PROV_PASS_MAX) {
      Serial.println("ERR pass must be 0-63 chars");
      return;
    }
    val.toCharArray(curPass, sizeof(curPass));
    Serial.println("OK pass set (send save to persist)");
  } else if (key == "host") {
    if (n == 0 || n > PROV_HOST_MAX ||
        val.indexOf('/') >= 0 || val.indexOf(':') >= 0 ||
        val.indexOf(' ') >= 0) {
      Serial.println("ERR host: 1-127 chars, no scheme/port/spaces");
      return;
    }
    val.toCharArray(curHost, sizeof(curHost));
    Serial.printf("OK host=%s (send save to persist)\n", curHost);
  } else if (key == "scheme") {
    if (val != "https://" && val != "http://") {
      Serial.println("ERR scheme must be https:// or http://");
      return;
    }
    val.toCharArray(curScheme, sizeof(curScheme));
    Serial.printf("OK scheme=%s (send save to persist)\n", curScheme);
  } else if (key == "port") {
    int v = val.toInt();
    if (v <= 0 || v > 65535 || String(v) != val) {
      Serial.println("ERR port must be 1-65535");
      return;
    }
    snprintf(curPort, sizeof(curPort), "%d", v);
    Serial.printf("OK port=%d (send save to persist)\n", v);
  } else if (key == "agency") {
    bool ok = n >= 1 && n <= PROV_AGENCY_MAX;
    for (size_t i = 0; ok && i < n; i++) {
      char c = val[i];
      ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    }
    if (!ok) {
      Serial.println("ERR agency: 1-8 chars, lowercase a-z0-9");
      return;
    }
    val.toCharArray(curAgency, sizeof(curAgency));
    Serial.printf("OK agency=%s (send save to persist)\n", curAgency);
  } else if (key == "mhost") {
    // Empty clears the broker and turns the HA link off.
    if (n > PROV_HOST_MAX ||
        val.indexOf('/') >= 0 || val.indexOf(':') >= 0 ||
        val.indexOf(' ') >= 0) {
      Serial.println("ERR mhost: 0-127 chars, no scheme/port/spaces");
      return;
    }
    val.toCharArray(curMhost, sizeof(curMhost));
    Serial.printf("OK mhost=%s (send save to persist)\n", curMhost);
  } else if (key == "mport") {
    int v = val.toInt();
    if (val[0] && (v <= 0 || v > 65535 || String(v) != val)) {
      Serial.println("ERR mport must be 1-65535 (empty means 1883)");
      return;
    }
    snprintf(curMport, sizeof(curMport), "%d", v);
    Serial.printf("OK mport=%d (send save to persist)\n", v);
  } else if (key == "muser") {
    if (n > PROV_USER_MAX) {
      Serial.println("ERR muser must be 0-63 chars");
      return;
    }
    val.toCharArray(curMuser, sizeof(curMuser));
    Serial.printf("OK muser=%s (send save to persist)\n", curMuser);
  } else if (key == "mpass") {
    if (n > PROV_PASS_MAX) {
      Serial.println("ERR mpass must be 0-63 chars");
      return;
    }
    val.toCharArray(curMpass, sizeof(curMpass));
    Serial.println("OK mpass set (send save to persist)");
  } else if (key == "bmin" || key == "bmax") {
    const bool isMin = key == "bmin";
    int v = val.toInt();
    if (val[0] && (v < 0 || v > 255 || String(v) != val)) {
      Serial.println("ERR bmin/bmax must be 0-255 (empty means the default)");
      return;
    }
    // Keep the window sane against the other bound as it stands now.
    if (isMin && val[0] && v > (int)bvalOf(curBmax, 255)) {
      Serial.println("ERR bmin must not exceed the current bmax");
      return;
    }
    if (!isMin && val[0] && v < (int)bvalOf(curBmin, 0)) {
      Serial.println("ERR bmax must not be below the current bmin");
      return;
    }
    char *dst = isMin ? curBmin : curBmax;
    size_t cap = isMin ? sizeof(curBmin) : sizeof(curBmax);
    val.toCharArray(dst, cap);
    Serial.printf("OK %s=%s (send save to persist)\n", isMin ? "bmin" : "bmax",
                  dst);
  } else {
    Serial.println("ERR unknown key (try: ssid pass host scheme port agency mhost mport muser mpass bmin bmax)");
  }
}

static void cmdGet(const String &arg) {
  if (arg == "ssid") {
    if (curSsid[0]) Serial.printf("VAL ssid=%s\n", curSsid);
    else Serial.println("VAL ssid EMPTY");
  } else if (arg == "pass") {
    // Never echo the password over the wire — the tool only needs to know
    // it is set and how long it is.
    Serial.printf("VAL pass %s len=%u\n", curPass[0] ? "SET" : "EMPTY",
                  (unsigned)strlen(curPass));
  } else if (arg == "host") {
    Serial.printf("VAL host=%s\n", curHost);
  } else if (arg == "scheme") {
    Serial.printf("VAL scheme=%s\n", curScheme);
  } else if (arg == "port") {
    Serial.printf("VAL port=%u\n", portOf(curPort));
  } else if (arg == "agency") {
    Serial.printf("VAL agency=%s\n", curAgency);
  } else if (arg == "mhost") {
    Serial.printf("VAL mhost=%s\n", curMhost);
  } else if (arg == "mport") {
    Serial.printf("VAL mport=%u\n", portOf(curMport));
  } else if (arg == "muser") {
    Serial.printf("VAL muser=%s\n", curMuser);
  } else if (arg == "mpass") {
    // Same rule as the WiFi password: never echoed back.
    Serial.printf("VAL mpass %s len=%u\n", curMpass[0] ? "SET" : "EMPTY",
                  (unsigned)strlen(curMpass));
  } else if (arg == "bmin") {
    Serial.printf("VAL bmin=%u\n", (unsigned)bvalOf(curBmin, 0));
  } else if (arg == "bmax") {
    Serial.printf("VAL bmax=%u\n", (unsigned)bvalOf(curBmax, 255));
  } else if (arg == "all" || arg.length() == 0) {
    cmdGet("ssid");
    cmdGet("pass");
    cmdGet("host");
    cmdGet("scheme");
    cmdGet("port");
    cmdGet("agency");
    cmdGet("mhost");
    cmdGet("mport");
    cmdGet("muser");
    cmdGet("mpass");
    cmdGet("bmin");
    cmdGet("bmax");
  } else {
    Serial.println("ERR unknown key (try: ssid pass host scheme port agency mhost mport muser mpass bmin bmax all)");
  }
}

static void handleLine(String line) {
  line.trim();
  if (line.length() == 0) {
    return;
  }
  String cmd = line;
  String arg;
  int sp = line.indexOf(' ');
  if (sp >= 0) {
    cmd = line.substring(0, sp);
    arg = line.substring(sp + 1);
  }

  if (cmd == "set") {
    cmdSet(arg);
  } else if (cmd == "get") {
    cmdGet(arg);
  } else if (cmd == "save") {
    prefs.putString("ssid", curSsid);
    prefs.putString("pass", curPass);
    prefs.putString("host", curHost);
    prefs.putString("scheme", curScheme);
    prefs.putString("agency", curAgency);
    prefs.putString("port", curPort);
    prefs.putString("mhost", curMhost);
    prefs.putString("mport", curMport);
    prefs.putString("muser", curMuser);
    prefs.putString("mpass", curMpass);
    prefs.putString("bmin", curBmin);
    prefs.putString("bmax", curBmax);
    storedSsidSet = curSsid[0] != '\0';
    Serial.println("OK saved");
  } else if (cmd == "erase") {
    prefs.clear();
    storedSsidSet = false;
    curSsid[0] = '\0';
    curPass[0] = '\0';
    Serial.println("OK erased (live session keeps its values until reboot)");
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd == "help") {
    printHelp();
  } else if (cmd == "reboot") {
    Serial.println("OK rebooting");
    Serial.flush();
    delay(50);
    esp_restart();
  } else {
    Serial.println("ERR unknown command (try: help)");
  }
}

void provLoad() {
  prefs.begin("otw", false);
  String ssid = prefs.getString("ssid", "");
  storedSsidSet = ssid.length() > 0;
  if (!storedSsidSet) {
    return;
  }
  ssid.toCharArray(curSsid, sizeof(curSsid));
  prefs.getString("pass", "").toCharArray(curPass, sizeof(curPass));
  prefs.getString("host", "").toCharArray(curHost, sizeof(curHost));
  prefs.getString("scheme", "").toCharArray(curScheme, sizeof(curScheme));
  prefs.getString("port", "").toCharArray(curPort, sizeof(curPort));
  prefs.getString("agency", "").toCharArray(curAgency, sizeof(curAgency));
  prefs.getString("mhost", "").toCharArray(curMhost, sizeof(curMhost));
  prefs.getString("mport", "").toCharArray(curMport, sizeof(curMport));
  prefs.getString("muser", "").toCharArray(curMuser, sizeof(curMuser));
  prefs.getString("mpass", "").toCharArray(curMpass, sizeof(curMpass));
  prefs.getString("bmin", "").toCharArray(curBmin, sizeof(curBmin));
  prefs.getString("bmax", "").toCharArray(curBmax, sizeof(curBmax));
}

// Provisioning is complete only with an SSID and a host: the binary has no
// fallback endpoint. (scheme/port empty still means HTTPS/443 at fetch time.)
bool provNeeded() { return curSsid[0] == '\0' || curHost[0] == '\0'; }

void provPumpSerial() {
  if (Serial.available() <= 0) {
    return;
  }
  // The banner is the handshake the browser tool reads first: fw version,
  // then whether NVS already holds an SSID.
  static bool bannerSent = false;
  if (!bannerSent) {
    bannerSent = true;
    Serial.printf("OTW-FW %d\n", OTW_FW_VERSION);
    if (storedSsidSet) {
      printStored();
    } else {
      Serial.println("UNPROVISIONED");
    }
  }

  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) {
      break;
    }
    if (c == '\n') {
      if (partial.length() >= PROV_LINE_MAX) {
        partial = "";
        Serial.println("ERR line too long");
        continue;
      }
      handleLine(partial);
      partial = "";
    } else {
      if (partial.length() >= PROV_LINE_MAX) {
        partial = "";
        // Drain to the end of the oversized line before resuming.
        while (Serial.available() > 0 && Serial.read() != '\n') {
        }
        Serial.println("ERR line too long");
        continue;
      }
      partial += (char)c;  // stray \r is trimmed in handleLine
    }
  }
}

const char *provSsid() { return curSsid; }
const char *provPass() { return curPass; }
// Empty scheme/port still means the protocol default (HTTPS/443) at fetch
// time — but nothing fetches until provNeeded() is false, which requires a
// host. The binary itself carries no endpoint.
const char *provScheme() { return curScheme[0] ? curScheme : "https://"; }
const char *provPollHost() { return curHost; }
uint16_t provPort() {
  uint16_t p = portOf(curPort);
  return p ? p : 443;
}
const char *provAgency() { return curAgency; }
// Optional Home Assistant broker. An empty host turns the HA link off;
// an empty port means the MQTT default (1883) at connect time.
const char *provMqttHost() { return curMhost; }
uint16_t provMqttPort() { return mqttPortOf(curMport); }
const char *provMqttUser() { return curMuser; }
const char *provMqttPass() { return curMpass; }
// Brightness window applied to Home Assistant levels. Unset means the full
// 0-255 range — the mapping is then the identity.
uint8_t provBmin() { return bvalOf(curBmin, 0); }
uint8_t provBmax() { return bvalOf(curBmax, 255); }
