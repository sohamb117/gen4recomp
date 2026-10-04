/*
 * Unit tests for the SDL-free shell pieces: SHA-1 (FIPS 180 vectors and
 * chunking), the ROM table, CRC-32/Adler-32 and the PNG container, and the
 * window -> stylus mapping for every layout, rotation, swap and scale mode.
 *
 * The touch test does not reuse the inverse math it checks: it projects DS
 * pixels forward with the same parameters SDL_RenderTextureRotated receives
 * (destination rect centre/size and a clockwise angle), so it verifies that
 * where a pixel is drawn is where a tap lands.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card.h"
#include "launch.h"
#include "layout.h"
#include "png.h"
#include "rewind.h"
#include "romdb.h"
#include "scale2x.h"
#include "sha1.h"
#include "slots.h"
#include "sync_plan.h"
#include "undo.h"

static int failures, checks;

#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        checks++;                                                                                                 \
        if (!(cond)) {                                                                                            \
            failures++;                                                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                           \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
        }                                                                                                         \
    } while (0)

static void sha1_of(const void *data, size_t len, size_t chunk, char hex[41])
{
    np_sha1 s;
    uint8_t d[20];
    np_sha1_init(&s);
    const uint8_t *p = data;
    while (len) {
        size_t n = chunk && chunk < len ? chunk : len;
        np_sha1_update(&s, p, n);
        p += n;
        len -= n;
    }
    np_sha1_final(&s, d);
    np_sha1_hex(d, hex);
}

static void test_sha1(void)
{
    static const struct {
        const char *msg, *hex;
    } v[] = {
        {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrst"
         "nopqrstu",
         "a49b2446a02c645bf419f995b67091253a04a259"},
        {"The quick brown fox jumps over the lazy dog", "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"},
    };
    char hex[41];
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        size_t chunks[] = {0, 1, 3, 63, 64, 65};
        for (size_t c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
            sha1_of(v[i].msg, strlen(v[i].msg), chunks[c], hex);
            CHECK(!strcmp(hex, v[i].hex), "sha1(\"%s\") chunk %zu = %s, want %s", v[i].msg, chunks[c], hex,
                  v[i].hex);
        }
    }
    size_t n = 1000000;
    char *a = malloc(n);
    memset(a, 'a', n);
    sha1_of(a, n, 0, hex);
    CHECK(!strcmp(hex, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"), "sha1(1M x 'a') = %s", hex);
    sha1_of(a, n, 4093, hex);
    CHECK(!strcmp(hex, "34aa973cd4c4daa4f61eeb2bdbad27316534016f"), "sha1(1M x 'a', 4093-byte chunks) = %s", hex);
    free(a);
}

static void test_romdb(void)
{
    const np_rom_entry *e = np_romdb_lookup("0862ec35b24de5c7e2dcb88c9eea0873110d755c");
    CHECK(e && e->game == NP_GAME_PLATINUM && e->status == NP_ROM_ACCEPTED, "platinum rev1 accepted");
    e = np_romdb_lookup("ce81046eda7d232513069519cb2085349896dec7");
    CHECK(e && e->game == NP_GAME_PLATINUM && e->status == NP_ROM_UNSUPPORTED, "platinum rev0 unsupported");
    e = np_romdb_lookup("a46233d8b79a69ea87aa295a0efad5237d02841e");
    CHECK(e && e->game == NP_GAME_DIAMOND && e->status == NP_ROM_ACCEPTED, "diamond accepted");
    e = np_romdb_lookup("99083bf15ec7c6b81b4ba241ee10abd9e80999ac");
    CHECK(e && e->game == NP_GAME_PEARL && e->status == NP_ROM_ACCEPTED, "pearl accepted");
    CHECK(!np_romdb_lookup("a9993e364706816aba3e25717850c26c9cd0d89d"), "unknown hash rejected");
    for (int g = 0; g < NP_GAME_COUNT; g++)
        CHECK(np_romdb_accepted((np_game)g) != NULL, "accepted dump for game %d", g);
}

typedef struct membuf {
    uint8_t *data;
    size_t len, cap;
} membuf;

static int mem_sink(void *user, const void *data, size_t len)
{
    membuf *m = user;
    if (m->len + len > m->cap) {
        m->cap = (m->len + len) * 2;
        m->data = realloc(m->data, m->cap);
    }
    memcpy(m->data + m->len, data, len);
    m->len += len;
    return 0;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Walks the chunks, checks every CRC, inflates the stored blocks and
 * compares against the source pixels. */
static void check_png(uint32_t w, uint32_t h)
{
    uint32_t *px = malloc((size_t)w * h * 4);
    for (uint32_t i = 0; i < w * h; i++)
        px[i] = (i * 2654435761u) & 0xFFFFFF;
    membuf m = {0};
    CHECK(np_png_encode(mem_sink, &m, px, w, h, w) == 0, "png encode %ux%u", w, h);
    CHECK(m.len > 8 && !memcmp(m.data, "\x89PNG\r\n\x1a\n", 8), "png signature");
    size_t pos = 8;
    uint8_t *raw = malloc((size_t)(1 + 3 * w) * h);
    size_t raw_len = 0;
    int saw_iend = 0, crc_ok = 1, zlib_ok = 1;
    while (pos + 12 <= m.len) {
        uint32_t len = be32(m.data + pos);
        const uint8_t *type = m.data + pos + 4, *body = type + 4;
        if (np_crc32(0, type, len + 4) != be32(body + len))
            crc_ok = 0;
        if (!memcmp(type, "IHDR", 4))
            CHECK(be32(body) == w && be32(body + 4) == h && body[8] == 8 && body[9] == 2, "IHDR fields");
        if (!memcmp(type, "IDAT", 4)) {
            const uint8_t *z = body + 2, *end = body + len - 4;
            zlib_ok &= body[0] == 0x78 && ((body[0] << 8 | body[1]) % 31) == 0;
            int final = 0;
            while (!final && z + 5 <= end) {
                final = z[0] & 1;
                uint32_t n = z[1] | z[2] << 8, nn = z[3] | z[4] << 8;
                zlib_ok &= (n ^ 0xFFFF) == nn && (z[0] & 6) == 0;
                memcpy(raw + raw_len, z + 5, n);
                raw_len += n;
                z += 5 + n;
            }
            zlib_ok &= final && z == end && np_adler32(1, raw, raw_len) == be32(end);
        }
        if (!memcmp(type, "IEND", 4))
            saw_iend = 1;
        pos += 12 + len;
    }
    CHECK(crc_ok, "png chunk CRCs");
    CHECK(zlib_ok, "png zlib stream");
    CHECK(saw_iend && pos == m.len, "png ends with IEND");
    CHECK(raw_len == (size_t)(1 + 3 * w) * h, "png raw length %zu", raw_len);
    int same = 1;
    for (uint32_t y = 0; y < h && raw_len == (size_t)(1 + 3 * w) * h; y++) {
        const uint8_t *row = raw + (size_t)y * (1 + 3 * w);
        same &= row[0] == 0;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = px[y * w + x];
            same &= row[1 + 3 * x] == (uint8_t)(p >> 16) && row[2 + 3 * x] == (uint8_t)(p >> 8) &&
                    row[3 + 3 * x] == (uint8_t)p;
        }
    }
    CHECK(same, "png pixels round-trip %ux%u", w, h);
    free(raw);
    free(px);
    free(m.data);
}

static void test_png(void)
{
    CHECK(np_crc32(0, "123456789", 9) == 0xCBF43926u, "crc32 check value");
    CHECK(np_adler32(1, "Wikipedia", 9) == 0x11E60398u, "adler32 check value");
    check_png(3, 2);
    check_png(256, 384); /* several 64 KiB stored blocks */
}

/* Where SDL draws the centre of bottom-screen pixel (tx, ty). */
static void project(const np_layout *l, int tx, int ty, float *wx, float *wy)
{
    const np_screen_place *sp = &l->screen[1];
    float k = sp->w / 256.0f;
    float dx = ((float)tx + 0.5f - 128.0f) * k, dy = ((float)ty + 0.5f - 96.0f) * k;
    double a = l->rotation * 3.14159265358979 / 2.0;
    float c = (float)cos(a), s = (float)sin(a);
    *wx = sp->cx + dx * c - dy * s;
    *wy = sp->cy + dx * s + dy * c;
}

static void test_layout_exhaustive(void)
{
    static const float sizes[][2] = {{960, 720}, {390, 844}, {1024, 1024}, {300, 200}, {2560, 1440}, {256, 384}};
    static const int pts[][2] = {{0, 0}, {255, 191}, {128, 96}, {17, 150}, {255, 0}, {0, 191}, {200, 3}};
    for (int mode = 0; mode < NP_LAYOUT_COUNT; mode++)
        for (int rot = 0; rot < 4; rot++)
            for (int swap = 0; swap < 2; swap++)
                for (int scale = 0; scale < NP_SCALE_COUNT; scale++)
                    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
                        float W = sizes[si][0], H = sizes[si][1];
                        np_layout_params p = {(np_layout_mode)mode, swap, rot, (np_scale_mode)scale, 256};
                        np_layout l;
                        np_layout_compute(&l, &p, W, H);
                        const char *ctx = "";
                        char buf[96];
                        snprintf(buf, sizeof buf, "mode %d rot %d swap %d scale %d win %.0fx%.0f", mode, rot, swap,
                                 scale, W, H);
                        ctx = buf;
                        for (int i = 0; i < 2; i++) {
                            const np_screen_place *sp = &l.screen[i];
                            if (!sp->visible)
                                continue;
                            CHECK(sp->bx >= -0.01f && sp->by >= -0.01f && sp->bx + sp->bw <= W + 0.01f &&
                                      sp->by + sp->bh <= H + 0.01f,
                                  "%s: screen %d inside window", ctx, i);
                            CHECK(fabsf(sp->w / sp->h - 256.0f / 192.0f) < 1e-3f, "%s: aspect", ctx);
                        }
                        if (l.screen[0].visible && l.screen[1].visible) {
                            const np_screen_place *a = &l.screen[0], *b = &l.screen[1];
                            float ox = fminf(a->bx + a->bw, b->bx + b->bw) - fmaxf(a->bx, b->bx);
                            float oy = fminf(a->by + a->bh, b->by + b->bh) - fmaxf(a->by, b->by);
                            CHECK(ox <= 0.01f || oy <= 0.01f, "%s: screens overlap", ctx);
                        }
                        if (scale == NP_SCALE_INTEGER && l.scale >= 1.0f)
                            CHECK(l.scale == floorf(l.scale), "%s: integer scale %f", ctx, l.scale);
                        CHECK(l.screen[1].visible == (mode != NP_LAYOUT_TOP_ONLY), "%s: bottom visibility", ctx);
                        CHECK(l.screen[0].visible == (mode != NP_LAYOUT_BOTTOM_ONLY), "%s: top visibility", ctx);

                        int tx, ty;
                        if (!l.screen[1].visible) {
                            CHECK(!np_layout_touch(&l, W / 2, H / 2, 1, &tx, &ty), "%s: no bottom, no touch", ctx);
                            continue;
                        }
                        for (size_t k = 0; k < sizeof pts / sizeof pts[0]; k++) {
                            float wx, wy;
                            project(&l, pts[k][0], pts[k][1], &wx, &wy);
                            int ok = np_layout_touch(&l, wx, wy, 0, &tx, &ty);
                            CHECK(ok && tx == pts[k][0] && ty == pts[k][1],
                                  "%s: pixel (%d,%d) drawn at (%.2f,%.2f) maps to %s(%d,%d)", ctx, pts[k][0],
                                  pts[k][1], wx, wy, ok ? "" : "nothing ", tx, ty);
                        }
                        /* The top screen's centre is not the touch screen... */
                        if (l.screen[0].visible)
                            CHECK(!np_layout_touch(&l, l.screen[0].cx, l.screen[0].cy, 0, &tx, &ty),
                                  "%s: top screen is not touchable", ctx);
                        /* ...but a held stylus dragged there clamps to an edge. */
                        if (np_layout_touch(&l, -1000.0f, -1000.0f, 1, &tx, &ty))
                            CHECK((tx == 0 || tx == 255) && (ty == 0 || ty == 191), "%s: clamped to a corner (%d,%d)",
                                  ctx, tx, ty);
                        else
                            CHECK(0, "%s: clamp with bottom visible", ctx);
                    }
}

static void test_layout_fixed(void)
{
    np_layout l;
    int tx, ty;
    /* 1:1 vertical stack: bottom screen starts 192 px down. */
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_VERTICAL, 0, 0, NP_SCALE_FIT, 256}, 256, 384);
    CHECK(np_layout_touch(&l, 10.5f, 200.5f, 0, &tx, &ty) && tx == 10 && ty == 8, "vertical 1x: (10,8) got (%d,%d)", tx,
          ty);
    CHECK(!np_layout_touch(&l, 10.5f, 100.5f, 0, &tx, &ty), "vertical 1x: top screen not touchable");
    /* Swapped: the bottom screen is on top. */
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_VERTICAL, 1, 0, NP_SCALE_FIT, 256}, 256, 384);
    CHECK(np_layout_touch(&l, 10.5f, 8.5f, 0, &tx, &ty) && tx == 10 && ty == 8, "vertical swapped: got (%d,%d)", tx,
          ty);
    /* Rotated 90 degrees clockwise into a 384x256 window: the bottom
     * screen is the left half, its top-left corner at the window's top
     * edge, x = 191. */
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_VERTICAL, 0, 1, NP_SCALE_FIT, 256}, 384, 256);
    CHECK(np_layout_touch(&l, 191.5f, 0.5f, 0, &tx, &ty) && tx == 0 && ty == 0, "rot90: (0,0) got (%d,%d)", tx, ty);
    CHECK(np_layout_touch(&l, 0.5f, 255.5f, 0, &tx, &ty) && tx == 255 && ty == 191, "rot90: (255,191) got (%d,%d)", tx,
          ty);
    /* Integer scale 2 side by side in a 1100x400 window: 1024x384 centred. */
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_HORIZONTAL, 0, 0, NP_SCALE_INTEGER, 256}, 1100, 400);
    CHECK(l.scale == 2.0f && l.origin_x == 38.0f && l.origin_y == 8.0f, "integer 2x origin (%f,%f) scale %f",
          l.origin_x, l.origin_y, l.scale);
    CHECK(np_layout_touch(&l, 38.0f + 512.0f + 3.0f, 8.0f + 5.0f, 0, &tx, &ty) && tx == 1 && ty == 2,
          "integer 2x: got (%d,%d)", tx, ty);
    /* Hybrid, swapped: the bottom screen is the large one at 2x. */
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_HYBRID, 1, 0, NP_SCALE_FIT, 256}, 768, 384);
    CHECK(np_layout_touch(&l, 100.5f, 50.5f, 0, &tx, &ty) && tx == 50 && ty == 25, "hybrid swapped: got (%d,%d)", tx,
          ty);
    /* Widescreen: 342-column screens with the DS picture 43 columns in;
     * the side bars are not touchable but a held stylus clamps to them. */
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_VERTICAL, 0, 0, NP_SCALE_FIT, 342}, 342, 384);
    CHECK(l.screen[1].w == 342.0f && l.screen[1].h == 192.0f, "wide screen size %fx%f", l.screen[1].w, l.screen[1].h);
    CHECK(np_layout_touch(&l, 43.5f, 192.5f, 0, &tx, &ty) && tx == 0 && ty == 0, "wide: (0,0) got (%d,%d)", tx, ty);
    CHECK(np_layout_touch(&l, 298.5f, 383.5f, 0, &tx, &ty) && tx == 255 && ty == 191, "wide: (255,191) got (%d,%d)",
          tx, ty);
    CHECK(!np_layout_touch(&l, 42.5f, 200.5f, 0, &tx, &ty), "wide: left bar not touchable");
    CHECK(!np_layout_touch(&l, 299.5f, 200.5f, 0, &tx, &ty), "wide: right bar not touchable");
    CHECK(np_layout_touch(&l, 10.5f, 200.5f, 1, &tx, &ty) && tx == 0 && ty == 8, "wide: clamped (%d,%d)", tx, ty);
}

static void test_slot_names(void)
{
    static const char *const ok[] = {"Slot 1", "Nuzlocke (2nd)", "a", "Run #3!", "it's.mine", "CONSOLE", "COM10",
                                     "x2345678901234567890123456789012"};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        CHECK(!np_slot_name_problem(ok[i]), "\"%s\" should be valid", ok[i]);
    static const char *const bad[] = {"",      " lead", "trail ", "dot.",  ".hidden", "a/b", "a\\b", "a:b",
                                      "q?",    "CON",   "nul",    "Com1", "lpt9.x",  "aux ", "tab\t",
                                      "x23456789012345678901234567890123"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(np_slot_name_problem(bad[i]) != NULL, "\"%s\" should be invalid", bad[i]);

    CHECK(np_slot_name_eq("Slot 1", "sLOT 1") && !np_slot_name_eq("Slot 1", "Slot 10"), "case-insensitive equality");

    char out[NP_SLOT_NAME_MAX + 1];
    np_slot_sanitize("Pokemon Platinum (USA)", "Imported", out);
    CHECK(!strcmp(out, "Pokemon Platinum (USA)"), "sanitize keeps a good name: %s", out);
    np_slot_sanitize("  my:save*file  .", "Imported", out);
    CHECK(!strcmp(out, "my_save_file"), "sanitize replaces and trims: \"%s\"", out);
    np_slot_sanitize("...", "Imported", out);
    CHECK(!strcmp(out, "Imported"), "sanitize falls back: \"%s\"", out);
    np_slot_sanitize("NUL", "Imported", out);
    CHECK(!strcmp(out, "Imported"), "sanitize avoids device names: \"%s\"", out);
    np_slot_sanitize("A very long save file name that goes on and on", "Imported", out);
    CHECK(!strcmp(out, "A very long save file name that") && !np_slot_name_problem(out),
          "sanitize caps length, then trims the cut: \"%s\"", out);
    np_slot_sanitize("ends with spaces after the cut x                ", "Imported", out);
    CHECK(!np_slot_name_problem(out), "sanitize result valid: \"%s\"", out);

    const char *taken[] = {"Slot 1", "slot 2", "Run", "Run (2)", "x2345678901234567890123456789012"};
    np_slot_unique("Fresh", taken, 5, out);
    CHECK(!strcmp(out, "Fresh"), "unique keeps a free name: %s", out);
    np_slot_unique("run", taken, 5, out);
    CHECK(!strcmp(out, "run (3)"), "unique skips taken suffixes case-insensitively: %s", out);
    np_slot_unique("x2345678901234567890123456789012", taken, 5, out);
    CHECK(!strcmp(out, "x234567890123456789012345678 (2)") && !np_slot_name_problem(out),
          "unique shortens to fit: %s", out);
    np_slot_unique("abcdefghijklmnopqrstuvwxyz 12345", (const char *const[]){"abcdefghijklmnopqrstuvwxyz 12345"}, 1,
                   out);
    CHECK(!np_slot_name_problem(out), "unique never ends a cut name with a space: \"%s\"", out);
    np_slot_default_name(taken, 5, out);
    CHECK(!strcmp(out, "Slot 3"), "default name skips Slot 1/2: %s", out);
    np_slot_default_name(NULL, 0, out);
    CHECK(!strcmp(out, "Slot 1"), "default name: %s", out);
}

static void test_sav_footer(void)
{
    static const char snip[] = "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";
    static const char cookie[] = "|-DESMUME SAVE-|";
    size_t footer = NP_DESMUME_FOOTER_BYTES;
    uint8_t *buf = calloc(1, NP_SAVE_BYTES + footer + 64);
    size_t raw = 0;
    const char *why = NULL;
    CHECK(np_sav_normalize(buf, NP_SAVE_BYTES, &raw, &why) == 0 && raw == NP_SAVE_BYTES, "raw 512 KiB accepted");
    /* DeSmuME .dsv: raw image, then text marker, six u32 fields, cookie. */
    memcpy(buf + NP_SAVE_BYTES, snip, sizeof snip - 1);
    memcpy(buf + NP_SAVE_BYTES + footer - 16, cookie, 16);
    CHECK(sizeof snip - 1 + 24 + 16 == footer, "footer layout is %zu bytes", sizeof snip - 1 + 24 + 16);
    raw = 0;
    CHECK(np_sav_normalize(buf, NP_SAVE_BYTES + footer, &raw, &why) == 0 && raw == NP_SAVE_BYTES,
          "DeSmuME footer stripped");
    CHECK(np_sav_normalize(buf, NP_SAVE_BYTES - 1, &raw, &why) == -1 && why, "short file rejected");
    CHECK(np_sav_normalize(buf, NP_SAVE_BYTES + 1, &raw, &why) == -1, "odd size rejected");
    CHECK(np_sav_normalize(buf, 8192, &raw, &why) == -1, "8 KiB EEPROM save rejected");
    /* A .dsv for a 256 KiB chip: footer right after 256 KiB. */
    memset(buf, 0, NP_SAVE_BYTES + footer);
    memcpy(buf + NP_SAVE_BYTES / 2, snip, sizeof snip - 1);
    memcpy(buf + NP_SAVE_BYTES / 2 + footer - 16, cookie, 16);
    CHECK(np_sav_normalize(buf, NP_SAVE_BYTES / 2 + footer, &raw, &why) == -1, "256 KiB DeSmuME save rejected");
    /* Marker in the right place but a mangled cookie. */
    memset(buf, 0, NP_SAVE_BYTES + footer);
    memcpy(buf + NP_SAVE_BYTES, snip, sizeof snip - 1);
    CHECK(np_sav_normalize(buf, NP_SAVE_BYTES + footer, &raw, &why) == -1, "footer without cookie rejected");
    free(buf);
}

static void test_launch(void)
{
    np_launch l;
    char err[160];
    char *a1[] = {"nativeplat", "--game", "Platinum", "--slot", "My Run"};
    CHECK(np_launch_parse_args(5, a1, &l, err, sizeof err) == 0 && l.game == NP_GAME_PLATINUM &&
              !strcmp(l.slot, "My Run") && !l.force_launcher,
          "args: --game/--slot (%s)", err);
    char *a2[] = {"nativeplat", "--game=pearl", "--slot=2", "-psn_0_12345", "-NSDocumentRevisionsDebugMode", "YES"};
    CHECK(np_launch_parse_args(6, a2, &l, err, sizeof err) == 0 && l.game == NP_GAME_PEARL &&
              np_launch_slot_number(l.slot) == 2,
          "args: = form, macOS extras ignored (%s)", err);
    char *a3[] = {"nativeplat", "--launcher", "--game", "diamond"};
    CHECK(np_launch_parse_args(4, a3, &l, err, sizeof err) == 0 && l.force_launcher && l.game == NP_GAME_DIAMOND,
          "args: --launcher");
    char *a4[] = {"nativeplat"};
    CHECK(np_launch_parse_args(1, a4, &l, err, sizeof err) == 0 && l.game < 0 && !l.slot[0], "args: none");
    char *b1[] = {"nativeplat", "--game", "emerald"};
    CHECK(np_launch_parse_args(3, b1, &l, err, sizeof err) == -1 && strstr(err, "emerald"), "args: bad game (%s)",
          err);
    char *b2[] = {"nativeplat", "--slot", "1"};
    CHECK(np_launch_parse_args(3, b2, &l, err, sizeof err) == -1, "args: slot without game");
    char *b3[] = {"nativeplat", "--game"};
    CHECK(np_launch_parse_args(2, b3, &l, err, sizeof err) == -1, "args: missing value");
    char *b4[] = {"nativeplat", "--fullscreen"};
    CHECK(np_launch_parse_args(2, b4, &l, err, sizeof err) == -1, "args: unknown option");

    CHECK(np_launch_is_url("nativeplat://launch") && np_launch_is_url("NativePlat:launch") &&
              !np_launch_is_url("/Users/x/nativeplat.nds") && !np_launch_is_url("nativeplat"),
          "url detection");
    CHECK(np_launch_parse_url("nativeplat://launch?game=platinum&slot=Slot%201", &l, err, sizeof err) == 0 &&
              l.game == NP_GAME_PLATINUM && !strcmp(l.slot, "Slot 1"),
          "url: game+slot (%s)", err);
    CHECK(np_launch_parse_url("nativeplat://launch/?slot=My+%28best%29+run&game=DIAMOND", &l, err, sizeof err) == 0 &&
              l.game == NP_GAME_DIAMOND && !strcmp(l.slot, "My (best) run"),
          "url: + and escapes, any order (%s)", err);
    CHECK(np_launch_parse_url("nativeplat://launch?game=pearl#frag", &l, err, sizeof err) == 0 &&
              l.game == NP_GAME_PEARL && !l.slot[0],
          "url: fragment ignored (%s)", err);
    CHECK(np_launch_parse_url("nativeplat://launch?launcher=1", &l, err, sizeof err) == 0 && l.force_launcher,
          "url: launcher");
    CHECK(np_launch_parse_url("nativeplat://", &l, err, sizeof err) == 0 && l.game < 0, "url: bare scheme");
    CHECK(np_launch_parse_url("nativeplat://launch?game=platinum&slot=a%2", &l, err, sizeof err) == -1,
          "url: truncated escape");
    CHECK(np_launch_parse_url("nativeplat://launch?game=platinum&slot=a%00b", &l, err, sizeof err) == -1,
          "url: NUL escape");
    CHECK(np_launch_parse_url("nativeplat://launch?game=red", &l, err, sizeof err) == -1, "url: bad game");
    CHECK(np_launch_parse_url("nativeplat://delete?game=pearl", &l, err, sizeof err) == -1, "url: unknown action");
    CHECK(np_launch_parse_url("nativeplat://launch?game", &l, err, sizeof err) == -1, "url: key without value");
    CHECK(np_launch_parse_url("nativeplat://launch?slot=3", &l, err, sizeof err) == -1, "url: slot without game");
    CHECK(np_launch_parse_url("nativeplat://launch?game=platinum&color=red", &l, err, sizeof err) == -1,
          "url: unknown parameter");
    CHECK(np_launch_slot_number("12") == 12 && np_launch_slot_number("1a") == 0 && np_launch_slot_number("") == 0 &&
              np_launch_slot_number("Slot 1") == 0,
          "slot numbers");
}

static void test_undo(void)
{
    np_undo u;
    np_undo_init(&u, 4);
    uint8_t cur[4] = {0};
    CHECK(np_undo_undo(&u, cur) == -1 && np_undo_redo(&u, cur) == -1, "empty stacks refuse");
    /* Edits 0 -> 1 -> 2 -> 3, each pushing the state before it. */
    for (uint8_t v = 0; v < 3; v++) {
        np_undo_push(&u, cur);
        memset(cur, v + 1, 4);
    }
    CHECK(np_undo_undo(&u, cur) == 0 && cur[0] == 2, "undo to 2 (%d)", cur[0]);
    CHECK(np_undo_undo(&u, cur) == 0 && cur[0] == 1, "undo to 1 (%d)", cur[0]);
    CHECK(np_undo_redo(&u, cur) == 0 && cur[0] == 2, "redo to 2 (%d)", cur[0]);
    np_undo_push(&u, cur); /* a new edit drops the redo history */
    memset(cur, 9, 4);
    CHECK(np_undo_redo(&u, cur) == -1 && cur[0] == 9, "redo cleared by an edit");
    CHECK(np_undo_undo(&u, cur) == 0 && cur[0] == 2, "undo the new edit (%d)", cur[0]);
    np_undo_free(&u);

    /* Depth is bounded: the oldest snapshots fall off. */
    np_undo_init(&u, 4);
    memset(cur, 0, 4);
    for (int v = 0; v < NP_UNDO_DEPTH + 5; v++) {
        np_undo_push(&u, cur);
        memset(cur, v + 1, 4);
    }
    int steps = 0;
    while (np_undo_undo(&u, cur) == 0)
        steps++;
    CHECK(steps == NP_UNDO_DEPTH && cur[0] == 5, "%d undo steps, oldest state %d", steps, cur[0]);
    steps = 0;
    while (np_undo_redo(&u, cur) == 0)
        steps++;
    CHECK(steps == NP_UNDO_DEPTH && cur[0] == NP_UNDO_DEPTH + 5, "%d redo steps back to %d", steps, cur[0]);
    np_undo_free(&u);
}

static void test_scale2x(void)
{
    /* A diagonal edge: the staircase gets its corners filled in, flat
     * areas and the image border just double. Row stride 4 > width 3. */
    enum { A = 1, B = 2 };
    const uint32_t src[3 * 4] = {A, A, B, 0, A, B, B, 0, B, B, B, 0};
    uint32_t dst[6 * 6];
    np_scale2x(src, 3, 3, 4, dst);
    const uint32_t want[6 * 6] = {
        A, A, A, A, B, B, /* */
        A, A, A, B, B, B, /* */
        A, A, A, B, B, B, /* */
        A, B, B, B, B, B, /* */
        B, B, B, B, B, B, /* */
        B, B, B, B, B, B,
    };
    int same = 1;
    for (int i = 0; i < 36; i++)
        same &= dst[i] == want[i];
    CHECK(same, "scale2x diagonal");
    const uint32_t flat[4] = {7, 7, 7, 7};
    uint32_t out[16];
    np_scale2x(flat, 2, 2, 2, out);
    int all = 1;
    for (int i = 0; i < 16; i++)
        all &= out[i] == 7;
    CHECK(all, "scale2x flat");
}

/* Both pages fill the whole image with their own colours, differ, and
 * change when the data shown on them changes. */
static void test_card(void)
{
    static uint32_t a[NP_CARD_W * NP_CARD_H], b[NP_CARD_W * NP_CARD_H];
    np_card_info in = {.game = "Platinum", .name = "NATIVE", .tid = 3452, .money = 3000, .badges = 0x05,
                       .play_hours = 12, .play_minutes = 34, .dex_seen = 40, .dex_caught = 25, .party_count = 2,
                       .party = {"Turtwig", "Pok\xc3\xa9mon"}, .party_level = {14, 5}, .year = 2026, .month = 10,
                       .day = 4};
    for (int kind = NP_CARD_TRAINER; kind <= NP_CARD_DIPLOMA; kind++) {
        memset(a, 0xAB, sizeof a);
        np_card_render((np_card_kind)kind, &in, a);
        int untouched = 0, colors = 0;
        uint32_t seen[64];
        for (size_t i = 0; i < NP_CARD_W * NP_CARD_H; i++) {
            untouched += (a[i] & 0xFF000000u) != 0;
            int known = 0;
            for (int k = 0; k < colors; k++)
                known |= seen[k] == a[i];
            if (!known && colors < 64)
                seen[colors++] = a[i];
        }
        CHECK(!untouched, "card %d: %d pixels not drawn", kind, untouched);
        CHECK(colors >= 6, "card %d: only %d colours", kind, colors);
        np_card_info more = in;
        more.dex_caught = 26;
        np_card_render((np_card_kind)kind, &more, b);
        CHECK(memcmp(a, b, sizeof a) != 0, "card %d ignores the caught count", kind);
    }
    np_card_render(NP_CARD_DIPLOMA, &in, b);
    np_card_render(NP_CARD_TRAINER, &in, a);
    CHECK(memcmp(a, b, sizeof a) != 0, "trainer card and diploma differ");
}

/* Snapshots of changing length with sparse and dense changes come back
 * byte-exact, newest first; a small budget keeps only the newest ones. */
static void test_rewind(void)
{
    enum { N = 40, MAXLEN = 70000 };
    static uint8_t hist[N][MAXLEN];
    static size_t lens[N];
    uint32_t seed = 12345;
    for (int i = 0; i < N; i++) {
        lens[i] = 60000 + (size_t)(i % 7) * 1500;
        if (i)
            memcpy(hist[i], hist[i - 1], MAXLEN);
        else
            for (size_t k = 0; k < MAXLEN; k++)
                hist[0][k] = (uint8_t)(k * 7);
        int changes = i % 5 == 4 ? 20000 : 50; /* every fifth: a dense change */
        for (int c = 0; c < changes; c++) {
            seed = seed * 1103515245u + 12345u;
            hist[i][(seed >> 8) % MAXLEN] ^= (uint8_t)(seed >> 24 | 1);
        }
        memset(hist[i] + lens[i], 0, MAXLEN - lens[i]); /* bytes past a snapshot are not part of it */
    }
    for (int pass = 0; pass < 2; pass++) {
        size_t budget = pass ? 120000 : 16u << 20;
        np_rewind *r = np_rewind_create(budget, 1000);
        CHECK(r != NULL, "rewind create");
        if (!r)
            return;
        for (int i = 0; i < N; i++)
            CHECK(np_rewind_push(r, hist[i], lens[i]) == 0, "push %d", i);
        int depth = np_rewind_depth(r);
        CHECK(pass ? depth > 0 && depth < N - 1 : depth == N - 1, "pass %d depth %d", pass, depth);
        CHECK(np_rewind_used(r) <= budget, "within budget");
        int ok = 1;
        for (int i = N - 2; i >= N - 1 - depth; i--) {
            const uint8_t *s;
            size_t len;
            ok &= np_rewind_step_back(r, &s, &len) == 0 && len == lens[i] && !memcmp(s, hist[i], len);
        }
        CHECK(ok, "pass %d: every step back is exact", pass);
        const uint8_t *s;
        size_t len;
        CHECK(np_rewind_step_back(r, &s, &len) == -1, "nothing older");
        /* Recording resumes from the restored point. */
        CHECK(np_rewind_push(r, hist[N - 1], lens[N - 1]) == 0 && np_rewind_depth(r) == 1, "push after rewind");
        CHECK(np_rewind_step_back(r, &s, &len) == 0 && len == lens[N - 1 - depth] &&
                  !memcmp(s, hist[N - 1 - depth], len),
              "step back after re-push");
        np_rewind_destroy(r);
    }
}

static np_sync_side side(np_sync_kind kind, uint8_t fill)
{
    np_sync_side s = {kind, {0}};
    memset(s.hash, fill, sizeof s.hash);
    return s;
}

/* The three-way rules, conflict names and state lines. */
static void test_sync_plan(void)
{
    np_sync_side absent = side(NP_SYNC_ABSENT, 0), empty = side(NP_SYNC_EMPTY, 0);
    np_sync_side a = side(NP_SYNC_DATA, 0xA), b = side(NP_SYNC_DATA, 0xB), c = side(NP_SYNC_DATA, 0xC);
    uint8_t base_a[20], base_c[20];
    memset(base_a, 0xA, 20);
    memset(base_c, 0xC, 20);
    const struct {
        const np_sync_side *l, *r;
        const uint8_t *base;
        np_sync_action want;
        const char *what;
    } cases[] = {
        {&absent, &absent, NULL, NP_SYNC_SAME, "nothing anywhere"},
        {&a, &absent, NULL, NP_SYNC_PUSH, "new here"},
        {&absent, &a, NULL, NP_SYNC_PULL, "new there"},
        {&empty, &a, NULL, NP_SYNC_PULL, "fresh slot here never wins"},
        {&a, &empty, base_a, NP_SYNC_PUSH, "fresh slot there never wins"},
        {&empty, &empty, NULL, NP_SYNC_SAME, "both fresh"},
        {&a, &a, NULL, NP_SYNC_SAME, "same content"},
        {&a, &b, base_a, NP_SYNC_PULL, "only there changed"},
        {&b, &a, base_a, NP_SYNC_PUSH, "only here changed"},
        {&a, &b, base_c, NP_SYNC_CONFLICT, "both changed"},
        {&a, &b, NULL, NP_SYNC_CONFLICT, "never synced and different"},
        {&c, &absent, base_c, NP_SYNC_PUSH, "missing there is not a deletion"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        CHECK(np_sync_decide(cases[i].l, cases[i].r, cases[i].base) == cases[i].want, "sync: %s", cases[i].what);

    char out[NP_SLOT_NAME_MAX + 1];
    CHECK(!np_sync_conflict_name("Slot 1", 2026, 10, 4, NULL, 0, out) && !strcmp(out, "Slot 1 (conflict 2026-10-04)"),
          "conflict name: %s", out);
    const char *taken[] = {"Slot 1 (conflict 2026-10-04)"};
    CHECK(!np_sync_conflict_name("Slot 1", 2026, 10, 4, taken, 1, out) && strcmp(out, taken[0]) &&
              !np_slot_name_problem(out),
          "unique conflict name: %s", out);
    CHECK(!np_sync_conflict_name("Nuzlocke run with a long nam", 2026, 1, 2, NULL, 0, out) &&
              strlen(out) <= NP_SLOT_NAME_MAX && strstr(out, "(conflict 2026-01-02)") && !np_slot_name_problem(out),
          "long conflict name: %s", out);

    static const char *const games[] = {"diamond", "pearl", "platinum"};
    np_sync_record r = {2, "Slot 1", {0}, 524288, 1759500000123456789LL, 524288, -5, "Slot 1 (conflict 2026-10-04)"};
    for (int i = 0; i < 20; i++)
        r.base[i] = (uint8_t)(i * 13);
    char line[256];
    int n = np_sync_record_format(&r, games, line, sizeof line);
    np_sync_record back;
    CHECK(n > 0 && line[n - 1] == '\n' && !np_sync_record_parse(line, games, 3, &back) &&
              !memcmp(&back, &r, sizeof r),
          "record round trip: %s", line);
    r.conflict[0] = '\0';
    np_sync_record_format(&r, games, line, sizeof line);
    CHECK(!np_sync_record_parse(line, games, 3, &back) && !back.conflict[0], "record without conflict");
    CHECK(np_sync_record_parse("platinum\tSlot 1\tzz\t1\t2\t3\t4\t\n", games, 3, &back), "bad hash refused");
    CHECK(np_sync_record_parse("emerald\tSlot 1\t0000000000000000000000000000000000000000\t1\t2\t3\t4\t\n", games, 3,
                               &back),
          "unknown game refused");
    CHECK(np_sync_record_parse("platinum\tSlot/1\t0000000000000000000000000000000000000000\t1\t2\t3\t4\t\n", games, 3,
                               &back),
          "bad slot name refused");
}
int main(void)
{
    test_sha1();
    test_romdb();
    test_png();
    test_layout_fixed();
    test_layout_exhaustive();
    test_slot_names();
    test_sav_footer();
    test_launch();
    test_undo();
    test_scale2x();
    test_card();
    test_rewind();
    test_sync_plan();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
