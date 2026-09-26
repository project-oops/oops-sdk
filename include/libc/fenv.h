/*
 * <fenv.h> - floating-point environment.
 */
#ifndef OOPS_LIBC_FENV_H
#define OOPS_LIBC_FENV_H

#ifdef __cplusplus
extern "C" {
#endif

#define FE_TONEAREST  0x0000
#define FE_DOWNWARD   0x0400
#define FE_UPWARD     0x0800
#define FE_TOWARDZERO 0x0c00

typedef int fenv_t;
typedef int fexcept_t;

static inline int fesetround(int r) { (void)r; return 0; }
static inline int fegetround(void)   { return FE_TONEAREST; }

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_FENV_H */
