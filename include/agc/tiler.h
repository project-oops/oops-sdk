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

/*
 * Computes 2D pixel coordinates (*out_x, *out_y) in [0..127] from a 32-bit
 * dword offset within a 64KB macro-tile (kRenderTarget = 27, 64KB_R_X).
 * Exact closed-form algebraic inverse of the RDNA2 basis vectors over GF(2).
 */
static inline void agc_detile_pixel(uint32_t offset_dwords, uint32_t *out_x,
                                   uint32_t *out_y) {
  uint32_t x0 = (offset_dwords >> 0) & 1u;
  uint32_t x1 = (offset_dwords >> 1) & 1u;
  uint32_t y0 = (offset_dwords >> 2) & 1u;
  uint32_t y1 = (offset_dwords >> 3) & 1u;
  uint32_t y2 = (offset_dwords >> 4) & 1u;
  uint32_t x2 = (offset_dwords >> 5) & 1u;

  uint32_t y3 = (offset_dwords >> 10) & 1u;
  uint32_t x3 = ((offset_dwords >> 6) & 1u) ^ y3;

  uint32_t x4 = (offset_dwords >> 11) & 1u;
  uint32_t y4 = ((offset_dwords >> 7) & 1u) ^ x4;

  uint32_t x6 = (offset_dwords >> 13) & 1u;
  uint32_t y5 = ((offset_dwords >> 8) & 1u) ^ x6;

  uint32_t y6 = (offset_dwords >> 12) & 1u;
  uint32_t x5 = ((offset_dwords >> 9) & 1u) ^ y6;

  if (out_x) {
    *out_x = x0 | (x1 << 1) | (x2 << 2) | (x3 << 3) | (x4 << 4) | (x5 << 5) |
             (x6 << 6);
  }
  if (out_y) {
    *out_y = y0 | (y1 << 1) | (y2 << 2) | (y3 << 3) | (y4 << 4) | (y5 << 5) |
             (y6 << 6);
  }
}

/*
 * Detiles an AMD RDNA2 GPU scanout / render target buffer (kRenderTarget = 27)
 * into a linear 32bpp RGBX buffer. `src` must hold at least
 * agc_tile_surface_bytes(width, height).
 */
void agc_detile_surface(void *dest, const void *src, uint32_t width,
                        uint32_t height);

/* ---- buffer resource constants (V#) ----------------------------------------
 *
 * A buffer access on this generation names four consecutive scalar registers
 * holding a 128-bit resource constant. agc_buffer_descriptor() packs one.
 *
 * # Where each field's position comes from
 *
 * Corroborated by this collection's own descriptor decoder, which reads the
 * same constant back out of a translated shader (orbistoun, the buffer-resource
 * reader in the shader model - base address 47:0, stride 61:48, swizzle enable
 * at 63, record count 95:64, add-thread-id at 119, out-of-bounds mode 125:124):
 *
 *   word 0      base address, low 32
 *   word 1      [15:0]  base address, high 16   -> 47:0 overall
 *               [29:16] stride in bytes, 0..16383
 *               [31]    swizzle enable
 *   word 2      number of records
 *   word 3      [23]    add-thread-id enable
 *               [29:28] out-of-bounds select
 *
 * The rest of word 3 is the *conversion* half of the descriptor, which the
 * decoder above deliberately does not read because untyped accesses ignore it.
 * Its positions here are from the published instruction-set reference for this
 * generation, not from anything this collection has measured:
 *
 *   word 3      [11:0]  four 3-bit destination channel selects, x,y,z,w
 *               [18:12] format
 *               [31:30] resource type, 0 for a buffer
 *
 * A typed access (the *_FORMAT_* opcodes) converts through that format, so it
 * is the field that decides whether a 32-bit pixel arrives in a register
 * unaltered. The format *codes* are measured - see AGC_BUF_FMT_* below - but
 * the bit position they are written to is not. Treat a first bring-up that
 * produces a correctly-addressed but wrongly-converted surface as evidence
 * about this field.
 */

/* Channel select: 0 zero, 1 one, 4 R, 5 G, 6 B, 7 A. Identity for four
 * components is x=R, y=G, z=B, w=A. */
#define AGC_BUF_DST_SEL_IDENTITY 0x00000FACu /* (7<<9)|(6<<6)|(5<<3)|4 */

/*
 * Typed-buffer format codes, for gfx1030, taken from this collection's measured
 * format table (orbistoun, crates/orbistoun-shader/data/buffer-formats.toml -
 * every row observed by assembling the code and reading back the name the
 * reference disassembler prints). Only the two the scanout tiler needs are
 * named here.
 */
#define AGC_BUF_FMT_32_UINT 20u    /* one 32-bit component, no conversion */
#define AGC_BUF_FMT_32_32_UINT 62u /* two 32-bit components, no conversion */

/* Out-of-bounds select 3: an access past the record count reads zero and drops
 * writes, rather than clamping. The honest failure of the four. */
#define AGC_BUF_OOB_STRUCTURED 3u

/*
 * Packs a buffer resource constant into `out`, four 32-bit words in the order a
 * shader's s[n:n+3] expects.
 *
 * `base` is a GPU virtual byte address and is truncated to 48 bits. `stride` is
 * bytes per record and must be 0..16383; `num_records` is in units of stride
 * when there is one and bytes when there is not. `format` is an AGC_BUF_FMT_*
 * code. Swizzle and add-thread-id are left clear: both change where an access
 * lands, and neither is wanted by anything here.
 *
 * Returns 0, or -1 without writing `out` when `out` is null, `stride` exceeds
 * 16383, or `format` does not fit the 7-bit field.
 */
int agc_buffer_descriptor(uint32_t out[4], uint64_t base, uint32_t stride,
                          uint32_t num_records, uint32_t format);

/* ---- the compute tiler's dispatch interface ---------------------------------
 *
 * What the shader in <agc/shader_tiler.h> expects, recovered by decoding the
 * container header and walking the payload rather than from any vendor source.
 * Every line is a statement about bytes already in this repository:
 *
 *   COMPUTE_PGM_RSRC2 = 0x00000992 -> 9 user scalar registers, s0..s8, with
 *   both workgroup-id-x and workgroup-id-y enabled (they arrive in s9, s10, and
 *   the payload's first two instructions read exactly those). Thread dimensions
 *   from the header are 8 x 8 x 1.
 *
 *   The payload moves s1,s2,s3,s4 into s12..s15 and s5,s6,s7,s8 into s0..s3,
 *   then uses s[12:15] for eight single-component typed loads and s[0:3] for
 *   four two-component typed stores - so user data 1..4 is the *source*
 *   descriptor and user data 5..8 the *destination* one. Eight dwords in and
 *   eight dwords out per thread agree.
 *
 *   User data 0 is read once, early, and never again. Its meaning is NOT
 *   established: the surface width in pixels is the obvious candidate and the
 *   one AGC_TILER_USER_DATA_ARG0_IS_WIDTH assumes, but nothing here proves it.
 *
 * Hence: this interface is a decode, not a measurement. It has never been
 * dispatched on hardware. agc_display_flip() still tiles on the CPU, and the
 * acceleration flag stays clear until agc_display_try_gpu_tiler() has compared
 * a GPU-produced surface against agc_tile_surface() byte for byte on a real
 * device. Do not enable it on the strength of this comment.
 */
#define AGC_TILER_USER_DATA_COUNT 9u
#define AGC_TILER_SRC_DESC_SLOT 1u /* user data 1..4 */
#define AGC_TILER_DST_DESC_SLOT 5u /* user data 5..8 */
#define AGC_TILER_THREADS_X 8u
#define AGC_TILER_THREADS_Y 8u
#define AGC_TILER_PIXELS_PER_THREAD 8u

/*
 * Fills `user_data` (AGC_TILER_USER_DATA_COUNT words) and the workgroup counts
 * for tiling a width x height surface from `linear_src` into `tiled_dst`.
 *
 * Returns 0, or -1 when an argument is null, either extent is 0, or the
 * dispatch does not divide evenly - one workgroup covers
 * THREADS_X * PIXELS_PER_THREAD by THREADS_Y pixels (64 x 8), so a surface
 * whose width is not a multiple of 64 or whose height is not a multiple of 8
 * is refused rather than partially tiled. 1920x1080 divides; 1280x720 divides.
 */
int agc_tiler_dispatch_params(uint32_t *user_data, uint32_t *groups_x,
                              uint32_t *groups_y, uint64_t linear_src,
                              uint64_t tiled_dst, uint32_t width,
                              uint32_t height);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AGC_TILER_H */
