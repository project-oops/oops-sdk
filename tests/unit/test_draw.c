#include "tests/test_common.h"
#include "oops/draw.h"

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

void run_unit_tests_draw(void) {
    TEST_SUITE_BEGIN("2D Graphics Canvas & Primitives");
    RUN_TEST(test_draw_surface_clear_and_pixel);
    RUN_TEST(test_draw_rect_clipping);
    RUN_TEST(test_draw_alpha_blend);
    RUN_TEST(test_draw_line_and_circle);
    RUN_TEST(test_draw_text_and_blit);
}

