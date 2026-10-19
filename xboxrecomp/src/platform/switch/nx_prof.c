/*
 * nx_prof.c - a sampling profiler for the Switch build (RSC2_NX_PROF=1).
 *
 * The Linux one (a signal to a thread, which prints its backtrace) has no
 * equivalent here, and the Switch's profile is not the PC's: nouveau, the
 * slower CPU and the GL queue change where the time goes. So a thread of its
 * own pauses each registered thread every RSC2_NX_PROF_US microseconds
 * (1000 by default), reads its registers (svcGetThreadContext3), lets it go
 * on, and walks its frame records -- the build keeps frame pointers -- from
 * there. Counts are kept per address, module-relative, and every
 * RSC2_NX_PROF_SECS seconds (5) each thread's hottest are printed:
 *
 *   [PROF] pb 4870 samples (0 missed)
 *   [PROF] pb leaf 312 0x3086b54
 *   [PROF] pb in 4870 0x3085f20
 *
 * "leaf" is where the thread was, "in" any return address on its stack
 * (once per sample), "pair" a leaf with its caller: by lr (the leaf's
 * address as is) or by the first frame record (the address | 1).
 * rsc2/switch/nx_prof.py names them from rsc2.elf.
 *
 * Threads take part by calling nx_prof_register from themselves; which ones
 * are sampled is RSC2_NX_PROF_THREADS (names, comma-separated; "pb" by
 * default, the push-buffer executor; "gl" is the GL queue's thread).
 *
 * Apart from nx_host.c because <switch.h> clashes with win32_compat.h.
 */
#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u64 s_base;                      /* the module's: rsc2.elf's address 0 */

#define PROF_THREADS 4
#define PROF_SLOTS   8192               /* a power of two */
#define PROF_DEPTH   24
#define PROF_TOP_LEAF 60
#define PROF_TOP_IN   120

typedef struct { u32 off, n; } Slot;
typedef struct { u32 leaf, up, n; } Pair;
typedef struct {
    char name[16];
    Handle h;
    u32 samples, missed;
    Slot leaf[PROF_SLOTS], in[PROF_SLOTS];
    Pair pair[PROF_SLOTS];              /* where it was, and the first return address */
} ProfThread;

static ProfThread *s_th[PROF_THREADS];
static int s_nth;
static Mutex s_lock;
static Thread s_sampler;
static int s_started, s_on = -1;

static void slot_add(Slot *t, u32 off)
{
    u32 i = (off * 2654435761u) & (PROF_SLOTS - 1);
    int k;
    for (k = 0; k < 64; k++, i = (i + 1) & (PROF_SLOTS - 1)) {
        if (t[i].off == off) {
            t[i].n++;
            return;
        }
        if (!t[i].n) {
            t[i].off = off;
            t[i].n = 1;
            return;
        }
    }
}

static void pair_add(Pair *t, u32 leaf, u32 up)
{
    u32 i = ((leaf * 2654435761u) ^ (up * 40503u)) & (PROF_SLOTS - 1);
    int k;
    for (k = 0; k < 64; k++, i = (i + 1) & (PROF_SLOTS - 1)) {
        if (t[i].leaf == leaf && t[i].up == up) {
            t[i].n++;
            return;
        }
        if (!t[i].n) {
            t[i].leaf = leaf;
            t[i].up = up;
            t[i].n = 1;
            return;
        }
    }
}

static u32 rel(u64 a)
{
    u64 base = s_base;
    return a >= base && a - base < 0x80000000ull ? (u32)(a - base) : 0;
}

static void sample(ProfThread *t)
{
    ThreadContext ctx;
    MemoryInfo mi;
    u32 pi, seen[PROF_DEPTH + 2];
    int n = 0, i, j;
    u64 fp, lo, hi;

    if (R_FAILED(svcSetThreadActivity(t->h, ThreadActivity_Paused))) {
        t->missed++;
        return;
    }
    if (R_FAILED(svcGetThreadContext3(&ctx, t->h))) {
        svcSetThreadActivity(t->h, ThreadActivity_Runnable);
        t->missed++;
        return;
    }
    /* The frame records live in the thread's stack: walk them only inside
     * the mapping its sp is in. The thread stays paused meanwhile. */
    seen[n++] = rel(ctx.lr);
    fp = ctx.fp;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.sp)) && (mi.perm & Perm_R)) {
        lo = mi.addr;
        hi = mi.addr + mi.size;
        while (n < PROF_DEPTH && fp >= lo && fp + 16 <= hi && !(fp & 7)) {
            const u64 *rec = (const u64 *)(uintptr_t)fp;
            u64 next = rec[0];
            seen[n++] = rel(rec[1]);
            if (next <= fp)
                break;
            fp = next;
        }
    }
    svcSetThreadActivity(t->h, ThreadActivity_Runnable);
    t->samples++;
    slot_add(t->leaf, rel(ctx.pc.x));
    /* A leaf without a frame record (memcpy, a lock) was called from lr;
     * one with a record from its record's return address -- lr may be
     * stale there. The script decides which by the leaf's name. */
    pair_add(t->pair, rel(ctx.pc.x), seen[0] ? seen[0] - 4 : 0);
    if (n > 1 && seen[1])
        pair_add(t->pair, rel(ctx.pc.x) | 1u, seen[1] - 4);
    for (i = 0; i < n; i++) {
        if (!seen[i])
            continue;
        for (j = 0; j < i && seen[j] != seen[i]; j++)
            ;
        if (j == i)
            slot_add(t->in, seen[i] - 4);   /* the call, not the return */
    }
}

static int pair_by_count(const void *a, const void *b)
{
    const Pair *x = (const Pair *)a, *y = (const Pair *)b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

static int by_count(const void *a, const void *b)
{
    const Slot *x = (const Slot *)a, *y = (const Slot *)b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

static void report(ProfThread *t)
{
    static Slot sorted[PROF_SLOTS];
    int i;
    fprintf(stderr, "[PROF] %s %u samples (%u missed)\n", t->name, t->samples, t->missed);
    memcpy(sorted, t->leaf, sizeof sorted);
    qsort(sorted, PROF_SLOTS, sizeof *sorted, by_count);
    for (i = 0; i < PROF_TOP_LEAF && sorted[i].n; i++)
        fprintf(stderr, "[PROF] %s leaf %u %#x\n", t->name, sorted[i].n, sorted[i].off);
    memcpy(sorted, t->in, sizeof sorted);
    qsort(sorted, PROF_SLOTS, sizeof *sorted, by_count);
    for (i = 0; i < PROF_TOP_IN && sorted[i].n; i++)
        fprintf(stderr, "[PROF] %s in %u %#x\n", t->name, sorted[i].n, sorted[i].off);
    {
        static Pair ps[PROF_SLOTS];
        memcpy(ps, t->pair, sizeof ps);
        qsort(ps, PROF_SLOTS, sizeof *ps, pair_by_count);
        for (i = 0; i < PROF_TOP_IN && ps[i].n; i++)
            fprintf(stderr, "[PROF] %s pair %u %#x %#x\n", t->name, ps[i].n, ps[i].leaf, ps[i].up);
    }
    memset(t->pair, 0, sizeof t->pair);
    memset(t->leaf, 0, sizeof t->leaf);
    memset(t->in, 0, sizeof t->in);
    t->samples = t->missed = 0;
}

static void sampler(void *unused)
{
    const char *e = getenv("RSC2_NX_PROF_US"), *f = getenv("RSC2_NX_PROF_SECS");
    u64 every_ns = (u64)(e ? atoi(e) : 1000) * 1000;
    u64 report_ticks = armNsToTicks((u64)(f ? atoi(f) : 5) * 1000000000ull);
    u64 last = armGetSystemTick();
    int i;
    (void)unused;
    MemoryInfo mi;
    u32 pi;
    if (every_ns < 100000)
        every_ns = 100000;
    /* The mapping this code is in starts at the module's base (the .nro's
     * text is first). __start__ reads as 0 in this build. */
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&sampler)))
        s_base = mi.addr;
    fprintf(stderr, "[PROF] module base %#llx\n", (unsigned long long)s_base);
    for (;;) {
        svcSleepThread((s64)every_ns);
        mutexLock(&s_lock);
        for (i = 0; i < s_nth; i++)
            sample(s_th[i]);
        if (armGetSystemTick() - last >= report_ticks) {
            last = armGetSystemTick();
            for (i = 0; i < s_nth; i++)
                report(s_th[i]);
        }
        mutexUnlock(&s_lock);
    }
}

static int wanted(const char *name)
{
    const char *list = getenv("RSC2_NX_PROF_THREADS"), *p;
    size_t n = strlen(name);
    if (!list)
        list = "pb";
    for (p = list; *p; ) {
        const char *q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len == n && strncmp(p, name, n) == 0)
            return 1;
        if (!q)
            break;
        p = q + 1;
    }
    return 0;
}

/* From the thread to be sampled. */
void nx_prof_register(const char *name)
{
    ProfThread *t;
    if (s_on < 0) {
        const char *e = getenv("RSC2_NX_PROF");
        s_on = e && strcmp(e, "0") != 0;
    }
    if (!s_on || !wanted(name))
        return;
    t = (ProfThread *)calloc(1, sizeof *t);
    if (!t)
        return;
    snprintf(t->name, sizeof t->name, "%s", name);
    t->h = threadGetCurHandle();
    mutexLock(&s_lock);
    if (s_nth < PROF_THREADS)
        s_th[s_nth++] = t;
    if (!s_started) {
        s_started = 1;
        /* Core 3: the game's threads are on 0-2 (win32_compat.c). */
        if (R_SUCCEEDED(threadCreate(&s_sampler, sampler, NULL, NULL, 0x10000, 0x2B, 3))
                || R_SUCCEEDED(threadCreate(&s_sampler, sampler, NULL, NULL, 0x10000, 0x2B, -2)))
            threadStart(&s_sampler);
        fprintf(stderr, "[PROF] sampling started\n");
    }
    mutexUnlock(&s_lock);
    fprintf(stderr, "[PROF] thread %s sampled\n", name);
}
