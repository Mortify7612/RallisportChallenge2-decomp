/*
 * host_backtrace.h -- backtrace() and backtrace_symbols_fd() everywhere.
 *
 * glibc has them in <execinfo.h>; Android's bionic only from API 33. Below
 * that they are built here on the unwinder and dladdr(), which is all the
 * crash reports and write watches need: addresses and symbol names.
 */
#ifndef HOST_BACKTRACE_H
#define HOST_BACKTRACE_H

#if defined(__ANDROID__) && __ANDROID_API__ < 33
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <unwind.h>

struct host_bt { void **buf; int n, max; };

static _Unwind_Reason_Code host_bt_step(struct _Unwind_Context *ctx, void *arg)
{
    struct host_bt *b = (struct host_bt *)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && b->n < b->max)
        b->buf[b->n++] = (void *)pc;
    return b->n < b->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}

static inline int backtrace(void **buf, int max)
{
    struct host_bt b = { buf, 0, max };
    _Unwind_Backtrace(host_bt_step, &b);
    return b.n;
}

static inline void backtrace_symbols_fd(void *const *buf, int n, int fd)
{
    int i;
    for (i = 0; i < n; i++) {
        Dl_info d;
        if (dladdr(buf[i], &d) && d.dli_sname)
            dprintf(fd, "%s(%s+0x%lx) [%p]\n", d.dli_fname ? d.dli_fname : "?", d.dli_sname,
                    (unsigned long)((uintptr_t)buf[i] - (uintptr_t)d.dli_saddr), buf[i]);
        else
            dprintf(fd, "%s [%p]\n", d.dli_fname ? d.dli_fname : "?", buf[i]);
    }
}
#else
#include <execinfo.h>
#endif

#endif /* HOST_BACKTRACE_H */
