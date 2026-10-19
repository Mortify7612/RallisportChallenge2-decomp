/*
 * nv2a_gl.c - the NV2A on a host GPU through OpenGL 4.3 core (with
 * ARB_clip_control).
 *
 * The same entry points as the Vulkan renderer (nv2a_vk.h), for hosts
 * without Vulkan: the Switch's homebrew Mesa (nouveau, through EGL) above
 * all. A build has one or the other -- XBOXRECOMP_HAVE_GL without
 * XBOXRECOMP_HAVE_VULKAN compiles this file and leaves nv2a_vk.c empty.
 *
 * Shared with the Vulkan renderer: the shaders (shaders/nv2a.vert,
 * nv2a.frag, nv2a_common.glsl, and title vertex programs translated by
 * nv2a_vsh_to_glsl around nv2a_draw.c's head and tail), the per-draw
 * uniform block and its fill (nv2a_draw.c), and the executor's texture
 * decoder. The GLSL is Vulkan's, made GL's on the way in: no descriptor
 * sets, and the 2D and cube samplers on texture units 0-3 and 4-7.
 *
 * Coordinates need no flipping: GL's lower-left window origin puts NDC
 * y = -1 on the first row in memory, as Vulkan's upper-left one does, so
 * surfaces read back row for row; the depth range is set to Vulkan's 0..1
 * (glClipControl). The one thing that does flip is facing: Vulkan's area
 * formula has a minus sign GL's lacks, so the front face is inverted.
 *
 * Kept simple for a first version: guest RAM is the truth between frames.
 * A surface is uploaded from it the first time a frame uses it and copied
 * back to it at the flip, or as soon as a texture overlapping it is
 * sampled. Internal resolution is the title's; no MSAA, AO or HDR yet.
 * Visibility queries are counted with GL occlusion queries, read back when
 * the title asks.
 *
 * The context lives on the executor's thread. With a window
 * (RECOMP_FB_WINDOW), fb_present.c makes it and hands it over
 * (nv2a_gl_set_window); without one, a hidden window of our own carries it.
 */
#if defined(XBOXRECOMP_HAVE_GL) && !defined(XBOXRECOMP_HAVE_VULKAN)

#include "nv2a_vk.h"
#include "nv2a_draw.h"
#include "nv2a_combiner.h"
#include "nv2a_vsh_interp.h"
#include "xbox_memory_layout.h"

#include <SDL.h>
#include <GL/glcorearb.h>
#if defined(__SWITCH__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nv2a_vert_src.h"
#include "nv2a_frag_src.h"
#include "nv2a_common_glsl_src.h"

#include "../d3d/d3d8_swizzle.h"      /* xbox_swizzle_masks, swizzle_deposit */
extern uint32_t nv2a_pb_presented_frames(void);

/* ---- GL entry points, through SDL ------------------------------------- */

#define GL_FNS(X) \
    X(PFNGLGETSTRINGPROC, glGetString) \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv) \
    X(PFNGLENABLEPROC, glEnable) \
    X(PFNGLDISABLEPROC, glDisable) \
    X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLSCISSORPROC, glScissor) \
    X(PFNGLCLEARCOLORPROC, glClearColor) \
    X(PFNGLCLEARDEPTHPROC, glClearDepth) \
    X(PFNGLCLEARSTENCILPROC, glClearStencil) \
    X(PFNGLCLEARPROC, glClear) \
    X(PFNGLCOLORMASKPROC, glColorMask) \
    X(PFNGLDEPTHMASKPROC, glDepthMask) \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLSTENCILFUNCPROC, glStencilFunc) \
    X(PFNGLSTENCILOPPROC, glStencilOp) \
    X(PFNGLSTENCILMASKPROC, glStencilMask) \
    X(PFNGLBLENDFUNCPROC, glBlendFunc) \
    X(PFNGLBLENDEQUATIONPROC, glBlendEquation) \
    X(PFNGLBLENDCOLORPROC, glBlendColor) \
    X(PFNGLCULLFACEPROC, glCullFace) \
    X(PFNGLFRONTFACEPROC, glFrontFace) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLREADBUFFERPROC, glReadBuffer) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) \
    X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) \
    X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements) \
    X(PFNGLGENSAMPLERSPROC, glGenSamplers) \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri) \
    X(PFNGLSAMPLERPARAMETERFPROC, glSamplerParameterf) \
    X(PFNGLBINDSAMPLERPROC, glBindSampler) \
    X(PFNGLCLIPCONTROLPROC, glClipControl) \
    X(PFNGLGENQUERIESPROC, glGenQueries) \
    X(PFNGLBEGINQUERYPROC, glBeginQuery) \
    X(PFNGLENDQUERYPROC, glEndQuery) \
    X(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv) \
    X(PFNGLQUERYCOUNTERPROC, glQueryCounter) \
    X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v) \
    X(PFNGLGETINTEGER64VPROC, glGetInteger64v) \
    X(PFNGLCREATESHADERPROGRAMVPROC, glCreateShaderProgramv) \
    X(PFNGLCOMPRESSEDTEXIMAGE2DPROC, glCompressedTexImage2D) \
    X(PFNGLVERTEXATTRIBFORMATPROC, glVertexAttribFormat) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) \
    X(PFNGLVERTEXATTRIBBINDINGPROC, glVertexAttribBinding) \
    X(PFNGLBINDVERTEXBUFFERPROC, glBindVertexBuffer) \
    X(PFNGLGETSTRINGIPROC, glGetStringi) \
    X(PFNGLGENPROGRAMPIPELINESPROC, glGenProgramPipelines) \
    X(PFNGLBINDPROGRAMPIPELINEPROC, glBindProgramPipeline) \
    X(PFNGLUSEPROGRAMSTAGESPROC, glUseProgramStages) \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) \
    X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer) \
    X(PFNGLCOPYIMAGESUBDATAPROC, glCopyImageSubData) \
    X(PFNGLFENCESYNCPROC, glFenceSync) \
    X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync) \
    X(PFNGLDELETESYNCPROC, glDeleteSync) \
    X(PFNGLFLUSHPROC, glFlush)

#define GL_DECL(t, n) static t p_##n;
GL_FNS(GL_DECL)
#undef GL_DECL
#define GL_NAME(t, n) n
/* The program's calls go through the pointers: no link-time GL symbols. */
#define glGetString p_glGetString
#define glGetIntegerv p_glGetIntegerv
#define glEnable p_glEnable
#define glDisable p_glDisable
#define glViewport p_glViewport
#define glScissor p_glScissor
#define glClearColor p_glClearColor
#define glClearDepth p_glClearDepth
#define glClearStencil p_glClearStencil
#define glClear p_glClear
#define glColorMask p_glColorMask
#define glDepthMask p_glDepthMask
#define glDepthFunc p_glDepthFunc
#define glStencilFunc p_glStencilFunc
#define glStencilOp p_glStencilOp
#define glStencilMask p_glStencilMask
#define glBlendFunc p_glBlendFunc
#define glBlendEquation p_glBlendEquation
#define glBlendColor p_glBlendColor
#define glCullFace p_glCullFace
#define glFrontFace p_glFrontFace
#define glPixelStorei p_glPixelStorei
#define glReadPixels p_glReadPixels
#define glReadBuffer p_glReadBuffer
#define glGenTextures p_glGenTextures
#define glDeleteTextures p_glDeleteTextures
#define glBindTexture p_glBindTexture
#define glTexImage2D p_glTexImage2D
#define glTexSubImage2D p_glTexSubImage2D
#define glTexParameteri p_glTexParameteri
#define glActiveTexture p_glActiveTexture
#define glGenFramebuffers p_glGenFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glBlitFramebuffer p_glBlitFramebuffer
#define glGenRenderbuffers p_glGenRenderbuffers
#define glBindRenderbuffer p_glBindRenderbuffer
#define glRenderbufferStorage p_glRenderbufferStorage
#define glDeleteRenderbuffers p_glDeleteRenderbuffers
#define glGenBuffers p_glGenBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glBindBufferBase p_glBindBufferBase
#define glBindBufferRange p_glBindBufferRange
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glVertexAttribPointer p_glVertexAttribPointer
#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glDeleteShader p_glDeleteShader
#define glDeleteProgram p_glDeleteProgram
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glDrawElements p_glDrawElements
#define glGenSamplers p_glGenSamplers
#define glSamplerParameteri p_glSamplerParameteri
#define glSamplerParameterf p_glSamplerParameterf
#define glBindSampler p_glBindSampler
#define glClipControl p_glClipControl
#define glGenQueries p_glGenQueries
#define glBeginQuery p_glBeginQuery
#define glEndQuery p_glEndQuery
#define glGetQueryObjectuiv p_glGetQueryObjectuiv
#define glQueryCounter p_glQueryCounter
#define glGetQueryObjectui64v p_glGetQueryObjectui64v
#define glGetInteger64v p_glGetInteger64v
#define glCreateShaderProgramv p_glCreateShaderProgramv
#define glCompressedTexImage2D p_glCompressedTexImage2D
#define glVertexAttribFormat p_glVertexAttribFormat
#define glDeleteBuffers p_glDeleteBuffers
#define glVertexAttribBinding p_glVertexAttribBinding
#define glBindVertexBuffer p_glBindVertexBuffer
#define glGetStringi p_glGetStringi
#define glGenProgramPipelines p_glGenProgramPipelines
#define glBindProgramPipeline p_glBindProgramPipeline
#define glUseProgramStages p_glUseProgramStages
#define glMapBufferRange p_glMapBufferRange
#define glUnmapBuffer p_glUnmapBuffer
#define glCopyImageSubData p_glCopyImageSubData
#define glFenceSync p_glFenceSync
#define glClientWaitSync p_glClientWaitSync
#define glDeleteSync p_glDeleteSync
#define glFlush p_glFlush

static int gl_load(void)
{
    int ok = 1;
#define GL_LOAD(t, n) \
    if (!(p_##n = (t)SDL_GL_GetProcAddress(#n))) { \
        fprintf(stderr, "[GL] missing %s\n", #n); ok = 0; }
    GL_FNS(GL_LOAD)
#undef GL_LOAD
    return ok;
}

/* ---- state ------------------------------------------------------------- */

#define GL_MAX_SURF  32
#define GL_MAX_ZETA  8
#define GL_MAX_TEX   2048
#define GL_MAX_PROG  1024
#define GL_MAX_SAMP  256
#define GL_SURF_H    1024           /* rows of a linear surface, as Vulkan's */
#define GL_MAX_REPORTS 256
#define GL_MAX_QUERIES 4096
#define GL_RING_SIZE   (32u << 20)
#define GL_MAX_FRAG    1024         /* specialised fragment programs */
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#define GL_MAX_GT      512          /* GPU time stamps in a frame */
#define GL_MAX_GT_STATS 48

typedef struct {
    uint32_t va, pitch, bpp, w, h, fmt;
    int swz;
    uint32_t *swx, *swy;
    GLuint tex;
    int dirty;
    uint32_t rows;                  /* rows the title uses (max clip bottom) */
    uint32_t up_epoch;              /* the epoch it last came from guest RAM */
    uint32_t last_use;
    uint32_t gen;                   /* bumped whenever its contents change */
    /* Guest RAM under it when it last became dirty (flip_writeback() off):
     * if the title writes there afterwards, the GPU's pixels are out of
     * date and never copied back (surf_writeback). */
    uint64_t dref;
    int dref_valid;
    /* What guest RAM held when the texture last matched it (an upload, a
     * write-back): the next upload waits until that changes. */
    uint64_t ref, sref;
    uint32_t ref_ep, ref_full_ep;
    int ref_valid;
    /* Dirty with nothing but one full clear (alias_resolve). */
    int clear_only;
    /* Guest RAM changed under it: upload before the next draw, or never if a
     * full clear comes first (surf_settle). */
    int pending_up;
    uint32_t clear_argb;
} GSurf;

/* A surface the title samples as a texture, copied on the GPU (rt_tex), or
 * a cube map whose six faces are surfaces (rt_cube). */
typedef struct {
    uint32_t va, w, h, x8, last_use;
    int cube;
    uint32_t fgen[6];               /* the surface's (each face's) gen + 1 at its copy */
    GLuint tex;
} GRtTex;
#define GL_MAX_RT 16

typedef struct {
    uint32_t key, w, h;
    GLuint rb;
    uint32_t last_use;
} GZeta;

typedef struct { uint32_t va; uint64_t stamp; int q0, q1, with_carry; } GReport;

/* A flipped frame the GPU may still be drawing: its visibility reports and
 * its surfaces' copy-backs (into pbo), landed a frame later (pend_retire). */
typedef struct {
    int active;
    GLsync fence;
    GLuint *queries;
    int nq, clear_q, nreps;
    GReport reps[GL_MAX_REPORTS];
    GLuint pbo;
    struct { GSurf *s; GLuint tex; uint32_t gen, rows; size_t off; } wb[GL_MAX_SURF];
    int nwb;
} GPend;

typedef struct GTex_ {
    int used;
    Nv2aVkTex d;                    /* the key, sampler fields cleared */
    uint64_t hash, shash;           /* of the bytes uploaded: full, sparse */
    uint32_t hash_ep;               /* the epoch they were last checked */
    uint32_t full_ep;               /* ... in full */
    GLuint tex;
    uint32_t levels, last_use;
} GTex;

typedef struct {
    uint32_t hash;                  /* 0: the CPU-vertex program */
    GLuint prog;                    /* the vertex stage, separable */
    int ninputs, failed;
    uint8_t inputs[16];
} GProg;

/* What a specialised fragment program is built for (nv2a.frag NV2A_SPEC),
 * as nv2a_vk.c's SpecVals. */
typedef struct {
    uint32_t misc_x, misc_y, misc_w;
    uint32_t misc2[4], misc3[4];
    uint32_t cw[8][4];
    float ptsz, tsz[4], vpw;
    uint32_t fast;
} GSpec;

typedef struct {
    uint64_t h;                     /* 0: free */
    GLuint prog;
    GLsync fence;                   /* the compiling thread's, until passed */
    volatile int state;             /* 0 not built, 1 queued, 2 built, -1 failed */
} GFrag;

static struct {
    int state;                      /* 0 not tried, 1 up, -1 off */
    SDL_Window *win;
    SDL_GLContext ctx;
    int own_win, win_w, win_h;
    GLuint fbo, rfbo, vao, vbo, ibo, ubo, cbo, pbo;
    /* One streaming buffer for every draw's uniforms, constants, vertices
     * and indices (draw_common), written unsynchronized at ring_off and
     * orphaned when it wraps. */
    GLuint ring;
    size_t ring_off, ubo_align;
    /* GL_ARB_buffer_storage: the ring mapped once, for good, and a fence
     * for each quarter, set when the writes leave it and waited for before
     * they come back round (ring_reserve). */
    uint8_t *ring_map;
    GLsync ring_fence[4];
    int ring_q;
    int b_ninputs;                  /* vertex attributes enabled, -1 unknown */
    /* The last draw's uniforms and where they sit in the ring: a draw with
     * the same ones binds nothing new. */
    Nv2aVkUbo last_ub;
    size_t last_ub_off;
    uint32_t last_ub_lap;           /* ring_lap it was written in */
    int last_ub_valid;
    uint32_t ring_lap;              /* times the ring has wrapped */
    uint32_t pf_ub_same, pf_consts;
    /* set_state's last state, as a key: a draw with the same one makes no
     * state calls (each is work in Mesa even when nothing changes).
     * state_valid = 0 where anything else touches that state. */
    uint32_t state_key[24];
    int state_valid;
    uint64_t pf_prep_us, pf_glcall_us;  /* draw_common: before its GL calls; the calls */
    /* What draw_common last bound, so unchanged binds are skipped; any other
     * glBindTexture (bind_other) forgets it. */
    GLuint b_tex[8], b_samp[4], b_vs, b_fs;
    int b_valid;
    GLuint white2d, whitecube;
    GSurf surf[GL_MAX_SURF];
    int nsurf;
    GZeta zeta[GL_MAX_ZETA];
    int nzeta;
    GSurf *cur_cs;
    GZeta *cur_zs;
    GTex *tex;
    GProg prog[GL_MAX_PROG];
    int nprog;
    /* The fragment stage: the general nv2a.frag (with and without its depth
     * write) and the specialised ones; one program pipeline holds a draw's
     * two stages. */
    GLuint pipe, fs_gen[2];
    int bc_ok;                      /* DXT textures uploaded as they are */
    GFrag frag[GL_MAX_FRAG];
    int spec_left;                  /* specialised compiles left this frame */
    uint32_t pf_spec, pf_spec_gen;  /* compiled; draws on the general shader */
    uint32_t pf_fragz[4];           /* draws writing gl_FragDepth: poff, zint, other; all */
    uint64_t pf_spec_us;
    struct { uint64_t key; GLuint s; } samp[GL_MAX_SAMP];
    int nsamp;
    uint32_t epoch, frame;
    uint32_t const_serial, const_epoch;
    int zpass;
    /* Visibility reports, answered at the end of the frame (reports_resolve)
     * rather than as each is asked for, which waited for the GPU every time.
     * Queries [0, nq) are this window's; clear_q is where the last
     * CLEAR_REPORT_VALUE fell in it (-1: before it, and carry holds what the
     * queries since then and before the window counted). */
    GLuint *queries;
    int nq, clear_q;
    uint64_t carry;
    GReport reps[GL_MAX_REPORTS];
    int nreps;
    /* The flipped frame still on the GPU (pend_retire), and the query
     * pools: one for it, one for the frame being recorded. */
    GPend pend;
    GLuint *qpool[2];
    uint32_t present_va, present_pitch;
    GRtTex rt[GL_MAX_RT];
    uint32_t prev_present_va;
    /* RECOMP_PERF: where the pushbuffer thread's time goes (perf_report). */
    uint64_t pf_draw_us, pf_tex_us, pf_flush_us, pf_present_us, pf_t0, pf_cpu0;
    uint64_t pf_wait_us, pf_wb_us;  /* waiting for the GPU at flips; every copy-back */
    uint64_t pf_queue_us;           /* queueing a flip's copy-backs */
    uint32_t pf_draws, pf_tex_up, pf_tex_kb, pf_surf_up, pf_wb, pf_frames;
    uint8_t *rb;                    /* readback scratch */
    size_t rb_size;
    /* RECOMP_GL_TRACE: per frame, what became of the draws. */
    uint32_t st_draws, st_cpu, st_notarget, st_noclip, st_clears, st_tex, st_texfail, st_wb;
} G;

/* RECOMP_GL_STEPS=<n>: log the first n first-time operations as they start
 * (a driver or emulator that dies mid-way shows where). */
static void step(const char *what, uint32_t a, uint32_t b)
{
    static int left = -1;
    if (left < 0)
        left = getenv("RECOMP_GL_STEPS") ? atoi(getenv("RECOMP_GL_STEPS")) : 0;
    if (left > 0) {
        left--;
        fprintf(stderr, "[GL step] %s %08X %08X\n", what, a, b);
        fflush(stderr);
    }
}

static volatile SDL_Window *s_handed_win;
static volatile SDL_GLContext s_handed_ctx;
static volatile int s_win_w, s_win_h;

/* fb_present.c's window thread: its window and the context it made (and
 * released) for us. */
void nv2a_gl_set_window(void *win, void *ctx, int w, int h)
{
    s_win_w = w;
    s_win_h = h;
    s_handed_ctx = (SDL_GLContext)ctx;
    __atomic_store_n(&s_handed_win, (SDL_Window *)win, __ATOMIC_RELEASE);
}

/* Whether fb_present.c should make a GL window for us. */
int nv2a_gl_wanted(void)
{
    const char *e = getenv("RECOMP_VULKAN");      /* the GPU renderer, whatever API */
    return e && *e && strcmp(e, "0") != 0;
}

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)((uintptr_t)XBOX_FOLD(va) + xbox_GetMemoryOffset());
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* 64 bytes of every KB, and the last 64: for checking a big texture for
 * changes every epoch (the full hash runs every GL_TEX_FULL_EPOCHS). */
static uint64_t hash_sparse(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t off;
    for (off = 0; off + 64 <= n; off += 1024)
        h = h * 31 + hash_bytes(p + off, 64);
    return n >= 64 ? h * 31 + hash_bytes(p + n - 64, 64) : h;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* RECOMP_PERF=1 (on by default on the Switch): every 5 s, the frame rate,
 * the pushbuffer thread's share of a CPU and where it went, then each busy
 * guest thread's (kernel_bridge.c). */
static int perf_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PERF");
#if defined(__SWITCH__)
        on = !(e && strcmp(e, "0") == 0);
#else
        on = e && strcmp(e, "0") != 0;
#endif
    }
    return on;
}

/* RECOMP_GL_GPU_TIME=1 (with RECOMP_PERF): where the
 * GPU's time goes, after the [PERF] line. A timestamp is taken whenever the
 * work changes -- another render target, texture or surface uploads, the
 * reflection's cube copies, copy-backs, the present -- and the span to the
 * next is charged to what began it. A flipped frame's stamps are read when
 * it lands (pend_retire), after its fence, so they never wait. A span also
 * holds any time the GPU waited for the CPU to send it work, so the parts
 * add up to the frame time unless the GPU is the bottleneck; how much
 * further the GPU falls behind the CPU between a frame's first stamp and
 * its last tells the two apart (only the change: the Switch's
 * glGetInteger64v(GL_TIMESTAMP) is not on the queries' clock). Pipelined
 * flips only. */
enum { GT_TARGET, GT_DEPTH, GT_TEX, GT_SURF, GT_CUBE, GT_RT, GT_READ, GT_PRESENT, GT_OTHER };
typedef struct { uint32_t va; uint16_t w, h; uint8_t kind, swz, pad[2]; } GtLabel;
typedef struct {
    GLuint q[GL_MAX_GT + 1];
    uint64_t cpu[GL_MAX_GT + 1];    /* now_us() at each stamp */
    GtLabel l[GL_MAX_GT];
    int n;
} GtFrame;
static struct {
    int on;
    GtFrame fr[2];
    int cur;                        /* the frame being recorded */
    int pend;                       /* 1 + the flipped frame's, 0: none */
    GtLabel top;                    /* what the GPU is being given now */
    struct { GtLabel l; uint64_t ns; } st[GL_MAX_GT_STATS];
    int nst;
    uint32_t frames;
    int64_t off_ns;                 /* GPU clock - CPU clock */
    int64_t lag0_ns, lag1_ns;       /* summed lag at frames' first and last stamps */
} GT;

static int pipelined(void);

/* Ties the GPU's clock to now_us(). */
static void gt_sync_clock(void)
{
    GLint64 g = 0;
    uint64_t c = now_us();
    glGetInteger64v(GL_TIMESTAMP, &g);
    GT.off_ns = (int64_t)g - (int64_t)((c + now_us()) / 2 * 1000);
}

static void gt_init(void)
{
    const char *e = getenv("RECOMP_GL_GPU_TIME");
    GT.on = e && strcmp(e, "0") != 0;
    GT.on = GT.on && perf_on() && pipelined();
    if (GT.on) {
        glGenQueries(GL_MAX_GT + 1, GT.fr[0].q);
        glGenQueries(GL_MAX_GT + 1, GT.fr[1].q);
        gt_sync_clock();
    }
}

static GtLabel gt_label(int kind, uint32_t va, uint32_t w, uint32_t h, int swz)
{
    GtLabel l;
    memset(&l, 0, sizeof l);
    l.kind = (uint8_t)kind;
    l.va = va;
    l.w = (uint16_t)w;
    l.h = (uint16_t)h;
    l.swz = (uint8_t)swz;
    return l;
}

/* The GPU's work from here on is l. */
static void gt_mark(GtLabel l)
{
    GtFrame *f = &GT.fr[GT.cur];
    if (!GT.on)
        return;
    GT.top = l;
    if ((f->n && memcmp(&f->l[f->n - 1], &l, sizeof l) == 0) || f->n == GL_MAX_GT)
        return;
    f->l[f->n] = l;
    f->cpu[f->n] = now_us();
    glQueryCounter(f->q[f->n], GL_TIMESTAMP);
    f->n++;
}

/* Work of another kind in the middle of a target's: gt_pop(gt_push(k)). */
static GtLabel gt_push(int kind)
{
    GtLabel prev = GT.top;
    gt_mark(gt_label(kind, 0, 0, 0, 0));
    return prev;
}

static void gt_pop(GtLabel prev)
{
    gt_mark(prev);
}

/* At the flip, before its fence: the frame's last stamp. */
static void gt_flip(void)
{
    GtFrame *f = &GT.fr[GT.cur];
    if (!GT.on)
        return;
    GT.pend = 0;
    if (f->n) {
        f->cpu[f->n] = now_us();
        glQueryCounter(f->q[f->n], GL_TIMESTAMP);
        GT.pend = GT.cur + 1;
    }
    GT.cur ^= 1;
    GT.fr[GT.cur].n = 0;
}

/* The flipped frame's spans, once its fence has passed. */
static void gt_land(void)
{
    GtFrame *f;
    GLuint64 t0 = 0, t1 = 0;
    int i, k;
    if (!GT.pend)
        return;
    f = &GT.fr[GT.pend - 1];
    GT.pend = 0;
    glGetQueryObjectui64v(f->q[0], GL_QUERY_RESULT, &t0);
    GT.lag0_ns += (int64_t)t0 - (int64_t)(f->cpu[0] * 1000) - GT.off_ns;
    for (i = 0; i < f->n; i++, t0 = t1) {
        GtLabel l = f->l[i];
        glGetQueryObjectui64v(f->q[i + 1], GL_QUERY_RESULT, &t1);
        if (t1 <= t0)
            continue;
        for (k = 0; k < GT.nst; k++)
            if (memcmp(&GT.st[k].l, &l, sizeof l) == 0)
                break;
        if (k == GT.nst) {
            if (GT.nst == GL_MAX_GT_STATS) {
                k = GT.nst - 1;
                GT.st[k].l = gt_label(GT_OTHER, 0, 0, 0, 0);
            } else {
                GT.nst++;
                GT.st[k].l = l;
                GT.st[k].ns = 0;
            }
        }
        GT.st[k].ns += t1 - t0;
    }
    GT.lag1_ns += (int64_t)t0 - (int64_t)(f->cpu[f->n] * 1000) - GT.off_ns;
    GT.frames++;
}

static void gt_report(void)
{
    static const char *names[] = { "", "", "texture uploads", "surface uploads",
        "reflection cube copies", "render-target copies", "copy-backs", "present", "other" };
    int order[GL_MAX_GT_STATS], i, j;
    uint64_t sum = 0;
    double f;
    if (!GT.on || !GT.frames)
        return;
    f = (double)GT.frames * 1e6;
    for (i = 0; i < GT.nst; i++) {
        sum += GT.st[i].ns;
        for (j = i; j > 0 && GT.st[order[j - 1]].ns < GT.st[i].ns; j--)
            order[j] = order[j - 1];
        order[j] = i;
    }
    fprintf(stderr, "[PERF]   GPU %.1f ms a frame (%u frames timed), falling %.1f ms further"
            " behind the CPU across one:\n", (double)sum / f, GT.frames,
            (double)(GT.lag1_ns - GT.lag0_ns) / f);
    for (i = 0; i < GT.nst && i < 10; i++) {
        const GtLabel *l = &GT.st[order[i]].l;
        char what[64];
        if (l->kind == GT_TARGET && l->swz)
            snprintf(what, sizeof what, "swizzled %08X %ux%u", l->va, l->w, l->h);
        else if (l->kind == GT_TARGET)
            snprintf(what, sizeof what, "surface %08X %u wide%s", l->va, l->w,
                     l->va == G.present_va || l->va == G.prev_present_va ? " (shown)" : "");
        else if (l->kind == GT_DEPTH)
            snprintf(what, sizeof what, "depth only %08X", l->va);
        else
            snprintf(what, sizeof what, "%s", names[l->kind]);
        fprintf(stderr, "[PERF]     %-36s %6.1f ms\n", what, (double)GT.st[order[i]].ns / f);
    }
    GT.nst = 0;
    GT.frames = 0;
    GT.lag0_ns = GT.lag1_ns = 0;
    gt_sync_clock();
}

/* RECOMP_GL_EXP=<n>|cycle (a measurement, not for playing): 1 skips the
 * occlusion queries (reports then see nothing drawn), 2 discards every
 * primitive before rasterizing (no pixel work; the screen goes wrong), 3
 * both. "cycle" steps through 0-3 at each [PERF] report. */
static int s_exp = -1, s_exp_cycle;

static int gl_exp(void)
{
    if (s_exp < 0) {
        const char *e = getenv("RECOMP_GL_EXP");
        s_exp_cycle = e && strcmp(e, "cycle") == 0;
        s_exp = e && !s_exp_cycle ? atoi(e) & 3 : 0;
        if (s_exp & 2)
            glEnable(GL_RASTERIZER_DISCARD);
    }
    return s_exp;
}

static void perf_report(void)
{
    extern void xbox_thread_perf_report(double secs);
    uint64_t t = now_us(), cpu = w32_thread_cpu_us();
    double secs, f;
    if (!G.pf_t0) {
        G.pf_t0 = t;
        G.pf_cpu0 = cpu;
        return;
    }
    secs = (double)(t - G.pf_t0) / 1e6;
    if (secs < 5.0)
        return;
    f = G.pf_frames ? (double)G.pf_frames : 1.0;
    fprintf(stderr, "[PERF] %.1f fps; pushbuffer thread %.0f%% cpu; per frame: %.0f draws %.1f ms,"
            " textures %.1f ms (%.1f uploads, %.0f KB), surface uploads %.1f, write-backs %.1f"
            " (%.1f ms), flush %.1f ms (GPU wait %.1f, queueing copy-backs %.1f), present %.1f ms\n",
            G.pf_frames / secs, (double)(cpu - G.pf_cpu0) / (secs * 1e4),
            G.pf_draws / f, G.pf_draw_us / f / 1000.0, G.pf_tex_us / f / 1000.0,
            G.pf_tex_up / f, G.pf_tex_kb / f, G.pf_surf_up / f, G.pf_wb / f,
            G.pf_wb_us / f / 1000.0, G.pf_flush_us / f / 1000.0, G.pf_wait_us / f / 1000.0,
            G.pf_queue_us / f / 1000.0,
            G.pf_present_us / f / 1000.0);
    if (G.pf_spec || G.pf_spec_gen)
        fprintf(stderr, "[PERF]   shaders: %u specialised built (%.1f ms), %.0f draws a frame"
                " waiting on the general one\n", G.pf_spec, G.pf_spec_us / 1000.0,
                G.pf_spec_gen / f);
    {
        extern void nv2a_pb_perf_take(uint64_t out[5]);
        uint64_t pb[5];
        nv2a_pb_perf_take(pb);
        fprintf(stderr, "[PERF]   executor per frame: %.1f ms in all; fetching vertices %.1f,"
                " triangle lists %.1f, state %.1f, the renderer's draws %.1f (setting up %.1f,"
                " uploading and GL calls %.1f)\n", pb[4] / f / 1e6, pb[0] / f / 1e6, pb[1] / f / 1e6,
                pb[2] / f / 1e6, pb[3] / f / 1e6, G.pf_prep_us / f / 1000.0,
                G.pf_glcall_us / f / 1000.0);
        G.pf_prep_us = G.pf_glcall_us = 0;
    }
    if (G.pf_draws)
        fprintf(stderr, "[PERF]   per frame: %.0f draws with the uniforms of the draw before,"
                " %.0f vertex constant uploads\n", G.pf_ub_same / f, G.pf_consts / f);
    G.pf_ub_same = G.pf_consts = 0;
    if (G.pf_fragz[3])
        fprintf(stderr, "[PERF]   of %.0f draws a frame, writing their depth: %.0f polygon offset,"
                " %.0f z-buffer rounding, %.0f other\n", G.pf_fragz[3] / f, G.pf_fragz[0] / f,
                G.pf_fragz[1] / f, G.pf_fragz[2] / f);
    memset(G.pf_fragz, 0, sizeof G.pf_fragz);
    G.pf_spec = G.pf_spec_gen = 0;
    G.pf_spec_us = 0;
    if (s_exp_cycle || s_exp > 0)
        fprintf(stderr, "[PERF]   experiment %d (RECOMP_GL_EXP)\n", gl_exp());
    gt_report();
    xbox_thread_perf_report(secs);
    if (s_exp_cycle) {
        s_exp = (s_exp + 1) & 3;
        if (s_exp & 2)
            glEnable(GL_RASTERIZER_DISCARD);
        else
            glDisable(GL_RASTERIZER_DISCARD);
    }
    G.pf_draw_us = G.pf_tex_us = G.pf_flush_us = G.pf_present_us = 0;
    G.pf_wait_us = G.pf_wb_us = G.pf_queue_us = 0;
    G.pf_draws = G.pf_tex_up = G.pf_tex_kb = G.pf_surf_up = G.pf_wb = G.pf_frames = 0;
    G.pf_t0 = t;
    G.pf_cpu0 = cpu;
}

/* A texture bind outside draw_common: whatever it last bound is not known
 * to be there any more. */
static void bind_other(GLenum target, GLuint t)
{
    glBindTexture(target, t);
    G.b_valid = 0;
}

/* ---- shaders ----------------------------------------------------------- */

/* Vulkan GLSL to GL GLSL: no `set = 0, `; the sampler arrays on units 0-3
 * (2D) and 4-7 (cube) rather than bindings 1 and 2, which would overlap; the
 * shared include inlined; gl_VertexIndex is gl_VertexID. */
static char *gl_source(const char *src, const char *defines)
{
    size_t n = strlen(src) + strlen(k_nv2a_common_glsl_src) + strlen(defines) + 256;
    char *out = (char *)malloc(n), *q = out;
    const char *p = src;
    if (!out)
        return NULL;
    q += sprintf(q, "#version 430 core\n%s", defines);
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) + 1 : strlen(p);
        if (!strncmp(p, "#version", 8) || !strncmp(p, "#extension", 10)) {
            /* dropped */
        } else if (!strncmp(p, "#include", 8)) {
            q += sprintf(q, "%s\n", k_nv2a_common_glsl_src);
        } else {
            memcpy(q, p, len);
            q += len;
        }
        p += len;
    }
    *q = 0;
    /* In place: every substitution shortens or keeps the length. */
    {
        static const char *const from[] = {
            "set = 0, binding = 1) uniform sampler2D", "set = 0, binding = 2) uniform samplerCube",
            "set = 0, ", "gl_VertexIndex",
        };
        static const char *const to[] = {
            "binding = 0) uniform sampler2D", "binding = 4) uniform samplerCube",
            "", "gl_VertexID",
        };
        size_t k;
        for (k = 0; k < sizeof from / sizeof from[0]; k++) {
            char *m;
            size_t fl = strlen(from[k]), tl = strlen(to[k]);
            while ((m = strstr(out, from[k])) != NULL) {
                memcpy(m, to[k], tl);
                memmove(m + tl, m + fl, strlen(m + fl) + 1);
            }
        }
    }
    return out;
}

/* One stage as a separable program: a draw pairs its vertex program with
 * the fragment program its combiner setup picks (frag_get) in G.pipe. */
static GLuint sep_program(GLenum type, const char *src)
{
    GLuint p = glCreateShaderProgramv(type, 1, &src);
    GLint ok = 0;
    if (!p)
        return 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "[GL] %s program failed:\n%s\n", type == GL_VERTEX_SHADER ? "vertex"
                : "fragment", log);
        if (getenv("RECOMP_GL_SHADER_DUMP"))
            fprintf(stderr, "%s\n", src);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* nv2a.frag's general program: it reads the combiner setup from the
 * uniforms. `fast`: built without its gl_FragDepth write. */
static GLuint frag_general(int fast)
{
    char *src = gl_source(k_nv2a_frag_src, fast ? "#define NV2A_FAST_DEPTH 1\n" : "");
    GLuint p = src ? sep_program(GL_FRAGMENT_SHADER, src) : 0;
    free(src);
    return p;
}

/* ---- specialised fragment programs -------------------------------------
 *
 * The general nv2a.frag reads the combiner setup from the uniforms, so every
 * pixel walks all of it with its register file R[] indexed at run time, and
 * it writes gl_FragDepth, which turns off the early depth test. On the
 * Switch that made the race's 640x480 scene about 135 ms of GPU time a
 * frame. As in the Vulkan renderer, a draw's setup is compiled again with
 * those values as constants (NV2A_SPEC), and without the depth write when
 * the draw does not need it (NV2A_FAST_DEPTH). They are compiled on a
 * thread of their own (sc_worker), or without one on this thread, a few a
 * frame (RECOMP_GL_SPEC_PER_FRAME, default 2, 1 on the Switch, where Mesa
 * takes most of a second over each); draws use the
 * general program until theirs is built. RECOMP_GL_SPEC=0 keeps every draw
 * on the general program; RECOMP_GL_FAST_DEPTH=0 keeps the depth write. */
static int spec_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_SPEC");
        on = !(e && strcmp(e, "0") == 0);
    }
    return on;
}

static int spec_per_frame(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("RECOMP_GL_SPEC_PER_FRAME");
#if defined(__SWITCH__)
        n = e ? atoi(e) : 1;        /* about 0.9 s each there */
#else
        n = e ? atoi(e) : 2;
#endif
        if (n < 1)
            n = 1;
    }
    return n;
}

/* Whether the fragment shader has to write the depth itself: the polygon
 * offset, or the z-buffer's integer rounding (the GL renderer always keeps
 * the w-buffer reversed, which the rasteriser produces). As nv2a_vk.c. */
static int needs_fragdepth(const Nv2aVkState *st)
{
    static int fast = -1;
    if (fast < 0) {
        const char *e = getenv("RECOMP_GL_FAST_DEPTH");
        fast = !(e && strcmp(e, "0") == 0);
    }
    if (!fast)
        return 1;
    if (!G.cur_zs || !st->depth_test)
        return 0;
    if (st->poff_factor != 0.0f || st->poff_units != 0.0f)
        return 1;
    return st->wbuffer ? 0 : (int)st->zint;
}

static void spec_vals(GSpec *v, const Nv2aVkUbo *ub, int fast)
{
    uint32_t i, n = (ub->misc[0] & 1u) ? (ub->misc2[0] & 0xFFu) : 0;
    memset(v, 0, sizeof *v);
    v->misc_x = ub->misc[0];
    v->misc_y = ub->misc3[0] ? ub->misc[1] : 0;     /* alpha func, if tested */
    v->misc_w = ub->misc[3];
    memcpy(v->misc2, ub->misc2, sizeof v->misc2);
    memcpy(v->misc3, ub->misc3, sizeof v->misc3);
    if (fast)
        v->misc3[3] = 0;                            /* zint: depth path only */
    if (n > 8)
        n = 8;
    for (i = 0; i < n; i++)
        memcpy(v->cw[i], ub->cw[i], sizeof v->cw[i]);
    v->ptsz = ub->pts[2];
    for (i = 0; i < 4; i++)
        v->tsz[i] = ub->ts[i][2];
    v->vpw = ub->vp[3];
    v->fast = (uint32_t)fast;
}

/* ---- generated fragment programs ---------------------------------------
 *
 * The specialised program, written out for one setup: only the texture
 * stages that fetch, the combiner stages that run, each as plain
 * statements on named registers -- no functions, switches, loops or
 * arrays. nv2a.frag with the setup's values as constants compiles to the
 * same thing, but Mesa inlines and folds the whole general shader first:
 * 1-7 s a program on the Switch, a freeze each time a new setup came into
 * view. Every rule follows nv2a.frag (stage_fetch, rc_stage, the final
 * combiner, alpha_pass, the depth write), which follows the software
 * rasteriser. RECOMP_GL_SPEC_GEN=0 builds from nv2a.frag instead. */
typedef struct { char *p; size_t n, cap; } GBuf;

static void gb(GBuf *b, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (!b->p)
        return;
    va_start(ap, fmt);
    k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= b->cap - b->n) {
        free(b->p);                 /* too long: give up, the caller falls back */
        b->p = NULL;
        return;
    }
    b->n += (size_t)k;
}

/* map_in: x (a vec3 or float expression) by the input mapping of b. */
static void gen_map_in(char *o, size_t n, const char *x, uint32_t b)
{
    switch ((b >> 5) & 7u) {
    case 0:  snprintf(o, n, "max(%s, 0.0)", x); break;
    case 1:  snprintf(o, n, "(1.0 - clamp(%s, 0.0, 1.0))", x); break;
    case 2:  snprintf(o, n, "(2.0 * max(%s, 0.0) - 1.0)", x); break;
    case 3:  snprintf(o, n, "(-2.0 * max(%s, 0.0) + 1.0)", x); break;
    case 4:  snprintf(o, n, "(max(%s, 0.0) - 0.5)", x); break;
    case 5:  snprintf(o, n, "(-max(%s, 0.0) + 0.5)", x); break;
    case 6:  snprintf(o, n, "(%s)", x); break;
    default: snprintf(o, n, "(-(%s))", x); break;
    }
}

/* read_rgb / read_alpha of combiner input b. r0 reads zero. */
static void gen_rgb(char *o, size_t n, uint32_t b)
{
    char x[32];
    b &= 0xFFu;
    if ((b & 15u) == 0)
        snprintf(x, sizeof x, "vec3(0.0)");
    else
        snprintf(x, sizeof x, "r%u.%s", b & 15u, (b & 0x10u) ? "aaa" : "rgb");
    gen_map_in(o, n, x, b);
}

static void gen_alpha(char *o, size_t n, uint32_t b)
{
    char x[32];
    b &= 0xFFu;
    if ((b & 15u) == 0)
        snprintf(x, sizeof x, "0.0");
    else
        snprintf(x, sizeof x, "r%u.%s", b & 15u, (b & 0x10u) ? "a" : "b");
    gen_map_in(o, n, x, b);
}

/* map_out m, then the clamp to [-1, 1], of expression x. */
static void gen_map_out(char *o, size_t n, const char *x, uint32_t m)
{
    switch (m) {
    case 0x08: snprintf(o, n, "clamp(%s - 0.5, -1.0, 1.0)", x); break;
    case 0x10: snprintf(o, n, "clamp(%s * 2.0, -1.0, 1.0)", x); break;
    case 0x18: snprintf(o, n, "clamp((%s - 0.5) * 2.0, -1.0, 1.0)", x); break;
    case 0x20: snprintf(o, n, "clamp(%s * 4.0, -1.0, 1.0)", x); break;
    case 0x30: snprintf(o, n, "clamp(%s * 0.5, -1.0, 1.0)", x); break;
    default:   snprintf(o, n, "clamp(%s, -1.0, 1.0)", x); break;
    }
}

/* Texture stage st's fetch into tN (stage_fetch). */
static void gen_stage(GBuf *b, const GSpec *v, uint32_t st)
{
    uint32_t mode = (v->misc_w >> (st * 5u)) & 0x1Fu, inp, j;
    if (mode == 0) {
        gb(b, "    t%u = vec4(0.0, 0.0, 0.0, 1.0);\n", st);
        return;
    }
    if (mode == 1 || mode == 2) {
        gb(b, "    t%u = texture(t2d[%u], tc%u.xy / (tc%u.w != 0.0 ? tc%u.w : 1.0) * u.ts[%u].xy);\n",
           st, st, st, st, st, st);
        return;
    }
    if (mode == 3) {
        if (v->tsz[st] != 0.0f)
            gb(b, "    t%u = texture(tcube[%u], tc%u.xyz);\n", st, st, st);
        else
            gb(b, "    t%u = texture(t2d[%u], tc%u.xy * u.ts[%u].xy);\n", st, st, st, st);
        return;
    }
    if (mode == 4) {
        gb(b, "    t%u = clamp(tc%u, 0.0, 1.0);\n", st, st);
        return;
    }
    if (mode == 5) {
        for (j = 0; j < 4; j++)
            gb(b, "    if (tc%u[%u] %s 0.0) discard;\n", st, j,
               ((v->misc2[3] >> (st * 4u + j)) & 1u) ? ">=" : "<");
        gb(b, "    t%u = vec4(0.0);\n", st);
        return;
    }
    if (st == 0) {
        gb(b, "    t0 = vec4(0.0);\n");
        return;
    }
    inp = st == 1 ? 0u : st == 2 ? (v->misc3[1] >> 16) & 0xFu : (v->misc3[1] >> 20) & 0xFu;
    if (inp > 3)
        inp = 3;
    if (mode == 15) {
        gb(b, "    t%u = texture(t2d[%u], vec2(t%u.a, t%u.r));\n", st, st, inp, inp);
        return;
    }
    if (mode == 16) {
        gb(b, "    t%u = texture(t2d[%u], vec2(t%u.g, t%u.b));\n", st, st, inp, inp);
        return;
    }
    if (mode == 6 || mode == 7) {
        gb(b, "    t%u = texture(t2d[%u], (tc%u.xy + (t%u.rg * 2.0 - 1.0)) * u.ts[%u].xy);\n",
           st, st, st, inp, st);
        return;
    }
    {
        /* dot_input: the input stage's texel as a vector, then the dot. */
        uint32_t m = (v->misc3[2] >> ((st - 1u) * 4u)) & 0xFu;
        gb(b, "    {\n        vec3 c = t%u.rgb;\n", inp);
        if (m == 1)
            gb(b, "        vec3 x = c * 255.0 - 128.0;\n"
                  "        c = x / mix(vec3(128.0), vec3(127.0), greaterThanEqual(x, vec3(0.0)));\n");
        else if (m == 3)
            gb(b, "        vec3 x = floor(c * 255.0 + 0.5);\n"
                  "        c = mix(x, x - 256.0, greaterThanEqual(x, vec3(128.0))) / 127.0;\n");
        else if (m != 0)
            gb(b, "        c = c * 2.0 - 1.0;\n");
        gb(b, "        d%u = dot(tc%u.xyz, c);\n    }\n", st, st);
    }
    if (mode == 9) {
        gb(b, "    t%u = texture(t2d[%u], vec2(d%u, d%u) * u.ts[%u].xy);\n", st, st, st - 1, st, st);
        return;
    }
    if (st >= 2 && (mode == 11 || mode == 13 || mode == 14 || mode == 12 || mode == 18)) {
        char N[64];
        snprintf(N, sizeof N, st == 3 ? "vec3(d1, d2, d3)" : "vec3(d1, d2, 0.0)");
        if (mode == 11 || mode == 13 || mode == 14) {
            gb(b, "    t%u = texture(tcube[%u], %s);\n", st, st, N);
            return;
        }
        if (!(v->misc_x & 4u)) {
            gb(b, "    t%u = vec4(0.0);\n", st);
            return;
        }
        gb(b, "    {\n        vec3 N = %s, E = %s;\n        float nn = dot(N, N);\n"
              "        t%u = texture(tcube[%u], nn != 0.0 ? 2.0 * N * dot(N, E) / nn - E : E);\n    }\n",
           N, mode == 12 ? "vec3(tc1.w, tc2.w, tc3.w)" : "vec3(0.0, 0.0, 1.0)", st, st);
        return;
    }
    gb(b, "    t%u = vec4(0.0);\n", st);    /* 10, 17 and the rest */
}

/* One general combiner stage s (rc_stage). */
static void gen_rc(GBuf *b, const GSpec *v, uint32_t s, uint32_t flags)
{
    uint32_t icw = v->cw[s][0], aicw = v->cw[s][1], ocw = v->cw[s][2], aocw = v->cw[s][3];
    uint32_t fl = ocw >> 12, afl = aocw >> 12, d;
    char A[96], B[96], C[96], D[96], t[160];
    gb(b, "    {\n");
    gb(b, "        r1 = u.f0[%u];\n        r2 = u.f1[%u];\n",
       (flags & 0x010u) ? s : 0u, (flags & 0x100u) ? s : 0u);
    if ((fl & 4u) || (afl & 4u))
        gb(b, "        bool mux = %s;\n", (flags & 1u) ? "r12.a >= 0.5" : "(int(r12.a * 255.0) & 1) != 0");
    gen_rgb(A, sizeof A, icw >> 24); gen_rgb(B, sizeof B, icw >> 16);
    gen_rgb(C, sizeof C, icw >> 8);  gen_rgb(D, sizeof D, icw);
    gb(b, "        vec3 A = %s, B = %s, C = %s, D = %s;\n", A, B, C, D);
    gen_alpha(A, sizeof A, aicw >> 24); gen_alpha(B, sizeof B, aicw >> 16);
    gen_alpha(C, sizeof C, aicw >> 8);  gen_alpha(D, sizeof D, aicw);
    gb(b, "        float aA = %s, aB = %s, aC = %s, aD = %s;\n", A, B, C, D);
    gb(b, "        vec3 pab = %s, pcd = %s;\n", (fl & 2u) ? "vec3(dot(A, B))" : "A * B",
       (fl & 1u) ? "vec3(dot(C, D))" : "C * D");
    gb(b, "        vec3 sum = %s;\n", (fl & 4u) ? "mux ? pcd : pab" : "pab + pcd");
    gen_map_out(t, sizeof t, "pab", fl & 0x38u); gb(b, "        vec3 ab = %s;\n", t);
    gen_map_out(t, sizeof t, "pcd", fl & 0x38u); gb(b, "        vec3 cd = %s;\n", t);
    gen_map_out(t, sizeof t, "sum", fl & 0x38u); gb(b, "        vec3 ms = %s;\n", t);
    gb(b, "        float apab = aA * aB, apcd = aC * aD;\n");
    gb(b, "        float asum = %s;\n", (afl & 4u) ? "mux ? apcd : apab" : "apab + apcd");
    gen_map_out(t, sizeof t, "apab", afl & 0x38u); gb(b, "        float aab = %s;\n", t);
    gen_map_out(t, sizeof t, "apcd", afl & 0x38u); gb(b, "        float acd = %s;\n", t);
    gen_map_out(t, sizeof t, "asum", afl & 0x38u); gb(b, "        float ams = %s;\n", t);
    if ((d = (ocw >> 4) & 15u) != 0) {
        gb(b, "        r%u.rgb = ab;\n", d);
        if (fl & 0x80u) gb(b, "        r%u.a = ab.b;\n", d);
    }
    if ((d = ocw & 15u) != 0) {
        gb(b, "        r%u.rgb = cd;\n", d);
        if (fl & 0x40u) gb(b, "        r%u.a = cd.b;\n", d);
    }
    if ((d = (ocw >> 8) & 15u) != 0) gb(b, "        r%u.rgb = ms;\n", d);
    if ((d = (aocw >> 4) & 15u) != 0) gb(b, "        r%u.a = aab;\n", d);
    if ((d = aocw & 15u) != 0) gb(b, "        r%u.a = acd;\n", d);
    if ((d = (aocw >> 8) & 15u) != 0) gb(b, "        r%u.a = ams;\n", d);
    gb(b, "    }\n");
}

/* The whole program for v, as Vulkan GLSL (gl_source makes it GL's). */
static char *spec_gen(const GSpec *v)
{
    GBuf b = { (char *)malloc(64 * 1024), 0, 64 * 1024 };
    uint32_t st, s;
    int fast = (int)v->fast;
    float vpw = v->vpw;
    gb(&b, "#version 450\n#include \"nv2a_common.glsl\"\n"
           "layout(set = 0, binding = 1) uniform sampler2D t2d[4];\n"
           "layout(set = 0, binding = 2) uniform samplerCube tcube[4];\n"
           "layout(location = 0) in vec4 v_d0;\nlayout(location = 1) in vec4 v_d1;\n"
           "layout(location = 2) in float v_fog;\nlayout(location = 3) in vec4 v_t0;\n"
           "layout(location = 4) in vec4 v_t1;\nlayout(location = 5) in vec4 v_t2;\n"
           "layout(location = 6) in vec4 v_t3;\n"
           "layout(location = 7) noperspective in float v_zs;\n"
           "layout(location = 0) out vec4 o_color;\n\nvoid main()\n{\n");
    if (!fast) {
        /* The depth, with the polygon offset, before any discard. */
        gb(&b, "    float zs = %s / u.vp.z;\n", vpw == 0.0f ? "gl_FragCoord.z" : "v_zs");
        gb(&b, "    float zraw = %s;\n", vpw != 0.0f ? "1.0 / gl_FragCoord.w" : "zs");
        gb(&b, "    if (u.zoff.x != 0.0 || u.zoff.y != 0.0) {\n"
               "        float dz = max(abs(dFdx(zs)) * u.zoff.z, abs(dFdy(zs)) * u.zoff.w);\n"
               "        float off = u.zoff.x * dz + u.zoff.y;\n%s"
               "        zraw = max(zraw + off, 0.0);\n    }\n",
           vpw != 0.0f ? "        off *= zraw / max(1.0 / u.vp.z - zs, 1.0);\n" : "");
    }
    gb(&b, "    vec4 tc0 = v_t0, tc1 = v_t1, tc2 = v_t2, tc3 = %s;\n",
       v->ptsz != 0.0f ? "vec4(gl_PointCoord, 1.0, 1.0)" : "v_t3");
    gb(&b, "    vec4 t0 = vec4(0.0), t1 = vec4(0.0), t2 = vec4(0.0), t3 = vec4(0.0);\n"
           "    float d0 = 0.0, d1 = 0.0, d2 = 0.0, d3 = 0.0;\n    vec4 outc;\n");
    for (st = 0; st < 4; st++)
        gen_stage(&b, v, st);
    if (!(v->misc_x & 1u)) {
        gb(&b, "    outc = %s;\n", (v->misc_x & 2u) ? "texture(t2d[0], v_t0.xy * u.ts[0].xy) * v_d0" : "v_d0");
    } else {
        uint32_t ctl = v->misc2[0], n = (ctl & 0xFFu) < 8u ? ctl & 0xFFu : 8u, flags = ctl >> 8;
        uint32_t f0 = v->misc2[1], f1 = v->misc2[2];
        gb(&b, "    vec4 r1 = vec4(0.0), r2 = vec4(0.0), r3 = vec4(u.fogc.rgb, v_fog), r4 = v_d0,"
               " r5 = v_d1, r6 = vec4(0.0), r7 = vec4(0.0);\n"
               "    vec4 r8 = t0, r9 = t1, r10 = t2, r11 = t3, r12 = vec4(0.0, 0.0, 0.0, %s),"
               " r13 = vec4(0.0), r14 = vec4(0.0), r15 = vec4(0.0);\n",
           (v->misc_w & 0x1Fu) ? "t0.a" : "1.0");
        for (s = 0; s < n; s++)
            gen_rc(&b, v, s, flags);
        if (f0 || f1) {
            uint32_t ff = f1 & 0xFFu;
            char A[96], B[96], C[96], D[96], E[96], F[96];
            gb(&b, "    {\n        r1 = u.fc0;\n        r2 = u.fc1;\n");
            gb(&b, "        vec3 a = %s, b = %s;\n", (ff & 0x40u) ? "1.0 - r5.rgb" : "r5.rgb",
               (ff & 0x20u) ? "1.0 - r12.rgb" : "r12.rgb");
            gb(&b, "        r14 = vec4(%s, 0.0);\n", (ff & 0x80u) ? "clamp(a + b, 0.0, 1.0)" : "a + b");
            gen_rgb(E, sizeof E, f1 >> 24); gen_rgb(F, sizeof F, f1 >> 16);
            gb(&b, "        r15 = vec4(%s * %s, 0.0);\n", E, F);
            gen_rgb(A, sizeof A, f0 >> 24); gen_rgb(B, sizeof B, f0 >> 16);
            gen_rgb(C, sizeof C, f0 >> 8);  gen_rgb(D, sizeof D, f0);
            gen_alpha(E, sizeof E, f1 >> 8);
            gb(&b, "        vec3 A = %s, B = %s, C = %s, D = %s;\n", A, B, C, D);
            gb(&b, "        outc = vec4(D + C * (1.0 - A) + B * A, %s);\n    }\n", E);
        } else {
            gb(&b, "    outc = r12;\n");
        }
    }
    gb(&b, "    outc = clamp(outc, 0.0, 1.0);\n");
    if (v->misc3[0]) {
        static const char *const cmp[] = { "<", "==", "<=", ">", "!=", ">=" };
        uint32_t f = v->misc_y;
        if (f == 0x200)
            gb(&b, "    discard;\n");
        else if (f >= 0x201 && f <= 0x206)
            gb(&b, "    if (!(int(outc.a * 255.0 + 0.5) %s int(u.misc.z & 0xFFu))) discard;\n",
               cmp[f - 0x201]);
    }
    gb(&b, "    o_color = outc;\n");
    if (!fast) {
        if (vpw == 2.0f)
            gb(&b, "    gl_FragDepth = min(WREV_C / max(zraw, 1e-30), 1.0);\n");
        else if (vpw != 0.0f)
            gb(&b, "    gl_FragDepth = zraw * u.vp.z;\n");
        else if (v->misc3[3])
            gb(&b, "    gl_FragDepth = roundEven(zraw) * u.vp.z;\n");
        else
            gb(&b, "    gl_FragDepth = (u.zoff.x != 0.0 || u.zoff.y != 0.0) ? zraw * u.vp.z"
                   " : gl_FragCoord.z;\n");
    }
    gb(&b, "}\n");
    return b.p;
}

static GLuint spec_build(const GSpec *v)
{
    char defs[2048], *q = defs, *src;
    GLuint p;
    int i;
    q += sprintf(q, "#define NV2A_SPEC 1\n%s", v->fast ? "#define NV2A_FAST_DEPTH 1\n" : "");
    q += sprintf(q, "#define SP_NST %u\n", (v->misc_x & 1u) ? (v->misc2[0] & 0xFFu) < 8u
                 ? (v->misc2[0] & 0xFFu) : 8u : 0u);
    for (i = 0; i < 4; i++)
        q += sprintf(q, "#define SP_TEX%d %d\n", i, ((v->misc_w >> (i * 5)) & 0x1Fu) != 0);
    q += sprintf(q, "const uvec4 SP_MISC = uvec4(%uu, %uu, 0u, %uu);\n", v->misc_x, v->misc_y, v->misc_w);
    q += sprintf(q, "const uvec4 SP_MISC2 = uvec4(%uu, %uu, %uu, %uu);\n",
                 v->misc2[0], v->misc2[1], v->misc2[2], v->misc2[3]);
    q += sprintf(q, "const uvec4 SP_MISC3 = uvec4(%uu, %uu, %uu, %uu);\n",
                 v->misc3[0], v->misc3[1], v->misc3[2], v->misc3[3]);
    q += sprintf(q, "const uvec4 SP_CW[8] = uvec4[8](");
    for (i = 0; i < 8; i++)
        q += sprintf(q, "uvec4(%uu, %uu, %uu, %uu)%s", v->cw[i][0], v->cw[i][1], v->cw[i][2],
                     v->cw[i][3], i < 7 ? ", " : ");\n");
    q += sprintf(q, "const float SP_PTSZ = %.9g;\nconst float SP_VPW = %.9g;\n", v->ptsz, v->vpw);
    sprintf(q, "const float SP_TSZ[4] = float[4](%.9g, %.9g, %.9g, %.9g);\n",
            v->tsz[0], v->tsz[1], v->tsz[2], v->tsz[3]);
    {
        static int gen = -1;
        if (gen < 0) {
            const char *e = getenv("RECOMP_GL_SPEC_GEN");
            gen = !(e && strcmp(e, "0") == 0);
        }
        if (gen) {
            char *g = spec_gen(v);
            src = g ? gl_source(g, "") : NULL;
            free(g);
            if (src && getenv("RECOMP_GL_SHADER_DUMP"))
                fprintf(stderr, "[GL] generated fragment program:\n%s\n", src);
        } else {
            src = gl_source(k_nv2a_frag_src, defs);
        }
    }
    p = src ? sep_program(GL_FRAGMENT_SHADER, src) : 0;
    free(src);
    return p;
}

/* Compiling off the render thread: on the Switch, Mesa takes 0.6-5 s over
 * a specialised program, and every new combiner setup froze the game that
 * long. A thread with its own context, sharing the render context's
 * objects, builds them and fences each; draws use the general program until
 * theirs has passed its fence. On the Switch the context is made through
 * EGL with no surface (EGL lets a window surface be current on one thread
 * only); elsewhere through SDL, on a hidden window of its own. Off on the
 * Switch unless RECOMP_GL_SPEC_THREAD=1: nouveau is not safe with two
 * contexts in use at once (the car select drew white, then the game
 * exited), and sharing the cores with the game a compile took 3-4 s there.
 * RECOMP_GL_SPEC_THREAD=0
 * compiles on the render thread instead, a few a frame. */
#define GL_SC_JOBS 256
static struct {
    int on;                         /* 1 running, 0 not (compile inline) */
    volatile int ready;             /* the worker's start: 1 ok, -1 failed */
    struct { GFrag *e; GSpec v; } q[GL_SC_JOBS];
    unsigned head, tail;
    SDL_mutex *lock;
    SDL_cond *cond;
#if defined(__SWITCH__)
    EGLDisplay dpy;
    EGLContext ctx;
#else
    SDL_Window *win;                /* hidden, the worker's own */
    SDL_GLContext ctx;
#endif
} SC;

static int sc_current(void)
{
#if defined(__SWITCH__)
    return eglBindAPI(EGL_OPENGL_API) && eglMakeCurrent(SC.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, SC.ctx);
#else
    return SDL_GL_MakeCurrent(SC.win, SC.ctx) == 0;
#endif
}

static DWORD WINAPI sc_worker(LPVOID unused)
{
    (void)unused;
    if (!sc_current()) {
#if defined(__SWITCH__)
        fprintf(stderr, "[GL] shader thread: make current failed (EGL 0x%x)\n", eglGetError());
#else
        fprintf(stderr, "[GL] shader thread: make current failed (%s)\n", SDL_GetError());
#endif
        __atomic_store_n(&SC.ready, -1, __ATOMIC_RELEASE);
        return 0;
    }
    __atomic_store_n(&SC.ready, 1, __ATOMIC_RELEASE);
    for (;;) {
        GFrag *e;
        GSpec v;
        uint64_t t0;
        SDL_LockMutex(SC.lock);
        while (SC.head == SC.tail)
            SDL_CondWait(SC.cond, SC.lock);
        e = SC.q[SC.tail % GL_SC_JOBS].e;
        v = SC.q[SC.tail % GL_SC_JOBS].v;
        SC.tail++;
        SDL_UnlockMutex(SC.lock);
        t0 = now_us();
        e->prog = spec_build(&v);
        if (e->prog) {
            e->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
        }
        __atomic_add_fetch(&G.pf_spec_us, now_us() - t0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&G.pf_spec, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&e->state, e->prog ? 2 : -1, __ATOMIC_RELEASE);
    }
    return 0;
}

/* On the render thread, its context current: the worker's context and the
 * worker. */
static void sc_init(void)
{
    const char *env = getenv("RECOMP_GL_SPEC_THREAD");
    HANDLE th;
    uint64_t t0;
#if defined(__SWITCH__)
    if (!(env && strcmp(env, "0") != 0) || !spec_on())
        return;
#else
    if ((env && strcmp(env, "0") == 0) || !spec_on())
        return;
#endif
#if defined(__SWITCH__)
    {
        EGLint id = 0, n = 0;
        EGLConfig cfg;
        EGLint pick[] = { EGL_CONFIG_ID, 0, EGL_NONE };
        static const EGLint attrs[] = {
            EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
            EGL_NONE };
        EGLContext main_ctx = eglGetCurrentContext();
        SC.dpy = eglGetCurrentDisplay();
        if (!eglQueryContext(SC.dpy, main_ctx, EGL_CONFIG_ID, &id))
            return;
        pick[1] = id;
        if (!eglChooseConfig(SC.dpy, pick, &cfg, 1, &n) || n != 1)
            return;
        SC.ctx = eglCreateContext(SC.dpy, cfg, main_ctx, attrs);
        if (SC.ctx == EGL_NO_CONTEXT) {
            fprintf(stderr, "[GL] shader thread: no context (EGL 0x%x)\n", eglGetError());
            return;
        }
    }
#else
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SC.win = SDL_CreateWindow("rsc2 (GL shaders)", 0, 0, 16, 16,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SC.ctx = SC.win ? SDL_GL_CreateContext(SC.win) : NULL;
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GL_MakeCurrent(G.win, G.ctx);
    if (!SC.ctx) {
        fprintf(stderr, "[GL] shader thread: no context (%s)\n", SDL_GetError());
        return;
    }
#endif
    SC.lock = SDL_CreateMutex();
    SC.cond = SDL_CreateCond();
    th = SC.lock && SC.cond ? CreateThread(NULL, 8u << 20, sc_worker, NULL, 0, NULL) : NULL;
    if (!th)
        return;
    CloseHandle(th);
    for (t0 = now_us(); !__atomic_load_n(&SC.ready, __ATOMIC_ACQUIRE) && now_us() - t0 < 2000000; )
        SDL_Delay(1);
    SC.on = __atomic_load_n(&SC.ready, __ATOMIC_ACQUIRE) == 1;
    fprintf(stderr, "[GL] specialised shaders compile %s\n",
            SC.on ? "on their own thread" : "on the render thread (no second context)");
}

static int sc_queue(GFrag *e, const GSpec *v)
{
    int ok = 0;
    SDL_LockMutex(SC.lock);
    if (SC.head - SC.tail < GL_SC_JOBS) {
        SC.q[SC.head % GL_SC_JOBS].e = e;
        SC.q[SC.head % GL_SC_JOBS].v = *v;
        SC.head++;
        e->state = 1;
        ok = 1;
        SDL_CondSignal(SC.cond);
    }
    SDL_UnlockMutex(SC.lock);
    return ok;
}

/* The fragment program for a draw with uniforms ub. */
static GLuint frag_get(const Nv2aVkState *st, const Nv2aVkUbo *ub)
{
    int fast = !needs_fragdepth(st);
    GSpec v;
    GFrag *e = NULL;
    uint64_t h;
    uint32_t slot, n;

    G.pf_fragz[3]++;
    if (!fast)
        G.pf_fragz[st->poff_factor != 0.0f || st->poff_units != 0.0f ? 0 : st->zint ? 1 : 2]++;
    if (!spec_on())
        return G.fs_gen[fast];
    spec_vals(&v, ub, fast);
    h = hash_bytes((const uint8_t *)&v, sizeof v) | 1;
    slot = (uint32_t)(h >> 7) & (GL_MAX_FRAG - 1);
    for (n = 0; n < GL_MAX_FRAG; n++) {
        GFrag *c = &G.frag[(slot + n) & (GL_MAX_FRAG - 1)];
        if (c->h == h || !c->h) {
            e = c;
            break;
        }
    }
    if (e && e->h == h) {
        if (__atomic_load_n(&e->state, __ATOMIC_ACQUIRE) == 2) {
            if (e->fence) {
                if (glClientWaitSync(e->fence, 0, 0) == GL_TIMEOUT_EXPIRED) {
                    G.pf_spec_gen++;
                    return G.fs_gen[fast];
                }
                glDeleteSync(e->fence);
                e->fence = 0;
            }
            return e->prog;
        }
        G.pf_spec_gen++;                /* queued, or failed */
        return G.fs_gen[fast];
    }
    G.pf_spec_gen++;
    if (!e)
        return G.fs_gen[fast];
    if (SC.on) {
        if (sc_queue(e, &v))
            e->h = h;
        return G.fs_gen[fast];
    }
    if (G.spec_left <= 0)
        return G.fs_gen[fast];
    G.spec_left--;
    e->h = h;
    {
        uint64_t t0 = perf_on() ? now_us() : 0;
        e->prog = spec_build(&v);
        if (t0)
            G.pf_spec_us += now_us() - t0;
    }
    G.pf_spec++;
    e->state = e->prog ? 2 : -1;
    return e->prog ? e->prog : G.fs_gen[fast];
}

static GProg *prog_get(uint32_t hash)
{
    int i;
    GProg *p;
    for (i = 0; i < G.nprog; i++)
        if (G.prog[i].hash == hash)
            return &G.prog[i];
    if (G.nprog == GL_MAX_PROG)
        return NULL;
    p = &G.prog[G.nprog++];
    memset(p, 0, sizeof *p);
    p->hash = hash;
    step("program", hash, 0);
    if (hash == 0) {
        char *vs = gl_source(k_nv2a_vert_src, "");
        p->prog = vs ? sep_program(GL_VERTEX_SHADER, vs) : 0;
        p->ninputs = 8;
        free(vs);
    } else {
        static char body[256 * 1024];
        uint32_t inmask = 0, k, j;
        char *src, *q, *gl;
        if (!nv2a_vsh_to_glsl(body, sizeof body, &inmask)) {
            p->failed = 1;
            return p;
        }
        src = (char *)malloc(strlen(nv2a_prog_head) + strlen(nv2a_prog_main)
                             + strlen(nv2a_prog_tail) + strlen(body) + 2048);
        if (!src) {
            p->failed = 1;
            return p;
        }
        q = src + sprintf(src, "%s", nv2a_prog_head);
        for (k = 0, j = 0; k < 16; k++)
            if (inmask & (1u << k)) {
                q += sprintf(q, "layout(location = %u) in vec4 v%u;\n", j, k);
                p->inputs[j++] = (uint8_t)k;
            }
        p->ninputs = (int)j;
        sprintf(q, "%s%s%s", nv2a_prog_main, body, nv2a_prog_tail);
        gl = gl_source(src, "");
        free(src);
        p->prog = gl ? sep_program(GL_VERTEX_SHADER, gl) : 0;
        free(gl);
    }
    if (!p->prog)
        p->failed = 1;
    return p;
}

/* ---- init -------------------------------------------------------------- */

static GLuint white_tex(GLenum target)
{
    static const uint32_t px = 0xFFFFFFFFu;
    GLuint t;
    int f;
    glGenTextures(1, &t);
    bind_other(target, t);
    if (target == GL_TEXTURE_CUBE_MAP)
        for (f = 0; f < 6; f++)
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0, GL_BGRA,
                         GL_UNSIGNED_BYTE, &px);
    else
        glTexImage2D(target, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &px);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
    return t;
}

static int has_ext(const char *name)
{
    GLint n = 0, k;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (k = 0; k < n; k++)
        if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)k), name))
            return 1;
    return 0;
}

static int gl_init(void)
{
    SDL_Window *win = (SDL_Window *)__atomic_load_n(&s_handed_win, __ATOMIC_ACQUIRE);
    int k;

    if (win) {
        G.win = win;
        G.ctx = (SDL_GLContext)s_handed_ctx;
    } else {
        if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "[GL] SDL video: %s\n", SDL_GetError());
            return 0;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        G.win = SDL_CreateWindow("rsc2 (GL, hidden)", 0, 0, 64, 64,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        G.ctx = G.win ? SDL_GL_CreateContext(G.win) : NULL;
        G.own_win = 1;
        if (!G.ctx) {
            fprintf(stderr, "[GL] no context: %s\n", SDL_GetError());
            return 0;
        }
    }
    if (SDL_GL_MakeCurrent(G.win, G.ctx) != 0) {
        fprintf(stderr, "[GL] make current: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_SetSwapInterval(0);           /* the executor paces the flips */
    if (!gl_load())
        return 0;
    fprintf(stderr, "[GL] %s, %s (%s)\n", (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER), G.own_win ? "offscreen" : "window");
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_SCISSOR_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGenFramebuffers(1, &G.fbo);
    glGenFramebuffers(1, &G.rfbo);
    glGenVertexArrays(1, &G.vao);
    glBindVertexArray(G.vao);
    glGenBuffers(1, &G.vbo);
    glGenBuffers(1, &G.ibo);
    glGenBuffers(1, &G.ubo);
    glGenBuffers(1, &G.cbo);
    glGenBuffers(1, &G.pbo);
    glBindBuffer(GL_UNIFORM_BUFFER, G.ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(Nv2aVkUbo), NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, G.cbo);
    glBufferData(GL_UNIFORM_BUFFER, NV2A_VSH_CONSTANTS * 16, NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, G.ubo);
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, G.cbo);
    {
        GLint a = 256;
        glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &a);
        G.ubo_align = a > 16 ? (size_t)a : 16;
    }
    glGenBuffers(1, &G.ring);
    glBindBuffer(GL_ARRAY_BUFFER, G.ring);
    {
        /* Mapped once and kept mapped where the driver can
         * (GL_ARB_buffer_storage): a map and unmap per draw was Mesa
         * validation work on every draw. RECOMP_GL_PERSIST=0 maps per draw. */
        typedef void (APIENTRYP storage_fn)(GLenum, GLsizeiptr, const void *, GLbitfield);
        const char *e = getenv("RECOMP_GL_PERSIST");
        storage_fn bs = (e && strcmp(e, "0") == 0) || !has_ext("GL_ARB_buffer_storage") ? NULL
                      : (storage_fn)SDL_GL_GetProcAddress("glBufferStorage");
        GLbitfield fl = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        if (bs) {
            bs(GL_ARRAY_BUFFER, GL_RING_SIZE, NULL, fl);
            G.ring_map = (uint8_t *)glMapBufferRange(GL_ARRAY_BUFFER, 0, GL_RING_SIZE, fl);
        }
        if (!G.ring_map) {
            if (bs) {                   /* storage is immutable: start again */
                glDeleteBuffers(1, &G.ring);
                glGenBuffers(1, &G.ring);
                glBindBuffer(GL_ARRAY_BUFFER, G.ring);
            }
            glBufferData(GL_ARRAY_BUFFER, GL_RING_SIZE, NULL, GL_STREAM_DRAW);
        }
        fprintf(stderr, "[GL] draw data ring %s\n", G.ring_map ? "mapped persistently"
                : "mapped per draw");
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, G.ring);      /* the VAO's, for good */
    /* Every attribute a vec4 at 16 bytes a step within a vertex, from one
     * binding whose offset and stride move per draw (glBindVertexBuffer):
     * one call a draw instead of a glVertexAttribPointer per attribute,
     * each making Mesa look at the vertex layout again. */
    for (k = 0; k < 16; k++) {
        glVertexAttribFormat((GLuint)k, 4, GL_FLOAT, GL_FALSE, (GLuint)k * 16);
        glVertexAttribBinding((GLuint)k, 0);
    }
    G.b_ninputs = -1;
    G.white2d = white_tex(GL_TEXTURE_2D);
    G.whitecube = white_tex(GL_TEXTURE_CUBE_MAP);
    G.tex = (GTex *)calloc(GL_MAX_TEX, sizeof(GTex));
    for (k = 0; k < 2; k++) {
        G.qpool[k] = (GLuint *)calloc(GL_MAX_QUERIES, sizeof(GLuint));
        glGenQueries(GL_MAX_QUERIES, G.qpool[k]);
    }
    G.queries = G.qpool[0];
    glGenBuffers(1, &G.pend.pbo);
    gt_init();
    G.clear_q = -1;
    G.epoch = 1;
    {
        /* DXT as S3TC (GL_EXT_texture_compression_s3tc): the GPU reads the
         * blocks as the title stored them. RECOMP_GL_BC=0 decodes them on
         * the CPU instead, as before. */
        const char *e = getenv("RECOMP_GL_BC");
        G.bc_ok = !(e && strcmp(e, "0") == 0) && has_ext("GL_EXT_texture_compression_s3tc");
        fprintf(stderr, "[GL] DXT textures %s\n", G.bc_ok ? "uploaded compressed"
                : "decoded on the CPU");
    }
    {
        uint64_t t0 = now_us();
        GLint nbin = 0;
        G.fs_gen[0] = frag_general(0);
        G.fs_gen[1] = frag_general(1);
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nbin);
        fprintf(stderr, "[GL] general fragment programs built in %.0f ms; %d program binary"
                " format(s)\n", (now_us() - t0) / 1000.0, (int)nbin);
    }
    if (!G.fs_gen[0] || !G.fs_gen[1] || !prog_get(0)->prog) {
        fprintf(stderr, "[GL] the general programs did not build; renderer off\n");
        return 0;
    }
    glGenProgramPipelines(1, &G.pipe);
    glBindProgramPipeline(G.pipe);
    G.spec_left = spec_per_frame();
    sc_init();
    return 1;
}

int nv2a_vk_active(void)
{
    if (G.state == 0) {
        if (!nv2a_gl_wanted()) {
            G.state = -1;
            return 0;
        }
        /* The context is the window's: until fb_present.c has made it (its
         * thread starts at the title's first clear), not yet -- the frames
         * before go to the software rasteriser. Without a window, our own. */
        if (getenv("RECOMP_FB_WINDOW") && *getenv("RECOMP_FB_WINDOW") != '0'
                && !__atomic_load_n(&s_handed_win, __ATOMIC_ACQUIRE))
            return 0;
        G.state = gl_init() ? 1 : -1;
    }
    return G.state > 0;
}

/* ---- surfaces ---------------------------------------------------------- */

static GLenum surf_ifmt(uint32_t bpp, uint32_t color_fmt)
{
    if (bpp == 4)
        return GL_RGBA8;
    if (bpp == 2)
        return (color_fmt == 1 || color_fmt == 2) ? GL_RGB5_A1 : GL_RGB565;
    return 0;
}

static void surf_pixfmt(const GSurf *s, GLenum *fmt, GLenum *type)
{
    if (s->bpp == 4) {
        *fmt = GL_BGRA;
        *type = GL_UNSIGNED_INT_8_8_8_8_REV;
    } else if (s->fmt == GL_RGB5_A1) {
        *fmt = GL_BGRA;
        *type = GL_UNSIGNED_SHORT_1_5_5_5_REV;
    } else {
        *fmt = GL_RGB;
        *type = GL_UNSIGNED_SHORT_5_6_5;
    }
}

/* Between a linear w-wide image and the surface's swizzled guest bytes. */
static void surf_swizzle(GSurf *s, uint8_t *g, uint8_t *lin, uint32_t rows, int to_guest)
{
    uint32_t x, y;
    if (!s->swx) {
        uint32_t mx, my;
        s->swx = (uint32_t *)malloc(sizeof(uint32_t) * s->w);
        s->swy = (uint32_t *)malloc(sizeof(uint32_t) * s->h);
        xbox_swizzle_masks(s->w, s->h, &mx, &my);
        for (x = 0; x < s->w; x++) s->swx[x] = swizzle_deposit(x, mx);
        for (y = 0; y < s->h; y++) s->swy[y] = swizzle_deposit(y, my);
    }
    for (y = 0; y < rows; y++) {
        uint8_t *row = lin + (size_t)y * s->w * s->bpp;
        for (x = 0; x < s->w; x++) {
            uint32_t gi = s->swy[y] | s->swx[x];
            if (to_guest)
                memcpy(g + (size_t)gi * s->bpp, row + (size_t)x * s->bpp, s->bpp);
            else
                memcpy(row + (size_t)x * s->bpp, g + (size_t)gi * s->bpp, s->bpp);
        }
    }
}

static size_t surf_bytes(const GSurf *s)
{
    return s->swz ? (size_t)s->w * s->h * s->bpp : (size_t)s->rows * s->pitch;
}

static uint8_t *scratch(size_t n)
{
    if (n > G.rb_size) {
        free(G.rb);
        G.rb = (uint8_t *)malloc(n);
        G.rb_size = G.rb ? n : 0;
    }
    return G.rb;
}

static int tex_hash_sparse(void);
#define GL_TEX_SPARSE_MIN   (64u << 10)
#define GL_TEX_FULL_EPOCHS  30u

/* The texture now matches guest RAM: remember what guest RAM holds. */
static void surf_ref(GSurf *s)
{
    size_t n = surf_bytes(s);
    s->ref = hash_bytes(guest(s->va), n);
    s->sref = hash_sparse(guest(s->va), n);
    s->ref_ep = s->ref_full_ep = G.epoch;
    s->ref_valid = 1;
}

/* 1 while guest RAM still holds what the texture was matched to, checked at
 * most once an epoch (sparsely, as textures are, where they are). Every
 * surface used to be uploaded again each epoch: 1.2 MB of copying and, for a
 * swizzled one, unswizzling for each, even straight after its write-back. */
static int surf_unchanged(GSurf *s)
{
    size_t n = surf_bytes(s);
    if (!s->ref_valid)
        return 0;
    if (s->ref_ep == G.epoch)
        return 1;
    if (tex_hash_sparse() && n >= GL_TEX_SPARSE_MIN && G.epoch - s->ref_full_ep < GL_TEX_FULL_EPOCHS
            && (G.epoch + (uint32_t)(s - G.surf)) % GL_TEX_FULL_EPOCHS != 0) {
        if (hash_sparse(guest(s->va), n) != s->sref)
            return 0;
    } else {
        if (hash_bytes(guest(s->va), n) != s->ref)
            return 0;
        s->ref_full_ep = G.epoch;
    }
    s->ref_ep = G.epoch;
    return 1;
}

/* Guest RAM into the surface's texture (the rows the title uses). */
static void surf_upload(GSurf *s)
{
    uint32_t rows = s->swz ? s->h : (s->rows < s->h ? s->rows : s->h), y;
    uint8_t *lin;
    GLenum fmt, type;
    s->up_epoch = G.epoch;
    s->gen++;
    if (!rows)
        return;
    lin = scratch((size_t)s->w * rows * s->bpp);
    if (!lin)
        return;
    if (s->swz)
        surf_swizzle(s, guest(s->va), lin, rows, 0);
    else
        for (y = 0; y < rows; y++)
            memcpy(lin + (size_t)y * s->w * s->bpp, guest(s->va) + (size_t)y * s->pitch,
                   (size_t)s->w * s->bpp);
    surf_pixfmt(s, &fmt, &type);
    G.pf_surf_up++;
    {
        static int trace = -1;
        if (trace < 0)
            trace = getenv("RECOMP_GL_UPLOAD_TRACE") != NULL;
        if (trace && (G.frame % 60) == 0)
            fprintf(stderr, "[GL] F%u upload surface %08X %ux%u bpp %u%s rows %u ref %d\n",
                    G.frame, s->va, s->w, s->h, s->bpp, s->swz ? " swizzled" : "", rows,
                    s->ref_valid);
    }
    step("upload", s->va, s->fmt);
    {
        GtLabel prev = gt_push(GT_SURF);
        bind_other(GL_TEXTURE_2D, s->tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)s->w, (GLsizei)rows, fmt, type, lin);
        gt_pop(prev);
    }
    surf_ref(s);
}

static void tex_touch(uint32_t va, size_t bytes);
static GLuint tex_upload(const Nv2aVkTex *dp, uint32_t bytes, struct GTex_ *e, uint64_t h,
                         uint32_t *levels_out);
static void reports_resolve(void);
static void flush_pipelined(void);

/* RECOMP_GL_FLIP_WRITEBACK=0 (the Switch's default): surfaces never shown
 * (shadow maps, the reflection's faces) are not copied back at flips
 * (flush_pipelined): they stay dirty, and a texture over them still has
 * them copied back when it is drawn with. Only a CPU read of them would
 * miss it. The Switch's Mesa reads pixels into a buffer on the CPU, waiting
 * for the frame: a third of a second a frame in a race. */
static int flip_writeback(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_FLIP_WRITEBACK");
#if defined(__SWITCH__)
        on = e && strcmp(e, "0") != 0;
#else
        on = !(e && strcmp(e, "0") == 0);
#endif
    }
    return on;
}

/* The surface's texture into guest RAM. */
static const char *wb_why = "?";
static void pend_retire_inner(void);
/* RECOMP_GL_SYNC_TRACE: who lands the flipped frame early (the source line;
 * the flip's own is flush_pipelined's). */
#define pend_retire() do { \
        static int t_ = -1; \
        if (t_ < 0) t_ = getenv("RECOMP_GL_SYNC_TRACE") != NULL; \
        if (t_ && G.pend.active && (G.frame % 60) == 0) \
            fprintf(stderr, "[GLSYNC] F%u line %d (%s)\n", G.frame, __LINE__, wb_why); \
        pend_retire_inner(); \
    } while (0)

/* The rows of s that guest RAM holds. */
static uint32_t surf_wb_rows(const GSurf *s)
{
    return s->swz ? s->h : (s->rows < s->h ? s->rows : s->h);
}

/* Copy s's texture into the bound pixel pack buffer at off. */
static void surf_readpix(GSurf *s, uint32_t rows, size_t off)
{
    GLenum fmt, type;
    surf_pixfmt(s, &fmt, &type);
    step("write-back", s->va, s->fmt);
    {
        static int trace = -1;
        if (trace < 0)
            trace = getenv("RECOMP_GL_UPLOAD_TRACE") != NULL;
        if (trace && (G.frame % 60) == 0)
            fprintf(stderr, "[GL] F%u write-back (%s) surface %08X %ux%u bpp %u%s rows %u\n",
                    G.frame, wb_why, s->va, s->w, s->h, s->bpp, s->swz ? " swizzled" : "", rows);
    }
    {
        GtLabel prev = gt_push(GT_READ);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, G.rfbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, (GLsizei)s->w, (GLsizei)rows, fmt, type, (void *)(uintptr_t)off);
        gt_pop(prev);
    }
}

/* Read-back pixels of s (rows of w * bpp bytes) into guest RAM. `clean`:
 * they are what the texture holds now, so s is no longer dirty. */
static void surf_land(GSurf *s, uint8_t *lin, uint32_t rows, int clean)
{
    uint32_t y;
    {
        static int trace = -1;
        if (trace < 0)
            trace = getenv("RECOMP_GL_TRACE") != NULL;
        if (trace && (G.frame % 30) == 0) {
            size_t k, nz = 0, n = (size_t)s->w * rows * s->bpp;
            for (k = 0; k < n; k += s->bpp)
                nz += lin[k] | lin[k + 1] | (s->bpp == 4 ? lin[k + 2] : 0);
            fprintf(stderr, "[GL] F%u write-back %08X %ux%u: %zu non-black\n",
                    nv2a_pb_presented_frames(), s->va, s->w, rows, nz);
        }
    }
    if (s->swz)
        surf_swizzle(s, guest(s->va), lin, rows, 1);
    else
        for (y = 0; y < rows; y++)
            memcpy(guest(s->va) + (size_t)y * s->pitch, lin + (size_t)y * s->w * s->bpp,
                   (size_t)s->w * s->bpp);
    if (clean) {
        s->dirty = 0;
        /* What is in guest RAM now is what the texture holds. */
        s->up_epoch = G.epoch;
        surf_ref(s);
    }
    tex_touch(s->va, surf_bytes(s));
}

/* The GPU's contents of s into guest RAM, now: waits for the GPU. */
static void surf_writeback(GSurf *s)
{
    uint32_t rows = surf_wb_rows(s);
    uint8_t *lin;
    uint64_t t0;
    size_t n;
    const void *m;

    /* A flipped frame's copy-back of it, landed first, may be all it needs:
     * that frame has had a frame's time to finish. */
    if (s->dirty == 2) {
        static int t_ = -1;
        if (t_ < 0) t_ = getenv("RECOMP_GL_SYNC_TRACE") != NULL;
        if (t_ && G.pend.active && (G.frame % 60) == 0)
            fprintf(stderr, "[GLSYNC] F%u for %08X %ux%u%s\n", G.frame, s->va, s->w, s->h,
                    s->swz ? " swizzled" : "");
        pend_retire();
    }
    if (!s->dirty || !rows)
        return;
    /* Kept dirty across flips, and since then the title has written over
     * its memory -- freed it and loaded something else there (the menu's
     * targets under the track's tables at Start Race): the title's bytes
     * win, as they would on the console. */
    if (s->dref_valid && hash_sparse(guest(s->va), surf_bytes(s)) != s->dref) {
        s->dirty = 0;
        s->clear_only = 0;
        s->ref_valid = 0;
        s->dref_valid = 0;
        return;
    }
    n = (size_t)s->w * rows * s->bpp;
    lin = scratch(n);
    if (!lin)
        return;
    pend_retire();      /* lands before this, so never over it */
    t0 = perf_on() ? now_us() : 0;
    G.pf_wb++;
    /* Through a pixel buffer: the GPU copies into it and it is mapped
     * here. (Read straight into client memory, Ryujinx's emulated GPU
     * took the emulator down with an X BadMatch.) */
    glBindBuffer(GL_PIXEL_PACK_BUFFER, G.pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)n, NULL, GL_STREAM_READ);
    surf_readpix(s, rows, 0);
    m = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)n, GL_MAP_READ_BIT);
    if (m)
        memcpy(lin, m, n);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    surf_land(s, lin, rows, 1);
    if (t0)
        G.pf_wb_us += now_us() - t0;
}

/* Copy back every dirty surface that overlaps [va, va + bytes). */
static void writeback_overlap(uint32_t va, uint32_t bytes)
{
    int i;
    for (i = 0; i < G.nsurf; i++) {
        GSurf *s = &G.surf[i];
        if (s->dirty && va < s->va + surf_bytes(s) && s->va < va + bytes)
            wb_why = "overlap", surf_writeback(s);
    }
}

/* Another surface over cs's bytes that the GPU has and guest RAM has not.
 * D3D clears a swizzled render target through a linear surface on the same
 * memory and then draws swizzled: both were copied back every frame, one
 * over the other, and the loser uploaded again the next (two of them in a
 * race). A surface that holds nothing but a full clear over exactly cs's
 * bytes hands cs the clear on the GPU instead, as the Vulkan renderer does;
 * any other overlap is copied back first, so cs's upload sees it. */
static void alias_resolve(GSurf *cs)
{
    int i;
    for (i = 0; i < G.nsurf; i++) {
        GSurf *q = &G.surf[i];
        uint32_t c = 0;
        if (q == cs || !q->dirty || !(cs->va < q->va + surf_bytes(q) && cs->va + surf_bytes(cs) > q->va))
            continue;
        if (q->clear_only && q->va == cs->va && surf_bytes(q) == surf_bytes(cs)
                && q->bpp == cs->bpp && q->fmt == cs->fmt) {
            c = q->clear_argb;
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, G.rfbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cs->tex, 0);
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            G.state_valid = 0;
            glClearColor((float)((c >> 16) & 0xFF) / 255.0f, (float)((c >> 8) & 0xFF) / 255.0f,
                         (float)(c & 0xFF) / 255.0f, (float)(c >> 24) / 255.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            G.cur_cs = NULL;            /* the draw framebuffer binding moved */
            G.cur_zs = NULL;
            cs->dirty = 1;
            cs->gen++;
            cs->pending_up = 0;
            cs->clear_only = 1;
            cs->clear_argb = c;
            if (!cs->swz && cs->rows < q->rows)
                cs->rows = q->rows;
            q->dirty = 0;
            q->clear_only = 0;
            q->ref_valid = 0;           /* guest RAM gets cs's bytes, not q's */
            continue;
        }
        {
            static int trace = -1;
            if (trace < 0)
                trace = getenv("RECOMP_GL_UPLOAD_TRACE") != NULL;
            if (trace && (G.frame % 60) == 0)
                fprintf(stderr, "[GL] F%u alias: target %08X %ux%u pitch %u bpp %u%s rows %u over %08X\n",
                        G.frame, cs->va, cs->w, cs->h, cs->pitch, cs->bpp, cs->swz ? " swizzled" : "",
                        cs->rows, q->va);
        }
        wb_why = "alias", surf_writeback(q);
    }
}

static GSurf *surf_get(const Nv2aVkState *st)
{
    GLenum ifmt = surf_ifmt(st->bpp, st->color_fmt);
    uint32_t w, rows = st->clip_y + st->clip_h;
    int swz = st->swz_w != 0, i;
    GSurf *s = NULL;

    if (!ifmt || !st->color_va || (!st->pitch && !swz))
        return NULL;
    w = swz ? st->swz_w : st->pitch / st->bpp;
    if (!w || w > 4096)
        return NULL;
    if (swz)
        rows = st->swz_h;
    if (rows > GL_SURF_H)
        rows = GL_SURF_H;
    {
        extern int xbox_VaReadable(uint32_t va);
        uint32_t bytes = swz ? w * st->swz_h * st->bpp : rows * st->pitch;
        if (!xbox_VaReadable(st->color_va) || !xbox_VaReadable(st->color_va + bytes - 4))
            return NULL;
    }
    for (i = 0; i < G.nsurf; i++)
        if (G.surf[i].va == st->color_va && G.surf[i].bpp == st->bpp && G.surf[i].fmt == ifmt
                && G.surf[i].swz == swz
                && (swz ? G.surf[i].w == w && G.surf[i].h == st->swz_h
                        : G.surf[i].pitch == st->pitch)) {
            s = &G.surf[i];
            break;
        }
    if (!s) {
        if (G.nsurf == GL_MAX_SURF) {
            int best = -1;
            for (i = 0; i < G.nsurf; i++)
                if (!G.surf[i].dirty && &G.surf[i] != G.cur_cs
                        && (best < 0 || G.surf[i].last_use < G.surf[best].last_use))
                    best = i;
            if (best < 0)
                return NULL;
            s = &G.surf[best];
            glDeleteTextures(1, &s->tex);
            free(s->swx);
            free(s->swy);
        } else {
            s = &G.surf[G.nsurf++];
        }
        memset(s, 0, sizeof *s);
        step("surface", st->color_va, ifmt);
        s->va = st->color_va;
        s->pitch = st->pitch;
        s->bpp = st->bpp;
        s->fmt = ifmt;
        s->swz = swz;
        s->w = w;
        s->h = swz ? st->swz_h : GL_SURF_H;
        glGenTextures(1, &s->tex);
        bind_other(GL_TEXTURE_2D, s->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, (GLsizei)s->w, (GLsizei)s->h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    if (rows > s->rows) {
        s->rows = rows;
        s->ref_valid = 0;            /* more rows than the reference covers */
    }
    s->last_use = G.frame;
    alias_resolve(s);
    if (!s->dirty && !s->pending_up && s->up_epoch != G.epoch && !surf_unchanged(s))
        s->pending_up = 1;
    return s;
}

static GZeta *zeta_get(uint32_t key, uint32_t w)
{
    int i;
    GZeta *z;
    if (!key)
        return NULL;
    if (w < 640)
        w = 640;
    for (i = 0; i < G.nzeta; i++)
        if (G.zeta[i].key == key && G.zeta[i].w >= w) {
            G.zeta[i].last_use = G.frame;
            return &G.zeta[i];
        }
    if (G.nzeta == GL_MAX_ZETA) {
        int best = 0;
        for (i = 1; i < G.nzeta; i++)
            if (G.zeta[i].last_use < G.zeta[best].last_use)
                best = i;
        z = &G.zeta[best];
        glDeleteRenderbuffers(1, &z->rb);
    } else {
        z = &G.zeta[G.nzeta++];
    }
    memset(z, 0, sizeof *z);
    step("zeta", key, w);
    z->key = key;
    z->w = w;
    z->h = GL_SURF_H;
    glGenRenderbuffers(1, &z->rb);
    glBindRenderbuffer(GL_RENDERBUFFER, z->rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, (GLsizei)z->w, (GLsizei)z->h);
    /* New storage is undefined: start it far and unstencilled. */
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, G.fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, z->rb);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    G.state_valid = 0;
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    G.cur_cs = NULL;                /* the framebuffer's attachments moved */
    G.cur_zs = NULL;
    z->last_use = G.frame;
    return z;
}

/* The target is about to be drawn into (not wholly cleared): bring in
 * what guest RAM has first, if it changed. */
static void surf_settle(GSurf *s)
{
    if (s && s->pending_up) {
        s->pending_up = 0;
        surf_upload(s);
    }
}

/* Bind the draw's surfaces. 0 when there is nothing to draw into. */
/* Bind st's colour surface and depth buffer to draw into; `colour` 0: the
 * depth buffer alone (a clear of depth and stencil only). */
static int begin_target(const Nv2aVkState *st, int colour)
{
    GSurf *cs = colour ? surf_get(st) : NULL;
    /* The width the colour surface would have, unbound or not: zeta_get
     * keeps a buffer per width, and a depth clear must reach the one the
     * draws after it use (a wider one left the race's depth uncleared). */
    uint32_t w = st->swz_w ? st->swz_w : st->bpp ? st->pitch / st->bpp : 0;
    GZeta *zs = zeta_get(st->zeta_key, cs ? cs->w : w);
    if (!cs && !zs)
        return 0;
    if (G.cur_cs == cs && G.cur_zs == zs)
        return 1;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, G.fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           cs ? cs->tex : 0, 0);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              zs ? zs->rb : 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        static int shown;
        if (shown++ < 10)
            fprintf(stderr, "[GL] incomplete target: surf %08X zeta %08X\n",
                    cs ? cs->va : 0, zs ? zs->key : 0);
        G.cur_cs = NULL;
        G.cur_zs = NULL;
        return 0;
    }
    G.cur_cs = cs;
    G.cur_zs = zs;
    gt_mark(cs ? gt_label(GT_TARGET, cs->va, cs->w, cs->swz ? cs->h : 0, cs->swz)
               : gt_label(GT_DEPTH, zs->key, zs->w, 0, 0));
    return 1;
}

static void target_size(uint32_t *w, uint32_t *h)
{
    *w = G.cur_cs ? G.cur_cs->w : G.cur_zs->w;
    *h = G.cur_cs ? G.cur_cs->h : G.cur_zs->h;
    if (G.cur_zs && G.cur_zs->w < *w) *w = G.cur_zs->w;
    if (G.cur_zs && G.cur_zs->h < *h) *h = G.cur_zs->h;
}

/* The clip rectangle, within the attachments; 0 if empty. */
static int set_scissor(const Nv2aVkState *st)
{
    uint32_t w, h, x0 = st->clip_x, y0 = st->clip_y;
    uint32_t x1 = st->clip_x + st->clip_w, y1 = st->clip_y + st->clip_h;
    target_size(&w, &h);
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x0 >= x1 || y0 >= y1)
        return 0;
    glScissor((GLint)x0, (GLint)y0, (GLsizei)(x1 - x0), (GLsizei)(y1 - y0));
    return 1;
}

static void mark_drawn(const Nv2aVkState *st)
{
    if (G.cur_cs) {
        uint32_t rows = st->clip_y + st->clip_h;
        if (!G.cur_cs->dirty && !flip_writeback()) {
            G.cur_cs->dref = hash_sparse(guest(G.cur_cs->va), surf_bytes(G.cur_cs));
            G.cur_cs->dref_valid = 1;
        }
        G.cur_cs->dirty = 1;
        G.cur_cs->gen++;
        if (rows > G.cur_cs->h)
            rows = G.cur_cs->h;
        if (rows > G.cur_cs->rows)
            G.cur_cs->rows = rows;
    }
}

/* ---- textures ---------------------------------------------------------- */

static GLenum wrap_mode(uint32_t m)
{
    switch (m) {
    case 1:  return GL_REPEAT;
    case 2:  return GL_MIRRORED_REPEAT;
    default: return GL_CLAMP_TO_EDGE;
    }
}

static GLuint sampler(const Nv2aVkTex *t, uint32_t levels)
{
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int mip = min >= 3 && min <= 6 && levels > 1;
    uint32_t maxl = t->max_lod >> 8 > t->min_level ? (t->max_lod >> 8) - t->min_level : 0;
    uint64_t key = (mag & 0xF) | (min & 0xF) << 4 | (t->addr_u & 0xF) << 8
                 | (t->addr_v & 0xF) << 12 | (t->addr_p & 0xF) << 16
                 | (uint64_t)mip << 20 | (uint64_t)(t->filter & 0x1FFF) << 21
                 | (uint64_t)(mip ? (maxl < levels ? maxl : levels) : 0) << 34;
    GLuint s;
    GLenum minf;
    int i;
    for (i = 0; i < G.nsamp; i++)
        if (G.samp[i].key == key)
            return G.samp[i].s;
    if (G.nsamp == GL_MAX_SAMP)
        return G.samp[0].s;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag == 2 ? GL_LINEAR : GL_NEAREST);
    if (!mip)
        minf = (min == 2 || min == 4 || min == 6) ? GL_LINEAR : GL_NEAREST;
    else if (min == 5 || min == 6)
        minf = (min == 6) ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    else
        minf = (min == 4) ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, (GLint)minf);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, (GLint)wrap_mode(t->addr_u));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, (GLint)wrap_mode(t->addr_v));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R, (GLint)wrap_mode(t->addr_p));
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, 0.0f);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, mip ? (float)(maxl < levels ? maxl : levels) : 0.25f);
    if (mip) {
        int32_t b = (int32_t)(t->filter << 19) >> 19;
        glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS, (float)b / 256.0f);
    }
    G.samp[G.nsamp].key = key;
    G.samp[G.nsamp].s = s;
    G.nsamp++;
    return s;
}

static int tex_same(const Nv2aVkTex *a, const Nv2aVkTex *b)
{
    return a->offset == b->offset && a->width == b->width && a->height == b->height
        && a->pitch == b->pitch && a->color == b->color && a->levels == b->levels
        && a->min_level == b->min_level && a->palette == b->palette && a->cube == b->cube
        && a->size_from_format == b->size_from_format;
}

/* Textures are checked against guest RAM at most once an epoch: hashing
 * every texture of every draw in full was four fifths of the pushbuffer
 * thread in a race. RECOMP_GL_TEX_HASH=sparse|full: how a texture of 64 KB
 * or more is checked -- sparse (hash_sparse, the full hash every
 * GL_TEX_FULL_EPOCHS, staggered) by default on the Switch, full elsewhere,
 * as the Vulkan renderer does on Android. */
static int tex_hash_sparse(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_TEX_HASH");
#if defined(__SWITCH__)
        on = !(e && strcmp(e, "full") == 0);
#else
        on = e && strcmp(e, "sparse") == 0;
#endif
    }
    return on;
}

static uint64_t tex_hash_full(const Nv2aVkTex *d, uint32_t bytes)
{
    uint64_t h = hash_bytes(guest(d->offset), bytes);
    if (d->palette)
        h ^= hash_bytes(guest(d->palette), 1024) * 31;
    return h;
}

static uint64_t tex_hash_part(const Nv2aVkTex *d, uint32_t bytes)
{
    uint64_t h = hash_sparse(guest(d->offset), bytes);
    if (d->palette)
        h ^= hash_bytes(guest(d->palette), 1024) * 31;
    return h;
}

/* 1 while guest RAM still holds what entry c was made from. */
static int tex_unchanged(GTex *c, uint32_t bytes)
{
    if (c->hash_ep == G.epoch)
        return 1;
    if (tex_hash_sparse() && bytes >= GL_TEX_SPARSE_MIN && G.epoch - c->full_ep < GL_TEX_FULL_EPOCHS
            && (G.epoch + (uint32_t)(c - G.tex)) % GL_TEX_FULL_EPOCHS != 0) {
        if (tex_hash_part(&c->d, bytes) != c->shash)
            return 0;
    } else {
        if (tex_hash_full(&c->d, bytes) != c->hash)
            return 0;
        c->full_ep = G.epoch;
    }
    c->hash_ep = G.epoch;
    return 1;
}

/* Guest bytes under [va, va + bytes) changed (a surface's write-back):
 * textures there are checked again, in full. */
static void tex_touch(uint32_t va, size_t bytes)
{
    uint32_t i;
    if (!G.tex)
        return;
    for (i = 0; i < GL_MAX_TEX; i++) {
        GTex *c = &G.tex[i];
        if (c->used && c->hash_ep && va < c->d.offset + nv2a_pb_tex_bytes(&c->d)
                && c->d.offset < va + bytes)
            c->hash_ep = c->full_ep = 0;
    }
}

/* The rt cache entry for a surface at va sampled as w x h (a cube: six
 * faces), made (or taken from the least recently used) when missing. */
static GRtTex *rt_slot(uint32_t va, uint32_t w, uint32_t h, int x8, int cube)
{
    GLenum target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    GRtTex *e = NULL;
    int i, f;
    for (i = 0; i < GL_MAX_RT; i++)
        if (G.rt[i].tex && G.rt[i].va == va && G.rt[i].w == w && G.rt[i].h == h
                && G.rt[i].x8 == (uint32_t)x8 && G.rt[i].cube == cube) {
            e = &G.rt[i];
            break;
        }
    if (!e) {
        e = &G.rt[0];
        for (i = 0; i < GL_MAX_RT; i++) {
            if (!G.rt[i].tex) { e = &G.rt[i]; break; }
            if (G.rt[i].last_use < e->last_use)
                e = &G.rt[i];
        }
        if (e->tex)
            glDeleteTextures(1, &e->tex);
        memset(e, 0, sizeof *e);
        step("rt texture", va, w | h << 16);
        e->va = va;
        e->w = w;
        e->h = h;
        e->x8 = (uint32_t)x8;
        e->cube = cube;
        glGenTextures(1, &e->tex);
        bind_other(target, e->tex);
        for (f = 0; f < (cube ? 6 : 1); f++)
            glTexImage2D(cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : GL_TEXTURE_2D, 0, GL_RGBA8,
                         (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
        if (x8)
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }
    e->last_use = G.frame;
    return e;
}

/* A cube map whose faces are swizzled surfaces (RSC2's car reflection: six
 * 128x128 faces, three redrawn a frame), put together on the GPU from the
 * faces' textures as the Vulkan renderer does. Each face's surface was drawn
 * since its copy-back or still matches guest RAM. Through guest RAM, the
 * faces drawn were copied back and the whole cube decoded again for every
 * frame -- waiting for the GPU in the middle of it. Any face without such a
 * surface: the old path. */
static GLuint rt_cube(const Nv2aVkTex *d, uint32_t bytes, uint32_t *levels_out)
{
    GSurf *face[6];
    GRtTex *e;
    uint32_t fbytes = bytes / 6, f;
    int i, x8, copied = 0;
    GtLabel prev;

    if (d->color == 0x06) x8 = 0;
    else if (d->color == 0x07) x8 = 1;
    else return 0;
    if (d->width != d->height || bytes % 6 || d->levels > 1)
        return 0;
    for (f = 0; f < 6; f++) {
        uint32_t va = d->offset + f * fbytes;
        face[f] = NULL;
        for (i = 0; i < G.nsurf; i++) {
            GSurf *q = &G.surf[i];
            if (q->va == va && q->swz && q->w == d->width && q->h == d->height && q->bpp == 4
                    && q->fmt == GL_RGBA8) {
                face[f] = q;
                break;
            }
        }
        if (!face[f] || face[f]->pending_up)
            return 0;
        /* Anything else dirty over the face: not ours to resolve. */
        for (i = 0; i < G.nsurf; i++) {
            GSurf *q = &G.surf[i];
            if (q != face[f] && q->dirty && va < q->va + surf_bytes(q) && va + fbytes > q->va)
                return 0;
        }
        if (!face[f]->dirty && !surf_unchanged(face[f]))
            return 0;
    }
    e = rt_slot(d->offset, d->width, d->height, x8, 1);
    for (f = 0; f < 6; f++) {
        if (e->fgen[f] == face[f]->gen + 1)
            continue;
        if (!copied++)
            prev = gt_push(GT_CUBE);
        glCopyImageSubData(face[f]->tex, GL_TEXTURE_2D, 0, 0, 0, 0,
                           e->tex, GL_TEXTURE_CUBE_MAP, 0, 0, 0, (GLint)f,
                           (GLsizei)d->width, (GLsizei)d->height, 1);
        e->fgen[f] = face[f]->gen + 1;
    }
    if (copied)
        gt_pop(prev);
    *levels_out = 1;
    return e->tex;
}

/* A texture that is a surface the GPU drew and guest RAM does not have yet:
 * sampled from a copy made on the GPU (glCopyImageSubData, again only when
 * the surface changed) instead of reading the surface back -- which waits
 * for the GPU in the middle of the frame -- and decoding it again. A copy,
 * not the surface itself, so a draw never samples what it renders into.
 * 2D A8R8G8B8 / X8R8G8B8 textures laid out exactly as the surface qualify,
 * as in the Vulkan renderer; anything else takes the old path.
 * RECOMP_GL_RT_DIRECT=0 turns it off. */
static GLuint rt_tex(const Nv2aVkTex *d, uint32_t bytes, uint32_t *levels_out)
{
    static int on = -1;
    GSurf *q = NULL;
    GRtTex *e = NULL;
    int i, x8, swz;

    if (on < 0) {
        const char *env = getenv("RECOMP_GL_RT_DIRECT");
        on = !(env && strcmp(env, "0") == 0);
    }
    if (!on)
        return 0;
    if (d->cube)
        return rt_cube(d, bytes, levels_out);
    switch (d->color) {
    case 0x06: x8 = 0; swz = 1; break;          /* A8R8G8B8 */
    case 0x07: x8 = 1; swz = 1; break;          /* X8R8G8B8 */
    case 0x12: x8 = 0; swz = 0; break;          /* LIN_A8R8G8B8 */
    case 0x1E: x8 = 1; swz = 0; break;          /* LIN_X8R8G8B8 */
    default: return 0;
    }
    for (i = 0; i < G.nsurf; i++) {
        GSurf *c = &G.surf[i];
        if (!c->dirty || !(d->offset < c->va + surf_bytes(c) && d->offset + bytes > c->va))
            continue;
        if (q || c->va != d->offset)
            return 0;                           /* not exactly one surface */
        q = c;
    }
    if (!q || q->bpp != 4 || q->fmt != GL_RGBA8 || q->swz != swz)
        return 0;
    if (swz ? (d->width != q->w || d->height != q->h)
            : (d->pitch != q->pitch || d->width > q->w || d->height > q->h))
        return 0;
    e = rt_slot(q->va, d->width, d->height, x8, 0);
    if (e->fgen[0] != q->gen + 1) {
        GtLabel prev = gt_push(GT_RT);
        glCopyImageSubData(q->tex, GL_TEXTURE_2D, 0, 0, 0, 0,
                           e->tex, GL_TEXTURE_2D, 0, 0, 0, 0,
                           (GLsizei)e->w, (GLsizei)e->h, 1);
        gt_pop(prev);
        e->fgen[0] = q->gen + 1;
    }
    *levels_out = 1;
    return e->tex;
}

/* A decoded, uploaded texture for stage state t; 0 when it cannot be had. */
static GLuint tex_get(const Nv2aVkTex *t, uint32_t *levels_out)
{
    Nv2aVkTex d = *t;
    uint32_t bytes, slot, n;
    uint64_t h;
    GTex *e = NULL;

    d.filter = d.addr_u = d.addr_v = d.addr_p = 0;
    d.max_lod = 0;
    G.st_tex++;
    if (!d.valid || !d.width || !d.height || d.width > 4096 || d.height > 4096) {
        G.st_texfail++;
        return 0;
    }
    bytes = nv2a_pb_tex_bytes(&d);
    if (!bytes) {
        G.st_texfail++;
        return 0;
    }
    {
        GLuint rt = rt_tex(&d, bytes, levels_out);
        if (rt)
            return rt;
    }
    /* Drawn by the GPU this frame: guest RAM has to have it first. */
    {
        static int trace = -1;
        int i;
        if (trace < 0)
            trace = getenv("RECOMP_GL_UPLOAD_TRACE") != NULL;
        for (i = 0; trace && i < G.nsurf; i++)
            if (G.surf[i].dirty && d.offset < G.surf[i].va + surf_bytes(&G.surf[i])
                    && G.surf[i].va < d.offset + bytes && (G.frame % 60) == 0)
                fprintf(stderr, "[GL] F%u texture %08X %ux%u fmt %02X%s levels %u needs surface %08X\n",
                        G.frame, d.offset, d.width, d.height, d.color, d.cube ? " cube" : "",
                        d.levels, G.surf[i].va);
    }
    writeback_overlap(d.offset, bytes);
    slot = (d.offset * 2654435761u) & (GL_MAX_TEX - 1);
    for (n = 0; n < GL_MAX_TEX; n++) {
        GTex *c = &G.tex[(slot + n) & (GL_MAX_TEX - 1)];
        if (!c->used) {
            e = c;
            break;
        }
        if (tex_same(&c->d, &d)) {
            if (tex_unchanged(c, bytes)) {
                c->last_use = G.frame;
                *levels_out = c->levels;
                return c->tex;
            }
            e = c;
            break;
        }
    }
    h = tex_hash_full(&d, bytes);
    {
        static int trace = -1;
        if (trace < 0)
            trace = getenv("RECOMP_GL_UPLOAD_TRACE") != NULL;
        if (trace && (G.frame % 20) == 0)
            fprintf(stderr, "[GL] F%u upload texture %08X %ux%u fmt %02X%s levels %u (%s)\n",
                    G.frame, d.offset, d.width, d.height, d.color, d.cube ? " cube" : "",
                    d.levels, !e ? "evicting" : e->used ? "changed" : "new");
    }
    if (!e) {
        /* Full: the least recently used in this neighbourhood. */
        uint32_t k;
        e = &G.tex[slot];
        for (k = 1; k < 16; k++) {
            GTex *c = &G.tex[(slot + k) & (GL_MAX_TEX - 1)];
            if (c->last_use < e->last_use)
                e = c;
        }
    }
    {
        uint64_t t0 = perf_on() ? now_us() : 0;
        GtLabel prev = gt_push(GT_TEX);
        GLuint r = tex_upload(&d, bytes, e, h, levels_out);
        gt_pop(prev);
        if (t0)
            G.pf_tex_us += now_us() - t0;
        G.pf_tex_up++;
        G.pf_tex_kb += bytes >> 10;
        return r;
    }
}

/* A DXT texture as it is: each face's levels follow one another in guest
 * RAM, faces a stride apart (nv2a_pb_tex_bytes / 6), from min_level on.
 * Decoding a streamed 512x1024 track texture on the Switch's CPU took tens
 * of milliseconds, and the decoded copy is 4-8 times the bytes for the GPU
 * to read. */
static GLuint tex_upload_bc(const Nv2aVkTex *d, uint32_t bytes, GTex *e, uint64_t h,
                            uint32_t *levels_out)
{
    uint32_t faces = d->cube ? 6 : 1, bb = d3d8_format_dxt_block_bytes(d->color);
    uint32_t fstride = bytes / faces, skip = 0, levels, l, f, w0, h0;
    GLenum ifmt = d->color == 0x0C ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                : d->color == 0x0E ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                                   : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    GLenum target = d->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    const uint8_t *src = guest(d->offset);

    levels = d->levels > d->min_level ? d->levels - d->min_level : 1;
    if (levels > 13)
        levels = 13;
    for (l = 0; l < d->min_level; l++) {
        uint32_t lw = d->width >> l ? d->width >> l : 1, lh = d->height >> l ? d->height >> l : 1;
        skip += ((lw + 3) / 4) * ((lh + 3) / 4) * bb;
    }
    w0 = d->width >> d->min_level ? d->width >> d->min_level : 1;
    h0 = d->height >> d->min_level ? d->height >> d->min_level : 1;
    step("texture bc", d->offset, d->color | w0 << 16);
    if (e->used && e->tex)
        glDeleteTextures(1, &e->tex);
    glGenTextures(1, &e->tex);
    bind_other(target, e->tex);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    for (f = 0; f < faces; f++) {
        const uint8_t *fs = src + (size_t)f * fstride + skip;
        for (l = 0; l < levels; l++) {
            uint32_t lw = w0 >> l ? w0 >> l : 1, lh = h0 >> l ? h0 >> l : 1;
            uint32_t n = ((lw + 3) / 4) * ((lh + 3) / 4) * bb;
            glCompressedTexImage2D(d->cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : GL_TEXTURE_2D,
                                   (GLint)l, ifmt, (GLsizei)lw, (GLsizei)lh, 0, (GLsizei)n, fs);
            fs += n;
        }
    }
    e->used = 1;
    e->d = *d;
    e->hash = h;
    e->shash = tex_hash_part(d, bytes);
    e->hash_ep = e->full_ep = G.epoch;
    e->levels = levels;
    e->last_use = G.frame;
    *levels_out = levels;
    return e->tex;
}

/* Decode d into entry e and upload it. */
static GLuint tex_upload(const Nv2aVkTex *dp, uint32_t bytes, GTex *e, uint64_t h,
                         uint32_t *levels_out)
{
    Nv2aVkTex d = *dp;
    uint32_t faces, levels, lv, f, w0, h0, *buf;
    faces = d.cube ? 6 : 1;
    if (G.bc_ok && d.size_from_format && d3d8_format_dxt_block_bytes(d.color))
        return tex_upload_bc(&d, bytes, e, h, levels_out);
    levels = d.size_from_format && d.levels > d.min_level ? d.levels - d.min_level : 1;
    if (levels > 13)
        levels = 13;
    w0 = d.size_from_format ? (d.width >> d.min_level) : d.width;
    h0 = d.size_from_format ? (d.height >> d.min_level) : d.height;
    if (!w0) w0 = 1;
    if (!h0) h0 = 1;
    buf = (uint32_t *)scratch((size_t)w0 * h0 * 4);
    if (!buf)
        return 0;
    step("texture", d.offset, d.color | w0 << 16);
    if (e->used && e->tex)
        glDeleteTextures(1, &e->tex);
    glGenTextures(1, &e->tex);
    {
        GLenum target = d.cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        bind_other(target, e->tex);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
        for (f = 0; f < faces; f++)
            for (lv = 0; lv < levels; lv++) {
                uint32_t lw = w0 >> lv ? w0 >> lv : 1, lh = h0 >> lv ? h0 >> lv : 1;
                nv2a_pb_tex_decode(&d, f, d.min_level + lv, lw, lh, buf);
                glTexImage2D(d.cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : GL_TEXTURE_2D,
                             (GLint)lv, GL_RGBA8, (GLsizei)lw, (GLsizei)lh, 0, GL_BGRA,
                             GL_UNSIGNED_INT_8_8_8_8_REV, buf);
            }
    }
    e->used = 1;
    e->d = d;
    e->hash = h;
    e->shash = tex_hash_part(&d, bytes);
    e->hash_ep = e->full_ep = G.epoch;
    e->levels = levels;
    e->last_use = G.frame;
    *levels_out = levels;
    return e->tex;
}

/* ---- draws ------------------------------------------------------------- */

static GLenum blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return GL_ZERO;
    case 0x0001: return GL_ONE;
    case 0x0300: return GL_SRC_COLOR;
    case 0x0301: return GL_ONE_MINUS_SRC_COLOR;
    case 0x0302: return GL_SRC_ALPHA;
    case 0x0303: return GL_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return GL_DST_ALPHA;
    case 0x0305: return GL_ONE_MINUS_DST_ALPHA;
    case 0x0306: return GL_DST_COLOR;
    case 0x0307: return GL_ONE_MINUS_DST_COLOR;
    case 0x0308: return GL_SRC_ALPHA_SATURATE;
    case 0x8001: return GL_CONSTANT_COLOR;
    case 0x8002: return GL_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return GL_CONSTANT_ALPHA;
    case 0x8004: return GL_ONE_MINUS_CONSTANT_ALPHA;
    default:     return GL_ONE;
    }
}

static GLenum blend_eq(uint32_t e)
{
    switch (e) {
    case 0x800A: return GL_FUNC_SUBTRACT;
    case 0x800B: return GL_FUNC_REVERSE_SUBTRACT;
    case 0x8007: return GL_MIN;
    case 0x8008: return GL_MAX;
    default:     return GL_FUNC_ADD;
    }
}

static GLenum compare_fn(uint32_t f)
{
    if (f >= 0x200 && f <= 0x207)
        return GL_NEVER + (f - 0x200);       /* NEVER..ALWAYS, in GL's order too */
    return GL_ALWAYS;
}

static GLenum stencil_op(uint32_t op)
{
    switch (op) {
    case 0x1E00: case 1: return GL_KEEP;
    case 0x0000: case 2: return GL_ZERO;
    case 0x1E01: case 3: return GL_REPLACE;
    case 0x1E02: case 4: return GL_INCR;
    case 0x1E03: case 5: return GL_DECR;
    case 0x150A: case 6: return GL_INVERT;
    case 0x8507: case 7: return GL_INCR_WRAP;
    case 0x8508: case 8: return GL_DECR_WRAP;
    default:             return GL_KEEP;
    }
}

/* LESS <-> GREATER, LEQUAL <-> GEQUAL: the reversed w-buffer's tests. */
static uint32_t depth_func_rev(uint32_t f)
{
    switch (f) {
    case 0x201: return 0x204;
    case 0x203: return 0x206;
    case 0x204: return 0x201;
    case 0x206: return 0x203;
    default:    return f;
    }
}

static int fb_widescreen(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_WIDESCREEN");
        on = e && *e && strcmp(e, "0") != 0;
    }
    return on;
}

float nv2a_vk_view_aspect(void)
{
    static float force = -1.0f;
    float a = 16.0f / 9.0f;
    if (force < 0.0f) {
        const char *e = getenv("RECOMP_VIEW_ASPECT");
        force = e ? (float)atof(e) : 0.0f;
    }
    if (fb_widescreen() && force > a)
        return force;
    return a;
}

static void set_state(const Nv2aVkState *st)
{
    int depth_mode = st->wbuffer ? 2 : 0;     /* float depth: the reversed w-buffer */
    float bc[4];
    uint32_t key[24];

    memset(key, 0, sizeof key);
    key[0] = st->blend_enable;  key[1] = st->blend_sfactor; key[2] = st->blend_dfactor;
    key[3] = st->blend_equation; key[4] = st->blend_color;  key[5] = st->color_mask;
    key[6] = st->cull_enable;   key[7] = st->cull_face;     key[8] = st->front_face;
    key[9] = st->depth_test;    key[10] = st->depth_func;   key[11] = st->depth_mask;
    key[12] = st->wbuffer;      key[13] = st->st_test;      key[14] = st->st_func;
    key[15] = st->st_ref;       key[16] = st->st_mask;      key[17] = st->st_fail;
    key[18] = st->st_zfail;     key[19] = st->st_zpass;     key[20] = st->st_write;
    key[21] = st->st_wmask;     key[22] = G.cur_zs != NULL;
    if (G.state_valid && memcmp(key, G.state_key, sizeof key) == 0)
        return;
    memcpy(G.state_key, key, sizeof key);
    G.state_valid = 1;

    if (st->blend_enable && !(st->blend_sfactor == 1 && st->blend_dfactor == 0)) {
        glEnable(GL_BLEND);
        glBlendFunc(blend_factor(st->blend_sfactor), blend_factor(st->blend_dfactor));
        glBlendEquation(blend_eq(st->blend_equation));
        nv2a_rc_unpack(st->blend_color, bc);
        glBlendColor(bc[0], bc[1], bc[2], bc[3]);
    } else {
        glDisable(GL_BLEND);
    }
    glColorMask((st->color_mask & 0x00010000u) != 0, (st->color_mask & 0x00000100u) != 0,
                (st->color_mask & 0x00000001u) != 0, (st->color_mask & 0x01000000u) != 0);
    if (st->cull_enable) {
        glEnable(GL_CULL_FACE);
        glCullFace(st->cull_face == 0x0404 ? GL_FRONT
                   : st->cull_face == 0x0408 ? GL_FRONT_AND_BACK : GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }
    /* Inverted from the title's: Vulkan's facing area carries a minus sign
     * GL's does not (y down), and the coordinates here are Vulkan's. */
    glFrontFace(st->front_face == 0x0901 ? GL_CW : GL_CCW);
    if (G.cur_zs && st->depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(compare_fn(depth_mode == 2 ? depth_func_rev(st->depth_func) : st->depth_func));
        glDepthMask(st->depth_mask ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    if (G.cur_zs && st->st_test) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(compare_fn(st->st_func), (GLint)(st->st_ref & 0xFF), st->st_mask & 0xFF);
        glStencilOp(stencil_op(st->st_fail), stencil_op(st->st_zfail), stencil_op(st->st_zpass));
        glStencilMask(st->st_write ? (st->st_wmask & 0xFF) : 0);
    } else {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
    }
}

/* [ring_off, ring_off + total) of the persistently mapped ring, made safe
 * to write: crossing into a quarter waits for the fence set when the writes
 * last left it (the GPU has read what was there), and leaving one sets its
 * fence. The ring holds several frames, so the wait is normally none. */
static void ring_reserve(size_t total)
{
    size_t q = GL_RING_SIZE / 4;
    int last = (int)((G.ring_off + total - 1) / q);
    while (G.ring_q != last) {
        if (G.ring_fence[G.ring_q])
            glDeleteSync(G.ring_fence[G.ring_q]);
        G.ring_fence[G.ring_q] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        G.ring_q = (G.ring_q + 1) & 3;
        if (G.ring_fence[G.ring_q]) {
            while (glClientWaitSync(G.ring_fence[G.ring_q], GL_SYNC_FLUSH_COMMANDS_BIT,
                                    1000000000ull) == GL_TIMEOUT_EXPIRED)
                ;
            glDeleteSync(G.ring_fence[G.ring_q]);
            G.ring_fence[G.ring_q] = 0;
        }
    }
}

static int draw_common(const Nv2aVkState *st, GProg *p, const void *v, uint32_t stride,
                       uint32_t nv, const uint32_t *idx, uint32_t nidx)
{
    Nv2aVkUbo ub;
    int i, stage_on[4], zq = 0;
    uint32_t w, h, levels;
    GLuint tex2d[4], texcube[4], samp[4];
    uint64_t pt0 = perf_on() ? now_us() : 0, pt1 = 0;

    /* Textures first: one may be a surface drawn earlier this frame, which
     * is copied back before it is decoded. */
    for (i = 0; i < 4; i++) {
        GLuint t = 0;
        stage_on[i] = 0;
        tex2d[i] = G.white2d;
        texcube[i] = G.whitecube;
        samp[i] = 0;
        if (nv2a_draw_stage_samples(st, i) && (t = tex_get(&st->tex[i], &levels)) != 0) {
            stage_on[i] = 1;
            if (st->tex[i].cube)
                texcube[i] = t;
            else
                tex2d[i] = t;
            samp[i] = sampler(&st->tex[i], levels);
        }
    }
    G.st_draws++;
    if (p->hash == 0)
        G.st_cpu++;
    {
        static int trace = -1;
        if (trace < 0)
            trace = getenv("RECOMP_GL_TRACE") ? atoi(getenv("RECOMP_GL_TRACE")) : 0;
        if (trace >= 2 && (G.frame % 30) == 0 && G.st_draws <= 4) {
            const float *f = (const float *)v;
            fprintf(stderr, "[GL]   draw prog %08X in %d nv %u nidx %u topo %u surf %08X zeta %08X"
                    " clip %u,%u %ux%u cm %08X blend %u %X %X depth %u %X mask %u st %u cull %u %X"
                    " at %u %X %X rc %u tex0 %08X fmt %02X %ux%u sff %d | v0 %g %g %g %g\n",
                    p->hash, p->ninputs, nv, nidx, st->topology, st->color_va, st->zeta_key,
                    st->clip_x, st->clip_y, st->clip_w, st->clip_h, st->color_mask, st->blend_enable,
                    st->blend_sfactor, st->blend_dfactor, st->depth_test, st->depth_func,
                    st->depth_mask, st->st_test, st->cull_enable, st->cull_face, st->alpha_test,
                    st->alpha_func, st->alpha_ref, st->use_rc, st->tex[0].offset, st->tex[0].color,
                    st->tex[0].width, st->tex[0].height, st->tex[0].size_from_format,
                    f[0], f[1], f[2], f[3]);
        }
    }
    if (!begin_target(st, 1)) {
        G.st_notarget++;
        return 1;
    }
    surf_settle(G.cur_cs);
    target_size(&w, &h);
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    if (!set_scissor(st)) {
        G.st_noclip++;
        return 1;
    }
    {
        const GSurf *c = G.cur_cs;
        nv2a_draw_ubo_fill(&ub, st, stage_on, w, h, st->wbuffer ? 2 : 0, 1.0f, 1.0f,
                           c && !c->swz && c->bpp == 4 && c->w == 640 && G.cur_zs && c->rows <= 480
                           ? nv2a_draw_wide_factor(nv2a_vk_view_aspect()) : 1.0f);
    }
    /* Everything the draw reads, in one piece of the ring: the uniforms, the
     * vertex constants when they changed, the vertices, the indices. A
     * glBufferSubData / glBufferData per buffer per draw was 0.4 ms a draw
     * on the Switch (each could wait for the GPU or allocate). */
    {
        size_t a = G.ubo_align, o_c, o_v, o_i, total;
        int consts = G.const_epoch != G.epoch || G.const_serial != nv2a_vsh_const_serial();
        size_t vbytes = (size_t)nv * stride, ibytes = (size_t)nidx * 4;
        uint8_t *m;
        /* The same uniforms as the draw before, still in this lap of the
         * ring: bound already, so neither copied nor bound again (a
         * constant-buffer rebind each draw is driver work, and on nouveau
         * GPU work too). Consecutive draws mostly share them. */
        int ub_eq = G.last_ub_valid && memcmp(&ub, &G.last_ub, sizeof ub) == 0, same = 0;
        if (pt0) {
            pt1 = now_us();
            G.pf_prep_us += pt1 - pt0;
        }
        glBindBuffer(GL_ARRAY_BUFFER, G.ring);
        for (;;) {
            same = ub_eq && G.last_ub_lap == G.ring_lap;
            o_c = same ? 0 : (sizeof ub + a - 1) & ~(a - 1);
            o_v = consts ? ((o_c + NV2A_VSH_CONSTANTS * 16 + a - 1) & ~(a - 1)) : o_c;
            o_i = (o_v + vbytes + 15) & ~(size_t)15;
            total = o_i + ibytes;
            if (total > GL_RING_SIZE)
                return 1;
            if (G.ring_off + total <= GL_RING_SIZE)
                break;
            /* Wrap, and the vertex constants with it: mapped per draw, new
             * storage (draws in flight keep the old); persistently, back to
             * the start, once the GPU is past it (ring_reserve). */
            if (!G.ring_map)
                glBufferData(GL_ARRAY_BUFFER, GL_RING_SIZE, NULL, GL_STREAM_DRAW);
            G.ring_off = 0;
            G.ring_lap++;
            consts = 1;
        }
        if (G.ring_map) {
            ring_reserve(total);
            m = G.ring_map + G.ring_off;
        } else {
            m = (uint8_t *)glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)G.ring_off,
                                            (GLsizeiptr)total, GL_MAP_WRITE_BIT
                                            | GL_MAP_UNSYNCHRONIZED_BIT
                                            | GL_MAP_INVALIDATE_RANGE_BIT);
            if (!m)
                return 1;
        }
        if (!same)
            memcpy(m, &ub, sizeof ub);
        if (consts)
            memcpy(m + o_c, nv2a_vsh_constants(), NV2A_VSH_CONSTANTS * 16);
        memcpy(m + o_v, v, vbytes);
        memcpy(m + o_i, idx, ibytes);
        if (!G.ring_map)
            glUnmapBuffer(GL_ARRAY_BUFFER);
        if (same) {
            G.pf_ub_same++;
        } else {
            glBindBufferRange(GL_UNIFORM_BUFFER, 0, G.ring, (GLintptr)G.ring_off, sizeof ub);
            G.last_ub = ub;
            G.last_ub_off = G.ring_off;
            G.last_ub_lap = G.ring_lap;
            G.last_ub_valid = 1;
        }
        if (consts) {
            G.pf_consts++;
            glBindBufferRange(GL_UNIFORM_BUFFER, 3, G.ring, (GLintptr)(G.ring_off + o_c),
                              NV2A_VSH_CONSTANTS * 16);
            G.const_epoch = G.epoch;
            G.const_serial = nv2a_vsh_const_serial();
        }
        o_v += G.ring_off;
        o_i += G.ring_off;
        G.ring_off = (G.ring_off + total + a - 1) & ~(a - 1);

        for (i = 0; i < 4; i++) {
            if (!G.b_valid || G.b_tex[i] != tex2d[i]) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, tex2d[i]);
                G.b_tex[i] = tex2d[i];
            }
            if (!G.b_valid || G.b_tex[4 + i] != texcube[i]) {
                glActiveTexture(GL_TEXTURE4 + i);
                glBindTexture(GL_TEXTURE_CUBE_MAP, texcube[i]);
                G.b_tex[4 + i] = texcube[i];
            }
            if (!G.b_valid || G.b_samp[i] != samp[i]) {
                glBindSampler((GLuint)i, samp[i]);
                glBindSampler((GLuint)(4 + i), samp[i]);
                G.b_samp[i] = samp[i];
            }
        }
        G.b_valid = 1;
        {
            GLuint fs = frag_get(st, &ub);
            if (G.b_vs != p->prog) {
                glUseProgramStages(G.pipe, GL_VERTEX_SHADER_BIT, p->prog);
                G.b_vs = p->prog;
            }
            if (G.b_fs != fs) {
                glUseProgramStages(G.pipe, GL_FRAGMENT_SHADER_BIT, fs);
                G.b_fs = fs;
            }
        }
        if (G.b_ninputs != p->ninputs) {
            for (i = 0; i < 16; i++) {
                if (i < p->ninputs)
                    glEnableVertexAttribArray((GLuint)i);
                else
                    glDisableVertexAttribArray((GLuint)i);
            }
            G.b_ninputs = p->ninputs;
        }
        glBindVertexBuffer(0, G.ring, (GLintptr)o_v, (GLsizei)stride);
        set_state(st);
        zq = G.zpass && !(gl_exp() & 1);
        if (zq && G.nq == GL_MAX_QUERIES)
            reports_resolve();
        if (zq)
            glBeginQuery(GL_SAMPLES_PASSED, G.queries[G.nq]);
        step("draw", p->hash, st->color_va);
        glDrawElements(st->topology == 2 ? GL_POINTS : st->topology == 1 ? GL_LINES : GL_TRIANGLES,
                       (GLsizei)nidx, GL_UNSIGNED_INT, (const void *)(uintptr_t)o_i);
    }
    if (zq) {
        glEndQuery(GL_SAMPLES_PASSED);
        G.nq++;
    }
    if (pt1)
        G.pf_glcall_us += now_us() - pt1;
    mark_drawn(st);
    if (G.cur_cs)
        G.cur_cs->clear_only = 0;
    return 1;
}

void nv2a_vk_draw(const Nv2aVkState *st, const Nv2aVshOutput *v, uint32_t nv,
                  const uint32_t *idx, uint32_t nidx)
{
    GProg *p;
    uint64_t t0;
    if (!nidx || !nv || !nv2a_vk_active())
        return;
    t0 = perf_on() ? now_us() : 0;
    p = prog_get(0);
    if (p && p->prog)
        (void)draw_common(st, p, v, sizeof *v, nv, idx, nidx);
    if (t0)
        G.pf_draw_us += now_us() - t0;
    G.pf_draws++;
}

int nv2a_vk_program(uint32_t hash, uint8_t inputs[16])
{
    GProg *p;
    static int cpu = -1;
    if (cpu < 0)
        cpu = getenv("RECOMP_VK_CPU_VSH") != NULL;
    if (cpu || !hash || !nv2a_vk_active())
        return -1;
    p = prog_get(hash);
    if (!p || p->failed)
        return -1;
    memcpy(inputs, p->inputs, 16);
    return p->ninputs;
}

int nv2a_vk_draw_program(const Nv2aVkState *st, uint32_t hash, const float *vin,
                         uint32_t nv, const uint32_t *idx, uint32_t nidx)
{
    GProg *p;
    if (!nidx || !nv || !nv2a_vk_active())
        return 1;
    p = prog_get(hash);
    if (!p || p->failed)
        return 0;
    {
        uint64_t t0 = perf_on() ? now_us() : 0;
        int r = draw_common(st, p, vin, (uint32_t)p->ninputs * 16, nv, idx, nidx);
        if (t0)
            G.pf_draw_us += now_us() - t0;
        G.pf_draws++;
        return r;
    }
}

/* A full colour clear about to overwrite st's surface: what another
 * surface over exactly those bytes holds that guest RAM does not -- drawn
 * this frame, or a flipped frame's copy-back still on its way -- is of no
 * use any more, so it is dropped (an in-flight copy never lands) instead of
 * copied back by alias_resolve, which waited for the GPU. RSC2 clears its
 * swizzled shadow map, reflection faces and a 64x64 effect target through
 * linear surfaces every frame. After the Vulkan renderer's drop_inflight. */
static void drop_inflight(const Nv2aVkState *st)
{
    int swz = st->swz_w != 0, i;
    uint32_t bytes;
    if (swz)
        bytes = st->clip_w >= st->swz_w && st->clip_h >= st->swz_h
              ? st->swz_w * st->swz_h * st->bpp : 0;
    else
        bytes = st->bpp && st->clip_w >= st->pitch / st->bpp ? st->clip_h * st->pitch : 0;
    if (!bytes)
        return;
    for (i = 0; i < G.nsurf; i++) {
        GSurf *q = &G.surf[i];
        /* Only the same bytes in another shape: the one buffer drawn as two
         * surfaces. */
        if (!q->dirty || q->va != st->color_va || surf_bytes(q) != bytes
                || (q->swz == swz && q->bpp == st->bpp
                    && (swz ? q->w == st->swz_w : q->pitch == st->pitch)))
            continue;
        q->dirty = 0;
        q->clear_only = 0;
        q->ref_valid = 0;
    }
}

void nv2a_vk_clear(const Nv2aVkState *st, uint32_t flags, uint32_t argb, uint32_t zs)
{
    GLbitfield bits = 0;
    if (!nv2a_vk_active())
        return;
    /* Every channel over the whole surface: nothing under it survives. */
    if ((flags & 0xF0) == 0xF0 && st->clip_x == 0 && st->clip_y == 0)
        drop_inflight(st);
    /* Depth and stencil only: the colour surface is not touched, so it is
     * not resolved either -- RSC2 clears the reflection faces' depth with
     * a linear surface over each face set, which copied the face back. */
    if (!begin_target(st, (flags & 0xF0) != 0) || !set_scissor(st))
        return;
    G.st_clears++;
    if (G.cur_cs && G.cur_cs->pending_up) {
        GSurf *c = G.cur_cs;
        int full = (flags & 0xF0) == 0xF0 && st->clip_x == 0 && st->clip_y == 0
                && st->clip_w >= c->w && st->clip_h >= (c->swz ? c->h : c->rows);
        if (full) {
            c->pending_up = 0;          /* every byte of it is about to go */
            c->up_epoch = G.epoch;
        } else {
            surf_settle(c);
        }
    }
    G.state_valid = 0;                  /* masks below */
    if ((flags & 0xF0) && G.cur_cs) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor((float)((argb >> 16) & 0xFF) / 255.0f, (float)((argb >> 8) & 0xFF) / 255.0f,
                     (float)(argb & 0xFF) / 255.0f, (float)(argb >> 24) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if ((flags & 0x03) && G.cur_zs) {
        float zmax = st->zeta_fmt == 1 ? 65535.0f : 16777215.0f;
        float raw = st->zeta_fmt == 1 ? (float)(zs & 0xFFFF) : (float)(zs >> 8);
        const float wrev_c = 1.0f / 1024.0f;
        if (flags & 0x01) {
            glDepthMask(GL_TRUE);
            glClearDepth(st->wbuffer ? (raw > wrev_c ? wrev_c / raw : 1.0f) : raw / zmax);
            bits |= GL_DEPTH_BUFFER_BIT;
        }
        if (flags & 0x02) {
            glStencilMask(0xFF);
            glClearStencil((GLint)(zs & 0xFF));
            bits |= GL_STENCIL_BUFFER_BIT;
        }
    }
    if (!bits)
        return;
    step("clear", st->color_va, bits);
    glClear(bits);
    if (bits & GL_COLOR_BUFFER_BIT) {
        GSurf *c = G.cur_cs;
        mark_drawn(st);
        /* A full clear leaves nothing but the clear; a partial one, more. */
        c->clear_only = st->clip_x == 0 && st->clip_y == 0 && st->clip_w >= c->w
                     && st->clip_h >= (c->swz ? c->h : c->rows);
        if (c->clear_only)
            c->clear_argb = argb;
    }
}

/* ---- sync, visibility, presentation ------------------------------------ */

/* Whether the end-of-frame copy-back of s is worth making. The surfaces
 * being flipped are read by nothing in guest RAM when a window shows them
 * from their textures, and copying them back waited for the GPU every frame
 * (most of the pushbuffer thread's time). They stay dirty, so anything that
 * does read them through guest RAM still copies them back first.
 * RECOMP_GL_FB_WRITEBACK=1 copies every surface back as before, =0 skips
 * the flipped ones even headless (for measuring); frame dumps keep them. */
static int wb_mode(void)
{
    static int mode = -2;
    if (mode == -2) {
        const char *e = getenv("RECOMP_GL_FB_WRITEBACK");
        mode = e ? atoi(e) != 0 : -1;
        if (mode < 0 && (getenv("RECOMP_FB_DUMP") || getenv("RECOMP_FB_DUMP_AT")
                         || getenv("RECOMP_VIDEO_RECORD") || getenv("RECOMP_FB_DUMP_FLIPS")))
            mode = 1;
    }
    return mode;
}

static int writeback_needed(const GSurf *s)
{
    int mode = wb_mode();
    if (mode == 1 || s->swz)
        return 1;
    if (s->va != G.present_va && s->va != G.prev_present_va)
        return 1;
    return mode == 0 ? 0 : G.own_win;
}

/* Whether flips are pipelined (flush_pipelined). RECOMP_GL_PIPELINE=0
 * waits for each frame at its flip as before; so do frame dumps and
 * recordings, which read the frame from guest RAM right after it, unless
 * RECOMP_GL_PIPELINE=1 (their frames are then a flip late). */
static int pipelined(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GL_PIPELINE");
        on = e ? strcmp(e, "0") != 0 : wb_mode() != 1;
    }
    return on;
}

void nv2a_vk_flush(void)
{
    int i;
    uint64_t t0;
    if (G.state <= 0)
        return;
    t0 = perf_on() ? now_us() : 0;
    if (pipelined()) {
        flush_pipelined();
    } else {
        reports_resolve();
        if (t0)
            G.pf_wait_us += now_us() - t0;
        for (i = 0; i < G.nsurf; i++) {
            if (!G.surf[i].dirty || !writeback_needed(&G.surf[i]))
                continue;
            G.st_wb++;
            wb_why = "flush", surf_writeback(&G.surf[i]);
        }
    }
    {
        static int trace = -1;
        if (trace < 0)
            trace = getenv("RECOMP_GL_TRACE") != NULL;
        if (trace && (G.frame % 30) == 0) {
            fprintf(stderr, "[GL] F%u draws %u (cpu %u) no-target %u no-clip %u clears %u"
                    " tex %u failed %u written back %u present %08X surfs %d:",
                    nv2a_pb_presented_frames(), G.st_draws, G.st_cpu, G.st_notarget,
                    G.st_noclip, G.st_clears, G.st_tex, G.st_texfail, G.st_wb, G.present_va,
                    G.nsurf);
            for (i = 0; i < G.nsurf; i++)
                fprintf(stderr, " %08X/%u/%u%s", G.surf[i].va, G.surf[i].pitch, G.surf[i].bpp,
                        G.surf[i].swz ? "s" : "");
            fputc('\n', stderr);
        }
        G.st_draws = G.st_cpu = G.st_notarget = G.st_noclip = G.st_clears = 0;
        G.st_tex = G.st_texfail = G.st_wb = 0;
    }
    G.epoch++;
    G.frame++;
    G.spec_left = spec_per_frame();
    G.cur_cs = NULL;
    G.cur_zs = NULL;
    if (t0) {
        G.pf_flush_us += now_us() - t0;
        G.pf_frames++;
        perf_report();
    }
}

/* Answer every report asked for since the last time: one wait for the GPU
 * at most, at the end of the frame (or when the query pool runs out), as the
 * Vulkan renderer answers them at its sync. Until then a report's slot holds
 * what D3D put there -- "not done" -- which the title polls. */
static void reports_write(const GLuint *queries, int nq, int clear_q,
                          const GReport *reps, int nreps)
{
    static uint32_t *res;
    int i, k;
    uint64_t since;
    if (!nq && !nreps)
        return;
    if (!res)
        res = (uint32_t *)calloc(GL_MAX_QUERIES, sizeof(uint32_t));
    for (i = 0; i < nq; i++) {
        GLuint n = 0;
        glGetQueryObjectuiv(queries[i], GL_QUERY_RESULT, &n);
        res[i] = n;
    }
    for (k = 0; k < nreps; k++) {
        uint64_t sum = reps[k].with_carry ? G.carry : 0;
        uint32_t count, done = 0;
        uint8_t *g = guest(reps[k].va);
        for (i = reps[k].q0; i < reps[k].q1; i++)
            sum += res[i];
        count = (uint32_t)sum;
        memcpy(g, &reps[k].stamp, 8);
        memcpy(g + 8, &count, 4);
        memcpy(g + 12, &done, 4);
    }
    since = clear_q < 0 ? G.carry : 0;
    for (i = clear_q < 0 ? 0 : clear_q; i < nq; i++)
        since += res[i];
    G.carry = since;
}

/* This window's reports, now (after the flipped frame's: they count on
 * from it). */
static void reports_resolve(void)
{
    pend_retire();
    reports_write(G.queries, G.nq, G.clear_q, G.reps, G.nreps);
    G.clear_q = -1;
    G.nq = 0;
    G.nreps = 0;
}

/* Land the flipped frame: wait for it -- it has had the next frame's
 * recording to finish in -- then answer its reports and put its surfaces'
 * copy-backs in guest RAM. A surface drawn again since keeps its newer
 * contents marked dirty; one emptied meanwhile (alias_resolve) is skipped. */
static void pend_retire_inner(void)
{
    GPend *p = &G.pend;
    uint64_t t0;
    int i;
    if (!p->active)
        return;
    p->active = 0;
    t0 = perf_on() ? now_us() : 0;
    while (glClientWaitSync(p->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) == GL_TIMEOUT_EXPIRED)
        ;
    glDeleteSync(p->fence);
    p->fence = 0;
    if (t0) {
        uint64_t t = now_us();
        G.pf_wait_us += t - t0;
        t0 = t;                     /* write-back time is the landing alone */
    }
    gt_land();
    reports_write(p->queries, p->nq, p->clear_q, p->reps, p->nreps);
    if (p->nwb) {
        size_t total = p->wb[p->nwb - 1].off + (size_t)p->wb[p->nwb - 1].s->w
                       * p->wb[p->nwb - 1].rows * p->wb[p->nwb - 1].s->bpp;
        const uint8_t *m;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo);
        m = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)total,
                                              GL_MAP_READ_BIT);
        for (i = 0; m && i < p->nwb; i++) {
            GSurf *s = p->wb[i].s;
            uint32_t rows = p->wb[i].rows;
            size_t n = (size_t)s->w * rows * s->bpp;
            uint8_t *lin;
            if (s->tex != p->wb[i].tex || !s->dirty || !(lin = scratch(n)))
                continue;
            memcpy(lin, m + p->wb[i].off, n);
            surf_land(s, lin, rows, s->dirty == 2 && s->gen == p->wb[i].gen);
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        p->nwb = 0;
    }
    if (t0)
        G.pf_wb_us += now_us() - t0;
}

/* The flip, pipelined: the frame's copy-backs are queued on the GPU and its
 * reports kept, and both land a frame later (pend_retire) -- as the Vulkan
 * renderer does -- instead of waiting here for the GPU to finish the frame,
 * so the next frame is recorded while the GPU draws this one. */
static void flush_pipelined(void)
{
    GPend *p = &G.pend;
    size_t total = 0;
    uint64_t t0;
    int i;

    pend_retire();
    t0 = perf_on() ? now_us() : 0;
    p->nwb = 0;
    for (i = 0; i < G.nsurf; i++) {
        GSurf *s = &G.surf[i];
        uint32_t rows = surf_wb_rows(s);
        if (s->dirty != 1 || !rows || !writeback_needed(s))
            continue;
        if (!flip_writeback() && s->va != G.present_va && s->va != G.prev_present_va)
            continue;
        p->wb[p->nwb].s = s;
        p->wb[p->nwb].tex = s->tex;
        p->wb[p->nwb].gen = s->gen;
        p->wb[p->nwb].rows = rows;
        p->wb[p->nwb].off = total;
        total += ((size_t)s->w * rows * s->bpp + 255) & ~(size_t)255;
        p->nwb++;
    }
    if (p->nwb) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)total, NULL, GL_STREAM_READ);
        for (i = 0; i < p->nwb; i++) {
            G.st_wb++;
            G.pf_wb++;
            wb_why = "flip", surf_readpix(p->wb[i].s, p->wb[i].rows, p->wb[i].off);
            p->wb[i].s->dirty = 2;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    if (t0)
        G.pf_queue_us += now_us() - t0;
    p->queries = G.queries;
    p->nq = G.nq;
    p->clear_q = G.clear_q;
    p->nreps = G.nreps;
    memcpy(p->reps, G.reps, (size_t)G.nreps * sizeof G.reps[0]);
    G.queries = G.queries == G.qpool[0] ? G.qpool[1] : G.qpool[0];
    G.nq = 0;
    G.nreps = 0;
    G.clear_q = -1;
    gt_flip();
    p->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    p->active = 1;
    glFlush();
}

void nv2a_vk_zpass(int enable)
{
    if (G.state <= 0)
        return;
    G.zpass = enable;
}

void nv2a_vk_report_clear(void)
{
    if (G.state <= 0)
        return;
    G.clear_q = G.nq;
}

void nv2a_vk_report_get(uint32_t va, uint64_t stamp)
{
    if (G.state <= 0)
        return;
    if (G.nreps == GL_MAX_REPORTS)
        reports_resolve();
    G.reps[G.nreps].va = va;
    G.reps[G.nreps].stamp = stamp;
    G.reps[G.nreps].q0 = G.clear_q < 0 ? 0 : G.clear_q;
    G.reps[G.nreps].q1 = G.nq;
    G.reps[G.nreps].with_carry = G.clear_q < 0;
    G.nreps++;
}

void nv2a_vk_present(uint32_t va, uint32_t pitch)
{
    if (va != G.present_va)
        G.prev_present_va = G.present_va;
    G.present_va = va;
    G.present_pitch = pitch;
}

void nv2a_vk_present_queued(void)
{
    GSurf *s = NULL;
    int i, ww = s_win_w, wh = s_win_h, dw, dh, ox, oy;
    uint32_t sw, sh;
    float aspect;

    if (G.state <= 0 || G.own_win || !G.present_va)
        return;
    for (i = 0; i < G.nsurf; i++)
        if (G.surf[i].va == G.present_va && !G.surf[i].swz && G.surf[i].bpp == 4)
            s = &G.surf[i];
    if (!s || ww <= 0 || wh <= 0)
        return;
    sw = s->w;
    sh = s->rows ? s->rows : 480;
    aspect = (fb_widescreen() && sw * 3 == sh * 4) ? nv2a_vk_view_aspect() : (float)sw / (float)sh;
    if ((float)ww / (float)wh > aspect) {
        dh = wh;
        dw = (int)((float)wh * aspect + 0.5f);
    } else {
        dw = ww;
        dh = (int)((float)ww / aspect + 0.5f);
    }
    ox = (ww - dw) / 2;
    oy = (wh - dh) / 2;
    gt_mark(gt_label(GT_PRESENT, 0, 0, 0, 0));
    /* The surface was just copied back from (or uploaded to) its texture,
     * which holds the frame: row 0 is the top, so the blit turns it over. */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, G.rfbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    G.state_valid = 0;
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, (GLint)sw, (GLint)sh, ox, oy + dh, ox + dw, oy,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glEnable(GL_SCISSOR_TEST);
    {
        uint64_t t0 = perf_on() ? now_us() : 0;
        SDL_GL_SwapWindow(G.win);
        if (t0)
            G.pf_present_us += now_us() - t0;
    }
    G.cur_cs = NULL;                       /* the draw framebuffer binding moved */
    G.cur_zs = NULL;
}

/* ---- the rest of nv2a_vk.h --------------------------------------------- */

void *nv2a_vk_instance_handle(void) { return NULL; }
int   nv2a_vk_device_names(char (*names)[256], int max) { (void)names; (void)max; return 0; }
void  nv2a_vk_set_surface(uint64_t surface) { (void)surface; }
void  nv2a_vk_set_window_size(int w, int h) { s_win_w = w; s_win_h = h; }
int   nv2a_vk_attach_hwnd(void *hinstance, void *hwnd) { (void)hinstance; (void)hwnd; return 0; }
int   nv2a_vk_create_android_surface(void *anw, uint64_t *surface) { (void)anw; (void)surface; return 0; }
void  nv2a_vk_set_wayland(void *d, void *s) { (void)d; (void)s; }
void  nv2a_vk_set_gamma(const uint8_t *r, const uint8_t *g, const uint8_t *b) { (void)r; (void)g; (void)b; }
void  nv2a_vk_overlay_toggle(void) { }
/* nv2a_vk.c's own stubs for these are never linked in a GL build: nothing
 * else pulls that object in. */
int   nv2a_gamma_ramp(uint8_t out[3][256]) { (void)out; return 0; }
void  xbox_FlightDumpRequest(void) { }

#endif /* XBOXRECOMP_HAVE_GL && !XBOXRECOMP_HAVE_VULKAN */
