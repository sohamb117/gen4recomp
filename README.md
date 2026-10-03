# Pokémon Platinum, built to run natively

This is a fork of [pret/pokeplatinum](https://github.com/pret/pokeplatinum), the
decompilation of Pokémon Platinum, with a port that compiles the same C into a
native program instead of into a DS ROM.

The decompilation still builds the original cartridge image, byte for byte. The
port is built from those same sources, so work that lands upstream reaches the
port by merging rather than by being rewritten.

Where it runs today:

* **Linux**, **Windows**, **32-bit ARM** and **Android**, all built out of
  `pc/`, one makefile per host off the same pipeline
* **Nintendo 3DS**, in `3ds/`

You supply your own cartridge dump. Nothing in this repository contains game
data, and neither the ROM build nor the port will produce one for you.

## What is here

| Path | What it is |
| --- | --- |
| `src/` `include/` `res/` `lib/` | the decompilation, from upstream |
| `pc/` | the PC port: host layer, build, tests, tools, one makefile per host |
| `pc/hw/` | DS hardware models: 2D, 3D and sound |
| `3ds/` | the Nintendo 3DS port |
| `tools/` | the decomp's asset tools, plus `tools/armrec` |
| `docs/` | reference for the data files and formats |

## Quick start

Building the ROM first is required, because the port reads the cartridge
filesystem out of it, and `BUILD=build/rom` is where the port looks.

```sh
git clone https://github.com/wize-logic/pokeplatinum-port
cd pokeplatinum-port

make BUILD=build/rom              # build the ROM and check it against the real one
make -f pc/Makefile -j$(nproc)    # build the PC port
./pc/play.sh                      # play it in a window
```

[`INSTALL.md`](INSTALL.md) lists the packages you need and covers the 3DS
build, the Windows, ARM and Android cross builds, and the tests.

## Licence

GPL-3.0-or-later. See [`LICENSE`](LICENSE).

The port includes hardware models derived from
[melonDS](https://github.com/melonDS-emu/melonDS), which is GPLv3-or-later;
that is what fixes the licence for the whole work. They are kept together in
`pc/hw/` with their provenance recorded, and
[`pc/hw/README.md`](pc/hw/README.md) says what "derived" means for each file.
