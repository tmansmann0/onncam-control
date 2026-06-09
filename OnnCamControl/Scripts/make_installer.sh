#!/usr/bin/env bash
# Build a distributable DMG installer for OnnCam Control, or install the app
# straight into /Applications.
#
# Usage:
#   ./Scripts/make_installer.sh             # build dist/OnnCam-Control-<version>.dmg
#   ./Scripts/make_installer.sh --install   # also copy the app into /Applications and launch it
#   ARCHES="arm64 x86_64" ./Scripts/make_installer.sh   # universal binary
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

if [[ -f "$ROOT/version.env" ]]; then
  source "$ROOT/version.env"
fi
APP_NAME=${APP_NAME:-MyApp}
MARKETING_VERSION=${MARKETING_VERSION:-0.0.0}

INSTALL=0
for arg in "$@"; do
  case "$arg" in
    --install) INSTALL=1 ;;
    --help|-h)
      sed -n '2,9p' "$0"
      exit 0
      ;;
  esac
done

APP_BUNDLE="$ROOT/${APP_NAME}.app"
DIST_DIR="$ROOT/dist"
DMG_NAME="${APP_NAME// /-}-${MARKETING_VERSION}.dmg"
DMG_PATH="$DIST_DIR/$DMG_NAME"
VOLUME_NAME="${APP_NAME} ${MARKETING_VERSION}"

echo "==> Building ${APP_NAME}.app (release)"
SIGNING_MODE=${SIGNING_MODE:-adhoc} "$ROOT/Scripts/package_app.sh" release

echo "==> Staging DMG contents"
STAGING=$(mktemp -d /tmp/onncam-dmg.XXXXXX)
trap 'rm -rf "$STAGING"' EXIT
cp -R "$APP_BUNDLE" "$STAGING/"
ln -s /Applications "$STAGING/Applications"

echo "==> Creating $DMG_PATH"
mkdir -p "$DIST_DIR"
rm -f "$DMG_PATH"
hdiutil create \
  -volname "$VOLUME_NAME" \
  -srcfolder "$STAGING" \
  -ov \
  -format UDZO \
  "$DMG_PATH" >/dev/null

echo "OK: $DMG_PATH"

if [[ "$INSTALL" == "1" ]]; then
  echo "==> Installing to /Applications"
  # Quit the running app (whether launched from the repo or /Applications).
  pkill -f "${APP_NAME}.app/Contents/MacOS/${APP_NAME}" 2>/dev/null || true
  sleep 0.5
  rm -rf "/Applications/${APP_NAME}.app"
  cp -R "$APP_BUNDLE" "/Applications/${APP_NAME}.app"
  echo "==> Launching installed app"
  open "/Applications/${APP_NAME}.app"
  echo "OK: installed /Applications/${APP_NAME}.app"
fi
