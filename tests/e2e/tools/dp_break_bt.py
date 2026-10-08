#!/usr/bin/python3
"""dp_break_bt.py - the guest call chain at a function, from a replayed D/P milestone session under lldb.

    /usr/bin/python3 tests/e2e/tools/dp_break_bt.py --cmds /tmp/x/cmds.txt --save build/e2e/<out>/<ms>/start.sav \
        [--game diamond] [--fn PrintErrorMessageAndReset --fn GF_AssertFail] [--fn 'sub_0205EC6C:$x1 == 0']
        [-e NAME=VALUE ...]

Record the session first as tools/dp_lcrng_trace.py says (a wrapper at build/gameplay/np_gp-core-dp that tees the
--serve command stream to a file; np_gp's answers do not feed back, so the stream plus the milestone's start.sav
replay the run exactly). This launches the copy under lldb, breaks on each w2c_<game>_<fn> and prints the first
stop's backtrace (the recompiled guest functions keep their decomp names; the wrap_ thunks are dropped) and the
stopped function's guest arguments: x0 is the wasm instance, x1.. are r0.. . `FN:COND` breaks only when the lldb
expression COND holds (e.g. '$x1 == 0': the guest's r0 is NULL).
D/P's comm error screen ("A communication error has occurred", then OS_ResetSystem) is PrintErrorMessageAndReset,
called by GF_AssertFail or a failed Heap_Alloc while the link layer is up (error_handling.c, heap.c AllocFail).
Attaching to a live run does not work: the stop interrupts np_gp's blocking stdin read and it exits as on EOF.
/usr/bin/python3 because lldb's Python module is built for it. The ROM, save and binary are copied to a temp dir
first: debugserver cannot open files under ~/Documents (TCC).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, subprocess.run(["lldb", "-P"], capture_output=True, text=True).stdout.strip())
import lldb  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
ROMS = {"diamond": "games/diamond/build/diamond.us/pokediamond.us.nds",
        "pearl": "games/diamond/build/pearl.us/pokepearl.us.nds"}

ap = argparse.ArgumentParser()
ap.add_argument("--cmds", required=True)
ap.add_argument("--save", required=True)
ap.add_argument("--game", default="diamond", choices=sorted(ROMS))
ap.add_argument("--fn", action="append")
ap.add_argument("-e", dest="env", action="append", default=[])
ap.add_argument("--depth", type=int, default=60)
a = ap.parse_args()
fns = a.fn or ["PrintErrorMessageAndReset", "GF_AssertFail"]
tmp = tempfile.mkdtemp(prefix="brbt.")
try:
    gp, rom, sav, cmds = (os.path.join(tmp, n) for n in ("np_gp", "rom.nds", "t.sav", "cmds.txt"))
    shutil.copyfile(os.path.join(ROOT, "build", "gameplay", "np_gp-core-dp"), gp)
    os.chmod(gp, 0o755)
    shutil.copyfile(os.path.join(ROOT, ROMS[a.game]), rom)
    shutil.copyfile(a.save, sav)
    shutil.copyfile(a.cmds, cmds)
    dbg = lldb.SBDebugger.Create()
    dbg.SetAsync(False)
    target = dbg.CreateTarget(gp)
    for f in fns:
        name, _, cond = f.partition(":")
        bp = target.BreakpointCreateByName("w2c_%s_%s" % (a.game, name))
        if cond:
            bp.SetCondition(cond)
    args = [rom, "--game", a.game, "--serve", "1", "-e", "PC_E2E=1", "--save", sav, "-o", "text_instant=1"]
    for e in a.env:
        args += ["-e", e]
    info = lldb.SBLaunchInfo(args)
    info.AddOpenFileAction(0, cmds, True, False)
    info.AddOpenFileAction(1, os.path.join(tmp, "out.txt"), False, True)
    info.AddOpenFileAction(2, os.path.join(tmp, "err.txt"), False, True)
    err = lldb.SBError()
    proc = target.Launch(info, err)
    if not err.Success():
        raise SystemExit("launch: %s" % err)
    dbg.HandleCommand("process handle SIGPIPE -s false -p true -n false")
    while proc.GetState() == lldb.eStateStopped:
        th = proc.GetSelectedThread()
        if th.GetStopReason() == lldb.eStopReasonBreakpoint:
            fr0 = th.GetFrameAtIndex(0)
            print("args r0..r3:", " ".join("%#x" % fr0.FindRegister("x%d" % i).GetValueAsUnsigned() for i in range(1, 5)))
            for i in range(min(th.GetNumFrames(), a.depth)):
                name = th.GetFrameAtIndex(i).GetFunctionName() or "?"
                if not name.startswith("wrap_"):
                    print("  #%d %s" % (i, name))
            proc.Kill()
            break
        proc.Continue()
    else:
        print("no stop; process state", proc.GetState())
        print(open(os.path.join(tmp, "err.txt")).read()[-2000:])
finally:
    shutil.rmtree(tmp, ignore_errors=True)
