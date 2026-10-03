# The Nintendo 3DS port

The same sources as the PC port, on a console that cannot identity-map guest
memory.

```sh
make -f 3ds/Makefile              # build/3ds/pokeplatinum.3dsx
make -f 3ds/Makefile status       # what the toolchain and the tree look like
make -f 3ds/Makefile card SD=/mnt/sd   # stage a card
```

Needs devkitARM with libctru and citro3d.

## What is different from the PC port

* **Guest memory is a slab.** 3DS userspace will not map anything at
  0x02000000, so every DS region becomes a slice of one allocation and a guest
  address becomes a name that one translation step turns into slab plus offset.
  `include/3ds_guest_map.h` is the table both halves read.
* **The picture goes through the PICA.** The DS backgrounds, the sprites and
  the 3D layer are drawn on the GPU rather than composed on the CPU, and each
  step was checked against the software renderer before it became the default.
* **The models are pure C.** Almost everything under `src/` compiles on a build
  machine with no console and no emulator, which is what lets `tests/` check it
  against the PC port's own renderer.

## Layout

| Path | What it is |
| --- | --- |
| `src/` | the console layer: memory, GPU, audio, input, faults |
| `include/` | the guest map, and shadows of the SDK's headers |
| `tests/` | host-runnable checks, and the emulator harness |
| `patches/` | diffs against SDK sources this console cannot compile |
| `hw/` | shared hardware model sources |

## Runtime files

The console reads and writes under `sdmc:/3ds/pokeplatinum/`:

    pokeplatinum.us.nds     the cartridge, if it is not packed into the 3dsx
    <rom>.sav               the save
    input.txt               an input script, if you want a repeatable run
    3d.txt, bg.txt, p3d.txt renderer switches, for bisecting a wrong picture

Nothing here ships a cartridge image. Your own dump goes in that directory, or
into the 3dsx at build time.
