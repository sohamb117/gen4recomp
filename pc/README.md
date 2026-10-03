# The PC port

This directory builds the decompilation as a native program.

```sh
make -f pc/Makefile -j$(nproc)     # build build/pc/pokeplatinum
./pc/play.sh                       # play it
```

See [`../INSTALL.md`](../INSTALL.md) for the packages and the other targets.

## How it works

The game's C compiles for the host almost unchanged. What it expects
underneath is a DS, so the port supplies one:

* **Guest memory is identity mapped.** Main RAM lives at 0x02000000 in this
  process, so a guest pointer and a host pointer are the same value and code on
  both sides passes pointers to the other for free. That is why the build is
  32-bit, and it is why the save format matches the cartridge's.
* **The hardware is modelled, not emulated.** `pc/hw/` holds the 2D and 3D
  engines and the sound mixer; `pc/src/` holds the rest of the machine, the
  DMA, the timers, the RTC, the touch panel, the card and the two processors'
  message queues.
* **Anything the models cannot answer aborts by name.** A wrong picture that
  looks right is the expensive kind of defect, so where a model would have to
  guess it stops instead and says which register it does not understand.

## Layout

| Path | What it is |
| --- | --- |
| `src/` | the host layer: memory, devices, video, audio, input, saves |
| `hw/` | DS hardware models derived from melonDS |
| `include/` | the port's own headers, and shadows of the SDK's |
| `sdl/` | the viewer and the launcher, which are separate programs |
| `patches/` | diffs against game sources, applied into the build tree |
| `tests/` | the test suite and its fixtures |
| `replays/` | recorded input scripts, frame-addressed |
| `diff/` | the differential runner against melonDS |
| `mods/` | mods and content packages; see `mods/README.md` |
| `arm7snd/` | the ARM7 sound driver, compiled as host code |

## Inputs

Every input is an environment variable, and every variable has a matching
flag that is translated into it before the port reads anything. So a run driven
by flags and the same run driven by variables are the same run.

```sh
build/pc/pokeplatinum --help
```

The ones worth knowing: `PC_ROM` is the cartridge, `PC_SAVE` is the save file,
`PC_INPUT` replays a script, `PC_FRAMES` ends the run after N frames, and
`PC_DUMP_FRAMES` writes a PNG per frame.

## Determinism

The same inputs produce the same run, byte for byte, and the test suite checks
it: two runs are compared frame by frame and their whole guest memory is
digested and compared. That is what makes everything else measurable, so
anything that would leak the host into a frame, a clock, an address from ASLR,
the size of your environment, is a defect rather than a detail.
