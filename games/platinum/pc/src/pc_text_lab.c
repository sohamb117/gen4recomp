/*
 * The text lab: decode every message bank through the game's own path and
 * prove the glyphs reach a window.
 *
 * The offline decoder (pc/tests/pc_text.py) is an oracle for the two ciphers
 * in src/message.c: it can say a bank decodes to the English it was compiled
 * from. It cannot say the font has a glyph for every index, or that RenderText
 * can walk the control codes without hanging. This walks the same sequence a
 * dialogue box uses and fingerprints the window the printer wrote. A glyph past
 * the font traps here with a bank and entry number, instead of silently
 * becoming a question mark.
 *
 * It does not open a field or wait for a script. Fonts, heaps and the
 * filesystem are up before the first VBlank, so running it from a station save
 * would spend a minute booting to throw the boot away.
 *
 * The printer's 1024-step cap. Text_AddPrinter's instant path loops RenderText
 * at most 1024 times, and a page break waits 100 ticks unless auto-scroll is on
 * and A is new. 255 messages have more page breaks than that cap absorbs, so
 * the lab holds A and sets AUTO_SCROLL_NO_WAIT, which is what a player mashing
 * A does.
 *
 * Lines longer than the window. Window_CopyGlyph's dest is u8 tiles, and 44
 * messages have a run of glyphs that would walk off even that and smash the
 * pixel buffer. The printer does not wrap, so the lab inserts CHAR_CR, and
 * CHAR_CONTROL_CLEAR when the next line would walk off the bottom. The wrap is
 * the lab's, not the game's.
 *
 * Trainer names. Bank 618 is 885 compressed trainer names. The game never
 * feeds those to RenderText as a format string; they are template arguments
 * that go through String_ConcatTrainerName, so the lab decompresses them first.
 *
 * The digest is FNV-1a 64, the same function --state-digest uses, over the
 * window's 4bpp tile buffer after each message, folded into one value per bank.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
static int lab_mkdir(const char *d) { return _mkdir(d); }
#else
static int lab_mkdir(const char *d) { return mkdir(d, 0755); }
#endif

#include "pch/global_pch.h"

#include "constants/charcode.h"
#include "constants/graphics.h"
#include "constants/heap.h"
#include "constants/narc.h"

#include "bg_window.h"
#include "charcode.h"
#include "font.h"
#include "font_manager.h"
#include "heap.h"
#include "message.h"
#include "narc.h"
#include "render_text.h"
#include "string_gf.h"
#include "string_template.h"
#include "system.h"
#include "text.h"

#include "pc_png.h"
#include "pc_state.h"

#include "res/fonts/pl_font.naix"

#define LAB_TEXT_HEAP     HEAP_ID_POKEDEX
#define LAB_TEXT_HEAP_SZ  0x20000 /* same carve the sprite lab could take */
#define LAB_TEXT_AT_DEF   2
#define LAB_MAX_LIST      64
#define LAB_STR_CHARS     2048
#define LAB_GET_CHARS     1024
#define LAB_MAX_ARGS      24
#define LAB_WIN_W_TILES   32
#define LAB_WIN_H_TILES   24
#define LAB_WIN_PX_W      (LAB_WIN_W_TILES * TILE_WIDTH_PIXELS)
#define LAB_WIN_PX_H      (LAB_WIN_H_TILES * TILE_HEIGHT_PIXELS)
#define LAB_PIN_BANK_A    344
#define LAB_PIN_BANK_B    546

enum lab_text_mode {
    LAB_TEXT_NONE = 0,
    LAB_TEXT_ALL,
    LAB_TEXT_PIN,
    LAB_TEXT_LIST
};

static enum lab_text_mode sMode;
static unsigned long sAtFrame = LAB_TEXT_AT_DEF;
static int sDone;
static int sParsed;
static u16 sList[LAB_MAX_LIST];
static int sListN;
static FILE *sOut;
static const char *sDumpDir;
static int sRendered;
static int sWrapped;
static int sFailed;
static int sBanksDone;
static u32 sNumGlyphs;
static uint64_t sPinFnv[2];
static int sPinGot[2];

static void lab_say(const char *fmt, ...)
{
    va_list ap;
    va_list aq;

    va_start(ap, fmt);
    va_copy(aq, ap);
    vfprintf(stderr, fmt, ap);
    if (sOut != NULL) vfprintf(sOut, fmt, aq);
    va_end(aq);
    va_end(ap);
    fflush(stderr);
    if (sOut != NULL) fflush(sOut);
}

static u32 lab_read_num_glyphs(enum HeapID heapID)
{
    FontHeader hdr;

    (void)heapID;
    memset(&hdr, 0, sizeof hdr);
    NARC_ReadFromMemberByIndexPair(&hdr, NARC_INDEX_GRAPHIC__PL_FONT,
                                   font_message_NFGR, 0, sizeof hdr);
    return hdr.numGlyphs;
}

/*
 * Walk the way RenderText does: skip format-arg sequences and the
 * printer's own control codes, and refuse anything it would pass to
 * Font_TryLoadGlyph that is 0 or past the font. The font remaps those
 * to CHAR_QUESTION; the lab is the place that says so.
 */
static int lab_check_glyphs(const charcode_t *str, u32 bank, u32 entry)
{
    while (*str != CHAR_EOS) {
        if (*str == CHAR_FORMAT_ARG) {
            str = CharCode_SkipFormatArg(str);
            continue;
        }
        if (*str == CHAR_CR || *str == CHAR_CONTROL_CLEAR
                || *str == CHAR_CONTROL_SCROLL) {
            str++;
            continue;
        }
        if (*str == CHAR_PLACEHOLDER_BEGIN) {
            str++;
            if (*str != CHAR_EOS) str++;
            continue;
        }
        if (*str == 0 || *str > sNumGlyphs) {
            lab_say("pc_lab: text FAIL bank=%u entry=%u glyph=%u "
                    "numGlyphs=%u, Font_TryLoadGlyph would remap this "
                    "to '?'\n",
                    bank, entry, (unsigned)*str, sNumGlyphs);
            sFailed++;
            return 0;
        }
        str++;
    }
    return 1;
}

/*
 * The printer does not wrap. destX / destY are u16 and the clip in
 * Window_CopyGlyph underflows when they pass the window, so a 471-glyph
 * line would write off the end of the tile buffer. Insert CHAR_CR at
 * the right edge and CHAR_CONTROL_CLEAR at the bottom so every glyph
 * still goes through RenderText inside the window the lab actually has.
 */
static int lab_fit(String *dst, const String *src, enum Font font)
{
    const charcode_t *c = String_GetData(src);
    u32 x = 0;
    u32 y = 0;
    u8 letterH = Font_GetAttribute((u8)font, FONTATTR_MAX_LETTER_HEIGHT);
    int wrapped = 0;

    String_Clear(dst);
    while (*c != CHAR_EOS) {
        if (*c == CHAR_FORMAT_ARG) {
            const charcode_t *end = CharCode_SkipFormatArg(c);
            u32 type = CharCode_FormatArgType(c);

            if (type == CHAR_CONTROL_CURSOR_X) {
                x = CharCode_FormatArgParam(c, 0);
            } else if (type == CHAR_CONTROL_CURSOR_Y) {
                y = CharCode_FormatArgParam(c, 0);
            } else if (type == CHAR_CONTROL_MOVE) {
                u16 p = CharCode_FormatArgParam(c, 0);

                if (p == 0xFE00 || p == 0xFE01) {
                    x = 0;
                    if (p == 0xFE01) y = 0;
                }
            }
            while (c < end) String_AppendChar(dst, *c++);
            continue;
        }
        if (*c == CHAR_CR) {
            x = 0;
            y += letterH;
            String_AppendChar(dst, *c++);
            continue;
        }
        if (*c == CHAR_CONTROL_CLEAR || *c == CHAR_CONTROL_SCROLL) {
            x = 0;
            if (*c == CHAR_CONTROL_CLEAR) y = 0;
            else y += letterH;
            String_AppendChar(dst, *c++);
            continue;
        }
        if (*c == CHAR_PLACEHOLDER_BEGIN) {
            String_AppendChar(dst, *c++);
            if (*c != CHAR_EOS) String_AppendChar(dst, *c++);
            continue;
        }

        {
            const TextGlyph *g = Font_TryLoadGlyph(font, *c);
            u32 w = g->width;
            u32 h = g->height ? g->height : letterH;

            if (x > 0 && x + w > LAB_WIN_PX_W) {
                if (y + letterH + h > LAB_WIN_PX_H) {
                    String_AppendChar(dst, CHAR_CONTROL_CLEAR);
                    y = 0;
                } else {
                    String_AppendChar(dst, CHAR_CR);
                    y += letterH;
                }
                x = 0;
                wrapped = 1;
            }
            if (y + h > LAB_WIN_PX_H) {
                String_AppendChar(dst, CHAR_CONTROL_CLEAR);
                x = 0;
                y = 0;
                wrapped = 1;
            }
            String_AppendChar(dst, *c++);
            x += w;
        }
    }
    return wrapped;
}

static int lab_dump_png(const Window *window, const char *path)
{
    unsigned char *raw, *z;
    size_t rawn, zcap;
    unsigned y, x;
    int ok;
    const u8 *tiles = window->pixels;

    rawn = (size_t)LAB_WIN_PX_H * (1 + 3 * LAB_WIN_PX_W);
    zcap = rawn + rawn / 2 + 256;
    raw = malloc(rawn);
    z = malloc(zcap);
    if (raw == NULL || z == NULL) {
        free(raw);
        free(z);
        fprintf(stderr, "pc_lab: text: out of host memory writing %s\n", path);
        return 0;
    }
    for (y = 0; y < LAB_WIN_PX_H; y++) {
        unsigned char *row = raw + y * (1 + 3 * LAB_WIN_PX_W);
        unsigned ty = y / 8;
        unsigned iy = y % 8;

        row[0] = 0;
        for (x = 0; x < LAB_WIN_PX_W; x++) {
            unsigned tx = x / 8;
            unsigned ix = x % 8;
            const u8 *tile = tiles + ((ty * LAB_WIN_W_TILES + tx) * TILE_SIZE_4BPP);
            u8 packed = tile[iy * 4 + ix / 2];
            u8 idx = (ix & 1) ? (u8)(packed >> 4) : (u8)(packed & 0x0F);
            /* 4bpp text: 15 is the fill, 1 is the letter, 2 is the shadow. */
            unsigned char g = (unsigned char)(idx == 15 ? 240
                                              : idx == 1 ? 16
                                              : idx == 2 ? 80
                                              : (unsigned)idx * 16);

            row[1 + 3 * x + 0] = g;
            row[1 + 3 * x + 1] = g;
            row[1 + 3 * x + 2] = g;
        }
    }
    ok = pc_png_write_raw(path, LAB_WIN_PX_W, LAB_WIN_PX_H, raw, rawn,
                          z, zcap, "pc_lab: text");
    free(raw);
    free(z);
    return ok;
}

static int lab_expect(const charcode_t *got, const char *ascii, u32 bank, u32 entry)
{
    const char *p;
    const charcode_t *g = got;

    for (p = ascii; *p != '\0'; p++, g++) {
        charcode_t want;

        if (*p >= 'A' && *p <= 'Z') want = (charcode_t)(CHAR_A + (*p - 'A'));
        else if (*p >= 'a' && *p <= 'z') want = (charcode_t)(CHAR_a + (*p - 'a'));
        else if (*p >= '0' && *p <= '9') want = (charcode_t)(CHAR_0 + (*p - '0'));
        else if (*p == ' ') want = CHAR_SPACE;
        else if (*p == '-') want = CHAR_MINUS;
        else {
            fprintf(stderr, "pc_lab: text: pin string has a char this "
                    "check does not name: '%c'\n", *p);
            return 0;
        }
        if (*g != want) {
            lab_say("pc_lab: text FAIL bank=%u entry=%u pin: got char "
                    "%u, want '%c' (%u)\n",
                    bank, entry, (unsigned)*g, *p, (unsigned)want);
            sFailed++;
            return 0;
        }
    }
    if (*g != CHAR_EOS) {
        lab_say("pc_lab: text FAIL bank=%u entry=%u pin: expected EOS "
                "after \"%s\", got %u\n",
                bank, entry, ascii, (unsigned)*g);
        sFailed++;
        return 0;
    }
    return 1;
}

static int lab_one(Window *window, StringTemplate *tmpl,
                   String *raw, String *formatted, String *fitted,
                   charcode_t *getbuf, const MessageBank *bank,
                   u32 bankID, u32 entry, uint64_t *bankFnv)
{
    u32 pixBytes;
    int wrapped;
    char dump[512];

    MessageBank_Get(bank, entry, getbuf);
    String_CopyChars(raw, getbuf);

    if (String_IsTrainerName(raw)) {
        String_Clear(formatted);
        String_ConcatTrainerName(formatted, raw);
        String_Copy(raw, formatted);
    }

    if (bankID == LAB_PIN_BANK_A && entry == 0) {
        if (!lab_expect(String_GetData(raw), "Start debug", bankID, entry)) {
            return 0;
        }
    } else if (bankID == LAB_PIN_BANK_B && entry == 4) {
        if (!lab_expect(String_GetData(raw), "Sound Test", bankID, entry)) {
            return 0;
        }
    }

    StringTemplate_Format(tmpl, formatted, raw);
    if (!lab_check_glyphs(String_GetData(formatted), bankID, entry)) {
        return 0;
    }

    wrapped = lab_fit(fitted, formatted, FONT_MESSAGE);
    if (wrapped) sWrapped++;

    Window_FillTilemap(window, Font_GetAttribute(FONT_MESSAGE, FONTATTR_BG_COLOR));
    Text_AddPrinterWithParams(window, FONT_MESSAGE, fitted, 0, 0,
                              TEXT_SPEED_NO_TRANSFER, NULL);

    pixBytes = (u32)window->width * (u32)window->height * TILE_SIZE_4BPP;
    *bankFnv = pc_state_fnv1a(*bankFnv, window->pixels, pixBytes);
    sRendered++;

    if (sDumpDir != NULL) {
        snprintf(dump, sizeof dump, "%s/bank-%u-%u.png",
                 sDumpDir, bankID, entry);
        if (!lab_dump_png(window, dump)) {
            sFailed++;
            return 0;
        }
    }
    return 1;
}

static int lab_bank(Window *window, StringTemplate *tmpl,
                    String *raw, String *formatted, String *fitted,
                    charcode_t *getbuf, enum HeapID heapID, u32 bankID)
{
    MessageBank *bank;
    u32 n, i, size;
    uint64_t fnv = PC_STATE_FNV64_OFFSET;
    int before = sRendered;
    int wrapBefore = sWrapped;

    (void)heapID;
    /*
     * The bank lives in host memory, not on a guest heap. APPLICATION is
     * the opening cutscene's heap at frame 2, and a 314 KB bank does not
     * fit in the 128 KB child the sprite lab already measured as the
     * largest carve that succeeds. MessageBank_Get only reads the blob;
     * it does not care who allocated it.
     */
    size = NARC_GetMemberSizeByIndexPair(NARC_INDEX_MSGDATA__PL_MSG, bankID);
    bank = malloc(size);
    if (bank == NULL || size < 4) {
        lab_say("pc_lab: text FAIL bank=%u host alloc %u bytes failed\n",
                bankID, size);
        free(bank);
        sFailed++;
        return 0;
    }
    NARC_ReadWholeMemberByIndexPair(bank, NARC_INDEX_MSGDATA__PL_MSG, bankID);
    n = MessageBank_EntryCount(bank);
    for (i = 0; i < n; i++) {
        if (!lab_one(window, tmpl, raw, formatted, fitted, getbuf,
                     bank, bankID, i, &fnv)) {
            free(bank);
            return 0;
        }
    }
    free(bank);
    sBanksDone++;

    lab_say("pc_lab: text bank=%u entries=%u rendered=%d wrapped=%d "
            "fnv=%016llx\n",
            bankID, n, sRendered - before, sWrapped - wrapBefore,
            (unsigned long long)fnv);

    if (bankID == LAB_PIN_BANK_A) {
        sPinFnv[0] = fnv;
        sPinGot[0] = 1;
    } else if (bankID == LAB_PIN_BANK_B) {
        sPinFnv[1] = fnv;
        sPinGot[1] = 1;
    }
    return 1;
}

static void lab_parse(void)
{
    const char *s = getenv("PC_LAB_TEXT");
    const char *at = getenv("PC_LAB_TEXT_AT");
    const char *out = getenv("PC_LAB_TEXT_OUT");
    const char *dump = getenv("PC_LAB_TEXT_DUMP");

    sParsed = 1;
    if (s == NULL || s[0] == '\0') return;
    if (strcmp(s, "all") == 0) {
        sMode = LAB_TEXT_ALL;
    } else if (strcmp(s, "pin") == 0) {
        sMode = LAB_TEXT_PIN;
    } else {
        const char *p = s;

        sMode = LAB_TEXT_LIST;
        while (*p != '\0' && sListN < LAB_MAX_LIST) {
            char *end;
            unsigned long v = strtoul(p, &end, 0);

            if (end == p) {
                fprintf(stderr, "pc_lab: PC_LAB_TEXT wants 'all', 'pin'"
                        " or a comma-separated bank list, got '%s'\n", s);
                exit(2);
            }
            sList[sListN++] = (u16)v;
            p = (*end == ',') ? end + 1 : end;
        }
        if (sListN == 0) {
            fprintf(stderr, "pc_lab: PC_LAB_TEXT list was empty\n");
            exit(2);
        }
    }
    if (at != NULL && at[0] != '\0') sAtFrame = strtoul(at, NULL, 0);
    if (out != NULL && out[0] != '\0') {
        sOut = fopen(out, "w");
        if (sOut == NULL) {
            fprintf(stderr, "pc_lab: cannot write %s\n", out);
            exit(2);
        }
    }
    if (dump != NULL && dump[0] != '\0') {
        sDumpDir = dump;
        if (lab_mkdir(dump) != 0 && errno != EEXIST) {
            fprintf(stderr, "pc_lab: cannot create %s: %s\n", dump,
                    strerror(errno));
            exit(2);
        }
    }
}

static const u16 sPinBanks[] = {
    LAB_PIN_BANK_A, /* stripped debug menu: oracle, Latin */
    LAB_PIN_BANK_B, /* sound test: oracle, Latin */
    618,            /* compressed trainer names */
    304,            /* highest format-arg index (18) */
    108,            /* longest message (878 charcodes) */
    416,            /* most page-breaks */
};

static void lab_finish(void)
{
    lab_say("pc_lab: text done banks=%d rendered=%d wrapped=%d failed=%d "
            "numGlyphs=%u\n",
            sBanksDone, sRendered, sWrapped, sFailed, sNumGlyphs);
    if (sPinGot[0]) {
        lab_say("pc_lab: text pin bank=%u fnv=%016llx\n",
                LAB_PIN_BANK_A, (unsigned long long)sPinFnv[0]);
    }
    if (sPinGot[1]) {
        lab_say("pc_lab: text pin bank=%u fnv=%016llx\n",
                LAB_PIN_BANK_B, (unsigned long long)sPinFnv[1]);
    }
    if ((sMode == LAB_TEXT_PIN || sMode == LAB_TEXT_ALL)
            && (!sPinGot[0] || !sPinGot[1])) {
        lab_say("pc_lab: text FAIL pin banks %u and %u were not both "
                "rendered\n", LAB_PIN_BANK_A, LAB_PIN_BANK_B);
        if (sOut != NULL) fclose(sOut);
        exit(2);
    }
    if (sOut != NULL) fclose(sOut);
    if (sFailed) exit(2);
    exit(0);
}

void pc_lab_text_frame(unsigned long long frame)
{
    BgConfig *bg;
    Window window;
    StringTemplate *tmpl;
    String *raw, *formatted, *fitted;
    charcode_t *getbuf;
    NARC *narc;
    u32 nBanks, i;
    u32 savedKeys;

    if (!sParsed) lab_parse();
    if (sMode == LAB_TEXT_NONE || sDone) return;
    if (frame < sAtFrame) return;
    sDone = 1;

    fprintf(stderr, "pc_lab: text starting at frame %llu mode=%s\n",
            frame,
            sMode == LAB_TEXT_ALL ? "all"
            : sMode == LAB_TEXT_PIN ? "pin" : "list");
    fflush(stderr);

    if (!Heap_Create(HEAP_ID_APPLICATION, LAB_TEXT_HEAP, LAB_TEXT_HEAP_SZ)) {
        fprintf(stderr, "pc_lab: text: Heap_Create(%d, %u) failed --"
                " APPLICATION is too full at frame %llu (free=%u)\n",
                LAB_TEXT_HEAP, LAB_TEXT_HEAP_SZ, frame,
                HeapExp_FndGetTotalFreeSize(HEAP_ID_APPLICATION));
        exit(2);
    }

    sNumGlyphs = lab_read_num_glyphs(LAB_TEXT_HEAP);
    if (sNumGlyphs == 0) {
        fprintf(stderr, "pc_lab: text: font_message header has numGlyphs=0\n");
        exit(2);
    }

    /* Holds A so TextPrinter_WaitAutoMode returns on the first tick
     * instead of 100. AUTO_SCROLL_NO_WAIT is what a player mashing A
     * looks like to the printer; without it the 1024-step instant
     * loop cannot finish the 255 longest page-break messages. */
    RenderControlFlags_SetAutoScrollFlags(AUTO_SCROLL_NO_WAIT);
    savedKeys = (u16)gSystem.pressedKeys;
    gSystem.pressedKeys |= PAD_BUTTON_A;

    bg = BgConfig_New(LAB_TEXT_HEAP);
    if (bg == NULL) {
        fprintf(stderr, "pc_lab: text: BgConfig_New failed\n");
        exit(2);
    }
    /*
     * Window_FillTilemap sizes the fill from bgs[bgLayer].tileSize, and
     * Window_Add refuses a layer with no tilemap. AddToTopLeftCorner leaves
     * bgLayer=0xFF, so FillTilemap would index off the end of bgs[] and
     * paint 0x0F across the heap; that is how the first pin run smashed
     * the StringTemplate. A dummy layer 0 with a real tileSize is enough;
     * TEXT_SPEED_NO_TRANSFER never uploads it.
     */
    bg->bgs[BG_LAYER_MAIN_0].tilemapBuffer = Heap_Alloc(LAB_TEXT_HEAP, 0x800);
    bg->bgs[BG_LAYER_MAIN_0].bufferSize = 0x800;
    bg->bgs[BG_LAYER_MAIN_0].tileSize = TILE_SIZE_4BPP;
    bg->bgs[BG_LAYER_MAIN_0].colorMode = GX_BG_COLORMODE_16;
    bg->bgs[BG_LAYER_MAIN_0].type = BG_TYPE_STATIC;
    if (bg->bgs[BG_LAYER_MAIN_0].tilemapBuffer == NULL) {
        fprintf(stderr, "pc_lab: text: tilemap alloc failed\n");
        exit(2);
    }
    MI_CpuClear16(bg->bgs[BG_LAYER_MAIN_0].tilemapBuffer, 0x800);
    Window_Init(&window);
    Window_Add(bg, &window, BG_LAYER_MAIN_0, 0, 0,
               LAB_WIN_W_TILES, LAB_WIN_H_TILES, 0, 1);
    if (window.pixels == NULL) {
        fprintf(stderr, "pc_lab: text: Window_Add failed\n");
        exit(2);
    }

    tmpl = StringTemplate_New(LAB_MAX_ARGS, 64, LAB_TEXT_HEAP);
    raw = String_Init(LAB_STR_CHARS, LAB_TEXT_HEAP);
    formatted = String_Init(LAB_STR_CHARS, LAB_TEXT_HEAP);
    fitted = String_Init(LAB_STR_CHARS, LAB_TEXT_HEAP);
    getbuf = Heap_Alloc(LAB_TEXT_HEAP, LAB_GET_CHARS * sizeof(charcode_t));
    if (tmpl == NULL || raw == NULL || formatted == NULL || fitted == NULL
            || getbuf == NULL) {
        fprintf(stderr, "pc_lab: text: string / template alloc failed\n");
        exit(2);
    }

    narc = NARC_ctor(NARC_INDEX_MSGDATA__PL_MSG, LAB_TEXT_HEAP);
    if (narc == NULL) {
        fprintf(stderr, "pc_lab: text: could not open pl_msg\n");
        exit(2);
    }
    nBanks = NARC_GetFileCount(narc);
    NARC_dtor(narc);
    lab_say("pc_lab: text archive pl_msg banks=%u numGlyphs=%u\n",
            nBanks, sNumGlyphs);

    if (sMode == LAB_TEXT_ALL) {
        for (i = 0; i < nBanks; i++) {
            if (!lab_bank(&window, tmpl, raw, formatted, fitted, getbuf,
                          LAB_TEXT_HEAP, i)) {
                break;
            }
            if ((i % 50) == 49) {
                fprintf(stderr, "pc_lab: text progress banks=%u rendered=%d\n",
                        i + 1, sRendered);
                fflush(stderr);
            }
        }
    } else if (sMode == LAB_TEXT_PIN) {
        for (i = 0; i < (u32)(sizeof sPinBanks / sizeof sPinBanks[0]); i++) {
            if (sPinBanks[i] >= nBanks) {
                lab_say("pc_lab: text FAIL pin bank %u is past pl_msg "
                        "(%u banks)\n", sPinBanks[i], nBanks);
                sFailed++;
                break;
            }
            if (!lab_bank(&window, tmpl, raw, formatted, fitted, getbuf,
                          LAB_TEXT_HEAP, sPinBanks[i])) {
                break;
            }
        }
    } else {
        for (i = 0; i < (u32)sListN; i++) {
            if (sList[i] >= nBanks) {
                lab_say("pc_lab: text FAIL bank %u is past pl_msg "
                        "(%u banks)\n", sList[i], nBanks);
                sFailed++;
                break;
            }
            if (!lab_bank(&window, tmpl, raw, formatted, fitted, getbuf,
                          LAB_TEXT_HEAP, sList[i])) {
                break;
            }
        }
    }

    gSystem.pressedKeys = savedKeys;
    lab_finish();
}
