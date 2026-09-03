#include <stdint.h>
#include <stddef.h>
#include "agc/tiler.h"

/* Precomputed 128-element LUTs for 64KB RDNA2 macro-tiles (32bpp kRenderTarget) */
static uint32_t s_lut_x[128];
static uint32_t s_lut_y[128];
static int s_lut_initialized = 0;

static void init_tiler_lut(void) {
    /* RDNA2 GFX10.3 basis vectors for 32bpp (4 bytes/pixel) in 64KB blocks */
    static const uint32_t x_basis[7] = {
        0x000004u, 0x000008u, 0x000080u, 0x000100u, 0x002200u, 0x000800u, 0x008400u
    };
    static const uint32_t y_basis[7] = {
        0x000010u, 0x000020u, 0x000040u, 0x001100u, 0x000200u, 0x000400u, 0x004800u
    };
    for (uint32_t i = 0; i < 128u; i++) {
        uint32_t bx = 0, by = 0;
        for (int b = 0; b < 7; b++) {
            if ((i >> b) & 1u) {
                bx ^= x_basis[b];
                by ^= y_basis[b];
            }
        }
        /* Store in 32-bit pixel offsets (byte offset >> 2) */
        s_lut_x[i] = bx >> 2;
        s_lut_y[i] = by >> 2;
    }
    s_lut_initialized = 1;
}

void agc_tile_surface(void *dest, const void *src, uint32_t width, uint32_t height) {
    if (!dest || !src || width == 0 || height == 0) {
        return;
    }

    if (!s_lut_initialized) {
        init_tiler_lut();
    }

    uint32_t *dst32 = (uint32_t *)dest;
    const uint32_t *src32 = (const uint32_t *)src;
    uint32_t tiles_per_row = (width + 127u) >> 7;

    for (uint32_t ty = 0; ty < height; ty += 128u) {
        uint32_t block_h = (ty + 128u <= height) ? 128u : (height - ty);
        uint32_t row_tile_idx = (ty >> 7) * tiles_per_row;

        for (uint32_t tx = 0; tx < width; tx += 128u) {
            uint32_t block_w = (tx + 128u <= width) ? 128u : (width - tx);
            uint32_t tile_idx = row_tile_idx + (tx >> 7);
            uint32_t *tile_dest = dst32 + (tile_idx << 14); /* 16,384 pixels per 64KB block */

            for (uint32_t ly = 0; ly < block_h; ly++) {
                uint32_t y_off = s_lut_y[ly];
                const uint32_t *src_row = src32 + (ty + ly) * width + tx;

                for (uint32_t lx = 0; lx < block_w; lx++) {
                    tile_dest[y_off ^ s_lut_x[lx]] = src_row[lx];
                }
            }
        }
    }
}
