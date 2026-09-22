#include "oops/savedata.h"
#include "oops/freestd.h"
#include "oops/fs.h"
#include "oops/sysmodule.h"
#include "oops/system.h"

typedef int (*sceSaveDataInitialize3_t)(void *param);
typedef int (*sceSaveDataTerminate_t)(void);
typedef int (*sceSaveDataMount2_t)(void *mountParam);
typedef int (*sceSaveDataUmount_t)(void *umountParam);

static sceSaveDataInitialize3_t s_pfn_sceSaveDataInitialize3 = NULL;
static sceSaveDataTerminate_t s_pfn_sceSaveDataTerminate = NULL;
static sceSaveDataMount2_t s_pfn_sceSaveDataMount2 = NULL;
static sceSaveDataUmount_t s_pfn_sceSaveDataUmount = NULL;

#ifndef OOPS_HOST_BUILD
__attribute__((weak)) int sceKernelDlsym(int handle, const char *symbol, void **out);
#endif

static bool s_savedata_initialized = false;
static bool s_savedata_module_loaded = false;

static void safe_strcpy(char *dst, const char *src, size_t max_len) {
  if (!dst || max_len == 0)
    return;
  size_t i = 0;
  if (src) {
    while (src[i] && i + 1 < max_len) {
      dst[i] = src[i];
      i++;
    }
  }
  dst[i] = '\0';
}

static int mkdir_p(const char *path) {
  if (!path || path[0] == '\0')
    return -1;

  char tmp[256];
  size_t len = obs_strlen(path);
  if (len >= sizeof(tmp))
    return -1;

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

int oops_savedata_init(void) {
  if (s_savedata_initialized) {
    return 0;
  }

  if (oops_sysmodule_load(OOPS_SYSMODULE_SAVE_DATA) == 0) {
    s_savedata_module_loaded = true;
#ifndef OOPS_HOST_BUILD
    if (sceKernelDlsym) {
      void *p = NULL;
      if (sceKernelDlsym(0x2001, "sceSaveDataInitialize3", &p) == 0 && p)
        s_pfn_sceSaveDataInitialize3 = (sceSaveDataInitialize3_t)p;
      if (sceKernelDlsym(0x2001, "sceSaveDataTerminate", &p) == 0 && p)
        s_pfn_sceSaveDataTerminate = (sceSaveDataTerminate_t)p;
      if (sceKernelDlsym(0x2001, "sceSaveDataMount2", &p) == 0 && p)
        s_pfn_sceSaveDataMount2 = (sceSaveDataMount2_t)p;
      if (sceKernelDlsym(0x2001, "sceSaveDataUmount", &p) == 0 && p)
        s_pfn_sceSaveDataUmount = (sceSaveDataUmount_t)p;
    }
#endif
  }

  if (!s_pfn_sceSaveDataInitialize3) {
    return -1;
  }

  uint8_t init_param[64];
  for (size_t i = 0; i < sizeof(init_param); i++)
    init_param[i] = 0;

  int rc = s_pfn_sceSaveDataInitialize3(init_param);
  if (rc == 0) {
    s_savedata_initialized = true;
  }
  return rc;
}

int oops_savedata_mount(const char *dir_name, oops_savedata_mode_t mode,
                        char *out_mount_path, size_t max_path_len) {
  if (!dir_name || !out_mount_path || max_path_len == 0)
    return -1;
  out_mount_path[0] = '\0';

  /* 1. Try vendor libSceSaveData if available */
  if (!s_savedata_initialized) {
    (void)oops_savedata_init();
  }

  if (s_savedata_initialized && s_pfn_sceSaveDataMount2) {
    int32_t user_id = oops_user_get_initial_user_id();
    if (user_id < 0)
      user_id = 0;

    /* SceSaveDataMount2 layout */
    uint8_t mount_param[512];
    for (size_t i = 0; i < sizeof(mount_param); i++)
      mount_param[i] = 0;

    *(int32_t *)(mount_param + 0) = user_id;
    safe_strcpy((char *)(mount_param + 8), dir_name, 32);
    *(uint32_t *)(mount_param + 48) = (uint32_t)mode;

    /* Buffer for returned mount point */
    char mount_point[64];
    for (size_t i = 0; i < sizeof(mount_point); i++)
      mount_point[i] = 0;
    *(char **)(mount_param + 56) = mount_point;
    *(uint32_t *)(mount_param + 64) = sizeof(mount_point);

    int rc = s_pfn_sceSaveDataMount2(mount_param);
    if (rc == 0) {
      safe_strcpy(out_mount_path, mount_point, max_path_len);
      return 0;
    }
  }

  /* 2. Fallback: transparent title-scoped persistent directory */
  const char *app_id = oops_log_get_app_id();
  if (!app_id || app_id[0] == '\0') {
    app_id = "default";
  }

  char slot_path[256];
#ifndef OOPS_HOST_BUILD
  if (oops_fs_exists("/data")) {
    oops_snprintf(slot_path, sizeof(slot_path), "/data/savedata/%s/%s", app_id, dir_name);
  } else {
    oops_snprintf(slot_path, sizeof(slot_path), "savedata/%s/%s", app_id, dir_name);
  }
#else
  oops_snprintf(slot_path, sizeof(slot_path), "savedata/%s/%s", app_id, dir_name);
#endif

  if (obs_strlen(slot_path) >= max_path_len) {
    return -1;
  }

  if (mode & OOPS_SAVEDATA_MODE_CREATE) {
    if (mkdir_p(slot_path) != 0) {
      return -1;
    }
  } else {
    /* If CREATE was not specified, verify that the directory already exists */
    if (!oops_fs_exists(slot_path)) {
      return -1;
    }
  }

  safe_strcpy(out_mount_path, slot_path, max_path_len);
  return 0;
}

int oops_savedata_unmount(const char *mount_path, bool commit) {
  if (!mount_path || mount_path[0] == '\0')
    return -1;

  /* Check if mounted via vendor SCE container (e.g. /savedata0) */
  if (obs_strncmp(mount_path, "/savedata", 9) == 0 &&
      (mount_path[9] == '\0' || (mount_path[9] >= '0' && mount_path[9] <= '9'))) {
    if (!s_savedata_initialized || !s_pfn_sceSaveDataUmount) {
      return -1;
    }

    uint8_t umount_param[128];
    for (size_t i = 0; i < sizeof(umount_param); i++)
      umount_param[i] = 0;

    safe_strcpy((char *)(umount_param + 0), mount_path, 64);
    *(uint32_t *)(umount_param + 64) = commit ? 1 : 0;

    return s_pfn_sceSaveDataUmount(umount_param);
  }

  /* Persistent filesystem fallback: commits are handled directly on write */
  (void)commit;
  return 0;
}

int oops_savedata_load_file(const char *dir_name, const char *file_name,
                            void **out_data, size_t *out_size) {
  if (!dir_name || !file_name || !out_data || !out_size)
    return -1;

  char mount[256];
  if (oops_savedata_mount(dir_name, OOPS_SAVEDATA_MODE_READ_ONLY, mount, sizeof(mount)) != 0)
    return -1;

  char full_path[384];
  oops_snprintf(full_path, sizeof(full_path), "%s/%s", mount, file_name);

  int rc = oops_fs_read_all(full_path, out_data, out_size);
  (void)oops_savedata_unmount(mount, false);
  return rc;
}

int oops_savedata_save_file(const char *dir_name, const char *file_name,
                            const void *data, size_t size) {
  if (!dir_name || !file_name || !data)
    return -1;

  char mount[256];
  if (oops_savedata_mount(dir_name, (oops_savedata_mode_t)(OOPS_SAVEDATA_MODE_CREATE | OOPS_SAVEDATA_MODE_READ_WRITE),
                          mount, sizeof(mount)) != 0)
    return -1;

  char full_path[384];
  oops_snprintf(full_path, sizeof(full_path), "%s/%s", mount, file_name);

  int rc = oops_fs_write_all(full_path, data, size);
  (void)oops_savedata_unmount(mount, true);
  return rc;
}

void oops_savedata_term(void) {
  if (s_savedata_initialized && s_pfn_sceSaveDataTerminate) {
    s_pfn_sceSaveDataTerminate();
    s_savedata_initialized = false;
  }
  if (s_savedata_module_loaded) {
    (void)oops_sysmodule_unload(OOPS_SYSMODULE_SAVE_DATA);
    s_savedata_module_loaded = false;
  }
  s_pfn_sceSaveDataInitialize3 = NULL;
  s_pfn_sceSaveDataTerminate = NULL;
  s_pfn_sceSaveDataMount2 = NULL;
  s_pfn_sceSaveDataUmount = NULL;
}
