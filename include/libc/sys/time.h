/*
 * <sys/time.h> - time types and gettimeofday.
 */
#ifndef OOPS_LIBC_SYS_TIME_H
#define OOPS_LIBC_SYS_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Angle brackets, not quotes: this file's own basename is time.h, so a quoted "time.h"
 * resolves to this header (a guarded no-op) instead of the top-level <time.h>, leaving
 * time_t undefined for a consumer that reaches <sys/time.h> first (e.g. QuickJS). */
#include <time.h>

/* Guarded, because `oops-apps/common/posix` ships a `<sys/time.h>` too with a different
 * include guard, and a compile can reach both despite this one being earlier on the
 * include path. The `_*_DECLARED` convention is the one `<sys/types.h>` uses. */
#ifndef _TIMEVAL_DECLARED
#define _TIMEVAL_DECLARED
struct timeval {
    time_t tv_sec;
    long tv_usec;
};
#endif

#ifndef _TIMEZONE_DECLARED
#define _TIMEZONE_DECLARED
struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
#endif

int gettimeofday(struct timeval *tv, void *tz);

/*
 * Setting a file's times always fails: the SDK's filesystem cannot set them (defined
 * in `oops-apps/common/posix/posix.c`, whose `sys/stat.h` reports all three as zero).
 *
 * Declared here rather than only in the shim's `<sys/time.h>` because this is the copy
 * a compile reaches: libc++'s `src/filesystem/time_utils.h:32` includes `<sys/time.h>`
 * for `::utimes`. `fcntl.h` has the same arrangement for the `F_*` commands.
 */
int utimes(const char *path, const struct timeval times[2]);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_SYS_TIME_H */
