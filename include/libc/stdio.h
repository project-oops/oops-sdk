/*
 * <stdio.h> - the names a port's own code calls, over this SDK's filesystem and log.
 *
 * A port loads things: a texture, a model, a level, a config. It does that with `fopen` and
 * `fread`, and it reports what happened with `printf`. This SDK has had both underneath -
 * `oops_fs_open`, `oops_klog`, `oops_vsnprintf` - under names nothing being ported calls.
 *
 * **Where the output goes.** There is no terminal. `stdout` and `stderr` are the kernel log, one
 * line per flush or newline, tagged `stdout`/`stderr` - so `printf` from a port arrives where
 * every other diagnostic in this collection does. A program whose output is data rather than
 * diagnostics should write to a file, and on a console it should have been doing that anyway.
 *
 * On the target include path only - see <libc/math.h> for why, and for what happens to a port
 * that gets an implicit declaration instead.
 */
#ifndef OOPS_LIBC_STDIO_H
#define OOPS_LIBC_STDIO_H

/* C linkage for a C++ includer; `stdlib.h` carries the reasoning. */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 1024

/* An open file: the descriptor, and what has happened to it. A port keeps the pointer and passes
 * it back; nothing here needs it to be anything larger. */
typedef struct oops_FILE {
    int fd;
    int eof;
    int err;
    int is_log; /* stdout and stderr, which have no descriptor */
    /*
     * One character of pushback, for `ungetc`, or -1 when empty. C guarantees exactly one, so a
     * single slot is the whole contract rather than a simplification of it (2026-09-23,
     * `REQ-20260923T1810Z-7d42`).
     *
     * It belongs to the *stream*, not to `fgetc`, so `fread` consumes it too - a caller that
     * ungets a byte and then reads a block must see that byte first, and a pushback only
     * `fgetc` knew about would silently vanish from any other read.
     */
    int pushback;
} FILE;

extern FILE *stdout;
extern FILE *stderr;
extern FILE *stdin;

/*
 * `rename` (2026-09-22), the partner to the `remove` already declared below. `oops_fs_rename` is
 * underneath. Neverball wanted it: it writes a config to a temporary path and moves it into
 * place, which is how a program avoids leaving a half-written file if it is interrupted.
 */
int rename(const char *from, const char *to);

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t count, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t count, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
int fflush(FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
char *fgets(char *buf, int size, FILE *f);
int fputc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int puts(const char *s);
int remove(const char *path);

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *fmt, va_list args);
int vfprintf(FILE *f, const char *fmt, va_list args);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int vsprintf(char *buf, const char *fmt, va_list args);

/*
 * **`asprintf` and `vasprintf`, because libc++'s localization needs them** (2026-09-23,
 * `REQ-20260923T1810Z-7d42`, alongside `MB_CUR_MAX`).
 *
 * They format into a buffer they allocate, and hand it over: the caller owns it and frees it
 * with `free`. On failure `*ret` is set to null and `-1` is returned, which is what every caller
 * checks and is the one behaviour worth getting right, because the alternative - leaving `*ret`
 * untouched - hands the caller a stale pointer it will free.
 *
 * These are BSD rather than ISO C, and they are here because libc++'s locale support is written
 * against a BSD-family C library: 23 of its 45 sources stop on `asprintf` without them, which is
 * every stream and every numeric facet. A title that never touches a stream never links them.
 *
 * `free` for the result rather than a paired `asprintf_free`, because that is what the BSDs and
 * glibc both specify and a ported program will not know to call anything else.
 */
int asprintf(char **ret, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vasprintf(char **ret, const char *fmt, va_list args);

/*
 * **`ungetc`, which is what a stream parser is built on** (2026-09-23,
 * `REQ-20260923T1810Z-7d42`). libc++'s `std_stream.h` calls it for `std::cin`'s `putback`, so
 * `iostream.cpp` and `ostream.cpp` do not compile without it.
 *
 * Pushes one character back so the next read returns it. C guarantees one character of pushback
 * and no more, and `EOF` is refused rather than stored - pushing back end-of-file would make a
 * stream that has ended look like one that has not.
 */
int ungetc(int c, FILE *f);

/*
 * **`sscanf`, because that is how a model file is read** (2026-09-20). An OBJ loader is a
 * `fgets` and an `sscanf("%f %f %f")`; so is an MTL loader, and so is every level format anyone
 * wrote by hand. It converts `%d %i %u %o %x %X %p`, the float forms, `%s %c %n %%` and
 * `%[...]` scansets, with the length modifiers, a width, and `*` to skip a field. It returns the
 * number of assignments, or `EOF` when the input ran out before the first one - which is how a
 * read loop tells end of file from a line that did not parse.
 *
 * **`fscanf` and `scanf` are not here.** Both need to put back a character when a conversion
 * reads one too many, and this SDK's file handles have no pushback. Read the line with `fgets`
 * and scan the line, which is what the code being ported does anyway.
 */
int sscanf(const char *s, const char *fmt, ...) __attribute__((format(scanf, 2, 3)));
int vsscanf(const char *s, const char *fmt, va_list args);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_STDIO_H */
