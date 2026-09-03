#ifndef OOPS_MEMORY_H
#define OOPS_MEMORY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum oops_mem_type {
    OOPS_MEM_WB_ONION  = 0,   /* CPU cached, GPU coherent */
    OOPS_MEM_WC_GARLIC = 3,   /* GPU write-combined (framebuffers, textures) */
    OOPS_MEM_WB_GARLIC = 10   /* GPU write-back */
} oops_mem_type_t;

int oops_mem_alloc_direct(size_t size, size_t alignment, oops_mem_type_t type, int64_t *out_phys);
int oops_mem_free_direct(int64_t phys, size_t size);
int oops_mem_batch_map(void *vaddr_base, int64_t phys_base, size_t total_size, size_t page_size, uint8_t prot);
int oops_mem_unmap(void *vaddr, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_MEMORY_H */
