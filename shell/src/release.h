/*
 * What the updater needs from a GitHub release, kept SDL-free so it is
 * unit-tested: the fields of the REST API's "latest release" JSON, version
 * ordering, which asset is this platform's build, and the digest a
 * `sha256sums.txt` asset lists for it (the format `sha256sum` writes).
 */
#ifndef NP_RELEASE_H
#define NP_RELEASE_H

#include <stddef.h>
#include <stdint.h>

#define NP_RELEASE_MAX_ASSETS 32

typedef struct np_release_asset {
    char name[128];
    char url[512]; /* browser_download_url */
    uint64_t size;
} np_release_asset;

typedef struct np_release {
    char tag[64];       /* tag_name, e.g. "v0.2.0" */
    char name[128];     /* release title */
    char html_url[512]; /* the release page */
    int draft, prerelease;
    int nassets;
    np_release_asset asset[NP_RELEASE_MAX_ASSETS];
} np_release;

/* Parses a release object. Unknown fields are skipped; strings are
 * unescaped (\uXXXX to UTF-8). Returns 0, or -1 for malformed JSON or a
 * release without tag_name. */
int np_release_parse(const char *json, size_t len, np_release *r);

/* Compares dotted numeric versions, ignoring a leading 'v' and anything
 * from the first '-' or '+' on except that a pre-release ("1.0.0-rc1")
 * orders before its release. Returns <0, 0 or >0 like strcmp. */
int np_version_compare(const char *a, const char *b);

/* The asset for `platform` ("macos", "windows"): a .zip whose name contains
 * the platform word (case-insensitive). Returns its index or -1. */
int np_release_pick_asset(const np_release *r, const char *platform);
/* Index of the asset named exactly `name`, or -1. */
int np_release_find_asset(const np_release *r, const char *name);

/* Finds `file` in sha256sum output ("<64 hex>  <name>" or "<hex> *<name>"
 * per line) and writes its lower-case digest. Returns 0 or -1. */
int np_sha256sums_lookup(const char *text, size_t len, const char *file, char hex[65]);

#endif
