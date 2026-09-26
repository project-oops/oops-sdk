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
} heap_block_header_t;

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

    uint8_t *block = (uint8_t *)sys_vm_alloc(page_aligned);
    if (block == NULL) {
        heap_release();
        oops_log_warn("HEAP", "large mmap alloc failed for %zu bytes", size);
        return NULL;
    }

    heap_block_header_t *hdr = (heap_block_header_t *)block;
    hdr->magic = OOPS_HEAP_MAGIC;
    hdr->class_idx = 0xFF; /* Direct mmap marker */
    hdr->payload_size = size;
    hdr->total_size = page_aligned;
    hdr->mmap_base = block;

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
        sys_vm_free(mmap_base, total_sz);
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
