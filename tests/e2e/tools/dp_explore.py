"""explore.py SAVE RECIPE_LINES CMDS...  cmds: KEY:N (hold N frames then settle), TKEY:N (trace), SHOT:name, WAIT:N, POS"""
import os, sys, tempfile, subprocess
R = "/Users/soham/Documents/code/nativeplat-e2e-dp/tests/e2e"
sys.path.insert(0, R); sys.path.insert(0, R + "/tools")
import run
from np_e2e import Session

game = run.Game("diamond"); game.tools()
work = tempfile.mkdtemp(prefix="exp.")
sav = os.path.join(work, "x.sav")
open(sav, "wb").write(open(sys.argv[1], "rb").read())
if sys.argv[2]:
    rec = os.path.join(work, "m.recipe")
    open(rec, "w").write(sys.argv[2].replace(";", "\n") + "\n")
    run.mint(game, rec, sav, sav, work)
s = Session(game.gp, game.rom, game.name, sav, os.path.join(work, "run.log"), 200000, options=run.DEFAULT_OPTIONS)
run.boot_continue(s)
p = s.probe()
print("start", p.map_id, p.x, p.z, flush=True)
class _Ctx:
    dir = "/tmp"
    def resolve(self, name):
        return run.resolve_name(game, name) if hasattr(run, "resolve_name") else int(name)
CTX = _Ctx()
for k in " ".join(sys.argv[3:]).replace(",", " ").split():
    if k.startswith("SHOT:"):
        out = "/tmp/shot_%s" % k[5:]
        s.dump(out + ".ppm")
        subprocess.run(["sips", "-s", "format", "png", out + ".ppm", "--out", out + ".png"], capture_output=True)
        print("shot", out + ".png")
        continue
    keys, n = k.rsplit(":", 1)
    if keys == "BOT":
        import bots, json
        name, _, arg = n.partition("=")
        step = json.loads(arg.replace(";", ",")) if arg else {}
        try:
            getattr(bots, "bot_" + name)(s, step, CTX)
        except Exception as e:
            print("bot error", e)
        p = s.probe()
        print(k, "->", (p.x, p.z), "map", p.map_id, flush=True)
        continue
    if keys == "WAIT":
        s.run(int(n)); continue
    if keys.startswith("T"):
        keys = keys[1:]; last = None; tr = []
        for f in range(int(n)):
            s.run(1, keys if f < 8 else None)
            p = s.probe()
            if (p.x, p.z) != last:
                last = (p.x, p.z); tr.append("%d:%d,%d" % (f, p.x, p.z))
        print(k, " ".join(tr), flush=True)
        continue
    s.run(int(n), keys)
    s.run(24)
    p = s.probe()
    print(k, "->", (p.x, p.z), "map", p.map_id, "battle", s.in_battle, "field", s.field_ready, flush=True)
s.kill()
