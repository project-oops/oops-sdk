/*
 * <fcntl.h> - file control options and open().
 *
 * Directs open() through the SDK's file layer (SYS_open): on target hardware a
 * descriptor from the platform libc's open() discards writes silently, while one from
 * SYS_open commits them (D013).
 */
#ifndef OOPS_LIBC_FCNTL_H
#define OOPS_LIBC_FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "oops/fs.h"

#define O_RDONLY OOPS_O_RDONLY
#define O_WRONLY OOPS_O_WRONLY
#define O_RDWR OOPS_O_RDWR
#define O_ACCMODE 0x0003
#define O_CREAT OOPS_O_CREAT
#define O_TRUNC OOPS_O_TRUNC
#define O_APPEND OOPS_O_APPEND
/* FreeBSD's value, not honoured: `oops_fs_open` has no exclusive-create, so
 * `O_CREAT|O_EXCL` creates the file whether or not it already exists. Defined because
 * code names it (`ghc::filesystem`'s `copy_file`), and with one process there is no
 * race to lose. */
#define O_EXCL 0x0800
/* FreeBSD's value, not honoured: `oops_fs_open` cannot open a directory, so this has
 * no effect and opening a directory fails. Defined because libc++'s
 * `src/filesystem/operations.cpp` passes it when walking a tree. */
#define O_DIRECTORY 0x00020000
/* FreeBSD's value. Asks `open` to refuse a symbolic link, and there are none here:
 * `common/posix`'s `symlink` refuses and `S_ISLNK` is never true. */
#define O_NOFOLLOW 0x0100
/* FreeBSD's value. Not in `oops/fs.h`, whose descriptors are always blocking, but
 * `fcntl` below takes it for a non-blocking socket. */
#define O_NONBLOCK 0x0004
/* There is no exec on this platform, so nothing to close across one. */
#define O_CLOEXEC 0x0000

/*
 * The `fcntl` commands live here because this header is the one a compile reaches:
 * `app.mk` puts the SDK's libc include ahead of `oops-apps/common/posix`'s, which
 * shadows that layer's `fcntl.h`.
 *
 * Only `F_GETFL` and `F_SETFL` mean anything on this platform; the implementation is
 * in `oops-apps/common/posix/posix.c`, over `oops_set_nonblocking`. The others are
 * defined because programs name them in switch statements.
 */
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4

/*
 * The `*at()` family's anchor. `openat(dirfd, path, ...)` resolves `path` relative to
 * an open directory descriptor, and there are none here, so the only meaningful
 * `dirfd` is `AT_FDCWD`; the working directory is `/` and cannot change. For that
 * case `openat` is `open`, and for any other it fails with `EBADF`.
 * `AT_SYMLINK_NOFOLLOW` is defined because callers pass it.
 */
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x0200
#define AT_REMOVEDIR 0x0800

int open(const char *path, int flags, ...);
/* Defined in `oops-apps/common/posix/posix.c`, like `fcntl` below. */
int openat(int dirfd, const char *path, int flags, ...);
/*
 * Declared here, defined in `oops-apps/common/posix/posix.c`, like `clock_gettime`
 * and `nanosleep`. Honours `F_SETFL`'s `O_NONBLOCK` and fails `EINVAL` on anything
 * else.
 */
int fcntl(int fd, int cmd, ...);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_FCNTL_H */
