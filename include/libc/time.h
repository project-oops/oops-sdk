/*
 * <time.h> - the two functions a port calls, and an honest account of what they are.
 *
 * **`time()` is a wall clock now** (2026-09-22), and the paragraph that used to stand here was
 * wrong in a way worth recording.
 *
 * It said there was no calendar this SDK could read, so `time()` answered seconds from the
 * payload's own start, and concluded that `localtime`, `gmtime`, `mktime` and `strftime` should
 * therefore not exist at all - that a program needing them should fail to link rather than print
 * 1970. That reads like a decision and was not one. `time()` was built on
 * `sceKernelGetProcessTimeCounter` because that is the clock that was already wired for frame
 * pacing, and the absence of a calendar was then explained rather than investigated.
 *
 * The kernel here is FreeBSD-derived and answers `clock_gettime(CLOCK_REALTIME)` through the
 * same syscall table `SYS_open` and `SYS_mkdir` already come from. `oops_time_get_epoch_seconds`
 * asks it, and `time()` returns that. Neverball wanted it for an ordinary reason - it stamps
 * replays with the date they were recorded.
 *
 * **What is still true**: if the platform does not answer, `time()` returns 0 and the conversions
 * below will render 1970. The clock reports its own failure as 0 (see `<oops/time.h>`), which a
 * caller can check; there is no value a real date cannot take, so it cannot be signalled any
 * other way.
 *
 * `clock()` is the same clock in `CLOCKS_PER_SEC` units - elapsed, not CPU time, because there
 * is one program running and the difference is not worth pretending to.
 *
 * On the target include path only - see <libc/math.h>.
 */
#ifndef OOPS_LIBC_TIME_H
#define OOPS_LIBC_TIME_H

/* C linkage for a C++ includer; `stdlib.h` carries the reasoning. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef int64_t time_t;
typedef int64_t clock_t;

#define CLOCKS_PER_SEC 1000000

/* Seconds from the payload's start. See above: not since the epoch. */
time_t time(time_t *out);
clock_t clock(void);

/*
 * The calendar. `struct tm` is C's, field for field and in C's order.
 *
 * **Everything here is UTC, including `localtime`.** A local time needs a timezone, and a
 * timezone needs a database and a setting this console does not hand a payload. `localtime`
 * returning UTC is an hour or two wrong; inventing an offset would be wrong by a made-up number,
 * and reporting failure would break every caller that only wants a date on a file. The
 * divergence is named here so nobody has to discover it from a screenshot.
 *
 * `gmtime` and `localtime` return a pointer to one static `struct tm`, which is what C says and
 * what makes them unsafe to call from two threads at once. `gmtime_r` takes the buffer instead
 * and is the one to reach for in a thread.
 */
struct tm {
  int tm_sec;   /* 0-60, 60 for a leap second */
  int tm_min;   /* 0-59 */
  int tm_hour;  /* 0-23 */
  int tm_mday;  /* 1-31 */
  int tm_mon;   /* 0-11, January is 0 */
  int tm_year;  /* years since 1900 */
  int tm_wday;  /* 0-6, Sunday is 0 */
  int tm_yday;  /* 0-365 */
  int tm_isdst; /* always 0 here: no timezone, so no daylight saving */
};

struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t mktime(struct tm *tm);
size_t strftime(char *buf, size_t max, const char *format, const struct tm *tm);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_TIME_H */
