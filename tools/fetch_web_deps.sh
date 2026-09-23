#!/usr/bin/env bash
# Fetch the pinned browser bundle the flasher imports. Vendored rather than
# loaded from a CDN so the deployed page has no third-party runtime dependency.
#
#   tools/fetch_web_deps.sh          # into web/vendor/
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/web/vendor/esptool-js.js"

ESPTOOL_JS_VERSION="0.7.0"
ESPTOOL_JS_SHA256="00ebe6fb0b202976d3e2a3c6b9a6bd438c26a81d2af6739c95ac276a64c1a84e"
URL="https://unpkg.com/esptool-js@${ESPTOOL_JS_VERSION}/bundle.js"

mkdir -p "$(dirname "$DEST")"

if [[ -f "$DEST" ]]; then
  have="$(sha256sum "$DEST" 2>/dev/null | cut -d' ' -f1 || true)"
  if [[ "$have" == "$ESPTOOL_JS_SHA256" ]]; then
    echo "esptool-js ${ESPTOOL_JS_VERSION} already vendored"
    exit 0
  fi
fi

echo "Fetching esptool-js ${ESPTOOL_JS_VERSION}"
if command -v curl >/dev/null 2>&1; then
  curl -fsSL "$URL" -o "$DEST"
else
  wget -qO "$DEST" "$URL"
fi

have="$(sha256sum "$DEST" | cut -d' ' -f1)"
if [[ "$have" != "$ESPTOOL_JS_SHA256" ]]; then
  echo "error: esptool-js checksum mismatch" >&2
  echo "  expected $ESPTOOL_JS_SHA256" >&2
  echo "  got      $have" >&2
  rm -f "$DEST"
  exit 1
fi
echo "Wrote $DEST"