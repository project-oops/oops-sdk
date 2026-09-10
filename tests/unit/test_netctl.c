#include "oops/netctl.h"
#include "tests/test_common.h"

static void test_netctl_null_safety(void) {
  ASSERT_EQ(oops_net_ctl_get_info(NULL), -1);
}

static void test_netctl_unsupported(void) {
  /* Weak symbols not present on host -> returns -1 */
  ASSERT_EQ(oops_net_ctl_init(), -1);

  oops_net_info_t info;
  ASSERT_EQ(oops_net_ctl_get_info(&info), -1);

  oops_net_ctl_term();
}

void run_unit_tests_netctl(void) {
  TEST_SUITE_BEGIN("Network Control & Telemetry (libSceNetCtl)");
  RUN_TEST(test_netctl_null_safety);
  RUN_TEST(test_netctl_unsupported);
}
