"""The e2e harness's link to a running game: np_gp in serve mode.

    s = Session(gp, rom, "platinum", save="x.sav", log="run.log", budget=30000)
    s.run(60, "a")                       # hold A for 60 frames
    s.run(600, until=["field_ready=1"])  # run until the field is free
    p = s.probe()                        # player tile, battle UI, terrain
    s.dump("shot.ppm")
    s.quit()

np_gp (tests/gameplay/np_gp.c, --serve 1) runs the core and answers one
command per line; the guest publishes the probe block (core/include/np_e2e.h)
because the session starts it with PC_E2E=1. Every frame this session runs
counts against its budget; going over raises Budget.
"""
import os
import re
import struct
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

STATUS = ["link_active", "field_ready", "quicksave_seq", "quicksave_result", "map_id", "in_battle", "e2e", "resets"]

# core/include/np_e2e.h
E2E_MAGIC = 0x31453245
E2E_GRID = 64
E2E_MAX_OBJECTS = 64
E2E_MAX_BATTLERS = 4
E2E_MAX_PARTY = 6
UI_NONE, UI_BATTLE_MENU, UI_BATTLE_PARTY = 0, 1, 2
TILE_BEHAVIOR, TILE_COLLISION, TILE_KNOWN = 0x00FF, 0x0100, 0x8000
E2E_LAYERS = 2
STEP_LAYER, STEP_DIRS, STEP_TARGET, STEP_JUMP = 0x8000, 0x000F, 0x00F0, 0x0F00
_HEAD = struct.Struct("<5I3i5I2i2I")
_OBJ = struct.Struct("<hhHH")
_BATTLE = struct.Struct("<4I")
_MON = struct.Struct("<3H4HH4BB2BB")


class Mon:
    """One np_e2e_mon: a battler or a party member in the battle report."""

    def __init__(self, raw, off):
        v = _MON.unpack_from(raw, off)
        self.species, self.hp, self.max_hp = v[0:3]
        self.moves = list(v[3:7])
        self.disabled_move = v[7]
        self.pp = list(v[8:12])
        self.level = v[12]
        self.types = [t for t in v[13:15] if t != 0xFF]
        self.egg = v[15]

    @property
    def alive(self):
        return self.species != 0 and not self.egg and self.hp > 0

    def __repr__(self):
        return "Mon(%d L%d %d/%d moves=%s pp=%s)" % (self.species, self.level, self.hp, self.max_hp, self.moves,
                                                    self.pp)


FACINGS = {"up": 0, "down": 1, "left": 2, "right": 3}
DIR_KEYS = ["up", "down", "left", "right"]
DIR_DELTA = [(0, -1), (0, 1), (-1, 0), (1, 0)]


class HarnessError(Exception):
    """A milestone failure the report names (not a crash of the harness)."""


class Budget(HarnessError):
    pass


class Dead(HarnessError):
    pass


class Probe:
    """One np_e2e_block, unpacked."""

    def __init__(self, raw):
        (self.magic, self.version, self.frame, self.field, self.map_id, self.x, self.z, self.y, self.facing,
         self.move_state, self.ui, self.ui_arg, self.ui_count, self.grid_x0, self.grid_z0, self.grid_seq,
         nobjects) = _HEAD.unpack_from(raw, 0)
        off = _HEAD.size
        self.objects = []
        for i in range(min(nobjects, E2E_MAX_OBJECTS)):
            x, z, local_id, gfx = _OBJ.unpack_from(raw, off + i * _OBJ.size)
            self.objects.append((x, z, local_id, gfx))
        off += E2E_MAX_OBJECTS * _OBJ.size
        self.grid = struct.unpack_from("<%dH" % (E2E_GRID * E2E_GRID), raw, off)
        off += E2E_GRID * E2E_GRID * 2
        # the battle report (np_e2e_block.battle_frame ...): refreshed while the battle menu waits
        self.battle_frame = self.battle_type = self.menu_battler = 0
        self.battlers, self.party = [], []
        if len(raw) >= off + _BATTLE.size + (E2E_MAX_BATTLERS + E2E_MAX_PARTY) * _MON.size:
            self.battle_frame, self.battle_type, self.menu_battler, nparty = _BATTLE.unpack_from(raw, off)
            off += _BATTLE.size
            self.battlers = [Mon(raw, off + i * _MON.size) for i in range(E2E_MAX_BATTLERS)]
            off += E2E_MAX_BATTLERS * _MON.size
            self.party = [Mon(raw, off + i * _MON.size) for i in range(min(nparty, E2E_MAX_PARTY))]
            off += E2E_MAX_PARTY * _MON.size
        # v3: the game's own step check by layer (np_e2e.h NP_E2E_STEP_*); steps_seq 0 (Platinum) or a v2
        # guest: none
        self.steps_seq, self.player_height, self.steps, self.heights = 0, 0, None, None
        n = E2E_LAYERS * E2E_GRID * E2E_GRID
        if self.version >= 3 and len(raw) >= off + 8 + n * 4:
            self.steps_seq, self.player_height = struct.unpack_from("<Ii", raw, off)
            off += 8
            if self.steps_seq:
                self.steps = struct.unpack_from("<%dH" % n, raw, off)
                self.heights = struct.unpack_from("<%dh" % n, raw, off + n * 2)

    def layers(self):
        """{(x, z): {height: {d: (tx, tz, target height or None if unknown)}}} from the step layers, or None.

        Every layer the flood from the player reached, with the steps the game's movement check allows from it
        (a ledge's lands two tiles away). A step off the window, or onto a tile of a map block the game has not
        loaded (the check refuses those: no tile behavior), is unknown, not refused: its target height is None."""
        if not self.steps:
            return None
        g = E2E_GRID * E2E_GRID
        out = {}
        for l in range(E2E_LAYERS):
            base = l * g
            for cell in range(g):
                st = self.steps[base + cell]
                if not st & STEP_LAYER:
                    continue
                gx, gz = cell % E2E_GRID, cell // E2E_GRID
                x, z = self.grid_x0 + gx, self.grid_z0 + gz
                moves = {}
                for d in range(4):
                    if not st & (1 << d):
                        ngx, ngz = gx + DIR_DELTA[d][0], gz + DIR_DELTA[d][1]
                        nx, nz = x + DIR_DELTA[d][0], z + DIR_DELTA[d][1]
                        inside = 0 <= ngx < E2E_GRID and 0 <= ngz < E2E_GRID
                        if nx >= 0 and nz >= 0 and (not inside or not self.grid[ngz * E2E_GRID + ngx] & TILE_KNOWN):
                            moves[d] = (nx, nz, None)
                        continue
                    r = 2 if st & (0x100 << d) else 1
                    tgx, tgz = gx + DIR_DELTA[d][0] * r, gz + DIR_DELTA[d][1] * r
                    th = None
                    if 0 <= tgx < E2E_GRID and 0 <= tgz < E2E_GRID:
                        th = self.heights[(1 if st & (0x10 << d) else 0) * g + tgz * E2E_GRID + tgx]
                    moves[d] = (x + DIR_DELTA[d][0] * r, z + DIR_DELTA[d][1] * r, th)
                out.setdefault((x, z), {})[self.heights[base + cell]] = moves
        return out

    @property
    def battle_fresh(self):
        """The battle report is this menu's: refreshed within the last few frames."""
        return self.battle_frame != 0 and self.frame - self.battle_frame <= 8 and bool(self.battlers)

    def cell(self, x, z):
        """The grid cell at map tile (x, z), or None outside the window."""
        gx, gz = x - self.grid_x0, z - self.grid_z0
        if 0 <= gx < E2E_GRID and 0 <= gz < E2E_GRID:
            return self.grid[gz * E2E_GRID + gx]
        return None

    def __repr__(self):
        return "Probe(map=%d pos=(%d,%d) face=%d ui=%d/%d x%d field=%d objs=%d)" % (
            self.map_id, self.x, self.z, self.facing, self.ui, self.ui_arg, self.ui_count, self.field,
            len(self.objects))


def key_arg(keys):
    """'A', 'up+B', ['a', 'b'] -> np_gp's 'a+b'; '' / None -> 'none'."""
    if not keys:
        return "none"
    if isinstance(keys, (list, tuple)):
        keys = "+".join(keys)
    keys = keys.lower().replace(" ", "")
    return keys or "none"


class Session:
    def __init__(self, gp, rom, game, save, log, budget, options=(), env=None):
        self.game = game
        self.save_path = save
        self.budget = budget
        self.frame = 0
        self.status = [0] * 16
        self.defects = []
        self.shot_frames = []
        self.shot_dir = None
        self.summary = None
        self.ended = None  # why the game ended on purpose (bots.bot_wait_reset): no end save or probe after it
        self.log_path = log
        self._log = open(log, "w")
        cmd = [gp, rom, "--game", game, "--serve", "1", "-e", "PC_E2E=1", "--save", save]
        for k, v in (env or {}).items():
            cmd += ["-e", "%s=%s" % (k, v)]
        for o in options:
            cmd += ["-o", o]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._log,
                                     text=True, bufsize=1)

    # ---- protocol
    def _cmd(self, line):
        if self.proc.poll() is not None:
            raise Dead("np_gp exited (status %s) at frame %d" % (self.proc.returncode, self.frame))
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        while True:
            out = self.proc.stdout.readline()
            if not out:
                self.proc.wait()
                raise Dead("np_gp exited (status %s) at frame %d%s" % (
                    self.proc.returncode, self.frame, ": " + self.defects[-1] if self.defects else ""))
            out = out.rstrip("\n")
            if out.startswith("DEFECT"):
                self.defects.append(out)
                self._log.write("[e2e] %s\n" % out)
                continue
            if out.startswith("error"):
                raise HarnessError("np_gp: %s (for: %s)" % (out, line[:80]))
            return out

    def note(self, text):
        """A line in the run log, stamped with the frame."""
        self._log.flush()
        self._log.write("[e2e] frame %d: %s\n" % (self.frame, text))
        self._log.flush()

    @property
    def remaining(self):
        return self.budget - self.frame

    def run(self, n, keys=None, touch=None, until=()):
        """Hold keys (and a touch) for up to n frames; True if a condition stopped it.

        Frames listed in shot_frames are dumped on the way (into shot_dir)."""
        n = int(n)
        if n <= 0:
            return False
        if self.remaining <= 0:
            raise Budget("frame budget %d spent" % self.budget)
        n = min(n, self.remaining)
        if isinstance(until, str):
            until = [until]
        while n > 0:
            chunk = n
            pending = [f for f in self.shot_frames if f >= self.frame]
            if pending and pending[0] - self.frame + 1 < chunk:
                chunk = pending[0] - self.frame + 1
            hit = self._run(chunk, keys, touch, until)
            n -= chunk
            if self.shot_frames and self.frame - 1 in self.shot_frames:
                self.dump(os.path.join(self.shot_dir, "frame_%06d.ppm" % (self.frame - 1)))
            if hit:
                return True
        return False

    def _run(self, n, keys, touch, until):
        line = "run %d %s" % (n, key_arg(keys))
        if touch:
            line += " %d %d" % (touch[0], touch[1])
        for c in until:
            line += " until %s" % c
        out = self._cmd(line)
        if out.startswith("dead"):
            self.frame = int(out.split()[1])
            raise Dead("the core stopped at frame %d%s" % (self.frame, ": " + self.defects[-1] if self.defects else ""))
        parts = out.split()
        if parts[0] != "ok":
            raise HarnessError("np_gp: unexpected answer %r" % out[:80])
        self.frame = int(parts[1])
        self.status = [int(v) for v in parts[3:19]]
        return parts[2] == "1"

    def stat(self, name):
        return self.status[STATUS.index(name)]

    @property
    def field_ready(self):
        return self.status[1]

    @property
    def map_id(self):
        return self.status[4]

    @property
    def in_battle(self):
        return self.status[5]

    def probe(self):
        out = self._cmd("e2e")
        if out == "e2e none":
            return None
        raw = bytes.fromhex(out[4:])
        p = Probe(raw)
        if p.magic != E2E_MAGIC:
            raise HarnessError("probe block has a bad magic (stale guest build?)")
        return p

    def dump(self, path):
        self._cmd("dump %s" % path)

    def opt(self, name, value):
        self._cmd("opt %s=%d" % (name, value))

    def sched(self, path):
        self._cmd("sched %s" % path)

    def quit(self):
        """Ends the run (the save is flushed); returns np_gp's summary line. Once the core stopped np_gp ends by
        itself; its last lines (the DEFECT the stop is, the summary) are read either way."""
        if self.proc.poll() is None:
            try:
                self.proc.stdin.write("quit\n")
                self.proc.stdin.flush()
            except (BrokenPipeError, OSError):
                pass
        try:
            rest = self.proc.stdout.read()
        except (OSError, ValueError):
            rest = ""
        self.proc.wait()
        for line in rest.splitlines():
            if line.startswith("DEFECT"):
                self.defects.append(line)
            elif line.startswith("frames "):
                self.summary = line
        if not self._log.closed:
            self._log.close()
        return self.summary

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        if not self._log.closed:
            self._log.close()


# ---- the game's tile behaviors (Platinum's include/constants/field/map_tile_behaviors.h; D/P's
# MetatileBehavior numbering is the same table: its asm predicates test the same values, e.g. the
# four jumps 0x38..0x3B and the waterfall 0x13).
_BEHAVIORS = None


def behaviors():
    global _BEHAVIORS
    if _BEHAVIORS is None:
        path = os.path.join(ROOT, "games", "platinum", "include", "constants", "field", "map_tile_behaviors.h")
        out, n, inside = {}, 0, False
        with open(path) as f:
            for line in f:
                if line.startswith("enum TileBehavior"):
                    inside = True
                    continue
                if inside and line.startswith("}"):
                    break
                m = re.match(r"\s*TILE_BEHAVIOR_(\w+)\s*(?:=\s*(\w+))?\s*,", line)
                if inside and m:
                    if m.group(2):
                        n = int(m.group(2), 0)
                    out[m.group(1)] = n
                    n += 1
        _BEHAVIORS = out
    return _BEHAVIORS
