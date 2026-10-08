"""Static world routes for Ruby/Sapphire/Emerald: which maps to cross, and which exit tile to aim for on each.

    w = World("emerald")                       # or "ruby" / "sapphire"; cached per process
    w.route("MAP_LITTLEROOT_TOWN", 10, 10, "MAP_RUSTBORO_CITY", 27, 20) -> [Leg, ...]   # raises NoRoute

A Leg is (map, x, z, kind, next_map): walk on `map` to its tile (x, z), in `map`'s own coordinates (map.json event
coordinates, (0, 0) top-left).
  "connection": (x, z) is the first tile across the border, outside the map (x -1 / width or z -1 / height): the
                connected map `next_map`'s tile as seen from `map`; stepping onto it changes the map id.
  "warp":       (x, z) is a warp event's tile on `map`; next_map is its dest_map. An animated door (MB_ANIMATED_DOOR)
                is walked into moving north from the tile below; an arrow mat (MB_*_ARROW_WARP) is stood on and pushed
                in its direction (World.push_dir); every other warp is stepped onto.
  "dive":       (x, z) is a diveable water tile on `map` (surfing): A there and YES dives to next_map.
  "emerge":     (x, z) is an underwater tile one may surface from: B there and YES surfaces on next_map.
  "goal":       the last leg, (tx, tz) on the destination map; next_map is None.

The route is the cheapest in steps (Dijkstra over (map, x, y, elevation) states) through a static model of every map,
built from the decomp's data (.cache/gba/pokeemerald, .cache/gba/pokeruby -- Ruby and Sapphire share one tree; cites
are pokeemerald's file:line, pokeruby's in brackets where the line differs):

Data
  data/maps/map_groups.json: map id = group index (group_order) << 8 | index in the group (MAP_GROUP / MAP_NUM).
  data/maps/<Map>/map.json: layout, connections (up/down/left/right; dive/emerge only with route(dive=True)),
    map_type, warp_events; scripts.inc's setdivewarp (the fixed dive warp of a map without that connection).
  data/layouts/layouts.json: width, height, blockdata_filepath (map.bin), primary/secondary tileset.
  map.bin: a u16 per tile, metatile id bits 0-9, collision bits 10-11, elevation bits 12-15
    (include/global.fieldmap.h:7-12 MAPGRID_*, both decomps).
  Behavior: GetMetatileAttributesById (src/fieldmap.c:375 [389]): metatile ids < NUM_METATILES_IN_PRIMARY (512,
    include/fieldmap.h:6 [7]) index the primary tileset's attributes, the rest the secondary's, past the end 0xFF;
    behavior = attribute & 0xFF (global.fieldmap.h:39 METATILE_ATTR_BEHAVIOR_MASK). The attribute file is the
    tileset's .metatileAttributes (src/data/tilesets/headers.h -> metatiles.h INCBIN_U16 [data/tilesets/headers.inc
    -> metatiles.inc .incbin]): u16 per metatile in both games.
  Behavior numbers are include/constants/metatile_behaviors.h's (Emerald an enum, pokeruby #defines; every value used
    here is the same in both).

On-foot step from (x, y) at elevation e in direction d (field_player_avatar.c:693 CheckForObjectEventCollision [592],
  as games/emerald/pc/src/emerald_e2e.c e2e_step):
  - a ledge: the tile entered is MB_JUMP_<d> (event_object_movement.c:7631 GetLedgeJumpDirection [7485]): jump two
    tiles; the landing must exist (GetMapBorderIdAt != CONNECTION_INVALID).
  - else blocked by the entered tile's collision, a missing tile (fieldmap.c:522 GetMapBorderIdAt [526]), the tile
    left by gOppositeDirectionBlockedMetatileFuncs and the tile entered by gDirectionBlockedMetatileFuncs
    (event_object_movement.c:893-905 [782-795]; metatile_behavior.c:933-977 [1000-1046] Is{East,West,North,South}Blocked),
    then by IsElevationMismatchAt (event_object_movement.c:7673 [7528 IsZCoordMismatchAt]): e 0 goes anywhere, tile
    elevation 0 or 15 takes any, else they must match.
  - the new elevation is ObjectEventUpdateElevation's (event_object_movement.c:7725 [7586]): unchanged when either tile
    is 15, else the new tile's; the "previous" elevation (PlayerGetElevation, field_player_avatar.c:1192 [1002], what
    warps and Surf compare) follows only tiles at 1..14. A state is (map, x, y, current, previous elevation).
  - never planned onto: MetatileBehavior_IsForcedMovementTile (metatile_behavior.c:338 [460]: MB_WALK_EAST..
    MB_TRICK_HOUSE_PUZZLE_8_FLOOR 0x40-0x48, the currents 0x50-0x53, MB_MUDDY_SLOPE 0xD0, MB_CRACKED_FLOOR 0xD2,
    MB_ICE 0x20, the secret base jump/spin mats 0xBB/0xBC) except the waterfall, handled below.
  - water: a tile with TILE_FLAG_SURFABLE (metatile_behavior.c:9 sTileBitAttributes [7]) only with surf=True. Land to
    water (IsPlayerFacingSurfableFishableWater, field_player_avatar.c:1322 [1121]): previous elevation 3 and a
    MetatileBehavior_IsSurfableFishableWater tile (metatile_behavior.c:1162 [1199]); the surfer takes the water's
    elevation (1). Water to land: an elevation mismatch is allowed onto elevation 3 (CanStopSurfing,
    field_player_avatar.c:729 [616]). MB_WATERFALL 0x13 is entered, and left, only going north.
Warps (field_control_avatar.c ProcessPlayerFieldInput):
  - fire only when a warp event sits on the tile at the player's previous elevation, or at elevation 0
    (GetWarpEventAtPosition :860 [812]); MAP_DYNAMIC / non-numeric dest_warp_id destinations are never used (the tile
    is not planned onto when stepping on it would fire one).
  - stepped onto (TryStartWarpEventScript :487/:702 [543/692]): IsWarpMetatileBehavior (:751 [731]): MB_ANIMATED_DOOR,
    MB_LADDER, the escalators, MetatileBehavior_IsNonAnimDoor (MB_NON_ANIMATED_DOOR, MB_WATER_DOOR,
    MB_DEEP_SOUTH_WARP), the Lavaridge gym warps, MB_AQUA_HIDEOUT_WARP, MB_MT_PYRE_HOLE, and in Emerald only
    MB_MOSSDEEP_GYM_WARP and the union room's MB_BRIDGE_OVER_OCEAN. A warp arrived on is not re-fired (no step taken).
  - arrow mats (TryArrowWarp :164-168/:688 [243-246/678]): standing on MB_{EAST,WEST,NORTH,SOUTH}_ARROW_WARP,
    MB_WATER_SOUTH_ARROW_WARP, MB_STAIRS_OUTSIDE_ABANDONED_SHIP (north), MB_SHOAL_CAVE_ENTRANCE (south) and pushing
    that way (metatile_behavior.c:288-320 [410-445]); checked before the step.
  - doors (TryDoorWarp :175-179/:833 [254-257/786]): pushing north into an MB_ANIMATED_DOOR tile (its warp event at
    the player's previous elevation, or at 0 when the player's own tile is at elevation 0: GetInFrontOfPlayerPosition
    :200-210 [293-303]).
    MB_PETALBURG_GYM_DOOR 0x8D is MetatileBehavior_IsDoor but not IsWarpDoor: it never warps.
  - arrival: the destination warp event's tile, elevation 0 (field_player_avatar.c:1391 [1189]) then updated on the
    tile. On a door the player walks one tile south (field_screen_effect.c:264 SetUpWarpExitTask -> Task_ExitDoor :338
    [field_fadetransition.c:182, 237]); on a MetatileBehavior_IsNonAnimDoor tile one tile in its facing
    (Task_ExitNonAnimDoor :386 [285]; facing is south, north for MB_DEEP_SOUTH_WARP: overworld.c:929
    GetAdjustedInitialDirection [723]).
Connections: a tile past the edge belongs to the connected map at the offset (fieldmap.c:178 FillSouthConnection and
  siblings [172]); stepping onto it moves the player there (CameraMove :603 [607], SetPositionFromConnection :578
  [582]): south (x - offset, 0), north (x - offset, height' - 1), east (0, y - offset), west (width' - 1, y - offset).
Dive (route(dive=True); TrySetDiveWarp field_control_avatar.c:965 [917], A/B through TrySetupDiveDownScript /
  TrySetupDiveEmergeScript :463/:473 [519/529], which also want FLAG_BADGE07_GET and a party member with Dive):
  - down: on the player's own tile, MetatileBehavior_IsDiveable (metatile_behavior.c:853 [927]: MB_SEMI_DEEP_WATER
    0x11, MB_DEEP_WATER 0x12, MB_SOOTOPOLIS_DEEP_WATER 0x14) on a map that is not MAP_TYPE_UNDERWATER;
  - up: on a MAP_TYPE_UNDERWATER map, any tile but MB_NO_SURFACING 0x19 / MB_SEAWEED_NO_SURFACING 0x2A
    (MetatileBehavior_IsUnableToEmerge :863 [IsNotSurfacable :937]);
  - where to (SetDiveWarp overworld.c:756 [575]): the map's dive/emerge connection at the same (x, y), else the
    fixed dive warp its map scripts set (setdivewarp MAP, [warp 255,] x, y), used here only when the map's
    scripts.inc has exactly one. The arrival takes the tile's elevation as a warp arrival does; cost as a warp.
Cost: 1 a step, 6 into tall grass (MB_TALL_GRASS 0x02, MB_LONG_GRASS 0x03, MB_ASHGRASS 0x24), 2 a ledge jump, 10 a
  warp (+1 for the walk out of a door).

Objects (NPCs, Cut trees, rocks, boulders), scripts, coord events and flags are invisible to this model: avoid_maps
and avoid_warps are the escape hatches.
"""
import heapq
import json
import os
import re
import sys
from array import array
from collections import namedtuple

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DECOMPS = {"emerald": "pokeemerald", "ruby": "pokeruby", "sapphire": "pokeruby"}

Leg = namedtuple("Leg", "map x z kind next_map")


class NoRoute(Exception):
    pass


# ---- metatile behaviors (include/constants/metatile_behaviors.h, the same numbers in both decomps)
MB_TALL_GRASS, MB_LONG_GRASS, MB_ASHGRASS = 0x02, 0x03, 0x24
MB_WATERFALL = 0x13
MB_JUMP = {0x38: (1, 0), 0x39: (-1, 0), 0x3A: (0, -1), 0x3B: (0, 1)}  # MB_JUMP_EAST/WEST/NORTH/SOUTH
MB_ANIMATED_DOOR, MB_NON_ANIMATED_DOOR, MB_WATER_DOOR, MB_DEEP_SOUTH_WARP = 0x69, 0x60, 0x6C, 0x6E
MB_PETALBURG_GYM_DOOR = 0x8D
GRASS = frozenset({MB_TALL_GRASS, MB_LONG_GRASS, MB_ASHGRASS})

# directions: 0 north, 1 south, 2 west, 3 east (the probe's order)
DIRS = ((0, -1), (0, 1), (-1, 0), (1, 0))

# MetatileBehavior_Is{North,South,East,West}Blocked (metatile_behavior.c:933-977): MB_IMPASSABLE_EAST 0x30, WEST 0x31,
# NORTH 0x32, SOUTH 0x33, NORTHEAST 0x34, NORTHWEST 0x35, SOUTHEAST 0x36, SOUTHWEST 0x37, SOUTH_AND_NORTH 0xC0,
# WEST_AND_EAST 0xC1, MB_SECRET_BASE_BREAKABLE_DOOR 0xBE (east and west)
_NORTH_BLOCKED = frozenset({0x32, 0x34, 0x35, 0xC0})
_SOUTH_BLOCKED = frozenset({0x33, 0x36, 0x37, 0xC0})
_EAST_BLOCKED = frozenset({0x30, 0x34, 0x36, 0xC1, 0xBE})
_WEST_BLOCKED = frozenset({0x31, 0x35, 0x37, 0xC1, 0xBE})
# moving d: the tile left must not block side d (gOppositeDirectionBlockedMetatileFuncs), the tile entered must not
# block the side it is entered from (gDirectionBlockedMetatileFuncs), event_object_movement.c:893-905
_LEAVE_BLOCKED = (_NORTH_BLOCKED, _SOUTH_BLOCKED, _WEST_BLOCKED, _EAST_BLOCKED)
_ENTER_BLOCKED = (_SOUTH_BLOCKED, _NORTH_BLOCKED, _EAST_BLOCKED, _WEST_BLOCKED)

# MetatileBehavior_IsForcedMovementTile (metatile_behavior.c:338) less MB_WATERFALL
FORBIDDEN = frozenset(set(range(0x40, 0x49)) | set(range(0x50, 0x54)) | {0xD0, 0xD2, 0x20, 0xBB, 0xBC})
# MetatileBehavior_IsSurfableFishableWater (metatile_behavior.c:1162): pond, interior/semi-deep, deep, Sootopolis
# deep, ocean water and the currents
SURF_START = frozenset({0x10, 0x11, 0x12, 0x14, 0x15, 0x50, 0x51, 0x52, 0x53})
# arrow warps and the direction pushed (metatile_behavior.c:288-320): MB_EAST_ARROW_WARP 0x62, WEST 0x63, NORTH 0x64,
# SOUTH 0x65, MB_WATER_SOUTH_ARROW_WARP 0x6D, MB_STAIRS_OUTSIDE_ABANDONED_SHIP 0x1B (north),
# MB_SHOAL_CAVE_ENTRANCE 0x1C (south)
ARROW = {0x62: 3, 0x63: 2, 0x64: 0, 0x65: 1, 0x6D: 1, 0x1B: 0, 0x1C: 1}
# IsWarpMetatileBehavior (field_control_avatar.c:751): MB_ANIMATED_DOOR, MB_LADDER 0x61, MB_UP/DOWN_ESCALATOR
# 0x6A/0x6B, IsNonAnimDoor (0x60, 0x6C, 0x6E), MB_LAVARIDGE_GYM_B1F_WARP 0x29, MB_LAVARIDGE_GYM_1F_WARP 0x68,
# MB_AQUA_HIDEOUT_WARP 0x67, MB_MT_PYRE_HOLE 0x0F; Emerald adds MB_MOSSDEEP_GYM_WARP 0x0E and IsUnionRoomWarp's
# MB_BRIDGE_OVER_OCEAN 0x70 (pokeruby field_control_avatar.c:731 has neither)
_STEP_WARP_RS = frozenset({0x69, 0x61, 0x6A, 0x6B, 0x60, 0x6C, 0x6E, 0x29, 0x68, 0x67, 0x0F})
STEP_WARP = {"pokeemerald": _STEP_WARP_RS | {0x0E, 0x70}, "pokeruby": _STEP_WARP_RS}
DOORS = frozenset({MB_ANIMATED_DOOR, MB_PETALBURG_GYM_DOOR})  # MetatileBehavior_IsDoor
NONANIM_DOORS = frozenset({MB_NON_ANIMATED_DOOR, MB_WATER_DOOR, MB_DEEP_SOUTH_WARP})  # MetatileBehavior_IsNonAnimDoor
# Maps a walker never crosses: the Seaside Cycling Road gates' coord events push a player on foot back
# (Route110_SeasideCyclingRoad{South,North}Entrance/scripts.inc BikeCheck: GetPlayerAvatarBike 0 -> NoBike), and
# walk_to does not ride the bike
ON_FOOT_BLOCKED = ("MAP_ROUTE110_SEASIDE_CYCLING_ROAD_SOUTH_ENTRANCE", "MAP_ROUTE110_SEASIDE_CYCLING_ROAD_NORTH_ENTRANCE")
# MetatileBehavior_IsDiveable / IsUnableToEmerge [IsNotSurfacable] (module docstring, Dive)
DIVEABLE = frozenset({0x11, 0x12, 0x14})
NO_EMERGE = frozenset({0x19, 0x2A})
_SETDIVEWARP = re.compile(r"^\s*setdivewarp\s+(MAP_\w+)((?:\s*,\s*\d+)+)\s*$", re.M)

COST_STEP, COST_GRASS, COST_WARP = 1, 6, 10
NUM_METATILES_IN_PRIMARY = 512
_CONN_DIRS = {"up": "n", "down": "s", "left": "w", "right": "e"}


class _Map:
    __slots__ = ("id", "name", "folder", "layout", "w", "h", "beh", "coll", "elev", "conns", "warps", "warp_at",
                 "underwater", "dive_to")


class World:
    """One game's maps (lazy per map), cached per process."""

    _worlds = {}

    def __new__(cls, game="emerald"):
        game = game.lower()
        if game not in DECOMPS:
            raise ValueError("unknown GBA game %r (one of %s)" % (game, ", ".join(sorted(DECOMPS))))
        w = cls._worlds.get(DECOMPS[game])
        if w is None:
            w = super().__new__(cls)
            w._load(DECOMPS[game])
            cls._worlds[DECOMPS[game]] = w
        return w

    # ---------------------------------------------------------------- loading
    def _load(self, decomp):
        self.decomp = decomp
        self.dir = os.path.join(ROOT, ".cache", "gba", decomp)
        with open(os.path.join(self.dir, "data", "maps", "map_groups.json")) as f:
            groups = json.load(f)
        with open(os.path.join(self.dir, "data", "layouts", "layouts.json")) as f:
            self._layouts = {l["id"]: l for l in json.load(f)["layouts"] if l.get("id")}
        self._ids, self._names, self._json, self._maps = {}, {}, {}, {}
        for g, group in enumerate(groups["group_order"]):
            for n, folder in enumerate(groups[group]):
                with open(os.path.join(self.dir, "data", "maps", folder, "map.json")) as f:
                    j = json.load(f)
                mid = g << 8 | n
                self._ids[j["id"]] = mid
                self._names[mid] = j["id"]
                self._json[mid] = j
        self._attrs = {}
        self._tileset_files = None
        self._surfable = self._parse_surfable()
        self.step_warp = STEP_WARP[decomp]

    def objects(self, mid):
        """[(local id, x, y)] of map mid's object events (map.json object_events; the local id is the 1-based
        index, as the probe reports ObjectEvent.localId): where a person stands before the game spawns it near
        the camera."""
        j = self._json.get(self.map_id(mid), {})
        return [(i + 1, o["x"], o["y"]) for i, o in enumerate(j.get("object_events", []))]
    def _parse_surfable(self):
        """sTileBitAttributes' TILE_FLAG_SURFABLE behaviors (metatile_behavior.c:9 [7])."""
        with open(os.path.join(self.dir, "src", "metatile_behavior.c")) as f:
            src = f.read()
        out = set()
        if self.decomp == "pokeruby":
            # TILE_ATTRIBUTES(unused, surfable, wildEncounter), one per behavior in order
            body = src[src.index("sTileBitAttributes[]"):]
            body = body[:body.index("};")]
            for i, m in enumerate(re.finditer(r"TILE_ATTRIBUTES\((TRUE|FALSE), (TRUE|FALSE), (TRUE|FALSE)\)", body)):
                if m.group(2) == "TRUE":
                    out.add(i)
        else:
            with open(os.path.join(self.dir, "include", "constants", "metatile_behaviors.h")) as f:
                hdr = re.sub(r"//.*", "", f.read())
            values, v = {}, 0
            for m in re.finditer(r"(MB_\w+)(?:\s*=\s*(\w+))?\s*,", hdr[hdr.index("enum"):]):
                if m.group(2):
                    v = int(m.group(2), 0)
                values[m.group(1)] = v
                v += 1
            body = src[src.index("sTileBitAttributes["):]
            body = body[:body.index("};")]
            for m in re.finditer(r"\[(MB_\w+)\]\s*=\s*([^,\n]+)", body):
                if "TILE_FLAG_SURFABLE" in m.group(2):
                    out.add(values[m.group(1)])
        return frozenset(out)

    def _tileset_attrs(self, label):
        """A tileset's metatile attribute u16s, from its .metatileAttributes INCBIN."""
        a = self._attrs.get(label)
        if a is not None:
            return a
        if self._tileset_files is None:
            files, attr_of = {}, {}
            if self.decomp == "pokeruby":
                with open(os.path.join(self.dir, "data", "tilesets", "metatiles.inc")) as f:
                    for m in re.finditer(r"(gMetatileAttributes_\w+)::[^\n]*\n\s*\.incbin \"([^\"]+)\"", f.read()):
                        files[m.group(1)] = m.group(2)
                with open(os.path.join(self.dir, "data", "tilesets", "headers.inc")) as f:
                    # gTileset_X:: then .byte isCompressed, .byte isSecondary, .2byte padding, .4byte tiles,
                    # palettes, metatiles, metatileAttributes, callback
                    for m in re.finditer(r"(gTileset_\w+)::[^\n]*((?:\n\s*\.\w+[^\n]*)+)", f.read()):
                        words = re.findall(r"\.4byte (\w+)", m.group(2))
                        if len(words) >= 4:
                            attr_of[m.group(1)] = words[3]
            else:
                with open(os.path.join(self.dir, "src", "data", "tilesets", "metatiles.h")) as f:
                    for m in re.finditer(r"(gMetatileAttributes_\w+)\[\]\s*=\s*INCBIN_U16\(\"([^\"]+)\"\)", f.read()):
                        files[m.group(1)] = m.group(2)
                with open(os.path.join(self.dir, "src", "data", "tilesets", "headers.h")) as f:
                    for m in re.finditer(r"(gTileset_\w+)\s*=\s*\{[^}]*?\.metatileAttributes\s*=\s*(\w+)", f.read()):
                        attr_of[m.group(1)] = m.group(2)
            self._tileset_files = {t: files[a] for t, a in attr_of.items() if a in files}
        a = array("H")
        path = self._tileset_files.get(label)
        if not path:
            raise KeyError("%s: no metatile attributes for tileset %s" % (self.decomp, label))
        with open(os.path.join(self.dir, path), "rb") as f:
            a.frombytes(f.read())
        if sys.byteorder != "little":
            a.byteswap()
        self._attrs[label] = a
        return a

    def _map(self, mid):
        m = self._maps.get(mid)
        if m is not None or mid in self._maps:
            return m
        j = self._json.get(mid)
        lay = self._layouts.get(j["layout"]) if j else None
        if not lay or not lay.get("blockdata_filepath"):
            self._maps[mid] = None
            return None
        m = _Map()
        m.id, m.name, m.folder, m.layout = mid, j["id"], j["name"], lay
        m.w, m.h = lay["width"], lay["height"]
        blocks = array("H")
        with open(os.path.join(self.dir, lay["blockdata_filepath"]), "rb") as f:
            blocks.frombytes(f.read())
        if sys.byteorder != "little":
            blocks.byteswap()
        prim = self._tileset_attrs(lay["primary_tileset"])
        sec = self._tileset_attrs(lay["secondary_tileset"])
        np_, ns = len(prim), len(sec)
        n = m.w * m.h
        beh, coll, elev = bytearray(n), bytearray(n), bytearray(n)
        for i in range(n):
            v = blocks[i]
            mt = v & 0x3FF
            if mt < NUM_METATILES_IN_PRIMARY:
                beh[i] = prim[mt] & 0xFF if mt < np_ else 0xFF
            else:
                beh[i] = sec[mt - NUM_METATILES_IN_PRIMARY] & 0xFF if mt - NUM_METATILES_IN_PRIMARY < ns else 0xFF
            coll[i] = (v >> 10) & 3
            elev[i] = v >> 12
        m.beh, m.coll, m.elev = bytes(beh), bytes(coll), bytes(elev)
        m.conns = {"n": [], "s": [], "w": [], "e": []}
        for c in j.get("connections") or ():
            d = _CONN_DIRS.get(c.get("direction"))
            if d and c.get("map") in self._ids:
                m.conns[d].append((int(c["offset"]), self._ids[c["map"]]))
        # Dive: (map id, x, y) the dive (or, underwater, the surfacing) leads to; x, y None = the same tile there
        m.underwater = j.get("map_type") == "MAP_TYPE_UNDERWATER"
        m.dive_to = None
        kind = "emerge" if m.underwater else "dive"
        for c in j.get("connections") or ():
            if c.get("direction") == kind and c.get("map") in self._ids:
                m.dive_to = (self._ids[c["map"]], None, None)
        if m.dive_to is None:
            try:
                with open(os.path.join(self.dir, "data", "maps", j["name"], "scripts.inc")) as f:
                    fixed = {(mm.group(1), tuple(int(v) for v in mm.group(2).split(",")[1:])[-2:])
                             for mm in _SETDIVEWARP.finditer(f.read())}
            except OSError:
                fixed = set()
            if len(fixed) == 1:
                (name, (fx, fy)), = fixed
                if name in self._ids:
                    m.dive_to = (self._ids[name], fx, fy)
        m.warps, m.warp_at = [], {}
        for k, wv in enumerate(j.get("warp_events") or ()):
            dest = self._ids.get(wv["dest_map"])  # MAP_DYNAMIC is no map
            try:
                dw = int(wv["dest_warp_id"])
            except ValueError:  # WARP_ID_DYNAMIC, WARP_ID_SECRET_BASE
                dest, dw = None, None
            rec = (wv["x"], wv["y"], wv["elevation"], dest, dw, k)
            m.warps.append(rec)
            m.warp_at.setdefault((wv["x"], wv["y"]), []).append(rec)
        self._maps[mid] = m
        return m

    # ---------------------------------------------------------------- public helpers
    def map_id(self, name):
        if isinstance(name, int):
            return name
        name = name if name.startswith("MAP_") else "MAP_" + name
        try:
            return self._ids[name]
        except KeyError:
            raise KeyError("no map %s in %s" % (name, self.decomp)) from None

    def map_name(self, mid):
        return self._names.get(mid, "map %d" % mid)

    def size(self, mid):
        m = self._map(self.map_id(mid))
        if m is None:
            raise KeyError("map %s has no layout" % self.map_name(self.map_id(mid)))
        return m.w, m.h

    def tile(self, mid, x, y):
        """(behavior, collision, elevation) of (x, y) on map mid, a tile past the edge resolved through the
        connection; None where there is no tile."""
        r = self._resolve(self._map(self.map_id(mid)), x, y)
        if r is None:
            return None
        m, i = r[0], r[3]
        return m.beh[i], m.coll[i], m.elev[i]

    def push_dir(self, mid, x, y):
        """The direction (0 north, 1 south, 2 west, 3 east) that fires the warp at (x, y): an arrow mat's, north
        into a door, None for a warp that fires when stepped onto."""
        t = self.tile(mid, x, y)
        if t is None:
            return None
        if t[0] in ARROW:
            return ARROW[t[0]]
        return 0 if t[0] == MB_ANIMATED_DOOR else None

    def warps(self, mid):
        """[(x, y, elevation, dest_map or None, dest_warp or None)] of map mid, in warp id order."""
        m = self._map(self.map_id(mid))
        return [w[:5] for w in m.warps] if m else []

    def surfable(self, beh):
        return beh in self._surfable

    # ---------------------------------------------------------------- the model
    def _resolve(self, m, x, y):
        """(map, x, y, index) of the tile drawn at (x, y) of map m: in m, or one past its edge in a connected map."""
        if m is None:
            return None
        w, h = m.w, m.h
        if 0 <= x < w:
            if 0 <= y < h:
                return m, x, y, y * w + x
            for off, cid in m.conns["s" if y >= h else "n"]:
                if off <= x:
                    c = self._map(cid)
                    if c is not None and x - off < c.w:
                        cy = y - h if y >= h else c.h + y
                        if 0 <= cy < c.h:
                            return c, x - off, cy, cy * c.w + x - off
            return None
        if 0 <= y < h:
            for off, cid in m.conns["e" if x >= w else "w"]:
                if off <= y:
                    c = self._map(cid)
                    if c is not None and y - off < c.h:
                        cx = x - w if x >= w else c.w + x
                        if 0 <= cx < c.w:
                            return c, cx, y - off, (y - off) * c.w + cx
        return None

    def _warp_event(self, m, x, y, elevation):
        """GetWarpEventAtPosition: the first warp event at (x, y) at `elevation` or at elevation 0."""
        for rec in m.warp_at.get((x, y), ()):
            if rec[2] == elevation or rec[2] == 0:
                return rec
        return None

    def _arrive(self, mid, k):
        """The state a warp to (mid, warp k) leaves the player in: (map, x, y, e, pe, extra cost), or None."""
        m = self._map(mid)
        if m is None or k >= len(m.warps):
            return None
        x, y = m.warps[k][0], m.warps[k][1]
        if not (0 <= x < m.w and 0 <= y < m.h):
            return None
        i = y * m.w + x
        te = m.elev[i]
        e = pe = 0  # InitPlayerAvatar spawns at ELEVATION_TRANSITION, then ObjectEventUpdateElevation on the tile
        if te != 15:
            e = te
            if te:
                pe = te
        b = m.beh[i]
        if b in DOORS or b in NONANIM_DOORS:  # the exit walk: south, north out of MB_DEEP_SOUTH_WARP
            dy = -1 if b == MB_DEEP_SOUTH_WARP else 1
            r = self._resolve(m, x, y + dy)
            if r is None:
                return m.id, x, y, e, pe, 0
            c, nx, ny, j = r
            ne = c.elev[j]
            if te != 15 and ne != 15:
                e = ne
                if ne:
                    pe = ne
            return c.id, nx, ny, e, pe, 1
        return m.id, x, y, e, pe, 0

    def route(self, src_map, sx, sz, dst_map, tx, tz, elevation=None, surf=False, avoid_maps=(), avoid_warps=(),
              dive=False):
        """The cheapest static route from (sx, sz) on src_map to (tx, tz) on dst_map as [Leg]; raises NoRoute.
        dive=True (implies surf) also dives and surfaces (module docstring, Dive)."""
        surf = surf or dive
        src = self._map(self.map_id(src_map))
        dst = self._map(self.map_id(dst_map))
        if src is None or dst is None:
            raise NoRoute("no layout for %s" % (src_map if src is None else dst_map))
        r = self._resolve(src, sx, sz)
        g = self._resolve(dst, tx, tz)
        if r is None:
            raise NoRoute("(%d,%d) is no tile of %s" % (sx, sz, src.name))
        if g is None:
            raise NoRoute("(%d,%d) is no tile of %s" % (tx, tz, dst.name))
        src, sx, sz, si = r
        dst, tx, tz = g[0], g[1], g[2]
        goal_m, goal_xy = dst.id, (tx, tz)
        avoid_m = ({self.map_id(a) for a in avoid_maps} | {self._ids[a] for a in ON_FOOT_BLOCKED if a in self._ids}) - {src.id}
        avoid_w = {(self.map_id(a), int(x), int(y)) for a, x, y in avoid_warps}
        te = src.elev[si]
        if elevation is None:
            e = te if te != 15 else 0
            pe = te if 0 < te < 15 else 3
        else:
            e = int(elevation)
            pe = e if 0 < e < 15 else (te if 0 < te < 15 else 3)
        start = (src.id, sx, sz, e, pe)
        if (src.id, sx, sz) == (goal_m, tx, tz):
            return [Leg(goal_m, tx, tz, "goal", None)]

        surfable, step_warp = self._surfable, self.step_warp
        maps = self._maps
        resolve, warp_event, arrive = self._resolve, self._warp_event, self._arrive
        dist = {start: 0}
        parent = {start: None}
        heap = [(0, 0, start)]
        tick = 1
        push = heapq.heappush
        pop = heapq.heappop
        found = None

        def warp_to(rec, cost, state, legs):
            """Queue the arrival of warp event `rec`, fired from `state` after `legs` (the warp's leg last)."""
            nonlocal tick
            dmid, dk = rec[3], rec[4]
            if dmid is None or dmid in avoid_m:
                return
            a = arrive(dmid, dk)
            if a is None:
                return
            ns = a[:5]
            nc = cost + COST_WARP + a[5]
            if nc < dist.get(ns, 1 << 60):
                dist[ns] = nc
                parent[ns] = (state, legs)
                push(heap, (nc, tick, ns))
                tick += 1

        while heap:
            cost, _, state = pop(heap)
            if cost > dist.get(state, 1 << 60):
                continue
            mid, x, y, e, pe = state
            if mid == goal_m and (x, y) == goal_xy:
                found = state
                break
            m = maps[mid]
            w = m.w
            i = y * w + x
            bt, et = m.beh[i], m.elev[i]
            # underwater the diver crosses "water" behaviours freely: only collision and elevation (3 or 0) count
            t_surf = bt in surfable and not m.underwater
            arrow = ARROW.get(bt)
            if dive and m.dive_to is not None and (bt not in NO_EMERGE if m.underwater else bt in DIVEABLE):
                tid, fx, fy = m.dive_to
                t = maps.get(tid) if tid in maps else self._map(tid)
                ax, ay = (x, y) if fx is None else (fx, fy)
                if t is not None and tid not in avoid_m and 0 <= ax < t.w and 0 <= ay < t.h:
                    te = t.elev[ay * t.w + ax]
                    ns = (tid, ax, ay, te if te != 15 else 0, te if 0 < te < 15 else pe)
                    nc = cost + COST_WARP
                    if nc < dist.get(ns, 1 << 60):
                        dist[ns] = nc
                        parent[ns] = (state, (Leg(mid, x, y, "emerge" if m.underwater else "dive", tid),))
                        push(heap, (nc, tick, ns))
                        tick += 1
            for d in (0, 1, 2, 3):
                dx, dy = DIRS[d]
                # TryArrowWarp: standing on an arrow mat, pushing its way
                if arrow == d and (mid, x, y) not in avoid_w:
                    rec = warp_event(m, x, y, pe)
                    if rec is not None:
                        warp_to(rec, cost, state, (Leg(mid, x, y, "warp", rec[3]),))
                        continue
                if t_surf and bt == MB_WATERFALL and d != 0:
                    continue
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < m.h:
                    n, j = m, ny * w + nx
                    nmx, nmy = nx, ny
                else:
                    r = resolve(m, nx, ny)
                    if r is None:
                        continue
                    n, nmx, nmy, j = r
                    if n.id in avoid_m:
                        continue
                bn = n.beh[j]
                # TryDoorWarp: pushing north into an animated door of this map (gMapHeader's warps), before any
                # collision
                if d == 0 and bn == MB_ANIMATED_DOOR and n is m and (mid, nx, ny) not in avoid_w:
                    rec = warp_event(m, nx, ny, pe if et else 0)
                    if rec is not None:
                        warp_to(rec, cost, state, (Leg(mid, nx, ny, "warp", rec[3]),))
                        continue
                cross = None if n is m else (nx, ny, n.id)
                if MB_JUMP.get(bn) == (dx, dy):
                    # a ledge: over it, landing one tile beyond whatever is there
                    r = resolve(n, nmx + dx, nmy + dy)
                    if r is None:
                        continue
                    n, nmx, nmy, j = r
                    if n.id in avoid_m:
                        continue
                    if cross is None and n is not m:
                        cross = (x + 2 * dx, y + 2 * dy, n.id)
                    bn = n.beh[j]
                    if n.coll[j] or bn in FORBIDDEN or (bn in surfable and not n.underwater and not t_surf):
                        continue
                    ne = n.elev[j]
                    if et == 15 or ne == 15:
                        ne_e, ne_pe = e, pe
                    else:
                        ne_e, ne_pe = ne, (ne if ne else pe)
                    step = 2 + (COST_GRASS - 1 if bn in GRASS else 0)
                else:
                    if n.coll[j] or bt in _LEAVE_BLOCKED[d] or bn in _ENTER_BLOCKED[d] or bn in FORBIDDEN:
                        continue
                    ne = n.elev[j]
                    mismatch = e != 0 and ne != 0 and ne != 15 and ne != e
                    n_surf = bn in surfable and not n.underwater
                    if n_surf:
                        if not surf or (bn == MB_WATERFALL and d != 0):
                            continue
                        if not t_surf:
                            # IsPlayerFacingSurfableFishableWater: Surf from elevation 3 onto fishable water
                            if pe != 3 or bn not in SURF_START:
                                continue
                            mismatch = False
                    if mismatch:
                        if not (t_surf and not n_surf and ne == 3):  # CanStopSurfing
                            continue
                        ne_e = ne_pe = 3
                    elif et == 15 or ne == 15:
                        ne_e, ne_pe = e, pe
                    else:
                        ne_e, ne_pe = ne, (ne if ne else pe)
                    step = COST_GRASS if bn in GRASS else COST_STEP
                nc = cost + step
                ns = (n.id, nmx, nmy, ne_e, ne_pe)
                legs = (Leg(mid, cross[0], cross[1], "connection", cross[2]),) if cross else ()
                if bn in step_warp and not (n.id == goal_m and (nmx, nmy) == goal_xy):
                    # TryStartWarpEventScript: stepping onto a warp tile fires its warp event
                    rec = warp_event(n, nmx, nmy, ne_pe)
                    if rec is not None and (n.id, nmx, nmy) not in avoid_w:
                        if rec[3] is not None:  # never onto a dynamic warp: somewhere this model cannot follow
                            warp_to(rec, nc, state, legs + (Leg(n.id, nmx, nmy, "warp", rec[3]),))
                        continue
                if nc < dist.get(ns, 1 << 60):
                    dist[ns] = nc
                    parent[ns] = (state, legs)
                    push(heap, (nc, tick, ns))
                    tick += 1
        if found is None:
            gi = tz * dst.w + tx
            raise NoRoute("no static route from %s (%d,%d) to %s (%d,%d)%s; the goal tile is behaviour 0x%02X, "
                          "collision %d, elevation %d" % (src.name, sx, sz, dst.name, tx, tz, " surfing" if surf else "",
                                                          dst.beh[gi], dst.coll[gi], dst.elev[gi]))
        legs = []
        s = found
        while parent[s] is not None:
            s, step_legs = parent[s]
            legs.extend(reversed(step_legs))
        legs.reverse()
        legs.append(Leg(goal_m, tx, tz, "goal", None))
        return legs

    # ---------------------------------------------------------------- display
    def render(self, mid):
        """The static map as ASCII, with a one-tile ring of the connected maps' tiles."""
        m = self._map(self.map_id(mid))
        if m is None:
            raise KeyError("map %s has no layout" % self.map_name(self.map_id(mid)))
        warp_tiles = {(w[0], w[1]) for w in m.warps}
        other = {}
        lines = ["%s (%d): %dx%d, layout %s, tilesets %s / %s" % (
            m.name, m.id, m.w, m.h, m.layout["id"], m.layout["primary_tileset"], m.layout["secondary_tileset"])]
        lines.append("legend: '#' collision, '.' floor, 'g' tall grass, '~' surfable water, 'F' waterfall, "
                     "'v' ledge, 'M' arrow mat, 'D' door, 'W' other warp event tile, ',' connected map, "
                     "' ' no tile, '=' other behaviour (listed)")
        lines.append("     " + "".join(str(x % 10) for x in range(-1, m.w + 1)))
        for y in range(-1, m.h + 1):
            row = []
            for x in range(-1, m.w + 1):
                r = self._resolve(m, x, y)
                if r is None:
                    row.append(" ")
                    continue
                c, cx, cy, j = r
                b = c.beh[j]
                inside = c is m
                if not inside:
                    ch = "#" if c.coll[j] else ","
                elif (x, y) in warp_tiles and b in ARROW:
                    ch = "M"
                elif (x, y) in warp_tiles and b in DOORS:
                    ch = "D"
                elif (x, y) in warp_tiles and b in self.step_warp:
                    ch = "W"
                elif b == MB_WATERFALL:
                    ch = "F"
                elif b in self._surfable:
                    ch = "~"
                elif b in MB_JUMP:
                    ch = "v"
                elif c.coll[j]:
                    ch = "#"
                elif b in GRASS:
                    ch = "g"
                elif b == 0:
                    ch = "."
                else:
                    ch = "="
                    other.setdefault(b, []).append((x, y))
                row.append(ch)
            lines.append("%4d %s" % (y, "".join(row)))
        elevs = sorted(set(m.elev))
        lines.append("elevations: %s" % " ".join(str(v) for v in elevs))
        if len(elevs) > 1:
            lines.append("     " + "".join(str(x % 10) for x in range(m.w)))
            for y in range(m.h):
                lines.append("%4d %s" % (y, "".join("%X" % m.elev[y * m.w + x] for x in range(m.w))))
        for d, name in (("n", "up"), ("s", "down"), ("w", "left"), ("e", "right")):
            for off, cid in m.conns[d]:
                lines.append("connection %s offset %d -> %s" % (name, off, self.map_name(cid)))
        for k, (x, y, el, dest, dw, _) in enumerate(m.warps):
            b = m.beh[y * m.w + x] if 0 <= x < m.w and 0 <= y < m.h else None
            lines.append("warp %d at (%d,%d) elevation %d behaviour %s -> %s warp %s" % (
                k, x, y, el, "0x%02X" % b if b is not None else "?", self.map_name(dest) if dest is not None
                else "(dynamic)", dw if dw is not None else "(dynamic)"))
        for b, tiles in sorted(other.items()):
            lines.append("'=' 0x%02X: %d tiles, e.g. %s" % (b, len(tiles), tiles[:4]))
        return "\n".join(lines)
