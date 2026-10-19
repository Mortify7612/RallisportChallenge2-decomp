/*
 * xbox_mmio.c - device registers reached by direct call.
 *
 * Guest code reaches memory through MEMn(addr) and MEMn_W(addr, value)
 * (templates/runtime/recomp_types.h). At or above 0xFD000000 -- the NV2A and
 * the MCPX devices -- those call xbox_mmio_read / xbox_mmio_write here
 * instead of touching memory, and a device model that registered the range
 * answers. Everything else up there is plain memory, as the apertures always
 * were.
 *
 * This replaces trapping the registers as page faults and decoding the
 * faulting instruction (mmio_decode.h), which needed a host that can resume
 * a fault -- not an emulated Switch, which ends the program -- and cost a
 * kernel round trip per access. The models' callbacks are the ones the fault
 * decoder called, so both paths share them; RECOMP_MMIO_TRAPS=1 still arms
 * the page traps too, for host code that touches the registers through a
 * raw pointer (nothing does today).
 */
#include "xbox_memory_layout.h"
#include "xbox_mmio.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XBOX_MMIO_MAX 16

static struct {
    uint32_t lo, hi;        /* [lo, hi) */
    uint32_t off_base;      /* the callbacks get va - off_base */
    void *dev;
    xbox_mmio_read_fn rd;   /* NULL: reads see plain memory */
    xbox_mmio_write_fn wr;  /* NULL: writes go to plain memory */
} s_mmio[XBOX_MMIO_MAX];
static volatile int s_mmio_count;

void xbox_MmioRegister(uint32_t base, uint32_t len, uint32_t off_base, void *dev,
                       xbox_mmio_read_fn rd, xbox_mmio_write_fn wr)
{
    int n = s_mmio_count;
    if (n >= XBOX_MMIO_MAX) {
        fprintf(stderr, "  MMIO: no room to register 0x%08X\n", base);
        return;
    }
    s_mmio[n].lo = base;
    s_mmio[n].hi = base + len;
    s_mmio[n].off_base = off_base;
    s_mmio[n].dev = dev;
    s_mmio[n].rd = rd;
    s_mmio[n].wr = wr;
    __atomic_store_n(&s_mmio_count, n + 1, __ATOMIC_RELEASE);
}

int xbox_MmioTraps(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_MMIO_TRAPS");
        on = e && strcmp(e, "0") != 0;
    }
    return on;
}

static int find(uint32_t va)
{
    int i, n = __atomic_load_n(&s_mmio_count, __ATOMIC_ACQUIRE);
    for (i = 0; i < n; i++)
        if (va >= s_mmio[i].lo && va < s_mmio[i].hi)
            return i;
    return -1;
}

static void *plain(uint32_t va)
{
    return (uint8_t *)xbox_GetMemoryOffset() + XBOX_FOLD(va);
}

/* A title waits for the hardware by reading a register until a bit changes,
 * and here the bit is changed by another host thread (the NV2A busy-bit
 * acknowledger, a device model's worker). With time slicing that thread runs
 * sooner or later; on a host without it for threads of equal priority -- the
 * Switch, or Linux under SCHED_FIFO on one CPU -- the poll keeps the core and
 * the bit never changes. So a thread that keeps reading registers lets the
 * others run every so often. Ordinary register traffic is short bursts and
 * barely pays for it. */
#define XBOX_MMIO_YIELD_EVERY 64

/* RECOMP_MMIO_POLL_TRACE=1: every 4M reads, the registers read most --
 * what a thread spinning on the hardware is waiting for. (RSC2's main
 * thread: the PFB flush at 0xFD100410, which nv2a_ack_thread clears once
 * the executor has caught up; cleared as soon as it was read instead, the
 * game hung on 0xFD400B10 at startup.) */
static void poll_trace(uint32_t va)
{
    static int on = -1;
    static struct { uint32_t va; unsigned long n; } top[32];
    static unsigned long total;
    int k;
    if (on < 0)
        on = getenv("RECOMP_MMIO_POLL_TRACE") != NULL;
    if (!on)
        return;
    for (k = 0; k < 32 && top[k].va && top[k].va != va; k++)
        ;
    if (k < 32) {
        top[k].va = va;
        top[k].n++;
    }
    if (++total % (4ul << 20) == 0) {
        fprintf(stderr, "[MMIO] most read:");
        for (k = 0; k < 32 && top[k].va; k++)
            if (top[k].n > total / 50)
                fprintf(stderr, " %08X x%lu", top[k].va, top[k].n);
        fputc('\n', stderr);
        memset(top, 0, sizeof top);
        total = 0;
    }
}

uint64_t xbox_mmio_read(uint32_t va, int size)
{
    static __thread unsigned reads;
    int i = find(va);
    uint64_t v = 0;
    poll_trace(va);
    if (++reads % XBOX_MMIO_YIELD_EVERY == 0) {
        extern void xbox_thread_tick(int poll);
        xbox_thread_tick(1);
        sched_yield();
    }
    if (i >= 0 && s_mmio[i].rd)
        return s_mmio[i].rd(s_mmio[i].dev, va - s_mmio[i].off_base, size);
    memcpy(&v, plain(va), (size_t)size);
    return v;
}

void xbox_mmio_write(uint32_t va, uint64_t value, int size)
{
    extern void xbox_Nv2aKick(uint32_t va);
    int i = find(va);
    if (va >= 0xFD000000u && va < 0xFE000000u) {
        /* After the write, so the woken acknowledge thread sees it. */
        int r = i >= 0 && s_mmio[i].wr;
        if (r)
            s_mmio[i].wr(s_mmio[i].dev, va - s_mmio[i].off_base, value, size);
        else
            memcpy(plain(va), &value, (size_t)size);
        xbox_Nv2aKick(va);
        return;
    }
    if (i >= 0 && s_mmio[i].wr) {
        s_mmio[i].wr(s_mmio[i].dev, va - s_mmio[i].off_base, value, size);
        return;
    }
    memcpy(plain(va), &value, (size_t)size);
}
