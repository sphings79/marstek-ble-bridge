#!/usr/bin/env bash
# Rebuild the web app for the bridge and drop it into web/, which becomes the web partition.
#
# Two differences from the hosted build, both to fit the flash budget that two OTA slots leave:
#
#   - Everything is stored gzipped. The server hands the compressed file straight to the browser,
#     so the ESP32 never compresses anything itself - it just serves ~200 KB instead of ~650 KB,
#     which is also noticeably quicker over WiFi.
#   - webbluetooth.png is dropped. It explains to visitors that their browser lacks Web Bluetooth,
#     which cannot happen here: in bridge mode the browser talks to us over the network.
#
# The base path is "/" because the bridge serves from its own root; the hosted deployment uses
# /marstek/control/. Only index.html differs between the two, the JS bundle is identical.
set -euo pipefail

APP_DIR="${1:-../marstek-ble-control}"
BRIDGE_DIR="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -f "$APP_DIR/package.json" ]; then
    echo "No app checkout at $APP_DIR - pass its path as the first argument." >&2
    exit 1
fi

echo "Building the app from $APP_DIR"
(cd "$APP_DIR" && npx vite build --base=/ --outDir dist-bridge >/dev/null)

rm -rf "$BRIDGE_DIR/web"
mkdir -p "$BRIDGE_DIR/web"
cp -r "$APP_DIR/dist-bridge/." "$BRIDGE_DIR/web/"
rm -rf "$APP_DIR/dist-bridge"

rm -f "$BRIDGE_DIR/web/webbluetooth.png"
find "$BRIDGE_DIR/web" -type f \( -name '*.js' -o -name '*.html' -o -name '*.css' -o -name '*.svg' \) \
    -exec gzip -9 {} +

touch "$BRIDGE_DIR/web/.gitkeep"

echo "web/ now holds:"
total=0
while IFS= read -r -d "" f; do
    size=$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f")
    total=$((total + size))
    printf '  %-40s %8d bytes\n' "${f#"$BRIDGE_DIR/web/"}" "$size"
done < <(find "$BRIDGE_DIR/web" -type f -not -name .gitkeep -print0)
printf '  %-40s %8d bytes\n' "TOTAL" "$total"
