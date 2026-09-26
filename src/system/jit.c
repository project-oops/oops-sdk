/*
 * Executable memory for code generated at run time (oops/jit.h).
 *
 * On the target it prefers a shared-memory dual mapping (separate writable and
 * executable views of the same pages) and falls back to `mprotect` on one mapping.
 * The host build uses POSIX `mmap`. A caller writes through `rw_addr`, runs through
 * `rx_addr`, and calls `oops_jit_flush_icache` in between.
 */

#include "oops/jit.h"
#include "oops/freestd.h"
#include "oops/system.h"

#if defined(OOPS_HOST_BUILD) || (defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 1)
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

#define JIT_HOST_PAGE_SIZE 4096UL

static size_t get_host_page_size(void) {
#if defined(_SC_PAGESIZE)
    long sz = sysconf(_SC_PAGESIZE);
    if (sz > 0) {
        return (size_t)sz;
    }
#endif
    return JIT_HOST_PAGE_SIZE;
}

static size_t align_page(size_t size) {
    size_t pg = get_host_page_size();
    return (size + pg - 1) & ~(pg - 1);
}

int oops_jit_is_available(void) {
    return 1;
}

int oops_jit_get_method(void) {
    return OOPS_JIT_METHOD_HOST;
}

int oops_jit_alloc(size_t size, oops_jit_memory_t *out_mem) {
    if (!out_mem || size == 0) {
        oops_log_warn("JIT", "alloc invalid arguments");
        return -1;
    }

    size_t aligned = align_page(size);
    if (aligned < size) {
        oops_log_warn("JIT", "alloc size overflow size=%zu", size);
        return -1; /* Overflow */
    }

    /* One view that is both, or nothing: a read-write mapping handed out as `rx_addr`
     * would fault on the first call into it. */
    void *ptr = mmap(NULL, aligned, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (ptr == MAP_FAILED || !ptr) {
        oops_log_warn("JIT",
                      "alloc: the host refused a writable, executable mapping "
                      "of %zu bytes",
                      aligned);
        return -1;
    }

    out_mem->rx_addr = ptr;
    out_mem->rw_addr = ptr;
    out_mem->size = aligned;
    out_mem->handle = -1;
    out_mem->method = OOPS_JIT_METHOD_HOST;
    oops_log_debug("JIT", "alloc size=%zu aligned=%zu -> addr=%p (HOST)", size, aligned,
                   ptr);
    return 0;
}

int oops_jit_free(oops_jit_memory_t *mem) {
    if (!mem || !mem->rx_addr || mem->size == 0) {
        return -1;
    }
    oops_log_debug("JIT", "free rx=%p rw=%p size=%zu (HOST)", mem->rx_addr,
                   mem->rw_addr, mem->size);
    int rc = munmap(mem->rx_addr, mem->size);
    mem->rx_addr = NULL;
    mem->rw_addr = NULL;
    mem->size = 0;
    mem->handle = -1;
    mem->method = OOPS_JIT_METHOD_NONE;
    return (rc == 0) ? 0 : -1;
}

int oops_jit_flush_icache(const void *addr, size_t size) {
    if (!addr || size == 0) {
        return -1;
    }

#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char *)(uintptr_t)addr, (char *)(uintptr_t)addr + size);
#endif
#if defined(__x86_64__) || defined(_M_X64)
    __asm__ volatile("mfence" ::: "memory");
#endif
    return 0;
}

#else

/* Target implementation: Prospero freestanding runtime */
#include "oops/syscall.h"

#define JIT_TARGET_PAGE_SIZE 0x4000UL /* 16 KB pages */

#define SCE_PROT_READ 0x01
#define SCE_PROT_WRITE 0x02
#define SCE_PROT_EXEC 0x04
#define SCE_PROT_RWX (SCE_PROT_READ | SCE_PROT_WRITE | SCE_PROT_EXEC)
#define SCE_PROT_RX (SCE_PROT_READ | SCE_PROT_EXEC)
#define SCE_PROT_RW (SCE_PROT_READ | SCE_PROT_WRITE)

#define MAP_ANON 0x1000
#define MAP_PRIVATE 0x0002

/* Platform dynamic symbol hooks from libkernel */
__attribute__((weak)) int sceKernelJitCreateSharedMemory(const char *name, size_t size,
                                                         int flags, int *handle);
__attribute__((weak)) int sceKernelJitMapSharedMemory(int handle, int flags,
                                                      void **addr);
__attribute__((weak)) int sceKernelJitCreateAliasOfSharedMemory(int handle, int flags,
                                                                void **addr);
__attribute__((weak)) int sceKernelMunmap(void *addr, size_t size);
__attribute__((weak)) int sceKernelClose(int fd);
__attribute__((weak)) void *mmap(void *addr, size_t len, int prot, int flags, int fd,
                                 int64_t offset);
__attribute__((weak)) int mprotect(void *addr, size_t len, int prot);
__attribute__((weak)) int munmap(void *addr, size_t len);

static int s_jit_probed = 0;
static int s_jit_supported = 0;
static int s_jit_cached_method = OOPS_JIT_METHOD_NONE;

static size_t align_page(size_t size) {
    return (size + (JIT_TARGET_PAGE_SIZE - 1)) & ~(JIT_TARGET_PAGE_SIZE - 1);
}

static int try_alloc_shared_mem(size_t aligned, oops_jit_memory_t *out_mem) {
    if (!sceKernelJitCreateSharedMemory || !sceKernelJitMapSharedMemory ||
        !sceKernelJitCreateAliasOfSharedMemory) {
        return -1;
    }

    int handle = -1;
    int rc = sceKernelJitCreateSharedMemory("oops-jit", aligned, SCE_PROT_RWX, &handle);
    if (rc != 0 || handle < 0) {
        return -1;
    }

    void *rx_view = NULL;
    rc = sceKernelJitMapSharedMemory(handle, SCE_PROT_RX, &rx_view);
    if (rc != 0 || !rx_view) {
        if (sceKernelClose) {
            sceKernelClose(handle);
        } else {
            sys_call(SYS_close, handle, 0, 0, 0, 0, 0);
        }
        return -1;
    }

    void *rw_view = NULL;
    rc = sceKernelJitCreateAliasOfSharedMemory(handle, SCE_PROT_RW, &rw_view);
    if (rc != 0 || !rw_view) {
        if (sceKernelMunmap) {
            sceKernelMunmap(rx_view, aligned);
        } else {
            sys_call(SYS_munmap, (long)rx_view, (long)aligned, 0, 0, 0, 0);
        }
        if (sceKernelClose) {
            sceKernelClose(handle);
        } else {
            sys_call(SYS_close, handle, 0, 0, 0, 0, 0);
        }
        return -1;
    }

    out_mem->rx_addr = rx_view;
    out_mem->rw_addr = rw_view;
    out_mem->size = aligned;
    out_mem->handle = handle;
    out_mem->method = OOPS_JIT_METHOD_SHARED_MEM;
    return 0;
}

static int try_alloc_mprotect(size_t aligned, oops_jit_memory_t *out_mem) {
    void *ptr = NULL;

    if (mmap) {
        ptr = mmap(NULL, aligned, SCE_PROT_RW, MAP_ANON | MAP_PRIVATE, -1, 0);
    } else {
        ptr = (void *)sys_call(SYS_mmap, 0, (long)aligned, SCE_PROT_RW,
                               MAP_ANON | MAP_PRIVATE, -1, 0);
    }

    if (!ptr || ptr == (void *)-1) {
        return -1;
    }

    /* Attempt to relax permission to RWX (unblocked by kstuff-lite trap) */
    int mrc = -1;
    if (mprotect) {
        mrc = mprotect(ptr, aligned, SCE_PROT_RWX);
    } else {
        mrc = (int)sys_call(SYS_mprotect, (long)ptr, (long)aligned, SCE_PROT_RWX, 0, 0,
                            0);
    }

    if (mrc != 0) {
        if (munmap) {
            munmap(ptr, aligned);
        } else {
            sys_call(SYS_munmap, (long)ptr, (long)aligned, 0, 0, 0, 0);
        }
        return -1;
    }

    out_mem->rx_addr = ptr;
    out_mem->rw_addr = ptr;
    out_mem->size = aligned;
    out_mem->handle = -1;
    out_mem->method = OOPS_JIT_METHOD_MPROTECT;
    return 0;
}

static void probe_jit(void) {
    if (s_jit_probed) {
        return;
    }

    oops_jit_memory_t test_mem;
    memset(&test_mem, 0, sizeof(test_mem));

    /* 1. Try Sony shared memory dual-mapping */
    if (try_alloc_shared_mem(JIT_TARGET_PAGE_SIZE, &test_mem) == 0) {
        s_jit_supported = 1;
        s_jit_cached_method = OOPS_JIT_METHOD_SHARED_MEM;
        oops_jit_free(&test_mem);
        s_jit_probed = 1;
        return;
    }

    /* 2. Try direct mprotect */
    if (try_alloc_mprotect(JIT_TARGET_PAGE_SIZE, &test_mem) == 0) {
        s_jit_supported = 1;
        s_jit_cached_method = OOPS_JIT_METHOD_MPROTECT;
        oops_jit_free(&test_mem);
        s_jit_probed = 1;
        oops_log_info("JIT", "probe result: supported=%d method=MPROTECT",
                      s_jit_supported);
        return;
    }

    s_jit_supported = 0;
    s_jit_cached_method = OOPS_JIT_METHOD_NONE;
    s_jit_probed = 1;
    oops_log_warn("JIT", "probe result: JIT unavailable");
}

int oops_jit_is_available(void) {
    probe_jit();
    return s_jit_supported;
}

int oops_jit_get_method(void) {
    probe_jit();
    return s_jit_cached_method;
}

int oops_jit_alloc(size_t size, oops_jit_memory_t *out_mem) {
    if (!out_mem || size == 0) {
        oops_log_warn("JIT", "alloc invalid arguments");
        return -1;
    }

    memset(out_mem, 0, sizeof(*out_mem));
    size_t aligned = align_page(size);
    if (aligned < size) {
        oops_log_warn("JIT", "alloc size overflow size=%zu", size);
        return -1;
    }

    /* Attempt Sony shared memory first */
    if (try_alloc_shared_mem(aligned, out_mem) == 0) {
        oops_log_debug("JIT", "alloc size=%zu -> rx=%p rw=%p handle=%d (SHARED_MEM)",
                       size, out_mem->rx_addr, out_mem->rw_addr, out_mem->handle);
        return 0;
    }

    /* Fall back to direct mprotect (kstuff-lite) */
    if (try_alloc_mprotect(aligned, out_mem) == 0) {
        oops_log_debug("JIT", "alloc size=%zu -> rx=%p rw=%p (MPROTECT)", size,
                       out_mem->rx_addr, out_mem->rw_addr);
        return 0;
    }

    oops_log_warn("JIT", "alloc failed for size=%zu aligned=%zu", size, aligned);
    return -1;
}

int oops_jit_free(oops_jit_memory_t *mem) {
    if (!mem || mem->size == 0) {
        return -1;
    }

    oops_log_debug("JIT", "free rx=%p rw=%p size=%zu handle=%d method=%d", mem->rx_addr,
                   mem->rw_addr, mem->size, mem->handle, mem->method);
    int rc = 0;

    if (mem->method == OOPS_JIT_METHOD_SHARED_MEM) {
        if (mem->rx_addr) {
            if (sceKernelMunmap) {
                rc |= sceKernelMunmap(mem->rx_addr, mem->size);
            } else {
                rc |= (int)sys_call(SYS_munmap, (long)mem->rx_addr, (long)mem->size, 0,
                                    0, 0, 0);
            }
        }
        if (mem->rw_addr && mem->rw_addr != mem->rx_addr) {
            if (sceKernelMunmap) {
                rc |= sceKernelMunmap(mem->rw_addr, mem->size);
            } else {
                rc |= (int)sys_call(SYS_munmap, (long)mem->rw_addr, (long)mem->size, 0,
                                    0, 0, 0);
            }
        }
        if (mem->handle >= 0) {
            if (sceKernelClose) {
                rc |= sceKernelClose(mem->handle);
            } else {
                rc |= (int)sys_call(SYS_close, mem->handle, 0, 0, 0, 0, 0);
            }
        }
    } else {
        void *addr = mem->rx_addr ? mem->rx_addr : mem->rw_addr;
        if (addr) {
            if (munmap) {
                rc |= munmap(addr, mem->size);
            } else {
                rc |=
                    (int)sys_call(SYS_munmap, (long)addr, (long)mem->size, 0, 0, 0, 0);
            }
        }
    }

    memset(mem, 0, sizeof(*mem));
    return (rc == 0) ? 0 : -1;
}

int oops_jit_flush_icache(const void *addr, size_t size) {
    if (!addr || size == 0) {
        return -1;
    }

#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char *)(uintptr_t)addr, (char *)(uintptr_t)addr + size);
#endif
    __asm__ volatile("mfence" ::: "memory");
    return 0;
}

#endif
