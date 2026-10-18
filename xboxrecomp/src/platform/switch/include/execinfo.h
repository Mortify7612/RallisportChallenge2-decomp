/* execinfo.h for Switch homebrew: a frame-pointer walk (platform/nx_host.c).
 * Symbol names are not available; addr2line on the .elf turns the
 * addresses (printed relative to the module base) into names. */
#ifndef NX_EXECINFO_H
#define NX_EXECINFO_H

int backtrace(void **buffer, int size);
char **backtrace_symbols(void *const *buffer, int size);
void backtrace_symbols_fd(void *const *buffer, int size, int fd);

#endif
