#include "oops/system.h"
#include "tests/test_common.h"

static void test_system_info_query(void) {
  oops_system_info_t info;
  int rc = oops_system_get_info(&info);
  ASSERT_EQ(rc, 0);

  /* Generation must be 0 (unknown/host), 4 (Orbis-generation), or 5
   * (Prospero-generation) */
  ASSERT_TRUE(info.generation == 0 || info.generation == 4 ||
              info.generation == 5);
  ASSERT_TRUE(info.total_ram_mb >= 8192);
}

static void test_system_null_safety(void) {
  ASSERT_EQ(oops_system_get_info(NULL), -1);
  ASSERT_EQ(oops_user_get_name(0, NULL, 0), -1);
  ASSERT_EQ(oops_system_notify(NULL), -1);
}

static void test_system_hw_telemetry(void) {
  ASSERT_EQ(oops_system_get_hw_info(NULL), -1);
  ASSERT_EQ(oops_system_get_cpu_temp(NULL), -1);
  ASSERT_EQ(oops_system_get_soc_temp(0, NULL), -1);
  ASSERT_EQ(oops_system_get_fan_duty(NULL), -1);
  ASSERT_EQ(oops_system_get_cpu_freq(NULL), -1);
  ASSERT_EQ(oops_system_get_hw_serial(NULL, 0), -1);
  ASSERT_EQ(oops_system_get_hw_model(NULL, 0), -1);

  oops_hw_info_t hw;
  int rc = oops_system_get_hw_info(&hw);
  ASSERT_EQ(rc, 0);

  /* On host without platform sensors, values gracefully report -1 */
  ASSERT_EQ(hw.cpu_temp_c, -1);
  ASSERT_EQ(hw.soc_temp_c, -1);
  ASSERT_EQ(hw.fan_duty_pct, -1);
}

static void test_system_services_and_multiuser(void) {
  int32_t users[4];
  size_t count = 0;
  int button = -1;

  ASSERT_EQ(oops_user_get_logged_in_users(NULL, 4, &count), -1);
  ASSERT_EQ(oops_user_get_logged_in_users(users, 0, &count), -1);

  (void)oops_user_get_logged_in_users(users, 4, &count);

  ASSERT_EQ(oops_system_get_enter_button(NULL), -1);
  ASSERT_EQ(oops_system_hide_splash(), -1);
  ASSERT_EQ(oops_system_power_tick(), -1);
  ASSERT_EQ(oops_system_navigate_home(), -1);
  ASSERT_EQ(oops_system_get_enter_button(&button), -1);
}

static void test_system_pltauth_check(void) {
  int status = oops_system_check_pltauth();
  /* On host or PS4 it returns 1; on PS5 it returns 0 or 1 */
  ASSERT_TRUE(status == 0 || status == 1);
}

#ifdef OOPS_HOST_BUILD
const char *oops_test_get_last_klog(void);
#endif

static void test_system_klog(void) {
  /* Null safety */
  oops_klog(NULL, NULL);
  oops_kprintf(NULL, NULL);

  oops_klog("TEST", "Hello telemetry");
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[TEST] Hello telemetry\n");
#endif

  oops_kprintf("SYS", "ErrorCode: 0x%08x (%d)", 0x1234, 4660);
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SYS] ErrorCode: 0x00001234 (4660)\n");
#endif
}

void run_unit_tests_system(void) {
  TEST_SUITE_BEGIN("System & User Services");
  RUN_TEST(test_system_info_query);
  RUN_TEST(test_system_null_safety);
  RUN_TEST(test_system_hw_telemetry);
  RUN_TEST(test_system_services_and_multiuser);
  RUN_TEST(test_system_pltauth_check);
  RUN_TEST(test_system_klog);
}
