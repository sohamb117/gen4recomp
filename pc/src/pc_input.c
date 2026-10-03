/*
 * Scripted input: the keypad and the touch panel.
 *
 * Modelled on the sibling diamond port's pc_input.c but a level down in
 * ambition, because this port is a level down in hardware: there is no ARM7
 * and no SPI bus here, so the keypad is two words of memory and the touch
 * panel is pc_tp.c's sample, which TP_GetCalibratedResult hands back as-is, so
 * a script feeds screen coordinates.
 *
 * The keypad is two words, and both are active-low:
 *   reg_PAD_KEYINPUT (0x04000130)  a b select start right left up down r l,
 *                                  bits 0-9, a register the ARM9 reads.
 *   HW_BUTTON_XY_BUF (0x027FFFA8)  X (0x0400), Y (0x0800), DEBUG (0x2000),
 *                                  pre-shifted, refreshed by the ARM7 on
 *                                  hardware; bit 15 is the hinge and it is
 *                                  active-high (set means lid closed).
 *
 * PAD_Read ORs the two and XORs with 0x2FFF, so the idle state is not zero: a
 * zeroed XY buffer reads as X, Y and DEBUG held at once. pc_input_init()
 * writes 0x03FF and 0x2C00 before any guest code runs.
 *
 * The script (PC_INPUT=file) is frame-addressed and level-held: a line's state
 * persists until a later line replaces it, because held-versus-pressed is
 * derived by the game from consecutive frame levels. One frame is one VBlank.
 *
 *   # comment / blank lines ignored; lines must be frame-ascending
 *   150 keys START            # hold START from frame 150
 *   156 keys none             # release everything at frame 156
 *   400 keys A B              # chords are one line
 *   520 touch 128 96          # pen down at screen (128,96) on the lower LCD
 *   540 release               # pen up
 *
 * Applied at the top of OS_Halt, before the VBlank handler runs, so the frame
 * whose logic follows that VBlank sees the new level.
 */

#include <nitro/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void pc_tp_set(u16 x, u16 y, int touching);

#define PC_REG_KEYINPUT (*(volatile u16 *)0x04000130u)
#define PC_XY_BUF       (*(volatile u16 *)0x027FFFA8u)

#define PC_KEYPORT_MASK 0x03FFu
#define PC_XYPORT_MASK  0x2C00u

/* Button name -> PAD_Read bit. The names are the SDK's own, minus the
 * PAD_BUTTON_/PAD_KEY_ prefixes, because a script is typed by hand. */
static const struct { const char *name; u16 mask; } sButtons[] = {
    { "A",      0x0001 }, { "B",     0x0002 },
    { "SELECT", 0x0004 }, { "START", 0x0008 },
    { "RIGHT",  0x0010 }, { "LEFT",  0x0020 },
    { "UP",     0x0040 }, { "DOWN",  0x0080 },
    { "R",      0x0100 }, { "L",     0x0200 },
    { "X",      0x0400 }, { "Y",     0x0800 },
    { "DEBUG",  0x2000 },
};

typedef struct PcInputLine {
    unsigned long long frame;
    enum { PC_IN_KEYS, PC_IN_TOUCH, PC_IN_RELEASE } kind;
    u16 keys;           /* PAD_Read-positive mask for PC_IN_KEYS */
    u16 x, y;           /* PC_IN_TOUCH */
} PcInputLine;

static PcInputLine *sScript;
static int sScriptLen;
static int sScriptPos;

/*
 * PC_RECORD_INPUT=path, write what the port CONSUMES, as a PC_INPUT
 * script, one line per change, flushed as written so a crash keeps the
 * session. This exists because the first live session to reach a Pokémon
 * battle found a crash and left nothing replayable behind it: the port had
 * diamond's scripted-input half without its recording half. Levels are
 * sampled at the same frame boundary a replay would apply them, so a
 * recorded session replays against the same build frame-for-frame.
 */
static FILE *sRecord;
static u16 sHeld;                       /* PAD_Read-positive, as published */
static int sRecTouchOn, sRecTouchX, sRecTouchY;
static u16 sRecHeld = 0xFFFF;           /* impossible, so frame 0 records */
static int sRecOnFile = -1, sRecXFile = -1, sRecYFile = -1;

static void record_tick(unsigned long long frame)
{
    if (sRecord == NULL) {
        return;
    }
    if (sHeld != sRecHeld) {
        int i, any = 0;

        fprintf(sRecord, "%llu keys", frame);
        for (i = 0; i < (int)(sizeof sButtons / sizeof *sButtons); i++) {
            if (sHeld & sButtons[i].mask) {
                fprintf(sRecord, " %s", sButtons[i].name);
                any = 1;
            }
        }
        fprintf(sRecord, any ? "\n" : " none\n");
        sRecHeld = sHeld;
    }
    if (sRecTouchOn != sRecOnFile ||
        (sRecTouchOn && (sRecTouchX != sRecXFile || sRecTouchY != sRecYFile))) {
        if (sRecTouchOn) {
            fprintf(sRecord, "%llu touch %d %d\n", frame, sRecTouchX, sRecTouchY);
        } else {
            fprintf(sRecord, "%llu release\n", frame);
        }
        sRecOnFile = sRecTouchOn;
        sRecXFile = sRecTouchX;
        sRecYFile = sRecTouchY;
    }
    fflush(sRecord);
}

/* Publish a PAD_Read-positive mask as the two active-low words. */
static void publish_keys(u16 held)
{
    PC_REG_KEYINPUT = (u16)(~held & PC_KEYPORT_MASK);
    PC_XY_BUF       = (u16)(~held & PC_XYPORT_MASK);
    sHeld = held;
}

/* The one funnel for the pen, so the recorder sees screen pixels, pc_tp
 * stores raw counts and inverting those would be a second calibration. */
static void set_touch(int on, unsigned x, unsigned y)
{
    pc_tp_set((u16)x, (u16)y, on);
    sRecTouchOn = on;
    if (on) {
        sRecTouchX = (int)x;
        sRecTouchY = (int)y;
    }
}

static int parse_script(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int cap = 0, lineno = 0;

    if (f == NULL) {
        fprintf(stderr, "pc-input: cannot open PC_INPUT=%s\n", path);
        return 0;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        char word[32];
        unsigned long long frame;
        int off = 0;
        PcInputLine ent;

        lineno++;
        if (sscanf(line, " %31s%n", word, &off) != 1) continue;   /* blank */
        if (word[0] == '#') continue;
        if (sscanf(line, " %llu %31s%n", &frame, word, &off) != 2) {
            goto bad;
        }
        memset(&ent, 0, sizeof ent);
        ent.frame = frame;
        if (strcmp(word, "keys") == 0) {
            const char *p = line + off;

            ent.kind = PC_IN_KEYS;
            for (;;) {
                int n = 0, i, hit = 0;

                if (sscanf(p, " %31s%n", word, &n) != 1) break;
                p += n;
                if (strcmp(word, "none") == 0) continue;
                for (i = 0; i < (int)(sizeof sButtons / sizeof *sButtons); i++) {
                    if (strcmp(word, sButtons[i].name) == 0) {
                        ent.keys |= sButtons[i].mask;
                        hit = 1;
                        break;
                    }
                }
                if (!hit) goto bad;
            }
        } else if (strcmp(word, "touch") == 0) {
            unsigned x, y;

            if (sscanf(line + off, " %u %u", &x, &y) != 2) goto bad;
            ent.kind = PC_IN_TOUCH;
            ent.x = (u16)x;
            ent.y = (u16)y;
        } else if (strcmp(word, "release") == 0) {
            ent.kind = PC_IN_RELEASE;
        } else {
            goto bad;
        }

        if (sScriptLen > 0 && sScript[sScriptLen - 1].frame > ent.frame) {
            fprintf(stderr, "pc-input: %s:%d: frames must not decrease\n",
                    path, lineno);
            fclose(f);
            return 0;
        }
        if (sScriptLen == cap) {
            cap = cap ? cap * 2 : 64;
            sScript = realloc(sScript, (size_t)cap * sizeof *sScript);
            if (sScript == NULL) {
                fprintf(stderr, "pc-input: out of memory\n");
                fclose(f);
                return 0;
            }
        }
        sScript[sScriptLen++] = ent;
    }
    fclose(f);
    fprintf(stderr, "pc-input: %s: %d scripted event%s\n",
            path, sScriptLen, sScriptLen == 1 ? "" : "s");
    return 1;

bad:
    fprintf(stderr, "pc-input: %s:%d: cannot parse: %s", path, lineno, line);
    fclose(f);
    return 0;
}

/* Idle the two words (nothing pressed, lid open) and load PC_INPUT if set.
 * Called from pc_main.c after armrec_mem_init(); the KEYINPUT store it
 * replaces used to live inline there. */
int pc_input_init(void)
{
    extern void pc_tp_boot_userinfo(void);
    const char *path = getenv("PC_INPUT");

    publish_keys(0);
    pc_tp_boot_userinfo();
    set_touch(0, 0, 0);
    if (path != NULL && !parse_script(path)) {
        return 0;
    }
    {
        const char *rec = getenv("PC_RECORD_INPUT");

        if (rec != NULL && rec[0] != '\0') {
            sRecord = fopen(rec, "wb");
            if (sRecord == NULL) {
                fprintf(stderr, "pc-input: cannot open PC_RECORD_INPUT=%s\n",
                        rec);
                return 0;
            }
            fprintf(sRecord, "# recorded by PC_RECORD_INPUT, replay with "
                             "PC_INPUT=this-file\n");
            fprintf(stderr, "pc-input: recording consumed input to %s\n", rec);
        }
    }
    return 1;
}

/* The live producer's write side, pc/src/pc_view.c feeds the viewer's
 * keys and pen through here. Same two stores and the same tp seam the
 * script uses; `keys` is the PAD_Read-positive mask. */
void pc_input_live(unsigned keys, int touch_on, unsigned x, unsigned y)
{
    publish_keys((u16)keys);
    set_touch(touch_on ? 1 : 0, x, y);
}

/* Apply every script line due at or before `frame`. Called from OS_Halt
 * before the VBlank handler is dispatched. */
/*
 * The warp sweep sets this once it has arrived on a map. Its input script
 * presses A continuously so that arrival scenes finish, but A on a settled
 * overworld starts conversations and battles, and a battle that crashes is
 * a finding about battles, filed against the map the sweep happened to be
 * standing on. Holding the keypad idle keeps the two apart.
 */
static int sHoldIdle;

void pc_input_hold_idle(int on)
{
    if (on && !sHoldIdle) publish_keys(0);
    sHoldIdle = on;
}

void pc_input_frame(unsigned long long frame)
{
    if (sHoldIdle) {
        /* The script still advances, so releasing the hold resumes where the
         * run would have been rather than replaying what was skipped. */
        while (sScriptPos < sScriptLen && sScript[sScriptPos].frame <= frame) {
            sScriptPos++;
        }
        publish_keys(0);
        record_tick(frame);
        return;
    }

    while (sScriptPos < sScriptLen && sScript[sScriptPos].frame <= frame) {
        const PcInputLine *ent = &sScript[sScriptPos++];

        switch (ent->kind) {
        case PC_IN_KEYS:
            publish_keys(ent->keys);
            break;
        case PC_IN_TOUCH:
            set_touch(1, ent->x, ent->y);
            break;
        case PC_IN_RELEASE:
            set_touch(0, 0, 0);
            break;
        }
    }
    record_tick(frame);
}
