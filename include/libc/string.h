/*
 * <string.h> - the names a port's own code calls.
 *
 * `memset`, `memcpy` and `memcmp` were already here under their own names, because the
 * compiler emits calls to them whether a program writes them or not. The string half
 * was not: this SDK has `obs_strlen` and `obs_strcmp`, which nothing being ported
 * calls.
 *
 * On the target include path only - see <libc/math.h> for why, and for what happens to
 * a port that gets an implicit declaration instead.
 */
#ifndef OOPS_LIBC_STRING_H
#define OOPS_LIBC_STRING_H

/* C linkage for a C++ includer; `stdlib.h` carries the reasoning. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

void *memset(void *dest, int value, size_t len);
void *memcpy(void *dest, const void *src, size_t len);
void *memmove(void *dest, const void *src, size_t len);
int memcmp(const void *a, const void *b, size_t len);
void *memchr(const void *s, int c, size_t len);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

/*
 * **The locale-aware pair, on a platform with one locale** (2026-09-23,
 * `REQ-20260923T1810Z-7d42`). libc++'s locale support calls both, so every stream and
 * every numeric facet needs them present.
 *
 * `strcoll` orders two strings by the current locale's collating sequence and `strxfrm`
 * turns a string into a form that `strcmp` orders the same way. In the "C" locale - the
 * only one here - the collating sequence *is* byte order, so `strcoll` is `strcmp` and
 * `strxfrm` is a bounded copy. Those are not simplifications: they are what the
 * standard specifies these two to do in this locale, which is why they can be
 * implementations rather than the loud refusals a verb with nothing behind it gets.
 */
int strcoll(const char *a, const char *b);
size_t strxfrm(char *dest, const char *src, size_t n);

/*
 * **The parser's half of <string.h>** (2026-09-20). A port that loads anything - an OBJ
 * mesh, an MTL material, a level file, a config - is built out of these four, and
 * without them the loader is the part of the port that has to be rewritten.
 *
 * `strtok` keeps its state in a static, as C says it does, which makes it the one
 * function here that a second caller can break; `strtok_r` is the reentrant form and is
 * what new code should use. Both are here because old code calls the first.
 */
char *strdup(const char *s);
char *strtok(char *s, const char *delim);
char *strtok_r(char *s, const char *delim, char **save);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
/* There is no errno here, so every code reads as one string: "unknown error". It exists
 * because a program that prints it needs it to link, not because it says anything. */
char *strerror(int errnum);
/* The reentrant one, in its XSI form - returns 0, or ERANGE when `buf` cannot hold the
   message. libc++'s `system_error.cpp` calls it unconditionally off Windows, so a
   target without it does not fail to link, it fails to *compile* the C++ standard
   library. */
int strerror_r(int errnum, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

/*
 * **FreeBSD's `<string.h>` includes `<strings.h>`, and so does this one when there is
 * one.**
 *
 * `strcasecmp`, `strncasecmp` and `ffs` are POSIX's, and POSIX puts them in
 * `<strings.h>`. FreeBSD then includes that header from this one under `__BSD_VISIBLE`,
 * which is the default - so on the system this target is derived from, `#include
 * <string.h>` really does declare `strcasecmp`, and a great deal of portable code
 * relies on it. macOS and glibc do the same.
 *
 * Without this, that code fails on an undeclared `strcasecmp` while looking at a header
 * that on every machine its author has ever used would have declared it. Bugdom's
 * `Bones.c` says
 * `#include <string.h> // strcasecmp` in as many words, and Pomme's bundled
 * `ghc::filesystem` calls `::strcasecmp` having included no such header at all.
 *
 * `__has_include`, for the reason `sys/types.h` gives where it reaches for
 * `<sys/select.h>`: these are POSIX rather than C, they come from a port layer -
 * `oops-apps/common/posix` - and this file is the freestanding C library, which titles
 * without that layer include on its own.
 */
#if defined(__has_include)
#if __has_include(<strings.h>)
#include <strings.h>
#endif
#endif

#endif /* OOPS_LIBC_STRING_H */
