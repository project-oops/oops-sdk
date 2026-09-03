#ifndef OOPS_SYSTEM_H
#define OOPS_SYSTEM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oops_system_info {
    int      generation;       /* 4 = Orbis / PS4, 5 = Prospero / PS5 */
    uint32_t firmware_raw;     /* e.g. 0x12400009 */
    char     firmware_str[16]; /* e.g. "12.40" */
    char     model_str[32];    /* e.g. "CFI-1116A" */
    size_t   total_ram_mb;
    size_t   total_vram_mb;
    int      soc_temp_c;
} oops_system_info_t;

int32_t oops_user_get_initial_user_id(void);
int oops_system_get_info(oops_system_info_t *out_info);
int oops_system_notify(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_SYSTEM_H */
