/*
 * The ARM7 sound driver's host , 
 *
 * pc/arm7snd/ is pokediamond's decompiled SND driver, compiled here with
 * every symbol renamed into the arm7_ namespace (pc/Makefile's SND7NS
 * pass). This file is everything the renamed objects import and the pump
 * that stands in for SndThread: on hardware the driver runs on its own
 * processor, woken 192 times a second by a periodic OS alarm and by PXI
 * words from the ARM9. Here the same loop body runs on the ARM9's frame
 * clock (pc_arm7snd_frame() at each VBlank delivery) and immediately
 * when a command list arrives, which is the "finished before you looked"
 * schedule every SDK caller already tolerates (the same argument
 * pc_dma.c makes for DMA).
 *
 * What the shims are not: a second OS. The driver's OS surface in use is
 * tiny, one non-blocking message queue (the command mailbox), interrupt
 * enable/disable brackets, an alarm clock for SND alarms, two PMIC amp
 * bits, the two BIOS sound tables and the SOUNDBIAS ramp. Each shim is
 * the smallest truthful model of exactly that surface; anything the
 * driver could ask for beyond it stays a link error rather than a stub.
 *
 * TIME. One 192 Hz driver tick per 2728 sound ticks (the SDK's own
 * periodic-alarm interval; ~5.209 ms of the 33.51 MHz clock through the
 * /64 prescaler). A frame is 3.2094 ticks, accumulated in fixed point so
 * a long run neither gains nor loses ticks against the frame clock. All
 * of it is guest time: no host clock is consulted anywhere.
 */

#include <nitro/types.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * The three registers this file reaches directly, SOUNDBIAS, and the SPU
 * page the heartbeat sums, are guest addresses, and a guest address is a
 * host address only on the PC port. On the 3DS the I/O page is a row of the
 * guest slab, so the base is a pointer set once the slab exists. Same store
 * either way; the offset arithmetic is the DS's on both.
 */
#if defined(__3DS__)
extern unsigned char *armrec_io_base;
#define A7SND_IO(a) ((void *)(armrec_io_base + ((unsigned)(a) - 0x04000000u)))
#else
#define A7SND_IO(a) ((void *)(uintptr_t)(a))
#endif

/* ---------------------------------------------------------------- the
 * driver's entry points (renamed objects) */
extern void arm7_SND_CommandInit(void);
extern void arm7_SND_CommandProc(void);
extern void arm7_SND_ExChannelInit(void);
extern void arm7_SND_SeqInit(void);
extern void arm7_SND_AlarmInit(void);
extern void arm7_SND_Enable(void);
extern void arm7_SND_SetOutputSelector(int l, int r, int c1, int c3);
extern void arm7_SND_SetMasterVolume(int vol);
extern void arm7_SND_UpdateExChannel(void);
extern void arm7_SND_SeqMain(BOOL step);
extern void arm7_SND_ExChannelMain(BOOL step);
extern void arm7_SND_UpdateSharedWork(void);
extern u32 arm7_SND_CalcRandom(void);

/* ---------------------------------------------------------------- host
 * functions the shims forward to */
extern u32 OS_DisableInterrupts(void);
extern u32 OS_RestoreInterrupts(u32 mode);
extern void MIi_CpuCopy32(const void *src, void *dest, u32 size);
extern void pc_pxi_reply(int tag, u32 data);
extern uint32_t pc_snd_pitch_table(int index);
extern uint32_t pc_snd_volume_table(int index);

#define SND_TICKS_PER_STEP 2728u
/* 192 Hz steps per frame in 1/10000ths: 2728-tick steps against the
 * 560,190-cycle frame the port's clock uses = 3.2094 steps a frame. */
#define STEPS_PER_FRAME_E4 32094u

/* ---------------------------------------------------------------- the
 * command mailbox (OS_message.c's surface, one queue, non-blocking) */

#define SND_MAILBOX 16

static u32 mailbox[SND_MAILBOX];
static int mb_head, mb_count;

void arm7_OS_InitMessageQueue(void *mq, void *array, int size)
{
    (void)mq; (void)array; (void)size;
    mb_head = mb_count = 0;
}

BOOL arm7_OS_SendMessage(void *mq, void *msg, s32 flags)
{
    (void)mq; (void)flags;
    if (mb_count == SND_MAILBOX) {
        return FALSE;
    }
    mailbox[(mb_head + mb_count) % SND_MAILBOX] = (u32)(uintptr_t)msg;
    mb_count++;
    return TRUE;
}

BOOL arm7_OS_ReceiveMessage(void *mq, u32 *msg, s32 flags)
{
    (void)mq; (void)flags;
    if (mb_count == 0) {
        return FALSE;
    }
    *msg = mailbox[mb_head];
    mb_head = (mb_head + 1) % SND_MAILBOX;
    mb_count--;
    return TRUE;
}

/* ---------------------------------------------------------------- OS glue */

u32 arm7_OS_DisableInterrupts(void) { return OS_DisableInterrupts(); }
u32 arm7_OS_RestoreInterrupts(u32 m) { return OS_RestoreInterrupts(m); }
void arm7_OS_SpinWait(u32 cycles) { (void)cycles; }

void arm7_MIi_CpuCopy32(const void *src, void *dest, u32 size)
{
    MIi_CpuCopy32(src, dest, size);
}

/* The PMIC amp enable/mute bits. pc_pm.c models the register file the
 * funnel writes; the driver's two calls bracket SND_Enable/Disable and
 * nothing here reads them back, so the truthful model is "accepted". */
void arm7_PMi_SetControl(u32 bit) { (void)bit; }
void arm7_PMi_ResetControl(u32 bit) { (void)bit; }

/* ---------------------------------------------------------------- time:
 * The sound tick and the SND alarm clock */

static uint64_t snd_ticks;

u64 arm7_OS_GetTick(void) { return snd_ticks; }

/* SND_alarm.c drives one OSAlarm per SND alarm slot (8 of them). The
 * registry is keyed by the OSAlarm pointer inside the driver's own
 * struct; the handler fires from the pump at tick granularity, which is
 * the resolution the driver itself schedules at. */
struct snd_alarm {
    void *key;
    uint64_t fire;
    uint64_t period;              /* 0: one-shot */
    void (*handler)(void *);
    void *arg;
    int live;
};

#define SND_ALARMS 8
static struct snd_alarm alarms[SND_ALARMS];

static struct snd_alarm *alarm_slot(void *key, int make)
{
    int i, free_i = -1;
    for (i = 0; i < SND_ALARMS; i++) {
        if (alarms[i].live && alarms[i].key == key) return &alarms[i];
        if (!alarms[i].live && free_i < 0) free_i = i;
    }
    if (!make || free_i < 0) return NULL;
    alarms[free_i].key = key;
    alarms[free_i].live = 1;
    return &alarms[free_i];
}

void arm7_OS_CreateAlarm(void *alarm)
{
    struct snd_alarm *a = alarm_slot(alarm, 1);
    if (a != NULL) a->handler = NULL;
}

void arm7_OS_CancelAlarm(void *alarm)
{
    struct snd_alarm *a = alarm_slot(alarm, 0);
    if (a != NULL) a->live = 0;
}

void arm7_OS_SetAlarm(void *alarm, u64 tick, void (*handler)(void *), void *arg)
{
    struct snd_alarm *a = alarm_slot(alarm, 1);
    if (a == NULL) return;
    a->fire = snd_ticks + tick;
    a->period = 0;
    a->handler = handler;
    a->arg = arg;
}

void arm7_OS_SetPeriodicAlarm(void *alarm, u64 start, u64 period,
                              void (*handler)(void *), void *arg)
{
    struct snd_alarm *a = alarm_slot(alarm, 1);
    if (a == NULL) return;
    a->fire = start;
    a->period = period;
    a->handler = handler;
    a->arg = arg;
}

static void alarms_advance(void)
{
    int i;
    for (i = 0; i < SND_ALARMS; i++) {
        struct snd_alarm *a = &alarms[i];
        while (a->live && a->handler != NULL && a->fire <= snd_ticks) {
            if (a->period != 0) {
                a->fire += a->period;
            } else {
                a->live = 0;
            }
            a->handler(a->arg);
        }
    }
}

/* ---------------------------------------------------------------- PXI */

static void (*snd_recv_cb)(int tag, u32 data, int err);

void arm7_PXI_SetFifoRecvCallback(s32 tag, void (*cb)(int, u32, int))
{
    (void)tag;                     /* only the sound tag comes here */
    snd_recv_cb = cb;
}

s32 arm7_PXI_SendWordByFifo(s32 tag, u32 data, BOOL err)
{
    (void)err;
    pc_pxi_reply(tag, data);
    return 0;
}

/* ---------------------------------------------------------------- BIOS */

u32 arm7_SVC_GetPitchTable(int idx) { return pc_snd_pitch_table(idx); }
u32 arm7_SVC_GetVolumeTable(int idx) { return pc_snd_volume_table(idx); }

/* SOUNDBIAS ramps to half-amplitude on set, to zero on reset; the step
 * count is how slowly hardware slews it, unobservable at frame
 * granularity. pc_spu reads the register for its output clamp. */
void arm7_SVC_SoundBiasSet(int step)
{
    (void)step;
    *(volatile u16 *)A7SND_IO(0x04000504) = 0x200;
}

void arm7_SVC_SoundBiasReset(int step)
{
    (void)step;
    *(volatile u16 *)A7SND_IO(0x04000504) = 0;
}

/* ---------------------------------------------------------------- what
 * SND_main.c's thread scaffolding provided (the file is not compiled;
 * this is its whole remaining surface) */

static int interval_on;
static int wake_pending;

void arm7_SNDi_LockMutex(void) { }
void arm7_SNDi_UnlockMutex(void) { }
void arm7_SND_StartIntervalTimer(void) { interval_on = 1; }
void arm7_SND_StopIntervalTimer(void) { interval_on = 0; }
void arm7_SND_SendWakeupMessage(void) { wake_pending = 1; }

/* ---------------------------------------------------------------- the pump */

/*
 * How much of the driver has actually run. Both are for a port that has come
 * up silent and has no stderr to ask; the 3DS reads them into its own
 * report (3ds/src/3ds_snd_watch.c) and they separate the two silences that
 * look identical from the SPU end: a driver that is never pumped, and one that
 * is pumped but never asked for a sound.
 */
unsigned long pc_arm7snd_steps;
unsigned long pc_arm7snd_commands;

/*
 * ...and the first few command words themselves, undereferenced. The whole
 * ARM9 boot sends three of these, so eight is the lot with room to spare, and
 * the raw word is the interesting part: it is the head of a command list, and
 * whether it arrives as a DS address or as a host pointer is the question a
 * port with a translator has to be able to ask. Not dereferenced here for the
 * same reason, on a console where the two are different, following the wrong
 * one is a fault.
 */
#define PC_ARM7SND_CMDLOG 8
u32 pc_arm7snd_cmdlog[PC_ARM7SND_CMDLOG];

/*
 * ...and the first few commands the lists carry, as `id` and the four
 * arguments. Two of those arguments are pointers, the sequence data and the
 * bank, and on a port whose guest memory is not identity-mapped their
 * spelling is the difference between a sequencer that plays and one that
 * reads noise and produces nothing. Recorded rather than printed because the
 * console this matters on has no stderr that reaches anywhere.
 */
#define PC_ARM7SND_ARGLOG 6
u32 pc_arm7snd_arglog[PC_ARM7SND_ARGLOG][5];
unsigned long pc_arm7snd_arglog_n;

static void snd_step(BOOL update)
{
    pc_arm7snd_steps++;
    arm7_SND_UpdateExChannel();
    arm7_SND_CommandProc();
    arm7_SND_SeqMain(update);
    arm7_SND_ExChannelMain(update);
    arm7_SND_UpdateSharedWork();
    (void)arm7_SND_CalcRandom();
}

/* SndThread's init sequence, minus the thread. The bias ramp is the one
 * piece that lived OUTSIDE the driver on hardware, NitroSpMain raises
 * SOUNDBIAS at ARM7 boot before SND_Init, and without it every mixed
 * zero sits at the negative rail (measured: a WAV of silence at
 * full-scale minus). */
void pc_arm7snd_init(void)
{
    arm7_SVC_SoundBiasSet(0x100);
    arm7_SND_CommandInit();
    arm7_SND_ExChannelInit();
    arm7_SND_SeqInit();
    arm7_SND_AlarmInit();
    arm7_SND_Enable();
    arm7_SND_SetOutputSelector(0, 0, 0, 0);
    arm7_SND_SetMasterVolume(127);
    interval_on = 1;
}

/* A word from the ARM9 (pc_snd.c's tag-7 responder): deliver through the
 * driver's own PXI callback (which mails the list) then run the loop
 * body once with no sequencer step, exactly what a wakeup message did.
 * PC_SND_TRACE=1 prints each list's command ids as they arrive. */
void pc_arm7snd_receive(u32 data)
{
    static int trace = -1;

    if (pc_arm7snd_commands < PC_ARM7SND_CMDLOG) {
        pc_arm7snd_cmdlog[pc_arm7snd_commands] = data;
    }
    pc_arm7snd_commands++;

    /* The list, walked exactly as SND_CommandProc walks it: cmd[0] is the
     * next link and cmd[1] the id. Safe to follow because the ARM9 built the
     * list in its own memory and hands the driver its own pointer, measured
     * on both ports, where the words are 0x0061DDE0 and 0x02xxxxxx
     * respectively and each is a pointer where it was produced. */
    if (data != 0) {
        const u32 *cmd = (const u32 *)(uintptr_t)data;

        while (cmd != NULL && pc_arm7snd_arglog_n < PC_ARM7SND_ARGLOG) {
            u32 *row = pc_arm7snd_arglog[pc_arm7snd_arglog_n++];
            int i;

            for (i = 0; i < 5; i++) {
                row[i] = cmd[1 + i];
            }
            cmd = (const u32 *)(uintptr_t)cmd[0];
        }
    }

    if (trace < 0) {
        extern char *getenv(const char *);
        trace = getenv("PC_SND_TRACE") != NULL;
    }
    if (trace && data != 0) {
        const u32 *cmd = (const u32 *)(uintptr_t)data;
        fprintf(stderr, "pc-snd: list");
        while (cmd != NULL) {
            fprintf(stderr, " [id=%u %08x %08x %08x %08x]",
                    (unsigned)cmd[1], (unsigned)cmd[2], (unsigned)cmd[3],
                    (unsigned)cmd[4], (unsigned)cmd[5]);
            cmd = (const u32 *)(uintptr_t)cmd[0];
        }
        fprintf(stderr, "\n");
    }

    if (snd_recv_cb != NULL) {
        snd_recv_cb(7, data, 0);
    }
    wake_pending = 0;
    snd_step(FALSE);
}

/* The 192 Hz cadence, batched at the frame boundary. */
void pc_arm7snd_frame(void)
{
    static uint32_t acc_e4;
    static uint32_t frames;

    /* PC_SND_TRACE: a register heartbeat, did the driver ever reach the
     * SPU page, plus the shared-work status words the sequencer keeps. */
    {
        static int trace = -1;
        if (trace < 0) {
            extern char *getenv(const char *);
            trace = getenv("PC_SND_TRACE") != NULL;
        }
        if (trace && (++frames % 300) == 0) {
            const uint8_t *p = (const uint8_t *)A7SND_IO(0x04000400u);
            uint32_t sum = 0;
            unsigned i;
            for (i = 0; i < 0x120; i++) sum += p[i];
            fprintf(stderr, "pc-snd: hb f=%u pagesum=%08x soundcnt=%02x%02x "
                            "bias=%04x ticks=%llu\n",
                    (unsigned)frames, (unsigned)sum,
                    p[0x101], p[0x100],
                    *(volatile uint16_t *)A7SND_IO(0x04000504u),
                    (unsigned long long)snd_ticks);
        }
    }

    if (wake_pending) {
        wake_pending = 0;
        snd_step(FALSE);
    }

    acc_e4 += STEPS_PER_FRAME_E4;
    while (acc_e4 >= 10000u) {
        acc_e4 -= 10000u;
        snd_ticks += SND_TICKS_PER_STEP;
        alarms_advance();
        if (interval_on) {
            snd_step(TRUE);
        }
    }
}
