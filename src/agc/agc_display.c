#include <stdint.h>
#include <stddef.h>
#include "agc/display.h"
#include "agc/tiler.h"

/* PS5 Kernel / VideoOut stubs */
typedef int64_t sce_off_t;

/* Exactly 32 bytes matching PS5 kernel batch-map descriptor */
struct obs_batch_map_entry {
    void *vaddr;
    sce_off_t paddr;
    size_t len;
    uint8_t prot;
    uint8_t pad[3];
    uint32_t flags;
};

/* Exactly 32 bytes matching PS5 kernel video buffer descriptor */
struct SceVideoOutBuffer {
    void *data;
    void *metadata;
    void *reserved[2];
};

/* Imported platform symbols (weak so compilation succeeds in any environment) */
__attribute__((weak)) int sceUserServiceInitialize(const void *param);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceVideoOutOpen(int userId, int type, int index, const void *param);
__attribute__((weak)) int sceVideoOutClose(int handle);
__attribute__((weak)) void sceVideoOutSetBufferAttribute2(void *attr,
                                                          uint64_t pixelformat, uint32_t tiling_mode,
                                                          uint32_t width, uint32_t height, uint64_t option,
                                                          uint32_t dcc_control, uint64_t dcc_clear_color);
__attribute__((weak)) int sceVideoOutRegisterBuffers2(int handle, int startIndex, int unk,
                                                      const struct SceVideoOutBuffer *buffers,
                                                      int bufferCount,
                                                      const void *attribute,
                                                      int category, void *reserved);
__attribute__((weak)) int sceVideoOutSubmitFlip(int handle, int index, unsigned int flipMode, int64_t flipArg);
__attribute__((weak)) int sceKernelAllocateMainDirectMemory(size_t len, size_t alignment, int memoryType, sce_off_t *paddr);
__attribute__((weak)) int sceKernelAllocateDirectMemory(sce_off_t searchStart, sce_off_t searchEnd,
                                                        size_t len, size_t alignment, int memoryType, sce_off_t *paddr);
__attribute__((weak)) int sceKernelGetDirectMemorySize(void);
__attribute__((weak)) int sceKernelReleaseDirectMemory(sce_off_t paddr, size_t len);
__attribute__((weak)) int sceKernelBatchMap(struct obs_batch_map_entry *entries, int num_entries, int *completed);
__attribute__((weak)) int sceKernelMunmap(void *addr, size_t len);
__attribute__((weak)) int sceKernelOpen(const char *path, int flags, int mode);

#define AGC_STRIDE_BYTES       0xa00000u   /* 10 MB per tiled buffer (2MB-aligned) */
#define AGC_TOTAL_ALLOC_BYTES  0x2000000u  /* 32 MB (16 x 2MB pages) */
#define AGC_VM_BASE            0x4000000000ULL

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
};

static struct agc_display s_agc_display;
static agc_log_fn s_logger = 0;

void agc_display_set_logger(agc_log_fn fn) {
    s_logger = fn;
}

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
        arc = sceKernelAllocateMainDirectMemory(AGC_TOTAL_ALLOC_BYTES, 0x200000u, 3 /* WC_GARLIC */, &physical);
        agc_log("agc-alloc-main", "AllocateMainDirectMemory", (uint64_t)(uint32_t)arc);
    } else if (sceKernelAllocateDirectMemory && sceKernelGetDirectMemorySize) {
        arc = sceKernelAllocateDirectMemory(0, (sce_off_t)sceKernelGetDirectMemorySize(),
                                            AGC_TOTAL_ALLOC_BYTES, 0x200000u, 3, &physical);
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

    /* 3. Batch-map 16 pages of 2MB into 0x4000000000ULL with GPU protection 0x33 */
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
        b_entries[p].len   = 0x200000u;
        b_entries[p].prot  = 0x33; /* PROT_CPU_RW | PROT_GPU_RW */
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
    disp->target_gpu_fb[1] = (uint32_t *)((unsigned char *)disp->mapped_base + AGC_STRIDE_BYTES);
    disp->linear_scratch_fb = (uint32_t *)((unsigned char *)disp->mapped_base + 2 * AGC_STRIDE_BYTES);

    agc_log("agc-b0-addr", "buffer 0 addr", (uint64_t)(uintptr_t)disp->target_gpu_fb[0]);
    agc_log("agc-b1-addr", "buffer 1 addr", (uint64_t)(uintptr_t)disp->target_gpu_fb[1]);
    agc_log("agc-lin-addr", "scratch addr", (uint64_t)(uintptr_t)disp->linear_scratch_fb);

    /* 4. Configure buffer attribute: 256-byte buffer, 64-bit SDR format, 1920x1080, pitch 0 */
    unsigned char attr[256];
    for (size_t i = 0; i < sizeof(attr); i++) attr[i] = 0;
    if (sceVideoOutSetBufferAttribute2) {
        sceVideoOutSetBufferAttribute2(attr, 0x8000000000000000ULL, 0 /* kLinear */,
                                       width, height, 0, 0, 0);
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

    int rrc = sceVideoOutRegisterBuffers2(disp->handle, 0, 0, buffers, 2, attr, 0, 0);
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
    agc_tile_surface(disp->target_gpu_fb[0], disp->linear_scratch_fb, width, height);
    agc_tile_surface(disp->target_gpu_fb[1], disp->linear_scratch_fb, width, height);

    return disp;
}

int agc_display_is_ready(const agc_display_t *disp) {
    return disp && disp->ready;
}

uint32_t *agc_display_get_framebuffer(agc_display_t *disp) {
    if (!disp || !disp->ready) return 0;
    return disp->linear_scratch_fb;
}

unsigned int agc_display_get_width(const agc_display_t *disp) {
    return disp ? disp->width : 0;
}

unsigned int agc_display_get_height(const agc_display_t *disp) {
    return disp ? disp->height : 0;
}

int agc_display_flip(agc_display_t *disp) {
    if (!disp || !disp->ready) return -1;

    unsigned int shown = disp->fb_index;
    disp->fb_index = (disp->fb_index + 1) % 2;

    /* Tile the linear scratch buffer into the targeted display buffer */
    agc_tile_surface(disp->target_gpu_fb[shown], disp->linear_scratch_fb, disp->width, disp->height);

    disp->flip_count++;

    if (sceVideoOutSubmitFlip) {
        int frc = sceVideoOutSubmitFlip(disp->handle, (int)shown, 1, 0);
        agc_log("agc-flip-rc", "SubmitFlip rc", (uint64_t)(uint32_t)frc);
        return frc;
    }
    return 0;
}

void agc_display_clear(agc_display_t *disp, uint32_t color) {
    if (!disp || !disp->ready || !disp->linear_scratch_fb) return;
    size_t count = (size_t)disp->width * (size_t)disp->height;
    for (size_t i = 0; i < count; i++) {
        disp->linear_scratch_fb[i] = color;
    }
}

uint64_t agc_display_get_flip_count(const agc_display_t *disp) {
    if (!disp) return 0;
    return disp->flip_count;
}

int agc_display_get_last_error(const agc_display_t *disp) {
    return disp ? disp->last_error : -1;
}

void agc_display_close(agc_display_t *disp) {
    if (!disp) return;
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
