# Suspected game/port defects found by the e2e milestones

Each entry: what was seen, a minimal repro, and what is known about the cause. "Suspected" until the cause is
pinned in the port or shown to be the cartridge's own behaviour.

## D/P: the GTS connect screen waits with no time-out (observation, not blocking)

Seen: the GTS receptionist's trade path opens "Connecting to Nintendo Wi-Fi Connection... Please wait a moment..."
and nothing changes for 1800+ frames; B cancels it ("Reconnect to Nintendo WFC?"), NO returns to the field.
D/P is not affected by the Platinum Wi-Fi trap (fixed, see below): it links no DWC trap stubs and runs its own
recompiled NitroDWC, Auto Connect and WCM code against the port's WM model (games/platinum/pc/src/pc_wm.c). That
search never finishes here. Platinum's Auto Connect is a model (games/platinum/pc/src/pc_dwc_connect.c) that ends
the search with error 51099 ("No compatible access point in range"). Whether a real unconfigured DS shows 51099 here
after its scan instead of waiting is [INFERENCE: likely, unverified]; if so, the WM model's scan path is the
suspect. diamond/97-gts-offline uses B + NO.

## Platinum: Nintendo WFC SETTINGS on the main menu ends in a guest abort (port gap)

Seen: CONTINUE save, title, main menu, DOWN, DOWN, A on NINTENDO WFC SETTINGS: frame ~1700
`pc_os_lite: OS_ResetSystem: soft reset has no host meaning yet` and `guest trap: abort()`.
Cause: WFCSettings_StartApplication runs the prebuilt Nintendo WFC utility (DWC_StartUtility), then always calls
OS_ResetSystem. The utility is not in this build, so DWC_StartUtility (pc_dwc_connect.c) returns at once, as when the
player leaves without saving. The abort comes from the reset that follows: the port has no soft reset back to the
title screen yet (pc_os_lite). Before the Wi-Fi fix below, the same entry died earlier on a silent
signature-mismatch trap.

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
