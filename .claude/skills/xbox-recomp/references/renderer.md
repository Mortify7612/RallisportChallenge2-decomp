# NV2A pushbuffer and Vulkan renderer lessons

Architecture that worked: the lifted D3D writes NV2A pushbuffers into guest
RAM, and the executor (`nv2a_pb_exec.c`) parses them. Vertex programs run as
translated GLSL (an interpreter is the fallback), register combiners run in a
fragment uber-shader plus specialised variants, and surfaces are copied back
to guest RAM at flip only where the game reads them.

## Pushbuffer parsing bugs (each looked like a game bug)

- **Ring wrap:** scanning the old lap only up to the highest PUT dropped the
  commands between the lap's last PUT and its JUMP. A lost
  `SET_COLOR_MASK` let alpha into the reflection cube, so the car came out
  white, once per lap and timing-dependent. Scan last_put..JUMP, then
  JUMP target..put.
- **Split DrawVerticesUP:** D3D splits a draw bigger than one method header
  (2047 words) into BEGIN, INLINE_ARRAY, BEGIN, INLINE_ARRAY, ..., END. A
  BEGIN while a batch is open must *draw* that batch, not discard it. This
  was the tree pop-in and HUD text loss.
- Points and lines were silently dropped ("not yet"), so antennas went
  missing. Implement POINTS (size from SET_POINT_SIZE/8 or oPts; point
  sprite coords on stage 3) and LINES/strips.
- `RECOMP_PRIM_STATS` counts primitives the renderer can't draw; check it
  early.

## Vertex programs

- **All outputs start as (0,0,0,1), including oPos**, as in xemu. With w=0, a
  screen-space program that writes only oPos.xyz drew nothing; that was the
  missing sun glare and some mountain popping.
- Vertex colour outputs are clamped to [0,1].
- The GLSL header string inside `nv2a_vk.c` (`k_prog_head`) duplicates the
  UBO layout from `nv2a_common.glsl`. Keep them in sync, and bump
  `VK_PROG_VERSION` when either changes, or every program silently falls back
  to the CPU (30 fps).

## Depth, offsets, culling

- W-buffer (CONTROL0 bit 20), integer depth rounding, min-LOD clamp, depth
  written only when the depth test is on, per-program viewport constants,
  SET_CLIP_MIN/MAX and stencil (the sky dome is masked by stencil).
- **Polygon offset is in screen-Z LSBs even under the w-buffer.** Adding it to
  w, as xemu does, pushed decals about 15 m forward. Convert with
  z = zmax − B/w, so dw = dz·w/(zmax−z). It fixed tyre marks and the shadow
  blob.
- For speed, use a reversed w-buffer into D32F and avoid `gl_FragDepth` unless
  needed (a fast-depth shader variant chosen per draw), so early-Z works.
- Sampler LOD bias (SET_TEXTURE_FILTER bits 0-12, signed 5.8) and the
  max-LOD clamp both matter.

## Render targets, textures, performance

- Games alias render-target memory with different shapes (linear vs
  swizzled, cube faces, shadow maps). Sample dirty targets GPU-side (copy
  into a texture image), blit swizzled targets into cube faces, and turn a
  "cleared-only linear surface aliasing a swizzled target" into a GPU clear.
  This took syncs from 5 to 1 per frame.
- **Frame pipelining:** submit at flip, retire the previous frame at the next
  flip (two slots), and retire early only when a known in-flight surface is
  retargeted. That gave +23-45% fps.
- The combiner uber-shader was nearly all GPU time at high resolution
  (dynamic register indexing goes to local memory). Specialise per combiner
  setup with constants, compile on a worker thread and cache to disk, and
  draw with the uber-shader until the specialised one is ready (no stalls).
- Upload DXT1/3/5 as BC1/2/3 instead of decoding on the CPU. Streamed track
  textures caused periodic 40 ms hitches.
- Hash textures once per epoch, not per draw. Build per-draw UBOs in local
  memory and memcpy, because reading back from write-combined memory costs
  about 1 µs per draw.
- Pick "the frame" as the surface with the AvSetDisplayMode pitch that isn't
  swizzled, not the largest clip (car select renders the car into a
  1280×440 target).

## Presentation and image quality

- **Gamma ramp:** the game sets per-scene ramps through D3D SetGammaRamp, and
  the console's video DAC applies them. Without them the image looked flat.
  Apply them as a LUT pass at present.
- **Widescreen:** the game reads XC_VIDEO EEPROM flags (widescreen
  0x00010000, 720p 0x20000, 1080i 0x40000, 480p 0x80000, letterbox
  0x100000). It renders anamorphic 640×480, so scale horizontally by
  ceil(scale·4/3) for sharp 16:9.
- MSAA (scene targets with zeta), anisotropic filtering, and SAO ambient
  occlusion from the reversed w-buffer were added as launcher options.
- MAILBOX presentation with a separate flip pacer (fps limit). RSC2 keeps
  time by the real clock, so running above 60 fps was fine.

## Lessons from the Android port and later (2026-10-17)

- **Pipeline creation is the shader stall, not compiling.** About 35 ms per
  pipeline against about 3 ms per glslang compile, and 52 at a race load.
  Build both on a worker thread. While they build, draw program draws on the
  CPU vertex path (it already exists as a fallback). Record every pipeline
  built and rebuild them in the background at the next launch (warm-up).
  Also: don't build the general pipeline when the specialised one is ready.
- **Android swapchain:** use preTransform IDENTITY, and treat SUBOPTIMAL as
  OK unless the extent changed. Android reports suboptimal on every present
  of a rotated panel, and rebuilding the swapchain each frame cost half the
  frame rate.
- **Mobile GPUs:** combiner maths in mediump (`NV2A_MP`) gives about +17%.
  Measure on the phone with simpleperf (NDK `app_profiler.py`, debuggable
  APK, `-gline-tables-only`). Cool the phone to about 45 C before each run
  and use alternating A/B runs, because thermal throttling moves results by
  5 fps or more.
- **Executor CPU on ARM:** hashing guest memory to detect changes was a
  third of the time. Use sparse hashes (64 B per KB) with a periodic full
  check. Unroll the vertex fetch per format and size, and handle bulk
  pushbuffer methods (inline arrays, index lists, constants) in one call.
- **Shortcuts with aliased surfaces need an exact match.** Cancelling an
  in-flight copy-back is safe only for the same bytes in another shape (a
  linear clear of a swizzled shadow map). Widening that to "anything inside
  the cleared range" broke the pause menu, so regression-test menus against
  the last release binary.
- **Filling screens wider than 16:9:** squeeze clip-space x of scene draws
  by (16/9)/aspect in the vertex tail, except flat (w=1) vertices within one
  title pixel of the screen edge, which keeps full-screen passes covering the
  screen. Widen the title's own culling frustum to the same aspect (found by
  reading the frustum code), and present stretched to the real aspect.

## HDR output (2026-10-17)

- HDR10 swapchain (needs instance ext VK_EXT_swapchain_colorspace) plus a
  final pass: decode with gamma 2.2, put the title's white at paper white
  (203 nits), stretch only luminance above a knee, and keep the HUD flat. To
  tell the HUD apart, replay its draws (the last draws with depth test off)
  into a side buffer.
- **Check usage flags when a new pass samples an existing image.** The
  gamma-ramp output had no SAMPLED usage. RADV returned the right data
  anyway, but NVIDIA returned zeros, so the race (the only scenes with a
  ramp) went black. Run headless tests on every GPU you have; validation
  layers would have caught it.
- On KDE Wayland the HDR desktop keeps the monitor in HDR all the time, so
  there's no HDR logo when the game starts. Check with WAYLAND_DEBUG=1 for
  `set_tf_named(11)` (PQ) and `set_primaries_named(6)` (BT.2020).
- **On Wayland, draw HDR in the compositor's own colour space.** A PQ or
  scRGB window makes KWin convert every frame, and tonemap it too if the
  content's peak (after scaling white to the SDR brightness) is above the
  screen's. Instead: a PASS_THROUGH swapchain (NVIDIA's and Mesa's WSI then
  set no description), 10-bit, and set the compositor's *preferred*
  description object itself on the surface (wp_color_management_v1 surface
  feedback `get_preferred`, then `set_image_description`). Read its info
  (KWin: BT.2020, gamma 2.2, 0..peak, ref = SDR brightness) and encode for
  it, and redo it on `preferred_changed`. Only one colour-management surface
  object is allowed per wl_surface, so never use a WSI HDR colour space on
  the same surface. dlopen libwayland-client; the generated protocol code
  works with wl_surface/wl_output types set to NULL. RSC2: wl_color.c; on a
  PRIME desktop (KWin on the iGPU, screen on the dGPU) this cut slow frames
  from 61% to 1.3%. KWin never direct-scans out buffers from a GPU other
  than its render GPU.

## Texture descriptors carry stale registers

A texture stage's state includes registers its format never reads: the
palette address (only P8, format 0x0B, uses it) and the pitch (only linear
formats do; swizzled and DXT size from format). Titles leave whatever an
earlier draw set there. Keyed on the whole descriptor, RSC2's DXT textures
showed up with two or three pitches and palettes each, so every one was
cached, hash-checked and uploaded several times, and any per-draw lookup
cache missed. Clear the unused fields before keying (the GL renderer's
tex_get does; check the Vulkan renderer's texture cache for the same).

Checking textures for changes less often (sampled hashes from smaller sizes,
skipping "settled" textures) saved little on the Switch and made RSC2's road
flash, a change seen late. Make each check cheaper, not rarer.
