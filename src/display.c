#include "oops/display.h"
#include "oops/draw.h"
#include "oops/system.h"
#include "oops/target.h"

#if OOPS_TARGET_IS_PROSPERO
#include "agc/display.h"
#include "gnm/display.h"
#else
#include "agc/display.h"
#include "gnm/display.h"
#endif

struct oops_display {
    oops_display_backend_t backend;
    /* A renderer draws the scanout buffers in place (oops_display_use_scanout). */
    int use_scanout;
#if OOPS_TARGET_IS_PROSPERO
    agc_display_t *agc;
#else
    gnm_display_t *gnm;
#endif
};

static struct oops_display s_unified_display;
static int s_opened = 0; /* a backend has been handed out since the last close */

oops_display_t *oops_display_open(oops_display_backend_t backend, unsigned int width,
                                  unsigned int height) {
    return oops_display_open_adopting(backend, width, height, 0, 0);
}

oops_display_t *oops_display_open_adopting(oops_display_backend_t backend,
                                           unsigned int width, unsigned int height,
                                           void *const *adopt, int adopt_count) {
    oops_log_info("DISP", "oops_display_open backend=%d size=%ux%u adopting=%d",
                  (int)backend, width, height, adopt_count);
    struct oops_display *disp = &s_unified_display;
    /* A second open without a close would overwrite the pointer to a live backend
     * and leak it. Closing a backend whose open failed is a no-op, so this is
     * safe on every path. */
    if (s_opened) {
        oops_display_close(disp);
    }
    disp->backend = backend;
    disp->use_scanout = 0;

#if OOPS_TARGET_IS_PROSPERO
    /* Prospero / Trinity native target: AGC graphics */
    if (backend == OOPS_DISPLAY_BACKEND_AUTO || backend == OOPS_DISPLAY_BACKEND_AGC) {
        disp->backend = OOPS_DISPLAY_BACKEND_AGC;
        disp->agc = agc_display_open_adopting(width, height, adopt, adopt_count);
        s_opened = 1;
        return disp;
    }
    /* GNM requested on a Prospero native target: refused to prevent NeoMode linkage */
    disp->agc = (agc_display_t *)0;
    return (oops_display_t *)0;
#else
    /* Orbis / Neo target: GNM graphics */
    if (backend == OOPS_DISPLAY_BACKEND_AUTO || backend == OOPS_DISPLAY_BACKEND_GNM) {
        disp->backend = OOPS_DISPLAY_BACKEND_GNM;
        /* The PS4 backend names no foreign buffers; a caller that asked is told by
         * oops_display_adopted_index returning -1, not by the open failing. */
        (void)adopt;
        (void)adopt_count;
        disp->gnm = gnm_display_open(width, height);
        s_opened = 1;
        return disp;
    }
    /* AGC requested on an Orbis target: unsupported */
    disp->gnm = (gnm_display_t *)0;
    return (oops_display_t *)0;
#endif
}

int oops_display_is_ready(const oops_display_t *disp) {
    if (!disp)
        return 0;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_is_ready(disp->agc);
#else
    return gnm_display_is_ready(disp->gnm);
#endif
}

int oops_display_is_gpu_accelerated(const oops_display_t *disp) {
    if (!disp)
        return 0;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_is_gpu_accelerated(disp->agc);
#else
    return 0;
#endif
}

int oops_display_try_gpu_tiler(oops_display_t *disp) {
    if (!disp)
        return -1;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_try_gpu_tiler(disp->agc);
#else
    /* The GNM backend has no compute tiler; the CPU path is all there is. */
    return 0;
#endif
}

uint32_t *oops_display_get_framebuffer(oops_display_t *disp) {
    if (!disp)
        return (uint32_t *)0;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_get_framebuffer(disp->agc);
#else
    return gnm_display_get_framebuffer(disp->gnm);
#endif
}

unsigned int oops_display_get_width(const oops_display_t *disp) {
    if (!disp)
        return 0;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_get_width(disp->agc);
#else
    return gnm_display_get_width(disp->gnm);
#endif
}

unsigned int oops_display_get_height(const oops_display_t *disp) {
    if (!disp)
        return 0;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_get_height(disp->agc);
#else
    return gnm_display_get_height(disp->gnm);
#endif
}

uint64_t oops_display_get_flip_count(const oops_display_t *disp) {
    if (!disp)
        return 0;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_get_flip_count(disp->agc);
#else
    return gnm_display_get_flip_count(disp->gnm);
#endif
}

int oops_display_get_last_error(const oops_display_t *disp) {
    if (!disp)
        return -1;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_get_last_error(disp->agc);
#else
    return gnm_display_get_last_error(disp->gnm);
#endif
}

int oops_display_get_video_handle(const oops_display_t *disp) {
    if (!disp)
        return -1;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_get_video_handle(disp->agc);
#else
    return gnm_display_get_video_handle(disp->gnm);
#endif
}

const char *oops_display_get_backend_name(const oops_display_t *disp) {
    if (!disp)
        return "none";
#if OOPS_TARGET_IS_PROSPERO
    return (OOPS_TARGET == OOPS_TARGET_TRINITY) ? "AGC (Trinity)" : "AGC (Prospero)";
#else
    return (OOPS_TARGET == OOPS_TARGET_NEO) ? "GNM (Neo)" : "GNM (Orbis)";
#endif
}

void oops_display_clear(oops_display_t *disp, uint32_t color) {
    if (!disp)
        return;
#if OOPS_TARGET_IS_PROSPERO
    agc_display_clear(disp->agc, color);
#else
    gnm_display_clear(disp->gnm, color);
#endif
}

int oops_display_flip(oops_display_t *disp) {
    if (!disp)
        return -1;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_flip(disp->agc);
#else
    return gnm_display_flip(disp->gnm);
#endif
}

int oops_display_present(oops_display_t *disp, const uint32_t *pixels) {
    if (!disp)
        return -1;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_present(disp->agc, pixels);
#else
    return gnm_display_present(disp->gnm, pixels);
#endif
}

int oops_display_read_shown(oops_display_t *disp, uint32_t *pixels) {
    if (!disp)
        return -1;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_read_shown(disp->agc, pixels);
#else
    return gnm_display_read_shown(disp->gnm, pixels);
#endif
}

oops_display_scanout_layout_t oops_display_scanout_layout(const oops_display_t *disp) {
    if (!disp)
        return OOPS_DISPLAY_SCANOUT_NONE;
#if OOPS_TARGET_IS_PROSPERO
    return (oops_display_scanout_layout_t)agc_display_scanout_layout(disp->agc);
#else
    return (oops_display_scanout_layout_t)gnm_display_scanout_layout(disp->gnm);
#endif
}

uint32_t *oops_display_scanout(oops_display_t *disp, int which) {
    if (!disp)
        return (uint32_t *)0;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_scanout(disp->agc, which);
#else
    return gnm_display_scanout(disp->gnm, which);
#endif
}

int oops_display_adopted_index(const oops_display_t *disp, int nth) {
    if (!disp)
        return -1;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_adopted_index(disp->agc, nth);
#else
    /* The PS4 backend names no foreign buffers, so there is never an index to
     * report. A caller that gets -1 keeps whatever copy it was doing, which is
     * what it was doing before this existed. */
    (void)nth;
    return -1;
#endif
}

int oops_display_flip_index(oops_display_t *disp, int index) {
    if (!disp)
        return -1;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_flip_index(disp->agc, index);
#else
    (void)index;
    return -1;
#endif
}

int oops_display_wait_scanout(oops_display_t *disp) {
    if (!disp)
        return -1;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_wait_scanout(disp->agc);
#else
    return gnm_display_wait_scanout(disp->gnm);
#endif
}

int oops_display_flip_scanout(oops_display_t *disp) {
    if (!disp)
        return -1;
#if OOPS_TARGET_IS_PROSPERO
    return agc_display_flip_scanout(disp->agc);
#else
    return gnm_display_flip_scanout(disp->gnm);
#endif
}

oops_display_scanout_layout_t oops_display_use_scanout(oops_display_t *disp) {
    const oops_display_scanout_layout_t layout = oops_display_scanout_layout(disp);
    if (layout != OOPS_DISPLAY_SCANOUT_NONE)
        disp->use_scanout = 1;
    return layout;
}

/* Here rather than with the drawing calls, because which buffer the next flip
 * shows is the display's to know. A program that flips its framebuffer draws
 * on that, linear. A renderer drawing the scanout buffers in place has the next
 * one described in its own layout, so a CPU overlay - a HUD over a GL frame -
 * lands in what is flipped rather than in a framebuffer nothing shows. */
oops_surface_t oops_display_get_surface(oops_display_t *disp) {
    oops_surface_t surf = {NULL, 0, 0, 0, OOPS_SURFACE_LINEAR};
    if (!disp)
        return surf;
    surf.width = oops_display_get_width(disp);
    surf.height = oops_display_get_height(disp);
    if (disp->use_scanout) {
        surf.pixels = oops_display_scanout(disp, 0);
        if (oops_display_scanout_layout(disp) == OOPS_DISPLAY_SCANOUT_RX) {
            surf.layout = OOPS_SURFACE_RX;
            surf.pitch = (surf.width + 127u) & ~127u;
        } else {
            surf.pitch = surf.width;
        }
        return surf;
    }
    surf.pixels = oops_display_get_framebuffer(disp);
    surf.pitch = surf.width;
    return surf;
}

void oops_display_close(oops_display_t *disp) {
    if (!disp)
        return;
    oops_log_info("DISP", "oops_display_close");
#if OOPS_TARGET_IS_PROSPERO
    agc_display_close(disp->agc);
#else
    gnm_display_close(disp->gnm);
#endif
    s_opened = 0;
}

#if OOPS_TARGET_IS_PROSPERO
/* Stubs for the gnm backend when the build targets prospero or trinity
 * (OOPS_TARGET_IS_PROSPERO), so code that calls gnm_* directly still links without
 * that backend compiled in. Weak on purpose: OOPS_SDK_C_SRCS lists every
 * backend, so a consumer following it compiles src/gnm/gnm_display.c alongside
 * this file, and the real definitions there must win rather than collide. */
__attribute__((weak)) gnm_display_t *gnm_display_open(unsigned int width,
                                                      unsigned int height) {
    (void)width;
    (void)height;
    return (gnm_display_t *)0;
}
__attribute__((weak)) int gnm_display_is_ready(const gnm_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) uint32_t *gnm_display_get_framebuffer(gnm_display_t *disp) {
    (void)disp;
    return (uint32_t *)0;
}
__attribute__((weak)) unsigned int gnm_display_get_width(const gnm_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) unsigned int gnm_display_get_height(const gnm_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) uint64_t gnm_display_get_flip_count(const gnm_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) int gnm_display_get_last_error(const gnm_display_t *disp) {
    (void)disp;
    return -1;
}
__attribute__((weak)) int gnm_display_get_video_handle(const gnm_display_t *disp) {
    (void)disp;
    return -1;
}
__attribute__((weak)) void gnm_display_clear(gnm_display_t *disp, uint32_t color) {
    (void)disp;
    (void)color;
}
__attribute__((weak)) int gnm_display_flip(gnm_display_t *disp) {
    (void)disp;
    return -1;
}
__attribute__((weak)) int gnm_display_present(gnm_display_t *disp,
                                              const uint32_t *pixels) {
    (void)disp;
    (void)pixels;
    return -1;
}
__attribute__((weak)) int gnm_display_read_shown(gnm_display_t *disp,
                                                 uint32_t *pixels) {
    (void)disp;
    (void)pixels;
    return -1;
}
__attribute__((weak)) int gnm_display_scanout_layout(const gnm_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) uint32_t *gnm_display_scanout(gnm_display_t *disp, int which) {
    (void)disp;
    (void)which;
    return (uint32_t *)0;
}
__attribute__((weak)) int gnm_display_wait_scanout(gnm_display_t *disp) {
    (void)disp;
    return -1;
}
__attribute__((weak)) int gnm_display_flip_scanout(gnm_display_t *disp) {
    (void)disp;
    return -1;
}
__attribute__((weak)) int gnm_display_set_flip_rate(gnm_display_t *disp,
                                                    unsigned int rate) {
    (void)disp;
    (void)rate;
    return -1;
}
__attribute__((weak)) void gnm_display_close(gnm_display_t *disp) {
    (void)disp;
}
#else
/* Stubs for the agc backend when the build targets orbis or neo
 * (OOPS_TARGET_IS_ORBIS), weak for the same reason: a consumer that compiles
 * src/agc/agc_display.c as well gets the real backend. */
__attribute__((weak)) void agc_display_set_logger(agc_log_fn fn) {
    (void)fn;
}
__attribute__((weak)) agc_display_t *agc_display_open(unsigned int width,
                                                      unsigned int height) {
    (void)width;
    (void)height;
    return (agc_display_t *)0;
}
__attribute__((weak)) int agc_display_is_ready(const agc_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) int agc_display_is_gpu_accelerated(const agc_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) uint32_t *agc_display_get_framebuffer(agc_display_t *disp) {
    (void)disp;
    return (uint32_t *)0;
}
__attribute__((weak)) unsigned int agc_display_get_width(const agc_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) unsigned int agc_display_get_height(const agc_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) uint64_t agc_display_get_flip_count(const agc_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) int agc_display_get_last_error(const agc_display_t *disp) {
    (void)disp;
    return -1;
}
__attribute__((weak)) int agc_display_get_video_handle(const agc_display_t *disp) {
    (void)disp;
    return -1;
}
__attribute__((weak)) void agc_display_clear(agc_display_t *disp, uint32_t color) {
    (void)disp;
    (void)color;
}
__attribute__((weak)) int agc_display_flip(agc_display_t *disp) {
    (void)disp;
    return -1;
}
__attribute__((weak)) int agc_display_present(agc_display_t *disp,
                                              const uint32_t *pixels) {
    (void)disp;
    (void)pixels;
    return -1;
}
__attribute__((weak)) int agc_display_read_shown(agc_display_t *disp,
                                                 uint32_t *pixels) {
    (void)disp;
    (void)pixels;
    return -1;
}
__attribute__((weak)) int agc_display_scanout_layout(const agc_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) uint32_t *agc_display_scanout(agc_display_t *disp, int which) {
    (void)disp;
    (void)which;
    return (uint32_t *)0;
}
__attribute__((weak)) int agc_display_wait_scanout(agc_display_t *disp) {
    (void)disp;
    return -1;
}
__attribute__((weak)) int agc_display_flip_scanout(agc_display_t *disp) {
    (void)disp;
    return -1;
}
__attribute__((weak)) int agc_display_try_gpu_tiler(agc_display_t *disp) {
    (void)disp;
    return 0;
}
__attribute__((weak)) void agc_display_close(agc_display_t *disp) {
    (void)disp;
}
#endif
