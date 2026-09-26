/*
 * <strings.h> - case-insensitive string comparisons.
 */
#ifndef OOPS_LIBC_STRINGS_H
#define OOPS_LIBC_STRINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

/* The pre-POSIX memory pair <strings.h> is the home for. bcopy takes its arguments in the opposite
 * order to memcpy and handles overlap; a decompiled codebase that predates memcpy/memset uses both. */
void bcopy(const void *src, void *dst, size_t n);
void bzero(void *dst, size_t n);
int bcmp(const void *s1, const void *s2, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_STRINGS_H */
