/*
 * The Orbis/Neo display backend behind <oops/display.h>: two linear VideoOut scanout
 * buffers, the framebuffer being one of them.
 */
#ifndef OOPS_GNM_DISPLAY_H
#define OOPS_GNM_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gnm_display gnm_display_t;

gnm_display_t *gnm_display_open(unsigned int width, unsigned int height);
int gnm_display_is_ready(const gnm_display_t *disp);
uint32_t *gnm_display_get_framebuffer(gnm_display_t *disp);
unsigned int gnm_display_get_width(const gnm_display_t *disp);
unsigned int gnm_display_get_height(const gnm_display_t *disp);
uint64_t gnm_display_get_flip_count(const gnm_display_t *disp);
int gnm_display_get_last_error(const gnm_display_t *disp);
int gnm_display_get_video_handle(const gnm_display_t *disp);
void gnm_display_clear(gnm_display_t *disp, uint32_t color);
int gnm_display_flip(gnm_display_t *disp);
/* A caller's linear image into the buffer on screen; the framebuffer is untouched. */
int gnm_display_present(gnm_display_t *disp, const uint32_t *pixels);
/* The buffer on screen, copied out. */
int gnm_display_read_shown(gnm_display_t *disp, uint32_t *pixels);
/* The scanout buffers for a renderer that draws them itself - see
 * oops_display_scanout_layout in <oops/display.h>, whose values these return. */
int gnm_display_scanout_layout(const gnm_display_t *disp);
uint32_t *gnm_display_scanout(gnm_display_t *disp, int which);
int gnm_display_wait_scanout(gnm_display_t *disp);
int gnm_display_flip_scanout(gnm_display_t *disp);
int gnm_display_set_flip_rate(gnm_display_t *disp, unsigned int rate);
void gnm_display_close(gnm_display_t *disp);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_GNM_DISPLAY_H */
