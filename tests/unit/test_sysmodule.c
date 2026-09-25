#include "oops/sysmodule.h"
#include "tests/test_common.h"

static void test_sysmodule_constants(void) {
  ASSERT_EQ(OOPS_SYSMODULE_NET, 0x0001);
  ASSERT_EQ(OOPS_SYSMODULE_HTTP, 0x0002);
  ASSERT_EQ(OOPS_SYSMODULE_SSL, 0x0003);
  ASSERT_EQ(OOPS_SYSMODULE_FONT, 0x0084);
  ASSERT_EQ(OOPS_SYSMODULE_IME_DIALOG, 0x0096);
  ASSERT_EQ(OOPS_SYSMODULE_MESSAGE_DIALOG, 0x00A4);
  ASSERT_EQ(OOPS_SYSMODULE_SAVE_DATA, 0x00B6);
  ASSERT_EQ(OOPS_SYSMODULE_NET_CTL, 0x0011);
  ASSERT_EQ(OOPS_SYSMODULE_JSON, 0x0080);
  ASSERT_EQ(OOPS_SYSMODULE_PNG_ENC, 0x008D);
  ASSERT_EQ(OOPS_SYSMODULE_NP_TROPHY, 0x00AD);
  ASSERT_EQ(OOPS_SYSMODULE_USBD, 0x00B7);
  ASSERT_EQ(OOPS_SYSMODULE_ZLIB, 0x00C5);
}

static void test_sysmodule_unsupported_environment(void) {
  /* On host without platform runtime, weak stubs resolve to NULL and return -1
   */
  ASSERT_EQ(oops_sysmodule_load(OOPS_SYSMODULE_FONT), -1);
  ASSERT_EQ(oops_sysmodule_unload(OOPS_SYSMODULE_FONT), -1);
  ASSERT_EQ(oops_sysmodule_is_loaded(OOPS_SYSMODULE_FONT), -1);
}

void run_unit_tests_sysmodule(void) {
  TEST_SUITE_BEGIN("System Module Loader (libSceSysmodule)");
  RUN_TEST(test_sysmodule_constants);
  RUN_TEST(test_sysmodule_unsupported_environment);
}
