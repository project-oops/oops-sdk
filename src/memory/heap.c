/*
 * The freestanding heap: a segregated-fit slab allocator for objects up to 4 KB and a
 * direct page mapping for larger ones, all backed by anonymous virtual memory
 * (SYS_mmap 477).
 */

#include "oops/heap.h"
#include "oops/freestd.h"
#include "oops/syscall.h"
#include "oops/system.h"

#ifdef OOPS_HOST_BUILD
#include <errno.h> /* the host's `errno`; the target reads it with `sys_get_errno` */
#include <sys/mman.h>
#include <unistd.h>
#endif

#define OOPS_HEAP_MAGIC 0x4f4f5053 /* "OOPS" */

#define OOPS_PROT_RW 0x03         /* PROT_READ | PROT_WRITE */
#define OOPS_MAP_ANON_PRIV 0x1002 /* MAP_PRIVATE | MAP_ANON (FreeBSD/Prospero) */
#define OOPS_PAGE_SIZE 16384UL    /* 16 KB default page alignment */
#define ARENA_SIZE 65536UL        /* 64 KB per slab arena */

typedef struct heap_block_header {
    uint32_t magic;
    uint32_t class_idx;
    size_t payload_size;
    size_t total_size;
    void *mmap_base;
    /* Physical offset when this block is backed by direct memory, 0 when it came from
     * flexible memory. Freeing a direct block has to release that offset as well as
     * unmapping the address, or the pool leaks with nothing to show for it. */
    uint64_t physical;
}
/*
 * **Sized to a multiple of 16, and that is not cosmetic.**
 *
 * Every pointer this heap returns is `block + sizeof(*this)`, so the header's size *is*
 * malloc's alignment guarantee. x86-64 requires 16 for the aligned SSE moves the
 * compiler emits freely, and adding `physical` took the header from 32 bytes to 40 -
 * which made every allocation in the process 8-byte aligned and faulted the first
 * aligned store into one. It arrived as a general protection fault at address 0
 * immediately after entry, long before anything that would point at the allocator.
 *
 * `aligned(16)` makes the compiler round the size up, so a field added later cannot
 * silently reintroduce this.
 */
__attribute__((aligned(16))) heap_block_header_t;

/* The guarantee above, as something the compiler refuses rather than something a reader
 * has to notice. A header that is not a multiple of 16 misaligns every pointer this
 * heap returns, and the first symptom is a general protection fault nowhere near here.
 */
_Static_assert(sizeof(heap_block_header_t) % 16u == 0u,
               "heap block header must keep malloc's pointers 16-byte aligned");

typedef struct free_chunk {
    struct free_chunk *next;
} free_chunk_t;

static const size_t s_class_sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
#define NUM_CLASSES (sizeof(s_class_sizes) / sizeof(s_class_sizes[0]))
#define MAX_SLAB_SIZE 4096

static free_chunk_t *s_freelists[NUM_CLASSES] = {0};
static oops_heap_stats_t s_stats = {0};
static volatile int s_heap_lock = 0;

static inline void heap_acquire(void) {
    while (__atomic_test_and_set(&s_heap_lock, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }
}

static inline void heap_release(void) {
    __atomic_clear(&s_heap_lock, __ATOMIC_RELEASE);
}

static void *sys_vm_alloc(size_t size) {
#ifndef OOPS_HOST_BUILD
    long ret =
        sys_call(SYS_mmap, 0, (long)size, OOPS_PROT_RW, OOPS_MAP_ANON_PRIV, -1, 0);
    if (ret < 0 || (unsigned long)ret >= 0xfffffffffffff000UL) {
        return NULL;
    }
    return (void *)ret;
#else
    void *ptr =
        mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return NULL;
    }
    return ptr;
#endif
}

#ifndef OOPS_HOST_BUILD
/*
 * Direct memory, for the allocations that flexible memory cannot hold.
 *
 * A big app is given two pools and they are not the same size. Measured on FW 12.40:
 * `FMEM size: 0x1c000000` is 448MB, and `DMEM size: 0x300000000` is 12GB. Anonymous
 * `mmap` draws from the flexible one, so every byte this heap ever handed out came from
 * that 448MB - and Ship of Harkinian converting a ROM reached 378MB of it before a 4MB
 * request was refused with no errno, which arrived at the player as `std::bad_alloc`.
 *
 * The large pool was sitting untouched apart from the display. So large blocks come
 * from there now, and small ones stay on flexible memory where the 2MB allocation
 * granularity below would be pure waste.
 *
 * Declared weak: a payload that links no `sce*` imports still builds, and the fallback
 * to `mmap` keeps working where these do not bind.
 */
__attribute__((weak)) int
sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd, size_t len,
                              size_t alignment, int memoryType, int64_t *physicalOut);
__attribute__((weak)) int sceKernelMapDirectMemory(void **addrInOut, size_t len,
                                                   int prot, int flags,
                                                   int64_t physicalAddr,
                                                   size_t alignment);
__attribute__((weak)) int sceKernelReleaseDirectMemory(int64_t start, size_t len);
__attribute__((weak)) size_t sceKernelGetDirectMemorySize(void);

/* Direct memory is allocated in 2MB units, which is why it is not used for small
 * blocks. Anything at or above this threshold rounds up to that granularity. */
#define OOPS_DIRECT_ALIGN 0x200000u
#define OOPS_DIRECT_MIN_BYTES 0x100000u

/*
 * `memoryType` is the one value here not measured for this use. The display allocates
 * type 3, but that is memory a GPU reads; for CPU-visible heap memory the write-back
 * type is the documented choice. Rather than pick one and have a wrong guess look like
 * a missing pool, both are tried in turn and the one that answers is logged and
 * remembered.
 */
static int s_direct_type = -1;

static void *sys_vm_alloc_direct(size_t size, uint64_t *physical_out) {
    static const int types[] = {0, 3};
    const size_t len =
        (size + (OOPS_DIRECT_ALIGN - 1)) & ~(size_t)(OOPS_DIRECT_ALIGN - 1);
    size_t i;

    if (!sceKernelAllocateDirectMemory || !sceKernelMapDirectMemory ||
        !sceKernelGetDirectMemorySize) {
        return NULL;
    }

    for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        int64_t physical = 0;
        void *addr = NULL;
        int rc;

        if (s_direct_type >= 0 && types[i] != s_direct_type) {
            continue;
        }

        rc = sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(),
                                           len, OOPS_DIRECT_ALIGN, types[i], &physical);
        if (rc != 0) {
            continue;
        }

        /* Read and write for the CPU; this is heap memory, not something a GPU reads.
         */
        rc = sceKernelMapDirectMemory(&addr, len, 0x3, 0, physical, OOPS_DIRECT_ALIGN);
        if (rc != 0 || addr == NULL) {
            if (sceKernelReleaseDirectMemory) {
                (void)sceKernelReleaseDirectMemory(physical, len);
            }
            continue;
        }

        if (s_direct_type < 0) {
            s_direct_type = types[i];
            oops_log_info("HEAP",
                          "large blocks are coming from direct memory (type %d, %lluMB "
                          "pool) - flexible memory is only 448MB on this platform",
                          types[i],
                          (unsigned long long)sceKernelGetDirectMemorySize() /
                              (1024u * 1024u));
        }
        *physical_out = (uint64_t)physical;
        return addr;
    }

    return NULL;
}
#endif /* OOPS_HOST_BUILD */

static void sys_vm_free(void *ptr, size_t size) {
    if (ptr == NULL || size == 0) {
        return;
    }
#ifndef OOPS_HOST_BUILD
    (void)sys_call(SYS_munmap, (long)ptr, (long)size, 0, 0, 0, 0);
#else
    (void)munmap(ptr, size);
#endif
}

static int find_class(size_t size) {
    for (size_t i = 0; i < NUM_CLASSES; i++) {
        if (size <= s_class_sizes[i]) {
            return (int)i;
        }
    }
    return -1;
}

static int replenish_class(int class_idx) {
    size_t chunk_payload = s_class_sizes[class_idx];
    size_t chunk_total = sizeof(heap_block_header_t) + chunk_payload;

    /* Align chunk total to 16 bytes */
    chunk_total = (chunk_total + 15) & ~15ULL;

    size_t arena_sz = ARENA_SIZE;
    if (chunk_total * 4 > arena_sz) {
        arena_sz = chunk_total * 16;
    }

    uint8_t *arena = (uint8_t *)sys_vm_alloc(arena_sz);

#ifndef OOPS_HOST_BUILD
    /*
     * Flexible memory first, direct memory when it runs out.
     *
     * Small arenas belong in the flexible pool: direct memory is handed out in 2MB
     * units, so backing a 64KB arena with it wastes most of the mapping. But "wasteful"
     * beats "out of memory" by a distance - Ship of Harkinian converting a ROM reached
     * file 538 of 547 and then died here, with 12GB of direct memory untouched, because
     * this path had no second choice.
     *
     * The arena is rounded up to the granularity and the extra is carved into chunks
     * rather than wasted, so a fallback arena yields more objects, not fewer. Arenas
     * are never released, so unlike the large path there is no offset to keep.
     */
    if (arena == NULL) {
        uint64_t physical = 0;
        size_t direct_sz =
            (arena_sz + (OOPS_DIRECT_ALIGN - 1)) & ~(size_t)(OOPS_DIRECT_ALIGN - 1);

        arena = (uint8_t *)sys_vm_alloc_direct(direct_sz, &physical);
        if (arena != NULL) {
            arena_sz = direct_sz;
            oops_log_info(
                "HEAP",
                "flexible memory is full; class %d arenas now come from direct "
                "memory (%zuKB)",
                class_idx, direct_sz / 1024u);
        }
    }
#endif

    if (arena == NULL) {
        oops_log_warn("HEAP", "replenish_class %d (%zu bytes) failed: out of memory",
                      class_idx, chunk_payload);
        return -1;
    }

    size_t count = arena_sz / chunk_total;
    oops_log_trace("HEAP", "replenished class %d (%zu bytes): arena=%zu count=%zu",
                   class_idx, chunk_payload, arena_sz, count);
    for (size_t i = 0; i < count; i++) {
        uint8_t *block = arena + (i * chunk_total);
        heap_block_header_t *hdr = (heap_block_header_t *)block;
        hdr->magic = OOPS_HEAP_MAGIC;
        hdr->class_idx = (uint32_t)class_idx;
        hdr->payload_size = 0;
        hdr->total_size = chunk_total;

        free_chunk_t *node = (free_chunk_t *)(block + sizeof(heap_block_header_t));
        node->next = s_freelists[class_idx];
        s_freelists[class_idx] = node;
    }

    return 0;
}

void *oops_malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    heap_acquire();

    int class_idx = find_class(size);
    if (class_idx >= 0) {
        /* Small allocation from slab */
        if (s_freelists[class_idx] == NULL) {
            if (replenish_class(class_idx) != 0) {
                heap_release();
                return NULL;
            }
        }

        free_chunk_t *node = s_freelists[class_idx];
        s_freelists[class_idx] = node->next;

        heap_block_header_t *hdr =
            (heap_block_header_t *)((uint8_t *)node - sizeof(heap_block_header_t));
        hdr->payload_size = size;

        s_stats.current_allocated_bytes += size;
        if (s_stats.current_allocated_bytes > s_stats.peak_allocated_bytes) {
            s_stats.peak_allocated_bytes = s_stats.current_allocated_bytes;
        }
        s_stats.total_alloc_count++;

        heap_release();
        return (void *)node;
    }

    /* Large allocation via direct mmap */
    size_t total_needed = sizeof(heap_block_header_t) + size;
    size_t page_aligned = (total_needed + OOPS_PAGE_SIZE - 1) & ~(OOPS_PAGE_SIZE - 1);

    uint64_t physical = 0;
    uint8_t *block = NULL;

    /*
     * Both pools, every time - only the order changes.
     *
     * The threshold decides which is *preferred*, not which is *available*: above it
     * the 2MB granularity of direct memory is worth paying, below it the flexible pool
     * is the better fit. But either can be empty, and a size just under the threshold
     * must not be refused while 12GB sits free. Measured: a 975,120-byte request failed
     * with the flexible pool at its 448MB ceiling, purely because it was 73KB below the
     * cutoff.
     */
#ifndef OOPS_HOST_BUILD
    if (page_aligned >= OOPS_DIRECT_MIN_BYTES) {
        block = (uint8_t *)sys_vm_alloc_direct(page_aligned, &physical);
        if (block != NULL) {
            page_aligned = (page_aligned + (OOPS_DIRECT_ALIGN - 1)) &
                           ~(size_t)(OOPS_DIRECT_ALIGN - 1);
        }
    }
#endif

    if (block == NULL) {
        physical = 0;
        block = (uint8_t *)sys_vm_alloc(page_aligned);
    }

#ifndef OOPS_HOST_BUILD
    /* Flexible memory refused it; take the granularity hit rather than the failure. */
    if (block == NULL) {
        block = (uint8_t *)sys_vm_alloc_direct(page_aligned, &physical);
        if (block != NULL) {
            page_aligned = (page_aligned + (OOPS_DIRECT_ALIGN - 1)) &
                           ~(size_t)(OOPS_DIRECT_ALIGN - 1);
        }
    }
#endif
    if (block == NULL) {
        /*
         * Say why, and say what was already out.
         *
         * "failed for N bytes" on its own names neither the reason nor the scale, and a
         * caller turns it straight into `std::bad_alloc` with the size lost. The three
         * facts that separate the cases: `errno` from the kernel (a refused mapping and
         * an exhausted budget are different faults), how much this heap is already
         * holding, and its peak - a request for 4MB failing while the heap holds 40MB
         * is a budget, while the same failure holding almost nothing is the mapping
         * call itself.
         */
#ifndef OOPS_HOST_BUILD
        const int err = sys_get_errno();
#else
        const int err = errno;
#endif
        const unsigned long long held =
            (unsigned long long)s_stats.current_allocated_bytes;
        const unsigned long long peak =
            (unsigned long long)s_stats.peak_allocated_bytes;

        heap_release();
        oops_log_warn("HEAP",
                      "large mmap alloc failed: wanted %zu bytes (%zu page-aligned), "
                      "errno=%d, heap already holds %lluKB, peak %lluKB",
                      size, page_aligned, err, held / 1024u, peak / 1024u);
        return NULL;
    }

    heap_block_header_t *hdr = (heap_block_header_t *)block;
    hdr->magic = OOPS_HEAP_MAGIC;
    hdr->class_idx = 0xFF; /* Direct mmap marker */
    hdr->payload_size = size;
    hdr->total_size = page_aligned;
    hdr->mmap_base = block;
    hdr->physical = physical;

    s_stats.current_allocated_bytes += size;
    if (s_stats.current_allocated_bytes > s_stats.peak_allocated_bytes) {
        s_stats.peak_allocated_bytes = s_stats.current_allocated_bytes;
    }
    s_stats.total_alloc_count++;

    heap_release();
    oops_log_trace("HEAP", "large mmap alloc: size=%zu page_aligned=%zu ptr=%p", size,
                   page_aligned, (void *)(block + sizeof(heap_block_header_t)));
    return (void *)(block + sizeof(heap_block_header_t));
}

void *oops_aligned_alloc(size_t alignment, size_t size) {
    if (size == 0 || alignment == 0) {
        return NULL;
    }
    /* Alignment must be a power of two */
    if ((alignment & (alignment - 1)) != 0) {
        return NULL;
    }
    /* Size must be an integral multiple of alignment (C17 §7.22.3.1) */
    if ((size % alignment) != 0) {
        return NULL;
    }
    if (alignment < 16) {
        alignment = 16;
    }

    size_t total_needed = size + alignment + sizeof(heap_block_header_t);
    size_t page_aligned = (total_needed + OOPS_PAGE_SIZE - 1) & ~(OOPS_PAGE_SIZE - 1);

    uint8_t *block = (uint8_t *)sys_vm_alloc(page_aligned);
    if (block == NULL) {
        oops_log_warn("HEAP", "aligned alloc failed: align=%zu size=%zu", alignment,
                      size);
        return NULL;
    }

    uintptr_t min_payload = (uintptr_t)block + sizeof(heap_block_header_t);
    uintptr_t aligned_addr = (min_payload + (alignment - 1)) & ~(alignment - 1);

    heap_block_header_t *hdr =
        (heap_block_header_t *)(aligned_addr - sizeof(heap_block_header_t));
    hdr->magic = OOPS_HEAP_MAGIC;
    hdr->class_idx = 0xFE; /* Aligned direct mmap marker */
    hdr->payload_size = size;
    hdr->total_size = page_aligned;
    hdr->mmap_base = block;

    heap_acquire();
    s_stats.current_allocated_bytes += size;
    if (s_stats.current_allocated_bytes > s_stats.peak_allocated_bytes) {
        s_stats.peak_allocated_bytes = s_stats.current_allocated_bytes;
    }
    s_stats.total_alloc_count++;
    heap_release();

    oops_log_trace("HEAP", "aligned alloc: align=%zu size=%zu addr=%p", alignment, size,
                   (void *)aligned_addr);
    return (void *)aligned_addr;
}

void oops_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    heap_block_header_t *hdr =
        (heap_block_header_t *)((uint8_t *)ptr - sizeof(heap_block_header_t));
    if (hdr->magic != OOPS_HEAP_MAGIC) {
        /* Corrupted or foreign pointer */
        oops_log_warn("HEAP",
                      "oops_free: block corruption or bad pointer %p (magic=0x%x)", ptr,
                      hdr->magic);
        return;
    }

    heap_acquire();

    size_t user_size = hdr->payload_size;
    if (s_stats.current_allocated_bytes >= user_size) {
        s_stats.current_allocated_bytes -= user_size;
    }
    s_stats.total_free_count++;

    if (hdr->class_idx < NUM_CLASSES) {
        /* Return to slab freelist */
        hdr->payload_size = 0;
        free_chunk_t *node = (free_chunk_t *)ptr;
        node->next = s_freelists[hdr->class_idx];
        s_freelists[hdr->class_idx] = node;
    } else if (hdr->class_idx == 0xFF || hdr->class_idx == 0xFE) {
        /* Unmap direct large or aligned allocation */
        hdr->magic = 0;
        void *mmap_base = hdr->mmap_base ? hdr->mmap_base : (void *)hdr;
        size_t total_sz = hdr->total_size;
        uint64_t physical = hdr->physical;
        sys_vm_free(mmap_base, total_sz);
#ifndef OOPS_HOST_BUILD
        /* A direct block's address is unmapped above; the pool behind it is separate
         * and has to be given back too, or 12GB drains one allocation at a time. */
        if (physical != 0 && sceKernelReleaseDirectMemory) {
            (void)sceKernelReleaseDirectMemory((int64_t)physical, total_sz);
        }
#else
        (void)physical;
#endif
    }

    heap_release();
}

void *oops_calloc(size_t count, size_t size) {
    if (count == 0 || size == 0) {
        return NULL;
    }
    size_t total = count * size;
    if (total / count != size) {
        return NULL; /* Overflow check */
    }

    void *ptr = oops_malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *oops_realloc(void *ptr, size_t new_size) {
    if (ptr == NULL) {
        return oops_malloc(new_size);
    }
    if (new_size == 0) {
        oops_free(ptr);
        return NULL;
    }

    heap_block_header_t *hdr =
        (heap_block_header_t *)((uint8_t *)ptr - sizeof(heap_block_header_t));
    if (hdr->magic != OOPS_HEAP_MAGIC) {
        oops_log_warn("HEAP", "oops_realloc: block corruption or bad pointer %p", ptr);
        return NULL;
    }

    heap_acquire();
    size_t cur_payload = hdr->payload_size;
    size_t cur_total = hdr->total_size;
    uint32_t class_idx = hdr->class_idx;
    heap_release();

    /* If it fits in the current small block capacity, adjust payload size in-place */
    if (class_idx < NUM_CLASSES) {
        size_t class_capacity = s_class_sizes[class_idx];
        if (new_size <= class_capacity) {
            heap_acquire();
            if (new_size > cur_payload) {
                s_stats.current_allocated_bytes += (new_size - cur_payload);
            } else {
                s_stats.current_allocated_bytes -= (cur_payload - new_size);
            }
            if (s_stats.current_allocated_bytes > s_stats.peak_allocated_bytes) {
                s_stats.peak_allocated_bytes = s_stats.current_allocated_bytes;
            }
            hdr->payload_size = new_size;
            heap_release();
            return ptr;
        }
    } else if (class_idx == 0xFF) {
        /* If large block capacity is sufficient */
        size_t avail = cur_total - sizeof(heap_block_header_t);
        if (new_size <= avail && new_size > (avail / 2)) {
            heap_acquire();
            if (new_size > cur_payload) {
                s_stats.current_allocated_bytes += (new_size - cur_payload);
            } else {
                s_stats.current_allocated_bytes -= (cur_payload - new_size);
            }
            if (s_stats.current_allocated_bytes > s_stats.peak_allocated_bytes) {
                s_stats.peak_allocated_bytes = s_stats.current_allocated_bytes;
            }
            hdr->payload_size = new_size;
            heap_release();
            return ptr;
        }
    }

    void *new_ptr = oops_malloc(new_size);
    if (new_ptr == NULL) {
        return NULL;
    }

    size_t copy_len = (new_size < cur_payload) ? new_size : cur_payload;
    memcpy(new_ptr, ptr, copy_len);
    oops_free(ptr);
    return new_ptr;
}

int oops_heap_get_stats(oops_heap_stats_t *out_stats) {
    if (out_stats == NULL) {
        return -1;
    }
    heap_acquire();
    *out_stats = s_stats;
    heap_release();
    oops_log_trace("HEAP", "get_stats: allocated=%zu peak=%zu allocs=%zu frees=%zu",
                   out_stats->current_allocated_bytes, out_stats->peak_allocated_bytes,
                   out_stats->total_alloc_count, out_stats->total_free_count);
    return 0;
}
