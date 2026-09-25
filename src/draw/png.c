#include "oops/draw.h"
#include "oops/heap.h"
#include "oops/system.h"
#include <stddef.h>
#include <stdint.h>

/* Helper memory functions */
static void png_memset(void *p, int v, size_t n) {
  uint8_t *b = (uint8_t *)p;
  for (size_t i = 0; i < n; i++) b[i] = (uint8_t)v;
}

static void png_memcpy(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  const uint8_t *s = (const uint8_t *)src;
  for (size_t i = 0; i < n; i++) d[i] = s[i];
}

/* Big-endian 32-bit integer reader */
static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* RFC 1951 Deflate Decompressor */

typedef struct {
  const uint8_t *in;
  size_t in_len;
  size_t in_pos;
  uint32_t bit_buf;
  int bit_count;
} bit_reader_t;

static void br_init(bit_reader_t *br, const uint8_t *in, size_t len) {
  br->in = in;
  br->in_len = len;
  br->in_pos = 0;
  br->bit_buf = 0;
  br->bit_count = 0;
}

static uint32_t br_get_bits(bit_reader_t *br, int n) {
  while (br->bit_count < n) {
    if (br->in_pos >= br->in_len) return (uint32_t)-1;
    br->bit_buf |= (uint32_t)br->in[br->in_pos++] << br->bit_count;
    br->bit_count += 8;
  }
  uint32_t val = br->bit_buf & ((1u << n) - 1u);
  br->bit_buf >>= n;
  br->bit_count -= n;
  return val;
}

typedef struct {
  int min_code[16];
  int max_code[16];
  int offset[16];
  uint16_t symbols[320];
} huffman_tree_t;

static int build_huffman_tree(huffman_tree_t *tree, const uint8_t *lens, int num_symbols) {
  int count[16];
  for (int i = 0; i < 16; i++) count[i] = 0;
  for (int i = 0; i < num_symbols; i++) {
    if (lens[i] > 0 && lens[i] < 16) count[lens[i]]++;
  }

  int next_code[16];
  int code = 0;
  count[0] = 0;
  for (int i = 1; i < 16; i++) {
    code = (code + count[i - 1]) << 1;
    next_code[i] = code;
  }

  int total_syms = 0;
  for (int i = 1; i < 16; i++) {
    if (count[i] > 0) {
      tree->min_code[i] = next_code[i];
      tree->max_code[i] = next_code[i] + count[i] - 1;
      tree->offset[i] = total_syms;
      total_syms += count[i];
    } else {
      tree->min_code[i] = -1;
      tree->max_code[i] = -1;
      tree->offset[i] = 0;
    }
  }

  for (int i = 0; i < num_symbols; i++) {
    int l = lens[i];
    if (l > 0 && l < 16) {
      int sym_idx = tree->offset[l] + (next_code[l] - tree->min_code[l]);
      next_code[l]++;
      if (sym_idx < 320) tree->symbols[sym_idx] = (uint16_t)i;
    }
  }
  return 0;
}

static int huffman_decode(bit_reader_t *br, const huffman_tree_t *tree) {
  int code = 0;
  for (int len = 1; len < 16; len++) {
    uint32_t b = br_get_bits(br, 1);
    if (b == (uint32_t)-1) return -1;
    code = (code << 1) | (int)b;
    if (tree->max_code[len] >= 0 && code <= tree->max_code[len] && code >= tree->min_code[len]) {
      return tree->symbols[tree->offset[len] + (code - tree->min_code[len])];
    }
  }
  return -1;
}

static const uint16_t k_len_base[29] = {
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t k_len_extra[29] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t k_dist_base[30] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t k_dist_extra[30] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t k_clen_order[19] = {
  16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int deflate_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_max, size_t *out_written) {
  bit_reader_t br;
  br_init(&br, in, in_len);
  size_t out_pos = 0;

  int bfinal = 0;
  while (!bfinal) {
    bfinal = (int)br_get_bits(&br, 1);
    uint32_t btype = br_get_bits(&br, 2);

    if (btype == 0) {
      /* Uncompressed block */
      br.bit_buf = 0;
      br.bit_count = 0;
      if (br.in_pos + 4 > br.in_len) return -1;
      uint16_t len = (uint16_t)((uint16_t)br.in[br.in_pos] | (uint16_t)((uint16_t)br.in[br.in_pos + 1] << 8));
      br.in_pos += 4;
      if (br.in_pos + len > br.in_len || out_pos + len > out_max) return -1;
      png_memcpy(out + out_pos, br.in + br.in_pos, len);
      out_pos += len;
      br.in_pos += len;
    } else if (btype == 1 || btype == 2) {
      huffman_tree_t lit_tree;
      huffman_tree_t dist_tree;

      if (btype == 1) {
        /* Fixed Huffman */
        uint8_t fixed_lit[288];
        for (int i = 0; i <= 143; i++) fixed_lit[i] = 8;
        for (int i = 144; i <= 255; i++) fixed_lit[i] = 9;
        for (int i = 256; i <= 279; i++) fixed_lit[i] = 7;
        for (int i = 280; i <= 287; i++) fixed_lit[i] = 8;
        build_huffman_tree(&lit_tree, fixed_lit, 288);

        uint8_t fixed_dist[32];
        for (int i = 0; i < 32; i++) fixed_dist[i] = 5;
        build_huffman_tree(&dist_tree, fixed_dist, 32);
      } else {
        /* Dynamic Huffman */
        uint32_t hlit = br_get_bits(&br, 5) + 257;
        uint32_t hdist = br_get_bits(&br, 5) + 1;
        uint32_t hclen = br_get_bits(&br, 4) + 4;
        if (hlit > 288 || hdist > 32 || hclen > 19) return -1;

        uint8_t clen[19];
        png_memset(clen, 0, sizeof(clen));
        for (uint32_t i = 0; i < hclen; i++) {
          clen[k_clen_order[i]] = (uint8_t)br_get_bits(&br, 3);
        }

        huffman_tree_t clen_tree;
        build_huffman_tree(&clen_tree, clen, 19);

        uint8_t code_lens[320];
        png_memset(code_lens, 0, sizeof(code_lens));
        uint32_t total = hlit + hdist;
        uint32_t idx = 0;

        while (idx < total) {
          int sym = huffman_decode(&br, &clen_tree);
          if (sym < 0) return -1;
          if (sym < 16) {
            code_lens[idx++] = (uint8_t)sym;
          } else if (sym == 16) {
            if (idx == 0) return -1;
            uint8_t prev = code_lens[idx - 1];
            uint32_t rep = br_get_bits(&br, 2) + 3;
            while (rep-- && idx < total) code_lens[idx++] = prev;
          } else if (sym == 17) {
            uint32_t rep = br_get_bits(&br, 3) + 3;
            while (rep-- && idx < total) code_lens[idx++] = 0;
          } else if (sym == 18) {
            uint32_t rep = br_get_bits(&br, 7) + 11;
            while (rep-- && idx < total) code_lens[idx++] = 0;
          }
        }
        build_huffman_tree(&lit_tree, code_lens, (int)hlit);
        build_huffman_tree(&dist_tree, code_lens + hlit, (int)hdist);
      }

      /* Decode symbols */
      for (;;) {
        int sym = huffman_decode(&br, &lit_tree);
        if (sym < 0) return -1;
        if (sym < 256) {
          if (out_pos >= out_max) return -1;
          out[out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
          break; /* End of block */
        } else {
          int len_idx = sym - 257;
          if (len_idx >= 29) return -1;
          uint32_t len = k_len_base[len_idx];
          if (k_len_extra[len_idx] > 0) {
            len += br_get_bits(&br, k_len_extra[len_idx]);
          }

          int dist_sym = huffman_decode(&br, &dist_tree);
          if (dist_sym < 0 || dist_sym >= 30) return -1;
          uint32_t dist = k_dist_base[dist_sym];
          if (k_dist_extra[dist_sym] > 0) {
            dist += br_get_bits(&br, k_dist_extra[dist_sym]);
          }

          if (dist > out_pos || out_pos + len > out_max) return -1;
          for (uint32_t k = 0; k < len; k++) {
            out[out_pos] = out[out_pos - dist];
            out_pos++;
          }
        }
      }
    } else {
      return -1; /* Invalid block type 3 */
    }
  }

  if (out_written) *out_written = out_pos;
  return 0;
}

/* PNG Scanline Unfiltering */

static uint8_t paeth(int a, int b, int c) {
  int p = a + b - c;
  int pa = p > a ? p - a : a - p;
  int pb = p > b ? p - b : b - p;
  int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return (uint8_t)a;
  if (pb <= pc) return (uint8_t)b;
  return (uint8_t)c;
}

int oops_png_decode(const void *png_data, size_t png_size,
                    uint32_t *out_pixels, uint32_t target_w, uint32_t target_h,
                    uint32_t *out_orig_w, uint32_t *out_orig_h) {
  oops_log_debug("PNG", "decode size=%zu target=%ux%u", png_size, target_w, target_h);
  if (!png_data || png_size < 33 || !out_pixels) {
    oops_log_warn("PNG", "decode: invalid arguments");
    return -1;
  }

  const uint8_t *p = (const uint8_t *)png_data;
  /* Check PNG signature */
  if (p[0] != 0x89 || p[1] != 'P' || p[2] != 'N' || p[3] != 'G' ||
      p[4] != 0x0D || p[5] != 0x0A || p[6] != 0x1A || p[7] != 0x0A) {
    oops_log_warn("PNG", "decode: invalid PNG header signature");
    return -2;
  }

  size_t pos = 8;
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bit_depth = 0;
  uint8_t color_type = 0;

  /* First pass: calculate total IDAT length and find IHDR */
  size_t idat_total = 0;
  while (pos + 8 <= png_size) {
    uint32_t chunk_len = read_be32(p + pos);
    const uint8_t *type = p + pos + 4;
    pos += 8;

    if (pos + chunk_len + 4 > png_size) return -3;

    if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R') {
      if (chunk_len < 13) return -4;
      width = read_be32(p + pos);
      height = read_be32(p + pos + 4);
      bit_depth = p[pos + 8];
      color_type = p[pos + 9];
    } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
      idat_total += chunk_len;
    } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
      break;
    }
    pos += chunk_len + 4; /* skip chunk data + CRC */
  }

  if (width == 0 || height == 0 || idat_total == 0) return -5;
  if (bit_depth != 8) return -6; /* only 8-bit supported */
  if (color_type != 2 && color_type != 6) return -7; /* RGB (2) or RGBA (6) */

  if (out_orig_w) *out_orig_w = width;
  if (out_orig_h) *out_orig_h = height;

  int bpp = (color_type == 6) ? 4 : 3;
  size_t scanline_bytes = (size_t)width * (size_t)bpp;
  size_t raw_size = ((size_t)scanline_bytes + 1u) * (size_t)height;

  /* Assemble IDAT stream */
  uint8_t *idat_buf = (uint8_t *)oops_malloc(idat_total);
  if (!idat_buf) return -8;

  pos = 8;
  size_t idat_copied = 0;
  while (pos + 8 <= png_size) {
    uint32_t chunk_len = read_be32(p + pos);
    const uint8_t *type = p + pos + 4;
    pos += 8;
    if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
      png_memcpy(idat_buf + idat_copied, p + pos, chunk_len);
      idat_copied += chunk_len;
    } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
      break;
    }
    pos += chunk_len + 4;
  }

  /* Allocate buffer for decompressed raw scanlines */
  uint8_t *raw_buf = (uint8_t *)oops_malloc(raw_size);
  if (!raw_buf) {
    oops_free(idat_buf);
    return -9;
  }

  /* Skip 2 bytes zlib header (CMF, FLG) */
  if (idat_copied < 6) {
    oops_free(idat_buf);
    oops_free(raw_buf);
    return -10;
  }
  size_t deflate_len = (idat_copied >= 6) ? (idat_copied - 6) : 0;
  size_t written = 0;
  int drc = deflate_decompress(idat_buf + 2, deflate_len, raw_buf, raw_size, &written);
  oops_free(idat_buf);

  if (drc != 0 || written < raw_size) {
    oops_free(raw_buf);
    return -11;
  }

  /* Unfilter scanlines */
  uint8_t *curr_line = raw_buf;
  uint8_t *prev_line = NULL;

  for (uint32_t y = 0; y < height; y++) {
    uint8_t filter = *curr_line++;
    for (size_t x = 0; x < scanline_bytes; x++) {
      uint8_t a = (x >= (size_t)bpp) ? curr_line[x - (size_t)bpp] : 0;
      uint8_t b = prev_line ? prev_line[x] : 0;
      uint8_t c = (prev_line && x >= (size_t)bpp) ? prev_line[x - (size_t)bpp] : 0;

      switch (filter) {
        case 0: /* None */ break;
        case 1: /* Sub */ curr_line[x] += a; break;
        case 2: /* Up */ curr_line[x] += b; break;
        case 3: /* Average */ curr_line[x] += (uint8_t)(((int)a + (int)b) / 2); break;
        case 4: /* Paeth */ curr_line[x] += paeth(a, b, c); break;
        default: break;
      }
    }
    prev_line = curr_line;
    curr_line += scanline_bytes;
  }

  /* Resample/Downscale into out_pixels (ARGB format) */
  uint32_t tw = (target_w > 0) ? target_w : width;
  uint32_t th = (target_h > 0) ? target_h : height;

  for (uint32_t dy = 0; dy < th; dy++) {
    uint32_t sy = (dy * height) / th;
    const uint8_t *src_row = raw_buf + (size_t)sy * (scanline_bytes + 1u) + 1u;
    for (uint32_t dx = 0; dx < tw; dx++) {
      uint32_t sx = (dx * width) / tw;
      const uint8_t *pix = src_row + (size_t)sx * (size_t)bpp;
      uint8_t r = pix[0];
      uint8_t g = pix[1];
      uint8_t b = pix[2];
      uint8_t a = (color_type == 6) ? pix[3] : 0xFF;
      out_pixels[dy * tw + dx] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
  }

  oops_log_debug("PNG", "decoded %ux%u -> %ux%u", width, height, tw, th);
  oops_free(raw_buf);
  return 0;
}
