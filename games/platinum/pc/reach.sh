#!/usr/bin/env bash
#
# How far the port gets before it stops, and what stopped it.
#
# Ported from the sibling diamond port's pc/reach.sh, which is where the
# reasoning below was paid for.
#
# The frame count alone is not the signal. That port once cleared a geometry
# abort at frame 1122 and the reach stayed at exactly 1122: it now drew that
# frame and died just after it, of something else. A check that compared only
# frame counts would have reported "no change" on a run that moved the wall
# from a diagnosed trap to an undiagnosed SIGSEGV. So the record is (frames,
# how it ended, where, longest still stretch), and any of them moving is news.
#
# The still stretch is there because a frozen port exits 0. Ticking VBlanks
# while drawing the same picture forever ends "clean exit after N frames" and
# reads exactly like a healthy run. The manifest carries a digest per sampled
# frame, so the longest run of identical ones is free to compute. A real
# playthrough holds a picture still for about 240 frames at its longest, so a
# stretch in the thousands is a freeze and not a fade.
#
# Nothing here is a gate. A reach that goes down is a regression and should be
# loud; a reach that goes up is the point. Both are reported.
#
# The cap has to stay ahead of the wall, or this goes blind in the most
# misleading way available: a run that ends "clean exit after N frames" because
# N is where the harness stopped reads exactly like a port with no wall left.
# Raise REACH_FRAMES the moment a reach comes back "clean exit after N".
#
# Differences from the diamond copy, all mechanical:
#   * this port has no CLI; every input is an environment variable, so this
#     script sets PC_FRAMES, PC_DUMP_FRAMES and PC_ROM rather than flags.
#   * the ROM is the tree's own build, so a missing ROM is a build problem.
#   * PC_SAVE=none, so a reach never writes a save and two reaches in a row
#     measure the same boot.
#
# What an input-less reach is evidence of. By default this drives no input, so
# past the title screen the port sits in the attract loop: "20,000 frames,
# clean exit" means the boot and the attract loop are clear and says nothing
# about the game. `--input FILE` runs a PC_INPUT script instead, and
# pc/replays/new-game.txt drives a cold boot to a save in Twinleaf.
#
# Usage: pc/reach.sh [--frames N] [--rom PATH] [--input FILE] [--quiet]
# Env: REACH_FRAMES, REACH_TIMEOUT, REACH_INPUT, REACH_STRIDE, REACH_OUT
# REACH_OUT gets one tab-separated line: frames, ending, site, still stretch.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TARGET="build/pc/pokeplatinum"
# The attract loop's period is 24,000 FRAMES, measured 2026-08-12 by running
# 120,000 input-less frames and finding the repeats in the manifest digests
# (three consecutive 24,000 gaps). The old 20,000 cap therefore stopped inside
# the first cycle, every time, and had never once shown the port the whole of
# what it loops over. 26,000 covers a cycle with margin, and costs less wall
# clock than 20,000 used to now that a reach writes a manifest instead of
# 20,000 PNGs. 120,000 (five cycles) was clean, so the wall is not just
# past the cap.
FRAMES="${REACH_FRAMES:-26000}"
# One PNG a second, for looking at what was on screen when a run stopped. The
# manifest gets a row per frame regardless, and that is what the count and the
# still-stretch are read from.
STRIDE="${REACH_STRIDE:-60}"
TIMEOUT="${REACH_TIMEOUT:-600}"
ROM="${PC_ROM:-}"
INPUT="${REACH_INPUT:-}"
QUIET=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --frames) FRAMES="$2"; shift 2;;
        --rom)    ROM="$2"; shift 2;;
        --input)  INPUT="$2"; shift 2;;
        --quiet)  QUIET=1; shift;;
        *) printf 'usage: pc/reach.sh [--frames N] [--rom PATH] [--input FILE] [--quiet]\n' >&2; exit 2;;
    esac
done

[[ -x "$TARGET" ]] || { printf 'reach: no %s, build first\n' "$TARGET" >&2; exit 0; }

# The tree builds its own ROM; that is the default and $PC_ROM overrides it.
# A dump beside the repo is accepted too, for the same reason diamond accepts
# one: it is the artifact a differential oracle will want.
if [[ -z "$ROM" ]]; then
    if [[ -f "build/rom/pokeplatinum.us.nds" ]]; then
        ROM="build/rom/pokeplatinum.us.nds"
    else
        for p in *.nds; do
            [[ -f "$p" ]] && { ROM="$p"; break; }
        done
    fi
fi
[[ -n "$ROM" ]] || {
    printf 'reach: no ROM. Build one (ninja -C build/rom) or set $PC_ROM\n' >&2
    exit 0
}

tmp=$(mktemp -d /tmp/pc_reach.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# PC_DUMP_FRAMES rather than a counter inside the port: the manifest is flushed
# per line, so the count survives a signal, which is the ending this actually
# has to measure.
# `|| rc=$?` rather than a bare call: bash announces "Segmentation fault" for a
# signalled command run on its own, and not for one in a list. Dying of a signal
# is the expected outcome here and the script reports it properly below.
#
# `env` with an array rather than an assignment prefix: a prefix has to be
# literal at parse time, so `${INPUT:+PC_INPUT="$INPUT"}` expands into the
# COMMAND position and the run dies with 127 before the port is reached.
#
# The frame count comes from manifest ROWS, not from PNG files. A row is one
# fprintf and costs nothing measurable; a PNG is an encode, and on gameplay
# frames that is 5x the whole run (194 -> 38 frames/s, measured over the
# replay 2026-08-12). Counting rows made a 27,400-frame replay reach 141 s
# instead of ~12 minutes and 12 MB instead of ~2 GB, and it is exact rather
# than strided.
rc=0
envv=(PC_ROM="$ROM" PC_SAVE=none PC_FRAMES="$FRAMES" PC_DUMP_FRAMES="$tmp/f"
      PC_DUMP_FROM="0:$STRIDE")
[[ -n "$INPUT" ]] && envv+=(PC_INPUT="$INPUT")
{ env "${envv[@]}" timeout "$TIMEOUT" "$TARGET" > "$tmp/out" 2>&1 || rc=$?; } 2>/dev/null

manifest="$tmp/f/frames.txt"
frames=$(grep -vc '^#' "$manifest" 2>/dev/null || true)
frames="${frames:-0}"

# The longest run of identical digests, in frames. Sampled every $STRIDE, so
# the answer is a multiple of it; '-' rows are the frames with no image and
# are skipped rather than treated as equal to each other.
still=$(awk -v s="$STRIDE" '
    !/^#/ && $7 != "-" {
        if ($7 == prev) { n++; if (n > best) best = n } else { n = 0 }
        prev = $7
    }
    END { printf "%d", best * s }' "$manifest" 2>/dev/null)
still="${still:-0}"

# How it ended, in the three kinds that mean different things.
case "$rc" in
    0)   ending="clean exit after $FRAMES frames";;
    124) ending="killed by REACH_TIMEOUT after ${TIMEOUT}s";;
    139) ending="SIGSEGV";;
    134) ending="SIGABRT";;
    *)   if [[ "$rc" -gt 128 ]]; then ending="signal $((rc - 128))"; else ending="exit $rc"; fi;;
esac

# Where. A named trap prints its own name and is the good case, every pc/src
# trap in this port does. A bare signal has to be asked for, and gdb is
# optional: without it the reach is still a number, just without a site.
#
# Only asked when the run did not end cleanly. This port prints informational
# lines at boot ("pc_snd: ARM7 sound driver is live behind PXI tag 7"), and the
# first draft of this script matched one of those and reported it as the site
# of a clean exit; a line that reads like a diagnosis and diagnoses nothing.
site=""
if [[ "$rc" -ne 0 ]]; then
    site=$(grep -m1 -oE '.*(trap|TRAP|unimplemented|not implemented|abort|FATAL|fatal).*' "$tmp/out" | cut -c1-160)
    [[ -n "$site" ]] || site=$(grep -m1 -oE '^(pc[-_][a-z0-9]+|armrec)[:.] .*' "$tmp/out" | tail -1 | cut -c1-160)
fi
if [[ "$rc" -eq 0 ]]; then
    site="(clean exit; the cap is what stopped it, not a wall)"
    # ...unless it stopped drawing long ago, which exits 0 just the same. A
    # quarter of the run on one picture is far past the ~240 frames the real
    # playthrough's longest fade holds.
    if [[ "$frames" -gt 0 && "$still" -gt $((frames / 4)) ]]; then
        site="FROZEN: the same picture for $still of $frames frames, and it still exited 0"
    fi
fi
if [[ -z "$site" && "$rc" -gt 128 ]] && command -v gdb >/dev/null 2>&1; then
    site=$(env "${envv[@]}" \
             timeout "$TIMEOUT" gdb -q -batch -ex run -ex "bt 3" \
             --args "$TARGET" 2>/dev/null \
           | grep -m1 -E '^#0 ' | cut -c1-160)
    [[ -n "$site" ]] && site="(no message) $site"
fi
[[ -n "$site" ]] || site="(no message, and gdb could not be asked)"

# The sampled frames are the only picture of what was on screen when a run
# stopped, and the tmpdir goes away in a second. Worth keeping exactly when
# something went wrong.
kept=""
if [[ "$rc" -ne 0 ]]; then
    kept="build/pc/reach-frames"
    rm -rf "$kept" && mkdir -p "$(dirname "$kept")" && cp -r "$tmp/f" "$kept" \
        && cp "$tmp/out" "$kept/output.txt" || kept=""
fi

if [[ "$QUIET" -eq 0 ]]; then
    printf 'reach: %s frames, %s%s\n' "$frames" "$ending" \
        "${INPUT:+ (driven by $INPUT)}"
    printf 'reach: %s\n' "$site"
    printf 'reach: longest still stretch %s frames (sampled every %s)\n' \
        "$still" "$STRIDE"
    [[ -n "$kept" ]] && printf 'reach: frames and output kept in %s\n' "$kept"
fi
printf '%s\t%s\t%s\t%s\n' "$frames" "$ending" "$site" "$still" \
    > "${REACH_OUT:-/dev/null}"
exit 0
