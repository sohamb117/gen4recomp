# Mods

Two mechanisms, and they are deliberately separate doors.

## Compile-time mods

`MODS="name ..."` selects directories under `pc/mods/`. A mod may carry:

    <mod>/patches/<path-of-source>.patch   a diff against a game source
    <mod>/src/*.c                          new code, compiled as game code
    <mod>/plugins/patches/<path>.patch     the same, for a plugin overlay
    <mod>/plugins/src/**/*.c

Patches are authored against what the compile consumes, which is the source
with `pc/patches/` already applied, and mods stack in the order `MODS` names
them. A mod patch that stops applying fails loudly and names its mod.

```sh
make -f pc/Makefile MODS="mymod" -j$(nproc)
./pc/play.sh MODS="mymod"
```

A modded build gets its own build directory and its own save, because the plain
binary is what every test and every pinned number measures.

`MODS_DIR` points somewhere else, so a project can keep its mod in its own
repository and never write to this tree.

An unknown name is an error, not a mod that silently did not load.

## Runtime content packages

`PC_MODS="pkg ..."` loads cooked content with no rebuild. A package is authored
in the decompilation's own formats:

    <pkg>/content/     graphics, text, maps, models, events
    <pkg>/records/     frozen ids for anything new
    <pkg>/.cooked/     what the cook step produced; not authored by hand

```sh
make -f pc/Makefile cook PC_MODS="mypkg"
./pc/play.sh PC_MODS="mypkg"
```

`pc/modcook.py`'s header documents every recipe: which file under `content/`
produces which cooked member, and what each one requires. `pc/modport.py`
extracts source material from another cartridge you own.

With `PC_MODS` unset, `loadorder.txt` in this directory is read instead, one
package name per line.

The port refuses a stale `.cooked/` rather than serving it, and a compile-time
plugin named in `PC_MODS` is an error that tells you to use `MODS` instead.
