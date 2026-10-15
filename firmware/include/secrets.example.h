#pragma once
// Copy this file to firmware/include/secrets.h (gitignored) and fill it in.

#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// Hostname or IP of the machine running server/proxy.py (no scheme, no slash).
// LAN proxy: use its address with PROXY_PORT 8000 (firmware/include/config.h).
// Hosted on DigitalOcean App Platform: use the app domain (e.g.
// "open-transit-wall-nuvez.ondigitalocean.app") and set PROXY_PORT 80 in
// config.h. Turn off the app's HTTP->HTTPS redirect first — this firmware
// speaks plain HTTP and does not follow redirects.
#define PROXY_HOST    "192.168.1.50"
