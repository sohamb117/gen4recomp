# nativeplat shell

The player-facing SDL3 app: launcher, ROM import, game view, options and
touch controls. It drives a game core through `core/include/np_core.h` and
knows nothing about wasm.

## Build

```sh
cmake -S shell -B build/shell -G Ninja -DNP_CORE=stub   # or -DNP_CORE=real
cmake --build build/shell
ctest --test-dir build/shell --output-on-failure
```

- `NP_CORE=stub` links `core/stub/np_core_stub.c`, a test-pattern core
  (title from the cartridge header, frame counter, keypad and stylus state,
  sine tone, save round-trip).
- `NP_CORE=real` adds `core/` and links `np_runtime` plus every
  `np_guest_<game>` it defines via `np_link_guest_modules()`.
- macOS builds `nativeplat.app`; `-DCMAKE_SYSTEM_NAME=iOS` with the Xcode
  generator uses `platform/ios/Info.plist.in` (needs an iOS build of SDL3).

## Importing a cartridge

Drop a `.nds` on the window (or the Dock icon), or use *Import ROM*. The file
is identified by SHA-1:

| Game | Accepted dump (SHA-1) |
| --- | --- |
| Diamond (USA) | `a46233d8b79a69ea87aa295a0efad5237d02841e` |
| Pearl (USA) | `99083bf15ec7c6b81b4ba241ee10abd9e80999ac` |
| Platinum (USA, Rev 1) | `0862ec35b24de5c7e2dcb88c9eea0873110d755c` |

Platinum Rev 0 (`ce81046e…`) is recognised and refused with a pointer to
Rev 1; anything else is refused with its computed hash. Accepted files are
copied (temp file + rename) into the user data folder.

## User data

SDL's per-user pref folder (`SDL_GetPrefPath("nativeplat", "nativeplat")`),
or, in portable mode, `userdata/` beside the executable when a
`portable.txt` sits next to it (for a macOS bundle: next to `nativeplat.app`).

```
roms/<game>.nds          imported cartridge
saves/<game>/<slot>.sav[.bak]  save slots; written atomically, previous image kept as .bak
screenshots/             F12 captures (PNG, both screens at native size)
options.ini              settings and bindings
```

## Save slots

Each game has any number of named save slots (raw 512 KiB flash images, the
format melonDS and DeSmuME's "raw .sav" use). Picking a game on the launcher
opens its slots: *Continue* (last used), *New save slot*, each slot (Play,
Rename, Duplicate, Export .sav, Delete with confirmation) and *Import .sav*
(also by dropping a `.sav`/`.dsv` on that page). Imports must be exactly
512 KiB, or a DeSmuME `.dsv` (512 KiB + its 122-byte footer, which is
stripped). Names: up to 32 letters, digits, spaces and `- _ ( ) . ! ' #`,
compared case-insensitively; Windows device names are refused. Names are typed
or picked on an on-screen keyboard (gamepad/mouse/touch).

## Launching

```sh
nativeplat --game platinum --slot "My run"   # or --slot 2 (2nd slot as listed)
nativeplat --launcher                         # ignore "On startup: Continue"
open 'nativeplat://launch?game=platinum&slot=My%20run'
```

`--game` alone continues that game's last slot. The `nativeplat:` URL scheme is
registered in both Info.plists; SDL delivers opened URLs as
`SDL_EVENT_DROP_FILE` on macOS and iOS. On Windows the scheme is not
registered (that needs an installer writing the registry), so use the flags.
Unknown games, missing slots, games whose core is not in the build, or games
not yet imported all land on the launcher with a message.

```sh
nativeplat --editor --save ~/Downloads/backup.sav [--game pearl]
```

opens the save editor on any save file, without a slot or a game core, and
quits when the editor closes. Platinum saves identify themselves; Diamond and
Pearl share a format, so `--game` (or whichever of the two is imported)
decides whose ROM supplies the names. Saving keeps the previous file as
`<file>.bak` next to it.

*Options > Real-time clock* feeds the device's local time to the game's RTC
(default), or the port's fixed clock (2009-03-22 10:00, advancing with frames);
it applies from the next boot.

## Save editor

*Edit save...* on a slot opens an editor for that slot's backup image
(features/save4), with names and game tables read from the imported ROM
(features/ndsdata). Tabs: **Trainer** (name, gender, IDs, money, coins,
badges, play time), **Party** (species, nickname, level/EXP, ability, held
item, moves, IVs, EVs, friendship; nature, shininess, PID and OT shown
read-only), **Boxes** (18 x 30 grid: edit, move/swap, release), **Bag**
(change item, quantity, remove, add) and **Pokedex** (seen/caught per species,
mark all, clear). Level and EXP move together, party stats are recomputed
with the game's formula, a new move gets full PP and EVs are capped at 510.

Every edit can be undone (16 steps; X / Ctrl+Z, redo Y / Ctrl+Shift+Z).
*Save* (Ctrl+S) writes the slot atomically and keeps the previous image as
`.bak`. A slot whose blocks fail their checksums is refused with the reason.
L/R or Page Up/Down switch tabs; numbers take typed digits or per-digit +/-;
lists filter as you type. Limitations: no party/box transfers, item pockets
are not checked against item data, alternate forms use the base species'
stats.

**Events** (Platinum saves) turns on the MYSTERY GIFT main-menu option and
the Pokedex-obtained flag it also needs, lists the three Wonder Cards
(remove one together with its pending gift) and adds Wonder Cards we write
ourselves for the Member Card (Darkrai), Oak's Letter (Shaymin), Azure Flute
(Arceus) and Secret Key (Rotom) events: the deliveryman in any Poke Mart then
hands over the item. *Import .pgt / .pcd...* (or dropping such a file on the
editor) adds any gift file you own. No event files ship with nativeplat.

*Export Trainer Card PNG...* and *Export Pokedex diploma PNG...* at the end
of the Trainer tab write a 768x576 image of the open save (name, ID, money,
Pokedex counts, play time, badges, party; the diploma shows the caught count
out of 493 and today's date). Both layouts are nativeplat's own, drawn with
the shell's bitmap font (`src/card.c`, no game graphics).

## Display effects and performance

*Battle layout* switches to another screen layout for the length of each
battle (the core's `NP_STAT_IN_BATTLE`: from the intro effect to the fade
back to the field) and back afterwards. It is shown unswapped, so *Hybrid
(large top)* gives the battle scene the large screen with the touch menu
beside it. *Same as screen layout* turns it off.

*Options > Effect 1 / Effect 2* chain two effects, each with an intensity:
**LCD grid** (gaps between DS pixels), **Scanlines**, **CRT** (scanlines,
aperture-grille mask, vignette and, with *CRT curvature*, a barrel-bent
picture) and **Smooth** (Scale2x on the CPU, then linear sampling). They are
drawn with plain SDL_Renderer geometry and tiny repeating pattern textures, no
shaders, so they look the same on Metal (macOS/iOS), Direct3D/Vulkan
(Windows) and the software renderer, and follow every layout, rotation and
scale. Patterns are skipped below 2 window pixels per DS pixel.

*Performance* only changes presentation: **Custom** uses the VSync, FPS cap
and effect options as set; **High** = effects, VSync, no cap; **Balanced** =
no curvature, VSync, 60 FPS cap; **Low** = no effects, VSync off, 30 FPS cap;
**Auto** = High, dropping to Low while producing a frame takes over 12 ms
(averaged over 120 frames) and returning under 5 ms.

*UI scale* fixes the menus' text scale (1x-6x; *Auto* follows the window,
capped so a page always fits) and *Reduce motion* stops the UI's only
animation, the blinking text cursor.

## Default controls

| DS | Keyboard | Gamepad (by position) |
| --- | --- | --- |
| D-pad | Arrows, W A S D | D-pad, left stick |
| A | Z, Enter, Space | East |
| B | X, Backspace | South |
| X | C | North |
| Y | V | West |
| L / R | Q / E | LB / RB |
| Start | Escape | Start |
| Select | Tab, Left/Right Shift | Back |
| Fast-forward (hold) | F | RT |
| Fast-forward (toggle) | G | LT |
| Rewind (hold) | R | L3 |

Everything above is rebindable in *Options > Controls* (three keys and one
gamepad button per action; a key or button bound to a new action is removed
from its old one). Fixed shell keys: `1` cycles speed, `F9` touch controls,
`F10` (or Cmd+,) options, `F11` / Alt+Enter fullscreen, `F12` screenshot.
Gamepad Guide or R3 opens the options. The mouse is the stylus on the bottom
screen in every layout and rotation; on touch screens fingers are, and an
on-screen pad (d-pad, A/B/X/Y, L/R, Start/Select, FF, Menu) appears after the
first touch (*Touch controls: Auto/On/Off*).

*Edit touch controls...* rearranges them for the window's current
orientation (landscape and portrait are separate): drag a control to move
it, drag the square on its corner to resize it, and use the toolbar (or Tab
/ Shift+Tab, arrows, `-` `=` for size, `[` `]` for opacity; on a gamepad
L/R select, the d-pad moves, X/Y resize, A cycles opacity) to fade it or
*Reset* the orientation to the built-in arrangement. The outlines show where
the DS screens are. Edited layouts are saved to `touch-controls.ini`
(centre as a fraction of the window, size as a fraction of its short side,
so they follow resizing); unedited ones keep the built-in arrangement.
*Rumble on press* gives a short gamepad rumble for every touch-control or
gamepad press, the desktop stand-in for a phone's haptic tick.

*Controller skin* uses Delta's DS skins (`.deltaskin`: a zip with
`info.json` whose `gameTypeIdentifier` is `com.rileytestut.delta.game.ds`):
Enter imports one (or drop it on the window), Left/Right switch between
installed skins and *None*. Import keeps `info.json` and the art it names in
`skins/<name>/`. For each orientation the first representation of
iphone/edgeToEdge, iphone/standard, ipad/standard, ipad/splitView is used:
its art is fitted to the window, the DS screens go to its `screens`
(`inputFrame` picks top or bottom, `outputFrame` is where; older skins'
`gameScreenFrame` is split in two), and touches on its `items` press
buttons, the d-pad (8-way from its centre), `menu`, `fastForward`,
`toggleFastForward`, `quickSave` (F1) and `quickLoad` (F2), with
`extendedEdges` growing the touch areas. The touch screen stays the
stylus. PNG art loads anywhere; PDF art (`resizable`) is rasterized with
CoreGraphics on macOS and iOS for the window's size, and refused on Windows.
A skin replaces the built-in touch controls whenever it covers the
orientation.

## Saving, snapshots and rewind

| Key | Action |
| --- | --- |
| F1 | Quick save: the game's own save, made without opening its menu (the core waits up to a second for the player to be free; "Can't save right now" otherwise) |
| F2, F2 | Quick load: press twice within 3 s to reboot the slot from its last save |
| F5 / F7 | Take / restore an in-memory snapshot in the current slot |
| F6 | Next snapshot slot (4) |
| R (hold) | Rewind |
| `-` / `=` | Camera farther / closer |
| `3` / `4` | Camera tilt down / toward the horizon (5 degrees) |
| `0` | Original camera |

Snapshots and rewind use the core's in-session states (`np_core_state_*`):
the whole machine, held in memory only and dropped when the game closes; the
cartridge save remains the persistent state. Rewind records every sixth
frame and, while held, steps back at 3x through *Rewind history* (Off / 10 /
30 / 60 s, 8 MB per second of budget, the newest snapshot in full and older
ones as run-length-coded XOR deltas, `src/rewind.c`). Quick load, snapshots
and rewind are refused during a wireless session.

The *Options* game rows are applied live by the core: music and sound-effect
volume, 3D render scale (1x-4x), widescreen 3D (both screens widen; the DS
picture stays centred and the stylus maps to it), camera zoom and tilt,
instant text, and *Fix cartridge bugs* (opt-in fixes of documented bugs).

## Timing, speed and audio

The guest runs at the DS refresh rate, 59.8261 Hz, from a wall-clock
accumulator independent of the display (VSync on/off, optional FPS cap). The
*Logic clock: Exact 60 Hz* option runs it at 60 Hz instead, so a 60 Hz
display shows every frame once. Speeds 2x, 3x, 4x, 8x run several guest
frames per presented frame; *Uncapped* runs frames for 12 ms of each iterate.

Audio (32728 Hz stereo from the core) goes through an SDL audio stream that
resamples to the device; a +-0.5% feedback on the stream ratio keeps latency
near 60 ms. While fast-forwarding, audio is time-stretched by dropping: each
chunk is queued only while the device queue is short, the rest is discarded,
so you hear normal-pitch snippets rather than sped-up audio. The game pauses
(and audio stops) while a menu is open, when minimized, and in the
background; going to the background also flushes the save. During a local
wireless session (`NP_STAT_LINK_ACTIVE`) the speed is locked to 1x and the
game keeps running when minimized or in the background, because the partner
drops a station that is silent for 4 s.

*Music filter* (Off / 1X / 2X / 3X, as in Gen1Recomp) runs the output
through 1-3 cascaded one-pole low-passes at 6 kHz (`src/lowpass.c`): -6, -12
or -18 dB per octave above the corner, nothing below it. The core mixes music
and effects into one stream, so both are filtered.

## Local wireless

*Options > Local wireless (LAN)* opens a UDP transport (`src/net.c`) on *LAN
port* (default 2009; the next three ports are tried if it is taken) that
finds other nativeplat instances on the LAN; *Join by IP:port* adds a station
beyond broadcast range. *Wireless status* shows stations in range or the
error. Each install keeps a station id in `options.ini` (`[wireless]
station_id`), generated once: the game derives the console's MAC address
from it and stores that in saves, so a changing id would trigger the game's
"different DS" clock penalty. *Internet relay host:port* and *Room PIN* switch
the transport to a relay server (`server/relay`, `docs/RELAY.md`): players
who enter the same relay and PIN meet as if in range, without port
forwarding.

## Mods

*Options > Mods...* manages runtime content packages for Platinum (the
format in `games/platinum/pc/mods/README.md`: `mod.toml`, `content/`,
`records/` and the cooked `.cooked/`). Packages are plain folders in
`<user data>/mods/`, which the core reads read-only as `/content`
(`np_host.content_root`); the enabled ones and their load order are
`mods/loadorder.txt`, the file the core itself reads, so the folder also
works by hand. *Install package (.zip)...* (or dropping a .zip on the page)
takes a zip holding one cooked package; member names must be plain relative
paths and links are refused, so a package cannot reach outside the folder.
It is extracted under a temporary name and renamed into place. Enter turns a
package on or off, Left/Right move it in the load order, X twice deletes it.
`requires` / `load_after` problems are shown before a boot. Changes apply
when the game boots; if a package stops the boot ("modfs: ..."), the page
opens with the message and that package selected.

*Seal enabled packages as a cart...* saves the enabled packages, in order,
as a named custom cart (`carts/<name>.cart`) with a SHA-256 over each
package's name, `mod.toml` and cooked digest. A slot's menu binds it to a
cart (*Cart:* cycles through them; stored as `saves/<game>/<slot>.cart` and
carried by rename/duplicate/delete); that slot then always boots exactly the
cart's packages (`PC_MODS`, overriding `loadorder.txt`) and refuses to start
if one changed since sealing. Link play is pinned to the active set: each
boot derives a realm from it (the cart's or the loose set's hash; none for
vanilla), and local wireless only meets stations of the same realm (on a LAN
the realm changes the packet magic; through a relay it makes its own room,
`<PIN>~<realm>`), so modded and vanilla games never trade or battle.

## GBA cartridge (Pal Park)

*Options > GBA cartridge (Pal Park)* inserts a Game Boy Advance cartridge
image you own into Platinum's GBA slot; *GBA save* picks its save file
(default: the cartridge's name with `.sav`, as mGBA writes it). Left ejects.
The slot is read when the game boots, so a change applies from the next boot
(F2 twice reloads). The core reads the ROM through `np_host.gba_rom_read`;
the save is loaded padded with 0xFF (an erased chip) and written back
atomically, keeping the previous file as `.sav.bak`, after the game writes
it (Pal Park migration rewrites one Gen 3 save slot). With a National Dex
save the main menu offers *MIGRATE FROM <game>*.

## Folder sync

*Options > Sync folder* picks a folder (for example inside iCloud Drive,
Dropbox or a network share) that mirrors every save slot as
`<folder>/<game>/<slot>.sav`; cartridges are never copied. Slots sync when the
app starts (before a game can boot), when it quits, after every in-game or
editor save, and on *Sync now*; renaming or deleting a slot removes its
synced copy unless another device changed it since. *Sync status* shows the
last result. Left on *Sync folder* turns sync off.

Each slot is compared three ways (`src/sync_plan.c`): this device's file, the
folder's file, and the content both had after the last sync, remembered in
`sync-state.txt` with both files' size and modification time so unchanged
files are not re-read. The side that changed wins. A fresh, never-saved slot
never overwrites a save, and a missing file is never taken as a deletion.
When both sides changed, this device's copy stays the slot, the folder's copy
becomes a new slot `<slot> (conflict <date>)` (synced too, so neither version
is lost anywhere), and a chooser shows both (trainer, play time, badges,
save time) with *Keep this device's*, *Use the other copy*, *Keep both* or
*Decide later*.

## Updates

*Options > Updates...* appears when the build names a GitHub repository
(`-DNP_UPDATE_REPO=owner/name`, or `[updates] repo = owner/name` in
`options.ini`; `api =` overrides `https://api.github.com`, for testing
against a local server). Nothing is sent until the player presses *Check
for updates*: the app then reads the repository's latest release, compares
its tag with its own version (dotted numbers; a `-rc` pre-release sorts
before its release), and offers *Download and verify* only if the release
carries this platform's zip (an asset name containing `macos` or `windows`,
ending `.zip`) and a `sha256sums.txt` in `sha256sum` format. The zip is
written to the Downloads folder as `<name>.part`, hashed while it arrives,
and renamed only if its SHA-256 matches the listed one (otherwise deleted).
*Show in Finder* / *Show in Explorer* reveals it; the app never replaces
itself.

Networking lives behind one function, `np_http_get` (`src/http.h`): a GET
that follows redirects and streams the body to a callback, with a cancel
flag, on a worker thread. The backends use what the OS ships, so no TLS
library is bundled: `src/http_curl.c` links the macOS SDK's libcurl (system
trust store), `src/http_winhttp.c` uses WinHTTP. iOS builds have no backend
and no updater (`NP_HAVE_HTTP` undefined): updates come from the App Store.

## Autotest

`NP_AUTOTEST` skips the launcher, boots a core for a synthetic cartridge
header with an in-memory save (booting twice to prove the save round-trips),
runs N frames through the real renderer, writes a PNG of the window and
exits 0:

```sh
SDL_VIDEO_DRIVER=dummy NP_AUTOTEST="frames=120,png=/tmp/shot.png" \
  build/shell/nativeplat.app/Contents/MacOS/nativeplat
```

Keys (comma separated): `frames`, `png`, `game`, `layout`
(`vertical|horizontal|hybrid|top|bottom`), `battle_layout` (same values or
`off`), `rotation` (0-3), `swap`, `scale`
(`integer`), `filter` (`linear`), `touch=XxY`, `keys=a+up`, `controls=1`,
`size=WxH`, `page=launcher|options|controls|about`, `storage=1` (saves and an
options round-trip file in the user-data root; refused unless portable mode is
on, so tests never touch a player's data), `import=<path>` (run the
importer; repeatable), `rom=<path>` (boot a real cartridge, save in memory),
`boot=app` (start like the real app: options, command-line launch options,
launcher; implies `storage=1`), `press=<schedule>` or `press=@<file>` (DS keys
and stylus taps per frame, below), `shots=N` (also write `<png>-<iteration>.png`
every N iterations), `clock=real` (device RTC; the default is the port's fixed
clock so runs repeat exactly), `slot=<name>` (save slot used with `storage=1`),
and `script=F:kind:args;...` to push synthetic `key`, `text`, mouse
(`down/move/up`), finger (`fdown/fmove/fup`), `drop:<path or URL>` and
`dialog:<path>` (answer the open file dialog) events before frame F.
`lan=<port>` turns local wireless on, `peer=<host:port>` joins a station and
`station=<hex>` sets the station id (autotests otherwise use a fixed id).
`rewind=F+N` holds rewind for N iterations from iteration F; `render_scale`,
`widescreen`, `zoom`, `tilt`, `instant_text`, `fix_bugs` and
`rewind_seconds` set the game options. `sync=<folder>` sets the sync folder
(only with portable storage). `gba=<rom>` and `gbasave=<sav>` fill the GBA
slot; `page=mods` captures the mod manager. `music_filter=N` (the output
the autotest measures is filtered; it reports `audio_treble`, the RMS of
sample-to-sample steps), `ui_scale=N` and `reduce_motion=1` set those
options. With `boot=app` the process's own arguments apply, e.g.
`--editor --save <file>`.

A press schedule is steps separated by `;` or newlines (`#` comments in files):
`F:keys[:N[:R:C]]` holds keys (`a`, `start`, `a+up`, ..., or `none`) for N
frames (default 6) from frame F, repeated every R frames C times;
`F:tap:X:Y[:N[:R:C]]` touches the bottom screen. `+D` instead of F means D
frames after the previous step.

Real Platinum title screen:
`NP_AUTOTEST="frames=1500,png=/tmp/t.png,rom=/path/pokeplatinum.us.nds"`.

`tests/platinum_first_save.press` plays a new game to the first in-game save
(player NATIVE, rival BARRY). CTest `shell_platinum_first_save` (real-core
builds) runs it on portable storage, checks the slot file, reboots to the
CONTINUE menu and parses the save with `np_save4`; it skips without the ROM.
