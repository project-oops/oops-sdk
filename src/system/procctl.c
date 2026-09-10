/*
 * Process Control Implementation.
 *
 * ptrace-based process control and remote execution for FreeBSD/Prospero
 * targets.
 */

#include "oops/freestd.h"
#include "oops/inject.h"
#include "oops/krw.h"
#include "oops/syscall.h"

#define SYS_wait4 7
#define SYS_ptrace 26
#define SYS_mmap 477
#define SYS_munmap 73
#define SYS_mprotect 74

#define PT_CONTINUE 7
#define PT_STEP 9
#define PT_ATTACH 10
#define PT_DETACH 11
#define PT_IO 12
#define PT_GETNUMLWPS 14
#define PT_GETLWPLIST 15
#define PT_GETREGS 33
#define PT_SETREGS 34

#define PIOD_READ_D 1
#define PIOD_WRITE_D 2
#define PIOD_READ_I 3
#define PIOD_WRITE_I 4

struct ptrace_io_desc {
  int piod_op;
  void *piod_offs;
  void *piod_addr;
  size_t piod_len;
};

static const uintptr_t KERNEL_OFFSET_PROC_P_PID = 0xBC;

static int sys_ptrace(int request, pid_t pid, void *addr, int data) {
  return (int)sys_call(SYS_ptrace, (long)request, (long)pid, (long)addr,
                       (long)data, 0, 0);
}

static pid_t sys_wait4(pid_t pid, int *status, int options, void *rusage) {
  return (pid_t)sys_call(SYS_wait4, (long)pid, (long)status, (long)options,
                         (long)rusage, 0, 0);
}

/* injector.h replaced by oops/krw.h */

int procctl_attach(pid_t pid) {
  if (krw_is_ready()) {
    uintptr_t kproc = krw_get_proc(pid);
    uint32_t p_flag = 0;
    uint32_t p_flag2 = 0;
    uint8_t p_state = 0;
    uintptr_t p_pptr = 0;
    pid_t p_oppid = 0;
    uint32_t p_ptevents = 0;

    krw_copyout(kproc + 0xB0, &p_flag, sizeof(p_flag));
    krw_copyout(kproc + 0xB4, &p_flag2, sizeof(p_flag2));
    krw_copyout(kproc + 0xB8, &p_state, sizeof(p_state));
    krw_copyout(kproc + 0xE0, &p_pptr, sizeof(p_pptr));
    krw_copyout(kproc + 0x1FC, &p_oppid, sizeof(p_oppid));
    krw_copyout(kproc + 0x24C, &p_ptevents, sizeof(p_ptevents));

    klog_write_hex("target kproc=", kproc);
    klog_write_hex("target p_flag=", p_flag);
    klog_write_hex("target p_flag2=", p_flag2);
    klog_write_hex("target p_pptr=", p_pptr);
    if (p_pptr != 0) {
      pid_t ppid = 0;
      krw_copyout(p_pptr + KERNEL_OFFSET_PROC_P_PID, &ppid, sizeof(ppid));
      klog_write_num("target parent pid=", (int64_t)ppid);
    }
    klog_write_num("target p_oppid=", (int64_t)p_oppid);
    klog_write_hex("target p_ptevents=", (uint64_t)p_ptevents);
    klog_write_num("target p_state=", (int64_t)p_state);
  }

  int ret = sys_ptrace(PT_ATTACH, pid, NULL, 0);
  int err = sys_get_errno();
  klog_write_num("PT_ATTACH ret=", (int64_t)ret);
  if (ret != 0) {
    klog_write_num("PT_ATTACH errno=", (int64_t)err);
    return -1;
  }

  int status = 0;
  pid_t wret = sys_wait4(pid, &status, 0, NULL);
  int werr = sys_get_errno();
  klog_write_num("wait4 ret=", (int64_t)wret);
  if (wret < 0) {
    klog_write_num("wait4 errno=", (int64_t)werr);
    return -1;
  }
  klog_write_num("status=", (int64_t)status);
  if (wret > 0) {
    klog_write_num("wait4 stop signal=", (int64_t)((status >> 8) & 0xff));
  }
  return 0;
}

int procctl_detach(pid_t pid, int sig) {
  /* FreeBSD ptrace(2): PT_DETACH addr argument must be (void *)1 */
  int ret = sys_ptrace(PT_DETACH, pid, (void *)1, sig);
  if (ret != 0) {
    ret = sys_ptrace(PT_DETACH, pid, NULL, sig);
  }
  int err = sys_get_errno();
  klog_write_num("PT_DETACH ret=", (int64_t)ret);
  if (ret != 0) {
    klog_write_num("PT_DETACH errno=", (int64_t)err);
  }
  return ret;
}

static int s_target_lwp = 0;

static int get_target_lwp(pid_t pid) {
  if (s_target_lwp > 0) {
    return s_target_lwp;
  }
  int numlwps = sys_ptrace(PT_GETNUMLWPS, pid, NULL, 0);
  if (numlwps > 0) {
    int lwps[32];
    if (numlwps > 32)
      numlwps = 32;
    if (sys_ptrace(PT_GETLWPLIST, pid, (void *)lwps, numlwps) > 0) {
      klog_write_num("resolved target LWP=", (int64_t)lwps[0]);
      s_target_lwp = lwps[0];
      return lwps[0];
    }
  }
  return (int)pid;
}

int procctl_step(pid_t pid) {
  int step_id = get_target_lwp(pid);
  int ret = sys_ptrace(PT_STEP, step_id, (void *)1, 0);
  if (ret != 0) {
    ret = sys_ptrace(PT_STEP, pid, (void *)1, 0);
  }
  if (ret != 0) {
    klog_write_num("procctl_step failed, errno=", (int64_t)sys_get_errno());
    return -1;
  }
  int status = 0;
  if (sys_wait4(pid, &status, 0, NULL) < 0) {
    klog_write_num("procctl_step wait4 failed, errno=",
                   (int64_t)sys_get_errno());
    return -1;
  }
  return 0;
}

int procctl_continue(pid_t pid, int sig) {
  int continue_id = get_target_lwp(pid);
  int ret = sys_ptrace(PT_CONTINUE, continue_id, (void *)1, sig);
  if (ret != 0) {
    ret = sys_ptrace(PT_CONTINUE, pid, (void *)1, sig);
  }
  if (ret != 0) {
    klog_write_num("procctl_continue failed, errno=", (int64_t)sys_get_errno());
    return -1;
  }
  return 0;
}

int procctl_getregs(pid_t pid, struct reg *r) {
  if (r == NULL) {
    return -1;
  }

  int numlwps = sys_ptrace(PT_GETNUMLWPS, pid, NULL, 0);
  klog_write_num("target total LWPs=", (int64_t)numlwps);

  int lwps[32] = {0};
  int count = 0;
  if (numlwps > 0) {
    if (numlwps > 32)
      numlwps = 32;
    count = sys_ptrace(PT_GETLWPLIST, pid, (void *)lwps, numlwps);
    klog_write_num("fetched LWP count=", (int64_t)count);
  }

  for (int i = 0; i < count; i++) {
    klog_write_num("testing LWP=", (int64_t)lwps[i]);
    int ret = sys_ptrace(PT_GETREGS, lwps[i], r, 0);
    if (ret == 0) {
      s_target_lwp = lwps[i];
      klog_write_num("successfully read registers from LWP=", (int64_t)lwps[i]);
      return 0;
    }
    klog_write_num("LWP getregs errno=", (int64_t)sys_get_errno());
  }

  int ret = sys_ptrace(PT_GETREGS, pid, r, 0);
  if (ret == 0) {
    s_target_lwp = (int)pid;
    klog_write_num("successfully read registers from PID=", (int64_t)pid);
    return 0;
  }
  klog_write_num("PID getregs errno=", (int64_t)sys_get_errno());

  for (int attempt = 0; attempt < 20; attempt++) {
    volatile int spin = 0;
    for (int j = 0; j < 200000; j++)
      spin++;

    for (int i = 0; i < count; i++) {
      ret = sys_ptrace(PT_GETREGS, lwps[i], r, 0);
      if (ret == 0) {
        s_target_lwp = lwps[i];
        klog_write_num("delayed read registers from LWP=", (int64_t)lwps[i]);
        return 0;
      }
    }
    ret = sys_ptrace(PT_GETREGS, pid, r, 0);
    if (ret == 0) {
      s_target_lwp = (int)pid;
      klog_write_num("delayed read registers from PID=", (int64_t)pid);
      return 0;
    }
  }

  klog_write("ERROR: all LWPs failed getregs");
  return -1;
}

int procctl_setregs(pid_t pid, const struct reg *r) {
  if (r == NULL) {
    return -1;
  }
  int target_id = (s_target_lwp > 0) ? s_target_lwp : (int)pid;
  int ret = sys_ptrace(PT_SETREGS, target_id, (void *)r, 0);
  if (ret != 0 && target_id != (int)pid) {
    ret = sys_ptrace(PT_SETREGS, pid, (void *)r, 0);
  }
  if (ret != 0) {
    klog_write_num("procctl_setregs failed, errno=", (int64_t)sys_get_errno());
  }
  return ret;
}

int procctl_copyin(pid_t pid, const void *src, uintptr_t dst_addr, size_t len) {
  if (src == NULL || dst_addr == 0 || len == 0) {
    return -1;
  }
  struct ptrace_io_desc iod;
  iod.piod_op = PIOD_WRITE_D;
  iod.piod_offs = (void *)dst_addr;
  iod.piod_addr = (void *)src;
  iod.piod_len = len;
  int ret = sys_ptrace(PT_IO, pid, &iod, 0);
  if (ret != 0) {
    klog_write_hex("procctl_copyin failed at dst=", dst_addr);
    klog_write_num("procctl_copyin errno=", (int64_t)sys_get_errno());
  }
  return ret;
}

int procctl_copyout(pid_t pid, uintptr_t src_addr, void *dst, size_t len) {
  if (dst == NULL || src_addr == 0 || len == 0) {
    return -1;
  }
  struct ptrace_io_desc iod;
  iod.piod_op = PIOD_READ_D;
  iod.piod_offs = (void *)src_addr;
  iod.piod_addr = dst;
  iod.piod_len = len;
  int ret = sys_ptrace(PT_IO, pid, &iod, 0);
  if (ret != 0) {
    /* Fall back to PIOD_READ_I for instruction/text pages */
    iod.piod_op = PIOD_READ_I;
    ret = sys_ptrace(PT_IO, pid, &iod, 0);
  }
  if (ret != 0) {
    klog_write_hex("procctl_copyout failed at src=", src_addr);
    klog_write_num("procctl_copyout errno=", (int64_t)sys_get_errno());
  }
  return ret;
}

int procctl_setlong(pid_t pid, uintptr_t addr, uint64_t val) {
  return procctl_copyin(pid, &val, addr, sizeof(val));
}

uint64_t procctl_getlong(pid_t pid, uintptr_t addr) {
  uint64_t val = 0;
  procctl_copyout(pid, addr, &val, sizeof(val));
  return val;
}

int procctl_setint(pid_t pid, uintptr_t addr, uint32_t val) {
  return procctl_copyin(pid, &val, addr, sizeof(val));
}

uint32_t procctl_getint(pid_t pid, uintptr_t addr) {
  uint32_t val = 0;
  procctl_copyout(pid, addr, &val, sizeof(val));
  return val;
}

static uintptr_t s_remote_syscall_gadget = 0;

void procctl_set_syscall_gadget(uintptr_t gadget) {
  s_remote_syscall_gadget = gadget;
}

uintptr_t procctl_find_syscall_gadget(pid_t pid, uintptr_t libkernel_base) {
  /* 1. Try resolving via kernel dynlib table (W0xkN0+ZkCE) */
  uintptr_t sym = krw_dynlib_resolve(pid, 0x2001, "W0xkN0+ZkCE");
  if (sym == 0) {
    sym = krw_dynlib_resolve(pid, 1, "W0xkN0+ZkCE");
  }
  if (sym != 0) {
    s_remote_syscall_gadget = sym + 0x0A;
    klog_write_hex("resolved syscall gadget via kernel NID: ",
                   s_remote_syscall_gadget);
    return s_remote_syscall_gadget;
  }

  if (libkernel_base == 0) {
    return 0;
  }
  uintptr_t canonical = libkernel_base + 0x5ba;
  uint8_t buf[4] = {0};
  if (procctl_copyout(pid, canonical, buf, 2) == 0) {
    if (buf[0] == 0x0f && buf[1] == 0x05) { /* 0f 05 = syscall */
      klog_write_hex("verified syscall gadget at getpid+0xa: ", canonical);
      s_remote_syscall_gadget = canonical;
      return s_remote_syscall_gadget;
    }
  }
  /* Always fallback to canonical getpid + 0xa on FW 12.40 */
  klog_write_hex("using canonical syscall gadget: ", canonical);
  s_remote_syscall_gadget = canonical;
  return s_remote_syscall_gadget;
}

long procctl_remote_syscall(pid_t pid, int sysno, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5,
                            uint64_t a6) {
  klog_write_num("remote_syscall sysno=", (int64_t)sysno);
  struct reg bak_reg, jmp_reg;
  if (procctl_getregs(pid, &bak_reg) != 0) {
    klog_write("remote_syscall: failed to read bak_reg");
    return -1;
  }

  memcpy(&jmp_reg, &bak_reg, sizeof(jmp_reg));
  jmp_reg.r_rax = (uint64_t)sysno;
  jmp_reg.r_rdi = a1;
  jmp_reg.r_rsi = a2;
  jmp_reg.r_rdx = a3;
  jmp_reg.r_r10 = a4;
  jmp_reg.r_r8 = a5;
  jmp_reg.r_r9 = a6;

  if (s_remote_syscall_gadget != 0) {
    jmp_reg.r_rip = s_remote_syscall_gadget;
  }

  if (procctl_setregs(pid, &jmp_reg) != 0) {
    klog_write("remote_syscall: failed to set jmp_reg");
    return -1;
  }

  if (s_remote_syscall_gadget != 0) {
    if (procctl_step(pid) != 0) {
      klog_write("remote_syscall: procctl_step failed");
      procctl_setregs(pid, &bak_reg);
      return -1;
    }
    if (procctl_getregs(pid, &jmp_reg) != 0) {
      klog_write("remote_syscall: getregs failed after syscall");
      procctl_setregs(pid, &bak_reg);
      return -1;
    }
    long result = (long)jmp_reg.r_rax;
    if ((jmp_reg.r_rflags & 1) != 0) {
      klog_write_num("remote_syscall failed, errno=", (int64_t)result);
      procctl_setregs(pid, &bak_reg);
      return -result;
    }
    klog_write_hex("remote_syscall success, rax=", (uint64_t)result);
    procctl_setregs(pid, &bak_reg);
    return result;
  } else {
    /* Single step until return */
    int steps = 0;
    while (jmp_reg.r_rsp <= bak_reg.r_rsp) {
      if (procctl_step(pid) != 0) {
        klog_write_num("remote_syscall: procctl_step failed at step ",
                       (int64_t)steps);
        procctl_setregs(pid, &bak_reg);
        return -1;
      }
      if (procctl_getregs(pid, &jmp_reg) != 0) {
        klog_write("remote_syscall: getregs failed during stepping");
        procctl_setregs(pid, &bak_reg);
        return -1;
      }
      steps++;
      if (steps > 2000) {
        klog_write("WARNING: remote_syscall step limit exceeded (2000 steps)");
        break;
      }
    }
    long result = (long)jmp_reg.r_rax;
    klog_write_hex("remote_syscall result rax=", (uint64_t)result);
    procctl_setregs(pid, &bak_reg);
    return result;
  }
}

uintptr_t procctl_remote_mmap(pid_t pid, uintptr_t addr, size_t len, int prot,
                              int flags, int fd, off_t off) {
  klog_write_hex("remote_mmap req_addr=", addr);
  klog_write_hex("remote_mmap req_len=", (uint64_t)len);
  long ret = procctl_remote_syscall(
      pid, SYS_mmap, (uint64_t)addr, (uint64_t)len, (uint64_t)prot,
      (uint64_t)flags, (uint64_t)fd, (uint64_t)off);
  klog_write_hex("remote_mmap ret=", (uint64_t)ret);
  if (ret <= 0 || (uintptr_t)ret < 0x10000ULL ||
      (uintptr_t)ret > 0x00007fffffffffffULL) {
    return 0;
  }
  return (uintptr_t)ret;
}

int procctl_remote_munmap(pid_t pid, uintptr_t addr, size_t len) {
  return (int)procctl_remote_syscall(pid, SYS_munmap, (uint64_t)addr,
                                     (uint64_t)len, 0, 0, 0, 0);
}

int procctl_remote_mprotect(pid_t pid, uintptr_t addr, size_t len, int prot) {
  /* Try remote syscall first, fall back to direct kernel R/W mprotect */
  int ret = (int)procctl_remote_syscall(pid, SYS_mprotect, (uint64_t)addr,
                                        (uint64_t)len, (uint64_t)prot, 0, 0, 0);
  if (ret != 0 && krw_is_ready()) {
    ret = krw_mprotect(pid, addr, len, prot);
  }
  return ret;
}
