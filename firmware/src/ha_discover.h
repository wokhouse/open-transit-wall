// Find the Home Assistant host on the LAN without being told it.
//
// HA Core advertises itself via mDNS as _home-assistant._tcp. The
// Mosquitto add-on runs on that same host, so the machine answering for
// HA's name is where MQTT/1883 lives. A broker that advertises
// _mqtt._tcp itself is tried first and wins, keeping standalone brokers
// that do advertise working (their SRV port is used).
//
// This is a minimal RFC 6762 client: one PTR query per service (sent
// twice), then parse the answers for a PTR (instance name) and an SRV
// (port). The address dialed is the source of the matching response, so
// no .local name resolution is needed afterwards. Blocking with a
// timeout; callers run it only when the link is down.
#ifndef HA_DISCOVER_H
#define HA_DISCOVER_H

#include <stdint.h>
#include <stddef.h>

// Fills host (dotted-quad, NUL-terminated, at least hostLen bytes) and
// port with the MQTT endpoint found on the LAN. True on success. Blocks
// up to ~5 s.
bool haDiscover(char *host, size_t hostLen, uint16_t *port);

#endif
