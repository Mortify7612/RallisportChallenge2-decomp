/**
 * NV2A on a host GPU, through Vulkan. See nv2a_vk.h.
 *
 * Structure. Everything runs on the executor thread. Work is recorded into a
 * "segment": an upload command buffer (texture and surface uploads) and a draw
 * command buffer, submitted together, upload first. A segment ends at a sync
 * point -- the flip, a texture that overlaps a surface the GPU has drawn, a
 * ring running out -- where it is submitted and waited for, every ring is
 * reset, and objects retired during it are destroyed. One sync a frame keeps
 * the lifetime rules trivial; overlapping CPU and GPU frames is a later step.
 *
 * Surfaces. Each guest colour surface (address, pitch, bpp) is a VkImage the
 * size of the surface; each zeta offset a depth/stencil image. Colour images
 * are filled from guest RAM when first used and whenever guest RAM changed
 * under a surface the GPU has not touched since its last copy-back, and are
 * copied back to guest RAM at the flip. Depth never leaves the GPU, as on
 * the real thing the title never reads it.
 *
 * Textures. Decoded on the CPU by the executor's sampler (every format it
 * knows: swizzled, DXT, palettised, YUV) into BGRA8 images, keyed by their
 * descriptor and the hash of their bytes, rechecked once per segment.
 * ponytail: decode is per texel and the hash covers every byte; upload BC
 * blocks directly and hash on write-watch if either shows in a profile.
 */
#include "nv2a_vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#if !defined(XBOXRECOMP_HAVE_VULKAN)

int  nv2a_vk_active(void) { return 0; }
void nv2a_vk_draw(const Nv2aVkState *s, const Nv2aVshOutput *v, uint32_t nv,
                  const uint32_t *idx, uint32_t nidx)
{ (void)s; (void)v; (void)nv; (void)idx; (void)nidx; }
void nv2a_vk_clear(const Nv2aVkState *s, uint32_t f, uint32_t c, uint32_t z)
{ (void)s; (void)f; (void)c; (void)z; }
void nv2a_vk_flush(void) { }
void nv2a_vk_present(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void nv2a_vk_zpass(int enable) { (void)enable; }
void nv2a_vk_set_gamma(const uint8_t *r, const uint8_t *g, const uint8_t *b) { (void)r; (void)g; (void)b; }
int nv2a_gamma_ramp(uint8_t out[3][256]) { (void)out; return 0; }
void nv2a_vk_overlay_toggle(void) { }
void xbox_FlightDumpRequest(void) { }
void nv2a_vk_report_clear(void) { }
void nv2a_vk_report_get(uint32_t va, uint64_t stamp) { (void)va; (void)stamp; }
void *nv2a_vk_instance_handle(void) { return NULL; }
int nv2a_vk_device_names(char (*names)[256], int max) { (void)names; (void)max; return 0; }
int nv2a_vk_attach_hwnd(void *hinstance, void *hwnd) { (void)hinstance; (void)hwnd; return 0; }
void nv2a_vk_set_surface(uint64_t surface) { (void)surface; }
void nv2a_vk_set_window_size(int w, int h) { (void)w; (void)h; }
int nv2a_vk_program(uint32_t hash, uint8_t inputs[16])
{ (void)hash; (void)inputs; return -1; }
void nv2a_vk_draw_program(const Nv2aVkState *s, uint32_t hash, const float *vin,
                          uint32_t nv, const uint32_t *idx, uint32_t nidx)
{ (void)s; (void)hash; (void)vin; (void)nv; (void)idx; (void)nidx; }

#else

#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR 1
#include <windows.h>
#endif
#include <vulkan/vulkan.h>

/* Android's libvulkan.so exports only the core functions of the API level
 * the app targets (1.1 at API 29), so the 1.2/1.3 ones the renderer uses are
 * looked up on the device once it exists (vk_load_device_fns), trying the
 * core name first and then the extension alias the same entry point had. */
#if defined(__ANDROID__)
#define VK_LATE_FNS(X) \
    X(vkCmdBeginRendering, "KHR") X(vkCmdEndRendering, "KHR") \
    X(vkCmdSetCullMode, "EXT") X(vkCmdSetFrontFace, "EXT") \
    X(vkCmdSetDepthTestEnable, "EXT") X(vkCmdSetDepthWriteEnable, "EXT") \
    X(vkCmdSetDepthCompareOp, "EXT") X(vkCmdSetStencilTestEnable, "EXT") \
    X(vkCmdSetStencilOp, "EXT") X(vkResetQueryPool, "EXT")
#define VK_LATE_DECL(name, sfx) static PFN_##name p_##name;
VK_LATE_FNS(VK_LATE_DECL)
#undef VK_LATE_DECL
#define vkCmdBeginRendering       p_vkCmdBeginRendering
#define vkCmdEndRendering         p_vkCmdEndRendering
#define vkCmdSetCullMode          p_vkCmdSetCullMode
#define vkCmdSetFrontFace         p_vkCmdSetFrontFace
#define vkCmdSetDepthTestEnable   p_vkCmdSetDepthTestEnable
#define vkCmdSetDepthWriteEnable  p_vkCmdSetDepthWriteEnable
#define vkCmdSetDepthCompareOp    p_vkCmdSetDepthCompareOp
#define vkCmdSetStencilTestEnable p_vkCmdSetStencilTestEnable
#define vkCmdSetStencilOp         p_vkCmdSetStencilOp
#define vkResetQueryPool          p_vkResetQueryPool

/* 0 if any is missing. */
static int vk_load_device_fns(VkDevice dev)
{
    int ok = 1;
#define VK_LATE_LOAD(name, sfx) \
    p_##name = (PFN_##name)vkGetDeviceProcAddr(dev, #name); \
    if (!p_##name) \
        p_##name = (PFN_##name)vkGetDeviceProcAddr(dev, #name sfx); \
    if (!p_##name) { \
        fprintf(stderr, "  [VK] %s not available\n", #name); \
        ok = 0; \
    }
    VK_LATE_FNS(VK_LATE_LOAD)
#undef VK_LATE_LOAD
    return ok;
}
#endif
#include "../d3d/d3d8_swizzle.h"
#if defined(XBOXRECOMP_HAVE_GLSLANG)
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

static const uint32_t s_vert_spv[] =
#include "nv2a_vert.spv.h"
;
static const uint32_t s_frag_spv[] =
#include "nv2a_frag.spv.h"
;
/* The same, built with NV2A_FAST_DEPTH: no gl_FragDepth write. */
static const uint32_t s_frag_fast_spv[] =
#include "nv2a_frag_fast.spv.h"
;
/* Its source, compiled again per combiner setup (spec_*). */
#include "nv2a_frag_src.h"
#include "nv2a_common_glsl_src.h"
#include <pthread.h>

#define VK_RING_VTX    (96u << 20)
#define VK_RING_IDX    (32u << 20)
#define VK_RING_UBO    (16u << 20)
#define VK_RING_STAGE  (256u << 20)
#define VK_RING_RB     (64u << 20)
#define VK_MAX_SETS    16384
#define VK_MAX_SURF    32
#define VK_MAX_ZETA    8
#define VK_MAX_PIPES   4096          /* power of two: hashed */
#define VK_MAX_PROGS   1024          /* power of two */
#define VK_MAX_SAMPLERS 512
#define VK_TEX_SLOTS   8192          /* power of two */
#define VK_MAX_RETIRED 4096
#define VK_SURF_H      1024
#define VK_MAX_QUERIES 4096
#define VK_MAX_REPORTS 512
/* Frame slots: the GPU draws one frame while the CPU records the next
 * (vk_flip). Each slot has its own command buffers, fences, descriptor pool,
 * query pools and half of every ring. */
#define VK_SLOTS 2

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    VkDeviceSize size, used;    /* size and use of one frame slot's half */
    VkDeviceSize base;          /* where the current slot's half starts */
    int coherent;
} Ring;

typedef struct Surf_ {
    uint32_t va, pitch, bpp, w, h;
    int swz;                    /* guest copy is swizzled (Morton order) */
    /* At an internal scale above 1, `img` is (w, h) * scale and `nimg` a
     * native-size twin that guest RAM is copied through. */
    VkImage nimg;
    VkDeviceMemory nmem;
    VkImageView nview;
    uint32_t *swx, *swy;        /* per-axis swizzle offsets, in texels */
    VkFormat fmt;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    int dirty;                  /* GPU has drawn since the last copy-back;
                                 * 2: and a submitted frame carries that
                                 * copy-back, nothing drawn since (vk_flip) */
    uint32_t rows;              /* rows the title uses (max clip bottom)  */
    uint32_t dirty_rows;
    uint64_t hash;              /* guest bytes as of the last copy either way */
    uint64_t hnow;              /* guest bytes this epoch (surf_hash_now) */
    uint32_t hnow_ep, hnow_bytes;
    int hash_valid;
    uint32_t last_use;
    uint32_t gen;               /* changes whenever its contents do */
    /* Since its last copy-back it has only been cleared, whole, to one
     * colour: the same bytes in any layout (alias_resolve). */
    int clear_only;
    uint32_t clear_argb;
    /* RECOMP_VK_MSAA: a multisampled twin the scene is drawn into, `msh`
     * title rows tall; every pass resolves it into `img`, which stays what
     * everything else reads. `ms_stale`: `img` was written directly (an
     * upload, a clear) and the twin must be refreshed from it first. */
    VkImage msimg;
    VkDeviceMemory msmem;
    VkImageView msview;
    uint32_t msh;
    int ms_stale;
} Surf;

/* A surface the title samples as a texture, copied on the GPU (rt_tex), or
 * a cube map whose six faces are surfaces (rt_cube). */
typedef struct {
    uint32_t va, w, h;          /* surface address; image size in pixels */
    int cube;
    uint32_t gen;               /* the surface's gen it was copied at */
    uint32_t fgen[6];           /* per face, for a cube */
    int init;                   /* layout is SHADER_READ_ONLY_OPTIMAL */
    uint32_t last_use;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view, view_x8;  /* view_x8: alpha reads 1 (X8R8G8B8) */
} RtTex;
#define VK_MAX_RTTEX 16
#define VK_KICKS 64

typedef struct {
    uint32_t key, w, h;
    uint32_t samples;           /* 1, or RECOMP_VK_MSAA for a twin's depth */
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkImageView dview;          /* depth only, for sampling (AO); lazy */
    uint32_t last_use;
} Zeta;

typedef struct {
    uint64_t key[4];            /* pipe_key; [3] = specialised shader, 0 none */
    VkPipeline pipe;            /* NULL while a worker builds it (spec) */
    int used;
    int failed;
} Pipe;

/* A title vertex program, compiled to a Vulkan vertex shader. */
typedef struct {
    uint32_t hash;
    int state;                  /* 0 empty, 1 compiled, -1 failed */
    VkShaderModule vs;
    uint32_t ninputs;
    uint8_t inputs[16];
} Prog;

typedef struct {
    uint64_t key;
    VkSampler s;
} Samp;

typedef struct {
    int used;
    Nv2aVkTex d;                /* descriptor (filter/address zeroed) */
    uint32_t bytes;
    uint64_t hash;
    uint32_t epoch;             /* segment it was last validated in */
    uint32_t last_use;          /* frame */
    uint32_t levels;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
} TexEnt;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
} Retired;

typedef struct {
    uint32_t va, q0, q1;
    uint64_t stamp, base;
    int pre;                    /* no CLEAR_REPORT_VALUE before it this frame */
} Rep;

/* A surface's copy-back recorded into a submitted frame. */
typedef struct {
    struct Surf_ *s;
    VkImage img;                /* the surface's image when it was recorded */
    VkDeviceSize off;
    uint32_t rows;
} RbEnt;

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer ucbs[VK_KICKS], dcbs[VK_KICKS];
    VkFence kfence[VK_KICKS], fence;
    VkDescriptorPool dpool;
    VkQueryPool tspool, qpool;
    VkSemaphore sem_acq, sem_done;
} Slot;

/* A frame submitted at a flip and not yet waited for (vk_flip). */
typedef struct {
    int active, slot, kick;
    uint32_t seq, nts, nq, nreps, rep_first;
    int cleared;
    uint8_t qsamples[VK_MAX_QUERIES];
    Rep reps[VK_MAX_REPORTS];
    RbEnt rb[VK_MAX_SURF];
    int nrb;
    Retired retired[VK_MAX_RETIRED];
    int nretired;
} Pend;

static struct {
    int state;                  /* 0 untried, 1 up, -1 off/failed */
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t qf;
    VkPhysicalDeviceMemoryProperties memp;
    VkPhysicalDeviceLimits limits;
    VkFormat dsfmt;
    int depth_clamp;
    float aniso;                    /* RECOMP_VK_ANISO, clamped; <= 1 off */
    uint32_t msaa;                  /* RECOMP_VK_MSAA sample count, 1 off */
    uint32_t cur_samples;           /* of the rendering instance */
    uint8_t qsamples[VK_MAX_QUERIES], fqsamples[VK_MAX_QUERIES];
    /* Fullscreen passes (post_*): one or two images in, push constants. */
    VkDescriptorSetLayout post_dsl;
    VkPipelineLayout post_pl;
    VkShaderModule post_vs;
    VkSampler post_samp;
    VkPipeline up_pipe;             /* resolved image -> multisampled twin */
    uint32_t st_ms_refresh, st_ao;
    /* The title's gamma ramp, applied when presenting (gamma_*). */
    uint8_t gm[3][256];         /* the one in use (presenter thread) */
    int gm_on;                  /* not the identity, and not RECOMP_GAMMA=0 */
    uint32_t gm_seq_seen;
    int gm_lut_dirty;
    VkImage gm_lut, gm_img;
    VkDeviceMemory gm_lut_mem, gm_img_mem;
    VkImageView gm_lut_view, gm_img_view;
    uint32_t gm_w, gm_h;
    VkPipeline gm_pipe;
    int dump_gamma;             /* the pending dump is pre-gamma: apply it */
    /* The FPS counter (overlay_*). */
    VkImage ov_img;
    VkDeviceMemory ov_mem;
    VkImageView ov_view;
    /* RECOMP_VK_AO: ambient occlusion on the presented scene (ao_*). */
    int ao_on;
    float ao_radius, ao_intensity, ao_bias, ao_far, ao_near, ao_tan;
    VkImage ao_img;
    VkDeviceMemory ao_mem;
    VkImageView ao_view;
    VkPipeline ao_pipe[2], ao_apply[2];   /* [1]: multisampled depth/target */
    uint32_t flip_va[2];                  /* the last two presented surfaces */
    uint32_t ao_frame, ao_zdraws, ao_done;
    VkCommandPool pool;
    VkCommandBuffer ucb, dcb;
    VkFence fence;
    /* Kicks (vk_kick): segments handed to the GPU without waiting, so it
     * draws while the CPU records the rest of the frame. Each has its own
     * command buffers and fence; the next sync waits for all of them. */
    VkCommandBuffer ucbs[VK_KICKS], dcbs[VK_KICKS];
    VkFence kfence[VK_KICKS];
    int kick;                   /* segments kicked since the last sync */
    VkQueryPool tspool;         /* GPU timestamps, two per segment (stats) */
    uint32_t nts;
    double gpu_ms;              /* GPU execution time since the last stats */
    uint32_t seg_draws;         /* draws recorded in this segment */
    int kick_draws;             /* RECOMP_VK_KICK: draws per kick, 0 = off */
    uint32_t st_kicks;
    uint32_t st_spec_draws, st_uber_draws;
    int recording;
    Ring vtx, idx, ubo, stage, rb;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkDescriptorPool dpool;
    uint32_t sets_used;
    VkShaderModule vs, fs;
    VkShaderModule fs_fast;     /* fs without the gl_FragDepth write */
    int wrev;                   /* w-buffer kept as WREV_C / w (float depth) */
    int bc_ok;                  /* DXT textures uploaded as BC (no decode) */
    VkPipelineCache pcache;
    Pipe pipes[VK_MAX_PIPES];
    int npipes;
    Prog progs[VK_MAX_PROGS];
    uint32_t const_serial, const_epoch;
    VkDeviceSize const_off;
    Samp samps[VK_MAX_SAMPLERS];
    int nsamps;
    Surf surf[VK_MAX_SURF];
    int nsurf;
    uint32_t gen;               /* source of Surf.gen values */
    RtTex rt[VK_MAX_RTTEX];
    int rt_direct;              /* RECOMP_VK_RT_DIRECT (default on) */
    Zeta zeta[VK_MAX_ZETA];
    int nzeta;
    TexEnt tex[VK_TEX_SLOTS];
    uint32_t ntex;
    Retired retired[VK_MAX_RETIRED];
    int nretired;
    /* white 1x1 stand-ins for unbound stages */
    VkImage white2d, whitecube;
    VkDeviceMemory white2d_mem, whitecube_mem;
    VkImageView white2d_view, whitecube_view;
    /* the rendering instance being recorded */
    int rendering;
    Surf *cur_cs;
    Zeta *cur_zs;
    uint32_t epoch, frame;
    uint32_t scale;             /* RECOMP_VK_SCALE: internal resolution */
    uint32_t sx;                /* horizontal scale: `scale`, or more for a
                                 * widescreen title's anamorphic frame */
    int wide_lines;             /* device can draw lines wider than 1 */
    uint32_t nimages;           /* live images (one memory allocation each) */
    /* Presentation: the window thread hands over a surface (fb_present.c);
     * the flip blits the finished frame into the swapchain. */
    int has_swapchain_ext;
    volatile uint64_t surface_handle;
    volatile int win_w, win_h;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_fmt;
    VkExtent2D sc_ext;
    VkImage sc_img[8];
    uint32_t sc_n, sc_idx;
    VkSemaphore sem_acq, sem_done;
    int present_pending, sc_dead;
    /* Visibility tests: one occlusion query per draw while counting is on;
     * a report sums the queries since CLEAR_REPORT_VALUE and is written to
     * guest RAM when the segment's results are back. */
    VkQueryPool qpool;
    uint32_t nq, rep_first, nreps, zpass;
    uint32_t st_reports, st_qdraws;
    VkQueryPool fqpool;          /* flight recorder: pixels per draw */
    uint32_t nfq, fq_frame[VK_MAX_QUERIES], fq_seq[VK_MAX_QUERIES];
    int precise;
    uint64_t rep_acc;
    Rep reps[VK_MAX_REPORTS];
    int rep_cleared;            /* a CLEAR_REPORT_VALUE this frame */
    /* Frame slots (vk_flip): the handles above (pool, ucbs, fence, dpool,
     * tspool, qpool, sem_*) are the current slot's, copied from here. */
    Slot slots[VK_SLOTS];
    int cur_slot;
    int pipelined;              /* RECOMP_VK_PIPELINE (default on) */
    uint32_t flip_seq;
    Pend pend[VK_SLOTS];        /* pend[i]: slot i's frame in flight */
    double st_wait_ms;          /* CPU time spent waiting on the GPU */
    double st_tex_ms;           /* decoding and staging textures */
    char htex[8][64];           /* this frame's texture uploads (hitch log) */
    int nhtex;
    uint32_t st_rt_copies;      /* surfaces copied on the GPU to sample */
    uint32_t st_alias_clears;   /* aliased clears carried over on the GPU */
    uint8_t *present_dump_px;
    const char *present_dump_path;
    uint32_t dump_w, dump_h;    /* the pending dump's size and byte order */
    int dump_bgr;
    /* last descriptor set, reused while the textures stay the same */
    VkDescriptorSet last_set;
    VkImageView last_views[8];
    VkSampler last_samps[8];
    /* stats */
    uint32_t st_draws, st_tris, st_syncs, st_tex_up, st_tex_hash_mb, st_surf_up;
    uint32_t st_compiles, st_pipes_new;
    double st_compile_ms, st_pipe_ms;
} V;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

/* ---- on-disk caches ---------------------------------------------------------
 *
 * Compiled vertex programs (SPIR-V) and the driver's pipeline cache, so a
 * second run does not stutter where the first compiled. RECOMP_VK_CACHE
 * names the directory; default $XDG_CACHE_HOME (or ~/.cache, or
 * %LOCALAPPDATA%) /xboxrecomp/vk. RECOMP_VK_CACHE=0 turns it off. */
#if defined(_WIN32)
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir_one(p) mkdir((p), 0755)
#endif

static const char *cache_dir(void)
{
    static char dir[1024];
    static int done;
    if (!done) {
        const char *e = getenv("RECOMP_VK_CACHE"), *base;
        char *q;
        done = 1;
        if (e && strcmp(e, "0") == 0)
            return NULL;
        if (e && *e)
            snprintf(dir, sizeof dir, "%s", e);
        else if ((base = getenv("XDG_CACHE_HOME")) && *base)
            snprintf(dir, sizeof dir, "%s/xboxrecomp/vk", base);
        else if ((base = getenv("LOCALAPPDATA")) && *base)
            snprintf(dir, sizeof dir, "%s/xboxrecomp/vk", base);
        else if ((base = getenv("HOME")) && *base)
            snprintf(dir, sizeof dir, "%s/.cache/xboxrecomp/vk", base);
        else
            return NULL;
        for (q = dir + 1; *q; q++)
            if (*q == '/' || *q == '\\') {
                char c = *q;
                *q = 0;
                mkdir_one(dir);
                *q = c;
            }
        mkdir_one(dir);
    }
    return dir[0] ? dir : NULL;
}

static void *file_read(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

static void file_write(const char *path, const void *a, size_t na,
                       const void *b, size_t nb)
{
    char tmp[1100];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f)
        return;
    fwrite(a, 1, na, f);
    if (b)
        fwrite(b, 1, nb, f);
    fclose(f);
    remove(path);                         /* Windows rename will not replace */
    rename(tmp, path);
}

static void pipeline_cache_save(void)
{
    const char *dir = cache_dir();
    char path[1100];
    size_t n = 0;
    void *buf;
    if (!dir || !V.pcache)
        return;
    if (vkGetPipelineCacheData(V.dev, V.pcache, &n, NULL) != VK_SUCCESS || !n)
        return;
    buf = malloc(n);
    if (buf && vkGetPipelineCacheData(V.dev, V.pcache, &n, buf) == VK_SUCCESS) {
        snprintf(path, sizeof path, "%s/pipelines.bin", dir);
        file_write(path, buf, n, NULL, 0);
    }
    free(buf);
}

/* RECOMP_WIDESCREEN: show 4:3-sized frames stretched to 16:9. */
static int fb_widescreen(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_WIDESCREEN");
        on = e && *e && strcmp(e, "0") != 0;
    }
    return on;
}

/* `s` contains `sub`, ignoring ASCII case. */
static int name_has(const char *s, const char *sub)
{
    size_t n = strlen(sub), i;
    for (; *s; s++) {
        for (i = 0; i < n; i++) {
            char a = s[i], b = sub[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (!a || a != b)
                break;
        }
        if (i == n)
            return 1;
    }
    return 0;
}

static int vk_fail(const char *what, VkResult r)
{
    fprintf(stderr, "  [VK] %s failed (%d)\n", what, (int)r);
    return 0;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < V.memp.memoryTypeCount; i++)
        if ((bits & (1u << i))
            && (V.memp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int alloc_mem(VkMemoryRequirements *req, VkMemoryPropertyFlags want,
                     VkDeviceMemory *out, VkMemoryPropertyFlags *got)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    uint32_t t = mem_type(req->memoryTypeBits, want);
    VkResult r;
    if (t == UINT32_MAX)
        return 0;
    ai.allocationSize = req->size;
    ai.memoryTypeIndex = t;
    r = vkAllocateMemory(V.dev, &ai, NULL, out);
    if (r != VK_SUCCESS)
        return vk_fail("vkAllocateMemory", r);
    if (got)
        *got = V.memp.memoryTypes[t].propertyFlags;
    return 1;
}

static int ring_init(Ring *rg, VkDeviceSize size, VkBufferUsageFlags usage,
                     int readback)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements req;
    VkMemoryPropertyFlags got = 0;
    VkResult r;
    bi.size = size * VK_SLOTS;        /* one half per frame slot */
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(V.dev, &bi, NULL, &rg->buf);
    if (r != VK_SUCCESS)
        return vk_fail("vkCreateBuffer", r);
    vkGetBufferMemoryRequirements(V.dev, rg->buf, &req);
    if (!(readback && alloc_mem(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                    | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &rg->mem, &got))
        && !alloc_mem(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                          | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &rg->mem, &got))
        return 0;
    vkBindBufferMemory(V.dev, rg->buf, rg->mem, 0);
    r = vkMapMemory(V.dev, rg->mem, 0, VK_WHOLE_SIZE, 0, (void **)&rg->map);
    if (r != VK_SUCCESS)
        return vk_fail("vkMapMemory", r);
    rg->size = size;
    rg->used = 0;
    rg->base = 0;
    rg->coherent = (got & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return 1;
}

/* Space in the current frame slot's half of a ring, or NULL when it is full
 * for this segment. Offsets are into the whole buffer. */
static void *ring_alloc(Ring *rg, VkDeviceSize n, VkDeviceSize align,
                        VkDeviceSize *off)
{
    VkDeviceSize o = (rg->used + align - 1) & ~(align - 1);
    if (o + n > rg->size)
        return NULL;
    rg->used = o + n;
    *off = rg->base + o;
    return rg->map + rg->base + o;
}

/* Samples for the next image_create; set around the multisampled ones. */
static uint32_t s_img_samples = 1;

static int image_create(uint32_t w, uint32_t h, uint32_t levels, uint32_t layers,
                        VkFormat fmt, VkImageUsageFlags usage, int cube,
                        VkImageAspectFlags aspect, VkImage *img,
                        VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements req;
    VkResult r;
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = (VkSampleCountFlagBits)s_img_samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = vkCreateImage(V.dev, &ci, NULL, img);
    if (r != VK_SUCCESS)
        return vk_fail("vkCreateImage", r);
    vkGetImageMemoryRequirements(V.dev, *img, &req);
    if (!alloc_mem(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem, NULL)) {
        vkDestroyImage(V.dev, *img, NULL);
        return 0;
    }
    vkBindImageMemory(V.dev, *img, *mem, 0);
    V.nimages++;
    vi.image = *img;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = layers;
    r = vkCreateImageView(V.dev, &vi, NULL, view);
    if (r != VK_SUCCESS) {
        vkDestroyImage(V.dev, *img, NULL);
        vkFreeMemory(V.dev, *mem, NULL);
        return vk_fail("vkCreateImageView", r);
    }
    return 1;
}

static void retire(VkImage img, VkDeviceMemory mem, VkImageView view)
{
    if (V.nretired == VK_MAX_RETIRED) {
        /* Cannot happen within one segment in practice; leak rather than
         * destroy something the GPU may still read. */
        return;
    }
    V.retired[V.nretired].img = img;
    V.retired[V.nretired].mem = mem;
    V.retired[V.nretired].view = view;
    V.nretired++;
}

static void barrier_image(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                          uint32_t levels, uint32_t layers,
                          VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags ss, VkAccessFlags sa,
                          VkPipelineStageFlags ds, VkAccessFlags da)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = levels;
    b.subresourceRange.layerCount = layers;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

static void segment_begin(void)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    if (V.recording)
        return;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(V.ucb, &bi);
    vkBeginCommandBuffer(V.dcb, &bi);
    V.recording = 1;
    V.seg_draws = 0;
    if (V.tspool && V.nts + 2 <= 2 * (VK_KICKS + 2))
        vkCmdWriteTimestamp(V.ucb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, V.tspool, V.nts);
    if (V.kick || V.pend[V.cur_slot ^ 1].active) {
        /* Segments already kicked -- or the flipped frame -- may still be
         * drawing with images this one's uploads overwrite: its transfers
         * wait for them. */
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(V.ucb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        /* And its draws see what the kicked ones drew. */
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
}

static void end_rendering(void)
{
    if (!V.rendering)
        return;
    vkCmdEndRendering(V.dcb);
    V.rendering = 0;
    V.cur_cs = NULL;
    V.cur_zs = NULL;
    V.cur_samples = 1;
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    /* Four independent lanes, so the multiplies overlap. Not a quality hash;
     * a change detector over memory the title rewrites wholesale. */
    uint64_t h0 = 0x9E3779B97F4A7C15ull, h1 = 0xC2B2AE3D27D4EB4Full;
    uint64_t h2 = 0x165667B19E3779F9ull, h3 = 0xD6E8FEB86659FD93ull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t a, b, c, d;
        memcpy(&a, p + i, 8); memcpy(&b, p + i + 8, 8);
        memcpy(&c, p + i + 16, 8); memcpy(&d, p + i + 24, 8);
        h0 = (h0 ^ a) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ b) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ c) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ d) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    for (; i < n; i++)
        h0 = (h0 ^ p[i]) * 0x100000001B3ull;
    return h0 ^ (h1 * 31) ^ (h2 * 1009) ^ (h3 * 65599) ^ n;
}

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

/* RECOMP_VK_DRAWLOG=<file>:<frame>[,<frame>...]: every draw and clear of
 * those presented frames, with a hash of each input -- vertex data, indices,
 * constants, every bound texture's bytes -- and the state. Two runs of the
 * same replay should log the same thing; where they do not is the bug. */
extern uint32_t nv2a_pb_presented_frames(void);
static FILE *s_dlog;
static int s_dlog_on = -1;
static uint32_t s_dlog_frames[64];
static int s_dlog_n;
static uint32_t s_dlog_seq;


/* RECOMP_VK_PROBE=<x>,<y>:<frame>,...: in those frames, the colour of one
 * screen pixel (title coordinates) after every draw to the 640-wide main
 * surface, logged with the draw -- which draw painted it, and with what. */
static int s_probe_on = -1, s_probe_x, s_probe_y, s_probe_nf;
static uint32_t s_probe_frames[64];
static struct { VkDeviceSize off; uint32_t seq, prog; uint32_t f; int bpp; } s_probe_q[4096];
static int s_probe_nq;

static int probe_active(void)
{
    uint32_t f;
    int i;
    if (s_probe_on < 0) {
        const char *e = getenv("RECOMP_VK_PROBE"), *c;
        s_probe_on = 0;
        if (e && sscanf(e, "%d,%d", &s_probe_x, &s_probe_y) == 2 && (c = strchr(e, ':'))) {
            for (c++; *c && s_probe_nf < 64; ) {
                s_probe_frames[s_probe_nf++] = (uint32_t)strtoul(c, (char **)&c, 10);
                while (*c == ',') c++;
            }
            s_probe_on = 1;
        }
    }
    if (!s_probe_on)
        return 0;
    f = nv2a_pb_presented_frames();
    for (i = 0; i < s_probe_nf; i++)
        if (s_probe_frames[i] == f)
            return 1;
    return 0;
}

/* RECOMP_VK_FLIGHT=<dir>: a flight recorder. The draw log of the last
 * FLIGHT_FRAMES presented frames is kept in memory (texture contents not
 * hashed, so it is cheap enough to leave on while playing); F10 in the
 * window (xbox_FlightDumpRequest) writes it to <dir>/flight_<frame>.txt.
 * For catching something that happens now and then, which no replay
 * reaches reliably. */
#define FLIGHT_FRAMES 180
static int s_flight = -1;
static char s_flight_dir[512];
static struct { char *buf; size_t len; uint32_t frame; } s_fl[FLIGHT_FRAMES];
static uint32_t s_fl_cur = 0xFFFFFFFFu;
static volatile int s_fl_dump;

void xbox_FlightDumpRequest(void) { s_fl_dump = 1; }

static void flight_write(uint32_t now)
{
    char path[600];
    FILE *f;
    uint32_t k;
    snprintf(path, sizeof path, "%s/flight_%06u.txt", s_flight_dir, now);
    f = fopen(path, "w");
    if (!f)
        return;
    for (k = 1; k <= FLIGHT_FRAMES; k++) {
        uint32_t slot = (now + k) % FLIGHT_FRAMES;
        if (s_fl[slot].buf && s_fl[slot].frame < now)
            fwrite(s_fl[slot].buf, 1, s_fl[slot].len, f);
    }
    fclose(f);
    fprintf(stderr, "  [VK] flight recorder: wrote %s\n", path);
}

static int flight_active(void)
{
    uint32_t f;
    if (s_flight < 0) {
        const char *e = getenv("RECOMP_VK_FLIGHT");
        s_flight = e && *e;
#if defined(_WIN32)
        if (s_flight)
            fprintf(stderr, "  [VK] flight recorder needs open_memstream -- not on Windows\n");
        s_flight = 0;
#endif
        if (s_flight) {
            snprintf(s_flight_dir, sizeof s_flight_dir, "%s", e);
            fprintf(stderr, "  [VK] flight recorder on (F10 writes the last %u frames to %s)\n",
                    FLIGHT_FRAMES, e);
        }
    }
    if (!s_flight)
        return 0;
    f = nv2a_pb_presented_frames();
    if (f != s_fl_cur) {
        uint32_t slot = f % FLIGHT_FRAMES;
        if (s_dlog) {
            fclose(s_dlog);                   /* finalises the previous slot */
            s_dlog = NULL;
        }
        {
            /* RECOMP_VK_FLIGHT_AT=<frame>: dump then, as F10 would. */
            static long at = -2;
            if (at == -2) {
                const char *e = getenv("RECOMP_VK_FLIGHT_AT");
                at = e ? atol(e) : -1;
            }
            if (at >= 0 && f == (uint32_t)at)
                s_fl_dump = 1;
        }
        if (s_fl_dump) {
            s_fl_dump = 0;
            flight_write(f);
        }
        free(s_fl[slot].buf);
        s_fl[slot].buf = NULL;
        s_fl[slot].len = 0;
        s_fl[slot].frame = f;
#if !defined(_WIN32)
        s_dlog = open_memstream(&s_fl[slot].buf, &s_fl[slot].len);
#endif
        s_fl_cur = f;
    }
    return s_dlog != NULL;
}

static int dlog_active(void)
{
    uint32_t f;
    int i;
    if (s_flight != 0 && flight_active())
        return 1;
    if (s_dlog_on < 0) {
        const char *e = getenv("RECOMP_VK_DRAWLOG"), *c;
        s_dlog_on = 0;
        if (e && (c = strrchr(e, ':'))) {
            char path[512];
            size_t n = (size_t)(c - e) < sizeof path - 1 ? (size_t)(c - e) : sizeof path - 1;
            memcpy(path, e, n);
            path[n] = 0;
            for (c++; *c && s_dlog_n < 64; ) {
                s_dlog_frames[s_dlog_n++] = (uint32_t)strtoul(c, (char **)&c, 10);
                while (*c == ',') c++;
            }
            s_dlog = fopen(path, "w");
            s_dlog_on = s_dlog != NULL;
        }
    }
    if (!s_dlog_on)
        return 0;
    f = nv2a_pb_presented_frames();
    for (i = 0; i < s_dlog_n; i++)
        if (s_dlog_frames[i] == f)
            return 1;
    return 0;
}

extern uint32_t g_nv2a_draw_pb_va;

static void dlog_draw(const Nv2aVkState *st, uint32_t prog, const void *v,
                      size_t vbytes, const uint32_t *idx, uint32_t nidx)
{
    int i;
    fprintf(s_dlog, "F%u D%u prog %08X surf %08X/%u swz %u zeta %08X clip %u,%u,%u,%u"
            " v %016llX/%zu i %016llX/%u c %016llX rc %016llX"
            " blend %u %X %X %X cm %08X depth %u %X %u w%u st %u %X %X cull %u %X %X at %u %X %X"
            " sp %05X va %08X zoff %g %g pb %08X",
            nv2a_pb_presented_frames(), s_dlog_seq++, prog, st->color_va, st->bpp,
            st->swz_w, st->zeta_key, st->clip_x, st->clip_y, st->clip_w, st->clip_h,
            (unsigned long long)hash_bytes((const uint8_t *)v, vbytes), vbytes,
            (unsigned long long)hash_bytes((const uint8_t *)idx, (size_t)nidx * 4), nidx,
            (unsigned long long)hash_bytes((const uint8_t *)nv2a_vsh_constants(), NV2A_VSH_CONSTANTS * 16),
            (unsigned long long)hash_bytes((const uint8_t *)st->rc, sizeof *st->rc),
            st->blend_enable, st->blend_sfactor, st->blend_dfactor, st->blend_equation,
            st->color_mask, st->depth_test, st->depth_func, st->depth_mask, st->wbuffer,
            st->st_test, st->st_func, st->st_ref, st->cull_enable, st->cull_face,
            st->front_face, st->alpha_test, st->alpha_func, st->alpha_ref,
            st->rc ? st->rc->stage_program : 0, st->vtx_va,
            st->poff_factor, st->poff_units, g_nv2a_draw_pb_va);
    for (i = 0; i < 4; i++) {
        const Nv2aVkTex *t = &st->tex[i];
        uint32_t bytes;
        if (!t->valid)
            continue;
        bytes = nv2a_pb_tex_bytes(t);
        fprintf(s_dlog, " t%d %08X f%02X %ux%u l%u/%u c%d flt %08X maxlod %03X %016llX", i, t->offset, t->color,
                t->width, t->height, t->min_level, t->levels, t->cube, t->filter, t->max_lod,
                bytes && !s_flight ? (unsigned long long)hash_bytes(guest(t->offset), bytes) : 0ull);
    }
    fputc('\n', s_dlog);
    /* The combiner words themselves, once per distinct set. */
    if (st->rc) {
        static uint64_t seen[256];
        static unsigned nseen;
        uint64_t h = hash_bytes((const uint8_t *)st->rc, sizeof *st->rc);
        unsigned k;
        for (k = 0; k < nseen && seen[k] != h; k++)
            ;
        if (k == nseen && nseen < 256) {
            const uint32_t *w = (const uint32_t *)st->rc;
            seen[nseen++] = h;
            fprintf(s_dlog, "RC %016llX", (unsigned long long)h);
            for (k = 0; k < sizeof *st->rc / 4; k++)
                fprintf(s_dlog, " %08X", w[k]);
            fputc('\n', s_dlog);
        }
    }
}

static size_t surf_bytes(const Surf *s)
{
    return s->swz ? (size_t)s->w * s->h * s->bpp : (size_t)s->rows * s->pitch;
}

static uint64_t surf_hash(const Surf *s)
{
    return hash_bytes(guest(s->va), surf_bytes(s));
}

/* surf_hash, computed at most once an epoch, as textures are (tex_get):
 * a cube map's six faces were hashed again for every draw that sampled
 * it. Our own writes to the surface's bytes (write_back) update it. */
static uint64_t surf_hash_now(Surf *s)
{
    uint32_t bytes = surf_bytes(s);
    if (s->hnow_ep != V.epoch + 1 || s->hnow_bytes != bytes) {
        s->hnow = hash_bytes(guest(s->va), bytes);
        s->hnow_ep = V.epoch + 1;
        s->hnow_bytes = bytes;
    }
    return s->hnow;
}

/* Between a swizzled guest surface and linear rows: `to_guest` swizzles
 * rows into guest, otherwise unswizzles guest into rows. */
static void surf_swizzle(Surf *s, uint8_t *g, uint8_t *lin, uint32_t rows, int to_guest)
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
        if (s->bpp == 4) {
            uint32_t *gp = (uint32_t *)g, *rp = (uint32_t *)row;
            for (x = 0; x < s->w; x++) {
                if (to_guest) gp[s->swy[y] | s->swx[x]] = rp[x];
                else rp[x] = gp[s->swy[y] | s->swx[x]];
            }
        } else {
            uint16_t *gp = (uint16_t *)g, *rp = (uint16_t *)row;
            for (x = 0; x < s->w; x++) {
                if (to_guest) gp[s->swy[y] | s->swx[x]] = rp[x];
                else rp[x] = gp[s->swy[y] | s->swx[x]];
            }
        }
    }
}

/* ---- sync points -------------------------------------------------------- */

static void destroy_list(const Retired *r, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (r[i].view) vkDestroyImageView(V.dev, r[i].view, NULL);
        if (r[i].img)  vkDestroyImage(V.dev, r[i].img, NULL);
        if (r[i].mem) {
            vkFreeMemory(V.dev, r[i].mem, NULL);
            V.nimages--;
        }
    }
}

static void destroy_retired(void)
{
    destroy_list(V.retired, V.nretired);
    V.nretired = 0;
}

/* A segment's occlusion results into the reports that asked for them, from
 * query pool `qp`. Returns the pixels counted after the segment's last
 * CLEAR_REPORT_VALUE (from query `rep_first`), which a later report with no
 * clear of its own adds on. */
static uint64_t reports_write(VkQueryPool qp, uint32_t nq, const uint8_t *qsamples,
                              const Rep *reps, uint32_t nreps, uint32_t rep_first)
{
    static uint64_t res[VK_MAX_QUERIES + 1];
    uint32_t i, q, div = V.sx * V.scale;
    if (nq) {
        uint64_t raw[VK_MAX_QUERIES];
        if (vkGetQueryPoolResults(V.dev, qp, 0, nq, sizeof raw, raw, 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)
                != VK_SUCCESS)
            memset(raw, 0, sizeof raw);
        res[0] = 0;
        for (q = 0; q < nq; q++)              /* prefix sums, in pixels */
            res[q + 1] = res[q] + raw[q] / (qsamples[q] > 1 ? qsamples[q] : 1);
    } else {
        res[0] = 0;
    }
    for (i = 0; i < nreps; i++) {
        uint64_t n = reps[i].base + res[reps[i].q1] - res[reps[i].q0];
        uint32_t count = (uint32_t)(n / div), done = 0;
        uint8_t *g = guest(reps[i].va);
        static int trace = -1;
        static uint32_t seen;
        if (trace < 0)
            trace = getenv("RECOMP_REPORT_TRACE") != NULL;
        if (trace && ((seen++ % 200) < 12 || getenv("RECOMP_REPORT_TRACE_ALL")))
            fprintf(stderr, "[REPORT] vk at 0x%08X: %u pixels (%u queries)\n",
                    reps[i].va, count, reps[i].q1 - reps[i].q0);
        memcpy(g, &reps[i].stamp, 8);
        memcpy(g + 8, &count, 4);
        memcpy(g + 12, &done, 4);
    }
    if (nq)
        vkResetQueryPool(V.dev, qp, 0, nq);
    return res[nq] - res[rep_first];
}

/* The current segment's reports, once it has been waited for. */
static void reports_resolve(void)
{
    V.rep_acc += reports_write(V.qpool, V.nq, V.qsamples, V.reps, V.nreps, V.rep_first);
    V.nreps = 0;
    V.rep_first = 0;
    V.nq = 0;
}

/* RECOMP_VK_ALPHA_WATCH=<hex va>:<hex bytes>: report which surface
 * copy-back changes the alpha bytes of that range (32bpp data assumed), and
 * whether something else changed them between syncs. */
static long s_aw_va = -2;
static uint32_t s_aw_n;
static uint8_t *s_aw_prev;
static uint32_t aw_check(const char *what, const Surf *s)
{
    uint32_t k, changed = 0, first = 0;
    const uint8_t *g;
    if (s_aw_va == -2) {
        const char *e = getenv("RECOMP_VK_ALPHA_WATCH"), *c;
        s_aw_va = -1;
        if (e && (c = strchr(e, ':'))) {
            s_aw_va = (long)strtoul(e, NULL, 16);
            s_aw_n = (uint32_t)strtoul(c + 1, NULL, 16) / 4;
            s_aw_prev = (uint8_t *)malloc(s_aw_n);
            g = guest((uint32_t)s_aw_va);
            for (k = 0; k < s_aw_n; k++)
                s_aw_prev[k] = g[k * 4 + 3];
        }
    }
    if (s_aw_va < 0)
        return 0;
    g = guest((uint32_t)s_aw_va);
    for (k = 0; k < s_aw_n; k++)
        if (s_aw_prev[k] != g[k * 4 + 3]) {
            if (!changed++) first = k;
            s_aw_prev[k] = g[k * 4 + 3];
        }
    if (changed) {
        static unsigned shown;
        if (shown++ < 300) {
            if (s)
                fprintf(stderr, "[VKAW] F%u %s surf %08X %ux%u pitch %u swz %d bpp %u fmt %d: %u alpha bytes changed, first at %08X\n",
                        nv2a_pb_presented_frames(), what, s->va, s->w, s->h, s->pitch, s->swz,
                        s->bpp, (int)s->fmt, changed, (uint32_t)s_aw_va + first * 4);
            else
                fprintf(stderr, "[VKAW] F%u %s: %u alpha bytes changed, first at %08X\n",
                        nv2a_pb_presented_frames(), what, changed, (uint32_t)s_aw_va + first * 4);
        }
    }
    return changed;
}

/* Submit the segment and wait for it. With `readback`, every dirty surface
 * is copied back into guest RAM first. */
/* Hand the segment recorded so far to the GPU and go on recording, without
 * waiting: the GPU draws the start of the frame while the CPU works on the
 * rest. Rings, descriptor sets and retired images are only recycled at the
 * next sync, which waits for every kicked segment. */
static void vk_kick(void)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBuffer cbs[2];
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    if (!V.recording || V.present_pending || V.kick + 1 >= VK_KICKS)
        return;
    end_rendering();
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(V.ucb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (V.tspool && V.nts + 2 <= 2 * (VK_KICKS + 2)) {
        vkCmdWriteTimestamp(V.dcb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V.tspool, V.nts + 1);
        V.nts += 2;
    }
    vkEndCommandBuffer(V.ucb);
    vkEndCommandBuffer(V.dcb);
    cbs[0] = V.ucb;
    cbs[1] = V.dcb;
    si.commandBufferCount = 2;
    si.pCommandBuffers = cbs;
    if (vkQueueSubmit(V.q, 1, &si, V.kfence[V.kick]) != VK_SUCCESS) {
        vk_fail("vkQueueSubmit (kick)", VK_ERROR_DEVICE_LOST);
        V.state = -1;
        return;
    }
    V.kick++;
    V.ucb = V.ucbs[V.kick];
    V.dcb = V.dcbs[V.kick];
    V.recording = 0;
    V.st_kicks++;
}

static void vk_sync_inner(int readback);
/* RECOMP_VK_SYNC_TRACE: who ends each segment (caller address; feed it to
 * addr2line -f -e rsc2). */
#define vk_sync(rb) do { \
        static int t_ = -1; \
        if (t_ < 0) t_ = getenv("RECOMP_VK_SYNC_TRACE") != NULL; \
        if (t_ && V.recording) fprintf(stderr, "[VKSYNC] F%u line %d rb %d\n", \
                                       nv2a_pb_presented_frames(), __LINE__, (rb)); \
        vk_sync_inner(rb); \
    } while (0)

/* End the segment and submit it, with the surfaces' copy-backs when
 * `readback` (listed in `list`). Does not wait. 0 on failure. */
static int segment_submit(int readback, RbEnt *list, int *nrb)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkCommandBuffer cbs[2];
    int i, any = 0;
    VkResult r;

    *nrb = 0;
    aw_check("between syncs (not a copy-back)", NULL);
    end_rendering();
    if (readback) {
        for (i = 0; i < V.nsurf; i++) {
            Surf *s = &V.surf[i];
            VkBufferImageCopy c;
            VkDeviceSize off;
            uint32_t rows = s->dirty_rows < s->h ? s->dirty_rows : s->h;
            if (s->dirty != 1 || !rows)   /* 2: already on its way */
                continue;
            if (!ring_alloc(&V.rb, (VkDeviceSize)s->w * rows * s->bpp, 16, &off))
                continue;         /* ponytail: 64 MB of surfaces per sync */
            if (!any) {
                VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb,
                                     0, NULL, 0, NULL);
            }
            any = 1;
            if (V.sx > 1 && s->nimg) {
                /* Down to native size first: guest RAM holds the title's
                 * resolution, whatever the GPU renders at. */
                VkImageBlit b;
                VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                memset(&b, 0, sizeof b);
                b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
                b.srcOffsets[1].x = (int32_t)(s->w * V.sx);
                b.srcOffsets[1].y = (int32_t)(rows * V.scale);
                b.srcOffsets[1].z = 1;
                b.dstOffsets[1].x = (int32_t)s->w;
                b.dstOffsets[1].y = (int32_t)rows;
                b.dstOffsets[1].z = 1;
                vkCmdBlitImage(V.dcb, s->img, VK_IMAGE_LAYOUT_GENERAL, s->nimg,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
                mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb,
                                     0, NULL, 0, NULL);
            }
            memset(&c, 0, sizeof c);
            c.bufferOffset = off;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageExtent.width = s->w;
            c.imageExtent.height = rows;
            c.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(V.dcb, V.sx > 1 && s->nimg ? s->nimg : s->img,
                                   VK_IMAGE_LAYOUT_GENERAL, V.rb.buf, 1, &c);
            list[*nrb].s = s;
            list[*nrb].img = s->img;
            list[*nrb].off = off;
            list[*nrb].rows = rows;
            (*nrb)++;
        }
        if (any) {
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb,
                                 0, NULL, 0, NULL);
        }
    }
    {
        /* Uploads made in this segment are read by the draws after them. */
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(V.ucb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb,
                             0, NULL, 0, NULL);
    }
    if (V.tspool && V.nts + 2 <= 2 * (VK_KICKS + 2)) {
        vkCmdWriteTimestamp(V.dcb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V.tspool, V.nts + 1);
        V.nts += 2;
    }
    vkEndCommandBuffer(V.ucb);
    vkEndCommandBuffer(V.dcb);
    cbs[0] = V.ucb;
    cbs[1] = V.dcb;
    si.commandBufferCount = 2;
    si.pCommandBuffers = cbs;
    {
        static const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        if (V.present_pending) {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &V.sem_acq;
            si.pWaitDstStageMask = &wait_stage;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &V.sem_done;
        }
    }
    r = vkQueueSubmit(V.q, 1, &si, V.fence);
    if (r == VK_SUCCESS && V.present_pending) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        VkResult pr;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &V.sem_done;
        pi.swapchainCount = 1;
        pi.pSwapchains = &V.swapchain;
        pi.pImageIndices = &V.sc_idx;
        pr = vkQueuePresentKHR(V.q, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
            V.sc_dead = 1;
        V.present_pending = 0;
    }
    if (r != VK_SUCCESS) {
        vk_fail("vkQueueSubmit", r);
        V.state = -1;
        return 0;
    }
    return 1;
}

/* Wait for slot `sl`'s frame: its final segment and `kick` kicked ones;
 * then its command buffers can be reused. */
static void slot_wait(Slot *sl, int kick, uint32_t nts)
{
    VkResult r;
    {
        double t0 = now_ms();
        r = vkWaitForFences(V.dev, 1, &sl->fence, VK_TRUE, UINT64_MAX);
        V.st_wait_ms += now_ms() - t0;
    }
    if (r != VK_SUCCESS) {
        vk_fail("vkWaitForFences (device lost?)", r);
        V.state = -1;
        return;
    }
    vkResetFences(V.dev, 1, &sl->fence);
    if (kick) {
        double t0 = now_ms();
        vkWaitForFences(V.dev, (uint32_t)kick, sl->kfence, VK_TRUE, UINT64_MAX);
        V.st_wait_ms += now_ms() - t0;
        vkResetFences(V.dev, (uint32_t)kick, sl->kfence);
    }
    if (sl->tspool && nts) {
        uint64_t ts[2 * (VK_KICKS + 2)];
        uint32_t k;
        if (vkGetQueryPoolResults(V.dev, sl->tspool, 0, nts, sizeof ts, ts, 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
            for (k = 0; k + 1 < nts; k += 2)
                if (ts[k + 1] > ts[k])
                    V.gpu_ms += (double)(ts[k + 1] - ts[k]) * V.limits.timestampPeriod * 1e-6;
        vkResetQueryPool(V.dev, sl->tspool, 0, nts);
    }
    vkResetCommandPool(V.dev, sl->pool, 0);
}

/* Copy-backs that have landed into guest RAM. `deferred`: they are a
 * flipped frame's, and a surface drawn again since stays dirty. */
static void write_back(const RbEnt *list, int n, int deferred)
{
    int i;
    if (!n)
        return;
    if (!V.rb.coherent) {
        VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
        mr.memory = V.rb.mem;
        mr.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(V.dev, 1, &mr);
    }
    /* The bytes change under any surface aliasing them, too. */
    for (i = 0; i < V.nsurf; i++)
        V.surf[i].hnow_ep = 0;
    for (i = 0; i < n; i++) {
        Surf *s = list[i].s;
        uint32_t y, rows = list[i].rows;
        /* Gone (evicted, recreated), or emptied meanwhile (alias_resolve
         * moved its contents to another surface). */
        if (s->img != list[i].img || !s->dirty)
            continue;
        if (s->swz)
            surf_swizzle(s, guest(s->va), V.rb.map + list[i].off, rows, 1);
        else
            for (y = 0; y < rows; y++)
                memcpy(guest(s->va) + (size_t)y * s->pitch,
                       V.rb.map + list[i].off + (size_t)y * s->w * s->bpp,
                       (size_t)s->w * s->bpp);
        aw_check("copy-back", s);
        if (!deferred || s->dirty == 2) {
            s->dirty = 0;
            s->dirty_rows = 0;
        }
        s->hash = surf_hash_now(s);
        s->hash_valid = 1;
    }
}

/* The current slot's handles and ring halves. */
static void slot_use(int i)
{
    Slot *sl = &V.slots[i];
    V.cur_slot = i;
    V.pool = sl->pool;
    memcpy(V.ucbs, sl->ucbs, sizeof V.ucbs);
    memcpy(V.dcbs, sl->dcbs, sizeof V.dcbs);
    memcpy(V.kfence, sl->kfence, sizeof V.kfence);
    V.fence = sl->fence;
    V.dpool = sl->dpool;
    V.tspool = sl->tspool;
    V.qpool = sl->qpool;
    V.sem_acq = sl->sem_acq;
    V.sem_done = sl->sem_done;
    V.ucb = V.ucbs[0];
    V.dcb = V.dcbs[0];
    V.vtx.base = V.vtx.size * (VkDeviceSize)i;
    V.idx.base = V.idx.size * (VkDeviceSize)i;
    V.ubo.base = V.ubo.size * (VkDeviceSize)i;
    V.stage.base = V.stage.size * (VkDeviceSize)i;
    V.rb.base = V.rb.size * (VkDeviceSize)i;
    V.vtx.used = V.idx.used = V.ubo.used = V.stage.used = V.rb.used = 0;
}

/* Finish a flipped frame: wait for it, land its copy-backs and reports,
 * and free its slot. */
static void pend_retire(Pend *p)
{
    Slot *sl;
    Pend *newer;
    uint64_t delta;
    uint32_t i;
    int chain = 0;

    if (!p->active)
        return;
    p->active = 0;
    sl = &V.slots[p->slot];
    slot_wait(sl, p->kick, p->nts);
    write_back(p->rb, p->nrb, 1);
    delta = reports_write(sl->qpool, p->nq, p->qsamples, p->reps, p->nreps, p->rep_first);
    /* Reports made since, with no clear of their own before them, count
     * on from this frame's tail; they were recorded without it. */
    newer = &V.pend[p->slot ^ 1];
    if (newer->active && newer->seq > p->seq) {
        for (i = 0; i < newer->nreps; i++)
            if (newer->reps[i].pre)
                newer->reps[i].base += delta;
        chain = newer->cleared;
    }
    if (!chain) {
        for (i = 0; i < V.nreps; i++)
            if (V.reps[i].pre)
                V.reps[i].base += delta;
        if (!V.rep_cleared)
            V.rep_acc += delta;
    }
    vkResetDescriptorPool(V.dev, sl->dpool, 0);
    destroy_list(p->retired, p->nretired);
    p->nretired = 0;
}

/* The flipped frame still in flight, if any. */
static void pend_retire_all(void)
{
    int i;
    for (i = 0; i < VK_SLOTS; i++)
        if (i != V.cur_slot)
            pend_retire(&V.pend[i]);
}

/* Submit the segment and wait for it. With `readback`, every dirty surface
 * is copied back into guest RAM first. */
static void vk_sync_inner(int readback)
{
    static RbEnt list[VK_MAX_SURF];
    int nrb = 0;

    /* A flipped frame's copy-backs and reports first: whoever asked for
     * this sync may be about to read them. */
    pend_retire_all();
    if (!V.recording)
        return;
    if (!segment_submit(readback, list, &nrb))
        return;
    slot_wait(&V.slots[V.cur_slot], V.kick, V.nts);
    V.kick = 0;
    V.nts = 0;
    V.ucb = V.ucbs[0];
    V.dcb = V.dcbs[0];
    V.recording = 0;
    V.st_syncs++;
    write_back(list, nrb, 0);
    reports_resolve();
    if (V.nfq) {
        /* Flight recorder: the pass counts, as "Q F<frame> D<draw> <samples>"
         * lines next to the draws they belong to. */
        static uint64_t raw[VK_MAX_QUERIES];
        uint32_t k;
        if (vkGetQueryPoolResults(V.dev, V.fqpool, 0, V.nfq, sizeof raw, raw, 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS
                && s_dlog)
            for (k = 0; k < V.nfq; k++)
                fprintf(s_dlog, "Q F%u D%u %llu\n", V.fq_frame[k], V.fq_seq[k],
                        (unsigned long long)(raw[k] / (V.fqsamples[k] > 1 ? V.fqsamples[k] : 1)));
        vkResetQueryPool(V.dev, V.fqpool, 0, V.nfq);
        V.nfq = 0;
    }
    if (s_probe_nq) {
        int k;
        uint32_t lastc = 0xFFFFFFFFu;
        if (!V.rb.coherent) {
            VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
            mr.memory = V.rb.mem;
            mr.size = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(V.dev, 1, &mr);
        }
        for (k = 0; k < s_probe_nq; k++) {
            uint32_t c = 0;
            memcpy(&c, V.rb.map + s_probe_q[k].off, s_probe_q[k].bpp == 2 ? 2 : 4);
            if (c != lastc)
                fprintf(stderr, "[VKPROBE] F%u after D%u prog %08X pixel %d,%d = %08X\n",
                        s_probe_q[k].f, s_probe_q[k].seq, s_probe_q[k].prog,
                        s_probe_x, s_probe_y, c);
            lastc = c;
        }
        s_probe_nq = 0;
    }
    if (V.present_dump_px) {
        FILE *f = fopen(V.present_dump_path, "wb");
        uint32_t w = V.dump_w, h = V.dump_h, y, x, row = (w * 3 + 3) & ~3u;
        if (!V.rb.coherent) {
            VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
            mr.memory = V.rb.mem;
            mr.size = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(V.dev, 1, &mr);
        }
        if (f) {
            uint8_t hdr[54], *line = (uint8_t *)calloc(1, row);
            uint32_t total = 54 + row * h, img = row * h;
            int bgr = V.dump_bgr;
            memset(hdr, 0, sizeof hdr);
            hdr[0] = 'B'; hdr[1] = 'M';
            memcpy(hdr + 2, &total, 4);
            hdr[10] = 54; hdr[14] = 40;
            memcpy(hdr + 18, &w, 4);
            memcpy(hdr + 22, &h, 4);
            hdr[26] = 1; hdr[28] = 24;
            memcpy(hdr + 34, &img, 4);
            fwrite(hdr, 1, 54, f);
            for (y = 0; line && y < h; y++) {
                const uint8_t *src = V.present_dump_px + (size_t)(h - 1 - y) * w * 4;
                for (x = 0; x < w; x++) {
                    line[x * 3 + 0] = src[x * 4 + (bgr ? 0 : 2)];
                    line[x * 3 + 1] = src[x * 4 + 1];
                    line[x * 3 + 2] = src[x * 4 + (bgr ? 2 : 0)];
                    if (V.dump_gamma) {          /* as the window shows it */
                        line[x * 3 + 0] = V.gm[2][line[x * 3 + 0]];
                        line[x * 3 + 1] = V.gm[1][line[x * 3 + 1]];
                        line[x * 3 + 2] = V.gm[0][line[x * 3 + 2]];
                    }
                }
                fwrite(line, 1, row, f);
            }
            free(line);
            fclose(f);
            fprintf(stderr, "  [VK] wrote %s (%ux%u, what the window shows)\n",
                    V.present_dump_path, w, h);
        }
        V.present_dump_px = NULL;
        V.dump_gamma = 0;
    }
    V.vtx.used = V.idx.used = V.ubo.used = V.stage.used = V.rb.used = 0;
    vkResetDescriptorPool(V.dev, V.dpool, 0);
    V.sets_used = 0;
    V.last_set = VK_NULL_HANDLE;
    destroy_retired();
    V.epoch++;
}

/* The flip: submit the frame and go on to record the next one in the other
 * slot while the GPU draws it. The frame before is waited for now -- it has
 * had a whole frame's time -- and its copy-backs and visibility reports land
 * in guest RAM a frame later than a full sync would put them. Anything that
 * needs them sooner syncs (any_dirty_overlap and the rest see a surface
 * whose copy-back is in flight as dirty). */
static void vk_flip(void)
{
    int cur = V.cur_slot, nxt = cur ^ 1, i;
    Pend *p = &V.pend[cur];

    /* Debug captures read their results right after the sync. */
    if (!V.pipelined || !V.recording || V.nfq || s_probe_nq || V.present_dump_px) {
        vk_sync(1);
        return;
    }
    if (!segment_submit(1, p->rb, &p->nrb))
        return;
    for (i = 0; i < p->nrb; i++)
        p->rb[i].s->dirty = 2;
    p->slot = cur;
    p->kick = V.kick;
    p->nts = V.nts;
    p->seq = ++V.flip_seq;
    p->nq = V.nq;
    memcpy(p->qsamples, V.qsamples, V.nq);
    p->nreps = V.nreps;
    memcpy(p->reps, V.reps, V.nreps * sizeof V.reps[0]);
    p->rep_first = V.rep_first;
    p->cleared = V.rep_cleared;
    memcpy(p->retired, V.retired, (size_t)V.nretired * sizeof V.retired[0]);
    p->nretired = V.nretired;
    p->active = 1;
    V.kick = 0;
    V.nts = 0;
    V.nq = V.nreps = V.rep_first = 0;
    V.rep_cleared = 0;
    V.nretired = 0;
    V.recording = 0;
    V.st_syncs++;
    pend_retire(&V.pend[nxt]);
    slot_use(nxt);
    V.sets_used = 0;
    V.last_set = VK_NULL_HANDLE;
    V.epoch++;
}

/* Surfaces over [va, va+bytes) whose only GPU-newer contents are a flipped
 * frame's copy-back (dirty 2): landing that frame is enough -- it has had
 * the whole frame to finish -- rather than a full sync of this one. RSC2
 * clears its swizzled shadow map through a linear surface each frame. */
static void settle_inflight(uint32_t va, uint32_t bytes, const Surf *except)
{
    int i;
    for (i = 0; i < V.nsurf; i++) {
        const Surf *s = &V.surf[i];
        if (s != except && s->dirty == 2 && va < s->va + surf_bytes(s) && va + bytes > s->va) {
            pend_retire_all();
            return;
        }
    }
}

static int any_dirty_overlap(uint32_t va, uint32_t bytes, const Surf *except)
{
    int i;
    for (i = 0; i < V.nsurf; i++) {
        const Surf *s = &V.surf[i];
        if (s == except || !s->dirty)
            continue;
        if (va < s->va + surf_bytes(s) && va + bytes > s->va)
            return 1;
    }
    return 0;
}

/* ---- setup -------------------------------------------------------------- */

static VkShaderModule shader(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    ci.codeSize = bytes;
    ci.pCode = code;
    if (vkCreateShaderModule(V.dev, &ci, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

static int white_images(void)
{
    VkDeviceSize off;
    uint32_t *px = (uint32_t *)ring_alloc(&V.stage, 7 * 4, 16, &off);
    VkBufferImageCopy c;
    int i;
    if (!px)
        return 0;
    for (i = 0; i < 7; i++)
        px[i] = 0xFFFFFFFFu;
    if (!image_create(1, 1, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      0, VK_IMAGE_ASPECT_COLOR_BIT, &V.white2d, &V.white2d_mem,
                      &V.white2d_view))
        return 0;
    if (!image_create(1, 1, 1, 6, VK_FORMAT_B8G8R8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      1, VK_IMAGE_ASPECT_COLOR_BIT, &V.whitecube, &V.whitecube_mem,
                      &V.whitecube_view))
        return 0;
    segment_begin();
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = c.imageExtent.height = c.imageExtent.depth = 1;
    barrier_image(V.ucb, V.white2d, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(V.ucb, V.stage.buf, V.white2d,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier_image(V.ucb, V.white2d, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    c.imageSubresource.layerCount = 6;
    barrier_image(V.ucb, V.whitecube, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    {
        /* Six layers of one texel each, all from the same staged white. */
        VkBufferImageCopy cc[6];
        for (i = 0; i < 6; i++) {
            cc[i] = c;
            cc[i].imageSubresource.baseArrayLayer = (uint32_t)i;
            cc[i].imageSubresource.layerCount = 1;
        }
        vkCmdCopyBufferToImage(V.ucb, V.stage.buf, V.whitecube,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, cc);
    }
    barrier_image(V.ucb, V.whitecube, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    vk_sync(0);
    return V.state != -1;
}

static int glsl_to_spirv(const char *src, uint32_t hash, int frag, uint32_t **words, size_t *bytes);
static void ao_init(void);

static VkShaderModule glsl_module(const char *src, int frag)
{
    uint32_t *w = NULL;
    size_t n = 0;
    VkShaderModule m;
    if (!glsl_to_spirv(src, 0xF0110000u + (uint32_t)frag, frag, &w, &n))
        return VK_NULL_HANDLE;
    m = shader(w, n);
    free(w);
    return m;
}

/* A fullscreen-triangle pipeline (fragment shader `fs`) drawing into one
 * `fmt` attachment of `samples`, no depth. `blend`: 0 replace, 1 multiply
 * the colour (alpha kept). */
static VkPipeline post_pipeline(VkShaderModule fs, VkFormat fmt, uint32_t samples, int blend)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo bs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipeline p;
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = V.post_vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vps.viewportCount = vps.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = (VkSampleCountFlagBits)samples;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    memset(&ba, 0, sizeof ba);
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                      | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend == 1) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                          | VK_COLOR_COMPONENT_B_BIT;
    }
    bs.attachmentCount = 1;
    bs.pAttachments = &ba;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &fmt;
    gp.pNext = &ri;
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &bs;
    gp.pDynamicState = &dy;
    gp.layout = V.post_pl;
    if (vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &gp, NULL, &p) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return p;
}

/* Fullscreen passes: their layout and vertex shader, and the MSAA setting. */
static void post_init(void)
{
    static const char *vs_src =
        "#version 450\n"
        "void main() {\n"
        "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char *up_src =
        "#version 450\n"
        "layout(set = 0, binding = 0) uniform sampler2D src;\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() { o = texelFetch(src, ivec2(gl_FragCoord.xy), 0); }\n";
    VkDescriptorSetLayoutBinding b[2];
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPushConstantRange pr;
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    const char *e;

    memset(b, 0, sizeof b);
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1] = b[0];
    b[1].binding = 1;
    lci.bindingCount = 2;
    lci.pBindings = b;
    pr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pr.offset = 0;
    pr.size = 64;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &V.post_dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pr;
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.25f;
    if (vkCreateDescriptorSetLayout(V.dev, &lci, NULL, &V.post_dsl) != VK_SUCCESS
            || vkCreatePipelineLayout(V.dev, &plci, NULL, &V.post_pl) != VK_SUCCESS
            || vkCreateSampler(V.dev, &sci, NULL, &V.post_samp) != VK_SUCCESS
            || !(V.post_vs = glsl_module(vs_src, 0))) {
        fprintf(stderr, "  [VK] fullscreen passes unavailable\n");
        V.post_pl = VK_NULL_HANDLE;
        V.msaa = 1;
        return;
    }

    /* RECOMP_VK_MSAA=<2|4|8>: multisampled scene targets, as many samples as
     * the GPU allows for both colour and depth. */
    e = getenv("RECOMP_VK_MSAA");
    V.msaa = 1;
    if (e && atoi(e) > 1) {
        VkSampleCountFlags ok = V.limits.framebufferColorSampleCounts
                              & V.limits.framebufferDepthSampleCounts
                              & V.limits.framebufferStencilSampleCounts;
        uint32_t want = (uint32_t)atoi(e), n;
        for (n = 8; n > 1; n >>= 1)
            if (n <= want && (ok & n))
                break;
        if (n > 1) {
            VkShaderModule fs = glsl_module(up_src, 1);
            V.up_pipe = fs ? post_pipeline(fs, VK_FORMAT_B8G8R8A8_UNORM, n, 0) : VK_NULL_HANDLE;
            if (V.up_pipe) {
                V.msaa = n;
                fprintf(stderr, "  [VK] MSAA %ux on the scene targets\n", n);
            }
        }
    }
}

static int vk_init(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkPhysicalDevice pds[16];
    uint32_t npd = 16, i, best = UINT32_MAX, picked = UINT32_MAX;
    VkResult r;
    const char *pick = getenv("RECOMP_VK_DEVICE");
    const char *layers[1] = { "VK_LAYER_KHRONOS_validation" };

    app.pApplicationName = "xboxrecomp";
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    if (getenv("RECOMP_VK_VALIDATION")) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = layers;
    }
    {
        /* Whatever window-system surfaces the loader offers, so the window
         * thread can make one for SDL's window on any platform. */
        static const char *want[] = {
            "VK_KHR_surface", "VK_KHR_xlib_surface", "VK_KHR_xcb_surface",
            "VK_KHR_wayland_surface", "VK_KHR_win32_surface",
            "VK_KHR_android_surface", "VK_EXT_metal_surface",
        };
        static const char *on[8];
        VkExtensionProperties ep[256];
        uint32_t ne = 256, j, k, n = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &ne, ep);
        for (j = 0; j < sizeof want / sizeof want[0]; j++)
            for (k = 0; k < ne; k++)
                if (!strcmp(ep[k].extensionName, want[j])) {
                    on[n++] = want[j];
                    break;
                }
        ici.enabledExtensionCount = n;
        ici.ppEnabledExtensionNames = on;
    }
    r = vkCreateInstance(&ici, NULL, &V.inst);
    if (r != VK_SUCCESS)
        return vk_fail("vkCreateInstance", r);
    r = vkEnumeratePhysicalDevices(V.inst, &npd, pds);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !npd)
        return vk_fail("vkEnumeratePhysicalDevices", r);
    for (i = 0; i < npd; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        fprintf(stderr, "  [VK] device %u: %s (Vulkan %u.%u, type %d)\n", i,
                p.deviceName, VK_VERSION_MAJOR(p.apiVersion),
                VK_VERSION_MINOR(p.apiVersion), (int)p.deviceType);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        /* RECOMP_VK_DEVICE: an index, or (part of) the device name -- the
         * launcher saves names, as the order can change. A card that is not
         * there leaves the automatic choice: a discrete GPU, else the first. */
        if (pick && picked == UINT32_MAX
                && (*pick >= '0' && *pick <= '9' ? (uint32_t)atoi(pick) == i
                                                 : name_has(p.deviceName, pick)))
            picked = i;
        if (best == UINT32_MAX
                || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            VkPhysicalDeviceProperties bp;
            if (best != UINT32_MAX) {
                vkGetPhysicalDeviceProperties(pds[best], &bp);
                if (bp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                    continue;
            }
            best = i;
        }
    }
    if (pick && picked == UINT32_MAX)
        fprintf(stderr, "  [VK] RECOMP_VK_DEVICE=%s matches no device -- automatic\n", pick);
    if (picked != UINT32_MAX)
        best = picked;
    if (best == UINT32_MAX) {
        fprintf(stderr, "  [VK] no Vulkan 1.3 device\n");
        return 0;
    }
    V.pd = pds[best];
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(V.pd, &p);
        V.limits = p.limits;
        fprintf(stderr, "  [VK] using %s\n", p.deviceName);
    }
    vkGetPhysicalDeviceMemoryProperties(V.pd, &V.memp);
    {
        VkQueueFamilyProperties qfp[16];
        uint32_t nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, qfp);
        for (i = 0; i < nq; i++)
            if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                break;
        if (i == nq)
            return vk_fail("graphics queue", VK_ERROR_FEATURE_NOT_PRESENT);
        V.qf = i;
    }
    {
        static const VkFormat cand[] = { VK_FORMAT_D32_SFLOAT_S8_UINT,
                                         VK_FORMAT_D24_UNORM_S8_UINT };
        V.dsfmt = VK_FORMAT_UNDEFINED;
        for (i = 0; i < 2; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(V.pd, cand[i], &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                V.dsfmt = cand[i];
                break;
            }
        }
        if (V.dsfmt == VK_FORMAT_UNDEFINED)
            return vk_fail("depth/stencil format", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        VkPhysicalDeviceFeatures have;
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        vkGetPhysicalDeviceFeatures(V.pd, &have);
        qci.queueFamilyIndex = V.qf;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        f13.dynamicRendering = VK_TRUE;
        f12.hostQueryReset = VK_TRUE;
        f13.pNext = &f12;
        f2.pNext = &f13;
        f2.features.occlusionQueryPrecise = have.occlusionQueryPrecise;
        V.precise = have.occlusionQueryPrecise;
        f2.features.depthClamp = have.depthClamp;
        V.depth_clamp = have.depthClamp;
        f2.features.largePoints = have.largePoints;
        f2.features.wideLines = have.wideLines;
        V.wide_lines = have.wideLines;
        /* RECOMP_VK_ANISO=<2..16>: anisotropic filtering for the title's
         * mipmapped textures, which sharpens road and terrain at grazing
         * angles. The NV2A has it too, but the title never asks. */
        {
            const char *e = getenv("RECOMP_VK_ANISO");
            float a = e ? (float)atof(e) : 0.0f;
            if (a > 1.0f && have.samplerAnisotropy) {
                f2.features.samplerAnisotropy = VK_TRUE;
                V.aniso = a;
            }
        }
        dci.pNext = &f2;
        {
            static const char *swext[1] = { "VK_KHR_swapchain" };
            VkExtensionProperties ep[512];
            uint32_t ne = 512, k;
            vkEnumerateDeviceExtensionProperties(V.pd, NULL, &ne, ep);
            for (k = 0; k < ne; k++)
                if (!strcmp(ep[k].extensionName, "VK_KHR_swapchain"))
                    V.has_swapchain_ext = 1;
            if (V.has_swapchain_ext) {
                dci.enabledExtensionCount = 1;
                dci.ppEnabledExtensionNames = swext;
            }
        }
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        r = vkCreateDevice(V.pd, &dci, NULL, &V.dev);
        if (r != VK_SUCCESS)
            return vk_fail("vkCreateDevice", r);
#if defined(__ANDROID__)
        if (!vk_load_device_fns(V.dev))
            return vk_fail("vkGetDeviceProcAddr", VK_ERROR_INITIALIZATION_FAILED);
#endif
        vkGetDeviceQueue(V.dev, V.qf, 0, &V.q);
    }
    {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkCommandBuffer cbs[2];
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkQueryPoolCreateInfo qci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        const char *e = getenv("RECOMP_VK_KICK");
        int sl, k;
        V.kick_draws = e ? atoi(e) : 16;
        e = getenv("RECOMP_VK_PIPELINE");
        V.pipelined = e ? atoi(e) != 0 : 1;
        pci.queueFamilyIndex = V.qf;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 2;
        qci.queryType = VK_QUERY_TYPE_OCCLUSION;
        qci.queryCount = VK_MAX_QUERIES;
        for (sl = 0; sl < VK_SLOTS; sl++) {
            Slot *q = &V.slots[sl];
            if (vkCreateCommandPool(V.dev, &pci, NULL, &q->pool) != VK_SUCCESS)
                return 0;
            cai.commandPool = q->pool;
            for (k = 0; k < VK_KICKS; k++) {
                if (vkAllocateCommandBuffers(V.dev, &cai, cbs) != VK_SUCCESS)
                    return 0;
                q->ucbs[k] = cbs[0];
                q->dcbs[k] = cbs[1];
                if (vkCreateFence(V.dev, &fci, NULL, &q->kfence[k]) != VK_SUCCESS)
                    return 0;
            }
            if (vkCreateFence(V.dev, &fci, NULL, &q->fence) != VK_SUCCESS)
                return 0;
            if (getenv("RECOMP_VK_STATS") && V.limits.timestampPeriod > 0.0f) {
                VkQueryPoolCreateInfo tq = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                tq.queryType = VK_QUERY_TYPE_TIMESTAMP;
                tq.queryCount = 2 * (VK_KICKS + 2);
                if (vkCreateQueryPool(V.dev, &tq, NULL, &q->tspool) == VK_SUCCESS)
                    vkResetQueryPool(V.dev, q->tspool, 0, tq.queryCount);
                else
                    q->tspool = VK_NULL_HANDLE;
            }
            if (vkCreateQueryPool(V.dev, &qci, NULL, &q->qpool) != VK_SUCCESS)
                return 0;
            vkResetQueryPool(V.dev, q->qpool, 0, VK_MAX_QUERIES);
            vkCreateSemaphore(V.dev, &sci, NULL, &q->sem_acq);
            vkCreateSemaphore(V.dev, &sci, NULL, &q->sem_done);
        }
        if (vkCreateQueryPool(V.dev, &qci, NULL, &V.fqpool) == VK_SUCCESS)
            vkResetQueryPool(V.dev, V.fqpool, 0, VK_MAX_QUERIES);
    }
    if (!ring_init(&V.vtx, VK_RING_VTX, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0)
        || !ring_init(&V.idx, VK_RING_IDX, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 0)
        || !ring_init(&V.ubo, VK_RING_UBO, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0)
        || !ring_init(&V.stage, VK_RING_STAGE / VK_SLOTS, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0)
        || !ring_init(&V.rb, VK_RING_RB, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1))
        return 0;
    {
        VkDescriptorSetLayoutBinding b[4];
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkDescriptorPoolSize ps[2];
        VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        VkPipelineCacheCreateInfo pcci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        memset(b, 0, sizeof b);
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorCount = 4;
        b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[2] = b[1];
        b[2].binding = 2;
        b[3] = b[0];                   /* vertex program constants */
        b[3].binding = 3;
        b[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        lci.bindingCount = 4;
        lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(V.dev, &lci, NULL, &V.dsl) != VK_SUCCESS)
            return 0;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &V.dsl;
        if (vkCreatePipelineLayout(V.dev, &plci, NULL, &V.pl) != VK_SUCCESS)
            return 0;
        ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ps[0].descriptorCount = VK_MAX_SETS * 2;
        ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps[1].descriptorCount = VK_MAX_SETS * 8;
        dpci.maxSets = VK_MAX_SETS;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = ps;
        {
            int sl;
            for (sl = 0; sl < VK_SLOTS; sl++)
                if (vkCreateDescriptorPool(V.dev, &dpci, NULL, &V.slots[sl].dpool) != VK_SUCCESS)
                    return 0;
            slot_use(0);
        }
        {
            /* The driver checks the header (vendor, device, UUID) and
             * ignores data from anything else. */
            const char *dir = cache_dir();
            char path[1100];
            size_t n = 0;
            void *data = NULL;
            if (dir) {
                snprintf(path, sizeof path, "%s/pipelines.bin", dir);
                data = file_read(path, &n);
            }
            pcci.initialDataSize = n;
            pcci.pInitialData = data;
            if (vkCreatePipelineCache(V.dev, &pcci, NULL, &V.pcache) != VK_SUCCESS) {
                pcci.initialDataSize = 0;
                pcci.pInitialData = NULL;
                vkCreatePipelineCache(V.dev, &pcci, NULL, &V.pcache);
            }
            free(data);
        }
    }
    V.vs = shader(s_vert_spv, sizeof s_vert_spv);
    V.fs = shader(s_frag_spv, sizeof s_frag_spv);
    V.fs_fast = shader(s_frag_fast_spv, sizeof s_frag_fast_spv);
    {
        /* The reversed w-buffer needs a float depth buffer for its precision.
         * RECOMP_VK_WREV=0 keeps w/zmax written by the fragment shader. */
        const char *e = getenv("RECOMP_VK_WREV");
        V.wrev = V.dsfmt == VK_FORMAT_D32_SFLOAT_S8_UINT && !(e && strcmp(e, "0") == 0);
        e = getenv("RECOMP_VK_RT_DIRECT");
        V.rt_direct = !(e && strcmp(e, "0") == 0);
        e = getenv("RECOMP_VK_BC");
        V.bc_ok = !(e && strcmp(e, "0") == 0);
        if (V.bc_ok) {
            /* Every desktop GPU samples BC; many phones do not (they keep
             * the CPU decode). */
            static const VkFormat bcf[3] = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                                             VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK };
            int k;
            for (k = 0; k < 3; k++) {
                VkFormatProperties fp;
                vkGetPhysicalDeviceFormatProperties(V.pd, bcf[k], &fp);
                if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
                        || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
                    V.bc_ok = 0;
            }
        }
    }
    if (!V.vs || !V.fs || !V.fs_fast)
        return vk_fail("shader modules", VK_ERROR_INITIALIZATION_FAILED);
    {
        const char *e = getenv("RECOMP_VK_SCALE");
        V.scale = e ? (uint32_t)atoi(e) : 1;
        if (V.scale < 1) V.scale = 1;
        if (V.scale > 8) V.scale = 8;
        /* RECOMP_WIDESCREEN: the title squeezes a 16:9 view into 640 columns,
         * stretched back out on screen; 4/3 more columns keep it as sharp
         * across as it is down. Rounded up to whole pixels per pixel.
         * RECOMP_VK_WIDE_SHARP=0 keeps the plain scale (a third fewer
         * pixels to draw). */
        const char *sharp = getenv("RECOMP_VK_WIDE_SHARP");
        V.sx = V.scale;
        if (fb_widescreen() && !(sharp && strcmp(sharp, "0") == 0))
            V.sx = (V.scale * 4 + 2) / 3;
        if (V.sx > 8) V.sx = 8;
    }
    if (!white_images())
        return 0;
    post_init();
    ao_init();
    V.cur_samples = 1;
    fprintf(stderr, "  [VK] renderer up: depth format %d, depth clamp %d, scale %ux\n",
            (int)V.dsfmt, V.depth_clamp, V.scale);
    if (V.sx != V.scale)
        fprintf(stderr, "  [VK] widescreen: %ux across, %ux down\n", V.sx, V.scale);
    return 1;
}

int nv2a_vk_active(void)
{
    if (V.state == 0) {
        const char *e = getenv("RECOMP_VULKAN");
        if (!e || !*e || strcmp(e, "0") == 0) {
            V.state = -1;
            return 0;
        }
        V.state = vk_init() ? 1 : -1;
        if (V.state < 0)
            fprintf(stderr, "  [VK] Vulkan unavailable -- software rasteriser\n");
    }
    return V.state > 0;
}

/* ---- pipelines and samplers --------------------------------------------- */

static VkBlendFactor blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;
    }
}

#define WREV_C (1.0f / 1024.0f)        /* as nv2a_common.glsl */

/* The draw's depth encoding, as the shaders' vp.w: 0 z-buffer, 1 w-buffer as
 * w/zmax, 2 w-buffer as WREV_C / w (reversed). */
static int depth_mode(const Nv2aVkState *st)
{
    return st->wbuffer ? (V.wrev ? 2 : 1) : 0;
}

/* Whether the fragment shader has to write the depth itself: the polygon
 * offset, the z-buffer's integer rounding, the w/zmax encoding. Otherwise the
 * fast variant draws, and hidden fragments never reach the shader -- writing
 * gl_FragDepth turns that early test off, and at 3x it doubled the GPU time. */
static int needs_fragdepth(const Nv2aVkState *st)
{
    if (!V.cur_zs || !st->depth_test)
        return 0;
    if (st->poff_factor != 0.0f || st->poff_units != 0.0f)
        return 1;
    return st->wbuffer ? !V.wrev : (int)st->zint;
}

/* LESS <-> GREATER, LEQUAL <-> GEQUAL: for the reversed w-buffer. */
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

/* Everything a pipeline is built from, so a worker thread can build one. */
typedef struct {
    VkFormat cfmt;
    int has_depth, blend;
    uint32_t samples;
    uint32_t cm, beq, sf, df, topo, ninputs;
    VkShaderModule vs, fs;
} PipeDesc;

/* `count`: the main thread's statistics (a worker leaves them alone). */
static VkPipeline pipe_build(const PipeDesc *d, int count)
{
    int i;
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[16];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo bs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP,
    };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipeline p;
    VkResult r;

    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = d->vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = d->fs;
    st[1].pName = "main";
    vb.binding = 0;
    vb.stride = 16 * d->ninputs;
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    for (i = 0; i < (int)d->ninputs; i++) {
        va[i].location = (uint32_t)i;
        va[i].binding = 0;
        va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[i].offset = (uint32_t)i * 16;
    }
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = d->ninputs;
    vi.pVertexAttributeDescriptions = va;
    ia.topology = d->topo == 2 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : d->topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    /* Depth clamp turns off clipping against z, leaving the clip-space x/y
     * planes and w > 0: the NV2A clips only at the eye, and a triangle that
     * crosses the far value is drawn and clamped, not cut. */
    rs.depthClampEnable = V.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    /* Lines one title pixel wide at whatever scale the GPU renders. */
    rs.lineWidth = V.wide_lines ? (float)V.scale : 1.0f;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(d->samples ? d->samples : 1);
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.maxDepthBounds = 1.0f;
    memset(&ba, 0, sizeof ba);
    ba.blendEnable = d->blend ? VK_TRUE : VK_FALSE;
    ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = blend_factor(d->sf);
    ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = blend_factor(d->df);
    ba.colorBlendOp = ba.alphaBlendOp = blend_op(d->beq);
    ba.colorWriteMask = d->cm;
    bs.attachmentCount = d->cfmt != VK_FORMAT_UNDEFINED ? 1 : 0;
    bs.pAttachments = &ba;
    dy.dynamicStateCount = sizeof dyn / sizeof dyn[0];
    dy.pDynamicStates = dyn;
    ri.colorAttachmentCount = d->cfmt != VK_FORMAT_UNDEFINED ? 1 : 0;
    ri.pColorAttachmentFormats = &d->cfmt;
    ri.depthAttachmentFormat = d->has_depth ? V.dsfmt : VK_FORMAT_UNDEFINED;
    ri.stencilAttachmentFormat = d->has_depth ? V.dsfmt : VK_FORMAT_UNDEFINED;
    gp.pNext = &ri;
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &bs;
    gp.pDynamicState = &dy;
    gp.layout = V.pl;
    {
        double t0 = now_ms();
        r = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &gp, NULL, &p);
        if (count) {
            V.st_pipe_ms += now_ms() - t0;
            V.st_pipes_new++;
        }
    }
    if (r != VK_SUCCESS) {
        vk_fail("vkCreateGraphicsPipelines", r);
        return VK_NULL_HANDLE;
    }
    return p;
}

/* The table key of a draw's pipeline; `spec` the specialised fragment
 * shader's key, 0 for the general one. */
static void pipe_key(const PipeDesc *d, uint32_t prog, int fast, uint64_t spec, uint64_t k[4])
{
    k[0] = (uint64_t)d->cfmt | (uint64_t)d->has_depth << 16 | (uint64_t)d->cm << 20
         | (uint64_t)d->blend << 24 | (uint64_t)(d->blend ? d->beq : 0) << 32;
    k[1] = d->blend ? ((uint64_t)d->sf << 32 | d->df) : 0;
    k[2] = (uint64_t)prog << 8 | d->ninputs | (uint64_t)(d->topo & 3) << 40
         | (uint64_t)fast << 44 | (uint64_t)(d->samples & 0xF) << 48;
    k[3] = spec;
}

/* The entry for `k`, or where it would go (Pipe.used 0); NULL when full. */
static Pipe *pipe_find(const uint64_t k[4])
{
    uint32_t slot = (uint32_t)((k[0] * 0x9E3779B97F4A7C15ull) ^ (k[1] * 31)
                               ^ (k[2] * 0xC2B2AE3D27D4EB4Full) ^ (k[3] * 0xFF51AFD7ED558CCDull))
                  & (VK_MAX_PIPES - 1), probe;
    for (probe = 0; probe < VK_MAX_PIPES; probe++) {
        Pipe *pp = &V.pipes[(slot + probe) & (VK_MAX_PIPES - 1)];
        if (!pp->used)
            return V.npipes >= VK_MAX_PIPES * 3 / 4 ? NULL : pp;
        if (pp->key[0] == k[0] && pp->key[1] == k[1] && pp->key[2] == k[2] && pp->key[3] == k[3])
            return pp;
    }
    return NULL;
}

static void pipe_desc(PipeDesc *d, VkFormat cfmt, int has_depth, const Nv2aVkState *s,
                      VkShaderModule vs, uint32_t ninputs)
{
    memset(d, 0, sizeof *d);
    d->cfmt = cfmt;
    d->has_depth = has_depth;
    if (s->color_mask & 0x00010000u) d->cm |= VK_COLOR_COMPONENT_R_BIT;
    if (s->color_mask & 0x00000100u) d->cm |= VK_COLOR_COMPONENT_G_BIT;
    if (s->color_mask & 0x00000001u) d->cm |= VK_COLOR_COMPONENT_B_BIT;
    if (s->color_mask & 0x01000000u) d->cm |= VK_COLOR_COMPONENT_A_BIT;
    d->blend = s->blend_enable && !(s->blend_sfactor == 1 && s->blend_dfactor == 0);
    d->beq = s->blend_equation;
    d->sf = s->blend_sfactor;
    d->df = s->blend_dfactor;
    d->topo = s->topology;
    d->vs = vs;
    d->ninputs = ninputs;
    d->samples = V.cur_samples ? V.cur_samples : 1;
}

/* The draw's pipeline with the general fragment shader, built now if new. */
static VkPipeline pipeline(VkFormat cfmt, int has_depth, const Nv2aVkState *s,
                           VkShaderModule vs, uint32_t prog, uint32_t ninputs)
{
    int fast = !needs_fragdepth(s);
    PipeDesc d;
    uint64_t k[4];
    Pipe *pp;
    VkPipeline p;

    pipe_desc(&d, cfmt, has_depth, s, vs, ninputs);
    d.fs = fast ? V.fs_fast : V.fs;
    pipe_key(&d, prog, fast, 0, k);
    pp = pipe_find(k);
    if (!pp)
        return VK_NULL_HANDLE;
    if (pp->used)
        return pp->pipe;
    p = pipe_build(&d, 1);
    if (!p)
        return VK_NULL_HANDLE;
    memcpy(pp->key, k, sizeof k);
    pp->pipe = p;
    pp->used = 1;
    V.npipes++;
    return p;
}

static VkSamplerAddressMode address_mode(uint32_t m)
{
    switch (m) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

/* SET_TEXTURE_FILTER bits 0-12: the mip LOD bias, signed 5.8 fixed point. */
static float tex_lod_bias(const Nv2aVkTex *t)
{
    int32_t b = (int32_t)(t->filter << 19) >> 19;
    return (float)b / 256.0f;
}

static VkSampler sampler(const Nv2aVkTex *t, uint32_t levels)
{
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int mip = min >= 3 && min <= 6 && levels > 1;
    /* The image starts at the title's minimum level, so the hardware's max
     * LOD is relative to level min_level here. */
    uint32_t maxl = t->max_lod >> 8 > t->min_level ? (t->max_lod >> 8) - t->min_level : 0;
    uint64_t key = (mag & 0xF) | (min & 0xF) << 4 | (t->addr_u & 0xF) << 8
                 | (t->addr_v & 0xF) << 12 | (t->addr_p & 0xF) << 16
                 | (uint64_t)mip << 20 | (uint64_t)(t->filter & 0x1FFF) << 21
                 | (uint64_t)(mip ? (maxl < levels ? maxl : levels) : 0) << 34;
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    int i;
    for (i = 0; i < V.nsamps; i++)
        if (V.samps[i].key == key)
            return V.samps[i].s;
    if (V.nsamps == VK_MAX_SAMPLERS)
        return V.samps[0].s;
    /* MAG 2 is tent; MIN 1/2 nearest/tent at level 0, 3-6 the mip modes
     * (nearest/tent within a level x nearest/linear between levels). */
    ci.magFilter = mag == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (min == 2 || min == 4 || min == 6) ? VK_FILTER_LINEAR
                                                      : VK_FILTER_NEAREST;
    ci.mipmapMode = (min == 5 || min == 6) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                           : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = address_mode(t->addr_u);
    ci.addressModeV = address_mode(t->addr_v);
    ci.addressModeW = address_mode(t->addr_p);
    /* The title's LOD bias and max-LOD clamp. Ignored, distant alpha-tested
     * foliage sat a mip level off from where the title tuned it and its
     * cut-out coverage jumped as the level changed: trees popped. */
    ci.minLod = 0.0f;
    ci.maxLod = mip ? (float)(maxl < levels ? maxl : levels) : 0.25f;
    if (mip) {
        /* RECOMP_VK_LOD_BIAS=<n> adds n to every bias (0 = the title's own;
         * "off" ignores the title's). For comparing foliage shimmer. */
        static int mode = -1;
        static float extra;
        float bias, lim = V.limits.maxSamplerLodBias;
        if (mode < 0) {
            const char *e = getenv("RECOMP_VK_LOD_BIAS");
            mode = e && !strcmp(e, "off") ? 0 : 1;
            extra = e && mode ? (float)atof(e) : 0.0f;
        }
        bias = (mode ? tex_lod_bias(t) : 0.0f) + extra;
        ci.mipLodBias = bias > lim ? lim : (bias < -lim ? -lim : bias);
    }
    /* Only where the title filters linearly within a mip level; its point-
     * sampled textures stay point-sampled. */
    if (V.aniso > 1.0f && mip && ci.minFilter == VK_FILTER_LINEAR) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = V.aniso < V.limits.maxSamplerAnisotropy ? V.aniso
                                                                   : V.limits.maxSamplerAnisotropy;
    }
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(V.dev, &ci, NULL, &V.samps[V.nsamps].s) != VK_SUCCESS)
        return V.nsamps ? V.samps[0].s : VK_NULL_HANDLE;
    V.samps[V.nsamps].key = key;
    return V.samps[V.nsamps++].s;
}

/* ---- textures ----------------------------------------------------------- */

static uint32_t tex_key_hash(const Nv2aVkTex *d)
{
    uint32_t h = d->offset * 2654435761u;
    h ^= (d->color << 24) ^ (d->width << 12) ^ d->height ^ (d->levels << 20)
       ^ (d->min_level << 28) ^ (uint32_t)d->cube << 31 ^ d->pitch * 40503u
       ^ d->palette * 97u;
    return h;
}

static int tex_same(const Nv2aVkTex *a, const Nv2aVkTex *b)
{
    return a->offset == b->offset && a->color == b->color
        && a->width == b->width && a->height == b->height
        && a->pitch == b->pitch && a->levels == b->levels
        && a->min_level == b->min_level && a->cube == b->cube
        && a->palette == b->palette;
}

/* The image for a texture stage, decoded and uploaded if its bytes changed.
 * Returns its view and level count, or 0 to use the white stand-in. */
/* Why a stage got the white stand-in instead of its texture: counted for
 * RECOMP_VK_STATS, the first few of each reason printed. */
static uint32_t s_tex_fail[16];
static void tex_fail(int why, const Nv2aVkTex *d)
{
    if (why < 16 && s_tex_fail[why]++ < 3)
        fprintf(stderr, "  [VK] texture %08X fmt %02X %ux%u not available (reason %d, %u textures, %u images)\n",
                d->offset, d->color, d->width, d->height, why, V.ntex, V.nimages);
}

static RtTex *rt_slot(uint32_t va, uint32_t w, uint32_t h, int cube)
{
    RtTex *e, *old = &V.rt[0];
    int i;
    for (i = 0; i < VK_MAX_RTTEX; i++)
        if (V.rt[i].img && V.rt[i].va == va && V.rt[i].w == w && V.rt[i].h == h
                && V.rt[i].cube == cube)
            return &V.rt[i];
    for (i = 0; i < VK_MAX_RTTEX; i++) {
        if (!V.rt[i].img) {
            old = &V.rt[i];
            break;
        }
        if (V.rt[i].last_use < old->last_use)
            old = &V.rt[i];
    }
    e = old;
    if (e->img) {
        retire(e->img, e->mem, e->view);
        if (e->view_x8 != e->view)
            retire(VK_NULL_HANDLE, VK_NULL_HANDLE, e->view_x8);
    }
    memset(e, 0, sizeof *e);
    if (!image_create(w, h, 1, cube ? 6 : 1, VK_FORMAT_B8G8R8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      cube, VK_IMAGE_ASPECT_COLOR_BIT, &e->img, &e->mem, &e->view)) {
        memset(e, 0, sizeof *e);
        return NULL;
    }
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = e->img;
        vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.components.a = VK_COMPONENT_SWIZZLE_ONE;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = cube ? 6 : 1;
        if (vkCreateImageView(V.dev, &vi, NULL, &e->view_x8) != VK_SUCCESS)
            e->view_x8 = e->view;
    }
    e->va = va;
    e->w = w;
    e->h = h;
    e->cube = cube;
    return e;
}

/* A cube map whose faces are swizzled surfaces (RSC2's car reflection: six
 * 128x128 faces, three redrawn a frame). Each face comes from its surface's
 * image -- one drawn this frame, or one unchanged since its copy-back (its
 * guest bytes still hash the same) -- blitted square at the vertical scale,
 * as cube faces must be square and widescreen draws them wider than tall.
 * Any face without such a surface: the old path. */
static int rt_cube(const Nv2aVkTex *d, uint32_t bytes, VkImageView *view, uint32_t *levels_out)
{
    Surf *face[6];
    RtTex *e;
    uint32_t fbytes = bytes / 6, n, f;
    int i, x8, any_new = 0;

    if (d->color == 0x06) x8 = 0;
    else if (d->color == 0x07) x8 = 1;
    else return 0;
    if (d->width != d->height || bytes % 6 || (d->levels > 1 && d->size_from_format))
        return 0;
    for (f = 0; f < 6; f++) {
        uint32_t va = d->offset + f * fbytes;
        face[f] = NULL;
        for (i = 0; i < V.nsurf; i++) {
            Surf *q = &V.surf[i];
            if (q->va == va && q->swz && q->w == d->width && q->h == d->height && q->bpp == 4
                    && q->fmt == VK_FORMAT_B8G8R8A8_UNORM) {
                face[f] = q;
                break;
            }
        }
        if (!face[f])
            return 0;
        /* Anything else dirty over the face: not ours to resolve. */
        for (i = 0; i < V.nsurf; i++) {
            Surf *q = &V.surf[i];
            if (q != face[f] && q->dirty && va < q->va + surf_bytes(q) && va + fbytes > q->va)
                return 0;
        }
        if (!face[f]->dirty && (!face[f]->hash_valid || surf_hash_now(face[f]) != face[f]->hash))
            return 0;
    }
    n = d->width * V.scale;
    e = rt_slot(d->offset, n, n, 1);
    if (!e)
        return 0;
    e->last_use = V.frame;
    for (f = 0; f < 6; f++)
        if (!e->init || e->fgen[f] != face[f]->gen)
            any_new = 1;
    if (any_new) {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        segment_begin();
        end_rendering();
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT
                         | VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                    | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                    | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        barrier_image(V.dcb, e->img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6,
                      e->init ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        for (f = 0; f < 6; f++) {
            VkImageBlit b;
            if (e->init && e->fgen[f] == face[f]->gen)
                continue;
            memset(&b, 0, sizeof b);
            b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
            b.dstSubresource.baseArrayLayer = f;
            b.srcOffsets[1].x = (int32_t)(d->width * V.sx);
            b.srcOffsets[1].y = (int32_t)(d->height * V.scale);
            b.srcOffsets[1].z = 1;
            b.dstOffsets[1].x = (int32_t)n;
            b.dstOffsets[1].y = (int32_t)n;
            b.dstOffsets[1].z = 1;
            vkCmdBlitImage(V.dcb, face[f]->img, VK_IMAGE_LAYOUT_GENERAL, e->img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
            e->fgen[f] = face[f]->gen;
            V.st_rt_copies++;
        }
        barrier_image(V.dcb, e->img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        e->init = 1;
    }
    *view = x8 ? e->view_x8 : e->view;
    *levels_out = 1;
    return 1;
}

/* A texture that is a surface the GPU drew this frame: sampled from a copy
 * made on the GPU, at the render scale, instead of syncing, reading the
 * surface back to guest RAM and decoding it again. That round trip stalled
 * the CPU on the GPU twice a frame in a race (the car-shadow mask 83E78000,
 * a 64x64 effect target 837F0000), so their times added up instead of
 * overlapping. 2D A8R8G8B8 / X8R8G8B8 textures laid out exactly as the
 * surface (same swizzling, same pitch) qualify; anything else -- cube faces,
 * other formats, partial overlaps -- takes the old path. */
static int rt_tex(const Nv2aVkTex *d, uint32_t bytes, VkImageView *view, uint32_t *levels_out)
{
    Surf *q = NULL;
    RtTex *e = NULL;
    uint32_t w, h;
    int i, x8, swz;
    VkImageCopy c;

    if (!V.rt_direct)
        return 0;
    if (d->cube)
        return rt_cube(d, bytes, view, levels_out);
    switch (d->color) {
    case 0x06: x8 = 0; swz = 1; break;          /* A8R8G8B8 */
    case 0x07: x8 = 1; swz = 1; break;          /* X8R8G8B8 */
    case 0x12: x8 = 0; swz = 0; break;          /* LIN_A8R8G8B8 */
    case 0x1E: x8 = 1; swz = 0; break;          /* LIN_X8R8G8B8 */
    default: return 0;
    }
    for (i = 0; i < V.nsurf; i++) {
        Surf *c2 = &V.surf[i];
        if (!c2->dirty || !(d->offset < c2->va + surf_bytes(c2) && d->offset + bytes > c2->va))
            continue;
        if (q || c2->va != d->offset)
            return 0;                           /* not exactly one surface */
        q = c2;
    }
    if (!q || q->bpp != 4 || q->fmt != VK_FORMAT_B8G8R8A8_UNORM || q->swz != swz)
        return 0;
    if (swz ? (d->width != q->w || d->height != q->h)
            : (d->pitch != q->pitch || d->width > q->w || d->height > q->h))
        return 0;
    w = d->width * V.sx;
    h = d->height * V.scale;

    e = rt_slot(q->va, w, h, 0);
    if (!e)
        return 0;
    if (!e->init)
        e->gen = q->gen - 1;
    e->last_use = V.frame;
    if (e->gen != q->gen) {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        segment_begin();
        end_rendering();
        /* After the draws that wrote the surface and that read this image. */
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                    | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        barrier_image(V.dcb, e->img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                      e->init ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        memset(&c, 0, sizeof c);
        c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
        c.extent.width = w;
        c.extent.height = h;
        c.extent.depth = 1;
        vkCmdCopyImage(V.dcb, q->img, VK_IMAGE_LAYOUT_GENERAL, e->img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier_image(V.dcb, e->img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        e->init = 1;
        e->gen = q->gen;
        V.st_rt_copies++;
    }
    *view = x8 ? e->view_x8 : e->view;
    *levels_out = 1;
    return 1;
}

static int tex_get(const Nv2aVkTex *t, VkImageView *view, uint32_t *levels_out)
{
    Nv2aVkTex d = *t;
    uint32_t slot, n, bytes, faces, levels, lv, f, w0, h0;
    TexEnt *e = NULL;
    uint64_t h;
    VkDeviceSize total = 0, off;
    uint8_t *dst;
    VkBufferImageCopy regs[6 * 16];
    uint32_t nregs = 0;

    d.filter = d.addr_u = d.addr_v = d.addr_p = 0;
    if (!d.valid || !d.width || !d.height || d.width > 4096 || d.height > 4096)
        { tex_fail(1, &d); return 0; }
    bytes = nv2a_pb_tex_bytes(&d);
    if (!bytes)
        { tex_fail(2, &d); return 0; }
    /* The title may draw a surface and sample it in the same frame. */
    if (rt_tex(&d, bytes, view, levels_out))
        return 1;
    settle_inflight(d.offset, bytes, NULL);
    if (any_dirty_overlap(d.offset, bytes, NULL)) {
        static int trace = -1;
        if (trace < 0)
            trace = getenv("RECOMP_VK_RT_TRACE") != NULL;
        if (trace) {
            int k;
            fprintf(stderr, "[RTTEX] F%u tex %08X fmt %02X %ux%u pitch %u lv %u/%u cube %d swzfmt %d bytes %u:",
                    nv2a_pb_presented_frames(), d.offset, d.color, d.width, d.height, d.pitch,
                    d.min_level, d.levels, d.cube, d.size_from_format, bytes);
            for (k = 0; k < V.nsurf; k++) {
                const Surf *q = &V.surf[k];
                if (q->dirty && d.offset < q->va + surf_bytes(q) && d.offset + bytes > q->va)
                    fprintf(stderr, " surf %08X %ux%u pitch %u bpp %u swz %d fmt %d rows %u",
                            q->va, q->w, q->h, q->pitch, q->bpp, q->swz, (int)q->fmt, q->rows);
            }
            fputc('\n', stderr);
        }
        vk_sync(1);
    }

    slot = tex_key_hash(&d) & (VK_TEX_SLOTS - 1);
    for (n = 0; n < VK_TEX_SLOTS; n++) {
        TexEnt *c = &V.tex[(slot + n) & (VK_TEX_SLOTS - 1)];
        if (!c->used) {
            if (!e)
                e = c;
            break;
        }
        if (tex_same(&c->d, &d)) {
            e = c;
            break;
        }
    }
    if (!e)
        { tex_fail(3, &d); return 0; }                          /* table full */
    if (e->used && e->epoch == V.epoch) {
        e->last_use = V.frame;
        *view = e->view;
        *levels_out = e->levels;
        return 1;
    }
    h = hash_bytes(guest(d.offset), bytes);
    if (d.palette && (d.color == 0x0B))
        h ^= hash_bytes(guest(d.palette), 1024) * 3;
    V.st_tex_hash_mb += bytes >> 10;
    if (e->used && e->hash == h && e->bytes == bytes) {
        e->epoch = V.epoch;
        e->last_use = V.frame;
        *view = e->view;
        *levels_out = e->levels;
        return 1;
    }

    /* New or changed: decode every face and level into the staging ring. */
    double up_t0 = now_ms();
    faces = d.cube ? 6 : 1;
    if (V.bc_ok && d.size_from_format && d3d8_format_dxt_block_bytes(d.color)) {
        /* DXT: the GPU reads the blocks as they are (BC1/2/3), so they are
         * copied, not decoded. Decoding a streamed 512x1024 track texture
         * on the CPU took 11 ms, a stutter whenever the next section of the
         * stage streamed in. */
        uint32_t bb = d3d8_format_dxt_block_bytes(d.color), fstride = bytes / faces, skip = 0, l;
        VkFormat bf = d.color == 0x0C ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK
                    : d.color == 0x0E ? VK_FORMAT_BC2_UNORM_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        const uint8_t *src = guest(d.offset);
        levels = d.levels > d.min_level ? d.levels - d.min_level : 1;
        if (levels > 13)
            levels = 13;
        for (l = 0; l < d.min_level; l++) {
            uint32_t lw = d.width >> l ? d.width >> l : 1, lh = d.height >> l ? d.height >> l : 1;
            skip += ((lw + 3) / 4) * ((lh + 3) / 4) * bb;
        }
        w0 = d.width >> d.min_level ? d.width >> d.min_level : 1;
        h0 = d.height >> d.min_level ? d.height >> d.min_level : 1;
        total = 0;
        for (lv = 0; lv < levels; lv++) {
            uint32_t lw = w0 >> lv ? w0 >> lv : 1, lh = h0 >> lv ? h0 >> lv : 1;
            total += (VkDeviceSize)((lw + 3) / 4) * ((lh + 3) / 4) * bb * faces;
        }
        dst = (uint8_t *)ring_alloc(&V.stage, total, 16, &off);
        if (!dst) {
            vk_sync(0);
            dst = (uint8_t *)ring_alloc(&V.stage, total, 16, &off);
            if (!dst)
                { tex_fail(4, &d); return 0; }
        }
        segment_begin();
        if (e->used)
            retire(e->img, e->mem, e->view);
        else
            V.ntex++;
        e->used = 0;
        if (!image_create(w0, h0, levels, faces, bf,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          d.cube, VK_IMAGE_ASPECT_COLOR_BIT, &e->img, &e->mem, &e->view))
            { tex_fail(5, &d); return 0; }
        for (f = 0; f < faces; f++) {
            const uint8_t *fs = src + (size_t)f * fstride + skip;
            for (lv = 0; lv < levels; lv++) {
                uint32_t lw = w0 >> lv ? w0 >> lv : 1, lh = h0 >> lv ? h0 >> lv : 1;
                uint32_t n = ((lw + 3) / 4) * ((lh + 3) / 4) * bb;
                VkBufferImageCopy *c = &regs[nregs++];
                memcpy(dst, fs, n);
                memset(c, 0, sizeof *c);
                c->bufferOffset = off;
                c->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                c->imageSubresource.mipLevel = lv;
                c->imageSubresource.baseArrayLayer = f;
                c->imageSubresource.layerCount = 1;
                c->imageExtent.width = lw;
                c->imageExtent.height = lh;
                c->imageExtent.depth = 1;
                fs += n;
                dst += n;
                off += n;
            }
        }
        goto upload;
    }
    levels = d.size_from_format && d.levels > d.min_level ? d.levels - d.min_level : 1;
    if (levels > 13)
        levels = 13;
    w0 = d.size_from_format ? (d.width >> d.min_level) : d.width;
    h0 = d.size_from_format ? (d.height >> d.min_level) : d.height;
    if (!w0) w0 = 1;
    if (!h0) h0 = 1;
    for (lv = 0; lv < levels; lv++) {
        uint32_t lw = w0 >> lv ? w0 >> lv : 1, lh = h0 >> lv ? h0 >> lv : 1;
        total += (VkDeviceSize)lw * lh * 4 * faces;
    }
    dst = (uint8_t *)ring_alloc(&V.stage, total, 16, &off);
    if (!dst) {
        vk_sync(0);
        dst = (uint8_t *)ring_alloc(&V.stage, total, 16, &off);
        if (!dst)
            { tex_fail(4, &d); return 0; }
    }
    segment_begin();
    if (e->used)
        retire(e->img, e->mem, e->view);
    else
        V.ntex++;
    e->used = 0;
    if (!image_create(w0, h0, levels, faces, VK_FORMAT_B8G8R8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      d.cube, VK_IMAGE_ASPECT_COLOR_BIT, &e->img, &e->mem, &e->view))
        { tex_fail(5, &d); return 0; }
    for (f = 0; f < faces; f++) {
        for (lv = 0; lv < levels; lv++) {
            uint32_t lw = w0 >> lv ? w0 >> lv : 1, lh = h0 >> lv ? h0 >> lv : 1;
            VkBufferImageCopy *c = &regs[nregs++];
            nv2a_pb_tex_decode(&d, f, d.min_level + lv, lw, lh, (uint32_t *)dst);
            {
                /* RECOMP_VK_TEX_DUMP=<hex address>[,<hex>...]: those
                 * textures' levels, every face, as BMPs in the working
                 * directory (each time they are uploaded). */
                static uint32_t want[8];
                static int nwant = -1, wi;
                int hit = 0;
                if (nwant < 0) {
                    const char *e = getenv("RECOMP_VK_TEX_DUMP");
                    nwant = 0;
                    while (e && *e && nwant < 8) {
                        want[nwant++] = (uint32_t)strtoul(e, (char **)&e, 16);
                        while (*e == ',') e++;
                    }
                }
                for (wi = 0; wi < nwant; wi++)
                    hit |= want[wi] == d.offset;
                if (hit) {
                    char path[64];
                    FILE *df;
                    snprintf(path, sizeof path, "texdump_%08X_f%u_l%u.bmp", d.offset, f, lv);
                    df = fopen(path, "wb");
                    if (df) {
                        uint8_t hdr[54];
                        uint32_t row = lw * 4, total = 54 + row * lh, y;
                        int32_t nh = -(int32_t)lh;
                        memset(hdr, 0, sizeof hdr);
                        hdr[0] = 'B'; hdr[1] = 'M';
                        memcpy(hdr + 2, &total, 4);
                        hdr[10] = 54; hdr[14] = 40;
                        memcpy(hdr + 18, &lw, 4);
                        memcpy(hdr + 22, &nh, 4);
                        hdr[26] = 1; hdr[28] = 32;
                        fwrite(hdr, 1, 54, df);
                        for (y = 0; y < lh; y++)
                            fwrite(dst + (size_t)y * row, 1, row, df);
                        fclose(df);
                    }
                }
            }
            {
                /* RECOMP_VK_TEX_ALPHA=<hex address>: alpha statistics of
                 * that texture's level-0 faces, each time it is decoded. */
                static long awant = -2;
                static unsigned ashown;
                static uint64_t alast[6];
                if (awant == -2) {
                    const char *e = getenv("RECOMP_VK_TEX_ALPHA");
                    awant = e ? (long)strtoul(e, NULL, 16) : -1;
                }
                if (awant >= 0 && (uint32_t)awant == d.offset && lv == 0 && ashown < 4000 && f < 6) {
                    uint32_t n = lw * lh, k, amin = 255, amax = 0, hi = 0;
                    uint64_t sum = 0;
                    int continue_print = 1;
                    for (k = 0; k < n; k++) {
                        uint32_t av = dst[k * 4 + 3];
                        amin = av < amin ? av : amin;
                        amax = av > amax ? av : amax;
                        hi += av > 0x80;
                        sum += av;
                    }
                    if (alast[f] == sum + ((uint64_t)hi << 40) + 1)
                        continue_print = 0;
                    alast[f] = sum + ((uint64_t)hi << 40) + 1;
                    if (continue_print) ashown++;
                    if (continue_print) fprintf(stderr, "[VKALPHA] F%u tex %08X face %u alpha min %u max %u mean %.1f >0x80 %u/%u\n",
                            nv2a_pb_presented_frames(), d.offset, f, amin, amax,
                            n ? (double)sum / n : 0.0, hi, n);
                }
            }
            memset(c, 0, sizeof *c);
            c->bufferOffset = off;
            c->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c->imageSubresource.mipLevel = lv;
            c->imageSubresource.baseArrayLayer = f;
            c->imageSubresource.layerCount = 1;
            c->imageExtent.width = lw;
            c->imageExtent.height = lh;
            c->imageExtent.depth = 1;
            dst += (size_t)lw * lh * 4;
            off += (VkDeviceSize)lw * lh * 4;
        }
    }
upload:
    barrier_image(V.ucb, e->img, VK_IMAGE_ASPECT_COLOR_BIT, levels, faces,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(V.ucb, V.stage.buf, e->img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nregs, regs);
    barrier_image(V.ucb, e->img, VK_IMAGE_ASPECT_COLOR_BIT, levels, faces,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    e->used = 1;
    e->d = d;
    e->bytes = bytes;
    e->hash = h;
    e->epoch = V.epoch;
    e->last_use = V.frame;
    e->levels = levels;
    V.st_tex_up++;
    {
        /* For RECOMP_VK_HITCH: what was decoded this frame, and how long. */
        double ms = now_ms() - up_t0;
        V.st_tex_ms += ms;
        if (V.nhtex < 8) {
            snprintf(V.htex[V.nhtex], sizeof V.htex[0], "%08X f%02X %ux%u l%u c%d %.1fms",
                     d.offset, d.color, d.width, d.height, levels, d.cube, ms);
            V.nhtex++;
        }
    }
    {
        static int trace = -1;
        if (trace < 0)
            trace = getenv("RECOMP_VK_TEX_TRACE") != NULL;
        if (trace && (V.frame % 200) == 0)
            fprintf(stderr, "  [VK] upload tex %08X fmt %02X %ux%u lv %u/%u cube %d pitch %u (%u bytes)\n",
                    d.offset, d.color, d.width, d.height, d.min_level, d.levels,
                    d.cube, d.pitch, bytes);
    }
    *view = e->view;
    *levels_out = levels;
    return 1;
}

/* ---- surfaces ----------------------------------------------------------- */

static VkFormat surf_format(const Nv2aVkState *s)
{
    if (s->bpp == 4)
        return VK_FORMAT_B8G8R8A8_UNORM;
    if (s->bpp == 2)
        return (s->color_fmt == 1 || s->color_fmt == 2)
             ? VK_FORMAT_A1R5G5B5_UNORM_PACK16 : VK_FORMAT_R5G6B5_UNORM_PACK16;
    return VK_FORMAT_UNDEFINED;
}

static void surf_upload(Surf *s)
{
    VkDeviceSize off;
    uint32_t y, rows = s->rows < s->h ? s->rows : s->h;
    uint8_t *dst;
    VkBufferImageCopy c;
    if (!rows)
        return;
    dst = (uint8_t *)ring_alloc(&V.stage, (VkDeviceSize)s->w * rows * s->bpp, 16, &off);
    if (!dst)
        return;
    if (s->swz)
        surf_swizzle(s, guest(s->va), dst, rows, 0);
    else
        for (y = 0; y < rows; y++)
            memcpy(dst + (size_t)y * s->w * s->bpp, guest(s->va) + (size_t)y * s->pitch,
                   (size_t)s->w * s->bpp);
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = s->w;
    c.imageExtent.height = rows;
    c.imageExtent.depth = 1;
    if (V.sx > 1 && s->nimg) {
        VkImageBlit b;
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        vkCmdCopyBufferToImage(V.ucb, V.stage.buf, s->nimg, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(V.ucb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = (int32_t)s->w;
        b.srcOffsets[1].y = (int32_t)rows;
        b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = (int32_t)(s->w * V.sx);
        b.dstOffsets[1].y = (int32_t)(rows * V.scale);
        b.dstOffsets[1].z = 1;
        vkCmdBlitImage(V.ucb, s->nimg, VK_IMAGE_LAYOUT_GENERAL, s->img,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    } else {
        vkCmdCopyBufferToImage(V.ucb, V.stage.buf, s->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    s->gen = ++V.gen;
    s->clear_only = 0;
    s->ms_stale = 1;
    V.st_surf_up++;
}

static Surf *surf_get(const Nv2aVkState *st)
{
    VkFormat fmt = surf_format(st);
    uint32_t w, rows = st->clip_y + st->clip_h;
    Surf *s = NULL;
    int i;

    int swz = st->swz_w != 0;
    if (fmt == VK_FORMAT_UNDEFINED || !st->color_va || (!st->pitch && !swz))
        return NULL;
    w = swz ? st->swz_w : st->pitch / st->bpp;
    if (!w || w > 4096)
        return NULL;
    if (swz)
        rows = st->swz_h;
    if (rows > VK_SURF_H)
        rows = VK_SURF_H;
    {
        extern int xbox_VaReadable(uint32_t va);
        uint32_t bytes = swz ? w * st->swz_h * st->bpp : rows * st->pitch;
        if (!xbox_VaReadable(st->color_va) || !xbox_VaReadable(st->color_va + bytes - 4))
            return NULL;
    }
    for (i = 0; i < V.nsurf; i++)
        if (V.surf[i].va == st->color_va && V.surf[i].bpp == st->bpp
            && V.surf[i].fmt == fmt && V.surf[i].swz == swz
            && (swz ? V.surf[i].w == w && V.surf[i].h == st->swz_h
                    : V.surf[i].pitch == st->pitch)) {
            s = &V.surf[i];
            break;
        }
    if (!s && !swz) {
        /* D3D clears a swizzled target through a linear surface of the same
         * bytes -- a fill does not care about the layout. Same surface. */
        for (i = 0; i < V.nsurf; i++)
            if (V.surf[i].swz && V.surf[i].va == st->color_va
                && V.surf[i].bpp == st->bpp && st->clip_x == 0 && st->clip_y == 0
                && st->clip_w == V.surf[i].w && st->clip_h == V.surf[i].h)
                return &V.surf[i];
    }
    if (!s) {
        if (V.nsurf == VK_MAX_SURF) {
            /* Reuse the least recently used clean one. */
            int best = -1;
            for (i = 0; i < V.nsurf; i++)
                if (!V.surf[i].dirty && &V.surf[i] != V.cur_cs
                    && (best < 0 || V.surf[i].last_use < V.surf[best].last_use))
                    best = i;
            if (best < 0)
                return NULL;
            s = &V.surf[best];
            retire(s->img, s->mem, s->view);
            if (s->nimg)
                retire(s->nimg, s->nmem, s->nview);
            if (s->msimg)
                retire(s->msimg, s->msmem, s->msview);
            free(s->swx);
            free(s->swy);
        } else {
            s = &V.surf[V.nsurf++];
        }
        memset(s, 0, sizeof *s);
        s->va = st->color_va;
        s->pitch = st->pitch;
        s->bpp = st->bpp;
        s->fmt = fmt;
        s->w = w;
        s->h = swz ? st->swz_h : VK_SURF_H;
        s->swz = swz;
        s->rows = rows;
        if (!image_create(s->w * V.sx, s->h * V.scale, 1, 1, fmt,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                          | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                          | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                          | VK_IMAGE_USAGE_SAMPLED_BIT,
                          0, VK_IMAGE_ASPECT_COLOR_BIT, &s->img, &s->mem, &s->view)) {
            s->va = 0;
            return NULL;
        }
        if (V.sx > 1
            && !image_create(s->w, s->h, 1, 1, fmt,
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             0, VK_IMAGE_ASPECT_COLOR_BIT, &s->nimg, &s->nmem, &s->nview))
            s->nimg = VK_NULL_HANDLE;
        segment_begin();
        barrier_image(V.ucb, s->img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (s->nimg)
            barrier_image(V.ucb, s->nimg, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        surf_upload(s);
        s->hash = surf_hash(s);
        s->hash_valid = 1;
    } else if (!s->dirty && s != V.cur_cs) {
        /* Not drawn since its last copy-back: guest RAM is the truth, and the
         * title (or a copy) may have written it since. */
        uint64_t h;
        if (rows > s->rows)
            s->rows = rows;
        h = surf_hash_now(s);
        if (!s->hash_valid || h != s->hash) {
            static int trace = -1;
            static uint32_t shown;
            if (trace < 0)
                trace = getenv("RECOMP_VK_SURF_UPLOAD_TRACE") != NULL;
            if (trace && shown++ < 400)
                fprintf(stderr, "  [VK] F%u re-upload surface %08X %ux%u pitch %u swz %d bpp %u rows %u (hash %s)\n",
                        nv2a_pb_presented_frames(), s->va, s->w, s->h, s->pitch, s->swz,
                        s->bpp, s->rows, s->hash_valid ? "changed" : "unknown");
            segment_begin();
            surf_upload(s);
            s->hash = h;
            s->hash_valid = 1;
        }
    }
    if (rows > s->rows)
        s->rows = rows;
    s->last_use = V.frame;
    return s;
}

/* `samples` > 1: the depth of a multisampled twin, `h` rows (a separate
 * buffer from the single-sampled one of the same key). */
static Zeta *zeta_get(uint32_t key, uint32_t w, uint32_t samples, uint32_t h)
{
    Zeta *z = NULL;
    int i;
    if (!key)
        return NULL;
    if (samples <= 1) {
        samples = 1;
        h = VK_SURF_H;
        if (w < 1024)
            w = 1024;
    }
    for (i = 0; i < V.nzeta; i++)
        if (V.zeta[i].key == key && V.zeta[i].samples == samples
                && V.zeta[i].w >= w && V.zeta[i].h >= h)
            return &V.zeta[i];
    for (i = 0; i < V.nzeta; i++)
        if (V.zeta[i].key == key && V.zeta[i].samples == samples) {
            z = &V.zeta[i];           /* too narrow: replace */
            if (z == V.cur_zs)
                end_rendering();
            retire(z->img, z->mem, z->view);
            if (z->dview)
                retire(VK_NULL_HANDLE, VK_NULL_HANDLE, z->dview);
            break;
        }
    if (!z) {
        if (V.nzeta == VK_MAX_ZETA) {
            int best = 0;
            for (i = 1; i < V.nzeta; i++)
                if (V.zeta[i].last_use < V.zeta[best].last_use)
                    best = i;
            z = &V.zeta[best];
            if (z == V.cur_zs)
                end_rendering();
            retire(z->img, z->mem, z->view);
            if (z->dview)
                retire(VK_NULL_HANDLE, VK_NULL_HANDLE, z->dview);
        } else {
            z = &V.zeta[V.nzeta++];
        }
    }
    memset(z, 0, sizeof *z);
    z->key = key;
    z->w = w;
    z->h = h;
    z->samples = samples;
    s_img_samples = samples;
    i = image_create(z->w * V.sx, z->h * V.scale, 1, 1, V.dsfmt,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                     | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0,
                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                     &z->img, &z->mem, &z->view);
    s_img_samples = 1;
    if (!i) {
        z->key = 0;
        return NULL;
    }
    segment_begin();
    barrier_image(V.ucb, z->img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                  1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    return z;
}

/* Before drawing into `cs`: other surfaces still holding GPU-only contents
 * over its memory. Titles alias one buffer in two shapes -- RSC2 clears its
 * 512x512 shadow map and a 64x64 effect target through a linear surface,
 * then draws them swizzled -- and those contents normally travel through
 * guest RAM (a sync). A surface that was only cleared to one colour, over
 * the same bytes, is the same bytes in any layout: clear `cs` to it on the
 * GPU instead. Returns 1 when a sync is still needed. */
static int alias_resolve(Surf *cs)
{
    uint32_t bytes = cs->swz ? cs->w * cs->h * cs->bpp : cs->rows * cs->pitch;
    int i, need = 0;
    for (i = 0; i < V.nsurf; i++) {
        Surf *q = &V.surf[i];
        if (q == cs || !q->dirty || !(cs->va < q->va + surf_bytes(q) && cs->va + bytes > q->va))
            continue;
        if (V.rt_direct && q->clear_only && q->va == cs->va && surf_bytes(q) == bytes
                && q->bpp == cs->bpp && q->fmt == cs->fmt) {
            VkClearColorValue cc;
            VkImageSubresourceRange rng;
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            segment_begin();
            end_rendering();
            mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT
                             | VK_ACCESS_TRANSFER_READ_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            memset(&cc, 0, sizeof cc);
            cc.float32[0] = (float)((q->clear_argb >> 16) & 0xFF) / 255.0f;
            cc.float32[1] = (float)((q->clear_argb >> 8) & 0xFF) / 255.0f;
            cc.float32[2] = (float)(q->clear_argb & 0xFF) / 255.0f;
            cc.float32[3] = (float)(q->clear_argb >> 24) / 255.0f;
            memset(&rng, 0, sizeof rng);
            rng.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rng.levelCount = 1;
            rng.layerCount = 1;
            vkCmdClearColorImage(V.dcb, cs->img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rng);
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                             | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &mb, 0, NULL, 0, NULL);
            /* `cs` now carries those bytes back to guest RAM; `q` has none. */
            cs->dirty = 1;
            cs->dirty_rows = cs->h;
            if (!cs->swz && cs->rows < q->rows)
                cs->rows = q->rows;
            cs->gen = ++V.gen;
            cs->clear_only = 1;
            cs->clear_argb = q->clear_argb;
            cs->ms_stale = 1;
            q->dirty = 0;
            q->dirty_rows = 0;
            q->clear_only = 0;
            V.st_alias_clears++;
            continue;
        }
        need = 1;
    }
    return need;
}

/* ---- multisampling ----------------------------------------------------- */

/* The scene targets: 640 wide, 32-bit, linear, drawn with depth. Smaller
 * targets (reflections, shadow maps, effects) stay single-sampled; their
 * edges are filtered away when they are sampled anyway. */
static int ms_eligible(const Surf *cs, const Nv2aVkState *st)
{
    return V.msaa > 1 && V.up_pipe && !cs->swz && cs->bpp == 4 && cs->w == 640
        && cs->fmt == VK_FORMAT_B8G8R8A8_UNORM && st->zeta_key && cs->rows <= 480;
}

static void ms_create(Surf *cs)
{
    int ok;
    s_img_samples = V.msaa;
    ok = image_create(cs->w * V.sx, 480 * V.scale, 1, 1, cs->fmt,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, VK_IMAGE_ASPECT_COLOR_BIT,
                      &cs->msimg, &cs->msmem, &cs->msview);
    s_img_samples = 1;
    if (!ok) {
        cs->msimg = VK_NULL_HANDLE;
        return;
    }
    cs->msh = 480;
    cs->ms_stale = 1;
    segment_begin();
    barrier_image(V.dcb, cs->msimg, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

/* A descriptor set for a fullscreen pass: `a` and `b` (b may be NULL). */
static VkDescriptorSet post_set(VkImageView a, VkImageView b)
{
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo ii[2];
    VkWriteDescriptorSet wr[2];
    VkDescriptorSet set;
    int k;
    ai.descriptorPool = V.dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &V.post_dsl;
    if (vkAllocateDescriptorSets(V.dev, &ai, &set) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    V.sets_used++;
    ii[0].sampler = ii[1].sampler = V.post_samp;
    ii[0].imageView = a;
    ii[1].imageView = b ? b : a;
    ii[0].imageLayout = ii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    memset(wr, 0, sizeof wr);
    for (k = 0; k < 2; k++) {
        wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[k].dstSet = set;
        wr[k].dstBinding = (uint32_t)k;
        wr[k].descriptorCount = 1;
        wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(V.dev, 2, wr, 0, NULL);
    return set;
}

/* `cs->img` was written directly: copy it into the multisampled twin (every
 * sample of a pixel gets the pixel) before the next pass draws there. */
static void ms_refresh(Surf *cs)
{
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    VkDescriptorSet set = post_set(cs->view, NULL);
    VkViewport vp;
    VkRect2D sc;
    if (!set)
        return;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    sc.offset.x = sc.offset.y = 0;
    sc.extent.width = cs->w * V.sx;
    sc.extent.height = cs->msh * V.scale;
    ri.renderArea = sc;
    ri.layerCount = 1;
    ca.imageView = cs->msview;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(V.dcb, &ri);
    vp.x = vp.y = 0.0f;
    vp.width = (float)sc.extent.width;
    vp.height = (float)sc.extent.height;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(V.dcb, 0, 1, &vp);
    vkCmdSetScissor(V.dcb, 0, 1, &sc);
    vkCmdBindPipeline(V.dcb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.up_pipe);
    vkCmdBindDescriptorSets(V.dcb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.post_pl, 0, 1, &set, 0, NULL);
    vkCmdDraw(V.dcb, 3, 1, 0, 0);
    vkCmdEndRendering(V.dcb);
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    V.last_set = VK_NULL_HANDLE;    /* the draw path rebinds its own */
    cs->ms_stale = 0;
    V.st_ms_refresh++;
}

/* ---- ambient occlusion -------------------------------------------------- */

/* RECOMP_VK_AO=1: screen-space ambient occlusion from the depth buffer alone
 * (scalable ambient obscurance, McGuire et al. 2012), multiplied into the
 * presented scene between its 3D draws and the HUD.
 *
 * The title keeps a w-buffer, stored reversed as WREV_C / w, so every depth
 * sample is a linear eye distance; with the field of view that gives eye-
 * space positions. The title's projection is not read: the vertical field of
 * view is a setting (RECOMP_VK_AO_FOV, degrees), and the horizontal follows
 * from the shown aspect. RECOMP_VK_AO_RADIUS (title units, ~metres),
 * _INTENSITY, _BIAS, _FAR (where it fades out) and _NEAR (where it fades in,
 * so the car keeps the title's own shading) tune it; RECOMP_VK_AO_SHOW=1
 * shows the occlusion alone, 2 the depth in 4-unit bands, 3 how much each
 * surface faces the camera. */

static const char *k_ao_common =
    "#version 450\n"
    "#ifdef MS\n"
    "layout(set = 0, binding = 0) uniform sampler2DMS dz;\n"
    "#else\n"
    "layout(set = 0, binding = 0) uniform sampler2D dz;\n"
    "#endif\n"
    "layout(set = 0, binding = 1) uniform sampler2D aot;\n"
    "layout(push_constant) uniform PC { vec4 proj; vec4 prm; vec4 sz; } pc;\n"
    "layout(location = 0) out vec4 o;\n"
    "float W(ivec2 p) {\n"
    "    p = clamp(p, ivec2(0), ivec2(pc.sz.xy) - 1);\n"
    "    float d = texelFetch(dz, p, 0).r;\n"
    "    return d > 0.0 ? 1.0 / (pc.sz.z * d) : 1e9;\n"
    "}\n"
    "vec3 P(vec2 px, float w) {\n"
    "    vec2 ndc = px / pc.sz.xy * 2.0 - 1.0;\n"
    "    return vec3(ndc * pc.proj.xy * w, w);\n"
    "}\n";

/* proj.xy tan(half fov) x/y; prm radius, intensity, bias, far;
 * sz render size in pixels, 1/WREV_C, blur step. */
static const char *k_ao_main =
    "void main() {\n"
    "    vec2 fc = gl_FragCoord.xy;\n"
    "    ivec2 ip = ivec2(fc);\n"
    "    float w = W(ip);\n"
    "    if (pc.proj.z > 1.5 && pc.proj.z < 2.5) { o = vec4(fract(w / 4.0)); return; }\n"
    "    if (w > pc.prm.w) { o = vec4(1.0); return; }\n"
    "    vec3 p = P(fc, w);\n"
    "    float wl = W(ip - ivec2(1, 0)), wr = W(ip + ivec2(1, 0));\n"
    "    float wu = W(ip - ivec2(0, 1)), wd = W(ip + ivec2(0, 1));\n"
    "    vec3 dx = abs(wr - w) < abs(w - wl) ? P(fc + vec2(1, 0), wr) - p : p - P(fc - vec2(1, 0), wl);\n"
    "    vec3 dy = abs(wd - w) < abs(w - wu) ? P(fc + vec2(0, 1), wd) - p : p - P(fc - vec2(0, 1), wu);\n"
    "    vec3 n = normalize(cross(dy, dx));\n"
    "    if (n.z > 0.0) n = -n;\n"
    "    if (pc.proj.z > 2.5) { o = vec4(pc.proj.z > 3.5 ? abs(n.x) : -n.z); return; }\n"
    "    float R = pc.prm.x;\n"
    "    float rpx = min(R / (w * pc.proj.y) * 0.5 * pc.sz.y, 0.1 * pc.sz.y);\n"
    "    if (rpx < 1.0) { o = vec4(1.0); return; }\n"
    "    const int B[16] = int[](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);\n"
    "    float phi = 6.2831853 * float(B[(ip.y & 3) * 4 + (ip.x & 3)]) / 16.0;\n"
    "    float sum = 0.0;\n"
    "    const int N = 16;\n"
    "    for (int i = 0; i < N; i++) {\n"
    "        float a = (float(i) + 0.5) / float(N);\n"
    "        float ang = a * 6.2831853 * 7.0 + phi;\n"
    "        vec2 q = fc + vec2(cos(ang), sin(ang)) * a * rpx;\n"
    "        if (any(lessThan(q, vec2(0.0))) || any(greaterThanEqual(q, pc.sz.xy))) continue;\n"
    "        vec3 v = P(q, W(ivec2(q))) - p;\n"
    "        float vv = dot(v, v), vn = dot(v, n);\n"
    "        float f = max(R * R - vv, 0.0);\n"
    "        sum += f * f * f * max((vn - pc.prm.z) / (0.01 + vv), 0.0);\n"
    "    }\n"
    "    float R6 = R * R * R * R * R * R;\n"
    "    float ao = max(0.0, 1.0 - sum * pc.prm.y / R6 * (5.0 / float(N)));\n"
    "    ao = mix(ao, 1.0, smoothstep(pc.prm.w * 0.6, pc.prm.w, w));\n"
    "    ao = mix(1.0, ao, smoothstep(pc.proj.w * 0.6, pc.proj.w, w));\n"
    "    o = vec4(ao);\n"
    "}\n";

/* Depth-aware blur of the occlusion, as a factor the target is multiplied by. */
static const char *k_ao_apply =
    "void main() {\n"
    "    ivec2 ip = ivec2(gl_FragCoord.xy);\n"
    "    float w0 = W(ip);\n"
    "    if (w0 > pc.prm.w) { o = vec4(1.0); return; }\n"
    "    float s = 0.0, ws = 0.0;\n"
    "    for (int y = -2; y < 2; y++)\n"
    "        for (int x = -2; x < 2; x++) {\n"
    "            ivec2 q = clamp(ip + ivec2(x, y), ivec2(0), ivec2(pc.sz.xy) - 1);\n"
    "            float k = max(0.0, 1.0 - abs(W(q) - w0) / (0.05 * w0));\n"
    "            s += texelFetch(aot, q, 0).r * k;\n"
    "            ws += k;\n"
    "        }\n"
    "    float ao = ws > 0.0 ? s / ws : 1.0;\n"
    "    o = vec4(vec3(ao), 1.0);\n"
    "}\n";

static VkPipeline ao_build(const char *body, int ms, VkFormat fmt, uint32_t samples)
{
    char src[8192];
    VkShaderModule fs;
    VkPipeline p;
    /* #version must stay first: the define goes after it. */
    snprintf(src, sizeof src, "#version 450\n%s%s%s", ms ? "#define MS 1\n" : "",
             strchr(k_ao_common, '\n') + 1, body);
    fs = glsl_module(src, 1);
    if (!fs)
        return VK_NULL_HANDLE;
    /* RECOMP_VK_AO_SHOW=1: the occlusion term alone, in place of the scene. */
    p = post_pipeline(fs, fmt, samples,
                      fmt == VK_FORMAT_R8_UNORM || getenv("RECOMP_VK_AO_SHOW") ? 0 : 1);
    vkDestroyShaderModule(V.dev, fs, NULL);
    return p;
}

static void ao_init(void)
{
    const char *e = getenv("RECOMP_VK_AO");
    float fov;
    V.ao_on = e && *e && strcmp(e, "0") != 0 && V.post_pl;
    if (!V.ao_on)
        return;
    e = getenv("RECOMP_VK_AO_RADIUS");
    V.ao_radius = e ? (float)atof(e) : 1.0f;
    e = getenv("RECOMP_VK_AO_INTENSITY");
    V.ao_intensity = e ? (float)atof(e) : 0.7f;
    e = getenv("RECOMP_VK_AO_NEAR");
    V.ao_near = e ? (float)atof(e) : 8.0f;
    e = getenv("RECOMP_VK_AO_BIAS");
    V.ao_bias = e ? (float)atof(e) : 0.03f;
    e = getenv("RECOMP_VK_AO_FAR");
    V.ao_far = e ? (float)atof(e) : 150.0f;
    e = getenv("RECOMP_VK_AO_FOV");
    fov = e ? (float)atof(e) : 50.0f;
    V.ao_tan = tanf(fov * 0.5f * 3.14159265f / 180.0f);
    fprintf(stderr, "  [VK] ambient occlusion: radius %g, intensity %g, fov %g\n",
            V.ao_radius, V.ao_intensity, fov);
}

/* The fullscreen pass `pipe` into one attachment. */
static void ao_pass(VkImageView target, VkImageView resolve, uint32_t w, uint32_t h,
                    VkPipeline pipe, VkDescriptorSet set, const float *pc)
{
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkViewport vp;
    VkRect2D sc;
    sc.offset.x = sc.offset.y = 0;
    sc.extent.width = w;
    sc.extent.height = h;
    ri.renderArea = sc;
    ri.layerCount = 1;
    ca.imageView = target;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = pipe == V.ao_pipe[0] || pipe == V.ao_pipe[1]
              ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (resolve) {
        ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        ca.resolveImageView = resolve;
        ca.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(V.dcb, &ri);
    vp.x = vp.y = 0.0f;
    vp.width = (float)w;
    vp.height = (float)h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(V.dcb, 0, 1, &vp);
    vkCmdSetScissor(V.dcb, 0, 1, &sc);
    vkCmdBindPipeline(V.dcb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(V.dcb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.post_pl, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(V.dcb, V.post_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48, pc);
    vkCmdDraw(V.dcb, 3, 1, 0, 0);
    vkCmdEndRendering(V.dcb);
}

static void ao_barrier(void)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                     | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                     | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                     | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                     | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                         | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                         | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                         | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                         | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                         | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

/* Before each draw: is this where the presented scene's 3D ends? The first
 * draw without a depth test after the frame's depth-tested ones, into a
 * surface that is shown. Runs the passes and ends the rendering instance
 * (the caller begins it again); returns 1 when it did. */
static int ao_maybe(const Nv2aVkState *st)
{
    Surf *cs = V.cur_cs;
    Zeta *zs = V.cur_zs;
    int ms;
    uint32_t w, h, va;
    float pc[12];
    VkDescriptorSet set;

    if (!cs || !zs || cs->swz || cs->bpp != 4 || cs->w != 640)
        return 0;
    va = cs->va & 0x0FFFFFFFu;
    if (va != (V.flip_va[0] & 0x0FFFFFFFu) && va != (V.flip_va[1] & 0x0FFFFFFFu))
        return 0;
    if (V.ao_frame != V.frame) {
        V.ao_frame = V.frame;
        V.ao_zdraws = 0;
    }
    if (st->depth_test) {
        if (depth_mode(st) == 2 && V.wrev)
            V.ao_zdraws++;
        return 0;
    }
    if (V.ao_zdraws < 32 || V.ao_done == V.frame + 1)
        return 0;
    V.ao_done = V.frame + 1;
    ms = cs->msimg && zs->samples > 1;
    if (!V.ao_pipe[ms])
        V.ao_pipe[ms] = ao_build(k_ao_main, ms, VK_FORMAT_R8_UNORM, 1);
    if (!V.ao_apply[ms])
        V.ao_apply[ms] = ao_build(k_ao_apply, ms, cs->fmt, ms ? V.msaa : 1);
    if (!V.ao_pipe[ms] || !V.ao_apply[ms]) {
        fprintf(stderr, "  [VK] ambient occlusion shaders failed -- off\n");
        V.ao_on = 0;
        return 0;
    }
    if (!V.ao_img) {
        if (!image_create(640 * V.sx, 480 * V.scale, 1, 1, VK_FORMAT_R8_UNORM,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                          VK_IMAGE_ASPECT_COLOR_BIT, &V.ao_img, &V.ao_mem, &V.ao_view)) {
            V.ao_on = 0;
            return 0;
        }
        barrier_image(V.dcb, V.ao_img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    if (!zs->dview) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = zs->img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = V.dsfmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(V.dev, &vi, NULL, &zs->dview) != VK_SUCCESS) {
            zs->dview = VK_NULL_HANDLE;
            return 0;
        }
    }
    set = post_set(zs->dview, V.ao_view);
    if (!set)
        return 0;
    end_rendering();
    w = 640 * V.sx;
    h = (cs->msimg ? cs->msh : (cs->rows < 480 ? cs->rows : 480)) * V.scale;
    pc[0] = V.ao_tan * (fb_widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f);
    pc[1] = V.ao_tan;
    pc[2] = getenv("RECOMP_VK_AO_SHOW") ? (float)atof(getenv("RECOMP_VK_AO_SHOW")) : 0.0f;
    pc[3] = V.ao_near;
    pc[4] = V.ao_radius;
    pc[5] = V.ao_intensity;
    pc[6] = V.ao_bias;
    pc[7] = V.ao_far;
    pc[8] = (float)w;
    pc[9] = (float)h;
    pc[10] = 1.0f / WREV_C;
    pc[11] = (float)((V.scale + 1) / 2);
    ao_barrier();
    ao_pass(V.ao_view, VK_NULL_HANDLE, w, h, V.ao_pipe[ms], set, pc);
    ao_barrier();
    if (ms)
        ao_pass(cs->msview, cs->view, w, h, V.ao_apply[ms], set, pc);
    else
        ao_pass(cs->view, VK_NULL_HANDLE, w, h, V.ao_apply[ms], set, pc);
    ao_barrier();
    cs->dirty = 1;
    cs->gen = ++V.gen;
    cs->clear_only = 0;
    if (cs->dirty_rows < h / V.scale)
        cs->dirty_rows = h / V.scale;
    V.last_set = VK_NULL_HANDLE;
    V.st_ao++;
    return 1;
}

/* Make (colour, zeta) the rendering instance being recorded. */
static int begin_target(const Nv2aVkState *st)
{
    Surf *cs;
    Zeta *zs;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    uint32_t w, h;

    cs = surf_get(st);
    if (cs)
        settle_inflight(cs->va, cs->rows * cs->pitch, cs);
    if (cs && any_dirty_overlap(cs->va, cs->rows * cs->pitch, cs) && alias_resolve(cs)) {
        if (getenv("RECOMP_VK_SYNC_TRACE")) {
            int k;
            fprintf(stderr, "[VKALIAS] F%u target %08X %ux%u pitch %u swz %d rows %u bytes %u:",
                    nv2a_pb_presented_frames(), cs->va, cs->w, cs->h, cs->pitch, cs->swz,
                    cs->rows, cs->rows * cs->pitch);
            for (k = 0; k < V.nsurf; k++) {
                const Surf *q = &V.surf[k];
                if (q != cs && q->dirty && cs->va < q->va + surf_bytes(q)
                        && cs->va + cs->rows * cs->pitch > q->va)
                    fprintf(stderr, " dirty %08X %ux%u pitch %u swz %d rows %u bytes %zu",
                            q->va, q->w, q->h, q->pitch, q->swz, q->rows, surf_bytes(q));
            }
            fputc('\n', stderr);
        }
        vk_sync(1);
        cs = surf_get(st);
    }
    if (cs && !cs->msimg && ms_eligible(cs, st))
        ms_create(cs);
    zs = cs && cs->msimg ? zeta_get(st->zeta_key, cs->w, V.msaa, cs->msh)
                         : zeta_get(st->zeta_key, cs ? cs->w : 1024, 1, 0);
    if (!cs && !zs)
        return 0;
    if (V.kick_draws > 0 && V.recording && V.seg_draws >= (uint32_t)V.kick_draws
            && !(V.rendering && V.cur_cs == cs && V.cur_zs == zs))
        vk_kick();
    segment_begin();
    if (V.rendering && V.cur_cs == cs && V.cur_zs == zs)
        return 1;
    end_rendering();
    if (cs && cs->msimg && cs->ms_stale)
        ms_refresh(cs);
    {
        /* Draws in the previous instance precede this one's. */
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(V.dcb,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                             | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }
    w = cs ? cs->w : zs->w;
    h = cs ? (cs->msimg ? cs->msh : cs->h) : zs->h;
    if (zs && zs->w < w) w = zs->w;
    if (zs && zs->h < h) h = zs->h;
    ri.renderArea.extent.width = w * V.sx;
    ri.renderArea.extent.height = h * V.scale;
    ri.layerCount = 1;
    if (cs) {
        ca.imageView = cs->view;
        ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (cs->msimg) {
            /* Drawn multisampled, resolved into `img` as the pass ends. */
            ca.imageView = cs->msview;
            ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            ca.resolveImageView = cs->view;
            ca.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
    }
    if (zs) {
        da.imageView = zs->view;
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        ri.pStencilAttachment = &da;
    }
    vkCmdBeginRendering(V.dcb, &ri);
    V.rendering = 1;
    V.cur_cs = cs;
    V.cur_zs = zs;
    V.cur_samples = cs && cs->msimg ? V.msaa : 1;
    if (zs)
        zs->last_use = V.frame;
    return 1;
}

/* The clip rectangle, within the attachments. */
static VkRect2D clip_rect(const Nv2aVkState *st)
{
    VkRect2D r;
    uint32_t w = V.cur_cs ? V.cur_cs->w : V.cur_zs->w;
    uint32_t h = V.cur_cs ? (V.cur_cs->msimg ? V.cur_cs->msh : V.cur_cs->h) : V.cur_zs->h;
    uint32_t x0 = st->clip_x, y0 = st->clip_y;
    uint32_t x1 = st->clip_x + st->clip_w, y1 = st->clip_y + st->clip_h;
    if (V.cur_zs && V.cur_zs->w < w) w = V.cur_zs->w;
    if (V.cur_zs && V.cur_zs->h < h) h = V.cur_zs->h;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x0 > x1) x0 = x1;
    if (y0 > y1) y0 = y1;
    r.offset.x = (int32_t)(x0 * V.sx);
    r.offset.y = (int32_t)(y0 * V.scale);
    r.extent.width = (x1 - x0) * V.sx;
    r.extent.height = (y1 - y0) * V.scale;
    return r;
}

static void mark_drawn(const Nv2aVkState *st)
{
    if (V.cur_cs) {
        uint32_t rows = st->clip_y + st->clip_h;
        V.cur_cs->dirty = 1;
        V.cur_cs->gen = ++V.gen;
        V.cur_cs->clear_only = 0;
        if (rows > V.cur_cs->h)
            rows = V.cur_cs->h;
        if (rows > V.cur_cs->dirty_rows)
            V.cur_cs->dirty_rows = rows;
        if (rows > V.cur_cs->rows)
            V.cur_cs->rows = rows;
    }
}

/* ---- entry points ------------------------------------------------------- */

void nv2a_vk_clear(const Nv2aVkState *st, uint32_t flags, uint32_t argb, uint32_t zs)
{
    VkClearAttachment at[2];
    VkClearRect cr;
    uint32_t n = 0;
    float zmax;

    if (!nv2a_vk_active())
        return;
    if (dlog_active())
        fprintf(s_dlog, "F%u C%u clear %X colour %08X zs %08X surf %08X zeta %08X clip %u,%u,%u,%u w%u\n",
                nv2a_pb_presented_frames(), s_dlog_seq++, flags, argb, zs, st->color_va,
                st->zeta_key, st->clip_x, st->clip_y, st->clip_w, st->clip_h, st->wbuffer);
    if (!begin_target(st))
        return;
    zmax = st->zeta_fmt == 1 ? 65535.0f : 16777215.0f;
    memset(at, 0, sizeof at);
    if ((flags & 0xF0) && V.cur_cs) {
        at[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        at[n].colorAttachment = 0;
        at[n].clearValue.color.float32[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
        at[n].clearValue.color.float32[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
        at[n].clearValue.color.float32[2] = (float)(argb & 0xFF) / 255.0f;
        at[n].clearValue.color.float32[3] = (float)(argb >> 24) / 255.0f;
        n++;
    }
    if ((flags & 0x03) && V.cur_zs) {
        if (flags & 0x01)
            at[n].aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (flags & 0x02)
            at[n].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        float raw = st->zeta_fmt == 1 ? (float)(zs & 0xFFFF) : (float)(zs >> 8);
        /* A w-buffer's raw value is a w: reversed, it is stored as WREV_C / w. */
        if (depth_mode(st) == 2)
            at[n].clearValue.depthStencil.depth = raw > WREV_C ? WREV_C / raw : 1.0f;
        else
            at[n].clearValue.depthStencil.depth = raw / zmax;
        at[n].clearValue.depthStencil.stencil = zs & 0xFF;
        n++;
    }
    if (!n)
        return;
    cr.rect = clip_rect(st);
    cr.baseArrayLayer = 0;
    cr.layerCount = 1;
    if (!cr.rect.extent.width || !cr.rect.extent.height)
        return;
    vkCmdClearAttachments(V.dcb, n, at, 1, &cr);
    if (flags & 0xF0) {
        mark_drawn(st);
        /* A clear of every channel over the whole surface. */
        if ((flags & 0xF0) == 0xF0 && V.cur_cs && cr.rect.offset.x == 0 && cr.rect.offset.y == 0
                && cr.rect.extent.width >= V.cur_cs->w * V.sx
                && cr.rect.extent.height >= (V.cur_cs->swz ? V.cur_cs->h : V.cur_cs->rows) * V.scale) {
            V.cur_cs->clear_only = 1;
            V.cur_cs->clear_argb = argb;
        }
    }
}

static VkStencilOp stencil_op(uint32_t op)
{
    switch (op) {
    case 0x1E00: case 1: return VK_STENCIL_OP_KEEP;
    case 0x0000: case 2: return VK_STENCIL_OP_ZERO;
    case 0x1E01: case 3: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: case 6: return VK_STENCIL_OP_INVERT;
    case 0x8507: case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:             return VK_STENCIL_OP_KEEP;
    }
}

static VkCompareOp compare_op(uint32_t f)
{
    if (f >= 0x200 && f <= 0x207)
        return (VkCompareOp)(f - 0x200);
    return VK_COMPARE_OP_ALWAYS;
}

typedef struct {
    float vp[4];
    uint32_t misc[4], misc2[4];
    float fogc[4], fogp[4];
    uint32_t cw[8][4];
    float f0[8][4], f1[8][4];
    float fc0[4], fc1[4];
    float ts[4][4];
    uint32_t misc3[4];
    float pts[4];        /* point size px, render scale, sprite, size from oPts */
    float zoff[4];       /* polygon offset: slope factor, units (depth LSBs);
                          * host pixels per title pixel across, down */
} Nv2aVkUbo;

/* ---- specialised fragment shaders ----------------------------------------
 *
 * The general fragment shader reads the combiner setup -- texture-shader
 * modes, eight stages of combiner words, the final combiner, alpha test --
 * from uniforms, so every pixel walks all of it, and its register file R[]
 * is indexed at run time, which keeps it out of registers. At 3x that shader
 * was nearly all the GPU time. A draw's setup is compiled again with those
 * values as constants (nv2a.frag NV2A_SPEC), and the compiler keeps only what
 * the setup does. A worker thread compiles them and builds their pipelines;
 * draws use the general shader until theirs is ready, so nothing stalls.
 * SPIR-V is cached on disk (frag_<source hash>_<setup>.spv). RECOMP_VK_SPEC=0
 * turns it off. */

typedef struct {
    uint32_t misc_x, misc_y, misc_w;
    uint32_t misc2[4], misc3[4];
    uint32_t cw[8][4];
    float ptsz, tsz[4], vpw;
    uint32_t fast;
} SpecVals;

typedef struct {
    uint64_t h;
    volatile int state;         /* 0 free, 1 queued, 2 ready, -1 failed */
    VkShaderModule fs;
} SpecEnt;

typedef struct {
    SpecVals sv;
    uint64_t h;
    SpecEnt *se;
    Pipe *pp;
    PipeDesc d;
} SpecJob;

#define VK_MAX_SPECS 4096
#define VK_SPEC_JOBS 1024
static SpecEnt s_spec[VK_MAX_SPECS];
static SpecJob s_jobs[VK_SPEC_JOBS];
static unsigned s_job_head, s_job_tail;
static pthread_mutex_t s_job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_job_cond = PTHREAD_COND_INITIALIZER;
static int s_spec_on = -1;
static uint64_t s_frag_src_hash;
static uint32_t s_spec_ready, s_spec_failed;

static int glsl_to_spirv(const char *src, uint32_t hash, int frag, uint32_t **words, size_t *bytes);

/* The specialised source: constants, the common block, nv2a.frag's body. */
static char *spec_source(const SpecVals *v)
{
    size_t n = sizeof k_nv2a_frag_src + sizeof k_nv2a_common_glsl_src + 4096;
    char *src = (char *)malloc(n), *q;
    const char *body = k_nv2a_frag_src, *inc;
    int i;
    if (!src)
        return NULL;
    q = src;
    q += sprintf(q, "#version 450\n#define NV2A_SPEC 1\n%s",
                 v->fast ? "#define NV2A_FAST_DEPTH 1\n" : "");
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
    q += sprintf(q, "const float SP_TSZ[4] = float[4](%.9g, %.9g, %.9g, %.9g);\n",
                 v->tsz[0], v->tsz[1], v->tsz[2], v->tsz[3]);
    q += sprintf(q, "%s\n", k_nv2a_common_glsl_src);
    /* nv2a.frag minus its #version, #extension and #include lines. */
    while (*body) {
        const char *eol = strchr(body, '\n');
        size_t len = eol ? (size_t)(eol - body) + 1 : strlen(body);
        if (strncmp(body, "#version", 8) && strncmp(body, "#extension", 10)
                && strncmp(body, "#include", 8)) {
            memcpy(q, body, len);
            q += len;
        }
        body += len;
    }
    *q = 0;
    (void)inc;
    return src;
}

static VkShaderModule spec_compile(const SpecVals *v, uint64_t h)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    const char *dir = cache_dir();
    char path[1100];
    uint32_t *spv = NULL;
    size_t bytes = 0, n = 0;
    void *f = NULL;
    char *src;

    if (dir) {
        snprintf(path, sizeof path, "%s/frag_%016llX_%016llX.spv", dir,
                 (unsigned long long)s_frag_src_hash, (unsigned long long)h);
        f = file_read(path, &n);
    }
    if (f && n && (n & 3) == 0) {
        ci.codeSize = n;
        ci.pCode = (const uint32_t *)f;
        if (vkCreateShaderModule(V.dev, &ci, NULL, &m) != VK_SUCCESS)
            m = VK_NULL_HANDLE;
        free(f);
        if (m)
            return m;
    } else {
        free(f);
    }
    src = spec_source(v);
    if (!src)
        return VK_NULL_HANDLE;
    if (glsl_to_spirv(src, (uint32_t)h, 1, &spv, &bytes)) {
        ci.codeSize = bytes;
        ci.pCode = spv;
        if (vkCreateShaderModule(V.dev, &ci, NULL, &m) != VK_SUCCESS)
            m = VK_NULL_HANDLE;
        if (m && dir)
            file_write(path, spv, bytes, NULL, 0);
    }
    free(spv);
    free(src);
    return m;
}

static void *spec_worker(void *unused)
{
    (void)unused;
    for (;;) {
        SpecJob j;
        pthread_mutex_lock(&s_job_lock);
        while (s_job_head == s_job_tail)
            pthread_cond_wait(&s_job_cond, &s_job_lock);
        j = s_jobs[s_job_tail % VK_SPEC_JOBS];
        s_job_tail++;
        pthread_mutex_unlock(&s_job_lock);

        if (__atomic_load_n(&j.se->state, __ATOMIC_ACQUIRE) == 1) {
            VkShaderModule m = spec_compile(&j.sv, j.h);
            j.se->fs = m;
            __atomic_store_n(&j.se->state, m ? 2 : -1, __ATOMIC_RELEASE);
            if (m) s_spec_ready++; else s_spec_failed++;
        }
        if (__atomic_load_n(&j.se->state, __ATOMIC_ACQUIRE) == 2) {
            VkPipeline p;
            j.d.fs = j.se->fs;
            p = pipe_build(&j.d, 0);
            if (p)
                __atomic_store_n(&j.pp->pipe, p, __ATOMIC_RELEASE);
            else
                __atomic_store_n(&j.pp->failed, 1, __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&j.pp->failed, 1, __ATOMIC_RELEASE);
        }
    }
    return NULL;
}

static int spec_enabled(void)
{
    if (s_spec_on < 0) {
        const char *e = getenv("RECOMP_VK_SPEC");
        pthread_t th;
        s_spec_on = !(e && strcmp(e, "0") == 0);
        if (s_spec_on) {
            s_frag_src_hash = hash_bytes((const uint8_t *)k_nv2a_frag_src, sizeof k_nv2a_frag_src)
                            ^ hash_bytes((const uint8_t *)k_nv2a_common_glsl_src,
                                         sizeof k_nv2a_common_glsl_src) * 31;
            {
                /* glslang's process state before the worker compiles. */
                uint32_t *w = NULL;
                size_t b = 0;
                glsl_to_spirv("#version 450\nvoid main() {}\n", 0, 0, &w, &b);
                free(w);
            }
            if (pthread_create(&th, NULL, spec_worker, NULL) != 0)
                s_spec_on = 0;
            else
                pthread_detach(th);
        }
    }
    return s_spec_on;
}

/* The draw's setup, from the uniforms it is about to be drawn with. */
static void spec_vals(SpecVals *v, const Nv2aVkUbo *ub, int fast)
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
    if (n > 8) n = 8;
    for (i = 0; i < n; i++)
        memcpy(v->cw[i], ub->cw[i], sizeof v->cw[i]);
    v->ptsz = ub->pts[2];
    for (i = 0; i < 4; i++)
        v->tsz[i] = ub->ts[i][2];
    v->vpw = ub->vp[3];
    v->fast = (uint32_t)fast;
}

/* The draw's specialised pipeline when it is ready; otherwise it is queued
 * (once) and NULL tells the caller to draw with the general one. */
static VkPipeline pipeline_spec(VkFormat cfmt, int has_depth, const Nv2aVkState *s,
                                VkShaderModule vs, uint32_t prog, uint32_t ninputs,
                                const Nv2aVkUbo *ub)
{
    int fast = !needs_fragdepth(s);
    SpecVals v;
    PipeDesc d;
    uint64_t k[4], h;
    Pipe *pp;
    SpecEnt *se = NULL;
    uint32_t slot, n;

    if (!spec_enabled())
        return VK_NULL_HANDLE;
    spec_vals(&v, ub, fast);
    h = hash_bytes((const uint8_t *)&v, sizeof v) | 1;
    pipe_desc(&d, cfmt, has_depth, s, vs, ninputs);
    pipe_key(&d, prog, fast, h, k);
    pp = pipe_find(k);
    if (!pp)
        return VK_NULL_HANDLE;
    if (pp->used)
        return __atomic_load_n(&pp->pipe, __ATOMIC_ACQUIRE);
    /* New: find or claim the shader, queue the build. */
    slot = (uint32_t)(h >> 7) & (VK_MAX_SPECS - 1);
    for (n = 0; n < VK_MAX_SPECS; n++) {
        SpecEnt *c = &s_spec[(slot + n) & (VK_MAX_SPECS - 1)];
        if (c->h == h) {
            se = c;
            break;
        }
        if (!c->h) {
            se = c;
            se->h = h;
            se->state = 1;
            break;
        }
    }
    if (!se || se->state < 0)
        return VK_NULL_HANDLE;
    pthread_mutex_lock(&s_job_lock);
    if (s_job_head - s_job_tail < VK_SPEC_JOBS) {
        SpecJob *j = &s_jobs[s_job_head % VK_SPEC_JOBS];
        j->sv = v;
        j->h = h;
        j->se = se;
        j->pp = pp;
        j->d = d;
        s_job_head++;
        memcpy(pp->key, k, sizeof k);
        pp->pipe = VK_NULL_HANDLE;
        pp->failed = 0;
        pp->used = 1;
        V.npipes++;
        pthread_cond_signal(&s_job_cond);
    }
    pthread_mutex_unlock(&s_job_lock);
    return VK_NULL_HANDLE;
}

/* The vertex program constants, uploaded when the title changed them. */
static int const_upload(VkDeviceSize *off)
{
    void *p;
    if (V.const_epoch == V.epoch && V.const_serial == nv2a_vsh_const_serial()) {
        *off = V.const_off;
        return 1;
    }
    p = ring_alloc(&V.ubo, NV2A_VSH_CONSTANTS * 16,
                   V.limits.minUniformBufferOffsetAlignment, off);
    if (!p)
        return 0;
    memcpy(p, nv2a_vsh_constants(), NV2A_VSH_CONSTANTS * 16);
    V.const_off = *off;
    V.const_epoch = V.epoch;
    V.const_serial = nv2a_vsh_const_serial();
    return 1;
}

/* RECOMP_VK_ZPASS_TRACE=<frame>: that frame's visibility-test draws and the
 * report slots they answer. */
static int zpass_trace(void)
{
    static long at = -2;
    if (at == -2) {
        const char *e = getenv("RECOMP_VK_ZPASS_TRACE");
        at = e ? atol(e) : -1;
    }
    return at >= 0 && nv2a_pb_presented_frames() == (uint32_t)at;
}

static void draw_common(const Nv2aVkState *st, VkShaderModule vs, uint32_t prog,
                        uint32_t ninputs, const void *v, uint32_t nv,
                        const uint32_t *idx, uint32_t nidx)
{
    VkImageView views[8];
    VkSampler samps[8];
    VkDeviceSize voff = 0, ioff = 0, uoff = 0, coff = 0;
    VkDeviceSize vbytes = (VkDeviceSize)nv * ninputs * 16;
    void *vp, *ip;
    Nv2aVkUbo *ub, *ubm, ubl;
    VkPipeline pipe;
    VkDescriptorSet set;
    VkViewport vpt;
    VkRect2D sc;
    int i, stage_on[4];
    float zmax;

    if (dlog_active())
        dlog_draw(st, prog, v, vbytes, idx, nidx);
    {
        /* RECOMP_VK_SKIP_PROG=<hex>[,<hex>...]: drop those programs' draws,
         * to see what they are. */
        static uint32_t skip[8];
        static int nskip = -1;
        int k;
        if (nskip < 0) {
            const char *e = getenv("RECOMP_VK_SKIP_PROG");
            nskip = 0;
            while (e && *e && nskip < 8) {
                skip[nskip++] = (uint32_t)strtoul(e, (char **)&e, 16);
                while (*e == ',') e++;
            }
        }
        for (k = 0; k < nskip; k++)
            if (skip[k] == prog
                    && !(getenv("RECOMP_VK_SKIP_ODD") && !(nv2a_pb_presented_frames() & 1)))
                return;
    }
    {
        /* RECOMP_VK_STAGE_TRACE: stage programs using modes the shader does
         * not implement (6+: bump-env, BRDF, dot products), once each. */
        static int trace = -1, n;
        static uint32_t seen[32];
        uint32_t sp = st->use_rc ? st->rc->stage_program : 0;
        int k, j;
        if (trace < 0)
            trace = getenv("RECOMP_VK_STAGE_TRACE") != NULL;
        for (k = 0; trace && k < 4; k++)
            if (((sp >> (k * 5)) & 0x1F) > 5) {
                for (j = 0; j < n && seen[j] != sp; j++)
                    ;
                if (j == n && n < 32) {
                    seen[n++] = sp;
                    fprintf(stderr, "  [VK] stage program %05X: modes %u %u %u %u (prog %08X)"
                            " dotmap %08X input %08X tex3 %08X fmt %02X cube %d %ux%u\n",
                            sp, sp & 0x1F, (sp >> 5) & 0x1F, (sp >> 10) & 0x1F,
                            (sp >> 15) & 0x1F, prog, st->dot_mapping, st->other_input,
                            st->tex[3].offset, st->tex[3].color, st->tex[3].cube,
                            st->tex[3].width, st->tex[3].height);
                    {
                        const Nv2aCombiner *rc = st->rc;
                        uint32_t q;
                        fprintf(stderr, "  [VK]   rc control %08X final %08X %08X fc %08X %08X\n",
                                rc->control, rc->final0, rc->final1, rc->final_c0, rc->final_c1);
                        for (q = 0; q < (rc->control & 0xF) && q < 8; q++)
                            fprintf(stderr, "  [VK]   rc%u cicw %08X aicw %08X cocw %08X aocw %08X f0 %08X f1 %08X\n",
                                    q, rc->color_icw[q], rc->alpha_icw[q], rc->color_ocw[q],
                                    rc->alpha_ocw[q], rc->factor0[q], rc->factor1[q]);
                    }
                    for (j = 0; j < 4; j++)
                        fprintf(stderr, "  [VK]   tex%d %08X fmt %02X %ux%u cube %d\n", j,
                                st->tex[j].offset, st->tex[j].color, st->tex[j].width,
                                st->tex[j].height, st->tex[j].cube);
                    j = n - 1;
                }
                break;
            }
    }
    /* A long run of draws into one target (the race's main scene) is kicked
     * in pieces too, so the GPU starts on it while the CPU records the rest. */
    if (V.kick_draws > 0 && V.recording && V.seg_draws >= (uint32_t)V.kick_draws * 2)
        vk_kick();
    /* Textures first: one may be a surface drawn earlier this frame, which
     * ends the segment and copies it back before it is decoded. */
    for (i = 0; i < 4; i++) {
        uint32_t mode = st->use_rc ? (st->rc->stage_program >> (i * 5)) & 0x1F
                                   : (i == 0 && st->tex[0].valid ? 1 : 0);
        uint32_t lv = 1;
        VkImageView view = VK_NULL_HANDLE;
        stage_on[i] = 0;
        views[i] = V.white2d_view;
        views[4 + i] = V.whitecube_view;
        /* Every mode that samples its stage's texture: the projective ones,
         * cube, bump-env, dot-ST/reflect/STR and the dependent reads. */
        int samples = mode == 1 || mode == 2 || mode == 3 || mode == 6 || mode == 7
                   || mode == 9 || (mode >= 11 && mode <= 16) || mode == 18;
        if (samples && tex_get(&st->tex[i], &view, &lv)) {
            stage_on[i] = 1;
            if (st->tex[i].cube)
                views[4 + i] = view;
            else
                views[i] = view;
        }
        samps[i] = samps[4 + i] = sampler(&st->tex[i], lv);
        if (V.state < 0)
            return;
    }

    if (!begin_target(st))
        return;
    if (V.ao_on && ao_maybe(st) && !begin_target(st))
        return;
    pipe = pipeline(V.cur_cs ? V.cur_cs->fmt : VK_FORMAT_UNDEFINED, V.cur_zs != NULL, st,
                    vs, prog, ninputs);
    if (!pipe)
        return;

    vp = ring_alloc(&V.vtx, vbytes, 16, &voff);
    ip = ring_alloc(&V.idx, (VkDeviceSize)nidx * 4, 4, &ioff);
    ubm = (Nv2aVkUbo *)ring_alloc(&V.ubo, sizeof *ubm,
                                  V.limits.minUniformBufferOffsetAlignment, &uoff);
    if (!vp || !ip || !ubm || !const_upload(&coff) || V.sets_used >= VK_MAX_SETS) {
        vk_sync(0);
        if (!begin_target(st))
            return;
        vp = ring_alloc(&V.vtx, vbytes, 16, &voff);
        ip = ring_alloc(&V.idx, (VkDeviceSize)nidx * 4, 4, &ioff);
        ubm = (Nv2aVkUbo *)ring_alloc(&V.ubo, sizeof *ubm,
                                      V.limits.minUniformBufferOffsetAlignment, &uoff);
        if (!vp || !ip || !ubm || !const_upload(&coff))
            return;
    }
    memcpy(vp, v, (size_t)vbytes);
    memcpy(ip, idx, (size_t)nidx * 4);

    /* Per-draw constants. Built in ordinary memory and copied over once: the
     * ring is mapped device memory, write-combined on most drivers, where
     * the reads below (the specialisation key) cost about a microsecond
     * a draw. */
    ub = &ubl;
    memset(ub, 0, sizeof *ub);
    {
        uint32_t w = V.cur_cs ? V.cur_cs->w : V.cur_zs->w;
        uint32_t h = V.cur_cs ? V.cur_cs->h : V.cur_zs->h;
        if (V.cur_zs && V.cur_zs->w < w) w = V.cur_zs->w;
        zmax = st->zeta_fmt == 1 ? 65535.0f : 16777215.0f;
        ub->vp[0] = 2.0f / (float)w;
        ub->vp[1] = 2.0f / (float)h;
        ub->vp[2] = 1.0f / zmax;
        ub->vp[3] = (float)depth_mode(st);
        vpt.x = 0.0f;
        vpt.y = 0.0f;
        vpt.width = (float)(w * V.sx);
        vpt.height = (float)(h * V.scale);
        vpt.minDepth = 0.0f;
        vpt.maxDepth = 1.0f;
    }
    {
        /* RECOMP_VK_DOT_REFLECT=1: the bump-reflection texture-shader modes.
         * They follow the NV2A texture-shader rules, but RalliSport
         * Challenge 2's car paint comes out with white blow-outs on
         * down-facing panels, so they stay off until that is understood. */
        static int refl = -1;
        if (refl < 0) {
            const char *e = getenv("RECOMP_VK_DOT_REFLECT");
            refl = e && strcmp(e, "0") != 0;
        }
        ub->misc[0] = (st->use_rc ? 1u : 0u) | (stage_on[0] ? 2u : 0u) | (refl ? 4u : 0u);
    }
    ub->misc[1] = st->alpha_func;
    ub->misc[2] = st->alpha_ref;
    ub->misc[3] = st->use_rc ? st->rc->stage_program : (stage_on[0] ? 1u : 0u);
    if (st->use_rc) {
        const Nv2aCombiner *rc = st->rc;
        ub->misc2[0] = rc->control;
        ub->misc2[1] = rc->final0;
        ub->misc2[2] = rc->final1;
        for (i = 0; i < 8; i++) {
            ub->cw[i][0] = rc->color_icw[i];
            ub->cw[i][1] = rc->alpha_icw[i];
            ub->cw[i][2] = rc->color_ocw[i];
            ub->cw[i][3] = rc->alpha_ocw[i];
            nv2a_rc_unpack(rc->factor0[i], ub->f0[i]);
            nv2a_rc_unpack(rc->factor1[i], ub->f1[i]);
        }
        nv2a_rc_unpack(rc->final_c0, ub->fc0);
        nv2a_rc_unpack(rc->final_c1, ub->fc1);
    }
    ub->misc2[3] = st->clip_plane_mode;
    {
        /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        float c[4];
        nv2a_rc_unpack(st->fog_color, c);
        ub->fogc[0] = c[2];
        ub->fogc[1] = c[1];
        ub->fogc[2] = c[0];
        ub->fogc[3] = st->fog_enable ? 1.0f : 0.0f;
        ub->fogp[0] = st->fog_param[0];
        ub->fogp[1] = st->fog_param[1];
        ub->fogp[2] = (float)st->fog_mode;
    }
    for (i = 0; i < 4; i++) {
        const Nv2aVkTex *t = &st->tex[i];
        ub->ts[i][0] = ub->ts[i][1] = 1.0f;
        if (stage_on[i] && !t->size_from_format && t->width && t->height) {
            ub->ts[i][0] = 1.0f / (float)t->width;
            ub->ts[i][1] = 1.0f / (float)t->height;
        }
        ub->ts[i][2] = stage_on[i] && t->cube ? 1.0f : 0.0f;
        ub->ts[i][3] = stage_on[i] ? 1.0f : 0.0f;
    }
    ub->misc3[0] = st->alpha_test ? 1u : 0u;
    ub->misc3[1] = st->other_input;
    ub->misc3[2] = st->dot_mapping;
    ub->misc3[3] = st->zint;
    /* Points are square: sized between the two scales when they differ. */
    ub->pts[0] = st->point_size * 0.5f * (float)(V.sx + V.scale);
    ub->pts[1] = 0.5f * (float)(V.sx + V.scale);
    ub->pts[2] = st->topology == 2 && st->point_sprite ? 1.0f : 0.0f;
    ub->pts[3] = st->point_params ? 1.0f : 0.0f;
    {
        /* RECOMP_VK_ZOFFSET=0: draw without the polygon offset. */
        static int zoff = -1;
        if (zoff < 0) {
            const char *e = getenv("RECOMP_VK_ZOFFSET");
            zoff = !(e && strcmp(e, "0") == 0);
        }
        ub->zoff[0] = zoff ? st->poff_factor : 0.0f;
        ub->zoff[1] = zoff ? st->poff_units : 0.0f;
        ub->zoff[2] = (float)V.sx;      /* host pixels per title pixel */
        ub->zoff[3] = (float)V.scale;
    }
    memcpy(ubm, ub, sizeof *ub);

    /* Descriptor set: reused while the images and samplers stay put. */
    if (V.last_set && !memcmp(views, V.last_views, sizeof views)
        && !memcmp(samps, V.last_samps, sizeof samps)) {
        set = V.last_set;
    } else {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        VkDescriptorImageInfo ii[8];
        VkDescriptorBufferInfo bi, ci;
        VkWriteDescriptorSet wr[4];
        ai.descriptorPool = V.dpool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &V.dsl;
        if (vkAllocateDescriptorSets(V.dev, &ai, &set) != VK_SUCCESS)
            return;
        V.sets_used++;
        for (i = 0; i < 8; i++) {
            ii[i].sampler = samps[i];
            ii[i].imageView = views[i];
            ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        bi.buffer = V.ubo.buf;
        bi.offset = 0;
        bi.range = sizeof(Nv2aVkUbo);
        ci.buffer = V.ubo.buf;
        ci.offset = 0;
        ci.range = NV2A_VSH_CONSTANTS * 16;
        memset(wr, 0, sizeof wr);
        for (i = 0; i < 4; i++) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = set;
            wr[i].dstBinding = (uint32_t)i;
        }
        wr[0].descriptorCount = 1;
        wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr[0].pBufferInfo = &bi;
        wr[1].descriptorCount = 4;
        wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[1].pImageInfo = ii;
        wr[2].descriptorCount = 4;
        wr[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[2].pImageInfo = ii + 4;
        wr[3].descriptorCount = 1;
        wr[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        wr[3].pBufferInfo = &ci;
        vkUpdateDescriptorSets(V.dev, 4, wr, 0, NULL);
        V.last_set = set;
        memcpy(V.last_views, views, sizeof views);
        memcpy(V.last_samps, samps, sizeof samps);
    }

    {
        VkPipeline sp = pipeline_spec(V.cur_cs ? V.cur_cs->fmt : VK_FORMAT_UNDEFINED,
                                      V.cur_zs != NULL, st, vs, prog, ninputs, ub);
        if (sp)
            pipe = sp;
        if (sp) V.st_spec_draws++; else V.st_uber_draws++;
    }
    vkCmdBindPipeline(V.dcb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    {
        uint32_t doff[2];
        doff[0] = (uint32_t)uoff;
        doff[1] = (uint32_t)coff;
        vkCmdBindDescriptorSets(V.dcb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl, 0, 1,
                                &set, 2, doff);
    }
    vkCmdBindVertexBuffers(V.dcb, 0, 1, &V.vtx.buf, &voff);
    vkCmdBindIndexBuffer(V.dcb, V.idx.buf, ioff, VK_INDEX_TYPE_UINT32);
    vkCmdSetViewport(V.dcb, 0, 1, &vpt);
    sc = clip_rect(st);
    if (!sc.extent.width || !sc.extent.height)
        return;
    vkCmdSetScissor(V.dcb, 0, 1, &sc);
    {
        float bc[4];
        nv2a_rc_unpack(st->blend_color, bc);
        vkCmdSetBlendConstants(V.dcb, bc);
    }
    {
        /* RECOMP_VK_CULL=0 draws both faces whatever the title asks;
         * RECOMP_VK_FLIP_FRONT swaps the winding meaning. */
        static int cull = -1, flip = -1;
        VkCullModeFlags cm = VK_CULL_MODE_NONE;
        int ccw = st->front_face == 0x0901;
        if (cull < 0) {
            const char *e = getenv("RECOMP_VK_CULL");
            cull = !(e && strcmp(e, "0") == 0);
            flip = getenv("RECOMP_VK_FLIP_FRONT") != NULL;
        }
        if (cull && st->cull_enable) {
            switch (st->cull_face) {
            case 0x0404: cm = VK_CULL_MODE_FRONT_BIT; break;
            case 0x0408: cm = VK_CULL_MODE_FRONT_AND_BACK; break;
            default:     cm = VK_CULL_MODE_BACK_BIT; break;
            }
        }
        if (flip)
            ccw = !ccw;
        vkCmdSetCullMode(V.dcb, cm);
        vkCmdSetFrontFace(V.dcb, ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                     : VK_FRONT_FACE_CLOCKWISE);
    }
    vkCmdSetDepthTestEnable(V.dcb, V.cur_zs && st->depth_test);
    vkCmdSetDepthWriteEnable(V.dcb, V.cur_zs && st->depth_test && st->depth_mask);
    vkCmdSetDepthCompareOp(V.dcb, compare_op(depth_mode(st) == 2 ? depth_func_rev(st->depth_func)
                                                                  : st->depth_func));
    vkCmdSetStencilTestEnable(V.dcb, V.cur_zs && st->st_test);
    vkCmdSetStencilOp(V.dcb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_op(st->st_fail),
                      stencil_op(st->st_zpass), stencil_op(st->st_zfail),
                      compare_op(st->st_func));
    vkCmdSetStencilCompareMask(V.dcb, VK_STENCIL_FACE_FRONT_AND_BACK, st->st_mask & 0xFF);
    vkCmdSetStencilWriteMask(V.dcb, VK_STENCIL_FACE_FRONT_AND_BACK,
                             st->st_write ? (st->st_wmask & 0xFF) : 0);
    vkCmdSetStencilReference(V.dcb, VK_STENCIL_FACE_FRONT_AND_BACK, st->st_ref & 0xFF);
    if (zpass_trace() && V.cur_zs && st->st_test && st->st_write && (st->st_wmask & 0xFF)
            && !V.zpass && V.cur_cs && V.cur_cs->w == 640)
        fprintf(stderr, "[ZPASS] F%u stencil-write prog %08X nidx %u depth %u %X st func %X ref %X mask %X"
                " wmask %X ops fail %X zfail %X zpass %X cm %08X at %u %X\n", nv2a_pb_presented_frames(),
                prog, nidx, st->depth_test, st->depth_func, st->st_func, st->st_ref, st->st_mask,
                st->st_wmask, st->st_fail, st->st_zfail, st->st_zpass, st->color_mask,
                st->alpha_test, st->alpha_ref);
    if (V.zpass && V.nq < VK_MAX_QUERIES) {
        uint32_t q = V.nq++;
        V.st_qdraws++;
        V.qsamples[q] = (uint8_t)V.cur_samples;
        if (zpass_trace())
            fprintf(stderr, "[ZPASS] F%u q%u prog %08X surf %08X zeta %08X nidx %u depth %u %X %u st %u %X %X mask %X"
                    " cm %08X at %u %X %X clip %u,%u,%u,%u\n", nv2a_pb_presented_frames(), q, prog,
                    st->color_va, st->zeta_key, nidx, st->depth_test, st->depth_func, st->depth_mask,
                    st->st_test, st->st_func, st->st_ref, st->st_mask, st->color_mask, st->alpha_test,
                    st->alpha_func, st->alpha_ref, st->clip_x, st->clip_y, st->clip_w, st->clip_h);
        vkCmdBeginQuery(V.dcb, V.qpool, q, V.precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        vkCmdDrawIndexed(V.dcb, nidx, 1, 0, 0, 0);
        vkCmdEndQuery(V.dcb, V.qpool, q);
        if (zpass_trace() && getenv("RECOMP_VK_ZPASS_PROBE") && st->st_test && st->st_func == 0x202) {
            /* Which stencil values lie under this test: the same draw
             * against each, counted in a pool of its own. */
            static const uint32_t refs[6] = { 0x00, 0x01, 0x14, 0xC8, 0xC9, 0xFF };
            static VkQueryPool dq;
            uint64_t res[6];
            int k;
            if (!dq) {
                VkQueryPoolCreateInfo qci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                qci.queryType = VK_QUERY_TYPE_OCCLUSION;
                qci.queryCount = 6;
                vkCreateQueryPool(V.dev, &qci, NULL, &dq);
            }
            end_rendering();
            vkCmdResetQueryPool(V.dcb, dq, 0, 6);
            begin_target(st);
            vkCmdBindPipeline(V.dcb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            {
                uint32_t doff[2] = { (uint32_t)uoff, (uint32_t)coff };
                vkCmdBindDescriptorSets(V.dcb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl, 0, 1, &set, 2, doff);
            }
            vkCmdBindVertexBuffers(V.dcb, 0, 1, &V.vtx.buf, &voff);
            vkCmdBindIndexBuffer(V.dcb, V.idx.buf, ioff, VK_INDEX_TYPE_UINT32);
            vkCmdSetViewport(V.dcb, 0, 1, &vpt);
            vkCmdSetScissor(V.dcb, 0, 1, &sc);
            for (k = 0; k < 6; k++) {
                vkCmdSetStencilReference(V.dcb, VK_STENCIL_FACE_FRONT_AND_BACK, refs[k]);
                vkCmdBeginQuery(V.dcb, dq, (uint32_t)k, 0);
                vkCmdDrawIndexed(V.dcb, nidx, 1, 0, 0, 0);
                vkCmdEndQuery(V.dcb, dq, (uint32_t)k);
            }
            vkCmdSetStencilReference(V.dcb, VK_STENCIL_FACE_FRONT_AND_BACK, st->st_ref & 0xFF);
            {
                /* The test quad itself, made visible (colour writes on,
                 * no stencil), to see where it lies. */
                Nv2aVkState vis = *st;
                VkPipeline vp2;
                vis.color_mask = 0x01010101u;
                vis.st_test = 0;
                vp2 = pipeline(V.cur_cs ? V.cur_cs->fmt : VK_FORMAT_UNDEFINED, V.cur_zs != NULL,
                               &vis, vs, prog, ninputs);
                if (vp2) {
                    vkCmdBindPipeline(V.dcb, VK_PIPELINE_BIND_POINT_GRAPHICS, vp2);
                    vkCmdSetStencilTestEnable(V.dcb, VK_FALSE);
                    vkCmdDrawIndexed(V.dcb, nidx, 1, 0, 0, 0);
                    mark_drawn(st);
                }
            }
            {
                /* And the stencil buffer itself, as a PGM. */
                uint32_t w = 640 * V.sx, h = 480 * V.scale;
                VkDeviceSize off;
                uint8_t *px = (uint8_t *)ring_alloc(&V.rb, (VkDeviceSize)w * h, 16, &off);
                Zeta *zz = V.cur_zs;
                if (px && zz) {
                    VkBufferImageCopy c;
                    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                    end_rendering();
                    mb.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
                    memset(&c, 0, sizeof c);
                    c.bufferOffset = off;
                    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
                    c.imageSubresource.layerCount = 1;
                    c.imageExtent.width = w;
                    c.imageExtent.height = h;
                    c.imageExtent.depth = 1;
                    vkCmdCopyImageToBuffer(V.dcb, zz->img, VK_IMAGE_LAYOUT_GENERAL, V.rb.buf, 1, &c);
                    vk_sync(0);
                    {
                        char path[64];
                        FILE *f;
                        snprintf(path, sizeof path, "stencil_F%u.pgm", nv2a_pb_presented_frames());
                        f = fopen(path, "wb");
                        if (f) {
                            fprintf(f, "P5 %u %u 255\n", w, h);
                            fwrite(px, 1, (size_t)w * h, f);
                            fclose(f);
                        }
                    }
                }
            }
            vk_sync(0);
            if (vkGetQueryPoolResults(V.dev, dq, 0, 6, sizeof res, res, 8,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
                fprintf(stderr, "[ZPASS] stencil under the test: 00:%llu 01:%llu 14:%llu C8:%llu C9:%llu FF:%llu\n",
                        (unsigned long long)res[0], (unsigned long long)res[1], (unsigned long long)res[2],
                        (unsigned long long)res[3], (unsigned long long)res[4], (unsigned long long)res[5]);
            return;
        }
    } else if (s_flight > 0 && s_dlog && V.fqpool && V.nfq < VK_MAX_QUERIES) {
        /* Flight recorder: how many samples this draw actually wrote. */
        uint32_t q = V.nfq++;
        V.fqsamples[q] = (uint8_t)V.cur_samples;
        V.fq_frame[q] = nv2a_pb_presented_frames();
        V.fq_seq[q] = s_dlog_seq - 1;
        vkCmdBeginQuery(V.dcb, V.fqpool, q, V.precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        vkCmdDrawIndexed(V.dcb, nidx, 1, 0, 0, 0);
        vkCmdEndQuery(V.dcb, V.fqpool, q);
    } else {
        vkCmdDrawIndexed(V.dcb, nidx, 1, 0, 0, 0);
    }
    if (V.cur_cs && V.cur_cs->w >= 640 && !V.cur_cs->swz && s_probe_nq < 4096
        && probe_active()) {
        VkDeviceSize off;
        Surf *cs = V.cur_cs;
        if (ring_alloc(&V.rb, 16, 16, &off)) {
            VkBufferImageCopy c;
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            end_rendering();
            mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            memset(&c, 0, sizeof c);
            c.bufferOffset = off;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageOffset.x = (int32_t)((uint32_t)s_probe_x * V.sx);
            c.imageOffset.y = (int32_t)((uint32_t)s_probe_y * V.scale);
            c.imageExtent.width = c.imageExtent.height = c.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(V.dcb, cs->img, VK_IMAGE_LAYOUT_GENERAL, V.rb.buf, 1, &c);
            s_probe_q[s_probe_nq].off = off;
            s_probe_q[s_probe_nq].seq = s_dlog_seq;
            s_probe_q[s_probe_nq].prog = prog;
            s_probe_q[s_probe_nq].f = nv2a_pb_presented_frames();
            s_probe_q[s_probe_nq].bpp = (int)cs->bpp;
            s_probe_nq++;
        }
    }
    if (s_aw_va >= 0 && V.cur_cs && V.cur_cs->swz && V.cur_cs->va >= (uint32_t)s_aw_va
        && V.cur_cs->va < (uint32_t)s_aw_va + s_aw_n * 4 && (st->color_mask & 0x01000000u)) {
        static unsigned shown;
        if (shown++ < 100)
            fprintf(stderr, "[VKAW] F%u alpha-writing draw prog %08X into %08X cm %08X blend %u %X %X\n",
                    nv2a_pb_presented_frames(), prog, V.cur_cs->va, st->color_mask,
                    st->blend_enable, st->blend_sfactor, st->blend_dfactor);
    }
    mark_drawn(st);
    V.st_draws++;
    V.seg_draws++;
    V.st_tris += nidx / 3;
}

void nv2a_vk_draw(const Nv2aVkState *st, const Nv2aVshOutput *v, uint32_t nv,
                  const uint32_t *idx, uint32_t nidx)
{
    if (!nidx || !nv || !nv2a_vk_active())
        return;
    draw_common(st, V.vs, 0, sizeof *v / 16, v, nv, idx, nidx);
}

/* ---- title vertex programs on the GPU ----------------------------------- */

static const char k_prog_head[] =
    "#version 450\n"
    "layout(set = 0, binding = 0, std140) uniform Nv2aDraw {\n"
    "    vec4 vp; uvec4 misc; uvec4 misc2; vec4 fogc; vec4 fogp;\n"
    "    uvec4 cw[8]; vec4 f0[8]; vec4 f1[8]; vec4 fc0; vec4 fc1;\n"
    "    vec4 ts[4]; uvec4 misc3; vec4 pts; vec4 zoff;\n"
    "} u;\n"
    "layout(set = 0, binding = 3, std140) uniform Nv2aConst { vec4 c[192]; } k;\n"
    "layout(location = 0) out vec4 v_d0;\n"
    "layout(location = 1) out vec4 v_d1;\n"
    "layout(location = 2) out float v_fog;\n"
    "layout(location = 3) out vec4 v_t0;\n"
    "layout(location = 4) out vec4 v_t1;\n"
    "layout(location = 5) out vec4 v_t2;\n"
    "layout(location = 6) out vec4 v_t3;\n"
    "layout(location = 7) noperspective out float v_zs;\n"
    "const float WREV_C = 1.0 / 1024.0;\n"
    "#define INF uintBitsToFloat(0x7F800000u)\n"
    "float rcc(float x) {\n"
    "    float f = 1.0 / x;\n"
    "    if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
    "    if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
    "    return f;\n"
    "}\n"
    "vec4 vexp(float x) { float fl = floor(x); return vec4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
    "vec4 vlog(float x) {\n"
    "    float ax = abs(x);\n"
    "    if (ax == 0.0) return vec4(-INF, 1.0, -INF, 1.0);\n"
    "    float e = floor(log2(ax));\n"
    "    return vec4(e, ax / exp2(e), log2(ax), 1.0);\n"
    "}\n"
    "vec4 vlit(vec4 c) {\n"
    "    float nl = max(c.x, 0.0), nh = max(c.y, 0.0);\n"
    "    float p = clamp(c.w, -127.9961, 127.9961);\n"
    "    return vec4(1.0, nl, c.x > 0.0 ? pow(nh, p) : 0.0, 1.0);\n"
    "}\n"
    "float fog_factor(float d) {\n"
    "    float px = u.fogp.x, py = u.fogp.y, f;\n"
    "    uint mode = uint(u.fogp.z);\n"
    "    if (u.fogc.a == 0.0) return 1.0;\n"
    "    if (mode == 0x800u || mode == 0x802u) f = px + exp2(d * py * 16.0) - 1.5;\n"
    "    else if (mode == 0x801u || mode == 0x803u) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
    "    else f = px + d * py - 1.0;\n"
    "    if (mode == 0x802u || mode == 0x803u || mode == 0x804u) f = abs(f);\n"
    "    if (isnan(f)) f = 1.0;\n"
    "    return clamp(f, 0.0, 1.0);\n"
    "}\n";

static const char k_prog_main[] =
    "void main() {\n"
    "    vec4 R[13];\n"
    "    vec4 O[13];\n"
    "    for (int i = 0; i < 13; i++) { R[i] = vec4(0.0); O[i] = vec4(0.0); }\n"
    /* All outputs start (0,0,0,1), oPos (R12) too -- see nv2a_vsh_interp.c. */
    "    R[12].w = 1.0; O[0].w = 1.0; O[5].w = 1.0; O[6].w = 1.0;\n"
    "    O[3].w = 1.0; O[4].w = 1.0;\n"
    "    O[9].w = 1.0; O[10].w = 1.0; O[11].w = 1.0; O[12].w = 1.0;\n"
    "    vec4 A = vec4(0.0), B = vec4(0.0), C = vec4(0.0), M = vec4(0.0), I = vec4(0.0);\n"
    "    int a0 = 0;\n";

static const char k_prog_tail[] =
    "    float w = R[12].w;\n"
    "    vec2 ndc = R[12].xy * u.vp.xy - 1.0;\n"
    "    gl_Position = vec4(ndc * w, u.vp.w == 2.0 ? WREV_C : R[12].z * u.vp.z * w, w);\n"
    "    v_zs = R[12].z * u.vp.z;\n"
    /* oD0/oD1 are clamped to [0,1] by the hardware before interpolation
     * (as in D3D); unclamped, a light term past 1 went through the
     * combiners' expand-normal mapping and lit panels of the car white. */
    "    v_d0 = clamp(O[3], 0.0, 1.0); v_d1 = clamp(O[4], 0.0, 1.0);\n"
    "    v_fog = fog_factor(O[5].x);\n"
    "    v_t0 = O[9]; v_t1 = O[10]; v_t2 = O[11]; v_t3 = O[12];\n"
    /* Points: the program's oPts when point parameters are on, else
     * SET_POINT_SIZE -- either at the render scale. */
    "    gl_PointSize = max(u.pts.w != 0.0 ? O[6].x * u.pts.y : u.pts.x, 1.0);\n"
    "}\n";

/* Bump when the translation or the frame around it changes: cached SPIR-V
 * from an older translator is then ignored. */
#define VK_PROG_VERSION 7

/* GLSL to SPIR-V at runtime. glslang's C interface: the reference
 * compiler, packaged on every desktop platform and in the Android NDK.
 * Without it, only programs already in the disk cache run on the GPU. */
static int glsl_to_spirv(const char *src, uint32_t hash, int frag, uint32_t **words, size_t *bytes)
{
#if defined(XBOXRECOMP_HAVE_GLSLANG)
    static int inited;
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *pr;
    int ok = 0;
    if (!inited) {
        glslang_initialize_process();
        inited = 1;
    }
    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = frag ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_3;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_6;
    in.code = src;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();
    sh = glslang_shader_create(&in);
    if (!sh)
        return 0;
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        fprintf(stderr, "  [VK] program %08X did not compile: %s\n", hash,
                glslang_shader_get_info_log(sh));
        glslang_shader_delete(sh);
        return 0;
    }
    pr = glslang_program_create();
    glslang_program_add_shader(pr, sh);
    if (glslang_program_link(pr, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        size_t n;
        glslang_program_SPIRV_generate(pr, frag ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX);
        n = glslang_program_SPIRV_get_size(pr);
        *words = (uint32_t *)malloc(n * 4);
        if (*words) {
            glslang_program_SPIRV_get(pr, *words);
            *bytes = n * 4;
            ok = 1;
        }
    } else {
        fprintf(stderr, "  [VK] program %08X did not link: %s\n", hash,
                glslang_program_get_info_log(pr));
    }
    glslang_program_delete(pr);
    glslang_shader_delete(sh);
    return ok;
#else
    (void)src; (void)hash; (void)words; (void)bytes;
    return 0;
#endif
}

static int prog_compile(Prog *p)
{
    static char body[256 * 1024];
    char *src;
    size_t n;
    uint32_t inmask = 0, i, j, *spv = NULL;
    size_t spv_bytes = 0;
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };

    if (!nv2a_vsh_to_glsl(body, sizeof body, &inmask))
        return 0;
    {
        /* Compiled before? The file is keyed by the program's hash and the
         * translator's version, and holds its input list then SPIR-V. */
        const char *dir = cache_dir();
        char path[1100];
        size_t n = 0;
        uint8_t *f = NULL;
        if (dir) {
            snprintf(path, sizeof path, "%s/prog_v%u_%08X.spv", dir, VK_PROG_VERSION, p->hash);
            f = (uint8_t *)file_read(path, &n);
        }
        if (f && n > 24 && !memcmp(f, "NVP1", 4) && ((n - 24) & 3) == 0) {
            memcpy(&p->ninputs, f + 4, 4);
            memcpy(p->inputs, f + 8, 16);
            ci.codeSize = n - 24;
            ci.pCode = (const uint32_t *)(f + 24);
            if (p->ninputs <= 16
                && vkCreateShaderModule(V.dev, &ci, NULL, &p->vs) == VK_SUCCESS) {
                free(f);
                return 1;
            }
        }
        free(f);
    }
    n = sizeof k_prog_head + sizeof k_prog_main + sizeof k_prog_tail + strlen(body) + 2048;
    src = (char *)malloc(n);
    if (!src)
        return 0;
    {
        char *q = src;
        q += sprintf(q, "%s", k_prog_head);
        for (i = 0, j = 0; i < 16; i++)
            if (inmask & (1u << i)) {
                q += sprintf(q, "layout(location = %u) in vec4 v%u;\n", j, i);
                p->inputs[j++] = (uint8_t)i;
            }
        p->ninputs = j;
        q += sprintf(q, "%s%s%s", k_prog_main, body, k_prog_tail);
    }
    if (getenv("RECOMP_VK_PROG_DUMP"))
        fprintf(stderr, "  [VK] program %08X:\n%s\n", p->hash, src);
    if (!glsl_to_spirv(src, p->hash, 0, &spv, &spv_bytes)) {
        free(src);
        return 0;
    }
    ci.codeSize = spv_bytes;
    ci.pCode = spv;
    if (vkCreateShaderModule(V.dev, &ci, NULL, &p->vs) != VK_SUCCESS)
        p->vs = VK_NULL_HANDLE;
    {
        const char *dir = cache_dir();
        if (dir && p->vs) {
            char path[1100];
            uint8_t hdr[24];
            memcpy(hdr, "NVP1", 4);
            memcpy(hdr + 4, &p->ninputs, 4);
            memcpy(hdr + 8, p->inputs, 16);
            snprintf(path, sizeof path, "%s/prog_v%u_%08X.spv", dir, VK_PROG_VERSION, p->hash);
            file_write(path, hdr, sizeof hdr, ci.pCode, ci.codeSize);
        }
    }
    free(spv);
    free(src);
    return p->vs != VK_NULL_HANDLE;
}

static Prog *prog_get(uint32_t hash)
{
    uint32_t slot = (hash * 2654435761u) & (VK_MAX_PROGS - 1), n;
    for (n = 0; n < VK_MAX_PROGS; n++) {
        Prog *p = &V.progs[(slot + n) & (VK_MAX_PROGS - 1)];
        if (p->state == 0) {
            double t0 = now_ms();
            p->hash = hash;
            p->state = prog_compile(p) ? 1 : -1;
            V.st_compile_ms += now_ms() - t0;
            V.st_compiles++;
            return p;
        }
        if (p->hash == hash)
            return p;
    }
    return NULL;
}

int nv2a_vk_program(uint32_t hash, uint8_t inputs[16])
{
    static int off = -1;
    Prog *p;
    if (off < 0)
        off = getenv("RECOMP_VK_CPU_VSH") != NULL;
    if (off || !nv2a_vk_active())
        return -1;
    p = prog_get(hash);
    if (!p || p->state != 1)
        return -1;
    memcpy(inputs, p->inputs, 16);
    return (int)p->ninputs;
}

void nv2a_vk_draw_program(const Nv2aVkState *st, uint32_t hash, const float *vin,
                          uint32_t nv, const uint32_t *idx, uint32_t nidx)
{
    Prog *p;
    if (!nidx || !nv || !nv2a_vk_active())
        return;
    p = prog_get(hash);
    if (!p || p->state != 1)
        return;
    draw_common(st, p->vs, hash, p->ninputs ? p->ninputs : 1, vin, nv, idx, nidx);
}


/* ---- visibility tests --------------------------------------------------- */

void nv2a_vk_zpass(int enable)
{
    V.zpass = enable != 0;
}

/* The title's gamma ramp (D3D SetGammaRamp; see rsc2_overrides.c). On the
 * console the DAC maps every pixel through it on the way to the TV --
 * RalliSport Challenge 2's race ramp lifts the shadows and midtones and warms
 * them, much of its look. Handed to the presenter through a sequence lock;
 * RECOMP_GAMMA_TRACE prints each new one, RECOMP_GAMMA=0 ignores them. */
static volatile uint32_t s_gm_seq;
static uint8_t s_gm_next[3][256];

void nv2a_vk_set_gamma(const uint8_t *r, const uint8_t *g, const uint8_t *b)
{
    static uint8_t last[3][256];
    static int have;
    if (have && !memcmp(last[0], r, 256) && !memcmp(last[1], g, 256) && !memcmp(last[2], b, 256))
        return;
    memcpy(last[0], r, 256);
    memcpy(last[1], g, 256);
    memcpy(last[2], b, 256);
    have = 1;
    if (getenv("RECOMP_GAMMA_TRACE")) {
        int i;
        fprintf(stderr, "[GAMMA] new ramp, every 15th entry r/g/b:");
        for (i = 0; i < 256; i += 15)
            fprintf(stderr, " %u:%u/%u/%u", i, r[i], g[i], b[i]);
        fputc('\n', stderr);
    }
    __atomic_add_fetch(&s_gm_seq, 1, __ATOMIC_ACQ_REL);     /* odd: writing */
    memcpy(s_gm_next, last, sizeof s_gm_next);
    __atomic_add_fetch(&s_gm_seq, 1, __ATOMIC_ACQ_REL);     /* even: done */
}

/* The title's current ramp, for the other ways a frame leaves (the plain
 * window, video recording): 1 when it is not the identity. Any thread. */
int nv2a_gamma_ramp(uint8_t out[3][256])
{
    static int off = -1;
    uint32_t seq;
    int i, c, id = 1;
    if (off < 0) {
        const char *e = getenv("RECOMP_GAMMA");
        off = e && strcmp(e, "0") == 0;
    }
    seq = __atomic_load_n(&s_gm_seq, __ATOMIC_ACQUIRE);
    if (off || !seq || (seq & 1))
        return 0;
    memcpy(out, s_gm_next, 3 * 256);
    if (__atomic_load_n(&s_gm_seq, __ATOMIC_ACQUIRE) != seq)
        return 0;
    for (c = 0; c < 3 && id; c++)
        for (i = 0; i < 256; i++)
            if (out[c][i] != i) {
                id = 0;
                break;
            }
    return !id;
}

/* The newest ramp, if there is one the presenter has not taken. */
static void gamma_poll(void)
{
    static int off = -1;
    uint32_t seq = __atomic_load_n(&s_gm_seq, __ATOMIC_ACQUIRE);
    uint8_t t[3][256];
    int i, c;
    if (off < 0) {
        const char *e = getenv("RECOMP_GAMMA");
        off = e && strcmp(e, "0") == 0;
    }
    if (off || seq == V.gm_seq_seen || (seq & 1))
        return;
    memcpy(t, s_gm_next, sizeof t);
    if (__atomic_load_n(&s_gm_seq, __ATOMIC_ACQUIRE) != seq)
        return;                 /* torn: next frame */
    V.gm_seq_seen = seq;
    memcpy(V.gm, t, sizeof t);
    V.gm_on = 0;
    for (c = 0; c < 3; c++)
        for (i = 0; i < 256; i++)
            if (t[c][i] != i)
                V.gm_on = 1;
    V.gm_lut_dirty = 1;
}

/* The presented region of `s` through the ramp, into gm_img; returns the
 * image to present from (`s->img` when there is no ramp to apply). */
static VkImage gamma_apply(Surf *s, uint32_t w, uint32_t h)
{
    static const char *fs_src =
        "#version 450\n"
        "layout(set = 0, binding = 0) uniform sampler2D src;\n"
        "layout(set = 0, binding = 1) uniform sampler2D lut;\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() {\n"
        "    vec4 c = texelFetch(src, ivec2(gl_FragCoord.xy), 0);\n"
        "    ivec3 i = ivec3(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5);\n"
        "    o = vec4(texelFetch(lut, ivec2(i.r, 0), 0).r, texelFetch(lut, ivec2(i.g, 0), 0).g,\n"
        "             texelFetch(lut, ivec2(i.b, 0), 0).b, c.a);\n"
        "}\n";
    VkDescriptorSet set;
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    float pc[12];

    gamma_poll();
    if (!V.gm_on || !V.post_pl || s->fmt != VK_FORMAT_B8G8R8A8_UNORM)
        return s->img;
    if (!V.gm_pipe) {
        VkShaderModule fs = glsl_module(fs_src, 1);
        V.gm_pipe = fs ? post_pipeline(fs, VK_FORMAT_B8G8R8A8_UNORM, 1, 0) : VK_NULL_HANDLE;
        if (fs)
            vkDestroyShaderModule(V.dev, fs, NULL);
        if (!V.gm_pipe) {
            V.gm_on = 0;
            fprintf(stderr, "  [VK] gamma ramp shader failed -- presenting without it\n");
            return s->img;
        }
    }
    if (!V.gm_lut) {
        if (!image_create(256, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0,
                          VK_IMAGE_ASPECT_COLOR_BIT, &V.gm_lut, &V.gm_lut_mem, &V.gm_lut_view))
            return s->img;
        barrier_image(V.ucb, V.gm_lut, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        V.gm_lut_dirty = 1;
    }
    if (V.gm_lut_dirty) {
        VkDeviceSize off;
        uint8_t *p = (uint8_t *)ring_alloc(&V.stage, 1024, 16, &off);
        VkBufferImageCopy c;
        int i;
        if (!p)
            return s->img;
        for (i = 0; i < 256; i++) {
            p[i * 4 + 0] = V.gm[0][i];
            p[i * 4 + 1] = V.gm[1][i];
            p[i * 4 + 2] = V.gm[2][i];
            p[i * 4 + 3] = 255;
        }
        memset(&c, 0, sizeof c);
        c.bufferOffset = off;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = 256;
        c.imageExtent.height = 1;
        c.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(V.ucb, V.stage.buf, V.gm_lut, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        V.gm_lut_dirty = 0;
    }
    if (!V.gm_img || V.gm_w != w || V.gm_h != h) {
        if (V.gm_img)
            retire(V.gm_img, V.gm_img_mem, V.gm_img_view);
        V.gm_img = VK_NULL_HANDLE;
        if (!image_create(w, h, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0,
                          VK_IMAGE_ASPECT_COLOR_BIT, &V.gm_img, &V.gm_img_mem, &V.gm_img_view))
            return s->img;
        V.gm_w = w;
        V.gm_h = h;
        barrier_image(V.dcb, V.gm_img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    set = post_set(s->view, V.gm_lut_view);
    if (!set)
        return s->img;
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    memset(pc, 0, sizeof pc);
    ao_pass(V.gm_img_view, VK_NULL_HANDLE, w, h, V.gm_pipe, set, pc);
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    V.last_set = VK_NULL_HANDLE;
    return V.gm_img;
}

void nv2a_vk_report_clear(void)
{
    V.rep_acc = 0;
    V.rep_first = V.nq;
    V.rep_cleared = 1;
}

void nv2a_vk_report_get(uint32_t va, uint64_t stamp)
{
    if (V.state <= 0)
        return;
    if (zpass_trace())
        fprintf(stderr, "[ZPASS] F%u report %08X <- q%u..q%u\n", nv2a_pb_presented_frames(),
                va, V.rep_first, V.nq);
    if (V.nreps == VK_MAX_REPORTS)
        vk_sync(0);
    if (V.nreps == VK_MAX_REPORTS)
        return;
    V.reps[V.nreps].va = va;
    V.reps[V.nreps].stamp = stamp;
    V.reps[V.nreps].base = V.rep_acc;
    V.reps[V.nreps].q0 = V.rep_first;
    V.reps[V.nreps].q1 = V.nq;
    V.reps[V.nreps].pre = !V.rep_cleared;
    V.nreps++;
    V.st_reports++;
    /* Nothing recorded yet this segment: answer now rather than at a sync
     * that may be a frame away. */
    if (!V.recording) {
        pend_retire_all();        /* its tail counts towards this one */
        reports_resolve();
    }
}

/* ---- presentation -------------------------------------------------------- */

void *nv2a_vk_instance_handle(void)
{
    return V.state > 0 && V.has_swapchain_ext ? (void *)V.inst : NULL;
}

int nv2a_vk_device_names(char (*names)[256], int max)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst;
    VkPhysicalDevice pds[16];
    uint32_t npd = 16, i;
    int n = 0, pass;

    app.pApplicationName = "xboxrecomp";
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS)
        return 0;
    if (vkEnumeratePhysicalDevices(inst, &npd, pds) < 0)
        npd = 0;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < npd && n < max; i++) {
            VkPhysicalDeviceProperties p;
            int discrete;
            vkGetPhysicalDeviceProperties(pds[i], &p);
            discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (p.apiVersion < VK_API_VERSION_1_3 || discrete != (pass == 0)
                    || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
                continue;
            snprintf(names[n++], 256, "%s", p.deviceName);
        }
    vkDestroyInstance(inst, NULL);
    return n;
}

void nv2a_vk_set_surface(uint64_t surface)
{
    V.surface_handle = surface;
}

/* The Win32 window (fb_present.c's GDI window) as a Vulkan surface. */
int nv2a_vk_attach_hwnd(void *hinstance, void *hwnd)
{
#if defined(_WIN32)
    VkWin32SurfaceCreateInfoKHR ci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    VkSurfaceKHR surf;
    if (V.state <= 0)
        return 0;
    ci.hinstance = (HINSTANCE)hinstance;
    ci.hwnd = (HWND)hwnd;
    if (vkCreateWin32SurfaceKHR(V.inst, &ci, NULL, &surf) != VK_SUCCESS)
        return 0;
    V.surface_handle = (uint64_t)(uintptr_t)surf;
    return 1;
#else
    (void)hinstance; (void)hwnd;
    return 0;
#endif
}

void nv2a_vk_set_window_size(int w, int h)
{
    V.win_w = w;
    V.win_h = h;
}

static void swapchain_destroy(void)
{
    if (V.swapchain) {
        vkDeviceWaitIdle(V.dev);
        vkDestroySwapchainKHR(V.dev, V.swapchain, NULL);
        V.swapchain = VK_NULL_HANDLE;
    }
}

static int swapchain_create(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[64];
    VkPresentModeKHR modes[16];
    uint32_t nf = 64, nm = 16, i;
    VkBool32 ok = VK_FALSE;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainKHR old = V.swapchain;
    const char *vs = getenv("RECOMP_VK_VSYNC");

    vkGetPhysicalDeviceSurfaceSupportKHR(V.pd, V.qf, V.surface, &ok);
    if (!ok) {
        fprintf(stderr, "  [VK] this GPU cannot present to the window (PRIME? try prime-run)\n");
        return 0;
    }
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.pd, V.surface, &caps);
    vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, V.surface, &nf, fmts);
    vkGetPhysicalDeviceSurfacePresentModesKHR(V.pd, V.surface, &nm, modes);
    V.sc_fmt = fmts[0].format;
    for (i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM
            || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            V.sc_fmt = fmts[i].format;
            ci.imageColorSpace = fmts[i].colorSpace;
            break;
        }
    if (i == nf)
        ci.imageColorSpace = fmts[0].colorSpace;
    /* The flip is paced to 60 Hz already: present without blocking on the
     * display unless RECOMP_VK_VSYNC=1 asks for FIFO. */
    if (!(vs && strcmp(vs, "1") == 0))
        for (i = 0; i < nm; i++) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) { mode = modes[i]; break; }
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
        }
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        V.sc_ext = caps.currentExtent;
    } else {
        V.sc_ext.width = V.win_w > 0 ? (uint32_t)V.win_w : 1280;
        V.sc_ext.height = V.win_h > 0 ? (uint32_t)V.win_h : 960;
    }
    if (!V.sc_ext.width || !V.sc_ext.height)
        return 0;                              /* minimised */
    ci.surface = V.surface;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = V.sc_fmt;
    ci.imageExtent = V.sc_ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                  | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                      : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    if (old)
        vkDeviceWaitIdle(V.dev);
    if (vkCreateSwapchainKHR(V.dev, &ci, NULL, &V.swapchain) != VK_SUCCESS) {
        V.swapchain = VK_NULL_HANDLE;
        if (old)
            vkDestroySwapchainKHR(V.dev, old, NULL);
        return 0;
    }
    if (old)
        vkDestroySwapchainKHR(V.dev, old, NULL);
    V.sc_n = 8;
    vkGetSwapchainImagesKHR(V.dev, V.swapchain, &V.sc_n, V.sc_img);
    V.sc_dead = 0;
    fprintf(stderr, "  [VK] presenting %ux%u, format %d, mode %d\n", V.sc_ext.width,
            V.sc_ext.height, (int)V.sc_fmt, (int)mode);
    return 1;
}

/* RECOMP_VK_PRESENT_DUMP=<file>: the next presented image, as a BMP, read
 * back from the swapchain -- what the window shows, scale included. */
static void present_dump(void)
{
    static int left = -1;
    const char *path = getenv("RECOMP_VK_PRESENT_DUMP");
    VkDeviceSize off;
    uint8_t *px;
    VkBufferImageCopy c;
    if (!path)
        return;
    if (left < 0) {
        const char *e = getenv("RECOMP_VK_PRESENT_DUMP_AT");
        left = e ? atoi(e) : 600;
    }
    if (left-- != 0)
        return;
    px = (uint8_t *)ring_alloc(&V.rb, (VkDeviceSize)V.sc_ext.width * V.sc_ext.height * 4, 16, &off);
    if (!px)
        return;
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = V.sc_ext.width;
    c.imageExtent.height = V.sc_ext.height;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(V.dcb, V.sc_img[V.sc_idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           V.rb.buf, 1, &c);
    {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }
    V.present_dump_px = px;
    V.present_dump_path = path;
    V.dump_w = V.sc_ext.width;
    V.dump_h = V.sc_ext.height;
    V.dump_bgr = V.sc_fmt == VK_FORMAT_B8G8R8A8_UNORM || V.sc_fmt == VK_FORMAT_B8G8R8A8_SRGB;
}

/* RECOMP_VK_HIRES_DUMP=<file>:<frame>: the frame the title presents as
 * <frame>, at the render scale, as a BMP -- what the window is fed, before
 * it is fitted, and without needing a window (headless runs). */
static void hires_dump(uint32_t va, uint32_t pitch)
{
    static long want = -2;
    static char path[512];
    VkBufferImageCopy c;
    VkDeviceSize off;
    uint8_t *px;
    Surf *s = NULL;
    uint32_t rows;
    int i;

    if (want == -2) {
        const char *e = getenv("RECOMP_VK_HIRES_DUMP"), *colon;
        want = -1;
        if (e && (colon = strrchr(e, ':')) != NULL && (size_t)(colon - e) < sizeof path) {
            memcpy(path, e, (size_t)(colon - e));
            path[colon - e] = 0;
            want = atol(colon + 1);
        }
    }
    if (want < 0 || nv2a_pb_presented_frames() != (uint32_t)want || V.present_dump_px)
        return;
    for (i = 0; i < V.nsurf; i++)
        if (V.surf[i].va == va && !V.surf[i].swz && V.surf[i].pitch == pitch) {
            s = &V.surf[i];
            break;
        }
    if (!s)
        return;
    want = -1;
    rows = s->rows ? s->rows : s->h;
    px = (uint8_t *)ring_alloc(&V.rb, (VkDeviceSize)s->w * V.sx * rows * V.scale * 4, 16, &off);
    if (!px)
        return;
    segment_begin();
    end_rendering();
    {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = s->w * V.sx;
    c.imageExtent.height = rows * V.scale;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(V.dcb, s->img, VK_IMAGE_LAYOUT_GENERAL, V.rb.buf, 1, &c);
    {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
    }
    V.present_dump_px = px;
    V.present_dump_path = path;
    V.dump_w = s->w * V.sx;
    V.dump_h = rows * V.scale;
    V.dump_bgr = s->fmt == VK_FORMAT_B8G8R8A8_UNORM || s->fmt == VK_FORMAT_B8G8R8A8_SRGB;
    gamma_poll();
    V.dump_gamma = V.gm_on;
}

/* ---- FPS counter --------------------------------------------------------- */

/* RECOMP_FPS_OVERLAY=1 (or F3 in the window): frames per second and frame
 * times in a corner of the window -- the average, the fastest and slowest
 * frame of the last second, and a graph of the last 160 frames against the
 * frame-rate limit. Frame time is the time between presents. Drawn on the
 * CPU with a built-in 5x7 font and blitted into the window image after the
 * title's frame, so neither the title, MSAA nor the gamma ramp touch it. */

#define OV_W 168
#define OV_H 64
#define OV_N 256

static volatile int s_ov_on = -1;
static double s_ov_dt[OV_N];            /* ms, ring */
static uint32_t s_ov_i, s_ov_count;
static double s_ov_last, s_ov_shown_at;
static char s_ov_text[3][32];
static uint8_t s_ov_px[OV_W * OV_H * 4];

void nv2a_vk_overlay_toggle(void)
{
    s_ov_on = s_ov_on > 0 ? 0 : 1;
}

/* ' ', '.', 0-9, then F P S m s a v g i n x: 5x7, one byte per row. */
static const char k_ov_chars[] = " .0123456789FPSmsavginx";
static const uint8_t k_ov_font[][7] = {
    {0,0,0,0,0,0,0}, {0,0,0,0,0,0x0C,0x0C},
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0,0,0x1A,0x15,0x15,0x11,0x11},
    {0,0,0x0E,0x10,0x0E,0x01,0x1E}, {0,0,0x0E,0x01,0x0F,0x11,0x0F},
    {0,0,0x11,0x11,0x11,0x0A,0x04}, {0,0,0x0F,0x11,0x0F,0x01,0x0E},
    {0x04,0,0x0C,0x04,0x04,0x04,0x0E}, {0,0,0x16,0x19,0x11,0x11,0x11},
    {0,0,0x11,0x0A,0x04,0x0A,0x11},
};

static void ov_fill(int x, int y, int w, int h, uint32_t rgb)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) {
            uint8_t *p;
            if (i < 0 || j < 0 || i >= OV_W || j >= OV_H)
                continue;
            p = s_ov_px + ((size_t)j * OV_W + i) * 4;
            p[0] = (uint8_t)(rgb >> 16);
            p[1] = (uint8_t)(rgb >> 8);
            p[2] = (uint8_t)rgb;
            p[3] = 255;
        }
}

static void ov_text(int x, int y, const char *t, uint32_t rgb)
{
    for (; *t; t++, x += 6) {
        const char *c = strchr(k_ov_chars, *t);
        int r, b;
        if (!c)
            continue;
        for (r = 0; r < 7; r++)
            for (b = 0; b < 5; b++)
                if (k_ov_font[c - k_ov_chars][r] & (0x10 >> b))
                    ov_fill(x + b, y + r, 1, 1, rgb);
    }
}

/* Called once per present: time this frame, and draw the counter. */
static void overlay_frame(uint32_t ox, uint32_t oy)
{
    double now = now_ms(), target;
    VkDeviceSize off;
    uint8_t *stage;
    uint32_t k, n, i;
    int hz;

    if (s_ov_on < 0) {
        const char *e = getenv("RECOMP_FPS_OVERLAY");
        s_ov_on = e && *e && strcmp(e, "0") != 0;
    }
    if (s_ov_last > 0.0) {
        s_ov_dt[s_ov_i] = now - s_ov_last;
        s_ov_i = (s_ov_i + 1) % OV_N;
        if (s_ov_count < OV_N)
            s_ov_count++;
    }
    s_ov_last = now;
    if (s_ov_on <= 0 || !s_ov_count)
        return;
    {
        const char *e = getenv("RECOMP_FLIP_HZ");
        hz = e ? atoi(e) : 60;
        target = 1000.0 / (hz > 0 ? hz : 60);
    }
    /* The numbers: the last second's frames, refreshed four times a second
     * so they can be read. */
    if (now - s_ov_shown_at >= 250.0) {
        double sum = 0.0, lo = 1e9, hi = 0.0;
        n = 0;
        for (i = 0; i < s_ov_count && sum < 1000.0; i++) {
            double d = s_ov_dt[(s_ov_i + OV_N - 1 - i) % OV_N];
            sum += d;
            if (d < lo) lo = d;
            if (d > hi) hi = d;
            n++;
        }
        snprintf(s_ov_text[0], sizeof s_ov_text[0], "%.1f FPS", n && sum > 0.0 ? n * 1000.0 / sum : 0.0);
        snprintf(s_ov_text[1], sizeof s_ov_text[1], "%.2f ms avg", n ? sum / n : 0.0);
        snprintf(s_ov_text[2], sizeof s_ov_text[2], "min %.1f max %.1f", lo < 1e9 ? lo : 0.0, hi);
        s_ov_shown_at = now;
    }
    ov_fill(0, 0, OV_W, OV_H, 0x101418);
    ov_text(4, 3, s_ov_text[0], 0xFFFFFF);
    ov_text(4, 12, s_ov_text[1], 0xC8D0D8);
    ov_text(4, 21, s_ov_text[2], 0xC8D0D8);
    /* The graph: one column per frame, newest on the right, scaled so twice
     * the frame-rate limit fills it; the line is the limit. */
    {
        const int gx = 4, gy = 31, gw = OV_W - 8, gh = OV_H - 34;
        ov_fill(gx, gy, gw, gh, 0x1C2228);
        for (i = 0; i < (uint32_t)gw && i < s_ov_count; i++) {
            double d = s_ov_dt[(s_ov_i + OV_N - 1 - i) % OV_N];
            int h = (int)(d / (2.0 * target) * gh + 0.5);
            uint32_t col = d <= target * 1.1 ? 0x58D068 : d <= target * 2.0 ? 0xE8C040 : 0xE85848;
            if (h > gh) h = gh;
            if (h < 1) h = 1;
            ov_fill(gx + gw - 1 - (int)i, gy + gh - h, 1, h, col);
        }
        ov_fill(gx, gy + gh / 2, gw, 1, 0x8090A0);
    }

    /* Into the window, scaled up for its size, at the frame's corner. */
    if (!V.ov_img) {
        if (!image_create(OV_W, OV_H, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0,
                          VK_IMAGE_ASPECT_COLOR_BIT, &V.ov_img, &V.ov_mem, &V.ov_view)) {
            V.ov_img = VK_NULL_HANDLE;
            return;
        }
        barrier_image(V.dcb, V.ov_img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    stage = (uint8_t *)ring_alloc(&V.stage, sizeof s_ov_px, 16, &off);
    if (!stage)
        return;
    memcpy(stage, s_ov_px, sizeof s_ov_px);
    {
        VkBufferImageCopy c;
        VkImageBlit b;
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        memset(&c, 0, sizeof c);
        c.bufferOffset = off;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = OV_W;
        c.imageExtent.height = OV_H;
        c.imageExtent.depth = 1;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
        vkCmdCopyBufferToImage(V.dcb, V.stage.buf, V.ov_img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
        k = V.sc_ext.height / 540;
        if (k < 1) k = 1;
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = OV_W;
        b.srcOffsets[1].y = OV_H;
        b.srcOffsets[1].z = 1;
        b.dstOffsets[0].x = (int32_t)(ox + 8 * k);
        b.dstOffsets[0].y = (int32_t)(oy + 8 * k);
        b.dstOffsets[1].x = (int32_t)(ox + 8 * k + OV_W * k);
        b.dstOffsets[1].y = (int32_t)(oy + 8 * k + OV_H * k);
        b.dstOffsets[1].z = 1;
        if ((uint32_t)b.dstOffsets[1].x <= V.sc_ext.width && (uint32_t)b.dstOffsets[1].y <= V.sc_ext.height)
            vkCmdBlitImage(V.dcb, V.ov_img, VK_IMAGE_LAYOUT_GENERAL, V.sc_img[V.sc_idx],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
}

/* Blit the surface the title just finished into the window. */
void nv2a_vk_present(uint32_t va, uint32_t pitch)
{
    Surf *s = NULL;
    VkResult r;
    VkImageBlit b;
    VkClearColorValue black;
    VkImageSubresourceRange rng;
    uint32_t dw, dh, ox, oy, sw, sh;
    int i;

    if (V.state <= 0)
        return;
    V.flip_va[0] = V.flip_va[1];
    V.flip_va[1] = va;
    hires_dump(va, pitch);
    if (!V.has_swapchain_ext)
        return;
    if (!V.surface) {
        if (!V.surface_handle)
            return;
        V.surface = (VkSurfaceKHR)(uintptr_t)V.surface_handle;
    }
    if (V.swapchain && (V.sc_dead
            || (V.win_w > 0 && ((uint32_t)V.win_w != V.sc_ext.width
                                || (uint32_t)V.win_h != V.sc_ext.height)))) {
        if (V.recording)
            vk_sync(0);
        if (!swapchain_create())
            swapchain_destroy();
    }
    if (!V.swapchain) {
        static int failed;
        if (failed > 3)
            return;
        if (V.recording)
            vk_sync(0);
        if (!swapchain_create()) {
            failed++;
            return;
        }
    }
    for (i = 0; i < V.nsurf; i++)
        if (V.surf[i].va == va && !V.surf[i].swz && V.surf[i].pitch == pitch) {
            s = &V.surf[i];
            break;
        }
    if (!s)
        return;
    r = vkAcquireNextImageKHR(V.dev, V.swapchain, 100000000ull, V.sem_acq,
                              VK_NULL_HANDLE, &V.sc_idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        V.sc_dead = 1;
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return;
    segment_begin();
    end_rendering();
    {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    barrier_image(V.dcb, V.sc_img[V.sc_idx], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    memset(&black, 0, sizeof black);
    memset(&rng, 0, sizeof rng);
    rng.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    rng.levelCount = 1;
    rng.layerCount = 1;
    vkCmdClearColorImage(V.dcb, V.sc_img[V.sc_idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &black, 1, &rng);
    {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    /* The title's frame, scaled to fit with its aspect kept: its own
     * shape, or 16:9 for a 4:3-sized frame a widescreen title drew
     * anamorphic (RECOMP_WIDESCREEN). */
    sw = s->w;
    sh = s->rows ? s->rows : s->h;
    {
        uint32_t aw = sw;
        if (fb_widescreen() && sw * 3 == sh * 4)
            aw = sh * 16 / 9;
        if ((uint64_t)V.sc_ext.width * sh <= (uint64_t)V.sc_ext.height * aw) {
            dw = V.sc_ext.width;
            dh = (uint32_t)((uint64_t)V.sc_ext.width * sh / aw);
        } else {
            dh = V.sc_ext.height;
            dw = (uint32_t)((uint64_t)V.sc_ext.height * aw / sh);
        }
    }
    ox = (V.sc_ext.width - dw) / 2;
    oy = (V.sc_ext.height - dh) / 2;
    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
    b.srcOffsets[1].x = (int32_t)(sw * V.sx);
    b.srcOffsets[1].y = (int32_t)(sh * V.scale);
    b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = (int32_t)ox;
    b.dstOffsets[0].y = (int32_t)oy;
    b.dstOffsets[1].x = (int32_t)(ox + dw);
    b.dstOffsets[1].y = (int32_t)(oy + dh);
    b.dstOffsets[1].z = 1;
    {
        /* Through the title's gamma ramp first, as the console's DAC. */
        VkImage src = gamma_apply(s, sw * V.sx, sh * V.scale);
        vkCmdBlitImage(V.dcb, src, VK_IMAGE_LAYOUT_GENERAL, V.sc_img[V.sc_idx],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    {
        /* The counter goes on top of the frame (the blit above must land
         * first: same image, both transfer writes). */
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(V.dcb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        overlay_frame(ox, oy);
    }
    if (getenv("RECOMP_VK_PRESENT_DUMP")) {
        barrier_image(V.dcb, V.sc_img[V.sc_idx], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        present_dump();
        barrier_image(V.dcb, V.sc_img[V.sc_idx], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    } else {
        barrier_image(V.dcb, V.sc_img[V.sc_idx], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    V.present_pending = 1;
}

/* RECOMP_VK_HITCH=<ms>: a frame that took longer, with what the renderer did
 * in it -- new pipelines, compiles, uploads, waiting on the GPU. Nothing of
 * the renderer's means the time went in the title (loading, its own work). */
static uint32_t p_pipes, p_comp, p_tex, p_surf, p_hash;
static double p_pipe_ms, p_comp_ms, p_wait, p_tex_ms;

static void hitch_check(void)
{
    static double limit = -1.0, last;
    double now = now_ms(), dt;
    if (limit < 0.0) {
        const char *e = getenv("RECOMP_VK_HITCH");
        limit = e ? atof(e) : 0.0;
    }
    if (limit <= 0.0)
        return;
    dt = last > 0.0 ? now - last : 0.0;
    last = now;
    if (dt > limit) {
        int k;
        fprintf(stderr, "[HITCH] frame %u (title %u) %.1f ms: pipelines %u (%.1f ms) programs %u (%.1f ms)"
                " tex uploads %u (%.1f ms) hashed %u KB surf uploads %u gpu-wait %.1f ms\n",
                V.frame, nv2a_pb_presented_frames(), dt, V.st_pipes_new - p_pipes, V.st_pipe_ms - p_pipe_ms,
                V.st_compiles - p_comp, V.st_compile_ms - p_comp_ms, V.st_tex_up - p_tex,
                V.st_tex_ms - p_tex_ms, V.st_tex_hash_mb - p_hash, V.st_surf_up - p_surf,
                V.st_wait_ms - p_wait);
        for (k = 0; k < V.nhtex; k++)
            fprintf(stderr, "[HITCH]   tex %s\n", V.htex[k]);
    }
}

/* The counters as of the end of this frame (after the stats reset them). */
static void hitch_snap(void)
{
    p_pipes = V.st_pipes_new;
    p_pipe_ms = V.st_pipe_ms;
    p_comp = V.st_compiles;
    p_comp_ms = V.st_compile_ms;
    p_tex = V.st_tex_up;
    p_hash = V.st_tex_hash_mb;
    p_surf = V.st_surf_up;
    p_wait = V.st_wait_ms;
    p_tex_ms = V.st_tex_ms;
    V.nhtex = 0;
}

void nv2a_vk_flush(void)
{
    static int stats = -1;
    if (V.state <= 0)
        return;
    vk_flip();
    hitch_check();
    V.frame++;
    {
        static uint32_t saved_npipes;
        if (V.npipes != (int)saved_npipes && V.frame % 600 == 0) {
            pipeline_cache_save();
            saved_npipes = (uint32_t)V.npipes;
        }
    }
    if (stats < 0)
        stats = getenv("RECOMP_VK_STATS") != NULL;
    if (stats) {
        if (V.frame % 120 == 0) {
            fprintf(stderr, "  [VK] frame %u (last 120): draws %u tris %u syncs %u tex uploads %u"
                    " hashed %u MB surf uploads %u textures %u pipelines %d gpu-wait %.1f ms/frame"
                    " rt copies %u alias clears %u kicks %u gpu %.2f ms/frame spec %u/%u\n",
                    V.frame, V.st_draws, V.st_tris, V.st_syncs, V.st_tex_up,
                    V.st_tex_hash_mb >> 10, V.st_surf_up, V.ntex, V.npipes, V.st_wait_ms / 120.0,
                    V.st_rt_copies, V.st_alias_clears, V.st_kicks, V.gpu_ms / 120.0,
                    V.st_spec_draws, V.st_spec_draws + V.st_uber_draws);
            V.st_wait_ms = 0;
            V.st_rt_copies = 0;
            V.st_alias_clears = 0;
            V.st_kicks = 0;
            V.gpu_ms = 0;
            V.st_spec_draws = V.st_uber_draws = 0;
            if (V.st_compiles || V.st_pipes_new)
                fprintf(stderr, "  [VK]   programs compiled %u (%.1f ms), pipelines created %u (%.1f ms)\n",
                        V.st_compiles, V.st_compile_ms, V.st_pipes_new, V.st_pipe_ms);
            fprintf(stderr, "  [VK]   visibility: %u reports, %u counted draws\n",
                    V.st_reports, V.st_qdraws);
            V.st_reports = V.st_qdraws = 0;
            if (V.msaa > 1 || V.st_ao)
                fprintf(stderr, "  [VK]   msaa refreshes %u, ao passes %u\n", V.st_ms_refresh, V.st_ao);
            V.st_ms_refresh = V.st_ao = 0;
            V.st_compiles = V.st_pipes_new = 0;
            V.st_compile_ms = V.st_pipe_ms = 0;
            V.st_draws = V.st_tris = V.st_syncs = V.st_tex_up = 0;
            V.st_tex_hash_mb = V.st_surf_up = 0;
        }
    }
    hitch_snap();
}

#endif /* XBOXRECOMP_HAVE_VULKAN */
