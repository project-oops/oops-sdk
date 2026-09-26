/*
 * Files and directories over the platform's syscalls: descriptors, whole-file helpers,
 * directory walking, and the application's writable storage locations.
 */
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
#define OOPS_O_RDWR 0x0002
#define OOPS_O_CREAT 0x0200
#define OOPS_O_TRUNC 0x0400
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
int oops_fs_chmod(const char *path, int mode);
int oops_fs_rmdir(const char *path);

/* Recursively delete a directory and everything under it. Returns 0 when the tree is
 * gone (including when it never existed), non-zero if something could not be removed.
 */
int oops_fs_rmtree(const char *path);

/*
 * Walking a directory, in the POSIX shape over `SYS_getdents`: open, read entries until
 * there are none, close. `oops_fs_opendir` returns a handle or NULL; `oops_fs_readdir`
 * fills `out` and returns 1 for an entry, 0 at the end and -1 on error, so a caller
 * can tell finished from failed. `.` and `..` are returned, not filtered.
 */
typedef struct oops_dir oops_dir_t;

typedef struct oops_dirent {
    char name[256];   /* NUL-terminated; the kernel's own limit is 255 */
    int is_directory; /* 1 when the entry is itself a directory */
} oops_dirent_t;

oops_dir_t *oops_fs_opendir(const char *path);
int oops_fs_readdir(oops_dir_t *dir, oops_dirent_t *out);
int oops_fs_closedir(oops_dir_t *dir);

/* Storage locations for application persistence & data */
typedef enum oops_storage_location {
    OOPS_STORAGE_APP_DATA, /* Internal persistent storage: /data/<app_id> (escaped) or
                              ./data/<app_id> */
    OOPS_STORAGE_USB, /* External USB storage: /mnt/usb0/<app_id> or /mnt/usb1/<app_id>
                       */
    OOPS_STORAGE_PREFER_USB, /* USB storage if mounted, else internal /data/<app_id> */
} oops_storage_location_t;

/**
 * Resolve and ensure (create) a writable directory for application storage.
 * On target hardware, accessing /data or /mnt/usb automatically ensures
 * sandbox escape privileges.
 *
 * loc: Storage location preference.
 * out_path: Buffer receiving the resolved absolute directory path.
 * max_len: Capacity of out_path buffer.
 *
 * Returns: 0 on success, or negative error code.
 */
int oops_fs_get_storage_dir(oops_storage_location_t loc, char *out_path,
                            size_t max_len);

/**
 * Format a full path to a file inside the resolved application storage directory.
 *
 * loc: Storage location preference.
 * rel_path: Relative filename or subpath (e.g. "config.ini" or "saves/slot1.dat").
 * out_path: Buffer receiving the formatted path.
 * max_len: Capacity of out_path buffer.
 *
 * Returns: 0 on success, or negative error code.
 */
int oops_fs_storage_path(oops_storage_location_t loc, const char *rel_path,
                         char *out_path, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_FS_H */
