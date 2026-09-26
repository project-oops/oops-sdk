#ifndef OOPS_ZIP_H
#define OOPS_ZIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result status codes */
#define OOPS_ZIP_OK 0
#define OOPS_ZIP_ERR_PARAM (-1)
#define OOPS_ZIP_ERR_NOT_FOUND (-2)
#define OOPS_ZIP_ERR_READ (-3)
#define OOPS_ZIP_ERR_BAD_HEADER (-4)
#define OOPS_ZIP_ERR_UNSUPPORTED (-5)
#define OOPS_ZIP_ERR_DECOMPRESS (-6)
#define OOPS_ZIP_ERR_WRITE (-7)
#define OOPS_ZIP_ERR_NOMEM (-8)

/**
 * Extract a ZIP archive from disk into `dest_dir`.
 * Subdirectories are created automatically as needed.
 * Supports STORED (method 0) and DEFLATE (method 8) compression.
 *
 * Returns OOPS_ZIP_OK (0) on success, or a negative OOPS_ZIP_ERR_* code.
 */
int oops_zip_extract(const char *zip_path, const char *dest_dir);

/**
 * Extract a ZIP archive held in memory into `dest_dir`.
 * Returns OOPS_ZIP_OK (0) on success, or a negative OOPS_ZIP_ERR_* code.
 */
int oops_zip_extract_mem(const void *zip_data, size_t zip_size, const char *dest_dir);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_ZIP_H */
