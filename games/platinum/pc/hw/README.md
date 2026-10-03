# pc/hw: the DS hardware models

Everything in this directory is derived from
[melonDS](https://github.com/melonDS-emu/melonDS), Copyright 2016-2026 melonDS
team, GPLv3-or-later. That is what makes this whole repository
GPL-3.0-or-later.

They are kept together, in one directory, so that "which code came from
melonDS" is a question with a directory for an answer rather than a search.

| File | Derived from |
| --- | --- |
| `pc_gpu2d.c` | `src/GPU2D_Soft.cpp`, `src/GPU2D.cpp` |
| `pc_gpu3d.c` | `src/GPU3D.cpp`, `src/GPU3D.h` |
| `pc_gpu3d_soft.c` | `src/GPU3D_Soft.cpp`, `src/GPU3D_Soft.h` |
| `pc_spu.c` | `src/SPU.cpp`, `src/SPU.h` |

## What "derived" means here

The arithmetic is theirs and the plumbing is ours.

Theirs: every shift, truncation, division and comparison. A formulation that is
algebraically equal but rounds differently is a wrong pixel rather than a
tidier one, so those expressions are transcribed rather than rearranged. Each
file's own header says which parts of upstream it took and which it left.

Ours: where the registers come from, how guest memory is reached, and the
storage. Upstream's C++ wants a `melonDS::NDS` to hang off, which would mean a
second DS inside the process with a second VRAM model to keep in step with the
port's own. So the algorithms are lifted into C against this port's memory
instead of the C++ being linked against melonDS's state.

## How they are checked

`pcdiff-melon` drives the real melonDS over a sweep of register writes and
reports what it produced, and the tests replay every configuration here and
compare. That is the only way these rules can be checked at all: a register's
meaning is console behaviour and leaves no trace in a ROM image.

```sh
make -f pc/Makefile melon-deps    # what it needs
make -f pc/Makefile melon         # build the oracle
make -f pc/Makefile diff          # run the port against it
```

Where an oracle stops short of the hardware, the file says so rather than
implying coverage it does not have.

## Rebasing

When pulling in newer melonDS, read each file's header first: it lists what was
deliberately left out and why, which is what a diff against upstream cannot
tell you.
