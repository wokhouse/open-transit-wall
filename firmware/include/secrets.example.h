#pragma once
// Copy this file to firmware/include/secrets.h (gitignored) and fill it in.

#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// Hostname or IP of the proxy (no scheme, no slash).
// Default target: the hosted app on DigitalOcean App Platform. config.h
// already defaults to PROXY_SCHEME "https://" + PROXY_PORT 443, and the
// firmware verifies the app's TLS chain against the roots pinned in
// include/proxy_ca.h. For LAN dev against server/proxy.py, put its address
// here and switch PROXY_SCHEME/PROXY_PORT in config.h to http/8000.
#define PROXY_HOST    "open-transit-wall-nuvez.ondigitalocean.app"
