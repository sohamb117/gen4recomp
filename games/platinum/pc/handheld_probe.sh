#!/bin/sh
# Everything a handheld can be asked before a frontend exists, in one run.
#
# The handheld notes end on a list of lines to type on a small ARM device.
# This is that list, runnable, plus the two binaries the engine tree ships
# beside it. Copy the three files to the device and run this:
#
#   ./handheld_probe.sh              # everything
#   ./handheld_probe.sh --no-bench   # skip the frame-time run
#
# Every question is asked the way it will matter later. The ELF, non-PIE and
# identity-map answers come from pc-probe, which links the port's own
# armrec_rt.c rather than a hello-world; the frame time comes from the port
# itself, headless, which is the number the plan's DMIPS arithmetic is
# guessing at and the 3DS has already caught an emulator understating by
# seventeen times.
#
# Nothing here writes to the device outside the working directory, and
# nothing needs root.
set -u
BENCH=1
[ "${1:-}" = "--no-bench" ] && BENCH=0
here=$(dirname "$0")

say() { printf '\n===== %s =====\n' "$1"; }

say "what this device is"
uname -a
# The rest are best-effort: a Linux handheld has no getprop, an Android one
# has no /sys/class/drm the shell can read. Missing output is an answer.
[ -r /proc/cpuinfo ] && grep -E 'model name|Hardware|Processor|BogoMIPS|CPU part' /proc/cpuinfo | sort -u
command -v getprop >/dev/null 2>&1 && {
    echo "-- Android properties"
    echo "  abilist: $(getprop ro.product.cpu.abilist)     # armeabi-v7a must be here"
    echo "  release: $(getprop ro.build.version.release)   # 13 stock 556, 14 stock DS, or Gamma"
    echo "  device:  $(getprop ro.product.device)"
}

say "the panels"
# On Linux: two DSI connectors, each 60 Hz. On Android: ask the display
# service instead, because the top panel is the one reported at 40 Hz.
if ls /sys/class/drm/*/status >/dev/null 2>&1; then
    for s in /sys/class/drm/*/status; do
        c=$(dirname "$s"); printf '  %-28s %s\n' "$(basename "$c")" "$(cat "$s")"
    done
    for m in /sys/class/drm/card*-DSI-*/modes; do
        [ -r "$m" ] && { echo "  modes $(dirname "$m" | xargs basename):"; sed 's/^/    /' "$m"; }
    done
elif command -v dumpsys >/dev/null 2>&1; then
    dumpsys display 2>/dev/null | grep -iE 'fps|refresh|mode ' | head -20
else
    echo "  (neither /sys/class/drm nor dumpsys, report what this device has)"
fi

say "does the engine's memory model hold here"
if [ -x "$here/pc-probe" ]; then
    "$here/pc-probe"; echo "  pc-probe exit $?"
else
    echo "  pc-probe missing, build it with: make -f pc/Makefile.arm probe"
fi

if [ "$BENCH" = 1 ]; then
    say "what a frame costs"
    if [ ! -x "$here/pokeplatinum" ]; then
        echo "  the port is not here, build it with: make -f pc/Makefile.arm static"
    elif [ ! -f "$here/pokeplatinum.us.nds" ]; then
        echo "  pokeplatinum.us.nds is not beside the binary; the port needs its ROM"
    else
        echo "  900 frames, headless, no pacing. This is the number the plan guesses."
        # PC_BENCH=1 is what makes the port report its own breakdown; without
        # it the run is silent and the bench prints nothing, which is how the
        # first draft of this script was wrong. Same shape as pc/Makefile's
        # `bench` target. The replay is used when it is here and skipped when
        # it is not, a bare boot still times frames.
        # `env` and not a bare assignment prefix: a shell only recognises
        # assignments that are LITERAL words, so ${script:+PC_INPUT=...} is
        # expanded too late and the shell tries to run the result as the
        # command. Measured; the first draft did exactly that, the
        # "not found" went to stderr, and the grep below swallowed it, so the
        # bench looked like it had simply printed nothing.
        set -- env PC_ROM="$here/pokeplatinum.us.nds" PC_SAVE=none PC_PACE=0 \
               PC_BENCH=1 PC_FRAMES=900
        [ -f "$here/new-game.txt" ] && set -- "$@" PC_INPUT="$here/new-game.txt"
        "$@" "$here/pokeplatinum" 2>&1 >/dev/null | grep '^pc-bench' \
            || echo "  the port printed no pc-bench line, send its whole output"
    fi
fi

say "done"
echo "Send this whole output back; the numbers that matter are the identity"
echo "map verdict, the panel modes, and the frame time."
