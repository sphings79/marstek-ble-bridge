#!/usr/bin/env bash
# Compare the three places the app lives in and fail if they are not the same state:
#   1. marstek-ble-control master (origin)
#   2. the Control commit built into the latest bridge release (docs/firmware/build-info.txt on origin)
#   3. the bundle the hosted app serves right now (needs HOSTING_URL; skipped without it)
# The beta build is deliberately not compared. Read-only: nothing is changed anywhere.
set -euo pipefail
. "$(dirname "$0")/release-common.sh"

[ -n "${CONTROL_DIR:-}" ] && [ -f "$CONTROL_DIR/package.json" ] || die "CONTROL_DIR is not set or wrong."
git -C "$CONTROL_DIR" fetch -q origin
git -C "$BRIDGE_DIR" fetch -q origin

control_head=$(git -C "$CONTROL_DIR" rev-parse origin/master)
bridge_stamp=$(git -C "$BRIDGE_DIR" show origin/master:docs/firmware/build-info.txt 2>/dev/null \
    | sed -n 's/^control_commit=//p') || true
bridge_version=$(git -C "$BRIDGE_DIR" show origin/master:docs/manifest.json | sed -n 's/.*"version": "\(.*\)".*/\1/p' | head -1)
released=$(gh release view --repo "$(git -C "$BRIDGE_DIR" remote get-url origin | sed 's#.*github.com[:/]##; s#\.git$##')" \
    --json tagName -q .tagName 2>/dev/null || echo "?")

status=0
row() { printf '%-34s %s\n' "$1" "$2"; }

row "Control origin/master" "${control_head:0:10}"
row "Bridge installer (manifest)" "v$bridge_version"
row "Latest bridge release" "$released"
row "Control commit in bridge images" "${bridge_stamp:0:10}"

[ "$released" = "v$bridge_version" ] || { echo "MISMATCH: latest release $released is not the manifest version v$bridge_version"; status=1; }
if [ -z "$bridge_stamp" ]; then
    echo "MISMATCH: no control_commit in docs/firmware/build-info.txt"; status=1
elif [ "$bridge_stamp" != "$control_head" ]; then
    echo "MISMATCH: bridge images were built from an older Control commit; Control has newer commits:"
    git -C "$CONTROL_DIR" log --oneline "$bridge_stamp..$control_head" 2>/dev/null | sed 's/^/  /' || true
    status=1
fi

if [ -n "${HOSTING_URL:-}" ]; then
    live=$(curl -4 -sL --max-time 20 "$HOSTING_URL" | bundle_name || true)
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    git -C "$CONTROL_DIR" diff --quiet HEAD "origin/master" 2>/dev/null \
        || die "Control checkout is not at origin/master; the hosted comparison would be wrong."
    build_hosted "$tmp/out"
    expected=$(bundle_name < "$tmp/out/index.html")
    row "Hosted app serves" "${live:-?}"
    row "Hosted app should serve" "$expected"
    [ "$live" = "$expected" ] || { echo "MISMATCH: hosted app is not the Control master build"; status=1; }
else
    echo "(HOSTING_URL not set: hosted app not checked)"
fi

[ "$status" -eq 0 ] && echo "OK: Control, Bridge and hosting are the same state." || echo "NOT IN SYNC."
exit "$status"
