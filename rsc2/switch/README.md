# Nintendo Switch build (homebrew, work in progress)

`rsc2.nro`, a homebrew program for a Switch running custom firmware
(Atmosphère). It is built with devkitPro's devkitA64 and libnx. The game
code is the same as on the other platforms; what differs is the host layer
under it, in `xboxrecomp/src/platform/switch/`.

**Status:**
- It builds.
- In the Ryujinx emulator it boots through memory setup, the kernel, the save data and the disc archives, and copies the Z: cache.
- It stops where the PC build stops when the fault-based devices are off. See "Emulator" below.
- It has not run on a real Switch yet.

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
address space has to be one host range.

- **Where it goes:** the only place an unprivileged Horizon process can map
  memory at an address of its choosing is its stack region (`svcMapMemory`
  moves heap pages there). That region is under 2 GB.
- **Folding:** the Switch build moves the guest's top half down
  (`XBOX_FOLD_HIGH`, `platform/guest_addr.h`), so the whole guest space
  needs only 0x44000000 bytes. In the folded layout:
  - the contiguous window at 0x80000000 sits at 0x40000000;
  - the device apertures sit at 0x3D000000–0x3FFFFFFF.
- **Testing the folding on a PC:** `cmake -DRSC2_FOLD_HIGH=ON` turns it on
  in any build. A Linux replay with it matches the normal build within
  run-to-run noise.
- **Mappings:**
  - A large anonymous `mmap` (the reservation) takes a slot of the stack
    region and maps nothing.
  - RAM, the contiguous window and the device apertures are heap memory
    moved there.
  - Small anonymous mappings come from the heap.
- **No page aliasing:** the Xbox RAM mirrors and the 0xF0000000 aperture are
  not mapped. RSC2 uses neither: a Linux replay with `RECOMP_MIRRORS=0
  RECOMP_TILED=0` matches the baseline within run-to-run noise.
- **Checking a host:** `RSC2_NX_PROBE=1` in `rsc2.env` logs the address
  regions and which mapping syscalls the process may use.
  `RSC2_NX_PROBE=2` also faults on purpose to check that a fault can be
  resumed.

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

## Emulator (Ryujinx)

To run it on a PC:
- Ryubing's Ryujinx from Flathub (`io.github.ryubing.Ryujinx`) runs the `.nro`.
- It needs `prod.keys` from your own console in
  `~/.var/app/io.github.ryubing.Ryujinx/config/Ryujinx/system/`.
- The emulated SD card is `.../config/Ryujinx/sdcard/`.

What it can test:
- **No resumed faults:** Ryujinx does not hand faults to the program's
  exception handler. A fault on an unmapped address ends the guest.
- **So the devices must be off:** audio, USB and the GPU timers all work by
  faulting, so they have to be switched off in `rsc2.env`:
  `RECOMP_AC97_READY=0`, `RECOMP_USB=0`, `RECOMP_NV2A_IRQ=0`.
- **How far it gets that way:** the game then boots as far as the PC build
  does with the same switches. It crashes in DirectSound after the Z: cache
  copy, because DirectSound was never created.

Getting further in the emulator needs those devices without faults.

What Ryujinx's probe showed:
- system resource budget 0, so `svcMapPhysicalMemory` fails;
- `svcMapMemory` works in the stack region;
- `svcSetMemoryPermission` is refused there (`InvalidCurrentMemory`).

## To check first on hardware

1. **Fault resume:** does the fault path resume correctly? Use
   `RSC2_NX_PROBE=2`.
2. **Trap pages:** the device models arm their traps with
   `svcSetMemoryPermission`, which Ryujinx refuses on stack-region memory.
   If hardware refuses it too, the trap pages have to be left unmapped
   instead.
3. **Speed:** how fast the software renderer is on the Switch's CPU.
