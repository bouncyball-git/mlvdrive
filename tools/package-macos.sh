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

1. Drag "MLV Drive" to Applications, then open it there. It lives in the menu bar.
   It isn't signed by a registered Apple developer: macOS refuses it the first time. Then:
   System Settings > Privacy & Security > "MLV Drive was blocked..." > Open Anyway.
2. Turn on its file system extension, once: System Settings > General > Login Items &
   Extensions > File System Extensions > MLV Drive (quit and reopen System Settings if it
   isn't listed).
3. For clips on the Desktop, in Documents or on external or network drives: System Settings >
   Privacy & Security > Full Disk Access > + > MLV Drive.
4. In MLV Drive's menu: choose your MLV folder; the drive mounts on ~/MLV Drive.
   The menu's Setup shows whether 2 and 3 are done and opens the right pane.

Command line (optional): MLV Drive's menu > Setup > Install the mlvdrive command; then
mlvdrive mount | unmount | rescan (mlvdrive with no arguments for help).

Licence GPL-3.0. Inspired by and partly derived from MLVFS by David Milligan; uses code from
MLV-App (third_party/mlvapp).
TXT
rm -f "$DMG"
# LZMA (ULMO, macOS 10.15+): 10 MB; zlib (UDZO) made 97 MB of it: the pixel maps (360 MB of text).
hdiutil create -volname "MLV Drive $VERSION" -srcfolder "$STAGE" -fs HFS+ -format ULMO -ov "$DMG" > /dev/null
echo "$DMG ($(du -h "$DMG" | cut -f1))"
