#!/usr/bin/env bash
# One release of the bridge and the hosted app from the same Control commit.
#
#   tools/release.sh <version> [--yes]
#
# Prerequisites: CHANGELOG.md already has a "## v<version>" section and is committed on master;
# Control is on master, clean and equal to origin/master. The steps:
#   1. tag, bundle the Control build (sync-web.sh), build the firmware
#   2. copy the images, bump manifest.json, record the Control commit in docs/firmware/build-info.txt, commit
#   3. ask, then push master + tag and create the GitHub release
#   4. ask, then back up and replace the hosted app (only if HOSTING_* is configured)
#   5. run check-sync.sh
# Each outward step shows what it will do and waits for "yes" (or --yes). Beta is never touched.
set -euo pipefail
. "$(dirname "$0")/release-common.sh"

VERSION="${1:-}"; VERSION="${VERSION#v}"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "Usage: tools/release.sh <x.y.z> [--yes]"
ASSUME_YES=no; [ "${2:-}" = "--yes" ] && ASSUME_YES=yes
TAG="v$VERSION"
cd "$BRIDGE_DIR"

confirm() {
    [ "$ASSUME_YES" = yes ] && return 0
    read -r -p "$1 Type yes to continue: " answer
    [ "$answer" = yes ] || die "Stopped."
}

# --- Preconditions ---
[ -n "${IDF_EXPORT:-}" ] && [ -f "$IDF_EXPORT" ] || die "IDF_EXPORT is not set or wrong."
git fetch -q origin
[ "$(git branch --show-current)" = master ] || die "Bridge is not on master."
[ -z "$(git status --porcelain)" ] || die "Bridge has uncommitted changes."
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/master)" ] || die "Bridge master differs from origin/master."
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && die "Tag $TAG already exists locally."
[ -z "$(git ls-remote --tags origin "refs/tags/$TAG")" ] || die "Tag $TAG already exists on origin."
grep -q "^## $TAG " CHANGELOG.md || die "CHANGELOG.md has no '## $TAG' section."
require_control_ready
CONTROL_COMMIT=$(git -C "$CONTROL_DIR" rev-parse HEAD)
echo "Releasing $TAG from Control $(git -C "$CONTROL_DIR" log --oneline -1)"

# --- 1. Tag, bundle, build ---
git tag "$TAG"
trap 'echo "Build failed - removing the local tag $TAG"; git tag -d "$TAG" >/dev/null' ERR
tools/sync-web.sh "$CONTROL_DIR" >/dev/null
(. "$IDF_EXPORT" >/dev/null 2>&1; idf.py build >/dev/null)
strings -a build/marstek-ble-bridge.bin | grep -qx "$TAG" || die "Firmware image does not carry version $TAG."
trap - ERR

# --- 2. Images, manifest, build info ---
cp build/marstek-ble-bridge.bin build/web.bin docs/firmware/
sed -i.bak -E "s/\"version\": \"[0-9.]+\"/\"version\": \"$VERSION\"/" docs/manifest.json && rm docs/manifest.json.bak
printf 'version=%s\ncontrol_commit=%s\n' "$TAG" "$CONTROL_COMMIT" > docs/firmware/build-info.txt
git add docs
git commit -q -m "Refresh the installer images for $TAG"

# --- 3. Publish ---
confirm "About to push master and $TAG and create the GitHub release $TAG (bridge + web image)."
git push origin master
git push origin "$TAG"
notes=$(mktemp)
{
    echo "**[Flash it from your browser](https://sphings79.github.io/marstek-ble-bridge/)** — nothing to install."
    echo
    awk -v t="## $TAG " 'index($0,t)==1{f=1;next} /^## v/{f=0} f' CHANGELOG.md
} > "$notes"
gh release create "$TAG" build/marstek-ble-bridge.bin build/web.bin --title "$TAG" --notes-file "$notes"
rm -f "$notes"

# --- 4. Hosted app ---
if [ -n "${HOSTING_SSH:-}" ]; then
    : "${HOSTING_WEBROOT:?}" "${HOSTING_OWNER:?}" "${HOSTING_BACKUP_DIR:?}"
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    build_hosted "$tmp/out"
    echo "Hosted app now serves: $(curl -4 -sL --max-time 20 "${HOSTING_URL:-}" | bundle_name || echo '?')"
    echo "New bundle:            $(bundle_name < "$tmp/out/index.html")"
    confirm "About to back up $HOSTING_WEBROOT/control on $HOSTING_SSH and replace it. control-beta is not touched."
    ssh -4 "$HOSTING_SSH" "tar -czf '$HOSTING_BACKUP_DIR/marstek-control-\$(date +%Y%m%d-%H%M).tar.gz' -C '$HOSTING_WEBROOT' control && rm -rf '$HOSTING_WEBROOT/control.new' && mkdir '$HOSTING_WEBROOT/control.new'"
    tar -C "$tmp/out" -cf - . | ssh -4 "$HOSTING_SSH" "tar -C '$HOSTING_WEBROOT/control.new' -xf -"
    ssh -4 "$HOSTING_SSH" "chown -R '$HOSTING_OWNER' '$HOSTING_WEBROOT/control.new' && mv '$HOSTING_WEBROOT/control' '$HOSTING_WEBROOT/control.old' && mv '$HOSTING_WEBROOT/control.new' '$HOSTING_WEBROOT/control' && rm -rf '$HOSTING_WEBROOT/control.old'"
else
    echo "HOSTING_SSH not set: hosted app NOT updated. Upload it by hand, then run tools/check-sync.sh."
fi

# --- 5. Check ---
tools/check-sync.sh
