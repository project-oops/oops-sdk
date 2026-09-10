/*
 * Freestanding Syscall Trampoline Implementation.
 *
 * Calls syscall instruction inside libkernel text segment.
 */

#include "oops/syscall.h"

static long s_ptr_syscall = 0;
static int *(*s_ptr_error)(void) = NULL;

int sys_call_init(const payload_args_t *args) {
  if (args == NULL || args->sys_dynlib_dlsym == NULL) {
    return -1;
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
    return -1;
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
