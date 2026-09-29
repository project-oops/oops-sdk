/*
 * Freestanding high-level filesystem operations.
 */

#include "oops/fs.h"
#include "oops/freestd.h"
#include "oops/heap.h"
#include "oops/memory.h"
#include "oops/syscall.h"
#include "oops/system.h"

#ifdef OOPS_HOST_BUILD
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#endif

/*
 * **The `fs` channel of `/app0/oops-log`** - see `oops_log_channel_level` in `oops/system.h`.
 *
 * File access is gated through the unified logging system:
 * `fs=debug` prints the opens that fail, `fs=trace` prints all operations.
 */
static int fs_open_raw(const char *path, int flags, int mode);

int oops_fs_open(const char *path, int flags, int mode) {
  if (path == NULL) {
    return -1;
  }

  const int fd = fs_open_raw(path, flags, mode);

  if (fd < 0) {
#ifndef OOPS_HOST_BUILD
    oops_log_debug("FS", "open failed errno=%d flags=0x%x %s", sys_get_errno(), flags, path);
#else
    oops_log_debug("FS", "open failed errno=%d flags=0x%x %s", errno, flags, path);
#endif
  } else {
    oops_log_trace("FS", "open fd=%d flags=0x%x %s", fd, flags, path);
  }
  return fd;
}

/*
 * Low-level descriptor open.
 *
 * **Why this bypasses platform libc open() on target** (REQ-20260925T1936Z-6c8d):
 * Measured on physical hardware (gl-cts, GCTS00001, FW 12.40): calling platform libc open()
 * returns a file descriptor where write(), fflush(), and fsync() report success (errno 0),
 * but the bytes are silently discarded and reading back in the same process returns 0 bytes.
 * Direct sys_call(SYS_open, ...) produces a descriptor that reliably commits data to storage.
 * See docs/decisions/D013-libc-open-descriptor-discards-writes-route-through-sys-open.md.
 */
/* Compiled on the host too, and not static, so `tests/unit/test_fs.c` can reach it.
 * It is pure string work with no syscall in it, and the only reason a mistake here ever
 * needed a console to find was that nothing on this side could call it. */
const char *oops_fs_resolve_path(const char *path, char *buf, size_t max);

/*
 * The path as this kernel needs it: absolute, with no `.` or `..` components left.
 *
 * A payload has no working directory the kernel resolves against, so a relative path
 * reaches `SYS_open` unchanged and fails. Ported code hands out relative paths
 * constantly - libzip opened `./soh.o2r` and failed while the file sat at
 * `/app0/soh.o2r`, and `std::filesystem::absolute` had already turned the same name into
 * `/app0/./soh.o2r`, which fails too for the `.` in the middle.
 *
 * This is the one place every path crosses into the kernel, which is why the resolution
 * belongs here rather than in each caller: `open`, `stat` through `oops_fs_exists`,
 * libzip and libc++'s filesystem all arrive at this function.
 *
 * The base is `/app0`, the place a package is mounted, which is what the POSIX layer's
 * `getcwd` reports for the same reason. A path too long to rewrite is passed through
 * untouched, so it fails as written rather than resolving to some other file.
 */
const char *oops_fs_resolve_path(const char *path, char *buf, size_t max) {
  size_t n = 0;
  const char *p;
  int trailing_dot;

  if (!path || !buf || max < 2u) return path;

  /*
   * A trailing `/.` is not noise to be tidied away: it asserts that what precedes it is a
   * directory, and it is how `common/posix`'s `is_directory` asks this kernel that
   * question - a file answers ENOTDIR. Collapsing it makes every regular file open as a
   * directory, and a caller that believed the answer then iterated one: libultraship
   * built a `directory_iterator` over `soh.o2r` and threw.
   */
  {
    size_t plen = 0;
    while (path[plen]) plen++;
    trailing_dot = (plen == 1u && path[0] == '.') ||
                   (plen >= 2u && path[plen - 1u] == '.' && path[plen - 2u] == '/');
  }
  if (path[0] != '/') {
    const char *base = "/app0";
    while (*base && n + 1u < max) buf[n++] = *base++;
  }

  for (p = path; *p;) {
    const char *seg;
    size_t len, i;

    while (*p == '/') p++;
    if (!*p) break;
    seg = p;
    while (*p && *p != '/') p++;
    len = (size_t)(p - seg);

    if (len == 1u && seg[0] == '.') continue;
    if (len == 2u && seg[0] == '.' && seg[1] == '.') {
      while (n > 0u && buf[n - 1u] != '/') n--;
      if (n > 1u) n--; /* the separator too, but never the leading '/' */
      continue;
    }
    if (n + len + 2u >= max) return path;
    buf[n++] = '/';
    for (i = 0; i < len; i++) buf[n++] = seg[i];
  }
  if (n == 0u) buf[n++] = '/';
  if (trailing_dot && n + 3u < max) {
    buf[n++] = '/';
    buf[n++] = '.';
  }
  buf[n] = '\0';
  return buf;
}

static int fs_open_raw(const char *path, int flags, int mode) {
#ifndef OOPS_HOST_BUILD
  char resolved[1024];
  int target_flags = 0;

  path = oops_fs_resolve_path(path, resolved, sizeof(resolved));

  if ((flags & 3) == OOPS_O_RDONLY) target_flags |= 0;
  else if ((flags & 3) == OOPS_O_WRONLY) target_flags |= 1;
  else if ((flags & 3) == OOPS_O_RDWR) target_flags |= 2;

  if (flags & OOPS_O_CREAT) target_flags |= 0x0200;
  if (flags & OOPS_O_TRUNC) target_flags |= 0x0400;
  if (flags & OOPS_O_APPEND) target_flags |= 0x0008;
  if (flags & OOPS_O_DIRECTORY) target_flags |= 0x00020000;

  int target_mode = mode ? mode : 0644;
  return (int)sys_call(SYS_open, (long)path, target_flags, target_mode, 0, 0, 0);
#else
  int host_flags = 0;
  if ((flags & 3) == OOPS_O_RDONLY) host_flags |= O_RDONLY;
  else if ((flags & 3) == OOPS_O_WRONLY) host_flags |= O_WRONLY;
  else if ((flags & 3) == OOPS_O_RDWR) host_flags |= O_RDWR;

  if (flags & OOPS_O_CREAT) host_flags |= O_CREAT;
  if (flags & OOPS_O_TRUNC) host_flags |= O_TRUNC;
  if (flags & OOPS_O_APPEND) host_flags |= O_APPEND;

  mode_t host_mode = mode ? (mode_t)mode : 0644;
  return open(path, host_flags, host_mode);
#endif
}

#ifndef OOPS_HOST_BUILD
#include <stdarg.h>
#include "libc/fcntl.h"

/*
 * open() implementation for target payloads and hosted titles linking liboops.a.
 * Routes open() calls through oops_fs_open() -> SYS_open so ported code writing with
 * open()/fopen() gets a descriptor that actually commits data rather than discarding it.
 */
int open(const char *path, int flags, ...) {
  int mode = 0644;
  if (flags & OOPS_O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  return oops_fs_open(path, flags, mode);
}
#endif

int oops_fs_close(int fd) {
  if (fd < 0) {
    return -1;
  }
  oops_log_trace("FS", "close fd=%d", fd);
#ifndef OOPS_HOST_BUILD
  return (int)sys_call(SYS_close, fd, 0, 0, 0, 0, 0);
#else
  return close(fd);
#endif
}

int64_t oops_fs_read(int fd, void *buf, size_t count) {
  if (fd < 0 || buf == NULL) {
    return -1;
  }
#ifndef OOPS_HOST_BUILD
  int64_t rc = (int64_t)sys_call(SYS_read, fd, (long)buf, (long)count, 0, 0, 0);
#else
  int64_t rc = (int64_t)read(fd, buf, count);
#endif
  oops_log_trace("FS", "read fd=%d count=%zu rc=%ld", fd, count, (long)rc);
  return rc;
}

int64_t oops_fs_write(int fd, const void *buf, size_t count) {
  if (fd < 0 || (buf == NULL && count > 0)) {
    return -1;
  }
#ifndef OOPS_HOST_BUILD
  int64_t rc = (int64_t)sys_call(SYS_write, fd, (long)buf, (long)count, 0, 0, 0);
#else
  int64_t rc = (int64_t)write(fd, buf, count);
#endif
  oops_log_trace("FS", "write fd=%d count=%zu rc=%ld", fd, count, (long)rc);
  return rc;
}

int64_t oops_fs_seek(int fd, int64_t offset, int whence) {
  if (fd < 0) {
    return -1;
  }
#ifndef OOPS_HOST_BUILD
  int64_t rc = (int64_t)sys_call(SYS_lseek, fd, offset, whence, 0, 0, 0);
#else
  int host_whence = SEEK_SET;
  if (whence == OOPS_SEEK_CUR) host_whence = SEEK_CUR;
  else if (whence == OOPS_SEEK_END) host_whence = SEEK_END;
  int64_t rc = (int64_t)lseek(fd, (off_t)offset, host_whence);
#endif
  oops_log_trace("FS", "seek fd=%d offset=%ld whence=%d rc=%ld", fd, (long)offset, whence, (long)rc);
  return rc;
}

int64_t oops_fs_tell(int fd) {
  return oops_fs_seek(fd, 0, OOPS_SEEK_CUR);
}

int oops_fs_exists(const char *path) {
  if (path == NULL) {
    return 0;
  }
  int fd = oops_fs_open(path, OOPS_O_RDONLY, 0);
  if (fd >= 0) {
    oops_fs_close(fd);
    return 1;
  }
  return 0;
}

int64_t oops_fs_file_size(const char *path) {
  if (path == NULL) {
    return -1;
  }
  int fd = oops_fs_open(path, OOPS_O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }
  int64_t sz = oops_fs_seek(fd, 0, OOPS_SEEK_END);
  oops_fs_close(fd);
  return sz;
}

int oops_fs_read_all(const char *path, void **out_data, size_t *out_size) {
  if (path == NULL || out_data == NULL || out_size == NULL) {
    return -1;
  }
  *out_data = NULL;
  *out_size = 0;

  int fd = oops_fs_open(path, OOPS_O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  int64_t sz = oops_fs_seek(fd, 0, OOPS_SEEK_END);
  if (sz < 0) {
    oops_fs_close(fd);
    return -1;
  }
  (void)oops_fs_seek(fd, 0, OOPS_SEEK_SET);

#ifndef OOPS_HOST_BUILD
  void *buf = oops_malloc((size_t)sz + 1);
#else
  void *buf = malloc((size_t)sz + 1);
#endif
  if (buf == NULL) {
    oops_fs_close(fd);
    return -2;
  }

  size_t total_read = 0;
  while (total_read < (size_t)sz) {
    int64_t n = oops_fs_read(fd, (char *)buf + total_read, (size_t)sz - total_read);
    if (n <= 0) {
      break;
    }
    total_read += (size_t)n;
  }
  oops_fs_close(fd);

  ((char *)buf)[total_read] = '\0';
  *out_data = buf;
  *out_size = total_read;
  return 0;
}

void oops_fs_free_data(void *data) {
  if (data == NULL) {
    return;
  }
#ifndef OOPS_HOST_BUILD
  oops_free(data);
#else
  free(data);
#endif
}

int oops_fs_write_all(const char *path, const void *data, size_t size) {
  if (path == NULL || (data == NULL && size > 0)) {
    return -1;
  }

  int fd = oops_fs_open(path, OOPS_O_WRONLY | OOPS_O_CREAT | OOPS_O_TRUNC, 0644);
  if (fd < 0) {
    return -1;
  }

  size_t total_written = 0;
  while (total_written < size) {
    int64_t n = oops_fs_write(fd, (const char *)data + total_written, size - total_written);
    if (n <= 0) {
      break;
    }
    total_written += (size_t)n;
  }
  oops_fs_close(fd);

  return (total_written == size) ? 0 : -2;
}

int oops_fs_mkdir(const char *path, int mode) {
  if (path == NULL) {
    return -1;
  }
#ifndef OOPS_HOST_BUILD
  /* Resolved, as `oops_fs_rename` explains. */
  char resolved[1024];
  int target_mode = mode ? mode : 0755;
  path = oops_fs_resolve_path(path, resolved, sizeof(resolved));
  int rc = (int)sys_call(SYS_mkdir, (long)path, target_mode, 0, 0, 0, 0);
#else
  mode_t host_mode = mode ? (mode_t)mode : 0755;
  int rc = mkdir(path, host_mode);
#endif
  oops_log_debug("FS", "mkdir path=%s rc=%d", path, rc);
  return rc;
}

int oops_fs_rename(const char *from, const char *to) {
  if (from == NULL || to == NULL) {
    return -1;
  }
#ifndef OOPS_HOST_BUILD
  /*
   * Both paths resolved, for the reason `fs_open_raw` resolves its one: a payload has no
   * working directory, so a relative name reaches the kernel unchanged and fails. Opening
   * has resolved since the archive was reported missing while it sat in /app0; these five
   * calls never did, and a caller mixing them looks as though only *some* of its file
   * operations work.
   *
   * This is what stranded `oot.o2r`. libultraship writes an archive to `<name>.<random>.part`
   * and renames it into place, ZAPD is handed `--otrfile oot.o2r` relative, and the rename
   * was the one step that could not see /app0. The conversion ran to 547 of 547 and left a
   * `.part` file behind with nothing to say why.
   */
  char from_buf[1024];
  char to_buf[1024];
  from = oops_fs_resolve_path(from, from_buf, sizeof(from_buf));
  to = oops_fs_resolve_path(to, to_buf, sizeof(to_buf));
  int rc = (int)sys_call(SYS_rename, (long)from, (long)to, 0, 0, 0, 0);
#else
  int rc = rename(from, to);
#endif
  oops_log_debug("FS", "rename %s -> %s rc=%d", from, to, rc);
  return rc;
}

int oops_fs_chmod(const char *path, int mode) {
  if (path == NULL) {
    return -1;
  }
#ifndef OOPS_HOST_BUILD
  /* Resolved, as `oops_fs_rename` explains. */
  char resolved[1024];
  path = oops_fs_resolve_path(path, resolved, sizeof(resolved));
  int rc = (int)sys_call(SYS_chmod, (long)path, mode, 0, 0, 0, 0);
#else
  int rc = chmod(path, (mode_t)mode);
#endif
  oops_log_debug("FS", "chmod path=%s mode=0%o rc=%d", path, (unsigned)mode, rc);
  return rc;
}

int oops_fs_rmdir(const char *path) {
  if (path == NULL) {
    return -1;
  }
#ifndef OOPS_HOST_BUILD
  /* Resolved, as `oops_fs_rename` explains. */
  char resolved[1024];
  path = oops_fs_resolve_path(path, resolved, sizeof(resolved));
  int rc = (int)sys_call(SYS_rmdir, (long)path, 0, 0, 0, 0, 0);
#else
  int rc = rmdir(path);
#endif
  oops_log_debug("FS", "rmdir path=%s rc=%d", path, rc);
  return rc;
}

int oops_fs_rmtree(const char *path) {
  if (path == NULL) {
    return -1;
  }
  oops_dir_t *dir = oops_fs_opendir(path);
  if (dir != NULL) {
    oops_dirent_t ent;
    while (oops_fs_readdir(dir, &ent) == 1) {
      if (ent.name[0] == '.' &&
          (ent.name[1] == '\0' || (ent.name[1] == '.' && ent.name[2] == '\0'))) {
        continue; /* skip "." and ".." */
      }
      char child[1024];
      int n = 0;
      for (const char *p = path; *p != '\0' && n < (int)sizeof(child) - 1; p++) {
        child[n++] = *p;
      }
      if (n > 0 && child[n - 1] != '/' && n < (int)sizeof(child) - 1) {
        child[n++] = '/';
      }
      for (const char *p = ent.name; *p != '\0' && n < (int)sizeof(child) - 1; p++) {
        child[n++] = *p;
      }
      child[n] = '\0';
      if (ent.is_directory) {
        oops_fs_rmtree(child);
      } else {
        oops_fs_unlink(child);
      }
    }
    oops_fs_closedir(dir);
  }
  int rc = oops_fs_rmdir(path);
  if (rc != 0 && !oops_fs_exists(path)) {
    rc = 0; /* a tree that is already gone is the desired end state */
  }
  return rc;
}

/* ---------------------------------------------------------------------------
 * Walking a directory
 *
 * `getdents` fills a buffer with variable-length records and returns the bytes written, zero at
 * the end of the directory. Each record carries its own length, so the buffer is walked by
 * stepping `d_reclen` at a time rather than by a fixed stride - a `d_reclen` of zero would be a
 * malformed record and an infinite loop, so it is checked.
 *
 * The record layout is FreeBSD's `struct dirent`, which is what the kernel here speaks: a 32-bit
 * inode, a 16-bit record length, an 8-bit type, an 8-bit name length, then the name. It is
 * declared locally rather than taken from a header because this SDK has no `<dirent.h>` and
 * should not grow one for a struct only this file reads.
 *
 * One buffer is filled per `getdents` call and drained across several `readdir` calls, which is
 * what makes a directory of any size cost a fixed amount of memory.
 * --------------------------------------------------------------------------- */

#ifndef OOPS_HOST_BUILD

/*
 * 64K, because a smaller buffer is not a smaller read - it is a refusal.
 *
 * `getdents` does not fill a buffer up to whatever size it is given and stop: it returns a
 * whole block of directory entries or `EINVAL`, and this filesystem's blocks are larger
 * than 4K. Both readers answered `0x80020016` - libkernel's `0x80020000 | errno`, so errno
 * 22 - for every directory on the console, which read as "empty directory" all the way up
 * through `readdir` to `std::filesystem::directory_iterator`.
 *
 * The buffer sits in the `oops_dir` this allocates per open directory, not on a stack, so
 * the cost is 64K while a directory is being walked and nothing at all otherwise.
 */
#define OOPS_DIRENT_BUF 65536
#define OOPS_DT_DIR 4 /* FreeBSD's DT_DIR */

struct oops_bsd_dirent {
  uint32_t d_fileno;
  uint16_t d_reclen;
  uint8_t d_type;
  uint8_t d_namlen;
  char d_name[256];
};

/*
 * Who fills that buffer.
 *
 * This asked `sys_call(SYS_getdents)` and nothing else, which was wrong twice over. This
 * kernel does not answer syscall 272 - `obscene/data/obscene-report.txt` carries no
 * `getdents` among the exports - and the failure was silent: `opendir` succeeded,
 * `readdir` returned "end of directory" on its first call, and a directory with files in
 * it read as empty. Ship of Harkinian looked for the ROM sitting beside its own
 * `eboot.bin` and did not see it, and said so in a message that was wrong.
 *
 * What the platform does export is `sceKernelGetdents` and `sceKernelGetdirentries`
 * (obscene-report.txt:577-578, both `present|shared`), and `oops-apps/common/symbols.txt`
 * already named the first for the loader - it was declared and never called. Both are
 * asked for by name, with the raw syscall kept last so a kernel that does answer it still
 * works, and a route that answers nothing is a warning rather than an empty directory.
 */
__attribute__((weak)) int sceKernelGetdents(int fd, char *buf, int nbytes);
__attribute__((weak)) int sceKernelGetdirentries(int fd, char *buf, int nbytes,
                                                 long *basep);

#define OOPS_DIR_ROUTE_GETDENTS 1
#define OOPS_DIR_ROUTE_GETDIRENTRIES 2
#define OOPS_DIR_ROUTE_SYSCALL 3

/* Which route answered, chosen once and then kept. Directory reading is not per-title
 * behaviour, so one process needs to work this out once. */
static int s_dir_route;

#endif

struct oops_dir {
  int fd;
#ifndef OOPS_HOST_BUILD
  int used;   /* bytes of buf that getdents filled */
  int offset; /* how far through buf readdir has walked */
  char buf[OOPS_DIRENT_BUF];
#endif
};

oops_dir_t *oops_fs_opendir(const char *path) {
  if (path == NULL) {
    return NULL;
  }

  oops_dir_t *dir = (oops_dir_t *)oops_malloc(sizeof(*dir));
  if (dir == NULL) {
    oops_log_warn("FS", "opendir: out of memory allocating dir context for %s", path);
    return NULL;
  }

  /*
   * `O_DIRECTORY` first, then plain read-only.
   *
   * A plain read-only open of a directory succeeds here and hands back a descriptor that
   * every directory reader then refuses - which is how a directory with files in it read as
   * empty. Asking for a directory says what the descriptor is for, and a platform that does
   * not know the flag refuses the open rather than answering wrongly, so the fallback is
   * safe and the two together cost one extra syscall on the platforms that need it.
   */
  dir->fd = oops_fs_open(path, OOPS_O_RDONLY | OOPS_O_DIRECTORY, 0);
  if (dir->fd < 0) {
    dir->fd = oops_fs_open(path, OOPS_O_RDONLY, 0);
  }
  if (dir->fd < 0) {
    oops_log_debug("FS", "opendir failed to open %s", path);
    oops_free(dir);
    return NULL;
  }
#ifndef OOPS_HOST_BUILD
  dir->used = 0;
  dir->offset = 0;
#endif
  oops_log_debug("FS", "opendir opened %s (fd=%d)", path, dir->fd);
  return dir;
}

#ifndef OOPS_HOST_BUILD

static long oops_dir_read_route(int route, oops_dir_t *dir) {
  long base = 0;

  switch (route) {
    case OOPS_DIR_ROUTE_GETDENTS:
      if (sceKernelGetdents == NULL) {
        return -1;
      }
      return (long)sceKernelGetdents(dir->fd, dir->buf, OOPS_DIRENT_BUF);
    case OOPS_DIR_ROUTE_GETDIRENTRIES:
      if (sceKernelGetdirentries == NULL) {
        return -1;
      }
      return (long)sceKernelGetdirentries(dir->fd, dir->buf, OOPS_DIRENT_BUF, &base);
    default:
      return sys_call(SYS_getdents, dir->fd, (long)dir->buf, OOPS_DIRENT_BUF, 0, 0, 0);
  }
}

static const char *oops_dir_route_name(int route) {
  switch (route) {
    case OOPS_DIR_ROUTE_GETDENTS:
      return "sceKernelGetdents";
    case OOPS_DIR_ROUTE_GETDIRENTRIES:
      return "sceKernelGetdirentries";
    default:
      return "sys_call(getdents)";
  }
}

/*
 * Fill the buffer, choosing a route the first time.
 *
 * While choosing, only a positive count settles it: a route that is not there answers zero
 * as readily as an empty directory does, and taking that for the end is exactly the
 * failure this replaced. So the choice needs bytes, and a directory that really is empty
 * costs one pass down the list and then reads as empty, which it is.
 */
static long oops_dir_fill(oops_dir_t *dir) {
  int route;
  int saw_end = 0;

  if (s_dir_route != 0) {
    return oops_dir_read_route(s_dir_route, dir);
  }

  for (route = OOPS_DIR_ROUTE_GETDENTS; route <= OOPS_DIR_ROUTE_SYSCALL; route++) {
    long n = oops_dir_read_route(route, dir);
    if (n > 0) {
      s_dir_route = route;
      oops_log_info("FS", "directories are read through %s", oops_dir_route_name(route));
      return n;
    }
    if (n == 0) {
      saw_end = 1;
    } else {
      /* Say what each one answered, not just that none worked. "Refused" alone sent a day
       * after the descriptor when the reader was fine, and the other way round. */
      oops_log_warn("FS", "%s(fd=%d) -> %ld, errno=%d", oops_dir_route_name(route),
                    dir->fd, n, sys_get_errno());
    }
  }

  if (saw_end) {
    return 0;
  }
  oops_log_warn("FS",
                "no directory reader answered for fd=%d - sceKernelGetdents %s, "
                "sceKernelGetdirentries %s",
                dir->fd, (sceKernelGetdents != NULL) ? "bound" : "unbound",
                (sceKernelGetdirentries != NULL) ? "bound" : "unbound");
  return -1;
}

#endif

int oops_fs_readdir(oops_dir_t *dir, oops_dirent_t *out) {
  if (dir == NULL || out == NULL) {
    return -1;
  }

#ifndef OOPS_HOST_BUILD
  for (;;) {
    if (dir->offset >= dir->used) {
      /* Buffer drained: ask for more. Zero means the directory is finished. */
      long n = oops_dir_fill(dir);
      if (n < 0) {
        return -1;
      }
      if (n == 0) {
        return 0;
      }
      dir->used = (int)n;
      dir->offset = 0;
    }

    const struct oops_bsd_dirent *ent =
        (const struct oops_bsd_dirent *)(const void *)(dir->buf + dir->offset);

    if (ent->d_reclen == 0 || (int)ent->d_reclen > dir->used - dir->offset) {
      /* A record that does not fit or does not advance is a malformed buffer, not an entry.
         Stopping is the only safe answer; carrying on would loop for ever. */
      return -1;
    }
    dir->offset += (int)ent->d_reclen;

    /* A zero inode is a deleted entry the kernel left in place - skip it and take the next. */
    if (ent->d_fileno == 0u) {
      continue;
    }

    unsigned int len = ent->d_namlen;
    if (len >= sizeof(out->name)) {
      len = (unsigned int)sizeof(out->name) - 1u;
    }
    for (unsigned int i = 0; i < len; i++) {
      out->name[i] = ent->d_name[i];
    }
    out->name[len] = '\0';
    out->is_directory = (ent->d_type == OOPS_DT_DIR) ? 1 : 0;
    oops_log_trace("FS", "readdir entry: %s (is_dir=%d)", out->name, out->is_directory);
    return 1;
  }
#else
  (void)dir;
  (void)out;
  return -1; /* the host build has no use for this and does not pretend otherwise */
#endif
}

int oops_fs_closedir(oops_dir_t *dir) {
  if (dir == NULL) {
    return -1;
  }
  int rc = oops_fs_close(dir->fd);
  oops_free(dir);
  oops_log_debug("FS", "closedir rc=%d", rc);
  return rc;
}

int oops_fs_unlink(const char *path) {
  if (path == NULL) {
    return -1;
  }
#ifndef OOPS_HOST_BUILD
  /* Resolved, as `oops_fs_rename` explains. */
  char resolved[1024];
  path = oops_fs_resolve_path(path, resolved, sizeof(resolved));
  int rc = (int)sys_call(SYS_unlink, (long)path, 0, 0, 0, 0, 0);
#else
  int rc = unlink(path);
#endif
  oops_log_debug("FS", "unlink %s rc=%d", path, rc);
  return rc;
}

static int oops_fs_mkdir_p(const char *path) {
  if (!path || path[0] == '\0') {
    return -1;
  }
  char tmp[256];
  size_t len = obs_strlen(path);
  if (len >= sizeof(tmp)) {
    return -1;
  }
  for (size_t i = 0; i < len; i++) {
    tmp[i] = path[i];
    if (i > 0 && (path[i] == '/' || path[i] == '\\')) {
      tmp[i] = '\0';
      if (!oops_fs_exists(tmp)) {
        (void)oops_fs_mkdir(tmp, 0755);
      }
      tmp[i] = path[i];
    }
  }
  if (!oops_fs_exists(path)) {
    (void)oops_fs_mkdir(path, 0755);
  }
  return oops_fs_exists(path) ? 0 : -1;
}

int oops_fs_get_storage_dir(oops_storage_location_t loc, char *out_path, size_t max_len) {
  if (!out_path || max_len == 0) {
    return -1;
  }
  out_path[0] = '\0';

  const char *app_id = oops_log_get_app_id();
  if (!app_id || app_id[0] == '\0') {
    app_id = "default";
  }

  char resolved[256];
  resolved[0] = '\0';

#ifndef OOPS_HOST_BUILD
  int usb0_ok = oops_fs_exists("/mnt/usb0");
  int usb1_ok = oops_fs_exists("/mnt/usb1");

  if (loc == OOPS_STORAGE_USB || loc == OOPS_STORAGE_PREFER_USB) {
    if (usb0_ok) {
      (void)oops_snprintf(resolved, sizeof(resolved), "/mnt/usb0/%s", app_id);
    } else if (usb1_ok) {
      (void)oops_snprintf(resolved, sizeof(resolved), "/mnt/usb1/%s", app_id);
    } else if (loc == OOPS_STORAGE_USB) {
      return -1;
    }
  }

  if (resolved[0] == '\0') {
    /* Refuses rather than silently unmounting the package. See
     * `oops_system_allow_sandbox_escape`.
     *
     * Loud for OOPS_STORAGE_APP_DATA, which asked for /data and is being told no. Quiet for
     * OOPS_STORAGE_PREFER_USB, which asked for a USB stick and said what to fall back to: no stick
     * and no escape is that caller's ordinary answer, not a fault. `oops_log_enable_disk_sink` is
     * the one that matters - every title calls it at entry, and six of them opened with this as an
     * ERROR while nothing was wrong. */
    if (!oops_system_sandbox_escape_allowed()) {
      if (loc == OOPS_STORAGE_PREFER_USB) {
        oops_log_debug("FS", "no USB mounted, and /data is outside the sandbox: no storage dir");
      } else {
        oops_log_error("FS",
                       "refusing to leave the sandbox for /data: it unmounts /app0 and every asset "
                       "in it. Write to /app0, which is writable, or call "
                       "oops_system_allow_sandbox_escape() if this title accepts losing them.");
      }
      return -1;
    }
    (void)oops_system_escape_sandbox();
    if (oops_fs_exists("/data")) {
      (void)oops_snprintf(resolved, sizeof(resolved), "/data/%s", app_id);
    } else {
      (void)oops_snprintf(resolved, sizeof(resolved), "data/%s", app_id);
    }
  }
#else
  if (loc == OOPS_STORAGE_USB) {
    if (oops_fs_exists("/mnt/usb0")) {
      (void)oops_snprintf(resolved, sizeof(resolved), "/mnt/usb0/%s", app_id);
    } else if (oops_fs_exists("usb0")) {
      (void)oops_snprintf(resolved, sizeof(resolved), "usb0/%s", app_id);
    } else {
      return -1;
    }
  } else {
    (void)oops_snprintf(resolved, sizeof(resolved), "data/%s", app_id);
  }
#endif

  if (oops_fs_mkdir_p(resolved) != 0) {
    return -1;
  }

  size_t rlen = obs_strlen(resolved);
  if (rlen + 1 > max_len) {
    return -1;
  }
  for (size_t i = 0; i <= rlen; i++) {
    out_path[i] = resolved[i];
  }
  oops_log_info("FS", "storage dir resolved to %s", out_path);
  return 0;
}

int oops_fs_storage_path(oops_storage_location_t loc, const char *rel_path,
                         char *out_path, size_t max_len) {
  if (!rel_path || !out_path || max_len == 0) {
    return -1;
  }
  char base[256];
  if (oops_fs_get_storage_dir(loc, base, sizeof(base)) != 0) {
    return -1;
  }
  while (*rel_path == '/' || *rel_path == '\\') {
    rel_path++;
  }
  int written = oops_snprintf(out_path, max_len, "%s/%s", base, rel_path);
  return (written > 0 && (size_t)written < max_len) ? 0 : -1;
}

