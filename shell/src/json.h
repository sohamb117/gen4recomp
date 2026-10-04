/*
 * A small JSON reader for the documents the shell consumes (GitHub release
 * metadata, Delta skin info.json): the text is split into a flat token
 * array in one pass, values are looked up by key or index, and strings are
 * unescaped only when asked for. No allocation beyond the caller's token
 * array; nesting is capped so hostile input cannot exhaust the stack (the
 * parser is iterative anyway). SDL-free and unit-tested.
 */
#ifndef NP_JSON_H
#define NP_JSON_H

#include <stddef.h>

typedef enum np_json_type { NP_JSON_OBJECT, NP_JSON_ARRAY, NP_JSON_STRING, NP_JSON_PRIMITIVE } np_json_type;

typedef struct np_json_tok {
    np_json_type type;
    int start, end; /* byte range; strings exclude the quotes */
    int size;       /* objects: members, arrays: elements */
    int next;       /* index of the token after this value's subtree */
} np_json_tok;

#define NP_JSON_MAX_DEPTH 64

/* Tokenizes one JSON value (whitespace around it allowed). Returns the token
 * count, or -1 for malformed input, too deep nesting or more than `max`
 * tokens. Token 0 is the root. */
int np_json_parse(const char *s, size_t len, np_json_tok *t, int max);

/* The value of member `key` in object `obj`, or -1. */
int np_json_get(const char *s, const np_json_tok *t, int obj, const char *key);
/* Element `i` of array `arr`, or -1. */
int np_json_at(const np_json_tok *t, int arr, int i);

/* Unescapes string token `i` into out (UTF-8, truncated to cap - 1 bytes).
 * Returns 0, or -1 if `i` is not a string or has a bad escape. */
int np_json_string(const char *s, const np_json_tok *t, int i, char *out, size_t cap);
/* Whether string token `i` equals `str` (compared raw: no escapes). */
int np_json_eq(const char *s, const np_json_tok *t, int i, const char *str);
/* Number token `i` (`fallback` for anything else). */
double np_json_number(const char *s, const np_json_tok *t, int i, double fallback);
/* true / false token `i` (`fallback` for anything else). */
int np_json_bool(const char *s, const np_json_tok *t, int i, int fallback);

#endif
