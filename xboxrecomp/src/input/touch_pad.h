/*
 * touch_pad.h -- on-screen touch controls standing in for controller 1.
 *
 * A floating stick on the left half of the screen, buttons on the right
 * (touch_pad.c has the layout). The window feeds it finger events; the
 * input layer merges its state into port 0 like the keyboard; the renderer
 * draws it from the same layout table, so what is drawn is what is hit.
 */
#ifndef TOUCH_PAD_H
#define TOUCH_PAD_H

#include <stdint.h>
#include "xinput_xbox.h"

/* One on-screen button. Positions are in screen-height units: x from the
 * left edge (anchor 0), the right edge (1) or the centre (2); y from the
 * top. `analog` is an XBOX_BUTTON_* index, or -1 for a digital `bit`. */
typedef struct {
    int      anchor;
    float    x, y, r;
    int      analog;
    uint16_t bit;
    uint32_t rgb;
    const char *label;
} TouchButton;

#define TOUCH_STICK_R    0.14f      /* how far the stick travels, height units */
#define TOUCH_KNOB_R     0.06f

enum { TOUCH_DOWN, TOUCH_MOVE, TOUCH_UP };

/* Whether touch controls exist at all: on by default on Android,
 * RECOMP_TOUCH=1 elsewhere (=0 off). */
int  touchpad_enabled(void);

/* A finger event, x and y as fractions of the window (SDL's tfinger). */
void touchpad_finger(int kind, int64_t id, float x, float y);
/* The window's size in pixels (for the layout's aspect). */
void touchpad_set_view(int w, int h);
/* A real controller was used: the touch controls hide until touched. */
void touchpad_pad_activity(void);

/* For the input layer: whether a real controller is on port 0, and the
 * touch state merged into port 0's (returns 1 while the controls are shown,
 * when port 0 counts as connected even without a pad). */
void touchpad_pad_present(int present);
int  touchpad_shown(void);
int  touchpad_merge(XBOX_INPUT_STATE *st);

/* The layout. */
int  touchpad_layout(const TouchButton **buttons);

/* What to draw now: 0 when hidden. Otherwise the opacity, which buttons
 * are held (bit i = button i), and the stick: shown, its centre and the
 * knob's, in window fractions. */
int  touchpad_draw_state(float *opacity, uint32_t *held, int *stick_on,
                         float stick[4]);

#endif
