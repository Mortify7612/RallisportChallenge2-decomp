# Nintendo Switch build (homebrew, work in progress)

`rsc2.nro`, a homebrew program for a Switch running custom firmware
(Atmosphère). It is built with devkitPro's devkitA64 and libnx. The game
code is the same as on the other platforms; what differs is the host layer
under it, in `xboxrecomp/src/platform/switch/`.

**Status:**
- It runs on a real Switch (Atmosphère, title-override mode). With the
  OpenGL renderer it gets through the intros and menus to car selection at
  about 7 fps, and loads a race at about 2 fps. The software renderer
  manages 1–3 fps.
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

### Threads

Horizon does not time-slice threads of equal priority, and the title
busy-waits for the GPU (and its device models) constantly. libnx starts
every pthread at priority 59 on the default core while the main thread --
the title's -- keeps 44, so a wait on it starved every host thread. Every
thread is now placed explicitly (win32_compat.c, "Thread placement"): one
priority, allowed on cores 0–2. Device-register polls yield every 64 reads.
`taskset -c 0-2 chrt -f 10 ./rsc2` models this on Linux (one CPU shows
every busy-wait).

### Diagnostics

- **Stall watchdog** (`RECOMP_STALL_DUMP=<s>`, 10 by default here): when
  no frame is shown for that long, each guest thread's last kernel calls,
  live guest stack and the host exception count are printed, twice. It
  stands in for `kill -QUIT` thread dumps.
- `RSC2_FIBER_TRACE=1` traces fiber switches and first runs a coroutine
  self-test.

### Other gaps

- **Missing headers:** newlib lacks `sys/mman.h`, `dlfcn.h`, `execinfo.h`,
  `sys/sysinfo.h` and `sys/syscall.h`; `platform/switch/include/` supplies
  them.
- **Fibers:** they use `host_coro.c`, as on Android. Its `coro_start`
  must be a hidden global symbol: as a bare label, devkitA64 took its
  address through the GOT, got `coro_switch` instead, and every new fiber
  spun there forever.
- **Diagnostics:** the Linux signal-based ones (`kill -QUIT` thread dumps,
  the sampling profiler) are left out.

## Emulator (Eden)

Eden (a yuzu fork, v0.2.1 tested) runs the `.nro` with the OpenGL
renderer, and runs the homebrew's own Mesa: shader compile failures, GL
errors, crashes and wrong frames show up there as on the console. It is
not a guide to speed: its GPU is the PC's.

Setup, once:
- the AppImage in `work/eden`, extracted there with
  `./Eden.AppImage --appimage-extract` (gives `AppDir/bin/eden-cli`);
- `prod.keys` from your own console in `~/.local/share/eden/keys/`;
- the game in `~/.local/share/eden/sdmc/switch/rsc2/game/`. Symlinks to
  your copy are fine, but make `UDATA` and `TDATA` real copies, so a test
  never writes to your saves.

`rsc2/switch/eden_test.sh` runs the current build there with no window
(`eden-cli` in Xvfb), replaying recorded input (`work/my_run2.xi` by
default, which reaches a race):

```
rsc2/switch/eden_test.sh -t 100 -d 300,600,900 -o work/eden-runs/base
rsc2/switch/eden_test.sh -t 100 -d 300,600,900 -r work/eden-runs/base RECOMP_GL_SPEC=0
```

Each run leaves the game's log, Eden's log, the dumped frames and a
`summary.txt` (crashes, stalls, the last `[PERF]` reports, differing
pixels against `-r`) in its folder, and exits 1 if the game crashed.
Settings go into `rsc2.env` on Eden's SD card for the run (`eden-cli -p`
does not reach homebrew); the file is put back afterwards. Two runs of
the same build agree to within about 1000 pixels a frame. Compare against
references made in Eden: its saves differ from a PC's, and so do the
menus the replay walks through.

Eden shares the console's `svc` limits: `MapPhysicalMemory` fails (system
resource 0) and `svcSetMemoryPermission` returns 0xd401.

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

1. **Speed:** keep render targets on the GPU instead of reading them back
   every frame (also what Ryujinx needs), then profile the CPU side.
