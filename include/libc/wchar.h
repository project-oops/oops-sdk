/*
 * `<wchar.h>`: wide characters, for libc++'s `std::wstring` and the code built on it
 * (Luanti and its Irrlicht fork keep all their text as `wchar_t`).
 *
 * `wchar_t` is a Unicode scalar value (32 bits on this target) and the multibyte
 * encoding is UTF-8, in every function here: there is one locale, and it is UTF-8 for
 * conversions. `mbrtowc` carries an incomplete sequence across calls in `mbstate_t`,
 * and rejects overlong forms, surrogates and values past U+10FFFF with `EILSEQ`.
 *
 * Two approximations, both named where they live:
 * - `swprintf`/`vswprintf` and `wcsftime` narrow the format to UTF-8, format it with
 * the byte functions and decode the result. A `%ls` or `%lc` argument is therefore not
 *   converted - the byte formatter does not read wide strings - and a result longer
 * than 2 KiB fails.
 * - The stream functions (`fgetwc`, `fputwc`, ...) read and write UTF-8 on a byte
 * stream, and `ungetwc` pushes back only a character that is one byte in it.
 *
 * C requires `<wchar.h>` to declare `struct tm` as an incomplete type, because
 * `wcsftime` takes one, and libc++'s `<cwchar>` relies on it: its `using ::tm` with
 * `using_if_exists` resolves to "does not exist" when nothing has declared it, and
 * then conflicts with `<ctime>`'s `using ::tm`.
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

/* POSIX has `<wchar.h>` define `FILE` too, and libc++'s `<cwchar>` imports it: without
 * it here a translation unit that includes this before `<stdio.h>` records `::FILE` as
 * absent, and
 * `<cstdio>`'s import then conflicts. `<stdio.h>` completes it. */
#ifndef OOPS_FILE_DECLARED
#define OOPS_FILE_DECLARED
typedef struct oops_FILE FILE;
#endif

/* Conversion, UTF-8 each way. */
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
int mbsinit(const mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
wint_t btowc(int c);
int wctob(wint_t wc);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps);
size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len,
                  mbstate_t *ps);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps);
size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len,
                  mbstate_t *ps);

/* Strings and memory. Collation is code-point order, as `strcoll`'s is. */
size_t wcslen(const wchar_t *s);
wchar_t *wcscpy(wchar_t *d, const wchar_t *s);
wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcscat(wchar_t *d, const wchar_t *s);
wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int wcscoll(const wchar_t *a, const wchar_t *b);
size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
size_t wcsspn(const wchar_t *s, const wchar_t *accept);
size_t wcscspn(const wchar_t *s, const wchar_t *reject);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
wchar_t *wcsstr(const wchar_t *hay, const wchar_t *needle);
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n);

/* Numbers: the ASCII prefix converted by the byte functions. */
long wcstol(const wchar_t *s, wchar_t **end, int base);
unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base);
long long wcstoll(const wchar_t *s, wchar_t **end, int base);
unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base);
double wcstod(const wchar_t *s, wchar_t **end);
float wcstof(const wchar_t *s, wchar_t **end);
long double wcstold(const wchar_t *s, wchar_t **end);

/* Formatting: see the header comment for what is approximated. */
int swprintf(wchar_t *dst, size_t n, const wchar_t *fmt, ...);
int vswprintf(wchar_t *dst, size_t n, const wchar_t *fmt, va_list args);
size_t wcsftime(wchar_t *dst, size_t n, const wchar_t *fmt, const struct tm *t);

/* Streams, UTF-8 on the byte stream underneath. */
wint_t fgetwc(struct oops_FILE *f);
wint_t getwc(struct oops_FILE *f);
wint_t getwchar(void);
wint_t fputwc(wchar_t wc, struct oops_FILE *f);
wint_t putwc(wchar_t wc, struct oops_FILE *f);
wint_t putwchar(wchar_t wc);
wint_t ungetwc(wint_t wc, struct oops_FILE *f);
int fputws(const wchar_t *s, struct oops_FILE *f);
int fwide(struct oops_FILE *f, int mode);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OOPS_LIBC_WCHAR_H */
