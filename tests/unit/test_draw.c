#include "oops/draw.h"
#include "agc/tiler.h"
#include "tests/test_common.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Unit tests for the 2D software renderer, `oops/draw.h`, linear and tiled. */

/* Clear fills every pixel, and an out-of-bounds pixel write is ignored. */
static void test_draw_surface_clear_and_pixel(void) {
    uint32_t buf[32 * 32];
    oops_surface_t surf = {buf, 32, 32, 32, OOPS_SURFACE_LINEAR};

    oops_draw_clear(&surf, OOPS_COLOR_RED);
    for (int i = 0; i < 32 * 32; i++) {
        ASSERT_EQ(buf[i], OOPS_COLOR_RED);
    }

    oops_draw_pixel(&surf, 5, 5, OOPS_COLOR_BLUE);
    ASSERT_EQ(buf[5 * 32 + 5], OOPS_COLOR_BLUE);

    /* Out of bounds pixel setting must not crash or modify memory */
    oops_draw_pixel(&surf, -1, 5, OOPS_COLOR_GREEN);
    oops_draw_pixel(&surf, 32, 5, OOPS_COLOR_GREEN);
    oops_draw_pixel(&surf, 5, -1, OOPS_COLOR_GREEN);
    oops_draw_pixel(&surf, 5, 32, OOPS_COLOR_GREEN);
}

/* Rectangles are clipped to the surface. */
static void test_draw_rect_clipping(void) {
    uint32_t buf[20 * 20];
    oops_surface_t surf = {buf, 20, 20, 20, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&surf, OOPS_COLOR_BLACK);

    /* Normal rect inside */
    oops_draw_rect(&surf, 5, 5, 10, 10, OOPS_COLOR_WHITE);
    ASSERT_EQ(buf[5 * 20 + 5], OOPS_COLOR_WHITE);
    ASSERT_EQ(buf[14 * 20 + 14], OOPS_COLOR_WHITE);
    ASSERT_EQ(buf[4 * 20 + 5], OOPS_COLOR_BLACK);
    ASSERT_EQ(buf[15 * 20 + 14], OOPS_COLOR_BLACK);

    /* Overlapping boundary rect */
    oops_draw_rect(&surf, 15, 15, 10, 10, OOPS_COLOR_GREEN);
    ASSERT_EQ(buf[19 * 20 + 19], OOPS_COLOR_GREEN);

    /* Fully out of bounds rect */
    oops_draw_rect(&surf, -50, -50, 10, 10, OOPS_COLOR_RED);
    oops_draw_rect(&surf, 50, 50, 10, 10, OOPS_COLOR_RED);
}

/* Absurd extents clamp instead of overflowing: INT_MAX wide from x = 10 fills
 * to the edge, a rectangle that starts past the edge draws nothing, and one
 * that ends before 0 too. */
static void test_draw_rect_extreme_extents(void) {
    uint32_t buf[20 * 20];
    oops_surface_t surf = {buf, 20, 20, 20, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&surf, OOPS_COLOR_BLACK);

    oops_draw_rect(&surf, 10, 0, INT_MAX, 1, OOPS_COLOR_RED);
    ASSERT_EQ(buf[9], OOPS_COLOR_BLACK);
    ASSERT_EQ(buf[10], OOPS_COLOR_RED);
    ASSERT_EQ(buf[19], OOPS_COLOR_RED);
    ASSERT_EQ(buf[20], OOPS_COLOR_BLACK); /* row 1 untouched */

    oops_draw_rect(&surf, INT_MAX - 5, 0, 100, 1, OOPS_COLOR_GREEN);
    ASSERT_EQ(buf[19], OOPS_COLOR_RED);
    oops_draw_rect(&surf, INT_MIN, INT_MIN, INT_MAX, INT_MAX, OOPS_COLOR_BLUE);
    ASSERT_EQ(buf[0], OOPS_COLOR_BLACK);

    oops_draw_rect_blend(&surf, 5, 5, INT_MAX, INT_MAX, OOPS_RGBA(255, 255, 255, 128));
    ASSERT_NE(buf[5 * 20 + 5], OOPS_COLOR_BLACK);
    ASSERT_EQ(buf[4 * 20 + 4], OOPS_COLOR_BLACK);

    /* A source rectangle entirely past the edge blits nothing. */
    uint32_t before = buf[0];
    oops_draw_blit(&surf, 0, 0, &surf, INT_MAX - 1, 0, INT_MAX, 1);
    ASSERT_EQ(buf[0], before);
}

/* A 50% source blends source-over onto the destination. */
static void test_draw_alpha_blend(void) {
    uint32_t buf[10 * 10];
    oops_surface_t surf = {buf, 10, 10, 10, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&surf, OOPS_RGB(0, 0, 0)); /* Destination: solid black (0,0,0) */

    /* 50% transparent white source: (255, 255, 255, 128) */
    oops_color_t semi_white = OOPS_RGBA(255, 255, 255, 128);
    oops_draw_rect_blend(&surf, 0, 0, 10, 10, semi_white);

    /* Expected: (255 * 128 + 0 * 127) / 255 = 128 */
    uint32_t px = buf[0];
    uint32_t r = (px >> 16) & 0xFF;
    uint32_t g = (px >> 8) & 0xFF;
    uint32_t b = px & 0xFF;
    ASSERT_TRUE(r >= 127 && r <= 129);
    ASSERT_TRUE(g >= 127 && g <= 129);
    ASSERT_TRUE(b >= 127 && b <= 129);
}

/* A line reaches both endpoints and a filled circle covers its centre. */
static void test_draw_line_and_circle(void) {
    uint32_t buf[20 * 20];
    oops_surface_t surf = {buf, 20, 20, 20, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&surf, OOPS_COLOR_BLACK);

    /* Diagonal line */
    oops_draw_line(&surf, 0, 0, 19, 19, OOPS_COLOR_YELLOW);
    ASSERT_EQ(buf[0], OOPS_COLOR_YELLOW);
    ASSERT_EQ(buf[19 * 20 + 19], OOPS_COLOR_YELLOW);

    /* Circle midpoint center */
    oops_draw_circle(&surf, 10, 10, 4, OOPS_COLOR_CYAN, 1);
    ASSERT_EQ(buf[10 * 20 + 10], OOPS_COLOR_CYAN);
}

/* Text advances eight pixels a glyph, and a blit copies a sub-surface. */
static void test_draw_text_and_blit(void) {
    uint32_t buf[64 * 64];
    oops_surface_t surf = {buf, 64, 64, 64, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&surf, OOPS_COLOR_BLACK);

    int nx = oops_draw_text(&surf, 0, 0, "TEST", OOPS_COLOR_WHITE, 1);
    ASSERT_EQ(nx, 32);

    /* Blit sub-surface */
    uint32_t sprite_buf[8 * 8];
    oops_surface_t sprite = {sprite_buf, 8, 8, 8, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&sprite, OOPS_COLOR_MAGENTA);

    oops_draw_blit(&surf, 20, 20, &sprite, 0, 0, 8, 8);
    ASSERT_EQ(buf[20 * 64 + 20], OOPS_COLOR_MAGENTA);
    ASSERT_EQ(buf[27 * 64 + 27], OOPS_COLOR_MAGENTA);
}

/* A surface whose pitch exceeds its width is a window onto a wider buffer.
 * Clear must touch only the visible columns of each row, never the pixels
 * between rows. */
static void test_draw_clear_respects_pitch(void) {
    uint32_t buf[8 * 16];
    for (int i = 0; i < 8 * 16; i++)
        buf[i] = OOPS_COLOR_BLACK;
    oops_surface_t surf = {buf, 8, 8, 16,
                           OOPS_SURFACE_LINEAR}; /* 8 wide, 8 tall, rows 16 apart */

    oops_draw_clear(&surf, OOPS_COLOR_RED);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 16; x++) {
            ASSERT_EQ(buf[y * 16 + x], (x < 8) ? OOPS_COLOR_RED : OOPS_COLOR_BLACK);
        }
    }
}

/* A sprite wrapped from a const array blits without copying; blit_blend honours
 * the source's per-pixel alpha so a transparent pixel shows the destination
 * through. */
static void test_draw_sprite_and_blit_blend(void) {
    static const uint32_t sprite_px[4] = {
        OOPS_COLOR_RED, OOPS_RGBA(0, 0, 0, 0), /* opaque red, fully transparent */
        OOPS_RGBA(255, 255, 255, 128), OOPS_COLOR_BLUE /* 50% white, opaque blue */
    };
    oops_sprite_t sprite = {2, 2, sprite_px};
    oops_surface_t src = oops_surface_from_sprite(&sprite);
    ASSERT_TRUE(src.pixels != NULL);
    ASSERT_EQ(src.width, 2);
    ASSERT_EQ(src.pitch, 2);

    uint32_t buf[4 * 4];
    oops_surface_t dst = {buf, 4, 4, 4, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&dst, OOPS_COLOR_BLACK);

    oops_draw_blit_blend(&dst, 1, 1, &src, 0, 0, 2, 2);
    ASSERT_EQ(buf[1 * 4 + 1], OOPS_COLOR_RED);   /* opaque source wins */
    ASSERT_EQ(buf[1 * 4 + 2], OOPS_COLOR_BLACK); /* transparent: destination shows */
    ASSERT_EQ(buf[2 * 4 + 2], OOPS_COLOR_BLUE);  /* opaque blue */
    uint32_t blended = buf[2 * 4 + 1];           /* 50% white over black -> ~128 grey */
    ASSERT_TRUE(((blended >> 16) & 0xFF) >= 127 && ((blended >> 16) & 0xFF) <= 129);

    /* A straight blit ignores alpha and stamps the raw pixel. */
    oops_draw_clear(&dst, OOPS_COLOR_BLACK);
    oops_draw_blit(&dst, 0, 0, &src, 0, 0, 2, 2);
    ASSERT_EQ(buf[0 * 4 + 1],
              OOPS_RGBA(0, 0, 0, 0)); /* copied verbatim, not composited */
}

/* A horizontal gradient runs black to white monotonically. */
static void test_draw_gradient(void) {
    uint32_t buf[1 * 16];
    oops_surface_t s = {buf, 16, 1, 16, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&s, OOPS_COLOR_BLACK);
    /* Horizontal black->white across 16 px: left end black, right end white,
     * monotonic. */
    oops_draw_rect_gradient(&s, 0, 0, 16, 1, OOPS_RGB(0, 0, 0), OOPS_RGB(255, 255, 255),
                            0);
    ASSERT_EQ(buf[0] & 0xFF, 0);
    ASSERT_EQ(buf[15] & 0xFF, 255);
    ASSERT_TRUE((buf[8] & 0xFF) > (buf[4] & 0xFF));
}

/* Text width follows the longest line, and lower case has its own glyphs. */
static void test_draw_text_width_and_lowercase(void) {
    /* Longest line drives the width; \n resets. "AB\nCDE" -> 3 glyphs * 8 = 24 at
     * scale 1. */
    ASSERT_EQ(oops_draw_text_width("AB\nCDE", 1), 24);
    ASSERT_EQ(oops_draw_text_width("hi", 2), 2 * 8 * 2);
    ASSERT_EQ(oops_draw_text_width(NULL, 1), 0);

    /* Lower case is not folded to upper: 'a' and 'A' render differently, and 'a' is
     * not the blank glyph. */
    uint32_t a[8 * 8], A[8 * 8];
    oops_surface_t sa = {a, 8, 8, 8, OOPS_SURFACE_LINEAR},
                   sA = {A, 8, 8, 8, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&sa, OOPS_COLOR_BLACK);
    oops_draw_clear(&sA, OOPS_COLOR_BLACK);
    oops_draw_text(&sa, 0, 0, "a", OOPS_COLOR_WHITE, 1);
    oops_draw_text(&sA, 0, 0, "A", OOPS_COLOR_WHITE, 1);
    int lit_a = 0, differ = 0;
    for (int i = 0; i < 8 * 8; i++) {
        if (a[i] == OOPS_COLOR_WHITE)
            lit_a = 1;
        if (a[i] != A[i])
            differ = 1;
    }
    ASSERT_TRUE(lit_a);
    ASSERT_TRUE(differ);

    /* And 'a' must be its own glyph, not the '?' substitution an out-of-range
     * char would give. */
    uint32_t q[8 * 8];
    oops_surface_t sq = {q, 8, 8, 8, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&sq, OOPS_COLOR_BLACK);
    oops_draw_text(&sq, 0, 0, "?", OOPS_COLOR_WHITE, 1);
    int not_qmark = 0;
    for (int i = 0; i < 8 * 8; i++)
        if (a[i] != q[i])
            not_qmark = 1;
    ASSERT_TRUE(not_qmark);
}

/* Blended pixel/line/circle composite source-over; a transparent colour leaves
 * the destination, a 50% colour blends toward it, and the filled circle never
 * double-composites its centre. */
static void test_draw_blend_primitives(void) {
    uint32_t buf[16 * 16];
    oops_surface_t s = {buf, 16, 16, 16, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&s, OOPS_COLOR_BLACK);

    oops_draw_pixel_blend(&s, 1, 1,
                          OOPS_RGBA(255, 255, 255, 0)); /* transparent: no change */
    ASSERT_EQ(buf[1 * 16 + 1], OOPS_COLOR_BLACK);
    oops_draw_pixel_blend(&s, 2, 2,
                          OOPS_RGBA(255, 255, 255, 128)); /* 50% white over black */
    ASSERT_TRUE(((buf[2 * 16 + 2] >> 16) & 0xFF) >= 127 &&
                ((buf[2 * 16 + 2] >> 16) & 0xFF) <= 129);

    /* A 50% line over black: every touched pixel is ~grey, none left black on the
     * path ends. */
    oops_draw_line_blend(&s, 0, 0, 15, 15, OOPS_RGBA(255, 255, 255, 128));
    ASSERT_TRUE(((buf[0] >> 16) & 0xFF) >= 127 && ((buf[0] >> 16) & 0xFF) <= 129);
    ASSERT_TRUE(((buf[15 * 16 + 15] >> 16) & 0xFF) >= 127);

    /* Filled 50% circle: centre composites exactly once (a double-blend would
     * over-darken it below ~128); one clean blend leaves ~128. */
    oops_draw_clear(&s, OOPS_COLOR_BLACK);
    oops_draw_circle_blend(&s, 8, 8, 4, OOPS_RGBA(255, 255, 255, 128), 1);
    uint32_t centre = buf[8 * 16 + 8];
    ASSERT_TRUE(((centre >> 16) & 0xFF) >= 127 && ((centre >> 16) & 0xFF) <= 129);
    ASSERT_EQ(buf[0], OOPS_COLOR_BLACK); /* corner outside the circle untouched */
}

/* The PNG decoder refuses bad arguments and decodes a 1x1 RGBA image. */
static void test_draw_png_decode(void) {
    /* Null / invalid parameter rejections */
    uint32_t out[16];
    ASSERT_EQ(oops_png_decode(NULL, 0, out, 1, 1, NULL, NULL), -1);
    ASSERT_EQ(oops_png_decode("invalid", 7, out, 1, 1, NULL, NULL), -1);

    /* A minimal valid 1x1 RGBA PNG with a red pixel (255, 0, 0, 255) */
    static const uint8_t min_png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
        0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
        0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

    uint32_t orig_w = 0, orig_h = 0;
    uint32_t decoded_pixel = 0;
    int rc = oops_png_decode(min_png, sizeof(min_png), &decoded_pixel, 1, 1, &orig_w,
                             &orig_h);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(orig_w, 1u);
    ASSERT_EQ(orig_h, 1u);
    /* 32bpp ARGB: 0xFFFF0000 (red) */
    ASSERT_EQ(decoded_pixel, 0xFFFF0000u);
}

/* One scene through every drawing call, onto whichever surface. */
static void draw_rx_scene(oops_surface_t *s, const oops_surface_t *sprite) {
    oops_draw_clear(s, 0xff102030u);
    oops_draw_rect(s, 3, 4, 150, 20, OOPS_COLOR_RED);
    oops_draw_rect(s, 190, 140, 40, 40, OOPS_COLOR_GREEN); /* clipped at both edges */
    oops_draw_rect_blend(s, 100, 10, 90, 130, 0x80ffffffu);
    oops_draw_rect_gradient(s, 10, 30, 120, 60, 0xff0000ffu, 0x80ff0000u, 1);
    oops_draw_line(s, 0, 149, 199, 0, OOPS_COLOR_YELLOW);
    oops_draw_line_blend(s, 0, 0, 199, 149, 0x80ff00ffu);
    oops_draw_circle(s, 60, 110, 30, OOPS_COLOR_CYAN, 1);
    oops_draw_circle(s, 150, 70, 45, OOPS_COLOR_WHITE, 0);
    oops_draw_circle_blend(s, 128, 128, 20, 0x60ff8000u, 1);
    oops_draw_pixel(s, 127, 127, OOPS_COLOR_BLUE);
    oops_draw_pixel(s, 128, 128, OOPS_COLOR_BLUE);
    oops_draw_pixel_blend(s, 129, 127, 0x80000000u);
    oops_draw_text(s, 20, 130, "Tiled Q9 @ 64KB_R_X", OOPS_COLOR_WHITE, 1);
    oops_draw_text(s, 110, 100, "RX", OOPS_COLOR_GREEN, 3);
    oops_draw_blit(s, 170, 120, sprite, 0, 0, 16, 16);
    oops_draw_blit_blend(s, 120, 125, sprite, 4, 4, 12, 12);
}

/* Drawing in the GPU's 64KB_R_X layout (OOPS_SURFACE_RX): the same scene drawn on a
 * linear surface and on a tiled one comes out the same pixel for pixel, the tiled one
 * read through agc_tile_pixel. It is 200 x 150, so blocks reach past it on both axes,
 * and the padding the scene cannot reach keeps its fill. Blits run both ways between
 * the layouts. */
static void test_draw_rx_layout_matches_linear(void) {
    enum { W = 200, H = 150, PITCH = 256, WORDS = 4 * 16384 };
    uint32_t *lin = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    uint32_t *rx = (uint32_t *)malloc((size_t)WORDS * sizeof(uint32_t));
    uint32_t *back = (uint32_t *)calloc((size_t)W * H, sizeof(uint32_t));
    static uint32_t sprite_px[16 * 16];
    ASSERT_TRUE(lin && rx && back);
    for (int i = 0; i < 16 * 16; i++)
        sprite_px[i] = (i & 1) ? 0x80ff00ffu : 0xff00ff00u;
    oops_surface_t sprite = {sprite_px, 16, 16, 16, OOPS_SURFACE_LINEAR};
    oops_surface_t sl = {lin, W, H, W, OOPS_SURFACE_LINEAR};
    oops_surface_t sr = {rx, W, H, PITCH, OOPS_SURFACE_RX};
    for (size_t i = 0; i < WORDS; i++)
        rx[i] = 0x5a5a5a5au;

    draw_rx_scene(&sl, &sprite);
    draw_rx_scene(&sr, &sprite);

    size_t differ = 0;
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            const size_t t = (size_t)((y >> 7) * 2u + (x >> 7)) * 16384u +
                             agc_tile_pixel(x & 127u, y & 127u);
            differ += rx[t] != lin[y * W + x];
        }
    }
    ASSERT_EQ(differ, 0u);
    size_t padding = 0;
    for (size_t i = 0; i < WORDS; i++)
        padding += rx[i] == 0x5a5a5a5au;
    ASSERT_EQ(padding, (size_t)WORDS - (size_t)W * H);

    /* And out again: a tiled source blitted onto a linear surface is the linear scene.
     */
    oops_surface_t sb = {back, W, H, W, OOPS_SURFACE_LINEAR};
    oops_draw_blit(&sb, 0, 0, &sr, 0, 0, W, H);
    ASSERT_EQ(memcmp(back, lin, (size_t)W * H * sizeof(uint32_t)), 0);

    free(back);
    free(rx);
    free(lin);
}

void run_unit_tests_draw(void) {

    TEST_SUITE_BEGIN("2D Graphics Canvas & Primitives");
    RUN_TEST(test_draw_surface_clear_and_pixel);
    RUN_TEST(test_draw_clear_respects_pitch);
    RUN_TEST(test_draw_rect_clipping);
    RUN_TEST(test_draw_rect_extreme_extents);
    RUN_TEST(test_draw_alpha_blend);
    RUN_TEST(test_draw_line_and_circle);
    RUN_TEST(test_draw_text_and_blit);
    RUN_TEST(test_draw_sprite_and_blit_blend);
    RUN_TEST(test_draw_gradient);
    RUN_TEST(test_draw_text_width_and_lowercase);
    RUN_TEST(test_draw_blend_primitives);
    RUN_TEST(test_draw_png_decode);
    RUN_TEST(test_draw_rx_layout_matches_linear);
}
