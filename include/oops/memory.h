#ifndef OOPS_MEMORY_H
#define OOPS_MEMORY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum oops_mem_type {
    OOPS_MEM_WB_ONION  = 0,   /* CPU cached, GPU coherent (system memory) */
    OOPS_MEM_WC_GARLIC = 3,   /* GPU write-combined (framebuffers, render targets, textures) */
    OOPS_MEM_WB_GARLIC = 10   /* GPU write-back (scratch, compute scratch buffers) */
} oops_mem_type_t;

/* Memory protection flags */
#define OOPS_PROT_CPU_READ   0x11
#define OOPS_PROT_CPU_WRITE  0x22
#define OOPS_PROT_CPU_RW     (OOPS_PROT_CPU_READ | OOPS_PROT_CPU_WRITE)
#define OOPS_PROT_GPU_READ   0x10
#define OOPS_PROT_GPU_WRITE  0x20
#define OOPS_PROT_GPU_RW     (OOPS_PROT_GPU_READ | OOPS_PROT_GPU_WRITE)

/* Low-level physical and virtual direct memory operations */
int oops_mem_alloc_direct(size_t size, size_t alignment, oops_mem_type_t type, int64_t *out_phys);
int oops_mem_free_direct(int64_t phys, size_t size);
int oops_mem_map_direct(void **out_vaddr, size_t size, int prot, int flags, int64_t phys, size_t alignment);
int oops_mem_batch_map(void *vaddr_base, int64_t phys_base, size_t total_size, size_t page_size, uint8_t prot);
int oops_mem_unmap(void *vaddr, size_t size);

/* High-level managed GPU/CPU allocations: direct memory, mapped CPU+GPU read-write, tracked so
 * that free and the physical lookup need only the pointer. Sizes round up to 64 KB. Not
 * thread-safe. */
void *oops_mem_alloc(size_t size, size_t alignment, oops_mem_type_t type);
void oops_mem_free(void *ptr);

/* Physical offset behind a pointer into a managed allocation, or -1 if the pointer is not in
 * one. 0 is a valid physical offset and is never used to mean "unknown". */
int64_t oops_mem_get_phys(const void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_MEMORY_H */
