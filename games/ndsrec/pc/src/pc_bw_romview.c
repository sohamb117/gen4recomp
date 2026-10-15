/*
 * Mods and custom carts on Black/White and HeartGold/SoulSilver: the content packages
 * (pc/src/pc_modfs.c, shared) served at the cartridge read instead of
 * through FS / NARC hooks (docs/BWHGSS_HOOKS.md, "Mods and custom carts").
 *
 * B/W's FS is TWL-SDK 5's, recompiled, with no FS symbols placed; but every
 * file it opens it finds through the ROM's FAT, which it reads off the
 * cartridge like any other bytes, and every cartridge read reaches
 * pc_card_rom.c's rom_read. So this file presents a ROM view:
 *
 *   - a package's whole file (replace/<nitro path>) is placed after the
 *     image's used area, in the cartridge's padding (the card's address
 *     space ends at its capacity; B/W fill all 256 MB), and its FAT entry
 *     points there;
 *   - a NARC with claimed members (narc/<nitro path>/<index>, appends past
 *     the cartridge's count included, pc_modfs_narc_file_count) is rebuilt
 *     with those members in place, its BTAF rewritten, and placed the same
 *     way;
 *   - reads return the cartridge's bytes with the moved FAT entries patched
 *     in and the moved files over the padding;
 *   - the moved files lie past the TWL-only area's start, which TWL-SDK's FS
 *     will not read in DS mode, so the in-memory header's limit (0x92) is
 *     moved past the view (build).
 *
 * The packages are pc_modfs_boot's (pc_main.c: PC_MODS / loadorder.txt
 * under the content root), so selecting and pinning them, the load order
 * and the claim rules are the same as on D/P/Pt. The view is built on the
 * first cartridge read; with nothing claimed it is not built and every read
 * goes straight to the cartridge as before. Only B/W and HeartGold /
 * SoulSilver compile this file and its hook (pc_card_rom.c under
 * PC_BW_ROMVIEW; games/ndsrec/pc/Makefile.wasm, games/heartgold/pc/
 * Makefile.wasm): HG/SS's NitroSDK 4.2 FS finds its files through the same
 * FAT, and its 128 MB cartridge has its padding after the used area too.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "np_guest_abi.h"
#include "pc_modfs.h"
#include "pc_wasm.h"

#define ALIGN_ROM 0x200u
#define ROM_HEADER_BUF 0x02FFFE00u /* HW_ROM_HEADER_BUF, TWL-SDK's map (ARMREC_TWL) */
#define TWL_AREA_UNIT 0x80000u
#define ALIGN_MEMBER 4u

typedef struct {
    uint32_t fid;
    uint32_t start; /* in the view, after the used area */
    uint32_t len;
    uint8_t *data;
} moved_file;

static moved_file *sMoved;
static int sNMoved, sCapMoved;
static uint32_t sCartSize, sViewSize, sFatOff, sFatSize;
static int sState; /* 0 not yet built, 1 the cartridge as is, 2 a view */

static void cart_read(uint32_t off, void *dst, uint32_t len)
{
    if (len == 0) return;
    if (np_host_rom_read(off, dst, len) != 0) {
        pc_wasm_fatalf("pc_bw_romview: read of %u bytes at rom:%#x failed", (unsigned)len, (unsigned)off);
    }
}

static uint32_t u32_at(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned u16_at(const uint8_t *p) { return p[0] | p[1] << 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, unsigned v) { p[0] = v; p[1] = v >> 8; }

static uint8_t *slurp(const char *host, uint32_t *len)
{
    FILE *f = fopen(host, "rb");
    long n;
    uint8_t *buf;

    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        pc_wasm_fatalf("pc_bw_romview: cannot read %s", host);
    }
    buf = malloc(n ? (size_t)n : 1);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        pc_wasm_fatalf("pc_bw_romview: cannot read %s", host);
    }
    fclose(f);
    *len = (uint32_t)n;
    return buf;
}

static void fat_entry(uint32_t fid, uint32_t *start, uint32_t *end)
{
    uint8_t e[8];

    cart_read(sFatOff + 8 * fid, e, 8);
    *start = u32_at(e);
    *end = u32_at(e + 4);
}

static void move_file(uint32_t fid, uint8_t *data, uint32_t len)
{
    if (sNMoved == sCapMoved) {
        sCapMoved = sCapMoved ? sCapMoved * 2 : 16;
        sMoved = realloc(sMoved, (size_t)sCapMoved * sizeof *sMoved);
        if (sMoved == NULL) pc_wasm_fatal("pc_bw_romview: out of memory");
    }
    sMoved[sNMoved].fid = fid;
    sMoved[sNMoved].data = data;
    sMoved[sNMoved].len = len;
    sNMoved++;
}

/* A NARC (header, BTAF, BTNF, GMIF) with its claimed members in place. */
static void narc_view(const char *path, uint32_t fid, uint32_t start, uint32_t end)
{
    uint8_t head[0x20];
    uint8_t *orig, *out, *btnf;
    uint32_t hsize, btafSize, btnfSize, gmifBase, total, pos, i, n, count, btnfOff;
    uint32_t *mlen;
    uint8_t **mdata;
    int claimed = 0;

    if (end - start < sizeof head) return;
    cart_read(start, head, sizeof head);
    if (memcmp(head, "NARC", 4) != 0) return;
    hsize = u16_at(head + 0xC);
    if (hsize + 12 > sizeof head || memcmp(head + hsize, "BTAF", 4) != 0) return;
    n = u16_at(head + hsize + 8);
    count = pc_modfs_narc_file_count(path, n);
    for (i = 0; i < count && !claimed; i++) {
        claimed = i >= n || pc_modfs_host_member(path, i) != NULL;
    }
    if (!claimed) return;

    orig = malloc(end - start);
    if (orig == NULL) pc_wasm_fatal("pc_bw_romview: out of memory");
    cart_read(start, orig, end - start);
    btafSize = u32_at(orig + hsize + 4);
    btnfOff = hsize + btafSize;
    btnf = orig + btnfOff;
    if (memcmp(btnf, "BTNF", 4) != 0) pc_wasm_fatalf("pc_bw_romview: %s: no BTNF", path);
    btnfSize = u32_at(btnf + 4);
    if (count != n && btnfSize > 0x10) {
        pc_wasm_fatalf("pc_bw_romview: %s: members appended to a NARC with member names", path);
    }
    gmifBase = btnfOff + btnfSize + 8;

    mlen = calloc(count, sizeof *mlen);
    mdata = calloc(count, sizeof *mdata);
    if (mlen == NULL || mdata == NULL) pc_wasm_fatal("pc_bw_romview: out of memory");
    total = 0;
    for (i = 0; i < count; i++) {
        const char *host = pc_modfs_host_member(path, i);

        if (host != NULL) {
            mdata[i] = slurp(host, &mlen[i]);
        } else {
            const uint8_t *e = orig + hsize + 12 + 8 * i;

            mdata[i] = orig + gmifBase + u32_at(e);
            mlen[i] = u32_at(e + 4) - u32_at(e);
        }
        total = (total + ALIGN_MEMBER - 1) & ~(ALIGN_MEMBER - 1);
        total += mlen[i];
    }
    total = (total + ALIGN_MEMBER - 1) & ~(ALIGN_MEMBER - 1);

    {
        const uint32_t newBtaf = 12 + 8 * count;
        const uint32_t size = hsize + newBtaf + btnfSize + 8 + total;

        out = malloc(size);
        if (out == NULL) pc_wasm_fatal("pc_bw_romview: out of memory");
        memset(out, 0xFF, size);
        memcpy(out, orig, hsize);
        put32(out + 8, size);
        memcpy(out + hsize, "BTAF", 4);
        put32(out + hsize + 4, newBtaf);
        put16(out + hsize + 8, count);
        put16(out + hsize + 10, 0);
        memcpy(out + hsize + newBtaf, btnf, btnfSize);
        pos = hsize + newBtaf + btnfSize;
        memcpy(out + pos, "GMIF", 4);
        put32(out + pos + 4, 8 + total);
        pos = 0;
        for (i = 0; i < count; i++) {
            pos = (pos + ALIGN_MEMBER - 1) & ~(ALIGN_MEMBER - 1);
            put32(out + hsize + 12 + 8 * i, pos);
            put32(out + hsize + 16 + 8 * i, pos + mlen[i]);
            memcpy(out + hsize + newBtaf + btnfSize + 8 + pos, mdata[i], mlen[i]);
            pos += mlen[i];
        }
        for (i = 0; i < count; i++) {
            if (mdata[i] < orig || mdata[i] >= orig + (end - start)) free(mdata[i]);
        }
        free(mdata);
        free(mlen);
        free(orig);
        move_file(fid, out, size);
        fprintf(stderr, "pc_bw_romview: %s: %u members (%u on the cartridge), %u bytes\n", path, (unsigned)count,
                (unsigned)n, (unsigned)size);
    }
}

/* The FNT, directory by directory (the root is 0xF000), each file's path. */
static void walk(const uint8_t *fnt, uint32_t fntSize, unsigned did, char *path, size_t plen)
{
    const uint8_t *e = fnt + 8 * (did & 0xFFF);
    uint32_t p = u32_at(e);
    uint32_t fid = u16_at(e + 4);

    while (p < fntSize && fnt[p] != 0) {
        const unsigned n = fnt[p] & 0x7F;
        const int sub = fnt[p] & 0x80;

        if (plen + n + 2 > 512 || p + 1 + n > fntSize) pc_wasm_fatal("pc_bw_romview: bad FNT");
        memcpy(path + plen, fnt + p + 1, n);
        path[plen + n] = '\0';
        p += 1 + n;
        if (sub) {
            path[plen + n] = '/';
            path[plen + n + 1] = '\0';
            walk(fnt, fntSize, u16_at(fnt + p), path, plen + n + 1);
            p += 2;
        } else {
            const char *host = pc_modfs_host_file(path);
            uint32_t start, end;

            if (host != NULL) {
                uint32_t len;
                uint8_t *data = slurp(host, &len);

                move_file(fid, data, len);
                fprintf(stderr, "pc_bw_romview: %s: %u bytes from the package\n", path, (unsigned)len);
            } else {
                fat_entry(fid, &start, &end);
                narc_view(path, fid, start, end);
            }
            fid++;
        }
    }
}

static void build(void)
{
    uint8_t h[0x50];
    uint8_t *fnt;
    uint32_t fntOff, fntSize, at, used;
    char path[512];
    int i;

    sState = 1;
    sCartSize = np_host_rom_size();
    cart_read(0, h, sizeof h);
    fntOff = u32_at(h + 0x40);
    fntSize = u32_at(h + 0x44);
    sFatOff = u32_at(h + 0x48);
    sFatSize = u32_at(h + 0x4C);
    fnt = malloc(fntSize);
    if (fnt == NULL) pc_wasm_fatal("pc_bw_romview: out of memory");
    cart_read(fntOff, fnt, fntSize);
    path[0] = '\0';
    walk(fnt, fntSize, 0xF000, path, 0);
    free(fnt);
    if (sNMoved == 0) return;

    /* After the image's used area (the header's NTR size at 0x80, and the
     * TWL total at 0x210 on a DSi-enhanced cartridge), in the cartridge's
     * own padding: the card's address space ends at its capacity (B/W fill
     * all 256 MB), and the SDK does not read past it. */
    used = u32_at(h + 0x80);
    if ((h[0x12] & 2) != 0) {
        uint8_t t[4];

        cart_read(0x210, t, 4);
        if (u32_at(t) > used) used = u32_at(t);
    }
    if (used == 0 || used > sCartSize) used = sCartSize;
    at = (used + ALIGN_ROM - 1) & ~(ALIGN_ROM - 1);
    for (i = 0; i < sNMoved; i++) {
        if (8 * sMoved[i].fid >= sFatSize) pc_wasm_fatal("pc_bw_romview: file id past the FAT");
        sMoved[i].start = at;
        at = (at + sMoved[i].len + ALIGN_ROM - 1) & ~(ALIGN_ROM - 1);
    }
    sViewSize = at > sCartSize ? at : sCartSize;
    /* TWL-SDK's FS refuses a ROM-archive read at or past the TWL-only area
     * when not running on a DSi (Black sub_0207A980: offset >= the header's
     * u16 at 0x92 << 19, from the header copy at HW_ROM_HEADER_BUF), and the
     * moved files are past it; the copy's limit moves past the view. The
     * cartridge's own TWL area is never read in DS mode. A DS-only
     * cartridge's NitroSDK (HeartGold/SoulSilver) has no such limit, and its
     * header copy is elsewhere: nothing to move. */
    if ((h[0x12] & 2) != 0) {
        volatile uint8_t *twlLimit = (volatile uint8_t *)(uintptr_t)(ROM_HEADER_BUF + 0x92);
        const unsigned units = (sViewSize + TWL_AREA_UNIT - 1) / TWL_AREA_UNIT;

        twlLimit[0] = units & 0xFF;
        twlLimit[1] = units >> 8;
    }
    sState = 2;
    fprintf(stderr, "pc_bw_romview: %d files moved, the view is %u bytes (the cartridge %u)\n", sNMoved,
            (unsigned)sViewSize, (unsigned)sCartSize);
}

int pc_bw_romview_read(uint32_t src, void *dst, uint32_t len)
{
    uint8_t *d = dst;
    const uint32_t end = src + len;
    int i;

    if (sState == 0) build();
    if (sState != 2) return 0;
    if (src > sViewSize || len > sViewSize - src) {
        pc_wasm_fatalf("pc_bw_romview: read of %u bytes at rom:%#x is past the %u-byte view", (unsigned)len,
                       (unsigned)src, (unsigned)sViewSize);
    }
    if (src < sCartSize) {
        const uint32_t stop = end < sCartSize ? end : sCartSize;

        cart_read(src, d, stop - src);
        for (i = 0; i < sNMoved; i++) {
            uint8_t e[8];
            uint32_t k, at = sFatOff + 8 * sMoved[i].fid;

            put32(e, sMoved[i].start);
            put32(e + 4, sMoved[i].start + sMoved[i].len);
            for (k = 0; k < 8; k++) {
                if (at + k >= src && at + k < stop) d[at + k - src] = e[k];
            }
        }
    }
    if (end > sCartSize) {
        const uint32_t from = src > sCartSize ? src : sCartSize;

        memset(d + (from - src), 0xFF, end - from);
    }
    for (i = 0; i < sNMoved; i++) { /* the moved files, over the padding */
        const uint32_t a = sMoved[i].start > src ? sMoved[i].start : src;
        const uint32_t b = sMoved[i].start + sMoved[i].len < end ? sMoved[i].start + sMoved[i].len : end;

        if (a < b) memcpy(d + (a - src), sMoved[i].data + (a - sMoved[i].start), b - a);
    }
    return 1;
}
