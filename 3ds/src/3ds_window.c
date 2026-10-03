/*
 * 3ds/src/3ds_window.c: the port window, handed out of the slab.
 *
 * See 3ds_window.h for what the window is and why the symbols are spelled the
 * PC port's way. What is here is a bump allocator over one row of the guest
 * map and a self-test.
 *
 * There is no link-placed half, and this game's PC build does not have one
 * either. tools/armrec/armrec_rt.h still describes the window as two halves, a
 * linker-placed sound buffer plus a bump allocator for the rest, which is the
 * sibling diamond port's arrangement carried over with the file. Neither name
 * exists in this game, and the only thing that reaches the window is
 * SoundSystem_Get(), which a patch rewrites to call pc_guest_window_alloc() at
 * run time. So the whole window is allocatable here.
 *
 * The caller is one object, and it is big. SoundSystem is 0xBCDA0 bytes,
 * measured in the host build's DWARF and in the ARM11 objects, which agree
 * exactly; the ROM build's mwcc makes it 24 bytes smaller with every member at
 * the same offset, so the difference is trailing padding. It is 37% of the
 * 2 MB window, which is the number to remember if the window is ever resized.
 *
 * Running out returns NULL, which is not what the PC does. There it traps and
 * the process dies with the arithmetic on stderr: the window is sized to what
 * the port asks of it, so exhausting it means a new caller arrived. That is
 * still the right reading of the failure, but this console has no stderr, so a
 * NULL plus window_strerror() is what can be reported today.
 */

#include "3ds_window.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"

/* NNS_SndHeapAlloc asserts `(u32)block->buffer & 0x1f) == 0` on every block it
 * hands out, and the sound heap is the object that lives here. The window's
 * base and size are both 32-byte multiples, so an aligned slab makes every
 * block aligned in host terms as well as guest terms. */
#define WINDOW_ALIGN 32u

_Static_assert(GUEST_BACK_WINDOW == GUEST_SIZE_WINDOW,
               "the port window is fully backed: every address in it is real");
_Static_assert(GUEST_BASE_WINDOW % (int)WINDOW_ALIGN == 0
                   && GUEST_SIZE_WINDOW % (int)WINDOW_ALIGN == 0,
               "the window's own bounds must be 32-byte aligned");
/*
 * The reason this region exists at all: SOUNDxSAD carries 27 bits, so a wave
 * the SPU is asked to play has to have a guest address below 0x08000000. The
 * window ends at 0x02C00000. The allocator checks the same thing about a block; this
 * checks it about the region, at compile time, so moving the row somewhere
 * roomier cannot silently break the sound.
 */
_Static_assert((unsigned)GUEST_BASE_WINDOW + (unsigned)GUEST_SIZE_WINDOW
                   <= (1u << 27),
               "the port window must fit in SOUNDxSAD's 27 bits");
/*
 * And the narrower of the two fields a window address has to cross: the PXI
 * FIFO's command word carries `data:26`, which is why the PC side
 * took a slice of this same hole for the ARM7's command-list mirror. The
 * window ends at 0x02C00000, so both fields can carry it.
 */
_Static_assert((unsigned)GUEST_BASE_WINDOW + (unsigned)GUEST_SIZE_WINDOW
                   <= (1u << 26),
               "the port window must fit in the FIFO's 26-bit data field");

static uint32_t sUsed;
static int      sBlocks;
static char     sError[160];

uint32_t pc_guest_window_base(void) { return (uint32_t)GUEST_BASE_WINDOW; }
uint32_t pc_guest_window_size(void) { return (uint32_t)GUEST_SIZE_WINDOW; }
uint32_t pc_guest_window_used(void) { return sUsed; }
int      pc_guest_window_blocks(void) { return sBlocks; }

uint32_t pc_guest_window_placed_end(void) { return (uint32_t)GUEST_BASE_WINDOW; }

const char *window_strerror(void) { return sError; }

void window_reset(void)
{
    sUsed = 0;
    sBlocks = 0;
    sError[0] = '\0';
}

void *pc_guest_window_alloc(uint32_t bytes, const char *what)
{
    uint32_t at;
    void *host;

    sError[0] = '\0';
    bytes = (bytes + (WINDOW_ALIGN - 1u)) & ~(WINDOW_ALIGN - 1u);

    if (bytes > (uint32_t)GUEST_SIZE_WINDOW - sUsed) {
        (void)snprintf(sError, sizeof sError,
                       "%s asked for %u bytes of the port window and %u are "
                       "left of %u",
                       what != NULL ? what : "a caller", (unsigned)bytes,
                       (unsigned)((uint32_t)GUEST_SIZE_WINDOW - sUsed),
                       (unsigned)GUEST_SIZE_WINDOW);
        return NULL;
    }

    /*
     * Through the translator, not through mem_region_ptr(): this file must not
     * know where the slab is, and going the same way every other guest access
     * goes means a broken binding fails here rather than three regions away.
     * A non-NULL answer covers the whole block; the row is contiguous and
     * fully backed, asserted above, so one lookup is enough.
     */
    at = (uint32_t)GUEST_BASE_WINDOW + sUsed;
    host = armrec_host_ptr(at);
    if (host == NULL) {
        (void)snprintf(sError, sizeof sError,
                       "%s asked for %u bytes of the port window before the "
                       "slab was bound",
                       what != NULL ? what : "a caller", (unsigned)bytes);
        return NULL;
    }

    sUsed += bytes;
    sBlocks++;
    /*
     * Zeroed rather than trusted to be zero. mem_init() zeroes the slab once,
     * but a block allocated after guest code has already run through this
     * region should not have to know that, and a `static` in decompiled C is
     * zero before main() by the language's own rule.
     */
    memset(host, 0, bytes);
    return host;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

/*
 * Same shape as guest_selftest(): a count on the console, where there is room
 * for one line, and a line number per failure on the host, where there is a
 * terminal and a person reading it.
 */
#ifdef WINDOW_SELFTEST_VERBOSE
#define FAILNOTE() fprintf(stderr, "  3ds_window.c:%d failed\n", __LINE__)
#else
#define FAILNOTE() ((void)0)
#endif

#define CHECK(cond)                                                           \
    do {                                                                      \
        ran++;                                                                \
        if (!(cond)) {                                                        \
            failed++;                                                         \
            FAILNOTE();                                                       \
        }                                                                     \
    } while (0)

/*
 * sizeof(SoundSystem), the one object the window exists for today. Written
 * down because no 3DS translation unit can compute it: including the game's
 * headers here would drag <nitro.h> in beside libctru's world, and settled
 * decision 16 says those two may not meet in one file. Measured in the host
 * build's DWARF and in build/3ds/abi's ARM11 objects, which agree.
 */
#define WINDOW_SOUND_SYSTEM_BYTES 0x000BCDA0u

int window_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;
    uint8_t *fill;
    uint8_t *a;
    uint8_t *b;
    uint8_t *big;
    uint32_t left;

    window_reset();

    fill = (uint8_t *)armrec_host_ptr((uint32_t)GUEST_BASE_WINDOW);
    if (fill == NULL) {
        /* Unbound slab: the allocator must refuse rather than fault, and that
         * is all this test can check. */
        ran++;
        if (pc_guest_window_alloc(32, "selftest") != NULL) {
            failed++;
            FAILNOTE();
        }
        if (ranOut != NULL) {
            *ranOut = ran;
        }
        return failed + 1;
    }

    /* Poison the whole region, so "the block came back zeroed" is a claim
     * about the allocator and not about a slab that happened to be fresh. */
    memset(fill, 0xA5, (size_t)GUEST_SIZE_WINDOW);

    /* Constants first. */
    CHECK(pc_guest_window_base() == (uint32_t)GUEST_BASE_WINDOW);
    CHECK(pc_guest_window_size() == (uint32_t)GUEST_SIZE_WINDOW);
    CHECK(pc_guest_window_placed_end() == pc_guest_window_base());
    CHECK(pc_guest_window_used() == 0);
    CHECK(pc_guest_window_blocks() == 0);

    /* One byte, which is really 32. */
    a = (uint8_t *)pc_guest_window_alloc(1, "selftest a");
    CHECK(a != NULL);
    if (a == NULL) {
        goto done;
    }
    CHECK(a == fill);
    CHECK(((uintptr_t)a & (WINDOW_ALIGN - 1u)) == 0);
    CHECK(pc_guest_window_used() == WINDOW_ALIGN);
    CHECK(pc_guest_window_blocks() == 1);
    CHECK(a[0] == 0 && a[31] == 0);
    /* Rounding is the allocator's, not the caller's: byte 32 is still poison
     * because nothing has handed it out yet. */
    CHECK(a[32] == 0xA5);

    /* Both directions through the translator, and the 27-bit fit the region
     * exists for. */
    CHECK(armrec_guest_addr(a) == (uint32_t)GUEST_BASE_WINDOW);
    CHECK(armrec_host_ptr((uint32_t)GUEST_BASE_WINDOW) == a);
    CHECK(armrec_guest_addr(a) < (1u << 27));

    /* A second block does not overlap the first, and an odd size rounds up. */
    b = (uint8_t *)pc_guest_window_alloc(33, "selftest b");
    CHECK(b != NULL);
    if (b == NULL) {
        goto done;
    }
    CHECK(b == a + WINDOW_ALIGN);
    CHECK(((uintptr_t)b & (WINDOW_ALIGN - 1u)) == 0);
    CHECK(pc_guest_window_used() == WINDOW_ALIGN + 64u);
    CHECK(pc_guest_window_blocks() == 2);
    CHECK(b[0] == 0 && b[63] == 0);
    CHECK(b[64] == 0xA5);

    a[0] = 0x11;
    b[0] = 0x22;
    CHECK(a[0] == 0x11 && b[0] == 0x22);
    CHECK(armrec_guest_addr(b) == (uint32_t)GUEST_BASE_WINDOW + WINDOW_ALIGN);
    CHECK(armrec_guest_addr(b) < (1u << 27));

    /* The object the window is for. Its last byte has to be inside the window
     * and has to translate back, because a block that fits arithmetically and
     * runs off the end of the backing is the bug this whole port is about. */
    window_reset();
    memset(fill, 0xA5, (size_t)GUEST_SIZE_WINDOW);
    big = (uint8_t *)pc_guest_window_alloc(WINDOW_SOUND_SYSTEM_BYTES,
                                           "SoundSystem");
    CHECK(big != NULL);
    if (big == NULL) {
        goto done;
    }
    CHECK(big == fill);
    CHECK(pc_guest_window_used() == WINDOW_SOUND_SYSTEM_BYTES);
    CHECK(big[0] == 0);
    CHECK(big[WINDOW_SOUND_SYSTEM_BYTES - 1] == 0);
    CHECK(armrec_host_ptr((uint32_t)GUEST_BASE_WINDOW
                          + WINDOW_SOUND_SYSTEM_BYTES - 1u)
          == big + WINDOW_SOUND_SYSTEM_BYTES - 1u);
    CHECK(armrec_guest_addr(big + WINDOW_SOUND_SYSTEM_BYTES - 1u)
          == (uint32_t)GUEST_BASE_WINDOW + WINDOW_SOUND_SYSTEM_BYTES - 1u);
    CHECK(pc_guest_window_size() - pc_guest_window_used()
          > WINDOW_SOUND_SYSTEM_BYTES / 2u); /* room left for 2.5 and the SPU */

    /* Exhaustion: refused, and the refusal changes nothing. */
    CHECK(pc_guest_window_alloc((uint32_t)GUEST_SIZE_WINDOW, "too big") == NULL);
    CHECK(pc_guest_window_used() == WINDOW_SOUND_SYSTEM_BYTES);
    CHECK(pc_guest_window_blocks() == 1);
    CHECK(window_strerror()[0] != '\0');

    /* Exactly what is left is not too much; one byte more is. */
    left = pc_guest_window_size() - pc_guest_window_used();
    CHECK(pc_guest_window_alloc(left, "the rest") != NULL);
    CHECK(pc_guest_window_used() == pc_guest_window_size());
    CHECK(window_strerror()[0] == '\0');
    CHECK(pc_guest_window_alloc(1, "one past the end") == NULL);
    CHECK(pc_guest_window_used() == pc_guest_window_size());

    /* The last byte of a full window is still the last byte of the region. */
    CHECK(armrec_host_ptr((uint32_t)GUEST_BASE_WINDOW
                          + (uint32_t)GUEST_SIZE_WINDOW - 1u)
          == fill + GUEST_SIZE_WINDOW - 1u);
    CHECK(armrec_host_ptr((uint32_t)GUEST_BASE_WINDOW
                          + (uint32_t)GUEST_SIZE_WINDOW) == NULL);

done:
    /* Hand back what the test took. The caller still has to zero the slab:
     * This scribbled 0xA5 over 2 MB of it. */
    window_reset();
    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
