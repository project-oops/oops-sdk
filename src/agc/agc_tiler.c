#include "agc/tiler.h"
#include <stddef.h>
#include <stdint.h>

/* Precomputed 128-element LUTs for 64KB RDNA2 macro-tiles, 32bpp kRenderTarget,
 * in the swizzle addrlib calls 64KB_R_X.
 */
static uint32_t s_lut_x[128];
static uint32_t s_lut_y[128];
static int s_lut_initialized = 0;

static uint8_t s_inv_lx[16384];
static uint8_t s_inv_ly[16384];

static void init_tiler_lut(void) {
  /* RDNA2 basis vectors for 32bpp (4 bytes/pixel) in 64KB blocks, for the GFX10
   * level without RB+ that this part reports - not GFX10.3, which this comment
   * claimed until the gfx level was actually settled. The vectors did not
   * change; only the label on them was wrong. It was settled by deriving
   * GB_ADDR_CONFIG from this table rather than by assuming a family: inverting
   * addrlib against these numbers, no RB+ (GFX10.3) configuration reproduces
   * them at all, and the non-RB+ one does, at 16 pipes with a 256 B interleave.
   * The derivation is written out where its result is used, beside
   * OOPS_GB_ADDR_CONFIG in oops-mesa's drm_device.c.
   *
   * Each entry is the byte-address contribution of one x (or y) bit inside a
   * tile; a pixel's tile-relative byte address is the XOR of the entries for
   * its set bits. The 14 vectors are linearly independent across the 14 address
   * bits above the 4-byte pixel, which is what makes a tile a permutation of
   * its 16,384 pixels. The unit test pins one address per basis bit, so an edit
   * here fails there before it reaches a display. */
  static const uint32_t x_basis[7] = {0x000004u, 0x000008u, 0x000080u,
                                      0x000100u, 0x002200u, 0x000800u,
                                      0x008400u};
  static const uint32_t y_basis[7] = {0x000010u, 0x000020u, 0x000040u,
                                      0x001100u, 0x000200u, 0x000400u,
                                      0x004800u};
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

  /* Precompute the inverse permutation for 16,384 pixels per 64KB macro-tile.
   * Writing destination memory in strictly sequential order (tile_dest[0..16383])
   * allows the CPU to stream stores directly through Write-Combining (WC) write
   * buffers into GDDR6 without evictions or cache line thrashing. */
  for (uint32_t i = 0; i < 16384u; i++) {
    uint32_t x = 0, y = 0;
    agc_detile_pixel(i, &x, &y);
    s_inv_lx[i] = (uint8_t)x;
    s_inv_ly[i] = (uint8_t)y;
  }

  s_lut_initialized = 1;
}

void agc_tile_init(void) {
  if (!s_lut_initialized) {
    init_tiler_lut();
  }
}

int agc_buffer_descriptor(uint32_t out[4], uint64_t base, uint32_t stride,
                          uint32_t num_records, uint32_t format) {
  if (!out || stride > 0x3FFFu || format > 0x7Fu) {
    return -1;
  }

  /* Base is 48 bits; the high half shares word 1 with the stride. Truncating
   * rather than refusing an over-wide address would point the descriptor at
   * memory the caller did not name, so refuse that too. */
  if ((base >> 48) != 0u) {
    return -1;
  }

  out[0] = (uint32_t)(base & 0xFFFFFFFFu);
  out[1] = (uint32_t)((base >> 32) & 0xFFFFu) | ((stride & 0x3FFFu) << 16);
  /* bit 31 of word 1 is swizzle enable, left clear */
  out[2] = num_records;
  out[3] = AGC_BUF_DST_SEL_IDENTITY | ((format & 0x7Fu) << 12) |
           (AGC_BUF_OOB_STRUCTURED << 28);
  /* bit 23 of word 3 is add-thread-id, left clear; bits 31:30 are the resource
   * type, 0 for a buffer */
  return 0;
}

int agc_tiler_dispatch_params(uint32_t *user_data, uint32_t *groups_x,
                              uint32_t *groups_y, uint64_t linear_src,
                              uint64_t tiled_dst, uint32_t width,
                              uint32_t height) {
  if (!user_data || !groups_x || !groups_y || width == 0 || height == 0) {
    return -1;
  }

  const uint32_t span_x = AGC_TILER_THREADS_X * AGC_TILER_PIXELS_PER_THREAD;
  const uint32_t span_y = AGC_TILER_THREADS_Y;
  if ((width % span_x) != 0u || (height % span_y) != 0u) {
    return -1;
  }

  /* The source is the linear surface, one 32-bit pixel per record. The
   * destination is the tiled surface addressed as dword *pairs*, because the
   * stores are two-component: the swizzle puts a pixel and its x+1 neighbour in
   * adjacent dwords (x bit 0 contributes 4 bytes, x bit 1 contributes 8), so a
   * pair is contiguous and a two-component record is the right unit. */
  uint32_t src_desc[4];
  uint32_t dst_desc[4];
  size_t tiled_bytes = agc_tile_surface_bytes(width, height);

  if (agc_buffer_descriptor(src_desc, linear_src, 4u,
                            width * height, AGC_BUF_FMT_32_UINT) != 0) {
    return -1;
  }
  if (agc_buffer_descriptor(dst_desc, tiled_dst, 8u,
                            (uint32_t)(tiled_bytes / 8u),
                            AGC_BUF_FMT_32_32_UINT) != 0) {
    return -1;
  }

  /* User data 0: read once by the shader and never again. Unestablished; the
   * surface width in pixels is the assumption, and the one thing a hardware
   * bring-up should vary first if the addressing comes out wrong. */
  user_data[0] = width;
  for (int i = 0; i < 4; i++) {
    user_data[AGC_TILER_SRC_DESC_SLOT + (unsigned)i] = src_desc[i];
    user_data[AGC_TILER_DST_DESC_SLOT + (unsigned)i] = dst_desc[i];
  }

  *groups_x = width / span_x;
  *groups_y = height / span_y;
  return 0;
}

size_t agc_tile_surface_bytes(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    return 0;
  }
  size_t tiles_x = ((size_t)width + AGC_TILE_DIM - 1u) / AGC_TILE_DIM;
  size_t tiles_y = ((size_t)height + AGC_TILE_DIM - 1u) / AGC_TILE_DIM;
  return tiles_x * tiles_y * (size_t)AGC_TILE_BYTES;
}

void agc_tile_surface(void *dest, const void *src, uint32_t width,
                      uint32_t height) {
  if (!dest || !src || width == 0 || height == 0) {
    return;
  }

  agc_tile_init();

  uint32_t *dst32 = (uint32_t *)dest;
  const uint32_t *src32 = (const uint32_t *)src;
  uint32_t tiles_per_row = (width + 127u) >> 7;

  for (uint32_t ty = 0; ty < height; ty += 128u) {
    uint32_t block_h = (ty + 128u <= height) ? 128u : (height - ty);
    uint32_t row_tile_idx = (ty >> 7) * tiles_per_row;

    for (uint32_t tx = 0; tx < width; tx += 128u) {
      uint32_t block_w = (tx + 128u <= width) ? 128u : (width - tx);
      uint32_t tile_idx = row_tile_idx + (tx >> 7);
      /* 16,384 pixels per 64KB block */
      uint32_t *tile_dest = dst32 + (size_t)tile_idx * (AGC_TILE_BYTES / 4u);

      const uint32_t *rows[128];
      for (uint32_t r = 0; r < block_h; r++) {
        rows[r] = src32 + (ty + r) * width + tx;
      }

      if (block_w == 128u && block_h == 128u) {
        for (uint32_t i = 0; i < 16384u; i++) {
          tile_dest[i] = rows[s_inv_ly[i]][s_inv_lx[i]];
        }
      } else {
        for (uint32_t i = 0; i < 16384u; i++) {
          uint32_t lx = s_inv_lx[i];
          uint32_t ly = s_inv_ly[i];
          if (lx < block_w && ly < block_h) {
            tile_dest[i] = rows[ly][lx];
          }
        }
      }
    }
  }
}


void agc_detile_surface(void *dest, const void *src, uint32_t width,
                        uint32_t height) {
  if (!dest || !src || width == 0 || height == 0) {
    return;
  }

  agc_tile_init();

  uint32_t *dst32 = (uint32_t *)dest;
  const uint32_t *src32 = (const uint32_t *)src;
  uint32_t tiles_per_row = (width + 127u) >> 7;

  for (uint32_t ty = 0; ty < height; ty += 128u) {
    uint32_t block_h = (ty + 128u <= height) ? 128u : (height - ty);
    uint32_t row_tile_idx = (ty >> 7) * tiles_per_row;

    for (uint32_t tx = 0; tx < width; tx += 128u) {
      uint32_t block_w = (tx + 128u <= width) ? 128u : (width - tx);
      uint32_t tile_idx = row_tile_idx + (tx >> 7);
      const uint32_t *tile_src = src32 + (size_t)tile_idx * (AGC_TILE_BYTES / 4u);

      for (uint32_t ly = 0; ly < block_h; ly++) {
        uint32_t y_off = s_lut_y[ly];
        uint32_t *dst_row = dst32 + (ty + ly) * width + tx;

        for (uint32_t lx = 0; lx < block_w; lx++) {
          dst_row[lx] = tile_src[y_off ^ s_lut_x[lx]];
        }
      }
    }
  }
}
