import os, sys, tempfile
R = "/Users/soham/Documents/code/nativeplat-e2e-dp/tests/e2e"
sys.path.insert(0, R)
import run, bots
from np_e2e import Session
game = run.Game("diamond"); game.tools()
work = tempfile.mkdtemp(); sav = work + "/x.sav"
open(sav, "wb").write(open(sys.argv[1], "rb").read())
open(work + "/m.recipe", "w").write(sys.argv[2] + "\n"); run.mint(game, work + "/m.recipe", sav, sav, work)
s = Session(game.gp, game.rom, game.name, sav, work + "/run.log", 20000, options=run.DEFAULT_OPTIONS)
run.boot_continue(s); p = s.probe()
t = bots.Terrain(); t.update(p)
goal = (int(sys.argv[3]), int(sys.argv[4]))
dirs = t.path((p.x, p.z), goal)
x, z = p.x, p.z; pts = []
for d in dirs or []:
    dx, dz = bots.DIR_DELTA[d]; x += dx; z += dz; pts.append((x, z))
print("from", (p.x, p.z), "window", p.grid_x0, p.grid_z0, "len", len(dirs or []), pts[::5])
s.kill()
