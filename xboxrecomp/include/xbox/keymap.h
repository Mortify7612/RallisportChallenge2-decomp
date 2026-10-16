/*
 * Keyboard bindings for the keyboard-as-controller (RECOMP_KEYBOARD).
 *
 * Shared by the input layer, which reads them, the framebuffer window, which
 * turns host keys into the Win32 virtual-key codes the bindings are written
 * in, and a title's launcher, which edits them. Header-only so all three
 * agree without a link dependency between them.
 *
 * A binding is a pad input and up to KM_SLOTS keys. One key may drive
 * several inputs -- an arrow can be both the d-pad, for menus, and the
 * stick, for steering -- so the table is per input, not per key.
 *
 * The text form, in RECOMP_KEYMAP and in settings files, is
 *
 *     id=vk[+vk];id=vk[+vk];...      e.g.  "rt=26;lt=28;lx-=25+64"
 *
 * with the virtual-key codes in hex. Inputs it leaves out keep their
 * defaults; "id=" with no keys unbinds one.
 */
#ifndef XBOX_KEYMAP_H
#define XBOX_KEYMAP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KM_SLOTS 2

enum {
    KM_LX_NEG, KM_LX_POS, KM_LY_POS, KM_LY_NEG,     /* left stick: left right up down */
    KM_RT, KM_LT,
    KM_A, KM_B, KM_X, KM_Y, KM_BLACK, KM_WHITE,
    KM_START, KM_BACK,
    KM_UP, KM_DOWN, KM_LEFT, KM_RIGHT,              /* d-pad */
    KM_LTHUMB, KM_RTHUMB,                           /* stick clicks */
    KM_RX_NEG, KM_RX_POS, KM_RY_POS, KM_RY_NEG,     /* right stick */
    KM_MENU,            /* Start, or Back while the title's menus run */
    KM_COUNT
};

typedef struct {
    unsigned char key[KM_COUNT][KM_SLOTS];          /* 0 = empty slot */
} KeyMap;

static const char *const km_id[KM_COUNT] = {
    "lx-", "lx+", "ly+", "ly-", "rt", "lt",
    "a", "b", "x", "y", "black", "white", "start", "back",
    "up", "down", "left", "right", "ls", "rs",
    "rx-", "rx+", "ry+", "ry-", "menu",
};

static const char *const km_pad_name[KM_COUNT] = {
    "Left stick left", "Left stick right", "Left stick up", "Left stick down",
    "Right trigger", "Left trigger",
    "A", "B", "X", "Y", "Black", "White", "Start", "Back",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "Left stick click", "Right stick click",
    "Right stick left", "Right stick right", "Right stick up", "Right stick down",
    "Start, or Back in menus",
};

/* The layout the toolkit has always had: the keys a Dreamcast or Saturn
 * emulator would pick. A title with a better idea passes RECOMP_KEYMAP. */
static inline void km_defaults(KeyMap *m)
{
    static const unsigned char d[KM_COUNT][KM_SLOTS] = {
        { 0x64 }, { 0x66 }, { 0x68 }, { 0x62 },     /* numpad 4 6 8 2 */
        { '3' }, { '1' },
        { 'Z' }, { 'X' }, { 'A' }, { 'S' }, { 'E' }, { 'Q' },
        { 0x0D }, { 0x08 },                         /* Enter, Backspace */
        { 0x26 }, { 0x28 }, { 0x25 }, { 0x27 },     /* arrows */
        { 0x10 }, { 0x11 },                         /* Shift, Ctrl */
        { 'J' }, { 'L' }, { 'I' }, { 'K' },
        { 0 },                                      /* menu: titles opt in */
    };
    memcpy(m->key, d, sizeof d);
}

static inline int km_find(const char *id, size_t n)
{
    int i;
    for (i = 0; i < KM_COUNT; i++)
        if (strlen(km_id[i]) == n && strncmp(km_id[i], id, n) == 0)
            return i;
    return -1;
}

/* Applies `text` over whatever `m` holds. Unknown ids are skipped. */
static inline void km_parse(KeyMap *m, const char *text)
{
    const char *p = text;
    while (p && *p) {
        const char *end = p + strcspn(p, ";"), *eq = memchr(p, '=', (size_t)(end - p));
        if (eq) {
            int i = km_find(p, (size_t)(eq - p)), s = 0;
            const char *v = eq + 1;
            if (i >= 0) {
                memset(m->key[i], 0, KM_SLOTS);
                while (v < end && s < KM_SLOTS) {
                    char *stop;
                    unsigned long vk = strtoul(v, &stop, 16);
                    if (stop == v)
                        break;
                    if (vk > 0 && vk < 256)
                        m->key[i][s++] = (unsigned char)vk;
                    v = stop;
                    if (*v == '+')
                        v++;
                }
            }
        }
        p = *end ? end + 1 : end;
    }
}

static inline void km_format(const KeyMap *m, char *out, size_t n)
{
    size_t used = 0;
    int i, s;
    if (n)
        out[0] = 0;
    for (i = 0; i < KM_COUNT && used < n; i++) {
        used += (size_t)snprintf(out + used, n - used, "%s%s=", i ? ";" : "", km_id[i]);
        for (s = 0; s < KM_SLOTS && used < n; s++)
            if (m->key[i][s])
                used += (size_t)snprintf(out + used, n - used, "%s%02X",
                                         s && m->key[i][0] ? "+" : "", m->key[i][s]);
    }
}

/* What a key is called on the keycap; NULL for codes with no name. */
static inline const char *km_key_name(int vk)
{
    static char one[128][2];
    static const struct { unsigned char vk; const char *name; } t[] = {
        { 0x08, "Backspace" }, { 0x09, "Tab" }, { 0x0D, "Enter" }, { 0x10, "Shift" },
        { 0x11, "Ctrl" }, { 0x12, "Alt" }, { 0x14, "Caps Lock" }, { 0x1B, "Esc" },
        { 0x20, "Space" }, { 0x21, "Page Up" }, { 0x22, "Page Down" }, { 0x23, "End" },
        { 0x24, "Home" }, { 0x25, "Left" }, { 0x26, "Up" }, { 0x27, "Right" },
        { 0x28, "Down" }, { 0x2D, "Insert" }, { 0x2E, "Delete" },
        { 0x60, "Num 0" }, { 0x61, "Num 1" }, { 0x62, "Num 2" }, { 0x63, "Num 3" },
        { 0x64, "Num 4" }, { 0x65, "Num 5" }, { 0x66, "Num 6" }, { 0x67, "Num 7" },
        { 0x68, "Num 8" }, { 0x69, "Num 9" }, { 0x6A, "Num *" }, { 0x6B, "Num +" },
        { 0x6D, "Num -" }, { 0x6E, "Num ." }, { 0x6F, "Num /" },
        { 0x70, "F1" }, { 0x71, "F2" }, { 0x72, "F3" }, { 0x73, "F4" }, { 0x74, "F5" },
        { 0x75, "F6" }, { 0x76, "F7" }, { 0x77, "F8" }, { 0x78, "F9" }, { 0x79, "F10" },
        { 0x7A, "F11" }, { 0x7B, "F12" },
        { 0xBA, ";" }, { 0xBB, "=" }, { 0xBC, "," }, { 0xBD, "-" }, { 0xBE, "." },
        { 0xBF, "/" }, { 0xC0, "`" }, { 0xDB, "[" }, { 0xDC, "\\" }, { 0xDD, "]" },
        { 0xDE, "'" },
    };
    size_t i;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        char *s = one[vk];
        s[0] = (char)vk;
        s[1] = 0;
        return s;
    }
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].vk == vk)
            return t[i].name;
    return NULL;
}

/* Keys the window keeps for itself: F3 FPS counter, F9 mark, F10 flight
 * recorder, F11 fullscreen. */
static inline int km_key_reserved(int vk)
{
    return vk == 0x72 || vk == 0x78 || vk == 0x79 || vk == 0x7A;
}

#endif /* XBOX_KEYMAP_H */

/* Its own guard: a file may include this before SDL.h and again after. */
#if (defined(SDL_h_) || defined(_SDL_H)) && !defined(XBOX_KEYMAP_SDL_H)
#define XBOX_KEYMAP_SDL_H
/* SDL key -> virtual-key code: letters and digits are their uppercase
 * ASCII, the rest by table. 0 for keys with no counterpart. */
static inline int km_sdl_to_vk(SDL_Keycode k)
{
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (int)(k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (int)(k - SDLK_0);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 0x61 + (int)(k - SDLK_KP_1);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (int)(k - SDLK_F1);
    switch (k) {
    case SDLK_KP_0:         return 0x60;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:     return 0x0D;
    case SDLK_BACKSPACE:    return 0x08;
    case SDLK_TAB:          return 0x09;
    case SDLK_ESCAPE:       return 0x1B;
    case SDLK_SPACE:        return 0x20;
    case SDLK_PAGEUP:       return 0x21;
    case SDLK_PAGEDOWN:     return 0x22;
    case SDLK_END:          return 0x23;
    case SDLK_HOME:         return 0x24;
    case SDLK_LEFT:         return 0x25;
    case SDLK_UP:           return 0x26;
    case SDLK_RIGHT:        return 0x27;
    case SDLK_DOWN:         return 0x28;
    case SDLK_INSERT:       return 0x2D;
    case SDLK_DELETE:       return 0x2E;
    case SDLK_CAPSLOCK:     return 0x14;
    case SDLK_KP_MULTIPLY:  return 0x6A;
    case SDLK_KP_PLUS:      return 0x6B;
    case SDLK_KP_MINUS:     return 0x6D;
    case SDLK_KP_PERIOD:    return 0x6E;
    case SDLK_KP_DIVIDE:    return 0x6F;
    case SDLK_SEMICOLON:    return 0xBA;
    case SDLK_EQUALS:       return 0xBB;
    case SDLK_COMMA:        return 0xBC;
    case SDLK_MINUS:        return 0xBD;
    case SDLK_PERIOD:       return 0xBE;
    case SDLK_SLASH:        return 0xBF;
    case SDLK_BACKQUOTE:    return 0xC0;
    case SDLK_LEFTBRACKET:  return 0xDB;
    case SDLK_BACKSLASH:    return 0xDC;
    case SDLK_RIGHTBRACKET: return 0xDD;
    case SDLK_QUOTE:        return 0xDE;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL:  case SDLK_RCTRL:  return 0x11;
    case SDLK_LALT:   case SDLK_RALT:   return 0x12;
    default:                return 0;
    }
}
#endif
