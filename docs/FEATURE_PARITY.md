# Gen1Recomp feature parity

Every player-facing Gen1Recomp (G1R Deluxe) feature, its Diamond/Pearl/Platinum
equivalent, and where nativeplat stands. Source inventory: Gen1Recomp `dev`
(340e256) docs and code. Gen1Recomp's launcher is proprietary (its LICENSE.MD
term 2), so nativeplat ships its own launcher; no Gen1Recomp code is copied.

Status: `done` works end to end · `wip` in progress · `planned` designed, not
started · `n/a` no DS equivalent (reason given).

## Launcher and import

| Gen1Recomp | DPPt equivalent | Status |
|---|---|---|
| Multi-game launcher, per-game cards | Diamond / Pearl / Platinum cards with readiness | wip (shell) |
| ROM import with SHA-1 verification | US Diamond, Pearl, Platinum rev1 `.nds`; rev0 recognised and refused | wip (shell) |
| File picker and drag-and-drop import | SDL3 dialog + drop; iOS document picker | wip (shell) |
| Generated cache from ROM | Not needed: the core reads the cartridge filesystem at runtime; the verified ROM is kept in private storage | wip |
| Save slots, Continue / Edit / Delete | Per-game save slots in the launcher | planned |
| Custom carts | Sealed mod packages with manifest hash | planned |
| Import inbox (mobile Documents) | Same on iOS | planned |

## Display

| Gen1Recomp | DPPt equivalent | Status |
|---|---|---|
| Screen layout / position | Stacked, side-by-side, single, hybrid, swap, rotation | wip (shell) |
| Integer / fit scaling, filters | Same | wip (shell) |
| Survey zoom + void fill | Field camera zoom-out through the game's camera API; letterbox fill | wip (core: `NP_OPT_CAMERA_ZOOM` 64..1024, Platinum; UI: shell) |
| Perspective tilt | Field camera pitch control (the overworld is already 3D) | wip (core: `NP_OPT_CAMERA_TILT` ±45°, Platinum; UI: shell) |
| Colour modes | n/a (Game Boy palettes); replaced by display filters | n/a |
| GBC screen effects / Shader FX | Two-slot post-process chain (LCD grid, CRT, scanlines, presets) | planned |
| Performance presets | Presets for 3D resolution, shaders, frame cap | planned |
| V-Sync, display frame cap | Same | wip (shell) |
| Logic clock 60 / cart rate | 60 Hz or DS 59.8261 Hz | wip (shell) |
| Widescreen battle layout | Dual-screen battle layout presets | planned |
| Screenshots | F12 PNG of both screens | wip (shell) |
| Pokédex diploma / printer export | Export the in-game diploma and Trainer Card as images | planned |
| Upscaled 3D | Internal 3D resolution multiplier | wip (core: `NP_OPT_RENDER_SCALE` 1..4 and `NP_OPT_WIDESCREEN`, live, Platinum; UI: shell) |

## Audio

| Gen1Recomp | DPPt equivalent | Status |
|---|---|---|
| ROM-derived music/SFX | Native SDAT playback through the port's ARM7 sound driver + SPU model | wip (core; Platinum. Diamond/Pearl: the sound heap moved into the port window (`games/diamond/pc/patches/arm9/src/sound.c.patch`) so SOUNDxSAD's 27 bits reach the waves. Title theme measured against Platinum's: rms 7354/6014 vs 7239/6001, spectral flatness 0.003 vs 0.002 (white noise 0.56), pitch-class profile correlation 0.999, same beat lags. Title Start SE and text-advance SE audible; cries not yet heard, because the Rowan intro stops before his Pokémon) |
| Music / SFX volume | Master + BGM/SE volumes | wip (core: `NP_OPT_BGM_VOLUME` / `NP_OPT_SE_VOLUME` per sequence player, Platinum; measured in Twinleaf with the X menu's sounds and BGM muted: RMS 3282 at `se_volume` 256, 0.0 at 0. Diamond/Pearl: `games/diamond/pc/src/pc_dp_snd.c` maps driver players to SDAT players from NitroSystem's sSeqPlayer/sPlayer. With `bgm_volume` 0 the title theme and Rowan's theme read 0.0 while the text-advance SE bursts keep their full-volume level (rms 2646/1128/122/13 per 10 frames in both runs); master + UI: shell) |
| Music low-pass filter | Optional output filter | planned |
| Fast-forward audio | Natural pitch at 1x, muted or dropped above | wip (shell) |
| Lifecycle pause on mobile | Same | wip (shell) |

## Input

| Gen1Recomp | DPPt equivalent | Status |
|---|---|---|
| Keyboard / gamepad defaults | DS buttons incl. X/Y/L/R | wip (shell) |
| Rebinding UI | Same, incl. fast-forward bindings | wip (shell) |
| Touch: mouse and fingers | Stylus on the bottom screen in every layout | wip (shell) |
| On-screen touch pad | DS control deck on touch devices | wip (shell) |
| Touch layout editor, haptics | Same | planned |
| Touch skins (Delta / RetroArch import) | Delta DS skins (`com.rileytestut.delta.game.ds`) | planned |
| Speed hotkeys, up to very high speeds | 1x-8x and uncapped | wip (shell) |
| F1 quick save / F2 quick load | Trigger the in-game save from the field; reload last save | wip (core: `NP_OPT_QUICKSAVE_SEQ` runs the start menu's save without UI, `NP_STAT_QUICKSAVE_RESULT`; UI: shell) |

## Saves

| Gen1Recomp | DPPt equivalent | Status |
|---|---|---|
| Normal save, atomic writes, backups | Backup chip image, atomic write + `.bak` | wip (shell/core) |
| Semantic checkpoints | Whole-machine snapshots at frame boundaries (in-session) | wip (core: `np_core_state_*`, ~6.9 MB, ~5 ms on macOS; not on Windows yet; UI: shell) |
| Cloud / device sync | Self-hostable sync server + client | planned |
| Portable mode (`portable.txt`) | Same | wip (shell) |

## Save editor

| Gen1Recomp | DPPt equivalent | Status |
|---|---|---|
| Embedded + standalone editor | Gen 4 save editor (block checksums, PKM encryption) | planned |
| Party / box / bag / trainer / events / Pokédex | Same for Gen 4 structures | planned |
| Checks, undo/redo, backups | Same | planned |

## Save conversion and transfer

| Gen1Recomp | DPPt equivalent | Status |
|---|---|---|
| Gen 1/2/3 save import/export | DS `.sav` import/export (emulator-compatible raw image) | planned |
| Cross-generation transfer | Pal Park: a Gen 3 `.sav` + ROM header presented in the emulated GBA slot | wip (core done for Platinum: `np_host.gba_rom_*`/`gba_save_*`, the ROM in the slot window, a 128 KiB flash (MX29L010 ID) / 32 KiB SRAM chip model behind the SDK's CTRDG bus accesses (`pc/src/pc_agb_slot.c`, `pc/patches/.../ctrdg/src`), save stored once writes settle; verified with pret/pokeemerald and a generated Emerald save (`tools/gba/gen3_save.py`): MIGRATE FROM EMERALD on the main menu, six Pokémon migrated, both saves written back and valid; UI: shell; D/P: needs the same CTRDG patches on its SDK) |

## Link and online

| Gen1Recomp | DPPt equivalent | Status |
|---|---|---|
| LAN link battles/trades | DS local wireless (Union Room, Underground, battles, trades) over LAN | wip (Platinum: ARM7 WM model `pc/src/pc_wm.c` behind PXI tag 10 over UDP `shell/src/net.c`; verified end to end between two headless stations by `tests/link/run_link_tests.py`: Union Room trade (both saves swapped), Union Room single battle to the WIN/LOSE screen, Underground meeting; Colosseum not yet tested; D/P not wired) |
| Relay online lobby, battles, trades, spectators, tournaments, PINs | Same over a self-hostable relay | wip (UDP room relay keyed by PIN, `server/relay`, docs/RELAY.md; a Union Room trade completes through it with 10% loss on each side (`tests/link` relay_trade); no lobby/spectators/tournaments yet) |
| Fast-forward locked to 1x in link play | Same | wip (core: `NP_STAT_LINK_ACTIVE` from the WM model; shell forces 1x, np_headless paces to 60 Hz) |
| Mystery Gift | Wonder Card (PGT/PCD) injection through the in-game Mystery Gift menu | planned |

## Rulesets and options

| Gen1Recomp | DPPt equivalent | Status |
|---|---|---|
| Faithful / modern rulesets | Faithful vs. documented bug fixes (decomp `docs/bugs_and_glitches.md`) | wip (core: `NP_RULE_FIX_BUGS` fixes Fire Fang/Wonder Guard, Rage, trainer form stats in Platinum; `PC_NP_RULES_CHECK=1` runs each fix through the game's code with the bit off and on and logs PASS) |
| Text speed, battle animations, battle style | Native game options (already in DPPt), plus instant text (`NP_OPT_TEXT_INSTANT`) | done (in game); instant text done in Platinum core (a box fills on its first frame; prompts and scrolls unchanged), UI: shell |
| Event tickets (Emerald) | Event items for DPPt events (e.g. Member Card, Oak's Letter, Azure Flute) via Mystery Gift | planned |

## Mods

| Gen1Recomp | DPPt equivalent | Status |
|---|---|---|
| Mod platform, manager, profiles, dependencies | Port's mod system (`pc/mods`, cook/port) behind an in-app manager | wip (core done for Platinum: runtime packages load from `np_host.content_root`, a read-only WASI preopen, ordered by `PC_MODS`/`loadorder.txt`, errors as `np_core_last_error`; example `pc/mods/example_menu_text`; manager UI: shell; D/P: no runtime packages yet) |
| Mod catalog, update-all | Same | planned |
| Online arena restrictions | Vanilla or sealed packages only | planned |
| Tiled map editing | n/a for now (DS maps are 3D models + BDHC); revisit | planned |

## Platforms and distribution

| Gen1Recomp | DPPt equivalent | Status |
|---|---|---|
| macOS, Windows builds | Native app bundle / exe | planned |
| iOS IPA (AltStore/SideStore/Feather) | Same | planned (needs Xcode) |
| Home Screen shortcuts, URL launch | `nativeplat://launch?game=platinum` | planned |
| Launch flags | `--game`, `--slot`, `--launcher` | planned |
| In-place updater | Release check + verified download | planned |

## n/a, with reasons

- Colour modes / GBC palettes: the DS renders in full colour; display filters replace them.
- Yellow Pikachu volume, Crystal Buena points, FireRed Help/Quest Log/location previews: no counterpart in DPPt.
- Game Boy Printer protocol: no DS counterpart; image export covers it.
