# Gen1Recomp feature parity

Every player-facing Gen1Recomp (G1R Deluxe) feature, its Diamond/Pearl/Platinum
equivalent, and its status per game. Source inventory: Gen1Recomp `dev`
(340e256) docs and code. Gen1Recomp's launcher is proprietary (its LICENSE.MD
term 2), so nativeplat ships its own launcher; no Gen1Recomp code is copied.

Status: **done** shown working in the packaged macOS app (evidence linked) ·
**core** works in the core / headless tests, not yet shown in the app ·
**shell** a game-independent app feature, shown with Platinum, the same code for every game · **wip** in
progress · **no** not started · **n/a** no counterpart (reason given).

Evidence names are cases in [docs/evidence/README.md](evidence/README.md)
(screenshots in `build/evidence/`, made by `tests/mac/feature_matrix.py` on
`build/dist/nativeplat-macos-arm64.zip`, 2026-10-04). Platinum rows: main
904498583; Diamond/Pearl rows and Windows: main 33e316444.

**All three games** were run in the packaged app (main 33e316444 plus this
shell): the in-game rows were run on Diamond, Pearl and Platinum from saves
minted with `tests/gameplay` recipes (`--game diamond|pearl` runs of
`tests/mac/feature_matrix.py`; Diamond and Pearl evidence names carry a
`diamond-` / `pearl-` prefix). Shell rows that do not depend on the game
(launcher, slots, options UI, sync, updater, skins, relay) were exercised
with Platinum data and run the same code for every game.

## Launcher and import

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Multi-game launcher, per-game cards | Diamond / Pearl / Platinum cards with readiness | done | done | done | launcher_import, games_boot |
| ROM import with SHA-1 verification | US dumps; Platinum rev 0 recognised and refused | done | done | done | launcher_import |
| File picker and drag-and-drop import | SDL3 dialog + drop | done | done | done | launcher_import (2 drops, 1 dialog) |
| Generated cache from ROM | Not needed: the core reads the cartridge filesystem at runtime | n/a | n/a | n/a |  |
| Save slots, Continue / Edit / Delete | Named slots: new, duplicate, rename, export/import .sav, delete | done | done | done | slots, continue; diamond-continue, pearl-continue |
| Custom carts | Sealed mod package sets bound to a slot | done | done | done | carts; diamond-carts, pearl-carts |
| Import inbox (mobile Documents) | Same on iOS | no (iOS deferred) | no | no |  |

## Display

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Screen layout / position | Vertical, side by side, hybrid, single screen, swap, rotation, integer/linear | done | done | done | layouts; diamond-layouts, pearl-layouts |
| Widescreen battle layout | Separate layout while `NP_STAT_IN_BATTLE` | done | done (wild Bidoof) | done (wild Bidoof) | battle_layout; diamond-battle_layout, pearl-battle_layout |
| Survey zoom | Field camera zoom (`-` / `=`) | done | done | done | camera; diamond-camera, pearl-camera |
| Perspective tilt | Field camera tilt (`3` / `4`), `0` resets | done | done | done | camera; diamond-camera, pearl-camera |
| GBC screen effects / Shader FX | Two-slot chain: LCD grid, scanlines, CRT (+curvature), smooth | done | done | done | effects; diamond-effects, pearl-effects |
| Performance presets | Custom / High / Balanced / Low / Auto | done | done | done | effects; diamond-effects, pearl-effects |
| V-Sync, display frame cap | Same | done (Options) | done (Options) | done (Options) | effects-preset-options; diamond-effects-preset-options, pearl-effects-preset-options |
| Logic clock 60 / cart rate | 60 Hz or 59.8261 Hz | done (Options) | done (Options) | done (Options) |  |
| Upscaled 3D | 3D render scale 1x-4x, widescreen 3D | done | done | done | render_scale; diamond-render_scale, pearl-render_scale |
| Screenshots | F12 PNG | done | shell | shell |  |
| Pokédex diploma / printer export | Trainer Card and diploma PNG from the save editor | done | done (editor on a D/P save) | done (editor on a D/P save) | trainer_card; diamond-editor, pearl-editor |
| Colour modes | GB palettes | n/a | n/a | n/a | DS is full colour |

## Audio

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| ROM-derived music/SFX | Native SDAT playback via the ARM7 sound driver model | done | done | done | audio, games_boot; diamond-audio, pearl-audio |
| Music / SFX volume | BGM / SE volume per sequence player | done (Sandgem field: BGM 0 halves peak 29152 to 14880) | done (BGM 0: peak 25968 to 9644) | done (BGM 0: peak 25968 to 9644) | audio; diamond-audio, pearl-audio |
| Music low-pass filter | Off / 1X / 2X / 3X | done (treble 834 to 476 at 3X) | done (treble 661 to 400) | done (treble 661 to 400) | audio; diamond-audio, pearl-audio |
| Fast-forward audio | Normal-pitch snippets above 1x | done | shell | shell |  |

## Input

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Keyboard / gamepad defaults, rebinding | Every action, 3 keys + 1 pad button | done | done | done | controls |
| Touch: mouse and fingers | Stylus on the bottom screen in every layout | done | done (touch bag in tests/gameplay) | done (touch bag in tests/gameplay) |  |
| On-screen touch pad, layout editor | Same, per orientation | done | shell | shell | touch_editor |
| Touch skins | Delta `.deltaskin` (hand-made test skin) | done | shell | shell | skin |
| Speed hotkeys up to very high | `1` cycles 1x-8x/uncapped; F hold, G toggle | done (1800 iterations ran 6642 frames with G) | done (6648 frames) | done | speed; diamond-speed, pearl-speed |
| F1 quick save / F2 quick load | The game's own save without its menu; reboot from it | done | done | done | quicksave; diamond-quicksave, pearl-quicksave |

## Saves

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Normal save, atomic writes, backups | Backup chip image, temp + rename, `.bak` | done | done | done | slots, editor; diamond-editor, pearl-editor |
| Semantic checkpoints | 4 in-memory snapshot slots (F5/F6/F7) and hold-to-rewind | done | done (snapshot 1 loaded; rewind depth 286 to 271) | done (snapshot 1 loaded; rewind depth 286 to 271) | snapshots; diamond-snapshots, pearl-snapshots |
| Cloud / device sync | Folder sync, three-way with conflict chooser | done | shell | shell | sync |
| Portable mode (`portable.txt`) | Same | done | done | done | portable (every case runs portable) |

## Save editor

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Embedded + standalone editor | Slot menu *Edit save...*; `--editor --save` | done | done | done | editor, standalone_editor; diamond-editor, pearl-editor |
| Party / box / bag / trainer / events / Pokédex | Six tabs | done | done | done | editor; diamond-editor, pearl-editor |
| Checks, undo/redo, backups | Checksums, 16-step undo, `.bak` | done | done | done | editor; diamond-editor, pearl-editor |

## Save conversion and transfer

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Save import/export | Raw 512 KiB `.sav` / DeSmuME `.dsv` | done | shell | shell | slots |
| Cross-generation transfer | Pal Park: GBA cart + save in the slot | done (MIGRATE FROM EMERALD on the main menu) | done (MIGRATE FROM EMERALD) | done (MIGRATE FROM EMERALD) | palpark; diamond-palpark, pearl-palpark |

## Link and online

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| LAN link battles/trades | DS local wireless over LAN (Union Room, Underground) | done (two installs on one Mac, see `lan`); headless: tests/link trade, battle, Underground | done (Diamond app meets a Platinum app; D↔D, D↔P, D↔Pt trades headless in tests/link) | done (Diamond app meets a Platinum app; D↔D, D↔P, D↔Pt trades headless in tests/link) | lan; diamond-lan |
| Relay lobby, PINs | UDP relay rooms keyed by PIN | done (two installs, both "1 in range") | shell (relay transport is game-independent) | shell (relay transport is game-independent) | relay |
| Spectators, tournaments |  | no | no | no |  |
| Fast-forward locked to 1x in link play | `NP_STAT_LINK_ACTIVE` | done (snapshots/rewind/quick load refused while active) | core | core |  |
| Mystery Gift | Wonder Cards written by the editor (Member Card, Oak's Letter, Azure Flute, Secret Key) | done (card added, editor) | done (Member Card added; deliveryman gives it, headless) | done (Member Card added; deliveryman gives it, headless) | editor; diamond-editor, pearl-editor |

## Rulesets and options

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Faithful / modern rulesets | *Fix cartridge bugs* (`NP_RULE_FIX_BUGS`) | done (app log: rules check PASS) | done (Fire Fang/Wonder Guard, Rage: PASS) | done (Fire Fang/Wonder Guard, Rage: PASS) | rules; diamond-rules, pearl-rules |
| Text speed | Instant text | done | done | done | instant_text; diamond-instant_text, pearl-instant_text |
| Event tickets | Event items via Mystery Gift | done | done (Member Card, Oak's Letter, Azure Flute; no Secret Key in D/P) | done (Member Card, Oak's Letter, Azure Flute; no Secret Key in D/P) | editor; diamond-editor, pearl-editor |

## Mods

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Mod platform, manager | Runtime content packages, in-app manager | done (example package changes the main menu) | done (example_text: CONTINUE FROM THE PACKAGE) | done (example_text: CONTINUE FROM THE PACKAGE) | mods; diamond-mods, pearl-mods |
| Mod catalog, update-all |  | no | no | no |  |
| Online arena restrictions | Link realm pinned to the sealed/loose package set | done (shell) | done (shell) | done (shell) | carts; diamond-carts, pearl-carts |

## Platforms and distribution

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| macOS build | Self-contained ad-hoc signed app zip, all three cores | done | done (boots) | done (boots) | games_boot; `tools/package_macos.sh --test` |
| Windows build | exe zip, all three cores (zig cross-build) | done (title under wine) | done (title under wine) | done (title under wine) | windows (`tools/package_windows.sh --test`) |
| iOS IPA |  | no (needs Xcode) |  |  |  |
| URL launch | `nativeplat://launch?game=&slot=` through LaunchServices | done | shell | shell | url_open, launch |
| Launch flags | `--game`, `--slot`, `--launcher`, `--editor` | done | done | done | launch, games_boot |
| In-place updater | Release check + verified download (never self-replaces) | done |  |  | updater |
| UI scale, reduced motion, About |  | done |  |  | ui_scale, about |

## n/a, with reasons

- Colour modes / GBC palettes: the DS renders in full colour; display effects replace them.
- Yellow Pikachu volume, Crystal Buena points, FireRed Help/Quest Log/location previews: no counterpart in DPPt.
- Game Boy Printer protocol: no DS counterpart; image export covers it.
