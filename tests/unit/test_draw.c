#include "tests/test_common.h"
#include "oops/draw.h"
#include <limits.h>

static void test_draw_surface_clear_and_pixel(void) {
    uint32_t buf[32 * 32];
    oops_surface_t surf = { buf, 32, 32, 32 };

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

static void test_draw_rect_clipping(void) {
    uint32_t buf[20 * 20];
    oops_surface_t surf = { buf, 20, 20, 20 };
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

/* Absurd extents clamp instead of overflowing: INT_MAX wide from x = 10 fills to the edge,
 * a rectangle that starts past the edge draws nothing, and one that ends before 0 too. */
static void test_draw_rect_extreme_extents(void) {
    uint32_t buf[20 * 20];
    oops_surface_t surf = { buf, 20, 20, 20 };
    oops_draw_clear(&surf, OOPS_COLOR_BLACK);

    oops_draw_rect(&surf, 10, 0, INT_MAX, 1, OOPS_COLOR_RED);
    ASSERT_EQ(buf[9], OOPS_COLOR_BLACK);
    ASSERT_EQ(buf[10], OOPS_COLOR_RED);
    ASSERT_EQ(buf[19], OOPS_COLOR_RED);
    ASSERT_EQ(buf[20], OOPS_COLOR_BLACK);   /* row 1 untouched */

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

static void test_draw_alpha_blend(void) {
    uint32_t buf[10 * 10];
    oops_surface_t surf = { buf, 10, 10, 10 };
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

static void test_draw_line_and_circle(void) {
    uint32_t buf[20 * 20];
    oops_surface_t surf = { buf, 20, 20, 20 };
    oops_draw_clear(&surf, OOPS_COLOR_BLACK);

    /* Diagonal line */
    oops_draw_line(&surf, 0, 0, 19, 19, OOPS_COLOR_YELLOW);
    ASSERT_EQ(buf[0], OOPS_COLOR_YELLOW);
    ASSERT_EQ(buf[19 * 20 + 19], OOPS_COLOR_YELLOW);

    /* Circle midpoint center */
    oops_draw_circle(&surf, 10, 10, 4, OOPS_COLOR_CYAN, 1);
    ASSERT_EQ(buf[10 * 20 + 10], OOPS_COLOR_CYAN);
}

static void test_draw_text_and_blit(void) {
    uint32_t buf[64 * 64];
    oops_surface_t surf = { buf, 64, 64, 64 };
    oops_draw_clear(&surf, OOPS_COLOR_BLACK);

    int nx = oops_draw_text(&surf, 0, 0, "TEST", OOPS_COLOR_WHITE, 1);
    ASSERT_EQ(nx, 32);

    /* Blit sub-surface */
    uint32_t sprite_buf[8 * 8];
    oops_surface_t sprite = { sprite_buf, 8, 8, 8 };
    oops_draw_clear(&sprite, OOPS_COLOR_MAGENTA);

    oops_draw_blit(&surf, 20, 20, &sprite, 0, 0, 8, 8);
    ASSERT_EQ(buf[20 * 64 + 20], OOPS_COLOR_MAGENTA);
    ASSERT_EQ(buf[27 * 64 + 27], OOPS_COLOR_MAGENTA);
}

/* A surface whose pitch exceeds its width is a window onto a wider buffer. Clear must touch
 * only the visible columns of each row, never the pixels between rows. */
static void test_draw_clear_respects_pitch(void) {
    uint32_t buf[8 * 16];
    for (int i = 0; i < 8 * 16; i++) buf[i] = OOPS_COLOR_BLACK;
    oops_surface_t surf = { buf, 8, 8, 16 };  /* 8 wide, 8 tall, rows 16 apart */

    oops_draw_clear(&surf, OOPS_COLOR_RED);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 16; x++) {
            ASSERT_EQ(buf[y * 16 + x], (x < 8) ? OOPS_COLOR_RED : OOPS_COLOR_BLACK);
        }
    }
}

/* A sprite wrapped from a const array blits without copying; blit_blend honours the source's
 * per-pixel alpha so a transparent pixel shows the destination through. */
static void test_draw_sprite_and_blit_blend(void) {
    static const uint32_t sprite_px[4] = {
        OOPS_COLOR_RED, OOPS_RGBA(0, 0, 0, 0),          /* opaque red, fully transparent */
        OOPS_RGBA(255, 255, 255, 128), OOPS_COLOR_BLUE  /* 50% white, opaque blue */
    };
    oops_sprite_t sprite = { 2, 2, sprite_px };
    oops_surface_t src = oops_surface_from_sprite(&sprite);
    ASSERT_TRUE(src.pixels != NULL);
    ASSERT_EQ(src.width, 2);
    ASSERT_EQ(src.pitch, 2);

    uint32_t buf[4 * 4];
    oops_surface_t dst = { buf, 4, 4, 4 };
    oops_draw_clear(&dst, OOPS_COLOR_BLACK);

    oops_draw_blit_blend(&dst, 1, 1, &src, 0, 0, 2, 2);
    ASSERT_EQ(buf[1 * 4 + 1], OOPS_COLOR_RED);          /* opaque source wins */
    ASSERT_EQ(buf[1 * 4 + 2], OOPS_COLOR_BLACK);        /* transparent: destination shows */
    ASSERT_EQ(buf[2 * 4 + 2], OOPS_COLOR_BLUE);         /* opaque blue */
    uint32_t blended = buf[2 * 4 + 1];                  /* 50% white over black -> ~128 grey */
    ASSERT_TRUE(((blended >> 16) & 0xFF) >= 127 && ((blended >> 16) & 0xFF) <= 129);

    /* A straight blit ignores alpha and stamps the raw pixel. */
    oops_draw_clear(&dst, OOPS_COLOR_BLACK);
    oops_draw_blit(&dst, 0, 0, &src, 0, 0, 2, 2);
    ASSERT_EQ(buf[0 * 4 + 1], OOPS_RGBA(0, 0, 0, 0));   /* copied verbatim, not composited */
}

static void test_draw_gradient(void) {
    uint32_t buf[1 * 16];
    oops_surface_t s = { buf, 16, 1, 16 };
    oops_draw_clear(&s, OOPS_COLOR_BLACK);
    /* Horizontal black->white across 16 px: left end black, right end white, monotonic. */
    oops_draw_rect_gradient(&s, 0, 0, 16, 1, OOPS_RGB(0, 0, 0), OOPS_RGB(255, 255, 255), 0);
    ASSERT_EQ(buf[0] & 0xFF, 0);
    ASSERT_EQ(buf[15] & 0xFF, 255);
    ASSERT_TRUE((buf[8] & 0xFF) > (buf[4] & 0xFF));
}

static void test_draw_text_width_and_lowercase(void) {
    /* Longest line drives the width; \n resets. "AB\nCDE" -> 3 glyphs * 8 = 24 at scale 1. */
    ASSERT_EQ(oops_draw_text_width("AB\nCDE", 1), 24);
    ASSERT_EQ(oops_draw_text_width("hi", 2), 2 * 8 * 2);
    ASSERT_EQ(oops_draw_text_width(NULL, 1), 0);

    /* Lower case is no longer folded to upper: 'a' and 'A' render differently, and 'a' is not
     * the blank glyph. */
    uint32_t a[8 * 8], A[8 * 8];
    oops_surface_t sa = { a, 8, 8, 8 }, sA = { A, 8, 8, 8 };
    oops_draw_clear(&sa, OOPS_COLOR_BLACK);
    oops_draw_clear(&sA, OOPS_COLOR_BLACK);
    oops_draw_text(&sa, 0, 0, "a", OOPS_COLOR_WHITE, 1);
    oops_draw_text(&sA, 0, 0, "A", OOPS_COLOR_WHITE, 1);
    int lit_a = 0, differ = 0;
    for (int i = 0; i < 8 * 8; i++) {
        if (a[i] == OOPS_COLOR_WHITE) lit_a = 1;
        if (a[i] != A[i]) differ = 1;
    }
    ASSERT_TRUE(lit_a);
    ASSERT_TRUE(differ);

    /* And 'a' must be its own glyph, not the '?' substitution an out-of-range char would give. */
    uint32_t q[8 * 8];
    oops_surface_t sq = { q, 8, 8, 8 };
    oops_draw_clear(&sq, OOPS_COLOR_BLACK);
    oops_draw_text(&sq, 0, 0, "?", OOPS_COLOR_WHITE, 1);
    int not_qmark = 0;
    for (int i = 0; i < 8 * 8; i++) if (a[i] != q[i]) not_qmark = 1;
    ASSERT_TRUE(not_qmark);

}

/* Blended pixel/line/circle composite source-over; a transparent colour leaves the destination,
 * a 50% colour blends toward it, and the filled circle never double-composites its centre. */
static void test_draw_blend_primitives(void) {
    uint32_t buf[16 * 16];
    oops_surface_t s = { buf, 16, 16, 16 };
    oops_draw_clear(&s, OOPS_COLOR_BLACK);

    oops_draw_pixel_blend(&s, 1, 1, OOPS_RGBA(255, 255, 255, 0));   /* transparent: no change */
    ASSERT_EQ(buf[1 * 16 + 1], OOPS_COLOR_BLACK);
    oops_draw_pixel_blend(&s, 2, 2, OOPS_RGBA(255, 255, 255, 128)); /* 50% white over black */
    ASSERT_TRUE(((buf[2 * 16 + 2] >> 16) & 0xFF) >= 127 && ((buf[2 * 16 + 2] >> 16) & 0xFF) <= 129);

    /* A 50% line over black: every touched pixel is ~grey, none left black on the path ends. */
    oops_draw_line_blend(&s, 0, 0, 15, 15, OOPS_RGBA(255, 255, 255, 128));
    ASSERT_TRUE(((buf[0] >> 16) & 0xFF) >= 127 && ((buf[0] >> 16) & 0xFF) <= 129);
    ASSERT_TRUE(((buf[15 * 16 + 15] >> 16) & 0xFF) >= 127);

    /* Filled 50% circle: centre composites exactly once (a double-blend would over-darken it
     * below ~128); one clean blend leaves ~128. */
    oops_draw_clear(&s, OOPS_COLOR_BLACK);
    oops_draw_circle_blend(&s, 8, 8, 4, OOPS_RGBA(255, 255, 255, 128), 1);
    uint32_t centre = buf[8 * 16 + 8];
    ASSERT_TRUE(((centre >> 16) & 0xFF) >= 127 && ((centre >> 16) & 0xFF) <= 129);
    ASSERT_EQ(buf[0], OOPS_COLOR_BLACK);   /* corner outside the circle untouched */
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
}

