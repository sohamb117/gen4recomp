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
saves/<game>.sav[.bak]   backup chip; written atomically, previous image kept as .bak
screenshots/             F12 captures (PNG, both screens at native size)
options.ini              settings and bindings
```

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

Everything above is rebindable in *Options > Controls* (three keys and one
gamepad button per action; a key or button bound to a new action is removed
from its old one). Fixed shell keys: `1` cycles speed, `F9` touch controls,
`F10` (or Cmd+,) options, `F11` / Alt+Enter fullscreen, `F12` screenshot.
Gamepad Guide or R3 opens the options. The mouse is the stylus on the bottom
screen in every layout and rotation; on touch screens fingers are, and an
on-screen pad (d-pad, A/B/X/Y, L/R, Start/Select, FF, Menu) appears after the
first touch (*Touch controls: Auto/On/Off*).

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
background; going to the background also flushes the save.

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
(`vertical|horizontal|hybrid|top|bottom`), `rotation` (0-3), `swap`, `scale`
(`integer`), `filter` (`linear`), `touch=XxY`, `keys=a+up`, `controls=1`,
`size=WxH`, `page=launcher|options|controls|about`, `storage=1` (saves and an
options round-trip file in the user-data root; refused unless portable mode is
on, so tests never touch a player's data), `import=<path>` (run the
importer), and `script=F:kind:args;...` to push synthetic `key`, mouse
(`down/move/up`) and finger (`fdown/fmove/fup`) events before frame F.
