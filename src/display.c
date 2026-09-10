#include "oops/display.h"
#include "oops/target.h"

#if OOPS_TARGET_IS_PS5
#include "agc/display.h"
#include "gnm/display.h"
#else
#include "agc/display.h"
#include "gnm/display.h"
#endif

struct oops_display {
  oops_display_backend_t backend;
#if OOPS_TARGET_IS_PS5
  agc_display_t *agc;
#else
  gnm_display_t *gnm;
#endif
};

static struct oops_display s_unified_display;
static int s_opened =
    0; /* a backend has been handed out since the last close */

oops_display_t *oops_display_open(oops_display_backend_t backend,
                                  unsigned int width, unsigned int height) {
  struct oops_display *disp = &s_unified_display;
  /* A second open without a close would overwrite the pointer to a live backend
   * and leak it. Closing a backend whose open failed is a no-op, so this is
   * safe on every path. */
  if (s_opened) {
    oops_display_close(disp);
  }
  disp->backend = backend;

#if OOPS_TARGET_IS_PS5
  /* PS5 Native target (Prospero / Trinity): AGC graphics */
  if (backend == OOPS_DISPLAY_BACKEND_AUTO ||
      backend == OOPS_DISPLAY_BACKEND_AGC) {
    disp->backend = OOPS_DISPLAY_BACKEND_AGC;
    disp->agc = agc_display_open(width, height);
    s_opened = 1;
    return disp;
  }
  /* GNM requested on a PS5 native target: refused to prevent NeoMode linkage */
  disp->agc = (agc_display_t *)0;
  return (oops_display_t *)0;
#else
  /* PS4 target (Orbis / Neo): GNM graphics */
  if (backend == OOPS_DISPLAY_BACKEND_AUTO ||
      backend == OOPS_DISPLAY_BACKEND_GNM) {
    disp->backend = OOPS_DISPLAY_BACKEND_GNM;
    disp->gnm = gnm_display_open(width, height);
    s_opened = 1;
    return disp;
  }
  /* AGC requested on a PS4 target: unsupported */
  disp->gnm = (gnm_display_t *)0;
  return (oops_display_t *)0;
#endif
}

int oops_display_is_ready(const oops_display_t *disp) {
  if (!disp)
    return 0;
#if OOPS_TARGET_IS_PS5
  return agc_display_is_ready(disp->agc);
#else
  return gnm_display_is_ready(disp->gnm);
#endif
}

int oops_display_is_gpu_accelerated(const oops_display_t *disp) {
  if (!disp)
    return 0;
#if OOPS_TARGET_IS_PS5
  return agc_display_is_gpu_accelerated(disp->agc);
#else
  return 0;
#endif
}

uint32_t *oops_display_get_framebuffer(oops_display_t *disp) {
  if (!disp)
    return (uint32_t *)0;
#if OOPS_TARGET_IS_PS5
  return agc_display_get_framebuffer(disp->agc);
#else
  return gnm_display_get_framebuffer(disp->gnm);
#endif
}

unsigned int oops_display_get_width(const oops_display_t *disp) {
  if (!disp)
    return 0;
#if OOPS_TARGET_IS_PS5
  return agc_display_get_width(disp->agc);
#else
  return gnm_display_get_width(disp->gnm);
#endif
}

unsigned int oops_display_get_height(const oops_display_t *disp) {
  if (!disp)
    return 0;
#if OOPS_TARGET_IS_PS5
  return agc_display_get_height(disp->agc);
#else
  return gnm_display_get_height(disp->gnm);
#endif
}

uint64_t oops_display_get_flip_count(const oops_display_t *disp) {
  if (!disp)
    return 0;
#if OOPS_TARGET_IS_PS5
  return agc_display_get_flip_count(disp->agc);
#else
  return gnm_display_get_flip_count(disp->gnm);
#endif
}

int oops_display_get_last_error(const oops_display_t *disp) {
  if (!disp)
    return -1;
#if OOPS_TARGET_IS_PS5
  return agc_display_get_last_error(disp->agc);
#else
  return gnm_display_get_last_error(disp->gnm);
#endif
}

int oops_display_get_video_handle(const oops_display_t *disp) {
  if (!disp)
    return -1;
#if OOPS_TARGET_IS_PS5
  return agc_display_get_video_handle(disp->agc);
#else
  return gnm_display_get_video_handle(disp->gnm);
#endif
}

const char *oops_display_get_backend_name(const oops_display_t *disp) {
  if (!disp)
    return "none";
#if OOPS_TARGET_IS_PS5
  return (OOPS_TARGET == OOPS_TARGET_TRINITY) ? "AGC (Trinity)"
                                              : "AGC (Prospero)";
#else
  return (OOPS_TARGET == OOPS_TARGET_NEO) ? "GNM (Neo)" : "GNM (Orbis)";
#endif
}

void oops_display_clear(oops_display_t *disp, uint32_t color) {
  if (!disp)
    return;
#if OOPS_TARGET_IS_PS5
  agc_display_clear(disp->agc, color);
#else
  gnm_display_clear(disp->gnm, color);
#endif
}

int oops_display_flip(oops_display_t *disp) {
  if (!disp)
    return -1;
#if OOPS_TARGET_IS_PS5
  return agc_display_flip(disp->agc);
#else
  return gnm_display_flip(disp->gnm);
#endif
}

void oops_display_close(oops_display_t *disp) {
  if (!disp)
    return;
#if OOPS_TARGET_IS_PS5
  agc_display_close(disp->agc);
#else
  gnm_display_close(disp->gnm);
#endif
  s_opened = 0;
}

#if OOPS_TARGET_IS_PS5
/* Stubs for the gnm backend when the build targets prospero or trinity
 * (OOPS_TARGET_IS_PS5), so code that calls gnm_* directly still links without
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
__attribute__((weak)) uint32_t *
gnm_display_get_framebuffer(gnm_display_t *disp) {
  (void)disp;
  return (uint32_t *)0;
}
__attribute__((weak)) unsigned int
gnm_display_get_width(const gnm_display_t *disp) {
  (void)disp;
  return 0;
}
__attribute__((weak)) unsigned int
gnm_display_get_height(const gnm_display_t *disp) {
  (void)disp;
  return 0;
}
__attribute__((weak)) uint64_t
gnm_display_get_flip_count(const gnm_display_t *disp) {
  (void)disp;
  return 0;
}
__attribute__((weak)) int
gnm_display_get_last_error(const gnm_display_t *disp) {
  (void)disp;
  return -1;
}
__attribute__((weak)) int
gnm_display_get_video_handle(const gnm_display_t *disp) {
  (void)disp;
  return -1;
}
__attribute__((weak)) void gnm_display_clear(gnm_display_t *disp,
                                             uint32_t color) {
  (void)disp;
  (void)color;
}
__attribute__((weak)) int gnm_display_flip(gnm_display_t *disp) {
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
 * (OOPS_TARGET_IS_PS4), weak for the same reason: a consumer that compiles
 * src/agc/agc_display.c as well gets the real backend. */
__attribute__((weak)) void agc_display_set_logger(agc_log_fn fn) { (void)fn; }
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
__attribute__((weak)) int
agc_display_is_gpu_accelerated(const agc_display_t *disp) {
  (void)disp;
  return 0;
}
__attribute__((weak)) uint32_t *
agc_display_get_framebuffer(agc_display_t *disp) {
  (void)disp;
  return (uint32_t *)0;
}
__attribute__((weak)) unsigned int
agc_display_get_width(const agc_display_t *disp) {
  (void)disp;
  return 0;
}
__attribute__((weak)) unsigned int
agc_display_get_height(const agc_display_t *disp) {
  (void)disp;
  return 0;
}
__attribute__((weak)) uint64_t
agc_display_get_flip_count(const agc_display_t *disp) {
  (void)disp;
  return 0;
}
__attribute__((weak)) int
agc_display_get_last_error(const agc_display_t *disp) {
  (void)disp;
  return -1;
}
__attribute__((weak)) int
agc_display_get_video_handle(const agc_display_t *disp) {
  (void)disp;
  return -1;
}
__attribute__((weak)) void agc_display_clear(agc_display_t *disp,
                                             uint32_t color) {
  (void)disp;
  (void)color;
}
__attribute__((weak)) int agc_display_flip(agc_display_t *disp) {
  (void)disp;
  return -1;
}
__attribute__((weak)) void agc_display_close(agc_display_t *disp) {
  (void)disp;
}
#endif
