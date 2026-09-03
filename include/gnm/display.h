#ifndef OOPS_GNM_DISPLAY_H
#define OOPS_GNM_DISPLAY_H

#include <stdint.h>
#include <stddef.h>

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
void gnm_display_clear(gnm_display_t *disp, uint32_t color);
int gnm_display_flip(gnm_display_t *disp);
void gnm_display_close(gnm_display_t *disp);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_GNM_DISPLAY_H */
