# Nintendo Switch build (homebrew, work in progress)

`rsc2.nro`, a homebrew program for a Switch running custom firmware
(Atmosphère). It is built with devkitPro's devkitA64 and libnx. The game
code is the same as on the other platforms; what differs is the host layer
under it, in `xboxrecomp/src/platform/switch/`.

**Status:**
- It runs on a real Switch (Atmosphère, title-override mode) with the
  software renderer: it boots, copies the Z: cache and reaches the menus,
  at 1–3 fps.
- The OpenGL renderer (`kernel/nv2a_gl.c`) works on Linux and has not been
  tried on hardware yet.
- In the Ryujinx emulator it boots and plays the intro movies with the
  software renderer. See "Emulator" below.

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

- **Memory:** start hbmenu through a game (hold R while starting any game
  from the HOME menu) so it gets about 3.2 GB. From the Album it runs in
  applet mode, about 380 MB, and the program stops with a message saying
  so (`RSC2_NX_LOW_MEMORY=1` tries anyway).
- **From a PC:** `rsc2/switch/send.sh <switch-ip> [NAME=value ...]` sends
  the build with nxlink (press Y in hbmenu first, or after: it retries for
  `WAIT` seconds) and streams stdout and stderr back into
  `work/nxlink.log`. The arguments override `rsc2.env` for that run.
  Don't test the netloader's port by hand: hbmenu leaves netloader mode
  after any connection.
- **Logs:** without nxlink they go to `rsc2.log` in the same folder,
  buffered (unbuffered SD writes cost about 60 ms a line).
- **Settings:** `rsc2.env` takes the same `RECOMP_*` and `RSC2_*` switches
  as the environment on other platforms.
- **Renderer:** `RECOMP_VULKAN` (on by default) means the GPU renderer,
  here OpenGL 4.3 through devkitPro's Mesa. `RECOMP_VULKAN=0` is the
  software rasteriser.

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
- **No page aliasing:** Horizon cannot map one memory twice, so the Xbox
  RAM mirrors are not mapped (RSC2 does not use them: a Linux replay with
  `RECOMP_MIRRORS=0` matches the baseline). The 0xF0000000 tiled aperture,
  an alias of the contiguous window that the game's error screen writes
  through, is folded onto the window itself instead.
- **Checking a host:** `RSC2_NX_PROBE=1` in `rsc2.env` logs the address
  regions and which mapping syscalls the process may use.
  `RSC2_NX_PROBE=2` also faults on purpose to check that a fault can be
  resumed.

### Devices

The game's device registers (APU, AC'97, USB/OHCI, the NV2A interrupt and
timer registers) are reached by direct calls, not faults:

- **Stores:** the lifter writes every guest store as `MEMn_W(addr, value)`
  (`tools/recomp/mem_stores.py`).
- **Routing:** reads and stores at or above 0xFD000000 call
  `xbox_mmio_read`/`xbox_mmio_write` (`kernel/xbox_mmio.c`), where each
  device model registered its range.
- **Faults:** with `RECOMP_MMIO_TRAPS=1` the page traps are armed as well,
  as on Linux before. The fault path itself works on hardware:
  `__libnx_exception_entry` builds a Win32-style CONTEXT, runs the
  vectored handlers and resumes with `svcReturnFromException`; an
  unhandled fault prints the registers and ends the program.

### Files

Horizon's file system differs from the others in two ways the title runs
into when it copies a disc archive into its Z: cache:

- **A second open fails** (EIO) while another handle has the file open
  for writing. The new handle shares the open one's descriptor and keeps
  its own position (`w32_handle_share`).
- **An open file cannot be renamed** (EIO). The rename closes every
  descriptor on the file, renames it and reopens them at the same
  positions (`w32_handle_rename_open`; `RECOMP_RENAME_REOPEN=1` forces this
  path on Linux for testing).

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
  exception handler, but the devices no longer need faults (see
  "Devices").
- **Software renderer only:** any GPU read-back from the homebrew's Mesa
  kills Ryujinx (Vulkan backend) or hangs it (OpenGL backend), and the
  OpenGL renderer still reads surfaces back every frame.

What Ryujinx's probe showed (real hardware agrees):
- system resource budget 0, so `svcMapPhysicalMemory` fails;
- `svcMapMemory` works in the stack region;
- `svcSetMemoryPermission` is refused there (`InvalidCurrentMemory`).

## Next

1. **OpenGL renderer on hardware:** `send.sh <ip> RECOMP_VULKAN=1`.
2. **GPU-resident surfaces:** keep render targets on the GPU instead of
   reading them back every frame, for speed and for Ryujinx.
