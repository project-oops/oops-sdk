#ifndef OOPS_OFFSETS_H
#define OOPS_OFFSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Common kernel structure field offsets (firmware-invariant).
 * Sourced from FreeBSD kernel headers (proc.h, ucred.h, filedesc.h).
 */
typedef struct oops_common_offsets {
    /* Process (struct proc) field offsets */
    uint32_t p_ucred;
    uint32_t p_fd;
    uint32_t p_pid;
    uint32_t p_titleid;
    uint32_t p_comm;

    /* File descriptor (struct filedesc) field offsets */
    uint32_t fd_rdir;
    uint32_t fd_jdir;

    /* Credentials (struct ucred) field offsets */
    uint32_t cr_uid;
    uint32_t cr_ruid;
    uint32_t cr_svuid;
    uint32_t cr_ngroups;
    uint32_t cr_rgid;
    uint32_t cr_svgid;
    uint32_t cr_prison;
    uint32_t cr_sceauthid;
    uint32_t cr_scecaps;
    uint32_t cr_sceattrs;
    uint32_t cr_sceattr0;

    /* Prison (struct prison) field offsets */
    uint32_t pr_ref;
} oops_common_offsets_t;

/**
 * Per-firmware kernel symbol and data offsets.
 */
typedef struct oops_fw_offsets {
    uint32_t fw_version_raw; /* e.g. 0x04030000 or 0x10010000 */
    char fw_version_str[16]; /* e.g. "04.03" or "10.01" */
    uint64_t kdata_base;     /* Kernel .data section base VMA */
    uint64_t allproc;        /* kdata-relative or absolute allproc list head */
    uint64_t rootvnode;      /* kdata-relative or absolute rootvnode pointer */
    uint64_t security_flags; /* kdata-relative security flags offset */
    uint64_t qa_flags;       /* kdata-relative QA flags offset */
    uint64_t utoken_flags;   /* kdata-relative utoken flags offset */
    uint64_t sysents;        /* kdata-relative sysent table offset */
    uint64_t sysentvec;      /* kdata-relative sysentvec offset */
} oops_fw_offsets_t;

/**
 * Load and parse firmware offsets from a TOML formatted string in memory.
 * Returns 0 on success, or negative error code on parse error / bad arguments.
 */
int oops_offsets_load_string(const char *toml_str);

/**
 * Load and parse firmware offsets from a TOML file path.
 * Returns 0 on success, or negative error code on file I/O or parse error.
 */
int oops_offsets_load_file(const char *path);

/**
 * Attempts to load offsets from standard search locations:
 *   1. /app0/offsets.toml
 *   2. /data/offsets.toml
 *   3. ./offsets.toml
 *   4. data/offsets.toml
 *
 * Returns 0 on success, or -1 if no configuration file could be loaded.
 */
int oops_offsets_load_default(void);

/**
 * Returns a pointer to the active common offsets table.
 * If not yet loaded, automatically calls oops_offsets_load_default().
 */
const oops_common_offsets_t *oops_offsets_get_common(void);

/**
 * Returns a pointer to the firmware offsets for the specified firmware version
 * (raw BCD/hex), or NULL if unsupported.
 */
const oops_fw_offsets_t *oops_offsets_get_fw(uint32_t fw_raw);

/**
 * Returns a pointer to the firmware offsets for the specified firmware version
 * string (e.g. "04.03"), or NULL if unsupported.
 */
const oops_fw_offsets_t *oops_offsets_get_fw_str(const char *fw_str);

/**
 * Reset all loaded offsets (clears tables; useful for test suites or reloads).
 */
void oops_offsets_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_OFFSETS_H */
