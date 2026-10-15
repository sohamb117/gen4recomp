#!/usr/bin/env bash
# Run a heavy command (a wasm module build, a native core build, a long
# headless run) under a machine-wide semaphore, so parallel agents and
# worktrees do not push a 16 GB machine into swap.
#
#   tools/heavy.sh cmake --build build/core-dp
#   NP_HEAVY_SLOTS=1 tools/heavy.sh make -f pc/Makefile.wasm -j4
#   tools/heavy.sh --run tests/e2e/run.py --game platinum
#
# Two pools. Builds (the default) share NP_HEAVY_SLOTS slots (default 2):
# each is many processes at -j N. Two is what 16 GB holds: a native core
# build at -j4 peaks at 4-9 GB (one wasm2c'd C file compiles in up to 1.0 GB
# for Platinum, 1.9 GB D/P, 1.3 GB R/S/E, 3.1 GB HG/SS, 3.5 GB B/W; measured
# 2026-10-08), next to ~7 GB the desktop keeps resident. Runs (--run:
# headless np_gp runs, e2e milestone chains, gameplay scenarios, all one
# thread each) share their own NP_HEAVY_RUN_SLOTS slots (default 6), so six
# or more runs go in parallel without queueing behind, or blocking, a build.
#
# A slot is a directory /tmp/np-heavy/slot.<n> (runs: run.<n>) holding the
# owner's pid; a slot whose owner is gone is reclaimed. Nothing starts while
# the disk under the working directory has less than NP_HEAVY_MIN_FREE_GB
# (default 2) GiB free: it waits for space, deleting nothing. The command
# runs niced. Waiting is printed once, so a log shows the time spent queued.
set -euo pipefail

POOL=slot
SLOTS="${NP_HEAVY_SLOTS:-2}"
if [ "${1:-}" = "--run" ]; then
    shift
    POOL=run
    SLOTS="${NP_HEAVY_RUN_SLOTS:-6}"
fi
DIR=/tmp/np-heavy
MIN_FREE_KB=$((${NP_HEAVY_MIN_FREE_GB:-2} * 1024 * 1024))
mkdir -p "$DIR"

disk_ok() {
    local free
    free=$(df -k . | awk 'NR == 2 { print $4 }')
    [ "${free:-0}" -ge "$MIN_FREE_KB" ]
}

claim() {
    local i
    for ((i = 0; i < SLOTS; i++)); do
        local s="$DIR/$POOL.$i"
        if mkdir "$s" 2>/dev/null; then
            echo $$ > "$s/pid"
            SLOT="$s"
            return 0
        fi
        local owner
        owner="$(cat "$s/pid" 2>/dev/null || true)"
        if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
            rm -rf "$s"
        fi
    done
    return 1
}

SLOT=""
waited_disk=0 waited_slot=0
while :; do
    if ! disk_ok; then
        [ $waited_disk = 1 ] || echo "heavy.sh: waiting for disk (<${NP_HEAVY_MIN_FREE_GB:-2} GiB free): $*" >&2
        waited_disk=1
        sleep 30
        continue
    fi
    claim && break
    [ $waited_slot = 1 ] || echo "heavy.sh: waiting for a free $POOL slot ($SLOTS in use): $*" >&2
    waited_slot=1
    sleep 5
done
trap 'rm -rf "$SLOT"' EXIT INT TERM
nice -n 10 "$@"
