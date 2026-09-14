#include "oops/system.h"
#include "oops/syscall.h"
#include "oops/target.h"
#include "oops/freestd.h"
#include <stdarg.h>

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

int32_t oops_user_get_initial_user_id(void) {
  int32_t user = -1;
  if (sceUserServiceGetInitialUser) {
    if (sceUserServiceGetInitialUser(&user) != 0 && sceUserServiceInitialize) {
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

  if (sceUserServiceGetUserName) {
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

  if (!sceUserServiceGetLoginUserIdList) {
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
  if (sceKernelGetDirectMemorySize) {
    out_info->direct_mem_mb = sceKernelGetDirectMemorySize() / (1024 * 1024);
  }

  /* 4. Kernel / firmware extraction via kern.version sysctl */
  if (sysctlbyname) {
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
  if (!text || !sceSysUtilSendSystemNotificationWithText)
    return -1;
  /* Type 0 = standard system dialogue notification popup */
  return sceSysUtilSendSystemNotificationWithText(0, text);
}

int oops_system_hide_splash(void) {
  if (sceSystemServiceHideSplashScreen) {
    return sceSystemServiceHideSplashScreen();
  }
  return -1;
}

int oops_system_power_tick(void) {
  if (sceSystemServicePowerTick) {
    return sceSystemServicePowerTick();
  }
  return -1;
}

int oops_system_navigate_home(void) {
  if (sceSystemServiceNavigateToGoHome) {
    return sceSystemServiceNavigateToGoHome();
  }
  return -1;
}

int oops_system_get_enter_button(int *out_button) {
  if (!out_button)
    return -1;
  *out_button = -1;
  if (sceSystemServiceParamGetInt) {
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

int oops_system_get_cpu_temp(int *out_temp_celsius) {
  if (!out_temp_celsius)
    return -1;
  *out_temp_celsius = -1;
  if (!sceKernelGetCpuTemperature)
    return -1;
  return sceKernelGetCpuTemperature(out_temp_celsius);
}

int oops_system_get_soc_temp(int sensor_idx, int *out_temp_celsius) {
  if (!out_temp_celsius)
    return -1;
  *out_temp_celsius = -1;
  if (!sceKernelGetSocSensorTemperature)
    return -1;
  return sceKernelGetSocSensorTemperature(sensor_idx, out_temp_celsius);
}

int oops_system_get_fan_duty(int *out_duty_pct) {
  if (!out_duty_pct)
    return -1;
  *out_duty_pct = -1;
  if (!sceKernelGetCurrentFanDuty)
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
  if (!sceKernelGetCpuFrequency)
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
  if (!sceKernelGetHwSerialNumber)
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
  if (!sceKernelGetHwModelName)
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

void oops_klog(const char *tag, const char *msg) {
  if (msg == NULL) return;
  char buf[512];
  size_t pos = 0;
  if (tag != NULL && tag[0] != '\0') {
    buf[pos++] = '[';
    for (size_t i = 0; tag[i] != '\0' && pos < 40; i++) {
      buf[pos++] = tag[i];
    }
    if (pos < 42) {
      buf[pos++] = ']';
      buf[pos++] = ' ';
    }
  }
  for (size_t i = 0; msg[i] != '\0' && pos < sizeof(buf) - 2; i++) {
    buf[pos++] = msg[i];
  }
  buf[pos++] = '\n';
  buf[pos] = '\0';

#ifndef OOPS_HOST_BUILD
  (void)sys_call(SYS_klog, 7, (long)buf, 0, 0, 0, 0);
#else
  for (size_t i = 0; i < sizeof(s_host_last_klog) - 1 && buf[i] != '\0'; i++) {
    s_host_last_klog[i] = buf[i];
    s_host_last_klog[i + 1] = '\0';
  }
  fputs(buf, stderr);
#endif
}

void oops_kprintf(const char *tag, const char *fmt, ...) {
  if (fmt == NULL) return;
  char buf[512];
  va_list args;
  va_start(args, fmt);
  (void)oops_vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  oops_klog(tag, buf);
}
