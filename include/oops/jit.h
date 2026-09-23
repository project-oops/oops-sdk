#ifndef OOPS_JIT_H
#define OOPS_JIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Strategy used to establish executable dynamic memory.
 */
typedef enum oops_jit_method {
  OOPS_JIT_METHOD_NONE = 0,       /* Dynamic executable memory is unavailable */
  OOPS_JIT_METHOD_SHARED_MEM = 1, /* Sony sceKernelJitCreateSharedMemory dual-mapping */
  OOPS_JIT_METHOD_MPROTECT = 2,   /* mprotect / mmap execution (unlocked under kstuff-lite) */
  OOPS_JIT_METHOD_HOST = 3        /* Host test environment (POSIX mprotect / mmap) */
} oops_jit_method_t;

/**
 * Handle and dual-view pointers for allocated JIT memory.
 *
 * In accordance with W^X architectures:
 * - rw_addr: Writable view used to emit or recompile machine instructions.
 * - rx_addr: Executable view used to invoke the generated instructions.
 *
 * On systems supporting shared memory dual-mapping, rw_addr and rx_addr are
 * distinct virtual addresses pointing to the same underlying physical frames.
 * On systems with relaxed W^X (e.g. kstuff-lite mprotect), rw_addr and rx_addr
 * may point to the same virtual address.
 */
typedef struct oops_jit_memory {
  void *rx_addr;  /* Executable view (read-exec) */
  void *rw_addr;  /* Writable view (read-write) */
  size_t size;    /* Allocated size in bytes (page-aligned) */
  int handle;     /* Shared memory handle descriptor, or -1 */
  int method;     /* Method used (oops_jit_method_t) */
} oops_jit_memory_t;

/**
 * Check if dynamic executable memory generation is available in the current
 * process environment.
 *
 * Returns 1 if supported, 0 if refused or unavailable.
 */
int oops_jit_is_available(void);

/**
 * Query the active or preferred JIT strategy for the current environment.
 *
 * Returns an oops_jit_method_t value.
 */
int oops_jit_get_method(void);

/**
 * Allocate a dynamic executable buffer of at least `size` bytes.
 *
 * Rounds `size` up to the system page boundary (typically 16 KB on Prospero,
 * 4 KB on host). Attempts Sony shared memory dual-mapping first. If refused
 * by the kernel (e.g. without custom authinfo), falls back to direct mprotect
 * if permitted by the environment (e.g. kstuff-lite).
 *
 * Returns 0 on success with `out_mem` populated, or -1 on error.
 */
int oops_jit_alloc(size_t size, oops_jit_memory_t *out_mem);

/**
 * Free a previously allocated JIT memory buffer.
 *
 * Unmaps all active views, closes any open shared memory handles, and zeroes
 * the memory structure.
 *
 * Returns 0 on success, or -1 on error.
 */
int oops_jit_free(oops_jit_memory_t *mem);

/**
 * Flush CPU instruction caches and serialize pipeline execution for the
 * modified code range.
 *
 * Must be invoked after emitting machine code into `rw_addr` and before
 * jumping to `rx_addr`.
 *
 * Returns 0 on success, or -1 on invalid arguments.
 */
int oops_jit_flush_icache(const void *addr, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_JIT_H */

