/*
 * The polygon list, per frame, as text, so "the mon quad is two pixels left
 * of where it was" becomes a diff instead of a screenshot argument.
 *
 * `--dump-polys N[-M]` writes one line per polygon of every frame in that
 * range: the polygon's screen-space vertices, its ID, its alpha, its depth
 * range, its texture parameters and the flags the rasterizer sorted it by.
 * Everything here is read AFTER SWAP_BUFFERS has published the list and
 * after the viewport transform has run, so the numbers are the ones the
 * rasterizer is about to draw with rather than anything the game wrote.
 *
 * Why text and why sorted the way the hardware sorts. Two frames of a
 * sprite animation differ in a handful of numbers, and the whole value of
 * this instrument is that `diff` finds them. So the fields are fixed-width,
 * the order is the render list's own order (which is the order the hardware
 * draws in, translucent last), and nothing is printed that changes when it
 * should not, no pointers, no addresses, no timing.
 *
 * The first customer is the battle-sprite ghosting: a Pokemon is a textured
 * quad and its shadow is a second quad from the same texture page at a much
 * larger Z, so "did the two move together" is two lines of this dump.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_gpu3d.h"
#include "pc_gpu3d_soft.h"

static FILE *poly_out;
static uint64_t poly_from = 1, poly_to;      /* inclusive; to == 0 is "off" */

/*
 * PC_DUMP_POLYS=N or N-M, and the file goes beside nothing: it is written to
 * stdout when the value is a bare range, because a run that wants this wants
 * to read it. PC_DUMP_POLYS_FILE names a file instead.
 */
void pc_polydump_init(void)
{
    const char *spec = getenv("PC_DUMP_POLYS");
    const char *path = getenv("PC_DUMP_POLYS_FILE");
    char *end = NULL;
    unsigned long long a, b;

    if (spec == NULL || spec[0] == '\0') {
        return;
    }
    a = strtoull(spec, &end, 10);
    if (end == spec) {
        fprintf(stderr, "pc-polys: --dump-polys wants N or N-M; got %s\n",
                spec);
        exit(2);
    }
    b = a;
    if (end != NULL && *end == '-') {
        const char *p = end + 1;

        b = strtoull(p, &end, 10);
        if (end == p) {
            fprintf(stderr, "pc-polys: --dump-polys wants N or N-M; got %s\n",
                    spec);
            exit(2);
        }
    }
    if (end != NULL && *end != '\0') {
        fprintf(stderr, "pc-polys: --dump-polys wants N or N-M; got %s\n",
                spec);
        exit(2);
    }
    if (b < a) {
        fprintf(stderr, "pc-polys: --dump-polys %llu-%llu ends before it "
                        "starts\n", a, b);
        exit(2);
    }

    poly_from = (uint64_t)a;
    poly_to = (uint64_t)b;
    poly_out = stdout;
    if (path != NULL && path[0] != '\0') {
        poly_out = fopen(path, "w");
        if (poly_out == NULL) {
            perror(path);
            exit(1);
        }
    }
    fprintf(poly_out,
            "# polygon list, frames %llu..%llu\n"
            "# frame poly  id alpha  fmt  pal  attr     verts"
            "  z:min..max  w:min..max  flags  x,y ...\n",
            (unsigned long long)poly_from, (unsigned long long)poly_to);
    fflush(poly_out);
}

/*
 * Called from the render path with the list SWAP_BUFFERS published. `frame`
 * is the port's own frame counter, so a line here and a PNG from
 * --dump-frames name the same instant.
 */
void pc_polydump_frame(uint64_t frame)
{
    PcGxPolygon **list;
    uint32_t n, i;

    if (poly_out == NULL || frame < poly_from || frame > poly_to) {
        return;
    }
    list = pc_gpu3d_render_polygons(&n);
    fprintf(poly_out, "frame %llu polygons %u\n",
            (unsigned long long)frame, (unsigned)n);
    for (i = 0; i < n; i++) {
        const PcGxPolygon *p = list[i];
        int32_t zmin, zmax, wmin, wmax;
        uint32_t v;

        if (p == NULL || p->NumVertices == 0) {
            continue;
        }
        zmin = zmax = p->FinalZ[0];
        wmin = wmax = p->FinalW[0];
        for (v = 1; v < p->NumVertices && v < 10; v++) {
            if (p->FinalZ[v] < zmin) zmin = p->FinalZ[v];
            if (p->FinalZ[v] > zmax) zmax = p->FinalZ[v];
            if (p->FinalW[v] < wmin) wmin = p->FinalW[v];
            if (p->FinalW[v] > wmax) wmax = p->FinalW[v];
        }
        fprintf(poly_out,
                "  %5llu %4u  %2u %5u %4u %5u %08X  %2u"
                "  %9d..%-9d %7d..%-7d  %c%c%c%c%c ",
                (unsigned long long)frame, (unsigned)i,
                (unsigned)((p->Attr >> 24) & 0x3Fu),   /* polygon ID   */
                (unsigned)((p->Attr >> 16) & 0x1Fu),   /* alpha        */
                (unsigned)((p->TexParam >> 26) & 7u),  /* texture fmt  */
                (unsigned)p->TexPalette,
                (unsigned)p->TexParam,
                (unsigned)p->NumVertices,
                zmin, zmax, wmin, wmax,
                p->Type ? 'L' : 'P',
                p->Translucent ? 't' : '-',
                p->IsShadowMask ? 'm' : (p->IsShadow ? 's' : '-'),
                p->FacingView ? 'f' : 'b',
                p->WBuffer ? 'w' : 'z');
        for (v = 0; v < p->NumVertices && v < 10; v++) {
            fprintf(poly_out, " %d,%d",
                    (int)p->Vertices[v]->FinalPosition[0],
                    (int)p->Vertices[v]->FinalPosition[1]);
        }
        fputc('\n', poly_out);
    }
    fflush(poly_out);
}
