/*
 * 3ds/src/3ds_replay.c: see 3ds_replay.h.
 */

#include "3ds_replay.h"

#include <stdio.h>
#include <string.h>

/*
 * Button name -> PAD_Read bit. pc/src/pc_input.c's table, copied rather than
 * shared: that file is not on this chain's include path and cannot be
 * compiled here, and the names are the SDK's own so neither copy is free to
 * drift. 3ds/tests/run.sh greps both and fails if they stop agreeing.
 */
static const struct {
    const char *name;
    uint16_t mask;
} sButtons[] = {
    { "A", 0x0001 },      { "B", 0x0002 },
    { "SELECT", 0x0004 }, { "START", 0x0008 },
    { "RIGHT", 0x0010 },  { "LEFT", 0x0020 },
    { "UP", 0x0040 },     { "DOWN", 0x0080 },
    { "R", 0x0100 },      { "L", 0x0200 },
    { "X", 0x0400 },      { "Y", 0x0800 },
    { "DEBUG", 0x2000 },
};

static ReplayEvent sScript[REPLAY_MAX];
static int sLen;
static int sPos;
static ReplayState sHeld;

uint16_t replay_button_mask(const char *name)
{
    int i;

    for (i = 0; i < (int)(sizeof sButtons / sizeof *sButtons); i++) {
        if (strcmp(name, sButtons[i].name) == 0) {
            return sButtons[i].mask;
        }
    }
    return 0;
}

int replay_parse_line(const char *line, ReplayEvent *out)
{
    char word[32];
    unsigned long long frame;
    int off = 0;

    if (sscanf(line, " %31s%n", word, &off) != 1) {
        return 0;                      /* blank */
    }
    if (word[0] == '#') {
        return 0;
    }
    if (sscanf(line, " %llu %31s%n", &frame, word, &off) != 2) {
        return -1;
    }

    memset(out, 0, sizeof *out);
    out->frame = (uint32_t)frame;

    if (strcmp(word, "keys") == 0) {
        const char *p = line + off;

        out->kind = REPLAY_KEYS;
        for (;;) {
            int n = 0;
            uint16_t mask;

            if (sscanf(p, " %31s%n", word, &n) != 1) {
                break;
            }
            p += n;
            if (strcmp(word, "none") == 0) {
                continue;
            }
            mask = replay_button_mask(word);
            if (mask == 0) {
                return -1;
            }
            out->keys |= mask;
        }
        return 1;
    }
    if (strcmp(word, "touch") == 0) {
        unsigned x, y;

        if (sscanf(line + off, " %u %u", &x, &y) != 2) {
            return -1;
        }
        out->kind = REPLAY_TOUCH;
        out->x = (uint16_t)x;
        out->y = (uint16_t)y;
        return 1;
    }
    if (strcmp(word, "release") == 0) {
        out->kind = REPLAY_RELEASE;
        return 1;
    }
    return -1;
}

void replay_reset(void)
{
    sLen = 0;
    sPos = 0;
    memset(&sHeld, 0, sizeof sHeld);
}

int replay_init(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int lineno = 0;

    replay_reset();
    if (f == NULL) {
        return 0;                      /* the ordinary case: no script */
    }

    while (fgets(line, sizeof line, f) != NULL) {
        ReplayEvent ev;
        int r;

        lineno++;
        r = replay_parse_line(line, &ev);
        if (r == 0) {
            continue;
        }
        if (r < 0) {
            fprintf(stderr, "3ds-replay: %s:%d: cannot parse\n", path, lineno);
            goto refuse;
        }
        if (sLen > 0 && sScript[sLen - 1].frame > ev.frame) {
            fprintf(stderr, "3ds-replay: %s:%d: frames must not decrease\n",
                    path, lineno);
            goto refuse;
        }
        if (sLen == REPLAY_MAX) {
            fprintf(stderr, "3ds-replay: %s: more than %d events\n",
                    path, REPLAY_MAX);
            goto refuse;
        }
        sScript[sLen++] = ev;
    }
    fclose(f);
    fprintf(stderr, "3ds-replay: %s: %d event(s)\n", path, sLen);
    return sLen;

refuse:
    /* Nothing loaded rather than a prefix. Half a session is a run that looks
     * like it went somewhere and did not, which is the failure this whole
     * file exists to make impossible. */
    fclose(f);
    replay_reset();
    return 0;
}

int replay_active(void)
{
    return sLen > 0;
}

int replay_events(void)
{
    return sLen;
}

int replay_position(void)
{
    return sPos;
}

int replay_frame(uint64_t frame, ReplayState *out)
{
    if (sLen == 0) {
        return 0;
    }

    /* Level-held: every line due at or before this frame is applied, and the
     * last one wins. The game derives held-vs-pressed from consecutive frame
     * levels, so a script that only marked edges would have to keep its own
     * pairs matched across a skipped frame. */
    while (sPos < sLen && sScript[sPos].frame <= frame) {
        const ReplayEvent *ev = &sScript[sPos++];

        switch (ev->kind) {
        case REPLAY_KEYS:
            sHeld.keys = ev->keys;
            break;
        case REPLAY_TOUCH:
            sHeld.touching = 1;
            sHeld.x = ev->x;
            sHeld.y = ev->y;
            break;
        case REPLAY_RELEASE:
        default:
            sHeld.touching = 0;
            sHeld.x = 0;
            sHeld.y = 0;
            break;
        }
    }

    if (out != NULL) {
        *out = sHeld;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#ifdef REPLAY_SELFTEST_VERBOSE
#define FAILNOTE() fprintf(stderr, "  3ds_replay.c:%d failed\n", __LINE__)
#else
#define FAILNOTE() ((void)0)
#endif

#define CHECK(cond)                                                           \
    do {                                                                      \
        ran++;                                                                \
        if (!(cond)) {                                                        \
            failed++;                                                         \
            FAILNOTE();                                                       \
        }                                                                     \
    } while (0)

int replay_selftest(int *ranOut)
{
    ReplayEvent ev;
    ReplayState st;
    int ran = 0;
    int failed = 0;

    /* The names, against the register bits. A is bit 0 and X and Y are the
     * ARM7's word, which is 3ds_input.c's business and not this file's;
     * here they are just two more bits of one mask. */
    CHECK(replay_button_mask("A") == 0x0001);
    CHECK(replay_button_mask("START") == 0x0008);
    CHECK(replay_button_mask("X") == 0x0400);
    CHECK(replay_button_mask("Y") == 0x0800);
    CHECK(replay_button_mask("none") == 0);
    CHECK(replay_button_mask("a") == 0);
    CHECK(replay_button_mask("") == 0);

    /* Lines that are not events. */
    CHECK(replay_parse_line("\n", &ev) == 0);
    CHECK(replay_parse_line("   \t \n", &ev) == 0);
    CHECK(replay_parse_line("# recorded by ...\n", &ev) == 0);
    CHECK(replay_parse_line("   # indented\n", &ev) == 0);

    /* Lines that are. */
    CHECK(replay_parse_line("392 keys START\n", &ev) == 1
          && ev.frame == 392 && ev.kind == REPLAY_KEYS && ev.keys == 0x0008);
    CHECK(replay_parse_line("0 keys none\n", &ev) == 1
          && ev.frame == 0 && ev.kind == REPLAY_KEYS && ev.keys == 0);
    CHECK(replay_parse_line("709 keys A B\n", &ev) == 1
          && ev.keys == 0x0003);
    CHECK(replay_parse_line("520 touch 128 96\n", &ev) == 1
          && ev.kind == REPLAY_TOUCH && ev.x == 128 && ev.y == 96);
    CHECK(replay_parse_line("540 release\n", &ev) == 1
          && ev.kind == REPLAY_RELEASE);
    /* Trailing comment on a real line: the button loop stops at a word it
     * does not know, and '#' is not a button. This is a refusal on purpose;
     * a recorded script never writes one, and accepting it would mean
     * accepting a typo'd button name too. */
    CHECK(replay_parse_line("1 keys A # hi\n", &ev) == -1);

    /* Lines that are wrong. */
    CHECK(replay_parse_line("keys A\n", &ev) == -1);
    CHECK(replay_parse_line("12 keys NOPE\n", &ev) == -1);
    CHECK(replay_parse_line("12 wobble\n", &ev) == -1);
    CHECK(replay_parse_line("12 touch 4\n", &ev) == -1);

    /* The frame walk. Nothing loaded is the ordinary console, and it must not
     * touch the caller's state. */
    replay_reset();
    st.keys = 0xAAAA;
    CHECK(replay_frame(0, &st) == 0);
    CHECK(st.keys == 0xAAAA);
    CHECK(replay_active() == 0);

    /* A tiny script, planted rather than read, so this runs with no file
     * system: hold START at 10, release at 20, pen down at 30, up at 40. */
    replay_reset();
    sLen = 0;
    sScript[sLen].frame = 10; sScript[sLen].kind = REPLAY_KEYS;
    sScript[sLen].keys = 0x0008; sLen++;
    sScript[sLen].frame = 20; sScript[sLen].kind = REPLAY_KEYS;
    sScript[sLen].keys = 0; sLen++;
    sScript[sLen].frame = 30; sScript[sLen].kind = REPLAY_TOUCH;
    sScript[sLen].x = 100; sScript[sLen].y = 50; sLen++;
    sScript[sLen].frame = 40; sScript[sLen].kind = REPLAY_RELEASE; sLen++;

    CHECK(replay_active() == 1);
    CHECK(replay_events() == 4);

    memset(&st, 0, sizeof st);
    CHECK(replay_frame(0, &st) == 1 && st.keys == 0 && st.touching == 0);
    CHECK(replay_frame(9, &st) == 1 && st.keys == 0);
    /* Level-held: the state persists over every frame between two lines. */
    CHECK(replay_frame(10, &st) == 1 && st.keys == 0x0008);
    CHECK(replay_frame(15, &st) == 1 && st.keys == 0x0008);
    CHECK(replay_frame(19, &st) == 1 && st.keys == 0x0008);
    CHECK(replay_frame(20, &st) == 1 && st.keys == 0);
    CHECK(replay_frame(30, &st) == 1
          && st.touching == 1 && st.x == 100 && st.y == 50);
    CHECK(replay_frame(39, &st) == 1 && st.touching == 1);
    CHECK(replay_frame(40, &st) == 1 && st.touching == 0);
    CHECK(replay_position() == 4);

    /*
     * A frame the walk jumped over applies everything it passed, last line
     * winning; which is what a console dropping a frame must do, and the
     * reason the state is a level and not a queue of presses.
     */
    replay_reset();
    sLen = 0;
    sScript[sLen].frame = 10; sScript[sLen].kind = REPLAY_KEYS;
    sScript[sLen].keys = 0x0008; sLen++;
    sScript[sLen].frame = 20; sScript[sLen].kind = REPLAY_KEYS;
    sScript[sLen].keys = 0x0001; sLen++;
    memset(&st, 0, sizeof st);
    CHECK(replay_frame(1000, &st) == 1 && st.keys == 0x0001);
    CHECK(replay_position() == 2);
    /* And past the end it holds the last level rather than idling. */
    CHECK(replay_frame(2000, &st) == 1 && st.keys == 0x0001);

    replay_reset();
    CHECK(replay_events() == 0);

    if (ranOut != NULL) {
        *ranOut = ran;
    }
    return failed;
}
