#ifndef OOPS_AGC_TILER_H
#define OOPS_AGC_TILER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tiles a 32bpp linear RGBX buffer into AMD RDNA2 GPU display scanout format (kRenderTarget = 27) */
void agc_tile_surface(void *dest, const void *src, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AGC_TILER_H */
