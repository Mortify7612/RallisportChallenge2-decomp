/**
 * Function tracing for recompiled code.
 *
 * recomp_types.h declares these and tools.recomp emits calls to them under
 * --trace-functions, but until now nothing defined them: the definitions lived
 * in one game project, so enabling tracing anywhere else failed at link time
 * with three unresolved symbols and no hint that the fix was to go and copy a
 * file. They belong with the runtime that declares them.
 *
 * Output goes to stderr, unbuffered, because the question tracing answers is
 * usually "what was the last thing that happened before it died".
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include "xbox_memory_layout.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

/* A run that recurses produces trace lines without limit, and the useful
 * window is rarely the first few thousand. The budget stops a diagnostic from
 * filling a disk, and is deliberately generous: a budget that runs out before
 * the interesting part turns "no trace here" into a false negative, which is
 * worse than a large file. Override with RECOMP_TRACE_BUDGET. */
static long trace_budget(void)
{
    static long budget = -1;
    if (budget < 0) {
        const char *env = getenv("RECOMP_TRACE_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 400000;
        if (budget < 0) budget = 0;
    }
    return budget > 0 ? budget-- : 0;
}

/* Where a title spends its calls.
 *
 * A recompiled title that is CPU-bound gives no clue which guest code is
 * responsible: the native profile is a wall of sub_XXXX, and the guest has no
 * program counter to sample. Counting entries does answer it, and the trace
 * hook is already on every function the run was generated to trace -- so
 * tracing everything and tallying instead of printing turns the existing
 * mechanism into a profile for the cost of an array increment.
 *
 * Counts, not time: a function called once that loops for a minute does not
 * appear here, and one called ten million times cheaply does. It says where
 * the calls go, which is the first question, not the last.
 *
 * Enable with RECOMP_TRACE_PROFILE=1. The report goes to stderr at exit,
 * hottest first.
 */
#define PROF_SLOTS 8192                 /* open addressing, power of two */

static struct { uint32_t va; unsigned long long hits; } g_prof[PROF_SLOTS];
static const char *g_prof_name[PROF_SLOTS];
static int g_prof_used, g_prof_full;
static unsigned long long g_prof_calls;

static void prof_report(void)
{
    int taken[40], ntaken = 0;
    int i, j, shown;

    if (!g_prof_used)
        return;
    fprintf(stderr, "\n[PROFILE] %d functions entered%s, hottest first:\n",
            g_prof_used, g_prof_full ? " (table full, some dropped)" : "");
    for (shown = 0; shown < 40; shown++) {
        int best = -1;
        for (i = 0; i < PROF_SLOTS; i++) {
            if (!g_prof[i].hits)
                continue;
            for (j = 0; j < ntaken; j++)
                if (taken[j] == i)
                    break;
            if (j < ntaken)
                continue;
            if (best < 0 || g_prof[i].hits > g_prof[best].hits)
                best = i;
        }
        if (best < 0)
            break;
        taken[ntaken++] = best;
        fprintf(stderr, "  %14llu  %s (0x%08X)\n",
                g_prof[best].hits, g_prof_name[best] ? g_prof_name[best] : "?",
                g_prof[best].va);
    }
    fflush(stderr);
}

/* RECOMP_TRACE_PROFILE=1 profiles; a larger number is also how often to
 * report, in calls. The default suits a title burning a core in a spin loop;
 * a title that no longer has one may never reach it, and then the only report
 * is the one at exit -- which a killed run never gets. */
static unsigned long long prof_interval(void)
{
    static unsigned long long every;
    if (!every) {
        const char *v = getenv("RECOMP_TRACE_PROFILE");
        unsigned long long n = v ? strtoull(v, NULL, 0) : 0;
        every = n > 1 ? n : 20000000ull;
    }
    return every;
}

static int prof_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("RECOMP_TRACE_PROFILE") ? 1 : 0;
        if (on)
            atexit(prof_report);
    }
    return on;
}

static void prof_count(const char *name, uint32_t va)
{
    unsigned i = (va * 2654435761u) & (PROF_SLOTS - 1);
    unsigned n;

    /* A title being profiled for a hang or a slowdown is a title that gets
     * killed rather than exited, and a kill does not reach atexit. Report as
     * it goes, so there is always a recent one. */
    if (++g_prof_calls % prof_interval() == 0)
        prof_report();

    for (n = 0; n < PROF_SLOTS; n++) {
        unsigned k = (i + n) & (PROF_SLOTS - 1);
        if (g_prof[k].va == va && g_prof[k].hits) { g_prof[k].hits++; return; }
        if (!g_prof[k].hits) {
            g_prof[k].va = va;
            g_prof[k].hits = 1;
            g_prof_name[k] = name;
            g_prof_used++;
            return;
        }
    }
    g_prof_full = 1;
}

/* ---------------------------------------------------------------------------
 * Differential snapshots: the state a lifted function started from and the
 * state it left, so the original x86 can be replayed from the same start and
 * the two compared.
 *
 * The host is an x86 too, so the original instructions are the reference
 * implementation -- run natively in a 32-bit process (tools/replay32 in the
 * game project) they give exactly what the lifted C should have computed. A
 * function whose results differ contains the bug; snapshotting its callees
 * the same way narrows it to one.
 *
 *   RECOMP_SNAPSHOT=<va>[:<n>]   the n-th call (default 1) of a function
 *                                that tools.recomp --trace-functions lists
 *   RECOMP_SNAPSHOT_DIR=<dir>    where pre.bin and post.bin go (default .)
 *
 * File format: "XRSNAP01", then eight dwords eax ecx edx ebx esp ebp esi edi,
 * fs base, region count, and per region { u32 va, u32 size, bytes }. Regions
 * are guest RAM (0x00010000-0x04000000) and the contiguous window. Other
 * guest threads keep running in between, so their writes show up in the diff
 * as well; the replay tool reports clustered ranges for that reason.
 * ------------------------------------------------------------------------- */
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */

extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
/* The lifted code's SSE register file (RecompXmm, 16 bytes each). */
typedef struct { uint32_t u[4]; } SnapXmm;
extern RECOMP_TLS SnapXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3,
                          g_xmm4, g_xmm5, g_xmm6, g_xmm7;

static uint32_t s_snap_va, s_snap_nth = 1, s_snap_calls, s_snap_esp;
static int s_snap_state = -1;   /* -1 unread, 0 off/done, 1 armed, 2 in call */

static void snap_write_to(const char *which, uint32_t va, const char *dirp);
static void snap_write(const char *which)
{
    snap_write_to(which, s_snap_va, getenv("RECOMP_SNAPSHOT_DIR"));
}

static void snap_write_to(const char *which, uint32_t snap_va, const char *dirp)
{
    /* The low page holds the fake TIBs/KPCR fs: points at (0x4000 for the
     * first thread). A host cannot map below vm.mmap_min_addr, so the replay
     * tool relocates that region and moves the fs base with it. */
    static const struct { uint32_t va, size; } regions[] = {
        { 0x00010000u, 0x04000000u - 0x00010000u },
        { XBOX_CONTIG_BASE, XBOX_CONTIG_SIZE },
        { 0x00001000u, 0x0000F000u },
    };
    const char *dir = dirp;
    char path[1024];
    uint32_t hdr[10], n = 3, i;
    FILE *f;

    snprintf(path, sizeof path, "%s/%s.bin", dir ? dir : ".", which);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[SNAP] cannot write %s\n", path);
        return;
    }
    hdr[0] = g_eax; hdr[1] = g_ecx; hdr[2] = g_edx; hdr[3] = g_ebx;
    hdr[4] = g_esp; hdr[5] = g_ebp; hdr[6] = g_esi; hdr[7] = g_edi;
    hdr[8] = XBOX_FS_BASE; hdr[9] = n;
    /* XRSNAP02: the eight SSE registers follow the header -- the lifted code
     * passes floats in them between functions, so a replay without them
     * starts the native run from garbage.
     * XRSNAP03 adds the x87 state after them: control word, top index and
     * the eight-entry ring in its physical order. Floats are passed and
     * returned in st(0) too, and without them every such function replayed
     * against an empty FPU (_ftol came back 0x80000000). */
    fwrite("XRSNAP03", 1, 8, f);
    fwrite(hdr, 4, 10, f);
    fwrite(&g_xmm0, 16, 1, f); fwrite(&g_xmm1, 16, 1, f);
    fwrite(&g_xmm2, 16, 1, f); fwrite(&g_xmm3, 16, 1, f);
    fwrite(&g_xmm4, 16, 1, f); fwrite(&g_xmm5, 16, 1, f);
    fwrite(&g_xmm6, 16, 1, f); fwrite(&g_xmm7, 16, 1, f);
    {
        uint32_t fpu[2];
        fpu[0] = g_fp_control_word;
        fpu[1] = (uint32_t)g_fp_top & 7u;
        fwrite(fpu, 4, 2, f);
        fwrite(g_fp_stack, 8, 8, f);
    }
    for (i = 0; i < n; i++) {
        fwrite(&regions[i].va, 4, 1, f);
        fwrite(&regions[i].size, 4, 1, f);
        fwrite((const void *)((uintptr_t)regions[i].va + xbox_GetMemoryOffset()),
               1, regions[i].size, f);
    }
    fclose(f);
    fprintf(stderr, "[SNAP] %s: 0x%08X esp=%08X -> %s\n",
            which, snap_va, g_esp, path);
}

/* Block trace during the snapshotted call: {va, eax ecx edx ebx esp ebp esi
 * edi} per labelled block of the traced functions, on the snapshot thread
 * only. RECOMP_SNAPSHOT_BB=1 turns it on; it goes to bb.bin beside pre.bin. */
static FILE *s_bb_file;
static RECOMP_TLS int s_bb_thread;

void recomp_trace_bb(uint32_t va, uint32_t ebp)
{
    uint32_t rec[9];
    if (!s_bb_file || !s_bb_thread)
        return;
    rec[0] = va;
    rec[1] = g_eax; rec[2] = g_ecx; rec[3] = g_edx; rec[4] = g_ebx;
    rec[5] = g_esp; rec[6] = ebp;   rec[7] = g_esi; rec[8] = g_edi;
    fwrite(rec, 4, 9, s_bb_file);
    fflush(s_bb_file);   /* the call being traced may well be the one that crashes */
}

static void snap_enter(uint32_t va)
{
    if (s_snap_state < 0) {
        const char *e = getenv("RECOMP_SNAPSHOT");
        s_snap_state = 0;
        if (e && *e) {
            char *end;
            s_snap_va = (uint32_t)strtoul(e, &end, 16);
            if (*end == ':')
                s_snap_nth = (uint32_t)strtoul(end + 1, NULL, 0);
            s_snap_state = 1;
        }
    }
    if (s_snap_state != 1 || va != s_snap_va)
        return;
    if (++s_snap_calls != s_snap_nth)
        return;
    s_snap_esp = g_esp;
    snap_write("pre");
    s_snap_state = 2;
    if (getenv("RECOMP_SNAPSHOT_BB")) {
        const char *dir = getenv("RECOMP_SNAPSHOT_DIR");
        char path[1024];
        snprintf(path, sizeof path, "%s/bb.bin", dir ? dir : ".");
        s_bb_file = fopen(path, "wb");
        s_bb_thread = 1;
    }
}

static void snap_exit(uint32_t va)
{
    /* The matching return. The exit hook runs before the ret, so esp is back
     * at the return address -- equal to the entry's -- while a recursive inner
     * call returns with esp still below it. (So post.bin's esp is the entry
     * esp; the native run's is past the ret, by 4 plus any stdcall bytes.) */
    if (s_snap_state != 2 || va != s_snap_va || g_esp < s_snap_esp)
        return;
    snap_write("post");
    s_snap_state = 0;
    if (s_bb_file) {
        fclose(s_bb_file);
        s_bb_file = NULL;
        s_bb_thread = 0;
    }
}

/* Batch mode: RECOMP_SNAPSHOT_LIST=<file of hex VAs> snapshots the first
 * call of each listed function after RECOMP_SNAPSHOT_AFTER_MS, into
 * <RECOMP_SNAPSHOT_DIR>/<VA>/{pre,post}.bin -- one run to collect the cases a
 * differential sweep over many functions needs. */
#define SNAPB_MAX 4096
static struct { uint32_t va, esp; int state; unsigned tid; } s_snapb[SNAPB_MAX];
static int s_snapb_n = -1;
static unsigned long s_snapb_after;
static RECOMP_TLS unsigned s_snapb_tid;
static unsigned s_snapb_tids;
static SRWLOCK s_snapb_lock = SRWLOCK_INIT;

static int snapb_find(uint32_t va)
{
    int lo = 0, hi = s_snapb_n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (s_snapb[m].va == va) return m;
        if (s_snapb[m].va < va) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

static int snapb_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void snapb_init(void)
{
    const char *e = getenv("RECOMP_SNAPSHOT_LIST");
    FILE *f;
    char line[64];
    s_snapb_n = 0;
    if (!e || !(f = fopen(e, "r")))
        return;
    while (s_snapb_n < SNAPB_MAX && fgets(line, sizeof line, f))
        if (line[0] && line[0] != '\n')
            s_snapb[s_snapb_n++].va = (uint32_t)strtoul(line, NULL, 16);
    fclose(f);
    qsort(s_snapb, s_snapb_n, sizeof s_snapb[0], snapb_cmp);
    e = getenv("RECOMP_SNAPSHOT_AFTER_MS");
    s_snapb_after = e ? strtoul(e, NULL, 0) : 0;
    s_snapb_after += GetTickCount();
    fprintf(stderr, "[SNAP] batch: %d functions\n", s_snapb_n);
}

static void snapb_dir(char *buf, size_t n, uint32_t va)
{
    const char *dir = getenv("RECOMP_SNAPSHOT_DIR");
    snprintf(buf, n, "%s/%08X", dir ? dir : ".", va);
#ifdef _WIN32
    CreateDirectoryA(buf, NULL);
#else
    mkdir(buf, 0755);
#endif
}

static void snapb_enter(uint32_t va)
{
    int i;
    char d[1024];
    if (s_snapb_n < 0)
        snapb_init();
    if (!s_snapb_n || (i = snapb_find(va)) < 0 || s_snapb[i].state != 0)
        return;
    if (GetTickCount() < s_snapb_after)
        return;
    if (!s_snapb_tid)
        s_snapb_tid = __atomic_add_fetch(&s_snapb_tids, 1, __ATOMIC_SEQ_CST);
    AcquireSRWLockExclusive(&s_snapb_lock);
    if (s_snapb[i].state == 0) {
        s_snapb[i].state = 1;
        s_snapb[i].esp = g_esp;
        s_snapb[i].tid = s_snapb_tid;
        snapb_dir(d, sizeof d, va);
        snap_write_to("pre", va, d);
        if (getenv("RECOMP_SNAPSHOT_BB") && !s_bb_file) {
            char path[1100];
            snprintf(path, sizeof path, "%s/bb.bin", d);
            s_bb_file = fopen(path, "wb");
            s_bb_thread = 1;
        }
    }
    ReleaseSRWLockExclusive(&s_snapb_lock);
}

static void snapb_exit(uint32_t va)
{
    int i;
    char d[1024];
    if (s_snapb_n <= 0 || (i = snapb_find(va)) < 0 || s_snapb[i].state != 1)
        return;
    if (s_snapb[i].tid != s_snapb_tid || g_esp < s_snapb[i].esp)
        return;
    AcquireSRWLockExclusive(&s_snapb_lock);
    s_snapb[i].state = 2;
    snapb_dir(d, sizeof d, va);
    snap_write_to("post", va, d);
    if (s_bb_file && s_bb_thread) {
        fclose(s_bb_file);
        s_bb_file = NULL;
        s_bb_thread = 0;
    }
    ReleaseSRWLockExclusive(&s_snapb_lock);
}

/* RECOMP_TREE_TRACE=<file>: RalliSport Challenge 2's object culling, every
 * frame, for chasing objects that drop out (tree pop-in). Binary records:
 *   u32 'TTF0', u32 frame, u32 kind, u32 n, u32 extra, then n entries of
 *   { u32 inst, f32 dist, f32 weight, u8 lod, u8 type, u16 pad }
 * kind 1: sub_0010F0D0's output as sub_00273E70 walks it -- 24-byte
 *         records at the renderer object's +0x10 (its 6th argument), the
 *         count at +0xC (extra = the count slot, 5th argument);
 * kind 2: the render buckets at sub_00273890's entry (type = record type);
 * kinds 5-8: sub_00273E70's four lists (obj+0x22A60, +0x2125C, +0x1B258,
 *         +0x12254), as it returns;
 * kind 9: a mark (F9 in the window; extra = the mark count).
 * Needs trace hooks on 0x0010F0D0, 0x00273360 and 0x00273890. */
volatile int g_recomp_user_mark;
static FILE *s_tt;
static int s_tt_on = -1;
static uint32_t s_tt_out, s_tt_cnt, s_tt_a3, s_tt_a4, s_tt_obj;

static void tt_hdr(uint32_t kind, uint32_t n, uint32_t extra)
{
    extern uint32_t nv2a_pb_presented_frames(void);
    uint32_t h[5];
    h[0] = 0x30465454u;     /* "TTF0" */
    h[1] = nv2a_pb_presented_frames();
    h[2] = kind;
    h[3] = n;
    h[4] = extra;
    fwrite(h, 4, 5, s_tt);
}

static void tt_entry(const uint8_t *m, uint32_t rec, uint32_t type)
{
    struct { uint32_t inst; float dist, w; uint8_t lod, type; uint16_t pad; } e;
    e.inst = *(const uint32_t *)(m + rec);
    memcpy(&e.dist, m + rec + 4, 4);
    memcpy(&e.w, m + rec + 0x14, 4);
    e.lod = m[rec + 8];
    e.type = (uint8_t)type;
    e.pad = (uint16_t)(m[rec + 0x12] | m[rec + 9] << 8);   /* byte 0x12, byte 9 */
    fwrite(&e, sizeof e, 1, s_tt);
}

static void tree_trace(uint32_t va)
{
    const uint8_t *m;
    static int marks;
    if (s_tt_on < 0) {
        const char *f = getenv("RECOMP_TREE_TRACE");
        s_tt = f && *f ? fopen(f, "wb") : NULL;
        s_tt_on = s_tt != NULL;
    }
    if (!s_tt_on)
        return;
    m = (const uint8_t *)xbox_GetMemoryOffset();
    if (g_recomp_user_mark != marks) {
        marks = g_recomp_user_mark;
        tt_hdr(9, 0, (uint32_t)marks);
        fprintf(stderr, "[TREETRACE] mark %d\n", marks);
    }
    if (va == 0x0010F0D0u) {
        const uint32_t *sp = (const uint32_t *)(m + g_esp);
        if (sp[0] != 0x002743D1u)            /* only sub_00273E70's call */
            return;
        s_tt_out = sp[6];       /* records: the renderer object's +0x10 */
        s_tt_a3 = sp[3];
        s_tt_a4 = sp[4];
        s_tt_cnt = sp[5];
        s_tt_obj = g_esi;       /* sub_00273E70 keeps the count at esi+0xc */
    } else if (va == 0x00273360u && s_tt_cnt) {
        uint32_t n = *(const uint32_t *)(m + s_tt_obj + 0xC), i;
        tt_hdr(3, 0, s_tt_a3);
        tt_hdr(4, 0, s_tt_a4);
        tt_hdr(1, n > 4096 ? 4096 : n, *(const uint32_t *)(m + s_tt_cnt));
        for (i = 0; i < n && i < 4096; i++)
            tt_entry(m, s_tt_out + 24 * i, 0);
        {
            /* kinds 5-8: the four lists sub_00273E70 sorts the records
             * into (count at -4, 24-byte records). */
            static const uint32_t lists[4] = { 0x22A60u, 0x2125Cu, 0x1B258u, 0x12254u };
            int l;
            for (l = 0; l < 4; l++) {
                uint32_t base = s_tt_obj + lists[l], c = *(const uint32_t *)(m + base - 4);
                if (c > 4096)
                    c = 0;
                tt_hdr(5 + (uint32_t)l, c, lists[l]);
                for (i = 0; i < c; i++)
                    tt_entry(m, base + 24 * i, (uint32_t)l);
            }
        }
        s_tt_cnt = 0;
    } else if (va == 0x00273890u) {
        uint32_t obj = g_eax, nb = *(const uint32_t *)(m + obj + 8), i, total = 0;
        for (i = 0; i < nb && i < 256; i++) {
            uint32_t b = obj + 24 * i + 24;
            uint32_t beg = *(const uint32_t *)(m + b), end = *(const uint32_t *)(m + b + 4);
            if (beg && end > beg)
                total += (end - beg) / 12;
        }
        tt_hdr(2, total, obj);
        for (i = 0; i < nb && i < 256; i++) {
            uint32_t b = obj + 24 * i + 24, it;
            uint32_t beg = *(const uint32_t *)(m + b), end = *(const uint32_t *)(m + b + 4);
            for (it = beg; beg && it + 12 <= end; it += 12)
                tt_entry(m, *(const uint32_t *)(m + it), *(const uint32_t *)(m + it + 4));
        }
        fflush(s_tt);
    }
}

void recomp_trace_enter(const char *name, uint32_t va)
{
    tree_trace(va);
    /* RECOMP_DUMP_BUCKETS: RalliSport Challenge 2's render buckets at the
     * entry of sub_00273890 (eax = renderer): bucket i at eax+24*i+24 holds
     * [begin, end) of 12-byte items, each pointing at a 24-byte record. */
    if (va == 0x00273890u && getenv("RECOMP_DUMP_BUCKETS")) {
        static int shown;
        const uint8_t *m = (const uint8_t *)xbox_GetMemoryOffset();
        if (shown++ < 3) {
            uint32_t obj = g_eax, n = *(const uint32_t *)(m + obj + 8), i, k;
            fprintf(stderr, "[BUCKETS] obj %08X n %u\n", obj, n);
            for (i = 0; i < n && i < 12; i++) {
                uint32_t b = obj + 24 * i + 24;
                uint32_t beg = *(const uint32_t *)(m + b), end = *(const uint32_t *)(m + b + 4);
                fprintf(stderr, "[BUCKETS]  b%u at %08X items %08X..%08X (%u) key %08X\n", i, b, beg, end,
                        end > beg ? (end - beg) / 12 : 0, *(const uint32_t *)(m + b - 8));
                for (k = 0; beg && k < 3 && beg + k * 12 < end; k++) {
                    uint32_t it = beg + k * 12, rec = *(const uint32_t *)(m + it);
                    float d, w;
                    memcpy(&d, m + rec + 4, 4);
                    memcpy(&w, m + rec + 0x14, 4);
                    fprintf(stderr, "[BUCKETS]    item %08X rec %08X type %u  rec: inst %08X dist %g weight %g\n",
                            it, rec, *(const uint32_t *)(m + it + 4), *(const uint32_t *)(m + rec), d, w);
                }
            }
        }
    }
    snapb_enter(va);
    if (s_snapb_n > 0) return;
    snap_enter(va);
    if (s_snap_state >= 0 && getenv("RECOMP_SNAPSHOT")) return;   /* snapshot runs are quiet */
    if (prof_enabled()) { prof_count(name, va); return; }
    if (!trace_budget()) return;
    /* The return address as well as the registers: at entry it is still at
     * [esp], and it names the call site, which is the thing a trace of "who
     * reached this" actually needs. Reading a guest stack dump for it works
     * only when the frames above are still live. */
    {
        /* RECOMP_TRACE_FRAMES: prefix the presented frame, so calls can be
         * counted per frame. */
        static int fr = -1;
        extern uint32_t nv2a_pb_presented_frames(void);
        if (fr < 0)
            fr = getenv("RECOMP_TRACE_FRAMES") != NULL;
        if (fr)
            fprintf(stderr, "[F%u] ", nv2a_pb_presented_frames());
    }
    fprintf(stderr, "[TRACE] -> %s (0x%08X)  from=%08X esp=%08X eax=%08X "
            "ecx=%08X esi=%08X edi=%08X ebx=%08X\n",
            name, va,
            *(const uint32_t *)((uintptr_t)g_esp + xbox_GetMemoryOffset()),
            g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);

    /* RECOMP_TRACE_STACK=<n>: the first n values on the guest stack that
     * look like code addresses -- the call chain, innermost first. */
    {
        static long nstack = -2;
        if (nstack == -2) {
            const char *e = getenv("RECOMP_TRACE_STACK");
            nstack = e ? atol(e) : 0;
        }
        if (nstack > 0) {
            const uint32_t *sp = (const uint32_t *)((uintptr_t)g_esp + xbox_GetMemoryOffset());
            int i, shown = 0;
            fprintf(stderr, "         stack:");
            for (i = 1; i < 512 && shown < nstack; i++) {
                uint32_t v = sp[i];
                if (v >= 0x00011000u && v < 0x00380000u) {
                    fprintf(stderr, " %08X", v);
                    shown++;
                }
            }
            fprintf(stderr, "\n");
        }
    }
    /* The stack arguments too, when asked. Registers alone do not say which
     * argument arrived null, and for a function with a long argument list,
     * counting pushes back from the call site is guesswork. */
    if (getenv("RECOMP_TRACE_ARGS")) {
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        int n = atoi(getenv("RECOMP_TRACE_ARGS"));
        int i;

        if (n <= 0 || n > 32)
            n = 8;
        fprintf(stderr, "         args:");
        for (i = 1; i <= n; i++)
            fprintf(stderr, " %d=%08X", i,
                    *(const uint32_t *)(mem + g_esp + i * 4));
        fprintf(stderr, "\n");
        /* And the object eax points at. A matrix of NaNs says the maths went
         * wrong; whether its inputs were already zero says whether the maths
         * is at fault or the data behind it was never built. */
        /* Follow the pointer arguments one level. A matrix that arrives as
         * NaN was copied from somewhere, and the object it came from is what
         * needs looking at -- the value alone says only that it is wrong. */
        if (getenv("RECOMP_TRACE_DEREF")) {
            for (i = 1; i <= n; i++) {
                uint32_t a = *(const uint32_t *)(mem + g_esp + i * 4);
                int k;
                if (a < 0x00010000u || a >= 0x04000000u)
                    continue;
                fprintf(stderr, "         arg%d -> [%08X]:", i, a);
                for (k = 0; k < 12; k++)
                    fprintf(stderr, " %08X",
                            *(const uint32_t *)(mem + a + k * 4));
                fprintf(stderr, "\n");
            }
        }
        if (g_eax > 0x00010000u && g_eax < 0x04000000u) {
            fprintf(stderr, "         [eax=%08X]:", g_eax);
            for (i = 0; i < 24; i++)
                fprintf(stderr, " %08X",
                        *(const uint32_t *)(mem + g_eax + i * 4));
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
}

/* Entry values answer "what was it called with"; only exit values answer "what
 * did the caller get back", which is the question when a callee-saved register
 * comes back wrong. */
void recomp_trace_exit(const char *name, uint32_t va)
{
    /* RECOMP_DUMP_VIEWDIST: RalliSport Challenge 2's per-track-position
     * view-distance keyframes, after sub_0026EF60 has filled the gaps. ebx
     * holds the array object: records of 0x1E8 bytes from [ebx+8] to
     * [ebx+0xC], four groups of fifteen settings from offset 0xF8. */
    if (va == 0x0026EF60u && getenv("RECOMP_DUMP_VIEWDIST")) {
        const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
        uint32_t b = *(const uint32_t *)(mem + g_ebx + 8), e = *(const uint32_t *)(mem + g_ebx + 0xC);
        uint32_t n = e > b ? (e - b) / 0x1E8u : 0, r, g, k;
        fprintf(stderr, "[VIEWDIST] %u records at %08X\n", n, b);
        for (r = 0; r < n && r < 400; r++) {
            for (g = 0; g < 4; g++) {
                fprintf(stderr, "[VIEWDIST] r%u g%u", r, g);
                for (k = 0; k < 15; k++) {
                    float f;
                    memcpy(&f, mem + b + r * 0x1E8u + 0xF8u + (g * 15u + k) * 4u, 4);
                    fprintf(stderr, " %g", f);
                }
                fputc('\n', stderr);
            }
        }
    }
    snapb_exit(va);
    if (s_snapb_n > 0) return;
    snap_exit(va);
    if (getenv("RECOMP_SNAPSHOT")) return;
    if (prof_enabled()) return;     /* entries alone carry the count */
    if (!trace_budget()) return;
    fprintf(stderr, "[TRACE] <- %s (0x%08X)  esp=%08X eax=%08X ecx=%08X "
            "esi=%08X edi=%08X ebx=%08X\n",
            name, va, g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebx);
    fflush(stderr);
}

/* esp at a specific point inside a traced function. The epilogue's
 * `mov esp, ebp` hides drift from any return-time sample, so a leak of a few
 * bytes per call is only visible from a sample taken before it. */
void recomp_trace_esp(const char *name, const char *tag)
{
    if (prof_enabled()) return;
    if (!trace_budget()) return;
    fprintf(stderr, "[ESP] %s @%s  esp=%08X esi=%08X edi=%08X\n",
            name, tag, g_esp, g_esi, g_edi);
    fflush(stderr);
}

/* ---------------------------------------------------------------------------
 * Guest debug output (INT 2D / DebugService).
 *
 * The Xbox kernel debug trap. eax selects the service and ecx carries its
 * argument; service 1 is "print this ANSI_STRING", which is what
 * OutputDebugStringA and the XDK's DbgPrint compile down to. On hardware the
 * kernel consumes the trap and resumes at the int3 that follows, skipping it.
 *
 * Printing it is the whole point: this is the title telling us what it thinks
 * is happening, and during bring-up that is the most valuable output there is.
 * ------------------------------------------------------------------------- */

/* Defined in xbox_memory_layout.c; declared extern per consumer, as
 * kernel_bridge.c and nv2a_pb_replay.c already do. */
extern ptrdiff_t g_xbox_mem_offset;

void recomp_debug_service(uint32_t service, uint32_t arg_va)
{
    const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
    uint16_t length;
    uint32_t buffer_va;

    if (service != 1) {
        fprintf(stderr, "[GUEST] DebugService %u (arg 0x%08X), ignored\n",
                (unsigned)service, arg_va);
        fflush(stderr);
        return;
    }

    /* ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; } */
    if (!arg_va)
        return;
    length    = *(const uint16_t *)(mem + arg_va);
    buffer_va = *(const uint32_t *)(mem + arg_va + 4);
    if (!buffer_va || !length)
        return;

    fprintf(stderr, "[GUEST] %.*s", (int)length, (const char *)(mem + buffer_va));
    if (length && ((const char *)(mem + buffer_va))[length - 1] != '\n')
        fputc('\n', stderr);
    fflush(stderr);
}
