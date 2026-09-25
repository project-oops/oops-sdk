/*
 * The C library a port expects, over the one this SDK already had.
 *
 * Every function here is a name change and nothing more: `sqrtf` is `oops_sqrtf`, `strlen` is
 * `obs_strlen`, `malloc` is `oops_malloc`. The reason it is worth a file is that a port does not
 * call the SDK's names, and the way that failure shows up is the expensive one - a payload link
 * passes `--unresolved-symbols=ignore-all`, so an unresolved `sqrtf` links silently and faults on
 * the console. `tools/libc-check` links a translation unit calling every one of these and fails
 * if any is undefined, which is the check that catches it here instead.
 *
 * **Target only.** On a host build the real C library provides all of this, and defining it again
 * would be a duplicate symbol; the headers live in `include/libc`, which only the target build
 * puts on the include path.
 */
#ifndef OOPS_HOST_BUILD

#include "libc/assert.h"
#include "libc/errno.h"
#include "libc/math.h"
#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/string.h"
#include "libc/time.h"
#include "oops/time.h"
#include "oops/freestd.h"
#include "oops/fs.h"
#include "oops/heap.h"
#include "oops/math.h"
#include "oops/syscall.h"
#include "oops/system.h"

/* ---------------------------------------------------------------------------
 * math
 * --------------------------------------------------------------------------- */

float sqrtf(float x) { return oops_sqrtf(x); }
float fabsf(float x) { return oops_fabsf(x); }
float floorf(float x) { return oops_floorf(x); }
float ceilf(float x) { return oops_ceilf(x); }
float fmodf(float x, float y) { return oops_fmodf(x, y); }
float sinf(float x) { return oops_sinf(x); }
float cosf(float x) { return oops_cosf(x); }
float tanf(float x) { return oops_tanf(x); }
float atan2f(float y, float x) { return oops_atan2f(y, x); }
float expf(float x) { return oops_expf(x); }
float logf(float x) { return oops_logf(x); }
float powf(float base, float exp_) { return oops_powf(base, exp_); }

float atanf(float x) { return oops_atan2f(x, 1.0f); }

/* asin and acos from atan2, which is the identity that needs no new series: asin(x) is the angle
 * whose sine is x, and sqrt(1 - x^2) is its cosine. Both clamp, because a caller that arrives
 * with 1.0000001 from its own arithmetic wants pi/2 and not a NaN. */
float asinf(float x) {
    if (x >= 1.0f) return (float)M_PI_2;
    if (x <= -1.0f) return -(float)M_PI_2;
    return oops_atan2f(x, oops_sqrtf(1.0f - x * x));
}

float acosf(float x) {
    if (x >= 1.0f) return 0.0f;
    if (x <= -1.0f) return (float)M_PI;
    return oops_atan2f(oops_sqrtf(1.0f - x * x), x);
}

float log2f(float x) { return oops_logf(x) * 1.44269504088896340736f; }
float log10f(float x) { return oops_logf(x) * 0.43429448190325182765f; }
float hypotf(float x, float y) { return oops_sqrtf(x * x + y * y); }

/* Away from zero at the half, which is what round() means and what floor(x + 0.5) does not do
 * for negatives. */
float roundf(float x) {
    return (x >= 0.0f) ? oops_floorf(x + 0.5f) : oops_ceilf(x - 0.5f);
}

float truncf(float x) { return (x >= 0.0f) ? oops_floorf(x) : oops_ceilf(x); }
float fminf(float a, float b) { return (a < b) ? a : b; }
float fmaxf(float a, float b) { return (a > b) ? a : b; }

/*
 * **The double forms are real doubles, and used not to be.**
 *
 * Each of these was the float kernel with the argument narrowed and the result widened, which
 * costs nothing a shader notices and breaks anything that reasons about its own precision: it
 * moves the smallest representable step from 1e-16 to 6e-8. Extreme Tux Racer's quaternion
 * interpolation guards a singularity at `1 - cosphi > 1e-13` and then divides by
 * `sin(acos(cosphi))` - correct for doubles, and through a float `acos` the argument rounds to
 * exactly 1, `acos` returns 0, and the divide is `0/0`. The NaN reached the course lookup and
 * faulted two layers below. `<oops/math.h>` carries the full account.
 *
 * `hypot`, `round` and `trunc` stay float-backed: nothing has needed their last digits, and this
 * note is here so the next person converting one knows the others are deliberate.
 */
double sqrt(double x) { return oops_sqrt(x); }
double fabs(double x) { return (x < 0.0) ? -x : x; }
double floor(double x) { return oops_floor(x); }
double ceil(double x) { return oops_ceil(x); }
double fmod(double x, double y) { return oops_fmod(x, y); }
double sin(double x) { return oops_sin(x); }
double cos(double x) { return oops_cos(x); }
double tan(double x) { return oops_tan(x); }
double asin(double x) { return oops_asin(x); }
double acos(double x) { return oops_acos(x); }
double atan(double x) { return oops_atan(x); }
double atan2(double y, double x) { return oops_atan2(y, x); }
double exp(double x) { return oops_exp(x); }
double log(double x) { return oops_ln(x); }
double log10(double x) { return oops_ln(x) * 0.43429448190325182765; }
double pow(double base, double exp_) { return oops_pow(base, exp_); }
double hypot(double x, double y) { return (double)hypotf((float)x, (float)y); }
double round(double x) { return (double)roundf((float)x); }
double trunc(double x) { return (double)truncf((float)x); }

/*
 * Round to nearest, **ties to even** - which is what separates these from `round` above, and
 * what libvorbis's floor and psychoacoustic code assumes.
 *
 * **`target("sse4.1")` is not an optimisation. Without it these called themselves.**
 *
 * The comment here used to say the builtins "lower to a single SSE4.1 instruction on this
 * target". They lower to `roundsd` only when the compiler is *told* it may emit SSE4.1, and the
 * baseline for `x86_64` is SSE2. With no instruction to use, clang lowered `__builtin_rint` the
 * only other way it can: a call to `rint` - from inside `rint`. Twenty-nine bytes of function
 * that recursed until the stack hit its guard page.
 *
 * Neverball found it on the console, one fault after the GL one: thread `SDLAudioP1`, writing
 * below `rsp`, and a backtrace that was the same address several hundred times. SDL's audio
 * resampler calls `rint` per sample.
 *
 * The attribute enables the feature for these functions alone, so the builtin has its
 * instruction and lowers inline. It is not a claim about the target beyond what the rest of this
 * SDK already assumes: oops-gl issues RDNA2 command streams to it, and there is no such console
 * without SSE4.1.
 *
 * `lrint` and `llrint` are left as they are and are not part of this: `cvtsd2si` is SSE2, takes
 * its rounding from MXCSR already, and is what they were compiling to all along.
 *
 * Unlike `round` and `trunc` beside them, these do not go through a `float` round trip: the
 * builtins have double forms, and narrowing to float first would change the answer for exactly
 * the values a round-to-nearest call is asked about.
 */
__attribute__((target("sse4.1"))) double rint(double x) { return __builtin_rint(x); }
__attribute__((target("sse4.1"))) float rintf(float x) { return __builtin_rintf(x); }
__attribute__((target("sse4.1"))) double nearbyint(double x) { return __builtin_nearbyint(x); }
__attribute__((target("sse4.1"))) float nearbyintf(float x) { return __builtin_nearbyintf(x); }
long lrint(double x) { return __builtin_lrint(x); }
long long llrint(double x) { return __builtin_llrint(x); }
double fmin(double a, double b) { return (a < b) ? a : b; }
double fmax(double a, double b) { return (a > b) ? a : b; }

double log2(double x) { return (double)log2f((float)x); }

/* **The sign moved, not copied by arithmetic.** `mag * (sign < 0 ? -1 : 1)` is wrong for a
 * negative zero, which is the one case a program calls copysign to get right - so the bit is
 * moved. The builtin is what a hosted <math.h> uses and it compiles to one instruction. */
float copysignf(float mag, float sign) { return __builtin_copysignf(mag, sign); }
double copysign(double mag, double sign) { return __builtin_copysign(mag, sign); }

/* The fractional part, with the integer part written out - and the fraction keeps x's sign, as
 * C says it does, so modff(-1.5) is -0.5 with -1 out. */
float modff(float x, float *ipart) {
    const float t = truncf(x);
    if (ipart) *ipart = t;
    return x - t;
}

double modf(double x, double *ipart) {
    float ip = 0.0f;
    const float frac = modff((float)x, &ip);
    if (ipart) *ipart = (double)ip;
    return (double)frac;
}

/*
 * x * 2^exp, and its inverse. Built out of the exponent field rather than out of `powf`, which
 * would round twice and lose the exactness that is the whole reason to call these.
 *
 * **Written out, for the reason given above `rint`.** These were `__builtin_ldexp` and
 * `__builtin_frexp`, and those have no inline lowering on *any* x86-64 - there is no instruction
 * to give them, so no `target` attribute rescues these the way it does the rounding four. Each
 * one was a call to itself. libpng's gamma tables and libvorbis both reach `frexp`.
 *
 * The scaling below is musl's `scalbn`: three steps rather than one, because a single
 * `2^exp` cannot be represented once `exp` leaves the exponent range, and doing it in stages
 * keeps every intermediate finite. Each step is an exact power of two, so the only rounding is
 * whatever the final multiply owes.
 */
typedef union { double d; uint64_t u; } oops_double_bits;

double ldexp(double x, int exp_) {
    double y = x;
    if (exp_ > 1023) {
        y *= 0x1p1023;
        exp_ -= 1023;
        if (exp_ > 1023) {
            y *= 0x1p1023;
            exp_ -= 1023;
            if (exp_ > 1023) exp_ = 1023;
        }
    } else if (exp_ < -1022) {
        /* Down in two steps of 2^-969, which stays normal, rather than one that would flush. */
        y *= 0x1p-1022 * 0x1p53;
        exp_ += 1022 - 53;
        if (exp_ < -1022) {
            y *= 0x1p-1022 * 0x1p53;
            exp_ += 1022 - 53;
            if (exp_ < -1022) exp_ = -1022;
        }
    }
    const oops_double_bits scale = { .u = (uint64_t)(0x3ff + exp_) << 52 };
    return y * scale.d;
}

double frexp(double x, int *exp_) {
    oops_double_bits b = { .d = x };
    int e = (int)((b.u >> 52) & 0x7ffu);

    if (e == 0) {
        /* Zero stays zero with an exponent of zero; a subnormal is scaled into the normal range
         * first and the borrowed exponent taken back off. */
        if (x == 0.0) {
            if (exp_) *exp_ = 0;
            return x;
        }
        b.d = x * 0x1p64;
        e = (int)((b.u >> 52) & 0x7ffu) - 64;
    } else if (e == 0x7ff) {
        if (exp_) *exp_ = 0;     /* infinity and NaN come back unchanged */
        return x;
    }

    if (exp_) *exp_ = e - 1022;
    b.u = (b.u & ~(0x7ffULL << 52)) | ((uint64_t)1022 << 52);
    return b.d;   /* the significand, in [0.5, 1) with x's sign */
}

/* **The float forms go through the double ones, and that is exact rather than convenient.**
 * Every `float` is a `double`, `frexp`'s result has a `float`'s significand so narrowing it
 * loses nothing, and `ldexp` on a double cannot overflow for any exponent a `float` result can
 * survive - so the narrowing at the end is the only rounding, which is the one a correct
 * `ldexpf` performs anyway. */
float ldexpf(float x, int exp_) { return (float)ldexp((double)x, exp_); }

float frexpf(float x, int *exp_) { return (float)frexp((double)x, exp_); }

/* ---------------------------------------------------------------------------
 * string
 * --------------------------------------------------------------------------- */

size_t strlen(const char *s) { return obs_strlen(s); }
int strcmp(const char *a, const char *b) { return obs_strcmp(a, b); }
int strncmp(const char *a, const char *b, size_t n) { return obs_strncmp(a, b, n); }
char *strncpy(char *dest, const char *src, size_t n) { return obs_strncpy(dest, src, n); }

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0') {
    }
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest + obs_strlen(dest);
    while ((*d++ = *src++) != '\0') {
    }
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest + obs_strlen(dest);
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

char *strchr(const char *s, int c) {
    const char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char *)(size_t)s;
        if (!*s) return (char *)0;
    }
}

char *strrchr(const char *s, int c) {
    const char ch = (char)c;
    const char *last = (const char *)0;
    for (;; s++) {
        if (*s == ch) last = s;
        if (!*s) break;
    }
    return (char *)(size_t)last;
}

char *strstr(const char *haystack, const char *needle) {
    return obs_strstr(haystack, needle);
}

/* The "C" locale's collating sequence is byte order, so this is `strcmp` - see `<libc/string.h>`
 * for why that is the specified behaviour here rather than a shortcut. */
int strcoll(const char *a, const char *b) { return strcmp(a, b); }

/* `strxfrm` transforms `src` so that `strcmp` on the results orders the same way `strcoll`
 * orders the originals. With `strcoll` being `strcmp`, the transform is the identity and this is
 * a bounded copy.
 *
 * **The return is the length of the transform, not of what was copied**, and it excludes the
 * terminator. Callers size a buffer by calling with `n == 0` and a null `dest`, then calling
 * again - so returning the copied count would make the first call report 0 and the caller
 * allocate nothing. Nothing is written when `n` is 0, which is what makes the sizing call safe.
 */
size_t strxfrm(char *dest, const char *src, size_t n) {
    const size_t len = strlen(src);

    if (n != 0u) {
        const size_t copy = (len < n - 1u) ? len : n - 1u;
        for (size_t i = 0u; i < copy; i++) dest[i] = src[i];
        dest[copy] = '\0';
    }

    return len;
}

/* Overlap-safe, which is the whole difference from memcpy: a caller that meant memmove and got a
 * forward copy loses data only when the regions overlap, which is the case it used memmove for. */
void *memmove(void *dest, const void *src, size_t len) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || len == 0u) return dest;
    if (d < s) {
        for (size_t i = 0; i < len; i++) d[i] = s[i];
    } else {
        for (size_t i = len; i-- > 0;) d[i] = s[i];
    }
    return dest;
}

void *memchr(const void *s, int c, size_t len) {
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char ch = (unsigned char)c;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == ch) return (void *)(size_t)(p + i);
    }
    return (void *)0;
}

size_t strspn(const char *s, const char *accept) { return obs_strspn(s, accept); }
size_t strcspn(const char *s, const char *reject) { return obs_strcspn(s, reject); }
char *strpbrk(const char *s, const char *accept) { return obs_strpbrk(s, accept); }

char *strdup(const char *s) {
    if (!s) return (char *)0;
    const size_t n = strlen(s) + 1u;
    char *copy = (char *)oops_malloc(n);
    if (!copy) return (char *)0;
    memcpy(copy, s, n);
    return copy;
}

char *strtok_r(char *s, const char *delim, char **save) {
    return obs_strtok_r(s, delim, save);
}

/* **The static is C's, not an oversight.** `strtok` has kept its place between calls since
 * 1989, which is why `strtok_r` exists; a port calling the first from two places at once has
 * the bug it would have anywhere. */
static char *libc_strtok_save;

char *strtok(char *s, const char *delim) { return strtok_r(s, delim, &libc_strtok_save); }

/* There is no errno here - the SDK's calls return their own codes - so every number reads the
 * same. It exists so that a program which prints it links. */
char *strerror(int errnum) {
    (void)errnum;
    return (char *)(size_t) "unknown error";
}

/* ---------------------------------------------------------------------------
 * stdlib
 * --------------------------------------------------------------------------- */

void *malloc(size_t size) { return oops_malloc(size); }
void *calloc(size_t count, size_t size) { return oops_calloc(count, size); }
void *realloc(void *ptr, size_t size) { return oops_realloc(ptr, size); }
void *aligned_alloc(size_t alignment, size_t size) { return oops_aligned_alloc(alignment, size); }
void free(void *ptr) { oops_free(ptr); }

int abs(int x) { return (x < 0) ? -x : x; }
long labs(long x) { return (x < 0) ? -x : x; }

long strtol(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');
    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0') { base = 8; p++; }
        else base = 10;
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    long v = 0;
    for (;; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)(size_t)p;
    return neg ? -v : v;
}

double strtod(const char *s, char **end) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');
    double v = 0.0;
    while (*p >= '0' && *p <= '9') v = v * 10.0 + (double)(*p++ - '0');
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9') {
            v += (double)(*p++ - '0') * scale;
            scale *= 0.1;
        }
    }
    if (*p == 'e' || *p == 'E') {
        const char *mark = p;
        char *ee = (char *)0;
        const long n = strtol(p + 1, &ee, 10);
        if (ee && ee != p + 1) {
            for (long i = 0; i < n; i++) v *= 10.0;
            for (long i = 0; i > n; i--) v *= 0.1;
            p = (const char *)ee;
        } else {
            p = mark;
        }
    }
    if (end) *end = (char *)(size_t)p;
    return neg ? -v : v;
}

int atoi(const char *s) { return (int)strtol(s, (char **)0, 10); }
long atol(const char *s) { return strtol(s, (char **)0, 10); }
double atof(const char *s) { return strtod(s, (char **)0); }

/* `strtoul` over `strtol`: this SDK's `long` and `unsigned long` are both 64 bits, so the digits
 * a value needs fit either way and the cast is the whole of the difference. A leading minus is
 * still accepted and still negates, which is what C says unsigned conversion does. */
unsigned long strtoul(const char *s, char **end, int base) {
    return (unsigned long)strtol(s, end, base);
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }

/*
 * The C99 names beside the `long` forms above. `long` is 64 bits on this target, so the integer
 * pair is the same conversion under a different spelling and not a second parser to keep
 * correct.
 *
 * `strtold` widens a double, which is lossy - `long double` is 80-bit here and this parses at 53
 * bits of mantissa. It is the honest cheap answer: a real 80-bit parser is a different piece of
 * work, and no caller in the collection needs that precision. `include/libc/stdlib.h` says so
 * where a caller reads it.
 */
long long strtoll(const char *s, char **end, int base) {
  return (long long)strtol(s, end, base);
}

unsigned long long strtoull(const char *s, char **end, int base) {
  return (unsigned long long)strtoul(s, end, base);
}

long double strtold(const char *s, char **end) { return (long double)strtod(s, end); }

/*
 * No environment block, so every name is unset. See `include/libc/stdlib.h` for why that is the
 * honest answer and not a placeholder - a payload is launched rather than spawned, and NULL is
 * what a caller would get from a shell that exported nothing.
 */
/* **The environment starts empty and a payload may fill it.**
 *
 * Nothing is inherited - that part of the old note is still true and is why this begins with no
 * entries. What changed on 2026-09-23 is that `setenv` now keeps what it is given instead of
 * dropping it, because there is one variable a payload genuinely knows and a POSIX program
 * genuinely needs: `HOME`. A title mounts its savedata, which is the only writable directory it
 * has, and exports the mount point; ports then find it the way they already look for it, with
 * no patch to their own sources. Neverball's `pick_home_path` asks `getenv("HOME")` and falls
 * back to the read-only package directory, so with this unset it asked for a player name on
 * every launch and correctly failed to keep it.
 *
 * Fixed storage rather than the heap: this runs before a title's own initialisation and is not
 * worth a malloc that has to be got right during start-up. Sixteen names is far more than the
 * handful a port sets, and a name that does not fit is refused rather than truncated - a
 * silently shortened `HOME` would be a path to somewhere else. */
#define OOPS_ENV_MAX      16
#define OOPS_ENV_NAME_MAX 32
#define OOPS_ENV_VAL_MAX  192

static char s_env_name[OOPS_ENV_MAX][OOPS_ENV_NAME_MAX];
static char s_env_val[OOPS_ENV_MAX][OOPS_ENV_VAL_MAX];
static int s_env_used[OOPS_ENV_MAX];

static int oops_env_find(const char *name) {
  for (int i = 0; i < OOPS_ENV_MAX; i++) {
    if (!s_env_used[i]) continue;
    const char *a = s_env_name[i];
    const char *b = name;
    while (*a && *a == *b) { a++; b++; }
    if (*a == '\0' && *b == '\0') return i;
  }
  return -1;
}

char *getenv(const char *name) {
  if (!name || name[0] == '\0') return (char *)0;
  const int at = oops_env_find(name);
  return at >= 0 ? s_env_val[at] : (char *)0;
}

int setenv(const char *name, const char *value, int overwrite) {
  if (!name || name[0] == '\0' || !value) return -1;
  /* An '=' in a name is what separates a name from a value everywhere else, so a name carrying
     one could never be looked up again. */
  for (const char *p = name; *p; p++) {
    if (*p == '=') return -1;
  }

  int at = oops_env_find(name);
  if (at >= 0 && !overwrite) return 0;
  if (at < 0) {
    for (int i = 0; i < OOPS_ENV_MAX && at < 0; i++) {
      if (!s_env_used[i]) at = i;
    }
    if (at < 0) return -1; /* full */
  }

  size_t n = 0;
  while (name[n]) n++;
  size_t v = 0;
  while (value[v]) v++;
  if (n >= OOPS_ENV_NAME_MAX || v >= OOPS_ENV_VAL_MAX) return -1;

  for (size_t i = 0; i <= n; i++) s_env_name[at][i] = name[i];
  for (size_t i = 0; i <= v; i++) s_env_val[at][i] = value[i];
  s_env_used[at] = 1;
  return 0;
}

int unsetenv(const char *name) {
  if (!name || name[0] == '\0') return -1;
  const int at = oops_env_find(name);
  if (at >= 0) {
    s_env_used[at] = 0;
    s_env_name[at][0] = '\0';
    s_env_val[at][0] = '\0';
  }
  return 0;
}

long long llabs(long long x) { return (x < 0) ? -x : x; }

div_t div(int num, int den) {
    div_t r;
    r.quot = den ? num / den : 0;
    r.rem = den ? num % den : 0;
    return r;
}

ldiv_t ldiv(long num, long den) {
    ldiv_t r;
    r.quot = den ? num / den : 0;
    r.rem = den ? num % den : 0;
    return r;
}

/* Sorting and searching. The algorithms are `obs_qsort` and `obs_bsearch` in
 * `src/system/freestd.c`, which builds on the host too, so `test_freestd.c` can run them - this
 * file cannot be tested at all. See `<libc/stdlib.h>` for why a GL 1.x port needs a sort. */
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *)) {
    obs_qsort(base, count, size, compare);
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *)) {
    return obs_bsearch(key, base, count, size, compare);
}

/* The usual linear congruential generator, seeded 1 unless srand says otherwise - see
 * <libc/stdlib.h> for what it is and is not for. */
static unsigned long s_rand_state = 1ul;

int rand(void) {
    s_rand_state = s_rand_state * 6364136223846793005ul + 1442695040888963407ul;
    return (int)((s_rand_state >> 33) & 0x7ffffffful);
}

void srand(unsigned int seed) { s_rand_state = seed; }

/*
 * `exit` and `abort` end the process, through the platform's own exit - there is nothing else
 * they can honestly do. A payload is called, not spawned, so there is no `main` to return
 * through and no atexit list to run; a program that wanted its own cleanup should have done it
 * before calling these, as it should on any system.
 *
 * Neither returns. If the syscall does not resolve - it is weak, like every platform import -
 * the loop below is what is left, and a program that has asked to stop stopping is better than
 * one that carries on with a state it declared unusable.
 *
 * # Why they say so first
 *
 * **On this platform the exit syscall does not end the process, it raises `SIGSYS`** - obSCEne
 * `REQ-20260917T1450Z-2e71` established that a `big-app` container cannot terminate itself at
 * all, because lifecycle belongs to the shell. So every `exit` here becomes a fatal signal with
 * a register dump and no sentence, and every `abort` becomes the same one function further away.
 *
 * That cost a diagnosis on 2026-09-23. `cxx-throw` died mid-probe with `signal: 12 (SIGSYS)` and
 * a one-frame backtrace, and the only way to learn that the frame was *inside `exit`* was to
 * resolve the address against the link map by hand. A library that aborts on purpose - libc++abi
 * does, on any uncaught exception - was indistinguishable from one that faulted by accident.
 *
 * So both say what they are and who asked, before the syscall that will not work. It costs one
 * klog line on a path that was about to end the program anyway, and it turns "a fatal signal
 * somewhere" into "this program called abort". The loop still follows, because a title that has
 * declared itself unusable should stop rather than continue - and parking is also what makes the
 * line above survive to be read.
 */
void exit(int status) {
    oops_klog("libc", "exit called; this container cannot terminate itself, so it parks instead");
    sys_call(SYS_exit, (long)(unsigned int)status, 0, 0, 0, 0, 0);
    for (;;) {
    }
}

void abort(void) {
    oops_klog("libc", "abort called - something gave up deliberately, look above for why");
    exit(1);
}

/* ---------------------------------------------------------------------------
 * errno
 *
 * The platform carries the FreeBSD-derived POSIX exports, and errno lives behind `__error()`
 * there as it does on FreeBSD - `src/net/net.c` has read it that way since the socket work, and
 * obSCEne measured the import callable on firmware 12.40 (sweep 20260909-083918).
 *
 * The import is weak, like every platform call here, so it is checked before it is used. The
 * fallback is one process-wide slot rather than a failure: `errno` is read far more often than
 * it is set, and a caller doing `if (errno == ENOENT)` after a failed call must not fault on a
 * console where the import did not bind. A single slot is wrong only for a program reading
 * errno set by another thread, which is already a race on any system.
 */
__attribute__((weak)) int *__error(void);

static int s_errno_fallback;

int *oops_errno_location(void) {
  if (__error) {
    return __error();
  }
  return &s_errno_fallback;
}

/* ---------------------------------------------------------------------------
 * stdio
 *
 * `stdout` and `stderr` have no descriptor: they are the kernel log, and a write to them is
 * buffered until a newline or a flush so that a line arrives as a line. Everything else is a
 * descriptor from oops_fs_open.
 * --------------------------------------------------------------------------- */

/* The trailing -1 is `pushback`, meaning empty. It is spelled out rather than left to the
 * initialiser's implicit zero because zero is a valid character and would be handed to the first
 * read as a stray NUL. */
/* The three write-buffer fields trail each of these as null, zero, zero and unbuffered: these
 * are log streams, and `fwrite` sends a log stream to `libc_log_write` before the buffer is ever
 * reached. */
static FILE s_stdout = {-1, 0, 0, 1, -1, 0, 0u, 0u, 1};
static FILE s_stderr = {-1, 0, 0, 2, -1, 0, 0u, 0u, 1};
static FILE s_stdin = {-1, 1, 0, 0, -1, 0, 0u, 0u, 1}; /* nothing to read; at EOF from the start */
FILE *stdout = &s_stdout;
FILE *stderr = &s_stderr;
FILE *stdin = &s_stdin;

/* One line's worth per sink. A longer line is flushed in pieces, which the log shows as more
 * than one line - the alternative is dropping the tail, and a truncated diagnostic is worse
 * than a split one. */
static char s_log_buf[2][256];
static size_t s_log_len[2];

static void libc_log_flush(int sink) {
    const int i = (sink == 2) ? 1 : 0;
    if (s_log_len[i] == 0u) return;
    s_log_buf[i][s_log_len[i]] = '\0';
    oops_klog((sink == 2) ? "stderr" : "stdout", s_log_buf[i]);
    s_log_len[i] = 0u;
}

static void libc_log_putc(int sink, char c) {
    const int i = (sink == 2) ? 1 : 0;
    if (c == '\n') {
        libc_log_flush(sink);
        return;
    }
    if (s_log_len[i] + 2u >= sizeof(s_log_buf[i])) libc_log_flush(sink);
    s_log_buf[i][s_log_len[i]++] = c;
}

static void libc_log_write(int sink, const char *s, size_t n) {
    for (size_t k = 0; k < n; k++) libc_log_putc(sink, s[k]);
}

/* `remove` was already here, a few lines down; this is its missing partner. */
int rename(const char *from, const char *to) { return oops_fs_rename(from, to); }

/*
 * **The write buffer, because a write on this platform is a syscall.**
 *
 * See `<libc/stdio.h>` for what it cost not to have one: a serialiser writing a field at a time
 * turns one frame into thousands of two-to-four-byte syscalls, measured at 2.3 seconds inside a
 * single frame of Neverball's replay recording, while the physics it was blamed on took 0ms.
 *
 * 4 KiB, allocated on the stream's first buffered write and never grown - a caller writing more
 * than that in one go is already writing in blocks and is passed straight through, which also
 * keeps a large `fwrite` from being copied twice.
 */
#define LIBC_WBUF_CAP 4096u

/* Empties the buffer to the descriptor. 0 on success, EOF on a failed write - and the buffer is
 * dropped either way, because retrying it later would write it at the wrong offset. */
static int libc_wbuf_flush(FILE *f) {
    if (!f || !f->wbuf || f->wbuf_len == 0u) return 0;
    const unsigned int n = f->wbuf_len;
    f->wbuf_len = 0u;
    if (oops_fs_write(f->fd, f->wbuf, n) < 0) {
        f->err = 1;
        return EOF;
    }
    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) return (FILE *)0;
    int flags = OOPS_O_RDONLY;
    if (mode[0] == 'w') flags = OOPS_O_WRONLY | OOPS_O_CREAT | OOPS_O_TRUNC;
    else if (mode[0] == 'a') flags = OOPS_O_WRONLY | OOPS_O_CREAT | OOPS_O_APPEND;
    /* The '+' of "r+", "w+" and "a+" asks for both directions. */
    for (const char *m = mode; *m; m++) {
        if (*m == '+') flags = (flags & ~(OOPS_O_RDONLY | OOPS_O_WRONLY)) | OOPS_O_RDWR;
    }
    const int fd = oops_fs_open(path, flags, 0666);
    if (fd < 0) return (FILE *)0;
    FILE *f = (FILE *)oops_malloc(sizeof(FILE));
    if (!f) {
        oops_fs_close(fd);
        return (FILE *)0;
    }
    f->fd = fd;
    f->eof = 0;
    f->err = 0;
    f->is_log = 0;
    f->pushback = -1; /* empty; zero is a valid character, so it cannot be the empty value */
    f->wbuf = (unsigned char *)0; /* allocated on the first buffered write, not on every open */
    f->wbuf_len = 0u;
    f->wbuf_cap = 0u;
    f->nobuf = 0;
    return f;
}

/* Wraps a descriptor the caller already opened. See `<libc/stdio.h>` for why `mode` is ignored
 * rather than parsed. */
FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    if (fd < 0) return (FILE *)0;
    FILE *f = (FILE *)oops_malloc(sizeof(FILE));
    if (!f) return (FILE *)0;
    f->fd = fd;
    f->eof = 0;
    f->err = 0;
    f->is_log = 0;
    f->pushback = -1;
    f->wbuf = (unsigned char *)0;
    f->wbuf_len = 0u;
    f->wbuf_cap = 0u;
    f->nobuf = 0;
    return f;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    if (f->is_log) {
        libc_log_flush(f->is_log);
        return 0;
    }
    /* Whatever is buffered belongs in the file before the descriptor goes. */
    const int flushed = libc_wbuf_flush(f);
    if (f->wbuf) oops_free(f->wbuf);
    const int rc = oops_fs_close(f->fd);
    oops_free(f);
    return (rc < 0 || flushed != 0) ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *f) {
    if (!f || f->is_log || !ptr || size == 0u || count == 0u) return 0u;

    /* A read has to see what this stream has written. The descriptor's offset is behind by
       whatever is buffered, so reading before it lands would return the bytes it is about to
       overwrite - which only bites a stream opened "r+" or "w+", and silently. */
    if (libc_wbuf_flush(f) != 0) return 0u;

    /* A pushed-back character belongs to the stream, so it is delivered here too and not only
     * from `fgetc`. Taking it shortens this read by one byte and the loop above the caller asks
     * again, which is simpler than splicing it into the descriptor read and cannot get the
     * count wrong. */
    if (f->pushback >= 0) {
        *(unsigned char *)ptr = (unsigned char)f->pushback;
        f->pushback           = -1;

        if (size * count == 1u) return 1u / size;

        const int64_t rest = oops_fs_read(f->fd, (unsigned char *)ptr + 1, size * count - 1u);
        if (rest < 0) {
            f->err = 1;
            return 1u / size;
        }
        if ((size_t)rest + 1u < size * count) f->eof = 1;
        return ((size_t)rest + 1u) / size;
    }

    const int64_t got = oops_fs_read(f->fd, ptr, size * count);
    if (got < 0) {
        f->err = 1;
        return 0u;
    }
    if ((size_t)got < size * count) f->eof = 1;
    return (size_t)got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t count, FILE *f) {
    if (!f || !ptr || size == 0u || count == 0u) return 0u;
    if (f->is_log) {
        libc_log_write(f->is_log, (const char *)ptr, size * count);
        return count;
    }

    const size_t bytes = size * count;

    /* A block-sized write goes straight out, behind whatever is already buffered so the file
       keeps the order the caller wrote in. */
    if (f->nobuf || bytes >= LIBC_WBUF_CAP) {
        if (libc_wbuf_flush(f) != 0) return 0u;
        const int64_t put = oops_fs_write(f->fd, ptr, bytes);
        if (put < 0) {
            f->err = 1;
            return 0u;
        }
        return (size_t)put / size;
    }

    if (!f->wbuf) {
        f->wbuf = (unsigned char *)oops_malloc(LIBC_WBUF_CAP);
        if (!f->wbuf) {
            /* No buffer is a slow stream, not a broken one. */
            const int64_t put = oops_fs_write(f->fd, ptr, bytes);
            if (put < 0) { f->err = 1; return 0u; }
            return (size_t)put / size;
        }
        f->wbuf_cap = LIBC_WBUF_CAP;
        f->wbuf_len = 0u;
    }

    if (f->wbuf_len + bytes > f->wbuf_cap) {
        if (libc_wbuf_flush(f) != 0) return 0u;
    }

    const unsigned char *src = (const unsigned char *)ptr;
    for (size_t i = 0u; i < bytes; i++) {
        f->wbuf[f->wbuf_len + i] = src[i];
    }
    f->wbuf_len += (unsigned int)bytes;
    return count;
}

int fseek(FILE *f, long offset, int whence) {
    if (!f || f->is_log) return -1;
    /* Buffered bytes are not in the file yet, so a seek before they land would put them at the
       new offset instead of the one they were written at. */
    if (libc_wbuf_flush(f) != 0) return -1;
    f->eof = 0;
    return (oops_fs_seek(f->fd, (int64_t)offset, whence) < 0) ? -1 : 0;
}

long ftell(FILE *f) {
    if (!f || f->is_log) return -1L;
    /* The descriptor's offset is behind the caller's by whatever is still buffered, and `ftell`
       has to answer where the *stream* is - Neverball's demo header rewrite depends on it. */
    return (long)oops_fs_tell(f->fd) + (long)f->wbuf_len;
}

/* `long` is 64 bits on this target, so these carry no more range than the pair above - see
 * `<libc/stdio.h>` for why the names exist anyway. A pushback is discarded by a seek, because
 * the character it held came from somewhere the stream is no longer positioned. */
int fseeko(FILE *f, off_t offset, int whence) {
    if (f) f->pushback = -1;
    return fseek(f, (long)offset, whence);
}

off_t ftello(FILE *f) { return (off_t)ftell(f); }

void rewind(FILE *f) { (void)fseek(f, 0L, SEEK_SET); }
int feof(FILE *f) { return f ? f->eof : 1; }
int ferror(FILE *f) { return f ? f->err : 1; }

int fflush(FILE *f) {
    if (f && f->is_log) libc_log_flush(f->is_log);
    else if (!f) { libc_log_flush(1); libc_log_flush(2); } /* fflush(NULL): all of them */
    else if (f) return libc_wbuf_flush(f);
    return 0;
}

int fgetc(FILE *f) {
    /* Checked here as well as in `fread`, because `fread` refuses a log stream outright and
     * `ungetc` on `stdin` has to work regardless of what is underneath it. */
    if (f && f->pushback >= 0) {
        const int c = f->pushback;
        f->pushback = -1;
        return c;
    }
    unsigned char c;
    if (fread(&c, 1u, 1u, f) != 1u) return EOF;
    return (int)c;
}

int getc(FILE *f) { return fgetc(f); }

/* Nothing to unbuffer - see `<libc/stdio.h>`. `setbuf` is a no-op because the stream is already
 * what it is asking for; `setvbuf` accepts `_IONBF` and refuses the buffered modes rather than
 * accepting them and ignoring them. */
void setbuf(FILE *f, char *buf) {
    (void)f;
    (void)buf;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    (void)buf;
    (void)size;
    if (!f) return -1;
    /* `_IONBF` is honoured by emptying the buffer and keeping it empty; the caller's buffer and
       size are still ignored, because the stream's own is 4 KiB and not worth a second path. */
    if (mode == _IONBF) {
        (void)libc_wbuf_flush(f);
        f->nobuf = 1;
        return 0;
    }
    return -1;
}

/*
 * One character of pushback - see `<libc/stdio.h>` for why one is the whole contract.
 *
 * `EOF` is refused because storing it would make the next read report a character that is not
 * one, and a stream that has genuinely ended would look like it had not. Pushing back onto a
 * slot that is already full is refused too: C leaves a second pushback undefined, and dropping
 * the first silently is the version of undefined that costs a caller the most to find.
 *
 * Clearing `eof` is required: the point of ungetting after a failed read is that there is now
 * something to read, so a stream that reported end-of-file must stop reporting it.
 */
int ungetc(int c, FILE *f) {
    if (!f || c == EOF || f->pushback >= 0) return EOF;
    f->pushback = (int)(unsigned char)c;
    f->eof      = 0;
    return (int)(unsigned char)c;
}

char *fgets(char *buf, int size, FILE *f) {
    if (!buf || size <= 0) return (char *)0;
    int n = 0;
    while (n < size - 1) {
        const int c = fgetc(f);
        if (c == EOF) break;
        buf[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return (char *)0;
    buf[n] = '\0';
    return buf;
}

int fputc(int c, FILE *f) {
    const char ch = (char)c;
    return (fwrite(&ch, 1u, 1u, f) == 1u) ? c : EOF;
}

/* Functions rather than macros, so the stream argument is evaluated once - `putc(c, streams[i++])`
 * means what it reads as here. See the note beside their declarations. */
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int getchar(void) { return fgetc(stdin); }

int fputs(const char *s, FILE *f) {
    if (!s) return EOF;
    const size_t n = obs_strlen(s);
    return (fwrite(s, 1u, n, f) == n) ? 0 : EOF;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout);
}

int remove(const char *path) { return oops_fs_unlink(path); }

/* The formatted family, all through oops_vsnprintf. A line longer than the buffer is truncated
 * rather than overrun, and the return is what was written - not what would have been - because
 * the formatter underneath reports the one and not the other. */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    return oops_vsnprintf(buf, size, fmt, args);
}

int vsprintf(char *buf, const char *fmt, va_list args) {
    return oops_vsnprintf(buf, (size_t)0x7fffffff, fmt, args);
}

/*
 * `vasprintf` grows a buffer rather than asking how big one needs to be, and the reason is the
 * sentence three comments up: **`oops_vsnprintf` returns what it wrote, not what it would have
 * written.**
 *
 * The usual implementation is two calls - `vsnprintf(NULL, 0, ...)` to learn the length, then
 * one allocation that fits. Here the probe would return 0 every time and every result would be
 * an empty string. So this doubles a buffer until the result demonstrably fits.
 *
 * "Demonstrably" is `n + 1 < cap`: at least one byte spare, so nothing was cut. Exactly filling
 * the buffer (`n + 1 == cap`) is indistinguishable from being truncated at the boundary, so that
 * case grows too - one wasted iteration on an exact fit, against silently losing the last
 * character otherwise.
 *
 * The ceiling is there because the alternative to a limit is a title that answers a bad format
 * string by allocating until the process dies. A megabyte is far past any message a locale facet
 * or a log line produces, and passing it returns -1 like any other failure rather than
 * truncating, because a caller that checks the return gets to decide and one that does not gets
 * a null pointer rather than a plausible half-message.
 */
int vasprintf(char **ret, const char *fmt, va_list args) {
    size_t cap = 128u;

    for (;;) {
        char *const buf = (char *)malloc(cap);
        if (buf == NULL) {
            *ret = NULL;
            return -1;
        }

        /* `args` is walked by the call, so each attempt needs its own copy. Reusing it after a
           truncated attempt reads past the end of the argument list. */
        va_list attempt;
        va_copy(attempt, args);
        const int n = oops_vsnprintf(buf, cap, fmt, attempt);
        va_end(attempt);

        if (n < 0) {
            free(buf);
            *ret = NULL;
            return -1;
        }

        if ((size_t)n + 1u < cap) {
            *ret = buf;
            return n;
        }

        free(buf);

        if (cap >= (size_t)1u << 20) {
            *ret = NULL;
            return -1;
        }
        cap *= 2u;
    }
}

int asprintf(char **ret, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vasprintf(ret, fmt, args);
    va_end(args);
    return n;
}

/* The conversion is `obs_vsscanf` in `src/system/scanf.c`, which builds on the host too so that
 * `test_scanf.c` can run it. See `<libc/stdio.h>` for why a loader needs this and why `fscanf`
 * is not beside it. */
int vsscanf(const char *s, const char *fmt, va_list args) { return obs_vsscanf(s, fmt, args); }

int sscanf(const char *s, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = obs_vsscanf(s, fmt, args);
    va_end(args);
    return n;
}

int vfprintf(FILE *f, const char *fmt, va_list args) {
    char line[512];
    const int n = oops_vsnprintf(line, sizeof(line), fmt, args);
    if (n <= 0) return n;
    const size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1u;
    return (fwrite(line, 1u, len, f) == len) ? (int)len : -1;
}

int vprintf(const char *fmt, va_list args) { return vfprintf(stdout, fmt, args); }

int printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vfprintf(stdout, fmt, args);
    va_end(args);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vfprintf(f, fmt, args);
    va_end(args);
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = oops_vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = oops_vsnprintf(buf, (size_t)0x7fffffff, fmt, args);
    va_end(args);
    return n;
}

/* ---------------------------------------------------------------------------
 * time
 *
 * Both are the SDK's monotonic nanosecond clock, and neither is a calendar - see <libc/time.h>
 * for why that is said out loud rather than approximated.
 * --------------------------------------------------------------------------- */

time_t time(time_t *out) {
    /*
     * The wall clock, not the process clock. This used to divide `oops_time_get_ns()`, which
     * counts from process start - fine for `srand(time(NULL))` and useless for a date. See
     * `include/libc/time.h` for why that was never a decision.
     *
     * Falling back to the process clock when the platform will not answer keeps the old
     * behaviour for the callers that only wanted a changing number, and `time()` returning
     * something small is exactly what a caller checking for a plausible date will reject.
     */
    uint64_t epoch = oops_time_get_epoch_seconds();
    const time_t s = epoch ? (time_t)epoch : (time_t)(oops_time_get_ns() / 1000000000ull);
    if (out) *out = s;
    return s;
}

/* ---------------------------------------------------------------------------
 * setjmp / longjmp
 *
 * **Ours, in assembly, rather than the platform's.** `include/libc/setjmp.h` used to declare
 * these as imports the FreeBSD-derived C library would resolve at load, on the reasoning that
 * `setjmp` cannot be written in C so it should not be attempted. The first title to need them
 * showed why that does not work here: `mkmodule` refuses an imported symbol whose library the
 * mined corpus cannot name, because a module must declare where each import resolves. An import
 * nobody can attribute is not a dependency this SDK can ship.
 *
 * libpng is what needs them - its error handling is `setjmp(png_jmpbuf(png_ptr))`, which every
 * caller writes, including Neverball's `share/fs_png.c`.
 *
 * The System V AMD64 ABI says what has to be saved: the callee-saved registers `rbx`, `rbp`,
 * `r12`-`r15`, the stack pointer, and where to resume. Eight quadwords, well inside the twelve
 * `jmp_buf` gives. Nothing here is platform-specific - it is the architecture's calling
 * convention, which is published.
 *
 * Written as a global assembly block rather than a `.S` file because `oops-sdk.mk` compiles C.
 * --------------------------------------------------------------------------- */

__asm__(".text\n"
        ".globl setjmp\n"
        ".type setjmp,@function\n"
        "setjmp:\n"
        "  movq %rbx,  0(%rdi)\n"
        "  movq %rbp,  8(%rdi)\n"
        "  movq %r12, 16(%rdi)\n"
        "  movq %r13, 24(%rdi)\n"
        "  movq %r14, 32(%rdi)\n"
        "  movq %r15, 40(%rdi)\n"
        /* The stack pointer as it will be *after* this function returns, so longjmp resumes
           into the caller's frame rather than into one that has gone. */
        "  leaq 8(%rsp), %rax\n"
        "  movq %rax, 48(%rdi)\n"
        /* The return address, which is where longjmp jumps back to. */
        "  movq (%rsp), %rax\n"
        "  movq %rax, 56(%rdi)\n"
        "  xorl %eax, %eax\n"
        "  ret\n"
        ".size setjmp,.-setjmp\n"
        "\n"
        ".globl longjmp\n"
        ".type longjmp,@function\n"
        "longjmp:\n"
        "  movq  0(%rdi), %rbx\n"
        "  movq  8(%rdi), %rbp\n"
        "  movq 16(%rdi), %r12\n"
        "  movq 24(%rdi), %r13\n"
        "  movq 32(%rdi), %r14\n"
        "  movq 40(%rdi), %r15\n"
        "  movq 56(%rdi), %rdx\n"
        "  movq 48(%rdi), %rsp\n"
        /* C requires longjmp(env, 0) to make setjmp return 1: zero is the value setjmp itself
           returns, so it must never be handed back. */
        "  movl %esi, %eax\n"
        "  testl %eax, %eax\n"
        "  jnz 1f\n"
        "  incl %eax\n"
        "1:\n"
        "  jmp *%rdx\n"
        ".size longjmp,.-longjmp\n");

/* ---------------------------------------------------------------------------
 * The calendar
 *
 * Howard Hinnant's civil-from-days algorithm, which is the one every modern C library and
 * `<chrono>` implementation uses: shift the year so it starts in March, and leap days land at
 * the end of the cycle where they stop being a special case. It is exact for the whole range of
 * a 64-bit `time_t` and has no table.
 *
 * Written out rather than taken from anywhere: it is arithmetic on a published calendar, the
 * kind of thing `docs/CONVENTIONS.md` calls a format fact.
 * --------------------------------------------------------------------------- */

#define OOPS_SECS_PER_DAY 86400

static const char *const s_wday_short[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const s_mon_short[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

struct tm *gmtime_r(const time_t *t, struct tm *out) {
    if (!t || !out) return (struct tm *)0;

    int64_t secs = (int64_t)*t;
    int64_t days = secs / OOPS_SECS_PER_DAY;
    int64_t rem = secs % OOPS_SECS_PER_DAY;
    if (rem < 0) { /* C's division truncates toward zero; days must floor */
        rem += OOPS_SECS_PER_DAY;
        days -= 1;
    }

    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)((rem % 3600) / 60);
    out->tm_sec = (int)(rem % 60);

    /* 1970-01-01 was a Thursday, which is 4. */
    out->tm_wday = (int)((days + 4) % 7);
    if (out->tm_wday < 0) out->tm_wday += 7;

    /* Civil from days: shift the epoch to 0000-03-01 so leap days end the cycle. */
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint64_t doe = (uint64_t)(z - era * 146097);                      /* 0..146096 */
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* 0..399 */
    const int64_t y = (int64_t)yoe + era * 400;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           /* 0..365 */
    const uint64_t mp = (5 * doy + 2) / 153;                                /* 0..11, March = 0 */
    const uint64_t d = doy - (153 * mp + 2) / 5 + 1;                        /* 1..31 */
    const uint64_t m = mp < 10 ? mp + 3 : mp - 9;                           /* 1..12 */

    const int64_t year = y + (m <= 2 ? 1 : 0);
    out->tm_year = (int)(year - 1900);
    out->tm_mon = (int)m - 1;
    out->tm_mday = (int)d;
    out->tm_isdst = 0;

    /* Day of the year, counted from January the first of this year. */
    {
        static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        const int leap =
            ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 1 : 0;
        out->tm_yday = cum[out->tm_mon] + out->tm_mday - 1 +
                       ((leap && out->tm_mon > 1) ? 1 : 0);
    }
    return out;
}

struct tm *gmtime(const time_t *t) {
    static struct tm s_tm;
    return gmtime_r(t, &s_tm);
}

/* UTC, as `include/libc/time.h` says: there is no timezone to apply. */
struct tm *localtime_r(const time_t *t, struct tm *out) { return gmtime_r(t, out); }
struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t mktime(struct tm *tm) {
    if (!tm) return (time_t)-1;

    /* days_from_civil, the inverse of the above. */
    int64_t y = (int64_t)tm->tm_year + 1900;
    int64_t m = (int64_t)tm->tm_mon + 1;

    /* Accept a month outside 1..12, as C requires mktime to normalise. */
    y += (m - 1) / 12;
    m = (m - 1) % 12 + 1;
    if (m <= 0) { m += 12; y -= 1; }

    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint64_t yoe = (uint64_t)(y - era * 400);
    const uint64_t doy =
        (uint64_t)((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5) + (uint64_t)tm->tm_mday - 1u;
    const uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + (int64_t)doe - 719468;

    const int64_t secs = days * OOPS_SECS_PER_DAY + (int64_t)tm->tm_hour * 3600 +
                         (int64_t)tm->tm_min * 60 + (int64_t)tm->tm_sec;

    /* C says mktime writes the normalised fields back. */
    const time_t out = (time_t)secs;
    (void)gmtime_r(&out, tm);
    return out;
}

/*
 * `strftime`, for the conversions a port actually writes. Anything else is copied through
 * unchanged rather than silently dropped, so an unsupported `%q` appears in the output and is
 * visible instead of vanishing.
 */
static size_t oops_strftime_num(char *buf, size_t max, size_t at, int value, int width) {
    char tmp[16];
    int n = 0;
    int v = value < 0 ? -value : value;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0 && n < (int)sizeof(tmp));
    while (n < width && n < (int)sizeof(tmp)) tmp[n++] = '0';
    if (value < 0 && n < (int)sizeof(tmp)) tmp[n++] = '-';
    while (n > 0) {
        if (at + 1 >= max) return at;
        buf[at++] = tmp[--n];
    }
    return at;
}

static size_t oops_strftime_str(char *buf, size_t max, size_t at, const char *s) {
    while (*s) {
        if (at + 1 >= max) return at;
        buf[at++] = *s++;
    }
    return at;
}

size_t strftime(char *buf, size_t max, const char *format, const struct tm *tm) {
    if (!buf || !format || !tm || max == 0) return 0;

    size_t at = 0;
    for (const char *p = format; *p; p++) {
        if (*p != '%') {
            if (at + 1 >= max) { buf[at] = '\0'; return 0; }
            buf[at++] = *p;
            continue;
        }
        p++;
        switch (*p) {
        case 'Y': at = oops_strftime_num(buf, max, at, tm->tm_year + 1900, 1); break;
        case 'y': at = oops_strftime_num(buf, max, at, (tm->tm_year + 1900) % 100, 2); break;
        case 'm': at = oops_strftime_num(buf, max, at, tm->tm_mon + 1, 2); break;
        case 'd': at = oops_strftime_num(buf, max, at, tm->tm_mday, 2); break;
        case 'H': at = oops_strftime_num(buf, max, at, tm->tm_hour, 2); break;
        case 'M': at = oops_strftime_num(buf, max, at, tm->tm_min, 2); break;
        case 'S': at = oops_strftime_num(buf, max, at, tm->tm_sec, 2); break;
        case 'j': at = oops_strftime_num(buf, max, at, tm->tm_yday + 1, 3); break;
        case 'a':
            at = oops_strftime_str(buf, max, at,
                                   s_wday_short[(tm->tm_wday >= 0 && tm->tm_wday < 7)
                                                    ? tm->tm_wday : 0]);
            break;
        case 'b':
            at = oops_strftime_str(buf, max, at,
                                   s_mon_short[(tm->tm_mon >= 0 && tm->tm_mon < 12)
                                                   ? tm->tm_mon : 0]);
            break;
        case '%':
            if (at + 1 < max) buf[at++] = '%';
            break;
        case '\0':
            /* A trailing '%' with nothing after it. Stop rather than read past the string. */
            buf[at] = '\0';
            return at;
        default:
            /* Unsupported: emit it as written, so it is visible in the output. */
            if (at + 2 < max) { buf[at++] = '%'; buf[at++] = *p; }
            break;
        }
    }
    buf[at] = '\0';
    return at;
}

clock_t clock(void) {
    return (clock_t)(oops_time_get_ns() / 1000ull); /* CLOCKS_PER_SEC is 1,000,000 */
}

/* ---------------------------------------------------------------------------
 * The calendar as text: asctime, ctime, difftime
 *
 * C fixes this rendering exactly - `Www Mmm dd hh:mm:ss yyyy\n`, twenty-six bytes with the
 * terminator - so this is transcription and not a choice of format. `strftime` above could
 * express it, but not portably: the day-of-month field is space-padded, which is `%e`, and that
 * is an extension rather than one of the conversions that function documents itself as carrying.
 *
 * `s_wday_short` and `s_mon_short` are already here for `strftime`, which is most of the work.
 *
 * **The indices are clamped before they reach those tables.** A `struct tm` that a caller filled
 * in by hand - rather than one `gmtime` produced - can carry any `tm_wday` at all, and C says the
 * result is undefined rather than saying it is a read off the end of a seven-element array. A
 * fault here would be a long way from the mistake that caused it.
 * --------------------------------------------------------------------------- */

char *asctime_r(const struct tm *tm, char *buf) {
    if (!tm || !buf) return (char *)0;
    const int wd = (tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0;
    const int mo = (tm->tm_mon >= 0 && tm->tm_mon < 12) ? tm->tm_mon : 0;
    (void)oops_snprintf(buf, 26u, "%s %s%3d %02d:%02d:%02d %d\n", s_wday_short[wd],
                        s_mon_short[mo], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
                        tm->tm_year + 1900);
    return buf;
}

/* One static buffer, which is what C specifies and what makes these the two calls in this file a
 * second thread can tread on. The `_r` forms above and beside them are the ones to reach for. */
char *asctime(const struct tm *tm) {
    static char s_asctime[26];
    return asctime_r(tm, s_asctime);
}

char *ctime_r(const time_t *t, char *buf) {
    struct tm tmp;
    if (!t || !buf || !localtime_r(t, &tmp)) return (char *)0;
    return asctime_r(&tmp, buf);
}

char *ctime(const time_t *t) {
    static char s_ctime[26];
    return ctime_r(t, s_ctime);
}

double difftime(time_t end, time_t start) {
    return (double)end - (double)start;
}

/* ---------------------------------------------------------------------------
 * assert
 * --------------------------------------------------------------------------- */

void oops_assert_failed(const char *expr, const char *file, int line) {
    char msg[256];
    (void)oops_snprintf(msg, sizeof(msg), "assertion failed: %s, %s:%d", expr ? expr : "?",
                        file ? file : "?", line);
    oops_klog("assert", msg);
    abort();
}

#endif /* !OOPS_HOST_BUILD */
