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

/* Drawing and presentation */
void oops_display_clear(oops_display_t *disp, uint32_t color);
int oops_display_flip(oops_display_t *disp);

/* Close output and release video resources */
void oops_display_close(oops_display_t *disp);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_DISPLAY_H */
