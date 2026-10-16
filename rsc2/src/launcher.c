/*
 * Pre-game launcher: a settings window shown before the title boots.
 *
 * Every setting is an RECOMP_* environment variable the runtime already
 * reads, so the launcher only has to choose values: they are saved to
 * settings.ini (~/.config/rsc2 or %APPDATA%\rsc2) and put into the
 * environment before boot. A variable set by hand still wins, which keeps
 * scripted and headless runs exactly as they were.
 *
 * When it shows: by default, unless the saved "show at startup" is off.
 * --launcher / RSC2_LAUNCHER=1 force it, --no-launcher / RSC2_LAUNCHER=0
 * skip it, and it never shows for headless runs (RECOMP_FB_WINDOW=0) or
 * input replays (RSC2_INPUT_REPLAY), which must not wait on a window.
 *
 * The window is SDL2 with text from stb_truetype and an embedded Noto Sans
 * (generated headers, rsc2/cmake/embed_bin.cmake).
 * Mouse, keyboard and controller all drive it. A Windows build links a
 * static SDL2 for it; built without one (RSC2_LAUNCHER_SDL unset), the saved
 * settings still apply and the window is skipped.
 */
#include "launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

/* The renderer's GPU list (xboxrecomp nv2a_vk.c). */
int nv2a_vk_device_names(char (*names)[256], int max);

/* ── settings ─────────────────────────────────────────────── */

#define MAXV 12

enum {
    O_RENDERER, O_GPU, O_SCALE, O_ASPECT, O_DISPLAY, O_FPS, O_FPSCOUNT, O_SHOW,
    O_MSAA, O_ANISO, O_AO, O_SOUND, O_REVERB, O_KEYBOARD, O_RUMBLE,
    O_COUNT
};

typedef struct {
    const char *key;            /* settings.ini */
    const char *env;            /* NULL: launcher-only */
    const char *section;        /* header drawn above the row, or NULL */
    const char *label, *help;
    int col;                    /* 0 left column, 1 right */
    int soon;                   /* shown, not available yet */
    int n, def, cur;
    char name[MAXV][64];        /* what the row shows */
    char val[MAXV][256];        /* what is saved / exported; "" = unset */
    const char *env_set;        /* the variable was set by hand */
} Opt;

static Opt s_opt[O_COUNT];
static char s_path[1024];

static void opt_add(Opt *o, const char *name, const char *val)
{
    if (o->n >= MAXV)
        return;
    snprintf(o->name[o->n], sizeof o->name[0], "%s", name);
    snprintf(o->val[o->n], sizeof o->val[0], "%s", val);
    o->n++;
}

static Opt *opt_def(int i, const char *key, const char *env, const char *section,
                    int col, const char *label, const char *help)
{
    Opt *o = &s_opt[i];
    memset(o, 0, sizeof *o);
    o->key = key;
    o->env = env;
    o->section = section;
    o->col = col;
    o->label = label;
    o->help = help;
    return o;
}

static void opts_init(int list_gpus)
{
    Opt *o;
    o = opt_def(O_RENDERER, "renderer", "RECOMP_VULKAN", "DISPLAY", 0, "Renderer",
                "Vulkan draws on your graphics card. Software is the slow CPU "
                "fallback, for when Vulkan does not work.");
    opt_add(o, "Vulkan", "1");
    opt_add(o, "Software", "0");

    o = opt_def(O_GPU, "gpu", "RECOMP_VK_DEVICE", NULL, 0, "Graphics card",
                "Which GPU renders the game. Automatic prefers a dedicated card.");
    opt_add(o, "Automatic", "");
    if (list_gpus) {
        char names[MAXV - 1][256];
        int n = nv2a_vk_device_names(names, MAXV - 1), i;
        for (i = 0; i < n; i++) {
            char shown[64];
            /* "NVIDIA GeForce RTX 5070 Ti" fits; long names are cut. */
            snprintf(shown, sizeof shown, "%.26s%s", names[i],
                     strlen(names[i]) > 26 ? "..." : "");
            opt_add(o, shown, names[i]);
        }
    }

    o = opt_def(O_SCALE, "scale", "RECOMP_VK_SCALE", NULL, 0, "Resolution",
                "Internal rendering resolution. The original is 640\xC3\x97" "480; "
                "higher is sharper and costs more GPU time.");
    opt_add(o, "1\xC3\x97  (640\xC3\x97" "480)", "1");
    opt_add(o, "2\xC3\x97  (1280\xC3\x97" "960)", "2");
    opt_add(o, "3\xC3\x97  (1920\xC3\x97" "1440)", "3");
    opt_add(o, "4\xC3\x97  (2560\xC3\x97" "1920)", "4");
    opt_add(o, "5\xC3\x97  (3200\xC3\x97" "2400)", "5");
    opt_add(o, "6\xC3\x97  (3840\xC3\x97" "2880)", "6");
    o->def = 1;

    o = opt_def(O_ASPECT, "aspect", "RECOMP_WIDESCREEN", NULL, 0, "Aspect ratio",
                "16:9 turns on the game's own widescreen mode, as on an Xbox set to a "
                "widescreen TV: a wider view, with the HUD moved out to the edges.");
    opt_add(o, "4:3  (original)", "0");
    opt_add(o, "16:9  widescreen", "1");

    o = opt_def(O_DISPLAY, "display", "RECOMP_FULLSCREEN", NULL, 0, "Display mode",
                "Fullscreen uses your desktop resolution. F11 switches while playing.");
    opt_add(o, "Windowed", "0");
    opt_add(o, "Fullscreen", "1");

    o = opt_def(O_FPS, "fps_limit", "RECOMP_FLIP_HZ", NULL, 0, "Frame rate limit",
                "The game keeps time by the clock, so it runs at the right speed at any "
                "frame rate. 60 is the original; higher needs a faster PC.");
    opt_add(o, "60  (original)", "60");
    opt_add(o, "90", "90");
    opt_add(o, "120", "120");
    opt_add(o, "144", "144");
    opt_add(o, "Unlimited", "0");

    o = opt_def(O_FPSCOUNT, "fps_counter", "RECOMP_FPS_OVERLAY", NULL, 0, "FPS counter",
                "Frames per second and frame times in the corner, with a graph of "
                "recent frames. F3 switches it while playing.");
    opt_add(o, "Off", "0");
    opt_add(o, "On", "1");

    o = opt_def(O_SHOW, "show_launcher", NULL, "LAUNCHER", 0, "Show at startup",
                "When off, the game starts straight away. Start it with --launcher "
                "to get this screen back.");
    opt_add(o, "Yes", "1");
    opt_add(o, "No", "0");

    o = opt_def(O_MSAA, "msaa", "RECOMP_VK_MSAA", "GRAPHICS", 1, "Anti-aliasing",
                "Multisample anti-aliasing smooths the jagged edges of the 3D scene. "
                "4\xC3\x97 is a good balance; each step costs GPU time.");
    opt_add(o, "Off", "0");
    opt_add(o, "2\xC3\x97 MSAA", "2");
    opt_add(o, "4\xC3\x97 MSAA", "4");
    opt_add(o, "8\xC3\x97 MSAA", "8");

    o = opt_def(O_ANISO, "anisotropic", "RECOMP_VK_ANISO", NULL, 1, "Texture filtering",
                "Anisotropic filtering keeps the road and ground sharp into the "
                "distance instead of blurring. Costs very little.");
    opt_add(o, "Standard", "0");
    opt_add(o, "4\xC3\x97 anisotropic", "4");
    opt_add(o, "8\xC3\x97 anisotropic", "8");
    opt_add(o, "16\xC3\x97 anisotropic", "16");
    o->def = 3;

    o = opt_def(O_AO, "ambient_occlusion", "RECOMP_VK_AO", NULL, 1, "Ambient occlusion",
                "Shades the corners and contact points of the scenery for more depth. "
                "Not in the original game; costs some GPU time.");
    opt_add(o, "Off", "0");
    opt_add(o, "On", "1");

    o = opt_def(O_SOUND, "sound", "RECOMP_AUDIO", "AUDIO", 1, "Sound",
                "Turns all game audio on or off.");
    opt_add(o, "On", "1");
    opt_add(o, "Off", "0");

    o = opt_def(O_REVERB, "reverb", "RECOMP_APU_REVERB", NULL, 1, "Reverb",
                "The game's own room echo for each place on the track (tunnels, "
                "forest, valleys, in the car), rebuilt from its settings. Normal is "
                "the original level.");
    opt_add(o, "Off", "0");
    opt_add(o, "Light", "0.15");
    opt_add(o, "Normal", "0.3");
    opt_add(o, "Strong", "0.5");
    o->def = 2;

    o = opt_def(O_KEYBOARD, "keyboard", "RECOMP_KEYBOARD", "CONTROLS", 1, "Keyboard",
                "Play with the keyboard as controller 1: arrows steer, Z X A S are "
                "A B X Y, 1 and 3 are the triggers, Enter is Start.");
    opt_add(o, "On", "1");
    opt_add(o, "Off", "0");

    o = opt_def(O_RUMBLE, "rumble", "RECOMP_RUMBLE", NULL, 1, "Vibration",
                "Controller rumble from the game.");
    opt_add(o, "On", "1");
    opt_add(o, "Off", "0");
}

static int opt_find(const Opt *o, const char *val)
{
    int i;
    for (i = 0; i < o->n; i++)
        if (strcmp(o->val[i], val) == 0)
            return i;
    return -1;
}

static void settings_dir(char *out, size_t n)
{
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    snprintf(out, n, "%s\\rsc2", base && *base ? base : ".");
#else
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x)
        snprintf(out, n, "%s/rsc2", x);
    else
        snprintf(out, n, "%s/.config/rsc2", h && *h ? h : ".");
#endif
}

static void settings_load(void)
{
    char dir[900], line[512];
    FILE *f;
    int i;

    settings_dir(dir, sizeof dir);
#ifdef _WIN32
    snprintf(s_path, sizeof s_path, "%s\\settings.ini", dir);
#else
    snprintf(s_path, sizeof s_path, "%s/settings.ini", dir);
#endif
    for (i = 0; i < O_COUNT; i++)
        s_opt[i].cur = s_opt[i].def;
    f = fopen(s_path, "r");
    if (!f)
        return;
    /* A saved GPU stays chosen even when the list was not built (launcher
     * hidden) or the card is absent: the renderer falls back by itself. */
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *end;
        if (!eq || line[0] == '#' || line[0] == ';')
            continue;
        *eq = 0;
        end = eq + 1 + strcspn(eq + 1, "\r\n");
        *end = 0;
        for (i = 0; i < O_COUNT; i++) {
            if (strcmp(s_opt[i].key, line) == 0) {
                int k = opt_find(&s_opt[i], eq + 1);
                if (k < 0 && i == O_GPU && eq[1]) {
                    char shown[64];
                    snprintf(shown, sizeof shown, "%.26s%s", eq + 1,
                             strlen(eq + 1) > 26 ? "..." : "");
                    opt_add(&s_opt[i], shown, eq + 1);
                    k = s_opt[i].n - 1;
                }
                if (k >= 0)
                    s_opt[i].cur = k;
            }
        }
    }
    fclose(f);
}

static void settings_save(void)
{
    char dir[900];
    FILE *f;
    int i;

    settings_dir(dir, sizeof dir);
#ifdef _WIN32
    _mkdir(dir);
#else
    {
        /* ~/.config may not exist yet either. */
        char parent[900], *sl;
        snprintf(parent, sizeof parent, "%s", dir);
        sl = strrchr(parent, '/');
        if (sl) {
            *sl = 0;
            mkdir(parent, 0755);
        }
        mkdir(dir, 0755);
    }
#endif
    f = fopen(s_path, "w");
    if (!f) {
        fprintf(stderr, "[LAUNCHER] cannot write %s\n", s_path);
        return;
    }
    fprintf(f, "# RalliSport Challenge 2 (recompiled) settings, written by the launcher.\n");
    for (i = 0; i < O_COUNT; i++)
        fprintf(f, "%s=%s\n", s_opt[i].key, s_opt[i].val[s_opt[i].cur]);
    fclose(f);
}

static void env_put(const char *k, const char *v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static int vk_only(int i)
{
    return i == O_GPU || i == O_SCALE || i == O_MSAA || i == O_ANISO || i == O_AO
        || i == O_FPSCOUNT;
}

/* Only variables nobody set by hand. The Vulkan-only settings are left out
 * for the software renderer. */
static void settings_apply(void)
{
    int i, soft = strcmp(s_opt[O_RENDERER].val[s_opt[O_RENDERER].cur], "0") == 0;
    for (i = 0; i < O_COUNT; i++) {
        const Opt *o = &s_opt[i];
        const char *v = o->val[o->cur];
        if (!o->env || o->soon || o->env_set || !*v)
            continue;
        if (soft && vk_only(i))
            continue;
        env_put(o->env, v);
    }
}

static int row_disabled(int i)
{
    int soft = strcmp(s_opt[O_RENDERER].val[s_opt[O_RENDERER].cur], "0") == 0;
    if (s_opt[i].soon || s_opt[i].env_set)
        return 1;
    if (soft && vk_only(i))
        return 1;
    return i == O_GPU && s_opt[i].n < 2;
}

/* ── the window (SDL2) ────────────────────────────────────── */

#ifndef RSC2_LAUNCHER_SDL

static int launcher_window(void) { return 1; }

#else

/* The game has its own entry point; SDL must not bring SDL_main. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include "font_Regular.h"     /* k_font_Regular: assets/fonts/NotoSans-Regular.ttf */
#include "font_Bold.h"        /* k_font_Bold */

/* Layout in logical units; the renderer's pixels per unit is s_px. */
#define WIN_W 1000
#define WIN_H 760
#define ROW_H 42
#define SEC_H 34
#define COL_W 444
#define COL0_X 40
#define COL1_X 516
#define TOP_Y 128

enum { B_DEFAULTS = O_COUNT, B_QUIT, B_START, FOCUS_COUNT };

typedef struct {
    SDL_Texture *tex;
    stbtt_packedchar pc[3][256];
    int first[3], count[3];
    int aw, ah;
    float ascent, size;
} Font;

typedef struct { float x, y, w, h; } Box;

static SDL_Renderer *s_ren;
static float s_px = 1.0f;
static Font s_body, s_bold, s_title, s_small, s_smallb;
static int s_focus = B_START, s_hover = -1;
static Box s_row_box[O_COUNT], s_btn_box[3];

/* Colours. */
static const SDL_Color C_BG0 = { 0x0E, 0x13, 0x1E, 255 }, C_BG1 = { 0x17, 0x1F, 0x30, 255 };
static const SDL_Color C_TEXT = { 0xE8, 0xEC, 0xF4, 255 }, C_MUTED = { 0x8A, 0x95, 0xAD, 255 };
static const SDL_Color C_DIM = { 0x55, 0x5F, 0x76, 255 }, C_ACCENT = { 0xFF, 0x7A, 0x1A, 255 };
static const SDL_Color C_ROW = { 0x1F, 0x29, 0x3E, 255 }, C_ROWH = { 0x19, 0x22, 0x34, 255 };
static const SDL_Color C_LINE = { 0x26, 0x30, 0x46, 255 }, C_PILL = { 0x2A, 0x35, 0x50, 255 };

static int font_init(Font *f, const unsigned char *ttf, float size)
{
    static const int ranges[3][2] = { { 32, 95 }, { 160, 96 }, { 0x2010, 43 } };
    stbtt_pack_context pk;
    stbtt_pack_range pr[3];
    stbtt_fontinfo info;
    unsigned char *a;
    Uint32 *rgba;
    int i, asc, desc, gap;
    float px = size * s_px;

    memset(f, 0, sizeof *f);
    f->size = size;
    for (i = 0; i < 3; i++) {
        f->first[i] = ranges[i][0];
        f->count[i] = ranges[i][1];
        pr[i].font_size = px;
        pr[i].first_unicode_codepoint_in_range = ranges[i][0];
        pr[i].array_of_unicode_codepoints = NULL;
        pr[i].num_chars = ranges[i][1];
        pr[i].chardata_for_range = f->pc[i];
    }
    /* The smallest atlas everything fits in. */
    for (f->aw = 512; ; f->aw *= 2) {
        int ok;
        f->ah = f->aw;
        a = (unsigned char *)calloc((size_t)f->aw * f->ah, 1);
        if (!a || !stbtt_PackBegin(&pk, a, f->aw, f->ah, 0, 1, NULL)) {
            free(a);
            return 0;
        }
        stbtt_PackSetOversampling(&pk, 2, 1);
        ok = stbtt_PackFontRanges(&pk, ttf, 0, pr, 3);
        stbtt_PackEnd(&pk);
        if (ok || f->aw >= 4096)
            break;
        free(a);
    }
    stbtt_InitFont(&info, ttf, 0);
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    f->ascent = asc * stbtt_ScaleForPixelHeight(&info, px) / s_px;

    rgba = (Uint32 *)malloc((size_t)f->aw * f->ah * 4);
    if (!rgba) {
        free(a);
        return 0;
    }
    for (i = 0; i < f->aw * f->ah; i++)
        rgba[i] = (Uint32)a[i] << 24 | 0x00FFFFFFu;
    free(a);
    f->tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                               f->aw, f->ah);
    if (f->tex) {
        SDL_UpdateTexture(f->tex, NULL, rgba, f->aw * 4);
        SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
    }
    free(rgba);
    return f->tex != NULL;
}

/* Next code point of a UTF-8 string. */
static unsigned utf8(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) {
        c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u);
        p += 3;
    } else if (c >= 0xE0 && p[0] && p[1]) {
        c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u);
        p += 2;
    } else if (c >= 0xC0 && p[0]) {
        c = (c & 31) << 6 | (p[0] & 63u);
        p += 1;
    }
    *s = (const char *)p;
    return c;
}

static const stbtt_packedchar *glyph(const Font *f, unsigned c, int *range, int *idx)
{
    int i;
    for (i = 0; i < 3; i++)
        if (c >= (unsigned)f->first[i] && c < (unsigned)(f->first[i] + f->count[i])) {
            *range = i;
            *idx = (int)c - f->first[i];
            return &f->pc[i][*idx];
        }
    *range = 0;
    *idx = '?' - 32;
    return &f->pc[0][*idx];
}

/* Width in logical units; `spacing` adds between letters (section heads). */
static float text_w(const Font *f, const char *s, float spacing)
{
    float w = 0;
    while (*s) {
        int r, k;
        w += glyph(f, utf8(&s), &r, &k)->xadvance / s_px + spacing;
    }
    return w;
}

/* `y` is the top of the line box (ascent below it is the baseline). */
static float text(const Font *f, float x, float y, const char *s, SDL_Color c, float spacing)
{
    float px = x * s_px, py = (y + f->ascent) * s_px;
    SDL_SetTextureColorMod(f->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(f->tex, c.a);
    while (*s) {
        stbtt_aligned_quad q;
        int r, k;
        SDL_Rect src;
        SDL_FRect dst;
        glyph(f, utf8(&s), &r, &k);
        stbtt_GetPackedQuad(f->pc[r], f->aw, f->ah, k, &px, &py, &q, 0);
        px += spacing * s_px;
        src.x = (int)(q.s0 * f->aw + 0.5f);
        src.y = (int)(q.t0 * f->ah + 0.5f);
        src.w = (int)((q.s1 - q.s0) * f->aw + 0.5f);
        src.h = (int)((q.t1 - q.t0) * f->ah + 0.5f);
        dst.x = q.x0;
        dst.y = q.y0;
        dst.w = q.x1 - q.x0;
        dst.h = q.y1 - q.y0;
        if (src.w > 0 && src.h > 0)
            SDL_RenderCopyF(s_ren, f->tex, &src, &dst);
    }
    return px / s_px;
}

/* Wrapped at word boundaries to `w`. */
static void text_wrap(const Font *f, float x, float y, float w, float lh,
                      const char *s, SDL_Color c)
{
    char line[512];
    while (*s) {
        size_t n = 0, last = 0;
        while (s[n] && n < sizeof line - 1) {
            char tmp[512];
            memcpy(tmp, s, n + 1);
            tmp[n + 1] = 0;
            if (text_w(f, tmp, 0) > w)
                break;
            if (s[n] == ' ')
                last = n;
            n++;
        }
        if (s[n] && last)
            n = last;
        memcpy(line, s, n);
        line[n] = 0;
        text(f, x, y, line, c, 0);
        s += n;
        while (*s == ' ')
            s++;
        y += lh;
    }
}

static void fill(float x, float y, float w, float h, SDL_Color c)
{
    SDL_FRect r = { x * s_px, y * s_px, w * s_px, h * s_px };
    SDL_SetRenderDrawColor(s_ren, c.r, c.g, c.b, c.a);
    SDL_RenderFillRectF(s_ren, &r);
}

/* A filled rounded rectangle, as a triangle fan. */
static void rrect(float x, float y, float w, float h, float rad, SDL_Color c)
{
    enum { SEG = 6 };
    SDL_Vertex v[1 + 4 * (SEG + 1)];
    int idx[3 * 4 * (SEG + 1)], n = 1, k = 0, corner, i;
    const float cx[4] = { x + w - rad, x + rad, x + rad, x + w - rad };
    const float cy[4] = { y + h - rad, y + h - rad, y + rad, y + rad };

    memset(v, 0, sizeof v);
    v[0].position.x = (x + w * 0.5f) * s_px;
    v[0].position.y = (y + h * 0.5f) * s_px;
    /* Round the outline in one direction: each corner a quarter turn. */
    for (corner = 0; corner < 4; corner++)
        for (i = 0; i <= SEG; i++) {
            float t = (float)M_PI * 0.5f * ((float)corner + (float)i / SEG);
            v[n].position.x = (cx[corner] + cosf(t) * rad) * s_px;
            v[n].position.y = (cy[corner] + sinf(t) * rad) * s_px;
            n++;
        }
    for (i = 0; i < n; i++)
        v[i].color = c;
    for (i = 1; i < n; i++) {
        idx[k++] = 0;
        idx[k++] = i;
        idx[k++] = i + 1 < n ? i + 1 : 1;
    }
    SDL_RenderGeometry(s_ren, NULL, v, n, idx, k);
}

/* A small filled arrow centred on (x, y), pointing left (d < 0) or right. */
static void arrow(float x, float y, int d, SDL_Color c)
{
    SDL_Vertex v[3];
    int i;
    memset(v, 0, sizeof v);
    v[0].position.x = (x - 3.5f * d) * s_px; v[0].position.y = (y - 6) * s_px;
    v[1].position.x = (x + 3.5f * d) * s_px; v[1].position.y = y * s_px;
    v[2].position.x = (x - 3.5f * d) * s_px; v[2].position.y = (y + 6) * s_px;
    for (i = 0; i < 3; i++)
        v[i].color = c;
    SDL_RenderGeometry(s_ren, NULL, v, 3, NULL, 0);
}

static void background(void)
{
    SDL_Vertex v[4];
    static const int idx[6] = { 0, 1, 2, 2, 1, 3 };
    int i;
    memset(v, 0, sizeof v);
    for (i = 0; i < 4; i++) {
        SDL_Color c = i < 2 ? C_BG0 : C_BG1;
        v[i].position.x = (i & 1) ? WIN_W * s_px : 0;
        v[i].position.y = i < 2 ? 0 : WIN_H * s_px;
        v[i].color = c;
    }
    SDL_RenderGeometry(s_ren, NULL, v, 4, idx, 6);
}

static int in_box(const Box *b, float x, float y)
{
    return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

static void draw_row(int i, float x, float y)
{
    const Opt *o = &s_opt[i];
    int dis = row_disabled(i), foc = s_focus == i;
    float vx = x + COL_W - 236, vw = 220, cy = y + (ROW_H - 24) * 0.5f;
    SDL_Color lc = dis && !o->soon && !o->env_set ? C_DIM : C_TEXT;
    SDL_Color vc = dis ? C_MUTED : C_TEXT, ac = foc && !dis ? C_ACCENT : C_DIM;

    s_row_box[i].x = x;
    s_row_box[i].y = y;
    s_row_box[i].w = COL_W;
    s_row_box[i].h = ROW_H;
    if (foc) {
        rrect(x, y, COL_W, ROW_H, 8, C_ROW);
        rrect(x, y + 9, 3, ROW_H - 18, 1.5f, C_ACCENT);
    } else if (s_hover == i) {
        rrect(x, y, COL_W, ROW_H, 8, C_ROWH);
    }
    text(&s_body, x + 18, cy, o->label, lc, 0);
    if (o->soon) {
        const char *t = "COMING SOON";
        float tw = text_w(&s_smallb, t, 0.8f);
        rrect(vx + vw - tw - 20, y + 10, tw + 20, ROW_H - 20, 11, C_PILL);
        text(&s_smallb, vx + vw - tw - 10, y + 13, t, C_ACCENT, 0.8f);
        text(&s_body, vx + vw - tw - 34 - text_w(&s_body, o->name[o->cur], 0), cy,
             o->name[o->cur], C_MUTED, 0);
        return;
    }
    if (o->env_set) {
        char t[64];
        snprintf(t, sizeof t, "set by %s", o->env);
        text(&s_small, vx + vw - text_w(&s_small, t, 0), cy + 3, t, C_MUTED, 0);
        return;
    }
    {
        const char *t = o->name[o->cur];
        char res[64];
        float tw;
        /* Widescreen renders 4/3 more columns (nv2a_vk.c): say so. */
        if (i == O_SCALE && strcmp(s_opt[O_ASPECT].val[s_opt[O_ASPECT].cur], "1") == 0) {
            int k = atoi(o->val[o->cur]), kx = (k * 4 + 2) / 3;
            snprintf(res, sizeof res, "%d\xC3\x97  (%d\xC3\x97%d)", k,
                     640 * (kx > 8 ? 8 : kx), 480 * k);
            t = res;
        }
        tw = text_w(&s_bold, t, 0);
        text(&s_bold, vx + (vw - tw) * 0.5f, cy, t, vc, 0);
        arrow(vx + 6, y + ROW_H * 0.5f - 1.5f, -1, ac);
        arrow(vx + vw - 6, y + ROW_H * 0.5f - 1.5f, 1, ac);
    }
}

static void draw_button(int b, float x, float y, float w, const char *label, int primary)
{
    int foc = s_focus == b, hov = s_hover == b;
    SDL_Color bg = primary ? C_ACCENT : (foc ? C_PILL : C_ROW);
    SDL_Color fg = primary ? (SDL_Color){ 0x14, 0x0C, 0x05, 255 } : C_TEXT;
    Box *bx = &s_btn_box[b - B_DEFAULTS];
    float tw = text_w(&s_bold, label, 0);

    bx->x = x;
    bx->y = y;
    bx->w = w;
    bx->h = 46;
    if (primary && (foc || hov))
        bg = (SDL_Color){ 0xFF, 0x93, 0x45, 255 };
    else if (!primary && hov)
        bg = C_PILL;
    if (foc) /* focus ring */
        rrect(x - 3, y - 3, w + 6, 52, 12, primary ? (SDL_Color){ 0xFF, 0xC2, 0x8F, 255 } : C_MUTED);
    rrect(x, y, w, 46, 10, bg);
    text(&s_bold, x + (w - tw) * 0.5f, y + 11, label, fg, 0);
}

static void keycap(float *x, float y, const char *key, const char *what)
{
    float kw = text_w(&s_smallb, key, 0) + 14;
    rrect(*x, y, kw, 22, 5, C_PILL);
    text(&s_smallb, *x + 7, y + 3, key, C_TEXT, 0);
    *x += kw + 6;
    *x = text(&s_small, *x, y + 3, what, C_MUTED, 0) + 18;
}

static void draw(void)
{
    float y[2] = { TOP_Y, TOP_Y };
    int i;

    background();
    text(&s_title, 48, 26, "RalliSport Challenge 2", C_TEXT, 0);
    text(&s_body, 50, 70, "Static recompilation \xC2\xB7 Launcher", C_MUTED, 0);
    fill(48, 102, 56, 3, C_ACCENT);
    fill(104, 103, WIN_W - 152, 1, C_LINE);

    for (i = 0; i < O_COUNT; i++) {
        const Opt *o = &s_opt[i];
        float x = o->col ? COL1_X : COL0_X;
        if (o->section) {
            text(&s_smallb, x + 18, y[o->col] + 10, o->section, C_ACCENT, 1.6f);
            y[o->col] += SEC_H;
        }
        draw_row(i, x, y[o->col]);
        y[o->col] += ROW_H + 2;
    }

    /* What the focused row does. */
    if (s_focus < O_COUNT) {
        const Opt *o = &s_opt[s_focus];
        char t[400];
        if (o->env_set)
            snprintf(t, sizeof t, "%s  (Locked: the %s environment variable is set to \"%s\".)",
                     o->help, o->env, o->env_set);
        else if (row_disabled(s_focus) && !o->soon)
            snprintf(t, sizeof t, "%s  (Only used by the Vulkan renderer.)", o->help);
        else
            snprintf(t, sizeof t, "%s", o->help);
        text_wrap(&s_body, 58, 592, WIN_W - 116, 22, t, C_MUTED);
    } else {
        char t[1100];
        snprintf(t, sizeof t, "Settings are saved to %s", s_path);
        text_wrap(&s_body, 58, 592, WIN_W - 116, 22, t, C_DIM);
    }

    fill(48, 674, WIN_W - 96, 1, C_LINE);
    {
        float hx = 48;
        keycap(&hx, 646, "Enter", "start");
        keycap(&hx, 646, "Esc", "quit");
        keycap(&hx, 646, "Arrows", "choose and change");
        keycap(&hx, 646, "A / Start", "on a controller");
    }
    draw_button(B_DEFAULTS, 48, 696, 130, "Defaults", 0);
    draw_button(B_QUIT, WIN_W - 48 - 190 - 12 - 110, 696, 110, "Quit", 0);
    draw_button(B_START, WIN_W - 48 - 190, 696, 190, "Start game", 1);
}

static void change(int i, int d)
{
    Opt *o = &s_opt[i];
    if (i >= O_COUNT || row_disabled(i) || o->n < 2)
        return;
    o->cur = (o->cur + d + o->n) % o->n;
}

static void move_focus(int d)
{
    if (d > 0)
        s_focus = s_focus < O_COUNT - 1 ? s_focus + 1 : s_focus < O_COUNT ? B_START
                : s_focus == B_START ? 0 : B_START;
    else
        s_focus = s_focus == 0 ? B_START : s_focus >= O_COUNT ? O_COUNT - 1 : s_focus - 1;
}

/* Left/right: values on a row, the next button along the footer. */
static void side(int d)
{
    if (s_focus < O_COUNT) {
        change(s_focus, d);
        return;
    }
    s_focus += d;
    if (s_focus < B_DEFAULTS) s_focus = B_DEFAULTS;
    if (s_focus > B_START) s_focus = B_START;
}

static void defaults(void)
{
    int i;
    for (i = 0; i < O_COUNT; i++)
        if (!s_opt[i].env_set)
            s_opt[i].cur = s_opt[i].def;
}

/* 1 start, 0 quit, -1 nothing yet. */
static int activate(int f)
{
    if (f == B_START)
        return 1;
    if (f == B_QUIT)
        return 0;
    if (f == B_DEFAULTS)
        defaults();
    else
        change(f, 1);
    return -1;
}

static int hit(float x, float y)
{
    int i;
    for (i = 0; i < O_COUNT; i++)
        if (in_box(&s_row_box[i], x, y))
            return i;
    for (i = 0; i < 3; i++)
        if (in_box(&s_btn_box[i], x, y))
            return B_DEFAULTS + i;
    return -1;
}

static int launcher_window(void)
{
    SDL_Window *win;
    SDL_GameController *pads[8] = { 0 };
    int result = -1, ow, oh, ww, wh, i;
    Uint32 stick_next = 0;
    int stick_dir = 0;

    SDL_SetMainReady();
    /* Windows: sharp on scaled displays, laid out in points like the other
     * hosts (the renderer's output size then carries the scale). */
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    SDL_SetHint("SDL_WINDOWS_DPI_SCALING", "1");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[LAUNCHER] SDL init failed (%s) -- starting the game\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    win = SDL_CreateWindow("RalliSport Challenge 2", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_ALLOW_HIGHDPI);
    s_ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED
                                              | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (win && !s_ren)
        s_ren = SDL_CreateRenderer(win, -1, 0);
    if (!s_ren) {
        fprintf(stderr, "[LAUNCHER] no window (%s) -- starting the game\n", SDL_GetError());
        if (win)
            SDL_DestroyWindow(win);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
        return 1;
    }
    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_BLEND);
    SDL_GetRendererOutputSize(s_ren, &ow, &oh);
    SDL_GetWindowSize(win, &ww, &wh);
    s_px = ww > 0 ? (float)ow / (float)ww : 1.0f;
    if (!font_init(&s_body, k_font_Regular, 16) || !font_init(&s_bold, k_font_Bold, 16)
            || !font_init(&s_title, k_font_Bold, 32) || !font_init(&s_small, k_font_Regular, 13)
            || !font_init(&s_smallb, k_font_Bold, 12)) {
        fprintf(stderr, "[LAUNCHER] font setup failed -- starting the game\n");
        result = 1;
    }
    if (getenv("RSC2_LAUNCHER_FOCUS"))   /* screenshots of a focused row */
        s_focus = atoi(getenv("RSC2_LAUNCHER_FOCUS"));
    for (i = 0; i < SDL_NumJoysticks() && i < 8; i++)
        if (SDL_IsGameController(i))
            pads[i] = SDL_GameControllerOpen(i);

    while (result < 0) {
        SDL_Event ev;
        SDL_SetRenderDrawColor(s_ren, C_BG0.r, C_BG0.g, C_BG0.b, 255);
        SDL_RenderClear(s_ren);
        draw();
        if (getenv("RSC2_LAUNCHER_SHOT")) {
            /* RSC2_LAUNCHER_SHOT=<file.bmp>: the first frame, then quit. */
            SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32,
                                                             SDL_PIXELFORMAT_ARGB8888);
            if (sf && SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888,
                                           sf->pixels, sf->pitch) == 0)
                SDL_SaveBMP(sf, getenv("RSC2_LAUNCHER_SHOT"));
            SDL_FreeSurface(sf);
            result = getenv("RSC2_LAUNCHER_SHOT_GO") ? 1 : 0;  /* then boot */
            break;
        }
        SDL_RenderPresent(s_ren);
        if (!SDL_WaitEventTimeout(&ev, 50)) {
            /* Held stick: repeat. */
            if (stick_dir && SDL_GetTicks() >= stick_next) {
                if (stick_dir & 1) move_focus(stick_dir == 1 ? -1 : 1);
                else side(stick_dir == 2 ? -1 : 1);
                stick_next = SDL_GetTicks() + 140;
            }
            continue;
        }
        do {
            switch (ev.type) {
            case SDL_QUIT:
                result = 0;
                break;
            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                case SDLK_UP:     move_focus(-1); break;
                case SDLK_DOWN:   move_focus(1); break;
                case SDLK_TAB:    move_focus(ev.key.keysym.mod & KMOD_SHIFT ? -1 : 1); break;
                case SDLK_LEFT:   side(-1); break;
                case SDLK_RIGHT:  side(1); break;
                case SDLK_SPACE:  result = activate(s_focus); break;
                case SDLK_RETURN: case SDLK_KP_ENTER:
                    result = s_focus >= O_COUNT ? activate(s_focus) : 1;
                    break;
                case SDLK_ESCAPE: result = 0; break;
                default: break;
                }
                break;
            case SDL_MOUSEMOTION:
                s_hover = hit(ev.motion.x, ev.motion.y);
                if (s_hover >= 0 && s_hover < O_COUNT)
                    s_focus = s_hover;
                break;
            case SDL_MOUSEBUTTONDOWN: {
                int h = hit(ev.button.x, ev.button.y);
                if (ev.button.button != SDL_BUTTON_LEFT || h < 0)
                    break;
                s_focus = h;
                if (h < O_COUNT) {
                    const Box *b = &s_row_box[h];
                    /* The value's left half steps back, the rest forward. */
                    change(h, ev.button.x < b->x + COL_W - 126 && ev.button.x >= b->x + COL_W - 236
                              ? -1 : 1);
                } else {
                    result = activate(h);
                }
                break;
            }
            case SDL_MOUSEWHEEL:
                if (s_hover >= 0 && s_hover < O_COUNT)
                    change(s_hover, ev.wheel.y > 0 ? -1 : 1);
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (ev.cdevice.which < 8 && !pads[ev.cdevice.which])
                    pads[ev.cdevice.which] = SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                switch (ev.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP:    move_focus(-1); break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  move_focus(1); break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  side(-1); break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: side(1); break;
                case SDL_CONTROLLER_BUTTON_A:          result = activate(s_focus); break;
                case SDL_CONTROLLER_BUTTON_B:          s_focus = B_QUIT; break;
                case SDL_CONTROLLER_BUTTON_START:      result = 1; break;
                default: break;
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX
                        || ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                    int y = ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY;
                    int d = ev.caxis.value < -20000 ? -1 : ev.caxis.value > 20000 ? 1 : 0;
                    /* 1 up, 3 down, 2 left, 4 right; 0 released. */
                    int dir = d ? (y ? (d < 0 ? 1 : 3) : (d < 0 ? 2 : 4)) : 0;
                    if (!d && ((stick_dir & 1) != y))
                        break;
                    if (dir && dir != stick_dir) {
                        if (y) move_focus(d);
                        else side(d);
                        stick_next = SDL_GetTicks() + 380;
                    }
                    stick_dir = dir;
                }
                break;
            default:
                break;
            }
        } while (result < 0 && SDL_PollEvent(&ev));
    }

    for (i = 0; i < 8; i++)
        if (pads[i])
            SDL_GameControllerClose(pads[i]);
    {
        Font *fs[5] = { &s_body, &s_bold, &s_title, &s_small, &s_smallb };
        for (i = 0; i < 5; i++)
            if (fs[i]->tex)
                SDL_DestroyTexture(fs[i]->tex);
    }
    SDL_DestroyRenderer(s_ren);
    s_ren = NULL;
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    return result;
}

#endif /* RSC2_LAUNCHER_SDL */

/* ── entry ────────────────────────────────────────────────── */

int rsc2_launcher(int argc, char **argv)
{
    const char *force = getenv("RSC2_LAUNCHER"), *fb = getenv("RECOMP_FB_WINDOW");
    const char *replay = getenv("RSC2_INPUT_REPLAY");
    int show, i, headless = (fb && strcmp(fb, "0") == 0) || (replay && *replay);

    show = !headless;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--launcher") == 0)
            show = 2;
        else if (strcmp(argv[i], "--no-launcher") == 0)
            show = 0;
    }
    if (force && *force)
        show = strcmp(force, "0") != 0 ? 2 : 0;

    /* Listing the GPUs makes a Vulkan instance; only worth it for the window. */
    opts_init(show != 0);
    settings_load();
    for (i = 0; i < O_COUNT; i++) {
        const char *v = s_opt[i].env ? getenv(s_opt[i].env) : NULL;
        if (v && *v)
            s_opt[i].env_set = v;
    }
    if (show == 1 && strcmp(s_opt[O_SHOW].val[s_opt[O_SHOW].cur], "0") == 0)
        show = 0;

    if (show) {
        int r = launcher_window();
        if (r == 0) {
            settings_save();
            return 0;
        }
        settings_save();
    }
    settings_apply();
    return 1;
}
