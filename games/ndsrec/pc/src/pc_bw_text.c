/*
 * NP_OPT_TEXT_INSTANT on Black/White: Platinum's rule (pc/patches/src/
 * text.c.patch, PcNp_RushPrinter) on the game's print stream.
 *
 * B/W print a message through a print stream, a task the game runs every
 * frame (Black sub_0201CEE0, White sub_0201CEFC). Its work block:
 *
 *   +0x00 state: 0 running, 1 paused for a button, 2 done
 *   +0x14 the current code (u16 *); 0xFFFF ends the message, 0xF000 then
 *         0xBEnn is a control tag
 *   +0x1E the per-character delay counter (also tag BE02's timed wait)
 *   +0x21 the callback's last answer (non-zero: busy, the task polls it)
 *   +0x24 the callback u8 cb(u32 arg), +0x2C / +0x30 its default / next arg
 *   +0x34 the glyph job, which the task passes to the one-glyph renderer
 *         (Black sub_0201C974, White sub_0201C990: (job, sp, 0) -> the
 *         code after the glyph; it consumes newlines and tags other than
 *         0xBEnn, and stops at 0xBEnn and at the end)
 *
 * Control tags (Black sub_0201D110, White sub_0201D12C, on the work block):
 * BE00 / BE01 wait for a button then scroll / clear, BE02 waits N frames,
 * BE03..BE09 set the speed or the callback's argument or clear the window.
 *
 * pc/patches/<VER>/arm9/asm/ndsrec_arm9_006.s.patch retargets the task's
 * one call to the glyph renderer to PcBw_RushPrinter. Off (the default) it
 * is that call. On, after the glyph the task asked for it keeps going within
 * the frame, as the task would over the following frames, for as long as
 * there is only text, newlines and BE03..BE09 tags: it runs the stream's
 * callback after each item as the task does (and stops while it is busy),
 * applies BE03..BE09 through the game's own tag handler, and stops at the
 * end, at a button wait (BE00 / BE01), at a timed wait (BE02), or when the
 * stream leaves the running state. The task then finishes the frame as
 * usual: the bitmap transfer, the last item's callback, the delay before
 * the next character. So a page fills at once, and every prompt, scroll,
 * scripted pause and callback wait still behaves as the game wrote it.
 * Addresses: docs/BWHGSS_HOOKS.md, "Instant text".
 */
#include <stdint.h>

#include "armrec_rt.h"
#include "pc_np_options.h"

#if defined(PC_BW_VER_WHITE)
#define BW_PRINT_GLYPH sub_0201C990
#define BW_PRINT_TAG sub_0201D12C
#else
#define BW_PRINT_GLYPH sub_0201C974
#define BW_PRINT_TAG sub_0201D110
#endif

extern uint64_t BW_PRINT_GLYPH(uint32_t job, uint32_t sp, uint32_t passTags, uint32_t unused);
extern uint64_t BW_PRINT_TAG(uint32_t stream, uint32_t unused1, uint32_t unused2, uint32_t unused3);

#define STREAM_STATE 0x00
#define STREAM_SP 0x14
#define STREAM_CB_RESULT 0x21
#define STREAM_CB 0x24
#define STREAM_CB_ARG_DEFAULT 0x2C
#define STREAM_CB_ARG 0x30
#define STREAM_JOB 0x34

#define CODE_END 0xFFFFu
#define CODE_TAG 0xF000u
#define TAG_GROUP 0xBEu
#define TAG_LAST_WAIT 0x02u /* BE00, BE01 button waits; BE02 timed wait */

/* No message comes near this many items; a bound in case one never ends. */
#define RUSH_MAX 4096

static uint32_t rd32(uint32_t a)
{
    return *(const volatile uint32_t *)(uintptr_t)a;
}

static unsigned rd16(uint32_t a)
{
    return *(const volatile uint16_t *)(uintptr_t)a;
}

static void wr32(uint32_t a, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)a = v;
}

static void wr8(uint32_t a, unsigned v)
{
    *(volatile uint8_t *)(uintptr_t)a = (uint8_t)v;
}

/* Called by the print task in place of the glyph renderer: (job, sp, 0) ->
 * the code to go on from. */
uint32_t PcBw_RushPrinter(uint32_t job, uint32_t sp, uint32_t passTags)
{
    const uint32_t stream = job - STREAM_JOB;
    int i;

    sp = (uint32_t)ARMREC_CALL(BW_PRINT_GLYPH, job, sp, passTags, 0);
    if (!pc_np_opt.text_instant) return sp;
    for (i = 0; i < RUSH_MAX; i++) {
        const unsigned code = rd16(sp);
        const int tag = code == CODE_TAG && (rd16(sp + 2) >> 8) == TAG_GROUP;
        uint32_t cb;

        if (rd32(stream + STREAM_STATE) != 0) break; /* paused or done */
        if (code == CODE_END) break;                 /* the task marks it done */
        if (tag && (rd16(sp + 2) & 0xFF) <= TAG_LAST_WAIT) break;
        cb = rd32(stream + STREAM_CB);
        if (cb != 0) { /* the callback the task runs after each item */
            const unsigned busy = (unsigned)(uint8_t)armrec_call_code(cb, rd32(stream + STREAM_CB_ARG), 0, 0, 0);

            wr8(stream + STREAM_CB_RESULT, busy);
            if (busy) break; /* the task polls it from here */
            wr32(stream + STREAM_CB_ARG, rd32(stream + STREAM_CB_ARG_DEFAULT));
        }
        if (tag) {
            wr32(stream + STREAM_SP, sp);
            ARMREC_CALL(BW_PRINT_TAG, stream, 0, 0, 0);
            sp = rd32(stream + STREAM_SP);
        } else {
            sp = (uint32_t)ARMREC_CALL(BW_PRINT_GLYPH, job, sp, passTags, 0);
        }
    }
    return sp;
}
