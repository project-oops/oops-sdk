/*
 * <fcntl.h> - file control options and open().
 *
 * Directs open() through the SDK's file layer (SYS_open). On target hardware,
 * a descriptor from platform libc open() discards writes silently, whereas
 * raw SYS_open creates a descriptor that reliably commits data to storage.
 * See docs/decisions/D013-libc-open-descriptor-discards-writes-route-through-sys-open.md.
 */
#ifndef OOPS_LIBC_FCNTL_H
#define OOPS_LIBC_FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "oops/fs.h"

#define O_RDONLY   OOPS_O_RDONLY
#define O_WRONLY   OOPS_O_WRONLY
#define O_RDWR     OOPS_O_RDWR
#define O_ACCMODE  0x0003
#define O_CREAT    OOPS_O_CREAT
#define O_TRUNC    OOPS_O_TRUNC
#define O_APPEND   OOPS_O_APPEND
/* FreeBSD's value. **Not honoured**: `oops_fs_open` has no exclusive-create, so `O_CREAT|O_EXCL`
 * creates the file whether or not it was already there rather than failing. It is defined because
 * code names it - `ghc::filesystem`'s `copy_file` passes it for the "do not overwrite" case - and
 * a missing macro is a compile error where this is a race nobody here can lose: one process. */
#define O_EXCL     0x0800
/* FreeBSD's value. Not in `oops/fs.h` because the SDK's own filesystem calls have no use for it -
 * a descriptor there is always blocking - but `fcntl` below takes it, and a port that asks for a
 * non-blocking socket asks for it by this name. */
#define O_NONBLOCK 0x0004
/* No exec on this platform for a descriptor to survive, so nothing to close across one. */
#define O_CLOEXEC  0x0000

/*
 * **The `fcntl` commands, which live here because this header is the one that wins.**
 *
 * `oops-apps/common/posix` also ships an `fcntl.h`, and it is *shadowed*: `app.mk` puts the SDK's
 * libc include ahead of the shim's, so a port writing `#include <fcntl.h>` reaches this file. The
 * commands were in the shim and invisible, which read as ioquake3 calling an undeclared `fcntl`
 * while the constants sat in a header on the same include path.
 *
 * Only `F_GETFL` and `F_SETFL` mean anything on this platform - the implementation is in
 * `oops-apps/common/posix/posix.c`, over `oops_set_nonblocking`. The others are defined because
 * programs name them in switch statements that never run.
 */
#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4

int open(const char *path, int flags, ...);
/*
 * Declared here, defined in `oops-apps/common/posix/posix.c` - the same split `clock_gettime` and
 * `nanosleep` have. Honours `F_SETFL`'s `O_NONBLOCK` and fails `EINVAL` on anything else; see that
 * definition for why a general `fcntl` is not on offer.
 */
int fcntl(int fd, int cmd, ...);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_LIBC_FCNTL_H */
