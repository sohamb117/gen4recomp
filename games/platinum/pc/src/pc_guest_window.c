/*
 * The port's guest-addressed window.
 *
 * See pc/include/pc_guest_window.h for what this is for. What is here is a
 * bump allocator and a trap, and both choices are deliberate.
 *
 * Two halves, and only one of them is allocated. The window's low end is
 * whatever pc/Makefile's PC_GUEST_BSS told the linker to place there, today
 * arm9/src/sound.c's statics, 0xBCD80 bytes of them, because GCC inlines
 * GetSoundDataPointer() inside that file and leaves no accessor to override.
 * `__pc_guest_window_free` is where that ends and this allocator begins.
 *
 * A bump allocator for the other half, because nothing in it has a lifetime.
 * Its one caller today is pc_pxi.c's command-list mirror, which lives for the
 * whole process: the ARM9 has a fixed command array and recycles a node only
 * after the ARM7 has finished with it. A free list would be dead code with no
 * test able to reach it, and a block that never moves is what makes a run
 * reproducible; the same object gets the same guest address on every run, so
 * `--watch`, `--state-digest` and a gdb session all agree between runs.
 *
 * The window is found by name, not by address. armrec_rt.c's regions[] is
 * where a guest region is declared and this file does not get to hold a second
 * copy of the number; if the region is ever moved or resized, nothing here
 * needs to know. The lookup also means this file cannot be used before
 * armrec_mem_init(), which is correct: there is no guest memory to hand out
 * before then, and asking says so rather than returning an unmapped pointer.
 *
 * Running out traps, and the message carries the arithmetic. The window is
 * sized in armrec_rt.h to what the port asks of it, so exhausting it is not a
 * condition to recover from; it means a new caller arrived and the constant
 * was not raised with it. The trap names the caller, what it wanted, what is
 * left and which constant to raise, because the alternative is a NULL that
 * becomes a guest write to address 0 several frames later.
 */

#include "pc_guest_window.h"

#include "armrec_rt.h"

#include <stdio.h>
#include <string.h>

#define WINDOW_ALIGN 32u

/*
 * Where the link-placed half ends and the run-time half begins. The generated
 * fragment (pc/Makefile, PC_GUEST_BSS) puts the named objects' .bss at the
 * window's base and defines this immediately after it, so the allocator cannot
 * hand out an address the game's own statics already occupy and neither side
 * has to know how big the other is.
 */
extern char __pc_guest_window_free[] __attribute__((weak));

static uint32_t win_base;
static uint32_t win_size;
static uint32_t win_used;
static int      win_blocks;

static void window_find(void)
{
    int i, n;

    if (win_base != 0) {
        return;
    }
    n = armrec_region_count();
    for (i = 0; i < n; i++) {
        uint32_t base, size;
        const char *name = NULL;
        if (!armrec_region_at(i, &base, &size, &name)) {
            continue;
        }
        if (name != NULL && strcmp(name, ARMREC_PORT_WINDOW_NAME) == 0) {
            /*
             * Weak, and absent is a real answer rather than an error: a test
             * binary links this file without the port's own linker fragment,
             * so nothing was link-placed and the allocator starts at the
             * region's base. armrec_mem_init() maps the region in exactly that
             * case, so the two halves agree without either being told.
             */
            uint32_t placed = __pc_guest_window_free
                ? (uint32_t)(uintptr_t)__pc_guest_window_free : base;
            win_base = base;
            win_size = size;
            /*
             * The link-placed .bss is already spent. Checked rather than
             * assumed: the fragment and regions[] are generated from the same
             * two constants, so if they ever disagree it is a build that put
             * the game's statics somewhere else entirely, and handing out
             * addresses on top of them would be the quietest possible bug.
             */
            if (placed < base || placed > base + size) {
                static char msg[256];
                (void)snprintf(msg, sizeof msg,
                               "__pc_guest_window_free is 0x%08X, outside the "
                               "port window at 0x%08X..0x%08X. pc/Makefile's "
                               "generated fragment and armrec_rt.h's "
                               "ARM_PORT_WINDOW_* disagree.",
                               (unsigned)placed, (unsigned)base,
                               (unsigned)(base + size));
                armrec_trap("pc_guest_window", msg);
            }
            win_used = placed - base;
            return;
        }
    }
}

uint32_t pc_guest_window_base(void) { window_find(); return win_base; }
uint32_t pc_guest_window_size(void) { window_find(); return win_size; }
uint32_t pc_guest_window_used(void) { window_find(); return win_used; }
int      pc_guest_window_blocks(void) { return win_blocks; }

uint32_t pc_guest_window_placed_end(void)
{
    window_find();
    if (win_base == 0) {
        return 0u;
    }
    return __pc_guest_window_free ? (uint32_t)(uintptr_t)__pc_guest_window_free
                                  : win_base;
}

void *pc_guest_window_alloc(uint32_t bytes, const char *what)
{
    static char msg[512];
    uint32_t at;

    window_find();
    if (win_base == 0) {
        (void)snprintf(msg, sizeof msg,
                       "%s asked for %u bytes of the port window before "
                       "armrec_mem_init() mapped it. There is no guest memory "
                       "to hand out yet.",
                       what ? what : "a caller", (unsigned)bytes);
        armrec_trap("pc_guest_window", msg);
    }

    bytes = (bytes + (WINDOW_ALIGN - 1u)) & ~(WINDOW_ALIGN - 1u);
    if (bytes > win_size - win_used) {
        (void)snprintf(msg, sizeof msg,
                       "%s asked for %u bytes of the port window and %u are "
                       "left of %u. Raise ARM_PORT_WINDOW_SIZE in "
                       "tools/armrec/armrec_rt.h; the hole it sits in runs "
                       "to 0x03000000, so there is room.",
                       what ? what : "a caller", (unsigned)bytes,
                       (unsigned)(win_size - win_used), (unsigned)win_size);
        armrec_trap("pc_guest_window", msg);
    }

    at = win_base + win_used;
    win_used += bytes;
    win_blocks++;
    /*
     * Zeroed rather than trusted to be zero. It is fresh anonymous memory the
     * first time, which is what test_determinism's "not one non-zero byte"
     * check relies on, but a caller that allocates after guest code has run
     * should not have to know that, and a `static` in decompiled C is zero
     * before main() by the language's own rule.
     */
    memset((void *)(uintptr_t)at, 0, bytes);
    return (void *)(uintptr_t)at;
}
