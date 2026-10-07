#!/usr/bin/env python3
"""dp_lcrng_trace.py - who writes D/P's LCRNG state before a breakpoint, traced with lldb on np_gp.

The milestone session is replayed, not driven: np_gp --serve reads one command per line and its answers do not
feed back once recorded, so a recorded command stream plus the milestone's start.sav reproduce the run exactly.

  1. record: put a wrapper at build/gameplay/np_gp-core-dp that, for --serve runs, does
       tee /tmp/x/cmds.txt | <the real np_gp> "$@"
     run the milestone once (run.py), then restore the real binary.
  2. trace:  tools/dp_lcrng_trace.py --cmds /tmp/x/cmds.txt --save build/e2e/<out>/<ms>/start.sav \
                 [--clock "2009-03-22 21:00:00"] [--game diamond] [--at w2c_diamond_ov05_021E17A0+404]
     prints every writer of sLCRNG_State (SetLCRNGSeed, LCRandom, InitializeMainRNG's inlined store) and, at
     `--at`, x0 (default: ov05_021E17A0 just after its LCRandom call = the Vs. Seeker roll's raw value).

lldb launches through debugserver, which cannot open files under ~/Documents (TCC): the ROM, save and binary are
copied to a temp dir first. The state's wasm address comes from w2c_<game>_GetLCRNGSeed's disassembly.
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
ROMS = {"diamond": "games/diamond/build/diamond.us/pokediamond.us.nds",
        "pearl": "games/diamond/build/pearl.us/pokepearl.us.nds"}

HOOKS = r'''
import collections
C = collections.Counter()
N = [0]
def onwrite(frame, w, d):
    t = frame.GetThread()
    C[tuple(t.GetFrameAtIndex(i).GetFunctionName() for i in range(3))] += 1
    N[0] += 1
    return False
def at(frame, bp_loc, d):
    print("AT x0=%#x after %d state writes" % (frame.FindRegister("x0").GetValueAsUnsigned(), N[0]))
    for k, v in C.most_common(40):
        print("  %6d %s" % (v, " <- ".join(k)))
    return False
'''


def state_offset(gp, game):
    out = subprocess.run(["lldb", "-b", "-o", "disassemble -n w2c_%s_GetLCRNGSeed" % game, gp],
                         capture_output=True, text=True).stdout
    lo = re.search(r"mov\s+w9, #(0x[0-9a-f]+)", out)
    hi = re.search(r"movk\s+w9, #(0x[0-9a-f]+), lsl #16", out)
    if not (lo and hi):
        raise SystemExit("no state offset in w2c_%s_GetLCRNGSeed:\n%s" % (game, out))
    return int(hi.group(1), 16) << 16 | int(lo.group(1), 16)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cmds", required=True)
    ap.add_argument("--save", required=True)
    ap.add_argument("--game", default="diamond", choices=sorted(ROMS))
    ap.add_argument("--clock")
    ap.add_argument("--at", default=None)
    a = ap.parse_args()
    at = a.at or "w2c_%s_ov05_021E17A0+404" % a.game
    name, off = (at.split("+") + ["0"])[:2]
    tmp = tempfile.mkdtemp(prefix="lcrng.")
    try:
        gp = os.path.join(tmp, "np_gp")
        shutil.copyfile(os.path.join(ROOT, "build", "gameplay", "np_gp-core-dp"), gp)
        os.chmod(gp, 0o755)
        rom, sav, cmds = (os.path.join(tmp, n) for n in ("rom.nds", "t.sav", "cmds.txt"))
        shutil.copyfile(os.path.join(ROOT, ROMS[a.game]), rom)
        shutil.copyfile(a.save, sav)
        shutil.copyfile(a.cmds, cmds)
        with open(os.path.join(tmp, "lcrng_hooks.py"), "w") as f:
            f.write(HOOKS)
        env = "-e PC_E2E=1" + (" -e 'PC_RTC=%s'" % a.clock if a.clock else "")
        script = "\n".join([
            "target create %s" % gp,
            "command script import %s" % os.path.join(tmp, "lcrng_hooks.py"),
            "process handle SIGPIPE -s false -p true -n false",
            # the first SetLCRNGSeed stops once: x0 is the wasm instance, [x0+0x18] its memory base
            "br set -n w2c_%s_SetLCRNGSeed -K false -o true" % a.game,
            "br set -n %s -K false -R %s" % (name, off),
            "br command add -F lcrng_hooks.at 2",
            "process launch -i %s -o %s/out.txt -e %s/err.txt -- %s --game %s --serve 1 %s --save %s -o text_instant=1"
            % (cmds, tmp, tmp, rom, a.game, env, sav),
            "watchpoint set expression -w write -s 4 -- (*(unsigned long*)($x0+0x18)) + %#x" % state_offset(gp, a.game),
            "watchpoint command add -F lcrng_hooks.onwrite 1",
            "continue",
        ])
        with open(os.path.join(tmp, "trace.lldb"), "w") as f:
            f.write(script + "\n")
        out = subprocess.run(["lldb", "-b", "-s", os.path.join(tmp, "trace.lldb")], capture_output=True,
                             text=True).stdout
        hits = [l for l in out.splitlines() if l.startswith("AT ") or re.match(r"\s+\d+ w2c_", l)]
        print("\n".join(hits) if hits else out[-3000:])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
