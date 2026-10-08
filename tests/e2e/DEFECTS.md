# Suspected game/port defects found by the e2e milestones

Each entry: what was seen, a minimal repro, and what is known about the cause. "Suspected" until the cause is
pinned in the port or shown to be the cartridge's own behaviour.

No open entries.

## Fixed: Emerald dropped the player through Granite Cave B1F's floor on arrival (copyvar through NULL)

Arriving on Granite Cave B1F by ladder, the first step fell to B2F at once, on plain cave floor. B1F's map scripts
(data/maps/GraniteCave_B1F/scripts.inc) set VAR_ICE_STEP_COUNT on transition with `copyvar VAR_ICE_STEP_COUNT, 1`
(data/scripts/cave_hole.inc in both decomps; the ROMs are built without UBFIX) and fall through a hole whenever it
is 0 (CaveHole_CheckFallDownHole). `copyvar` copies `*GetVarPointer(1)`, and GetVarPointer returns NULL for a number
below VARS_START (pokeemerald src/event_data.c:164-172, src/scrcmd.c:368-373; pokeruby the same lines): the console
reads the BIOS there, open bus and never 0; the port's zeroed low memory made the count 0. Filling the BIOS region
with the open-bus word was tried and rejected: other NULL reads (the naming screen's VBlank callback after
FREE_AND_SET_NULL(sNamingScreen)) then followed it as a pointer and crashed the native core. The fix is
games/{emerald,ruby}/pc/patches/scrcmd.c.patch: ScrCmd_copyvar reads a source that is no variable as its number,
as VarGet does and as the UBFIX script (`setvar VAR_ICE_STEP_COUNT, 1`) means. Regression: Emerald milestone 11
(Granite Cave B1F/B2F), tests/dp/regress.sh e-/r-/s- cases unchanged.

## Fixed: choosing a sphere or trap in the D/P Underground reset the game (armrec)

Any sphere or trap chosen from an Underground list (UG menu > SPHERES/TRAPS > an entry) ended in "A communication
error has occurred" and a reset: GF_AssertFail with the link layer up (error_handling.c). sub_0205EC6C was called on
NULL from ov18_0224A0EC: the list's scroll prompts were never made. The UG start menu (ov18_02249684) compares the
chosen entry, a function pointer from its data table (0x02249B51: `.word` of a Thumb function carries bit 0, as on
the ROM and as C's &F does through irbridge), with `ldr rN, =ov18_02249B50` literals, and armrec translated those
as 0x02249B50: nothing matched. The cause was the recompiler: literal_c_value (games/platinum/tools/armrec/armrec.py)
dropped the interworking bit build_data_blobs already set. A bare literal naming a Thumb function now gets bit 0
(51 `ldr =thumbfunc; cmp` sites in D/P); calls through such words still dispatch (armrec_dispatch tries the exact
address, then bit 0 clear). Regressions: the armrec bridge test (asm literal, asm `.word`, C &F of one Thumb
function agree, and a call through the literal works), tests/gameplay dp scenario 9-underground, milestones 91/92.

## Fixed: the D/P e2e probe reset the game in the Underground

The probe's tile grid asked GetMetatileBehavior about tiles past the map matrix; the block-loader provider asserts
on such a block index (ov05_021EF844), harmless on the surface but a comm error and reset in the Underground. The
probe (games/diamond/pc/game/pc_dp_field.c e2e_on_matrix) now asks only for tiles on the matrix.

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
no parent and no access point. 50099 is the Auto Connect library's own choice: overlay 4 ov04_021ECCEC returns
-50099 when the search found nothing and -51099 when it found a configured access point it could not use (the flag
ov04_021EC2C4 sets). Platinum's modelled AC layer (pc_dwc_connect.c) follows the same rule. Regression: tests/gameplay
dp scenario 8-gts-offline.

## Fixed: Platinum connecting to Nintendo WFC trapped the core

Answering YES on the Global Terminal's Nintendo WFC screen trapped the core: `wasm trap: Unreachable`. The cause was
NintendoWFC_ConnectToDWCServer, which called pc/stubs.list trap stubs generated as `void f(void)` with arguments:
CPS_SetSslHandshakePriority, DWC_Auth_SetCustomNas and DWC_AC_Create. wasm-ld routes such calls to a silent
signature-mismatch thunk. games/platinum/pc/src/pc_dwc_connect.c and pc_dwc_auth.c now give those functions, and the
rest of the Auto Connect layer, their header signatures, modelling a console that finds no access point. The game
then shows its own "No access point in range ... Error code: 50099" and "Reconnect to Nintendo WFC?";
NO returns to the field. The regression test is tests/gameplay scenario 8-wfc-offline.

An audit of the remaining signature-mismatch thunks (64; listed by pc/wasm/check_module.py) found that every other
caller sits behind a successful connection: NAS login, SVL, ND download, SOCL sockets, VCT voice chat, the PPW lobby.
With the connection failing, none of them is reachable offline.
