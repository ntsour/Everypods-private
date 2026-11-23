#!/bin/zsh
# Builds and signs EveryPods.app. IOBluetooth only delivers its async callbacks to a signed app
# bundle, and the deployment target must be older than the OS or `open` fails with -10825.
#
# Usage: ./build.sh            (signs with the first "Apple Development" identity found)
#        SIGN_IDENTITY="Developer ID Application: ..." ./build.sh
set -euo pipefail
cd "$(dirname "$0")"

IDENTITY="${SIGN_IDENTITY:-$(security find-identity -v -p codesigning | awk '/Apple Development/ {print $2; exit}')}"
[[ -n "$IDENTITY" ]] || { echo "No signing identity found. Set SIGN_IDENTITY." >&2; exit 1; }

APP=build/EveryPods.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp Info.plist "$APP/Contents/Info.plist"
swiftc -O -target arm64-apple-macos13.0 \
  -framework AppKit -framework IOBluetooth -framework Foundation \
  Sources/*.swift -o "$APP/Contents/MacOS/EveryPodsMac"
codesign --force --sign "$IDENTITY" "$APP"
echo "Built $APP"
echo "Run with: open $APP    (log: ~/Library/Logs/EveryPodsMac.log)"
