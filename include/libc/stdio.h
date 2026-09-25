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
    /*
     * **Writes are buffered, because a write here is a syscall.**
     *
     * A `FILE` on this platform is a descriptor, and `fwrite` used to pass straight to
     * `oops_fs_write`. That is fine for a program writing a block at a time and ruinous for one
     * writing a field at a time - which is what a serialiser does. Neverball records a replay by
     * writing each command's shorts and floats individually, so a frame became thousands of
     * two-to-four-byte syscalls: measured at **2.3 seconds in one frame**, with the physics it
     * was blamed on taking 0ms. The game was unplayable and nothing looked wrong.
     *
     * Flushed when full, by `fflush`, by `fclose`, and before any seek or read so the descriptor
     * offset is never behind what the caller has written. `setvbuf(_IONBF)` empties and disables
     * it, which is the one mode that was already honoured.
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
/* Clears both of the above. C's way of saying "I have handled that error, carry on with this
 * stream" - without it a caller that recovers from a short read has no way to stop `ferror`
 * answering yes forever. */
void clearerr(FILE *f);
int fflush(FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
char *fgets(char *buf, int size, FILE *f);
int fputc(int c, FILE *f);
/* `putc` is `fputc`, and `getc` above is `fgetc` - C allows both to be macros evaluating the
 * stream more than once, and neither is, which is the stricter promise. The pair had been split:
 * `getc` was here and `putc` was not, so a port writing a file a byte at a time - Extreme Tux
 * Racer's `common.cpp`, saving a course - compiled against libc++'s `<cstdio>`, found
 * `using ::putc` resolving to nothing, and failed on a name every C library has. */
int putc(int c, FILE *f);
int putchar(int c);
int getchar(void);
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
 * **`setbuf` and `setvbuf` on a C library that does not buffer** (2026-09-23).
 *
 * `basic_filebuf` calls `setbuf(file, nullptr)` to take over buffering itself, and without the
 * declaration every `<fstream>` user fails to compile. There is nothing to take over: a `FILE`
 * here is a descriptor and a few flags, and `fwrite` goes straight to `oops_fs_write`.
 *
 * So `setbuf` is a no-op that is *already correct* - asking for an unbuffered stream is asking
 * for what this is - and `setvbuf` reports success for `_IONBF` and failure for the two buffered
 * modes it cannot provide. Refusing is the honest answer there rather than accepting and
 * ignoring: a caller that checks the return learns the truth, and one that does not is no worse
 * off than with a silent lie.
 */
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

void setbuf(FILE *f, char *buf);
int  setvbuf(FILE *f, char *buf, int mode, size_t size);

/*
 * **`fdopen`, which is nearly free here** (2026-09-23). libc++'s `basic_filebuf` has a
 * constructor taking a native handle and reaches for it, so `<fstream>` does not compile
 * without the declaration.
 *
 * A `FILE` on this platform *is* a descriptor and a few flags, so this wraps rather than
 * converts: no buffer to attach, no mode to reconcile. The `mode` string is accepted and
 * ignored, because the descriptor already carries the access the caller opened it with and
 * re-deriving it from a string would be inventing a second source of truth for it.
 *
 * `fclose` on the result closes the descriptor, which is what POSIX specifies.
 */
FILE *fdopen(int fd, const char *mode);
/* The descriptor behind a stream - `fdopen`'s inverse, and the first half of "how big has this
 * log file grown". Defined in `oops-apps/common/posix/posix.c`, beside the `fstat` that is the
 * other half, so there is one copy per link. */
int fileno(FILE *stream);

/*
 * **`fseeko` / `ftello`, the 64-bit-offset pair** (2026-09-23). `basic_filebuf` seeks with these
 * rather than `fseek`, so `<fstream>` needs them.
 *
 * On this target `long` is already 64 bits, so they carry no more range than `fseek` and
 * `ftell` do and are one-line forwards. That is worth saying rather than leaving to be
 * rediscovered: they are not here because the plain pair is too narrow, they are here because
 * the *names* are what libc++ reaches for, and a 32-bit host is where the distinction lives.
 */
typedef long off_t;

int   fseeko(FILE *f, off_t offset, int whence);
off_t ftello(FILE *f);

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
