/*
 * Kernel Read/Write Interface Contract.
 *
 * Defines the contract for consuming kernel R/W primitives established by the
 * session (kstuff-lite / elfldr / webkit chain), pinned against
 * ps5-payload-dev-sdk.
 */

#ifndef OOPS_KRW_H
#define OOPS_KRW_H

#include "oops/freestd.h"
#include <stddef.h>
#include <stdint.h>

#if (defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 1) ||                      \
    defined(OOPS_HOST_BUILD) || defined(OBSCENE_HOST_BUILD)
#include <sys/types.h>
#else
#ifndef _PID_T_DECLARED
typedef int32_t pid_t;
#define _PID_T_DECLARED
#endif
#endif

#define OBS_KEXPORT_MAX 16384

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  char nid[12];
  uint32_t handle;
  uint64_t vaddr;
} obs_kexport_entry_t;

typedef struct {
  uint32_t count;
  uint32_t capacity;
  obs_kexport_entry_t entries[OBS_KEXPORT_MAX];
} obs_kexport_table_t;

/**
 * Payload entry arguments provided by kstuff-lite / elfldr / obscene-injector.
 * Pinned against ps5-payload-dev-sdk/crt/payload.h, with kexport_table
 * extension.
 */
typedef struct payload_args {
  int (*sys_dynlib_dlsym)(int, const char *, void *);
  int *rwpipe;
  int *rwpair;
  long kpipe_addr;
  long kdata_base_addr;
  int *payloadout;
  void *kexport_table;
} payload_args_t;

int krw_dump_all_exports(pid_t pid, obs_kexport_table_t *table);
const void *obs_kexport_lookup(const obs_kexport_table_t *table,
                               const char *nid);

/**
 * Initialize kernel R/W from payload arguments.
 * Resolves kernel data base, text base, and critical offsets (allproc, vmspace,
 * ucred). Returns 0 on success, or a negative error code.
 */
int krw_init(const payload_args_t *args);

/**
 * Check if kernel R/W is initialized and available.
 */
int krw_is_ready(void);

/**
 * Kernel memory addresses and metadata.
 */
uintptr_t krw_kdata_base(void);
uintptr_t krw_ktext_base(void);
uintptr_t krw_allproc_addr(void);
void krw_set_allproc_addr(uintptr_t addr);
uint32_t krw_fw_version(void);

/**
 * Copy data between user and kernel address space.
 */
int krw_copyin(const void *uaddr, uintptr_t kaddr, size_t len);
int krw_copyout(uintptr_t kaddr, void *uaddr, size_t len);

/**
 * Primitive scalar memory access.
 */
uint64_t krw_read64(uintptr_t kaddr);
uint32_t krw_read32(uintptr_t kaddr);
uint16_t krw_read16(uintptr_t kaddr);
uint8_t krw_read8(uintptr_t kaddr);

int krw_write64(uintptr_t kaddr, uint64_t val);
int krw_write32(uintptr_t kaddr, uint32_t val);
int krw_write16(uintptr_t kaddr, uint16_t val);
int krw_write8(uintptr_t kaddr, uint8_t val);

/**
 * Process and credentials inspection/modification.
 */
uintptr_t krw_get_proc(pid_t pid);
uintptr_t krw_find_proc_by_name(const char *name);
uintptr_t krw_get_ucred(pid_t pid);

uint64_t krw_get_ucred_authid(pid_t pid);
int krw_set_ucred_authid(pid_t pid, uint64_t authid);

int krw_get_ucred_caps(pid_t pid, uint8_t caps[16]);
int krw_set_ucred_caps(pid_t pid, const uint8_t caps[16]);

int krw_get_ucred_attrs(pid_t pid, uint8_t attrs[32]);
int krw_set_ucred_attrs(pid_t pid, const uint8_t attrs[32]);

uintptr_t krw_get_root_vnode(void);
uintptr_t krw_get_proc_cdir(pid_t pid);
int krw_set_proc_cdir(pid_t pid, uintptr_t vnode);
uintptr_t krw_get_proc_rootdir(pid_t pid);
int krw_set_proc_rootdir(pid_t pid, uintptr_t vnode);
uintptr_t krw_get_proc_jaildir(pid_t pid);
int krw_set_proc_jaildir(pid_t pid, uintptr_t vnode);

/**
 * Privilege elevation helpers for ptrace and syscall bypass.
 */
int krw_elevate_current_process(void);
int krw_elevate_process(pid_t pid);
int krw_restore_current_process(void);
int krw_swap_ucred(pid_t target_pid);
int krw_restore_ucred(void);
int krw_apply_ptrace_kernel_patch(void);

/**
 * Directly adjust page protection in a target process address space.
 */
int krw_mprotect(pid_t pid, uintptr_t addr, size_t len, int prot);
uintptr_t krw_find_target_libkernel_base(uintptr_t target_kproc,
                                         uintptr_t rip_hint);
uintptr_t krw_dynlib_resolve(pid_t pid, int sprx_handle, const char *nid);
uintptr_t krw_dynlib_resolve_any(pid_t pid, const char *sname);

void klog_write(const char *msg);
void klog_write_hex(const char *prefix, uint64_t val);
void klog_write_num(const char *prefix, int64_t num);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_KRW_H */
