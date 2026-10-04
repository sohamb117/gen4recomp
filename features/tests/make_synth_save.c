/* SPDX-License-Identifier: GPL-3.0-or-later */
/* make_synth_save pt|dp|pt-corrupt <out.sav>: write a synthetic 512 KiB save
 * image. pt-corrupt flips a byte in both copies of the general block so no
 * valid copy remains.
 * make_synth_save card <type> <out.pcd>: write a Wonder Card of that
 * MysteryGiftType (card id 42) built by save4_mg_build_card. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synth_save.h"

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
    if (argc == 4 && !strcmp(argv[1], "card")) {
        save4_card_spec spec = {(uint16_t)strtoul(argv[2], NULL, 0), 42, 0, {0, 0, 0}, 3500, "Test Card", "Test"};
        uint8_t card[SAVE4_WONDERCARD_SIZE];
        if (save4_mg_build_card(&spec, card) != SAVE4_OK) {
            fprintf(stderr, "%s: cannot build a card of type %s\n", argv[0], argv[2]);
            return 2;
        }
        return write_out(argv[3], card, sizeof card);
    }
    const char *mode = argc == 3 ? argv[1] : "";
    int pt = !strcmp(mode, "pt"), dp = !strcmp(mode, "dp"), bad = !strcmp(mode, "pt-corrupt");
    if (!pt && !dp && !bad) {
        fprintf(stderr, "usage: %s pt|dp|pt-corrupt <out.sav>\n       %s card <type> <out.pcd>\n", argv[0], argv[0]);
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
    int rc = write_out(argv[2], img, SAVE4_IMAGE_SIZE);
    free(img);
    return rc;
}
