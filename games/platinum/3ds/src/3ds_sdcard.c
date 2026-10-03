/*
 * 3ds/src/3ds_sdcard.c: see 3ds_sdcard.h.
 *
 * Lifted whole from 3ds_snd_watch.c, which had the only copy while there was
 * only one report to write.
 */

#include "3ds_sdcard.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/*
 * Every directory on the way to `path`, made if it is not there. sd_open_write
 * calls it only after an fopen has already failed, so its normal path costs
 * nothing; the save image calls it once, before its first open.
 */
void sd_make_dirs(const char *path)
{
    char buf[128];
    size_t i;

    if (strlen(path) >= sizeof buf) {
        return;
    }
    strcpy(buf, path);

    /* One mkdir per slash, over the prefix that ends there, so the last
     * component, which is the file, is never made. A prefix ending in ':' is
     * the device itself ("sdmc:") and is not a directory anything can make. */
    for (i = 1; buf[i] != '\0'; i++) {
        if (buf[i] != '/') {
            continue;
        }
        buf[i] = '\0';
        if (buf[i - 1] != ':') {
            (void)mkdir(buf, 0777);
        }
        buf[i] = '/';
    }
}

FILE *sd_open_write(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        sd_make_dirs(path);
        f = fopen(path, "w");
    }
    return f;
}
