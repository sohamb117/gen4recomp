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
#include "json.h"
#include "launch.h"
#include "layout.h"
#include "lowpass.h"
#include "modpkg.h"
#include "png.h"
#include "release.h"
#include "rewind.h"
#include "romdb.h"
#include "scale2x.h"
#include "sha1.h"
#include "sha256.h"
#include "skinfmt.h"
#include "slots.h"
#include "sync_plan.h"
#include "touchlayout.h"
#include "undo.h"
#include "zip.h"

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
        if (np_game_known((np_game)g)) /* 5 and 6 are unassigned (HGSS) */
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
                        np_layout_params p = {(np_layout_mode)mode, swap, rot, (np_scale_mode)scale, 256, NULL};
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
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_VERTICAL, 0, 0, NP_SCALE_FIT, 256, NULL}, 256, 384);
    CHECK(np_layout_touch(&l, 10.5f, 200.5f, 0, &tx, &ty) && tx == 10 && ty == 8, "vertical 1x: (10,8) got (%d,%d)", tx,
          ty);
    CHECK(!np_layout_touch(&l, 10.5f, 100.5f, 0, &tx, &ty), "vertical 1x: top screen not touchable");
    /* Swapped: the bottom screen is on top. */
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_VERTICAL, 1, 0, NP_SCALE_FIT, 256, NULL}, 256, 384);
    CHECK(np_layout_touch(&l, 10.5f, 8.5f, 0, &tx, &ty) && tx == 10 && ty == 8, "vertical swapped: got (%d,%d)", tx,
          ty);
    /* Rotated 90 degrees clockwise into a 384x256 window: the bottom
     * screen is the left half, its top-left corner at the window's top
     * edge, x = 191. */
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_VERTICAL, 0, 1, NP_SCALE_FIT, 256, NULL}, 384, 256);
    CHECK(np_layout_touch(&l, 191.5f, 0.5f, 0, &tx, &ty) && tx == 0 && ty == 0, "rot90: (0,0) got (%d,%d)", tx, ty);
    CHECK(np_layout_touch(&l, 0.5f, 255.5f, 0, &tx, &ty) && tx == 255 && ty == 191, "rot90: (255,191) got (%d,%d)", tx,
          ty);
    /* Integer scale 2 side by side in a 1100x400 window: 1024x384 centred. */
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_HORIZONTAL, 0, 0, NP_SCALE_INTEGER, 256, NULL}, 1100, 400);
    CHECK(l.scale == 2.0f && l.origin_x == 38.0f && l.origin_y == 8.0f, "integer 2x origin (%f,%f) scale %f",
          l.origin_x, l.origin_y, l.scale);
    CHECK(np_layout_touch(&l, 38.0f + 512.0f + 3.0f, 8.0f + 5.0f, 0, &tx, &ty) && tx == 1 && ty == 2,
          "integer 2x: got (%d,%d)", tx, ty);
    /* Hybrid, swapped: the bottom screen is the large one at 2x. */
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_HYBRID, 1, 0, NP_SCALE_FIT, 256, NULL}, 768, 384);
    CHECK(np_layout_touch(&l, 100.5f, 50.5f, 0, &tx, &ty) && tx == 50 && ty == 25, "hybrid swapped: got (%d,%d)", tx,
          ty);
    /* Widescreen: 342-column screens with the DS picture 43 columns in;
     * the side bars are not touchable but a held stylus clamps to them. */
    np_layout_compute(&l, &(np_layout_params){NP_LAYOUT_VERTICAL, 0, 0, NP_SCALE_FIT, 342, NULL}, 342, 384);
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
    char *e1[] = {"nativeplat", "--editor", "--save", "/tmp/My Save.sav", "--game=pearl"};
    CHECK(np_launch_parse_args(5, e1, &l, err, sizeof err) == 0 && l.editor && !strcmp(l.save, "/tmp/My Save.sav") &&
              l.game == NP_GAME_PEARL,
          "args: --editor --save (%s)", err);
    char *e2[] = {"nativeplat", "--editor"};
    CHECK(np_launch_parse_args(2, e2, &l, err, sizeof err) == -1, "args: --editor needs --save");
    char *e3[] = {"nativeplat", "--editor", "--save", "x.sav", "--slot", "1", "--game", "platinum"};
    CHECK(np_launch_parse_args(8, e3, &l, err, sizeof err) == -1, "args: --editor with a slot");

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

    static const char *const games[] = {"diamond", "pearl", "platinum", "black", "white"};
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

static const uint8_t zip_data[] = {
80,75,3,4,20,0,0,0,0,0,0,0,33,0,0,0,0,0,0,0,0,0,0,0,
0,0,4,0,0,0,112,107,103,47,80,75,3,4,20,0,0,0,8,0,0,0,33,0,
251,112,250,60,54,1,0,0,65,8,0,0,12,0,0,0,112,107,103,47,109,111,100,46,
116,111,109,108,149,213,187,110,131,64,16,133,225,158,167,24,81,167,96,102,150,91,164,148,
121,14,11,240,34,163,0,142,20,44,57,111,159,88,123,232,207,74,116,203,95,157,111,181,
203,85,62,164,140,207,97,251,94,227,101,139,251,227,114,196,231,81,22,251,176,197,215,209,
103,58,122,151,109,88,118,121,253,32,235,48,198,245,167,44,214,101,143,82,201,125,150,227,
22,229,250,251,159,44,147,220,30,243,188,13,187,140,235,125,250,122,147,97,156,210,39,85,
10,148,14,218,20,24,29,104,72,133,211,133,105,42,2,95,116,169,168,233,194,235,84,52,
116,17,44,21,45,95,244,169,232,232,162,110,82,209,211,69,227,24,144,159,188,61,55,207,
24,29,171,43,63,123,135,217,149,223,189,199,238,202,15,223,99,120,229,151,215,10,211,43,
191,189,42,198,87,126,125,85,204,175,252,254,106,0,160,188,0,117,16,48,158,128,6,24,
48,222,128,134,243,234,103,220,253,26,10,140,87,160,13,24,24,207,64,27,56,176,12,7,
45,28,88,134,131,14,14,44,195,65,7,7,150,225,160,135,3,227,29,88,5,7,206,59,
48,133,3,231,29,152,194,129,243,14,204,206,71,32,227,21,112,56,240,140,119,192,225,192,
121,7,22,224,192,121,7,86,195,129,243,14,172,134,3,231,29,88,3,7,158,225,160,245,
226,15,80,75,3,4,20,0,0,0,8,0,0,0,33,0,59,124,138,223,11,0,0,0,
18,0,0,0,13,0,0,0,112,107,103,47,115,109,97,108,108,46,116,120,116,203,72,205,
201,201,87,200,64,144,92,0,80,75,3,4,20,0,0,0,0,0,0,0,33,0,51,240,
196,104,16,0,0,0,16,0,0,0,18,0,0,0,112,107,103,47,46,99,111,111,107,101,
100,47,100,105,103,101,115,116,48,49,50,51,52,53,54,55,56,57,97,98,99,100,101,102,
80,75,3,4,20,0,0,0,0,0,0,0,33,0,30,187,193,19,9,0,0,0,9,0,
0,0,8,0,0,0,112,107,103,47,108,105,110,107,46,46,47,46,46,47,101,116,99,80,
75,1,2,20,3,20,0,0,0,0,0,0,0,33,0,0,0,0,0,0,0,0,0,0,
0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,128,1,0,0,0,0,112,107,103,
47,80,75,1,2,20,3,20,0,0,0,8,0,0,0,33,0,251,112,250,60,54,1,0,
0,65,8,0,0,12,0,0,0,0,0,0,0,0,0,0,0,128,1,34,0,0,0,112,
107,103,47,109,111,100,46,116,111,109,108,80,75,1,2,20,3,20,0,0,0,8,0,0,
0,33,0,59,124,138,223,11,0,0,0,18,0,0,0,13,0,0,0,0,0,0,0,0,
0,0,0,128,1,130,1,0,0,112,107,103,47,115,109,97,108,108,46,116,120,116,80,75,
1,2,20,3,20,0,0,0,0,0,0,0,33,0,51,240,196,104,16,0,0,0,16,0,
0,0,18,0,0,0,0,0,0,0,0,0,0,0,128,1,184,1,0,0,112,107,103,47,
46,99,111,111,107,101,100,47,100,105,103,101,115,116,80,75,1,2,20,3,20,0,0,0,
0,0,0,0,33,0,30,187,193,19,9,0,0,0,9,0,0,0,8,0,0,0,0,0,
0,0,0,0,0,0,255,161,248,1,0,0,112,107,103,47,108,105,110,107,80,75,5,6,
0,0,0,0,5,0,5,0,29,1,0,0,39,2,0,0,0,0
};

/* A zip written by Python's zipfile: a directory, a dynamic-Huffman and a
 * fixed-Huffman deflated member, a stored one and a symlink. */
static void test_zip(void)
{
    char big[2200];
    size_t n = (size_t)snprintf(big, sizeof big, "id = \"example_menu_text\"\nname = \"Example: main menu labels\"\n");
    for (int i = 0; i < 40; i++)
        n += (size_t)snprintf(big + n, sizeof big - n, "line %d of the dynamic huffman block, abcabcabc %d\n", i, i * 7);
    np_zip z;
    np_zip_entry e;
    CHECK(!np_zip_open(&z, zip_data, sizeof zip_data) && z.count == 5, "zip open (%u members)", z.count);
    CHECK(!np_zip_entry_at(&z, 0, &e) && e.is_dir && !strcmp(e.name, "pkg/"), "zip dir entry");
    static uint8_t out[4096];
    CHECK(np_zip_find(&z, "pkg/mod.toml", &e) == 1 && e.method == 8 && e.size == n && !np_zip_extract(&z, &e, out) &&
              memcmp(out, big, n) == 0,
          "zip dynamic-Huffman member");
    CHECK(np_zip_find(&z, "pkg/small.txt", &e) >= 0 && e.size == 18 && !np_zip_extract(&z, &e, out) &&
              memcmp(out, "hello hello hello\n", 18) == 0,
          "zip fixed-Huffman member");
    CHECK(np_zip_find(&z, "pkg/.cooked/digest", &e) >= 0 && e.method == 0 && !np_zip_extract(&z, &e, out) &&
              memcmp(out, "0123456789abcdef", 16) == 0,
          "zip stored member");
    CHECK(np_zip_find(&z, "pkg/link", &e) >= 0 && e.is_symlink, "zip symlink flagged");
    CHECK(np_zip_find(&z, "pkg/none", &e) == -1, "zip missing member");
    static uint8_t bad[sizeof zip_data];
    memcpy(bad, zip_data, sizeof bad);
    np_zip_find(&z, "pkg/mod.toml", &e);
    bad[e.local + 30 + 12 + 40] ^= 0x55; /* inside the deflate stream */
    np_zip zb;
    CHECK(!np_zip_open(&zb, bad, sizeof bad) && np_zip_find(&zb, "pkg/mod.toml", &e) >= 0 && np_zip_extract(&zb, &e, out),
          "zip corruption detected");
    CHECK(np_zip_open(&zb, bad, 100), "zip truncated refused");
    static const struct {
        const char *name;
        int safe;
    } names[] = {{"pkg/mod.toml", 1}, {"dir/", 1},  {"../x", 0}, {"a/../b", 0}, {"/abs", 0}, {"a/./b", 0},
                 {"a\\b", 0},         {"C:x", 0},   {"a//b", 0}, {"", 0},       {".", 0},    {"..", 0}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        CHECK(np_zip_name_safe(names[i].name) == names[i].safe, "zip name \"%s\"", names[i].name);
}

/* mod.toml as the example package writes it, plus dependency rules. */
static void test_modpkg(void)
{
    static const char toml[] = "# comment\n"
                               "id = \"example_menu_text\"\n"
                               "name = \"Example: main \\\"menu\\\" labels\"\n"
                               "version = \"1.0.0\"\n"
                               "authors = [\"nativeplat\", \"someone\"]\n"
                               "requires = [\"base_text\"]\n"
                               "load_after = []\n";
    np_mod_info a;
    const char *why = "";
    CHECK(!np_mod_parse(toml, sizeof toml - 1, &a, &why) && !strcmp(a.id, "example_menu_text") &&
              !strcmp(a.name, "Example: main \"menu\" labels") && !strcmp(a.version, "1.0.0") &&
              !strcmp(a.authors, "nativeplat, someone") && a.nrequires == 1 && !strcmp(a.requires[0], "base_text") &&
              a.nafter == 0,
          "mod.toml parsed (%s)", why);
    np_mod_info bad;
    static const char *const refused[] = {"id = \"Bad-Id\"\nname = \"x\"\nversion = \"1\"\n",
                                          "id = \"ok\"\nversion = \"1\"\n",
                                          "id = \"ok\nname = \"x\"\nversion = \"1\"\n"};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        CHECK(np_mod_parse(refused[i], strlen(refused[i]), &bad, &why), "mod.toml %zu refused", i);

    np_mod_info base = {.id = "base_text", .name = "b", .version = "1"};
    np_mod_info late = {.id = "late", .name = "l", .version = "1", .nafter = 1, .load_after = {"example_menu_text"}};
    char msg[96];
    const np_mod_info *ok[] = {&base, &a, &late};
    CHECK(np_mod_check_order(ok, 3, msg, sizeof msg) == -1, "valid order");
    const np_mod_info *missing[] = {&a};
    CHECK(np_mod_check_order(missing, 1, msg, sizeof msg) == 0 && strstr(msg, "base_text"), "missing requirement: %s",
          msg);
    const np_mod_info *wrong[] = {&base, &late, &a};
    CHECK(np_mod_check_order(wrong, 3, msg, sizeof msg) == 1 && strstr(msg, "after"), "load_after order: %s", msg);
}

/* FIPS 180-4 SHA-256 vectors, hashed whole and in odd chunks. */
static void test_sha256(void)
{
    static const struct {
        const char *msg;
        const char *hex;
    } v[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++)
        for (size_t chunk = 1; chunk <= 64; chunk += 63) {
            np_sha256 s;
            np_sha256_init(&s);
            size_t n = strlen(v[i].msg);
            for (size_t k = 0; k < n; k += chunk)
                np_sha256_update(&s, v[i].msg + k, n - k < chunk ? n - k : chunk);
            uint8_t d[32];
            char hex[65];
            np_sha256_final(&s, d);
            np_sha256_hex(d, hex);
            CHECK(!strcmp(hex, v[i].hex), "sha256 vector %zu chunk %zu: %s", i, chunk, hex);
        }
    static uint8_t mil[1000000];
    memset(mil, 'a', sizeof mil);
    np_sha256 s;
    np_sha256_init(&s);
    np_sha256_update(&s, mil, sizeof mil);
    uint8_t d[32];
    char hex[65];
    np_sha256_final(&s, d);
    np_sha256_hex(d, hex);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256 million a");
}

/* Token shapes, lookups and refusals of the JSON reader. */
static void test_json(void)
{
    static const char doc[] = " {\"a\": [1, -2.5e3, {\"b\": \"x\\\"y\\u00e9\"}, [], true], \"c\": {}, \"d\": null,"
                              " \"e\": \"\\ud83d\\ude00\"} ";
    np_json_tok t[64];
    int n = np_json_parse(doc, sizeof doc - 1, t, 64);
    CHECK(n == 16 && t[0].type == NP_JSON_OBJECT && t[0].size == 4 && t[0].next == n, "json shape (%d tokens)", n);
    int a = np_json_get(doc, t, 0, "a");
    CHECK(a >= 0 && t[a].type == NP_JSON_ARRAY && t[a].size == 5, "json array");
    CHECK(np_json_number(doc, t, np_json_at(t, a, 0), 0) == 1 && np_json_number(doc, t, np_json_at(t, a, 1), 0) == -2500,
          "json numbers");
    char s[32];
    int b = np_json_get(doc, t, np_json_at(t, a, 2), "b");
    CHECK(!np_json_string(doc, t, b, s, sizeof s) && !strcmp(s, "x\"y\xc3\xa9"), "json string escapes");
    CHECK(t[np_json_at(t, a, 3)].size == 0 && np_json_bool(doc, t, np_json_at(t, a, 4), 0) == 1 &&
              np_json_at(t, a, 5) == -1,
          "json empty array, bool, bounds");
    CHECK(t[np_json_get(doc, t, 0, "c")].type == NP_JSON_OBJECT && np_json_get(doc, t, 0, "zz") == -1 &&
              np_json_bool(doc, t, np_json_get(doc, t, 0, "d"), 7) == 7,
          "json object, missing key, null");
    CHECK(!np_json_string(doc, t, np_json_get(doc, t, 0, "e"), s, sizeof s) && !strcmp(s, "\xf0\x9f\x98\x80"),
          "json surrogate pair");
    static const char *const bad[] = {"",         "{",         "{\"a\" 1}", "[1,]",     "[1 2]",   "{\"a\":01x}",
                                      "\"a\\q\"", "[tru]",     "{} {}",     "{1:2}",    "\"\x01\"", "[-]"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(np_json_parse(bad[i], strlen(bad[i]), t, 64) < 0, "json refuses \"%s\"", bad[i]);
    CHECK(np_json_parse("[1,2,3]", 7, t, 3) < 0, "json token limit");
    char deep[200];
    memset(deep, '[', sizeof deep);
    CHECK(np_json_parse(deep, sizeof deep, t, 64) < 0, "json depth limit");
}

/* A trimmed GitHub "latest release" response and the updater's rules. */
static void test_release(void)
{
    static const char json[] =
        "{\"url\":\"https://api.github.com/repos/o/n/releases/1\",\"html_url\":\"https://github.com/o/n/releases/tag/"
        "v0.2.0\",\"id\":1,\"author\":{\"login\":\"o\",\"id\":2,\"site_admin\":false},\"tag_name\":\"v0.2.0\","
        "\"name\":\"nativeplat 0.2 \\u00e9\\ud83d\\ude00\",\"draft\":false,\"prerelease\":false,\"body\":\"line\\nnext "
        "\\\"quoted\\\"\",\"assets\":[{\"name\":\"nativeplat-0.2.0-windows-x64.zip\",\"size\":123,"
        "\"browser_download_url\":\"https://e/w.zip\",\"uploader\":null,\"label\":null},{\"name\":\"nativeplat-0.2.0-"
        "macOS.zip\",\"size\":9876543210,\"browser_download_url\":\"https://e/m.zip\",\"x\":[1,2.5e3,-4,[true]]},"
        "{\"name\":\"sha256sums.txt\",\"size\":300,\"browser_download_url\":\"https://e/s.txt\"}]}";
    np_release r;
    CHECK(!np_release_parse(json, sizeof json - 1, &r) && !strcmp(r.tag, "v0.2.0") && r.nassets == 3 &&
              !strcmp(r.name, "nativeplat 0.2 \xc3\xa9\xf0\x9f\x98\x80") && !r.draft &&
              !strcmp(r.html_url, "https://github.com/o/n/releases/tag/v0.2.0") && r.asset[1].size == 9876543210ull,
          "release parsed");
    CHECK(np_release_pick_asset(&r, "macos") == 1 && np_release_pick_asset(&r, "windows") == 0 &&
              np_release_pick_asset(&r, "linux") == -1,
          "asset per platform");
    CHECK(np_release_find_asset(&r, "sha256sums.txt") == 2, "sums asset");
    CHECK(np_release_parse(json, sizeof json - 2, &r), "truncated JSON refused");
    CHECK(np_release_parse("{\"name\":\"x\"}", 12, &r), "release without tag refused");
    char deep[300];
    memset(deep, '[', sizeof deep);
    memcpy(deep, "{\"a\":", 5);
    CHECK(np_release_parse(deep, sizeof deep, &r), "deep nesting refused");

    CHECK(np_version_compare("v0.2.0", "0.1.0") > 0 && np_version_compare("0.1.0", "v0.1.0") == 0 &&
              np_version_compare("0.10.0", "0.9.9") > 0 && np_version_compare("1.0.0-rc1", "1.0.0") < 0 &&
              np_version_compare("1.0.0-rc2", "1.0.0-rc1") > 0 && np_version_compare("1.0", "1.0.1") < 0,
          "version order");

    static const char sums[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  other.zip\r\n"
                               "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD *nativeplat-0.2.0-"
                               "macOS.zip\n";
    char hex[65];
    CHECK(!np_sha256sums_lookup(sums, sizeof sums - 1, "nativeplat-0.2.0-macOS.zip", hex) &&
              !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "sha256sums lookup");
    CHECK(!np_sha256sums_lookup(sums, sizeof sums - 1, "other.zip", hex), "sha256sums CRLF line");
    CHECK(np_sha256sums_lookup(sums, sizeof sums - 1, "macOS.zip", hex), "sha256sums exact names only");
}

/* The default arrangement hits where the original controls were, and the
 * file format round-trips. */
static void test_touchlayout(void)
{
    const float W = 1280, H = 720, u = 720.0f / 7.0f, m = 0.3f * u;
    np_tc_layout l;
    np_tc_default(&l, W, H);
    uint16_t keys;
    int ff, menu;
    float dcx = m + 1.5f * u, dcy = H - m - 1.5f * u, fcx = W - m - 1.5f * u;
    CHECK(np_tc_hit(&l, W, H, dcx - u, dcy, &keys, &ff, &menu) && keys == NP_KEY_LEFT, "d-pad left (%x)", keys);
    CHECK(np_tc_hit(&l, W, H, dcx + u, dcy - u, &keys, &ff, &menu) && keys == (NP_KEY_RIGHT | NP_KEY_UP),
          "d-pad diagonal (%x)", keys);
    CHECK(np_tc_hit(&l, W, H, dcx, dcy, &keys, &ff, &menu) && keys == 0, "d-pad centre is neutral");
    CHECK(np_tc_hit(&l, W, H, fcx + 0.95f * u, dcy, &keys, &ff, &menu) && keys == NP_KEY_A, "A east");
    CHECK(np_tc_hit(&l, W, H, fcx, dcy - 0.95f * u, &keys, &ff, &menu) && keys == NP_KEY_X, "X north");
    CHECK(np_tc_hit(&l, W, H, m + 0.5f * u, m + 0.3f * u, &keys, &ff, &menu) && keys == NP_KEY_L, "L corner");
    CHECK(np_tc_hit(&l, W, H, W * 0.5f + 0.95f * u, m + 0.3f * u, &keys, &ff, &menu) && menu && !keys, "menu");
    CHECK(!np_tc_hit(&l, W, H, W * 0.5f, H * 0.5f, &keys, &ff, &menu), "screen centre is no control");
    CHECK(np_tc_pick(&l, W, H, fcx + 0.95f * u, dcy) == NP_TC_A, "pick A");

    np_tc_layout moved = l, back;
    moved.item[NP_TC_B].cx = 0.25f;
    moved.item[NP_TC_B].opacity = 0.4f;
    char text[1024];
    int n = np_tc_format(&moved, text, sizeof text);
    np_tc_default(&back, 100, 100);
    int ok = n > 0;
    for (char *line = text, *eol; ok && (eol = strchr(line, '\n')); line = eol + 1) {
        *eol = '\0';
        char *eq = strchr(line, '=');
        eq[-1] = '\0';
        ok = !np_tc_parse(&back, line, eq + 1);
    }
    CHECK(ok && fabsf(back.item[NP_TC_B].cx - 0.25f) < 1e-4f && fabsf(back.item[NP_TC_B].opacity - 0.4f) < 1e-3f &&
              fabsf(back.item[NP_TC_DPAD].w - moved.item[NP_TC_DPAD].w) < 1e-4f,
          "touch layout round trip");
    CHECK(np_tc_parse(&back, "zz", "0 0 1 1 1") && np_tc_parse(&back, "a", "0.5 0.5 nan 1 1") &&
              np_tc_parse(&back, "a", "0.5 0.5"),
          "bad touch lines refused");
    np_tc_item it = {2, -1, 0, 9, 0};
    np_tc_clamp(&it);
    CHECK(it.cx == 1 && it.cy == 0 && it.w == 0.05f && it.h == 1 && it.opacity == 0.1f, "touch item clamp");
}

/* A Delta DS skin's info.json: representation choice, items, screens and
 * extended edges; and screens placed at a skin's frames map the stylus. */
static void test_skin(void)
{
    static const char json[] =
        "{\"name\":\"Test \\u00e9\",\"identifier\":\"x.test\",\"gameTypeIdentifier\":\"com.rileytestut.delta.game.ds\","
        "\"representations\":{\"ipad\":{\"standard\":{\"landscape\":{\"assets\":{\"large\":\"ipad.png\"},"
        "\"mappingSize\":{\"width\":10,\"height\":10},\"screens\":[{\"outputFrame\":{\"x\":0,\"y\":0,\"width\":1,"
        "\"height\":1}}]}}},\"iphone\":{\"edgeToEdge\":{\"portrait\":{\"assets\":{\"resizable\":\"p.pdf\"},"
        "\"mappingSize\":{\"width\":375,\"height\":812},\"extendedEdges\":{\"top\":5,\"bottom\":5,\"left\":5,\"right\":5},"
        "\"translucent\":true,\"items\":[{\"inputs\":{\"up\":\"up\",\"down\":\"down\",\"left\":\"left\",\"right\":"
        "\"right\"},\"frame\":{\"x\":20,\"y\":600,\"width\":120,\"height\":120}},{\"inputs\":[\"a\"],\"frame\":{\"x\":300,"
        "\"y\":620,\"width\":50,\"height\":50},\"extendedEdges\":{\"right\":20}},{\"inputs\":[\"menu\"],\"frame\":{"
        "\"x\":170,\"y\":760,\"width\":30,\"height\":30}},{\"inputs\":{\"x\":\"touchScreenX\",\"y\":\"touchScreenY\"},"
        "\"frame\":{\"x\":0,\"y\":290,\"width\":375,\"height\":281}},{\"inputs\":[\"thumbstick\"],\"frame\":{\"x\":0,"
        "\"y\":0,\"width\":1,\"height\":1}}],\"screens\":[{\"inputFrame\":{\"x\":0,\"y\":0,\"width\":256,\"height\":192},"
        "\"outputFrame\":{\"x\":0,\"y\":0,\"width\":375,\"height\":281}},{\"inputFrame\":{\"x\":0,\"y\":192,\"width\":256,"
        "\"height\":192},\"outputFrame\":{\"x\":0,\"y\":290,\"width\":375,\"height\":281}}]}}}}}";
    static np_skin_def d;
    const char *why = "";
    CHECK(!np_skin_parse(json, sizeof json - 1, &d, &why), "skin parsed (%s)", why);
    const np_skin_rep *p = &d.rep[1], *l = &d.rep[0];
    CHECK(!strcmp(d.name, "Test \xc3\xa9") && p->present && p->asset_is_pdf && !strcmp(p->asset, "p.pdf") &&
              p->translucent && p->nitems == 4 && p->nscreens == 2,
          "portrait from iphone/edgeToEdge (%d items)", p->nitems);
    CHECK(l->present && !l->asset_is_pdf && !strcmp(l->asset, "ipad.png"), "landscape falls back to ipad");
    CHECK(p->item[0].action == NP_SKIN_DPAD && p->item[1].keys == NP_KEY_A && p->item[2].action == NP_SKIN_MENU &&
              p->item[3].action == NP_SKIN_TOUCH_SCREEN,
          "skin item kinds");
    /* An item's extendedEdges override the skin's per edge. */
    CHECK(p->item[0].hit.x == 15 && p->item[0].hit.w == 130 && p->item[1].hit.w == 75 && p->item[1].hit.x == 295,
          "extended edges (default and per item)");
    CHECK(np_skin_item_at(p, 345, 640) == 1 && np_skin_item_at(p, 80, 660) == 0 && np_skin_item_at(p, 1, 1) == -1,
          "skin item lookup");
    CHECK(np_skin_dpad_keys(&p->item[0], 30, 660) == NP_KEY_LEFT && np_skin_dpad_keys(&p->item[0], 80, 660) == 0,
          "skin d-pad");
    CHECK(p->screen[1].input.y == 192 && p->screen[1].output.y == 290, "skin screens");
    static const char *const bad[] = {
        "{\"gameTypeIdentifier\":\"com.rileytestut.delta.game.gba\",\"representations\":{}}",
        "{\"gameTypeIdentifier\":\"com.rileytestut.delta.game.ds\",\"representations\":{}}",
        "{\"gameTypeIdentifier\":\"com.rileytestut.delta.game.ds\",\"representations\":{\"iphone\":{\"standard\":{"
        "\"portrait\":{\"assets\":{\"small\":\"../x.png\"},\"mappingSize\":{\"width\":1,\"height\":1},\"screens\":[{"
        "\"outputFrame\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}}]}}}}}",
        "not json"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(np_skin_parse(bad[i], strlen(bad[i]), &d, &why), "skin %zu refused", i);

    /* Bottom screen at (100, 400) 512x384 in a 800x900 window. */
    float frames[8] = {100, 0, 512, 384, 100, 400, 512, 384};
    np_layout lay;
    np_layout_compute(&lay, &(np_layout_params){NP_LAYOUT_HORIZONTAL, 1, 1, NP_SCALE_FIT, 256, frames}, 800, 900);
    int tx, ty;
    CHECK(np_layout_touch(&lay, 100.5f + 2 * 10, 400.5f + 2 * 20, 0, &tx, &ty) && tx == 10 && ty == 20,
          "stylus through a skin frame (%d,%d)", tx, ty);
    CHECK(!np_layout_touch(&lay, 300, 200, 0, &tx, &ty), "top screen at its skin frame is not the stylus");
    float hidden[8] = {0, 0, 0, 0, 0, 0, 400, 300};
    np_layout_compute(&lay, &(np_layout_params){NP_LAYOUT_VERTICAL, 0, 0, NP_SCALE_FIT, 256, hidden}, 800, 900);
    CHECK(!lay.screen[0].visible && lay.screen[1].visible && lay.screen[1].w == 400, "skin hides a screen");
}

/* RMS of a filtered stereo sine after the filter settles. */
static double lowpass_rms(int stages, double hz)
{
    enum { RATE = 32728, N = 8192 };
    static int16_t buf[N * 2];
    for (int i = 0; i < N; i++)
        buf[2 * i] = buf[2 * i + 1] = (int16_t)(16000.0 * sin(2 * 3.14159265358979 * hz * i / RATE));
    np_lowpass f = {0};
    np_lowpass_config(&f, stages, RATE);
    np_lowpass_run(&f, buf, N);
    double sum = 0;
    for (int i = N / 2; i < N; i++)
        sum += (double)buf[2 * i] * buf[2 * i];
    return sqrt(sum / (N / 2));
}

/* Off passes through; each stage cuts treble more and leaves the bass. */
static void test_lowpass(void)
{
    double ref = 16000.0 / sqrt(2.0);
    CHECK(fabs(lowpass_rms(0, 12000) - ref) < 20, "low-pass off is transparent");
    double hi1 = lowpass_rms(1, 12000), hi2 = lowpass_rms(2, 12000), hi3 = lowpass_rms(3, 12000);
    CHECK(hi1 < ref * 0.6 && hi2 < hi1 * 0.7 && hi3 < hi2 * 0.7, "treble falls per stage (%.0f %.0f %.0f)", hi1, hi2,
          hi3);
    CHECK(lowpass_rms(3, 200) > ref * 0.97, "bass passes 3X (%.0f)", lowpass_rms(3, 200));
    np_lowpass f = {0};
    int16_t loud[4] = {32767, -32768, 32767, -32768};
    np_lowpass_config(&f, 3, 32728);
    np_lowpass_run(&f, loud, 2);
    CHECK(loud[0] <= 32767 && loud[1] >= -32768, "low-pass stays in range");
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
    test_zip();
    test_modpkg();
    test_sha256();
    test_release();
    test_json();
    test_touchlayout();
    test_skin();
    test_lowpass();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
