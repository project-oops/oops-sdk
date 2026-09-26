/*
 * Save data: mount a title's save slot (libSceSaveData, or a persistent directory
 * where that is unavailable) and load or save whole files in it.
 */
#ifndef OOPS_SAVEDATA_H
#define OOPS_SAVEDATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum oops_savedata_mode {
    OOPS_SAVEDATA_MODE_READ_ONLY = 0x01,
    OOPS_SAVEDATA_MODE_READ_WRITE = 0x02,
    OOPS_SAVEDATA_MODE_CREATE = 0x04
} oops_savedata_mode_t;

/**
 * Initialize the SaveData subsystem.
 * Automatically loads OOPS_SYSMODULE_SAVE_DATA.
 * Returns: 0 on success, or negative error code.
 */
int oops_savedata_init(void);

/**
 * Mount a savedata slot directory.
 *
 * dir_name: Directory name of the save slot (e.g. "SAVE0000" or title-specific).
 * mode: Read-only, Read-write, or Create if non-existent.
 * out_mount_path: Buffer receiving the mounted filesystem path (e.g. "/savedata0"
 *                 or persistent filesystem fallback path).
 * max_path_len: Capacity of out_mount_path buffer.
 *
 * If vendor libSceSaveData is available and authorized, mounts an encrypted container.
 * Otherwise, falls back transparently to a title-scoped persistent directory
 * (/data/savedata/<app_id>/<dir_name> on target, ./savedata/<app_id>/<dir_name> on
 * host).
 *
 * Returns: 0 on success, or negative error code.
 */
int oops_savedata_mount(const char *dir_name, oops_savedata_mode_t mode,
                        char *out_mount_path, size_t max_path_len);

/**
 * Unmount a previously mounted savedata slot.
 *
 * mount_path: The mount path returned by oops_savedata_mount().
 * commit: If true, commits pending changes to encrypted storage.
 *
 * Returns: 0 on success, or negative error code.
 */
int oops_savedata_unmount(const char *mount_path, bool commit);

/**
 * Load a whole file from a savedata slot.
 * Automatically mounts the slot, reads the file via oops_fs_read_all,
 * and unmounts the slot.
 *
 * out_data: Allocated buffer with file contents (must be freed with oops_fs_free_data).
 * out_size: Size in bytes of loaded data.
 * Returns: 0 on success, or negative error code.
 */
int oops_savedata_load_file(const char *dir_name, const char *file_name,
                            void **out_data, size_t *out_size);

/**
 * Save a whole buffer to a file within a savedata slot.
 * Automatically mounts the slot with OOPS_SAVEDATA_MODE_CREATE |
 * OOPS_SAVEDATA_MODE_READ_WRITE, writes the data via oops_fs_write_all, and unmounts
 * the slot with commit=true.
 *
 * Returns: 0 on success, or negative error code.
 */
int oops_savedata_save_file(const char *dir_name, const char *file_name,
                            const void *data, size_t size);

/**
 * Terminate the SaveData subsystem and unload sysmodule.
 */
void oops_savedata_term(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_SAVEDATA_H */
