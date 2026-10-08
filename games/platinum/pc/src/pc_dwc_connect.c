/*
 * The prebuilt Wi-Fi pieces the compiled connect path reaches with no
 * network, modelled as the same unconfigured console pc_dwc_auth.c
 * describes: no connection slot is set up and no access point answers.
 *
 * The compiled DWC C (dwc_connectinet.c) drives NitroDWC's Auto Connect
 * layer (libdwcac) and NitroWiFi's CPS SSL thread priority whenever the game
 * connects to Nintendo Wi-Fi Connection (NintendoWFC_ConnectToDWCServer: the
 * Global Terminal machines, the GTS, the Wi-Fi Club, ...); the Nintendo WFC
 * Settings entry starts the prebuilt WFC utility. All of these used to be
 * void(void) trap stubs from pc/stubs.list. The callers pass arguments and
 * read results, so wasm-ld routed every call to a signature-mismatch thunk
 * and the core died on a silent `unreachable` (tests/gameplay scenario
 * 8-wfc-offline is the regression).
 *
 * What the library sees here: DWC_AC_Create accepts the configuration, the
 * search runs for AC_SEARCH_FRAMES DWC_AC_Process calls, then the status is
 * AC_NO_ACCESS_POINT. dwc_connectinet.c treats a status below -10 as a
 * connection failure: it first tries the Nintendo Spot (WDS) beacon, which
 * this console never hears (DWC_WDS_STATE_FAILED), then sets
 * DWC_ERROR_AC_ANY with the AC code. The game's NintendoWFC_HandleError takes
 * that as DWC_ETYPE_DISCONNECT, cleans up (DWC_CleanupInet -> DWC_AC_Destroy)
 * and shows its own error message with the code, as a DS without Wi-Fi
 * settings does.
 *
 * Signatures are NitroDWC's include/ac/dwc_ac.h and util/dwc_utility.h and
 * NitroWiFi's include/nitroWiFi/cps.h.
 */
#include <nitro.h>
#include <ac/dwc_ac.h>
#include <nitroWiFi/cps.h>
#include <util/dwc_utility.h>

/* What the Auto Connect library itself reports when the search finds
 * nothing: -50099 ("No access point in range"). -51099 is only for a
 * configured access point that was found but could not be used (D/P's
 * recompiled library, overlay 4 ov04_021ECCEC). The AC layer reports its
 * errors negated, and the game prints the code. */
#define AC_NO_ACCESS_POINT (-50099)

/* About two seconds of searching before giving up. A real console's scan
 * length is not modelled; this only keeps the "Connecting..." screen on
 * screen long enough to read. */
#define AC_SEARCH_FRAMES 120

static BOOL sCreated;
static int sSearchFrames;

static int ac_status(void)
{
    if (!sCreated) {
        return DWC_AC_STATE_NULL;
    }
    return sSearchFrames < AC_SEARCH_FRAMES ? DWC_AC_STATE_SEARCH : AC_NO_ACCESS_POINT;
}

BOOL DWC_AC_Create(DWCACConfig *config)
{
    /* Nothing is allocated through config->alloc: the search below needs
     * no work memory. */
    (void)config;
    sCreated = TRUE;
    sSearchFrames = 0;
    return TRUE;
}

int DWC_AC_Process(void)
{
    /* Zero while the search is still running: dwc_connectinet.c stores
     * this as ac_state and only asks DWC_AC_GetStatus for the outcome once
     * it is non-zero, after which it stops calling Process. */
    if (!sCreated || sSearchFrames < AC_SEARCH_FRAMES) {
        if (sCreated) {
            sSearchFrames++;
        }
        return 0;
    }
    return ac_status();
}

int DWC_AC_GetStatus(void)
{
    return ac_status();
}

u8 DWC_AC_GetApType(void)
{
    return DWC_AC_AP_TYPE_FALSE;
}

BOOL DWC_AC_GetApSpotInfo(u8 *apSpotInfo)
{
    (void)apSpotInfo;
    return FALSE;
}

BOOL DWC_AC_Destroy(void)
{
    sCreated = FALSE;
    sSearchFrames = 0;
    return TRUE;
}

void DWC_AC_SetSpecifyApEx(const void *ssid, const void *wep, int wepMode, const char *apSpotInfo, int overrideType)
{
    /* Only reached after a WDS success, which never happens here. */
    (void)ssid;
    (void)wep;
    (void)wepMode;
    (void)apSpotInfo;
    (void)overrideType;
}

BOOL DWC_AC_CheckWiFiStation(const void *ssid, u16 len)
{
    (void)ssid;
    (void)len;
    return FALSE;
}

BOOL DWC_AC_StartupGetWDSInfo(DWCWDSData *nspotInfo)
{
    (void)nspotInfo;
    return TRUE;
}

DWCWDSState DWC_AC_ProcessGetWDSInfo(void)
{
    /* No Nintendo Spot beacon is ever heard. */
    return DWC_WDS_STATE_FAILED;
}

void DWC_AC_CleanupGetWDSInfo(void)
{
}

/* CPS's SSL handshake thread priority: DWC_InitInetEx sets it, and
 * DWC_GetInetStatus reads it back to re-initialise for the Nintendo Spot
 * attempt. No SSL thread exists here; the value is only kept so the
 * read-back returns what was set. */
static u32 sSslHandshakePriority;

void CPS_SetSslHandshakePriority(u32 priority)
{
    sSslHandshakePriority = priority;
}

u32 CPS_GetSslHandshakePriority(void)
{
    return sSslHandshakePriority;
}

int DWC_StartUtility(void *work, int language, int param)
{
    /* The Nintendo WFC Settings utility is a prebuilt application this
     * build does not have. It returns at once, as the utility does when the
     * player leaves it without saving anything; WFCSettings_StartApplication
     * then resets the console as it always does afterwards. */
    (void)work;
    (void)language;
    (void)param;
    return DWC_UTIL_RESULT_FAILED;
}
