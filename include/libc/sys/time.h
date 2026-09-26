/*
 * <sys/time.h> - time types and gettimeofday.
 */
#ifndef OOPS_LIBC_SYS_TIME_H
#define OOPS_LIBC_SYS_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Angle brackets, not quotes: this file's own basename is time.h, so a quoted "time.h" resolves to
 * this header (a guarded no-op) instead of the top-level <time.h>, leaving time_t undefined for a
 * consumer that reaches <sys/time.h> first (e.g. QuickJS). */
#include <time.h>

/* **Guarded, because `oops-apps/common/posix` ships a `<sys/time.h>` too** and the two have
 * different include guards, so a compile that reaches both would define this twice. This one is
 * meant to win - it sits at include position 24 against the shim's 37 - but Neverball reached the
 * shim's `<sys/types.h>` rather than the SDK's, so "meant to" is not something to rely on. The
 * `_*_DECLARED` convention is the same one `<sys/types.h>` uses for its typedefs. */
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
 * **Setting a file's times, and it always fails** - defined in
 * `oops-apps/common/posix/posix.c`, which is where the reasoning lives. The SDK's filesystem
 * cannot set them, and `sys/stat.h` in that same layer reports all three as zero for the same
 * reason.
 *
 * Declared *here* rather than only in the shim's `<sys/time.h>` because this is the copy a
 * compile reaches: libc++'s `src/filesystem/time_utils.h:32` includes `<sys/time.h>` for
 * `::utimes`, and with the declaration in the shadowed copy three of its filesystem sources do
 * not compile. The same trap `fcntl.h` records for the `F_*` commands.
 */
int utimes(const char *path, const struct timeval times[2]);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_SYS_TIME_H */
