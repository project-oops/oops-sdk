#ifndef OOPS_SAVEDATA_H
#define OOPS_SAVEDATA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum oops_savedata_mode {
    OOPS_SAVEDATA_MODE_READ_ONLY  = 0x01,
    OOPS_SAVEDATA_MODE_READ_WRITE = 0x02,
    OOPS_SAVEDATA_MODE_CREATE     = 0x04
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
 * out_mount_path: Buffer receiving the mounted filesystem path (e.g. "/savedata0").
 * max_path_len: Capacity of out_mount_path buffer.
 *
 * Returns: 0 on success, or negative error code.
 */
int oops_savedata_mount(const char *dir_name, oops_savedata_mode_t mode, char *out_mount_path, size_t max_path_len);

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
 * Terminate the SaveData subsystem and unload sysmodule.
 */
void oops_savedata_term(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_SAVEDATA_H */

