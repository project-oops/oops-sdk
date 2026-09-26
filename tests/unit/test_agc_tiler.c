#include "agc/tiler.h"
#include "tests/test_common.h"
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

/* The basis is hardware-derived, and a permutation check alone cannot tell it
 * from any other permutation. Pin the address of one pixel per basis bit, plus
 * the whole 8x8 micro-tile, so an edit to the basis fails here before it
 * reaches a display. */
static void test_tiler_swizzle_golden(void) {
    enum { W = 128, H = 128 };
    static const struct {
        uint32_t x, y, offset;
    } pins[] = {
        {0, 0, 0},         {1, 0, 1},    {2, 0, 2},     {4, 0, 32},   {8, 0, 64},
        {16, 0, 2176},     {32, 0, 512}, {64, 0, 8448}, {0, 1, 4},    {0, 2, 8},
        {0, 4, 16},        {0, 8, 1088}, {0, 16, 128},  {0, 32, 256}, {0, 64, 4608},
        {8, 8, 1024}, /* x3 and y3 share address bit 8, which cancels; bit 12
                         stays */
        {127, 127, 15423},
    };
    uint32_t *src = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    uint32_t *dst = (uint32_t *)calloc((size_t)W * H, sizeof(uint32_t));
    ASSERT_TRUE(src != NULL && dst != NULL);
    for (uint32_t i = 0; i < (uint32_t)(W * H); i++)
        src[i] = i;

    agc_tile_init();
    agc_tile_init(); /* idempotent */
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
static void check_placement(uint32_t w, uint32_t h, uint32_t x, uint32_t y,
                            size_t expect_word) {
    size_t n = (size_t)w * h;
    size_t words = agc_tile_surface_bytes(w, h) / sizeof(uint32_t);
    uint32_t *src = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint32_t *dst = (uint32_t *)calloc(words, sizeof(uint32_t));
    ASSERT_TRUE(src != NULL && dst != NULL);
    for (size_t i = 0; i < n; i++)
        src[i] = (uint32_t)i + 1u;

    agc_tile_surface(dst, src, w, h);

    ASSERT_EQ(dst[expect_word], (size_t)y * w + x + 1u);
    free(dst);
    free(src);
}

static void test_tiler_tile_placement(void) {
    check_placement(256, 128, 128, 0, 16384); /* second tile of the first tile row */
    check_placement(128, 256, 0, 128, 16384); /* first tile of the second tile row */
    check_placement(256, 256, 128, 128, 3 * 16384); /* tile row 1, column 1 */
    /* Bottom-right pixel at 1080p: tile (14, 8) of a 15-wide grid is tile 134;
     * inside it, (127, 55) is lut_x[127] ^ lut_y[55] = 0x2BE3 ^ 0x019C. */
    check_placement(1920, 1080, 1919, 1079, 134u * 16384u + 0x2A7Fu);
}

/* Every visible source pixel lands exactly once inside the tiled extent,
 * nothing is written past it, and the margin of a partial tile is left alone.
 * The words after the tiled extent are a canary. */
static void check_bijection(uint32_t w, uint32_t h) {
    const size_t guard = 4096;
    size_t tiled_words = agc_tile_surface_bytes(w, h) / sizeof(uint32_t);
    size_t n = (size_t)w * h;
    ASSERT_TRUE(tiled_words >= n);

    uint32_t *src = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint32_t *dst = (uint32_t *)malloc((tiled_words + guard) * sizeof(uint32_t));
    uint8_t *seen = (uint8_t *)calloc(n + 1, sizeof(uint8_t));
    ASSERT_TRUE(src != NULL && dst != NULL && seen != NULL);

    for (size_t i = 0; i < n; i++)
        src[i] = (uint32_t)i + 1u;
    for (size_t i = 0; i < tiled_words + guard; i++)
        dst[i] = 0;

    agc_tile_surface(dst, src, w, h);

    size_t written = 0;
    for (size_t i = 0; i < tiled_words; i++) {
        uint32_t val = dst[i];
        if (val == 0)
            continue; /* margin of a partial tile */
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
    check_bijection(1920, 1080); /* 15 x 9 tiles, last tile row 56 pixels tall */
}

static void test_tiler_bijection_partial_tiles(void) {
    check_bijection(200, 100); /* partial in both axes */
    check_bijection(129, 3);   /* one pixel into a second tile column */
    check_bijection(1, 1);
}

static void test_tiler_null_safety(void) {
    uint32_t buf[16];
    agc_tile_surface(NULL, buf, 4, 4);
    agc_tile_surface(buf, NULL, 4, 4);
    agc_tile_surface(buf, buf, 0, 0);
}

#include "agc/display.h"
#include "oops/agc.h"
#include "oops/display.h"

static void test_agc_dcb_desc_contract(void) {
    ASSERT_EQ(sizeof(oops_agc_dcb_desc), 16);
    ASSERT_EQ(offsetof(oops_agc_dcb_desc, gpu_addr), 0);
    ASSERT_EQ(offsetof(oops_agc_dcb_desc, size), 8);
    ASSERT_EQ(offsetof(oops_agc_dcb_desc, flags), 12);
    ASSERT_EQ(offsetof(oops_agc_dcb_desc, pad), 14);
}

static void test_agc_display_acceleration_query(void) {
    ASSERT_EQ(agc_display_is_gpu_accelerated(NULL), 0);
    ASSERT_EQ(oops_display_is_gpu_accelerated(NULL), 0);
}

/* Every field of the resource constant, pinned to the bit it is packed into.
 * Half of these positions are corroborated by this collection's own descriptor
 * decoder and half come from the published reference (tiler.h says which is
 * which), so the point of pinning them is that a later correction to either
 * fails loudly here instead of silently addressing the wrong memory. */
static void test_buffer_descriptor_layout(void) {
    uint32_t d[4];

    /* Base address splits across words 0 and 1, low 48 bits only. */
    ASSERT_EQ(agc_buffer_descriptor(d, 0x0000123456789ABCull, 0, 0, 0), 0);
    ASSERT_EQ(d[0], 0x56789ABCu);
    ASSERT_EQ(d[1] & 0xFFFFu, 0x1234u);

    /* Stride occupies 61:48, i.e. word 1 bits 29:16, and tops out at 16383. */
    ASSERT_EQ(agc_buffer_descriptor(d, 0, 0x3FFFu, 0, 0), 0);
    ASSERT_EQ(d[1], 0x3FFFu << 16);
    ASSERT_EQ(agc_buffer_descriptor(d, 0, 4u, 0, 0), 0);
    ASSERT_EQ(d[1], 4u << 16);

    /* Swizzle enable (bit 63) and add-thread-id (bit 119) stay clear: both move
     * where an access lands and neither is wanted. */
    ASSERT_EQ(d[1] >> 31, 0u);
    ASSERT_EQ((d[3] >> 23) & 1u, 0u);

    /* Record count is the whole of word 2. */
    ASSERT_EQ(agc_buffer_descriptor(d, 0, 0, 0xDEADBEEFu, 0), 0);
    ASSERT_EQ(d[2], 0xDEADBEEFu);

    /* Format sits at word 3 bits 18:12, channel selects below it, out-of-bounds
     * select at 29:28, and the resource type (31:30) is 0 for a buffer. */
    ASSERT_EQ(agc_buffer_descriptor(d, 0, 0, 0, AGC_BUF_FMT_32_UINT), 0);
    ASSERT_EQ((d[3] >> 12) & 0x7Fu, 20u);
    ASSERT_EQ(d[3] & 0xFFFu, AGC_BUF_DST_SEL_IDENTITY);
    ASSERT_EQ((d[3] >> 28) & 3u, AGC_BUF_OOB_STRUCTURED);
    ASSERT_EQ(d[3] >> 30, 0u);

    ASSERT_EQ(agc_buffer_descriptor(d, 0, 0, 0, AGC_BUF_FMT_32_32_UINT), 0);
    ASSERT_EQ((d[3] >> 12) & 0x7Fu, 62u);

    /* Channel selects are the identity mapping x=R y=G z=B w=A. */
    ASSERT_EQ(AGC_BUF_DST_SEL_IDENTITY & 7u, 4u);
    ASSERT_EQ((AGC_BUF_DST_SEL_IDENTITY >> 3) & 7u, 5u);
    ASSERT_EQ((AGC_BUF_DST_SEL_IDENTITY >> 6) & 7u, 6u);
    ASSERT_EQ((AGC_BUF_DST_SEL_IDENTITY >> 9) & 7u, 7u);
}

/* A descriptor that is wrong in a way the builder could have caught is a wrong
 * address, not an error, so each of these refuses rather than truncating. */
static void test_buffer_descriptor_refusals(void) {
    uint32_t d[4];
    ASSERT_EQ(agc_buffer_descriptor(NULL, 0, 0, 0, 0), -1);
    ASSERT_EQ(agc_buffer_descriptor(d, 0, 0x4000u, 0, 0), -1);    /* stride > 14 bits */
    ASSERT_EQ(agc_buffer_descriptor(d, 0, 0, 0, 0x80u), -1);      /* format > 7 bits */
    ASSERT_EQ(agc_buffer_descriptor(d, 1ull << 48, 0, 0, 0), -1); /* base > 48 bits */
    ASSERT_EQ(agc_buffer_descriptor(d, 0xFFFFFFFFFFFFull, 0x3FFFu, 0, 0x7Fu), 0);
}

/* The dispatch the decoded shader interface implies: 8x8 threads, eight pixels
 * each, so one workgroup covers 64x8 pixels. */
static void test_tiler_dispatch_params(void) {
    uint32_t ud[AGC_TILER_USER_DATA_COUNT];
    uint32_t gx = 0, gy = 0;

    ASSERT_EQ(agc_tiler_dispatch_params(ud, &gx, &gy, 0x4000000000ull, 0x4000A00000ull,
                                        1920, 1080),
              0);
    ASSERT_EQ(gx, 30u);  /* 1920 / 64 */
    ASSERT_EQ(gy, 135u); /* 1080 / 8 */

    /* User data 0 is the width, under the assumption tiler.h records. */
    ASSERT_EQ(ud[0], 1920u);

    /* Source descriptor in slots 1..4: linear, one pixel per record. */
    ASSERT_EQ(ud[AGC_TILER_SRC_DESC_SLOT + 0], 0x00000000u);
    ASSERT_EQ(ud[AGC_TILER_SRC_DESC_SLOT + 1] & 0xFFFFu, 0x40u);
    ASSERT_EQ((ud[AGC_TILER_SRC_DESC_SLOT + 1] >> 16) & 0x3FFFu, 4u);
    ASSERT_EQ(ud[AGC_TILER_SRC_DESC_SLOT + 2], 1920u * 1080u);
    ASSERT_EQ((ud[AGC_TILER_SRC_DESC_SLOT + 3] >> 12) & 0x7Fu, AGC_BUF_FMT_32_UINT);

    /* Destination descriptor in slots 5..8: tiled, addressed as dword pairs, so
     * its record count is the tiled byte count over eight. */
    ASSERT_EQ(ud[AGC_TILER_DST_DESC_SLOT + 0], 0x00A00000u);
    ASSERT_EQ((ud[AGC_TILER_DST_DESC_SLOT + 1] >> 16) & 0x3FFFu, 8u);
    ASSERT_EQ(ud[AGC_TILER_DST_DESC_SLOT + 2],
              (uint32_t)(agc_tile_surface_bytes(1920, 1080) / 8u));
    ASSERT_EQ((ud[AGC_TILER_DST_DESC_SLOT + 3] >> 12) & 0x7Fu, AGC_BUF_FMT_32_32_UINT);

    /* 720p divides too - it is what home asks for before the backend promotes. */
    ASSERT_EQ(agc_tiler_dispatch_params(ud, &gx, &gy, 0, 0x10000, 1280, 720), 0);
    ASSERT_EQ(gx, 20u);
    ASSERT_EQ(gy, 90u);
}

/* A partial workgroup would tile part of a surface and leave the rest, which
 * looks like a working display with a corrupt edge. Refuse instead. */
static void test_tiler_dispatch_refusals(void) {
    uint32_t ud[AGC_TILER_USER_DATA_COUNT];
    uint32_t gx = 0, gy = 0;
    ASSERT_EQ(agc_tiler_dispatch_params(NULL, &gx, &gy, 0, 0, 1920, 1080), -1);
    ASSERT_EQ(agc_tiler_dispatch_params(ud, NULL, &gy, 0, 0, 1920, 1080), -1);
    ASSERT_EQ(agc_tiler_dispatch_params(ud, &gx, NULL, 0, 0, 1920, 1080), -1);
    ASSERT_EQ(agc_tiler_dispatch_params(ud, &gx, &gy, 0, 0, 0, 1080), -1);
    ASSERT_EQ(agc_tiler_dispatch_params(ud, &gx, &gy, 0, 0, 1920, 0), -1);
    ASSERT_EQ(agc_tiler_dispatch_params(ud, &gx, &gy, 0, 0, 1900, 1080), -1);
    ASSERT_EQ(agc_tiler_dispatch_params(ud, &gx, &gy, 0, 0, 1920, 1081), -1);
}

static void test_agc_detile_pixel_golden(void) {
    uint32_t x = 0, y = 0;
    /* Hardware oracle anchor point: (15, 15) -> 0x43f (byte 4348) */
    agc_detile_pixel(0x43fu, &x, &y);
    ASSERT_EQ(x, 15u);
    ASSERT_EQ(y, 15u);

    /* Independent emulator check: (32, 21) -> 0x294 (byte 2640) */
    agc_detile_pixel(0x294u, &x, &y);
    ASSERT_EQ(x, 32u);
    ASSERT_EQ(y, 21u);

    /* Origin */
    agc_detile_pixel(0u, &x, &y);
    ASSERT_EQ(x, 0u);
    ASSERT_EQ(y, 0u);
}

/* agc_tile_pixel is agc_detile_pixel run backwards, for every pixel of a block, and
 * agrees with the whole-surface tiler, whose placement the display has shown correctly.
 */
static void test_agc_tile_pixel_inverts_detile(void) {
    for (uint32_t i = 0; i < 16384u; i++) {
        uint32_t x = 0, y = 0;
        agc_detile_pixel(i, &x, &y);
        ASSERT_EQ(agc_tile_pixel(x, y), i);
    }
    ASSERT_EQ(agc_tile_pixel(15u, 15u), 0x43fu);
    ASSERT_EQ(agc_tile_pixel(32u, 21u), 0x294u);

    enum { W = 128, H = 128 };
    uint32_t *lin = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    uint32_t *tiled = (uint32_t *)calloc((size_t)W * H, sizeof(uint32_t));
    ASSERT_TRUE(lin != NULL && tiled != NULL);
    for (uint32_t i = 0; i < (uint32_t)(W * H); i++)
        lin[i] = 0xff000000u | (i * 2654435761u >> 8);
    agc_tile_surface(tiled, lin, W, H);
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            ASSERT_EQ(tiled[agc_tile_pixel(x, y)], lin[y * W + x]);
        }
    }
    free(tiled);
    free(lin);
}

static void test_agc_detile_surface_roundtrip(void) {
    enum { W = 256, H = 256 };
    size_t n = (size_t)W * H;
    size_t tiled_words = agc_tile_surface_bytes(W, H) / sizeof(uint32_t);
    uint32_t *orig = (uint32_t *)malloc(n * sizeof(uint32_t));
    uint32_t *tiled = (uint32_t *)calloc(tiled_words, sizeof(uint32_t));
    uint32_t *detiled = (uint32_t *)calloc(n, sizeof(uint32_t));
    ASSERT_TRUE(orig != NULL && tiled != NULL && detiled != NULL);

    for (size_t i = 0; i < n; i++) {
        orig[i] = 0xff000000u | (uint32_t)i;
    }

    agc_tile_surface(tiled, orig, W, H);
    agc_detile_surface(detiled, tiled, W, H);

    for (size_t i = 0; i < n; i++) {
        ASSERT_EQ(detiled[i], orig[i]);
    }

    free(detiled);
    free(tiled);
    free(orig);
}

void run_unit_tests_agc_tiler(void) {
    TEST_SUITE_BEGIN("AGC Tiler & GPU Pipeline (RDNA2 GFX10.3)");
    RUN_TEST(test_tiler_surface_bytes);
    RUN_TEST(test_tiler_swizzle_golden);
    RUN_TEST(test_tiler_tile_placement);
    RUN_TEST(test_tiler_bijection_128x128);
    RUN_TEST(test_tiler_bijection_1920x1080);
    RUN_TEST(test_tiler_bijection_partial_tiles);
    RUN_TEST(test_tiler_null_safety);
    RUN_TEST(test_agc_detile_pixel_golden);
    RUN_TEST(test_agc_tile_pixel_inverts_detile);
    RUN_TEST(test_agc_detile_surface_roundtrip);
    RUN_TEST(test_agc_dcb_desc_contract);
    RUN_TEST(test_agc_display_acceleration_query);
    RUN_TEST(test_buffer_descriptor_layout);
    RUN_TEST(test_buffer_descriptor_refusals);
    RUN_TEST(test_tiler_dispatch_params);
    RUN_TEST(test_tiler_dispatch_refusals);
}
