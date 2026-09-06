#include "oops/savedata.h"
#include "oops/sysmodule.h"
#include "oops/system.h"

__attribute__((weak)) int sceSaveDataInitialize3(void *param);
__attribute__((weak)) int sceSaveDataTerminate(void);
__attribute__((weak)) int sceSaveDataMount2(void *mountParam);
__attribute__((weak)) int sceSaveDataUmount(void *umountParam);

static bool s_savedata_initialized = false;
static bool s_savedata_module_loaded = false;

static void safe_strcpy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < max_len) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

int oops_savedata_init(void) {
    if (s_savedata_initialized) {
        return 0;
    }

    if (oops_sysmodule_load(OOPS_SYSMODULE_SAVE_DATA) == 0) {
        s_savedata_module_loaded = true;
    }

    if (!sceSaveDataInitialize3) {
        return -1;
    }

    uint8_t init_param[64];
    for (size_t i = 0; i < sizeof(init_param); i++) init_param[i] = 0;

    int rc = sceSaveDataInitialize3(init_param);
    if (rc == 0) {
        s_savedata_initialized = true;
    }
    return rc;
}

int oops_savedata_mount(const char *dir_name, oops_savedata_mode_t mode, char *out_mount_path, size_t max_path_len) {
    if (!dir_name || !out_mount_path || max_path_len == 0) return -1;
    out_mount_path[0] = '\0';

    if (!s_savedata_initialized) {
        if (oops_savedata_init() != 0) {
            return -1;
        }
    }

    if (!sceSaveDataMount2) {
        return -1;
    }

    int32_t user_id = oops_user_get_initial_user_id();
    if (user_id < 0) user_id = 0;

    /* SceSaveDataMount2 layout */
    uint8_t mount_param[512];
    for (size_t i = 0; i < sizeof(mount_param); i++) mount_param[i] = 0;

    *(int32_t *)(mount_param + 0) = user_id;
    safe_strcpy((char *)(mount_param + 8), dir_name, 32);
    *(uint32_t *)(mount_param + 48) = (uint32_t)mode;

    /* Buffer for returned mount point */
    char mount_point[64];
    for (size_t i = 0; i < sizeof(mount_point); i++) mount_point[i] = 0;
    *(char **)(mount_param + 56) = mount_point;
    *(uint32_t *)(mount_param + 64) = sizeof(mount_point);

    int rc = sceSaveDataMount2(mount_param);
    if (rc == 0) {
        safe_strcpy(out_mount_path, mount_point, max_path_len);
    }
    return rc;
}

int oops_savedata_unmount(const char *mount_path, bool commit) {
    if (!mount_path) return -1;
    if (!s_savedata_initialized || !sceSaveDataUmount) {
        return -1;
    }

    uint8_t umount_param[128];
    for (size_t i = 0; i < sizeof(umount_param); i++) umount_param[i] = 0;

    safe_strcpy((char *)(umount_param + 0), mount_path, 64);
    *(uint32_t *)(umount_param + 64) = commit ? 1 : 0;

    return sceSaveDataUmount(umount_param);
}

void oops_savedata_term(void) {
    if (s_savedata_initialized && sceSaveDataTerminate) {
        sceSaveDataTerminate();
        s_savedata_initialized = false;
    }
    if (s_savedata_module_loaded) {
        (void)oops_sysmodule_unload(OOPS_SYSMODULE_SAVE_DATA);
        s_savedata_module_loaded = false;
    }
}

