/*
 * <time.h> - the two functions a port calls, and an honest account of what they are.
 *
 * **`time()` is not a wall clock here.** There is no calendar this SDK can read without the
 * platform's own, so it answers seconds from an arbitrary origin - the payload's own start.
 * That is exactly right for the way ports use it, `srand(time(NULL))` and measuring how long
 * something took, and exactly wrong for formatting a date. A port that prints today's date with
 * this will print nonsense, so it should not: `localtime`, `gmtime`, `mktime` and `strftime` are
 * **not here**, and a program that needs them fails to link rather than printing 1970.
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

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_TIME_H */
