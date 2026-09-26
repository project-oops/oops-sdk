/*
 * <stdio.h> - the names a port's own code calls, over this SDK's filesystem
 * (`oops_fs_open`) and log (`oops_klog`, `oops_vsnprintf`).
 *
 * There is no terminal. `stdout` and `stderr` are the kernel log, one line per flush or
 * newline, tagged `stdout`/`stderr`, so `printf` from a port arrives where every other
 * diagnostic does. A program whose output is data should write to a file.
 *
 * On the target include path only - see <libc/math.h> for why, and for what happens to
 * a port that gets an implicit declaration instead.
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

/* An open file: the descriptor, and what has happened to it. A port keeps the pointer
 * and passes it back; nothing here needs it to be anything larger. */
typedef struct oops_FILE {
    int fd;
    int eof;
    int err;
    int is_log; /* stdout and stderr, which have no descriptor */
    /*
     * One character of pushback, for `ungetc`, or -1 when empty. C guarantees exactly
     * one. It belongs to the stream, not to `fgetc`, so `fread` consumes it too.
     */
    int pushback;
    /*
     * Writes are buffered, because a write here is a syscall and a serialiser writes a
     * field at a time (a Neverball replay frame was thousands of tiny writes). Flushed
     * when full, by `fflush`, by `fclose`, and before any seek or read so the
     * descriptor offset is never behind what the caller has written.
     * `setvbuf(_IONBF)` empties and disables it.
     */
    unsigned char *wbuf;
    unsigned int wbuf_len;
    unsigned int wbuf_cap;
    int nobuf;
} FILE;

extern FILE *stdout;
extern FILE *stderr;
extern FILE *stdin;

/*
 * `rename`, over `oops_fs_rename`: how a program writes a file to a temporary path and
 * moves it into place, so an interruption never leaves it half-written.
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
/* Clears both of the above, so a caller that recovers from a short read can carry on
 * with the stream. */
void clearerr(FILE *f);

/* `<prefix>: <reason>` on stderr, from the current `errno` through `strerror`. A NULL
 * or empty prefix prints the reason alone, as C specifies. */
void perror(const char *prefix);
int fflush(FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
char *fgets(char *buf, int size, FILE *f);
int fputc(int c, FILE *f);
/* `putc` is `fputc`, and `getc` above is `fgetc`. Both are functions, not macros, so
 * the stream is evaluated once; libc++'s `<cstdio>` needs both declared. */
int putc(int c, FILE *f);
int putchar(int c);
int getchar(void);
int fputs(const char *s, FILE *f);
int puts(const char *s);
int remove(const char *path);

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vprintf(const char *fmt, va_list args);
int vfprintf(FILE *f, const char *fmt, va_list args);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int vsprintf(char *buf, const char *fmt, va_list args);

/*
 * `asprintf` and `vasprintf` (BSD, not ISO C), which libc++'s locale support needs for
 * every stream and numeric facet. They format into a buffer they allocate; the caller
 * frees it with `free`, as the BSDs and glibc specify. On failure `*ret` is set to null
 * and -1 is returned, so the caller never frees a stale pointer.
 */
int asprintf(char **ret, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vasprintf(char **ret, const char *fmt, va_list args);

/*
 * `ungetc` pushes one character back so the next read returns it (libc++'s
 * `std_stream.h` uses it for `std::cin`'s `putback`). C guarantees one character of
 * pushback; `EOF` is refused rather than stored, so an ended stream stays ended.
 */
int ungetc(int c, FILE *f);

/*
 * `setbuf` and `setvbuf`, which libc++'s `basic_filebuf` calls. `setbuf` is a no-op.
 * `setvbuf` honours `_IONBF` by flushing and disabling the stream's own write buffer,
 * and refuses the buffered modes (a caller's buffer is never adopted) rather than
 * accepting and ignoring them.
 */
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

void setbuf(FILE *f, char *buf);
int setvbuf(FILE *f, char *buf, int mode, size_t size);

/*
 * `fdopen` wraps a descriptor in a `FILE` (libc++'s `basic_filebuf` needs it). The
 * `mode` string is ignored, since the descriptor already carries its access. `fclose`
 * on the result closes the descriptor, as POSIX specifies.
 */
FILE *fdopen(int fd, const char *mode);
/* The descriptor behind a stream, `fdopen`'s inverse. Defined in
 * `oops-apps/common/posix/posix.c`, beside `fstat`, so there is one copy per link. */
int fileno(FILE *stream);

/*
 * `fseeko` / `ftello`, which `basic_filebuf` seeks with. `long` is 64 bits on this
 * target, so they are forwards to `fseek` and `ftell` with no extra range.
 */
typedef long off_t;

int fseeko(FILE *f, off_t offset, int whence);
off_t ftello(FILE *f);

/*
 * `sscanf` converts `%d %i %u %o %x %X %p`, the float forms, `%s %c %n %%` and `%[...]`
 * scansets, with the length modifiers, a width, and `*` to skip a field. It returns the
 * number of assignments, or `EOF` when the input ran out before the first one, which
 * is how a read loop tells end of file from a line that did not parse.
 *
 * `fscanf` and `scanf` are not provided: read the line with `fgets` and scan it.
 */
int sscanf(const char *s, const char *fmt, ...) __attribute__((format(scanf, 2, 3)));
int vsscanf(const char *s, const char *fmt, va_list args);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_STDIO_H */
