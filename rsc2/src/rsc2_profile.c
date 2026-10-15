/*
 * RSC2_PROFILE=<file>: a sampling profiler for hosts without perf.
 *
 * ITIMER_PROF delivers SIGPROF to whichever thread is burning CPU; the
 * handler records that thread and its RIP. A reporter thread rewrites <file>
 * every few seconds with the hottest functions per thread, so a run killed
 * by timeout still leaves a report. Static functions in the runtime have no
 * dynamic symbol; they show as rsc2+0x... -- feed that to addr2line -f.
 *
 * RSC2_PROFILE_HZ sets the rate (default 1000); RSC2_PROFILE_DELAY=<s>
 * starts sampling that many seconds in, to profile one phase (a race) alone.
 * RSC2_PROFILE_WALL=1 samples every thread on wall-clock time instead of CPU
 * time, so a thread that is blocked shows where it is blocked -- CPU-time
 * samples go mostly to whichever threads spin.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define MAX_SAMPLES (1u << 22)

static struct { uint32_t tid; uintptr_t rip; } *s_samples;
static volatile uint32_t s_count;
static const char *s_out;

static void on_prof(int sig, siginfo_t *si, void *uc)
{
    uint32_t i;
    (void)sig; (void)si;
    i = __atomic_fetch_add(&s_count, 1, __ATOMIC_RELAXED);
    if (i >= MAX_SAMPLES)
        return;
    s_samples[i].tid = (uint32_t)syscall(SYS_gettid);
    s_samples[i].rip = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
}

struct bucket { uint32_t tid; uintptr_t key; const char *name; uint32_t n; };

static int by_count(const void *a, const void *b)
{
    const struct bucket *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

static void report(void)
{
    uint32_t n = s_count < MAX_SAMPLES ? s_count : MAX_SAMPLES, i, j, nb = 0;
    struct bucket *b = calloc(65536, sizeof *b);
    uint32_t tids[64] = {0}, tcount[64] = {0}, nt = 0;
    FILE *f;

    if (!b)
        return;
    for (i = 0; i < n; i++) {
        Dl_info di;
        uintptr_t key = s_samples[i].rip;
        const char *name = NULL;
        if (dladdr((void *)key, &di)) {
            if (di.dli_sname) {
                key = (uintptr_t)di.dli_saddr;
                name = di.dli_sname;
            } else {
                key = (key - (uintptr_t)di.dli_fbase) & ~(uintptr_t)0x3F;
                name = di.dli_fname;
            }
        }
        for (j = 0; j < nt && tids[j] != s_samples[i].tid; j++) ;
        if (j == nt && nt < 64) tids[nt++] = s_samples[i].tid;
        if (j < 64) tcount[j]++;
        for (j = 0; j < nb; j++)
            if (b[j].tid == s_samples[i].tid && b[j].key == key)
                break;
        if (j == nb) {
            if (nb == 65536)
                continue;
            b[nb].tid = s_samples[i].tid;
            b[nb].key = key;
            b[nb].name = name;
            nb++;
        }
        b[j].n++;
    }
    qsort(b, nb, sizeof *b, by_count);
    f = fopen(s_out, "w");
    if (f) {
        fprintf(f, "%u samples\n", n);
        for (j = 0; j < nt; j++)
            fprintf(f, "thread %u: %u samples (%.1f%%)\n", tids[j], tcount[j],
                    n ? 100.0 * tcount[j] / n : 0.0);
        for (i = 0; i < nb && i < 1500; i++) {
            if (b[i].name && strchr(b[i].name, '/'))
                fprintf(f, "%6u %5.1f%%  tid %u  %s+0x%lx\n", b[i].n,
                        100.0 * b[i].n / n, b[i].tid, strrchr(b[i].name, '/') + 1,
                        (unsigned long)b[i].key);
            else
                fprintf(f, "%6u %5.1f%%  tid %u  %s\n", b[i].n, 100.0 * b[i].n / n,
                        b[i].tid, b[i].name ? b[i].name : "?");
        }
        fclose(f);
    }
    free(b);
}

static struct itimerval s_it;

static void *reporter(void *unused)
{
    const char *d = getenv("RSC2_PROFILE_DELAY");
    (void)unused;
    if (d && atoi(d) > 0)
        sleep((unsigned)atoi(d));
    if (getenv("RSC2_PROFILE_WALL")) {
        /* Signal every thread, each period. The thread list is re-read once
         * a second; this thread skips itself. */
        pid_t pid = getpid(), self = (pid_t)syscall(SYS_gettid), tids[256];
        int nt = 0, i;
        long ticks = 0, per_s = 1000000 / (long)s_it.it_interval.tv_usec;
        struct timespec ts = { 0, (long)s_it.it_interval.tv_usec * 1000 };
        for (;;) {
            if (ticks % per_s == 0) {
                DIR *dir = opendir("/proc/self/task");
                struct dirent *de;
                nt = 0;
                while (dir && (de = readdir(dir)) && nt < 256) {
                    pid_t t = (pid_t)atoi(de->d_name);
                    if (t > 0 && t != self)
                        tids[nt++] = t;
                }
                if (dir) closedir(dir);
            }
            for (i = 0; i < nt; i++)
                syscall(SYS_tgkill, pid, tids[i], SIGPROF);
            nanosleep(&ts, NULL);
            if (++ticks % (per_s * 5) == 0)
                report();
        }
    }
    setitimer(ITIMER_PROF, &s_it, NULL);
    for (;;) {
        sleep(5);
        report();
    }
    return NULL;
}

__attribute__((constructor)) static void rsc2_profile_init(void)
{
    struct sigaction sa;
    pthread_t th;
    const char *hz_s = getenv("RSC2_PROFILE_HZ");
    long hz = hz_s ? atol(hz_s) : 1000;

    s_out = getenv("RSC2_PROFILE");
    if (!s_out || !*s_out)
        return;
    s_samples = calloc(MAX_SAMPLES, sizeof *s_samples);
    if (!s_samples)
        return;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    if (hz < 10) hz = 10;
    s_it.it_interval.tv_sec = 0;
    s_it.it_interval.tv_usec = 1000000 / hz;
    s_it.it_value = s_it.it_interval;
    pthread_create(&th, NULL, reporter, NULL);
    pthread_detach(th);
}
#endif
