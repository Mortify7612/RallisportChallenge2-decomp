# Audio and input lessons

## Audio (MCPX APU, DirectSound)

- The voice processor's resampler ignored the pitch rate, so SFX sounded
  "metallic" while music was fine. Use a per-voice cubic resampler at
  rate = out/in.
- The mixbin layout matters: the stub mixdown sent even bins to L and odd to
  R, playing the multipass bin and the reverb-send bin dry and clipping. Map
  speakers properly (L 0/4/6/8, R 1/5/7/9, C/LFE to both, FX 11-30 to both,
  drop multipass bin 31), undo the headroom, and add a soft limiter.
- The GP DSP56300 program isn't emulated. Reverb was rebuilt from the game's
  own **I3DL2 listener presets**: hook the DirectSound SetI3DL2Listener
  wrapper, then an FDN reverb with level normalisation and coefficient glide.
- Bink movie audio was bit-exact against ffmpeg once the lifter bugs were
  fixed, which makes it a good early audio check.
- For headless audio tests use `SDL_AUDIODRIVER=dummy RECOMP_AUDIO=1
  RECOMP_AUDIO_WAV=<file>`. Use `RECOMP_AUDIO=0` for unattended runs.
- `RECOMP_APU_DSP_ACK`/`RECOMP_DSP_ACK_ALLOC`-style workarounds unblock
  titles that wait on the DSP.
- **Music breaking up under load (RSC2 multi-car races on ARM):**
  - **Mixer pacing.** A mixer paced by the clock alone that resets its
    schedule when it falls behind loses that time from the output queue
    for good, and after a few late frames every hiccup is a dropout. Keep
    a cushion: when the queue is below ~24 ms, run at 4/3 of real time
    until it's back. Catching up all at once made the title's streams run
    dry (a metallic buzz).
  - **The APU interrupt must not wait for DISPATCH_LEVEL** (see
    kernel-hle.md). It waited up to 33 ms, and DirectSound refilled its
    streams late.
  - **Measure before guessing:** count output-queue underruns, stream
    voices with no data, interrupt waits and guest-lock waits every 5 s.
    Some no-data frames are normal: the PC showed ~100 per 5 s and sounds
    fine.

## Controllers

- `xbox_InputInit` was never called, so SDL pads never opened. Pads need the
  window thread's SDL event loop (hotplug, lowest free port, snapshot under a
  mutex). Map LB to White and RB to Black (as xemu does).
- Rumble arrives as an OHCI OUT endpoint-2 or SET_REPORT packet. Re-send it
  every 100 ms with a 250 ms duration.

## Keyboard as a controller

- Find the game's control-preset table and use its default preset for the
  keyboard layout. RSC2 preset 1: RT accelerate, LT brake, A e-brake, B/X
  gear up/down, Black camera, Y look back, White reset.
- **Check how steering is read before binding arrows.** RSC2 treats d-pad
  left/right as digital full lock overriding the stick, so steering keys go
  to the stick only, with easing (`RECOMP_KEY_STEER_MS`).
- One "menu" key: Start in a race, Back while the menu dispatcher ran in the
  last 250 ms. Hook the dispatcher through the manual function table; no
  regen is needed for vtable-only functions.
- The bindings live in a header shared by the input layer, window and
  launcher (`keymap.h`, Win32 VK codes, `id=vk[+vk];...` text form).
