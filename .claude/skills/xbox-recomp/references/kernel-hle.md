# Kernel / HLE lessons

## Memory map

- Reserve the **full 4 GB guest span** at boot and keep it reserved
  (PROT_NONE) for the life of the process. Carve each window (RAM mirrors,
  contiguous, NV2A, MCPX, flash, tiled aperture, kernel page) out of it just
  before mapping it. Releasing the tail let host heaps, drivers and LLVM land
  inside guest space, so stray guest pointers corrupted the host silently.
- Map guest memory **before** loading SDL or the Vulkan driver. The launcher
  loading them first let the OS place libraries where the contiguous window
  had to go ("contiguous window failed error 8").
- **Allocate contiguous memory top-down like the console**
  (`RECOMP_CONTIG_TOPDOWN`), and honour the low/high limits of
  MmAllocateContiguousMemoryEx. RSC2's boot heap records its size wrongly
  (as KB), so its free claimed every pointer within 128 MB above its base;
  bottom-up allocation put race buffers in that range.
- `MmGetPhysicalAddress` tagging (`RECOMP_PHYS_TAG`): heap memory isn't in the
  contiguous window, so devices (APU, OHCI, NV2A) must resolve tagged
  physical addresses back to the heap. Music and movie audio were silent
  without it.

## Threads, DPCs, interrupts

- `ObReferenceObjectByHandle` must return real **guest thread objects**
  (SignalState at +4, ExitStatus at +0x120). XAPI's GetExitCodeThread reads
  them, and the stage loader polled a thread that never looked finished.
- **Dispatch lock:** on the console, code at DISPATCH_LEVEL can't be
  interrupted by a DPC. Taking a lock on raising to DISPATCH (guest threads
  and DPC context alike, ISRs excluded, 50 ms timeout with a log) fixed rare
  `0xCCCCCCCC` pad-handle crashes and garbage USB lengths.
- **DISPATCH_LEVEL doesn't mask device interrupts.** It holds off DPCs and
  thread switches; an ISR comes in anyway, and code sharing data with an
  ISR raises to the ISR's IRQL (KeSynchronizeExecution). A device model
  that holds its interrupt while *any* thread is at DISPATCH delays it for
  as long as the title works there: RSC2's APU interrupt waited up to 33 ms
  in races. Count threads above DISPATCH separately and gate device
  interrupts on that (RSC2 `xbox_IrqlBlocksDeviceInterrupts`, cd146c9).
- **Waits must sleep.** Spins that yield and read the clock cost little on
  x86 Linux, but on some Android kernels every clock read is a syscall
  (vDSO fallback). An idle executor loop, a lock wait and a frame pacer
  ate most of two big cores. Use events, futexes and absolute-deadline
  sleeps (376e72b).
- `RECOMP_GUEST_LOCK=1` runs one guest thread at a time (released during
  kernel calls), like the single-CPU Xbox. It doesn't cover DPCs. On RSC2 it
  costs nothing measurable and is the default on ARM.
- D3D vblank and flip pacing need the PTIMER/PCRTC/PMC interrupt model.
- XAPI fibers (SwitchToFiber) were HLE'd with host coroutines (ucontext on
  POSIX, Win32 fibers on Windows). Android's libc has no ucontext, so it
  needs its own context switch.

## Video and system flags

- `ExQueryNonVolatileSetting` XC_VIDEO must use EEPROM bit positions (see
  renderer.md). The old value 0x03 read as SDTV 4:3.
- The AV query the headers call "option 6" is really AV_QUERY_AV_CAPABILITIES
  and must OR in the XC_VIDEO flags, or D3D CreateDevice fails and the game
  hangs.
- `XNetGetTitleXnAddr` must answer "no cable" cleanly.

## Saves and paths

- Partition paths map to the game folder or a save folder (`kernel_path.c`).
  In RSC2, `UDATA`/`TDATA` live in the game folder and the Z: cache in
  `~/.local/share/xboxrecomp`. Know where saves land before running tests.
- A profile saved by the Linux build showed as "Unable to load" on Windows.
  Unresolved; suspect a file-attribute or metadata difference.

## D3D's kick and GPU overlap (PFB flush, PATT_COLOR0, NOP notify)

XDK D3D's push-buffer kick sets the PFB write-back flush (`0xFD100410` bit
`0x10000`) and spins until it clears. If the software GPU only clears it once
the executor has caught up with PUT, the game and the executor take turns:
frame time is the sum of both. On the console the flush is a CPU
write-combining flush, done at once. Clearing it immediately is right, but a
D3D that is then ahead of the GPU waits for push-buffer room through two
things a plain register aperture does not provide (RSC2 hung on race load):

- **PGRAPH `PATT_COLOR0` (`0x400B10`) is D3D's GPU progress counter.** It
  queues the pattern object's `SET_MONOCHROME_COLOR0` (method `0x0310` on a
  non-3D subchannel) and polls the register (bits 2-6 against its counter).
  The executor must write it when it executes that method.
- **`NV097_NO_OPERATION` with a non-zero parameter is a notify interrupt.**
  The GPU sets `PGRAPH_INTR` ERROR (bit 20), `NSOURCE` NOTIFICATION (bit 0),
  `TRAPPED_ADDR` (method in bits 0-12, subchannel in 16-18) and
  `TRAPPED_DATA_LOW` (the parameter), raises PMC bit 12, and stalls until
  the ISR writes the ERROR bit back (xemu `pgraph.c`). D3D patches one into
  space the GPU has not reached and waits on a device event its ISR sets.
- **The GPU stays stalled until `PGRAPH_FIFO` (`0x400720`) is back on, not
  only until the ERROR bit is cleared.** D3D's handler turns FIFO access
  off, clears the interrupt, and only then reads `TRAPPED_DATA_LOW`. An
  executor that goes on at the clear can overwrite the parameter with the
  next notify. RSC2 lost its "push-buffer room" notify (5) that way and
  froze in a wait with no timeout, but only once notifies were delivered
  promptly. Count "parameter changed between clear and read" in the model,
  and add a knob that sleeps after the clear to widen the window. That
  proved the race in one run.

With both modelled (toolkit `nv2a_irq.c`, `RECOMP_NV2A_PFB_ASYNC`), RSC2's main
thread went from ~15M flush polls a second to ~2000, and its Switch race
from 17-20 to 20-33 fps. GET, idle bits and semaphores must still advance
only as the executor really completes work: D3D's buffer reuse waits on
those. `RECOMP_MMIO_POLL_TRACE=1` names the register a spinning thread reads.

## Threads created suspended

Honour `PsCreateSystemThreadEx`'s `CreateSuspended`. The XDK CRT's thread
wrapper passes `(flags >> 2) & 1`, and titles create workers suspended,
then set up what the worker waits on, then call `NtResumeThread`. A
worker started at once races its creator. It usually loses by a hair,
so the bug hides until the timing shifts. On RSC2 a profiler thread was
enough to tip it: the worker waited on heap garbage (handle `0x9`) and
crashed at the first movie.
