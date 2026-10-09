# macOS app evidence

Every row was run on the packaged app (`tools/package_macos.sh --test` →
`build/dist/nativeplat-macos-arm64.zip`, all three cores, main 80dc9d8c7),
unzipped into a temporary folder with `portable.txt` beside the bundle, by
`tests/mac/feature_matrix.py`: one `NP_AUTOTEST` run per step in a real window
(Metal renderer), each on its own portable user data. The screenshots were
inspected one by one (2026-10-04).

```sh
tests/mac/feature_matrix.py --list          # cases
tests/mac/feature_matrix.py [CASE...]       # on build/dist/nativeplat-macos-arm64.zip
NP_KEEP_FULL=1 tests/mac/feature_matrix.py  # also keep the 2x window captures
```

Screenshots (`<case>-<step>-small.png`, 960 wide) and logs (`<case>-<step>.log`,
first line = the exact `NP_AUTOTEST` and arguments) are in `build/evidence/`,
outside git: they show game imagery. `build/evidence/matrix.json` holds every
run's autotest line and summary. Inputs (saves minted from pc_lab recipes:
`tests/gameplay/recipes/sandgem.recipe`, `tests/link/recipes/union-*.recipe`)
are in `build/evidence/inputs/`. "Sandgem" below is that save; `CONTINUE` is
`press=1250:start;1400:a;1500:a;1600:a` (title → CONTINUE → field at ~1700),
and in-game runs start as `nativeplat --game platinum --slot Sandgem`.

| Feature | How (autotest, abridged) | Screenshots | What they show |
|---|---|---|---|
| Launcher: import 3 ROMs | `boot=app`, `drop:` Diamond and Pearl, click Import ROM, `dialog:` Platinum | launcher_import | Three cards "Ready" |
| All three games run | card click → New save slot → OK; `press=1400:start` | games_boot-diamond/pearl/platinum | Diamond, Pearl and Platinum title screens |
| Save slots | Import .sav (dialog), Duplicate, Rename (typed), Export (dialog), Delete (confirm, Yes), New | slots-1 … slots-7 | Toasts per action; exported file byte-identical; new slot boots Platinum |
| Continue | card → "Continue: Sandgem" → `CONTINUE` | continue | Sandgem Town field, Pokétch |
| Save editor | Edit save… → PageDown through tabs; Money +3, `C` undo, `V` redo; Events "Add Member Card"; Esc → Save | editor-1 … editor-12 | Six tabs; $3003/$3000/$3003; Wonder Card 1 = Member Card, 1/8 gifts; "Saved"; `.bak` written |
| Trainer Card, diploma | Trainer tab → Export… → `dialog:` path | trainer_card-card/diploma(-export) | The two PNGs (GQ, ID 27182, party, 2/493) |
| Standalone editor | `nativeplat --editor --save backup.sav` | standalone_editor | Editor on the file, no slot |
| Layouts | `[video] layout =` vertical / horizontal / hybrid / top / bottom, swap, rotation 90, integer+linear | layouts-* | Each arrangement in the Sandgem field |
| Battle layout | `battle_layout = hybrid`, `tests/gameplay/schedules/wild.press` head | battle_layout-field/battle | Vertical in the field; large top + side touch screen in the wild battle |
| Effects, presets | `effect1/2`, `crt_curvature`, `performance = low/high/balanced` | effects-* | LCD grid + scanlines, curved CRT, smooth; Low drops effects, High keeps CRT; Options shows "Balanced", VSync "(preset decides)" |
| Render scale, widescreen | `render_scale = 1/4`, `widescreen = 1`, top screen only | render_scale-* (+ sheets/render-crop.png) | 4x has smooth 3D edges vs 1x; widescreen shows more of Sandgem |
| Camera hotkeys | `script=` `-`×6, `=`×4, `4`×6, `0` | camera-* | Toasts "zoom 175%", "125%", "tilt +30 deg", back to 100% |
| Speed, fast-forward | `1`×3 at 1700; `G` at 1700 | speed-4x, speed-ff-toggle | "Speed 4x": 1800 iterations ran 2028 frames; "Fast-forward on": 6642 frames |
| F1 / F2 | walk, `F1`; `F2 F2` | quicksave-f1/f2 | "Saved" (core: "quick save 1: saved (map 418)", slot file changed); "Reloaded the last save" at the copyright screen |
| Snapshots, rewind | `F5` 1760, walk, `F7` 1860; `F6 F5`; `rewind=1880+30` | snapshots-* | Walked away / "Snapshot 1 loaded" at the door / "Snapshot 2 taken" / rewound (depth 283 → 268, frame 1792 at iteration 1915) |
| BGM/SE volume, low-pass | `bgm_volume = 0`, `se_volume = 0`, `music_filter = 3`; output measured | audio-* logs | Peak/treble: default 29152/834, BGM 0 14880/501, SE 0 29152/666, filter 3X 27149/476 |
| Instant text | new game, A presses at 1700…2000, capture at 2015 | instant_text-off/on | Off: "Welcome" half printed; on: the same presses are already past it (Rowan on screen) |
| Rules | `fix_bugs = 1`, env `PC_NP_RULES_CHECK=1`, Options | rules-options, rules-check.txt | "Fix cartridge bugs < On >"; core log "rules check: PASS" |
| Controls | Options → Controls… → A → `K` | controls-page/rebound | A bound to K; `a = K` in options.ini |
| Touch controls, editor | `touch_controls = on`; Edit touch controls…, Tab/arrows/`=`/`[` | touch_editor-* | Pad over the game; editor overlay; B resized/faded |
| Delta skin | `drop:` hand-made `Test.deltaskin` (tests/mac/fixtures.py), 1280x592 and 390x844 | skin-landscape/portrait | Skin art with the DS screens in its frames, both orientations |
| Mods | Options → Mods… → `drop:` example zip; boot | mods-installed/menu | "[on] Example: main menu labels ready"; main menu "RESUME ADVENTURE", "START FRESH (MODDED)" |
| Custom carts | Seal enabled packages as a cart… "Menu Cart"; slot Cart: → Menu Cart; boot | carts-* | "Sealed cart Menu Cart (1 package, 0a33980d)"; "Sandgem now plays cart Menu Cart"; modded menu |
| GBA cart, Pal Park | Options → GBA cartridge `dialog:` pokeemerald.gba, GBA save `dialog:` generated Emerald save; National Dex save | palpark-options/menu | "GBA: pokeemerald.gba, save emerald.sav"; main menu "MIGRATE FROM EMERALD" |
| LAN, two installs | Two copies of the app, `[wireless] enabled, port, peer`, `realtime=1`, tests/link `trade-a/b.sched` | lan-station-a/b-008000/010000/011000 | Both in the Union Room, chat log "UNIONA/UNIONB: I've entered the Union Room"; A talks to B (GREET/DRAW/BATTLE/TRADE menu), B "Awaiting a response from UNIONA" |
| Relay | Relay host:port and PIN typed in Options, LAN on; 2nd install with the same relay+PIN (local `server/relay`) | relay-options, relay-station-b | Both "1 in range" |
| Folder sync | sync folder; both copies changed; chooser; "Use the other copy" | sync-1/2/3 | Chooser shows this device (GQ) vs other copy (UNIONA); "Kept one copy" |
| Updater | `[updates] api =` local release server (fixtures.ReleaseServer); Check, Download and verify; bad digest | updater-* | "v9.9.9 is available", "Verified (SHA-256 …)", wrong digest refused and deleted |
| Launch flags | `--game platinum --slot Sandgem`, `--launcher`, unknown slot | launch-* | Game starts; launcher; "Platinum has no save slot Nope" |
| URL | `open -n -a nativeplat.app 'nativeplat://launch?game=platinum&slot=Sandgem'` (LaunchServices) | url_open | Log "link: nativeplat://…", Platinum running in slot Sandgem |
| Windows package | `tools/package_windows.sh --test`: zig cross-build with all six cores, the exe run under wine (OrbStack amd64, image `nativeplat-wine`), DS `frames=1500,press=1200:start:10`, GBA `frames=900,press=400:start:10`; `NP_WIN_SHOTS=build/evidence/pkg-win` | pkg-win/shot-diamond/pearl/platinum/ruby/sapphire/emerald.png | The six title screens from nativeplat.exe (wine, dummy video; main 80dc9d8c7) |
| Portable mode | `portable.txt` beside the bundle (every case) | portable | Launcher footer "Portable data: …/userdata/" |
| UI scale, reduced motion | `ui_scale = 2/6`; `reduce_motion = 1` on the name page | ui_scale-* | 2x small text; "6x (5x fits)"; steady caret |
| About | About button | about | License and credits |

## Diamond and Pearl

The same in-game cases, run as `tests/mac/feature_matrix.py --game diamond`
and `--game pearl` on the package built from main 80dc9d8c7 (evidence names
`diamond-<case>` / `pearl-<case>`). Saves: `tests/gameplay/dp/recipes/sandgem.recipe`
and `tests/link/recipes/dp-union-a.recipe` minted with `tests/gameplay/mint.sh`
(`NP_GAME=diamond|pearl`, base = the new-game save from
`tests/dp/<game>_first_save.sched`). Contact sheets: `build/evidence/sheets/`
`d-continue-editor.jpg`, `d-display.jpg`, `d-session.jpg`, `d-misc.jpg`,
`d-lan.jpg`, `pearl.jpg`.

| Feature | Diamond / Pearl evidence | What they show |
|---|---|---|
| Continue | diamond-continue, pearl-continue | Sandgem Town field (full-size 3D), Pokétch |
| Save editor, Mystery Gift | diamond-editor-*, pearl-editor-* | "Edit Diamond save" / Pearl, all six tabs, undo/redo; Events lists Member Card, Oak's Letter, Azure Flute (no Secret Key on D/P); Member Card added (1/8 gifts); saved with `.bak`; save4 reads it as DP |
| Layouts, swap, rotation | *-layouts-* | All eight arrangements |
| Battle layout | *-battle_layout-field/battle | Vertical in the field; wild BIDOOF battle in hybrid (large top) |
| Effects, presets | *-effects-* | LCD+scanlines, curved CRT, smooth, Low/High, Options "Balanced" |
| Render scale, widescreen | *-render_scale-* | 1x, 4x, 4x widescreen (wider Sandgem) |
| Camera | *-camera-* | Toasts zoom 175% / 125% / tilt +30 / reset 100%, the field following |
| Speed | *-speed-* | "Speed 4x" (2025 frames in 1800 iterations), "Fast-forward on" (6648) |
| F1 / F2 | *-quicksave-* | "Saved" (core: "quick save 1: saved (map 418)"); "Reloaded the last save" |
| Snapshots, rewind | *-snapshots-* | "Snapshot 1 loaded", "Snapshot 2 taken", rewind depth 286 → 271 |
| Audio | *-audio-* logs | peak/treble default 25968/661, BGM 0 9644/196, SE 0 25968/631, low-pass 3X 24677/400 (both games) |
| Instant text | *-instant_text-off/on | Off: Rowan's "Howev…" half printed; on: the same presses already reach the adventure-rules page |
| Rules | *-rules-options, log | "Fix cartridge bugs < On >"; core: Fire Fang/Wonder Guard, Shadow Force, Rage checks, "rules check: PASS" |
| Mods | *-mods-installed/menu | "Mods (Diamond)" / Pearl, `example_text` (cooked with `pc/modcook.py`) installed from a zip, enabled; main menu "CONTINUE FROM THE PACKAGE", "NEW GAME (CONTENT PACKAGE)" |
| Custom carts | *-carts-* | Sealed "Menu Cart" (1 package), bound to Sandgem, boots the modded menu |
| Pal Park | *-palpark-options/menu | Emerald cart and save inserted; main menu "MIGRATE FROM EMERALD" |
| LAN, Diamond ↔ Platinum | diamond-lan-station-a/b (`-008000/-009000/-012000.png`), diamond-lan-parties.txt | A Diamond app and a Platinum app on this Mac meet in the Union Room, open the trade screen (TURTWIG ↔ CHIMCHAR), the trade animation runs on both, Diamond: "Take good care of CHIMCHAR!"; afterwards the Diamond save holds CHIMCHAR and the Platinum save TURTWIG (tests/link `dp-pt-trade` schedules, `realtime=1`) |

## Ruby, Sapphire and Emerald

The 12 GBA cases, run as `tests/mac/feature_matrix.py --game ruby`,
`--game sapphire` and `--game emerald` on the package built from main
80dc9d8c7 (evidence names `<game>-gba_<case>`; 12/12 ok on each game).
Saves are the games' own: the house save `tests/rse/first_battle.sh` writes
(`build/rse/first_battle/<game>/house.sav`, littleroot.sched and a quick
save). `tools/package_macos.sh --test` also boots each
of the six games from the unzipped bundle (`NP_MAC_SHOTS=build/evidence/pkg-mac`:
`<game>-arm64.png`, the six title screens; GBA: START at 400, capture at 900).

| Feature | Evidence | What they show |
|---|---|---|
| Continue | *-gba_continue | CONTINUE from the title into the Littleroot house |
| F1 / F2 | *-gba_quicksave-* | "Saved" (the game's own save, slot file changed); F2 F2 reboots into the saved game |
| Snapshots, rewind | *-gba_snapshots-* | F5/F7, F6 slot 2, rewind depth falls |
| Speed | *-gba_speed-* | 4x and the G fast-forward toggle: more guest frames than iterations |
| Audio | *-gba_audio-* logs | BGM 0 and SE 0 change the measured output per channel owner |
| Layouts | *-gba_layouts-* | One 240x160 screen: fit, rotation 90, integer+linear, LCD effect |
| Touch pad, editor | *-gba_touch-* | A/B pad without X/Y; the editor skips X/Y; the layout is saved |
| Delta skin | *-gba_skin-* | A GBA `.deltaskin` (fixtures.deltaskin(gba=True)), landscape and portrait |
| Mods | *-gba_mods-* | The example data patch installed from a zip; the main menu labels patched |
| Custom carts | *-gba_carts-* | Cart sealed, bound to the slot, boots the patched game (Emerald: title screen) |
| Save slots | *-gba_slots-* | `.sav` import, Continue from the imported slot |
| URL | *-gba_launch-url | `nativeplat://launch?game=<game>&slot=…` starts that game's slot |

Link cable (headless, two `np_headless` stations over the network layer):
`tests/link/run_gba_link_tests.py` trades Ruby↔Sapphire and Ruby↔Emerald
(each save's party holds the other side's Pokémon afterwards) and battles
Ruby vs Sapphire in the Colosseum (in_battle 3605..14830 on both, the same
Win/Loss screen). Sheets: `build/evidence/rse/link-trade-rs.png`,
`link-trade-re.png`, `link-battle-rs.png`.

## Black, White, HeartGold and SoulSilver

Not the packaged app: a development build with only these four cores, made
from the cores' `.wasm` (2026-10-08, the proposal builds of
docs/BW_PLAN.md and docs/HANDOFF-hgss.md):

```sh
cmake -S shell -B build/app-bwhgss -G Ninja -DCMAKE_BUILD_TYPE=Release -DNP_CORE=real \
  -DBUILD_TESTING=OFF -DNP_BUILD_TESTS=OFF "-DCMAKE_C_FLAGS_RELEASE=-O1 -g0 -DNDEBUG" \
  -DNP_GUEST_WASM_black=<ndsrec-black.wasm> -DNP_GUEST_WASM_white=<ndsrec-white.wasm> \
  -DNP_GUEST_WASM_heartgold=<pokeheartgold.wasm> -DNP_GUEST_WASM_soulsilver=<pokesoulsilver.wasm> \
  -DNP_GUEST_POSTPROCESS=$PWD/tools/wasm2c_postprocess.py
NP_BW_CORE=<dir with np_headless> NP_HGSS_CORE=<dir> NP_BLACK_ROM=… NP_WHITE_ROM=… NP_HG_ROM=… NP_SS_ROM=… \
  NP_SAVE5=<np_save5> tests/bwhgss/parity.sh          # headless; also makes the B/W bedroom saves
NP_BLACK_ROM=… tests/mac/feature_matrix.py --app build/app-bwhgss/nativeplat.app --game black   # n2_* cases
```

All 15 `n2_*` cases ran on each game: 15/15 ok on Black and White; on
HeartGold and SoulSilver 11 ok and 4 skipped with their reason (continue,
slots, editor: no in-game save; audio: no music yet). The per-feature status is
in docs/FEATURE_PARITY.md ("Black / White and HeartGold / SoulSilver"). A 16th
case, `n2_render`, was added afterwards and passes on all four: F12's core frame
is 256x384 at render scale 1, 512x768 at 2 and 684x768 with widescreen
(`<game>-n2_render-*-frame.png`; White's widescreen bedroom looked at).
A 17th, `n2_battle_layout` (B/W only; 2026-10-09, an app built the same way with only
the Black and White cores, build/app-bw, from main 73db46f55's cores), runs the bedroom save
through the gift box into Bianca's battle with `battle_layout = hybrid`: the bedroom vertical,
the battle hybrid (large top, the touch screen small at the right), the bedroom after it
vertical again, on Black and White (`<game>-n2_battle_layout-{field,battle,after}.png`, looked at).
Screenshots looked at: Black title (n2_boot), bedroom after CONTINUE
(n2_continue), "Snapshot 1 loaded" in the bedroom (n2_snapshots-restored),
the editor's Trainer tab on the real save (n2_editor-trainer: AAAAAAA, ID
45994, $3000, 0:03:03), HeartGold's intro in the hybrid layout and the touch
layout editor; headless: the bedroom after the X-menu save and after CONTINUE,
HeartGold's naming screen.

## Defects and gaps

- **Platinum ↔ Platinum LAN trade not completed in the app.** The stations
  met and talked, but tests/link's Platinum trade schedule is timed for frame
  lockstep; two free-running apps drift, and B's accept presses fell on "You
  declined the offer". The Diamond ↔ Platinum run above completed its trade;
  the Platinum trade is covered headless by `tests/link/run_link_tests.py`.
- **Snapshots/rewind are refused for ~a second after CONTINUE**: the main
  menu's wireless check keeps `NP_STAT_LINK_ACTIVE` set until the field;
  F5 at frame 1700 was refused as "during a wireless session". By design,
  not a shell bug; the case takes snapshots from 1760.
- Shell defects fixed for this pass: editor labels were cut off; options rows
  that Left/Right cannot change showed `< >`; autotest discarded option keys
  under `boot=app`, ignored speed, did not poll the network in menus, and
  silently truncated long scripts; `package_macos.sh` left Diamond and Pearl
  out unless given their paths, and only tested Platinum. For Diamond and
  Pearl: the mod manager and custom carts were Platinum-only (now per game:
  `mods/<game>/`, `carts/<game>/`, L/R switch the page's game), and the GBA
  slot was only inserted for Platinum (now every game, for Pal Park).
  `package_windows.sh` gained the same D/P defaults and per-game wine test.
