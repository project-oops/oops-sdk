#include "oops/agc.h"
#include "oops/gpu.h"
#include "oops/memory.h"
#include "oops/target.h"

#if OOPS_TARGET_IS_PROSPERO

__attribute__((weak)) int sceKernelUsleep(unsigned int microseconds);

struct oops_gpu_queue {
  void *agc_queue_handle;
  uint8_t *dcb_mem;
  size_t dcb_size;
  volatile uint32_t *fence;
  uint32_t last_fence;
};

struct oops_gpu_shader {
  void *shader_obj;
  void *payload_mem;
  size_t payload_size;
  uint8_t hdr[384];
  size_t hdr_size;
};

static int s_agc_inited = 0;
static uint8_t s_agc_state[64];

static int init_agc_subsystem(void) {
  if (s_agc_inited)
    return 0;
  if (!sceAgcInit)
    return -1;
  for (size_t i = 0; i < sizeof(s_agc_state); i++) {
    s_agc_state[i] = 0;
  }
  int rc = sceAgcInit(s_agc_state, 0xd);
  if (rc == 0) {
    s_agc_inited = 1;
    return 0;
  }
  return rc;
}

int oops_gpu_available(void) {
  if (!sceAgcInit || !sceAgcDriverCreateQueue || !sceAgcDriverSubmitDcb ||
      !sceAgcCreateShader) {
    return 0;
  }
  return (init_agc_subsystem() == 0);
}

oops_gpu_queue_t *oops_gpu_create_compute_queue(void) {
  if (!oops_gpu_available())
    return (oops_gpu_queue_t *)0;

  oops_gpu_queue_t *q = (oops_gpu_queue_t *)oops_mem_alloc(
      sizeof(oops_gpu_queue_t), 16, OOPS_MEM_WB_ONION);
  if (!q)
    return (oops_gpu_queue_t *)0;

  void *queue_handle = (void *)0;
  int qrc = sceAgcDriverCreateQueue(3, &queue_handle, 0);
  if (qrc != 0 || !queue_handle) {
    oops_mem_free(q);
    return (oops_gpu_queue_t *)0;
  }
  q->agc_queue_handle = queue_handle;

  /* Allocate 64KB coherent Onion WB memory for command buffer stream */
  q->dcb_size = 0x10000u;
  q->dcb_mem =
      (uint8_t *)oops_mem_alloc(q->dcb_size, 0x1000, OOPS_MEM_WB_ONION);

  /* Allocate coherent Onion WB memory for completion fence */
  q->fence =
      (volatile uint32_t *)oops_mem_alloc(0x1000, 0x1000, OOPS_MEM_WB_ONION);

  if (!q->dcb_mem || !q->fence) {
    if (q->dcb_mem)
      oops_mem_free(q->dcb_mem);
    if (q->fence)
      oops_mem_free((void *)q->fence);
    if (sceAgcDriverDestroyQueue)
      sceAgcDriverDestroyQueue(q->agc_queue_handle);
    oops_mem_free(q);
    return (oops_gpu_queue_t *)0;
  }

  *q->fence = 0;
  q->last_fence = 0;
  return q;
}

void oops_gpu_destroy_queue(oops_gpu_queue_t *queue) {
  if (!queue)
    return;
  if (queue->agc_queue_handle && sceAgcDriverDestroyQueue) {
    sceAgcDriverDestroyQueue(queue->agc_queue_handle);
    queue->agc_queue_handle = (void *)0;
  }
  if (queue->dcb_mem) {
    oops_mem_free(queue->dcb_mem);
    queue->dcb_mem = (uint8_t *)0;
  }
  if (queue->fence) {
    oops_mem_free((void *)queue->fence);
    queue->fence = (volatile uint32_t *)0;
  }
  oops_mem_free(queue);
}

oops_gpu_shader_t *oops_gpu_create_shader(const void *container_hdr,
                                          size_t hdr_size, const void *payload,
                                          size_t payload_size) {
  if (!container_hdr || !payload || hdr_size < 0x100 || payload_size == 0) {
    return (oops_gpu_shader_t *)0;
  }
  if (!oops_gpu_available())
    return (oops_gpu_shader_t *)0;

  oops_gpu_shader_t *sh = (oops_gpu_shader_t *)oops_mem_alloc(
      sizeof(oops_gpu_shader_t), 16, OOPS_MEM_WB_ONION);
  if (!sh)
    return (oops_gpu_shader_t *)0;

  /* Payload must reside in 256-byte-aligned GPU-accessible memory */
  sh->payload_size = payload_size;
  sh->payload_mem = oops_mem_alloc(payload_size, 256, OOPS_MEM_WB_ONION);
  if (!sh->payload_mem) {
    oops_mem_free(sh);
    return (oops_gpu_shader_t *)0;
  }

  uint8_t *dst = (uint8_t *)sh->payload_mem;
  const uint8_t *src = (const uint8_t *)payload;
  for (size_t i = 0; i < payload_size; i++) {
    dst[i] = src[i];
  }

  /* Cache header */
  sh->hdr_size = (hdr_size > sizeof(sh->hdr)) ? sizeof(sh->hdr) : hdr_size;
  for (size_t i = 0; i < sh->hdr_size; i++) {
    sh->hdr[i] = ((const uint8_t *)container_hdr)[i];
  }

  void *obj_out = (void *)0;
  int src_rc = sceAgcCreateShader(&obj_out, sh->hdr, sh->payload_mem, 0);
  if (src_rc != 0 || !obj_out) {
    oops_mem_free(sh->payload_mem);
    oops_mem_free(sh);
    return (oops_gpu_shader_t *)0;
  }
  sh->shader_obj = obj_out;
  return sh;
}

void oops_gpu_destroy_shader(oops_gpu_shader_t *shader) {
  if (!shader)
    return;
  if (shader->payload_mem) {
    oops_mem_free(shader->payload_mem);
    shader->payload_mem = (void *)0;
  }
  oops_mem_free(shader);
}

int oops_gpu_dispatch(oops_gpu_queue_t *queue,
                      const oops_gpu_dispatch_t *dispatch) {
  if (!queue || !dispatch || !dispatch->shader || !sceAgcDriverSubmitDcb) {
    return -1;
  }
  if (!queue->dcb_mem || !queue->fence)
    return -1;
  if (dispatch->user_data_count > 16)
    return -1;

  uint32_t *dw = (uint32_t *)queue->dcb_mem;
  const oops_gpu_shader_t *sh = dispatch->shader;
  uint64_t payload_va = (uint64_t)(uintptr_t)sh->payload_mem;

  /* Extract and patch register table from shader header (offset +0x88) */
  const uint32_t *reg_table = (const uint32_t *)(sh->hdr + 0x88);
  for (int i = 0; i < 11; i++) {
    uint32_t reg_idx = reg_table[i * 2];
    uint32_t reg_val = reg_table[i * 2 + 1];
    if (reg_idx == 0x20c) { /* COMPUTE_PGM_LO */
      reg_val = (uint32_t)(payload_va >> 8);
    } else if (reg_idx == 0x20d) { /* COMPUTE_PGM_HI */
      reg_val = (uint32_t)(payload_va >> 40);
    }
    *dw++ = 0xc0017600u; /* SET_SH_REG, count 1 */
    *dw++ = reg_idx;
    *dw++ = reg_val;
  }

  /* Bind user data registers (COMPUTE_USER_DATA_0..N at 0x240) */
  for (uint32_t u = 0; u < dispatch->user_data_count; u++) {
    *dw++ = 0xc0017600u; /* SET_SH_REG */
    *dw++ = 0x240u + u;
    *dw++ = dispatch->user_data[u];
  }

  /* Emit DISPATCH_DIRECT */
  uint32_t gx = (dispatch->grid_x > 0) ? dispatch->grid_x : 1u;
  uint32_t gy = (dispatch->grid_y > 0) ? dispatch->grid_y : 1u;
  uint32_t gz = (dispatch->grid_z > 0) ? dispatch->grid_z : 1u;

  *dw++ = 0xc0031500u; /* DISPATCH_DIRECT */
  *dw++ = gx;
  *dw++ = gy;
  *dw++ = gz;
  *dw++ = 0x41u; /* initiator */

  /* RELEASE_MEM: EOP event write to completion fence in coherent Onion memory
   */
  uint64_t fence_gpu = (uint64_t)(uintptr_t)queue->fence;
  uint32_t next_fence = queue->last_fence + 1u;
  if (next_fence == 0)
    next_fence = 1u;
  *queue->fence = 0x0u;

  *dw++ = 0xc0064900u; /* PACKET3_RELEASE_MEM, count 6 */
  *dw++ = 0x06603514u; /* GCR_SEQ | GCR_GL2_WB | GCR_GLM_INV | GCR_GLM_WB |
                          CACHE_POLICY(3) | EVENT_TYPE(0x14) | EVENT_INDEX(5) */
  *dw++ = 0x20000000u; /* DATA_SEL(1) = 32-bit int low */
  *dw++ = (uint32_t)fence_gpu;
  *dw++ = (uint32_t)(fence_gpu >> 32);
  *dw++ = next_fence; /* fence value */
  *dw++ = 0u;
  *dw++ = 0u;

  /* Trailing PM4 NOPs for prefetch safety */
  for (int p = 0; p < 16; p++) {
    dw[p] = 0xffff1000u;
  }
  dw += 16;

  uint32_t dcb_dwords = (uint32_t)(dw - (uint32_t *)queue->dcb_mem);
  oops_agc_dcb_desc desc;
  desc.gpu_addr = (uint64_t)(uintptr_t)queue->dcb_mem;
  desc.size = dcb_dwords; /* STRICTLY in DWORDs */
  desc.flags = 0;
  desc.pad = 0;

  int submit_rc = sceAgcDriverSubmitDcb(&desc);
  if (submit_rc != 0) {
    return submit_rc;
  }

  /* Wait for completion fence retirement */
  int retired = 0;
  for (int poll = 0; poll < 10000; poll++) {
    if (*queue->fence == next_fence) {
      retired = 1;
      break;
    }
    if (sceKernelUsleep) {
      sceKernelUsleep(10);
    }
  }

  if (retired) {
    queue->last_fence = next_fence;
    return 0;
  }
  return -2; /* fence timeout */
}

uint32_t oops_gpu_get_last_fence(const oops_gpu_queue_t *queue) {
  if (!queue)
    return 0;
  return queue->last_fence;
}

#else

/* Host / Non-PS5 Stubs */
int oops_gpu_available(void) { return 0; }
oops_gpu_queue_t *oops_gpu_create_compute_queue(void) {
  return (oops_gpu_queue_t *)0;
}
void oops_gpu_destroy_queue(oops_gpu_queue_t *queue) { (void)queue; }
oops_gpu_shader_t *oops_gpu_create_shader(const void *hdr, size_t hs,
                                          const void *p, size_t ps) {
  (void)hdr;
  (void)hs;
  (void)p;
  (void)ps;
  return (oops_gpu_shader_t *)0;
}
void oops_gpu_destroy_shader(oops_gpu_shader_t *sh) { (void)sh; }
int oops_gpu_dispatch(oops_gpu_queue_t *q, const oops_gpu_dispatch_t *d) {
  (void)q;
  (void)d;
  return -1;
}
uint32_t oops_gpu_get_last_fence(const oops_gpu_queue_t *q) {
  (void)q;
  return 0;
}

#endif
