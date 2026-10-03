#!/usr/bin/env python3
"""Run the port and melonDS over the same ROM and report where they disagree.

This tree builds the ROM, so both sides execute the same image. That is the
whole reason the comparison is worth making: a port diffed against an emulator
running someone else's dump has to live with every difference that follows from
the two not being the same program, and this one does not. A divergence here is
the port's.

  $ make -f pc/Makefile diff FRAMES=600
  $ pc/diff/pcdiff.py --frames 600 --every 10
  $ pc/diff/pcdiff.py --frames 600 --input pc/replays/new-game.txt

The two `boot` checkpoints are NOT the same instant; the oracle's is before
the ARM9's first instruction and the port's is at the game's entry point, with
the cartridge boot already done by the host, so the traces are aligned by
their pictures and the offset is printed rather than assumed.
"""

import argparse
import atexit
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PORT = os.path.join(ROOT, "build", "pc", "pokeplatinum")
MELON = os.path.join(ROOT, "build", "pc", "pcdiff-melon")
ROM = os.path.join(ROOT, "build", "rom", "pokeplatinum.us.nds")


class Trace:
    def __init__(self):
        self.spans = []          # (addr, len, name) in the order written
        self.cps = []            # [{label, frame, fb, spu, d{addr: digest}}]
        self.stopped = None
        self.complete = False


def parse(path):
    t = Trace()
    cp = None
    with open(path) as f:
        for line in f:
            w = line.split()
            if not w:
                continue
            if w[0] == "span":
                t.spans.append((int(w[1], 16), int(w[2]), w[3]))
            elif w[0] == "cp":
                frame = None
                if w[1].startswith("frame:"):
                    frame = int(w[1].split(":")[1])
                cp = {"label": w[1], "frame": frame, "fb": None,
                      "spu": [], "d": {}}
                t.cps.append(cp)
            elif cp is None:
                continue
            elif w[0] == "fb":
                cp["fb"] = (w[1], w[2])
            elif w[0] == "spu":
                cp["spu"].append(" ".join(w[1:]))
            elif w[0] == "d":
                cp["d"][int(w[1], 16)] = w[3]
            elif w[0] == "stopped":
                t.stopped = line.strip()
            elif w[0] == "end":
                t.complete = True
    return t


def align(port, melon):
    """The frame offset that best lines the two traces up, by picture.

    The port boots faster than the console; it does not execute the
    cartridge's own boot, and its hardware models do not wait for anything,
    so its frame N is the console's frame N+k for some k this has to find
    rather than assume. The picture is what it searches on: it is the one
    signal both sides certainly compute the same way, and a run where nothing
    is drawn yet gives the same digest at every frame, which is exactly the
    case where no offset is discoverable and the report should say so.

    Returns (offset_in_frames, matched, total, ambiguous).
    """
    pf = [c for c in port.cps if c["frame"] is not None and c["fb"]]
    mf = {c["frame"]: c for c in melon.cps if c["frame"] is not None and c["fb"]}
    if not pf or not mf:
        return None, 0, 0, False

    mframes = sorted(mf)
    best = []
    for off in range(0, max(mframes) + 1):
        hit = tot = 0
        for c in pf:
            m = mf.get(c["frame"] + off)
            if m is None:
                continue
            tot += 1
            if c["fb"] == m["fb"]:
                hit += 1
        if tot:
            best.append((hit, -off, off, tot))
    if not best:
        return None, 0, 0, False
    best.sort(reverse=True)
    hit, _, off, tot = best[0]
    # A picture that never changes matches at every offset, and an offset
    # picked out of a tie is a number with no evidence behind it.
    ties = sum(1 for b in best if b[0] == hit)
    return off, hit, tot, ties > 1


# The spans that cannot legitimately agree, and why.
#
# The report prints every span it has, which is right: deciding in advance
# which ones are comparable is how a harness ends up measuring nothing and
# calling it a pass. But four of them can never say anything, for reasons that
# are known and are not the port's, and leaving them in the table unlabelled
# lets them pose as evidence, three of the four score a perfect match by
# being zero on both sides.
#
# So they are named here with the reason, and the reason is what the report
# prints beside them. A span that stops being blind is a span whose line stops
# matching what it says.
BLIND = {
    "ITCM": "the oracle's bus read returns zeros here, ITCM is core-local "
            "and not visible to ARM9Read32",
    "ARM7-WRAM/IWRAM": "same: the ARM9 bus the oracle reads through does not "
                       "reach the ARM7's own WRAM",
    "port-window": "host state at a guest address by design; the port keeps "
                   "objects here that the console has no equivalent for",
    "palette": "read 0 while the 2D engines are power-gated; the console's "
               "palette memory is not on the ARM9 bus when POWCNT says off",
    "OAM": "read 0 while the 2D engines are power-gated, for the same reason "
           "as the palette",
}


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("pcdiff: %s failed (%d)\n%s\n"
                         % (what, r.returncode, r.stderr[-2000:]))
        sys.exit(1)
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rom", default=ROM)
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--every", type=int, default=10,
                    help="checkpoint interval in frames (default 10)")
    ap.add_argument("--input", help="the input script, given to both sides")
    ap.add_argument("--sav", help="the save image to start from")
    # The PORT'S OWN default, not an epoch nobody runs at. Both sides are
    # given it explicitly, because a default each side picks for itself is a
    # difference in the run rather than in the port: this defaulted to
    # 2000-01-01 while the port defaulted to 2009-03-22, nine years apart in
    # every comparison ever made with it.
    ap.add_argument("--rtc", default="2009-03-22T10:00:00")
    ap.add_argument("--offset", type=int,
                    help="the frame offset, instead of searching for it")
    ap.add_argument("--keep", metavar="DIR",
                    help="keep the traces here instead of a temp directory")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    for p, what in ((PORT, "the port"), (MELON, "the oracle")):
        if not os.path.exists(p):
            sys.stderr.write("pcdiff: no %s at %s, `make -f pc/Makefile %s`\n"
                             % (what, p, "melon" if p == MELON else ""))
            return 2
    if not os.path.exists(args.rom):
        sys.stderr.write("pcdiff: no ROM at %s, `ninja -C build/rom`\n" % args.rom)
        return 2

    import shutil
    import tempfile
    tmp = args.keep or tempfile.mkdtemp(prefix="pcdiff-")
    os.makedirs(tmp, exist_ok=True)
    # --keep is the caller's directory and stays. A temp directory this made
    # is this program's to remove: it left one per invocation behind, each
    # holding a few megabytes of traces, and nothing ever collected them.
    if not args.keep:
        atexit.register(shutil.rmtree, tmp, True)
    spans = os.path.join(tmp, "spans.txt")
    ptrace = os.path.join(tmp, "port.txt")
    mtrace = os.path.join(tmp, "melon.txt")

    # The port writes the span list and the oracle reads it: one source of
    # truth for the memory map, so the two sides cannot end up digesting
    # different addresses and calling the result agreement.
    run([PORT, "--rom", args.rom, "--diff-spans", spans], "--diff-spans")

    def port_run(trace, frames, script, rtc):
        penv = dict(os.environ)
        penv.update({"PC_ROM": args.rom, "PC_SAVE": args.sav or "none",
                     "PC_FRAMES": str(frames), "PC_DIFF_TRACE": trace,
                     "PC_DIFF_EVERY": str(args.every),
                     "PC_RTC": rtc.replace("T", " ")})
        if script:
            penv["PC_INPUT"] = script
        r = subprocess.run([PORT], env=penv, capture_output=True, text=True)
        if r.returncode != 0:
            # Not a warning. A port that died has written a truncated trace,
            # and every number below it is computed over a run that did not
            # finish; which reads as agreement thinning out rather than as a
            # crash. This used to print and carry on.
            sys.stderr.write("pcdiff: the port exited %d, the trace it wrote "
                             "is not a whole run\n%s\n"
                             % (r.returncode, r.stderr[-2000:]))
            sys.exit(1)

    def melon_run(trace, frames, script, rtc):
        cmd = [MELON, "--rom", args.rom, "--spans", spans, "--trace", trace,
               "--frames", str(frames), "--every", str(args.every),
               "--rtc", rtc]
        if script:
            cmd += ["--input", script]
        if args.sav:
            cmd += ["--sav", args.sav]
        run(cmd, "the oracle")

    # The script is phase-shifted for the oracle, and this is not a detail.
    # The port's frame N is the console's N+k; the port does not execute the
    # cartridge boot, so feeding both sides the same frame numbers lands
    # every key k frames late on the console. That was every input-driven
    # comparison this harness had ever made.
    #
    # k is measured rather than assumed: an input-free probe over the first
    # few hundred frames aligns the two by picture, exactly as the report
    # does, and the shifted copy is written from what it found. --offset skips
    # the probe when the number is already known.
    shift = args.offset
    if args.input and shift is None:
        pp = os.path.join(tmp, "probe-port.txt")
        pm = os.path.join(tmp, "probe-melon.txt")
        pframes = min(args.frames, 600)
        port_run(pp, pframes, None, args.rtc)
        melon_run(pm, pframes, None, args.rtc)
        shift, phit, ptot, pamb = align(parse(pp), parse(pm))
        if shift is None or pamb:
            sys.stderr.write(
                "pcdiff: the input-free probe could not align the two sides "
                "(%s), so the script cannot be phase-shifted. Give --offset.\n"
                % ("nothing drawn" if shift is None else "several offsets tie"))
            return 1
        if not args.quiet:
            print("pcdiff: probe aligned at +%d frames (%d/%d pictures); "
                  "shifting the script by that" % (shift, phit, ptot))

    mscript = args.input
    if args.input and shift:
        mscript = os.path.join(tmp, "melon-input.txt")
        with open(args.input) as f, open(mscript, "w") as g:
            g.write("# %s, every frame number shifted +%d so the console\n"
                    "# receives each event at the instant the port does.\n"
                    % (os.path.basename(args.input), shift))
            for line in f:
                head, sep, rest = line.partition(" ")
                if sep and head.rstrip(":").isdigit():
                    n = int(head.rstrip(":"))
                    g.write("%d%s %s" % (n + shift,
                                         ":" if head.endswith(":") else "",
                                         rest))
                else:
                    g.write(line)

    port_run(ptrace, args.frames, args.input, args.rtc)
    melon_run(mtrace, args.frames, mscript, args.rtc)

    port = parse(ptrace)
    melon = parse(mtrace)

    if args.offset is not None:
        off, hit, tot, ambiguous = args.offset, 0, 0, False
        for c in port.cps:
            if c["frame"] is None or not c["fb"]:
                continue
            for m in melon.cps:
                if m["frame"] == c["frame"] + off and m["fb"]:
                    tot += 1
                    hit += (c["fb"] == m["fb"])
    else:
        off, hit, tot, ambiguous = align(port, melon)

    print("pcdiff: %d frames, checkpoint every %d, %d port and %d oracle "
          "checkpoints" % (args.frames, args.every, len(port.cps), len(melon.cps)))
    if melon.stopped:
        print("  the oracle %s" % melon.stopped)
    if off is None:
        print("  no alignment: one side wrote no picture at all")
        return 1
    print("  frame offset: port frame N is the console's N+%d "
          "(%d of %d pictures match%s)"
          % (off, hit, tot, ", AMBIGUOUS, several offsets score the same, "
                           "so the picture is not yet distinguishing them"
                           if ambiguous else ""))

    # Per-span agreement over the aligned checkpoints. The point of running
    # this at all is to find out WHICH spans are comparable, so every span is
    # reported rather than only the failing ones.
    mby = {c["frame"]: c for c in melon.cps if c["frame"] is not None}
    pairs = [(c, mby[c["frame"] + off]) for c in port.cps
             if c["frame"] is not None and c["frame"] + off in mby]

    names = {a: n for a, _l, n in port.spans}
    width = max([len(n) for n in names.values()] + [8])
    print()
    print("  %-*s  %9s  %s" % (width, "span", "agree", "note"))
    quiet = []
    blind = []
    for addr, ln, name in port.spans:
        agree = seen = 0
        firstf = None
        for p, m in pairs:
            if addr not in p["d"] or addr not in m["d"]:
                continue
            seen += 1
            if p["d"][addr] == m["d"][addr]:
                agree += 1
            elif firstf is None:
                firstf = p["frame"]
        if seen == 0:
            print("  %-*s  %9s  %s" % (width, name, "-", "not in both traces"))
            continue

        if name in BLIND:
            print("  %-*s  %4d/%-4d  BLIND: %s" % (width, name, agree, seen,
                                                   BLIND[name]))
            blind.append(name)
            continue

        # A span nothing wrote agrees with itself, and that is not evidence.
        # Four of this port's regions never change over a boot on either side,
        # so they score a perfect match while saying nothing at all about
        # whether the port is right. Counting the distinct digests each side
        # produced is what separates "these two agree" from "neither of these
        # moved", and only the first is a result.
        pv = {p["d"][addr] for p, _m in pairs if addr in p["d"]}
        mv = {m["d"][addr] for _p, m in pairs if addr in m["d"]}
        # A span nothing wrote agrees with itself, and only THAT is silence.
        # Two constants that differ are a divergence; a steady one, which is
        # usually easier to explain than an intermittent one, not a weaker
        # result. Calling both cases "constant" would file a real finding
        # under "no evidence", which is the wrong direction to be wrong in.
        still = (len(pv) == 1 and len(mv) == 1)
        if still and firstf is None:
            note = "constant on both sides, no evidence either way"
            quiet.append(name)
        elif still:
            note = "constant on each side but NOT EQUAL, differs from frame %d on" % firstf
        elif firstf is None:
            note = "agrees, and moved %d/%d times" % (len(pv) - 1, len(mv) - 1)
        else:
            note = "first diverges at port frame %d" % firstf
        print("  %-*s  %4d/%-4d  %s" % (width, name, agree, seen, note))

    fbagree = sum(1 for p, m in pairs if p["fb"] == m["fb"])
    spuagree = sum(1 for p, m in pairs if p["spu"] == m["spu"])
    print()
    print("  %-*s  %4d/%-4d  %s" % (width, "picture", fbagree, len(pairs),
                                    "%d distinct on the port, %d on the console"
                                    % (len({p["fb"] for p, _m in pairs}),
                                       len({m["fb"] for _p, m in pairs}))))
    print("  %-*s  %4d/%-4d" % (width, "SPU", spuagree, len(pairs)))
    if blind:
        print()
        print("  %d span(s) are BLIND and their score above is not evidence: %s."
              % (len(blind), ", ".join(blind)))
        print("  Each carries the reason it cannot agree; a line whose reason "
              "stops being")
        print("  true is a line to delete, not one to trust.")
    if quiet:
        print()
        print("  %d span(s) said nothing this run (%s); a longer run, or one"
              % (len(quiet), ", ".join(quiet)))
        print("  driven by an input script, is what makes them speak.")
    if not args.keep:
        print("\n  traces were in %s and have been removed; --keep DIR keeps them"
              % tmp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
