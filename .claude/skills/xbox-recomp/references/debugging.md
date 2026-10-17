# Debugging methods that found real bugs

## Determinism first: input record/replay + frame clock

- Hook the game's **XInputGetState** (find its only caller; in RSC2 one input
  function polled pad 0 once per main-loop frame). Record `<call#> <port>
  <ret> <22-byte XINPUT_STATE hex>` per change, and replay by call number.
  Replaying by wall time or by presented frames drifted, because loading
  screens present a varying number of frames.
- Recording or replaying turns on a **frame-locked guest clock**: TSC, QPC
  and KeTickCount advance 1/60 s per game frame, ticked from the XInput hook,
  with per-thread 50 µs steps within a frame. Stalls over 100 ms flow at 1/8.
- Replays are close but not bit-exact: they diverge after a race load
  (threads). Compare frames only up to the first divergence, and use the
  noise between two runs of the *same* build as the baseline.
- When the save progress changes, an old replay hits new dialogs. Patch the
  `.xi` file with extra button presses (give them packet numbers between
  their neighbours) and shift later call numbers, rather than re-recording.

## Headless frames

- `RECOMP_FB_WINDOW=0` runs without a window. `RECOMP_FB_DUMP_AT=<dir>:<n>,<n>,...`
  dumps numbered frames, `RECOMP_VK_HIRES_DUMP=<bmp>:<frame>` dumps the
  scaled Vulkan surface, and `RECOMP_FB_DUMP_FLIPS` dumps every flip (for
  ffmpeg).
- Compare BMPs with numpy (no Pillow needed). Make contact sheets with
  `ffmpeg -filter_complex xstack` and look at them.
- Movies play by wall clock, so slow runs (qemu, debug builds) differ in the
  intro by design.

## Differential replay of single functions

List functions in `trace_functions.json` and regen. `RECOMP_SNAPSHOT=<va>:<n>`
(or `RECOMP_SNAPSHOT_LIST` + `_AFTER_MS`) saves pre/post guest state (XRSNAP03:
GPRs, xmm, x87 cw/top/ring). `tools/replay32` then runs the *original* x86 code
natively on the pre-state and compares. `replay_sweep.py` batches it; the
baseline noise was SIGNAL 4-5, regs 0. This found the `fcmovcc` bug. Sweeping
367 race-time float functions found no lifting bug, which ruled the lifter
out for the tree problem. Restore `trace_functions.json` to `[]` afterwards.

## Finding who writes or reads memory

- `RECOMP_WATCH=<va>` or a pointer chain `"[[0x006DF414]]+0x14"`: page-traps
  writes and names the writing guest function. It only sees guest-thread
  writes.
- `RECOMP_READ_TRAP=<va>:<bytes>[:<frame>]`: who reads.
- `RECOMP_PEEKF`/`RECOMP_POKEF`: sample or force guest floats live.
- `addr2func.py`: guest address -> function + disassembly.

## Renderer diagnostics (Vulkan)

`RECOMP_VK_DRAWLOG=<file>:<frames>` (per-draw state, combiner words, "pb
<addr>" source), `RECOMP_VK_PROBE=x,y:<frames>` (pixel after each draw),
`RECOMP_VK_FLIGHT=<dir>` + F10 (flight recorder with per-draw occlusion
sample counts), `RECOMP_VSH_DUMP=<hash>` (vertex program inputs, outputs and
listing), `RECOMP_VK_STATS` (draws, syncs, gpu ms, gpu-wait),
`RECOMP_VK_HITCH=<ms>` (what happened in slow frames). Per-draw pixel counts
in the flight log found the tree pop-in: one batch's samples dropped by
63 quads.

## Hangs and crashes

- `kill -QUIT <pid>` prints all-thread backtraces. gdb is unusable because
  the trap-flag stepping fights it.
- A crash handler that prints guest registers, the guest function (dladdr on
  generated symbols, link with `-rdynamic`) and peek values.
- Hang patterns seen: a polled object that never signals (thread exit
  status), a spin on a device bit that is never set (PFB flush, AC'97 reset
  bit, DSP ack), and a heap free claiming foreign memory.

## Performance measurement traps

- Sample stacks with tgkill SIGUSR2 (no `perf` needed) to see *where* a thread
  waits. RSC2's main thread spun 67% in D3D's PFB flush wait, so the CPU and
  GPU were serialised.
- NVIDIA randomly runs a whole process in a slower clock state (the same work
  took 8.6 vs 12 ms). Alternate A/B runs and compare frame windows after
  warm-up.
- When a harness wraps the game (`prime-run`, `run.sh`), `timeout`/`pkill`
  hits the wrapper, not the game. A stray headless game then poisons every
  later measurement. Kill by exact name (`pkill -x`) and refuse to start if
  one is running.
- On hybrid laptops/desktops, verify which GPU ran: sysfs `gpu_busy_percent`
  and `nvidia-smi`, not the log line alone.
