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

