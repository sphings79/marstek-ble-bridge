#!/usr/bin/env bash
# Rebuild the web app for the bridge and drop it into web/, which becomes the web partition.
#
# The bridge serves the app from its own root, so it needs a base of "/" - the hosted deployment
# uses /marstek/control/. Only index.html differs between the two; the JS bundle is byte-identical.
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
touch "$BRIDGE_DIR/web/.gitkeep"

echo "web/ now holds:"
find "$BRIDGE_DIR/web" -type f -not -name .gitkeep -print0 |
    while IFS= read -r -d "" f; do
        printf '  %s (%s)\n' "${f#"$BRIDGE_DIR/web/"}" "$(du -h "$f" | cut -f1)"
    done
