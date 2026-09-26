/*
 * Freestanding standard helpers.
 *
 * Zero libc dependency. Provides string length/comparison, integer/hex
 * formatting, and memory operations for freestanding targets (probe, injector,
 * payloads).
 */

#ifndef OOPS_FREESTD_H
#define OOPS_FREESTD_H

#include <stddef.h>
#include <stdint.h>

#if (defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 1) || defined(OOPS_HOST_BUILD) ||  \
    defined(OBSCENE_HOST_BUILD)
#include <string.h>
#include <sys/types.h>
#else
#ifndef _PID_T_DECLARED
typedef int32_t pid_t;
#define _PID_T_DECLARED
#endif

#ifndef _OFF_T_DECLARED
typedef int64_t off_t;
#define _OFF_T_DECLARED
#endif

#ifndef _SSIZE_T_DECLARED
typedef int64_t ssize_t;
#define _SSIZE_T_DECLARED
#endif

#ifdef __cplusplus
extern "C" {
#endif
void *memset(void *dest, int value, size_t len);
void *memcpy(void *dest, const void *src, size_t len);
int memcmp(const void *s1, const void *s2, size_t len);
#ifdef __cplusplus
}
#endif
#endif

#define OBS_NUM_MAX 24

#ifdef __cplusplus
extern "C" {
#endif

size_t obs_strlen(const char *s);
int obs_strcmp(const char *s1, const char *s2);
int obs_strncmp(const char *s1, const char *s2, size_t n);
char *obs_strncpy(char *dest, const char *src, size_t n);
char *obs_strstr(const char *haystack, const char *needle);

size_t obs_format_u64(char *dest, uint64_t value);
size_t obs_format_i64(char *dest, int64_t value);
size_t obs_format_hex(char *dest, uint64_t value);

void obs_compute_nid(const char *name, char out_nid[12]);

/*
 * The pieces a parser and a depth sort are built from, here rather than in
 * `src/system/libc.c` because that file is target-only and holds only renames, so an
 * algorithm there could not be tested on the host. `<libc/string.h>`'s `strspn`,
 * `strtok_r` and their kin, and `<libc/stdlib.h>`'s `qsort` and `bsearch`, are
 * one-line wrappers over these.
 */
size_t obs_strspn(const char *s, const char *accept);
size_t obs_strcspn(const char *s, const char *reject);
char *obs_strpbrk(const char *s, const char *accept);
char *obs_strtok_r(char *s, const char *delim, char **save);
/* `qsort`'s contract: not stable, so equal elements end in whatever order the partition
 * left. The stack is bounded at log2(count) frames (see the definition). */
void obs_qsort(void *base, size_t count, size_t size,
               int (*compare)(const void *, const void *));
void *obs_bsearch(const void *key, const void *base, size_t count, size_t size,
                  int (*compare)(const void *, const void *));

#include <stdarg.h>

int oops_vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int oops_snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* `<libc/stdio.h>`'s `sscanf` and `vsscanf`, defined in `src/system/scanf.c`, which
 * says what the conversion does not promise. */
int obs_vsscanf(const char *s, const char *fmt, va_list args);
int obs_sscanf(const char *s, const char *fmt, ...)
    __attribute__((format(scanf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif /* OOPS_FREESTD_H */
