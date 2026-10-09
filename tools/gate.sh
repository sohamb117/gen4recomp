#!/usr/bin/env bash
# tools/gate.sh: the regression gate for a branch, only as heavy as its diff.
#
#   tools/gate.sh [--dry-run] [--base REF] [BRANCH]
#   tools/gate.sh --refresh-cores [--base REF]
#
# The changes are `git diff --name-only BASE...BRANCH` (BASE main, BRANCH
# HEAD by default) and, when BRANCH is checked out here, its uncommitted and
# untracked files. They select what runs:
#
# - tests/dp/regress.sh cases (--only), for each core whose inputs changed
#   (listed in regress.sh's header, printed by `regress.sh --inputs GROUP`):
#   D/P (d-*, p-*) for games/diamond and the Platinum port code it builds,
#   Pt (plat-*) for games/platinum, R/S/E (e-*, r-*, s-*) for
#   games/gba-common, games/emerald and games/ruby, and every core for core/,
#   shell/src/net.[ch], tools/wasm2c_postprocess.py and tools/toolchains.lock.
#   A C source or header whose change is only comments and whitespace does
#   not count. A changed file a case names (a schedule) selects that case, a
#   changed case line in tests/dp/expected.txt that case, a changed
#   regress.sh every case. They run with NP_GATE_CORES (default
#   ~/Library/Caches/nativeplat-gate-cores, prebuilt cores and the commit
#   each was built from, as regress.sh's header says), so a selected core
#   whose inputs are unchanged since that commit is not rebuilt.
# - the e2e milestone checks, always (seconds): tests/e2e/tools/plan.py
#   --check, and tests/e2e/run.py --check for each affected game: the game
#   of a changed tests/e2e/<game>/, every game for any other change to
#   tests/e2e/, tests/gameplay/ or a *.py.
#
# Anything else (shell/, features/, docs/, ...) needs nothing more.
# --dry-run prints the plan without running it.
#
# --refresh-cores brings the gate cores up to BASE after core inputs change
# there: NP_GATE_CORES's core-* and core-*.rev link into another checkout's
# build/ (on this machine nativeplat-integrate, which no one works in). That
# checkout is moved to BASE (detached; refused with tracked changes). A core
# whose inputs changed since its .rev in comments only (C files, as above)
# is stamped BASE as it is; its regress.sh rebuilds, incrementally, each
# other core whose inputs differ, with NP_MIN_FREE_GB=3 (a build step under
# 3 GiB free fails), then stamps it and runs its cases. Exit status: 0 all
# passed, 1 a failure, 2 usage.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
base=main branch=HEAD dry=0 refresh=0
while [ $# -gt 0 ]; do
    case $1 in
    --dry-run) dry=1; shift ;;
    --base) base=$2; shift 2 ;;
    --refresh-cores) refresh=1; shift ;;
    -*) sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
    *) branch=$1; shift ;;
    esac
done
gates=${NP_GATE_CORES:-$HOME/Library/Caches/nativeplat-gate-cores}

# comment_only OLD NEW HERE: of the C files on stdin, those whose change
# from commit OLD to NEW (HERE=1: the working tree) is only comments and
# whitespace (literals kept as they are).
comment_only() {
    python3 -c '
import re, subprocess, sys

def blob(spec):
    r = subprocess.run(["git", "show", spec], capture_output=True)
    return r.stdout.decode("utf-8", "surrogateescape") if r.returncode == 0 else None

def code(text):
    """text without comments: each comment one space, whitespace runs
    outside string and character literals squeezed, blank lines dropped"""
    parts, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            parts.append((False, " "))
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 2 if text[i] == "\\" else 1
            parts.append((False, " "))
        elif text[i] in "\"\x27":
            j = i + 1
            while j < n and text[j] not in (text[i], "\n"):
                j += 2 if text[j] == "\\" else 1
            parts.append((True, text[i:j + 1]))
            i = j + 1
        else:
            j = i
            while j < n and text[j] not in "\"\x27/":
                j += 1
            j = max(j, i + 1)
            parts.append((False, text[i:j]))
            i = j
    out, buf = [], []
    for lit, s in parts + [(True, "")]:
        if not lit:
            buf.append(s)
            continue
        out.append(re.sub(r"\s*\n\s*", "\n", re.sub(r"[ \t\f\v]+", " ", "".join(buf))))
        buf = []
        out.append(s)
    return "".join(out).strip()

base, head, here = sys.argv[1:4]
for f in sys.stdin.read().splitlines():
    old = blob(base + ":" + f)
    if here == "1":
        try:
            new = open(f, encoding="utf-8", errors="surrogateescape").read()
        except OSError:
            new = None
    else:
        new = blob(head + ":" + f)
    if old is not None and new is not None and code(old) == code(new):
        print(f)
' "$@"
}
is_c() { case $1 in *.c | *.h | *.inc | *.c.in) return 0 ;; esac; return 1; }

if [ $refresh = 1 ]; then
    host=$(cd -P "$gates/core-dp/../.." 2>/dev/null && pwd) &&
        git -C "$host" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
        { echo "gate: $gates/core-dp is not in a checkout's build/" >&2; exit 2; }
    if [ "$(cd -P "$host" && pwd)" = "$(pwd -P)" ]; then
        echo "gate: the gate cores are this checkout's own; refresh them from a checkout of their own" >&2
        exit 2
    fi
    if [ -n "$(git -C "$host" status --porcelain --untracked-files=no)" ]; then
        echo "gate: $host has tracked changes; not moving it" >&2
        exit 1
    fi
    git -C "$host" checkout -q --detach "$base"
    rev=$(git -C "$host" rev-parse HEAD)
    stale=" "
    for grp in dp plat rse; do
        r=$(cat "$gates/core-$grp.rev" 2>/dev/null || true)
        if [ -z "$r" ]; then stale="$stale$grp "; continue; fi
        # shellcheck disable=SC2046 # pathspec word list
        diff=$(git -C "$host" diff --no-renames --name-only "$r" "$rev" -- $("$host/tests/dp/regress.sh" --inputs $grp))
        if [ -z "$diff" ]; then
            echo "gate: core-$grp: built from ${r:0:9}, its inputs unchanged since"
            continue
        fi
        # Changed only in comments (C files, as the gate counts them): the
        # core stands for $rev too.
        code=$(printf '%s\n' "$diff" | while read -r f; do is_c "$f" || echo "$f"; done)
        if [ -z "$code" ] && [ "$(printf '%s\n' "$diff" | comment_only "$r" "$rev" 0)" = "$diff" ]; then
            echo "$rev" >"$gates/core-$grp.rev"
            echo "gate: core-$grp: built from ${r:0:9}, its inputs changed in comments only since; stamped ${rev:0:9}"
            continue
        fi
        stale="$stale$grp "
    done
    only=()
    while read -r name game rest; do
        case $name in '' | '#'*) continue ;; esac
        case $game in diamond | pearl) grp=dp ;; platinum) grp=plat ;; *) grp=rse ;; esac
        case $stale in *" $grp "*) only+=(--only "$name") ;; esac
    done <"$host/tests/dp/expected.txt"
    if [ ${#only[@]} = 0 ]; then
        echo "gate: $gates is up to date with $base ($(git -C "$host" rev-parse --short HEAD))"
        exit 0
    fi
    echo "gate: rebuilding$stale in $host at $base ($(git -C "$host" rev-parse --short HEAD))"
    cd "$host"
    NP_GATE_CORES= NP_MIN_FREE_GB=${NP_MIN_FREE_GB:-3} exec tests/dp/regress.sh "${only[@]}"
fi

head=$(git rev-parse --verify --quiet "$branch^{commit}") || { echo "gate: no commit $branch" >&2; exit 2; }
mb=$(git merge-base "$base" "$head")
here=0
[ "$head" = "$(git rev-parse HEAD)" ] && here=1
if [ $here = 0 ] && [ $dry = 0 ]; then
    echo "gate: $branch is not checked out here; run the gate in its worktree (or --dry-run)" >&2
    exit 2
fi
if [ $here = 1 ]; then
    changed=$({ git diff --no-renames --name-only "$mb"; git ls-files --others --exclude-standard; } | sort -u)
    show() { cat "$1"; }
else
    changed=$(git diff --no-renames --name-only "$mb" "$head")
    show() { git show "$head:$1"; }
fi
echo "gate: $branch against $base (merge base $(git rev-parse --short "$mb")): $(printf '%s' "$changed" | grep -c . || true) changed files"

under() { # under FILE SPEC...: FILE is one of the pathspecs or below one
    local f=$1 s
    shift
    for s in "$@"; do
        case $f in "$s" | "$s"/*) return 0 ;; esac
    done
    return 1
}
inputs_dp=$(tests/dp/regress.sh --inputs dp)
inputs_plat=$(tests/dp/regress.sh --inputs plat)
inputs_rse=$(tests/dp/regress.sh --inputs rse)

# The changed C files among the core inputs, and those changed only in
# comments and whitespace.
core_cfiles() {
    local f
    printf '%s\n' "$changed" | while read -r f; do
        is_c "$f" || continue
        # shellcheck disable=SC2086 # pathspec word lists
        if under "$f" $inputs_dp $inputs_plat $inputs_rse; then echo "$f"; fi
    done
}
cfiles=$(core_cfiles)
comments=
if [ -n "$cfiles" ]; then comments=$(printf '%s\n' "$cfiles" | comment_only "$mb" "$head" "$here"); fi

# What each change selects.
nl='
'
groups=" " cases_by_name=" " e2e=" " all_e2e=0 all_cases=0 hits=
while read -r f; do
    [ -n "$f" ] || continue
    case "$nl$comments$nl" in *"$nl$f$nl"*) echo "gate: $f: comments only"; continue ;; esac
    # shellcheck disable=SC2086
    if under "$f" $inputs_dp; then groups="$groups dp "; fi
    # shellcheck disable=SC2086
    if under "$f" $inputs_plat; then groups="$groups plat "; fi
    # shellcheck disable=SC2086
    if under "$f" $inputs_rse; then groups="$groups rse "; fi
    # shellcheck disable=SC2086
    if under "$f" $inputs_dp $inputs_plat $inputs_rse; then hits="$hits$f$nl"; fi
    case $f in
    tests/dp/regress.sh) all_cases=1 ;;
    tests/dp/expected.txt)
        if [ $here = 1 ]; then diff=$(git diff -U0 "$mb" -- "$f"); else diff=$(git diff -U0 "$mb" "$head" -- "$f"); fi
        for n in $(printf '%s\n' "$diff" | sed -n 's/^[-+]\([^-+#[:space:]][^[:space:]]*\).*/\1/p'); do
            cases_by_name="$cases_by_name$n "
        done
        ;;
    esac
    case $f in
    tests/e2e/*/*)
        g=${f#tests/e2e/}
        g=${g%%/*}
        case $g in
        platinum | diamond | pearl | emerald | ruby | sapphire) e2e="$e2e$g " ;;
        *) all_e2e=1 ;;
        esac
        ;;
    tests/e2e/* | tests/gameplay/* | *.py) all_e2e=1 ;;
    esac
done <<EOF
$changed
EOF
if [ $all_e2e = 1 ]; then
    e2e="platinum diamond pearl emerald ruby sapphire"
else
    e2e=$(printf '%s\n' $e2e | awk '!seen[$0]++' | xargs)
fi

# The selected regress.sh cases: those of a selected core, named by a changed
# case line, or using a changed file (a schedule).
cases= ds= gba=
while read -r name game frames hash args; do
    case $name in '' | '#'*) continue ;; esac
    case $game in
    diamond | pearl) grp=dp ;;
    platinum) grp=plat ;;
    *) grp=rse ;;
    esac
    pick=$all_cases
    case $groups in *" $grp "*) pick=1 ;; esac
    case $cases_by_name in *" $name "*) pick=1 ;; esac
    for a in $args; do
        case "$nl$changed$nl" in *"$nl$a$nl"*) pick=1 ;; esac
    done
    if [ $pick = 1 ]; then
        cases="$cases $name"
        if [ $grp = rse ]; then gba="$gba $name"; else ds="$ds $name"; fi
    fi
done <<EOF
$(show tests/dp/expected.txt)
EOF

if [ -n "$hits" ]; then
    n=$(printf '%s' "$hits" | grep -c .)
    echo "gate: $n changed core inputs: $(printf '%s' "$hits" | head -n 3 | xargs)$([ "$n" -gt 3 ] && echo " ...")"
fi
if [ -n "$ds" ]; then echo "gate: D/P/Pt cases:$ds"; else echo "no D/P/Pt inputs changed: skipping"; fi
if [ -n "$gba" ]; then echo "gate: R/S/E cases:$gba"; else echo "no R/S/E inputs changed: skipping"; fi
echo "gate: e2e checks: plan.py${e2e:+, run.py --check for $e2e}"
if [ $dry = 1 ]; then exit 0; fi

status=0
for g in $e2e; do
    if out=$(python3 tests/e2e/run.py --game "$g" --check 2>&1); then
        echo "ok   e2e check $g"
    else
        echo "FAIL e2e check $g:"
        printf '%s\n' "$out" | grep -v ' ok$' | sed 's/^/    /'
        status=1
    fi
done
if out=$(python3 tests/e2e/tools/plan.py --check 2>&1); then
    echo "ok   plan.py --check"
else
    echo "FAIL plan.py --check:"
    printf '%s\n' "$out" | sed 's/^/    /'
    status=1
fi
if [ -n "$cases" ]; then
    only=()
    for c in $cases; do only+=(--only "$c"); done
    NP_GATE_CORES=$gates tests/dp/regress.sh "${only[@]}" ||
        status=1
fi
exit $status
