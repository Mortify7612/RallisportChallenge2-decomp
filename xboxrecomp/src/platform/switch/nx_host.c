/*
 * nx_host.c - what the POSIX platform layer takes from the OS elsewhere,
 * on Nintendo Switch homebrew (libnx): memory mapping, the fault path the
 * device models trap through, and a few libc gaps.
 *
 * Memory. Guest addresses are host addresses at a fixed offset, so the
 * guest's address space has to be one host range with each window (RAM,
 * the contiguous window, the device apertures) at its own guest address.
 * Horizon has no mmap, and the only place an unprivileged process may map
 * memory at an address of its choosing is its stack region (svcMapMemory,
 * which moves heap pages there). That region is under 2 GB, so the Switch
 * build folds the guest's top half down (platform/guest_addr.h), which
 * needs 0x44000000 bytes. A large anonymous mmap is then a slot of the stack
 * region with nothing behind it (a reservation, as PROT_NONE is elsewhere),
 * and fixed mappings and commits inside it are heap memory moved there.
 * Small anonymous mappings come from the heap. There is no aliasing: a
 * second view of one memfd fails. (svcMapPhysicalMemory into the alias
 * region would place memory too, but needs a system resource budget, which
 * homebrew does not reliably have -- RSC2_NX_PROBE=1 shows what a host
 * allows.)
 *
 * Faults. The APU, AC'97, USB and NV2A timer models trap the title's
 * accesses to their registers and emulate the instruction (mmio_decode.h).
 * Horizon delivers a fault to the process's entry with the faulting
 * registers split between a context record (x0-x8, lr, sp, pc, the
 * syndrome) and the live registers; __libnx_exception_entry below saves the
 * live ones, runs the vectored handlers on a CONTEXT built from both, writes
 * the result back and resumes with svcReturnFromException. The kernel
 * handles one user exception per process at a time, so one stack serves.
 */
#if defined(__SWITCH__)

#include <switch/types.h>
#include <switch/result.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/virtmem.h>
#include <switch/runtime/env.h>

#include "win32_compat.h"

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/iosupport.h>

#define NX_PAGE 0x1000ull

#ifndef EXCEPTION_BREAKPOINT
#define EXCEPTION_BREAKPOINT 0x80000003u
#endif

/* Anonymous mappings this large are reservations in the stack region. */
#define NX_SPAN_MIN (256ull << 20)

static pthread_mutex_t s_vm_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uint64_t pg_down(uint64_t v) { return v & ~(NX_PAGE - 1); }
static inline uint64_t pg_up(uint64_t v)   { return (v + NX_PAGE - 1) & ~(NX_PAGE - 1); }

/* ---------------------------------------------------------------------- */
/* Spans: reservations in the stack region                                 */
/* ---------------------------------------------------------------------- */

#define NX_MAX_SPANS 8
static struct { uint64_t lo, hi; } s_span[NX_MAX_SPANS];
static int s_nspan;

static int in_span(uint64_t a, uint64_t len)
{
    int i;
    for (i = 0; i < s_nspan; i++)
        if (a >= s_span[i].lo && a + len <= s_span[i].hi)
            return 1;
    return 0;
}

/* RSC2_NX_PROBE=1 (rsc2.env): log the address regions and which mapping
 * syscalls this process may use, trying each on a page at `at`; with
 * RSC2_NX_PROBE=2, also whether a fault can be resumed (a host that cannot,
 * like an emulator without user exception handlers, ends the process). */
static LONG CALLBACK probe_veh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->X[0] = 0x600DF00Dull;
    ep->ContextRecord->Pc += 4;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void nx_probe(uint64_t at)
{
    static const struct { u32 id; const char *name; } info[] = {
        { InfoType_AliasRegionAddress, "alias" }, { InfoType_AliasRegionSize, "alias size" },
        { InfoType_HeapRegionAddress, "heap" }, { InfoType_HeapRegionSize, "heap size" },
        { InfoType_AslrRegionAddress, "aslr" }, { InfoType_AslrRegionSize, "aslr size" },
        { InfoType_StackRegionAddress, "stack" }, { InfoType_StackRegionSize, "stack size" },
        { InfoType_SystemResourceSizeTotal, "system resource" },
        { InfoType_TotalMemorySize, "total memory" }, { InfoType_UsedMemorySize, "used memory" },
    };
    const char *e = getenv("RSC2_NX_PROBE");
    size_t i;
    void *src;
    Result rc;
    Handle h;

    if (!e || !*e || *e == '0')
        return;
    for (i = 0; i < sizeof info / sizeof info[0]; i++) {
        uint64_t v = 0;
        rc = svcGetInfo(&v, info[i].id, CUR_PROCESS_HANDLE, 0);
        fprintf(stderr, "[nx probe] %-16s %#lx%s\n", info[i].name, (unsigned long)v,
                R_FAILED(rc) ? " (failed)" : "");
    }
    fprintf(stderr, "[nx probe] syscall hints: MapPhysicalMemory %d, CreateCodeMemory %d,"
            " ControlCodeMemory %d, MapProcessMemory %d\n", envIsSyscallHinted(0x2C),
            envIsSyscallHinted(0x4B), envIsSyscallHinted(0x4C), envIsSyscallHinted(0x74));

    rc = svcMapPhysicalMemory((void *)(uintptr_t)at, NX_PAGE);
    fprintf(stderr, "[nx probe] svcMapPhysicalMemory at %#lx: 0x%x\n", (unsigned long)at, rc);
    if (R_SUCCEEDED(rc))
        svcUnmapPhysicalMemory((void *)(uintptr_t)at, NX_PAGE);

    src = memalign(NX_PAGE, NX_PAGE);
    rc = svcCreateCodeMemory(&h, src, NX_PAGE);
    fprintf(stderr, "[nx probe] svcCreateCodeMemory: 0x%x\n", rc);
    if (R_SUCCEEDED(rc)) {
        rc = svcControlCodeMemory(h, CodeMapOperation_MapOwner, (void *)(uintptr_t)at, NX_PAGE, Perm_Rw);
        fprintf(stderr, "[nx probe] code memory owner map at %#lx: 0x%x\n", (unsigned long)at, rc);
        if (R_SUCCEEDED(rc)) {
            volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)at;
            *p = 0x1234;
            fprintf(stderr, "[nx probe]   written and read back: %#x; set read-only: 0x%x\n", *p,
                    svcSetMemoryPermission((void *)(uintptr_t)at, NX_PAGE, Perm_R));
            svcControlCodeMemory(h, CodeMapOperation_UnmapOwner, (void *)(uintptr_t)at, NX_PAGE, 0);
        }
        svcCloseHandle(h);
    }

    rc = svcMapMemory((void *)(uintptr_t)at, src, NX_PAGE);
    fprintf(stderr, "[nx probe] svcMapMemory at %#lx: 0x%x\n", (unsigned long)at, rc);
    if (R_SUCCEEDED(rc)) {
        fprintf(stderr, "[nx probe]   set read-only: 0x%x\n",
                svcSetMemoryPermission((void *)(uintptr_t)at, NX_PAGE, Perm_R));
        svcSetMemoryPermission((void *)(uintptr_t)at, NX_PAGE, Perm_Rw);
        svcUnmapMemory((void *)(uintptr_t)at, src, NX_PAGE);
    }
    {
        uint64_t sa = 0, ss = 0;
        svcGetInfo(&sa, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&ss, InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0);
        rc = svcMapMemory((void *)(uintptr_t)(sa + ss / 2), src, NX_PAGE);
        fprintf(stderr, "[nx probe] svcMapMemory in the stack region: 0x%x\n", rc);
        if (R_SUCCEEDED(rc))
            svcUnmapMemory((void *)(uintptr_t)(sa + ss / 2), src, NX_PAGE);
    }
    free(src);

    if (*e == '2') {
        PVOID vh = AddVectoredExceptionHandler(1, probe_veh);
        uint64_t x0 = 0;
        fprintf(stderr, "[nx probe] faulting on purpose at %#lx...\n", (unsigned long)at);
        fflush(stderr);
        __asm__ volatile("mov x0, %1\n\tldr w0, [x0]\n\tmov %0, x0" : "=r"(x0) : "r"(at) : "x0", "memory");
        fprintf(stderr, "[nx probe] fault resumed: x0 = %#lx (0x600df00d expected)\n", (unsigned long)x0);
        RemoveVectoredExceptionHandler(vh);
    }
    fflush(stderr);
}

/* The guest space's slot, taken before main: libnx maps every thread's
 * stack into the stack region at a random place, so by the time the layout
 * asks for 1088 MB in one piece the 2 GB region can be too broken up to have
 * it (a black screen, all devices unmapped). The first reservation of up to
 * this size gets it. */
#define NX_EARLY_SPAN (0x44000000ull + (32ull << 20))
static void *s_early;
static uint64_t s_early_len;

__attribute__((constructor)) static void nx_early_span(void)
{
    virtmemLock();
    s_early = virtmemFindStack(NX_EARLY_SPAN, 0x10000);
    if (s_early && !virtmemAddReservation(s_early, NX_EARLY_SPAN))
        s_early = NULL;
    virtmemUnlock();
    s_early_len = s_early ? NX_EARLY_SPAN : 0;
}

/* A slot of the stack region, the one place an unprivileged process can
 * map memory at an address of its choosing (svcMapMemory). The guest space
 * fits it folded (platform/guest_addr.h): 0x44000000 bytes. */
static void *span_new(uint64_t len)
{
    void *base = NULL;
    static int probed;

    len = (len + 0x1FFFFFull) & ~0x1FFFFFull;
    if (s_nspan >= NX_MAX_SPANS)
        return NULL;
    if (s_early && len <= s_early_len) {
        base = s_early;             /* reserved already; the tail stays unused */
        s_early = NULL;
    } else {
        virtmemLock();
        base = virtmemFindStack(len, 0x10000);
        if (base && !virtmemAddReservation(base, len))
            base = NULL;
        virtmemUnlock();
    }
    if (!base) {
        uint64_t sa = 0, ss = 0;
        svcGetInfo(&sa, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&ss, InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0);
        fprintf(stderr, "[nx] no room for a %lu MB reservation in the stack region"
                " (%#lx + %#lx)\n", (unsigned long)(len >> 20), (unsigned long)sa,
                (unsigned long)ss);
        return NULL;
    }
    s_span[s_nspan].lo = (uint64_t)(uintptr_t)base;
    s_span[s_nspan].hi = (uint64_t)(uintptr_t)base + len;
    s_nspan++;
    fprintf(stderr, "[nx] guest space reserved at %p (%lu MB)\n", base,
            (unsigned long)(len >> 20));
    if (!probed++)
        nx_probe((uint64_t)(uintptr_t)base);
    return base;
}

/* Calls fn on each [a, b) piece of [lo, hi) that is mapped (want_mapped) or
 * not. Stops at the first failure. */
static int for_pieces(uint64_t lo, uint64_t hi, int want_mapped,
                      int (*fn)(uint64_t a, uint64_t b, void *u), void *u)
{
    while (lo < hi) {
        MemoryInfo mi;
        u32 pi;
        uint64_t end;
        if (R_FAILED(svcQueryMemory(&mi, &pi, lo)))
            return -1;
        end = mi.addr + mi.size;
        if (end > hi || end <= lo)
            end = hi;
        if ((mi.type != MemType_Unmapped) == want_mapped && fn(lo, end, u) != 0)
            return -1;
        lo = end;
    }
    return 0;
}

static u32 nx_perm(int prot)
{
    if (prot & PROT_WRITE) return Perm_Rw;
    if (prot & PROT_READ)  return Perm_R;
    return Perm_None;
}

/* Memory mapped into a span: heap memory moved there with svcMapMemory
 * (the source stays allocated, and inaccessible, while it is). Unmapped
 * whole, as it was mapped. */
#define NX_MAX_PIECES 256
static struct { uint64_t dst, len; void *src; } s_piece[NX_MAX_PIECES];

static int piece_map(uint64_t a, uint64_t b, void *u)
{
    void *src;
    Result rc;
    int i;
    (void)u;
    for (i = 0; i < NX_MAX_PIECES && s_piece[i].len; i++)
        ;
    if (i == NX_MAX_PIECES || !(src = memalign(NX_PAGE, b - a))) {
        fprintf(stderr, "[nx] out of memory mapping %lu KB at %#lx\n",
                (unsigned long)((b - a) >> 10), (unsigned long)a);
        return -1;
    }
    rc = svcMapMemory((void *)(uintptr_t)a, src, b - a);
    if (R_FAILED(rc)) {
        fprintf(stderr, "[nx] svcMapMemory(%#lx, %lu KB) failed: 0x%x\n",
                (unsigned long)a, (unsigned long)((b - a) >> 10), rc);
        free(src);
        return -1;
    }
    s_piece[i].dst = a;
    s_piece[i].len = b - a;
    s_piece[i].src = src;
    memset((void *)(uintptr_t)a, 0, b - a);      /* as an anonymous mmap is */
    return 0;
}

static int piece_unmap(uint64_t a, uint64_t b, void *u)
{
    int i, rc = 0;
    (void)u;
    for (i = 0; i < NX_MAX_PIECES; i++) {
        uint64_t lo = s_piece[i].dst, hi = lo + s_piece[i].len;
        if (!s_piece[i].len || hi <= a || lo >= b)
            continue;
        if (lo < a || hi > b) {             /* part of a mapping: leave it */
            rc = -1;
            continue;
        }
        svcSetMemoryPermission((void *)(uintptr_t)lo, hi - lo, Perm_Rw);
        if (R_SUCCEEDED(svcUnmapMemory((void *)(uintptr_t)lo, s_piece[i].src, hi - lo))) {
            free(s_piece[i].src);
            s_piece[i].len = 0;
        } else {
            rc = -1;
        }
    }
    return rc;
}

static int piece_perm(uint64_t a, uint64_t b, void *u)
{
    u32 perm = *(u32 *)u;
    Result rc = svcSetMemoryPermission((void *)(uintptr_t)a, b - a, perm);
    if (R_FAILED(rc)) {
        fprintf(stderr, "[nx] svcSetMemoryPermission(%#lx, %lu KB, %u) failed: 0x%x\n",
                (unsigned long)a, (unsigned long)((b - a) >> 10), perm, rc);
        return -1;
    }
    return 0;
}

static int piece_busy(uint64_t a, uint64_t b, void *u)
{
    (void)a; (void)b; (void)u;
    return -1;
}

/* ---------------------------------------------------------------------- */
/* Heap-backed mappings                                                    */
/* ---------------------------------------------------------------------- */

#define NX_MAX_HEAPMAPS 1024
static struct { uint64_t lo, len; } s_hm[NX_MAX_HEAPMAPS];

static int hm_find(uint64_t a)
{
    int i;
    for (i = 0; i < NX_MAX_HEAPMAPS; i++)
        if (s_hm[i].len && a >= s_hm[i].lo && a < s_hm[i].lo + s_hm[i].len)
            return i;
    return -1;
}

static void *hm_alloc(uint64_t len)
{
    int i;
    void *p = memalign(NX_PAGE, len);
    if (!p)
        return NULL;
    memset(p, 0, len);
    for (i = 0; i < NX_MAX_HEAPMAPS; i++)
        if (!s_hm[i].len) {
            s_hm[i].lo = (uint64_t)(uintptr_t)p;
            s_hm[i].len = len;
            return p;
        }
    free(p);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* memfd: one view each                                                    */
/* ---------------------------------------------------------------------- */

#define NX_MEMFD_BASE 0x40000
#define NX_MAX_MEMFD 64
static uint64_t s_memfd_view[NX_MAX_MEMFD];
static int s_nmemfd;

int memfd_create(const char *name, unsigned int flags)
{
    int fd;
    (void)name; (void)flags;
    pthread_mutex_lock(&s_vm_lock);
    fd = s_nmemfd < NX_MAX_MEMFD ? NX_MEMFD_BASE + s_nmemfd++ : -1;
    pthread_mutex_unlock(&s_vm_lock);
    if (fd < 0)
        errno = EMFILE;
    return fd;
}

/* ---------------------------------------------------------------------- */
/* mmap / munmap / mprotect                                                */
/* ---------------------------------------------------------------------- */

void *mmap(void *addr, size_t len_in, int prot, int flags, int fd, off_t off)
{
    uint64_t a = (uint64_t)(uintptr_t)addr, len = pg_up(len_in);
    void *ret = MAP_FAILED;
    int memfd = -1;
    u32 perm = nx_perm(prot);

    if (!len) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    pthread_mutex_lock(&s_vm_lock);
    if (!(flags & MAP_ANONYMOUS)) {
        memfd = fd - NX_MEMFD_BASE;
        if (memfd < 0 || memfd >= s_nmemfd || off != 0) {
            errno = EBADF;
            goto out;
        }
        if (s_memfd_view[memfd]) {
            static int said;
            if (!said++)
                fprintf(stderr, "[nx] a second view of a mapping: no page aliasing on Switch\n");
            errno = ENOTSUP;
            goto out;
        }
    }
    if (a && (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && !in_span(a, len)) {
        errno = ENOMEM;
        goto out;
    }
    if (a && in_span(a, len)) {
        if (flags & MAP_FIXED_NOREPLACE) {
            if (for_pieces(a, a + len, 1, piece_busy, NULL) != 0) {
                errno = EEXIST;
                goto out;
            }
        } else if (flags & MAP_FIXED) {
            for_pieces(a, a + len, 1, piece_unmap, NULL);
        }
        if (for_pieces(a, a + len, 0, piece_map, NULL) != 0) {
            errno = ENOMEM;
            goto out;
        }
        if (perm != Perm_Rw)
            for_pieces(a, a + len, 1, piece_perm, &perm);
        ret = addr;
    } else if (len >= NX_SPAN_MIN && memfd < 0) {
        ret = span_new(len);
        if (!ret) {
            ret = MAP_FAILED;
            errno = ENOMEM;
        }
    } else {
        ret = hm_alloc(len);
        if (!ret) {
            ret = MAP_FAILED;
            errno = ENOMEM;
        } else if (perm != Perm_Rw) {
            svcSetMemoryPermission(ret, len, perm);
        }
    }
    if (ret != MAP_FAILED && memfd >= 0)
        s_memfd_view[memfd] = (uint64_t)(uintptr_t)ret;
out:
    pthread_mutex_unlock(&s_vm_lock);
    return ret;
}

int munmap(void *addr, size_t len_in)
{
    uint64_t a = pg_down((uint64_t)(uintptr_t)addr), len = pg_up(len_in);
    int i, rc = 0;

    pthread_mutex_lock(&s_vm_lock);
    if ((i = hm_find(a)) >= 0) {
        svcSetMemoryPermission((void *)(uintptr_t)s_hm[i].lo, s_hm[i].len, Perm_Rw);
        free((void *)(uintptr_t)s_hm[i].lo);
        s_hm[i].len = 0;
    } else if (in_span(a, len)) {
        rc = for_pieces(a, a + len, 1, piece_unmap, NULL);
    }
    for (i = 0; i < s_nmemfd; i++)
        if (s_memfd_view[i] == a)
            s_memfd_view[i] = 0;
    pthread_mutex_unlock(&s_vm_lock);
    return rc;
}

/* As on Linux, making part of a reservation accessible commits it. */
int mprotect(void *addr, size_t len_in, int prot)
{
    uint64_t a = pg_down((uint64_t)(uintptr_t)addr);
    uint64_t len = pg_up((uint64_t)(uintptr_t)addr + len_in) - a;
    u32 perm = nx_perm(prot);
    int rc = 0;

    if (prot & PROT_EXEC) {
        errno = ENOTSUP;
        return -1;
    }
    pthread_mutex_lock(&s_vm_lock);
    if (in_span(a, len)) {
        if (perm != Perm_None)
            rc = for_pieces(a, a + len, 0, piece_map, NULL);
        if (rc == 0)
            rc = for_pieces(a, a + len, 1, piece_perm, &perm);
    } else if (hm_find(a) >= 0) {
        rc = R_SUCCEEDED(svcSetMemoryPermission(addr, len, perm)) ? 0 : -1;
    } else {
        rc = -1;
    }
    pthread_mutex_unlock(&s_vm_lock);
    if (rc)
        errno = EACCES;
    return rc;
}

/* newlib declares these; libnx implements none of them. */
long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE:          return (long)NX_PAGE;
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF:  return 3;   /* the cores an application gets */
    default:                    errno = EINVAL; return -1;
    }
}

int posix_memalign(void **out, size_t align, size_t size)
{
    void *p = memalign(align, size);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

/* No processes to start: RECOMP_FB_VIDEO's ffmpeg pipe (fb_present.c)
 * fails, as when ffmpeg is missing elsewhere. The movies are the title's
 * own Bink decoder, lifted, and need nothing from here. */
FILE *popen(const char *cmd, const char *mode)
{
    (void)cmd; (void)mode;
    errno = ENOSYS;
    return NULL;
}

int pclose(FILE *f)
{
    (void)f;
    return -1;
}

/* UTC broken-down time to seconds, without the time zone mktime applies. */
time_t timegm(struct tm *tm)
{
    long long y = tm->tm_year + 1900LL, m = tm->tm_mon + 1, era, yoe, doy, doe, days;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec);
}

int sysinfo(struct sysinfo *info)
{
    uint64_t total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    memset(info, 0, sizeof(*info));
    info->mem_unit = 1;
    info->totalram = total;
    info->freeram = total > used ? total - used : 0;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* The log                                                                 */
/* ---------------------------------------------------------------------- */

/* stdout and stderr into a file on the SD card, buffered. Unbuffered, every
 * write was its own trip to the file system service -- about 60 ms a line on
 * a console -- and the boot's few thousand log lines took minutes. Every
 * thread's streams end on descriptors 1 and 2, so the device behind those is
 * replaced: a write copies into a large stdio buffer, and a thread writes it
 * out twice a second (and nx_log_flush does at once). */
static FILE *s_log;
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;

static ssize_t log_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    (void)r; (void)fd;
    pthread_mutex_lock(&s_log_lock);
    fwrite(ptr, 1, len, s_log);
    pthread_mutex_unlock(&s_log_lock);
    return (ssize_t)len;
}

void nx_log_flush(void)
{
    if (!s_log)
        return;
    pthread_mutex_lock(&s_log_lock);
    fflush(s_log);
    pthread_mutex_unlock(&s_log_lock);
}

static void *log_flusher(void *unused)
{
    (void)unused;
    for (;;) {
        usleep(500 * 1000);
        nx_log_flush();
    }
    return NULL;
}

static const devoptab_t s_log_dev = {
    .name = "rsc2log",
    .write_r = log_write,
};

/* CPU time the calling thread has used, in microseconds (ticks at 19.2 MHz;
 * InfoType_ThreadTickCount on 13.0.0+, the older id before). */
uint64_t nx_thread_cpu_us(void)
{
    u64 t = 0;
    if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, (u64)-1))
            && R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE,
                                   (u64)-1)))
        return 0;
    return t * 10 / 192;
}

/* The generated code reads the thread pointer from TPIDR_EL0 (-mtp=tpidr_el0,
 * rsc2/CMakeLists.txt) with one instruction. libnx keeps it elsewhere
 * (TPIDRRO_EL0 + 0x1F8) and GCC's default reached it through a call to
 * __aarch64_read_tp, which every lifted function made -- about a tenth of
 * the game thread. TPIDR_EL0 is the thread's own (Horizon saves it with
 * the thread) and unused by libnx, so each thread copies its pointer there
 * when it starts: the same pointer, so the same addresses as the rest of
 * the program's thread-locals. A thread that skipped this and ran lifted
 * code would fault at its first register access, near address 0. */
void nx_tp_mirror(void)
{
    extern void *__aarch64_read_tp(void);
    uint64_t tp = (uint64_t)(uintptr_t)__aarch64_read_tp();
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(tp));
}

/* Thread placement (win32_compat.c, "Thread placement"): priority 59
 * (libnx's for pthreads; 60 for a guest thread under RECOMP_GUEST_PRIO=lower),
 * allowed on the application's cores 0-2 with its preferred core in turn. */
void nx_thread_place(int guest)
{
    nx_tp_mirror();
    static volatile int next;
    int core = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % 3;
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 0x7);
    Result rp = svcSetThreadPriority(CUR_THREAD_HANDLE, guest ? 0x3C : 0x3B);
    if (R_FAILED(rc) || R_FAILED(rp))
        fprintf(stderr, "[nx] thread placement: core mask 0x%x, priority 0x%x\n", rc, rp);
}

/* The calling thread's priority (lower is more urgent; the game's threads
 * run at 0x3B, nx_thread_place). For a thread that has to keep real time
 * against them: the audio mixer. */
void nx_thread_priority(int prio)
{
    Result rp = svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
    if (R_FAILED(rp))
        fprintf(stderr, "[nx] thread priority 0x%x: 0x%x\n", prio, rp);
}

int nx_log_open(const char *path)
{
    pthread_t t;
    s_log = fopen(path, "wb");
    if (!s_log)
        return 0;
    setvbuf(s_log, NULL, _IOFBF, 256 * 1024);
    devoptab_list[STD_OUT] = &s_log_dev;
    devoptab_list[STD_ERR] = &s_log_dev;
    if (pthread_create(&t, NULL, log_flusher, NULL) == 0)
        pthread_detach(t);
    return 1;
}

/* ---------------------------------------------------------------------- */
/* Symbols and backtraces                                                  */
/* ---------------------------------------------------------------------- */

extern char __start__[];       /* switch.ld: the module's base */

int dladdr(const void *addr, Dl_info *info)
{
    memset(info, 0, sizeof(*info));
    info->dli_fname = "rsc2.elf";
    info->dli_fbase = __start__;
    (void)addr;
    return 1;
}

/* Frame-pointer walk: the build keeps frame pointers in the hand-written
 * code; generated code may omit them, which ends the walk early. */
int backtrace(void **buffer, int size)
{
    void **fp = (void **)__builtin_frame_address(0);
    int n = 0;
    while (fp && n < size) {
        MemoryInfo mi;
        u32 pi;
        if (((uintptr_t)fp & 7) || R_FAILED(svcQueryMemory(&mi, &pi, (uintptr_t)fp))
                || !(mi.perm & Perm_R))
            break;
        if (!fp[1])
            break;
        buffer[n++] = fp[1];
        if ((void **)fp[0] <= fp)
            break;
        fp = (void **)fp[0];
    }
    return n;
}

char **backtrace_symbols(void *const *buffer, int size)
{
    (void)buffer; (void)size;
    return NULL;
}

void backtrace_symbols_fd(void *const *buffer, int size, int fd)
{
    int i;
    (void)fd;
    for (i = 0; i < size; i++)
        fprintf(stderr, "  #%d rsc2.elf+%#lx\n", i,
                (unsigned long)((uintptr_t)buffer[i] - (uintptr_t)__start__));
}

/* ---------------------------------------------------------------------- */
/* Faults                                                                  */
/* ---------------------------------------------------------------------- */

/* What the kernel saves at x1 on a user exception. */
typedef struct {
    uint64_t x[9];                  /* x0..x8 */
    uint64_t lr, sp, pc;
    uint32_t pstate, afsr0, afsr1, esr;
    uint64_t far;
} NxExcCtx;

/* What the entry below saves of the live registers. */
typedef struct {
    uint64_t x[21];                 /* x9..x29 */
    uint64_t fpcr_fpsr;             /* fpcr low, fpsr high */
    uint64_t q[32][2];
} NxExcFrame;

_Static_assert(sizeof(NxExcFrame) == 22 * 8 + 32 * 16, "NxExcFrame layout");

static uint8_t s_exc_stack[0x10000] __attribute__((aligned(16)));
__attribute__((visibility("hidden"))) void *nx_exc_stack_top = s_exc_stack + sizeof(s_exc_stack);

/* For the stall watchdog (kernel_bridge.c): how many exceptions came
 * through here, and the last one. A fault that a handler "resumes" without
 * fixing comes straight back, silently, forever. */
static volatile uint64_t s_exc_n, s_exc_pc, s_exc_far;
static volatile uint32_t s_exc_esr;

void nx_exc_stats(uint64_t *n, uint64_t *pc_off, uint64_t *far, uint32_t *esr)
{
    *n = s_exc_n;
    *pc_off = s_exc_pc ? s_exc_pc - (uintptr_t)__start__ : 0;
    *far = s_exc_far;
    *esr = s_exc_esr;
}

__attribute__((visibility("hidden"), used))
uint32_t nx_exc_dispatch(uint32_t desc, NxExcCtx *k, NxExcFrame *f)
{
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    EXCEPTION_POINTERS ep;
    uint32_t ec = k->esr >> 26;
    int i;

    s_exc_n++;
    s_exc_pc = k->pc;
    s_exc_far = k->far;
    s_exc_esr = k->esr;

    for (i = 0; i < 9; i++)
        ctx.X[i] = k->x[i];
    for (i = 9; i < 30; i++)
        ctx.X[i] = f->x[i - 9];
    ctx.X[30] = k->lr;
    ctx.Sp = k->sp;
    ctx.Pc = k->pc;
    ctx.Pstate = k->pstate;
    ctx.HasV = 1;
    memcpy(ctx.V, f->q, sizeof(ctx.V));

    memset(&rec, 0, sizeof(rec));
    switch (ec) {
    case 0x24: case 0x25:                       /* data abort */
        rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = (k->esr >> 6) & 1;   /* WnR */
        rec.ExceptionInformation[1] = k->far;
        break;
    case 0x20: case 0x21:                       /* instruction abort */
        rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = 8;
        rec.ExceptionInformation[1] = k->far;
        break;
    case 0x3C:                                  /* brk */
        rec.ExceptionCode = EXCEPTION_BREAKPOINT;
        break;
    case 0x00:                                  /* undefined instruction */
        rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
        break;
    default:
        rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
        break;
    }
    rec.ExceptionAddress = (PVOID)(uintptr_t)k->pc;
    ep.ExceptionRecord = &rec;
    ep.ContextRecord = &ctx;

    if (w32_veh_run(&ep)) {
        for (i = 0; i < 9; i++)
            k->x[i] = ctx.X[i];
        for (i = 9; i < 30; i++)
            f->x[i - 9] = ctx.X[i];
        k->lr = ctx.X[30];
        k->sp = ctx.Sp;
        k->pc = ctx.Pc;
        k->pstate = (uint32_t)ctx.Pstate;
        memcpy(f->q, ctx.V, sizeof(f->q));
        return 0;
    }

    fprintf(stderr, "\n[nx] unhandled exception 0x%x: pc rsc2.elf+%#lx far %#lx esr %#x\n",
            desc, (unsigned long)(k->pc - (uintptr_t)__start__), (unsigned long)k->far, k->esr);
    for (i = 0; i < 31; i++)
        fprintf(stderr, "  x%-2d %016lx%s", i, (unsigned long)ctx.X[i], i % 3 == 2 ? "\n" : "");
    fprintf(stderr, "  sp  %016lx  lr rsc2.elf+%#lx\n", (unsigned long)ctx.Sp,
            (unsigned long)(ctx.X[30] - (uintptr_t)__start__));
    fflush(stderr);
    nx_log_flush();
    return 0xF801;      /* not handled: the kernel ends the process (crash report) */
}

/* Replaces libnx's weak entry (which only ever reports and ends). Entered
 * from crt0 with x0 = the exception type and x1 = the kernel's context
 * record; x2-x8 are free (saved in the record), x9-x29 and the SIMD
 * registers still hold the faulting code's values. */
__asm__(
    ".section .text.__libnx_exception_entry, \"ax\", %progbits\n"
    ".global __libnx_exception_entry\n"
    ".type __libnx_exception_entry, %function\n"
    ".balign 4\n"
    "__libnx_exception_entry:\n"
    "    cbz   x1, 2f\n"
    "    adrp  x2, nx_exc_stack_top\n"
    "    ldr   x2, [x2, #:lo12:nx_exc_stack_top]\n"
    "    mov   sp, x2\n"
    "    sub   sp, sp, #688\n"              /* sizeof(NxExcFrame) */
    "    stp   x9, x10, [sp, #0]\n"
    "    stp   x11, x12, [sp, #16]\n"
    "    stp   x13, x14, [sp, #32]\n"
    "    stp   x15, x16, [sp, #48]\n"
    "    stp   x17, x18, [sp, #64]\n"
    "    stp   x19, x20, [sp, #80]\n"
    "    stp   x21, x22, [sp, #96]\n"
    "    stp   x23, x24, [sp, #112]\n"
    "    stp   x25, x26, [sp, #128]\n"
    "    stp   x27, x28, [sp, #144]\n"
    "    mrs   x3, fpcr\n"
    "    mrs   x4, fpsr\n"
    "    orr   x3, x3, x4, lsl #32\n"
    "    stp   x29, x3, [sp, #160]\n"
    "    add   x3, sp, #176\n"
    "    stp   q0, q1, [x3, #0]\n"
    "    stp   q2, q3, [x3, #32]\n"
    "    stp   q4, q5, [x3, #64]\n"
    "    stp   q6, q7, [x3, #96]\n"
    "    stp   q8, q9, [x3, #128]\n"
    "    stp   q10, q11, [x3, #160]\n"
    "    stp   q12, q13, [x3, #192]\n"
    "    stp   q14, q15, [x3, #224]\n"
    "    stp   q16, q17, [x3, #256]\n"
    "    stp   q18, q19, [x3, #288]\n"
    "    stp   q20, q21, [x3, #320]\n"
    "    stp   q22, q23, [x3, #352]\n"
    "    stp   q24, q25, [x3, #384]\n"
    "    stp   q26, q27, [x3, #416]\n"
    "    stp   q28, q29, [x3, #448]\n"
    "    stp   q30, q31, [x3, #480]\n"
    "    mov   x2, sp\n"
    "    mov   x29, #0\n"
    "    bl    nx_exc_dispatch\n"
    "    mov   w8, w0\n"
    "    add   x3, sp, #176\n"
    "    ldp   q0, q1, [x3, #0]\n"
    "    ldp   q2, q3, [x3, #32]\n"
    "    ldp   q4, q5, [x3, #64]\n"
    "    ldp   q6, q7, [x3, #96]\n"
    "    ldp   q8, q9, [x3, #128]\n"
    "    ldp   q10, q11, [x3, #160]\n"
    "    ldp   q12, q13, [x3, #192]\n"
    "    ldp   q14, q15, [x3, #224]\n"
    "    ldp   q16, q17, [x3, #256]\n"
    "    ldp   q18, q19, [x3, #288]\n"
    "    ldp   q20, q21, [x3, #320]\n"
    "    ldp   q22, q23, [x3, #352]\n"
    "    ldp   q24, q25, [x3, #384]\n"
    "    ldp   q26, q27, [x3, #416]\n"
    "    ldp   q28, q29, [x3, #448]\n"
    "    ldp   q30, q31, [x3, #480]\n"
    "    ldp   x29, x3, [sp, #160]\n"
    "    msr   fpcr, x3\n"
    "    lsr   x3, x3, #32\n"
    "    msr   fpsr, x3\n"
    "    ldp   x9, x10, [sp, #0]\n"
    "    ldp   x11, x12, [sp, #16]\n"
    "    ldp   x13, x14, [sp, #32]\n"
    "    ldp   x15, x16, [sp, #48]\n"
    "    ldp   x17, x18, [sp, #64]\n"
    "    ldp   x19, x20, [sp, #80]\n"
    "    ldp   x21, x22, [sp, #96]\n"
    "    ldp   x23, x24, [sp, #112]\n"
    "    ldp   x25, x26, [sp, #128]\n"
    "    ldp   x27, x28, [sp, #144]\n"
    "    mov   w0, w8\n"
    "    b     1f\n"
    "2:  mov   w0, #0xF801\n"
    "1:  svc   0x28\n"                      /* svcReturnFromException */
    "    b     .\n"
    ".size __libnx_exception_entry, . - __libnx_exception_entry\n"
    ".text\n"
);

#endif /* __SWITCH__ */
