/*
 * ZIP archive extraction, from a file or from memory, onto the filesystem.
 */
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
 * Filter callback for oops_zip_extract_filter and oops_zip_extract_mem_filter.
 * Return 1 to extract the entry, or 0 to skip it.
 */
typedef int (*oops_zip_filter_fn)(const char *filename, void *userdata);

/**
 * Progress callback for oops_zip_extract_filter_progress and
 * oops_zip_extract_mem_filter_progress.
 * Called with current entry index (0 to total) and total entry count.
 */
typedef void (*oops_zip_progress_fn)(uint32_t current, uint32_t total,
                                     void *userdata);

/**
 * Extract a ZIP archive from disk into `dest_dir`.
 * Subdirectories are created automatically as needed.
 * Supports STORED (method 0) and DEFLATE (method 8) compression.
 *
 * Returns OOPS_ZIP_OK (0) on success, or a negative OOPS_ZIP_ERR_* code.
 */
int oops_zip_extract(const char *zip_path, const char *dest_dir);

/**
 * Extract matching entries from a ZIP archive on disk into `dest_dir`.
 */
int oops_zip_extract_filter(const char *zip_path, const char *dest_dir,
                            oops_zip_filter_fn filter, void *userdata);

/**
 * Extract matching entries from a ZIP archive on disk with progress updates.
 */
int oops_zip_extract_filter_progress(const char *zip_path, const char *dest_dir,
                                     oops_zip_filter_fn filter,
                                     oops_zip_progress_fn progress,
                                     void *userdata);

/**
 * Extract a ZIP archive held in memory into `dest_dir`.
 * Returns OOPS_ZIP_OK (0) on success, or a negative OOPS_ZIP_ERR_* code.
 */
int oops_zip_extract_mem(const void *zip_data, size_t zip_size,
                         const char *dest_dir);

/**
 * Extract matching entries from a ZIP archive held in memory into `dest_dir`.
 */
int oops_zip_extract_mem_filter(const void *zip_data, size_t zip_size,
                                const char *dest_dir,
                                oops_zip_filter_fn filter, void *userdata);

/**
 * Extract matching entries from memory with progress updates.
 */
int oops_zip_extract_mem_filter_progress(const void *zip_data, size_t zip_size,
                                         const char *dest_dir,
                                         oops_zip_filter_fn filter,
                                         oops_zip_progress_fn progress,
                                         void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_ZIP_H */
