/*
 * Overlays on Diamond/Pearl: what FS_StartOverlay has to do beyond what
 * Platinum's games/platinum/pc/src/pc_fs_overlay.c does, which calls in here
 * under PC_GAME_DP.
 *
 * The load path is the SDK's own C (arm9/lib/NitroSDK/src/FS_overlay.c, and
 * the game's src/poke_overlay.c above it): FS_LoadOverlayInfo reads the
 * ROM's overlay table entry, FS_LoadOverlayImage clears the window's .bss
 * and reads the overlay file into guest RAM at ram_address, and
 * FS_StartOverlay runs the overlay. Every path that makes an overlay
 * runnable ends in FS_StartOverlay (FS_LoadOverlay, and poke_overlay.c's
 * image + start pairs, sync and async), so that is where it becomes
 * resident:
 *
 *   pc_dp_overlay_start  armrec_load_overlay(id, ram_address, ram + bss):
 *                        the recompiled functions of that window resolve to
 *                        this overlay from now on, everything it overlaps is
 *                        evicted, and its assembly data is written. Then
 *                        pc_fs_overlay.c resets the overlay's C statics.
 *   pc_dp_overlay_sinit  the static initialisers.
 *
 * Unloading does not drop residency. On hardware an unloaded overlay's code
 * stays in RAM, callable, until something is loaded over it, and that is
 * exactly when armrec_load_overlay evicts it.
 *
 * Static initialisers. The ROM's overlay table gives each overlay a
 * [sinit_init, sinit_init_end) array of code addresses, in the overlay's own
 * image (now in guest RAM). Every one of D's 87 overlays has the array; most
 * hold a single zero. The non-zero entries are one of two kinds:
 *
 *   - a recompiled function (overlay_12.s's ov12_022312BC): armrec knows its
 *     guest address, and it is dispatched. Only a *resident* claimant
 *     counts: the Poketch apps (ov21..ov50) and ov51 all load at
 *     0x02254840, where ov51's first function is recompiled and every app's
 *     is its C sinit, so asking for any claimant's name made the Watch's
 *     load dispatch into the evicted ov51 (Continue with a Poketch aborted);
 *   - a decompiled C function (overlay_01.c's NitroStaticInit, the
 *     overlay_NN_sinit.c ov22_02254840 family): its guest address means
 *     nothing here, so each such TU records its function at start-up through
 *     the include/sinit.h shadow (games/diamond/pc/include, pc/mk/game.mk:
 *     pc_dp_sinit_record(PC_DP_OVERLAY, NitroStaticInit)), and the records
 *     for the loaded overlay are called.
 *
 * Both together must account for every non-zero entry, and that is checked:
 * a TU that lost its record, or a C function that also became reachable by
 * guest address (and would run twice), stops the run by name.
 */
#include <nitro.h>
#include <nitro/fs.h>
#include <stdio.h>
#include <string.h>

#include <pc_wasm.h>

#include "armrec_rt.h"

#define PC_DP_SINIT_MAX 64

static struct {
    unsigned overlay;
    void (*fn)(void);
} sSinit[PC_DP_SINIT_MAX];
static int sSinitN;

void pc_dp_sinit_record(unsigned overlay_id, void (*fn)(void))
{
    if (sSinitN == PC_DP_SINIT_MAX) {
        pc_wasm_fatalf("pc_dp_overlay: more than %d C static initialisers",
                       PC_DP_SINIT_MAX);
    }
    sSinit[sSinitN].overlay = overlay_id;
    sSinit[sSinitN].fn = fn;
    sSinitN++;
}

void pc_dp_overlay_start(FSOverlayInfo *p_ovi)
{
    const FSOverlayInfoHeader *h = &p_ovi->header;

    if (p_ovi->target != MI_PROCESSOR_ARM9) {
        pc_wasm_fatalf("pc_dp_overlay: overlay %u started for the ARM7",
                       (unsigned)h->id);
    }
    /* An overlay stored BLZ-compressed (FS_OVERLAY_FLAG_COMP; D's never
     * are, TWL-SDK games' mostly are) is decompressed in place before it is
     * made resident, where the SDK's FS_StartOverlay does it: the image
     * FS_LoadOverlayImage read ends `compressed` bytes above ram_address. */
    if (h->flag & 1u) {
        MIi_UncompressBackward((u8 *)h->ram_address + h->compressed);
    }
    /* PC_TRACE_OVERLAYS=1: one line per overlay made resident, so a run
     * shows which overlays its path loads (Black: whether the opaque,
     * self-modifying ov230 is among them). */
    {
        extern char *getenv(const char *);
        extern unsigned long long pc_irq_frames(void);
        static int sTrace = -1;

        if (sTrace < 0) {
            const char *e = getenv("PC_TRACE_OVERLAYS");
            sTrace = e != NULL && e[0] != '\0' && e[0] != '0';
        }
        if (sTrace) {
            fprintf(stderr, "pc_dp_overlay: frame %llu: overlay %u at 0x%08X\n",
                    pc_irq_frames(), (unsigned)h->id, (unsigned)(uintptr_t)h->ram_address);
        }
    }
    armrec_load_overlay((int)h->id, (uint32_t)(uintptr_t)h->ram_address,
                        h->ram_size + h->bss_size);
}

void pc_dp_overlay_sinit(FSOverlayInfo *p_ovi)
{
    const FSOverlayInfoHeader *h = &p_ovi->header;
    const u32 *p = (const u32 *)h->sinit_init;
    const u32 *q = (const u32 *)h->sinit_init_end;
    int entries = 0, dispatched = 0, recorded = 0;
    int i;

    for (i = 0; i < sSinitN; i++) {
        if (sSinit[i].overlay == h->id) {
            sSinit[i].fn();
            recorded++;
        }
    }
    for (; p < q; p++) {
        u32 addr = *p;

        if (addr == 0) {
            continue;
        }
        entries++;
        if (armrec_code_live(addr)) {
            (void)armrec_dispatch(addr, 0, 0, 0, 0);
            dispatched++;
        }
    }
    if (recorded + dispatched != entries) {
        pc_wasm_fatalf("pc_dp_overlay: overlay %u's ROM table has %d static "
                       "initialiser(s); %d ran as recompiled code and %d as "
                       "recorded C (pc_dp_sinit_record)", (unsigned)h->id,
                       entries, dispatched, recorded);
    }
}
