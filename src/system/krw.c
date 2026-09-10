/*
 * Kernel Read/Write Implementation.
 *
 * Consumes the kernel R/W primitive established for the session by kstuff-lite,
 * pinned against ps5-payload-dev-sdk.
 */

#include "oops/krw.h"
#include "oops/freestd.h"
#include "oops/syscall.h"

__attribute__((weak)) void klog_write(const char *msg) {
  if (msg == NULL) {
    return;
  }
  char buf[256];
  const char *prefix = "[INJECTOR] ";
  size_t plen = obs_strlen(prefix);
  size_t mlen = obs_strlen(msg);
  if (plen + mlen + 2 > sizeof(buf)) {
    mlen = sizeof(buf) - plen - 2;
  }
  memcpy(buf, prefix, plen);
  memcpy(buf + plen, msg, mlen);
  buf[plen + mlen] = '\n';
  buf[plen + mlen + 1] = '\0';
  sys_call(SYS_klog, 7, (long)buf, 0, 0, 0, 0);
}

__attribute__((weak)) void klog_write_hex(const char *prefix, uint64_t hex) {
  char buf[128];
  size_t plen = prefix != NULL ? obs_strlen(prefix) : 0;
  if (plen > sizeof(buf) - 20) {
    plen = sizeof(buf) - 20;
  }
  if (prefix != NULL && plen > 0) {
    memcpy(buf, prefix, plen);
  }
  size_t hlen = obs_format_hex(buf + plen, hex);
  buf[plen + hlen] = '\0';
  klog_write(buf);
}

__attribute__((weak)) void klog_write_num(const char *prefix, int64_t num) {
  char buf[128];
  size_t plen = prefix != NULL ? obs_strlen(prefix) : 0;
  if (plen > sizeof(buf) - 25) {
    plen = sizeof(buf) - 25;
  }
  if (prefix != NULL && plen > 0) {
    memcpy(buf, prefix, plen);
  }
  size_t nlen = obs_format_i64(buf + plen, num);
  buf[plen + nlen] = '\0';
  klog_write(buf);
}

#define IPPROTO_IPV6 41
#define IPV6_PKTINFO 46
#define IN6_PKTINFOSZ 5

#define SYS_read 3
#define SYS_write 4
#define SYS_getpid 20
#define SYS_setsockopt 105
#define SYS_dynlib_get_obj_member 649

#define EINVAL 22
#define EFAULT 14
#define ESRCH 3

typedef union kernel_pipebuf {
  unsigned int n[IN6_PKTINFOSZ];
  struct __attribute__((packed)) {
    unsigned int cnt;
    unsigned int in;
    unsigned int out;
    unsigned long reserved;
  } flags;

  struct __attribute__((packed)) {
    unsigned int size;
    unsigned long kaddr;
    unsigned long reserved;
  } pbuf;

  struct __attribute__((packed)) {
    unsigned long kaddr;
    unsigned int reserved[3];
  } vbuf;
} kernel_pipebuf_t;

/* State variables */
static int s_master_sock = -1;
static int s_victim_sock = -1;
static int s_rwpipe[2] = {-1, -1};
static uintptr_t s_pipe_addr = 0;
static uintptr_t s_kdata_base = 0;
static uintptr_t s_ktext_base = 0;
static uintptr_t s_allproc_addr = 0;
static uint32_t s_fw_version = 0;
static int s_ready = 0;

/* Saved original credentials of injector process */
static uint64_t s_orig_authid = 0;
static uint8_t s_orig_caps[16];
static uint8_t s_orig_attrs[32];
static int s_elevated = 0;
static uint32_t s_orig_my_uids[3] = {0};
static int s_my_uids_elevated = 0;
static uint32_t s_orig_target_uids[3] = {0};
static int s_target_uids_elevated = 0;
static pid_t s_elevated_target_pid = 0;
static uintptr_t s_orig_ucred_ptr = 0;
static uintptr_t s_orig_td_ptr = 0;
static uintptr_t s_orig_td_ucred_offset = 0;
static uintptr_t s_orig_td_ucred_ptr = 0;

/* Offsets pinned from ps5-payload-dev-sdk */
static const uintptr_t KERNEL_OFFSET_PROC_P_UCRED = 0x40;
static const uintptr_t KERNEL_OFFSET_PROC_P_FD = 0x48;
static const uintptr_t KERNEL_OFFSET_PROC_P_PID = 0xBC;
static const uintptr_t KERNEL_OFFSET_PROC_P_VMSPACE = 0x200;

static const uintptr_t KERNEL_OFFSET_UCRED_CR_SCEAUTHID = 0x58;
static const uintptr_t KERNEL_OFFSET_UCRED_CR_SCECAPS = 0x60;
static const uintptr_t KERNEL_OFFSET_UCRED_CR_SCEATTRS = 0x80;

static uintptr_t s_offset_vmspace_p_root = 0x1d0;
static uintptr_t s_offset_vmspace_vm_pmap = 0x2e8;
static uintptr_t s_kernel_root_vnode = 0;

static uint32_t detect_fw_version(void) {
  struct Sce_Proc_Param {
    unsigned long structsize;
    unsigned int magic;
    unsigned int ent_count;
    unsigned int sdk_ps4_ver;
    unsigned int sdk_ps5_ver;
  } *param = NULL;

  if (sys_call(SYS_dynlib_get_obj_member, 0x2, 8, (long)&param, 0, 0, 0) == 0 &&
      param != NULL && param->sdk_ps5_ver > 0x02000009) {
    return param->sdk_ps5_ver;
  }
  return 0x12400000; /* Fallback default (FW 12.40) */
}

int krw_init(const payload_args_t *args) {
  if (args == NULL || args->rwpair == NULL || args->rwpipe == NULL) {
    return -EINVAL;
  }
  sys_call_init(args);
  if (args->rwpair[0] < 0 || args->rwpair[1] < 0) {
    return -EINVAL;
  }
  if (args->rwpipe[0] < 0 || args->rwpipe[1] < 0) {
    return -EINVAL;
  }
  if (args->kpipe_addr == 0 || args->kdata_base_addr == 0) {
    return -EFAULT;
  }

  s_master_sock = args->rwpair[0];
  s_victim_sock = args->rwpair[1];
  s_rwpipe[0] = args->rwpipe[0];
  s_rwpipe[1] = args->rwpipe[1];
  s_pipe_addr = (uintptr_t)args->kpipe_addr;
  s_kdata_base = (uintptr_t)args->kdata_base_addr;

  s_fw_version = detect_fw_version();

  /* Firmware offset table lookup pinned from SDK kernel.c */
  switch (s_fw_version & 0xffff0000u) {
  case 0x01000000:
  case 0x01010000:
  case 0x01020000:
    s_ktext_base = s_kdata_base - 0x1B40000;
    s_allproc_addr = s_kdata_base + 0x26D1BF8;
    s_offset_vmspace_p_root = 0x1c0;
    s_offset_vmspace_vm_pmap = 0x2c0;
    break;
  case 0x01050000:
  case 0x01100000:
  case 0x01110000:
  case 0x01120000:
  case 0x01130000:
  case 0x01140000:
    s_ktext_base = s_kdata_base - 0x1B40000;
    s_allproc_addr = s_kdata_base + 0x26D1C18;
    s_offset_vmspace_p_root = 0x1c8;
    s_offset_vmspace_vm_pmap = 0x2e0;
    break;
  case 0x02000000:
  case 0x02200000:
  case 0x02250000:
  case 0x02260000:
  case 0x02300000:
  case 0x02500000:
  case 0x02700000:
    s_ktext_base = s_kdata_base - 0x1B80000;
    s_allproc_addr = s_kdata_base + 0x2701C28;
    s_offset_vmspace_p_root = 0x1c8;
    s_offset_vmspace_vm_pmap = 0x2e0;
    break;
  case 0x03000000:
  case 0x03100000:
  case 0x03200000:
  case 0x03210000:
    s_ktext_base = s_kdata_base - 0x0BD0000;
    s_allproc_addr = s_kdata_base + 0x276DC58;
    s_offset_vmspace_p_root = 0x1c8;
    s_offset_vmspace_vm_pmap = 0x2e0;
    break;
  case 0x04000000:
  case 0x04020000:
  case 0x04030000:
  case 0x04500000:
  case 0x04510000:
    s_ktext_base = s_kdata_base - 0x0C00000;
    s_allproc_addr = s_kdata_base + 0x27EDCB8;
    s_offset_vmspace_p_root = 0x1c8;
    s_offset_vmspace_vm_pmap = 0x2e0;
    break;
  case 0x05000000:
  case 0x05020000:
  case 0x05100000:
  case 0x05500000:
    s_ktext_base = s_kdata_base - 0x0C40000;
    s_allproc_addr = s_kdata_base + 0x291DD00;
    s_offset_vmspace_p_root = 0x1c8;
    s_offset_vmspace_vm_pmap = 0x2e0;
    break;
  case 0x06000000:
  case 0x06020000:
  case 0x06500000:
    s_ktext_base = s_kdata_base - 0x0C60000;
    s_allproc_addr = s_kdata_base + 0x2869D20;
    s_offset_vmspace_p_root = 0x1d0;
    s_offset_vmspace_vm_pmap = 0x2e8;
    break;
  case 0x07000000:
  case 0x07010000:
  case 0x07200000:
  case 0x07400000:
  case 0x07600000:
  case 0x07610000:
    s_ktext_base = s_kdata_base - 0x0C50000;
    s_allproc_addr = s_kdata_base + 0x2859D50;
    s_offset_vmspace_p_root = 0x1d0;
    s_offset_vmspace_vm_pmap = 0x2e8;
    break;
  case 0x08000000:
  case 0x08200000:
  case 0x08400000:
  case 0x08600000:
    s_ktext_base = s_kdata_base - 0x0C70000;
    s_allproc_addr = s_kdata_base + 0x2875D50;
    s_offset_vmspace_p_root = 0x1d0;
    s_offset_vmspace_vm_pmap = 0x2e8;
    break;
  default:
    /* 9.xx through 13.xx default heuristics */
    s_ktext_base = s_kdata_base - 0x0D50000;
    s_allproc_addr = s_kdata_base + 0x2885E00;
    s_offset_vmspace_p_root = 0x1d0;
    s_offset_vmspace_vm_pmap = 0x2e8;
    break;
  }

  s_ready = 1;
  return 0;
}

int krw_is_ready(void) { return s_ready; }

uintptr_t krw_kdata_base(void) { return s_kdata_base; }

uintptr_t krw_ktext_base(void) { return s_ktext_base; }

uintptr_t krw_allproc_addr(void) { return s_allproc_addr; }

void krw_set_allproc_addr(uintptr_t addr) { s_allproc_addr = addr; }

uint32_t krw_fw_version(void) { return s_fw_version; }

static int raw_kernel_write(uintptr_t kaddr, const void *data, size_t len) {
  kernel_pipebuf_t buf;
  memset(&buf, 0, sizeof(buf));

  if (!(kaddr & 0xffff000000000000ULL)) {
    return -EFAULT;
  }

  buf.vbuf.kaddr = kaddr;
  if (sys_call(SYS_setsockopt, s_master_sock, IPPROTO_IPV6, IPV6_PKTINFO,
               (long)&buf, sizeof(buf), 0) != 0) {
    return -1;
  }

  if (sys_call(SYS_setsockopt, s_victim_sock, IPPROTO_IPV6, IPV6_PKTINFO,
               (long)data, (long)len, 0) != 0) {
    return -1;
  }

  return 0;
}

int krw_copyin(const void *uaddr, uintptr_t kaddr, size_t len) {
  kernel_pipebuf_t buf;

  if (uaddr == NULL || kaddr == 0 || len == 0) {
    return -EINVAL;
  }

  memset(&buf, 0, sizeof(buf));
  buf.flags.reserved = 0x40000000;
  if (raw_kernel_write(s_pipe_addr, &buf, sizeof(buf)) != 0) {
    return -1;
  }

  buf.pbuf.size = 0x40000000;
  buf.pbuf.kaddr = kaddr;
  buf.pbuf.reserved = 0;
  if (raw_kernel_write(s_pipe_addr + 12, &buf, sizeof(buf)) != 0) {
    return -1;
  }

  if (sys_call(SYS_write, s_rwpipe[1], (long)uaddr, (long)len, 0, 0, 0) < 0) {
    return -1;
  }

  return 0;
}

int krw_copyout(uintptr_t kaddr, void *uaddr, size_t len) {
  kernel_pipebuf_t buf;

  if (uaddr == NULL || kaddr == 0 || len == 0) {
    return -EINVAL;
  }

  memset(&buf, 0, sizeof(buf));
  buf.flags.cnt = 0x40000000;
  buf.flags.in = 0x40000000;
  if (raw_kernel_write(s_pipe_addr, &buf, sizeof(buf)) != 0) {
    return -1;
  }

  buf.pbuf.size = 0x40000000;
  buf.pbuf.kaddr = kaddr;
  buf.pbuf.reserved = 0;
  if (raw_kernel_write(s_pipe_addr + 12, &buf, sizeof(buf)) != 0) {
    return -1;
  }

  if (sys_call(SYS_read, s_rwpipe[0], (long)uaddr, (long)len, 0, 0, 0) < 0) {
    return -1;
  }

  return 0;
}

uint64_t krw_read64(uintptr_t kaddr) {
  uint64_t val = 0;
  krw_copyout(kaddr, &val, sizeof(val));
  return val;
}

uint32_t krw_read32(uintptr_t kaddr) {
  uint32_t val = 0;
  krw_copyout(kaddr, &val, sizeof(val));
  return val;
}

uint16_t krw_read16(uintptr_t kaddr) {
  uint16_t val = 0;
  krw_copyout(kaddr, &val, sizeof(val));
  return val;
}

uint8_t krw_read8(uintptr_t kaddr) {
  uint8_t val = 0;
  krw_copyout(kaddr, &val, sizeof(val));
  return val;
}

int krw_write64(uintptr_t kaddr, uint64_t val) {
  return krw_copyin(&val, kaddr, sizeof(val));
}

int krw_write32(uintptr_t kaddr, uint32_t val) {
  return krw_copyin(&val, kaddr, sizeof(val));
}

int krw_write16(uintptr_t kaddr, uint16_t val) {
  return krw_copyin(&val, kaddr, sizeof(val));
}

int krw_write8(uintptr_t kaddr, uint8_t val) {
  return krw_copyin(&val, kaddr, sizeof(val));
}

uintptr_t krw_get_proc(pid_t pid) {
  uintptr_t proc = 0;
  if (krw_copyout(s_allproc_addr, &proc, sizeof(proc)) != 0) {
    return 0;
  }

  while (proc != 0) {
    pid_t p_pid = 0;
    if (krw_copyout(proc + KERNEL_OFFSET_PROC_P_PID, &p_pid, sizeof(p_pid)) !=
        0) {
      break;
    }
    if (p_pid == pid) {
      return proc;
    }
    if (krw_copyout(proc, &proc, sizeof(proc)) != 0) {
      break;
    }
  }
  return 0;
}

uintptr_t krw_find_proc_by_name(const char *name) {
  if (name == NULL || !s_ready || s_allproc_addr == 0) {
    return 0;
  }
  uintptr_t proc = 0;
  if (krw_copyout(s_allproc_addr, &proc, sizeof(proc)) != 0) {
    return 0;
  }

  while (proc != 0) {
    char comm[32];
    memset(comm, 0, sizeof(comm));
    krw_copyout(proc + 0x61E, comm, 16);
    if (obs_strcmp(comm, name) == 0) {
      return proc;
    }
    if (krw_copyout(proc, &proc, sizeof(proc)) != 0) {
      break;
    }
  }
  return 0;
}

uintptr_t krw_get_ucred(pid_t pid) {
  uintptr_t proc = krw_get_proc(pid);
  if (proc == 0) {
    return 0;
  }
  uintptr_t ucred = 0;
  if (krw_copyout(proc + KERNEL_OFFSET_PROC_P_UCRED, &ucred, sizeof(ucred)) !=
      0) {
    return 0;
  }
  return ucred;
}

uint64_t krw_get_ucred_authid(pid_t pid) {
  uintptr_t ucred = krw_get_ucred(pid);
  if (ucred == 0) {
    return 0;
  }
  uint64_t authid = 0;
  krw_copyout(ucred + KERNEL_OFFSET_UCRED_CR_SCEAUTHID, &authid,
              sizeof(authid));
  return authid;
}

int krw_set_ucred_authid(pid_t pid, uint64_t authid) {
  uintptr_t ucred = krw_get_ucred(pid);
  if (ucred == 0) {
    return -1;
  }
  return krw_copyin(&authid, ucred + KERNEL_OFFSET_UCRED_CR_SCEAUTHID,
                    sizeof(authid));
}

int krw_get_ucred_caps(pid_t pid, uint8_t caps[16]) {
  uintptr_t ucred = krw_get_ucred(pid);
  if (ucred == 0 || caps == NULL) {
    return -1;
  }
  return krw_copyout(ucred + KERNEL_OFFSET_UCRED_CR_SCECAPS, caps, 16);
}

int krw_set_ucred_caps(pid_t pid, const uint8_t caps[16]) {
  uintptr_t ucred = krw_get_ucred(pid);
  if (ucred == 0 || caps == NULL) {
    return -1;
  }
  return krw_copyin(caps, ucred + KERNEL_OFFSET_UCRED_CR_SCECAPS, 16);
}

int krw_get_ucred_attrs(pid_t pid, uint8_t attrs[32]) {
  uintptr_t ucred = krw_get_ucred(pid);
  if (ucred == 0 || attrs == NULL) {
    return -1;
  }
  return krw_copyout(ucred + KERNEL_OFFSET_UCRED_CR_SCEATTRS, attrs, 32);
}

int krw_set_ucred_attrs(pid_t pid, const uint8_t attrs[32]) {
  uintptr_t ucred = krw_get_ucred(pid);
  if (ucred == 0 || attrs == NULL) {
    return -1;
  }
  return krw_copyin(attrs, ucred + KERNEL_OFFSET_UCRED_CR_SCEATTRS, 32);
}

uintptr_t krw_get_root_vnode(void) {
  if (s_kernel_root_vnode != 0) {
    return s_kernel_root_vnode;
  }
  pid_t mypid = (pid_t)sys_call(SYS_getpid, 0, 0, 0, 0, 0, 0);
  uintptr_t rdir = krw_get_proc_rootdir(mypid);
  if (rdir != 0) {
    s_kernel_root_vnode = rdir;
    return rdir;
  }
  return 0;
}

uintptr_t krw_get_proc_rootdir(pid_t pid) {
  uintptr_t proc = krw_get_proc(pid);
  if (proc == 0)
    return 0;
  uintptr_t fd = 0;
  if (krw_copyout(proc + KERNEL_OFFSET_PROC_P_FD, &fd, sizeof(fd)) != 0 ||
      fd == 0)
    return 0;
  uintptr_t rdir = 0;
  krw_copyout(fd + 0x18, &rdir, sizeof(rdir));
  return rdir;
}

int krw_set_proc_rootdir(pid_t pid, uintptr_t vnode) {
  uintptr_t proc = krw_get_proc(pid);
  if (proc == 0)
    return -1;
  uintptr_t fd = 0;
  if (krw_copyout(proc + KERNEL_OFFSET_PROC_P_FD, &fd, sizeof(fd)) != 0 ||
      fd == 0)
    return -1;
  return krw_copyin(&vnode, fd + 0x18, sizeof(vnode));
}

uintptr_t krw_get_proc_jaildir(pid_t pid) {
  uintptr_t proc = krw_get_proc(pid);
  if (proc == 0)
    return 0;
  uintptr_t fd = 0;
  if (krw_copyout(proc + KERNEL_OFFSET_PROC_P_FD, &fd, sizeof(fd)) != 0 ||
      fd == 0)
    return 0;
  uintptr_t jdir = 0;
  krw_copyout(fd + 0x20, &jdir, sizeof(jdir));
  return jdir;
}

int krw_set_proc_jaildir(pid_t pid, uintptr_t vnode) {
  uintptr_t proc = krw_get_proc(pid);
  if (proc == 0)
    return -1;
  uintptr_t fd = 0;
  if (krw_copyout(proc + KERNEL_OFFSET_PROC_P_FD, &fd, sizeof(fd)) != 0 ||
      fd == 0)
    return -1;
  return krw_copyin(&vnode, fd + 0x20, sizeof(vnode));
}

int krw_elevate_current_process(void) {
  pid_t mypid = (pid_t)sys_call(SYS_getpid, 0, 0, 0, 0, 0, 0);
  uintptr_t kproc = krw_get_proc(mypid);
  if (kproc == 0) {
    return -1;
  }

  /* Backup current credentials */
  s_orig_authid = krw_get_ucred_authid(mypid);
  krw_get_ucred_caps(mypid, s_orig_caps);
  krw_get_ucred_attrs(mypid, s_orig_attrs);

  /* Elevate authid to allow ptrace */
  krw_set_ucred_authid(mypid, 0x4800000000010003ULL);

  /* Enable all capabilities */
  uint8_t privcaps[16];
  memset(privcaps, 0xff, sizeof(privcaps));
  krw_set_ucred_caps(mypid, privcaps);

  /* Set ptrace attribute */
  uint8_t attrs[32];
  memcpy(attrs, s_orig_attrs, sizeof(attrs));
  attrs[3] |= 0x80;
  krw_set_ucred_attrs(mypid, attrs);

  /* Patch syscall range for unrestricted syscalls */
  uintptr_t kaddr = 0;
  if (krw_copyout(kproc + 0x3e8, &kaddr, sizeof(kaddr)) == 0 && kaddr != 0) {
    uintptr_t low = 0;
    uintptr_t high = ~(uintptr_t)0;
    krw_copyin(&low, kaddr + 0xf0, sizeof(low));
    krw_copyin(&high, kaddr + 0xf8, sizeof(high));
  }

  /* Elevate current UIDs to root */
  uintptr_t my_ucred = krw_get_ucred(mypid);
  if (my_ucred != 0 && !s_my_uids_elevated) {
    krw_copyout(my_ucred + 0x04, s_orig_my_uids, sizeof(s_orig_my_uids));
    s_my_uids_elevated = 1;
    uint32_t zeros[3] = {0};
    krw_copyin(zeros, my_ucred + 0x04, sizeof(zeros));
  }

  uint64_t my_readback_caps[2] = {0};
  krw_get_ucred_caps(mypid, (uint8_t *)my_readback_caps);
  klog_write_hex("my elevated caps[0]=", my_readback_caps[0]);
  klog_write_hex("my elevated caps[1]=", my_readback_caps[1]);

  /* Clear P2_NOTRACE and P2_PTRACEREQ on current proc */
  uint32_t my_p_flag2 = 0;
  if (krw_copyout(kproc + 0xB4, &my_p_flag2, sizeof(my_p_flag2)) == 0) {
    klog_write_hex("my orig p_flag2=", my_p_flag2);
    uint32_t clear_mask2 = 0x00004006;
    if (my_p_flag2 & clear_mask2) {
      my_p_flag2 &= ~clear_mask2;
      krw_copyin(&my_p_flag2, kproc + 0xB4, sizeof(my_p_flag2));
      klog_write_hex("cleared my p_flag2 bits, new p_flag2=", my_p_flag2);
    }
  }

  /* Apply kernel ptrace patch to enable game PT_ATTACH without AppContext
   * gating rejection */
  if (krw_apply_ptrace_kernel_patch() != 0) {
    klog_write("WARNING: krw_apply_ptrace_kernel_patch failed");
  } else {
    klog_write("ptrace kernel patch active");
  }

  /* Check SceShellCore attributes for diagnostic inspection */
  uintptr_t shellproc = krw_find_proc_by_name("SceShellCore");
  if (shellproc != 0) {
    pid_t shellpid = 0;
    krw_copyout(shellproc + KERNEL_OFFSET_PROC_P_PID, &shellpid,
                sizeof(shellpid));
    uintptr_t shellucred = 0;
    krw_copyout(shellproc + KERNEL_OFFSET_PROC_P_UCRED, &shellucred,
                sizeof(shellucred));
    uint64_t shellauthid = 0;
    uintptr_t shellprison = 0;
    if (shellucred != 0) {
      krw_copyout(shellucred + KERNEL_OFFSET_UCRED_CR_SCEAUTHID, &shellauthid,
                  sizeof(shellauthid));
      krw_copyout(shellucred + 0x30, &shellprison, sizeof(shellprison));
    }
    klog_write_num("SceShellCore pid=", (int64_t)shellpid);
    klog_write_hex("SceShellCore kproc=", shellproc);
    klog_write_hex("SceShellCore ucred=", shellucred);
    klog_write_hex("SceShellCore authid=", shellauthid);
    klog_write_hex("SceShellCore prison=", shellprison);
  }

  s_elevated = 1;
  return 0;
}

int krw_elevate_process(pid_t pid) {
  uintptr_t kproc = krw_get_proc(pid);
  if (kproc == 0) {
    return -1;
  }

  s_elevated_target_pid = pid;

  /* Elevate authid and capabilities to prevent sandbox denial */
  krw_set_ucred_authid(pid, 0x4800000000010003ULL);

  uint8_t privcaps[16];
  memset(privcaps, 0xff, sizeof(privcaps));
  krw_set_ucred_caps(pid, privcaps);

  /* Set ptrace attribute on target */
  uint8_t attrs[32];
  if (krw_get_ucred_attrs(pid, attrs) == 0) {
    attrs[3] |= 0x80;
    krw_set_ucred_attrs(pid, attrs);
  }

  uint64_t tgt_readback_caps[2] = {0};
  krw_get_ucred_caps(pid, (uint8_t *)tgt_readback_caps);
  klog_write_hex("target elevated caps[0]=", tgt_readback_caps[0]);
  klog_write_hex("target elevated caps[1]=", tgt_readback_caps[1]);
  klog_write_hex("target elevated attr[3]=", (uint64_t)attrs[3]);

  /* Elevate target UIDs to root (0) to satisfy
   * priv_check_cred(PRIV_DEBUG_UNPRIV) */
  uintptr_t target_ucred = krw_get_ucred(pid);
  if (target_ucred != 0 && !s_target_uids_elevated) {
    krw_copyout(target_ucred + 0x04, s_orig_target_uids,
                sizeof(s_orig_target_uids));
    s_target_uids_elevated = 1;
    uint32_t zeros[3] = {0};
    krw_copyin(zeros, target_ucred + 0x04, sizeof(zeros));
    klog_write_num("target orig cr_uid=", (int64_t)s_orig_target_uids[0]);
  }

  /* Note: preserve target game rootdir/jaildir vnodes so game asset reads
   * remain intact */

  /* Clear P_SUGID (0x100), P_TRACED (0x800), P_STOPPED_TRACE (0x20000),
   * P_STOPPED_SIG (0x40000) on target proc */
  uint32_t p_flag = 0;
  if (krw_copyout(kproc + 0xB0, &p_flag, sizeof(p_flag)) == 0) {
    uint32_t clear_mask =
        0x00060900; /* P_SUGID | P_TRACED | P_STOPPED_TRACE | P_STOPPED_SIG */
    if (p_flag & clear_mask) {
      p_flag &= ~clear_mask;
      krw_copyin(&p_flag, kproc + 0xB0, sizeof(p_flag));
      klog_write_hex("cleared target p_flag bits, new p_flag=", p_flag);
    }
  }

  /* Clear all p_flag2 bits on target proc to guarantee no trace restrictions */
  uint32_t zero_p_flag2 = 0;
  krw_copyin(&zero_p_flag2, kproc + 0xB4, sizeof(zero_p_flag2));

  return 0;
}

int krw_swap_ucred(pid_t target_pid) {
  (void)target_pid;
  /* Current process is already elevated with full caps and authid.
   * With ptrace kernel patch active, ptrace attach succeeds without
   * needing prison manipulation or unverified thread struct writes. */
  return 0;
}

int krw_restore_ucred(void) {
  if (s_orig_td_ptr != 0 && s_orig_td_ucred_offset != 0) {
    krw_copyin(&s_orig_td_ucred_ptr, s_orig_td_ptr + s_orig_td_ucred_offset,
               sizeof(s_orig_td_ucred_ptr));
    s_orig_td_ptr = 0;
    s_orig_td_ucred_offset = 0;
    s_orig_td_ucred_ptr = 0;
  }
  if (s_target_uids_elevated && s_elevated_target_pid > 0) {
    uintptr_t target_ucred = krw_get_ucred(s_elevated_target_pid);
    if (target_ucred != 0) {
      krw_copyin(s_orig_target_uids, target_ucred + 0x04,
                 sizeof(s_orig_target_uids));
    }
    s_target_uids_elevated = 0;
    s_elevated_target_pid = 0;
  }
  if (s_orig_ucred_ptr == 0) {
    return 0;
  }
  pid_t mypid = (pid_t)sys_call(SYS_getpid, 0, 0, 0, 0, 0, 0);
  uintptr_t my_kproc = krw_get_proc(mypid);
  if (my_kproc == 0) {
    return -1;
  }
  int ret = krw_copyin(&s_orig_ucred_ptr, my_kproc + KERNEL_OFFSET_PROC_P_UCRED,
                       sizeof(s_orig_ucred_ptr));
  s_orig_ucred_ptr = 0;
  return ret;
}

int krw_restore_current_process(void) {
  krw_restore_ucred();
  if (!s_elevated) {
    return 0;
  }
  pid_t mypid = (pid_t)sys_call(SYS_getpid, 0, 0, 0, 0, 0, 0);
  if (s_my_uids_elevated) {
    uintptr_t my_ucred = krw_get_ucred(mypid);
    if (my_ucred != 0) {
      krw_copyin(s_orig_my_uids, my_ucred + 0x04, sizeof(s_orig_my_uids));
    }
    s_my_uids_elevated = 0;
  }
  krw_set_ucred_authid(mypid, s_orig_authid);
  krw_set_ucred_caps(mypid, s_orig_caps);
  krw_set_ucred_attrs(mypid, s_orig_attrs);
  s_elevated = 0;
  return 0;
}

int krw_mprotect(pid_t pid, uintptr_t addr, size_t len, int prot) {
  (void)prot;
  uintptr_t kproc = krw_get_proc(pid);
  if (kproc == 0)
    return -1;
  uintptr_t vmspace = 0;
  if (krw_copyout(kproc + KERNEL_OFFSET_PROC_P_VMSPACE, &vmspace,
                  sizeof(vmspace)) != 0 ||
      vmspace == 0) {
    return -1;
  }

  /* Walk vm_map entries to locate the region covering addr and adjust
   * protection */
  uintptr_t root = 0;
  if (krw_copyout(vmspace + s_offset_vmspace_p_root, &root, sizeof(root)) !=
          0 ||
      root == 0) {
    return -1;
  }

  uintptr_t entry = root;
  while (entry != 0) {
    uintptr_t start = 0, end = 0;
    krw_copyout(entry + 0x20, &start, sizeof(start));
    krw_copyout(entry + 0x28, &end, sizeof(end));

    if (addr >= start && (addr + len) <= end) {
      uint8_t p = (uint8_t)(prot & 0x7);
      /* entry->protection at offset 0x58 or 0x5c depending on struct */
      krw_copyin(&p, entry + 0x58, sizeof(p));
      krw_copyin(&p, entry + 0x59, sizeof(p)); /* max_protection */
      return 0;
    }

    uintptr_t next = 0;
    if (krw_copyout(entry + 0x10, &next, sizeof(next)) != 0 || next == root) {
      break;
    }
    entry = next;
  }
  return 0;
}

int krw_apply_ptrace_kernel_patch(void) {
  if (s_kdata_base == 0) {
    return -1;
  }

  uintptr_t patch_offset = 0;
  uint32_t fw = s_fw_version & 0xffff0000u;
  switch (fw) {
  case 0x03000000u:
  case 0x03100000u:
  case 0x03200000u:
  case 0x03210000u:
    patch_offset = 0x6466498ULL;
    break;
  case 0x04020000u:
    patch_offset = 0x6505498ULL;
    break;
  case 0x04000000u:
  case 0x04030000u:
  case 0x04500000u:
  case 0x04510000u:
    patch_offset = 0x6506498ULL;
    break;
  case 0x05000000u:
  case 0x05020000u:
  case 0x05100000u:
  case 0x05500000u:
    patch_offset = 0x6646710ULL;
    break;
  case 0x06000000u:
  case 0x06020000u:
  case 0x06500000u:
    patch_offset = 0x6596910ULL;
    break;
  case 0x07000000u:
  case 0x07010000u:
  case 0x07010100u:
  case 0x07200000u:
  case 0x07400000u:
  case 0x07600000u:
  case 0x07610000u:
    patch_offset = 0xAC8088ULL;
    break;
  case 0x08000000u:
  case 0x08200000u:
  case 0x08400000u:
  case 0x08600000u:
    patch_offset = 0xAC3088ULL;
    break;
  case 0x09000000u:
    patch_offset = 0xD72088ULL;
    break;
  case 0x09050000u:
  case 0x09200000u:
  case 0x09400000u:
  case 0x09600000u:
    patch_offset = 0xD73088ULL;
    break;
  case 0x10000000u:
  case 0x10010000u:
  case 0x10200000u:
  case 0x10400000u:
  case 0x10600000u:
    patch_offset = 0xD79088ULL;
    break;
  case 0x11000000u:
  case 0x11200000u:
  case 0x11400000u:
  case 0x11600000u:
    patch_offset = 0xD8C088ULL;
    break;
  case 0x12000000u:
  case 0x12020000u:
  case 0x12200000u:
  case 0x12400000u:
  case 0x12600000u:
  case 0x12700000u:
    patch_offset = 0xD83088ULL;
    break;
  case 0x13000000u:
  case 0x13200000u:
    patch_offset = 0xD99088ULL;
    break;
  default:
    patch_offset = 0xD83088ULL;
    break;
  }

  uintptr_t patch_addr = s_kdata_base + patch_offset;
  uint8_t scratch[16] = {0};
  if (krw_copyout(patch_addr, scratch, sizeof(scratch)) != 0) {
    klog_write("kpatch READ failed");
    return -1;
  }
  klog_write_hex("kpatch addr=", patch_addr);
  klog_write_hex("kpatch read byte[1]=", (uint64_t)scratch[1]);

  if ((scratch[1] & 3) == 3) {
    klog_write("kpatch already set");
    return 0;
  }

  scratch[1] |= 3;
  if (krw_copyin(scratch, patch_addr, sizeof(scratch)) != 0) {
    klog_write("kpatch WRITE failed");
    return -1;
  }
  klog_write_hex("kpatch written, new byte[1]=", (uint64_t)scratch[1]);

  uint8_t verify[16] = {0};
  if (krw_copyout(patch_addr, verify, sizeof(verify)) != 0) {
    klog_write("kpatch VERIFY failed");
    return -1;
  }
  klog_write_hex("kpatch verify byte[1]=", (uint64_t)verify[1]);
  return ((verify[1] & 3) == 3) ? 0 : -1;
}

uintptr_t krw_find_target_libkernel_base(uintptr_t target_kproc,
                                         uintptr_t rip_hint) {
  if (target_kproc != 0) {
    uintptr_t cur = 0;
    if (krw_copyout(target_kproc + 0x3E8, &cur, sizeof(cur)) == 0 && cur != 0) {
      uintptr_t node = 0;
      if (krw_copyout(cur, &node, sizeof(node)) == 0 && node != 0) {
        int count = 0;
        while (node != 0 && count < 64) {
          uint32_t entry_sel = 0;
          krw_copyout(node + 0x28, &entry_sel, sizeof(entry_sel));
          uint64_t mapbase = 0;
          krw_copyout(node + 0x30, &mapbase, sizeof(mapbase));
          if ((entry_sel == 0x2001 || entry_sel == 1) && mapbase != 0) {
            klog_write_hex("found target libkernel mapbase via sel=",
                           (uint64_t)entry_sel);
            klog_write_hex("target libkernel mapbase=", mapbase);
            return (uintptr_t)mapbase;
          }
          uintptr_t next = 0;
          if (krw_copyout(node, &next, sizeof(next)) != 0 || next == node) {
            break;
          }
          node = next;
          count++;
        }
      }
    }
  }
  if (rip_hint >= 0x800000000ULL && rip_hint < 0x900000000ULL) {
    klog_write("using RIP hint for libkernel base: 0x800000000");
    return 0x800000000ULL;
  }
  klog_write("defaulting target libkernel base: 0x800000000");
  return 0x800000000ULL;
}

uintptr_t krw_dynlib_resolve(pid_t pid, int sprx_handle, const char *nid) {
  uintptr_t kproc = krw_get_proc(pid);
  if (kproc == 0)
    return 0;

  uintptr_t kaddr = 0;
  if (krw_copyout(kproc + 0x3E8, &kaddr, sizeof(kaddr)) != 0 || kaddr == 0)
    return 0;

  uintptr_t cur = 0;
  if (krw_copyout(kaddr, &cur, sizeof(cur)) != 0 || cur == 0)
    return 0;

  uint8_t module_record[0x180] = {0};
  int found = 0;
  int count = 0;
  while (cur != 0 && count < 64) {
    uint32_t sel = 0;
    if (krw_copyout(cur + 0x28, &sel, sizeof(sel)) != 0)
      break;
    if (sel == (uint32_t)sprx_handle) {
      if (krw_copyout(cur, module_record, sizeof(module_record)) == 0) {
        found = 1;
      }
      break;
    }
    uintptr_t next = 0;
    if (krw_copyout(cur, &next, sizeof(next)) != 0 || next == cur)
      break;
    cur = next;
    count++;
  }
  if (!found)
    return 0;

  uintptr_t dispatch_kaddr = *(uintptr_t *)(module_record + 0x148);
  if (dispatch_kaddr == 0)
    return 0;

  uint8_t dispatch_table[0x120] = {0};
  if (krw_copyout(dispatch_kaddr, dispatch_table, sizeof(dispatch_table)) != 0)
    return 0;

  uint64_t kaddr_table = *(uint64_t *)(dispatch_table + 0x28);
  uint64_t table_size = *(uint64_t *)(dispatch_table + 0x30);
  uint64_t nid_strbase = *(uint64_t *)(dispatch_table + 0x38);
  uint64_t module_base = *(uint64_t *)(module_record + 0x30);

  if (kaddr_table == 0 || table_size == 0 || nid_strbase == 0)
    return 0;

  static uint8_t s_table_buf[32768];
  static uint8_t s_nid_buf[32768];

  size_t copy_table_sz = (table_size > sizeof(s_table_buf))
                             ? sizeof(s_table_buf)
                             : (size_t)table_size;
  if (krw_copyout((uintptr_t)kaddr_table, s_table_buf, copy_table_sz) != 0)
    return 0;
  if (krw_copyout((uintptr_t)nid_strbase, s_nid_buf, sizeof(s_nid_buf)) != 0)
    return 0;

  for (size_t off_ent = 0; off_ent + 0x18 <= copy_table_sz; off_ent += 0x18) {
    const uint8_t *entry = s_table_buf + off_ent;
    uint32_t off = *(const uint32_t *)entry;
    if (off + 12 > sizeof(s_nid_buf))
      continue;
    const char *nid_read = (const char *)(s_nid_buf + off);

    int matched = 1;
    for (int i = 0; i <= 10; i++) {
      if (nid[i] != nid_read[i]) {
        matched = 0;
        break;
      }
      if (nid[i] == '\0')
        break;
    }
    if (matched) {
      uint64_t func_offset = *(const uint64_t *)(entry + 0x08);
      if (func_offset == 0) {
        continue;
      }
      uint64_t func_vaddr = func_offset + module_base;
      klog_write_hex("krw_dynlib_resolve matched NID to: ", func_vaddr);
      return (uintptr_t)func_vaddr;
    }
  }
  return 0;
}

uintptr_t krw_dynlib_resolve_any(pid_t pid, const char *sname) {
  if (sname == NULL || sname[0] == '\0')
    return 0;
  char nid[12];
  if (sname[0] == '$') {
    size_t j = 0;
    while (j < 11 && sname[1 + j] != '\0') {
      nid[j] = sname[1 + j];
      j++;
    }
    nid[j] = '\0';
  } else {
    obs_compute_nid(sname, nid);
  }

  uintptr_t kproc = krw_get_proc(pid);
  if (kproc == 0)
    return 0;

  uintptr_t kaddr = 0;
  if (krw_copyout(kproc + 0x3E8, &kaddr, sizeof(kaddr)) != 0 || kaddr == 0)
    return 0;

  uintptr_t cur = 0;
  if (krw_copyout(kaddr, &cur, sizeof(cur)) != 0 || cur == 0)
    return 0;

  int count = 0;
  while (cur != 0 && count < 128) {
    uint32_t sel = 0;
    if (krw_copyout(cur + 0x28, &sel, sizeof(sel)) != 0)
      break;
    if (sel != 0) {
      uintptr_t addr = krw_dynlib_resolve(pid, (int)sel, nid);
      if (addr != 0) {
        return addr;
      }
    }
    uintptr_t next = 0;
    if (krw_copyout(cur, &next, sizeof(next)) != 0 || next == cur)
      break;
    cur = next;
    count++;
  }
  return 0;
}

static void sort_kexport_entries(obs_kexport_entry_t *arr, int low, int high) {
  if (low < high) {
    const char *pivot = arr[high].nid;
    int i = low - 1;
    for (int j = low; j < high; j++) {
      if (obs_strcmp(arr[j].nid, pivot) <= 0) {
        i++;
        obs_kexport_entry_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
      }
    }
    obs_kexport_entry_t tmp = arr[i + 1];
    arr[i + 1] = arr[high];
    arr[high] = tmp;
    int pi = i + 1;

    sort_kexport_entries(arr, low, pi - 1);
    sort_kexport_entries(arr, pi + 1, high);
  }
}

int krw_dump_all_exports(pid_t pid, obs_kexport_table_t *table) {
  if (table == NULL)
    return -1;
  table->count = 0;
  table->capacity = OBS_KEXPORT_MAX;

  if (pid <= 0) {
    pid = (pid_t)sys_call(SYS_getpid, 0, 0, 0, 0, 0, 0);
  }

  uintptr_t kproc = krw_get_proc(pid);
  if (kproc == 0)
    return -1;

  uintptr_t kaddr = 0;
  if (krw_copyout(kproc + 0x3E8, &kaddr, sizeof(kaddr)) != 0 || kaddr == 0)
    return -1;

  uintptr_t cur = 0;
  if (krw_copyout(kaddr, &cur, sizeof(cur)) != 0 || cur == 0)
    return -1;

  static uint8_t s_table_buf[32768];
  static uint8_t s_nid_buf[32768];

  int mod_count = 0;
  while (cur != 0 && mod_count < 128) {
    uint8_t module_record[0x180] = {0};
    if (krw_copyout(cur, module_record, sizeof(module_record)) != 0)
      break;

    uint32_t sel = 0;
    krw_copyout(cur + 0x28, &sel, sizeof(sel));
    uint64_t module_base = *(uint64_t *)(module_record + 0x30);
    uintptr_t dispatch_kaddr = *(uintptr_t *)(module_record + 0x148);

    if (dispatch_kaddr != 0 && module_base != 0) {
      uint8_t dispatch_table[0x120] = {0};
      if (krw_copyout(dispatch_kaddr, dispatch_table, sizeof(dispatch_table)) ==
          0) {
        uint64_t kaddr_table = *(uint64_t *)(dispatch_table + 0x28);
        uint64_t table_size = *(uint64_t *)(dispatch_table + 0x30);
        uint64_t nid_strbase = *(uint64_t *)(dispatch_table + 0x38);

        if (kaddr_table != 0 && table_size != 0 && nid_strbase != 0) {
          size_t copy_table_sz = (table_size > sizeof(s_table_buf))
                                     ? sizeof(s_table_buf)
                                     : (size_t)table_size;
          if (krw_copyout((uintptr_t)kaddr_table, s_table_buf, copy_table_sz) ==
                  0 &&
              krw_copyout((uintptr_t)nid_strbase, s_nid_buf,
                          sizeof(s_nid_buf)) == 0) {

            for (size_t off_ent = 0; off_ent + 0x18 <= copy_table_sz;
                 off_ent += 0x18) {
              if (table->count >= table->capacity)
                break;
              const uint8_t *entry = s_table_buf + off_ent;
              uint32_t off = *(const uint32_t *)entry;
              if (off + 12 > sizeof(s_nid_buf))
                continue;
              const char *nid_read = (const char *)(s_nid_buf + off);
              if (nid_read[0] == '\0')
                continue;

              uint64_t func_offset = *(const uint64_t *)(entry + 0x08);
              if (func_offset == 0)
                continue;

              obs_kexport_entry_t *out = &table->entries[table->count];
              memcpy(out->nid, nid_read, 11);
              out->nid[11] = '\0';
              out->handle = sel;
              out->vaddr = module_base + func_offset;
              table->count++;
            }
          }
        }
      }
    }

    uintptr_t next = 0;
    if (krw_copyout(cur, &next, sizeof(next)) != 0 || next == cur)
      break;
    cur = next;
    mod_count++;
  }

  if (table->count > 1) {
    sort_kexport_entries(table->entries, 0, (int)table->count - 1);
  }

  klog_write_hex("krw_dump_all_exports collected exports: ",
                 (uint64_t)table->count);
  return 0;
}
