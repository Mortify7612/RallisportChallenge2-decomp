/**
 * NV2A on a host GPU, through Vulkan.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes the title's commands and
 * runs its vertex programs; with RECOMP_VULKAN=1 the triangles, clears and
 * per-pixel work go here instead of to the software rasteriser. The host GPU
 * clips in homogeneous space, interpolates perspective-correctly, and runs
 * the register combiners as a shader (shaders/nv2a.frag), so a race frame is
 * a few milliseconds of GPU time rather than most of a second of CPU.
 *
 * Guest RAM stays the source of truth between frames: every surface the GPU
 * drew is copied back at the flip (and before a texture that overlaps it is
 * sampled), so the framebuffer window, the BMP dumps and render-to-texture
 * all keep working unchanged. Textures are decoded on the CPU by the
 * executor's own sampler and cached by content.
 *
 * Off unless RECOMP_VULKAN is set; without Vulkan at build time every entry
 * point is a stub and nv2a_vk_active() is 0.
 */
#ifndef XBOXRECOMP_NV2A_VK_H
#define XBOXRECOMP_NV2A_VK_H

#include <stdint.h>
#include "nv2a_vsh_interp.h"
#include "nv2a_combiner.h"

/* One texture stage, as the executor decoded it. */
typedef struct {
    uint32_t offset;            /* guest VA of level 0, face 0 */
    uint32_t width, height, pitch, color, levels, min_level;
    uint32_t filter, addr_u, addr_v, addr_p, palette;
    uint32_t max_lod;           /* CONTROL0 MAX_LOD_CLAMP, 4.8 fixed point */
    int      cube, valid;
    int      size_from_format;  /* swizzled/DXT: normalised coordinates */
} Nv2aVkTex;

/* Everything a draw needs besides its vertices. */
typedef struct {
    uint32_t color_va;          /* resolved surface address, 0 = none */
    uint32_t vtx_va;            /* vertex attribute 0's address (draw log) */
    uint32_t pitch, bpp, color_fmt;
    uint32_t swz_w, swz_h;      /* swizzled surface: its size; 0 = linear */
    uint32_t zeta_key;          /* zeta offset, 0 = no depth buffer */
    uint32_t zeta_fmt;          /* 1 = Z16, 2 = Z24S8 */
    uint32_t clip_x, clip_y, clip_w, clip_h;
    uint32_t blend_enable, blend_sfactor, blend_dfactor, blend_equation;
    uint32_t blend_color, color_mask;
    uint32_t depth_test, depth_func, depth_mask, wbuffer, zint;
    uint32_t topology;          /* 0 triangles, 1 lines, 2 points */
    float    point_size;        /* SET_POINT_SIZE, pixels */
    uint32_t point_params;      /* SET_POINT_PARAMS_ENABLE: size from oPts */
    uint32_t point_sprite;      /* SET_POINT_SMOOTH_ENABLE: stage 3 = point coord */
    uint32_t st_test, st_func, st_ref, st_mask, st_wmask;
    uint32_t st_fail, st_zfail, st_zpass, st_write;
    uint32_t cull_enable, cull_face, front_face;
    float    poff_factor, poff_units;   /* polygon offset in force, 0 = none */
    const Nv2aCombiner *rc;
    int      use_rc;
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t fog_enable, fog_mode, fog_color;
    float    fog_param[2];
    uint32_t clip_plane_mode;
    uint32_t dot_mapping;       /* SET_DOT_RGBMAPPING            */
    uint32_t other_input;       /* SET_SHADER_OTHER_STAGE_INPUT  */
    Nv2aVkTex tex[4];
} Nv2aVkState;

/* RECOMP_VULKAN set and the device came up. Cheap after the first call. */
int  nv2a_vk_active(void);

/* Triangles: `v` holds `nv` transformed vertices, `idx` a triangle list
 * into them. */
void nv2a_vk_draw(const Nv2aVkState *s, const Nv2aVshOutput *v, uint32_t nv,
                  const uint32_t *idx, uint32_t nidx);

/* The loaded vertex program, compiled for the GPU: returns how many inputs
 * it reads and which (v-register numbers, in order), or -1 when it has to
 * run on the CPU (constant writes, no shader compiler, RECOMP_VK_CPU_VSH). */
int  nv2a_vk_program(uint32_t hash, uint8_t inputs[16]);

/* Draw with that program on the GPU: `vin` holds nv vertices of its inputs
 * (4 floats each, in the order nv2a_vk_program gave). 0 when the program or
 * its pipeline is still being built on the worker: draw it another way. */
int nv2a_vk_draw_program(const Nv2aVkState *s, uint32_t hash, const float *vin,
                         uint32_t nv, const uint32_t *idx, uint32_t nidx);

/* The same draw with the inputs as the title laid them out, for the GPU to
 * read and convert itself (VK_EXT_vertex_input_dynamic_state): the
 * vertices' bytes copied as they are, rather than turned into four floats
 * an input on the CPU. An input whose format Vulkan has no match for (the
 * packed 11:11:10 normal) comes converted, in `conv`; one the title does
 * not supply reads (0, 0, 0, 1). */
enum {
    NV2A_VF_F32 = 0,            /* + size - 1: float, 1 to 4 components */
    NV2A_VF_UN8 = 4,            /* unsigned byte, normalised */
    NV2A_VF_SN16 = 8,           /* signed short, normalised */
    NV2A_VF_SS16 = 12,          /* signed short, as is */
    NV2A_VF_BGRA8 = 16,         /* D3DCOLOR */
    NV2A_VF_COUNT
};
enum { NV2A_VS_RAW, NV2A_VS_CONV, NV2A_VS_NONE };
typedef struct {
    const void *raw;            /* vertex 0's first byte */
    uint32_t raw_bytes, raw_stride;
    const float *conv;          /* `nconv` float4s a vertex */
    uint32_t nconv;
    uint8_t src[16];            /* per input, NV2A_VS_* */
    uint8_t fmt[16];            /* NV2A_VS_RAW: NV2A_VF_* */
    uint16_t off[16];           /* RAW: byte offset in a vertex; CONV: its slot */
} Nv2aVkVerts;

/* 1 when nv2a_vk_draw_program_raw can be used, and for which formats. */
int nv2a_vk_raw_verts(void);
int nv2a_vk_vfmt_ok(int fmt);
int nv2a_vk_draw_program_raw(const Nv2aVkState *s, uint32_t hash, const Nv2aVkVerts *vx,
                             uint32_t nv, const uint32_t *idx, uint32_t nidx);

/* CLEAR_SURFACE: `flags` is its parameter (0x01 Z, 0x02 stencil, 0xF0
 * colour), `argb` the colour clear value, `zs` ZSTENCIL_CLEAR_VALUE. */
void nv2a_vk_clear(const Nv2aVkState *s, uint32_t flags, uint32_t argb,
                   uint32_t zs);

/* Finish everything queued and copy every surface the GPU drew back into
 * guest RAM. Called at the flip. */
void nv2a_vk_flush(void);

/* Visibility tests: SET_ZPASS_PIXEL_COUNT_ENABLE, CLEAR_REPORT_VALUE and
 * GET_REPORT. The report (stamp, pixel count, 0 = done) is written to
 * guest RAM at `va` once the GPU has the answer -- at the next sync. */
void nv2a_vk_zpass(int enable);
void nv2a_vk_report_clear(void);
void nv2a_vk_report_get(uint32_t va, uint64_t stamp);

/* Put the finished surface at `va` in the window (FLIP_STALL, before
 * nv2a_vk_flush). Does nothing until the window thread has handed over a
 * surface with nv2a_vk_set_surface. */
void nv2a_vk_present(uint32_t va, uint32_t pitch);

/* After nv2a_vk_flush and the flip pacing: hand the frame to the screen. */
void nv2a_vk_present_queued(void);

/* The shape the 3D view is shown at: 16:9 for a widescreen title, or the
 * screen's own when it is wider. A title that culls by its own field of
 * view widens that to match (RSC2: rsc2_overrides.c). */
float nv2a_vk_view_aspect(void);

/* For the window thread (fb_present.c), without Vulkan types: the instance
 * to make a surface with (NULL when the renderer is not up or cannot
 * present), the surface it made, and the window's drawable size. */
void *nv2a_vk_instance_handle(void);

/* The GPUs the renderer could use (Vulkan 1.3), for a settings screen:
 * fills up to `max` names, discrete GPUs first, and returns how many. Makes
 * and drops its own instance, so it works before the renderer is up. */
int   nv2a_vk_device_names(char (*names)[256], int max);
void  nv2a_vk_set_surface(uint64_t surface);
void  nv2a_vk_set_window_size(int w, int h);
int   nv2a_vk_attach_hwnd(void *hinstance, void *hwnd);   /* Win32 window */
/* Android: a surface for SDL's ANativeWindow, from the renderer's loader. */
int   nv2a_vk_create_android_surface(void *anw, uint64_t *surface);
/* A Wayland window's wl_display and wl_surface (SDL's): HDR frames can then
 * be handed over in the compositor's own colour space (wl_color.h). */
void  nv2a_vk_set_wayland(void *wl_display, void *wl_surface);

/* Implemented by nv2a_pb_exec.c, for the texture cache: one decoded level
 * of one face as A8R8G8B8 (w*h texels), and the bytes the whole texture
 * occupies in guest RAM (every face and level), for its content hash. */
void     nv2a_pb_tex_decode(const Nv2aVkTex *t, uint32_t face, uint32_t level,
                            uint32_t w, uint32_t h, uint32_t *dst);
uint32_t nv2a_pb_tex_bytes(const Nv2aVkTex *t);

#endif
