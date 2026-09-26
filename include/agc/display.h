#ifndef OOPS_AGC_DISPLAY_H
#define OOPS_AGC_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agc_display agc_display_t;

typedef void (*agc_log_fn)(const char *tag, const char *msg, uint64_t val);
void agc_display_set_logger(agc_log_fn fn);

agc_display_t *agc_display_open(unsigned int width, unsigned int height);
int agc_display_is_ready(const agc_display_t *disp);
int agc_display_is_gpu_accelerated(const agc_display_t *disp);
uint32_t *agc_display_get_framebuffer(agc_display_t *disp);
unsigned int agc_display_get_width(const agc_display_t *disp);
unsigned int agc_display_get_height(const agc_display_t *disp);
uint64_t agc_display_get_flip_count(const agc_display_t *disp);
int agc_display_get_last_error(const agc_display_t *disp);
int agc_display_get_video_handle(const agc_display_t *disp);
void agc_display_clear(agc_display_t *disp, uint32_t color);
int agc_display_flip(agc_display_t *disp);
/* A caller's linear image on screen, tiled and flipped as agc_display_flip does
 * its own; the display's linear surface is untouched. */
int agc_display_present(agc_display_t *disp, const uint32_t *pixels);
/* The image on screen, detiled into a linear width x height image. */
int agc_display_read_shown(agc_display_t *disp, uint32_t *pixels);
/* The scanout buffers for a renderer that draws them itself - see
 * oops_display_scanout_layout in <oops/display.h>, whose values these return. */
int agc_display_scanout_layout(const agc_display_t *disp);
uint32_t *agc_display_scanout(agc_display_t *disp, int which);
int agc_display_wait_scanout(agc_display_t *disp);
int agc_display_flip_scanout(agc_display_t *disp);
/* Open the display naming buffers the caller allocated alongside its own pair,
 * so a renderer can be scanned out of its own target with no copy. VideoOut
 * registration is single-shot (obSCEne `-9a4c`), so this is the only chance to
 * name them - there is no adding one later. `adopt` may be null with a count of
 * zero, which is exactly agc_display_open. */
agc_display_t *agc_display_open_adopting(unsigned int width, unsigned int height,
                                         void *const *adopt, int adopt_count);
/* The flip index the nth adopted buffer was given, or -1. */
int agc_display_adopted_index(const agc_display_t *disp, int nth);
/* Flip a buffer by index, including an adopted one. 0 on success. */
int agc_display_flip_index(agc_display_t *disp, int index);
void agc_display_close(agc_display_t *disp);

/*
 * Hardware bring-up for the RDNA2 compute tiler. NOT called by agc_display_open
 * and not called by anything else in this SDK: a caller has to ask for it.
 *
 * Fills the linear surface with a pattern, tiles it onto one scanout buffer with
 * the compute shader and onto the other with agc_tile_surface(), and compares
 * the two byte for byte. On an exact match the display switches to GPU tiling
 * for later flips; on anything else - mismatch, dispatch failure, fence
 * timeout - it tears the GPU path down and stays on the CPU tiler.
 *
 * Only safe before the first flip, because it writes both scanout buffers.
 *
 * The shader's dispatch interface was recovered by decoding the payload in
 * <agc/shader_tiler.h>, not measured (see the note in <agc/tiler.h>), and one
 * argument's meaning is still a guess. A wrong guess here is a malformed
 * dispatch, and a malformed dispatch can wedge the GPU - which a comparison
 * after the fact cannot undo. Run it on a device you can recover.
 *
 * Returns 1 if GPU tiling is now enabled, 0 if it is not, negative on a bad
 * argument.
 */
int agc_display_try_gpu_tiler(agc_display_t *disp);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AGC_DISPLAY_H */
