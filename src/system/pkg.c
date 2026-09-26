#include "oops/pkg.h"

/* Package install, uninstall and progress (oops/pkg.h) over `libSceAppInstUtil`. The
 * entry points are weak, so a build without them fails the call instead of the link. */

/* Platform weak symbols for libSceAppInstUtil */
__attribute__((weak)) int sceAppInstUtilInitialize(void);
__attribute__((weak)) int sceAppInstUtilTerminate(void);
__attribute__((weak)) int sceAppInstUtilAppInstallPkg(const char *pkgPath,
                                                      void *reserved);
__attribute__((weak)) int sceAppInstUtilAppUnInstall(const char *titleId);
__attribute__((weak)) int sceAppInstUtilAppExists(const char *titleId, int32_t *exists);
__attribute__((weak)) int sceAppInstUtilGetInstallProgress(const char *contentId,
                                                           uint32_t *progress);
__attribute__((weak)) int
sceAppInstUtilGetInstallProgressInfo(const char *contentId, uint32_t *state,
                                     uint32_t *progress, uint32_t *progressSize,
                                     uint32_t *totalSize, uint32_t *restSec);

static int s_pkg_initialized = 0;

int oops_pkg_init(void) {
    if (s_pkg_initialized)
        return 0;
    if (sceAppInstUtilInitialize) {
        int rc = sceAppInstUtilInitialize();
        if (rc == 0)
            s_pkg_initialized = 1;
        return rc;
    }
    return -1;
}

void oops_pkg_term(void) {
    if (s_pkg_initialized) {
        if (sceAppInstUtilTerminate) {
            sceAppInstUtilTerminate();
        }
        s_pkg_initialized = 0;
    }
}

int oops_pkg_install(const char *pkg_path) {
    if (!pkg_path || pkg_path[0] == '\0')
        return -1;
    if (!s_pkg_initialized) {
        if (oops_pkg_init() != 0 && !sceAppInstUtilAppInstallPkg)
            return -1;
    }
    if (!sceAppInstUtilAppInstallPkg)
        return -1;
    return sceAppInstUtilAppInstallPkg(pkg_path, NULL);
}

int oops_app_exists(const char *title_id, bool *out_exists) {
    if (!title_id || !out_exists)
        return -1;
    *out_exists = false;
    if (!s_pkg_initialized) {
        if (oops_pkg_init() != 0 && !sceAppInstUtilAppExists)
            return -1;
    }
    if (!sceAppInstUtilAppExists)
        return -1;

    int32_t exists = 0;
    int rc = sceAppInstUtilAppExists(title_id, &exists);
    if (rc == 0) {
        *out_exists = (exists != 0);
    }
    return rc;
}

int oops_app_uninstall(const char *title_id) {
    if (!title_id || title_id[0] == '\0')
        return -1;
    if (!s_pkg_initialized) {
        if (oops_pkg_init() != 0 && !sceAppInstUtilAppUnInstall)
            return -1;
    }
    if (!sceAppInstUtilAppUnInstall)
        return -1;
    return sceAppInstUtilAppUnInstall(title_id);
}

int oops_pkg_get_progress(const char *content_id, uint32_t *out_progress_pct) {
    if (!content_id || !out_progress_pct)
        return -1;
    *out_progress_pct = 0;
    if (!s_pkg_initialized) {
        if (oops_pkg_init() != 0 && !sceAppInstUtilGetInstallProgress)
            return -1;
    }
    if (!sceAppInstUtilGetInstallProgress)
        return -1;
    return sceAppInstUtilGetInstallProgress(content_id, out_progress_pct);
}

int oops_pkg_get_progress_info(const char *content_id,
                               oops_pkg_progress_info_t *out_info) {
    if (!content_id || !out_info)
        return -1;
    for (size_t i = 0; i < sizeof(*out_info); i++)
        ((unsigned char *)out_info)[i] = 0;

    if (!s_pkg_initialized) {
        if (oops_pkg_init() != 0 && !sceAppInstUtilGetInstallProgressInfo)
            return -1;
    }
    if (!sceAppInstUtilGetInstallProgressInfo)
        return -1;

    uint32_t state = 0;
    uint32_t prog = 0;
    uint32_t prog_size = 0;
    uint32_t tot_size = 0;
    uint32_t rest_sec = 0;

    int rc = sceAppInstUtilGetInstallProgressInfo(content_id, &state, &prog, &prog_size,
                                                  &tot_size, &rest_sec);
    if (rc == 0) {
        out_info->state = state;
        out_info->progress_pct = prog;
        out_info->progress_bytes = (uint64_t)prog_size;
        out_info->total_bytes = (uint64_t)tot_size;
        out_info->remaining_seconds = rest_sec;
    }
    return rc;
}
