#include "tests/test_common.h"
#include "oops/audio.h"

static void test_audio_null_and_bounds(void) {
    int16_t samples[512 * 2];
    /* Write to null port */
    ASSERT_EQ(oops_audio_write(NULL, samples, 512), -1);

    /* Close null port */
    oops_audio_close(NULL);

    /* Volume on null port */
    ASSERT_EQ(oops_audio_set_volume(NULL, 1.0f, 1.0f), -1);
}

void run_unit_tests_audio(void) {
    TEST_SUITE_BEGIN("Audio PCM Subsystem");
    RUN_TEST(test_audio_null_and_bounds);
}

