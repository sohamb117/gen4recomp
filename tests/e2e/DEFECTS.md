# Suspected game/port defects found by the e2e milestones

Each entry: what was seen, a minimal repro, and what is known about the cause. "Suspected" until the cause is
pinned in the port or shown to be the cartridge's own behaviour.

## Fixed: Black/White's Musical show never opened its curtain (port: the ARM7's VBlank count)

Black and White, tests/e2e/black/75-musical: the dress-up and the backstage scene (zone 78, "I guess everyone is
ready. Let's go up on stage!") play. Then the show app (map_id 0 from frame 8007 on Black, 8107 on White) shows the
closed red curtain with the Musical logo over the audience forever. The audience animates, and no key or tap does
anything. There is no DEFECT or trap line. Repro: `NP_CORE_BUILD=$PWD/build/core-bwm tools/heavy.sh --run python3
tests/e2e/run.py --game black --systems --only 75-musical --planned` (from 15's end save plus the milestone's recipe).
Step 12 (advance_text) fails with "text did not end in 12000 frames".

Cause: the show's main function, ov132_021F879C, was stuck in state 6, waiting for its show scripts to end. It was
found with macOS `sample` on np_gp at the hang, and lldb read its work pointer at the function's entry. The scripts
(ov132_021FAC74, run by the script VM sub_02011298) are stepped by ov132_021FABD8 once per elapsed frame. The elapsed
count is how much `OS_GetVBlankCount()` changed since the last frame: work+0x1E8 = count - work+0x1EC. That count is
the word at 0x02FFFC3C, TWL-SDK's HW_VBLANK_COUNT_BUF. On a console, the ARM7's VBlank interrupt (NitroSDK
os_irqTable.c, OSi_IrqVBlank) increments it. The port models the ARM7 in host code, and nothing there incremented
it. It read 0 at the hang, so the scripts never took a step.

Fix: OS_Halt in pc_os_lite.c now increments HW_VBLANK_COUNT_BUF on each delivered VBlank, before the ARM9's handler
runs. The address comes from the SDK header each build compiles against: 0x027FFC3C on D/P/Pt and HG/SS, 0x02FFFC3C
on Black/White. So the counter now counts in every game.

On the fixed core (build/core-bwm2, bw-script), the curtain is open by frame 8903. Darmanitan and three other
performers dance on the lit stage until frame 13709. Then the run goes back to the backstage (78) and the theater
(77). 75-musical passes on both games: Black in 14923 frames, White in 15055.

## Fixed: Black/White stopped in N's Castle's throne room and in the ending on a VRAM bank overlap (armrec)

Black and White, zone 278 (the throne room): the scene where N calls his dragon and the stone answers (scr 0556
script 3, zone_event 278 trigger 1 on 0x40B6 == 1) aborted the core every time, Black at frame 7618 and White at 7760
of the repro: `armrec: VRAM banks D and I are both mapped at 0x06600000 ... VRAMCNT = 83 8B 80 84 83 81 82 80 82,
window sub OBJ block 0`, then `guest trap: abort()`. The same trap fired about 14.6k frames after Ghetsis's win, in N's
farewell before the credits. Repro: /tmp/League2833/t277.sav (a synthesized save at zone 277: 8 badges, 0x40A1/A2/A5/
A6/D4/D5/DC set, the stone), walk into 278, trigger 0's scene, then trigger 1.

Cause: armrec's VRAM model refused two banks in one window block ("this port aliases each window onto one bank's
storage"), on the claim that the SDK never makes that state. It does: GxSetBankForSubOBJ (NitroSDK gx_vramcnt.c, and
every role's setter has the same shape) stores the new bank first, then moves the old one to LCDC, so between two
register stores D (128 KB) and I (16 KB) are both sub OBJ. The decompiled C ports never saw it (their VRAM hook runs
at the setter's return); the recompiled ROM's per-store hook (ARMREC_VRAM_HOOK) does. Fixed in armrec_rt.c by modelling
the console: overlapping banks are legal, reads OR them and a write reaches each of them. The copying VRAM model
(wasm and Windows, every shipped core) shows the OR in the window and writes the bytes the guest changed back to
every bank; the native aliasing model fences such a block and stops only if a frame renders while the overlap stands.
On the fixed core (build/core-bwm, bw-script) the repro plays the whole scene (N, then Reshiram and Zekrom in the
room) and saves at frame 16932.

## Fixed: HeartGold froze at a trainer's sight encounter; scripted NPCs drew as a white block (port)

HeartGold 04 (tests/e2e/heartgold/04-route30-31-violet) stops at the first route trainer. Repro: start from 03's end
save (Route 29 west of New Bark), CONTINUE, `walk_to (553,292)` north up Route 30 (wild battles fled): Youngster
Joey (Route 30 object 4 at (553,332), std_trainer TRAINER_YOUNGSTER_JOEY) spots the player at (552,332) around
frame 8759 and the field never moves again: no battle starts, `field_ready` stays 0, A/B do nothing. The top screen
shows the player and Joey as one white/blue block with only the player's shadow drawn. The same block, without
the freeze, stands where the player and a scripted NPC are during the Cherrygrove guide-gent tour (02) and the
Cherrygrove rival scene (03); their milestones pass on their saves, their pictures show the block. Core: hgss-play2's
22:23 build of main c2f26070e. Lead from the port side: before 165d72148 the player looked exactly like this when
sprite textures were never uploaded (VBlank-queue tasks failing to be created): look for GF_AssertFail from
SysTask_CreateOnVBlankQueue or a full 32-entry VBlank queue during the encounter (the emote and the approach
loading textures in one long frame), else another unprototyped callback (unk_02037C94.c sub_020381C0, :1170;
custom_safari_zone.c). Every route trainer waits on it.

Fixed on the port side by hgss-play3 in ced1934b8: the trainer approach's sub_02064598 read its task from an r0 the
C never passed (five more such C-to-assembly calls fixed with it). HeartGold 04 now passes through Joey and the
route trainers after him, and the 02/03 sheets no longer show the block.

## Fixed: HeartGold's first battle aborted in the 3D command FIFO (port)

The first wild battle (Route 29) stopped as it began: `pc-gpu3d: command 0x10 arrived while 0x16 still wanted 3 of
its 16 parameters`, then `abort()`. G3_LoadMtx44 -> GX_SendFifo64B (gxasm.s) was recompiled without the geometry
hook, so its stmia to the FIFO went to plain memory; GX_SendFifo64B is a host override since c2f26070e. Wild
battles, the rival (03) and the catching tutorial now run.

## Fixed: HeartGold's first field load hung (port)

The intro reached the first map load at frame 16169 and no frame completed after it: the field init (fieldmap.c
ov01_021E662C) called CARD_SpiWaitGetStatus twice and the second OS_LockCard spun forever, because armrec emitted
`bl _ll_udiv` as a goto into msl.s's helper body, whose `bx lr` returned before OS_UnLockCard; the same check
then needed the cartridge's IR chip on AUXSPI (command 0x08 -> 0xAA). Fixed by armrec fe357c02a (a `.type NAME,
@function` label starts a function), the IR chip model 53b112d0c, the timer/tick, geometry-port and card-timing
fixes up to 165d72148 and the prop-animation callback 9d997a419: HeartGold 01 now passes from a blank chip.

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

## Fixed: the Platinum e2e probe put the Union Room into a communication error

The same class on Platinum. A Union Room station run with the probe on (PC_E2E=1: every np_gp / e2e bot run)
showed "A communication error has occurred" about 180 frames after entering the room (map 466), alone or with a
partner; without the probe, with PC_E2E=0 or with another variable in the environment, it stayed in the room. Repro:
a station minted from tests/link/recipes/union-b.recipe on tests/link/schedules/trade-b.sched, np_headless
--lockstep beside an idle station, 6000 frames: `link_active 1 -> 0` at 5657 with the probe, none without. The probe's
tile grid (games/platinum/pc/src/pc_np_field.c e2e_tile) asked TerrainCollisionManager about every tile of its 64x64
window; for a tile past the map matrix the land-data provider
(LandDataManager_GetRelativeLoadedMapsQuadrantOfTile, overlay005/land_data.c) raises
CommManager_SetCommError(COMM_ERROR_RESET_SAVEPOINT) while the comm layer is up (and GF_ASSERTs otherwise). The
probe (pc_np_field.c e2e_on_matrix) now asks only for tiles on the matrix, as D/P's does; the guest's frames without
the probe are unchanged.

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
