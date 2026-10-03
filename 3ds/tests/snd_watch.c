/*
 * 3ds/tests/snd_watch.c: the silent-channel check, on the host.
 *
 * The checks are snd_watch_selftest()'s, the same ones the 3dsx runs over its
 * own translator. What this adds is the report: the console's only way of
 * saying what it saw is a file on the SD card, so the file is written here and
 * read back, because a check whose verdict never leaves the console is not a
 * check.
 *
 * The spu's log is defined here. On the console pc/hw/pc_spu.c owns these
 * three names and fills them from real keyons. Supplying them here is what
 * lets the classification be driven over addresses chosen on purpose,
 * including a host pointer put through SOUNDxSAD's mask, which is the failure
 * the whole check exists to catch and which a boot is not going to produce to
 * order.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "3ds_guest.h"
#include "3ds_guest_map.h"
#include "3ds_snd_watch.h"

/* pc/hw/pc_spu.c's, for this link. The bound is its PC_SPU_SRC_LOG. */
uint32_t pc_spu_src_log[1024][2];
uint32_t pc_spu_src_log_n;
uint32_t pc_spu_src_log_dropped;
uint32_t pc_spu_unreadable[2];
uint32_t pc_spu_unreadable_first[2];
uint32_t pc_spu_unread_log[16][4];
uint32_t pc_spu_unread_log_n;
uint32_t pc_spu_keyons[2];
uint64_t pc_spu_latched;
unsigned long pc_arm7snd_steps;
unsigned long pc_arm7snd_commands;
uint32_t pc_arm7snd_cmdlog[8];
uint32_t pc_arm7snd_arglog[6][5];
unsigned long pc_arm7snd_arglog_n;
void *arm7_SNDi_SharedWork;

uint64_t pc_spu_samples(void)
{
    return 0;
}

/* armrec_rt_3ds.c's, which this link does not take: it pulls in libctru.
 * The two geometry counts are pc/hw's, which it does not take either. */
unsigned long armrec_gx_discarded(void)
{
    return 0;
}

uint32_t pc_gpu3d_num_polygons(void)
{
    return 0;
}

uint32_t pc_gpu3d_soft_polygons_drawn(void)
{
    return 0;
}

/* One `key value` line out of the report, or NULL. */
static const char *field(const char *text, const char *key, char *buf,
                         size_t size)
{
    const char *at = text;
    size_t klen = strlen(key);

    while (at != NULL && *at != '\0') {
        if (strncmp(at, key, klen) == 0 && at[klen] == ' ') {
            const char *v = at + klen + 1;
            const char *end = strchr(v, '\n');
            size_t n = (end != NULL ? (size_t)(end - v) : strlen(v));

            if (n >= size) {
                n = size - 1;
            }
            memcpy(buf, v, n);
            buf[n] = '\0';
            return buf;
        }
        at = strchr(at, '\n');
        if (at != NULL) {
            at++;
        }
    }
    return NULL;
}

static int report_says(const char *path, const char *key, const char *want)
{
    char text[2048];
    char got[128];
    FILE *f = fopen(path, "r");
    size_t n;

    if (f == NULL) {
        printf("snd_watch: no report at %s\n", path);
        return 0;
    }
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';

    if (field(text, key, got, sizeof got) == NULL) {
        printf("snd_watch: the report has no `%s' line\n", key);
        return 0;
    }
    if (strcmp(got, want) != 0) {
        printf("snd_watch: report %s is `%s', wanted `%s'\n", key, got, want);
        return 0;
    }
    return 1;
}

int main(void)
{
    const char *path = "build/3ds/snd-report.txt";
    void *slab;
    int ran = 0;
    int failed;

    if (posix_memalign(&slab, 0x1000, GUEST_SLAB_BYTES) != 0 || slab == NULL) {
        printf("snd_watch: no %u bytes for a slab\n", (unsigned)GUEST_SLAB_BYTES);
        return 2;
    }
    guest_bind(slab);

    failed = snd_watch_selftest(&ran);
    printf("snd_watch: %d checks, %d failed\n", ran, failed);

    /*
     * The report, both ways round. A boot that never keys a channel has to
     * say so rather than saying nothing, an absent file and a passing file
     * look the same to a test that only greps for FAIL.
     */
    snd_watch_reset();
    snd_watch_set_path(path);
    remove(path);

    /* The first game frame writes whatever it has, so the file exists on a
     * run that never keys a channel. That is the case a grep for FAIL cannot
     * tell apart from a pass. */
    snd_watch_step();
    failed += !report_says(path, "verdict", "NONE");
    failed += !report_says(path, "keyons", "0");

    pc_spu_src_log[0][0] = (uint32_t)GUEST_BASE_WINDOW + 0x40u;
    pc_spu_src_log[0][1] = 0x800u;
    pc_spu_src_log_n = 1;
    snd_watch_step();
    if (snd_watch_write(path) != 0) {
        printf("snd_watch: could not rewrite %s\n", path);
        failed++;
    } else {
        failed += !report_says(path, "verdict", "PASS");
        failed += !report_says(path, "window", "1");
        failed += !report_says(path, "first-window", "02A00040 2048");
    }

    pc_spu_src_log[1][0] = (uint32_t)GUEST_BASE_MAIN;
    pc_spu_src_log[1][1] = 0x800u;
    pc_spu_src_log_n = 2;
    snd_watch_step();
    if (snd_watch_write(path) != 0) {
        printf("snd_watch: could not rewrite %s\n", path);
        failed++;
    } else {
        failed += !report_says(path, "verdict", "FAIL");
        failed += !report_says(path, "stray", "1");
        failed += !report_says(path, "first-bad", "02000000");
    }

    /*
     * And the directory. A console that has always launched this port from
     * its own RomFS has never had an sdmc:/3ds/pokeplatinum made for it, and
     * an fopen does not make one, so the report would go nowhere on exactly
     * the setup most likely to be running.
     */
    {
        const char *deep = "build/3ds/snd-watch-dir/report.txt";

        remove(deep);
        rmdir("build/3ds/snd-watch-dir");
        if (snd_watch_write(deep) != 0) {
            printf("snd_watch: %s was not made on the way to the file\n", deep);
            failed++;
        }
        remove(deep);
        rmdir("build/3ds/snd-watch-dir");
    }

    if (failed == 0) {
        printf("  %-56s ok\n", "keyed source addresses, and the report");
    }

    guest_bind(NULL);
    free(slab);
    return failed != 0;
}
