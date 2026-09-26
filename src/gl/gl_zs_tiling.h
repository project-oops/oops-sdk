/*
 * The byte offset of a pixel in oops-gl's depth and stencil surfaces on the console.
 *
 * Both are 64KB_Z_X (DB_Z_INFO's and DB_STENCIL_INFO's SW_MODE 24): 64 KiB blocks laid
 * out row by row across the surface's pitch, and inside a block a pixel's offset is the
 * XOR of one vector per set bit of its in-block x and y. The vectors are addrlib's,
 * under the chip identity and the GB_ADDR_CONFIG oops-mesa derived from the display
 * tiler, set up as ac_surface.c sets up a depth-stencil pair - and **tools/zs-tiling
 * checks every pixel of a 3 x 2 block surface against addrlib** for both, with an
 * eight-pipe control that disagrees. Its tracked output,
 * tools/zs-tiling/zs_tiling_gfx1013.txt, is where the numbers below come from; change
 * them there.
 *
 * The depth vectors share their upper four bits a coordinate with the display tiler's
 * 32-bit ones (agc_tiler.c) - the pipe and bank bits of the same configuration - and
 * differ in the lower three, which order the pixels of a 256-byte micro-tile: Z
 * interleaves x and y there, the display's R swizzle does not. (This said "x and y
 * exchanged" until 2026-09-19, which the two tables contradict.)
 *
 * Coordinates are the surface's: row 0 is the top, as the GPU draws it, so a GL window
 * row y is surface row height - 1 - y. `pitch` is the surface's width in pixels, padded
 * to whole blocks - 128 for depth, 256 for stencil - as glContextCreate allocates them.
 */
#ifndef OOPS_GL_ZS_TILING_H
#define OOPS_GL_ZS_TILING_H

#include <stddef.h>
#include <stdint.h>

/* Z_32_FLOAT: 128 x 128 pixels a block, 7 bits of each coordinate inside it. */
static const uint32_t gl_zs_depth_x_basis[7] = {0x00004u, 0x00010u, 0x00040u, 0x00100u,
                                                0x02200u, 0x00800u, 0x08400u};
static const uint32_t gl_zs_depth_y_basis[7] = {0x00008u, 0x00020u, 0x00080u, 0x01100u,
                                                0x00200u, 0x00400u, 0x04800u};
/* STENCIL_8: 256 x 256 pixels a block, 8 bits of each. */
static const uint32_t gl_zs_stencil_x_basis[8] = {
    0x00001u, 0x00004u, 0x00010u, 0x00140u, 0x00200u, 0x00800u, 0x02400u, 0x08000u};
static const uint32_t gl_zs_stencil_y_basis[8] = {
    0x00002u, 0x00008u, 0x00020u, 0x00100u, 0x00280u, 0x00400u, 0x01800u, 0x04000u};

/* The block's base - blocks row by row across `pitch` - plus the XOR of the in-block
 * bits' vectors. `bits` is log2 of the block's side. */
static inline size_t gl_zs_offset(const uint32_t *xb, const uint32_t *yb, unsigned bits,
                                  uint32_t pitch, uint32_t x, uint32_t y) {
    uint32_t in_block = 0;
    for (unsigned b = 0; b < bits; b++) {
        if ((x >> b) & 1u)
            in_block ^= xb[b];
        if ((y >> b) & 1u)
            in_block ^= yb[b];
    }
    const size_t block =
        (size_t)(y >> bits) * (size_t)(pitch >> bits) + (size_t)(x >> bits);
    return block * 65536u + in_block;
}

/* Byte offsets into the depth surface (a float a pixel) and the stencil surface (a
 * byte). */
static inline size_t gl_zs_depth_offset(uint32_t pitch, uint32_t x, uint32_t y) {
    return gl_zs_offset(gl_zs_depth_x_basis, gl_zs_depth_y_basis, 7u, pitch, x, y);
}
static inline size_t gl_zs_stencil_offset(uint32_t pitch, uint32_t x, uint32_t y) {
    return gl_zs_offset(gl_zs_stencil_x_basis, gl_zs_stencil_y_basis, 8u, pitch, x, y);
}

#endif
