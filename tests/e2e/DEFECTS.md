# Suspected game/port defects found by the e2e milestones

Each entry: what was seen, a minimal repro, and what is known about the cause. "Suspected" until the cause is
pinned in the port or shown to be the cartridge's own behaviour.

No open entries.

## Fixed: OS_ResetSystem stopped the core (Platinum, Diamond, Pearl)

Every reset the game makes itself ended the run: the player's L+R+START+SELECT, Platinum's NINTENDO WFC SETTINGS
on the main menu (WFCSettings_StartApplication runs the WFC utility, DWC_StartUtility, then always calls
OS_ResetSystem), the error-reset screens and the reset after the credits all reached
`pc_os_lite: OS_ResetSystem: soft reset has no host meaning yet` and `guest trap: abort()`. The cause was the port
itself: games/platinum/pc/src/pc_os_lite.c (D/P link the same file) had no reboot. It now does what the console
does: the runtime (np_host_reset, core/runtime/np_core.c np_rt_reboot) stores the backup chip if it is dirty, keeps
it, discards the instance (memory, fibers) and boots a fresh one from _start within the same frame; the new
instance loads the kept chip, and the carry puts back the reset parameter word (RESET_ERROR survives) and the RTC.
Options and the link are the host's and stay. The reset is counted in the `resets` status (NP_STAT_RESETS) and
logged (`pc_os_lite: OS_ResetSystem: soft reset (parameter N)`, `np_core: soft reset N`); bots.wait_reset waits for
the counter, so 56 (Platinum) and 59b (D/P) end on it with the core running and judge the save the game wrote.
Regressions: tests/gameplay scenarios 9-soft-reset and 10-wfc-settings (Platinum), dp 7-soft-reset; core/tests
test_soft_reset.

## Not a defect: D/P's GTS connect screen "waits with no time-out"

The entry said the GTS trade path's "Connecting to Nintendo Wi-Fi Connection..." stayed up for 1800+ frames. That
does not reproduce: after YES on "Save this Nintendo DS system's Nintendo Wi-Fi Connection User Information to this
Game Card and connect?" the connecting screen lasts about 250 frames with no input, then D/P's own recompiled DWC
shows "No access point in range. Please try again when closer to an access point. ... (50099)" and waits for a
button; B gives "Reconnect to Nintendo WFC?", NO returns to the field (the 97 milestone's own contact sheet shows
the 50099 box in its "wfc-connecting" shot). The search runs against the port's WM model (pc_wm.c), which reports
no parent and no access point. 50099, not Platinum's 51099, is the Auto Connect library's own choice: overlay 4
ov04_021ECCEC returns -50099 when the search found nothing and -51099 when it found a configured access point it
could not use (the flag ov04_021EC2C4 sets). Regression: tests/gameplay dp scenario 8-gts-offline.

## Fixed: Platinum connecting to Nintendo WFC trapped the core

Answering YES on the Global Terminal's Nintendo WFC screen trapped the core: `wasm trap: Unreachable`. The cause was
NintendoWFC_ConnectToDWCServer, which called pc/stubs.list trap stubs generated as `void f(void)` with arguments:
CPS_SetSslHandshakePriority, DWC_Auth_SetCustomNas and DWC_AC_Create. wasm-ld routes such calls to a silent
signature-mismatch thunk. games/platinum/pc/src/pc_dwc_connect.c and pc_dwc_auth.c now give those functions, and the
rest of the Auto Connect layer, their header signatures, modelling a console that finds no access point. The game
then shows its own "No compatible access point in range ... Error code: 51099" and "Reconnect to Nintendo WFC?";
NO returns to the field. The regression test is tests/gameplay scenario 8-wfc-offline.

An audit of the remaining signature-mismatch thunks (64; listed by pc/wasm/check_module.py) found that every other
caller sits behind a successful connection: NAS login, SVL, ND download, SOCL sockets, VCT voice chat, the PPW lobby.
With the connection failing, none of them is reachable offline.
