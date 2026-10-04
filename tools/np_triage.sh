#!/usr/bin/env bash
# np_triage.sh: run np_headless under a hang watchdog and explain hangs,
# traps and crashes in guest source terms.
#
#   tools/np_triage.sh [--bin NP_HEADLESS] [--stall SEC] [--out DIR] \
#                      <diamond|pearl|platinum> <rom.nds> [np_headless args...]
#
# Runs np_headless in the background with `--progress 1` (one "[progress]
# frame K" line per frame on stderr) and watches the frame counter:
#   - hang: no new frame for more than SEC seconds (default 5): the process is
#     sampled (`sample <pid> 3`), the guest frames of its hottest stack are
#     printed innermost first, plus the top-of-stack histogram; then killed.
#   - trap (np_headless "FAILED at frame K"): the run is repeated to frame K+1
#     under lldb with a breakpoint on np_rt_wasm_trap (the run is
#     deterministic, so the same trap recurs) and the guest backtrace printed.
#   - crash (a signal; the generated code has no signal handler, so a wild
#     guest access past the memory's size kills the process): the faulting
#     thread of the macOS crash report (~/Library/Logs/DiagnosticReports) is
#     printed.
#
# Guest frames: wasm2c names every function after the module's name section,
# `w2c_<game>_<mangled>` (characters other than [A-Za-z0-9_], a leading '_'
# and a '_' after '_' escaped as 0xHH), so the demangled name IS the C or asm
# symbol (`c2u$F` / `aw$sig$F` are the bridge's adapters around F). Names that
# hold a guest address (armrec's `sub_02xxxxxx`, local `_02xxxxxx` labels) are
# also looked up in the ROM build's link map (arm9.elf.xMAP) for the
# enclosing symbol; overlays share address windows, so all candidates show.
#
# Binary: --bin, else $NP_HEADLESS, else build/core-dp/np_headless (D/P) or
# build/core-plat/np_headless (Platinum) under the repo root.
# Output: DIR (default a fresh mktemp dir) gets run.log and sample.txt /
# lldb.txt / crash.ips.
# Exit status: 0 frames ran, 1 trap, crash or failure, 3 hang, 2 usage.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
bin=${NP_HEADLESS:-}
stall=5
out=
while [ $# -gt 0 ]; do
    case $1 in
    --bin) bin=$2; shift 2 ;;
    --stall) stall=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    *) break ;;
    esac
done
if [ $# -lt 2 ]; then
    sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
fi
game=$1 rom=$2
shift 2
case $game in
diamond | pearl) bin=${bin:-$root/build/core-dp/np_headless} ;;
platinum) bin=${bin:-$root/build/core-plat/np_headless} ;;
*) echo "np_triage: unknown game $game" >&2; exit 2 ;;
esac
[ -x "$bin" ] || { echo "np_triage: no np_headless at $bin" >&2; exit 2; }
[ -f "$rom" ] || { echo "np_triage: no ROM at $rom" >&2; exit 2; }
[ -n "$out" ] || out=$(mktemp -d "${TMPDIR:-/tmp}/np_triage.XXXXXX")
mkdir -p "$out"
log=$out/run.log

case $game in
diamond) xmap=$root/games/diamond/arm9/build/diamond.us/arm9.elf.xMAP ;;
pearl) xmap=$root/games/diamond/arm9/build/pearl.us/arm9.elf.xMAP ;;
*) xmap= ;;
esac

# analyze MODE FILE: MODE sample (sample(1) report), lldb (bt output) or ips
# (crash report); prints the guest stack in source names.
analyze() {
    python3 - "$1" "$2" "$game" "$xmap" <<'EOF'
import json, re, sys
mode, path, game, xmap = sys.argv[1:5]
text = open(path, errors="replace").read()
pre = "w2c_%s_" % game

def demangle(sym):
    n = sym[len(pre):] if sym.startswith(pre) else sym
    return re.sub(r"0x([0-9A-F]{2})", lambda m: chr(int(m.group(1), 16)), n)

# Link map symbols per object file ("ADDR SIZE .section NAME (lib obj)");
# asm functions have size 0, so "enclosing" = nearest preceding symbol of an
# object whose symbols span the address. Overlays share address windows, so
# several objects can match: all are listed, nearest first.
objs = {}
if xmap:
    try:
        for line in open(xmap, errors="replace"):
            m = re.match(r"\s+([0-9A-Fa-f]{8}) [0-9A-Fa-f]{8} (\.\S+)\s+([^\s$]\S*)\s*\((.*)\)", line)
            if m and m.group(2) in (".text", ".itcm", ".init") and m.group(3) != m.group(2):
                objs.setdefault(m.group(4), []).append((int(m.group(1), 16), m.group(3)))
    except OSError:
        pass
for syms in objs.values():
    syms.sort()
known = {n for syms in objs.values() for _, n in syms}

def where(name):
    m = re.search(r"(?<![0-9A-Fa-f])(0[12][0-9A-Fa-f]{6})(?![0-9A-Fa-f])", name)
    if not m or not objs or name in known:
        return ""
    a = int(m.group(1), 16) & ~1
    hits = []
    for syms in objs.values():
        if syms[0][0] <= a <= syms[-1][0] + 0x400:
            best = max((s for s in syms if s[0] <= a), key=lambda s: s[0])
            if best[1] != name and a - best[0] < 0x2000:
                hits.append((a - best[0], "%s+0x%x" % (best[1], a - best[0])))
    if not hits:
        return ""
    hits.sort()
    return "  [in %s%s]" % (" | ".join(h[1] for h in hits[:3]), " | ..." if len(hits) > 3 else "")

def show(frames):
    """frames: innermost first, raw symbol names."""
    for f in frames:
        if f.startswith(pre):
            d = demangle(f)
            print("  %s%s" % (d, where(d)))
        else:
            print("  (host) %s" % f)

def analyze_sample(text):
    # Call graph: one node per line, depth = column of its sample count.
    graph = text.split("Call graph:", 1)[-1].split("Total number in stack", 1)[0]
    node_re = re.compile(r"^([\s+!:|]*?)(\d+) (\S.*?)(?:  \(in ([^)]*)\).*)?$")
    threads, stack = [], []
    for line in graph.splitlines():
        m = node_re.match(line)
        if not m:
            continue
        depth, count, name = len(m.group(1)), int(m.group(2)), m.group(3).strip()
        node = {"count": count, "name": name, "kids": []}
        while stack and stack[-1][0] >= depth:
            stack.pop()
        (stack[-1][1]["kids"] if stack else threads).append(node)
        stack.append((depth, node))
    if not threads:
        print("  (no call graph)")
        return

    def has_guest(n):
        return n["name"].startswith(pre) or any(has_guest(k) for k in n["kids"])

    cands = [t for t in threads if has_guest(t)] or threads
    t = max(cands, key=lambda n: n["count"])
    total = t["count"]
    path_nodes, n = [], t
    while n["kids"]:
        n = max(n["kids"], key=lambda k: k["count"])
        path_nodes.append(n)
    guest = [p for p in path_nodes if p["name"].startswith(pre)]
    print("hottest stack of %s (%d samples), guest frames innermost first:" % (t["name"].split()[0], total))
    for p in reversed(guest[-40:]):
        d = demangle(p["name"])
        print("  %5.1f%%  %s%s" % (100.0 * p["count"] / total, d, where(d)))
    if path_nodes and not path_nodes[-1]["name"].startswith(pre):
        print("  (innermost host frame: %s)" % path_nodes[-1]["name"])

    top = text.split("Sort by top of stack", 1)
    if len(top) > 1:
        print("top of stack:")
        for line in top[1].splitlines()[1:12]:
            m = re.match(r"\s+(\S+)\s+\(in [^)]*\)\s+(\d+)", line)
            if m:
                d = demangle(m.group(1))
                print("  %6s  %s%s" % (m.group(2), d, where(d)))

if mode == "sample":
    analyze_sample(text)
elif mode == "lldb":
    bt = text.rsplit("(lldb) bt", 1)[-1]  # the stop report repeats frame #0
    frames = re.findall(r"frame #\d+: 0x[0-9a-f]+ \S+`([^\s(]+)", bt)
    print("guest backtrace at the trap, innermost first:")
    show([f for f in frames if not f.startswith(("np_rt_wasm_trap", "wasm_rt_trap"))][:60])
elif mode == "ips":
    j = json.loads(text.split("\n", 1)[1])
    ex = j.get("exception", {})
    print("crash: %s %s %s" % (ex.get("type", "?"), ex.get("signal", ""), ex.get("subtype", "")))
    th = j["threads"][j["faultingThread"]]
    show([f.get("symbol", "0x%x" % f.get("imageOffset", 0)) for f in th["frames"]][:60])
EOF
}

"$bin" "$game" "$rom" "$@" --progress 1 >"$log" 2>&1 &
pid=$!
trap 'kill -9 $pid 2>/dev/null' INT TERM
started=$(date +%s)
touch "$out/.started"

last_frame=0
last_change=$started
hung=0
while kill -0 $pid 2>/dev/null; do
    sleep 1
    frame=$(grep -a '^\[progress\] frame ' "$log" | tail -n 1 | awk '{print $3}')
    frame=${frame:-0}
    now=$(date +%s)
    if [ "$frame" != "$last_frame" ]; then
        last_frame=$frame
        last_change=$now
    elif [ $((now - last_change)) -gt "$stall" ]; then
        hung=1
        break
    fi
done

tail_log() {
    echo "log tail:"
    grep -av '^\[progress\]' "$log" | tail -n "$1" | sed 's/^/  /'
    echo "files: $out"
}

if [ $hung = 1 ]; then
    echo "HANG: frame counter stuck at $last_frame for over ${stall}s (pid $pid), sampling"
    sample $pid 3 -file "$out/sample.txt" >/dev/null 2>&1
    kill -9 $pid 2>/dev/null
    wait $pid 2>/dev/null
    analyze sample "$out/sample.txt"
    tail_log 15
    exit 3
fi

wait $pid 2>/dev/null
rc=$?
if grep -aq '^FAILED at frame' "$log"; then
    k=$(sed -n 's/^FAILED at frame \([0-9]*\):.*/\1/p' "$log" | tail -n 1)
    echo "TRAP at frame $k: $(sed -n 's/^FAILED at frame [0-9]*: //p' "$log" | tail -n 1)"
    echo "re-running to frame $((k + 1)) under lldb for the backtrace"
    lldb --batch -o 'breakpoint set -n np_rt_wasm_trap' -o run -o 'bt 80' -k 'bt 80' -k 'kill' \
        -- "$bin" "$game" "$rom" "$@" --frames $((k + 1)) >"$out/lldb.txt" 2>&1 </dev/null
    analyze lldb "$out/lldb.txt"
    tail_log 10
    exit 1
fi
if [ $rc -gt 128 ]; then
    echo "CRASH: signal $((rc - 128)) after frame $last_frame"
    ips=
    for _ in $(seq 1 20); do
        ips=$(find ~/Library/Logs/DiagnosticReports -maxdepth 1 -name "$(basename "$bin")-*.ips" \
            -newer "$out/.started" 2>/dev/null | sort | tail -n 1)
        [ -n "$ips" ] && break
        sleep 1
    done
    if [ -n "$ips" ]; then
        cp "$ips" "$out/crash.ips"
        analyze ips "$out/crash.ips"
    else
        echo "  (no crash report; re-run under lldb: lldb -- $bin $game $rom $* )"
    fi
    tail_log 10
    exit 1
fi
if [ $rc != 0 ]; then
    echo "FAILURE: exit $rc after frame $last_frame"
    tail_log 25
    exit 1
fi
grep -av '^\[progress\]' "$log" | grep -a '^frames \|^status\|^\[headless\]'
echo "files: $out"
exit 0
