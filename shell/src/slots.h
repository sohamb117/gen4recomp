/*
 * Save slots: each game keeps any number of named backup-chip images,
 * saves/<game>/<name>.sav. A slot name is also a file name on macOS, iOS and
 * Windows, so the allowed alphabet is deliberately small, and names compare
 * case-insensitively (APFS and NTFS are case-insensitive by default).
 *
 * The files are raw 512 KiB flash images, the format DeSmuME ("raw .sav"
 * export) and melonDS read and write; DeSmuME's native .dsv adds a
 * 122-byte footer, which import strips.
 *
 * SDL-free so the rules are unit-tested.
 */
#ifndef NP_SLOTS_H
#define NP_SLOTS_H

#include <stddef.h>
#include <stdint.h>

#define NP_SAVE_BYTES 0x80000u /* 4 Mbit flash in D/P/Pt cartridges */
#define NP_SLOT_NAME_MAX 32    /* characters, excluding the terminator */
#define NP_DESMUME_FOOTER_BYTES 122u

/* NULL if `name` is a valid slot name, else a short reason for the player. */
const char *np_slot_name_problem(const char *name);

/* Whether a character may appear in a slot name. */
int np_slot_char_ok(unsigned char c);

/* Case-insensitive ASCII comparison, as the file systems compare names. */
int np_slot_name_eq(const char *a, const char *b);

/* A valid name derived from arbitrary text (e.g. an imported file's base
 * name, extension already removed): bad characters become '_', runs of
 * spaces collapse, length is capped. Falls back to `fallback`. */
void np_slot_sanitize(const char *in, const char *fallback, char out[NP_SLOT_NAME_MAX + 1]);

/* `base` if no name in `taken` equals it, else "base (2)", "base (3)", ...
 * (base shortened so the result stays within NP_SLOT_NAME_MAX). `base` must
 * be valid. Returns 0, or -1 if 999 candidates are all taken. */
int np_slot_unique(const char *base, const char *const *taken, int ntaken, char out[NP_SLOT_NAME_MAX + 1]);

/* "Slot N" with the smallest N >= 1 that is free. */
void np_slot_default_name(const char *const *taken, int ntaken, char out[NP_SLOT_NAME_MAX + 1]);

/*
 * Validates an imported save file. Accepts a raw image of exactly
 * NP_SAVE_BYTES, or a DeSmuME .dsv (raw image + footer). On success returns
 * 0 and sets *raw_len to NP_SAVE_BYTES (the bytes to keep, from offset 0);
 * otherwise returns -1 and *why explains.
 */
int np_sav_normalize(const uint8_t *data, size_t size, size_t *raw_len, const char **why);

#endif
