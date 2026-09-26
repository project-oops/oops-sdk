/*
 * <assert.h>.
 *
 * A failed assertion writes the file, line and expression to the kernel log and then
 * ends the payload as `abort` does (see <libc/stdlib.h>), so the reason is in the log.
 *
 * `NDEBUG` removes them, as everywhere. On the target include path only - see
 * <libc/math.h>.
 */
#ifndef OOPS_LIBC_ASSERT_H
#define OOPS_LIBC_ASSERT_H

/* C linkage for a C++ includer; `stdlib.h` carries the reasoning. */
#ifdef __cplusplus
extern "C" {
#endif

/* Declared whether or not NDEBUG removes the macro: the function is in the library
 * either way, and a translation unit that defines NDEBUG should still see its
 * prototype. */
void oops_assert_failed(const char *expr, const char *file, int line);

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) ((expr) ? (void)0 : oops_assert_failed(#expr, __FILE__, __LINE__))
#endif

/* C11 spells the compile-time assertion `_Static_assert` and defines this spelling of
 * it here, so a header that writes `static_assert` in C is asking for this file. C++
 * and C23 have it as a keyword, and NDEBUG does not remove it: it costs nothing at run
 * time. */
#if !defined(__cplusplus) && !defined(static_assert) && __STDC_VERSION__ < 202311L
#define static_assert _Static_assert
#endif

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_ASSERT_H */
