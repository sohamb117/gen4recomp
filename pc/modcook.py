#!/usr/bin/env python3
"""Cook a runtime content package into .cooked/.

A package is authored in the decomp's own formats under content/ and
records/. This script shells the tools the ROM build already uses
(nitrogfx, nitrobtx, nitromdl, msgenc, jsoncnv, dataproc, nitrosfx, the script
assembler) and writes loose cooked files the running port overlays.
It does not rebuild res/ into a private ROM and it does not cook
plugins/.

Recipes:

    content/narc/<nitro-path>/<idx>.png
        -> nitrogfx (png -> NCGR, no scramble flags)
        -> .cooked/narc/<nitro-path>/<idx>

    content/pokemon/<species>/{female,male}_{back,front}.png
        + sibling <face>.png.key (4-byte scramble)
        + sibling normal.pal and (for a new species) shiny.pal
        -> nitrogfx in.png out.NCGR -encodefronttoback -scan
        -> nitrogfx pal out.NCLR -bitdepth 8 -nopad -comp 10
        -> .cooked/narc/poketool/pokegra/pl_pokegra.narc/
              <species*6+0> female_back
              <species*6+1> male_back
              <species*6+2> female_front
              <species*6+3> male_front
              <species*6+4> normal palette
              <species*6+5> shiny palette
        Species numbers come from generated/species.txt
        (SPECIES_NONE is 0) plus records/species/<name>.json.
        A name that is not in either is a cook error. Keys and
        pals are companions, not their own recipes. A replace of
        a vanilla species cooks the faces that are present (a
        lone male_front + normal.pal is enough). A new species
        needs the decomp layout, all four faces and both pals,
        because the overlay refuses a hole.

    content/text/<bank>.json
        -> msgenc -e --json -c charmap.txt -H <tmp.h> in.json out
        -> .cooked/narc/msgdata/pl_msg.narc/<idx>
        <bank> is a generated/text_banks.txt name (diploma -> 1)
        or a decimal index.

    content/people/<name>.png
        -> nitrobtx pack --texture in --name <name>
           --spritesheet --frame-height 32 --extract-palette
           (the same argv res/graphics/field_sprites/meson.build
           uses for a 32x32 NPC)
        -> .cooked/narc/data/mmodel/mmodel.narc/<idx>
        <name> must have records/gfx/<name>.json. The first free
        OBJ_EVENT_GFX id is 276 (implicit rows 0..275;
        BERRY_SPROUT = 4096 is a hole). The first free mmodel
        member is the field_sprites.order count (470). The stock
        generic_32x32.nsbmd (member 421) is reused; this recipe
        does not encode a mesh. .cooked/generated/billboard_gfx.txt
        lists "gfx-id nsbtx-member" so the running port can clone
        the youngster billboard row without a rebuild.

    content/events/<map>.json
        -> tools/jsoncnv/event.py (decomp res/field/events schema)
        -> .cooked/narc/fielddata/eventdata/zone_event.narc/<idx>
        <map> is a zone_event.order stem (events_jubilife_city
        or jubilife_city -> member 2). A graphics_id of
        OBJ_EVENT_GFX_<NAME> that records/gfx allocated is
        rewritten to that integer before event.py runs; a
        vanilla name is left for event.py. The overlay replaces
        the whole member, so the JSON is the full map.

    content/props/<name>.nsbmd
        -> copy to .cooked/narc/fielddata/build_model/build_model.narc/<idx>
        <name> must have records/props/<name>.json. The first
        free prop id is the meson list count (590). The running
        port loads these on top of the area's area_build list.
        .cooked/generated/extra_props.txt lists the ids.

    content/props/<name>.gltf
    content/props/<name>.glb
        -> nitromdl in out.nsbmd
        -> same dest as a prebuilt .nsbmd
        One static triangle mesh, no skinning. A texture in the
        glTF is a cook error (TEX0 is not written yet). A sibling
        <name>.bin is the glTF buffer, not its own recipe.
        .nsbmd and .gltf/.glb for the same <name> is a cook error.

    content/maps/<name>/land_data.bin
        -> copy to .cooked/narc/fielddata/land_data/land_data.narc/<idx>
        A records/maps/<name>.json makes this a new header: the
        first free header is MAP_HEADER_COUNT (593), the first
        free land_data member is the map_data.order count (666),
        and cook writes a 1x1 map_matrix (first free 289) plus
        .cooked/generated/cooked_maps.txt so the header is
        addressable with no rebuild. A name that is a decimal
        index or map_data_NNN with no record replaces that
        vanilla land_data member (a prop-on-vanilla overlay).
        Optional siblings, same <name>, all require the record:
        events.json -> zone_event.narc append (first free 534)
        scripts.s   -> scr_seq.narc append (first free 1124)
                       plus a generated OnTransition init member
        init.s      -> authored init instead of the generated one
        text.json   -> pl_msg.narc append (first free 724)
        cooked_maps.txt points the header at those members.
        Extra keys on records/maps/<name>.json (area, map_type,
        bike, run, ...) override the NOTHING defaults.

    content/maps/<name>/terrain.json
    content/maps/<name>/model.gltf
    content/maps/<name>/model.glb
        -> pack one land_data member (same dest as land_data.bin)
        terrain.json is the recipe. model.gltf / model.glb is the
        embedded map NSBMD (nitromdl; same one-mesh rules as a
        prop). A sibling model.bin is the glTF buffer.
        land_data.bin and terrain.json for the same <name> is a
        cook error. JSON:
          attributes  32x32 or 1024 u16s, or omitted (all 0)
          tiles       32 strings of 32 chars, '.' walkable '#' blocked
          props       [{model, x, y, z, scale, rx, ry, rz}, ...]
                      model is a records/props name or an integer
                      at most 32; position/scale are game units
          bdhc.plates [{x0, z0, x1, z1, y}, ...] in game units
                      omitted: one flat plate, height 0, full map
                      (map_data_001 layout, map_data_178 constant)
                      sloped plates (y0 != y1) refuse: no oracle
        Member is 4-byte padded with 0xFF. Map model must fit
        the 0xF000 loaded-map buffer; BDHC must fit 0x9000.

    records/<kind>/<name>.json
        Allocates a frozen id. The record string is
        <mod-id>:<kind>/<name> (records/props/ -> kind "prop").
        {} auto-allocates past the measured vanilla ceiling;
        {"id": N} pins N so a server can bind the number. Extra
        keys are display-only. .cooked/ids.toml freezes the
        numbers; a lock that disagrees with a pin is a cook
        error. Two packages that assign different numbers to
        the same string, or the same number to two names, are
        a cook error.

Every other file under content/ or records/ is a cook error (no
recipe). Later recipes fill those holes; they do not change this
layout. A cook that wrote no cooked member and allocated no
record is a cook error.

The digest at .cooked/digest is FNV-1a-64 over the sorted
package-relative paths and bytes of every regular file under
content/ and records/. pc_modfs_boot recomputes the same number
and refuses a mismatch; it does not shell this script.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3

# Built binaries live under the ROM meson dir. Scripts live in the tree.
# Do not search PATH; a random nitrogfx is not this tree's tool.
TOOL_REL = {
    "nitrogfx": ("rom", "tools/nitrogfx/nitrogfx"),
    "nitrobtx": ("rom", "tools/nitrobtx/nitrobtx"),
    "nitromdl": ("rom", "tools/nitromdl/nitromdl"),
    "msgenc": ("rom", "tools/msgenc/msgenc"),
    "nitrosfx": ("rom", "tools/nitrosfx/nitrosfx"),
    "jsoncnv": ("src", "tools/jsoncnv/jsoncnv.py"),
    "eventpy": ("src", "tools/jsoncnv/event.py"),
    "scriptasm": ("src", "tools/scripts/make_script_bin.sh"),
}

# src/pokemon.c BuildPokemonSpriteTemplate, the default (pl_pokegra) arm:
#   character = species * 6 + face + (gender != GENDER_FEMALE ? 1 : 0)
#   palette   = species * 6 + 4 + shiny
# FACE_FRONT is 2; male is not GENDER_FEMALE, so it adds 1.
# make_pl_pokegra.py writes the six members in face-major order:
#   0 female_back, 1 male_back, 2 female_front, 3 male_front,
#   4 normal.pal, 5 shiny.pal.
FACE_FRONT = 2
POKEGRA_NITRO = "poketool/pokegra/pl_pokegra.narc"
PL_MSG_NITRO = "msgdata/pl_msg.narc"

# pl_pokegra holds species 0..493 (NONE through ARCEUS). EGG (494)
# and BAD_EGG (495) live in pl_otherpoke, measured: PC_LAB_SPRITE=494
# loads narc=117 char=132, not species*6. First free C id is 496.
# 494 species * 6 members = 2964, the vanilla FAT.
POKEGRA_VANILLA_SPECIES = 494
POKEGRA_VANILLA_COUNT = POKEGRA_VANILLA_SPECIES * 6
SPECIES_ARCEUS = 493
SPECIES_EGG = 494
SPECIES_BAD_EGG = 495

# records/<dir>/<name>.json -> kind token in the frozen string.
# ROADMAP writes coolhouse:prop/cool_house from records/props/.
RECORD_KIND = {
    "species": "species",
    "props": "prop",
    "gfx": "gfx",
    "maps": "map",
    "items": "item",
}

# A compiled #define that refuses a new id forces a rebuild into
# build/pc-mods (or --modinclude). NARC appends do not. Species
# pokegra is the no-rebuild path: BuildPokemonSpriteTemplate has
# no ceiling on the default arm. Billboard people are the same:
# overlay005's four gfx tables are sentinel-scanned and the port
# hook clones the youngster row for a cooked id, so gfx is not
# in this set. Props and map headers grew the same kind of hook
# (area extra-prop load + cooked MapHeader table), so they are
# not in this set either. Items still compile into a C table.
KIND_NEEDS_REBUILD = frozenset({"item"})

# res/graphics/field_sprites/field_sprites.narc is the ROM file
# at data/mmodel/mmodel.narc. Member 421 is generic_32x32.nsbmd.
# The order file's line count is the vanilla FAT (0..n-1).
MMODEL_NITRO = "data/mmodel/mmodel.narc"
ZONE_EVENT_NITRO = "fielddata/eventdata/zone_event.narc"
BUILD_MODEL_NITRO = "fielddata/build_model/build_model.narc"
LAND_DATA_NITRO = "fielddata/land_data/land_data.narc"
MAP_MATRIX_NITRO = "fielddata/mapmatrix/map_matrix.narc"
SCR_SEQ_NITRO = "fielddata/script/scr_seq.narc"

# LoadedMapBuffers / BDHC_BUFFER_SIZE. A packed member that does
# not fit is refused rather than overflowing the field loader.
MAP_MODEL_FILE_SIZE = 0xF000
BDHC_BUFFER_SIZE = 0x9000
MAX_MAP_PROPS = 32
TERRAIN_ATTR_COUNT = 32 * 32
FX32_ONE = 4096

# records/maps extra keys that override NEW_MAP_HEADER_FIELDS.
# matrix / scripts / init / msg / events are cooked, not authored.
MAP_HEADER_OVERRIDE = {
    "area": 0,
    "preloaded": 1,
    "day": 6,
    "night": 7,
    "wild": 8,
    "label": 10,
    "window": 11,
    "weather": 12,
    "camera": 13,
    "map_type": 14,
    "battle_bg": 15,
    "bike": 16,
    "run": 17,
    "escape": 18,
    "fly": 19,
}

# Auto-generated init when scripts.s is present and init.s is not.
# OnTransition 1 runs the first ScriptEntry of the regular archive.
DEFAULT_INIT_SCRIPT = (
    '#include "macros/scrcmd.inc"\n'
    "\n"
    "    InitScriptEntry_OnTransition 1\n"
    "    InitScriptEntryEnd\n"
    "\n"
    "    InitScriptEnd\n"
)

# AreaDataManager.mapPropModelFiles is 768 slots. A prop id at
# or past that is not a NARC problem; the area loader would
# write off the end of the array.
MAX_MAP_PROP_MODEL_FILES = 768

# MAP_HEADER_NOTHING's numeric fields, with mapMatrixID swapped
# for the cooked 1x1. scripts_empty=401, scripts_init_empty=897
# (scr_seq.naix), TEXT_BANK_JUBILIFE_CITY=23, SEQ_DUMMY=1000,
# events_empty=0, LocationNames_Text_MysteryZone=0,
# MAP_LABEL_WINDOW_WATER=6, BACKGROUND_FOREST=3.
NEW_MAP_HEADER_FIELDS = (
    0, 0, 0, 401, 897, 23, 1000, 1000, 65535, 0, 0, 6, 0, 0, 0, 3,
    0, 0, 0, 0,
)

POKEGRA_FACES = (
    ("female_back.png", 0),
    ("male_back.png", 1),
    ("female_front.png", 2),
    ("male_front.png", 3),
)
POKEGRA_PALS = (
    ("normal.pal", 4),
    ("shiny.pal", 5),
)
POKEGRA_COMPANIONS = frozenset(
    name + ".key" for name, _ in POKEGRA_FACES
) | frozenset(name for name, _ in POKEGRA_PALS)

# res/pokemon/<name>/ extras. Icon and cry are not this recipe;
# leaving them in the folder is not a cook error.
POKEGRA_IGNORE = frozenset({
    "data.json", "sprite_data.json", "icon.png", "cry.wav", "cry.txt",
    "footprint.png", "meson.build",
})

_species_ids: dict[str, int] | None = None
_text_banks: dict[str, int] | None = None
_species_extra: dict[str, int] = {}
_pkg_ids: dict[str, dict[str, int]] = {}
_pkg_kinds: dict[str, dict[str, str]] = {}
_pkg_rebuild_why: dict[str, list[str]] = {}
_cooked_pokemon: set[str] = set()
_gfx_nsbtx: dict[str, int] = {}
_cooked_people: set[str] = set()
_cooked_props: set[str] = set()
_cooked_maps: set[str] = set()
_map_land: dict[str, int] = {}
_map_matrix: dict[str, int] = {}
_map_events: dict[str, int] = {}
_map_scripts: dict[str, int] = {}
_map_init: dict[str, int] = {}
_map_msg: dict[str, int] = {}
_map_header_overrides: dict[str, dict[str, int]] = {}


def die(msg: str) -> None:
    print("cook: " + msg, file=sys.stderr)
    sys.exit(2)


def fnv1a(h: int, data: bytes) -> int:
    for b in data:
        h ^= b
        h = (h * FNV64_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def input_files(pkg: Path) -> list[tuple[str, Path]]:
    """Regular files under content/ and records/, package-relative."""
    found: list[tuple[str, Path]] = []

    def walk(dirpath: Path, rel: str) -> None:
        try:
            names = sorted(os.listdir(dirpath))
        except FileNotFoundError:
            return
        for name in names:
            if name in (".", ".."):
                continue
            child = dirpath / name
            childrel = name if rel == "" else rel + "/" + name
            try:
                st = child.stat()
            except OSError:
                die("claimed file missing: %s" % childrel)
            if stat.S_ISREG(st.st_mode):
                found.append((childrel, child))
            elif stat.S_ISDIR(st.st_mode):
                walk(child, childrel)
            else:
                die("claimed file missing: %s" % childrel)

    walk(pkg / "content", "content")
    walk(pkg / "records", "records")
    found.sort(key=lambda x: x[0])
    return found


def digest_hex(pkg: Path) -> str:
    h = FNV64_OFFSET
    for rel, path in input_files(pkg):
        h = fnv1a(h, rel.encode("utf-8"))
        h = fnv1a(h, b"\0")
        h = fnv1a(h, path.read_bytes())
        h = fnv1a(h, b"\0")
    return "%016x" % h


def digest_line(pkg: Path) -> str:
    return "v1 %s" % digest_hex(pkg)


def split_names(s: str) -> list[str]:
    names = []
    for tok in s.replace(",", " ").split():
        if tok:
            names.append(tok)
    return names


def read_loadorder(path: Path) -> list[str]:
    if not path.is_file():
        return []
    names = []
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            names.append(line)
    return names


def resolve_packages(mods_dir: Path, mods: str | None) -> list[Path]:
    if mods is not None:
        names = split_names(mods)
    else:
        names = read_loadorder(mods_dir / "loadorder.txt")
    pkgs = []
    for name in names:
        path = mods_dir / name
        if not path.is_dir():
            die("unknown package '%s' (looked in %s)" % (name, mods_dir))
        toml = path / "mod.toml"
        if not toml.is_file():
            if (path / "src").is_dir() and (path / "patches").is_dir():
                die("'%s' is a compile-time plugin (src/ + patches/); "
                    "use MODS=, not PC_MODS" % name)
            die("unknown package '%s' (looked in %s)" % (name, mods_dir))
        pkgs.append(path)
    return pkgs


def rom_build(cli_dir: str | None) -> Path:
    if cli_dir:
        return Path(cli_dir)
    env = os.environ.get("PC_ROM_BUILD")
    if env:
        return Path(env)
    return ROOT / "build" / "rom"


def find_tool(name: str, rom: Path, needed_for: str) -> Path:
    kind, rel = TOOL_REL[name]
    path = (rom / rel) if kind == "rom" else (ROOT / rel)
    if not path.is_file():
        die("%s not found (needed for %s); look in %s"
            % (name, needed_for, path.parent))
    return path


def run_tool(name: str, argv: list[str], src: str,
             env: dict[str, str] | None = None) -> None:
    try:
        r = subprocess.run(argv, capture_output=True, text=True, env=env)
    except OSError as e:
        die("%s failed on %s: %s" % (name, src, e))
    if r.returncode != 0:
        err = (r.stderr or r.stdout or "exit %d" % r.returncode).strip()
        die("%s failed on %s: %s" % (name, src, err))


def load_const_ids(path: Path, prefix: str) -> dict[str, int]:
    if not path.is_file():
        die("%s not found" % path)
    ids: dict[str, int] = {}
    for i, raw in enumerate(path.read_text().splitlines()):
        name = raw.strip()
        if not name.startswith(prefix):
            continue
        ids[name[len(prefix):].lower()] = i
    return ids


def vanilla_species_ids() -> dict[str, int]:
    global _species_ids
    if _species_ids is None:
        _species_ids = load_const_ids(ROOT / "generated" / "species.txt",
                                      "SPECIES_")
        if not _species_ids:
            die("generated/species.txt has no SPECIES_ entries")
    return _species_ids


def species_ids() -> dict[str, int]:
    ids = dict(vanilla_species_ids())
    ids.update(_species_extra)
    return ids


def text_bank_ids() -> dict[str, int]:
    global _text_banks
    if _text_banks is None:
        _text_banks = load_const_ids(ROOT / "generated" / "text_banks.txt",
                                     "TEXT_BANK_")
        if not _text_banks:
            die("generated/text_banks.txt has no TEXT_BANK_ entries")
    return _text_banks


def strip_toml_comment(line: str) -> str:
    out = []
    in_str = False
    i = 0
    while i < len(line):
        c = line[i]
        if c == '"' and not in_str:
            in_str = True
            out.append(c)
        elif c == '"' and in_str:
            in_str = False
            out.append(c)
        elif c == '#' and not in_str:
            break
        else:
            out.append(c)
        i += 1
    return "".join(out).strip()


def parse_ids_toml(path: Path) -> dict[str, int]:
    """Same grammar as pc_modfs.c: one "record-string" = integer per line."""
    if not path.is_file():
        return {}
    out: dict[str, int] = {}
    for raw in path.read_text().splitlines():
        line = strip_toml_comment(raw)
        if not line:
            continue
        if line[0] == '[' and ',' not in line.split(']', 1)[0]:
            die("tables are not read; put the id locks at the top (%s)"
                % path)
        if '=' not in line:
            die("expected key = value in %s" % path)
        key, val = line.split('=', 1)
        key = key.strip()
        val = val.strip()
        if key.startswith('"'):
            if len(key) < 2 or not key.endswith('"'):
                die("unterminated key in %s" % path)
            key = key[1:-1]
        if key == "":
            die("empty key in %s" % path)
        if not re.fullmatch(r"-?\d+", val):
            die("expected an integer for '%s' in %s" % (key, path))
        out[key] = int(val)
    return out


def write_ids_toml(path: Path, ids: dict[str, int]) -> None:
    lines = []
    for key in sorted(ids):
        lines.append('"%s" = %d\n' % (key, ids[key]))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(lines))


def parse_record_rel(rel: str) -> tuple[str, str] | None:
    """records/<dir>/<name>.json -> (kind token, name) or None."""
    parts = rel.split("/")
    if (len(parts) != 3
            or parts[0] != "records"
            or not parts[2].endswith(".json")
            or parts[2] == ".json"
            or parts[1] == ""):
        return None
    kind_dir = parts[1]
    if kind_dir not in RECORD_KIND:
        return None
    return RECORD_KIND[kind_dir], parts[2][:-5]


def load_record_json(src: Path, rel: str) -> dict:
    try:
        text = src.read_text()
    except OSError as e:
        die("cannot read %s: %s" % (rel, e))
    if not text.strip():
        return {}
    try:
        data = json.loads(text)
    except json.JSONDecodeError as e:
        die("invalid JSON in %s: %s" % (rel, e))
    if data is None:
        return {}
    if not isinstance(data, dict):
        die("%s must be a JSON object" % rel)
    return data


def record_json_id(src: Path, rel: str, kind: str) -> int | None:
    data = load_record_json(src, rel)
    for key in ("id", kind):
        if key in data:
            val = data[key]
            if isinstance(val, bool) or not isinstance(val, int):
                die("%s field '%s' must be an integer" % (rel, key))
            if val < 1:
                die("%s field '%s' must be >= 1 (got %d)" % (rel, key, val))
            return val
    return None


def record_map_overrides(src: Path, rel: str) -> dict[str, int]:
    """Optional MapHeader fields on records/maps/<name>.json."""
    data = load_record_json(src, rel)
    out: dict[str, int] = {}
    for key, val in data.items():
        if key in ("id", "map"):
            continue
        if key not in MAP_HEADER_OVERRIDE:
            continue
        if isinstance(val, bool) or not isinstance(val, int):
            die("%s field '%s' must be an integer" % (rel, key))
        if val < 0 or val > 65535:
            die("%s field '%s' is out of range (0..65535)" % (rel, key))
        out[key] = val
    return out


def vanilla_taken_for_kind(kind: str) -> dict[int, str]:
    """id -> vanilla name, so a pin cannot steal SPECIES_EGG."""
    taken: dict[int, str] = {}
    if kind == "species":
        for name, i in vanilla_species_ids().items():
            taken[i] = name
        return taken
    if kind == "gfx":
        # Implicit rows only: BERRY_SPROUT = 4096 is a hole, not a ceiling.
        path = ROOT / "generated" / "object_events_gfx.txt"
        n = 0
        for raw in path.read_text().splitlines():
            name = raw.strip()
            if not name.startswith("OBJ_EVENT_GFX_"):
                continue
            rest = name[len("OBJ_EVENT_GFX_"):]
            if "=" in rest:
                continue
            taken[n] = rest.lower()
            n += 1
        return taken
    if kind == "map":
        path = ROOT / "generated" / "map_headers.txt"
        n = 0
        for raw in path.read_text().splitlines():
            name = raw.strip()
            if not name.startswith("MAP_HEADER_"):
                continue
            rest = name[len("MAP_HEADER_"):]
            if "=" in rest:
                continue
            # COUNT is the ceiling, not a map. INVALID/DYNAMIC are
            # aliases and never appear without '='.
            if rest == "COUNT":
                continue
            taken[n] = rest.lower()
            n += 1
        return taken
    if kind == "item":
        path = ROOT / "generated" / "items.txt"
        n = 0
        for raw in path.read_text().splitlines():
            name = raw.strip()
            if name == "MAX_ITEMS" or not name.startswith("ITEM_"):
                continue
            rest = name[len("ITEM_"):]
            if "=" in rest:
                continue
            taken[n] = rest.lower()
            n += 1
        return taken
    if kind == "prop":
        meson = ROOT / "res" / "field" / "props" / "models" / "meson.build"
        if not meson.is_file():
            die("res/field/props/models/meson.build not found "
                "(needed to measure the vanilla prop ceiling)")
        names = re.findall(r"'([^']+\.nsbmd)'", meson.read_text())
        if not names:
            die("no prop models in %s" % meson)
        for i, name in enumerate(names):
            taken[i] = Path(name).stem.lower()
        return taken
    die("unknown record kind '%s'" % kind)
    raise AssertionError("unreachable")


def next_free_id(taken: set[int], vanilla_ids: dict[int, str]) -> int:
    start = (max(vanilla_ids) + 1) if vanilla_ids else 0
    n = start
    while n in taken:
        n += 1
    if n >= 65535:
        die("no free id past %d" % start)
    return n


def record_string(pkg_id: str, kind: str, name: str) -> str:
    return "%s:%s/%s" % (pkg_id, kind, name)


def read_pkg_id(pkg: Path) -> str:
    toml = pkg / "mod.toml"
    if not toml.is_file():
        die("missing mod.toml in %s" % pkg)
    for raw in toml.read_text().splitlines():
        line = strip_toml_comment(raw)
        if not line or "=" not in line:
            continue
        key, val = line.split("=", 1)
        if key.strip() == "id":
            val = val.strip()
            if val.startswith('"') and val.endswith('"') and len(val) >= 2:
                val = val[1:-1]
            if val:
                return val
    die("mod.toml in '%s' has no id" % pkg.name)
    raise AssertionError("unreachable")


def mmodel_vanilla_count() -> int:
    """field_sprites.order line count = vanilla mmodel.narc FAT."""
    path = ROOT / "res" / "graphics" / "field_sprites" / "field_sprites.order"
    if not path.is_file():
        die("res/graphics/field_sprites/field_sprites.order not found "
            "(needed to measure the vanilla mmodel ceiling)")
    n = 0
    for raw in path.read_text().splitlines():
        if raw.strip():
            n += 1
    if n < 1:
        die("field_sprites.order is empty")
    return n


def zone_event_ids() -> dict[str, int]:
    path = ROOT / "res" / "field" / "events" / "zone_event.order"
    if not path.is_file():
        die("res/field/events/zone_event.order not found")
    ids: dict[str, int] = {}
    n = 0
    for raw in path.read_text().splitlines():
        name = raw.strip()
        if not name:
            continue
        ids[name] = n
        if name.startswith("events_"):
            ids[name[len("events_"):]] = n
        n += 1
    return ids


def land_data_vanilla_count() -> int:
    """map_data.order line count = vanilla land_data.narc FAT."""
    path = ROOT / "res" / "field" / "maps" / "data" / "map_data.order"
    if not path.is_file():
        die("res/field/maps/data/map_data.order not found "
            "(needed to measure the vanilla land_data ceiling)")
    n = 0
    for raw in path.read_text().splitlines():
        if raw.strip():
            n += 1
    if n < 1:
        die("map_data.order is empty")
    return n


def map_matrix_vanilla_count() -> int:
    """map_matrix.order line count = vanilla map_matrix.narc FAT."""
    path = ROOT / "res" / "field" / "matrices" / "map_matrices.order"
    if not path.is_file():
        die("res/field/matrices/map_matrices.order not found "
            "(needed to measure the vanilla map_matrix ceiling)")
    n = 0
    for raw in path.read_text().splitlines():
        if raw.strip():
            n += 1
    if n < 1:
        die("map_matrix.order is empty")
    return n


def zone_event_vanilla_count() -> int:
    """zone_event.order line count = vanilla zone_event.narc FAT."""
    path = ROOT / "res" / "field" / "events" / "zone_event.order"
    if not path.is_file():
        die("res/field/events/zone_event.order not found "
            "(needed to measure the vanilla zone_event ceiling)")
    n = 0
    for raw in path.read_text().splitlines():
        if raw.strip():
            n += 1
    if n < 1:
        die("zone_event.order is empty")
    return n


def scr_seq_vanilla_count() -> int:
    """scripts.order line count = vanilla scr_seq.narc FAT."""
    path = ROOT / "res" / "field" / "scripts" / "scripts.order"
    if not path.is_file():
        die("res/field/scripts/scripts.order not found "
            "(needed to measure the vanilla scr_seq ceiling)")
    n = 0
    for raw in path.read_text().splitlines():
        if raw.strip():
            n += 1
    if n < 1:
        die("scripts.order is empty")
    return n


def text_bank_vanilla_count() -> int:
    """generated/text_banks.txt row count = vanilla pl_msg.narc FAT."""
    return len(text_bank_ids())


def allocate_all_records(pkgs: list[Path]) -> None:
    """Assign ids for every records/*.json across the enabled set."""
    global _species_extra, _pkg_ids, _pkg_kinds, _pkg_rebuild_why, _gfx_nsbtx
    global _map_land, _map_matrix, _map_events, _map_scripts, _map_init
    global _map_msg, _map_header_overrides
    _species_extra = {}
    _pkg_ids = {}
    _pkg_kinds = {}
    _pkg_rebuild_why = {}
    _gfx_nsbtx = {}
    _map_land = {}
    _map_matrix = {}
    _map_events = {}
    _map_scripts = {}
    _map_init = {}
    _map_msg = {}
    _map_header_overrides = {}

    taken: dict[str, set[int]] = {}
    owner: dict[tuple[str, int], str] = {}
    vanilla: dict[str, dict[int, str]] = {}
    string_to_id: dict[str, int] = {}

    for kind in RECORD_KIND.values():
        vanilla[kind] = vanilla_taken_for_kind(kind)
        taken[kind] = set(vanilla[kind])

    # Honour every package's existing lock so a removed record is not
    # reused out from under a second launch.
    for pkg in pkgs:
        locks = parse_ids_toml(pkg / ".cooked" / "ids.toml")
        for key, num in locks.items():
            parts = key.split(":", 1)
            if len(parts) != 2 or "/" not in parts[1]:
                die("bad lock key '%s' in %s" % (key, pkg / ".cooked" / "ids.toml"))
            kind = parts[1].split("/", 1)[0]
            if kind not in taken:
                die("unknown kind in lock '%s' (%s)" % (key, pkg.name))
            taken[kind].add(num)

    for pkg in pkgs:
        pkg_id = read_pkg_id(pkg)
        locks = parse_ids_toml(pkg / ".cooked" / "ids.toml")
        assigned: dict[str, int] = {}
        kinds: dict[str, str] = {}
        why: list[str] = []
        files = input_files(pkg)
        recs: list[tuple[str, str, Path, str]] = []
        for rel, src in files:
            parsed = parse_record_rel(rel)
            if parsed is None:
                if rel.startswith("records/"):
                    die("no recipe for %s" % rel)
                continue
            kind, name = parsed
            recs.append((kind, name, src, rel))
        recs.sort(key=lambda t: (t[0], t[1]))
        for kind, name, src, rel in recs:
            key = record_string(pkg_id, kind, name)
            pinned = record_json_id(src, rel, kind)
            locked = locks.get(key)
            vanilla_by_name = {n: i for i, n in vanilla[kind].items()}

            if name in vanilla_by_name:
                vid = vanilla_by_name[name]
                if pinned is not None and pinned != vid:
                    die("record %s pins %d but vanilla '%s' is %d"
                        % (key, pinned, name, vid))
                if locked is not None and locked != vid:
                    die("lock collision: %s locked %d, vanilla is %d"
                        % (key, locked, vid))
                num = vid
            elif pinned is not None and locked is not None and pinned != locked:
                die("lock collision: %s locked %d, record says %d"
                    % (key, locked, pinned))
            elif locked is not None:
                num = locked
            elif pinned is not None:
                num = pinned
            else:
                num = next_free_id(taken[kind], vanilla[kind])

            if key in string_to_id and string_to_id[key] != num:
                die("lock collision: %s is %d and %d"
                    % (key, string_to_id[key], num))

            if name not in vanilla_by_name:
                prev = owner.get((kind, num))
                if prev is not None and prev != key:
                    die("id %d already taken for %s (%s vs %s)"
                        % (num, kind, prev, key))
                if num in vanilla[kind] and vanilla[kind][num] != name:
                    die("id %d is vanilla '%s', not '%s' (%s)"
                        % (num, vanilla[kind][num], name, key))

            if kind == "species":
                if num * 6 + 5 >= 65535:
                    die("species %d would overflow pl_pokegra (%s)"
                        % (num, key))
                _species_extra[name] = num
            if kind == "prop" and num >= MAX_MAP_PROP_MODEL_FILES:
                die("prop %d is past AreaDataManager's %d-slot table (%s)"
                    % (num, MAX_MAP_PROP_MODEL_FILES, key))
            if kind == "map":
                _map_header_overrides[key] = record_map_overrides(src, rel)

            if kind in KIND_NEEDS_REBUILD and name not in vanilla_by_name:
                why.append("%s allocated %s %d (compiled table)"
                           % (rel, kind, num))

            taken[kind].add(num)
            owner[(kind, num)] = key
            string_to_id[key] = num
            assigned[key] = num
            kinds[key] = kind

        # Keep stale locks so deleting a record and putting it back
        # does not renumber it.
        for key, num in locks.items():
            if key not in assigned:
                assigned[key] = num

        _pkg_ids[pkg.name] = assigned
        _pkg_kinds[pkg.name] = kinds
        _pkg_rebuild_why[pkg.name] = why

    # NSBTX members for new people, across the enabled set, so two
    # packages do not both plant mmodel/470. Frozen gfx ids live in
    # ids.toml; the member number is recomputed each cook.
    new_gfx = []
    vanilla_gfx = vanilla_taken_for_kind("gfx")
    vanilla_gfx_names = set(vanilla_gfx.values())
    for pkg in pkgs:
        kinds = _pkg_kinds.get(pkg.name, {})
        assigned = _pkg_ids.get(pkg.name, {})
        for key, num in assigned.items():
            if kinds.get(key) != "gfx":
                continue
            name = key.split("/")[-1]
            if name in vanilla_gfx_names:
                continue
            new_gfx.append((key, num))
    new_gfx.sort(key=lambda t: (t[1], t[0]))
    next_member = mmodel_vanilla_count()
    for key, _num in new_gfx:
        if next_member >= 65535:
            die("no free mmodel member past %d" % next_member)
        _gfx_nsbtx[key] = next_member
        next_member += 1

    # Land-data members and 1x1 matrices for new map headers.
    new_maps = []
    vanilla_maps = vanilla_taken_for_kind("map")
    vanilla_map_names = set(vanilla_maps.values())
    for pkg in pkgs:
        kinds = _pkg_kinds.get(pkg.name, {})
        assigned = _pkg_ids.get(pkg.name, {})
        for key, num in assigned.items():
            if kinds.get(key) != "map":
                continue
            name = key.split("/")[-1]
            if name in vanilla_map_names:
                continue
            new_maps.append((key, num))
    new_maps.sort(key=lambda t: (t[1], t[0]))
    next_land = land_data_vanilla_count()
    next_matrix = map_matrix_vanilla_count()
    next_event = zone_event_vanilla_count()
    next_script = scr_seq_vanilla_count()
    next_msg = text_bank_vanilla_count()
    for key, _num in new_maps:
        if next_land >= 65535:
            die("no free land_data member past %d" % next_land)
        if next_matrix >= 65535:
            die("no free map_matrix member past %d" % next_matrix)
        _map_land[key] = next_land
        _map_matrix[key] = next_matrix
        next_land += 1
        next_matrix += 1
        pkg = None
        for p in pkgs:
            if key in _pkg_ids.get(p.name, {}):
                pkg = p
                break
        if pkg is None:
            continue
        name = key.split("/")[-1]
        maps_dir = pkg / "content" / "maps" / name
        if (maps_dir / "events.json").is_file():
            if next_event >= 65535:
                die("no free zone_event member past %d" % next_event)
            _map_events[key] = next_event
            next_event += 1
        if (maps_dir / "scripts.s").is_file():
            if next_script >= 65535:
                die("no free scr_seq member past %d" % next_script)
            _map_scripts[key] = next_script
            next_script += 1
        if (maps_dir / "scripts.s").is_file() or (maps_dir / "init.s").is_file():
            if next_script >= 65535:
                die("no free scr_seq member past %d" % next_script)
            _map_init[key] = next_script
            next_script += 1
        if (maps_dir / "text.json").is_file():
            if next_msg >= 65535:
                die("no free pl_msg member past %d" % next_msg)
            _map_msg[key] = next_msg
            next_msg += 1


def write_generated_tables(pkg: Path, dest_root: Path) -> None:
    """Stage appended generated/*.txt under .cooked/generated/.

    Does not write build/pc/geninclude. A rebuild copies these into
    $(BUILD)/modinclude only when --modinclude is set and a record
    actually needs a compiled #define.
    """
    assigned = _pkg_ids.get(pkg.name, {})
    if not assigned:
        return
    kinds_needed = set()
    new_by_kind: dict[str, list[tuple[str, int]]] = {}
    for key, num in assigned.items():
        if ":" not in key or "/" not in key.split(":", 1)[1]:
            continue
        kind, name = key.split(":", 1)[1].split("/", 1)
        vanilla_by_name = {n: i for i, n in vanilla_taken_for_kind(kind).items()}
        if name in vanilla_by_name:
            continue
        kinds_needed.add(kind)
        new_by_kind.setdefault(kind, []).append((name, num))
    if not kinds_needed:
        return

    gen = dest_root / "generated"
    gen.mkdir(parents=True, exist_ok=True)
    table = {
        "species": (ROOT / "generated" / "species.txt", "SPECIES_"),
        "gfx": (ROOT / "generated" / "object_events_gfx.txt", "OBJ_EVENT_GFX_"),
        "map": (ROOT / "generated" / "map_headers.txt", "MAP_HEADER_"),
        "item": (ROOT / "generated" / "items.txt", "ITEM_"),
    }
    for kind, rows in new_by_kind.items():
        if kind not in table:
            continue
        src, prefix = table[kind]
        body = src.read_text()
        extra = []
        for name, num in sorted(rows, key=lambda r: r[1]):
            extra.append("%s%s = %d\n" % (prefix, name.upper(), num))
        (gen / src.name).write_text(body + "".join(extra))


def print_rebuild_report(pkgs: list[Path], modinclude: Path | None) -> None:
    any_force = False
    for pkg in pkgs:
        why = _pkg_rebuild_why.get(pkg.name, [])
        assigned = _pkg_ids.get(pkg.name, {})
        species_new = []
        gfx_new = []
        prop_new = []
        map_new = []
        for key, num in assigned.items():
            if ":species/" in key and num > SPECIES_BAD_EGG:
                species_new.append((key, num))
            if ":gfx/" in key and key in _gfx_nsbtx:
                gfx_new.append((key, num, _gfx_nsbtx[key]))
            if ":prop/" in key:
                vanilla_prop = vanilla_taken_for_kind("prop")
                if key.split("/")[-1].lower() not in set(vanilla_prop.values()):
                    prop_new.append((key, num))
            if ":map/" in key and key in _map_land:
                map_new.append((key, num, _map_land[key],
                                _map_matrix.get(key, -1)))
        if why:
            any_force = True
            reason = "; ".join(why)
            if modinclude is not None:
                print("cook: '%s' forced a rebuild into %s: %s"
                      % (pkg.name, modinclude, reason))
            else:
                print("cook: '%s' would rebuild (%s); NARC-only cook "
                      "does not write build/pc/geninclude"
                      % (pkg.name, reason))
        narc_bits = ["%s -> %d" % (k, n) for k, n in species_new]
        narc_bits += ["%s -> %d (mmodel %d)" % (k, n, m)
                      for k, n, m in gfx_new]
        narc_bits += ["%s -> %d" % (k, n) for k, n in prop_new]
        narc_bits += ["%s -> %d (land %d matrix %d)" % (k, n, land, mx)
                      for k, n, land, mx in map_new]
        if narc_bits:
            print("cook: '%s' NARC append only (%s); no rebuild"
                  % (pkg.name, ", ".join(narc_bits)))
        if not why and not narc_bits and assigned:
            print("cook: '%s' records frozen, no rebuild" % pkg.name)
    if not any_force:
        return
    if modinclude is not None:
        staged = 0
        for pkg in pkgs:
            src = pkg / ".cooked" / "generated"
            if not src.is_dir():
                continue
            for path in src.iterdir():
                if path.is_file():
                    dest = modinclude / path.name
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, dest)
                    staged += 1
        if staged:
            print("cook: staged %d generated table(s) into %s"
                  % (staged, modinclude))


def find_charmap(rom: Path, needed_for: str) -> Path:
    # meson copies charmap next to the built msgenc; the source tree
    # has the same file. Prefer the ROM-build copy so --rom-build
    # points at one tree.
    candidates = (
        rom / "tools" / "msgenc" / "charmap.txt",
        ROOT / "tools" / "msgenc" / "charmap.txt",
    )
    for path in candidates:
        if path.is_file():
            return path
    die("charmap.txt not found (needed for %s); look in %s"
        % (needed_for, candidates[0].parent))
    raise AssertionError("unreachable")


def parse_narc_png(rel: str) -> tuple[str, str] | None:
    """content/narc/<nitro-path>/<idx>.png -> (nitro-path, idx) or None."""
    prefix = "content/narc/"
    suffix = ".png"
    if not rel.startswith(prefix) or not rel.endswith(suffix):
        return None
    rest = rel[len(prefix): -len(suffix)]
    slash = rest.rfind("/")
    if slash <= 0:
        return None
    nitro = rest[:slash]
    idx = rest[slash + 1:]
    if not idx.isdigit():
        return None
    return nitro, idx


def parse_pokemon_dir(rel: str) -> str | None:
    """content/pokemon/<species>/<file> -> species dir or None."""
    parts = rel.split("/")
    if (len(parts) == 4
            and parts[0] == "content"
            and parts[1] == "pokemon"
            and parts[2] != ""
            and parts[3] != ""):
        return parts[2]
    return None


def parse_pokemon_face(rel: str) -> str | None:
    """A face PNG is the recipe that cooks the whole species dir."""
    parts = rel.split("/")
    if parse_pokemon_dir(rel) is None:
        return None
    if any(parts[3] == name for name, _ in POKEGRA_FACES):
        return parts[2]
    return None


def is_pokemon_companion(rel: str) -> bool:
    """Sibling key / pal / decomp extra, not their own recipe."""
    parts = rel.split("/")
    if parse_pokemon_dir(rel) is None:
        return False
    return parts[3] in POKEGRA_COMPANIONS or parts[3] in POKEGRA_IGNORE


def parse_text_json(rel: str) -> str | None:
    """content/text/<bank>.json -> bank name or None."""
    parts = rel.split("/")
    if (len(parts) == 3
            and parts[0] == "content"
            and parts[1] == "text"
            and parts[2].endswith(".json")
            and parts[2] != ".json"):
        return parts[2][:-5]
    return None


def resolve_text_bank(name: str, rel: str) -> int:
    if name.isdigit():
        return int(name)
    ids = text_bank_ids()
    if name.lower() not in ids:
        die("unknown text bank '%s' (needed for %s); look in %s"
            % (name, rel, ROOT / "generated" / "text_banks.txt"))
    return ids[name.lower()]


def cook_narc_png(src: Path, rel: str, dest_root: Path, rom: Path) -> Path:
    parsed = parse_narc_png(rel)
    if parsed is None:
        die("no recipe for %s" % rel)
    nitro, idx = parsed
    nitrogfx = find_tool("nitrogfx", rom, rel)
    out_dir = dest_root / "narc" / nitro
    out_dir.mkdir(parents=True, exist_ok=True)
    # nitrogfx keys the encoder off the output extension. The overlay
    # member is the index with no suffix, so encode to a temp .NCGR
    # and publish the bytes under the index name.
    # -encodefronttoback -scan need a sibling .png.key (the pokemon
    # scramble). That argv belongs on the pokemon sprite recipe, not here.
    tmp = out_dir / (idx + ".NCGR")
    run_tool("nitrogfx",
             [str(nitrogfx), str(src), str(tmp)],
             rel)
    dest = out_dir / idx
    tmp.replace(dest)
    if dest.stat().st_size == 0:
        die("nitrogfx wrote an empty file for %s" % rel)
    return dest


def cook_pokemon_dir(src: Path, rel: str, dest_root: Path,
                     rom: Path) -> Path:
    name = parse_pokemon_dir(rel)
    if name is None:
        die("no recipe for %s" % rel)
    key = name.lower()
    if key in _cooked_pokemon:
        return dest_root / "narc" / POKEGRA_NITRO / "0"
    ids = species_ids()
    if key not in ids:
        die("unknown species '%s' (needed for %s); look in %s "
            "or add records/species/%s.json"
            % (name, rel, ROOT / "generated" / "species.txt", name))
    species = ids[key]
    poke_dir = src.parent
    new_species = species > SPECIES_BAD_EGG

    present = {p.name for p in poke_dir.iterdir() if p.is_file()}
    faces = [(fn, off) for fn, off in POKEGRA_FACES if fn in present]
    pals = [(fn, off) for fn, off in POKEGRA_PALS if fn in present]
    if new_species:
        missing = []
        for fn, _ in POKEGRA_FACES:
            if fn not in present:
                missing.append(fn)
        for fn, _ in POKEGRA_PALS:
            if fn not in present:
                missing.append(fn)
        if missing:
            die("new species '%s' needs the decomp layout "
                "(front/back, both pals); missing %s"
                % (name, ", ".join(missing)))
    if not faces:
        die("no pokemon face PNG in content/pokemon/%s/ (needed for %s)"
            % (name, rel))
    if "normal.pal" not in present and not pals:
        die("normal.pal missing (needed for %s)" % rel)

    nitrogfx = find_tool("nitrogfx", rom, rel)
    out_dir = dest_root / "narc" / POKEGRA_NITRO
    out_dir.mkdir(parents=True, exist_ok=True)

    first: Path | None = None
    # Same argv make_pl_pokegra.py uses. The generic
    # content/narc/<path>/<idx>.png recipe must not gain these
    # flags: they open <png>.key and die without it.
    for fn, off in faces:
        png = poke_dir / fn
        face_rel = "content/pokemon/%s/%s" % (name, fn)
        key_path = poke_dir / (fn + ".key")
        if not key_path.is_file():
            die("%s missing (needed for %s)" % (key_path.name, face_rel))
        idx = species * 6 + off
        tmp_ncgr = out_dir / ("%d.NCGR" % idx)
        run_tool("nitrogfx",
                 [str(nitrogfx), str(png), str(tmp_ncgr),
                  "-encodefronttoback", "-scan"],
                 face_rel)
        dest = out_dir / str(idx)
        tmp_ncgr.replace(dest)
        if dest.stat().st_size == 0:
            die("nitrogfx wrote an empty file for %s" % face_rel)
        if first is None:
            first = dest

    for fn, off in pals:
        pal = poke_dir / fn
        pal_rel = "content/pokemon/%s/%s" % (name, fn)
        idx = species * 6 + off
        tmp_nclr = out_dir / ("%d.NCLR" % idx)
        run_tool("nitrogfx",
                 [str(nitrogfx), str(pal), str(tmp_nclr),
                  "-bitdepth", "8", "-nopad", "-comp", "10"],
                 pal_rel)
        dest = out_dir / str(idx)
        tmp_nclr.replace(dest)
        if dest.stat().st_size == 0:
            die("nitrogfx wrote an empty file for %s" % pal_rel)
        if first is None:
            first = dest

    # Overlay refuses a hole. Vanilla FAT is 2964 (species 0..493).
    # A new species at N writes N*6 .. N*6+5 and fills 2964 .. N*6-1
    # the way meson `touch`es a missing face: an empty member.
    if new_species:
        base = species * 6
        for i in range(POKEGRA_VANILLA_COUNT, base + 6):
            hole = out_dir / str(i)
            if not hole.exists():
                hole.write_bytes(b"")

    _cooked_pokemon.add(key)
    if first is None:
        die("wrote nothing for content/pokemon/%s/" % name)
    return first


def parse_people_png(rel: str) -> str | None:
    """content/people/<name>.png -> name or None."""
    parts = rel.split("/")
    if (len(parts) == 3
            and parts[0] == "content"
            and parts[1] == "people"
            and parts[2].endswith(".png")
            and parts[2] != ".png"):
        return parts[2][:-4]
    return None


def parse_events_json(rel: str) -> str | None:
    """content/events/<map>.json -> stem or None."""
    parts = rel.split("/")
    if (len(parts) == 3
            and parts[0] == "content"
            and parts[1] == "events"
            and parts[2].endswith(".json")
            and parts[2] != ".json"):
        return parts[2][:-5]
    return None


def find_gfx_record(name: str) -> tuple[str, int] | None:
    suffix = ":gfx/" + name.lower()
    for assigned in _pkg_ids.values():
        for key, num in assigned.items():
            if key.endswith(suffix):
                return key, num
    return None


def cook_people_png(src: Path, rel: str, dest_root: Path, rom: Path) -> Path:
    name = parse_people_png(rel)
    if name is None:
        die("no recipe for %s" % rel)
    if name.lower() in _cooked_people:
        return dest_root / "narc" / MMODEL_NITRO / "0"
    found = find_gfx_record(name)
    if found is None:
        die("unknown gfx '%s' (needed for %s); add records/gfx/%s.json"
            % (name, rel, name))
    key, gfx_id = found
    vanilla = vanilla_taken_for_kind("gfx")
    if gfx_id in vanilla:
        die("%s is vanilla gfx %d; this recipe is for a new "
            "OBJ_EVENT_GFX (records/gfx/%s.json with a new name)"
            % (rel, gfx_id, name))
    member = _gfx_nsbtx.get(key)
    if member is None:
        die("no mmodel member for %s" % key)
    nitrobtx = find_tool("nitrobtx", rom, rel)
    out_dir = dest_root / "narc" / MMODEL_NITRO
    out_dir.mkdir(parents=True, exist_ok=True)
    tmp = out_dir / ("%d.nsbtx" % member)
    run_tool("nitrobtx",
             [str(nitrobtx), "pack",
              "--texture", str(src),
              "--name", name,
              "--spritesheet",
              "--frame-height", "32",
              "--extract-palette",
              str(tmp)],
             rel)
    dest = out_dir / str(member)
    tmp.replace(dest)
    if dest.stat().st_size == 0:
        die("nitrobtx wrote an empty file for %s" % rel)
    _cooked_people.add(name.lower())
    return dest


def resolve_events_graphics(data: object, rel: str) -> None:
    """Rewrite allocated OBJ_EVENT_GFX_* names to the frozen integer."""
    if not isinstance(data, dict):
        return
    objects = data.get("object_events")
    if not isinstance(objects, list):
        return
    for obj in objects:
        if not isinstance(obj, dict):
            continue
        gid = obj.get("graphics_id")
        if isinstance(gid, int):
            continue
        if not isinstance(gid, str):
            die("%s object graphics_id must be a name or integer" % rel)
        if not gid.startswith("OBJ_EVENT_GFX_"):
            continue
        rest = gid[len("OBJ_EVENT_GFX_"):].lower()
        found = find_gfx_record(rest)
        if found is None:
            continue
        _key, num = found
        obj["graphics_id"] = num


def resolve_events_map_headers(data: object, rel: str) -> None:
    """Rewrite allocated MAP_HEADER_* dests to the frozen integer."""
    if not isinstance(data, dict):
        return
    warps = data.get("warp_events")
    if not isinstance(warps, list):
        return
    for warp in warps:
        if not isinstance(warp, dict):
            continue
        dest = warp.get("dest_header_id")
        if isinstance(dest, int):
            continue
        if not isinstance(dest, str):
            die("%s dest_header_id must be a name or integer" % rel)
        if not dest.startswith("MAP_HEADER_"):
            continue
        rest = dest[len("MAP_HEADER_"):].lower()
        found = find_map_record(rest)
        if found is None:
            continue
        _key, num = found
        warp["dest_header_id"] = num


def run_eventpy(data: dict, dest: Path, stem: str, rel: str,
                dest_root: Path, rom: Path) -> None:
    resolve_events_graphics(data, rel)
    resolve_events_map_headers(data, rel)
    eventpy = find_tool("eventpy", rom, rel)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp_json = dest_root / (".events-%s.json" % stem)
    tmp_json.write_text(json.dumps(data, indent=2) + "\n")
    env = os.environ.copy()
    # event.py does `from convert import` (same dir) and
    # convert.py does `from generated import` (ROM meson root).
    env["PYTHONPATH"] = os.pathsep.join(
        [str(ROOT / "tools" / "jsoncnv"), str(rom)]
        + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else [])
    )
    run_tool("event.py",
             [sys.executable, str(eventpy), str(tmp_json), str(dest)],
             rel, env=env)
    if tmp_json.is_file():
        tmp_json.unlink()
    if not dest.is_file() or dest.stat().st_size == 0:
        die("event.py wrote an empty file for %s" % rel)
    if dest.stat().st_size >= 2048:
        die("%s cooked to %d bytes; zone_event tmp buffer is 2048"
            % (rel, dest.stat().st_size))


def cook_events_json(src: Path, rel: str, dest_root: Path, rom: Path) -> Path:
    stem = parse_events_json(rel)
    if stem is None:
        die("no recipe for %s" % rel)
    ids = zone_event_ids()
    if stem not in ids:
        die("unknown events map '%s' (needed for %s); look in %s"
            % (stem, rel, ROOT / "res" / "field" / "events" / "zone_event.order"))
    idx = ids[stem]
    try:
        data = json.loads(src.read_text())
    except json.JSONDecodeError as e:
        die("invalid JSON in %s: %s" % (rel, e))
    if not isinstance(data, dict):
        die("%s must be a JSON object" % rel)
    dest = dest_root / "narc" / ZONE_EVENT_NITRO / str(idx)
    run_eventpy(data, dest, stem, rel, dest_root, rom)
    return dest


def write_billboard_table(pkg: Path, dest_root: Path) -> None:
    """gfx-id nsbtx-member rows the runtime hook reads."""
    assigned = _pkg_ids.get(pkg.name, {})
    kinds = _pkg_kinds.get(pkg.name, {})
    rows = []
    for key, num in assigned.items():
        if kinds.get(key) != "gfx":
            continue
        member = _gfx_nsbtx.get(key)
        if member is None:
            continue
        if key.split("/")[-1].lower() not in _cooked_people:
            continue
        rows.append((num, member))
    if not rows:
        return
    gen = dest_root / "generated"
    gen.mkdir(parents=True, exist_ok=True)
    lines = ["%d %d\n" % (gfx, member) for gfx, member in sorted(rows)]
    (gen / "billboard_gfx.txt").write_text("".join(lines))


def cook_text_json(src: Path, rel: str, dest_root: Path, rom: Path) -> Path:
    name = parse_text_json(rel)
    if name is None:
        die("no recipe for %s" % rel)
    idx = resolve_text_bank(name, rel)
    msgenc = find_tool("msgenc", rom, rel)
    charmap = find_charmap(rom, rel)
    out_dir = dest_root / "narc" / PL_MSG_NITRO
    out_dir.mkdir(parents=True, exist_ok=True)
    dest = out_dir / str(idx)
    # meson: msgenc -e --json -c charmap.txt -H bank/<name>.h in out
    header = dest_root / (".msgenc-%s.h" % name)
    run_tool("msgenc",
             [str(msgenc), "-e", "--json", "-c", str(charmap),
              "-H", str(header), str(src), str(dest)],
             rel)
    if header.is_file():
        header.unlink()
    if not dest.is_file() or dest.stat().st_size == 0:
        die("msgenc wrote an empty file for %s" % rel)
    return dest


def parse_props_nsbmd(rel: str) -> str | None:
    """content/props/<name>.nsbmd -> name or None."""
    parts = rel.split("/")
    if (len(parts) == 3
            and parts[0] == "content"
            and parts[1] == "props"
            and parts[2].endswith(".nsbmd")
            and parts[2] != ".nsbmd"):
        return parts[2][:-6]
    return None


def parse_props_gltf(rel: str) -> str | None:
    """content/props/<name>.gltf or .glb -> name or None."""
    parts = rel.split("/")
    if (len(parts) != 3
            or parts[0] != "content"
            or parts[1] != "props"
            or parts[2] in (".gltf", ".glb")):
        return None
    if parts[2].endswith(".gltf"):
        return parts[2][:-5]
    if parts[2].endswith(".glb"):
        return parts[2][:-4]
    return None


def is_prop_gltf_bin(rel: str, src: Path) -> bool:
    """content/props/<name>.bin next to <name>.gltf is the buffer."""
    parts = rel.split("/")
    if (len(parts) != 3
            or parts[0] != "content"
            or parts[1] != "props"
            or not parts[2].endswith(".bin")
            or parts[2] == ".bin"):
        return False
    return src.with_suffix(".gltf").is_file()


def parse_maps_model(rel: str) -> str | None:
    """content/maps/<name>/model.gltf or model.glb -> name or None."""
    parts = rel.split("/")
    if (len(parts) != 4
            or parts[0] != "content"
            or parts[1] != "maps"
            or parts[2] == ""
            or parts[3] not in ("model.gltf", "model.glb")):
        return None
    return parts[2]


def is_map_model_bin(rel: str, src: Path) -> bool:
    """content/maps/<name>/model.bin next to model.gltf is the buffer."""
    parts = rel.split("/")
    if (len(parts) != 4
            or parts[0] != "content"
            or parts[1] != "maps"
            or parts[3] != "model.bin"):
        return False
    return (src.parent / "model.gltf").is_file()


def parse_maps_land_data(rel: str) -> str | None:
    """content/maps/<name>/land_data.bin -> name or None."""
    parts = rel.split("/")
    if (len(parts) == 4
            and parts[0] == "content"
            and parts[1] == "maps"
            and parts[2] != ""
            and parts[3] == "land_data.bin"):
        return parts[2]
    return None


def parse_maps_sibling(rel: str, filename: str) -> str | None:
    """content/maps/<name>/<filename> -> name or None."""
    parts = rel.split("/")
    if (len(parts) == 4
            and parts[0] == "content"
            and parts[1] == "maps"
            and parts[2] != ""
            and parts[3] == filename):
        return parts[2]
    return None


def find_prop_record(name: str) -> tuple[str, int] | None:
    suffix = ":prop/" + name.lower()
    for assigned in _pkg_ids.values():
        for key, num in assigned.items():
            if key.endswith(suffix):
                return key, num
    return None


def find_map_record(name: str) -> tuple[str, int] | None:
    suffix = ":map/" + name.lower()
    for assigned in _pkg_ids.values():
        for key, num in assigned.items():
            if key.endswith(suffix):
                return key, num
    return None


def _prop_dest(name: str, rel: str, dest_root: Path) -> tuple[int, Path]:
    if name.lower() in _cooked_props:
        return -1, dest_root / "narc" / BUILD_MODEL_NITRO / "0"
    found = find_prop_record(name)
    if found is None:
        die("unknown prop '%s' (needed for %s); add records/props/%s.json"
            % (name, rel, name))
    _key, prop_id = found
    vanilla = vanilla_taken_for_kind("prop")
    if prop_id in vanilla:
        die("%s is vanilla prop %d; this recipe is for a new "
            "prop (records/props/%s.json with a new name)"
            % (rel, prop_id, name))
    if prop_id >= MAX_MAP_PROP_MODEL_FILES:
        die("prop %d is past AreaDataManager's %d-slot table (%s)"
            % (prop_id, MAX_MAP_PROP_MODEL_FILES, rel))
    out_dir = dest_root / "narc" / BUILD_MODEL_NITRO
    out_dir.mkdir(parents=True, exist_ok=True)
    return prop_id, out_dir / str(prop_id)


def cook_props_nsbmd(src: Path, rel: str, dest_root: Path,
                     rom: Path) -> Path:
    name = parse_props_nsbmd(rel)
    if name is None:
        die("no recipe for %s" % rel)
    prop_id, dest = _prop_dest(name, rel, dest_root)
    if prop_id < 0:
        return dest
    for ext in (".gltf", ".glb"):
        other = src.with_name(name + ext)
        if other.is_file():
            die("%s and %s both exist; pick one source" % (rel, other.name))
    blob = src.read_bytes()
    if len(blob) < 8 or blob[:4] != b"BMD0":
        die("%s is not an NSBMD (missing BMD0)" % rel)
    if b"MDL0" not in blob:
        die("%s has no MDL0 (the field draws a mesh, not TEX0 alone)"
            % rel)
    dest.write_bytes(blob)
    _cooked_props.add(name.lower())
    return dest


def cook_props_gltf(src: Path, rel: str, dest_root: Path,
                    rom: Path) -> Path:
    name = parse_props_gltf(rel)
    if name is None:
        die("no recipe for %s" % rel)
    for ext in (".nsbmd", ".gltf", ".glb"):
        other = src.with_name(name + ext)
        if other.resolve() != src.resolve() and other.is_file():
            die("%s and %s both exist; pick one source" % (rel, other.name))
    prop_id, dest = _prop_dest(name, rel, dest_root)
    if prop_id < 0:
        return dest
    nitromdl = find_tool("nitromdl", rom, rel)
    tmp = dest.with_suffix(".nsbmd.tmp")
    run_tool("nitromdl", [str(nitromdl), str(src), str(tmp)], rel)
    blob = tmp.read_bytes() if tmp.is_file() else b""
    if tmp.is_file():
        tmp.unlink()
    if len(blob) < 8 or blob[:4] != b"BMD0":
        die("nitromdl wrote no NSBMD for %s" % rel)
    if b"MDL0" not in blob:
        die("nitromdl wrote no MDL0 for %s" % rel)
    dest.write_bytes(blob)
    _cooked_props.add(name.lower())
    return dest


def resolve_land_data_member(name: str, rel: str) -> tuple[int, str | None]:
    """Return (member, new-map record key or None)."""
    found = find_map_record(name)
    if found is not None:
        key, _num = found
        member = _map_land.get(key)
        if member is None:
            die("no land_data member for %s" % key)
        return member, key
    if name.isdigit():
        return int(name), None
    if name.startswith("map_data_"):
        rest = name[len("map_data_"):]
        if rest.isdigit():
            return int(rest), None
    die("unknown map '%s' (needed for %s); add records/maps/%s.json "
        "or use a land_data index / map_data_NNN"
        % (name, rel, name))
    raise AssertionError("unreachable")


def pack_matrix_1x1(land_id: int) -> bytes:
    """Same layout jsoncnv/map_matrix.py writes for a 1x1 indoor."""
    name = b"m"
    return bytes([1, 1, 0, 0, len(name)]) + name + struct.pack("<H", land_id)


def cook_maps_land_data(src: Path, rel: str, dest_root: Path,
                        rom: Path) -> Path:
    name = parse_maps_land_data(rel)
    if name is None:
        die("no recipe for %s" % rel)
    if name.lower() in _cooked_maps:
        return dest_root / "narc" / LAND_DATA_NITRO / "0"
    member, map_key = resolve_land_data_member(name, rel)
    if member >= 65535:
        die("land_data member %d will not fit in a NARC FAT (%s)"
            % (member, rel))
    if (src.parent / "terrain.json").is_file():
        die("%s and terrain.json both exist; pick one source" % rel)
    blob = src.read_bytes()
    if len(blob) < 16:
        die("%s is too short to be a land_data member" % rel)
    attr_sz, props_sz, model_sz, bdhc_sz = struct.unpack_from("<4I", blob, 0)
    if attr_sz != 2048:
        die("%s terrainAttributesSize is %d, not 2048" % (rel, attr_sz))
    need = 16 + attr_sz + props_sz + model_sz + bdhc_sz
    if len(blob) < need:
        die("%s is %d bytes, header says %d" % (rel, len(blob), need))
    return write_land_data_member(blob, name, member, map_key, dest_root)


def to_fx32(value: float) -> int:
    return int(round(float(value) * FX32_ONE))


def intern_tuple(table: list, item: tuple) -> int:
    if item in table:
        return table.index(item)
    table.append(item)
    return len(table) - 1


def pack_terrain_attributes(data: dict, rel: str) -> bytes:
    tiles = [0] * TERRAIN_ATTR_COUNT
    raw = data.get("attributes")
    if raw is None:
        pass
    elif isinstance(raw, int):
        tiles = [raw & 0xFFFF] * TERRAIN_ATTR_COUNT
    elif isinstance(raw, list) and raw and isinstance(raw[0], list):
        if len(raw) != 32:
            die("%s attributes must be 32 rows, not %d" % (rel, len(raw)))
        for z, row in enumerate(raw):
            if not isinstance(row, list) or len(row) != 32:
                die("%s attributes row %d must be 32 values" % (rel, z))
            for x, v in enumerate(row):
                tiles[z * 32 + x] = int(v) & 0xFFFF
    elif isinstance(raw, list):
        if len(raw) != TERRAIN_ATTR_COUNT:
            die("%s attributes must be 1024 values, not %d"
                % (rel, len(raw)))
        tiles = [int(v) & 0xFFFF for v in raw]
    else:
        die("%s attributes must be an integer, a 32x32 grid, or 1024 values"
            % rel)

    rows = data.get("tiles")
    if rows is not None:
        if not isinstance(rows, list) or len(rows) != 32:
            die("%s tiles must be 32 strings of 32 chars" % rel)
        for z, row in enumerate(rows):
            if not isinstance(row, str) or len(row) != 32:
                die("%s tiles row %d must be 32 chars" % (rel, z))
            for x, ch in enumerate(row):
                if ch == "#":
                    tiles[z * 32 + x] = 0x8000
                elif ch == ".":
                    tiles[z * 32 + x] = 0
                else:
                    die("%s tiles uses '%s'; only '.' and '#' "
                        "are packed" % (rel, ch))
    return struct.pack("<%dH" % TERRAIN_ATTR_COUNT, *tiles)


def pack_map_props(data: dict, rel: str) -> bytes:
    props = data.get("props") or []
    if not isinstance(props, list):
        die("%s props must be a JSON array" % rel)
    if len(props) > MAX_MAP_PROPS:
        die("%s has %d props; a land_data member holds at most %d"
            % (rel, len(props), MAX_MAP_PROPS))
    out = bytearray()
    for i, prop in enumerate(props):
        if not isinstance(prop, dict):
            die("%s props[%d] must be a JSON object" % (rel, i))
        model = prop.get("model", prop.get("id"))
        if model is None:
            die("%s props[%d] needs a model" % (rel, i))
        if isinstance(model, str):
            found = find_prop_record(model)
            if found is None:
                die("%s props[%d] unknown prop '%s'; add "
                    "records/props/%s.json" % (rel, i, model, model))
            model_id = found[1]
        elif isinstance(model, int):
            model_id = model
        else:
            die("%s props[%d] model must be a name or an integer"
                % (rel, i))
        scale = prop.get("scale", 1)
        sx = prop.get("sx", scale)
        sy = prop.get("sy", scale)
        sz = prop.get("sz", scale)
        out += struct.pack(
            "<i3i3i3i2I",
            int(model_id),
            to_fx32(prop.get("x", 0)),
            to_fx32(prop.get("y", 0)),
            to_fx32(prop.get("z", 0)),
            int(prop.get("rx", 0)),
            int(prop.get("ry", 0)),
            int(prop.get("rz", 0)),
            to_fx32(sx),
            to_fx32(sy),
            to_fx32(sz),
            0, 0)
    return bytes(out)


def pack_bdhc(data: dict, rel: str) -> bytes:
    """One or more flat plates. Layout measured from map_data_001."""
    spec = data.get("bdhc")
    if spec is None:
        spec = {"plates": [{
            "x0": -256, "z0": -256, "x1": 256, "z1": 256, "y": 0,
        }]}
    if not isinstance(spec, dict):
        die("%s bdhc must be a JSON object" % rel)
    plates_in = spec.get("plates")
    if not isinstance(plates_in, list) or not plates_in:
        die("%s bdhc.plates must be a non-empty array" % rel)

    points: list[tuple[int, int]] = []
    normals: list[tuple[int, int, int]] = []
    constants: list[int] = []
    plates: list[tuple[int, int, int, int]] = []
    for i, plate in enumerate(plates_in):
        if not isinstance(plate, dict):
            die("%s bdhc.plates[%d] must be a JSON object" % (rel, i))
        try:
            x0 = float(plate["x0"])
            z0 = float(plate["z0"])
            x1 = float(plate["x1"])
            z1 = float(plate["z1"])
        except (KeyError, TypeError, ValueError):
            die("%s bdhc.plates[%d] needs x0,z0,x1,z1" % (rel, i))
        y0 = plate.get("y0", plate.get("y", 0))
        y1 = plate.get("y1", y0)
        try:
            y0f = float(y0)
            y1f = float(y1)
        except (TypeError, ValueError):
            die("%s bdhc.plates[%d] y is not a number" % (rel, i))
        if y0f != y1f:
            die("%s bdhc.plates[%d] is sloped (y0 != y1); "
                "flat plates only (no measured slope oracle)"
                % (rel, i))
        p0 = intern_tuple(points, (to_fx32(x0), to_fx32(z0)))
        p1 = intern_tuple(points, (to_fx32(x1), to_fx32(z1)))
        n = intern_tuple(normals, (0, FX32_ONE, 0))
        c = intern_tuple(constants, to_fx32(-y0f))
        plates.append((p0, p1, n, c))

    zs = sorted(set(z for _x, z in points))
    if len(zs) < 2:
        die("%s bdhc plates need two distinct Z values "
            "(a strip is every Z except the minimum)" % rel)
    strips_out = []
    access: list[int] = []
    for scan in zs[1:]:
        hit = []
        for idx, (p0, p1, _n, _c) in enumerate(plates):
            zmin = min(points[p0][1], points[p1][1])
            zmax = max(points[p0][1], points[p1][1])
            if zmin <= scan <= zmax:
                hit.append(idx)
        if not hit:
            continue
        start = len(access)
        access.extend(hit)
        strips_out.append((scan, len(hit), start))

    if not strips_out:
        die("%s bdhc produced no strips" % rel)
    if len(points) > 65535 or len(normals) > 65535 \
            or len(constants) > 65535 or len(plates) > 65535 \
            or len(strips_out) > 65535 or len(access) > 65535:
        die("%s bdhc is past a u16 count" % rel)

    out = bytearray(b"BDHC")
    out += struct.pack("<6H",
                       len(points), len(normals), len(constants),
                       len(plates), len(strips_out), len(access))
    for x, z in points:
        out += struct.pack("<ii", x, z)
    for x, y, z in normals:
        out += struct.pack("<iii", x, y, z)
    for c in constants:
        out += struct.pack("<i", c)
    for p0, p1, n, c in plates:
        out += struct.pack("<4H", p0, p1, n, c)
    for scan, count, start in strips_out:
        out += struct.pack("<iHH", scan, count, start)
    if access:
        out += struct.pack("<%dH" % len(access), *access)
    if len(out) > BDHC_BUFFER_SIZE:
        die("%s BDHC is %d bytes; loaded-map buffer is %d"
            % (rel, len(out), BDHC_BUFFER_SIZE))
    return bytes(out)


def find_map_model(src_dir: Path, rel: str) -> Path:
    gltf = src_dir / "model.gltf"
    glb = src_dir / "model.glb"
    if gltf.is_file() and glb.is_file():
        die("%s has both model.gltf and model.glb; pick one" % rel)
    if gltf.is_file():
        return gltf
    if glb.is_file():
        return glb
    die("%s needs a sibling model.gltf (or model.glb)" % rel)
    raise AssertionError("unreachable")


def cook_map_model(src: Path, rel: str, dest: Path, rom: Path) -> bytes:
    nitromdl = find_tool("nitromdl", rom, rel)
    tmp = dest.with_suffix(".nsbmd.tmp")
    run_tool("nitromdl", [str(nitromdl), str(src), str(tmp)], rel)
    blob = tmp.read_bytes() if tmp.is_file() else b""
    if tmp.is_file():
        tmp.unlink()
    if len(blob) < 8 or blob[:4] != b"BMD0":
        die("nitromdl wrote no NSBMD for %s" % rel)
    if b"MDL0" not in blob:
        die("nitromdl wrote no MDL0 for %s" % rel)
    if len(blob) > MAP_MODEL_FILE_SIZE:
        die("map model is %d bytes; loaded-map buffer is %d (%s)"
            % (len(blob), MAP_MODEL_FILE_SIZE, rel))
    return blob


def write_land_data_member(blob: bytes, name: str, member: int,
                           map_key: str | None, dest_root: Path) -> Path:
    out_dir = dest_root / "narc" / LAND_DATA_NITRO
    out_dir.mkdir(parents=True, exist_ok=True)
    dest = out_dir / str(member)
    dest.write_bytes(blob)
    if map_key is not None:
        matrix_id = _map_matrix.get(map_key)
        if matrix_id is None:
            die("no map_matrix member for %s" % map_key)
        mx_dir = dest_root / "narc" / MAP_MATRIX_NITRO
        mx_dir.mkdir(parents=True, exist_ok=True)
        (mx_dir / str(matrix_id)).write_bytes(pack_matrix_1x1(member))
    _cooked_maps.add(name.lower())
    return dest


def cook_maps_terrain(src: Path, rel: str, dest_root: Path,
                      rom: Path) -> Path:
    name = parse_maps_sibling(rel, "terrain.json")
    if name is None:
        die("no recipe for %s" % rel)
    if name.lower() in _cooked_maps:
        return dest_root / "narc" / LAND_DATA_NITRO / "0"
    if (src.parent / "land_data.bin").is_file():
        die("%s and land_data.bin both exist; pick one source" % rel)
    member, map_key = resolve_land_data_member(name, rel)
    if member >= 65535:
        die("land_data member %d will not fit in a NARC FAT (%s)"
            % (member, rel))
    try:
        data = json.loads(src.read_text())
    except json.JSONDecodeError as e:
        die("invalid JSON in %s: %s" % (rel, e))
    if not isinstance(data, dict):
        die("%s must be a JSON object" % rel)

    attrs = pack_terrain_attributes(data, rel)
    props = pack_map_props(data, rel)
    bdhc = pack_bdhc(data, rel)
    model_src = find_map_model(src.parent, rel)
    out_dir = dest_root / "narc" / LAND_DATA_NITRO
    out_dir.mkdir(parents=True, exist_ok=True)
    dest = out_dir / str(member)
    model = cook_map_model(model_src, rel, dest, rom)
    blob = (struct.pack("<4I", 2048, len(props), len(model), len(bdhc))
            + attrs + props + model + bdhc)
    while len(blob) % 4:
        blob += b"\xff"
    return write_land_data_member(blob, name, member, map_key, dest_root)


def require_map_record(name: str, rel: str) -> str:
    found = find_map_record(name)
    if found is None:
        die("unknown map '%s' (needed for %s); add records/maps/%s.json"
            % (name, rel, name))
    return found[0]


def cook_maps_events(src: Path, rel: str, dest_root: Path,
                     rom: Path) -> Path:
    name = parse_maps_sibling(rel, "events.json")
    if name is None:
        die("no recipe for %s" % rel)
    key = require_map_record(name, rel)
    idx = _map_events.get(key)
    if idx is None:
        die("no zone_event member for %s" % key)
    try:
        data = json.loads(src.read_text())
    except json.JSONDecodeError as e:
        die("invalid JSON in %s: %s" % (rel, e))
    if not isinstance(data, dict):
        die("%s must be a JSON object" % rel)
    dest = dest_root / "narc" / ZONE_EVENT_NITRO / str(idx)
    run_eventpy(data, dest, name, rel, dest_root, rom)
    return dest


def find_assembler(rel: str) -> tuple[str, str]:
    gcc = shutil.which("arm-none-eabi-gcc")
    objcopy = shutil.which("arm-none-eabi-objcopy")
    if not gcc or not objcopy:
        die("arm-none-eabi-gcc/objcopy not found (needed for %s)" % rel)
    return gcc, objcopy


def assemble_script(src: Path, dest: Path, rel: str, rom: Path) -> None:
    scriptasm = find_tool("scriptasm", rom, rel)
    enumproc = rom / "tools" / "enumproc" / "enumproc"
    if not enumproc.is_file():
        die("enumproc not found (needed for %s); look in %s"
            % (rel, enumproc.parent))
    gcc, objcopy = find_assembler(rel)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp_dir = dest.parent / (".scr-" + dest.name)
    if tmp_dir.exists():
        shutil.rmtree(tmp_dir)
    tmp_dir.mkdir()
    try:
        run_tool("scriptasm",
                 [str(scriptasm),
                  "-i", str(ROOT / "include"),
                  "-i", str(ROOT / "asm"),
                  "-i", str(rom),
                  "--enumproc", str(enumproc),
                  "--assembler", gcc,
                  "--objcopy", objcopy,
                  "-d", str(tmp_dir),
                  str(src)],
                 rel)
        produced = tmp_dir / src.stem
        if not produced.is_file() or produced.stat().st_size == 0:
            die("script assembler wrote nothing for %s" % rel)
        dest.write_bytes(produced.read_bytes())
    finally:
        if tmp_dir.exists():
            shutil.rmtree(tmp_dir)


def cook_maps_scripts(src: Path, rel: str, dest_root: Path,
                      rom: Path) -> Path:
    name = parse_maps_sibling(rel, "scripts.s")
    if name is None:
        die("no recipe for %s" % rel)
    key = require_map_record(name, rel)
    idx = _map_scripts.get(key)
    if idx is None:
        die("no scr_seq member for %s" % key)
    dest = dest_root / "narc" / SCR_SEQ_NITRO / str(idx)
    assemble_script(src, dest, rel, rom)
    return dest


def cook_maps_init(src: Path, rel: str, dest_root: Path,
                   rom: Path) -> Path:
    name = parse_maps_sibling(rel, "init.s")
    if name is None:
        die("no recipe for %s" % rel)
    key = require_map_record(name, rel)
    idx = _map_init.get(key)
    if idx is None:
        die("no init scr_seq member for %s" % key)
    dest = dest_root / "narc" / SCR_SEQ_NITRO / str(idx)
    assemble_script(src, dest, rel, rom)
    if dest.stat().st_size >= 256:
        die("%s cooked to %d bytes; initScripts buffer is 256"
            % (rel, dest.stat().st_size))
    return dest


def write_default_init(key: str, dest_root: Path, rom: Path) -> None:
    idx = _map_init.get(key)
    if idx is None:
        return
    dest = dest_root / "narc" / SCR_SEQ_NITRO / str(idx)
    if dest.is_file():
        return
    tmp = dest_root / (".init-%s.s" % key.replace(":", "_").replace("/", "_"))
    tmp.write_text(DEFAULT_INIT_SCRIPT)
    try:
        assemble_script(tmp, dest, "content/maps/%s/init.s"
                        % key.split("/")[-1], rom)
    finally:
        if tmp.is_file():
            tmp.unlink()
    if dest.stat().st_size >= 256:
        die("generated init for %s is %d bytes; initScripts buffer is 256"
            % (key, dest.stat().st_size))


def cook_maps_text(src: Path, rel: str, dest_root: Path,
                   rom: Path) -> Path:
    name = parse_maps_sibling(rel, "text.json")
    if name is None:
        die("no recipe for %s" % rel)
    key = require_map_record(name, rel)
    idx = _map_msg.get(key)
    if idx is None:
        die("no pl_msg member for %s" % key)
    msgenc = find_tool("msgenc", rom, rel)
    charmap = find_charmap(rom, rel)
    out_dir = dest_root / "narc" / PL_MSG_NITRO
    out_dir.mkdir(parents=True, exist_ok=True)
    dest = out_dir / str(idx)
    header = dest_root / (".msgenc-map-%s.h" % name)
    run_tool("msgenc",
             [str(msgenc), "-e", "--json", "-c", str(charmap),
              "-H", str(header), str(src), str(dest)],
             rel)
    if header.is_file():
        header.unlink()
    if not dest.is_file() or dest.stat().st_size == 0:
        die("msgenc wrote an empty file for %s" % rel)
    return dest


def write_extra_props_table(pkg: Path, dest_root: Path) -> None:
    """prop-id rows the area loader appends on top of area_build."""
    assigned = _pkg_ids.get(pkg.name, {})
    kinds = _pkg_kinds.get(pkg.name, {})
    rows = []
    for key, num in assigned.items():
        if kinds.get(key) != "prop":
            continue
        if key.split("/")[-1].lower() not in _cooked_props:
            continue
        rows.append(num)
    if not rows:
        return
    gen = dest_root / "generated"
    gen.mkdir(parents=True, exist_ok=True)
    lines = ["%d\n" % n for n in sorted(set(rows))]
    (gen / "extra_props.txt").write_text("".join(lines))


def write_cooked_maps_table(pkg: Path, dest_root: Path) -> None:
    """id + MapHeader fields the runtime hook reads."""
    assigned = _pkg_ids.get(pkg.name, {})
    kinds = _pkg_kinds.get(pkg.name, {})
    rows = []
    for key, num in assigned.items():
        if kinds.get(key) != "map":
            continue
        if key not in _map_land or key.split("/")[-1].lower() not in _cooked_maps:
            continue
        matrix_id = _map_matrix.get(key)
        if matrix_id is None:
            continue
        fields = list(NEW_MAP_HEADER_FIELDS)
        fields[2] = matrix_id
        if key in _map_scripts:
            fields[3] = _map_scripts[key]
        if key in _map_init:
            fields[4] = _map_init[key]
        if key in _map_msg:
            fields[5] = _map_msg[key]
        if key in _map_events:
            fields[9] = _map_events[key]
        for name, idx in MAP_HEADER_OVERRIDE.items():
            if name in _map_header_overrides.get(key, {}):
                fields[idx] = _map_header_overrides[key][name]
        rows.append((num, fields))
    if not rows:
        return
    gen = dest_root / "generated"
    gen.mkdir(parents=True, exist_ok=True)
    lines = ["# id area preloaded matrix scripts init msg day night "
             "wild events label window weather camera mapType battleBG "
             "bike run escape fly\n"]
    for num, fields in sorted(rows):
        lines.append("%d %s\n" % (num, " ".join(str(x) for x in fields)))
    (gen / "cooked_maps.txt").write_text("".join(lines))


def cook_one(rel: str, src: Path, dest_root: Path, rom: Path) -> Path | None:
    if parse_narc_png(rel) is not None:
        return cook_narc_png(src, rel, dest_root, rom)
    if parse_pokemon_face(rel) is not None:
        name = parse_pokemon_face(rel)
        assert name is not None
        if name.lower() in _cooked_pokemon:
            return None
        return cook_pokemon_dir(src, rel, dest_root, rom)
    if is_pokemon_companion(rel):
        return None
    if parse_pokemon_dir(rel) is not None:
        die("no recipe for %s" % rel)
    if parse_record_rel(rel) is not None:
        return None
    if parse_text_json(rel) is not None:
        return cook_text_json(src, rel, dest_root, rom)
    if parse_people_png(rel) is not None:
        name = parse_people_png(rel)
        assert name is not None
        if name.lower() in _cooked_people:
            return None
        return cook_people_png(src, rel, dest_root, rom)
    if parse_events_json(rel) is not None:
        return cook_events_json(src, rel, dest_root, rom)
    if parse_props_nsbmd(rel) is not None:
        name = parse_props_nsbmd(rel)
        assert name is not None
        if name.lower() in _cooked_props:
            return None
        return cook_props_nsbmd(src, rel, dest_root, rom)
    if parse_props_gltf(rel) is not None:
        name = parse_props_gltf(rel)
        assert name is not None
        if name.lower() in _cooked_props:
            return None
        return cook_props_gltf(src, rel, dest_root, rom)
    if is_prop_gltf_bin(rel, src):
        return None
    if parse_maps_land_data(rel) is not None:
        name = parse_maps_land_data(rel)
        assert name is not None
        if name.lower() in _cooked_maps:
            return None
        return cook_maps_land_data(src, rel, dest_root, rom)
    if parse_maps_sibling(rel, "terrain.json") is not None:
        name = parse_maps_sibling(rel, "terrain.json")
        assert name is not None
        if name.lower() in _cooked_maps:
            return None
        return cook_maps_terrain(src, rel, dest_root, rom)
    if parse_maps_model(rel) is not None:
        if not (src.parent / "terrain.json").is_file():
            die("%s needs a sibling terrain.json" % rel)
        return None
    if is_map_model_bin(rel, src):
        return None
    if parse_maps_sibling(rel, "events.json") is not None:
        return cook_maps_events(src, rel, dest_root, rom)
    if parse_maps_sibling(rel, "scripts.s") is not None:
        return cook_maps_scripts(src, rel, dest_root, rom)
    if parse_maps_sibling(rel, "init.s") is not None:
        return cook_maps_init(src, rel, dest_root, rom)
    if parse_maps_sibling(rel, "text.json") is not None:
        return cook_maps_text(src, rel, dest_root, rom)
    die("no recipe for %s" % rel)
    raise AssertionError("unreachable")


def cook_package(pkg: Path, rom: Path) -> int:
    global _cooked_pokemon, _cooked_people, _cooked_props, _cooked_maps
    _cooked_pokemon = set()
    _cooked_people = set()
    _cooked_props = set()
    _cooked_maps = set()
    files = input_files(pkg)
    assigned = _pkg_ids.get(pkg.name, {})
    if not files:
        content = pkg / "content"
        records = pkg / "records"
        if content.is_dir() or records.is_dir():
            if not assigned:
                die("wrote nothing in '%s'" % pkg.name)
        elif not assigned:
            return 0

    staging = pkg / ".cooked" / ".tmp"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True, exist_ok=True)

    wrote = 0
    try:
        for rel, src in files:
            if cook_one(rel, src, staging, rom) is not None:
                wrote += 1
        assigned_now = _pkg_ids.get(pkg.name, {})
        kinds_now = _pkg_kinds.get(pkg.name, {})
        for key, _num in assigned_now.items():
            if kinds_now.get(key) != "map":
                continue
            if key in _map_scripts and key in _map_init:
                write_default_init(key, staging, rom)
                wrote += 1
        write_generated_tables(pkg, staging)
        write_billboard_table(pkg, staging)
        write_extra_props_table(pkg, staging)
        write_cooked_maps_table(pkg, staging)
        if wrote == 0 and not assigned:
            die("wrote nothing in '%s'" % pkg.name)
        cooked = pkg / ".cooked"
        cooked.mkdir(parents=True, exist_ok=True)
        for name in ("fs", "narc", "generated"):
            dest = cooked / name
            src = staging / name
            if dest.exists():
                shutil.rmtree(dest)
            if src.exists():
                src.rename(dest)
        if assigned:
            write_ids_toml(cooked / "ids.toml", assigned)
        (cooked / "digest").write_text(digest_line(pkg) + "\n")
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return wrote + (1 if assigned else 0)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Cook runtime content packages into .cooked/.")
    ap.add_argument("--mods",
                    help="enabled packages (overrides PC_MODS / loadorder.txt)")
    ap.add_argument("--mods-dir",
                    help="package directory (default PC_MODS_DIR or pc/mods)")
    ap.add_argument("--rom-build",
                    help="ROM meson dir that holds the built tools "
                         "(default PC_ROM_BUILD or build/rom)")
    ap.add_argument("--print-digest", metavar="PKG",
                    help="print the input digest for PKG and exit")
    ap.add_argument("--modinclude",
                    help="stage appended generated tables here when a "
                         "record needs a compiled define. Never "
                         "build/pc/geninclude.")
    args = ap.parse_args(argv)

    if args.print_digest:
        pkg = Path(args.print_digest)
        if not pkg.is_dir():
            die("not a directory: %s" % pkg)
        print(digest_line(pkg))
        return 0

    mods_dir = Path(args.mods_dir
                    or os.environ.get("PC_MODS_DIR")
                    or "pc/mods")
    if args.mods is not None:
        mods = args.mods
    elif "PC_MODS" in os.environ:
        mods = os.environ["PC_MODS"]
    else:
        mods = None

    rom = rom_build(args.rom_build)
    pkgs = resolve_packages(mods_dir, mods)
    if not pkgs:
        die("no packages to cook")

    if args.modinclude is not None:
        modinclude = Path(args.modinclude)
        if modinclude.resolve() == (ROOT / "build" / "pc" / "geninclude").resolve():
            die("refusing to write vanilla build/pc/geninclude")
    else:
        modinclude = None

    allocate_all_records(pkgs)

    total = 0
    for pkg in pkgs:
        total += cook_package(pkg, rom)
    if total == 0:
        die("wrote nothing")
    print_rebuild_report(pkgs, modinclude)
    return 0


if __name__ == "__main__":
    sys.exit(main())
