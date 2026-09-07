/*
 * Freestanding standard helpers.
 *
 * Zero libc dependency. Provides string length/comparison, integer/hex formatting,
 * and memory operations for freestanding targets (probe, injector, payloads).
 */

#ifndef OOPS_FREESTD_H
#define OOPS_FREESTD_H

#include <stddef.h>
#include <stdint.h>

#if (defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 1) || defined(OOPS_HOST_BUILD) || defined(OBSCENE_HOST_BUILD)
#include <sys/types.h>
#include <string.h>
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

size_t obs_format_u64(char *dest, uint64_t value);
size_t obs_format_i64(char *dest, int64_t value);
size_t obs_format_hex(char *dest, uint64_t value);

void obs_compute_nid(const char *name, char out_nid[12]);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_FREESTD_H */
