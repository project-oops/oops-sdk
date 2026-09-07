#include "tests/test_common.h"
#include "agc/tiler.h"
#include <stdlib.h>

static void test_tiler_surface_bytes(void) {
    ASSERT_EQ(agc_tile_surface_bytes(0, 0), 0);
    ASSERT_EQ(agc_tile_surface_bytes(128, 0), 0);
    ASSERT_EQ(agc_tile_surface_bytes(1, 1), 65536);
    ASSERT_EQ(agc_tile_surface_bytes(128, 128), 65536);
    ASSERT_EQ(agc_tile_surface_bytes(129, 128), 2 * 65536);
    ASSERT_EQ(agc_tile_surface_bytes(1920, 1080), 15 * 9 * 65536);
    ASSERT_EQ(agc_tile_surface_bytes(3840, 2160), 30 * 17 * 65536);
}

/* The basis is hardware-derived, and a permutation check alone cannot tell it from any other
 * permutation. Pin the address of one pixel per basis bit, plus the whole 8x8 micro-tile, so
 * an edit to the basis fails here before it reaches a display. */
static void test_tiler_swizzle_golden(void) {
    enum { W = 128, H = 128 };
    static const struct { uint32_t x, y, offset; } pins[] = {
        {   0,   0,     0 },
        {   1,   0,     1 }, {   2,   0,     2 }, {   4,   0,    32 }, {   8,   0,    64 },
        {  16,   0,  2176 }, {  32,   0,   512 }, {  64,   0,  8448 },
        {   0,   1,     4 }, {   0,   2,     8 }, {   0,   4,    16 }, {   0,   8,  1088 },
        {   0,  16,   128 }, {   0,  32,   256 }, {   0,  64,  4608 },
        {   8,   8,  1024 },   /* x3 and y3 share address bit 8, which cancels; bit 12 stays */
        { 127, 127, 15423 },
    };
    uint32_t *src = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    uint32_t *dst = (uint32_t *)calloc((size_t)W * H, sizeof(uint32_t));
    ASSERT_TRUE(src != NULL && dst != NULL);
    for (uint32_t i = 0; i < (uint32_t)(W * H); i++) src[i] = i;

    agc_tile_init();
    agc_tile_init();   /* idempotent */
    agc_tile_surface(dst, src, W, H);

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        ASSERT_EQ(dst[pins[i].offset], pins[i].y * W + pins[i].x);
    }
    /* 8x8 micro-tile: pixel-offset bits are x0 x1 y0 y1 y2 x2 */
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            uint32_t off = (x & 3u) | ((y & 7u) << 2) | ((x >> 2) << 5);
            ASSERT_EQ(dst[off], y * W + x);
        }
    }
    free(dst);
    free(src);
}

/* Tiles are laid out row-major across the rounded-up surface, 64 KB each. */
static void check_placement(uint32_t w, uint32_t h, uint32_t x, uint32_t y, size_t expect_word) {
    size_t n = (size_t)w * h;
    size_t words = agc_tile_surface_bytes(w, h) / sizeof(uint32_t);
    uint32_t *src = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint32_t *dst = (uint32_t *)calloc(words, sizeof(uint32_t));
    ASSERT_TRUE(src != NULL && dst != NULL);
    for (size_t i = 0; i < n; i++) src[i] = (uint32_t)i + 1u;

    agc_tile_surface(dst, src, w, h);

    ASSERT_EQ(dst[expect_word], (size_t)y * w + x + 1u);
    free(dst);
    free(src);
}

static void test_tiler_tile_placement(void) {
    check_placement(256, 128, 128, 0, 16384);        /* second tile of the first tile row */
    check_placement(128, 256, 0, 128, 16384);        /* first tile of the second tile row */
    check_placement(256, 256, 128, 128, 3 * 16384);  /* tile row 1, column 1 */
    /* Bottom-right pixel at 1080p: tile (14, 8) of a 15-wide grid is tile 134; inside it,
     * (127, 55) is lut_x[127] ^ lut_y[55] = 0x2BE3 ^ 0x019C. */
    check_placement(1920, 1080, 1919, 1079, 134u * 16384u + 0x2A7Fu);
}

/* Every visible source pixel lands exactly once inside the tiled extent, nothing is written
 * past it, and the margin of a partial tile is left alone. The words after the tiled extent
 * are a canary. */
static void check_bijection(uint32_t w, uint32_t h) {
    const size_t guard = 4096;
    size_t tiled_words = agc_tile_surface_bytes(w, h) / sizeof(uint32_t);
    size_t n = (size_t)w * h;
    ASSERT_TRUE(tiled_words >= n);

    uint32_t *src = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint32_t *dst = (uint32_t *)malloc((tiled_words + guard) * sizeof(uint32_t));
    uint8_t *seen = (uint8_t *)calloc(n + 1, sizeof(uint8_t));
    ASSERT_TRUE(src != NULL && dst != NULL && seen != NULL);

    for (size_t i = 0; i < n; i++) src[i] = (uint32_t)i + 1u;
    for (size_t i = 0; i < tiled_words + guard; i++) dst[i] = 0;

    agc_tile_surface(dst, src, w, h);

    size_t written = 0;
    for (size_t i = 0; i < tiled_words; i++) {
        uint32_t val = dst[i];
        if (val == 0) continue;  /* margin of a partial tile */
        ASSERT_TRUE(val <= n);
        ASSERT_EQ(seen[val], 0);
        seen[val] = 1;
        written++;
    }
    ASSERT_EQ(written, n);
    for (size_t i = tiled_words; i < tiled_words + guard; i++) {
        ASSERT_EQ(dst[i], 0);
    }

    free(seen);
    free(dst);
    free(src);
}

static void test_tiler_bijection_128x128(void) {
    check_bijection(128, 128);
}

static void test_tiler_bijection_1920x1080(void) {
    check_bijection(1920, 1080);  /* 15 x 9 tiles, last tile row 56 pixels tall */
}

static void test_tiler_bijection_partial_tiles(void) {
    check_bijection(200, 100);    /* partial in both axes */
    check_bijection(129, 3);      /* one pixel into a second tile column */
    check_bijection(1, 1);
}

static void test_tiler_null_safety(void) {
    uint32_t buf[16];
    agc_tile_surface(NULL, buf, 4, 4);
    agc_tile_surface(buf, NULL, 4, 4);
    agc_tile_surface(buf, buf, 0, 0);
}

void run_unit_tests_agc_tiler(void) {
    TEST_SUITE_BEGIN("AGC Tiler (RDNA2 GFX10.3)");
    RUN_TEST(test_tiler_surface_bytes);
    RUN_TEST(test_tiler_swizzle_golden);
    RUN_TEST(test_tiler_tile_placement);
    RUN_TEST(test_tiler_bijection_128x128);
    RUN_TEST(test_tiler_bijection_1920x1080);
    RUN_TEST(test_tiler_bijection_partial_tiles);
    RUN_TEST(test_tiler_null_safety);
}
