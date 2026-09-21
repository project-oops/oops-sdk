/*
 * setjmp/longjmp, over the platform's own.
 *
 * **Declarations, not an implementation.** `setjmp` cannot be written in C - it has to save the
 * callee-saved registers, the stack pointer and the return address, which is architecture
 * assembly - and this SDK does not need to write it, because the console carries the
 * FreeBSD-derived C library that already has it. These bind at load exactly as `__error` and the
 * POSIX socket calls do.
 *
 * **`jmp_buf` is FreeBSD's amd64 layout and that is not ours to choose.** `_JBLEN` is 12 there,
 * so the buffer is twelve `long`s: the platform's `setjmp` writes it and the platform's
 * `longjmp` reads it, and a smaller one here would be a buffer overrun in somebody else's code.
 * Nothing in this SDK interprets the contents.
 *
 * It arrives because FreeType wants it: `ftstdlib.h` includes `<setjmp.h>` unconditionally for
 * `ft_jmp_buf`, and 15 of its sources failed on the missing header before reaching anything of
 * their own. Note the shape of that - the *type* is what every FreeType translation unit needs,
 * while the *functions* are called only from the gzip and LZW decompressors. A build that leaves
 * those modules out compiles against this header and never references the symbols.
 *
 * **These are not confirmed to bind on hardware.** `__error` was measured (obSCEne sweep
 * 20260909-083918); this pair has not been, and a caller that actually longjmps should want that
 * confirmed first. Declared here so the type is available and the gap is visible, rather than
 * left as a missing header that reads like an oversight.
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
