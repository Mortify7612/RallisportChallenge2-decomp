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
