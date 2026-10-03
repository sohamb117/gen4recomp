#!/usr/bin/env python3
"""Test runner for the PC port.

Every test drives the real binary. Nothing here links its own copy of
anything, so a test that passes says the shipped program works.

  python3 pc/tests/run_tests.py               run everything
  python3 pc/tests/run_tests.py determinism   run one test
  PC_TEST_WORKERS=1                           run them one at a time

PC_BIN picks a different build, which is how the -O2 and sanitizer builds
get tested without overwriting the normal one. Tests that need the binary
skip themselves when it has not been built yet.

  determinism    two identical scripted runs produce identical frames
  mi_selftest    the LZ and copy routines against hand-made vectors
  png_decode     the PNGs the port writes decode under Python zlib
  viewer         pcview renders a channel this test builds itself
  widescreen     wider-than-DS frames keep the picture centred
  hd3d           the 3D layer at 2x and 4x internal resolution
  view_channel   the shared page protocol between port and viewer
  session        closing the window ends the run
  save_durable   the .sav on disk is the save you made, backup kept
  save_lab       loading a save lands where the save was made
  battle_math    damage and accuracy against worked examples
  clock_rollover the real-time clock across midnight and year end
  text_decode    the game text encoding, both directions
  corpus         a spread of ROM data decodes without a diagnostic
  fast_forward   the turbo key changes speed and nothing else
  mods           the mod loader cooks and serves a package
  dist           the release zip unpacks and runs
  patch_headers  every patch in pc/patches applies and rebuilds its object
  sync_gate      the upstream merge script parks a merge that breaks
  ci             pc/ci.sh describes a gate that can still run
  arm7_excluded  no ARM7 source reaches the build
  abi_layout     structs laid out the same by both compilers
  win_frames     the Windows build renders the same frames
  diff           the port against melonDS over the same ROM
  polys          the 3D pipeline against dumped polygon lists
  pclaunch       the launcher finds a ROM and starts the game
  ppwlobby       every lobby symbol resolves to a loud stub
  cli            every flag matches an environment variable
  state_digest   guest memory is identical between two runs
  sym            --watch reads a symbol without disturbing the run
  selftest       the in-binary vector suites for RC4, MD5, SHA-1, LZ
"""

import glob
import json
import mmap
import os
import re
import shutil
import shlex
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# The window channel's header, in order, so the three tests that read or
# synthesize a page all learn about a protocol change at once. Adding
# `publisher` to pc_view.h moved every field after it by a word; the tests
# that carried their own offsets kept reading, a pid as a sequence number,
# and pixels a word out, and failed with a story about something else.
VIEW_FIELDS = ("magic", "version", "publisher", "seq", "frame_lo", "frame_hi",
               "upper_engine", "width", "height", "touch_wanted")
# The viewer-owned words, by offset from the end of the pixel arrays.
VIEW_IN_FIELDS = ("in_seq", "in_keys", "in_touch", "in_touch_x", "in_touch_y",
                  "in_aspect_n", "in_aspect_d", "in_viewer_pid", "in_quit",
                  "in_turbo")
VIEW_HDR = 4 * len(VIEW_FIELDS)
VIEW_MAGIC = 0x50504C56
VIEW_VERSION = 7
VIEW_W, VIEW_H, VIEW_WIDE_MAX, VIEW_HD_MAX = 256, 192, 342, 4
# One screen's pixel array: the widest frame at the highest internal
# resolution, which is what the port sizes the page for.
VIEW_PLANE = VIEW_WIDE_MAX * VIEW_HD_MAX * VIEW_H * VIEW_HD_MAX * 4


def view_header(buf, off=0):
    """The header as a dict, from bytes or a mmap."""
    vals = struct.unpack_from("<%dI" % len(VIEW_FIELDS), buf, off)
    return dict(zip(VIEW_FIELDS, vals))


def view_pack_header(buf, **fields):
    """Write named header fields into a bytearray, zeroing the rest."""
    struct.pack_into("<%dI" % len(VIEW_FIELDS), buf, 0,
                     *[fields.get(n, 0) for n in VIEW_FIELDS])


def view_in_off(name):
    """Byte offset of one viewer-owned word, after both pixel arrays."""
    return VIEW_HDR + 2 * VIEW_PLANE + 4 * VIEW_IN_FIELDS.index(name)


def view_px_off(plane, y, x, width):
    """Byte offset of one pixel: rows are packed at the frame's own width."""
    return VIEW_HDR + plane * VIEW_PLANE + (y * width + x) * 4
# PC_BIN points the runner at a different binary. The -O2 / UBSan / ASan
# detectors live in their own build/pc-* trees so they cannot overwrite
# the ship build.
BINARY = os.environ.get("PC_BIN") or os.path.join(ROOT, "build", "pc", "pokeplatinum")

FRAMES = 900
SCRIPT = """300 keys A
306 keys none
700 keys START
706 keys none
"""


def run_port(dumpdir, script_path):
    # The port creates the frame-dump dir itself, but the audio dump is
    # opened earlier in boot (pc_snd_init) than the video dir is made,
    # give it a directory to land in.
    os.makedirs(dumpdir, exist_ok=True)
    env = dict(os.environ)
    env.update({
        "PC_SAVE": "none",
        "PC_INPUT": script_path,
        "PC_DUMP_FRAMES": dumpdir,
        "PC_DUMP_AUDIO": os.path.join(dumpdir, "audio.wav"),
        "PC_FRAMES": str(FRAMES),
    })
    subprocess.run([BINARY], env=env, check=True, timeout=600,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def test_determinism(tmp):
    script = os.path.join(tmp, "script.txt")
    with open(script, "w") as f:
        f.write(SCRIPT)
    a = os.path.join(tmp, "det-a")
    b = os.path.join(tmp, "det-b")
    run_port(a, script)
    run_port(b, script)

    for name in ("frames.txt", "frame-%06d.png" % FRAMES,
                 "audio.wav"):
        pa = open(os.path.join(a, name), "rb").read()
        pb = open(os.path.join(b, name), "rb").read()
        assert pa == pb, "two identical runs differ in %s" % name

    # The mix is not just deterministic but alive: 900 frames cross the
    # copyright screen into the opening jingle, and a silent WAV here
    # means the driver or the mixer lost its voice.
    import struct
    wav = open(os.path.join(a, "audio.wav"), "rb").read()
    data = wav[44:]
    vals = struct.unpack("<%dh" % (len(data) // 2), data[:len(data) // 2 * 2])
    peak = max(abs(v) for v in vals) if vals else 0
    assert peak > 1000, "audio is silent (peak %d); the driver lost its voice" % peak
    return ("manifests, frame %d and %.1fs of audio byte-identical across "
            "two runs (peak %d)" % (FRAMES, len(vals) / 2 / 32728.0, peak))


def test_mi_selftest(tmp):
    env = dict(os.environ)
    env.update({"PC_SAVE": "none", "PC_FRAMES": "60",
                "PC_MI_SELFTEST": "1"})
    subprocess.run([BINARY], env=env, check=True, timeout=300,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return "LZ8/LZ16/copy vectors pass inside the binary"


def png_decode(data, name="<png>"):
    """(width, height, RGB bytes) from a truecolour PNG, checking every CRC.

    Python's zlib is the independent decoder for an encoder this repository
    wrote itself, so this is the oracle for both the port's frame dumps and
    the viewer's screenshots. Filter type 0 only, which is all the encoder
    emits; an unexpected filter byte is a failure rather than something to
    quietly reconstruct.
    """
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "%s: not a PNG" % name
    pos, idat, w, h = 8, b"", None, None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])
        assert zlib.crc32(ctype + body) & 0xFFFFFFFF == crc, \
            "%s: bad CRC in %r" % (name, ctype)
        if ctype == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
            assert body[8:] == b"\x08\x02\x00\x00\x00", \
                "%s: not 8-bit truecolour, uninterlaced" % name
        elif ctype == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    assert len(raw) == h * (1 + w * 3), "%s: wrong decoded size" % name
    stride = 1 + w * 3
    out = bytearray()
    for y in range(h):
        assert raw[y * stride] == 0, "%s: row %d uses filter %d" % (
            name, y, raw[y * stride])
        out += raw[y * stride + 1:(y + 1) * stride]
    return w, h, bytes(out)


def png_indexed4_decode(data, name="<png>"):
    """(width, height, 48-byte PLTE, w*h indices) from a 4-bit indexed PNG."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "%s: not a PNG" % name
    pos, idat, w, h, plte = 8, b"", None, None, None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            w, h, bit, color, comp, filt, inter = struct.unpack(">IIBBBBB",
                                                                body)
            assert (bit, color, comp, filt, inter) == (4, 3, 0, 0, 0), \
                "%s: not 4-bit indexed, uninterlaced" % name
        elif ctype == b"PLTE":
            plte = body
        elif ctype == b"IDAT":
            idat += body
        pos += 12 + length
    assert w and h and plte is not None and len(plte) >= 48, \
        "%s: missing IHDR/PLTE" % name
    raw = zlib.decompress(idat)
    stride = 1 + (w + 1) // 2
    assert len(raw) == h * stride, "%s: wrong decoded size" % name
    indices = bytearray(w * h)
    for y in range(h):
        assert raw[y * stride] == 0, "%s: row %d uses filter %d" % (
            name, y, raw[y * stride])
        row = raw[y * stride + 1:(y + 1) * stride]
        for x in range(w):
            b = row[x // 2]
            indices[y * w + x] = (b >> 4) if (x & 1) == 0 else (b & 0x0F)
    return w, h, plte[:48], bytes(indices)


def png_indexed4_write(path, w, h, plte, indices):
    """Write a 4-bit indexed PNG (filter 0). nitrogfx accepts this shape."""
    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(0, w, 2):
            left = indices[y * w + x]
            right = indices[y * w + x + 1] if x + 1 < w else 0
            raw.append((left << 4) | right)
    ihdr = struct.pack(">IIBBBBB", w, h, 4, 3, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 9)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"PLTE", plte[:48]) + chunk(b"IDAT", idat)
            + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(data)


def test_png_decode(tmp):
    # Reuse the determinism run's first dump if present; else make one.
    d = os.path.join(tmp, "det-a")
    if not os.path.isdir(d):
        script = os.path.join(tmp, "script.txt")
        with open(script, "w") as f:
            f.write(SCRIPT)
        run_port(d, script)

    checked = 0
    for name in sorted(os.listdir(d)):
        if not name.endswith(".png"):
            continue
        w, h, _ = png_decode(open(os.path.join(d, name), "rb").read(), name)
        assert (w, h) == (VIEW_W, VIEW_H * 2), \
            "%s is %dx%d, not two DS screens" % (name, w, h)
        checked += 1
    assert checked > 0, "no PNGs to check"
    return "%d PNGs decode clean under Python's zlib" % checked


def test_viewer(tmp):
    """pcview against a channel this test writes itself.

    The viewer is the one program here whose output lands on a screen, which
    is exactly where a test cannot look, so --shot renders through the REAL
    draw path under SDL's dummy driver and this test reads the PPM back. The
    channel is synthesized in this function, not published by the port, so a
    wrong pixel here is the viewer's and nobody else's. Selftest runs first:
    it checks the layout maths, the mouse inverse, the scalers and the SDL
    path from inside; this end checks placement from OUTSIDE, against
    coordinates computed independently in Python.
    """
    # BUILD it first, always. Make is a no-op when the binary is current, and
    # the alternative is testing whatever binary happened to be lying around:
    # A pcview compiled against an older pc_view.h reads `width` from the
    # word before it and shears every wide frame, which fails this test with
    # a story about margins rather than about being stale.
    viewer = os.path.join(ROOT, "build", "pc", "pcview")
    build = subprocess.run(["make", "-f", PC_MAKEFILE, "pcview"],
                           capture_output=True, text=True, cwd=ROOT, timeout=600)
    if not os.path.exists(viewer):
        return ("skipped: no build/pc/pcview and it would not build; it "
                "needs SDL2, which the port itself never does: "
                + (build.stderr or build.stdout).strip().splitlines()[-1][:120])

    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    r = subprocess.run([viewer, "--selftest"], env=env,
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, "pcview --selftest failed:\n%s%s" % (r.stdout, r.stderr)

    # The synthetic channel: engine A is red with a green upper-left
    # quadrant (so a flip or a mirror moves a colour), engine B solid blue.
    #
    # Version 3's header is magic, version, seq, frame_lo, frame_hi,
    # upper_engine, width, and `width` is the one that matters here,
    # because the pixel arrays are sized for the widest frame the protocol
    # allows while rows are packed at `width`. Writing 256 here is what makes
    # the rows below land where a native frame's rows go; the arrays are
    # still WIDE apart from each other.
    W, H, WIDE = VIEW_W, VIEW_H, VIEW_WIDE_MAX
    HDR, PLANE = VIEW_HDR, VIEW_PLANE
    name = "pv-test-%d" % os.getpid()
    path = "/dev/shm/" + name
    buf = bytearray(HDR + 2 * PLANE + 4096 * 40)
    view_pack_header(buf, magic=VIEW_MAGIC, version=VIEW_VERSION, seq=2,
                     frame_lo=1, width=W)
    for y in range(H):
        for x in range(W):
            a = 0x00FF00 if (x < W // 2 and y < H // 2) else 0xFF0000
            struct.pack_into("<I", buf, view_px_off(0, y, x, W), a)
            struct.pack_into("<I", buf, view_px_off(1, y, x, W), 0x0000FF)
    with open(path, "wb") as f:
        f.write(buf)

    shots = 0
    try:
        for extra, desc in ((["--layout", "stacked"], "stacked"),
                            (["--layout", "wide"], "wide"),
                            ([], "smart"),
                            (["--layout", "stacked", "--render-scale", "2",
                              "--filter", "scale2x"], "scale2x"),
                            (["--layout", "stacked", "--integer"],
                             "integer")):
            out = os.path.join(tmp, "pv.ppm")
            r = subprocess.run([viewer, name, "--no-audio", "--shot", out,
                                "--wait", "2000"] + extra,
                               env=env, capture_output=True, text=True,
                               timeout=60)
            assert r.returncode == 0, "%s: %s" % (desc, r.stderr)
            d = open(out, "rb").read()
            parts = d.split(b"\n", 3)
            w, h = map(int, parts[1].split())
            px = parts[3]

            def at(fx, fy):
                i = (int(fy * h) * w + int(fx * w)) * 3
                return tuple(px[i:i + 3])

            if desc == "wide":
                ok = (at(.125, .25) == (0, 255, 0) and
                      at(.375, .75) == (255, 0, 0) and
                      at(.75, .5) == (0, 0, 255))
            elif desc == "smart":
                # The default: engine A fills the left two thirds, engine B
                # sits beside it at half the height, vertically centred. The
                # quadrant colours say the main screen was not mirrored and
                # the touch screen is where the layout maths puts it.
                ok = (at(.15, .25) == (0, 255, 0) and
                      at(.5, .75) == (255, 0, 0) and
                      at(.85, .5) == (0, 0, 255) and
                      at(.85, .05) == (0, 0, 0))
            else:
                ok = (at(.25, .125) == (0, 255, 0) and
                      at(.75, .375) == (255, 0, 0) and
                      at(.5, .75) == (0, 0, 255))
            assert ok, "%s: quadrants landed wrong (%dx%d)" % (desc, w, h)
            shots += 1

        # A wide frame, packed the way the port packs one: rows `width`
        # apart, the 256-column panel in the middle, margins beside it. A
        # viewer that kept stepping by 256 would shear the picture a little
        # more with every row, which is exactly what a spot check of three
        # points catches and a check of the top-left corner alone does not.
        margin = (WIDE - W) // 2
        view_pack_header(buf, magic=VIEW_MAGIC, version=VIEW_VERSION, seq=2,
                         frame_lo=1, width=WIDE)
        for y in range(H):
            for x in range(WIDE):
                panel = margin <= x < margin + W
                struct.pack_into("<I", buf, view_px_off(0, y, x, WIDE),
                                 0xFF0000 if panel else 0x00FF00)
                struct.pack_into("<I", buf, view_px_off(1, y, x, WIDE),
                                 0x0000FF if panel else 0x000000)
        with open(path, "wb") as f:
            f.write(buf)

        out = os.path.join(tmp, "pv-wide.ppm")
        r = subprocess.run([viewer, name, "--no-audio", "--shot", out,
                            "--layout", "stacked", "--wait", "2000"],
                           env=env, capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, "wide frame: %s" % r.stderr
        d = open(out, "rb").read()
        parts = d.split(b"\n", 3)
        w, h = map(int, parts[1].split())
        px = parts[3]

        def at(fx, fy):
            i = (int(fy * h) * w + int(fx * w)) * 3
            return tuple(px[i:i + 3])

        # Stacked layout: the top screen is the upper half. The margin is
        # 12.6% of the width, so 5% in is inside it and the middle is not.
        assert at(.05, .25) == (0, 255, 0), \
            "wide: the left margin is not drawn (%dx%d)" % (w, h)
        assert at(.95, .25) == (0, 255, 0), "wide: the right margin is not drawn"
        assert at(.5, .25) == (255, 0, 0), "wide: the panel is not in the middle"
        assert at(.5, .75) == (0, 0, 255), "wide: the touch screen moved"
        assert at(.05, .75) == (0, 0, 0), \
            "wide: the touch screen's margins are not black"
        # The window is shaped for the picture it was given, not for 4:3.
        assert w * (H * 2) > h * W, \
            "wide: the composed picture is no wider than a native one"

        # The same frame as a PNG, decoded by something that is not the
        # encoder. F12 writes PNG at the window's own resolution now, and
        # the encoder it uses is the port's, moved into a shared header,
        # so the check that matters is that the two formats carry the same
        # pixels at the same size. Python's zlib is the independent decoder,
        # exactly as it is for the port's frame dumps.
        outp = os.path.join(tmp, "pv-wide.png")
        r = subprocess.run([viewer, name, "--no-audio", "--shot", outp,
                            "--layout", "stacked", "--wait", "2000"],
                           env=env, capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, "png shot: %s" % r.stderr
        pw, ph, prgb = png_decode(open(outp, "rb").read())
        assert (pw, ph) == (w, h), \
            "the PNG shot is %dx%d and the PPM shot is %dx%d" % (pw, ph, w, h)
        assert prgb == px, "the PNG and PPM shots of one frame differ"
        shots += 1

        # The smart layout with the pen wanted. The port sets touch_wanted
        # when the game starts the SDK's touch sampling, and the viewer's
        # answer is to make room rather than to take it: the touch screen
        # grows to the main screen's size and the window widens to fit,
        # instead of the game shrinking at the moment a battle starts. The
        # picture is what says which happened.
        view_pack_header(buf, magic=VIEW_MAGIC, version=VIEW_VERSION, seq=2,
                         frame_lo=1, width=W, height=H, touch_wanted=1)
        for y in range(H):
            for x in range(W):
                a = 0x00FF00 if (x < W // 2 and y < H // 2) else 0xFF0000
                struct.pack_into("<I", buf, view_px_off(0, y, x, W), a)
                struct.pack_into("<I", buf, view_px_off(1, y, x, W), 0x0000FF)
        with open(path, "wb") as f:
            f.write(buf)
        out = os.path.join(tmp, "pv-touch.ppm")
        r = subprocess.run([viewer, name, "--no-audio", "--shot", out,
                            "--wait", "2000"],
                           env=env, capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, "smart+touch: %s" % r.stderr
        d = open(out, "rb").read()
        parts = d.split(b"\n", 3)
        w, h = map(int, parts[1].split())
        px = parts[3]

        def at2(fx, fy):
            i = (int(fy * h) * w + int(fx * w)) * 3
            return tuple(px[i:i + 3])

        # Two equal screens side by side: the right half is all touch screen
        # now, top to bottom, where at rest its top and bottom corners were
        # letterbox black.
        assert at2(.75, .05) == (0, 0, 255) and at2(.75, .95) == (0, 0, 255), \
            "smart+touch: the touch screen did not grow to full height"
        assert at2(.1, .2) == (0, 255, 0) and at2(.4, .8) == (255, 0, 0), \
            "smart+touch: the main screen moved or was mirrored"
        shots += 1
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    return ("selftest passes and %d shot layouts place every quadrant "
            "where Python says it goes" % shots)




def test_patch_headers(tmp):
    """pc/patches diffs name their source, and the build rebuilds on one.

    The second half is the one with teeth, and it exists because the build did
    not do it. A patch was not a prerequisite of the object it patches, so
    adding one changed nothing: `make` reported success having compiled
    nothing, the binary held no trace of the patch, and the only symptom was an
    instrument that printed no output. Every patch here is a behaviour change
    to the game, so a patch the build ignores is a silent wrong answer. The
    check asks make itself, `-W <patch>` is "pretend this file just changed",
    and it touches nothing, and requires that each object comes back out of
    date.

    The headers are cosmetic, since the build applies each patch to an
    explicit target, but a patch is read by whoever has to re-author it, so
    a header naming a scratch path it was written against is worse than no
    header at all.
    """
    # pc/mods/<name>/patches follows the same convention one level up
    # (pc/mods/README.md), and nothing enforced it: a mod's diff carrying a
    # scratch path or naming the wrong source is the same defect as the
    # port's, and mods are carried into the mirror the same way. There are
    # none today, which is exactly when to write the check.
    bad, checked, modroots = [], 0, []
    root = os.path.join(ROOT, "pc", "patches")
    roots = [root]
    for d in sorted(glob.glob(os.path.join(ROOT, "pc", "mods", "*", "patches"))
                    + glob.glob(os.path.join(ROOT, "pc", "mods", "*",
                                             "plugins", "patches"))):
        roots.append(d)
        modroots.append(d)
    for base in roots:
      for dirpath, _, names in os.walk(base):
        for n in names:
            if not n.endswith(".patch"):
                continue
            full = os.path.join(dirpath, n)
            rel = os.path.relpath(full, base)[:-len(".patch")]
            text = open(full, errors="replace").read()
            checked += 1

            # A patch may open with prose explaining the mwcc-ism it works
            # around; that documentation is the point of keeping these as
            # files. What matters is the ---/+++ pair, wherever it sits.
            for line in text.split("\n"):
                if line.startswith("--- ") and line != "--- a/" + rel:
                    bad.append("%s: %s" % (rel, line))
                elif line.startswith("+++ ") and line != "+++ b/" + rel:
                    bad.append("%s: %s" % (rel, line))

            # And no scratch path anywhere, prose included.
            for m in re.finditer(r"/tmp/\S+|/home/\S+", text):
                bad.append("%s: scratch path %s" % (rel, m.group(0)[:48]))
    assert not bad, ("patch headers not normalized (%d):\n  %s"
                     % (len(bad), "\n  ".join(bad[:5])))

    # And the dependency, read out of make's own database rather than inferred
    # from timestamps: -p prints each goal with its prerequisites already
    # expanded, which is exactly the list that decides whether editing the patch
    # recompiles anything. Timestamps could not answer it, the mod stamp
    # rebuilds unconditionally, so under --dry-run every object looks stale.
    objs = {}
    for dirpath, _, names in os.walk(root):
        for n in names:
            if not n.endswith(".c.patch"):
                continue
            rel = os.path.relpath(os.path.join(dirpath, n), root)[:-len(".patch")]
            objs[os.path.join(root, rel + ".patch")] = os.path.join(
                ROOT, "build", "pc", "obj", "game", rel[:-len(".c")] + ".o")

    rebuilt = ""
    if objs:
        db = subprocess.run(["make", "-f", os.path.join(ROOT, "pc", "Makefile"),
                             "-p", "-n"] + sorted(objs.values()), cwd=ROOT,
                            capture_output=True, text=True, timeout=900).stdout
        prereqs = {}
        for line in db.split("\n"):
            head, sep, rest = line.partition(":")
            if sep and head in objs.values():
                prereqs[head] = rest
        missing = sorted(os.path.relpath(o, ROOT) for o in objs.values()
                         if o not in prereqs)
        assert not missing, ("make named no rule for %d patched object(s): %s"
                             % (len(missing), ", ".join(missing[:3])))
        missed = sorted(os.path.relpath(p, ROOT) for p, o in objs.items()
                        if p not in prereqs[o])
        assert not missed, (
            "%d patch(es) are not a prerequisite of the object they patch, so "
            "editing one compiles nothing and the binary keeps the old "
            "behaviour:\n  %s" % (len(missed), "\n  ".join(missed)))
        rebuilt = ", %d rebuild their object when the diff changes" % len(objs)

    # And the rule is proven to bite, because there are no mod patches in the
    # tree today and a check that has never seen a violation is a comment. A
    # deliberately wrong one is authored under a name nothing else uses, the
    # same walk is run over it, and it has to complain about both things.
    fixture = os.path.join(ROOT, "pc", "mods", "zz-header-fixture", "patches",
                           "src")
    try:
        os.makedirs(fixture, exist_ok=True)
        with open(os.path.join(fixture, "main.c.patch"), "w") as f:
            f.write("--- a/wrong/place.c\n+++ b/wrong/place.c\n"
                    "@@ -1 +1 @@\n-x\n+y\n"
                    "# authored from /tmp/scratch/base.c\n")
        caught = []
        for dirpath, _, names in os.walk(os.path.dirname(fixture)):
            for n in names:
                if not n.endswith(".patch"):
                    continue
                full = os.path.join(dirpath, n)
                rel = os.path.relpath(full, os.path.join(
                    ROOT, "pc", "mods", "zz-header-fixture",
                    "patches"))[:-len(".patch")]
                text = open(full, errors="replace").read()
                for line in text.split("\n"):
                    if line.startswith("--- ") and line != "--- a/" + rel:
                        caught.append(line)
                    elif line.startswith("+++ ") and line != "+++ b/" + rel:
                        caught.append(line)
                for m in re.finditer(r"/tmp/\S+|/home/\S+", text):
                    caught.append(m.group(0))
        assert len(caught) >= 3, (
            "a patch naming the wrong source and carrying a scratch path was "
            "not caught: %s" % caught)
    finally:
        shutil.rmtree(os.path.join(ROOT, "pc", "mods", "zz-header-fixture"),
                      ignore_errors=True)

    return ("%d patches (%d in %d mod(s)): every header names its own source, "
            "no scratch paths%s"
            % (checked,
               sum(1 for b in modroots for _d, _x, ns in os.walk(b)
                   for f in ns if f.endswith(".patch")),
               len(modroots), rebuilt + "; a wrong header and a scratch path "
               "in a mod patch are both caught"))


SYNC_SH = os.path.join(ROOT, "pc", "sync_upstream.sh")
PC_MAKEFILE = os.path.join(ROOT, "pc", "Makefile")

# What pc/sync_upstream.sh reads out of `make status` in order to decide whether
# an upstream merge is safe to keep. Each entry is (literal, why it matters).
# These are the coupling: the Makefile prints them, the script greps them.
GATE_SIGNALS = [
    ("C files compiled:", "how much of the tree compiled"),
    ("skipped:", "the non-compiling count, which gates a sync"),
    ("unresolved symbols:", "reported by the sync, deliberately not a gate"),
    ("link: ok", "whether there is a binary at all"),
    ("pc/patches diff no longer applies", "a dead patch, which gates a sync"),
]


# Identity for fixture repos. Dates are pinned so a recreated FakeSDK
# checkout hashes the same way if a later case needs that; the usual path
# copies the directory instead.
_FIX_GIT = {
    "GIT_AUTHOR_NAME": "fixture",
    "GIT_AUTHOR_EMAIL": "fixture@test",
    "GIT_COMMITTER_NAME": "fixture",
    "GIT_COMMITTER_EMAIL": "fixture@test",
    "GIT_AUTHOR_DATE": "2020-01-01T00:00:00 +0000",
    "GIT_COMMITTER_DATE": "2020-01-01T00:00:00 +0000",
    "EMAIL": "fixture@test",
}


def _fix_git(cwd, *args, env=None, check=True):
    e = os.environ.copy()
    e.update(_FIX_GIT)
    if env:
        e.update(env)
    r = subprocess.run(["git", "-C", cwd, *args], capture_output=True, text=True, env=e)
    if check and r.returncode != 0:
        raise AssertionError("git %s failed (%d): %s%s"
                             % (" ".join(args), r.returncode, r.stdout, r.stderr))
    return r


def _fix_write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def _fix_sdk(dest, head=None):
    """A tiny wrap-git checkout. `head` is recorded so a wrap file can pin it."""
    os.makedirs(dest, exist_ok=True)
    if not os.path.isdir(os.path.join(dest, ".git")):
        _fix_git(dest, "init", "-q")
        _fix_git(dest, "config", "user.email", "fixture@test")
        _fix_git(dest, "config", "user.name", "fixture")
        _fix_git(dest, "config", "commit.gpgsign", "false")
        _fix_write(os.path.join(dest, "README"), "fixture sdk\n")
        _fix_git(dest, "add", "README")
        _fix_git(dest, "commit", "-q", "-m", "fixture sdk")
    return _fix_git(dest, "rev-parse", "HEAD").stdout.strip()


def _fix_tree(root, wrap_rev):
    """A pret-shaped tree the real sync script will accept as ROOT."""
    _fix_write(os.path.join(root, ".gitignore"),
               "build/\nsubprojects/FakeSDK/\n_bin/\n")
    _fix_write(os.path.join(root, "src", "keep.c"), "int keep(void) { return 1; }\n")
    _fix_write(os.path.join(root, "src", "ghost.c"), "int ghost(void) { return 0; }\n")
    _fix_write(os.path.join(root, "src", "patched.c"), "int patched(void) { return 2; }\n")
    _fix_write(os.path.join(root, "include", "foo.h"), "#define FOO 1\n")
    _fix_write(os.path.join(root, "pc", "patches", "src", "patched.c.patch"),
               "--- a/src/patched.c\n+++ b/src/patched.c\n"
               "@@ -1 +1 @@\n-int patched(void) { return 2; }\n"
               "+int patched(void) { return 3; }\n")
    _fix_write(os.path.join(root, "subprojects", "FakeSDK.wrap"),
               "[wrap-git]\n"
               "directory = FakeSDK\n"
               "revision = %s\n"
               "url = https://example.invalid/fake.git\n" % wrap_rev)
    # The literals the real script greps, and the cheap targets it runs.
    _fix_write(os.path.join(root, "pc", "Makefile"), r"""
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
BUILD := $(ROOT)/build/pc
SKIPPED := 0
ifneq ($(wildcard $(ROOT)/pc/.fixture-skipped),)
SKIPPED := $(shell cat $(ROOT)/pc/.fixture-skipped)
endif

.PHONY: status headers test objlist all

all:
	@true

headers:
	@mkdir -p $(BUILD)

status:
	@mkdir -p $(BUILD)
	@if [ -f $(ROOT)/pc/.fixture-dead-patch ]; then \
	  echo "SKIP src/patched.c (pc/patches diff no longer applies)"; \
	fi
	@echo "C files compiled: 1  skipped: $(SKIPPED)"
	@echo "unresolved symbols: 0"
	@echo "link: ok ($(BUILD)/pokeplatinum)"

test:
	@if [ -f $(ROOT)/pc/.fixture-fail-tests ]; then echo FAIL fixture; exit 1; fi
	@echo PASS

objlist:
	@find $(ROOT)/src -name '*.c' 2>/dev/null | \
	  sed 's|^$(ROOT)/src/|$(BUILD)/obj/game/src/|;s|\.c$$|.o|'
""")
    _fix_write(os.path.join(root, "pc", "reach.sh"),
               "#!/bin/sh\nprintf '0\\tok\\tfixture\\n'\n")
    os.chmod(os.path.join(root, "pc", "reach.sh"), 0o755)
    shutil.copy(SYNC_SH, os.path.join(root, "pc", "sync_upstream.sh"))
    os.chmod(os.path.join(root, "pc", "sync_upstream.sh"), 0o755)
    bindir = os.path.join(root, "_bin")
    os.makedirs(bindir, exist_ok=True)
    # ninja fails when a wrap-git pin no longer matches the checkout, that
    # is the ROM-build-cannot-follow-a-wrap class, without a real meson.
    _fix_write(os.path.join(bindir, "ninja"), r"""#!/bin/bash
root="${PC_FIXTURE_ROOT:-$PWD}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    -C) shift 2;;
    *) shift;;
  esac
done
mkdir -p "$root/build/rom"
: > "$root/build/rom/pokeplatinum.us.nds"
: > "$root/build/rom/main.nef"
: > "$root/build/rom/trainer_ai_script.o"
for w in "$root"/subprojects/*.wrap; do
  [[ -f "$w" ]] || continue
  grep -q '^\[wrap-git\]' "$w" || continue
  dir=$(sed -n 's/^[[:space:]]*directory[[:space:]]*=[[:space:]]*//p' "$w" | head -1)
  rev=$(sed -n 's/^[[:space:]]*revision[[:space:]]*=[[:space:]]*//p' "$w" | head -1)
  [[ -n "$dir" && -n "$rev" && -d "$root/subprojects/$dir/.git" ]] || continue
  have=$(git -C "$root/subprojects/$dir" rev-parse HEAD 2>/dev/null) || continue
  if [[ "$have" != "$rev"* && "$rev" != "$have"* ]]; then
    echo "ninja stub: wrap $dir pin $rev does not match $have" >&2
    exit 1
  fi
done
exit 0
""")
    _fix_write(os.path.join(bindir, "meson"),
               "#!/bin/bash\necho meson \"$*\" >> \"${PC_FIXTURE_ROOT:-$PWD}/_meson.log\"\nexit 0\n")
    os.chmod(os.path.join(bindir, "ninja"), 0o755)
    os.chmod(os.path.join(bindir, "meson"), 0o755)


def _fix_seed_objects(work):
    obj = os.path.join(work, "build", "pc", "obj", "game", "src")
    os.makedirs(obj, exist_ok=True)
    for name in ("keep.o", "ghost.o", "patched.o"):
        _fix_write(os.path.join(obj, name), "object\n")


def _fix_drive(tmp, hazard):
    """Clone a tiny tree, apply one upstream hazard, run the real sync.

    Returns (result, recomp_before, recomp_after, work_dir).
    `result` is the CompletedProcess from sync_upstream.sh.
    """
    up = os.path.join(tmp, "up-" + hazard)
    work = os.path.join(tmp, "work-" + hazard)
    os.makedirs(up)
    sdk = os.path.join(up, "subprojects", "FakeSDK")
    wrap_rev = _fix_sdk(sdk)
    _fix_tree(up, wrap_rev)
    _fix_git(up, "init", "-q", "-b", "main")
    _fix_git(up, "config", "user.email", "fixture@test")
    _fix_git(up, "config", "user.name", "fixture")
    _fix_git(up, "config", "commit.gpgsign", "false")
    _fix_git(up, "add", "-A")
    _fix_git(up, "commit", "-q", "-m", "base")
    subprocess.run(["git", "clone", "-q", "-b", "main", up, work],
                   check=True, capture_output=True, text=True)
    _fix_git(work, "checkout", "-q", "-B", "recomp")
    _fix_git(work, "config", "user.email", "fixture@test")
    _fix_git(work, "config", "user.name", "fixture")
    _fix_git(work, "config", "commit.gpgsign", "false")
    shutil.copytree(sdk, os.path.join(work, "subprojects", "FakeSDK"))
    shutil.copytree(os.path.join(up, "_bin"), os.path.join(work, "_bin"))
    _fix_seed_objects(work)
    # The hazard is a commit on origin/main, ahead of recomp.
    if hazard == "clean":
        _fix_write(os.path.join(up, "src", "keep.c"),
                   "int keep(void) { return 4; }\n")
        _fix_git(up, "add", "src/keep.c")
        _fix_git(up, "commit", "-q", "-m", "harmless change")
    elif hazard == "ghost":
        os.remove(os.path.join(up, "src", "ghost.c"))
        _fix_git(up, "add", "-A")
        _fix_git(up, "commit", "-q", "-m", "delete ghost.c")
    elif hazard == "header":
        os.remove(os.path.join(up, "include", "foo.h"))
        _fix_write(os.path.join(up, "include", "bar.h"), "#define BAR 1\n")
        _fix_write(os.path.join(up, "pc", ".fixture-skipped"), "1\n")
        _fix_git(up, "add", "-A")
        _fix_git(up, "commit", "-q", "-m", "move foo.h to bar.h")
    elif hazard == "wrap":
        _fix_write(os.path.join(up, "subprojects", "FakeSDK.wrap"),
                   "[wrap-git]\n"
                   "directory = FakeSDK\n"
                   "revision = bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
                   "url = https://example.invalid/fake.git\n")
        _fix_git(up, "add", "subprojects/FakeSDK.wrap")
        _fix_git(up, "commit", "-q", "-m", "bump FakeSDK wrap")
    elif hazard == "patch":
        _fix_write(os.path.join(up, "src", "patched.c"),
                   "int patched(void) { return 99; }\n")
        _fix_write(os.path.join(up, "pc", ".fixture-dead-patch"), "1\n")
        _fix_git(up, "add", "-A")
        _fix_git(up, "commit", "-q", "-m", "rewrite patched.c")
    else:
        raise AssertionError("unknown hazard %r" % hazard)
    _fix_git(work, "fetch", "-q", "origin")
    before = _fix_git(work, "rev-parse", "recomp").stdout.strip()
    env = os.environ.copy()
    env["PATH"] = os.path.join(work, "_bin") + os.pathsep + env["PATH"]
    env["PC_FIXTURE_ROOT"] = work
    # GIT_DIR and friends from the parent would point at the real tree.
    for k in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY"):
        env.pop(k, None)
    result = subprocess.run(
        ["bash", os.path.join(work, "pc", "sync_upstream.sh"), "--no-reach", "--quiet"],
        cwd=work, capture_output=True, text=True, timeout=60, env=env)
    after = _fix_git(work, "rev-parse", "recomp").stdout.strip()
    return result, before, after, work


def test_sync_gate(tmp):
    """pc/sync_upstream.sh can still read the build, and still parks.

    The sync gate is only as good as its parsing, and it fails in the worst
    available direction: `make status` renames a field, every grep in the gate
    comes back empty, the script reports "compiled ?, unresolved ?" and adopts
    the merge anyway. Nothing errors. A silently blind gate is worse than no
    gate, because the loop trusts it.

    So the literals are asserted to live on BOTH sides, printed by
    pc/Makefile, grepped by the script, and a rename to either file has to
    touch this list, which is the point.

    The other half is the failure classes. Coupling checks cannot see a
    parked merge that never parks: a deleted source whose .o would keep
    linking, a wrap pin meson cannot follow, a header move that raises the
    non-compiling count, a patch that no longer applies. Each is a commit on
    origin/main of a scratch clone; the real script is copied in and driven
    with stub ninja/meson so the gate runs in seconds. A clean batch must
    still adopt, or the park cases are just "the script always exits 3".
    """
    assert os.access(SYNC_SH, os.X_OK), "pc/sync_upstream.sh is not executable"

    syn = subprocess.run(["bash", "-n", SYNC_SH], capture_output=True, text=True)
    assert syn.returncode == 0, "sync_upstream.sh does not parse: " + syn.stderr.strip()

    script = open(SYNC_SH).read()
    mk = open(PC_MAKEFILE).read()
    missing = []
    for lit, why in GATE_SIGNALS:
        if lit not in mk:
            missing.append("pc/Makefile no longer prints %r (%s)" % (lit, why))
        if lit not in script:
            missing.append("the sync no longer greps %r (%s)" % (lit, why))
    assert not missing, ("the sync gate and the build have drifted apart "
                         "(%d):\n  %s" % (len(missing), "\n  ".join(missing)))

    # The other half of the coupling: the gate rebuilds the ROM first because
    # the PC build consumes the xmap and the trainer-AI object out of it. A
    # sync that skipped that step would build against a stale map, and the
    # symptom would look like anything except a stale map.
    for art in ("main.nef.xMAP", "trainer_ai_script.o"):
        assert art in mk, "pc/Makefile no longer names %s" % art
    assert "ninja -C build/rom" in script, \
        "the sync no longer rebuilds the ROM before the port"

    # And updates stale meson wraps before that. pret #1247 bumped
    # NitroSDK.wrap so the ARM7 became a compiled .nef; ninja cannot re-fetch a
    # wrap, so the build paired the new packer with the old checkout and failed
    # with "nef path does not end in .nef", naming neither. A sync that stopped
    # doing this would meet that class again and read it as a port defect.
    assert "meson subprojects update" in script, \
        "the sync no longer updates stale meson wraps before building the ROM"
    wraps = glob.glob(os.path.join(ROOT, "subprojects", "*.wrap"))
    assert wraps, "no subprojects/*.wrap, has the wrap layout changed?"

    # Refusals must be cheap and safe. Off the sync branch, --abort does
    # nothing and says so; it must not exit nonzero or touch the tree.
    before = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"],
                            capture_output=True, text=True).stdout.strip()
    branch = subprocess.run(["git", "-C", ROOT, "rev-parse", "--abbrev-ref", "HEAD"],
                            capture_output=True, text=True).stdout.strip()
    if branch != "sync/upstream":
        r = subprocess.run([SYNC_SH, "--abort"], cwd=ROOT,
                           capture_output=True, text=True, timeout=60)
        assert r.returncode == 0, \
            "--abort off the sync branch should be a no-op, got %d: %s" % (
                r.returncode, r.stdout + r.stderr)
        after = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"],
                               capture_output=True, text=True).stdout.strip()
        assert before == after, "--abort moved HEAD off the sync branch"

    # Drive the real script. A clean batch adopts; each named class parks
    # on sync/upstream with a report and leaves recomp where it was.
    parked = []
    r, b, a, work = _fix_drive(tmp, "clean")
    assert r.returncode == 0, (
        "clean batch should adopt, got %d:\n%s%s" % (r.returncode, r.stdout, r.stderr))
    assert a != b, "clean batch did not fast-forward recomp"
    assert not os.path.exists(os.path.join(work, "pc", ".upstream-sync-report")), \
        "clean batch left a needs-hands report"

    for kind, want_in_report in (
        ("ghost", None),
        ("header", "non-compiling count"),
        ("wrap", "ROM build"),
        ("patch", "dead patch"),
    ):
        r, b, a, work = _fix_drive(tmp, kind)
        report = os.path.join(work, "pc", ".upstream-sync-report")
        ghost_obj = os.path.join(work, "build", "pc", "obj", "game", "src", "ghost.o")
        if kind == "ghost":
            # The script's answer to a deleted source is to prune the
            # leftover .o, not to park. A park here would mean the prune
            # failed and a ghost definition went on linking. After prune
            # the rest of the gate is clean, so recomp should move and the
            # object should be gone.
            assert r.returncode == 0, (
                "deleted source should prune and adopt, got %d:\n%s%s"
                % (r.returncode, r.stdout, r.stderr))
            assert a != b, "deleted source did not fast-forward after prune"
            assert not os.path.exists(ghost_obj), \
                "ghost object still present after a deleted-source sync"
            parked.append("ghost-pruned")
            continue
        assert r.returncode == 3, (
            "%s should park (exit 3), got %d:\n%s%s"
            % (kind, r.returncode, r.stdout, r.stderr))
        assert a == b, "%s fast-forwarded recomp (was %s, now %s)" % (kind, b, a)
        assert os.path.isfile(report), "%s parked with no report" % kind
        body = open(report).read()
        assert "needs hands" in body or "GATE FAILED" in body or "what failed" in body, (
            "%s report is not a needs-hands note:\n%s" % (kind, body))
        if want_in_report:
            assert want_in_report.lower() in body.lower(), (
                "%s report does not name %r:\n%s" % (kind, want_in_report, body))
        on = _fix_git(work, "rev-parse", "--abbrev-ref", "HEAD").stdout.strip()
        assert on == "sync/upstream", (
            "%s left HEAD on %s, not the parked sync branch" % (kind, on))
        parked.append(kind)

    return ("%d gate signal(s); --abort is a safe no-op; "
            "clean adopts; parked %s" % (len(GATE_SIGNALS), ",".join(parked)))


CI_SH = os.path.join(ROOT, "pc", "ci.sh")


def test_ci(tmp):
    """pc/ci.sh still describes a gate that can run.

    The whole build gate is one script rather than a list of steps
    somewhere only a hosted runner can interpret, so it can be run here,
    now, before anybody pushes. This checks that it stays runnable: it
    parses, every stage it accepts is a stage it implements, its package
    list has one home, and every command it would run names a program
    that list covers.

    Rename a stage or a script and this is an assertion in a second
    rather than a failure twenty minutes into a build.
    """
    assert os.access(CI_SH, os.X_OK), "pc/ci.sh is not executable"
    syn = subprocess.run(["bash", "-n", CI_SH], capture_output=True, text=True)
    assert syn.returncode == 0, "pc/ci.sh does not parse: " + syn.stderr.strip()

    script = open(CI_SH).read()
    stages = set(re.findall(r"^stage_([a-z]+)\(\)", script, re.M))
    assert stages, "pc/ci.sh defines no stages"
    # The accepted-stage list is separate from the functions, so the two can
    # drift: a stage_foo nothing accepts is unreachable, an accepted foo with
    # no function dies at the call.
    accepted = set()
    for line in script.split("\n"):
        m = re.match(r"^\s*([a-z|]+)\)\s*;;\s*$", line)
        if m and "|" in m.group(1):
            accepted |= set(m.group(1).split("|"))
    assert accepted == stages, (
        "pc/ci.sh accepts %s and implements %s"
        % (sorted(accepted), sorted(stages)))

    # The package list and the program-to-package map, from the script itself.
    def ci(*args):
        r = subprocess.run([CI_SH] + list(args), cwd=ROOT,
                           capture_output=True, text=True, timeout=120)
        assert r.returncode == 0, ("pc/ci.sh %s failed (%d): %s"
                                   % (" ".join(args), r.returncode, r.stderr))
        return [l for l in r.stdout.split("\n") if l.strip()]

    packages = ci("packages")
    assert packages, "pc/ci.sh names no packages"
    assert len(packages) == len(set(packages)), "the package list repeats itself"
    missing_tool, missing_pkg = [], []
    programs = set()
    for pair in ci("tools"):
        prog, _, pkg = pair.partition(" ")
        programs.add(prog)
        if pkg.strip() not in packages:
            missing_pkg.append("%s comes from %s, which is not installed"
                               % (prog, pkg.strip()))
        if shutil.which(prog) is None:
            missing_tool.append(prog)
    assert not missing_pkg, "\n  ".join(missing_pkg)
    assert not missing_tool, ("pc/ci.sh names program(s) this machine does "
                              "not have: %s" % ", ".join(missing_tool))

    # Every command the stages would run, without running any of them. The
    # first word must be a program the package list covers, and every path it
    # names inside the tree must exist, a stage calling a script that was
    # renamed is otherwise found by a runner.
    dry = subprocess.run([CI_SH, "--dry-run", "all"], cwd=ROOT,
                         capture_output=True, text=True, timeout=120)
    assert dry.returncode == 0, "pc/ci.sh --dry-run all failed: " + dry.stderr
    planned = [l[2:] for l in dry.stdout.split("\n") if l.startswith("+ ")]
    assert planned, "--dry-run printed no commands"
    for cmd in planned:
        words = shlex.split(cmd)
        assert words[0] in programs or shutil.which(words[0]) or \
            os.path.exists(os.path.join(ROOT, words[0])), \
            "%r runs %r, which is neither a listed program nor a file here" % (
                cmd, words[0])
        for word in words[1:]:
            if word.startswith("pc/") and not word.endswith("/"):
                assert os.path.exists(os.path.join(ROOT, word)), \
                    "%r names %s, which does not exist" % (cmd, word)

    return ("%d stage(s), %d package(s), and every command --dry-run plans\n"
            "        names a program the list covers"
            % (len(stages), len(packages)))


# Shared work and the port window came back under the pin (their
# host-pointer words are marked). Main RAM's marked words are the
# function pointers a relink moves; two runs of the SAME binary still
# disagree there (suite replay 2026-08-15: CE088222B239D9BF vs
# 5329865D1FA1D5D0), and opening a file for --dump-polys moves it too.
# That leftover is a host allocation, not a link-layout pointer, and
# it is why this one region stays out.
REPLAY_UNPINNABLE = {
    "02000000": "main RAM, function pointers marked; a host allocation "
                "still lands here and moves with fopen / ASLR",
}


def test_selftest(tmp):
    """The in-binary vector suites: RC4, MD5/SHA-1/HMAC, and the LZ decoders.

    These check the port's reimplementations against the standards' own answers
    rather than against the port's past self, which is what makes them different
    from everything else here. RC4 matters most: pc_src/pc_crypto_rc4.c is a
    reimplementation in C of two ARM assembly routines this port cannot run, and
    a reimplementation validated against nothing is a guess.

    The DGT vectors had been written and never wired, so nothing had ever run
    them. They pass.
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    env["PC_SAVE"] = "none"
    r = subprocess.run([BINARY, "--selftest"], env=env, capture_output=True,
                       text=True, timeout=600)
    lines = [l for l in r.stderr.split("\n") if l.startswith("pc-selftest ")]
    assert lines, "the port printed no pc-selftest lines:\n" + r.stderr[-500:]

    bad = [l for l in lines if " FAIL " in l]
    assert not bad, "a vector suite failed:\n  " + "\n  ".join(bad)
    assert r.returncode == 0, ("every suite reported PASS but the exit status "
                               "was %d" % r.returncode)

    suites = [l.split()[1] for l in lines if l.split()[1] != "all"]
    assert len(suites) >= 3, "expected at least three suites, got %s" % suites
    return "%d vector suite(s) pass: %s" % (len(suites), ", ".join(suites))


def _cov_path_of(source):
    """Where pc_corpus.py just wrote a station's coverage. Not cleared, the
    station's own run did that before it booted."""
    sys.path.insert(0, os.path.join(ROOT, "pc", "tests"))
    import pc_scrcov
    return os.path.join(pc_scrcov.COVDIR, source + ".cov")


def _cov_check(paths, require):
    """The script-coverage ratchet, for a test that has just driven the port.

    A digest pins what reached the screen and a save assert pins what reached
    the save; neither notices a run that stops EXECUTING something and still
    ends up looking the same. This is the third question, and it is answered by
    the runs that were happening anyway rather than by a run of its own.

    Returns a fragment for the test's own result line. Raises when a source
    lost ground, because that is a regression.
    """
    sys.path.insert(0, os.path.join(ROOT, "pc", "tests"))
    import pc_scrcov
    per_source = pc_scrcov.read([p for p in paths if p and os.path.exists(p)])
    bad = pc_scrcov.check(per_source, require=require)
    assert not bad, ("field-script coverage went backwards:\n  %s"
                     % "\n  ".join(bad))
    counts = pc_scrcov.merge(per_source)
    got = sum(len(v) for v in pc_scrcov.names(counts).values())
    return "; %d script command(s) executed, none lost" % got


def test_sym(tmp):
    """--watch turns a decomp name into bytes, and changes nothing by looking.

    The digest says whether two runs ended in the same state. When they did not,
    the next question is *what* moved, and every debugging session so far has
    hand-rolled some version of asking it.

    The property with real teeth here is the last one: an instrument that
    perturbs the run it measures is worse than no instrument, because it makes
    every measurement taken with it suspect. So this requires the state digest to
    be byte-identical with and without a watch registered, reading memory and
    writing lines to stderr must not move a single guest byte.

    Also checked, because each has a wrong answer that looks plausible:

      * A name resolves to an address and its linker-given size, and the report
        says which ADDRESS SPACE it came out of. Every name in this tree is a
        host object, the compiler placed the decompiled globals where the host
        linker wanted them, and a report that said "guest" would be lying.
      * An unknown name is refused by name, and the port exits nonzero rather
        than starting a run whose watch silently does nothing.
      * The watch reports every frame, not once.
    """
    syms = subprocess.run(["nm", BINARY], capture_output=True, text=True)
    assert syms.returncode == 0, "nm failed on the port"
    named = None
    for line in syms.stdout.split("\n"):
        f = line.split()
        # A sizeable initialized global with a decomp-looking name.
        if len(f) == 3 and f[1] in ("B", "D") and f[2].startswith("g") and f[2].isidentifier():
            named = f[2]
            break
    assert named, "no decomp global found in the port's symbol table"

    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    base = dict(env, PC_SAVE="none", PC_FRAMES="30")

    r = subprocess.run([BINARY, "--watch", named, "--watch", "%s:16" % named],
                       env=base, capture_output=True, text=True, timeout=600)
    lines = [l for l in r.stderr.split("\n") if l.startswith("pc-sym ")]
    assert lines, ("no pc-sym output for --watch %s:\n%s"
                   % (named, r.stderr[-500:]))

    spaces = {l.split()[3] for l in lines}
    assert spaces == {"host"}, (
        "every decompiled global in this tree is a host object, but the watch "
        "reported %s" % sorted(spaces))

    labels = {l.split()[1] for l in lines}
    assert "start" in labels, "no start report"
    frames = sorted(l for l in labels if l.startswith("frame:"))
    assert len(frames) > 1, ("a watch must report every frame, got %s"
                             % sorted(labels))

    # Both watches present, and the sized one reports the size asked for.
    assert any(":16" in l for l in lines), "the NAME:LEN watch did not report"

    bad = subprocess.run([BINARY, "--watch", "NoSuchSymbolAnywhere"],
                         env=base, capture_output=True, text=True, timeout=300)
    assert bad.returncode != 0, "an unknown watch name was accepted"
    assert "NoSuchSymbolAnywhere" in bad.stderr, \
        "the refusal does not name the symbol it could not find"

    # The observer property.
    def digest(args):
        e = dict(env, PC_SAVE="none", PC_FRAMES="120", PC_STATE_DIGEST="1")
        out = subprocess.run([BINARY] + args, env=e, capture_output=True,
                             text=True, timeout=600)
        return [l for l in out.stderr.split("\n") if l.startswith("pc-state ")]

    off = digest([])
    on = digest(["--watch", named, "--watch", "%s:64" % named])
    assert off and off == on, (
        "the state digest moved when a watch was registered, so --watch is "
        "perturbing the run it measures:\n  %s"
        % "\n  ".join(_digest_diff(off, on)[:6]))

    return ("%s read by name at a host address over %d frames; the digest is "
            "unchanged with the watch registered" % (named, len(frames)))


DIGEST_FRAMES = 120


def test_state_digest(tmp):
    """Guest memory itself is identical between runs, and under a perturbed host.

    test_determinism compares frame dumps and audio, which is a *consequence* of
    determinism rather than the thing itself: a divergence inside guest memory is
    invisible to it until that state reaches a pixel, and plenty of state never
    does. This compares the state.

    Three runs, because two only rule out the easy failure:

      * A second identical run, catches anything that varies run to run;
      * `env -i`, an empty environment, catches a host value that reached guest
        memory through getenv, a locale or $HOME;
      * `setarch -R`, ASLR off, catches a host POINTER stored in guest memory,
        which is the failure that actually happens. Next door it did: a stale
        host pointer in a live stack-argument frame made three runs give three
        digests differing in exactly sixteen bytes.

    The port is expected to agree in all three. It has no recompiled functions,
    so it has no stack-argument frames for a host pointer to hide in, and this
    test is what says so rather than assuming it.

    ASLR is on by default here, so the plain pair already runs with the address
    space moving underneath it; setarch is the *control* that proves a match was
    not luck.
    """
    def digest(extra_env=None, prefix=()):
        env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        env.update({"PC_SAVE": "none", "PC_FRAMES": str(DIGEST_FRAMES),
                    "PC_STATE_DIGEST": "1"})
        if extra_env is False:          # env -i: nothing but what the port needs
            env = {"PC_SAVE": "none", "PC_FRAMES": str(DIGEST_FRAMES),
                   "PC_STATE_DIGEST": "1"}
        r = subprocess.run(list(prefix) + [BINARY], env=env,
                           capture_output=True, text=True, timeout=600)
        lines = [l for l in r.stderr.split("\n") if l.startswith("pc-state ")]
        assert lines, ("the port printed no pc-state lines under "
                       "PC_STATE_DIGEST=1:\n" + r.stderr[-600:])
        return lines

    a = digest()
    b = digest()
    assert a == b, ("two identical runs disagree in guest memory:\n  %s"
                    % "\n  ".join(l for l in _digest_diff(a, b)[:6]))

    c = digest(extra_env=False)
    assert a == c, ("a run with an empty environment disagrees, so a host value "
                    "is reaching guest memory:\n  %s"
                    % "\n  ".join(l for l in _digest_diff(a, c)[:6]))

    if subprocess.run(["bash", "-c", "command -v setarch"],
                      capture_output=True).returncode == 0:
        d = digest(prefix=("setarch", "-R"))
        assert a == d, ("a run with ASLR disabled disagrees, so a host POINTER "
                        "is stored in guest memory:\n  %s"
                        % "\n  ".join(l for l in _digest_diff(a, d)[:6]))
        control = ", ASLR-off control matches"
    else:
        control = ", setarch absent so no ASLR control"

    # Both endings report, and the report is complete rather than truncated.
    labels = sorted({l.split()[1] for l in a})
    assert labels == ["exit", "start"], \
        "expected a start and an exit report, got %s" % labels
    for label in labels:
        rows = [l for l in a if l.split()[1] == label]
        assert any(" TOTAL " in r for r in rows), \
            "the %s report has no TOTAL line, so it was truncated" % label

    regions = len([l for l in a if l.split()[1] == "start"]) - 1
    assert regions > 0, "no regions in the digest"
    return ("%d guest regions, identical across two runs and an empty "
            "environment%s" % (regions, control))


def _digest_diff(a, b):
    """The differing lines, paired, so a failure names the region."""
    out = []
    for x, y in zip(a, b):
        if x != y:
            out.append("%s\n    vs %s" % (x, y))
    if len(a) != len(b):
        out.append("(line counts differ: %d vs %d)" % (len(a), len(b)))
    return out


CLI_FRAMES = 300


def test_cli(tmp):
    """`--help` is the whole input contract, and a flag IS its variable.

    Every input to this port is an environment variable, because that is what
    makes a run reproducible from its recorded inputs. Flags do not weaken that:
    pc_args_apply translates each one into its variable with putenv before the
    port reads anything, so there is one input with two spellings rather than
    two that must be kept in step.

    Three things are checked, and the third is the one that matters:

      * Every PC_* variable the port actually reads has a row in the table.
        The authoritative set is grepped out of the source, not taken from any
        list, so an input added without documenting it fails here. Five inputs
        had accumulated undocumented before this test existed.
      * Every flag on --help names a variable the port really reads, so the
        page cannot advertise an input that does nothing.
      * A run driven by flags and a run driven by the variables are
        BYTE-IDENTICAL, same frame manifest, same final frame, same audio.
        The other two checks compare lists; this one compares behaviour, and it
        is the only one that could catch a translation that silently dropped a
        value.
    """
    out = subprocess.run([BINARY, "--help"], capture_output=True, text=True,
                         timeout=120)
    assert out.returncode == 0, "--help exited %d" % out.returncode
    help_text = out.stdout

    # The page names each variable in brackets on its own line, under the flag.
    documented = {}
    last_flag = None
    for line in help_text.split("\n"):
        m = re.match(r"\s+(--[a-z0-9-]+)", line)
        if m:
            last_flag = m.group(1)
        m = re.match(r"\s+\[(PC_[A-Z0-9_]+)\]", line)
        if m and last_flag:
            documented[m.group(1)] = last_flag
    assert documented, ("--help lists no variables at all, so every comparison "
                        "below would be vacuous")

    # What the port really reads, from the port.
    reads = set()
    for d in ("src", "hw"):
        for path in glob.glob(os.path.join(ROOT, "pc", d, "*.c")):
            for m in re.finditer(r'getenv\("(PC_[A-Z0-9_]+)"\)', open(path).read()):
                reads.add(m.group(1))
    assert reads, "no getenv(\"PC_...\") found in pc/src or pc/hw"

    undocumented = sorted(reads - set(documented))
    assert not undocumented, (
        "%d input(s) the port reads are not on --help, so they are "
        "undiscoverable and unreachable by flag:\n  %s"
        % (len(undocumented), "\n  ".join(undocumented)))

    phantom = sorted(set(documented) - reads)
    assert not phantom, (
        "--help advertises %d input(s) the port never reads:\n  %s"
        % (len(phantom), "\n  ".join(phantom)))

    # And the equivalence, on the real binary.
    script = os.path.join(tmp, "cli-script.txt")
    with open(script, "w") as f:
        f.write(SCRIPT)

    by_env = os.path.join(tmp, "cli-env")
    by_flag = os.path.join(tmp, "cli-flag")
    os.makedirs(by_env, exist_ok=True)
    os.makedirs(by_flag, exist_ok=True)

    env = dict(os.environ)
    for k in list(env):
        if k.startswith("PC_"):
            del env[k]

    e = dict(env)
    e.update({"PC_SAVE": "none", "PC_INPUT": script,
              "PC_DUMP_FRAMES": by_env,
              "PC_DUMP_AUDIO": os.path.join(by_env, "audio.wav"),
              "PC_FRAMES": str(CLI_FRAMES)})
    subprocess.run([BINARY], env=e, check=True, timeout=600,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    subprocess.run([BINARY,
                    "--save", "none",
                    "--input", script,
                    "--dump-frames", by_flag,
                    "--dump-audio", os.path.join(by_flag, "audio.wav"),
                    "--frames", str(CLI_FRAMES)],
                   env=env, check=True, timeout=600,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    for name in ("frames.txt", "frame-%06d.png" % CLI_FRAMES, "audio.wav"):
        a = open(os.path.join(by_env, name), "rb").read()
        b = open(os.path.join(by_flag, name), "rb").read()
        assert a == b, ("driving by flag and by variable differ in %s, so a "
                        "flag is not the same input as its variable" % name)

    # A rejected argument must apply nothing: a port that booted with half a
    # command line applied would make its own recorded inputs a lie.
    bad = subprocess.run([BINARY, "--frames", "5", "--nonsense"],
                         env=env, capture_output=True, text=True, timeout=120)
    assert bad.returncode != 0, "an unknown option was accepted"
    assert "--help" in (bad.stdout + bad.stderr), \
        "the error for an unknown option does not point at --help"

    return ("%d inputs, all on --help and all reachable by flag; %d frames "
            "byte-identical driven either way" % (len(reads), CLI_FRAMES))


def test_widescreen(tmp):
    """Widescreen widens the camera and leaves the game alone.

    The port owns the projection, so --aspect widens the 3D field of view
    rather than stretching pixels. The claim that makes it safe is
    geometric: the clip matrix's X column is scaled by 256/W and the
    viewport is scaled the other way, so THE CENTRE 256 columns of a wide
    Render are the native picture. That is what is checked here, pixel for
    pixel over a fixed boot, because it is the difference between a
    widescreen feature and a picture that merely looks wider.

    If that check ever fails it is worth reading before it is re-pinned.
    The one thing that can legitimately move it is the box test: it culls
    against the frustum actually being drawn, so a game asking "is this
    cube visible" can get a different answer wide than native and take a
    different path. That is the feature working, and it is also why
    --aspect refuses to start beside the instruments that compare a native
    picture.

    The margins themselves are covered end to end by test_viewer, which
    publishes a synthetic wide frame and reads the composed picture back.
    Here they are only required to be black during a scene that draws no 3D
    at the edges; which is the attract loop, and which is the case where
    a packing mistake would show as garbage.
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    WIDE, W, H = VIEW_WIDE_MAX, VIEW_W, VIEW_H
    FRAMES = 900

    def run(args, extra_env=None, expect=0):
        e = dict(env, PC_SAVE="none", PC_PACE="0", PC_FRAMES=str(FRAMES))
        if extra_env:
            e.update(extra_env)
        r = subprocess.run([BINARY] + args, env=e, capture_output=True,
                           text=True, timeout=900)
        assert r.returncode == expect, (
            "%s: expected exit %d, got %d\n%s"
            % (" ".join(args) or "(no flags)", expect, r.returncode,
               (r.stderr or r.stdout)[-400:]))
        return r

    # What the flag will not accept. Each of these has a plausible wrong
    # behaviour, silently clamping, or rendering at a width the protocol
    # cannot carry, and refusing is the only one a person can act on.
    for spec in ("junk", "9:0", "0:9", "100", "400", "16:9:2"):
        r = run(["--aspect", spec, "--frames", "5"], expect=2)
        assert "--aspect" in (r.stderr + r.stdout), \
            "the refusal of --aspect %s does not name the flag" % spec

    # And what it will not run beside: the instruments that exist to
    # compare a native picture.
    for guard in (["--dump-frames", os.path.join(tmp, "d")],
                  ["--diff-spans", os.path.join(tmp, "spans.txt")]):
        r = run(["--aspect", "16:9"] + guard + ["--frames", "5"], expect=2)
        assert "--aspect" in r.stderr, \
            "the refusal beside %s does not name --aspect" % guard[0]

    def published(name):
        d = open("/dev/shm/" + name, "rb").read()
        h = view_header(d)
        assert h["magic"] == VIEW_MAGIC and h["version"] == VIEW_VERSION, \
            ("the channel is not this build's protocol (%x, v%d)"
             % (h["magic"], h["version"]))
        assert h["seq"] % 2 == 0, "the page was read mid-write"
        return h["width"], h["frame_lo"], d

    def px(d, plane, y, x, width):
        return struct.unpack_from("<I", d, view_px_off(plane, y, x, width))[0]

    names = ["wide-%d-%s" % (os.getpid(), k) for k in ("nat", "auto", "169")]
    try:
        run(["--view", names[0]])
        run(["--aspect", "auto", "--view", names[1]])
        run(["--aspect", "16:9", "--view", names[2]])

        nat_w, nat_f, nat = published(names[0])
        auto_w, _, _ = published(names[1])
        wide_w, wide_f, wide = published(names[2])

        assert nat_w == W, "a run with no --aspect published %d columns" % nat_w
        assert auto_w == W, (
            "--aspect auto with no viewer published %d columns; an adaptive "
            "width with nothing to adapt to must stay native" % auto_w)
        assert W < wide_w <= WIDE, \
            "--aspect 16:9 published %d columns" % wide_w
        assert wide_w % 2 == 0, \
            "%d columns cannot split into two equal margins" % wide_w
        assert nat_f == wide_f == FRAMES, \
            "the runs did not both reach frame %d" % FRAMES

        margin = (wide_w - W) // 2
        diff = [(y, x) for y in range(H) for x in range(W)
                if px(wide, 0, y, margin + x, wide_w) != px(nat, 0, y, x, W)]
        assert not diff, (
            "%d of %d pixels differ between the centre of the wide picture "
            "and the native one (first at %r). The centre of a wide render "
            "is supposed to BE the native picture; see this test's docstring "
            "for the one thing that can legitimately move it."
            % (len(diff), W * H, diff[0]))

        lit = [x for y in range(H)
               for x in list(range(margin)) + list(range(margin + W, wide_w))
               if px(wide, 0, y, x, wide_w) != 0]
        assert not lit, (
            "%d margin pixels are not black in a scene that draws no 3D at "
            "the edges; the rows are probably packed at the wrong width"
            % len(lit))
    finally:
        for n in names:
            try:
                os.unlink("/dev/shm/" + n)
            except OSError:
                pass

    return ("16:9 renders %d columns, the centre %d are the native picture "
            "pixel for pixel over %d frames, auto with no window stays native"
            % (wide_w, W, FRAMES))


def test_view_channel(tmp):
    """One page, one publisher, and a page nobody owns is free to take.

    Two ports publishing on one channel produce a picture assembled from two
    different games, sixty times a second. It looks like a display bug and it
    is not one, which is what makes it worth refusing by name: a session left
    running from hours earlier was still publishing under the default channel
    name while a new one wrote the same page, and across a protocol version
    the older port does not even maintain the fields the reader needs, the
    reader took a row width out of what the other port considers pixel data
    and read every row at the wrong stride.

    The two cases a port must tell apart differ in one observable: a live
    publisher advances `seq`, an abandoned page does not. Both are checked
    here, because refusing the second case would be just as wrong, on Linux
    a page outlives the process that made it, so every second run would fail.
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    name = "chan-test-%d" % os.getpid()
    path = "/dev/shm/" + name
    holder = None
    try:
        holder = subprocess.Popen(
            [BINARY], env=dict(env, PC_SAVE="none", PC_VIEW=name,
                               PC_FRAMES="100000"),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # Wait for it to actually be publishing rather than merely started.
        deadline = time.time() + 60
        seq = None
        while time.time() < deadline:
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                with open(path, "rb") as f:
                    s = view_header(f.read(VIEW_HDR))["seq"]
                if seq is not None and s != seq:
                    break
                seq = s
            time.sleep(0.05)
        else:
            return "skipped: the holding port never started publishing"

        second = subprocess.run(
            [BINARY], env=dict(env, PC_SAVE="none", PC_VIEW=name,
                               PC_FRAMES="10"),
            capture_output=True, text=True, timeout=300)
        assert second.returncode != 0, (
            "a second port on a channel another port is publishing on "
            "started anyway; both would write the same page every frame")
        assert str(holder.pid) in second.stderr or "another port" in second.stderr, \
            ("the refusal does not say who holds the channel:\n%s"
             % second.stderr[-300:])
    finally:
        if holder is not None:
            holder.kill()
            holder.wait(timeout=60)

    # The same name, now that nobody is publishing on it: this must work, and
    # it is the case every ordinary re-run on Linux hits.
    again = subprocess.run(
        [BINARY], env=dict(env, PC_SAVE="none", PC_VIEW=name, PC_FRAMES="10"),
        capture_output=True, text=True, timeout=300)
    try:
        os.unlink(path)
    except OSError:
        pass
    assert again.returncode == 0, (
        "a page left behind by a port that has exited was refused, so every "
        "second run on the same channel would fail:\n%s" % again.stderr[-300:])

    return ("a second live publisher is refused by name, an abandoned page "
            "is reused")


def test_window_is_the_session(tmp):
    """Closing the window ends the run, however the window went away.

    The port has no window (the viewer holds it) so a port that outlived
    its viewer was a process nobody could see and nobody believed was there.
    They accumulated: one left running in the morning was still publishing on
    the default channel that night, and the next session wrote the same page,
    which looks like a display bug and is not one.

    Three behaviours, and the third is why the first two are not enough:

      * The viewer says it is closing, and the run ends;
      * the viewer dies without saying anything (killed, crashed) and the
        run ends anyway, because a published pid can be asked whether it is
        still alive;
      * --keep-alive turns both off, for attaching a viewer to a long
        headless run and detaching again.

    A clean exit rather than a kill, because the atexit handlers close the
    frame dump and the WAV header and the state report runs.
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}

    def start(name, extra=None):
        e = dict(env, PC_SAVE="none", PC_VIEW=name, PC_FRAMES="100000")
        if extra:
            e.update(extra)
        p = subprocess.Popen([BINARY], env=e,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        path = "/dev/shm/" + name
        deadline = time.time() + 90
        seq = None
        while time.time() < deadline:
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                with open(path, "rb") as f:
                    s = view_header(f.read(VIEW_HDR))["seq"]
                if seq is not None and s != seq:
                    return p, path
                seq = s
            time.sleep(0.05)
        p.kill(); p.wait(timeout=60)
        return None, path

    def poke(path, **fields):
        with open(path, "r+b") as f:
            for k, v in fields.items():
                f.seek(view_in_off(k))
                f.write(struct.pack("<I", v))

    def ended(p, seconds):
        try:
            return p.wait(timeout=seconds) == 0
        except subprocess.TimeoutExpired:
            return None

    names = ["sess-%d-%s" % (os.getpid(), k) for k in ("quit", "died", "keep")]
    procs = []
    try:
        # 1. The viewer says goodbye.
        p, path = start(names[0])
        if p is None:
            return "skipped: the port never started publishing"
        procs.append(p)
        poke(path, in_quit=1)
        assert ended(p, 30), "the port kept running after the window closed"

        # 2. The viewer dies without a word. A sleep stands in for it: a real
        #    pid that this test can kill, which is exactly what the port has
        #    to notice.
        p, path = start(names[1])
        if p is None:
            return "skipped: the port never started publishing"
        procs.append(p)
        stand_in = subprocess.Popen(["sleep", "300"])
        poke(path, in_viewer_pid=stand_in.pid)
        time.sleep(2)           # the port must NOT leave while it is alive
        assert p.poll() is None, \
            "the port ended while its viewer was still running"
        stand_in.kill(); stand_in.wait(timeout=30)
        assert ended(p, 30), \
            "the port kept running after its viewer died without saying so"

        # 3. --keep-alive means neither ends it.
        p, path = start(names[2], {"PC_KEEP_ALIVE": "1"})
        if p is None:
            return "skipped: the port never started publishing"
        procs.append(p)
        poke(path, in_quit=1)
        assert ended(p, 5) is None, \
            "--keep-alive did not keep the run alive past a closing window"
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait(timeout=60)
        for n in names:
            try:
                os.unlink("/dev/shm/" + n)
            except OSError:
                pass

    return ("a closing window ends the run, a viewer that dies without saying "
            "so ends it too, and --keep-alive holds it open")


def test_mods(tmp):
    """The mod framework: opt-in, loud, and never a byte of the vanilla port.

    Three claims, each of which has a quiet failure mode this pins shut:

      * A mod's patch reaches the compile (the marker symbol lands in the
        modded object), a framework that silently built vanilla under
        MODS= would look identical from the outside;
      * the vanilla build directory is untouched by a modded build, the
        vanilla binary is the one every pinned number measures, and one
        stray object would poison it invisibly;
      * an unknown mod name refuses at make time, because "the mod did not
        load" discovered in-game is the worst version of that error.

    The fixture mod is authored HERE, against the build input the vanilla
    build actually consumed, so this test cannot rot the way a shipped
    example would: the patch is regenerated against today's source every
    run and removed again in `finally`.
    """
    van_obj = os.path.join(ROOT, "build", "pc", "obj", "game", "src", "main.o")
    if not os.path.exists(van_obj):
        return "skipped: no vanilla build to author against (make -f pc/Makefile)"

    # Quality-bar item 7: content must not rewrite vanilla game/SDK objects
    # or geninclude. pc_modfs.o lives under obj/pc/ and is the one new
    # vanilla object; it is not in this snapshot.
    def snapshot_vanilla_tree():
        snap = {}
        for base in (os.path.join(ROOT, "build", "pc", "obj", "game"),
                     os.path.join(ROOT, "build", "pc", "geninclude")):
            if not os.path.isdir(base):
                continue
            for dirpath, _, files in os.walk(base):
                for name in files:
                    path = os.path.join(dirpath, name)
                    st = os.stat(path)
                    snap[os.path.relpath(path, ROOT)] = (
                        st.st_mtime_ns, st.st_size)
        return snap

    def assert_vanilla_untouched(snap, when):
        now = snapshot_vanilla_tree()
        changed = sorted(
            k for k in set(snap) | set(now)
            if snap.get(k) != now.get(k))
        assert not changed, (
            "content-only work rewrote vanilla game/SDK objects or "
            "geninclude (%s): %s" % (when, changed[:8]))

    van_snap = snapshot_vanilla_tree()
    assert os.path.isfile(os.path.join(ROOT, "build", "pc", "obj", "pc",
                                       "pc_modfs.o")), \
        "vanilla is missing pc_modfs.o"

    # The input the compile consumed: strip_asm's copy, or the base-patched
    # copy if src/main.c ever grows a pc/patches diff.
    base = None
    for suffix in (".patched.c", ".stripped.c"):
        if os.path.exists(van_obj + suffix):
            base = van_obj + suffix
            break
    if base is None:
        base = os.path.join(ROOT, "src", "main.c")

    fixture = os.path.join(ROOT, "pc", "mods", "zz-test-fixture")
    mods_obj = os.path.join(ROOT, "build", "pc-mods", "obj", "game", "src", "main.o")
    marker = "zz_test_fixture_marker"
    plug_marker = "zz_plugins_patch_marker"
    host_marker = "zz_host_layer_marker"
    van_stat = os.stat(van_obj)

    def write_unified(old_path, new_text, dest_patch, a_name):
        old = open(old_path).read()
        old_tmp = os.path.join(tmp, "diff-old")
        new_tmp = os.path.join(tmp, "diff-new")
        open(old_tmp, "w").write(old)
        open(new_tmp, "w").write(new_text)
        diff = subprocess.run(["diff", "-u", old_tmp, new_tmp],
                              capture_output=True, text=True)
        lines = diff.stdout.split("\n")
        lines[0] = "--- a/" + a_name
        lines[1] = "+++ b/" + a_name
        os.makedirs(os.path.dirname(dest_patch), exist_ok=True)
        open(dest_patch, "w").write("\n".join(lines))

    try:
        os.makedirs(os.path.join(fixture, "patches", "src"), exist_ok=True)
        mine = os.path.join(tmp, "mine.c")
        shutil.copy(base, mine)
        with open(mine, "a") as f:
            f.write("\nconst int %s = 42;\n" % marker)
        write_unified(base, open(mine).read(),
                      os.path.join(fixture, "patches", "src", "main.c.patch"),
                      "src/main.c")

        # plugins/patches applies on top of historical patches/.
        plug_src = open(mine).read() + "const int %s = 7;\n" % plug_marker
        write_unified(mine, plug_src,
                      os.path.join(fixture, "plugins", "patches", "src",
                                   "main.c.patch"),
                      "src/main.c")

        os.makedirs(os.path.join(fixture, "plugins", "include"), exist_ok=True)
        open(os.path.join(fixture, "plugins", "include", "zz_plug.h"),
             "w").write("#define ZZ_PLUG_MARK 7\n")
        os.makedirs(os.path.join(fixture, "include"), exist_ok=True)
        open(os.path.join(fixture, "include", "zz_hist.h"),
             "w").write("#define ZZ_HIST_MARK 3\n")
        os.makedirs(os.path.join(fixture, "plugins", "src", "deep"),
                    exist_ok=True)
        open(os.path.join(fixture, "plugins", "src", "deep", "zz_plug.c"),
             "w").write('#include "zz_plug.h"\n'
                        '#include "constants/heap.h"\n'
                        "const int zz_plug_mark = ZZ_PLUG_MARK;\n"
                        "const int zz_heap_patch = ZZ_HEAP_PATCH;\n")
        os.makedirs(os.path.join(fixture, "src"), exist_ok=True)
        open(os.path.join(fixture, "src", "zz_hist.c"),
             "w").write('#include "zz_hist.h"\n'
                        "const int zz_hist_mark = ZZ_HIST_MARK;\n")

        heap_h = os.path.join(ROOT, "include", "constants", "heap.h")
        write_unified(heap_h,
                      open(heap_h).read().replace(
                          "#endif // POKEPLATINUM_CONSTANTS_HEAP_H",
                          "#define ZZ_HEAP_PATCH 91\n"
                          "#endif // POKEPLATINUM_CONSTANTS_HEAP_H"),
                      os.path.join(fixture, "plugins", "patches",
                                   "include", "constants", "heap.h.patch"),
                      "include/constants/heap.h")

        host_c = os.path.join(ROOT, "pc", "src", "pc_div0.c")
        write_unified(host_c,
                      open(host_c).read() + "\nconst int %s = 1;\n" % host_marker,
                      os.path.join(fixture, "plugins", "patches",
                                   "pc", "src", "pc_div0.c.patch"),
                      "pc/src/pc_div0.c")

        # fuzz=0: first and last context lines are garbage. Default fuzz
        # (2) can ignore them; --fuzz=0 cannot.
        fuzz_patch = os.path.join(fixture, "plugins", "patches",
                                  "src", "timer.c.patch")
        os.makedirs(os.path.dirname(fuzz_patch), exist_ok=True)
        open(fuzz_patch, "w").write(
            "--- a/src/timer.c\n"
            "+++ b/src/timer.c\n"
            "@@ -12,6 +12,6 @@\n"
            " XXX void Timer_Start(void)\n"
            " {\n"
            "-    sTimerValue = 0;\n"
            "+    sTimerValue = 1;\n"
            "     sShouldResetTimer = FALSE;\n"
            " YYY\n"
        )

        with open(os.path.join(fixture, "README.md"), "w") as f:
            f.write("Test fixture: adds one marker symbol; created and "
                    "removed by the test suite.\n")

        r = subprocess.run(["make", "-f", PC_MAKEFILE,
                            "MODS=zz-test-fixture", mods_obj],
                           capture_output=True, text=True, cwd=ROOT,
                           timeout=600)
        assert r.returncode == 0, ("the modded object did not build:\n%s"
                                   % (r.stderr or r.stdout)[-400:])
        nm = subprocess.run(["nm", mods_obj], capture_output=True, text=True)
        assert marker in nm.stdout, \
            "the mod's patch never reached the compile"
        assert plug_marker in nm.stdout, \
            "plugins/patches/ never reached the compile"

        plug_obj = os.path.join(ROOT, "build", "pc-mods", "obj", "mods",
                                "zz-test-fixture", "plugins", "src",
                                "deep", "zz_plug.o")
        hist_obj = os.path.join(ROOT, "build", "pc-mods", "obj", "mods",
                                "zz-test-fixture", "src", "zz_hist.o")
        host_obj = os.path.join(ROOT, "build", "pc-mods", "obj", "pc",
                                "pc_div0.o")
        r = subprocess.run(["make", "-f", PC_MAKEFILE,
                            "MODS=zz-test-fixture",
                            plug_obj, hist_obj, host_obj],
                           capture_output=True, text=True, cwd=ROOT,
                           timeout=600)
        assert r.returncode == 0, ("plugin / host objects did not build:\n%s"
                                   % (r.stderr or r.stdout)[-800:])
        assert os.path.isfile(plug_obj), "nested plugins/src did not compile"
        nm = subprocess.run(["nm", plug_obj], capture_output=True, text=True)
        assert "zz_plug_mark" in nm.stdout, nm.stdout
        assert "zz_heap_patch" in nm.stdout, \
            "header patch did not reach the plugin compile"
        nm = subprocess.run(["nm", hist_obj], capture_output=True, text=True)
        assert "zz_hist_mark" in nm.stdout, \
            "historical include/ did not resolve"
        nm = subprocess.run(["nm", host_obj], capture_output=True, text=True)
        assert host_marker in nm.stdout, \
            "a host-layer patch on pc/src was ignored"

        staged_heap = os.path.join(ROOT, "build", "pc-mods", "modinclude",
                                   "constants", "heap.h")
        assert os.path.isfile(staged_heap), "header patch was not staged"
        assert "ZZ_HEAP_PATCH" in open(staged_heap).read(), \
            "staged heap.h does not carry the patch"

        timer_obj = os.path.join(ROOT, "build", "pc-mods", "obj", "game",
                                 "src", "timer.o")
        r = subprocess.run(["make", "-f", PC_MAKEFILE,
                            "MODS=zz-test-fixture", timer_obj],
                           capture_output=True, text=True, cwd=ROOT,
                           timeout=600)
        out = r.stderr + r.stdout
        assert "no longer applies" in out and "timer.c" in out, \
            "fuzz=0 did not refuse a patch that needs fuzz:\n%s" % out[-800:]
        assert not os.path.isfile(timer_obj) or os.path.isfile(
            timer_obj + ".skipped"), \
            "a fuzz-needed patch produced a timer object"

        # The same file, vanilla: untouched on disk and free of the marker.
        s2 = os.stat(van_obj)
        assert (s2.st_mtime_ns, s2.st_size) == (van_stat.st_mtime_ns,
                                                van_stat.st_size), \
            "a modded build touched the vanilla object tree"
        nm = subprocess.run(["nm", van_obj], capture_output=True, text=True)
        assert marker not in nm.stdout, "the marker leaked into vanilla"

        # The manifest a modded binary would print.
        manifest = os.path.join(ROOT, "build", "pc-mods", "mods_manifest.c")
        r = subprocess.run(["make", "-f", PC_MAKEFILE,
                            "MODS=zz-test-fixture", manifest],
                           capture_output=True, text=True, cwd=ROOT,
                           timeout=120)
        assert r.returncode == 0 and \
            'pc_mods_manifest[] = "zz-test-fixture"' in open(manifest).read(), \
            "the manifest does not name the enabled mod"

        # And the refusal.
        r = subprocess.run(["make", "-f", PC_MAKEFILE,
                            "MODS=no-such-mod", "status"],
                           capture_output=True, text=True, cwd=ROOT,
                           timeout=120)
        assert r.returncode != 0 and "no-such-mod" in (r.stderr + r.stdout), \
            "an unknown mod name did not refuse at make time"
    finally:
        shutil.rmtree(fixture, ignore_errors=True)

    assert_vanilla_untouched(van_snap, "after compile-time fixture")

    # Runtime packages. Drive the binary against a temp PC_MODS_DIR so
    # nothing is left in pc/mods/. PC_ROM points at a missing file so
    # the process dies right after pc_modfs_boot prints (or refuses).
    if not os.path.exists(BINARY):
        return ("a mod patch reaches the modded object, vanilla is untouched, "
                "the manifest names it, an unknown name refuses; "
                "runtime packages skipped (no binary)")

    def write_toml(pkgdir, **fields):
        os.makedirs(pkgdir, exist_ok=True)
        lines = []
        for k in ("id", "name", "version", "authors", "requires", "load_after"):
            if k not in fields:
                continue
            v = fields[k]
            if isinstance(v, list):
                inner = ", ".join('"%s"' % x for x in v)
                lines.append("%s = [%s]\n" % (k, inner))
            else:
                lines.append('%s = "%s"\n' % (k, v))
        with open(os.path.join(pkgdir, "mod.toml"), "w") as f:
            f.writelines(lines)

    def boot(mods_dir, mods=None):
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e["PC_MODS_DIR"] = mods_dir
        e["PC_SAVE"] = "none"
        e["PC_ROM"] = os.path.join(tmp, "no-such.nds")
        if mods is not None:
            e["PC_MODS"] = mods
        return subprocess.run([BINARY], env=e, capture_output=True, text=True,
                              timeout=120)

    root = os.path.join(tmp, "mods")
    os.makedirs(root, exist_ok=True)
    write_toml(os.path.join(root, "alpha"),
               id="alpha", name="Alpha", version="1.0")
    write_toml(os.path.join(root, "beta"),
               id="beta", name="Beta", version="1.0")

    r = boot(root, "alpha,beta")
    out = r.stderr + r.stdout
    assert "modfs: 0 files, 0 members, order=[alpha, beta]" in out, out[-800:]

    r = boot(root, "beta alpha")
    out = r.stderr + r.stdout
    assert "modfs: 0 files, 0 members, order=[beta, alpha]" in out, out[-800:]

    r = boot(root, "nope")
    out = r.stderr + r.stdout
    assert r.returncode != 0, "unknown package did not refuse"
    assert "nope" in out and root in out, out[-800:]

    for key, body in (
        ("id", 'name = "X"\nversion = "1"\n'),
        ("name", 'id = "x"\nversion = "1"\n'),
        ("version", 'id = "x"\nname = "X"\n'),
    ):
        d = os.path.join(tmp, "miss-" + key)
        os.makedirs(os.path.join(d, "x"), exist_ok=True)
        open(os.path.join(d, "x", "mod.toml"), "w").write(body)
        r = boot(d, "x")
        out = r.stderr + r.stdout
        assert r.returncode != 0 and key in out, (key, out[-800:])

    write_toml(os.path.join(root, "alpha2"),
               id="alpha", name="Alpha2", version="1.0")
    r = boot(root, "alpha,alpha2")
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "id" in out, out[-800:]

    write_toml(os.path.join(root, "need"),
               id="need", name="Need", version="1.0", requires=["ghost"])
    r = boot(root, "need")
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "need" in out and "ghost" in out, out[-800:]

    write_toml(os.path.join(root, "late"),
               id="late", name="Late", version="1.0", load_after=["beta"])
    r = boot(root, "late,beta")
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "late" in out and "beta" in out, out[-800:]

    plug = os.path.join(root, "plugin")
    os.makedirs(os.path.join(plug, "src"), exist_ok=True)
    os.makedirs(os.path.join(plug, "patches"), exist_ok=True)
    r = boot(root, "plugin")
    out = r.stderr + r.stdout
    assert r.returncode != 0, "compile-time plugin did not refuse"
    assert "MODS=" in out and "PC_MODS" in out, out[-800:]

    nest = os.path.join(root, "plugnest")
    os.makedirs(os.path.join(nest, "plugins", "src"), exist_ok=True)
    r = boot(root, "plugnest")
    out = r.stderr + r.stdout
    assert r.returncode != 0, "plugins/ nest did not refuse as a compile plugin"
    assert "MODS=" in out and "PC_MODS" in out, out[-800:]

    empty = os.path.join(tmp, "mods-empty")
    os.makedirs(empty, exist_ok=True)
    r = boot(empty, mods=None)
    out = r.stderr + r.stdout
    assert "modfs:" not in out, out[-800:]

    with open(os.path.join(root, "loadorder.txt"), "w") as f:
        f.write("# comment\n\nalpha\nbeta\n")
    r = boot(root, mods=None)
    out = r.stderr + r.stdout
    assert "modfs: 0 files, 0 members, order=[alpha, beta]" in out, out[-800:]

    r = boot(root, "beta")
    out = r.stderr + r.stdout
    assert "modfs: 0 files, 0 members, order=[beta]" in out, out[-800:]

    e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    e["PC_SAVE"] = "none"
    e["PC_ROM"] = os.path.join(tmp, "no-such.nds")
    r = subprocess.run([BINARY, "PC_MODS_DIR=" + root, "PC_MODS=alpha"],
                       env=e, capture_output=True, text=True, timeout=120)
    out = r.stderr + r.stdout
    assert "modfs: 0 files, 0 members, order=[alpha]" in out, out[-800:]

    cooked = os.path.join(root, "alpha", ".cooked")
    os.makedirs(cooked, exist_ok=True)
    r = boot(root, "alpha")
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "cook" in out, out[-800:]
    shutil.rmtree(cooked, ignore_errors=True)

    # Whole-file overlay. Scan claims (missing ROM is enough); bytes come
    # back through the game's own FS_OpenFile via PC_MODFS_PROBE.
    def boot_probe(mods_dir, mods, probe, extra=None):
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e["PC_MODS_DIR"] = mods_dir
        e["PC_SAVE"] = "none"
        e["PC_MODS"] = mods
        e["PC_MODFS_PROBE"] = probe
        if extra:
            e.update(extra)
        return subprocess.run([BINARY], env=e, capture_output=True, text=True,
                              timeout=120)

    def plant(pkgdir, rel, data):
        path = os.path.join(pkgdir, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    def cook_digest_bytes(pkgdir):
        r = subprocess.run(
            [sys.executable, os.path.join(ROOT, "pc", "modcook.py"),
             "--print-digest", pkgdir],
            capture_output=True, text=True, cwd=ROOT, timeout=30)
        assert r.returncode == 0, r.stderr or r.stdout
        return (r.stdout.strip() + "\n").encode()

    blob = b"MODFS-1.2-PROBE\n"
    plant(os.path.join(root, "alpha"), "replace/modfs/probe.bin", blob)
    r = boot(root, "alpha")
    out = r.stderr + r.stdout
    assert "modfs: 1 files, 0 members, order=[alpha]" in out, out[-800:]

    r = subprocess.run(
        ["make", "-f", PC_MAKEFILE, "status",
         "PC_MODS=alpha", "PC_MODS_DIR=" + root],
        capture_output=True, text=True, cwd=ROOT, timeout=180)
    out = r.stderr + r.stdout
    assert "modfs: 1 files, 0 members, order=[alpha]" in out, \
        "make status did not print the overlay line:\n%s" % out[-800:]

    r = boot_probe(root, "alpha", "modfs/probe.bin")
    out = r.stderr + r.stdout
    assert r.returncode == 0, out[-800:]
    assert ("modfs: probe modfs/probe.bin %d %s"
            % (len(blob), blob.hex())) in out, out[-800:]

    later = b"FROM-BETA\n"
    plant(os.path.join(root, "beta"), "replace/modfs/probe.bin", later)
    r = boot(root, "alpha,beta")
    out = r.stderr + r.stdout
    assert ("modfs: 'alpha' and 'beta' both replace modfs/probe.bin; "
            "later wins, 'alpha' loses") in out, out[-800:]
    r = boot_probe(root, "alpha,beta", "modfs/probe.bin")
    out = r.stderr + r.stdout
    assert r.returncode == 0, out[-800:]
    assert ("modfs: probe modfs/probe.bin %d %s"
            % (len(later), later.hex())) in out, out[-800:]

    cooked_blob = b"FROM-COOKED\n"
    plant(os.path.join(root, "alpha"), ".cooked/digest",
          cook_digest_bytes(os.path.join(root, "alpha")))
    plant(os.path.join(root, "alpha"), ".cooked/fs/modfs/probe.bin",
          cooked_blob)
    r = boot_probe(root, "alpha", "modfs/probe.bin")
    out = r.stderr + r.stdout
    assert r.returncode == 0, out[-800:]
    assert ("modfs: probe modfs/probe.bin %d %s"
            % (len(cooked_blob), cooked_blob.hex())) in out, out[-800:]

    loose = os.path.join(tmp, "loose")
    loose_blob = b"FROM-MODFS\n"
    plant(loose, "fs/modfs/probe.bin", loose_blob)
    r = boot_probe(root, "alpha", "modfs/probe.bin",
                   {"PC_MODFS": os.path.abspath(loose)})
    out = r.stderr + r.stdout
    assert r.returncode == 0, out[-800:]
    assert "later wins, 'alpha' loses" in out, out[-800:]
    assert ("modfs: probe modfs/probe.bin %d %s"
            % (len(loose_blob), loose_blob.hex())) in out, out[-800:]

    gone = os.path.join(root, "gone")
    write_toml(gone, id="gone", name="Gone", version="1.0")
    os.makedirs(os.path.join(gone, "replace", "modfs"), exist_ok=True)
    os.symlink("/no/such/modfs-missing",
               os.path.join(gone, "replace", "modfs", "gone.bin"))
    r = boot(root, "gone")
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "gone.bin" in out, out[-800:]

    # NARC member overlay. Scan claims (missing ROM is enough); bytes
    # come back through src/narc.c via PC_MODFS_PROBE_NARC. A hit never
    # opens the archive, so the fixture does not need the image.
    def boot_probe_narc(mods_dir, mods, probe, extra=None):
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e["PC_MODS_DIR"] = mods_dir
        e["PC_SAVE"] = "none"
        e["PC_ROM"] = os.path.join(tmp, "no-such.nds")
        e["PC_MODS"] = mods
        e["PC_MODFS_PROBE_NARC"] = probe
        if extra:
            e.update(extra)
        return subprocess.run([BINARY], env=e, capture_output=True, text=True,
                              timeout=120)

    narc_path = "poketool/pokegra/pl_pokegra.narc/0"
    narc_blob = b"MODFS-1.3-NARC\n"
    plant(os.path.join(root, "alpha"),
          "narc/poketool/pokegra/pl_pokegra.narc/0", narc_blob)
    r = boot(root, "alpha")
    out = r.stderr + r.stdout
    assert "modfs: 1 files, 1 members, order=[alpha]" in out, out[-800:]

    r = boot_probe_narc(root, "alpha", narc_path)
    out = r.stderr + r.stdout
    assert r.returncode == 0, out[-800:]
    assert ("modfs: probe-narc %s %d %s"
            % (narc_path, len(narc_blob), narc_blob.hex())) in out, out[-800:]

    narc_later = b"FROM-BETA-NARC\n"
    plant(os.path.join(root, "beta"),
          "narc/poketool/pokegra/pl_pokegra.narc/0", narc_later)
    r = boot(root, "alpha,beta")
    out = r.stderr + r.stdout
    assert ("modfs: 'alpha' and 'beta' both replace %s; "
            "later wins, 'alpha' loses" % narc_path) in out, out[-800:]
    r = boot_probe_narc(root, "alpha,beta", narc_path)
    out = r.stderr + r.stdout
    assert r.returncode == 0, out[-800:]
    assert ("modfs: probe-narc %s %d %s"
            % (narc_path, len(narc_later), narc_later.hex())) in out, out[-800:]

    narc_cooked = b"FROM-COOKED-NARC\n"
    plant(os.path.join(root, "alpha"), ".cooked/digest",
          cook_digest_bytes(os.path.join(root, "alpha")))
    plant(os.path.join(root, "alpha"),
          ".cooked/narc/poketool/pokegra/pl_pokegra.narc/0", narc_cooked)
    r = boot_probe_narc(root, "alpha", narc_path)
    out = r.stderr + r.stdout
    assert r.returncode == 0, out[-800:]
    assert ("modfs: probe-narc %s %d %s"
            % (narc_path, len(narc_cooked), narc_cooked.hex())) in out, out[-800:]

    loose_narc = b"FROM-MODFS-NARC\n"
    plant(loose, "narc/poketool/pokegra/pl_pokegra.narc/0", loose_narc)
    r = boot_probe_narc(root, "alpha", narc_path,
                        {"PC_MODFS": os.path.abspath(loose)})
    out = r.stderr + r.stdout
    assert r.returncode == 0, out[-800:]
    assert "later wins, 'alpha' loses" in out, out[-800:]
    assert ("modfs: probe-narc %s %d %s"
            % (narc_path, len(loose_narc), loose_narc.hex())) in out, out[-800:]

    gone_m = os.path.join(root, "gone-m")
    write_toml(gone_m, id="gonem", name="GoneM", version="1.0")
    os.makedirs(os.path.join(gone_m, "narc", "poketool", "pokegra",
                             "pl_pokegra.narc"), exist_ok=True)
    os.symlink("/no/such/modfs-member",
               os.path.join(gone_m, "narc", "poketool", "pokegra",
                            "pl_pokegra.narc", "0"))
    r = boot(root, "gone-m")
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "pl_pokegra.narc/0" in out, out[-800:]

    bad = os.path.join(root, "badidx")
    write_toml(bad, id="badidx", name="BadIdx", version="1.0")
    plant(bad, "narc/poketool/pokegra/pl_pokegra.narc/face", b"x")
    r = boot(root, "badidx")
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "not an index" in out, out[-800:]

    # NARC append. A planted index past the ROM FAT grows GetFileCount;
    # a hole or an index that will not fit in u16 numFiles refuses.
    # Member bytes still come back through the hook with no image.
    # Count needs the mounted ROM, same as --modfs-probe.
    append_path = "poketool/pokegra/pl_pokegra.narc/2964"
    append_blob = b"MODFS-1.4-APPEND\n"
    plant(os.path.join(root, "alpha"),
          "narc/poketool/pokegra/pl_pokegra.narc/2964", append_blob)
    r = boot_probe_narc(root, "alpha", append_path)
    out = r.stderr + r.stdout
    assert r.returncode == 0, out[-800:]
    assert ("modfs: probe-narc %s %d %s"
            % (append_path, len(append_blob), append_blob.hex())) in out, \
        out[-800:]

    over = os.path.join(root, "over")
    write_toml(over, id="over", name="Over", version="1.0")
    plant(over, "narc/poketool/pokegra/pl_pokegra.narc/65535", b"x")
    r = boot(root, "over")
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "65535" in out, out[-800:]

    def boot_probe_count(mods_dir, mods, probe, extra=None):
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e["PC_MODS_DIR"] = mods_dir
        e["PC_SAVE"] = "none"
        e["PC_MODS"] = mods
        e["PC_MODFS_PROBE_COUNT"] = probe
        e["PC_ROM"] = ROM
        if extra:
            e.update(extra)
        return subprocess.run([BINARY], env=e, capture_output=True, text=True,
                              timeout=120)

    # Cook. A planted PNG becomes a cooked narc member the running
    # port serves. A missing tool, a cook that wrote nothing, and a
    # digest that no longer matches content/ all refuse by name.
    cook_py = os.path.join(ROOT, "pc", "modcook.py")
    nitrogfx = os.path.join(ROOT, "build", "rom", "tools", "nitrogfx",
                            "nitrogfx")
    png_src = os.path.join(ROOT, "res", "pokemon", "piplup", "male_front.png")

    def run_cook(mods_dir, mods, extra=None, rom_build=None):
        e = os.environ.copy()
        cmd = [sys.executable, cook_py, "--mods-dir", mods_dir, "--mods", mods]
        if rom_build is not None:
            cmd.extend(["--rom-build", rom_build])
        if extra:
            e.update(extra)
        return subprocess.run(cmd, env=e, capture_output=True, text=True,
                              cwd=ROOT, timeout=120)

    emptycook = os.path.join(root, "emptycook")
    write_toml(emptycook, id="emptycook", name="Empty", version="1.0")
    os.makedirs(os.path.join(emptycook, "content"), exist_ok=True)
    r = run_cook(root, "emptycook")
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "wrote nothing" in out, out[-800:]

    r = run_cook(root, "")
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "no packages" in out, out[-800:]

    norecipe = os.path.join(root, "norecipe")
    write_toml(norecipe, id="norecipe", name="NoRecipe", version="1.0")
    plant(norecipe, "content/unknown.bin", b"{}\n")
    r = run_cook(root, "norecipe")
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "no recipe" in out, out[-800:]
    assert "unknown.bin" in out, out[-800:]

    cooked_ok = False
    if os.path.exists(nitrogfx) and os.path.exists(png_src):
        gamma = os.path.join(root, "gamma")
        write_toml(gamma, id="gamma", name="Gamma", version="1.0")
        plant(gamma, "content/narc/poketool/pokegra/pl_pokegra.narc/0.png",
              open(png_src, "rb").read())
        r = run_cook(root, "gamma")
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        cooked_member = os.path.join(gamma, ".cooked", "narc", "poketool",
                                     "pokegra", "pl_pokegra.narc", "0")
        assert os.path.isfile(cooked_member), "cook wrote no narc member"
        cooked_bytes = open(cooked_member, "rb").read()
        assert cooked_bytes[:4] == b"RGCN", cooked_bytes[:16]
        assert len(cooked_bytes) > 16
        assert open(os.path.join(gamma, ".cooked", "digest"), "rb").read() \
            == cook_digest_bytes(gamma)

        r = boot_probe_narc(root, "gamma",
                            "poketool/pokegra/pl_pokegra.narc/0")
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert ("modfs: probe-narc poketool/pokegra/pl_pokegra.narc/0 %d %s"
                % (len(cooked_bytes), cooked_bytes.hex())) in out, out[-800:]

        plant(gamma, "content/narc/poketool/pokegra/pl_pokegra.narc/0.png",
              open(png_src, "rb").read() + b"\x00")
        r = boot(root, "gamma")
        out = r.stderr + r.stdout
        assert r.returncode != 0 and "cook" in out, out[-800:]

        r = run_cook(root, "gamma",
                     rom_build=os.path.join(tmp, "no-rom-build"))
        out = r.stderr + r.stdout
        assert r.returncode != 0, out[-800:]
        assert "nitrogfx" in out and "0.png" in out, out[-800:]

        plant(gamma, "content/narc/poketool/pokegra/pl_pokegra.narc/0.png",
              open(png_src, "rb").read())
        r = subprocess.run(
            ["make", "-f", PC_MAKEFILE, "cook",
             "PC_MODS_DIR=" + root, "PC_MODS=gamma"],
            capture_output=True, text=True, cwd=ROOT, timeout=120)
        assert r.returncode == 0, (r.stderr or r.stdout)[-800:]
        assert os.path.isfile(cooked_member)
        cooked_ok = True

    # SPECIES_PIPLUP is 393 (generated/species.txt, SPECIES_NONE is 0).
    # src/pokemon.c BuildPokemonSpriteTemplate:
    #   character = species * 6 + face + (gender != GENDER_FEMALE ? 1 : 0)
    #   palette   = species * 6 + 4 + shiny
    # FACE_FRONT is 2; male adds 1. A wrong index that lands on
    # Turtwig (387*6 + 2 + 1 = 2325) is a failed task.
    SPECIES_PIPLUP = 393
    SPECIES_TURTWIG = 387
    PIPLUP_CHAR = SPECIES_PIPLUP * 6 + 2 + 1
    PIPLUP_PAL = SPECIES_PIPLUP * 6 + 4
    assert PIPLUP_CHAR == 2361, PIPLUP_CHAR
    assert PIPLUP_PAL == 2362, PIPLUP_PAL

    msgenc = os.path.join(ROOT, "build", "rom", "tools", "msgenc", "msgenc")
    key_src = os.path.join(ROOT, "res", "pokemon", "piplup", "male_front.png.key")
    pal_src = os.path.join(ROOT, "res", "pokemon", "piplup", "normal.pal")
    replace_ok = False
    newsmon_ok = False
    if (os.path.exists(nitrogfx) and os.path.exists(msgenc)
            and os.path.exists(png_src) and os.path.exists(key_src)
            and os.path.exists(pal_src)):
        face = os.path.join(root, "replaceface")
        write_toml(face, id="replaceface", name="ReplaceFace", version="1.0")
        poke_dir = os.path.join(face, "content", "pokemon", "piplup")
        os.makedirs(poke_dir, exist_ok=True)
        os.makedirs(os.path.join(face, "content", "text"), exist_ok=True)
        w, h, plte, indices = png_indexed4_decode(
            open(png_src, "rb").read(), "male_front.png")
        plte = bytearray(plte)
        plte[15 * 3:15 * 3 + 3] = b"\xff\x00\xff"
        indices = bytearray(indices)
        for y in range(8, 24):
            for x in range(8, 24):
                indices[y * w + x] = 15
        png_indexed4_write(os.path.join(poke_dir, "male_front.png"),
                           w, h, bytes(plte), bytes(indices))
        shutil.copy(key_src, os.path.join(poke_dir, "male_front.png.key"))
        pal = open(pal_src, "rb").read()
        if pal.endswith(b"255 255 255\r\n"):
            pal = pal[:-len(b"255 255 255\r\n")] + b"255 0 255\r\n"
        else:
            pal = pal.replace(b"255 255 255", b"255 0 255", 1)
        open(os.path.join(poke_dir, "normal.pal"), "wb").write(pal)
        plant(face, "content/text/dummy_0215.json",
              b'{\n  "key": 12345,\n  "messages": [\n'
              b'    {"id": "Modsys_Planted", "en_US": "PLANTED BANK"}\n'
              b'  ]\n}\n')
        # Content-only: no .c anywhere in the package.
        for dirpath, _, files in os.walk(face):
            for name in files:
                assert not name.endswith(".c"), dirpath

        r = run_cook(root, "replaceface")
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        cooked_char = os.path.join(face, ".cooked", "narc", "poketool",
                                   "pokegra", "pl_pokegra.narc", "2361")
        cooked_pal = os.path.join(face, ".cooked", "narc", "poketool",
                                  "pokegra", "pl_pokegra.narc", "2362")
        cooked_bank = os.path.join(face, ".cooked", "narc", "msgdata",
                                   "pl_msg.narc", "215")
        assert os.path.isfile(cooked_char), "cook did not write 2361"
        assert os.path.isfile(cooked_pal), "cook did not write 2362"
        assert os.path.isfile(cooked_bank), "cook did not write pl_msg/215"
        assert open(cooked_char, "rb").read()[:4] == b"RGCN"
        assert open(cooked_pal, "rb").read()[:4] == b"RLCN"
        bank_bytes = open(cooked_bank, "rb").read()
        assert len(bank_bytes) > 0

        # Missing key is a cook error that names the key, not a silent
        # encode without the scramble.
        nokey = os.path.join(root, "nokey")
        write_toml(nokey, id="nokey", name="NoKey", version="1.0")
        nd = os.path.join(nokey, "content", "pokemon", "piplup")
        os.makedirs(nd, exist_ok=True)
        shutil.copy(os.path.join(poke_dir, "male_front.png"),
                    os.path.join(nd, "male_front.png"))
        shutil.copy(os.path.join(poke_dir, "normal.pal"),
                    os.path.join(nd, "normal.pal"))
        r = run_cook(root, "nokey")
        out = r.stderr + r.stdout
        assert r.returncode != 0 and "male_front.png.key" in out, out[-800:]

        replace_ok = True

    # Records + a new species. 494 is SPECIES_EGG (pl_otherpoke
    # char=132); the first free C id is 496. Auto-allocate past
    # BAD_EGG, freeze in ids.toml, cook every face, fill the
    # pokegra hole so GetFileCount can grow.
    SPECIES_NEW = 496
    NEW_CHAR = SPECIES_NEW * 6 + 2 + 1
    NEW_PAL = SPECIES_NEW * 6 + 4
    NEW_COUNT = SPECIES_NEW * 6 + 6
    assert NEW_CHAR == 2979, NEW_CHAR
    assert NEW_PAL == 2980, NEW_PAL
    assert NEW_COUNT == 2982, NEW_COUNT
    newsmon_ok = False
    geninclude = os.path.join(ROOT, "build", "pc", "geninclude",
                              "generated", "species.h")
    gen_stat = os.stat(geninclude) if os.path.exists(geninclude) else None
    if (os.path.exists(nitrogfx) and os.path.exists(png_src)
            and os.path.exists(key_src) and os.path.exists(pal_src)):
        sprig = os.path.join(root, "sprig")
        write_toml(sprig, id="coolhouse", name="Sprig", version="1.0")
        sd = os.path.join(sprig, "content", "pokemon", "sprigatito")
        os.makedirs(sd, exist_ok=True)
        os.makedirs(os.path.join(sprig, "records", "species"), exist_ok=True)
        os.makedirs(os.path.join(sprig, "records", "props"), exist_ok=True)
        open(os.path.join(sprig, "records", "species", "sprigatito.json"),
             "w").write("{}\n")
        open(os.path.join(sprig, "records", "props", "cool_house.json"),
             "w").write("{}\n")
        donor = os.path.join(ROOT, "res", "pokemon", "piplup")
        for face in ("female_back", "male_back", "female_front",
                     "male_front"):
            shutil.copy(os.path.join(donor, face + ".png"),
                        os.path.join(sd, face + ".png"))
            shutil.copy(os.path.join(donor, face + ".png.key"),
                        os.path.join(sd, face + ".png.key"))
        w, h, plte, indices = png_indexed4_decode(
            open(os.path.join(sd, "male_front.png"), "rb").read(),
            "male_front.png")
        plte = bytearray(plte)
        plte[15 * 3:15 * 3 + 3] = b"\xff\x00\xff"
        indices = bytearray(indices)
        for y in range(8, 24):
            for x in range(8, 24):
                indices[y * w + x] = 15
        png_indexed4_write(os.path.join(sd, "male_front.png"),
                           w, h, bytes(plte), bytes(indices))
        pal = open(pal_src, "rb").read()
        if pal.endswith(b"255 255 255\r\n"):
            pal = pal[:-len(b"255 255 255\r\n")] + b"255 0 255\r\n"
        else:
            pal = pal.replace(b"255 255 255", b"255 0 255", 1)
        open(os.path.join(sd, "normal.pal"), "wb").write(pal)
        shutil.copy(os.path.join(donor, "shiny.pal"),
                    os.path.join(sd, "shiny.pal"))
        for dirpath, _, files in os.walk(sprig):
            for name in files:
                assert not name.endswith(".c"), dirpath

        r = run_cook(root, "sprig")
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "no rebuild" in out, out[-800:]
        assert "496" in out, out[-800:]
        ids_path = os.path.join(sprig, ".cooked", "ids.toml")
        ids_txt = open(ids_path).read()
        assert '"coolhouse:species/sprigatito" = 496' in ids_txt, ids_txt
        assert '"coolhouse:prop/cool_house" = 590' in ids_txt, ids_txt
        cooked_root = os.path.join(sprig, ".cooked", "narc", "poketool",
                                   "pokegra", "pl_pokegra.narc")
        for idx in range(2964, 2982):
            p = os.path.join(cooked_root, str(idx))
            assert os.path.isfile(p), "cook did not write pokegra/%d" % idx
        assert open(os.path.join(cooked_root, "2979"), "rb").read()[:4] == b"RGCN"
        assert open(os.path.join(cooked_root, "2980"), "rb").read()[:4] == b"RLCN"
        assert os.stat(os.path.join(cooked_root, "2964")).st_size == 0

        # Second cook keeps the frozen numbers.
        r = run_cook(root, "sprig")
        assert r.returncode == 0, (r.stderr or r.stdout)[-800:]
        assert '"coolhouse:species/sprigatito" = 496' in open(ids_path).read()

        # Lock vs pin.
        open(os.path.join(sprig, "records", "species", "sprigatito.json"),
             "w").write('{"id": 650}\n')
        r = run_cook(root, "sprig")
        out = r.stderr + r.stdout
        assert r.returncode != 0 and "lock collision" in out, out[-800:]
        open(os.path.join(sprig, "records", "species", "sprigatito.json"),
             "w").write("{}\n")

        # Pin 494 is SPECIES_EGG, not a new species.
        pin494 = os.path.join(root, "pin494")
        write_toml(pin494, id="pin494", name="Pin494", version="1.0")
        os.makedirs(os.path.join(pin494, "records", "species"), exist_ok=True)
        open(os.path.join(pin494, "records", "species", "sprigatito.json"),
             "w").write('{"id": 494}\n')
        r = run_cook(root, "pin494")
        out = r.stderr + r.stdout
        assert r.returncode != 0 and "egg" in out.lower(), out[-800:]

        # Server-authored id.
        pin650 = os.path.join(root, "pin650")
        write_toml(pin650, id="pin650", name="Pin650", version="1.0")
        os.makedirs(os.path.join(pin650, "records", "species"), exist_ok=True)
        open(os.path.join(pin650, "records", "species", "sprigatito.json"),
             "w").write('{"id": 650}\n')
        r = run_cook(root, "pin650")
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert '"pin650:species/sprigatito" = 650' in open(
            os.path.join(pin650, ".cooked", "ids.toml")).read()

        # Content without a record is still a cook error.
        orphan = os.path.join(root, "orphan")
        write_toml(orphan, id="orphan", name="Orphan", version="1.0")
        od = os.path.join(orphan, "content", "pokemon", "sprigatito")
        os.makedirs(od, exist_ok=True)
        shutil.copy(os.path.join(sd, "male_front.png"),
                    os.path.join(od, "male_front.png"))
        shutil.copy(os.path.join(sd, "male_front.png.key"),
                    os.path.join(od, "male_front.png.key"))
        shutil.copy(os.path.join(sd, "normal.pal"),
                    os.path.join(od, "normal.pal"))
        r = run_cook(root, "orphan")
        out = r.stderr + r.stdout
        assert r.returncode != 0 and "sprigatito" in out, out[-800:]

        if gen_stat is not None:
            g2 = os.stat(geninclude)
            assert (g2.st_mtime_ns, g2.st_size) == (
                gen_stat.st_mtime_ns, gen_stat.st_size), \
                "cook wrote vanilla build/pc/geninclude"
        newsmon_ok = True

    # Billboard person. records/gfx allocates 276 (implicit rows
    # 0..275). PNG -> nitrobtx onto stock generic_32x32 (mmodel 470).
    # Events JSON overlays Jubilife zone_event member 2. No .c.
    nitrobtx = os.path.join(ROOT, "build", "rom", "tools", "nitrobtx",
                            "nitrobtx")
    eventpy = os.path.join(ROOT, "tools", "jsoncnv", "event.py")
    person_ok = False
    GFX_NEW = 276
    MMODEL_NEW = 470
    JUBILIFE_EVENTS = 2
    if os.path.exists(nitrobtx) and os.path.exists(eventpy):
        person = os.path.join(root, "person")
        write_toml(person, id="coolhouse", name="Person", version="1.0")
        os.makedirs(os.path.join(person, "content", "people"), exist_ok=True)
        os.makedirs(os.path.join(person, "content", "events"), exist_ok=True)
        os.makedirs(os.path.join(person, "records", "gfx"), exist_ok=True)
        open(os.path.join(person, "records", "gfx",
                          "magenta_person.json"), "w").write("{}\n")
        # decomp NPC sheet is 32x512, 4-bit, 16 frames of 32. Index
        # 15 = magenta, every pixel, so every facing is the plant.
        w, h = 32, 512
        plte = bytearray(48)
        plte[15 * 3:15 * 3 + 3] = b"\xff\x00\xff"
        indices = bytes([15] * (w * h))
        png_indexed4_write(os.path.join(person, "content", "people",
                                        "magenta_person.png"),
                           w, h, bytes(plte), indices)
        ev = json.loads(open(os.path.join(
            ROOT, "res", "field", "events",
            "events_jubilife_city.json")).read())
        ev["object_events"].append({
            "id": "LOCALID_MAGENTA_PERSON",
            "graphics_id": "OBJ_EVENT_GFX_MAGENTA_PERSON",
            "movement_type": "MOVEMENT_TYPE_NONE",
            "trainer_type": "TRAINER_TYPE_NONE",
            "hidden_flag": "0",
            "script": 0,
            "initial_dir": 1,
            "data": [],
            "movement_range_x": 0,
            "movement_range_z": 0,
            "x": 182,
            "z": 777,
            "y": 0,
        })
        open(os.path.join(person, "content", "events",
                          "jubilife_city.json"), "w").write(
            json.dumps(ev, indent=2) + "\n")
        for dirpath, _, files in os.walk(person):
            for name in files:
                assert not name.endswith(".c"), dirpath

        r = run_cook(root, "person")
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "no rebuild" in out, out[-800:]
        assert str(GFX_NEW) in out and str(MMODEL_NEW) in out, out[-800:]
        ids_txt = open(os.path.join(person, ".cooked", "ids.toml")).read()
        assert '"coolhouse:gfx/magenta_person" = %d' % GFX_NEW in ids_txt, \
            ids_txt
        cooked_nsbtx = os.path.join(person, ".cooked", "narc", "data",
                                    "mmodel", "mmodel.narc", str(MMODEL_NEW))
        cooked_ev = os.path.join(person, ".cooked", "narc", "fielddata",
                                 "eventdata", "zone_event.narc",
                                 str(JUBILIFE_EVENTS))
        assert os.path.isfile(cooked_nsbtx), "cook did not write mmodel/470"
        assert os.path.isfile(cooked_ev), "cook did not write zone_event/2"
        assert os.stat(cooked_nsbtx).st_size > 0
        assert os.stat(cooked_ev).st_size > 0
        side = open(os.path.join(person, ".cooked", "generated",
                                 "billboard_gfx.txt")).read()
        assert "%d %d" % (GFX_NEW, MMODEL_NEW) in side, side
        person_ok = True

    # Prebuilt prop + land_data. Dummy-box NSBMD as prop 590 (meson
    # list is 0..589). Jubilife land_data member 15 gets one extra
    # MapProp. records/maps/hub clones map_data_015 as member 666
    # with a 1x1 matrix 289 and header 593. No .c.
    PROP_NEW = 590
    LAND_JUBILIFE = 15
    LAND_NEW = 666
    MATRIX_NEW = 289
    MAP_NEW = 593
    house_ok = False
    dummy_nsbmd = os.path.join(ROOT, "res", "field", "props", "models",
                               "prop_model_000.nsbmd")
    land015 = os.path.join(ROOT, "res", "field", "maps", "data",
                           "map_data_015.bin")
    if os.path.isfile(dummy_nsbmd) and os.path.isfile(land015):
        house = os.path.join(root, "house")
        write_toml(house, id="coolhouse", name="House", version="1.0")
        os.makedirs(os.path.join(house, "content", "props"), exist_ok=True)
        os.makedirs(os.path.join(house, "content", "maps", "hub"),
                    exist_ok=True)
        os.makedirs(os.path.join(house, "content", "maps", "15"),
                    exist_ok=True)
        os.makedirs(os.path.join(house, "records", "props"), exist_ok=True)
        os.makedirs(os.path.join(house, "records", "maps"), exist_ok=True)
        open(os.path.join(house, "records", "props", "cool_house.json"),
             "w").write("{}\n")
        open(os.path.join(house, "records", "maps", "hub.json"),
             "w").write("{}\n")
        shutil.copy(dummy_nsbmd,
                    os.path.join(house, "content", "props",
                                 "cool_house.nsbmd"))
        land = open(land015, "rb").read()
        attr, props_sz, model_sz, bdhc_sz = struct.unpack_from("<4I", land, 0)
        fx = 4096
        # Player at local (20,9) is (64, 32, -112) in map-local FX32.
        # One tile south is z=-64. Scale 2: a white cube in the plaza.
        extra = struct.pack("<i3i3i3i2I",
                            PROP_NEW,
                            64 * fx, 32 * fx, -64 * fx,
                            0, 0, 0,
                            2 * fx, 2 * fx, 2 * fx,
                            0, 0)
        overlay = (struct.pack("<4I", attr, props_sz + 48, model_sz,
                               bdhc_sz)
                   + land[16:16 + attr + props_sz]
                   + extra
                   + land[16 + attr + props_sz:])
        open(os.path.join(house, "content", "maps", "15",
                          "land_data.bin"), "wb").write(overlay)
        shutil.copy(land015,
                    os.path.join(house, "content", "maps", "hub",
                                 "land_data.bin"))
        for dirpath, _, files in os.walk(house):
            for name in files:
                assert not name.endswith(".c"), dirpath

        r = run_cook(root, "house")
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "no rebuild" in out, out[-800:]
        ids_txt = open(os.path.join(house, ".cooked", "ids.toml")).read()
        assert '"coolhouse:prop/cool_house" = %d' % PROP_NEW in ids_txt, \
            ids_txt
        assert '"coolhouse:map/hub" = %d' % MAP_NEW in ids_txt, ids_txt
        cooked_prop = os.path.join(house, ".cooked", "narc", "fielddata",
                                   "build_model", "build_model.narc",
                                   str(PROP_NEW))
        cooked_land = os.path.join(house, ".cooked", "narc", "fielddata",
                                   "land_data", "land_data.narc",
                                   str(LAND_JUBILIFE))
        cooked_hub = os.path.join(house, ".cooked", "narc", "fielddata",
                                  "land_data", "land_data.narc",
                                  str(LAND_NEW))
        cooked_mx = os.path.join(house, ".cooked", "narc", "fielddata",
                                 "mapmatrix", "map_matrix.narc",
                                 str(MATRIX_NEW))
        assert os.path.isfile(cooked_prop), "cook did not write build_model/590"
        assert os.path.isfile(cooked_land), "cook did not write land_data/15"
        assert os.path.isfile(cooked_hub), "cook did not write land_data/666"
        assert os.path.isfile(cooked_mx), "cook did not write map_matrix/289"
        assert open(cooked_prop, "rb").read()[:4] == b"BMD0"
        extras = open(os.path.join(house, ".cooked", "generated",
                                   "extra_props.txt")).read()
        assert str(PROP_NEW) in extras, extras
        maps_txt = open(os.path.join(house, ".cooked", "generated",
                                     "cooked_maps.txt")).read()
        assert str(MAP_NEW) in maps_txt and str(MATRIX_NEW) in maps_txt, \
            maps_txt
        house_ok = True

    # Hub map from source. terrain.json + model.gltf pack the
    # land_data member (not a hex-edited map_data_015). Dummy-box
    # still sits as MapProp 590. Events, scripts, text ride along.
    # No .c.
    EVENTS_NEW = 534
    SCRIPTS_NEW = 1124
    INIT_NEW = 1125
    MSG_NEW = 724
    hub_ok = False
    enumproc = os.path.join(ROOT, "build", "rom", "tools", "enumproc",
                            "enumproc")
    nitromdl = os.path.join(ROOT, "build", "rom", "tools", "nitromdl",
                            "nitromdl")

    def write_floor_gltf(path):
        import base64
        verts = [-256, 0, -256, 256, 0, -256, 256, 0, 256, -256, 0, 256]
        idx = [0, 1, 2, 0, 2, 3]
        cols = [0.0, 0.0, 0.0] * 4
        pos_b = struct.pack("<%df" % len(verts), *verts)
        idx_b = struct.pack("<%dH" % len(idx), *idx)
        col_b = struct.pack("<%df" % len(cols), *cols)
        blob = pos_b + idx_b + col_b
        uri = "data:application/octet-stream;base64," + \
            base64.b64encode(blob).decode("ascii")
        gltf = {
            "asset": {"version": "2.0"},
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0}],
            "meshes": [{"name": "floor", "primitives": [{
                "attributes": {"POSITION": 0, "COLOR_0": 2},
                "indices": 1, "mode": 4}]}],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 4,
                 "type": "VEC3", "min": [-256, 0, -256],
                 "max": [256, 0, 256]},
                {"bufferView": 1, "componentType": 5123, "count": 6,
                 "type": "SCALAR"},
                {"bufferView": 2, "componentType": 5126, "count": 4,
                 "type": "VEC3"},
            ],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_b)},
                {"buffer": 0, "byteOffset": len(pos_b),
                 "byteLength": len(idx_b)},
                {"buffer": 0, "byteOffset": len(pos_b) + len(idx_b),
                 "byteLength": len(col_b)},
            ],
            "buffers": [{"byteLength": len(blob), "uri": uri}],
        }
        open(path, "w").write(json.dumps(gltf, indent=2) + "\n")

    if (os.path.isfile(dummy_nsbmd) and os.path.isfile(nitromdl)
            and shutil.which("arm-none-eabi-gcc")
            and os.path.isfile(enumproc)
            and os.path.exists(msgenc)
            and os.path.exists(eventpy)):
        hub = os.path.join(root, "hubmap")
        write_toml(hub, id="coolhouse", name="Hub", version="1.0")
        os.makedirs(os.path.join(hub, "content", "props"), exist_ok=True)
        os.makedirs(os.path.join(hub, "content", "maps", "hub"),
                    exist_ok=True)
        os.makedirs(os.path.join(hub, "records", "props"), exist_ok=True)
        os.makedirs(os.path.join(hub, "records", "maps"), exist_ok=True)
        open(os.path.join(hub, "records", "props", "cool_house.json"),
             "w").write("{}\n")
        open(os.path.join(hub, "records", "maps", "hub.json"),
             "w").write('{"area": 6, "map_type": 1, "window": 1, '
                        '"battle_bg": 2, "bike": 1, "run": 1, "fly": 1}\n')
        shutil.copy(dummy_nsbmd,
                    os.path.join(hub, "content", "props",
                                 "cool_house.nsbmd"))
        write_floor_gltf(os.path.join(hub, "content", "maps", "hub",
                                      "model.gltf"))
        open(os.path.join(hub, "content", "maps", "hub", "terrain.json"),
             "w").write(json.dumps({
                 "props": [{
                     "model": "cool_house",
                     "x": 64, "y": 32, "z": -64, "scale": 2,
                 }],
             }, indent=2) + "\n")
        open(os.path.join(hub, "content", "maps", "hub", "events.json"),
             "w").write(json.dumps({
                 "bg_events": [],
                 "object_events": [{
                     "id": "LOCALID_HUB_SIGN",
                     "graphics_id": "OBJ_EVENT_GFX_YOUNGSTER",
                     "movement_type": "MOVEMENT_TYPE_NONE",
                     "trainer_type": "TRAINER_TYPE_NONE",
                     "hidden_flag": "0",
                     "script": 2,
                     "initial_dir": 0,
                     "data": [],
                     "movement_range_x": 0,
                     "movement_range_z": 0,
                     "x": 20,
                     "z": 10,
                     "y": 0,
                 }],
                 "warp_events": [{
                     "x": 20,
                     "z": 8,
                     "dest_header_id": "MAP_HEADER_JUBILIFE_CITY",
                     "dest_warp_id": 0,
                 }],
                 "coord_events": [],
             }, indent=2) + "\n")
        open(os.path.join(hub, "content", "maps", "hub", "scripts.s"),
             "w").write(
            '#include "macros/scrcmd.inc"\n\n'
            "    ScriptEntry Hub_OnTransition\n"
            "    ScriptEntry Hub_Talk\n"
            "    ScriptEntryEnd\n\n"
            "Hub_OnTransition:\n"
            "    SetTrainerFlag 1\n"
            "    End\n\n"
            "Hub_Talk:\n"
            "    End\n")
        open(os.path.join(hub, "content", "maps", "hub", "text.json"),
             "w").write(
            '{\n  "key": 1,\n  "messages": [\n'
            '    {"id": "Hub_Text_Hello", "en_US": "HUB MAP"}\n'
            "  ]\n}\n")
        for dirpath, _, files in os.walk(hub):
            for name in files:
                assert not name.endswith(".c"), dirpath

        r = run_cook(root, "hubmap")
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "no rebuild" in out, out[-800:]
        ids_txt = open(os.path.join(hub, ".cooked", "ids.toml")).read()
        assert '"coolhouse:map/hub" = %d' % MAP_NEW in ids_txt, ids_txt
        cooked_ev = os.path.join(hub, ".cooked", "narc", "fielddata",
                                 "eventdata", "zone_event.narc",
                                 str(EVENTS_NEW))
        cooked_sc = os.path.join(hub, ".cooked", "narc", "fielddata",
                                 "script", "scr_seq.narc",
                                 str(SCRIPTS_NEW))
        cooked_in = os.path.join(hub, ".cooked", "narc", "fielddata",
                                 "script", "scr_seq.narc",
                                 str(INIT_NEW))
        cooked_msg = os.path.join(hub, ".cooked", "narc", "msgdata",
                                  "pl_msg.narc", str(MSG_NEW))
        cooked_hub = os.path.join(hub, ".cooked", "narc", "fielddata",
                                  "land_data", "land_data.narc",
                                  str(LAND_NEW))
        assert os.path.isfile(cooked_ev), "cook did not write zone_event/534"
        assert os.path.isfile(cooked_sc), "cook did not write scr_seq/1124"
        assert os.path.isfile(cooked_in), "cook did not write scr_seq/1125"
        assert os.path.isfile(cooked_msg), "cook did not write pl_msg/724"
        assert os.path.isfile(cooked_hub), "cook did not write land_data/666"
        packed = open(cooked_hub, "rb").read()
        attr, props_sz, model_sz, bdhc_sz = struct.unpack_from(
            "<4I", packed, 0)
        assert attr == 2048, attr
        assert props_sz == 48, props_sz
        model_off = 16 + attr + props_sz
        assert packed[model_off:model_off + 4] == b"BMD0"
        bdhc_off = model_off + model_sz
        assert packed[bdhc_off:bdhc_off + 4] == b"BDHC"
        # From source, not a hex-edited Jubilife cell.
        assert len(packed) < 8000, len(packed)
        maps_txt = open(os.path.join(hub, ".cooked", "generated",
                                     "cooked_maps.txt")).read()
        assert ("%d 6 0 %d %d %d %d" %
                (MAP_NEW, MATRIX_NEW, SCRIPTS_NEW, INIT_NEW, MSG_NEW)) \
            in maps_txt, maps_txt
        assert " %d " % EVENTS_NEW in maps_txt, maps_txt

        missing = os.path.join(root, "nomodel")
        write_toml(missing, id="nomodel", name="NoModel", version="1.0")
        os.makedirs(os.path.join(missing, "content", "maps", "hub"),
                    exist_ok=True)
        os.makedirs(os.path.join(missing, "records", "maps"), exist_ok=True)
        open(os.path.join(missing, "records", "maps", "hub.json"),
             "w").write("{}\n")
        open(os.path.join(missing, "content", "maps", "hub",
                          "terrain.json"), "w").write("{}\n")
        r = run_cook(root, "nomodel")
        out = r.stderr + r.stdout
        assert r.returncode != 0, "terrain.json without model should refuse"
        assert "model.gltf" in out, out[-400:]

        slope = os.path.join(root, "slope")
        write_toml(slope, id="slope", name="Slope", version="1.0")
        os.makedirs(os.path.join(slope, "content", "maps", "hub"),
                    exist_ok=True)
        os.makedirs(os.path.join(slope, "records", "maps"), exist_ok=True)
        open(os.path.join(slope, "records", "maps", "hub.json"),
             "w").write("{}\n")
        shutil.copy(os.path.join(hub, "content", "maps", "hub",
                                 "model.gltf"),
                    os.path.join(slope, "content", "maps", "hub",
                                 "model.gltf"))
        open(os.path.join(slope, "content", "maps", "hub",
                          "terrain.json"), "w").write(json.dumps({
                              "bdhc": {"plates": [{
                                  "x0": -256, "z0": -256,
                                  "x1": 256, "z1": 256,
                                  "y0": 0, "y1": 16,
                              }]},
                          }) + "\n")
        r = run_cook(root, "slope")
        out = r.stderr + r.stdout
        assert r.returncode != 0, "sloped plate should refuse"
        assert "sloped" in out, out[-400:]
        hub_ok = True

    cook_claim = (
        "a planted PNG cooks to a narc member the running port returns, "
        "a missing nitrogfx names the file, an empty content/ writes "
        "nothing, and a stale digest names make cook"
        if cooked_ok else
        "cook refuses an empty content/ and a file with no recipe; "
        "PNG smoke skipped (no nitrogfx)")
    if replace_ok:
        cook_claim += (
            "; Piplup front-male cooks to narc members 2361 and 2362 "
            "(393*6+2+1, 393*6+4) and dummy_0215.json to pl_msg/215")
    else:
        cook_claim += "; sprite+text replace skipped (no nitrogfx/msgenc)"
    if newsmon_ok:
        cook_claim += (
            "; records allocate coolhouse:species/sprigatito -> 496 "
            "(494 is vanilla egg) and coolhouse:prop/cool_house -> 590, "
            "ids.toml freezes them, a lock collision and a pin of 494 "
            "refuse, a pin of 650 binds, and all six pl_pokegra members "
            "plus the 2964..2975 hole cook")
    else:
        cook_claim += "; new-species records skipped (no nitrogfx)"
    if person_ok:
        cook_claim += (
            "; records allocate coolhouse:gfx/magenta_person -> 276 "
            "and cook the PNG to mmodel/470 plus Jubilife zone_event/2")
    else:
        cook_claim += "; billboard person skipped (no nitrobtx)"
    if house_ok:
        cook_claim += (
            "; records allocate coolhouse:prop/cool_house -> 590 and "
            "coolhouse:map/hub -> 593 (land 666 matrix 289) and cook "
            "the prebuilt NSBMD to build_model/590 plus Jubilife "
            "land_data/15")
    else:
        cook_claim += "; prebuilt prop skipped (no dummy-box NSBMD)"

    # glTF box through nitromdl, same Jubilife placement as the prebuilt
    # dummy-box. Magenta COLOR_0 so the frame is not "another white cube".
    # No .c. Skip if nitromdl is not in the ROM build.
    nitromdl = os.path.join(ROOT, "build", "rom", "tools", "nitromdl",
                            "nitromdl")
    gltf_ok = False
    if os.path.isfile(nitromdl) and os.path.isfile(land015):
        import base64
        box = os.path.join(root, "gltfbox")
        write_toml(box, id="gltfbox", name="GltfBox", version="1.0")
        os.makedirs(os.path.join(box, "content", "props"), exist_ok=True)
        os.makedirs(os.path.join(box, "content", "maps", "15"), exist_ok=True)
        os.makedirs(os.path.join(box, "records", "props"), exist_ok=True)
        open(os.path.join(box, "records", "props", "cool_house.json"),
             "w").write("{}\n")
        verts = [-2, 0, -2, 2, 0, -2, 2, 0, 2, -2, 0, 2,
                 -2, 4, -2, 2, 4, -2, 2, 4, 2, -2, 4, 2]
        idx = [0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
               0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7,
               0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2]
        cols = [1.0, 0.0, 1.0] * 8
        pos_b = struct.pack("<%df" % len(verts), *verts)
        idx_b = struct.pack("<%dH" % len(idx), *idx)
        col_b = struct.pack("<%df" % len(cols), *cols)
        blob = pos_b + idx_b + col_b
        uri = "data:application/octet-stream;base64," + \
            base64.b64encode(blob).decode("ascii")
        gltf = {
            "asset": {"version": "2.0"},
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0, "translation": [0, 8, 0]}],
            "meshes": [{"name": "box", "primitives": [{
                "attributes": {"POSITION": 0, "COLOR_0": 2},
                "indices": 1, "mode": 4}]}],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 8,
                 "type": "VEC3", "min": [-2, 0, -2], "max": [2, 4, 2]},
                {"bufferView": 1, "componentType": 5123, "count": 36,
                 "type": "SCALAR"},
                {"bufferView": 2, "componentType": 5126, "count": 8,
                 "type": "VEC3"},
            ],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_b)},
                {"buffer": 0, "byteOffset": len(pos_b),
                 "byteLength": len(idx_b)},
                {"buffer": 0, "byteOffset": len(pos_b) + len(idx_b),
                 "byteLength": len(col_b)},
            ],
            "buffers": [{"byteLength": len(blob), "uri": uri}],
        }
        open(os.path.join(box, "content", "props", "cool_house.gltf"),
             "w").write(json.dumps(gltf, indent=2) + "\n")
        land = open(land015, "rb").read()
        attr, props_sz, model_sz, bdhc_sz = struct.unpack_from("<4I", land, 0)
        fx = 4096
        extra = struct.pack("<i3i3i3i2I",
                            PROP_NEW,
                            64 * fx, 32 * fx, -64 * fx,
                            0, 0, 0,
                            4 * fx, 4 * fx, 4 * fx,
                            0, 0)
        overlay = (struct.pack("<4I", attr, props_sz + 48, model_sz,
                               bdhc_sz)
                   + land[16:16 + attr + props_sz]
                   + extra
                   + land[16 + attr + props_sz:])
        open(os.path.join(box, "content", "maps", "15",
                          "land_data.bin"), "wb").write(overlay)
        for dirpath, _, files in os.walk(box):
            for name in files:
                assert not name.endswith(".c"), dirpath

        r = run_cook(root, "gltfbox")
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "no rebuild" in out, out[-800:]
        cooked_prop = os.path.join(box, ".cooked", "narc", "fielddata",
                                   "build_model", "build_model.narc",
                                   str(PROP_NEW))
        assert os.path.isfile(cooked_prop), "cook did not write build_model/590"
        cooked = open(cooked_prop, "rb").read()
        assert cooked[:4] == b"BMD0" and b"MDL0" in cooked
        extras = open(os.path.join(box, ".cooked", "generated",
                                   "extra_props.txt")).read()
        assert str(PROP_NEW) in extras, extras

        tex = os.path.join(root, "gltftex")
        write_toml(tex, id="gltftex", name="GltfTex", version="1.0")
        os.makedirs(os.path.join(tex, "content", "props"), exist_ok=True)
        os.makedirs(os.path.join(tex, "records", "props"), exist_ok=True)
        open(os.path.join(tex, "records", "props", "cool_house.json"),
             "w").write("{}\n")
        bad = json.loads(open(os.path.join(box, "content", "props",
                                           "cool_house.gltf")).read())
        bad["images"] = [{"uri": "x.png"}]
        open(os.path.join(tex, "content", "props", "cool_house.gltf"),
             "w").write(json.dumps(bad) + "\n")
        r = run_cook(root, "gltftex")
        out = r.stderr + r.stdout
        assert r.returncode != 0, "textured glTF should refuse"
        assert "TEX0" in out or "texture" in out, out[-400:]
        gltf_ok = True

    if gltf_ok:
        cook_claim += (
            "; content/props/cool_house.gltf cooks through nitromdl to "
            "build_model/590 and a textured glTF names TEX0")
    else:
        cook_claim += "; glTF prop skipped (no nitromdl)"
    if hub_ok:
        cook_claim += (
            "; content/maps/hub/{terrain.json,model.gltf,events,scripts,text} "
            "cook to zone_event/534, scr_seq/1124+1125, pl_msg/724, "
            "land_data/666 from source; a missing model.gltf and a sloped "
            "BDHC plate refuse by name")
    else:
        cook_claim += "; hub map cook skipped (no assembler/msgenc/nitromdl)"

    # Windows: the same overlay, driven only by KEY=VALUE argv (WSL
    # does not forward PC_*). A tiny PE links the Makefile.win
    # pc_modfs.o + pc_args.o so a full exe is not required. Fixture
    # is repo-relative so the PE cwd can see it. Created and deleted
    # here; no Nintendo bytes.
    win_claim = ""
    win_obj = os.path.join(ROOT, "build", "pc-win32", "obj", "pc",
                           "pc_modfs.o")
    win_args = os.path.join(ROOT, "build", "pc-win32", "obj", "pc",
                            "pc_args.o")
    r = subprocess.run(["make", "-f", WIN_MAKEFILE, win_obj, win_args],
                       capture_output=True, text=True, cwd=ROOT,
                       timeout=180)
    assert r.returncode == 0, (
        "Makefile.win did not build pc_modfs.o:\n%s"
        % (r.stderr or r.stdout)[-800:])
    assert os.path.isfile(win_obj), "Makefile.win produced no pc_modfs.o"
    mingw = shutil.which("i686-w64-mingw32-gcc")
    if mingw is None:
        win_claim = (
            "; Makefile.win builds pc_modfs.o; PE probe skipped "
            "(no i686-w64-mingw32-gcc)")
    else:
        win_root = os.path.join(ROOT, "build", "pc-win32", "zz-mods")
        win_src = os.path.join(tmp, "modfs-win-smoke.c")
        win_stub = os.path.join(tmp, "modfs-win-stubs.c")
        win_smoke = os.path.join(ROOT, "build", "pc-win32",
                                 "zz-modfs-smoke.exe")
        try:
            shutil.rmtree(win_root, ignore_errors=True)
            write_toml(os.path.join(win_root, "alpha"),
                       id="alpha", name="Alpha", version="1.0")
            win_blob = b"MODFS-WIN-PROBE\n"
            plant(os.path.join(win_root, "alpha"),
                  "replace/modfs/probe.bin", win_blob)
            open(win_stub, "w").write(
                "int FS_ConvertPathToFileID(void) { return 0; }\n"
                "int FS_CreateFileFromMemory(void) { return 0; }\n"
                "void FS_InitFile(void) {}\n"
                "int FS_SeekFile(void) { return 0; }\n"
                "int FS_WaitAsync(void) { return 0; }\n"
                "int FSi_SendCommand(void) { return 0; }\n"
                "int MapHeader_GetAreaDataArchiveID(void) { return 0; }\n"
                "int MapHeader_GetMapMatrixID(void) { return 0; }\n"
                "int NARC_FindID(void) { return 0; }\n"
                "unsigned NARC_GetFileCount(void) { return 0; }\n"
                "unsigned NARC_GetMemberSize(void) { return 0; }\n"
                "unsigned NARC_GetMemberSizeByIndexPair(void) { return 0; }\n"
                "void NARC_ReadFromMember(void) {}\n"
                "void NARC_ReadFromMemberByIndexPair(void) {}\n"
                "void NARC_ReadWholeMember(void) {}\n"
                "void NARC_ReadWholeMemberByIndexPair(void) {}\n"
                # pc_modfs.c's FS_OpenFileFast marks p_file->arc as a host
                # pointer so the state digest skips it. That lives in
                # pc_state.c, which this smoke does not link; the smoke is
                # here to prove the overlay compiles and runs on PE, not to
                # digest anything.
                "void pc_state_mark_host_field(const void *f) { (void)f; }\n")
            open(win_src, "w").write(
                '#include <stdio.h>\n'
                '#include <stdlib.h>\n'
                '#include "pc_args.h"\n'
                '#include "pc_modfs.h"\n'
                "int main(int argc, char **argv)\n"
                "{\n"
                "    const char *host;\n"
                "    FILE *f;\n"
                "    unsigned char buf[64];\n"
                "    unsigned n, i;\n"
                "    if (pc_args_apply(argc, argv) != 0) return 2;\n"
                "    pc_modfs_boot();\n"
                "    host = pc_modfs_host_file(\"modfs/probe.bin\");\n"
                "    if (host == NULL) {\n"
                "        fputs(\"modfs: probe failed\\n\", stderr);\n"
                "        return 2;\n"
                "    }\n"
                "    f = fopen(host, \"rb\");\n"
                "    if (f == NULL) {\n"
                "        fputs(\"modfs: probe open failed\\n\", stderr);\n"
                "        return 2;\n"
                "    }\n"
                "    n = (unsigned)fread(buf, 1, sizeof buf, f);\n"
                "    fclose(f);\n"
                "    fprintf(stderr, \"modfs: probe modfs/probe.bin %u \", n);\n"
                "    for (i = 0; i < n; i++) fprintf(stderr, \"%02x\", buf[i]);\n"
                "    fputc('\\n', stderr);\n"
                "    return 0;\n"
                "}\n")
            r = subprocess.run(
                [mingw, "-o", win_smoke, win_src, win_stub, win_obj,
                 win_args, "-I" + os.path.join(ROOT, "pc", "include"),
                 "-I" + os.path.join(ROOT, "pc", "src")],
                capture_output=True, text=True, cwd=ROOT, timeout=60)
            assert r.returncode == 0, (
                "PE overlay smoke did not link:\n%s"
                % (r.stderr or r.stdout)[-800:])
            e = {k: v for k, v in os.environ.items()
                 if not k.startswith("PC_")}
            r = subprocess.run(
                [win_smoke, "PC_MODS=alpha",
                 "PC_MODS_DIR=build/pc-win32/zz-mods"],
                env=e, capture_output=True, text=True, cwd=ROOT,
                timeout=60)
            out = r.stderr + r.stdout
            assert r.returncode == 0, out[-800:]
            assert "modfs: 1 files, 0 members, order=[alpha]" in out, \
                out[-800:]
            assert ("modfs: probe modfs/probe.bin %d %s"
                    % (len(win_blob), win_blob.hex())) in out, out[-800:]
            win_claim = (
                "; Makefile.win builds pc_modfs.o; a PE smoke parses "
                "PC_MODS from argv and the planted replace/ file is "
                "what the overlay returns")
        finally:
            shutil.rmtree(win_root, ignore_errors=True)
            for p in (win_smoke,):
                try:
                    os.remove(p)
                except OSError:
                    pass

    # NDS content-port. Bytes come from a ROM, never from a decomp
    # files/ tree. The running port serves what the pipeline wrote.
    port_py = os.path.join(ROOT, "pc", "modport.py")

    def run_modport(args):
        return subprocess.run(
            [sys.executable, port_py] + args,
            capture_output=True, text=True, cwd=ROOT, timeout=120)

    r = run_modport([])
    out = r.stderr + r.stdout
    assert r.returncode != 0, "missing --rom did not refuse"
    assert "need --rom" in out and "decomp" in out, out[-400:]

    r = run_modport(["--rom", tmp])
    out = r.stderr + r.stdout
    assert r.returncode != 0, "a directory was accepted as a ROM"
    assert "directory" in out, out[-400:]

    junk = os.path.join(tmp, "not-nds.bin")
    open(junk, "wb").write(b"NARC" + b"\x00" * 512)
    r = run_modport(["--rom", junk, "--identify"])
    out = r.stderr + r.stdout
    assert r.returncode != 0, "a loose NARC was accepted as an NDS ROM"
    assert "not an NDS ROM" in out, out[-400:]

    r = run_modport(["--rom", ROM, "--text", "0", "--out",
                     os.path.join(tmp, "no-text")])
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "--text" in out, out[-400:]

    r = run_modport(["--rom", ROM, "--map", "3", "--out",
                     os.path.join(tmp, "no-map")])
    out = r.stderr + r.stdout
    assert r.returncode != 0 and "--map" in out, out[-400:]

    port_claim = (
        "a missing ROM, a decomp directory and a loose NARC refuse "
        "by name; --text and --map refuse until a conversion is measured")

    if os.path.isfile(ROM):
        r = run_modport(["--rom", ROM, "--identify"])
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-400:]
        assert "CPUE" in out and "platinum" in out, out[-400:]
        assert "pl_pokegra.narc" in out, out[-400:]

        port_pkg = os.path.join(root, "frompt")
        write_toml(port_pkg, id="frompt", name="FromPt", version="1.0")
        r = run_modport(["--rom", ROM, "--out", port_pkg,
                         "--pokemon", "piplup"])
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        pip_idx = 393 * 6 + 3
        pip_path = os.path.join(
            port_pkg, "narc", "poketool", "pokegra", "pl_pokegra.narc",
            str(pip_idx))
        assert os.path.isfile(pip_path), "platinum --pokemon wrote no member"
        pip_bytes = open(pip_path, "rb").read()
        assert len(pip_bytes) == 6448, len(pip_bytes)
        r = boot_probe_narc(root, "frompt",
                            "poketool/pokegra/pl_pokegra.narc/%d" % pip_idx)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert ("modfs: probe-narc poketool/pokegra/pl_pokegra.narc/%d %d %s"
                % (pip_idx, len(pip_bytes), pip_bytes.hex())) in out, \
            out[-800:]
        port_claim += (
            "; Platinum --identify is CPUE and --pokemon piplup writes "
            "member 2361, which the running port returns")

    hg_rom = os.path.join(ROOT, "..", "pokeheartgold", "build",
                          "heartgold.us", "pokeheartgold.us.nds")
    if os.path.isfile(hg_rom):
        r = run_modport(["--rom", hg_rom, "--identify"])
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-400:]
        assert "IPKE" in out and "heartgold" in out, out[-400:]
        assert "a/0/0/4" in out, out[-400:]

        hg_pkg = os.path.join(root, "fromhg")
        write_toml(hg_pkg, id="fromhg", name="FromHg", version="1.0")
        r = run_modport(["--rom", hg_rom, "--out", hg_pkg,
                         "--pokemon", "chikorita", "--as", "piplup"])
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        hg_idx = 393 * 6 + 3
        hg_path = os.path.join(
            hg_pkg, "narc", "poketool", "pokegra", "pl_pokegra.narc",
            str(hg_idx))
        hg_bytes = open(hg_path, "rb").read()
        assert len(hg_bytes) == 6448, len(hg_bytes)
        if os.path.isfile(ROM):
            assert hg_bytes != pip_bytes, \
                "HeartGold Chikorita matched Platinum Piplup"
        r = boot_probe_narc(root, "fromhg",
                            "poketool/pokegra/pl_pokegra.narc/%d" % hg_idx)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert ("modfs: probe-narc poketool/pokegra/pl_pokegra.narc/%d %d %s"
                % (hg_idx, len(hg_bytes), hg_bytes.hex())) in out, \
            out[-800:]
        no_c = []
        for dirpath, _, files in os.walk(hg_pkg):
            no_c.extend(n for n in files if n.endswith(".c"))
        assert not no_c, "ported package contained C: %s" % no_c
        port_claim += (
            "; HeartGold --identify is IPKE, --pokemon chikorita --as "
            "piplup writes member 2361 from a/0/0/4, and the running "
            "port returns those bytes (not Platinum's)")
    else:
        port_claim += "; HeartGold ROM not in the sibling tree"

    dia_rom = os.path.join(ROOT, "..", "..", "backup.nds")
    if os.path.isfile(dia_rom):
        r = run_modport(["--rom", dia_rom, "--identify"])
        out = r.stderr + r.stdout
        assert r.returncode == 0 and "ADAE" in out and "diamond" in out, \
            out[-400:]
        dia_pkg = os.path.join(root, "fromd")
        write_toml(dia_pkg, id="fromd", name="FromD", version="1.0")
        r = run_modport(["--rom", dia_rom, "--out", dia_pkg,
                         "--pokemon", "1"])
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert os.path.isfile(os.path.join(
            dia_pkg, "narc", "poketool", "pokegra", "pl_pokegra.narc",
            "9")), out[-400:]
        assert "does not decode" in out and "warning" in out, out[-400:]
        port_claim += (
            "; Diamond --identify is ADAE and --pokemon 1 writes "
            "member 9 from pokegra.narc and warns that the NCGR "
            "does not decode on Platinum")
    else:
        port_claim += "; Diamond ROM not next to the checkout"

    blk_rom = os.path.join(ROOT, "..", "pokeblack", "build", "black.us",
                           "pokeblack.us.nds")
    if os.path.isfile(blk_rom):
        r = run_modport(["--rom", blk_rom, "--identify"])
        out = r.stderr + r.stdout
        assert r.returncode == 0 and "IRBO" in out and "black" in out, \
            out[-400:]
        r = run_modport(["--rom", blk_rom, "--out",
                         os.path.join(tmp, "fromblk"),
                         "--pokemon", "chikorita"])
        out = r.stderr + r.stdout
        assert r.returncode != 0, "Black --pokemon should refuse"
        assert "Gen-5" in out or "14285" in out, out[-400:]
        port_claim += (
            "; Black --identify is IRBO and --pokemon refuses the "
            "Gen-5 archive")
    else:
        port_claim += "; Black ROM not in the sibling tree"

    assert_vanilla_untouched(van_snap, "after content-only packages")

    if not os.path.exists(ROM):
        return ("a mod patch reaches the modded object, vanilla is untouched, "
                "the manifest names it, an unknown name refuses; "
                "runtime packages resolve in listed order and refuse the "
                "documented errors; a planted replace/ file is what "
                "FS_OpenFile returns, later wins, and .cooked/fs beats replace/; "
                "a planted narc/<path>/<idx> is what the NARC readers return, "
                "later wins, and .cooked/narc beats narc/; "
                "a planted append is readable and an index past 65535 "
                "refuses; NARC_GetFileCount skipped (no ROM); "
                + cook_claim + win_claim + "; " + port_claim)

    r = boot_probe_count(root, "alpha", append_path)
    out = r.stderr + r.stdout
    assert r.returncode == 0, out[-800:]
    assert ("modfs: probe-count poketool/pokegra/pl_pokegra.narc 2965 2964 "
            "%d %s" % (len(append_blob), append_blob.hex())) in out, out[-800:]

    hole = os.path.join(root, "hole")
    write_toml(hole, id="hole", name="Hole", version="1.0")
    plant(hole, "narc/poketool/pokegra/pl_pokegra.narc/2965", b"gap\n")
    r = boot_probe_count(root, "hole",
                         "poketool/pokegra/pl_pokegra.narc/2965")
    out = r.stderr + r.stdout
    assert r.returncode != 0, "append hole did not refuse"
    assert "2964" in out and "hole" in out, out[-800:]

    # The probe already returned the HeartGold bytes. This is the
    # product: the running port must DRAW them. PC_LAB_SPRITE walks
    # pokemon_sprite.c, so a matching probe with a foreign scramble
    # would still fail here. HeartGold's 6448-byte NCGR draws;
    # Diamond's same-size member fills the canvas (no conversion).
    if os.path.isfile(hg_rom):
        dump_hg = os.path.join(tmp, "sprite-dump-hg")
        os.makedirs(dump_hg, exist_ok=True)
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e["PC_MODS_DIR"] = root
        e["PC_SAVE"] = "none"
        e["PC_MODS"] = "fromhg"
        e["PC_ROM"] = ROM
        e["PC_FRAMES"] = "10"
        e["PC_PACE"] = "0"
        e["PC_LAB_SPRITE"] = "393"
        e["PC_LAB_SPRITE_DUMP"] = dump_hg
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=120)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "failed=0" in out, out[-800:]
        assert ("species=393 form=0 face=2 gender=0 shiny=0 "
                "pid=0x00000000 narc=4 char=2361 pltt=2362") in out, \
            out[-800:]
        # HeartGold Chikorita, not Platinum Piplup. Measured 2026-08-16
        # against build/pc/pokeplatinum on the sibling HeartGold ROM.
        assert "char_fnv=efc6c20d1c9fcf8b pltt_fnv=92ff4187a6718848" \
            in out, out[-800:]
        assert "char_fnv=0ab387a09e642c50" not in out, \
            "ported Piplup still hashed as vanilla Platinum"
        planted = os.path.join(dump_hg, "sp-393-f0-face2-g0-s0-00000000.png")
        assert os.path.isfile(planted), "sprite lab wrote no HeartGold dump"
        pw, ph, prgb = png_decode(open(planted, "rb").read(), planted)
        assert (pw, ph) == (160, 80), (pw, ph)
        # (8,8) is beige (205,205,172) on vanilla Piplup; HeartGold
        # Chikorita's background is cyan (16,172,255). Leaf at (42,24)
        # is (131,230,49).
        i = (8 * pw + 8) * 3
        assert prgb[i:i + 3] == b"\x10\xac\xff", prgb[i:i + 3]
        i = (24 * pw + 42) * 3
        assert prgb[i:i + 3] == b"\x83\xe6\x31", prgb[i:i + 3]
        bg = prgb[0:3]
        nz = sum(1 for p in range(0, len(prgb), 3)
                 if prgb[p:p + 3] != bg)
        assert 1000 <= nz <= 4000, \
            "foreign sprite was not a compact draw (%d)" % nz
        port_claim += (
            "; PC_LAB_SPRITE on 393 draws HeartGold Chikorita "
            "(char_fnv=efc6c20d1c9fcf8b, cyan (8,8), leaf (42,24)) "
            "not Platinum Piplup")

    if replace_ok:
        def boot_lab(mods, extra):
            e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
            e["PC_MODS_DIR"] = root
            e["PC_SAVE"] = "none"
            e["PC_MODS"] = mods
            e["PC_ROM"] = ROM
            e["PC_FRAMES"] = "10"
            e["PC_PACE"] = "0"
            e.update(extra)
            return subprocess.run([BINARY], env=e, capture_output=True,
                                  text=True, timeout=120)

        face = os.path.join(root, "replaceface")
        cooked_bank = os.path.join(face, ".cooked", "narc", "msgdata",
                                   "pl_msg.narc", "215")
        bank_bytes = open(cooked_bank, "rb").read()
        r = boot_probe_narc(root, "replaceface", "msgdata/pl_msg.narc/215")
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert ("modfs: probe-narc msgdata/pl_msg.narc/215 %d %s"
                % (len(bank_bytes), bank_bytes.hex())) in out, out[-800:]

        dump = os.path.join(tmp, "sprite-dump")
        r = boot_lab("replaceface",
                     {"PC_LAB_SPRITE": "393,387",
                      "PC_LAB_SPRITE_DUMP": dump})
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        # Piplup front-male, non-shiny: member 2361 / 2362, planted pixels.
        assert ("species=393 form=0 face=2 gender=0 shiny=0 "
                "pid=0x00000000 narc=4 char=2361 pltt=2362") in out, out[-800:]
        # Turtwig front-male must stay the vanilla members and hashes.
        # char_fnv / pltt_fnv measured on the unmodded ROM this tree builds.
        assert ("species=387 form=0 face=2 gender=0 shiny=0 "
                "pid=0x00000000 narc=4 char=2325 pltt=2326") in out, out[-800:]
        assert "char_fnv=15e580bff491b5d8 pltt_fnv=85a3b41aa78676c4" in out, \
            out[-800:]
        planted = os.path.join(dump, "sp-393-f0-face2-g0-s0-00000000.png")
        assert os.path.isfile(planted), "sprite lab wrote no Piplup dump"
        pw, ph, prgb = png_decode(open(planted, "rb").read(), planted)
        assert (pw, ph) == (160, 80), (pw, ph)
        # (8,8) is beige (205,205,172) on vanilla Piplup; the fixture
        # paints index 15 = magenta (255,0,255) over that 16x16.
        i = (8 * pw + 8) * 3
        assert prgb[i:i + 3] == b"\xff\x00\xff", prgb[i:i + 3]
        # And the overlay did not redraw Turtwig as that magenta block.
        turtwig = os.path.join(dump, "sp-387-f0-face2-g0-s0-00000000.png")
        tw, th, trgb = png_decode(open(turtwig, "rb").read(), turtwig)
        ti = (8 * tw + 8) * 3
        assert trgb[ti:ti + 3] != b"\xff\x00\xff", trgb[ti:ti + 3]

        r = boot_lab("replaceface", {"PC_LAB_TEXT": "215"})
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "pc_lab: text bank=215 entries=1 rendered=1" in out, out[-800:]

        cook_claim += (
            "; PC_LAB_SPRITE on 393 draws the planted magenta at (8,8) "
            "from members 2361/2362 and Turtwig 387 stays vanilla 2325; "
            "PC_LAB_TEXT and NARC_ReadWholeMember return the cooked bank")

        if newsmon_ok:
            dump2 = os.path.join(tmp, "sprite-dump-496")
            r = boot_lab("sprig",
                         {"PC_LAB_SPRITE": "496",
                          "PC_LAB_SPRITE_DUMP": dump2})
            out = r.stderr + r.stdout
            assert r.returncode == 0, out[-800:]
            assert "pl_pokegra=2982" in out, out[-800:]
            assert ("species=496 form=0 face=2 gender=0 shiny=0 "
                    "pid=0x00000000 narc=4 char=2979 pltt=2980") in out, \
                out[-800:]
            planted = os.path.join(dump2, "sp-496-f0-face2-g0-s0-00000000.png")
            assert os.path.isfile(planted), "sprite lab wrote no 496 dump"
            pw, ph, prgb = png_decode(open(planted, "rb").read(), planted)
            assert (pw, ph) == (160, 80), (pw, ph)
            i = (8 * pw + 8) * 3
            assert prgb[i:i + 3] == b"\xff\x00\xff", prgb[i:i + 3]
            r = boot_probe_count(root, "sprig",
                                 "poketool/pokegra/pl_pokegra.narc/2979")
            out = r.stderr + r.stdout
            assert r.returncode == 0, out[-800:]
            assert "modfs: probe-count poketool/pokegra/pl_pokegra.narc 2982 " \
                   in out, out[-800:]
            cook_claim += (
                "; PC_LAB_SPRITE on 496 draws the planted magenta at (8,8) "
                "from members 2979/2980 and NARC_GetFileCount is 2982")

    if person_ok:
        dumpf = os.path.join(tmp, "person-field")
        os.makedirs(dumpf, exist_ok=True)
        lab = os.path.join(tmp, "jub.lab")
        # MAP_HEADER_JUBILIFE_CITY=3, fly tile 180,777, FACE_DOWN=1.
        # PC_LAB_AT=1800, warp, 360 settle, save at 2250.
        open(lab, "w").write("map 3 180 777 1\n")
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e.update({
            "PC_MODS_DIR": root,
            "PC_MODS": "person",
            "PC_LAB": lab,
            "PC_LAB_AT": "1800",
            "PC_SAVE": os.path.join(tmp, "jub-person.sav"),
            "PC_ROM": ROM,
            "PC_FRAMES": "3000",
            "PC_PACE": "0",
            "PC_DUMP_FRAMES": dumpf,
            "PC_DUMP_FROM": "2240",
        })
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=180)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "pc_lab: applied 1 line(s) at frame 2250" in out, out[-800:]
        last = os.path.join(dumpf, "frame-002249.png")
        assert os.path.isfile(last), "field dump wrote no Jubilife frame"
        fw, fh, frgb = png_decode(open(last, "rb").read(), last)
        assert (fw, fh) == (256, 384), (fw, fh)
        magenta = 0
        top = fw * 192 * 3
        for i in range(0, top, 3):
            if frgb[i:i + 3] == b"\xff\x00\xff":
                magenta += 1
        assert magenta >= 100, \
            "planted billboard magenta missing from Jubilife frame (%d)" \
            % magenta
        cook_claim += (
            "; PC_LAB on Jubilife (map 3 at 180,777) dumps a field "
            "frame whose top screen has the planted magenta")

    if house_ok:
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e.update({
            "PC_MODS_DIR": root,
            "PC_MODS": "house",
            "PC_SAVE": "none",
            "PC_ROM": ROM,
            "PC_MODFS_PROBE_MAP": str(MAP_NEW),
        })
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=120)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert ("modfs: probe-map %d area=0 matrix=%d"
                % (MAP_NEW, MATRIX_NEW)) in out, out[-800:]

        dumpf = os.path.join(tmp, "house-field")
        os.makedirs(dumpf, exist_ok=True)
        lab = os.path.join(tmp, "jub-house.lab")
        open(lab, "w").write("map 3 180 777 1\n")
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e.update({
            "PC_MODS_DIR": root,
            "PC_MODS": "house",
            "PC_LAB": lab,
            "PC_LAB_AT": "1800",
            "PC_SAVE": os.path.join(tmp, "jub-house.sav"),
            "PC_ROM": ROM,
            "PC_FRAMES": "3000",
            "PC_PACE": "0",
            "PC_DUMP_FRAMES": dumpf,
            "PC_DUMP_FROM": "2240",
        })
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=180)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "pc_lab: applied 1 line(s) at frame 2250" in out, out[-800:]
        last = os.path.join(dumpf, "frame-002249.png")
        assert os.path.isfile(last), "field dump wrote no Jubilife frame"
        fw, fh, frgb = png_decode(open(last, "rb").read(), last)
        assert (fw, fh) == (256, 384), (fw, fh)
        white = 0
        top = fw * 192 * 3
        for i in range(0, top, 3):
            if (frgb[i] > 230 and frgb[i + 1] > 230
                    and frgb[i + 2] > 230):
                white += 1
        # Vanilla same lab is 267 near-white pixels; the planted
        # dummy-box cube is 1291. Fail if the cube is missing.
        assert white >= 800, \
            "planted prop cube missing from Jubilife frame (%d white)" \
            % white
        cook_claim += (
            "; PC_MODFS_PROBE_MAP on 593 returns area 0 matrix 289; "
            "PC_LAB on Jubilife dumps a field frame whose top screen "
            "has the planted white cube")

    if gltf_ok:
        dumpf = os.path.join(tmp, "gltf-field")
        os.makedirs(dumpf, exist_ok=True)
        lab = os.path.join(tmp, "jub-gltf.lab")
        open(lab, "w").write("map 3 180 777 1\n")
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e.update({
            "PC_MODS_DIR": root,
            "PC_MODS": "gltfbox",
            "PC_LAB": lab,
            "PC_LAB_AT": "1800",
            "PC_SAVE": os.path.join(tmp, "jub-gltf.sav"),
            "PC_ROM": ROM,
            "PC_FRAMES": "3000",
            "PC_PACE": "0",
            "PC_DUMP_FRAMES": dumpf,
            "PC_DUMP_FROM": "2240",
        })
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=180)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "pc_lab: applied 1 line(s) at frame 2250" in out, out[-800:]
        last = os.path.join(dumpf, "frame-002249.png")
        assert os.path.isfile(last), "field dump wrote no Jubilife frame"
        fw, fh, frgb = png_decode(open(last, "rb").read(), last)
        assert (fw, fh) == (256, 384), (fw, fh)
        magenta = 0
        top = fw * 192 * 3
        for i in range(0, top, 3):
            r8, g8, b8 = frgb[i], frgb[i + 1], frgb[i + 2]
            if r8 > 180 and g8 < 80 and b8 > 180:
                magenta += 1
        assert magenta >= 100, \
            "planted glTF cube missing from Jubilife frame (%d magenta)" \
            % magenta
        cook_claim += (
            "; PC_LAB on Jubilife dumps a field frame whose top screen "
            "has the cooked glTF magenta cube")

    if hub_ok:
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e.update({
            "PC_MODS_DIR": root,
            "PC_MODS": "hubmap",
            "PC_SAVE": "none",
            "PC_ROM": ROM,
            "PC_MODFS_PROBE_MAP": str(MAP_NEW),
        })
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=120)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert ("modfs: probe-map %d area=6 matrix=%d"
                % (MAP_NEW, MATRIX_NEW)) in out, out[-800:]

        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e.update({
            "PC_MODS_DIR": root,
            "PC_MODS": "hubmap",
            "PC_SAVE": "none",
            "PC_ROM": ROM,
            "PC_FRAMES": "10",
            "PC_PACE": "0",
            "PC_LAB_TEXT": str(MSG_NEW),
        })
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=120)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "pc_lab: text bank=724 entries=1 rendered=1" in out, out[-800:]
        assert "pl_msg banks=725" in out, out[-800:]

        dumpf = os.path.join(tmp, "hub-field")
        os.makedirs(dumpf, exist_ok=True)
        lab = os.path.join(tmp, "hub.lab")
        scan = os.path.join(tmp, "hub.scan")
        open(lab, "w").write("map 593 20 9 1\n")
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e.update({
            "PC_MODS_DIR": root,
            "PC_MODS": "hubmap",
            "PC_LAB": lab,
            "PC_LAB_AT": "1800",
            "PC_LAB_MAPSCAN": scan,
            "PC_SAVE": os.path.join(tmp, "hub.sav"),
            "PC_ROM": ROM,
            "PC_FRAMES": "3000",
            "PC_PACE": "0",
            "PC_DUMP_FRAMES": dumpf,
            "PC_DUMP_FROM": "2240",
            "PC_TRACE_SCRIPT": "1800",
        })
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=180)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "pc_lab: applied 1 line(s) at frame 2250" in out, out[-800:]
        assert "op=0023" in out, out[-800:]
        scan_txt = open(scan).read()
        assert "map 593\n" in scan_txt, scan_txt[:400]
        assert "player 20 9 " in scan_txt and "behavior 00" in scan_txt, \
            scan_txt[:400]
        assert "object gfx 4 script 2 at 20 10" in scan_txt, scan_txt[:400]
        last = os.path.join(dumpf, "frame-002249.png")
        assert os.path.isfile(last), "field dump wrote no hub frame"
        fw, fh, frgb = png_decode(open(last, "rb").read(), last)
        assert (fw, fh) == (256, 384), (fw, fh)
        white = 0
        top = fw * 192 * 3
        for i in range(0, top, 3):
            if (frgb[i] > 230 and frgb[i + 1] > 230
                    and frgb[i + 2] > 230):
                white += 1
        assert white >= 800, \
            "planted prop cube missing from hub frame (%d white)" % white
        cook_claim += (
            "; PC_LAB map 593 20 9 lands on header 593 (MAPSCAN) "
            "behavior 00, OnTransition fires SetTrainerFlag (op=0023), "
            "a youngster object is on (20,10), PC_LAB_TEXT bank 724 "
            "renders, and the dumped frame has the planted white cube")

    # Cook the sample packages if a checkout with them is around, and
    # copy them out first so cooking cannot dirty that tree.
    first_ok = False
    first_dir = None
    first_cands = []
    env_mmo = os.environ.get("OPENMMO_DIR")
    if env_mmo:
        first_cands.append(os.path.join(env_mmo, "mmo", "mods"))
    first_cands.append(os.path.abspath(os.path.join(
        ROOT, "..", "..", "..", "OpenMMO", "mmo", "mods")))
    for cand in first_cands:
        if all(os.path.isdir(os.path.join(cand, n))
               for n in ("hub", "bodies", "sprigatito")):
            first_dir = cand
            break
    if first_dir is None:
        cook_claim += ("; first packages skipped (no mmo/mods/"
                       "{hub,bodies,sprigatito})")
    elif not (os.path.isfile(nitromdl)
              and shutil.which("arm-none-eabi-gcc")
              and os.path.isfile(enumproc)
              and os.path.exists(msgenc)
              and os.path.exists(eventpy)
              and os.path.exists(nitrogfx)
              and os.path.exists(nitrobtx)):
        cook_claim += ("; first packages skipped "
                       "(no assembler/msgenc/nitrogfx/nitrobtx/nitromdl)")
    else:
        for pkg_name in ("hub", "bodies", "sprigatito"):
            src = os.path.join(first_dir, pkg_name)
            for dirpath, _, files in os.walk(src):
                if "/.cooked/" in (dirpath + "/"):
                    continue
                for name in files:
                    assert not name.endswith(".c"), os.path.join(
                        dirpath, name)
        staged = os.path.join(tmp, "first-packages")
        os.makedirs(staged, exist_ok=True)
        for pkg_name in ("hub", "bodies", "sprigatito"):
            shutil.copytree(
                os.path.join(first_dir, pkg_name),
                os.path.join(staged, pkg_name),
                ignore=shutil.ignore_patterns(".cooked", ".git"))
        r = run_cook(staged, "bodies,hub,sprigatito")
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "no rebuild" in out, out[-800:]
        bodies_ids = open(os.path.join(staged, "bodies", ".cooked",
                                       "ids.toml")).read()
        hub_ids = open(os.path.join(staged, "hub", ".cooked",
                                    "ids.toml")).read()
        sprig_ids = open(os.path.join(staged, "sprigatito", ".cooked",
                                      "ids.toml")).read()
        assert '"bodies:gfx/magenta_person" = 276' in bodies_ids, bodies_ids
        assert '"bodies:prop/cool_house" = 590' in bodies_ids, bodies_ids
        assert '"hub:map/hub" = 593' in hub_ids, hub_ids
        assert '"sprigatito:species/sprigatito" = 496' in sprig_ids, \
            sprig_ids

        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e.update({
            "PC_MODS_DIR": staged,
            "PC_MODS": "bodies,hub,sprigatito",
            "PC_SAVE": "none",
            "PC_ROM": ROM,
            "PC_MODFS_PROBE_MAP": "593",
        })
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=120)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "modfs: probe-map 593 area=6 matrix=289" in out, out[-800:]

        dump_sp = os.path.join(tmp, "first-sprite")
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e.update({
            "PC_MODS_DIR": staged,
            "PC_MODS": "bodies,hub,sprigatito",
            "PC_SAVE": "none",
            "PC_ROM": ROM,
            "PC_FRAMES": "10",
            "PC_PACE": "0",
            "PC_LAB_SPRITE": "496",
            "PC_LAB_SPRITE_DUMP": dump_sp,
        })
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=120)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "pl_pokegra=2982" in out, out[-800:]
        assert ("species=496 form=0 face=2 gender=0 shiny=0 "
                "pid=0x00000000 narc=4 char=2979 pltt=2980") in out, \
            out[-800:]
        # Authored faces, not a Piplup donor and not HeartGold Chikorita.
        assert "char_fnv=035493445caa6588 pltt_fnv=0474ed7b4d6227ef" \
            in out, out[-800:]
        assert "char_fnv=0ab387a09e642c50" not in out, out[-800:]
        planted = os.path.join(dump_sp,
                               "sp-496-f0-face2-g0-s0-00000000.png")
        assert os.path.isfile(planted), "sprite lab wrote no 496 dump"
        pw, ph, prgb = png_decode(open(planted, "rb").read(), planted)
        assert (pw, ph) == (160, 80), (pw, ph)
        i = (8 * pw + 8) * 3
        assert prgb[i:i + 3] == b"\xff\x00\xff", prgb[i:i + 3]

        # Text lab is a different boot: the sprite lab owns the 10
        # frames when both are set.
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e.update({
            "PC_MODS_DIR": staged,
            "PC_MODS": "bodies,hub,sprigatito",
            "PC_SAVE": "none",
            "PC_ROM": ROM,
            "PC_FRAMES": "10",
            "PC_PACE": "0",
            "PC_LAB_TEXT": "724",
        })
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=120)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "pc_lab: text bank=724 entries=1 rendered=1" in out, out[-800:]

        dumpf = os.path.join(tmp, "first-hub")
        os.makedirs(dumpf, exist_ok=True)
        lab = os.path.join(tmp, "first-hub.lab")
        scan = os.path.join(tmp, "first-hub.scan")
        open(lab, "w").write("map 593 20 9 1\n")
        e = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        e.update({
            "PC_MODS_DIR": staged,
            "PC_MODS": "bodies,hub,sprigatito",
            "PC_LAB": lab,
            "PC_LAB_AT": "1800",
            "PC_LAB_MAPSCAN": scan,
            "PC_SAVE": os.path.join(tmp, "first-hub.sav"),
            "PC_ROM": ROM,
            "PC_FRAMES": "3000",
            "PC_PACE": "0",
            "PC_DUMP_FRAMES": dumpf,
            "PC_DUMP_FROM": "2240",
            "PC_TRACE_SCRIPT": "1800",
        })
        r = subprocess.run([BINARY], env=e, capture_output=True,
                           text=True, timeout=180)
        out = r.stderr + r.stdout
        assert r.returncode == 0, out[-800:]
        assert "pc_lab: applied 1 line(s) at frame 2250" in out, out[-800:]
        assert "op=0023" in out, out[-800:]
        scan_txt = open(scan).read()
        assert "map 593\n" in scan_txt, scan_txt[:400]
        assert "player 20 9 " in scan_txt and "behavior 00" in scan_txt, \
            scan_txt[:400]
        assert "object gfx 276 script 2 at 20 10" in scan_txt, \
            scan_txt[:400]
        last = os.path.join(dumpf, "frame-002249.png")
        assert os.path.isfile(last), "field dump wrote no first-hub frame"
        fw, fh, frgb = png_decode(open(last, "rb").read(), last)
        assert (fw, fh) == (256, 384), (fw, fh)
        magenta = cyan = 0
        top = fw * 192 * 3
        for i in range(0, top, 3):
            r8, g8, b8 = frgb[i], frgb[i + 1], frgb[i + 2]
            if r8 == 255 and g8 == 0 and b8 == 255:
                magenta += 1
            if r8 < 80 and g8 > 180 and b8 > 180:
                cyan += 1
        assert magenta >= 100, \
            "landed magenta person missing from hub frame (%d)" % magenta
        assert cyan >= 100, \
            "landed cyan prop missing from hub frame (%d)" % cyan
        first_ok = True

    if first_ok:
        cook_claim += (
            "; landed mmo/mods/{hub,bodies,sprigatito} have no .c and "
            "cook to gfx 276 / prop 590 / map 593 / species 496; "
            "PC_LAB_SPRITE on 496 draws the authored magenta "
            "(char_fnv=035493445caa6588) from members 2979/2980 and "
            "NARC_GetFileCount is 2982; PC_MODFS_PROBE_MAP on 593 "
            "returns area 6 matrix 289; PC_LAB map 593 20 9 lands on "
            "header 593, OnTransition fires SetTrainerFlag (op=0023), "
            "object gfx 276 is on (20,10), bank 724 renders, and the "
            "dumped frame has the magenta person and the cyan cube")

    assert_vanilla_untouched(van_snap, "after field labs")

    return ("a mod patch reaches the modded object, vanilla is untouched, "
            "the manifest names it, an unknown name refuses; "
            "plugins/src nests, plugins/include and include/ resolve "
            "without a relative climb, plugins/patches stacks on "
            "patches/, a header patch stages into modinclude, a "
            "pc/src patch is not ignored, and fuzz=0 refuses a "
            "patch that needs fuzz; "
            "runtime packages resolve in listed order and refuse the "
            "documented errors; a planted replace/ file is what "
            "FS_OpenFile returns, later wins, and .cooked/fs beats replace/; "
            "a planted narc/<path>/<idx> is what the NARC readers return, "
            "later wins, and .cooked/narc beats narc/; "
            "a planted append grows NARC_GetFileCount, a hole names the "
            "missing index, and an append past 65535 refuses; "
            "make status prints the overlay line; later-wins names the "
            "loser; vanilla game/SDK objects and geninclude stay "
            "byte-identical and pc_modfs.o is the one new vanilla object; "
            + cook_claim + win_claim + "; " + port_claim)


STUBS_LIST = os.path.join(ROOT, "pc", "stubs.list")
STUBS_OBJ = os.path.join(ROOT, "build", "pc", "obj", "pc", "stubs.o")


def test_ppwlobby_stubbed(tmp):
    """The Wi-Fi Plaza lobby is a trap, not a lie, and not a C++ compile.

    lib/ppwlobby is ten C++ files. The port does not compile them, and the
    decision is deliberate rather than pending: the build references exactly 21
    symbols from that library (20 PPW_Lobby* plus one prebuilt helper), every
    one of them reached only from the Wi-Fi Plaza and WFC overlays, which need a
    DWC internet connection, which needs the wireless stack this port does not
    model. Compiling the library would add a C++ toolchain to both pipelines and
    put a C++ runtime's symbols into a single-namespace link, to supply
    twenty-one functions nothing reachable calls.

    Two ways that decision could rot, and this checks both:

      * A ppwlobby source could enter the build, dragging a C++ runtime in
        behind it. Checked against `make objlist`, the build's own answer.
      * A PPW_ symbol could gain a definition that is not a trap, a stub that
        returns 0 and lets a lobby path proceed on a fabricated answer. Every
        one must resolve to the weak trap in stubs.o and to nothing else.

    The count is asserted non-zero on purpose. If the library's symbols were
    renamed out of the PPW_ prefix, every loop above would iterate over nothing
    and this test would pass while checking absolutely nothing; which is
    precisely how test_mirror once passed over a mirror that had lost every file
    it was supposed to be guarding.
    """
    objs = subprocess.run(["make", "-s", "-f", PC_MAKEFILE, "objlist"],
                          cwd=ROOT, capture_output=True, text=True, timeout=600)
    assert objs.returncode == 0, "make objlist failed: " + objs.stderr.strip()[:300]
    compiled = [l.strip() for l in objs.stdout.split("\n") if l.strip()]
    from_lib = [o for o in compiled if "/ppwlobby/" in o or "/ilobby/" in o]
    assert not from_lib, ("lib/ppwlobby sources have entered the build (%d), which "
                          "pulls a C++ runtime into the link:\n  %s"
                          % (len(from_lib), "\n  ".join(from_lib[:4])))

    if not os.path.exists(STUBS_OBJ):
        return "skipped: no %s (build first)" % os.path.relpath(STUBS_OBJ, ROOT)

    listed = {}
    for line in open(STUBS_LIST):
        if line.startswith("#") or not line.strip():
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) >= 3:
            listed[parts[0]] = (parts[1], parts[2])

    all_objs = subprocess.run(
        ["bash", "-c", 'find "$1" -name "*.o"', "_",
         os.path.join(ROOT, "build", "pc", "obj")],
        capture_output=True, text=True).stdout.split()

    # Referenced but undefined, across the whole object tree.
    referenced = set()
    for o in all_objs:
        out = subprocess.run(["nm", "-u", o], capture_output=True, text=True).stdout
        for line in out.split("\n"):
            f = line.split()
            if f and f[-1].startswith("PPW_"):
                referenced.add(f[-1])
    assert referenced, ("no PPW_ symbol is referenced anywhere in the build. The "
                        "lobby surface has been renamed or removed, and every "
                        "check in this test is now iterating over nothing.")

    unstubbed = sorted(s for s in referenced if s not in listed)
    assert not unstubbed, ("%d referenced lobby symbol(s) are not in "
                           "pc/stubs.list, so the link is resolving them from "
                           "somewhere unrecorded:\n  %s"
                           % (len(unstubbed), "\n  ".join(unstubbed[:6])))

    silent = sorted(s for s in referenced
                    if listed[s][0] != "func" or not listed[s][1].strip())
    assert not silent, ("%d lobby symbol(s) are not loud traps with a recorded "
                        "reason:\n  %s" % (len(silent), "\n  ".join(silent[:6])))

    # And the trap is the ONLY definition. A second, non-trapping definition
    # elsewhere would win or clash, and either way the trap stops being the
    # thing that runs.
    definers = {}
    for o in all_objs:
        out = subprocess.run(["nm", "--defined-only", o],
                             capture_output=True, text=True).stdout
        for line in out.split("\n"):
            f = line.split()
            if len(f) >= 3 and f[-1] in referenced:
                definers.setdefault(f[-1], []).append((o, f[-2]))
    wrong = []
    for s in sorted(referenced):
        where = definers.get(s, [])
        if len(where) != 1 or os.path.abspath(where[0][0]) != os.path.abspath(STUBS_OBJ):
            wrong.append("%s defined by %s" % (s, [os.path.basename(w[0]) for w in where] or "nothing"))
        elif where[0][1] != "W":
            wrong.append("%s is %s, not a weak trap" % (s, where[0][1]))
    assert not wrong, ("%d lobby symbol(s) do not resolve to the weak trap in "
                       "stubs.o:\n  %s" % (len(wrong), "\n  ".join(wrong[:6])))

    return ("%d lobby symbols referenced, all weak traps in stubs.o with a "
            "recorded reason, no ppwlobby source in the build" % len(referenced))


ARM7_GEN = os.path.join(ROOT, "pc", "gen_arm7_excludes.py")
ABI_LAYOUT = os.path.join(ROOT, "pc", "abi_layout.py")
PCDIFF = os.path.join(ROOT, "pc", "diff", "pcdiff.py")
MELON_HOST = os.path.join(ROOT, "build", "pc", "pcdiff-melon")
PCLAUNCH = os.path.join(ROOT, "build", "pc", "pclaunch")
ROM = os.path.join(ROOT, "build", "rom", "pokeplatinum.us.nds")


def test_arm7_excluded(tmp):
    """ARM7 sources are named, and the build actually leaves them out.

    keeps the other processor's code out of this
    single-namespace link. Upstream #1247 turned the ARM7 into a compiled
    binary, which moved 86 ARM7 sources into libraries/ where pc/Makefile's
    `find` picks them up, 77 failed to compile against ARM9 headers and 9
    compiled, and those 9 would have linked silently.

    The check that matters is not "did the generator find them" but "did the
    BUILD drop them", because the first attempt at this got the paths right and
    the spelling wrong: pc/Makefile spells its SDK roots through $(ROOT), which
    is ".../pc/..", and $(filter-out) is a string match, so 86 correct-but-
    normalized paths matched none of the find output and the exclusion did
    nothing at all while looking entirely correct. So this asserts against
    `make objlist`, which is the build's own answer to what it will link.
    """
    roots = sorted(glob.glob(os.path.join(ROOT, "subprojects", "Nitro*")))
    roots = [r for r in roots if os.path.isdir(os.path.join(r, "libraries"))]
    assert roots, "no NitroSDK-family wraps with libraries/, layout changed?"

    gen = subprocess.run([sys.executable, ARM7_GEN] + roots,
                         capture_output=True, text=True, timeout=120)
    assert gen.returncode == 0, "gen_arm7_excludes.py failed: " + gen.stderr.strip()
    arm7 = [l for l in gen.stdout.split("\n") if l.strip()]
    assert arm7, ("no ARM7 sources identified at all. Either the SDK wrap "
                  "predates the compiled-ARM7 transition, or the meson parse "
                  "has stopped matching, and a silent empty list is how ARM7 "
                  "code gets into the link.")

    missing = [p for p in arm7 if not os.path.isfile(p)]
    assert not missing, ("gen_arm7_excludes.py named %d path(s) that do not "
                         "exist, so its spelling cannot match the Makefile's: "
                         "%s" % (len(missing), missing[:3]))

    objs = subprocess.run(["make", "-s", "-f", PC_MAKEFILE, "objlist"],
                          cwd=ROOT, capture_output=True, text=True, timeout=600)
    assert objs.returncode == 0, "make objlist failed: " + objs.stderr.strip()[:400]
    listed = set()
    for line in objs.stdout.split("\n"):
        line = line.strip()
        if line:
            listed.add(re.sub(r".*/build/pc/obj/", "", line))

    leaked = []
    for src in arm7:
        rel = os.path.relpath(src, ROOT)
        if "game/" + rel[:-2] + ".o" in listed:
            leaked.append(rel)
    assert not leaked, ("the build still expects %d ARM7 object(s), the "
                        "exclusion is not reaching SDK_SRCS:\n  %s"
                        % (len(leaked), "\n  ".join(leaked[:5])))

    return ("%d ARM7 source(s) identified from the SDK's meson and all of them "
            "absent from the build's object list" % len(arm7))


# The struct layouts the ROM's compiler and the host's compiler are known to
# disagree about, each with the reason it is accepted. A ninth entry appearing
# is the point of this test; an entry disappearing means the list is stale.
#
# Three rules produce all of them, each confirmed against both compilers
# directly rather than inferred from the game's own headers:
#
#  * a bitfield run's storage unit. mwcc reserves the whole declared type and
#    starts a fresh naturally-aligned unit whenever the declared type changes;
#    gcc packs the bits and lets the next member take the next free byte. Six
#    of the eight are this.
#  * `#pragma pack(1)`, which gcc honours and mwcc does not. `_InitPacket` is
#    21 bytes on the host and 24 in the ROM, and the host matches the size the
#    header itself writes down two lines above the struct.
#  * a member's alignment not reaching its struct. SoundSystem's captureBuffer
#    is 32-byte aligned and lands at the same offset under both, but gcc gives
#    the struct alignment 32 and mwcc leaves it 4.
#
# None of the eight crosses a boundary where the ROM's layout is authoritative,
# which is why they can be accepted rather than fixed: all eight are runtime
# state built and read entirely by host-compiled code. The one to watch is
# _InitPacket, which is a wire format.
#
# The list also matters to a differential harness: a comparison against an
# emulator running the ROM will find these eight regions disagreeing by
# construction, and must not read that as a divergence in the port.
ABI_KNOWN = {
    "BattleMon": (192, 192,
                  ["ppCur", "ppUps", "level", "friendship", "nickname"],
                  "eleven u32 bitfield flags end at bit 41.5; mwcc reserves "
                  "the rest of the u32 and gcc puts ppCur at byte 42"),
    "ListMenuTemplate": (32, 32,
                         ["letterSpacing", "lineSpacing", "pagerMode",
                          "fontID", "cursorType"],
                         "the run's declared type changes u8 -> u16, which "
                         "starts a new even-aligned unit under mwcc only"),
    "NamingScreenTouchHitbox": (6, 4, ["cursorX", "cursorY"],
                                "same type change, u16 -> u8; a file-local "
                                "table the naming screen builds itself"),
    "SNDChannelInfo": (12, 12, ["volume", "pan", "pad_"],
                       "two BOOL:1 flags cost mwcc a whole int; filled by the "
                       "sound driver, which the host compiler also compiles"),
    "SNDPlayerInfo": (12, 12,
                      ["trackBitMask", "tempo", "volume", "pad_", "pad2_"],
                      "the same two BOOL:1 flags in the player's copy"),
    "SoundSystem": (773512, 773536, [],
                    "every member agrees; only the struct's own alignment "
                    "differs, and the game has exactly one of them"),
    "UnkStruct_ov17_022444BC": (44, 44,
                                ["unk_28_0", "unk_28_1", "unk_28_2",
                                 "unk_28_3", "unk_28_4", "unk_28_6",
                                 "unk_28_8", "unk_28_10", "unk_28_12"],
                                "contest move display flags, cleared and "
                                "filled in the same overlay"),
    "_InitPacket": (24, 21, ["localip", "localport"],
                    "#pragma pack(1); NAT negotiation, and nothing in this "
                    "port can send one"),
}


def test_abi_layout(tmp):
    """The host's -m32 layout against the ROM compiler's, for every shared struct.

    The port compiles pret's headers with gcc instead of the Metrowerks ARM
    compiler the ROM build uses, and both see the same declarations, so a
    layout they disagree on is the ABI and nothing else. That the two agree is
    the assumption the whole 32-bit build rests on and the thing nothing else
    in this suite touches.

    Neither compiler is asked twice: mwcc's `-sym on` and gcc's `-g` already
    recorded what each decided, so this reads the objects the real builds
    produced. A purpose-built probe compiled with hand-written flags would be
    answering a question about the probe.

    Eight structs disagree and each is written down above with why it is
    survivable. The test is that the list does not grow, and that it does not
    silently shrink either, because a name dropping out means the entry is
    stale and the reason next to it is describing something that no longer
    happens.
    """
    if not glob.glob(os.path.join(ROOT, "build", "rom", "**", "*.o"),
                     recursive=True):
        return "skipped: no ROM objects to compare against (ninja -C build/rom)"

    out = os.path.join(tmp, "abi.json")
    # --jobs is held down deliberately. This scans ~3000 objects and would
    # happily take every core, but it shares the suite with tests that drive
    # the port in real time and measure it against a timeout.
    run = subprocess.run(
        [sys.executable, ABI_LAYOUT, "--quiet", "--json", out, "--jobs", "4"],
        cwd=ROOT, capture_output=True, text=True, timeout=900)
    assert run.returncode in (0, 1), ("pc/abi_layout.py failed: "
                                      + (run.stderr.strip() or run.stdout.strip())[:400])
    with open(out) as f:
        report = json.load(f)

    assert report["rom_objects"] > 500, (
        "only %d object(s) in build/rom were compiled by mwcc, the ROM build "
        "is partial, and a comparison against a partial build would pass by "
        "not looking" % report["rom_objects"])
    assert report["host_objects"] > 500, (
        "only %d host object(s) carry debug info, is -g still in pc/Makefile's "
        "flags? Without it this test compares nothing."
        % report["host_objects"])
    assert len(report["shared"]) > 3000, (
        "only %d struct(s) are named by both compilers; this test is only worth "
        "anything while that number is most of the game" % len(report["shared"]))

    found = {d["name"]: d for d in report["differences"]}
    new = sorted(set(found) - set(ABI_KNOWN))
    assert not new, (
        "%d struct(s) newly disagree between the ROM compiler and the host's:\n  %s"
        "\nRun `make -f pc/Makefile abi` for the whole report and "
        "`python3 pc/abi_layout.py --dump NAME` for one struct's two layouts. "
        "Judge each one by whether the ROM's layout is authoritative for it, "
        "anything read out of ROM data, an archive or a save has to be fixed "
        "rather than listed." % (len(new), "\n  ".join(new)))

    gone = sorted(set(ABI_KNOWN) - set(found))
    assert not gone, (
        "%s no longer disagree(s), so the recorded reason is stale, delete "
        "the entry rather than leaving an explanation for something that has "
        "stopped happening" % ", ".join(gone))

    changed = []
    for name, d in sorted(found.items()):
        rom, host, moved, _why = ABI_KNOWN[name]
        got = (d["rom_size"], d["pc_size"], [m[0] for m in d["moved"]])
        if got != (rom, host, moved):
            changed.append("%s: expected rom=%d host=%d moved=%s, got rom=%d "
                           "host=%d moved=%s"
                           % (name, rom, host, moved, got[0], got[1], got[2]))
    assert not changed, ("a known disagreement changed shape, which means it is "
                         "not the disagreement the note describes:\n  "
                         + "\n  ".join(changed))

    return ("%d struct(s) shared by both compilers, the %d known disagreements "
            "and no others (%d mwcc objects, %d gcc objects)"
            % (len(report["shared"]), len(found),
               report["rom_objects"], report["host_objects"]))


def melon_oracle():
    """The oracle binary, if it exists and is not older than what built it.

    A stale oracle is worse than none: it answers, and the answer is about a
    melonDS or a source file that is no longer there. The dependency list comes
    from `make melon-deps` rather than a copy here, so it cannot drift from the
    rule that builds the thing.
    """
    if not os.path.exists(MELON_HOST):
        return None, "no oracle at %s (make -f pc/Makefile melon)" % \
            os.path.relpath(MELON_HOST, ROOT)
    deps = subprocess.run(["make", "-s", "-f", PC_MAKEFILE, "melon-deps"],
                          cwd=ROOT, capture_output=True, text=True, timeout=120)
    if deps.returncode != 0:
        return None, "make melon-deps failed: " + deps.stderr.strip()[:200]
    built = os.path.getmtime(MELON_HOST)
    stale = [d for d in deps.stdout.split()
             if os.path.exists(d) and os.path.getmtime(d) > built]
    if stale:
        return None, ("the oracle is older than %s, rebuild it with "
                      "`make -f pc/Makefile melon`"
                      % os.path.relpath(stale[0], ROOT))
    return MELON_HOST, None


def test_diff(tmp):
    """The port and melonDS over the same ROM, aligned, and what they agree on.

    The property that makes this worth anything is that this tree builds the
    ROM the emulator runs, so both sides execute the same image and a
    divergence is the port's rather than an artefact of comparing a
    decompilation against somebody else's dump.

    What is asserted here is the HARNESS, not the port's correctness. The
    comparison's job is to produce a number that a human then argues about;
    a test that pinned today's divergences would fail every time the port got
    better. So this checks the things that must hold for the number to mean
    anything at all:

      - both sides write a complete trace over the same span list
      - the two can be aligned, and the offset is the one measured by hand
      - the spans that agree are agreeing about something that MOVED, because
        a region neither side wrote agrees with itself and proves nothing
      - the picture agrees at a substantial fraction of checkpoints, which is
        the one end-to-end statement: the port draws what the console draws

    And the property with teeth, the same one --watch is held to: tracing must
    not change the run. A run with --diff-trace and a run without produce the
    same --state-digest, or every measurement taken with this is suspect.
    """
    if not os.path.exists(ROM):
        return "skipped: no ROM at %s (ninja -C build/rom)" % os.path.relpath(ROM, ROOT)
    oracle, why = melon_oracle()
    if oracle is None:
        return "skipped: " + why

    out = os.path.join(tmp, "run")
    run = subprocess.run(
        [sys.executable, PCDIFF, "--frames", "600", "--every", "10",
         "--keep", out],
        cwd=ROOT, capture_output=True, text=True, timeout=1800)
    assert run.returncode == 0, ("pcdiff.py failed: "
                                 + (run.stderr.strip() or run.stdout)[-800:])

    sys.path.insert(0, os.path.join(ROOT, "pc", "diff"))
    import pcdiff
    port = pcdiff.parse(os.path.join(out, "port.txt"))
    melon = pcdiff.parse(os.path.join(out, "melon.txt"))

    assert port.complete and melon.complete, (
        "a trace has no `end` line, so a side stopped early: port=%s melon=%s"
        % (port.complete, melon.complete))
    assert melon.stopped is None, "the oracle " + str(melon.stopped)
    assert [s[0] for s in port.spans] == [s[0] for s in melon.spans], (
        "the two sides digested different addresses; the oracle is meant to "
        "read the span list the port writes, and did not")
    assert len(port.cps) == len(melon.cps) == 61, (
        "expected 61 checkpoints a side over 600 frames every 10, got %d and %d"
        % (len(port.cps), len(melon.cps)))

    off, hit, tot, ambiguous = pcdiff.align(port, melon)
    assert not ambiguous, (
        "the alignment is ambiguous: several offsets score the same, so the "
        "picture is not distinguishing them and no offset here is evidence")
    # Measured 2026-08-11 over this ROM. Pinned because the port booting
    # suddenly faster or slower than the console is a real change worth
    # noticing, and because everything below reads at this offset.
    assert off == 140, (
        "the port used to reach the console's frame N+140 and now reaches "
        "N+%d. That is a change in how long the port's boot takes relative to "
        "the console's, find out what changed before moving this number."
        % off)

    mby = {c["frame"]: c for c in melon.cps if c["frame"] is not None}
    pairs = [(c, mby[c["frame"] + off]) for c in port.cps
             if c["frame"] is not None and c["frame"] + off in mby]
    assert len(pairs) >= 40, "only %d aligned checkpoints" % len(pairs)

    # The picture is the one signal that both moves and agrees, which is a
    # measured fact about this port and not an assumption. Over this run every
    # memory span either never changed on both sides, agreeing with itself,
    # which proves nothing, or changed and diverged. The picture changes at
    # most checkpoints and matches at about half, so it is the only end-to-end
    # statement available: the port draws what the console draws, often.
    pfb = {p["fb"] for p, _m in pairs}
    mfb = {m["fb"] for _p, m in pairs}
    assert len(pfb) > 1 and len(mfb) > 1, (
        "the picture did not change over the run (%d distinct on the port, %d "
        "on the console), so a match between the two says nothing. Either the "
        "run is too short or a side has stopped drawing." % (len(pfb), len(mfb)))
    fb = sum(1 for p, m in pairs if p["fb"] == m["fb"])
    assert fb >= len(pairs) // 4, (
        "the picture agreed at %d of %d checkpoints; it was 24 of 46 when this "
        "was written. A collapse means the port has stopped drawing what the "
        "console draws." % (fb, len(pairs)))

    # And the scripted leg, which is the one the harness was silently failing
    # at until the phase shift landed. The port's frame N is the console's
    # N+140, so feeding both sides the same frame numbers landed every key
    # 140 console frames late: the console sat at prompts the port had walked
    # past, and produced barely half as many distinct pictures over the same
    # run. That is what this pins, not agreement, which is the port's
    # business, but that the console is playing the same session.
    sout = os.path.join(tmp, "scripted")
    srun = subprocess.run(
        [sys.executable, PCDIFF, "--frames", "1200", "--every", "20",
         "--input", os.path.join(ROOT, "pc", "replays", "new-game.txt"),
         "--keep", sout],
        cwd=ROOT, capture_output=True, text=True, timeout=1800)
    assert srun.returncode == 0, ("scripted pcdiff.py failed: "
                                  + (srun.stderr.strip() or srun.stdout)[-800:])
    assert "probe aligned at +140 frames" in srun.stdout, (
        "the input-free probe did not measure the known offset:\n"
        + srun.stdout[:400])
    sp = pcdiff.parse(os.path.join(sout, "port.txt"))
    sm = pcdiff.parse(os.path.join(sout, "melon.txt"))
    smby = {c["frame"]: c for c in sm.cps if c["frame"] is not None}
    spairs = [(c, smby[c["frame"] + 140]) for c in sp.cps
              if c["frame"] is not None and c["frame"] + 140 in smby]
    pdist = len({p["fb"] for p, _m in spairs})
    mdist = len({m["fb"] for _p, m in spairs})
    assert mdist >= pdist * 3 // 4, (
        "the console drew %d distinct pictures against the port's %d over the "
        "same script. It is not following it; the phase shift is the usual "
        "reason." % (mdist, pdist))
    sfb = sum(1 for p, m in spairs if p["fb"] == m["fb"])
    assert sfb >= len(spairs) // 4, (
        "the scripted run agreed at %d of %d checkpoints; it was 28 of 53 "
        "when this was written, against 13 before the phase shift."
        % (sfb, len(spairs)))

    # Reported rather than required: no span is in this list today, and the
    # day one is, that is a result and not a regression.
    moved_and_agreed = []
    for addr, _ln, name in port.spans:
        pv = {p["d"][addr] for p, _m in pairs if addr in p["d"]}
        mv = {m["d"][addr] for _p, m in pairs if addr in m["d"]}
        if (len(pv) > 1 and len(mv) > 1
                and all(p["d"].get(addr) == m["d"].get(addr) for p, m in pairs)):
            moved_and_agreed.append(name)

    # Looking must not change the run.
    def digest(extra):
        env = dict(os.environ)
        env.update({"PC_ROM": ROM, "PC_SAVE": "none", "PC_FRAMES": "120",
                    "PC_STATE_DIGEST": "1"})
        env.update(extra)
        r = subprocess.run([BINARY], env=env, capture_output=True,
                           text=True, timeout=600)
        return [l for l in r.stderr.split("\n") if l.startswith("pc-state exit")]

    plain = digest({})
    traced = digest({"PC_DIFF_TRACE": os.path.join(tmp, "probe.txt"),
                     "PC_DIFF_EVERY": "10"})
    assert plain and plain == traced, (
        "--diff-trace changed the run it is measuring: the exit digest differs "
        "with the trace on. An instrument that perturbs what it measures makes "
        "every number it produced suspect.\n  without: %s\n  with:    %s"
        % (plain[:3], traced[:3]))

    # The oracle's two new outputs, and the one fact that makes an audio
    # differential possible at all: both engines write the same format at the
    # same rate for the same frame count. That was ASSUMED and was wrong,
    # melonDS resamples its mix to whatever OutputSampleRate says, and its
    # default gave 48 kHz, so 600 frames came out 1.47x too long. The oracle
    # now pins its rate to the port's; if that line is ever removed, this is
    # what says so.
    owav = os.path.join(tmp, "oracle.wav")
    osav = os.path.join(tmp, "oracle.sav")
    spans = os.path.join(out, "spans.txt")
    orun = subprocess.run(
        [oracle, "--rom", ROM, "--spans", spans,
         "--trace", os.path.join(tmp, "o.trace"), "--frames", "600",
         "--every", "600", "--wav", owav, "--sav-out", osav],
        capture_output=True, text=True, timeout=1800)
    assert orun.returncode == 0, "the oracle failed: " + orun.stderr[-800:]
    assert os.path.getsize(osav) == 524288, (
        "--sav-out wrote %d bytes, not the cartridge's 512 KB"
        % os.path.getsize(osav))

    pwav = os.path.join(tmp, "port.wav")
    penv = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    prun = subprocess.run(
        [BINARY], timeout=1800, capture_output=True, text=True,
        env=dict(penv, PC_ROM=ROM, PC_SAVE="none", PC_FRAMES="600",
                 PC_PACE="0", PC_DUMP_AUDIO=pwav))
    assert prun.returncode == 0, "the port failed: " + prun.stderr[-800:]

    def wav_rate_and_samples(path):
        with open(path, "rb") as f:
            head = f.read(48)
        assert head[:4] == b"RIFF" and head[8:12] == b"WAVE", "%s is not a WAV" % path
        rate = struct.unpack("<I", head[24:28])[0]
        channels = struct.unpack("<H", head[22:24])[0]
        bits = struct.unpack("<H", head[34:36])[0]
        data = struct.unpack("<I", head[40:44])[0]
        return rate, channels, bits, data // (channels * bits // 8)

    orate, och, obits, osamp = wav_rate_and_samples(owav)
    prate, pch, pbits, psamp = wav_rate_and_samples(pwav)
    assert (orate, och, obits) == (prate, pch, pbits), (
        "the two engines write different audio formats: oracle %d Hz %dch %dbit, "
        "port %d Hz %dch %dbit" % (orate, och, obits, prate, pch, pbits))
    # Same wall-clock length to within a frame's worth of samples. Not
    # sample-for-sample: that is a comparison of the two mixes, which is a
    # later task's business and would fail here for real reasons.
    slack = orate // 60 + 1
    assert abs(osamp - psamp) <= slack, (
        "600 frames gave the oracle %d samples and the port %d, more than a "
        "frame apart; the two are not running at the same rate" % (osamp, psamp))

    return ("offset %d, the picture agrees at %d/%d checkpoints, %d span(s) "
            "both moved and agreed%s; both engines mix at %d Hz and 600 "
            "frames give %d samples against %d"
            % (off, fb, len(pairs), len(moved_and_agreed),
               " (" + ", ".join(moved_and_agreed) + ")" if moved_and_agreed else "",
               orate, osamp, psamp))


def _launch_run(exe, work, args, rom=None, picker="true", timeout=120):
    """One pclaunch run, isolated from this machine.

    XDG_CONFIG_HOME and APPDATA point at a scratch directory so a developer's
    own saved settings never decide a test, and SDL_VIDEODRIVER=dummy gives it
    a window with no display.

    PCLAUNCH_PICKER is set on every run, always. The launcher opens a file
    dialog by itself when it finds no ROM, so on a machine with zenity an
    unset picker puts a modal window on somebody's screen and hangs the suite
    until a human clicks it.
    """
    env = dict(os.environ)
    env["XDG_CONFIG_HOME"] = work
    env["APPDATA"] = work
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    env["PCLAUNCH_PICKER"] = picker
    env.pop("POKEPLATINUM_ROM", None)
    for k in list(env):
        if k.startswith("PC_"):
            del env[k]
    if rom:
        env["POKEPLATINUM_ROM"] = rom
    return subprocess.run([exe] + list(args), capture_output=True, text=True,
                          env=env, cwd=work, timeout=timeout)


def _launch_cmd(out, prefix):
    """The command a --dry-run PLAY said it would run, or None if it did not."""
    for line in out.splitlines():
        if line.startswith(prefix):
            return line[len(prefix):].split()
    return None


def test_pclaunch(tmp):
    """The launcher: double-click, Enter, playing, and both halves start.

    The acceptance is one sentence, with a ROM known, launching pclaunch and
    pressing nothing but Enter must reach gameplay, and the only way to
    assert that rather than restate it is to drive the real event loop.
    `--keys` pushes keydowns into SDL's own queue one a frame and then quits,
    `--dry-run` makes PLAY compose and print instead of starting anything, and
    SDL_VIDEODRIVER=dummy gives all of it a window with no display.

    The check that catches what testing the launcher alone cannot is the last
    one: the two command lines it composed are handed to the REAL port and the
    REAL viewer, and the picture has to come out. A launcher that keeps
    offering a flag the port stopped taking passes every menu test ever
    written and fails the first time someone presses PLAY.

    That last check is also why this port needs a two-process test where
    diamond's needed one: the port renders into a shared page and the viewer
    presents it, so "it launched" is only true if BOTH command lines are good
    and the second can find what the first published.
    """
    if not os.path.exists(PCLAUNCH):
        r = subprocess.run(["make", "-f", PC_MAKEFILE, "pclaunch"],
                           capture_output=True, text=True, cwd=ROOT, timeout=600)
        if not os.path.exists(PCLAUNCH):
            return ("skipped: no build/pc/pclaunch and it would not build, "
                    "it needs SDL2, which the port itself never does: "
                    + (r.stderr or r.stdout).strip().splitlines()[-1][:120])
    if not os.path.exists(ROM):
        return "skipped: no ROM at %s (ninja -C build/rom)" % os.path.relpath(ROM, ROOT)

    # A copy of the launcher somewhere with no .nds anywhere near it. The scan
    # looks beside the executable, in build/rom and two directories up, so the
    # real build/pc/pclaunch can never see the "nothing found" path.
    lone_dir = os.path.join(tmp, "bin", "a", "b")
    os.makedirs(lone_dir)
    lone = os.path.join(lone_dir, "pclaunch")
    shutil.copy2(PCLAUNCH, lone)

    def work(name):
        d = os.path.join(tmp, name)
        os.makedirs(d, exist_ok=True)
        return d

    # 1. Enter and nothing else.
    w = work("enter")
    r = _launch_run(lone, w, ["--keys", "return", "--dry-run"], rom=ROM)
    port_cmd = _launch_cmd(r.stdout, "would run:")
    view_cmd = _launch_cmd(r.stdout, "would view:")
    assert port_cmd and ROM in port_cmd and "--rom" in port_cmd, (
        "Enter alone did not reach PLAY:\n" + (r.stdout + r.stderr)[-400:])
    assert view_cmd, ("PLAY composed a port command and no viewer command, so "
                      "nothing would have presented it:\n" + r.stdout[-300:])
    assert "--view" in port_cmd, "the port was not told to publish a channel"
    chan = port_cmd[port_cmd.index("--view") + 1]
    assert chan in view_cmd, (
        "the two halves were given different channels (%s vs %s), so the "
        "viewer would wait for a page nobody writes" % (chan, view_cmd))

    # 2. PLAY is really where the cursor starts; a neighbouring row must not
    #    launch, or the check above would pass on a menu that starts anywhere.
    for keys in ("up,return", "down,return"):
        r2 = _launch_run(lone, work(keys.replace(",", "_")),
                         ["--keys", keys, "--dry-run"], rom=ROM)
        assert _launch_cmd(r2.stdout, "would run:") is None, (
            "`%s` launched the game, so the cursor does not start on PLAY, "
            "Enter on the ROM row cycles the ring instead of playing, which is "
            "the failure this whole test exists for" % keys)

    # 3. A binding in the config reaches the viewer, and the viewer honours
    #    it. Half of this is the launcher's job (carry the line, pass it) and
    #    half is the viewer's (accept the name, move the key), and a test of
    #    either alone passes while a player's remapped A does nothing. So the
    #    config is written by hand, the composed line is checked for it, and
    #    the real viewer is asked to parse the same spec.
    wb = work("bind")
    with open(os.path.join(wb, "pokeplatinum-launcher.cfg"), "w") as f:
        f.write("rom %s\nbind a=z,start=space\n" % ROM)
    rb = _launch_run(lone, wb, ["--keys", "return", "--dry-run"])
    view_b = _launch_cmd(rb.stdout, "would view:")
    assert view_b and "--bind" in view_b, (
        "a binding in the config did not reach the viewer's command line:\n"
        + rb.stdout[-300:])
    assert view_b[view_b.index("--bind") + 1] == "a=z,start=space", (
        "the binding was rewritten on the way: %s"
        % view_b[view_b.index("--bind") + 1])
    saved_b = open(os.path.join(wb, "pokeplatinum-launcher.cfg")).read()
    assert "bind a=z,start=space" in saved_b, (
        "the launcher wrote its settings out and dropped the binding:\n"
        + saved_b)
    viewer = os.path.join(ROOT, "build", "pc", "pcview")
    if os.path.exists(viewer):
        env = dict(os.environ, SDL_VIDEODRIVER="dummy")
        good = subprocess.run([viewer, "--bind", "a=z,start=space",
                               "--wait", "1"], env=env, capture_output=True,
                              text=True, timeout=120)
        assert "--bind" not in good.stderr, (
            "the viewer refused the binding the launcher composes: %s"
            % good.stderr.strip()[:200])
        bad = subprocess.run([viewer, "--bind", "a=nosuchkey", "--wait", "1"],
                             env=env, capture_output=True, text=True,
                             timeout=120)
        assert bad.returncode == 2 and "nosuchkey" in bad.stderr, (
            "an unreadable binding was accepted (%d); a key that does nothing "
            "with no message is what that becomes" % bad.returncode)

    # 4. Asked once, remembered.
    cfg = os.path.join(w, "pokeplatinum-launcher.cfg")
    saved = open(cfg).read() if os.path.exists(cfg) else ""
    assert ("rom " + ROM) in saved, "no `rom %s` written to %s" % (ROM, cfg)
    r = _launch_run(lone, w, ["--keys", "return", "--dry-run"])
    again = _launch_cmd(r.stdout, "would run:")
    assert again and ROM in again, (
        "the remembered ROM did not survive into a second run with nothing in "
        "the environment naming one:\n" + (r.stdout + r.stderr)[-300:])

    # 4. A first run with nothing findable opens the picker by itself.
    picker = "printf '%%s\\n' %s" % ROM
    r = _launch_run(lone, work("first"), ["--keys", "return", "--dry-run"],
                    picker=picker)
    assert "asking for one" in r.stderr, (
        "no ROM was findable and nothing opened a dialog, so the menu would "
        "have sat on `(none found)` waiting to be understood:\n"
        + r.stderr[-300:])
    picked = _launch_cmd(r.stdout, "would run:")
    assert picked and ROM in picked, (
        "the picked ROM did not reach PLAY:\n" + (r.stdout + r.stderr)[-300:])

    # 5. The composed lines, given to the real programs. Both, together, the
    #    way PLAY runs them, the port publishing and the viewer presenting.
    #
    #    This one asks the INSTALLED launcher, not the lone copy above: the
    #    launcher names the two programs relative to itself, which is what
    #    makes a zip work and what makes the copy in an empty directory point
    #    at binaries that are not there.
    r = _launch_run(PCLAUNCH, work("real"), ["--keys", "return", "--dry-run"],
                    rom=ROM)
    port_cmd = _launch_cmd(r.stdout, "would run:")
    view_cmd = _launch_cmd(r.stdout, "would view:")
    assert port_cmd and view_cmd, (
        "the installed launcher composed nothing:\n" + (r.stdout + r.stderr)[-300:])
    assert os.path.exists(port_cmd[0]) and os.path.exists(view_cmd[0]), (
        "the launcher named programs that are not beside it: %s, %s"
        % (port_cmd[0], view_cmd[0]))

    shot = os.path.join(tmp, "launch.ppm")
    port = subprocess.Popen(port_cmd + ["--frames", "600", "--save", "none"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            text=True, cwd=ROOT)
    try:
        venv = dict(os.environ)
        venv.update({"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})
        v = subprocess.run(view_cmd + ["--shot", shot, "--wait", "20000"],
                           capture_output=True, text=True, env=venv,
                           cwd=ROOT, timeout=180)
    finally:
        port.terminate()
        try:
            port.wait(timeout=30)
        except subprocess.TimeoutExpired:
            port.kill()
    assert v.returncode == 0 and os.path.exists(shot), (
        "the launcher's own command lines did not produce a picture. This is "
        "the launcher offering a flag one of the two programs no longer "
        "takes.\n  port: %s\n  view: %s\n  %s"
        % (" ".join(port_cmd), " ".join(view_cmd),
           (v.stderr or v.stdout)[-400:]))
    assert os.path.getsize(shot) > 1000, "the shot is too small to be a frame"

    return ("Enter alone plays; neither neighbour does; a rebinding survives "
            "the round trip and the viewer honours it; the ROM is remembered; "
            "and both composed command lines drive the real port and viewer to "
            "a frame")


def test_dist(tmp):
    """The one zip, and that what comes out of it runs.

    A manifest listing would pass on an archive full of truncated files, so
    the two programs that can prove themselves without a ROM are extracted
    and run: the port's in-binary vector suites, and the launcher composing
    its command lines. That is the difference between "the zip has the right
    names in it" and "the zip is playable".

    Also pinned: nothing escapes the single top-level directory (a zip that
    unpacks into the user's cwd is the friction this task exists to remove),
    the executable bits survive (zipfile writes external_attr as 0 by default
    and a Linux player would unzip three files they cannot run), and the
    README's first line is still the whole installation procedure.
    """
    import zipfile

    if not os.path.exists(BINARY):
        return "skipped: no port binary to package"

    r = subprocess.run(["make", "-f", os.path.join(ROOT, "pc", "Makefile"),
                        "dist"], cwd=ROOT, capture_output=True, text=True,
                       timeout=900)
    assert r.returncode == 0, "make dist: %s" % (r.stderr.strip()[-400:] or "no output")

    zpath = os.path.join(ROOT, "build", "pc", "dist",
                         "pokeplatinum-pc-linux.zip")
    assert os.path.exists(zpath), "make dist succeeded and wrote no %s" % zpath

    want_exec = {"pokeplatinum-pc/pclaunch", "pokeplatinum-pc/pokeplatinum",
                 "pokeplatinum-pc/pcview"}
    want_plain = {"pokeplatinum-pc/LICENSE.txt", "pokeplatinum-pc/README.txt"}
    with zipfile.ZipFile(zpath) as zf:
        names = zf.namelist()
        roots = {n.split("/")[0] for n in names}
        assert roots == {"pokeplatinum-pc"}, \
            "the zip unpacks into %s, not one directory" % sorted(roots)
        for n in names:
            assert not n.startswith("/") and ".." not in n.split("/"), \
                "%s escapes the archive root" % n
        missing = sorted((want_exec | want_plain) - set(names))
        assert not missing, "the zip is missing %s" % missing
        for i in zf.infolist():
            mode = (i.external_attr >> 16) & 0o777
            assert i.filename not in want_exec or mode & 0o111, \
                "%s unzips without its executable bit (mode %o)" % (i.filename, mode)
        readme = zf.read("pokeplatinum-pc/README.txt").decode().splitlines()
        assert readme[0] == "unzip, double-click.", \
            "the README opens with %r" % readme[0]
        assert "GNU GENERAL PUBLIC LICENSE" in \
            zf.read("pokeplatinum-pc/LICENSE.txt").decode(), \
            "LICENSE.txt is not the licence"
        zf.extractall(tmp)
        # Python's extractall drops the mode; `unzip` does not. Applying the
        # archive's OWN recorded mode is what makes the two runs below an
        # end-to-end check: a zip that forgot the executable bit leaves these
        # files unrunnable here exactly as it would for a player.
        for i in zf.infolist():
            mode = (i.external_attr >> 16) & 0o777
            if mode:
                os.chmod(os.path.join(tmp, i.filename), mode)

    out = os.path.join(tmp, "pokeplatinum-pc")
    # From ROOT, explicitly: the extracted binary cannot find a ROM beside
    # its own exe (the zip ships none), so it falls back to the cwd-relative
    # build/rom default, which made this row pass from ROOT and fail from
    # pc/ (`make -C pc test`), looking exactly like a broken archive.
    r = subprocess.run([os.path.join(out, "pokeplatinum"), "--selftest"],
                       capture_output=True, text=True, timeout=120, cwd=ROOT)
    # The suite count is not pinned here: it grows, and this test is about the
    # archive rather than about how many vector suites there are.
    assert r.returncode == 0 and re.search(r"pc-selftest all\s+PASS",
                                           r.stdout + r.stderr), \
        "the extracted port fails its own vector suites (%d): %s" % (
            r.returncode, (r.stdout + r.stderr).strip()[-500:])
    r = subprocess.run([os.path.join(out, "pclaunch"), "--dry-run",
                        "--keys", "return"],
                       capture_output=True, text=True, timeout=120,
                       env=dict(os.environ, SDL_VIDEODRIVER="dummy",
                                SDL_AUDIODRIVER="dummy"))
    assert "would run" in r.stdout + r.stderr, \
        "the extracted launcher composes nothing (%d): %s" % (
            r.returncode, (r.stdout + r.stderr).strip()[-300:])

    return ("%.0f MB, one directory, %d entries; the extracted port passes its "
            "vector suites and the extracted launcher composes a command line"
            % (os.path.getsize(zpath) / 1e6, len(names)))


def test_fast_forward(tmp):
    """Held fast-forward changes the speed and nothing else.

    The second half is the one worth a test. Making a port go faster by
    dropping frames, skipping a VBlank or shortening a wait would all look
    like fast-forward on screen and would all be a different game underneath
   , and the whole value of holding it through a battle is that the battle
    is the same battle. So the pinned claim is that a fast-forwarded run and
    an unpaced run produce the same FRAMES, byte for byte, from the same
    script.

    The first half is pinned from both sides, because either one alone can
    pass on a broken pacer: a paced run of 180 frames must take about three
    seconds, and a fast-forwarded run of the same length must not.
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}

    def start(name, frames, dumpdir, turbo, pace="1"):
        e = dict(env, PC_SAVE="none", PC_VIEW=name, PC_FRAMES=str(frames),
                 PC_KEEP_ALIVE="1", PC_PACE=pace,
                 PC_INPUT=os.path.join(ROOT, "pc", "replays", "new-game.txt"))
        if dumpdir:
            e["PC_DUMP_FRAMES"] = dumpdir
            e["PC_DUMP_FROM"] = "0:10"
        p = subprocess.Popen([BINARY], env=e, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        path = "/dev/shm/" + name
        deadline = time.time() + 90
        while time.time() < deadline:
            if os.path.exists(path) and os.path.getsize(path) > VIEW_HDR + 2 * VIEW_PLANE:
                break
            time.sleep(0.02)
        assert os.path.exists(path), "the port never published a channel"
        with open(path, "r+b") as f:
            hdr = view_header(f.read(VIEW_HDR))
            assert hdr["version"] == VIEW_VERSION, \
                "channel version %d, this test knows %d" % (hdr["version"],
                                                            VIEW_VERSION)
            if turbo:
                f.seek(view_in_off("in_turbo"))
                f.write(struct.pack("<I", 1))
        t0 = time.monotonic()
        rc = p.wait(timeout=600)
        dt = time.monotonic() - t0
        try:
            os.unlink(path)
        except OSError:
            pass
        assert rc == 0, "the port exited %d" % rc
        return dt

    names = ["ff-%d-%s" % (os.getpid(), k) for k in ("paced", "held", "unpaced")]
    try:
        paced = start(names[0], 180, None, False)
        held = start(names[1], 900, os.path.join(tmp, "held"), True)
        unpaced = start(names[2], 900, os.path.join(tmp, "unpaced"), False,
                        pace="0")
    finally:
        for n in names:
            try:
                os.unlink("/dev/shm/" + n)
            except OSError:
                pass

    # 180 frames is 3.0 s at 60 Hz. A pacer that had stopped pacing would
    # finish this in a fraction of a second.
    assert paced > 2.4, \
        "180 paced frames took %.2f s; the pacer is not pacing" % paced
    # And the held run must actually be uncapped: 900 frames is 15 s paced.
    assert held < 7.5, \
        "900 held frames took %.2f s, fast-forward is not uncapping" % held

    a = open(os.path.join(tmp, "held", "frames.txt"), "rb").read()
    b = open(os.path.join(tmp, "unpaced", "frames.txt"), "rb").read()
    assert a and a == b, \
        "a fast-forwarded run drew a different picture (%d vs %d bytes)" % (
            len(a), len(b))

    return ("180 paced frames take %.1f s and 900 held take %.1f s (%.1fx "
            "realtime); the held run's %d frames are byte-identical to an "
            "unpaced run's"
            % (paced, held, (900 / 60.0) / held, a.count(b"\n")))


def test_hd3d(tmp):
    """Internal resolution: the 3D layer gains pixels and the 2D art does not.

    Three claims. A filter that invented sub-pixels for the 2D art would be
    inventing detail the artists did not draw, so the touch screen, which
    carries no 3D at all, must come out an EXACT SxS replication of the same
    frame rendered natively, every pixel, at every scale offered. The top
    screen must not, or the internal resolution is doing nothing.

    And the third, which is what the recording behind the feature costs: at
    any scale above 1 the 2D engine lays the 3D layer down at every column the
    window admits (a phantom where the rasterizer drew nothing) so that it
    can tell a pixel the 3D layer is showing at from one a sprite covered, and
    takes the phantoms back out before the blend unit runs. PC_HD3D_RECORD
    arms that over a NATIVE render, where the published frame must not move by
    a pixel. Without this the claim "it puts everything back" is untested,
    because at a scale above 1 there is nothing to compare against.

    The ports run concurrently and are matched by published frame number,
    because the frame the port draws is the same frame either way: this feeds
    them the same script and compares one frame against one frame.

    Frame 3200 is the target: the Jubilife station's own pinned frame, a
    CONTINUE boot standing in the town with the 3D layer live on the top
    screen. The recorded new-game flow used to serve here at frame 6000,
    five thousand frames of boot, intro and naming with no 3D layer on the
    top screen at all, and was discarded with the other new-game rows: a
    station reaches a real 3D scene in half the frames with none wasted.
    """
    TARGET = 3200
    # 4 is the ceiling and it is behind PC_TEST_LONG for one reason: at
    # sixteen times the pixels the boot to the station is minutes rather
    # than the seconds 2 costs. What only 4 can say is that the ceiling is
    # real, the sort key that used to lose every translucent polygon up
    # there fits at any scale now, and pc/ci.sh asks.
    SCALES = (2, 4) if os.environ.get("PC_TEST_LONG") == "1" else (2,)
    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    script = os.path.join(ROOT, "pc", "replays", "lab-continue.txt")

    # Minted fresh into this test's tmp rather than shared with test_corpus:
    # The workers run in parallel and a station artifact two tests reach for
    # is a race. Each port then gets a copy of the save, so no two runs open
    # one file.
    mint = os.path.join(tmp, "jubilife.sav")
    r = subprocess.run([sys.executable,
                        os.path.join(ROOT, "pc", "tests", "pc_lab.py"),
                        "--out", mint,
                        os.path.join(ROOT, "pc", "tests", "corpus",
                                     "jubilife.recipe")],
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, ("minting the station failed: "
                               + (r.stderr or r.stdout)[-400:])

    def start(name, hd, record=False):
        # PC_FRAMES is the TARGET itself, so each port publishes the frame
        # being compared and then parks on it. Asking for a frame a running
        # port is passing through is a four-millisecond window, and four ports
        # of very different speeds plus a loaded machine is exactly the
        # arrangement that makes a reader miss it and then wait out its whole
        # deadline. The page stays mapped and readable after the port exits.
        sav = os.path.join(tmp, "jub-%s.sav" % name)
        shutil.copy(mint, sav)
        e = dict(env, PC_SAVE=sav, PC_VIEW=name, PC_FRAMES=str(TARGET),
                 PC_KEEP_ALIVE="1", PC_PACE="0", PC_INPUT=script)
        if hd > 1:
            e["PC_HD3D"] = str(hd)
        if record:
            e["PC_HD3D_RECORD"] = "1"
        return subprocess.Popen([BINARY], env=e, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)

    def grab(m, p, target, wait=600):
        """The published frame `target`, read under the seqlock.

        Sleeps between looks rather than spinning: four ports are rendering
        while this waits, and a reader that holds a core to itself takes it
        from the one it is waiting for.
        """
        deadline = time.time() + wait
        while time.time() < deadline:
            hdr = view_header(m)
            if hdr["seq"] & 1 or hdr["frame_lo"] != target:
                if p.poll() is not None and hdr["frame_lo"] < target:
                    raise AssertionError("the port ended at frame %d, before "
                                         "%d" % (hdr["frame_lo"], target))
                time.sleep(0.01)
                continue
            w, h = hdr["width"], hdr["height"]
            a = m[VIEW_HDR:VIEW_HDR + w * h * 4]
            b = m[VIEW_HDR + VIEW_PLANE:VIEW_HDR + VIEW_PLANE + w * h * 4]
            if view_header(m)["seq"] == hdr["seq"]:
                return w, h, a, b
        raise AssertionError("frame %d never appeared (the port reached %d)"
                             % (target, view_header(m)["frame_lo"]))

    tags = [("1", 1, False), ("rec", 1, True)] \
        + [(str(s), s, False) for s in SCALES]
    names = ["hd-%d-%s" % (os.getpid(), t) for t, _, _ in tags]
    procs = []
    files = []
    maps = []
    got = {}
    try:
        procs = [start(n, hd, rec) for n, (_, hd, rec) in zip(names, tags)]
        # Every page is mapped before any of them is read: a fast port is done
        # and has unlinked its name long before the slow one is grabbed, and
        # a mapping made first outlives that.
        for n in names:
            deadline = time.time() + 120
            while time.time() < deadline and not (
                    os.path.exists("/dev/shm/" + n)
                    and os.path.getsize("/dev/shm/" + n) > VIEW_HDR + VIEW_PLANE):
                time.sleep(0.02)
            assert os.path.exists("/dev/shm/" + n), "no channel at /dev/shm/" + n
            fh = open("/dev/shm/" + n, "rb")
            files.append(fh)
            maps.append(mmap.mmap(fh.fileno(), 0, prot=mmap.PROT_READ))
        for m, p, (tag, hd, _) in zip(maps, procs, tags):
            # The wait scales with the work: at scale S the rasterizer does
            # S^2 the pixels and the compose S^2 the sub-pixels, and a port
            # that is merely slow must not read as a port that is wrong.
            got[tag] = grab(m, p, TARGET, 600 * hd * hd)
    finally:
        for p in procs:
            try:
                p.terminate()
                p.wait(timeout=60)
            except Exception:
                pass
        for m in maps:
            m.close()
        for fh in files:
            fh.close()
        for n in names:
            try:
                os.unlink("/dev/shm/" + n)
            except OSError:
                pass

    nw, nh, na, nb = got["1"]
    assert (nw, nh) == (VIEW_W, VIEW_H), \
        "the native run published %dx%d" % (nw, nh)
    assert got["rec"] == got["1"], \
        "PC_HD3D_RECORD moved the picture: the recording is supposed to put " \
        "the composed line back exactly as it found it"

    def replicated(plane, w, h, s):
        out = bytearray()
        for y in range(h):
            row = plane[y * w * 4:(y + 1) * w * 4]
            wide = bytearray()
            for x in range(w):
                wide += row[x * 4:x * 4 + 4] * s
            out += wide * s
        return bytes(out)

    seen = []
    for s in SCALES:
        hw, hh, ha, hb = got[str(s)]
        assert (hw, hh) == (VIEW_W * s, VIEW_H * s), \
            "--hd3d %d published %dx%d, not %dx%d" % (s, hw, hh,
                                                      VIEW_W * s, VIEW_H * s)
        ra, rb = replicated(na, nw, nh, s), replicated(nb, nw, nh, s)
        assert rb == hb, \
            "the touch screen carries no 3D, so at --hd3d %d it must be an " \
            "exact %dx%d replication, %d of %d pixels differ" % (
                s, s, s,
                sum(1 for i in range(0, len(rb), 4)
                    if rb[i:i + 4] != hb[i:i + 4]), hw * hh)
        differs = sum(1 for i in range(0, len(ra), 4)
                      if ra[i:i + 4] != ha[i:i + 4])
        assert differs > hw * hh // 100, \
            "at --hd3d %d the top screen is a replication too (%d of %d " \
            "pixels differ); the 3D layer was not rasterized any larger" % (
                s, differs, hw * hh)
        seen.append((s, hw, hh, differs))

    # And the instruments refuse to run beside it, for the reason the port
    # states: its own surfaces are a point-sample of a different rasterization.
    r = subprocess.run([BINARY, "--hd3d", "2", "--save", "none",
                        "--frames", "10", "--dump-frames",
                        os.path.join(tmp, "nope")],
                       env=env, capture_output=True, text=True, timeout=120)
    assert r.returncode == 2 and "point-sample" in r.stderr, \
        "--hd3d ran beside --dump-frames (%d): %s" % (r.returncode,
                                                      r.stderr.strip()[-200:])
    # ...and a scale the rasterizer cannot honour is refused at the door
    # rather than clamped into a picture the two halves disagree about.
    r = subprocess.run([BINARY, "--hd3d", str(VIEW_HD_MAX + 1), "--save",
                        "none", "--frames", "10"],
                       env=env, capture_output=True, text=True, timeout=120)
    assert r.returncode == 2 and "1..%d" % VIEW_HD_MAX in r.stderr, \
        "--hd3d %d was not refused (%d): %s" % (VIEW_HD_MAX + 1, r.returncode,
                                                r.stderr.strip()[-200:])

    return ("frame %d at " % TARGET
            + ", ".join("%dx%d" % (w, h) for _, w, h, _ in seen)
            + ": the touch screen is an exact replication at each and the top "
              "screen differs in "
            + ", ".join("%.1f%%" % (100.0 * d / (w * h))
                        for _, w, h, d in seen)
            + " of it; the recording moves no pixel over a native render and "
              "the frame instruments refuse to run beside any of it"
            + ("" if os.environ.get("PC_TEST_LONG") == "1"
               else " (PC_TEST_LONG=1 adds the 4x ceiling)"))


def test_polys(tmp):
    """The polygon-list instrument: what it prints, and that printing is free.

    Two claims. The list is the one the rasterizer drew with, the same
    polygon count and the same screen-space vertices, frame after frame from
    the same script, and asking for it does not change the run. That second
    one is the property --watch and --diff-trace are held to and it is the one
    with teeth: an instrument that perturbs what it measures is worse than no
    instrument, and this one runs inside the render path.
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    script = os.path.join(ROOT, "pc", "replays", "new-game.txt")
    at, after = 600, 610

    # WHAT "printing is free" can and cannot be checked against here, and it
    # was measured rather than assumed. Opening a file used to shift every
    # later host allocation and move the three regions that recorded host
    # addresses. The digest now skips those words by address, so a leftover
    # REPLAY_UNPINNABLE entry is the only reason to ignore a region here.
    # Measured three ways at 610 scripted frames with ASLR off, a range
    # that never fires and a range that dumps 450 polygons give the SAME
    # digest, and dumping to stdout instead of a file gives a third: the
    # difference tracked the fopen, not the dumping, and only in unmarked
    # host-pointer words.
    #
    # So the comparison is the frames themselves and every guest region
    # that is not in REPLAY_UNPINNABLE.
    def run(dump):
        e = dict(env, PC_SAVE="none", PC_FRAMES=str(after), PC_PACE="0",
                 PC_INPUT=script, PC_STATE_DIGEST="1",
                 PC_DUMP_FRAMES=os.path.join(tmp, "f" + (dump and "1" or "0")),
                 PC_DUMP_FROM="%d:1" % (at - 20))
        if dump:
            e["PC_DUMP_POLYS"] = "%d-%d" % (at, at + 1)
            e["PC_DUMP_POLYS_FILE"] = dump
        r = subprocess.run([BINARY], env=e, capture_output=True, text=True,
                           timeout=900)
        assert r.returncode == 0, "the port exited %d" % r.returncode
        regions = {}
        for l in r.stderr.splitlines():
            w = l.split()
            if (len(w) >= 5 and w[0] == "pc-state" and w[1] == "exit"
                    and w[2] != "TOTAL"):
                regions[w[-3]] = w[-1]
        assert regions, "no exit state digest"
        frames = open(os.path.join(e["PC_DUMP_FRAMES"], "frames.txt")).read()
        return regions, frames

    a = os.path.join(tmp, "a.txt")
    b = os.path.join(tmp, "b.txt")
    with_regions, with_frames = run(a)
    again_regions, again_frames = run(b)
    without_regions, without_frames = run(None)

    assert with_frames == without_frames, \
        "asking for the polygon list changed the picture"
    moved = [k for k in without_regions
             if k not in REPLAY_UNPINNABLE
             and with_regions.get(k) != without_regions[k]]
    assert not moved, (
        "asking for the polygon list changed guest memory at %s, and those "
        "are regions that hold no host addresses, so this is the instrument"
        % ", ".join(sorted(moved)))

    da, db = open(a).read(), open(b).read()
    assert da == db, "two identical runs printed different polygon lists"

    frames = {}
    for line in da.splitlines():
        if line.startswith("frame "):
            w = line.split()
            frames[int(w[1])] = int(w[3])
        elif line.startswith("  "):
            w = line.split()
            # frame, index, id, alpha, fmt, pal, attr, verts, z, w, flags,
            # then that many x,y pairs.
            nverts = int(w[7])
            pts = w[11:]
            assert len(pts) == nverts, \
                "a polygon says %d vertices and printed %d" % (nverts, len(pts))
            for pt in pts:
                x, _, y = pt.partition(",")
                int(x), int(y)
            assert 0 <= int(w[2]) <= 63, "polygon id %s out of range" % w[2]
            assert 0 <= int(w[3]) <= 31, "alpha %s out of range" % w[3]

    assert sorted(frames) == [at, at + 1], \
        "asked for frames %d-%d and got %s" % (at, at + 1, sorted(frames))
    assert all(n > 0 for n in frames.values()), \
        "the instrument printed a frame with no polygons in it: %r" % frames
    for f, n in frames.items():
        got = sum(1 for l in da.splitlines()
                  if l.startswith("  %5d " % f) or l.startswith("  %d " % f))
        assert got == n, \
            "frame %d says %d polygons and printed %d lines" % (f, n, got)

    return ("frames %d-%d: %s polygons, every vertex count and every "
            "id/alpha in range, identical across two runs%s"
            % (at, at + 1, "/".join(str(frames[k]) for k in sorted(frames)),
               ", the picture is unchanged and so is every guest region that "
               "does not hold host addresses"))


def test_save_durable(tmp):
    """The save is on disk when the game saves, not when the run ends.

    This is the trust claim a player with a two-hundred-hour file actually
    cares about, and the thing that would break it is somebody adding a flush
    at exit. So what is pinned here is the negative: over a run that does not
    save, the file is not touched, not by a clean exit, and not by the run
    being killed outright.

    A SYNTHETIC 512 KB image, not a real save, and deliberately: minting a
    real one costs a three-minute scripted run to reach an in-game save, and
    none of what is checked here needs the contents to mean anything. That a
    real save round-trips is a measurement rather than a test, for the same
    reason; see the phase notes.
    """
    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    script = os.path.join(ROOT, "pc", "replays", "new-game.txt")
    sav = os.path.join(tmp, "fixture.sav")
    with open(sav, "wb") as f:
        f.write(b"\xFF" * 524288)     # an erased flash chip, which is what a
                                      # blank cartridge save looks like
    before = open(sav, "rb").read()

    e = dict(env, PC_SAVE=sav, PC_FRAMES="2600", PC_PACE="0", PC_INPUT=script)
    r = subprocess.run([BINARY], env=e, capture_output=True, text=True,
                       timeout=900)
    assert r.returncode == 0, "the port exited %d" % r.returncode
    assert "524288 bytes" in r.stderr, (
        "the port did not report opening the 512 KB save it was given:\n"
        + r.stderr[:400])
    assert open(sav, "rb").read() == before, \
        "a run that never saved rewrote the save file on the way out"

    p = subprocess.Popen([BINARY], env=dict(e, PC_FRAMES="200000"),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 30
        while time.time() < deadline and p.poll() is None:
            time.sleep(0.5)
            if time.time() > deadline - 25:
                break
        assert p.poll() is None, "the port ended before it could be killed"
        p.kill()
        p.wait(timeout=60)
    finally:
        if p.poll() is None:
            p.kill()
            p.wait(timeout=60)
    after = open(sav, "rb").read()
    assert len(after) == 524288, \
        "the killed run left a %d-byte save" % len(after)
    assert after == before, "the killed run changed the save file"

    return ("512 KB opened and reported; a clean 2600-frame run and a killed "
            "one both leave it byte-identical")


WIN_BUILD = os.path.join(ROOT, "build", "pc-win32")
WIN_EXE = os.path.join(WIN_BUILD, "pokeplatinum.exe")
WIN_MAKEFILE = os.path.join(ROOT, "pc", "Makefile.win")


def pe_subsystem(path):
    """(subsystem, has_rsrc) from a PE image, without a toolchain."""
    with open(path, "rb") as f:
        d = f.read(0x400)
        off = struct.unpack_from("<I", d, 0x3C)[0]
        f.seek(0)
        # Enough for the section table, whose length depends on how many
        # sections there are; a fixed read was too short for the SDL exes
        # and reported "no resource section" for one that has one.
        d = f.read(off + 0x400)
        nsec, = struct.unpack_from("<H", d, off + 6)
        optsz, = struct.unpack_from("<H", d, off + 20)
        f.seek(0)
        d = f.read(off + 24 + optsz + nsec * 40)
    assert d[off:off + 4] == b"PE\x00\x00", "%s is not a PE image" % path
    sub, = struct.unpack_from("<H", d, off + 24 + 68)
    secs = off + 24 + optsz
    names = set()
    for i in range(nsec):
        names.add(d[secs + i * 40:secs + i * 40 + 8].rstrip(b"\x00"))
    return sub, b".rsrc" in names


def test_win_frames(tmp):
    """The Windows build draws the same frames as the Linux one.

    The cross build was verified by hand once and nothing re-checked it, so
    it could drift for weeks, the two share every line of C and none of the
    toolchain, and the ABI flags that keep guest struct layouts identical are
    a compile option away from being wrong.

    The exe cache is part of the test. Windows serves a stale image for a
    rebuilt exe at a fixed WSL path; it has happened twice here. So the run
    is a fresh-named copy, and without that this test would eventually pass
    on yesterday's build and say nothing.

    Also checked, because nothing else does: all three exes are GUI-subsystem
    and carry a resource section. A console box on a double-click is exactly
    the friction the launcher exists to remove, and it is invisible from
    Linux.
    """
    if not os.path.exists(WIN_EXE):
        return ("skipped: no %s, `make -f pc/Makefile.win status`"
                % os.path.relpath(WIN_EXE, ROOT))
    if subprocess.run(["bash", "-c", "command -v i686-w64-mingw32-gcc"],
                      capture_output=True).returncode != 0:
        return "skipped: no i686-w64-mingw32-gcc, so the exe cannot be trusted"

    # Never measure a stale exe: bring it up to date first. A no-op when it
    # already is, which is the usual case.
    #
    # NOT -j$(nproc), and the reason is the suite around it. This runs inside
    # a worker of an already-parallel suite, so when the exe IS stale it puts
    # a 24-way build next to seven other tests, and several of those have
    # wall-clock deadlines or measure the port's own frame rate. That is a
    # test failing because of what another test is doing to the machine,
    # which is the least debuggable kind of red there is. Half the box is
    # still a fast rebuild and leaves the rest of the suite room to be true.
    jobs = max(2, (os.cpu_count() or 4) // 2)
    b = subprocess.run(["make", "-f", WIN_MAKEFILE, "-j%d" % jobs,
                        "status"], cwd=ROOT, capture_output=True, text=True,
                       timeout=3600)
    assert b.returncode == 0 and "link: ok" in b.stdout, (
        "the Windows build is not current and would not build:\n"
        + (b.stdout + b.stderr)[-800:])

    for name in ("pokeplatinum.exe", "pcview.exe", "pclaunch.exe"):
        path = os.path.join(WIN_BUILD, name)
        if not os.path.exists(path):
            continue
        sub, rsrc = pe_subsystem(path)
        assert sub == 2, (
            "%s is subsystem %d, not 2 (GUI); a double-click would flash a "
            "console box" % (name, sub))
        assert rsrc, "%s has no resource section, so it has no icon" % name

    fresh = os.path.join(WIN_BUILD, "wintest-%d.exe" % os.getpid())
    shutil.copy(WIN_EXE, fresh)
    frames = 1200
    script = os.path.join(ROOT, "pc", "replays", "new-game.txt")
    wdump, ldump = os.path.join(tmp, "win"), os.path.join(tmp, "lin")
    try:
        r = subprocess.run(
            [fresh, "PC_SAVE=none", "PC_FRAMES=%d" % frames, "PC_PACE=0",
             "PC_INPUT=" + script, "PC_DUMP_FRAMES=" + wdump,
             "PC_DUMP_AUDIO=" + os.path.join(tmp, "win.wav")],
            capture_output=True, text=True, timeout=1800)
        if r.returncode != 0 or not os.path.exists(
                os.path.join(wdump, "frames.txt")):
            return ("skipped: the exe would not run here (%d), WSL interop "
                    "is how this test reaches Windows: %s"
                    % (r.returncode, (r.stderr or r.stdout).strip()[-200:]))
    finally:
        try:
            os.unlink(fresh)
        except OSError:
            pass

    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    env.update({"PC_SAVE": "none", "PC_FRAMES": str(frames), "PC_PACE": "0",
                "PC_INPUT": script, "PC_DUMP_FRAMES": ldump,
                "PC_DUMP_AUDIO": os.path.join(tmp, "lin.wav")})
    r = subprocess.run([BINARY], env=env, capture_output=True, text=True,
                       timeout=1800)
    assert r.returncode == 0, "the Linux port exited %d" % r.returncode

    win = open(os.path.join(wdump, "frames.txt"), "rb").read()
    lin = open(os.path.join(ldump, "frames.txt"), "rb").read()
    if win != lin:
        wl, ll = win.split(b"\n"), lin.split(b"\n")
        first = next((i for i in range(min(len(wl), len(ll)))
                      if wl[i] != ll[i]), None)
        raise AssertionError(
            "the two builds draw different frames from frame %s on:\n"
            "  windows %s\n  linux   %s"
            % (first, wl[first][:90] if first is not None else "?",
               ll[first][:90] if first is not None else "?"))

    # The sound as well, and it costs nothing: both runs were going to
    # happen anyway. The mixer is where a float or long-long ABI difference
    # between the two toolchains would show up as something a player hears.
    wav = [open(os.path.join(tmp, n), "rb").read() for n in ("win.wav",
                                                             "lin.wav")]
    assert wav[0] == wav[1], (
        "the two builds mixed different audio over the same %d frames "
        "(%d and %d bytes)" % (frames, len(wav[0]), len(wav[1])))

    # The launcher, on windows, which is where its one windows-only behaviour
    # LIVES: it copies the game to a name nothing has cached before starting
    # it, because Windows serves stale images at fixed paths. The copies are
    # pruned to a small ring, and the name is printed so a session can say
    # which image it ran.
    launcher = os.path.join(WIN_BUILD, "pclaunch.exe")
    if os.path.exists(launcher):
        for stale in glob.glob(os.path.join(WIN_BUILD, "pplay-*.exe")):
            try:
                os.unlink(stale)
            except OSError:
                pass
        seen = set()
        for _ in range(5):
            r = subprocess.run([launcher, "--dry-run", "--keys", "return"],
                               capture_output=True, text=True, timeout=300)
            line = [l for l in r.stderr.splitlines()
                    if l.startswith("pclaunch: running ")]
            assert line, (
                "the Windows launcher did not say which image it ran:\n"
                + (r.stderr or r.stdout)[-400:])
            assert "pplay-" in line[0], (
                "it ran the fixed path, which Windows may have cached: %s"
                % line[0])
            assert line[0].split("running ")[1] in r.stdout, (
                "it printed one image and composed a command line with "
                "another:\n%s\n%s" % (line[0], r.stdout[:200]))
            seen.add(line[0])
            time.sleep(0.05)
        assert len(seen) == 5, \
            "five launches reused a name: %d distinct" % len(seen)
        ring = glob.glob(os.path.join(WIN_BUILD, "pplay-*.exe"))
        assert len(ring) <= 3, \
            "five launches left %d copies behind; the ring is 3" % len(ring)
        for stale in ring:
            try:
                os.unlink(stale)
            except OSError:
                pass

    return ("%d frames and %.1f s of audio byte-identical between the mingw "
            "and gcc builds, run from a fresh-named copy; all three exes are "
            "GUI-subsystem with a resource section, and the launcher runs a "
            "fresh copy it names and prunes to three"
            % (frames, (len(wav[0]) - 44) / 4.0 / 32728))


# Which tests need build/pc/pokeplatinum. The plan checks deliberately do not:
# A plan that has drifted should be catchable on a tree that will not build,
# which is exactly when someone is most likely to be reading it.
# Battle mechanics whose result is arithmetic rather than a roll, and the
# arithmetic stated HERE rather than read back off the port. That is the whole
# point of this table: every other battle check in this suite compares the port
# against a recording of itself, so all of them would still pass if the damage
# formula were wrong in a stable way. These would not.
#
# `expect` is given the trace line's own fields and returns the delta the game
# owes. It may use the defender's current hp (that is what Super Fang halves)
# but never anything the port computed about damage.
#
# Type immunity is the one thing that would silently make a case vacuous, a
# Ghost is immune to Sonic Boom and a Normal to Night Shade, and an immune move
# produces no HP line at all, so each case names a foe the move can touch,
# and a case that produces no line fails rather than passing quietly.
BATTLE_MATH = (
    ("seismic-toss", "MOVE_SEISMIC_TOSS", 37, 396, 30,
     "Seismic Toss deals the user's level",
     lambda user_level, foe_hp: -user_level),
    ("night-shade", "MOVE_NIGHT_SHADE", 37, 393, 30,
     "Night Shade deals the user's level",
     lambda user_level, foe_hp: -user_level),
    ("dragon-rage", "MOVE_DRAGON_RAGE", 37, 393, 30,
     "Dragon Rage deals 40, whatever the levels",
     lambda user_level, foe_hp: -40),
    ("sonic-boom", "MOVE_SONIC_BOOM", 37, 396, 30,
     "Sonic Boom deals 20, whatever the levels",
     lambda user_level, foe_hp: -20),
    ("super-fang", "MOVE_SUPER_FANG", 37, 396, 30,
     "Super Fang halves the target's current HP",
     lambda user_level, foe_hp: -(foe_hp // 2)),
)


def test_text_decode(tmp):
    """Every message bank decodes, and the ones that can be read match their source.

    The oracle is the source the messages were compiled from. `res/text/*.json`
    is human-authored English; the NARC the ROM build produces is that text run
    through two ciphers. pc/tests/pc_text.py restates both ciphers from
    src/message.c, decodes the NARC and compares the strings. Nothing here
    compares the port against a recording of the port, if the decode were
    wrong, this is the only check in the suite that would say so.

    Comparison is restricted to messages that are plain Latin on both sides,
    because the charset the decoder maps is a Latin block rather than a font: a
    Japanese or icon glyph would fail on the RENDERING and prove nothing about
    the decode. What is checked over all of them is that every bank's entry
    table stays inside its own bank, which is the out-of-range trap 9.19 wants.
    """
    narc = os.path.join(ROOT, "build", "rom", "res", "text", "pl_msg.narc")
    if not os.path.exists(narc):
        return "skipped: no build/rom/res/text/pl_msg.narc (build the res pipeline)"

    sys.path.insert(0, os.path.join(ROOT, "pc", "tests"))
    import pc_text

    banks = pc_text.narc_files(narc)
    assert banks, "the message NARC holds no banks"

    total, refused = 0, []
    for i, blob in enumerate(banks):
        try:
            total += len(pc_text.decode_bank(blob))
        except ValueError as e:
            refused.append("bank %d: %s" % (i, e))
    assert not refused, (
        "%d bank(s) have an entry table that runs outside the bank:\n  %s"
        % (len(refused), "\n  ".join(refused[:6])))

    table, eos = pc_text.charset()
    src = pc_text.source_banks()
    matched = counted = 0
    bad = []
    for bank_id, want in sorted(src.items()):
        if bank_id >= len(banks):
            continue
        got = pc_text.decode_bank(banks[bank_id])
        if len(got) != len(want):
            bad.append("bank %d holds %d message(s), its source has %d"
                       % (bank_id, len(got), len(want)))
            continue
        counted += 1
        for i, (chars, text) in enumerate(zip(got, want)):
            if text is None:
                continue
            plain = re.sub(r"\{[^}]*\}", "", text)
            shown = pc_text.render(chars, table, eos)
            if not plain or "\ufffd" in shown:
                continue
            if shown != plain:
                bad.append("bank %d entry %d decodes to %r, its source says %r"
                           % (bank_id, i, shown, plain))
            else:
                matched += 1

    assert not bad, ("the decode disagrees with the text it was compiled from "
                     "(%d):\n  %s" % (len(bad), "\n  ".join(bad[:6])))
    assert matched > 1000, (
        "only %d message(s) were comparable, which is too few to have checked "
        "anything, the charset or the bank mapping has drifted" % matched)

    return ("%d bank(s), %d message(s) decode with every entry table inside its "
            "own bank; %d bank(s) carry a readable source and %d message(s) "
            "decode to exactly the English they were compiled from"
            % (len(banks), total, counted, matched))


def test_clock_rollover(tmp):
    """The same save, booted later, moves the day and the minute timers by exactly that much.

    The deterministic clock makes time an input, so a daily event is testable
    without waiting for one. `GameTime.day` is the single number every daily
    system in the game hangs off, sub_02055A14 compares it against today and
    calls FieldSystem_HandleDailyEvents with the difference, so booting one
    save on a later date and reading the day back through the save reader
    observes the rollover without watching anything on screen.

    The honey tree is the same question one level down, and it is here because
    it is the one clock-driven system whose answer is arithmetic rather than a
    recording. Slathering leaves a tree with 1440 minutes on it and
    SpecialEncounter_DecrementHoneyTreeTimers subtracts the minutes that have
    passed, floored at zero, so with the elapsed span read out of the two
    saves' own clocks, the counter the port must hold is a number this test
    computes. The corpus pins the same values, but a pin agrees with whatever
    the port did last time and this does not.

    The expectation throughout is arithmetic the test owns: two days later is
    two days, not "whatever the port said last time".
    """
    if not os.path.exists(BINARY) or not os.path.exists(ROM):
        return "skipped: no binary or ROM"

    sys.path.insert(0, os.path.join(ROOT, "pc", "tests"))
    import pc_lab, pc_save

    # What HoneyTree_SlatherTree leaves on a tree, a day's worth of minutes.
    # The recipe writes it, so the expectation below is this number minus the
    # elapsed span rather than anything the game has to agree about.
    SLATHERED = 24 * 60

    recipe = os.path.join(tmp, "clock.recipe")
    with open(recipe, "w") as f:
        f.write("name CLOCK\ngender GENDER_MALE\ntrainer-id 50001\n"
                "money 3000\nparty SPECIES_MONFERNO 18 ITEM_NONE\n"
                "honey 0 %d\n"
                "map MAP_HEADER_JUBILIFE_CITY 0xB4 0x309 FACE_DOWN\n" % SLATHERED)
    sav = os.path.join(tmp, "clock.sav")
    pc_lab.mint(recipe, sav)
    minted = pc_save.Save(sav)
    before, minted_at = minted.game_day, minted.minute_of_epoch
    assert minted.honey_trees[0]["minutesRemaining"] == SLATHERED, (
        "the recipe did not leave honey tree 0 slathered; it holds %d minutes"
        % minted.honey_trees[0]["minutesRemaining"])

    # A recipe with nothing to change: what matters is that the field runs on
    # the later date and saves again, which is the same derived-station path a
    # recipe applied to an existing save takes.
    again = os.path.join(tmp, "clock2.recipe")
    with open(again, "w") as f:
        f.write("money 3000\n")
    lab = os.path.join(tmp, "clock2.lab")
    pc_lab.compile_recipe(again, lab)

    checked = []
    for days, when in ((0, "2009-03-22 16:00:00"),
                       (1, "2009-03-23 13:00:00"),
                       (2, "2009-03-24 13:00:00")):
        run = os.path.join(tmp, "clock-%d.sav" % days)
        shutil.copy(sav, run)
        env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        env.update({"PC_SAVE": run, "PC_LAB": lab, "PC_LAB_AT": "1800",
                    "PC_RTC": when, "PC_FRAMES": "6000", "PC_PACE": "0",
                    "PC_INPUT": os.path.join(ROOT, "pc", "replays",
                                             "lab-continue.txt")})
        r = subprocess.run([BINARY], env=env, capture_output=True, text=True,
                           timeout=1800)
        assert r.returncode == 0, ("the port exited %d booting the save on %s\n%s"
                                   % (r.returncode, when, r.stderr[-400:]))
        booted = pc_save.Save(run)
        after = booted.game_day
        assert after == before + days, (
            "booting the save %d day(s) later left GameTime.day at %d, and it "
            "was %d before, a difference of %d, not %d. Every daily system "
            "hangs off this number." % (days, after, before, after - before, days))

        # The elapsed span is the difference between the two saves' own clocks,
        # so nothing here assumes how long the boot took to reach the field.
        elapsed = booted.minute_of_epoch - minted_at
        want = max(0, SLATHERED - elapsed)
        got = booted.honey_trees[0]["minutesRemaining"]
        assert got == want, (
            "%d minute(s) passed, so honey tree 0 owes %d minute(s) and holds "
            "%d. The tree is slathered for %d and the timers are decremented by "
            "the elapsed minutes, floored at zero."
            % (elapsed, want, got, SLATHERED))
        checked.append("+%d day / %d min, tree %d" % (days, elapsed, got))

    return ("the save's day was %d; booting it later moved it by exactly %s, "
            "which is what every daily event is driven from"
            % (before, "; ".join(checked)))


def test_battle_math(tmp):
    """Fixed-damage moves, against expectations this test computes itself.

    The oracle here is arithmetic, which is what makes this different from
    every other battle check in the suite. A pinned digest, a pinned save, a
    replay that still plays, all of those compare the port against a
    recording of the port, and all of them would keep passing if the damage
    formula were wrong in a stable way. Seismic Toss owes the user's level
    because that is what the move is, and no amount of agreement between two
    runs establishes it.

    It is also the one battle oracle that needs no alignment with melonDS: the
    game's own rules are the specification, so there is nothing to synchronise
    and no seed to share.

    Each case builds its own party with the lab, a chosen species, a chosen
    level, and the move under test in slot 0, because a station driven with A
    alone uses slot 0 every turn, then reads the HP trace the battle emits.
    """
    if not os.path.exists(BINARY):
        return "skipped: no port binary"
    if not os.path.exists(ROM):
        return "skipped: no ROM"

    sys.path.insert(0, os.path.join(ROOT, "pc", "tests"))
    import pc_lab

    replay = os.path.join(ROOT, "pc", "replays", "lab-battle.txt")
    assert os.path.exists(replay), "pc/replays/lab-battle.txt is missing"

    def run(case):
        name, move, level, foe, foe_level, _why, _expect = case
        recipe = os.path.join(tmp, name + ".recipe")
        with open(recipe, "w") as f:
            f.write("name MATH\ngender GENDER_MALE\ntrainer-id 30001\n"
                    "money 3000\nparty SPECIES_MACHOKE %d ITEM_NONE\n"
                    "party-move 0 0 %s\n"
                    "map MAP_HEADER_JUBILIFE_CITY 0xB4 0x309 FACE_DOWN\n"
                    % (level, move))
        sav = os.path.join(tmp, name + ".sav")
        pc_lab.mint(recipe, sav)

        env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
        env.update({"PC_SAVE": sav, "PC_FRAMES": "5000", "PC_PACE": "0",
                    "PC_INPUT": replay, "PC_TRACE_BATTLE": "1",
                    "PC_LAB_BATTLE": "wild %d %d" % (foe, foe_level),
                    "PC_LAB_BATTLE_AT": "2600"})
        r = subprocess.run([BINARY], env=env, capture_output=True, text=True,
                           timeout=1800)
        return name, r

    import concurrent.futures

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        for name, r in pool.map(run, BATTLE_MATH):
            results[name] = r

    # Leech Seed, kept separate because it is asserted on a POSITIVE delta and
    # that is what makes it sound. Every other residual, burn, poison,
    # sandstorm, lands as a negative delta on the target, and so do the foe's
    # own attacks and its recoil, and the trace's `move` field is the current
    # move rather than the source of the tick, so "which -8 was the burn" is
    # not answerable from the stream. Nothing else in these battles heals the
    # user, so the drain is unambiguous, and it is the same eighth-of-max-HP
    # rule the whole residual class uses.
    seed_note = _battle_leech_seed(tmp, pc_lab, replay)

    checked = []
    for case in BATTLE_MATH:
        name, move, level, foe, foe_level, why, expect = case
        r = results[name]
        assert r.returncode == 0, ("%s: the port exited %d\n%s"
                                   % (name, r.returncode, r.stderr[-500:]))
        lines = []
        for line in r.stderr.split("\n"):
            if not line.startswith("pc-battle:"):
                continue
            f = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
            lines.append(f)

        # The move under test, landing on the FOE (battler 1). A move the foe
        # is immune to emits nothing, which is why an empty list is a failure
        # and not a pass.
        hits = [f for f in lines
                if f.get("species") == str(foe) and int(f["delta"]) < 0
                and f["move"] == str(pc_lab.resolve(move, "battle math"))]
        assert hits, (
            "%s never landed on species %d in 5000 frames; either the battle "
            "did not start, or the foe is immune and the case proves nothing.\n"
            "%d HP line(s) seen: %s"
            % (move, foe, len(lines), lines[:4]))

        hit = hits[0]
        foe_hp = int(hit["hp"].split("/")[0])
        want = expect(level, foe_hp)
        got = int(hit["delta"])
        assert got == want, (
            "%s: %s, so a level %d user should have dealt %d to a %d-HP "
            "target and dealt %d instead." % (name, why, level, -want, foe_hp, -got))
        checked.append("%s %d" % (name, -got))

    return ("%d fixed-damage rule(s) hold against arithmetic computed in the "
            "test, not read off the port: %s; %s"
            % (len(checked), ", ".join(checked), seed_note))


def _battle_leech_seed(tmp, pc_lab, replay):
    """Leech Seed drains an eighth of the TARGET's maximum HP to the user."""
    recipe = os.path.join(tmp, "leech.recipe")
    with open(recipe, "w") as f:
        f.write("name MATH\ngender GENDER_MALE\ntrainer-id 30001\n"
                "money 3000\nparty SPECIES_MACHOKE 37 ITEM_NONE\n"
                "party-move 0 0 MOVE_LEECH_SEED\n"
                "map MAP_HEADER_JUBILIFE_CITY 0xB4 0x309 FACE_DOWN\n")
    sav = os.path.join(tmp, "leech.sav")
    pc_lab.mint(recipe, sav)

    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    env.update({"PC_SAVE": sav, "PC_FRAMES": "6000", "PC_PACE": "0",
                "PC_INPUT": replay, "PC_TRACE_BATTLE": "1",
                "PC_LAB_BATTLE": "wild 396 30", "PC_LAB_BATTLE_AT": "2600"})
    r = subprocess.run([BINARY], env=env, capture_output=True, text=True,
                       timeout=1800)
    assert r.returncode == 0, "leech seed: the port exited %d" % r.returncode

    rows = []
    for line in r.stderr.split("\n"):
        if line.startswith("pc-battle:"):
            rows.append(dict(kv.split("=", 1) for kv in line.split()[1:]
                             if "=" in kv))
    foe = [f for f in rows if f.get("species") == "396"]
    assert foe, "leech seed: the battle produced no HP line for the target"
    foe_max = int(foe[0]["hp"].split("/")[1])
    drains = [int(f["delta"]) for f in rows
              if f.get("species") == "67" and int(f["delta"]) > 0]
    assert drains, ("leech seed drained nothing in 6000 frames; either it "
                    "never landed or the drain does not reach the user")
    want = foe_max // 8
    assert drains[0] == want, (
        "Leech Seed drains an eighth of the target's maximum HP, so a %d-HP "
        "target owes %d and gave %d" % (foe_max, want, drains[0]))
    return ("Leech Seed drained %d, an eighth of the target's %d"
            % (drains[0], foe_max))


def test_save_lab(tmp):
    """A recipe minted by the lab reads back as exactly what it asked for.

    Two subsystems arrive together and each is the other's oracle: the port's
    --lab writes a save through the game's own setters, and pc/tests/pc_save.py
    reads one back through the game's own layout. Neither is checked by
    anything else in the suite, the replay drives a new game and never mints
    a save at all, so this is the property test that keeps them honest: write
    a recipe, read it back, every field survives.

    It also pins the two things the lab got wrong on the way in, because both
    produced saves that LOOKED right:

      * A save with no saved map objects is refused by the CONTINUE path, so
        the lab runs FieldSystem_SaveObjects the way the game's own save does;
      * a station's map is loaded for real rather than written into the save,
        because the objects belong to whatever map the player was standing on.

    Neither shows up in the save's own fields, which is why the test boots the
    minted save and looks at the screen rather than stopping at the bytes.
    """
    sys.path.insert(0, os.path.join(ROOT, "pc", "tests"))
    import pc_lab, pc_save

    recipe = os.path.join(tmp, "station.recipe")
    with open(recipe, "w") as f:
        f.write("""# every verb that has an answer in the save
name TESTER
gender GENDER_MALE
trainer-id 4242
money 54321
badge BADGE_ID_COAL
badge BADGE_ID_FOREST
party SPECIES_TURTWIG 12 ITEM_ORAN_BERRY
party SPECIES_STARLY 7 ITEM_NONE
item ITEM_POKE_BALL 20
item ITEM_POTION 5
flag FLAG_UNLOCKED_VALLEY_WINDWORKS_DOOR
var VAR_OREBURGH_GATE_1F_HIKER_STATE 3
poketch POKETCH_APPID_PEDOMETER
poketch-steps 42
map MAP_HEADER_TWINLEAF_TOWN 0x74 0x376 FACE_DOWN
""")

    sav = os.path.join(tmp, "station.sav")
    pc_lab.mint(recipe, sav)
    save = pc_save.Save(sav)

    assert save.result == "ok", (
        "the minted save does not arbitrate clean: %s %s"
        % (save.result, save.errors))
    assert not save.errors, "footer errors in a freshly minted save: %s" % save.errors

    assert save.name == "TESTER", "name read back as %r" % save.name
    assert save.trainer_id == 4242, "trainer id read back as %d" % save.trainer_id
    assert save.money == 54321, "money read back as %d" % save.money
    assert save.badges == 2, "badge count read back as %d" % save.badges

    consts = pc_lab.constants()
    assert save.position[0] == consts["MAP_HEADER_TWINLEAF_TOWN"], (
        "the station is on map %d, not the one the recipe named" % save.position[0])
    assert save.position[1] == 0x74 and save.position[2] == 0x376, \
        "the station is at %s, not where the recipe put it" % (save.position[1:3],)

    party = save.party
    assert len(party) == 2, "party has %d member(s), recipe gave 2" % len(party)
    assert party[0]["species"] == consts["SPECIES_TURTWIG"], \
        "party slot 0 is species %d" % party[0]["species"]
    assert party[0]["level"] == 12, "party slot 0 is level %d" % party[0]["level"]
    assert party[0]["held_item"] == consts["ITEM_ORAN_BERRY"], \
        "party slot 0 holds item %d" % party[0]["held_item"]
    assert party[1]["species"] == consts["SPECIES_STARLY"], \
        "party slot 1 is species %d" % party[1]["species"]
    for i, mon in enumerate(party):
        assert mon["checksum_ok"], "party slot %d fails its own checksum" % i
        assert mon["max_hp"] > 0 and mon["hp"] == mon["max_hp"], \
            "party slot %d has %d/%d HP" % (i, mon["hp"], mon["max_hp"])

    bag = save.bag
    assert bag.get(consts["ITEM_POKE_BALL"]) == 20, "bag has %s" % bag
    assert bag.get(consts["ITEM_POTION"]) == 5, "bag has %s" % bag

    assert save.flag(consts["FLAG_UNLOCKED_VALLEY_WINDWORKS_DOOR"]), "the recipe's flag is clear"
    assert save.var(consts["VAR_OREBURGH_GATE_1F_HIKER_STATE"]) == 3, \
        "the recipe's var read back as %d" % save.var(
            consts["VAR_OREBURGH_GATE_1F_HIKER_STATE"])

    poketch = save.poketch
    assert poketch["enabled"], "the recipe enabled the Poketch, the save did not"
    assert poketch["app"] == consts["POKETCH_APPID_PEDOMETER"], \
        "poketch app is %s, not the pedometer" % poketch["app"]
    assert poketch["steps"] == 42, "poketch steps read back as %d" % poketch["steps"]

    # Same recipe, same bytes. A lab that drifted between runs would make
    # every pinned digest downstream of it worthless.
    twin = os.path.join(tmp, "twin.sav")
    pc_lab.mint(recipe, twin)
    assert open(sav, "rb").read() == open(twin, "rb").read(), \
        "two mints of the same recipe produced different saves"

    # And it is a save the game will actually continue from. The screen is the
    # evidence, because the failure this catches leaves every field correct.
    env = {k: v for k, v in os.environ.items() if not k.startswith("PC_")}
    script = os.path.join(ROOT, "pc", "replays", "lab-continue.txt")
    frames = os.path.join(tmp, "continue")
    r = subprocess.run([BINARY], timeout=900, capture_output=True, text=True,
                       env=dict(env, PC_SAVE=sav, PC_INPUT=script,
                                PC_FRAMES="3200", PC_PACE="0",
                                PC_DUMP_FRAMES=frames, PC_DUMP_FROM="3200"))
    assert r.returncode == 0, "continuing from the minted save exited %d:\n%s" % (
        r.returncode, r.stderr[-800:])
    png = os.path.join(frames, "frame-003200.png")
    assert os.path.exists(png), "no frame was dumped at the end of the continue"
    with open(png, "rb") as f:
        _w, _h, rgb = png_decode(f.read(), png)
    distinct = len(set(rgb[i:i + 3] for i in range(0, len(rgb), 3)))
    assert distinct > 64, (
        "the continue ended on a flat %d-colour screen, which is what the "
        "error box looks like; the minted save was refused" % distinct)
    return ("%d verb(s) round-trip, two mints byte-identical, and the save "
            "continues into map %d" % (15, save.position[0]))


# The stations the suite drives on every run: one early, one mid-game, one at
# the end, so the fast subset spans the story state the corpus exists to vary.
# About 28 s each, and they fit inside the longest test's wall clock in a
# parallel worker. PC_TEST_CORPUS=all drives every station instead.
#
# Chosen by measuring what each one executes rather than by geography: script
# coverage said the geographic three dispatched exactly zero field commands
# between them, so the ratchet they fed was guarding nothing.
#
# battle-wild is here for a different reason, and it is a scar. Every battle
# station asserts the save its fight produced, and the port stopped being able
# to write that save after those stations were pinned. Nothing failed, because
# this list held no battle station. One in the list costs 30 s and closes the
# class.
#
# egg-hatch is the same argument one class further on: it is the only station
# that runs a cutscene overlay to its end and then proves it happened from the
# save. It is also one of the longest, so it goes first.
#
# menu-box is the third: the only station that drives a full-screen application
# through its own menus rather than starting it from a lab hook, and the only
# one that proves what a menu did from the save.
CORPUS_FAST = ("menu-box", "egg-hatch", "canalave", "hm-cut", "sunyshore",
               "battle-wild", "poketch-digital")

CORPUS_PINNED_BY_TOOL = ("digest", "name", "map", "money", "badges", "party")


def test_corpus(tmp):
    """Every station in the manifest, and the fast subset actually driven.

    A station is a recipe (the save to mint, in the game's own names) and a
    spec (how to drive it, and what must be true afterwards). Growing the
    corpus is meant to be adding two text files, not writing a test, so what
    is checked here is the MANIFEST plus a subset that runs, and the
    manifest half is what keeps the cheap half honest as the corpus grows.

    The rule with teeth is that a spec may not carry a digest it did not earn.
    pc_corpus.py --pin writes a digest only after two identical runs and one
    under an empty environment, and it writes the semantic fields alongside it
    in the same pass; a digest typed in by hand arrives without them. So a
    spec with a digest and no semantic asserts fails the manifest, which is
    the mechanical form of "pinned the way the standard says".

    The subset is driven end to end: mint the save from the recipe, boot it,
    and check both the final frame against its pin AND the save through the
    reader. Both halves are needed and neither is redundant, the two walls
    the save lab hit left every field in the save correct and only showed on
    the screen, and a station that checked only the screen would be blind the
    other way.
    """
    sys.path.insert(0, os.path.join(ROOT, "pc", "tests"))
    import pc_corpus, pc_lab

    corpus = os.path.join(ROOT, "pc", "tests", "corpus")
    names = pc_corpus.stations()
    assert names, "no stations in %s" % os.path.relpath(corpus, ROOT)

    # No save is ever committed: a station regenerates its own, so a .sav in
    # the corpus directory is a blob that has escaped the recipe that made it.
    blobs = [f for f in os.listdir(corpus)
             if not f.endswith((".recipe", ".spec"))]
    assert not blobs, ("the corpus directory holds files that are not recipes "
                       "or specs: %s" % ", ".join(sorted(blobs)[:5]))

    problems = []
    for name in names:
        spec_path = os.path.join(corpus, name + ".spec")
        if not os.path.exists(spec_path):
            problems.append("%s has a recipe and no spec" % name)
            continue
        spec = pc_corpus.read_spec(name)

        script = os.path.join(ROOT, spec.get("input", ""))
        if not os.path.exists(script):
            problems.append("%s drives %s, which does not exist"
                            % (name, spec.get("input")))
        try:
            int(spec.get("frames", ""), 0)
        except ValueError:
            problems.append("%s has no usable frame count" % name)

        if "digest" not in spec:
            problems.append("%s carries no pinned digest (pc_corpus.py --pin)"
                            % name)
        else:
            missing = [k for k in CORPUS_PINNED_BY_TOOL if k not in spec]
            if missing:
                problems.append(
                    "%s has a digest but no %s, a digest written by hand "
                    "rather than by --pin, which is the one thing the pin "
                    "standard exists to stop" % (name, ", ".join(missing)))
        if spec.get("rtc2") and "digest2" not in spec:
            problems.append("%s names rtc2 but carries no digest2 "
                            "(pc_corpus.py --pin)" % name)

        # The recipe resolves: a name the headers do not have is a station
        # that would fail at mint time, and this catches it in a second.
        try:
            pc_lab.compile_recipe(os.path.join(corpus, name + ".recipe"),
                                  os.path.join(tmp, name + ".lab"))
        except SystemExit as e:
            problems.append("%s does not compile: %s" % (name, e))

    assert not problems, ("the corpus manifest is not sound (%d):\n  %s"
                          % (len(problems), "\n  ".join(problems[:6])))

    if not os.path.exists(BINARY):
        return "%d station(s) in the manifest, none driven (no binary)" % len(names)
    if not os.path.exists(ROM):
        return "%d station(s) in the manifest, none driven (no ROM)" % len(names)

    want = names if os.environ.get("PC_TEST_CORPUS") == "all" else \
        [n for n in CORPUS_FAST if n in names]

    # DRIVEN CONCURRENTLY, which is what stops this test from being the whole
    # suite's wall clock. Each station is an independent boot of a save it
    # mints itself, and everything a station touches is keyed by its own name:
    # The .sav, the .lab, the .cov and the frame directory, so the only
    # thing that made the loop serial was boot()'s shared stderr slot, and
    # pc_corpus returns that now. Four stations one after another put this at
    # 160-180 s and the suite at 250; concurrently they cost the slowest one.
    #
    # The pool is small on purpose. Every station saturates a core and the
    # suite runner is already running this test beside several others, so the
    # cap here is about not making every timeout in this file a lie.
    import concurrent.futures
    driven = {}
    with concurrent.futures.ThreadPoolExecutor(min(6, len(want) or 1)) as pool:
        def drive(name):
            return name, pc_corpus.run_station(name, tmp, quiet=True, cov=True)
        for name, (_spec, save, bad) in pool.map(drive, want):
            assert not bad, "station %s: %s" % (name, "; ".join(bad))
            driven[name] = save

    script = _cov_check([_cov_path_of(n) for n in driven], require=sorted(driven))

    maps = sorted({s.position[0] for s in driven.values()})
    return ("%d station(s) in the manifest, all pinned; %d driven end to end "
            "(maps %s), every digest and every save assert holding%s"
            % (len(names), len(driven), ", ".join(str(m) for m in maps), script))


TESTS = [
    ("determinism", test_determinism, True),
    ("mi_selftest", test_mi_selftest, True),
    ("png_decode", test_png_decode, True),
    ("viewer", test_viewer, False),
    ("widescreen", test_widescreen, True),
    ("hd3d", test_hd3d, True),
    ("view_channel", test_view_channel, True),
    ("session", test_window_is_the_session, True),
    ("save_durable", test_save_durable, True),
    ("save_lab", test_save_lab, True),
    ("battle_math", test_battle_math, True),
    ("clock_rollover", test_clock_rollover, True),
    ("text_decode", test_text_decode, False),
    ("corpus", test_corpus, True),
    ("fast_forward", test_fast_forward, True),
    ("mods", test_mods, False),
    ("dist", test_dist, False),
    ("patch_headers", test_patch_headers, False),
    ("sync_gate", test_sync_gate, False),
    ("ci", test_ci, False),
    ("arm7_excluded", test_arm7_excluded, False),
    ("abi_layout", test_abi_layout, False),
    ("win_frames", test_win_frames, True),
    ("diff", test_diff, True),
    ("polys", test_polys, True),
    ("pclaunch", test_pclaunch, True),
    ("ppwlobby", test_ppwlobby_stubbed, False),
    ("cli", test_cli, True),
    ("state_digest", test_state_digest, True),
    ("sym", test_sym, True),
    ("selftest", test_selftest, True),
]


def main():
    want = sys.argv[1:]
    have_binary = os.path.exists(BINARY)
    if not have_binary:
        print("no %s, running only the tests that do not need it "
              "(build with `make -f pc/Makefile`)" % os.path.relpath(BINARY, ROOT),
              file=sys.stderr)

    picked = [(n, f, b) for (n, f, b) in TESTS if not want or n in want]

    # WORKERS, from the start rather than after the suite got slow. Next door it
    # reached 480 s before anyone looked, and the arithmetic here is already
    # unfriendly: the replay alone is 46 s and three other tests boot the port for
    # hundreds of frames each. These are subprocess-bound, so threads are enough
    # and the accounting stays simple.
    #
    # The cap is deliberate and not cpu_count(). Each test spawns a port that
    # saturates a core, so oversubscribing turns a 46 s test into a slower one and
    # makes every timeout in this file a lie about what it was measuring.
    #
    # RAISED FROM 4 TO 8 on 2026-08-13, measured rather than guessed. The suite
    # had grown to 37 tests and 748 s of serial work; four workers ran it in
    # 248 s and eight in 192 s on a 24-core box. Individual tests do get slower
    # under the extra contention (that is the cost being bought) but the
    # wall clock is what a run waits on. Eight is not a ceiling anyone measured
    # a cliff at; it is where the return flattened, because past it the suite is
    # pinned by its longest single test rather than by how many run at once.
    workers = int(os.environ.get("PC_TEST_WORKERS", "0"))
    if workers <= 0:
        workers = max(1, min(8, (os.cpu_count() or 2) - 1))

    # What the previous run measured, for the longest-first ordering below. It
    # is a scheduling hint and nothing else: a missing, stale or corrupt file
    # costs one badly ordered run and never a wrong verdict, so every failure
    # reading it is swallowed on purpose.
    times_path = os.path.join(ROOT, "build", "pc", ".test-times.json")
    try:
        with open(times_path) as f:
            took_before = {k: float(v) for k, v in json.load(f).items()}
    except Exception:
        took_before = {}
    took = {}

    # Every test gets its own directory, which is what makes the parallelism safe
    # rather than merely faster. png_decode used to read the frames determinism
    # had just written into a shared tmp; an ordering dependency that was
    # invisible while the suite ran in order and would have become a race the
    # moment it did not. It makes its own run now; that costs one boot and buys a
    # suite whose result does not depend on the order it happened to run in.
    results = {}
    with tempfile.TemporaryDirectory(prefix="pplat-tests-") as tmp:
        def run_one(entry):
            name, fn, needs_binary = entry
            if needs_binary and not have_binary:
                return name, "SKIP", "needs the built port"
            own = os.path.join(tmp, name)
            os.makedirs(own, exist_ok=True)
            try:
                msg = fn(own)
                # A test that declines to run is not a test that passed. Tests
                # say so by returning a message opening "skipped:", and that
                # used to come back as PASS, so a gated-out test printed and
                # counted exactly like a passing one. That is how long_parity's
                # pin went stale behind PC_TEST_LONG and stayed stale through a
                # phase close that read the summary and believed it. The
                # verdict carries the distinction now, and the summary names
                # every test that did not run.
                verdict = "SKIP" if str(msg).startswith("skipped") else "PASS"
                return name, verdict, msg
            except Exception as e:
                return name, "FAIL", str(e)

        if workers == 1 or len(picked) == 1:
            for entry in picked:
                t0 = time.time()
                n, verdict, msg = run_one(entry)
                took[n] = time.time() - t0
                results[n] = (verdict, msg)
                print("%s %-12s %s" % (verdict, n, msg), flush=True)
        else:
            import concurrent.futures

            # LONGEST FIRST, which is worth as much here as the worker count is.
            # The pool takes work in submission order, and the registry below is
            # grouped by subject rather than by cost, so `replay`, the longest
            # test in the suite, sat second from the end and did not start until
            # most of the others had. The suite then finished 55 s after replay
            # could have, waiting on a test that had been queued behind trivia.
            # Sorting by last known duration cost nothing and recovered it.
            #
            # The durations come from the previous run rather than a table
            # anyone maintains: a hardcoded cost is a comment that goes stale
            # silently and reintroduces exactly this. An unmeasured test sorts
            # optimistically high so a NEW slow test is not the one left to
            # start last; the mistake only costs one run either way.
            order = sorted(picked, key=lambda e: -took_before.get(e[0], 1e6))
            with concurrent.futures.ThreadPoolExecutor(workers) as ex:
                def timed(entry):
                    t0 = time.time()
                    out = run_one(entry)
                    took[entry[0]] = time.time() - t0
                    return out

                futures = [ex.submit(timed, e) for e in order]
                for fut in concurrent.futures.as_completed(futures):
                    n, verdict, msg = fut.result()
                    results[n] = (verdict, msg)
            # Printed in table order, not completion order: a suite whose output
            # reshuffles between runs is one nobody can diff.
            for name, _fn, _b in picked:
                verdict, msg = results[name]
                print("%s %-12s %s" % (verdict, name, msg))

    # Merged rather than replaced: a run of one test must not throw away what
    # is known about the other thirty-six and leave the next full run unsorted.
    if took:
        merged = dict(took_before)
        merged.update(took)
        try:
            os.makedirs(os.path.dirname(times_path), exist_ok=True)
            with open(times_path, "w") as f:
                json.dump({k: round(v, 1) for k, v in merged.items()}, f,
                          indent=1, sort_keys=True)
        except Exception:
            pass

    failed = sum(1 for v, _ in results.values() if v == "FAIL")
    skipped = sorted(n for n, (v, _) in results.items() if v == "SKIP")
    passed = len(results) - failed - len(skipped)
    if skipped:
        print("%d test(s) did NOT run: %s" % (len(skipped), ", ".join(skipped)))
    print("%d passed, %d skipped, %d failed%s" % (
        passed, len(skipped), failed,
        " in %d worker(s)" % workers if workers > 1 and len(picked) > 1 else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
