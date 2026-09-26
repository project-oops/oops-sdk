#include "oops/escalate.h"
#include "tests/test_common.h"

static void test_escalate_constants(void) {
    ASSERT_EQ(OOPS_SYSTEM_AUTHID, 0x3000000000000001ULL);
}

static void test_escalate_unsupported_environment(void) {
    /* On host without kernel R/W primitives, functions gracefully fail */
    ASSERT_EQ(oops_kernel_rw_init(), -1);
    ASSERT_EQ(oops_kernel_pipe_init(NULL, NULL, 0, 0), -1);
    ASSERT_EQ(oops_kernel_get_current_proc(), 0);
    ASSERT_EQ(oops_kernel_find_proc_by_pid(1), 0);
    ASSERT_EQ(oops_kernel_copyout(0x1000, NULL, 0), -1);
    ASSERT_EQ(oops_kernel_copyin(NULL, 0x1000, 0), -1);
    ASSERT_EQ(oops_escalate_to_system_authid(), -1);
    ASSERT_EQ(oops_jailbreak_process(-1), -1);
    ASSERT_EQ(oops_escape_jail(), -1);
    ASSERT_EQ(oops_has_system_authid(), -1);
}

void run_unit_tests_escalate(void) {
    TEST_SUITE_BEGIN("Privilege Escalation & Kernel R/W");
    RUN_TEST(test_escalate_constants);
    RUN_TEST(test_escalate_unsupported_environment);
}
