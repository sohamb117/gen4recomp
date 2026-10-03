/*
 * pc/src/pc_gpu3d_gl.c: the gl producer, its selector, context and renderer.
 *
 * The context is offscreen, in this process, and reached at run time: EGL
 * through dlopen and dlsym, never a link-time import, so a machine with no GL
 * runs exactly the binary it always ran and the refusal is a printed line
 * instead of a loader error. Mesa's surfaceless platform is asked for first,
 * because the producer needs no window and no display server; a plain default
 * display is the fallback, and a pbuffer stands in where surfaceless
 * MakeCurrent is refused.
 *
 * The renderer follows the 3DS pilot's settled answers, re-aimed at GLES2. The
 * geometry engine has already done the matrices, the lighting, the clipping
 * and the viewport transform, so a vertex goes out as (ndc*w, z, w): the GPU
 * divides by w, recovers the screen position the DS computed, and interpolates
 * every attribute against the same w the DS used. What the pilot could only
 * prove on its own console does not carry, and the referee here is the
 * desktop's own --gpu3d-diff instrument, which judges every frame this file
 * draws against the software oracle in the same process.
 *
 * A frame this renderer cannot draw honestly is refused, per frame, and the
 * software rasterizer draws it instead. Refused: shadow polygons, a shading
 * mode other than modulation, a frame whose fog enable is set, the clear
 * image, mixed depth modes in one frame, a texture past the converter's
 * ceiling, and vertex overflow. Each refusal is counted and reported at exit,
 * so a run that quietly fell back to soft says so in numbers.
 *
 * Knowingly not drawn yet, so the diff's report reads honestly: edge marking
 * and the DS's anti-aliasing, which needs a second colour buffer no GL fixed
 * function reaches. Those diffs land in the instrument's blend and raster
 * classes and are the next increment's work.
 */

/* A separate link namespace, and it is load-bearing: this binary defines
 * the DS SDK's own time() and exports it (ELF interposition, the
 * override of libc's time() has to be dynamic to win, so it is visible to
 * every flat-loaded library). Measured twice on the first probes: Mesa's
 * init called time(), reached the DWC stack's, and died on
 * OS_IsTickAvailable, and RTLD_DEEPBIND was NOT enough, because Mesa
 * dlopens its gallium driver internally, flat, where deep binding on
 * libEGL does not reach. dlmopen(LM_ID_NEWLM) loads the whole GL stack
 * where the executable's symbols do not exist, and Mesa's internal loads
 * stay in the caller's namespace, so the driver is isolated too. The
 * price is a second libc in that namespace whose errno and FILE* are not
 * ours ([[static-dlopen-loads-a-second-libc]] is the ledger entry);
 * nothing of libc crosses this seam, only GL entry points. */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_gpu3d.h"
#include "pc_gpu3d_gl.h"
#include "pc_gpu3d_gl_tex.h"

static int GlActive;

/* Why the run is not on gl, said once at exit beside how often each frame
 * path ran; a spike whose fallback is silent is a benchmark measuring
 * the wrong producer. */
static unsigned long long GlFrames, GlPolys;
static unsigned long long GlRefused[8];
static const char *const kRefusedName[8] = {
    "shadow", "shading", "fog", "clear-image", "mixed-depth", "texture",
    "room", "gl-error"
};
enum {
    REF_SHADOW = 0, REF_SHADING, REF_FOG, REF_CLEARIMG, REF_MIXED,
    REF_TEXTURE, REF_ROOM, REF_GLERR
};

int pc_gpu3d_gl_active(void)
{
    return GlActive;
}

#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>

typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int EGLint;
typedef unsigned EGLBoolean, EGLenum;

#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_OPENGL_ES_API             0x30A0
#define EGL_OPENGL_ES2_BIT            0x0004
#define EGL_RENDERABLE_TYPE           0x3040
#define EGL_SURFACE_TYPE              0x3033
#define EGL_PBUFFER_BIT               0x0001
#define EGL_WIDTH                     0x3057
#define EGL_HEIGHT                    0x3056
#define EGL_NONE                      0x3038
#define EGL_CONTEXT_CLIENT_VERSION    0x3098

/*
 * The corner of the GLES2 ABI this file touches, spelled locally: the
 * build machine has the runtime libraries, not the headers, and vendoring
 * a header for this many names would be more surface than this. All cdecl
 * on this ABI. Numbers are the GLES2 core's own.
 */
#define GL_RENDERER                   0x1F01
#define GL_VERSION                    0x1F02
#define GL_EXTENSIONS                 0x1F03
#define GL_COLOR_BUFFER_BIT           0x4000
#define GL_DEPTH_BUFFER_BIT           0x0100
#define GL_STENCIL_BUFFER_BIT         0x0400
#define GL_TEXTURE_2D                 0x0DE1
#define GL_RGBA                       0x1908
#define GL_UNSIGNED_BYTE              0x1401
#define GL_FLOAT                      0x1406
#define GL_TRIANGLES                  0x0004
#define GL_TEXTURE0                   0x84C0
#define GL_TEXTURE_MIN_FILTER         0x2801
#define GL_TEXTURE_MAG_FILTER         0x2800
#define GL_TEXTURE_WRAP_S             0x2802
#define GL_TEXTURE_WRAP_T             0x2803
#define GL_NEAREST                    0x2600
#define GL_CLAMP_TO_EDGE              0x812F
#define GL_REPEAT                     0x2901
#define GL_MIRRORED_REPEAT            0x8370
#define GL_FRAMEBUFFER                0x8D40
#define GL_RENDERBUFFER               0x8D41
#define GL_COLOR_ATTACHMENT0          0x8CE0
#define GL_DEPTH_ATTACHMENT           0x8D00
#define GL_STENCIL_ATTACHMENT         0x8D20
#define GL_FRAMEBUFFER_COMPLETE       0x8CD5
#define GL_DEPTH24_STENCIL8_OES       0x88F0
#define GL_DEPTH_COMPONENT16          0x81A5
#define GL_STENCIL_INDEX8             0x8D48
#define GL_DEPTH_TEST                 0x0B71
#define GL_STENCIL_TEST               0x0B90
#define GL_BLEND                      0x0BE2
#define GL_DITHER                     0x0BD0
#define GL_CULL_FACE                  0x0B44
#define GL_SCISSOR_TEST               0x0C11
#define GL_LESS                       0x0201
#define GL_EQUAL                      0x0202
#define GL_GREATER                    0x0204
#define GL_NOTEQUAL                   0x0205
#define GL_ALWAYS                     0x0207
#define GL_KEEP                       0x1E00
#define GL_REPLACE                    0x1E01
#define GL_INCR                       0x1E02
#define GL_ZERO                       0
#define GL_ONE                        1
#define GL_SRC_ALPHA                  0x0302
#define GL_ONE_MINUS_SRC_ALPHA        0x0303
#define GL_CONSTANT_ALPHA             0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA   0x8004
#define GL_FUNC_ADD                   0x8006
#define GL_MAX_EXT                    0x8008
#define GL_VERTEX_SHADER              0x8B31
#define GL_FRAGMENT_SHADER            0x8B30
#define GL_COMPILE_STATUS             0x8B81
#define GL_LINK_STATUS                0x8B82
#define GL_NO_ERROR                   0

static EGLDisplay (*p_eglGetPlatformDisplayEXT)(EGLenum, void *,
                                                const EGLint *);
static EGLDisplay (*p_eglGetDisplay)(void *);
static EGLBoolean (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*p_eglBindAPI)(EGLenum);
static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *,
                                       EGLConfig *, EGLint, EGLint *);
static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                        const EGLint *);
static EGLSurface (*p_eglCreatePbufferSurface)(EGLDisplay, EGLConfig,
                                               const EGLint *);
static EGLBoolean (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
                                      EGLContext);
static EGLBoolean (*p_eglTerminate)(EGLDisplay);
static void *(*p_eglGetProcAddress)(const char *);

static const unsigned char *(*p_glGetString)(unsigned);
static unsigned (*p_glGetError)(void);
static void (*p_glEnable)(unsigned);
static void (*p_glDisable)(unsigned);
static void (*p_glViewport)(int, int, int, int);
static void (*p_glClearColor)(float, float, float, float);
static void (*p_glClearDepthf)(float);
static void (*p_glClearStencil)(int);
static void (*p_glClear)(unsigned);
static void (*p_glDepthFunc)(unsigned);
static void (*p_glDepthMask)(unsigned char);
static void (*p_glBlendFuncSeparate)(unsigned, unsigned, unsigned, unsigned);
static void (*p_glBlendEquationSeparate)(unsigned, unsigned);
static void (*p_glStencilFunc)(unsigned, int, unsigned);
static void (*p_glStencilOp)(unsigned, unsigned, unsigned);
static void (*p_glColorMask)(unsigned char, unsigned char, unsigned char,
                             unsigned char);
static void (*p_glBlendColor)(float, float, float, float);
static void (*p_glUniform2f)(int, float, float);
static void (*p_glUniform4f)(int, float, float, float, float);
static unsigned (*p_glCreateShader)(unsigned);
static void (*p_glShaderSource)(unsigned, int, const char *const *,
                                const int *);
static void (*p_glCompileShader)(unsigned);
static void (*p_glGetShaderiv)(unsigned, unsigned, int *);
static void (*p_glGetShaderInfoLog)(unsigned, int, int *, char *);
static unsigned (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(unsigned, unsigned);
static void (*p_glBindAttribLocation)(unsigned, unsigned, const char *);
static void (*p_glLinkProgram)(unsigned);
static void (*p_glGetProgramiv)(unsigned, unsigned, int *);
static void (*p_glGetProgramInfoLog)(unsigned, int, int *, char *);
static void (*p_glUseProgram)(unsigned);
static int (*p_glGetUniformLocation)(unsigned, const char *);
static void (*p_glUniform1i)(int, int);
static void (*p_glVertexAttribPointer)(unsigned, int, unsigned,
                                       unsigned char, int, const void *);
static void (*p_glEnableVertexAttribArray)(unsigned);
static void (*p_glDrawArrays)(unsigned, int, int);
static void (*p_glGenTextures)(int, unsigned *);
static void (*p_glBindTexture)(unsigned, unsigned);
static void (*p_glActiveTexture)(unsigned);
static void (*p_glTexImage2D)(unsigned, int, int, int, int, int, unsigned,
                              unsigned, const void *);
static void (*p_glTexParameteri)(unsigned, unsigned, int);
static void (*p_glGenFramebuffers)(int, unsigned *);
static void (*p_glBindFramebuffer)(unsigned, unsigned);
static void (*p_glFramebufferTexture2D)(unsigned, unsigned, unsigned,
                                        unsigned, int);
static void (*p_glGenRenderbuffers)(int, unsigned *);
static void (*p_glBindRenderbuffer)(unsigned, unsigned);
static void (*p_glRenderbufferStorage)(unsigned, unsigned, int, int);
static void (*p_glFramebufferRenderbuffer)(unsigned, unsigned, unsigned,
                                           unsigned);
static unsigned (*p_glCheckFramebufferStatus)(unsigned);
static void (*p_glReadPixels)(int, int, int, int, unsigned, unsigned,
                              void *);

/* The kept context: brought up once at selection, current on the main
 * thread (render_frame_now() runs there) for the rest of the run. */
static void *GlLibEgl, *GlLibGles;
static EGLDisplay GlDpy;
static EGLContext GlCtx;
static EGLSurface GlSurf;
static int GlHaveMaxBlend;

/* The renderer's own objects, sized at the first frame, the render grid
 * (scale and wide width) is fixed before guest code runs but after the
 * producer is selected. */
static int RndW, RndH;
static unsigned Fbo, FboTex, FboDepth, FboStencil;
static unsigned Prog, WhiteTex;
static int USlide = -1, UFlat = -1;
/* The sample point, in grid pixels, the pilot's `half=`, kept a knob
 * (PC_GPU3D_HALF) because its value is a property of the rasterizer
 * under it and wants re-sweeping per driver. The pilot's PICA landed on
 * 15/32; THIS default is llvmpipe's own sweep (2026-08-27, town-walk,
 * 2,500 frames x {0.25..0.5}): the diff's EXACT class, pixels only the
 * clear wrote, the kill criterion that has to be zero, is 19,870 bad
 * at 0.5, 6,621 at 13/32, 931 at 5/16 and ZERO at 1/4, where gl's edges
 * stay inside every span the DS drew; the cost is +5%% mean gap in the
 * REPORTED class, which is the distribution that gets weighed, not a bar it
 * sets. See gl_emit(). */
static double SampleOff = 0.25;
static float *Verts;                /* 10 floats a vertex, client arrays */
static int VertN;
static unsigned char *ReadBuf;      /* RGBA8, RndW x RndH               */
static uint32_t *TexScratch;        /* the converter's output           */

/* Triangulating fans out of the DS's own limits: 6,144 vertices, and a
 * clipped polygon carries at most ten, the 3DS's arithmetic. */
#define GL3D_MAX_TRIS  8192
#define GL3D_MAX_VERTS (GL3D_MAX_TRIS * 3)
#define GL3D_MAX_BATCH 1024

struct gl3d_batch {
    int first, count;               /* into the vertex array             */
    int tex;                        /* cache slot, or -1 untextured      */
    uint32_t texparam;              /* for the wrap bits at bind         */
    uint8_t blend, depthWrite, depthEqual;
    /* Attr bits 24-29. In the key because edge marking's stencil
     * reference is per draw, the 3DS measured seven IDs a frame,
     * following objects, so the split lands where the texture already
     * split. Alpha is deliberately NOT here: it rides in the vertex, and
     * every value 1..31 appears in this game. */
    uint8_t polyid;
};
static struct gl3d_batch Batch[GL3D_MAX_BATCH];
static int BatchN;

/* The last six vertices are edge marking's reset quad, not the game's:
 * The whole target at the far plane, drawn between one neighbour
 * direction and the next to put depth and stencil back to "the rear
 * plane, nothing drawn". */
#define GL3D_QUAD_VERTS 6
#define GL3D_QUAD_BASE  (GL3D_MAX_VERTS - GL3D_QUAD_VERTS)

/*
 * Decode each image once per frame, the software renderer's own
 * verdict re-applied: the cache's life is the frame's texture latch, a
 * generation number invalidates everything the moment the next frame
 * latches, and there is no content to hash because there is no state
 * carried across the latch. The 3DS survey's working set is 28 images a
 * frame; 64 slots is past double, and a frame needing more is refused.
 * The image key strips wrap, flip and the coordinate-transform mode
 * (PC_GLTEX_KEY_MASK), sampler state, set at bind, two polygons
 * sampling one image through different wraps share one upload.
 */
#define GL3D_TEX_SLOTS 64
static struct {
    uint32_t key, pal;
    unsigned gen;
    unsigned name;                  /* the GL texture object             */
    int w, h;
} TexCache[GL3D_TEX_SLOTS];
static unsigned TexGen;

static int gl_sym_all(void)
{
    struct { void *fn; const char *name; } *e, entries[] = {
        { &p_glGetString, "glGetString" },
        { &p_glGetError, "glGetError" },
        { &p_glEnable, "glEnable" },
        { &p_glDisable, "glDisable" },
        { &p_glViewport, "glViewport" },
        { &p_glClearColor, "glClearColor" },
        { &p_glClearDepthf, "glClearDepthf" },
        { &p_glClearStencil, "glClearStencil" },
        { &p_glClear, "glClear" },
        { &p_glDepthFunc, "glDepthFunc" },
        { &p_glDepthMask, "glDepthMask" },
        { &p_glBlendFuncSeparate, "glBlendFuncSeparate" },
        { &p_glBlendEquationSeparate, "glBlendEquationSeparate" },
        { &p_glStencilFunc, "glStencilFunc" },
        { &p_glStencilOp, "glStencilOp" },
        { &p_glColorMask, "glColorMask" },
        { &p_glBlendColor, "glBlendColor" },
        { &p_glUniform2f, "glUniform2f" },
        { &p_glUniform4f, "glUniform4f" },
        { &p_glCreateShader, "glCreateShader" },
        { &p_glShaderSource, "glShaderSource" },
        { &p_glCompileShader, "glCompileShader" },
        { &p_glGetShaderiv, "glGetShaderiv" },
        { &p_glGetShaderInfoLog, "glGetShaderInfoLog" },
        { &p_glCreateProgram, "glCreateProgram" },
        { &p_glAttachShader, "glAttachShader" },
        { &p_glBindAttribLocation, "glBindAttribLocation" },
        { &p_glLinkProgram, "glLinkProgram" },
        { &p_glGetProgramiv, "glGetProgramiv" },
        { &p_glGetProgramInfoLog, "glGetProgramInfoLog" },
        { &p_glUseProgram, "glUseProgram" },
        { &p_glGetUniformLocation, "glGetUniformLocation" },
        { &p_glUniform1i, "glUniform1i" },
        { &p_glVertexAttribPointer, "glVertexAttribPointer" },
        { &p_glEnableVertexAttribArray, "glEnableVertexAttribArray" },
        { &p_glDrawArrays, "glDrawArrays" },
        { &p_glGenTextures, "glGenTextures" },
        { &p_glBindTexture, "glBindTexture" },
        { &p_glActiveTexture, "glActiveTexture" },
        { &p_glTexImage2D, "glTexImage2D" },
        { &p_glTexParameteri, "glTexParameteri" },
        { &p_glGenFramebuffers, "glGenFramebuffers" },
        { &p_glBindFramebuffer, "glBindFramebuffer" },
        { &p_glFramebufferTexture2D, "glFramebufferTexture2D" },
        { &p_glGenRenderbuffers, "glGenRenderbuffers" },
        { &p_glBindRenderbuffer, "glBindRenderbuffer" },
        { &p_glRenderbufferStorage, "glRenderbufferStorage" },
        { &p_glFramebufferRenderbuffer, "glFramebufferRenderbuffer" },
        { &p_glCheckFramebufferStatus, "glCheckFramebufferStatus" },
        { &p_glReadPixels, "glReadPixels" },
        { NULL, NULL }
    };

    for (e = entries; e->name != NULL; e++) {
        void *fn = dlsym(GlLibGles, e->name);

        if (fn == NULL) {
            fprintf(stderr, "pc-gpu3d: gl refused, libGLESv2 has no %s\n",
                    e->name);
            return -1;
        }
        memcpy(e->fn, &fn, sizeof fn);
    }
    return 0;
}

/*
 * Bring the offscreen GLES2 context up and KEEP it: display, context and
 * both library handles stay for the run, current on this thread. Returns
 * 0 with the renderer named on stderr, or -1 with the reason, one line
 * either way, so a headless log says exactly how far the machine got.
 */
static int gl_context_up(void)
{
    EGLConfig cfg;
    EGLint ncfg = 0, major = 0, minor = 0;
    static const EGLint want[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };
    static const EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2,
                                      EGL_NONE };
    static const EGLint pbattr[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64,
                                     EGL_NONE };
    const unsigned char *renderer = NULL, *version = NULL, *exts = NULL;

    GlLibEgl = dlmopen(LM_ID_NEWLM, "libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (GlLibEgl == NULL) {
        fprintf(stderr, "pc-gpu3d: gl refused, no libEGL.so.1 for this"
                        " ABI (%s)\n", dlerror());
        return -1;
    }

    p_eglGetProcAddress = (void *(*)(const char *))
        dlsym(GlLibEgl, "eglGetProcAddress");
    p_eglGetDisplay = (EGLDisplay (*)(void *))
        dlsym(GlLibEgl, "eglGetDisplay");
    p_eglInitialize = (EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *))
        dlsym(GlLibEgl, "eglInitialize");
    p_eglBindAPI = (EGLBoolean (*)(EGLenum))dlsym(GlLibEgl, "eglBindAPI");
    p_eglChooseConfig = (EGLBoolean (*)(EGLDisplay, const EGLint *,
                                        EGLConfig *, EGLint, EGLint *))
        dlsym(GlLibEgl, "eglChooseConfig");
    p_eglCreateContext = (EGLContext (*)(EGLDisplay, EGLConfig, EGLContext,
                                         const EGLint *))
        dlsym(GlLibEgl, "eglCreateContext");
    p_eglCreatePbufferSurface = (EGLSurface (*)(EGLDisplay, EGLConfig,
                                                const EGLint *))
        dlsym(GlLibEgl, "eglCreatePbufferSurface");
    p_eglMakeCurrent = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface,
                                       EGLContext))
        dlsym(GlLibEgl, "eglMakeCurrent");
    p_eglTerminate = (EGLBoolean (*)(EGLDisplay))
        dlsym(GlLibEgl, "eglTerminate");

    if (p_eglGetDisplay == NULL || p_eglInitialize == NULL
        || p_eglBindAPI == NULL || p_eglChooseConfig == NULL
        || p_eglCreateContext == NULL || p_eglMakeCurrent == NULL
        || p_eglTerminate == NULL) {
        fprintf(stderr, "pc-gpu3d: gl refused, libEGL is missing core"
                        " entry points\n");
        return -1;
    }

    if (p_eglGetProcAddress != NULL) {
        p_eglGetPlatformDisplayEXT =
            (EGLDisplay (*)(EGLenum, void *, const EGLint *))
            p_eglGetProcAddress("eglGetPlatformDisplayEXT");
    }
    if (p_eglGetPlatformDisplayEXT != NULL) {
        GlDpy = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                           NULL, NULL);
    }
    if (GlDpy == NULL) {
        GlDpy = p_eglGetDisplay(NULL);      /* EGL_DEFAULT_DISPLAY */
    }
    if (GlDpy == NULL || !p_eglInitialize(GlDpy, &major, &minor)) {
        fprintf(stderr, "pc-gpu3d: gl refused; no EGL display would"
                        " initialize (surfaceless, then default)\n");
        GlDpy = NULL;
        return -1;
    }

    if (!p_eglBindAPI(EGL_OPENGL_ES_API)
        || !p_eglChooseConfig(GlDpy, want, &cfg, 1, &ncfg) || ncfg < 1
        || (GlCtx = p_eglCreateContext(GlDpy, cfg, NULL, ctxattr)) == NULL) {
        fprintf(stderr, "pc-gpu3d: gl refused, EGL %d.%d is up but a"
                        " GLES2 context would not come\n", major, minor);
        p_eglTerminate(GlDpy);
        GlDpy = NULL;
        return -1;
    }

    /* Surfaceless first; a 64x64 pbuffer if the driver wants a surface.
     * The renderer draws to its own FBO either way. */
    if (!p_eglMakeCurrent(GlDpy, NULL, NULL, GlCtx)) {
        if (p_eglCreatePbufferSurface != NULL) {
            GlSurf = p_eglCreatePbufferSurface(GlDpy, cfg, pbattr);
        }
        if (GlSurf == NULL
            || !p_eglMakeCurrent(GlDpy, GlSurf, GlSurf, GlCtx)) {
            fprintf(stderr, "pc-gpu3d: gl refused; the context would not"
                            " make current, surfaceless or pbuffer\n");
            p_eglTerminate(GlDpy);
            GlDpy = NULL;
            return -1;
        }
    }

    /* The SAME namespace as libEGL, found from its handle: glvnd
     * dispatches GLES calls to the current context through state the two
     * libraries share, and a copy in another namespace would not see it. */
    {
        Lmid_t lmid = 0;

        GlLibGles = dlinfo(GlLibEgl, RTLD_DI_LMID, &lmid) == 0
                  ? dlmopen(lmid, "libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL)
                  : NULL;
    }
    if (GlLibGles == NULL || gl_sym_all() != 0) {
        if (GlLibGles == NULL) {
            fprintf(stderr, "pc-gpu3d: gl refused, no libGLESv2.so.2 in"
                            " the GL namespace\n");
        }
        p_eglMakeCurrent(GlDpy, NULL, NULL, NULL);
        p_eglTerminate(GlDpy);
        GlDpy = NULL;
        return -1;
    }

    renderer = p_glGetString(GL_RENDERER);
    version = p_glGetString(GL_VERSION);
    exts = p_glGetString(GL_EXTENSIONS);
    GlHaveMaxBlend = exts != NULL
                  && strstr((const char *)exts, "GL_EXT_blend_minmax") != NULL;
    fprintf(stderr, "pc-gpu3d: gl context up, EGL %d.%d, %s, %s\n",
            major, minor,
            renderer != NULL ? (const char *)renderer : "(renderer unknown)",
            version != NULL ? (const char *)version : "(version unknown)");
    return 0;
}

/* ------------------------------------------------------------------ */
/* The renderer                                                        */
/* ------------------------------------------------------------------ */

static unsigned gl_shader(unsigned kind, const char *src)
{
    unsigned sh = p_glCreateShader(kind);
    int ok = 0;

    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];

        p_glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "pc-gpu3d: gl shader did not compile: %s\n", log);
        return 0;
    }
    return sh;
}

/*
 * One program: texture times vertex colour, both channels, the survey
 * found modulation on 100%% of polygons in every scene, so the other three
 * shading modes are a per-frame refusal rather than shader variants
 * nobody measured. An untextured polygon binds the white texel, which
 * modulates to the vertex colour exactly. The discard is the DS's own
 * alpha-zero rule: an index-zero texel disappears rather than painting
 * black (the alpha TEST register never arms in this game, surveyed).
 */
static int gl_program_up(void)
{
    /* The slide is edge marking's: the whole picture, a pixel over, in
     * clip units, x and y arrive multiplied through by w, so the slide
     * is too (the 3DS's uSlide, unchanged). Zero for the picture. */
    static const char *kVert =
        "attribute vec4 aPos;\n"
        "attribute vec4 aCol;\n"
        "attribute vec2 aTex;\n"
        "uniform vec2 uSlide;\n"
        "varying vec4 vCol;\n"
        "varying vec2 vTex;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPos.xy + uSlide * aPos.w, aPos.zw);\n"
        "  vCol = aCol;\n"
        "  vTex = aTex;\n"
        "}\n";
    /*
     * THE DS's own modulate, in integers, because the float shortcut was
     * measured and it is not a shortcut: texture2D(...) * vCol left one
     * six-bit step on nearly every mid-tone pixel of every textured
     * surface, 50 M pixels of the first diffed run, a dim green ocean
     * over the whole town, since the DS computes ((t+1)*(v+1)-1)>>6
     * and float multiplication rounds the other way across the mid-range
     * (t = v = 32: DS 17, float 16). So the texel's six bits and the
     * ramp's nine are recovered, the DS's expression is evaluated as
     * written, and the result is expanded exactly the way the landing's
     * >>2 inverts. The vertex ramp arrives at its full nine bits
     * (FinalColor / 511) and is cut to six per PIXEL, matching the
     * software interpolator, which carries the ramp fine and truncates
     * at the pixel too. Alpha is the same shape in five bits, and the
     * discard is the DS's alpha-zero rule.
     */
    static const char *kFrag =
        "precision highp float;\n"
        "varying vec4 vCol;\n"
        "varying vec2 vTex;\n"
        "uniform sampler2D uTex;\n"
        "uniform vec4 uFlat;\n"
        "void main() {\n"
        "  vec4 t = texture2D(uTex, vTex);\n"
        "  vec3 t6 = floor(floor(t.rgb * 255.0 + 0.5) / 4.0 + 0.001);\n"
        "  vec3 v6 = floor(floor(vCol.rgb * 511.0 + 0.5) / 8.0 + 0.001);\n"
        "  vec3 c6 = floor(((t6 + 1.0) * (v6 + 1.0) - 1.0) / 64.0"
        " + 0.001);\n"
        "  float t5 = floor(floor(t.a * 255.0 + 0.5) / 8.0 + 0.001);\n"
        "  float p5 = floor(vCol.a * 31.0 + 0.5);\n"
        "  float a5 = floor(((t5 + 1.0) * (p5 + 1.0) - 1.0) / 32.0"
        " + 0.001);\n"
        "  if (a5 < 0.5) discard;\n"
        "  vec3 rgb = (c6 * 4.0 + floor(c6 / 16.0 + 0.001)) / 255.0;\n"
        /* Edge marking's colour pass: the fragment keeps its own shape
         * (the discard above is the polygon's alpha cut-out) and paints
         * the table colour instead of its own. uFlat.a is the switch. */
        "  rgb = mix(rgb, uFlat.rgb, uFlat.a);\n"
        "  gl_FragColor = vec4(rgb,\n"
        "                      (a5 * 8.0 + floor(a5 / 4.0 + 0.001))"
        " / 255.0);\n"
        "}\n";
    unsigned vs, fs;
    int ok = 0;

    vs = gl_shader(GL_VERTEX_SHADER, kVert);
    fs = gl_shader(GL_FRAGMENT_SHADER, kFrag);
    if (vs == 0 || fs == 0) {
        return -1;
    }
    Prog = p_glCreateProgram();
    p_glAttachShader(Prog, vs);
    p_glAttachShader(Prog, fs);
    p_glBindAttribLocation(Prog, 0, "aPos");
    p_glBindAttribLocation(Prog, 1, "aCol");
    p_glBindAttribLocation(Prog, 2, "aTex");
    p_glLinkProgram(Prog);
    p_glGetProgramiv(Prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];

        p_glGetProgramInfoLog(Prog, sizeof log, NULL, log);
        fprintf(stderr, "pc-gpu3d: gl program did not link: %s\n", log);
        return -1;
    }
    p_glUseProgram(Prog);
    p_glUniform1i(p_glGetUniformLocation(Prog, "uTex"), 0);
    USlide = p_glGetUniformLocation(Prog, "uSlide");
    UFlat = p_glGetUniformLocation(Prog, "uFlat");
    p_glUniform2f(USlide, 0.0f, 0.0f);
    p_glUniform4f(UFlat, 0.0f, 0.0f, 0.0f, 0.0f);
    return 0;
}

/* The FBO on the render grid, built at the first frame, the grid (HD
 * scale and wide width together) is fixed before guest code runs but
 * after selection. Depth is asked for at 24 bits with stencil packed
 * beside it; a driver without that extension gets 16-bit depth and its
 * own stencil, and the diff's raster class carries the difference. */
static int gl_fbo_up(void)
{
    unsigned status;

    RndW = pc_gpu3d_gl_grid_w();
    RndH = pc_gpu3d_gl_grid_h();

    Verts = (float *)malloc(sizeof *Verts * 10u * GL3D_MAX_VERTS);
    ReadBuf = (unsigned char *)malloc((size_t)RndW * (size_t)RndH * 4u);
    TexScratch = (uint32_t *)malloc(sizeof *TexScratch
                                    * PC_GLTEX_MAX_TEXELS);
    if (Verts == NULL || ReadBuf == NULL || TexScratch == NULL) {
        fprintf(stderr, "pc-gpu3d: gl refused, no memory for the frame"
                        " buffers (%d x %d)\n", RndW, RndH);
        return -1;
    }

    p_glGenTextures(1, &FboTex);
    p_glBindTexture(GL_TEXTURE_2D, FboTex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, RndW, RndH, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    p_glGenFramebuffers(1, &Fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, Fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, FboTex, 0);

    p_glGenRenderbuffers(1, &FboDepth);
    p_glBindRenderbuffer(GL_RENDERBUFFER, FboDepth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES,
                            RndW, RndH);
    if (p_glGetError() == GL_NO_ERROR) {
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                    GL_RENDERBUFFER, FboDepth);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                    GL_RENDERBUFFER, FboDepth);
    } else {
        p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                                RndW, RndH);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                    GL_RENDERBUFFER, FboDepth);
        p_glGenRenderbuffers(1, &FboStencil);
        p_glBindRenderbuffer(GL_RENDERBUFFER, FboStencil);
        p_glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8,
                                RndW, RndH);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                    GL_RENDERBUFFER, FboStencil);
        fprintf(stderr, "pc-gpu3d: gl depth is 16-bit on this driver"
                        " (no packed 24+8)\n");
    }

    status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "pc-gpu3d: gl refused; the framebuffer is"
                        " incomplete (0x%X) at %d x %d\n", status, RndW,
                        RndH);
        return -1;
    }

    /* The white texel: an untextured polygon is its vertex colour, and
     * modulating by exact white is the identity. */
    {
        static const unsigned char kWhite[4] = { 255, 255, 255, 255 };

        p_glGenTextures(1, &WhiteTex);
        p_glBindTexture(GL_TEXTURE_2D, WhiteTex);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                       GL_UNSIGNED_BYTE, kWhite);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    if (gl_program_up() != 0) {
        return -1;
    }

    /* The standing state: no dither (the readback must be the arithmetic,
     * not a pattern), no culling (pc_gpu3d.c already applied the facing
     * bits when it built the polygon, culling again removes an
     * arbitrary subset, the 3DS's ground-vanishing lesson), no scissor. */
    p_glDisable(GL_DITHER);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_SCISSOR_TEST);
    p_glViewport(0, 0, RndW, RndH);
    p_glActiveTexture(GL_TEXTURE0);
    return 0;
}

/*
 * A DS image as a GL texture object, decoded through the proved converter
 * and uploaded, once per frame per image, the generation being the
 * frame's own latch. Returns the slot, or -1 (too big, or a frame past
 * the slot count).
 */
static int gl_texture(uint32_t texparam, uint32_t texpal)
{
    uint32_t key = texparam & PC_GLTEX_KEY_MASK;
    int i, slot = -1;

    for (i = 0; i < GL3D_TEX_SLOTS; i++) {
        if (TexCache[i].name != 0 && TexCache[i].key == key
            && TexCache[i].pal == texpal) {
            slot = i;
            break;
        }
        if (slot < 0 && (TexCache[i].name == 0 || TexCache[i].gen != TexGen)) {
            slot = i;
        }
    }
    if (slot < 0) {
        return -1;
    }
    if (TexCache[slot].name != 0 && TexCache[slot].key == key
        && TexCache[slot].pal == texpal && TexCache[slot].gen == TexGen) {
        return slot;                /* converted earlier this frame */
    }

    if (pc_gltex_convert(TexScratch, texparam, texpal) != 0) {
        return -1;
    }
    if (TexCache[slot].name == 0) {
        p_glGenTextures(1, &TexCache[slot].name);
    }
    TexCache[slot].key = key;
    TexCache[slot].pal = texpal;
    TexCache[slot].gen = TexGen;
    TexCache[slot].w = (int)pc_gltex_width(texparam);
    TexCache[slot].h = (int)pc_gltex_height(texparam);
    p_glBindTexture(GL_TEXTURE_2D, TexCache[slot].name);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TexCache[slot].w,
                   TexCache[slot].h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   TexScratch);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return slot;
}

static unsigned gl_wrap(int mode)
{
    return mode == PC_GLTEX_WRAP_CLAMP ? GL_CLAMP_TO_EDGE
         : mode == PC_GLTEX_WRAP_MIRROR ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

/*
 * One vertex into the array, 3ds_pica3d.c's transform, GL flavour. The
 * divide is undone before it happens: (ndc * w, z, w), so the GPU's own
 * divide recovers the screen position the DS computed and interpolates
 * every attribute against the same w the DS used.
 *
 * The sample point is +15/32 of a pixel, not +1/2: the software
 * rasterizer evaluates a span at integer x and y, a GPU at the pixel's
 * centre, so the geometry slides to put the DS's sample under the centre,
 * and exactly half would land every edge of this game's integer
 * vertices on a centre, a tie the two machines break opposite ways (the
 * pilot swept it: 15/32 is where the difference flattens).
 *
 * DS y stays DOWN: DS row 0 maps to GL's BOTTOM window row, so the
 * readback's row order is the DS's own and nothing flips anywhere.
 * Winding flips with it, and culling is off, so nothing cares.
 *
 * Depth is two problems and the polygon says which: a Z-buffer polygon's
 * FinalZ is screen-linear on both machines and goes out scaled to the
 * window; a W-buffer polygon's is the renormalized W, which no GPU
 * interpolates perspective-correctly on request, but 1/W is
 * screen-linear, so 1/W is fed and the frame's test runs reversed
 * (GL_GREATER, clear at 0).
 */
static int gl_emit(const PcGxPolygon *p, unsigned i, int texW, int texH)
{
    const PcGxVertex *v = p->Vertices[i];
    double w, ndcx, ndcy, depth;
    float *o;
    uint32_t alpha;

    if (v == NULL) {
        return -1;
    }
    w = (double)p->FinalW[i];
    if (w == 0.0) {
        /* The geometry engine nails a zero-W corner to the top-left; a
         * triangle through it is not drawn (the 3DS rule). */
        return -1;
    }

    ndcx = (2.0 * ((double)v->FinalPosition[0] + SampleOff)
            / (double)RndW) - 1.0;
    ndcy = (2.0 * ((double)v->FinalPosition[1] + SampleOff)
            / (double)RndH) - 1.0;

    if (p->WBuffer) {
        double fw = (double)p->FinalZ[i];

        depth = fw > 0.0 ? 1.0 / fw : 1.0;
        if (depth > 1.0) {
            depth = 1.0;
        }
    } else {
        depth = (double)p->FinalZ[i] / 16777215.0;
    }

    o = &Verts[(size_t)VertN * 10u];
    o[0] = (float)(ndcx * w);
    o[1] = (float)(ndcy * w);
    o[2] = (float)((2.0 * depth - 1.0) * w);
    o[3] = (float)w;

    /*
     * FinalColor is (c5 << 4) + 0xF, a NINE-bit ramp, and it goes out
     * whole: the software interpolator carries all nine bits between
     * vertices and cuts to six at the pixel, so cutting here would ramp
     * coarser than the oracle (and dividing the raw field by 63 is the
     * pilot's largest finding: every lit surface saturates white). The
     * shader does the per-pixel >> 3. Alpha rides in the vertex so a
     * change of alpha is not a change of state; 0 is wireframe, which
     * this game never draws (surveyed), and is expressed opaque.
     */
    alpha = (p->Attr >> 16) & 0x1Fu;
    o[4] = (float)v->FinalColor[0] / 511.0f;
    o[5] = (float)v->FinalColor[1] / 511.0f;
    o[6] = (float)v->FinalColor[2] / 511.0f;
    o[7] = alpha == 0u ? 1.0f : (float)alpha / 31.0f;

    /* 1/16-texel units to a fraction of the image. Row 0 of the converted
     * image is DS texel row 0 and GL's v = 0 samples the first row
     * uploaded, so nothing flips here either. */
    o[8] = (float)v->TexCoords[0] / (16.0f * (float)texW);
    o[9] = (float)v->TexCoords[1] / (16.0f * (float)texH);
    VertN++;
    return 0;
}

/* The frame's clear, exact by construction: five bits to six the DS's own
 * way (c * 2, + 1 if nonzero), six to eight replicating the top two,
 * and the landing's >> 2 inverts that exactly, so an untouched pixel
 * round-trips byte-equal, which is the diff's exact class. */
static void gl_clear(const PcGxRenderRegs *r, int reversed)
{
    uint32_t c = r->ClearAttr1;
    uint32_t red = (c & 0x1Fu) * 2u;
    uint32_t green = ((c >> 5) & 0x1Fu) * 2u;
    uint32_t blue = ((c >> 10) & 0x1Fu) * 2u;
    uint32_t a5 = (c >> 16) & 0x1Fu;
    uint32_t a8 = (a5 << 3) | (a5 >> 2);

    if (red) red++;
    if (green) green++;
    if (blue) blue++;
    red = (red << 2) | (red >> 4);
    green = (green << 2) | (green >> 4);
    blue = (blue << 2) | (blue >> 4);

    p_glDepthMask(1);
    p_glClearColor((float)red / 255.0f, (float)green / 255.0f,
                   (float)blue / 255.0f, (float)a8 / 255.0f);
    p_glClearDepthf(reversed ? 0.0f : 1.0f);
    /* The DS does not blend a translucent polygon against an EMPTY pixel:
     * empty meaning destination alpha zero, and occupancy lives in
     * the stencil: a rear plane with alpha is already occupied. */
    p_glClearStencil(a5 != 0u ? 1 : 0);
    p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT
              | GL_STENCIL_BUFFER_BIT);
}

/*
 * Edge marking as the neighbour-describing offset draw, the 3DS
 * derivation whole (3ds_pica3d_gpu.c edge_mark(), which owns the
 * reasoning): the DS marks a pixel when any of its four neighbours
 * belongs to a different OPAQUE polygon ID and lies further away, and a
 * GPU's depth and stencil units only ever test a fragment at its own
 * pixel, so the geometry is drawn one pixel over instead, putting the
 * neighbour's depth and ID under the pixel that wants them. Per
 * direction: a reset quad (rear plane at the far end, the clear's ID;
 * the DS's border cells hold exactly those two values), the opaque
 * geometry slid one pixel writing depth and its ID into stencil, then
 * the geometry where it really is, testing "nearer than the buffer" and
 * "ID not equal", painting the edge colour blended 17/32, the coverage
 * the DS's own final pass leaves on a marked pixel, and the pilot's
 * pixel diff chose the blend over flat paint.
 *
 * What it does not reproduce is the 3DS note's too: the DS only marks a
 * pixel on its polygon's own rasterized edge, and blends against the
 * anti-aliasing's under-pixel rather than the surface; both live in
 * the diff's blend class, priced, not hidden.
 */
static void gl_edge_quad(const PcGxRenderRegs *r, int reversed)
{
    static const float kX[GL3D_QUAD_VERTS] = { -1, 1, 1, -1, 1, -1 };
    static const float kY[GL3D_QUAD_VERTS] = { -1, -1, 1, -1, 1, 1 };
    float z = reversed ? -1.0f : 1.0f;  /* the far end, per depth mode */
    int i;

    (void)r;
    for (i = 0; i < GL3D_QUAD_VERTS; i++) {
        float *o = &Verts[(size_t)(GL3D_QUAD_BASE + i) * 10u];

        memset(o, 0, 10 * sizeof *o);
        o[0] = kX[i];
        o[1] = kY[i];
        o[2] = z;
        o[3] = 1.0f;
        o[7] = 1.0f;                /* the discard must pass */
    }
}

/* EdgeTable's colour for a polygon group: five bits to six the DS's own
 * way, six to eight replicating, the expansions the landing inverts. */
static void gl_edge_colour(const PcGxRenderRegs *r, unsigned polyid,
                           float *out)
{
    unsigned c = r->EdgeTable[(polyid >> 3) & 7u];
    unsigned red = (c << 1) & 0x3Eu;
    unsigned green = (c >> 4) & 0x3Eu;
    unsigned blue = (c >> 9) & 0x3Eu;

    if (red) red++;
    if (green) green++;
    if (blue) blue++;
    out[0] = (float)((red << 2) | (red >> 4)) / 255.0f;
    out[1] = (float)((green << 2) | (green >> 4)) / 255.0f;
    out[2] = (float)((blue << 2) | (blue >> 4)) / 255.0f;
}

static void gl_edge_mark(const PcGxRenderRegs *r, int reversed)
{
    static const struct { float dx, dy; } kDirs[4] = {
        { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f }
    };
    unsigned nearer = reversed ? GL_GREATER : GL_LESS;
    int clearid = (int)((r->ClearAttr1 >> 24) & 0x3Fu);
    int dir, i;

    gl_edge_quad(r, reversed);

    for (dir = 0; dir < 4; dir++) {
        /* Reset: the rear plane, at the far end, with the clear ID,
         * depth and stencil only, the picture is not touched. */
        p_glUniform2f(USlide, 0.0f, 0.0f);
        p_glColorMask(0, 0, 0, 0);
        p_glDepthFunc(GL_ALWAYS);
        p_glDepthMask(1);
        p_glStencilFunc(GL_ALWAYS, clearid, 0x3Fu);
        p_glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
        p_glBindTexture(GL_TEXTURE_2D, WhiteTex);
        p_glDrawArrays(GL_TRIANGLES, GL3D_QUAD_BASE, GL3D_QUAD_VERTS);

        /* Step A: the neighbour's topmost surface, one pixel over, into
         * depth and stencil. Opaque only; the ID edge marking reads is
         * the opaque one. */
        p_glUniform2f(USlide, 2.0f * kDirs[dir].dx / (float)RndW,
                      2.0f * kDirs[dir].dy / (float)RndH);
        p_glDepthFunc(nearer);
        p_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        for (i = 0; i < BatchN; i++) {
            const struct gl3d_batch *b = &Batch[i];

            if (b->count == 0 || b->blend) {
                continue;
            }
            p_glStencilFunc(GL_ALWAYS, (int)b->polyid, 0x3Fu);
            p_glBindTexture(GL_TEXTURE_2D,
                            b->tex >= 0 ? TexCache[b->tex].name : WhiteTex);
            p_glDrawArrays(GL_TRIANGLES, b->first, b->count);
        }

        /* Step B: where the picture really is, marking the near side of
         * every ID boundary, colour only, blended 17/32. */
        p_glUniform2f(USlide, 0.0f, 0.0f);
        p_glColorMask(1, 1, 1, 1);
        p_glDepthMask(0);
        p_glEnable(GL_BLEND);
        p_glBlendColor(0.0f, 0.0f, 0.0f, 17.0f / 32.0f);
        p_glBlendFuncSeparate(GL_CONSTANT_ALPHA,
                              GL_ONE_MINUS_CONSTANT_ALPHA, GL_ZERO, GL_ONE);
        p_glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        for (i = 0; i < BatchN; i++) {
            const struct gl3d_batch *b = &Batch[i];
            float col[3];

            if (b->count == 0 || b->blend) {
                continue;
            }
            gl_edge_colour(r, b->polyid, col);
            p_glUniform4f(UFlat, col[0], col[1], col[2], 1.0f);
            p_glStencilFunc(GL_NOTEQUAL, (int)b->polyid, 0x3Fu);
            p_glBindTexture(GL_TEXTURE_2D,
                            b->tex >= 0 ? TexCache[b->tex].name : WhiteTex);
            p_glDrawArrays(GL_TRIANGLES, b->first, b->count);
        }
        p_glDisable(GL_BLEND);
        p_glDepthMask(1);
        p_glUniform4f(UFlat, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}

/* The picture out of the FBO and into the rasterizer's own rows, eight
 * bits back down to the DS's six (and five of alpha), the exact inverse
 * of every expansion on the way in. */
static void gl_land(void)
{
    int x, y;

    p_glReadPixels(0, 0, RndW, RndH, GL_RGBA, GL_UNSIGNED_BYTE, ReadBuf);
    for (y = 0; y < RndH; y++) {
        const unsigned char *src = &ReadBuf[(size_t)y * (size_t)RndW * 4u];
        uint32_t *row = pc_gpu3d_gl_row(y);

        for (x = 0; x < RndW; x++) {
            row[x] = (uint32_t)(src[0] >> 2)
                   | ((uint32_t)(src[1] >> 2) << 8)
                   | ((uint32_t)(src[2] >> 2) << 16)
                   | ((uint32_t)(src[3] >> 3) << 24);
            src += 4;
        }
    }
}

int pc_gpu3d_gl_frame(struct PcGxPolygon **polys, int npolys)
{
    const PcGxRenderRegs *r = pc_gpu3d_render_regs();
    int reversed = 0, haveMode = 0, textured, drawn = 0;
    int i;
    struct gl3d_batch *b;

    if (!GlActive) {
        return -1;
    }
    if (r->DispCnt & (1u << 7)) {
        GlRefused[REF_FOG]++;
        return -1;
    }
    if (r->DispCnt & (1u << 14)) {
        GlRefused[REF_CLEARIMG]++;
        return -1;
    }
    if (Fbo == 0 && gl_fbo_up() != 0) {
        /* The grid could not be built; nothing about the next frame will
         * differ, so the producer stands down for the run and says so. */
        GlActive = 0;
        fprintf(stderr, "pc-gpu3d: gl stood down; the run continues on"
                        " soft\n");
        return -1;
    }
    textured = (r->DispCnt & 1u) != 0;

    /* Build: one walk over the published list, in order; the order IS
     * the picture for the translucent half. A new batch only when the
     * state or the image changes. */
    TexGen++;
    VertN = 0;
    BatchN = 0;
    for (i = 0; i < npolys; i++) {
        const PcGxPolygon *p = polys[i];
        int tex = -1, texW = 1, texH = 1;
        uint8_t blend, depthWrite, depthEqual;
        unsigned n, k;

        if (p->Degenerate || p->NumVertices < 3) {
            continue;
        }
        if (p->IsShadowMask || p->IsShadow) {
            GlRefused[REF_SHADOW]++;
            return -1;
        }
        if (((p->Attr >> 4) & 3u) != 0u) {
            GlRefused[REF_SHADING]++;
            return -1;
        }
        if (!haveMode) {
            reversed = p->WBuffer != 0;
            haveMode = 1;
        } else if ((p->WBuffer != 0) != reversed) {
            GlRefused[REF_MIXED]++;
            return -1;
        }

        if (textured && ((p->TexParam >> 26) & 7u) != 0u) {
            tex = gl_texture(p->TexParam, p->TexPalette);
            if (tex < 0) {
                GlRefused[REF_TEXTURE]++;
                return -1;
            }
            texW = TexCache[tex].w;
            texH = TexCache[tex].h;
        }

        blend = p->Translucent;
        /* An opaque polygon always writes depth; a translucent one only
         * with Attr bit 11; the DS's own rule, and the reason two
         * translucent surfaces behind each other both show. */
        depthWrite = p->Translucent ? (uint8_t)((p->Attr >> 11) & 1u) : 1u;
        depthEqual = (uint8_t)((p->Attr >> 14) & 1u);

        n = p->NumVertices;
        if (VertN + (int)(n - 2u) * 3 > GL3D_QUAD_BASE) {
            GlRefused[REF_ROOM]++;
            return -1;
        }
        if (BatchN == 0 || Batch[BatchN - 1].tex != tex
            || Batch[BatchN - 1].texparam != p->TexParam
            || Batch[BatchN - 1].blend != blend
            || Batch[BatchN - 1].depthWrite != depthWrite
            || Batch[BatchN - 1].depthEqual != depthEqual
            || Batch[BatchN - 1].polyid != (uint8_t)((p->Attr >> 24) & 0x3Fu)) {
            if (BatchN >= GL3D_MAX_BATCH) {
                GlRefused[REF_ROOM]++;
                return -1;
            }
            b = &Batch[BatchN++];
            b->first = VertN;
            b->count = 0;
            b->tex = tex;
            b->texparam = p->TexParam;
            b->blend = blend;
            b->depthWrite = depthWrite;
            b->depthEqual = depthEqual;
            b->polyid = (uint8_t)((p->Attr >> 24) & 0x3Fu);
        }

        /* A fan: (0, k, k+1); a quad is the n = 4 case with no branch. */
        for (k = 1; k + 1 < n; k++) {
            if (gl_emit(p, 0, texW, texH) != 0
                || gl_emit(p, k, texW, texH) != 0
                || gl_emit(p, k + 1, texW, texH) != 0) {
                VertN = Batch[BatchN - 1].first + Batch[BatchN - 1].count;
                break;
            }
            Batch[BatchN - 1].count += 3;
        }
        drawn++;
    }

    /* Draw. The clear happens whether or not there are polygons: an empty
     * list is the rear plane, never the previous frame. */
    p_glBindFramebuffer(GL_FRAMEBUFFER, Fbo);
    gl_clear(r, reversed);

    if (VertN > 0) {
        p_glUseProgram(Prog);
        p_glVertexAttribPointer(0, 4, GL_FLOAT, 0, 10 * (int)sizeof(float),
                                Verts);
        p_glVertexAttribPointer(1, 4, GL_FLOAT, 0, 10 * (int)sizeof(float),
                                Verts + 4);
        p_glVertexAttribPointer(2, 2, GL_FLOAT, 0, 10 * (int)sizeof(float),
                                Verts + 8);
        p_glEnableVertexAttribArray(0);
        p_glEnableVertexAttribArray(1);
        p_glEnableVertexAttribArray(2);
        p_glEnable(GL_DEPTH_TEST);
        p_glEnable(GL_STENCIL_TEST);

        for (i = 0; i < BatchN; i++) {
            unsigned func;

            b = &Batch[i];
            if (b->count == 0) {
                continue;
            }

            /* Which end of the depth buffer is near is the frame's rule:
             * The usual GL_LESS for a Z frame, reversed for W. EQUAL is
             * the DS's decal-join test and stays EQUAL in both, with
             * the honest caveat that the DS's is an integer window
             * (+/- 0x200) and GL's is exact, a raster-class difference
             * the diff prices rather than a tolerance anyone tunes. */
            func = b->depthEqual ? GL_EQUAL
                 : reversed ? GL_GREATER : GL_LESS;
            p_glDepthFunc(func);
            p_glDepthMask(b->depthWrite);

            p_glBindTexture(GL_TEXTURE_2D,
                            b->tex >= 0 ? TexCache[b->tex].name : WhiteTex);
            if (b->tex >= 0) {
                p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                  (int)gl_wrap(pc_gltex_wrap_s(b->texparam)));
                p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                  (int)gl_wrap(pc_gltex_wrap_t(b->texparam)));
            }

            if (b->blend) {
                /*
                 * Empty destination first: write the source as-is where
                 * nothing has been drawn and mark the pixel occupied;
                 * then the usual src-over where something has. The
                 * double-draw on a fresh pixel is deliberate and
                 * self-cancelling, src blended over itself is src;
                 * which is what lets both passes walk the same batch (the
                 * 3DS derivation, kept whole).
                 */
                p_glDisable(GL_BLEND);
                p_glStencilFunc(GL_EQUAL, 0, 0xFFu);
                p_glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
                p_glDrawArrays(GL_TRIANGLES, b->first, b->count);

                p_glEnable(GL_BLEND);
                if (GlHaveMaxBlend) {
                    /* The DS keeps the larger of the two alphas; MAX is
                     * that exactly where the driver has it. */
                    p_glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX_EXT);
                    p_glBlendFuncSeparate(GL_SRC_ALPHA,
                                          GL_ONE_MINUS_SRC_ALPHA,
                                          GL_ONE, GL_ONE);
                } else {
                    p_glBlendFuncSeparate(GL_SRC_ALPHA,
                                          GL_ONE_MINUS_SRC_ALPHA,
                                          GL_ONE, GL_ZERO);
                }
                p_glStencilFunc(GL_NOTEQUAL, 0, 0xFFu);
                p_glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                p_glDrawArrays(GL_TRIANGLES, b->first, b->count);
                if (GlHaveMaxBlend) {
                    p_glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
                }
            } else {
                p_glDisable(GL_BLEND);
                p_glStencilFunc(GL_ALWAYS, 0, 0x00u);
                p_glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
                p_glDrawArrays(GL_TRIANGLES, b->first, b->count);
            }
        }
        /* Last, the DS's own order: edge marking is a final pass, and it
         * reads the picture's depth buffer and then destroys it. */
        if (r->DispCnt & (1u << 5)) {
            gl_edge_mark(r, reversed);
        }
        p_glDisable(GL_STENCIL_TEST);
        p_glDisable(GL_DEPTH_TEST);
    }

    {
        unsigned err = p_glGetError();

        if (err != GL_NO_ERROR) {
            /* A frame that errored is not a frame to land: the software
             * rasterizer draws it and the count says how often. */
            GlRefused[REF_GLERR]++;
            return -1;
        }
    }

    gl_land();
    GlFrames++;
    GlPolys += (unsigned long long)drawn;
    return drawn;
}

static void gl_report(void)
{
    int i;

    fprintf(stderr, "pc-gpu3d-gl: frames %llu polygons %llu; refused",
            GlFrames, GlPolys);
    for (i = 0; i < 8; i++) {
        fprintf(stderr, " %s %llu", kRefusedName[i], GlRefused[i]);
    }
    fprintf(stderr, "\n");
}

int pc_gpu3d_gl_select(void)
{
    const char *spec = getenv("PC_GPU3D");

    if (spec == NULL || spec[0] == '\0' || strcmp(spec, "soft") == 0) {
        return 0;
    }
    if (strcmp(spec, "gl") != 0) {
        return -1;
    }

    if (gl_context_up() != 0) {
        return 0;                   /* refused, said why, the run is soft */
    }
    {
        const char *half = getenv("PC_GPU3D_HALF");

        if (half != NULL && half[0] != '\0') {
            SampleOff = atof(half);
        }
    }
    GlActive = 1;
    atexit(gl_report);
    fprintf(stderr, "pc-gpu3d: the gl producer answers this run's frames"
                    " (soft remains the oracle; --gpu3d-diff judges"
                    " them)\n");
    return 0;
}

#else /* not linux */

int pc_gpu3d_gl_frame(struct PcGxPolygon **polys, int npolys)
{
    (void)polys;
    (void)npolys;
    return -1;
}

int pc_gpu3d_gl_select(void)
{
    const char *spec = getenv("PC_GPU3D");

    if (spec == NULL || spec[0] == '\0' || strcmp(spec, "soft") == 0) {
        return 0;
    }
    if (strcmp(spec, "gl") != 0) {
        return -1;
    }
#if defined(_WIN32)
    fprintf(stderr, "pc-gpu3d: gl refused, the WGL half of 5.2 is not"
                    " built\n");
#else
    fprintf(stderr, "pc-gpu3d: gl refused, no context path for this"
                    " host\n");
#endif
    return 0;
}

#endif
