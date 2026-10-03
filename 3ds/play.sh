#!/usr/bin/env bash
#
# 3ds/play.sh: run the port in the emulator on your own desktop, with the
# keyboard, and leave the reports where they can be watched.
#
# Why this exists beside azahar_shot.sh. That script drives the emulator with
# xdotool, which on this desktop needs a private X server it starts itself;
# the whole apparatus is there so an unattended check can press a button. A
# person at the keyboard needs none of it and is much better at the game, so
# this is the same emulator, the same 3dsx and the same SD card, launched on
# the display that is already in front of you.
#
# What it sets up. The AppImage needs libOpenGL.so.0 out of the unpacked
# prefix (it is not a system package here), and the SD card directory has to
# exist before the port can write a report into it; a console that has only
# ever launched this port from its own RomFS has never had one made.
#
# The old reports are moved aside, not left. Both files are rewritten in
# place while a run is going, so a stale one from yesterday reads exactly like
# a live one from a run that has not got there yet.
#
# Buttons are the emulator's own default map: a=A, s=B, z=X, x=Y, q=L, w=R,
# m=Start, n=Select, and the arrow keys are the D-pad. The touch screen is the
# mouse on the lower panel.
#
# It does not boot straight into the game. The first thing on screen is this
# port's diagnostic screen, a blue panel over a maroon one with every host
# model's self-test total on it, and A is what hands the console to the
# game. The 8x8 block in each panel's top-left corner is the verdict: amber
# passed, red did not.
#
# Usage: 3ds/play.sh [file.3dsx]
# Env:   AZAHAR, GLVND_PREFIX, SDMC (the emulator's SD card root)

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

AZAHAR=${AZAHAR:-$HOME/opt/azahar/azahar.AppImage}
GLVND_PREFIX=${GLVND_PREFIX:-$HOME/opt/glvnd/root/usr}
SDMC=${SDMC:-$HOME/.local/share/azahar-emu/sdmc}
APP=${1:-build/3ds/pokeplatinum.3dsx}

for f in "$AZAHAR" "$APP"; do
    [[ -e "$f" ]] || { printf 'play: missing %s\n' "$f" >&2; exit 2; }
done

export LD_LIBRARY_PATH="$GLVND_PREFIX/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

dir="$SDMC/3ds/pokeplatinum"
mkdir -p "$dir"
for r in perf-report.txt snd-report.txt; do
    [[ -f "$dir/$r" ]] && mv -f "$dir/$r" "$dir/$r.prev"
done

printf 'play: %s\n' "$APP"
printf 'play: reports land in %s\n' "$dir"
printf 'play: keys a=A s=B z=X x=Y q=L w=R m=Start n=Select, arrows are the D-pad\n'
printf 'play: the diagnostic screen comes up first, press A to start the game.\n'
printf 'play: the corner block on each panel is the verdict: amber passed.\n'

exec "$AZAHAR" "$APP"
