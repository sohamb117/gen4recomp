/*
 * 3ds/src/3ds_snd_addr.c: SOUNDxSAD's 27 bits, and what fits through them.
 *
 * See 3ds_snd_addr.h for the rule. What is here is the two functions the SPU
 * glue will call and the self-test that makes the rule checkable before there
 * is any SPU.
 *
 * The mask is hardware's and it is applied, not widened. SoundxSAD is bits
 * 26..2 of a source address; pc/hw/pc_spu.c writes `v & 0x07FFFFFCu` into the
 * channel and this file does the same. A DS address field is a fact about the
 * console the port transcribes, the object at the far end of it moves into
 * guest memory instead, which is what the port window is for.
 *
 * The low two bits are dropped, like hardware. Nothing this port hands the SPU
 * is misaligned: pc_guest_window_alloc() returns 32-byte blocks and NNS's
 * sound heap asserts the same alignment on every block inside them. A caller
 * that offsets into a block by an odd number of bytes gets what a DS would
 * give it, which is the word below.
 *
 * WHY A `STRAY` class exists here and not on PC. On PC a host pointer that
 * crossed the mask landed in no mapped region and pc_spu counted it as `lost`.
 * On this console the process heap starts at 0x08000000 and the slab is a few
 * pages into it, so the mask maps large parts of the heap onto real guest
 * regions: 0x0A000000 becomes main RAM's base and 0x14000000, the linear
 * heap, becomes the I/O page. 3ds/tests/snd_addr.c measures the aliasing in
 * megabyte steps and prints it. A truncated pointer can therefore arrive as a
 * plausible address, and the only property that separates it from a real wave
 * is that every wave in this port lives in the sound heap, inside SoundSystem,
 * inside the window. So a readable source address outside the window is
 * counted, and the bytes are still returned: a console could legitimately play
 * from main RAM, and this file counts rather than judges.
 */

#include "3ds_snd_addr.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_window.h"

/* Bits 26..2. The same constant pc/hw/pc_spu.c masks with. */
#define SND_SAD_MASK  0x07FFFFFCu
#define SND_SAD_LIMIT (1u << 27)

uint32_t snd_addr_bad[SND_ADDR_KINDS];
uint32_t snd_addr_first[SND_ADDR_KINDS];

static void record(int kind, uint32_t value)
{
    if (snd_addr_bad[kind]++ == 0) {
        snd_addr_first[kind] = value;
    }
}

void snd_addr_reset(void)
{
    memset(snd_addr_bad, 0, sizeof snd_addr_bad);
    memset(snd_addr_first, 0, sizeof snd_addr_first);
}

uint32_t snd_sad_from_host(const void *host, const char *what)
{
    uint32_t guest = armrec_guest_addr(host);

    (void)what;

    if (guest == 0) {
        /*
         * Not guest memory at all. On PC this was `struct SoundData`, a static
         * in decompiled C, and the whole game was silent for it. The value
         * recorded is the pointer's low 32 bits: on this console that is the
         * pointer, and on the LP64 test host it is at least enough to
         * recognise the object.
         */
        record(SND_ADDR_HOST, (uint32_t)(uintptr_t)host);
        return 0;
    }

    if (guest >= SND_SAD_LIMIT) {
        /* Guest memory the field cannot reach: the AGB slot and above. A DS
         * could not play from there either. */
        record(SND_ADDR_WIDE, guest);
        return 0;
    }

    return guest & SND_SAD_MASK;
}

void *snd_sad_ptr(uint32_t sad, uint32_t bytes)
{
    uint32_t at = sad & SND_SAD_MASK;
    uint8_t *first;

    if (bytes == 0) {
        bytes = 1;
    }

    first = (uint8_t *)armrec_host_ptr(at);
    if (first == NULL) {
        record(SND_ADDR_LOST, sad);
        return NULL;
    }

    /*
     * The whole span, not just its first byte. A wave that starts inside a
     * region and ends past it is not a wave, and handing the mixer the first
     * byte would turn a truncated length into a buffer overrun. Contiguity is
     * checked through the translator rather than assumed from the region
     * table: the two agree today, and if they ever stop the mixer must not be
     * the place that finds out.
     */
    if (bytes > 1) {
        uint8_t *last = (uint8_t *)armrec_host_ptr(at + bytes - 1u);

        if (last == NULL || last != first + (bytes - 1u)) {
            record(SND_ADDR_LOST, sad);
            return NULL;
        }
    }

    if (at < (uint32_t)GUEST_BASE_WINDOW
        || at - (uint32_t)GUEST_BASE_WINDOW >= (uint32_t)GUEST_SIZE_WINDOW) {
        record(SND_ADDR_STRAY, sad);
    }

    return first;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef SND_ADDR_SELFTEST_VERBOSE
#define FAILNOTE() fprintf(stderr, "  3ds_snd_addr.c:%d failed\n", __LINE__)
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

#define WAVE_BYTES 4096u

int snd_addr_selftest(int *ranOut)
{
    int ran = 0;
    int failed = 0;
    uint8_t *wave;
    uint8_t *read;
    uint32_t sad;
    uint32_t i;

    window_reset();
    snd_addr_reset();

    wave = (uint8_t *)pc_guest_window_alloc(WAVE_BYTES, "selftest wave");
    CHECK(wave != NULL);
    if (wave == NULL) {
        goto done;
    }

    /*
     * The plan's own check: the host pointer is far above anything SOUNDxSAD
     * could carry; the 3DS heap starts at 0x08000000 and the test host's
     * pointers are 64-bit, and the guest address it maps to fits with room
     * to spare.
     */
    CHECK((uintptr_t)wave > 0x07FFFFFFu);

    sad = snd_sad_from_host(wave, "selftest wave");
    CHECK(sad == (uint32_t)GUEST_BASE_WINDOW);
    CHECK(sad < SND_SAD_LIMIT);
    CHECK((sad & ~SND_SAD_MASK) == 0);
    CHECK(snd_addr_bad[SND_ADDR_HOST] == 0);
    CHECK(snd_addr_bad[SND_ADDR_WIDE] == 0);

    /* Written through the host pointer the game holds... */
    for (i = 0; i < WAVE_BYTES; i++) {
        wave[i] = (uint8_t)(i * 7u + 1u);
    }

    /* ...and found by the DMA through the address the register holds. */
    read = (uint8_t *)snd_sad_ptr(sad, WAVE_BYTES);
    CHECK(read == wave);
    CHECK(snd_addr_bad[SND_ADDR_STRAY] == 0);
    CHECK(snd_addr_bad[SND_ADDR_LOST] == 0);
    if (read != NULL) {
        CHECK(read[0] == 1);
        CHECK(read[1] == 8);
        CHECK(read[WAVE_BYTES - 1] == (uint8_t)((WAVE_BYTES - 1) * 7u + 1u));
    }

    /* A word read the way the mixer takes one, at an offset into the wave. */
    {
        uint32_t off = 0x40u;
        uint32_t *w = (uint32_t *)snd_sad_ptr(sad + off, 4);

        CHECK(w != NULL);
        if (w != NULL) {
            CHECK(*w == (uint32_t)wave[off]
                        + ((uint32_t)wave[off + 1] << 8)
                        + ((uint32_t)wave[off + 2] << 16)
                        + ((uint32_t)wave[off + 3] << 24));
        }
    }

    /* The low two bits are the field's, not the caller's. */
    CHECK(snd_sad_ptr(sad + 0x43u, 4) == snd_sad_ptr(sad + 0x40u, 4));

    /* A span that leaves the region is refused, not clamped. */
    {
        uint32_t tail = (uint32_t)GUEST_BASE_WINDOW
                        + (uint32_t)GUEST_SIZE_WINDOW - 16u;
        uint32_t before = snd_addr_bad[SND_ADDR_LOST];

        CHECK(snd_sad_ptr(tail, 4) != NULL);
        CHECK(snd_sad_ptr(tail, 64) == NULL);
        CHECK(snd_addr_bad[SND_ADDR_LOST] == before + 1u);
    }

    /* HOST: a pointer that is not guest memory at all, the class the PC port
     * spent a task on. A stack address is the cheapest one to produce. */
    {
        uint32_t before = snd_addr_bad[SND_ADDR_HOST];

        CHECK(snd_sad_from_host(&ran, "a stack address") == 0);
        CHECK(snd_addr_bad[SND_ADDR_HOST] == before + 1u);
        CHECK(snd_addr_first[SND_ADDR_HOST] == (uint32_t)(uintptr_t)&ran);
    }

    /* WIDE: guest memory past the field's reach. The AGB slot is the only such
     * region in the map, which is itself worth asserting, if a later row
     * lands above 0x08000000 this check is where it shows up. */
    {
        void *agb = armrec_host_ptr((uint32_t)GUEST_BASE_AGB);

        CHECK(agb != NULL);
        CHECK(snd_sad_from_host(agb, "the AGB slot") == 0);
        CHECK(snd_addr_bad[SND_ADDR_WIDE] == 1);
        CHECK(snd_addr_first[SND_ADDR_WIDE] == (uint32_t)GUEST_BASE_AGB);
    }

    /* LOST: a source address with nothing behind it. */
    {
        uint32_t before = snd_addr_bad[SND_ADDR_LOST];

        CHECK(snd_sad_ptr(0x02400000u, 4) == NULL); /* the 4 MB hole */
        CHECK(snd_sad_ptr(0x06000000u, 4) == NULL); /* VRAM, until 2.7 */
        CHECK(snd_addr_bad[SND_ADDR_LOST] == before + 2u);
    }

    /* STRAY: readable, outside the window, counted, and still returned. */
    {
        uint32_t elsewhere = (uint32_t)GUEST_BASE_MAIN + 0x100u;
        uint32_t before = snd_addr_bad[SND_ADDR_STRAY];

        CHECK(snd_sad_ptr(elsewhere, 4) == armrec_host_ptr(elsewhere));
        CHECK(snd_addr_bad[SND_ADDR_STRAY] == before + 1u);
        CHECK(snd_addr_first[SND_ADDR_STRAY] == elsewhere);
    }

    /*
     * The hazard this console adds, demonstrated rather than described: a
     * plausible heap pointer, truncated by the field, lands on main RAM's base
     * and is readable. That is why STRAY is counted at all, on PC the same
     * mistake produced an address in no region and was loud.
     */
    {
        uint32_t heapish = 0x0A000000u;
        uint32_t before = snd_addr_bad[SND_ADDR_STRAY];

        CHECK((heapish & SND_SAD_MASK) == (uint32_t)GUEST_BASE_MAIN);
        CHECK(snd_sad_ptr(heapish, 4) != NULL);
        CHECK(snd_addr_bad[SND_ADDR_STRAY] == before + 1u);
    }

    /* The counters are a measurement, so they have to be resettable. */
    snd_addr_reset();
    CHECK(snd_addr_bad[SND_ADDR_HOST] == 0);
    CHECK(snd_addr_bad[SND_ADDR_WIDE] == 0);
    CHECK(snd_addr_bad[SND_ADDR_LOST] == 0);
    CHECK(snd_addr_bad[SND_ADDR_STRAY] == 0);
    CHECK(snd_addr_first[SND_ADDR_HOST] == 0);

done:
    window_reset();
    snd_addr_reset();
    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
