/*
 * NV2A interrupt sources with register semantics: PMC, PCRTC and PTIMER.
 *
 * The NV2A register aperture is plain memory here, and for most of it that is
 * enough. The interrupt plumbing is the exception. On hardware
 *
 *   - PCRTC_INTR_0 and PTIMER_INTR_0 are write-1-to-clear status registers,
 *   - PMC_INTR_0 is not storage at all but the OR of every block whose status
 *     is set and enabled, and the interrupt line is that OR gated by
 *     PMC_INTR_EN_0,
 *   - PTIMER_TIME_0/1 count, and PTIMER_ALARM_0 raises an interrupt when the
 *     count passes it.
 *
 * Plain memory gets all of that wrong, and the runtime papered over it by
 * holding the status registers at zero from the ack thread ("nothing ever
 * raises a GPU interrupt") and poking the vblank bits in just before calling
 * the ISR. That works until a title depends on the timer. XDK 5849's D3D does
 * once a frame interval is configured: it stops waiting for the CRTC's vblank
 * and synthesises one from a PTIMER alarm every 16.68 ms (59.94 Hz) or 20 ms,
 * re-arming the alarm from the vblank DPC. With a timer that never counts and
 * an alarm that never fires, BlockUntilVerticalBlank waits forever --
 * RalliSport Challenge 2 froze on its title screen the moment START moved it
 * to the next screen.
 *
 * So the three register pages are trapped (like the OHCI controllers) and
 * served from this model; every other register on them is backing store.
 * The interrupt line is raised from the timer thread while a source is
 * pending and enabled -- level-triggered, as on hardware.
 *
 * Opt-in with RECOMP_NV2A_IRQ, because it changes what a title that worked
 * with the old behaviour sees.
 *
 * With RECOMP_NV2A_PFB_ASYNC the PFB flush and PGRAPH's page are modelled
 * too: the flush is done at once, so the title builds the next frame while
 * the executor draws this one, and the notify a NO_OPERATION with a
 * parameter raises reaches D3D's ISR, which it needs once it is ahead.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "../platform/mmio_decode.h"

#define NV2A_BASE        0xFD000000u
#define PG_PMC           0x000000u
#define PG_PTIMER        0x009000u
#define PG_PCRTC         0x600000u
#define PG_PFB           0x100000u
#define PAGE             0x1000u

#define PMC_INTR_0       0x000100u
#define PMC_INTR_EN_0    0x000140u
#define PMC_INTR_PTIMER  (1u << 20)
#define PMC_INTR_PCRTC   (1u << 24)

#define PTIMER_INTR_0    0x009100u
#define PTIMER_INTR_EN_0 0x009140u
#define PTIMER_NUMERATOR 0x009200u
#define PTIMER_DENOMINATOR 0x009210u
#define PTIMER_TIME_0    0x009400u
#define PTIMER_TIME_1    0x009410u
#define PTIMER_ALARM_0   0x009420u

#define PFB_WBC          0x100410u     /* write-back cache flush */
#define PFB_WBC_FLUSH    0x00010000u

#define PCRTC_INTR_0     0x600100u
#define PCRTC_INTR_EN_0  0x600140u

/* PGRAPH, for the notify a NO_OPERATION with a parameter raises (xemu's
 * pgraph.c and nv2a_regs.h). */
#define PG_PGRAPH        0x400000u
#define PMC_INTR_PGRAPH  (1u << 12)
#define PGRAPH_INTR      0x400100u
#define PGRAPH_INTR_ERROR (1u << 20)
#define PGRAPH_NSOURCE   0x400108u
#define PGRAPH_INTR_EN   0x400140u
#define PGRAPH_TRAPPED_ADDR 0x400704u
#define PGRAPH_TRAPPED_DATA 0x400708u
#define PGRAPH_FIFO      0x400720u     /* bit 0: PGRAPH may take methods */

/* The NV2A's core clock, which PTIMER counts in before the numerator and
 * denominator scale it -- xemu's default for the retail PLL setting. */
#define NV2A_CORE_HZ     233333324ull

static int s_enabled;
static int s_pfb;                       /* PFB page trapped too */
static uint8_t s_store[5][PAGE];        /* backing for everything else */
static volatile uint32_t s_pcrtc_intr, s_ptimer_intr, s_pgraph_intr;
static volatile uint32_t s_alarm, s_alarm_armed;
static volatile uint32_t s_fifo_off;    /* the ISR's DPC turned PGRAPH_FIFO off */
/* The parameter as it was when the ISR cleared the
 * notify, and how often the ISR then read another. */
static volatile uint32_t s_ack_data, s_ack_armed, s_ack_changed;
static struct timespec s_t0;

static int page_index(uint32_t off)
{
    switch (off & ~(PAGE - 1)) {
    case PG_PMC:    return 0;
    case PG_PTIMER: return 1;
    case PG_PCRTC:  return 2;
    case PG_PFB:    return s_pfb ? 3 : -1;
    case PG_PGRAPH: return s_pfb ? 4 : -1;
    default:        return -1;
    }
}

static uint32_t store_rd(uint32_t off)
{
    int p = page_index(off);
    uint32_t v;
    if (p < 0) return 0;
    memcpy(&v, &s_store[p][off & (PAGE - 1) & ~3u], 4);
    return v;
}

static void store_wr(uint32_t off, uint32_t v)
{
    int p = page_index(off);
    if (p < 0) return;
    memcpy(&s_store[p][off & (PAGE - 1) & ~3u], &v, 4);
}

/* Notifies queued for the ISR (RECOMP_NV2A_NOTIFY_ASYNC, on by default):
 * the executor records one and goes on, and the next is put in the trap
 * registers once D3D's handler has finished the one before -- when it
 * turns FIFO access back on with the interrupt cleared. One at a time,
 * each with its own parameter, which is all the handler needs; the
 * executor stopping until each was handled (as the GPU does) was about 5%
 * of its time in a Switch race, waiting on another core. */
#define NQ 64
static struct { uint32_t addr, data; } s_nq[NQ];
static unsigned s_nq_head, s_nq_tail;   /* pushed / presented, ever */
static int s_presented;                 /* one is in the trap registers */
static volatile int s_nq_lock;

static void nq_lock(void)
{
    while (__atomic_test_and_set(&s_nq_lock, __ATOMIC_ACQUIRE))
        Sleep(0);
}

static void nq_unlock(void)
{
    __atomic_clear(&s_nq_lock, __ATOMIC_RELEASE);
}

/* Under nq_lock: the next queued notify into the trap registers, if the
 * last is done with. */
static void nq_present(void)
{
    uint32_t i;
    if (s_presented || s_nq_tail == s_nq_head || __atomic_load_n(&s_fifo_off, __ATOMIC_SEQ_CST)
            || (__atomic_load_n(&s_pgraph_intr, __ATOMIC_SEQ_CST) & PGRAPH_INTR_ERROR))
        return;
    i = s_nq_tail++ % NQ;
    store_wr(PGRAPH_TRAPPED_ADDR, s_nq[i].addr);
    store_wr(PGRAPH_TRAPPED_DATA, s_nq[i].data);
    store_wr(PGRAPH_NSOURCE, 1u);
    s_presented = 1;
    __atomic_or_fetch(&s_pgraph_intr, PGRAPH_INTR_ERROR, __ATOMIC_SEQ_CST);
    {
        extern void xbox_KernelTimerWake(void);
        xbox_KernelTimerWake();         /* with RECOMP_NV2A_IRQ_WAKE only */
    }
}

/* PTIMER's count: host time in core-clock ticks, scaled by the guest's
 * DENOMINATOR/NUMERATOR (xemu's ptimer_get_clock). */
static uint64_t ptimer_clock(void)
{
    struct timespec now;
    uint64_t ns, num = store_rd(PTIMER_NUMERATOR), den = store_rd(PTIMER_DENOMINATOR);
    unsigned __int128 t;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ns = (uint64_t)(now.tv_sec - s_t0.tv_sec) * 1000000000ull
       + (uint64_t)(now.tv_nsec - s_t0.tv_nsec);
    if (!num) num = 1;
    if (!den) den = 1;
    t = (unsigned __int128)ns * NV2A_CORE_HZ / 1000000000ull;
    return (uint64_t)(t * den / num);
}

static uint32_t ptimer_time0(void) { return (uint32_t)((ptimer_clock() & 0x7FFFFFFull) << 5); }

static uint32_t pmc_pending(void)
{
    uint32_t v = 0;
    if (s_pcrtc_intr & store_rd(PCRTC_INTR_EN_0))   v |= PMC_INTR_PCRTC;
    if (s_ptimer_intr & store_rd(PTIMER_INTR_EN_0)) v |= PMC_INTR_PTIMER;
    if (s_pgraph_intr & store_rd(PGRAPH_INTR_EN))   v |= PMC_INTR_PGRAPH;
    return v;
}

static uint64_t reg_read(void *dev, uint32_t off, int size)
{
    uint32_t v;
    (void)dev;
    switch (off & ~3u) {
    case PMC_INTR_0:     v = pmc_pending(); break;
    case PCRTC_INTR_0:   v = s_pcrtc_intr; break;
    case PTIMER_INTR_0:  v = s_ptimer_intr; break;
    case PTIMER_TIME_0:  v = ptimer_time0(); break;
    case PGRAPH_INTR:    v = s_pgraph_intr; break;
    case PGRAPH_TRAPPED_DATA:
        v = store_rd(off);
        if (__atomic_exchange_n(&s_ack_armed, 0, __ATOMIC_SEQ_CST) && v != s_ack_data) {
            uint32_t n = __atomic_add_fetch(&s_ack_changed, 1, __ATOMIC_SEQ_CST);
            if (n <= 5)
                fprintf(stderr, "[NOTIFY] parameter %08X cleared, %08X read: overwritten (%u)\n",
                        s_ack_data, v, n);
        }
        break;
    case PTIMER_TIME_1:  v = (uint32_t)((ptimer_clock() >> 27) & 0x1FFFFFFFull); break;
    default:             v = store_rd(off); break;
    }
    v >>= (off & 3u) * 8;
    return size >= 4 ? v : (v & ((1u << (size * 8)) - 1));
}

static void reg_write(void *dev, uint32_t off, uint64_t val, int size)
{
    uint32_t v = (uint32_t)val;
    (void)dev;
    if (size < 4) {                          /* merge a narrow write */
        uint32_t sh = (off & 3u) * 8, mask = ((1u << (size * 8)) - 1) << sh;
        v = (store_rd(off) & ~mask) | ((v << sh) & mask);
    }
    switch (off & ~3u) {
    case PMC_INTR_0:     break;                              /* read-only */
    case PCRTC_INTR_0:   __atomic_and_fetch(&s_pcrtc_intr, ~v, __ATOMIC_SEQ_CST); break;
    case PTIMER_INTR_0:  __atomic_and_fetch(&s_ptimer_intr, ~v, __ATOMIC_SEQ_CST); break;
    case PGRAPH_INTR:
        if (v & PGRAPH_INTR_ERROR) {
            s_ack_data = store_rd(PGRAPH_TRAPPED_DATA);
            __atomic_store_n(&s_ack_armed, 1, __ATOMIC_SEQ_CST);
        }
        __atomic_and_fetch(&s_pgraph_intr, ~v, __ATOMIC_SEQ_CST);
        break;
    case PFB_WBC:
        /* The flush is done the moment it is asked for. D3D's kick sets the
         * bit and spins until it clears; when the ack thread cleared it, the
         * spin lasted until that thread finished executing whatever
         * pushbuffer it was in the middle of, so every kick waited on the
         * software GPU and the game and the GPU never overlapped. GET is what
         * holds the title back when it gets ahead, as on hardware. */
        store_wr(off, v & ~PFB_WBC_FLUSH);
        xbox_Nv2aAckWake();                 /* new work, if the executor idles */
        break;
        /* With RECOMP_NV2A_PFB_ASYNC. RalliSport Challenge 2's D3D, once it
         * is ahead of the GPU and short of push-buffer room, patches a
         * NO_OPERATION with a parameter into space the GPU has not reached
         * (0x00368002) and waits for the notify interrupt it raises
         * (xbox_Nv2aIrqNotify); it also polls PGRAPH PATT_COLOR0, its
         * progress counter, which the executor writes. Without those it
         * hung on the first race load. */
    case PGRAPH_FIFO:
        /* D3D's PGRAPH handler (0x0036C070) turns method fetch off while it
         * works and back on when done; the next queued notify waits for
         * that (xbox_Nv2aIrqNotify). */
        store_wr(off, v);
        __atomic_store_n(&s_fifo_off, !(v & 1u), __ATOMIC_SEQ_CST);
        if (v & 1u) {
            nq_lock();
            if (s_presented && !(__atomic_load_n(&s_pgraph_intr, __ATOMIC_SEQ_CST)
                                 & PGRAPH_INTR_ERROR))
                s_presented = 0;            /* handled: the next one */
            nq_present();
            nq_unlock();
        }
        break;
    case PTIMER_ALARM_0:
        store_wr(off, v);
        s_alarm = v;
        __atomic_store_n(&s_alarm_armed, 1, __ATOMIC_SEQ_CST);
        break;
    default:             store_wr(off, v); break;
    }
}

int xbox_Nv2aIrqEnabled(void) { return __atomic_load_n(&s_enabled, __ATOMIC_SEQ_CST); }

/* The PFB flush done at once (RECOMP_NV2A_PFB_ASYNC): the title can be
 * ahead of the executor. */
int xbox_Nv2aIrqAsync(void) { return s_enabled && s_pfb; }

int xbox_Nv2aIrqOwns(uint32_t va)
{
    return s_enabled && va >= NV2A_BASE && va < NV2A_BASE + 0x01000000u
        && page_index(va - NV2A_BASE) >= 0;
}

int xbox_Nv2aIrqHandleMmio(void *ctx, uint32_t va)
{
#ifdef MMIO_FAULT_HOST
    if (!xbox_Nv2aIrqOwns(va))
        return 0;
    if (!mmio_emulate((PCONTEXT)ctx, va - NV2A_BASE, NULL, reg_read, reg_write)) {
        static int shown;
        const uint8_t *ip = (const uint8_t *)CONTEXT_PC((PCONTEXT)ctx);
        if (shown++ < 20)
            fprintf(stderr, "  [NV2A-IRQ] undecoded access at +0x%06X: "
                    "%02X %02X %02X %02X %02X %02X\n", va - NV2A_BASE,
                    ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        return 0;
    }
    return 1;
#else
    (void)ctx; (void)va;
    return 0;
#endif
}

uint32_t xbox_Nv2aIrqPeek(uint32_t off)
{
    return (uint32_t)reg_read(NULL, off, 4);
}

/* A PGRAPH register the executor sets (PATT_COLOR0): into the model's
 * page when it serves PGRAPH, else 0 (the caller writes plain memory). */
int xbox_Nv2aIrqPoke(uint32_t off, uint32_t v)
{
    if (!s_enabled || page_index(off) < 0)
        return 0;
    store_wr(off, v);
    return 1;
}

/* RECOMP_PERF: notifies since the last call, and the executor's time in
 * xbox_Nv2aIrqNotify. */
static uint64_t s_notify_ns;
static uint32_t s_notify_n;

void xbox_Nv2aIrqNotifyStats(uint32_t *n, uint64_t *ns)
{
    *n = s_notify_n;
    *ns = s_notify_ns;
    s_notify_n = 0;
    s_notify_ns = 0;
}

/* Half a second without the ISR: say so (a few times) and go on. */
static int notify_late(const struct timespec *t0, uint32_t param)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000 <= 500)
        return 0;
    {
        static int shown;
        if (shown++ < 5)
            fprintf(stderr, "  [NV2A-IRQ] notify %08X not acknowledged in 500 ms\n", param);
    }
    return 1;
}

/* The executor met a NO_OPERATION whose parameter is not 0: on the NV2A a
 * notify, trapped for the CPU -- PGRAPH_INTR's ERROR with NSOURCE
 * NOTIFICATION and the method and parameter in TRAPPED_ADDR / DATA_LOW --
 * and the GPU waits until the ISR clears it (xemu's pgraph.c). 0 when
 * PGRAPH is not modelled (RECOMP_NV2A_PFB_ASYNC).
 *
 * D3D's handler (0x0036C070) turns PGRAPH_FIFO off, clears the interrupt
 * and only then reads TRAPPED_DATA (the parameter) and acts on it. A notify
 * put in the trap registers at the clear alone could overwrite the
 * parameter first: a 5 ("push-buffer room", which sets the event D3D waits
 * on at 0x0036809B) was handled as another, and the game froze. So the next
 * goes in only once FIFO access is back on. By default the executor queues
 * the notify and goes on (nq_present delivers it); RECOMP_NV2A_NOTIFY_ASYNC=0
 * waits for each, as the GPU does, and RECOMP_NV2A_NOTIFY_FIFO=0 for the
 * clear alone, as before. */
int xbox_Nv2aIrqNotify(uint32_t subch, uint32_t method, uint32_t param)
{
    struct timespec t0, now;
    static int gate = -1, async = -1;
    if (!s_enabled || !s_pfb)
        return 0;
    if (gate < 0) {
        const char *e = getenv("RECOMP_NV2A_NOTIFY_FIFO");
        gate = !e || strcmp(e, "0") != 0;
        e = getenv("RECOMP_NV2A_NOTIFY_ASYNC");
        async = gate && (!e || strcmp(e, "0") != 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (async) {
        nq_lock();
        while (s_nq_head - s_nq_tail >= NQ) {   /* full: the ISR is far behind */
            nq_unlock();
            if (notify_late(&t0, param)) {
                nq_lock();
                s_presented = 0;                /* given up on: the next */
                __atomic_and_fetch(&s_pgraph_intr, ~PGRAPH_INTR_ERROR, __ATOMIC_SEQ_CST);
                nq_present();
                break;
            }
            Sleep(0);
            nq_lock();
        }
        s_nq[s_nq_head % NQ].addr = (method & 0x1FFFu) | ((subch & 7u) << 16);
        s_nq[s_nq_head % NQ].data = param;
        s_nq_head++;
        nq_present();
        nq_unlock();
    } else {
        /* A handler still at work (FIFO access off) has yet to read the
         * trap registers this would overwrite. */
        while (gate && __atomic_load_n(&s_fifo_off, __ATOMIC_SEQ_CST) && !notify_late(&t0, param))
            Sleep(0);
        store_wr(PGRAPH_TRAPPED_ADDR, (method & 0x1FFFu) | ((subch & 7u) << 16));
        store_wr(PGRAPH_TRAPPED_DATA, param);
        store_wr(PGRAPH_NSOURCE, 1u);
        __atomic_or_fetch(&s_pgraph_intr, PGRAPH_INTR_ERROR, __ATOMIC_SEQ_CST);
        {
            extern void xbox_KernelTimerWake(void);
            xbox_KernelTimerWake();
        }
        while ((__atomic_load_n(&s_pgraph_intr, __ATOMIC_SEQ_CST) & PGRAPH_INTR_ERROR)
                || (gate && __atomic_load_n(&s_fifo_off, __ATOMIC_SEQ_CST))) {
            if (notify_late(&t0, param)) {
                __atomic_and_fetch(&s_pgraph_intr, ~PGRAPH_INTR_ERROR, __ATOMIC_SEQ_CST);
                break;
            }
            Sleep(0);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    s_notify_n++;
    s_notify_ns += (uint64_t)(now.tv_sec - t0.tv_sec) * 1000000000u
                 + (uint64_t)(now.tv_nsec - t0.tv_nsec);
    return 1;
}

void xbox_Nv2aIrqRaiseVblank(void)
{
    __atomic_or_fetch(&s_pcrtc_intr, 1u, __ATOMIC_SEQ_CST);
}

/* Advance the alarm, and say whether the line is asserted: something is
 * pending and enabled, and PMC_INTR_EN_0 lets hardware interrupts through. */
int xbox_Nv2aIrqTick(void)
{
    if (!s_enabled)
        return 0;
    if (__atomic_load_n(&s_alarm_armed, __ATOMIC_SEQ_CST)
            && (int32_t)(ptimer_time0() - s_alarm) >= 0) {
        __atomic_store_n(&s_alarm_armed, 0, __ATOMIC_SEQ_CST);
        __atomic_or_fetch(&s_ptimer_intr, 1u, __ATOMIC_SEQ_CST);
    }
    return pmc_pending() && (store_rd(PMC_INTR_EN_0) & 1u);
}

void xbox_Nv2aIrqInit(void)
{
    static const uint32_t pages[5] = { PG_PMC, PG_PTIMER, PG_PCRTC, PG_PFB, PG_PGRAPH };
    ptrdiff_t off = xbox_GetMemoryOffset();
    int i, npages;

    if (s_enabled || !getenv("RECOMP_NV2A_IRQ"))
        return;
    s_pfb = getenv("RECOMP_NV2A_PFB_ASYNC") && strcmp(getenv("RECOMP_NV2A_PFB_ASYNC"), "0") != 0;
    npages = s_pfb ? 5 : 3;
    if (!off) {
        fprintf(stderr, "  NV2A-IRQ: guest memory not mapped yet; disabled\n");
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &s_t0);
    for (i = 0; i < npages; i++)
        memcpy(s_store[i], (void *)((uintptr_t)off + NV2A_BASE + pages[i]), PAGE);
    /* On before the pages close: the ack thread checks this to keep its
     * hands off them, and it runs concurrently with this. */
    __atomic_store_n(&s_enabled, 1, __ATOMIC_SEQ_CST);
    for (i = 0; i < npages; i++) {
        void *at = (void *)((uintptr_t)off + NV2A_BASE + pages[i]);
        DWORD old;
        if (!VirtualProtect(at, PAGE, PAGE_NOACCESS, &old)) {
            fprintf(stderr, "  NV2A-IRQ: cannot trap page +0x%06X; disabled\n", pages[i]);
            while (--i >= 0)
                VirtualProtect((void *)((uintptr_t)off + NV2A_BASE + pages[i]),
                               PAGE, PAGE_READWRITE, &old);
            __atomic_store_n(&s_enabled, 0, __ATOMIC_SEQ_CST);
            return;
        }
    }
    fprintf(stderr, "  NV2A-IRQ: PMC, PTIMER and PCRTC interrupt registers modelled%s\n",
            npages > 3 ? " (and the PFB flush and PGRAPH's notify)" : "");
}
