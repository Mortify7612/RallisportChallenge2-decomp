# Nintendo Switch build (homebrew, work in progress)

`rsc2.nro`, a homebrew program for a Switch running custom firmware
(Atmosphère). It is built with devkitPro's devkitA64 and libnx. The game
code is the same as on the other platforms; what differs is the host layer
under it, in `xboxrecomp/src/platform/switch/`.

**Status:** it builds. It has not run on a Switch or in an emulator yet.

## Building (no root needed)

```sh
rsc2/switch/build_nro.sh        # -> rsc2/build-switch/rsc2.nro (+ rsc2.elf)
```

Without a devkitPro install (`$DEVKITPRO` unset), the script runs itself in
devkitPro's container image with podman or docker. Pull the image once with
`podman pull docker.io/devkitpro/devkita64`. The generated code in
`rsc2/src/recomp/` must exist, as for any build (`rsc2/regen.sh`).

`rsc2.elf` keeps the symbols. Crash addresses are printed relative to the
module (`rsc2.elf+0x...`), so `aarch64-none-elf-addr2line -fe rsc2.elf 0x...`
names them.

## On the Switch

On the SD card, next to the program:

```
sdmc:/switch/rsc2/rsc2.nro
sdmc:/switch/rsc2/game/        the game files (default.xbe, Media, ...)
sdmc:/switch/rsc2/rsc2.env     optional settings, NAME=value per line
```

- **Memory:** start it through a game (hold R while starting a game) so it
  gets full memory. Album/applet mode leaves only a few hundred MB.
- **Logs:**
  - Normally they go to `rsc2.log` in the same folder.
  - Started from a PC with `nxlink -s rsc2.nro`, stdout and stderr stream
    to the PC's terminal instead.
- **Settings:** `rsc2.env` takes the same `RECOMP_*` and `RSC2_*` switches
  as the environment on other platforms.
- **Renderer:** the default is the software rasteriser (`RECOMP_VULKAN=0`).
  Homebrew has no Vulkan, so a GPU renderer (OpenGL through devkitPro's
  Mesa, or deko3d) is still to come.

## How the host layer works (`platform/switch/nx_host.c`)

### Guest memory

Every guest address is a host address at a fixed offset, so the guest's
4 GB is one host range.

- **Reservation:** a large anonymous `mmap` (the 4 GB reservation) takes a
  slot of the process's alias region and maps nothing.
- **Windows:** RAM, the contiguous window and the device apertures are then
  mapped at their guest addresses inside it with `svcMapPhysicalMemory`.
  Its memory is what the heap leaves free, so `__libnx_initheap` shrinks
  the heap by 224 MB.
- **Small mappings** come from the heap.
- **No page aliasing:**
  - The Xbox RAM mirrors and the 0xF0000000 aperture are not mapped.
  - RSC2 uses neither. A Linux replay with `RECOMP_MIRRORS=0
    RECOMP_TILED=0` matches the baseline within run-to-run noise.

### Device faults

The APU, AC'97, USB (OHCI) and NV2A timer models trap the game's register
accesses as faults, as on Linux.

- **Entry:** `__libnx_exception_entry` replaces libnx's crash-only one.
- **Handling:** it saves the live registers and builds the Win32-style
  CONTEXT that the existing AArch64 decoder (`mmio_decode.h`) expects.
- **Resume:** it runs the vectored handlers, writes the result back, and
  resumes with `svcReturnFromException`.
- **Unhandled faults** print the registers and return to the kernel. The
  process then ends, and Atmosphère writes a crash report under
  `sdmc:/atmosphere/crash_reports/`.

### Other gaps

- **Missing headers:** newlib lacks `sys/mman.h`, `dlfcn.h`, `execinfo.h`,
  `sys/sysinfo.h` and `sys/syscall.h`; `platform/switch/include/` supplies
  them.
- **Fibers:** they use `host_coro.c`, as on Android.
- **Diagnostics:** the Linux signal-based ones (`kill -QUIT` thread dumps,
  the sampling profiler) are left out.

## To check first on hardware or in an emulator

1. Does `svcMapPhysicalMemory` work for this process? It needs a system
   resource size. The log prints the alias region and the memory left for
   the guest, and any failing call with its result code.
2. Does the fault path resume correctly? The game reaching its title screen
   with sound means the APU traps work.
3. Speed of the software renderer on the Switch's CPU.
