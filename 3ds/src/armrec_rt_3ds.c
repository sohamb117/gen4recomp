/*
 * 3ds/src/armrec_rt_3ds.c: armrec's runtime, the half that is not memory.
 *
 * armrec_mem_3ds.c answers the memory half of tools/armrec/armrec_rt.c on this
 * console: the region table, the translation, the slab. The rest of that file
 * is four more things, the GX command port, the ARM9 coprocessor, the VRAM
 * window queries and the fatal path, and none of them had a 3DS side. This is
 * that side. It is an export layer and almost nothing else: the models are
 * 3ds_vram.c, 3ds_cp.c, 3ds_io.c and 3ds_fault.c, each of which is pure C a
 * build machine can run.
 *
 * Why the models do not carry armrec's names themselves. A model with a local
 * name links into a host test on its own, with no armrec header and no second
 * definition to collide with. And an export layer is where the two ports'
 * differences become visible in one place instead of scattered through four
 * files: the render bracket that is a no-op here, the address conversion the
 * register shadow forces, and the trap that draws instead of writing to a
 * stderr this console does not have.
 *
 * tools/armrec/armrec_rt.c is not edited, because the sibling diamond port
 * builds that file and wants the PC half byte-identical. The cost is that a
 * few names appear twice, and 3ds/tests/vram_pin.py keeps the copies honest.
 *
 * Addresses arrive as host pointers, which is the one surprise here. The
 * register shadow makes HW_REG_BASE the slab's I/O row, so an address the
 * ioreg shadows hand to armrec_cp_ptr() is a pointer into the slab rather than
 * 0x04000280. The conversion happens here, at the door, and it accepts a real
 * guest address too: the two ranges cannot overlap on this console.
 */

#include <stdio.h>

#include "armrec_rt.h"

#include "3ds_cp.h"
#include "3ds_fault.h"
#include "3ds_guest.h"
#include "3ds_io.h"
#include "3ds_vram.h"

/*
 * The geometry engine's three entry points, declared rather than included.
 * pc/include is not on this file's compile line and must not be: it holds the
 * DS SDK shadows, and a translation unit that sees both those and libctru is
 * the thing this port does not have. Nothing here needs the rest of
 * pc_gpu3d.h, and the link is what checks the three signatures.
 */
uint32_t *pc_gpu3d_stage(uint32_t addr);
void pc_gpu3d_refresh_regs(void);
int pc_gpu3d_installed(void);

/* The models spell these locally; the values have to be the same or a caller
 * asking armrec for the sub-screen palettes would get the main screen's. */
_Static_assert(ARMREC_CPU_ARM9 == IO_CPU_ARM9, "");
_Static_assert(ARMREC_CPU_ARM7 == IO_CPU_ARM7, "");
_Static_assert((int)ARMREC_EXTPAL_ABG == (int)VRAM_EXTPAL_ABG, "");
_Static_assert((int)ARMREC_EXTPAL_BBG == (int)VRAM_EXTPAL_BBG, "");
_Static_assert((int)ARMREC_EXTPAL_AOBJ == (int)VRAM_EXTPAL_AOBJ, "");
_Static_assert((int)ARMREC_EXTPAL_BOBJ == (int)VRAM_EXTPAL_BOBJ, "");
_Static_assert(ARM_VRAM_BANKS == VRAM_BANKS, "");
_Static_assert(ARM_VRAM_BLK == VRAM_BLK, "");
_Static_assert(ARMREC_TEX_SLOT_SIZE == 0x20000u, "");
_Static_assert(ARMREC_TEXPAL_SLOT_SIZE == 0x4000u, "");

/*
 * A guest address for something that may already be one. armrec_guest_addr()
 * answers 0 for a pointer that is not in the slab, and 0 is not a guest
 * address in this map, so it doubles as "this was a guest address to begin
 * with"; which is what a caller in a file the register shadow does not reach
 * would pass.
 */
static uint32_t ioreg_guest(uint32_t addr)
{
    uint32_t guest = armrec_guest_addr((const void *)(uintptr_t)addr);

    return guest != 0 ? guest : addr;
}

/* ------------------------------------------------------------------ */
/* The two processors' I/O pages                                       */
/* ------------------------------------------------------------------ */

/*
 * pc/hw/pc_spu.c is the only caller: the sound driver runs as the ARM7 and the
 * mixer reads the ARM9's page. On PC the running processor's page is the live
 * mapping at 0x04000000 and the other's is a save buffer; here it is the
 * translated I/O row and a save buffer, which is the same answer with the
 * address space taken out.
 */
void *armrec_io_page(int cpu)
{
    return io_page(cpu);
}

/* ------------------------------------------------------------------ */
/* VRAM                                                                */
/* ------------------------------------------------------------------ */

void armrec_vram_touch(void) { vram_touch(); }

/*
 * The instrumentation hooks, which this console did not have. The five SDK
 * files that write a VRAMCNT register are compiled with
 * -finstrument-functions so the remap runs after the store, a store through
 * a macro cannot carry code after it, so the function's exit is the first
 * moment the new mapping is known and the last one before the caller copies
 * tiles through a window. tools/armrec/armrec_rt.c holds the pair for the PC
 * port and is not linked here, so on this console both names were UNDEFINED
 * WEAK: the linker answers a call to one of those with a nop, and every hook
 * in the port has been doing nothing since the hardware model. The remap still happened,
 * once a frame from the renderer, which is why nothing looked broken, but a
 * screen that switches banks and loads its assets in the same frame loads
 * them through the OLD windows, which is the exact defect the instrumentation
 * was added to the PC port to fix.
 *
 * It surfaced as a build that hangs. A nop'd `bl` is harmless, the function
 * returns on the next instruction, but -O2's -foptimize-sibling-calls turns
 * the last hook of a function into a tail `b`, and a nop'd tail branch means
 * the function never returns at all: it falls into whatever the linker put
 * next. Measured 2026-08-17: gx_vramcnt.c at -O2 alone, 87 tail branches, and
 * the game stops dead the moment it is handed the console.
 *
 * no_instrument_function on both, or the hook instruments itself.
 */
__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void *this_fn, void *call_site)
{
    (void)this_fn;
    (void)call_site;
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void *this_fn, void *call_site)
{
    (void)this_fn;
    (void)call_site;
    vram_touch();
}

void *armrec_vram_bank_ptr(int bank) { return vram_bank_ptr(bank); }

uint32_t armrec_vram_bank_size(int bank) { return vram_bank_size(bank); }

uint32_t armrec_vram_bank_lcdc(int bank) { return vram_bank_lcdc(bank); }

uint32_t armrec_vram_cnt_addr(int bank) { return vram_cnt_addr(bank); }

unsigned long armrec_vram_remaps(void) { return vram_remaps(); }

int armrec_vram_lookup(uint32_t a, int *bank, uint32_t *off)
{
    return vram_lookup(a, bank, off);
}

void *armrec_vram_extpal(int which, int slot) { return vram_extpal(which, slot); }

void *armrec_vram_texture(int slot) { return vram_texture(slot); }

void *armrec_vram_texpal(int slot) { return vram_texpal(slot); }

int armrec_vram_bank_in_lcdc(int bank) { return vram_bank_in_lcdc(bank); }

/*
 * The render bracket is a no-op here, and which of the PC port's two answers
 * this console gets is a decision rather than a copy. On POSIX the five
 * windows are aliasing mmaps of one backing object, so a CPU write is already
 * in the bank store and the bracket does nothing. On Windows the 64 KB mapping
 * granularity forces a copy model, and the bracket is where writes move
 * between the store and the windows.
 *
 * This console is the first case and more so: there are no window mappings at
 * all. 3ds_vram.c resolves every VRAM address through vram_lookup() into the
 * one 0xA4000 store, so a write through the BG window IS the bank, with no
 * aliasing to reconcile and nothing for a frame boundary to push. A copy here
 * would have nowhere to copy from.
 */
void armrec_vram_render_begin(void) { }
void armrec_vram_render_end(void) { }

/* ------------------------------------------------------------------ */
/* The coprocessor and the geometry engine                             */
/* ------------------------------------------------------------------ */

void *armrec_cp_ptr(uint32_t addr)
{
    return cp_ptr(ioreg_guest(addr));
}

/*
 * The geometry command window, which on a DS is 0x04000400-0x040005CB of the
 * ARM9's I/O space. Spelled here rather than included: pc/include is not on
 * this file's compile line, for the reason given above the three declarations.
 */
#define GX_CMD_BASE  0x04000400u
#define GX_CMD_END   0x040005CCu

/*
 * And the collision that makes the buffer below necessary. The ARM7's sixteen
 * sound channels are 0x04000400-0x0400051F of the *ARM7's* I/O space, which is
 * different hardware from the geometry engine at the same numbers on the ARM9.
 * This port has one I/O page, so those are the same bytes, and the sound
 * latch reads them once a frame. A geometry command stored as plain memory
 * therefore arrives at the mixer as a channel register: MTX_PUSH's dummy zero
 * is channel 4's source address, MTX_IDENTITY's is channel 5's. Measured on
 * the console, first bad lane at frame 370 of a boot.
 *
 * So the not-installed case gets its own words. A command port is write-only
 * on hardware and nothing reads one back, so a discard buffer is the same
 * model the page was providing minus the aliasing; the count says how much is
 * being thrown away, which is the geometry engine's own "before" number.
 */
/*
 * Sixteen words wider than the window: pc/src/pc_gx.c stores at *fixed*
 * offsets from the pointer it was handed, nine of them for the widest
 * matrix load, so a port near the top of the window is written past its own
 * address, exactly as it would be on the page it used to get.
 */
static uint32_t sGxDiscard[(GX_CMD_END - GX_CMD_BASE) / 4 + 16];
static unsigned long sGxDiscarded;

unsigned long armrec_gx_discarded(void) { return sGxDiscarded; }

/*
 * The staging word, and the one place this file has to know something the PC
 * port does not. pc_gpu3d_stage() hands back plain memory when the engine is
 * not installed; which is every build that leaves the 3D engine out, and
 * this one until the geometry engine is hooked, and "plain memory" there
 * means the guest address itself. That is a pointer on PC and is not one here,
 * so the not-installed case is answered through the translator instead, except
 * inside the command window, where it is answered privately.
 */
uint32_t *armrec_gx_port(uint32_t addr)
{
    uint32_t guest = ioreg_guest(addr);

    if (!pc_gpu3d_installed()) {
        if (guest >= GX_CMD_BASE && guest < GX_CMD_END) {
            sGxDiscarded++;
            return &sGxDiscard[(guest - GX_CMD_BASE) / 4];
        }
        return (uint32_t *)armrec_host_ptr(guest);
    }
    return pc_gpu3d_stage(guest);
}

void *armrec_gx_reg(uint32_t addr)
{
    pc_gpu3d_refresh_regs();
    return armrec_host_ptr(ioreg_guest(addr));
}

/* ------------------------------------------------------------------ */
/* The fatal path                                                      */
/* ------------------------------------------------------------------ */

/*
 * PC writes two lines to stderr and calls abort(). This console has no stderr,
 * and a black screen is not a report, so the same two lines go where the fault
 * handler's do: over the last picture, until Start. The message a model passes
 * is the whole of the debugging channel here, which is why the callers spell
 * theirs out.
 */
void armrec_trap(const char *fn, const char *what)
{
    char top[64];

    snprintf(top, sizeof top, "TRAP IN %s", fn != NULL ? fn : "?");
    fault_stop(top, what);

    /* fault_stop() ends the process. This is here because the attribute on
     * armrec_trap() promises as much and nothing tells the compiler so. */
    for (;;) {
    }
}
