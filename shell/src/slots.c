/*
 * Slot name rules and save-file import validation (see slots.h).
 *
 * Allowed: ASCII letters, digits, space, - _ ( ) . ! ' and #. Not allowed:
 * a leading '.' (hidden files), trailing '.' or space (Windows strips
 * them), and the Windows device names (CON, NUL, COM1, ...), which cannot
 * be file names there even with an extension.
 */
#include "slots.h"

#include <stdio.h>
#include <string.h>

static const char desmume_snip[] = "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";
static const char desmume_cookie[] = "|-DESMUME SAVE-|";

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

int np_slot_name_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower((unsigned char)*a) != lower((unsigned char)*b))
            return 0;
    return *a == *b;
}

int np_slot_char_ok(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(" -_().!'#", c) != NULL;
}

static int is_device_name(const char *name)
{
    static const char *const fixed[] = {"con", "prn", "aux", "nul"};
    char stem[8];
    size_t n = 0;
    while (name[n] && name[n] != '.' && n < sizeof stem - 1) {
        stem[n] = (char)lower((unsigned char)name[n]);
        n++;
    }
    stem[n] = '\0';
    while (n && stem[n - 1] == ' ') /* "CON .x" is still CON */
        stem[--n] = '\0';
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++)
        if (!strcmp(stem, fixed[i]))
            return 1;
    return n == 4 && (!strncmp(stem, "com", 3) || !strncmp(stem, "lpt", 3)) && stem[3] >= '1' && stem[3] <= '9';
}

const char *np_slot_name_problem(const char *name)
{
    size_t n = strlen(name);
    if (n == 0)
        return "The name is empty.";
    if (n > NP_SLOT_NAME_MAX)
        return "The name is too long (32 characters at most).";
    for (size_t i = 0; i < n; i++)
        if (!np_slot_char_ok((unsigned char)name[i]))
            return "Use letters, digits, spaces and - _ ( ) . ! ' # only.";
    if (name[0] == '.' || name[0] == ' ')
        return "The name cannot start with a dot or a space.";
    if (name[n - 1] == '.' || name[n - 1] == ' ')
        return "The name cannot end with a dot or a space.";
    if (is_device_name(name))
        return "That name is reserved by Windows.";
    return NULL;
}

void np_slot_sanitize(const char *in, const char *fallback, char out[NP_SLOT_NAME_MAX + 1])
{
    size_t n = 0;
    for (; *in && n < NP_SLOT_NAME_MAX; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
        if (!np_slot_char_ok(c))
            c = '_';
        if (c == ' ' && (n == 0 || out[n - 1] == ' '))
            continue;
        if (c == '.' && n == 0)
            continue;
        out[n++] = (char)c;
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.'))
        n--;
    out[n] = '\0';
    if (np_slot_name_problem(out)) {
        strncpy(out, fallback, NP_SLOT_NAME_MAX);
        out[NP_SLOT_NAME_MAX] = '\0';
    }
}

static int taken_by(const char *name, const char *const *taken, int ntaken)
{
    for (int i = 0; i < ntaken; i++)
        if (np_slot_name_eq(name, taken[i]))
            return 1;
    return 0;
}

int np_slot_unique(const char *base, const char *const *taken, int ntaken, char out[NP_SLOT_NAME_MAX + 1])
{
    if (!taken_by(base, taken, ntaken)) {
        snprintf(out, NP_SLOT_NAME_MAX + 1, "%s", base);
        return 0;
    }
    for (int k = 2; k < 1000; k++) {
        char suffix[8];
        int sl = snprintf(suffix, sizeof suffix, " (%d)", k);
        size_t keep = strlen(base);
        if (keep + (size_t)sl > NP_SLOT_NAME_MAX)
            keep = NP_SLOT_NAME_MAX - (size_t)sl;
        while (keep && base[keep - 1] == ' ') /* no "name  (2)" after cutting */
            keep--;
        snprintf(out, NP_SLOT_NAME_MAX + 1, "%.*s%s", (int)keep, base, suffix);
        if (!taken_by(out, taken, ntaken))
            return 0;
    }
    return -1;
}

void np_slot_default_name(const char *const *taken, int ntaken, char out[NP_SLOT_NAME_MAX + 1])
{
    for (int k = 1;; k++) {
        snprintf(out, NP_SLOT_NAME_MAX + 1, "Slot %d", k);
        if (!taken_by(out, taken, ntaken))
            return;
    }
}

int np_sav_normalize(const uint8_t *data, size_t size, int gba, size_t *raw_len, const char **why)
{
    if (gba) {
        if (size == NP_GBA_SAVE_BYTES || size == NP_GBA_SAVE_BYTES + NP_MGBA_RTC_BYTES) {
            *raw_len = NP_GBA_SAVE_BYTES;
            return 0;
        }
        *why = "Not a Ruby/Sapphire/Emerald save: expected a 128 KiB (1 Mbit flash) .sav.";
        return -1;
    }
    if (size == NP_SAVE_BYTES) {
        *raw_len = NP_SAVE_BYTES;
        return 0;
    }
    size_t snip = sizeof desmume_snip - 1, cookie = sizeof desmume_cookie - 1;
    if (size > NP_SAVE_BYTES + snip + cookie && !memcmp(data + NP_SAVE_BYTES, desmume_snip, snip) &&
        !memcmp(data + size - cookie, desmume_cookie, cookie)) {
        *raw_len = NP_SAVE_BYTES;
        return 0;
    }
    if (size > NP_SAVE_BYTES && size >= cookie && !memcmp(data + size - cookie, desmume_cookie, cookie))
        *why = "This DeSmuME save is not for a 512 KiB (4 Mbit) cartridge.";
    else
        *why = "Not a Diamond/Pearl/Platinum save: expected a 512 KiB raw .sav or a DeSmuME .dsv.";
    return -1;
}
