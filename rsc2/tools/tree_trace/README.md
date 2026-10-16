# Tree pop-in tracing

Tools for chasing objects that drop out of view (issue #1, tree pop-in).

## What it turned out to be

Whole stands of 2D trees vanished because the executor dropped part of a
draw. RalliSport Challenge 2 draws its tree sprites in batches through
D3D's `DrawVerticesUP`. One method header holds at most 2047 words, so D3D
splits a batch of more than 63 quads into chunks:
`BEGIN(QUADS)`, `INLINE_ARRAY`, `BEGIN(QUADS)`, `INLINE_ARRAY`, `END`.
The executor treated the second `BEGIN` as a fresh start and threw the first
63 quads away.

This was visible whenever two runs of sprites merged into one batch. The
game flushes the sprite batch before each full mesh, so when a mesh between
two runs faded in and moved to the opaque list, the runs merged. Long HUD
text batches were cut the same way. Fixed in `nv2a_pb_exec.c`: a `BEGIN`
while a batch is open draws that batch first.

## The game's object pipeline

- `sub_0010F0D0` culls objects. Its output is a list of 24-byte records:
  `{inst, dist @+4, class @+8, byte @+9, fade @+0x12, weight @+0x14}`.
- `sub_00273E70` sorts those records into four lists:
  - `+0x1B258`: opaque. An object goes here only when its fade byte is
    0xFF and its weight is 0.
  - `+0x12254`: fading. These are bucketed by distance.
  - `+0x2125C` and `+0x22A60`.
- `sub_00273890` walks the buckets. It copies type-1 items into the list at
  `renderer+0x2978` and passes that list to `sub_00114C80` (flags 0x10).
- `sub_00275010` passes the opaque list to `sub_00114C80`.
- `sub_00114C80` handles each record by its weight:
  - Weight 1.0: the record goes into the sprite batch at `0x4947D0+0x1F14`.
  - Weight between 0 and 1: crossfade, so the record goes into the batch
    and the mesh is drawn as well.
  - Weight 0: the mesh is drawn on its own by `sub_00112530`. The batch is
    flushed first by `sub_001101E0`.
- The game's pushbuffer put pointer is at `[0x372F70]`.

## Capturing

1. Put the trace hooks in `rsc2/config/trace_functions.json` and regenerate:
   ```json
   ["0x0010F0D0", "0x00273360", "0x00273890", "0x00112530", "0x001101E0",
    "0x00275010", "0x00114C80", "0x00273710"]
   ```
   then `rsc2/regen.sh && ninja -C rsc2/build`.
2. Run with the tracer and the flight recorder. A replay works too: set
   `RECOMP_VK_FLIGHT_AT=<frame>` in place of F9.
   ```sh
   RECOMP_TREE_TRACE=$PWD/work/tree/tree.bin RECOMP_VK_FLIGHT=$PWD/work/tree \
   RECOMP_VIDEO_RECORD=$PWD/work/tree/video.mkv rsc2/run.sh
   ```
3. Press **F9** (or **Guide** on a controller) when trees pop. This marks
   the frame in the trace and writes the flight recorder's last 180 frames.
   Every draw in the flight log carries `pb <address>`, its place in the
   pushbuffer. The trace records the put pointer around each drawing
   function, so a draw can be traced back to the object that issued it.
4. `RECOMP_TREE_PBDUMP=<n>` lists the pushbuffer methods of every sprite
   batch with at least n items, on stderr.
5. Set the hooks back to `[]` and regenerate afterwards.

## Analysing

- `tt.py`: reader for the trace (`tt.read(path)`).
- `batches.py tree.bin FRAME`: the main pass's sprite list (`B<n>` is a run
  of n sprites, `M` a mesh, `m<w>` a crossfade) and its batches, for the
  frame before and the frame itself.
- `analyze.py tree.bin`: render-bucket records that drop out before each
  mark.
- `steps.py flight_N.txt PROG,...`: per-program pixel drops on the main
  screen. Tree sprites are `6B1E852A`.
- `drops.py flight_N.txt`: draws that stop being submitted.

## Trace record kinds

Each record is a header `{u32 'TTF0', frame, kind, n, extra}` followed by n
16-byte entries.

| Kind | Contents |
|---|---|
| 1 | Cull output (layout above). |
| 2 | Render buckets at `sub_00273890` entry. |
| 3, 4 | The cull's 3rd and 4th arguments. |
| 5–8 | The four sorted lists (`+0x22A60`, `+0x2125C`, `+0x1B258`, `+0x12254`). In each entry, `pad` holds byte 0x12 and byte 9 << 8. |
| 9 | A mark. |
| 10 | One `sub_00112530` call. Two entries:<br>• `{inst, f32 inst+0x10, bytes, lod, draws, jumps, flags}`<br>• `{put0, put1, mesh, 0}` |
| 11 | One sprite batch flush. Entries: `{inst, 0, 0, alpha, 0, 0}`. `extra` = draws << 24 \| bytes. |
| 12 | Put range of a drawing function: `{function, put0, put1, depth}`. |
| 13 | One `sub_00114C80` call: its list, as in kind 1. `extra` = the return address, which identifies the caller (`0x273872` = buckets, `0x275058` = opaque). |
