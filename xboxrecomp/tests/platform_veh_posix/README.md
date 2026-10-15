# Vectored exception handlers and handles on a POSIX host

The two pieces of the Win32 shim (`src/platform/win32_compat.c`) that a
recompiled title's fault and wait paths sit on.

```
cmake -S tests/platform_veh_posix -B build/platform-veh
cmake --build build/platform-veh
ctest --test-dir build/platform-veh --output-on-failure
```

## What it guarantees

**A fault reaches the handler with the Win32 view of it.** An access violation
arrives as `EXCEPTION_ACCESS_VIOLATION` with the faulting address in
`ExceptionInformation[1]` and the write flag in `[0]`, and the `CONTEXT` holds
the faulting `Rip`. This is what the NV2A and APU MMIO decoders consume.

**`EXCEPTION_CONTINUE_EXECUTION` resumes from the `CONTEXT`.** A handler that
advances `Rip` past the faulting instruction and writes a destination register
sees execution continue there with that register value -- the whole contract
an MMIO read emulation depends on.

**Handlers run in registration order, `First` at the front, and a declining
handler passes the fault on.**

**A value that is not a handle fails like it does on Windows.** Waiting on,
setting or closing an address the shim never handed out returns failure with
`ERROR_INVALID_HANDLE` instead of dereferencing it. Before this, a guest
`KEVENT` address with no host shadow went to `pthread_mutex_lock` and killed the
process.
