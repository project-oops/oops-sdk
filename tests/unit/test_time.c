#include "tests/test_common.h"
#include "oops/time.h"

static void test_time_monotonic(void) {
    uint64_t t0 = oops_time_get_ticks();
    for (volatile int i = 0; i < 50000; i++);
    uint64_t t1 = oops_time_get_ticks();
    ASSERT_TRUE(t1 > t0);

    uint64_t u0 = oops_time_get_us();
    for (volatile int i = 0; i < 50000; i++);
    uint64_t u1 = oops_time_get_us();
    ASSERT_TRUE(u1 >= u0);
}

static void test_time_frequency(void) {
    uint64_t freq = oops_time_get_frequency();
    ASSERT_TRUE(freq >= 1000000000ULL); /* At least 1 GHz */

    uint64_t cnt_freq = oops_time_get_counter_frequency();
    ASSERT_TRUE(cnt_freq >= 100000000ULL); /* At least 100 MHz */
}

void run_unit_tests_time(void) {
    TEST_SUITE_BEGIN("High-Resolution Timing");
    RUN_TEST(test_time_monotonic);
    RUN_TEST(test_time_frequency);
}

