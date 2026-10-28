/*
 * MCPX APU MMIO Hook - VEH instruction decoder for APU register access
 *
 * Reuses the same x86-64 instruction decoder pattern as nv2a_mmio_hook.c
 * but routes reads/writes through the MCPX APU register handlers.
 */

#include "apu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

/* Global APU state pointer -- referenced from main.c regardless of which
 * platform's MMIO hook is active, so define it before the #if guard. */
MCPXAPUState *g_apu_state = NULL;

/* The APU's registers at constant addresses, from lifted code (APU_RDn /
 * APU_WRn, the lifter's direct_apu_access): the model's own read and write,
 * which the fault path below ends in too, without the fault. */
uint32_t xbox_ApuMmioRead(uint32_t va, unsigned size)
{
    return g_apu_state ? (uint32_t)mcpx_apu_mmio_read(g_apu_state, va - 0xFE800000u, size) : 0;
}

void xbox_ApuMmioWrite(uint32_t va, uint32_t val, unsigned size)
{
    if (g_apu_state)
        mcpx_apu_mmio_write(g_apu_state, va - 0xFE800000u, val, size);
}

/* The MMIO hook decodes the faulting instruction under a Win32 VEH -- on
 * Linux the sigaction shim in win32_compat.c. x86-64 uses the decoder below;
 * AArch64 the shared one in mmio_decode.h. Hosts without a fault path have
 * no hook. */
#include "../platform/mmio_decode.h"
#ifdef MMIO_FAULT_HOST

/* APU MMIO base in Xbox VA space */
#define APU_MMIO_BASE  0xFE800000u
#define APU_MMIO_SIZE  0x00080000u  /* 512KB */

/* (g_apu_state is defined above, outside the Win32 guard) */

/* Statistics */
static int g_apu_mmio_read_count = 0;
static int g_apu_mmio_write_count = 0;
static int g_apu_mmio_decode_fail = 0;

#if !defined(__aarch64__)
/* ============================================================
 * x86-64 register access helpers (same as NV2A hook)
 * ============================================================ */

static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t*)&ctx->Rax;
    case 1:  return (uint64_t*)&ctx->Rcx;
    case 2:  return (uint64_t*)&ctx->Rdx;
    case 3:  return (uint64_t*)&ctx->Rbx;
    case 4:  return (uint64_t*)&ctx->Rsp;
    case 5:  return (uint64_t*)&ctx->Rbp;
    case 6:  return (uint64_t*)&ctx->Rsi;
    case 7:  return (uint64_t*)&ctx->Rdi;
    case 8:  return (uint64_t*)&ctx->R8;
    case 9:  return (uint64_t*)&ctx->R9;
    case 10: return (uint64_t*)&ctx->R10;
    case 11: return (uint64_t*)&ctx->R11;
    case 12: return (uint64_t*)&ctx->R12;
    case 13: return (uint64_t*)&ctx->R13;
    case 14: return (uint64_t*)&ctx->R14;
    case 15: return (uint64_t*)&ctx->R15;
    default: return NULL;
    }
}

static int decode_modrm_len(const uint8_t *ip, int has_rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm = (modrm & 7) | (has_rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1; /* SIB */
    if (mod == 0 && (rm & 7) == 5) len += 4; /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* ZF, SF and CF after an emulated compare-shaped access, so the jcc after
 * it branches as it would on the hardware. OF is cleared, as TEST and the
 * logical ops do; it is not modelled for ADD/SUB/CMP. */
static void apu_set_flags(PCONTEXT ctx, uint64_t result, int size, int cf)
{
    ctx->EFlags &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
    if (result == 0) ctx->EFlags |= 0x0040u;
    if (result & (1ULL << (size * 8 - 1))) ctx->EFlags |= 0x0080u;
    if (cf) ctx->EFlags |= 0x0001u;
}

/* ============================================================
 * Instruction decoder for APU MMIO access
 * ============================================================ */

#endif /* !aarch64 helpers */

#if defined(__aarch64__)
static uint64_t apu_rd(void *dev, uint32_t off, int size)
{
    (void)dev;
    g_apu_mmio_read_count++;
    return mcpx_apu_mmio_read(g_apu_state, off, (unsigned)size);
}

static void apu_wr(void *dev, uint32_t off, uint64_t val, int size)
{
    (void)dev;
    g_apu_mmio_write_count++;
    mcpx_apu_mmio_write(g_apu_state, off, val, (unsigned)size);
}

static bool apu_decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Pc;
    (void)is_write;
    if (!g_apu_state)
        return false;
    if (mmio_emulate(ctx, mmio_offset, NULL, apu_rd, apu_wr))
        return true;
    if (++g_apu_mmio_decode_fail <= 20) {
        fprintf(stderr, "[APU] MMIO decode fail at PC=%p offset=0x%X: %02X %02X %02X %02X\n",
                (void *)ip, mmio_offset, ip[0], ip[1], ip[2], ip[3]);
        fflush(stderr);
    }
    return false;
}
#else
static bool apu_decode_and_handle(PCONTEXT ctx, uint32_t mmio_offset, int is_write)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    if (!g_apu_state) return false;

    int prefix_len = 0;
    int has_66 = 0;
    int rex = 0, has_rex = 0;

    while (1) {
        uint8_t b = ip[prefix_len];
        if (b == 0x66) { has_66 = 1; prefix_len++; }
        else if (b == 0xF2 || b == 0xF3) { prefix_len++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix_len++; }
        else break;
    }

    int rex_w = has_rex && (rex & 0x08);
    int rex_r = has_rex && (rex & 0x04);
    int rex_b = has_rex && (rex & 0x01);

    const uint8_t *opcode = ip + prefix_len;
    int access_size = 4;
    if (has_66) access_size = 2;
    if (rex_w) access_size = 8;

    /* MOV r/m, r (write: 88/89) */
    if (opcode[0] == 0x89 || opcode[0] == 0x88) {
        if (opcode[0] == 0x88) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r/m, imm32 (write: C7 /0) */
    if (opcode[0] == 0xC7) {
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint32_t imm = *(uint32_t *)(opcode + 1 + modrm_len);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, imm, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len + 4;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r/m8, imm8 (write: C6 /0) */
    if (opcode[0] == 0xC6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        uint8_t imm = *(opcode + 1 + modrm_len);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, imm, 1);
        ctx->Rip += prefix_len + 1 + modrm_len + 1;
        g_apu_mmio_write_count++;
        return true;
    }

    /* MOV r, r/m (read: 8A/8B) */
    if (opcode[0] == 0x8B || opcode[0] == 0x8A) {
        if (opcode[0] == 0x8A) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t *dst = ctx_reg64(ctx, reg);
        if (access_size == 1) *dst = (*dst & ~0xFFULL) | (val & 0xFF);
        else if (access_size == 2) *dst = (*dst & ~0xFFFFULL) | (val & 0xFFFF);
        else if (access_size == 4) *dst = val & 0xFFFFFFFF;
        else *dst = val;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m8 (0F B6) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB6) {
        access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, 1);
        *ctx_reg64(ctx, reg) = val & 0xFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* MOVZX r32, r/m16 (0F B7) */
    if (opcode[0] == 0x0F && opcode[1] == 0xB7) {
        access_size = 2;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, 2);
        *ctx_reg64(ctx, reg) = val & 0xFFFF;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* TEST r/m, r (84/85) - read */
    if (opcode[0] == 0x85 || opcode[0] == 0x84) {
        if (opcode[0] == 0x84) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        uint64_t result = mem_val & reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* CMP r/m, r (38/39) */
    if (opcode[0] == 0x39 || opcode[0] == 0x38) {
        if (opcode[0] == 0x38) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        if (access_size <= 4) {
            mem_val &= (1ULL << (access_size * 8)) - 1;
            reg_val &= (1ULL << (access_size * 8)) - 1;
        }
        uint64_t result = mem_val - reg_val;
        ctx->EFlags &= ~(0x0001 | 0x0040 | 0x0080 | 0x0800);
        if (result == 0) ctx->EFlags |= 0x0040;
        if (mem_val < reg_val) ctx->EFlags |= 0x0001;
        if (result & (1ULL << (access_size * 8 - 1))) ctx->EFlags |= 0x0080;
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* OR r/m, r (08/09) */
    if (opcode[0] == 0x09 || opcode[0] == 0x08) {
        if (opcode[0] == 0x08) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, mem_val | reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* AND r/m, r (20/21) */
    if (opcode[0] == 0x21 || opcode[0] == 0x20) {
        if (opcode[0] == 0x20) access_size = 1;
        int modrm_len = decode_modrm_len(opcode + 1, rex_b);
        int reg = ((opcode[1] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        uint64_t reg_val = *ctx_reg64(ctx, reg);
        mcpx_apu_mmio_write(g_apu_state, mmio_offset, mem_val & reg_val, access_size);
        ctx->Rip += prefix_len + 1 + modrm_len;
        g_apu_mmio_write_count++;
        return true;
    }

    /* The immediate forms. GCC folds `MEM32(reg) & mask` straight into a
     * memory-operand TEST and `MEM32(reg) |= bit` into an ALU op on memory,
     * so the lifted DirectSound reaches the APU through these as often as
     * through a plain MOV; RalliSport Challenge 2's first APU access was
     * `test dword [rax+r8], 0x18`. */

    /* TEST r/m, imm (F6 /0 imm8, F7 /0 imm16/32) */
    if ((opcode[0] == 0xF6 || opcode[0] == 0xF7) && ((opcode[1] >> 3) & 7) == 0) {
        int modrm_len, imm_len;
        uint64_t imm, mem_val;
        if (opcode[0] == 0xF6) access_size = 1;
        modrm_len = decode_modrm_len(opcode + 1, rex_b);
        imm_len = access_size == 1 ? 1 : access_size == 2 ? 2 : 4;
        imm = imm_len == 1 ? opcode[1 + modrm_len]
            : imm_len == 2 ? *(const uint16_t *)(opcode + 1 + modrm_len)
            : (uint64_t)(int64_t)*(const int32_t *)(opcode + 1 + modrm_len);
        mem_val = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size);
        apu_set_flags(ctx, mem_val & imm, access_size, 0);
        ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* ALU r/m, imm (80 /n imm8, 81 /n imm16/32, 83 /n imm8 sign-extended):
     * ADD OR AND SUB XOR CMP. ADC/SBB need the incoming carry and are left
     * to the decode-fail report. */
    if (opcode[0] == 0x80 || opcode[0] == 0x81 || opcode[0] == 0x83) {
        int ext = (opcode[1] >> 3) & 7;
        if (ext != 2 && ext != 3) {
            int modrm_len, imm_len;
            uint64_t imm, m, r, mask;
            int cf = 0;
            if (opcode[0] == 0x80) access_size = 1;
            modrm_len = decode_modrm_len(opcode + 1, rex_b);
            imm_len = (opcode[0] == 0x81) ? (access_size == 2 ? 2 : 4) : 1;
            if (opcode[0] == 0x81)
                imm = access_size == 2 ? *(const uint16_t *)(opcode + 1 + modrm_len)
                    : (uint64_t)(int64_t)*(const int32_t *)(opcode + 1 + modrm_len);
            else if (opcode[0] == 0x83)
                imm = (uint64_t)(int64_t)(int8_t)opcode[1 + modrm_len];
            else
                imm = opcode[1 + modrm_len];
            mask = access_size < 8 ? (1ULL << (access_size * 8)) - 1 : ~0ULL;
            m = mcpx_apu_mmio_read(g_apu_state, mmio_offset, access_size) & mask;
            imm &= mask;
            switch (ext) {
            case 0: r = m + imm; cf = (r & mask) < m; break;   /* ADD */
            case 1: r = m | imm; break;                        /* OR  */
            case 4: r = m & imm; break;                        /* AND */
            case 5: case 7: r = m - imm; cf = m < imm; break;  /* SUB, CMP */
            default: r = m ^ imm; break;                       /* XOR */
            }
            apu_set_flags(ctx, r & mask, access_size, cf);
            if (ext != 7) {
                mcpx_apu_mmio_write(g_apu_state, mmio_offset, r & mask, access_size);
                g_apu_mmio_write_count++;
            } else {
                g_apu_mmio_read_count++;
            }
            ctx->Rip += prefix_len + 1 + modrm_len + imm_len;
            return true;
        }
    }

    /* MOVSX r, r/m8|16 (0F BE / 0F BF) */
    if (opcode[0] == 0x0F && (opcode[1] == 0xBE || opcode[1] == 0xBF)) {
        int sz = opcode[1] == 0xBE ? 1 : 2;
        int modrm_len = decode_modrm_len(opcode + 2, rex_b);
        int reg = ((opcode[2] >> 3) & 7) | (rex_r ? 8 : 0);
        uint64_t v = mcpx_apu_mmio_read(g_apu_state, mmio_offset, sz);
        int64_t sv = sz == 1 ? (int64_t)(int8_t)v : (int64_t)(int16_t)v;
        uint64_t *dst = ctx_reg64(ctx, reg);
        if (has_66)     *dst = (*dst & ~0xFFFFULL) | ((uint64_t)sv & 0xFFFF);
        else if (rex_w) *dst = (uint64_t)sv;
        else            *dst = (uint64_t)sv & 0xFFFFFFFFULL;
        ctx->Rip += prefix_len + 2 + modrm_len;
        g_apu_mmio_read_count++;
        return true;
    }

    /* Unrecognized */
    g_apu_mmio_decode_fail++;
    if (g_apu_mmio_decode_fail <= 20) {
        fprintf(stderr, "[APU] MMIO decode fail at RIP=%p offset=0x%X: %02X %02X %02X %02X %02X %02X\n",
                (void*)ctx->Rip, mmio_offset, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return false;
}
#endif /* x86-64 */

/* ============================================================
 * Public API (called from VEH in main.c)
 * ============================================================ */

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write)
{
    uint32_t mmio_offset = fault_xbox_va - APU_MMIO_BASE;
    bool ok = apu_decode_and_handle(ctx, mmio_offset, is_write);

    /* What the title actually asks the APU for. The DSPs are stubbed here, so
     * a title that waits on one waits forever, and the only way to work out
     * what it is waiting for is to see the register traffic that precedes the
     * wait. */
    static int trace = -1;              /* read once: this runs per fault */
    if (trace < 0)
        trace = getenv("RECOMP_APU_TRACE") != NULL;
    if (trace) {
        static unsigned n;
        if (n++ < 400) {
            /* The value as well as the offset: finding which register carries
             * the command-block address means recognising the address when it
             * goes past, and an offset alone never shows it. */
            uint64_t v = g_apu_state
                       ? mcpx_apu_mmio_read(g_apu_state, mmio_offset, 4) : 0;
            fprintf(stderr, "  [APUMMIO] %s 0x%05X = %08X%s\n",
                    is_write ? "write" : "read ", mmio_offset,
                    (uint32_t)v, ok ? "" : "  (decode failed)");
        }
    }
    return ok;
}

#endif /* MMIO_FAULT_HOST */
