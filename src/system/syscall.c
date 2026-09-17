/*
 * Freestanding Syscall Trampoline Implementation.
 *
 * Calls syscall instruction inside libkernel text segment.
 */

#include "oops/syscall.h"

static long s_ptr_syscall = 0;
static int *(*s_ptr_error)(void) = NULL;

#ifndef OOPS_HOST_BUILD
#include "oops/target.h"

static long find_libkernel_syscall_gadget(void) {
#if OOPS_TARGET_IS_PROSPERO
  /* PS5 Prospero: libkernel is mapped Execute-Only (XO). Dereferencing causes SYSTEM_XO_VIOLATION.
   * getpid is at 0x4e0; getpid + 0xa (0x4ea) is 'syscall; jb +1; ret'. */
  return (long)(0x800000000ULL + 0x4eaULL);
#else
  /* PS4 Orbis: getpid is at 0x5b0; getpid + 0xa (0x5ba) is 'syscall'. */
  return (long)(0x800000000ULL + 0x5baULL);
#endif
}
#endif

int sys_call_init(const payload_args_t *args) {
  if (args == NULL) {
#ifndef OOPS_HOST_BUILD
    s_ptr_syscall = find_libkernel_syscall_gadget();
    return 0;
#else
    return -1;
#endif
  }

  /* Shape check: args must be aligned and within canonical user space */
  unsigned long uargs = (unsigned long)args;
#ifndef OOPS_HOST_BUILD
  if (uargs == ~0UL) {
    __attribute__((weak)) long sceKernelWrite(int fd, const void *buf, unsigned long len);
    (void)sceKernelWrite(-1, (const void *)uargs, 0);
  }
#endif
  if (uargs < 0x10000UL || uargs >= 0x0000800000000000UL || (uargs & 0x7UL) != 0) {
#ifndef OOPS_HOST_BUILD
    /* Non-pointer or unaligned args passed from title entry point */
    s_ptr_syscall = find_libkernel_syscall_gadget();
    return 0;
#else
    return -1;
#endif
  }

  /* Callable check: a title loader hands off a struct where offset 0 is 0x2 (D324).
   * Refuse anything that is not a canonical callable address. */
  unsigned long dlsym_addr = (unsigned long)args->sys_dynlib_dlsym;
  if (dlsym_addr < 0x10000UL || dlsym_addr >= 0x0000800000000000UL) {
#ifndef OOPS_HOST_BUILD
    s_ptr_syscall = find_libkernel_syscall_gadget();
    return 0;
#else
    return -1;
#endif
  }

  /* Check if dynamic kexport_table was staged by the injector */
  uintptr_t uktable = (uintptr_t)args->kexport_table;
  if (uktable >= 0x10000UL && uktable < 0x0000800000000000UL && (uktable & 7) == 0) {
    const void *gptr = obs_kexport_lookup(args->kexport_table, "W0xkN0+ZkCE");
    if (gptr != NULL && (uintptr_t)gptr >= 0x10000UL) {
      s_ptr_syscall = (long)(uintptr_t)gptr + 0x0a;
      return 0;
    }
  }

  /* Start with args->sys_dynlib_dlsym which is guaranteed to be in libkernel
   * text */
  s_ptr_syscall = (long)args->sys_dynlib_dlsym;

  void *sym = NULL;
  if (args->sys_dynlib_dlsym(0x1, "getpid", &sym) == 0 && sym != NULL) {
    s_ptr_syscall = (long)sym;
  } else if (args->sys_dynlib_dlsym(0x2001, "getpid", &sym) == 0 &&
             sym != NULL) {
    s_ptr_syscall = (long)sym;
  }

  s_ptr_syscall += 0xa; /* Jump directly to syscall instruction */

  void *esym = NULL;
  if (args->sys_dynlib_dlsym(0x2001, "__error", &esym) == 0 && esym != NULL) {
    s_ptr_error = (int *(*)(void))esym;
  } else if (args->sys_dynlib_dlsym(0x1, "__error", &esym) == 0 &&
             esym != NULL) {
    s_ptr_error = (int *(*)(void))esym;
  }

  return 0;
}

static int s_last_errno = 0;

void sys_enable_direct(int enable) { (void)enable; }
int sys_get_errno(void) {
  if (s_last_errno != 0) {
    return s_last_errno;
  }
  if (s_ptr_error != NULL) {
    return *s_ptr_error();
  }
  return 0;
}

long sys_call(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
  long ret;
  register long r10_arg __asm__("r10") = a4;
  register long r8_arg __asm__("r8") = a5;
  register long r9_arg __asm__("r9") = a6;

  if (s_ptr_syscall == 0) {
#ifndef OOPS_HOST_BUILD
    s_ptr_syscall = find_libkernel_syscall_gadget();
#else
    return -1;
#endif
  }

  __asm__ volatile("movq %7, %%rax\n"
                   "movq %8, %%r10\n"
                   "callq *%9\n"
                   : "=a"(ret)
                   : "D"(a1), "S"(a2), "d"(a3), "r"(r10_arg), "r"(r8_arg),
                     "r"(r9_arg), "r"(num), "r"(a4), "r"(s_ptr_syscall)
                   : "rcx", "r11", "memory");
  return ret;
}
