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
 * in, and the rumble becomes a constant force that flips direction. When
 * the wheel opens it gives a short tug left, then right, so a tester can
 * tell whether force feedback reaches it at all.
 *
 * Testers without a debugger: everything above is written to a log file,
 * rsc2_wheel_log.txt next to the program (or in the settings folder if
 * that is read-only; the previous run's is kept as .prev.txt). It holds the
 * system, every joystick SDL sees and why it was or was not taken, what
 * force feedback the device offers and whether each effect started, the
 * axes and buttons as they move with what the game was sent, and every
 * 20 seconds a summary of each axis's range and the buttons seen.
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
 *   RECOMP_WHEEL_TUG=0         no tug when the wheel opens
 *   RECOMP_WHEEL_LOG=0         no log file
 *   RECOMP_WHEEL_LOGFILE=path  the log file somewhere else
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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static SRWLOCK g_lock = SRWLOCK_INIT;
#define LOCK()   AcquireSRWLockExclusive(&g_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&g_lock)
#else
#include <pthread.h>
#include <sys/stat.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

/* Which test build a log came from. */
#define WHEEL_TEST_BUILD "wheel test 1"

/* ---- the log ------------------------------------------------------------- */

#define LOG_CAP (8L << 20)       /* live lines stop here; summaries go on */

static FILE  *g_log;
static Uint32 g_log_t0;
static long   g_log_bytes;
static int    g_log_capped;

static void vlog_line(int live, const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    fprintf(stderr, "  [WHEEL] %s\n", buf);
    if (!g_log)
        return;
    if (live && g_log_bytes >= LOG_CAP) {
        if (!g_log_capped)
            fprintf(g_log, "(log full: movement no longer logged, summaries continue)\n");
        g_log_capped = 1;
        return;
    }
    g_log_bytes += fprintf(g_log, "%9.3f  %s\n", (SDL_GetTicks() - g_log_t0) / 1000.0, buf);
    fflush(g_log);
}

/* Something that happens once or rarely. */
static void wlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_line(0, fmt, ap);
    va_end(ap);
}

/* Movement: dropped once the log is large. */
static void wlog_live(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_line(1, fmt, ap);
    va_end(ap);
}

static FILE *open_log_at(const char *path)
{
    char prev[1100];
    const char *dot = strrchr(path, '.');
    FILE *f;
    if (dot) {
        snprintf(prev, sizeof prev, "%.*s.prev%s", (int)(dot - path), path, dot);
        remove(prev);
        rename(path, prev);
    }
    f = fopen(path, "w");
    if (f)
        fprintf(stderr, "  [WHEEL] log: %s\n", path);
    return f;
}

static void log_open(void)
{
    const char *v = getenv("RECOMP_WHEEL_LOG"), *p = getenv("RECOMP_WHEEL_LOGFILE");
    char path[1024];
    if (v && strcmp(v, "0") == 0)
        return;
    g_log_t0 = SDL_GetTicks();
    if (p && *p) {
        g_log = open_log_at(p);
        return;
    }
#ifdef _WIN32
    {
        DWORD n = GetModuleFileNameA(NULL, path, sizeof path - 32);
        char *slash;
        if (n && n < sizeof path - 32 && (slash = strrchr(path, '\\'))) {
            strcpy(slash + 1, "rsc2_wheel_log.txt");
            g_log = open_log_at(path);
        }
        if (!g_log && (p = getenv("APPDATA"))) {
            snprintf(path, sizeof path, "%s\\rsc2", p);
            CreateDirectoryA(path, NULL);
            snprintf(path, sizeof path, "%s\\rsc2\\rsc2_wheel_log.txt", p);
            g_log = open_log_at(path);
        }
    }
#else
    g_log = open_log_at("rsc2_wheel_log.txt");
    if (!g_log && (p = getenv("HOME"))) {
        snprintf(path, sizeof path, "%s/.config/rsc2", p);
        mkdir(path, 0755);
        snprintf(path, sizeof path, "%s/.config/rsc2/rsc2_wheel_log.txt", p);
        g_log = open_log_at(path);
    }
#endif
}

static void log_system(void)
{
    SDL_version c, l;
    char when[64];
    time_t now = time(NULL);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    SDL_VERSION(&c);
    SDL_GetVersion(&l);
    wlog("RalliSport Challenge 2 steering wheel log (%s, built %s %s), started %s",
         WHEEL_TEST_BUILD, __DATE__, __TIME__, when);
#ifdef _WIN32
    {
        char name[128] = "?", disp[64] = "", build[32] = "?";
        DWORD n;
        const char *key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        n = sizeof name;
        RegGetValueA(HKEY_LOCAL_MACHINE, key, "ProductName", RRF_RT_REG_SZ, NULL, name, &n);
        n = sizeof disp;
        RegGetValueA(HKEY_LOCAL_MACHINE, key, "DisplayVersion", RRF_RT_REG_SZ, NULL, disp, &n);
        n = sizeof build;
        RegGetValueA(HKEY_LOCAL_MACHINE, key, "CurrentBuild", RRF_RT_REG_SZ, NULL, build, &n);
        /* Windows 11 still calls itself 10 in ProductName; the build says. */
        wlog("system: %s %s, build %s%s", name, disp, build,
             atoi(build) >= 22000 ? " (Windows 11)" : "");
    }
#else
    wlog("system: %s", SDL_GetPlatform());
#endif
    wlog("SDL %d.%d.%d (built against %d.%d.%d)", l.major, l.minor, l.patch,
         c.major, c.minor, c.patch);
}

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
    int loaded, enabled, linear, tug;
    char device[96];
    int ax_steer, ax_thr, ax_brk;
    float lock, dz;
    int spring, damper, rumble, gain;
    signed char btn[WHB_COUNT][WHB_SLOTS];
    char buttons_text[256];
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
    C.linear = env_int("RECOMP_WHEEL_LINEAR", 1, 0, 1);
    C.tug = env_int("RECOMP_WHEEL_TUG", 1, 0, 1);
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
    snprintf(C.buttons_text, sizeof C.buttons_text, "%s", v && *v ? v : k_default_buttons);
    parse_buttons(C.buttons_text);
    C.loaded = 1;
}

static void log_settings(void)
{
    wlog("settings: device \"%s\", axes steer %d throttle %d brake %d, lock %d%%, "
         "deadzone %d%%, linear %s, spring %d%%, damper %d%%, rumble %d%%, gain %d%%",
         C.device[0] ? C.device : "(auto)", C.ax_steer, C.ax_thr, C.ax_brk,
         (int)(C.lock * 100.0f + 0.5f), (int)(C.dz * 100.0f + 0.5f), C.linear ? "on" : "off",
         C.spring, C.damper, C.rumble, C.gain);
    wlog("buttons: %s (hat = d-pad)", C.buttons_text);
}

/* ---- the device ----------------------------------------------------------- */

#define MAX_AXES 16
#define MAX_BUTTONS 64

static struct {
    SDL_Joystick *js;
    SDL_JoystickID id;
    SDL_Haptic *hap;
    unsigned caps;
    int spring_id, damper_id, rumble_id;
    int rumble_on, rumble_sign;
    Uint32 rumble_at;
    int naxes, nbuttons, nhats;
    Sint16 rest[MAX_AXES];          /* pedals: the end each rests at, 0 unknown */
    /* for the log */
    Sint16 logged_axis[MAX_AXES];
    Uint32 axes_logged_at;
    Uint8 logged_button[MAX_BUTTONS];
    Uint8 logged_hat;
    Sint16 amin[MAX_AXES], amax[MAX_AXES];
    Uint64 buttons_seen;
    Uint8 hats_seen;
    int ffb_errors;
    Uint32 summary_at;
    WORD logged_rumble[2];
    Uint32 rumble_logged_at;
    unsigned long rumble_updates;
} W;

static XBOX_INPUT_STATE g_snap;
static int   g_connected;
static WORD  g_rumble[2];
static DWORD g_packet;
static volatile long g_reads;       /* the title's reads of the wheel */

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

/* Why device_index is (or is not) the wheel, or NULL when it is not. */
static const char *wheel_reason(int device_index)
{
    const char *name = SDL_JoystickNameForIndex(device_index);
    if (C.device[0])
        return contains_ci(name, C.device) ? "name matches RECOMP_WHEEL_DEVICE" : NULL;
    if (SDL_JoystickGetDeviceType(device_index) == SDL_JOYSTICK_TYPE_WHEEL)
        return "SDL knows it as a wheel";
    if (contains_ci(name, "wheel"))
        return "\"wheel\" in its name";
    return NULL;
}

int xbox_WheelIsWheel(int device_index)
{
    load_settings();
    return C.enabled && wheel_reason(device_index) != NULL;
}

static const char *type_name(SDL_JoystickType t)
{
    switch (t) {
    case SDL_JOYSTICK_TYPE_GAMECONTROLLER: return "gamepad";
    case SDL_JOYSTICK_TYPE_WHEEL:          return "wheel";
    case SDL_JOYSTICK_TYPE_ARCADE_STICK:   return "arcade stick";
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK:   return "flight stick";
    case SDL_JOYSTICK_TYPE_DANCE_PAD:      return "dance pad";
    case SDL_JOYSTICK_TYPE_GUITAR:         return "guitar";
    case SDL_JOYSTICK_TYPE_DRUM_KIT:       return "drum kit";
    case SDL_JOYSTICK_TYPE_ARCADE_PAD:     return "arcade pad";
    case SDL_JOYSTICK_TYPE_THROTTLE:       return "throttle";
    default:                               return "unknown";
    }
}

static void log_device(int i, const char *what)
{
    char guid[64];
    const char *why = wheel_reason(i);
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
    wlog("%s %d: \"%s\", USB %04x:%04x v%04x, type %s, %s, guid %s -> %s%s",
         what, i, SDL_JoystickNameForIndex(i) ? SDL_JoystickNameForIndex(i) : "(unnamed)",
         SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i),
         SDL_JoystickGetDeviceProductVersion(i),
         type_name(SDL_JoystickGetDeviceType(i)),
         SDL_IsGameController(i) ? "has a gamepad mapping" : "no gamepad mapping",
         guid, why ? "wheel: " : "not a wheel", why ? why : "");
#if SDL_VERSION_ATLEAST(2, 24, 0)
    if (SDL_JoystickPathForIndex(i))
        wlog("    path %s", SDL_JoystickPathForIndex(i));
#endif
}

static void log_haptic_caps(unsigned q)
{
    static const struct { unsigned bit; const char *name; } k[] = {
        { SDL_HAPTIC_CONSTANT, "constant" }, { SDL_HAPTIC_SINE, "sine" },
        { SDL_HAPTIC_LEFTRIGHT, "leftright" }, { SDL_HAPTIC_TRIANGLE, "triangle" },
        { SDL_HAPTIC_SAWTOOTHUP, "sawtoothup" }, { SDL_HAPTIC_SAWTOOTHDOWN, "sawtoothdown" },
        { SDL_HAPTIC_RAMP, "ramp" }, { SDL_HAPTIC_SPRING, "spring" },
        { SDL_HAPTIC_DAMPER, "damper" }, { SDL_HAPTIC_INERTIA, "inertia" },
        { SDL_HAPTIC_FRICTION, "friction" }, { SDL_HAPTIC_CUSTOM, "custom" },
        { SDL_HAPTIC_GAIN, "gain" }, { SDL_HAPTIC_AUTOCENTER, "autocenter" },
        { SDL_HAPTIC_STATUS, "status" }, { SDL_HAPTIC_PAUSE, "pause" },
    };
    char buf[400] = "";
    size_t i;
    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (q & k[i].bit) {
            strncat(buf, " ", sizeof buf - strlen(buf) - 1);
            strncat(buf, k[i].name, sizeof buf - strlen(buf) - 1);
        }
    wlog("force feedback offers:%s", buf[0] ? buf : " nothing");
}

static int new_effect(SDL_HapticEffect *e, const char *what, int run)
{
    int id = SDL_HapticNewEffect(W.hap, e);
    if (id < 0) {
        wlog("force feedback: %s could not be created: %s", what, SDL_GetError());
        W.ffb_errors++;
        return -1;
    }
    if (run && SDL_HapticRunEffect(W.hap, id, 1) < 0) {
        wlog("force feedback: %s could not start: %s", what, SDL_GetError());
        W.ffb_errors++;
    } else {
        wlog("force feedback: %s %s", what, run ? "running" : "ready");
    }
    return id;
}

static int ffb_condition(int type, int strength, const char *what)
{
    SDL_HapticEffect e;
    memset(&e, 0, sizeof e);
    e.type = (Uint16)type;
    e.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    e.condition.direction.dir[0] = 1;
    e.condition.length = SDL_HAPTIC_INFINITY;
    e.condition.right_sat[0] = 0xFFFF;
    e.condition.left_sat[0] = 0xFFFF;
    e.condition.right_coeff[0] = (Sint16)(strength * 0x7FFF / 100);
    e.condition.left_coeff[0] = (Sint16)(strength * 0x7FFF / 100);
    return new_effect(&e, what, 1);
}

/* A short pull one way, then the other: does force reach the wheel at all,
 * and which way is which. Blocks the input thread for under a second, once,
 * as the wheel is opened. */
static void ffb_tug(void)
{
    SDL_HapticEffect e;
    int id, dir;
    if (!C.tug || !(W.caps & SDL_HAPTIC_CONSTANT))
        return;
    for (dir = -1; dir <= 1; dir += 2) {
        memset(&e, 0, sizeof e);
        e.type = SDL_HAPTIC_CONSTANT;
        e.constant.direction.type = SDL_HAPTIC_CARTESIAN;
        e.constant.direction.dir[0] = 1;
        e.constant.length = 300;
        e.constant.level = (Sint16)(dir * 0x3000);
        id = SDL_HapticNewEffect(W.hap, &e);
        if (id < 0 || SDL_HapticRunEffect(W.hap, id, 1) < 0) {
            wlog("force feedback: test tug %s failed: %s", dir < 0 ? "one way" : "the other way",
                 SDL_GetError());
            W.ffb_errors++;
            if (id >= 0)
                SDL_HapticDestroyEffect(W.hap, id);
            return;
        }
        SDL_Delay(400);
        SDL_HapticDestroyEffect(W.hap, id);
    }
    wlog("force feedback: test tug played (level -0x3000 then +0x3000, 300 ms each)");
}

static void ffb_open(void)
{
    int i, n = SDL_NumHaptics();
    wlog("force feedback: %d haptic device(s) in SDL; the wheel %s one",
         n, SDL_JoystickIsHaptic(W.js) == 1 ? "is" : "is not");
    for (i = 0; i < n; i++)
        wlog("    haptic %d: \"%s\"", i, SDL_HapticName(i) ? SDL_HapticName(i) : "?");
    W.hap = SDL_HapticOpenFromJoystick(W.js);
    if (!W.hap) {
        wlog("force feedback: none (%s)", SDL_GetError());
        return;
    }
    W.caps = SDL_HapticQuery(W.hap);
    log_haptic_caps(W.caps);
    wlog("force feedback: %d effect slot(s), %d at once, %d axis/axes",
         SDL_HapticNumEffects(W.hap), SDL_HapticNumEffectsPlaying(W.hap), SDL_HapticNumAxes(W.hap));
    if (W.caps & SDL_HAPTIC_GAIN) {
        if (SDL_HapticSetGain(W.hap, C.gain) < 0)
            wlog("force feedback: gain not set: %s", SDL_GetError());
    }
    if (W.caps & SDL_HAPTIC_AUTOCENTER)
        SDL_HapticSetAutocenter(W.hap, 0);
    ffb_tug();
    if (C.spring && (W.caps & SDL_HAPTIC_SPRING)) {
        W.spring_id = ffb_condition(SDL_HAPTIC_SPRING, C.spring, "centring spring");
    } else if (C.spring && (W.caps & SDL_HAPTIC_AUTOCENTER)) {
        if (SDL_HapticSetAutocenter(W.hap, C.spring) < 0)
            wlog("force feedback: autocentre not set: %s", SDL_GetError());
        else
            wlog("force feedback: autocentre at %d%% in place of a spring", C.spring);
    }
    if (C.damper && (W.caps & SDL_HAPTIC_DAMPER))
        W.damper_id = ffb_condition(SDL_HAPTIC_DAMPER, C.damper, "damper");
    if (C.rumble && (W.caps & (SDL_HAPTIC_SINE | SDL_HAPTIC_CONSTANT))) {
        SDL_HapticEffect e;
        memset(&e, 0, sizeof e);
        if (W.caps & SDL_HAPTIC_SINE) {
            e.type = SDL_HAPTIC_SINE;
            e.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
            e.periodic.direction.dir[0] = 1;
            e.periodic.length = SDL_HAPTIC_INFINITY;
            e.periodic.period = 50;
            W.rumble_id = new_effect(&e, "rumble (sine)", 0);
        } else {
            e.type = SDL_HAPTIC_CONSTANT;
            e.constant.direction.type = SDL_HAPTIC_CARTESIAN;
            e.constant.direction.dir[0] = 1;
            e.constant.length = SDL_HAPTIC_INFINITY;
            W.rumble_id = new_effect(&e, "rumble (flipping constant force)", 0);
        }
    }
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
    if (SDL_HapticUpdateEffect(W.hap, W.rumble_id, &e) < 0
            || (!W.rumble_on && SDL_HapticRunEffect(W.hap, W.rumble_id, 1) < 0)) {
        if (W.ffb_errors++ < 10)
            wlog("force feedback: rumble update failed: %s", SDL_GetError());
        return;
    }
    W.rumble_on = 1;
    W.rumble_updates++;
}

static void wheel_open(int index)
{
    int i;
    SDL_Joystick *js = SDL_JoystickOpen(index);
    if (!js) {
        wlog("cannot open device %d: %s", index, SDL_GetError());
        return;
    }
    memset(&W, 0, sizeof W);
    W.js = js;
    W.id = SDL_JoystickInstanceID(js);
    W.spring_id = W.damper_id = W.rumble_id = -1;
    W.rumble_sign = 1;
    W.naxes = SDL_JoystickNumAxes(js);
    W.nbuttons = SDL_JoystickNumButtons(js);
    W.nhats = SDL_JoystickNumHats(js);
    wlog("using \"%s\": %d axes, %d buttons, %d hats",
         SDL_JoystickName(js) ? SDL_JoystickName(js) : "(unnamed)", W.naxes, W.nbuttons, W.nhats);
    for (i = 0; i < W.naxes && i < MAX_AXES; i++) {
        W.amin[i] = W.amax[i] = W.logged_axis[i] = SDL_JoystickGetAxis(js, i);
    }
    if (C.ax_steer >= W.naxes || C.ax_thr >= W.naxes || C.ax_brk >= W.naxes)
        wlog("WARNING: the wheel has %d axes, but steer/throttle/brake are set to %d/%d/%d",
             W.naxes, C.ax_steer, C.ax_thr, C.ax_brk);
    ffb_open();
    W.summary_at = SDL_GetTicks();
}

static void log_summary(const char *why);

static void wheel_close(void)
{
    log_summary("wheel removed");
    if (W.hap)
        SDL_HapticClose(W.hap);
    if (W.js)
        SDL_JoystickClose(W.js);
    W.hap = NULL;
    W.js = NULL;
    LOCK();
    g_connected = 0;
    memset(&g_snap, 0, sizeof g_snap);
    UNLOCK();
    wlog("wheel removed");
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
    if (a < 0 || a >= MAX_AXES)
        return 0.0f;
    if (!W.rest[a]) {
        if (v > -1000 && v < 1000)
            return 0.0f;
        W.rest[a] = v < 0 ? -32768 : 32767;
        wlog("axis %d (%s) rests at %d: taken as released", a,
             a == C.ax_thr ? "throttle" : "brake", v);
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

/* The pad inputs a wheel button is bound to, for the log. */
static void button_targets(int b, char *out, size_t n)
{
    int w, k;
    out[0] = 0;
    for (w = 0; w < WHB_COUNT; w++)
        for (k = 0; k < WHB_SLOTS && C.btn[w][k] >= 0; k++)
            if (C.btn[w][k] == b) {
                if (out[0])
                    strncat(out, "+", n - strlen(out) - 1);
                strncat(out, k_wb_name[w], n - strlen(out) - 1);
            }
    if (!out[0])
        snprintf(out, n, "nothing");
}

static void log_movement(const XBOX_INPUT_STATE *st)
{
    Uint32 now = SDL_GetTicks();
    int i, moved = 0;
    for (i = 0; i < W.naxes && i < MAX_AXES; i++) {
        Sint16 v = SDL_JoystickGetAxis(W.js, i);
        if (v < W.amin[i]) W.amin[i] = v;
        if (v > W.amax[i]) W.amax[i] = v;
        if (abs(v - W.logged_axis[i]) > 1500)
            moved = 1;
    }
    /* Axes: a line when any moved, at most twenty a second. */
    if (moved && now - W.axes_logged_at >= 50) {
        char buf[600];
        size_t n = 0;
        n += (size_t)snprintf(buf + n, sizeof buf - n, "axes");
        for (i = 0; i < W.naxes && i < MAX_AXES && n < sizeof buf; i++) {
            W.logged_axis[i] = SDL_JoystickGetAxis(W.js, i);
            n += (size_t)snprintf(buf + n, sizeof buf - n, " %d:%d", i, W.logged_axis[i]);
        }
        if (n < sizeof buf)
            snprintf(buf + n, sizeof buf - n, "  -> game: steer %d, throttle %u, brake %u",
                     st->Gamepad.sThumbLX, st->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER],
                     st->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER]);
        wlog_live("%s", buf);
        W.axes_logged_at = now;
    }
    for (i = 0; i < W.nbuttons && i < MAX_BUTTONS; i++) {
        Uint8 b = SDL_JoystickGetButton(W.js, i);
        if (b != W.logged_button[i]) {
            char t[64];
            button_targets(i, t, sizeof t);
            if (b)
                W.buttons_seen |= (Uint64)1 << i;
            wlog_live("button %d %s  (-> %s)", i, b ? "pressed" : "released", t);
            W.logged_button[i] = b;
        }
    }
    if (W.nhats > 0) {
        Uint8 h = SDL_JoystickGetHat(W.js, 0);
        if (h != W.logged_hat) {
            W.hats_seen |= h;
            wlog_live("hat %s%s%s%s%s", h ? "" : "centred", (h & SDL_HAT_UP) ? "up " : "",
                      (h & SDL_HAT_DOWN) ? "down " : "", (h & SDL_HAT_LEFT) ? "left " : "",
                      (h & SDL_HAT_RIGHT) ? "right" : "");
            W.logged_hat = h;
        }
    }
}

static void log_rumble(WORD lo, WORD hi)
{
    Uint32 now = SDL_GetTicks();
    if ((lo == W.logged_rumble[0] && hi == W.logged_rumble[1]) || now - W.rumble_logged_at < 250)
        return;
    wlog_live("game rumble: low %u, high %u%s", lo, hi,
              W.rumble_id < 0 ? " (not played: no rumble effect)" : "");
    W.logged_rumble[0] = lo;
    W.logged_rumble[1] = hi;
    W.rumble_logged_at = now;
}

static void log_summary(const char *why)
{
    char buf[700];
    size_t n = 0;
    int i;
    if (!W.js)
        return;
    n += (size_t)snprintf(buf + n, sizeof buf - n, "summary (%s): axis ranges", why);
    for (i = 0; i < W.naxes && i < MAX_AXES && n < sizeof buf; i++)
        n += (size_t)snprintf(buf + n, sizeof buf - n, " %d:[%d..%d]", i, W.amin[i], W.amax[i]);
    wlog("%s", buf);
    n = 0;
    buf[0] = 0;
    for (i = 0; i < W.nbuttons && i < MAX_BUTTONS && n < sizeof buf; i++)
        if (W.buttons_seen & ((Uint64)1 << i))
            n += (size_t)snprintf(buf + n, sizeof buf - n, " %d", i);
    wlog("    buttons pressed so far:%s; hat seen:%s%s%s%s%s", buf[0] ? buf : " none",
         W.hats_seen ? "" : " none", (W.hats_seen & SDL_HAT_UP) ? " up" : "",
         (W.hats_seen & SDL_HAT_DOWN) ? " down" : "", (W.hats_seen & SDL_HAT_LEFT) ? " left" : "",
         (W.hats_seen & SDL_HAT_RIGHT) ? " right" : "");
    wlog("    game reads of the wheel: %ld; rumble updates sent: %lu; force feedback errors: %d",
         g_reads, W.rumble_updates, W.ffb_errors);
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
        wlog("virtual wheel failed: %s", SDL_GetError());
        return;
    }
    V.on = 1;
    V.cur = -1;
    V.t0 = SDL_GetTicks();
    wlog("virtual wheel: %d step(s) from %s", V.n, path);
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
        int i = ev->jdevice.which;
        log_device(i, "device added");
        if (W.js)
            wlog("    (a wheel is already in use; this one is left alone)");
        else if (wheel_reason(i))
            wheel_open(i);
    } else if (ev->type == SDL_JOYDEVICEREMOVED) {
        if (W.js && ev->jdevice.which == W.id)
            wheel_close();
    }
}

void xbox_WheelSdlPoll(void)
{
    XBOX_INPUT_STATE st;
    WORD lo, hi;
    static int reads_noted;
    load_settings();
    if (!C.enabled)
        return;
    virtual_tick();
    if (!W.js)
        return;
    wheel_state(&st);
    LOCK();
    g_snap = st;
    g_connected = 1;
    lo = g_rumble[0];
    hi = g_rumble[1];
    UNLOCK();
    log_movement(&st);
    log_rumble(lo, hi);
    ffb_rumble(lo, hi);
    if (!reads_noted && g_reads > 0) {
        wlog("the game is reading the wheel");
        reads_noted = 1;
    }
    if (SDL_GetTicks() - W.summary_at >= 20000) {
        log_summary("every 20 s");
        W.summary_at = SDL_GetTicks();
    }
}

void xbox_WheelSdlInit(void)
{
    int i, n;
    load_settings();
    if (!C.enabled)
        return;
    log_open();
    log_system();
    log_settings();
    if (!SDL_WasInit(SDL_INIT_JOYSTICK) && SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
        wlog("SDL joystick init failed: %s", SDL_GetError());
        return;
    }
    if (!SDL_WasInit(SDL_INIT_HAPTIC) && SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0)
        wlog("SDL haptic init failed: %s", SDL_GetError());
    n = SDL_NumJoysticks();
    wlog("%d joystick(s) at start (each is listed again as SDL reports it added)", n);
    for (i = 0; i < n; i++)
        log_device(i, "joystick");
    if (n == 0)
        wlog("no joysticks: is the wheel plugged in and (G29) switched to PS3 mode?");
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
    if (have)
        g_reads++;
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
