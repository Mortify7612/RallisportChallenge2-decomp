/*
 * The guest thread's registers in one thread-local block (RECOMP_REG_BLOCK).
 *
 * Where the game is a dlopen'd library (Android: SDLActivity loads
 * libmain.so), a thread-local is reached through a TLS descriptor, which
 * is a call into the dynamic linker's resolver, one per thread-local a
 * function names, and a lifted function names five to ten of them. With the
 * registers in one block it asks once, and each register is an offset from
 * the answer.
 *
 * The generated code reaches the block through recomp_types.h's macros
 * (g_eax is (g_rb.g_eax): the members keep the old names, as a member named
 * esp would itself be rewritten by the generated code's `#define esp`).
 * The old names stay for code written by hand: xbox_memory_layout.c
 * defines each as a TLS symbol aliasing its member (RECOMP_REG_BLOCK_OFF_*),
 * so both reach the same storage.
 */
#ifndef XBOXRECOMP_RECOMP_REG_BLOCK_H
#define XBOXRECOMP_RECOMP_REG_BLOCK_H

#include <stdint.h>

typedef struct RecompRegBlock {
    uint32_t g_eax, g_ecx, g_edx, g_esp;                /*  0 .. 12 */
    uint32_t g_ebx, g_esi, g_edi, g_ebp;                /* 16 .. 28 */
    uint32_t g_fs_base, g_seh_ebp, g_eflags_shadow;     /* 32, 36, 40 */
    int      g_df, g_fp_top, g_fp_cmp;                  /* 44, 48, 52 */
    uint16_t g_fp_control_word, g_fp_cc;                /* 56, 58 */
    uint32_t pad_;                                      /* 60 */
    double   g_fp_stack[8];                             /* 64 */
    /* xmm0-7 and mm0-7: RecompXmm (16 bytes) and RecompMmx (8), unions of
     * plain types at 8-byte alignment, held here as their bytes and read
     * back through a cast (recomp_types.h; the lifted code is built with
     * -fno-strict-aliasing). SSE-heavy lifted functions name several. */
    uint64_t g_xmm[8][2];                               /* 128 */
    uint64_t g_mm[8];                                   /* 256 */
} RecompRegBlock;

/* The members' offsets, as numbers the aliases' assembly can use
 * (checked against offsetof where the block is defined). */
#define RECOMP_REG_BLOCK_OFF_g_eax             0
#define RECOMP_REG_BLOCK_OFF_g_ecx             4
#define RECOMP_REG_BLOCK_OFF_g_edx             8
#define RECOMP_REG_BLOCK_OFF_g_esp             12
#define RECOMP_REG_BLOCK_OFF_g_ebx             16
#define RECOMP_REG_BLOCK_OFF_g_esi             20
#define RECOMP_REG_BLOCK_OFF_g_edi             24
#define RECOMP_REG_BLOCK_OFF_g_ebp             28
#define RECOMP_REG_BLOCK_OFF_g_fs_base         32
#define RECOMP_REG_BLOCK_OFF_g_seh_ebp         36
#define RECOMP_REG_BLOCK_OFF_g_eflags_shadow   40
#define RECOMP_REG_BLOCK_OFF_g_df              44
#define RECOMP_REG_BLOCK_OFF_g_fp_top          48
#define RECOMP_REG_BLOCK_OFF_g_fp_cmp          52
#define RECOMP_REG_BLOCK_OFF_g_fp_control_word 56
#define RECOMP_REG_BLOCK_OFF_g_fp_cc           58
#define RECOMP_REG_BLOCK_OFF_g_fp_stack        64
#define RECOMP_REG_BLOCK_OFF_g_xmm             128
#define RECOMP_REG_BLOCK_OFF_g_mm              256

#endif
