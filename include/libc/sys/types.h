/*
 * <sys/types.h> - standard system types (REQ-20260923T1810Z-7d42).
 *
 * Provides basic POSIX and BSD system type definitions for freestanding targets.
 *
 * # Under `include/libc/`, and that is not a tidying choice
 *
 * It lived at `include/sys/types.h` for a few hours on 2026-09-23 and **broke every hosted
 * title** - `mesa-cube`, `mesa-demos`, `dri-probe`, `gl-cts`. A hosted title compiles with
 * `--sysroot=<oops-mesa>/toolchain/sysroot` *and* `-I<oops-sdk>/include`, and the `-I` is
 * searched first, so `#include <sys/types.h>` reached this file instead of FreeBSD's. FreeBSD's
 * `sys/cpuset.h` then declared `cpuset(cpusetid_t *)` with no `cpusetid_t` in scope, and
 * oops-mesa's `src/runtime/threads.c` stopped with ten errors inside somebody else's header.
 * Measured: eleven errors with that `-I` present, zero without.
 *
 * `include/libc/` is the directory that means *freestanding C library, not for a hosted title*:
 * `common/app.mk` empties `OOPS_SDK_LIBC_INCLUDE` when `USE_MESA` is set, for exactly this
 * reason - its own comment cites `__clock_t` being `int` in the sysroot and `int64_t` here. So
 * a C library header belongs under it, and one left in `include/` is on the path for titles that
 * must not see it.
 *
 * **Anything else that is a C library header goes here too.** The rule is not "types go in
 * `sys/`", it is "what a hosted title must get from the sysroot instead of from us".
 */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifndef _PID_T_DECLARED
typedef int32_t pid_t;
#define _PID_T_DECLARED
#endif

#ifndef _OFF_T_DECLARED
typedef int64_t off_t;
#define _OFF_T_DECLARED
#endif

#ifndef _SSIZE_T_DECLARED
typedef int64_t ssize_t;
#define _SSIZE_T_DECLARED
#endif

#ifndef _UID_T_DECLARED
typedef uint32_t uid_t;
#define _UID_T_DECLARED
#endif

#ifndef _GID_T_DECLARED
typedef uint32_t gid_t;
#define _GID_T_DECLARED
#endif

#ifndef _MODE_T_DECLARED
typedef uint32_t mode_t;
#define _MODE_T_DECLARED
#endif

#ifndef _TIME_T_DECLARED
typedef int64_t time_t;
#define _TIME_T_DECLARED
#endif

#ifndef _SUSECONDS_T_DECLARED
typedef int64_t suseconds_t;
#define _SUSECONDS_T_DECLARED
#endif

/* `usleep`'s argument, and not the unsigned twin of `suseconds_t` above despite the names: that
 * one is a signed *difference* inside `struct timeval` and is 64-bit here, while this is a count
 * of microseconds to wait and is 32-bit on FreeBSD and on Linux both. A port that assumed they
 * matched would silently truncate a long sleep on one of them. */
#ifndef _USECONDS_T_DECLARED
typedef unsigned int useconds_t;
#define _USECONDS_T_DECLARED
#endif

/*
 * **FreeBSD's `<sys/types.h>` pulls in `<sys/select.h>`, and so does this one when there is one to
 * pull in.**
 *
 * `fd_set` is reached through this header far more often than through `<sys/select.h>` directly -
 * ioquake3's `net_ip.c` and `sys_unix.c` both declare one having included only `<sys/types.h>`,
 * which is correct on every BSD and on Linux. A port that compiles everywhere else and fails here
 * on an undeclared `fd_set` is this omission, not the port.
 *
 * `__has_include` rather than an unconditional include, because `fd_set` and `select` are POSIX
 * rather than C: they belong to a port layer - `oops-apps/common/posix` provides them - and this
 * file is the freestanding C library, which titles without that layer include on its own. So the
 * include appears exactly when something can satisfy it.
 */
#if defined(__has_include)
#if __has_include(<sys/select.h>)
#include <sys/select.h>
#endif
#endif

#endif /* _SYS_TYPES_H */
