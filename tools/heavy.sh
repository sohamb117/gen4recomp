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
# each is many processes at -j N. Runs (--run: headless np_gp runs, e2e
# milestone chains, gameplay scenarios, all one thread each) share their own
# NP_HEAVY_RUN_SLOTS slots (default 6), so six or more runs go in parallel
# without queueing behind, or blocking, a build.
#
# A slot is a directory /tmp/np-heavy/slot.<n> (runs: run.<n>) holding the
# owner's pid; a slot whose owner is gone is reclaimed. The command runs
# niced. Waiting is printed once, so a log shows the time spent queued.
set -euo pipefail

POOL=slot
SLOTS="${NP_HEAVY_SLOTS:-2}"
if [ "${1:-}" = "--run" ]; then
    shift
    POOL=run
    SLOTS="${NP_HEAVY_RUN_SLOTS:-6}"
fi
DIR=/tmp/np-heavy
mkdir -p "$DIR"

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
if ! claim; then
    echo "heavy.sh: waiting for a free $POOL slot ($SLOTS in use): $*" >&2
    until claim; do sleep 5; done
fi
trap 'rm -rf "$SLOT"' EXIT INT TERM
nice -n 10 "$@"
