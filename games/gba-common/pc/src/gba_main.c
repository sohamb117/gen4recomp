/*
 * The GBA guest's entry and its contract with the nativeplat runtime
 * (core/include/np_guest_abi.h): copy the player's ROM to 0x08000000,
 * apply the enabled packages' data patches (gba_mods.c), reset the
 * machine, run the game's AgbMain, and hand a frame to the host at every
 * VBlank (gba_frame_end, from the scanline clock).
 *
 * Soft reset (A+B+Start+Select, or the game's own) starts AgbMain again on
 * a fresh fiber, which retires the one that called SoftReset: the old call
 * stack is simply abandoned, as the console's SWI 0 abandons it.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gba_port.h"
#include "np_guest_abi.h"

#define AUDIO_RING 8192u

static np_frame_desc s_desc;
static uint32_t s_audio[AUDIO_RING];
static uint32_t s_audio_head;
static uint32_t s_qs_seq;   /* last request seen */
static uint32_t s_qs_cb2;   /* the game's callback2 while a quick save waits */

extern uint32_t gba_flash_dirty;
uint8_t *gba_flash_image(void);
uint32_t gba_flash_size(void);

void gba_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    np_host_log(buf, (uint32_t)(n < 0 ? 0 : n >= (int)sizeof buf ? (int)sizeof buf - 1 : n));
}

void gba_fatal(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    np_host_trap(buf, (uint32_t)(n < 0 ? 0 : n >= (int)sizeof buf ? (int)sizeof buf - 1 : n));
}

/* id of the last indirect call made (gbabridge.py stores it; the S records
 * of build/rse/<game>/bridge_report.txt name the calling function) */
uint32_t gba_icall_site;

void gba_bad_call(uint32_t addr) {
    gba_fatal("call to 0x%08x, which is not a compiled function (site 0x%08x, frame %llu, line %u)", addr,
              gba_icall_site, (unsigned long long)gba_frames, gba_vcount);
}

/* ------------------------------------------------------------- audio */

/* Stereo s16 frames into the ring the host drains. */
void gba_audio_push(int16_t l, int16_t r) {
    s_audio[s_audio_head & (AUDIO_RING - 1)] = (uint16_t)l | (uint32_t)(uint16_t)r << 16;
    s_audio_head++;
}

/* ------------------------------------------------------------ frames */

/* Runs once in place of the game's callback2 (main loop, field idle): the
 * game's own save, then the callback it replaced. */
static void quicksave_cb2(void) {
    uint32_t cb2 = s_qs_cb2;
    *gba_game.callback2 = cb2;
    s_qs_cb2 = 0;
    s_desc.status[NP_STAT_QUICKSAVE_RESULT] = gba_game.quicksave();
    s_desc.status[NP_STAT_QUICKSAVE_SEQ] = s_qs_seq;
    gba_call0(cb2);
}

void gba_frame_end(void) {
    s_desc.frame_lo = (uint32_t)gba_frames;
    s_desc.frame_hi = (uint32_t)(gba_frames >> 32);
    s_desc.audio_head = s_audio_head;
    s_desc.save_size = gba_flash_size();
    s_desc.save_image = (uint32_t)(uintptr_t)gba_flash_image();
    if (gba_flash_dirty) s_desc.save_dirty = 1;
    gba_flash_dirty = 0;
    gba_apu_frame();
    s_desc.audio_head = s_audio_head;
    gba_game.status(s_desc.status);
    gba_link_frame(s_desc.status);

    np_host_vblank(&s_desc);

    gba_keys = s_desc.in_keys & 0x3FF;
    /* a quick save is the game's own save, made from the field when the
     * player could open the start menu; refused anywhere else */
    if (s_desc.opt[NP_OPT_QUICKSAVE_SEQ] != s_qs_seq && !s_qs_cb2) {
        s_qs_seq = s_desc.opt[NP_OPT_QUICKSAVE_SEQ];
        if (s_desc.status[NP_STAT_FIELD_READY]) {
            s_qs_cb2 = *gba_game.callback2;
            *gba_game.callback2 = (uint32_t)(uintptr_t)quicksave_cb2;
        } else {
            s_desc.status[NP_STAT_QUICKSAVE_SEQ] = s_qs_seq;
            s_desc.status[NP_STAT_QUICKSAVE_RESULT] = NP_QS_REFUSED;
        }
    }
    if (s_desc.in_quit) exit(0);
}

uint32_t gba_option(uint32_t opt) { return opt < NP_OPT_COUNT ? s_desc.opt[opt] : 0; }

/* ------------------------------------------------------------ fibers */

#define FIBER_STACK (512u * 1024u)
static uint8_t s_stacks[2][FIBER_STACK] __attribute__((aligned(16)));
static uint32_t s_game_fiber, s_old_fiber;
static int s_next_stack;

static void run_game(void) {
    gba_io_reset();
    gba_apu_reset();
    gba_game.agb_main();
    gba_fatal("AgbMain returned");
}

void np_fiber_entry(uint32_t arg) {
    (void)arg;
    if (s_old_fiber) {
        np_host_fiber_destroy(s_old_fiber);
        s_old_fiber = 0;
    }
    run_game();
}

uint32_t gba_soft_resets;

void gba_soft_reset(void) {
    uint32_t self = np_host_fiber_self();
    uint8_t *stack = s_stacks[s_next_stack];
    gba_soft_resets++;
    s_next_stack ^= 1;
    /* the boot fiber (handle 1) is never destroyed, only left parked */
    s_old_fiber = self != 1 ? self : 0;
    s_game_fiber = np_host_fiber_create((uint32_t)(uintptr_t)(stack + FIBER_STACK), 0);
    np_host_fiber_switch(s_game_fiber);
    gba_fatal("soft reset: a retired fiber resumed");
}

/* ------------------------------------------------------------- boot */

int main(void) {
    uint32_t size = np_host_rom_size();
    if (size < 0x200 || size > NP_GBA_ROM_MAX) gba_fatal("ROM size %u is not a GBA cartridge", size);
    for (uint32_t off = 0; off < size; off += 0x100000) {
        uint32_t n = size - off < 0x100000 ? size - off : 0x100000;
        if (np_host_rom_read(off, GBA_PTR(GBA_ROM + off), n) != 0) gba_fatal("ROM read failed at 0x%x", off);
    }
    const char *code = (const char *)GBA_PTR(GBA_ROM + 0xAC);
    if (memcmp(code, gba_game.game_code, 4) != 0)
        gba_fatal("this is not %s: the cartridge's game code is %.4s, want %s", gba_game.name, code,
                  gba_game.game_code);
    gba_mods_apply(GBA_PTR(GBA_ROM), &size);

    s_desc.magic = NP_FRAME_MAGIC;
    s_desc.version = NP_GUEST_ABI_VERSION;
    s_desc.screen[0] = (uint32_t)(uintptr_t)gba_screen;
    s_desc.screen[1] = 0;
    s_desc.width = GBA_W;
    s_desc.height = GBA_H;
    s_desc.stride = GBA_W;
    s_desc.audio_ring = (uint32_t)(uintptr_t)s_audio;
    s_desc.audio_ring_frames = AUDIO_RING;
    s_desc.audio_rate = 32768;
    gba_flash_boot();
    run_game();
    return 0;
}
