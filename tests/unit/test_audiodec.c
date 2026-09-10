#include "oops/audiodec.h"
#include "tests/test_common.h"

/* On a host with no platform symbols, audio decode and its offload engine are
 * both unavailable; the subsystem reports each honestly, rejects bad arguments,
 * and never returns a decoder while the control-struct layout is unconfirmed.
 */
static void test_audiodec_unavailable_on_host(void) {
  ASSERT_EQ(oops_audiodec_available(), 0);
  ASSERT_EQ(oops_audiodec_offload_available(), 0);
}

static void test_audiodec_open_contract(void) {
  /* Bad codec is rejected before availability. */
  ASSERT_TRUE(oops_audiodec_open(999) == NULL);
  ASSERT_EQ(oops_audiodec_last_error(), OOPS_AUDIODEC_EPARAM);

  /* Valid codec on a host without the library: unavailable, not a fabricated
   * handle. */
  ASSERT_TRUE(oops_audiodec_open(OOPS_AUDIODEC_AAC) == NULL);
  ASSERT_EQ(oops_audiodec_last_error(), OOPS_AUDIODEC_EUNAVAIL);
}

static void test_audiodec_decode_and_close_contract(void) {
  int16_t pcm[64];
  /* Null / zero arguments rejected. */
  ASSERT_EQ(oops_audiodec_decode(NULL, NULL, 0, pcm, 64), OOPS_AUDIODEC_EPARAM);
  /* Close tolerates NULL. */
  oops_audiodec_close(NULL);
}

void run_unit_tests_audiodec(void) {
  TEST_SUITE_BEGIN("Hardware Audio Decode (libSceAudiodec / AJM)");
  RUN_TEST(test_audiodec_unavailable_on_host);
  RUN_TEST(test_audiodec_open_contract);
  RUN_TEST(test_audiodec_decode_and_close_contract);
}
