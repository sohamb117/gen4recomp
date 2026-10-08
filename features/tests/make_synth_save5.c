/* SPDX-License-Identifier: GPL-3.0-or-later */
/* make_synth_save5 black|white|black-corrupt|black-recover <out.sav>: write a
 * synthetic 512 KiB Black/White save image. black-corrupt flips a byte of the
 * trainer block in both copies (no valid copy remains); black-recover only in
 * the backup (the primary is used).
 * make_synth_save5 pgf <type> <id> <out.pgf>: write a Wonder Card. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synth_save5.h"

static int write_out(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, len, f) != len || fclose(f) != 0) {
        perror(path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "pgf")) {
        uint8_t card[SAVE5_PGF_SIZE];
        synth5_make_pgf(card, (uint8_t)strtoul(argv[2], NULL, 0), (uint16_t)strtoul(argv[3], NULL, 0));
        return write_out(argv[4], card, sizeof card);
    }
    const char *mode = argc == 3 ? argv[1] : "";
    int black = !strcmp(mode, "black"), white = !strcmp(mode, "white"), corrupt = !strcmp(mode, "black-corrupt"),
        recover = !strcmp(mode, "black-recover");
    if (!black && !white && !corrupt && !recover) {
        fprintf(stderr, "usage: %s black|white|black-corrupt|black-recover <out.sav>\n       %s pgf <type> <id> <out.pgf>\n",
                argv[0], argv[0]);
        return 2;
    }
    uint8_t *img = malloc(SAVE5_IMAGE_SIZE);
    if (!img)
        return 1;
    synth5_build(img, white ? SAVE5_GAME_WHITE : SAVE5_GAME_BLACK);
    if (corrupt)
        img[0x19400 + 0x30] ^= 0x5A;
    if (corrupt || recover)
        img[SAVE5_COPY_OFFSET + 0x19400 + 0x30] ^= 0x5A;
    int rc = write_out(argv[2], img, SAVE5_IMAGE_SIZE);
    free(img);
    return rc;
}
