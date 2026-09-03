#ifndef OOPS_AGC_DISPLAY_H
#define OOPS_AGC_DISPLAY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agc_display agc_display_t;

typedef void (*agc_log_fn)(const char *tag, const char *msg, uint64_t val);
void agc_display_set_logger(agc_log_fn fn);

agc_display_t *agc_display_open(unsigned int width, unsigned int height);
int agc_display_is_ready(const agc_display_t *disp);
uint32_t *agc_display_get_framebuffer(agc_display_t *disp);
unsigned int agc_display_get_width(const agc_display_t *disp);
unsigned int agc_display_get_height(const agc_display_t *disp);
uint64_t agc_display_get_flip_count(const agc_display_t *disp);
int agc_display_get_last_error(const agc_display_t *disp);
void agc_display_clear(agc_display_t *disp, uint32_t color);
int agc_display_flip(agc_display_t *disp);
void agc_display_close(agc_display_t *disp);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AGC_DISPLAY_H */
