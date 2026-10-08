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
`build/dist/nativeplat-macos-arm64.zip`, 2026-10-04). Platinum, Diamond,
Pearl, and Windows rows: main 80dc9d8c7.

**All three games** were run in the packaged app (main 80dc9d8c7 plus this
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
| macOS build | Self-contained ad-hoc signed app zip, all six cores (D/P/Pt, R/S/E) | done | done (boots) | done (boots) | games_boot; `tools/package_macos.sh --test` (pkg-mac) |
| Windows build | exe zip, all six cores (zig cross-build) | done (title under wine) | done (title under wine) | done (title under wine) | windows, pkg-win (`tools/package_windows.sh --test`) |
| iOS IPA |  | no (needs Xcode) |  |  |  |
| URL launch | `nativeplat://launch?game=&slot=` through LaunchServices | done | shell | shell | url_open, launch |
| Launch flags | `--game`, `--slot`, `--launcher`, `--editor` | done | done | done | launch, games_boot |
| In-place updater | Release check + verified download (never self-replaces) | done |  |  | updater |
| UI scale, reduced motion, About |  | done |  |  | ui_scale, about |

## Ruby / Sapphire / Emerald (GBA)

Status as above; **core** rows are shown headless (`np_headless`,
`tests/rse/first_battle.sh`, `tests/dp/regress.sh` e-/r-/s- cases);
**app** rows ran in the packaged macOS app (`tests/mac/feature_matrix.py
--game ruby|sapphire|emerald` on the zip built from main 80dc9d8c7, the 12
GBA cases, all ok on each game). Evidence is in `build/evidence/` (outside
git). Branch rse, 2026-10-07.

| Feature | GBA equivalent | Ruby | Sapphire | Emerald | Evidence |
|---|---|---|---|---|---|
| Boot, intro, new game | Boot to Littleroot and the house | core | core | core | regress r-/s-/e-littleroot |
| First battle | Route 101: Birch's bag, Torchic, the wild battle won | core (Poochyena; lab, map 260) | core (Poochyena) | core (Zigzagoon) | first_battle.sh; `*-01-first-battle.png` |
| Normal save / CONTINUE | The game's own flash save, CONTINUE from the title | core | core | core | first_battle.sh (continue) |
| F1 quick save, F2 F2 quick load | The game's own save from the field (refused elsewhere); quick load reboots from it | app (toast "Saved", slot changed; F2 F2 rebooted) | app | app | first_battle.sh; `<game>-gba_quicksave-*.png` |
| Snapshots, rewind | Core snapshots (`np_core_state_*`), 17.5 MB each | app (F5/F7, F6 slot 2, rewind depth) | app | app | first_battle.sh (state); `<game>-gba_snapshots-*.png` |
| Fast-forward, speed | Shell speed keys, game-independent | app (4x and G toggle: guest frames > iterations) | app | app | `<game>-gba_speed-*.png` |
| Save slots, import/export | 128 KiB flash `.sav` (mGBA/VBA), mGBA's +16-byte RTC record dropped | app (import, Continue; unit-tested) | app | app | test_shell (sav_footer); `<game>-gba_slots-*.png` |
| Screen layout | One 240x160 screen: rotation, integer/fit scale, effects | app (fit, rotation 90, integer+linear, LCD) | app | app | `<game>-gba_layouts-*.png` |
| Render scale / widescreen 3D | No 3D on the GBA | n/a | n/a | n/a |  |
| Music / SFX volume | Per m4a channel owner: BGM player's tracks (DirectSound and PSG) vs the SE players and cries | core (battle: both 256 rms 2267; BGM 0: 998; SE 0: 2035; both 0: 0); app | app | app | np_headless -o bgm_volume/se_volume; `<game>-gba_audio-*` |
| Cries | m4a DPCM (compressed) and reversed voices mixed | core (DPCM decode bit-exact vs cry_poochyena.wav) | core | core |  |
| Instant text | `NP_OPT_TEXT_INSTANT`: text printers print to the next wait (patches/text.c.patch) | core | core | core | `ruby-instant-text.png`, `emerald-instant-text.png` |
| Touch skins | Delta `.deltaskin` with `com.rileytestut.delta.game.gba`, own choice per console (`skin_gba`) | app (dropped GBA skin, landscape and portrait) | app | app | `<game>-gba_skin-*.png` |
| On-screen touch pad, editor | The DS layout without X/Y (not drawn, hit, picked or selected: `NP_TC_HIDE_GBA`) | app (pad A/B only; Tab skips X/Y; layout saved) | app | app | `<game>-gba_touch-*.png`, test_shell |
| Shell autotest | `NP_AUTOTEST rom=` on the real renderer | shell | shell | shell | `*-shell-autotest.png` |
| Mods / custom carts | Data packages: `mod.toml` + `<game>.ips` applied to the ROM at boot (`gba_mods.c`; the code is compiled, so data only); per-game `mods/<game>/`, carts pin the set | app (install .zip, menu patched; cart sealed, bound, boots the patch) | app | app | `*-mod-menu.png` vs `*-vanilla-menu.png`, `<game>-gba_mods-*`, `<game>-gba_carts-*` |
| nativeplat:// URL | Launch a GBA slot from a URL | app | app | app | `<game>-gba_launch-url.png` |
| Mystery Gift / e-Reader | n/a: Mystery Event needs an e-Reader/link partner distributing events that no longer exist; no offline event writer is in scope |  n/a | n/a | n/a |  |
| Link cable trades/battles | SIO multi-player as an exact synchronous bus over `np_host_net_*` (`gba_link.c`; LAN/relay/loopback) | core: trade with Sapphire and with Emerald (both saves hold the swapped Pokemon), Colosseum battle with Sapphire (same frames and Win/Loss screen on both) | core (trade, battle) | core (trade with Ruby, Direct Corner) | `tests/link/run_gba_link_tests.py`; `build/evidence/rse/link-trade-rs.png`, `link-trade-re.png`, `link-battle-rs.png` |
| macOS app / Windows build | All six cores in each package (main 80dc9d8c7): macOS arm64 zip, Windows x64 zig cross-build | app (package --test title; the 12 GBA cases on the packaged app); Windows: title under wine | same | same | `build/evidence/pkg-mac/<game>-arm64.png`, `pkg-win/shot-<game>.png`; docs/evidence/README.md |

## Black / White and HeartGold / SoulSilver

Where the cores are (docs/BW_PLAN.md, docs/HANDOFF-hgss.md): Black and White
boot, play a new game to the controllable bedroom, save with the X menu and
CONTINUE from it; the first battle stops (overlay 93). HeartGold and
SoulSilver play the title, the touch-screen tutorial, Prof. Oak and naming;
the first field load stops (overlay 123, `ds_protect`), so there is no in-game
save, and their sound is silent through the title and intro (one sound near
frame 8400).

Status as above, plus **not yet** (what it needs is said). **core**:
`tests/bwhgss/parity.sh` (np_headless; frames and logs in
`build/evidence/bwhgss/<game>/`). **app**: `tests/mac/feature_matrix.py --app
build/app-bwhgss/nativeplat.app --game <game>` (the 15 `n2_*` cases) on a
development build of the app with only these four cores (NP_CORE=real from the
cores' `.wasm`, 2026-10-08); evidence `build/evidence/<game>-n2_<case>-*`.
B/W in-game cases run in the bedroom from parity.sh's save (CONTINUE), HG/SS's
at ~1600 frames (the intro after the title). **shell**: the shell tests on
synthetic saves (`shell_editor_bw`, `shell_editor_hgss`, `shell_unit`). Black's
and White's columns are the same unless said; so are HeartGold's and
SoulSilver's.

| Feature | Black | White | HeartGold | SoulSilver | Evidence / what it needs |
|---|---|---|---|---|---|
| Launcher card, ROM import with SHA-1, drop | app | app | app | app | n2_import (drop: "Imported"), n2_boot (card → New → OK → title) |
| Generated cache from ROM | n/a | n/a | n/a | n/a | the core reads the cartridge at run time |
| Save slots, Continue / Edit | app (import, Continue, Edit save...) | app | not yet: New boots (n2_boot); Continue/Edit need an in-game save (field load) | same | n2_slots, n2_continue, n2_editor |
| Custom carts, mods | not yet: a mod loader in the game code (pc_modfs.c / gba_mods.c equivalent) | same | same | same |  |
| Screen layouts, swap, rotation, scaling | app | app | app | app | n2_layouts (8 layouts) |
| Battle layout | not yet: no NP_STAT_IN_BATTLE source; B/W's battle stops (docs/BWHGSS_HOOKS.md) | same | not yet: the hook is in (`pc/patches/src/encounter.c.patch`, Encounter_New to _Delete), not reachable (no field) | same |  |
| Camera zoom / tilt | not yet: NP_OPT_CAMERA_* hooks in the game code | same | same | same |  |
| Shader FX, performance presets | app | app | app | app | n2_effects |
| V-Sync, frame cap, logic clock, UI scale | shell | shell | shell | shell | game-independent |
| 3D render scale, widescreen 3D | core (renderer-side, no game hook: `-o render_scale=2` draws the title's Reshiram and the bedroom at 512x768; `-o widescreen=1` gives 342-wide frames with more of the bedroom's walls) | same core | core (the frame scales / widens; no 3D on screen before the field) | same | `build/evidence/bwhgss/hooks/` |
| Screenshots (F12) | app | app | app | app | n2_screenshot (`screenshots/<game>-*.png`) |
| Trainer Card, Pokédex diploma PNG | app/shell (editor on the save; export in shell_editor_bw) | same | shell (shell_editor_hgss exports both) | same | Unova / Johto badge names, 649 / 493 species |
| ROM-derived music / SFX | core + app (title rms 7424; app output treble 1219) | same | not yet: silent through the title and intro | same | parity.sh title; n2_audio |
| Music / SFX volume | not yet: the split classes players by Platinum's SDAT (1, 2, 7 music); B/W's music is players 0 and 6, so on Black's title `bgm_volume=0` leaves rms 7470 and `se_volume=0` takes it to 0; fix in docs/BWHGSS_HOOKS.md | same SDAT players | not yet: Platinum's numbering plus PLAYER_OPED 8; no music to hear yet | same | np_headless -o; n2_audio-bgm0 |
| Music low-pass filter | app (treble 1219 to 895 at 3X) | app (1224 to 895) | shell (no music to filter yet) | same | n2_audio-filter3 |
| Speed hotkeys, fast-forward | app (4x: 7000 iterations ran 7294 frames; G: 28000) | app | app (1600 iterations ran 1894; G: 6400) | app | n2_speed |
| Keyboard / gamepad rebinding | shell | shell | shell | shell |  |
| Touch (stylus) | core (the SAVE item tapped in the X menu) | same | core (touch tutorial, naming done by taps) | same | parity.sh |
| On-screen touch pad, layout editor | app | app | app | app | n2_touch |
| Touch skins | app | app | app | app | n2_skin |
| F1 quick save / F2 quick load | not yet: the core refuses every request (`pc_bw_e2e.c` bw_frame); needs the save routine (docs/BWHGSS_HOOKS.md) | same | not yet: the hook is in (`pc_hg_field.c`: Field_SaveGameNormal behind the start menu's gates), not reachable (no field) | same | n2_quicksave |
| Normal save, atomic writes, backups | core + app (X menu save; np_save5 verifies both copies; CONTINUE) | same | not yet: no in-game save (field load) | same | parity.sh save/continue, n2_continue |
| Snapshots (F5/F6/F7), rewind | core + app (bedroom: 3/3 round trips; rewind depth 387 to 372) | app (401 to 386) | core + app (title and Oak: round trips; rewind 263 to 248) | same | parity.sh, n2_snapshots |
| Folder sync | shell | shell | shell | shell | slot summaries read B/W and HG/SS saves |
| Portable mode | app | app | app | app | every n2 case |
| Save editor (embedded, standalone) | app (Edit save... on the real save: AAAAAAA, $3000) + shell | same | shell (synthetic saves; names and Add Pokemon with the HG ROM) | same | n2_editor; shell_editor_bw / hgss |
| Save editor tabs, checks, undo, backups | shell | shell | shell | shell | shell_editor_bw / hgss, shell_unit |
| Save import / export | app (import) + shell | same | shell (import from the HeartGold card) | same | n2_slots; shell_editor_hgss |
| Cross-generation transfer | not yet: Poké Transfer needs story progress and a second DS | same | not yet: Pal Park needs the field and the GBA slot | same |  |
| LAN / relay link | not yet: the games' wireless (C-Gear, Union Room) on the core's net host, past the battle / field blocks | same | same | same | the relay transport itself is shell |
| Fast-forward locked in link play | not yet (no link) | same | same | same |  |
| Mystery Gift | shell: `.pgf` cards into the 12 slots; delivery in game needs story progress | same | shell: `.pcd` / `.pgt` import; delivery needs the field | same | shell_editor_bw / hgss |
| Fix cartridge bugs, instant text | not yet: NP_OPT_RULES / TEXT_INSTANT in the game code (docs/BWHGSS_HOOKS.md) | same | same | same |  |
| macOS app | app (development build with these cores; packaging not run) | same | same | same | build/app-bwhgss |
| Windows build | not yet | not yet | not yet | not yet |  |
| URL launch, launch flags | app | app | app | app | n2_launch; `--game/--slot` in every case |

## n/a, with reasons

- Colour modes / GBC palettes: the DS renders in full colour; display effects replace them.
- Yellow Pikachu volume, Crystal Buena points, FireRed Help/Quest Log/location previews: no counterpart in DPPt.
- Game Boy Printer protocol: no DS counterpart; image export covers it.
