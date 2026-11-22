#!/bin/sh
# Mounts the probe, reads it, checks it, prints its report, unmounts.
#   macos/probe/run.sh [mount options]      one run, e.g. "iosize=4194304,reply=sync"
#   macos/probe/run.sh sweep                the measurements PLAN.md asks for
# Needs: the probe built (build.sh) and turned on in System Settings > General > Login Items &
# Extensions > File System Extensions; mlvdrive_tool in build/ (built here if missing).
# MNT: the mount point (default ~/MLVProbe), SRC: the folder given as the resource (default this
# folder), SECOND: a second folder for the sandbox check.
set -u
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
MNT=${MNT:-$HOME/MLVProbe}
SRC=${SRC:-$(pwd)}
SECOND=${SECOND:-}
TOOL="$ROOT/build/mlvdrive_tool"
CHECK="$ROOT/build/probe/probe_check"
SIZE=4148416

if [ ! -x "$TOOL" ]; then
    cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build "$ROOT/build" --target mlvdrive_tool >/dev/null || exit 1
fi

one() {
    opts="files=300,size=$SIZE${SECOND:+,second=$SECOND}${1:+,$1}"
    echo "=== $opts"
    mkdir -p "$MNT"
    if ! mount -F -t mlvprobe -o "$opts" "$SRC" "$MNT"; then
        echo "mount failed"
        return 1
    fi
    "$TOOL" read "$MNT" --passes=2 | tail -2
    "$TOOL" read "$MNT" --passes=1 --threads=4 | tail -1 | sed 's/^pass 1/4 readers/'
    "$CHECK" "$MNT" "$SIZE"
    head -c 1200 "$MNT/probe_report.txt" | sed -n '3,9p'
    umount "$MNT" || diskutil unmount force "$MNT" >/dev/null
}

if [ "${1:-}" = "sweep" ]; then
    for io in 262144 1048576 4194304; do one "iosize=$io,reply=sync"; done
    for io in 1048576 4194304; do one "iosize=$io,reply=async"; done
    one "iosize=1048576,reply=async,openclose=1"
    # a frame made on first read (5 ms: a 3K frame), on the workers at two QoS levels
    one "iosize=1048576,reply=async,make_us=5000,qos=initiated"
    one "iosize=1048576,reply=async,make_us=5000,qos=utility"
    one "iosize=1048576,reply=sync,make_us=5000"
else
    one "${1:-}"
fi
