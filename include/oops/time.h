#ifndef OOPS_TIME_H
#define OOPS_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void oops_time_init(void);
/*
 * **Wall-clock time: seconds since the Unix epoch** (2026-09-22), and zero when the
 * platform will not say.
 *
 * Everything else in this header is a *process* clock - `oops_time_get_ns` sits on
 * `sceKernelGetProcessTimeCounter`, which counts from process start. That is the right
 * answer for frame pacing and for measuring how long something took, and it is the
 * wrong answer for a date. `<libc/time.h>` used to conclude from that that a payload
 * simply could not know the date, and say so firmly; it was never measured, only
 * inherited from which clock happened to be wired first.
 *
 * This asks the kernel instead, through `clock_gettime(CLOCK_REALTIME)` - the same
 * FreeBSD syscall table every other call in `<oops/syscall.h>` uses. Titles want it for
 * ordinary reasons: Neverball stamps replays with the date they were recorded.
 *
 * **Zero means "the platform did not answer", and callers must treat it as unknown**
 * rather than as 1970. That is the one honest failure available: there is no sentinel a
 * date cannot be.
 */
uint64_t oops_time_get_epoch_seconds(void);

uint64_t oops_time_get_ticks(void);
uint64_t oops_time_get_counter(void);
uint64_t oops_time_get_frequency(void);
uint64_t oops_time_get_counter_frequency(void);
uint64_t oops_time_get_ns(void);
uint64_t oops_time_get_us(void);
uint64_t oops_time_get_ms(void);
double oops_time_get_seconds(void);
void oops_time_sleep_us(uint32_t microseconds);
void oops_time_sleep_ms(uint32_t milliseconds);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_TIME_H */
