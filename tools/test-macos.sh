#!/bin/sh
# The macOS checks of the FSKit drive (tools/test.ps1's counterpart). PASS / FAIL per check, the
# exit code the number of failures.
#
#   tools/test-macos.sh <MLV folder> [--all] [--mount=<mount point>]
#
#   mount      the folder mounts with the test patterns (mount -F -t mlvdrive)
#   frames     each clip's frames read from the drive, in random order, byte-identical to
#              mlvdrive_tool dng (first, middle, last and 20 random ones; --all: every frame)
#   wav        each clip's WAV identical to mlvdrive_tool wav
#   patterns   a chart's image (TestPatterns frame 125, chart 2) equals mlvdrive_tool patterns'
#   speed      the first clip read in order, unbuffered, twice (mlvdrive_tool read)
#   unmount    umount lets go
#
# Needs: the build (cmake --build build: MLV Drive.app and mlvdrive_tool), "MLV Drive" turned on
# in System Settings > General > Login Items & Extensions > File System Extensions, and for
# protected folders (Desktop, Documents, external drives) Full Disk Access for MLV Drive.app.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL="$ROOT/build/mlvdrive_tool"
FOLDER=""
ALL=0
MNT="$HOME/MLVDrive"
for a in "$@"; do
    case "$a" in
        --all) ALL=1 ;;
        --mount=*) MNT=${a#--mount=} ;;
        *) FOLDER=$a ;;
    esac
done
if [ -z "$FOLDER" ] || [ ! -d "$FOLDER" ] || [ ! -x "$TOOL" ]; then
    echo "usage: tools/test-macos.sh <MLV folder> [--all] [--mount=<mount point>]  (build first)"
    exit 2
fi
FOLDER=$(cd "$FOLDER" && pwd)
SCRATCH=$(mktemp -d)
FAILS=0
result() {  # name, 0/1 (ok), message
    if [ "$2" = 0 ]; then echo "PASS  $1: $3"; else echo "FAIL  $1: $3"; FAILS=$((FAILS + 1)); fi
}
cleanup() {
    mount | grep -q " on $MNT " && umount "$MNT" 2>/dev/null
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

# --- mount ---
mkdir -p "$MNT"
mount | grep -q " on $MNT " && umount "$MNT"
if mount -F -t mlvdrive -o test_patterns=1 "$FOLDER" "$MNT"; then
    result mount 0 "$FOLDER on $MNT ($(ls "$MNT" | wc -l | tr -d ' ') entries at the root)"
else
    result mount 1 "mount -F -t mlvdrive failed (module turned on? Full Disk Access?)"
    exit "$FAILS"
fi

# --- frames, wav: each clip (its folder on the drive: the clip's subfolder + its name) ---
first=""
find "$FOLDER" -iname '*.mlv' -type f | sort > "$SCRATCH/clips"
while IFS= read -r clip; do
    rel=$(dirname "${clip#$FOLDER}")
    name=$(basename "$clip"); name=${name%.*}
    dir="$MNT$rel/$name"; dir=$(echo "$dir" | sed 's|/\./|/|; s|//|/|g')
    if [ ! -d "$dir" ]; then
        result frames 1 "$name: no folder $dir on the drive"
        continue
    fi
    [ -z "$first" ] && first=$dir
    n=$(ls "$dir" | grep -ci '\.dng$')
    if [ "$ALL" = 1 ]; then
        seq 0 $((n - 1)) > "$SCRATCH/frames"
    else
        { echo 0; echo $((n / 2)); echo $((n - 1)); awk -v n="$n" 'BEGIN { srand(); for (i = 0; i < 20; i++) print int(rand() * n) }'; } > "$SCRATCH/frames"
    fi
    sort -R -u "$SCRATCH/frames" > "$SCRATCH/order"
    bad=0
    count=0
    while IFS= read -r f; do
        file=$(printf '%s/%s_%06d.dng' "$dir" "$name" "$f")
        "$TOOL" dng "$clip" "$f" "$SCRATCH/f.dng" > /dev/null 2>&1
        if ! cmp -s "$file" "$SCRATCH/f.dng"; then
            [ "$bad" -lt 3 ] && echo "      frame $f differs"
            bad=$((bad + 1))
        fi
        count=$((count + 1))
    done < "$SCRATCH/order"
    result frames "$([ "$bad" = 0 ] && echo 0 || echo 1)" "$name: $count of $n frames, random order, $bad differ"
    if [ -f "$dir/$name.wav" ]; then
        "$TOOL" wav "$clip" "$SCRATCH/a.wav" > /dev/null 2>&1
        cmp -s "$dir/$name.wav" "$SCRATCH/a.wav"
        result wav $? "$name.wav ($(stat -f %z "$dir/$name.wav") bytes) identical to mlvdrive_tool's"
    fi
done < "$SCRATCH/clips"

# --- patterns: a chart's image data (after the header: the timecodes differ) ---
"$TOOL" patterns "$SCRATCH/patterns" > /dev/null 2>&1
image=$((1920 * 1080 * 2))
if [ "$(tail -c "$image" "$MNT/TestPatterns/TestPatterns_000125.dng" | shasum)" = \
     "$(tail -c "$image" "$SCRATCH/patterns/02.dng" | shasum)" ]; then
    result patterns 0 "TestPatterns frame 125 (chart 2) equals mlvdrive_tool's chart"
else
    result patterns 1 "TestPatterns frame 125 differs from mlvdrive_tool's chart 2"
fi

# --- speed ---
if [ -n "$first" ]; then
    echo "speed ($first):"
    "$TOOL" read "$first" --passes=2 | sed 's/^/      /'
fi

# --- unmount ---
umount "$MNT"
! mount | grep -q " on $MNT "
result unmount $? "umount $MNT"
echo "$FAILS failed"
exit "$FAILS"
