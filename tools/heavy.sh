#!/usr/bin/env bash
# Run a heavy command (a wasm module build, a native core build, a long
# headless run) under a machine-wide semaphore, so parallel agents and
# worktrees do not push a 16 GB machine into swap.
#
#   tools/heavy.sh cmake --build build/core-dp
#   NP_HEAVY_SLOTS=1 tools/heavy.sh make -f pc/Makefile.wasm -j4
#
# A slot is a directory /tmp/np-heavy/slot.<n> holding the owner's pid; a slot
# whose owner is gone is reclaimed. The command runs niced. Waiting is printed
# once, so a log shows the time spent queued.
set -euo pipefail

SLOTS="${NP_HEAVY_SLOTS:-2}"
DIR=/tmp/np-heavy
mkdir -p "$DIR"

claim() {
    local i
    for ((i = 0; i < SLOTS; i++)); do
        local s="$DIR/slot.$i"
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
    echo "heavy.sh: waiting for a free slot ($SLOTS in use): $*" >&2
    until claim; do sleep 5; done
fi
trap 'rm -rf "$SLOT"' EXIT INT TERM
nice -n 10 "$@"
