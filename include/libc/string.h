/*
 * <string.h> - the names a port's own code calls.
 *
 * `memset`, `memcpy` and `memcmp` were already here under their own names, because the compiler
 * emits calls to them whether a program writes them or not. The string half was not: this SDK
 * has `obs_strlen` and `obs_strcmp`, which nothing being ported calls.
 *
 * On the target include path only - see <libc/math.h> for why, and for what happens to a port
 * that gets an implicit declaration instead.
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
 * **The parser's half of <string.h>** (2026-09-20). A port that loads anything - an OBJ mesh, an
 * MTL material, a level file, a config - is built out of these four, and without them the loader
 * is the part of the port that has to be rewritten.
 *
 * `strtok` keeps its state in a static, as C says it does, which makes it the one function here
 * that a second caller can break; `strtok_r` is the reentrant form and is what new code should
 * use. Both are here because old code calls the first.
 */
char *strdup(const char *s);
char *strtok(char *s, const char *delim);
char *strtok_r(char *s, const char *delim, char **save);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
/* There is no errno here, so every code reads as one string: "unknown error". It exists because
 * a program that prints it needs it to link, not because it says anything. */
char *strerror(int errnum);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_STRING_H */
