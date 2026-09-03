#include <stdint.h>
#include <stddef.h>
#include "gnm/display.h"

typedef int64_t sce_off_t;

struct SceVideoOutBufferAttribute {
    uint32_t pixelformat;
    uint32_t tiling_mode;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t reserved[44];
};

struct SceVideoOutFlipStatus {
    uint64_t count;
    uint64_t flip_arg;
    uint64_t submit_time;
    uint64_t flip_time;
    uint32_t current_buffer;
    uint32_t status;
};

__attribute__((weak)) int sceVideoOutOpen(int userId, int type, int index, const void *param);
__attribute__((weak)) int sceVideoOutClose(int handle);
__attribute__((weak)) void sceVideoOutSetBufferAttribute(struct SceVideoOutBufferAttribute *attr,
                                                         uint32_t pixelformat, uint32_t tiling_mode,
                                                         uint32_t width, uint32_t height, uint32_t pitch);
__attribute__((weak)) int sceVideoOutRegisterBuffers(int handle, int startIndex, void * const *addresses,
                                                     int bufferCount, const struct SceVideoOutBufferAttribute *attribute);
__attribute__((weak)) int sceVideoOutSubmitFlip(int handle, int index, unsigned int flipMode, int64_t flipArg);
__attribute__((weak)) int sceVideoOutGetFlipStatus(int handle, struct SceVideoOutFlipStatus *status);
__attribute__((weak)) size_t sceKernelGetDirectMemorySize(void);
__attribute__((weak)) int sceKernelAllocateDirectMemory(sce_off_t searchStart, sce_off_t searchEnd,
                                                        size_t len, size_t alignment, int memoryType, sce_off_t *paddr);
__attribute__((weak)) int sceKernelReleaseDirectMemory(sce_off_t paddr, size_t len);
__attribute__((weak)) int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags,
                                                   sce_off_t paddr, size_t alignment);
__attribute__((weak)) int sceKernelMunmap(void *addr, size_t len);

#define GNM_STRIDE_BYTES       0x800000u /* 8 MB per 1080p linear buffer */
#define GNM_TOTAL_ALLOC_BYTES  0x1000000u /* 16 MB double-buffered */

struct gnm_display {
    int handle;
    unsigned int width;
    unsigned int height;
    sce_off_t physical;
    void *mapped_base;
    uint32_t *buffers[2];
    unsigned int fb_index;
    int ready;
    int last_error;
};

static struct gnm_display s_gnm_display;

gnm_display_t *gnm_display_open(unsigned int width, unsigned int height) {
    struct gnm_display *disp = &s_gnm_display;
    for (size_t i = 0; i < sizeof(*disp); i++) ((unsigned char *)disp)[i] = 0;
    disp->handle = -1;
    disp->width = width;
    disp->height = height;

    if (!sceVideoOutOpen || !sceVideoOutRegisterBuffers || !sceKernelAllocateDirectMemory || !sceKernelMapDirectMemory) {
        disp->last_error = -1;
        return disp;
    }

    int vh = sceVideoOutOpen(0, 0, 0, 0);
    if (vh < 0) {
        disp->last_error = vh;
        return disp;
    }
    disp->handle = vh;

    size_t pool_size = sceKernelGetDirectMemorySize ? sceKernelGetDirectMemorySize() : 0x80000000u;
    sce_off_t physical = 0;
    int arc = sceKernelAllocateDirectMemory(0, (sce_off_t)pool_size, GNM_TOTAL_ALLOC_BYTES, 0x200000u, 3, &physical);
    if (arc != 0) {
        disp->last_error = arc;
        (void)sceVideoOutClose(disp->handle);
        disp->handle = -1;
        return disp;
    }
    disp->physical = physical;

    void *mapped = 0;
    int mrc = sceKernelMapDirectMemory(&mapped, GNM_TOTAL_ALLOC_BYTES, 0x33, 0, physical, 0x200000u);
    if (mrc != 0 || !mapped) {
        disp->last_error = mrc;
        (void)sceKernelReleaseDirectMemory(physical, GNM_TOTAL_ALLOC_BYTES);
        (void)sceVideoOutClose(disp->handle);
        disp->handle = -1;
        return disp;
    }
    disp->mapped_base = mapped;
    disp->buffers[0] = (uint32_t *)mapped;
    disp->buffers[1] = (uint32_t *)((unsigned char *)mapped + GNM_STRIDE_BYTES);

    struct SceVideoOutBufferAttribute attr;
    for (size_t i = 0; i < sizeof(attr); i++) ((unsigned char *)&attr)[i] = 0;
    if (sceVideoOutSetBufferAttribute) {
        sceVideoOutSetBufferAttribute(&attr, 0x80220000u, 0, width, height, width);
    }

    void *addresses[2];
    addresses[0] = disp->buffers[0];
    addresses[1] = disp->buffers[1];

    int rrc = sceVideoOutRegisterBuffers(disp->handle, 0, addresses, 2, &attr);
    if (rrc != 0) {
        disp->last_error = rrc;
        (void)sceKernelMunmap(mapped, GNM_TOTAL_ALLOC_BYTES);
        (void)sceKernelReleaseDirectMemory(physical, GNM_TOTAL_ALLOC_BYTES);
        (void)sceVideoOutClose(disp->handle);
        disp->handle = -1;
        return disp;
    }

    disp->ready = 1;
    disp->fb_index = 0;
    gnm_display_clear(disp, 0);

    return disp;
}

int gnm_display_is_ready(const gnm_display_t *disp) {
    return disp && disp->ready;
}

uint32_t *gnm_display_get_framebuffer(gnm_display_t *disp) {
    return disp && disp->ready ? disp->buffers[disp->fb_index] : 0;
}

unsigned int gnm_display_get_width(const gnm_display_t *disp) {
    return disp ? disp->width : 0;
}

unsigned int gnm_display_get_height(const gnm_display_t *disp) {
    return disp ? disp->height : 0;
}

uint64_t gnm_display_get_flip_count(const gnm_display_t *disp) {
    if (!disp || disp->handle < 0 || !sceVideoOutGetFlipStatus) return 0;
    struct SceVideoOutFlipStatus status;
    for (size_t i = 0; i < sizeof(status); i++) ((unsigned char *)&status)[i] = 0;
    if (sceVideoOutGetFlipStatus(disp->handle, &status) == 0) {
        return status.count;
    }
    return 0;
}

int gnm_display_get_last_error(const gnm_display_t *disp) {
    return disp ? disp->last_error : -1;
}

void gnm_display_clear(gnm_display_t *disp, uint32_t color) {
    uint32_t *fb = gnm_display_get_framebuffer(disp);
    if (!fb) return;
    size_t count = (size_t)disp->width * disp->height;
    for (size_t i = 0; i < count; i++) {
        fb[i] = color;
    }
}

int gnm_display_flip(gnm_display_t *disp) {
    if (!disp || !disp->ready || disp->handle < 0) return -1;
    unsigned int shown = disp->fb_index;
    disp->fb_index = (disp->fb_index + 1u) % 2u;

    int rc = sceVideoOutSubmitFlip(disp->handle, (int)shown, 1, 0);
    if (rc != 0) {
        disp->last_error = rc;
    }
    return rc;
}

void gnm_display_close(gnm_display_t *disp) {
    if (!disp) return;
    if (disp->handle > 0 && sceVideoOutClose) {
        (void)sceVideoOutClose(disp->handle);
        disp->handle = -1;
    }
    disp->ready = 0;
}
