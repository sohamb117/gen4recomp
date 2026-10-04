# Native wired shell

This is a complete copy of `../shell/` taken at the start of the UI work,
including the pre-existing uncommitted mod manager changes. The original
shell is untouched. Shared game cores, save-format libraries, and toolchains
are linked as read-only inputs from the original locations.

The launcher, page frames, options, controls, save slots, confirmations,
text entry, save editor, mod manager, toasts, and exported trainer/diploma
cards use the wired visual language. Game framebuffer colors are unchanged.
`src/theme.h` holds common native colors; `src/ui.c` owns the wire background,
double-line windows, cartridge emblems, and common drawing primitives. The
web counterpart is `../web/src/theme.css`.

```sh
cmake -S shell-wired -B build/shell-wired -G Ninja -DNP_CORE=stub \
  -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/shell-wired
ctest --test-dir build/shell-wired --output-on-failure
```

For the real game build use `NP_CORE=real`, the original
`NP_GUEST_WASM_<game>` paths, and `NP_GUEST_POSTPROCESS` pointing to
`tools/wasm2c_postprocess.py`. See `../design/WIRED.md` for the complete
command. The same copied shell supports the existing Windows/iOS CMake
paths; this iteration is tested on macOS arm64, not Windows/iOS devices.

The app keeps its data in SDL's `nativeplat/nativeplat-wired` preference
folder (or its own portable userdata directory). Its bundle ID is
`org.nativeplat.wired` and it registers `nativeplat-wired:` links. Existing
`nativeplat:` links are still accepted as command input for compatibility,
but this bundle does not register ownership of the original app's scheme.
Import cartridges/saves explicitly to use them in this separate app.

Copy-only build repair: the source copy contained `mods.c` but its copied
CMake target did not list it. This target includes that file so the copied
mod UI links successfully. No corresponding edit was made to `../shell/`.

`README.md` is the original shell feature reference captured with the copy;
use this file for the new app identity and storage location.
