#!/usr/bin/env bash
# tests/dp/regress.sh: Diamond/Pearl (and Platinum, Ruby/Sapphire/Emerald)
# headless regression runs.
#
#   tests/dp/regress.sh [--no-build] [--only NAME]... [--update --reason TEXT]
#
# Each case in tests/dp/expected.txt runs np_headless to a fixed frame count
# and compares the final hash (frames + audio, np_headless's formula):
#
#   NAME  GAME  FRAMES  HASH  [np_headless args...]
#
# Exit status: 0 all ran and matched (cases whose ROM is missing are skipped,
# printed SKIP), 1 a hash mismatch, trap, failure or build error, 2 usage.
#
# Build (unless --no-build): the guest modules are rebuilt by their own
# makefiles (incremental; the GBA games by games/gba-common/tools/gbabuild.py
# from the decomps in .cache/gba), then the native cores by cmake/ninja:
# Diamond and Pearl into $NP_DP_CORE (default build/core-dp), Platinum into
# $NP_PLAT_CORE (default build/core-plat), Ruby/Sapphire/Emerald into
# $NP_RSE_CORE (default build/core-rse), configured on first use. Each
# build runs under tools/heavy.sh with -j $NP_JOBS (default 4).
#
# Updating hashes is deliberate: --update re-runs the selected cases (all by
# default), writes the new hashes into expected.txt and appends one history
# line per changed hash with the date and the required --reason.
set -u

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
expected=tests/dp/expected.txt
dp_core=${NP_DP_CORE:-build/core-dp}
plat_core=${NP_PLAT_CORE:-build/core-plat}
rse_core=${NP_RSE_CORE:-build/core-rse}
build=1 update=0 reason=
only=()
while [ $# -gt 0 ]; do
    case $1 in
    --no-build) build=0; shift ;;
    --only) only+=("$2"); shift 2 ;;
    --update) update=1; shift ;;
    --reason) reason=$2; shift 2 ;;
    *) sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
    esac
done
if [ $update = 1 ] && [ -z "$reason" ]; then
    echo "regress: --update needs --reason TEXT (why the hashes change)" >&2
    exit 2
fi

rom_of() {
    case $1 in
    diamond) echo games/diamond/build/diamond.us/pokediamond.us.nds ;;
    pearl) echo games/diamond/build/pearl.us/pokepearl.us.nds ;;
    platinum) echo games/platinum/build/rom/pokeplatinum.us.nds ;;
    emerald) echo .cache/gba/pokeemerald/pokeemerald.gba ;;
    ruby | sapphire) echo ".cache/gba/pokeruby/poke$1.gba" ;;
    esac
}
core_of() {
    case $1 in
    diamond | pearl) echo "$dp_core" ;;
    platinum) echo "$plat_core" ;;
    emerald | ruby | sapphire) echo "$rse_core" ;;
    esac
}
selected() {
    [ ${#only[@]} = 0 ] && return 0
    local o
    for o in "${only[@]}"; do [ "$o" = "$1" ] && return 0; done
    return 1
}

# The cases to run: selected, ROM present.
names=() games=()
while read -r name game frames hash args; do
    case $name in '' | '#'*) continue ;; esac
    selected "$name" || continue
    if [ ! -f "$(rom_of "$game")" ]; then
        echo "SKIP $name: no $game ROM at $(rom_of "$game")"
        continue
    fi
    names+=("$name")
    games+=("$game")
done <"$expected"
if [ ${#names[@]} = 0 ]; then
    echo "regress: nothing to run"
    exit 0
fi
want() { printf '%s\n' "${games[@]}" | grep -qx "$1"; }

if [ $build = 1 ]; then
    # The machine-wide semaphore and job cap for heavy builds (tools/heavy.sh).
    jobs=${NP_JOBS:-4}
    heavy=$root/tools/heavy.sh
    blog=$(mktemp "${TMPDIR:-/tmp}/regress-build.XXXXXX")
    step() { # step LABEL CMD...: quiet unless it fails
        echo "build: $1"
        shift
        if ! "$@" >>"$blog" 2>&1; then
            tail -n 30 "$blog"
            echo "FAIL build ($blog)"
            exit 1
        fi
    }
    want diamond && step "diamond module" "$heavy" make -C games/diamond -f pc/Makefile.wasm -j"$jobs"
    want pearl && step "pearl module" "$heavy" make -C games/diamond -f pc/Makefile.wasm -j"$jobs" GAME_VERSION=PEARL
    want platinum && step "platinum module" "$heavy" make -C games/platinum -f pc/Makefile.wasm -j"$jobs"
    for g in emerald ruby sapphire; do
        want $g && step "$g module" "$heavy" games/gba-common/tools/gbabuild.py $g -j "$jobs"
    done
    configure() { # configure DIR GAME...
        local dir=$1
        shift
        [ -f "$dir/build.ninja" ] && return 0
        local defs=() g
        for g in "$@"; do
            case $g in
            diamond) defs+=("-DNP_GUEST_WASM_diamond=$root/games/diamond/build/pc-wasm/pokediamond.wasm") ;;
            pearl) defs+=("-DNP_GUEST_WASM_pearl=$root/games/diamond/build/pc-wasm/pokepearl.wasm") ;;
            platinum) defs+=("-DNP_GUEST_WASM_platinum=$root/games/platinum/build/pc-wasm/pokeplatinum.wasm") ;;
            emerald) defs+=("-DNP_GUEST_WASM_emerald=$root/games/emerald/build/pc-wasm/pokeemerald.wasm") ;;
            ruby | sapphire) defs+=("-DNP_GUEST_WASM_$g=$root/games/ruby/build/pc-wasm/poke$g.wasm") ;;
            esac
        done
        cmake -S core -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNP_BUILD_TESTS=OFF \
            -DNP_GUEST_POSTPROCESS="$root/tools/wasm2c_postprocess.py" "${defs[@]}"
    }
    if want diamond || want pearl; then
        dp=()
        [ -f "$(rom_of diamond)" ] && dp+=(diamond)
        [ -f "$(rom_of pearl)" ] && dp+=(pearl)
        step "configure $dp_core" configure "$dp_core" "${dp[@]}"
        step "core $dp_core" "$heavy" cmake --build "$dp_core" -j "$jobs"
    fi
    if want platinum; then
        step "configure $plat_core" configure "$plat_core" platinum
        step "core $plat_core" "$heavy" cmake --build "$plat_core" -j "$jobs"
    fi
    if want emerald || want ruby || want sapphire; then
        rse=()
        for g in emerald ruby sapphire; do [ -f "$(rom_of $g)" ] && rse+=($g); done
        step "configure $rse_core" configure "$rse_core" "${rse[@]}"
        step "core $rse_core" "$heavy" cmake --build "$rse_core" -j "$jobs"
    fi
    rm -f "$blog"
fi

fail=0
logdir=$(mktemp -d "${TMPDIR:-/tmp}/regress.XXXXXX")
declare -a new_lines=()
while read -r name game frames hash args; do
    case $name in '' | '#'*) continue ;; esac
    run=0
    for n in "${names[@]}"; do [ "$n" = "$name" ] && run=1; done
    [ $run = 1 ] || continue
    bin=$(core_of "$game")/np_headless
    if [ ! -x "$bin" ]; then
        echo "FAIL $name: no $bin"
        fail=1
        continue
    fi
    log=$logdir/$name.log
    # shellcheck disable=SC2086 # args is a word list by design
    "$bin" "$game" "$(rom_of "$game")" --frames "$frames" $args </dev/null >"$log" 2>&1
    rc=$?
    got=$(sed -n 's/^frames \([0-9]*\) .* hash \([0-9a-f]*\)$/\1 \2/p' "$log" | tail -n 1)
    ran=${got%% *} h=${got##* }
    ms=$(sed -n 's/^\[headless\] \([0-9.]*\) ms\/frame.*/\1/p' "$log")
    if [ $rc != 0 ] || [ "$ran" != "$frames" ]; then
        echo "FAIL $name: exit $rc after ${ran:-?} of $frames frames:"
        grep -a 'FAILED\|exited\|trap\|error' "$log" | tail -n 5 | sed 's/^/    /'
        echo "    log: $log"
        fail=1
    elif [ "$h" = "$hash" ]; then
        echo "ok   $name ($game $frames frames, $h, ${ms:-?} ms/frame)"
    elif [ $update = 1 ]; then
        echo "UPD  $name: $hash -> $h"
        new_lines+=("$name $hash $h")
    else
        echo "FAIL $name: hash $h, expected $hash ($game $frames frames; log $log)"
        fail=1
    fi
done <"$expected"

if [ $update = 1 ] && [ ${#new_lines[@]} -gt 0 ]; then
    tmp=$(mktemp)
    changes=$(mktemp)
    # The changes go through a file: macOS awk refuses a newline in -v.
    printf '%s\n' "${new_lines[@]}" >"$changes"
    awk 'NR == FNR { nh[$1] = $3; next }
        /^[^#[:space:]]/ && ($1 in nh) { sub($4, nh[$1]) }
        { print }' "$changes" "$expected" >"$tmp"
    rm -f "$changes"
    for l in "${new_lines[@]}"; do
        set -- $l
        echo "# $(date +%Y-%m-%d) $1: $2 -> $3: $reason" >>"$tmp"
    done
    mv "$tmp" "$expected"
    echo "updated $expected"
fi
[ $fail = 0 ] && rm -rf "$logdir"
exit $fail
