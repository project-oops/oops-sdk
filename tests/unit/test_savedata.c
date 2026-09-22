#include "oops/savedata.h"
#include "oops/fs.h"
#include "tests/test_common.h"

static void test_savedata_modes(void) {
  ASSERT_EQ(OOPS_SAVEDATA_MODE_READ_ONLY, 0x01);
  ASSERT_EQ(OOPS_SAVEDATA_MODE_READ_WRITE, 0x02);
  ASSERT_EQ(OOPS_SAVEDATA_MODE_CREATE, 0x04);
}

static void test_savedata_null_safety(void) {
  char path[64];
  ASSERT_EQ(oops_savedata_mount(NULL, OOPS_SAVEDATA_MODE_READ_WRITE, path,
                                sizeof(path)),
            -1);
  ASSERT_EQ(oops_savedata_mount("SAVE0000", OOPS_SAVEDATA_MODE_READ_WRITE, NULL,
                                sizeof(path)),
            -1);
  ASSERT_EQ(
      oops_savedata_mount("SAVE0000", OOPS_SAVEDATA_MODE_READ_WRITE, path, 0),
      -1);
  ASSERT_EQ(oops_savedata_unmount(NULL, false), -1);
  ASSERT_EQ(oops_savedata_load_file(NULL, "f", NULL, NULL), -1);
  ASSERT_EQ(oops_savedata_save_file(NULL, "f", "d", 1), -1);
}

static void test_savedata_unsupported(void) {
  char path[64];
  /* Weak symbols not present on host -> returns -1 */
  ASSERT_EQ(oops_savedata_init(), -1);
  /* Mounting non-existent slot without CREATE returns -1 */
  ASSERT_EQ(oops_savedata_mount("NONEXISTENT_SLOT_999", OOPS_SAVEDATA_MODE_READ_WRITE, path,
                                sizeof(path)),
            -1);
  ASSERT_EQ(oops_savedata_unmount("/savedata0", false), -1);
  oops_savedata_term();
}

static void test_savedata_filesystem_fallback(void) {
  char path[128];

  /* 1. Mounting with CREATE succeeds and creates the slot directory */
  ASSERT_EQ(oops_savedata_mount("UNITTEST_SLOT",
                                OOPS_SAVEDATA_MODE_CREATE | OOPS_SAVEDATA_MODE_READ_WRITE,
                                path, sizeof(path)),
            0);
  ASSERT_TRUE(strlen(path) > 0);
  ASSERT_TRUE(oops_fs_exists(path));

  /* 2. Unmounting the fallback path succeeds */
  ASSERT_EQ(oops_savedata_unmount(path, true), 0);

  /* 3. Mounting existing slot in READ_ONLY without CREATE succeeds */
  char path2[128];
  ASSERT_EQ(oops_savedata_mount("UNITTEST_SLOT",
                                OOPS_SAVEDATA_MODE_READ_ONLY,
                                path2, sizeof(path2)),
            0);
  ASSERT_EQ(strcmp(path, path2), 0);
  ASSERT_EQ(oops_savedata_unmount(path2, false), 0);
}

static void test_savedata_file_helpers(void) {
  const char test_data[] = "OOPS_PERSISTENT_SETTINGS_PAYLOAD_DATA";
  size_t test_size = sizeof(test_data);

  /* 1. Save file to slot */
  ASSERT_EQ(oops_savedata_save_file("SLOT_PREFS", "settings.bin", test_data, test_size), 0);

  /* 2. Load file back */
  void *loaded_data = NULL;
  size_t loaded_size = 0;
  ASSERT_EQ(oops_savedata_load_file("SLOT_PREFS", "settings.bin", &loaded_data, &loaded_size), 0);
  ASSERT_TRUE(loaded_data != NULL);
  ASSERT_EQ(loaded_size, test_size);
  ASSERT_EQ(memcmp(loaded_data, test_data, test_size), 0);
  oops_fs_free_data(loaded_data);

  /* 3. Loading non-existent file returns error */
  void *fail_data = NULL;
  size_t fail_size = 0;
  ASSERT_NE(oops_savedata_load_file("SLOT_PREFS", "does_not_exist.bin", &fail_data, &fail_size), 0);
}

void run_unit_tests_savedata(void) {
  TEST_SUITE_BEGIN("Save Data Management (libSceSaveData & Fallback)");
  RUN_TEST(test_savedata_modes);
  RUN_TEST(test_savedata_null_safety);
  RUN_TEST(test_savedata_unsupported);
  RUN_TEST(test_savedata_filesystem_fallback);
  RUN_TEST(test_savedata_file_helpers);
}
