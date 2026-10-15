#!/bin/sh
# End-to-end progression check for the Diamond/Pearl cores:
#
#  1. From power-on, play a new game to the first in-game save
#     (tests/dp/<game>_first_save.sched) with the backup chip in a file.
#  2. Check the game stored a 512 KiB save and np_save4 reads the trainer
#     name back out of it.
#  3. Boot again on that save, pick CONTINUE, and save once more from the
#     start menu (tests/dp/continue.sched): the core must load the chip, and
#     the new save must keep the name and carry a higher save counter.
#
#   tests/dp/first_save.sh [diamond] [pearl]       (default: both)
#
# Environment: NP_HEADLESS (default build/core-dp/np_headless), NP_SAVE4
# (default: build/features/np_save4, configured and rebuilt from this
# checkout's features/ on every run, so its JSON matches the counter_of
# reader below), OUT (default build/tests/dp-first-save; the last frame of
# every run is kept there as PPM for inspection).
# A game whose ROM (games/diamond/build/<game>.us/poke<game>.us.nds) or core
# is missing is skipped; exit 77 when every requested game was skipped.
# Each .sched names its frame count in a "# frames: N" line.
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$ROOT/tests/dp"
HEADLESS=${NP_HEADLESS:-$ROOT/build/core-dp/np_headless}
if [ -n "${NP_SAVE4:-}" ]; then
    SAVE4=$NP_SAVE4
else
    SAVE4=$ROOT/build/features/np_save4
    { [ -f "$ROOT/build/features/build.ninja" ] ||
        cmake -S "$ROOT/features" -B "$ROOT/build/features" -G Ninja; } >/dev/null &&
        cmake --build "$ROOT/build/features" --target np_save4 >/dev/null ||
        { echo "FAIL: cannot build np_save4 from $ROOT/features"; exit 1; }
fi
OUT=${OUT:-$ROOT/build/tests/dp-first-save}
NAME=NATIVE

fail() { echo "FAIL: $*"; exit 1; }
frames_of() { sed -n 's/^# frames: *\([0-9][0-9]*\).*/\1/p' "$1"; }
# The general block's newest (save counter, block counter) from an np_save4
# dump, over the copies whose checksum holds (a first save leaves the other
# copy erased), as one number: the save counter only moves when the large
# (storage) block changed, the block counter moves on every save.
counter_of() {
    python3 -c 'import json, sys
g = [b for b in json.load(open(sys.argv[1]))["blocks"] if b["name"] == "general"][0]
print(max(s * 2**32 + k for s, k, v in zip(g["save_counter"], g["block_counter"], g["valid"]) if v))' "$1"
}

[ -x "$SAVE4" ] || fail "np_save4 not found at $SAVE4"
mkdir -p "$OUT"
games=${*:-diamond pearl}
ran=0
for g in $games; do
    rom="$ROOT/games/diamond/build/$g.us/poke$g.us.nds"
    if [ ! -f "$rom" ]; then
        echo "SKIP $g: no ROM at $rom"
        continue
    fi
    if [ ! -x "$HEADLESS" ] || "$HEADLESS" "$g" "$rom" --frames 0 2>&1 | grep -q 'not built into'; then
        echo "SKIP $g: $HEADLESS has no $g core"
        continue
    fi
    sav="$OUT/$g.sav"
    rm -f "$sav"

    # 1-2: new game to the first save.
    sched="$HERE/${g}_first_save.sched"
    dir="$OUT/$g-new-game"
    rm -rf "$dir" && mkdir -p "$dir"
    "$HEADLESS" "$g" "$rom" --frames "$(frames_of "$sched")" --save "$sav" \
        --schedule "$sched" --dump "$dir" >"$OUT/$g-new-game.log" 2>&1 ||
        fail "$g new game: np_headless failed (see $OUT/$g-new-game.log)"
    grep -q 'stored 524288-byte save' "$OUT/$g-new-game.log" ||
        fail "$g new game: the game never stored its save"
    "$SAVE4" dump "$rom" "$sav" >"$OUT/$g-new-game.json" ||
        fail "$g new game: np_save4 cannot read $sav"
    grep -q "\"trainer\": {\"name\": \"$NAME\"" "$OUT/$g-new-game.json" ||
        fail "$g new game: trainer name is not $NAME ($OUT/$g-new-game.json)"
    c1=$(counter_of "$OUT/$g-new-game.json")
    echo "$g new game: saved as $NAME (save counter $c1)"

    # 3: CONTINUE on that save and save again.
    sched="$HERE/continue.sched"
    dir="$OUT/$g-continue"
    rm -rf "$dir" && mkdir -p "$dir"
    "$HEADLESS" "$g" "$rom" --frames "$(frames_of "$sched")" --save "$sav" \
        --schedule "$sched" --dump "$dir" >"$OUT/$g-continue.log" 2>&1 ||
        fail "$g continue: np_headless failed (see $OUT/$g-continue.log)"
    grep -q 'save: loaded from the runtime' "$OUT/$g-continue.log" ||
        fail "$g continue: the core did not load $sav"
    grep -q 'stored 524288-byte save' "$OUT/$g-continue.log" ||
        fail "$g continue: no save after CONTINUE"
    "$SAVE4" dump "$rom" "$sav" >"$OUT/$g-continue.json" ||
        fail "$g continue: np_save4 cannot read $sav"
    grep -q "\"trainer\": {\"name\": \"$NAME\"" "$OUT/$g-continue.json" ||
        fail "$g continue: trainer name is not $NAME"
    c2=$(counter_of "$OUT/$g-continue.json")
    [ "$c2" -gt "$c1" ] || fail "$g continue: save counter $c2 is not above $c1"
    echo "$g continue: loaded, saved again (save counter $c2)"
    ran=$((ran + 1))
done
[ "$ran" -gt 0 ] || exit 77
echo "PASS: $ran game(s)"
