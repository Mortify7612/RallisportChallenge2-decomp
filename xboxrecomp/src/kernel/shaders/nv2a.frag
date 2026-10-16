#version 450
#extension GL_GOOGLE_include_directive : require
/* The NV2A pixel pipeline: four texture stages, the register combiners, the
 * alpha test and the depth value. A port of nv2a_combiner.c's nv2a_rc_eval
 * and nv2a_pb_exec.c's rc_stage_fetch -- the software rasteriser is the
 * reference, and every rule here has its comment there. */
#include "nv2a_common.glsl"

layout(set = 0, binding = 1) uniform sampler2D t2d[4];
layout(set = 0, binding = 2) uniform samplerCube tcube[4];

layout(location = 0) in vec4 v_d0;
layout(location = 1) in vec4 v_d1;
layout(location = 2) in float v_fog;
layout(location = 3) in vec4 v_t0;
layout(location = 4) in vec4 v_t1;
layout(location = 5) in vec4 v_t2;
layout(location = 6) in vec4 v_t3;
layout(location = 7) noperspective in float v_zs;

layout(location = 0) out vec4 o_color;

/* The values that decide what the shader does: the texture-shader modes,
 * combiner words, alpha function and so on. Read from the uniforms here
 * (one shader for every draw); nv2a_vk.c also builds this source with
 * NV2A_SPEC and a header of constants for one combiner setup, and the
 * compiler then drops every branch and loop the setup does not use -- the
 * register file R[] stops being indexed at run time, which is what made
 * this shader most of the GPU time at higher resolutions. */
#ifdef NV2A_SPEC
#define K_MISC   uvec4(SP_MISC.x, SP_MISC.y, u.misc.z, SP_MISC.w)
#define K_MISC2  SP_MISC2
#define K_MISC3  SP_MISC3
#define K_CW(i)  SP_CW[i]
#define K_PTSZ   SP_PTSZ
#define K_TSZ(i) SP_TSZ[i]
#define K_VPW    SP_VPW
#else
#define K_MISC   u.misc
#define K_MISC2  u.misc2
#define K_MISC3  u.misc3
#define K_CW(i)  u.cw[i]
#define K_PTSZ   u.pts.z
#define K_TSZ(i) u.ts[i].z
#define K_VPW    u.vp.w
#endif

/* NV2A_MP: the precision of the combiner arithmetic -- mediump (16-bit,
 * about twice the rate on mobile GPUs) where nv2a_vk.c asks for it; the
 * combiners themselves are about 9 bits a channel. Texture coordinates,
 * depth and the texture-shader dot products stay full precision. */
#ifndef NV2A_MP
#define NV2A_MP
#endif

NV2A_MP vec4 R[16];

NV2A_MP float map_in(NV2A_MP float x, uint b)
{
    NV2A_MP float p = max(x, 0.0);
    switch ((b >> 5) & 7u) {
    case 0u: return p;
    case 1u: return 1.0 - clamp(x, 0.0, 1.0);
    case 2u: return 2.0 * p - 1.0;
    case 3u: return -2.0 * p + 1.0;
    case 4u: return p - 0.5;
    case 5u: return -p + 0.5;
    case 6u: return x;
    default: return -x;
    }
}

NV2A_MP float map_out(NV2A_MP float x, uint m)
{
    switch (m) {
    case 0x08u: return x - 0.5;
    case 0x10u: return x * 2.0;
    case 0x18u: return (x - 0.5) * 2.0;
    case 0x20u: return x * 4.0;
    case 0x30u: return x * 0.5;
    default:    return x;
    }
}

NV2A_MP vec3 map_out3(NV2A_MP vec3 v, uint m)
{
    return clamp(vec3(map_out(v.x, m), map_out(v.y, m), map_out(v.z, m)), -1.0, 1.0);
}

NV2A_MP vec3 read_rgb(uint b)
{
    b &= 0xFFu;
    NV2A_MP vec4 r = R[b & 15u];
    NV2A_MP vec3 v = (b & 0x10u) != 0u ? r.aaa : r.rgb;
    return vec3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));
}

NV2A_MP float read_alpha(uint b)
{
    b &= 0xFFu;
    NV2A_MP vec4 r = R[b & 15u];
    return map_in((b & 0x10u) != 0u ? r.a : r.b, b);
}

/* What each texture stage produced, and the dot products the texture-shader
 * modes leave behind for the stages after them. */
NV2A_MP vec4 T[4];
float D[4];
vec4 TC[4];

/* The texel of stage `inp` as a signed vector, by SET_DOT_RGBMAPPING. */
vec3 dot_input(uint st, uint inp)
{
    uint m = (K_MISC3.z >> ((st - 1u) * 4u)) & 0xFu;
    vec3 c = T[inp].rgb;
    if (m == 0u)
        return c;                                     /* ZERO_TO_ONE      */
    if (m == 1u) {                                    /* -1..1, D3D style */
        vec3 x = c * 255.0 - 128.0;
        return x / mix(vec3(128.0), vec3(127.0), greaterThanEqual(x, vec3(0.0)));
    }
    if (m == 3u) {                                    /* bytes as signed  */
        vec3 b = floor(c * 255.0 + 0.5);
        return mix(b, b - 256.0, greaterThanEqual(b, vec3(128.0))) / 127.0;
    }
    return c * 2.0 - 1.0;                             /* -1..1, GL style  */
}

/* The stage whose texel a texture-shader stage reads: stage 1 always reads
 * stage 0; 2 and 3 say which in SET_SHADER_OTHER_STAGE_INPUT. */
uint input_stage(uint st)
{
    if (st == 1u)
        return 0u;
    return st == 2u ? (K_MISC3.y >> 16) & 0xFu : (K_MISC3.y >> 20) & 0xFu;
}

/* What texture stage `st` contributes, by its SHADER_STAGE_PROGRAM mode
 * (the NV2A texture shader: modes 0-5 as the software rasteriser's
 * rc_stage_fetch, 6+ the dependent and dot-product modes). */
vec4 stage_fetch(uint st, sampler2D s2, samplerCube sc)
{
    uint mode = (K_MISC.w >> (st * 5u)) & 0x1Fu;
    vec4 c = TC[st], ts = u.ts[st];
    D[st] = 0.0;
    if (mode == 0u)
        return vec4(0.0, 0.0, 0.0, 1.0);
    if (mode == 1u || mode == 2u) {
        float q = c.w != 0.0 ? c.w : 1.0;
        return texture(s2, c.xy / q * ts.xy);
    }
    if (mode == 3u) {
        if (K_TSZ(st) != 0.0)
            return texture(sc, c.xyz);
        return texture(s2, c.xy * ts.xy);
    }
    if (mode == 4u)
        return clamp(c, 0.0, 1.0);
    if (mode == 5u) {
        for (uint j = 0u; j < 4u; j++) {
            bool ge = ((K_MISC2.w >> (st * 4u + j)) & 1u) != 0u;
            if (ge ? c[j] >= 0.0 : c[j] < 0.0)
                discard;
        }
        return vec4(0.0);
    }
    if (st == 0u)
        return vec4(0.0);                     /* the rest read a stage before */
    {
        uint inp = input_stage(st);
        vec4 tin = T[min(inp, 3u)];
        if (mode == 15u)                      /* DEPENDENT_AR */
            return texture(s2, vec2(tin.a, tin.r));
        if (mode == 16u)                      /* DEPENDENT_GB */
            return texture(s2, vec2(tin.g, tin.b));
        if (mode == 6u || mode == 7u) {       /* BUMPENVMAP(_LUM) */
            /* ponytail: identity bump matrix; the matrix registers are
             * not tracked yet. */
            vec2 d = tin.rg * 2.0 - 1.0;
            return texture(s2, (c.xy + d) * ts.xy);
        }
        vec3 n = dot_input(st, inp);
        D[st] = dot(c.xyz, n);
        if (mode == 17u)                      /* DOTPRODUCT: feeds later stages */
            return vec4(0.0);
        if (mode == 9u)                       /* DOT_ST: (previous dot, this dot) */
            return texture(s2, vec2(D[st - 1u], D[st]) * ts.xy);
        if (mode == 10u)                      /* DOT_ZW: depth replace, not done */
            return vec4(0.0);
        if (st >= 2u) {
            vec3 N = st == 3u ? vec3(D[1], D[2], D[3]) : vec3(D[1], D[2], 0.0);
            if (mode == 11u)                  /* DOT_REFLECT_DIFFUSE */
                return texture(sc, N);
            if (mode == 13u || mode == 14u)   /* DOT_STR_3D / _CUBE */
                return texture(sc, N);
            if ((mode == 12u || mode == 18u) && (K_MISC.x & 4u) == 0u)
                return vec4(0.0);             /* off unless RECOMP_VK_DOT_REFLECT */
            if (mode == 12u || mode == 18u) { /* DOT_REFLECT_SPECULAR(_CONST) */
                vec3 E = mode == 12u ? vec3(TC[1].w, TC[2].w, TC[3].w)
                                     : vec3(0.0, 0.0, 1.0);
                float nn = dot(N, N);
                vec3 R = nn != 0.0 ? 2.0 * N * dot(N, E) / nn - E : E;
                return texture(sc, R);
            }
        }
        return vec4(0.0);
    }
}

bool alpha_pass(float a)
{
    int v = int(a * 255.0 + 0.5), r = int(u.misc.z & 0xFFu);
    switch (K_MISC.y) {
    case 0x200u: return false;
    case 0x201u: return v <  r;
    case 0x202u: return v == r;
    case 0x203u: return v <= r;
    case 0x204u: return v >  r;
    case 0x205u: return v != r;
    case 0x206u: return v >= r;
    default:     return true;
    }
}

void main()
{
#ifdef NV2A_FLAT
    /* RECOMP_VK_FLAT: a flat colour, to measure what the combiners cost. */
    o_color = vec4(0.5, 0.5, 0.5, 1.0);
    return;
#endif
    /* The stored depth in title units (z or w LSBs), with the polygon offset:
     * factor * max screen-z slope + units, in z-buffer LSBs. Titles size it
     * that way under the w-buffer too (RSC2's car shadow: bias = k / dist^2),
     * so there it is carried over to w through z = zmax - B/w, which gives
     * dw = dz * w / (zmax - z). Worked out before any discard, as the slope
     * needs derivatives. */
#ifndef NV2A_FAST_DEPTH
    float zs = (K_VPW == 0.0 ? gl_FragCoord.z : v_zs) / u.vp.z;
    float zraw = K_VPW != 0.0 ? 1.0 / gl_FragCoord.w : zs;
    if (u.zoff.x != 0.0 || u.zoff.y != 0.0) {
        float dz = max(abs(dFdx(zs)) * u.zoff.z, abs(dFdy(zs)) * u.zoff.w);
        float off = u.zoff.x * dz + u.zoff.y;
        if (K_VPW != 0.0)
            off *= zraw / max(1.0 / u.vp.z - zs, 1.0);
        zraw = max(zraw + off, 0.0);
    }
#endif
    TC[0] = v_t0; TC[1] = v_t1; TC[2] = v_t2; TC[3] = v_t3;
    /* Point sprites: stage 3 samples at the point coordinate (xemu psh.c). */
    if (K_PTSZ != 0.0)
        TC[3] = vec4(gl_PointCoord, 1.0, 1.0);
    T[0] = T[1] = T[2] = T[3] = vec4(0.0);
    D[0] = D[1] = D[2] = D[3] = 0.0;
    T[0] = stage_fetch(0u, t2d[0], tcube[0]);
    T[1] = stage_fetch(1u, t2d[1], tcube[1]);
    T[2] = stage_fetch(2u, t2d[2], tcube[2]);
    T[3] = stage_fetch(3u, t2d[3], tcube[3]);
    vec4 t0 = T[0], t1 = T[1], t2 = T[2], t3 = T[3];
    vec4 outc;

    if ((K_MISC.x & 1u) == 0u) {
        /* No combiners programmed yet: texel * diffuse, as the software
         * rasteriser's fallback. */
        outc = v_d0;
        if ((K_MISC.x & 2u) != 0u)
            outc = texture(t2d[0], v_t0.xy * u.ts[0].xy) * v_d0;
    } else {
        uint ctl = K_MISC2.x, n = min(ctl & 0xFFu, 8u), flags = ctl >> 8;
        for (int i = 0; i < 16; i++)
            R[i] = vec4(0.0);
        R[3] = vec4(u.fogc.rgb, v_fog);
        R[4] = v_d0;
        R[5] = v_d1;
        R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;
        R[12].a = (K_MISC.w & 0x1Fu) != 0u ? t0.a : 1.0;

        for (uint s = 0u; s < n; s++) {
            uvec4 w = K_CW(s);
            uint icw = w.x, aicw = w.y, ocw = w.z, aocw = w.w;
            uint fl = ocw >> 12, afl = aocw >> 12;
            R[1] = u.f0[(flags & 0x010u) != 0u ? s : 0u];
            R[2] = u.f1[(flags & 0x100u) != 0u ? s : 0u];
            bool mux = (flags & 1u) != 0u ? R[12].a >= 0.5
                                          : (int(R[12].a * 255.0) & 1) != 0;
            NV2A_MP vec3 A = read_rgb(icw >> 24), B = read_rgb(icw >> 16);
            NV2A_MP vec3 C = read_rgb(icw >> 8),  D = read_rgb(icw);
            NV2A_MP float aA = read_alpha(aicw >> 24), aB = read_alpha(aicw >> 16);
            NV2A_MP float aC = read_alpha(aicw >> 8),  aD = read_alpha(aicw);

            NV2A_MP vec3 pab = (fl & 2u) != 0u ? vec3(dot(A, B)) : A * B;
            NV2A_MP vec3 pcd = (fl & 1u) != 0u ? vec3(dot(C, D)) : C * D;
            NV2A_MP vec3 sum = (fl & 4u) != 0u ? (mux ? pcd : pab) : pab + pcd;
            NV2A_MP vec3 ab = map_out3(pab, fl & 0x38u);
            NV2A_MP vec3 cd = map_out3(pcd, fl & 0x38u);
            NV2A_MP vec3 ms = map_out3(sum, fl & 0x38u);

            NV2A_MP float apab = aA * aB, apcd = aC * aD;
            NV2A_MP float asum = (afl & 4u) != 0u ? (mux ? apcd : apab) : apab + apcd;
            NV2A_MP float aab = clamp(map_out(apab, afl & 0x38u), -1.0, 1.0);
            NV2A_MP float acd = clamp(map_out(apcd, afl & 0x38u), -1.0, 1.0);
            NV2A_MP float ams = clamp(map_out(asum, afl & 0x38u), -1.0, 1.0);

            uint d;
            d = (ocw >> 4) & 15u;
            if (d != 0u) { R[d].rgb = ab; if ((fl & 0x80u) != 0u) R[d].a = ab.b; }
            d = ocw & 15u;
            if (d != 0u) { R[d].rgb = cd; if ((fl & 0x40u) != 0u) R[d].a = cd.b; }
            d = (ocw >> 8) & 15u;
            if (d != 0u) R[d].rgb = ms;
            d = (aocw >> 4) & 15u;
            if (d != 0u) R[d].a = aab;
            d = aocw & 15u;
            if (d != 0u) R[d].a = acd;
            d = (aocw >> 8) & 15u;
            if (d != 0u) R[d].a = ams;
            R[0] = vec4(0.0);
        }

        uint f0 = K_MISC2.y, f1 = K_MISC2.z;
        if (f0 != 0u || f1 != 0u) {
            uint ff = f1 & 0xFFu;
            R[1] = u.fc0;
            R[2] = u.fc1;
            NV2A_MP vec3 a = (ff & 0x40u) != 0u ? 1.0 - R[5].rgb : R[5].rgb;
            NV2A_MP vec3 b = (ff & 0x20u) != 0u ? 1.0 - R[12].rgb : R[12].rgb;
            R[14] = vec4((ff & 0x80u) != 0u ? clamp(a + b, 0.0, 1.0) : a + b, 0.0);
            NV2A_MP vec3 E = read_rgb(f1 >> 24), F = read_rgb(f1 >> 16);
            R[15] = vec4(E * F, 0.0);
            NV2A_MP vec3 A = read_rgb(f0 >> 24), B = read_rgb(f0 >> 16);
            NV2A_MP vec3 C = read_rgb(f0 >> 8),  D = read_rgb(f0);
            outc = vec4(D + C * (1.0 - A) + B * A, read_alpha(f1 >> 8));
        } else {
            outc = R[12];
        }
    }
    outc = clamp(outc, 0.0, 1.0);
    if (K_MISC3.x != 0u && !alpha_pass(outc.a))
        discard;
    o_color = outc;

    /* W-buffer: the depth is the eye-space w, perspective-correct. Z-buffer:
     * the NV2A keeps an integer, so round to it -- a colour pass under
     * DEPTH_FUNC EQUAL must land on exactly what its pre-pass stored.
     * NV2A_FAST_DEPTH (the variant drawn when none of that is needed) leaves
     * depth to the rasteriser, so hidden fragments are rejected before this
     * shader runs. */
#ifndef NV2A_FAST_DEPTH
    if (K_VPW == 2.0)
        gl_FragDepth = min(WREV_C / max(zraw, 1e-30), 1.0);
    else if (K_VPW != 0.0)
        gl_FragDepth = zraw * u.vp.z;
    else if (K_MISC3.w != 0u)
        gl_FragDepth = roundEven(zraw) * u.vp.z;
    else if (u.zoff.x != 0.0 || u.zoff.y != 0.0)
        gl_FragDepth = zraw * u.vp.z;
    else
        gl_FragDepth = gl_FragCoord.z;
#endif
}
