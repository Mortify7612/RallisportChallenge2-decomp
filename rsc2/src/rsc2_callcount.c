/*
 * RSC2_CALLCOUNT=<file>: per-guest-function call counts, in 5-second windows.
 *
 * Only active in a build whose generated sources were instrumented by
 * tools/callcount_instrument.py (every sub_XXXXXXXX gets a counter that
 * registers itself on its first call). Each window appends one block to
 * <file>: "t=<seconds>" and then "<va> <calls in this window>" for every
 * function called in it. Comparing two windows -- a scene where something is
 * drawn and one where it is not -- names the code that stopped running.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/host_backtrace.h"   /* backtrace(), also on Android */
#include <pthread.h>
#include <unistd.h>
#endif

#define CC_MAX 65536

static struct { uint32_t va; volatile unsigned long *n; unsigned long last; } s_cc[CC_MAX];
static volatile long s_cc_count;

void rsc2_callcount_register(uint32_t va, volatile unsigned long *n)
{
    long i = __atomic_fetch_add(&s_cc_count, 1, __ATOMIC_RELAXED);
#ifndef _WIN32
    {
        /* RSC2_CC_STACK=<va>: how that function was first reached. */
        static long want = -1;
        if (want < 0) {
            const char *e = getenv("RSC2_CC_STACK");
            want = e ? (long)strtoul(e, NULL, 16) : 0;
        }
        if (want && (uint32_t)want == va) {
            void *bt[64];
            int k, nb = backtrace(bt, 64);
            fprintf(stderr, "[CC-STACK] first call of %08X:\n", va);
            backtrace_symbols_fd(bt, nb, 2);
            (void)k;
        }
    }
#endif
    if (i < CC_MAX) {
        s_cc[i].va = va;
        s_cc[i].n = n;
    }
}

static void cc_dump(FILE *f, unsigned secs)
{
    long i, n = s_cc_count < CC_MAX ? s_cc_count : CC_MAX;
    fprintf(f, "t=%u\n", secs);
    for (i = 0; i < n; i++) {
        unsigned long now;
        if (!s_cc[i].n)
            continue;
        now = *s_cc[i].n;
        if (now != s_cc[i].last)
            fprintf(f, "%08X %lu\n", s_cc[i].va, now - s_cc[i].last);
        s_cc[i].last = now;
    }
    fflush(f);
}

#ifndef _WIN32
static void *cc_thread(void *arg)
{
    FILE *f = (FILE *)arg;
    unsigned t = 0;
    for (;;) {
        sleep(5);
        t += 5;
        cc_dump(f, t);
    }
    return NULL;
}

__attribute__((constructor)) static void cc_init(void)
{
    const char *out = getenv("RSC2_CALLCOUNT");
    FILE *f;
    pthread_t th;
    if (!out || !*out)
        return;
    f = fopen(out, "w");
    if (!f)
        return;
    pthread_create(&th, NULL, cc_thread, f);
    pthread_detach(th);
}
#endif
