# The differential runner

melonDS, headless, writing the same trace the port writes, so the two can be
compared frame by frame over the same ROM.

```sh
make -f pc/Makefile melon-deps    # what it needs and where it looks
make -f pc/Makefile melon         # build the oracle
make -f pc/Makefile diff          # run both and report
```

What makes this worth building: a port is normally compared against an emulator
running a *dump*, and lives with every difference that follows from the two not
being the same program. This tree builds its own ROM, so both sides execute the
same image and a divergence belongs to the port.

What is comparable is hardware state, not the game's variables. Every
decompiled global is a host object the host linker placed, so there is nothing
at a guest address to compare it against. What does live in guest memory is
what the hardware sees: VRAM, the palettes, OAM, the I/O window, and the
allocations the game makes out of main RAM.

melonDS is not vendored here. Point `MELONDS` at a checkout; the makefile says
so when it cannot find one.
