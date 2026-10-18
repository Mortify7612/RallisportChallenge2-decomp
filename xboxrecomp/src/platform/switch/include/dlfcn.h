/* dlfcn.h for Switch homebrew: one statically linked module, no dynamic
 * loading. dladdr() reports only the module base (platform/nx_host.c), so
 * the crash printer's addresses can be turned into names with addr2line. */
#ifndef NX_DLFCN_H
#define NX_DLFCN_H

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;

int dladdr(const void *addr, Dl_info *info);

#define RTLD_LAZY 1
#define RTLD_NOW  2
#define RTLD_DEFAULT ((void *)0)
static inline void *dlopen(const char *f, int m) { (void)f; (void)m; return 0; }
static inline void *dlsym(void *h, const char *s) { (void)h; (void)s; return 0; }
static inline int dlclose(void *h) { (void)h; return 0; }
static inline const char *dlerror(void) { return "no dynamic loading on Switch"; }

#endif
