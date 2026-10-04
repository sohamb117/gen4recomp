# macOS app evidence

Produced by `tests/mac/feature_matrix.py`: one `NP_AUTOTEST` run per step in
a real window (Metal renderer), each on its own copy of portable user data.
Screenshots and logs are in `build/evidence/` (not committed: they are large
and some contain game imagery). `build/evidence/matrix.json` holds every run's
exact autotest line and summary.

```sh
tests/mac/feature_matrix.py --list                 # the cases
tests/mac/feature_matrix.py                         # the packaged zip (build/dist)
tests/mac/feature_matrix.py --app path/nativeplat.app CASE...
```

## Status of this pack (2026-10-04)

**Launcher/UI rows were run against a stub-core build of the shell
(`-DNP_CORE=stub`, `build/shell-stub`), not yet against the packaged
three-core app.** The three-core package (`tools/package_macos.sh`) was not
built in this pass: the wasm rebuilds waited on the machine-wide heavy-build
slots. The UI paths (pages, dialogs, files written) are the same code in
both builds; the in-game rows need the real cores and are listed as not run.

| Case | Feature | Result (stub core) | Screenshots (build/evidence/) | Inspected |
| --- | --- | --- | --- | --- |
| launcher_import | Import all 3 ROMs (2 dropped, 1 via Import ROM + dialog) | ok | launcher_import.png | 3 cards Ready |
| slots | Import .sav, duplicate, rename, export, delete (confirm), new slot | ok, 7 steps | slots-1-import … slots-7-new.png | each toast/page; export byte-identical |
| editor | Six tabs, money edit, undo (X), redo (Y), Member Card gift, close prompt, save + .bak | ok, 12 steps | editor-1-trainer … editor-12-saved.png | all |
| trainer_card | Trainer Card + diploma PNG export | ok | trainer_card-card-export.png, trainer_card-diploma-export.png | both images |
| standalone_editor | `--editor --save <file>` | ok | standalone_editor.png | editor opens on file |
| controls | Rebind A to K, saved to options.ini | ok | controls-page.png, controls-rebound.png | `a = K` |
| touch_editor | Touch pad, layout editor, select/resize | ok | touch_editor-*.png | pad + editor overlay |
| skin | Hand-made .deltaskin dropped (landscape), portrait | ok | skin-landscape.png, skin-portrait.png | art + screens placed |
| mods | Example package .zip install (enabled) | ok (boot step: not run on real core) | mods-installed.png | `[on] ... ready` |
| relay | Relay host/PIN typed, LAN on; 2nd install on same relay+PIN | ok | relay-options.png, relay-station-b.png | both "1 in range" |
| sync | Mirror, conflict chooser (other device's UNIONA copy), resolve | ok | sync-1 … sync-3.png | chooser shows both copies |
| updater | Local release server: check, download+verify; wrong digest deleted | ok | updater-checked/downloaded/bad-digest.png | verified / refused |
| launch | `--game --slot`, `--launcher`, unknown slot message, nativeplat:// URL (drop event) | ok | launch-*.png | each |
| ui_scale | UI scale 2x, 6x ("5x fits"), reduce motion steady caret | ok | ui_scale-*.png | each |
| about | About / credits | ok | about.png | |
| portable | portable.txt → userdata/ | ok | portable.png | footer path |

Not run yet (need the three-core package): continue, games_boot, layouts,
battle_layout, effects, render_scale, camera, speed, quicksave, snapshots,
audio, instant_text, rules, carts, palpark, lan (two installs in the Union
Room), and `open nativeplat://` through LaunchServices. The cases exist in
`tests/mac/feature_matrix.py`; their key timings (title CONTINUE at 1250,
field at ~1700, wild battle by ~2600) come from tests/gameplay schedules and
still need tuning against real screenshots.
