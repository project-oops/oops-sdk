# D006 - Dynamic code generation and JIT memory interface

**Status:** decided
**Date:** 2026-09-14

### Context

Recompilers, dynamic translators, and runtime compiler engines (such as shader compilers, Java VMs, and emulator translation layers) require the ability to emit machine instructions into memory and subsequently execute them. Under PlayStation OS (Prospero / Orbis), the kernel enforces strict W^X (Write XOR Execute) memory policies by default.

### Architectural Solution: Dual Strategy

`oops-sdk` provides an explicit, clean-room JIT subsystem (`oops/jit.h` and `src/system/jit.c`) supporting a progressive fallback strategy:

1. **Sony Shared Memory Dual-Mapping (`OOPS_JIT_METHOD_SHARED_MEM`)**:
   - The platform-native mechanism exposes `sceKernelJitCreateSharedMemory`, `sceKernelJitMapSharedMemory`, and `sceKernelJitCreateAliasOfSharedMemory`.
   - The underlying physical memory is mapped to two distinct virtual addresses:
     - `rw_addr`: Writable view (mapped `SCE_PROT_READ | SCE_PROT_WRITE`) used by the recompiler to emit code.
     - `rx_addr`: Executable view (mapped `SCE_PROT_READ | SCE_PROT_EXEC`) used by the CPU to execute code.
   - This satisfies hardware W^X invariants without flipping page permissions.
   - This path is unlocked when the calling process carries JIT authority (e.g. WebKit Auth ID or `authinfo` stamped into the fSELF signature area).

2. **Relaxed W^X / Direct mprotect (`OOPS_JIT_METHOD_MPROTECT`)**:
   - On jailbroken systems running `kstuff-lite`, the kernel's `sys_mprotect` permission checks are bypassed via hardware debug-register traps (`mprotect_fix_start` -> `mprotect_fix_end`).
   - If `sceKernelJitCreateSharedMemory` returns `0x80020001` (`EPERM`) due to default game credentials, `oops_jit_alloc` transparently falls back to `mmap` + `mprotect` (`PROT_READ | PROT_WRITE | PROT_EXEC`).
   - In this mode, `rw_addr` and `rx_addr` point to the same virtual address, permitting immediate execution without requiring custom container authority blocks.

3. **Host Emulation (`OOPS_JIT_METHOD_HOST`)**:
   - On host test runners and CI environments, standard POSIX `mmap` / `mprotect` is utilized, ensuring full testability of dynamic code execution pipelines without hardware dependency.

### Cache Synchronization

Instruction caches must be synchronized and memory writes fenced before jumping to dynamically generated code. `oops_jit_flush_icache()` provides:
- Compiler cache clearing via `__builtin___clear_cache()`.
- Pipeline serialization and memory fencing via x86 `mfence`.

### Provenance and Safety

- All symbols (`sceKernelJitCreateSharedMemory`, `sceKernelJitMapSharedMemory`, `sceKernelJitCreateAliasOfSharedMemory`, `mmap`, `mprotect`, `munmap`) are declared as weak symbols or resolved dynamically.
- `oops_jit_alloc()` and `oops_jit_is_available()` fail safely and return `-1` or `0` on unpatched or unsupported platforms, adhering strictly to CONVENTIONS §3.

