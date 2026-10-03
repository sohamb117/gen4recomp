#!/usr/bin/env bash
#
# 3ds/tests/azahar_shot.sh: boot a .3dsx in the emulator, drive the buttons,
# keep the frames.
#
#   3ds/tests/azahar_shot.sh build/3ds/pokeplatinum.3dsx out.png
#   INPUT='press:a wait:1 shot:a.png release:a hold:m:1 wait:3 shot:exit.png' \
#       3ds/tests/azahar_shot.sh build/3ds/pokeplatinum.3dsx
#
# INPUT is a whitespace-separated program run after the warm-up, in order:
#
#   wait:<seconds>          let it run
#   hold:<key>:<seconds>    press a key, hold it, release it
#   press:<key>             press and leave held
#   release:<key>           let it up
#   tap:<key>               press and release; see the warning below
#   shot:<path>             grab the window
#
# Keys are X keysyms and the emulator's default keyboard map turns them into
# buttons: a=A, s=B, z=X, x=Y, q=L, w=R, m=Start, n=Select, t/g/f/h for the
# D-pad.
#
# Hold, do not tap. The emulated game polls HID once a frame; xdotool's tap is
# about 12 ms and 60 Hz frames are 16.7 ms apart, so a tap is regularly missed.
# Measured: `tap:m` left the port running, `hold:m:1` ended it.
#
# Why this runs its own X server. Under a rootless Xwayland with no window
# manager, xdotool's windowactivate has no _NET_ACTIVE_WINDOW to set,
# windowfocus errors out of XSetInputFocus, Qt ignores the XSendEvent form of a
# keystroke, and grabbing the X root gives a black screen. So the emulator is
# given a private Xvfb, where XSetInputFocus works and a window grab has
# something in it.
#
# Xvfb needs two things an unprivileged user may not have: a writable
# /tmp/.X11-unix and /usr/bin/xkbcomp, whose path is compiled into the X
# server. Both are solved without root by re-running this script inside a user
# namespace, where a tmpfs and an overlay over /usr/bin can be mounted. If that
# is refused, the script falls back to the desktop's own display: screenshots
# still work, input does not.
#
# Environment, because none of this is a system package:
#
#   AZAHAR         the AppImage
#   XVFB_PREFIX    Xvfb and xkbcomp, unpacked into a user prefix
#   XDOTOOL_PREFIX xdotool, likewise
#   GLVND_PREFIX   holds libOpenGL.so.0, which the AppImage needs
#   WARMUP         seconds before the input program starts (default 12)
#   DISPLAY_NUM    Xvfb display number (default 99)
#
# Exit status is 0 only if every shot came out.

set -u

AZAHAR=${AZAHAR:-$HOME/opt/azahar/azahar.AppImage}
XVFB_PREFIX=${XVFB_PREFIX:-$HOME/opt/xvfb/root/usr}
XDOTOOL_PREFIX=${XDOTOOL_PREFIX:-$HOME/opt/xdotool/root/usr}
GLVND_PREFIX=${GLVND_PREFIX:-$HOME/opt/glvnd/root/usr}
WARMUP=${WARMUP:-12}
DISPLAY_NUM=${DISPLAY_NUM:-99}

app=${1:-}
out=${2:-}
INPUT=${INPUT:-}

if [ -z "$app" ] || { [ -z "$out" ] && [ -z "$INPUT" ]; }; then
    echo "usage: $0 <file.3dsx> <out.png>   (or INPUT=... $0 <file.3dsx>)" >&2
    exit 2
fi
if [ -n "$out" ]; then
    INPUT="$INPUT shot:$out"
fi

lib=$XVFB_PREFIX/lib/x86_64-linux-gnu:$XDOTOOL_PREFIX/lib/x86_64-linux-gnu:$GLVND_PREFIX/lib/x86_64-linux-gnu
export LD_LIBRARY_PATH="$lib:${LD_LIBRARY_PATH:-}"
export PATH="$XVFB_PREFIX/bin:$XDOTOOL_PREFIX/bin:$PATH"
export QT_QPA_PLATFORM=xcb

# ------------------------------------------------------------------
# Re-run inside a user namespace, once, and set up the private display.
# ------------------------------------------------------------------
if [ "${AZAHAR_SHOT_NS:-0}" != "1" ] && [ -x "$XVFB_PREFIX/bin/Xvfb" ]; then
    if unshare --map-root-user --mount true 2> /dev/null; then
        export AZAHAR_SHOT_NS=1
        exec unshare --map-root-user --mount "$0" "$@"
    fi
    echo "azahar_shot: no user namespace; falling back to \$DISPLAY (no input)" >&2
fi

# A leaked x server is the same trap as a leaked emulator, and worse: the next
# run cannot own the display, so the emulator aborts at startup and the message
# is "private X server did not start"; which reads like a broken machine. A
# run killed hard leaves one behind, and six checks failed on one before it was
# found. Cleared HERE, before anything starts a server of our own, so this can
# only ever be ending a previous run's. Matched on the display number, so a
# desktop X server is never a candidate.
if pgrep -f "[X]vfb :$DISPLAY_NUM " > /dev/null 2>&1; then
    echo "azahar_shot: an X server from an earlier run is still up; ending it" >&2
    for p in $(pgrep -f "[X]vfb :$DISPLAY_NUM "); do kill -9 "$p" 2>/dev/null; done
    rm -f "/tmp/.X$DISPLAY_NUM-lock"
    sleep 1
fi

private_x=0
if [ "${AZAHAR_SHOT_NS:-0}" = "1" ]; then
    ovl=$(mktemp -d)
    mkdir -p "$ovl/upper" "$ovl/work"
    if mount -t overlay overlay \
             -o "lowerdir=/usr/bin,upperdir=$ovl/upper,workdir=$ovl/work" /usr/bin \
       && cp "$XVFB_PREFIX/bin/xkbcomp" /usr/bin/xkbcomp \
       && mount -t tmpfs -o mode=1777 tmpfs /tmp/.X11-unix; then
        rm -f "/tmp/.X$DISPLAY_NUM-lock"
        Xvfb ":$DISPLAY_NUM" -screen 0 1400x900x24 -nolisten tcp > /dev/null 2>&1 &
        for _ in $(seq 1 20); do
            if [ -e "/tmp/.X11-unix/X$DISPLAY_NUM" ]; then
                private_x=1
                break
            fi
            sleep 1
        done
    fi
    if [ "$private_x" = "1" ]; then
        export DISPLAY=":$DISPLAY_NUM"
        unset WAYLAND_DISPLAY
    else
        echo "azahar_shot: private X server did not start; input will not work" >&2
    fi
fi

# ------------------------------------------------------------------
for f in "$AZAHAR" "$app"; do
    if [ ! -e "$f" ]; then
        echo "azahar_shot: missing $f" >&2
        exit 2
    fi
done
if ! command -v ffmpeg > /dev/null; then
    echo "azahar_shot: no ffmpeg for the window grab" >&2
    exit 2
fi

# A leftover run is silent and fatal. The AppImage is a wrapper: killing it
# leaves AppRun.wrapped behind, and that survivor keeps the private display
# and the SD card. The next run then reports "private X server did not start",
# which reads like a broken machine and is actually the previous run still
# holding the door. Say so and clear it, rather than failing obscurely.
if pgrep -x AppRun.wrapped > /dev/null 2>&1; then
    echo "azahar_shot: an emulator from an earlier run is still up; ending it" >&2
    pkill -x AppRun.wrapped 2>/dev/null
    sleep 2
    pkill -9 -x AppRun.wrapped 2>/dev/null
fi

# And tear our own down however this script ends, not only when it ends well.
# Without this a timeout or an interrupt leaks both the emulator and the X
# server, and the leak is only discovered by the run after next.
cleanup() {
    [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null
    pkill -x AppRun.wrapped 2>/dev/null
    if [ "${private_x:-0}" = "1" ]; then
        pkill -x Xvfb 2>/dev/null
    fi
    if [ -n "${speed_saved:-}" ]; then
        sed -i "s/^frame_limit=.*/frame_limit=$speed_saved/; \
                s/^frame_limit\\\\default=.*/frame_limit\\\\default=$speed_saved_flag/" \
            "$AZAHAR_CONFIG"
    fi
}
trap cleanup EXIT INT TERM

# SPEED: run the console faster than a console, for anything that is not a
# picture taken at a wall-clock moment. The emulator's frame limiter is a
# percentage of real console speed and it is the binding constraint on the
# light scenes, measured 2026-08-17: at 1000 the emulator reports 568% and
# the game presents 19 FPS instead of 6, and 70 seconds of wall clock reach
# further into the boot than 200 seconds do at 100.
#
# It does not move what the port measures. The frame times in
# perf-report.txt are ARM11 system ticks, which advance with emulated time,
# so the same windows come out within 0.6 ms and every picture digest is
# unchanged. Every run here should use it, the gate included, measured, it
# passes fast-forwarded and its sound check covers three times the frames in
# the same seconds. The one thing to keep in mind is that a shot taken at a
# wall-clock offset lands somewhere else in the boot.
#
# `key\default=true` means "this is the default, rewrite it", Azahar
# ignores a value left with that flag set, which is how an edit to this file
# can look like it did nothing. Both lines move together.
AZAHAR_CONFIG=${AZAHAR_CONFIG:-$HOME/.config/azahar-emu/qt-config.ini}
if [ -n "${SPEED:-}" ] && [ -f "$AZAHAR_CONFIG" ]; then
    speed_saved=$(sed -n 's/^frame_limit=//p' "$AZAHAR_CONFIG" | head -1)
    speed_saved_flag=$(sed -n 's/^frame_limit\\default=//p' "$AZAHAR_CONFIG" | head -1)
    sed -i "s/^frame_limit=.*/frame_limit=$SPEED/; \
            s/^frame_limit\\\\default=.*/frame_limit\\\\default=false/" \
        "$AZAHAR_CONFIG"
fi

"$AZAHAR" -w "$app" > /dev/null 2>&1 &
pid=$!

win=
for _ in $(seq 1 40); do
    sleep 1
    win=$(xdotool search --name '^Azahar [0-9]' 2>/dev/null | tail -1)
    if [ -n "$win" ]; then
        break
    fi
done
if [ -z "$win" ]; then
    echo "azahar_shot: no emulator window appeared" >&2
    exit 1
fi

sleep "$WARMUP"
xdotool windowfocus --sync "$win" 2>/dev/null

grab() {
    eval "$(xdotool getwindowgeometry --shell "$win")"
    mkdir -p "$(dirname "$1")"
    ffmpeg -hide_banner -loglevel error -f x11grab -window_id "$win" \
           -video_size "${WIDTH}x${HEIGHT}" -i "$DISPLAY" \
           -frames:v 1 -update 1 -y "$1"
    if [ -s "$1" ]; then
        echo "  $1 (${WIDTH}x${HEIGHT})"
        return 0
    fi
    echo "azahar_shot: no frame in $1" >&2
    return 1
}

rc=0
for step in $INPUT; do
    verb=${step%%:*}
    rest=${step#*:}
    case $verb in
        wait) sleep "$rest" ;;
        hold)
            key=${rest%%:*}
            secs=${rest#*:}
            xdotool keydown "$key"
            sleep "$secs"
            xdotool keyup "$key"
            ;;
        press) xdotool keydown "$rest" ;;
        release) xdotool keyup "$rest" ;;
        tap) xdotool key "$rest" ;;
        shot) grab "$rest" || rc=1 ;;
        *)
            echo "azahar_shot: bad step '$step'" >&2
            rc=2
            ;;
    esac
done

# The trap does the teardown, on this path and on every other one.
exit "$rc"
