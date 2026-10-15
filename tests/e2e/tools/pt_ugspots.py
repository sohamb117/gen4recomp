#!/usr/bin/env python3
"""Print the Underground's mining spots (the wall sparkles) for a Platinum save, nearest first.

    python3 tests/e2e/tools/pt_ugspots.py SAVE [--near X,Z] [--frames N] [--schedule PRESS]

Boots SAVE with np_gp, plays PRESS (default: tests/gameplay/schedules/underground.press, CONTINUE, the
Explorer Kit on Y and A through Roark's intro) and reads the game's MiningEnv (sMiningEnv, its address from
the wasm link map) at the end. The spots are spawned from the save's Underground random seed
(mining.c MiningEnv_Init: Underground_GetRandomSeed), not from the frame, so a recipe's spots are the same in
every run that enters with the same save. Each spot is a MiningSpot {u16 x, u16 z, u8 0xFF, u8 index}; the
array is found as the longest run of 6-byte records whose fifth byte is 0xFF. A spot is a wall tile: stand on
the open tile next to it, face it, press A.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
MAP = os.path.join(ROOT, "games/platinum/build/pc-wasm/pokeplatinum.map")
GP = os.path.join(ROOT, "build/gameplay/np_gp-core-plat")
ROM = os.path.join(ROOT, "games/platinum/build/rom/pokeplatinum.us.nds")
CHUNK = 4096


def symbol(name):
    with open(MAP) as f:
        for line in f:
            p = line.split()
            if len(p) == 4 and p[3] == name:
                return int(p[0], 16)
    sys.exit("no %s in %s" % (name, MAP))


def peek(save, frames, schedule, addr, length):
    with tempfile.TemporaryDirectory() as tmp:
        sav = os.path.join(tmp, "s.sav")
        shutil.copyfile(save, sav)
        args = [GP, ROM, "--frames", str(frames + 2), "--save", sav, "--schedule", schedule,
                "-e", "PC_RTC=2009-03-22 21:00:00"]
        n = (length + CHUNK - 1) // CHUNK
        for i in range(n):
            args += ["--peek", "%d:%#x:%d" % (frames, addr + i * CHUNK, min(CHUNK, length - i * CHUNK))]
        out = subprocess.run(args, capture_output=True, text=True).stdout
    data = b""
    for line in out.splitlines():
        m = re.match(r"peek \d+ (0x[0-9a-f]+): (.*)", line)
        if m:
            data += b"".join(int(w, 16).to_bytes(4, "little") for w in m.group(2).split())
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("save")
    ap.add_argument("--near", default="168,375", help="player tile to sort by (Oreburgh's hole lands at 168,375)")
    ap.add_argument("--frames", type=int, default=6790)
    ap.add_argument("--schedule", default=os.path.join(ROOT, "tests/gameplay/schedules/underground.press"))
    ap.add_argument("--count", type=int, default=12)
    a = ap.parse_args()
    env_ptr = peek(a.save, a.frames, a.schedule, symbol("sMiningEnv"), 4)
    if len(env_ptr) < 4 or int.from_bytes(env_ptr, "little") == 0:
        sys.exit("no MiningEnv at frame %d (not in the Underground yet?)" % a.frames)
    env = peek(a.save, a.frames, a.schedule, int.from_bytes(env_ptr, "little"), 3 * CHUNK)
    best = (0, 0, [])
    for start in range(0, len(env) - 6, 2):
        spots, n = [], 0
        while start + 6 * (n + 1) <= len(env) and env[start + 6 * n + 4] == 0xFF:
            r = env[start + 6 * n:start + 6 * n + 6]
            x, z = int.from_bytes(r[0:2], "little"), int.from_bytes(r[2:4], "little")
            if x < 0x400 and z < 0x400:
                spots.append((x, z))
            n += 1
        if len(spots) > len(best[2]):
            best = (start, n, spots)
    start, n, spots = best
    nx, nz = (int(v) for v in a.near.split(","))
    spots.sort(key=lambda p: abs(p[0] - nx) + abs(p[1] - nz))
    print("%d mining spots (array at MiningEnv+%#x, %d slots)" % (len(spots), start, n))
    for x, z in spots[:a.count]:
        print("  (%d,%d)  distance %d" % (x, z, abs(x - nx) + abs(z - nz)))


if __name__ == "__main__":
    main()
