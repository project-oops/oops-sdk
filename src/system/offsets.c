#include "oops/offsets.h"

#define MAX_FW_ENTRIES 32
#define MAX_FILE_SIZE  16384

static oops_common_offsets_t s_common;
static oops_fw_offsets_t s_fws[MAX_FW_ENTRIES];
static size_t s_fw_count = 0;
static bool s_loaded = false;

/* Platform file I/O weak symbols */
__attribute__((weak)) int sceKernelOpen(const char *path, int flags, int mode);
__attribute__((weak)) int sceKernelRead(int fd, void *buf, size_t nbytes);
__attribute__((weak)) int sceKernelClose(int fd);

/* POSIX libc weak symbols (for host testing or libc environments) */
__attribute__((weak)) int open(const char *pathname, int flags, ...);
__attribute__((weak)) long read(int fd, void *buf, size_t count);
__attribute__((weak)) int close(int fd);

/* Helper string utilities (freestanding) */

static bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *skip_whitespace(const char *p) {
    while (*p && is_whitespace(*p)) p++;
    return p;
}

static bool str_eq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, size_t max_len) {
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

static uint64_t parse_u64(const char *str) {
    str = skip_whitespace(str);
    uint64_t val = 0;

    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
        while (*str) {
            char c = *str;
            if (c >= '0' && c <= '9') {
                val = (val << 4) | (uint64_t)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                val = (val << 4) | (uint64_t)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                val = (val << 4) | (uint64_t)(c - 'A' + 10);
            } else if (c == '_') {
                /* Ignore underscores like in 0xFFFFFFFF_80D40000 */
            } else {
                break;
            }
            str++;
        }
    } else {
        while (*str >= '0' && *str <= '9') {
            val = (val * 10) + (uint64_t)(*str - '0');
            str++;
        }
    }
    return val;
}

static uint32_t parse_fw_version_str_to_raw(const char *str) {
    /* Converts e.g. "04.03" or "4.03" or "10.01" to BCD 0x04030000 / 0x10010000 */
    uint32_t major = 0;
    uint32_t minor = 0;
    const char *p = str;

    while (*p >= '0' && *p <= '9') {
        major = (major * 10) + (uint32_t)(*p - '0');
        p++;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            minor = (minor * 10) + (uint32_t)(*p - '0');
            p++;
        }
    }

    uint8_t maj_bcd = (uint8_t)(((major / 10) << 4) | (major % 10));
    uint8_t min_bcd = (uint8_t)(((minor / 10) << 4) | (minor % 10));

    return ((uint32_t)maj_bcd << 24) | ((uint32_t)min_bcd << 16);
}

void oops_offsets_reset(void) {
    for (size_t i = 0; i < sizeof(s_common); i++) {
        ((uint8_t *)&s_common)[i] = 0;
    }
    for (size_t i = 0; i < sizeof(s_fws); i++) {
        ((uint8_t *)s_fws)[i] = 0;
    }
    s_fw_count = 0;
    s_loaded = false;
}

int oops_offsets_load_string(const char *toml_str) {
    if (!toml_str) return -1;

    oops_offsets_reset();

    enum {
        SEC_NONE,
        SEC_COMMON,
        SEC_FW
    } cur_sec = SEC_NONE;

    oops_fw_offsets_t *cur_fw = NULL;

    const char *line = toml_str;
    while (*line) {
        /* Find line end */
        const char *line_end = line;
        while (*line_end && *line_end != '\n') {
            line_end++;
        }

        /* Extract and trim line */
        const char *p = skip_whitespace(line);
        if (*p != '\0' && *p != '#' && p < line_end) {
            /* Check if section header [name] */
            if (*p == '[') {
                p++;
                const char *sec_start = p;
                while (p < line_end && *p != ']') p++;
                if (*p == ']') {
                    char sec_name[32];
                    size_t len = (size_t)(p - sec_start);
                    if (len >= sizeof(sec_name)) len = sizeof(sec_name) - 1;
                    for (size_t k = 0; k < len; k++) sec_name[k] = sec_start[k];
                    sec_name[len] = '\0';

                    if (str_eq(sec_name, "common")) {
                        cur_sec = SEC_COMMON;
                        cur_fw = NULL;
                    } else {
                        cur_sec = SEC_FW;
                        if (s_fw_count < MAX_FW_ENTRIES) {
                            cur_fw = &s_fws[s_fw_count++];
                            str_copy(cur_fw->fw_version_str, sec_name, sizeof(cur_fw->fw_version_str));
                            cur_fw->fw_version_raw = parse_fw_version_str_to_raw(sec_name);
                        } else {
                            cur_fw = NULL; /* Exceeded table */
                        }
                    }
                }
            } else {
                /* Key = Value pair */
                const char *eq = p;
                while (eq < line_end && *eq != '=') eq++;
                if (*eq == '=') {
                    char key[32];
                    const char *k_end = eq - 1;
                    while (k_end > p && is_whitespace(*k_end)) k_end--;
                    size_t k_len = (size_t)(k_end - p + 1);
                    if (k_len >= sizeof(key)) k_len = sizeof(key) - 1;
                    for (size_t k = 0; k < k_len; k++) key[k] = p[k];
                    key[k_len] = '\0';

                    const char *val_str = skip_whitespace(eq + 1);
                    uint64_t val = parse_u64(val_str);

                    if (cur_sec == SEC_COMMON) {
                        if (str_eq(key, "p_ucred")) s_common.p_ucred = (uint32_t)val;
                        else if (str_eq(key, "p_fd")) s_common.p_fd = (uint32_t)val;
                        else if (str_eq(key, "p_pid")) s_common.p_pid = (uint32_t)val;
                        else if (str_eq(key, "p_titleid")) s_common.p_titleid = (uint32_t)val;
                        else if (str_eq(key, "p_comm")) s_common.p_comm = (uint32_t)val;
                        else if (str_eq(key, "fd_rdir")) s_common.fd_rdir = (uint32_t)val;
                        else if (str_eq(key, "fd_jdir")) s_common.fd_jdir = (uint32_t)val;
                        else if (str_eq(key, "cr_uid")) s_common.cr_uid = (uint32_t)val;
                        else if (str_eq(key, "cr_ruid")) s_common.cr_ruid = (uint32_t)val;
                        else if (str_eq(key, "cr_svuid")) s_common.cr_svuid = (uint32_t)val;
                        else if (str_eq(key, "cr_ngroups")) s_common.cr_ngroups = (uint32_t)val;
                        else if (str_eq(key, "cr_rgid")) s_common.cr_rgid = (uint32_t)val;
                        else if (str_eq(key, "cr_svgid")) s_common.cr_svgid = (uint32_t)val;
                        else if (str_eq(key, "cr_prison")) s_common.cr_prison = (uint32_t)val;
                        else if (str_eq(key, "cr_sceauthid")) s_common.cr_sceauthid = (uint32_t)val;
                        else if (str_eq(key, "cr_scecaps")) s_common.cr_scecaps = (uint32_t)val;
                        else if (str_eq(key, "cr_sceattrs")) s_common.cr_sceattrs = (uint32_t)val;
                        else if (str_eq(key, "cr_sceattr0")) s_common.cr_sceattr0 = (uint32_t)val;
                        else if (str_eq(key, "pr_ref")) s_common.pr_ref = (uint32_t)val;
                    } else if (cur_sec == SEC_FW && cur_fw) {
                        if (str_eq(key, "kdata_base")) cur_fw->kdata_base = val;
                        else if (str_eq(key, "allproc")) cur_fw->allproc = val;
                        else if (str_eq(key, "rootvnode")) cur_fw->rootvnode = val;
                        else if (str_eq(key, "security_flags")) cur_fw->security_flags = val;
                        else if (str_eq(key, "qa_flags")) cur_fw->qa_flags = val;
                        else if (str_eq(key, "utoken_flags")) cur_fw->utoken_flags = val;
                        else if (str_eq(key, "sysents")) cur_fw->sysents = val;
                        else if (str_eq(key, "sysentvec")) cur_fw->sysentvec = val;
                    }
                }
            }
        }

        if (*line_end == '\n') {
            line = line_end + 1;
        } else {
            break;
        }
    }

    s_loaded = true;
    return 0;
}

int oops_offsets_load_file(const char *path) {
    if (!path) return -1;

    int fd = -1;
    if (sceKernelOpen) {
        fd = sceKernelOpen(path, 0 /* O_RDONLY */, 0);
    } else if (open) {
        fd = open(path, 0 /* O_RDONLY */);
    }

    if (fd < 0) return -1;

    char buf[MAX_FILE_SIZE];
    long bytes_read = 0;

    if (sceKernelRead) {
        bytes_read = sceKernelRead(fd, buf, sizeof(buf) - 1);
    } else if (read) {
        bytes_read = read(fd, buf, sizeof(buf) - 1);
    }

    if (sceKernelClose) {
        sceKernelClose(fd);
    } else if (close) {
        close(fd);
    }

    if (bytes_read <= 0) return -1;

    buf[(size_t)bytes_read] = '\0';
    return oops_offsets_load_string(buf);
}

int oops_offsets_load_default(void) {
    static const char *search_paths[] = {
        "/app0/offsets.toml",
        "/data/offsets.toml",
        "./offsets.toml",
        "data/offsets.toml",
        "../data/offsets.toml"
    };

    for (size_t i = 0; i < sizeof(search_paths) / sizeof(search_paths[0]); i++) {
        if (oops_offsets_load_file(search_paths[i]) == 0) {
            return 0;
        }
    }
    return -1;
}

const oops_common_offsets_t *oops_offsets_get_common(void) {
    if (!s_loaded) {
        (void)oops_offsets_load_default();
    }
    return &s_common;
}

const oops_fw_offsets_t *oops_offsets_get_fw(uint32_t fw_raw) {
    if (!s_loaded) {
        (void)oops_offsets_load_default();
    }
    /* Match on major.minor (upper 16 bits) */
    uint32_t target_group = fw_raw & 0xFFFF0000;
    for (size_t i = 0; i < s_fw_count; i++) {
        if ((s_fws[i].fw_version_raw & 0xFFFF0000) == target_group) {
            return &s_fws[i];
        }
    }
    return NULL;
}

const oops_fw_offsets_t *oops_offsets_get_fw_str(const char *fw_str) {
    if (!fw_str) return NULL;
    if (!s_loaded) {
        (void)oops_offsets_load_default();
    }
    for (size_t i = 0; i < s_fw_count; i++) {
        if (str_eq(s_fws[i].fw_version_str, fw_str)) {
            return &s_fws[i];
        }
    }
    /* Fallback: parse string to raw and match group */
    uint32_t raw = parse_fw_version_str_to_raw(fw_str);
    return oops_offsets_get_fw(raw);
}
