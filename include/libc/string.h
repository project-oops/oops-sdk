/*
 * <string.h> - the names a port's own code calls, and the `memset`, `memcpy` and
 * `memcmp` the compiler emits calls to whether a program writes them or not.
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
 * The locale-aware pair, which libc++'s locale support calls. In the "C" locale, the
 * only one here, the collating sequence is byte order, so `strcoll` is `strcmp` and
 * `strxfrm` is a bounded copy, exactly as the standard specifies for that locale.
 */
int strcoll(const char *a, const char *b);
size_t strxfrm(char *dest, const char *src, size_t n);

/*
 * The parser's half of <string.h>, what a port's file loaders are built from.
 * `strtok` keeps its state in a static, as C specifies, so a second caller can break
 * it; `strtok_r` is the reentrant form for new code.
 */
char *strdup(const char *s);
char *strtok(char *s, const char *delim);
char *strtok_r(char *s, const char *delim, char **save);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
/* Every code reads as one string, "unknown error"; it exists so a program that prints
 * it links. */
char *strerror(int errnum);
/* The reentrant one, in its XSI form: returns 0, or ERANGE when `buf` cannot hold the
   message. libc++'s `system_error.cpp` needs it to compile. */
int strerror_r(int errnum, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

/*
 * FreeBSD's `<string.h>` includes `<strings.h>` (under `__BSD_VISIBLE`, the default),
 * as macOS and glibc do, and portable code relies on it for `strcasecmp`; so does this
 * one when there is one. Conditional for the reason `sys/types.h` gives for
 * `<sys/select.h>`: these are POSIX, from a port layer (`oops-apps/common/posix`).
 */
#if defined(__has_include)
#if __has_include(<strings.h>)
#include <strings.h>
#endif
#endif

#endif /* OOPS_LIBC_STRING_H */
