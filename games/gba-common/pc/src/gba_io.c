/*
 * GBA I/O: the register file at 0x04000000 (plain guest memory, so the PPU
 * and the game see the same bytes), the side effects of register accesses,
 * DMA, timers, interrupts and the scanline clock. See gba_port.h for the
 * timing model.
 */
#include <string.h>

#include "gba_port.h"

uint32_t gba_vcount;
uint64_t gba_frames;
uint64_t gba_cycles;
uint32_t gba_keys;

#define INTR_CHECK (*(uint16_t *)(uintptr_t)0x03007FF8u)

/* ------------------------------------------------------------- timers */

typedef struct {
    uint16_t reload;
    uint16_t counter;  /* value at `stamp` */
    uint64_t stamp;    /* gba_cycles when `counter` was taken */
    uint64_t frac;     /* leftover cycles below one tick */
} gba_timer;

static gba_timer timers[4];
static const uint8_t k_prescale[4] = {0, 6, 8, 10};

static uint16_t tm_cnt(int t) { return IO16(R_TM0CNT + 4 * t + 2); }

/* Brings timer t up to gba_cycles; returns how many times it overflowed. */
static uint32_t timer_advance(int t, uint32_t cascade_ticks) {
    gba_timer *tm = &timers[t];
    uint16_t cnt = tm_cnt(t);
    uint64_t ticks;
    if (!(cnt & 0x80)) {
        tm->stamp = gba_cycles;
        return 0;
    }
    if (t > 0 && (cnt & 4)) {
        ticks = cascade_ticks;
    } else {
        uint64_t elapsed = gba_cycles - tm->stamp + tm->frac;
        ticks = elapsed >> k_prescale[cnt & 3];
        tm->frac = elapsed - (ticks << k_prescale[cnt & 3]);
    }
    tm->stamp = gba_cycles;
    uint32_t period = 0x10000u - tm->reload;
    uint64_t pos = (uint64_t)tm->counter + ticks;
    uint32_t ov = 0;
    if (pos >= 0x10000u) {
        uint64_t over = pos - 0x10000u;
        ov = 1 + (uint32_t)(over / period);
        pos = tm->reload + over % period;
    }
    tm->counter = (uint16_t)pos;
    if (ov && (cnt & 0x40)) gba_raise_irq((uint16_t)(IRQ_TIMER0 << t));
    return ov;
}

static void timers_advance(void) {
    uint32_t ov = 0;
    for (int t = 0; t < 4; t++) ov = timer_advance(t, ov);
}

/* --------------------------------------------------------------- DMA */

typedef struct {
    uint32_t src, dst, count;
} gba_dma;

static gba_dma dmas[4];

static uint16_t dma_cnt(int ch) { return IO16(R_DMA0SAD + 12 * ch + 10); }

static void dma_latch(int ch) {
    uint32_t base = R_DMA0SAD + 12 * ch;
    dmas[ch].src = IO32(base) & (ch == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu);
    dmas[ch].dst = IO32(base + 4) & (ch == 3 ? 0x0FFFFFFFu : 0x07FFFFFFu);
    uint32_t n = IO16(base + 8) & (ch == 3 ? 0xFFFFu : 0x3FFFu);
    dmas[ch].count = n ? n : (ch == 3 ? 0x10000u : 0x4000u);
}

static int in_io(uint32_t a) { return (a >> 24) == 4 && (a & 0x00FFFFFFu) < 0x400; }

void gba_dma_run(int ch) {
    uint32_t base = R_DMA0SAD + 12 * ch;
    uint16_t cnt = dma_cnt(ch);
    uint32_t unit = (cnt & 0x400) ? 4 : 2;
    int dctl = (cnt >> 5) & 3, sctl = (cnt >> 7) & 3;
    gba_dma *d = &dmas[ch];
    uint32_t src = d->src & ~(unit - 1), dst = d->dst & ~(unit - 1);
    for (uint32_t i = 0; i < d->count; i++) {
        uint32_t v;
        if (in_io(src))
            v = unit == 4 ? (gba_io_read16(src & 0x3FE) | (uint32_t)gba_io_read16((src + 2) & 0x3FE) << 16)
                          : gba_io_read16(src & 0x3FE);
        else
            v = unit == 4 ? *(uint32_t *)(uintptr_t)src : *(uint16_t *)(uintptr_t)src;
        if (in_io(dst)) {
            gba_io_write16(dst & 0x3FE, (uint16_t)v);
            if (unit == 4) gba_io_write16((dst + 2) & 0x3FE, (uint16_t)(v >> 16));
        } else if (dst < GBA_ROM) {
            if (unit == 4)
                *(uint32_t *)(uintptr_t)dst = v;
            else
                *(uint16_t *)(uintptr_t)dst = (uint16_t)v;
        }
        src += sctl == 0 ? unit : sctl == 1 ? (uint32_t)-unit : 0;
        dst += dctl == 0 || dctl == 3 ? unit : dctl == 1 ? (uint32_t)-unit : 0;
    }
    d->src = src;
    d->dst = dst;
    if (cnt & 0x4000) gba_raise_irq((uint16_t)(IRQ_DMA0 << ch));
    int timing = (cnt >> 12) & 3;
    if ((cnt & 0x200) && timing != 0) {
        uint32_t n = IO16(base + 8) & (ch == 3 ? 0xFFFFu : 0x3FFFu);
        d->count = n ? n : (ch == 3 ? 0x10000u : 0x4000u);
        if (dctl == 3) d->dst = IO32(base + 4);
    } else {
        IO16(base + 10) = cnt & 0x7FFF;
    }
}

static void dma_timing(int timing) {
    for (int ch = 0; ch < 4; ch++) {
        uint16_t cnt = dma_cnt(ch);
        if ((cnt & 0x8000) && ((cnt >> 12) & 3) == timing) gba_dma_run(ch);
    }
}

/* -------------------------------------------------------- interrupts */

static int irq_depth;
/* crt0.s IntrMain's search order; gIntrTable is indexed by position */
static const uint16_t k_irq_order[14] = {
    IRQ_VCOUNT, IRQ_SERIAL, IRQ_TIMER3, IRQ_HBLANK, IRQ_VBLANK, IRQ_TIMER0, IRQ_TIMER0 << 1,
    IRQ_TIMER0 << 2, IRQ_DMA0, IRQ_DMA0 << 1, IRQ_DMA0 << 2, IRQ_DMA0 << 3, IRQ_KEYPAD, IRQ_GAMEPAK,
};

void gba_raise_irq(uint16_t bits) { IO16(R_IF) |= bits; }

void gba_check_irqs(void) {
    while ((IO16(R_IME) & 1) && irq_depth < 4) {
        uint16_t ie = IO16(R_IE), pending = ie & IO16(R_IF) & 0x3FFF;
        if (!pending) return;
        int idx = 0;
        while (!(pending & k_irq_order[idx])) idx++;
        uint16_t bit = k_irq_order[idx];
        if (bit == IRQ_GAMEPAK) {  /* cartridge pulled: IntrMain spins */
            IO16(R_IF) &= (uint16_t)~bit;
            continue;
        }
        IO16(R_IF) &= (uint16_t)~bit;
        uint16_t ime = IO16(R_IME);
        IO16(R_IME) = bit == IRQ_VCOUNT ? 0 : 1;
        IO16(R_IE) = ie & (uint16_t)~bit & (IRQ_GAMEPAK | IRQ_SERIAL | IRQ_TIMER3 | IRQ_VCOUNT | IRQ_HBLANK);
        uint32_t handler = *(uint32_t *)(uintptr_t)(gba_game.intr_table + 4 * idx);
        if (!gba_is_code(handler))
            gba_fatal("IRQ %d (IF bit 0x%x) has handler 0x%08x (gIntrTable 0x%08x)", idx, bit, handler,
                      gba_game.intr_table);
        irq_depth++;
        gba_call0(handler);
        irq_depth--;
        IO16(R_IE) = ie;
        IO16(R_IME) = ime;
    }
}

/* BIOS IntrWait: wait until the game's handler has set one of `flags` in
 * the BIOS check word (0x03007FF8), clearing them first if asked. */
void gba_wait_irq(uint16_t flags, int discard_old) {
    IO16(R_IME) = 1;
    if (discard_old) INTR_CHECK &= (uint16_t)~flags;
    for (;;) {
        if (INTR_CHECK & flags) {
            INTR_CHECK &= (uint16_t)~flags;
            return;
        }
        gba_step_line();
    }
}

/* ------------------------------------------------------------ the clock */

void gba_step_line(void) {
    uint32_t y = gba_vcount;
    uint16_t stat = IO16(R_DISPSTAT);
    if (y < GBA_H) {
        gba_ppu_line((int)y);
        dma_timing(2);
    }
    if (stat & 0x10) gba_raise_irq(IRQ_HBLANK);
    gba_check_irqs();

    y = (y + 1) % GBA_LINES;
    gba_vcount = y;
    gba_cycles += GBA_LINE_CYCLES;
    timers_advance();
    stat = IO16(R_DISPSTAT);
    if (y == GBA_H) {
        gba_frames++;
        gba_frame_end();
        gba_ppu_vblank();
        if (stat & 0x08) gba_raise_irq(IRQ_VBLANK);
        dma_timing(1);
    }
    if (y == (uint32_t)(stat >> 8) && (stat & 0x20)) gba_raise_irq(IRQ_VCOUNT);
    gba_check_irqs();
}

/* ----------------------------------------------------- register access */

static uint32_t poll_off = 0xFFFF;
static uint16_t poll_val;
static uint32_t hblank_toggle;

/* A value read eight times in a row means the game is waiting on the
 * clock (a sampler reads it once or twice): that read moves it a line on. */
static uint32_t poll_count;
static void poll(uint32_t off, uint16_t v) {
    if (off == poll_off && v == poll_val) {
        if (++poll_count >= 8) {
            gba_step_line();
            poll_off = 0xFFFF;
        }
    } else {
        poll_off = off;
        poll_val = v;
        poll_count = 0;
    }
}

uint16_t gba_io_read16(uint32_t off) {
    off &= 0x3FE;
    switch (off) {
    case R_VCOUNT: {
        uint16_t v = (uint16_t)gba_vcount;
        poll(off, v);
        return v;
    }
    case R_DISPSTAT: {
        uint16_t s = IO16(R_DISPSTAT) & 0xFFF8;
        if (gba_vcount >= GBA_H && gba_vcount < GBA_LINES - 1) s |= 1;
        if (++hblank_toggle & 1) s |= 2;
        if (gba_vcount == (uint32_t)(s >> 8)) s |= 4;
        poll(off, s & ~2);
        return s;
    }
    case R_KEYINPUT:
        return (uint16_t)(~gba_keys & 0x3FF);
    case R_TM0CNT: case R_TM0CNT + 4: case R_TM0CNT + 8: case R_TM0CNT + 12: {
        gba_cycles += 8;  /* a busy-read of a timer must see it move */
        timers_advance();
        return timers[(off - R_TM0CNT) >> 2].counter;
    }
    default:
        return IO16(off);
    }
}

void gba_io_write16(uint32_t off, uint16_t v) {
    off &= 0x3FE;
    switch (off) {
    case R_DISPSTAT:
        IO16(off) = (uint16_t)((IO16(off) & 7) | (v & 0xFFF8));
        return;
    case R_VCOUNT: case R_KEYINPUT:
        return;
    case R_BG2X: case R_BG2X + 2: case R_BG2Y: case R_BG2Y + 2:
        IO16(off) = v;
        gba_ppu_reload_affine(2);
        return;
    case R_BG3X: case R_BG3X + 2: case R_BG3Y: case R_BG3Y + 2:
        IO16(off) = v;
        gba_ppu_reload_affine(3);
        return;
    case R_DMA0SAD + 10: case R_DMA0SAD + 22: case R_DMA0SAD + 34: case R_DMA0SAD + 46: {
        int ch = (int)(off - R_DMA0SAD - 10) / 12;
        uint16_t old = IO16(off);
        IO16(off) = v;
        if (!(old & 0x8000) && (v & 0x8000)) {
            dma_latch(ch);
            if (((v >> 12) & 3) == 0) gba_dma_run(ch);
        }
        return;
    }
    case R_TM0CNT: case R_TM0CNT + 4: case R_TM0CNT + 8: case R_TM0CNT + 12:
        timers_advance();
        timers[(off - R_TM0CNT) >> 2].reload = v;
        IO16(off) = v;
        return;
    case R_TM0CNT + 2: case R_TM0CNT + 6: case R_TM0CNT + 10: case R_TM0CNT + 14: {
        int t = (int)(off - R_TM0CNT - 2) >> 2;
        timers_advance();
        uint16_t old = IO16(off);
        IO16(off) = v & 0xC7;
        if (!(old & 0x80) && (v & 0x80)) {
            timers[t].counter = timers[t].reload;
            timers[t].stamp = gba_cycles;
            timers[t].frac = 0;
        }
        return;
    }
    case R_IF:
        IO16(off) &= (uint16_t)~v;
        return;
    case R_FIFO_A: case R_FIFO_A + 2: case R_FIFO_B: case R_FIFO_B + 2:
        return;  /* the mixer's output is taken from its buffer (gba_m4a.c) */
    default:
        IO16(off) = v;
        if (off >= R_SOUND1CNT_L && off < R_FIFO_A) gba_apu_write(off, v);
        return;
    }
}

void gba_io_reset(void) {
    memset(GBA_PTR(GBA_IO), 0, 0x400);
    memset(timers, 0, sizeof timers);
    memset(dmas, 0, sizeof dmas);
    IO16(R_BG2PA) = 0x100;
    IO16(R_BG2PA + 6) = 0x100;
    IO16(R_BG3PA) = 0x100;
    IO16(R_BG3PA + 6) = 0x100;
    IO16(R_SOUNDBIAS) = 0x200;
    IO16(R_KEYINPUT) = 0x3FF;
    gba_vcount = 0;
    irq_depth = 0;
}

/* ------------------------------------------- the bridge's volatile hooks */

static int is_io(const void *p) {
    uint32_t a = (uint32_t)(uintptr_t)p;
    return (a >> 24) == 4;
}

uint8_t gba_vload8(const void *p) {
    if (!is_io(p)) return *(const uint8_t *)p;
    uint32_t a = (uint32_t)(uintptr_t)p;
    return (uint8_t)(gba_io_read16(a & 0x3FE) >> ((a & 1) * 8));
}

uint16_t gba_vload16(const void *p) {
    if (!is_io(p)) return *(const uint16_t *)p;
    return gba_io_read16((uint32_t)(uintptr_t)p & 0x3FE);
}

uint32_t gba_vload32(const void *p) {
    if (!is_io(p)) return *(const uint32_t *)p;
    uint32_t a = (uint32_t)(uintptr_t)p & 0x3FC;
    return gba_io_read16(a) | (uint32_t)gba_io_read16(a + 2) << 16;
}

void gba_vstore8(void *p, uint8_t v) {
    if (!is_io(p)) {
        *(uint8_t *)p = v;
        return;
    }
    uint32_t a = (uint32_t)(uintptr_t)p & 0x3FF;
    if (a == 0x301) return; /* HALTCNT: the game halts until an interrupt; lines run when it waits */
    int sh = (a & 1) * 8;
    uint32_t off = a & 0x3FE;
    if (off == R_IF) {
        gba_io_write16(off, (uint16_t)(v << sh));
        return;
    }
    uint16_t cur = IO16(off);
    gba_io_write16(off, (uint16_t)((cur & ~(0xFF << sh)) | (v << sh)));
}

void gba_vstore16(void *p, uint16_t v) {
    if (!is_io(p)) {
        *(uint16_t *)p = v;
        return;
    }
    gba_io_write16((uint32_t)(uintptr_t)p & 0x3FE, v);
}

void gba_vstore32(void *p, uint32_t v) {
    if (!is_io(p)) {
        *(uint32_t *)p = v;
        return;
    }
    uint32_t a = (uint32_t)(uintptr_t)p & 0x3FC;
    if (a >= R_DMA0SAD && a < R_DMA0SAD + 48 && ((a - R_DMA0SAD) % 12) < 8) {
        IO32(a) = v; /* source/destination: latched when the channel starts */
        return;
    }
    if (a == R_FIFO_A || a == R_FIFO_B) return;
    gba_io_write16(a, (uint16_t)v);
    gba_io_write16(a + 2, (uint16_t)(v >> 16));
}
