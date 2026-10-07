#!/usr/bin/env python3
"""Solve a Platinum gym puzzle offline and print the walk_to steps' route (milestones 21 and 24).

  veilstone   punching bags: BFS over (bag positions, tire stacks left, player reach), the slide rules of
              src/overlay008/gym_features.c:2951-3253 (objects at table z-2, stop on a 4, stop before a 1 or a stack,
              topple that stack); prints each kick as (stand tile, direction, bag, where it stops, stack toppled).
  pastoria    water levels: BFS over (x, z, player height, water level) with the BDHC heights of the gym's land
              data, the water plate x1..25 z2..38 (gym_features.c:656), getHeight's BDHC-vs-plate choice
              (src/terrain_collision_manager.c:98-121), the <20 height step (:270), DYNAMIC_HEIGHT_COLLISION on
              plate-sourced tiles (:321-326) and the gated grounds at the player's height (gym_features.c:463-486);
              buttons are the coord events (blue HIGH, green MIDDLE, orange LOW). Prints the route's corners.
  canalave    moving platforms: Dijkstra over (x, z, floor, platform states), the four floors' collision maps and the
              platform paths of gym_features.c; prints the route as a `steps` bot route (corners, ride starts and ends).

    python3 tests/e2e/tools/pt_gym.py veilstone
    python3 tests/e2e/tools/pt_gym.py pastoria [--start X,Z,HEIGHT,WATER] [--goal X,Z]
    python3 tests/e2e/tools/pt_gym.py canalave [--goal X,Z,FLOOR]
"""
import argparse
import collections
import heapq
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import pt_map  # noqa: E402
from np_e2e import behaviors  # noqa: E402

PT = pt_map.PT
DIRS = {"up": (0, -1), "down": (0, 1), "left": (-1, 0), "right": (1, 0)}


def header(name):
    h = pt_map.header_fields(name)
    maps = json.load(open(os.path.join(PT, "res", "field", "matrices", "%s.json" % h["mapMatrixID"])))["maps"]
    ev = json.load(open(os.path.join(PT, "res", "field", "events", "%s.json" % h["eventsArchiveID"])))
    return maps, ev


def corners(path):
    """The tiles of a path of (x, z, height, water) where it turns or the water changes: walk_to via waypoints."""
    out = []
    for i in range(1, len(path) - 1):
        a, b, c = path[i - 1], path[i], path[i + 1]
        if (c[0] - b[0], c[1] - b[1]) != (b[0] - a[0], b[1] - a[1]) or b[3] != a[3]:
            out.append(tuple(b[:2]))
    return out


def veilstone():
    maps, ev = header("MAP_HEADER_VEILSTONE_CITY_GYM")
    land = pt_map.land(int(maps[0][0].split("_")[1]))
    src = open(os.path.join(PT, "src", "overlay008", "gym_features.c")).read()

    def table(name):
        body = re.search(r"%s\[[^\]]*\] = \{(.*?)\};" % name, src, re.S).group(1)
        return [int(v) for v in re.findall(r"-?\d+", body)]
    beh = table("sVeilstoneTileBehaviors")
    pairs = lambda v: [(v[i], v[i + 1] - 2) for i in range(0, len(v), 2)]  # objects sit at z-2 (:3148, :3154)
    bags0 = tuple(sorted(pairs(table("sPunchingBagPositions"))))
    stacks0 = frozenset(pairs(table("sTireStackPositions")))
    people = {(o["x"], o["z"]) for o in ev["object_events"]}
    maylene = next((o["x"], o["z"]) for o in ev["object_events"] if o["id"] == "LOCALID_MAYLENE")
    goal = (maylene[0], maylene[1] + 1)

    def wall(x, z):
        return not (0 <= x < 32 and 0 <= z < 32) or land[z * 32 + x] & 0x8000

    def flags(x, z, d, stacks):  # VeilstoneGym_GetTileFlags: 1 can't move, 2 stack ahead, 4 pause
        f, b = 0, beh[x + z * 32]
        f |= 4 if b == 4 else 0
        f |= 1 if (b == 3 and d in ("left", "right")) or (b == 2 and d in ("up", "down")) or (b == 5 and d == "left") else 0
        nx, nz = x + DIRS[d][0], z + DIRS[d][1]
        f |= 1 if beh[nx + nz * 32] == 1 else 0
        f |= 2 if (nx, nz) in stacks else 0
        return f

    def kick(bx, bz, d, stacks):
        if flags(bx, bz, d, stacks) not in (0, 4):
            return None
        x, z = bx, bz
        while True:
            x, z = x + DIRS[d][0], z + DIRS[d][1]
            f = flags(x, z, d, stacks)
            if f:
                break
        hit = (x + DIRS[d][0], z + DIRS[d][1]) if f & 2 else None
        return (x, z), (stacks - {hit} if hit else stacks), hit

    def reach(start, bags, stacks):
        block = set(bags) | stacks | people
        seen, q = {start}, collections.deque([start])
        while q:
            c = q.popleft()
            for dx, dz in DIRS.values():
                n = (c[0] + dx, c[1] + dz)
                if n not in seen and not wall(*n) and n not in block:
                    seen.add(n)
                    q.append(n)
        return seen

    entrance = next((w["x"], w["z"] - 1) for w in ev["warp_events"])
    first = ((bags0, stacks0), entrance)
    prev, q = {first: None}, collections.deque([first])
    while q:
        k = q.popleft()
        (bags, stacks), pos = k
        r = reach(pos, bags, stacks)
        if goal in r:
            kicks = []
            while prev[k]:
                k, kk = prev[k]
                kicks.append(kk)
            for i, (stand, d, bag, stop, hit) in enumerate(reversed(kicks), 1):
                print("kick %d: from %s %s: bag %s -> %s%s" % (i, stand, d, bag, stop,
                                                              ", topples the stack at %s" % (hit,) if hit else ""))
            print("then walk to %s, Maylene at %s" % (goal, maylene))
            return
        for i, (bx, bz) in enumerate(bags):
            for d, (dx, dz) in DIRS.items():
                stand = (bx - dx, bz - dz)
                if stand not in r:
                    continue
                res = kick(bx, bz, d, stacks)
                if not res:
                    continue
                stop, left, hit = res
                key = ((tuple(sorted(bags[:i] + (stop,) + bags[i + 1:])), left), stand)
                if key not in prev:
                    prev[key] = (k, (stand, d, (bx, bz), stop, hit))
                    q.append(key)
    sys.exit("veilstone: no solution")


def bdhc_heights(member):
    """Per tile of one 32x32 land data member, the heights (map units) of every BDHC plate over its centre."""
    raw = open(os.path.join(PT, "res", "field", "maps", "data", "map_data_%03d.bin" % member), "rb").read()
    attrs_size, props_size, model_size, _ = struct.unpack_from("<4I", raw, 0)
    off = 16 + attrs_size + props_size + model_size
    assert raw[off:off + 4] == b"BDHC"
    npts, nnorm, ncon, nplate, _, _ = struct.unpack_from("<6H", raw, off + 4)
    o = off + 16
    pts = [struct.unpack_from("<2i", raw, o + 8 * i) for i in range(npts)]
    o += 8 * npts
    norms = [struct.unpack_from("<3i", raw, o + 12 * i) for i in range(nnorm)]
    o += 12 * nnorm
    cons = [struct.unpack_from("<i", raw, o + 4 * i)[0] for i in range(ncon)]
    o += 4 * ncon
    plates = [struct.unpack_from("<4H", raw, o + 8 * i) for i in range(nplate)]
    out = {}
    for z in range(32):
        for x in range(32):
            lx, lz = ((x * 16 + 8) - 256) * 4096, ((z * 16 + 8) - 256) * 4096  # tile centre, map-centred fx32
            hs = []
            for a, b, n, c in plates:
                (x1, z1), (x2, z2) = pts[a], pts[b]
                nx, ny, nz = norms[n]
                if ny and min(x1, x2) <= lx <= max(x1, x2) and min(z1, z2) <= lz <= max(z1, z2):
                    hs.append(round(-(nx * lx / 4096 + nz * lz / 4096 + cons[c]) / ny))  # CalculateObjectHeight
            out[(x, z)] = hs
    return out


def pastoria(start, goal):
    b = behaviors()
    maps, ev = header("MAP_HEADER_PASTORIA_CITY_GYM")
    members = [int(row[0].split("_")[1]) for row in maps]  # one column of members, stacked in z
    land = [pt_map.land(m) for m in members]
    heights = [bdhc_heights(m) for m in members]
    objs = {(o["x"], o["z"]) for o in ev["object_events"]}
    levels = {2: 64, 3: 32, 4: 0}  # scripts_pastoria_city_gym.s entries: blue HIGH, green MIDDLE, orange LOW
    buttons = {(c["x"], c["z"]): levels[c["script"]] for c in ev["coord_events"]}
    gated = {b["PASTORIA_GYM_H_GROUND"]: 0, b["PASTORIA_GYM_M_GROUND"]: 32, b["PASTORIA_GYM_L_GROUND"]: 64}
    dyn = b["DYNAMIC_HEIGHT_COLLISION"]

    def attr(x, z):
        return land[z // 32][(z % 32) * 32 + x]

    def height(x, z, h, water):  # GetHeight (terrain_collision_manager.c:98-121)
        hs = heights[z // 32][(x, z % 32)]
        bv = min(hs, key=lambda v: abs(v - h)) if hs else None
        if 1 <= x <= 25 and 2 <= z <= 38:
            if bv is None:
                return water, "plate"
            if water <= bv or abs(bv - h) <= abs(water - h):
                return bv, "bdhc"
            return water, "plate"
        return (bv, "bdhc") if bv is not None else (None, None)

    def step(x, z, h, water, nx, nz):
        if not (0 <= nx < 32 and 0 <= nz < 32 * len(land)) or (nx, nz) in objs:
            return None
        nh, src = height(nx, nz, h, water)
        if nh is None or abs(nh - h) >= 20:
            return None
        a = attr(nx, nz)
        if a & 0xFF in gated:
            return nh if h == gated[a & 0xFF] else None
        if a & 0x8000 or (src == "plate" and a & 0xFF == dyn):
            return None
        return nh

    first = tuple(start)
    prev, q, end = {first: None}, collections.deque([first]), None
    while q:
        c = q.popleft()
        x, z, h, water = c
        if (x, z) == goal:
            end = c
            break
        for dx, dz in DIRS.values():
            nh = step(x, z, h, water, x + dx, z + dz)
            if nh is None:
                continue
            nw = buttons.get((x + dx, z + dz), water)
            if nw != water:
                nh = height(x + dx, z + dz, nh, nw)[0]
            n = (x + dx, z + dz, nh, nw)
            if n not in prev:
                prev[n] = c
                q.append(n)
    if end is None:
        sys.exit("pastoria: no route")
    path = []
    while end:
        path.append(end)
        end = prev[end]
    path.reverse()
    names = {64: "blue (HIGH)", 32: "green (MIDDLE)", 0: "orange (LOW)"}
    for a, c in zip(path, path[1:]):
        if c[3] != a[3]:
            print("# button %s at (%d,%d)" % (names[c[3]], c[0], c[1]))
    print("# %d steps; ends at height %d, water %d" % (len(path) - 1, path[-1][2], path[-1][3]))
    print("via = [%s]" % ", ".join("[%d, %d]" % w for w in corners(path)))
    print("x = %d\nz = %d" % goal)


def canalave(goal, trace=False, back=None):
    """Canalave Gym: Dijkstra over (x, z, floor, platform states). A step is allowed where the floor's collision map
    is 0 (sCanalaveGymCollisionMaps, the gym's DynamicMapFeaturesCheckCollision, gym_features.c:1637-1650) and no
    object of that floor stands; a step that ends on a platform's current tile of that floor rides it to its other
    end (Field_ProcessStep -> CanalaveGym_CheckIfPlayerOnPlatform, gym_features.c:1232-1303, 1608-1628). The states
    start as sCanalaveGymPlatformsStartInPositionB (persisted_map_features_init.c:22-47; reset on entering,
    CanalaveGym_Init). Tiles in a gym trainer's sight on its floor cost 30, so the route avoids being walked up to."""
    src = open(os.path.join(PT, "src", "overlay008", "gym_features.c")).read()
    i = src.index("sCanalaveGymCollisionMaps[4]")
    cells = [int(v) for v in re.findall(r"\b\d+\b", src[src.index("{", i):src.index("};", i)])]
    coll = [cells[f * 1024:(f + 1) * 1024] for f in range(4)]
    i = src.index("sCanalavePlatformPaths[CANALAVE_GYM_NUM_PLATFORMS]")
    body = src[i:src.index("};", i)]
    pos = re.findall(r"\.position([AB]) = \{ \.x = (\d+), \.y = (\d+), \.z = (\d+) \}", body)
    plats = []
    for k in range(0, len(pos), 2):
        a, b = pos[k], pos[k + 1]
        plats.append(((int(a[1]), int(a[2]) // 10, int(a[3])), (int(b[1]), int(b[2]) // 10, int(b[3]))))
    init = open(os.path.join(PT, "src", "persisted_map_features_init.c")).read()
    i = init.index("sCanalaveGymPlatformsStartInPositionB[")
    starts = re.findall(r"\b(TRUE|FALSE)\b", init[i:init.index("};", i)])
    mask0 = sum(1 << k for k, v in enumerate(starts) if v == "TRUE")
    ev = header("MAP_HEADER_CANALAVE_CITY_GYM")[1]
    blocked, sight = set(), set()
    looks = {"SOUTH": [(0, 1)], "NORTH": [(0, -1)], "WEST": [(-1, 0)], "EAST": [(1, 0)]}
    for o in ev["object_events"]:
        f = o.get("y", 0) // 10
        blocked.add((o["x"], o["z"], f))
        rng = o.get("data", [0])[0] if o.get("data") else 0
        if not str(o.get("script", "")).startswith("TRAINER_") or not rng:
            continue
        for word, dd in looks.items():
            if word in o.get("movement_type", ""):
                for dx, dz in dd:
                    for n in range(1, rng + 1):
                        x, z = o["x"] + dx * n, o["z"] + dz * n
                        if not (0 <= x < 32 and 0 <= z < 32) or coll[f][z * 32 + x]:
                            break
                        sight.add((x, z, f))
    start = (16, 27, 0, mask0)
    if back is not None:
        start, goal = back, (16, 26, 0)
    keys = {(0, -1): "U", (0, 1): "D", (-1, 0): "L", (1, 0): "R"}
    dist, came = {start: 0}, {start: None}
    heap = [(0, start)]
    end = None
    while heap:
        g, st = heapq.heappop(heap)
        if g > dist[st]:
            continue
        x, z, f, mask = st
        if (x, z, f) == goal:
            end = st
            break
        for (dx, dz), key in keys.items():
            nx, nz = x + dx, z + dz
            if not (0 <= nx < 32 and 0 <= nz < 32) or coll[f][nz * 32 + nx] or (nx, nz, f) in blocked:
                continue
            nf, nm, cost, ride = f, mask, 1 + (30 if (nx, nz, f) in sight else 0), None
            for k, (a, b) in enumerate(plats):
                here = b if mask >> k & 1 else a
                if here == (nx, f, nz):
                    there = a if mask >> k & 1 else b
                    nx, nz, nf, nm, cost, ride = there[0], there[2], there[1], mask ^ (1 << k), cost + 2, k
                    break
            ns = (nx, nz, nf, nm)
            if g + cost < dist.get(ns, 1 << 30):
                dist[ns] = g + cost
                came[ns] = (st, key, ride)
                heapq.heappush(heap, (g + cost, ns))
    if end is None:
        sys.exit("canalave: no route")
    moves, st = [], end
    while came[st] is not None:
        prev, key, ride = came[st]
        moves.append((key, ride, st))
        st = prev
    moves.reverse()
    if trace:
        for n, (key, ride, st) in enumerate(moves, 1):
            print("# %3d %s -> (%d,%d) floor %d%s" % (n, key, st[0], st[1], st[2], " ride %d" % ride if ride is not None else ""))
    # corners of the route: where it turns, the tile a ride starts from (stepped onto) and where it ends
    route, out, prev = [], [], start[:2]
    for n, (key, ride, st) in enumerate(moves):
        nxt = moves[n + 1] if n + 1 < len(moves) else None
        if ride is not None:
            d = {"U": (0, -1), "D": (0, 1), "L": (-1, 0), "R": (1, 0)}[key]
            route.append([prev[0] + d[0], prev[1] + d[1]])
            route.append([st[0], st[1]])
            out.append("ride platform %d to (%d,%d) floor %d" % (ride, st[0], st[1], st[2]))
        elif nxt is None or nxt[0] != key or nxt[1] is not None:
            route.append([st[0], st[1]])
        prev = st[:2]
    print("# %d steps, cost %d; %s" % (len(moves), dist[end], "; ".join(out)))
    print("route = %s" % json.dumps(route).replace("],[", "], ["))
    return end


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("gym", choices=("veilstone", "pastoria", "canalave"))
    ap.add_argument("--start", default="13,41,64,32", help="pastoria: x,z,height,water (entrance, water MIDDLE)")
    ap.add_argument("--trace", action="store_true", help="canalave: print every tile of the route")
    ap.add_argument("--back", action="store_true", help="canalave: also the route back to the entrance after it")
    ap.add_argument("--goal", default=None, help="pastoria: x,z (default 13,5, in front of Wake); canalave: x,z,floor "
                                                 "(default 16,4,3, in front of Byron)")
    a = ap.parse_args()
    if a.gym == "veilstone":
        veilstone()
    elif a.gym == "canalave":
        end = canalave(tuple(int(v) for v in (a.goal or "16,4,3").split(",")), a.trace)
        if a.back:
            print("# back to the entrance (16,26) from there:")
            canalave(None, a.trace, back=end)
    else:
        pastoria([int(v) for v in a.start.split(",")], tuple(int(v) for v in (a.goal or "13,5").split(",")))


if __name__ == "__main__":
    main()
