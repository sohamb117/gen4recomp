/*
 * The geometry engine.
 *
 * Derived from melonDS, which is GPLv3-or-later, so this file is too.
 * Upstream: src/GPU3D.cpp (the command decoder, the four matrix stacks, the
 * vertex and polygon pipelines, the clipper, the lighting sum, the three test
 * commands and the buffer swap) and src/GPU3D.h (the vertex and polygon
 * layouts). Copyright 2016-2026 melonDS team. See pc/hw/README for why the
 * algorithms are lifted into C here rather than linking melonDS's C++.
 *
 * Theirs: the arithmetic. Every shift, truncation and sign extension below is
 * upstream's, because the whole value of this file is that `pcdiff-melon
 * --geom-selftest` can drive their engine and get the same numbers. A cleaner
 * formulation that rounds differently is a defect that shows up as a wrong
 * pixel much later. Ours: the plumbing, the synchronous execution model, and
 * the storage.
 *
 * Synchronous execution. melonDS runs the FIFO against a cycle counter: a
 * command sits in CmdPIPE until the ARM9 has run far enough, and GXSTAT
 * reports the queue depth, the busy bit and the interrupt condition from that.
 * There is no cycle model here, because guest time is a counter that only
 * moves when the guest yields, so a command executes when its last parameter
 * arrives, in full, before the store returns. GXSTAT therefore always reads
 * back "FIFO empty, engine idle", which is what the SDK's spins need to
 * terminate.
 *
 * The 65 cycle-accounting calls upstream makes are deliberately absent rather
 * than forgotten: they change no architectural state, only when it becomes
 * visible. Grep for them in GPU3D.cpp when rebasing and check that is still
 * true of the new code.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "armrec_rt.h"
#include "pc_gpu3d.h"
#include "../src/pc_bench.h"
#include "pc_gpu3d_soft.h"

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* ------------------------------------------------------------------ */
/* Guest registers this engine reads or writes outside its own window  */
/* ------------------------------------------------------------------ */

#define REG_POWCNT1    0x04000304u   /* bit 3 gates the geometry engine */
#define REG_DISP3DCNT  0x04000060u   /* bit 13 is the RAM overflow flag */

/*
 * The rendering registers, the rasterizer's, latched below. They are outside the
 * command window, so identity mapping leaves them as plain guest memory and
 * nothing hooks a store to one; see.
 */
#define REG_EDGE_COLOR      0x04000330u   /* 8 halfwords */
#define REG_ALPHA_TEST_REF  0x04000340u
#define REG_CLEAR_COLOR     0x04000350u
#define REG_CLEAR_DEPTH     0x04000354u   /* +2 is CLRIMAGE_OFFSET */
#define REG_FOG_COLOR       0x04000358u
#define REG_FOG_OFFSET      0x0400035Cu
#define REG_FOG_TABLE       0x04000360u   /* 32 bytes */
#define REG_TOON_TABLE      0x04000380u   /* 32 halfwords */

/*
 * Guest addresses are not pointers on every host. On PC the map is the
 * identity and each of these is a cast the compiler folds away; on the 3DS
 * userland owns none of the DS's addresses, so the same number has to go
 * through the translator. pc/hw/pc_gpu2d.c's G2D_HOST() is the same macro for
 * the same reason and hostmap_ptr() is the same memoised lookup.
 *
 * This mattered more here than anywhere else it has been fixed: POWCNT1 gates
 * every geometry write, an unmapped read on the emulator answers zero rather
 * than faulting, and the engine therefore refused every command the game sent
 * it while looking exactly like a game that had not drawn anything yet.
 */
#if defined(__3DS__)
/*
 * The inline fast path. hostmap_ptr() is a memoised translation in another
 * object, and this macro sits in the innermost per-pixel accessors: a 2D frame
 * makes about 470,000 of these lookups and the whole frame is 46 million ARM11
 * cycles, so the call, the eight-way scan behind it and its counter are a
 * large part of the frame by themselves. On the desktop the macro is a cast,
 * which is why the cost only exists here.
 *
 * 3ds/src/3ds_hostmap.c publishes its most recent answer in the three
 * variables below. A scanline renderer stays inside one block for a long run,
 * so testing that one entry inline catches nearly every access and the real
 * cache stays behind it. hostmap_fast_tag is 0 whenever there is nothing safe
 * to use (an unmapped block, a flushed cache) so the test falls through to
 * the call rather than needing to encode those cases.
 *
 * HOSTMAP_FAST_SHIFT must equal HOSTMAP_BLK_SHIFT in 3ds/src/3ds_hostmap.c.
 * It cannot be included from here (this file is on the DS SDK's include chain,
 * not the port's), so 3ds/tests/run.sh greps both and fails if they differ,
 * the same arrangement the keypad constants already have.
 */
#define HOSTMAP_FAST_SHIFT 14
extern unsigned int hostmap_fast_tag;
extern unsigned char *hostmap_fast_base;
extern unsigned int hostmap_fast_limit;
void *hostmap_ptr(uint32_t guest);      /* 3ds/src/3ds_hostmap.h */

static inline void *hostmap_fast(uint32_t a)
{
    uint32_t tag = (a >> HOSTMAP_FAST_SHIFT) + 1u;
    uint32_t off = a & ((1u << HOSTMAP_FAST_SHIFT) - 1u);

    if (tag == hostmap_fast_tag && off < hostmap_fast_limit) {
        return hostmap_fast_base + off;
    }
    return hostmap_ptr(a);
}
#define G3D_HOST(a) hostmap_fast((uint32_t)(a))
#else
#define G3D_HOST(a) ((void *)(uintptr_t)(a))
#endif

static u16 io_read16(u32 addr)
{
    return *(volatile u16 *)G3D_HOST(addr);
}

static u32 io_read32(u32 addr)
{
    return *(volatile u32 *)G3D_HOST(addr);
}

static u8 io_read8(u32 addr)
{
    return *(volatile u8 *)G3D_HOST(addr);
}

static void io_write16(u32 addr, u16 v)
{
    *(volatile u16 *)G3D_HOST(addr) = v;
}

/* ------------------------------------------------------------------ */
/* Engine state                                                        */
/* ------------------------------------------------------------------ */

/* Parameter counts per command, upstream's CmdNumParams. Index is the opcode;
 * 0 means "one word, no parameters" for a real command and "not a command"
 * for everything else, WriteToGXFIFO tells the two apart the same way
 * upstream does, by the opcode being nonzero. */
static const u8 gx_num_params[256] = {
    /* 0x00 */ 0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x10 */ 1, 0, 1, 1, 1, 0, 16, 12, 16, 12, 9, 3, 3, 0, 0, 0,
    /* 0x20 */ 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0,
    /* 0x30 */ 1, 1, 1, 1, 32, 0,0,0,0,0,0,0,0,0,0,0,
    /* 0x40 */ 1, 0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x50 */ 1, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x60 */ 1, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x70 */ 3, 2, 1, 0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x80+ */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

typedef struct {
    int installed;

    /* --- the packed-FIFO decoder, upstream's WriteToGXFIFO state --- */
    u32 num_commands;
    u32 cur_command;
    u32 param_count;
    u32 total_params;

    /* --- the parameter accumulator for a multi-word command --- */
    u32 exec_cmd;              /* opcode being accumulated, 0 = none */
    u32 exec_params[32];
    u32 exec_count;

    u32 gxstat;                /* bits 1, 15, 30-31; the rest are derived */
    u32 zero_dot_w_limit;

    u32 matrix_mode;
    s32 proj[16], pos[16], vec[16], tex[16];
    s32 clip[16];
    int clip_dirty;

    u32 viewport[6];

    s32 proj_stack[16];
    s32 pos_stack[32][16];
    s32 vec_stack[32][16];
    s32 tex_stack[16];
    s32 proj_sp, pos_sp, tex_sp;

    u32 polygon_mode;
    s16 cur_vertex[3];
    u8  vertex_color[3];
    s16 texcoords[2];
    s16 raw_texcoords[2];
    s16 normal[3];

    s16 light_dir[4][3];
    s32 spec_recip[4];
    u8  light_color[4][3];
    u8  mat_diffuse[3];
    u8  mat_ambient[3];
    u8  mat_specular[3];
    u8  mat_emission[3];

    int use_shininess;
    u8  shininess[128];

    u32 polygon_attr;
    u32 cur_polygon_attr;
    u32 tex_param;
    u32 tex_palette;

    s32 pos_test_result[4];
    s16 vec_test_result[3];

    PcGxVertex temp_vtx[4];
    u32 vertex_num;
    u32 vertex_num_in_poly;
    u32 num_consecutive_polygons;
    PcGxPolygon *last_strip_polygon;
    u32 num_opaque_polygons;

    PcGxVertex  vertex_ram[PC_GX_MAX_VERTICES * 2];
    PcGxPolygon polygon_ram[PC_GX_MAX_POLYGONS * 2];

    PcGxVertex  *cur_vertex_ram;
    PcGxPolygon *cur_polygon_ram;
    u32 num_vertices;
    u32 num_polygons;
    u32 cur_ram_bank;

    PcGxPolygon *render_polygon_ram[PC_GX_MAX_POLYGONS];
    u32 render_num_polygons;

    /* --- the rendering registers, latched at the frame boundary --- */
    PcGxRenderRegs render_regs;

    u32 flush_request;
    u32 flush_attributes;

    /* --- the staging word pc/include/registers.h writes through --- */
    u32 stage_addr;            /* 0 = nothing staged */
    u32 stage_val;
} PcGx;

static PcGx gx;

/* ------------------------------------------------------------------ */
/* Traps                                                               */
/* ------------------------------------------------------------------ */

static void gx_fatal(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

#include <stdarg.h>
static void gx_fatal(const char *fmt, ...)
{
    va_list ap;
#if defined(__3DS__)
    /* No stderr on the console, and abort() there is a black screen. The
     * message goes over the last frame instead, until Start. */
    char what[192];

    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    armrec_trap("pc_gpu3d", what);
#else
    fputs("pc-gpu3d: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
#endif
}

/* ------------------------------------------------------------------ */
/* Matrix arithmetic, upstream's, shift for shift                    */
/* ------------------------------------------------------------------ */

static void mtx_identity(s32 *m)
{
    m[0] = 0x1000; m[1] = 0;      m[2] = 0;       m[3] = 0;
    m[4] = 0;      m[5] = 0x1000; m[6] = 0;       m[7] = 0;
    m[8] = 0;      m[9] = 0;      m[10] = 0x1000; m[11] = 0;
    m[12] = 0;     m[13] = 0;     m[14] = 0;      m[15] = 0x1000;
}

static void mtx_load_4x4(s32 *m, const s32 *s)
{
    memcpy(m, s, 16 * 4);
}

static void mtx_load_4x3(s32 *m, const s32 *s)
{
    m[0] = s[0];  m[1] = s[1];   m[2] = s[2];   m[3] = 0;
    m[4] = s[3];  m[5] = s[4];   m[6] = s[5];   m[7] = 0;
    m[8] = s[6];  m[9] = s[7];   m[10] = s[8];  m[11] = 0;
    m[12] = s[9]; m[13] = s[10]; m[14] = s[11]; m[15] = 0x1000;
}

static void mtx_mult_4x4(s32 *m, const s32 *s)
{
    s32 tmp[16];
    int i, j;
    memcpy(tmp, m, 16 * 4);

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            m[i * 4 + j] = (s32)(((s64)s[i * 4 + 0] * tmp[j] +
                                  (s64)s[i * 4 + 1] * tmp[4 + j] +
                                  (s64)s[i * 4 + 2] * tmp[8 + j] +
                                  (s64)s[i * 4 + 3] * tmp[12 + j]) >> 12);
}

static void mtx_mult_4x3(s32 *m, const s32 *s)
{
    s32 tmp[16];
    int i, j;
    memcpy(tmp, m, 16 * 4);

    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++)
            m[i * 4 + j] = (s32)(((s64)s[i * 3 + 0] * tmp[j] +
                                  (s64)s[i * 3 + 1] * tmp[4 + j] +
                                  (s64)s[i * 3 + 2] * tmp[8 + j]) >> 12);

    for (j = 0; j < 4; j++)
        m[12 + j] = (s32)(((s64)s[9] * tmp[j] +
                           (s64)s[10] * tmp[4 + j] +
                           (s64)s[11] * tmp[8 + j] +
                           (s64)0x1000 * tmp[12 + j]) >> 12);
}

static void mtx_mult_3x3(s32 *m, const s32 *s)
{
    s32 tmp[12];
    int i, j;
    memcpy(tmp, m, 12 * 4);

    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++)
            m[i * 4 + j] = (s32)(((s64)s[i * 3 + 0] * tmp[j] +
                                  (s64)s[i * 3 + 1] * tmp[4 + j] +
                                  (s64)s[i * 3 + 2] * tmp[8 + j]) >> 12);
}

static void mtx_scale(s32 *m, const s32 *s)
{
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++)
            m[i * 4 + j] = (s32)(((s64)s[i] * m[i * 4 + j]) >> 12);
}

static void mtx_translate(s32 *m, const s32 *s)
{
    int j;
    for (j = 0; j < 4; j++)
        m[12 + j] += (s32)(((s64)s[0] * m[j] +
                            (s64)s[1] * m[4 + j] +
                            (s64)s[2] * m[8 + j]) >> 12);
}

/*
 * Wide rendering, the recomp-style widescreen enhancement. Zero means
 * native. A wide width scales the clip matrix's X output column by 256/W,
 * which widens the horizontal field of view while the viewport (scaled the
 * other way at the VIEWPORT command below) keeps one world unit the same
 * number of pixels: the centre 256 columns of a wide render are exactly the
 * native picture, and the margins are world that was always there and never
 * had pixels. The scale is applied where vertices and box-test cubes enter
 * the pipeline, and deliberately NOT to anything the game can read back,
 * the clip matrix and POS_TEST answer with the console's own numbers, so
 * the billboard matrices the game builds from them place a character's quad
 * exactly over its shadow, while the box test still culls against the wider
 * frustum that is actually being drawn.
 */
static int      gx_wide_w;      /* 0 = native, else the wide width          */
static int32_t  gx_wide_f12;    /* 256 * 4096 / width, 1.12 fixed           */
static int      gx_hd = 1;      /* 3D resolution scale: 1 native, up to 4   */

/*
 * The final-position and viewport field widths, which are not constants.
 *
 * A vertex's screen position lands in a hardware field and wraps inside it;
 * so does the viewport's width and height. On a DS those fields are 9 and 8
 * bits for a position and 10 and 9 for a viewport extent, and the numbers
 * were spelled out as literals here while the only scale above native was 2.
 *
 * They are field widths over a surface, though, not properties of the game's
 * coordinates: at scale S the surface is S times as wide and S times as tall,
 * so the field that holds a coordinate in it needs the same headroom S times
 * over. Sizing them that way is what let the ceiling move off 2, and at
 * S = 1 and S = 2 these produce exactly the literals they replace, so no
 * native or double render moved a bit.
 */
static u32 gx_mask_x  = 0x1FFu, gx_mask_y  = 0xFFu;
static u32 gx_vpmask_x = 0x3FFu, gx_vpmask_y = 0x1FFu;
/* How many bits a Y coordinate takes in the polygon sort key; see the key
 * itself, in submit_polygon(). Eight at the DS's own size. */
static u32 gx_sortshift = 8;

/* The smallest 2^n - 1 that can hold every value below `span`. */
static u32 field_mask(u32 span)
{
    u32 m = 1;

    while (m < span) m <<= 1;
    return m - 1;
}

static void hd_masks(void)
{
    u32 m;

    gx_mask_x   = field_mask(512u * (u32)gx_hd);
    gx_mask_y   = field_mask(256u * (u32)gx_hd);
    gx_vpmask_x = field_mask(1024u * (u32)gx_hd);
    gx_vpmask_y = field_mask(512u * (u32)gx_hd);

    /* The sort key packs a Y into this many bits, so it is the Y field's own
     * width; which is what makes the packing collision-free by
     * construction rather than by the screen happening to be 192 rows. */
    gx_sortshift = 0;
    for (m = gx_mask_y; m != 0; m >>= 1) gx_sortshift++;
}

void pc_gpu3d_set_wide(int width)
{
    if (width <= 256) {
        gx_wide_w = 0;
        gx_wide_f12 = 4096;
    } else {
        gx_wide_w = width;
        gx_wide_f12 = (int32_t)((256 * 4096 + width / 2) / width);
    }
    gx.clip_dirty = 1;
}

int pc_gpu3d_wide_width(void)
{
    return gx_wide_w;
}

/*
 * True high-resolution 3D: the viewport gains S times the pixels on both
 * axes while the field of view stays put, so the rasterizer resolves real
 * sub-pixel detail. Purely a render-side scale; nothing the game reads
 * changes, and the 2D compositor keeps consuming a native-resolution
 * point-sample of the result.
 *
 * The coordinate fields above grow with S; everything else here is already
 * written in terms of gx_hd, so 3 and 4 needed no arithmetic of their own.
 */
void pc_gpu3d_set_hd(int scale)
{
    gx_hd = (scale >= 2 && scale <= PC_GPU3D_HD_MAX) ? scale : 1;
    hd_masks();
}

int pc_gpu3d_hd_scale(void)
{
    return gx_hd;
}

static void update_clip_matrix(void)
{
    if (!gx.clip_dirty) return;
    gx.clip_dirty = 0;
    memcpy(gx.clip, gx.proj, 16 * 4);
    mtx_mult_4x4(gx.clip, gx.pos);
}

/* ------------------------------------------------------------------ */
/* Clipping, upstream's ClipSegment/ClipAgainstPlane/ClipPolygon      */
/* ------------------------------------------------------------------ */

/*
 * The templates upstream instantiates over <comp, plane, attribs> become
 * ordinary parameters. `factor_den` is deliberately s32 while `factor_num` is
 * s64: upstream truncates the difference to 32 bits before dividing and the
 * result differs when it overflows, so widening it here would be a silent
 * change to the arithmetic rather than a tidy-up.
 */
#define CLIP_INTERPOLATE(vinf, voutf) \
    ((s32)((vinf) + (((s64)((voutf) - (vinf)) * factor_num) / factor_den)))

static void clip_segment(PcGxVertex *out, const PcGxVertex *vin,
                         const PcGxVertex *vout, int comp, s32 plane,
                         int attribs)
{
    /* both differences are computed in 32 bits and *then* widened, which is
     * upstream's order and is not the same as computing them in 64: these
     * overflow for a vertex far outside the frustum and the wrapped value is
     * what the divider then sees */
    s64 factor_num = (s32)(vin->Position[3] - (plane * vin->Position[comp]));
    s32 factor_den = (s32)(factor_num -
                           (s64)(s32)(vout->Position[3] -
                                      (plane * vout->Position[comp])));
    int i;

    for (i = 0; i < 3; i++) {
        if (i == comp) continue;
        out->Position[i] = CLIP_INTERPOLATE(vin->Position[i], vout->Position[i]);
    }
    out->Position[3] = CLIP_INTERPOLATE(vin->Position[3], vout->Position[3]);
    out->Position[comp] = plane * out->Position[3];

    if (attribs) {
        for (i = 0; i < 3; i++)
            out->Color[i] = CLIP_INTERPOLATE(vin->Color[i], vout->Color[i]);
        for (i = 0; i < 2; i++)
            out->TexCoords[i] = (s16)CLIP_INTERPOLATE(vin->TexCoords[i],
                                                      vout->TexCoords[i]);
    }

    out->Clipped = 1;
}

static int clip_against_plane(PcGxVertex *vertices, int nverts, int clipstart,
                              int comp, int attribs)
{
    PcGxVertex temp[10];
    int prev, next, i;
    int c = clipstart;

    if (clipstart == 2) {
        temp[0] = vertices[0];
        temp[1] = vertices[1];
    }

    for (i = clipstart; i < nverts; i++) {
        PcGxVertex vtx;
        prev = i - 1; if (prev < 0) prev = nverts - 1;
        next = i + 1; if (next >= nverts) next = 0;

        vtx = vertices[i];
        if (vtx.Position[comp] > vtx.Position[3]) {
            PcGxVertex *vprev, *vnext;

            /* The far plane is only clipped against when POLYGON_ATTR bit 12
             * says so; otherwise the whole polygon goes */
            if (comp == 2 && !(gx.cur_polygon_attr & (1 << 12))) return 0;

            vprev = &vertices[prev];
            if (vprev->Position[comp] <= vprev->Position[3])
                clip_segment(&temp[c++], &vtx, vprev, comp, 1, attribs);

            vnext = &vertices[next];
            if (vnext->Position[comp] <= vnext->Position[3])
                clip_segment(&temp[c++], &vtx, vnext, comp, 1, attribs);
        } else {
            temp[c++] = vtx;
        }
    }

    nverts = c; c = clipstart;
    for (i = clipstart; i < nverts; i++) {
        PcGxVertex vtx;
        prev = i - 1; if (prev < 0) prev = nverts - 1;
        next = i + 1; if (next >= nverts) next = 0;

        vtx = temp[i];
        if (vtx.Position[comp] < -vtx.Position[3]) {
            PcGxVertex *vprev, *vnext;

            vprev = &temp[prev];
            if (vprev->Position[comp] >= -vprev->Position[3])
                clip_segment(&vertices[c++], &vtx, vprev, comp, -1, attribs);

            vnext = &temp[next];
            if (vnext->Position[comp] >= -vnext->Position[3])
                clip_segment(&vertices[c++], &vtx, vnext, comp, -1, attribs);
        } else {
            vertices[c++] = vtx;
        }
    }

    for (i = 0; i < c; i++) {
        PcGxVertex *vtx = &vertices[i];
        vtx->Color[0] &= ~0xFFF; vtx->Color[0] += 0xFFF;
        vtx->Color[1] &= ~0xFFF; vtx->Color[1] += 0xFFF;
        vtx->Color[2] &= ~0xFFF; vtx->Color[2] += 0xFFF;
    }

    return c;
}

/* Z, then Y, then X, upstream's order, and it is not arbitrary: the comment
 * there records that hardware appears to process Y before X and that the two
 * disagree for W=0 vertices. Reordering this is a behaviour change. */
static int clip_polygon(PcGxVertex *vertices, int nverts, int clipstart,
                        int attribs)
{
    nverts = clip_against_plane(vertices, nverts, clipstart, 2, attribs);
    nverts = clip_against_plane(vertices, nverts, clipstart, 1, attribs);
    nverts = clip_against_plane(vertices, nverts, clipstart, 0, attribs);
    return nverts;
}

static int clip_coords_equal(const PcGxVertex *a, const PcGxVertex *b)
{
    return a->Position[0] == b->Position[0] &&
           a->Position[1] == b->Position[1] &&
           a->Position[2] == b->Position[2] &&
           a->Position[3] == b->Position[3];
}

/* ------------------------------------------------------------------ */
/* Polygon assembly                                                    */
/* ------------------------------------------------------------------ */

static void submit_polygon(void)
{
    PcGxVertex clippedvertices[10];
    PcGxVertex *reusedvertices[2];
    PcGxPolygon *poly;
    int clipstart = 0;
    int lastpolyverts = 0;
    int nverts = (gx.polygon_mode & 0x1) ? 4 : 3;
    int i, polytype;
    PcGxVertex *v0, *v1, *v2;
    s64 normalX, normalY, normalZ, dot;
    int facingview;
    u32 vtop = 0, vbot = 0, wsize = 0;
    /* Seeded with the render extents, which the wide and HD modes grow. */
    s32 scr_h = 192 * gx_hd;
    s32 ytop = scr_h, ybot = 0;
    s32 xtop = (gx_wide_w != 0 ? gx_wide_w : 256) * gx_hd, xbot = 0;
    u32 texfmt, polyalpha;

    v0 = &gx.temp_vtx[0];
    v1 = &gx.temp_vtx[1];
    v2 = &gx.temp_vtx[2];

    normalX = ((s64)(v0->Position[1] - v1->Position[1]) * (v2->Position[3] - v1->Position[3]))
            - ((s64)(v0->Position[3] - v1->Position[3]) * (v2->Position[1] - v1->Position[1]));
    normalY = ((s64)(v0->Position[3] - v1->Position[3]) * (v2->Position[0] - v1->Position[0]))
            - ((s64)(v0->Position[0] - v1->Position[0]) * (v2->Position[3] - v1->Position[3]));
    normalZ = ((s64)(v0->Position[0] - v1->Position[0]) * (v2->Position[1] - v1->Position[1]))
            - ((s64)(v0->Position[1] - v1->Position[1]) * (v2->Position[0] - v1->Position[0]));

    while ((((normalX >> 31) ^ (normalX >> 63)) != 0) ||
           (((normalY >> 31) ^ (normalY >> 63)) != 0) ||
           (((normalZ >> 31) ^ (normalZ >> 63)) != 0)) {
        normalX >>= 4;
        normalY >>= 4;
        normalZ >>= 4;
    }

    dot = ((s64)v1->Position[0] * normalX) + ((s64)v1->Position[1] * normalY)
        + ((s64)v1->Position[3] * normalZ);

    facingview = (dot <= 0);

    if (dot < 0) {
        if (!(gx.cur_polygon_attr & (1 << 7))) { gx.last_strip_polygon = NULL; return; }
    } else if (dot > 0) {
        if (!(gx.cur_polygon_attr & (1 << 6))) { gx.last_strip_polygon = NULL; return; }
    }

    /* strips reuse the previous polygon's last two vertices, if neither was
     * created by the clipper and the two polygons are the same shape */
    if (gx.polygon_mode >= 2 && gx.last_strip_polygon) {
        int id0, id1;
        if (gx.polygon_mode == 2) {
            if (gx.num_consecutive_polygons & 1) { id0 = 2; id1 = 1; }
            else                                 { id0 = 0; id1 = 2; }
            lastpolyverts = 3;
        } else {
            id0 = 3; id1 = 2;
            lastpolyverts = 4;
        }

        if (gx.last_strip_polygon->NumVertices == (u32)lastpolyverts &&
            !gx.last_strip_polygon->Vertices[id0]->Clipped &&
            !gx.last_strip_polygon->Vertices[id1]->Clipped) {
            reusedvertices[0] = gx.last_strip_polygon->Vertices[id0];
            reusedvertices[1] = gx.last_strip_polygon->Vertices[id1];

            clippedvertices[0] = *reusedvertices[0];
            clippedvertices[1] = *reusedvertices[1];

            clipstart = 2;
        }
    }

    for (i = clipstart; i < nverts; i++)
        clippedvertices[i] = gx.temp_vtx[i];

    polytype = 0;
    if (nverts == 3) {
        if (clip_coords_equal(&clippedvertices[0], &clippedvertices[1]) ||
            clip_coords_equal(&clippedvertices[0], &clippedvertices[2]) ||
            clip_coords_equal(&clippedvertices[1], &clippedvertices[2]))
            polytype = 1;
    }

    nverts = clip_polygon(clippedvertices, nverts, clipstart, 1);
    if (nverts == 0) { gx.last_strip_polygon = NULL; return; }

    if (gx.num_polygons >= PC_GX_MAX_POLYGONS ||
        gx.num_vertices + (u32)nverts > PC_GX_MAX_VERTICES) {
        gx.last_strip_polygon = NULL;
        /* DISP3DCNT bit 13 is the hardware's "polygon or vertex RAM
         * overflowed" flag. It is a rendering register, so it lives in guest
         * memory rather than in this struct; setting it is the one write the
         * geometry side makes outside its own window. */
        io_write16(REG_DISP3DCNT, (u16)(io_read16(REG_DISP3DCNT) | (1 << 13)));
        return;
    }

    for (i = clipstart; i < nverts; i++) {
        PcGxVertex *vtx = &clippedvertices[i];
        u32 posX, posY, w;

        /* W is truncated to 24 bits here; a zero W means the polygon is not
         * drawn, and the divisions below are the hardware's 32-bit divider,
         * which is why a W above 0xFFFF loses a bit first */
        vtx->Position[3] &= 0x00FFFFFF;
        w = (u32)vtx->Position[3];

        if (w == 0) {
            posX = 0;
            posY = 0;
        } else {
            u32 den = w;
            posX = (u32)vtx->Position[0] + w;
            posY = (u32)(-vtx->Position[1]) + w;

            if (w > 0xFFFF) {
                posX >>= 1;
                posY >>= 1;
                den  >>= 1;
            }

            den <<= 1;
            posX = ((posX * gx.viewport[4]) / den) + gx.viewport[0];
            posY = ((posY * gx.viewport[5]) / den) + gx.viewport[3];
        }

        vtx->FinalPosition[0] = (s32)(posX & gx_mask_x);
        vtx->FinalPosition[1] = (s32)(posY & gx_mask_y);
    }

    /* zero-dot W rejection */
    if (!(gx.cur_polygon_attr & (1 << 13))) {
        int zerodot = 1, allbehind = 1;

        for (i = 0; i < nverts; i++) {
            PcGxVertex *vtx = &clippedvertices[i];

            if (vtx->FinalPosition[0] != clippedvertices[0].FinalPosition[0] ||
                vtx->FinalPosition[1] != clippedvertices[0].FinalPosition[1]) {
                zerodot = 0;
                break;
            }
            if ((u32)vtx->Position[3] <= gx.zero_dot_w_limit) {
                allbehind = 0;
                break;
            }
        }

        if (zerodot && allbehind) { gx.last_strip_polygon = NULL; return; }
    }

    poly = &gx.cur_polygon_ram[gx.num_polygons++];
    poly->NumVertices = 0;

    poly->Attr = gx.cur_polygon_attr;
    poly->TexParam = gx.tex_param;
    poly->TexPalette = gx.tex_palette;

    poly->Degenerate = 0;
    poly->FacingView = (u8)facingview;

    texfmt = (gx.tex_param >> 26) & 0x7;
    polyalpha = (gx.cur_polygon_attr >> 16) & 0x1F;
    poly->Translucent = (u8)((texfmt == 1 || texfmt == 6) ||
                             (polyalpha > 0 && polyalpha < 31));

    poly->IsShadowMask = (u8)((gx.cur_polygon_attr & 0x3F000030) == 0x00000030);
    poly->IsShadow = (u8)(((gx.cur_polygon_attr & 0x30) == 0x30) && !poly->IsShadowMask);

    if (!poly->Translucent) gx.num_opaque_polygons++;

    poly->Type = polytype;

    if (gx.last_strip_polygon && clipstart > 0) {
        if (nverts == lastpolyverts) {
            poly->Vertices[0] = reusedvertices[0];
            poly->Vertices[1] = reusedvertices[1];
        } else {
            PcGxVertex a = *reusedvertices[0];
            PcGxVertex b = *reusedvertices[1];

            gx.cur_vertex_ram[gx.num_vertices] = a;
            poly->Vertices[0] = &gx.cur_vertex_ram[gx.num_vertices];
            gx.cur_vertex_ram[gx.num_vertices + 1] = b;
            poly->Vertices[1] = &gx.cur_vertex_ram[gx.num_vertices + 1];
            gx.num_vertices += 2;
        }
        poly->NumVertices += 2;
    }

    for (i = clipstart; i < nverts; i++) {
        PcGxVertex *vtx = &gx.cur_vertex_ram[gx.num_vertices];
        *vtx = clippedvertices[i];
        poly->Vertices[i] = vtx;

        gx.num_vertices++;
        poly->NumVertices++;

        vtx->FinalColor[0] = vtx->Color[0] >> 12;
        if (vtx->FinalColor[0]) vtx->FinalColor[0] = (vtx->FinalColor[0] << 4) + 0xF;
        vtx->FinalColor[1] = vtx->Color[1] >> 12;
        if (vtx->FinalColor[1]) vtx->FinalColor[1] = (vtx->FinalColor[1] << 4) + 0xF;
        vtx->FinalColor[2] = vtx->Color[2] >> 12;
        if (vtx->FinalColor[2]) vtx->FinalColor[2] = (vtx->FinalColor[2] << 4) + 0xF;
    }

    for (i = 0; i < nverts; i++) {
        PcGxVertex *vtx = poly->Vertices[i];
        u32 w;

        if (vtx->FinalPosition[1] < ytop) {
            xtop = vtx->FinalPosition[0];
            ytop = vtx->FinalPosition[1];
            vtop = (u32)i;
        }
        if (vtx->FinalPosition[1] > ybot ||
            (vtx->FinalPosition[1] == ybot && vtx->FinalPosition[0] > xbot)) {
            xbot = vtx->FinalPosition[0];
            ybot = vtx->FinalPosition[1];
            vbot = (u32)i;
        }

        w = (u32)vtx->Position[3];
        if (w == 0) poly->Degenerate = 1;

        while ((w >> wsize) && (wsize < 32))
            wsize += 4;
    }

    poly->VTop = vtop; poly->VBottom = vbot;
    poly->YTop = ytop; poly->YBottom = ybot;
    poly->XTop = xtop; poly->XBottom = xbot;

    if (ybot > scr_h) poly->Degenerate = 1;

    /*
     * The sort key, and the one field width in this file that was not a
     * COORDINATE MASK.
     *
     * Polygons are ordered by the row they end on, then the row they start
     * on, with translucent ones after every opaque one, and that last part
     * is carried by a bit ABOVE both Y fields rather than by the partition
     * below, because the stable sort that follows runs over opaque and
     * translucent together.
     *
     * Eight bits each was exactly right for a 192-row screen and silently
     * wrong above one. At four times the resolution a Y is ten bits, so ytop
     * spilled into ybot's field and ybot spilled clean over the translucent
     * flag: a translucent polygon stopped sorting as translucent, landed
     * among the opaque ones, and the opaque ones drew over it. What that
     * looked like was an overworld character's drop shadow vanishing, the
     * polygon submitted, projected and rasterized correctly, and then
     * painted over by the ground.
     *
     * The width is the Y field's own, so the packing cannot collide at any
     * scale, and at the DS's it is bit for bit the shift it replaces.
     */
    poly->SortKey = ((u32)ybot << gx_sortshift) | (u32)ytop;
    if (poly->Translucent) poly->SortKey |= 1u << (gx_sortshift * 2u);

    poly->WBuffer = (u8)((gx.flush_attributes & 0x2) != 0);

    for (i = 0; i < nverts; i++) {
        PcGxVertex *vtx = poly->Vertices[i];
        s32 w, wshifted, z;

        if (wsize < 16) {
            w = vtx->Position[3] << (16 - wsize);
            wshifted = w >> (16 - wsize);
        } else {
            w = vtx->Position[3] >> (wsize - 16);
            wshifted = w << (wsize - 16);
        }

        if (gx.flush_attributes & 0x2)
            z = wshifted;
        else if (vtx->Position[3])
            z = (s32)(((((s64)vtx->Position[2] * 0x4000) / vtx->Position[3]) + 0x3FFF) * 0x200);
        else
            z = 0x7FFE00;

        if (z < 0) z = 0;
        else if (z > 0xFFFFFF) z = 0xFFFFFF;

        poly->FinalZ[i] = z;
        poly->FinalW[i] = w;
    }

    if (gx.polygon_mode >= 2) gx.last_strip_polygon = poly;
    else                      gx.last_strip_polygon = NULL;
}

static void submit_vertex(void)
{
    s64 vertex[4];
    PcGxVertex *vt = &gx.temp_vtx[gx.vertex_num_in_poly];

    vertex[0] = gx.cur_vertex[0];
    vertex[1] = gx.cur_vertex[1];
    vertex[2] = gx.cur_vertex[2];
    vertex[3] = 0x1000;

    update_clip_matrix();
    vt->Position[0] = (s32)((vertex[0]*gx.clip[0] + vertex[1]*gx.clip[4] + vertex[2]*gx.clip[8]  + vertex[3]*gx.clip[12]) >> 12);
    /* Wide rendering scales the X the rasterizer sees, and nothing the
     * game can read: the clip matrix, POS_TEST and the matrix readbacks
     * stay the console's own, so the billboard and placement math the
     * game does against them lands exactly where a native run puts it. */
    if (gx_wide_w != 0) {
        vt->Position[0] = (s32)(((s64)vt->Position[0] * gx_wide_f12) >> 12);
    }
    vt->Position[1] = (s32)((vertex[0]*gx.clip[1] + vertex[1]*gx.clip[5] + vertex[2]*gx.clip[9]  + vertex[3]*gx.clip[13]) >> 12);
    vt->Position[2] = (s32)((vertex[0]*gx.clip[2] + vertex[1]*gx.clip[6] + vertex[2]*gx.clip[10] + vertex[3]*gx.clip[14]) >> 12);
    vt->Position[3] = (s32)((vertex[0]*gx.clip[3] + vertex[1]*gx.clip[7] + vertex[2]*gx.clip[11] + vertex[3]*gx.clip[15]) >> 12);

    vt->Color[0] = ((s32)gx.vertex_color[0] << 12) + 0xFFF;
    vt->Color[1] = ((s32)gx.vertex_color[1] << 12) + 0xFFF;
    vt->Color[2] = ((s32)gx.vertex_color[2] << 12) + 0xFFF;

    if ((gx.tex_param >> 30) == 3) {
        vt->TexCoords[0] = (s16)(((vertex[0]*gx.tex[0] + vertex[1]*gx.tex[4] + vertex[2]*gx.tex[8]) >> 24) + gx.raw_texcoords[0]);
        vt->TexCoords[1] = (s16)(((vertex[0]*gx.tex[1] + vertex[1]*gx.tex[5] + vertex[2]*gx.tex[9]) >> 24) + gx.raw_texcoords[1]);
    } else {
        vt->TexCoords[0] = gx.texcoords[0];
        vt->TexCoords[1] = gx.texcoords[1];
    }

    vt->Clipped = 0;

    gx.vertex_num++;
    gx.vertex_num_in_poly++;

    switch (gx.polygon_mode) {
    case 0:
        if (gx.vertex_num_in_poly == 3) {
            gx.vertex_num_in_poly = 0;
            submit_polygon();
            gx.num_consecutive_polygons++;
        }
        break;

    case 1:
        if (gx.vertex_num_in_poly == 4) {
            gx.vertex_num_in_poly = 0;
            submit_polygon();
            gx.num_consecutive_polygons++;
        }
        break;

    case 2:
        if (gx.num_consecutive_polygons & 1) {
            PcGxVertex tmp = gx.temp_vtx[1];
            gx.temp_vtx[1] = gx.temp_vtx[0];
            gx.temp_vtx[0] = tmp;

            gx.vertex_num_in_poly = 2;
            submit_polygon();
            gx.num_consecutive_polygons++;

            gx.temp_vtx[1] = gx.temp_vtx[2];
        } else if (gx.vertex_num_in_poly == 3) {
            gx.vertex_num_in_poly = 2;
            submit_polygon();
            gx.num_consecutive_polygons++;

            gx.temp_vtx[0] = gx.temp_vtx[1];
            gx.temp_vtx[1] = gx.temp_vtx[2];
        }
        break;

    case 3:
        if (gx.vertex_num_in_poly == 4) {
            PcGxVertex tmp = gx.temp_vtx[3];
            gx.temp_vtx[3] = gx.temp_vtx[2];
            gx.temp_vtx[2] = tmp;

            gx.vertex_num_in_poly = 2;
            submit_polygon();
            gx.num_consecutive_polygons++;

            gx.temp_vtx[0] = gx.temp_vtx[3];
            gx.temp_vtx[1] = gx.temp_vtx[2];
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Lighting                                                            */
/* ------------------------------------------------------------------ */

static void calculate_lighting(void)
{
    s32 normaltrans[3];
    u32 vtxbuff[3];
    int i;

    if ((gx.tex_param >> 30) == 2) {
        gx.texcoords[0] = (s16)(gx.raw_texcoords[0] +
            (((s64)gx.normal[0]*gx.tex[0] + (s64)gx.normal[1]*gx.tex[4] + (s64)gx.normal[2]*gx.tex[8]) >> 21));
        gx.texcoords[1] = (s16)(gx.raw_texcoords[1] +
            (((s64)gx.normal[0]*gx.tex[1] + (s64)gx.normal[1]*gx.tex[5] + (s64)gx.normal[2]*gx.tex[9]) >> 21));
    }

    normaltrans[0] = ((gx.normal[0]*gx.vec[0] + gx.normal[1]*gx.vec[4] + gx.normal[2]*gx.vec[8])  << 9) >> 21;
    normaltrans[1] = ((gx.normal[0]*gx.vec[1] + gx.normal[1]*gx.vec[5] + gx.normal[2]*gx.vec[9])  << 9) >> 21;
    normaltrans[2] = ((gx.normal[0]*gx.vec[2] + gx.normal[1]*gx.vec[6] + gx.normal[2]*gx.vec[10]) << 9) >> 21;

    vtxbuff[0] = (u32)gx.mat_emission[0] << 14;
    vtxbuff[1] = (u32)gx.mat_emission[1] << 14;
    vtxbuff[2] = (u32)gx.mat_emission[2] << 14;

    for (i = 0; i < 4; i++) {
        s32 dot, shinelevel;

        if (!(gx.cur_polygon_attr & (1 << i))) continue;

        /* The bottom 9 bits go before the sum, not after it */
        dot = ((gx.light_dir[i][0]*normaltrans[0]) >> 9) +
              ((gx.light_dir[i][1]*normaltrans[1]) >> 9) +
              ((gx.light_dir[i][2]*normaltrans[2]) >> 9);

        if (dot > 0) {
            s32 diffdot = (dot << 21) >> 21;
            vtxbuff[0] += ((u32)gx.mat_diffuse[0] * gx.light_color[i][0] * (u32)diffdot) & 0xFFFFF;
            vtxbuff[1] += ((u32)gx.mat_diffuse[1] * gx.light_color[i][1] * (u32)diffdot) & 0xFFFFF;
            vtxbuff[2] += ((u32)gx.mat_diffuse[2] * gx.light_color[i][2] * (u32)diffdot) & 0xFFFFF;

            dot += normaltrans[2];
            dot = (dot << 21) >> 21;
            dot = ((dot * dot) >> 10) & 0x3FF;

            shinelevel = ((dot * gx.spec_recip[i]) >> 8) - (1 << 9);

            if (shinelevel < 0) {
                shinelevel = 0;
            } else {
                shinelevel = (shinelevel << 18) >> 18;
                if (shinelevel < 0) shinelevel = 0;
                else if (shinelevel > 0x1FF) shinelevel = 0x1FF;
            }
        } else {
            shinelevel = 0;
        }

        if (gx.use_shininess) {
            shinelevel >>= 2;
            shinelevel = gx.shininess[shinelevel];
            shinelevel <<= 1;
        }

        vtxbuff[0] += (u32)(((gx.mat_specular[0] * shinelevel) + (gx.mat_ambient[0] << 9)) * gx.light_color[i][0]);
        vtxbuff[1] += (u32)(((gx.mat_specular[1] * shinelevel) + (gx.mat_ambient[1] << 9)) * gx.light_color[i][1]);
        vtxbuff[2] += (u32)(((gx.mat_specular[2] * shinelevel) + (gx.mat_ambient[2] << 9)) * gx.light_color[i][2]);
    }

    gx.vertex_color[0] = (u8)((vtxbuff[0] >> 14 > 31) ? 31 : (vtxbuff[0] >> 14));
    gx.vertex_color[1] = (u8)((vtxbuff[1] >> 14 > 31) ? 31 : (vtxbuff[1] >> 14));
    gx.vertex_color[2] = (u8)((vtxbuff[2] >> 14 > 31) ? 31 : (vtxbuff[2] >> 14));
}

/* ------------------------------------------------------------------ */
/* The three test commands                                             */
/* ------------------------------------------------------------------ */

static void box_test(const u32 *params)
{
    PcGxVertex cube[8];
    PcGxVertex face[10];
    s16 x0, y0, z0, x1, y1, z1;
    int i;

    memset(cube, 0, sizeof(cube));
    memset(face, 0, sizeof(face));

    gx.gxstat &= ~(1 << 1);

    x0 = (s16)(params[0] & 0xFFFF);
    y0 = (s16)(((s32)params[0]) >> 16);
    z0 = (s16)(params[1] & 0xFFFF);
    x1 = (s16)(((s32)params[1]) >> 16);
    y1 = (s16)(params[2] & 0xFFFF);
    z1 = (s16)(((s32)params[2]) >> 16);

    x1 = (s16)(x1 + x0);
    y1 = (s16)(y1 + y0);
    z1 = (s16)(z1 + z0);

    cube[0].Position[0] = x0; cube[0].Position[1] = y0; cube[0].Position[2] = z0;
    cube[1].Position[0] = x1; cube[1].Position[1] = y0; cube[1].Position[2] = z0;
    cube[2].Position[0] = x1; cube[2].Position[1] = y1; cube[2].Position[2] = z0;
    cube[3].Position[0] = x0; cube[3].Position[1] = y1; cube[3].Position[2] = z0;
    cube[4].Position[0] = x0; cube[4].Position[1] = y1; cube[4].Position[2] = z1;
    cube[5].Position[0] = x0; cube[5].Position[1] = y0; cube[5].Position[2] = z1;
    cube[6].Position[0] = x1; cube[6].Position[1] = y0; cube[6].Position[2] = z1;
    cube[7].Position[0] = x1; cube[7].Position[1] = y1; cube[7].Position[2] = z1;

    update_clip_matrix();
    for (i = 0; i < 8; i++) {
        s32 x = cube[i].Position[0];
        s32 y = cube[i].Position[1];
        s32 z = cube[i].Position[2];

        cube[i].Position[0] = (s32)(((s64)x*gx.clip[0] + (s64)y*gx.clip[4] + (s64)z*gx.clip[8]  + (s64)0x1000*gx.clip[12]) >> 12);
        if (gx_wide_w != 0) {
            cube[i].Position[0] =
                (s32)(((s64)cube[i].Position[0] * gx_wide_f12) >> 12);
        }
        cube[i].Position[1] = (s32)(((s64)x*gx.clip[1] + (s64)y*gx.clip[5] + (s64)z*gx.clip[9]  + (s64)0x1000*gx.clip[13]) >> 12);
        cube[i].Position[2] = (s32)(((s64)x*gx.clip[2] + (s64)y*gx.clip[6] + (s64)z*gx.clip[10] + (s64)0x1000*gx.clip[14]) >> 12);
        cube[i].Position[3] = (s32)(((s64)x*gx.clip[3] + (s64)y*gx.clip[7] + (s64)z*gx.clip[11] + (s64)0x1000*gx.clip[15]) >> 12);
    }

    {
        static const int faces[6][4] = {
            { 0, 1, 2, 3 },   /* -Z */
            { 4, 5, 6, 7 },   /* +Z */
            { 0, 3, 4, 5 },   /* -X */
            { 1, 2, 7, 6 },   /* +X */
            { 0, 1, 6, 5 },   /* -Y */
            { 2, 3, 4, 7 }    /* +Y */
        };
        int f, k;

        for (f = 0; f < 6; f++) {
            for (k = 0; k < 4; k++) face[k] = cube[faces[f][k]];
            if (clip_polygon(face, 4, 0, 0) > 0) {
                gx.gxstat |= (1 << 1);
                return;
            }
        }
    }
}

static void pos_test(void)
{
    s64 vertex[4];

    vertex[0] = gx.cur_vertex[0];
    vertex[1] = gx.cur_vertex[1];
    vertex[2] = gx.cur_vertex[2];
    vertex[3] = 0x1000;

    update_clip_matrix();
    gx.pos_test_result[0] = (s32)((vertex[0]*gx.clip[0] + vertex[1]*gx.clip[4] + vertex[2]*gx.clip[8]  + vertex[3]*gx.clip[12]) >> 12);
    gx.pos_test_result[1] = (s32)((vertex[0]*gx.clip[1] + vertex[1]*gx.clip[5] + vertex[2]*gx.clip[9]  + vertex[3]*gx.clip[13]) >> 12);
    gx.pos_test_result[2] = (s32)((vertex[0]*gx.clip[2] + vertex[1]*gx.clip[6] + vertex[2]*gx.clip[10] + vertex[3]*gx.clip[14]) >> 12);
    gx.pos_test_result[3] = (s32)((vertex[0]*gx.clip[3] + vertex[1]*gx.clip[7] + vertex[2]*gx.clip[11] + vertex[3]*gx.clip[15]) >> 12);
}

static void vec_test(u32 param)
{
    s16 normal[3];

    normal[0] = (s16)((s16)((param & 0x000003FF) << 6) >> 6);
    normal[1] = (s16)((s16)((param & 0x000FFC00) >> 4) >> 6);
    normal[2] = (s16)((s16)((param & 0x3FF00000) >> 14) >> 6);

    gx.vec_test_result[0] = (s16)((normal[0]*gx.vec[0] + normal[1]*gx.vec[4] + normal[2]*gx.vec[8])  >> 9);
    gx.vec_test_result[1] = (s16)((normal[0]*gx.vec[1] + normal[1]*gx.vec[5] + normal[2]*gx.vec[9])  >> 9);
    gx.vec_test_result[2] = (s16)((normal[0]*gx.vec[2] + normal[1]*gx.vec[6] + normal[2]*gx.vec[10]) >> 9);

    if (gx.vec_test_result[0] & 0x1000) gx.vec_test_result[0] |= (s16)0xF000;
    if (gx.vec_test_result[1] & 0x1000) gx.vec_test_result[1] |= (s16)0xF000;
    if (gx.vec_test_result[2] & 0x1000) gx.vec_test_result[2] |= (s16)0xF000;
}

/* ------------------------------------------------------------------ */
/* The command decoder                                                 */
/* ------------------------------------------------------------------ */

static void execute_command(u32 cmd, const u32 *params)
{
    u32 param = params[0];

    switch (cmd) {
    case 0x10:  /* MTX_MODE */
        gx.matrix_mode = param & 0x3;
        break;

    case 0x11:  /* MTX_PUSH */
        if (gx.matrix_mode == 0) {
            if (gx.proj_sp > 0) gx.gxstat |= (1 << 15);
            memcpy(gx.proj_stack, gx.proj, 16 * 4);
            gx.proj_sp = (gx.proj_sp + 1) & 0x1;
        } else if (gx.matrix_mode == 3) {
            if (gx.tex_sp > 0) gx.gxstat |= (1 << 15);
            memcpy(gx.tex_stack, gx.tex, 16 * 4);
            gx.tex_sp = (gx.tex_sp + 1) & 0x1;
        } else {
            if (gx.pos_sp > 30) gx.gxstat |= (1 << 15);
            memcpy(gx.pos_stack[gx.pos_sp & 0x1F], gx.pos, 16 * 4);
            memcpy(gx.vec_stack[gx.pos_sp & 0x1F], gx.vec, 16 * 4);
            gx.pos_sp = (gx.pos_sp + 1) & 0x3F;
        }
        break;

    case 0x12:  /* MTX_POP */
        if (gx.matrix_mode == 0) {
            if (gx.proj_sp == 0) gx.gxstat |= (1 << 15);
            gx.proj_sp = (gx.proj_sp - 1) & 0x1;
            memcpy(gx.proj, gx.proj_stack, 16 * 4);
            gx.clip_dirty = 1;
        } else if (gx.matrix_mode == 3) {
            if (gx.tex_sp == 0) gx.gxstat |= (1 << 15);
            gx.tex_sp = (gx.tex_sp - 1) & 0x1;
            memcpy(gx.tex, gx.tex_stack, 16 * 4);
        } else {
            s32 offset = ((s32)(param << 26)) >> 26;
            gx.pos_sp = (gx.pos_sp - offset) & 0x3F;
            if (gx.pos_sp > 30) gx.gxstat |= (1 << 15);
            memcpy(gx.pos, gx.pos_stack[gx.pos_sp & 0x1F], 16 * 4);
            memcpy(gx.vec, gx.vec_stack[gx.pos_sp & 0x1F], 16 * 4);
            gx.clip_dirty = 1;
        }
        break;

    case 0x13:  /* MTX_STORE */
        if (gx.matrix_mode == 0) {
            memcpy(gx.proj_stack, gx.proj, 16 * 4);
        } else if (gx.matrix_mode == 3) {
            memcpy(gx.tex_stack, gx.tex, 16 * 4);
        } else {
            u32 addr = param & 0x1F;
            if (addr > 30) gx.gxstat |= (1 << 15);
            memcpy(gx.pos_stack[addr], gx.pos, 16 * 4);
            memcpy(gx.vec_stack[addr], gx.vec, 16 * 4);
        }
        break;

    case 0x14:  /* MTX_RESTORE */
        if (gx.matrix_mode == 0) {
            memcpy(gx.proj, gx.proj_stack, 16 * 4);
            gx.clip_dirty = 1;
        } else if (gx.matrix_mode == 3) {
            memcpy(gx.tex, gx.tex_stack, 16 * 4);
        } else {
            u32 addr = param & 0x1F;
            if (addr > 30) gx.gxstat |= (1 << 15);
            memcpy(gx.pos, gx.pos_stack[addr], 16 * 4);
            memcpy(gx.vec, gx.vec_stack[addr], 16 * 4);
            gx.clip_dirty = 1;
        }
        break;

    case 0x15:  /* MTX_IDENTITY */
        if (gx.matrix_mode == 0) {
            mtx_identity(gx.proj);
            gx.clip_dirty = 1;
        } else if (gx.matrix_mode == 3) {
            mtx_identity(gx.tex);
        } else {
            mtx_identity(gx.pos);
            if (gx.matrix_mode == 2) mtx_identity(gx.vec);
            gx.clip_dirty = 1;
        }
        break;

    case 0x16:  /* MTX_LOAD_4x4 */
        if (gx.matrix_mode == 0)      { mtx_load_4x4(gx.proj, (const s32 *)params); gx.clip_dirty = 1; }
        else if (gx.matrix_mode == 3) { mtx_load_4x4(gx.tex, (const s32 *)params); }
        else {
            mtx_load_4x4(gx.pos, (const s32 *)params);
            if (gx.matrix_mode == 2) mtx_load_4x4(gx.vec, (const s32 *)params);
            gx.clip_dirty = 1;
        }
        break;

    case 0x17:  /* MTX_LOAD_4x3 */
        if (gx.matrix_mode == 0)      { mtx_load_4x3(gx.proj, (const s32 *)params); gx.clip_dirty = 1; }
        else if (gx.matrix_mode == 3) { mtx_load_4x3(gx.tex, (const s32 *)params); }
        else {
            mtx_load_4x3(gx.pos, (const s32 *)params);
            if (gx.matrix_mode == 2) mtx_load_4x3(gx.vec, (const s32 *)params);
            gx.clip_dirty = 1;
        }
        break;

    case 0x18:  /* MTX_MULT_4x4 */
        if (gx.matrix_mode == 0)      { mtx_mult_4x4(gx.proj, (const s32 *)params); gx.clip_dirty = 1; }
        else if (gx.matrix_mode == 3) { mtx_mult_4x4(gx.tex, (const s32 *)params); }
        else {
            mtx_mult_4x4(gx.pos, (const s32 *)params);
            if (gx.matrix_mode == 2) mtx_mult_4x4(gx.vec, (const s32 *)params);
            gx.clip_dirty = 1;
        }
        break;

    case 0x19:  /* MTX_MULT_4x3 */
        if (gx.matrix_mode == 0)      { mtx_mult_4x3(gx.proj, (const s32 *)params); gx.clip_dirty = 1; }
        else if (gx.matrix_mode == 3) { mtx_mult_4x3(gx.tex, (const s32 *)params); }
        else {
            mtx_mult_4x3(gx.pos, (const s32 *)params);
            if (gx.matrix_mode == 2) mtx_mult_4x3(gx.vec, (const s32 *)params);
            gx.clip_dirty = 1;
        }
        break;

    case 0x1A:  /* MTX_MULT_3x3 */
        if (gx.matrix_mode == 0)      { mtx_mult_3x3(gx.proj, (const s32 *)params); gx.clip_dirty = 1; }
        else if (gx.matrix_mode == 3) { mtx_mult_3x3(gx.tex, (const s32 *)params); }
        else {
            mtx_mult_3x3(gx.pos, (const s32 *)params);
            if (gx.matrix_mode == 2) mtx_mult_3x3(gx.vec, (const s32 *)params);
            gx.clip_dirty = 1;
        }
        break;

    case 0x1B:  /* MTX_SCALE: note: never applied to the vector matrix */
        if (gx.matrix_mode == 0)      { mtx_scale(gx.proj, (const s32 *)params); gx.clip_dirty = 1; }
        else if (gx.matrix_mode == 3) { mtx_scale(gx.tex, (const s32 *)params); }
        else                          { mtx_scale(gx.pos, (const s32 *)params); gx.clip_dirty = 1; }
        break;

    case 0x1C:  /* MTX_TRANS */
        if (gx.matrix_mode == 0)      { mtx_translate(gx.proj, (const s32 *)params); gx.clip_dirty = 1; }
        else if (gx.matrix_mode == 3) { mtx_translate(gx.tex, (const s32 *)params); }
        else {
            mtx_translate(gx.pos, (const s32 *)params);
            if (gx.matrix_mode == 2) mtx_translate(gx.vec, (const s32 *)params);
            gx.clip_dirty = 1;
        }
        break;

    case 0x20:  /* COLOR */
        gx.vertex_color[0] = (u8)(param & 0x1F);
        gx.vertex_color[1] = (u8)((param >> 5) & 0x1F);
        gx.vertex_color[2] = (u8)((param >> 10) & 0x1F);
        break;

    case 0x21:  /* NORMAL */
        gx.normal[0] = (s16)((s16)((param & 0x000003FF) << 6) >> 6);
        gx.normal[1] = (s16)((s16)((param & 0x000FFC00) >> 4) >> 6);
        gx.normal[2] = (s16)((s16)((param & 0x3FF00000) >> 14) >> 6);
        calculate_lighting();
        break;

    case 0x22:  /* TEXCOORD */
        gx.raw_texcoords[0] = (s16)(param & 0xFFFF);
        gx.raw_texcoords[1] = (s16)(param >> 16);
        if ((gx.tex_param >> 30) == 1) {
            gx.texcoords[0] = (s16)((gx.raw_texcoords[0]*gx.tex[0] + gx.raw_texcoords[1]*gx.tex[4] + gx.tex[8] + gx.tex[12]) >> 12);
            gx.texcoords[1] = (s16)((gx.raw_texcoords[0]*gx.tex[1] + gx.raw_texcoords[1]*gx.tex[5] + gx.tex[9] + gx.tex[13]) >> 12);
        } else {
            gx.texcoords[0] = gx.raw_texcoords[0];
            gx.texcoords[1] = gx.raw_texcoords[1];
        }
        break;

    case 0x23:  /* VTX_16 */
        gx.cur_vertex[0] = (s16)(params[0] & 0xFFFF);
        gx.cur_vertex[1] = (s16)(params[0] >> 16);
        gx.cur_vertex[2] = (s16)(params[1] & 0xFFFF);
        submit_vertex();
        break;

    case 0x24:  /* VTX_10 */
        gx.cur_vertex[0] = (s16)((param & 0x000003FF) << 6);
        gx.cur_vertex[1] = (s16)((param & 0x000FFC00) >> 4);
        gx.cur_vertex[2] = (s16)((param & 0x3FF00000) >> 14);
        submit_vertex();
        break;

    case 0x25:  /* VTX_XY */
        gx.cur_vertex[0] = (s16)(param & 0xFFFF);
        gx.cur_vertex[1] = (s16)(param >> 16);
        submit_vertex();
        break;

    case 0x26:  /* VTX_XZ */
        gx.cur_vertex[0] = (s16)(param & 0xFFFF);
        gx.cur_vertex[2] = (s16)(param >> 16);
        submit_vertex();
        break;

    case 0x27:  /* VTX_YZ */
        gx.cur_vertex[1] = (s16)(param & 0xFFFF);
        gx.cur_vertex[2] = (s16)(param >> 16);
        submit_vertex();
        break;

    case 0x28:  /* VTX_DIFF */
        gx.cur_vertex[0] = (s16)(gx.cur_vertex[0] + (s16)((s16)((param & 0x000003FF) << 6) >> 6));
        gx.cur_vertex[1] = (s16)(gx.cur_vertex[1] + (s16)((s16)((param & 0x000FFC00) >> 4) >> 6));
        gx.cur_vertex[2] = (s16)(gx.cur_vertex[2] + (s16)((s16)((param & 0x3FF00000) >> 14) >> 6));
        submit_vertex();
        break;

    case 0x29:  /* POLYGON_ATTR */
        gx.polygon_attr = param;
        break;

    case 0x2A:  /* TEXIMAGE_PARAM */
        gx.tex_param = param;
        break;

    case 0x2B:  /* PLTT_BASE */
        gx.tex_palette = param & 0x1FFF;
        break;

    case 0x30:  /* DIF_AMB */
        gx.mat_diffuse[0] = (u8)(param & 0x1F);
        gx.mat_diffuse[1] = (u8)((param >> 5) & 0x1F);
        gx.mat_diffuse[2] = (u8)((param >> 10) & 0x1F);
        gx.mat_ambient[0] = (u8)((param >> 16) & 0x1F);
        gx.mat_ambient[1] = (u8)((param >> 21) & 0x1F);
        gx.mat_ambient[2] = (u8)((param >> 26) & 0x1F);
        if (param & 0x8000) {
            gx.vertex_color[0] = gx.mat_diffuse[0];
            gx.vertex_color[1] = gx.mat_diffuse[1];
            gx.vertex_color[2] = gx.mat_diffuse[2];
        }
        break;

    case 0x31:  /* SPE_EMI */
        gx.mat_specular[0] = (u8)(param & 0x1F);
        gx.mat_specular[1] = (u8)((param >> 5) & 0x1F);
        gx.mat_specular[2] = (u8)((param >> 10) & 0x1F);
        gx.mat_emission[0] = (u8)((param >> 16) & 0x1F);
        gx.mat_emission[1] = (u8)((param >> 21) & 0x1F);
        gx.mat_emission[2] = (u8)((param >> 26) & 0x1F);
        gx.use_shininess = (param & 0x8000) != 0;
        break;

    case 0x32: { /* LIGHT_VECTOR */
        u32 l = param >> 30;
        s16 dir[3];
        s32 den;

        dir[0] = (s16)((s16)((param & 0x000003FF) << 6) >> 6);
        dir[1] = (s16)((s16)((param & 0x000FFC00) >> 4) >> 6);
        dir[2] = (s16)((s16)((param & 0x3FF00000) >> 14) >> 6);

        /* The order here is upstream's and is very specific: for the direction
         * it is discard 12 bits, negate, sign-extend to 11 bits; for the
         * specular reciprocal it is sign-extend, discard, negate */
        gx.light_dir[l][0] = (s16)((-((dir[0]*gx.vec[0] + dir[1]*gx.vec[4] + dir[2]*gx.vec[8])  >> 12) << 21) >> 21);
        gx.light_dir[l][1] = (s16)((-((dir[0]*gx.vec[1] + dir[1]*gx.vec[5] + dir[2]*gx.vec[9])  >> 12) << 21) >> 21);
        gx.light_dir[l][2] = (s16)((-((dir[0]*gx.vec[2] + dir[1]*gx.vec[6] + dir[2]*gx.vec[10]) >> 12) << 21) >> 21);
        den =                       -(((dir[0]*gx.vec[2] + dir[1]*gx.vec[6] + dir[2]*gx.vec[10]) << 9) >> 21) + (1 << 9);

        if (den == 0) gx.spec_recip[l] = 0;
        else          gx.spec_recip[l] = (1 << 18) / den;
        break;
    }

    case 0x33: { /* LIGHT_COLOR */
        u32 l = param >> 30;
        gx.light_color[l][0] = (u8)(param & 0x1F);
        gx.light_color[l][1] = (u8)((param >> 5) & 0x1F);
        gx.light_color[l][2] = (u8)((param >> 10) & 0x1F);
        break;
    }

    case 0x34: { /* SHININESS */
        int i;
        for (i = 0; i < 128; i += 4) {
            u32 val = params[i >> 2];
            gx.shininess[i + 0] = (u8)(val & 0xFF);
            gx.shininess[i + 1] = (u8)((val >> 8) & 0xFF);
            gx.shininess[i + 2] = (u8)((val >> 16) & 0xFF);
            gx.shininess[i + 3] = (u8)(val >> 24);
        }
        break;
    }

    case 0x40:  /* BEGIN_VTXS */
        gx.polygon_mode = param & 0x3;
        gx.vertex_num = 0;
        gx.vertex_num_in_poly = 0;
        gx.num_consecutive_polygons = 0;
        gx.last_strip_polygon = NULL;
        gx.cur_polygon_attr = gx.polygon_attr;
        break;

    case 0x41:  /* END_VTXS: no effect on hardware either */
        break;

    case 0x50:  /* SWAP_BUFFERS */
        gx.flush_request = 1;
        gx.flush_attributes = param & 0x3;
        break;

    case 0x60:  /* VIEWPORT: Y runs the other way up */
        gx.viewport[0] = param & 0xFF;
        gx.viewport[1] = (191 - ((param >> 8) & 0xFF)) & 0xFF;
        gx.viewport[2] = (param >> 16) & 0xFF;
        gx.viewport[3] = (191 - (param >> 24)) & 0xFF;
        /* Wide rendering scales the game's 256-pixel-space X coordinates
         * into the wide surface, so the clip-matrix scale above and this
         * cancel at the centre and the margins gain real pixels. */
        if (gx_wide_w != 0) {
            gx.viewport[0] = (gx.viewport[0] * (u32)gx_wide_w + 128) / 256;
            gx.viewport[2] = ((gx.viewport[2] + 1) * (u32)gx_wide_w + 128)
                             / 256 - 1;
        }
        /* High-resolution 3D: S times the pixels on both axes. */
        if (gx_hd > 1) {
            gx.viewport[0] *= (u32)gx_hd;
            gx.viewport[2]  = (gx.viewport[2] + 1) * (u32)gx_hd - 1;
            gx.viewport[1]  = (gx.viewport[1] + 1) * (u32)gx_hd - 1;
            gx.viewport[3] *= (u32)gx_hd;
        }
        gx.viewport[4] = (gx.viewport[2] - gx.viewport[0] + 1) & gx_vpmask_x;
        gx.viewport[5] = (gx.viewport[1] - gx.viewport[3] + 1) & gx_vpmask_y;
        break;

    case 0x70:  /* BOX_TEST */
        box_test(params);
        break;

    case 0x71:  /* POS_TEST */
        gx.cur_vertex[0] = (s16)(params[0] & 0xFFFF);
        gx.cur_vertex[1] = (s16)(params[0] >> 16);
        gx.cur_vertex[2] = (s16)(params[1] & 0xFFFF);
        pos_test();
        break;

    case 0x72:  /* VEC_TEST */
        vec_test(param);
        break;

    default:
        /* upstream ignores an unknown opcode, and so does hardware: the
         * decoder only sees opcodes it has a parameter count for */
        break;
    }
}

/*
 * One word arriving for a command. `cmd` is the opcode, `val` the word.
 *
 * A multi-parameter command accumulates. If a word for a *different* command
 * turns up while one is incomplete, something dropped parameters, the only
 * way that happens here is a caller that cached the port's address and stored
 * through it (see pc_gpu3d.h), so it aborts rather than silently executing a
 * command with the wrong operands.
 */
static void feed_command(u32 cmd, u32 val)
{
    u32 need = gx_num_params[cmd];

    if (need <= 1) {
        if (gx.exec_cmd != 0)
            gx_fatal("command 0x%02X arrived while 0x%02X still wanted %u of "
                     "its %u parameters, a caller cached a command port's "
                     "address and stored through it",
                     cmd, gx.exec_cmd, gx_num_params[gx.exec_cmd] - gx.exec_count,
                     gx_num_params[gx.exec_cmd]);
        execute_command(cmd, &val);
        return;
    }

    if (gx.exec_cmd != 0 && gx.exec_cmd != cmd)
        gx_fatal("command 0x%02X arrived while 0x%02X still wanted %u of its "
                 "%u parameters, a caller cached a command port's address "
                 "and stored through it",
                 cmd, gx.exec_cmd, gx_num_params[gx.exec_cmd] - gx.exec_count,
                 gx_num_params[gx.exec_cmd]);

    gx.exec_cmd = cmd;
    gx.exec_params[gx.exec_count++] = val;

    if (gx.exec_count >= need) {
        u32 c = gx.exec_cmd;
        gx.exec_cmd = 0;
        gx.exec_count = 0;
        execute_command(c, gx.exec_params);
    }
}

/*
 * The packed command port at 0x04000400: the first word holds up to four
 * opcodes, then their parameters follow one per word. Upstream's
 * WriteToGXFIFO, with the FIFO itself removed, a command executes as soon as
 * its parameters have arrived.
 */
static void write_to_gxfifo(u32 val)
{
    if (gx.num_commands == 0) {
        gx.num_commands = 4;
        gx.cur_command = val;
        gx.param_count = 0;
        gx.total_params = gx_num_params[gx.cur_command & 0xFF];

        if (gx.total_params > 0) return;
    } else {
        gx.param_count++;
    }

    for (;;) {
        if ((gx.cur_command & 0xFF) ||
            (gx.num_commands == 4 && gx.cur_command == 0))
            feed_command(gx.cur_command & 0xFF, val);

        if (gx.param_count >= gx.total_params) {
            gx.cur_command >>= 8;
            gx.num_commands--;
            if (gx.num_commands == 0) break;

            gx.param_count = 0;
            gx.total_params = gx_num_params[gx.cur_command & 0xFF];
        }
        if (gx.param_count < gx.total_params) break;
    }
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static int geometry_enabled(void)
{
    /* POWCNT1 bit 3. melonDS gates every write to 0x04000400-0x040006FF on
     * it, so the port has to as well or the two disagree about a config that
     * forgot to power the engine up. */
    return (io_read16(REG_POWCNT1) & (1 << 3)) != 0;
}

static u32 gxstat_read(void)
{
    /* The FIFO is always empty and the engine is never busy, settled
     * decision 35, so bits 0, 14, 16-24 and 27 are zero and 25/26 are set.
     * Bits 1 and 15 are real state; 8-13 are the two stack pointers. */
    return gx.gxstat
         | ((u32)(gx.pos_sp & 0x1F) << 8)
         | ((u32)(gx.proj_sp & 0x1) << 13)
         | (1u << 25)
         | (1u << 26);
}

static void gxstat_write(u32 val)
{
    if (val & 0x8000) {
        gx.gxstat &= ~0x8000u;
        gx.proj_sp = 0;
        gx.tex_sp = 0;
    }
    val &= 0xC0000000u;
    gx.gxstat &= 0x3FFFFFFFu;
    gx.gxstat |= val;
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

/*
 * The staging buffer, and the sentinel is the whole mechanism. A decompiled-C
 * write reaches a port as `*armrec_gx_port(ADDR) = v`, and the call happens
 * before the store, so the value can only be read afterwards, at the next
 * access, which is what commits it. What cannot be known is *how many* words
 * the guest stored, because a caller may hand the pointer to a block copy
 * (G3_MultMtx33 sends nine words through MI_Copy36B). So the buffer is refilled
 * with a value no store is likely to be, and the commit takes the leading run
 * of words that are not it.
 *
 * Three cases fall out of that and all three are wanted. One store commits one
 * word. Nine stores through an advancing pointer commit nine. And a *fixed*
 * pointer stored through nine times commits one, which is a dropped
 * parameter, so feed_command() aborts when the next command arrives, rather
 * than executing a matrix multiply with one operand.
 *
 * The residual risk is a real parameter equal to the sentinel, which would end
 * a block early. That is one word in 2^32 and it fails loudly for the same
 * reason (an incomplete command traps) rather than drawing something
 * wrong.
 */
#define GX_STAGE_WORDS    32
#define GX_STAGE_SENTINEL 0x5A3C7E91u

static u32 gx_stage_buf[GX_STAGE_WORDS];

/* GXSTAT and DISP_1DOT_DEPTH are writable and are reached through a plain
 * pointer, so a guest write is noticed by comparing what is there against what
 * we last put there. */
static u32 gx_shadow_gxstat;
static u16 gx_shadow_1dot;
static int gx_shadow_valid;

static void gx_detect_reg_writes(void)
{
    u32 stat;
    u16 dot;

    if (!gx_shadow_valid) return;

    stat = *(volatile u32 *)G3D_HOST(0x04000600u);
    if (stat != gx_shadow_gxstat) {
        gxstat_write(stat);
        gx_shadow_gxstat = stat;
    }

    dot = *(volatile u16 *)G3D_HOST(0x04000610u);
    if (dot != gx_shadow_1dot) {
        gx.zero_dot_w_limit = (((u32)dot & 0x7FFF) * 0x200) + 0x1FF;
        gx_shadow_1dot = dot;
    }
}

/* Put the whole status and result block where a plain read will find it. The
 * whole block rather than one register, because G3X_GetClipMtx() takes the
 * address of the first word and copies sixteen through MI_Copy64B. */
static void gx_publish_regs(void)
{
    volatile u32 *io32 = (volatile u32 *)G3D_HOST(0x04000600u);
    volatile s16 *vec = (volatile s16 *)G3D_HOST(0x04000630u);
    static const int vecmtx_idx[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };
    int i;

    update_clip_matrix();

    gx_shadow_gxstat = gxstat_read();
    io32[0] = gx_shadow_gxstat;                                 /* 0x600 */
    io32[1] = gx.num_polygons | (gx.num_vertices << 16);        /* 0x604 */

    for (i = 0; i < 4; i++)
        *(volatile u32 *)G3D_HOST(0x04000620u + i * 4) = (u32)gx.pos_test_result[i];
    for (i = 0; i < 3; i++)
        vec[i] = gx.vec_test_result[i];
    for (i = 0; i < 16; i++)
        *(volatile u32 *)G3D_HOST(0x04000640u + i * 4) = (u32)gx.clip[i];
    for (i = 0; i < 9; i++)
        *(volatile u32 *)G3D_HOST(0x04000680u + i * 4) = (u32)gx.vec[vecmtx_idx[i]];

    gx_shadow_1dot = *(volatile u16 *)G3D_HOST(0x04000610u);
    gx_shadow_valid = 1;
}

void pc_gpu3d_refresh_regs(void)
{
    if (!gx.installed) return;
    pc_gpu3d_flush();
    gx_publish_regs();
}

void pc_gpu3d_flush(void)
{
    u32 addr;
    u32 n = 0, i;

    if (!gx.installed) return;

    gx_detect_reg_writes();

    if (gx.stage_addr == 0) return;

    addr = gx.stage_addr;
    gx.stage_addr = 0;

    while (n < GX_STAGE_WORDS && gx_stage_buf[n] != GX_STAGE_SENTINEL) n++;
    for (i = 0; i < n; i++)
        pc_gpu3d_write(addr, gx_stage_buf[i], 4);
}

/*
 * A store through a pointer that came out of armrec_gx_port(). This exists for
 * pc/src/pc_gx.c, whose two functions are our transcriptions of the SDK's
 * assembly FIFO senders: they are handed the port's address and store through
 * it many times at *fixed* offsets, so the staging buffer's leading-run rule
 * would see three words where hardware sees twelve pushes. Resolving the
 * pointer back to the port it was armed for gives each store its own push.
 *
 * Returns 0 for a destination that is neither the staging buffer nor the
 * window itself, which is what test_cp_gx drives these with.
 */
int pc_gpu3d_store_through(void *dst, u32 v)
{
    uintptr_t a = (uintptr_t)dst;

    if (gx.installed && gx.stage_addr &&
        a >= (uintptr_t)gx_stage_buf &&
        a < (uintptr_t)(gx_stage_buf + GX_STAGE_WORDS)) {
        u32 port = gx.stage_addr;
        gx.stage_addr = 0;              /* nothing staged is owed a commit */
        pc_gpu3d_write(port, v, 4);
        gx.stage_addr = port;           /* still armed for the next store */
        return 1;
    }

    if (a >= PC_GX_FIFO_BASE && a < PC_GX_CMD_END) {
        pc_gpu3d_write((u32)a, v, 4);
        return 1;
    }

    return 0;
}

/* Called by armrec_gx_port(): commit whatever is staged, then arm `addr` and
 * hand back the buffer the guest is about to store into. */
u32 *pc_gpu3d_stage(u32 addr)
{
    int i;

    pc_gpu3d_flush();
    if (!gx.installed) {
        /* Before install the window is plain memory, which is what every test
         * that does not link this file sees. */
        return (u32 *)G3D_HOST(addr);
    }
    for (i = 0; i < GX_STAGE_WORDS; i++) gx_stage_buf[i] = GX_STAGE_SENTINEL;
    gx.stage_addr = addr;
    return gx_stage_buf;
}

void pc_gpu3d_write(u32 addr, u32 val, int size)
{
    if (!gx.installed) {
        switch (size) {
        case 1: *(volatile u8  *)G3D_HOST(addr) = (u8)val;  return;
        case 2: *(volatile u16 *)G3D_HOST(addr) = (u16)val; return;
        default: *(volatile u32 *)G3D_HOST(addr) = val;     return;
        }
    }

    pc_gpu3d_flush();

    if (addr >= PC_GX_FIFO_BASE && addr < PC_GX_CMD_END) {
        if (size != 4)
            gx_fatal("a %d-byte store to the command window at %08X. The "
                     "geometry FIFO is 32 bits wide and nothing in this tree "
                     "writes it any other way, so there is no oracle for what "
                     "a console does with this", size, addr);

        if (!geometry_enabled()) return;

        if (addr < PC_GX_FIFO_END) write_to_gxfifo(val);
        else                       feed_command((addr & 0x1FC) >> 2, val);
        return;
    }

    if (addr >= 0x04000600 && addr < PC_GX_IO_END) {
        if (!geometry_enabled()) return;

        switch (addr & ~3u) {
        case 0x04000600:
            gxstat_write(val);
            return;
        case 0x04000610:
            gx.zero_dot_w_limit = ((val & 0x7FFF) * 0x200) + 0x1FF;
            return;
        default:
            /* The result windows are read-only */
            return;
        }
    }

    /* not ours: plain memory */
    switch (size) {
    case 1: *(volatile u8  *)G3D_HOST(addr) = (u8)val;  break;
    case 2: *(volatile u16 *)G3D_HOST(addr) = (u16)val; break;
    default: *(volatile u32 *)G3D_HOST(addr) = val;     break;
    }
}

uint32_t pc_gpu3d_read(u32 addr, int size, int *handled)
{
    u32 word, shift;

    *handled = 0;
    if (!gx.installed) return 0;
    if (addr < 0x04000600 || addr >= PC_GX_IO_END) return 0;

    pc_gpu3d_flush();

    if (addr >= 0x04000640 && addr < 0x04000680) {
        update_clip_matrix();
        word = (u32)gx.clip[(addr & 0x3C) >> 2];
    } else if (addr >= 0x04000680 && addr <= 0x040006A0) {
        /* the 3x3 direction matrix, in the register order the SDK's
         * G3X_GetVectorMtx() copies out */
        static const int idx[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };
        word = (u32)gx.vec[idx[(addr - 0x04000680) >> 2]];
    } else if (addr >= 0x04000620 && addr < 0x04000630) {
        word = (u32)gx.pos_test_result[(addr - 0x04000620) >> 2];
    } else if (addr >= 0x04000630 && addr < 0x04000636) {
        /* VEC_RESULT is three 16-bit registers and melonDS answers them from
         * Read16 only, so a wider or unaligned read has no oracle and is left
         * to plain memory rather than being invented here */
        if (size != 2 || (addr & 1)) return 0;
        word = (u16)gx.vec_test_result[(addr - 0x04000630) >> 1];
        *handled = 1;
        return word;
    } else if ((addr & ~3u) == 0x04000600) {
        word = gxstat_read();
    } else if ((addr & ~3u) == 0x04000604) {
        word = gx.num_polygons | (gx.num_vertices << 16);
    } else {
        return 0;
    }

    *handled = 1;
    shift = (addr & 3) * 8;
    word >>= shift;
    if (size == 1) return word & 0xFF;
    if (size == 2) return word & 0xFFFF;
    return word;
}

/*
 * Latch the rendering registers, 5.2.
 *
 * They are plain guest memory, so the masks a console applies when a register
 * is *written* have to be applied here instead. Everything below is upstream's
 * Write16/Write32 arithmetic followed by upstream's VBlank() copy, in one
 * step: DISP3DCNT keeps only bits 0-11 and 14 (12 and 13 are the underflow and
 * overflow status this engine writes itself, and no part of the rasterizer
 * reads them), ALPHA_TEST_REF is five bits and reads as zero unless the alpha
 * test is enabled, FOG_OFFSET is fifteen bits scaled to Z-buffer units, and
 * the fog density table is 7-bit entries widened to the 34-entry form the
 * interpolation indexes, with the first and last duplicated.
 *
 * The one thing this cannot reproduce is a register *read back* after a
 * partial write, since guest memory holds the raw value rather than the masked
 * one. Nothing in the SDK reads any of these.
 */
static void latch_render_regs(void)
{
    PcGxRenderRegs *r = &gx.render_regs;
    int i;

    r->DispCnt  = io_read16(REG_DISP3DCNT) & 0x4FFF;
    r->AlphaRef = (r->DispCnt & (1 << 2)) ? (io_read16(REG_ALPHA_TEST_REF) & 0x1F) : 0;

    for (i = 0; i < 32; i++) r->ToonTable[i] = io_read16(REG_TOON_TABLE + i * 2);
    for (i = 0; i < 8; i++)  r->EdgeTable[i] = io_read16(REG_EDGE_COLOR + i * 2);

    r->FogColor  = io_read32(REG_FOG_COLOR);
    r->FogOffset = (io_read16(REG_FOG_OFFSET) & 0x7FFF) * 0x200;
    r->FogShift  = (r->DispCnt >> 8) & 0xF;

    r->FogDensityTable[0] = io_read8(REG_FOG_TABLE) & 0x7F;
    for (i = 0; i < 32; i++)
        r->FogDensityTable[i + 1] = io_read8(REG_FOG_TABLE + i) & 0x7F;
    r->FogDensityTable[33] = io_read8(REG_FOG_TABLE + 31) & 0x7F;

    r->ClearAttr1 = io_read32(REG_CLEAR_COLOR);
    r->ClearAttr2 = io_read32(REG_CLEAR_DEPTH);
}

static void latch_render_regs_if_mapped(void)
{
    if (armrec_mem_ready) latch_render_regs();
}

const PcGxRenderRegs *pc_gpu3d_render_regs(void)
{
    return &gx.render_regs;
}

/*
 * The rasterizer, when it is linked in. pc/hw/pc_gpu3d_soft.c defines these
 * strongly; the weak bodies here are so that a build without it, test_gpu3d
 * is one, still links and still runs the geometry half.
 */
__attribute__((weak)) void pc_gpu3d_soft_reset(void) { }
__attribute__((weak)) void pc_gpu3d_soft_render_frame(void) { }
__attribute__((weak)) int pc_gpu3d_soft_present(void) { return 0; }

/*
 * ...and the compositor calls this one, so a build without the rasterizer
 * needs a body for it too. It is never *reached* in such a build: both callers
 * in pc/hw/pc_gpu2d.c test pc_gpu3d_soft_present() first and trap, because a
 * 3D layer that is transparent everywhere is a picture as much as any other
 * and nothing here could justify it.
 */
__attribute__((weak)) const uint32_t *pc_gpu3d_soft_line(int y)
{
    static const uint32_t none[256];
    (void)y;
    return none;
}

static unsigned long long sRenderNs;

unsigned long long pc_gpu3d_render_ns(void)
{
    return sRenderNs;
}

void pc_gpu3d_vblank(void)
{
    int rendering;

    pc_gpu3d_flush();
    if (!gx.installed) return;
    if (!geometry_enabled()) return;

    /* POWCNT1 bit 2 is the *rendering* engine, and the sort, the publish, the
     * register latch and the frame belong to it: with rendering off the
     * geometry side still swaps banks and starts a new list, but nothing is
     * handed on. Upstream splits VBlank() the same way. */
    rendering = (io_read16(REG_POWCNT1) & (1 << 2)) != 0;

    if (rendering) {
    if (gx.flush_request && gx.num_polygons) {
        /* opaque polygons first, then translucent ones, then a stable sort by
         * SortKey over the range SWAP_BUFFERS' bit 0 selects */
        u32 io = 0, it = gx.num_opaque_polygons, i, n;

        for (i = 0; i < gx.num_polygons; i++) {
            PcGxPolygon *poly = &gx.cur_polygon_ram[i];
            if (poly->Translucent) gx.render_polygon_ram[it++] = poly;
            else                   gx.render_polygon_ram[io++] = poly;
        }

        n = (gx.flush_attributes & 0x1) ? gx.num_opaque_polygons : gx.num_polygons;

        /* insertion sort: stable by construction, which the ordering rule
         * requires, equal keys keep submission order */
        for (i = 1; i < n; i++) {
            PcGxPolygon *key = gx.render_polygon_ram[i];
            u32 j = i;
            while (j > 0 && gx.render_polygon_ram[j - 1]->SortKey > key->SortKey) {
                gx.render_polygon_ram[j] = gx.render_polygon_ram[j - 1];
                j--;
            }
            gx.render_polygon_ram[j] = key;
        }
    }

    if (gx.flush_request) gx.render_num_polygons = gx.num_polygons;

    /* The registers are latched whether or not the geometry side flushed,
     * a frame with no new polygons still redraws with whatever DISP3DCNT, the
     * clear colour and the fog tables say now. Upstream latches in the same
     * place and for the same reason. */
    latch_render_regs();
    {
        /* Timed unconditionally, and this is the one span that is: the
         * pacer's picture-skip decides on the number, so it cannot be a
         * thing only a diagnostic run knows. Two clock reads a frame. */
        unsigned long long t = pc_bench_enter();

        pc_gpu3d_soft_render_frame();
        t = pc_bench_enter() - t;
        sRenderNs += t;
        if (pc_bench_on) pc_bench_add(PC_BENCH_3D, t);
    }
    }

    if (!gx.flush_request) return;

    gx.cur_ram_bank = gx.cur_ram_bank ? 0 : 1;
    gx.cur_vertex_ram = &gx.vertex_ram[gx.cur_ram_bank ? PC_GX_MAX_VERTICES : 0];
    gx.cur_polygon_ram = &gx.polygon_ram[gx.cur_ram_bank ? PC_GX_MAX_POLYGONS : 0];

    gx.num_vertices = 0;
    gx.num_polygons = 0;
    gx.num_opaque_polygons = 0;
    gx.flush_request = 0;
}

void pc_gpu3d_reset(void)
{
    int installed = gx.installed;

    memset(&gx, 0, sizeof(gx));
    gx.installed = installed;

    gx.zero_dot_w_limit = 0xFFFFFF;

    mtx_identity(gx.proj);
    mtx_identity(gx.pos);
    mtx_identity(gx.vec);
    mtx_identity(gx.tex);
    gx.clip_dirty = 1;
    update_clip_matrix();

    gx.cur_vertex_ram = &gx.vertex_ram[0];
    gx.cur_polygon_ram = &gx.polygon_ram[0];

    /*
     * The two rendering registers with a power-on value that is not zero.
     * They are plain guest memory, so "reset" has to
     * *write* them rather than initialise a shadow; the next latch would
     * read the memory back otherwise. The values are melonDS's, which is the
     * only oracle for them: a ROM image cannot record console state, GBAtek
     * calls the registers undefined at power-on, and zeros would mean the
     * first frame cleared to depth 0x1FF, which every depth test fails.
     */
    if (armrec_mem_ready) {
        *(volatile u32 *)G3D_HOST(REG_CLEAR_COLOR) = 0x3F000000u;
        *(volatile u32 *)G3D_HOST(REG_CLEAR_DEPTH) = 0x00007FFFu;
    }
    latch_render_regs_if_mapped();
    pc_gpu3d_soft_reset();
}

void pc_gpu3d_install(void)
{
    pc_gpu3d_reset();
    gx.installed = 1;
}

int pc_gpu3d_installed(void)
{
    return gx.installed;
}

PcGxPolygon **pc_gpu3d_render_polygons(uint32_t *count)
{
    if (count) *count = gx.render_num_polygons;
    return gx.render_polygon_ram;
}

uint32_t pc_gpu3d_num_polygons(void) { return gx.num_polygons; }
uint32_t pc_gpu3d_num_vertices(void) { return gx.num_vertices; }

const int32_t *pc_gpu3d_matrix(int which)
{
    switch (which) {
    case 0: return gx.proj;
    case 1: return gx.pos;
    case 2: return gx.vec;
    default: return gx.tex;
    }
}

const int32_t *pc_gpu3d_clip_matrix(void)
{
    update_clip_matrix();
    return gx.clip;
}

uint32_t pc_gpu3d_gxstat(void) { return gxstat_read(); }
