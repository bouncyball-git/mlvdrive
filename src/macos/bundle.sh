#!/bin/sh
# Assembles "MLV Drive.app" in the build folder from the built programs, signs it and registers it
# with macOS (CMake runs this after building; by hand: src/macos/bundle.sh <build dir> <version>).
#
# The bundle is updated in place, never deleted and made again: a new bundle is a new registration,
# which FSKit treats as a new module, turned off in System Settings.
#
# Signing: $SIGN_IDENTITY, default "-" (ad hoc). The extension gets the app sandbox and a read-only
# file exception (it reads the MLV folders and the settings file); not the FSKit entitlement
# (com.apple.developer.fskit.fsmodule): it is restricted, an ad hoc signed binary carrying it is
# refused at launch, and FSKit runs the module without it.
set -eu
SRC=$(cd "$(dirname "$0")" && pwd)
BUILD=$1
VERSION=$2
IDENTITY=${SIGN_IDENTITY:--}
APP="$BUILD/MLV Drive.app"
APPEX="$APP/Contents/Extensions/MLVDriveFS.appex"

mkdir -p "$APP/Contents/MacOS" "$APPEX/Contents/MacOS" "$APPEX/Contents/Resources"
cp "$BUILD/MLV Drive" "$APP/Contents/MacOS/MLV Drive"
cp "$BUILD/MLVDriveFS" "$APPEX/Contents/MacOS/MLVDriveFS"
sed "s/@VERSION@/$VERSION/g" "$SRC/Info-app.plist.in" > "$APP/Contents/Info.plist"
sed "s/@VERSION@/$VERSION/g" "$SRC/Info-fs.plist.in" > "$APPEX/Contents/Info.plist"
# The focus pixel maps, where the module looks beside itself (Resources/pixelmaps).
rm -rf "$APPEX/Contents/Resources/pixelmaps"
cp -R "$SRC/../../third_party/mlvapp/pixelmaps" "$APPEX/Contents/Resources/pixelmaps"

# Inside out: the extension, then the app around it.
codesign --force --sign "$IDENTITY" --options runtime --entitlements "$SRC/fs.entitlements" "$APPEX"
codesign --force --sign "$IDENTITY" --options runtime "$APP"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f -R "$APP"
