/*
 * `<wchar.h>` for a platform whose C++ library has wide characters turned off.
 *
 * C requires `<wchar.h>` to declare `struct tm` as an incomplete type, because
 * `wcsftime` takes one, and libc++'s `<cwchar>` relies on it: its `using ::tm` with
 * `using_if_exists` resolves to "does not exist" when nothing has declared it, and
 * then conflicts with `<ctime>`'s `using ::tm` ("target of using declaration conflicts
 * with declaration already in scope"), which stops every stream source compiling.
 *
 * `_LIBCPP_HAS_WIDE_CHARACTERS 0` is set in oops-apps' libc++ configuration: there is
 * one locale and characters are single-byte, so the wide-character functions are
 * absent rather than stubbed. What is here is the types the standard puts in this
 * header, so code that includes it compiles, and the `tm` declaration.
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
#define WEOF ((wint_t) - 1)
#endif

typedef int wint_t;

/*
 * `mbstate_t` tracks a partial multibyte character between calls. With single-byte
 * characters there is never a partial one, so every field is unused - but the type has
 * to exist and has to be copyable, because callers declare one on the stack and pass
 * its address.
 *
 * There is a second declaration of this type, in oops-apps at
 * `src/oops-deps/libcxx/include/sys/_types/_mbstate_t.h`, which libc++ reaches by a
 * different include path. The two must stay identical in body and in guard
 * (`_MBSTATE_T_DECLARED`, FreeBSD's name), so whichever a translation unit sees first
 * wins; two different bodies give `typedef redefinition with different types` naming
 * neither file.
 */
#ifndef _MBSTATE_T_DECLARED
#define _MBSTATE_T_DECLARED
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
