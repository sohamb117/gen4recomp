/* SPDX-License-Identifier: GPL-3.0-or-later */
/* make_synth_save pt|dp|pt-corrupt <out.sav>: write a synthetic 512 KiB save
 * image. pt-corrupt flips a byte in both copies of the general block so no
 * valid copy remains. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synth_save.h"

int main(int argc, char **argv)
{
    const char *mode = argc == 3 ? argv[1] : "";
    int pt = !strcmp(mode, "pt"), dp = !strcmp(mode, "dp"), bad = !strcmp(mode, "pt-corrupt");
    if (!pt && !dp && !bad) {
        fprintf(stderr, "usage: %s pt|dp|pt-corrupt <out.sav>\n", argv[0]);
        return 2;
    }
    uint8_t *img = malloc(SAVE4_IMAGE_SIZE);
    if (!img)
        return 1;
    synth_save_build(img, dp ? SAVE4_GAME_DP : SAVE4_GAME_PT);
    if (bad) {
        img[0x100] ^= 0x5A;
        img[SAVE4_COPY_SIZE + 0x100] ^= 0x5A;
    }
    FILE *f = fopen(argv[2], "wb");
    if (!f || fwrite(img, 1, SAVE4_IMAGE_SIZE, f) != SAVE4_IMAGE_SIZE || fclose(f) != 0) {
        perror(argv[2]);
        free(img);
        return 1;
    }
    free(img);
    return 0;
}
