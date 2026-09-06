#include "tests/test_common.h"
#include "oops/savedata.h"

static void test_savedata_modes(void) {
    ASSERT_EQ(OOPS_SAVEDATA_MODE_READ_ONLY, 0x01);
    ASSERT_EQ(OOPS_SAVEDATA_MODE_READ_WRITE, 0x02);
    ASSERT_EQ(OOPS_SAVEDATA_MODE_CREATE, 0x04);
}

static void test_savedata_null_safety(void) {
    char path[64];
    ASSERT_EQ(oops_savedata_mount(NULL, OOPS_SAVEDATA_MODE_READ_WRITE, path, sizeof(path)), -1);
    ASSERT_EQ(oops_savedata_mount("SAVE0000", OOPS_SAVEDATA_MODE_READ_WRITE, NULL, sizeof(path)), -1);
    ASSERT_EQ(oops_savedata_mount("SAVE0000", OOPS_SAVEDATA_MODE_READ_WRITE, path, 0), -1);
    ASSERT_EQ(oops_savedata_unmount(NULL, false), -1);
}

static void test_savedata_unsupported(void) {
    char path[64];
    /* Weak symbols not present on host -> returns -1 */
    ASSERT_EQ(oops_savedata_init(), -1);
    ASSERT_EQ(oops_savedata_mount("SAVE0000", OOPS_SAVEDATA_MODE_READ_WRITE, path, sizeof(path)), -1);
    ASSERT_EQ(oops_savedata_unmount("/savedata0", false), -1);
    oops_savedata_term();
}

void run_unit_tests_savedata(void) {
    TEST_SUITE_BEGIN("Save Data Management (libSceSaveData)");
    RUN_TEST(test_savedata_modes);
    RUN_TEST(test_savedata_null_safety);
    RUN_TEST(test_savedata_unsupported);
}

