#!/usr/bin/env bash
# tests/rse/first_battle.sh [GAME...]: Ruby, Sapphire and Emerald (default
# all three) through the game's own save headless, with np_headless from
# $NP_RSE_CORE (default build/core-rse) and the decomp ROMs in .cache/gba.
#
# Per game:
#   quicksave  littleroot.sched to the house 1F, quick save at 10000
#              (quicksave_result 1, map 256, field_ready 1)
#   continue   that save: title, CONTINUE, back in the house (map 256)
#   state      --state-test from the continued game: snapshot round trips
#   battle     three legs, each from the previous leg's quick save:
#              <p>-1-home (clock, TV, out of the house), <p>-2-rival (the
#              rival's house), <p>-3-route101 (Birch, the starter, the
#              wild battle won: in_battle 0 -> 1 -> 0; R/S also reach the
#              lab, map 260)
#
# Work files go to build/rse/first_battle/<game> (outside git). Exit status:
# 0 every check passed (games whose ROM is missing are SKIPped), 1 a check
# failed, 2 usage.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
hl=${NP_RSE_CORE:-build/core-rse}/np_headless
games=("$@")
[ ${#games[@]} -eq 0 ] && games=(ruby sapphire emerald)
[ -x "$hl" ] || { echo "first_battle: no $hl (build the RSE core, or set NP_RSE_CORE)" >&2; exit 2; }

fail=0
check() { # NAME LOG PATTERN...: every pattern must occur in LOG
    local name=$1 log=$2 p
    shift 2
    for p in "$@"; do
        if ! grep -q -- "$p" "$log"; then
            echo "FAIL $name: no \"$p\" (log $log)"
            fail=1
            return 1
        fi
    done
    echo "ok   $name"
}

for g in "${games[@]}"; do
    case $g in
    ruby | sapphire) rom=.cache/gba/pokeruby/poke$g.gba p=rs legs=("4720 4700" "5222 5215" "5600 0") ;;
    emerald) rom=.cache/gba/pokeemerald/pokeemerald.gba p=e legs=("5070 5050" "5322 5320" "6200 0") ;;
    *) echo "first_battle: unknown game $g" >&2; exit 2 ;;
    esac
    if [ ! -f "$rom" ]; then
        echo "SKIP $g (no $rom)"
        continue
    fi
    w=build/rse/first_battle/$g
    rm -rf "$w" && mkdir -p "$w"
    run() { # LOG SAVE ARGS...
        local log=$1 sav=$2
        shift 2
        "$hl" $g "$rom" --save "$sav" "$@" > "$log" 2>&1
        echo "exit $?" >> "$log"
    }

    run "$w/quicksave.log" "$w/house.sav" --frames 11000 --schedule tests/rse/littleroot.sched \
        -o 10000:quicksave_seq=1
    check "$g quicksave" "$w/quicksave.log" "quicksave_result=1 map_id=256" "field_ready=1" "^exit 0"

    cp "$w/house.sav" "$w/continue.sav"
    run "$w/continue.log" "$w/continue.sav" --frames 1600 --schedule tests/rse/$p-1-home.sched
    check "$g continue" "$w/continue.log" "map_id 0 -> 256" "field_ready 0 -> 1" "^exit 0"

    cp "$w/house.sav" "$w/state.sav"
    run "$w/state.log" "$w/state.sav" --schedule tests/rse/$p-1-home.sched --state-test 1200
    check "$g state" "$w/state.log" "state round 3: .* ok" "^exit 0"

    prev=$w/house.sav
    for i in 1 2 3; do
        set -- ${legs[$((i - 1))]}
        leg=$(ls tests/rse/$p-$i-*.sched)
        cp "$prev" "$w/leg$i.sav"
        qs=()
        [ "$2" != 0 ] && qs=(-o "$2:quicksave_seq=1")
        run "$w/leg$i.log" "$w/leg$i.sav" --frames "$1" --schedule "$leg" ${qs[@]+"${qs[@]}"}
        case $i in
        3)
            pats=("in_battle 0 -> 1" "in_battle 1 -> 0" "^exit 0")
            [ $p = rs ] && pats+=("map_id 16 -> 260")
            check "$g battle (leg 3, $(basename "$leg"))" "$w/leg$i.log" "${pats[@]}" ;;
        *) check "$g leg $i ($(basename "$leg"))" "$w/leg$i.log" "quicksave_result=1" "field_ready=1" "^exit 0" ;;
        esac || break
        prev=$w/leg$i.sav
    done
done
exit $fail
