#include "oops/system.h"
#include "oops/syscall.h"
#include "oops/target.h"
#include "oops/freestd.h"
#include "oops/fs.h"
#include "oops/time.h"
#include <stdarg.h>
#ifdef OOPS_HOST_BUILD
#include <unistd.h>
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

int oops_system_launch_app(const char *title_id) {
  if (!title_id || title_id[0] == '\0') {
    return -1;
  }
  if (sceSystemServiceLaunchApp) {
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

    const char *argv[2] = {title_id, 0};
    int ret = sceSystemServiceLaunchApp(title_id, argv, &param);
    oops_kprintf("SYSTEM", "sceSystemServiceLaunchApp(%s): ret = 0x%08x\n",
                 title_id, ret);
    return ret;
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

void oops_log(const char *fmt, ...) {
  if (fmt == NULL) return;
  char buf[512];
  va_list args;
  va_start(args, fmt);
  int len = oops_vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (len < 0) return;
  oops_klog(NULL, buf);
}

void oops_klog(const char *tag, const char *msg) {
  if (msg == NULL) return;
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

  for (size_t i = 0; msg[i] != '\0' && pos < sizeof(buf) - 2; i++) {
    buf[pos++] = msg[i];
  }
  buf[pos++] = '\n';
  buf[pos] = '\0';

#ifndef OOPS_HOST_BUILD
  (void)sys_call(SYS_klog, 7, (long)buf, 0, 0, 0, 0);
  (void)sys_call(SYS_write, 1, (long)buf, (long)pos, 0, 0, 0);
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
  int len = oops_vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (len < 0) return;
  oops_klog(tag, buf);
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

  /* Build sockaddr_in for 127.0.0.1:9069 (FreeBSD / PS5 ABI) */
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
