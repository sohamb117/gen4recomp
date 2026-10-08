#!/usr/bin/env python3
"""The static Ruby/Sapphire/Emerald world model (tests/e2e/gba_world.py) from the command line.

    python3 tests/e2e/tools/gba_world_cli.py --game emerald route MAP_LITTLEROOT_TOWN 10 10 MAP_RUSTBORO_CITY 27 20 [--surf]
        [--elevation E] [--avoid-map MAP ...] [--avoid-warp MAP X Z ...]
    python3 tests/e2e/tools/gba_world_cli.py --game emerald map MAP_LITTLEROOT_TOWN

route prints the legs walk_to follows (one per map crossed: the tile to walk to there, how it leaves the map) and the
time the search took; map prints the static map as ASCII (legend in its header), its elevations, connections and warps.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gba_world  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", default="emerald", choices=sorted(gba_world.DECOMPS))
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("route", help="the legs from one map tile to another")
    r.add_argument("src")
    r.add_argument("sx", type=int)
    r.add_argument("sz", type=int)
    r.add_argument("dst")
    r.add_argument("tx", type=int)
    r.add_argument("tz", type=int)
    r.add_argument("--surf", action="store_true", help="the player can Surf")
    r.add_argument("--elevation", type=int, help="the player's elevation (default: the start tile's)")
    r.add_argument("--avoid-map", nargs="*", default=[], metavar="MAP")
    r.add_argument("--avoid-warp", nargs=3, action="append", default=[], metavar=("MAP", "X", "Z"))
    m = sub.add_parser("map", help="a map as ASCII")
    m.add_argument("map")
    args = ap.parse_args()

    t0 = time.perf_counter()
    w = gba_world.World(args.game)
    t1 = time.perf_counter()
    if args.cmd == "map":
        print(w.render(w.map_id(args.map)))
        return 0
    try:
        legs = w.route(args.src, args.sx, args.sz, args.dst, args.tx, args.tz, elevation=args.elevation,
                       surf=args.surf, avoid_maps=args.avoid_map, avoid_warps=[(a, int(x), int(z)) for a, x, z in
                                                                              args.avoid_warp])
    except gba_world.NoRoute as e:
        print("no route: %s" % e)
        return 1
    t2 = time.perf_counter()
    for leg in legs:
        nxt = "" if leg.next_map is None else " -> %s" % w.map_name(leg.next_map)
        how = ""
        if leg.kind == "warp":
            d = w.push_dir(leg.map, leg.x, leg.z)
            how = " (step on)" if d is None else " (push %s)" % ("north", "south", "west", "east")[d]
        print("%-10s %s (%d,%d)%s%s" % (leg.kind, w.map_name(leg.map), leg.x, leg.z, how, nxt))
    print("%d legs; load %.3f s, route %.3f s" % (len(legs), t1 - t0, t2 - t1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
