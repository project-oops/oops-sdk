#include "agc/tiler.h"
#include "oops/draw.h"
#include "tests/test_common.h"
#include <stdlib.h>

static void test_pipeline_draw_and_tile(void) {
  uint32_t width = 128;
  uint32_t height = 128;

  uint32_t *linear_fb = (uint32_t *)malloc(width * height * sizeof(uint32_t));
  uint32_t *tiled_fb = (uint32_t *)calloc(width * height, sizeof(uint32_t));
  ASSERT_TRUE(linear_fb != NULL && tiled_fb != NULL);

  oops_surface_t surf = {linear_fb, width, height, width};

  /* 1. Clear with dark blue */
  oops_draw_clear(&surf, OOPS_RGB(10, 20, 40));

  /* 2. Draw border and shapes */
  oops_draw_rect(&surf, 0, 0, width, 4, OOPS_COLOR_RED);
  oops_draw_rect(&surf, 0, height - 4, width, 4, OOPS_COLOR_RED);
  oops_draw_circle(&surf, 64, 64, 20, OOPS_COLOR_GREEN, 1);
  oops_draw_line(&surf, 0, 0, width - 1, height - 1, OOPS_COLOR_YELLOW);

  /* 3. Render text */
  oops_draw_text(&surf, 10, 10, "AGC TILE", OOPS_COLOR_WHITE, 1);

  /* Verify linear buffer drew correctly */
  ASSERT_EQ(linear_fb[0], OOPS_COLOR_YELLOW); /* Diagonal intersects top-left */
  ASSERT_EQ(linear_fb[64 * width + 64],
            OOPS_COLOR_YELLOW); /* Diagonal intersects (64,64) */
  ASSERT_EQ(linear_fb[64 * width + 55],
            OOPS_COLOR_GREEN); /* Off-diagonal inside circle */

  /* 4. Swizzle linear surface to RDNA2 scanout layout */
  agc_tile_surface(tiled_fb, linear_fb, width, height);

  /* Ensure tiled buffer is populated and not all zero */
  int non_zero_count = 0;
  for (uint32_t i = 0; i < width * height; i++) {
    if (tiled_fb[i] != 0)
      non_zero_count++;
  }
  ASSERT_EQ(non_zero_count, width * height);

  free(tiled_fb);
  free(linear_fb);
}

void run_integration_tests_pipeline(void) {
  TEST_SUITE_BEGIN("Integration: Draw Canvas -> AGC Tiler Scanout Pipeline");
  RUN_TEST(test_pipeline_draw_and_tile);
}
