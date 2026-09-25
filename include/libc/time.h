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

/* C's fixed rendering, `Www Mmm dd hh:mm:ss yyyy\n` - twenty-six bytes including the terminator,
 * which is the size `buf` must have. The plain forms return one static buffer each, as C says
 * they do; prefer the `_r` forms, which are this SDK's and have no such corner.
 *
 * These arrive because Extreme Tux Racer stamps a saved course with `asctime (localtime (...))`
 * and libc++'s `<ctime>` resolved `using ::asctime` to nothing. `strftime` was already here and
 * cannot stand in for them portably: the day field is space-padded, which is `%e`, an extension
 * rather than one of the conversions that function carries. */
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);
char *ctime(const time_t *t);
char *ctime_r(const time_t *t, char *buf);
double difftime(time_t end, time_t start);

/*
 * `clock_gettime` and the two clocks worth having.
 *
 * Added for C++: enabling threads in libc++ obliges `_LIBCPP_HAS_MONOTONIC_CLOCK`, and
 * `std::chrono::steady_clock::now()` lands here. Nothing in this header could answer it before -
 * `struct timespec` did not exist anywhere in the SDK or the apps shim, which is why it is
 * declared here rather than assumed.
 *
 * **Declared here and implemented in `oops-apps/common/posix/posix.c`**, the same split
 * `gettimeofday` already has: the shim is what every port linking a C library against this
 * platform pulls in, and putting a second definition in the SDK would collide with it.
 *
 * `CLOCK_MONOTONIC` is the platform's own counter and is exactly what it claims to be.
 * `CLOCK_REALTIME` carries the wall-clock second with the counter's nanoseconds on top, which is
 * right to a second and is what a caller measuring intervals wants; a caller wanting a precise
 * date wants `time()`. The values are FreeBSD's, because that is the kernel underneath.
 */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 4

#ifndef OOPS_HAVE_STRUCT_TIMESPEC
#define OOPS_HAVE_STRUCT_TIMESPEC 1
struct timespec {
  time_t tv_sec;
  long tv_nsec;
};
#endif

int clock_gettime(int clk_id, struct timespec *ts);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_TIME_H */
