#ifndef OOPS_ESCALATE_H
#define OOPS_ESCALATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SYSTEM_AUTHID value (root system authority identifier) */
#define OOPS_SYSTEM_AUTHID 0x3000000000000001ULL

/**
 * Initialize kernel read/write capabilities if available in current payload
 * context. Returns: 0 on success, -1 on failure.
 */
int oops_kernel_rw_init(void);

/**
 * Initialize kernel arbitrary read/write using pipe primitives (from WebKit
 * exploit arguments).
 *
 * rwpipe: file descriptors [read_fd, write_fd]
 * rwpair: file descriptors [read_fd, write_fd] for address manipulation
 * kpipe_addr: kernel address of the pipe buffer
 * kdata_base: kernel .data base address (or 0 to lookup from offsets table)
 */
int oops_kernel_pipe_init(int rwpipe[2], int rwpair[2], uint64_t kpipe_addr,
                          uint64_t kdata_base);

/**
 * Read memory from the kernel address space.
 * Returns: 0 on success, -1 on failure.
 */
int oops_kernel_copyout(uint64_t kaddr, void *buf, size_t size);

/**
 * Write memory to the kernel address space.
 * Returns: 0 on success, -1 on failure.
 */
int oops_kernel_copyin(const void *buf, uint64_t kaddr, size_t size);

/**
 * Walk the kernel allproc list to find the process struct matching pid.
 * Returns: Kernel address of target proc, or 0 if not found / unsupported.
 */
uint64_t oops_kernel_find_proc_by_pid(int pid);

/**
 * Get the kernel address of the current process struct.
 * Returns: Kernel address of current proc, or 0 on failure.
 */
uint64_t oops_kernel_get_current_proc(void);

/**
 * Escalate current process credentials to SYSTEM_AUTHID.
 * Returns: 0 on success, -1 on failure or unsupported environment.
 */
int oops_escalate_to_system_authid(void);

/**
 * Check if the current process holds SYSTEM_AUTHID credentials.
 * Returns: 1 if holder of SYSTEM_AUTHID, 0 if not, -1 on error/unsupported.
 */
int oops_has_system_authid(void);

/**
 * Applies the complete 11-write credential and filesystem escalation to the
 * given PID: 1-5: cr_uid, cr_ruid, cr_svuid, cr_ngroups, cr_rgid zeroed 6:
 * cr_sceAuthID set to SYSTEM_AUTHID 7-8: cr_sceCaps set to 0xFFFFFFFFFFFFFFFF
 *   9:   cr_sceAttr0 set to 0x80
 *   10:  fd_rdir set to kernel rootvnode (sandbox jailbreak)
 *   11:  fd_jdir set to kernel rootvnode (sandbox jailbreak)
 *
 * If pid is -1 or 0, targets the current calling process.
 * Returns: 0 on success, -1 on failure or unsupported environment.
 */
int oops_jailbreak_process(int pid);

/**
 * Escapes the filesystem sandbox jail for the current process by pointing
 * fd_rdir and fd_jdir to the kernel rootvnode.
 * Returns: 0 on success, -1 on failure.
 */
int oops_escape_jail(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_ESCALATE_H */
