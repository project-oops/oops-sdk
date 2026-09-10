#ifndef OOPS_PKG_H
#define OOPS_PKG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oops_pkg_progress_info {
  uint32_t
      state; /* 0 = Not installing, 1 = In progress, 2 = Completed, 3 = Error */
  uint32_t progress_pct;      /* 0 - 100 */
  uint64_t progress_bytes;    /* Bytes installed so far */
  uint64_t total_bytes;       /* Total package payload size in bytes */
  uint32_t remaining_seconds; /* Estimated rest seconds */
} oops_pkg_progress_info_t;

/**
 * Initialize the application and package installer utility service
 * (libSceAppInstUtil). Returns 0 on success, or a negative error code.
 */
int oops_pkg_init(void);

/**
 * Terminate the package installer utility service.
 */
void oops_pkg_term(void);

/**
 * Initiate asynchronous background installation of a PKG file from a local
 * filesystem path. Returns 0 on success, or a negative error code.
 */
int oops_pkg_install(const char *pkg_path);

/**
 * Check if an application is installed given its Title ID (e.g. "CUSA00001").
 * Sets *out_exists to true if installed, false otherwise.
 * Returns 0 on success, or a negative error code.
 */
int oops_app_exists(const char *title_id, bool *out_exists);

/**
 * Uninstall an application given its Title ID.
 * Returns 0 on success, or a negative error code.
 */
int oops_app_uninstall(const char *title_id);

/**
 * Query current installation progress percentage (0-100) for a Content ID.
 * Returns 0 on success, or a negative error code.
 */
int oops_pkg_get_progress(const char *content_id, uint32_t *out_progress_pct);

/**
 * Query detailed installation progress for a Content ID.
 * Returns 0 on success, or a negative error code.
 */
int oops_pkg_get_progress_info(const char *content_id,
                               oops_pkg_progress_info_t *out_info);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_PKG_H */
