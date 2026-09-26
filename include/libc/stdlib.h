/*
 * <stdlib.h> - the names a port's own code calls.
 *
 * The heap is this SDK's (`oops_malloc`), the exit path is the payload's. On the target
 * include path only - see <libc/math.h>.
 *
 * `rand` is the usual linear congruential generator, seeded 1 as the specification says
 * an unseeded one behaves. It is not for anything that needs to be unguessable.
 */
#ifndef OOPS_LIBC_STDLIB_H
#define OOPS_LIBC_STDLIB_H

/*
 * C linkage for a C++ includer, as on every header in this directory. Without it a C++
 * caller mangles every name here and links against nothing, and since a payload link
 * passes `--unresolved-symbols=ignore-all` that failure reaches the console instead of
 * stopping the build.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define RAND_MAX 0x7fffffff
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* There is exactly one locale ("C"), so characters are single-byte and MB_CUR_MAX is
 * the constant 1. */
#define MB_CUR_MAX ((size_t)1)

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void *aligned_alloc(size_t alignment, size_t size);
void free(void *ptr);

int abs(int x);
long labs(long x);
int atoi(const char *s);
long atol(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
double strtod(const char *s, char **end);

int rand(void);
void srand(unsigned int seed);

/* `exit` and `abort` log a line naming themselves, ask the kernel to exit, and park if
 * that returns: the container cannot terminate itself. */
void exit(int status);
void abort(void);

/* `atexit` accepts and never runs the handler: `exit` above parks rather than returning
 * through a C runtime, so there is no moment to run it. It reports success because the
 * system tears the process down anyway; a program with cleanup that matters calls it
 * itself. */
int atexit(void (*fn)(void));

/* There is no shell. `system(NULL)` answers 0, C's "no command processor is
 * available", and any command answers -1 with `ENOSYS`. */
int system(const char *command);

/*
 * Sorting and searching. `qsort` is a median-of-three quicksort with an insertion sort
 * under sixteen elements, recursing into the smaller partition so the stack stays
 * within log2(n) frames even on the already-sorted input a depth-sorted scene hands it
 * every frame. Not stable, which C does not promise either.
 */
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));

unsigned long strtoul(const char *s, char **end, int base);
float strtof(const char *s, char **end);

/*
 * The C99 `long long` and `long double` conversions (libc++'s `std::stoll`, `stoull`
 * and `stold` are built on them). `long` is 64 bits on this target, so `strtoll` and
 * `strtoull` are the `long` conversions under their C99 names. `strtold` is not exact:
 * it parses at double precision and widens, losing the extra mantissa bits of an
 * 80-bit parse.
 */
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
long double strtold(const char *s, char **end);

/*
 * The environment starts empty, and a payload may fill it.
 *
 * A payload is launched by the system, not spawned from a shell, so nothing is
 * inherited and every name begins unset; a caller branching on one takes the default
 * path it would take from a clean shell. A title that mounts savedata, its only
 * writable directory, exports the mount point as `HOME`, so every port that looks at
 * `HOME` finds it with no patch to its sources.
 *
 * Sixteen names, 32 characters each, 192 for a value, in fixed storage, since this is
 * used during start-up before a malloc can be relied on. A name or value that does not
 * fit is refused rather than truncated, because a shortened `HOME` is a different
 * path. `unsetenv` returns 0 for a name that was never set, as POSIX says.
 */
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);

/* `alloca` has to unwind with the frame, so it is the compiler's builtin, as in every C
 * library. */
#define alloca(n) __builtin_alloca(n)
long long llabs(long long x);

typedef struct {
    int quot;
    int rem;
} div_t;
typedef struct {
    long quot;
    long rem;
} ldiv_t;
typedef struct {
    long long quot;
    long long rem;
} lldiv_t;
div_t div(int num, int den);
ldiv_t ldiv(long num, long den);
lldiv_t lldiv(long long num, long long den);

/*
 * `realpath`, declared here and defined in `oops-apps/common/posix/posix.c`, like
 * `open`, `fcntl` and `clock_gettime`: it is POSIX rather than C and needs a
 * filesystem. The declaration is in this header rather than the port layer's because
 * this is the copy a compile reaches (libc++'s `src/filesystem/operations.cpp` calls
 * `::realpath` after including `<stdlib.h>`); `fcntl.h` has the same arrangement.
 *
 * The resolution is lexical, which is complete here: there are no symbolic links, so
 * collapsing `.` and `..` against an absolute path is the whole answer.
 */
char *realpath(const char *path, char *resolved);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_STDLIB_H */
