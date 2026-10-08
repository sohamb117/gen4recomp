/* SPDX-License-Identifier: GPL-3.0-or-later */
/* make_synth_save pt|dp|hg|ss|pt-corrupt|hg-corrupt <out.sav>: write a
 * synthetic 512 KiB save image. The -corrupt modes flip a byte in both
 * copies of the general block so no valid copy remains.
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
    static const struct {
        const char *mode;
        save4_game game;
        int corrupt;
    } kModes[] = {
        {"pt", SAVE4_GAME_PT, 0}, {"dp", SAVE4_GAME_DP, 0},         {"hg", SAVE4_GAME_HG, 0},
        {"ss", SAVE4_GAME_SS, 0}, {"pt-corrupt", SAVE4_GAME_PT, 1}, {"hg-corrupt", SAVE4_GAME_HG, 1},
    };
    int m = -1;
    for (size_t i = 0; argc == 3 && i < sizeof kModes / sizeof kModes[0]; i++)
        if (!strcmp(argv[1], kModes[i].mode))
            m = (int)i;
    if (m < 0) {
        fprintf(stderr, "usage: %s pt|dp|hg|ss|pt-corrupt|hg-corrupt <out.sav>\n       %s card <type> <out.pcd>\n",
                argv[0], argv[0]);
        return 2;
    }
    uint8_t *img = malloc(SAVE4_IMAGE_SIZE);
    if (!img)
        return 1;
    synth_save_build(img, kModes[m].game);
    if (kModes[m].corrupt) {
        img[0x100] ^= 0x5A;
        img[SAVE4_COPY_SIZE + 0x100] ^= 0x5A;
    }
    int rc = write_out(argv[2], img, SAVE4_IMAGE_SIZE);
    free(img);
    return rc;
}
