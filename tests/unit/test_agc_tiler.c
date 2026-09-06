#include "tests/test_common.h"
#include "agc/tiler.h"
#include <stdlib.h>

static void test_tiler_bijection_128x128(void) {
    uint32_t w = 128, h = 128;
    uint32_t *src = (uint32_t *)malloc(w * h * sizeof(uint32_t));
    uint32_t *dst = (uint32_t *)calloc(w * h, sizeof(uint32_t));
    ASSERT_TRUE(src != NULL && dst != NULL);

    for (uint32_t i = 0; i < w * h; i++) {
        src[i] = i + 1;
    }

    agc_tile_surface(dst, src, w, h);

    uint8_t *seen = (uint8_t *)calloc(w * h + 1, sizeof(uint8_t));
    ASSERT_TRUE(seen != NULL);

    for (uint32_t i = 0; i < w * h; i++) {
        uint32_t val = dst[i];
        ASSERT_TRUE(val >= 1 && val <= w * h);
        ASSERT_EQ(seen[val], 0);
        seen[val] = 1;
    }

    free(seen);
    free(dst);
    free(src);
}

static void test_tiler_null_safety(void) {
    uint32_t buf[16];
    agc_tile_surface(NULL, buf, 4, 4);
    agc_tile_surface(buf, NULL, 4, 4);
    agc_tile_surface(buf, buf, 0, 0);
}

void run_unit_tests_agc_tiler(void) {
    TEST_SUITE_BEGIN("AGC Tiler (RDNA2 GFX10.3)");
    RUN_TEST(test_tiler_bijection_128x128);
    RUN_TEST(test_tiler_null_safety);
}

