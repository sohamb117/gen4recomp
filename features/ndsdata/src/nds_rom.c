/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * NDS ROM container (header, FNT, FAT) and NARC archives.
 *
 * Header fields used (GBATEK "DS Cartridge Header"): title 0x000[12],
 * gamecode 0x00C[4], ROM version 0x01E, FNT offset/size 0x040/0x044,
 * FAT offset/size 0x048/0x04C. FAT entries are (start, end) u32 pairs. FNT:
 * a main table of 8-byte directory entries (subtable offset u32, first file
 * id u16, parent id u16 / directory count for the root) followed by
 * subtables of length-prefixed names (bit 7 = directory, followed by a u16
 * directory id 0xF000 | n), terminated by 0.
 *
 * NARC layout matches what pokeplatinum's tools/nitroarc writes: a 0x10-byte
 * "NARC" header (BOM 0xFFFE, version 0x0100, file size, header size, section
 * count 3) then "BTAF" (count u16, reserved u16, (start,end) pairs relative to
 * the GMIF image), "BTNF", and "GMIF" sections; each section header is
 * magic[4] + size u32 (size includes the 8-byte header).
 */
#include "ndsdata/ndsdata.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TABLE_SIZE (16u * 1024u * 1024u)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *nd_status_str(nd_status st)
{
    switch (st) {
    case ND_OK: return "ok";
    case ND_ERR_IO: return "I/O error";
    case ND_ERR_FORMAT: return "malformed data";
    case ND_ERR_NOT_FOUND: return "not found";
    case ND_ERR_NOMEM: return "out of memory";
    case ND_ERR_RANGE: return "out of range";
    case ND_ERR_UNSUPPORTED: return "unsupported";
    }
    return "unknown error";
}

const char *nd_game_name(nd_game g)
{
    switch (g) {
    case ND_GAME_DIAMOND: return "Diamond";
    case ND_GAME_PEARL: return "Pearl";
    case ND_GAME_PLATINUM: return "Platinum";
    case ND_GAME_BLACK: return "black";
    case ND_GAME_WHITE: return "white";
    case ND_GAME_HEARTGOLD: return "HeartGold";
    case ND_GAME_SOULSILVER: return "SoulSilver";
    default: return "Unknown";
    }
}

int nd_game_gen(nd_game g)
{
    switch (g) {
    case ND_GAME_DIAMOND:
    case ND_GAME_PEARL:
    case ND_GAME_PLATINUM:
    case ND_GAME_HEARTGOLD:
    case ND_GAME_SOULSILVER: return 4;
    case ND_GAME_BLACK:
    case ND_GAME_WHITE: return 5;
    default: return 0;
    }
}

int nd_read_stdio(void *user, uint64_t offset, void *dst, size_t len)
{
    FILE *f = (FILE *)user;
    if (offset > (uint64_t)LONG_MAX)
        return -1;
    if (fseek(f, (long)offset, SEEK_SET) != 0)
        return -1;
    return fread(dst, 1, len, f) == len ? 0 : -1;
}

nd_status nd_rom_read(const nd_rom *rom, uint64_t offset, void *dst, size_t len)
{
    if (rom->size && (offset > rom->size || len > rom->size - offset))
        return ND_ERR_RANGE;
    if (len == 0)
        return ND_OK;
    return rom->read(rom->user, offset, dst, len) == 0 ? ND_OK : ND_ERR_IO;
}

static nd_game game_from_code(const char *code)
{
    /* Retail game codes: ADA* Diamond, APA* Pearl, CPU* Platinum, IPK*
     * HeartGold, IPG* SoulSilver (pokeheartgold config.mk GAME_CODE), IRB*
     * Black, IRA* White (the fourth letter is the region, e.g. IRBO for the
     * US). */
    if (!memcmp(code, "ADA", 3))
        return ND_GAME_DIAMOND;
    if (!memcmp(code, "APA", 3))
        return ND_GAME_PEARL;
    if (!memcmp(code, "CPU", 3))
        return ND_GAME_PLATINUM;
    if (!memcmp(code, "IPK", 3))
        return ND_GAME_HEARTGOLD;
    if (!memcmp(code, "IPG", 3))
        return ND_GAME_SOULSILVER;
    if (!memcmp(code, "IRB", 3))
        return ND_GAME_BLACK;
    if (!memcmp(code, "IRA", 3))
        return ND_GAME_WHITE;
    return ND_GAME_UNKNOWN;
}

static nd_status load_table(const nd_rom *rom, uint32_t off, uint32_t size, uint8_t **out)
{
    if (size == 0 || size > MAX_TABLE_SIZE)
        return ND_ERR_FORMAT;
    uint8_t *buf = malloc(size);
    if (!buf)
        return ND_ERR_NOMEM;
    nd_status st = nd_rom_read(rom, off, buf, size);
    if (st != ND_OK) {
        free(buf);
        return st == ND_ERR_RANGE ? ND_ERR_FORMAT : st;
    }
    *out = buf;
    return ND_OK;
}

nd_status nd_rom_open(nd_rom *rom, nd_read_fn read, void *user, uint64_t size)
{
    memset(rom, 0, sizeof(*rom));
    if (!read)
        return ND_ERR_RANGE;
    rom->read = read;
    rom->user = user;
    rom->size = size;
    if (size && size < 0x200)
        return ND_ERR_FORMAT;

    uint8_t hdr[0x50];
    nd_status st = nd_rom_read(rom, 0, hdr, sizeof(hdr));
    if (st != ND_OK)
        return st;
    memcpy(rom->title, hdr, 12);
    rom->title[12] = 0;
    memcpy(rom->gamecode, hdr + 0x0C, 4);
    rom->gamecode[4] = 0;
    rom->rom_version = hdr[0x1E];
    rom->game = game_from_code(rom->gamecode);
    rom->fnt_offset = rd32(hdr + 0x40);
    rom->fnt_size = rd32(hdr + 0x44);
    rom->fat_offset = rd32(hdr + 0x48);
    rom->fat_size = rd32(hdr + 0x4C);
    if (rom->fat_size % 8 != 0 || rom->fnt_size < 8)
        return ND_ERR_FORMAT;
    rom->file_count = rom->fat_size / 8;

    if ((st = load_table(rom, rom->fnt_offset, rom->fnt_size, &rom->fnt)) != ND_OK)
        return st;
    if ((st = load_table(rom, rom->fat_offset, rom->fat_size, &rom->fat)) != ND_OK) {
        nd_rom_close(rom);
        return st;
    }
    return ND_OK;
}

void nd_rom_close(nd_rom *rom)
{
    free(rom->fnt);
    free(rom->fat);
    rom->fnt = NULL;
    rom->fat = NULL;
}

nd_status nd_rom_file_extent(const nd_rom *rom, uint32_t file_id, uint32_t *offset, uint32_t *length)
{
    if (!rom->fat || file_id >= rom->file_count)
        return ND_ERR_NOT_FOUND;
    uint32_t start = rd32(rom->fat + file_id * 8);
    uint32_t end = rd32(rom->fat + file_id * 8 + 4);
    if (end < start || (rom->size && end > rom->size))
        return ND_ERR_FORMAT;
    *offset = start;
    *length = end - start;
    return ND_OK;
}

nd_status nd_rom_find(const nd_rom *rom, const char *path, uint32_t *file_id)
{
    const uint8_t *fnt = rom->fnt;
    const uint32_t fnt_size = rom->fnt_size;
    if (!fnt)
        return ND_ERR_NOT_FOUND;
    uint32_t dir_count = rd16(fnt + 6);
    if (dir_count == 0 || (uint64_t)dir_count * 8 > fnt_size)
        return ND_ERR_FORMAT;

    uint32_t dir = 0;
    const char *p = path;
    while (*p == '/')
        p++;
    for (;;) {
        const char *slash = strchr(p, '/');
        size_t clen = slash ? (size_t)(slash - p) : strlen(p);
        int last = slash == NULL;
        if (clen == 0 || clen > 127)
            return ND_ERR_NOT_FOUND;

        uint32_t sub = rd32(fnt + dir * 8);
        uint32_t fid = rd16(fnt + dir * 8 + 4);
        int found = 0;
        while (sub < fnt_size) {
            uint8_t b = fnt[sub++];
            if (b == 0)
                break;
            uint32_t len = b & 0x7F;
            int is_dir = (b & 0x80) != 0;
            if (len == 0 || sub + len + (is_dir ? 2u : 0u) > fnt_size)
                return ND_ERR_FORMAT;
            int match = len == clen && memcmp(fnt + sub, p, clen) == 0;
            sub += len;
            if (is_dir) {
                uint32_t id = rd16(fnt + sub) & 0x0FFF;
                sub += 2;
                if (match && !last) {
                    if (id >= dir_count)
                        return ND_ERR_FORMAT;
                    dir = id;
                    found = 1;
                    break;
                }
            } else {
                if (match && last) {
                    *file_id = fid;
                    return ND_OK;
                }
                fid++;
            }
        }
        if (!found)
            return ND_ERR_NOT_FOUND;
        p = slash + 1;
    }
}

nd_status nd_rom_load_file(const nd_rom *rom, uint32_t file_id, uint8_t **out, size_t *out_len)
{
    uint32_t off, len;
    nd_status st = nd_rom_file_extent(rom, file_id, &off, &len);
    if (st != ND_OK)
        return st;
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf)
        return ND_ERR_NOMEM;
    if ((st = nd_rom_read(rom, off, buf, len)) != ND_OK) {
        free(buf);
        return st;
    }
    *out = buf;
    *out_len = len;
    return ND_OK;
}

nd_status nd_rom_load_path(const nd_rom *rom, const char *path, uint8_t **out, size_t *out_len)
{
    uint32_t id;
    nd_status st = nd_rom_find(rom, path, &id);
    if (st != ND_OK)
        return st;
    return nd_rom_load_file(rom, id, out, out_len);
}

/* ---------------------------------------------------------------- NARC */

static nd_status narc_check_header(const uint8_t *h, size_t avail)
{
    if (avail < 0x10 || memcmp(h, "NARC", 4) != 0 || rd16(h + 4) != 0xFFFE)
        return ND_ERR_FORMAT;
    if (rd16(h + 0x0C) != 0x10 || rd16(h + 0x0E) < 3)
        return ND_ERR_FORMAT;
    return ND_OK;
}

nd_status nd_narc_parse(nd_narc *narc, const uint8_t *data, size_t size)
{
    memset(narc, 0, sizeof(*narc));
    nd_status st = narc_check_header(data, size);
    if (st != ND_OK)
        return st;
    size_t off = 0x10;
    /* BTAF */
    if (off + 12 > size || memcmp(data + off, "BTAF", 4) != 0)
        return ND_ERR_FORMAT;
    uint32_t btaf_size = rd32(data + off + 4);
    narc->count = rd16(data + off + 8);
    if (btaf_size < 12 || btaf_size > size - off || 12 + (uint64_t)narc->count * 8 > btaf_size)
        return ND_ERR_FORMAT;
    narc->btaf = data + off + 12;
    off += btaf_size;
    /* BTNF */
    if (off + 8 > size || memcmp(data + off, "BTNF", 4) != 0)
        return ND_ERR_FORMAT;
    uint32_t btnf_size = rd32(data + off + 4);
    if (btnf_size < 8 || btnf_size > size - off)
        return ND_ERR_FORMAT;
    narc->btnf = data + off + 8;
    narc->btnf_size = btnf_size - 8;
    off += btnf_size;
    /* GMIF */
    if (off + 8 > size || memcmp(data + off, "GMIF", 4) != 0)
        return ND_ERR_FORMAT;
    uint32_t gmif_size = rd32(data + off + 4);
    if (gmif_size < 8 || gmif_size > size - off)
        return ND_ERR_FORMAT;
    narc->gmif = data + off + 8;
    narc->gmif_size = gmif_size - 8;
    narc->data = data;
    narc->size = size;
    return ND_OK;
}

nd_status nd_narc_member(const nd_narc *narc, uint32_t index, const uint8_t **ptr, size_t *len)
{
    if (index >= narc->count)
        return ND_ERR_NOT_FOUND;
    uint32_t start = rd32(narc->btaf + index * 8);
    uint32_t end = rd32(narc->btaf + index * 8 + 4);
    if (end < start || end > narc->gmif_size)
        return ND_ERR_FORMAT;
    *ptr = narc->gmif + start;
    *len = end - start;
    return ND_OK;
}

nd_status nd_rom_load_narc_member(const nd_rom *rom, uint32_t narc_file_id, uint32_t member,
                                  uint8_t **out, size_t *out_len)
{
    uint32_t base, flen;
    nd_status st = nd_rom_file_extent(rom, narc_file_id, &base, &flen);
    if (st != ND_OK)
        return st;
    uint8_t hdr[0x10 + 12];
    if (flen < sizeof(hdr))
        return ND_ERR_FORMAT;
    if ((st = nd_rom_read(rom, base, hdr, sizeof(hdr))) != ND_OK)
        return st;
    if ((st = narc_check_header(hdr, sizeof(hdr))) != ND_OK)
        return st;
    if (memcmp(hdr + 0x10, "BTAF", 4) != 0)
        return ND_ERR_FORMAT;
    uint32_t btaf_size = rd32(hdr + 0x14);
    uint32_t count = rd16(hdr + 0x18);
    if (btaf_size < 12 || 12 + (uint64_t)count * 8 > btaf_size || 0x10 + (uint64_t)btaf_size + 8 > flen)
        return ND_ERR_FORMAT;
    if (member >= count)
        return ND_ERR_NOT_FOUND;

    uint8_t ent[8];
    if ((st = nd_rom_read(rom, base + 0x10 + 12 + (uint64_t)member * 8, ent, 8)) != ND_OK)
        return st;
    uint32_t start = rd32(ent), end = rd32(ent + 4);

    uint64_t btnf_at = base + 0x10 + (uint64_t)btaf_size;
    uint8_t sec[8];
    if ((st = nd_rom_read(rom, btnf_at, sec, 8)) != ND_OK)
        return st;
    if (memcmp(sec, "BTNF", 4) != 0)
        return ND_ERR_FORMAT;
    uint64_t gmif_at = btnf_at + rd32(sec + 4);
    if (gmif_at + 8 > (uint64_t)base + flen)
        return ND_ERR_FORMAT;
    if ((st = nd_rom_read(rom, gmif_at, sec, 8)) != ND_OK)
        return st;
    if (memcmp(sec, "GMIF", 4) != 0)
        return ND_ERR_FORMAT;
    uint32_t gmif_size = rd32(sec + 4);
    if (gmif_size < 8 || end < start || end > gmif_size - 8 || gmif_at + gmif_size > (uint64_t)base + flen)
        return ND_ERR_FORMAT;

    size_t len = end - start;
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf)
        return ND_ERR_NOMEM;
    if ((st = nd_rom_read(rom, gmif_at + 8 + start, buf, len)) != ND_OK) {
        free(buf);
        return st;
    }
    *out = buf;
    *out_len = len;
    return ND_OK;
}
