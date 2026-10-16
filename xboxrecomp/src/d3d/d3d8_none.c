/*
 * d3d8_none.c -- the D3D8 compatibility layer on hosts that have neither
 * Direct3D 11 nor desktop OpenGL (Android). Titles rendered by the NV2A
 * pushbuffer executor never call into it; this keeps the library target.
 */
typedef int xbox_d3d8_none;
