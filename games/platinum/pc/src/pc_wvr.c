/*
 * The WVR radio, behind PXI tag 15, a device whose whole job here is to
 * answer "done, success".
 *
 * WVR is the ARM7-side wireless VRAM driver: WVR_StartUpAsync sends
 * WVR_PXI_COMMAND_STARTUP over tag 15, the ARM7 boots the radio into
 * VRAM-D, and the reply (command | result byte) fires the caller's
 * callback. This port dropped the send, the log said so on every boot:
 *
 *   pc_pxi: send on fifo tag 15 dropped (first word 0x10000)
 *
 * and the cost was measured on the first completed battle. The game's
 * WirelessDriver_Init sets its status to CONNECTING before the async call
 * and the callback is what moves it anywhere else, so a dropped reply left
 * it CONNECTING forever, and WirelessDriver_Initialized() is
 * `status != DISCONNECTED`, so the whole game believed the radio was up.
 * BattleMain_SetNetworkIconStrength then created the network-strength icon
 * at (240,0) in every battle, over tiles nothing had loaded: the user's
 * "corrupted wifi icon in the top right that doesn't clear".
 *
 * Answering success is the faithful model, not a shortcut: on hardware the
 * radio does start, and the game's own state machine (its callbacks, its
 * later WVR_TerminateAsync, its comm-system checks) runs the same way it
 * runs on a console that never joins a network. pokediamond's port answers
 * tag 15 the same way, for the same measured reason, its game
 * OS_Terminates on any other reply.
 */
#include <nitro/pxi.h>
#include <nitro/wvr/common/wvr_common.h>

extern void pc_pxi_set_responder(int tag, void (*fn)(u32 data));
extern void pc_pxi_reply(int tag, u32 data);

static void wvr_respond(u32 data)
{
    /* STARTUP and TERMINATE both complete instantly and well; the reply
     * carries the command word back with the result in the low byte. */
    pc_pxi_reply(PXI_FIFO_TAG_WVR, (data & 0xffff0000u) | WVR_RESULT_SUCCESS);
}

int pc_wvr_init(void)
{
    pc_pxi_set_responder(PXI_FIFO_TAG_WVR, wvr_respond);
    return 0;
}
