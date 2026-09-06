#ifndef OOPS_SYSTEM_H
#define OOPS_SYSTEM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oops_system_info {
    int      generation;       /* 4 = Orbis-generation, 5 = Prospero-generation */
    uint32_t firmware_raw;     /* e.g. 0x12400009 */
    char     firmware_str[16]; /* e.g. "12.40" */
    char     model_str[32];    /* e.g. "CFI-1116A" */
    size_t   total_ram_mb;
    size_t   direct_mem_mb;
    int32_t  initial_user_id;
    char     user_name[32];
} oops_system_info_t;

typedef struct oops_hw_info {
    int      cpu_temp_c;          /* CPU temp in Celsius (-1 if unavailable) */
    int      soc_temp_c;          /* SoC primary sensor temp in Celsius (-1 if unavailable) */
    int      fan_duty_pct;        /* Current fan duty 0-100% (-1 if unavailable) */
    uint64_t cpu_freq_hz;         /* CPU clock in Hz (0 if unavailable) */
    char     serial_number[64];   /* Console hardware serial number */
    char     model_name[64];      /* Console hardware model name */
} oops_hw_info_t;

int32_t oops_user_get_initial_user_id(void);
int oops_user_get_name(int32_t user_id, char *out_name, size_t max_len);
int oops_user_get_logged_in_users(int32_t *out_user_ids, size_t max_users, size_t *out_count);
int oops_system_get_info(oops_system_info_t *out_info);
int oops_system_notify(const char *text);

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

#ifdef __cplusplus
}
#endif

#endif /* OOPS_SYSTEM_H */
