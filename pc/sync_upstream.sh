#!/usr/bin/env bash
#
# Merge upstream pret into the port, and keep the merge only if the port still
# builds, still passes, and still reaches as far as it did.
#
# Upstream is a moving target and this port is deliberately built so that
# merging it is cheap: nothing under src/, lib/ or subprojects/ is ever edited,
# so the only file both sides own is .gitignore. That makes the git half of a
# sync nearly free and moves the entire risk to the build half, which is what
# this script exists to measure.
#
# The three failure classes, and only the first is pret's:
#   1. a non-matching decompilation, which their CI catches, not us;
#   2. C that matches on ARM but misbehaves compiled -m32, which is ours;
#   3. fresh boundaries where new upstream C meets the port's own models.
# A sync that only ran `git merge` has looked for none of them.
#
# The merge lands on sync/upstream, never on the working branch. A sync whose
# gate fails leaves that branch exactly as it was and the failed attempt on a
# named branch for the next run to fix, so an unattended sync can never be the
# reason the port stopped building. Only a clean gate fast-forwards.
#
# It never pushes. `origin` is pret and is fetch-only; nothing here pushes
# anywhere. It merges rather than rebases, because rebasing would force-push a
# branch other things point at to buy a tidier graph nobody reads.
#
# Batches, because small and often beats one big merge: a patch that stops
# applying fails loudly but individually, and a run that meets three dead
# patches at once does nothing else all run. The default is 20 upstream
# first-parent commits per attempt.
#
# The stale-object hazard is specific to syncing and the build cannot heal it
# on its own. pc/Makefile enumerates sources with `find` at parse time but
# links with `$(wildcard)` over the object tree, so a skipped file's absent .o
# simply drops out. A source upstream deletes or renames therefore leaves a
# stale .o that goes on linking. Every gate here prunes orphaned objects first.
#
# Exit codes:
#   0  nothing to do, or synced clean
#   1  refused, preconditions unmet, tree untouched
#   3  needs hands; the attempt is on sync/upstream with a written report
#
# Usage: pc/sync_upstream.sh [--max N|--all] [--dry-run] [--abort]
#                           [--rerun] [--no-reach] [--quiet]
#
#   --rerun  re-gate a merge parked on sync/upstream after repairing it, and
#            adopt it if it now passes. Judged against the baseline recorded
#            before the merge, which is not measurable from the merged tree.
#   --abort  discard the parked attempt. Not a way out of a failing gate:
#            throwing upstream work away is a decision for a human.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Defined here rather than beside the fetch: --rerun never reaches the fetch
# and `set -u` turns a variable that only some entry points define into an
# abort on the path that skipped it.
UPSTREAM="origin/main"
BRANCH_WORK="recomp"
BRANCH_SYNC="sync/upstream"
MARKER="pc/.upstream-last-sync"
REPORT="pc/.upstream-sync-report"
# The pre-merge reach, kept in a file rather than only in the report's prose so
# that --rerun compares against the same baseline the failed attempt did. The
# alternative is re-measuring on a tree that already has the merge in it, which
# is not a baseline, or parsing the report back, which means an instrument
# reading its own English.
BASELINE="pc/.upstream-sync-baseline"
INFLIGHT=".git/pc-upstream-inflight"
GATELOG="build/pc/upstream-gate.log"

MAX="${UPSTREAM_MAX:-20}"
DRYRUN=0
ABORT=0
RERUN=0
NOREACH=0
QUIET=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --max)     MAX="$2"; shift 2;;
        --all)     MAX=0; shift;;
        --dry-run) DRYRUN=1; shift;;
        --abort)   ABORT=1; shift;;
        --rerun)   RERUN=1; shift;;
        --no-reach) NOREACH=1; shift;;
        --quiet)   QUIET=1; shift;;
        *) printf 'usage: pc/sync_upstream.sh [--max N|--all] [--dry-run] [--abort]\n' >&2
           printf '                           [--rerun] [--no-reach] [--quiet]\n' >&2
           exit 2;;
    esac
done

say()  { [[ "$QUIET" -eq 1 ]] || printf 'sync: %s\n' "$*"; }
loud() { printf '\033[33msync: %s\033[0m\n' "$*"; }
bad()  { printf '\033[31msync: %s\033[0m\n' "$*"; }
good() { printf '\033[32msync: %s\033[0m\n' "$*"; }

# --------------------------------------------------------------------- helpers
#
# Defined before anything calls them, which --abort did not do in the first
# draft of this file: bash resolves a function at call time, so an --abort run
# reached `prune_orphans` fifty lines before the definition and died with
# "command not found" after it had already moved the branch.

# Objects that have left the build. See the header: the link globs the object
# tree, so one of these is a ghost definition rather than dead weight.
#
# Asked of the build, not inferred from the filesystem. The first version of
# this compared each .o against its own source path and deleted the ones whose
# .c was gone, which catches an upstream deletion and misses the other way a
# file leaves: still on disk, no longer enumerated. The ARM7 exclusion did
# exactly that to 86 sources, 9 of which had already compiled, and those nine
# .o files would have gone on linking the other processor's symbols into this
# image with nothing reporting it. `make objlist` is the build's own answer to
# "what do I expect", so it cannot drift from what the link consumes.
prune_orphans() {
    local n=0 o want
    [[ -d build/pc/obj ]] || { printf 'no object tree'; return 0; }
    # $(OBJS) is spelled with $(ROOT), which resolves to an unnormalized
    # ".../pc/../build/pc/obj/..."; find yields "build/pc/obj/...". Both are
    # reduced to the part below the object root rather than realpath'd, so the
    # comparison does not depend on the tree's location or on a symlink.
    local norm='s|.*/build/pc/obj/|build/pc/obj/|'
    want="$(mktemp)"
    if ! make -s -f pc/Makefile objlist 2>/dev/null | sed "$norm" | sort -u > "$want" \
       || [[ ! -s "$want" ]]; then
        rm -f "$want"
        printf 'could not read the expected object list; NOTHING PRUNED'
        return 0
    fi
    # .skipped markers are walked too, not just .o files, and that is not a
    # detail. A source that failed to compile has a marker and NO object, so a
    # sweep over '*.o' cannot see it, the 77 ARM7 markers from the merge that
    # exposed all this survived the prune and went on being counted as this
    # build's non-compiling files long after their sources had left it. The
    # count is what the gate reads, so a stale marker is a false failure that
    # cannot be cleared by fixing anything.
    while IFS= read -r f; do
        local base="${f%.skipped}"
        grep -qxF "$(printf '%s' "$base" | sed "$norm")" "$want" \
            || { rm -f "$base" "$base".*; n=$((n + 1)); }
    done < <(find build/pc/obj \( -name '*.o' -o -name '*.o.skipped' \) 2>/dev/null \
               | sort -u)
    rm -f "$want"
    printf 'pruned %d object(s) no longer in the build' "$n"
}

# The gate, in the order the dependencies actually run:
#   1. The ROM build FIRST. pc/Makefile consumes build/rom/main.nef.xMAP (the
#      linker-script symbol addresses) and build/rom/trainer_ai_script.o. Both
#      move when upstream's code moves, so a PC build over a stale xmap reads
#      the wrong addresses and the failure looks like anything but a stale map.
#   2. generated headers, which follow the ROM tree's generated/.
#   3. prune, then the PC build via `status` (which reports the numbers).
#   4. tests, then reach.
# Results land in globals rather than a return value: there are six of them and
# the caller needs all six to write the report.
G_rom=""; G_link=""; G_compiled=""; G_skipped=""; G_unres=""
G_dead=(); G_tests=""; G_testout=""; G_frames=""; G_ending=""; G_site=""

# A meson wrap whose revision the checkout no longer matches.
#
# Found by the first real batch this ran on, and it is the reason this function
# exists. pret #1247 ("Transition to compiled arm7 binary") bumped
# subprojects/NitroSDK.wrap so that ichneumon_sub became a COMPILED .nef instead
# of a prebuilt .sbin, and vendored a stricter ROM packer in the same commit.
# `ninja` cannot re-fetch a wrap, so the build combined the new tool with the old
# checkout and failed with a message that names neither of them:
#
#     rompacker:configuration:27: nef path does not end in .nef
#
# Resetting a subproject is safe HERE and would not be in most trees: nothing
# under subprojects/ is ever edited; what needs changing is
# patched into the copy the compile consumes), so a checkout is a cache and a
# reset costs a download. Any pc/patches diff against a subproject re-applies to
# the new checkout, and the gate reports it if one no longer does.
sync_subprojects() {
    local w dir rev have stale=()
    for w in subprojects/*.wrap; do
        [[ -f "$w" ]] || continue
        grep -q '^\[wrap-git\]' "$w" || continue
        dir="$(sed -n 's/^[[:space:]]*directory[[:space:]]*=[[:space:]]*//p' "$w" | head -1)"
        rev="$(sed -n 's/^[[:space:]]*revision[[:space:]]*=[[:space:]]*//p' "$w" | head -1)"
        [[ -n "$dir" && -n "$rev" ]] || continue
        # Only a pinned sha can be compared. A wrap tracking a branch or tag
        # would mismatch every time and update on every gate, which is a
        # download per run to discover nothing.
        [[ "$rev" =~ ^[0-9a-f]{7,40}$ ]] || continue
        [[ -d "subprojects/$dir/.git" ]] || continue
        have="$(git -C "subprojects/$dir" rev-parse HEAD 2>/dev/null)"
        [[ -n "$have" ]] || continue
        [[ "$have" == "$rev"* || "$rev" == "$have"* ]] && continue
        stale+=("$dir")
    done
    [[ "${#stale[@]}" -gt 0 ]] || return 0
    say "wrap revision moved for ${stale[*]}, updating subprojects and reconfiguring"
    meson subprojects update --reset >>"$GATELOG.rom" 2>&1
    meson setup --reconfigure build/rom >>"$GATELOG.rom" 2>&1
    return 0
}

run_build() {
    mkdir -p build/pc
    : > "$GATELOG.rom"
    sync_subprojects
    # The ROM targets by name, not ninja's default `all`, and the difference is
    # not a speed-up, `all` cannot be built at all since upstream raised the
    # meson floor to 1.12.0 (pret 1e9905c2ff, merged 2026-08-14).
    #
    # Subprojects are machine-aware. NitroSDK is the only subproject here
    # holding `native: true` targets (tools/makelcf, gen/nitro/fx), and the port
    # instantiates the whole of it a second time for the BUILD machine because
    # of them, 1562 duplicate targets under build.subprojects/, every library
    # in the SDK twice. The duplicates still carry the cross project arguments,
    # so ninja runs `gcc -wrap:sdk 2.0/sp1p2 -proc arm7tdmi -O4,s -sym on
    # -fp soft`, which gcc rejects on sight; 451 of them are precompiled
    # headers, so it dies within twenty targets. NitroSystem, NitroWiFi and
    # yyjson have no native target and are not duplicated. Nothing links the
    # duplicates; they are reachable only from `all`.
    #
    # Naming the three artifacts loses no coverage: pc/Makefile consumes
    # main.nef.xMAP and trainer_ai_script.o, pokeplatinum.us.nds proves the
    # merge still assembles a ROM, and everything real is a dependency of one
    # of them. `main.nef` rather than `main.nef.xMAP` because the map is a side
    # product of that link and not a target ninja knows by name. Measured on
    # the 4-commit batch: 13997 targets, zero failures, all three produced.
    #
    # Drop this back to a bare `ninja -C build/rom` when upstream stops
    # duplicating the SDK; the check is that build/rom/build.ninja has no
    # `build.subprojects/NitroSDK` targets in it.
    if ninja -C build/rom pokeplatinum.us.nds main.nef trainer_ai_script.o \
        >>"$GATELOG.rom" 2>&1; then
        G_rom="ok"
    else
        G_rom="FAILED"
        return 1
    fi
    make -s -f pc/Makefile headers >>"$GATELOG.rom" 2>&1
    prune_orphans >/dev/null

    make -f pc/Makefile status -j"$(nproc)" >"$GATELOG" 2>&1
    G_compiled="$(grep -oE 'C files compiled: [0-9]+' "$GATELOG" | grep -oE '[0-9]+' | head -1)"
    G_skipped="$(grep -oE 'skipped: [0-9]+' "$GATELOG" | grep -oE '[0-9]+' | head -1)"
    G_unres="$(grep -oE 'unresolved symbols: [0-9]+' "$GATELOG" | grep -oE '[0-9]+' | head -1)"
    grep -q '^link: ok' "$GATELOG" && G_link="ok" || G_link="FAILED"

    # A patch that no longer applies says so in as many words, and drops the
    # object without failing the build. That silence is exactly why this is
    # grepped rather than trusted to the exit status.
    mapfile -t G_dead < <(grep -oE 'SKIP +[^ ]+ \(pc/patches diff no longer applies\)' "$GATELOG" \
                            | awk '{print $2}')
    [[ "$G_link" == "ok" ]]
}

run_tests() {
    G_testout="$(make -s -f pc/Makefile test 2>&1)"
    if printf '%s' "$G_testout" | grep -q '^FAIL'; then
        G_tests="$(printf '%s' "$G_testout" | grep -c '^FAIL') failed"
        return 1
    fi
    G_tests="all pass"
}

# The reach record is (frames, ending, site) and any of the three moving is
# news: the sibling diamond port cleared a trap at frame 1122 and the frame count
# did not budge, so a frames-only comparison would have called that "no
# change". Driven by the replay when one exists: an input-less reach past the
# title screen only measures the attract loop, and "the replay still plays
# frame-for-frame" is the strongest evidence this port has.
run_reach() {
    local out; out="$(mktemp)"
    local args=(--quiet)
    [[ -f pc/replays/new-game.txt ]] && args+=(--input pc/replays/new-game.txt)
    [[ -n "${SYNC_REACH_ARGS:-}" ]] && read -ra args <<< "--quiet ${SYNC_REACH_ARGS}"
    REACH_OUT="$out" pc/reach.sh "${args[@]}" >/dev/null 2>&1
    IFS=$'\t' read -r G_frames G_ending G_site < "$out" 2>/dev/null
    rm -f "$out"
    [[ -n "$G_frames" ]] || { G_frames="?"; G_ending="reach did not report"; G_site=""; }
}


# The gate, the verdict and the adoption, one function because two entry
# points need all three: a fresh attempt that has just merged, and --rerun on a
# merge someone has since repaired. Reads TARGET, n_take, n_pending and the
# base_* baseline as globals; they are set differently by each entry point and
# mean the same thing to this.
gate_verdict() {
    local dirty=() summary remaining
    run_build || dirty+=("the build")
    if [[ "$G_rom" == "FAILED" ]]; then
        dirty=("the ROM build (see $GATELOG.rom)")
    else
        [[ "${#G_dead[@]}" -gt 0 ]] && dirty+=("${#G_dead[@]} dead patch(es)")
        if [[ -n "$base_skipped" && -n "$G_skipped" && "$G_skipped" -gt "$base_skipped" ]]; then
            dirty+=("non-compiling count $base_skipped -> $G_skipped")
        fi
        if [[ "$G_link" == "ok" ]]; then
            run_tests || dirty+=("tests: $G_tests")
            if [[ "$NOREACH" -eq 0 ]]; then
                # Cleared first, or the baseline's own numbers survive into the
                # summary: the first gate failure this script reported said
                # "reach 20000" on a run whose post-merge reach never executed,
                # because run_reach had already filled these in for the baseline.
                # A gate that reports a measurement it did not take is the one
                # failure mode worse than not measuring.
                G_frames=""; G_ending=""; G_site=""
                run_reach
                if [[ -n "$base_frames" && "$G_frames" != "?" ]]; then
                    if [[ "$G_frames" -lt "$base_frames" ]]; then
                        dirty+=("reach regressed $base_frames -> $G_frames frames")
                    elif [[ "$G_frames" -eq "$base_frames" && "$G_ending" != "$base_ending" ]]; then
                        dirty+=("reach ending changed at the same frame: '$base_ending' -> '$G_ending'")
                    fi
                fi
            fi
        fi
    fi

    # The unresolved-symbol count is reported and deliberately NOT a gate:
    # pc/Makefile says why; it counts names no project object defines and knows
    # nothing about libc, so fixing a defect can push it up. A gate on it would
    # have hidden the defect that taught the tree that.
    summary="rom ${G_rom:-not run}, link ${G_link:-not run}, compiled ${G_compiled:-?}, non-compiling ${G_skipped:-?}, unresolved ${G_unres:-?}, tests ${G_tests:-not run}, reach ${G_frames:-not run}"

    if [[ "${#dirty[@]}" -gt 0 ]]; then
        bad "GATE FAILED, recomp is untouched. $summary"
        printf '\033[31m       %s\033[0m\n' "${dirty[@]}"
        [[ "${#G_dead[@]}" -gt 0 ]] && {
            bad "dead patches (re-author against the build input, not the source):"
            printf '       %s\n' "${G_dead[@]}"
        }
        {
            printf 'upstream sync needs hands\n'
            printf 'target:  %s\n' "$(git log -1 --format='%h %s' "$TARGET")"
            printf 'batch:   %s commit(s) of %s pending\n' "$n_take" "$n_pending"
            printf 'summary: %s\n' "$summary"
            printf 'baseline reach: %s frames, %s\n' "${base_frames:-none}" "${base_ending:-}"
            printf 'what failed:\n'
            printf '  %s\n' "${dirty[@]}"
            if [[ "${#G_dead[@]}" -gt 0 ]]; then
                printf 'dead patches; each is re-authored against WHAT THE COMPILE CONSUMES\n'
                printf '(build/pc/obj/game/<path>.o.stripped.c when stripping succeeds, the\n'
                printf 'pristine source otherwise), never against the tree:\n'
                printf '  %s\n' "${G_dead[@]}"
            fi
            printf 'logs: %s (pc build), %s.rom (rom build)\n' "$GATELOG" "$GATELOG"
            printf 'The merge is committed on %s. Fix it there, then:\n' "$BRANCH_SYNC"
            printf '  pc/sync_upstream.sh --rerun   re-gate, and adopt it if it passes\n'
            printf '  pc/sync_upstream.sh --abort   discard the attempt\n'
            printf 'Prefer --rerun to running the gate by hand: a repair checked by a\n'
            printf 'different set of checks than the ones that rejected it is not checked.\n'
        } > "$REPORT"
        printf '%s\t%s\tneeds-hands %s\n' "$(git rev-parse "$TARGET")" "$(date -Iseconds)" \
            "$(printf '%s; ' "${dirty[@]}")" > "$MARKER"
        exit 3
    fi

    # ------------------------------------------------------------------- adopt it

    git checkout -q "$BRANCH_WORK" || { bad "could not return to $BRANCH_WORK"; exit 1; }
    if ! git merge --ff-only -q "$BRANCH_SYNC" >/dev/null 2>&1; then
        bad "$BRANCH_WORK would not fast-forward to $BRANCH_SYNC; it moved during the gate."
        bad "The verified merge is on $BRANCH_SYNC; nothing was lost."
        exit 3
    fi
    git branch -qD "$BRANCH_SYNC"
    rm -f "$INFLIGHT" "$REPORT" "$BASELINE"

    printf '%s\t%s\tclean\n' "$(git rev-parse "$TARGET")" "$(date -Iseconds)" > "$MARKER"

    good "adopted $n_take upstream commit(s) onto $BRANCH_WORK. $summary"
    [[ -n "$base_frames" && "$G_frames" != "$base_frames" ]] \
        && good "reach moved $base_frames -> $G_frames frames ($G_ending)"
    remaining=$((n_pending - n_take))
    [[ "$remaining" -gt 0 ]] \
        && say "$remaining upstream commit(s) still pending, run again for the next batch"
    say "NOT pushed and the parent gitlink is NOT bumped; both are deliberate acts."
    exit 0
}


# ------------------------------------------------------------------ preflight

[[ -f pc/Makefile && -d src ]] || { bad "not the pokeplatinum tree ($ROOT)"; exit 1; }

head_branch() { git rev-parse --abbrev-ref HEAD 2>/dev/null; }

# --abort: throw the attempt away and put the tree back on recomp. Prunes on
# the way out, because the objects on disk were built against the merged tree.
if [[ "$ABORT" -eq 1 ]]; then
    cur="$(head_branch)"
    if [[ "$cur" != "$BRANCH_SYNC" ]]; then
        say "nothing to abort (on $cur, not $BRANCH_SYNC)"
        exit 0
    fi
    git reset -q --hard && git clean -qfd
    git checkout -q "$BRANCH_WORK" || { bad "could not return to $BRANCH_WORK"; exit 1; }
    git branch -qD "$BRANCH_SYNC" 2>/dev/null
    rm -f "$INFLIGHT" "$REPORT" "$BASELINE"
    prune_note="$(prune_orphans 2>/dev/null)"
    good "attempt discarded; back on $BRANCH_WORK${prune_note:+ ($prune_note)}"
    exit 0
fi

cur="$(head_branch)"

# --rerun: the gate again, on a merge someone has since repaired, adopting it if
# it now passes. Without this the only way to finish a parked sync is to run the
# four gate commands by hand and hope they were the four the gate ran, and a
# repair verified by a different set of checks than the ones that rejected it is
# not verified.
if [[ "$RERUN" -eq 1 ]]; then
    [[ "$cur" == "$BRANCH_SYNC" ]] || {
        bad "--rerun wants the parked merge on $BRANCH_SYNC; HEAD is $cur"
        exit 1
    }
    [[ -z "$(git status --porcelain)" ]] || {
        bad "working tree is dirty, commit the repair on $BRANCH_SYNC first."
        exit 1
    }
    # The newest UPSTREAM commit this branch contains, not its own HEAD. The
    # marker's first field is documented as an upstream sha and the first
    # --rerun to adopt anything wrote a local one into it: a repair commit's
    # hash, in the column something later reads to ask how far behind pret this
    # tree is.
    TARGET="$(git merge-base HEAD "$UPSTREAM" 2>/dev/null || git rev-parse HEAD)"
    # Counts are for the report only, and on a rerun the batch is whatever is
    # already merged: report it as such rather than inventing a number.
    n_take="$(git rev-list --count "$BRANCH_WORK..$BRANCH_SYNC" 2>/dev/null || echo '?')"
    n_pending="$n_take"
    base_frames=""; base_ending=""; base_skipped=""
    if [[ -f "$BASELINE" ]]; then
        IFS=$'\t' read -r base_frames base_ending base_skipped < "$BASELINE"
        say "baseline from the original attempt: ${base_frames:-none} frames, ${base_ending:-}"
    else
        loud "no recorded baseline; the reach is reported but not compared"
    fi
    say "re-running the gate on $BRANCH_SYNC ($n_take commit(s) ahead of $BRANCH_WORK)"
    gate_verdict
    exit 0
fi

# A tree already parked on the sync branch is the "needs hands" state. Starting
# a second merge on top of an unresolved one is how a two-commit problem
# becomes a twenty-commit problem.
if [[ "$cur" == "$BRANCH_SYNC" ]]; then
    loud "an earlier attempt is still on $BRANCH_SYNC and needs hands."
    [[ -f "$REPORT" ]] && sed 's/^/     /' "$REPORT"
    loud "fix it, then re-gate with: pc/sync_upstream.sh --rerun"
    loud "or discard the attempt with: pc/sync_upstream.sh --abort"
    exit 3
fi
if [[ "$cur" != "$BRANCH_WORK" ]]; then
    bad "on branch '$cur'; this syncs $BRANCH_WORK. Checkout $BRANCH_WORK first."
    exit 1
fi
if [[ -n "$(git status --porcelain)" ]]; then
    bad "working tree is dirty. Commit or stash first; a sync must be able to"
    bad "tell its own merge apart from work in progress."
    exit 1
fi

# A previous attempt that died mid-flight (killed run, power cut) left its
# branch behind. Discard rather than build on it: the sibling port learned
# this the same way.
if [[ -f "$INFLIGHT" ]]; then
    loud "discarding a previous failed attempt"
    git branch -qD "$BRANCH_SYNC" 2>/dev/null
    rm -f "$INFLIGHT"
fi

# ------------------------------------------------------------------- the delta

git fetch -q origin || { bad "fetch from pret failed (offline?)"; exit 1; }

git rev-parse -q --verify "$UPSTREAM" >/dev/null \
    || { bad "no $UPSTREAM, is origin still pret?"; exit 1; }

BASE="$(git merge-base "$BRANCH_WORK" "$UPSTREAM")"
# Derived from git, not from the marker file: the merge-base is the truth about
# what has been adopted, and a marker can only ever disagree with it. The
# marker records ATTEMPTS, for cadence and staleness reporting.
mapfile -t PENDING < <(git rev-list --reverse --first-parent "$BASE..$UPSTREAM")
n_pending="${#PENDING[@]}"

if [[ "$n_pending" -eq 0 ]]; then
    say "up to date with pret ($(git log -1 --format=%h "$UPSTREAM"))"
    printf '%s\t%s\tup-to-date\n' "$(git rev-parse "$UPSTREAM")" "$(date -Iseconds)" \
        > "$MARKER"
    exit 0
fi

if [[ "$MAX" -gt 0 && "$n_pending" -gt "$MAX" ]]; then
    TARGET="${PENDING[$((MAX - 1))]}"
    n_take="$MAX"
else
    TARGET="${PENDING[$((n_pending - 1))]}"
    n_take="$n_pending"
fi

say "$n_pending upstream commit(s) pending; taking $n_take up to $(git log -1 --format='%h %s' "$TARGET" | cut -c1-64)"

# What the batch touches, in the two categories that predict the gate's result:
# A patched path is a candidate dead patch, and a deleted/renamed source is a
# candidate stale object.
patched_hit=()
while IFS= read -r f; do
    [[ -f "pc/patches/$f.patch" ]] && patched_hit+=("$f")
done < <(git diff --name-only "$BASE" "$TARGET" -- src subprojects lib 2>/dev/null)
# Only .c files: prune_orphans maps objects back to sources, so a deleted meson
# wrap or header is not a stale object and saying "1 source deleted" about one
# is an instrument crying wolf. The first real batch this ran on deleted
# subprojects/nitrorom.wrap and nothing else, and the unfiltered count reported
# it as a source.
mapfile -t gone < <(git diff --name-only --diff-filter=DR "$BASE" "$TARGET" -- src subprojects lib 2>/dev/null \
                      | grep -E '\.c$' || true)

[[ "${#patched_hit[@]}" -gt 0 ]] \
    && say "touches ${#patched_hit[@]} patched file(s): ${patched_hit[*]}"
[[ "${#gone[@]}" -gt 0 ]] \
    && say "${#gone[@]} source(s) deleted or renamed upstream (stale objects will be pruned)"

if [[ "$DRYRUN" -eq 1 ]]; then
    say "--dry-run: nothing merged, nothing built, tree untouched"
    exit 0
fi

# ------------------------------------------------------------------- baseline

# Measured BEFORE the merge, on recomp, so the comparison afterwards is against
# this tree rather than against a number written down days ago. Needs a built
# port; without one there is no baseline and the gate says so instead of
# inventing one.
base_frames=""; base_ending=""
if [[ "$NOREACH" -eq 0 ]]; then
    if [[ -x build/pc/pokeplatinum ]]; then
        say "measuring the baseline reach on $BRANCH_WORK"
        run_reach
        base_frames="$G_frames"; base_ending="$G_ending"
        say "baseline: $base_frames frames, $base_ending"
    else
        loud "no built port on $BRANCH_WORK, no baseline reach to compare against"
    fi
fi
base_skipped=""
[[ -d build/pc/obj/game ]] \
    && base_skipped="$(find build/pc/obj/game -name '*.skipped' 2>/dev/null | wc -l | tr -d ' ')"

# Recorded now, while the tree is still the pre-merge one. --rerun reads this
# back so a repair is judged against the same baseline that rejected it.
printf '%s\t%s\t%s\n' "$base_frames" "$base_ending" "$base_skipped" > "$BASELINE"

# ---------------------------------------------------------------- the merge

: > "$INFLIGHT"
git checkout -q -B "$BRANCH_SYNC" "$BRANCH_WORK" || { bad "could not create $BRANCH_SYNC"; rm -f "$INFLIGHT"; exit 1; }

if ! git merge --no-edit -q "$TARGET" >/dev/null 2>&1; then
    mapfile -t conflicts < <(git diff --name-only --diff-filter=U)
    # .gitignore is the one file both sides own (the port adds 35 lines to it),
    # so a conflict there is expected and mechanical: keep both sides' lines.
    if [[ "${#conflicts[@]}" -eq 1 && "${conflicts[0]}" == ".gitignore" ]]; then
        say "resolving the expected .gitignore conflict by keeping both sides"
        gi_ours="$(mktemp)"; gi_theirs="$(mktemp)"
        git show ":2:.gitignore" > "$gi_ours" 2>/dev/null
        git show ":3:.gitignore" > "$gi_theirs" 2>/dev/null
        { cat "$gi_ours"; grep -vxF -f "$gi_ours" "$gi_theirs" 2>/dev/null; } > .gitignore
        rm -f "$gi_ours" "$gi_theirs"
        git add .gitignore
        git commit -q --no-edit || { bad "commit after resolve failed"; }
    else
        bad "merge conflicts outside .gitignore (${#conflicts[@]}):"
        printf '       %s\n' "${conflicts[@]:0:8}"
        bad "this needs hands. The port is not supposed to own these files --"
        bad "if one of them was edited in the tree, that edit belongs in"
        bad "pc/patches/ or pc/include/ instead."
        {
            printf 'upstream sync stopped: merge conflict outside .gitignore\n'
            printf 'target: %s\n' "$(git log -1 --format='%h %s' "$TARGET")"
            printf 'conflicts:\n'
            printf '  %s\n' "${conflicts[@]}"
            printf 'The merge is left in progress on %s. Resolve it, or run\n' "$BRANCH_SYNC"
            printf 'pc/sync_upstream.sh --abort to discard the attempt.\n'
        } > "$REPORT"
        exit 3
    fi
fi

say "merged $n_take commit(s); running the gate"
gate_verdict
