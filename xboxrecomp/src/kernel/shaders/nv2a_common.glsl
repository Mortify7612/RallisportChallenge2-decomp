/* Per-draw state shared by both stages. std140, mirrored by Nv2aVkUbo in
 * nv2a_vk.c -- every member is 16-byte aligned so the two cannot drift. */
layout(set = 0, binding = 0, std140) uniform Nv2aDraw {
    vec4  vp;        /* 2/W, 2/H, 1/zmax, depth mode: 0 z-buffer, 1 w-buffer
                        stored as w/zmax, 2 w-buffer stored as WREV_C / w
                        (reversed: nearer is larger)                       */
    uvec4 misc;      /* flags (1 combiners, 2 legacy tex0), alpha func, ref, stage program */
    uvec4 misc2;     /* combiner control, final cw0, final cw1, clip-plane mode */
    vec4  fogc;      /* fog colour rgb, a = fog enable                     */
    vec4  fogp;      /* fog param x, y, mode                               */
    uvec4 cw[8];     /* colour ICW, alpha ICW, colour OCW, alpha OCW       */
    vec4  f0[8];     /* combiner factor 0, per stage                       */
    vec4  f1[8];
    vec4  fc0;       /* final combiner constants                           */
    vec4  fc1;
    vec4  ts[4];     /* texcoord scale xy, z = cube, w = bound             */
    uvec4 misc3;     /* alpha test enable, other-stage input, dot mapping, zint */
    vec4  pts;       /* point size px, render scale, sprite, size from oPts */
    vec4  zoff;      /* polygon offset: slope factor, units (depth LSBs);
                        host pixels per title pixel across, down           */
} u;

/* Reversed w-buffer (vp.w == 2): depth = WREV_C / w. With gl_Position.z set to
 * this constant the rasteriser produces it itself -- 1/w interpolates
 * linearly on screen -- so the fragment shader need not write gl_FragDepth
 * and early depth testing stays on. In a float depth buffer it keeps the
 * w-buffer's even precision; w below WREV_C (title units) clamps to 1. */
const float WREV_C = 1.0 / 1024.0;
