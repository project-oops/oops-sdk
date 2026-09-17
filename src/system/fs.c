/*
 * Freestanding high-level filesystem operations.
 */

#include "oops/fs.h"
#include "oops/freestd.h"
#include "oops/heap.h"
#include "oops/memory.h"
#include "oops/syscall.h"

#ifdef OOPS_HOST_BUILD
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#endif

int oops_fs_open(const char *path, int flags, int mode) {
  if (path == NULL) {
    return -1;
  }

#ifndef OOPS_HOST_BUILD
  int target_flags = 0;
  if ((flags & 3) == OOPS_O_RDONLY) target_flags |= 0;
  else if ((flags & 3) == OOPS_O_WRONLY) target_flags |= 1;
  else if ((flags & 3) == OOPS_O_RDWR) target_flags |= 2;

  if (flags & OOPS_O_CREAT) target_flags |= 0x0200;
  if (flags & OOPS_O_TRUNC) target_flags |= 0x0400;
  if (flags & OOPS_O_APPEND) target_flags |= 0x0008;

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

int oops_fs_close(int fd) {
  if (fd < 0) {
    return -1;
  }
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
  return (int64_t)sys_call(SYS_read, fd, (long)buf, (long)count, 0, 0, 0);
#else
  return (int64_t)read(fd, buf, count);
#endif
}

int64_t oops_fs_write(int fd, const void *buf, size_t count) {
  if (fd < 0 || (buf == NULL && count > 0)) {
    return -1;
  }
#ifndef OOPS_HOST_BUILD
  return (int64_t)sys_call(SYS_write, fd, (long)buf, (long)count, 0, 0, 0);
#else
  return (int64_t)write(fd, buf, count);
#endif
}

int64_t oops_fs_seek(int fd, int64_t offset, int whence) {
  if (fd < 0) {
    return -1;
  }
#ifndef OOPS_HOST_BUILD
  return (int64_t)sys_call(SYS_lseek, fd, offset, whence, 0, 0, 0);
#else
  int host_whence = SEEK_SET;
  if (whence == OOPS_SEEK_CUR) host_whence = SEEK_CUR;
  else if (whence == OOPS_SEEK_END) host_whence = SEEK_END;
  return (int64_t)lseek(fd, (off_t)offset, host_whence);
#endif
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
  int target_mode = mode ? mode : 0755;
  return (int)sys_call(SYS_mkdir, (long)path, target_mode, 0, 0, 0, 0);
#else
  mode_t host_mode = mode ? (mode_t)mode : 0755;
  return mkdir(path, host_mode);
#endif
}

int oops_fs_unlink(const char *path) {
  if (path == NULL) {
    return -1;
  }
#ifndef OOPS_HOST_BUILD
  return (int)sys_call(SYS_unlink, (long)path, 0, 0, 0, 0, 0);
#else
  return unlink(path);
#endif
}
