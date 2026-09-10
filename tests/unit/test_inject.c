#include "oops/inject.h"
#include "tests/test_common.h"

static void test_inject_null_safety(void) {
  ASSERT_EQ(oops_inject_elf(1234, NULL, 0, NULL), -1);
  uint8_t dummy[4] = {0};
  ASSERT_EQ(oops_inject_elf(-1, dummy, sizeof(dummy), NULL), -1);
  payload_args_t args;
  memset(&args, 0, sizeof(args));
  ASSERT_EQ(oops_inject_elf(0, dummy, sizeof(dummy), &args), -2);
}

static void test_inject_reg_layout(void) {
  ASSERT_TRUE(sizeof(struct reg) >= 160);
  ASSERT_TRUE(offsetof(struct reg, r_rip) > offsetof(struct reg, r_rax));
  ASSERT_TRUE(offsetof(struct reg, r_rsp) > offsetof(struct reg, r_rip));
}

static void test_inject_target_resolve(void) {
  ASSERT_EQ(target_resolve("42"), 42);
  ASSERT_EQ(target_resolve("9999"), 9999);
}

void run_unit_tests_inject(void) {
  TEST_SUITE_BEGIN("Process Control & Remote Injection Primitives");
  RUN_TEST(test_inject_null_safety);
  RUN_TEST(test_inject_reg_layout);
  RUN_TEST(test_inject_target_resolve);
}
