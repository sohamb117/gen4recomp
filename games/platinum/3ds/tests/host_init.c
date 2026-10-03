/*
 * 3ds/tests/host_init.c: the host layer's start-up order, on the host.
 *
 * 3ds/src/3ds_init.c is four weak references and two real ones, and the thing
 * worth checking about it is not that each step runs but that they run in
 * pc_main.c's order and that a failure stops the line. Both are host
 * questions: no libctru, no console, no guest code, one slab and six calls.
 *
 * The four pc/src steps are stubs here, because on this machine they are the
 * PC port's own files and linking them would drag in the DS SDK's include
 * chain for no gain: what is under test is the sequencer, not the responders.
 *
 * Two are not stubs. pc_input_init() is 3ds/src/3ds_input.c's, compiled and
 * linked for real, and that is what makes the position check below mean
 * something, the stubs on either side of it read KEYINPUT and see it
 * unwritten before and idled after, which is a claim no recorded call order
 * could make on its own. The renderer install cannot be stubbed at all: it
 * is a static wrapper inside 3ds_init.c, so it is in every link and only its
 * presence in the count is observable from here.
 *
 * Built twice by 3ds/tests/run.sh. With the stubs it is the game link, where
 * every step exists; with -DHOST_INIT_NO_STUBS it is the self-test .3dsx,
 * where the four pc/src names are absent and the weak references are NULL.
 * The second build is the one that would catch a weak reference turning
 * strong; it simply would not link.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "armrec_rt.h"

#include "armrec_mem_3ds.h"
#include "3ds_guest.h"
#include "3ds_init.h"
#include "3ds_input.h"

static int sRan;
static int sBad;

#define CHECK(what, cond)                                                  \
    do {                                                                   \
        sRan++;                                                            \
        if (!(cond)) {                                                     \
            printf("  %-56s FAILED\n", (what));                            \
            sBad++;                                                        \
        }                                                                  \
    } while (0)

#ifndef HOST_INIT_NO_STUBS

/* Which step, if any, is told to fail on this call of host_init(). */
static const char *sFailStep;

/* The order the sequencer actually used, as one string. */
static char sOrder[64];

/* KEYINPUT as the step on either side of pc_input_init() saw it. */
static uint16_t sKeysAtRom;
static uint16_t sKeysAtRtc;

static uint16_t keys_now(void)
{
    const uint16_t *p = (const uint16_t *)armrec_host_ptr(INPUT_KEYS_ADDR);

    return p != NULL ? *p : 0xFFFFu;
}

static int step(const char *name)
{
    if (sOrder[0] != '\0') {
        strncat(sOrder, ",", sizeof sOrder - strlen(sOrder) - 1);
    }
    strncat(sOrder, name, sizeof sOrder - strlen(sOrder) - 1);
    return (sFailStep != NULL && strcmp(sFailStep, name) == 0) ? -1 : 0;
}

int pc_rom_init(void)
{
    sKeysAtRom = keys_now();
    return step("rom");
}

int pc_rtc_init(void)
{
    sKeysAtRtc = keys_now();
    return step("rtc");
}

int pc_wvr_init(void)
{
    return step("wvr");
}

int pc_snd_init(void)
{
    return step("snd");
}

/* Back to a reset console: the sequencer's own state is reset by host_init(),
 * but KEYINPUT is not, and the position check needs it unwritten again. */
static void reset(const char *failStep)
{
    uint16_t *keys = (uint16_t *)armrec_host_ptr(INPUT_KEYS_ADDR);

    sOrder[0] = '\0';
    sKeysAtRom = 0xFFFFu;
    sKeysAtRtc = 0xFFFFu;
    sFailStep = failStep;
    if (keys != NULL) {
        *keys = 0;
    }
}

static void run_all(void)
{
    static const char *const kStopAt[] = { "rom", "rtc", "wvr", "snd" };
    static const char *const kUpTo[] = {
        "rom", "rom,rtc", "rom,rtc,wvr", "rom,rtc,wvr,snd"
    };
    unsigned i;

    reset(NULL);
    CHECK("host_init() runs every step", host_init() == 0);
    CHECK("all seven steps are in this link", host_init_ran() == 7);
    CHECK("nothing was skipped", host_init_skipped() == 0);
    CHECK("no step is named as failing", host_init_failed() == NULL);
    CHECK("pc_main.c's order, for the four pc/src steps",
          strcmp(sOrder, "rom,rtc,wvr,snd") == 0);

    /* The real pc_input_init() ran, and it ran between rom and rtc. */
    CHECK("rom saw KEYINPUT unwritten", sKeysAtRom == 0x0000u);
    CHECK("rtc saw KEYINPUT idled by pc_input_init()",
          sKeysAtRtc == INPUT_KEYS_MASK);
    CHECK("KEYINPUT is still idle after the sequence",
          keys_now() == INPUT_KEYS_MASK);

    /* A failure stops the line, and says which step it was. */
    for (i = 0; i < sizeof kStopAt / sizeof kStopAt[0]; i++) {
        reset(kStopAt[i]);
        CHECK("a failed step makes host_init() return -1", host_init() == -1);
        CHECK("the failing step names itself",
              host_init_failed() != NULL
                  && strcmp(host_init_failed(), kStopAt[i]) == 0);
        CHECK("no step after the failed one ran",
              strcmp(sOrder, kUpTo[i]) == 0);
    }

    /*
     * And the other return convention. pc_input_init() reports success as 1,
     * not 0, and the only way to make it fail is to take guest memory away;
     * which is also the real failure it is there to catch. Nothing after it
     * may run: the two steps that would follow allocate out of a slab that is
     * not bound.
     */
    reset(NULL);
    guest_bind(NULL);
    CHECK("pc_input_init()'s 1-is-success convention is honoured",
          host_init() == -1);
    CHECK("the input step names itself",
          host_init_failed() != NULL
              && strcmp(host_init_failed(), "input") == 0);
    CHECK("rtc, wvr and snd did not run after it", strcmp(sOrder, "rom") == 0);
}

#else /* HOST_INIT_NO_STUBS: the self-test .3dsx's link */

static void run_all(void)
{
    CHECK("host_init() runs with four steps absent", host_init() == 0);
    CHECK("pc_input_init() and the two renderer steps were there",
          host_init_ran() == 3);
    CHECK("the other four were counted as skipped", host_init_skipped() == 4);
    CHECK("nothing is named as failing", host_init_failed() == NULL);
}

#endif

int main(void)
{
    if (armrec_mem_init() != 0) {
        printf("host_init: init failed: %s\n", armrec_mem_strerror());
        return 2;
    }

    run_all();

    armrec_mem_free();
    printf("host_init: %d checks, %d failed\n", sRan, sBad);
    return sBad != 0;
}
