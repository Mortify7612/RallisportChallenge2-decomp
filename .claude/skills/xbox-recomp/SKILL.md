---
name: xbox-recomp
description: Playbook for statically recompiling an original Xbox game into a native PC program with the xboxrecomp toolkit (lifted x86 -> C, HLE kernel, NV2A pushbuffer executor, Vulkan renderer). Use when starting or continuing an Xbox (2001-2005, XBE) recompilation or port, when debugging one (crashes, hangs, missing geometry, wrong colours, audio), or when porting such a build to Windows, ARM64/Android or another platform. Distilled from the RalliSport Challenge 2 project.
---

# Xbox static recompilation playbook

Learned on RalliSport Challenge 2 (RSC2): from first boot to a released
Linux/Windows build with a Vulkan renderer, in about three days. Repo:
https://github.com/Mortify7612/RallisportChallenge2-decomp. Its vendored
`xboxrecomp/` is the patched toolkit. **Start a new game from that copy, not
from a fresh upstream clone**, because upstream lacks every fix listed in
[references/toolkit-fixes.md](references/toolkit-fixes.md).

## Reference files (open the one you need)

- [references/toolkit-fixes.md](references/toolkit-fixes.md): what was wrong in the toolkit and lifter, so you recognise it when a new game hits something similar.
- [references/debugging.md](references/debugging.md): the methods that actually found bugs (differential replay, input record/replay, headless frame dumps, write watches, flight recorder) and the measurement traps.
- [references/renderer.md](references/renderer.md): NV2A pushbuffer and Vulkan renderer lessons (pushbuffer parsing, vertex programs, depth, render targets, performance, widescreen, gamma).
- [references/kernel-hle.md](references/kernel-hle.md): kernel, memory map, threads/DPCs, video flags, fibers.
- [references/audio-input.md](references/audio-input.md): APU/DirectSound, controllers, keyboard mapping.
- [references/platforms.md](references/platforms.md): Windows (MinGW + Wine), ARM64/Android groundwork, releases, legal hygiene, Steam.

## Workflow that worked

1. **Layout.** Use a project root holding the user's game files (never in git),
   the vendored `xboxrecomp/`, and a game project (`<game>/`) with `regen.sh`,
   `config/seed_functions.json`, `config/trace_functions.json` (keep it `[]`
   normally), `src/<game>_overrides.c` and `src/main.c`. Put title defaults
   in a `defaults[]` env table in `main.c`; a hand-set env var wins, and `=0`
   unsets it.
2. **Get it booting headless, then in a window.** Seed unresolved functions,
   regen and rebuild until it reaches the intro movie. After editing seeds,
   overrides or excluded functions, run `regen.sh`; when the toolkit's
   `recomp_types.h` template changes, delete the generated copy.
3. **Make it deterministic before chasing bugs.** Record and replay input at
   the game's own XInputGetState call, with a frame-locked guest clock (see
   debugging.md). From then on every fix is checked with a headless replay,
   not by playing.
4. **Software rasteriser first, then Vulkan.** Keep the software path as a
   fallback (`RECOMP_VULKAN=0`). It also gives a GPU-independent reference
   for frame comparisons.
5. **Fix what the user reports against a real-console reference video** of
   the same scene. "Is this a bug or how the game looks?" is settled by
   footage, not by guessing. Several "game behaviours" were executor bugs.
6. **Then the polish players notice:** launcher with saved settings,
   widescreen through the game's own flag, resolution scale, MSAA/AF/AO,
   gamma ramp, controller + rumble, keyboard layout matching the game's
   default preset, FPS counter, fullscreen toggles.
7. **Release** binaries without game data (see platforms.md for the probe
   check).

## Rules that saved the most time

- **A bug that looks like the game's logic is usually the executor's.**
  Missing trees, a white car and the missing sun were all pushbuffer
  executor or vertex-program bugs: a dropped batch, a ring-wrap scan and an
  oPos.w initial value. Check the executor's handling of the exact methods
  involved before reverse-engineering the game's culling.
- **Reason from evidence, not from the first theory.** Bisect with env
  toggles (every renderer feature has an `=0` off switch), compare against
  replays, and look at frames. Several plausible theories (LOD bias,
  occlusion queries, render-target aliasing, cull-list overflow) were
  disproved before the real cause turned up.
- **Every feature gets an env switch to turn it off**, and every
  investigation leaves a diagnostic env behind (`RECOMP_*_TRACE`,
  `*_STATS`, `*_DUMP`). They get reused constantly.
- **The Xbox has one CPU; here guest threads run truly in parallel.**
  Ordering bugs between guest threads and DPCs/ISRs cause rare
  nondeterministic crashes, for example garbage handles of `0xCCCCCCCC`.
  See kernel-hle.md (dispatch lock, `RECOMP_GUEST_LOCK`).
- **Keep the guest's 4 GB span reserved for the life of the process.**
  Otherwise host libraries get mapped inside guest space, and stray guest
  pointers corrupt host memory silently instead of faulting.
- **The game's own settings files leak into tests.** A launcher's
  `settings.ini` applies to headless runs too, so set env explicitly in test
  harnesses. Saves can live in the game folder (`UDATA`/`TDATA`). Run tests
  against an isolated copy (symlink the game files, copy the saves) so tests
  never touch the player's save, and so a replay keeps matching the progress
  it was recorded with.

## Working with this user (captainslow48)

- Ask and wait for an explicit yes before launching a **windowed or
  capture** session. Headless runs (`RECOMP_FB_WINDOW=0`) are fine
  unasked.
- Commit and push each verified change as you go. Features that need a
  live test get committed only after the user confirms them live. Pushing
  needs the sandbox disabled (credentials are in the keyring).
- Players bring their own game data. Never commit or upload anything
  derived from the XBE, except the translated code built into release
  binaries, which the user explicitly chose to ship.
- Keep separate efforts on separate branches when asked (for example the
  `arm64` branch for the ARM work).
