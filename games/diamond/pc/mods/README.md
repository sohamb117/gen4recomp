# Runtime content packages (Diamond/Pearl)

The same packages as Platinum's (`games/platinum/pc/mods/README.md`, "Runtime
content packages"), loaded by the same code: Platinum's
`pc/src/pc_modfs.c` is compiled for Diamond and Pearl, and D's own FS and
NARC readers ask it first (`pc/game/pc_dp_modfs.c`,
`pc/patches/arm9/src/filesystem.c.patch`). There is no compile-time mod
door on D (`MODS=`); this is the runtime one.

The core serves the host's content directory (`np_host.content_root`,
`np_headless --content DIR`) read-only as `/content`, the default
`PC_MODS_DIR`. `PC_MODS` among the core's options picks the packages and
their order; when it is unset, `loadorder.txt` in the content directory does,
one name per line; `PC_MODS=""` loads none.

    <content_root>/loadorder.txt                      optional
    <content_root>/<pkg>/mod.toml                     id, name, version[, authors, requires, load_after]
    <content_root>/<pkg>/replace/<nitro path>         a whole ROM file, as is
    <content_root>/<pkg>/narc/<narc path>/<index>     one NARC member, as is
    <content_root>/<pkg>/content/...                  authored, cooked by pc/modcook.py
    <content_root>/<pkg>/.cooked/digest               + cooked members, from the cook

Later packages win a file or member both claim (the boot log says who
lost). A member index past the ROM's count appends to that NARC; a gap is
an error. A package error (unknown package, bad `mod.toml`, unmet
`requires`/`load_after`, a stale `.cooked/`, a claimed file that cannot be
read) stops the boot with the `modfs:` message as `np_core_last_error`.

What D does not have, because it is Platinum content with no D reader:
cooked billboard people, extra map props and cooked map headers
(`.cooked/generated/*.txt` is ignored on D).

## Cooking

`pc/modcook.py` turns `content/` into `.cooked/` and writes the digest the
port checks (a `.cooked/` that does not match `content/` + `records/` is
refused, not served). Its one recipe today is message banks:
`content/<narc path>/<index>.gmm` (pret's GMM XML, D's `charmap.txt`)
becomes member `<index>` of that NARC, encoded by D's own `tools/msgenc`,
which the script builds with the host C++ compiler on first use. No ROM, no
container. Run from `games/diamond`:

```sh
python3 pc/modcook.py --mods example_text
```

## Example

`example_text/` replaces Professor Rowan's introduction
(`msgdata/msg.narc` member 341, every message of the bank, written for this
package) and the main menu's first three entries (member 494: CONTINUE,
NEW GAME and MYSTERY GIFT get new labels; the rest of the bank is the
ROM's). Both banks are the same index on Diamond and Pearl.

```sh
cd games/diamond && python3 pc/modcook.py --mods example_text && cd ../..
build/<core>/np_headless diamond games/diamond/build/diamond.us/pokediamond.us.nds \
  --content games/diamond/pc/mods -e PC_MODS=example_text \
  -e PC_MODFS_PROBE_NARC=msgdata/msg.narc/341
```

`PC_MODFS_PROBE=<nitro path>` and `PC_MODFS_PROBE_NARC=<narc path>/<index>`
print the file or member the game would read (the member through both the
by-id and the NARC-object readers, which must agree) and exit, on the first
file the game opens.
