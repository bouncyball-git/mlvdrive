#!/bin/sh
# Makes the macOS release: dist/MLVDrive-<version>-macos.dmg, with MLV Drive.app (universal: Apple
# Silicon and Intel; ad hoc signed, or $SIGN_IDENTITY), an Applications shortcut and Install.txt.
#
#   tools/package-macos.sh
#
# Built in build-release (not build: the development app there stays the one registered here).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^project(MLVDrive VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
BUILD="$ROOT/build-release"
DIST="$ROOT/dist"
STAGE="$BUILD/dmg"
DMG="$DIST/MLVDrive-$VERSION-macos.dmg"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DMLVDRIVE_RELEASE_BUNDLE=ON > /dev/null
cmake --build "$BUILD"
APP="$BUILD/MLV Drive.app"
codesign --verify --deep --strict "$APP"
for bin in "MLV Drive" mlvdrive; do lipo "$APP/Contents/MacOS/$bin" -verify_arch arm64 x86_64; done
lipo "$APP/Contents/Extensions/MLVDriveFS.appex/Contents/MacOS/MLVDriveFS" -verify_arch arm64 x86_64

rm -rf "$STAGE"
mkdir -p "$STAGE" "$DIST"
ditto "$APP" "$STAGE/MLV Drive.app"
ln -s /Applications "$STAGE/Applications"
cat > "$STAGE/Install.txt" <<TXT
MLV Drive $VERSION for macOS 26 or later (Apple Silicon and Intel)

MLV Drive shows Magic Lantern MLV clips as folders of CinemaDNG files, made as they are read, so
DaVinci Resolve (or any raw-capable app) imports MLV directly: no conversion, no extra disk space.

Installing
1. Drag MLV Drive to Applications and open it FROM APPLICATIONS. Don't move it afterwards.
2. macOS says '"MLV Drive.app" Not Opened': click Done (not Move to Trash). Then System Settings >
   Privacy & Security > (Security) '"MLV Drive.app" was blocked...' > Open Anyway, confirm, and
   Open Anyway again. (MLV Drive isn't signed by a registered Apple developer.)
3. MLV Drive lives in the menu bar (a drive labelled MLV). Choose your folder of MLV clips, and
   allow its notifications.
4. Once: turn on its file system extension: System Settings > General > Login Items & Extensions
   > File System Extensions (i) > MLV Drive. Not listed? Quit System Settings and open it again.
5. Once, for clips on the Desktop, in Documents or Downloads, in iCloud Drive or on external or
   network drives: System Settings > Privacy & Security > Full Disk Access > MLV Drive on (+ to
   add it). MLV Drive's menu > Setup shows whether 4 and 5 are done and opens the right pane.
6. Mount from the menu: the drive is ~/MLV Drive (your home folder), one folder per clip. Files
   show as "Locked" in Preview: the drive is read-only, that is normal.

If a mount fails with "couldn't start MLV Drive's file system extension" (or "extensionKit error
2"): MLV Drive was moved or replaced after it was first opened. Restart the Mac.

Uninstalling: quit MLV Drive; switch its file system extension off and remove it from Full Disk
Access (System Settings); then delete it from Applications.

Everything else (the menu, the command line, troubleshooting): the README at the project's page.

Licence GPL-3.0. Inspired by and partly derived from MLVFS by David Milligan; uses code from
MLV-App (third_party/mlvapp).
TXT
rm -f "$DMG"
# LZMA (ULMO, macOS 10.15+): the smallest.
hdiutil create -volname "MLV Drive $VERSION" -srcfolder "$STAGE" -fs HFS+ -format ULMO -ov "$DMG" > /dev/null
echo "$DMG ($(du -h "$DMG" | cut -f1))"
