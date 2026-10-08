#!/usr/bin/env bash
# tests/bwhgss/parity.sh [GAME...]: the feature-parity checks Black, White,
# HeartGold and SoulSilver (default all four) can pass today, headless, with
# np_headless from $NP_BW_CORE (Black/White core) and $NP_HGSS_CORE
# (HeartGold/SoulSilver core) and the ROMs in $NP_BLACK_ROM, $NP_WHITE_ROM,
# $NP_HG_ROM, $NP_SS_ROM.
#
# Black / White:
#   title     boot to the title: ROM-derived music (audio rms), snapshot round
#             trips (--state-test) at the title
#   save      bw-save.sched: NEW GAME to the controllable bedroom, snapshot
#             round trips there, then the X menu SAVE: the game's own save,
#             which np_save5 must verify (both copies, every CRC)
#   continue  bw-continue.sched from that save: title, CONTINUE, the bedroom
# HeartGold / SoulSilver (the field does not load yet, docs/HANDOFF-hgss.md):
#   intro     hgss-intro.sched: title, the touch-screen tutorial driven by
#             stylus taps, Prof. Oak, the boy, the default name accepted;
#             snapshot round trips at the title and in Oak's introduction;
#             the audio level is reported, not checked: the core is silent
#             through the title and intro but for one sound near frame 8400
#
# Frames are dumped as PNGs to build/evidence/bwhgss/<game>/ (outside git).
# Exit status: 0 every check passed (games without core or ROM are SKIPped),
# 1 a check failed, 2 usage.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
here=tests/bwhgss
save5=${NP_SAVE5:-build/features/np_save5}
games=("$@")
[ ${#games[@]} -eq 0 ] && games=(black white heartgold soulsilver)

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
# The state test's rounds all replayed identically, and the run ended well.
state_ok() { # NAME LOG ROUNDS
    local n
    n=$(grep -c "^state round .* ok$" "$2")
    if [ "$n" -ne "$3" ] || grep -q "^FAILED\|mismatch" "$2"; then
        echo "FAIL $1: $n of $3 snapshot rounds replayed identically (log $2)"
        fail=1
        return 1
    fi
    echo "ok   $1 ($n snapshot round trips)"
}
loud() { # NAME LOG: nonzero audio from the ROM's sound data
    local l
    l=$(sed -n 's/^audio rms from frame [0-9]*: L \([0-9]*\).*/\1/p' "$2")
    if [ -z "$l" ] || [ "$l" -lt 100 ]; then
        echo "FAIL $1: audio rms \"$l\" (log $2)"
        fail=1
        return 1
    fi
    echo "ok   $1 (audio rms L $l)"
}
pngs() { # DIR: the dumped frames as PNGs
    local f
    for f in "$1"/*.ppm; do
        [ -f "$f" ] || continue
        sips -s format png "$f" --out "${f%.ppm}.png" > /dev/null 2>&1 && rm -f "$f"
    done
}

for g in "${games[@]}"; do
    case $g in
    black) hl=${NP_BW_CORE:-}/np_headless rom=${NP_BLACK_ROM:-} ;;
    white) hl=${NP_BW_CORE:-}/np_headless rom=${NP_WHITE_ROM:-} ;;
    heartgold) hl=${NP_HGSS_CORE:-}/np_headless rom=${NP_HG_ROM:-} ;;
    soulsilver) hl=${NP_HGSS_CORE:-}/np_headless rom=${NP_SS_ROM:-} ;;
    *) echo "parity: unknown game $g" >&2; exit 2 ;;
    esac
    [ -n "${NP_HEADLESS:-}" ] && hl=$NP_HEADLESS
    if [ ! -x "$hl" ] || [ ! -f "$rom" ]; then
        echo "SKIP $g (core $hl, ROM '$rom')"
        continue
    fi
    w=build/evidence/bwhgss/$g
    rm -rf "$w" && mkdir -p "$w"
    run() { # STEP ARGS...: one np_headless run, its frames in $w/<step>/
        local step=$1
        shift
        mkdir -p "$w/$step"
        "$hl" $g "$rom" --dump "$w/$step" "$@" > "$w/$step.log" 2>&1
        echo "exit $?" >> "$w/$step.log"
        pngs "$w/$step"
    }
    case $g in
    black | white)
        run title --frames 5400 --dump-from 4800 --dump-every 600 --rms-from 4000 \
            --state-test 4800 --state-span 120 --state-rounds 3
        check "$g title" "$w/title.log" "exit 0"
        state_ok "$g title snapshots" "$w/title.log" 3
        loud "$g title music" "$w/title.log"

        run save --frames 23200 --schedule $here/bw-save.sched --save "$w/game.sav" \
            --dump-from 21400 --dump-every 300 --state-test 21500 --state-span 120 --state-rounds 3
        check "$g new game to the bedroom, in-game save" "$w/save.log" "exit 0"
        state_ok "$g bedroom snapshots" "$w/save.log" 3
        if [ -x "$save5" ]; then
            "$save5" verify "$w/game.sav" > "$w/verify.log" 2>&1
            echo "exit $?" >> "$w/verify.log"
            check "$g save verifies (np_save5)" "$w/verify.log" "exit 0" "all checksums valid"
            "$save5" dump "$w/game.sav" > "$w/dump.json" 2>&1
        else
            echo "note $g: no $save5 (set NP_SAVE5); save not verified"
        fi

        cp "$w/game.sav" "$w/continue.sav"
        run continue --frames 7000 --schedule $here/bw-continue.sched --save "$w/continue.sav" \
            --dump-from 5000 --dump-every 1000
        check "$g CONTINUE to the bedroom" "$w/continue.log" "exit 0"
        ;;
    heartgold | soulsilver)
        run intro --frames 13900 --schedule $here/hgss-intro.sched --dump-from 1600 --dump-every 1500 \
            --rms-from 1300 --state-test 6000 --state-span 120 --state-rounds 3
        check "$g title, touch tutorial, Oak, naming" "$w/intro.log" "exit 0"
        state_ok "$g intro snapshots" "$w/intro.log" 3
        echo "note $g: $(grep '^audio rms' "$w/intro.log") (music: not yet)"
        run title --frames 1700 --state-test 1400 --state-span 120 --state-rounds 2 --dump-from 1700
        state_ok "$g title snapshots" "$w/title.log" 2
        ;;
    esac
done
exit $fail
