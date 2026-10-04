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

### In the nativeplat cores

The wasm core reads packages through the runtime, never the host filesystem:
the host names one directory as `np_host.content_root` and the guest sees it,
read-only, as `/content` (the default `PC_MODS_DIR` there). Lookups cannot
leave it: `..`, absolute paths and links that resolve outside are refused.
Which packages load, and in what order, is `PC_MODS` among the core's options
(`np_core_create`), or `loadorder.txt` in the content directory when it is
unset; `PC_MODS=""` loads none. A package error stops the boot with the
`modfs:` message as `np_core_last_error`.

    <content_root>/loadorder.txt          optional
    <content_root>/<pkg>/mod.toml         id, name, version[, authors, requires, load_after]
    <content_root>/<pkg>/content/...      authored
    <content_root>/<pkg>/.cooked/digest   + cooked members, from the cook step

`example_menu_text/` is a small example: authored labels for the main menu
(`content/text/main_menu_options.json`, every message of that bank). Cook it
with the ROM build's tools in the romtools container, then run it:

```sh
R=$(git rev-parse --show-toplevel)
docker --context orbstack run --rm --platform linux/amd64 -v "$R:$R" \
  -w "$R/games/platinum" -u "$(id -u):$(id -g)" nativeplat-romtools:1 \
  python3 pc/modcook.py --mods-dir pc/mods --mods example_menu_text --rom-build build/rom
build/core-plat/np_headless platinum games/platinum/build/rom/pokeplatinum.us.nds \
  --content games/platinum/pc/mods -e PC_MODS=example_menu_text --frames 700 --dump /tmp/mod
```

Diamond and Pearl load the same packages through this same code; their
layout, cook step (`games/diamond/pc/modcook.py`, no container) and example
are in `games/diamond/pc/mods/README.md`.
