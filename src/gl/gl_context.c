/*
 * oops-gl: Context creation, destruction, and presentation
 */

#include "gl_internal.h"
#include "oops/agc.h"

gl_context_t *g_gl_ctx = NULL;

#ifdef OOPS_HOST_BUILD
#include <stdlib.h>
static gl_context_t s_host_ctx;
static float s_host_depth[1920 * 1080];
static inline __attribute__((unused)) void gl_klog_line(const char *msg) { (void)msg; }
static inline __attribute__((unused)) void gl_klog_val(const char *tag, uint64_t val) { (void)tag; (void)val; }
#else
#include "oops/syscall.h"
#endif

__attribute__((weak)) int sceKernelUsleep(unsigned int microseconds);
__attribute__((weak)) int sceAgcDriverSubmitCommandBuffer(void *queue, const void *dcb);
__attribute__((weak)) int sceAgcDriverSubmitDcb(const oops_agc_dcb_desc *desc);
__attribute__((weak)) int sceAgcDriverCreateQueue(uint32_t type, void *queue_out, uint32_t flags);

#ifndef OOPS_HOST_BUILD

static void gl_klog_line(const char *msg) {
    char buf[160];
    const char *prefix = "[OOPS-GL] ";
    int n = 0;
    while (prefix[n] && n < 16) { buf[n] = prefix[n]; n++; }
    int m = 0;
    while (msg[m] && n < (int)sizeof(buf) - 2) { buf[n++] = msg[m++]; }
    buf[n++] = '\n';
    buf[n] = '\0';
    (void)sys_call(SYS_klog, 7, (long)buf, 0, 0, 0, 0);
}

static void gl_klog_val(const char *tag, uint64_t val) {
    char buf[160];
    char hex[17];
    uint64_t v = val;
    for (int i = 15; i >= 0; i--) {
        uint8_t d = (uint8_t)(v & 0xf);
        hex[i] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
        v >>= 4;
    }
    hex[16] = '\0';
    int hstart = 0;
    while (hstart < 15 && hex[hstart] == '0') hstart++;

    int n = 0;
    const char *pfx = "[OOPS-GL] ";
    while (pfx[n] && n < 16) { buf[n] = pfx[n]; n++; }
    int m = 0;
    while (tag && tag[m] && n < 48) { buf[n++] = tag[m++]; }
    if (n < 52) { buf[n++] = ':'; buf[n++] = ' '; buf[n++] = '0'; buf[n++] = 'x'; }
    m = hstart;
    while (hex[m] && n < (int)sizeof(buf) - 3) { buf[n++] = hex[m++]; }
    buf[n++] = '\n';
    buf[n] = '\0';
    (void)sys_call(SYS_klog, 7, (long)buf, 0, 0, 0, 0);
}
#endif

void gl_hw_fail(gl_context_t *ctx, const char *reason) {
    if (!ctx || ctx->hw_failed) return;
    ctx->hw_failed = GL_TRUE;
    ctx->hw_failure = reason;
    gl_klog_line("HARDWARE FAILURE: drawing has stopped; nothing falls back to the CPU");
    gl_klog_line(reason);
}

#ifndef OOPS_HOST_BUILD
/* Eight words per line, "<tag> <index>: <w0> ... <w7>", for the oracle record. */
static void gl_klog_words(const char *tag, const uint32_t *w, uint32_t n) {
    static const char hexd[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i += 8u) {
        char buf[128];
        int p = 0;
        for (int t = 0; tag[t] && p < 16; t++) buf[p++] = tag[t];
        buf[p++] = ' ';
        for (int s = 12; s >= 0; s -= 4) buf[p++] = hexd[(i >> s) & 0xfu];
        buf[p++] = ':';
        for (uint32_t k = i; k < n && k < i + 8u; k++) {
            buf[p++] = ' ';
            for (int s = 28; s >= 0; s -= 4) buf[p++] = hexd[(w[k] >> s) & 0xfu];
        }
        buf[p++] = '\n';
        buf[p] = '\0';
        (void)sys_call(SYS_klog, 7, (long)buf, 0, 0, 0, 0);
    }
}

/* The oracle record: the whole command stream about to be submitted, the shader words it points
 * at, and the descriptor table, as they sit in memory. The fence and GPU clock follow after the
 * wait, and the caller logs the pixel hash of the result. */
static void gl_hw_dump_stream(const gl_context_t *ctx, uint32_t total_words) {
    const uint32_t *payload = (const uint32_t *)ctx->gpu_payload;
    gl_klog_line("oracle-begin");
    gl_klog_val("oracle-frame", ctx->frame_count);
    gl_klog_val("oracle-dcb-words", (uint64_t)total_words);
    gl_klog_val("oracle-payload-va", (uint64_t)(uintptr_t)ctx->gpu_payload);
    gl_klog_val("oracle-vbo-va", (uint64_t)(uintptr_t)ctx->vbo_mem);
    gl_klog_val("oracle-fence-va", (uint64_t)(uintptr_t)ctx->fence);
    gl_klog_val("oracle-canary-va", (uint64_t)(uintptr_t)ctx->canary);
    gl_klog_val("oracle-color-va", (uint64_t)(uintptr_t)ctx->framebuffer);
    gl_klog_val("oracle-depth-va", (uint64_t)(uintptr_t)ctx->depth_buffer);
    gl_klog_words("dcb", ctx->dcb_mem, total_words);
    gl_klog_words("vs", payload, 64u);              /* NGG vertex shader at payload + 0x000 */
    gl_klog_words("pstex", payload + 0x80u, 64u);   /* textured pixel shader at + 0x200 */
    gl_klog_words("ps", payload + 0xc0u, 64u);      /* untextured pixel shader at + 0x300 */
    gl_klog_words("desc", payload + 0x240u, 16u);   /* T# and S# at + 0x900 */
}

/* A GPU-only clear with no draw call, read back by the CPU. The CPU writes a sentinel first, so a
 * pixel that comes back in the clear colour can only have been written by the GPU; the stream
 * that clears it is the frame's register state, one CP DMA fill and the fence, nothing else. */
static void gl_hw_self_test(gl_context_t *ctx) {
    uint32_t *fb = ctx->framebuffer;
    size_t n = (size_t)ctx->width * (size_t)ctx->height;
    if (!fb || n == 0u) {
        gl_hw_fail(ctx, "no framebuffer to run the GPU clear test against");
        return;
    }
    for (size_t i = 0; i < n; i++) fb[i] = 0xff123456u;
#if defined(__x86_64__)
    for (size_t p = 0; p < n; p += 16) __builtin_ia32_clflush((const void *)&fb[p]);
#endif
    const uint32_t colour = 0xff204060u;
    ctx->hw_clear_colour = colour;
    ctx->hw_clear_expected = (uint32_t)n;
    gl_hw_clear(ctx, GL_COLOR_BUFFER_BIT, colour, 1.0f);
    gl_hw_flush(ctx);
#if defined(__x86_64__)
    for (size_t p = 0; p < n; p += 16) __builtin_ia32_clflush((const void *)&fb[p]);
#endif
    uint32_t matched = 0;
    for (size_t i = 0; i < n; i++) {
        if (fb[i] == colour) matched++;
    }
    ctx->hw_clear_matched = matched;
    ctx->hw_clear_verified = (matched == (uint32_t)n && !ctx->hw_failed) ? GL_TRUE : GL_FALSE;
    gl_klog_val("hw-clear-test-colour", (uint64_t)colour);
    gl_klog_val("hw-clear-test-matched", (uint64_t)matched);
    gl_klog_val("hw-clear-test-expected", (uint64_t)n);
    gl_klog_val("hw-clear-test-fence", (uint64_t)ctx->hw_last_fence);
    gl_klog_val("hw-clear-test-timestamp", ctx->hw_last_timestamp);
    gl_klog_val("hw-clear-test-pass", (uint64_t)ctx->hw_clear_verified);
    if (!ctx->hw_clear_verified) {
        gl_hw_fail(ctx, "the GPU-only clear did not survive readback");
    }
}
#else
static inline __attribute__((unused)) void gl_hw_dump_stream(const gl_context_t *ctx, uint32_t total_words) { (void)ctx; (void)total_words; }
#endif

void gl_hw_flush(gl_context_t *ctx) {
    if (!ctx || !ctx->use_hardware || !ctx->hw_frame_active || ctx->dcb_words == 0) {
        return;
    }

    /* Reset canary words before frame execution */
    if (ctx->canary) {
        volatile uint32_t *c = (volatile uint32_t *)ctx->canary;
        for (int i = 0; i < 8; i++) c[i] = 0xaaaaaaaa;
#if defined(__x86_64__)
        __builtin_ia32_clflush((const void *)ctx->canary);
#endif
    }

    uint32_t *dw = ctx->dcb_mem + ctx->dcb_words;
    uint64_t fence_gpu = (uint64_t)(uintptr_t)ctx->fence;

    /* Two end-of-pipe RELEASE_MEM events close the stream. The first writes the fence word, the
     * second the GPU's own clock counter. A submission counts as confirmed only when both come
     * back, and that measurement, not the code path taken, is what glIsHardwareAccelerated()
     * reports. */
    *dw++ = 0xc0064900u; /* RELEASE_MEM: CACHE_FLUSH_AND_INV_TS with GL2 writeback */
    *dw++ = 0x06603514u;
    *dw++ = 0x20000000u; /* DATA_SEL=1: the 32-bit word below */
    *dw++ = (uint32_t)fence_gpu;
    *dw++ = (uint32_t)(fence_gpu >> 32);
    *dw++ = 0xbeefcafeu;
    *dw++ = 0u;
    *dw++ = 0u;
    /* The CP waits for that fence, which the event writes only once the CB and DB caches are
     * flushed and L2 written back, then copies the finished render target into the CPU-cached
     * readback buffer. WAIT_REG_MEM: function EQUAL, memory space, polled by the ME. */
    if (ctx->readback && ctx->framebuffer) {
        *dw++ = 0xc0053c00u;
        *dw++ = 0x00000013u;
        *dw++ = (uint32_t)fence_gpu;
        *dw++ = (uint32_t)(fence_gpu >> 32);
        *dw++ = 0xbeefcafeu;
        *dw++ = 0xffffffffu;
        *dw++ = 4u;
        gl_hw_emit_dma_copy(&dw, (uint64_t)(uintptr_t)ctx->framebuffer, (uint64_t)(uintptr_t)ctx->readback,
                            ctx->width * ctx->height * 4u);
    }
    *dw++ = 0xc0064900u; /* RELEASE_MEM: the same event, DATA_SEL=3: the 64-bit GPU clock counter */
    *dw++ = 0x06603514u;
    *dw++ = 0x60000000u;
    *dw++ = (uint32_t)(fence_gpu + 8u);
    *dw++ = (uint32_t)((fence_gpu + 8u) >> 32);
    *dw++ = 0u;
    *dw++ = 0u;
    *dw++ = 0u;

    for (int p = 0; p < 16; p++) {
        dw[p] = 0xffff1000u;
    }
    dw += 16;

    uint32_t total_words = (uint32_t)(dw - ctx->dcb_mem);

    volatile uint32_t *fence_w = (volatile uint32_t *)ctx->fence;
    fence_w[0] = 0x11111111u;
    fence_w[2] = 0u;
    fence_w[3] = 0u;
#if defined(__x86_64__)
    __builtin_ia32_clflush((const void *)ctx->fence);
    for (size_t p = 0; p < (size_t)total_words * sizeof(uint32_t); p += 64) {
        __builtin_ia32_clflush((const void *)((const char *)ctx->dcb_mem + p));
    }
#endif

    if (ctx->hw_dump_pending) {
        gl_hw_dump_stream(ctx, total_words);
    }

    oops_agc_dcb_desc desc;
    desc.gpu_addr = (uint64_t)(uintptr_t)ctx->dcb_mem;
    desc.size = total_words;
    desc.flags = 0u;
    desc.pad = 0u;

    int rc = -1;
    if (sceAgcDriverSubmitCommandBuffer) {
        rc = sceAgcDriverSubmitCommandBuffer(ctx->agc_queue, &desc);
    } else if (sceAgcDriverSubmitDcb) {
        rc = sceAgcDriverSubmitDcb(&desc);
    }

    if (rc == 0) {
        for (int iter = 0; iter < 100000; iter++) {
#if defined(__x86_64__)
            __builtin_ia32_clflush((const void *)ctx->fence);
#endif
            if (fence_w[0] == 0xbeefcafeu && (fence_w[2] != 0u || fence_w[3] != 0u)) {
                break;
            }
            if (sceKernelUsleep) {
                sceKernelUsleep(10);
            }
        }
    }

    if (ctx->canary) {
#if defined(__x86_64__)
        __builtin_ia32_clflush((const void *)ctx->canary);
#endif
        volatile uint32_t *c = (volatile uint32_t *)ctx->canary;
        ctx->canary_vs = c[0];
        ctx->canary_ps = c[1];
        ctx->canary_vs_s0 = c[2];
        ctx->canary_ps_s0 = c[3];
    }

#if defined(__x86_64__)
    __builtin_ia32_clflush((const void *)ctx->fence);
#endif
    uint32_t fence_val = fence_w[0];
    uint64_t ts = ((uint64_t)fence_w[3] << 32) | (uint64_t)fence_w[2];
    int fence_hit = (fence_val == 0xbeefcafeu) ? 1 : 0;
    int ts_hit = (ts != 0u && ts > ctx->hw_prev_timestamp) ? 1 : 0;
    ctx->hw_last_fence = fence_val;
    ctx->hw_last_timestamp = ts;
    if (fence_hit && ts_hit) {
        ctx->hw_prev_timestamp = ts;
        ctx->hw_frames_confirmed++;
    }

    if (ctx->frame_count % 60 == 0 || ctx->frame_count < 5) {
        gl_klog_val("flush-words", (uint64_t)total_words);
        gl_klog_val("submit-rc", (uint64_t)(uint32_t)rc);
        gl_klog_val("fence-hit", (uint64_t)fence_hit);
        gl_klog_val("fence-val", (uint64_t)fence_val);
        gl_klog_val("gpu-timestamp", ts);
        gl_klog_val("timestamp-hit", (uint64_t)ts_hit);
        gl_klog_val("frames-confirmed", (uint64_t)ctx->hw_frames_confirmed);
        gl_klog_val("canary-vs", (uint64_t)ctx->canary_vs);
        gl_klog_val("canary-ps", (uint64_t)ctx->canary_ps);
    }

    if (ctx->hw_dump_pending) {
        gl_klog_val("oracle-submit-rc", (uint64_t)(uint32_t)rc);
        gl_klog_val("oracle-fence", (uint64_t)fence_val);
        gl_klog_val("oracle-timestamp", ts);
        gl_klog_line("oracle-end");
        ctx->hw_dump_pending = GL_FALSE;
    }

    if (rc != 0) {
        gl_hw_fail(ctx, "the driver refused the command buffer");
    } else if (!fence_hit) {
        gl_hw_fail(ctx, "the end-of-pipe fence never arrived");
    } else if (!ts_hit) {
        gl_hw_fail(ctx, "the GPU clock at end of pipe is missing or did not advance");
    }

    ctx->hw_frame_active = GL_FALSE;
    ctx->dcb_words = 0;
    ctx->triangles_drawn = 0;
}

void *glContextCreate(struct oops_display *disp) {
    if (!disp) return NULL;

    unsigned int w = oops_display_get_width(disp);
    unsigned int h = oops_display_get_height(disp);
    if (!w || !h) {
        w = 1920;
        h = 1080;
    }

    gl_context_t *ctx = NULL;
    float *depth = NULL;

#ifndef OOPS_HOST_BUILD
    ctx = (gl_context_t *)oops_mem_alloc(sizeof(gl_context_t), 64, OOPS_MEM_WB_ONION);
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));

    /* The hardware depth surface is 64KB_Z_X tiled: 128 x 128 px blocks, so the allocation covers
     * the padded extent (1920 x 1080 needs 15 x 9 blocks = 1920 x 1152 floats). */
    size_t depth_px = (size_t)((w + 127u) & ~127u) * (size_t)((h + 127u) & ~127u);
    depth = (float *)oops_mem_alloc(depth_px * sizeof(float), 64 * 1024, OOPS_MEM_WC_GARLIC);
    if (!depth) {
        oops_mem_free(ctx);
        return NULL;
    }
#else
    ctx = &s_host_ctx;
    memset(ctx, 0, sizeof(*ctx));
    depth = s_host_depth;
    size_t depth_px = (size_t)w * (size_t)h;
#endif

    ctx->disp = disp;
    ctx->framebuffer = oops_display_get_framebuffer(disp);
    ctx->width = w;
    ctx->height = h;
    ctx->depth_buffer = depth;
    ctx->depth_px = depth_px;

    /* Viewport & Scissor defaults */
    ctx->vp_x = 0;
    ctx->vp_y = 0;
    ctx->vp_w = (GLsizei)w;
    ctx->vp_h = (GLsizei)h;

    ctx->sc_x = 0;
    ctx->sc_y = 0;
    ctx->sc_w = (GLsizei)w;
    ctx->sc_h = (GLsizei)h;

    /* Clear values */
    ctx->clear_color[0] = 0.0f;
    ctx->clear_color[1] = 0.0f;
    ctx->clear_color[2] = 0.0f;
    ctx->clear_color[3] = 1.0f;
    ctx->clear_depth = 1.0f;

    /* State defaults */
    ctx->cap_depth_test = GL_FALSE;
    ctx->cap_cull_face = GL_FALSE;
    ctx->cap_blend = GL_FALSE;
    ctx->cap_scissor_test = GL_FALSE;
    ctx->cap_lighting = GL_FALSE;
    ctx->cap_texture_2d = GL_FALSE;

    ctx->depth_func = GL_LESS;
    ctx->depth_mask = GL_TRUE;
    ctx->blend_src = GL_SRC_ALPHA;
    ctx->blend_dst = GL_ONE_MINUS_SRC_ALPHA;
    ctx->blend_src_alpha = GL_SRC_ALPHA;
    ctx->blend_dst_alpha = GL_ONE_MINUS_SRC_ALPHA;
    ctx->blend_equation = GL_FUNC_ADD;
    ctx->tex_env_mode = GL_MODULATE;
    ctx->perspective_hint = GL_DONT_CARE; /* the specification's default */
    ctx->tex_env_color[0] = 0.0f;
    ctx->tex_env_color[1] = 0.0f;
    ctx->tex_env_color[2] = 0.0f;
    ctx->tex_env_color[3] = 0.0f;
    ctx->cull_mode = GL_BACK;
    ctx->front_face = GL_CCW;
    ctx->shade_model = GL_SMOOTH;

    ctx->color_mask[0] = GL_TRUE;
    ctx->color_mask[1] = GL_TRUE;
    ctx->color_mask[2] = GL_TRUE;
    ctx->color_mask[3] = GL_TRUE;

    /* Matrices */
    ctx->matrix_mode = GL_MODELVIEW;
    /* The specification's defaults, set explicitly: a zeroed context would read as alignment 1,
     * which silently misplaces every row after the first for any caller that relies on the
     * default of 4 - which is most of them, because most never call glPixelStorei at all. */
    ctx->unpack_alignment = 4;
    ctx->unpack_row_length = 0;
    ctx->pack_alignment = 4;
    ctx->depth_near = 0.0f;
    ctx->depth_far = 1.0f;
    ctx->alpha_func = GL_ALWAYS; /* the specification's default, which is no test at all */
    ctx->alpha_ref = 0.0f;
    ctx->modelview_depth = 0;
    ctx->projection_depth = 0;
    ctx->texture_depth = 0;
    mat4_identity(&ctx->modelview_stack[0]);
    mat4_identity(&ctx->projection_stack[0]);
    mat4_identity(&ctx->texture_stack[0]);
    ctx->mvp_dirty = GL_TRUE;

    /* Current immediate mode attributes */
    ctx->cur_color[0] = 1.0f;
    ctx->cur_color[1] = 1.0f;
    ctx->cur_color[2] = 1.0f;
    ctx->cur_color[3] = 1.0f;
    ctx->cur_normal[0] = 0.0f;
    ctx->cur_normal[1] = 0.0f;
    ctx->cur_normal[2] = 1.0f;
    ctx->cur_texcoord[0] = 0.0f;
    ctx->cur_texcoord[1] = 0.0f;

    ctx->last_error = GL_NO_ERROR;

    /* Lighting defaults */
    ctx->cap_lighting = GL_FALSE;
    ctx->cap_normalize = GL_FALSE;
    ctx->cap_color_material = GL_FALSE;
    ctx->color_material_face = GL_FRONT_AND_BACK;
    ctx->color_material_mode = GL_AMBIENT_AND_DIFFUSE;
    ctx->light_model_ambient[0] = 0.2f;
    ctx->light_model_ambient[1] = 0.2f;
    ctx->light_model_ambient[2] = 0.2f;
    ctx->light_model_ambient[3] = 1.0f;
    ctx->light_model_local_viewer = GL_FALSE;
    ctx->light_model_two_side = GL_FALSE;

    gl_material_t default_mat;
    default_mat.ambient[0] = 0.2f; default_mat.ambient[1] = 0.2f; default_mat.ambient[2] = 0.2f; default_mat.ambient[3] = 1.0f;
    default_mat.diffuse[0] = 0.8f; default_mat.diffuse[1] = 0.8f; default_mat.diffuse[2] = 0.8f; default_mat.diffuse[3] = 1.0f;
    default_mat.specular[0] = 0.0f; default_mat.specular[1] = 0.0f; default_mat.specular[2] = 0.0f; default_mat.specular[3] = 1.0f;
    default_mat.emission[0] = 0.0f; default_mat.emission[1] = 0.0f; default_mat.emission[2] = 0.0f; default_mat.emission[3] = 1.0f;
    default_mat.shininess = 0.0f;
    ctx->mat_front = default_mat;
    ctx->mat_back = default_mat;

    for (int i = 0; i < OOPS_GL_LIGHT_COUNT; i++) {
        ctx->lights[i].enabled = GL_FALSE;
        ctx->lights[i].ambient[0] = 0.0f; ctx->lights[i].ambient[1] = 0.0f;
        ctx->lights[i].ambient[2] = 0.0f; ctx->lights[i].ambient[3] = 1.0f;
        float diff_val = (i == 0) ? 1.0f : 0.0f;
        ctx->lights[i].diffuse[0] = diff_val; ctx->lights[i].diffuse[1] = diff_val;
        ctx->lights[i].diffuse[2] = diff_val; ctx->lights[i].diffuse[3] = 1.0f;
        ctx->lights[i].specular[0] = diff_val; ctx->lights[i].specular[1] = diff_val;
        ctx->lights[i].specular[2] = diff_val; ctx->lights[i].specular[3] = 1.0f;
        ctx->lights[i].position[0] = 0.0f; ctx->lights[i].position[1] = 0.0f;
        ctx->lights[i].position[2] = 1.0f; ctx->lights[i].position[3] = 0.0f;
        ctx->lights[i].spot_direction[0] = 0.0f; ctx->lights[i].spot_direction[1] = 0.0f; ctx->lights[i].spot_direction[2] = -1.0f;
        ctx->lights[i].spot_exponent = 0.0f;
        ctx->lights[i].spot_cutoff = 180.0f;
        ctx->lights[i].spot_cutoff_cos = -1.0f;
        ctx->lights[i].const_att = 1.0f;
        ctx->lights[i].linear_att = 0.0f;
        ctx->lights[i].quad_att = 0.0f;
    }

    ctx->normal_matrix[0] = 1.0f; ctx->normal_matrix[4] = 1.0f; ctx->normal_matrix[8] = 1.0f;
    ctx->normal_matrix_dirty = GL_FALSE;

#ifndef OOPS_HOST_BUILD
    /* Initialize AGC Universal Graphics Queue if available */
    void *queue = NULL;
    int rc_q = -1;
    gl_klog_line("creating AGC universal queue...");
    if (sceAgcDriverCreateQueue) {
        rc_q = sceAgcDriverCreateQueue(0u, &queue, 0u);
    }
    gl_klog_val("sceAgcDriverCreateQueue rc", (uint64_t)(uint32_t)rc_q);
    if (rc_q == 0 && queue != NULL) {
        ctx->agc_queue = queue;
        ctx->gpu_payload = oops_mem_alloc(0x4000, 256, OOPS_MEM_WB_ONION);
        ctx->vbo_mem = oops_mem_alloc(65536, 256, OOPS_MEM_WB_ONION);
        ctx->fence = oops_mem_alloc(0x1000, 0x1000, OOPS_MEM_WB_ONION);
        ctx->canary = oops_mem_alloc(0x1000, 0x1000, OOPS_MEM_WB_ONION);
        ctx->readback = (uint32_t *)oops_mem_alloc((size_t)w * (size_t)h * 4u, 0x1000, OOPS_MEM_WB_ONION);
        ctx->dcb_capacity_dw = 65536;
        ctx->dcb_words = 0;
        ctx->hw_frame_active = GL_FALSE;
        ctx->dcb_mem = (uint32_t *)oops_mem_alloc(ctx->dcb_capacity_dw * sizeof(uint32_t),
                                                  0x1000, OOPS_MEM_WB_ONION);
        if (ctx->gpu_payload && ctx->vbo_mem && ctx->fence && ctx->canary && ctx->dcb_mem) {
            *(volatile uint32_t *)ctx->fence = 0x11111111u;

            /* 1. Dynamic 3D RDNA2 NGG Vertex Shader with Color (param0) and UV (param1) Export */
            uint64_t canary_gpu = (uint64_t)(uintptr_t)ctx->canary;
            uint64_t vbo_gpu = (uint64_t)(uintptr_t)ctx->vbo_mem;
            uint32_t *vs = (uint32_t *)ctx->gpu_payload;

            /* The NGG sequence is the one the open-source AMD stack emits for a primitive shader (Mesa,
             * ac_nir_lower_ngg: GS_ALLOC_REQ with m0 = vertices | primitives << 12, the primitive
             * export word with the vertex indices at bits 0, 10 and 20, and a wait for the export
             * counter before exec changes), following the RDNA ISA reference on s_sendmsg and export
             * ordering. Allocate, export the primitive from the GS thread, wait, then fetch and export
             * positions from the ES threads; confirmed on the console 2026-09-14 (D005). */
            vs[ 0] = 0xbfa00001u; /* s_inst_prefetch 0x1 */
            vs[ 1] = 0xbe8c037eu; /* s_mov_b32 s12, exec_lo (save the entry exec mask) */
            vs[ 2] = 0xbefc03ffu; /* s_mov_b32 m0, 0x1003: one primitive of three vertices per wave */
            vs[ 3] = 0x00001003u;
            vs[ 4] = 0xbf800000u; /* s_nop 0: an SALU write of m0 needs one wait state before s_sendmsg reads it */
            vs[ 5] = 0xbf900009u; /* s_sendmsg sendmsg(MSG_GS_ALLOC_REQ) */
            vs[ 6] = 0xbefe0381u; /* s_mov_b32 exec_lo, 1 (the primitive thread) */
            vs[ 7] = 0x7e0202ffu; /* v_mov_b32 v1, 0x20280600 (vertices 0, 1, 2 with edge flags) */
            vs[ 8] = 0x20280600u;
            vs[ 9] = 0xf8000941u; /* exp prim, v1, off, off, off done */
            vs[10] = 0x00000001u;
            vs[11] = 0xbf8cff0fu; /* s_waitcnt expcnt(0): the export must retire before exec changes */
            vs[12] = 0xbefe0387u; /* s_mov_b32 exec_lo, 7 (the three vertex threads) */
            vs[13] = 0xd765000eu; /* v_mbcnt_lo_u32_b32 v14, -1, 0 (lane index = vertex index) */
            vs[14] = 0x000100c1u;
            vs[15] = 0x341e1c84u; /* v_lshlrev_b32 v15, 4, v14 (lane * 16) */
            vs[16] = 0x341c1c85u; /* v_lshlrev_b32 v14, 5, v14 (lane * 32) */
            vs[17] = 0x4a1c1d0fu; /* v_add_nc_u32 v14, v15, v14 (lane * 48) */
            vs[18] = 0x4a1c1c08u; /* v_add_nc_u32 v14, s8, v14 (+ vbo_offset: user SGPR 0 arrives in s8 on this hardware) */
            vs[19] = 0xbe8203ffu; /* s_mov_b32 s2, vbo_lo */
            vs[20] = (uint32_t)vbo_gpu;
            vs[21] = 0xbe8303ffu; /* s_mov_b32 s3, vbo_hi */
            vs[22] = (uint32_t)(vbo_gpu >> 32);
            vs[23] = 0x7e020280u; /* v_mov_b32 v1, 0 */
            vs[24] = 0xd70f6a12u; /* v_add_co_u32 v18, vcc_lo, s2, v14 */
            vs[25] = 0x00021c02u;
            vs[26] = 0x50260203u; /* v_add_co_ci_u32 v19, vcc_lo, s3, v1, vcc_lo */
            vs[27] = 0xdc388000u; /* global_load_dwordx4 v[2:5], v[18:19], off offset:0 (pos) */
            vs[28] = 0x027d0012u;
            vs[29] = 0xdc388010u; /* global_load_dwordx4 v[10:13], v[18:19], off offset:16 (color) */
            vs[30] = 0x0a7d0012u;
            vs[31] = 0xdc388020u; /* global_load_dwordx4 v[6:9], v[18:19], off offset:32 (uv) */
            vs[32] = 0x067d0012u;
            vs[33] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
            vs[34] = 0xbe8603ffu; /* s_mov_b32 s6, canary_lo */
            vs[35] = (uint32_t)canary_gpu;
            vs[36] = 0xbe8703ffu; /* s_mov_b32 s7, canary_hi */
            vs[37] = (uint32_t)(canary_gpu >> 32);
            /* Diagnostic: each vertex lane stores its fetched pos.x to canary[2 + lane]. */
            vs[38] = 0x2c281e82u; /* v_lshrrev_b32 v20, 2, v15 (lane * 4) */
            vs[39] = 0xd70f6a12u; /* v_add_co_u32 v18, vcc_lo, s6, v20 */
            vs[40] = 0x00022806u;
            vs[41] = 0x50260207u; /* v_add_co_ci_u32 v19, vcc_lo, s7, v1, vcc_lo (v1 = 0) */
            vs[42] = 0xdc708008u; /* global_store_dword v[18:19], v2, off offset:8 */
            vs[43] = 0x007d0212u;
            vs[44] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
            vs[45] = 0xf800020fu; /* exp param0, v10, v11, v12, v13 (Color) */
            vs[46] = 0x0d0c0b0au;
            vs[47] = 0xf800021fu; /* exp param1, v6, v7, v8, v9 (UV) */
            vs[48] = 0x09080706u;
            vs[49] = 0xf80008cfu; /* exp pos0, v2, v3, v4, v5 done (Pos) */
            vs[50] = 0x05040302u;
            vs[51] = 0xbf8cff0fu; /* s_waitcnt expcnt(0) */
            vs[52] = 0xbefe0381u; /* s_mov_b32 exec_lo, 1 */
            vs[53] = 0x7e240206u; /* v_mov_b32 v18, s6 */
            vs[54] = 0x7e260207u; /* v_mov_b32 v19, s7 */
            vs[55] = 0x7e0202ffu; /* v_mov_b32 v1, 0xbeef0001 */
            vs[56] = 0xbeef0001u;
            vs[57] = 0xdc708000u; /* global_store_dword v[18:19], v1, off offset:0 */
            vs[58] = 0x007d0112u;
            vs[59] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
            vs[60] = 0xbefe030cu; /* s_mov_b32 exec_lo, s12 */
            vs[61] = 0xbf810000u; /* s_endpgm */
            for (size_t p = 62; p < 128; p++) vs[p] = 0xbf800000u;

            /* The NGG stage table points both the ES and GS program slots at this shader (offset 0). */

            /* 2. Hardware Textured + Gouraud Pixel Shader (offset 0x200) */
            uint32_t *ps_tex = (uint32_t *)((char *)ctx->gpu_payload + 0x200);
            ps_tex[ 0] = 0xbf8c0000u; /* s_waitcnt 0 */
            ps_tex[ 1] = 0xbefc0302u; /* s_mov_b32 m0, s2: the primitive mask follows the two user SGPRs */
            ps_tex[ 2] = 0xbe84037eu; /* s_mov_b32 s4, exec_lo */
            ps_tex[ 3] = 0xbefe0381u; /* s_mov_b32 exec_lo, 1 */
            ps_tex[ 4] = 0xbe8203ffu; /* s_mov_b32 s2, canary_lo */
            ps_tex[ 5] = (uint32_t)canary_gpu;
            ps_tex[ 6] = 0xbe8303ffu; /* s_mov_b32 s3, canary_hi */
            ps_tex[ 7] = (uint32_t)(canary_gpu >> 32);
            ps_tex[ 8] = 0x7e100202u; /* v_mov_b32 v8, s2 */
            ps_tex[ 9] = 0x7e120203u; /* v_mov_b32 v9, s3 */
            ps_tex[10] = 0x7e1402ffu; /* v_mov_b32 v10, 0xbeef0002 */
            ps_tex[11] = 0xbeef0002u;
            ps_tex[12] = 0xdc708004u; /* global_store_dword v[8:9], v10, off offset:4 */
            ps_tex[13] = 0x007d0a08u;
            ps_tex[14] = 0xbefe0304u; /* s_mov_b32 exec_lo, s4 */
            ps_tex[15] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
            ps_tex[16] = 0xc8080400u; /* v_interp_p1_f32 v2, v0, attr1.x (U) */
            ps_tex[17] = 0xc8090401u; /* v_interp_p2_f32 v2, v1, attr1.x */
            ps_tex[18] = 0xc80c0500u; /* v_interp_p1_f32 v3, v0, attr1.y (V) */
            ps_tex[19] = 0xc80d0501u; /* v_interp_p2_f32 v3, v1, attr1.y */
            ps_tex[20] = 0xc8200000u; /* v_interp_p1_f32 v8, v0, attr0.x (R) */
            ps_tex[21] = 0xc8210001u; /* v_interp_p2_f32 v8, v1, attr0.x */
            ps_tex[22] = 0xc8240100u; /* v_interp_p1_f32 v9, v0, attr0.y (G) */
            ps_tex[23] = 0xc8250101u; /* v_interp_p2_f32 v9, v1, attr0.y */
            ps_tex[24] = 0xc8280200u; /* v_interp_p1_f32 v10, v0, attr0.z (B) */
            ps_tex[25] = 0xc8290201u; /* v_interp_p2_f32 v10, v1, attr0.z */
            ps_tex[26] = 0xc82c0300u; /* v_interp_p1_f32 v11, v0, attr0.w (A) */
            ps_tex[27] = 0xc82d0301u; /* v_interp_p2_f32 v11, v1, attr0.w */
            ps_tex[28] = 0xf40c0100u; /* s_load_dwordx8 s[4:11], s[0:1], 0x00 */
            ps_tex[29] = 0xfa000000u;
            ps_tex[30] = 0xf4080300u; /* s_load_dwordx4 s[12:15], s[0:1], 0x20 */
            ps_tex[31] = 0xfa000020u;
            ps_tex[32] = 0xbf8cc07fu; /* s_waitcnt lgkmcnt(0) */
            ps_tex[33] = 0xf09c0f08u; /* image_sample_lz v[4:7], v[2:3], s[4:11], s[12:15] dmask:0xf dim:SQ_RSRC_IMG_2D */
            ps_tex[34] = 0x00610402u;
            ps_tex[35] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
            ps_tex[36] = 0x10081104u; /* v_mul_f32 v4, v4, v8 (R * R) */
            ps_tex[37] = 0x100a1305u; /* v_mul_f32 v5, v5, v9 (G * G) */
            ps_tex[38] = 0x100c1506u; /* v_mul_f32 v6, v6, v10 (B * B) */
            ps_tex[39] = 0x100e1707u; /* v_mul_f32 v7, v7, v11 (A * A) */
            /* 40..43: the alpha test, as in the untextured shader. It goes *after* the texture
             * multiply, so the alpha tested is the one that reaches the framebuffer rather than
             * the interpolated one before modulation - which is what GL specifies and is the
             * difference a texture with an alpha channel makes visible. */
            for (size_t p = GL_PS_ALPHA_SLOT_TEX; p < GL_PS_ALPHA_SLOT_TEX + 4u; p++) {
                ps_tex[p] = 0xbf800000u; /* s_nop 0 */
            }
            ps_tex[44] = 0xf800180fu; /* exp mrt0, v4, v5, v6, v7 done vm */
            ps_tex[45] = 0x07060504u;
            ps_tex[46] = 0xbf810000u; /* s_endpgm */
            for (size_t p = 47; p < 64; p++) ps_tex[p] = 0xbf800000u;

            /* 3. Hardware Gouraud Barycentric Interpolating Pixel Shader (untextured, offset 0x300) */
            uint32_t *ps_untex = (uint32_t *)((char *)ctx->gpu_payload + 0x300);
            ps_untex[ 0] = 0xbf8c0000u; /* s_waitcnt 0 */
            ps_untex[ 1] = 0xbefc0300u; /* s_mov_b32 m0, s0: the SPI hands the primitive mask to the SGPR after the user data; the interpolator reads it from m0 */
            ps_untex[ 2] = 0xbe84037eu; /* s_mov_b32 s4, exec_lo */
            ps_untex[ 3] = 0xbefe0381u; /* s_mov_b32 exec_lo, 1 */
            ps_untex[ 4] = 0xbe8003ffu; /* s_mov_b32 s0, canary_lo */
            ps_untex[ 5] = (uint32_t)canary_gpu;
            ps_untex[ 6] = 0xbe8103ffu; /* s_mov_b32 s1, canary_hi */
            ps_untex[ 7] = (uint32_t)(canary_gpu >> 32);
            ps_untex[ 8] = 0x7e100200u; /* v_mov_b32 v8, s0 */
            ps_untex[ 9] = 0x7e120201u; /* v_mov_b32 v9, s1 */
            ps_untex[10] = 0x7e1402ffu; /* v_mov_b32 v10, 0xbeef0002 */
            ps_untex[11] = 0xbeef0002u;
            ps_untex[12] = 0xdc708004u; /* global_store_dword v[8:9], v10, off offset:4 */
            ps_untex[13] = 0x007d0a08u;
            ps_untex[14] = 0xbefe0304u; /* s_mov_b32 exec_lo, s4 */
            ps_untex[15] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
            ps_untex[16] = 0xc8100000u; /* v_interp_p1_f32 v4, v0, attr0.x (R) */
            ps_untex[17] = 0xc8110001u; /* v_interp_p2_f32 v4, v1, attr0.x */
            ps_untex[18] = 0xc8140100u; /* v_interp_p1_f32 v5, v0, attr0.y (G) */
            ps_untex[19] = 0xc8150101u; /* v_interp_p2_f32 v5, v1, attr0.y */
            ps_untex[20] = 0xc8180200u; /* v_interp_p1_f32 v6, v0, attr0.z (B) */
            ps_untex[21] = 0xc8190201u; /* v_interp_p2_f32 v6, v1, attr0.z */
            ps_untex[22] = 0xc81c0300u; /* v_interp_p1_f32 v7, v0, attr0.w (A) */
            ps_untex[23] = 0xc81d0301u; /* v_interp_p2_f32 v7, v1, attr0.w */
            /* 24..27: the alpha test, patched in place by gl_ps_patch_alpha_test. Four words,
             * which is exactly what the longest form needs (a literal load is two). Left as
             * s_nop here, which is what "no alpha test" is. */
            for (size_t p = GL_PS_ALPHA_SLOT_UNTEX; p < GL_PS_ALPHA_SLOT_UNTEX + 4u; p++) {
                ps_untex[p] = 0xbf800000u; /* s_nop 0 */
            }
            ps_untex[28] = 0xf800180fu; /* exp mrt0, v4, v5, v6, v7 done vm */
            ps_untex[29] = 0x07060504u;
            ps_untex[30] = 0xbf810000u; /* s_endpgm */
            for (size_t p = 31; p < 64; p++) ps_untex[p] = 0xbf800000u;

            /* 4. Initialize active texture descriptor table at 0x900 */
            uint32_t *desc_table = (uint32_t *)((char *)ctx->gpu_payload + 0x900);
            memset(desc_table, 0, 64);
            desc_table[1] = 56u << 20; /* FORMAT = 56 (FMT_8_8_8_8_UNORM) */
            desc_table[2] = (1u << 31); /* RESOURCE_LEVEL = 1 */
            desc_table[3] = 0x90000facu; /* SQ_RSRC_IMG_2D, linear, DST_SEL = channels 0..3 */
            desc_table[9] = 0x00fff000u; /* Sampler MAX_LOD */

#if defined(__x86_64__)
            for (size_t p = 0; p < 0x1000; p += 64) {
                __builtin_ia32_clflush((const void *)((const char *)ctx->gpu_payload + p));
            }
#endif
            ctx->use_hardware = GL_TRUE;
            gl_klog_line("hardware AGC RDNA2 pipeline initialized successfully");
            gl_hw_self_test(ctx);
        }
    }
#endif

    g_gl_ctx = ctx;
    return (void *)ctx;
}

void glContextDestroy(void *ctx_handle) {
    gl_context_t *ctx = (gl_context_t *)ctx_handle;
    if (!ctx) return;

    /* Before the branch below, because the target arm frees `ctx` itself at the end of it and
     * the buffer storage has to go first. */
    gl_free_all_buffers(ctx);

#ifndef OOPS_HOST_BUILD
    if (ctx->agc_queue && sceAgcDriverDestroyQueue) {
        sceAgcDriverDestroyQueue(ctx->agc_queue);
        ctx->agc_queue = NULL;
    }
    for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
        if (ctx->textures[i].garlic_data) {
            oops_mem_free(ctx->textures[i].garlic_data);
            ctx->textures[i].garlic_data = NULL;
        }
    }
    if (ctx->gpu_payload) oops_mem_free(ctx->gpu_payload);
    if (ctx->readback) oops_mem_free(ctx->readback);
    if (ctx->fence) oops_mem_free(ctx->fence);
    if (ctx->canary) oops_mem_free(ctx->canary);
    if (ctx->dcb_mem) oops_mem_free(ctx->dcb_mem);
    if (ctx->depth_buffer) oops_mem_free(ctx->depth_buffer);
    oops_mem_free(ctx);
#else
    for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
        if (ctx->textures[i].pixels) {
            free(ctx->textures[i].pixels);
            ctx->textures[i].pixels = NULL;
        }
    }
#endif

    if (g_gl_ctx == ctx) {
        g_gl_ctx = NULL;
    }
}

void glContextMakeCurrent(void *ctx_handle) {
    g_gl_ctx = (gl_context_t *)ctx_handle;
}

void *glGetCurrentContext(void) {
    return (void *)g_gl_ctx;
}

void glSwapBuffers(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !ctx->disp) return;

    /* End any open immediate mode */
    if (ctx->imm_active) {
        glEnd();
    }

#ifndef OOPS_HOST_BUILD
    /* Flush hardware rendering before flipping */
    if (ctx->use_hardware && ctx->hw_frame_active) {
        gl_hw_flush(ctx);
        if (ctx->frame_count % 60 == 0 || ctx->frame_count < 5) {
            (void)sys_call(SYS_klog, 7, (long)"[OOPS-GL] AGC hardware frame rendered and flipped\n", 0, 0, 0, 0);
        }
    }
#endif

    /* Flip display buffer */
    oops_display_flip(ctx->disp);

    /* Update pointer to active backbuffer */
    ctx->framebuffer = oops_display_get_framebuffer(ctx->disp);
    ctx->frame_count++;
}

void glFlush(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (ctx->imm_active) {
        glEnd();
    }
#ifndef OOPS_HOST_BUILD
    if (ctx->use_hardware && ctx->hw_frame_active) {
        gl_hw_flush(ctx);
    }
#endif
}

void glFinish(void) {
    glFlush();
}

void glGetCanary(GLuint *vs_canary, GLuint *ps_canary) {
    gl_context_t *ctx = gl_get_ctx();
    if (vs_canary) *vs_canary = ctx ? ctx->canary_vs : 0;
    if (ps_canary) *ps_canary = ctx ? ctx->canary_ps : 0;
}

void glGetCanaryEx(GLuint *vs_canary, GLuint *ps_canary, GLuint *vs_s0, GLuint *ps_s0) {
    gl_context_t *ctx = gl_get_ctx();
    if (vs_canary) *vs_canary = ctx ? ctx->canary_vs : 0;
    if (ps_canary) *ps_canary = ctx ? ctx->canary_ps : 0;
    if (vs_s0) *vs_s0 = ctx ? ctx->canary_vs_s0 : 0;
    if (ps_s0) *ps_s0 = ctx ? ctx->canary_ps_s0 : 0;
}

/* Writes the alpha test into both pixel shaders, or takes it out.
 *
 * # The encodings are assembled, not remembered
 *
 * Every word below came from assembling the instruction in its comment for gfx1030 and reading
 * the object back:
 *
 *     clang -target amdgcn-amd-amdhsa -mcpu=gfx1030 -c alpha.s
 *
 * That matters because this repository has no way to notice a wrong instruction encoding: it
 * would assemble into the payload, the GPU would do something else, and the result would be a
 * frame that is wrong rather than a build that fails. Two of the results are cross-checks
 * against words already in the tree and agree with them - `s_endpgm` came out `0xbf810000`, and
 * the literal-load form `0x7e18_02ff` has the same `0xff` source marker as the canary load the
 * untextured shader already does.
 *
 * # Why patched rather than rebuilt
 *
 * The shaders are laid into the GPU payload at context creation, along with the descriptor
 * table, and the payload is flushed from the CPU's caches once. Rebuilding a shader would mean
 * redoing that; patching four words in place does not, and the four words are enough for every
 * form the test takes.
 *
 * # Wave32
 *
 * The mask registers are the `_lo` halves, because this runs wave32 - `VGT_SHADER_STAGES_EN`
 * sets `GS_W32` and `VS_W32`. A wave64 build would need `vcc` and `exec` instead, and the
 * encodings would differ.
 */
void gl_ps_patch_alpha_test(gl_context_t *ctx) {
    if (!ctx || !ctx->gpu_payload) return;

    uint32_t words[4] = {
        0xbf800000u, /* s_nop 0 */
        0xbf800000u,
        0xbf800000u,
        0xbf800000u,
    };

    if (ctx->cap_alpha_test && ctx->alpha_func != GL_ALWAYS) {
        if (ctx->alpha_func == GL_NEVER) {
            /* Every fragment fails, so kill the whole wave's lanes and let the export write
             * nothing. */
            words[0] = 0xbefe0380u; /* s_mov_b32 exec_lo, 0 */
        } else {
            uint32_t compare;
            switch (ctx->alpha_func) {
                case GL_LESS:     compare = 0x7c021907u; break; /* v_cmp_lt_f32  vcc_lo, v7, v12 */
                case GL_EQUAL:    compare = 0x7c041907u; break; /* v_cmp_eq_f32  vcc_lo, v7, v12 */
                case GL_LEQUAL:   compare = 0x7c061907u; break; /* v_cmp_le_f32  vcc_lo, v7, v12 */
                case GL_GREATER:  compare = 0x7c081907u; break; /* v_cmp_gt_f32  vcc_lo, v7, v12 */
                case GL_NOTEQUAL: compare = 0x7c1a1907u; break; /* v_cmp_neq_f32 vcc_lo, v7, v12 */
                case GL_GEQUAL:   compare = 0x7c0c1907u; break; /* v_cmp_ge_f32  vcc_lo, v7, v12 */
                default:
                    /* glAlphaFunc refused it, so the context cannot hold it. Leaving the nops
                     * in place is the safe reading of an impossible state: no test at all. */
                    compare = 0u;
                    break;
            }
            if (compare != 0u) {
                words[0] = 0x7e1802ffu;              /* v_mov_b32 v12, <literal> */
                words[1] = gl_f32_bits(ctx->alpha_ref);
                words[2] = compare;
                words[3] = 0x877e6a7eu;              /* s_and_b32 exec_lo, exec_lo, vcc_lo */
            }
        }
    }

    uint32_t *ps_untex = (uint32_t *)((char *)ctx->gpu_payload + 0x300);
    uint32_t *ps_tex = (uint32_t *)((char *)ctx->gpu_payload + 0x200);
    for (size_t i = 0; i < 4; i++) {
        ps_untex[GL_PS_ALPHA_SLOT_UNTEX + i] = words[i];
        ps_tex[GL_PS_ALPHA_SLOT_TEX + i] = words[i];
    }

#if !defined(OOPS_HOST_BUILD) && defined(__x86_64__)
    /* The payload is write-combined; the command processor reads what has left this core. */
    for (size_t p = 0x200; p < 0x400; p += 64) {
        __builtin_ia32_clflush((const void *)((const char *)ctx->gpu_payload + p));
    }
#endif
}

GLboolean glIsHardwareAccelerated(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !ctx->use_hardware || ctx->hw_failed) return GL_FALSE;
    /* Measured, not chosen: the clear test passed and the last submission's fence and GPU
     * clock both came back. */
    return (ctx->hw_clear_verified && ctx->hw_last_fence == 0xbeefcafeu && ctx->hw_frames_confirmed > 0u)
               ? GL_TRUE : GL_FALSE;
}

void glGetHardwareStatus(gl_hw_status_t *out) {
    gl_context_t *ctx = gl_get_ctx();
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!ctx) return;
    out->verified = glIsHardwareAccelerated();
    out->failed = ctx->hw_failed;
    out->failure = ctx->hw_failed ? ctx->hw_failure : NULL;
    out->fence = ctx->hw_last_fence;
    out->timestamp_lo = (GLuint)(ctx->hw_last_timestamp & 0xffffffffu);
    out->timestamp_hi = (GLuint)(ctx->hw_last_timestamp >> 32);
    out->frames_confirmed = ctx->hw_frames_confirmed;
    out->clear_colour = ctx->hw_clear_colour;
    out->clear_matched = ctx->hw_clear_matched;
    out->clear_expected = ctx->hw_clear_expected;
}

void glRequestHardwareDump(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (ctx) ctx->hw_dump_pending = GL_TRUE;
}

void glSetHardwarePrelude(const GLuint *words, GLuint count) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->hw_prelude = (words && count) ? (const uint32_t *)words : NULL;
    ctx->hw_prelude_words = ctx->hw_prelude ? (uint32_t)count : 0u;
}

const GLuint *glGetFrameReadback(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !ctx->readback || ctx->hw_frames_confirmed == 0u) return NULL;
#if defined(__x86_64__)
    size_t bytes = (size_t)ctx->width * (size_t)ctx->height * 4u;
    for (size_t p = 0; p < bytes; p += 64) __builtin_ia32_clflush((const void *)((const char *)ctx->readback + p));
#endif
    return ctx->readback;
}

