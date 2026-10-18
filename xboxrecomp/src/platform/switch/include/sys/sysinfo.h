/* sys/sysinfo.h for Switch homebrew: sysinfo() from the process's memory
 * counters (platform/nx_host.c). */
#ifndef NX_SYS_SYSINFO_H
#define NX_SYS_SYSINFO_H

struct sysinfo {
    unsigned long totalram, freeram, totalswap, freeswap;
    unsigned int mem_unit;
};

int sysinfo(struct sysinfo *info);

#endif
