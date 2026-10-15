/*
 * The wifi-account surface that boot actually touches, modeled as an
 * unconfigured console.
 *
 * The blanket assumption behind the trap stubs, "wifi code is dead
 * code", is false for a handful of libdwcauth/BACKUPl entries: the
 * game's own init path (NitroMain's wifi-ID check) calls into the
 * compiled NitroDWC account C, which consults the auth library for a
 * created wifi ID and the wifi-settings NVRAM. The honest state of this
 * machine is a console that never configured wifi: no ID exists, nothing
 * needs creating, the settings flash reads as erased. Every function
 * here says exactly that through the interface's own vocabulary, and the
 * compiled account code takes its normal "no configuration" branches.
 *
 * Signatures come from bm/util_wifiidtool.h, auth/dwc_auth.h and the
 * extern declarations in the account library's own dwc_init.c (BACKUPl
 * has no public header).
 */
#include <nitro.h>
#include <auth/dwc_auth.h>
#include <bm/util_wifiidtool.h>

#include <string.h>

void DWC_Auth_GetId(DWCAuthWiFiId *id)
{
    /* flg == 0 is the library's own "no wifi ID has been created". */
    memset(id, 0, sizeof *id);
}

BOOL DWC_Auth_CheckPseudoWiFiID(void)
{
    return FALSE;
}

BOOL DWC_Auth_CheckWiFiIDNeedCreate(void)
{
    /* Creating one would need the auth servers; an unconfigured console
     * that stays unconfigured is the state this port can be truthful
     * about. */
    return FALSE;
}

int DWC_BM_Init(void *work)
{
    /* The backup-manager init ahead of the ID read; its work area is the
     * caller's. Zero is the library's success, and the account code goes
     * on to ask the questions the functions below answer. */
    (void)work;
    return 0;
}

BOOL DWCi_BACKUPlInit(void *work)
{
    (void)work;
    return TRUE;
}

BOOL DWCi_BACKUPlRead(void *mem)
{
    /* The wifi-settings NVRAM of a console that never wrote any: erased
     * flash. The account code's checksum validation fails over 0xFF and
     * takes its fresh-settings path. The read size is the caller's
     * s_work area; 256 bytes covers the settings page the account code
     * reads through this interface. */
    memset(mem, 0xFF, 256);
    return TRUE;
}

BOOL DWCi_BACKUPlWritePage(const void *data, const BOOL *page, void *work)
{
    /* Accepted and dropped: nothing on this machine persists wifi
     * settings, and nothing reads them back except through the erased
     * model above. */
    (void)data;
    (void)page;
    (void)work;
    return TRUE;
}
void DWC_Auth_SetCustomNas(const char *nasaddr)
{
    /* DWC_SetAuthServer names the NAS before every connection attempt.
     * Remembering it would serve nothing: the connection fails at the
     * access-point search (pc_dwc_ac.c) before any server is contacted. */
    (void)nasaddr;
}
