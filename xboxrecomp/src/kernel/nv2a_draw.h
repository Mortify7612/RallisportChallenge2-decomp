/*
 * nv2a_draw.h - what a draw hands the shaders, independent of the graphics
 * API: the per-draw uniform block both renderers bind (nv2a_vk.c, Vulkan;
 * nv2a_gl.c, OpenGL) and the code that fills it from the executor's state.
 *
 * The block's layout is mirrored by Nv2aDraw in shaders/nv2a_common.glsl and
 * k_prog_head in nv2a_vk.c (std140; every member 16-byte aligned).
 */
#ifndef XBOXRECOMP_NV2A_DRAW_H
#define XBOXRECOMP_NV2A_DRAW_H

#include <stdint.h>
#include "nv2a_vk.h"

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
    float wide[4];       /* x: horizontal squeeze of perspective draws into
                          * the scene (wider screens than the title's), 1 off */
} Nv2aVkUbo;

/* Whether texture stage i's mode samples its texture: the projective ones,
 * cube, bump-env, dot-ST/reflect/STR and the dependent reads. */
int nv2a_draw_stage_samples(const Nv2aVkState *st, int i);

/* The squeeze of perspective scene draws on a screen wider than 16:9, for
 * the view aspect given (RECOMP_VK_WIDE_X=<k> forces it); 1 = none. */
float nv2a_draw_wide_factor(float view_aspect);

/* Fill the block for a draw into a w x h (title pixels) target. stage_on:
 * which stages have a texture bound; depth_mode: 0 z-buffer, 1 w-buffer as
 * w/zmax, 2 reversed w-buffer; sx, scale: host pixels per title pixel
 * across and down; wide: the draw's squeeze (nv2a_draw_wide_factor, or 1
 * for a draw that is not into the scene). */
void nv2a_draw_ubo_fill(Nv2aVkUbo *ub, const Nv2aVkState *st, const int stage_on[4],
                        uint32_t w, uint32_t h, int depth_mode, float sx, float scale,
                        float wide);

/* A translated title vertex program is nv2a_prog_head, its input
 * declarations, nv2a_prog_main, nv2a_vsh_to_glsl's body and nv2a_prog_tail. */
extern const char nv2a_prog_head[], nv2a_prog_main[], nv2a_prog_tail[];

#endif
