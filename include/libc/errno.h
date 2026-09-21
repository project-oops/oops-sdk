/*
 * errno, over the platform's own.
 *
 * **This is a bridge, not an invention.** The console carries the FreeBSD-derived POSIX exports,
 * and errno lives behind `__error()` exactly as it does on FreeBSD - `src/net/net.c` has read it
 * that way since the socket work, and obSCEne measured `__error` callable on firmware 12.40
 * (sweep 20260909-083918) alongside the rest of the POSIX set.
 *
 * It arrives now because three separate consumers wanted it and each was shimming its own:
 * SDL2's `SDL_RWops` reaches for `<errno.h>` on the stdio path, Extreme Tux Racer's `DirExists`
 * reads `errno` after a failed `opendir`, and libc++'s `string.cpp` compares it against `ERANGE`
 * in `std::stoi`. Three private answers to one standard question is the signal that the question
 * belongs here.
 *
 * `errno` is a macro over a function returning a pointer, which is what C requires of it and
 * what makes it work per-thread when the platform's does.
 */
#ifndef OOPS_LIBC_ERRNO_H
#define OOPS_LIBC_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The location of the calling thread's errno. Resolves to the platform's `__error()` when that
 * import binds, and to a single process-wide slot when it does not - so reading `errno` is
 * always safe, and the fallback is a value this SDK's own calls can still set rather than a
 * fault. `src/system/libc.c` has the detail.
 */
int *oops_errno_location(void);

#define errno (*oops_errno_location())

/*
 * **FreeBSD's numbering, because it is the platform's.** Not a set this SDK chose: `net.c`
 * records try-again as 35 on this console, which is FreeBSD's `EAGAIN` and pins the whole table
 * to that origin. A program comparing `errno` against a constant from somewhere else would be
 * comparing against the wrong number.
 */
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define ENXIO    6
#define EBADF    9
#define ECHILD   10
#define EDEADLK  11
#define ENOMEM   12
#define EACCES   13
#define EFAULT   14
#define EBUSY    16
#define EEXIST   17
#define EXDEV    18
#define ENODEV   19
#define ENOTDIR  20
#define EISDIR   21
#define EINVAL   22
#define ENFILE   23
#define EMFILE   24
#define ENOTTY   25
#define EFBIG    27
#define ENOSPC   28
#define ESPIPE   29
#define EROFS    30
#define EMLINK   31
#define EPIPE    32
#define EDOM     33
#define ERANGE   34
#define EAGAIN   35
#define EWOULDBLOCK EAGAIN

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_ERRNO_H */
