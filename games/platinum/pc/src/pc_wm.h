/*
 * The ARM7 wireless manager (WM) modeled behind PXI tag 10.
 *
 * The game keeps the stock NitroSDK ARM9 WM library; this is the other half,
 * the WMSP request/indication endpoint the ARM7 firmware runs, with the radio
 * replaced by datagrams through the host's np_host_net_* imports (contract
 * v2). pc_wm.c has the full description; these are the hooks the rest of the
 * port calls.
 */
#ifndef PC_WM_H
#define PC_WM_H

/* Registers the tag-10 responder and writes the station's firmware MAC and
 * allowed-channel word. Called once from pc_main.c, before NitroMain. */
int pc_wm_init(void);

/* The per-VBlank pump, called from OS_Halt before the VBlank is delivered:
 * runs queued WM requests, timers, the network and every resulting ARM9
 * callback, in the order a PXI interrupt would have delivered them. */
void pc_wm_step(void);

#endif /* PC_WM_H */
