/*
 * <math.h> - the names a port's own code calls.
 *
 * This SDK has had the functions for a long time, as `oops_sqrtf`, `oops_sinf` and the rest.
 * Nothing being ported calls them by those names: it calls `sqrtf`, and it includes <math.h> to
 * get it. Without this header the port does not compile - or, worse, compiles with an implicit
 * declaration and **links anyway**, because a payload link passes
 * `--unresolved-symbols=ignore-all` so the platform can resolve its own imports at load. The
 * failure then happens on the console, which is the most expensive place to find it.
 *
 * `oops-sdk/tools/libc-check` exists to stop exactly that: it calls every name declared here and
 * fails if any of them is undefined in the linked payload.
 *
 * **This is on the target include path only** (`include/libc`, added by the app build), never
 * the host's, so a host test that includes <math.h> gets the real one.
 *
 * The double forms are the float ones widened. A program doing double-precision numerics wants
 * more than this gives; a program calling `sqrt` on a coordinate does not, and that is what the
 * code being ported is doing.
 */
#ifndef OOPS_LIBC_MATH_H
#define OOPS_LIBC_MATH_H

/* C linkage for a C++ includer; `stdlib.h` carries the reasoning. The macros below are
   unaffected - only the declarations need it. */
#ifdef __cplusplus
extern "C" {
#endif

#include "oops/math.h"

#define M_PI    3.14159265358979323846
#define M_PI_2  1.57079632679489661923
#define M_PI_4  0.78539816339744830962
#define M_E     2.7182818284590452354
#define M_SQRT2 1.41421356237309504880

#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VAL  (__builtin_huge_val())
#define NAN       (__builtin_nanf(""))
#define INFINITY  (__builtin_inff())

float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float fmodf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float expf(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float powf(float base, float exp);
float hypotf(float x, float y);
float roundf(float x);
float truncf(float x);
float fminf(float a, float b);
float fmaxf(float a, float b);

double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
/* **`atan2l`, the one long-double function libc++ needs by name** (2026-09-25): `<complex>`'s
 * `std::arg(long double)` calls it, so any file that includes `<complex>` stops compiling without
 * it - OpenAL Soft's frequency-domain effects were the first. Computed in double: an 80-bit
 * argument loses its last eleven bits, which no caller here can see in an angle. */
long double atan2l(long double y, long double x);
double exp(double x);
double log(double x);
double log10(double x);
double pow(double base, double exp);
double hypot(double x, double y);
double round(double x);
double trunc(double x);

double sinh(double x);
float sinhf(float x);
double cosh(double x);
float coshf(float x);
double tanh(double x);
float tanhf(float x);
double asinh(double x);
float asinhf(float x);
double acosh(double x);
float acoshf(float x);
double atanh(double x);
float atanhf(float x);
double cbrt(double x);
float cbrtf(float x);
double log1p(double x);
float log1pf(float x);
double expm1(double x);
float expm1f(float x);

/*
 * **The round-to-nearest-integer family C99 requires beside `round`** (2026-09-21). `round` was
 * already here and rounds halves away from zero; these round to *even* on a tie, which is the
 * default IEEE mode and a different answer for exactly the inputs that matter.
 *
 * Found by libvorbis, which uses `rint` in five of its twenty sources - the decoder's floor and
 * psychoacoustic code, where round-half-to-even is the behaviour the format assumes.
 *
 * These are the compiler's builtins, which is what a hosted `<math.h>` expands them to, and
 * expressing them any other way here would be slower and no more correct. `nearbyint` differs
 * from `rint` only in whether it may raise the inexact flag, which nothing on this target reads.
 */
double rint(double x);
float rintf(float x);
double nearbyint(double x);
float nearbyintf(float x);
long lrint(double x);
long long llrint(double x);
double fmin(double a, double b);
double fmax(double a, double b);

/*
 * **The classification macros** (2026-09-20). A program that guards against a degenerate normal
 * or a division that went wrong writes `isnan(x)`, and until these existed that was a compile
 * error rather than a link one - so it was the first thing a port had to edit. They are the
 * compiler's own builtins, which is exactly what a hosted <math.h> expands them to.
 */
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define signbit(x)    __builtin_signbit(x)
#define isnormal(x)   __builtin_isnormal(x)

/*
 * **The five classification categories, and `fpclassify` over them** (2026-09-21). The macros
 * above answer one question each; C99 also requires the set they partition, and a caller that
 * wants to know *which* of the five a value is writes `fpclassify`.
 *
 * Found by libc++: its own `<math.h>` defines `fpclassify` for every floating type in terms of
 * `__builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)`, so a C++
 * standard library compiled against this header failed on five undeclared identifiers before
 * reaching anything of its own. The values are the ones every libc uses in this order; nothing
 * about them is ours to choose, and the builtin only needs them to be distinct.
 */
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define fpclassify(x)                                                          \
  __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)

/* The rest of what a port's own maths reaches for: the pieces that split or rebuild a float,
 * and log2's double form beside the float one that was already here. */
double log2(double x);
float copysignf(float mag, float sign);
double copysign(double mag, double sign);
float modff(float x, float *ipart);
double modf(double x, double *ipart);
float ldexpf(float x, int exp);
double ldexp(double x, int exp);
float frexpf(float x, int *exp);
double frexp(double x, int *exp);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_MATH_H */
