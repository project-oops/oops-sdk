#include "oops/videodec.h"
#include "tests/test_common.h"

/* On a host with no platform symbols, video decode is unavailable; the
 * subsystem must say so honestly, reject bad arguments before any platform
 * call, and never hand back a decoder while the decode struct layouts are
 * unconfirmed. */
static void test_videodec_unavailable_on_host(void) {
    ASSERT_EQ(oops_videodec_available(), 0);
}

static void test_videodec_open_contract(void) {
    /* Bad codec is rejected before availability is even consulted. */
    ASSERT_TRUE(oops_videodec_open(999, 1920, 1080) == NULL);
    ASSERT_EQ(oops_videodec_last_error(), OOPS_VIDEODEC_EPARAM);

    /* Zero dimensions rejected. */
    ASSERT_TRUE(oops_videodec_open(OOPS_VIDEODEC_H264, 0, 0) == NULL);
    ASSERT_EQ(oops_videodec_last_error(), OOPS_VIDEODEC_EPARAM);

    /* Valid request on a host without the library: unavailable, not a fabricated
     * handle. */
    ASSERT_TRUE(oops_videodec_open(OOPS_VIDEODEC_H264, 1920, 1080) == NULL);
    ASSERT_EQ(oops_videodec_last_error(), OOPS_VIDEODEC_EUNAVAIL);
}

static void test_videodec_decode_and_close_contract(void) {
    oops_videodec_frame_t frame;
    /* Null arguments are rejected. */
    ASSERT_EQ(oops_videodec_decode(NULL, NULL, 0, &frame), OOPS_VIDEODEC_EPARAM);
    /* Close tolerates NULL. */
    oops_videodec_close(NULL);
}

void run_unit_tests_videodec(void) {
    TEST_SUITE_BEGIN("Hardware Video Decode (libSceVideodec2)");
    RUN_TEST(test_videodec_unavailable_on_host);
    RUN_TEST(test_videodec_open_contract);
    RUN_TEST(test_videodec_decode_and_close_contract);
}
