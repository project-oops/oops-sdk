#ifndef OOPS_DISPLAY_H
#define OOPS_DISPLAY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OOPS_DISPLAY_DEFAULT_WIDTH  1920
#define OOPS_DISPLAY_DEFAULT_HEIGHT 1080

typedef enum oops_display_backend {
    OOPS_DISPLAY_BACKEND_AUTO = 0,
    OOPS_DISPLAY_BACKEND_GNM  = 4, /* Orbis / GCN (PS4) */
    OOPS_DISPLAY_BACKEND_AGC  = 5  /* Prospero / RDNA2 (PS5) */
} oops_display_backend_t;

typedef struct oops_display oops_display_t;

/* Open the primary display output using the specified backend (or auto-detected from compile target). */
oops_display_t *oops_display_open(oops_display_backend_t backend, unsigned int width, unsigned int height);

/* Query state */
int oops_display_is_ready(const oops_display_t *disp);
uint32_t *oops_display_get_framebuffer(oops_display_t *disp);
unsigned int oops_display_get_width(const oops_display_t *disp);
unsigned int oops_display_get_height(const oops_display_t *disp);
uint64_t oops_display_get_flip_count(const oops_display_t *disp);
int oops_display_get_last_error(const oops_display_t *disp);
const char *oops_display_get_backend_name(const oops_display_t *disp);

/* Drawing and presentation */
void oops_display_clear(oops_display_t *disp, uint32_t color);
int oops_display_flip(oops_display_t *disp);

/* Close output and release video resources */
void oops_display_close(oops_display_t *disp);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_DISPLAY_H */
