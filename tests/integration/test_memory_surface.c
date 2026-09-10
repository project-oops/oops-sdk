#define _POSIX_C_SOURCE 200112L
#include "oops/draw.h"
#include "oops/memory.h"
#include "tests/test_common.h"
#include <stdlib.h>

static void test_memory_surface_integration(void) {
  /* Allocate 64KB page-aligned block */
  size_t size = 64 * 1024;
  void *mem = NULL;
  int rc = posix_memalign(&mem, 0x10000, size);
  ASSERT_EQ(rc, 0);
  ASSERT_TRUE(mem != NULL);

  /* Bind surface (128x128 32bpp = 64KB) */
  oops_surface_t surf;
  surf.pixels = (uint32_t *)mem;
  surf.width = 128;
  surf.height = 128;
  surf.pitch = 128;

  oops_draw_clear(&surf, OOPS_COLOR_BLACK);
  oops_draw_rect(&surf, 10, 10, 50, 50, OOPS_COLOR_CYAN);
  ASSERT_EQ(surf.pixels[10 * 128 + 10], OOPS_COLOR_CYAN);

  /* Create offscreen surface and blit */
  uint32_t icon_buf[16 * 16];
  oops_surface_t icon = {icon_buf, 16, 16, 16};
  oops_draw_clear(&icon, OOPS_COLOR_RED);

  oops_draw_blit(&surf, 20, 20, &icon, 0, 0, 16, 16);
  ASSERT_EQ(surf.pixels[20 * 128 + 20], OOPS_COLOR_RED);

  free(mem);
}

void run_integration_tests_memory(void) {
  TEST_SUITE_BEGIN(
      "Integration: Direct Memory -> Surface -> Canvas Operations");
  RUN_TEST(test_memory_surface_integration);
}
