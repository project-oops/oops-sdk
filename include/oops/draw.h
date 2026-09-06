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

/* Display surface extraction */
oops_surface_t oops_display_get_surface(oops_display_t *disp);

/* 2D canvas drawing primitives */
void oops_draw_clear(oops_surface_t *surf, oops_color_t color);
void oops_draw_pixel(oops_surface_t *surf, int x, int y, oops_color_t color);
void oops_draw_rect(oops_surface_t *surf, int x, int y, int w, int h, oops_color_t color);
void oops_draw_rect_blend(oops_surface_t *surf, int x, int y, int w, int h, oops_color_t color);
void oops_draw_line(oops_surface_t *surf, int x0, int y0, int x1, int y1, oops_color_t color);
void oops_draw_circle(oops_surface_t *surf, int cx, int cy, int radius, oops_color_t color, int filled);

/* Text rendering with 8x8 embedded console font */
int  oops_draw_text(oops_surface_t *surf, int x, int y, const char *text, oops_color_t color, int scale);

/* Blitting between surfaces */
void oops_draw_blit(oops_surface_t *dst, int dx, int dy,
                    const oops_surface_t *src, int sx, int sy, int sw, int sh);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_DRAW_H */

