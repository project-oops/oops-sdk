/*
 * `<wchar.h>` for a platform whose C++ library has wide characters turned off.
 *
 * **This is here for one declaration, and the declaration is `struct tm`.** C requires
 * `<wchar.h>` to declare `tm` as an incomplete type, because `wcsftime` takes one - and libc++'s
 * `<cwchar>` relies on that: it does `using ::tm` with `using_if_exists`, which resolves to
 * "does not exist" when nothing has declared it yet. `<ctime>` is then included later, finds a
 * real `struct tm`, does its own `using ::tm`, and the two disagree about whether the name
 * exists. That presents as
 *
 *     error: target of using declaration conflicts with declaration already in scope
 *
 * a long way from anything to do with wide characters, and it blocked `ios.cpp`, `iostream.cpp`
 * and `ostream.cpp` - which is every stream (2026-09-23, `REQ-20260923T1810Z-7d42`).
 *
 * # Why this is a header and not a bigger decision
 *
 * `_LIBCPP_HAS_WIDE_CHARACTERS 0` is set in oops-apps' libc++ configuration and stays set: this
 * platform has one locale, characters are single-byte, and a wide-character API with no locale
 * behind it would be a surface with nothing to be right about. So the functions are deliberately
 * absent rather than stubbed. What is here is the types the standard puts in this header, so
 * that code which includes it compiles, and the `tm` declaration that libc++ needs.
 *
 * If a port ever genuinely needs `wcslen` and friends, they belong here - but the thing to
 * settle first is what locale they would be operating in, and today the answer is "the only
 * one".
 */
#ifndef OOPS_LIBC_WCHAR_H
#define OOPS_LIBC_WCHAR_H

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `wchar_t` is a keyword in C++ and a typedef in C. `stddef.h` supplies the C one. */

#ifndef WEOF
#  define WEOF ((wint_t)-1)
#endif

typedef int wint_t;

/*
 * `mbstate_t` tracks a partial multibyte character between calls. With single-byte characters
 * there is never a partial one, so every field is unused - but the type has to exist and has to
 * be copyable, because callers declare one on the stack and pass its address.
 *
 * **There is a second declaration of this type**, in oops-apps at
 * `src/oops-deps/libcxx/include/sys/_types/_mbstate_t.h`, which libc++ reaches by a different
 * include path. The two must stay identical in body *and* in guard: `_MBSTATE_T_DECLARED` is
 * FreeBSD's own name for it, so whichever header a translation unit sees first wins and the
 * other becomes a no-op.
 *
 * Getting that wrong is not subtle but it is well disguised - two structurally different
 * `mbstate_t` definitions produce `typedef redefinition with different types` in 21 of libc++'s
 * sources and nothing that names either file.
 */
#ifndef _MBSTATE_T_DECLARED
#  define _MBSTATE_T_DECLARED
typedef struct {
    unsigned int __state;
    unsigned int __bytes;
} mbstate_t;
#endif

/* The incomplete type this header exists for. `<time.h>` completes it. */
struct tm;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OOPS_LIBC_WCHAR_H */
