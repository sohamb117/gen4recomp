#!/usr/bin/env python3
"""Build the one zip a player downloads.

No installer, nothing on PATH, nothing written outside the folder it unzips
into. The archive holds the three programs, the licence and a README whose
first line is the whole installation procedure.

Executable bits are set by hand rather than left to zipfile's defaults,
because zipfile writes external_attr as 0 and a Linux player would unzip
three files they cannot run. Windows ignores the field.

What this produces is a local build artifact. The port binary is this
repository's sources compiled, and those sources are a decompilation of a
commercial game, so the zip is something you build for yourself and never
something that gets uploaded. This repository ships source and tools.
"""

import argparse
import os
import stat
import zipfile

README = """\
unzip, double-click.

That is the whole installation. Everything the game needs is in this folder.
Nothing is copied elsewhere on your computer, nothing is added to your PATH,
and there is no installer to run. Delete the folder and it is gone.

  Windows   double-click pclaunch.exe
  Linux     run ./pclaunch

The first time it starts it asks for a Pokemon Platinum cartridge dump, a
.nds file. It does not come with one and cannot make one for you; dump the
cartridge you own. After that it remembers where the file is and starts
straight into the game.

Your save sits beside the game as a .sav file: the ordinary 512 KB DS save
image, the same shape a cartridge dump has. The game writes it the moment you
save in-game, not when you quit, so closing the window is always safe, the
file on disk is already the one you saved. Nothing is buffered and nothing is
written on the way out.

The save you made before the current one is kept beside it as .sav.bak, so
the previous save always survives. If the live file is ever lost, rename the
backup over it.

Controls
  arrow keys        the D-pad
  X / Z             A / B
  S / A             X / Y
  Q / W             L / R
  Return            Start
  Backspace         Select
  Tab (held)        fast-forward, for grinding and hatching
  mouse             the touch screen, click and drag on the lower screen
  F11, Alt+Enter    fullscreen
  F12               screenshot
  Esc               quit

A gamepad works if one is plugged in; it is mapped by position, so the
button where a DS has A is A.

To change a key, add a line like

  bind a=z,start=space

to the launcher's settings file. It says where it is in its own first line,
and it lists what the names are.

This program is free software under the GNU General Public License version
3; see LICENSE.txt. It is not affiliated with or endorsed by Nintendo,
Game Freak or The Pokemon Company.
"""


def add(zf, src, dest, executable):
    with open(src, "rb") as f:
        data = f.read()
    info = zipfile.ZipInfo(dest, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3      # Unix, so external_attr is read as a mode
    mode = 0o755 if executable else 0o644
    info.external_attr = (stat.S_IFREG | mode) << 16
    zf.writestr(info, data)
    return len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--root", required=True,
                    help="the single directory the zip unpacks into")
    ap.add_argument("--exec", dest="execs", action="append", default=[],
                    metavar="SRC[:DEST]")
    ap.add_argument("--file", dest="files", action="append", default=[],
                    metavar="SRC[:DEST]")
    args = ap.parse_args()

    def split(spec):
        src, _, dest = spec.partition(":")
        return src, dest or os.path.basename(src)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    total = 0
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED,
                         compresslevel=6) as zf:
        for spec in args.execs:
            src, dest = split(spec)
            total += add(zf, src, args.root + "/" + dest, True)
        for spec in args.files:
            src, dest = split(spec)
            total += add(zf, src, args.root + "/" + dest, False)
        info = zipfile.ZipInfo(args.root + "/README.txt",
                               date_time=(1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.create_system = 3
        info.external_attr = (stat.S_IFREG | 0o644) << 16
        zf.writestr(info, README)
        total += len(README)

    size = os.path.getsize(args.out)
    print("  DIST    %s (%.1f MB from %.1f MB)"
          % (args.out, size / 1e6, total / 1e6))


if __name__ == "__main__":
    main()
