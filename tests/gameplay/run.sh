#!/bin/sh
# Platinum gameplay scenarios on the real core, headless.
#
#   tests/gameplay/run.sh [-o OUT] [scenario ...]    scripted scenarios (default: all)
#   tests/gameplay/run.sh [-o OUT] --soak [FRAMES]   seeded random input from field saves
#   tests/gameplay/run.sh [-o OUT] --perf            frames/second, render_scale 1 and 2
#
# OUT (default build/gameplay/out) gets one directory per scenario (the save,
# the run's log, the PPM shots and <name>.png, the contact sheet of those
# shots), and summary.txt, the pass/fail table. Exit status 1 if anything
# failed.
#
# A scenario is scenarios/<name>.scn, sourced here; it sets
#   RECIPE=recipes/x.recipe  start from a save minted from this lab recipe, or
#   FROM=<scenario>          start from the save another scenario left behind
#                            (run that one first), or neither: a blank chip,
#                            the real new-game route from the title screen
#   SCHEDULE=schedules/x.press   input (shell press format, shell/README.md)
#   FRAMES=N                 run length
#   SHOTS="F,F,..."          frames dumped into the contact sheet
#   EXPECT_MAP=N             map_id the run must end on
#   EXPECT_LOG='ERE'         must match the run's log (guest + status lines)
#   EXPECT_SAVE='expr'       Python expression over s, the np_save4 dump of the
#                            save the run ends with, that must be true
# Any DEFECT line from np_gp (trap, hang, VBlank stall, audio stall) fails it.
#
# Needs build/core-plat (cmake -S core -B build/core-plat with
# NP_GUEST_WASM_platinum, see core/CMakeLists.txt) and the ROM; builds np_gp
# and np_save4 itself. The ROM is never written to OUT.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
rom=${NP_ROM:-$root/games/platinum/build/rom/pokeplatinum.us.nds}
core=${NP_CORE_BUILD:-$root/build/core-plat}
out=$root/build/gameplay/out
mode=scenarios
soak_frames=100000

while [ $# -gt 0 ]; do
    case "$1" in
    -o) out=$2; shift 2 ;;
    --soak) mode=soak; shift; case "${1:-}" in [0-9]*) soak_frames=$1; shift ;; esac ;;
    --perf) mode=perf; shift ;;
    -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
    *) break ;;
    esac
done

[ -f "$rom" ] || { echo "run.sh: no ROM at $rom (set NP_ROM)" >&2; exit 2; }
[ -f "$core/libnp_guest_platinum.a" ] || { echo "run.sh: no Platinum core build in $core" >&2; exit 2; }

gp=$root/build/gameplay/np_gp
mkdir -p "$root/build/gameplay" "$out"
cc -O2 -std=c11 -Wall -I"$root/core/include" -I"$root/core/runtime" "$here/np_gp.c" \
    "$core/np_headless_np_registry.c" "$core/libnp_guest_platinum.a" "$core/libnp_runtime.a" \
    -lm -lpthread -o "$gp" || exit 2
if [ ! -x "$root/build/features/np_save4" ]; then
    { cmake -S "$root/features" -B "$root/build/features" -G Ninja &&
      cmake --build "$root/build/features" --target np_save4; } >/dev/null || exit 2
fi
save4=$root/build/features/np_save4
export NP_ROM="$rom" NP_GP="$gp"

# sheet DIR NAME: the PPM shots in DIR, in frame order, as DIR/NAME.png.
sheet() {
    set -- "$1" "$2" "$(ls "$1"/frame_*.ppm 2>/dev/null)"
    [ -n "$3" ] || return 0
    # shellcheck disable=SC2086
    montage $3 -tile 8x -geometry 256x384+2+2 -background '#202020' "$1/$2.png" 2>/dev/null ||
        echo "run.sh: montage (ImageMagick) failed, no contact sheet for $2" >&2
}

row() { printf '%-16s %-5s %8s %8s  %s\n' "$@" | tee -a "$out/summary.txt"; }

run_scenario() {
    name=$1
    RECIPE='' FROM='' SCHEDULE='' FRAMES='' SHOTS='' EXPECT_MAP='' EXPECT_LOG='' EXPECT_SAVE=''
    # shellcheck disable=SC1090
    . "$here/scenarios/$name.scn"
    d=$out/$name
    rm -rf "$d" && mkdir -p "$d"
    why=''
    if [ -n "$RECIPE" ]; then
        "$here/mint.sh" "$here/$RECIPE" "$d/start.sav" 2>"$d/mint.err" || why="mint failed"
    elif [ -n "$FROM" ]; then
        cp "$out/$FROM/end.sav" "$d/start.sav" 2>/dev/null || why="no save from $FROM (run it first)"
    fi
    [ -f "$d/start.sav" ] && cp "$d/start.sav" "$d/end.sav"
    if [ -z "$why" ]; then
        "$gp" "$rom" --frames "$FRAMES" --save "$d/end.sav" --schedule "$here/$SCHEDULE" \
            --dump "$d" --dump-at "$SHOTS" >"$d/run.out" 2>"$d/run.log"
        grep DEFECT "$d/run.out" >"$d/defects.txt"
        if [ -s "$d/defects.txt" ]; then why=$(head -1 "$d/defects.txt")
        elif ! grep -q "^frames $FRAMES " "$d/run.out"; then why="run ended early: $(tail -1 "$d/run.out")"
        fi
    fi
    if [ -z "$why" ] && [ -n "$EXPECT_MAP" ]; then
        map=$(sed -n 's/.* map \([0-9]*\)$/\1/p' "$d/run.out")
        [ "$map" = "$EXPECT_MAP" ] || why="ended on map $map, expected $EXPECT_MAP"
    fi
    if [ -z "$why" ] && [ -n "$EXPECT_LOG" ]; then
        grep -Eq "$EXPECT_LOG" "$d/run.log" || why="log lacks /$EXPECT_LOG/"
    fi
    if [ -z "$why" ] && [ -n "$EXPECT_SAVE" ]; then
        "$save4" dump "$rom" "$d/end.sav" >"$d/end.json" 2>&1 || why="np_save4 cannot read the save"
        [ -z "$why" ] && why=$(python3 - "$d/end.json" "$EXPECT_SAVE" <<'EOF'
import json, sys
s = json.load(open(sys.argv[1]))
try:
    ok = eval(sys.argv[2], {"s": s})
except Exception as e:  # a missing key is a failed expectation, said as such
    ok, err = False, " (%s: %s)" % (type(e).__name__, e)
else:
    err = ""
print("" if ok else "save: not (%s)%s" % (sys.argv[2], err))
EOF
)
    fi
    sheet "$d" "$name"
    fps=$(sed -n 's/.* fps \([0-9.]*\) .*/\1/p' "$d/run.out" 2>/dev/null)
    if [ -z "$why" ]; then row "$name" PASS "$FRAMES" "${fps:--}" "$d/$name.png"; return 0; fi
    row "$name" FAIL "${FRAMES:--}" "${fps:--}" "$why"
    return 1
}

# The soak: seeded random input from field saves, each run reproducible from
# its (save, seed) pair; every trap/hang/stall line names seed and frame.
# SOAK_SEEDS (default "1") lists the seeds run on each save.
run_soak() {
    printf '%-14s %6s %8s %7s  %s\n' save seed frames fps result | tee -a "$out/summary.txt"
    rc=0
    for recipe in sandgem roark gate; do
        d=$out/soak-$recipe
        rm -rf "$d" && mkdir -p "$d"
        "$here/mint.sh" "$here/recipes/$recipe.recipe" "$d/start.sav" 2>"$d/mint.err" || { echo "soak: mint $recipe failed"; rc=1; continue; }
        for seed in ${SOAK_SEEDS:-1}; do
            cp "$d/start.sav" "$d/seed$seed.sav"
            # 1250-1600: title, CONTINUE; random from 1800.
            "$gp" "$rom" --frames "$soak_frames" --save "$d/seed$seed.sav" --schedule "$here/schedules/continue.press" \
                --random "$seed" --random-from 1800 --dump "$d" --dump-every $((soak_frames / 8)) --dump-from 1800 \
                >"$d/seed$seed.out" 2>"$d/seed$seed.log"
            r=$(grep -c DEFECT "$d/seed$seed.out")
            fps=$(sed -n 's/.* fps \([0-9.]*\) .*/\1/p' "$d/seed$seed.out")
            n=$(sed -n 's/^frames \([0-9]*\) .*/\1/p' "$d/seed$seed.out")
            if [ "$r" = 0 ] && [ "$n" = "$soak_frames" ]; then res="clean, $(sed -n 's/.*\(static-max [^ ]*\).*/\1/p' "$d/seed$seed.out")"
            else res=$(grep DEFECT "$d/seed$seed.out" | head -3 | tr '\n' ' '); [ -n "$res" ] || res="ended at frame $n"; rc=1; fi
            printf '%-14s %6s %8s %7s  %s\n' "$recipe" "$seed" "${n:-0}" "${fps:--}" "$res" | tee -a "$out/summary.txt"
        done
        sheet "$d" "soak-$recipe"
    done
    return $rc
}

# Frames per second with no dumps: field walking, a battle and a 3D-heavy
# scene, each at render_scale 1 and 2; the time is taken from the frame the
# scene is up (the boot and CONTINUE are not timed).
run_perf() {
    printf '%-10s %-6s %8s %10s\n' scene scale fps ms/frame | tee -a "$out/summary.txt"
    rc=0
    for scene in field battle 3d; do
        case $scene in
        field)  recipe=sandgem;  sched=perf-field.press;  from=1800; frames=5400 ;;
        battle) recipe=roark;    sched=perf-battle.press; from=2100; frames=3800 ;;
        3d)     recipe=jubilife; sched=perf-3d.press;     from=1800; frames=5400 ;;
        esac
        d=$out/perf-$scene
        rm -rf "$d" && mkdir -p "$d"
        "$here/mint.sh" "$here/recipes/$recipe.recipe" "$d/start.sav" 2>"$d/mint.err" || { echo "perf: mint $recipe failed"; rc=1; continue; }
        for scale in 1 2; do
            cp "$d/start.sav" "$d/s$scale.sav"
            "$gp" "$rom" --frames "$frames" --save "$d/s$scale.sav" --schedule "$here/schedules/$sched" \
                -o "render_scale=$scale" --time-from "$from" >"$d/s$scale.out" 2>"$d/s$scale.log"
            grep -q DEFECT "$d/s$scale.out" && rc=1
            printf '%-10s %-6s %8s %10s\n' "$scene" "$scale" \
                "$(sed -n 's/.* fps \([0-9.]*\) .*/\1/p' "$d/s$scale.out")" \
                "$(sed -n 's/.* ms\/frame \([0-9.]*\) .*/\1/p' "$d/s$scale.out")" | tee -a "$out/summary.txt"
        done
    done
    return $rc
}

: >"$out/summary.txt"
case $mode in
soak) run_soak; exit $? ;;
perf) run_perf; exit $? ;;
esac

[ $# -gt 0 ] || set -- $(cd "$here/scenarios" && ls *.scn | sed 's/\.scn$//')
row scenario result frames fps "contact sheet / reason"
fail=0
for s in "$@"; do run_scenario "$s" || fail=1; done
exit $fail
