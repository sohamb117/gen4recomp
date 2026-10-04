nativeplat
==========

Native ports of Pokemon Diamond, Pearl and Platinum. The games are built
from the community decompilations and run natively; no emulator.

You need your own cartridge dump. Start nativeplat and drop the .nds file on
the window (or use Import ROM). Accepted dumps (SHA-1):

  Diamond (USA)          a46233d8b79a69ea87aa295a0efad5237d02841e
  Pearl (USA)            99083bf15ec7c6b81b4ba241ee10abd9e80999ac
  Platinum (USA, Rev 1)  0862ec35b24de5c7e2dcb88c9eea0873110d755c

Only the games built into this package can be started; the launcher shows
which ones are available.

User data (imported cartridges, saves, screenshots, options.ini) lives in the
per-user folder:

  Windows  %APPDATA%\nativeplat\nativeplat
  macOS    ~/Library/Application Support/nativeplat

Portable mode: put an empty file named portable.txt next to nativeplat.exe
(on macOS: next to nativeplat.app) and the data goes to userdata/ beside it.

Default controls: arrows or WASD = D-pad, Z/Enter = A, X/Backspace = B,
C = X, V = Y, Q/E = L/R, Escape = Start, Tab = Select, F = fast-forward
(hold), F10 = options (rebind everything there), F11 = fullscreen,
F12 = screenshot. The mouse is the stylus on the bottom screen.

macOS: the app is ad-hoc signed, not notarized. On first launch, right-click
nativeplat.app and choose Open (or allow it under System Settings > Privacy &
Security).

Licenses: LICENSE.txt (GPL-3.0) covers nativeplat and the game code;
THIRD-PARTY.txt lists the bundled libraries and their licenses.
