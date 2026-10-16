/*
 * host_coro.h -- minimal stackful coroutines (AArch64, x86-64).
 *
 * The XAPI fiber HLE in rsc2_overrides.c needs one host stack per guest
 * fiber. glibc has ucontext for that; Android's bionic does not, so there
 * this does the switch. RSC2_HOST_CORO=1 at build time selects it on desktop
 * Linux too, which is how it is tested.
 *
 *   sp = coro_init(stack, size, fn, arg);   a coroutine that will run fn(arg)
 *   coro_switch(&my_sp, sp);                 run it; returns when someone
 *                                            switches back to my_sp
 *
 * fn must never return: a fiber ends by switching away for good.
 */
#ifndef RSC2_HOST_CORO_H
#define RSC2_HOST_CORO_H

#include <stddef.h>

void *coro_init(void *stack, size_t size, void (*fn)(void *), void *arg);
void  coro_switch(void **save_sp, void *to_sp);

#endif
