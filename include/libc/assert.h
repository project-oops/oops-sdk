/*
 * <assert.h>.
 *
 * A failed assertion writes the file, line and expression to the kernel log and then
 * ends the payload, which is what `abort` can honestly do here - see <libc/stdlib.h>.
 * It is not silent: a port whose assertion fires on a console leaves the reason in the
 * log, which is the only place anyone will look.
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

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_ASSERT_H */
