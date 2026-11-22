#!/bin/sh
# Assembles "MLV Drive.app" in the build folder from the built programs, signs it and registers it
# with macOS (CMake runs this after building; by hand: src/macos/bundle.sh <build dir> <version>).
# --release (tools/package-macos.sh): no registering and no stopping the module running here: a
# second copy of the app registered beside the development one would compete for the module.
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
RELEASE=${3:-}
IDENTITY=${SIGN_IDENTITY:--}
APP="$BUILD/MLV Drive.app"
APPEX="$APP/Contents/Extensions/MLVDriveFS.appex"

# Copied to a new file, then renamed over the old one: FSKit keeps the extension running after an
# unmount, and a binary overwritten in place under a running process is killed (its pages no
# longer match its signature).
install_file() {
    cp "$1" "$2.new"
    mv -f "$2.new" "$2"
}

mkdir -p "$APP/Contents/MacOS" "$APPEX/Contents/MacOS" "$APPEX/Contents/Resources"
install_file "$BUILD/MLV Drive" "$APP/Contents/MacOS/MLV Drive"
install_file "$BUILD/MLVDriveFS" "$APPEX/Contents/MacOS/MLVDriveFS"
install_file "$BUILD/mlvdrive" "$APP/Contents/MacOS/mlvdrive"
sed "s/@VERSION@/$VERSION/g" "$SRC/Info-app.plist.in" > "$APP/Contents/Info.plist"
sed "s/@VERSION@/$VERSION/g" "$SRC/Info-fs.plist.in" > "$APPEX/Contents/Info.plist"
# The icon (tools/make_icons.py draws it): the app's, and the module's (System Settings' list).
mkdir -p "$APP/Contents/Resources"
cp "$SRC/../../assets/mlvdrive.icns" "$APP/Contents/Resources/mlvdrive.icns"
cp "$SRC/../../assets/mlvdrive.icns" "$APPEX/Contents/Resources/mlvdrive.icns"
# No pixel maps are bundled (users add their own: the README); a build before that had them here.
rm -rf "$APPEX/Contents/Resources/pixelmaps"

# Inside out: the extension, then the app around it. Ad hoc, each gets a designated requirement of
# its bundle identifier: macOS's privacy permissions (Full Disk Access, given to the app) are kept
# by that requirement, and an ad hoc signature's own (its exact hash) changes with every build.
if [ "$IDENTITY" = "-" ]; then
    codesign --force --sign - --options runtime --identifier org.mlvdrive.mlvdrive \
        -r='designated => identifier "org.mlvdrive.mlvdrive"' "$APP/Contents/MacOS/mlvdrive"
    codesign --force --sign - --options runtime --entitlements "$SRC/fs.entitlements" \
        -r='designated => identifier "org.mlvdrive.MLVDrive.fs"' "$APPEX"
    codesign --force --sign - --options runtime -r='designated => identifier "org.mlvdrive.MLVDrive"' "$APP"
else
    codesign --force --sign "$IDENTITY" --options runtime --identifier org.mlvdrive.mlvdrive "$APP/Contents/MacOS/mlvdrive"
    codesign --force --sign "$IDENTITY" --options runtime --entitlements "$SRC/fs.entitlements" "$APPEX"
    codesign --force --sign "$IDENTITY" --options runtime "$APP"
fi
[ "$RELEASE" = "--release" ] && exit 0
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f -R "$APP"

# FSKit keeps the module running after the last unmount, and the next mount would use that old
# code: stopped here when nothing of ours is mounted (otherwise it serves until then).
if ! mount | grep -q '(mlvdrive'; then
    pkill -x MLVDriveFS 2>/dev/null && sleep 2 || true  # (FSKit takes a moment to notice)
else
    echo "bundle.sh: an MLV Drive volume is mounted: the module running keeps the old code until all are unmounted and it is stopped (pkill -x MLVDriveFS)"
fi
