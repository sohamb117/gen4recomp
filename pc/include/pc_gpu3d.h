/*
 * The geometry engine.
 *
 * The DS's 3D pipeline is two units. This is the front one: it takes a stream
 * of geometry commands, keeps four matrix stacks, transforms and lights
 * vertices, assembles them into polygons, clips those against the view volume
 * and leaves the survivors in polygon and vertex RAM. The back one, the
 * rasterizer, is not here. pc_gpu3d_render_polygons() is the seam between
 * them, and it is the same seam the hardware has: a double-buffered polygon
 * list that SWAP_BUFFERS flips.
 *
 * Where commands come from is the part that is not melonDS's problem. On
 * hardware every write to 0x04000400 through 0x040005CB pushes a command or a
 * parameter. Identity mapping makes those addresses plain memory here, so a
 * store to one lands in the mapped I/O page and the next store overwrites it,
 * which is exactly what a FIFO must not do. Three producers therefore route
 * into pc_gpu3d_write32() instead:
 *
 *   - recompiled assembly, through ARM_ST32's ARMREC_GX_HOOK form. Total and
 *     exact, because the store carries both the address and the value.
 *   - decompiled C, through registers.h's redefinition of the reg_G3_* macros.
 *     A macro cannot run code after an assignment through it, so the pointer
 *     these hand back is a staging word and the command is committed by the
 *     next access.
 *   - DMA with 0x04000400 as its destination, which is how the SDK sends a
 *     display list. pc/src/pc_dma.c routes it.
 *
 * The staging word costs one thing: a caller that takes the port's address and
 * stores through it repeatedly stages one word and loses the rest. Six SDK
 * functions do exactly that, and the case cannot be hooked in C at all. So it
 * is made loud rather than silent, and those six are supplied by
 * pc/src/pc_gx_g3_util.c instead.
 *
 * Synchronous, which is also what the DMA model does. A command executes when
 * its last parameter is written, in full, before the store returns. There is
 * no cycle model, so the FIFO is never full and the engine is never busy:
 * GXSTAT reads back "empty, idle" and the SDK's spins exit at once. What that
 * gives up is the FIFO interrupt and the "less than half full" DMA trigger,
 * both modelled as always ready.
 *
 * Modelled: MTX_MODE and all four stacks, the clip matrix, BEGIN and END_VTXS,
 * all six vertex forms, strips and quads with their vertex reuse, normals,
 * texcoords and the three transform modes, the four lights and the whole
 * lighting sum, culling, clipping against all six planes, the viewport
 * transform, POS_TEST, VEC_TEST and BOX_TEST, and SWAP_BUFFERS with the
 * translucent split and the Y-sort. Not modelled: anything the rasterizer
 * owns, which stays plain memory.
 *
 * Derived from melonDS. See pc/hw/pc_gpu3d.c's header and pc/hw/README for why
 * it is C here rather than their C++ linked in.
 */

#ifndef POKEDIAMOND_PC_GPU3D_H
#define POKEDIAMOND_PC_GPU3D_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The command window. 0x04000400-0x0400043F is the packed FIFO port and
 * 0x04000440-0x040005CB is one address per command; the status and result
 * registers run to 0x040006A3. One window covers all of it because the hook
 * that has to see a store is the same hook either way. */
#define PC_GX_IO_BASE   0x04000400u
#define PC_GX_IO_END    0x040006A4u

#define PC_GX_FIFO_BASE 0x04000400u
#define PC_GX_FIFO_END  0x04000440u
#define PC_GX_CMD_BASE  0x04000440u
#define PC_GX_CMD_END   0x040005CCu

/* Vertex and polygon RAM, as the hardware sizes them. */
#define PC_GX_MAX_VERTICES 6144
#define PC_GX_MAX_POLYGONS 2048

/*
 * A vertex as the geometry engine leaves it. `Position` is the clip-space
 * coordinate the clipper worked in; `FinalPosition` is the screen pixel the
 * viewport transform produced; `FinalColor` is the 6-bit-per-channel colour
 * The rasterizer interpolates. Field names follow melonDS's so that the two can be
 * compared field by field without a translation table in the middle.
 */
typedef struct PcGxVertex {
    int32_t  Position[4];
    int32_t  Color[3];
    int16_t  TexCoords[2];
    uint8_t  Clipped;
    int32_t  FinalPosition[2];
    int32_t  FinalColor[3];
} PcGxVertex;

typedef struct PcGxPolygon {
    PcGxVertex *Vertices[10];
    uint32_t NumVertices;

    int32_t  FinalZ[10];
    int32_t  FinalW[10];
    uint8_t  WBuffer;

    uint32_t Attr;
    uint32_t TexParam;
    uint32_t TexPalette;

    uint8_t  Degenerate;
    uint8_t  FacingView;
    uint8_t  Translucent;
    uint8_t  IsShadowMask;
    uint8_t  IsShadow;

    int      Type;                /* 0 = polygon, 1 = line */

    uint32_t VTop, VBottom;
    int32_t  YTop, YBottom;
    int32_t  XTop, XBottom;
    uint32_t SortKey;
} PcGxPolygon;

/*
 * The rendering registers, as they stood when the last frame started.
 *
 * These live outside the command window, so identity mapping leaves them as
 * plain guest memory and no store to one is hooked, see.
 * pc_gpu3d_vblank() reads them out of guest memory and applies the masks the
 * hardware applies at write time, at the same point upstream latches its own
 * `Render*` shadows; the rasterizer reads only this copy, so a store made
 * halfway through a frame takes effect at the next frame like hardware's.
 *
 * `FogOffset` is already multiplied by 0x200 and `FogDensityTable` is already
 * the 34-entry form the interpolation indexes, both as upstream leaves them.
 */
typedef struct PcGxRenderRegs {
    uint32_t DispCnt;
    uint32_t AlphaRef;
    uint16_t ToonTable[32];
    uint16_t EdgeTable[8];
    uint32_t FogColor;
    uint32_t FogOffset;
    uint32_t FogShift;
    uint8_t  FogDensityTable[34];
    uint32_t ClearAttr1;
    uint32_t ClearAttr2;
} PcGxRenderRegs;

const PcGxRenderRegs *pc_gpu3d_render_regs(void);

/* Put the engine in its power-on state. Called from pc_gpu3d_install(). */
void pc_gpu3d_reset(void);

/*
 * Wide rendering: widen the horizontal field of view so `width` columns
 * carry real world instead of 256, leaving the centre 256 pixel-identical
 * to a native render. 0 or 256 returns to native. Set once at startup,
 * before guest code runs; the rasterizer picks the width up through
 * pc_gpu3d_soft_set_width(), and only the viewer channel ever shows the
 * margins, every instrument (frame dumps, the differential trace) reads
 * the native centre.
 */
void pc_gpu3d_set_wide(int width);
int  pc_gpu3d_wide_width(void);

/* True high-resolution 3D: rasterize at `scale` times the pixels on both
 * axes, field of view unchanged, nothing game-visible changed. The
 * rasterizer picks it up through pc_gpu3d_soft_set_scale(); anything outside
 * 1..PC_GPU3D_HD_MAX is refused back to native rather than clamped, because
 * a scale the rasterizer will not honour is a picture the two halves
 * disagree about. The same ceiling is spelled PC_VIEW_HD_MAX on the channel
 * side, where it also sizes the published page. */
/*
 * 4. Two things had to be sized from the surface rather than from the DS
 * before this could move off 2: the coordinate fields (hd_masks(), in
 * pc_gpu3d.c) and the polygon sort key (submit_polygon(), same file). The
 * second is the one that bit, eight bits of Y with the translucent flag
 * above them, which at 4x let a Y overrun the flag and sorted translucent
 * polygons in among the opaque ones, so the ground painted over every
 * character's drop shadow.
 *
 * What holds it at 4 now is fill rate: the rasterizer does N^2 the pixel
 * work, and no machine this has run on holds 60 fps past it.
 */
#define PC_GPU3D_HD_MAX 4
void pc_gpu3d_set_hd(int scale);
int  pc_gpu3d_hd_scale(void);

/*
 * Install the engine: reset it and arm the hook the recompiled and decompiled
 * halves reach it through. Called from pc_main.c after armrec_mem_init(),
 * because the model reads POWCNT1 out of guest memory and doing that before
 * the mapping exists is a fault rather than a zero.
 */
void pc_gpu3d_install(void);

/* Whether pc_gpu3d_install() has run. The hook is a no-op before it does, so
 * a test that drives registers without the engine gets plain memory. */
int pc_gpu3d_installed(void);

/* A store into the command window. `size` is 1, 2 or 4 bytes. */
void pc_gpu3d_write(uint32_t addr, uint32_t val, int size);

/* A load from the status or result window. Returns the value; `*handled` is
 * set to 0 for an address the engine does not own, which reads as memory. */
uint32_t pc_gpu3d_read(uint32_t addr, int size, int *handled);

/*
 * Commit the words staged by armrec_gx_port(). Every other entry point calls
 * this first; it is exposed because pc_video.c calls it at the frame boundary,
 * where there may be no further access to trigger it.
 */
void pc_gpu3d_flush(void);

/* The two halves of the decompiled-C path, reached through armrec_gx_port()
 * and armrec_gx_reg(); armrec_rt.c has weak no-ops so a build without this
 * file still links. */
uint32_t *pc_gpu3d_stage(uint32_t addr);
void pc_gpu3d_refresh_regs(void);

/* A store through a pointer armrec_gx_port() handed out, for pc/src/pc_gx.c's
 * transcriptions of the SDK's assembly FIFO senders. 0 if `dst` is neither the
 * staging buffer nor the command window. */
int pc_gpu3d_store_through(void *dst, uint32_t v);

/*
 * ...and the store that goes with it, spelled once so that pc/src/pc_gx.c and
 * pc/src/pc_gx_g3_util.c cannot disagree about it. A destination that is
 * neither the staging buffer nor the command window is an ordinary store,
 * which is what pc/tests/test_cp_gx.c drives these with: ordinary buffers, so
 * the fixed-destination behaviour is observed rather than absorbed.
 */
static inline void pc_gx_store(volatile uint32_t *dst, uint32_t v) {
    if (!pc_gpu3d_store_through((void *)dst, v)) *dst = v;
}

/*
 * The frame boundary. SWAP_BUFFERS does not take effect when it is executed,
 * on hardware it waits for VBlank, so this is where the polygon list becomes
 * the one the renderer sees and the geometry side starts a fresh bank.
 */
void pc_gpu3d_vblank(void);

/*
 * What the rasterizer has cost this run, in nanoseconds, timed whether or not
 * --bench asked. The pacer reads it: dropping a picture saves exactly this
 * plus the publish, and a frame that went over for some other reason is not
 * one a dropped picture can rescue. See pc_view.c.
 */
unsigned long long pc_gpu3d_render_ns(void);

/* The list the rasterizer draws: the polygons SWAP_BUFFERS published, in render
 * order (opaque first, then translucent, Y-sorted per SWAP_BUFFERS' flags). */
PcGxPolygon **pc_gpu3d_render_polygons(uint32_t *count);

/* Live geometry-side counts, i.e. what RAM_COUNT reports. */
uint32_t pc_gpu3d_num_polygons(void);
uint32_t pc_gpu3d_num_vertices(void);

/*
 * Read-only views of the engine's own state, for tests. The matrices are 16
 * fixed-point words in column order, as the hardware holds them.
 */
const int32_t *pc_gpu3d_matrix(int which);   /* 0 proj, 1 pos, 2 vec, 3 tex */
const int32_t *pc_gpu3d_clip_matrix(void);
uint32_t pc_gpu3d_gxstat(void);

#ifdef __cplusplus
}
#endif

#endif /* POKEDIAMOND_PC_GPU3D_H */
