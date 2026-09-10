#include "oops/krw.h"
#include "oops/syscall.h"
#include "tests/test_common.h"

static void test_krw_uninitialized_safety(void) {
  ASSERT_EQ(krw_is_ready(), 0);
  ASSERT_TRUE(krw_init(NULL) < 0);
  payload_args_t bad_args;
  memset(&bad_args, 0, sizeof(bad_args));
  ASSERT_TRUE(krw_init(&bad_args) < 0);
  ASSERT_EQ(krw_kdata_base(), 0);
  ASSERT_EQ(krw_ktext_base(), 0);
  ASSERT_EQ(krw_allproc_addr(), 0);
}

static void test_syscall_uninitialized_safety(void) {
  ASSERT_EQ(sys_call_init(NULL), -1);
  ASSERT_EQ(sys_call(SYS_getpid, 0, 0, 0, 0, 0, 0), -1);
}

void run_unit_tests_krw(void) {
  TEST_SUITE_BEGIN("Kernel Read/Write & Syscall Primitives");
  RUN_TEST(test_krw_uninitialized_safety);
  RUN_TEST(test_syscall_uninitialized_safety);
}
