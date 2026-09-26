/*
 * setjmp/longjmp, implemented in assembly rather than imported from the platform:
 * `mkmodule` refuses an imported symbol whose library the mined corpus cannot name,
 * and `setjmp` is not in that corpus.
 *
 * `src/system/libc.c` carries a global assembly block that saves `rbx`, `rbp`,
 * `r12`-`r15`, the stack pointer and the return address, per the System V AMD64 ABI:
 * eight quadwords. libpng's error handling (`setjmp(png_jmpbuf(png_ptr))`) needs the
 * functions; FreeType's `ftstdlib.h` needs the type.
 *
 * `jmp_buf` keeps FreeBSD's twelve-`long` size although only eight are used, so a
 * buffer allocated against this header is the size anything else on this platform
 * expects.
 */
#ifndef OOPS_LIBC_SETJMP_H
#define OOPS_LIBC_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

#define _JBLEN 12
typedef long jmp_buf[_JBLEN];
typedef long sigjmp_buf[_JBLEN];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

int sigsetjmp(sigjmp_buf env, int savemask);
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_SETJMP_H */
