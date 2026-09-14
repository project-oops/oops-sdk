#ifndef OOPS_SYSTEM_H
#define OOPS_SYSTEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oops_system_info {
  int generation;        /* 4 = Orbis-generation, 5 = Prospero-generation */
  uint32_t firmware_raw; /* e.g. 0x12400009 */
  char firmware_str[16]; /* e.g. "12.40" */
  char model_str[32];    /* e.g. "CFI-1116A" */
  size_t total_ram_mb;
  size_t direct_mem_mb;
  int32_t initial_user_id;
  char user_name[32];
} oops_system_info_t;

typedef struct oops_hw_info {
  int cpu_temp_c;   /* CPU temp in Celsius (-1 if unavailable) */
  int soc_temp_c;   /* SoC primary sensor temp in Celsius (-1 if unavailable) */
  int fan_duty_pct; /* Current fan duty 0-100% (-1 if unavailable) */
  uint64_t cpu_freq_hz;   /* CPU clock in Hz (0 if unavailable) */
  char serial_number[64]; /* Console hardware serial number */
  char model_name[64];    /* Console hardware model name */
} oops_hw_info_t;

int32_t oops_user_get_initial_user_id(void);
int oops_user_get_name(int32_t user_id, char *out_name, size_t max_len);
int oops_user_get_logged_in_users(int32_t *out_user_ids, size_t max_users,
                                  size_t *out_count);
int oops_system_get_info(oops_system_info_t *out_info);
int oops_system_notify(const char *text);

/* Kernel Log & Telemetry Output (/dev/klog on target, stderr on host) */
void oops_klog(const char *tag, const char *msg);
void oops_kprintf(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
const char *oops_test_get_last_klog(void);

/* System Service Controls (libSceSystemService) */
int oops_system_hide_splash(void);
int oops_system_power_tick(void);
int oops_system_navigate_home(void);
int oops_system_get_enter_button(int *out_button); /* 0 = Circle, 1 = Cross */

/* Hardware Telemetry & Diagnostics */
int oops_system_get_hw_info(oops_hw_info_t *out_hw);
int oops_system_get_cpu_temp(int *out_temp_celsius);
int oops_system_get_soc_temp(int sensor_idx, int *out_temp_celsius);
int oops_system_get_fan_duty(int *out_duty_pct);
int oops_system_get_cpu_freq(uint64_t *out_freq_hz);
int oops_system_get_hw_serial(char *out_serial, size_t max_len);
int oops_system_get_hw_model(char *out_model, size_t max_len);

/**
 * Application Category Types (applicationCategoryType in param.json /
 * param.sfo).
 *
 * Dictates direct memory (DMEM) allocation budget, HDMI video out bus ownership
 * via SceSysAvControl, and process lifecycle / multitasking behavior.
 * Category is orthogonal to process privilege (paid / authority ID).
 */
typedef enum oops_app_category {
  /**
   * Big App / Native Game (0x00000000).
   * - Direct Memory (DMEM): Full budget (~12.5 GB on Prospero, ~5.5 GB on Orbis).
   * - Display: Exclusive ownership of primary HDMI scanout (OBS_VIDEO_BUS_MAIN
   * = 0).
   * - Multitasking: Foreground exclusive; launching another Big App suspends or
   * terminates.
   * - Linker / Auth: Requires /app0/sce_module/libc.prx; gated by PFAuthClient
   *   (/dev/pltauth patch required).
   */
  OOPS_APP_CATEGORY_BIG_APP = 0,

  /**
   * System App (0x00010000 / 65536).
   * - Direct Memory (DMEM): 0 bytes granted by ResourceArbitrator (userland
   * mmap/malloc only).
   * - Display: Denied primary HDMI scanout (sceVideoOutOpen returns
   * 0x80290001).
   * - Multitasking: Background utility / daemon / standalone tool.
   * - Linker / Auth: libc.prx not enforced by rtld; bypasses PFAuthClient.
   */
  OOPS_APP_CATEGORY_SYSTEM_APP = 0x00010000,

  /**
   * Mini App (0x00020000 / 131072).
   * - Direct Memory (DMEM): Severely constrained budget (~256 MB - 512 MB).
   * - Display: Secondary overlay / system compositor layer (not raw exclusive
   * HDMI).
   * - Multitasking: Concurrent; runs alongside an active Big App without
   * preempting it.
   */
  OOPS_APP_CATEGORY_MINI_APP = 0x00020000,

  /**
   * Daemon (0x00000003 / 3).
   * - Direct Memory (DMEM): Minimal / system pool.
   * - Display: None (headless background daemon).
   */
  OOPS_APP_CATEGORY_DAEMON = 0x00000003,

  /**
   * Media App (0x00040000 / 262144).
   * - Direct Memory (DMEM): Custom media streaming budget.
   * - Display: Dedicated HDCP / protected video path.
   */
  OOPS_APP_CATEGORY_MEDIA_APP = 0x00040000,
} oops_app_category_t;

/* Checks if /dev/pltauth is patched for native Prospero category 0 execution.
 * Returns 1 if pltauth is patched (or on Orbis/host), 0 if unpatched/failing. */
int oops_system_check_pltauth(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_SYSTEM_H */
