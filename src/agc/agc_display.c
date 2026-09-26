#include "agc/display.h"
#include "agc/shader_tiler.h"
#include "agc/tiler.h"
#include "oops/agc.h"
#include "oops/gpu.h"
#include "oops/memory.h"
#include "oops/system.h"
#include "oops/time.h"
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
typedef int sce_equeue_t;
__attribute__((weak)) int sceVideoOutSetFlipRate(int handle, int rate);

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

/* How many foreign buffers one display will name to VideoOut alongside its own
 * pair. Two is enough for a renderer that double-buffers its own target, which
 * is the case this exists for; the cap keeps the registration array fixed. */
#define AGC_DISPLAY_MAX_ADOPT 2

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
  int tiling_mode;
  int owns_scratch;
  /* Buffers this display did not allocate, named to VideoOut in the one
   * registration it is allowed (see agc_display_open_adopting). They take the
   * flip indices after target_gpu_fb's pair, so the first is index 2. */
  void *adopted[AGC_DISPLAY_MAX_ADOPT];
  int adopted_count;
  /* The compute-tiler path, populated only by agc_display_try_gpu_tiler(). */
  oops_gpu_queue_t *gpu_queue;
  oops_gpu_shader_t *gpu_shader;
};

static struct agc_display s_agc_display;
static agc_log_fn s_logger = 0;

void agc_display_set_logger(agc_log_fn fn) { s_logger = fn; }

static void agc_log(const char *tag, const char *msg, uint64_t val) {
  if (s_logger) {
    s_logger(tag, msg, val);
  }
  oops_log_debug("AGC", "[%s] %s: 0x%llx (%lld)", tag, msg,
                 (unsigned long long)val, (long long)val);
}

agc_display_t *agc_display_open(unsigned int width, unsigned int height) {
  return agc_display_open_adopting(width, height, 0, 0);
}

agc_display_t *agc_display_open_adopting(unsigned int width,
                                         unsigned int height,
                                         void *const *adopt, int adopt_count) {
  struct agc_display *disp = &s_agc_display;
  for (size_t i = 0; i < sizeof(*disp); i++) {
    ((unsigned char *)disp)[i] = 0;
  }
  /* Prospero libSceVideoOut restricts 720p buffer registration to consoles configured
   * for 720p scanout mode, refusing 1280x720 with 0x80290005 on 1080p/1440p/4K displays.
   * Universal 1080p (1920x1080) is supported across all output modes. If requested
   * at 1280x720, promote to standard 1080p. */
  if (width == 1280 && height == 720) {
    width = 1920;
    height = 1080;
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
   * Linear scratch buffer: allocated in cached Onion memory (type 0) so CPU
   * reads and UI rendering run at L1/L2 cache speeds rather than issuing
   * uncached GDDR6 bus reads across Infinity Fabric. Falls back to direct
   * Garlic memory if Onion allocation fails.
   *
   * **Onion direct memory, through oops_mem_alloc - not the heap.** This came
   * from oops_malloc from 2026-09-19 10:27 until that evening, which is neither
   * of the things the comment above says: a large oops_malloc is an anonymous
   * mmap with no GPU access asked for, returned 24 bytes past its page (the
   * heap's block header). The GPU reads this buffer - the compute tiler does,
   * and oops-gl drew into it and copied it - and it addresses it in 256-byte
   * units. oops_mem_alloc maps direct memory with OOPS_PROT_GPU_RW, 64 KiB
   * aligned, which is what the comment asked for.
   */
  disp->target_gpu_fb[0] = (uint32_t *)disp->mapped_base;
  disp->target_gpu_fb[1] =
      (uint32_t *)((unsigned char *)disp->mapped_base + AGC_STRIDE_BYTES);
  size_t scratch_bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
  void *scratch = oops_mem_alloc(scratch_bytes, 64 * 1024, OOPS_MEM_WB_ONION);
  if (scratch) {
    disp->linear_scratch_fb = (uint32_t *)scratch;
    disp->owns_scratch = 1;
    agc_log("agc-scratch-onion", "scratch allocated in cached Onion direct memory",
            (uint64_t)(uintptr_t)scratch);
  } else {
    disp->linear_scratch_fb =
        (uint32_t *)((unsigned char *)disp->mapped_base + 2 * AGC_STRIDE_BYTES);
    disp->owns_scratch = 0;
    agc_log("agc-scratch-dmem", "scratch allocated in direct memory fallback",
            (uint64_t)(uintptr_t)disp->linear_scratch_fb);
  }

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
  disp->tiling_mode = 0; /* 0 = tiled mode required by libSceVideoOut on this port */
  if (sceVideoOutSetBufferAttribute2) {
    sceVideoOutSetBufferAttribute2(
        attr, 0x8000000000000000ULL,
        (uint32_t)disp->tiling_mode, width, height,
        0, 0, 0);
  }
  agc_log("agc-attr-0", "attr word 0", *(const uint64_t *)(attr + 0));
  agc_log("agc-attr-8", "attr word 1", *(const uint64_t *)(attr + 8));

  /*
   * 5. Register buffers - this display's two, and any the caller brought.
   *
   * **This is the only chance to name them.** VideoOut buffer registration is
   * single-shot and immutable, measured on hardware (obSCEne
   * `REQ-20260921T1202Z-9a4c`): a second `sceVideoOutRegisterBuffers2` on a
   * handle that already has buffers returns `0x80290010`
   * (`SCE_VIDEO_OUT_ERROR_SLOT_OCCUPIED`) whether it repeats the set, extends
   * it, or starts at a different index; `sceVideoOutUnregisterBuffer(s)` are
   * absent from `libSceVideoOut` altogether, so a set cannot be released; and a
   * concurrent handle on the same output is refused. Nothing can be added
   * later.
   *
   * So a renderer that wants its own buffer scanned out - drawing straight into
   * it instead of copying through this display's - has to hand it over here,
   * before the output has been registered at all. Adopted buffers take the
   * indices after this display's own pair, so the first is flip index 2.
   */
  struct SceVideoOutBuffer buffers[2 + AGC_DISPLAY_MAX_ADOPT];
  for (size_t i = 0; i < sizeof(buffers); i++) {
    ((unsigned char *)buffers)[i] = 0;
  }
  buffers[0].data = disp->target_gpu_fb[0];
  buffers[1].data = disp->target_gpu_fb[1];

  int registered = 2;
  for (int i = 0; adopt && i < adopt_count && i < AGC_DISPLAY_MAX_ADOPT; i++) {
    if (!adopt[i])
      continue;
    buffers[registered].data = adopt[i];
    disp->adopted[i] = adopt[i];
    registered++;
  }
  disp->adopted_count = registered - 2;

  int rrc = sceVideoOutRegisterBuffers2(disp->handle, 0, 0, buffers, registered,
                                        attr, 0, 0);
  agc_log("agc-reg-count", "buffers registered", (uint64_t)(uint32_t)registered);
  agc_log("agc-reg-rc", "RegisterBuffers2 rc", (uint64_t)(uint32_t)rrc);
  if (rrc != 0) {
    disp->last_error = (int)(0xE4000000u | (uint32_t)(rrc & 0xFFFFFF));
    (void)sceKernelMunmap(disp->mapped_base, AGC_TOTAL_ALLOC_BYTES);
    (void)sceKernelReleaseDirectMemory(physical, AGC_TOTAL_ALLOC_BYTES);
    (void)sceVideoOutClose(disp->handle);
    disp->handle = -1;
    return disp;
  }

  /* Clear and initialize initial buffers */
  disp->ready = 1;
  disp->fb_index = 0;
  disp->last_error = 0;
  disp->flip_count = 0;

  if (sceVideoOutSetFlipRate) {
    sceVideoOutSetFlipRate(disp->handle, 0); /* 0 = 60Hz */
  }

  agc_display_clear(disp, 0);
  if (disp->tiling_mode == 1) {
    for (size_t i = 0; i < (size_t)width * height; i++) {
      disp->target_gpu_fb[0][i] = 0;
      disp->target_gpu_fb[1][i] = 0;
    }
  } else {
    agc_tile_surface(disp->target_gpu_fb[0], disp->linear_scratch_fb, width,
                     height);
    agc_tile_surface(disp->target_gpu_fb[1], disp->linear_scratch_fb, width,
                     height);
  }

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
          /* The shader instantiates, and agc_tiler_dispatch_params() can now
           * build the two buffer resource constants it wants (source in user
           * data 1..4, destination in 5..8 - see <agc/tiler.h>). That is a
           * decode of the payload, not a measurement, so opening a display does
           * not dispatch it. agc_display_try_gpu_tiler() is the opt-in that
           * checks the GPU's output against the CPU tiler before trusting it. */
          disp->gpu_accelerated = 0;
          agc_log("agc-gpu-accel",
                  "CPU tiling active; compute tiler available via "
                  "agc_display_try_gpu_tiler",
                  0);
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

/* Tiles the linear surface `src` onto `dst` with the compute shader. Returns 0
 * when the dispatch retired its fence, negative otherwise. */
static int agc_gpu_tile(struct agc_display *disp, const uint32_t *src,
                        uint32_t *dst) {
  uint32_t user_data[AGC_TILER_USER_DATA_COUNT];
  uint32_t groups_x = 0;
  uint32_t groups_y = 0;

  if (!disp->gpu_queue || !disp->gpu_shader || !src || !dst)
    return -1;
  if (agc_tiler_dispatch_params(user_data, &groups_x, &groups_y,
                                (uint64_t)(uintptr_t)src,
                                (uint64_t)(uintptr_t)dst, disp->width,
                                disp->height) != 0) {
    return -1;
  }

  oops_gpu_dispatch_t d;
  d.shader = disp->gpu_shader;
  d.user_data = user_data;
  d.user_data_count = AGC_TILER_USER_DATA_COUNT;
  d.grid_x = groups_x;
  d.grid_y = groups_y;
  d.grid_z = 1u;
  return oops_gpu_dispatch(disp->gpu_queue, &d);
}

static void agc_gpu_teardown(struct agc_display *disp) {
  if (disp->gpu_shader) {
    oops_gpu_destroy_shader(disp->gpu_shader);
    disp->gpu_shader = (oops_gpu_shader_t *)0;
  }
  if (disp->gpu_queue) {
    oops_gpu_destroy_queue(disp->gpu_queue);
    disp->gpu_queue = (oops_gpu_queue_t *)0;
  }
  disp->gpu_accelerated = 0;
}

int agc_display_try_gpu_tiler(agc_display_t *disp) {
  if (!disp)
    return -1;
  if (!disp->ready || !disp->linear_scratch_fb || disp->tiling_mode != 0)
    return 0;
  if (disp->gpu_accelerated)
    return 1;

  /* Both scanout buffers get written below, and after the first flip one of
   * them is on screen or queued. */
  if (disp->flip_count != 0) {
    agc_log("agc-gpu-try", "refused: display has already flipped",
            disp->flip_count);
    return 0;
  }

  disp->gpu_queue = oops_gpu_create_compute_queue();
  disp->gpu_shader = oops_gpu_create_shader(
      s_agc_tiler_hdr_full, sizeof(s_agc_tiler_hdr_full),
      s_agc_tiler_payload_base, sizeof(s_agc_tiler_payload_base));
  if (!disp->gpu_queue || !disp->gpu_shader) {
    agc_log("agc-gpu-try", "refused: queue or shader would not create", 0);
    agc_gpu_teardown(disp);
    return 0;
  }

  size_t pixels = (size_t)disp->width * (size_t)disp->height;
  size_t tiled_bytes = agc_tile_surface_bytes(disp->width, disp->height);

  /* A distinct value per pixel. Comparing two buffers that are both zero would
   * pass whatever the shader did, including nothing. */
  for (size_t i = 0; i < pixels; i++) {
    disp->linear_scratch_fb[i] = (uint32_t)(i * 2654435761u) | 0xFF000000u;
  }

  /* Zero both destinations over the whole tiled extent first. The tiler leaves
   * the margin of a partial tile alone, so without this the comparison would
   * run over direct memory neither side wrote - and with it, a shader that
   * writes outside the pixels it owns fails here rather than on a display. */
  for (size_t i = 0; i < tiled_bytes / 4u; i++) {
    disp->target_gpu_fb[0][i] = 0u;
    disp->target_gpu_fb[1][i] = 0u;
  }

  int rc = agc_gpu_tile(disp, disp->linear_scratch_fb, disp->target_gpu_fb[1]);
  agc_log("agc-gpu-try", "dispatch rc", (uint64_t)(uint32_t)rc);
  if (rc != 0) {
    agc_gpu_teardown(disp);
    return 0;
  }

  agc_tile_surface(disp->target_gpu_fb[0], disp->linear_scratch_fb, disp->width,
                   disp->height);

  size_t mismatch_at = tiled_bytes;
  for (size_t i = 0; i < tiled_bytes / 4u; i++) {
    if (disp->target_gpu_fb[0][i] != disp->target_gpu_fb[1][i]) {
      mismatch_at = i * 4u;
      break;
    }
  }

  if (mismatch_at != tiled_bytes) {
    agc_log("agc-gpu-try", "refused: first mismatch at byte",
            (uint64_t)mismatch_at);
    agc_gpu_teardown(disp);
    /* Leave the buffers in the state a first flip expects. */
    agc_display_clear(disp, 0);
    agc_tile_surface(disp->target_gpu_fb[0], disp->linear_scratch_fb,
                     disp->width, disp->height);
    agc_tile_surface(disp->target_gpu_fb[1], disp->linear_scratch_fb,
                     disp->width, disp->height);
    return 0;
  }

  agc_log("agc-gpu-try", "compute tiler matches the CPU tiler, enabling",
          (uint64_t)tiled_bytes);
  disp->gpu_accelerated = 1;
  agc_display_clear(disp, 0);
  agc_tile_surface(disp->target_gpu_fb[0], disp->linear_scratch_fb, disp->width,
                   disp->height);
  agc_tile_surface(disp->target_gpu_fb[1], disp->linear_scratch_fb, disp->width,
                   disp->height);
  return 1;
}

/* Put the linear image `src` on screen: tiled onto the next scanout buffer and
 * flipped. agc_display_flip passes the display's own linear surface;
 * agc_display_present passes a caller's. With `src` NULL the next buffer is
 * flipped as it stands - a renderer drew it in place (agc_display_flip_scanout). */
static int agc_display_flip_from(agc_display_t *disp, const uint32_t *src) {
  if (!disp || !disp->ready)
    return -1;

  unsigned int shown = disp->fb_index;
  disp->fb_index = (disp->fb_index + 1) % 2;

  uint64_t t_flip_start = oops_time_get_us();

  /* Scanout buffer preparation: none when the buffer was drawn in place, a
   * linear direct copy when tiling_mode == 1, CPU software display-tiling when
   * tiling_mode == 0 (RDNA2 display-tiled). */
  if (!src) {
    /* Drawn in place. */
  } else if (disp->tiling_mode == 1) {
    const uint64_t *src64 = (const uint64_t *)src;
    uint64_t *dst64 = (uint64_t *)disp->target_gpu_fb[shown];
    size_t qwords = ((size_t)disp->width * disp->height * sizeof(uint32_t)) / sizeof(uint64_t);
    for (size_t i = 0; i < qwords; i++) {
      dst64[i] = src64[i];
    }
    if (disp->flip_count < 5) {
      agc_log("agc-scanout-lin", "linear frame scanout, first pixel", (uint64_t)disp->target_gpu_fb[shown][0]);
    }
  } else if (disp->gpu_accelerated) {
    /* Compute tiling, enabled only by agc_display_try_gpu_tiler() after it
     * matched this shader's output against the CPU tiler. A dispatch that stops
     * retiring later falls back rather than presenting a stale buffer. */
    if (agc_gpu_tile(disp, src, disp->target_gpu_fb[shown]) != 0) {
      agc_log("agc-gpu-fall", "dispatch failed mid-run, back to CPU tiling",
              disp->flip_count);
      agc_gpu_teardown(disp);
      agc_tile_surface(disp->target_gpu_fb[shown], src, disp->width,
                       disp->height);
    }
  } else {
    /* CPU software tiling: swizzle the linear image into RDNA2 tiled scanout surface */
    agc_tile_surface(disp->target_gpu_fb[shown], src, disp->width,
                     disp->height);
  }

  uint64_t t_tile_done = oops_time_get_us();

  if (disp->flip_count <= 5) {
    agc_log("agc-pix-0", "target_gpu_fb first pixel", (uint64_t)disp->target_gpu_fb[shown][0]);
    if (src)
      agc_log("agc-scr-0", "linear source first pixel", (uint64_t)src[0]);
  }

  disp->flip_count++;

  int frc = 0;
  if (sceVideoOutSubmitFlip) {
    frc = sceVideoOutSubmitFlip(disp->handle, (int)shown, 1, 0);
    if (frc != 0 || disp->flip_count <= 5) {
      agc_log("agc-flip-rc", "SubmitFlip rc", (uint64_t)(uint32_t)frc);
    }
  }

  uint64_t t_submit_done = oops_time_get_us();

  if (disp->flip_count <= 10 || (disp->flip_count % 60) == 0) {
    oops_log_debug("AGC", "flip %lu: tile=%lu submit=%lu total=%lu",
                   (unsigned long)disp->flip_count,
                   (unsigned long)(t_tile_done - t_flip_start),
                   (unsigned long)(t_submit_done - t_tile_done),
                   (unsigned long)(t_submit_done - t_flip_start));
  }

  return frc;
}

int agc_display_flip(agc_display_t *disp) {
  return disp ? agc_display_flip_from(disp, disp->linear_scratch_fb) : -1;
}

int agc_display_present(agc_display_t *disp, const uint32_t *pixels) {
  return pixels ? agc_display_flip_from(disp, pixels) : -1;
}

/* The scanout buffers, for a renderer that draws them itself. Tiling mode 0 is
 * the GPU's 64KB_R_X render-target swizzle the display tiler writes; 1 is rows. */
int agc_display_scanout_layout(const agc_display_t *disp) {
  if (!disp || !disp->ready)
    return 0;
  return disp->tiling_mode == 1 ? 1 : 2;
}

uint32_t *agc_display_scanout(agc_display_t *disp, int which) {
  if (!disp || !disp->ready)
    return (uint32_t *)0;
  return disp->target_gpu_fb[which ? (disp->fb_index + 1) % 2 : disp->fb_index];
}

/*
 * The flip index a buffer handed to agc_display_open_adopting was given, or -1.
 *
 * `nth` is the position in the array passed at open, so the first adopted
 * buffer is `nth = 0`. Indices run after this display's own pair, which makes
 * the first one 2 - but that is an implementation detail and callers should ask
 * rather than assume it.
 *
 * **There was an agc_display_adopt_buffer here until 2026-09-21, and it could
 * never have worked.** It re-registered the display's set with a foreign buffer
 * appended, after the display was already open. obSCEne `-9a4c` then measured
 * that VideoOut registration is single-shot and immutable: a second
 * registration returns `0x80290010` (`SCE_VIDEO_OUT_ERROR_SLOT_OCCUPIED`)
 * however it is shaped, unregistration is not exported at all, and a concurrent
 * handle is refused. So adoption has to happen *at open*, which is where it now
 * happens, and the function that promised otherwise is gone rather than left to
 * be found and trusted.
 */
int agc_display_adopted_index(const agc_display_t *disp, int nth) {
  if (!disp || !disp->ready || nth < 0 || nth >= disp->adopted_count)
    return -1;
  if (!disp->adopted[nth])
    return -1;
  return 2 + nth;
}

/* Flip a buffer by its index, as it stands - nothing tiled or copied into it.
 * This display's own two are 0 and 1; an adopted buffer's index comes from
 * agc_display_adopted_index. */
int agc_display_flip_index(agc_display_t *disp, int index) {
  if (!disp || !disp->ready || disp->handle <= 0 || !sceVideoOutSubmitFlip)
    return -1;
  if (index < 0)
    return -1;

  int rc = sceVideoOutSubmitFlip(disp->handle, index, 1, 0);
  if (rc != 0) {
    disp->last_error = (int)(0xE4000000u | (uint32_t)(rc & 0xFFFFFF));
    return -1;
  }
  disp->flip_count++;
  return 0;
}

/* **Flip pacing for a buffer drawn in place.** The submit call queues and
 * returns in microseconds, with up to 26 flips pending (obSCEne
 * 080-video/visual-flip), so a renderer that drew the next buffer straight after
 * a flip could be drawing into the buffer still on screen. Once no flip is
 * pending, the last one submitted is on screen and the other buffer - the next -
 * has left it. The same poll the display made before every flip until
 * 2026-09-19: a millisecond at a time, for at most about 100 ms, and no wait at
 * all where the status query is not linked. */
int agc_display_wait_scanout(agc_display_t *disp) {
  if (!disp || !disp->ready)
    return -1;
  if (disp->handle <= 0 || !sceVideoOutGetFlipStatus || !sceKernelUsleep)
    return 0;
  for (int i = 0; i < 100; i++) {
    struct agc_flip_status status;
    for (size_t k = 0; k < sizeof(status); k++)
      ((unsigned char *)&status)[k] = 0;
    if (sceVideoOutGetFlipStatus(disp->handle, &status) != 0)
      return 0;
    if (status.num_flip_pending <= 0)
      return 0;
    sceKernelUsleep(1000);
  }
  return 1;
}

int agc_display_flip_scanout(agc_display_t *disp) {
  return agc_display_flip_from(disp, (const uint32_t *)0);
}

/* The scanout buffer last flipped is the one on screen: `fb_index` has already
 * moved past it. Before the first flip both hold the image open() tiled into
 * them, so either answers. */
int agc_display_read_shown(agc_display_t *disp, uint32_t *pixels) {
  if (!disp || !disp->ready || !pixels)
    return -1;
  const uint32_t *on_screen = disp->target_gpu_fb[(disp->fb_index + 1) % 2];
  if (disp->tiling_mode == 1) {
    for (size_t i = 0; i < (size_t)disp->width * disp->height; i++)
      pixels[i] = on_screen[i];
  } else {
    agc_detile_surface(pixels, on_screen, disp->width, disp->height);
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
  oops_log_info("AGC", "agc_display_close: handle=%d", disp->handle);

  agc_gpu_teardown(disp);

  /* Released on whether they were allocated, not on whether acceleration ended
   * up enabled. agc_display_open() allocates the queue, the command buffer, the
   * fence and the shader payload while probing for GPU support and leaves the
   * flag clear, so a condition on the flag freed none of them - and the display
   * is a singleton that open() zeroes, so every close/open cycle lost three
   * 64 KB Onion allocations and a driver queue. */
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
  if (disp->owns_scratch && disp->linear_scratch_fb) {
    oops_mem_free(disp->linear_scratch_fb);
    disp->linear_scratch_fb = (uint32_t *)0;
    disp->owns_scratch = 0;
  }
  disp->shader_obj = (void *)0;

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
