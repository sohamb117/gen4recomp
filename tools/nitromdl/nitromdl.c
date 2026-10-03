/*
 * nitromdl, glTF 2.0 to NSBMD (BMD0 + MDL0).
 *
 * v1 is one static mesh, no skinning, no TEX0. Extra map props skip
 * NNS_G3dBindMdlTex (that bind crashed on a planted file), so a texture
 * in the glTF is refused rather than written and ignored. Vertex colour
 * (COLOR_0, or magenta if the mesh has none) is emitted as GX COLOR so
 * the field can draw the mesh without a bound TEX0.
 *
 * Layout follows NNSG3dRes* in nnsys/g3d/binres/res_struct.h and the
 * GPU command packing in GBATEK. The Patricia tree for a one-entry
 * dict matches nitrobtx's 1-name case (root + one split vs all-zero).
 */

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yyjson.h"

#define GX_NOP        0x00
#define GX_COLOR      0x20
#define GX_VTX_16     0x23
#define GX_BEGIN_VTXS 0x40
#define GX_END_VTXS   0x41

#define GX_TRIANGLES  0

#define COMP_BYTE   5120
#define COMP_UBYTE  5121
#define COMP_SHORT  5122
#define COMP_USHORT 5123
#define COMP_UINT   5125
#define COMP_FLOAT  5126

#define MODE_TRIANGLES 4

#define FX16_ONE  4096
#define FX32_ONE  4096

#define MAGENTA555 0x7C1F /* R=31 G=0 B=31 */

#define MATFLAG_SCALEONE  0x0002
#define MATFLAG_ROTZERO   0x0004
#define MATFLAG_TRANSZERO 0x0008
#define MATFLAG_DIFFUSE   0x0040
#define MATFLAG_AMBIENT   0x0080
#define MATFLAG_VTXCOLOR  0x0100
#define MATFLAG_SPECULAR  0x0200
#define MATFLAG_EMISSION  0x0400
#define MATFLAG_SHININESS 0x0800

#define SRT_TRANS_ZERO 0x0001
#define SRT_ROT_ZERO   0x0002
#define SRT_SCALE_ONE  0x0004
#define SRT_IDENTITY   (SRT_TRANS_ZERO | SRT_ROT_ZERO | SRT_SCALE_ONE)

#define SBC_RET      0x01
#define SBC_NODE     0x02
#define SBC_MAT      0x04
#define SBC_SHP      0x05
#define SBC_NODEDESC 0x06
#define SBC_POSSCALE 0x0b

#define SHP_USE_COLOR 0x00000002

#define MAX_VERTS  4096
#define MAX_IDX    12288
#define MAX_NAME   16

static const char *g_in_path;

static void die(const char *msg)
{
    fprintf(stderr, "nitromdl: %s", msg);
    if (g_in_path) {
        fprintf(stderr, " (%s)", g_in_path);
    }
    fputc('\n', stderr);
    exit(1);
}

static void dief(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "nitromdl: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_in_path) {
        fprintf(stderr, " (%s)", g_in_path);
    }
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        die("out of memory");
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        die("out of memory");
    }
    return q;
}

/* ---- growable byte buffer ---- */

typedef struct {
    uint8_t *p;
    size_t n;
    size_t cap;
} Buf;

static void buf_init(Buf *b)
{
    b->p = NULL;
    b->n = 0;
    b->cap = 0;
}

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->n + extra <= b->cap) {
        return;
    }
    size_t cap = b->cap ? b->cap : 256;
    while (b->n + extra > cap) {
        cap *= 2;
    }
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

static void buf_u8(Buf *b, uint8_t v)
{
    buf_reserve(b, 1);
    b->p[b->n++] = v;
}

static void buf_u16(Buf *b, uint16_t v)
{
    buf_reserve(b, 2);
    memcpy(b->p + b->n, &v, 2);
    b->n += 2;
}

static void buf_u32(Buf *b, uint32_t v)
{
    buf_reserve(b, 4);
    memcpy(b->p + b->n, &v, 4);
    b->n += 4;
}

static void buf_bytes(Buf *b, const void *src, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->p + b->n, src, n);
    b->n += n;
}

static void buf_pad4(Buf *b)
{
    while (b->n & 3) {
        buf_u8(b, 0);
    }
}

/* ---- one-entry G3D resource dict ---- */

static int first_one_bit(const char name[MAX_NAME])
{
    int i;
    for (i = 127; i >= 0; i--) {
        unsigned byte = (unsigned char)name[i / 8];
        if (byte & (1u << (i % 8))) {
            return i;
        }
    }
    return 0;
}

static void write_dict1(Buf *b, const char *name, uint32_t data)
{
    /* 8-byte header + 2 tree nodes + (sizeUnit, ofsName) + u32 data + name.
     * sizeDictBlk = 0x28. ofsEntry = 0x10. Tree: root + one split vs 0. */
    char n[MAX_NAME];
    memset(n, 0, sizeof n);
    if (name) {
        size_t len = strlen(name);
        if (len > MAX_NAME) {
            len = MAX_NAME;
        }
        memcpy(n, name, len);
    }

    buf_u8(b, 0);
    buf_u8(b, 1);
    buf_u16(b, 0x28);
    buf_u16(b, 8);
    buf_u16(b, 0x10);

    buf_u8(b, 127);
    buf_u8(b, 1);
    buf_u8(b, 0);
    buf_u8(b, 0);

    buf_u8(b, (uint8_t)first_one_bit(n));
    buf_u8(b, 0);
    buf_u8(b, 1);
    buf_u8(b, 0);

    buf_u16(b, 4);
    buf_u16(b, 8);
    buf_u32(b, data);
    buf_bytes(b, n, MAX_NAME);
}

static void write_dict0(Buf *b)
{
    /* Empty dict: root only + sizeUnit/ofsName. size = 16. */
    buf_u8(b, 0);
    buf_u8(b, 0);
    buf_u16(b, 16);
    buf_u16(b, 8);
    buf_u16(b, 12);
    buf_u8(b, 127);
    buf_u8(b, 0);
    buf_u8(b, 0);
    buf_u8(b, 0);
    buf_u16(b, 4);
    buf_u16(b, 4);
}

/* ---- display list ---- */

typedef struct {
    uint8_t ops[4];
    int nops;
    uint32_t params[32];
    int nparams;
    Buf *out;
} DL;

static void dl_flush(DL *dl)
{
    uint32_t packed;
    int i;
    if (dl->nops == 0) {
        return;
    }
    while (dl->nops < 4) {
        dl->ops[dl->nops++] = GX_NOP;
    }
    packed = (uint32_t)dl->ops[0]
        | ((uint32_t)dl->ops[1] << 8)
        | ((uint32_t)dl->ops[2] << 16)
        | ((uint32_t)dl->ops[3] << 24);
    buf_u32(dl->out, packed);
    for (i = 0; i < dl->nparams; i++) {
        buf_u32(dl->out, dl->params[i]);
    }
    dl->nops = 0;
    dl->nparams = 0;
}

static void dl_cmd(DL *dl, uint8_t op, const uint32_t *p, int np)
{
    int i;
    if (dl->nops == 4) {
        dl_flush(dl);
    }
    dl->ops[dl->nops++] = op;
    for (i = 0; i < np; i++) {
        if (dl->nparams >= 32) {
            die("display list packet overflow");
        }
        dl->params[dl->nparams++] = p[i];
    }
}

static int16_t to_fx16(float f)
{
    long v = lroundf(f * (float)FX16_ONE);
    if (v > 32767) {
        v = 32767;
    }
    if (v < -32768) {
        v = -32768;
    }
    return (int16_t)v;
}

static uint32_t pack_xy(float x, float y)
{
    uint16_t fx = (uint16_t)to_fx16(x);
    uint16_t fy = (uint16_t)to_fx16(y);
    return (uint32_t)fx | ((uint32_t)fy << 16);
}

static uint32_t pack_z(float z)
{
    return (uint16_t)to_fx16(z);
}

/* ---- glTF ---- */

typedef struct {
    uint8_t *data;
    size_t size;
    bool owned;
} Blob;

typedef struct {
    float pos[MAX_VERTS][3];
    uint16_t color[MAX_VERTS]; /* RGB555; 0 = unset */
    int nverts;
    uint16_t idx[MAX_IDX];
    int nidx;
    bool have_color;
    char name[MAX_NAME + 1];
} Mesh;

static char *dir_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t n;
    char *d;
    if (!slash) {
        d = xmalloc(2);
        d[0] = '.';
        d[1] = 0;
        return d;
    }
    n = (size_t)(slash - path);
    d = xmalloc(n + 1);
    memcpy(d, path, n);
    d[n] = 0;
    return d;
}

static Blob read_file(const char *path)
{
    Blob b = { 0 };
    FILE *f = fopen(path, "rb");
    long sz;
    if (!f) {
        dief("cannot open %s: %s", path, strerror(errno));
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        dief("cannot size %s", path);
    }
    rewind(f);
    b.size = (size_t)sz;
    b.data = xmalloc(b.size + 1);
    b.data[b.size] = 0;
    b.owned = true;
    if (fread(b.data, 1, b.size, f) != b.size) {
        fclose(f);
        dief("short read of %s", path);
    }
    fclose(f);
    return b;
}

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static Blob b64_decode(const char *s)
{
    Blob b = { 0 };
    size_t in = strlen(s);
    size_t i, o = 0;
    b.data = xmalloc(in + 4);
    b.owned = true;
    i = 0;
    while (i < in) {
        int v[4];
        int n = 0;
        while (i < in && n < 4) {
            unsigned char c = (unsigned char)s[i++];
            if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
                if (c == '=') {
                    v[n++] = -2;
                }
                continue;
            }
            v[n] = b64_val(c);
            if (v[n] < 0) {
                die("invalid base64 in data URI");
            }
            n++;
        }
        if (n == 0) {
            break;
        }
        if (n < 2) {
            die("truncated base64 in data URI");
        }
        b.data[o++] = (uint8_t)((v[0] << 2) | (v[1] >> 4));
        if (n > 2 && v[2] >= 0) {
            b.data[o++] = (uint8_t)((v[1] << 4) | (v[2] >> 2));
        }
        if (n > 3 && v[3] >= 0) {
            b.data[o++] = (uint8_t)((v[2] << 6) | v[3]);
        }
    }
    b.size = o;
    return b;
}

static Blob load_uri(const char *dir, const char *uri)
{
    const char *comma;
    char *path;
    Blob b;
    if (strncmp(uri, "data:", 5) == 0) {
        comma = strchr(uri, ',');
        if (!comma) {
            die("data URI missing comma");
        }
        if (!strstr(uri, ";base64,")) {
            die("data URI must be base64");
        }
        return b64_decode(comma + 1);
    }
    if (strchr(uri, ':')) {
        dief("unsupported buffer URI '%s'", uri);
    }
    path = xmalloc(strlen(dir) + 1 + strlen(uri) + 1);
    sprintf(path, "%s/%s", dir, uri);
    b = read_file(path);
    free(path);
    return b;
}

static yyjson_val *req_obj(yyjson_val *o, const char *k)
{
    yyjson_val *v = yyjson_obj_get(o, k);
    if (!v) {
        dief("glTF missing '%s'", k);
    }
    return v;
}

static int json_int(yyjson_val *v, int def)
{
    if (!v) {
        return def;
    }
    if (yyjson_is_int(v)) {
        return (int)yyjson_get_int(v);
    }
    if (yyjson_is_uint(v)) {
        return (int)yyjson_get_uint(v);
    }
    if (yyjson_is_real(v)) {
        return (int)yyjson_get_real(v);
    }
    return def;
}

static float json_real(yyjson_val *v, float def)
{
    if (!v) {
        return def;
    }
    if (yyjson_is_real(v)) {
        return (float)yyjson_get_real(v);
    }
    if (yyjson_is_int(v)) {
        return (float)yyjson_get_int(v);
    }
    if (yyjson_is_uint(v)) {
        return (float)yyjson_get_uint(v);
    }
    return def;
}

static int comp_size(int ctype)
{
    switch (ctype) {
    case COMP_BYTE:
    case COMP_UBYTE:
        return 1;
    case COMP_SHORT:
    case COMP_USHORT:
        return 2;
    case COMP_UINT:
    case COMP_FLOAT:
        return 4;
    default:
        dief("unsupported accessor componentType %d", ctype);
        return 0;
    }
}

static int type_ncomp(const char *t)
{
    if (!t) {
        die("accessor missing type");
    }
    if (strcmp(t, "SCALAR") == 0) {
        return 1;
    }
    if (strcmp(t, "VEC2") == 0) {
        return 2;
    }
    if (strcmp(t, "VEC3") == 0) {
        return 3;
    }
    if (strcmp(t, "VEC4") == 0) {
        return 4;
    }
    dief("unsupported accessor type '%s'", t);
    return 0;
}

typedef struct {
    yyjson_val *root;
    yyjson_val *accessors;
    yyjson_val *views;
    Blob *buffers;
    int nbuf;
} Gltf;

static const uint8_t *acc_ptr(const Gltf *g, yyjson_val *acc, int *count,
                              int *ncomp, int *ctype, int *stride)
{
    int view_i, off = 0, view_off = 0, view_len;
    yyjson_val *view;
    int buf_i;

    *ctype = json_int(yyjson_obj_get(acc, "componentType"), -1);
    *count = json_int(yyjson_obj_get(acc, "count"), -1);
    *ncomp = type_ncomp(yyjson_get_str(yyjson_obj_get(acc, "type")));
    if (*count < 0 || *ctype < 0) {
        die("accessor missing count or componentType");
    }
    view_i = json_int(yyjson_obj_get(acc, "bufferView"), -1);
    if (view_i < 0) {
        die("accessor has no bufferView");
    }
    off = json_int(yyjson_obj_get(acc, "byteOffset"), 0);
    view = yyjson_arr_get(g->views, (size_t)view_i);
    if (!view) {
        dief("bufferView %d missing", view_i);
    }
    buf_i = json_int(yyjson_obj_get(view, "buffer"), 0);
    view_off = json_int(yyjson_obj_get(view, "byteOffset"), 0);
    view_len = json_int(yyjson_obj_get(view, "byteLength"), 0);
    *stride = json_int(yyjson_obj_get(view, "byteStride"), 0);
    if (*stride == 0) {
        *stride = comp_size(*ctype) * *ncomp;
    }
    if (buf_i < 0 || buf_i >= g->nbuf) {
        dief("buffer %d missing", buf_i);
    }
    if ((size_t)(view_off + off + *count * *stride) > g->buffers[buf_i].size
        && view_len > 0
        && (size_t)(view_off + view_len) > g->buffers[buf_i].size) {
        die("accessor walks off the end of its buffer");
    }
    return g->buffers[buf_i].data + view_off + off;
}

static float read_comp(const uint8_t *p, int ctype, int i)
{
    const uint8_t *s = p + (size_t)i * (size_t)comp_size(ctype);
    switch (ctype) {
    case COMP_BYTE:
        return (float)(int8_t)s[0];
    case COMP_UBYTE:
        return (float)s[0];
    case COMP_SHORT: {
        int16_t v;
        memcpy(&v, s, 2);
        return (float)v;
    }
    case COMP_USHORT: {
        uint16_t v;
        memcpy(&v, s, 2);
        return (float)v;
    }
    case COMP_UINT: {
        uint32_t v;
        memcpy(&v, s, 4);
        return (float)v;
    }
    case COMP_FLOAT: {
        float v;
        memcpy(&v, s, 4);
        return v;
    }
    default:
        return 0;
    }
}

static uint16_t rgb_to_555(float r, float g, float b)
{
    int ir = (int)lroundf(r * 31.0f);
    int ig = (int)lroundf(g * 31.0f);
    int ib = (int)lroundf(b * 31.0f);
    if (ir < 0) {
        ir = 0;
    }
    if (ir > 31) {
        ir = 31;
    }
    if (ig < 0) {
        ig = 0;
    }
    if (ig > 31) {
        ig = 31;
    }
    if (ib < 0) {
        ib = 0;
    }
    if (ib > 31) {
        ib = 31;
    }
    return (uint16_t)(ir | (ig << 5) | (ib << 10));
}

static void load_mesh(const Gltf *g, Mesh *m)
{
    yyjson_val *meshes, *mesh, *prims, *prim, *attrs, *accs;
    yyjson_val *pos_a, *col_a, *idx_a;
    int pos_i, col_i, idx_i, mode;
    int count, ncomp, ctype, stride, i, c;
    const uint8_t *ptr;
    const char *mesh_name;

    memset(m, 0, sizeof *m);
    meshes = req_obj(g->root, "meshes");
    if (!yyjson_is_arr(meshes) || yyjson_arr_size(meshes) == 0) {
        die("glTF has no meshes");
    }
    if (yyjson_arr_size(meshes) != 1) {
        die("v1 encodes one mesh (no extras, no children)");
    }
    mesh = yyjson_arr_get(meshes, 0);
    mesh_name = yyjson_get_str(yyjson_obj_get(mesh, "name"));
    if (!mesh_name || !mesh_name[0]) {
        mesh_name = "model";
    }
    strncpy(m->name, mesh_name, MAX_NAME);
    m->name[MAX_NAME] = 0;

    prims = req_obj(mesh, "primitives");
    if (!yyjson_is_arr(prims) || yyjson_arr_size(prims) != 1) {
        die("v1 encodes one primitive");
    }
    prim = yyjson_arr_get(prims, 0);
    if (yyjson_obj_get(prim, "targets")) {
        die("morph targets are not supported");
    }
    mode = json_int(yyjson_obj_get(prim, "mode"), MODE_TRIANGLES);
    if (mode != MODE_TRIANGLES) {
        dief("v1 encodes TRIANGLES (mode 4), not mode %d", mode);
    }
    attrs = req_obj(prim, "attributes");
    if (yyjson_obj_get(attrs, "JOINTS_0") || yyjson_obj_get(attrs, "WEIGHTS_0")) {
        die("skinning is not supported");
    }
    pos_i = json_int(yyjson_obj_get(attrs, "POSITION"), -1);
    if (pos_i < 0) {
        die("primitive has no POSITION");
    }
    accs = g->accessors;
    pos_a = yyjson_arr_get(accs, (size_t)pos_i);
    if (!pos_a) {
        die("POSITION accessor missing");
    }
    ptr = acc_ptr(g, pos_a, &count, &ncomp, &ctype, &stride);
    if (ncomp != 3) {
        die("POSITION must be VEC3");
    }
    if (count > MAX_VERTS) {
        dief("too many vertices (%d, max %d)", count, MAX_VERTS);
    }
    m->nverts = count;
    for (i = 0; i < count; i++) {
        const uint8_t *row = ptr + (size_t)i * (size_t)stride;
        for (c = 0; c < 3; c++) {
            m->pos[i][c] = read_comp(row, ctype, c);
        }
    }

    col_i = json_int(yyjson_obj_get(attrs, "COLOR_0"), -1);
    if (col_i >= 0) {
        col_a = yyjson_arr_get(accs, (size_t)col_i);
        ptr = acc_ptr(g, col_a, &count, &ncomp, &ctype, &stride);
        if (count != m->nverts) {
            die("COLOR_0 count does not match POSITION");
        }
        if (ncomp < 3) {
            die("COLOR_0 must be VEC3 or VEC4");
        }
        m->have_color = true;
        for (i = 0; i < count; i++) {
            const uint8_t *row = ptr + (size_t)i * (size_t)stride;
            float r = read_comp(row, ctype, 0);
            float gch = read_comp(row, ctype, 1);
            float b = read_comp(row, ctype, 2);
            if (ctype == COMP_UBYTE) {
                r /= 255.0f;
                gch /= 255.0f;
                b /= 255.0f;
            } else if (ctype == COMP_USHORT) {
                r /= 65535.0f;
                gch /= 65535.0f;
                b /= 65535.0f;
            }
            m->color[i] = rgb_to_555(r, gch, b);
        }
    }

    idx_a = yyjson_obj_get(prim, "indices");
    if (idx_a) {
        idx_i = json_int(idx_a, -1);
        idx_a = yyjson_arr_get(accs, (size_t)idx_i);
        if (!idx_a) {
            die("indices accessor missing");
        }
        ptr = acc_ptr(g, idx_a, &count, &ncomp, &ctype, &stride);
        if (ncomp != 1) {
            die("indices must be SCALAR");
        }
        if (count > MAX_IDX) {
            dief("too many indices (%d, max %d)", count, MAX_IDX);
        }
        if (count % 3 != 0) {
            die("index count is not a multiple of 3");
        }
        m->nidx = count;
        for (i = 0; i < count; i++) {
            const uint8_t *row = ptr + (size_t)i * (size_t)stride;
            unsigned v = (unsigned)read_comp(row, ctype, 0);
            if (v >= (unsigned)m->nverts) {
                dief("index %u is past the vertex count", v);
            }
            m->idx[i] = (uint16_t)v;
        }
    } else {
        if (m->nverts % 3 != 0) {
            die("non-indexed mesh vertex count is not a multiple of 3");
        }
        m->nidx = m->nverts;
        for (i = 0; i < m->nverts; i++) {
            m->idx[i] = (uint16_t)i;
        }
    }
}

static void quat_rotate(const float q[4], const float v[3], float out[3])
{
    /* q = (x,y,z,w). v' = v + 2*w*(qxyz×v) + 2*(qxyz×(qxyz×v)) */
    float ux = q[0], uy = q[1], uz = q[2], w = q[3];
    float cx = uy * v[2] - uz * v[1];
    float cy = uz * v[0] - ux * v[2];
    float cz = ux * v[1] - uy * v[0];
    out[0] = v[0] + 2.0f * w * cx + 2.0f * (uy * cz - uz * cy);
    out[1] = v[1] + 2.0f * w * cy + 2.0f * (uz * cx - ux * cz);
    out[2] = v[2] + 2.0f * w * cz + 2.0f * (ux * cy - uy * cx);
}

static void bake_node(const Gltf *g, Mesh *m)
{
    yyjson_val *nodes = yyjson_obj_get(g->root, "nodes");
    yyjson_val *node = NULL;
    yyjson_val *tr, *sc, *rot, *mx;
    int i, n;
    float T[3] = { 0, 0, 0 };
    float S[3] = { 1, 1, 1 };
    float Q[4] = { 0, 0, 0, 1 };
    int have_q = 0;

    if (!nodes || !yyjson_is_arr(nodes)) {
        return;
    }
    n = (int)yyjson_arr_size(nodes);
    for (i = 0; i < n; i++) {
        yyjson_val *nd = yyjson_arr_get(nodes, (size_t)i);
        yyjson_val *mesh_i = yyjson_obj_get(nd, "mesh");
        if (mesh_i && json_int(mesh_i, -1) == 0) {
            node = nd;
            break;
        }
    }
    if (!node) {
        return;
    }
    mx = yyjson_obj_get(node, "matrix");
    if (mx && yyjson_is_arr(mx) && yyjson_arr_size(mx) == 16) {
        float M[16];
        int r;
        for (r = 0; r < 16; r++) {
            M[r] = json_real(yyjson_arr_get(mx, (size_t)r), r % 5 == 0 ? 1.0f : 0.0f);
        }
        for (i = 0; i < m->nverts; i++) {
            float x = m->pos[i][0], y = m->pos[i][1], z = m->pos[i][2];
            m->pos[i][0] = M[0] * x + M[4] * y + M[8] * z + M[12];
            m->pos[i][1] = M[1] * x + M[5] * y + M[9] * z + M[13];
            m->pos[i][2] = M[2] * x + M[6] * y + M[10] * z + M[14];
        }
        return;
    }
    tr = yyjson_obj_get(node, "translation");
    sc = yyjson_obj_get(node, "scale");
    rot = yyjson_obj_get(node, "rotation");
    if (tr && yyjson_is_arr(tr) && yyjson_arr_size(tr) == 3) {
        T[0] = json_real(yyjson_arr_get(tr, 0), 0);
        T[1] = json_real(yyjson_arr_get(tr, 1), 0);
        T[2] = json_real(yyjson_arr_get(tr, 2), 0);
    }
    if (sc && yyjson_is_arr(sc) && yyjson_arr_size(sc) == 3) {
        S[0] = json_real(yyjson_arr_get(sc, 0), 1);
        S[1] = json_real(yyjson_arr_get(sc, 1), 1);
        S[2] = json_real(yyjson_arr_get(sc, 2), 1);
    }
    if (rot && yyjson_is_arr(rot) && yyjson_arr_size(rot) == 4) {
        Q[0] = json_real(yyjson_arr_get(rot, 0), 0);
        Q[1] = json_real(yyjson_arr_get(rot, 1), 0);
        Q[2] = json_real(yyjson_arr_get(rot, 2), 0);
        Q[3] = json_real(yyjson_arr_get(rot, 3), 1);
        have_q = 1;
    }
    if (T[0] == 0 && T[1] == 0 && T[2] == 0
        && S[0] == 1 && S[1] == 1 && S[2] == 1 && !have_q) {
        return;
    }
    for (i = 0; i < m->nverts; i++) {
        float v[3] = { m->pos[i][0] * S[0], m->pos[i][1] * S[1], m->pos[i][2] * S[2] };
        float r[3];
        if (have_q) {
            quat_rotate(Q, v, r);
        } else {
            r[0] = v[0];
            r[1] = v[1];
            r[2] = v[2];
        }
        m->pos[i][0] = r[0] + T[0];
        m->pos[i][1] = r[1] + T[1];
        m->pos[i][2] = r[2] + T[2];
    }
}

static void parse_gltf(const char *path, Mesh *mesh)
{
    Blob file = read_file(path);
    const char *dir = dir_of(path);
    const char *json;
    size_t json_len;
    Blob bin_chunk = { 0 };
    yyjson_doc *doc;
    yyjson_read_err err;
    Gltf g = { 0 };
    yyjson_val *bufs, *buf, *asset;
    const char *ver;
    int i, nbuf;
    bool is_glb;

    is_glb = file.size >= 12 && memcmp(file.data, "glTF", 4) == 0;
    if (is_glb) {
        uint32_t version, length, off;
        memcpy(&version, file.data + 4, 4);
        memcpy(&length, file.data + 8, 4);
        if (version != 2) {
            dief("GLB version %u is not 2", version);
        }
        if (length > file.size) {
            die("GLB length walks off the file");
        }
        off = 12;
        json = NULL;
        json_len = 0;
        while (off + 8 <= length) {
            uint32_t clen, ctype;
            memcpy(&clen, file.data + off, 4);
            memcpy(&ctype, file.data + off + 4, 4);
            off += 8;
            if (off + clen > length) {
                die("GLB chunk walks off the file");
            }
            if (ctype == 0x4E4F534A) { /* JSON */
                json = (const char *)(file.data + off);
                json_len = clen;
            } else if (ctype == 0x004E4942) { /* BIN */
                bin_chunk.data = file.data + off;
                bin_chunk.size = clen;
                bin_chunk.owned = false;
            }
            off += (clen + 3u) & ~3u;
        }
        if (!json) {
            die("GLB has no JSON chunk");
        }
    } else {
        json = (const char *)file.data;
        json_len = file.size;
    }

    doc = yyjson_read_opts((char *)json, json_len, 0, NULL, &err);
    if (!doc) {
        dief("glTF JSON: %s at %zu", err.msg, err.pos);
    }
    g.root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(g.root)) {
        die("glTF root is not an object");
    }
    asset = yyjson_obj_get(g.root, "asset");
    ver = asset ? yyjson_get_str(yyjson_obj_get(asset, "version")) : NULL;
    if (!ver || (ver[0] != '2' && strcmp(ver, "2.0") != 0 && strncmp(ver, "2.", 2) != 0)) {
        dief("need glTF 2.0, not '%s'", ver ? ver : "(missing)");
    }
    if (yyjson_obj_get(g.root, "skins")) {
        die("skinning is not supported");
    }
    {
        yyjson_val *tex = yyjson_obj_get(g.root, "textures");
        yyjson_val *img = yyjson_obj_get(g.root, "images");
        if ((tex && yyjson_is_arr(tex) && yyjson_arr_size(tex) > 0)
            || (img && yyjson_is_arr(img) && yyjson_arr_size(img) > 0)) {
            die("TEX0 is not written yet; extra props do not bind a texture. "
                "Drop the image or use a prebuilt .nsbmd");
        }
    }
    g.accessors = req_obj(g.root, "accessors");
    g.views = req_obj(g.root, "bufferViews");
    bufs = yyjson_obj_get(g.root, "buffers");
    nbuf = bufs && yyjson_is_arr(bufs) ? (int)yyjson_arr_size(bufs) : 0;
    if (nbuf < 1) {
        die("glTF has no buffers");
    }
    g.nbuf = nbuf;
    g.buffers = xmalloc((size_t)nbuf * sizeof *g.buffers);
    memset(g.buffers, 0, (size_t)nbuf * sizeof *g.buffers);
    for (i = 0; i < nbuf; i++) {
        const char *uri;
        buf = yyjson_arr_get(bufs, (size_t)i);
        uri = yyjson_get_str(yyjson_obj_get(buf, "uri"));
        if (uri) {
            g.buffers[i] = load_uri(dir, uri);
        } else if (is_glb && i == 0 && bin_chunk.data) {
            g.buffers[i] = bin_chunk;
        } else {
            dief("buffer %d has no uri and no GLB BIN", i);
        }
    }

    load_mesh(&g, mesh);
    bake_node(&g, mesh);

    for (i = 0; i < nbuf; i++) {
        if (g.buffers[i].owned) {
            free(g.buffers[i].data);
        }
    }
    free(g.buffers);
    yyjson_doc_free(doc);
    free((void *)dir);
    if (file.owned) {
        free(file.data);
    }
}

/* ---- NSBMD writer ---- */

static uint16_t mesh_color(const Mesh *m)
{
    int i;
    if (!m->have_color) {
        return MAGENTA555;
    }
    for (i = 1; i < m->nverts; i++) {
        if (m->color[i] != m->color[0]) {
            /* Per-vertex colour is encoded; the first is still the material. */
            return m->color[0];
        }
    }
    return m->color[0];
}

static void emit_dl(Buf *out, const Mesh *m, float inv_scale)
{
    DL dl;
    uint32_t p[2];
    int i;
    uint16_t last = 0xFFFF;

    memset(&dl, 0, sizeof dl);
    dl.out = out;

    p[0] = GX_TRIANGLES;
    dl_cmd(&dl, GX_BEGIN_VTXS, p, 1);

    for (i = 0; i < m->nidx; i++) {
        int vi = m->idx[i];
        uint16_t col = m->have_color ? m->color[vi] : MAGENTA555;
        float x = m->pos[vi][0] * inv_scale;
        float y = m->pos[vi][1] * inv_scale;
        float z = m->pos[vi][2] * inv_scale;
        if (col != last) {
            p[0] = col;
            dl_cmd(&dl, GX_COLOR, p, 1);
            last = col;
        }
        p[0] = pack_xy(x, y);
        p[1] = pack_z(z);
        dl_cmd(&dl, GX_VTX_16, p, 2);
    }
    dl_cmd(&dl, GX_END_VTXS, NULL, 0);
    dl_flush(&dl);
}

static void write_nsbmd(const char *out_path, const Mesh *m)
{
    Buf mdl = { 0 };
    Buf file = { 0 };
    Buf dl = { 0 };
    float minv[3], maxv[3], maxabs, pos_scale, inv_scale;
    int i, c, ntri;
    uint16_t col555;
    uint32_t diffamb, specemi, polyattr;
    uint16_t matflag;
    uint32_t mdl_size, model_off, model_size;
    uint32_t ofs_sbc, ofs_mat, ofs_shp, ofs_evp;
    uint32_t sbc_sz, mat_sz, shp_sz, dl_sz;
    FILE *out;
    bool use_posscale;

    if (m->nidx < 3) {
        die("mesh has no triangles");
    }
    ntri = m->nidx / 3;

    minv[0] = maxv[0] = m->pos[0][0];
    minv[1] = maxv[1] = m->pos[0][1];
    minv[2] = maxv[2] = m->pos[0][2];
    for (i = 0; i < m->nverts; i++) {
        for (c = 0; c < 3; c++) {
            if (m->pos[i][c] < minv[c]) {
                minv[c] = m->pos[i][c];
            }
            if (m->pos[i][c] > maxv[c]) {
                maxv[c] = m->pos[i][c];
            }
        }
    }
    maxabs = 0;
    for (c = 0; c < 3; c++) {
        float a = fabsf(minv[c]);
        float b = fabsf(maxv[c]);
        if (a > maxabs) {
            maxabs = a;
        }
        if (b > maxabs) {
            maxabs = b;
        }
    }
    /* VTX_16 is 1.3.12 (±8). Scale the stored verts down if needed. */
    pos_scale = 1.0f;
    while (maxabs / pos_scale >= 7.9f) {
        pos_scale *= 2.0f;
        if (pos_scale > 1024.0f) {
            die("mesh is larger than nitromdl will encode (±8 after scale)");
        }
    }
    inv_scale = 1.0f / pos_scale;
    /* Official props always emit POSSCALE; dummy-box uses posScale 2
     * and boxPosScale 4. Match that so GFXBoxTest and the SBC path
     * see the same skeleton the field already draws. */
    use_posscale = true;
    if (pos_scale < 2.0f) {
        pos_scale = 2.0f;
        inv_scale = 0.5f;
    }

    buf_init(&dl);
    emit_dl(&dl, m, inv_scale);
    buf_pad4(&dl);
    dl_sz = (uint32_t)dl.n;

    /* SBC: nodedesc, node, optional POSSCALE up, mat, shp, optional down, RET */
    sbc_sz = 4 + 3 + 2 + 2 + 1; /* 12 */
    if (use_posscale) {
        sbc_sz += 2;
    }
    sbc_sz = (sbc_sz + 3u) & ~3u;

    /* mat: 4-byte header + mat dict (0x28) + two empty pairing dicts (16+16)
     * + NNSG3dResMatData (0x2c). Pairing dicts sit at 4+0x28=0x2c and 0x3c. */
    mat_sz = 4 + 0x28 + 16 + 16 + 0x2c; /* 0x80 */
    /* shp: dict 0x28 + ResShpData 0x10 + DL */
    shp_sz = 0x28 + 0x10 + dl_sz;

    /* model: 20-byte header + 44-byte info + nodeInfo (dict 0x28 + 4-byte node)
     * + SBC + mat + shp. ofsEvpMtx == size (no envelope). */
    ofs_sbc = 20 + 44 + 0x28 + 4; /* 0x70 */
    ofs_mat = ofs_sbc + sbc_sz;
    ofs_shp = ofs_mat + mat_sz;
    model_size = ofs_shp + shp_sz;
    ofs_evp = model_size;
    model_off = 8 + 0x28; /* MDL0 header + model dict */
    mdl_size = model_off + model_size;

    buf_init(&mdl);
    buf_bytes(&mdl, "MDL0", 4);
    buf_u32(&mdl, mdl_size);
    write_dict1(&mdl, m->name[0] ? m->name : "model", model_off);

    /* NNSG3dResMdl */
    buf_u32(&mdl, model_size);
    buf_u32(&mdl, ofs_sbc);
    buf_u32(&mdl, ofs_mat);
    buf_u32(&mdl, ofs_shp);
    buf_u32(&mdl, ofs_evp);

    /* NNSG3dResMdlInfo, 44 bytes */
    buf_u8(&mdl, 0); /* sbcType NORMAL */
    buf_u8(&mdl, 0); /* scaling STANDARD */
    buf_u8(&mdl, 0); /* texMtx MAYA */
    buf_u8(&mdl, 1); /* numNode */
    buf_u8(&mdl, 1); /* numMat */
    buf_u8(&mdl, 1); /* numShp */
    buf_u8(&mdl, 0); /* firstUnusedMtxStackID: dummy-box uses 0 */
    buf_u8(&mdl, 0);
    buf_u32(&mdl, (uint32_t)lroundf(pos_scale * (float)FX32_ONE));
    buf_u32(&mdl, (uint32_t)lroundf(inv_scale * (float)FX32_ONE));
    buf_u16(&mdl, (uint16_t)m->nverts);
    buf_u16(&mdl, (uint16_t)ntri);
    buf_u16(&mdl, (uint16_t)ntri);
    buf_u16(&mdl, 0);
    buf_u16(&mdl, (uint16_t)to_fx16(minv[0]));
    buf_u16(&mdl, (uint16_t)to_fx16(minv[1]));
    buf_u16(&mdl, (uint16_t)to_fx16(minv[2]));
    buf_u16(&mdl, (uint16_t)to_fx16(maxv[0] - minv[0]));
    buf_u16(&mdl, (uint16_t)to_fx16(maxv[1] - minv[1]));
    buf_u16(&mdl, (uint16_t)to_fx16(maxv[2] - minv[2]));
    {
        float box_scale = pos_scale * 2.0f;
        buf_u32(&mdl, (uint32_t)lroundf(box_scale * (float)FX32_ONE));
        buf_u32(&mdl, (uint32_t)lroundf((1.0f / box_scale) * (float)FX32_ONE));
    }

    /* nodeInfo: dict then identity node (flag + _00 only) */
    write_dict1(&mdl, "node", 0x28);
    buf_u16(&mdl, SRT_IDENTITY);
    buf_u16(&mdl, 0);

    /* SBC */
    {
        size_t sbc_at = mdl.n;
        buf_u8(&mdl, SBC_NODEDESC);
        buf_u8(&mdl, 0);
        buf_u8(&mdl, 0);
        buf_u8(&mdl, 0);
        buf_u8(&mdl, SBC_NODE);
        buf_u8(&mdl, 0);
        buf_u8(&mdl, 1);
        if (use_posscale) {
            buf_u8(&mdl, SBC_POSSCALE);
        }
        buf_u8(&mdl, SBC_MAT);
        buf_u8(&mdl, 0);
        buf_u8(&mdl, SBC_SHP);
        buf_u8(&mdl, 0);
        if (use_posscale) {
            buf_u8(&mdl, (uint8_t)(SBC_POSSCALE | 0x20));
        }
        buf_u8(&mdl, SBC_RET);
        while (mdl.n - sbc_at < sbc_sz) {
            buf_u8(&mdl, 0);
        }
    }

    /* Material list */
    buf_u16(&mdl, 0x2c); /* ofsDictTexToMatList: after the 4-byte hdr + mat dict */
    buf_u16(&mdl, 0x3c); /* ofsDictPlttToMatList */
    write_dict1(&mdl, "mat", 0x4c); /* mat data at 4+0x28+16+16 = 0x4c */
    write_dict0(&mdl);
    write_dict0(&mdl);

    col555 = mesh_color(m);
    /* DIF_AMB: bits 0-14 diffuse, 15 vtxcolor, 16-30 ambient.
     * SPE_EMI: bits 0-14 specular, 16-30 emission. */
    diffamb = (uint32_t)col555 | (1u << 15);
    specemi = ((uint32_t)col555 << 16);
    /* POLYGON_ATTR: light 0 + alpha 31, same bits as dummy-box.
     * COLOR in the DL still sets the vertex; area light overwrites
     * the material slots but not the submitted verts. */
    polyattr = 0x001f0081;
    matflag = (uint16_t)(MATFLAG_SCALEONE | MATFLAG_ROTZERO | MATFLAG_TRANSZERO
        | MATFLAG_DIFFUSE | MATFLAG_AMBIENT | MATFLAG_VTXCOLOR
        | MATFLAG_SPECULAR | MATFLAG_EMISSION | MATFLAG_SHININESS);

    buf_u16(&mdl, 0);      /* itemTag */
    buf_u16(&mdl, 0x2c);   /* size */
    buf_u32(&mdl, diffamb);
    buf_u32(&mdl, specemi);
    buf_u32(&mdl, polyattr);
    buf_u32(&mdl, 0x3ff8ffff); /* polyAttrMask: allow the usual bits */
    buf_u32(&mdl, 0);          /* texImageParam */
    buf_u32(&mdl, 0xffffffff);
    buf_u16(&mdl, 0);          /* texPlttBase */
    buf_u16(&mdl, matflag);
    buf_u16(&mdl, 0);
    buf_u16(&mdl, 0);
    buf_u32(&mdl, FX32_ONE);
    buf_u32(&mdl, FX32_ONE);

    /* Shape list */
    write_dict1(&mdl, "shp", 0x28);
    buf_u16(&mdl, 0);
    buf_u16(&mdl, 0x10);
    buf_u32(&mdl, SHP_USE_COLOR);
    buf_u32(&mdl, 0x10);
    buf_u32(&mdl, dl_sz);
    buf_bytes(&mdl, dl.p, dl.n);

    if (mdl.n != mdl_size) {
        dief("internal size mismatch: wrote %zu, planned %u", mdl.n, mdl_size);
    }

    /* BMD0, one block (MDL0). Version 2 is what NNS_G3dGetMdlSet asserts. */
    buf_init(&file);
    buf_bytes(&file, "BMD0", 4);
    buf_u16(&file, 0xFEFF);
    buf_u16(&file, 2);
    buf_u32(&file, 16 + 4 + (uint32_t)mdl.n);
    buf_u16(&file, 16);
    buf_u16(&file, 1);
    buf_u32(&file, 20);
    buf_bytes(&file, mdl.p, mdl.n);

    out = fopen(out_path, "wb");
    if (!out) {
        dief("cannot write %s: %s", out_path, strerror(errno));
    }
    if (fwrite(file.p, 1, file.n, out) != file.n) {
        fclose(out);
        dief("short write of %s", out_path);
    }
    fclose(out);

    free(dl.p);
    free(mdl.p);
    free(file.p);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: nitromdl <in.gltf|in.glb> <out.nsbmd>\n"
            "       one static mesh, triangles only, no skinning, no TEX0\n");
}

int main(int argc, char **argv)
{
    Mesh mesh;
    const char *in;
    const char *out;

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage();
        return 0;
    }
    if (argc != 3) {
        usage();
        return 2;
    }
    in = argv[1];
    out = argv[2];
    g_in_path = in;

    parse_gltf(in, &mesh);
    write_nsbmd(out, &mesh);
    return 0;
}
