#ifndef OOPS_FS_H
#define OOPS_FS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* File open flags (matching standard FreeBSD / POSIX flags) */
#define OOPS_O_RDONLY 0x0000
#define OOPS_O_WRONLY 0x0001
#define OOPS_O_RDWR   0x0002
#define OOPS_O_CREAT  0x0200
#define OOPS_O_TRUNC  0x0400
#define OOPS_O_APPEND 0x0008

/* Seek origins */
#define OOPS_SEEK_SET 0
#define OOPS_SEEK_CUR 1
#define OOPS_SEEK_END 2

/* File and directory info */
typedef struct oops_file_info {
  int64_t size;
  int is_directory;
} oops_file_info_t;

/* Low-level file descriptor operations */
int oops_fs_open(const char *path, int flags, int mode);
int oops_fs_close(int fd);
int64_t oops_fs_read(int fd, void *buf, size_t count);
int64_t oops_fs_write(int fd, const void *buf, size_t count);
int64_t oops_fs_seek(int fd, int64_t offset, int whence);
int64_t oops_fs_tell(int fd);

/* Metadata & presence checks */
int oops_fs_exists(const char *path);
int64_t oops_fs_file_size(const char *path);

/* High-level whole-file helpers (assets, configs, saves) */
int oops_fs_read_all(const char *path, void **out_data, size_t *out_size);
void oops_fs_free_data(void *data);
int oops_fs_write_all(const char *path, const void *data, size_t size);

/* Directory and file manipulation */
int oops_fs_mkdir(const char *path, int mode);
int oops_fs_unlink(const char *path);
int oops_fs_rename(const char *from, const char *to);

/*
 * **Walking a directory** (2026-09-22).
 *
 * This was absent for no better reason than that nothing had asked for it - `SYS_getdents` has
 * been in `<oops/syscall.h>` the whole time, next to the `SYS_mkdir` that `oops_fs_mkdir`
 * already uses. Neverball asked: `share/dir.c` lists levels, sets and replays, which is what a
 * game with user content does.
 *
 * The shape is the POSIX one because that is what a port expects and what the kernel gives:
 * open a directory, read entries until there are none, close it. `oops_fs_opendir` returns a
 * handle or NULL; `oops_fs_readdir` fills `out` and returns 1 for an entry, 0 at the end and
 * -1 on error, so a caller can tell "finished" from "failed" - which a NULL-or-not API cannot.
 *
 * `.` and `..` are **returned**, not filtered. They are directory entries and a caller that
 * wants them gone says so; hiding them here would be this SDK deciding what a port's file list
 * means.
 */
typedef struct oops_dir oops_dir_t;

typedef struct oops_dirent {
  char name[256];   /* NUL-terminated; the kernel's own limit is 255 */
  int is_directory; /* 1 when the entry is itself a directory */
} oops_dirent_t;

oops_dir_t *oops_fs_opendir(const char *path);
int oops_fs_readdir(oops_dir_t *dir, oops_dirent_t *out);
int oops_fs_closedir(oops_dir_t *dir);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_FS_H */

