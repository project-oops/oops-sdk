#ifndef OOPS_AGC_TILER_H
#define OOPS_AGC_TILER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The scanout layout is 64 KB macro-tiles of 128x128 32bpp pixels, laid out
 * row-major across a surface whose extent is rounded up to whole tiles. A tiled
 * buffer is therefore larger than the linear one whenever width or height is
 * not a multiple of 128: 1920x1080 is 15x9 tiles, 8,847,360 bytes against
 * 8,294,400 linear.
 */
#define AGC_TILE_DIM 128u
#define AGC_TILE_BYTES 0x10000u /* 128 * 128 * 4 */

/* Bytes a tiled destination must hold for a width x height surface; 0 if either
 * is 0. */
size_t agc_tile_surface_bytes(uint32_t width, uint32_t height);

/* Build the swizzle lookup table. agc_tile_surface() does this on first use; a
 * display calls it at open so that no two first callers race to build it.
 * Idempotent. */
void agc_tile_init(void);

/*
 * Tiles a 32bpp linear RGBX buffer into AMD RDNA2 GPU display scanout format
 * (kRenderTarget = 27). `dest` must hold agc_tile_surface_bytes(width, height);
 * the margin of a partial tile is left as it was.
 */
void agc_tile_surface(void *dest, const void *src, uint32_t width,
                      uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AGC_TILER_H */
