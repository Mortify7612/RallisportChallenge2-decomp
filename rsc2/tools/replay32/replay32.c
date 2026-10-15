/*
 * replay32 -- run an original Xbox function natively and diff it against the
 * lifted run.
 *
 *   replay32 <entry-va-hex> pre.bin post.bin [max-ranges]
 *
 * pre.bin/post.bin come from RECOMP_SNAPSHOT (xboxrecomp src/kernel/
 * recomp_trace.c). The guest regions are mapped at their original addresses
 * -- this is a static, non-PIE i386 binary loaded at 0x08048000, clear of
 * guest RAM below 0x04000000 -- the snapshot registers are loaded, and the
 * function runs on this CPU from its own bytes. Whatever it writes is what the
 * lifted C should have written; every difference against post.bin is either a
 * lifting bug or another guest thread's activity during the lifted call.
 *
 * fs: is pointed at the snapshot's TIB with set_thread_area (i386 glibc uses
 * gs, so fs is free). A fault -- including a call into the kernel thunks,
 * whose slots hold the runtime's synthetic 0xFE00xxxx addresses -- is
 * reported with its EIP and the run stops there.
 */
#define _GNU_SOURCE
#include <asm/ldt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

extern uint32_t guest_regs[8], out_regs[8], trace_tf;
extern uint32_t guest_xmm[32], out_xmm[32];
extern uint32_t guest_fpcw, guest_fpn;
extern double guest_st[2];
extern uint8_t out_fpu[112];
extern void ret_stub(void);

/* Block-trace comparison (--bb): the lifted run's per-block registers from
 * bb.bin, and the native run's at the same addresses, single-stepped. */
typedef struct { uint32_t va, r[8]; } bbrec;   /* eax ecx edx ebx esp ebp esi edi */
static bbrec *s_lifted, *s_native;
static size_t s_nlifted, s_nnative, s_native_cap;
static uint32_t s_fn_lo, s_fn_hi;
static uint8_t *s_is_block;                    /* bitmap over [s_fn_lo, s_fn_hi) */

static void record_block(greg_t *g);

static void on_trap(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = ucv;
    greg_t *g = uc->uc_mcontext.gregs;
    uint32_t eip = (uint32_t)g[REG_EIP];
    (void)sig; (void)si;
    if (eip == (uint32_t)(uintptr_t)ret_stub) {
        g[REG_EFL] &= ~0x100;                  /* back on the host: stop stepping */
        return;
    }
    record_block(g);
}

static void record_block(greg_t *g)
{
    uint32_t eip = (uint32_t)g[REG_EIP];
    if (s_is_block && eip >= s_fn_lo && eip < s_fn_hi
            && (s_is_block[(eip - s_fn_lo) >> 3] & (1u << ((eip - s_fn_lo) & 7)))) {
        bbrec *r;
        if (s_nnative == s_native_cap) {
            s_native_cap = s_native_cap ? s_native_cap * 2 : 4096;
            s_native = realloc(s_native, s_native_cap * sizeof *s_native);
        }
        r = &s_native[s_nnative++];
        r->va = eip;
        r->r[0] = (uint32_t)g[REG_EAX]; r->r[1] = (uint32_t)g[REG_ECX];
        r->r[2] = (uint32_t)g[REG_EDX]; r->r[3] = (uint32_t)g[REG_EBX];
        r->r[4] = (uint32_t)g[REG_ESP]; r->r[5] = (uint32_t)g[REG_EBP];
        r->r[6] = (uint32_t)g[REG_ESI]; r->r[7] = (uint32_t)g[REG_EDI];
    }
}

static int load_bb(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t cap = 0;
    uint32_t i;
    if (!f) { perror(path); return -1; }
    for (;;) {
        bbrec r;
        if (fread(&r, 4, 9, f) != 9) break;
        if (s_nlifted == cap) {
            cap = cap ? cap * 2 : 4096;
            s_lifted = realloc(s_lifted, cap * sizeof *s_lifted);
        }
        s_lifted[s_nlifted++] = r;
    }
    fclose(f);
    s_is_block = calloc(((s_fn_hi - s_fn_lo) >> 3) + 1, 1);
    for (i = 0; i < s_nlifted; i++) {
        uint32_t va = s_lifted[i].va;
        if (va >= s_fn_lo && va < s_fn_hi)
            s_is_block[(va - s_fn_lo) >> 3] |= (uint8_t)(1u << ((va - s_fn_lo) & 7));
    }
    return 0;
}

static void compare_bb(void)
{
    static const char *nm[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    size_t i, li = 0, n;
    /* The lifted log holds every traced function; keep the target's blocks. */
    bbrec *lf = malloc(s_nlifted * sizeof *lf);
    for (i = 0; i < s_nlifted; i++)
        if (s_lifted[i].va >= s_fn_lo && s_lifted[i].va < s_fn_hi)
            lf[li++] = s_lifted[i];
    n = li < s_nnative ? li : s_nnative;
    /* REPLAY_IGNORE_EBP: lifted functions keep ebp in a local, and a caller
     * that uses ebp as data does not publish it before a call (fpo), so a
     * callee's entry record shows a stale ebp it only saves and restores. */
    int skip_ebp = getenv("REPLAY_IGNORE_EBP") != NULL;
    printf("block trace: %zu lifted, %zu native records in 0x%08X..0x%08X\n",
           li, s_nnative, s_fn_lo, s_fn_hi);
    for (i = 0; i < n; i++) {
        int k, bad = lf[i].va != s_native[i].va;
        for (k = 0; k < 8 && !bad; k++)
            if (lf[i].r[k] != s_native[i].r[k] && !(skip_ebp && k == 5)) bad = 1;
        if (!bad) continue;
        printf("FIRST DIVERGENCE at record %zu:\n", i);
        if (i) {
            printf("  previous block 0x%08X agreed:", lf[i - 1].va);
            for (k = 0; k < 8; k++) printf(" %s=%08X", nm[k], lf[i - 1].r[k]);
            printf("\n");
        }
        printf("  native  0x%08X:", s_native[i].va);
        for (k = 0; k < 8; k++) printf(" %s=%08X", nm[k], s_native[i].r[k]);
        printf("\n  lifted  0x%08X:", lf[i].va);
        for (k = 0; k < 8; k++)
            printf(" %s=%08X%s", nm[k], lf[i].r[k], lf[i].r[k] != s_native[i].r[k] ? "*" : "");
        printf("\n");
        free(lf);
        return;
    }
    printf("block traces agree for %zu records%s\n", n,
           li != s_nnative ? " (lengths differ)" : "");
    free(lf);
}
extern int run_guest(uint32_t entry);
extern void abort_to_host(void);

typedef struct { uint32_t va, size; uint8_t *data; } region;
#define LOW_RELOC 0x05000000u   /* free in this process: below 0x08048000 */
typedef struct { uint32_t regs[8], fs; uint32_t n; region r[4]; uint32_t xmm[32]; int has_xmm;
                 int has_fpu; uint32_t fpcw, fptop; double fpst[8]; } snap;

static int load(const char *path, snap *s, int map_fixed)
{
    FILE *f = fopen(path, "rb");
    char magic[8];
    uint32_t hdr[10], i;
    if (!f) { perror(path); return -1; }
    if (fread(magic, 1, 8, f) != 8
            || (memcmp(magic, "XRSNAP01", 8) && memcmp(magic, "XRSNAP02", 8)
                && memcmp(magic, "XRSNAP03", 8))
            || fread(hdr, 4, 10, f) != 10) {
        fprintf(stderr, "%s: not a snapshot\n", path);
        return -1;
    }
    s->has_fpu = !memcmp(magic, "XRSNAP03", 8);
    s->has_xmm = s->has_fpu || !memcmp(magic, "XRSNAP02", 8);
    if (s->has_xmm && fread(s->xmm, 4, 32, f) != 32)
        return -1;
    if (s->has_fpu) {
        uint32_t fpu[2];
        if (fread(fpu, 4, 2, f) != 2 || fread(s->fpst, 8, 8, f) != 8)
            return -1;
        s->fpcw = fpu[0];
        s->fptop = fpu[1] & 7;
    }
    memcpy(s->regs, hdr, 32);
    s->fs = hdr[8];
    s->n = hdr[9] > 4 ? 4 : hdr[9];
    for (i = 0; i < s->n; i++) {
        if (fread(&s->r[i].va, 4, 1, f) != 1 || fread(&s->r[i].size, 4, 1, f) != 1)
            return -1;
        if (map_fixed) {
            /* Below vm.mmap_min_addr (the TIB page): mapped at LOW_RELOC
             * instead, and fs is rebased onto it in main. */
            uint32_t at = s->r[i].va < 0x10000u ? s->r[i].va + LOW_RELOC : s->r[i].va;
            void *p = mmap((void *)(uintptr_t)at, s->r[i].size,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != (void *)(uintptr_t)at) {
                fprintf(stderr, "cannot map 0x%08X+0x%X\n", s->r[i].va, s->r[i].size);
                return -1;
            }
            s->r[i].data = p;
        } else {
            s->r[i].data = malloc(s->r[i].size);
        }
        if (fread(s->r[i].data, 1, s->r[i].size, f) != s->r[i].size)
            return -1;
    }
    fclose(f);
    return 0;
}

static int s_faulted;

/* Kernel calls the replay can answer itself, by thunk-table slot (the
 * runtime gives slot i the synthetic address 0xFE000000 + 4i). Only ones with
 * no effect a single-threaded replay can observe: entering and leaving a
 * critical section nobody else contends, and IRQL changes. Anything else
 * stops the run. Slots are per title -- these are RalliSport Challenge 2's,
 * from its xbe_parser import list; REPLAY_KSLOTS="slot:argbytes:ret,..."
 * replaces them. */
typedef struct { uint32_t slot, argbytes, ret, keep_eax; } kslot;
static kslot s_kslots[32] = {
    { 21, 4, 0, 0 },   /* RtlEnterCriticalSection */
    { 20, 4, 0, 0 },   /* RtlLeaveCriticalSection */
    { 55, 0, 0, 0 },   /* KeRaiseIrqlToDpcLevel -> PASSIVE_LEVEL */
    { 54, 0, 0, 1 },   /* KfLowerIrql (fastcall cl) */
    { 66, 4, 0, 0 },   /* NtClearEvent */
};
static int s_nkslots = 5;

static int emulate_kernel(ucontext_t *uc)
{
    greg_t *g = uc->uc_mcontext.gregs;
    uint32_t slot = ((uint32_t)g[REG_EIP] - 0xFE000000u) / 4u;
    int i;
    for (i = 0; i < s_nkslots; i++) {
        if (s_kslots[i].slot != slot) continue;
        {
            /* REG_ESP: on i386 the kernel restores the stack pointer from
             * sigcontext.esp; REG_UESP is only esp_at_signal, a copy. */
            uint32_t esp = (uint32_t)g[REG_ESP];
            uint32_t ret = *(uint32_t *)(uintptr_t)esp;
            g[REG_ESP] = (greg_t)(esp + 4 + s_kslots[i].argbytes);
            g[REG_EIP] = (greg_t)ret;
            if (!s_kslots[i].keep_eax) g[REG_EAX] = (greg_t)s_kslots[i].ret;
            /* Resuming here skips the single-step trap for this address, so
             * a block starting at the return address is logged now. */
            if (trace_tf)
                record_block(g);
        }
        return 1;
    }
    return 0;
}

static void on_fault(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = ucv;
    if (sig == SIGSEGV && (uint32_t)uc->uc_mcontext.gregs[REG_EIP] >= 0xFE000000u
            && (uint32_t)uc->uc_mcontext.gregs[REG_EIP] < 0xFE001000u
            && emulate_kernel(uc))
        return;
    fprintf(stderr, "native run faulted: signal %d at EIP=0x%08X addr=%p"
            " (eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X)\n",
            sig, (unsigned)uc->uc_mcontext.gregs[REG_EIP], si->si_addr,
            (unsigned)uc->uc_mcontext.gregs[REG_EAX], (unsigned)uc->uc_mcontext.gregs[REG_ECX],
            (unsigned)uc->uc_mcontext.gregs[REG_EDX], (unsigned)uc->uc_mcontext.gregs[REG_ESI],
            (unsigned)uc->uc_mcontext.gregs[REG_EDI], (unsigned)uc->uc_mcontext.gregs[REG_ESP]);
    if ((uint32_t)uc->uc_mcontext.gregs[REG_EIP] >= 0xFE000000u)
        fprintf(stderr, "  (a kernel thunk call: slot %u)\n",
                ((uint32_t)uc->uc_mcontext.gregs[REG_EIP] - 0xFE000000u) / 4u);
    uc->uc_mcontext.gregs[REG_EIP] = (greg_t)(uintptr_t)abort_to_host;
    s_faulted = 1;
}

static int set_fs(uint32_t base)
{
    struct user_desc d;
    unsigned short sel;
    memset(&d, 0, sizeof d);
    d.entry_number = (unsigned)-1;
    d.base_addr = base;
    d.limit = 0xFFFFF;
    d.seg_32bit = 1;
    d.limit_in_pages = 1;
    d.useable = 1;
    if (syscall(SYS_set_thread_area, &d) != 0) { perror("set_thread_area"); return -1; }
    sel = (unsigned short)((d.entry_number << 3) | 3);
    __asm__ volatile("movw %0, %%fs" : : "r"(sel));
    return 0;
}

int main(int argc, char **argv)
{
    static const char *names[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    snap pre, orig, post;
    unsigned long long both = 0, only_native = 0, only_lifted = 0;
    struct sigaction sa;
    stack_t ss;
    uint32_t entry, i;
    int max_ranges = argc > 4 ? atoi(argv[4]) : 40, rc;
    unsigned long long total = 0;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <entry-va-hex> pre.bin post.bin [max-ranges]"
                " [--bb bb.bin [<lo-hex>] <hi-hex>]\n", argv[0]);
        return 2;
    }
    /* --bb bb.bin <hi> compares blocks in [entry, hi); --bb bb.bin <lo> <hi>
     * a wider range, to take the callees in as well. */
    {
        const char *ks = getenv("REPLAY_KSLOTS");
        if (ks) {
            s_nkslots = 0;
            while (*ks && s_nkslots < 32) {
                unsigned a1, a2, a3;
                if (sscanf(ks, "%u:%u:%u", &a1, &a2, &a3) != 3) break;
                s_kslots[s_nkslots].slot = a1; s_kslots[s_nkslots].argbytes = a2;
                s_kslots[s_nkslots].ret = a3; s_kslots[s_nkslots].keep_eax = 0;
                s_nkslots++;
                ks = strchr(ks, ',');
                if (!ks) break;
                ks++;
            }
        }
    }
    if (argc >= 8 && strcmp(argv[5], "--bb") == 0) {
        s_fn_lo = (uint32_t)strtoul(argv[1], NULL, 16);
        s_fn_hi = (uint32_t)strtoul(argv[7], NULL, 16);
        if (argc >= 9) {
            s_fn_lo = s_fn_hi;
            s_fn_hi = (uint32_t)strtoul(argv[8], NULL, 16);
        }
        if (load_bb(argv[6]))
            return 2;
        trace_tf = 1;
    }
    entry = (uint32_t)strtoul(argv[1], NULL, 16);
    if (load(argv[2], &pre, 1) || load(argv[2], &orig, 0) || load(argv[3], &post, 0))
        return 2;

    ss.ss_sp = malloc(1 << 16); ss.ss_size = 1 << 16; ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    if (trace_tf) {
        struct sigaction st;
        memset(&st, 0, sizeof st);
        st.sa_sigaction = on_trap;
        st.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGTRAP, &st, NULL);
    }

    if (set_fs(pre.fs < 0x10000u ? pre.fs + LOW_RELOC : pre.fs))
        return 2;
    memcpy(guest_regs, pre.regs, 32);
    if (pre.has_xmm)
        memcpy(guest_xmm, pre.xmm, sizeof pre.xmm);
    if (pre.has_fpu) {
        /* st(0) and st(1) as the lifted code held them: where this compiler
         * passes float arguments. Deeper entries are not loaded -- the lift
         * keeps no tag word, so which are live is unknown. */
        guest_fpcw = pre.fpcw;
        guest_st[0] = pre.fpst[pre.fptop];
        guest_st[1] = pre.fpst[(pre.fptop + 1) & 7];
        guest_fpn = 2;
    }
    rc = run_guest(entry);
    __asm__ volatile("movw %0, %%fs" : : "r"((unsigned short)0));
    if (rc == -1 || s_faulted) {
        printf("NATIVE RUN DID NOT COMPLETE (see fault above)\n");
        if (trace_tf) {
            printf("comparing the blocks it did run:\n");
            compare_bb();
        }
        return 1;
    }

    if (trace_tf)
        compare_bb();
    printf("registers (native vs lifted):\n");
    for (i = 0; i < 8; i++)
        printf("  %s %08X %08X%s\n", names[i], out_regs[i], post.regs[i],
               out_regs[i] != post.regs[i] ? "   <-- differs" : "");
    if (post.has_fpu && pre.has_fpu) {
        /* st(0) after the call: a float return value, or whatever the call
         * left on top. fnsave lays the registers out from st(0). */
        uint16_t sw, tw;
        long double nat;
        double lif = post.fpst[post.fptop];
        int top, phys, empty;
        memcpy(&sw, out_fpu + 4, 2);
        memcpy(&tw, out_fpu + 8, 2);
        top = (sw >> 11) & 7;
        phys = top;
        empty = ((tw >> (phys * 2)) & 3) == 3;
        memset(&nat, 0, sizeof nat);
        memcpy(&nat, out_fpu + 28, 10);
        if (!empty) {
            double a = (double)nat, d = a - lif;
            double m = (a < 0 ? -a : a) > (lif < 0 ? -lif : lif) ? (a < 0 ? -a : a) : (lif < 0 ? -lif : lif);
            int bad = !(a != a && lif != lif) && (d < 0 ? -d : d) > 1e-5 * (m > 1 ? m : 1);
            printf("  st0 %.9g %.9g%s\n", a, lif, bad ? "   <-- differs" : "");
        }
    }
    if (post.has_xmm) {
        /* Floats come back in xmm registers too; compare as floats so the
         * last-bit noise of a different evaluation order reads as noise. */
        for (i = 0; i < 8; i++) {
            int l, bad = 0;
            for (l = 0; l < 4; l++) {
                float a, b;
                memcpy(&a, &out_xmm[i * 4 + l], 4);
                memcpy(&b, &post.xmm[i * 4 + l], 4);
                if (a != b && !(a != a && b != b)) {
                    float d = a - b, m = (a < 0 ? -a : a) > (b < 0 ? -b : b) ? (a < 0 ? -a : a) : (b < 0 ? -b : b);
                    if ((d < 0 ? -d : d) > 1e-4f * (m > 1 ? m : 1))
                        bad = 1;
                }
            }
            if (bad) {
                float a[4], b[4];
                memcpy(a, &out_xmm[i * 4], 16);
                memcpy(b, &post.xmm[i * 4], 16);
                printf("  xmm%d native %g %g %g %g  lifted %g %g %g %g   <-- differs\n", i,
                       a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
            }
        }
    }

    for (i = 0; i < pre.n && i < post.n; i++) {
        const uint8_t *a = pre.r[i].data, *b = post.r[i].data;
        uint32_t sz = pre.r[i].size, off = 0;
        int shown = 0;
        const uint8_t *o = orig.r[i].data;
        while (off < sz) {
            uint32_t start, end, gap = 0, bytes = 0, k2;
            while (off < sz && a[off] == b[off]) off++;
            if (off >= sz) break;
            start = end = off;
            while (off < sz && gap < 64) {
                if (a[off] != b[off]) { end = off; gap = 0; bytes++; } else gap++;
                off++;
            }
            /* Who wrote it: a byte only the lifted run changed is usually
             * another guest thread at work during the lifted call. */
            {
                uint32_t nb = 0, nn = 0, nl = 0;
                for (k2 = start; k2 <= end; k2++) {
                    if (a[k2] == b[k2]) continue;
                    if (a[k2] != o[k2] && b[k2] != o[k2]) nb++;
                    else if (a[k2] != o[k2]) nn++;
                    else nl++;
                }
                both += nb; only_native += nn; only_lifted += nl;
                if (nb + nn == 0 && getenv("REPLAY_HIDE_LIFTED_ONLY")) { total += bytes; continue; }
            }
            total += bytes;
            if (shown++ < max_ranges) {
                uint32_t k;
                printf("  0x%08X..0x%08X  %6u bytes differ  native:",
                       pre.r[i].va + start, pre.r[i].va + end, bytes);
                for (k = start; k < start + 8 && k <= end; k++) printf(" %02X", a[k]);
                printf("  lifted:");
                for (k = start; k < start + 8 && k <= end; k++) printf(" %02X", b[k]);
                printf("\n");
            }
        }
    }
    printf("total differing bytes: %llu (both wrote: %llu, native only: %llu, "
           "lifted only: %llu)\n", total, both, only_native, only_lifted);
    printf("SIGNAL %llu\n", both + only_native);
    return 0;
}
