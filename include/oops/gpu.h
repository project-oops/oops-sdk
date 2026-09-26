/*
 * GPU compute: command queues, compute shaders and synchronous dispatches on AGC.
 * Every call fails cleanly on the host and on targets without GPU access.
 */
#ifndef OOPS_GPU_H
#define OOPS_GPU_H

#include "oops/target.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque handles for hardware GPU command queue and compute shader objects.
 */
typedef struct oops_gpu_queue oops_gpu_queue_t;
typedef struct oops_gpu_shader oops_gpu_shader_t;

/*
 * Definition of a compute dispatch workload.
 *
 * - shader: An instantiated compute shader created via
 * oops_gpu_create_shader().
 * - user_data: Array of 32-bit user SGPR register values (e.g. buffer
 * descriptors, constants, or memory virtual addresses mapped to
 * COMPUTE_USER_DATA_0..15).
 * - user_data_count: Number of DWORDs in user_data (0 to 16).
 * - grid_x, grid_y, grid_z: Threadgroup dimensions for DISPATCH_DIRECT.
 */
typedef struct oops_gpu_dispatch {
    const oops_gpu_shader_t *shader;
    const uint32_t *user_data;
    uint32_t user_data_count;
    uint32_t grid_x;
    uint32_t grid_y;
    uint32_t grid_z;
} oops_gpu_dispatch_t;

/*
 * Returns 1 if hardware GPU compute is supported and active on this platform
 * (Prospero/Trinity with libSceAgc/libSceAgcDriver); returns 0 on host builds
 * or environments without direct GPU access.
 */
int oops_gpu_available(void);

/*
 * Create an AGC hardware compute queue (Type 3). Allocates direct coherent
 * Onion WB memory for command stream buffers and synchronization fences.
 * Returns NULL on host or if queue creation fails.
 */
oops_gpu_queue_t *oops_gpu_create_compute_queue(void);

/*
 * Create an AGC hardware universal graphics queue (Type 0). Allocates direct coherent
 * Onion WB memory for command stream buffers and synchronization fences.
 * Returns NULL on host or if queue creation fails.
 */
oops_gpu_queue_t *oops_gpu_create_graphics_queue(void);

/*
 * Destroy a queue and free its command buffer and fence. Dispatches are synchronous,
 * so nothing is pending. Safe on NULL.
 */
void oops_gpu_destroy_queue(oops_gpu_queue_t *queue);

/*
 * Instantiate an RDNA2 GFX10.3 compute shader from its container header and
 * bytecode payload via sceAgcCreateShader.
 */
oops_gpu_shader_t *oops_gpu_create_shader(const void *container_hdr, size_t hdr_size,
                                          const void *payload, size_t payload_size);

/*
 * Destroy and release memory associated with a compute shader object.
 */
void oops_gpu_destroy_shader(oops_gpu_shader_t *shader);

/*
 * Execute a compute dispatch on the specified hardware queue:
 * - Emits SET_SH_REG packets for the shader header's register table, with the
 *   program address (COMPUTE_PGM_LO/HI) patched in.
 * - Emits SET_SH_REG packets for user-data SGPRs (COMPUTE_USER_DATA_0..N).
 * - Emits DISPATCH_DIRECT with grid_x, grid_y, grid_z.
 * - Emits RELEASE_MEM with cache writeback and an end-of-pipe fence write.
 * - Submits the DCB to libSceAgcDriver and waits for the fence.
 * Returns 0 on success, -2 on a fence timeout, another negative code on error.
 */
int oops_gpu_dispatch(oops_gpu_queue_t *queue, const oops_gpu_dispatch_t *dispatch);

/*
 * Query the latest retired fence value from the queue.
 */
uint32_t oops_gpu_get_last_fence(const oops_gpu_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_GPU_H */
