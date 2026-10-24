// Minimal mDNS client for broker discovery (see ha_discover.h).
//
// One PTR query per service, parse answers for PTR (instance) and SRV
// (port). Records are only accepted when their name matches the queried
// service, so unrelated announcements on the wire cannot contaminate
// the result. The host to dial is the source address of the matching
// response: the machine answering for _mqtt._tcp or _home-assistant._tcp
// is the one running the broker, and an IP needs no second resolution
// step (SRV target names are often .local names a constrained client
// cannot resolve). Name compression (RFC 1035 pointers) is handled
// recursively with a depth cap.

#include "ha_discover.h"

#include <Arduino.h>
#include <WiFiUdp.h>
#include <string.h>

#define D_PORT    5353
#define D_QUERY_MS  2500  // listen window per query
#define D_BUF       1500  // one mDNS response

static WiFiUDP dUdp;

struct Found {
  char src[40];  // dotted-quad source of the matching response
  char inst[128];
  uint16_t port;
};

static bool endsWithCi(const char *name, const char *suffix) {
  size_t n = strlen(name), s = strlen(suffix);
  if (n < s) {
    return false;
  }
  return strcasecmp(name + (n - s), suffix) == 0;
}

// Write a dotted name as DNS labels into buf at *n.
static void putName(uint8_t *buf, size_t *n, const char *name) {
  const char *p = name;
  while (*p) {
    const char *dot = strchr(p, '.');
    size_t l = dot ? (size_t)(dot - p) : strlen(p);
    if (l) {
      buf[(*n)++] = (uint8_t)l;
      memcpy(buf + *n, p, l);
      *n += l;
    }
    if (!dot) {
      break;
    }
    p = dot + 1;
  }
  buf[(*n)++] = 0;
}

// Read a (possibly compressed) name starting at *off into out.
static bool parseName(const uint8_t *d, size_t len, size_t *off, char *out,
                      size_t outLen, int depth) {
  size_t p = *off;
  size_t n = 0;
  bool first = true;
  out[0] = '\0';
  while (p < len) {
    uint8_t l = d[p];
    if (l == 0) {
      p++;
      break;
    }
    if ((l & 0xC0) == 0xC0) {
      if (depth >= 4) {
        return false;
      }
      size_t po = ((size_t)(l & 0x3F) << 8) | d[p + 1];
      char sub[192];
      if (!parseName(d, len, &po, sub, sizeof(sub), depth + 1)) {
        return false;
      }
      if (n + strlen(sub) + 2 > outLen) {
        return false;
      }
      if (!first) {
        out[n++] = '.';
      }
      memcpy(out + n, sub, strlen(sub));
      n += strlen(sub);
      out[n] = '\0';
      p += 2;
      break;  // a pointer ends the name
    }
    if (l > 63 || p + 1 + l > len || n + l + 2 > outLen) {
      return false;
    }
    if (!first) {
      out[n++] = '.';
    }
    memcpy(out + n, d + p + 1, l);
    n += l;
    out[n] = '\0';
    first = false;
    p += 1 + l;
  }
  *off = p;
  return true;
}

static void sendQuery(const char *name, uint16_t qtype) {
  uint8_t q[160];
  memset(q, 0, 12);  // id 0, flags 0, rest 0
  q[5] = 1;          // QDCOUNT 1
  size_t n = 12;
  putName(q, &n, name);
  q[n++] = (uint8_t)(qtype >> 8);
  q[n++] = (uint8_t)qtype;
  q[n++] = 0;
  q[n++] = 1;  // class IN
  dUdp.beginPacket(IPAddress(224, 0, 0, 251), D_PORT);
  dUdp.write(q, n);
  dUdp.endPacket();
}

// Consume responses until the deadline, retransmitting the query once at
// the midpoint (multicast answers are occasionally lost). Records are
// accepted only when their name matches the queried service.
static void listenUntil(uint32_t deadline, const char *qname,
                       uint16_t qtype, const char *svc, Found *f) {
  static uint8_t buf[D_BUF];
  uint32_t mid = deadline - D_QUERY_MS / 2;
  bool resent = false;
  while ((int32_t)(millis() - deadline) < 0) {
    if (!resent && (int32_t)(millis() - mid) >= 0) {
      resent = true;
      sendQuery(qname, qtype);
    }
    int sz = dUdp.parsePacket();
    if (sz <= 0) {
      delay(20);
      continue;
    }
    if (sz > D_BUF) {
      sz = D_BUF;
    }
    int rd = dUdp.read(buf, sz);
    if (rd < 12) {
      continue;
    }
    uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
    uint32_t rr = (uint32_t)((buf[6] << 8) | buf[7]) +
                  (uint32_t)((buf[8] << 8) | buf[9]) +
                  (uint32_t)((buf[10] << 8) | buf[11]);
    size_t off = 12;
    char name[256];
    for (uint16_t i = 0; i < qd && off < (size_t)rd; i++) {
      if (!parseName(buf, (size_t)rd, &off, name, sizeof(name), 0)) {
        break;
      }
      off += 4;
    }
    for (uint32_t i = 0; i < rr && off + 12 <= (size_t)rd; i++) {
      if (!parseName(buf, (size_t)rd, &off, name, sizeof(name), 0)) {
        break;
      }
      uint16_t type = (uint16_t)((buf[off] << 8) | buf[off + 1]);
      uint16_t rdlen = (uint16_t)((buf[off + 8] << 8) | buf[off + 9]);
      size_t rdata = off + 10;
      off = rdata + rdlen;
      if (type == 12 && !f->inst[0] && strcasecmp(name, svc) == 0) {
        // PTR naming an instance of the service we queried
        size_t po = rdata;
        if (parseName(buf, (size_t)rd, &po, f->inst, sizeof(f->inst), 0)) {
          snprintf(f->src, sizeof(f->src), "%s",
                   dUdp.remoteIP().toString().c_str());
        }
      } else if (type == 33 && rdlen >= 7 && endsWithCi(name, svc)) {
        // SRV for an instance of the service; the port is what we need
        f->port = (uint16_t)((buf[rdata + 4] << 8) | buf[rdata + 5]);
        if (!f->src[0]) {
          snprintf(f->src, sizeof(f->src), "%s",
                   dUdp.remoteIP().toString().c_str());
        }
      }
    }
  }
}

// Query one service; on return f->src is set if anything answered.
static void tryService(const char *svc, Found *f) {
  sendQuery(svc, 12);  // PTR
  listenUntil(millis() + D_QUERY_MS, svc, 12, svc, f);
  if (f->inst[0] && !f->port) {
    char qname[224];
    snprintf(qname, sizeof(qname), "%s.%s", f->inst, svc);
    sendQuery(qname, 33);  // direct SRV query for the port
    listenUntil(millis() + D_QUERY_MS, qname, 33, svc, f);
  }
}

bool haDiscover(char *host, size_t hostLen, uint16_t *port) {
  if (!dUdp.beginMulticast(IPAddress(224, 0, 0, 251), D_PORT)) {
    return false;
  }
  Found f = {};
  // A broker that advertises itself wins: its SRV port is the real MQTT
  // port. Otherwise fall back to HA's own advertisement; the add-on
  // broker shares HA's host, and its SRV port is the web UI, not MQTT.
  tryService("_mqtt._tcp.local", &f);
  bool viaBroker = f.src[0] != '\0';
  if (!viaBroker) {
    f = Found{};
    tryService("_home-assistant._tcp.local", &f);
  }
  dUdp.stop();
  if (!f.src[0]) {
    return false;
  }
  snprintf(host, hostLen, "%s", f.src);
  *port = viaBroker && f.port ? f.port : 1883;
  return true;
}
