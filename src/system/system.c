#include "oops/system.h"
#include "oops/syscall.h"
#include "oops/target.h"
#include "oops/freestd.h"
#include "oops/fs.h"
#include "oops/sysmodule.h" /* the lazy-loaded set, made resident before the sandbox escape */
#include "oops/time.h"
#include <stdarg.h>
#ifdef OOPS_HOST_BUILD
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#endif

__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceUserServiceInitialize(const void *param);
__attribute__((weak)) int
sceUserServiceGetUserName(int32_t userId, char *userName, size_t size);
__attribute__((weak)) int sceUserServiceGetLoginUserIdList(void *list);
__attribute__((weak)) int sceSystemServiceHideSplashScreen(void);
__attribute__((weak)) int sceSystemServicePowerTick(void);
__attribute__((weak)) int sceSystemServiceNavigateToGoHome(void);
__attribute__((weak)) int sceSystemServiceParamGetInt(int32_t paramId,
                                                      int32_t *value);
__attribute__((weak)) int
sceSystemServiceLaunchApp(const char *titleId, const char *const *argv,
                          const void *param);
__attribute__((weak)) int sceSystemServiceGetMainAppTitleId(char *titleId);
__attribute__((weak)) int sceSystemServiceIsAppSuspended(void);
__attribute__((weak)) int sceSystemServiceKillApp(int appId, int, int, int);
__attribute__((weak)) int sceSystemServiceGetAppIdOfBigApp(void);
__attribute__((weak)) int
sceSysUtilSendSystemNotificationWithText(int type, const char *msg);
__attribute__((weak)) int sysctlbyname(const char *name, void *oldp,
                                       size_t *oldlenp, void *newp,
                                       size_t newlen);
__attribute__((weak)) size_t sceKernelGetDirectMemorySize(void);
/* Weak symbols for hardware telemetry (libkernel / libkernel_sys) */
__attribute__((weak)) int sceKernelGetCpuTemperature(int *temperature);
__attribute__((weak)) int sceKernelGetSocSensorTemperature(int sensor,
                                                           int *temperature);
__attribute__((weak)) int sceKernelGetCurrentFanDuty(int *unk, int *duty);
__attribute__((weak)) long sceKernelGetCpuFrequency(void);
__attribute__((weak)) int sceKernelGetHwSerialNumber(char *buffer);
__attribute__((weak)) int sceKernelGetHwModelName(char *buffer);

/* Weak graphics markers for generation detection */
__attribute__((weak)) extern void *sceAgcDriverGetDefaultOwner;
__attribute__((weak)) extern void *sceGnmSubmitDone;

int oops_symbol_is_resolved(const void *fn_ptr) {
#if defined(OOPS_HOST_BUILD) || !defined(__x86_64__)
  return (fn_ptr != NULL);
#else
  if (!fn_ptr) {
    return 0;
  }
  uintptr_t addr = (uintptr_t)fn_ptr;
  if (addr < 0x10000ULL) {
    return 0;
  }
  /* If pointer points directly into system library address space
   * (on Prospero system libraries reside >= 0x800000000ULL; main ELF is < 0x1000000ULL) */
  if (addr >= 0x1000000ULL) {
    return 1;
  }
  /* In our ELF executable text (loaded at 0x400000):
   * Inspect the PLT stub: `ff 25 disp32` (jmpq *disp(%rip)). */
  const unsigned char *code = (const unsigned char *)fn_ptr;
  if (code[0] == 0xff && code[1] == 0x25) {
    int32_t disp = *(const int32_t *)(const void *)(code + 2);
    const uintptr_t *got_slot =
        (const uintptr_t *)(const void *)(code + 6 + disp);
    uintptr_t target = *got_slot;
    /* When unresolved by the dynamic linker, got_slot contains 0 or points back
     * to the PLT stub within our text segment (< 0x1000000ULL). Calling it triggers
     * an unpatched trap (PRX_NOT_RESOLVED_FUNCTION / 0xa0020101). */
    if (target < 0x1000000ULL) {
      return 0;
    }
    return 1;
  }
  return 1;
#endif
}

int32_t oops_user_get_initial_user_id(void) {
  int32_t user = -1;
  if (oops_symbol_is_resolved((const void *)&sceUserServiceGetInitialUser)) {
    if (sceUserServiceGetInitialUser(&user) != 0 &&
        oops_symbol_is_resolved((const void *)&sceUserServiceInitialize)) {
      sceUserServiceInitialize(NULL);
      (void)sceUserServiceGetInitialUser(&user);
    }
  }
  return user;
}

int oops_user_get_name(int32_t user_id, char *out_name, size_t max_len) {
  if (!out_name || max_len == 0)
    return -1;
  out_name[0] = '\0';

  if (user_id < 0) {
    user_id = oops_user_get_initial_user_id();
  }
  if (user_id < 0)
    return -1;

  if (oops_symbol_is_resolved((const void *)&sceUserServiceGetUserName)) {
    return sceUserServiceGetUserName(user_id, out_name, max_len);
  }
  return -1;
}

int oops_user_get_logged_in_users(int32_t *out_user_ids, size_t max_users,
                                  size_t *out_count) {
  if (!out_user_ids || max_users == 0)
    return -1;
  if (out_count)
    *out_count = 0;

  if (!oops_symbol_is_resolved(
          (const void *)&sceUserServiceGetLoginUserIdList)) {
    int32_t initial = oops_user_get_initial_user_id();
    if (initial >= 0) {
      out_user_ids[0] = initial;
      if (out_count)
        *out_count = 1;
      return 0;
    }
    return -1;
  }

  struct {
    int32_t userId[4];
  } loginList;
  for (size_t i = 0; i < sizeof(loginList); i++)
    ((unsigned char *)&loginList)[i] = 0;

  int rc = sceUserServiceGetLoginUserIdList(&loginList);
  if (rc != 0)
    return rc;

  size_t count = 0;
  for (int i = 0; i < 4 && count < max_users; i++) {
    if (loginList.userId[i] >= 0 && loginList.userId[i] != 0xFF) {
      out_user_ids[count++] = loginList.userId[i];
    }
  }
  if (out_count)
    *out_count = count;
  return 0;
}

int oops_system_get_info(oops_system_info_t *out_info) {
  if (!out_info)
    return -1;
  for (size_t i = 0; i < sizeof(*out_info); i++) {
    ((unsigned char *)out_info)[i] = 0;
  }

  /* 1. Generation detection (See obSCEne D121, D255: observed, never asserted)
   */
  if ((void *)&sceAgcDriverGetDefaultOwner != NULL) {
    out_info->generation = 5;
  } else if ((void *)&sceGnmSubmitDone != NULL) {
    out_info->generation = 4;
  } else {
#if defined(OBSCENE_GEN) && (OBSCENE_GEN == 5)
    out_info->generation = 5;
#elif defined(OBSCENE_GEN) && (OBSCENE_GEN == 4)
    out_info->generation = 4;
#else
    out_info->generation =
        0; /* Unknown: could-not-look must never be reported as a generation */
#endif
  }

  /* 2. User info */
  out_info->initial_user_id = oops_user_get_initial_user_id();
  if (out_info->initial_user_id >= 0) {
    oops_user_get_name(out_info->initial_user_id, out_info->user_name,
                       sizeof(out_info->user_name));
  }

  /* 3. Memory specs (16 GB unified RAM standard on the hardware) */
  out_info->total_ram_mb = 16384;
  if (oops_symbol_is_resolved((const void *)&sceKernelGetDirectMemorySize)) {
    out_info->direct_mem_mb = sceKernelGetDirectMemorySize() / (1024 * 1024);
  }

  /* 4. Kernel / firmware extraction via kern.version sysctl */
  if (oops_symbol_is_resolved((const void *)&sysctlbyname)) {
    char version[128];
    size_t len = sizeof(version) - 1;
    for (size_t i = 0; i < sizeof(version); i++)
      version[i] = 0;

    if (sysctlbyname("kern.version", version, &len, NULL, 0) == 0) {
      version[sizeof(version) - 1] = '\0';
      /* Search for "releases/" marker */
      static const char marker[] = "releases/";
      const char *at = NULL;
      for (size_t i = 0; version[i] != '\0'; i++) {
        size_t j = 0;
        while (marker[j] != '\0' && version[i + j] == marker[j]) {
          j++;
        }
        if (marker[j] == '\0') {
          at = version + i + j;
          break;
        }
      }

      if (at != NULL) {
        size_t k = 0;
        while (at[k] != '\0' && at[k] != ' ' &&
               k + 1 < sizeof(out_info->firmware_str)) {
          out_info->firmware_str[k] = at[k];
          k++;
        }
        out_info->firmware_str[k] = '\0';

        /* Parse numeric major.minor into firmware_raw (e.g. 12.40 ->
         * 0x12400000) */
        uint32_t major = 0, minor = 0;
        const char *p = out_info->firmware_str;
        while (*p >= '0' && *p <= '9') {
          major = major * 10 + (uint32_t)(*p - '0');
          p++;
        }
        if (*p == '.')
          p++;
        while (*p >= '0' && *p <= '9') {
          minor = minor * 10 + (uint32_t)(*p - '0');
          p++;
        }
        out_info->firmware_raw = ((major / 10) << 28) | ((major % 10) << 24) |
                                 ((minor / 10) << 20) | ((minor % 10) << 16);
      }
    }
  }

  return 0;
}

int oops_system_notify(const char *text) {
  if (!text || !oops_symbol_is_resolved((const void *)&sceSysUtilSendSystemNotificationWithText))
    return -1;
  /* Type 0 = standard system dialogue notification popup */
  return sceSysUtilSendSystemNotificationWithText(0, text);
}

int oops_system_hide_splash(void) {
  if (oops_symbol_is_resolved((const void *)&sceSystemServiceHideSplashScreen)) {
    return sceSystemServiceHideSplashScreen();
  }
  return -1;
}

int oops_system_power_tick(void) {
  if (oops_symbol_is_resolved((const void *)&sceSystemServicePowerTick)) {
    return sceSystemServicePowerTick();
  }
  return -1;
}

int oops_system_navigate_home(void) {
  if (oops_symbol_is_resolved((const void *)&sceSystemServiceNavigateToGoHome)) {
    return sceSystemServiceNavigateToGoHome();
  }
  return -1;
}

int oops_system_get_enter_button(int *out_button) {
  if (!out_button)
    return -1;
  *out_button = -1;
  if (oops_symbol_is_resolved((const void *)&sceSystemServiceParamGetInt)) {
    int32_t val = -1;
    /* 1000 = ORBIS_SYSTEM_SERVICE_PARAM_ID_ENTER_BUTTON_ASSIGN */
    int rc = sceSystemServiceParamGetInt(1000, &val);
    if (rc == 0) {
      *out_button = (int)val;
      return 0;
    }
    return rc;
  }
  return -1;
}

int oops_system_launch_app(const char *title_id) {
  if (!title_id || title_id[0] == '\0') {
    return -1;
  }
  if (oops_symbol_is_resolved((const void *)&sceSystemServiceLaunchApp)) {
    int32_t user_id = oops_user_get_initial_user_id();
    struct {
      size_t size;
      int32_t userId;
      int32_t enableCrashReport;
      int32_t checkAppSystemVer;
    } param;
    for (size_t i = 0; i < sizeof(param); i++) {
      ((unsigned char *)&param)[i] = 0;
    }
    param.size = sizeof(param);
    param.userId = (user_id >= 0) ? user_id : 0;
    param.checkAppSystemVer = 2; /* SkipSystemUpdateCheck (0x2) */

    const char *argv[2] = {title_id, 0};
    int ret = sceSystemServiceLaunchApp(title_id, argv, &param);
    oops_kprintf("SYSTEM", "sceSystemServiceLaunchApp(%s): ret = 0x%08x\n",
                 title_id, ret);
    return ret;
  }
  return -1;
}

int oops_system_get_running_app_title_id(char *out_title_id, size_t max_len) {
  if (!out_title_id || max_len == 0) {
    return -1;
  }
  out_title_id[0] = '\0';
  if (oops_symbol_is_resolved((const void *)&sceSystemServiceGetMainAppTitleId)) {
    char tid[64] = {0};
    int rc = sceSystemServiceGetMainAppTitleId(tid);
    if (rc == 0 && tid[0] != '\0') {
      size_t i = 0;
      while (tid[i] && i + 1 < max_len) {
        out_title_id[i] = tid[i];
        i++;
      }
      out_title_id[i] = '\0';
      return 0;
    }
  }
  return -1;
}

int oops_system_is_app_suspended(int *out_is_suspended) {
  if (!out_is_suspended) {
    return -1;
  }
  *out_is_suspended = 0;
  if (oops_symbol_is_resolved((const void *)&sceSystemServiceIsAppSuspended)) {
    *out_is_suspended = (sceSystemServiceIsAppSuspended() != 0) ? 1 : 0;
    return 0;
  }
  return -1;
}

int oops_system_kill_app(int app_id) {
  if (oops_symbol_is_resolved((const void *)&sceSystemServiceKillApp)) {
    int target = app_id;
    if (target <= 0 && oops_symbol_is_resolved((const void *)&sceSystemServiceGetAppIdOfBigApp)) {
      target = sceSystemServiceGetAppIdOfBigApp();
    }
    if (target > 0) {
      return sceSystemServiceKillApp(target, -1, 0, 0);
    }
  }
  return -1;
}

int oops_system_get_cpu_temp(int *out_temp_celsius) {
  if (!out_temp_celsius)
    return -1;
  *out_temp_celsius = -1;
  if (!oops_symbol_is_resolved((const void *)&sceKernelGetCpuTemperature))
    return -1;
  return sceKernelGetCpuTemperature(out_temp_celsius);
}

int oops_system_get_soc_temp(int sensor_idx, int *out_temp_celsius) {
  if (!out_temp_celsius)
    return -1;
  *out_temp_celsius = -1;
  if (!oops_symbol_is_resolved((const void *)&sceKernelGetSocSensorTemperature))
    return -1;
  return sceKernelGetSocSensorTemperature(sensor_idx, out_temp_celsius);
}

int oops_system_get_fan_duty(int *out_duty_pct) {
  if (!out_duty_pct)
    return -1;
  *out_duty_pct = -1;
  if (!oops_symbol_is_resolved((const void *)&sceKernelGetCurrentFanDuty))
    return -1;
  int unk = 0;
  int raw_duty = 0;
  int rc = sceKernelGetCurrentFanDuty(&unk, &raw_duty);
  if (rc == 0) {
    /* Convert 0-255 duty cycle to 0-100% */
    *out_duty_pct = (raw_duty * 100) / 255;
  }
  return rc;
}

int oops_system_get_cpu_freq(uint64_t *out_freq_hz) {
  if (!out_freq_hz)
    return -1;
  *out_freq_hz = 0;
  if (!oops_symbol_is_resolved((const void *)&sceKernelGetCpuFrequency))
    return -1;
  long freq = sceKernelGetCpuFrequency();
  if (freq > 0) {
    *out_freq_hz = (uint64_t)freq;
    return 0;
  }
  return -1;
}

int oops_system_get_hw_serial(char *out_serial, size_t max_len) {
  if (!out_serial || max_len == 0)
    return -1;
  out_serial[0] = '\0';
  if (!oops_symbol_is_resolved((const void *)&sceKernelGetHwSerialNumber))
    return -1;
  char buffer[1024];
  for (size_t i = 0; i < sizeof(buffer); i++)
    buffer[i] = 0;
  int rc = sceKernelGetHwSerialNumber(buffer);
  if (rc == 0) {
    size_t i = 0;
    while (buffer[i] && i + 1 < max_len) {
      out_serial[i] = buffer[i];
      i++;
    }
    out_serial[i] = '\0';
  }
  return rc;
}

int oops_system_get_hw_model(char *out_model, size_t max_len) {
  if (!out_model || max_len == 0)
    return -1;
  out_model[0] = '\0';
  if (!oops_symbol_is_resolved((const void *)&sceKernelGetHwModelName))
    return -1;
  char buffer[1024];
  for (size_t i = 0; i < sizeof(buffer); i++)
    buffer[i] = 0;
  int rc = sceKernelGetHwModelName(buffer);
  if (rc == 0) {
    size_t i = 0;
    while (buffer[i] && i + 1 < max_len) {
      out_model[i] = buffer[i];
      i++;
    }
    out_model[i] = '\0';
  }
  return rc;
}

int oops_system_get_hw_info(oops_hw_info_t *out_hw) {
  if (!out_hw)
    return -1;
  for (size_t i = 0; i < sizeof(*out_hw); i++) {
    ((unsigned char *)out_hw)[i] = 0;
  }

  out_hw->cpu_temp_c = -1;
  out_hw->soc_temp_c = -1;
  out_hw->fan_duty_pct = -1;

  (void)oops_system_get_cpu_temp(&out_hw->cpu_temp_c);
  (void)oops_system_get_soc_temp(0, &out_hw->soc_temp_c);
  (void)oops_system_get_fan_duty(&out_hw->fan_duty_pct);
  (void)oops_system_get_cpu_freq(&out_hw->cpu_freq_hz);
  (void)oops_system_get_hw_serial(out_hw->serial_number,
                                  sizeof(out_hw->serial_number));
  (void)oops_system_get_hw_model(out_hw->model_name,
                                 sizeof(out_hw->model_name));

  return 0;
}

int oops_system_check_pltauth(void) {
#if defined(OOPS_HOST_BUILD) || !defined(__FreeBSD__) || OOPS_TARGET_IS_ORBIS
  return 1;
#else
  int fd = (int)sys_call(SYS_open, (long)"/dev/pltauth", 0 /* O_RDONLY */, 0, 0,
                         0, 0);
  if (fd < 0) {
    fd = (int)sys_call(SYS_open, (long)"/dev/pltauth", 2 /* O_RDWR */, 0, 0, 0,
                       0);
  }
  if (fd < 0) {
    return 0;
  }
  long ret = sys_call(SYS_ioctl, (long)fd, 0xdeadbeef, 0, 0, 0, 0);
  sys_call(SYS_close, (long)fd, 0, 0, 0, 0, 0);
  return (ret == 0) ? 1 : 0;
#endif
}

#ifdef OOPS_HOST_BUILD
#include <stdio.h>
static char s_host_last_klog[512] = {0};
const char *oops_test_get_last_klog(void) { return s_host_last_klog; }
#endif

static char s_app_id[32] = {0};
static int s_app_id_resolved = 0;

void oops_log_init(const char *app_id) {
  if (app_id != NULL && app_id[0] != '\0') {
    obs_strncpy(s_app_id, app_id, sizeof(s_app_id) - 1);
    s_app_id[sizeof(s_app_id) - 1] = '\0';
    s_app_id_resolved = 1;
  } else {
    s_app_id[0] = '\0';
    s_app_id_resolved = 0;
  }
  /* The `system` channel, if the launch asked for one. Every other subsystem asks for its own
   * when it starts; this is logging asking for logging's. A title that calls
   * `oops_log_set_level` afterwards still wins, which is the order a caller would expect. */
  oops_log_set_level(oops_log_channel_level("system", oops_log_get_level()));
}

const char *oops_log_get_app_id(void) {
  if (s_app_id_resolved && s_app_id[0] != '\0') {
    return s_app_id;
  }

#if defined(OOPS_APP_ID)
  obs_strncpy(s_app_id, OOPS_APP_ID, sizeof(s_app_id) - 1);
  s_app_id[sizeof(s_app_id) - 1] = '\0';
  s_app_id_resolved = 1;
  return s_app_id;
#elif defined(OOPS_APP_NAME)
  obs_strncpy(s_app_id, OOPS_APP_NAME, sizeof(s_app_id) - 1);
  s_app_id[sizeof(s_app_id) - 1] = '\0';
  s_app_id_resolved = 1;
  return s_app_id;
#endif

  /* On target: check /app0/sce_sys/param.json */
#ifndef OOPS_HOST_BUILD
  int fd = oops_fs_open("/app0/sce_sys/param.json", OOPS_O_RDONLY, 0);
  if (fd >= 0) {
    char pbuf[256];
    int64_t n = oops_fs_read(fd, pbuf, sizeof(pbuf) - 1);
    oops_fs_close(fd);
    if (n > 0) {
      pbuf[n] = '\0';
      const char *key = "\"titleId\"";
      size_t klen = obs_strlen(key);
      for (size_t i = 0; i + klen + 4 < (size_t)n; i++) {
        if (obs_strncmp(pbuf + i, key, klen) == 0) {
          const char *p = pbuf + i + klen;
          while (*p == ' ' || *p == ':' || *p == '\t' || *p == '"') p++;
          size_t len = 0;
          while (p[len] != '\0' && p[len] != '"' && p[len] != ',' && p[len] != ' ' && p[len] != '\n' && len < sizeof(s_app_id) - 1) {
            s_app_id[len] = p[len];
            len++;
          }
          s_app_id[len] = '\0';
          if (len > 0) {
            s_app_id_resolved = 1;
            return s_app_id;
          }
        }
      }
    }
  }
#endif

  s_app_id_resolved = 1;
  return NULL;
}

static oops_log_level_t s_log_level = OOPS_LOG_INFO;
static int s_disk_sink_fd = -1;
static int s_disk_sink_ts_fd = -1;
static char s_disk_sink_path[128] = {0};

void oops_log_set_level(oops_log_level_t level) {
  s_log_level = level;
}

oops_log_level_t oops_log_get_level(void) {
  return s_log_level;
}

/* ---------------------------------------------------------------------------
 * `/app0/oops-log`: the verbosity a launch asked for. See `<oops/system.h>`.
 *
 * Held as the file's own bytes rather than parsed into a table, because the table would need a
 * maximum number of channels and a maximum name length, and a linear scan of a file this size is
 * done once per subsystem at startup. The whole point is that adding a channel needs no change
 * here.
 * --------------------------------------------------------------------------- */

static char s_log_cfg[512];
static int s_log_cfg_read = 0;

static void oops_log_cfg_load(void) {
  if (s_log_cfg_read) return;
  s_log_cfg_read = 1;
  s_log_cfg[0] = '\0';
#ifndef OOPS_HOST_BUILD
  int fd = oops_fs_open("/app0/oops-log", OOPS_O_RDONLY, 0);
  if (fd < 0) return;
  int64_t n = oops_fs_read(fd, s_log_cfg, sizeof(s_log_cfg) - 1);
  oops_fs_close(fd);
  s_log_cfg[(n > 0) ? (size_t)n : 0u] = '\0';
#endif
}

/* A level by name or digit, or `fallback` for anything else - a typo must not silence a
 * subsystem, which is the failure that would be hardest to notice from the log it produces. */
static oops_log_level_t oops_log_level_of(const char *s, size_t len,
                                          oops_log_level_t fallback) {
  static const struct {
    const char *name;
    oops_log_level_t level;
  } names[] = {
      {"none", OOPS_LOG_NONE},   {"error", OOPS_LOG_ERROR}, {"warn", OOPS_LOG_WARN},
      {"info", OOPS_LOG_INFO},   {"debug", OOPS_LOG_DEBUG}, {"trace", OOPS_LOG_TRACE},
  };
  if (len == 1u && s[0] >= '0' && s[0] <= '5') {
    return (oops_log_level_t)(s[0] - '0');
  }
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (obs_strlen(names[i].name) == len && obs_strncmp(s, names[i].name, len) == 0) {
      return names[i].level;
    }
  }
  return fallback;
}

oops_log_level_t oops_log_channel_level(const char *channel, oops_log_level_t fallback) {
  if (channel == NULL || *channel == '\0') return fallback;
  oops_log_cfg_load();
  if (s_log_cfg[0] == '\0') return fallback;

  const size_t want = obs_strlen(channel);
  const char *p = s_log_cfg;
  while (*p != '\0') {
    /* One line, with `#` ending it. Leading blanks are skipped so an indented file reads. */
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    const char *line = p;
    while (*p != '\0' && *p != '\n') p++;
    const char *end = p;
    for (const char *h = line; h < end; h++) {
      if (*h == '#') { end = h; break; }
    }

    const char *eq = (const char *)0;
    for (const char *c = line; c < end; c++) {
      if (*c == '=') { eq = c; break; }
    }
    if (eq != (const char *)0) {
      /* The name, without the blanks either side of it. */
      const char *ns = line;
      const char *ne = eq;
      while (ns < ne && (*ns == ' ' || *ns == '\t')) ns++;
      while (ne > ns && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
      if ((size_t)(ne - ns) == want && obs_strncmp(ns, channel, want) == 0) {
        const char *vs = eq + 1;
        const char *ve = end;
        while (vs < ve && (*vs == ' ' || *vs == '\t')) vs++;
        while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r')) ve--;
        return oops_log_level_of(vs, (size_t)(ve - vs), fallback);
      }
    }
    if (*p == '\n') p++;
  }
  return fallback;
}

void oops_klog_level(oops_log_level_t level, const char *tag, const char *msg) {
  if (msg == NULL || level == OOPS_LOG_NONE || level > s_log_level) return;
  char buf[512];
  size_t pos = 0;

  const char *app = oops_log_get_app_id();
  int have_app = (app != NULL && app[0] != '\0');
  int have_tag = (tag != NULL && tag[0] != '\0');

  /* If tag is identical to app, treat as no extra sub-tag */
  if (have_app && have_tag && obs_strcmp(tag, app) == 0) {
    have_tag = 0;
  }

  if (have_app || have_tag) {
    buf[pos++] = '[';
    if (have_app) {
      for (size_t i = 0; app[i] != '\0' && pos < 32; i++) {
        buf[pos++] = app[i];
      }
      if (have_tag && pos < 48) {
        buf[pos++] = ':';
      }
    }
    if (have_tag) {
      for (size_t i = 0; tag[i] != '\0' && pos < 48; i++) {
        buf[pos++] = tag[i];
      }
    }
    if (pos < 50) {
      buf[pos++] = ']';
      buf[pos++] = ' ';
    }
  }

  /* Level prefix for non-INFO messages */
  const char *lvl_prefix = NULL;
  if (level == OOPS_LOG_ERROR) {
    lvl_prefix = "ERROR: ";
  } else if (level == OOPS_LOG_WARN) {
    lvl_prefix = "WARN: ";
  } else if (level == OOPS_LOG_DEBUG) {
    lvl_prefix = "DEBUG: ";
  } else if (level == OOPS_LOG_TRACE) {
    lvl_prefix = "TRACE: ";
  }

  if (lvl_prefix != NULL) {
    for (size_t i = 0; lvl_prefix[i] != '\0' && pos < sizeof(buf) - 2; i++) {
      buf[pos++] = lvl_prefix[i];
    }
  }

  for (size_t i = 0; msg[i] != '\0' && pos < sizeof(buf) - 2; i++) {
    buf[pos++] = msg[i];
  }
  buf[pos++] = '\n';
  buf[pos] = '\0';

#ifndef OOPS_HOST_BUILD
  (void)sys_call(SYS_klog, 7, (long)buf, 0, 0, 0, 0);
  (void)sys_call(SYS_write, 1, (long)buf, (long)pos, 0, 0, 0);
  if (s_disk_sink_fd >= 0) {
    (void)sys_call(SYS_write, s_disk_sink_fd, (long)buf, (long)pos, 0, 0, 0);
  }
  if (s_disk_sink_ts_fd >= 0) {
    (void)sys_call(SYS_write, s_disk_sink_ts_fd, (long)buf, (long)pos, 0, 0, 0);
  }
#else
  for (size_t i = 0; i < sizeof(s_host_last_klog) - 1 && buf[i] != '\0'; i++) {
    s_host_last_klog[i] = buf[i];
    s_host_last_klog[i + 1] = '\0';
  }
  fputs(buf, stderr);
  if (s_disk_sink_fd >= 0) {
    (void)write(s_disk_sink_fd, buf, pos);
  }
  if (s_disk_sink_ts_fd >= 0) {
    (void)write(s_disk_sink_ts_fd, buf, pos);
  }
#endif
}

void oops_kprintf_level(oops_log_level_t level, const char *tag, const char *fmt, ...) {
  if (fmt == NULL || level == OOPS_LOG_NONE || level > s_log_level) return;
  char buf[512];
  va_list args;
  va_start(args, fmt);
  int len = oops_vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (len < 0) return;
  oops_klog_level(level, tag, buf);
}

void oops_log(const char *fmt, ...) {
  if (fmt == NULL) return;
  char buf[512];
  va_list args;
  va_start(args, fmt);
  int len = oops_vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (len < 0) return;
  oops_klog_level(OOPS_LOG_INFO, NULL, buf);
}

void oops_klog(const char *tag, const char *msg) {
  oops_klog_level(OOPS_LOG_INFO, tag, msg);
}

void oops_kprintf(const char *tag, const char *fmt, ...) {
  if (fmt == NULL) return;
  char buf[512];
  va_list args;
  va_start(args, fmt);
  int len = oops_vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (len < 0) return;
  oops_klog_level(OOPS_LOG_INFO, tag, buf);
}

int oops_log_enable_disk_sink(const char *app_name, int archive_timestamped) {
  if (s_disk_sink_fd >= 0 || s_disk_sink_ts_fd >= 0) {
    oops_log_close_disk_sink();
  }

  const char *app_id = app_name;
  if (!app_id || app_id[0] == '\0') {
    app_id = oops_log_get_app_id();
  }
  if (!app_id || app_id[0] == '\0') {
    app_id = "default";
  }

  char dir[256];
  if (oops_fs_get_storage_dir(OOPS_STORAGE_PREFER_USB, dir, sizeof(dir)) != 0) {
    return -1;
  }

  char path[256];
  (void)oops_snprintf(path, sizeof(path), "%s/latest.log", dir);

#ifndef OOPS_HOST_BUILD
  /* 0x0601 = O_WRONLY(0x1) | O_CREAT(0x200) | O_TRUNC(0x400) */
  long fd = sys_call(SYS_open, (long)path, 0x0601, 0666, 0, 0, 0);
  if (fd < 0) {
    return -1;
  }
  s_disk_sink_fd = (int)fd;
#else
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    return -1;
  }
  s_disk_sink_fd = fd;
#endif

  size_t plen = obs_strlen(path);
  if (plen + 1 < sizeof(s_disk_sink_path)) {
    for (size_t i = 0; i <= plen; i++) {
      s_disk_sink_path[i] = path[i];
    }
  } else {
    s_disk_sink_path[0] = '\0';
  }

  if (archive_timestamped) {
    uint64_t ts = 0;
#ifndef OOPS_HOST_BUILD
    struct {
      int64_t sec;
      long nsec;
    } tspec;
    if (sys_call(SYS_clock_gettime, 0, (long)&tspec, 0, 0, 0, 0) == 0 && tspec.sec > 0) {
      ts = (uint64_t)tspec.sec;
    }
#else
    time_t now = time(NULL);
    if (now > 0) {
      ts = (uint64_t)now;
    }
#endif
    if (ts > 0) {
      char ts_path[256];
      (void)oops_snprintf(ts_path, sizeof(ts_path), "%s/log-%lu.txt", dir, (unsigned long)ts);
#ifndef OOPS_HOST_BUILD
      long ts_fd = sys_call(SYS_open, (long)ts_path, 0x0601, 0666, 0, 0, 0);
      if (ts_fd >= 0) {
        s_disk_sink_ts_fd = (int)ts_fd;
      }
#else
      int ts_fd = open(ts_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (ts_fd >= 0) {
        s_disk_sink_ts_fd = ts_fd;
      }
#endif
    }
  }

  return 0;
}

const char *oops_log_get_disk_sink_path(void) {
  return (s_disk_sink_fd >= 0 && s_disk_sink_path[0] != '\0') ? s_disk_sink_path : NULL;
}

void oops_log_close_disk_sink(void) {
  if (s_disk_sink_fd >= 0) {
#ifndef OOPS_HOST_BUILD
    (void)sys_call(SYS_close, s_disk_sink_fd, 0, 0, 0, 0, 0);
#else
    (void)close(s_disk_sink_fd);
#endif
    s_disk_sink_fd = -1;
  }
  if (s_disk_sink_ts_fd >= 0) {
#ifndef OOPS_HOST_BUILD
    (void)sys_call(SYS_close, s_disk_sink_ts_fd, 0, 0, 0, 0, 0);
#else
    (void)close(s_disk_sink_ts_fd);
#endif
    s_disk_sink_ts_fd = -1;
  }
  s_disk_sink_path[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* Sandbox escape via decoupled daemon handshake                        */
/* ------------------------------------------------------------------ */
/* Sandbox escape via loopback IPC handshake                            */
/* ------------------------------------------------------------------ */

/**
 * oops_system_escape_sandbox — explicit opt-in sandbox escape.
 *
 * Connects to the resident sandbox-daemon via TCP 127.0.0.1:9069,
 * sends the calling process PID (4 bytes), and reads back an int32_t
 * status code (0 = success, negative = failure).
 *
 * This is the client-side half of the decoupled namespace service
 * described in oops-sdk/docs/decisions/D004.
 *
 * Returns: 0 on success, -1 on connection failure or timeout.
 */
int oops_system_escape_sandbox(void) {
#ifndef OOPS_HOST_BUILD
  /*
   * **This takes `/app0` with it. A title that reads its own package must not call this.**
   *
   * Measured on 2026-09-23 with a probe either side of the call: `/app0/eboot.bin` exists
   * before and does not exist after. `/app0` is the package mounted *inside* the sandbox, so
   * leaving the sandbox leaves it behind - and a title whose data, textures, models and themes
   * all live there is left running with none of them. Neverball did exactly that: a black
   * screen, `Failure to open "classic" theme file`, a window at the default size because even
   * its config had gone, and a thousand draw calls a frame of geometry with no assets on it.
   * Nothing faults, so there is nothing to find except an absence.
   *
   * What it is for is reaching `/data` and the storage outside the package, and a title that
   * wants somewhere to write should look at `/app0` first: it is writable, which is not
   * obvious, and it is the reason the five candidate paths a title usually probes all refuse -
   * they name the package from outside, where the process cannot see it.
   *
   * **Everything this SDK loads on demand is loaded first, because after the escape nothing can
   * be.**
   *
   * The daemon moves this process out of its randomised namespace - the one whose libraries a
   * crash dump names `/5DzF14NgCB/common/lib/...`, with a different prefix every boot. Modules
   * already resident keep working; a module the process asks for *afterwards* is looked for
   * along a path it no longer has, and the call into it raises
   * `0xa0020101 PRX_NOT_RESOLVED_FUNCTION`.
   *
   * That is not a theoretical ordering hazard. Neverball mounted its savedata before starting
   * SDL on 2026-09-23, which reached this through `oops_savedata_mount`'s fallback, and the
   * title then died inside `PROSPERO_VideoInit` - a few calls past an `oops_gfx_create` that had
   * just succeeded - on `oops_keyboard_init`, whose first act is to load module 0x0106. No fault
   * in the graphics it had just brought up, no GL error, just a title that exited before its
   * first frame and a log that stopped.
   *
   * The eight below are every module this SDK loads lazily - `audiodec.c`, `keyboard.c`,
   * `mouse.c`, `netctl.c`, `dialog.c` twice, `savedata.c` and `videodec.c`. Loading all of them
   * costs a title that wanted one writable directory some resident memory it may never use, and
   * that is the cheaper side of the trade by a distance: the alternative is the first use of any
   * of them killing the process, at a call site with no connection to the escape that caused it.
   *
   * **A module added to that list and not added here will fail the same way.** There is no way
   * to catch it at build time, so it is written down instead: anything reached through
   * `oops_sysmodule_load` on first use belongs in this list.
   */
  {
    static const uint16_t needed[] = {
        OOPS_SYSMODULE_KEYBOARD,       OOPS_SYSMODULE_MOUSE,
        OOPS_SYSMODULE_SAVE_DATA,      OOPS_SYSMODULE_NET_CTL,
        OOPS_SYSMODULE_IME_DIALOG,     OOPS_SYSMODULE_MESSAGE_DIALOG,
        OOPS_SYSMODULE_AUDIO_DEC,      OOPS_SYSMODULE_VIDEODEC,
    };
    for (unsigned i = 0; i < sizeof(needed) / sizeof(needed[0]); i++) {
      /* Already-resident is success and costs nothing; a refusal is not fatal here - it only
         means that one module is no better off than it would have been without this. */
      (void)oops_sysmodule_load(needed[i]);
    }
  }

  /* Resolve current PID */
  pid_t my_pid = (pid_t)sys_call(SYS_getpid, 0, 0, 0, 0, 0, 0);
  if (my_pid <= 0) {
    return -1;
  }

  /* Create a stream socket */
  int sock = (int)sys_call(SYS_socket, 2 /* AF_INET */,
                           1 /* SOCK_STREAM */, 6 /* IPPROTO_TCP */, 0, 0, 0);
  if (sock < 0) {
    return -1;
  }

  /* Build sockaddr_in for 127.0.0.1:9069 (FreeBSD / Prospero ABI) */
  char sockaddr[16];
  sockaddr[0] = 16;      /* sin_len = sizeof(struct sockaddr_in) */
  sockaddr[1] = 2;       /* sin_family = AF_INET (2) */
  sockaddr[2] = 0x23;    /* port high byte: 9069 = 0x236D */
  sockaddr[3] = 0x6D;    /* port low byte */
  sockaddr[4] = 127;     /* 127.0.0.1 (network byte order) */
  sockaddr[5] = 0;
  sockaddr[6] = 0;
  sockaddr[7] = 1;
  for (int i = 8; i < 16; i++) {
    sockaddr[i] = 0;
  }

  /* Connect with a short timeout (3 seconds) */
  long connect_rc = sys_call(SYS_connect, sock, (long)sockaddr, 16, 0, 0, 0);
  if (connect_rc != 0) {
    sys_call(SYS_close, sock, 0, 0, 0, 0, 0);
    return -1;
  }

  /* Send PID */
  long written = sys_call(SYS_write, sock, (long)&my_pid, 4, 0, 0, 0);
  if (written != 4) {
    sys_call(SYS_close, sock, 0, 0, 0, 0, 0);
    return -1;
  }

  /* Read 4-byte status code */
  int32_t status = 0;
  long read_rc = sys_call(SYS_read, sock, (long)&status, 4, 0, 0, 0);
  sys_call(SYS_close, sock, 0, 0, 0, 0, 0);

  if (read_rc != 4 || status != 0) {
    return -1;
  }

  return 0;
#else
  /* On the host side there is no sandbox to escape. */
  (void)0;
  return 0;
#endif
}

#ifndef OOPS_HOST_BUILD
/*
 * The vendor sleep, bound here rather than reached through `oops_time_sleep_ms`.
 *
 * `time.c` binds the same symbol the same way, and calling into it would be the tidier-looking
 * choice. It is not taken because four titles link `system.c` and not `time.c` - `tls-probe`,
 * `injector`, `tracer` and `pad-viz` - and a title's link ignores unresolved symbols rather than
 * failing, so the tidier choice buys a symbol that resolves nowhere and traps when first called.
 * A duplicated weak binding is the cheaper of the two.
 */
__attribute__((weak)) int sceKernelUsleep(unsigned int microseconds);
#endif

/*
 * The dashboard's close signal, caught. The platform's `sigaction` is a weak import here for the
 * same reason `sceKernelUsleep` above is: a title that does not install the handler must not
 * acquire a symbol that resolves nowhere. `seashell` proved this exact arrangement closes a
 * big-app cleanly (`home_main.c`); this is it, shared so every title can cooperate.
 */
__attribute__((weak)) int _sigaction(int sig, const void *act, void *oact);

static volatile int s_close_requested = 0;

#ifndef OOPS_HOST_BUILD
/* Only referenced by the install below, whose body is host-compiled away - so the handler is too,
 * or a host build at -Werror trips on an unused static function. */
static void oops_close_signal_handler(int sig) {
  (void)sig;
  s_close_requested = 1;
}
#endif

void oops_system_install_close_handler(void) {
#ifndef OOPS_HOST_BUILD
  if (!oops_symbol_is_resolved((const void *)&_sigaction)) {
    return;
  }
  /*
   * A `struct sigaction` this platform's kernel accepts: the handler pointer is its first field.
   * The rest (mask, flags) stays zero, which is the default disposition a plain handler wants.
   * 32 bytes is comfortably larger than the struct, so the tail is ignored. This is the layout
   * seashell uses and closes cleanly with.
   */
  unsigned char act[32];
  for (size_t i = 0; i < sizeof(act); i++) {
    act[i] = 0;
  }
  *(void **)(void *)(act + 0) = (void *)(uintptr_t)&oops_close_signal_handler;
  (void)_sigaction(15 /* SIGTERM */, act, 0);
  (void)_sigaction(2 /* SIGINT */, act, 0);
  (void)_sigaction(1 /* SIGHUP */, act, 0);
#endif
}

int oops_system_close_requested(void) {
  return s_close_requested;
}

void oops_system_park_until_closed(void) {
#ifndef OOPS_HOST_BUILD
  /*
   * **One line before the idling starts, so a harness knows the work is over.**
   *
   * A payload cannot exit. `exit`, `_Exit` and `sceKernelExit` are absent, `_exit` raises
   * `SIGSYS` because a big-app container's credentials do not permit FreeBSD syscall 1, and
   * returning from the entry point faults at zero because the dynamic linker gives it no caller
   * frame - all measured, `REQ-20260917T1450Z-2e71`. Parking is the conforming ending, and the
   * cost of it is that nothing outside can tell "finished and idling" from "still working": a
   * watcher following the title has to wait out its own timeout either way, which is a two
   * minute wait after a run that took forty seconds.
   *
   * So the payload says so. This is the last line any payload prints, it is printed exactly
   * once, and its text is fixed - a watcher greps for `park: work done` and stops following.
   * It is not a substitute for a result: whatever the payload measured is already in the log
   * above, because every caller of this function prints its own verdict first.
   */
  oops_klog("park", "work done");
  for (;;) {
    if (sceKernelUsleep) {
      (void)sceKernelUsleep(1000000u);
    } else {
      /*
       * No vendor sleep bound. Spinning is wrong for a whole second at a time, but this path
       * only exists so that the function still honours "never returns" when the platform is not
       * what it was measured to be - and a busy wait that keeps the log intact is better than a
       * return that faults at zero.
       */
      for (unsigned i = 0; i < 1000000u; i++) {
        __asm__ __volatile__("pause");
      }
    }
  }
#else
  /*
   * A host test has a real process lifecycle, so nothing on the host has any business calling
   * this - and a host build that hangs forever is a much worse outcome than one that stops. The
   * trap says "this was called where it makes no sense" rather than pretending to park.
   */
  __builtin_trap();
#endif
}

/* Suspend cooperation (pump the system event queue, drain the GPU, reach a suspend point) is an
 * opt-in translation unit, `suspend.c`, linked only by the big-app titles that service it. The
 * real functions live there and carry a weak `sceSystemServiceReceiveEvent` reference; a binary
 * that never suspends - a probe, a headless utility, the plain GL cubes - does not link it and
 * does not have to claim that symbol.
 *
 * These weak no-ops stand in when it is absent, so `gl_context.c`, `glut.c` and the SDL backend
 * can call the sequence unconditionally: a title that links `suspend.c` gets the strong versions
 * there (they override these), and everything else gets a harmless no-op that references nothing.
 * That is the whole opt-in: list `src/system/suspend.c` to be suspendable, omit it to not be. */
__attribute__((weak)) void oops_system_set_suspend_drain(void (*drain)(void)) { (void)drain; }
__attribute__((weak)) int oops_system_pump_events(void) { return 0; }
__attribute__((weak)) void oops_system_prepare_for_suspend(void) {}
