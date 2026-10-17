/*
 * touch_pad.c -- on-screen touch controls standing in for controller 1.
 *
 * The left 45% of the screen is a floating stick: it centres where a thumb
 * lands and steers by how far it is dragged (TOUCH_STICK_R at full lock),
 * and it is the left stick, so menus answer to it too. The right side has
 * the pedals and buttons below. A finger on a button holds it while it
 * stays on it; sliding to another button moves the press there, so a thumb
 * can rock between throttle and brake.
 *
 * Shown when no controller is connected, or after the screen is touched;
 * hidden again as soon as a real controller is used. RSC2_TOUCH_SHOW=1
 * keeps it on screen (screenshots).
 *
 * Fingers come from the window thread, the state is read from the title's;
 * both go through one lock.
 */
#include "touch_pad.h"
#include "xinput_xbox.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Racing first: throttle under the right thumb, brake beside it, the face
 * buttons above (A selects in menus and is the e-brake in a race, B goes
 * back), camera and car reset at the top right, Start (pause) at the top
 * centre. Colours are the Xbox pad's. */
static const TouchButton k_buttons[] = {
    { 1, 0.17f, 0.80f, 0.13f,  XBOX_BUTTON_RTRIGGER, 0, 0x58C060, "GAS" },
    { 1, 0.45f, 0.84f, 0.11f,  XBOX_BUTTON_LTRIGGER, 0, 0xD05048, "BRK" },
    { 1, 0.30f, 0.56f, 0.065f, XBOX_BUTTON_A,        0, 0x40B040, "A" },
    { 1, 0.19f, 0.45f, 0.065f, XBOX_BUTTON_B,        0, 0xD04040, "B" },
    { 1, 0.41f, 0.45f, 0.065f, XBOX_BUTTON_X,        0, 0x4070D0, "X" },
    { 1, 0.30f, 0.34f, 0.065f, XBOX_BUTTON_Y,        0, 0xD0B030, "Y" },
    { 1, 0.10f, 0.10f, 0.05f,  XBOX_BUTTON_BLACK,    0, 0x808890, "CAM" },
    { 1, 0.23f, 0.10f, 0.05f,  XBOX_BUTTON_WHITE,    0, 0x808890, "RST" },
    { 2, 0.00f, 0.08f, 0.05f,  -1, XBOX_GAMEPAD_START, 0x808890, "II" },
};
#define NB ((int)(sizeof k_buttons / sizeof k_buttons[0]))
#define MAX_FINGERS 10

typedef struct {
    int64_t id;
    int     used;
    int     stick;          /* this finger drives the stick */
    int     button;         /* the button it is on, -1 none */
    float   x, y;           /* window fractions */
} Finger;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static Finger s_f[MAX_FINGERS];
static float  s_aspect = 16.0f / 10.0f;    /* width / height */
static float  s_base[2], s_knob[2];        /* stick, window fractions */
static double s_touch_ms, s_pad_ms;
static int    s_any_pad = -1;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int touchpad_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_TOUCH");
#if defined(__ANDROID__)
        on = !(e && strcmp(e, "0") == 0);
#else
        on = e && strcmp(e, "0") != 0;
#endif
    }
    return on;
}

int touchpad_layout(const TouchButton **buttons)
{
    *buttons = k_buttons;
    return NB;
}

void touchpad_set_view(int w, int h)
{
    if (w > 0 && h > 0) {
        pthread_mutex_lock(&s_lock);
        s_aspect = (float)w / (float)h;
        pthread_mutex_unlock(&s_lock);
    }
}

void touchpad_pad_activity(void)
{
    s_pad_ms = now_ms();
}

/* A button's centre, in window fractions. */
static void button_centre(const TouchButton *b, float *cx, float *cy)
{
    float u = 1.0f / s_aspect;          /* one height unit across */
    *cx = b->anchor == 0 ? b->x * u : b->anchor == 1 ? 1.0f - b->x * u : 0.5f + b->x * u;
    *cy = b->y;
}

/* Which button (x, y) is on, -1 none. A little slack around each, so a
 * thumb that drifts does not drop the press. */
static int hit(float x, float y, float slack)
{
    int i, best = -1;
    float bd = 1e9f;
    for (i = 0; i < NB; i++) {
        float cx, cy, dx, dy, d;
        button_centre(&k_buttons[i], &cx, &cy);
        dx = (x - cx) * s_aspect;       /* in height units */
        dy = y - cy;
        d = sqrtf(dx * dx + dy * dy);
        if (d < k_buttons[i].r * slack && d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* Whether (x, y) is within button i, scaled by `slack`. */
static int on_button(int i, float x, float y, float slack)
{
    float cx, cy, dx, dy;
    button_centre(&k_buttons[i], &cx, &cy);
    dx = (x - cx) * s_aspect;
    dy = y - cy;
    return dx * dx + dy * dy < k_buttons[i].r * slack * k_buttons[i].r * slack;
}

static Finger *finger(int64_t id, int create)
{
    int i;
    for (i = 0; i < MAX_FINGERS; i++)
        if (s_f[i].used && s_f[i].id == id)
            return &s_f[i];
    if (!create)
        return NULL;
    for (i = 0; i < MAX_FINGERS; i++)
        if (!s_f[i].used) {
            memset(&s_f[i], 0, sizeof s_f[i]);
            s_f[i].used = 1;
            s_f[i].id = id;
            s_f[i].button = -1;
            return &s_f[i];
        }
    return NULL;
}

static int stick_taken(void)
{
    int i;
    for (i = 0; i < MAX_FINGERS; i++)
        if (s_f[i].used && s_f[i].stick)
            return 1;
    return 0;
}

void touchpad_finger(int kind, int64_t id, float x, float y)
{
    Finger *f;
    if (!touchpad_enabled())
        return;
    pthread_mutex_lock(&s_lock);
    s_touch_ms = now_ms();
    if (kind == TOUCH_UP) {
        f = finger(id, 0);
        if (f)
            f->used = 0;
        pthread_mutex_unlock(&s_lock);
        return;
    }
    f = finger(id, kind == TOUCH_DOWN);
    if (!f) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    f->x = x;
    f->y = y;
    if (kind == TOUCH_DOWN) {
        f->button = hit(x, y, 1.15f);
        if (f->button < 0 && x < 0.45f && !stick_taken()) {
            /* The stick centres under the thumb, kept on screen. */
            float rx = TOUCH_STICK_R / s_aspect, ry = TOUCH_STICK_R;
            f->stick = 1;
            s_base[0] = x < rx ? rx : x;
            s_base[1] = y < ry ? ry : y > 1.0f - ry ? 1.0f - ry : y;
        }
    } else if (!f->stick) {
        /* Sliding onto another button moves the press there; drifting off
         * the one held lets go only past some slack. */
        int b = hit(x, y, 1.0f);
        if (b >= 0)
            f->button = b;
        else if (f->button >= 0 && !on_button(f->button, x, y, 1.3f))
            f->button = -1;
    }
    if (f->stick) {
        float dx = (x - s_base[0]) * s_aspect, dy = y - s_base[1];
        float d = sqrtf(dx * dx + dy * dy);
        if (d > TOUCH_STICK_R) {
            dx *= TOUCH_STICK_R / d;
            dy *= TOUCH_STICK_R / d;
        }
        s_knob[0] = s_base[0] + dx / s_aspect;
        s_knob[1] = s_base[1] + dy;
    }
    pthread_mutex_unlock(&s_lock);
}

/* Shown: touch controls exist, and the screen was touched more recently
 * than a controller was used -- or there is no controller at all. */
int touchpad_shown(void)
{
    static int force = -1;
    if (!touchpad_enabled())
        return 0;
    if (force < 0)
        force = getenv("RSC2_TOUCH_SHOW") != NULL;
    if (force)
        return 1;
    if (s_any_pad == 0)
        return 1;
    return s_touch_ms > 0.0 && s_touch_ms >= s_pad_ms;
}

/* For xinput_device.c: whether a real controller is on port 0. */
void touchpad_pad_present(int present)
{
    s_any_pad = present;
}

/* The touch state, merged into `st` (a real pad's, or zeroed). 1 while the
 * controls are shown: port 0 then counts as connected. */
int touchpad_merge(XBOX_INPUT_STATE *st)
{
    int i;
    if (!touchpad_shown())
        return 0;
    pthread_mutex_lock(&s_lock);
    for (i = 0; i < MAX_FINGERS; i++) {
        const Finger *f = &s_f[i];
        if (!f->used)
            continue;
        if (f->stick) {
            float dx = (s_knob[0] - s_base[0]) * s_aspect / TOUCH_STICK_R;
            float dy = (s_knob[1] - s_base[1]) / TOUCH_STICK_R;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > 0.08f) {                /* a dead zone at the centre */
                float k = (d - 0.08f) / (0.92f * d);
                st->Gamepad.sThumbLX = (SHORT)(dx * k * 32767.0f);
                st->Gamepad.sThumbLY = (SHORT)(-dy * k * 32767.0f);
            }
        } else if (f->button >= 0) {
            const TouchButton *b = &k_buttons[f->button];
            if (b->analog >= 0)
                st->Gamepad.bAnalogButtons[b->analog] = 255;
            else
                st->Gamepad.wButtons |= b->bit;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return 1;
}

int touchpad_draw_state(float *opacity, uint32_t *held, int *stick_on, float stick[4])
{
    int i;
    if (!touchpad_shown())
        return 0;
    *opacity = 1.0f;
    *held = 0;
    *stick_on = 0;
    pthread_mutex_lock(&s_lock);
    for (i = 0; i < MAX_FINGERS; i++) {
        if (!s_f[i].used)
            continue;
        if (s_f[i].stick)
            *stick_on = 1;
        else if (s_f[i].button >= 0)
            *held |= 1u << s_f[i].button;
    }
    if (*stick_on) {
        stick[0] = s_base[0]; stick[1] = s_base[1];
        stick[2] = s_knob[0]; stick[3] = s_knob[1];
    } else {
        /* Where the stick rests: a hint for the left thumb. */
        stick[0] = stick[2] = 0.22f / s_aspect + 0.04f;
        stick[1] = stick[3] = 0.72f;
    }
    pthread_mutex_unlock(&s_lock);
    return 1;
}
