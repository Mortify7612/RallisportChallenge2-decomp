/* Vectored exception handlers and handle validation in the POSIX shim.
 * See README.md for what each check guarantees. */
#include "win32_compat.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

static int g_failures;
#define CHECK(cond, what) do { \
    if (cond) printf("  ok   %s\n", what); \
    else { printf("  FAIL %s\n", what); g_failures++; } \
} while (0)

static volatile uintptr_t g_seen_addr;
static volatile ULONG_PTR g_seen_write;
static volatile DWORD     g_seen_code;
static volatile int       g_order[4];
static volatile int       g_order_n;
static uint8_t           *g_guard;

/* Recognises a load from the guard page done by read_guard() below and
 * emulates it: result 0x1234 in eax, skip the 2-byte `mov eax, [rdi]`. */
static LONG handler_emulate(PEXCEPTION_POINTERS ep)
{
    PEXCEPTION_RECORD r = ep->ExceptionRecord;
    g_order[g_order_n++ & 3] = 2;
    g_seen_code = r->ExceptionCode;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    g_seen_write = r->ExceptionInformation[0];
    g_seen_addr  = r->ExceptionInformation[1];
    if ((uint8_t *)g_seen_addr < g_guard || (uint8_t *)g_seen_addr >= g_guard + 4096)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_seen_write) {
        ep->ContextRecord->Rip += 2;       /* mov [rdi], eax */
    } else {
        ep->ContextRecord->Rax = 0x1234;
        ep->ContextRecord->Rip += 2;       /* mov eax, [rdi] */
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static volatile int g_step_arm, g_step_seen;

static LONG handler_decline(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        g_step_seen++;
        ep->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_step_arm && ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        g_step_arm = 0;
        ep->ContextRecord->EFlags |= 0x100u;   /* step the next instruction */
    }
    g_order[g_order_n++ & 3] = 1;
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Fixed encodings so the handler knows the instruction length. */
static uint32_t read_guard(void *p)
{
    uint32_t v;
    __asm__ volatile("mov (%%rdi), %%eax" : "=a"(v) : "D"(p) : "memory");
    return v;
}

static void write_guard(void *p, uint32_t v)
{
    __asm__ volatile("mov %%eax, (%%rdi)" : : "a"(v), "D"(p) : "memory");
}

int main(void)
{
    PVOID h1, h2;
    uint32_t v;
    uint8_t not_a_handle[64];

    printf("vectored exception handlers\n");
    g_guard = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(g_guard != MAP_FAILED, "guard page mapped");

    h2 = AddVectoredExceptionHandler(0, handler_emulate);
    h1 = AddVectoredExceptionHandler(1, handler_decline);   /* goes first */
    CHECK(h1 && h2, "handlers registered");

    v = read_guard(g_guard + 0x10);
    CHECK(v == 0x1234, "read fault emulated: destination register written back");
    CHECK(g_seen_code == EXCEPTION_ACCESS_VIOLATION, "code is EXCEPTION_ACCESS_VIOLATION");
    CHECK(g_seen_addr == (uintptr_t)(g_guard + 0x10), "faulting address in ExceptionInformation[1]");
    CHECK(g_seen_write == 0, "read reported as a read");
    CHECK(g_order[0] == 1 && g_order[1] == 2, "First=1 handler runs before the other");

    g_order_n = 0;
    write_guard(g_guard + 0x20, 7);
    CHECK(g_seen_write == 1, "write reported as a write");
    CHECK(g_seen_addr == (uintptr_t)(g_guard + 0x20), "write fault address");

    /* Single step: set TF from the handler, as a page-unprotect-and-step
     * handler does, and expect EXCEPTION_SINGLE_STEP on the next instruction. */
    g_step_seen = 0;
    g_step_arm = 1;
    write_guard(g_guard + 0x30, 9);
    CHECK(g_step_seen == 1, "TF set by a handler arrives as EXCEPTION_SINGLE_STEP");

    CHECK(RemoveVectoredExceptionHandler(h1) == 1, "handler removed");
    CHECK(RemoveVectoredExceptionHandler(h1) == 0, "removing twice fails");
    RemoveVectoredExceptionHandler(h2);

    printf("handle validation\n");
    memset(not_a_handle, 0, sizeof(not_a_handle));
    SetLastError(0);
    CHECK(WaitForSingleObject((HANDLE)not_a_handle, 0) == WAIT_FAILED,
          "wait on a non-handle fails instead of dereferencing it");
    CHECK(GetLastError() == 6u, "...with ERROR_INVALID_HANDLE");
    CHECK(WaitForSingleObject((HANDLE)(uintptr_t)0x00374D2C, 0) == WAIT_FAILED,
          "wait on a raw guest address fails");
    CHECK(!SetEvent((HANDLE)not_a_handle), "SetEvent on a non-handle fails");
    CHECK(!CloseHandle((HANDLE)not_a_handle), "CloseHandle on a non-handle fails");
    {
        HANDLE ev = CreateEventA(NULL, TRUE, TRUE, NULL);
        HANDLE pair[2] = { ev, (HANDLE)not_a_handle };
        CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "a real event still waits");
        CHECK(WaitForMultipleObjects(2, pair, FALSE, 0) == WAIT_FAILED,
              "multi-wait containing a non-handle fails at once");
        CHECK(CloseHandle(ev), "a real event closes");
        CHECK(WaitForSingleObject(ev, 0) == WAIT_FAILED, "a closed handle is no longer valid");
    }
    CHECK(GetProcessHeap() != NULL && HeapAlloc(GetProcessHeap(), 0, 16) != NULL,
          "process heap still usable");

    printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "passed",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures != 0;
}
