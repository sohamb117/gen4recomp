#!/usr/bin/env python3
"""Does the console draw the same picture as the desktop port?

Both ports hash their two published surfaces with the same FNV-1a over the
same fixed 256x192 buffers, so a digest from one and a digest from the other
are comparable. Given identical input, identical frames must hash identically;
the first frame where they do not is the first frame the console got wrong,
and that is a far more useful thing to know than "the graphics are glitched".

  # desktop reference, every 60th frame digested
  PC_ROM=build/rom/pokeplatinum.us.nds PC_SAVE=none \\
    PC_INPUT=pc/replays/new-game.txt PC_FRAMES=27400 \\
    PC_DUMP_FRAMES=/tmp/ref PC_DUMP_FROM=0:60 ./build/pc/pokeplatinum

  # console: the same script on the SD card, then
  3ds/tests/frame_parity.py ~/.local/.../perf-report.txt /tmp/ref/frames.txt

The input has to be the same or this measures nothing. A console driven by
hand and a desktop driven by a script diverge the moment a button differs,
which looks exactly like a rendering fault. Put the same file the desktop
replayed on the card as input.txt and the comparison is about the port.

The alignment is found, not assumed. The console reports a window of frames
and hashes the last of them; the desktop's manifest numbers frames its own
way. Rather than hard-code the offset between the two, exactly the kind of
off-by-one that makes a broken run look clean; every small offset is tried
and the one that matches most is reported along with how well it did. An
alignment that matches nothing is itself the answer: the two never agreed.
"""

import argparse
import sys

# Offsets tried when aligning the two runs, in frames. Wider than the window
# so a report written on a different edge still lands.
SEARCH = range(-120, 181, 1)


def read_console(path):
    """[(first_frame, digest)] from the console's report."""
    out = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if p and p[0] == "w" and len(p) >= 8 and p[7] != "-":
                out.append((int(p[1]), p[7].upper()))
    return out


def read_manifest(path):
    """{frame: digest} from the desktop port's frame manifest."""
    out = {}
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.split()
            if len(p) >= 7 and p[6] != "-":
                out[int(p[0])] = p[6].upper()
    return out


def score(console, manifest, offset):
    hits = compared = 0
    first_bad = None
    for first, digest in console:
        frame = first + offset
        if frame not in manifest:
            continue
        compared += 1
        if manifest[frame] == digest:
            hits += 1
        elif first_bad is None:
            first_bad = (first, frame, digest, manifest[frame])
    return hits, compared, first_bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("report", help="the console's perf-report.txt")
    ap.add_argument("manifest", help="the desktop port's frames.txt")
    ap.add_argument("--offset", type=int,
                    help="force an alignment instead of searching")
    args = ap.parse_args()

    console = read_console(args.report)
    manifest = read_manifest(args.manifest)
    if not console:
        print("no digested windows in the console report", file=sys.stderr)
        return 2
    if not manifest:
        print("no digested frames in the manifest", file=sys.stderr)
        return 2

    if args.offset is not None:
        best = args.offset
        hits, compared, first_bad = score(console, manifest, best)
    else:
        best, hits, compared, first_bad = None, -1, 0, None
        for off in SEARCH:
            h, c, fb = score(console, manifest, off)
            if h > hits:
                best, hits, compared, first_bad = off, h, c, fb

    print(f"console windows {len(console)}, desktop digested frames "
          f"{len(manifest)}")
    print(f"alignment +{best} frames, {compared} frame(s) compared")

    if compared == 0:
        print("the two runs do not overlap at any alignment")
        return 2
    if hits == 0:
        print("NOTHING MATCHED. The two runs never drew the same frame, "
              "either the input differed or the picture is wrong from the "
              "first frame.")
        return 1

    # How far the agreement runs from the start, which is the number that
    # says where to look.
    run = 0
    for first, digest in console:
        frame = first + best
        if frame in manifest and manifest[frame] == digest:
            run += 1
        else:
            break

    print(f"identical {hits}/{compared} compared frame(s)")
    if run:
        last = console[run - 1][0] + best
        print(f"identical from the start through desktop frame {last}")
    if first_bad is None:
        print("no divergence: every compared frame is byte-identical")
        return 0

    cfirst, frame, cd, pd = first_bad
    print(f"\nFIRST DIVERGENCE at desktop frame {frame} "
          f"(console window starting {cfirst})")
    print(f"  console {cd}")
    print(f"  desktop {pd}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
