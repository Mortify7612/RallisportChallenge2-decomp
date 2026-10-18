/*
 * nv2a_draw.c - the per-draw uniform block, filled the same way for every
 * renderer (see nv2a_draw.h).
 */
#include "nv2a_draw.h"
#include "nv2a_combiner.h"

#include <stdlib.h>
#include <string.h>

int nv2a_draw_stage_samples(const Nv2aVkState *st, int i)
{
    uint32_t mode = st->use_rc ? (st->rc->stage_program >> (i * 5)) & 0x1F
                               : (i == 0 && st->tex[0].valid ? 1 : 0);
    return mode == 1 || mode == 2 || mode == 3 || mode == 6 || mode == 7
        || mode == 9 || (mode >= 11 && mode <= 16) || mode == 18;
}

float nv2a_draw_wide_factor(float view_aspect)
{
    static float force = -1.0f;
    if (force < 0.0f) {
        const char *e = getenv("RECOMP_VK_WIDE_X");
        force = e ? (float)atof(e) : 0.0f;
        if (!(force > 0.1f && force <= 1.0f))
            force = 0.0f;
    }
    return force > 0.0f ? force : (16.0f / 9.0f) / view_aspect;
}

void nv2a_draw_ubo_fill(Nv2aVkUbo *ub, const Nv2aVkState *st, const int stage_on[4],
                        uint32_t w, uint32_t h, int depth_mode, float sx, float scale,
                        float wide)
{
    float zmax = st->zeta_fmt == 1 ? 65535.0f : 16777215.0f;
    int i;

    memset(ub, 0, sizeof *ub);
    ub->vp[0] = 2.0f / (float)w;
    ub->vp[1] = 2.0f / (float)h;
    ub->vp[2] = 1.0f / zmax;
    ub->vp[3] = (float)depth_mode;
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
    ub->pts[0] = st->point_size * 0.5f * (sx + scale);
    ub->pts[1] = 0.5f * (sx + scale);
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
        ub->zoff[2] = sx;      /* host pixels per title pixel */
        ub->zoff[3] = scale;
    }
    ub->wide[0] = wide;
}

/* ---- the frame around a translated title vertex program ----------------
 *
 * GLSL for Vulkan; nv2a_gl.c makes it GL's on the way in. The uniform block
 * here must match Nv2aDraw in shaders/nv2a_common.glsl and Nv2aVkUbo
 * (nv2a_draw.h); bump VK_PROG_VERSION in nv2a_vk.c when any of them change. */

const char nv2a_prog_head[] =
    "#version 450\n"
    "layout(set = 0, binding = 0, std140) uniform Nv2aDraw {\n"
    "    vec4 vp; uvec4 misc; uvec4 misc2; vec4 fogc; vec4 fogp;\n"
    "    uvec4 cw[8]; vec4 f0[8]; vec4 f1[8]; vec4 fc0; vec4 fc1;\n"
    "    vec4 ts[4]; uvec4 misc3; vec4 pts; vec4 zoff; vec4 wide;\n"
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

const char nv2a_prog_main[] =
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

const char nv2a_prog_tail[] =
    "    float w = R[12].w;\n"
    "    vec2 ndc = R[12].xy * u.vp.xy - 1.0;\n"
    /* A screen wider than the title's: everything drawn into the scene is
     * squeezed towards the centre, so the 3D view is widened rather than
     * stretched and the HUD keeps its proportions -- except flat (w = 1)
     * vertices on the screen's edges, which keep full-screen passes
     * (colour grading, fades) covering the whole screen. "On the edge" is
     * within a title pixel of it: D3D's half-pixel offset puts a full-screen
     * quad at -0.5 .. 639.5, just outside on the left and just inside on the
     * right, which a tighter test pinned on one side only. */
    "    if (abs(w - 1.0) > 1e-5 || abs(ndc.x) < 1.0 - u.vp.x * 1.01) ndc.x *= u.wide.x;\n"
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

