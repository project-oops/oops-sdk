#ifndef OOPS_DISPLAY_H
#define OOPS_DISPLAY_H

#include "oops/target.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OOPS_DISPLAY_DEFAULT_WIDTH 1920
#define OOPS_DISPLAY_DEFAULT_HEIGHT 1080

typedef enum oops_display_backend {
  OOPS_DISPLAY_BACKEND_AUTO = 0,
  OOPS_DISPLAY_BACKEND_GNM = 4, /* Orbis-generation / GCN */
  OOPS_DISPLAY_BACKEND_AGC = 5  /* Prospero-generation / RDNA2 */
} oops_display_backend_t;

typedef struct oops_display oops_display_t;

/*
 * Open the primary display output. OOPS_DISPLAY_BACKEND_AUTO defers to the
 * compiled target mode: AGC for Prospero/Trinity (native) and GNM for
 * Orbis/Neo. Check oops_display_is_ready() - a backend that could not
 * open is returned, not hidden.
 */
oops_display_t *oops_display_open(oops_display_backend_t backend,
                                  unsigned int width, unsigned int height);

/* Query state */
int oops_display_is_ready(const oops_display_t *disp);
int oops_display_is_gpu_accelerated(const oops_display_t *disp);
uint32_t *oops_display_get_framebuffer(oops_display_t *disp);
unsigned int oops_display_get_width(const oops_display_t *disp);
unsigned int oops_display_get_height(const oops_display_t *disp);
/* Completed flips as the hardware counts them, or submitted flips on a backend
 * where the status query is unavailable. */
uint64_t oops_display_get_flip_count(const oops_display_t *disp);
int oops_display_get_last_error(const oops_display_t *disp);
const char *oops_display_get_backend_name(const oops_display_t *disp);
int oops_display_get_video_handle(const oops_display_t *disp);

/*
 * Move display tiling from the CPU to a compute shader, if the hardware agrees.
 *
 * Every flip converts the linear render target into the display-tiled scanout surface. On the
 * CPU that is a read of the whole frame out of write-combined video memory and a scattered write
 * back into it - 2,073,600 words each way at 1920x1080, with no caching on either side, and it
 * is the single largest cost in presenting a frame.
 *
 * This runs the compute tiler once, compares its output against the CPU tiler's byte for byte,
 * and only then lets later flips use it; a mismatch, a queue that will not create or a dispatch
 * that stops retiring all fall back to the CPU path rather than presenting a wrong buffer.
 *
 * **Call it before the first flip.** It writes a test pattern through both scanout buffers, so
 * afterwards it refuses (returning 0) rather than disturb a buffer that is on screen or queued.
 *
 * Returns 1 if the compute tiler is now in use, 0 if the CPU tiler stays, -1 for no display.
 * Not calling it is what every caller did before this existed, and leaves the CPU tiler in place.
 */
int oops_display_try_gpu_tiler(oops_display_t *disp);

/* Drawing and presentation */
void oops_display_clear(oops_display_t *disp, uint32_t color);
int oops_display_flip(oops_display_t *disp);

/*
 * **Another linear image on screen, without it becoming the framebuffer.**
 * `pixels` is a width x height image of 0xAARRGGBB words, the framebuffer's size
 * and layout. AGC tiles it onto the next scanout buffer and flips, as
 * oops_display_flip does with the framebuffer. GNM's framebuffer is itself a
 * scanout buffer, so it copies into the buffer on screen instead. Either way the
 * framebuffer is untouched, which is what lets a GL front buffer be shown while
 * the back buffer keeps its contents. Returns 0, or negative without a display.
 */
int oops_display_present(oops_display_t *disp, const uint32_t *pixels);

/* The image on screen now - the last one flipped or presented - as the same
 * kind of linear image. AGC detiles its scanout buffer; GNM copies it. Returns
 * 0, or negative without a display. */
int oops_display_read_shown(oops_display_t *disp, uint32_t *pixels);

/*
 * **For a renderer that draws the scanout buffers itself** - a GPU renderer, as
 * a title on the console is - rather than handing the display a linear image to
 * convert. The buffers are the display's two, in whatever layout VideoOut scans.
 */
typedef enum oops_display_scanout_layout {
  OOPS_DISPLAY_SCANOUT_NONE = 0,   /* no display, or no scanout buffers */
  OOPS_DISPLAY_SCANOUT_LINEAR = 1, /* rows of 0xAARRGGBB words, the width a row */
  /* The GPU's 64KB_R_X render-target swizzle at 32 bits a pixel: 64 KiB blocks
   * of 128 x 128 pixels, row by row across the width padded to 128, each
   * addressed as <agc/tiler.h>'s agc_tile_pixel says. */
  OOPS_DISPLAY_SCANOUT_RX = 2
} oops_display_scanout_layout_t;

oops_display_scanout_layout_t
oops_display_scanout_layout(const oops_display_t *disp);

/* The scanout buffer the next flip shows (`which` 0), or the one on screen
 * now (1). NULL without one. */
uint32_t *oops_display_scanout(oops_display_t *disp, int which);

/* Wait - up to about 100 ms - until the next buffer is off screen: every flip
 * submitted has completed, so the buffer that was on screen before the last one
 * is free to draw into. 0 once it is, 1 on the timeout, negative without a
 * display. */
int oops_display_wait_scanout(oops_display_t *disp);

/* Flip the next buffer as it was drawn - nothing tiled or copied into it - and
 * make the other one next. */
int oops_display_flip_scanout(oops_display_t *disp);

/* A renderer that draws the scanout buffers in place says so here, once, so
 * that oops_display_get_surface (<oops/draw.h>) describes the next scanout
 * buffer rather than a framebuffer nothing will flip. Returns the layout, or
 * OOPS_DISPLAY_SCANOUT_NONE - and nothing changes - without scanout buffers. */
oops_display_scanout_layout_t oops_display_use_scanout(oops_display_t *disp);

/* Close output and release video resources */
void oops_display_close(oops_display_t *disp);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_DISPLAY_H */
