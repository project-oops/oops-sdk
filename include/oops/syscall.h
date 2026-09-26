/*
 * Freestanding syscalls: FreeBSD syscall numbers, and `sys_call`, which routes them
 * through libkernel's registered syscall trampoline because Prospero refuses direct
 * syscalls from other code (PPRBUG-22859).
 */

#ifndef OOPS_SYSCALL_H
#define OOPS_SYSCALL_H

#include "oops/freestd.h"
#include "oops/krw.h"

#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_wait4 7
#define SYS_unlink 10
#define SYS_chmod 15
#define SYS_getpid 20
#define SYS_ptrace 26
#define SYS_kill 37
#define SYS_ioctl 54
#define SYS_munmap 73
#define SYS_mprotect 74
#define SYS_mkdir 136
#define SYS_rmdir 137
#define SYS_socket                                                                     \
    97 /* hardware-confirmed: sys_call(97, AF_INET, SOCK_STREAM, IPPROTO_TCP)          \
          returns a descriptor (obSCEne, 12.40) */
#define SYS_connect 98
#define SYS_bind 104
#define SYS_listen 106
#define SYS_accept 30
#define SYS_setsockopt 105
#define SYS_getdents 272
/*
 * FreeBSD's numbers, like every entry in this table: 128 and 232 are `rename` and
 * `clock_gettime` in `sys/kern/syscalls.master`. `clock_gettime` gives the wall clock
 * a calendar needs; `sceKernelGetProcessTimeCounter` counts from process start.
 */
#define SYS_rename 128
#define SYS_clock_gettime 232
#define SYS_mmap 477
#define SYS_lseek 478
#define SYS_klog 601
#define SYS_dynlib_get_obj_member 649

#ifdef __cplusplus
extern "C" {
#endif

/* Locates libkernel's syscall trampoline, using the payload's arguments when given.
 * 0 on success. `sys_call` locates it on first use if this was not called. */
int sys_call_init(const payload_args_t *args);
/* Issues syscall `num` through the trampoline and returns its result. */
long sys_call(long num, long a1, long a2, long a3, long a4, long a5, long a6);
/* The errno of the last failed syscall. */
int sys_get_errno(void);
/* Accepted and ignored: every syscall goes through the trampoline. */
void sys_enable_direct(int enable);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_SYSCALL_H */
