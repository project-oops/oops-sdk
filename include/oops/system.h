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

/* Log Levels */
typedef enum oops_log_level {
  OOPS_LOG_NONE = 0,
  OOPS_LOG_ERROR = 1,
  OOPS_LOG_WARN = 2,
  OOPS_LOG_INFO = 3,
  OOPS_LOG_DEBUG = 4,
  OOPS_LOG_TRACE = 5
} oops_log_level_t;

/* Kernel Log & Telemetry Output (/dev/klog on target, stderr on host) */
void oops_log_init(const char *app_id);
const char *oops_log_get_app_id(void);
void oops_log_set_level(oops_log_level_t level);
oops_log_level_t oops_log_get_level(void);

void oops_log(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void oops_klog(const char *tag, const char *msg);
void oops_kprintf(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void oops_klog_level(oops_log_level_t level, const char *tag, const char *msg);
void oops_kprintf_level(oops_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define oops_log_error(tag, ...) oops_kprintf_level(OOPS_LOG_ERROR, tag, __VA_ARGS__)
#define oops_log_warn(tag, ...)  oops_kprintf_level(OOPS_LOG_WARN,  tag, __VA_ARGS__)
#define oops_log_info(tag, ...)  oops_kprintf_level(OOPS_LOG_INFO,  tag, __VA_ARGS__)
#define oops_log_debug(tag, ...) oops_kprintf_level(OOPS_LOG_DEBUG, tag, __VA_ARGS__)
#define oops_log_trace(tag, ...) oops_kprintf_level(OOPS_LOG_TRACE, tag, __VA_ARGS__)

const char *oops_test_get_last_klog(void);

/* Runtime dynamic linker symbol resolution probe (prevents 0xa0020101 PLT traps) */
int oops_symbol_is_resolved(const void *fn_ptr);

/* System Service Controls (libSceSystemService) */
int oops_system_hide_splash(void);
int oops_system_power_tick(void);
int oops_system_navigate_home(void);
int oops_system_get_enter_button(int *out_button); /* 0 = Circle, 1 = Cross */
int oops_system_launch_app(const char *title_id);
int oops_system_get_running_app_title_id(char *out_title_id, size_t max_len);
int oops_system_is_app_suspended(int *out_is_suspended);
int oops_system_kill_app(int app_id);

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

struct payload_args;
/* System namespace & root filesystem binding (resolves kernel root vnode) */
int oops_system_init_namespace(const struct payload_args *args);

/**
 * Explicitly requests filesystem namespace elevation to access global /user and /data.
 *
 * Performs an on-demand event-driven handshake with the resident sandbox-daemon via
 * loopback TCP at 127.0.0.1:9069. Sends the calling process's 4-byte PID and receives
 * an int32_t status code (0 = success). Operates with zero background polling overhead.
 *
 * Returns: 0 on success, -1 on timeout or if daemon is unavailable.
 */
int oops_system_escape_sandbox(void);

/**
 * Finish, on a platform where finishing is not permitted. Never returns.
 *
 * # Why a title cannot simply return or exit
 *
 * Measured, retail firmware 12.40, obSCEne `REQ-20260917T1450Z-2e71` (check
 * `017-posix/process-exit-candidates`, sweep `20260917-160206`):
 *
 *   - `exit`, `_Exit`, `sceKernelExit`, `sceKernelExitProcess`,
 *     `sceSystemServiceKillLocalProcess` and `sceShellCoreUtilExitApp` are all **absent**.
 *   - `_exit` is **present** (`libkernel`, `0x8000003f0`) and raises `SIGSYS`: it reaches
 *     FreeBSD's `SYS_exit`, syscall 1, which a `big-app` container's credentials do not permit.
 *     The kernel logs `eboot.bin calls exit()` and then kills the process.
 *   - returning from the entry point faults at `rip: 0x0`, because the dynamic linker transfers
 *     control with no caller return frame - `%rbp` zero, `%rsp` holding argc.
 *
 * Process lifecycle belongs to `SceShellCore`. There is no userland call that ends a process
 * cleanly, so the conforming pattern is the one obSCEne's own sweeps have always used: emit the
 * final line, then idle, and let the host close the app (`pros close <ID>`, or
 * `obscene-tool hw close-app <ID>`). That produces no coredump, no crash report and no hung GPU
 * ring, where returning or calling `_exit` produces all three.
 *
 * # What this means for a caller
 *
 * **Say everything you have to say before calling this.** A harness watches the log for a
 * completion sentinel, so the last line printed is the result - an exit code is not available to
 * report one, and anything buffered and unflushed is lost.
 *
 * `sceSystemServiceNavigateToGoHome` is the alternative the same resolution names: return the
 * display to the home screen and idle until killed. It is deliberately not done here, because a
 * probe's caller usually wants the rendered output left on screen to be sampled.
 *
 * Self-contained on purpose: it binds the vendor sleep itself rather than calling
 * `oops_time_sleep_ms`, so that a title linking `system.c` without `time.c` does not acquire an
 * unresolved symbol. Four of them do exactly that today, and an app link ignores unresolved
 * symbols rather than failing, so the cost would have been a trap on first use.
 */
__attribute__((noreturn)) void oops_system_park_until_closed(void);

/*
 * Cooperate with the dashboard's "Close Application".
 *
 * The system does not force a big-app down without warning: it sends the process a signal first
 * (SIGTERM, with SIGINT/SIGHUP as siblings), and only kills it if it does not quiesce. A title
 * that ignores the signal keeps submitting GPU work and is killed mid-frame - which surfaces as a
 * crash. `seashell` closes cleanly because it catches the signal and stops; this is that pattern,
 * shared.
 *
 * Call `oops_system_install_close_handler()` once at start-up, then check
 * `oops_system_close_requested()` each frame and leave the render loop when it returns non-zero -
 * stop drawing, tear the GL objects down, and either return (a freestanding title with a caller
 * frame) or park (a hosted title that cannot). The kill then lands on a quiesced process.
 *
 * The handler needs the platform's `sigaction`; where it is unavailable the install is a no-op and
 * the request flag simply never sets, so a caller's loop behaves exactly as it did before.
 */
void oops_system_install_close_handler(void);
int  oops_system_close_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_SYSTEM_H */
