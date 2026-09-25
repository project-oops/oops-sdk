#include "oops/system.h"
#include "oops/fs.h"
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
  ASSERT_EQ(oops_system_get_enter_button(&button), -1);
  ASSERT_EQ(oops_system_hide_splash(), -1);
  ASSERT_EQ(oops_system_power_tick(), -1);
  ASSERT_EQ(oops_system_navigate_home(), -1);
  ASSERT_EQ(oops_system_launch_app(NULL), -1);
  ASSERT_EQ(oops_system_launch_app(""), -1);
  ASSERT_EQ(oops_system_launch_app("PPSA90001"), -1);

  char title_id[32];
  int suspended = 0;
  ASSERT_EQ(oops_system_get_running_app_title_id(NULL, 32), -1);
  ASSERT_EQ(oops_system_get_running_app_title_id(title_id, 0), -1);
  ASSERT_EQ(oops_system_get_running_app_title_id(title_id, sizeof(title_id)), -1);
  ASSERT_EQ(oops_system_is_app_suspended(NULL), -1);
  ASSERT_EQ(oops_system_is_app_suspended(&suspended), -1);
  ASSERT_EQ(oops_system_kill_app(-1), -1);
  ASSERT_EQ(oops_system_kill_app(123), -1);
}

static void test_system_pltauth_check(void) {
  int status = oops_system_check_pltauth();
  /* On host or Orbis it returns 1; on Prospero it returns 0 or 1 */
  ASSERT_TRUE(status == 0 || status == 1);
}

#ifdef OOPS_HOST_BUILD
const char *oops_test_get_last_klog(void);
#endif

static void test_system_klog(void) {
  /* Null safety */
  oops_klog(NULL, NULL);
  oops_kprintf(NULL, NULL);
  oops_log(NULL);

  /* Reset log identity to test fallback behavior */
  oops_log_init(NULL);
  ASSERT_TRUE(oops_log_get_app_id() == NULL);

  oops_klog("TEST", "Hello telemetry");
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[TEST] Hello telemetry\n");
#endif

  oops_kprintf("SYS", "ErrorCode: 0x%08x (%d)", 0x1234, 4660);
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SYS] ErrorCode: 0x00001234 (4660)\n");
#endif

  /* Set explicit app identity */
  oops_log_init("SCSH00001");
  ASSERT_STR_EQ(oops_log_get_app_id(), "SCSH00001");

  /* Single call without sub-tag */
  oops_log("shell starting");
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SCSH00001] shell starting\n");
#endif

  /* With component sub-tag */
  oops_klog("RENDER", "RDNA2 pipeline online");
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SCSH00001:RENDER] RDNA2 pipeline online\n");
#endif

  /* If tag matches app_id, avoid duplicate [APP:APP] */
  oops_klog("SCSH00001", "redundant tag");
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SCSH00001] redundant tag\n");
#endif

  /* Log level testing */
  ASSERT_TRUE(oops_log_get_level() == OOPS_LOG_INFO);
  oops_log_set_level(OOPS_LOG_ERROR);
  ASSERT_TRUE(oops_log_get_level() == OOPS_LOG_ERROR);

  /* Debug message must be filtered out at ERROR level */
  oops_log_debug("INP", "filtered debug message");
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SCSH00001] redundant tag\n");
#endif

  /* Error message must pass and include prefix */
  oops_log_error("INP", "fatal fault %d", 42);
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SCSH00001:INP] ERROR: fatal fault 42\n");
#endif

  /* Warn message must pass at WARN level */
  oops_log_set_level(OOPS_LOG_WARN);
  oops_log_warn("INP", "warning message");
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SCSH00001:INP] WARN: warning message\n");
#endif

  /* Info message must pass at INFO level */
  oops_log_set_level(OOPS_LOG_INFO);
  oops_log_info("INP", "info message");
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SCSH00001:INP] info message\n");
#endif

  /* Debug level enables debug messages, filters trace */
  oops_log_set_level(OOPS_LOG_DEBUG);
  oops_log_debug("INP", "visible debug message");
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SCSH00001:INP] DEBUG: visible debug message\n");
#endif
  oops_log_trace("INP", "filtered trace message");
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SCSH00001:INP] DEBUG: visible debug message\n");
#endif

  /* Trace level enables trace messages */
  oops_log_set_level(OOPS_LOG_TRACE);
  oops_log_trace("INP", "visible trace message %d", 99);
#ifdef OOPS_HOST_BUILD
  ASSERT_STR_EQ(oops_test_get_last_klog(), "[SCSH00001:INP] TRACE: visible trace message 99\n");
#endif

  /* Restore default INFO level */
  oops_log_set_level(OOPS_LOG_INFO);

  /* Reset for subsequent tests */
  oops_log_init(NULL);
}

static void test_system_disk_sink(void) {
  ASSERT_TRUE(oops_log_get_disk_sink_path() == NULL);

  int rc = oops_log_enable_disk_sink("TESTAPP", 1);
  ASSERT_EQ(rc, 0);

  const char *path = oops_log_get_disk_sink_path();
  ASSERT_TRUE(path != NULL);
  ASSERT_TRUE(strlen(path) > 0);

  /* Write logs while disk sink is active */
  oops_klog("DISK", "first disk telemetry record");
  oops_kprintf("DISK", "number=%d", 12345);

  /* Verify file exists and has content */
  ASSERT_EQ(oops_fs_exists(path), 1);
  ASSERT_TRUE(oops_fs_file_size(path) > 0);

  void *data = NULL;
  size_t size = 0;
  ASSERT_EQ(oops_fs_read_all(path, &data, &size), 0);
  ASSERT_TRUE(size > 0);
  ASSERT_TRUE(data != NULL);
  oops_fs_free_data(data);

  char path_copy[256];
  snprintf(path_copy, sizeof(path_copy), "%s", path);
  oops_log_close_disk_sink();
  ASSERT_TRUE(oops_log_get_disk_sink_path() == NULL);
  (void)oops_fs_unlink(path_copy);
}

void run_unit_tests_system(void) {
  TEST_SUITE_BEGIN("System & User Services");
  RUN_TEST(test_system_info_query);
  RUN_TEST(test_system_null_safety);
  RUN_TEST(test_system_hw_telemetry);
  RUN_TEST(test_system_services_and_multiuser);
  RUN_TEST(test_system_pltauth_check);
  RUN_TEST(test_system_klog);
  RUN_TEST(test_system_disk_sink);
}
