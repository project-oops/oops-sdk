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

#ifdef __cplusplus
}
#endif

#endif /* OOPS_FS_H */

