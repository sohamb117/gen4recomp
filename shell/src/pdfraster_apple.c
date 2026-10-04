/* np_pdf_render with CoreGraphics (see pdfraster.h). */
#include "pdfraster.h"

#include <CoreGraphics/CoreGraphics.h>

SDL_Surface *np_pdf_render(const void *data, size_t len, int w, int h)
{
    if (w <= 0 || h <= 0)
        return NULL;
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, data, len, NULL);
    CGPDFDocumentRef doc = provider ? CGPDFDocumentCreateWithProvider(provider) : NULL;
    CGPDFPageRef page = doc ? CGPDFDocumentGetPage(doc, 1) : NULL;
    SDL_Surface *surface = page ? SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888) : NULL;
    if (surface) {
        SDL_memset(surface->pixels, 0, (size_t)surface->pitch * (size_t)h);
        CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
        /* Premultiplied BGRA in memory, which is SDL's ARGB8888 on a
         * little-endian machine. */
        CGContextRef ctx = CGBitmapContextCreate(surface->pixels, (size_t)w, (size_t)h, 8, (size_t)surface->pitch, rgb,
                                                 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(rgb);
        if (ctx) {
            CGRect box = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
            CGContextScaleCTM(ctx, (CGFloat)w / box.size.width, (CGFloat)h / box.size.height);
            CGContextTranslateCTM(ctx, -box.origin.x, -box.origin.y);
            CGContextDrawPDFPage(ctx, page);
            CGContextRelease(ctx);
            /* The context drew premultiplied alpha; SDL blends straight. */
            SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        } else {
            SDL_DestroySurface(surface);
            surface = NULL;
        }
    }
    if (doc)
        CGPDFDocumentRelease(doc);
    if (provider)
        CGDataProviderRelease(provider);
    return surface;
}
