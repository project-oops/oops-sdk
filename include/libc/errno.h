/*
 * errno, over the platform's own.
 *
 * **This is a bridge, not an invention.** The console carries the FreeBSD-derived POSIX
 * exports, and errno lives behind `__error()` exactly as it does on FreeBSD -
 * `src/net/net.c` has read it that way since the socket work, and obSCEne measured
 * `__error` callable on firmware 12.40 (sweep 20260909-083918) alongside the rest of
 * the POSIX set.
 *
 * It arrives now because three separate consumers wanted it and each was shimming its
 * own: SDL2's `SDL_RWops` reaches for `<errno.h>` on the stdio path, Extreme Tux
 * Racer's `DirExists` reads `errno` after a failed `opendir`, and libc++'s `string.cpp`
 * compares it against `ERANGE` in `std::stoi`. Three private answers to one standard
 * question is the signal that the question belongs here.
 *
 * `errno` is a macro over a function returning a pointer, which is what C requires of
 * it and what makes it work per-thread when the platform's does.
 */
#ifndef OOPS_LIBC_ERRNO_H
#define OOPS_LIBC_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The location of the calling thread's errno. Resolves to the platform's `__error()`
 * when that import binds, and to a single process-wide slot when it does not - so
 * reading `errno` is always safe, and the fallback is a value this SDK's own calls can
 * still set rather than a fault. `src/system/libc.c` has the detail.
 */
int *oops_errno_location(void);

#define errno (*oops_errno_location())

/*
 * **FreeBSD's numbering, because it is the platform's.** Not a set this SDK chose:
 * `net.c` records try-again as 35 on this console, which is FreeBSD's `EAGAIN` and pins
 * the whole table to that origin. A program comparing `errno` against a constant from
 * somewhere else would be comparing against the wrong number.
 *
 * **The whole table, not the part someone needed.** This began as the thirty-odd names
 * the first three consumers happened to touch, and stopped at `EAGAIN` - which is where
 * the numbering only starts getting interesting, because everything above 35 is the
 * socket and network set. libc++'s
 * `<system_error>` maps forty-two of those onto `std::errc` and names them
 * unconditionally, so Extreme Tux Racer could not compile a single translation unit
 * that reached it: twenty errors and `-ferror-limit` giving up, on `ECONNREFUSED`,
 * `ECONNRESET`, `EDESTADDRREQ` and the rest.
 *
 * A partial table is how this file has failed twice now - once as three private shims,
 * once here
 * - and completing it costs nothing, because these are not choices to be made one at a
 * time. They are transcribed from the FreeBSD `sys/errno.h` that oops-mesa stages as
 * its target sysroot
 * (`oops-mesa/toolchain/sysroot/usr/include/sys/errno.h`), which is the platform's own
 * header rather than a recollection of it, and generated from it rather than typed.
 *
 * **What is deliberately absent.** FreeBSD does not define the four STREAMS errors -
 * `ENODATA`, `ENOSR`, `ENOSTR`, `ETIME` - and neither does this. libc++ guards all four
 * with `#ifdef` and drops those `errc` enumerators when they are missing, which is the
 * correct outcome: a number invented here would be one no syscall on this console will
 * ever return. The kernel's own pseudo-errors (`ERESTART` and the negative values
 * beside it) are likewise not userspace values and are not here.
 */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EDEADLK 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define ENOTBLK 15
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define ETXTBSY 26
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define EAGAIN 35
#define EWOULDBLOCK EAGAIN
#define EINPROGRESS 36
#define EALREADY 37
#define ENOTSOCK 38
#define EDESTADDRREQ 39
#define EMSGSIZE 40
#define EPROTOTYPE 41
#define ENOPROTOOPT 42
#define EPROTONOSUPPORT 43
#define ESOCKTNOSUPPORT 44
#define EOPNOTSUPP 45
#define ENOTSUP EOPNOTSUPP
#define EPFNOSUPPORT 46
#define EAFNOSUPPORT 47
#define EADDRINUSE 48
#define EADDRNOTAVAIL 49
#define ENETDOWN 50
#define ENETUNREACH 51
#define ENETRESET 52
#define ECONNABORTED 53
#define ECONNRESET 54
#define ENOBUFS 55
#define EISCONN 56
#define ENOTCONN 57
#define ESHUTDOWN 58
#define ETOOMANYREFS 59
#define ETIMEDOUT 60
#define ECONNREFUSED 61
#define ELOOP 62
#define ENAMETOOLONG 63
#define EHOSTDOWN 64
#define EHOSTUNREACH 65
#define ENOTEMPTY 66
#define EPROCLIM 67
#define EUSERS 68
#define EDQUOT 69
#define ESTALE 70
#define EREMOTE 71
#define EBADRPC 72
#define ERPCMISMATCH 73
#define EPROGUNAVAIL 74
#define EPROGMISMATCH 75
#define EPROCUNAVAIL 76
#define ENOLCK 77
#define ENOSYS 78
#define EFTYPE 79
#define EAUTH 80
#define ENEEDAUTH 81
#define EIDRM 82
#define ENOMSG 83
#define EOVERFLOW 84
#define ECANCELED 85
#define EILSEQ 86
#define ENOATTR 87
#define EDOOFUS 88
#define EBADMSG 89
#define EMULTIHOP 90
#define ENOLINK 91
#define EPROTO 92
#define ENOTCAPABLE 93
#define ECAPMODE 94
#define ENOTRECOVERABLE 95
#define EOWNERDEAD 96
#define EINTEGRITY 97

/* The largest errno the platform defines, for a caller bounding a table by it. FreeBSD
 * guards this one behind `_POSIX_SOURCE`; nothing freestanding here defines that, so it
 * is plain. */
#define ELAST 97

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_ERRNO_H */
