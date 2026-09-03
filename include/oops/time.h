#ifndef OOPS_TIME_H
#define OOPS_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t oops_time_get_ticks(void);
uint64_t oops_time_get_frequency(void);
uint64_t oops_time_get_us(void);
uint64_t oops_time_get_ms(void);
void oops_time_sleep_us(uint32_t microseconds);
void oops_time_sleep_ms(uint32_t milliseconds);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_TIME_H */
