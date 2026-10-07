# Suspected game/port defects found by the e2e milestones

Each entry: what was seen, a minimal repro, and what is known about the cause. "Suspected" until the cause is
pinned in the port or shown to be the cartridge's own behaviour.

## D/P: the GTS connect screen waits with no time-out (observation, not blocking)

Seen: the GTS receptionist's trade path opens "Connecting to Nintendo Wi-Fi Connection... Please wait a moment..."
and nothing changes for 1800+ frames; B cancels it ("Reconnect to Nintendo WFC?"), NO returns to the field.
The port models an unconfigured console (games/platinum/pc/src/pc_dwc_auth.c). Whether a real unconfigured DS
shows an error code here instead of waiting is [INFERENCE: likely, unverified]. diamond/97-gts-offline uses B + NO.

## Platinum: connecting to Nintendo WFC traps the core (port defect, not fixed)

Seen: Global Terminal 2F Box Data machine (platinum/103), USE, save YES, overwrite YES, then YES on the Nintendo
WFC screen ("Save this Nintendo DS system's Nintendo Wi-Fi Connection User Information to this DS Card and
connect?"): about 500 frames later `wasm trap: Unreachable instruction executed` (np_gp `DEFECT trap`).
Repro: platinum/103's start.recipe, the milestone's steps to `wfc-prompt`, then A instead of DOWN, A.
Cause: a backtrace from np_rt_wasm_trap (scratch np_gp with backtrace_symbols_fd) reads
`w2c_platinum_signature_mismatch:CPS_Resolve <- NintendoWFC_ConnectToDWCServer <- CommTask_ConnectingWifiBattle
<- CommManager_Update <- CommSys_Update <- NitroMain`. NintendoWFC_ConnectToDWCServer (src/nintendo_wfc/main.c:233-
284) calls DWC_SetAuthServer -> DWC_Auth_SetCustomNas(const char *) and DWC_ConnectInetAsync -> DWC_AC_Create,
both pc/stubs.list `func` entries generated as `void f(void)`: a call with arguments does not reach the loud
pc_trap_unreached body but wasm-ld's signature-mismatch thunk (silent `unreachable`; identical thunks are folded,
hence the CPS_Resolve name). The Auto Connect layer (DWC_AC_*) is not modelled at all. A fix models it as the
unconfigured console pc_dwc_auth.c already describes: SetCustomNas accepted, AC_Create TRUE, AC_Process/GetStatus
reporting the library's no-access-point failure so DWC_GetInetStatus returns DWC_CONNECTINET_STATE_ERROR and the
game prints its error code and returns. The exact code a real unconfigured DS shows is [INFERENCE: 51099-family,
unverified], which is why it is not fixed here. platinum/103 answers NO (the offline path).
