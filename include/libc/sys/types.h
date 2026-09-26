/*
 * <sys/types.h> - the POSIX and BSD system types, for freestanding titles.
 *
 * It lives under `include/libc/`, the freestanding C library that a hosted title must
 * not see: a hosted title compiles with `--sysroot=<oops-mesa>/toolchain/sysroot` and
 * `-I<oops-sdk>/include`, and the `-I` is searched first, so a copy in `include/` would
 * shadow FreeBSD's and break its `sys/cpuset.h`. `common/app.mk` empties
 * `OOPS_SDK_LIBC_INCLUDE` when `USE_MESA` is set for this reason. Every C library
 * header belongs here, not in `include/`.
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

/* A file's identity, 64-bit as FreeBSD sizes it. Nothing on this platform produces
 * these (`oops-apps/common/posix`'s `struct stat` leaves them zero), but a port has to
 * be able to declare them, and at the kernel's width so nothing truncates. */
#ifndef _DEV_T_DECLARED
typedef uint64_t dev_t;
#define _DEV_T_DECLARED
#endif

#ifndef _INO_T_DECLARED
typedef uint64_t ino_t;
#define _INO_T_DECLARED
#endif

#ifndef _NLINK_T_DECLARED
typedef uint64_t nlink_t;
#define _NLINK_T_DECLARED
#endif

#ifndef _TIME_T_DECLARED
typedef int64_t time_t;
#define _TIME_T_DECLARED
#endif

#ifndef _SUSECONDS_T_DECLARED
typedef int64_t suseconds_t;
#define _SUSECONDS_T_DECLARED
#endif

/* `usleep`'s argument, 32-bit on FreeBSD and Linux. Not the unsigned twin of
 * `suseconds_t`, which is a signed 64-bit difference inside `struct timeval`. */
#ifndef _USECONDS_T_DECLARED
typedef unsigned int useconds_t;
#define _USECONDS_T_DECLARED
#endif

/*
 * FreeBSD's `<sys/types.h>` pulls in `<sys/select.h>`, and ports rely on it for
 * `fd_set`; this one does too when one exists. `fd_set` and `select` are POSIX and
 * belong to a port layer (`oops-apps/common/posix` provides them), so the include is
 * conditional for titles without that layer.
 */
#if defined(__has_include)
#if __has_include(<sys/select.h>)
#include <sys/select.h>
#endif
#endif

#endif /* _SYS_TYPES_H */
