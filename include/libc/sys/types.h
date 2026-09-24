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

#endif /* _SYS_TYPES_H */
