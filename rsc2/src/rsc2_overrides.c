/**
 * RalliSport Challenge 2 - hand-written replacements for lifted functions.
 *
 * Every sub_XXXXXXXX defined here is skipped by the generator (the recompile
 * runs with --exclude-manual src/rsc2_overrides.c), so this definition is the
 * one every direct call and the dispatch table bind to.
 *
 * Register model (g_eax, g_esp, ...: the generated code's eax/esp are
 * macros for these): a guest call pushes its arguments, then a return address,
 * then calls the C function; the callee pops both, exactly as `ret N` would.
 */

#include "recomp/gen/recomp_types.h"

/* ── zlib 1.2.1 inflate_fast (0x000A0DC0, cdecl: strm, start) ────
 *
 * The XBE links zlib's hand-written MASM decoder (contrib/masmx86/inffas32.asm)
 * rather than inffast.c. Its entry is `jmp L_do_fast` over its own string and
 * mask tables, it probes for MMX with pushfd/popfd/cpuid, and its body is one
 * long run of labels the function detector splits into pieces that jump into
 * each other. The lift of it returned "Error decompressing block!" from
 * dice::rsc2::FlatArchive::decodeIndexData on the first archive index.
 *
 * Every caller reaches it through the inflate() lifted from C (0x000A5C90),
 * whose state layout is plain zlib 1.2.1, so this is zlib 1.2.1's own
 * inffast.c over guest memory. Same contract as the original: on entry
 * avail_in >= 6 and avail_out >= 258, and it may leave state->mode at TYPE
 * (end of block) or BAD (with strm->msg set).
 */

/* z_stream, 32-bit guest layout */
#define ZS_NEXT_IN    0x00
#define ZS_AVAIL_IN   0x04
#define ZS_NEXT_OUT   0x0C
#define ZS_AVAIL_OUT  0x10
#define ZS_MSG        0x18
#define ZS_STATE      0x1C

/* struct inflate_state (zlib 1.2.1: no gzhead, no dmax) */
#define IS_MODE       0x00
#define IS_WSIZE      0x20
#define IS_WHAVE      0x24
#define IS_WRITE      0x28
#define IS_WINDOW     0x2C
#define IS_HOLD       0x30
#define IS_BITS       0x34
#define IS_LENCODE    0x44
#define IS_DISTCODE   0x48
#define IS_LENBITS    0x4C
#define IS_DISTBITS   0x50

#define ZMODE_TYPE    11
#define ZMODE_BAD     27

/* inflate.c's copies of the messages, in .rdata */
#define ZMSG_BAD_LITLEN   0x003B816Cu  /* "invalid literal/length code" */
#define ZMSG_BAD_DIST     0x003B8154u  /* "invalid distance code" */
#define ZMSG_TOO_FAR      0x003B8134u  /* "invalid distance too far back" */

/* A table entry: { uint8 op; uint8 bits; uint16 val; } */
typedef struct { uint32_t op, bits, val; } zcode;

static inline zcode zcode_at(uint32_t table, uint32_t index)
{
    uint32_t e = MEM32(table + index * 4);
    zcode c = { e & 0xFFu, (e >> 8) & 0xFFu, e >> 16 };
    return c;
}

void sub_000A0DC0(void)
{
    uint32_t strm  = MEM32(g_esp + 4);
    uint32_t start = MEM32(g_esp + 8);
    uint32_t state = MEM32(strm + ZS_STATE);

    uint32_t in    = MEM32(strm + ZS_NEXT_IN);
    uint32_t last  = in + (MEM32(strm + ZS_AVAIL_IN) - 5);
    uint32_t out   = MEM32(strm + ZS_NEXT_OUT);
    uint32_t beg   = out - (start - MEM32(strm + ZS_AVAIL_OUT));
    uint32_t end   = out + (MEM32(strm + ZS_AVAIL_OUT) - 257);

    uint32_t wsize  = MEM32(state + IS_WSIZE);
    uint32_t whave  = MEM32(state + IS_WHAVE);
    uint32_t write  = MEM32(state + IS_WRITE);
    uint32_t window = MEM32(state + IS_WINDOW);
    uint32_t hold   = MEM32(state + IS_HOLD);
    uint32_t bits   = MEM32(state + IS_BITS);
    uint32_t lcode  = MEM32(state + IS_LENCODE);
    uint32_t dcode  = MEM32(state + IS_DISTCODE);
    uint32_t lmask  = (1u << MEM32(state + IS_LENBITS)) - 1;
    uint32_t dmask  = (1u << MEM32(state + IS_DISTBITS)) - 1;

    uint32_t op, len, dist, from;
    zcode here;

#define NEXTBYTE() (MEM8(in++))
#define COPYBYTE() (MEM8(out++) = MEM8(from++))

    do {
        if (bits < 15) {
            hold += (uint32_t)NEXTBYTE() << bits; bits += 8;
            hold += (uint32_t)NEXTBYTE() << bits; bits += 8;
        }
        here = zcode_at(lcode, hold & lmask);
dolen:
        op = here.bits; hold >>= op; bits -= op;
        op = here.op;
        if (op == 0) {                          /* literal */
            MEM8(out++) = (uint8_t)here.val;
        } else if (op & 16) {                   /* length base */
            len = here.val;
            op &= 15;                           /* number of extra bits */
            if (op) {
                if (bits < op) { hold += (uint32_t)NEXTBYTE() << bits; bits += 8; }
                len += hold & ((1u << op) - 1);
                hold >>= op; bits -= op;
            }
            if (bits < 15) {
                hold += (uint32_t)NEXTBYTE() << bits; bits += 8;
                hold += (uint32_t)NEXTBYTE() << bits; bits += 8;
            }
            here = zcode_at(dcode, hold & dmask);
dodist:
            op = here.bits; hold >>= op; bits -= op;
            op = here.op;
            if (op & 16) {                      /* distance base */
                dist = here.val;
                op &= 15;
                if (bits < op) {
                    hold += (uint32_t)NEXTBYTE() << bits; bits += 8;
                    if (bits < op) { hold += (uint32_t)NEXTBYTE() << bits; bits += 8; }
                }
                dist += hold & ((1u << op) - 1);
                hold >>= op; bits -= op;
                op = out - beg;                 /* max distance in output */
                if (dist > op) {                /* copy from the window */
                    op = dist - op;
                    if (op > whave) {
                        MEM32(strm + ZS_MSG) = ZMSG_TOO_FAR;
                        MEM32(state + IS_MODE) = ZMODE_BAD;
                        break;
                    }
                    from = window;
                    if (write == 0) {           /* very common case */
                        from += wsize - op;
                        if (op < len) {
                            len -= op;
                            do { COPYBYTE(); } while (--op);
                            from = out - dist;
                        }
                    } else if (write < op) {    /* wraps around the window */
                        from += wsize + write - op;
                        op -= write;
                        if (op < len) {
                            len -= op;
                            do { COPYBYTE(); } while (--op);
                            from = window;
                            if (write < len) {
                                op = write;
                                len -= op;
                                do { COPYBYTE(); } while (--op);
                                from = out - dist;
                            }
                        }
                    } else {                    /* contiguous in the window */
                        from += write - op;
                        if (op < len) {
                            len -= op;
                            do { COPYBYTE(); } while (--op);
                            from = out - dist;
                        }
                    }
                    while (len > 2) { COPYBYTE(); COPYBYTE(); COPYBYTE(); len -= 3; }
                    if (len) { COPYBYTE(); if (len > 1) COPYBYTE(); }
                } else {                        /* copy direct from output */
                    from = out - dist;
                    do { COPYBYTE(); COPYBYTE(); COPYBYTE(); len -= 3; } while (len > 2);
                    if (len) { COPYBYTE(); if (len > 1) COPYBYTE(); }
                }
            } else if ((op & 64) == 0) {        /* second-level distance code */
                here = zcode_at(dcode, here.val + (hold & ((1u << op) - 1)));
                goto dodist;
            } else {
                MEM32(strm + ZS_MSG) = ZMSG_BAD_DIST;
                MEM32(state + IS_MODE) = ZMODE_BAD;
                break;
            }
        } else if ((op & 64) == 0) {            /* second-level length code */
            here = zcode_at(lcode, here.val + (hold & ((1u << op) - 1)));
            goto dolen;
        } else if (op & 32) {                   /* end of block */
            MEM32(state + IS_MODE) = ZMODE_TYPE;
            break;
        } else {
            MEM32(strm + ZS_MSG) = ZMSG_BAD_LITLEN;
            MEM32(state + IS_MODE) = ZMODE_BAD;
            break;
        }
    } while (in < last && out < end);

#undef NEXTBYTE
#undef COPYBYTE

    /* Return whole unused bytes; on entry bits < 8, so in never backs up
     * past where it started. */
    len = bits >> 3;
    in -= len;
    bits -= len << 3;
    hold &= (1u << bits) - 1;

    MEM32(strm + ZS_NEXT_IN)   = in;
    MEM32(strm + ZS_NEXT_OUT)  = out;
    MEM32(strm + ZS_AVAIL_IN)  = in < last ? 5 + (last - in) : 5 - (in - last);
    MEM32(strm + ZS_AVAIL_OUT) = out < end ? 257 + (end - out) : 257 - (out - end);
    MEM32(state + IS_HOLD) = hold;
    MEM32(state + IS_BITS) = bits;

    g_esp += 4; /* ret (cdecl) */
}

/* ── Networking: no network cable ─────────────────────────────
 *
 * XNetGetTitleXnAddr (0x0030FD2A, stdcall, 1 arg: XNADDR *).
 *
 * The XDK's XNet stack drives the NVNET NIC and runs DHCP itself. There is no
 * NIC here, so the real implementation answers XNET_GET_XNADDR_PENDING (0)
 * forever, and boot polls it through sub_002071F0 with KeDelayExecutionThread
 * in between -- the hang seen on the first run.
 *
 * Answer what a console with nothing plugged into the Ethernet port answers:
 * XNET_GET_XNADDR_NONE, with the address zeroed. Live and System Link then
 * report "no connection" and the offline game carries on. System Link could
 * later be bridged to host UDP here instead.
 */
#define XNET_GET_XNADDR_NONE 0x0001u
#define XNADDR_SIZE          36u

void sub_0030FD2A(void)
{
    uint32_t xnaddr = MEM32(g_esp + 4);
    uint32_t i;

    if (xnaddr)
        for (i = 0; i < XNADDR_SIZE; i++)
            MEM8(xnaddr + i) = 0;
    g_eax = XNET_GET_XNADDR_NONE;
    g_esp += 8; /* ret 4 */
}

/* ── XAPI fibers: SwitchToFiber (0x00082E25, stdcall, 1 arg) ──────
 *
 * RalliSport Challenge 2 runs work as fibers once the intro ends. XAPI's
 * SwitchToFiber pushes ebp/esi/edi/ebx and the SEH head, saves esp in the
 * current fiber, loads the target's esp, pops, and `ret 4`s into the other
 * fiber. A lifted function cannot do that: its `ret` returns to the C caller
 * that called it, which then carried on with the other fiber's stack -- esp
 * jumped into a fiber stack on the heap and the next indirect call took the
 * process down.
 *
 * So each guest fiber gets a host coroutine (ucontext) with its own native
 * stack, and a switch is a swapcontext with the guest register file saved and
 * restored around it. The guest-visible bookkeeping is kept as the original
 * keeps it: the per-thread current-fiber slot at
 * [[fs:4] + [0x00415200]*4] + 0x90, and the saved esp at fiber+0x0C.
 * CreateFiber (0x00082D99) and ConvertThreadToFiber (0x00082E64) only build
 * guest structures, so their lifted bodies stay. A fiber's first activation
 * pops the frame CreateFiber built, exactly as the original's pops and
 * `ret 4` would, and enters the start thunk (0x00082D54) from there.
 */
/* Host coroutines: ucontext on glibc, host_coro.c where there is no ucontext
 * (Android; RSC2_HOST_CORO=1 selects it on Linux too), native fibers on
 * Windows. Everything else -- the guest bookkeeping, the register file, the
 * first-activation frame -- is shared. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#if defined(__ANDROID__) && !defined(RSC2_HOST_CORO)
#define RSC2_HOST_CORO 1
#endif
#if RSC2_HOST_CORO
#include "host_coro.h"
#else
#include <ucontext.h>
#endif
#endif

#define FIBER_TLS_INDEX_VA  0x00415200u
#define FIBER_SLOT_OFF      0x90u
#define FIBER_SAVED_ESP     0x0Cu
#define FIBER_HOST_STACK    (8u << 20)    /* lifted recursion runs native */
#define FIBER_MAX           64

typedef struct {
    uint32_t   va;                     /* guest fiber object */
    int        started;
#ifdef _WIN32
    LPVOID     hfiber;                 /* host fiber */
#elif RSC2_HOST_CORO
    void      *sp;                     /* saved host stack pointer */
    void      *stack;
#else
    ucontext_t uc;
    void      *stack;
#endif
    uint32_t   eax, ecx, edx, ebx, esp, ebp, esi, edi, seh_ebp, fs0;
} HostFiber;

static HostFiber      s_fibers[FIBER_MAX];
#ifdef _WIN32
static SRWLOCK         s_fiber_lock = SRWLOCK_INIT;
#define fiber_lock()   AcquireSRWLockExclusive(&s_fiber_lock)
#define fiber_unlock() ReleaseSRWLockExclusive(&s_fiber_lock)
#else
static pthread_mutex_t s_fiber_lock = PTHREAD_MUTEX_INITIALIZER;
#define fiber_lock()   pthread_mutex_lock(&s_fiber_lock)
#define fiber_unlock() pthread_mutex_unlock(&s_fiber_lock)
#endif

extern RECOMP_TLS uint32_t g_ebp;
extern recomp_func_t recomp_lookup(uint32_t va);

static HostFiber *fiber_get(uint32_t va)
{
    int i, freei = -1;
    HostFiber *f = NULL;
    fiber_lock();
    for (i = 0; i < FIBER_MAX; i++) {
        if (s_fibers[i].va == va) { f = &s_fibers[i]; break; }
        if (!s_fibers[i].va && freei < 0) freei = i;
    }
    if (!f && freei >= 0) {
        f = &s_fibers[freei];
        memset(f, 0, sizeof *f);
        f->va = va;
    }
    fiber_unlock();
    if (!f) {
        fprintf(stderr, "[FIBER] table full (%d fibers)\n", FIBER_MAX);
        abort();
    }
    return f;
}

static uint32_t fiber_thread_data(void)
{
    uint32_t tls = MEM32(XBOX_FS_BASE + 4);
    return MEM32(tls + MEM32(FIBER_TLS_INDEX_VA) * 4u);
}

static void fiber_save(HostFiber *f)
{
    f->eax = g_eax; f->ecx = g_ecx; f->edx = g_edx; f->ebx = g_ebx;
    f->esp = g_esp; f->ebp = g_ebp; f->esi = g_esi; f->edi = g_edi;
    f->seh_ebp = g_seh_ebp; f->fs0 = MEM32(XBOX_FS_BASE);
}

static void fiber_restore(const HostFiber *f)
{
    g_eax = f->eax; g_ecx = f->ecx; g_edx = f->edx; g_ebx = f->ebx;
    g_esp = f->esp; g_ebp = f->ebp; g_esi = f->esi; g_edi = f->edi;
    g_seh_ebp = f->seh_ebp; MEM32(XBOX_FS_BASE) = f->fs0;
}

/* First activation: pop what CreateFiber put on the fiber's stack -- SEH
 * head, ebx, edi, esi, ebp -- then do the `ret 4`: continue at the return
 * address with esp past it and its 4-byte operand. The start thunk treats
 * the word now at [esp] (another 0x00082D54) as its return address. */
static void fiber_run(HostFiber *f)
{
    uint32_t s = MEM32(f->va + FIBER_SAVED_ESP), ret;
    recomp_func_t fn;

    MEM32(XBOX_FS_BASE) = MEM32(s);
    g_ebx = MEM32(s + 4);  g_edi = MEM32(s + 8);
    g_esi = MEM32(s + 12); g_ebp = MEM32(s + 16); g_seh_ebp = g_ebp;
    ret = MEM32(s + 20);
    g_esp = s + 24 + 4;
    fn = recomp_lookup(ret);
    if (!fn) {
        fprintf(stderr, "[FIBER] start address 0x%08X has no lifted code\n", ret);
        abort();
    }
    fn();
    /* XAPI fibers end by exiting the thread from inside the start thunk; a
     * start routine that returns here has nowhere to go. */
    fprintf(stderr, "[FIBER] fiber 0x%08X returned from its start routine\n", f->va);
    abort();
}

#ifdef _WIN32
static VOID CALLBACK fiber_entry(LPVOID p)
{
    fiber_run((HostFiber *)p);
}
#elif RSC2_HOST_CORO
static void fiber_entry(void *p)
{
    fiber_run((HostFiber *)p);
}
#else
static void fiber_entry(unsigned lo, unsigned hi)
{
    fiber_run((HostFiber *)(((uintptr_t)hi << 32) | lo));
}
#endif

void sub_00082E25(void)
{
    uint32_t target = MEM32(g_esp + 4);
    uint32_t tdata = fiber_thread_data();
    uint32_t cur = MEM32(tdata + FIBER_SLOT_OFF);
    HostFiber *c, *t;

    {
        static unsigned long n;
        if (getenv("RSC2_FIBER_TRACE") && (++n <= 20 || n % 1000 == 0)) {
            uint32_t i, shown = 0;
            fprintf(stderr, "[FIBER] switch #%lu 0x%08X -> 0x%08X  from", n, cur, target);
            /* Where the switching fiber is parked: code-looking values on its
             * guest stack, innermost first (a lead, as with RECOMP_WATCH). */
            for (i = 0; i < 512 && shown < 14; i++) {
                uint32_t v = MEM32(g_esp + i * 4);
                if (v >= 0x00011000u && v < 0x0037F000u) {
                    fprintf(stderr, " %08X", v);
                    shown++;
                }
            }
            fprintf(stderr, "\n");
        }
    }
    if (target == cur) {             /* switching to yourself: nothing moves */
        g_esp += 8;
        return;
    }
    c = fiber_get(cur);
    t = fiber_get(target);
    c->started = 1;                  /* it is running: it is this context */

    MEM32(cur + FIBER_SAVED_ESP) = g_esp;
    MEM32(tdata + FIBER_SLOT_OFF) = target;

    g_esp += 8;                      /* what this call's ret 4 leaves on resume */
    fiber_save(c);

#ifdef _WIN32
    /* The thread's own context is a fiber the first time it switches. */
    if (!c->hfiber)
        c->hfiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
    if (!t->started) {
        t->hfiber = CreateFiber(FIBER_HOST_STACK, fiber_entry, t);
        if (!t->hfiber) {
            fprintf(stderr, "[FIBER] CreateFiber failed (%lu)\n", GetLastError());
            abort();
        }
        t->started = 1;
    }
    SwitchToFiber(t->hfiber);
#else
    if (!t->started) {
        uintptr_t p = (uintptr_t)t;
        t->stack = mmap(NULL, FIBER_HOST_STACK, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (t->stack == MAP_FAILED) {
            fprintf(stderr, "[FIBER] cannot allocate a host stack\n");
            abort();
        }
#if RSC2_HOST_CORO
        (void)p;
        t->sp = coro_init(t->stack, FIBER_HOST_STACK, fiber_entry, t);
#else
        getcontext(&t->uc);
        t->uc.uc_stack.ss_sp = t->stack;
        t->uc.uc_stack.ss_size = FIBER_HOST_STACK;
        t->uc.uc_link = NULL;
        makecontext(&t->uc, (void (*)(void))fiber_entry, 2,
                    (unsigned)(p & 0xFFFFFFFFu), (unsigned)(p >> 32));
#endif
        t->started = 1;
        {
            static int shown;
            if (shown++ < 8)
                fprintf(stderr, "[FIBER] first switch to fiber 0x%08X (from 0x%08X)\n",
                        target, cur);
        }
    } else {
        /* Resuming a fiber that switched away through here: its registers
         * come back from its record once swapcontext lands in it. */
    }
#if RSC2_HOST_CORO
    coro_switch(&c->sp, t->sp);
#else
    swapcontext(&c->uc, &t->uc);
#endif
#endif
    /* Resumed: some fiber switched back to this one. */
    fiber_restore(c);
}

/* ── XInputGetState (0x0037E590, stdcall: hDevice, pState) ───────
 *
 * Recorded and replayed here, at the call the game makes, rather than at the
 * emulated USB pad: replay by wall time drifted with every load, and replay
 * by presented frames still lost the race -- a loading screen keeps
 * presenting while it loads, so the race began a different number of frames
 * in each run and the recorded steering arrived before it. The game asks for
 * its pad state in a fixed order from its own loop, so call N here is the
 * same moment of the game in every run.
 *
 *   RSC2_INPUT_RECORD=<file>   every call that changed the answer:
 *                              <call#> <port> <return> <22 bytes hex>
 *   RSC2_INPUT_REPLAY=<file>   answer call N with what the recording said
 *                              (live keyboard and pads are ignored)
 *
 * Ports are numbered by first appearance of each device handle. Both modes
 * turn on the frame-locked guest clock (main.c), so the game's time per
 * frame is the same as when it was recorded.
 *
 * The body below is the lifted original, kept verbatim. */
/* The generated code's register names, for the lifted body only. */
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
static void xinput_get_state_lifted(void)
{
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;

loc_0037E590: ;
    PUSH32(esp, ebx);
    PUSH32(esp, esi);
    ebx = 0; /* xor self */
    _fa = (uint32_t)(ebx) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* xor result */
    { uint32_t _icall_esp = g_esp;
    { uint32_t _icall_target = MEM32(0x37FA3C); PUSH32(esp, 0x0037E59Au); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x0037E594u); } /* indirect call */
    }

loc_0037E59A: ;
    edx = MEM32(esp + 0xC);
    ecx = MEM32(edx + 0xA3);
    _fa = (uint32_t)(MEM8(ecx + 0x28)) & 0xFFu; _fb = (uint32_t)(0x10) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test MEM8(ecx + 0x28), 0x10 (8-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_0037E5AF; /* je: equal / zero */

loc_0037E5AA: ;
    PUSH32(esp, 0x57);
    POP32(esp, esi);
    goto loc_0037E5F4;

loc_0037E5AF: ;
    ecx = MEM32(edx);
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp ecx, 0 (32-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_0037E5BB; /* je: equal / zero */

loc_0037E5B5: ;
    _fa = (uint32_t)(MEM8(ecx + 4)) & 0xFFu; _fb = (uint32_t)(2) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test MEM8(ecx + 4), 2 (8-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_0037E5C0; /* je: equal / zero */

loc_0037E5BB: ;
    ebx = 0x48F;

loc_0037E5C0: ;
    ecx = MEM32(edx + 8);
    PUSH32(esp, edi);
    edi = MEM32(esp + 0x14);
    MEM32(edi) = ecx;
    MEM8(edx + 0xA2) = MEM8(edx + 0xA2) & 0xEF;
    _fa = (uint32_t)(MEM8(edx + 0xA2)) & 0xFFu; _fas = (int32_t)(int8_t)(_fa); /* and result */
    ecx = MEM32(edx + 0xA3);
    ecx = MEM32(ecx + 8);
    ecx = ZX8(MEM8(ecx));
    esi = edx + 0x14;
    edx = ecx;
    _fb = (uint32_t)(4) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */
    edi = edi + 4;
    _fa = (uint32_t)(edi) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* add result */
    ecx = ecx >> 2;
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* shift result */
    { uint64_t _n = (uint64_t)ecx * 4u;
    if (!g_df && esi < 0xFD000000u && edi < 0xFD000000u && (uint64_t)esi + _n <= 0xFD000000u && (uint64_t)edi + _n <= 0xFD000000u) {
      uint8_t *_d = (uint8_t*)XBOX_PTR(edi), *_s = (uint8_t*)XBOX_PTR(esi);
      if ((uint64_t)edi + _n <= esi || (uint64_t)esi + _n <= edi) memcpy(_d, _s, (size_t)_n);
      else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*4u) = MEM32(esi + _i*4u); }
      esi += ecx * 4u; edi += ecx * 4u; }
    else { uint32_t _i; int32_t _st = RECOMP_DF_STEP(4);
      for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*_st) = MEM32(esi + _i*_st);
      esi += ecx * _st; edi += ecx * _st; }
    ecx = 0; } /* rep movsd */
    ecx = edx;
    ecx = ecx & 3;
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* and result */
    { uint64_t _n = (uint64_t)ecx * 1u;
    if (!g_df && esi < 0xFD000000u && edi < 0xFD000000u && (uint64_t)esi + _n <= 0xFD000000u && (uint64_t)edi + _n <= 0xFD000000u) {
      uint8_t *_d = (uint8_t*)XBOX_PTR(edi), *_s = (uint8_t*)XBOX_PTR(esi);
      if ((uint64_t)edi + _n <= esi || (uint64_t)esi + _n <= edi) memcpy(_d, _s, (size_t)_n);
      else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM8(edi + _i*1u) = MEM8(esi + _i*1u); }
      esi += ecx * 1u; edi += ecx * 1u; }
    else { uint32_t _i; int32_t _st = RECOMP_DF_STEP(1);
      for (_i = 0; _i < ecx; _i++) MEM8(edi + _i*_st) = MEM8(esi + _i*_st);
      esi += ecx * _st; edi += ecx * _st; }
    ecx = 0; } /* rep movsb */
    esi = ebx;
    POP32(esp, edi);

loc_0037E5F4: ;
    SET_LO8(ecx, LO8(eax));
    { uint32_t _icall_esp = g_esp;
    { uint32_t _icall_target = MEM32(0x37FA38); PUSH32(esp, 0x0037E5FCu); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x0037E5F6u); } /* indirect call */
    }

loc_0037E5FC: ;
    eax = esi;
    POP32(esp, esi);
    POP32(esp, ebx);
    esp += 12; return; /* ret 8 */

}
#undef eax
#undef ecx
#undef edx
#undef esp
#undef ebx
#undef esi
#undef edi

#define XI_STATE_BYTES 0x16      /* dwPacketNumber + XINPUT_GAMEPAD */
#define XI_PORTS 4

typedef struct { uint32_t call, port, ret; uint8_t s[XI_STATE_BYTES]; } XiRec;

static int xi_port(uint32_t handle)
{
    static uint32_t handles[XI_PORTS];
    int i;
    for (i = 0; i < XI_PORTS; i++) {
        if (handles[i] == handle)
            return i;
        if (!handles[i]) {
            handles[i] = handle;
            return i;
        }
    }
    return XI_PORTS - 1;
}

void sub_0037E590(void)
{
    static int mode = -1;               /* 0 off, 1 record, 2 replay */
    static FILE *rec;
    static XiRec *rp;
    static int nrp;
    static uint32_t calls[XI_PORTS];
    static int cur[XI_PORTS];
    static uint8_t last[XI_PORTS][XI_STATE_BYTES];
    static uint32_t last_ret[XI_PORTS];
    static int have[XI_PORTS];
    uint32_t handle = MEM32(g_esp + 4), pstate = MEM32(g_esp + 8);
    int port, i;

    if (mode < 0) {
        const char *r = getenv("RSC2_INPUT_RECORD"), *p = getenv("RSC2_INPUT_REPLAY");
        mode = 0;
        if (p && *p) {
            FILE *f = fopen(p, "r");
            char line[160];
            int cap = 0;
            while (f && fgets(line, sizeof line, f)) {
                XiRec x;
                char hex[64];
                unsigned k;
                if (sscanf(line, "%u %u %x %63s", &x.call, &x.port, &x.ret, hex) != 4
                    || strlen(hex) != XI_STATE_BYTES * 2 || x.port >= XI_PORTS)
                    continue;
                for (k = 0; k < XI_STATE_BYTES; k++) {
                    unsigned v;
                    sscanf(hex + 2 * k, "%2x", &v);
                    x.s[k] = (uint8_t)v;
                }
                if (nrp == cap) {
                    XiRec *g = (XiRec *)realloc(rp, sizeof *rp * (size_t)(cap ? cap * 2 : 1024));
                    if (!g)
                        break;
                    rp = g;
                    cap = cap ? cap * 2 : 1024;
                }
                rp[nrp++] = x;
            }
            if (f)
                fclose(f);
            for (i = 0; i < XI_PORTS; i++)
                cur[i] = -1;
            mode = 2;
            fprintf(stderr, "[INPUT] replaying %d XInputGetState change(s) from %s\n", nrp, p);
        } else if (r && *r) {
            rec = fopen(r, "w");
            if (rec) {
                mode = 1;
                fprintf(stderr, "[INPUT] recording XInputGetState to %s\n", r);
            }
        }
    }

    xinput_get_state_lifted();          /* pops the arguments, sets eax */
    if (!mode || !pstate)
        return;
    port = xi_port(handle);
    calls[port]++;
    /* The game asks for pad 0 once per frame of its main loop, loading
     * screens included: that is its frame, so it ticks the game's clock --
     * not the GPU thread, which hitches differently in every run. */
    if (port == 0) {
        extern void xbox_GuestClockGameFrame(void);
        xbox_GuestClockGameFrame();
    }
    {
        /* RSC2_INPUT_TRACE: calls per port against frames presented, every
         * 300 frames -- where a recording and its replay part ways. */
        extern uint32_t nv2a_pb_presented_frames(void);
        static int trace = -1;
        static uint32_t next = 300;
        uint32_t fr = nv2a_pb_presented_frames();
        if (trace < 0)
            trace = getenv("RSC2_INPUT_TRACE") != NULL;
        if (trace && fr >= next) {
            fprintf(stderr, "[INPUT] frame %u: calls %u %u %u %u (handle %08X port %d)\n",
                    fr, calls[0], calls[1], calls[2], calls[3], handle, port);
            next = fr + 300;
        }
    }

    if (mode == 1) {
        uint8_t s[XI_STATE_BYTES];
        for (i = 0; i < XI_STATE_BYTES; i++)
            s[i] = MEM8(pstate + (uint32_t)i);
        /* Packet numbers differ run to run; compare the rest. */
        if (!have[port] || last_ret[port] != g_eax
            || memcmp(last[port] + 4, s + 4, XI_STATE_BYTES - 4)) {
            fprintf(rec, "%u %d %X ", calls[port], port, g_eax);
            for (i = 0; i < XI_STATE_BYTES; i++)
                fprintf(rec, "%02X", s[i]);
            fputc('\n', rec);
            fflush(rec);
            memcpy(last[port], s, XI_STATE_BYTES);
            last_ret[port] = g_eax;
            have[port] = 1;
        }
        return;
    }

    /* Replay: the latest recorded answer at or before this call. */
    for (i = cur[port] + 1; i < nrp && rp[i].call <= calls[port]; i++)
        if (rp[i].port == (uint32_t)port)
            cur[port] = i;
    if (cur[port] >= 0) {
        static uint32_t packet[XI_PORTS];
        const XiRec *x = &rp[cur[port]];
        /* A fresh packet number whenever the answer changes, as the device
         * would give; the rest exactly as recorded. */
        if (memcmp(last[port] + 4, x->s + 4, XI_STATE_BYTES - 4) || !have[port]) {
            packet[port]++;
            memcpy(last[port], x->s, XI_STATE_BYTES);
            have[port] = 1;
        }
        MEM32(pstate) = packet[port];
        for (i = 4; i < XI_STATE_BYTES; i++)
            MEM8(pstate + (uint32_t)i) = x->s[i];
        g_eax = x->ret;
    } else {
        for (i = 4; i < XI_STATE_BYTES; i++)
            MEM8(pstate + (uint32_t)i) = 0;
    }
}

/* ── DirectSound SetI3DL2Listener (0x00325ED3, stdcall: pDS, pds3dl, dwApply) ─
 *
 * The title moves its reverb with the car -- in-car, tunnels, forest,
 * valleys, building walls, an arena -- by handing DirectSound an I3DL2
 * listener. On the console that becomes coefficients for the reverb in the
 * GP's DSP program, which does not run here; the APU's own reverb takes the
 * parameters instead (mcpx_apu_set_i3dl2). Then the original body: the
 * interface wrapper's `this` adjustment and CDirectSound::SetI3DL2Listener
 * (0x00325C90), which keeps the title's view of the listener exact.
 */
extern void mcpx_apu_set_i3dl2(const uint32_t p[12]);
extern void sub_00325C90(void);

#define esp g_esp
void sub_00325ED3(void)
{
    uint32_t pds = MEM32(esp + 4), p = MEM32(esp + 8), apply = MEM32(esp + 12), i;

    if (p) {
        uint32_t v[12];
        for (i = 0; i < 12; i++)
            v[i] = MEM32(p + i * 4);
        mcpx_apu_set_i3dl2(v);
    }
    PUSH32(esp, apply);
    PUSH32(esp, p);
    PUSH32(esp, pds ? pds - 8 : 0);
    PUSH32(esp, 0x00325EF0u); RECOMP_ABI_CALL(0x00325C90u, sub_00325C90);
    esp += 16; return; /* ret 12 */
}
#undef esp

/* ── D3D gamma ramp upload (0x0036C636: eax = &register base, edx = ramp) ─
 *
 * D3D keeps the title's gamma ramp (256 red, then 256 green, then 256 blue
 * bytes) and loads it into the video DAC's palette at vblank; on the
 * console every pixel goes through it on its way to the TV. Nothing scans
 * out through a DAC here, so the ramp also goes to the renderer, which
 * applies it when it presents (nv2a_vk_set_gamma). Then the DAC writes, as
 * the original makes them.
 */
extern void nv2a_vk_set_gamma(const uint8_t *r, const uint8_t *g, const uint8_t *b);

#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
void sub_0036C636(void)
{
    uint8_t ramp[3][256];
    uint32_t regs = MEM32(eax), base = edx, i, c;

    for (c = 0; c < 3; c++)
        for (i = 0; i < 256; i++)
            ramp[c][i] = MEM8(edx + c * 0x100 + i);
    nv2a_vk_set_gamma(ramp[0], ramp[1], ramp[2]);
    MEM8(regs + 0x6813C8) = 0;
    for (i = 0; i < 256; i++) {
        if (!(i & 7))
            (void)MEM32(regs + 0x3240);     /* the original's read-back */
        MEM8(regs + 0x6813C9) = ramp[0][i];
        MEM8(regs + 0x6813C9) = ramp[1][i];
        MEM8(regs + 0x6813C9) = ramp[2][i];
    }
    /* Registers as the original's loop leaves them. */
    ecx = regs;
    eax = base + 0x200;
    edx = 0x100;
    esp += 4; /* ret */
}
#undef eax
#undef ecx
#undef edx
#undef esp

/* ── D3D SetGammaRamp (0x00365590, stdcall: flags, pRamp) ──────────────
 *
 * Where the title's ramps arrive. RalliSport Challenge 2 blends between two
 * per-scene ramps and sets the result; without D3DSGR_IMMEDIATE (2) D3D only
 * marks it for the vblank handler, which never loads it here, so the
 * renderer takes it now (nv2a_vk_set_gamma). The rest is the original:
 * nothing if it equals the ramp being shown, else into the device's next
 * slot, then loaded at once (sub_0036C636) or flagged for vblank.
 */
extern void sub_0036C636(void);

#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
void sub_00365590(void)
{
    uint32_t flags = MEM32(esp + 4), ramp = MEM32(esp + 8);
    uint32_t dev = MEM32(0x00375418u), blk = dev + 0x1C28u;
    uint32_t slot = MEM32(dev + 0x2478u) & 1, shown = MEM32(dev + 0x240Cu);
    uint32_t cur = blk + 0x1DCu + shown * 0x300u, dst = blk + 0x1DCu + slot * 0x300u, i;
    uint8_t r[256], g[256], b[256];

    for (i = 0; i < 256; i++) {
        r[i] = MEM8(ramp + i);
        g[i] = MEM8(ramp + 0x100 + i);
        b[i] = MEM8(ramp + 0x200 + i);
    }
    nv2a_vk_set_gamma(r, g, b);
    for (i = 0; i < 0xC0; i++)
        if (MEM32(cur + i * 4) != MEM32(ramp + i * 4))
            break;
    if (i < 0xC0) {
        for (i = 0; i < 0xC0; i++)
            MEM32(dst + i * 4) = MEM32(ramp + i * 4);
        MEM32(blk + 0x7E4u) = slot;
        if (flags & 2) {
            eax = blk;
            edx = dst;
            PUSH32(esp, 0x003655F7u); RECOMP_ABI_CALL(0x0036C636u, sub_0036C636);
        } else {
            MEM32(blk + 0x7DCu + slot * 4) = 1;
        }
    }
    eax = blk;
    esp += 12; /* ret 8 */
}
#undef eax
#undef ecx
#undef edx
#undef esp
