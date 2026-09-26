/*
 * Target process resolver.
 *
 * Walks the kernel's process list through krw to find a process by pid, name or title
 * id, or the foreground retail game when no target is given. Structure field positions
 * depend on the firmware version and are chosen from `krw_fw_version()`.
 */

#include "oops/inject.h"
#include "oops/freestd.h"
#include "oops/krw.h"
#include "oops/syscall.h"

#define SYS_getpid 20

static const uintptr_t KERNEL_OFFSET_PROC_P_UCRED = 0x40;
static const uintptr_t KERNEL_OFFSET_PROC_P_PID = 0xBC;
static const uintptr_t DEFAULT_OFFSET_PROC_P_COMM = 0x274;

/* The title id ("PPSAxxxxx", "CUSAxxxxx") of a proc, read at its firmware-dependent
 * field and otherwise found by scanning the structure. */
static int get_proc_title_id(uintptr_t proc, char *out_title, size_t out_len) {
    if (proc == 0 || out_title == NULL || out_len < 16) {
        return 0;
    }

    uint32_t fw = krw_fw_version() & 0xffff0000u;
    uintptr_t titleid_off = (fw >= 0x08000000u)   ? 0x470u
                            : (fw >= 0x07000000u) ? 0x49Au
                            : (fw >= 0x06000000u) ? 0x498u
                                                  : 0x470u;

    char tid[16];
    memset(tid, 0, sizeof(tid));
    if (krw_copyout(proc + titleid_off, tid, sizeof(tid)) == 0) {
        /* Check if it starts with CUSA or PPSA */
        if ((tid[0] == 'C' && tid[1] == 'U' && tid[2] == 'S' && tid[3] == 'A') ||
            (tid[0] == 'P' && tid[1] == 'P' && tid[2] == 'S' && tid[3] == 'A')) {
            size_t tlen = 0;
            while (tlen < 9 && tlen < out_len - 1 && tid[tlen] != '\0') {
                out_title[tlen] = tid[tlen];
                tlen++;
            }
            out_title[tlen] = '\0';
            return 1;
        }
    }

    /* Fallback: scan proc structure for PPSAxxxxx or CUSAxxxxx pattern */
    char pbuf[2048];
    memset(pbuf, 0, sizeof(pbuf));
    if (krw_copyout(proc, pbuf, sizeof(pbuf)) == 0) {
        for (uintptr_t off = 0x300; off < sizeof(pbuf) - 16; off++) {
            if ((pbuf[off] == 'P' && pbuf[off + 1] == 'P' && pbuf[off + 2] == 'S' &&
                 pbuf[off + 3] == 'A') ||
                (pbuf[off] == 'C' && pbuf[off + 1] == 'U' && pbuf[off + 2] == 'S' &&
                 pbuf[off + 3] == 'A')) {
                size_t tlen = 0;
                while (tlen < 9 && tlen < out_len - 1 && pbuf[off + tlen] != '\0') {
                    out_title[tlen] = pbuf[off + tlen];
                    tlen++;
                }
                out_title[tlen] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

/* Dynamically find p_comm offset in struct proc from our own process */
static uintptr_t detect_p_comm_offset(uintptr_t my_proc) {
    if (my_proc == 0) {
        return DEFAULT_OFFSET_PROC_P_COMM;
    }

    char buf[2048];
    if (krw_copyout(my_proc, buf, sizeof(buf)) != 0) {
        return DEFAULT_OFFSET_PROC_P_COMM;
    }

    for (uintptr_t off = 0x80; off <= sizeof(buf) - 16; off++) {
        if (obs_strcmp(buf + off, "eboot.bin") == 0 ||
            obs_strcmp(buf + off, "payload.elf") == 0 ||
            obs_strcmp(buf + off, "obscene.elf") == 0 ||
            obs_strcmp(buf + off, "obscene-injector.elf") == 0) {
            return off;
        }
    }
    for (uintptr_t off = 0x80; off <= sizeof(buf) - 16; off++) {
        if (obs_strncmp(buf + off, "SceSpZeroConf", 13) == 0 ||
            obs_strncmp(buf + off, "NPXS", 4) == 0) {
            return off;
        }
    }

    return DEFAULT_OFFSET_PROC_P_COMM;
}

static uintptr_t find_allproc_kaddr(pid_t mypid) {
    uintptr_t kdata = krw_kdata_base();
    if (kdata == 0)
        return krw_allproc_addr();

    uintptr_t start = kdata + 0x2700000;
    uintptr_t end = kdata + 0x2A00000;

    uintptr_t user_allproc = 0;
    int max_user_chain = 0;

    for (uintptr_t addr = start; addr < end; addr += 8) {
        uintptr_t p = krw_read64(addr);
        if ((p >> 40) != 0xffffcd && (p >> 40) != 0xffffff) {
            continue;
        }

        int user_chain_len = 0;
        int has_mypid = 0;
        uintptr_t curr = p;
        for (int i = 0; i < 200; i++) {
            if (curr == 0 || (curr >> 40) != 0xffffcd) {
                break;
            }
            pid_t pid = (pid_t)krw_read32(curr + KERNEL_OFFSET_PROC_P_PID);
            if (pid < 0 || pid > 65535) {
                break;
            }
            if (pid > 54) {
                user_chain_len++;
            }
            if (mypid > 0 && pid == mypid) {
                has_mypid = 1;
            }
            uintptr_t next = krw_read64(curr);
            if (next == 0 || next == curr) {
                break;
            }
            curr = next;
        }

        if (has_mypid) {
            user_allproc = addr;
            break;
        }

        if (user_chain_len > max_user_chain && user_chain_len >= 5) {
            user_allproc = addr;
            max_user_chain = user_chain_len;
        }
    }

    if (user_allproc != 0) {
        krw_set_allproc_addr(user_allproc);
        return user_allproc;
    }

    return krw_allproc_addr();
}

static int is_system_daemon(const char *comm) {
    if (comm == NULL || comm[0] == '\0') {
        return 1; /* Skip unknown/empty names; retail games always have comm
                   * "eboot.bin"
                   */
    }

    /* Any binary ending in .elf or .self is an internal daemon / payload */
    size_t len = obs_strlen(comm);
    if (len >= 4 && obs_strcmp(comm + len - 4, ".elf") == 0) {
        return 1;
    }
    if (len >= 5 && obs_strcmp(comm + len - 5, ".self") == 0) {
        return 1;
    }

    /* Any Sony system daemon starting with Sce, NPXS, or NPXX */
    if (obs_strncmp(comm, "Sce", 3) == 0 || obs_strncmp(comm, "NPXS", 4) == 0 ||
        obs_strncmp(comm, "NPXX", 4) == 0) {
        return 1;
    }

    static const char *system_names[] = {"kernel",
                                         "init",
                                         "pldmgr",
                                         "elfldr",
                                         "klogsrv",
                                         "ftpsrv",
                                         "shsrv",
                                         "kstuff",
                                         "shadowmount",
                                         "orbis_audiod",
                                         "AgcCompositor",
                                         "fs_cleaner",
                                         "webrtc_daemon",
                                         "sh",
                                         "sysctl",
                                         "df",
                                         NULL};

    for (size_t i = 0; system_names[i] != NULL; i++) {
        if (obs_strcmp(comm, system_names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

pid_t target_find_by_name(const char *name) {
    if (name == NULL || !krw_is_ready()) {
        return -1;
    }

    pid_t mypid = (pid_t)sys_call(SYS_getpid, 0, 0, 0, 0, 0, 0);
    uintptr_t allproc = find_allproc_kaddr(mypid);
    if (allproc == 0) {
        return -1;
    }

    uintptr_t myproc = krw_get_proc(mypid);
    uintptr_t comm_offset = detect_p_comm_offset(myproc);

    uintptr_t proc = krw_read64(allproc);
    while (proc != 0) {
        pid_t pid = (pid_t)krw_read32(proc + KERNEL_OFFSET_PROC_P_PID);
        if (pid > 0 && pid != mypid) {
            char comm[32];
            memset(comm, 0, sizeof(comm));
            krw_copyout(proc + comm_offset, comm, sizeof(comm) - 1);

            char tid[16];
            memset(tid, 0, sizeof(tid));
            get_proc_title_id(proc, tid, sizeof(tid));

            if (obs_strcmp(name, comm) == 0 ||
                (tid[0] != '\0' && obs_strcmp(name, tid) == 0)) {
                return pid;
            }
        }

        uintptr_t next = krw_read64(proc);
        if (next == 0 || next == proc) {
            break;
        }
        proc = next;
    }

    return -1;
}

pid_t target_find_foreground_app(void) {
    if (!krw_is_ready()) {
        return -1;
    }

    pid_t mypid = (pid_t)sys_call(SYS_getpid, 0, 0, 0, 0, 0, 0);
    uintptr_t allproc = find_allproc_kaddr(mypid);
    if (allproc == 0) {
        return -1;
    }
    klog_write_hex("resolved allproc=", allproc);

    uintptr_t myproc = krw_get_proc(mypid);
    klog_write_hex("myproc=", myproc);
    uintptr_t comm_offset = detect_p_comm_offset(myproc);
    klog_write_hex("p_comm offset=", comm_offset);

    uint32_t fw = krw_fw_version() & 0xffff0000u;
    uintptr_t name_off = (fw >= 0x12000000u)   ? 0x5E4u
                         : (fw >= 0x10000000u) ? 0x5DCu
                         : (fw >= 0x07000000u) ? 0x5D4u
                         : (fw >= 0x06000000u) ? 0x5C4u
                                               : 0x59Cu;
    uintptr_t path_off = (fw >= 0x12000000u)   ? 0x604u
                         : (fw >= 0x10000000u) ? 0x5FCu
                         : (fw >= 0x07000000u) ? 0x5F4u
                                               : 0x5BCu;

    pid_t game_pid = -1;

    uintptr_t next = 0;
    uintptr_t proc = krw_read64(allproc);
    while (proc != 0) {
        pid_t pid = (pid_t)krw_read32(proc + KERNEL_OFFSET_PROC_P_PID);
        if (pid > 0 && pid != mypid) {
            char pbuf[2048];
            memset(pbuf, 0, sizeof(pbuf));
            krw_copyout(proc, pbuf, sizeof(pbuf));

            char comm[32];
            memset(comm, 0, sizeof(comm));
            krw_copyout(proc + comm_offset, comm, sizeof(comm) - 1);
            if (comm[0] == '\0') {
                krw_copyout(proc + name_off, comm, sizeof(comm) - 1);
            }

            char raw_tid[16];
            memset(raw_tid, 0, sizeof(raw_tid));
            get_proc_title_id(proc, raw_tid, sizeof(raw_tid));

            char app_path[128];
            memset(app_path, 0, sizeof(app_path));
            krw_copyout(proc + path_off, app_path, sizeof(app_path) - 1);

            /* If app_path is empty, scan pbuf for path strings */
            if (app_path[0] == '\0') {
                for (uintptr_t off = 0x500; off < sizeof(pbuf) - 32; off++) {
                    if (obs_strncmp(pbuf + off, "/app0", 5) == 0 ||
                        obs_strncmp(pbuf + off, "/user/app", 9) == 0 ||
                        obs_strncmp(pbuf + off, "/mnt/sandbox", 12) == 0 ||
                        obs_strncmp(pbuf + off, "/system/vsh/app", 15) == 0) {
                        obs_strncpy(app_path, pbuf + off, sizeof(app_path) - 1);
                        break;
                    }
                }
            }

            /* 1. Skip system paths, VSH apps, daemons, and WebKit containers */
            for (uintptr_t off = 0; off < sizeof(pbuf) - 4; off++) {
                if (pbuf[off] == 'N' && pbuf[off + 1] == 'P' && pbuf[off + 2] == 'X' &&
                    (pbuf[off + 3] == 'S' || pbuf[off + 3] == 'X')) {
                    goto next_proc;
                }
            }
            if (obs_strstr(app_path, "/system/") != NULL ||
                obs_strncmp(app_path, "/system", 7) == 0 ||
                obs_strstr(app_path, "NPXS") != NULL ||
                obs_strstr(comm, "NPXS") != NULL ||
                obs_strncmp(raw_tid, "NPXS", 4) == 0 ||
                obs_strncmp(raw_tid, "NPXX", 4) == 0 ||
                obs_strncmp(raw_tid, "Sce", 3) == 0) {
                goto next_proc;
            }

            /* 2. Skip daemons and payloads ending in .self or .elf (retail games are
             * always eboot.bin) */
            if (obs_strstr(app_path, ".self") != NULL ||
                obs_strstr(comm, ".self") != NULL ||
                obs_strstr(app_path, ".elf") != NULL ||
                obs_strstr(comm, ".elf") != NULL) {
                goto next_proc;
            }

            /* 3. Skip known system daemons */
            if (is_system_daemon(comm)) {
                klog_write("skip system proc:");
                klog_write(comm[0] != '\0' ? comm : "(empty)");
                klog_write_num("  pid=", (int64_t)pid);
                goto next_proc;
            }

            klog_write(">>> candidate live proc:");
            klog_write(comm[0] != '\0' ? comm : "(unnamed)");
            klog_write_num("  pid=", (int64_t)pid);
            klog_write("  tid=");
            klog_write(raw_tid[0] != '\0' ? raw_tid : "(none)");
            klog_write("  path=");
            klog_write(app_path[0] != '\0' ? app_path : "(none)");

            /* 4. Match retail game by Title ID (PPSA / CUSA) */
            if ((raw_tid[0] == 'P' && raw_tid[1] == 'P' && raw_tid[2] == 'S' &&
                 raw_tid[3] == 'A') ||
                (raw_tid[0] == 'C' && raw_tid[1] == 'U' && raw_tid[2] == 'S' &&
                 raw_tid[3] == 'A')) {
                klog_write(">>> MATCHED RETAIL GAME BY TITLE ID:");
                klog_write(raw_tid);
                klog_write("  path=");
                klog_write(app_path[0] != '\0' ? app_path : "(unknown)");
                klog_write_num("  matched game pid=", (int64_t)pid);
                return pid;
            }

            /* 5. Match retail game by app path ending in eboot.bin */
            if (obs_strstr(app_path, "eboot.bin") != NULL &&
                (obs_strstr(app_path, "/app0/") != NULL ||
                 obs_strstr(app_path, "/user/app/") != NULL ||
                 obs_strstr(app_path, "/mnt/sandbox/") != NULL)) {
                klog_write(">>> MATCHED RETAIL GAME BY PATH:");
                klog_write(app_path);
                klog_write_num("  matched game pid=", (int64_t)pid);
                return pid;
            }

            /* 6. Candidate userland process */
            uintptr_t ucred = 0;
            uint32_t uid = 0;
            if (krw_copyout(proc + KERNEL_OFFSET_PROC_P_UCRED, &ucred, sizeof(ucred)) ==
                    0 &&
                ucred != 0) {
                krw_copyout(ucred + 0x04, &uid, sizeof(uid));
            }
            uintptr_t prison = 0;
            if (ucred != 0) {
                krw_copyout(ucred + 0x30, &prison, sizeof(prison));
            }

            klog_write(">>> candidate proc:");
            klog_write(comm);
            klog_write_num("  pid=", (int64_t)pid);
            klog_write_num("  uid=", (int64_t)uid);
            klog_write_hex("  kproc=", proc);
            klog_write_hex("  ucred=", ucred);
            klog_write_hex("  prison=", prison);
            if (raw_tid[0] != '\0') {
                klog_write("  proc titleid=");
                klog_write(raw_tid);
            }
            if (app_path[0] != '\0') {
                klog_write("  proc path=");
                klog_write(app_path);
            }

            if (obs_strcmp(comm, "eboot.bin") == 0) {
                if (raw_tid[0] != '\0' || obs_strstr(app_path, "/app0/") != NULL ||
                    obs_strstr(app_path, "/user/app/") != NULL ||
                    obs_strstr(app_path, "/mnt/sandbox/") != NULL) {
                    klog_write_num("  candidate game eboot.bin pid=", (int64_t)pid);
                    if (game_pid < 0) {
                        game_pid = pid;
                    }
                }
            }
        }
    next_proc:
        next = krw_read64(proc);
        if (next == 0 || next == proc) {
            break;
        }
        proc = next;
    }

    if (game_pid > 0) {
        klog_write_num("selected foreground game pid=", (int64_t)game_pid);
        return game_pid;
    }

    klog_write("ERROR: no active retail game found in userland");
    klog_write("ACTION: launch a retail game on the Prospero home screen, then re-run");
    return -1;
}

pid_t target_resolve(const char *target_spec) {
    if (target_spec != NULL && target_spec[0] != '\0') {
        /* Check if target_spec is numeric PID */
        pid_t pid = 0;
        int is_num = 1;
        for (size_t i = 0; target_spec[i] != '\0'; i++) {
            if (target_spec[i] < '0' || target_spec[i] > '9') {
                is_num = 0;
                break;
            }
            pid = (pid_t)(pid * 10 + (target_spec[i] - '0'));
        }

        if (is_num && pid > 0) {
            return pid;
        }

        /* Treat as process name or title ID */
        return target_find_by_name(target_spec);
    }

    return target_find_foreground_app();
}

int target_get_title_id(pid_t pid, char *out_title, size_t out_len) {
    if (pid <= 0 || out_title == NULL || out_len < 16) {
        return 0;
    }
    uintptr_t proc = krw_get_proc(pid);
    if (proc == 0) {
        return 0;
    }
    return get_proc_title_id(proc, out_title, out_len);
}
