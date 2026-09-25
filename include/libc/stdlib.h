/*
 * <stdlib.h> - the names a port's own code calls.
 *
 * The heap is this SDK's (`oops_malloc`), the exit path is the payload's. On the target include
 * path only - see <libc/math.h>.
 *
 * `rand` is the usual linear congruential generator, seeded 1 as the specification says an
 * unseeded one behaves. It is not for anything that needs to be unguessable; nothing in a port
 * that calls `rand()` needs that either, and a program that does should say so and bring its own.
 */
#ifndef OOPS_LIBC_STDLIB_H
#define OOPS_LIBC_STDLIB_H

/*
 * **C linkage when a C++ translation unit includes this** (2026-09-21), and the same block is on
 * every header in this directory.
 *
 * These are C functions. Without this, a C++ caller mangles every name here - `strtod` becomes
 * `strtod(char const*, char**)` - and links against nothing. A payload link passes
 * `--unresolved-symbols=ignore-all`, so that failure does not stop the build; it reaches the
 * console.
 *
 * It arrives now because the first C++ consumer arrived: libc++ compiled cleanly against these
 * headers and then asked the linker for ten mangled C names. Nothing was wrong with the
 * declarations, only with what language they were declared in.
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define RAND_MAX 0x7fffffff
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/*
 * MB_CUR_MAX (REQ-20260923T1810Z-7d42).
 * On this platform there is exactly one locale ("C"), so characters are single-byte
 * and MB_CUR_MAX is fixed at 1 rather than being a selectable locale function call.
 */
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

/* `exit` and `abort` end the payload's main loop and return to its caller; there is no process
 * to leave. A program that expects the console to return to its menu is disappointed either way,
 * and this at least unwinds rather than faulting. */
void exit(int status);
void abort(void);

/* **`atexit` accepts and never runs** (2026-09-25): `exit` above parks rather than returning
 * through a C runtime, so there is no moment at which a registered handler could run. Refusing
 * would be worse - SuperTux's `atexit(TTF_Quit)` would report failure for a cleanup that is
 * pointless when the system tears the process down anyway. A program with cleanup that matters
 * must call it itself, as the note on `exit` says. */
int atexit(void (*fn)(void));

/* **`system` has no shell to run** (2026-09-25). `system(NULL)` answers 0 - "no command processor
 * is available", which is how C lets a caller ask - and any command answers -1 with `ENOSYS`.
 * SuperTux's "open this folder" button and Squirrel's `system()` builtin both reach it. */
int system(const char *command);

/*
 * **Sorting and searching** (2026-09-20). `qsort` is here because depth-sorting transparent
 * geometry back to front is *the* GL 1.x way to draw it - there is no order-independent
 * transparency in a fixed-function pipeline - so a program that draws anything see-through
 * sorts, and sorts with this.
 *
 * It is a median-of-three quicksort with an insertion sort under sixteen elements and the
 * smaller partition recursed into, which bounds the stack at log2(n) frames - the payload's
 * stack is not the host's and a sorted input is exactly what a scene hands it frame after
 * frame. **Not stable**, which C does not promise either.
 */
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));

unsigned long strtoul(const char *s, char **end, int base);
float strtof(const char *s, char **end);

/*
 * **The `long long` and `long double` conversions** (2026-09-21). C99 requires them beside the
 * `long` forms above, and until now a program that wrote `strtoll` got a compile error - which
 * is how libc++ found them: `std::stoll`, `std::stoull` and `std::stold` are defined in terms of
 * exactly these three, so a standard library compiled against this header failed on them.
 *
 * On this target `long` is 64 bits, so `strtoll` and `strtoull` are the same conversion under
 * their C99 names rather than new code. `strtold` is **not** exact: `long double` is wider than
 * `double` here, and the value is parsed at double precision and widened. That loses the extra
 * mantissa bits an 80-bit parse would keep, and it is said here rather than discovered by a
 * program that needed them.
 */
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
long double strtold(const char *s, char **end);

/*
 * **The environment starts empty, and a payload may fill it** (2026-09-21, extended 2026-09-23).
 *
 * A payload is launched by the system, not spawned from a shell, so nothing is inherited and
 * every name begins unset - which is exactly what NULL means and exactly what a desktop returns
 * for a name nobody exported. A caller branching on it takes its default path, which is the
 * behaviour it would get from a clean shell. FreeType asked for it first (`ftinit.c` reads
 * `FREETYPE_PROPERTIES`), and sdl12-compat's `SDL12COMPAT_getenv_unsafe` had already had to
 * patch around its absence.
 *
 * `setenv` used to drop what it was given, on the reasoning that there was no block to write
 * into. There is now, because of the one variable a payload genuinely knows and a POSIX program
 * genuinely needs: **`HOME`**. Savedata is the only writable directory a title has, and a title
 * that mounts it and exports the mount point lets every port find its own way there - no patch
 * to the port's sources, because looking at `HOME` is what they already do. Neverball is the
 * case that asked: `pick_home_path` reads it and otherwise falls back to the read-only package
 * directory, so its `config_save` had nowhere to go and it asked for a player name every launch.
 *
 * Sixteen names, 32 characters each, 192 for a value, in fixed storage - this is used during
 * start-up and is not worth a malloc that has to work that early. A name or value that does not
 * fit is refused rather than truncated, because a silently shortened `HOME` is a path to
 * somewhere else entirely. `unsetenv` returns 0 for a name that was never set, as POSIX says.
 */
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);

/*
 * `alloca`, which is the compiler's and never a library's (2026-09-21). It has to unwind with
 * the frame, so it cannot be a function call - every C library defines it as this builtin, and
 * so does this one. Extreme Tux Racer asked for it in nine of its sources.
 */
#define alloca(n) __builtin_alloca(n)
long long llabs(long long x);

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
div_t div(int num, int den);
ldiv_t ldiv(long num, long den);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_STDLIB_H */
