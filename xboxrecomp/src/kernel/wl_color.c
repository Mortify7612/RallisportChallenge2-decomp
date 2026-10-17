/*
 * wl_color.c -- the window's colour description, set by us (wl_color.h).
 *
 * wp_color_management_v1 on SDL's wl_display, on an event queue of our own:
 * the surface feedback names the compositor's preferred description, and
 * that same object is set on the window, so the compositor sees frames in
 * its blending space. libwayland-client is dlopen()ed; the protocol code is
 * wayland-scanner's (wp_color_management_v1.[ch]), its wl_proxy calls
 * routed through the pointers below.
 */
#include "wl_color.h"

#if defined(__linux__) && !defined(__ANDROID__) && defined(__has_include)
#if __has_include(<wayland-client.h>)
#define WLC_HAVE 1
#endif
#endif

#ifndef WLC_HAVE

int  wlc_attach(void *d, void *s) { (void)d; (void)s; return 0; }
int  wlc_take_preferred(WlcDesc *d) { (void)d; return 0; }
int  wlc_poll(WlcDesc *d) { (void)d; return 0; }
void wlc_release(void) { }

#else

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

static struct {
    struct wl_proxy *(*marshal_flags)(struct wl_proxy *, uint32_t, const struct wl_interface *,
                                      uint32_t, uint32_t, ...);
    int (*add_listener)(struct wl_proxy *, void (**)(void), void *);
    uint32_t (*get_version)(struct wl_proxy *);
    void (*destroy)(struct wl_proxy *);
    void *(*create_wrapper)(void *);
    void (*wrapper_destroy)(void *);
    void (*set_queue)(struct wl_proxy *, struct wl_event_queue *);
    struct wl_event_queue *(*create_queue)(struct wl_display *);
    int (*roundtrip_queue)(struct wl_display *, struct wl_event_queue *);
    int (*dispatch_queue_pending)(struct wl_display *, struct wl_event_queue *);
    const struct wl_interface *registry_iface;
} W;

/* The generated inlines call these by name. */
#define wl_proxy_marshal_flags W.marshal_flags
#define wl_proxy_add_listener  W.add_listener
#define wl_proxy_get_version   W.get_version
#define wl_proxy_destroy       W.destroy
#include "wp_color_management_v1.h"
#include "wp_color_management_v1.c"

static struct wl_display *s_dpy;
static struct wl_surface *s_surf;
static struct wl_event_queue *s_q;
static struct wp_color_manager_v1 *s_cm;
static struct wp_color_management_surface_feedback_v1 *s_fb;
static struct wp_color_management_surface_v1 *s_cs;     /* ours: we set it */
static int s_changed;           /* the preference changed since we took it */

/* One description being fetched. */
static struct {
    int ready, failed, done;
    int tf, prim;
    uint32_t lmin, lmax, ref;   /* lmin in 0.0001 nits */
} D;

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)data; (void)version;
    if (!strcmp(iface, wp_color_manager_v1_interface.name) && !s_cm)
        s_cm = (struct wp_color_manager_v1 *)W.marshal_flags(
            (struct wl_proxy *)r, WL_REGISTRY_BIND, &wp_color_manager_v1_interface, 1, 0,
            name, wp_color_manager_v1_interface.name, 1, NULL);
}
static void reg_remove(void *data, struct wl_registry *r, uint32_t name)
{
    (void)data; (void)r; (void)name;
}
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void cm_intent(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; (void)v; }
static void cm_feature(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; (void)v; }
static void cm_tf(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; (void)v; }
static void cm_prim(void *d, struct wp_color_manager_v1 *m, uint32_t v) { (void)d; (void)m; (void)v; }
static void cm_done(void *d, struct wp_color_manager_v1 *m) { (void)d; (void)m; }
static const struct wp_color_manager_v1_listener cm_l = {
    .supported_intent = cm_intent, .supported_feature = cm_feature,
    .supported_tf_named = cm_tf, .supported_primaries_named = cm_prim, .done = cm_done,
};

static void fb_changed(void *d, struct wp_color_management_surface_feedback_v1 *f, uint32_t id)
{
    (void)d; (void)f; (void)id;
    s_changed = 1;
}
static void fb_changed2(void *d, struct wp_color_management_surface_feedback_v1 *f,
                        uint32_t hi, uint32_t lo)
{
    (void)d; (void)f; (void)hi; (void)lo;
    s_changed = 1;
}
static const struct wp_color_management_surface_feedback_v1_listener fb_l = {
    .preferred_changed = fb_changed, .preferred_changed2 = fb_changed2,
};

static void id_failed(void *d, struct wp_image_description_v1 *x, uint32_t cause, const char *msg)
{
    (void)d; (void)x; (void)cause;
    fprintf(stderr, "  [WLC] preferred description failed: %s\n", msg ? msg : "?");
    D.failed = 1;
}
static void id_ready(void *d, struct wp_image_description_v1 *x, uint32_t id)
{
    (void)d; (void)x; (void)id;
    D.ready = 1;
}
static void id_ready2(void *d, struct wp_image_description_v1 *x, uint32_t hi, uint32_t lo)
{
    (void)d; (void)x; (void)hi; (void)lo;
    D.ready = 1;
}
static const struct wp_image_description_v1_listener id_l = {
    .failed = id_failed, .ready = id_ready, .ready2 = id_ready2,
};

static void in_done(void *d, struct wp_image_description_info_v1 *i) { (void)d; (void)i; D.done = 1; }
static void in_icc(void *d, struct wp_image_description_info_v1 *i, int32_t fd, uint32_t size)
{
    (void)d; (void)i; (void)size;
    close(fd);
    D.tf = -1;                  /* an ICC profile: not ours to encode */
}
static void in_prim(void *d, struct wp_image_description_info_v1 *i, int32_t rx, int32_t ry,
                    int32_t gx, int32_t gy, int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
    (void)d; (void)i; (void)rx; (void)ry; (void)gx; (void)gy; (void)bx; (void)by; (void)wx; (void)wy;
}
static void in_prim_named(void *d, struct wp_image_description_info_v1 *i, uint32_t p)
{
    (void)d; (void)i;
    D.prim = (int)p;
}
static void in_tf_power(void *d, struct wp_image_description_info_v1 *i, uint32_t e)
{
    (void)d; (void)i; (void)e;
    D.tf = -1;
}
static void in_tf_named(void *d, struct wp_image_description_info_v1 *i, uint32_t tf)
{
    (void)d; (void)i;
    if (D.tf != -1)
        D.tf = (int)tf;
}
static void in_lum(void *d, struct wp_image_description_info_v1 *i, uint32_t mn, uint32_t mx, uint32_t ref)
{
    (void)d; (void)i;
    D.lmin = mn;
    D.lmax = mx;
    D.ref = ref;
}
static void in_tprim(void *d, struct wp_image_description_info_v1 *i, int32_t rx, int32_t ry,
                     int32_t gx, int32_t gy, int32_t bx, int32_t by, int32_t wx, int32_t wy)
{
    (void)d; (void)i; (void)rx; (void)ry; (void)gx; (void)gy; (void)bx; (void)by; (void)wx; (void)wy;
}
static void in_tlum(void *d, struct wp_image_description_info_v1 *i, uint32_t mn, uint32_t mx)
{
    (void)d; (void)i; (void)mn; (void)mx;
}
static void in_cll(void *d, struct wp_image_description_info_v1 *i, uint32_t v) { (void)d; (void)i; (void)v; }
static void in_fall(void *d, struct wp_image_description_info_v1 *i, uint32_t v) { (void)d; (void)i; (void)v; }
static const struct wp_image_description_info_v1_listener in_l = {
    .done = in_done, .icc_file = in_icc, .primaries = in_prim, .primaries_named = in_prim_named,
    .tf_power = in_tf_power, .tf_named = in_tf_named, .luminances = in_lum,
    .target_primaries = in_tprim, .target_luminance = in_tlum,
    .target_max_cll = in_cll, .target_max_fall = in_fall,
};

static int load(void)
{
    static int tried, ok;
    void *h;
    if (tried++)
        return ok;
    h = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return 0;
#define SYM(field, name) (*(void **)&W.field = dlsym(h, name))
    SYM(marshal_flags, "wl_proxy_marshal_flags");
    SYM(add_listener, "wl_proxy_add_listener");
    SYM(get_version, "wl_proxy_get_version");
    SYM(destroy, "wl_proxy_destroy");
    SYM(create_wrapper, "wl_proxy_create_wrapper");
    SYM(wrapper_destroy, "wl_proxy_wrapper_destroy");
    SYM(set_queue, "wl_proxy_set_queue");
    SYM(create_queue, "wl_display_create_queue");
    SYM(roundtrip_queue, "wl_display_roundtrip_queue");
    SYM(dispatch_queue_pending, "wl_display_dispatch_queue_pending");
    W.registry_iface = (const struct wl_interface *)dlsym(h, "wl_registry_interface");
#undef SYM
    ok = W.marshal_flags && W.add_listener && W.get_version && W.destroy && W.create_wrapper
      && W.wrapper_destroy && W.set_queue && W.create_queue && W.roundtrip_queue
      && W.dispatch_queue_pending && W.registry_iface;
    return ok;
}

int wlc_attach(void *wl_display, void *wl_surface)
{
    struct wl_proxy *wrap, *reg;
    if (s_cm)
        return 1;
    if (!wl_display || !wl_surface || !load())
        return 0;
    s_dpy = (struct wl_display *)wl_display;
    s_surf = (struct wl_surface *)wl_surface;
    s_q = W.create_queue(s_dpy);
    wrap = (struct wl_proxy *)W.create_wrapper(s_dpy);
    if (!s_q || !wrap)
        return 0;
    W.set_queue(wrap, s_q);
    reg = W.marshal_flags(wrap, WL_DISPLAY_GET_REGISTRY, W.registry_iface,
                          W.get_version(wrap), 0, NULL);
    W.wrapper_destroy(wrap);
    if (!reg)
        return 0;
    W.add_listener(reg, (void (**)(void))&reg_l, NULL);
    W.roundtrip_queue(s_dpy, s_q);
    W.destroy(reg);
    if (!s_cm)
        return 0;
    wp_color_manager_v1_add_listener(s_cm, &cm_l, NULL);
    s_fb = wp_color_manager_v1_get_surface_feedback(s_cm, s_surf);
    wp_color_management_surface_feedback_v1_add_listener(s_fb, &fb_l, NULL);
    W.roundtrip_queue(s_dpy, s_q);
    return 1;
}

/* The preferred description as `*d`, set on the window if `*d` is one we
 * encode. */
static int take(WlcDesc *d)
{
    struct wp_image_description_v1 *desc;
    struct wp_image_description_info_v1 *info;
    int ok;
    memset(&D, 0, sizeof D);
    s_changed = 0;
    desc = wp_color_management_surface_feedback_v1_get_preferred(s_fb);
    wp_image_description_v1_add_listener(desc, &id_l, NULL);
    while (!D.ready && !D.failed)
        if (W.roundtrip_queue(s_dpy, s_q) < 0)
            break;
    if (!D.ready) {
        wp_image_description_v1_destroy(desc);
        return 0;
    }
    info = wp_image_description_v1_get_information(desc);
    wp_image_description_info_v1_add_listener(info, &in_l, NULL);
    while (!D.done)
        if (W.roundtrip_queue(s_dpy, s_q) < 0)
            break;
    /* (The info object is destroyed by the compositor after done.) */
    W.destroy((struct wl_proxy *)info);
    ok = D.done && D.lmax > 0 && D.ref > 0
      && (D.tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22
          || D.tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ
          || D.tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB)
      && (D.prim == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020
          || D.prim == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
    if (ok) {
        if (!s_cs)
            s_cs = wp_color_manager_v1_get_surface(s_cm, s_surf);
        /* Applied with the driver's next commit, the next present. */
        wp_color_management_surface_v1_set_image_description(
            s_cs, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
        d->tf = D.tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22 ? WLC_TF_GAMMA22
              : D.tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ ? WLC_TF_PQ : WLC_TF_SRGB;
        d->bt2020 = D.prim == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
        d->lmin = (float)D.lmin * 1e-4f;
        d->lmax = (float)D.lmax;
        d->ref = (float)D.ref;
        /* PQ is absolute: 0-10000 nits whatever luminances says. */
        if (d->tf == WLC_TF_PQ) {
            d->lmin = 0.0f;
            d->lmax = 10000.0f;
        }
    }
    wp_image_description_v1_destroy(desc);
    return ok;
}

int wlc_take_preferred(WlcDesc *d)
{
    if (!s_fb)
        return 0;
    return take(d);
}

int wlc_poll(WlcDesc *d)
{
    if (!s_cs)
        return 0;
    W.dispatch_queue_pending(s_dpy, s_q);
    if (!s_changed)
        return 0;
    return take(d);
}

void wlc_release(void)
{
    if (!s_cs)
        return;
    wp_color_management_surface_v1_destroy(s_cs);
    s_cs = NULL;
}

#endif
