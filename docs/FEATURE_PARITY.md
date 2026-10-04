# Gen1Recomp feature parity

Every player-facing Gen1Recomp (G1R Deluxe) feature, its Diamond/Pearl/Platinum
equivalent, and its status per game. Source inventory: Gen1Recomp `dev`
(340e256) docs and code. Gen1Recomp's launcher is proprietary (its LICENSE.MD
term 2), so nativeplat ships its own launcher; no Gen1Recomp code is copied.

Status: **done** shown working in the packaged macOS app (evidence linked) ·
**core** works in the core / headless tests, not yet shown in the app ·
**shell** app-side works, the game cannot reach it yet · **wip** in
progress · **no** not started · **n/a** no counterpart (reason given).

Evidence names are cases in [docs/evidence/README.md](evidence/README.md)
(screenshots in `build/evidence/`, made by `tests/mac/feature_matrix.py` on
`build/dist/nativeplat-macos-arm64.zip`, 2026-10-04, main 904498583).

**Diamond and Pearl today:** both boot in the packaged app to their title
screen (`games_boot`) and run the intro, Rowan and the naming screens into
the bedroom (DPCore). Open defects block play beyond that: the field START
menu opens BAG by itself so SAVE is unreachable, and the field 3D is drawn at
about a third of its size. No Diamond/Pearl save has been made in game, so
every row that needs one (editor on a real D/P save, Continue, quick save,
Pal Park migration, link) is untested for D/P. Features already wired into
the D/P core are marked **core**.

Shell features that do not depend on the game (launcher, slots, options UI,
sync, updater, skins) work the same for every game; they were exercised with
Platinum data.

Landed on main after the evidence package was built (not yet shown in the
app): D/P quick save no longer overflows the idle thread's stack
(d12b3fa7e), and save4 plus the shell editor handle D/P Mystery Gift, unlock,
Pokédex flags and add-mon (b27d9a954).

## Launcher and import

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Multi-game launcher, per-game cards | Diamond / Pearl / Platinum cards with readiness | done | done | done | launcher_import, games_boot |
| ROM import with SHA-1 verification | US dumps; Platinum rev 0 recognised and refused | done | done | done | launcher_import |
| File picker and drag-and-drop import | SDL3 dialog + drop | done | done | done | launcher_import (2 drops, 1 dialog) |
| Generated cache from ROM | Not needed: the core reads the cartridge filesystem at runtime | n/a | n/a | n/a | |
| Save slots, Continue / Edit / Delete | Named slots: new, duplicate, rename, export/import .sav, delete | done | shell | shell | slots, continue |
| Custom carts | Sealed mod package sets bound to a slot | done | no (no D/P mods UI) | no | carts |
| Import inbox (mobile Documents) | Same on iOS | no (iOS deferred) | no | no | |

## Display

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Screen layout / position | Vertical, side by side, hybrid, single screen, swap, rotation, integer/linear | done | shell | shell | layouts |
| Widescreen battle layout | Separate layout while `NP_STAT_IN_BATTLE` | done | core (stat wired, f1c940ad3) | core | battle_layout |
| Survey zoom | Field camera zoom (`-` / `=`) | done | core (618a28f83) | core | camera |
| Perspective tilt | Field camera tilt (`3` / `4`), `0` resets | done | core | core | camera |
| GBC screen effects / Shader FX | Two-slot chain: LCD grid, scanlines, CRT (+curvature), smooth | done | shell | shell | effects |
| Performance presets | Custom / High / Balanced / Low / Auto | done | shell | shell | effects |
| V-Sync, display frame cap | Same | done (Options) | shell | shell | effects-preset-options |
| Logic clock 60 / cart rate | 60 Hz or 59.8261 Hz | done (Options) | shell | shell | |
| Upscaled 3D | 3D render scale 1x-4x, widescreen 3D | done | no | no | render_scale |
| Screenshots | F12 PNG | done | shell | shell | |
| Pokédex diploma / printer export | Trainer Card and diploma PNG from the save editor | done | no (needs a D/P save) | no | trainer_card |
| Colour modes | GB palettes | n/a | n/a | n/a | DS is full colour |

## Audio

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| ROM-derived music/SFX | Native SDAT playback via the ARM7 sound driver model | done | core (title theme measured against Platinum's, 8177dd64c) | core | audio, games_boot |
| Music / SFX volume | BGM / SE volume per sequence player | done (Sandgem field: BGM 0 halves peak 29152 to 14880) | core | core | audio |
| Music low-pass filter | Off / 1X / 2X / 3X | done (treble 834 to 476 at 3X) | shell | shell | audio |
| Fast-forward audio | Normal-pitch snippets above 1x | done | shell | shell | |

## Input

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Keyboard / gamepad defaults, rebinding | Every action, 3 keys + 1 pad button | done | done | done | controls |
| Touch: mouse and fingers | Stylus on the bottom screen in every layout | done | shell | shell | |
| On-screen touch pad, layout editor | Same, per orientation | done | shell | shell | touch_editor |
| Touch skins | Delta `.deltaskin` (hand-made test skin) | done | shell | shell | skin |
| Speed hotkeys up to very high | `1` cycles 1x-8x/uncapped; F hold, G toggle | done (1800 iterations ran 6642 frames with G) | shell | shell | speed |
| F1 quick save / F2 quick load | The game's own save without its menu; reboot from it | done | core (618a28f83; save unreachable in D/P today) | core | quicksave |

## Saves

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Normal save, atomic writes, backups | Backup chip image, temp + rename, `.bak` | done | shell | shell | slots, editor |
| Semantic checkpoints | 4 in-memory snapshot slots (F5/F6/F7) and hold-to-rewind | done | no (not tried in app) | no | snapshots |
| Cloud / device sync | Folder sync, three-way with conflict chooser | done | shell | shell | sync |
| Portable mode (`portable.txt`) | Same | done | done | done | portable (every case runs portable) |

## Save editor

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Embedded + standalone editor | Slot menu *Edit save...*; `--editor --save` | done | no (no D/P save) | no | editor, standalone_editor |
| Party / box / bag / trainer / events / Pokédex | Six tabs | done | no | no | editor |
| Checks, undo/redo, backups | Checksums, 16-step undo, `.bak` | done | no | no | editor |

## Save conversion and transfer

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Save import/export | Raw 512 KiB `.sav` / DeSmuME `.dsv` | done | shell | shell | slots |
| Cross-generation transfer | Pal Park: GBA cart + save in the slot | done (MIGRATE FROM EMERALD on the main menu) | core (CTRDG selftest, d7c2a72bf) | core | palpark |

## Link and online

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| LAN link battles/trades | DS local wireless over LAN (Union Room, Underground) | done (two installs on one Mac, see `lan`); headless: tests/link trade, battle, Underground | core (WM model linked, 04dbfeebb; untested) | core | lan |
| Relay lobby, PINs | UDP relay rooms keyed by PIN | done (two installs, both "1 in range") | shell | shell | relay |
| Spectators, tournaments | | no | no | no | |
| Fast-forward locked to 1x in link play | `NP_STAT_LINK_ACTIVE` | done (snapshots/rewind/quick load refused while active) | core | core | |
| Mystery Gift | Wonder Cards written by the editor (Member Card, Oak's Letter, Azure Flute, Secret Key) | done (card added, editor) | no | no | editor |

## Rulesets and options

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Faithful / modern rulesets | *Fix cartridge bugs* (`NP_RULE_FIX_BUGS`) | done (app log: rules check PASS) | core (Fire Fang + Rage) | core | rules |
| Text speed | Instant text | done | core | core | instant_text |
| Event tickets | Event items via Mystery Gift | done | no | no | editor |

## Mods

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| Mod platform, manager | Runtime content packages, in-app manager | done (example package changes the main menu) | core (22f5cff9e; manager is Platinum-only) | core | mods |
| Mod catalog, update-all | | no | no | no | |
| Online arena restrictions | Link realm pinned to the sealed/loose package set | done (shell) | n/a | n/a | carts |

## Platforms and distribution

| Gen1Recomp | DPPt equivalent | Platinum | Diamond | Pearl | Evidence |
|---|---|---|---|---|---|
| macOS build | Self-contained ad-hoc signed app zip, all three cores | done | done (boots) | done (boots) | games_boot; `tools/package_macos.sh --test` |
| Windows build | exe zip | builds (not re-checked here) | | | docs/BUILDING.md |
| iOS IPA | | no (needs Xcode) | | | |
| URL launch | `nativeplat://launch?game=&slot=` through LaunchServices | done | shell | shell | url_open, launch |
| Launch flags | `--game`, `--slot`, `--launcher`, `--editor` | done | done | done | launch, games_boot |
| In-place updater | Release check + verified download (never self-replaces) | done | | | updater |
| UI scale, reduced motion, About | | done | | | ui_scale, about |

## n/a, with reasons

- Colour modes / GBC palettes: the DS renders in full colour; display effects replace them.
- Yellow Pikachu volume, Crystal Buena points, FireRed Help/Quest Log/location previews: no counterpart in DPPt.
- Game Boy Printer protocol: no DS counterpart; image export covers it.
