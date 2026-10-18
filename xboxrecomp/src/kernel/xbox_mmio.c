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

uint64_t xbox_mmio_read(uint32_t va, int size)
{
    int i = find(va);
    uint64_t v = 0;
    if (i >= 0 && s_mmio[i].rd)
        return s_mmio[i].rd(s_mmio[i].dev, va - s_mmio[i].off_base, size);
    memcpy(&v, plain(va), (size_t)size);
    return v;
}

void xbox_mmio_write(uint32_t va, uint64_t value, int size)
{
    int i = find(va);
    if (i >= 0 && s_mmio[i].wr) {
        s_mmio[i].wr(s_mmio[i].dev, va - s_mmio[i].off_base, value, size);
        return;
    }
    memcpy(plain(va), &value, (size_t)size);
}
