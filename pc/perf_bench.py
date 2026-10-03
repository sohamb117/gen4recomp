#!/usr/bin/env python3
"""The 3D work's benchmark protocol, one row per run.

Four scenes, each a corpus STATION (a CONTINUE boot from a minted save,
never the new-game flow, whose frames are boot and intro), benched over a
window the input script holds stable. Every row carries rig + commit + the
station's pinned digest, so no number travels without the tree and scene it
measured, and `pc/perf_against.py` refuses to compare rows whose scenes
differ.

    $ python3 pc/perf_bench.py --rig linux-3900x            # the full ladder
    $ python3 pc/perf_bench.py --scene town-walk --quick    # one scene, hd3d 1..2

Ladder per scene: --hd3d {1..4} at default threads, PC_THREADS {1,2,4} at
--hd3d 2, wide (16:9) at --hd3d 2, all unpaced for throughput, then ONE
paced run at --hd3d 2 wide with --trace-pace for the skip/late story, its
pc-* stderr kept whole next to the TSV.

Rows append to build/pc/perf/bench.tsv unless --out says otherwise.
"""

import argparse
import datetime
import os
import platform
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.environ.get("PC_BIN") or os.path.join(ROOT, "build", "pc",
                                                  "pokeplatinum")
CORPUS = os.path.join(ROOT, "pc", "tests", "corpus")
REPLAYS = os.path.join(ROOT, "pc", "replays")
PERFDIR = os.path.join(ROOT, "build", "pc", "perf")

# scene -> (recipe, input, bench_from, bench_frames)
# The window sits where the input script holds the scene stable; each spec's
# own `frames:` pin is the floor the window must clear.
SCENES = {
    "town-walk": ("jubilife", "lab-walk.txt", 3300, 1000),
    "field-surf": ("hm-surf", "lab-fieldmove.txt", 4400, 1000),
    "battle": ("battle-trainer", "lab-battle.txt", 3000, 1500),
    "link-room": ("comm-union", "lab-comm-union.txt", 3300, 1000),
}

COLUMNS = ("date rig commit scene digest hd3d threads wide pace frames fps "
           "ms3d ms2d mspub msaudio mselse log").split()


def spec_digest(recipe):
    with open(os.path.join(CORPUS, recipe + ".spec")) as f:
        for line in f:
            m = re.match(r"digest:\s*([0-9A-F]+)", line)
            if m:
                return m.group(1)
    return "-"


def mint(recipe):
    sav = os.path.join(PERFDIR, recipe + ".sav")
    if os.path.exists(sav):
        return sav
    r = subprocess.run([sys.executable,
                        os.path.join(ROOT, "pc", "tests", "pc_lab.py"),
                        "--out", sav,
                        os.path.join(CORPUS, recipe + ".recipe")],
                       capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        raise SystemExit("minting %s failed:\n%s" % (recipe,
                                                     (r.stderr or r.stdout)[-600:]))
    return sav


def one_run(scene, hd, threads, wide, pace, keep_log=None):
    recipe, script, bfrom, bframes = SCENES[scene]
    sav = mint(recipe)
    run_sav = os.path.join(PERFDIR, "run.sav")
    shutil.copy(sav, run_sav)
    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    # A view channel, or the run never composes the wide/HD picture and never
    # publishes; those spans would bench as zero. Nobody reads the page;
    # the write side is the cost being measured.
    view = "perfbench-%d" % os.getpid()
    env.update(PC_SAVE=run_sav,
               PC_INPUT=os.path.join(REPLAYS, script),
               PC_FRAMES=str(bfrom + bframes),
               PC_PACE="1" if pace else "0",
               PC_VIEW=view,
               PC_BENCH="1", PC_BENCH_FROM=str(bfrom))
    if hd > 1:
        env["PC_HD3D"] = str(hd)
    if threads:
        env["PC_THREADS"] = str(threads)
    if wide:
        env["PC_ASPECT"] = "16:9"
    if pace:
        env["PC_TRACE_PACE"] = "1"
    r = subprocess.run([BINARY], env=env, capture_output=True, text=True,
                       timeout=3600)
    try:
        os.unlink("/dev/shm/" + view)
    except OSError:
        pass
    if r.returncode != 0:
        raise SystemExit("%s hd%d t%s run exited %d:\n%s"
                         % (scene, hd, threads, r.returncode,
                            r.stderr[-600:]))
    lines = [l for l in r.stderr.split("\n") if l.startswith("pc-")]
    logname = "-"
    if keep_log:
        with open(keep_log, "w") as f:
            f.write("\n".join(lines) + "\n")
        logname = os.path.basename(keep_log)

    row = {"fps": "-", "ms3d": "-", "ms2d": "-", "mspub": "-",
           "msaudio": "-", "mselse": "-"}
    for l in lines:
        m = re.match(r"pc-bench: (\d+) frames in [\d.]+ s, ([\d.]+) fps", l)
        if m:
            row["fps"] = m.group(2)
        m = re.match(r"pc-bench: (3D rasterize|2D compose|publish|audio mix|"
                     r"elsewhere)\s+[\d.]+\s+([\d.]+)", l)
        if m:
            key = {"3D rasterize": "ms3d", "2D compose": "ms2d",
                   "publish": "mspub", "audio mix": "msaudio",
                   "elsewhere": "mselse"}[m.group(1)]
            row[key] = "%.2f" % (float(m.group(2)) / 1000.0)
    if pace:
        paces = [l for l in lines if l.startswith("pc-pace:") and "fps" in l]
        if paces:
            m = re.search(r"([\d.]+) fps", paces[-1])
            if m:
                row["fps"] = m.group(1)
    row["log"] = logname
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rig", default=platform.node(),
                    help="a name for this machine in the rows")
    ap.add_argument("--scene", default="all", help="one scene, or all")
    ap.add_argument("--out", default=os.path.join(PERFDIR, "bench.tsv"))
    ap.add_argument("--quick", action="store_true",
                    help="hd3d 1..2 only, skip the threads ladder")
    ap.add_argument("--skip-paced", action="store_true")
    args = ap.parse_args()

    os.makedirs(PERFDIR, exist_ok=True)
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short=10",
                             "HEAD"], capture_output=True,
                            text=True).stdout.strip()
    date = datetime.date.today().isoformat()
    scenes = list(SCENES) if args.scene == "all" else [args.scene]

    fresh = not os.path.exists(args.out)
    out = open(args.out, "a")
    if fresh:
        out.write("\t".join(COLUMNS) + "\n")

    def emit(scene, hd, threads, wide, pace, keep_log=None):
        row = one_run(scene, hd, threads, wide, pace, keep_log)
        cells = [date, args.rig, commit, scene, spec_digest(SCENES[scene][0]),
                 str(hd), str(threads or 0), "1" if wide else "0",
                 "1" if pace else "0", str(SCENES[scene][3]),
                 row["fps"], row["ms3d"], row["ms2d"], row["mspub"],
                 row["msaudio"], row["mselse"], row["log"]]
        out.write("\t".join(cells) + "\n")
        out.flush()
        print("  %-10s hd%d t%-2s %s %s -> %s fps (3d %s ms, 2d %s ms)"
              % (scene, hd, threads or "d", "wide" if wide else "    ",
                 "paced" if pace else "     ", row["fps"], row["ms3d"],
                 row["ms2d"]))

    for scene in scenes:
        print("%s:" % scene)
        hds = (1, 2) if args.quick else (1, 2, 3, 4)
        for hd in hds:
            emit(scene, hd, None, False, False)
        if not args.quick:
            for t in (1, 2, 4):
                emit(scene, 2, t, False, False)
            emit(scene, 2, None, True, False)
        if not args.skip_paced:
            log = os.path.join(PERFDIR, "%s-%s-paced.log" % (scene, commit))
            emit(scene, 2, None, True, True, keep_log=log)
    out.close()
    print("rows appended to %s" % args.out)


if __name__ == "__main__":
    main()
