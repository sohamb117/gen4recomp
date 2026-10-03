/*
 * Sound-system surface that must answer before an SPU exists.
 *
 * The mixer, channels and capture unit live on the ARM7/SPU side; sound
 * commands already leave over the (modeled, dropping) PXI FIFO. What
 * cannot be dropped is a call that computes on hardware-derived state:
 * NNSi_SndCaptureStart divides by an interval that only means something
 * when a capture unit is producing samples, and with none it arrives as
 * zero, the measured first sound wall (SIGFPE, capture.c).
 *
 * Refusing the capture is the truthful model: the function's own FALSE
 * return is "capture could not start", the reverb setup above it hands
 * that failure back, and the game runs without reverb, exactly what a
 * DS would do if its capture unit refused. The rest of sound stays on
 * the SDK's compiled paths until a real audio layer lands.
 */
#include <nnsys/snd/capture.h>

#include <nitro/pxi.h>
#include <nitro/snd/common/command.h>
#include <nitro/snd/common/work.h>

#include <stdio.h>

/*
 * The real sound driver, the tag-7 responder, registered from
 * pc_main.c beside the RTC's.
 *
 * The ARM9's snd library queues SNDCommand lists and sends each list's
 * head pointer over PXI tag 7. The ARM7 driver put pokediamond's decompiled
 * ARM7 driver behind this tag (pc/arm7snd/, arm7_-namespaced;
 * pc/src/pc_arm7snd.c is its host), so a word here goes to the driver's
 * own PxiFifoCallback and the driver's own SND_CommandProc consumes the
 * list and advances SNDi_SharedWork->finishCommandTag, the SDK's real
 * acknowledgement, produced by the SDK's real code. A zero word is the
 * process-queued-lists poke and runs the loop body the same way.
 *
 * The silent model this replaced consumed lists and bumped the tag by
 * hand; its seam (this responder) is exactly where the driver now sits.
 */
static void snd_respond(u32 data)
{
    extern void pc_arm7snd_receive(u32 data);

    static int sAnnounced;

    if (!sAnnounced) {
        sAnnounced = 1;
        fprintf(stderr, "pc_snd: ARM7 sound driver is live behind PXI "
                        "tag 7 (pc/arm7snd)\n");
    }

    pc_arm7snd_receive(data);
}

int pc_snd_init(void)
{
    extern void pc_pxi_set_responder(int tag, void (*fn)(u32 data));
    extern void pc_arm7snd_init(void);
    extern int pc_audio_open_dump(const char *path);
    extern char *getenv(const char *);
    const char *dump;

    pc_arm7snd_init();
    pc_pxi_set_responder(PXI_FIFO_TAG_SOUND, snd_respond);

    /* PC_DUMP_AUDIO=path: the frame boundary's mixer output as a WAV
     * (pc_audio.c). */
    dump = getenv("PC_DUMP_AUDIO");
    if (dump != NULL && dump[0] != '\0') {
        (void)pc_audio_open_dump(dump);
    }
    return 0;
}

BOOL NNSi_SndCaptureStart(NNSSndCaptureType type, void *buffer0,
                          void *buffer1, u32 bufLen,
                          NNSSndCaptureFormat format, SNDCaptureIn input,
                          SNDCaptureOut output, BOOL loopFlag,
                          int sampleRate, int volume, int pan0, int pan1,
                          int interval, NNSSndCaptureCallback callback,
                          void *arg)
{
    (void)type; (void)buffer0; (void)buffer1; (void)bufLen; (void)format;
    (void)input; (void)output; (void)loopFlag; (void)sampleRate;
    (void)volume; (void)pan0; (void)pan1; (void)interval; (void)callback;
    (void)arg;
    return FALSE;
}
