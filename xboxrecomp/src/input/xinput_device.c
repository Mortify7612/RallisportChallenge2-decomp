/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include "../../include/xbox/keymap.h"

/* A steering wheel through SDL's joystick API (wheel_sdl.c), merged on top
 * of port 0 like the keyboard. */
#ifdef XBOX_INPUT_WHEEL
#include "wheel_sdl.h"
#else
#define xbox_WheelMerge(port, st)       0
#define xbox_WheelConnected(port)       0
#define xbox_WheelRumble(port, lo, hi)  ((void)0)
#endif
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

/* ---- keyboard, when there is no pad --------------------------------------
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on.
 * The whole point of a recompilation is to be able to look at the thing
 * running, and a build nobody can press a button in cannot be looked at.
 *
 * Off by default, because a keyboard silently acting as player 1 is
 * surprising when a real pad is what you meant to use. RECOMP_KEYBOARD=1
 * turns it on. It only ever answers for port 0, and is merged on top of a
 * pad connected there, so a real controller keeps working.
 *
 * Which key presses which pad input is a table (xbox/keymap.h). Without
 * RECOMP_KEYMAP it is the layout a Dreamcast or Saturn emulator would pick,
 * the closest thing to a convention here:
 *
 *   arrows        d-pad             Enter      START
 *   Z X A S       A B X Y           Backspace  BACK
 *   Q E           white black       1 3        triggers
 *   numpad 8/2/4/6 left thumb       I/K/J/L    right thumb
 *
 * RECOMP_KEY_STEER_MS=<ms> eases the left stick's X axis instead of
 * snapping it: a key takes that long to reach full lock and half that to
 * come back. A racing title is all but undrivable when a tap is a full
 * swerve.
 *
 * Keys come from the framebuffer window, which only receives them while it
 * has the focus, so typing in another window does not drive the game.
 */
static BOOL keyboard_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

static const KeyMap *keymap(void)
{
    static KeyMap m;
    static int loaded;
    if (!loaded) {
        const char *v = getenv("RECOMP_KEYMAP");
        km_defaults(&m);
        if (v && *v)
            km_parse(&m, v);
        loaded = 1;
    }
    return &m;
}

/* The framebuffer window records what is held; see src/video/fb_present.c.
 *
 * GetAsyncKeyState was the first attempt and reads nothing here -- under
 * Wine it answers about a state a GDI-drawing guest never touches. Going
 * through the window is also the better gate: a window only receives keys
 * while it has the focus, so there is no separate focus check to get wrong
 * and no way for typing in another application to drive the game.
 *
 * Declared rather than included so src/input does not depend on the video
 * module's include path. */
extern int xbox_FramebufferKeyDown(int vk);

/* The "menu" input (KM_MENU) is the key a PC player reaches for to pause:
 * Start in play, but Back in the title's menus, where Start would accept
 * whatever is highlighted. The title says when its menus run by calling
 * this from their per-frame input handler; a quarter of a second without
 * a call is play. */
static volatile DWORD s_menu_tick;

void xbox_InputMenuTick(void)
{
    DWORD t = GetTickCount(), last = s_menu_tick;
    static int trace = -1;
    if (trace < 0)
        trace = getenv("RECOMP_MENU_TRACE") != NULL;
    /* RECOMP_MENU_TRACE: each return to the menus, after how long without. */
    if (trace && (!last || (DWORD)(t - last) >= 250))
        fprintf(stderr, "  [MENU] menus active again after %lu ms\n",
                last ? (unsigned long)(t - last) : 0ul);
    s_menu_tick = t ? t : 1;
}

static int menu_active(void)
{
    DWORD t = s_menu_tick;
    return t && (DWORD)(GetTickCount() - t) < 250;
}

/* Any of the keys bound to pad input `in`. */
static BOOL bound_down(int in)
{
    const KeyMap *m = keymap();
    int s;
    for (s = 0; s < KM_SLOTS; s++)
        if (m->key[in][s] && xbox_FramebufferKeyDown(m->key[in][s]))
            return TRUE;
    return FALSE;
}

/* A thumb axis from two inputs, at the full deflection a digital key implies. */
static SHORT axis_from_keys(int negative, int positive)
{
    int v = 0;
    if (bound_down(negative)) v -= 32767;
    if (bound_down(positive)) v += 32767;
    return (SHORT)v;
}

/* The left stick's X axis eased toward where the keys point (see above).
 * Driven by the clock, not by calls: the title polls once a frame at
 * whatever the frame rate is, and other callers poll in between. */
static SHORT steer_axis(void)
{
    static int ms = -1;
    static float pos;
    static DWORD last;
    DWORD now;
    float target, dt, step;

    if (ms < 0) {
        const char *v = getenv("RECOMP_KEY_STEER_MS");
        ms = v && *v ? atoi(v) : 0;
        if (ms < 0)
            ms = 0;
    }
    target = (float)axis_from_keys(KM_LX_NEG, KM_LX_POS) / 32767.0f;
    if (ms == 0)
        return (SHORT)(target * 32767.0f);

    now = GetTickCount();
    dt = last ? (float)(DWORD)(now - last) : 0.0f;
    last = now;
    if (dt > 100.0f)        /* a stall is not a reason to swerve */
        dt = 100.0f;
    /* Out toward lock at 1/ms, back toward the centre twice as fast --
     * including on the way through it to the other side. */
    if (target != 0.0f && (pos == 0.0f || (pos > 0.0f) == (target > 0.0f))) {
        step = dt / (float)ms;
        if (pos < target)      pos = pos + step > target ? target : pos + step;
        else if (pos > target) pos = pos - step < target ? target : pos - step;
    } else {
        step = 2.0f * dt / (float)ms;
        if (pos > 0.0f)      pos = pos - step < 0.0f ? 0.0f : pos - step;
        else if (pos < 0.0f) pos = pos + step > 0.0f ? 0.0f : pos + step;
    }
    return (SHORT)(pos * 32767.0f);
}

static void keyboard_state(XBOX_INPUT_STATE *pState)
{
    static const struct { int in; WORD bit; } digital[] = {
        { KM_UP, XBOX_GAMEPAD_DPAD_UP },       { KM_DOWN, XBOX_GAMEPAD_DPAD_DOWN },
        { KM_LEFT, XBOX_GAMEPAD_DPAD_LEFT },   { KM_RIGHT, XBOX_GAMEPAD_DPAD_RIGHT },
        { KM_START, XBOX_GAMEPAD_START },      { KM_BACK, XBOX_GAMEPAD_BACK },
        { KM_LTHUMB, XBOX_GAMEPAD_LEFT_THUMB }, { KM_RTHUMB, XBOX_GAMEPAD_RIGHT_THUMB },
    };
    static const struct { int in, idx; } analog[] = {
        { KM_A, XBOX_BUTTON_A },         { KM_B, XBOX_BUTTON_B },
        { KM_X, XBOX_BUTTON_X },         { KM_Y, XBOX_BUTTON_Y },
        { KM_BLACK, XBOX_BUTTON_BLACK }, { KM_WHITE, XBOX_BUTTON_WHITE },
        { KM_LT, XBOX_BUTTON_LTRIGGER }, { KM_RT, XBOX_BUTTON_RTRIGGER },
    };
    static DWORD packet;
    WORD b = 0;
    size_t i;

    memset(pState, 0, sizeof(*pState));

    for (i = 0; i < sizeof digital / sizeof digital[0]; i++)
        if (bound_down(digital[i].in))
            b |= digital[i].bit;
    {
        /* Which of the two it means is settled when it goes down and kept
         * while it is held: Start opens the pause menu, and that menu must
         * not then see the same press turn into Back. */
        static int held_as = 0;          /* 0 up, 1 Start, 2 Back */
        if (bound_down(KM_MENU)) {
            if (!held_as)
                held_as = menu_active() ? 2 : 1;
            b |= held_as == 2 ? XBOX_GAMEPAD_BACK : XBOX_GAMEPAD_START;
        } else {
            held_as = 0;
        }
    }
    pState->Gamepad.wButtons = b;

    /* Analog on the console, so a key is 255 rather than a flag -- a title
     * that reads these as a pressure never sees a press if they are 1. */
    for (i = 0; i < sizeof analog / sizeof analog[0]; i++)
        pState->Gamepad.bAnalogButtons[analog[i].idx] = bound_down(analog[i].in) ? 255 : 0;

    pState->Gamepad.sThumbLX = steer_axis();
    pState->Gamepad.sThumbLY = axis_from_keys(KM_LY_NEG, KM_LY_POS);
    pState->Gamepad.sThumbRX = axis_from_keys(KM_RX_NEG, KM_RX_POS);
    pState->Gamepad.sThumbRY = axis_from_keys(KM_RY_NEG, KM_RY_POS);

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    pState->dwPacketNumber = ++packet;
}

/* The keyboard merged on top of whatever the pad reported: a real pad keeps
 * working, and the keyboard only adds to it. Shared by both backends. */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;
    keyboard_state(&kb);
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
#ifdef XBOX_INPUT_WHEEL
    xbox_WheelStart();
#endif

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0 && keyboard_enabled()) {
            keyboard_state(pState);
            xbox_WheelMerge(dwPort, pState);
            return ERROR_SUCCESS;
        }
        if (xbox_WheelConnected(dwPort)) {
            memset(pState, 0, sizeof(XBOX_INPUT_STATE));
            xbox_WheelMerge(dwPort, pState);
            return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    /* White sits left of Black on the Xbox pad: LB and RB, as xemu maps
     * them. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    /* Merge the keyboard on top rather than only standing in for a missing
     * pad.
     *
     * The first version put the keyboard behind XInput's failure, on the
     * assumption that with nothing plugged in the call would fail. It does
     * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
     * with every button at rest, so the fallback was unreachable and
     * pressing a key did nothing at all.
     *
     * Merging is also the better rule. A real pad keeps working -- its
     * buttons are already in pState and the keyboard only adds to them --
     * and there is no special case left to get wrong. */
    if (dwPort == 0 && keyboard_enabled())
        keyboard_merge(pState);
    xbox_WheelMerge(dwPort, pState);

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    result = XInputSetState(dwPort, &xi_vib);
    xbox_WheelRumble(dwPort, pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed);
    if (result != ERROR_SUCCESS && xbox_WheelConnected(dwPort))
        result = ERROR_SUCCESS;
    return result;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort] || xbox_WheelConnected(dwPort);
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS && xbox_WheelConnected(dwPort)) {
        memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
        pCaps->Type = 1;      /* a gamepad, which is what the wheel plays as */
        pCaps->SubType = 1;
        return ERROR_SUCCESS;
    }
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>
#include <pthread.h>

/* SDL's joystick layer belongs to the thread that pumps its events, which is
 * the framebuffer window's (src/video/fb_present.c). That thread opens and
 * closes pads as they come and go (xbox_InputSdlEvent), reads them once per
 * loop into a snapshot (xbox_InputSdlPoll), and sends rumble; the title's
 * input reads, on its own threads, only copy the snapshot. Calling SDL from
 * the title's thread raced the event pump, and nothing called
 * xbox_InputInit, so no pad was ever opened at all. */
static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static SDL_JoystickID      g_pad_id[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static XBOX_INPUT_STATE g_snap[XBOX_MAX_CONTROLLERS];
static WORD  g_rumble[XBOX_MAX_CONTROLLERS][2];
static int   g_rumble_dirty[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void xbox_InputInit(void)
{
    /* Nothing to do on this thread; the window thread owns the pads. */
}

/* From the window thread, once SDL video is up. Pads already attached
 * arrive as SDL_CONTROLLERDEVICEADDED events right after. */
void xbox_InputSdlInit(void)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)
            && SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "  [INPUT] SDL game controller init failed: %s\n", SDL_GetError());
#ifdef XBOX_INPUT_WHEEL
    xbox_WheelSdlInit();
#endif
}

/* Hotplug: a new pad takes the lowest free port, a removed one frees it. */
void xbox_InputSdlEvent(const SDL_Event *ev)
{
    int port;
#ifdef XBOX_INPUT_WHEEL
    xbox_WheelSdlEvent(ev);
#endif
    if (ev->type == SDL_CONTROLLERDEVICEADDED) {
        SDL_GameController *c;
        SDL_JoystickID id;
#ifdef XBOX_INPUT_WHEEL
        /* A wheel with a gamepad mapping is still the wheel's. */
        if (xbox_WheelIsWheel(ev->cdevice.which))
            return;
#endif
        for (port = 0; port < XBOX_MAX_CONTROLLERS; port++) {
            if (g_pads[port] && SDL_JoystickInstanceID(
                    SDL_GameControllerGetJoystick(g_pads[port]))
                    == SDL_JoystickGetDeviceInstanceID(ev->cdevice.which))
                return;                          /* already open */
        }
        for (port = 0; port < XBOX_MAX_CONTROLLERS && g_pads[port]; port++)
            ;
        if (port == XBOX_MAX_CONTROLLERS)
            return;
        c = SDL_GameControllerOpen(ev->cdevice.which);
        if (!c)
            return;
        id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
        pthread_mutex_lock(&g_lock);
        g_pads[port] = c;
        g_pad_id[port] = id;
        g_controller_connected[port] = TRUE;
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "  [INPUT] controller %d: %s\n", port + 1,
                SDL_GameControllerName(c) ? SDL_GameControllerName(c) : "(unnamed)");
    } else if (ev->type == SDL_CONTROLLERDEVICEREMOVED) {
        for (port = 0; port < XBOX_MAX_CONTROLLERS; port++) {
            if (g_pads[port] && g_pad_id[port] == ev->cdevice.which) {
                pthread_mutex_lock(&g_lock);
                SDL_GameControllerClose(g_pads[port]);
                g_pads[port] = NULL;
                g_controller_connected[port] = FALSE;
                memset(&g_snap[port], 0, sizeof g_snap[port]);
                pthread_mutex_unlock(&g_lock);
                fprintf(stderr, "  [INPUT] controller %d removed\n", port + 1);
            }
        }
    }
}

static void read_pad(SDL_GameController *c, XBOX_INPUT_STATE *st)
{
    WORD btn = 0;
    memset(st, 0, sizeof *st);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    st->Gamepad.wButtons = btn;

    /* Positional, so a Nintendo-layout pad's A is still the bottom button. */
    st->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    st->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    st->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    st->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    /* White sits left of Black on the Xbox pad: LB and RB, as xemu maps
     * them. */
    st->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    st->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    st->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    st->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    st->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    st->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    st->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    st->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
}

/* From the window thread, after its events: snapshot every pad and send
 * the rumble the title asked for. Rumble is re-sent while it is on, with a
 * short duration, so a title that stops updating it (a pause, a crash)
 * does not leave the pad buzzing. */
void xbox_InputSdlPoll(void)
{
    static Uint32 last_rumble[XBOX_MAX_CONTROLLERS];
    Uint32 now = SDL_GetTicks();
    int port;
#ifdef XBOX_INPUT_WHEEL
    xbox_WheelSdlPoll();
#endif
    for (port = 0; port < XBOX_MAX_CONTROLLERS; port++) {
        XBOX_INPUT_STATE st;
        WORD lo, hi;
        int dirty;
        if (!g_pads[port])
            continue;
        read_pad(g_pads[port], &st);
        pthread_mutex_lock(&g_lock);
        g_snap[port] = st;
        lo = g_rumble[port][0];
        hi = g_rumble[port][1];
        dirty = g_rumble_dirty[port];
        g_rumble_dirty[port] = 0;
        pthread_mutex_unlock(&g_lock);
        if (dirty || ((lo || hi) && now - last_rumble[port] >= 100)) {
            SDL_GameControllerRumble(g_pads[port], lo, hi, (lo || hi) ? 250 : 0);
            last_rumble[port] = now;
        }
    }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    BOOL have;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    pthread_mutex_lock(&g_lock);
    have = g_controller_connected[dwPort];
    if (have)
        *pState = g_snap[dwPort];
    pthread_mutex_unlock(&g_lock);

    if (!have) {
        if (dwPort == 0 && keyboard_enabled()) {
            keyboard_state(pState);
            xbox_WheelMerge(dwPort, pState);
            return ERROR_SUCCESS;
        }
        if (xbox_WheelConnected(dwPort)) {
            memset(pState, 0, sizeof(XBOX_INPUT_STATE));
            xbox_WheelMerge(dwPort, pState);
            return ERROR_SUCCESS;
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    pState->dwPacketNumber = ++g_packet[dwPort];
    if (dwPort == 0 && keyboard_enabled())
        keyboard_merge(pState);
    xbox_WheelMerge(dwPort, pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    static int rumble = -1;
    static const XBOX_VIBRATION still = { 0, 0 };
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    /* RECOMP_RUMBLE=0: the pad never vibrates. */
    if (rumble < 0) {
        const char *v = getenv("RECOMP_RUMBLE");
        rumble = !(v && strcmp(v, "0") == 0);
    }
    if (!rumble)
        pVibration = &still;
    pthread_mutex_lock(&g_lock);
    if (g_rumble[dwPort][0] != pVibration->wLeftMotorSpeed
            || g_rumble[dwPort][1] != pVibration->wRightMotorSpeed) {
        g_rumble[dwPort][0] = pVibration->wLeftMotorSpeed;
        g_rumble[dwPort][1] = pVibration->wRightMotorSpeed;
        g_rumble_dirty[dwPort] = 1;
    }
    pthread_mutex_unlock(&g_lock);
    xbox_WheelRumble(dwPort, pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed);
    return g_controller_connected[dwPort] || xbox_WheelConnected(dwPort)
        ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    /* The keyboard stands in for pad 0, so a title asking whether it is
     * there gets the same answer xbox_InputGetState gives. */
    if (dwPort == 0 && keyboard_enabled()) return TRUE;
    return g_controller_connected[dwPort] || xbox_WheelConnected(dwPort);
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort] && !(dwPort == 0 && keyboard_enabled())
            && !xbox_WheelConnected(dwPort))
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
