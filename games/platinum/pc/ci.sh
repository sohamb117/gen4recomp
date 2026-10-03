#!/usr/bin/env bash
#
# The whole gate, in one command: ROM build, PC build, tests, reach, and, on
# demand, the upstream merge gate.
#
# It is a script rather than a list of steps in some runner's own format,
# because a format only a hosted runner can interpret can only be tested by
# pushing and watching. Everything here runs on your machine, now, in a
# scratch clone, before anybody pushes anything. Point whatever CI you like
# at `pc/ci.sh <stage>`; the stages are the same either way.
#
# What it gates on, and what it only reports.
#   * the ROM build must succeed and must still match the cartridge (`make
#     check` runs the decomp's own test); a fresh checkout that cannot build
#     the ROM is the first thing a reviewer needs to know.
#   * `link: ok` must appear, and the non-compiling count must not exceed
#     PC_CI_MAX_SKIPPED. `make status` exits 0 whether or not the link held, so
#     the verdict is read from the lines it prints.
#   * every test must pass.
#   * the reach is reported, never gated. It is a record of (frames, ending,
#     site) whose value is in moving, and a runner is not the machine that
#     decides what a frame count means.
# The unresolved-symbol count is printed for the same reason: it counts names
# no project object defines and knows nothing about libc, so a gate on it would
# fail on a fixed defect.
#
# The reach runs without an input script here, because the suite already
# replays one under `test` and a second 45,000-frame pass would double the job
# for a weaker answer. Point PC_CI_REACH_INPUT at a script to drive one anyway.
#
# The usage text is a here-doc below rather than a comment block this script
# reads out of itself: help that is code cannot be broken by rewording a
# comment.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DRYRUN=0
JOBS="$(nproc 2>/dev/null || echo 2)"
ROMDIR="build/rom"

# Gates and knobs, all environment (this port's inputs always are).
MAX_SKIPPED="${PC_CI_MAX_SKIPPED:-0}"
REACH_INPUT="${PC_CI_REACH_INPUT:-}"
UPSTREAM_URL="${PC_CI_UPSTREAM_URL:-https://github.com/pret/pokeplatinum}"
UPSTREAM_MAX="${PC_CI_UPSTREAM_MAX:-20}"

# What the build needs on a bare Ubuntu machine. The first twelve are the
# decomp's own list, kept in the order it uses; the rest are the port's: -m32
# needs the multilib compiler and a 32-bit libc, `make status` reads the object
# tree with nm, and the viewer and launcher tests skip themselves without SDL2
# rather than failing, which is a quiet way to lose four tests.
#
# libpng-dev is the one a machine that already has it will never miss:
# nitrogfx, nitrobtx and nitrorom all take it as a native dependency, so
# without it the ROM build stops at `meson setup` with "Dependency libpng not
# found" and nothing has been compiled yet to hint at why.
PACKAGES=(
    bison flex g++ gcc-arm-none-eabi git make ninja-build pkg-config python3
    wget xz-utils libpng-dev
    binutils gcc-multilib libc6-dev-i386 libsdl2-dev
)

# Program -> package, for the one CI failure that is worth catching offline: a
# stage learns to run something and nobody adds the package. Checked from the
# test suite, which asserts every package here is in the list above and every
# program here exists where it runs.
TOOLS=(
    "bison bison"
    "flex flex"
    "g++ g++"
    "gcc gcc-multilib"
    "git git"
    "make make"
    "ninja ninja-build"
    "nm binutils"
    "pkg-config pkg-config"
    "python3 python3"
    "sdl2-config libsdl2-dev"
    "wget wget"
    "xz xz-utils"
)

say()  { printf 'ci: %s\n' "$*"; }
bad()  { printf '\033[31mci: %s\033[0m\n' "$*" >&2; }
good() { printf '\033[32mci: %s\033[0m\n' "$*"; }
die()  { bad "$*"; exit 1; }

usage() {
    cat << 'EOF'
Usage: pc/ci.sh [--dry-run] [--jobs N] <stage> [stage...]

  packages   the apt package names this needs, one per line
  tools      the programs it runs, and the package each comes from
  rom        toolchain, meson setup, ninja, and the decomp's own ROM test
  port       generated headers, then the PC build; gates on link and skips
  test       the test suite
  reach      how far the port gets and what stopped it (reported, not gated)
  upstream   merge pret and re-gate: the local half of the upstream check
  all        rom, port, test, reach

--dry-run prints every command each stage would run and runs none of them.
EOF
}

# Every command a stage runs is printed before it runs, plainly and with no
# colour, because --dry-run's output is parsed as well as read.
plan() { printf '+ %s\n' "$*"; }
run()  { plan "$@"; [[ "$DRYRUN" -eq 1 ]] && return 0; "$@"; }

# The numbers a reviewer wants without opening the log, for a CI that sets
# GITHUB_STEP_SUMMARY. Harmless anywhere else: with no such file this does
# nothing.
summary() {
    [[ -n "${GITHUB_STEP_SUMMARY:-}" ]] || return 0
    printf '%s\n' "$*" >> "$GITHUB_STEP_SUMMARY"
}

stage_packages() { printf '%s\n' "${PACKAGES[@]}"; }
stage_tools()    { printf '%s\n' "${TOOLS[@]}"; }

# The decomp's own Makefile owns the meson invocation, the toolchain fetch and
# the retail comparison, so this calls it rather than restating any of them,
# upstream is free to change the flags and this follows.
#
# `ninja` with no target rather than the ROM alone: several generated headers
# and asset indexes the port compiles against are not reachable from the ROM
# target, and building them by name means keeping a list of them here.
stage_rom() {
    run make configure BUILD="$ROMDIR" ROM_REVISION="${ROM_REVISION:-1}" \
        || die "the ROM build directory would not configure"
    run ninja -C "$ROMDIR" || die "the ROM build failed"
    run make check BUILD="$ROMDIR" \
        || die "the built ROM does not match retail"
    summary "ROM: built and matching"
}

stage_port() {
    run make -f pc/Makefile headers || die "generated headers failed"

    local out compiled skipped unres
    plan "make -f pc/Makefile status -j$JOBS"
    [[ "$DRYRUN" -eq 1 ]] && return 0
    out="$(make -f pc/Makefile status -j"$JOBS" 2>&1)"
    printf '%s\n' "$out"

    compiled="$(sed -n 's/^C files compiled: *\([0-9]*\).*/\1/p' <<< "$out" | head -1)"
    skipped="$(sed -n 's/^C files compiled:.*skipped: *\([0-9]*\).*/\1/p' <<< "$out" | head -1)"
    unres="$(sed -n 's/^unresolved symbols: *\([0-9]*\).*/\1/p' <<< "$out" | head -1)"
    summary "port: $compiled compiled, $skipped skipped, $unres unresolved"

    grep -q '^link: ok' <<< "$out" || die "no binary; see build/pc/link.log"
    [[ -n "$skipped" ]] || die "make status printed no non-compiling count"
    [[ "$skipped" -le "$MAX_SKIPPED" ]] \
        || die "$skipped file(s) did not compile (allowed $MAX_SKIPPED); \
build/pc/obj holds a .o.err for each"
    good "link ok, $compiled compiled, $skipped skipped, $unres unresolved"
}

stage_test() {
    # PC_TEST_LONG turns on the checks that are too slow for an edit-run-edit
    # loop and exactly right for a push: today that is hd3d's 4x ceiling,
    # which is sixteen times the pixels of a native frame. (The six-minute
    # new-game replay parities that used to ride this flag were discarded
    # 2026-08-26 with the other new-game rows.)
    run env PC_TEST_LONG=1 python3 pc/tests/run_tests.py \
        || die "the test suite failed"
    summary "tests: all pass"
}

# Reported, not gated, and the record is (frames, ending, site), because a
# wall that moves from a named trap to a signal at the same frame is news that
# a frame count alone cannot carry.
stage_reach() {
    local out args=(--quiet)
    [[ -n "$REACH_INPUT" ]] && args+=(--input "$REACH_INPUT")
    if [[ "$DRYRUN" -eq 1 ]]; then
        plan "pc/reach.sh ${args[*]}"
        return 0
    fi
    out="$(mktemp)"
    plan "pc/reach.sh ${args[*]}"
    REACH_OUT="$out" pc/reach.sh "${args[@]}" >/dev/null 2>&1
    local frames ending site still
    IFS=$'\t' read -r frames ending site still < "$out" 2>/dev/null
    rm -f "$out"
    [[ -n "${frames:-}" ]] || { say "reach: did not report"; return 0; }
    # Four fields, printed the way pc/reach.sh prints them: the site is a
    # sentence of its own, not a place to append to the ending. Read the
    # fourth even though only the site acts on it, an unread trailing field
    # lands inside $site and turns a diagnosis into a diagnosis plus a number.
    say "reach: $frames frames, $ending"
    [[ -n "${site:-}" ]] && say "reach: $site"
    [[ -n "${still:-}" ]] && say "reach: longest still stretch $still frames"
    summary "reach: $frames frames, $ending"
    return 0
}

# The merge gate, which is the whole point of running any of this on a
# schedule: upstream keeps moving, this port compiles the decomp's C with a
# different compiler for a different word size, and the day a merge breaks
# that is the day to know. pc/sync_upstream.sh does the work and never pushes;
# this only supplies the remote and turns its verdict into an exit status.
stage_upstream() {
    if ! git remote get-url upstream >/dev/null 2>&1; then
        run git remote add upstream "$UPSTREAM_URL" || die "could not add the remote"
    fi
    run git fetch -q upstream || die "could not fetch upstream"

    # A merge needs an author, and a fresh runner has none configured.
    git config user.email >/dev/null 2>&1 \
        || run git config user.email "ci@localhost"
    git config user.name >/dev/null 2>&1 \
        || run git config user.name "build gate"

    run pc/sync_upstream.sh --max "$UPSTREAM_MAX"
    local rc=$?
    [[ "$DRYRUN" -eq 1 ]] && return 0
    case "$rc" in
        0) good "upstream: clean or nothing to merge"; summary "upstream: clean" ;;
        3) [[ -f pc/.upstream-sync-report ]] && cat pc/.upstream-sync-report
           summary "upstream: parked; the merge does not hold"
           die "the merge is parked: it built or tested worse than the base" ;;
        *) die "the upstream gate refused (status $rc)" ;;
    esac
}

stage_all() { stage_rom && stage_port && stage_test && stage_reach; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRYRUN=1; shift;;
        --jobs)    JOBS="$2"; shift 2;;
        -h|--help) usage; exit 0;;
        -*) die "unknown option $1";;
        *)  break;;
    esac
done

[[ $# -gt 0 ]] || die "no stage named; pc/ci.sh --help lists them"

for stage in "$@"; do
    case "$stage" in
        packages|tools|rom|port|test|reach|upstream|all) ;;
        *) die "unknown stage '$stage'; pc/ci.sh --help lists them";;
    esac
done

for stage in "$@"; do
    "stage_$stage" || exit 1
done
