/*
 * wl_color -- the window's colour description, set by us (Wayland only).
 *
 * KWin composites every frame of a window whose description differs from
 * the screen's blending space (colour conversion, and its tonemapper when
 * the content's peak lies above the screen's). A window that takes the
 * compositor's own preferred description over has nothing to convert.
 * nv2a_vk.c does that for HDR when the swapchain's colour space is
 * PASS_THROUGH, so the Vulkan driver leaves the description to us.
 *
 * libwayland-client is loaded at run time: X11 sessions, Windows and
 * Android builds see wlc_attach() fail and nothing else changes.
 */
#ifndef XBOXRECOMP_WL_COLOR_H
#define XBOXRECOMP_WL_COLOR_H

/* What the description says frames hold: transfer function `tf`
 * (WLC_TF_*), BT.2020 primaries if `bt2020`, else BT.709, luminance from
 * `lmin` to `lmax` nits, white (the reference) at `ref`. */
enum { WLC_TF_GAMMA22 = 1, WLC_TF_PQ = 2, WLC_TF_SRGB = 3 };
typedef struct {
    int tf, bt2020;
    float lmin, lmax, ref;
} WlcDesc;

/* The window: SDL's wl_display and wl_surface. 0 when the session is not
 * Wayland, libwayland-client is missing or the compositor has no colour
 * management. */
int  wlc_attach(void *wl_display, void *wl_surface);

/* Fetch the compositor's preferred description for the window and set it.
 * 1 and `*d` filled when it was set (and is one wlc knows how to encode). */
int  wlc_take_preferred(WlcDesc *d);

/* At each present: 1 and `*d` filled when the compositor changed its
 * preference (the brightness slider, another screen) and wlc set the new
 * one. */
int  wlc_poll(WlcDesc *d);

/* Hand the description back to the Vulkan driver (an HDR10/scRGB or SDR
 * swapchain next). */
void wlc_release(void);

#endif
