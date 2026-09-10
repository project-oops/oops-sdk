#include "agc/display.h"
#include "agc/shader_tiler.h"
#include "agc/tiler.h"
#include "oops/agc.h"
#include "oops/memory.h"
#include <stddef.h>
#include <stdint.h>

/* Prospero-generation Kernel / VideoOut stubs */
typedef int64_t sce_off_t;

/* Exactly 32 bytes matching Prospero-generation kernel batch-map descriptor */
struct obs_batch_map_entry {
  void *vaddr;
  sce_off_t paddr;
  size_t len;
  uint8_t prot;
  uint8_t pad[3];
  uint32_t flags;
};

/* Exactly 32 bytes matching Prospero-generation kernel video buffer descriptor
 */
struct SceVideoOutBuffer {
  void *data;
  void *metadata;
  void *reserved[2];
};

/* Imported platform symbols (weak so compilation succeeds in any environment)
 */
__attribute__((weak)) int sceUserServiceInitialize(const void *param);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceVideoOutOpen(int userId, int type, int index,
                                          const void *param);
__attribute__((weak)) int sceVideoOutClose(int handle);
__attribute__((weak)) void
sceVideoOutSetBufferAttribute2(void *attr, uint64_t pixelformat,
                               uint32_t tiling_mode, uint32_t width,
                               uint32_t height, uint64_t option,
                               uint32_t dcc_control, uint64_t dcc_clear_color);
__attribute__((weak)) int
sceVideoOutRegisterBuffers2(int handle, int startIndex, int unk,
                            const struct SceVideoOutBuffer *buffers,
                            int bufferCount, const void *attribute,
                            int category, void *reserved);
__attribute__((weak)) int sceVideoOutSubmitFlip(int handle, int index,
                                                unsigned int flipMode,
                                                int64_t flipArg);

/* Flip status descriptor. The base fields occupy the first 64 bytes (confirmed
 * on 12.40). On native Prospero libSceVideoOut writes between 96 and 128+
 * bytes. Sized to 256 bytes with trailing padding to prevent
 * sceVideoOutGetFlipStatus from smashing the caller's stack frame. */
struct agc_flip_status {
  uint64_t count;
  uint64_t process_time;
  uint64_t submit_time;
  int64_t flip_arg;
  uint64_t reserved[2];
  int32_t num_gpu_flip_pending;
  int32_t num_flip_pending;
  int32_t current_buffer;
  uint32_t reserved1;
  uint8_t reserved2[192];
};
_Static_assert(sizeof(struct agc_flip_status) == 256,
               "flip status buffer must be 256 bytes");
__attribute__((weak)) int
sceVideoOutGetFlipStatus(int handle, struct agc_flip_status *status);
__attribute__((weak)) int sceKernelAllocateMainDirectMemory(size_t len,
                                                            size_t alignment,
                                                            int memoryType,
                                                            sce_off_t *paddr);
__attribute__((weak)) int
sceKernelAllocateDirectMemory(sce_off_t searchStart, sce_off_t searchEnd,
                              size_t len, size_t alignment, int memoryType,
                              sce_off_t *paddr);
/* size_t, as gnm and memory declare it: the pool is larger than 2 GB and an int
 * truncates it. */
__attribute__((weak)) size_t sceKernelGetDirectMemorySize(void);
__attribute__((weak)) int sceKernelReleaseDirectMemory(sce_off_t paddr,
                                                       size_t len);
__attribute__((weak)) int sceKernelBatchMap(struct obs_batch_map_entry *entries,
                                            int num_entries, int *completed);
__attribute__((weak)) int sceKernelMunmap(void *addr, size_t len);
__attribute__((weak)) int sceKernelOpen(const char *path, int flags, int mode);
__attribute__((weak)) int sceKernelUsleep(unsigned int microseconds);

#define AGC_STRIDE_BYTES 0xa00000u /* 10 MB per tiled buffer (2MB-aligned) */
#define AGC_TOTAL_ALLOC_BYTES 0x2000000u /* 32 MB (16 x 2MB pages) */
#define AGC_VM_BASE 0x4000000000ULL

struct agc_display {
  int handle;
  unsigned int width;
  unsigned int height;
  sce_off_t physical;
  void *mapped_base;
  uint32_t *target_gpu_fb[2];
  uint32_t *linear_scratch_fb;
  unsigned int fb_index;
  uint64_t flip_count;
  int ready;
  int last_error;
  void *agc_queue;
  void *shader_obj;
  void *shader_payload;
  uint8_t *dcb_mem;
  volatile uint32_t *fence;
  int gpu_accelerated;
};

static struct agc_display s_agc_display;
static agc_log_fn s_logger = 0;

void agc_display_set_logger(agc_log_fn fn) { s_logger = fn; }

static void agc_log(const char *tag, const char *msg, uint64_t val) {
  if (s_logger) {
    s_logger(tag, msg, val);
  }
}

agc_display_t *agc_display_open(unsigned int width, unsigned int height) {
  struct agc_display *disp = &s_agc_display;
  for (size_t i = 0; i < sizeof(*disp); i++) {
    ((unsigned char *)disp)[i] = 0;
  }
  disp->handle = -1;
  disp->width = width;
  disp->height = height;

  /* The mapping is fixed at 32 MB: two 10 MB tiled buffers and a linear scratch
   * in the remaining 12 MB. A surface that does not fit would be tiled straight
   * over its neighbour, so refuse it here, before the hardware is opened. */
  if (width == 0 || height == 0 ||
      agc_tile_surface_bytes(width, height) > AGC_STRIDE_BYTES ||
      (uint64_t)width * (uint64_t)height * 4u >
          AGC_TOTAL_ALLOC_BYTES - 2u * AGC_STRIDE_BYTES) {
    disp->last_error = -3;
    return disp;
  }

  /* The swizzle table, built once here rather than by whichever tiling call
   * comes first. */
  agc_tile_init();

  if (!sceVideoOutOpen || !sceVideoOutRegisterBuffers2) {
    disp->last_error = -1;
    return disp;
  }

  /* Probe /dev/dce (Display Controller Engine) */
  if (sceKernelOpen) {
    int dce = sceKernelOpen("/dev/dce", 2, 0);
    agc_log("agc-dce", "/dev/dce fd", (uint64_t)(uint32_t)dce);
  }

  /* 1. Open VideoOut handle */
  int vh1 = sceVideoOutOpen(0xFF, 0, 0, 0);
  agc_log("agc-vo-ff", "open(0xFF, 0, 0, 0)", (uint64_t)(uint32_t)vh1);

  int vh = vh1;
  if (vh <= 0) {
    vh = sceVideoOutOpen(0x100, 0, 0, 0);
    agc_log("agc-vo-100", "open(0x100, 0, 0, 0)", (uint64_t)(uint32_t)vh);
  }
  if (vh <= 0 && sceUserServiceGetInitialUser) {
    int32_t user = 0;
    if (sceUserServiceGetInitialUser(&user) != 0 && sceUserServiceInitialize) {
      sceUserServiceInitialize(0);
      (void)sceUserServiceGetInitialUser(&user);
    }
    if (user > 0) {
      vh = sceVideoOutOpen(user, 0, 0, 0);
      agc_log("agc-vo-user", "open(user, 0, 0, 0)", (uint64_t)(uint32_t)vh);
    }
  }
  if (vh <= 0) {
    disp->last_error = (int)(0xE1000000u | (uint32_t)(vh & 0xFFFFFF));
    return disp;
  }
  disp->handle = vh;

  /* 2. Allocate 32 MB direct Garlic WC memory */
  sce_off_t physical = 0;
  int arc = -1;
  if (sceKernelAllocateMainDirectMemory) {
    arc = sceKernelAllocateMainDirectMemory(
        AGC_TOTAL_ALLOC_BYTES, 0x200000u,
        3 /* write-combined GPU memory: 3, confirmed (obSCEne
             130-layout/memory-type, 12.40) */
        ,
        &physical);
    agc_log("agc-alloc-main", "AllocateMainDirectMemory",
            (uint64_t)(uint32_t)arc);
  } else if (sceKernelAllocateDirectMemory && sceKernelGetDirectMemorySize) {
    arc = sceKernelAllocateDirectMemory(
        0, (sce_off_t)sceKernelGetDirectMemorySize(), AGC_TOTAL_ALLOC_BYTES,
        0x200000u, 3, &physical);
    agc_log("agc-alloc-dir", "AllocateDirectMemory", (uint64_t)(uint32_t)arc);
  }
  if (arc != 0) {
    disp->last_error = (int)(0xE2000000u | (uint32_t)(arc & 0xFFFFFF));
    (void)sceVideoOutClose(disp->handle);
    disp->handle = -1;
    return disp;
  }
  disp->physical = physical;
  agc_log("agc-phys", "physical addr", (uint64_t)physical);

  /* 3. Batch-map 16 pages of 2MB into 0x4000000000ULL with GPU protection 0x33
   */
  if (!sceKernelBatchMap) {
    disp->last_error = -2;
    (void)sceKernelReleaseDirectMemory(physical, AGC_TOTAL_ALLOC_BYTES);
    (void)sceVideoOutClose(disp->handle);
    disp->handle = -1;
    return disp;
  }

  struct obs_batch_map_entry b_entries[16];
  for (size_t i = 0; i < sizeof(b_entries); i++) {
    ((unsigned char *)b_entries)[i] = 0;
  }
  for (size_t p = 0; p < 16; p++) {
    b_entries[p].vaddr = (void *)((uintptr_t)AGC_VM_BASE + p * 0x200000u);
    b_entries[p].paddr = (sce_off_t)((size_t)physical + p * 0x200000u);
    b_entries[p].len = 0x200000u;
    b_entries[p].prot = 0x33; /* PROT_CPU_RW | PROT_GPU_RW */
    b_entries[p].flags = 0;
  }

  int completed = 0;
  int brc = sceKernelBatchMap(b_entries, 16, &completed);
  agc_log("agc-bmap-rc", "BatchMap rc", (uint64_t)(uint32_t)brc);
  agc_log("agc-bmap-comp", "BatchMap completed", (uint64_t)(uint32_t)completed);
  if (brc != 0) {
    disp->last_error = (int)(0xE3000000u | (uint32_t)(brc & 0xFFFFFF));
    (void)sceKernelReleaseDirectMemory(physical, AGC_TOTAL_ALLOC_BYTES);
    (void)sceVideoOutClose(disp->handle);
    disp->handle = -1;
    return disp;
  }
  disp->mapped_base = (void *)(uintptr_t)AGC_VM_BASE;

  /* Setup buffer addresses:
   * Buffer 0 at 0MB
   * Buffer 1 at 10MB
   * Linear scratch buffer at 20MB
   */
  disp->target_gpu_fb[0] = (uint32_t *)disp->mapped_base;
  disp->target_gpu_fb[1] =
      (uint32_t *)((unsigned char *)disp->mapped_base + AGC_STRIDE_BYTES);
  disp->linear_scratch_fb =
      (uint32_t *)((unsigned char *)disp->mapped_base + 2 * AGC_STRIDE_BYTES);

  agc_log("agc-b0-addr", "buffer 0 addr",
          (uint64_t)(uintptr_t)disp->target_gpu_fb[0]);
  agc_log("agc-b1-addr", "buffer 1 addr",
          (uint64_t)(uintptr_t)disp->target_gpu_fb[1]);
  agc_log("agc-lin-addr", "scratch addr",
          (uint64_t)(uintptr_t)disp->linear_scratch_fb);

  /* 4. Configure buffer attribute: a 256-byte attribute block, 64-bit SDR
   * format, the requested width x height, pitch 0. The call writes the first 80
   * bytes of the block: tiling mode at 4, width at 12, height at 16, the format
   * word at 32 (obSCEne 080-video/attribute-block on 12.40). */
  unsigned char attr[256];
  for (size_t i = 0; i < sizeof(attr); i++)
    attr[i] = 0;
  if (sceVideoOutSetBufferAttribute2) {
    sceVideoOutSetBufferAttribute2(
        attr, 0x8000000000000000ULL,
        0 /* tiled: written by agc_tile_surface(), not linear */, width, height,
        0, 0, 0);
  }
  agc_log("agc-attr-0", "attr word 0", *(const uint64_t *)(attr + 0));
  agc_log("agc-attr-8", "attr word 1", *(const uint64_t *)(attr + 8));

  /* 5. Register buffers */
  struct SceVideoOutBuffer buffers[2];
  for (size_t i = 0; i < sizeof(buffers); i++) {
    ((unsigned char *)buffers)[i] = 0;
  }
  buffers[0].data = disp->target_gpu_fb[0];
  buffers[1].data = disp->target_gpu_fb[1];

  int rrc =
      sceVideoOutRegisterBuffers2(disp->handle, 0, 0, buffers, 2, attr, 0, 0);
  agc_log("agc-reg-rc", "RegisterBuffers2 rc", (uint64_t)(uint32_t)rrc);
  if (rrc != 0) {
    disp->last_error = (int)(0xE4000000u | (uint32_t)(rrc & 0xFFFFFF));
    (void)sceKernelMunmap(disp->mapped_base, AGC_TOTAL_ALLOC_BYTES);
    (void)sceKernelReleaseDirectMemory(physical, AGC_TOTAL_ALLOC_BYTES);
    (void)sceVideoOutClose(disp->handle);
    disp->handle = -1;
    return disp;
  }

  /* Clear and tile initial buffers */
  disp->ready = 1;
  disp->fb_index = 0;
  disp->last_error = 0;
  disp->flip_count = 0;
  agc_display_clear(disp, 0);
  agc_tile_surface(disp->target_gpu_fb[0], disp->linear_scratch_fb, width,
                   height);
  agc_tile_surface(disp->target_gpu_fb[1], disp->linear_scratch_fb, width,
                   height);

  /* Attempt hardware GPU acceleration setup via libSceAgc / libSceAgcDriver */
  disp->gpu_accelerated = 0;
  disp->agc_queue = (void *)0;
  disp->shader_obj = (void *)0;
  disp->shader_payload = (void *)0;
  disp->dcb_mem = (uint8_t *)0;
  disp->fence = (volatile uint32_t *)0;

  if (sceAgcInit && sceAgcDriverCreateQueue && sceAgcDriverSubmitDcb &&
      sceAgcCreateShader) {
    uint64_t agc_state[8];
    for (size_t i = 0; i < 8; i++)
      agc_state[i] = 0;
    int arc_init = sceAgcInit((void *)agc_state, 0xd);
    agc_log("agc-init-rc", "sceAgcInit rc", (uint64_t)(uint32_t)arc_init);

    void *queue = (void *)0;
    int qrc = sceAgcDriverCreateQueue(3u, &queue, 0u);
    agc_log("agc-queue-rc", "CreateQueue rc", (uint64_t)(uint32_t)qrc);

    if (qrc == 0 && queue != (void *)0) {
      disp->agc_queue = queue;

      /* Allocate coherent Onion memory for DCB buffer (64 KB), fence (64 B),
       * and shader payload (64 KB) */
      uint8_t *dcb =
          (uint8_t *)oops_mem_alloc(0x10000, 0x10000, OOPS_MEM_WB_ONION);
      volatile uint32_t *f = (volatile uint32_t *)oops_mem_alloc(
          0x10000, 0x10000, OOPS_MEM_WB_ONION);
      uint8_t *sp = (uint8_t *)oops_mem_alloc(0x10000, 256, OOPS_MEM_WB_ONION);

      if (dcb && f && sp) {
        disp->dcb_mem = dcb;
        disp->fence = f;
        disp->shader_payload = sp;
        *disp->fence = 0xbeefcafeu;

        for (size_t k = 0; k < sizeof(s_agc_tiler_payload_base); k++) {
          sp[k] = s_agc_tiler_payload_base[k];
        }

        uint8_t hdr_copy[384];
        for (size_t k = 0; k < sizeof(hdr_copy); k++)
          hdr_copy[k] = 0;
        for (size_t k = 0; k < sizeof(s_agc_tiler_hdr_full); k++) {
          hdr_copy[k] = s_agc_tiler_hdr_full[k];
        }

        void *s_obj = (void *)0;
        int src = sceAgcCreateShader(&s_obj, hdr_copy, sp, 0);
        agc_log("agc-shader-rc", "CreateShader rc", (uint64_t)(uint32_t)src);

        if (src == 0 && s_obj != (void *)0) {
          disp->shader_obj = s_obj;
          disp->gpu_accelerated = 1;
          agc_log("agc-gpu-accel", "GPU acceleration enabled", 1);
        } else {
          oops_mem_free((void *)dcb);
          oops_mem_free((void *)f);
          oops_mem_free((void *)sp);
          disp->dcb_mem = (uint8_t *)0;
          disp->fence = (volatile uint32_t *)0;
          disp->shader_payload = (void *)0;
          if (disp->agc_queue && sceAgcDriverDestroyQueue) {
            sceAgcDriverDestroyQueue(disp->agc_queue);
            disp->agc_queue = (void *)0;
          }
        }
      } else {
        if (dcb)
          oops_mem_free((void *)dcb);
        if (f)
          oops_mem_free((void *)f);
        if (sp)
          oops_mem_free((void *)sp);
        if (disp->agc_queue && sceAgcDriverDestroyQueue) {
          sceAgcDriverDestroyQueue(disp->agc_queue);
          disp->agc_queue = (void *)0;
        }
      }
    }
  }

  return disp;
}

int agc_display_is_ready(const agc_display_t *disp) {
  return disp && disp->ready;
}

uint32_t *agc_display_get_framebuffer(agc_display_t *disp) {
  if (!disp || !disp->ready)
    return 0;
  return disp->linear_scratch_fb;
}

unsigned int agc_display_get_width(const agc_display_t *disp) {
  return disp ? disp->width : 0;
}

unsigned int agc_display_get_height(const agc_display_t *disp) {
  return disp ? disp->height : 0;
}

/* Poll the flip status until nothing is pending, or give up after about 100 ms.
 * Both entry points are linked in the eboot context and present in the app
 * context on 12.40; where either is missing there is nothing to wait on and the
 * caller proceeds as it always did. */
static void agc_wait_for_flips(struct agc_display *disp) {
  if (disp->handle <= 0 || !sceVideoOutGetFlipStatus || !sceKernelUsleep)
    return;
  for (int i = 0; i < 100; i++) {
    struct agc_flip_status status;
    for (size_t k = 0; k < sizeof(status); k++)
      ((unsigned char *)&status)[k] = 0;
    if (sceVideoOutGetFlipStatus(disp->handle, &status) != 0)
      return;
    if (status.num_flip_pending <= 0)
      return;
    sceKernelUsleep(1000);
  }
}

int agc_display_flip(agc_display_t *disp) {
  if (!disp || !disp->ready)
    return -1;

  unsigned int shown = disp->fb_index;
  disp->fb_index = (disp->fb_index + 1) % 2;

  /* The buffer about to be written was submitted two flips ago. On prospero
   * (measured; the agc path also serves trinity) the submit call queues and
   * returns in microseconds - eight back-to-back all returned 0 in 3 to 12 us,
   * leaving pending flips that present over the next frames (obSCEne
   * 080-video/visual-flip; a later 32-submit burst put the hardware queue
   * capacity at exactly 26, submit 27 refused with QUEUE_FULL 0x80290012) - so
   * without a wait the tiler could write into a buffer that is still queued or
   * on screen. Drain the queue first: with two buffers, pending == 0 means the
   * other one is on screen and this one is free. */
  if (disp->flip_count > 0) {
    agc_wait_for_flips(disp);
  }

  /* Tiling step: hardware GPU dispatch when accelerated, CPU fallback otherwise
   */
  if (disp->gpu_accelerated && disp->dcb_mem && disp->fence &&
      disp->agc_queue && sceAgcDriverSubmitDcb) {
    uint64_t src_gpu =
        AGC_VM_BASE + 2u * AGC_STRIDE_BYTES; /* linear scratch FB */
    uint64_t dst_gpu =
        AGC_VM_BASE +
        (uint64_t)shown * AGC_STRIDE_BYTES; /* targeted display buffer */
    uint32_t *dw = (uint32_t *)disp->dcb_mem;

    /* Extract and patch register table from shader header (offset +0x88) */
    const uint32_t *reg_table = (const uint32_t *)(s_agc_tiler_hdr_full + 0x88);
    uint64_t payload_va = (uint64_t)(uintptr_t)disp->shader_payload;

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

    /* User data: source linear FB and dest tiled FB */
    *dw++ = 0xc0017600u; /* SET_SH_REG */
    *dw++ = 0x240u;      /* COMPUTE_USER_DATA_0 */
    *dw++ = (uint32_t)src_gpu;
    *dw++ = 0xc0017600u;
    *dw++ = 0x241u; /* COMPUTE_USER_DATA_1 */
    *dw++ = (uint32_t)(src_gpu >> 32);

    *dw++ = 0xc0017600u;
    *dw++ = 0x242u; /* COMPUTE_USER_DATA_2 */
    *dw++ = (uint32_t)dst_gpu;
    *dw++ = 0xc0017600u;
    *dw++ = 0x243u; /* COMPUTE_USER_DATA_3 */
    *dw++ = (uint32_t)(dst_gpu >> 32);

    *dw++ = 0xc0017600u;
    *dw++ = 0x244u; /* COMPUTE_USER_DATA_4: width | (height << 16) */
    *dw++ = (uint32_t)(disp->width & 0xffffu) |
            ((uint32_t)(disp->height & 0xffffu) << 16);

    /* DISPATCH_DIRECT: threadgroups covering width x height in 128x128
     * macro-tiles */
    uint32_t tg_x = (disp->width + 127u) >> 7;
    uint32_t tg_y = (disp->height + 127u) >> 7;
    *dw++ = 0xc0031500u; /* DISPATCH_DIRECT */
    *dw++ = (tg_x > 0) ? tg_x : 1u;
    *dw++ = (tg_y > 0) ? tg_y : 1u;
    *dw++ = 1u;    /* dim_z */
    *dw++ = 0x41u; /* initiator */

    /* RELEASE_MEM: EOP event write to completion fence in coherent Onion memory
     */
    uint64_t fence_gpu = (uint64_t)(uintptr_t)disp->fence;
    *disp->fence = 0x11111111u;

    *dw++ = 0xc0064900u; /* PACKET3_RELEASE_MEM, count 6 */
    *dw++ =
        0x06603514u;     /* GCR_SEQ | GCR_GL2_WB | GCR_GLM_INV | GCR_GLM_WB |
                            CACHE_POLICY(3) | EVENT_TYPE(0x14) | EVENT_INDEX(5) */
    *dw++ = 0x20000000u; /* DATA_SEL(1) = 32-bit int low */
    *dw++ = (uint32_t)fence_gpu;
    *dw++ = (uint32_t)(fence_gpu >> 32);
    *dw++ = 0xbeefcafeu; /* fence value */
    *dw++ = 0u;
    *dw++ = 0u;

    /* Trailing PM4 NOPs for prefetch safety */
    for (int p = 0; p < 16; p++) {
      dw[p] = 0xffff1000u;
    }
    dw += 16;

    uint32_t dcb_dwords = (uint32_t)(dw - (uint32_t *)disp->dcb_mem);
    oops_agc_dcb_desc desc;
    desc.gpu_addr = (uint64_t)(uintptr_t)disp->dcb_mem;
    desc.size = dcb_dwords; /* STRICTLY in DWORDs, per hardware requirement */
    desc.flags = 0;
    desc.pad = 0;

    int submit_rc = sceAgcDriverSubmitDcb(&desc);
    agc_log("agc-dcb-rc", "SubmitDcb rc", (uint64_t)(uint32_t)submit_rc);

    if (submit_rc == 0) {
      for (int poll = 0; poll < 10000; poll++) {
#if defined(__x86_64__)
        __builtin_ia32_clflush((const void *)disp->fence);
#endif
        if (*disp->fence == 0xbeefcafeu) {
          break;
        }
        if (sceKernelUsleep) {
          sceKernelUsleep(10);
        }
      }
    } else {
      /* Fallback to CPU tiling if submission fails */
      agc_tile_surface(disp->target_gpu_fb[shown], disp->linear_scratch_fb,
                       disp->width, disp->height);
    }
  } else {
    /* CPU software tiling fallback path */
    agc_tile_surface(disp->target_gpu_fb[shown], disp->linear_scratch_fb,
                     disp->width, disp->height);
  }

  disp->flip_count++;

  if (sceVideoOutSubmitFlip) {
    int frc = sceVideoOutSubmitFlip(disp->handle, (int)shown, 1, 0);
    agc_log("agc-flip-rc", "SubmitFlip rc", (uint64_t)(uint32_t)frc);
    return frc;
  }
  return 0;
}

void agc_display_clear(agc_display_t *disp, uint32_t color) {
  if (!disp || !disp->ready || !disp->linear_scratch_fb)
    return;
  size_t count = (size_t)disp->width * (size_t)disp->height;
  for (size_t i = 0; i < count; i++) {
    disp->linear_scratch_fb[i] = color;
  }
}

uint64_t agc_display_get_flip_count(const agc_display_t *disp) {
  if (!disp)
    return 0;
  /* The hardware's count of completed flips where the status call resolves; the
   * count of submits where it does not. The two differ by however many flips
   * are still pending. */
  if (disp->handle > 0 && sceVideoOutGetFlipStatus) {
    struct agc_flip_status status;
    for (size_t i = 0; i < sizeof(status); i++)
      ((unsigned char *)&status)[i] = 0;
    if (sceVideoOutGetFlipStatus(disp->handle, &status) == 0) {
      return status.count;
    }
  }
  return disp->flip_count;
}

int agc_display_get_last_error(const agc_display_t *disp) {
  return disp ? disp->last_error : -1;
}

int agc_display_get_video_handle(const agc_display_t *disp) {
  return disp ? disp->handle : -1;
}

void agc_display_close(agc_display_t *disp) {
  if (!disp)
    return;

  if (disp->gpu_accelerated) {
    if (disp->agc_queue && sceAgcDriverDestroyQueue) {
      sceAgcDriverDestroyQueue(disp->agc_queue);
      disp->agc_queue = (void *)0;
    }
    if (disp->dcb_mem) {
      oops_mem_free((void *)disp->dcb_mem);
      disp->dcb_mem = (uint8_t *)0;
    }
    if (disp->fence) {
      oops_mem_free((void *)disp->fence);
      disp->fence = (volatile uint32_t *)0;
    }
    if (disp->shader_payload) {
      oops_mem_free(disp->shader_payload);
      disp->shader_payload = (void *)0;
    }
    disp->shader_obj = (void *)0;
    disp->gpu_accelerated = 0;
  }

  if (disp->mapped_base) {
    (void)sceKernelMunmap(disp->mapped_base, AGC_TOTAL_ALLOC_BYTES);
    disp->mapped_base = 0;
  }
  if (disp->physical) {
    (void)sceKernelReleaseDirectMemory(disp->physical, AGC_TOTAL_ALLOC_BYTES);
    disp->physical = 0;
  }
  if (disp->handle > 0) {
    (void)sceVideoOutClose(disp->handle);
    disp->handle = -1;
  }
  disp->ready = 0;
}

int agc_display_is_gpu_accelerated(const agc_display_t *disp) {
  return (disp && disp->ready && disp->gpu_accelerated);
}
