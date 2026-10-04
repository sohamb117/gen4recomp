/*
 * The four ARM9 hardware timers (TM0..TM3), advanced one VBlank at a time.
 *
 * Why: nothing in the port counted them, so every clock the game builds on a
 * timer stood still. Timer 3 is the game's own (src/timer.c, Timer_Start at
 * boot): play time, the Poketch Stopwatch and Kitchen Timer read it, so play
 * time stayed 0:00:00 forever (measured: a 20,000-frame run that won a gym
 * and saved still saved 0:00:00). Timer 0 is the SDK tick (OS_InitTick in
 * src/system.c): OS_GetTick, which seeds Mystery Gift personalities and times
 * the GBA migrator and Wi-Fi waits, was constant.
 *
 * The model keeps the port's one clock: time is VBlanks, as in pc_rtc.c, and
 * a VBlank is 355 dots x 263 lines x 6 ARM9 cycles = 560190 cycles of the
 * 33.513982 MHz bus. Each delivered VBlank adds those cycles to every enabled
 * timer at its prescaler (1, 64, 256 or 1024; count-up timers take the lower
 * timer's overflows), reloads on overflow, and raises the timer interrupt
 * through the handler the game registered (OS_GetIrqFunction), the way
 * OS_Halt raises VBlank. Deterministic: inputs are the frame count only.
 *
 * The IO window is plain RAM, so TMxCNT_L cannot be both the reload the game
 * writes and the counter it reads. The model keeps the counter itself and
 * stores it into TMxCNT_L every step; a value there that is not the one
 * stored last is a game write, i.e. a new reload. Enabling a timer (the
 * control's start bit going 0 -> 1) loads the counter from the reload, as
 * on hardware. Resolution is one frame: a read between VBlanks sees the
 * counter as of the last one.
 */
#include <nitro.h>

#define PC_TIMER_CYCLES_PER_VBLANK 560190u

static struct {
    u16 last_written; /* what the model last stored into TMxCNT_L */
    u16 reload;
    u16 counter;
    u8 running;
    u32 acc; /* cycles not yet worth a tick at this prescaler */
} sTimers[4];

static volatile u16 *pc_timer_reg(int t, int high)
{
    return (volatile u16 *)(0x04000100u + 4u * (u32)t + (high ? 2u : 0u));
}

static void pc_timer_irq(int t)
{
    u32 bit = OS_IE_TIMER0 << t;
    OSIrqFunction fn;

    if (!(reg_OS_IME & 1) || !(reg_OS_IE & bit)) return;
    fn = OS_GetIrqFunction(bit);
    if (fn == NULL) return;
    /* IF is read by OS_GetTick / Timer_GetCurrentTimestamp to tell a
     * pending overflow from a taken one, so it is set only while the
     * handler runs, which is when it is set on hardware too. */
    reg_OS_IF = bit;
    fn();
    reg_OS_IF = 0;
}

void pc_timers_step(void)
{
    static const u16 prescale[4] = { 1, 64, 256, 1024 };
    u32 overflows_below = 0;
    int t;

    for (t = 0; t < 4; t++) {
        u16 ctl = *pc_timer_reg(t, 1);
        u16 low = *pc_timer_reg(t, 0);
        u32 ticks, total, n = 0;

        if (low != sTimers[t].last_written) sTimers[t].reload = low;
        if (!(ctl & 0x80)) {
            sTimers[t].running = 0;
            sTimers[t].last_written = low;
            overflows_below = 0;
            continue;
        }
        if (!sTimers[t].running) {
            sTimers[t].running = 1;
            sTimers[t].counter = sTimers[t].reload;
            sTimers[t].acc = 0;
        }

        if (t > 0 && (ctl & 0x04)) {
            ticks = overflows_below; /* count-up: one tick per lower overflow */
        } else {
            u32 p = prescale[ctl & 3];
            sTimers[t].acc += PC_TIMER_CYCLES_PER_VBLANK;
            ticks = sTimers[t].acc / p;
            sTimers[t].acc %= p;
        }

        total = sTimers[t].counter + ticks;
        if (total >= 0x10000u) {
            u32 period = 0x10000u - sTimers[t].reload;
            n = 1 + (total - 0x10000u) / period;
            total = sTimers[t].reload + (total - 0x10000u) % period;
        }
        sTimers[t].counter = (u16)total;
        *pc_timer_reg(t, 0) = sTimers[t].counter;
        sTimers[t].last_written = sTimers[t].counter;
        overflows_below = n;

        if (ctl & 0x40) {
            while (n--) pc_timer_irq(t);
        }
    }
}
