/*
 * Rasterizes page 1 of a PDF, for Delta skins whose art is a "resizable"
 * PDF. Apple platforms draw it with CoreGraphics (pdfraster_apple.c), a C
 * API on both macOS and iOS; other platforms have no PDF renderer and only
 * take skins with PNG art (NP_HAVE_PDF undefined).
 */
#ifndef NP_PDFRASTER_H
#define NP_PDFRASTER_H

#include <SDL3/SDL.h>

/* A w x h ARGB surface of the page scaled to fill it, or NULL. */
SDL_Surface *np_pdf_render(const void *data, size_t len, int w, int h);

#endif
