#include "gnm/display.h"
#include <stddef.h>
#include <stdint.h>

typedef int64_t sce_off_t;

struct SceVideoOutBufferAttribute {
    uint32_t pixelformat;
    uint32_t tiling_mode;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t reserved[44];
};

/* 64 bytes, confirmed on 12.40 in the app context (obSCEne
 * 080-video/flip-status): a fresh handle reads flip_arg = -1 at offset 24 and
 * current_buffer = -1 at offset 56, everything else zero, which is exactly this
 * shape. */
struct SceVideoOutFlipStatus {
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

typedef int sce_equeue_t;

__attribute__((weak)) int sceUserServiceInitialize(const void *param);
__attribute__((weak)) int sceUserServiceGetInitialUser(int32_t *userId);
__attribute__((weak)) int sceVideoOutOpen(int userId, int type, int index,
                                          const void *param);
__attribute__((weak)) int sceVideoOutClose(int handle);
__attribute__((weak)) void sceVideoOutSetBufferAttribute(
    struct SceVideoOutBufferAttribute *attr, uint32_t pixelformat, uint32_t tiling_mode,
    uint32_t aspect_ratio, uint32_t width, uint32_t height, uint32_t pitch);
__attribute__((weak)) int
sceVideoOutRegisterBuffers(int handle, int startIndex, void *const *addresses,
                           int bufferCount,
                           const struct SceVideoOutBufferAttribute *attribute);
__attribute__((weak)) int sceVideoOutSubmitFlip(int handle, int index,
                                                unsigned int flipMode, int64_t flipArg);
__attribute__((weak)) int
sceVideoOutGetFlipStatus(int handle, struct SceVideoOutFlipStatus *status);
__attribute__((weak)) int sceVideoOutSetFlipRate(int handle, int rate);
__attribute__((weak)) int sceVideoOutAddFlipEvent(sce_equeue_t eq, int handle,
                                                  void *arg);
__attribute__((weak)) int sceVideoOutIsFlipPending(int handle);
__attribute__((weak)) int sceKernelCreateEqueue(sce_equeue_t *eq, const char *name);
__attribute__((weak)) int sceKernelDeleteEqueue(sce_equeue_t eq);
__attribute__((weak)) int sceKernelWaitEqueue(sce_equeue_t eq, void *ev, int num,
                                              int *out, void *tmo);
__attribute__((weak)) size_t sceKernelGetDirectMemorySize(void);
__attribute__((weak)) int
sceKernelAllocateDirectMemory(sce_off_t searchStart, sce_off_t searchEnd, size_t len,
                              size_t alignment, int memoryType, sce_off_t *paddr);
__attribute__((weak)) int sceKernelReleaseDirectMemory(sce_off_t paddr, size_t len);
__attribute__((weak)) int sceKernelMapDirectMemory(void **addr, size_t len, int prot,
                                                   int flags, sce_off_t paddr,
                                                   size_t alignment);
__attribute__((weak)) int sceKernelMunmap(void *addr, size_t len);

#define GNM_FB_ALIGN 0x10000u /* 64 KB page alignment for scanout memory */
#define GNM_MAX_DIM                                                                    \
    0xFFFFu /* beyond any scanout mode; keeps width * height * 4 in range */

/* Two buffers, each the requested surface rounded up to the page, sized at
 * open. */
static size_t gnm_align_up(size_t value, size_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

struct gnm_display {
    int handle;
    sce_equeue_t flip_queue;
    int has_flip_queue;
    unsigned int width;
    unsigned int height;
    sce_off_t physical;
    void *mapped_base;
    size_t fb_bytes;    /* one buffer, page-aligned */
    size_t total_bytes; /* both buffers: what was allocated and mapped */
    int has_memory;
    uint32_t *buffers[2];
    unsigned int fb_index;
    uint64_t flip_seq;
    int ready;
    int last_error;
};

static struct gnm_display s_gnm_display;

gnm_display_t *gnm_display_open(unsigned int width, unsigned int height) {
    struct gnm_display *disp = &s_gnm_display;
    for (size_t i = 0; i < sizeof(*disp); i++)
        ((unsigned char *)disp)[i] = 0;
    disp->handle = -1;
    disp->width = width;
    disp->height = height;

    /* Dimensions are bounded so the byte count cannot overflow; beyond that the
     * direct memory allocator is the judge of what fits, and its refusal is
     * reported as it is. */
    if (width == 0 || height == 0 || width > GNM_MAX_DIM || height > GNM_MAX_DIM) {
        disp->last_error = -2;
        return disp;
    }
    disp->fb_bytes = gnm_align_up((size_t)width * (size_t)height * 4u, GNM_FB_ALIGN);
    disp->total_bytes = disp->fb_bytes * 2u;

    if (!sceVideoOutOpen || !sceVideoOutRegisterBuffers ||
        !sceKernelAllocateDirectMemory || !sceKernelMapDirectMemory) {
        disp->last_error = -1;
        return disp;
    }

    int32_t user = 0;
    if (sceUserServiceGetInitialUser) {
        if (sceUserServiceGetInitialUser(&user) != 0) {
            if (sceUserServiceInitialize) {
                (void)sceUserServiceInitialize(0);
                (void)sceUserServiceGetInitialUser(&user);
            }
        }
    }

    int vh = sceVideoOutOpen(user, 0, 0, 0);
    if (vh < 0) {
        disp->last_error = vh;
        return disp;
    }
    disp->handle = vh;

    size_t pool_size =
        sceKernelGetDirectMemorySize ? sceKernelGetDirectMemorySize() : 0x80000000u;
    sce_off_t physical = 0;
    int arc = sceKernelAllocateDirectMemory(0, (sce_off_t)pool_size, disp->total_bytes,
                                            GNM_FB_ALIGN, 3, &physical);
    if (arc != 0) {
        disp->last_error = arc;
        (void)sceVideoOutClose(disp->handle);
        disp->handle = -1;
        return disp;
    }
    disp->physical = physical;

    void *mapped = 0;
    int mrc = sceKernelMapDirectMemory(&mapped, disp->total_bytes, 0x33, 0, physical,
                                       GNM_FB_ALIGN);
    if (mrc != 0 || !mapped) {
        disp->last_error = mrc;
        (void)sceKernelReleaseDirectMemory(physical, disp->total_bytes);
        (void)sceVideoOutClose(disp->handle);
        disp->handle = -1;
        return disp;
    }
    disp->mapped_base = mapped;
    disp->has_memory = 1;
    disp->buffers[0] = (uint32_t *)mapped;
    disp->buffers[1] = (uint32_t *)((unsigned char *)mapped + disp->fb_bytes);

    struct SceVideoOutBufferAttribute attr;
    for (size_t i = 0; i < sizeof(attr); i++)
        ((unsigned char *)&attr)[i] = 0;
    if (sceVideoOutSetBufferAttribute) {
        sceVideoOutSetBufferAttribute(&attr, 0x80000000u, 1 /* linear */, 0 /* 16:9 */,
                                      width, height, width);
    }

    void *addresses[2];
    addresses[0] = disp->buffers[0];
    addresses[1] = disp->buffers[1];

    int rrc = sceVideoOutRegisterBuffers(disp->handle, 0, addresses, 2, &attr);
    if (rrc != 0) {
        disp->last_error = rrc;
        (void)sceKernelMunmap(mapped, disp->total_bytes);
        (void)sceKernelReleaseDirectMemory(physical, disp->total_bytes);
        (void)sceVideoOutClose(disp->handle);
        disp->handle = -1;
        return disp;
    }

    disp->flip_queue = -1;
    disp->has_flip_queue = 0;
    if (sceKernelCreateEqueue && sceVideoOutAddFlipEvent) {
        sce_equeue_t eq = -1;
        if (sceKernelCreateEqueue(&eq, "gnmFlipQueue") == 0 && eq >= 0) {
            disp->flip_queue = eq;
            if (sceVideoOutAddFlipEvent(eq, disp->handle, 0) == 0) {
                disp->has_flip_queue = 1;
            }
        }
    }
    if (sceVideoOutSetFlipRate) {
        sceVideoOutSetFlipRate(disp->handle, 0); /* 0 = 60Hz */
    }

    disp->ready = 1;
    disp->fb_index = 0;
    disp->flip_seq = 0;
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
    if (!disp || disp->handle < 0 || !sceVideoOutGetFlipStatus)
        return 0;
    struct SceVideoOutFlipStatus status;
    for (size_t i = 0; i < sizeof(status); i++)
        ((unsigned char *)&status)[i] = 0;
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
    if (!fb)
        return;
    size_t count = (size_t)disp->width * disp->height;
    for (size_t i = 0; i < count; i++) {
        fb[i] = color;
    }
}

int gnm_display_flip(gnm_display_t *disp) {
    if (!disp || !disp->ready || disp->handle < 0)
        return -1;
    unsigned int shown = disp->fb_index;
    disp->fb_index = (disp->fb_index + 1u) % 2u;
    disp->flip_seq++;

    int rc =
        sceVideoOutSubmitFlip(disp->handle, (int)shown, 1, (int64_t)disp->flip_seq);
    if (rc != 0) {
        disp->last_error = rc;
        return rc;
    }

    if (disp->has_flip_queue && sceVideoOutIsFlipPending && sceKernelWaitEqueue) {
        uint8_t ev[32];
        int out = 0;
        while (sceVideoOutIsFlipPending(disp->handle) > 0) {
            if (sceKernelWaitEqueue(disp->flip_queue, ev, 1, &out, 0) != 0) {
                break;
            }
        }
    }

    return 0;
}

/* The framebuffer here *is* a scanout buffer, and the one on screen is the other:
 * the last flipped, which `fb_index` has moved past. A caller's image goes
 * straight into that one - scanned out as it is written, which is what drawing
 * to a front buffer looks like - and the framebuffer is left alone. Before the
 * first flip nothing is on screen, so the image is flipped there once, and the
 * same buffer stays the one on screen. */
int gnm_display_present(gnm_display_t *disp, const uint32_t *pixels) {
    if (!disp || !disp->ready || disp->handle < 0 || !pixels)
        return -1;
    const unsigned int on_screen = (disp->fb_index + 1u) % 2u;
    uint32_t *dst = disp->buffers[on_screen];
    for (size_t i = 0; i < (size_t)disp->width * disp->height; i++)
        dst[i] = pixels[i];
    if (disp->flip_seq != 0)
        return 0;
    disp->flip_seq++;
    int rc =
        sceVideoOutSubmitFlip(disp->handle, (int)on_screen, 1, (int64_t)disp->flip_seq);
    if (rc != 0)
        disp->last_error = rc;
    return rc;
}

int gnm_display_read_shown(gnm_display_t *disp, uint32_t *pixels) {
    if (!disp || !disp->ready || !pixels)
        return -1;
    const uint32_t *src = disp->buffers[(disp->fb_index + 1u) % 2u];
    for (size_t i = 0; i < (size_t)disp->width * disp->height; i++)
        pixels[i] = src[i];
    return 0;
}

/* The framebuffer is the next scanout buffer already, in rows; the flip waits
 * for its own completion, so the next buffer is free as soon as it returns. */
int gnm_display_scanout_layout(const gnm_display_t *disp) {
    return disp && disp->ready ? 1 : 0;
}

uint32_t *gnm_display_scanout(gnm_display_t *disp, int which) {
    if (!disp || !disp->ready)
        return (uint32_t *)0;
    return disp->buffers[which ? (disp->fb_index + 1u) % 2u : disp->fb_index];
}

int gnm_display_wait_scanout(gnm_display_t *disp) {
    return disp && disp->ready ? 0 : -1;
}

int gnm_display_flip_scanout(gnm_display_t *disp) {
    return gnm_display_flip(disp);
}

int gnm_display_set_flip_rate(gnm_display_t *disp, unsigned int rate) {
    if (!disp || disp->handle < 0)
        return -1;
    if (sceVideoOutSetFlipRate) {
        int rc = sceVideoOutSetFlipRate(disp->handle, (int)rate);
        if (rc != 0)
            disp->last_error = rc;
        return rc;
    }
    return -1;
}

int gnm_display_get_video_handle(const gnm_display_t *disp) {
    return disp ? disp->handle : -1;
}

void gnm_display_close(gnm_display_t *disp) {
    if (!disp)
        return;
    if (disp->has_flip_queue && sceKernelDeleteEqueue) {
        sceKernelDeleteEqueue(disp->flip_queue);
        disp->flip_queue = -1;
        disp->has_flip_queue = 0;
    }
    if (disp->handle > 0 && sceVideoOutClose) {
        (void)sceVideoOutClose(disp->handle);
        disp->handle = -1;
    }
    /* Release what open took: the scanout handle is gone, so the memory behind it
     * can go. */
    if (disp->has_memory) {
        if (sceKernelMunmap)
            (void)sceKernelMunmap(disp->mapped_base, disp->total_bytes);
        if (sceKernelReleaseDirectMemory)
            (void)sceKernelReleaseDirectMemory(disp->physical, disp->total_bytes);
        disp->mapped_base = 0;
        disp->physical = 0;
        disp->has_memory = 0;
    }
    disp->ready = 0;
}
