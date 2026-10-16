/*
 * host_coro.c -- the stack switch behind the XAPI fiber HLE where there is no
 * ucontext (Android's libc has none). See host_coro.h.
 *
 * A switch pushes the callee-saved registers on the current stack, stores
 * that stack pointer, loads the other one and pops its registers, ending in
 * its return address: everything else a coroutine needs is the compiler's
 * job, since coro_switch is an ordinary call to it.
 */
#include "host_coro.h"

#include <stdint.h>
#include <stdlib.h>

#if defined(__aarch64__)
/* x19-x28, fp, lr and d8-d15: what AAPCS64 says a callee keeps. 160 bytes,
 * rounded to 176 to keep sp 16-aligned. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl coro_switch\n"
    ".type coro_switch, %function\n"
    "coro_switch:\n"
    "    sub  sp, sp, #176\n"
    "    stp  x19, x20, [sp, #0]\n"
    "    stp  x21, x22, [sp, #16]\n"
    "    stp  x23, x24, [sp, #32]\n"
    "    stp  x25, x26, [sp, #48]\n"
    "    stp  x27, x28, [sp, #64]\n"
    "    stp  x29, x30, [sp, #80]\n"
    "    stp  d8,  d9,  [sp, #96]\n"
    "    stp  d10, d11, [sp, #112]\n"
    "    stp  d12, d13, [sp, #128]\n"
    "    stp  d14, d15, [sp, #144]\n"
    "    mov  x2, sp\n"
    "    str  x2, [x0]\n"
    "    mov  sp, x1\n"
    "    ldp  x19, x20, [sp, #0]\n"
    "    ldp  x21, x22, [sp, #16]\n"
    "    ldp  x23, x24, [sp, #32]\n"
    "    ldp  x25, x26, [sp, #48]\n"
    "    ldp  x27, x28, [sp, #64]\n"
    "    ldp  x29, x30, [sp, #80]\n"
    "    ldp  d8,  d9,  [sp, #96]\n"
    "    ldp  d10, d11, [sp, #112]\n"
    "    ldp  d12, d13, [sp, #128]\n"
    "    ldp  d14, d15, [sp, #144]\n"
    "    add  sp, sp, #176\n"
    "    ret\n"
    ".size coro_switch, .-coro_switch\n"
    /* First activation lands here with fn in x19 and its argument in x20. */
    ".p2align 4\n"
    "coro_start:\n"
    "    mov  x0, x20\n"
    "    mov  x29, #0\n"
    "    blr  x19\n"
    "    brk  #0\n");

extern char coro_start[];

void *coro_init(void *stack, size_t size, void (*fn)(void *), void *arg)
{
    uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
    uint64_t *f = (uint64_t *)(top - 176);
    int i;
    for (i = 0; i < 176 / 8; i++)
        f[i] = 0;
    f[0] = (uint64_t)(uintptr_t)fn;          /* x19 */
    f[1] = (uint64_t)(uintptr_t)arg;         /* x20 */
    f[11] = (uint64_t)(uintptr_t)coro_start; /* x30: where the ret goes */
    return f;
}

#elif defined(__x86_64__)
/* rbx, rbp, r12-r15 (System V); the return address is the resume point. */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl coro_switch\n"
    ".type coro_switch, @function\n"
    "coro_switch:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq  %rsp, (%rdi)\n"
    "    movq  %rsi, %rsp\n"
    "    popq  %r15\n"
    "    popq  %r14\n"
    "    popq  %r13\n"
    "    popq  %r12\n"
    "    popq  %rbx\n"
    "    popq  %rbp\n"
    "    ret\n"
    ".size coro_switch, .-coro_switch\n"
    /* First activation: fn in r12, its argument in rbx; rsp is 16-aligned
     * here, so the call below enters fn as any call would. */
    ".p2align 4\n"
    "coro_start:\n"
    "    movq  %rbx, %rdi\n"
    "    xorl  %ebp, %ebp\n"
    "    call  *%r12\n"
    "    ud2\n");

extern char coro_start[];

void *coro_init(void *stack, size_t size, void (*fn)(void *), void *arg)
{
    uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
    /* After the six pops and the ret, rsp must be top - 16 (16-aligned). */
    uint64_t *f = (uint64_t *)(top - 16 - 7 * 8);
    f[0] = 0;                                  /* r15 */
    f[1] = 0;                                  /* r14 */
    f[2] = 0;                                  /* r13 */
    f[3] = (uint64_t)(uintptr_t)fn;            /* r12 */
    f[4] = (uint64_t)(uintptr_t)arg;           /* rbx */
    f[5] = 0;                                  /* rbp */
    f[6] = (uint64_t)(uintptr_t)coro_start;    /* return address */
    return f;
}

#else
#error "host_coro.c: no stack switch for this CPU"
#endif
