/*
 * <sys/types.h> - standard system types (REQ-20260923T1810Z-7d42).
 *
 * Provides basic POSIX and BSD system type definitions for freestanding targets.
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
