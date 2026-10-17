# Toolkit and lifter fixes (xboxrecomp v0.13.1 baseline)

All of these are in the vendored `xboxrecomp/` of the RSC2 repo. Each one
had a visible symptom, listed here so a new game's symptom can be matched to
a known cause.

## Lifter / translator (x86 -> C)

| Bug | Symptom |
|---|---|
| `fcmovcc` emitted as a comment (no-op) | Bounding boxes built with `fcomi`/`fcmovnbe`/`fcmovb` stayed inverted, so the car body and near-camera objects were culled |
| `movsx` from `bp`/`sp` wrong | Bink video IDCT corrupted |
| `lahf` carry, `pushfd`/`popfd`, `cpuid` | Feature probes and flag-based branches went wrong |
| `ficom`, `ffreep` missing | x87 compare paths broke |
| `cvt[t]ss2si`/`cvt[t]sd2si` rounding | Now go through `MMX_CVT_F2I`/`SSE_CVT_D2I`, which honour MXCSR, truncate when they should, and return 0x80000000 on overflow like x86 |
| Inline jump-table arms, `ARRAY_ELEMENT32`, a 4096-index cap, inline array sizes | Switch statements and tables truncated |
| Data islands not scanned for pointers | Functions reachable only through tables were never lifted |
| `fnstcw` reserved bit 6 should read 1 | Minor, unfixed |

Generated code must be compiled with `-fno-strict-aliasing -fwrapv -w`.
Without a declaration of `sqrtf`/`rsqrt` and similar in scope, C89 implicit
`int` returns corrupted matrices. `recomp_types.h` includes `<math.h>` for
this reason.

## Platform layer (Linux)

- Vectored exception handling over `sigaction`, including SIGTRAP: the AC'97
  and write-watch traps single-step a write with the x86 trap flag. On
  AArch64 there is no trap flag, so the handler emulates the store instead
  (see platforms.md).
- File mappings and views, `VirtualAlloc`/`VirtualFree` that really release
  memory, case-insensitive paths, rename and allocation info classes. Windows
  rename uses `FileRenameInfo` plus DELETE access, with `FILE_SHARE_DELETE`
  always set.
- The MMIO decoder: device registers are trapped pages, and the fault handler
  decodes the faulting host instruction (`platform/mmio_decode.h`, for x86-64
  and AArch64). The x86 decoder had to learn imm forms, ALU-on-memory forms,
  `test` and `r,rm` forms, because GCC folds `MEM32(x) |= bit` into those.

## Kernel bridge

See kernel-hle.md for the reasoning. In short: guest thread objects for
`ObReferenceObjectByHandle`, a handle registry, top-down contiguous memory,
physical-address tagging, the dispatch lock, `XNetGetTitleXnAddr` without a
cable, and kernel call tracing (`KERNEL_LOG`).

## Executor and devices

- NV2A semaphore release in the pushbuffer executor; PMC/PTIMER/PCRTC
  interrupt model (`nv2a_irq.c`), which D3D's vblank and flip pacing need.
- OHCI USB with an emulated pad on Linux, `RECOMP_USB_PADS=n` for more pads.
- PFB flush acks and PFIFO idle bits only after the executor has caught up.
- `nv2a_pb_scan.c` "note" survey gated behind `RECOMP_PB_SCAN`; it was 25% of
  executor time.
