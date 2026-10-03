/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dump_bank <rom.nds> <bank>: print one bank of the game's message NARC as a
 * JSON array of UTF-8 strings (consumed by check_banks.py). */
#include <stdio.h>
#include <stdlib.h>

#include "ndsdata/ndsdata.h"

static void jstr(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\')
            printf("\\%c", *p);
        else if (*p < 0x20)
            printf("\\u%04x", *p);
        else
            putchar(*p);
    }
    putchar('"');
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <rom.nds> <bank>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    nd_rom rom;
    if (nd_rom_open(&rom, nd_read_stdio, f, (uint64_t)ftell(f)) != ND_OK)
        return 1;
    uint32_t id;
    uint8_t *data;
    size_t len;
    nd_msgbank bank;
    if (nd_rom_find(&rom, nd_msg_narc_path(rom.game), &id) != ND_OK ||
        nd_rom_load_narc_member(&rom, id, (uint32_t)strtoul(argv[2], NULL, 0), &data, &len) != ND_OK ||
        nd_msgbank_parse(&bank, data, len) != ND_OK) {
        fprintf(stderr, "cannot read bank %s\n", argv[2]);
        return 1;
    }
    putchar('[');
    for (uint32_t i = 0; i < bank.count; i++) {
        char *s = NULL;
        if (nd_msgbank_get_utf8(&bank, i, &s) != ND_OK)
            return 1;
        if (i)
            putchar(',');
        jstr(s);
        free(s);
    }
    puts("]");
    free(data);
    nd_rom_close(&rom);
    fclose(f);
    return 0;
}
