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
#   live       a watched folder (the two smallest clips, cloned): a clip copied in shows, one
#              replaced under its name shows the new footage while a file opened before reads the
#              old, one deleted goes
#   rescan     not watched (-o watch=0): a clip shows only after `mlvdrive rescan`
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

# --- live: a watched folder's clips added, replaced (a file of it open), removed; rescan ---
# The two smallest clips, cloned (APFS: no space taken) into a scratch folder mounted on its own.
LIVE="$SCRATCH/live"
LMNT="$SCRATCH/live-mnt"
CLI="$ROOT/build/MLV Drive.app/Contents/MacOS/mlvdrive"
set -- $(ls -S -r $(cat "$SCRATCH/clips" | tr '\n' ' ') 2>/dev/null | head -2)
A=${1:-}
B=${2:-}
wait_for() {  # seconds, test...: until the test passes; the seconds it took, or fails
    limit=$1; shift
    t=0
    while [ "$t" -lt "$((limit * 10))" ]; do
        if "$@"; then echo "$((t / 10)).$((t % 10)) s"; return 0; fi
        sleep 0.1; t=$((t + 1))
    done
    return 1
}
frames_in() { ls "$1" 2>/dev/null | grep -ci '\.dng$'; }
if [ -n "$B" ]; then
    mkdir -p "$LIVE" "$LMNT" "$SCRATCH/keep"
    cp -c "$A" "$LIVE/LiveA.MLV"
    cp -c "$A" "$SCRATCH/keep/LiveA.MLV"  # (to make its frames again after it is replaced)
    nA=$("$TOOL" info "$A" | awk '/^frames/ { print $2 }')
    nB=$("$TOOL" info "$B" | awk '/^frames/ { print $2 }')
    if mount -F -t mlvdrive "$LIVE" "$LMNT"; then
        cp -c "$B" "$LIVE/LiveB.MLV"
        took=$(wait_for 15 test -d "$LMNT/LiveB") ; result live $? "a clip copied in shows ($took)"
        exec 3< "$LMNT/LiveA/LiveA_000001.dng"
        cp -c "$B" "$LIVE/LiveA.MLV.new" && mv -f "$LIVE/LiveA.MLV.new" "$LIVE/LiveA.MLV"
        took=$(wait_for 15 test "$(frames_in "$LMNT/LiveA")" = "$nB")
        result live $? "a clip replaced by other footage under its name: $nA -> $(frames_in "$LMNT/LiveA") frames ($took)"
        cat <&3 > "$SCRATCH/old.dng"
        exec 3<&-
        "$TOOL" dng "$SCRATCH/keep/LiveA.MLV" 1 "$SCRATCH/f.dng" > /dev/null 2>&1
        cmp -s "$SCRATCH/old.dng" "$SCRATCH/f.dng"
        result live $? "a file opened before the change still reads the old footage"
        "$TOOL" dng "$LIVE/LiveA.MLV" 1 "$SCRATCH/f.dng" > /dev/null 2>&1
        cmp -s "$LMNT/LiveA/LiveA_000001.dng" "$SCRATCH/f.dng"
        result live $? "the replaced clip's frame 1 is the new footage"
        rm "$LIVE/LiveB.MLV"
        took=$(wait_for 15 test ! -d "$LMNT/LiveB"); result live $? "a clip deleted goes ($took)"
        umount "$LMNT"
    else
        result live 1 "mount -F -t mlvdrive $LIVE failed"
    fi
    # Not watched: a clip shows only after `mlvdrive rescan`.
    if mount -F -t mlvdrive -o watch=0 "$LIVE" "$LMNT"; then
        cp -c "$B" "$LIVE/LiveC.MLV"
        sleep 4
        if [ -d "$LMNT/LiveC" ]; then
            result rescan 1 "watch=0: a clip showed without a rescan"
        else
            "$CLI" rescan > /dev/null
            took=$(wait_for 15 test -d "$LMNT/LiveC"); result rescan $? "watch=0: a clip shows after mlvdrive rescan ($took)"
        fi
        umount "$LMNT"
    else
        result rescan 1 "mount -o watch=0 failed"
    fi
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
