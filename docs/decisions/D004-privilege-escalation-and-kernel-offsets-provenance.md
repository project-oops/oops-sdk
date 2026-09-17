# D004 - Privilege escalation, kernel offsets provenance, and explicit opt-in


**decided** · 2026-09-04

Privilege escalation code was originally located inside `SELFish/runtime/` with an implicit
declaration in `crt0.c`. SELFish's charter forbids target runtime code ("anything that executes on
the console does not belong here, and neither does a convenience layer for writing homebrew").
Escalation capability was brought into `oops-sdk` as an explicit, consumer-invoked API
(`oops_escalate_to_system_authid()`) and removed entirely from `SELFish`.

### Provenance and Firmware Fragility

Kernel memory manipulation requires exact structure layout offsets. In accordance with
CONVENTIONS §1 ("Where a lawful reference exists, cite it. If you cannot explain where a behaviour
came from, that is the signal"):

1. **`p_ucred = 0x40` (Grade: Lawful Reference)**
   In the FreeBSD kernel (sys/sys/proc.h on x86_64), `struct proc` begins with:
   - `0x00`: `p_list` (`LIST_ENTRY(proc)`, 16 bytes)
   - `0x10`: `p_threads` (`TAILQ_HEAD(, thread)`, 16 bytes)
   - `0x20`: `p_slock` (`struct mtx`, 32 bytes)
   - `0x40`: `p_ucred` (`struct ucred *`, pointer at offset 0x40)
   The offset is a stable FreeBSD 9/11/12 ABI fact.

2. **`cr_sceauthid = 0x58` (Grade: Assumed / Unpinned Hypothesis)**
   Standard FreeBSD `struct ucred` (sys/sys/ucred.h) has no Sony authority identifier. The console
   kernel vendor-patches `struct ucred` to embed SCE security credentials.
   Offset `0x58` is an **assumed** provenance grade derived from prior research and loader
   implementations targeting firmware 1.xx–4.xx. It is not an ABI guarantee, carries no vendor
   specification, and is treated as an unpinned hypothesis until hardware measurement confirms or
   refutes it across specific firmware generations.

### Failure Behavior

`oops_escalate_to_system_authid()` never asserts success when primitives or offsets are absent.
If `sceKernelReadProcessMemory`, `sceKernelWriteProcessMemory`, or process credentials cannot be
resolved (e.g. running on host, emulator, or without an active kernel exploit primitive), it
returns `-1` honestly rather than trapping or faking success (CONVENTIONS §3).


### Decoupled sandbox namespace service

The sandbox escape capability is decoupled into a first-party background service
(`sandbox-daemon` in `oops-apps/src/sandbox-daemon/`) and an explicit opt-in client API
(`oops_system_escape_sandbox()` in `oops-sdk`).

#### Original design (trigger-file polling)

The initial implementation followed the pattern from
LightningMods/etaHEN and ArkSama/PS5-Lapy-JB-Daemon: a daemon polls for trigger files
(`/download0/etahen_jailbreak`) and performs kernel-side credential and vnode manipulation
on behalf of requesting processes.

#### Updated design (loopback TCP IPC — zero polling)

On Prospero FW 12.40, native Big App sandboxes do not mount `/download0`, making the
trigger-file mechanism inoperable. Furthermore, polling the filesystem in a timer loop
wastes CPU cycles and causes disk thrashing. The architecture was replaced with an
entirely event-driven loopback TCP service on `127.0.0.1:9069`:

- **Server (sandbox-daemon)**: Creates a TCP listener bound to `127.0.0.1:9069` and enters
  a blocking `sys_call(SYS_accept, ...)` loop. When no clients are connecting, the daemon
  thread is suspended in the kernel socket wait queue—consuming **0% CPU** and performing
  **zero disk I/O**. On each incoming connection:
  1. Reads the 4-byte client PID.
  2. Walks the dynamically resolved `allproc` chain to find the target `kproc`.
  3. Repoints `fd_rdir` (`0x10`), `fd_jdir` (`0x18`), and `fd_cdir` (`0x08`) to `rootvnode`.
  4. Sets root credentials (`cr_uid = 0`, `cr_ruid = 0`, `cr_sceauthid = 0x4801000000000013`,
     and all capabilities `0xFF`).
  5. Borrows `cr_prison` from PID 1 (`mini-syscore`, prison0).
  6. Sends an `int32_t` status code (`0` = success) and closes the socket.
- **Client (oops_system_escape_sandbox)**: Connects to `127.0.0.1:9069` **once on startup**,
  writes its PID (4 bytes), reads the status code, and closes the connection. It does not
  poll.

#### `struct filedesc` Layout & Kernel Offsets

In FreeBSD 11/12 and Prospero (`sys/sys/filedesc.h`), `struct filedesc` layout is:
- `0x00`: `fd_ofiles` (pointer to open file table)
- `0x08`: `fd_cdir` (`struct vnode *`, current working directory vnode)
- `0x10`: `fd_rdir` (`struct vnode *`, chroot root directory vnode)
- `0x18`: `fd_jdir` (`struct vnode *`, jail root directory vnode)

*Correction Note*: An earlier revision had shifted offsets (`0x18` treated as `fd_rdir` and
`0x20` as `fd_jdir`), which caused `fd_rdir` at `0x10` to remain untouched and corrupted
neighboring descriptor metadata. The offsets were aligned to the canonical FreeBSD 11/12 ABI,
and `krw_get_proc_cdir` / `krw_set_proc_cdir` were introduced at `0x08`.

#### Dynamic `rootvnode` Discovery

Standard FreeBSD processes that have never called `chroot(2)` (such as PID 1 `mini-syscore` or
standalone daemons) have `fd_rdir == NULL (0x0)` because path resolution defaults to the kernel
`rootvnode` symbol whenever `fd_rdir == NULL`. However, PID 1 permanently runs at `/`, meaning
its `fd_cdir` (`0x08`) is permanently rooted at `rootvnode`.
On PS5 FW 12.40 hardware, PID 1 has both `fd_rdir == 0xffffdc5d01d89fe0` and
`fd_cdir == 0xffffdc5d01d89fe0` (`fd_jdir == 0x0`). `sandbox-daemon` inspects PID 1 and self to
dynamically resolve the true `rootvnode` without hardcoding kernel pointer addresses.

#### Dynamic allproc discovery

Static heuristic kernel offsets for `allproc` resolution fail on FW 12.40. The daemon
incorporates dynamic proc-chain scanning from `pltauth-patch`: it scans the kernel data
memory range `[kdata+0x2600000, kdata+0x2B00000]` for a valid pointer chain containing
the daemon's own PID, then calls `krw_set_allproc_addr()` to pin the discovered head.
This approach is confirmed working on FW 12.40 via the pltauth-patch payload.

#### Dual-stream telemetry

The daemon uses a dual-stream logging helper that writes formatted diagnostic messages to
both `oops_klog` and direct stdout/stderr (`sys_call(SYS_write, 1, ...)`) for transparent
startup and handshake diagnostics, mirroring the `pltauth-patch` pattern.

#### Directory Enumeration & Title Discovery

Once a process is elevated, directory enumeration across `/user/app`, `/data/homebrew`, and
`/user/appmeta` requires compatibility with Prospero's FreeBSD 12 kernel:
- **Syscall 554 (`SYS_getdirentries`)**: Native FreeBSD 12 directory enumeration syscall,
  taking a 64-bit `off_t *basep` argument and returning `struct dirent` with `reclen` at `+16`,
  `namlen` at `+20`, and `name` at `+24`.
- **Direct-path Candidate Probing**: Where directory scanning syscalls are restricted, direct
  file lookup via `oops_fs_exists()` for `/user/appmeta/<ID>/param.json` and
  `/user/appmeta/<ID>/icon0.png` reliably discovers installed titles without invoking external
  system services.

#### Strict Clean-Room Provenance

The entire stack—client `oops_system_escape_sandbox()`, resident `sandbox-daemon`, kernel `krw`
primitives, and SeaShell UI—is 100% first-party OOPS code. No third-party daemons (etaHEN,
Lapy-JB-Daemon), cheats, or external shell servers are employed. Normal apps remain sandboxed by
default; system-level utilities invoke `oops_system_escape_sandbox()` explicitly to access `/user`
and `/data`.

