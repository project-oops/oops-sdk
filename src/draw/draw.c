#include "oops/draw.h"
#include "agc/tiler.h"
#include <stddef.h>
#include <stdint.h>

/* The 8x8 bitmap font now lives in a shared header, so the GL overlay (`src/hud/hud.c`) bakes the
 * same glyphs into a texture that this rasteriser draws a pixel at a time. The local `FONT_*`
 * spellings below are kept so the body of this file reads unchanged. */
#include "font8x8.h"

#define FONT_FIRST  OOPS_FONT8X8_FIRST
#define FONT_LAST   OOPS_FONT8X8_LAST
#define FONT_WIDTH  OOPS_FONT8X8_WIDTH
#define FONT_HEIGHT OOPS_FONT8X8_HEIGHT

/* oops_display_get_surface is the display's (src/display.c): which buffer the
 * next flip shows is its to know. Nothing here reaches the display. */

/*
 * **A pixel's index in a surface, in either layout** (<oops/draw.h>). Linear
 * is rows `pitch` apart. OOPS_SURFACE_RX is 128 x 128 blocks row by row, `pitch`
 * pixels to a row of blocks, each block in the GPU's 64KB_R_X order.
 * agc_tile_pixel gives that order as the XOR of one term per coordinate bit, so
 * a pixel's place in its block is x's part XOR y's part. Each part comes from a
 * table of 128 built from agc_tile_pixel the first time a tiled surface is
 * drawn on. Building the tables twice at once would write the same values, so
 * concurrent first draws are harmless.
 */
static uint32_t s_rx_x[128], s_rx_y[128];
static int s_rx_ready;

static void oops_rx_tables(void) {
  for (uint32_t i = 0; i < 128u; i++) {
    s_rx_x[i] = agc_tile_pixel(i, 0);
    s_rx_y[i] = agc_tile_pixel(0, i);
  }
  s_rx_ready = 1;
}

static inline size_t oops_surf_index(const oops_surface_t *s, uint32_t x,
                                     uint32_t y) {
  if (s->layout == OOPS_SURFACE_RX) {
    if (!s_rx_ready)
      oops_rx_tables();
    const size_t block =
        (size_t)(y >> 7) * (size_t)(s->pitch >> 7) + (size_t)(x >> 7);
    return block * 16384u + (size_t)(s_rx_x[x & 127u] ^ s_rx_y[y & 127u]);
  }
  return (size_t)y * s->pitch + (size_t)x;
}

/* One row's span [x0, x1) set to `color`, or `color` composited over it. The
 * linear case keeps the row pointer it always had. */
static void oops_span_fill(oops_surface_t *s, int y, int x0, int x1,
                           oops_color_t color) {
  if (s->layout == OOPS_SURFACE_RX) {
    for (int x = x0; x < x1; x++)
      s->pixels[oops_surf_index(s, (uint32_t)x, (uint32_t)y)] = color;
    return;
  }
  uint32_t *row = s->pixels + (size_t)y * s->pitch;
  for (int x = x0; x < x1; x++)
    row[x] = color;
}

/* Straight-alpha source-over of one pixel: out = src.rgb * a + dst.rgb * (1 -
 * a), opaque. */
static inline uint32_t oops_src_over(uint32_t src, uint32_t dst) {
  uint32_t sa = (src >> 24) & 0xFF;
  if (sa == 0)
    return dst;
  if (sa == 255)
    return src;
  uint32_t inv = 255 - sa;
  uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
  uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
  uint32_t r = (sr * sa + dr * inv) / 255;
  uint32_t g = (sg * sa + dg * inv) / 255;
  uint32_t b = (sb * sa + db * inv) / 255;
  return (0xFFu << 24) | (r << 16) | (g << 8) | b;
}

oops_surface_t oops_surface_from_sprite(const oops_sprite_t *sprite) {
  oops_surface_t s = {NULL, 0, 0, 0, OOPS_SURFACE_LINEAR};
  if (!sprite || !sprite->pixels)
    return s;
  /* Read-only alias: the surface is a blit source only, never a draw target. */
  s.pixels = (uint32_t *)sprite->pixels;
  s.width = sprite->width;
  s.height = sprite->height;
  s.pitch = sprite->width;
  return s;
}

void oops_draw_clear(oops_surface_t *surf, oops_color_t color) {
  if (!surf || !surf->pixels)
    return;
  /* Row by row: a surface can be a window onto a wider buffer, so pitch is not
   * width. */
  for (uint32_t y = 0; y < surf->height; y++)
    oops_span_fill(surf, (int)y, 0, (int)surf->width, color);
}

void oops_draw_pixel(oops_surface_t *surf, int x, int y, oops_color_t color) {
  if (!surf || !surf->pixels)
    return;
  if (x < 0 || (uint32_t)x >= surf->width || y < 0 ||
      (uint32_t)y >= surf->height)
    return;
  surf->pixels[oops_surf_index(surf, (uint32_t)x, (uint32_t)y)] = color;
}

/* Clip [pos, pos + len) to [0, limit) in 64-bit, so absurd arguments clamp
 * instead of overflowing int. Returns 0 when nothing is left. */
static int oops_clip_span(int pos, int len, uint32_t limit, int *out0,
                          int *out1) {
  int64_t a = pos;
  int64_t b = (int64_t)pos + (int64_t)len;
  if (a < 0)
    a = 0;
  if (b > (int64_t)limit)
    b = (int64_t)limit;
  if (a >= b)
    return 0;
  *out0 = (int)a;
  *out1 = (int)b;
  return 1;
}

void oops_draw_rect(oops_surface_t *surf, int x, int y, int w, int h,
                    oops_color_t color) {
  if (!surf || !surf->pixels || w <= 0 || h <= 0)
    return;

  int x0, x1, y0, y1;
  if (!oops_clip_span(x, w, surf->width, &x0, &x1))
    return;
  if (!oops_clip_span(y, h, surf->height, &y0, &y1))
    return;

  for (int py = y0; py < y1; py++)
    oops_span_fill(surf, py, x0, x1, color);
}

void oops_draw_rect_blend(oops_surface_t *surf, int x, int y, int w, int h,
                          oops_color_t color) {
  if (!surf || !surf->pixels || w <= 0 || h <= 0)
    return;

  uint32_t sa = (color >> 24) & 0xFF;
  if (sa == 0)
    return;
  if (sa == 255) {
    oops_draw_rect(surf, x, y, w, h, color);
    return;
  }

  int x0, x1, y0, y1;
  if (!oops_clip_span(x, w, surf->width, &x0, &x1))
    return;
  if (!oops_clip_span(y, h, surf->height, &y0, &y1))
    return;

  for (int py = y0; py < y1; py++) {
    for (int px = x0; px < x1; px++) {
      uint32_t *p = &surf->pixels[oops_surf_index(surf, (uint32_t)px, (uint32_t)py)];
      *p = oops_src_over(color, *p);
    }
  }
}

void oops_draw_rect_gradient(oops_surface_t *surf, int x, int y, int w, int h,
                             oops_color_t color_a, oops_color_t color_b,
                             int vertical) {
  if (!surf || !surf->pixels || w <= 0 || h <= 0)
    return;

  int x0, x1, y0, y1;
  if (!oops_clip_span(x, w, surf->width, &x0, &x1))
    return;
  if (!oops_clip_span(y, h, surf->height, &y0, &y1))
    return;

  int aa = (int)((color_a >> 24) & 0xFF), ar = (int)((color_a >> 16) & 0xFF);
  int ag = (int)((color_a >> 8) & 0xFF), ab = (int)(color_a & 0xFF);
  int ba = (int)((color_b >> 24) & 0xFF), br = (int)((color_b >> 16) & 0xFF);
  int bg = (int)((color_b >> 8) & 0xFF), bb = (int)(color_b & 0xFF);

  /* Interpolate over the full requested span, not the clipped one, so a clipped
   * rectangle shows the same slice of the gradient it would unclipped. */
  int span = (vertical ? h : w) - 1;
  if (span < 1)
    span = 1;

  for (int py = y0; py < y1; py++) {
    for (int px = x0; px < x1; px++) {
      uint32_t *p = &surf->pixels[oops_surf_index(surf, (uint32_t)px, (uint32_t)py)];
      int t = vertical ? (py - y) : (px - x);
      if (t < 0)
        t = 0;
      if (t > span)
        t = span;
      uint32_t ca = (uint32_t)(aa + (ba - aa) * t / span);
      uint32_t cr = (uint32_t)(ar + (br - ar) * t / span);
      uint32_t cg = (uint32_t)(ag + (bg - ag) * t / span);
      uint32_t cb = (uint32_t)(ab + (bb - ab) * t / span);
      uint32_t c = (ca << 24) | (cr << 16) | (cg << 8) | cb;
      *p = oops_src_over(c, *p);
    }
  }
}

void oops_draw_line(oops_surface_t *surf, int x0, int y0, int x1, int y1,
                    oops_color_t color) {
  if (!surf || !surf->pixels)
    return;

  int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
  int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx - dy;

  for (;;) {
    oops_draw_pixel(surf, x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;

    int e2 = 2 * err;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void oops_draw_circle(oops_surface_t *surf, int cx, int cy, int radius,
                      oops_color_t color, int filled) {
  if (!surf || !surf->pixels || radius <= 0)
    return;

  int x = 0;
  int y = radius;
  int d = 3 - 2 * radius;

  while (y >= x) {
    if (filled) {
      oops_draw_rect(surf, cx - x, cy - y, 2 * x + 1, 1, color);
      oops_draw_rect(surf, cx - x, cy + y, 2 * x + 1, 1, color);
      oops_draw_rect(surf, cx - y, cy - x, 2 * y + 1, 1, color);
      oops_draw_rect(surf, cx - y, cy + x, 2 * y + 1, 1, color);
    } else {
      oops_draw_pixel(surf, cx + x, cy + y, color);
      oops_draw_pixel(surf, cx - x, cy + y, color);
      oops_draw_pixel(surf, cx + x, cy - y, color);
      oops_draw_pixel(surf, cx - x, cy - y, color);
      oops_draw_pixel(surf, cx + y, cy + x, color);
      oops_draw_pixel(surf, cx - y, cy + x, color);
      oops_draw_pixel(surf, cx + y, cy - x, color);
      oops_draw_pixel(surf, cx - y, cy - x, color);
    }
    x++;
    if (d > 0) {
      y--;
      d = d + 4 * (x - y) + 10;
    } else {
      d = d + 4 * x + 6;
    }
  }
}

int oops_draw_text(oops_surface_t *surf, int x, int y, const char *text,
                   oops_color_t color, int scale) {
  if (!surf || !surf->pixels || !text)
    return x;
  if (scale < 1)
    scale = 1;

  int cur_x = x;
  int cur_y = y;

  for (const char *p = text; *p != '\0'; p++) {
    char c = *p;
    if (c == '\n') {
      cur_x = x;
      cur_y += FONT_HEIGHT * scale;
      continue;
    }

    if (c < FONT_FIRST || c > FONT_LAST) {
      c = '?';
    }

    const uint8_t *glyph = oops_font8x8[c - FONT_FIRST];
    for (int r = 0; r < FONT_HEIGHT; r++) {
      uint8_t bits = glyph[r];
      for (int col = 0; col < FONT_WIDTH; col++) {
        if ((bits >> (7 - col)) & 1) {
          oops_draw_rect(surf, cur_x + col * scale, cur_y + r * scale, scale,
                         scale, color);
        }
      }
    }
    cur_x += FONT_WIDTH * scale;
  }
  return cur_x;
}

void oops_draw_pixel_blend(oops_surface_t *surf, int x, int y,
                           oops_color_t color) {
  if (!surf || !surf->pixels)
    return;
  if (x < 0 || (uint32_t)x >= surf->width || y < 0 ||
      (uint32_t)y >= surf->height)
    return;
  uint32_t *p = &surf->pixels[oops_surf_index(surf, (uint32_t)x, (uint32_t)y)];
  *p = oops_src_over(color, *p);
}

void oops_draw_line_blend(oops_surface_t *surf, int x0, int y0, int x1, int y1,
                          oops_color_t color) {
  if (!surf || !surf->pixels)
    return;
  int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
  int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx - dy;
  for (;;) {
    oops_draw_pixel_blend(surf, x0, y0, color);
    if (x0 == x1 && y0 == y1)
      break;
    int e2 = 2 * err;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void oops_draw_circle_blend(oops_surface_t *surf, int cx, int cy, int radius,
                            oops_color_t color, int filled) {
  if (!surf || !surf->pixels || radius <= 0)
    return;
  if (filled) {
    /* Scanline fill: one composited span per row, so no pixel is blended twice.
     */
    for (int dy = -radius; dy <= radius; dy++) {
      int rr = radius * radius - dy * dy;
      int hw = 0;
      while ((hw + 1) * (hw + 1) <= rr)
        hw++;
      int y = cy + dy;
      for (int x = cx - hw; x <= cx + hw; x++)
        oops_draw_pixel_blend(surf, x, y, color);
    }
    return;
  }
  /* Outline: 8-way midpoint. A translucent outline may composite twice at its
   * four cardinal and four diagonal extremes (a handful of pixels); negligible
   * for a 1px stroke. */
  int x = 0, y = radius, d = 3 - 2 * radius;
  while (y >= x) {
    oops_draw_pixel_blend(surf, cx + x, cy + y, color);
    oops_draw_pixel_blend(surf, cx - x, cy + y, color);
    oops_draw_pixel_blend(surf, cx + x, cy - y, color);
    oops_draw_pixel_blend(surf, cx - x, cy - y, color);
    oops_draw_pixel_blend(surf, cx + y, cy + x, color);
    oops_draw_pixel_blend(surf, cx - y, cy + x, color);
    oops_draw_pixel_blend(surf, cx + y, cy - x, color);
    oops_draw_pixel_blend(surf, cx - y, cy - x, color);
    x++;
    if (d > 0) {
      y--;
      d = d + 4 * (x - y) + 10;
    } else {
      d = d + 4 * x + 6;
    }
  }
}

int oops_draw_text_width(const char *text, int scale) {
  if (!text)
    return 0;
  if (scale < 1)
    scale = 1;
  int cur = 0, max = 0;
  for (const char *p = text; *p != '\0'; p++) {
    if (*p == '\n') {
      if (cur > max)
        max = cur;
      cur = 0;
      continue;
    }
    cur += FONT_WIDTH * scale;
  }
  if (cur > max)
    max = cur;
  return max;
}

void oops_draw_blit(oops_surface_t *dst, int dx, int dy,
                    const oops_surface_t *src, int sx, int sy, int sw, int sh) {
  if (!dst || !dst->pixels || !src || !src->pixels || sw <= 0 || sh <= 0)
    return;

  /* Clip source bounds */
  if (sx < 0) {
    sw += sx;
    dx -= sx;
    sx = 0;
  }
  if (sy < 0) {
    sh += sy;
    dy -= sy;
    sy = 0;
  }
  if ((int64_t)sx + sw > (int64_t)src->width)
    sw = (int)src->width - sx;
  if ((int64_t)sy + sh > (int64_t)src->height)
    sh = (int)src->height - sy;
  if (sw <= 0 || sh <= 0)
    return;

  /* Clip destination bounds */
  if (dx < 0) {
    sw += dx;
    sx -= dx;
    dx = 0;
  }
  if (dy < 0) {
    sh += dy;
    sy -= dy;
    dy = 0;
  }
  if ((int64_t)dx + sw > (int64_t)dst->width)
    sw = (int)dst->width - dx;
  if ((int64_t)dy + sh > (int64_t)dst->height)
    sh = (int)dst->height - dy;
  if (sw <= 0 || sh <= 0)
    return;

  for (int y = 0; y < sh; y++) {
    if (src->layout == OOPS_SURFACE_LINEAR && dst->layout == OOPS_SURFACE_LINEAR) {
      const uint32_t *src_row =
          src->pixels + (size_t)(sy + y) * src->pitch + (size_t)sx;
      uint32_t *dst_row =
          dst->pixels + (size_t)(dy + y) * dst->pitch + (size_t)dx;
      for (int x = 0; x < sw; x++) {
        dst_row[x] = src_row[x];
      }
      continue;
    }
    for (int x = 0; x < sw; x++) {
      dst->pixels[oops_surf_index(dst, (uint32_t)(dx + x), (uint32_t)(dy + y))] =
          src->pixels[oops_surf_index(src, (uint32_t)(sx + x), (uint32_t)(sy + y))];
    }
  }
}

void oops_draw_blit_blend(oops_surface_t *dst, int dx, int dy,
                          const oops_surface_t *src, int sx, int sy, int sw,
                          int sh) {
  if (!dst || !dst->pixels || !src || !src->pixels || sw <= 0 || sh <= 0)
    return;

  if (sx < 0) {
    sw += sx;
    dx -= sx;
    sx = 0;
  }
  if (sy < 0) {
    sh += sy;
    dy -= sy;
    sy = 0;
  }
  if ((int64_t)sx + sw > (int64_t)src->width)
    sw = (int)src->width - sx;
  if ((int64_t)sy + sh > (int64_t)src->height)
    sh = (int)src->height - sy;
  if (sw <= 0 || sh <= 0)
    return;

  if (dx < 0) {
    sw += dx;
    sx -= dx;
    dx = 0;
  }
  if (dy < 0) {
    sh += dy;
    sy -= dy;
    dy = 0;
  }
  if ((int64_t)dx + sw > (int64_t)dst->width)
    sw = (int)dst->width - dx;
  if ((int64_t)dy + sh > (int64_t)dst->height)
    sh = (int)dst->height - dy;
  if (sw <= 0 || sh <= 0)
    return;

  for (int y = 0; y < sh; y++) {
    for (int x = 0; x < sw; x++) {
      const uint32_t s =
          src->pixels[oops_surf_index(src, (uint32_t)(sx + x), (uint32_t)(sy + y))];
      uint32_t *p =
          &dst->pixels[oops_surf_index(dst, (uint32_t)(dx + x), (uint32_t)(dy + y))];
      *p = oops_src_over(s, *p);
    }
  }
}

void oops_draw_blit_scaled_blend(oops_surface_t *dst, int dx, int dy, int dw,
                                 int dh, const oops_surface_t *src, int sx,
                                 int sy, int sw, int sh) {
  if (!dst || !dst->pixels || !src || !src->pixels || sw <= 0 || sh <= 0 ||
      dw <= 0 || dh <= 0)
    return;

  int x0 = (dx < 0) ? 0 : dx;
  int y0 = (dy < 0) ? 0 : dy;
  int x1 = (dx + dw > (int)dst->width) ? (int)dst->width : (dx + dw);
  int y1 = (dy + dh > (int)dst->height) ? (int)dst->height : (dy + dh);
  if (x0 >= x1 || y0 >= y1)
    return;

  for (int y = y0; y < y1; y++) {
    int src_y = sy + ((y - dy) * sh) / dh;
    if (src_y < 0)
      src_y = 0;
    if (src_y >= (int)src->height)
      src_y = (int)src->height - 1;

    for (int x = x0; x < x1; x++) {
      int src_x = sx + ((x - dx) * sw) / dw;
      if (src_x < 0)
        src_x = 0;
      if (src_x >= (int)src->width)
        src_x = (int)src->width - 1;

      uint32_t s =
          src->pixels[oops_surf_index(src, (uint32_t)src_x, (uint32_t)src_y)];
      uint32_t *p =
          &dst->pixels[oops_surf_index(dst, (uint32_t)x, (uint32_t)y)];
      *p = oops_src_over(s, *p);
    }
  }
}
