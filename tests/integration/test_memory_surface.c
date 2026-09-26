#define _POSIX_C_SOURCE 200112L
#include "oops/draw.h"
#include "tests/test_common.h"
#include <stdlib.h>

/* Integration test: drawing and blitting into a surface bound over a 64 KiB aligned
 * block. The block comes from posix_memalign because oops_mem_alloc needs the
 * platform's direct memory, which a host build does not have (test_memory.c). */

/* Rectangles and blits land at the right pixels of an externally allocated surface. */
static void test_memory_surface_integration(void) {
    size_t size = 64 * 1024;
    void *mem = NULL;
    int rc = posix_memalign(&mem, 0x10000, size);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(mem != NULL);

    /* 128 x 128 at 32 bits fills the 64 KiB exactly. */
    oops_surface_t surf = {(uint32_t *)mem, 128, 128, 128, OOPS_SURFACE_LINEAR};

    oops_draw_clear(&surf, OOPS_COLOR_BLACK);
    oops_draw_rect(&surf, 10, 10, 50, 50, OOPS_COLOR_CYAN);
    ASSERT_EQ(surf.pixels[10 * 128 + 10], OOPS_COLOR_CYAN);

    uint32_t icon_buf[16 * 16];
    oops_surface_t icon = {icon_buf, 16, 16, 16, OOPS_SURFACE_LINEAR};
    oops_draw_clear(&icon, OOPS_COLOR_RED);

    oops_draw_blit(&surf, 20, 20, &icon, 0, 0, 16, 16);
    ASSERT_EQ(surf.pixels[20 * 128 + 20], OOPS_COLOR_RED);

    free(mem);
}

void run_integration_tests_memory(void) {
    TEST_SUITE_BEGIN("Integration: Direct Memory -> Surface -> Canvas Operations");
    RUN_TEST(test_memory_surface_integration);
}
