/*
 * mmio_decode.h -- the instruction decoder for trapped MMIO (x86-64, AArch64).
 *
 * A device whose registers need semantics cannot be plain memory: the reads
 * have to be answered and the writes have to be seen. The way that works here
 * is to leave the page PAGE_NOACCESS, catch the access in a vectored handler,
 * decode the faulting instruction, service it against the device model, and
 * step over it.
 *
 * That decoder existed twice before this file -- once in nv2a_mmio_hook.c and
 * once in apu_mmio_hook.c -- as the same opcode table written out against two
 * different pairs of accessors. This is the same logic with the accessors
 * passed in, so a third device does not need a third copy. The two originals
 * still carry their own and can move onto this whenever they are next touched.
 *
 * Header-only and static inline: one function, one caller per device, and a
 * library for it would be more build wiring than code.
 *
 * The instructions covered are what the XDK device code actually emits against
 * registers -- moves both ways, the immediate forms, the zero-extending loads,
 * and the read-modify-write and flag-setting forms a poll loop is built from.
 * Anything outside that set returns 0 rather than guessing: stepping over an
 * instruction that was not understood corrupts the guest silently, which is
 * far worse than a fault naming the opcode.
 */
#ifndef MMIO_DECODE_H
#define MMIO_DECODE_H

#include <stdint.h>

/* Hosts with a fault path: Win32, Linux/Android on x86-64 or AArch64, where
 * the sigaction VEH in win32_compat.c fills a CONTEXT from the signal frame,
 * and the Switch, where the exception entry in switch/nx_host.c does. Device
 * models test MMIO_FAULT_HOST before trapping their pages. */
#if defined(_WIN32) || (defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))) \
    || defined(__SWITCH__)
#define MMIO_FAULT_HOST 1
#endif

#ifdef MMIO_FAULT_HOST
#if defined(_WIN32)
#include <windows.h>
#else
#include "win32_compat.h"
#endif

#ifndef CONTEXT_PC
#define CONTEXT_PC(c) ((c)->Rip)
#endif

/* Service one register access. dev is passed straight back to the callbacks. */
typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

#if defined(__aarch64__)
/* AArch64: every access is one fixed-width load or store, and compares
 * happen in registers afterwards, so there are no flags to fake -- only the
 * transfer, the base register writeback of the indexed forms, and the PC.
 *
 * Covered: LDR/STR of every size and sign-extension in the unsigned-offset,
 * unscaled, pre- and post-index, unprivileged and register-offset forms, for
 * general and SIMD&FP registers; and the single-register acquire/release
 * forms (LDAR, STLR, LDAPR). Pairs, exclusives and atomics return 0. */
static inline void mmio_a64_set(PCONTEXT ctx, int rt, uint64_t v)
{
    if (rt != 31)                       /* xzr: the value goes nowhere */
        ctx->X[rt] = v;
}

static inline uint64_t mmio_a64_get(PCONTEXT ctx, int rt)
{
    return rt == 31 ? 0 : ctx->X[rt];
}

static inline uint64_t *mmio_a64_base(PCONTEXT ctx, int rn)
{
    return rn == 31 ? &ctx->Sp : &ctx->X[rn];
}

static inline int mmio_emulate(PCONTEXT ctx, uint32_t off, void *dev,
                               mmio_read_fn rd, mmio_write_fn wr)
{
    uint32_t in = *(const uint32_t *)(uintptr_t)ctx->Pc;
    int size = (int)(in >> 30), rt = (int)(in & 31), rn = (int)((in >> 5) & 31);
    int vreg = (int)((in >> 26) & 1), opc = (int)((in >> 22) & 3);
    int bytes, load, sext = 0, wide = 0;
    int64_t wb = 0;
    int writeback = 0;

    if (!rd || !wr)
        return 0;

    if ((in & 0x3F000000u) == 0x08000000u) {     /* load/store exclusive class */
        int o2 = (in >> 23) & 1, l = (in >> 22) & 1, o1 = (in >> 21) & 1;
        int o0 = (in >> 15) & 1;
        if (!(o2 == 1 && o1 == 0 && o0 == 1))    /* LDAR / STLR only */
            return 0;
        bytes = 1 << size;
        if (l)
            mmio_a64_set(ctx, rt, rd(dev, off, bytes));
        else
            wr(dev, off, mmio_a64_get(ctx, rt), bytes);
        ctx->Pc += 4;
        return 1;
    }

    if ((in & 0x38000000u) != 0x38000000u)       /* not a single-register load/store */
        return 0;

    switch ((in >> 24) & 3) {
    case 1:                                      /* unsigned 12-bit offset */
        break;
    case 0:
        if (in & (1u << 21)) {
            if (((in >> 10) & 3) == 2)           /* register offset */
                break;
            /* LDAPR: the RCpc acquire load, in the atomic-ops space */
            if (!vreg && ((in >> 10) & 0x3F) == 0x30 && ((in >> 16) & 31) == 31
                    && ((in >> 22) & 3) == 2) {
                mmio_a64_set(ctx, rt, rd(dev, off, 1 << size));
                ctx->Pc += 4;
                return 1;
            }
            return 0;                            /* atomics */
        }
        switch ((in >> 10) & 3) {
        case 0: case 2: break;                   /* unscaled, unprivileged */
        case 1: case 3:                          /* post- / pre-index */
            writeback = 1;
            wb = (int64_t)((int32_t)(in << 11) >> 23);    /* imm9 */
            break;
        }
        break;
    default:
        return 0;
    }

    if (vreg) {
        bytes = (opc & 2) ? (size == 0 ? 16 : 0) : 1 << size;
        if (!bytes || !ctx->HasV)
            return 0;
        load = opc & 1;
        if (load) {
            uint64_t lo = rd(dev, off, bytes > 8 ? 8 : bytes);
            uint64_t hi = bytes > 8 ? rd(dev, off + 8, 8) : 0;
            ctx->V[rt][0] = lo;                  /* a scalar load clears the rest */
            ctx->V[rt][1] = hi;
        } else {
            uint64_t lo = ctx->V[rt][0];
            if (bytes < 8)
                lo &= (1ULL << (bytes * 8)) - 1;
            wr(dev, off, lo, bytes > 8 ? 8 : bytes);
            if (bytes > 8)
                wr(dev, off + 8, ctx->V[rt][1], 8);
        }
    } else {
        bytes = 1 << size;
        switch (opc) {
        case 0: load = 0; break;
        case 1: load = 1; break;
        case 2:
            if (size == 3) {                     /* PRFM: nothing to do */
                ctx->Pc += 4;
                return 1;
            }
            load = 1; sext = 1; wide = 1;        /* LDRSB/LDRSH/LDRSW -> Xt */
            break;
        default:
            if (size >= 2)
                return 0;
            load = 1; sext = 1; wide = 0;        /* LDRSB/LDRSH -> Wt */
            break;
        }
        if (load) {
            uint64_t v = rd(dev, off, bytes);
            if (bytes < 8)
                v &= (1ULL << (bytes * 8)) - 1;
            if (sext) {
                int sh = 64 - bytes * 8;
                v = (uint64_t)((int64_t)(v << sh) >> sh);
                if (!wide)
                    v &= 0xFFFFFFFFULL;
            }
            mmio_a64_set(ctx, rt, v);
        } else {
            uint64_t v = mmio_a64_get(ctx, rt);
            if (bytes < 8)
                v &= (1ULL << (bytes * 8)) - 1;
            wr(dev, off, v, bytes);
        }
    }

    if (writeback)
        *mmio_a64_base(ctx, rn) += (uint64_t)wb;
    ctx->Pc += 4;
    return 1;
}

#else /* x86-64 */

static inline uint64_t *mmio_ctx_reg(PCONTEXT c, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&c->Rax;   case 1:  return (uint64_t *)&c->Rcx;
    case 2:  return (uint64_t *)&c->Rdx;   case 3:  return (uint64_t *)&c->Rbx;
    case 4:  return (uint64_t *)&c->Rsp;   case 5:  return (uint64_t *)&c->Rbp;
    case 6:  return (uint64_t *)&c->Rsi;   case 7:  return (uint64_t *)&c->Rdi;
    case 8:  return (uint64_t *)&c->R8;    case 9:  return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10;   case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12;   case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14;   case 15: return (uint64_t *)&c->R15;
    default: return NULL;
    }
}

static inline int mmio_modrm_len(const uint8_t *ip, int rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm  = (modrm & 7) | (rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1;                    /* SIB    */
    if (mod == 0 && (rm & 7) == 5) len += 4;        /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* Flags after a compare-shaped operation, so a poll loop branches correctly.
 * ZF, SF and CF only: those are what jz/jnz, js and jb/jae read, and inventing
 * an overflow flag nothing here sets is worse than leaving it alone. */
static inline void mmio_set_flags(PCONTEXT ctx, uint64_t result, int size,
                                  int carry)
{
    ctx->EFlags &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
    if (size < 8)
        result &= (1ULL << (size * 8)) - 1;
    if (result == 0)
        ctx->EFlags |= 0x0040u;                                   /* ZF */
    if (result & (1ULL << (size * 8 - 1)))
        ctx->EFlags |= 0x0080u;                                   /* SF */
    if (carry)
        ctx->EFlags |= 0x0001u;                                   /* CF */
}

/* 1 if the instruction at ctx->Rip was serviced and Rip advanced past it. */
static inline int mmio_emulate(PCONTEXT ctx, uint32_t off, void *dev,
                               mmio_read_fn rd, mmio_write_fn wr)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    int prefix = 0, has66 = 0, rex = 0, has_rex = 0;
    const uint8_t *op;
    int size, rex_w, rex_r, rex_b, mlen, reg;

    if (!rd || !wr)
        return 0;

    for (;;) {
        uint8_t b = ip[prefix];
        if (b == 0x66)                   { has66 = 1; prefix++; }
        else if (b == 0xF2 || b == 0xF3) { prefix++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix++; }
        else break;
    }
    rex_w = has_rex && (rex & 0x08);
    rex_r = has_rex && (rex & 0x04);
    rex_b = has_rex && (rex & 0x01);

    op   = ip + prefix;
    size = has66 ? 2 : (rex_w ? 8 : 4);

    switch (op[0]) {
    case 0x88: case 0x89:                            /* MOV r/m, r   (write) */
        if (op[0] == 0x88) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0xC7:                                       /* MOV r/m, imm32       */
        mlen = mmio_modrm_len(op + 1, rex_b);
        wr(dev, off, *(const uint32_t *)(op + 1 + mlen), size);
        ctx->Rip += prefix + 1 + mlen + 4;
        return 1;

    case 0xC6:                                       /* MOV r/m8, imm8       */
        mlen = mmio_modrm_len(op + 1, rex_b);
        wr(dev, off, op[1 + mlen], 1);
        ctx->Rip += prefix + 1 + mlen + 1;
        return 1;

    case 0x8A: case 0x8B: {                          /* MOV r, r/m   (read)  */
        uint64_t v, *dst;
        if (op[0] == 0x8A) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        v    = rd(dev, off, size);
        dst  = mmio_ctx_reg(ctx, reg);
        if (size == 1)      *dst = (*dst & ~0xFFULL)   | (v & 0xFF);
        else if (size == 2) *dst = (*dst & ~0xFFFFULL) | (v & 0xFFFF);
        else if (size == 4) *dst = v & 0xFFFFFFFFULL;  /* 32-bit clears high */
        else                *dst = v;
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x84: case 0x85: {                          /* TEST r/m, r          */
        uint64_t m, r;
        if (op[0] == 0x84) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = *mmio_ctx_reg(ctx, reg);
        mmio_set_flags(ctx, m & r, size, 0);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x38: case 0x39: {                          /* CMP r/m, r           */
        uint64_t m, r, mask;
        if (op[0] == 0x38) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = *mmio_ctx_reg(ctx, reg);
        mask = (size < 8) ? ((1ULL << (size * 8)) - 1) : ~0ULL;
        m &= mask; r &= mask;
        mmio_set_flags(ctx, m - r, size, m < r);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x08: case 0x09:                            /* OR  r/m, r           */
        if (op[0] == 0x08) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, rd(dev, off, size) | *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x20: case 0x21:                            /* AND r/m, r           */
        if (op[0] == 0x20) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, rd(dev, off, size) & *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    /* ALU r, r/m: the register is the destination and the device the
     * source -- `ecx &= MEM32(base + off)`. 0B OR, 23 AND, 03 ADD, 2B SUB,
     * 33 XOR, 3B CMP (no write); the 8-bit forms one below each. */
    case 0x02: case 0x03: case 0x0A: case 0x0B: case 0x22: case 0x23:
    case 0x2A: case 0x2B: case 0x32: case 0x33: case 0x3A: case 0x3B: {
        uint64_t m, r, res, mask, *dst;
        int kind = op[0] & 0xF8, cf = 0;
        if ((op[0] & 1) == 0) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        mask = size < 8 ? (1ULL << (size * 8)) - 1 : ~0ULL;
        m    = rd(dev, off, size) & mask;
        dst  = mmio_ctx_reg(ctx, reg);
        r    = *dst & mask;
        switch (kind) {
        case 0x00: res = r + m; cf = (res & mask) < r; break;   /* ADD */
        case 0x08: res = r | m; break;                          /* OR  */
        case 0x20: res = r & m; break;                          /* AND */
        case 0x28: case 0x38: res = r - m; cf = r < m; break;   /* SUB CMP */
        default:   res = r ^ m; break;                          /* XOR */
        }
        res &= mask;
        mmio_set_flags(ctx, res, size, cf);
        if (kind != 0x38) {
            if (size == 1)      *dst = (*dst & ~0xFFULL)   | res;
            else if (size == 2) *dst = (*dst & ~0xFFFFULL) | res;
            else                *dst = res;          /* 32-bit clears high */
        }
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    /* GCC folds `MEM32(x) & mask` into TEST with an immediate and
     * `MEM32(x) |= bit` into an ALU op on memory, so a lifted driver reaches
     * a device through these as often as through MOV. */
    case 0xF6: case 0xF7: {                          /* TEST r/m, imm (/0)  */
        uint64_t imm;
        int ilen;
        if (((op[1] >> 3) & 7) != 0)
            return 0;
        if (op[0] == 0xF6) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        ilen = size == 1 ? 1 : size == 2 ? 2 : 4;
        imm = ilen == 1 ? op[1 + mlen]
            : ilen == 2 ? *(const uint16_t *)(op + 1 + mlen)
            : (uint64_t)(int64_t)*(const int32_t *)(op + 1 + mlen);
        mmio_set_flags(ctx, rd(dev, off, size) & imm, size, 0);
        ctx->Rip += prefix + 1 + mlen + ilen;
        return 1;
    }

    case 0x80: case 0x81: case 0x83: {               /* ALU r/m, imm        */
        int ext = (op[1] >> 3) & 7, ilen, cf = 0;
        uint64_t imm, m, r, mask;
        if (ext == 2 || ext == 3)                    /* ADC/SBB need CF in  */
            return 0;
        if (op[0] == 0x80) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        ilen = op[0] == 0x81 ? (size == 2 ? 2 : 4) : 1;
        if (op[0] == 0x81)
            imm = size == 2 ? *(const uint16_t *)(op + 1 + mlen)
                : (uint64_t)(int64_t)*(const int32_t *)(op + 1 + mlen);
        else if (op[0] == 0x83)
            imm = (uint64_t)(int64_t)(int8_t)op[1 + mlen];
        else
            imm = op[1 + mlen];
        mask = size < 8 ? (1ULL << (size * 8)) - 1 : ~0ULL;
        m = rd(dev, off, size) & mask;
        imm &= mask;
        switch (ext) {
        case 0:  r = m + imm; cf = (r & mask) < m; break;
        case 1:  r = m | imm; break;
        case 4:  r = m & imm; break;
        case 5: case 7: r = m - imm; cf = m < imm; break;
        default: r = m ^ imm; break;
        }
        mmio_set_flags(ctx, r & mask, size, cf);
        if (ext != 7)
            wr(dev, off, r & mask, size);
        ctx->Rip += prefix + 1 + mlen + ilen;
        return 1;
    }

    case 0x0F:
        if (op[1] == 0xBE || op[1] == 0xBF) {        /* MOVSX r, r/m8|16    */
            int s2 = (op[1] == 0xBE) ? 1 : 2;
            uint64_t v, *dst;
            int64_t sv;
            mlen = mmio_modrm_len(op + 2, rex_b);
            reg  = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            v    = rd(dev, off, s2);
            sv   = s2 == 1 ? (int64_t)(int8_t)v : (int64_t)(int16_t)v;
            dst  = mmio_ctx_reg(ctx, reg);
            if (has66)      *dst = (*dst & ~0xFFFFULL) | ((uint64_t)sv & 0xFFFF);
            else if (rex_w) *dst = (uint64_t)sv;
            else            *dst = (uint64_t)sv & 0xFFFFFFFFULL;
            ctx->Rip += prefix + 2 + mlen;
            return 1;
        }
        if (op[1] == 0xB6 || op[1] == 0xB7) {        /* MOVZX r32, r/m8|16   */
            int s = (op[1] == 0xB6) ? 1 : 2;
            mlen  = mmio_modrm_len(op + 2, rex_b);
            reg   = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            *mmio_ctx_reg(ctx, reg) = rd(dev, off, s) & ((1ULL << (s * 8)) - 1);
            ctx->Rip += prefix + 2 + mlen;
            return 1;
        }
        return 0;

    default:
        return 0;
    }
}

#endif /* x86-64 */
#endif /* MMIO_FAULT_HOST */
#endif /* MMIO_DECODE_H */
