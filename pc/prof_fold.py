#!/usr/bin/env python3
"""Fold PC_PROF samples into the rasterizer's cost centres.

The sampler writes one u32 EIP per tick; the binary links -no-pie, so each
is an absolute address addr2line can resolve directly. -O3 inlines the whole
span into render_polygon_scanline, which is why this reads the INLINE STACK
(-i) and classifies by the innermost frame it recognizes, a tick inside
texture_lookup's body is a texture tick even though the caller's symbol owns
the address.

    $ python3 pc/prof_fold.py build/pc/prof.bin
    $ python3 pc/prof_fold.py --bin build/pc/pokeplatinum --top 30 prof.bin
"""

import argparse
import collections
import os
import struct
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# centre -> function-name prefixes, innermost frame wins, first match takes it.
CENTRES = [
    ("interp-divide", ("interp_set_x",)),
    ("interp-lerp", ("interp_interpolate",)),
    ("texture", ("texture_lookup", "tex_at", "pal_at", "tex_read",
                 "texpal_read", "tex_direct", "pal_direct")),
    ("shading", ("render_pixel", "alpha_blend", "plot_translucent")),
    ("depth-test", ("depth_test",)),
    ("span-walk", ("render_polygon_scanline", "render_scanline", "slope_",
                   "span_setup", "render_shadow_mask")),
    ("final-pass", ("scanline_final_pass", "calculate_fog_density")),
    ("clear", ("clear_rows",)),
    ("band-seed", ("band_seed", "setup_polygon", "poly_shade_setup",
                   "bands_")),
    ("raster-other", ("pc_gpu3d_soft",)),
    # pc/src carries no line info, so these fall back on the symbol name.
    ("workers", ("worker_body", "pc_workers")),
    ("view-compose", ("view_compose", "hd_band", "pc_gpu2d_hd3d_row")),
    ("publish", ("pc_view_publish",)),
    ("2d-compose", ("draw_bg", "draw_sprites", "draw_scanline", "engine_",
                    "compose_", "master_bright", "do_capture", "win_",
                    "pc_gpu2d")),
    ("geometry", ("pc_gpu3d_",)),
    ("audio", ("pc_audio", "pc_snd", "arm7_", "SND")),
]

# ...then by file when no function matched.
FILES = [
    ("geometry", "pc_gpu3d.c"),
    ("raster-other", "pc_gpu3d_soft.c"),
    ("2d-compose", "pc_gpu2d.c"),
    ("view-compose", "pc_view.c"),
    ("audio", "pc_audio"),
    ("audio", "pc_snd"),
    ("audio", "arm7snd"),
    ("workers", "pc_workers.c"),
]

RASTER = {"interp-divide", "interp-lerp", "texture", "shading", "depth-test",
          "span-walk", "final-pass", "clear", "band-seed", "raster-other"}


def classify(frames):
    for fn, file in frames:                     # innermost first
        for centre, prefixes in CENTRES:
            if any(fn.startswith(p) for p in prefixes):
                return centre
    for fn, file in frames:
        for centre, part in FILES:
            if part in file:
                return centre
    return "game-code"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profile")
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "pc",
                                                  "pokeplatinum"))
    ap.add_argument("--top", type=int, default=15,
                    help="also list the top N innermost functions")
    ap.add_argument("--spike-us", type=int, default=0,
                    help="fold only samples from frames whose traced work"
                         " is at least this many microseconds (needs the"
                         " {0, work_us} frame markers pc_prof_frame writes"
                         " under PC_TRACE_PACE)")
    args = ap.parse_args()

    raw = open(args.profile, "rb").read()
    words = struct.unpack("<%dI" % (len(raw) // 4), raw[:len(raw) // 4 * 4])
    # The stream is samples with {0, work_us} frame markers interleaved,
    # EIP 0 is never a sample, so the walk is deterministic. A sample
    # belongs to the frame whose marker FOLLOWS it (the marker is written
    # at the period's end); the tail past the last marker belongs to no
    # complete frame and is dropped when filtering.
    eips = []
    pending = []
    nframes = nspikes = 0
    i = 0
    while i < len(words):
        w = words[i]
        if w == 0 and i + 1 < len(words):
            nframes += 1
            if not args.spike_us or words[i + 1] >= args.spike_us:
                if args.spike_us:
                    nspikes += 1
                eips.extend(pending)
            pending = []
            i += 2
            continue
        pending.append(w)
        i += 1
    if not args.spike_us:
        eips.extend(pending)
    if args.spike_us:
        print("%d frames marked, %d at or over %d us; %d of %d samples kept"
              % (nframes, nspikes, args.spike_us, len(eips),
                 len(words) - 2 * nframes))
    if not eips:
        raise SystemExit("no samples in %s%s" % (args.profile,
                         " after the spike filter" if args.spike_us else ""))
    counts = collections.Counter(eips)

    addrs = ["%#x" % a for a in counts]
    out = subprocess.run(["addr2line", "-e", args.bin, "-f", "-i", "-a"] +
                         addrs, capture_output=True, text=True).stdout
    frames_of = {}
    cur = None
    lines = out.split("\n")
    i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith("0x"):
            cur = int(l, 16)
            frames_of[cur] = []
            i += 1
            continue
        if cur is not None and i + 1 < len(lines):
            frames_of[cur].append((l, lines[i + 1]))
            i += 2
            continue
        i += 1

    centres = collections.Counter()
    funcs = collections.Counter()
    for eip, n in counts.items():
        frames = frames_of.get(eip) or [("??", "??")]
        centres[classify(frames)] += n
        funcs[frames[0][0]] += n

    total = sum(counts.values())
    raster = sum(centres[c] for c in RASTER)
    print("%d samples, %d unique addresses" % (total, len(counts)))
    print("%-14s %8s %7s %12s" % ("centre", "samples", "of run",
                                  "of rasterize"))
    for centre, n in centres.most_common():
        print("%-14s %8d %6.1f%% %11s" %
              (centre, n, 100.0 * n / total,
               "%6.1f%%" % (100.0 * n / raster)
               if centre in RASTER and raster else ""))
    if args.top:
        print("\ntop innermost functions:")
        for fn, n in funcs.most_common(args.top):
            print("%8d %6.1f%%  %s" % (n, 100.0 * n / total, fn))


if __name__ == "__main__":
    main()
