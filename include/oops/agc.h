#ifndef OOPS_AGC_H
#define OOPS_AGC_H

#include "agc/display.h"
#include "agc/driver.h"
#include "agc/tiler.h"
#include "oops/gpu.h"

/*
 * High-level AGC compute dispatch helper. Wraps oops_gpu_dispatch() with
 * explicit grid dimensions and user data registers.
 */
static inline int oops_agc_dispatch_compute(oops_gpu_queue_t *queue,
                                            const oops_gpu_shader_t *shader,
                                            uint32_t grid_x, uint32_t grid_y,
                                            uint32_t grid_z,
                                            const uint32_t *user_data,
                                            uint32_t user_data_count) {
  oops_gpu_dispatch_t d;
  d.shader = shader;
  d.user_data = user_data;
  d.user_data_count = user_data_count;
  d.grid_x = grid_x;
  d.grid_y = grid_y;
  d.grid_z = grid_z;
  return oops_gpu_dispatch(queue, &d);
}

#endif /* OOPS_AGC_H */
