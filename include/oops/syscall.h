/*
 * Freestanding Syscall Trampoline Interface.
 *
 * Routes syscalls through libkernel's registered syscall trampoline to satisfy
 * Prospero's direct-syscall mitigation (PPRBUG-22859).
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
#define SYS_getpid 20
#define SYS_ptrace 26
#define SYS_kill 37
#define SYS_ioctl 54
#define SYS_munmap 73
#define SYS_mprotect 74
#define SYS_mkdir 136
#define SYS_socket                                                             \
  97 /* hardware-confirmed: sys_call(97, AF_INET, SOCK_STREAM, IPPROTO_TCP)    \
        returns a descriptor (obSCEne, 12.40) */
#define SYS_connect 98
#define SYS_bind 104
#define SYS_listen 106
#define SYS_accept 30
#define SYS_setsockopt 105
#define SYS_getdents 272
#define SYS_mmap 477
#define SYS_lseek 478
#define SYS_klog 601
#define SYS_dynlib_get_obj_member 649

#ifdef __cplusplus
extern "C" {
#endif

int sys_call_init(const payload_args_t *args);
long sys_call(long num, long a1, long a2, long a3, long a4, long a5, long a6);
int sys_get_errno(void);
void sys_enable_direct(int enable);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_SYSCALL_H */
