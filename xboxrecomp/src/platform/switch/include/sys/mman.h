/* sys/mman.h for Switch homebrew, which has none: the subset the POSIX
 * platform layer uses, implemented on Horizon's memory syscalls in
 * platform/nx_host.c. No page aliasing: a second mapping of one memfd fails
 * (titles that need RAM mirrors cannot run; RSC2 does not). */
#ifndef NX_SYS_MMAN_H
#define NX_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_ANON            MAP_ANONYMOUS
#define MAP_NORESERVE       0x4000
#define MAP_FIXED_NOREPLACE 0x100000

#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);

/* An anonymous mapping object the size of the first mmap of it. */
int memfd_create(const char *name, unsigned int flags);

#endif
