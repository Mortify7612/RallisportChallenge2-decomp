/*
 * wheel_sdl.c -- a steering wheel and pedals as the Xbox pad on port 0.
 *
 * Wheels are not gamepads. On Windows they are DirectInput/HID devices that
 * XInput never sees, and on Linux SDL's GameController layer only opens
 * devices with a gamepad mapping, which wheels lack. Both are reached
 * through SDL's plain joystick API instead: a device SDL types as a wheel
 * (it knows the common Logitech, Thrustmaster and Fanatec ones), one whose
 * name contains "wheel", or the one RECOMP_WHEEL_DEVICE names.
 *
 * The title only knows the Xbox pad, so the wheel becomes one: steering on
 * the left stick's X axis, throttle and brake on the right and left
 * triggers, buttons and the hat on the pad's buttons. It is merged on top of
 * whatever else drives port 0, as the keyboard is.
 *
 * Steering is made linear. The game reads the stick through a 24% dead zone
 * that reaches full scale at 95%, then maps that through two slopes: the
 * first 60% of it turns the wheels to 20% of full lock, the rest covers the
 * other 80% (measured in RalliSport Challenge 2 by sweeping the stick and
 * reading the input at 0x12DCC94 and the target steering angle, 25 degrees
 * at full lock). Fine for a thumb, but a wheel then does nothing for its
 * first quarter turn and steers twice as hard past the knee. The exact
 * inverse is applied instead, so wheel angle maps linearly to steering
 * angle (RECOMP_WHEEL_LINEAR=0 sends the raw position).
 *
 * Force feedback is a centring spring and a damper, plus the title's pad
 * rumble played as a vibration. Where a driver has no spring (Linux's G29
 * driver only has a constant force and autocentre) the autocentre stands
 * in, and the rumble becomes a constant force that flips direction.
 *
 * Settings (environment):
 *   RECOMP_WHEEL=0             off
 *   RECOMP_WHEEL_DEVICE=text   the joystick whose name contains text
 *   RECOMP_WHEEL_AXES=s,t,b    steering, throttle, brake axis numbers; the
 *                              same number for throttle and brake is a
 *                              combined pedal axis (throttle below centre)
 *   RECOMP_WHEEL_LOCK=n        percent of the wheel's travel that reaches
 *                              full lock (default 100)
 *   RECOMP_WHEEL_DEADZONE=n    percent of travel ignored at the centre
 *   RECOMP_WHEEL_LINEAR=0      no inverse of the game's steering curve
 *   RECOMP_WHEEL_BUTTONS=...   a=0,b=2+4,... (pad input = wheel buttons);
 *                              names a b x y black white start back ls rs
 *                              up down left right
 *   RECOMP_WHEEL_SPRING=n      centring spring strength, percent (30)
 *   RECOMP_WHEEL_DAMPER=n      damper strength, percent (10)
 *   RECOMP_WHEEL_RUMBLE=n      rumble strength, percent (50)
 *   RECOMP_WHEEL_FFB=n         overall force feedback gain, percent (100)
 *   RECOMP_WHEEL_LOG=1         print the wheel's axes and buttons as they
 *                              move, to find the numbers above
 *   RECOMP_WHEEL_VIRTUAL=file  a scripted wheel for testing: lines of
 *                              "ms steer throttle brake buttons-hex hat"
 *
 * RECOMP_RUMBLE=0 also silences the wheel's rumble.
 */
#include "xinput_xbox.h"
#include "wheel_sdl.h"

#include <SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static SRWLOCK g_lock = SRWLOCK_INIT;
#define LOCK()   AcquireSRWLockExclusive(&g_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&g_lock)
#else
#include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

/* ---- settings ------------------------------------------------------------ */

enum {
    WHB_A, WHB_B, WHB_X, WHB_Y, WHB_BLACK, WHB_WHITE,
    WHB_START, WHB_BACK, WHB_LS, WHB_RS, WHB_UP, WHB_DOWN, WHB_LEFT, WHB_RIGHT,
    WHB_COUNT
};
static const char *const k_wb_name[WHB_COUNT] = {
    "a", "b", "x", "y", "black", "white",
    "start", "back", "ls", "rs", "up", "down", "left", "right"
};
#define WHB_SLOTS 4

/* Logitech's layout (G29/G920/G923), which most wheels copy: cross, square,
 * circle, triangle, then the right and left paddles. A is the e-brake and B
 * and X are gear up and down in the title's default controls, so the
 * paddles shift. */
static const char k_default_buttons[] =
    "a=0,x=1,b=2,y=3,b=4,x=5,black=6,white=7,back=8,start=9,rs=10,ls=11";

/* Where a G29 puts its pedals: DirectInput's Y and Rz on Windows, evdev's
 * Z and RZ on Linux (Y is the clutch there). */
#ifdef _WIN32
static const char k_default_axes[] = "0,1,2";
#else
static const char k_default_axes[] = "0,2,3";
#endif

static struct {
    int loaded, enabled, log, linear;
    char device[96];
    int ax_steer, ax_thr, ax_brk;
    float lock, dz;
    int spring, damper, rumble, gain;
    signed char btn[WHB_COUNT][WHB_SLOTS];
} C;

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *v = getenv(name);
    int n;
    if (!v || !*v)
        return def;
    n = atoi(v);
    return n < lo ? lo : n > hi ? hi : n;
}

static void parse_buttons(const char *s)
{
    while (*s) {
        char name[16];
        int n = 0, w;
        while (*s == ',' || *s == ' ')
            s++;
        while (*s && *s != '=' && *s != ',' && n < (int)sizeof name - 1)
            name[n++] = (char)tolower((unsigned char)*s++);
        name[n] = 0;
        if (*s != '=') {
            while (*s && *s != ',') s++;
            continue;
        }
        s++;
        for (w = 0; w < WHB_COUNT && strcmp(name, k_wb_name[w]); w++)
            ;
        for (;;) {
            char *end;
            long b = strtol(s, &end, 10);
            if (end == s)
                break;
            if (w < WHB_COUNT && b >= 0 && b < 128) {
                int k;
                for (k = 0; k < WHB_SLOTS && C.btn[w][k] >= 0; k++)
                    ;
                if (k < WHB_SLOTS)
                    C.btn[w][k] = (signed char)b;
            }
            s = end;
            if (*s != '+')
                break;
            s++;
        }
        while (*s && *s != ',')
            s++;
    }
}

static void load_settings(void)
{
    const char *v;
    if (C.loaded)
        return;
    v = getenv("RECOMP_WHEEL");
    C.enabled = !(v && strcmp(v, "0") == 0);
    C.log = env_int("RECOMP_WHEEL_LOG", 0, 0, 1);
    C.linear = env_int("RECOMP_WHEEL_LINEAR", 1, 0, 1);
    v = getenv("RECOMP_WHEEL_DEVICE");
    snprintf(C.device, sizeof C.device, "%s", v ? v : "");
    v = getenv("RECOMP_WHEEL_AXES");
    if (!v || sscanf(v, "%d,%d,%d", &C.ax_steer, &C.ax_thr, &C.ax_brk) != 3)
        sscanf(k_default_axes, "%d,%d,%d", &C.ax_steer, &C.ax_thr, &C.ax_brk);
    C.lock = (float)env_int("RECOMP_WHEEL_LOCK", 100, 5, 100) / 100.0f;
    C.dz = (float)env_int("RECOMP_WHEEL_DEADZONE", 0, 0, 50) / 100.0f;
    C.spring = env_int("RECOMP_WHEEL_SPRING", 30, 0, 100);
    C.damper = env_int("RECOMP_WHEEL_DAMPER", 10, 0, 100);
    C.rumble = env_int("RECOMP_WHEEL_RUMBLE", 50, 0, 100);
    v = getenv("RECOMP_RUMBLE");
    if (v && strcmp(v, "0") == 0)
        C.rumble = 0;
    C.gain = env_int("RECOMP_WHEEL_FFB", 100, 0, 100);
    memset(C.btn, -1, sizeof C.btn);
    v = getenv("RECOMP_WHEEL_BUTTONS");
    parse_buttons(v && *v ? v : k_default_buttons);
    C.loaded = 1;
}

/* ---- the device ----------------------------------------------------------- */

static struct {
    SDL_Joystick *js;
    SDL_JoystickID id;
    SDL_Haptic *hap;
    unsigned caps;
    int spring_id, damper_id, rumble_id;
    int rumble_on, rumble_sign;
    Uint32 rumble_at;
    int naxes, nbuttons, nhats;
    Sint16 rest[64];            /* pedals: the end each rests at, 0 unknown */
    Sint16 logged_axis[64];
    Uint8 logged_button[128];
} W = { NULL, -1, NULL, 0, -1, -1, -1, 0, 1, 0, 0, 0, 0, { 0 }, { 0 }, { 0 } };

static XBOX_INPUT_STATE g_snap;
static int   g_connected;
static WORD  g_rumble[2];
static DWORD g_packet;

static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!hay || !n)
        return 0;
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < n && hay[i]
             && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]); i++)
            ;
        if (i == n)
            return 1;
    }
    return 0;
}

int xbox_WheelIsWheel(int device_index)
{
    const char *name;
    load_settings();
    if (!C.enabled)
        return 0;
    name = SDL_JoystickNameForIndex(device_index);
    if (C.device[0])
        return contains_ci(name, C.device);
    return SDL_JoystickGetDeviceType(device_index) == SDL_JOYSTICK_TYPE_WHEEL
        || contains_ci(name, "wheel");
}

static void ffb_condition(int type, int strength, int *id)
{
    SDL_HapticEffect e;
    int i;
    memset(&e, 0, sizeof e);
    e.type = (Uint16)type;
    e.condition.type = (Uint16)type;
    e.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    e.condition.direction.dir[0] = 1;
    e.condition.length = SDL_HAPTIC_INFINITY;
    for (i = 0; i < 1; i++) {
        e.condition.right_sat[i] = 0xFFFF;
        e.condition.left_sat[i] = 0xFFFF;
        e.condition.right_coeff[i] = (Sint16)(strength * 0x7FFF / 100);
        e.condition.left_coeff[i] = (Sint16)(strength * 0x7FFF / 100);
    }
    *id = SDL_HapticNewEffect(W.hap, &e);
    if (*id >= 0)
        SDL_HapticRunEffect(W.hap, *id, 1);
}

static void ffb_open(void)
{
    W.hap = SDL_HapticOpenFromJoystick(W.js);
    if (!W.hap) {
        fprintf(stderr, "  [WHEEL] no force feedback: %s\n", SDL_GetError());
        return;
    }
    W.caps = SDL_HapticQuery(W.hap);
    if (W.caps & SDL_HAPTIC_GAIN)
        SDL_HapticSetGain(W.hap, C.gain);
    if (C.spring && (W.caps & SDL_HAPTIC_SPRING)) {
        if (W.caps & SDL_HAPTIC_AUTOCENTER)
            SDL_HapticSetAutocenter(W.hap, 0);
        ffb_condition(SDL_HAPTIC_SPRING, C.spring, &W.spring_id);
    } else if (W.caps & SDL_HAPTIC_AUTOCENTER) {
        SDL_HapticSetAutocenter(W.hap, C.spring);
    }
    if (C.damper && (W.caps & SDL_HAPTIC_DAMPER))
        ffb_condition(SDL_HAPTIC_DAMPER, C.damper, &W.damper_id);
    if (C.rumble && (W.caps & (SDL_HAPTIC_SINE | SDL_HAPTIC_CONSTANT))) {
        SDL_HapticEffect e;
        memset(&e, 0, sizeof e);
        if (W.caps & SDL_HAPTIC_SINE) {
            e.type = SDL_HAPTIC_SINE;
            e.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
            e.periodic.direction.dir[0] = 1;
            e.periodic.length = SDL_HAPTIC_INFINITY;
            e.periodic.period = 50;
        } else {
            e.type = SDL_HAPTIC_CONSTANT;
            e.constant.direction.type = SDL_HAPTIC_CARTESIAN;
            e.constant.direction.dir[0] = 1;
            e.constant.length = SDL_HAPTIC_INFINITY;
        }
        W.rumble_id = SDL_HapticNewEffect(W.hap, &e);
    }
    fprintf(stderr, "  [WHEEL] force feedback:%s%s%s%s%s\n",
            W.spring_id >= 0 ? " spring" : "",
            W.spring_id < 0 && (W.caps & SDL_HAPTIC_AUTOCENTER) ? " autocentre" : "",
            W.damper_id >= 0 ? " damper" : "",
            W.rumble_id >= 0 ? ((W.caps & SDL_HAPTIC_SINE) ? " rumble" : " rumble(constant)") : "",
            (W.caps & SDL_HAPTIC_GAIN) ? " gain" : "");
}

/* The title's two motors as one vibration: the stronger of the two, slower
 * when the big motor leads. Rate-limited -- a driver queues every update. */
static void ffb_rumble(WORD lo, WORD hi)
{
    SDL_HapticEffect e;
    Uint32 now = SDL_GetTicks();
    int mag = (int)((lo > hi ? lo : hi) * (long)C.rumble / 100 / 2);
    if (!W.hap || W.rumble_id < 0)
        return;
    if (now - W.rumble_at < ((W.caps & SDL_HAPTIC_SINE) ? 50u : 25u))
        return;
    W.rumble_at = now;
    if (!mag) {
        if (W.rumble_on)
            SDL_HapticStopEffect(W.hap, W.rumble_id);
        W.rumble_on = 0;
        return;
    }
    memset(&e, 0, sizeof e);
    if (W.caps & SDL_HAPTIC_SINE) {
        e.type = SDL_HAPTIC_SINE;
        e.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
        e.periodic.direction.dir[0] = 1;
        e.periodic.length = SDL_HAPTIC_INFINITY;
        e.periodic.period = lo >= hi ? 80 : 30;
        e.periodic.magnitude = (Sint16)mag;
    } else {
        /* No periodic effects: shake by turning a constant force around. */
        W.rumble_sign = -W.rumble_sign;
        e.type = SDL_HAPTIC_CONSTANT;
        e.constant.direction.type = SDL_HAPTIC_CARTESIAN;
        e.constant.direction.dir[0] = 1;
        e.constant.length = SDL_HAPTIC_INFINITY;
        e.constant.level = (Sint16)(mag * W.rumble_sign);
    }
    SDL_HapticUpdateEffect(W.hap, W.rumble_id, &e);
    if (!W.rumble_on)
        SDL_HapticRunEffect(W.hap, W.rumble_id, 1);
    W.rumble_on = 1;
}

static void wheel_open(int index)
{
    W.js = SDL_JoystickOpen(index);
    if (!W.js) {
        fprintf(stderr, "  [WHEEL] cannot open %s: %s\n",
                SDL_JoystickNameForIndex(index), SDL_GetError());
        return;
    }
    W.id = SDL_JoystickInstanceID(W.js);
    W.naxes = SDL_JoystickNumAxes(W.js);
    W.nbuttons = SDL_JoystickNumButtons(W.js);
    W.nhats = SDL_JoystickNumHats(W.js);
    memset(W.rest, 0, sizeof W.rest);
    W.spring_id = W.damper_id = W.rumble_id = -1;
    W.rumble_on = 0;
    W.rumble_sign = 1;
    fprintf(stderr, "  [WHEEL] %s: %d axes, %d buttons, %d hats; steering axis %d, "
            "throttle %d, brake %d\n", SDL_JoystickName(W.js) ? SDL_JoystickName(W.js) : "(unnamed)",
            W.naxes, W.nbuttons, W.nhats, C.ax_steer, C.ax_thr, C.ax_brk);
    ffb_open();
}

static void wheel_close(void)
{
    if (W.hap)
        SDL_HapticClose(W.hap);
    if (W.js)
        SDL_JoystickClose(W.js);
    W.hap = NULL;
    W.js = NULL;
    W.id = -1;
    LOCK();
    g_connected = 0;
    memset(&g_snap, 0, sizeof g_snap);
    UNLOCK();
    fprintf(stderr, "  [WHEEL] removed\n");
}

/* ---- reading it ------------------------------------------------------------ */

static Sint16 axis(int a)
{
    return a >= 0 && a < W.naxes ? SDL_JoystickGetAxis(W.js, a) : 0;
}

/* A pedal from 0 (resting) to 1 (floored). Pedals rest at one end of the
 * axis, which end depends on the wheel and the OS; the first reading away
 * from the centre (the device's first report, pedals up) says which. */
static float pedal(int a)
{
    Sint16 v = axis(a);
    if (a < 0 || a >= 64)
        return 0.0f;
    if (!W.rest[a]) {
        if (v > -1000 && v < 1000)
            return 0.0f;
        W.rest[a] = v < 0 ? -32768 : 32767;
        if (C.log)
            fprintf(stderr, "  [WHEEL] axis %d rests at %s\n", a, v < 0 ? "minimum" : "maximum");
    }
    return fabsf((float)v - (float)W.rest[a]) / 65535.0f;
}

/* The stick value the game turns into this much of full lock (0..1, signed).
 *
 * The game, for stick s: x = (s + 0.5) / 32767.5, u = (|x| - 0.24) / 0.71
 * clamped to 0..1, then lock = 0.05 + 0.25 u up to u = 0.6 and 2u - 1
 * past it. Below 2.5% of lock the stick is left in the dead zone; up to 5%
 * it gives the smallest angle the game has (5%). */
static SHORT steer_stick(float t)
{
    float a = fabsf(t), u, x, s;
    if (!C.linear) {
        s = t * 32767.0f;
        return (SHORT)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : s);
    }
    if (a < 0.025f)
        return 0;
    if (a > 1.0f)
        a = 1.0f;
    u = a <= 0.05f ? 0.0f : a <= 0.2f ? (a - 0.05f) * 4.0f : (a + 1.0f) * 0.5f;
    x = 0.24f + 0.71f * u;
    if (t > 0.0f) {
        s = ceilf(x * 32767.5f - 0.5f + 1e-3f);
        return (SHORT)(s > 32767.0f ? 32767 : s);
    }
    s = floorf(-x * 32767.5f - 0.5f - 1e-3f);
    return (SHORT)(s < -32768.0f ? -32768 : s);
}

static void wheel_state(XBOX_INPUT_STATE *st)
{
    static const WORD digital[WHB_COUNT] = {
        0, 0, 0, 0, 0, 0, XBOX_GAMEPAD_START, XBOX_GAMEPAD_BACK,
        XBOX_GAMEPAD_LEFT_THUMB, XBOX_GAMEPAD_RIGHT_THUMB, XBOX_GAMEPAD_DPAD_UP,
        XBOX_GAMEPAD_DPAD_DOWN, XBOX_GAMEPAD_DPAD_LEFT, XBOX_GAMEPAD_DPAD_RIGHT
    };
    static const int analog[WHB_COUNT] = {
        XBOX_BUTTON_A, XBOX_BUTTON_B, XBOX_BUTTON_X, XBOX_BUTTON_Y,
        XBOX_BUTTON_BLACK, XBOX_BUTTON_WHITE, -1, -1, -1, -1, -1, -1, -1, -1
    };
    float steer, thr, brk;
    int w, k;
    Sint16 sv = axis(C.ax_steer);

    memset(st, 0, sizeof *st);
    steer = sv >= 0 ? sv / 32767.0f : sv / 32768.0f;
    {
        float a = fabsf(steer) / C.lock;
        a = a <= C.dz ? 0.0f : (a - C.dz) / (1.0f - C.dz);
        steer = steer < 0.0f ? -a : a;
    }
    st->Gamepad.sThumbLX = steer_stick(steer);

    if (C.ax_thr == C.ax_brk) {
        /* One axis for both, centred: throttle one way, brake the other. */
        float v = axis(C.ax_thr) / 32768.0f;
        thr = v < 0.0f ? -v : 0.0f;
        brk = v > 0.0f ? v : 0.0f;
    } else {
        thr = pedal(C.ax_thr);
        brk = pedal(C.ax_brk);
    }
    st->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = (BYTE)(thr > 1.0f ? 255 : thr * 255.0f + 0.5f);
    st->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = (BYTE)(brk > 1.0f ? 255 : brk * 255.0f + 0.5f);

    for (w = 0; w < WHB_COUNT; w++) {
        int down = 0;
        for (k = 0; k < WHB_SLOTS && C.btn[w][k] >= 0; k++)
            if (C.btn[w][k] < W.nbuttons && SDL_JoystickGetButton(W.js, C.btn[w][k]))
                down = 1;
        if (!down)
            continue;
        if (analog[w] >= 0)
            st->Gamepad.bAnalogButtons[analog[w]] = 255;
        else
            st->Gamepad.wButtons |= digital[w];
    }
    if (W.nhats > 0) {
        Uint8 h = SDL_JoystickGetHat(W.js, 0);
        if (h & SDL_HAT_UP)    st->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_UP;
        if (h & SDL_HAT_DOWN)  st->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
        if (h & SDL_HAT_LEFT)  st->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_LEFT;
        if (h & SDL_HAT_RIGHT) st->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_RIGHT;
    }
}

static void wheel_log(void)
{
    int i;
    for (i = 0; i < W.naxes && i < 64; i++) {
        Sint16 v = SDL_JoystickGetAxis(W.js, i);
        if (abs(v - W.logged_axis[i]) > 3000) {
            fprintf(stderr, "  [WHEEL] axis %d = %d\n", i, v);
            W.logged_axis[i] = v;
        }
    }
    for (i = 0; i < W.nbuttons && i < 128; i++) {
        Uint8 b = SDL_JoystickGetButton(W.js, i);
        if (b != W.logged_button[i]) {
            fprintf(stderr, "  [WHEEL] button %d %s\n", i, b ? "down" : "up");
            W.logged_button[i] = b;
        }
    }
}

/* ---- a scripted wheel, for testing without one ------------------------------ */

#if SDL_VERSION_ATLEAST(2, 0, 14)
static struct {
    int on;
    SDL_Joystick *js;
    Uint32 t0;
    int n, cur;
    struct { Uint32 ms; Sint16 steer, thr, brk; Uint32 buttons; Uint8 hat; } *ev;
} V;

static void virtual_start(void)
{
    const char *path = getenv("RECOMP_WHEEL_VIRTUAL");
    FILE *f;
    char line[160];
    int cap = 0, idx;
    if (!path || !*path || !(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof line, f)) {
        unsigned ms, buttons = 0, hat = 0;
        int s, t, b;
        if (sscanf(line, "%u %d %d %d %x %u", &ms, &s, &t, &b, &buttons, &hat) < 4)
            continue;
        if (V.n == cap) {
            void *g = realloc(V.ev, sizeof *V.ev * (size_t)(cap = cap ? cap * 2 : 64));
            if (!g)
                break;
            V.ev = g;
        }
        V.ev[V.n].ms = ms;
        V.ev[V.n].steer = (Sint16)s;
        V.ev[V.n].thr = (Sint16)t;
        V.ev[V.n].brk = (Sint16)b;
        V.ev[V.n].buttons = buttons;
        V.ev[V.n].hat = (Uint8)hat;
        V.n++;
    }
    fclose(f);
    idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_WHEEL, 4, 16, 1);
    if (idx < 0 || !(V.js = SDL_JoystickOpen(idx))) {
        fprintf(stderr, "  [WHEEL] virtual wheel failed: %s\n", SDL_GetError());
        return;
    }
    V.on = 1;
    V.cur = -1;
    V.t0 = SDL_GetTicks();
    fprintf(stderr, "  [WHEEL] virtual wheel: %d step(s) from %s\n", V.n, path);
}

static void virtual_tick(void)
{
    Uint32 t;
    int i, c = V.cur;
    if (!V.on)
        return;
    t = SDL_GetTicks() - V.t0;
    while (c + 1 < V.n && V.ev[c + 1].ms <= t)
        c++;
    if (c == V.cur || c < 0)
        return;
    V.cur = c;
    SDL_JoystickSetVirtualAxis(V.js, 0, V.ev[c].steer);
    SDL_JoystickSetVirtualAxis(V.js, C.ax_thr < 4 ? C.ax_thr : 1, V.ev[c].thr);
    SDL_JoystickSetVirtualAxis(V.js, C.ax_brk < 4 ? C.ax_brk : 2, V.ev[c].brk);
    for (i = 0; i < 16; i++)
        SDL_JoystickSetVirtualButton(V.js, i, (Uint8)((V.ev[c].buttons >> i) & 1));
    SDL_JoystickSetVirtualHat(V.js, 0, V.ev[c].hat);
}
#else
static void virtual_start(void) { }
static void virtual_tick(void) { }
#endif

/* ---- the SDL side: events and polling, on the thread that owns SDL ------- */

void xbox_WheelSdlEvent(const SDL_Event *ev)
{
    load_settings();
    if (!C.enabled)
        return;
    if (ev->type == SDL_JOYDEVICEADDED) {
        if (!W.js && xbox_WheelIsWheel(ev->jdevice.which))
            wheel_open(ev->jdevice.which);
    } else if (ev->type == SDL_JOYDEVICEREMOVED) {
        if (W.js && ev->jdevice.which == W.id)
            wheel_close();
    }
}

void xbox_WheelSdlPoll(void)
{
    XBOX_INPUT_STATE st;
    WORD lo, hi;
    load_settings();
    if (!C.enabled)
        return;
    virtual_tick();
    if (!W.js)
        return;
    if (C.log)
        wheel_log();
    wheel_state(&st);
    LOCK();
    g_snap = st;
    g_connected = 1;
    lo = g_rumble[0];
    hi = g_rumble[1];
    UNLOCK();
    ffb_rumble(lo, hi);
}

void xbox_WheelSdlInit(void)
{
    load_settings();
    if (!C.enabled)
        return;
    if (!SDL_WasInit(SDL_INIT_JOYSTICK) && SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "  [WHEEL] SDL joystick init failed: %s\n", SDL_GetError());
        return;
    }
    if (!SDL_WasInit(SDL_INIT_HAPTIC) && SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0)
        fprintf(stderr, "  [WHEEL] SDL haptic init failed: %s\n", SDL_GetError());
    virtual_start();
}

/* ---- the title's side, from any thread ------------------------------------ */

int xbox_WheelMerge(DWORD port, XBOX_INPUT_STATE *st)
{
    XBOX_INPUT_STATE w;
    int i, have;
    if (port != 0)
        return 0;
    LOCK();
    have = g_connected;
    w = g_snap;
    UNLOCK();
    if (!have)
        return 0;
    st->Gamepad.wButtons |= w.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (w.Gamepad.bAnalogButtons[i] > st->Gamepad.bAnalogButtons[i])
            st->Gamepad.bAnalogButtons[i] = w.Gamepad.bAnalogButtons[i];
    if (w.Gamepad.sThumbLX)
        st->Gamepad.sThumbLX = w.Gamepad.sThumbLX;
    /* The title acts on changes of the packet number. */
    st->dwPacketNumber = ++g_packet;
    return 1;
}

int xbox_WheelConnected(DWORD port)
{
    int have;
    if (port != 0)
        return 0;
    LOCK();
    have = g_connected;
    UNLOCK();
    return have;
}

void xbox_WheelRumble(DWORD port, WORD lo, WORD hi)
{
    if (port != 0)
        return;
    LOCK();
    g_rumble[0] = lo;
    g_rumble[1] = hi;
    UNLOCK();
}

#ifdef _WIN32
/* On Windows the game window is Win32, not SDL, so the wheel gets a thread
 * of its own to own SDL's joystick layer. */
static DWORD WINAPI wheel_thread(LPVOID unused)
{
    (void)unused;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    xbox_WheelSdlInit();
    if (!SDL_WasInit(SDL_INIT_JOYSTICK))
        return 0;
    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            xbox_WheelSdlEvent(&ev);
        xbox_WheelSdlPoll();
        SDL_Delay(4);
    }
    return 0;
}

void xbox_WheelStart(void)
{
    static volatile LONG started;
    load_settings();
    if (!C.enabled || InterlockedExchange(&started, 1))
        return;
    CloseHandle(CreateThread(NULL, 0, wheel_thread, NULL, 0, NULL));
}
#endif
