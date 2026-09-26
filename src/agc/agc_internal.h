#ifndef OOPS_AGC_INTERNAL_H
#define OOPS_AGC_INTERNAL_H

#include "oops/gpu.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct oops_gpu_queue {
    void *agc_queue_handle;
    uint8_t *dcb_mem;
    size_t dcb_size;
    volatile uint32_t *fence;
    uint32_t last_fence;
};

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AGC_INTERNAL_H */
