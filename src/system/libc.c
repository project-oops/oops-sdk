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

/* The double forms are the float ones widened - see <libc/math.h>. */
double sqrt(double x) { return (double)oops_sqrtf((float)x); }
double fabs(double x) { return (x < 0.0) ? -x : x; }
double floor(double x) { return (double)oops_floorf((float)x); }
double ceil(double x) { return (double)oops_ceilf((float)x); }
double fmod(double x, double y) { return (double)oops_fmodf((float)x, (float)y); }
double sin(double x) { return (double)oops_sinf((float)x); }
double cos(double x) { return (double)oops_cosf((float)x); }
double tan(double x) { return (double)oops_tanf((float)x); }
double asin(double x) { return (double)asinf((float)x); }
double acos(double x) { return (double)acosf((float)x); }
double atan(double x) { return (double)atanf((float)x); }
double atan2(double y, double x) { return (double)oops_atan2f((float)y, (float)x); }
double exp(double x) { return (double)oops_expf((float)x); }
double log(double x) { return (double)oops_logf((float)x); }
double log10(double x) { return (double)log10f((float)x); }
double pow(double base, double exp_) { return (double)oops_powf((float)base, (float)exp_); }
double hypot(double x, double y) { return (double)hypotf((float)x, (float)y); }
double round(double x) { return (double)roundf((float)x); }
double trunc(double x) { return (double)truncf((float)x); }
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

/* x * 2^exp, and its inverse. Built out of the exponent field rather than out of `powf`, which
 * would round twice and lose the exactness that is the whole reason to call these. */
float ldexpf(float x, int exp_) { return __builtin_ldexpf(x, exp_); }
double ldexp(double x, int exp_) { return __builtin_ldexp(x, exp_); }
float frexpf(float x, int *exp_) { return __builtin_frexpf(x, exp_); }
double frexp(double x, int *exp_) { return __builtin_frexp(x, exp_); }

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
    if (!*needle) return (char *)(size_t)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)(size_t)haystack;
    }
    return (char *)0;
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
char *getenv(const char *name) {
  (void)name;
  return (char *)0;
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
 */
void exit(int status) {
    sys_call(SYS_exit, (long)(unsigned int)status, 0, 0, 0, 0, 0);
    for (;;) {
    }
}

void abort(void) { exit(1); }

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

static FILE s_stdout = {-1, 0, 0, 1};
static FILE s_stderr = {-1, 0, 0, 2};
static FILE s_stdin = {-1, 1, 0, 0}; /* nothing to read from; at end of file from the start */
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
    return f;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    if (f->is_log) {
        libc_log_flush(f->is_log);
        return 0;
    }
    const int rc = oops_fs_close(f->fd);
    oops_free(f);
    return (rc < 0) ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *f) {
    if (!f || f->is_log || !ptr || size == 0u || count == 0u) return 0u;
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
    const int64_t put = oops_fs_write(f->fd, ptr, size * count);
    if (put < 0) {
        f->err = 1;
        return 0u;
    }
    return (size_t)put / size;
}

int fseek(FILE *f, long offset, int whence) {
    if (!f || f->is_log) return -1;
    f->eof = 0;
    return (oops_fs_seek(f->fd, (int64_t)offset, whence) < 0) ? -1 : 0;
}

long ftell(FILE *f) {
    if (!f || f->is_log) return -1L;
    return (long)oops_fs_tell(f->fd);
}

void rewind(FILE *f) { (void)fseek(f, 0L, SEEK_SET); }
int feof(FILE *f) { return f ? f->eof : 1; }
int ferror(FILE *f) { return f ? f->err : 1; }

int fflush(FILE *f) {
    if (f && f->is_log) libc_log_flush(f->is_log);
    else if (!f) { libc_log_flush(1); libc_log_flush(2); } /* fflush(NULL): all of them */
    return 0;
}

int fgetc(FILE *f) {
    unsigned char c;
    if (fread(&c, 1u, 1u, f) != 1u) return EOF;
    return (int)c;
}

int getc(FILE *f) { return fgetc(f); }

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
    const time_t s = (time_t)(oops_time_get_ns() / 1000000000ull);
    if (out) *out = s;
    return s;
}

clock_t clock(void) {
    return (clock_t)(oops_time_get_ns() / 1000ull); /* CLOCKS_PER_SEC is 1,000,000 */
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
