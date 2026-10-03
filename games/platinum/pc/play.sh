#!/usr/bin/env bash
#
# pc/play.sh; one command that starts the game in a window.
#
# The port and the viewer are two processes on purpose (the port is -m32 and
# the host's SDL2 is 64-bit ELF, so they meet in a shared-memory page rather
# than in a link, pc/include/pc_view.h has the protocol). That is a fact about
# the build, not something a player should have to type, so this script starts
# both, waits for the channel, and takes the port down when the window closes.
#
#   ./pc/play.sh                    # play, saving to build/pc/live.sav
#   ./pc/play.sh --new              # erase that save and start over
#   ./pc/play.sh --save FILE        # play a particular save
#   ./pc/play.sh --input FILE       # watch a replay instead of playing
#   ./pc/play.sh --unpaced          # no 60 fps governor; runs as fast as it can
#   ./pc/play.sh --wayland          # do not force SDL onto x11 (see below)
#   ./pc/play.sh --software         # present through SDL's software renderer:
#                                   # the fix when WSLg stamps [WARN: COPY MODE]
#                                   # on the title and shows a black window;
#                                   # its compositor is copying window surfaces
#                                   # that OpenGL never writes; software
#                                   # rendering draws into plain X11 images,
#                                   # which copy mode does forward
#
# --aspect auto|16:9|16:10|off widens the 3D camera to fill the window (auto)
# or to a fixed shape, more world at the sides, the same picture in the
# middle. It is the PORT's, not the viewer's.
#
# Viewer options pass straight through: --scale N, --wide (screens side by
# side), --render-scale N, --filter nearest|linear|scale2x, --integer,
# --stretch, --fullscreen. `pc/sdl/pcview.c --help` documents them; in the
# window F11/Alt+Enter is fullscreen and F12 a screenshot.
#
# It must be run from a shell that can reach your display. Under WSL that means
# your own terminal (WSLg), not an automated one: a sandboxed shell here has no
# route to :0, and the failure is silent in the worst way, pcview starts,
# takes its audio device, and puts a window nowhere.
#
# Keys: arrows = D-pad, X = a, z = b, s = x, a = y, q = l, w = R,
#       Tab held = fast-forward,
#       Return = Start, Backspace = Select, mouse on the lower screen = pen,
#       Esc quits.
#
# MODS="name ..." runs the compile-time plugin build out of build/pc-mods
# instead, with its own save. PC_MODS="pkg ..." is the other door: runtime
# content packages, no rebuild. See pc/mods/README.md.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# MODS= selects mods and moves the whole build into a directory of its own
# (pc/mods/README.md); the port, the viewer and the save follow it, so a
# modded session cannot write over a vanilla save or run a vanilla binary by
# accident. Empty is the port as shipped.
MODS="${MODS:-}"
PCBUILD="build/pc"
[[ -n "$MODS" ]] && PCBUILD="build/pc-mods"

TARGET="$PCBUILD/pokeplatinum"
VIEWER="$PCBUILD/pcview"
SAVE="$ROOT/$PCBUILD/live.sav"
ROM="${PC_ROM:-}"
INPUT=""
FRAMES=""
PACE=1
FRESH=0
WANT_WAYLAND=0
ASPECT=""
VIEWER_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --new)     FRESH=1; shift;;
        --save)    SAVE="$2"; shift 2;;
        --rom)     ROM="$2"; shift 2;;
        --input)   INPUT="$2"; shift 2;;
        --frames)  FRAMES="$2"; shift 2;;
        --unpaced) PACE=0; shift;;
        --wayland) WANT_WAYLAND=1; shift;;
        --software) export SDL_RENDER_DRIVER=software; shift;;
        --wide)    VIEWER_ARGS+=(--layout wide); shift;;
        --aspect)  ASPECT="$2"; shift 2;;
        --scale|--render-scale|--filter)
                   VIEWER_ARGS+=("$1" "$2"); shift 2;;
        --integer|--stretch|--fullscreen)
                   VIEWER_ARGS+=("$1"); shift;;
        -h|--help) awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0;;
        *) printf 'play: unknown option %s (try --help)\n' "$1" >&2; exit 2;;
    esac
done

[[ -x "$TARGET" ]] || { printf 'play: no %s, run `make -f pc/Makefile -j$(nproc)`\n' "$TARGET" >&2; exit 1; }
if [[ ! -x "$VIEWER" ]]; then
    printf 'play: building the viewer...\n'
    make -s -f pc/Makefile pcview || {
        printf 'play: the viewer needs SDL2 (apt install libsdl2-dev)\n' >&2; exit 1; }
fi

if [[ -z "$ROM" ]]; then
    if [[ -f build/rom/pokeplatinum.us.nds ]]; then
        ROM="$ROOT/build/rom/pokeplatinum.us.nds"
    else
        printf 'play: no ROM. Build one (ninja -C build/rom) or pass --rom\n' >&2
        exit 1
    fi
fi

# The display check is up front and loud, because the way this fails otherwise
# is the way it failed on 2026-08-11: the viewer runs, reports its audio device,
# and never draws, so the port looks broken when the shell was.
if [[ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]]; then
    printf 'play: no $DISPLAY and no $WAYLAND_DISPLAY; this shell cannot open\n' >&2
    printf '      a window. Run it from your own terminal.\n' >&2
    exit 1
fi

# Prefer X11 under WSLg, and that is measured rather than taste. On this
# machine SDL picks x11+opengl and the readback comes back 100% drawn; forced
# onto wayland the same binary makes Mesa fall over first,
#   libEGL warning: failed to get driver name for fd -1
#   MESA: error: ZINK: failed to choose pdev
#   libEGL warning: egl: failed to create dri2 screen,
# and the picture that survives that is not the one x11 gives. SDL_VIDEODRIVER
# set in the environment always wins, and --wayland asks for it explicitly.
if [[ -z "${SDL_VIDEODRIVER:-}" && "$WANT_WAYLAND" -eq 0 ]] \
   && [[ -n "${DISPLAY:-}" ]] && [[ -e /mnt/wslg || -n "${WSL_DISTRO_NAME:-}" ]]; then
    export SDL_VIDEODRIVER=x11
fi

[[ "$FRESH" -eq 1 ]] && rm -f "$SAVE"

# A per-run channel name, so two sessions do not publish into one page.
NAME="pplat-$$"
cleanup() {
    [[ -n "${PORT_PID:-}" ]] && kill "$PORT_PID" 2>/dev/null
    rm -f "/dev/shm/$NAME"
}
trap cleanup EXIT INT TERM

REC="${SAVE%.sav}-session-$(date +%Y%m%d-%H%M%S).txt"
# The save as it was at launch, beside the recording. (start save, input
# script) fully reproduces a deterministic session, including any saves
# made DURING it, which are just deterministic writes, so this pair is a
# save state for debugging: replay to frame N, stop, attach gdb.
[[ -f "$SAVE" ]] && cp "$SAVE" "${REC%.txt}.sav.start"
envv=(PC_VIEW="$NAME" PC_ROM="$ROM" PC_SAVE="$SAVE" PC_RECORD_INPUT="$REC")
[[ -n "$ASPECT" ]] && envv+=(PC_ASPECT="$ASPECT")
[[ -n "$INPUT" ]]  && envv+=(PC_INPUT="$INPUT")
[[ -n "$FRAMES" ]] && envv+=(PC_FRAMES="$FRAMES")
[[ "$PACE" -eq 0 ]] && envv+=(PC_PACE=0)

printf 'play: %s\n' "$ROM"
printf 'play: save %s%s\n' "$SAVE" "$([[ -f "$SAVE" ]] && echo '' || echo ' (new)')"
printf 'play: recording the session to %s\n' "$REC"
printf 'play: arrows = D-pad, X = A, Z = B, Return = Start, Backspace = Select,\n'
printf '      mouse on the lower screen = stylus, Esc quits\n'

env "${envv[@]}" "$TARGET" &
PORT_PID=$!

# Wait for the channel rather than sleeping a guessed amount: static loading
# takes a second or two and a viewer that attaches early exits saying the page
# has no magic yet.
for _ in $(seq 1 100); do
    [[ -e "/dev/shm/$NAME" ]] && break
    kill -0 "$PORT_PID" 2>/dev/null || { printf 'play: the port exited before publishing a frame\n' >&2; exit 1; }
    sleep 0.1
done
[[ -e "/dev/shm/$NAME" ]] || { printf 'play: the port never published a channel\n' >&2; exit 1; }

"$VIEWER" "$NAME" ${VIEWER_ARGS[@]+"${VIEWER_ARGS[@]}"}
