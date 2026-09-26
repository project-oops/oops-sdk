#include "oops/netctl.h"
#include "tests/test_common.h"

/* Unit tests for network control, `oops/netctl.h`. */

/* Getting info into NULL is refused. */
static void test_netctl_null_safety(void) {
    ASSERT_EQ(oops_net_ctl_get_info(NULL), -1);
}

/* Without the platform library, init and info fail. */
static void test_netctl_unsupported(void) {
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
