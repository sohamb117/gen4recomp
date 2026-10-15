#!/usr/bin/env bash
# tests/dp/regress.sh: Diamond/Pearl (and Platinum, Ruby/Sapphire/Emerald)
# headless regression runs.
#
#   tests/dp/regress.sh [--no-build] [--only NAME]... [--update --reason TEXT]
#   tests/dp/regress.sh --inputs dp|plat|rse
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
# build runs under tools/heavy.sh with -j $NP_JOBS (default 4). With
# NP_MIN_FREE_GB=N, a build step finding less than N GiB free fails instead.
#
# Updating hashes is deliberate: --update re-runs the selected cases (all by
# default), writes the new hashes into expected.txt and appends one history
# line per changed hash with the date and the required --reason.
#
# Core inputs: the tracked files each core is built from, as git pathspecs
# (--inputs prints them; tools/gate.sh selects cases by them):
#   every core  core/CMakeLists.txt core/cmake core/include core/runtime
#               core/tools, shell/src/net.c shell/src/net.h (np_headless
#               links them), tools/wasm2c_postprocess.py tools/toolchains.lock
#   dp (D/P)    games/diamond, and the Platinum port code its makefile builds:
#               games/platinum/pc games/platinum/tools/armrec
#               games/platinum/subprojects
#   plat (Pt)   games/platinum
#   rse (R/S/E) games/gba-common games/emerald games/ruby (the decomps are
#               the read-only .cache/gba)
#
# Reused cores (NP_GATE_CORES=DIR; unset by default, tools/gate.sh sets it):
# DIR is laid out like build/ (core-dp, core-plat, core-rse; links will do),
# each core with the commit it was built from beside it (DIR/core-dp.rev,
# ...). A wanted core is run from DIR, building neither its guest modules
# nor the core, when this tree's inputs for it are that commit's (no diff, no
# untracked file), its CMakeCache.txt is configure()'s Release build of
# exactly the games whose ROMs are here, and its np_headless is not newer
# than its .rev; otherwise it is built as usual. After building a core whose
# inputs are HEAD's before and after, regress.sh writes <core dir>.rev (e.g.
# build/core-dp.rev), so any build/ can serve as such a DIR.
set -u

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
expected=tests/dp/expected.txt
dp_core=${NP_DP_CORE:-build/core-dp}
plat_core=${NP_PLAT_CORE:-build/core-plat}
rse_core=${NP_RSE_CORE:-build/core-rse}
gate=${NP_GATE_CORES:-}
build=1 update=0 reason=
only=()

inputs() { # inputs dp|plat|rse: the core's git pathspecs (header)
    echo core/CMakeLists.txt core/cmake core/include core/runtime core/tools \
        shell/src/net.c shell/src/net.h tools/wasm2c_postprocess.py tools/toolchains.lock
    case $1 in
    dp) echo games/diamond games/platinum/pc games/platinum/tools/armrec games/platinum/subprojects ;;
    plat) echo games/platinum ;;
    rse) echo games/gba-common games/emerald games/ruby ;;
    esac
}

while [ $# -gt 0 ]; do
    case $1 in
    --no-build) build=0; shift ;;
    --only) only+=("$2"); shift 2 ;;
    --update) update=1; shift ;;
    --reason) reason=$2; shift 2 ;;
    --inputs)
        case ${2:-} in dp | plat | rse) inputs "$2" | tr ' ' '\n'; exit 0 ;; esac
        echo "regress: --inputs dp|plat|rse" >&2
        exit 2
        ;;
    *) sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
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
group_of() {
    case $1 in
    diamond | pearl) echo dp ;;
    platinum) echo plat ;;
    emerald | ruby | sapphire) echo rse ;;
    esac
}
games_of() {
    case $1 in
    dp) echo diamond pearl ;;
    plat) echo platinum ;;
    rse) echo emerald ruby sapphire ;;
    esac
}
present() { # present GROUP: its games whose ROM is here (the ones its core holds)
    local g out=
    for g in $(games_of "$1"); do [ -f "$(rom_of "$g")" ] && out="$out $g"; done
    echo $out
}
# Each core's build directory, and the np_headless it runs from: the one in
# its build directory unless reused.
dir_of() { eval "echo \"\$${1}_core\""; }
dp_bin=$dp_core/np_headless plat_bin=$plat_core/np_headless rse_bin=$rse_core/np_headless
bin_of() { eval "echo \"\$$(group_of "$1")_bin\""; }
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
want_group() {
    local g
    for g in $(games_of "$1"); do want "$g" && return 0; done
    return 1
}

configure() { # configure DIR GAME...
    local dir=$1 home
    shift
    # A build dir copied or linked from another checkout keeps that checkout's
    # CMakeCache: cmake then regenerates into the other tree and ninja loops
    # ("manifest 'build.ninja' still dirty after 100 tries"). Refuse it.
    if [ -f "$dir/CMakeCache.txt" ]; then
        home=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$dir/CMakeCache.txt")
        if [ "$(cd "$home" 2>/dev/null && pwd -P)" != "$(cd "$root/core" && pwd -P)" ]; then
            echo "regress: $dir was configured from $home, not this tree's core ($root/core):" \
                "delete it (it is regenerable output) and rerun" >&2
            return 1
        fi
    fi
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

# same_inputs REV GROUP: this tree's inputs of GROUP are commit REV's: no
# difference, tracked or not.
same_inputs() {
    local paths
    paths=$(inputs "$2")
    git rev-parse --verify --quiet "$1^{commit}" >/dev/null || return 1
    # shellcheck disable=SC2086 # pathspec word lists
    git diff --quiet "$1" -- $paths || return 1
    # shellcheck disable=SC2086
    [ -z "$(git ls-files --others --exclude-standard -- $paths)" ]
}
# holds DIR GROUP: DIR is configure()'s build of exactly GROUP's present
# games, and DIR.rev says what it was built from.
holds() {
    local cache=$1/CMakeCache.txt built
    [ -x "$1/np_headless" ] && [ -f "$1.rev" ] && [ ! "$1/np_headless" -nt "$1.rev" ] && [ -f "$cache" ] ||
        return 1
    grep -qx 'CMAKE_BUILD_TYPE:STRING=Release' "$cache" && grep -qx 'NP_BOUNDS_CHECK:BOOL=OFF' "$cache" ||
        return 1
    built=$(sed -n 's/^NP_GUEST_WASM_\([a-z]*\):FILEPATH=..*/\1/p' "$cache" | sort | xargs)
    [ "$built" = "$(present "$2" | tr ' ' '\n' | sort | xargs)" ]
}

# Each wanted core: run from NP_GATE_CORES (header), or built here; one to
# build whose inputs are HEAD's now is stamped if they still are after.
reuse=" " clean=" "
head=$(git rev-parse HEAD)
for grp in dp plat rse; do
    want_group $grp || continue
    if [ -n "$gate" ]; then
        c=$gate/core-$grp
        rev=$(cat "$c.rev" 2>/dev/null)
        if [ -n "$rev" ] && holds "$c" $grp && same_inputs "$rev" $grp; then
            eval "${grp}_bin=\$c/np_headless"
            reuse="$reuse$grp "
            echo "reuse: $c (built from ${rev:0:9}, whose $grp inputs are this tree's)"
            continue
        fi
        echo "build: $grp: $c${rev:+ (${rev:0:9})} is not a build of this tree's $grp inputs"
    fi
    if [ $build = 1 ] && same_inputs "$head" $grp; then clean="$clean$grp "; fi
done
reused() { case $reuse in *" $1 "*) return 0 ;; esac; return 1; }

if [ $build = 1 ]; then
    # The machine-wide semaphore and job cap for heavy builds (tools/heavy.sh).
    jobs=${NP_JOBS:-4}
    heavy=$root/tools/heavy.sh
    blog=$(mktemp "${TMPDIR:-/tmp}/regress-build.XXXXXX")
    step() { # step LABEL CMD...: quiet unless it fails
        echo "build: $1"
        if [ -n "${NP_MIN_FREE_GB:-}" ] &&
            [ "$(df -k . | awk 'NR == 2 { print $4 }')" -lt $((NP_MIN_FREE_GB * 1024 * 1024)) ]; then
            echo "FAIL build: less than $NP_MIN_FREE_GB GiB free"
            exit 1
        fi
        shift
        if ! "$@" >>"$blog" 2>&1; then
            tail -n 30 "$blog"
            echo "FAIL build ($blog)"
            exit 1
        fi
    }
    if ! reused dp; then
        want diamond && step "diamond module" "$heavy" make -C games/diamond -f pc/Makefile.wasm -j"$jobs"
        want pearl && step "pearl module" "$heavy" make -C games/diamond -f pc/Makefile.wasm -j"$jobs" GAME_VERSION=PEARL
    fi
    if ! reused plat; then
        want platinum && step "platinum module" "$heavy" make -C games/platinum -f pc/Makefile.wasm -j"$jobs"
    fi
    if ! reused rse; then
        for g in emerald ruby sapphire; do
            want $g && step "$g module" "$heavy" games/gba-common/tools/gbabuild.py $g -j "$jobs"
        done
    fi
    for grp in dp plat rse; do
        want_group $grp && ! reused $grp || continue
        dir=$(dir_of $grp)
        rm -f "$dir.rev"
        # shellcheck disable=SC2046 # a word list of games
        step "configure $dir" configure "$dir" $(present $grp)
        step "core $dir" "$heavy" cmake --build "$dir" -j "$jobs"
        case $clean in *" $grp "*)
            if [ "$(git rev-parse HEAD)" = "$head" ] && same_inputs "$head" $grp; then echo "$head" >"$dir.rev"; fi
            ;;
        esac
    done
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
    bin=$(bin_of "$game")
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
