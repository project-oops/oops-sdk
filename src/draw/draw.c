#include "oops/draw.h"
#include <stdint.h>
#include <stddef.h>

/* Embedded 8x8 bitmap font (ASCII 0x20 ' ' through 0x5F '_') */
#define FONT_FIRST 0x20
#define FONT_LAST  0x5F
#define FONT_WIDTH  8
#define FONT_HEIGHT 8

static const uint8_t s_font[64][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ' ' */
    {0x30, 0x30, 0x30, 0x30, 0x30, 0x00, 0x30, 0x00}, /* '!' */
    {0x6C, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* '"' */
    {0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00}, /* '#' */
    {0x30, 0x7C, 0xC0, 0x78, 0x0C, 0xF8, 0x30, 0x00}, /* '$' */
    {0x00, 0xC6, 0xCC, 0x18, 0x30, 0x66, 0xC6, 0x00}, /* '%' */
    {0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00}, /* '&' */
    {0x60, 0x30, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ''' */
    {0x18, 0x30, 0x60, 0x60, 0x60, 0x30, 0x18, 0x00}, /* '(' */
    {0x60, 0x30, 0x18, 0x18, 0x18, 0x30, 0x60, 0x00}, /* ')' */
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, /* '*' */
    {0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00}, /* '+' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30}, /* ',' */
    {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}, /* '-' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}, /* '.' */
    {0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00}, /* '/' */
    {0x7C, 0xC6, 0xCE, 0xD6, 0xE6, 0xC6, 0x7C, 0x00}, /* '0' */
    {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, /* '1' */
    {0x7C, 0xC6, 0x06, 0x1C, 0x30, 0x60, 0xFE, 0x00}, /* '2' */
    {0x7C, 0xC6, 0x06, 0x3C, 0x06, 0xC6, 0x7C, 0x00}, /* '3' */
    {0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x0C, 0x00}, /* '4' */
    {0xFE, 0xC0, 0xF8, 0x06, 0x06, 0xC6, 0x7C, 0x00}, /* '5' */
    {0x38, 0x60, 0xC0, 0xF8, 0xC6, 0xC6, 0x7C, 0x00}, /* '6' */
    {0xFE, 0xC6, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00}, /* '7' */
    {0x7C, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0x7C, 0x00}, /* '8' */
    {0x7C, 0xC6, 0xC6, 0x7E, 0x06, 0x0C, 0x78, 0x00}, /* '9' */
    {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00}, /* ':' */
    {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x30, 0x00}, /* ';' */
    {0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00}, /* '<' */
    {0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00}, /* '=' */
    {0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00}, /* '>' */
    {0x7C, 0xC6, 0x0C, 0x18, 0x18, 0x00, 0x18, 0x00}, /* '?' */
    {0x7C, 0xC6, 0xDE, 0xDE, 0xDC, 0xC0, 0x7C, 0x00}, /* '@' */
    {0x38, 0x6C, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0x00}, /* 'A' */
    {0xFC, 0x66, 0x66, 0x7C, 0x66, 0x66, 0xFC, 0x00}, /* 'B' */
    {0x3C, 0x66, 0xC0, 0xC0, 0xC0, 0x66, 0x3C, 0x00}, /* 'C' */
    {0xF8, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0x00}, /* 'D' */
    {0xFE, 0x62, 0x68, 0x78, 0x68, 0x62, 0xFE, 0x00}, /* 'E' */
    {0xFE, 0x62, 0x68, 0x78, 0x68, 0x60, 0xF0, 0x00}, /* 'F' */
    {0x3C, 0x66, 0xC0, 0xC0, 0xCE, 0x66, 0x3E, 0x00}, /* 'G' */
    {0xC6, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0x00}, /* 'H' */
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}, /* 'I' */
    {0x1E, 0x06, 0x06, 0x06, 0xC6, 0xC6, 0x7C, 0x00}, /* 'J' */
    {0xC6, 0xCC, 0xD8, 0xF0, 0xD8, 0xCC, 0xC6, 0x00}, /* 'K' */
    {0xE0, 0x60, 0x60, 0x60, 0x62, 0x66, 0xFE, 0x00}, /* 'L' */
    {0xC6, 0xEE, 0xFE, 0xD6, 0xC6, 0xC6, 0xC6, 0x00}, /* 'M' */
    {0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00}, /* 'N' */
    {0x7C, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00}, /* 'O' */
    {0xFC, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00}, /* 'P' */
    {0x7C, 0xC6, 0xC6, 0xC6, 0xD6, 0xDE, 0x7C, 0x0E}, /* 'Q' */
    {0xFC, 0x66, 0x66, 0x7C, 0xD8, 0xCC, 0xC6, 0x00}, /* 'R' */
    {0x7C, 0xC6, 0x60, 0x38, 0x0C, 0xC6, 0x7C, 0x00}, /* 'S' */
    {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}, /* 'T' */
    {0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0x7C, 0x00}, /* 'U' */
    {0xC6, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x10, 0x00}, /* 'V' */
    {0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00}, /* 'W' */
    {0xC6, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0xC6, 0x00}, /* 'X' */
    {0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00}, /* 'Y' */
    {0xFE, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xFE, 0x00}, /* 'Z' */
    {0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00}, /* '[' */
    {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x00}, /* '\' */
    {0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00}, /* ']' */
    {0x18, 0x3C, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00}, /* '^' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00}  /* '_' */
};

oops_surface_t oops_display_get_surface(oops_display_t *disp) {
    oops_surface_t surf = { NULL, 0, 0, 0 };
    if (!disp) return surf;

    surf.pixels = oops_display_get_framebuffer(disp);
    surf.width = oops_display_get_width(disp);
    surf.height = oops_display_get_height(disp);
    surf.pitch = surf.width;
    return surf;
}

void oops_draw_clear(oops_surface_t *surf, oops_color_t color) {
    if (!surf || !surf->pixels) return;
    /* Row by row: a surface can be a window onto a wider buffer, so pitch is not width. */
    for (uint32_t y = 0; y < surf->height; y++) {
        uint32_t *row = surf->pixels + (size_t)y * surf->pitch;
        for (uint32_t x = 0; x < surf->width; x++) {
            row[x] = color;
        }
    }
}

void oops_draw_pixel(oops_surface_t *surf, int x, int y, oops_color_t color) {
    if (!surf || !surf->pixels) return;
    if (x < 0 || (uint32_t)x >= surf->width || y < 0 || (uint32_t)y >= surf->height) return;
    surf->pixels[(size_t)y * surf->pitch + (size_t)x] = color;
}

/* Clip [pos, pos + len) to [0, limit) in 64-bit, so absurd arguments clamp instead of
 * overflowing int. Returns 0 when nothing is left. */
static int oops_clip_span(int pos, int len, uint32_t limit, int *out0, int *out1) {
    int64_t a = pos;
    int64_t b = (int64_t)pos + (int64_t)len;
    if (a < 0) a = 0;
    if (b > (int64_t)limit) b = (int64_t)limit;
    if (a >= b) return 0;
    *out0 = (int)a;
    *out1 = (int)b;
    return 1;
}

void oops_draw_rect(oops_surface_t *surf, int x, int y, int w, int h, oops_color_t color) {
    if (!surf || !surf->pixels || w <= 0 || h <= 0) return;

    int x0, x1, y0, y1;
    if (!oops_clip_span(x, w, surf->width, &x0, &x1)) return;
    if (!oops_clip_span(y, h, surf->height, &y0, &y1)) return;

    for (int py = y0; py < y1; py++) {
        uint32_t *row = surf->pixels + (size_t)py * surf->pitch;
        for (int px = x0; px < x1; px++) {
            row[px] = color;
        }
    }
}

void oops_draw_rect_blend(oops_surface_t *surf, int x, int y, int w, int h, oops_color_t color) {
    if (!surf || !surf->pixels || w <= 0 || h <= 0) return;

    uint32_t sa = (color >> 24) & 0xFF;
    if (sa == 0) return;
    if (sa == 255) {
        oops_draw_rect(surf, x, y, w, h, color);
        return;
    }

    uint32_t sr = (color >> 16) & 0xFF;
    uint32_t sg = (color >> 8) & 0xFF;
    uint32_t sb = color & 0xFF;
    uint32_t inv_a = 255 - sa;

    int x0, x1, y0, y1;
    if (!oops_clip_span(x, w, surf->width, &x0, &x1)) return;
    if (!oops_clip_span(y, h, surf->height, &y0, &y1)) return;

    for (int py = y0; py < y1; py++) {
        uint32_t *row = surf->pixels + (size_t)py * surf->pitch;
        for (int px = x0; px < x1; px++) {
            uint32_t dst = row[px];
            uint32_t dr = (dst >> 16) & 0xFF;
            uint32_t dg = (dst >> 8) & 0xFF;
            uint32_t db = dst & 0xFF;

            uint32_t out_r = (sr * sa + dr * inv_a) / 255;
            uint32_t out_g = (sg * sa + dg * inv_a) / 255;
            uint32_t out_b = (sb * sa + db * inv_a) / 255;

            row[px] = (0xFFu << 24) | (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}

void oops_draw_line(oops_surface_t *surf, int x0, int y0, int x1, int y1, oops_color_t color) {
    if (!surf || !surf->pixels) return;

    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        oops_draw_pixel(surf, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;

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

void oops_draw_circle(oops_surface_t *surf, int cx, int cy, int radius, oops_color_t color, int filled) {
    if (!surf || !surf->pixels || radius <= 0) return;

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

int oops_draw_text(oops_surface_t *surf, int x, int y, const char *text, oops_color_t color, int scale) {
    if (!surf || !surf->pixels || !text) return x;
    if (scale < 1) scale = 1;

    int cur_x = x;
    int cur_y = y;

    for (const char *p = text; *p != '\0'; p++) {
        char c = *p;
        if (c == '\n') {
            cur_x = x;
            cur_y += FONT_HEIGHT * scale;
            continue;
        }

        /* Fold lowercase to uppercase */
        if (c >= 'a' && c <= 'z') c -= 32;

        if (c < FONT_FIRST || c > FONT_LAST) {
            c = '?';
        }

        const uint8_t *glyph = s_font[c - FONT_FIRST];
        for (int r = 0; r < FONT_HEIGHT; r++) {
            uint8_t bits = glyph[r];
            for (int col = 0; col < FONT_WIDTH; col++) {
                if ((bits >> (7 - col)) & 1) {
                    oops_draw_rect(surf, cur_x + col * scale, cur_y + r * scale, scale, scale, color);
                }
            }
        }
        cur_x += FONT_WIDTH * scale;
    }
    return cur_x;
}

void oops_draw_blit(oops_surface_t *dst, int dx, int dy,
                    const oops_surface_t *src, int sx, int sy, int sw, int sh) {
    if (!dst || !dst->pixels || !src || !src->pixels || sw <= 0 || sh <= 0) return;

    /* Clip source bounds */
    if (sx < 0) { sw += sx; dx -= sx; sx = 0; }
    if (sy < 0) { sh += sy; dy -= sy; sy = 0; }
    if ((int64_t)sx + sw > (int64_t)src->width) sw = (int)src->width - sx;
    if ((int64_t)sy + sh > (int64_t)src->height) sh = (int)src->height - sy;
    if (sw <= 0 || sh <= 0) return;

    /* Clip destination bounds */
    if (dx < 0) { sw += dx; sx -= dx; dx = 0; }
    if (dy < 0) { sh += dy; sy -= dy; dy = 0; }
    if ((int64_t)dx + sw > (int64_t)dst->width) sw = (int)dst->width - dx;
    if ((int64_t)dy + sh > (int64_t)dst->height) sh = (int)dst->height - dy;
    if (sw <= 0 || sh <= 0) return;

    for (int y = 0; y < sh; y++) {
        const uint32_t *src_row = src->pixels + (size_t)(sy + y) * src->pitch + (size_t)sx;
        uint32_t *dst_row = dst->pixels + (size_t)(dy + y) * dst->pitch + (size_t)dx;
        for (int x = 0; x < sw; x++) {
            dst_row[x] = src_row[x];
        }
    }
}
