/*
 * Read-only ZIP archives held in memory, for mod packages and Delta skins
 * (.deltaskin is a zip). Stored and deflated members are supported, which
 * is what every common zip tool writes; there is no zlib on iOS or in the
 * Windows SDK, so inflate is implemented here (RFC 1951).
 *
 * SDL-free so the parser and inflate are unit-tested. Nothing is trusted:
 * offsets and sizes are bounds-checked against the buffer and a member's
 * declared size, and names are reported as stored, for the caller to
 * validate with np_zip_name_safe().
 */
#ifndef NP_ZIP_H
#define NP_ZIP_H

#include <stddef.h>
#include <stdint.h>

typedef struct np_zip {
    const uint8_t *data;
    size_t len;
    size_t cdir;     /* offset of the central directory */
    uint32_t count;  /* members */
} np_zip;

typedef struct np_zip_entry {
    char name[512];  /* as stored, '/'-separated; truncated names are refused */
    uint32_t size;   /* uncompressed */
    uint32_t csize;  /* compressed */
    uint16_t method; /* 0 stored, 8 deflate */
    uint32_t crc;
    int is_dir;
    int is_symlink; /* Unix mode in the external attributes says link */
    size_t local;   /* local header offset */
} np_zip_entry;

/* Finds the central directory. Returns 0, or -1 if `data` is not a zip
 * this reader handles (multi-disk, ZIP64 or damaged). */
int np_zip_open(np_zip *z, const void *data, size_t len);
/* Member `i` (0..count-1). Returns 0 or -1. */
int np_zip_entry_at(const np_zip *z, uint32_t i, np_zip_entry *e);
/* Index of the member named `name` (exact match), or -1. */
int np_zip_find(const np_zip *z, const char *name, np_zip_entry *e);
/* Decompresses `e` into out (e->size bytes) and checks its CRC-32.
 * Returns 0 or -1. */
int np_zip_extract(const np_zip *z, const np_zip_entry *e, void *out);

/* Raw deflate stream -> out (exactly out_len bytes expected). Returns 0. */
int np_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);

/* Whether a member name is a plain relative path that stays inside the
 * extraction directory: no absolute path or drive, no "." / ".." part, no
 * backslash, no empty part except a trailing '/' for directories. */
int np_zip_name_safe(const char *name);

#endif
