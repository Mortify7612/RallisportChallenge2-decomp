# Tree pop-in tracing

Tools for chasing objects that drop out of view (issue: tree pop-in).

## Capturing

1. Put the trace hooks in `rsc2/config/trace_functions.json` and regenerate:
   ```json
   ["0x0010F0D0", "0x00273360", "0x00273890"]
   ```
   then `rsc2/regen.sh && ninja -C rsc2/build`.
2. Run with the tracer, the flight recorder and a video:
   ```sh
   RECOMP_TREE_TRACE=$PWD/work/tree/tree.bin RECOMP_VK_FLIGHT=$PWD/work/tree \
   RECOMP_VIDEO_RECORD=$PWD/work/tree/video.mkv rsc2/run.sh
   ```
3. Press **F9** (or **Guide** on a controller) when trees pop: it marks the
   frame in the trace and writes the flight recorder's last 180 frames.
4. Set the hooks back to `[]` and regenerate afterwards.

## Analysing

- `tt.py` — reader for the trace (`tt.read(path)`).
- `analyze.py tree.bin` — render-bucket records that drop out before each mark.
- `steps.py flight_N.txt PROG,...` — per-program pixel drops on the main screen.
- `drops.py flight_N.txt` — draws that stop being submitted.

Trace record kinds: 1 size-cull output (`sub_0010F0D0`, layout not yet
decoded), 2 render buckets at `sub_00273890` entry (instance, distance, fade
weight, LOD class, type), 3/4 the cull's 3rd/4th arguments, 9 a mark.
