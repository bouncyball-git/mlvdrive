#!/bin/sh
# Builds the probe: build/probe/MLV Drive Probe.app with the FSKit extension inside, and
# build/probe/probe_check, and registers the app with macOS. Signs with $SIGN_IDENTITY (default
# "-": ad hoc). The extension has the app sandbox only: the FSKit entitlement
# (com.apple.developer.fskit.fsmodule) is restricted, so an ad hoc signed binary carrying it is
# refused at launch, and FSKit runs the module without it.
#   macos/probe/build.sh [--bundle-id org.example.mlvprobe]
set -eu
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT="$ROOT/build/probe"
BUNDLE_ID=org.mlvdrive.probe
[ "${1:-}" = "--bundle-id" ] && BUNDLE_ID=$2
IDENTITY=${SIGN_IDENTITY:--}
ARCHS="-arch arm64 -arch x86_64"
FLAGS="-std=c++17 -O2 -mmacosx-version-min=26.0 -Wall -Wextra -Wno-unused-parameter -fobjc-arc"

APP="$OUT/MLV Drive Probe.app"
APPEX="$APP/Contents/Extensions/MLVProbeFS.appex"
# Updated in place (not deleted and made again): a new bundle is a new registration, which FSKit
# treats as a new module, turned off.
mkdir -p "$APP/Contents/MacOS" "$APPEX/Contents/MacOS"

clang++ $ARCHS $FLAGS probe_fs.mm -framework Foundation -framework FSKit -o "$APPEX/Contents/MacOS/MLVProbeFS"
sed "s/@BUNDLE_ID@/$BUNDLE_ID/g" Info.plist.in > "$APPEX/Contents/Info.plist"
clang++ $ARCHS $FLAGS host_main.mm -framework Foundation -o "$APP/Contents/MacOS/MLVProbe"
sed "s/@BUNDLE_ID@/$BUNDLE_ID/g" HostInfo.plist.in > "$APP/Contents/Info.plist"
clang++ -std=c++17 -O2 probe_check.cpp -o "$OUT/probe_check"

# Inside out: the extension (sandboxed, with the FSKit entitlement), then the app around it.
codesign --force --sign "$IDENTITY" --options runtime --entitlements fs.entitlements "$APPEX"
codesign --force --sign "$IDENTITY" --options runtime "$APP"
codesign --verify --deep --strict "$APP"
# Registered with macOS (as opening the app once does), so FSKit finds the module.
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f -R "$APP"
echo "built $APP ($( [ "$IDENTITY" = "-" ] && echo "ad hoc" || echo "$IDENTITY" ))"
