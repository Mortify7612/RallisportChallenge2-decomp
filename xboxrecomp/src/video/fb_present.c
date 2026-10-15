/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* The Vulkan renderer (kernel/nv2a_vk.c): when it is up, it presents into
 * this window itself and GDI stands down. RECOMP_VK_PRESENT=0 keeps GDI. */
extern void *nv2a_vk_instance_handle(void);
extern int   nv2a_vk_attach_hwnd(void *hinstance, void *hwnd);
extern void  nv2a_vk_set_window_size(int w, int h);
static volatile LONG s_vk_window;

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
}

/* Which keys are down, for the pad stand-in in src/input.
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them.
 *
 * Reading this needs no lock. Each entry is written only by the window
 * thread and read only by the USB thread, one byte at a time, and a press
 * seen a frame late is indistinguishable from one made a frame later. */
static volatile unsigned char s_key_down[256];

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        InterlockedExchange(&s_fb_running, 0);
        return 0;

    case WM_SIZE:
        if (s_vk_window)
            nv2a_vk_set_window_size(LOWORD(l), HIWORD(l));
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((unsigned)w < 256)
            s_key_down[w] = 1;
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if ((unsigned)w < 256)
            s_key_down[w] = 0;
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        memset((void *)s_key_down, 0, sizeof s_key_down);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    {
        const char *vp = getenv("RECOMP_VK_PRESENT");
        if (!(vp && strcmp(vp, "0") == 0) && nv2a_vk_instance_handle())
            s_vk_window = 1;
    }
    r.left = 0; r.top = 0;
    r.right = (LONG)s_fb_width * (s_vk_window ? 2 : 1);
    r.bottom = (LONG)s_fb_height * (s_vk_window ? 2 : 1);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);
    if (s_vk_window) {
        RECT cr;
        GetClientRect(hwnd, &cr);
        nv2a_vk_set_window_size(cr.right - cr.left, cr.bottom - cr.top);
        if (!nv2a_vk_attach_hwnd(GetModuleHandleA(NULL), hwnd)) {
            fprintf(stderr, "  [FBWIN] Vulkan surface failed -- GDI\n");
            s_vk_window = 0;
        } else {
            fprintf(stderr, "  [FBWIN] window open, presented by Vulkan\n");
        }
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            if (!s_vk_window)
                StretchDIBits(hdc, 0, 0, (int)s_fb_width, (int)s_fb_height,
                              0, 0, (int)s_fb_width, (int)s_fb_height,
                              s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else if (s_fb_va && s_fb_pitch && s_rgb && !s_vk_window) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            StretchDIBits(hdc, 0, 0, (int)s_fb_width, (int)s_fb_height,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        Sleep(16);
    }

    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#else /* !_WIN32 -- SDL2 */

/* The same window on POSIX hosts, through SDL2.
 *
 * Everything the Win32 version promises holds here too: frames are copied at
 * the flip and the window only ever shows a finished one; key state is kept
 * per Win32 virtual-key code, so src/input's keyboard mapping needs no second
 * copy; the title bar carries the title name, FPS and draw count; and
 * RECOMP_FB_DUMP / RECOMP_FB_WINDOW_DUMP_EVERY dump what the window shows.
 *
 * Additions a desktop window wants: it scales to whatever size it is dragged
 * to (aspect kept, nearest-neighbour off), F11 toggles fullscreen, and closing
 * it ends the process -- there is nothing left to look at, and a title with
 * no window would otherwise keep running headless until killed.
 *
 * All SDL video calls stay on the window thread. */
#include <SDL.h>
#include <SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* The Vulkan renderer (kernel/nv2a_vk.c), when it is up: the window is then
 * a Vulkan surface it presents to, at its own resolution, and this thread
 * only runs the window. RECOMP_VK_PRESENT=0 keeps the SDL renderer. */
extern void *nv2a_vk_instance_handle(void);
extern void  nv2a_vk_set_surface(uint64_t surface);
extern void  nv2a_vk_set_window_size(int w, int h);

static volatile int  s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;
static uint32_t     *s_present[2];
static volatile int  s_present_idx = -1;
static volatile unsigned char s_key_down[256];
static char          s_title[48] = "Xbox Recomp";
static volatile int  s_flips, s_frame_draws;

static void fb_convert_rows(uint32_t *dst_base, const uint8_t *src, uint32_t pitch)
{
    uint32_t bpp = pitch / s_fb_width, x, y;
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = dst_base + (size_t)y * s_fb_width;
        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *q = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = q[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    const char *pin = getenv("RECOMP_FB_VA");
    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    int next;
    if (!s_fb_running || !fb_va || !pitch || getenv("RECOMP_FB_VA"))
        return;
    next = (__atomic_load_n(&s_present_idx, __ATOMIC_ACQUIRE) == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
        if (!s_present[next])
            return;
    }
    fb_convert_rows(s_present[next],
                    (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset()),
                    pitch);
    __atomic_store_n(&s_present_idx, next, __ATOMIC_RELEASE);  /* whole frame */
}

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (char)(name[i] < 0x80 ? name[i] : '?');
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    __atomic_add_fetch(&s_flips, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_frame_draws, (int)draws, __ATOMIC_RELAXED);
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

/* SDL key -> Win32 virtual-key code: letters and digits are their uppercase
 * ASCII, the rest by table. 0 for keys with no counterpart. */
static int sdl_to_vk(SDL_Keycode k)
{
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (int)(k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (int)(k - SDLK_0);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 0x61 + (int)(k - SDLK_KP_1);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (int)(k - SDLK_F1);
    switch (k) {
    case SDLK_KP_0:      return 0x60;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return 0x0D;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB:       return 0x09;
    case SDLK_ESCAPE:    return 0x1B;
    case SDLK_SPACE:     return 0x20;
    case SDLK_LEFT:      return 0x25;
    case SDLK_UP:        return 0x26;
    case SDLK_RIGHT:     return 0x27;
    case SDLK_DOWN:      return 0x28;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL:  case SDLK_RCTRL:  return 0x11;
    case SDLK_LALT:   case SDLK_RALT:   return 0x12;
    default:             return 0;
    }
}

int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);
    line = (uint8_t *)calloc(1, row);
    for (y = 0; line && y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u)\n", path, s_fb_width, s_fb_height);
    return 0;
}

static int sdl_to_vk(SDL_Keycode k);

/* RECOMP_WIDESCREEN=1: the title draws 4:3-sized frames anamorphic, shown
 * at 16:9. */
static int fb_wide(void)
{
    const char *v = getenv("RECOMP_WIDESCREEN");
    return v && *v && strcmp(v, "0") != 0 && s_fb_width * 3 == s_fb_height * 4;
}

/* RECOMP_VIDEO_RECORD=<file>: every frame the title presents, at its own
 * resolution, piped to ffmpeg (H.264 in Matroska, which stays playable if
 * the process ends without closing it). Written from this thread when a new
 * flip arrives, so a frame this loop misses is missing from the video too. */
static FILE *s_video, *s_video_idx;
extern uint32_t nv2a_pb_presented_frames(void);
static void video_frame(const uint32_t *px)
{
    static int init;
    if (!init) {
        const char *path = getenv("RECOMP_VIDEO_RECORD");
        init = 1;
        if (path && *path) {
            char cmd[1024];
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgr0 -s %ux%u -r 60"
                     " -i - -c:v libx264 -preset ultrafast -crf 16 -pix_fmt yuv420p%s"
                     " -f matroska '%s'", s_fb_width, s_fb_height,
                     fb_wide() ? " -aspect 16:9" : "", path);
            s_video = popen(cmd, "w");
            {
                /* <file>.frames: the title's frame number of each video
                 * frame, for lining the video up with frame-numbered logs. */
                char ip[1024];
                snprintf(ip, sizeof ip, "%s.frames", path);
                s_video_idx = fopen(ip, "w");
            }
            fprintf(stderr, "  [FBWIN] recording video to %s%s\n", path,
                    s_video ? "" : " -- ffmpeg failed to start");
        }
    }
    if (s_video && s_video_idx) {
        fprintf(s_video_idx, "%u\n", nv2a_pb_presented_frames());
        fflush(s_video_idx);
    }
    if (s_video && fwrite(px, 4, (size_t)s_fb_width * s_fb_height, s_video)
                       != (size_t)s_fb_width * s_fb_height) {
        pclose(s_video);
        s_video = NULL;
    }
}

static void video_close(void)
{
    if (s_video) {
        pclose(s_video);
        s_video = NULL;
    }
}

/* Host game controllers (src/input/xinput_device.c): opened, closed and
 * read on this thread, which is the one pumping SDL's events. */
extern void xbox_InputSdlInit(void);
extern void xbox_InputSdlEvent(const SDL_Event *ev);
extern void xbox_InputSdlPoll(void);

/* One round of window events; returns the window's new drawable size
 * through *resized when it changed. */
static void fb_events(SDL_Window *win, int *resized)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_CONTROLLERBUTTONDOWN
                && (ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE
                    || ev.cbutton.button == SDL_CONTROLLER_BUTTON_MISC1)) {
            /* The controller's Guide button: the same mark as F9. */
            extern volatile int g_recomp_user_mark;
            extern void xbox_FlightDumpRequest(void);
            g_recomp_user_mark++;
            xbox_FlightDumpRequest();
        }
        if (ev.type == SDL_QUIT) {
            fprintf(stderr, "  [FBWIN] window closed -- exiting\n");
            fflush(stderr);
            video_close();
            _Exit(0);
        } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            int vk = sdl_to_vk(ev.key.keysym.sym);
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F10
                    && !ev.key.repeat) {
                /* The renderer's flight recorder (RECOMP_VK_FLIGHT). */
                extern void xbox_FlightDumpRequest(void);
                xbox_FlightDumpRequest();
            }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F9
                    && !ev.key.repeat) {
                /* A mark in the tree trace (RECOMP_TREE_TRACE), and the
                 * flight recorder's last frames with it. */
                extern volatile int g_recomp_user_mark;
                extern void xbox_FlightDumpRequest(void);
                g_recomp_user_mark++;
                xbox_FlightDumpRequest();
            }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F11
                    && !ev.key.repeat) {
                Uint32 fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
            if (vk) {
                s_key_down[vk] = ev.type == SDL_KEYDOWN;
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat
                        && getenv("RECOMP_KEY_TRACE"))
                    fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)vk);
            }
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED
                   || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            xbox_InputSdlEvent(&ev);
        } else if (ev.type == SDL_WINDOWEVENT
                   && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            /* Leaving with a key held would leave it held for ever. */
            memset((void *)s_key_down, 0, sizeof s_key_down);
        } else if (ev.type == SDL_WINDOWEVENT
                   && (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED
                       || ev.window.event == SDL_WINDOWEVENT_RESIZED)) {
            if (resized)
                *resized = 1;
        }
    }
    xbox_InputSdlPoll();
}

/* The window while Vulkan presents into it: events, the title bar and the
 * dumps (from the frame copied at the flip, as before). */
static void *fb_vk_loop(SDL_Window *win)
{
    Uint32 t0 = 0;
    int f0 = 0, frames = 0;
    const char *dump = getenv("RECOMP_FB_DUMP");
    const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
    int period = every ? atoi(every) : 0;

    while (s_fb_running) {
        int resized = 0, idx;
        fb_events(win, &resized);
        if (resized) {
            int dw, dh;
            SDL_Vulkan_GetDrawableSize(win, &dw, &dh);
            nv2a_vk_set_window_size(dw, dh);
        }
        idx = __atomic_load_n(&s_present_idx, __ATOMIC_ACQUIRE);
        {
            /* Only when a new frame came in: this loop now runs every 4 ms. */
            static int copied_flip = -1;
            int f = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
            if (idx >= 0 && s_present[idx] && f != copied_flip) {
                memcpy(s_rgb, s_present[idx], (size_t)s_fb_width * s_fb_height * 4);
                copied_flip = f;
                video_frame(s_rgb);
            }
        }
        frames++;
        if (dump && period > 0 && frames % period == 0)
            xbox_FramebufferDumpBmp(dump);
        {
            Uint32 now = SDL_GetTicks();
            if (now - t0 >= 1000) {
                int f = s_flips;
                char tb[128];
                snprintf(tb, sizeof tb, "%s | FPS: %.1f | Vulkan", s_title,
                         t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0);
                SDL_SetWindowTitle(win, tb);
                t0 = now; f0 = f;
            }
        }
        /* 4 ms: this loop also reads the controllers, and a 16 ms sleep
         * here added up to a frame of input lag. */
        SDL_Delay(4);
    }
    SDL_DestroyWindow(win);
    return NULL;
}

/* RECOMP_FULLSCREEN=1: open fullscreen (desktop resolution); F11 still
 * toggles. */
static Uint32 fb_start_flags(void)
{
    const char *v = getenv("RECOMP_FULLSCREEN");
    return v && *v && strcmp(v, "0") != 0 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
}

static void *fb_thread(void *unused)
{
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    Uint32 t0 = 0;
    int f0 = 0, frames = 0;
    const char *dump = getenv("RECOMP_FB_DUMP");
    const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
    int period = every ? atoi(every) : 0;
    int vsync = 0;

    (void)unused;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "  [FBWIN] SDL video init failed: %s\n", SDL_GetError());
        s_fb_running = 0;
        return NULL;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    xbox_InputSdlInit();
    {
        const char *vp = getenv("RECOMP_VK_PRESENT");
        void *vkinst = (vp && strcmp(vp, "0") == 0) ? NULL : nv2a_vk_instance_handle();
        if (vkinst) {
            VkSurfaceKHR surf = 0;
            win = SDL_CreateWindow(s_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   fb_wide() ? 1280 : (int)s_fb_width * 2, fb_wide() ? 720 : (int)s_fb_height * 2,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
                                   | SDL_WINDOW_VULKAN | fb_start_flags());
            if (win && SDL_Vulkan_CreateSurface(win, (VkInstance)vkinst, &surf)) {
                int dw, dh;
                SDL_Vulkan_GetDrawableSize(win, &dw, &dh);
                nv2a_vk_set_window_size(dw, dh);
                nv2a_vk_set_surface((uint64_t)(uintptr_t)surf);
                s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
                fprintf(stderr, "  [FBWIN] window open, presented by Vulkan (%dx%d, SDL %s)\n",
                        dw, dh, SDL_GetCurrentVideoDriver());
                return fb_vk_loop(win);
            }
            fprintf(stderr, "  [FBWIN] Vulkan surface failed (%s) -- SDL renderer\n",
                    SDL_GetError());
            if (win)
                SDL_DestroyWindow(win);
        }
    }
    win = SDL_CreateWindow(s_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           fb_wide() ? 1280 : (int)s_fb_width * 2, fb_wide() ? 720 : (int)s_fb_height * 2,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
                           | fb_start_flags());
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED
                                            | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (win && !ren)
        ren = SDL_CreateRenderer(win, -1, 0);
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  (int)s_fb_width, (int)s_fb_height) : NULL;
    if (!tex) {
        fprintf(stderr, "  [FBWIN] window/renderer creation failed: %s\n", SDL_GetError());
        s_fb_running = 0;
        return NULL;
    }
    SDL_RenderSetLogicalSize(ren, fb_wide() ? (int)s_fb_height * 16 / 9 : (int)s_fb_width,
                             (int)s_fb_height);
    {
        SDL_RendererInfo info;
        vsync = SDL_GetRendererInfo(ren, &info) == 0
             && (info.flags & SDL_RENDERER_PRESENTVSYNC);
    }
    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, SDL %s)\n",
            s_fb_width, s_fb_height, SDL_GetCurrentVideoDriver());

    while (s_fb_running) {
        int idx;
        fb_events(win, NULL);

        idx = __atomic_load_n(&s_present_idx, __ATOMIC_ACQUIRE);
        if (idx >= 0 && s_present[idx]) {
            memcpy(s_rgb, s_present[idx], (size_t)s_fb_width * s_fb_height * 4);
        } else if (s_fb_va && s_fb_pitch) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory. */
            fb_convert_rows(s_rgb, (const uint8_t *)((uintptr_t)s_fb_va
                                                     + xbox_GetMemoryOffset()),
                            s_fb_pitch);
        }
        SDL_UpdateTexture(tex, NULL, s_rgb, (int)s_fb_width * 4);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);   /* paced by vsync where available */

        frames++;
        if (dump && period > 0 && frames % period == 0)
            xbox_FramebufferDumpBmp(dump);
        {
            Uint32 now = SDL_GetTicks();
            if (now - t0 >= 1000) {
                int f = s_flips;
                char tb[128];
                snprintf(tb, sizeof tb, "%s | FPS: %.1f | draws: %d", s_title,
                         t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0, s_frame_draws);
                SDL_SetWindowTitle(win, tb);
                t0 = now; f0 = f;
            }
        }
        if (!vsync)
            SDL_Delay(16);
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    return NULL;
}

void xbox_FramebufferWindowStart(void)
{
    pthread_t th;
    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (__atomic_exchange_n(&s_fb_running, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    if (pthread_create(&th, NULL, fb_thread, NULL) == 0)
        pthread_detach(th);
    else
        s_fb_running = 0;
}
#endif
