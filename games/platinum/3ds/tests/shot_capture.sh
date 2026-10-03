#!/usr/bin/env bash
#
# 3ds/tests/shot_capture.sh: photograph a frame of the running game.
#
#   3ds/tests/shot_capture.sh build/3ds/pokeplatinum.3dsx out.png
#
# Writes out.png and prints one line describing what is on the two screens,
# short enough for a MANIFEST.txt row. Exit 0 if a picture came out, 1 if the
# screens are black, 2 if the capture failed or the console never came up.
#
# One boot, two shots. The first is the diagnostic screen the crt draws before
# anything else; the second is taken after A hands the console to the game.
# The diagnostic shot is not evidence for its own sake; it is the ruler.
# shot_verdict.py reads its corner block, so a console whose self-tests failed
# never gets photographed as a game frame, and shot_frame.py takes the two
# panel rectangles out of it in window coordinates so the game shot can be
# measured where the picture actually is. Guessing that from the game shot
# alone does not work: the frame's letterbox is black and so is the surround.
#
#   GAME_WAIT   seconds to let the game run after A (default 20)
#   GAME_AGAIN  seconds from the first game shot to the second (default 45)
#   SHOT_DIAG   keep the diagnostic shot here as well (default: discard it)
#   WARMUP      passed through to azahar_shot.sh (default 12 there)
#
# 20 seconds because the emulated console runs the game at about 10 FPS and
# the copyright screen is up well inside that; it was 40 while the port ran at
# 5. Both numbers are wall time on this machine rather than anything the game
# counts, which is why the second shot exists rather than a better first one.

set -u

here=$(dirname "$0")
app=${1:-}
out=${2:-}
GAME_WAIT=${GAME_WAIT:-20}
GAME_AGAIN=${GAME_AGAIN:-45}

if [ -z "$app" ] || [ -z "$out" ]; then
    echo "usage: $0 <file.3dsx> <out.png>" >&2
    exit 2
fi

tmp=$(mktemp -d /tmp/3ds_shot_capture.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# Two game shots, not one. A fade is a real frame and it is black, and the
# boot walks through several of them: the single grab at 40 seconds landed on
# one the day the port got fast enough to be past the copyright screen by
# then, and reported a working renderer as a dead one. The second grab is far
# enough after the first to be a different scene.
if ! err=$(INPUT="wait:3 shot:$tmp/diag.png hold:a:1 wait:$GAME_WAIT \
                  shot:$tmp/game.png wait:$GAME_AGAIN shot:$tmp/game2.png" \
           "$here/azahar_shot.sh" "$app" 2>&1); then
    echo "shot_capture: the emulator did not give up both frames: $err" >&2
    exit 2
fi

# Kept before the verdict, not after: a caller checking the console's own
# self-tests wants the shot most when it is the failing one.
if [ -n "${SHOT_DIAG:-}" ]; then
    mkdir -p "$(dirname "$SHOT_DIAG")"
    cp "$tmp/diag.png" "$SHOT_DIAG"
fi

if ! verdict=$("$here/shot_verdict.py" "$tmp/diag.png" 2>&1); then
    echo "shot_capture: not photographing a game frame over a console that is "\
"not passing: $verdict" >&2
    exit 2
fi

shot=$tmp/game.png
line=$("$here/shot_frame.py" "$tmp/diag.png" "$shot" 2>&1)
rc=$?
if [ "$rc" != "0" ]; then
    first=$line
    shot=$tmp/game2.png
    line=$("$here/shot_frame.py" "$tmp/diag.png" "$shot" 2>&1)
    rc=$?
    if [ "$rc" != "0" ]; then
        echo "$first" >&2
        echo "$line" >&2
        exit "$rc"
    fi
fi

mkdir -p "$(dirname "$out")"
cp "$shot" "$out"
echo "$line"
