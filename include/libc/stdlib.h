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

#include <stddef.h>

#define RAND_MAX 0x7fffffff
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
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
long long llabs(long long x);

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
div_t div(int num, int den);
ldiv_t ldiv(long num, long den);

#endif /* OOPS_LIBC_STDLIB_H */
