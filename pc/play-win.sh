#!/usr/bin/env bash
#
# pc/play-win.sh, start the WINDOWS build of the game in a window, from WSL.
#
# The Windows exe draws through Windows itself, so WSLg (and its copy-mode
# failure, where every Linux window shows black) is never in the path. This
# is the same reason pokediamond's play sessions run its .exe.
#
#   ./pc/play-win.sh                    # play, saving to build/pc-win32/live.sav
#   ./pc/play-win.sh --new              # erase that save and start over
#   ./pc/play-win.sh --save FILE        # play a particular save
#   ./pc/play-win.sh --aspect auto      # widescreen: the 3D camera fills the
#                                       # window (or 16:9 / 16:10 for a fixed
#                                       # shape). The port's, not the viewer's.
#
# Settings reach the exe as KEY=VALUE arguments (WSL does not forward
# environment variables to Windows processes), and every path must be
# repo-relative: the exe's working directory is this repo over the network
# (\\wsl.localhost\...), and a /home/... absolute path means nothing to
# Windows.
#
# The EXE cache (diamond's lesson, half a night lost): Windows can serve a
# STALE IMAGE for a rebuilt exe at the same WSL path. Every launch copies
# the exe to a fresh name first, onto NTFS now, which the staging note
# further down explains is worth doing for its own reasons.
#
# Keys: arrows = D-pad, X = a, z = b, s = x, a = y, q = l, w = R,
#       Tab held = fast-forward,
#       Return = Start, Backspace = Select, mouse on the lower screen = pen,
#       Esc quits. F11/Alt+Enter fullscreen, F12 screenshot.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# MODS= moves the compile-time plugin build into a directory of its own,
# and the save with it. PC_MODS= is the runtime content door, forwarded
# as KEY=VALUE below, because WSL does not pass the environment through.
# See pc/mods/README.md.
MODS="${MODS:-}"
PCBUILD="build/pc-win32"
[[ -n "$MODS" ]] && PCBUILD="build/pc-win32-mods"

ASPECT=""
SAVE="$PCBUILD/live.sav"
ROM="build/rom/pokeplatinum.us.nds"
FRESH=0
VIEWER_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --new)     FRESH=1; shift;;
        --save)    SAVE="$2"; shift 2;;
        --rom)     ROM="$2"; shift 2;;
        --wide)    VIEWER_ARGS+=(--layout wide); shift;;
        --aspect)  ASPECT="$2"; shift 2;;
        --scale|--render-scale|--filter)
                   VIEWER_ARGS+=("$1" "$2"); shift 2;;
        --integer|--stretch|--fullscreen)
                   VIEWER_ARGS+=("$1"); shift;;
        -h|--help) awk 'NR==1{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0;;
        *) printf 'play-win: unknown option %s (try --help)\n' "$1" >&2; exit 2;;
    esac
done

[[ -f $PCBUILD/pokeplatinum.exe ]] || {
    printf 'play-win: no exe, make -f pc/Makefile.win -j$(nproc)\n' >&2; exit 1; }
[[ -f $PCBUILD/pcview.exe ]] || {
    printf 'play-win: no viewer, make -f pc/Makefile.win viewer\n' >&2; exit 1; }
[[ -f "$ROM" ]] || { printf 'play-win: no ROM at %s\n' "$ROM" >&2; exit 1; }

# Paths must be RELATIVE (the exe's cwd is this repo over \\wsl.localhost).
case "$SAVE" in /*) SAVE="${SAVE#$ROOT/}";; esac
case "$ROM"  in /*) ROM="${ROM#$ROOT/}";;  esac
case "$SAVE" in /*) printf 'play-win: --save must live inside the repo\n' >&2; exit 1;; esac

[[ "$FRESH" -eq 1 ]] && rm -f "$SAVE"

# The EXE and the ROM are staged on ntfs, and this is worth more than the
# stale-image workaround it replaces. A Windows process reading this repo
# reads it over \\wsl.localhost, and the port reads its cartridge inside the
# frame that asks for it: measured over the new-game replay, one card read
# cost 20 to 57 ms from here and 0.08 ms from a native path. That is the
# whole difference between three frames in a hundred missing the 60 Hz
# deadline and almost none of them, and it was being read as the port's own
# jitter. The exe goes over for the same reason, 90 MB of it is paged in on
# demand as new code is first executed.
#
# The SAVE stays in the repo and so does the recording: the working directory
# is unchanged, and only the two read-only files move. If the native path is
# not there (no /mnt/c, a locked-down machine), the old behaviour stands;
# a slower session is better than no session.
N=$(date +%s)
STAGE_WIN=""
# The Windows user's own TEMP, asked for from /mnt/c so cmd.exe does not
# complain about a UNC working directory. Windows' own Temp if it will not say.
_tmp=$(cd /mnt/c 2>/dev/null && cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r\n')
_tmp=$(wslpath -u "$_tmp" 2>/dev/null || true)
[[ -d "${_tmp:-}" ]] || _tmp=/mnt/c/Windows/Temp
STAGE="$_tmp/pokeplatinum-pc"
if [[ -d "$_tmp" ]] && mkdir -p "$STAGE" 2>/dev/null; then
    cp "$PCBUILD/pokeplatinum.exe" "$STAGE/pp-$N.exe" 2>/dev/null &&
    STAGE_WIN=$(wslpath -w "$STAGE" 2>/dev/null) || STAGE_WIN=""
fi
if [[ -n "$STAGE_WIN" ]]; then
    # The ROM is 134 MB: copied only when it differs, not once a launch, and
    # under its own name so that --rom does not make two cartridges share one
    # staged file and pick by timestamp.
    _rom=$(basename "$ROM")
    if [[ ! -f "$STAGE/$_rom" || "$ROOT/$ROM" -nt "$STAGE/$_rom" ]]; then
        printf 'play-win: staging the ROM on %s (once per build)\n' "$STAGE_WIN"
        cp "$ROOT/$ROM" "$STAGE/$_rom"
    fi
    EXE="$STAGE/pp-$N.exe"
    # The VIEWER stays where it is: it presents a page the port already wrote,
    # so nothing it does is a card read, and it loads SDL2.dll out of its own
    # directory, moving it without the DLL is a viewer that will not start,
    # which is how this was found.
    VIEWER="./$PCBUILD/pcview.exe"
    ROM_ARG="$STAGE_WIN\\$_rom"
    ls -t "$STAGE"/pp-*.exe 2>/dev/null | tail -n +4 | xargs -r rm -f
else
    printf 'play-win: no native staging path, running off the WSL share,'
    printf ' which makes the cartridge slow\n'
    cp $PCBUILD/pokeplatinum.exe "$PCBUILD/pp-$N.exe"
    EXE="./$PCBUILD/pp-$N.exe"
    VIEWER="./$PCBUILD/pcview.exe"
    ROM_ARG="$ROM"
    ls -t $PCBUILD/pp-*.exe 2>/dev/null | tail -n +4 | xargs -r rm -f
fi

# Unique per launch, the way the launcher names its own channel. A session
# left running from an earlier day is still publishing under its name, and
# two ports writing one page is a torn picture and a recording of somebody
# else's game.
NAME="pplat-$$"
printf 'play-win: rom  %s\n' "$ROM_ARG"
printf 'play-win: save %s%s\n' "$SAVE" "$([[ -f "$SAVE" ]] && echo '' || echo ' (new)')"
printf 'play-win: X = A, Z = B, Return = Start, mouse = stylus, Esc quits\n'

REC="${SAVE%.sav}-session-$(date +%Y%m%d-%H%M%S).txt"
# The save as it was at launch, (start save, recording) is a replayable
# save state; see pc/play.sh's copy of this comment.
[[ -f "$SAVE" ]] && cp "$SAVE" "${REC%.txt}.sav.start"
# Content packages travel as argv. Paths must be repo-relative.
WIN_MODS=()
if [[ -n "${PC_MODS:-}" ]]; then
    WIN_MODS+=(PC_MODS="$PC_MODS")
fi
if [[ -n "${PC_MODS_DIR:-}" ]]; then
    _dir="$PC_MODS_DIR"
    case "$_dir" in /*) _dir="${_dir#$ROOT/}";; esac
    WIN_MODS+=(PC_MODS_DIR="$_dir")
fi
if [[ -n "${PC_MODFS:-}" ]]; then
    WIN_MODS+=(PC_MODFS="$PC_MODFS")
fi

printf 'play-win: recording the session to %s\n' "$REC"
"$EXE" PC_ROM="$ROM_ARG" PC_SAVE="$SAVE" PC_VIEW="$NAME" \
    PC_RECORD_INPUT="$REC" ${ASPECT:+PC_ASPECT="$ASPECT"} \
    ${WIN_MODS[@]+"${WIN_MODS[@]}"} &
PORT_PID=$!
trap '[[ -n "${PORT_PID:-}" ]] && kill $PORT_PID 2>/dev/null' EXIT INT TERM

# The viewer waits for the channel itself (attach-with-wait), so no sleep.
"$VIEWER" "$NAME" ${VIEWER_ARGS[@]+"${VIEWER_ARGS[@]}"}
