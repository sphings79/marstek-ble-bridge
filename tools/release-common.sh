#!/usr/bin/env bash
# Shared helpers for release.sh and check-sync.sh. Source it, do not run it.
#
# Settings come from the environment or from tools/release.local.env (git-ignored):
#   CONTROL_DIR          checkout of marstek-ble-control (required)
#   IDF_EXPORT           path to ESP-IDF's export.sh (required for release.sh)
#   HOSTING_SSH          user@host of the web server that hosts the app (optional)
#   HOSTING_WEBROOT      directory holding control/ and control-beta/ on that server
#   HOSTING_OWNER        owner of the files on the server (user:group)
#   HOSTING_BACKUP_DIR   where the server-side backups of control/ go
#   HOSTING_URL          public URL of the hosted app, for example https://example.org/marstek/control/
#   HOSTING_BASE         URL path the app is served from, for example /marstek/control/

BRIDGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$BRIDGE_DIR/tools/release.local.env" ] && . "$BRIDGE_DIR/tools/release.local.env"

die() { echo "ERROR: $*" >&2; exit 1; }

# Print the commit of the Control checkout and whether the tree is clean and equals origin/master.
require_control_ready() {
    [ -n "${CONTROL_DIR:-}" ] || die "CONTROL_DIR is not set (checkout of marstek-ble-control)."
    [ -f "$CONTROL_DIR/package.json" ] || die "No app checkout at $CONTROL_DIR."
    git -C "$CONTROL_DIR" fetch -q origin
    [ "$(git -C "$CONTROL_DIR" branch --show-current)" = master ] || die "Control is not on master."
    [ -z "$(git -C "$CONTROL_DIR" status --porcelain)" ] || die "Control has uncommitted changes."
    [ "$(git -C "$CONTROL_DIR" rev-parse HEAD)" = "$(git -C "$CONTROL_DIR" rev-parse origin/master)" ] \
        || die "Control master differs from origin/master (push or pull first)."
}

# Build the hosted variant of the app into $1 (served from $HOSTING_BASE).
build_hosted() {
    [ -n "${HOSTING_BASE:-}" ] || die "HOSTING_BASE is not set."
    (cd "$CONTROL_DIR" && npx vite build --base="$HOSTING_BASE" --outDir "$1" --emptyOutDir >/dev/null 2>&1)
}

# Name of the main JS bundle in an index.html on stdin.
bundle_name() { grep -oE 'assets/index-[^"]+\.js' | head -1; }
