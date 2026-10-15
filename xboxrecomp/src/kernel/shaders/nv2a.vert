#version 450
#extension GL_GOOGLE_include_directive : require
/* Vertices arrive already run through the title's vertex program on the CPU
 * (nv2a_vsh_interp.c): x, y in pixels, z in depth-buffer units, w the clip
 * w. Multiplying the screen position back by w gives a clip-space position
 * the hardware can clip, so the near plane is cut properly instead of on
 * the CPU. */
#include "nv2a_common.glsl"

layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_d0;
layout(location = 2) in vec4 a_d1;
layout(location = 3) in vec4 a_fog;
layout(location = 4) in vec4 a_t0;
layout(location = 5) in vec4 a_t1;
layout(location = 6) in vec4 a_t2;
layout(location = 7) in vec4 a_t3;

layout(location = 0) out vec4 v_d0;
layout(location = 1) out vec4 v_d1;
layout(location = 2) out float v_fog;
layout(location = 3) out vec4 v_t0;
layout(location = 4) out vec4 v_t1;
layout(location = 5) out vec4 v_t2;
layout(location = 6) out vec4 v_t3;
layout(location = 7) noperspective out float v_zs;   /* screen z / zmax */

/* The fixed-function fog unit, from the program's oFog.x (xemu vsh.c's
 * formulae, as nv2a_pb_exec.c's fog_factor). */
float fog_factor(float d)
{
    float px = u.fogp.x, py = u.fogp.y, f;
    uint mode = uint(u.fogp.z);
    if (u.fogc.a == 0.0)
        return 1.0;
    if (mode == 0x800u || mode == 0x802u)
        f = px + exp2(d * py * 16.0) - 1.5;
    else if (mode == 0x801u || mode == 0x803u)
        f = px + exp2(-d * d * py * py * 32.0) - 1.5;
    else
        f = px + d * py - 1.0;
    if (mode == 0x802u || mode == 0x803u || mode == 0x804u)
        f = abs(f);
    if (isnan(f))
        f = 1.0;
    return clamp(f, 0.0, 1.0);
}

void main()
{
    float w = a_pos.w;
    vec2 ndc = a_pos.xy * u.vp.xy - 1.0;
    gl_Position = vec4(ndc * w, u.vp.w == 2.0 ? WREV_C : a_pos.z * u.vp.z * w, w);
    v_zs = a_pos.z * u.vp.z;
    /* Vertex colours are clamped to [0,1] by the hardware, as in D3D. */
    v_d0 = clamp(a_d0, 0.0, 1.0);
    v_d1 = clamp(a_d1, 0.0, 1.0);
    v_fog = fog_factor(a_fog.x);
    v_t0 = a_t0;
    v_t1 = a_t1;
    v_t2 = a_t2;
    v_t3 = a_t3;
    gl_PointSize = max(u.pts.x, 1.0);
}
