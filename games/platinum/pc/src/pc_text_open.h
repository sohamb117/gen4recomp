/*
 * Opens a script the port reads by name (PC_INPUT, PC_LAB): a path, or the
 * text itself as "inline:line;line;..." (';' ends a line).
 *
 * The inline form exists for the wasm guest: the nativeplat runtime gives it
 * no preopened directories (core/runtime/np_wasi.c), so under np_headless
 * or the shell a path never opens, while an option string always arrives.
 * Header-only so every game's host list that compiles the reader gets it.
 * Windows hosts have no fmemopen and read scripts from files.
 */
#ifndef PC_TEXT_OPEN_H
#define PC_TEXT_OPEN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

static FILE *pc_text_open(const char *spec)
{
    return strncmp(spec, "inline:", 7) == 0 ? NULL : fopen(spec, "r");
}

#else

FILE *fmemopen(void *buf, size_t size, const char *mode);

static FILE *pc_text_open(const char *spec)
{
    char *text;
    size_t n, i;

    if (strncmp(spec, "inline:", 7) != 0) {
        return fopen(spec, "r");
    }
    spec += 7;
    n = strlen(spec);
    text = malloc(n + 2); /* lives as long as the FILE; read once at boot */
    if (text == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        text[i] = spec[i] == ';' ? '\n' : spec[i];
    }
    text[n] = '\n';
    text[n + 1] = '\0';
    return fmemopen(text, n + 1, "r");
}

#endif

#endif /* PC_TEXT_OPEN_H */
