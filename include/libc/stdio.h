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
} FILE;

extern FILE *stdout;
extern FILE *stderr;
extern FILE *stdin;

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

#endif /* OOPS_LIBC_STDIO_H */
