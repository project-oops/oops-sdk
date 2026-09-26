/*
 * Time: a process clock for frame pacing and measurement, the wall clock, and sleeps.
 */
#ifndef OOPS_TIME_H
#define OOPS_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the counter frequencies the process clocks are built on; idempotent. */
void oops_time_init(void);
/*
 * Wall-clock time: seconds since the Unix epoch, from the kernel's
 * `clock_gettime(CLOCK_REALTIME)`. Everything else in this header is a process clock
 * (`sceKernelGetProcessTimeCounter`, counting from process start), right for frame
 * pacing and wrong for a date.
 *
 * Zero means the platform did not answer; callers treat it as unknown, not as 1970.
 */
uint64_t oops_time_get_epoch_seconds(void);

/* The process clocks: raw tick and counter values with their frequencies, and time
 * since process start in nanoseconds, microseconds, milliseconds and seconds. */
uint64_t oops_time_get_ticks(void);
uint64_t oops_time_get_counter(void);
uint64_t oops_time_get_frequency(void);
uint64_t oops_time_get_counter_frequency(void);
uint64_t oops_time_get_ns(void);
uint64_t oops_time_get_us(void);
uint64_t oops_time_get_ms(void);
double oops_time_get_seconds(void);
/* Sleeps the calling thread. */
void oops_time_sleep_us(uint32_t microseconds);
void oops_time_sleep_ms(uint32_t milliseconds);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_TIME_H */
