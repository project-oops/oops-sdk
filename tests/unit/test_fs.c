#include "oops/fs.h"
#include "oops/freestd.h"
#include "tests/test_common.h"
#include <stdio.h>

#define TEST_PATH "/tmp/test_oops_fs.tmp"

static void test_fs_null_safety(void) {
  ASSERT_EQ(oops_fs_open(NULL, 0, 0), -1);
  ASSERT_EQ(oops_fs_close(-1), -1);
  ASSERT_EQ(oops_fs_read(-1, NULL, 0), -1);
  ASSERT_EQ(oops_fs_write(-1, NULL, 10), -1);
  ASSERT_EQ(oops_fs_seek(-1, 0, 0), -1);
  ASSERT_EQ(oops_fs_tell(-1), -1);
  ASSERT_EQ(oops_fs_exists(NULL), 0);
  ASSERT_EQ(oops_fs_file_size(NULL), -1);
  ASSERT_EQ(oops_fs_read_all(NULL, NULL, NULL), -1);
  ASSERT_EQ(oops_fs_write_all(NULL, NULL, 0), -1);
  ASSERT_EQ(oops_fs_mkdir(NULL, 0), -1);
  ASSERT_EQ(oops_fs_unlink(NULL), -1);
  oops_fs_free_data(NULL);
}

static void test_fs_read_write_seek(void) {
  /* Clean up any leftover file */
  (void)oops_fs_unlink(TEST_PATH);

  ASSERT_EQ(oops_fs_exists(TEST_PATH), 0);

  /* Open for write + create */
  int fd = oops_fs_open(TEST_PATH, OOPS_O_WRONLY | OOPS_O_CREAT | OOPS_O_TRUNC, 0644);
  ASSERT_TRUE(fd >= 0);

  const char payload[] = "Hello OOPS Filesystem!\nLine 2";
  size_t len = obs_strlen(payload);
  int64_t written = oops_fs_write(fd, payload, len);
  ASSERT_EQ(written, (int64_t)len);
  ASSERT_EQ(oops_fs_close(fd), 0);

  /* Verify existence and size */
  ASSERT_EQ(oops_fs_exists(TEST_PATH), 1);
  ASSERT_EQ(oops_fs_file_size(TEST_PATH), (int64_t)len);

  /* Open for read */
  fd = oops_fs_open(TEST_PATH, OOPS_O_RDONLY, 0);
  ASSERT_TRUE(fd >= 0);

  char buf[64];
  int64_t n = oops_fs_read(fd, buf, 5);
  ASSERT_EQ(n, 5);
  buf[5] = '\0';
  ASSERT_STR_EQ(buf, "Hello");
  ASSERT_EQ(oops_fs_tell(fd), 5);

  /* Seek to end and back */
  int64_t end_pos = oops_fs_seek(fd, 0, OOPS_SEEK_END);
  ASSERT_EQ(end_pos, (int64_t)len);

  int64_t cur_pos = oops_fs_seek(fd, 6, OOPS_SEEK_SET);
  ASSERT_EQ(cur_pos, 6);

  n = oops_fs_read(fd, buf, 4);
  ASSERT_EQ(n, 4);
  buf[4] = '\0';
  ASSERT_STR_EQ(buf, "OOPS");

  ASSERT_EQ(oops_fs_close(fd), 0);

  /* Whole file helpers */
  void *data = NULL;
  size_t size = 0;
  int rc = oops_fs_read_all(TEST_PATH, &data, &size);
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(size, len);
  ASSERT_TRUE(data != NULL);
  ASSERT_STR_EQ((const char *)data, payload);
  oops_fs_free_data(data);

  /* Overwrite using write_all */
  const char new_payload[] = "Overwritten content";
  rc = oops_fs_write_all(TEST_PATH, new_payload, obs_strlen(new_payload));
  ASSERT_EQ(rc, 0);
  ASSERT_EQ(oops_fs_file_size(TEST_PATH), (int64_t)obs_strlen(new_payload));

  /* Unlink */
  ASSERT_EQ(oops_fs_unlink(TEST_PATH), 0);
  ASSERT_EQ(oops_fs_exists(TEST_PATH), 0);
}

void run_unit_tests_fs(void) {
  TEST_SUITE_BEGIN("High-Level Filesystem Subsystem");
  RUN_TEST(test_fs_null_safety);
  RUN_TEST(test_fs_read_write_seek);
}
