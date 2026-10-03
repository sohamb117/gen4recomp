#!/usr/bin/env python3
"""Read the console's frame-time report and say what a scene cost.

The port writes one line per window of frames, mean and worst frame
interval, how many of those frames drew a polygon and how many the busiest
one drew, and a digest of the picture as the window closed. This turns a
range of frames into the two numbers a scene is judged by, and can put the
PC port's polygon count for the same frames beside it.

  3ds/tests/perf_report.py REPORT                      every window
  3ds/tests/perf_report.py REPORT --scene title 900-1500 --scene field 2000-
  3ds/tests/perf_report.py REPORT --watch              follow a live run
  3ds/tests/perf_report.py REPORT --pc-polys polys.txt --scene ...
  3ds/tests/perf_report.py REPORT --against OTHER      what a change cost

--against is the one to use when a change is being priced. A run TOTAL cannot
answer that: a configuration that costs more reaches fewer frames of the same
replay in the same wall clock, so the two runs' means cover different scenes.
Matching windows on their first frame compares the same frames of the same
script, and it is the difference between measuring 23% and measuring 4.9%.

--pc-polys takes the `frame N polygons M` lines the PC port's --dump-polys
writes, so the console's count and the desktop's are read off the same frames.
That comparison is what said the geometry engine was running at all, and it is
the only reason a polygon number here means anything: milliseconds are this
emulator's and are not a budget for anything.
"""

import argparse
import sys
import time


# Column names are read out of the report's own `#` header rather than
# hardcoded here. The console decides how many phases it measures; a report
# written before the 3D span existed has one column fewer, and a reader that
# counts from the left silently reads powcnt as a lookup count when it changes.
COLUMNS = ("first mean-ms max-ms poly-frames poly-max poly-mean digest "
           "blit-ms flush-ms wait-ms gpu2d-ms game-ms hostmap "
           "powcnt dispcnt-main dispcnt-sub").split()


class Window:
    __slots__ = ("first", "mean_ms", "max_ms", "poly_frames", "poly_max",
                 "poly_mean", "digest", "phase", "hostmap")

    def __init__(self, fields, columns):
        col = dict(zip(columns, fields))
        self.first = int(col["first"])
        self.mean_ms = float(col["mean-ms"])
        self.max_ms = float(col["max-ms"])
        self.poly_frames = int(col["poly-frames"])
        self.poly_max = int(col["poly-max"])
        self.poly_mean = int(col["poly-mean"])
        self.digest = col.get("digest", "-")
        self.phase = {name[:-3]: float(v) for name, v in col.items()
                      if name.endswith("-ms") and name not in
                      ("mean-ms", "max-ms")}
        self.hostmap = int(col.get("hostmap", 0))


def read_report(path):
    """(header dict, [Window]). Missing or half-written files give what is
    there: the port rewrites this file while it runs, so a read can land in
    the middle of one and that is not an error."""
    head, wins, columns = {}, [], COLUMNS
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line.startswith("#"):
                    parts = line.lstrip("#").split()
                    if parts[:1] == ["w"]:
                        columns = parts[1:]
                    continue
                if not line:
                    continue
                parts = line.split()
                if parts[0] == "w":
                    if len(parts) >= 7:
                        wins.append(Window(parts[1:], columns))
                elif len(parts) >= 2:
                    head[parts[0]] = parts[1]
    except FileNotFoundError:
        return {}, []
    return head, wins


def read_pc_polys(path):
    """{frame: polygons} from the PC port's --dump-polys output."""
    out = {}
    with open(path) as f:
        for line in f:
            if line.startswith("frame "):
                p = line.split()
                if len(p) >= 4 and p[2] == "polygons":
                    out[int(p[1])] = int(p[3])
    return out


def parse_range(spec):
    lo, _, hi = spec.partition("-")
    return int(lo or 0), (int(hi) if hi else None)


def summarise(wins, lo, hi, window_frames):
    sel = [w for w in wins
           if w.first >= lo and (hi is None or w.first < hi)]
    if not sel:
        return None
    frames = len(sel) * window_frames
    mean = sum(w.mean_ms for w in sel) / len(sel)
    worst = max(w.max_ms for w in sel)
    poly_frames = sum(w.poly_frames for w in sel)
    drew = [w for w in sel if w.poly_frames]
    poly_mean = (sum(w.poly_mean * w.poly_frames for w in drew) / poly_frames
                 if poly_frames else 0)
    names = [n for n in dict.fromkeys(k for w in sel for k in w.phase)]
    phases = {n: sum(w.phase.get(n, 0.0) for w in sel) / len(sel)
              for n in names}
    return {
        "windows": len(sel), "frames": frames,
        "first": sel[0].first, "last": sel[-1].first + window_frames - 1,
        "mean_ms": mean, "max_ms": worst,
        "fps": 1000.0 / mean if mean else 0.0,
        "poly_frames": poly_frames,
        "poly_max": max(w.poly_max for w in sel),
        "poly_mean": poly_mean,
        "phases": phases,
        "hostmap": sum(w.hostmap for w in sel) / len(sel),
    }


def pc_summary(pc, lo, hi):
    sel = [n for f, n in pc.items() if f >= lo and (hi is None or f < hi)]
    if not sel:
        return None
    drew = [n for n in sel if n]
    return {"frames": len(sel), "poly_frames": len(drew),
            "poly_max": max(sel),
            "poly_mean": sum(drew) / len(drew) if drew else 0}


def print_scene(name, s, pc=None):
    print(f"{name}")
    print(f"  frames      {s['first']}..{s['last']}  ({s['frames']} in "
          f"{s['windows']} window(s))")
    print(f"  ms/frame    {s['mean_ms']:.2f} mean, {s['max_ms']:.2f} worst"
          f"   = {s['fps']:.1f} fps")
    print(f"  polygons    {s['poly_frames']} frame(s) drew, "
          f"{s['poly_mean']:.0f} mean, {s['poly_max']} most")
    # The breakdown, widest first, so what to work on is the top line rather
    # than something to be found by reading. The share is of the mean frame,
    # not of the accounted part, because the whole point is that they add up.
    if s.get("phases"):
        order = sorted(s["phases"].items(), key=lambda kv: -kv[1])
        mean = s["mean_ms"] or 1.0
        print("  frame       " + "  ".join(
            f"{name} {ms:.1f}ms ({100.0 * ms / mean:.0f}%)"
            for name, ms in order if ms >= 0.005))
        if s.get("hostmap"):
            print(f"  translator  {s['hostmap']:.0f} lookup(s) per frame "
                  f"missed the inline cache")
    if pc:
        print(f"  PC port     {pc['poly_frames']} frame(s) drew, "
              f"{pc['poly_mean']:.0f} mean, {pc['poly_max']} most")


def compare(a_head, a_wins, b_head, b_wins, polys_only):
    """Two reports, window by window, matched on FIRST FRAME.

    A run total is not a comparison and this is why this mode exists. A
    configuration that costs more reaches fewer frames of the same replay in
    the same wall clock, so the two runs' means cover different sets of
    scenes, and the heavy ones weigh more in whichever run stopped earlier.
    Read that way, a change that cost 23% of a frame measured as 4.9%.

    Both runs follow the same script from frame 0, so a window's first frame
    names the same scene in both. The digest column cannot do this job: the
    port computes one only where a scene is pinned, and everywhere else it is
    zero for every window in both runs.
    """
    a = {w.first: w for w in a_wins}
    b = {w.first: w for w in b_wins}
    keys = sorted(set(a) & set(b))
    if polys_only:
        keys = [k for k in keys if a[k].poly_frames and b[k].poly_frames]
    if not keys:
        print("no window shares a first frame, were these the same replay?")
        return 1

    def mean(side, get):
        return sum(get(side[k]) for k in keys) / len(keys)

    print(f"{len(keys)} matched window(s), frames {keys[0]}..{keys[-1]}"
          f"{' with polygons' if polys_only else ''}")
    rows = [("frame", lambda w: w.mean_ms), ("worst", lambda w: w.max_ms)]
    names = sorted({n for k in keys for n in a[k].phase} &
                   {n for k in keys for n in b[k].phase})
    rows += [(n, (lambda n: lambda w: w.phase.get(n, 0.0))(n)) for n in names]
    print(f"  {'':10s} {'A':>9s} {'B':>9s} {'delta':>9s}")
    for name, get in rows:
        x, y = mean(a, get), mean(b, get)
        print(f"  {name:10s} {x:9.2f} {y:9.2f} {y - x:+9.2f}"
              f"  {100 * (y - x) / x:+6.1f}%" if x else
              f"  {name:10s} {x:9.2f} {y:9.2f} {y - x:+9.2f}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("report")
    ap.add_argument("--scene", nargs=2, action="append", metavar=("NAME", "A-B"),
                    default=[], help="name a range of frames")
    ap.add_argument("--pc-polys", metavar="FILE",
                    help="the PC port's --dump-polys output, for comparison")
    ap.add_argument("--watch", action="store_true",
                    help="reread every two seconds until interrupted")
    ap.add_argument("--windows", action="store_true",
                    help="print every window rather than a summary")
    ap.add_argument("--against", metavar="REPORT",
                    help="a second report; compare it window by window, "
                         "matched on first frame")
    ap.add_argument("--polys-only", action="store_true",
                    help="with --against, only windows that drew a polygon "
                         "in both runs")
    args = ap.parse_args()

    if args.against:
        a_head, a_wins = read_report(args.report)
        b_head, b_wins = read_report(args.against)
        if not a_wins or not b_wins:
            print("one of the reports has no windows yet")
            return 1
        return compare(a_head, a_wins, b_head, b_wins, args.polys_only)

    pc = read_pc_polys(args.pc_polys) if args.pc_polys else None

    while True:
        head, wins = read_report(args.report)
        wf = int(head.get("window-frames", 60))
        if not wins:
            print(f"{args.report}: no windows yet"
                  f" ({head.get('frames', 0)} frame(s))")
        elif args.windows:
            print("  first  mean  worst  polyframes polymax polymean digest")
            for w in wins:
                print(f"  {w.first:6d} {w.mean_ms:6.2f} {w.max_ms:6.2f}"
                      f"   {w.poly_frames:3d} {w.poly_max:5d} {w.poly_mean:6d}"
                      f"  {w.digest}")
        else:
            scenes = args.scene or [("whole run", "0-")]
            print(f"{args.report}: {head.get('frames', '?')} frame(s), "
                  f"{len(wins)} window(s)")
            for name, spec in scenes:
                lo, hi = parse_range(spec)
                s = summarise(wins, lo, hi, wf)
                if s is None:
                    print(f"{name}\n  no window in {spec} yet")
                    continue
                print_scene(name, s, pc_summary(pc, lo, hi) if pc else None)
        if not args.watch:
            return 0
        time.sleep(2)
        print()


if __name__ == "__main__":
    sys.exit(main())
