#include "oops/memory.h"

typedef int64_t sce_off_t;

struct obs_batch_map_entry {
    void *vaddr;
    sce_off_t paddr;
    size_t len;
    uint8_t prot;
    uint8_t pad[3];
    uint32_t flags;
};

__attribute__((weak)) int sceKernelAllocateMainDirectMemory(size_t len, size_t alignment, int memoryType, sce_off_t *paddr);
__attribute__((weak)) int sceKernelAllocateDirectMemory(sce_off_t searchStart, sce_off_t searchEnd,
                                                        size_t len, size_t alignment, int memoryType, sce_off_t *paddr);
__attribute__((weak)) int sceKernelGetDirectMemorySize(void);
__attribute__((weak)) int sceKernelReleaseDirectMemory(sce_off_t paddr, size_t len);
__attribute__((weak)) int sceKernelBatchMap(struct obs_batch_map_entry *entries, int num_entries, int *completed);
__attribute__((weak)) int sceKernelMunmap(void *addr, size_t len);

int oops_mem_alloc_direct(size_t size, size_t alignment, oops_mem_type_t type, int64_t *out_phys) {
    if (!out_phys) return -1;
    sce_off_t p = 0;
    int rc = -1;
    if (sceKernelAllocateMainDirectMemory) {
        rc = sceKernelAllocateMainDirectMemory(size, alignment, (int)type, &p);
    } else if (sceKernelAllocateDirectMemory && sceKernelGetDirectMemorySize) {
        rc = sceKernelAllocateDirectMemory(0, (sce_off_t)sceKernelGetDirectMemorySize(), size, alignment, (int)type, &p);
    }
    if (rc == 0) {
        *out_phys = (int64_t)p;
    }
    return rc;
}

int oops_mem_free_direct(int64_t phys, size_t size) {
    if (!sceKernelReleaseDirectMemory) return -1;
    return sceKernelReleaseDirectMemory((sce_off_t)phys, size);
}

int oops_mem_batch_map(void *vaddr_base, int64_t phys_base, size_t total_size, size_t page_size, uint8_t prot) {
    if (!sceKernelBatchMap || page_size == 0) return -1;
    size_t pages = (total_size + page_size - 1) / page_size;
    if (pages > 64) pages = 64; /* Cap per batch */

    struct obs_batch_map_entry entries[64];
    for (size_t i = 0; i < pages; i++) {
        entries[i].vaddr = (void *)((uintptr_t)vaddr_base + i * page_size);
        entries[i].paddr = (sce_off_t)(phys_base + (int64_t)i * (int64_t)page_size);
        entries[i].len   = page_size;
        entries[i].prot  = prot;
        entries[i].flags = 0;
    }
    int completed = 0;
    return sceKernelBatchMap(entries, (int)pages, &completed);
}

int oops_mem_unmap(void *vaddr, size_t size) {
    if (!sceKernelMunmap) return -1;
    return sceKernelMunmap(vaddr, size);
}
