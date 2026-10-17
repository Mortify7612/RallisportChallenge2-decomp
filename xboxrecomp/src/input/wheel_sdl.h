/* wheel_sdl.h -- a steering wheel as the Xbox pad on port 0 (wheel_sdl.c).
 * Built when SDL2 is available (XBOX_INPUT_WHEEL). */
#ifndef XBOX_WHEEL_SDL_H
#define XBOX_WHEEL_SDL_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

union SDL_Event;

/* The title's side, from any thread. */
int  xbox_WheelMerge(DWORD port, XBOX_INPUT_STATE *st);  /* 1 if a wheel is on the port */
int  xbox_WheelConnected(DWORD port);
void xbox_WheelRumble(DWORD port, WORD lo, WORD hi);

/* The SDL side, from the thread that pumps SDL's events. */
void xbox_WheelSdlInit(void);
void xbox_WheelSdlEvent(const union SDL_Event *ev);
void xbox_WheelSdlPoll(void);
int  xbox_WheelIsWheel(int device_index);

#ifdef _WIN32
/* Windows has no SDL window to pump events: a thread of its own. */
void xbox_WheelStart(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
