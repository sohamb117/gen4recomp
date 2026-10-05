# HeartGold/SoulSilver matching ROM build: handoff

Branch `hgss` (worktree `/Users/soham/Documents/code/nativeplat-hgss`). Paused
when HGSS was re-prioritized after E2E D/P/Pt and Black/White. Nothing here is
integrated into `main`.

## State

- `games/heartgold` is pret/pokeheartgold 9d8b759, vendored at b0eb928f8.
- `tools/diamond/` is now the shared `tools/ntr/` (git mv, so history follows):
  - `setup.sh diamond|heartgold`: per-game stand-ins for pret's mwccarm.zip
    and NitroSDK `tools/bin`:
    - mwccarm/mwldarm/mwasmarm wrappers for every CodeWarrior release in
      metroskrew's `share/metroskrew/sdk/ds/<ver>/*.exe.txt`; HGSS needs
      2.0/sp2p2, 2.0/sp2p3 and 1.2/sp2p3;
    - makelcf built from ntrtwl's NitroSDK 4.2 sources, with IRQ stack
      default 0x400 for Diamond (SDK 3.2) and 0x800 for HGSS (SDK 4.2,
      071210 per pokeheartgold's INSTALL.md);
    - makerom.py, makebanner.py (new; version 1 banner from the UTF-16 .bsf
      and nitrogfx's .nbfc/.nbfp), ntrcomp.sh (nitrogfx path now from
      `NITROGFX`, set by the wrapper);
    - specfiles: `ARM9-TS`/`ARM7-TS.lcf.template` (unchanged from Diamond)
      and a new `mwldarm.response.template`, which lists each section's
      plain objects through `=.text` filters so section-qualified autoload
      duplicates (`Object x.o (.itcm)`) appear once;
    - HGSS only: a no-op `tools/mwasmarm_patcher/mwasmarm_patcher`, because
      the real patcher rejects the metroskrew wrapper and every sub-make's
      `all` runs `patch_mwasmarm`. `make -o` does not reach sub-makes.
  - `census.py diamond|heartgold`: HGSS is counted per link unit from
    main.lsf (arm9 static plus ITCM/DTCM autoloads, each overlay) and
    sub/ichneumon_sub.lsf (arm7). Diamond's totals are unchanged.
- `tools/rom_build.sh heartgold|soulsilver` runs
  `setup.sh heartgold && make NOWINE=1 PROJECT_ROOT_NT=<root>/games/heartgold/ -j$NP_JOBS <game>`.
  `PROJECT_ROOT_NT` stands in for common.mk's `wslpath -w`, which doesn't
  exist in the container. `-j` is now `NP_JOBS` (default 6) for every game.

**Not yet run:** the HGSS build was still queued on `tools/heavy.sh` when it
was paused, so nothing has been built, linked or compared. Diamond/Pearl have
not been re-verified on the new `tools/ntr` paths either.

## Census (source-level, from `tools/ntr/census.py heartgold`)

| unit | asm funcs (.s) | C funcs | asm-in-C | C share |
|---|---:|---:|---:|---:|
| arm9 static | 5289 | 5762 | 44 | 51.9% |
| overlays (129) | 19085 | 4324 | 4 | 18.5% |
| arm7 | 889 | 160 | 23 | 14.9% |
| total | 25263 | 10246 | 71 | 28.8% |

The per-overlay rows come from running the script. One object has no source:
`lib/dsprot/coretests_decrypter_decoder.o`, a generated stub.

## Next steps

1. Prove Diamond still matches on the moved tools:
   `tools/heavy.sh tools/rom_build.sh diamond`, then `pearl`.
   Expected SHA-1s are in `games/diamond/*.sha1`.
2. `tools/heavy.sh tools/rom_build.sh heartgold`. Logs go to stdout. With
   COMPARE=1 (the default), make checks, in order:
   - `heartgold.us/filesystem.sha1`;
   - `main.sha1` (arm9 static and every `OVY_*.sbin`);
   - `sub/ichneumon_sub.sha1`;
   - `heartgold.us/rom.sha1`.

   The first failing check names the stage.
3. Where to look if a check fails:
   - Link fails or `.sbin` mismatches: the response template (object
     order or duplicates) and the lcf templates. pokeheartgold's common.mk
     sed-inserts `KEEP_SECTION .exceptix` (arm9) and an
     `SDK_SUBPRIV_ARENA_LO` line after `} > check.WORKRAM` (arm7). Compare
     against the SDK 4.2 template variant in
     `games/platinum/subprojects/NitroSDK-4.2.30001/specfiles` (meson
     renames aside). Use `tools/asmdiff` in games/heartgold for
     per-function diffs.
   - Asm objects differ: metroskrew mwasmarm 1.0-23 may need pret's
     line-ending/incbin patch after all. In that case, patch a copy of the
     assembler instead of no-oping the patcher.
   - Only the ROM differs: check makerom.py against the SDK 4.2 layout. That
     covers the 1TROM timings, RomSize 1G with RomFootPadding, banner and
     FNT placement, and the header bytes. pokeheartgold commits its own
     `heartgold.us/rom_header_template.sbin`. lhearachel/nitrorom
     (games/platinum/tools/nitrorom) reproduces the 4.2 packer.
   - Banner: `makebanner.py` is untested against a real banner. Its CRC
     covers 0x20..0x840.
4. `tools/heavy.sh tools/rom_build.sh soulsilver`. Its targets are
   pokeheartgold.us.nds sha1 4fcded0e2713dc03929845de631d0932ea2b5a37 and
   pokesoulsilver.us.nds sha1 f8dc38ea20c17541a43b58c5e6d18c1732c7e582.
5. Report the link maps:
   - arm9: `games/heartgold/build/<game>.us/main.elf` and
     `main.elf.xMAP` (overlays included);
   - arm7: `sub/build/ichneumon_sub.elf` and its `.xMAP`;
   - overlay table: `build/<game>.us/main_table.sbin`.
6. Integrate (ff-only into the user's checkout per the repo protocol), then
   clean `games/heartgold/build` if disk is tight. `df -h ~` showed 8.6 GiB
   free.
