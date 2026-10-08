/*
 * Stand-in implementation of np_core.h so the shell can be built, run and
 * tested before the wasm2c game cores exist. It exercises every part of the
 * contract the shell depends on, deterministically:
 *   - reads the cartridge header (first 0x200 bytes) through host->rom_read
 *     and draws its 12-character title and game code,
 *   - draws an animated per-game test pattern with a frame counter, the RTC,
 *     the keypad state and the stylus point (so layouts, rotation and touch
 *     mapping can be checked by eye),
 *   - produces a sine tone at np_core_audio_rate() into a ~1 s ring buffer,
 *   - round-trips a 512 KiB backup image: a boot counter is loaded, bumped
 *     and stored again once writes go quiet or on np_core_save_flush,
 *   - exposes a small "main RAM" window at 0x02000000 via np_core_guest_ptr
 *     (frame counter at +0, keys at +8).
 * Built only when the shell is configured with NP_CORE=stub.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font8x8.h"
#include "np_core.h"
#include "np_guest_abi.h"

#define SAVE_SIZE 0x80000u
#define AUDIO_RATE 32728u
#define RING_FRAMES 32768u /* ~1 s, power of two */
#define RAM_BASE 0x02000000u
#define RAM_SIZE 0x10000u
#define SAVE_QUIET_FRAMES 30u
#define SW NP_SCREEN_W
#define SH NP_SCREEN_H

static const char save_magic[8] = {'N', 'P', 'S', 'T', 'U', 'B', '0', '1'};

struct np_core {
    np_game game;
    np_host host;
    uint32_t fb[2][SW * SH];
    uint64_t frame;
    char title[13];
    char code[5];
    uint8_t save[SAVE_SIZE];
    int save_dirty;
    uint32_t quiet;
    uint32_t boots;
    int16_t ring[RING_FRAMES * 2];
    uint64_t ring_w, ring_r, samples_made;
    double phase;
    uint8_t ram[RAM_SIZE];
    char error[160];
    uint32_t opt[NP_OPT_COUNT];
    uint32_t status[NP_STAT_COUNT];
};

static int core_live;
static char create_error[160];

int np_core_available(np_game game) { return (unsigned)game < NP_GAME_COUNT; }

const char *np_core_create_error(void) { return create_error; }

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

np_core *np_core_create(np_game game, const np_host *host, const char *const *options)
{
    (void)options;
    if ((unsigned)game >= NP_GAME_COUNT || !host || !host->rom_read || !host->save_load || !host->save_store) {
        snprintf(create_error, sizeof create_error, "invalid arguments");
        return NULL;
    }
    if (core_live) {
        snprintf(create_error, sizeof create_error, "a core already exists in this process");
        return NULL;
    }
    uint8_t header[0x200];
    if (host->rom_size < sizeof header || host->rom_read(host->user, 0, header, sizeof header)) {
        snprintf(create_error, sizeof create_error, "cannot read the cartridge header");
        return NULL;
    }
    np_core *c = calloc(1, sizeof *c);
    if (!c) {
        snprintf(create_error, sizeof create_error, "out of memory");
        return NULL;
    }
    c->game = game;
    c->host = *host;
    for (int i = 0; i < 12; i++) {
        char ch = (char)header[i];
        c->title[i] = ch >= 0x20 && ch < 0x7F ? ch : ' ';
    }
    for (int i = 11; i >= 0 && c->title[i] == ' '; i--)
        c->title[i] = '\0';
    for (int i = 0; i < 4; i++) {
        char ch = (char)header[0x0C + i];
        c->code[i] = ch >= 0x20 && ch < 0x7F ? ch : '?';
    }

    int r = host->save_load(host->user, c->save, SAVE_SIZE);
    if (r < 0) {
        snprintf(create_error, sizeof create_error, "the save could not be read");
        free(c);
        return NULL;
    }
    if (r == 0 || memcmp(c->save, save_magic, sizeof save_magic)) {
        memset(c->save, 0xFF, SAVE_SIZE);
        memcpy(c->save, save_magic, sizeof save_magic);
        c->boots = 0;
    } else {
        c->boots = le32(c->save + 8);
    }
    c->boots++;
    c->opt[NP_OPT_BGM_VOLUME] = c->opt[NP_OPT_SE_VOLUME] = 256;
    c->opt[NP_OPT_RENDER_SCALE] = 1;
    c->opt[NP_OPT_CAMERA_ZOOM] = 256;
    c->status[NP_STAT_FIELD_READY] = 1; /* the test pattern is always "in the field" */
    for (int i = 0; i < 4; i++)
        c->save[8 + i] = (uint8_t)(c->boots >> (8 * i));
    c->save_dirty = 1;
    core_live = 1;
    create_error[0] = '\0';
    return c;
}

/* ---- drawing ------------------------------------------------------------ */

static uint32_t rgb(unsigned r, unsigned g, unsigned b) { return (r & 255) << 16 | (g & 255) << 8 | (b & 255); }

static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t c)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < SH; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < SW; i++)
            fb[j * SW + i] = c;
}

static void text(uint32_t *fb, int x, int y, int scale, const char *s, uint32_t c)
{
    for (; *s; s++, x += 8 * scale) {
        const uint8_t *g = np_font_glyph((unsigned char)*s);
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if ((g[row] >> col) & 1)
                    fill(fb, x + col * scale, y + row * scale, scale, scale, c);
    }
}

static void text_center(uint32_t *fb, int y, int scale, const char *s, uint32_t c)
{
    text(fb, (SW - (int)strlen(s) * 8 * scale) / 2, y, scale, s, c);
}

/* Seconds since 2000-01-01 -> calendar fields (days-to-civil, H. Hinnant). */
static void civil(int64_t secs, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int64_t z = days + 10957 + 719468; /* 2000-01-01 is day 10957 of the Unix era */
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*mo <= 2));
    *h = (int)(rem / 3600);
    *mi = (int)(rem / 60 % 60);
    *s = (int)(rem % 60);
}

static void draw_top(np_core *c, const np_input *in)
{
    static const uint32_t base[NP_GAME_COUNT][3] = {
        [NP_GAME_DIAMOND] = {40, 80, 190}, [NP_GAME_PEARL] = {190, 80, 130},
        [NP_GAME_PLATINUM] = {120, 115, 100}, [NP_GAME_BLACK] = {70, 70, 80},
        [NP_GAME_WHITE] = {200, 200, 205}, [NP_GAME_HEARTGOLD] = {200, 160, 40},
        [NP_GAME_SOULSILVER] = {150, 160, 175},
        [NP_GAME_RUBY] = {0, 0, 0}, [NP_GAME_SAPPHIRE] = {0, 0, 0}, [NP_GAME_EMERALD] = {0, 0, 0}};
    const uint32_t *b = base[c->game];
    uint32_t *fb = c->fb[0];
    unsigned t = (unsigned)c->frame;
    for (int y = 0; y < SH; y++) {
        for (int x = 0; x < SW; x++) {
            unsigned stripe = ((unsigned)(x + y) + t * 2) / 16 % 2;
            unsigned k = 60 + (unsigned)y * 120 / SH + stripe * 25;
            fb[y * SW + x] = rgb(b[0] * k / 160, b[1] * k / 160, b[2] * k / 160);
        }
    }
    fill(fb, 0, 0, SW, 28, rgb(0, 0, 0));
    text_center(fb, 6, 2, c->title[0] ? c->title : "(NO TITLE)", rgb(255, 255, 255));
    char line[64];
    snprintf(line, sizeof line, "%s  STUB CORE", c->code);
    text_center(fb, 36, 1, line, rgb(255, 230, 120));
    snprintf(line, sizeof line, "FRAME %06llu", (unsigned long long)c->frame);
    text(fb, 8, 56, 2, line, rgb(255, 255, 255));
    snprintf(line, sizeof line, "BOOT %u", (unsigned)c->boots);
    text(fb, 8, 78, 1, line, rgb(255, 255, 255));

    /* Deterministic clock unless the host supplies one (see np_core.h). */
    int64_t now = c->host.rtc_now ? c->host.rtc_now(c->host.user)
                                  : 291031200 + (int64_t)(c->frame * 10000 / 598261); /* 2009-03-22 10:00 */
    int y, mo, d, h, mi, s;
    civil(now, &y, &mo, &d, &h, &mi, &s);
    snprintf(line, sizeof line, "RTC %04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    text(fb, 8, 92, 1, line, rgb(255, 255, 255));

    /* A square bouncing around the lower half shows motion and tearing. */
    int px = (int)(t * 3 % (2 * (SW - 24)));
    int py = (int)(t * 2 % (2 * (SH - 120 - 24)));
    if (px >= SW - 24)
        px = 2 * (SW - 24) - px;
    if (py >= SH - 120 - 24)
        py = 2 * (SH - 120 - 24) - py;
    fill(fb, px, 112 + py, 24, 24, rgb(255, 255, 255));
    fill(fb, px + 4, 116 + py, 16, 16, rgb(b[0], b[1], b[2]));

    if (in->lid_closed) {
        for (int i = 0; i < SW * SH; i++)
            fb[i] = (fb[i] >> 2) & 0x3F3F3F;
        text_center(fb, 88, 2, "LID CLOSED", rgb(255, 255, 255));
    }
}

static void draw_bottom(np_core *c, const np_input *in)
{
    static const char *const names[12] = {"A", "B", "SE", "ST", "RT", "LF", "UP", "DN", "R", "L", "X", "Y"};
    uint32_t *fb = c->fb[1];
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++)
            fb[y * SW + x] = (x % 16 == 0 || y % 16 == 0) ? rgb(48, 52, 64) : rgb(20, 22, 30);
    /* Edge markers make orientation obvious under rotation. */
    fill(fb, 0, 0, SW, 2, rgb(255, 80, 80));    /* top edge: red */
    fill(fb, 0, SH - 2, SW, 2, rgb(80, 160, 255)); /* bottom edge: blue */
    text(fb, 4, 4, 1, "TOP-LEFT", rgb(255, 120, 120));

    for (int i = 0; i < 12; i++) {
        int x = 8 + (i % 6) * 41, y = 18 + (i / 6) * 22;
        int on = (in->keys >> i) & 1;
        fill(fb, x, y, 36, 18, on ? rgb(255, 210, 80) : rgb(60, 64, 80));
        text(fb, x + (36 - (int)strlen(names[i]) * 8) / 2, y + 5, 1, names[i], on ? rgb(0, 0, 0) : rgb(200, 200, 210));
    }

    char line[48];
    if (in->touch) {
        int tx = in->touch_x, ty = in->touch_y;
        fill(fb, 0, ty, SW, 1, rgb(120, 255, 120));
        fill(fb, tx, 0, 1, SH, rgb(120, 255, 120));
        fill(fb, tx - 3, ty - 3, 7, 7, rgb(255, 255, 255));
        snprintf(line, sizeof line, "TOUCH %3d,%3d", tx, ty);
    } else {
        snprintf(line, sizeof line, "TOUCH  -");
    }
    text(fb, 8, SH - 16, 1, line, rgb(255, 255, 255));
}

/* ---- audio ---------------------------------------------------------------- */

static void make_audio(np_core *c, const np_input *in)
{
    static const double base_hz[NP_GAME_COUNT] = {
        [NP_GAME_DIAMOND] = 440.0, [NP_GAME_PEARL] = 493.88, [NP_GAME_PLATINUM] = 523.25,
        [NP_GAME_BLACK] = 587.33, [NP_GAME_WHITE] = 659.25,
        [NP_GAME_HEARTGOLD] = 698.46, [NP_GAME_SOULSILVER] = 783.99,
        [NP_GAME_RUBY] = 0.0, [NP_GAME_SAPPHIRE] = 0.0, [NP_GAME_EMERALD] = 0.0};
    /* Samples owed after `frame` frames at 59.8261 Hz, in integer math so the
     * stream never drifts. */
    uint64_t due = c->frame * (uint64_t)AUDIO_RATE * 10000u / 598261u;
    double hz = base_hz[c->game] * ((in->keys & NP_KEY_A) ? 1.5 : 1.0);
    double amp = in->lid_closed ? 0.0 : 6000.0;
    for (; c->samples_made < due; c->samples_made++) {
        int16_t v = (int16_t)(amp * sin(c->phase));
        c->phase += 2.0 * 3.14159265358979 * hz / AUDIO_RATE;
        if (c->phase > 2.0 * 3.14159265358979)
            c->phase -= 2.0 * 3.14159265358979;
        uint32_t i = (uint32_t)(c->ring_w % RING_FRAMES);
        c->ring[2 * i] = v;
        c->ring[2 * i + 1] = v;
        c->ring_w++;
        if (c->ring_w - c->ring_r > RING_FRAMES)
            c->ring_r = c->ring_w - RING_FRAMES; /* drop the oldest */
    }
}

uint32_t np_core_audio_rate(const np_core *core)
{
    (void)core;
    return AUDIO_RATE;
}

size_t np_core_audio_read(np_core *c, int16_t *stereo, size_t max_frames)
{
    size_t n = 0;
    while (n < max_frames && c->ring_r < c->ring_w) {
        uint32_t i = (uint32_t)(c->ring_r % RING_FRAMES);
        stereo[2 * n] = c->ring[2 * i];
        stereo[2 * n + 1] = c->ring[2 * i + 1];
        c->ring_r++;
        n++;
    }
    return n;
}

/* ---- frames, saves, memory ---------------------------------------------------- */

int np_core_save_flush(np_core *c)
{
    if (!c->save_dirty)
        return 0;
    if (c->host.save_store(c->host.user, c->save, SAVE_SIZE)) {
        snprintf(c->error, sizeof c->error, "save_store failed");
        return -1;
    }
    c->save_dirty = 0;
    return 0;
}

int np_core_run_frame(np_core *c, const np_input *in, np_frame *out)
{
    c->frame++;
    draw_top(c, in);
    draw_bottom(c, in);
    make_audio(c, in);
    for (int i = 0; i < 8; i++)
        c->ram[i] = (uint8_t)(c->frame >> (8 * i));
    c->ram[8] = (uint8_t)in->keys;
    c->ram[9] = (uint8_t)(in->keys >> 8);
    /* Like the real cores: persist once the backup chip has been idle. */
    if (c->save_dirty && ++c->quiet >= SAVE_QUIET_FRAMES) {
        c->quiet = 0;
        if (np_core_save_flush(c))
            return -1;
    }
    if (c->opt[NP_OPT_QUICKSAVE_SEQ] != c->status[NP_STAT_QUICKSAVE_SEQ]) {
        /* An in-game save request: bump the boot counter's neighbour so the
         * image really changes, then store it. */
        c->save[12]++;
        c->save_dirty = 1;
        c->status[NP_STAT_QUICKSAVE_RESULT] = np_core_save_flush(c) ? NP_QS_FAILED : NP_QS_SAVED;
        c->status[NP_STAT_QUICKSAVE_SEQ] = c->opt[NP_OPT_QUICKSAVE_SEQ];
    }
    out->screen[0] = c->fb[0];
    out->screen[1] = c->fb[1];
    out->width = SW;
    out->height = SH;
    out->stride = SW;
    out->number = c->frame;
    return 0;
}

/* ---- contract v2: options, status, snapshots ---------------------------------
 * Options are stored and echoed; NP_OPT_QUICKSAVE_SEQ is honoured at the
 * next frame by flushing the save, like the real cores' save request. */

void np_core_set_option(np_core *c, uint32_t opt, uint32_t value)
{
    if (opt < NP_OPT_COUNT)
        c->opt[opt] = value;
}

uint32_t np_core_get_option(const np_core *c, uint32_t opt) { return opt < NP_OPT_COUNT ? c->opt[opt] : 0; }

uint32_t np_core_status(const np_core *c, uint32_t status)
{
    return status < NP_STAT_COUNT ? c->status[status] : 0;
}

/* A snapshot is the whole core struct after a magic word; the host
 * callbacks it contains are this process's, which the contract allows. */
static const uint32_t state_magic = 0x5354554Eu;

size_t np_core_state_size(const np_core *c)
{
    (void)c;
    return sizeof state_magic + sizeof(np_core);
}

int np_core_state_save(np_core *c, void *dst, size_t cap, size_t *written)
{
    size_t need = np_core_state_size(c);
    if (cap < need)
        return -1;
    memcpy(dst, &state_magic, sizeof state_magic);
    memcpy((uint8_t *)dst + sizeof state_magic, c, sizeof *c);
    if (written)
        *written = need;
    return 0;
}

int np_core_state_load(np_core *c, const void *src, size_t len)
{
    uint32_t magic;
    if (len != np_core_state_size(c))
        return -1;
    memcpy(&magic, src, sizeof magic);
    if (magic != state_magic)
        return -1;
    uint32_t opt[NP_OPT_COUNT];
    memcpy(opt, c->opt, sizeof opt); /* a load does not change options */
    memcpy(c, (const uint8_t *)src + sizeof magic, sizeof *c);
    memcpy(c->opt, opt, sizeof opt);
    return 0;
}

uint8_t *np_core_guest_ptr(np_core *c, uint32_t guest_addr, uint32_t len)
{
    if (guest_addr < RAM_BASE || guest_addr - RAM_BASE > RAM_SIZE || len > RAM_SIZE - (guest_addr - RAM_BASE))
        return NULL;
    return c->ram + (guest_addr - RAM_BASE);
}

const char *np_core_last_error(const np_core *c) { return c->error; }

void np_core_destroy(np_core *c)
{
    if (!c)
        return;
    free(c);
    core_live = 0;
}
