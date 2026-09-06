#include "tests/test_common.h"
#include "oops/pkg.h"

static void test_pkg_null_safety(void) {
    bool exists = false;
    uint32_t prog = 0;
    oops_pkg_progress_info_t info;

    ASSERT_EQ(oops_pkg_install(NULL), -1);
    ASSERT_EQ(oops_pkg_install(""), -1);
    ASSERT_EQ(oops_app_exists(NULL, &exists), -1);
    ASSERT_EQ(oops_app_exists("CUSA00001", NULL), -1);
    ASSERT_EQ(oops_app_uninstall(NULL), -1);
    ASSERT_EQ(oops_app_uninstall(""), -1);
    ASSERT_EQ(oops_pkg_get_progress(NULL, &prog), -1);
    ASSERT_EQ(oops_pkg_get_progress("EP0001-CUSA00001_00-0000000000000000", NULL), -1);
    ASSERT_EQ(oops_pkg_get_progress_info(NULL, &info), -1);
    ASSERT_EQ(oops_pkg_get_progress_info("EP0001-CUSA00001_00-0000000000000000", NULL), -1);
}

static void test_pkg_unsupported_environment(void) {
    bool exists = true;
    uint32_t prog = 99;
    oops_pkg_progress_info_t info;

    /* On host (non-PlayStation target), weak symbols resolve to NULL -> safe -1 errors */
    ASSERT_EQ(oops_pkg_init(), -1);
    ASSERT_EQ(oops_pkg_install("/data/test.pkg"), -1);
    ASSERT_EQ(oops_app_exists("CUSA00001", &exists), -1);
    ASSERT_TRUE(!exists);
    ASSERT_EQ(oops_app_uninstall("CUSA00001"), -1);
    ASSERT_EQ(oops_pkg_get_progress("EP0001-CUSA00001_00-0000000000000000", &prog), -1);
    ASSERT_EQ(prog, 0u);
    ASSERT_EQ(oops_pkg_get_progress_info("EP0001-CUSA00001_00-0000000000000000", &info), -1);
    ASSERT_EQ(info.progress_pct, 0u);
    oops_pkg_term();
}

void run_unit_tests_pkg(void) {
    TEST_SUITE_BEGIN("Package & App Management (libSceAppInstUtil)");
    RUN_TEST(test_pkg_null_safety);
    RUN_TEST(test_pkg_unsupported_environment);
}
