/*
 * errno, over the platform's own. The console carries the FreeBSD-derived POSIX
 * exports, and errno lives behind `__error()` as on FreeBSD; obSCEne measured `__error`
 * callable on firmware 12.40, and `src/net/net.c` reads it the same way.
 *
 * `errno` is a macro over a function returning a pointer, as C requires, which makes it
 * per-thread when the platform's is.
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
 * FreeBSD's numbering, because it is the platform's: `net.c` records try-again as 35 on
 * this console, which is FreeBSD's `EAGAIN`. The whole table is here, generated from
 * the FreeBSD `sys/errno.h` that oops-mesa stages as its target sysroot
 * (`oops-mesa/toolchain/sysroot/usr/include/sys/errno.h`); libc++'s `<system_error>`
 * names the socket and network set unconditionally.
 *
 * FreeBSD does not define the four STREAMS errors (`ENODATA`, `ENOSR`, `ENOSTR`,
 * `ETIME`), and neither does this: libc++ drops those `errc` enumerators when they are
 * missing, and an invented number would be one no syscall returns. The kernel's
 * pseudo-errors (`ERESTART` and the negative values beside it) are not userspace
 * values and are not here.
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
