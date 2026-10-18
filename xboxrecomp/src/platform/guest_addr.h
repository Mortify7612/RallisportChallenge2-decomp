/*
 * guest_addr.h - where a guest address lives in host memory.
 *
 * Guest memory is one host range at g_xbox_mem_offset (xbox_GetMemoryOffset),
 * and a guest address's host pointer is that base plus XBOX_FOLD(va). On most
 * hosts XBOX_FOLD is the identity, and the range spans the guest's whole
 * 4 GB. A host that cannot reserve 4 GB at a place of its choosing (the
 * Switch: only its stack region takes mappings at chosen addresses, and that
 * is under 2 GB) builds with XBOX_FOLD_HIGH, which moves the top half of the
 * guest space down into the second quarter: bits 31 and 30 flip for an
 * address with bit 31 set, so
 *
 *   0x80000000  contiguous window  -> 0x40000000
 *   0xF0000000  tiled aperture     -> 0x40000000   (the contiguous window)
 *   0xFD000000  NV2A registers     -> 0x3D000000
 *   0xFE800000  MCPX devices       -> 0x3E800000
 *   0xFF000000  flash              -> 0x3F000000
 *
 * and the range needs only 0x44000000 bytes. The tiled aperture is an alias
 * of the contiguous window, which a host without file-mapping views (the
 * Switch again) cannot map twice, so its first 64 MB fold onto the window
 * itself. Low guest addresses are where
 * they were; a title using 0x30000000-0x7FFFFFFF itself cannot fold (RSC2
 * maps nothing there). Within a window addresses stay contiguous, so a
 * pointer plus a length that stays inside one is fine.
 *
 * The generated code's XBOX_PTR (templates/runtime/recomp_types.h) does the
 * same; everything hand-written goes through these.
 */
#ifndef GUEST_ADDR_H
#define GUEST_ADDR_H

#include <stdint.h>
#include <stddef.h>

#if defined(XBOX_FOLD_HIGH)
/* Functions rather than macros, so an argument is evaluated once. */
static inline uint32_t xbox_fold(uint32_t va)
{
    return (va ^ ((uint32_t)((int32_t)va >> 31) << 30))
         + ((uint32_t)(va - 0xF0000000u < 0x04000000u) << 28);
}
/* Host offset back to the guest address (a fault address, say). */
static inline uint32_t xbox_unfold(uint32_t o)
{
    return o >= 0x30000000u ? o ^ 0xC0000000u : o;   /* tiled: as contiguous */
}
#define XBOX_FOLD(va)  xbox_fold((uint32_t)(va))
#define XBOX_UNFOLD(o) xbox_unfold((uint32_t)(o))
#else
#define XBOX_FOLD(va)  ((uint32_t)(va))
#define XBOX_UNFOLD(o) ((uint32_t)(o))
#endif

/* The host pointer for guest address va, given the base (the memory
 * offset). */
#define XBOX_HOST_AT(base, va) ((void *)((uintptr_t)(base) + XBOX_FOLD(va)))

#endif
