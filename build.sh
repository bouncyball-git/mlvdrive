#!/bin/sh
# Builds MLV Drive on macOS 26 or later (the Xcode Command Line Tools, CMake and Ninja); build.ps1's
# counterpart.
#
#   ./build.sh [clean] [build] [install] [uninstall] [package] [--debug]
#   ./build.sh pkg              (or --pkg): build, then the disk image, for a release
#
#   clean      deletes the build folders (build, build-release)
#   build      configures (if needed) and builds (the default action):
#                build/MLV Drive.app    the menu bar app, its FSKit extension and the mlvdrive command
#                build/mlvdrive_tool    the core on the command line (clip info, DNGs, timings)
#              Registered with macOS so its extension can be turned on, unless MLV Drive is installed
#              in /Applications: one copy at a time (two compete for the extension), then use install.
#   install    builds the release app (universal) and puts it in /Applications/MLV Drive.app, in
#              place of the one there (same place: macOS keeps finding its extension); quits and
#              unmounts a running copy first, unregisters build/MLV Drive.app, opens it.
#   uninstall  quits it, unmounts, resets its Full Disk Access, unregisters and deletes
#              /Applications/MLV Drive.app; keeps the settings (~/Library/Application Support/MLV
#              Drive). Its file system extension must be turned off first (System Settings), as no
#              command does that: uninstall stops and says so while it is on.
#   package    dist/MLVDrive-<version>-macos.dmg (tools/package-macos.sh: universal, ad hoc signed)
#   --debug    a Debug build (build only)
set -eu
cd "$(dirname "$0")"
ROOT=$(pwd)
BUILD="$ROOT/build"
RELEASE="$ROOT/build-release"
INSTALLED="/Applications/MLV Drive.app"
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
CONFIG=Release
ACTIONS=""
for a in "$@"; do
    case "$a" in
        --debug) CONFIG=Debug ;;
        pkg|--pkg|-pkg) ACTIONS="$ACTIONS build package" ;;
        clean|build|install|uninstall|package) ACTIONS="$ACTIONS $a" ;;
        *) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done
[ -z "$ACTIONS" ] && ACTIONS=build

quit_running() {
    if pgrep -xq "MLV Drive"; then
        osascript -e 'tell application id "org.mlvdrive.MLVDrive" to quit' 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -xq "MLV Drive" || break; sleep 0.5; done
    fi
    # Its volumes' mount points ("... on <mount point> (mlvdrive, ...)"), one a line (they may have spaces).
    mount | sed -n 's/^.* on \(.*\) (mlvdrive.*/\1/p' | while IFS= read -r m; do
        umount "$m" 2>/dev/null || true
    done
}

module_on() {
    plutil -p "$HOME/Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist" 2>/dev/null |
        grep -q 'org.mlvdrive.MLVDrive.fs'
}

for action in $ACTIONS; do
    case "$action" in
    clean)
        rm -rf "$BUILD" "$RELEASE"
        echo "deleted build, build-release"
        ;;
    build)
        # With MLV Drive in /Applications, build/MLV Drive.app isn't registered (one copy at a time).
        if [ -d "$INSTALLED" ]; then release_bundle=ON; else release_bundle=OFF; fi
        cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=$CONFIG -DMLVDRIVE_RELEASE_BUNDLE=$release_bundle > /dev/null
        cmake --build "$BUILD"
        if [ "$release_bundle" = ON ]; then
            echo "build: MLV Drive is installed in /Applications, so build/MLV Drive.app isn't registered; ./build.sh install puts this build there"
        fi
        ;;
    install)
        cmake -S "$ROOT" -B "$RELEASE" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
              -DMLVDRIVE_RELEASE_BUNDLE=ON > /dev/null
        cmake --build "$RELEASE"
        quit_running
        [ -d "$BUILD/MLV Drive.app" ] && "$LSREGISTER" -u "$BUILD/MLV Drive.app" 2>/dev/null || true
        first=0
        [ -d "$INSTALLED" ] || first=1
        # Into a new folder, then swapped in: the extension, still running after the unmount, keeps
        # its old file (a binary overwritten under it is killed); then stopped, to load the new one.
        rm -rf "$INSTALLED.new"
        ditto "$RELEASE/MLV Drive.app" "$INSTALLED.new"
        rm -rf "$INSTALLED"
        mv "$INSTALLED.new" "$INSTALLED"
        "$LSREGISTER" -f -R "$INSTALLED"
        pkill -x MLVDriveFS 2>/dev/null && sleep 2 || true
        open "$INSTALLED"
        echo "installed $INSTALLED"
        if [ "$first" = 1 ]; then
            echo "  first install: turn its file system extension on and give it Full Disk Access (README, Installing);"
            echo "  if another copy of MLV Drive was opened on this Mac before, restart the Mac first (README, Troubleshooting)"
        fi
        ;;
    uninstall)
        if [ ! -d "$INSTALLED" ]; then
            echo "uninstall: MLV Drive isn't in /Applications"
            continue
        fi
        if module_on; then
            echo "uninstall: first turn MLV Drive's file system extension off: System Settings > General >"
            echo "  Login Items & Extensions > File System Extensions (i) > MLV Drive (it would stay on otherwise)"
            exit 1
        fi
        quit_running
        pkill -x MLVDriveFS 2>/dev/null || true
        for service in SystemPolicyAllFiles SystemPolicyDesktopFolder SystemPolicyDocumentsFolder; do
            tccutil reset "$service" org.mlvdrive.MLVDrive > /dev/null 2>&1 || true
        done
        "$LSREGISTER" -u "$INSTALLED" 2>/dev/null || true
        rm -rf "$INSTALLED"
        echo "uninstalled $INSTALLED (Full Disk Access reset; the settings kept: ~/Library/Application Support/MLV Drive)"
        [ -L /usr/local/bin/mlvdrive ] && echo "  the mlvdrive command is still linked: sudo rm /usr/local/bin/mlvdrive"
        echo "  Start at login, if it was on: System Settings > General > Login Items & Extensions > Open at Login"
        ;;
    package)
        "$ROOT/tools/package-macos.sh"
        ;;
    esac
done
