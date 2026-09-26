#ifndef OOPS_HEAP_H
#define OOPS_HEAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The freestanding heap, with the C allocation contract.
 *
 * Backed by anonymous virtual memory (SYS_mmap / MAP_ANON | MAP_PRIVATE), not direct
 * memory, so it works in every application category (big apps, system apps with no
 * direct memory, daemons). `oops_aligned_alloc` follows C17: `size` a multiple of a
 * power-of-two `alignment`.
 */

void *oops_malloc(size_t size);
void oops_free(void *ptr);
void *oops_calloc(size_t count, size_t size);
void *oops_realloc(void *ptr, size_t new_size);
void *oops_aligned_alloc(size_t alignment, size_t size);

typedef struct oops_heap_stats {
    size_t current_allocated_bytes;
    size_t peak_allocated_bytes;
    size_t total_alloc_count;
    size_t total_free_count;
} oops_heap_stats_t;

/* A snapshot of the counters above. 0, or -1 for a NULL `out_stats`. */
int oops_heap_get_stats(oops_heap_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_HEAP_H */
