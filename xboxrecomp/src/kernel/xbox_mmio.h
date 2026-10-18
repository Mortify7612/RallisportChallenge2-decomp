/*
 * xbox_mmio.h - device registers reached by direct call (see xbox_mmio.c).
 */
#ifndef XBOX_MMIO_H
#define XBOX_MMIO_H

#include <stdint.h>

/* The same shapes as mmio_decode.h's callbacks, so a model's read and write
 * functions serve both the direct path and the fault path. */
typedef uint64_t (*xbox_mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*xbox_mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

/* Guest accesses to [base, base + len) go to rd / wr with offsets from
 * off_base. A NULL callback leaves that direction plain memory. Register
 * before guest code runs. */
void xbox_MmioRegister(uint32_t base, uint32_t len, uint32_t off_base, void *dev,
                       xbox_mmio_read_fn rd, xbox_mmio_write_fn wr);

/* RECOMP_MMIO_TRAPS=1: arm the old page traps as well. */
int xbox_MmioTraps(void);

/* Called by generated code (recomp_types.h) for every access at or above
 * 0xFD000000. */
uint64_t xbox_mmio_read(uint32_t va, int size);
void xbox_mmio_write(uint32_t va, uint64_t value, int size);

#endif
