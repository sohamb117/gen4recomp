# nativeplat

Native ports of Pokémon Diamond, Pearl, Platinum, HeartGold, SoulSilver,
Ruby, Sapphire, Emerald, Black and White (US). Diamond, Pearl, Platinum,
HeartGold, SoulSilver and the GBA games are built from the pret
decompilations, compiled to WebAssembly, turned back into C with wasm2c and
run natively by `core/runtime`; Black and White use a static recompilation of
the cartridge image with `ndsrec` and `armrec`. There is no emulator. The app
(`shell/`) adds the Gen1Recomp-style player features: launcher, save slots,
save editor, display effects, snapshots and rewind, local wireless over LAN or
a relay, mods, folder sync and more.

- How to build everything: [docs/BUILDING.md](docs/BUILDING.md)
- What works per game: [docs/FEATURE_PARITY.md](docs/FEATURE_PARITY.md)
- The macOS app, feature by feature, with screenshots: [docs/evidence/README.md](docs/evidence/README.md)
- App details (every option, file formats, autotest): [shell/README.md](shell/README.md)

## Playing on macOS

Status: all ten games boot in the packaged app. Diamond, Pearl, Platinum,
Ruby, Sapphire and Emerald have the broad app-feature coverage documented in
[docs/FEATURE_PARITY.md](docs/FEATURE_PARITY.md); Black, White, HeartGold and
SoulSilver have the core and app coverage documented there.

### Install

1. Unzip `nativeplat-macos-arm64.zip` (Apple Silicon; `tools/package_macos.sh`
   makes it, `--universal` adds Intel). It holds `nativeplat.app`,
   `README.txt`, `LICENSE.txt` and `THIRD-PARTY.txt`.
2. Move `nativeplat.app` wherever you like (Applications, or a folder of its
   own for portable mode, below). It needs macOS 11 or later.
3. The app is ad-hoc signed, not notarized: the first time, right-click it
   and choose **Open** (or allow it under System Settings > Privacy &
   Security).

The ten-core packages are `nativeplat-macos-arm64-c8948448c.zip` (SHA-256
`58fa10e2ea39e12bfd3fef146e05ad3e5fdc4c8b43c8295090510f1258d94c12`) and
`nativeplat-windows-x64-0bb574077.zip` (SHA-256
`14b640be85b0813b85ed7e806b62e6055ab3b09308f0947d51575ef92c28336a`).

Your data (imported cartridges, saves, screenshots, `options.ini`) lives in
`~/Library/Application Support/nativeplat/`. **Portable mode:** put an empty
file named `portable.txt` next to `nativeplat.app` and everything goes to
`userdata/` beside it instead (the launcher's last line shows which folder is
in use).

### Import your cartridges

nativeplat ships no game data. Drop your own `.nds` or `.gba` dump on the
window, or click **Import ROM**. The Import ROM dialog accepts both `.nds` and
`.gba`; the file is checked by SHA-1 and copied into your data folder, and the
launcher card then says **Ready**.

| Game | Accepted dump (SHA-1) |
| --- | --- |
| Diamond (USA) | `a46233d8b79a69ea87aa295a0efad5237d02841e` |
| Pearl (USA) | `99083bf15ec7c6b81b4ba241ee10abd9e80999ac` |
| Platinum (USA, Rev 1) | `0862ec35b24de5c7e2dcb88c9eea0873110d755c` |
| HeartGold (USA) | `4fcded0e2713dc03929845de631d0932ea2b5a37` |
| SoulSilver (USA) | `f8dc38ea20c17541a43b58c5e6d18c1732c7e582` |
| Ruby (USA) | `f28b6ffc97847e94a6c21a63cacf633ee5c8df1e` |
| Sapphire (USA) | `3ccbbd45f8553c36463f13b938e833f652b793e4` |
| Emerald (USA, Europe) | `f3ae088181bf583e55daf962a92bb46f4f1d07b7` |
| Black (USA, Europe) | `26ad0b9967aa279c4a266ee69f52b9b2332399a5` |
| White (USA, Europe) | `bc696a0dfb448c7b3a8a206f0f8214411a039208` |

Click a game to see its **save slots**: *Continue* (the last one played),
*New save slot...*, every slot (Play, Edit save..., Rename, Duplicate,
Export .sav, Cart, Delete) and *Import .sav...* (or drop a `.sav`/`.dsv` from
melonDS or DeSmuME on that page).

### Controls

| DS | Keyboard | Gamepad (by position) |
| --- | --- | --- |
| D-pad | Arrows, W A S D | D-pad, left stick |
| A / B | Z, Enter, Space / X, Backspace | East / South |
| X / Y | C / V | North / West |
| L / R | Q / E | LB / RB |
| Start / Select | Escape / Tab, Shift | Start / Back |
| Touch screen | the mouse on the bottom screen | |

| Key | Does |
| --- | --- |
| F10 or Cmd+, | Options (every binding is changeable in *Options > Controls...*) |
| F | Fast-forward while held; G toggles it; `1` cycles 1x/2x/3x/4x/8x/uncapped |
| F1 / F2 F2 | Quick save (the game's own save) / reload the last save |
| F5 / F6 / F7 | Take a snapshot / next of 4 snapshot slots / restore it |
| R (hold) | Rewind |
| `-` `=` / `3` `4` / `0` | Field camera farther, closer / tilt / original |
| F9 | Touch controls on/off |
| F11 | Fullscreen |
| F12 | Screenshot (PNG in `screenshots/`) |

### Features

Everything below is in *Options* (F10) unless noted; each was run in the
packaged app on Diamond, Pearl and Platinum with a screenshot, listed in
[docs/evidence/README.md](docs/evidence/README.md).

- **Display:** screen layout (vertical, side by side, hybrid, one screen),
  swap, rotation, integer scaling, filters, a separate layout for battles;
  two chained effects (LCD grid, scanlines, CRT with curvature, smooth) and
  performance presets; 3D render scale up to 4x and widescreen 3D; UI scale
  and reduced motion.
- **Game options (live):** music and sound-effect volume, the music
  low-pass filter, instant text, *Fix cartridge bugs*, real-time clock.
- **Save editor:** *Edit save...* on a slot: trainer, party (and Add
  Pokémon), boxes, bag, Pokédex and events (Mystery Gift Wonder Cards for
  the Darkrai, Shaymin, Arceus and Rotom events; event flags and vars), with
  undo/redo and a `.bak` of the previous file; Trainer Card and Pokédex
  diploma export as PNG. HeartGold/SoulSilver saves (Johto and Kanto
  badges) and Black/White saves edit the same way (`.pgf` Wonder Cards into
  Black/White's twelve slots). Also standalone:
  `nativeplat --editor --save file.sav`.
- **Touch and skins:** on-screen controls with a layout editor; Delta
  `.deltaskin` controller skins (drop one on the window).
- **Local wireless:** turn on *Local wireless (LAN)* on two Macs (or two
  copies on one Mac with different LAN ports and *Join by IP:port*) and meet
  in the Union Room (trades, battles; Diamond, Pearl and Platinum meet each
  other) or the Underground; *Internet relay host:port* and
  *Room PIN* do the same over a relay server ([docs/RELAY.md](docs/RELAY.md)).
- **Pal Park:** *GBA cartridge* takes a GBA ROM and save you own (e.g.
  Emerald); with a National Dex the main menu offers MIGRATE FROM ....
- **Mods and custom carts:** *Mods...* installs content packages (`.zip`)
  for each game (L/R switch games), orders and enables them, and seals the
  enabled set as a cart a slot can be bound to.
- **Folder sync:** *Sync folder* mirrors every slot to a folder (iCloud
  Drive, Dropbox, a share) and asks when both sides changed.
- **Updates:** *Updates...* checks the project's latest release only when you
  press *Check*, downloads the macOS zip and verifies its SHA-256; it never
  replaces itself.
- **Launching:** `nativeplat --game platinum --slot "My run"`,
  `--launcher`, or `open 'nativeplat://launch?game=platinum&slot=My%20run'`.
