/*
 * setjmp/longjmp, over the platform's own.
 *
 * **Implemented here, in assembly, and not imported from the platform.**
 *
 * This header first declared them as platform imports, on the reasoning that `setjmp`
 * cannot be written in C so this SDK should not try - the console carries a
 * FreeBSD-derived C library that has them, and `__error` binds that way already. That
 * was wrong for a reason that only shows up one step later: **`mkmodule` refuses an
 * imported symbol whose library the mined corpus cannot name**, because a module must
 * declare where each of its imports resolves. `setjmp` is not in that corpus. An import
 * nobody can attribute is not a dependency this SDK can ship, however certain we are
 * that the platform has it.
 *
 * So `src/system/libc.c` carries a global assembly block: save `rbx`, `rbp`,
 * `r12`-`r15`, the stack pointer and the return address, per the System V AMD64 ABI.
 * Eight quadwords. Nothing in it is platform-specific - it is the architecture's
 * published calling convention - and the result is a definition rather than a question.
 *
 * **libpng is what needs them.** Its error handling is `setjmp(png_jmpbuf(png_ptr))`,
 * written by every caller, including Neverball's `share/fs_png.c`. FreeType wants the
 * *type* far more widely - `ftstdlib.h` includes this header unconditionally for
 * `ft_jmp_buf` - but calls the functions only from its gzip module, which
 * `oops-deps/freetype` does not build.
 *
 * `jmp_buf` keeps FreeBSD's twelve-`long` size although only eight are used, so a
 * buffer allocated against this header stays the size anything else on this platform
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
