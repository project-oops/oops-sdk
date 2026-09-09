#ifndef OOPS_DRAW_H
#define OOPS_DRAW_H

#include <stdint.h>
#include <stddef.h>
#include "oops/display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t oops_color_t;

/* Standard 32bpp ARGB / XRGB color helpers */
#define OOPS_COLOR_BLACK   0xFF000000u
#define OOPS_COLOR_WHITE   0xFFFFFFFFu
#define OOPS_COLOR_RED     0xFFFF0000u
#define OOPS_COLOR_GREEN   0xFF00FF00u
#define OOPS_COLOR_BLUE    0xFF0000FFu
#define OOPS_COLOR_YELLOW  0xFFFFFF00u
#define OOPS_COLOR_CYAN    0xFF00FFFFu
#define OOPS_COLOR_MAGENTA 0xFFFF00FFu
#define OOPS_COLOR_GRAY    0xFF808080u

#define OOPS_RGB(r, g, b) \
    (((uint32_t)0xFF << 24) | (((uint32_t)(r) & 0xFF) << 16) | (((uint32_t)(g) & 0xFF) << 8) | ((uint32_t)(b) & 0xFF))

#define OOPS_RGBA(r, g, b, a) \
    ((((uint32_t)(a) & 0xFF) << 24) | (((uint32_t)(r) & 0xFF) << 16) | (((uint32_t)(g) & 0xFF) << 8) | ((uint32_t)(b) & 0xFF))

typedef struct oops_surface {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch; /* Row width in pixels */
} oops_surface_t;

/*
 * A sprite is image data compiled into the payload: 32-bit pixels in the surface's own order
 * (build them with OOPS_RGBA), row-major, width*height of them, so a sprite or a sprite sheet
 * lives in .rodata. Wrap one as a read-only source surface and blit it; address a single cell
 * of a sheet with the (sx, sy, sw, sh) sub-rectangle of oops_draw_blit / oops_draw_blit_blend.
 * No decoder: converting a PNG to this array is a build-time job for the app.
 */
typedef struct oops_sprite {
    uint32_t width;
    uint32_t height;
    const uint32_t *pixels;
} oops_sprite_t;

/* Display surface extraction */
oops_surface_t oops_display_get_surface(oops_display_t *disp);

/*
 * Present a sprite as a source surface without copying - the result aliases the sprite's const
 * pixels, so it is READ-ONLY: use it only as a blit source, never as a draw target. Returns an
 * empty surface (NULL pixels) for a NULL sprite.
 */
oops_surface_t oops_surface_from_sprite(const oops_sprite_t *sprite);

/* 2D canvas drawing primitives */
void oops_draw_clear(oops_surface_t *surf, oops_color_t color);
void oops_draw_pixel(oops_surface_t *surf, int x, int y, oops_color_t color);
void oops_draw_rect(oops_surface_t *surf, int x, int y, int w, int h, oops_color_t color);

/*
 * Fill a rectangle compositing `color` over what is there, straight-alpha source-over: the
 * output is out = src.rgb * a + dst.rgb * (1 - a), left opaque. Alpha 0 draws nothing, 255 is
 * a solid fill.
 */
void oops_draw_rect_blend(oops_surface_t *surf, int x, int y, int w, int h, oops_color_t color);

/*
 * Fill a rectangle with a linear gradient from `color_a` to `color_b`, top-to-bottom when
 * `vertical` is non-zero and left-to-right otherwise. Each pixel composites source-over, so
 * translucent endpoints layer over what is beneath.
 */
void oops_draw_rect_gradient(oops_surface_t *surf, int x, int y, int w, int h,
                             oops_color_t color_a, oops_color_t color_b, int vertical);

void oops_draw_line(oops_surface_t *surf, int x0, int y0, int x1, int y1, oops_color_t color);
void oops_draw_circle(oops_surface_t *surf, int cx, int cy, int radius, oops_color_t color, int filled);

/*
 * Text with the embedded console font (8x8, ASCII 0x20 through 0x7E, upper and lower case).
 * `scale` multiplies the cell. Returns the x after the last glyph.
 */
int  oops_draw_text(oops_surface_t *surf, int x, int y, const char *text, oops_color_t color, int scale);

/* Width in pixels oops_draw_text would occupy at `scale` (the longest line, if multi-line), so
 * a consumer can centre or right-align without counting characters itself. */
int  oops_draw_text_width(const char *text, int scale);

/*
 * Blit a source rectangle into the destination. oops_draw_blit is a straight copy; the source
 * alpha is ignored. oops_draw_blit_blend composites each source pixel over the destination with
 * its own alpha (straight-alpha source-over), so a sprite with a transparent margin layers
 * instead of stamping an opaque rectangle. (sx, sy, sw, sh) is the sheet cell to draw.
 */
void oops_draw_blit(oops_surface_t *dst, int dx, int dy,
                    const oops_surface_t *src, int sx, int sy, int sw, int sh);
void oops_draw_blit_blend(oops_surface_t *dst, int dx, int dy,
                          const oops_surface_t *src, int sx, int sy, int sw, int sh);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_DRAW_H */
