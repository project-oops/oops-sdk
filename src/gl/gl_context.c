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
static uint8_t s_host_stencil[1920 * 1080];
static uint32_t s_host_front[1920 * 1080];
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

/* One line in the kernel log, for the rest of the library. */
void gl_log_line(const char *msg) {
    if (msg) gl_klog_line(msg);
}

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
    gl_klog_words("pstex", payload + OOPS_GL_PS_TEX_OFFSET / 4u, OOPS_GL_PS_TEX_WORDS); /* textured pixel shader, + 0x400 */
    gl_klog_words("ps", payload + OOPS_GL_PS_UNTEX_OFFSET / 4u, OOPS_GL_PS_UNTEX_WORDS); /* untextured pixel shader, + 0x200 */
    gl_klog_words("desc", payload + OOPS_GL_DESC_TABLE_OFFSET / 4u, 32u); /* both units' T#/S# */
    gl_klog_words("stipple", payload + OOPS_GL_STIPPLE_OFFSET / 4u, OOPS_GL_STIPPLE_WORDS);
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

/* **Onto the scanout path** (gl_rx.h): the back becomes the display's next scanout buffer, once
 * it has left the screen, and the front the one on screen. Every colour buffer the CPU touches
 * from here on is in their 64KB_R_X swizzle (gl_color_index). */
static void gl_scanout_begin(gl_context_t *ctx) {
    (void)oops_display_wait_scanout(ctx->disp);
    uint32_t *next = oops_display_scanout(ctx->disp, 0);
    uint32_t *shown = oops_display_scanout(ctx->disp, 1);
    if (!next || !shown) return;
    ctx->hw_rx = GL_TRUE;
    ctx->color_tiled = GL_TRUE;
    ctx->back_fb = next;
    ctx->front_fb = shown;
    /* So that oops_display_get_surface hands a program's CPU overlay - gl1-cube's HUD - the
     * buffer this frame is drawn into, tiled, rather than a framebuffer nothing flips. */
    (void)oops_display_use_scanout(ctx->disp);
    gl_draw_targets(ctx);
    gl_klog_line("drawing straight into the scanout buffers (64KB_R_X)");
}
#else
static inline __attribute__((unused)) void gl_hw_dump_stream(const gl_context_t *ctx, uint32_t total_words) { (void)ctx; (void)total_words; }
#endif

/*
 * **The CPU's outstanding colour writes, made real.**
 *
 * `sfence` is the one that matters and `clflush` is the one that is usually a no-op, which is
 * the opposite of how it reads. On the scanout path the colour buffer is the display's memory,
 * mapped write-combined: a store goes into a write-combining buffer, reaches memory whenever
 * that buffer is evicted, and **is not ordered against a later load** - so a pixel rectangle
 * the CPU wrote can be read back, by this CPU or by the CP's DMA, as what was there before.
 * `sfence` drains the buffers and orders them ahead of everything after it. The `clflush` loop
 * covers the other case, a buffer mapped write-back, where the data sits in the cache rather
 * than in a WC buffer; on a WC line it costs a cycle and does nothing.
 *
 * Only the span that was written is flushed. A full-screen buffer is eight megabytes and a
 * glBitmap glyph is a hundred bytes, so flushing the whole buffer would make a HUD cost more
 * than the frame under it.
 *
 * **It is not what gl1-probe's six pixel-rectangle failures were.** This was written believing
 * it was, and the console said otherwise: the run of 2026-09-21 returned all eight failing
 * pixels byte for byte unchanged, which ruled the store ordering out and sent the search to
 * `glGetFrameReadbackSampled` above. The hazard is still real - the CP's DMA in `gl_hw_flush`
 * reads this buffer - so the drain stays; it is just not a fix for anything that was measured.
 *
 * It lives outside this file's target-only section because the host build calls it too, where
 * it is the bookkeeping and no barrier: the host's framebuffer is ordinary memory.
 */
void gl_color_cpu_drain(gl_context_t *ctx) {
    if (!ctx) return;
    if (ctx->cpu_color_lo < ctx->cpu_color_hi && ctx->cpu_color_buf) {
#if defined(__x86_64__) && !defined(OOPS_HOST_BUILD)
        const char *end = (const char *)(ctx->cpu_color_buf + ctx->cpu_color_hi);
        /* From the start of the first line to the end of the last, so a span that begins or
         * ends mid-line is covered whole. */
        const char *base = (const char *)((uintptr_t)(ctx->cpu_color_buf + ctx->cpu_color_lo) &
                                          ~(uintptr_t)63);
        for (const char *p = base; p < end; p += 64) __builtin_ia32_clflush((const void *)p);
        __builtin_ia32_sfence();
#endif
    }
    ctx->cpu_color_lo = 1u;
    ctx->cpu_color_hi = 0u;
}

void gl_hw_flush(gl_context_t *ctx) {
    if (!ctx) return;
    /* **Before the early return, not after it.** A flush is where "everything issued so far is
     * real" is promised, and that has to hold for a frame with nothing to submit as much as for
     * one with draws in it - a glDrawPixels followed by a glReadPixels builds no command stream
     * at all, and it is exactly the case that was failing. The CP's DMA copy below reads the
     * colour buffer too, so the drain has to precede it either way. */
    gl_color_cpu_drain(ctx);
    if (!ctx->use_hardware || !ctx->hw_frame_active || ctx->dcb_words == 0) {
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
    ctx->readback_of = NULL;
    if (ctx->readback && ctx->framebuffer) {
        ctx->readback_of = ctx->framebuffer;
        *dw++ = 0xc0053c00u;
        *dw++ = 0x00000013u;
        *dw++ = (uint32_t)fence_gpu;
        *dw++ = (uint32_t)(fence_gpu >> 32);
        *dw++ = 0xbeefcafeu;
        *dw++ = 0xffffffffu;
        *dw++ = 4u;
        gl_hw_emit_dma_copy(&dw, (uint64_t)(uintptr_t)ctx->framebuffer, (uint64_t)(uintptr_t)ctx->readback,
                            (uint32_t)gl_color_words(ctx) * 4u);
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
    ctx->hw_vbo_cursor = 0;
}

/* The textured pixel shader, laid into `ps_tex` (OOPS_GL_PS_TEX_WORDS words) with the canary
 * store aimed at `canary_gpu`. The patch slots - combine, fog, alpha test - start as GL_MODULATE,
 * no fog and no test; the gl_ps_patch_* functions rewrite them in place. */
void gl_ps_build_textured(uint32_t *ps_tex, uint64_t canary_gpu) {
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
    /* 16..31: the polygon stipple's discard, patched in place by gl_ps_patch_stipple; s_nop is
     * no stipple. It sits here because the fragment's position arrives in v2 and v3, which the
     * interpolation below uses as scratch, and because the discard must happen before whole-quad
     * mode so that the mask kept in s16 for the sample is the one it leaves. */
    for (size_t p = GL_PS_STIPPLE_SLOT; p < GL_PS_STIPPLE_SLOT + GL_PS_STIPPLE_WORDS; p++) {
        ps_tex[p] = 0xbf800000u; /* s_nop 0 */
    }
    /* 32..59 (since 2026-09-19; tools/shader/tex-prolog.s): **whole-quad mode for the sample**,
     * so the helper pixels of each 2x2 quad run and the sample's implicit derivatives - its level
     * of detail - exist; **q divided per fragment**; and the sample with that level of detail. It
     * was image_sample_lz, level zero, until then: the mip chain, the minification filter and GL
     * 1.4's LOD bias never took effect on the console. The live pixels' mask is kept in s16 and
     * restored after the sample, so the combine, fog, the alpha test and the export see exactly
     * the pixels they did - ACO's discipline (aco_insert_exec_mask.cpp:61-97, :150-163). */
    /* Written relative to the slot above it since 2026-09-20, so that a slot inserted before it
     * moves it rather than renumbering twenty-eight literals by hand. */
    uint32_t *const pro = ps_tex + GL_PS_STIPPLE_SLOT + GL_PS_STIPPLE_WORDS;
    pro[ 0] = 0xbe90037eu; /* s_mov_b32 s16, exec_lo */
    pro[ 1] = 0xbefe097eu; /* s_wqm_b32 exec_lo, exec_lo */
    pro[ 2] = 0xc8080400u; /* v_interp_p1_f32 v2, v0, attr1.x (s) */
    pro[ 3] = 0xc8090401u; /* v_interp_p2_f32 v2, v1, attr1.x */
    pro[ 4] = 0xc80c0500u; /* v_interp_p1_f32 v3, v0, attr1.y (t) */
    pro[ 5] = 0xc80d0501u; /* v_interp_p2_f32 v3, v1, attr1.y */
    pro[ 6] = 0xc8300700u; /* v_interp_p1_f32 v12, v0, attr1.w (q) */
    pro[ 7] = 0xc8310701u; /* v_interp_p2_f32 v12, v1, attr1.w */
    pro[ 8] = 0x7e18550cu; /* v_rcp_f32 v12, v12 */
    pro[ 9] = 0x10041902u; /* v_mul_f32 v2, v2, v12 (s/q) */
    pro[10] = 0x10061903u; /* v_mul_f32 v3, v3, v12 (t/q) */
    pro[11] = 0xc8200000u; /* v_interp_p1_f32 v8, v0, attr0.x (R) */
    pro[12] = 0xc8210001u; /* v_interp_p2_f32 v8, v1, attr0.x */
    pro[13] = 0xc8240100u; /* v_interp_p1_f32 v9, v0, attr0.y (G) */
    pro[14] = 0xc8250101u; /* v_interp_p2_f32 v9, v1, attr0.y */
    pro[15] = 0xc8280200u; /* v_interp_p1_f32 v10, v0, attr0.z (B) */
    pro[16] = 0xc8290201u; /* v_interp_p2_f32 v10, v1, attr0.z */
    pro[17] = 0xc82c0300u; /* v_interp_p1_f32 v11, v0, attr0.w (A) */
    pro[18] = 0xc82d0301u; /* v_interp_p2_f32 v11, v1, attr0.w */
    pro[19] = 0xf40c0100u; /* s_load_dwordx8 s[4:11], s[0:1], 0x00 */
    pro[20] = 0xfa000000u;
    pro[21] = 0xf4080300u; /* s_load_dwordx4 s[12:15], s[0:1], 0x20 */
    pro[22] = 0xfa000020u;
    pro[23] = 0xbf8cc07fu; /* s_waitcnt lgkmcnt(0) */
    /* 56..83: the sample itself - two words for a 2D texture, seven for a volume, which has its
     * third coordinate to interpolate and divide, twenty-four for a cube map, which has its face
     * to find first. gl_ps_patch_sample writes it; what it starts as is the 2D form, which is
     * what the shader has always done here. */
    ps_tex[GL_PS_SAMPLE_SLOT_TEX] = 0xf0800f08u;      /* image_sample v[4:7], v[2:3], ... 2D */
    ps_tex[GL_PS_SAMPLE_SLOT_TEX + 1u] = 0x00610402u;
    ps_tex[GL_PS_SAMPLE_SLOT_TEX + 2u] = gl_ps_s_branch(GL_PS_SAMPLE_WORDS - 3u);
    for (size_t p = GL_PS_SAMPLE_SLOT_TEX + 3u; p < GL_PS_SAMPLE_SLOT_TEX + GL_PS_SAMPLE_WORDS;
         p++) {
        ps_tex[p] = 0xbf800000u; /* s_nop 0 */
    }
    /* 84: the wait, after whichever sample ran. */
    ps_tex[GL_PS_SAMPLE_SLOT_TEX + GL_PS_SAMPLE_WORDS] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
    /* 59..78: the second unit's sample, still inside whole-quad mode - gl_ps_patch_unit1. A
     * branch over the slot is what "one unit" looks like, and is what this starts as. */
    ps_tex[GL_PS_UNIT1_SLOT_TEX] = gl_ps_s_branch(GL_PS_UNIT1_WORDS - 1u);
    for (size_t p = GL_PS_UNIT1_SLOT_TEX + 1u; p < GL_PS_UNIT1_SLOT_TEX + GL_PS_UNIT1_WORDS; p++) {
        ps_tex[p] = 0xbf800000u; /* s_nop 0 */
    }
    /* 79: the live pixels again, after both samples. */
    ps_tex[GL_PS_UNIT1_SLOT_TEX + GL_PS_UNIT1_WORDS] = 0xbefe0310u; /* s_mov_b32 exec_lo, s16 */
    /* 60..123: the combine, GL_MODULATE's four multiplies to start with and a branch over the rest
     * of the slot - gl_ps_patch_tex_env rewrites all of it. */
    uint32_t *const comb = ps_tex + GL_PS_COMBINE_SLOT_TEX;
    comb[0] = 0x10081104u; /* v_mul_f32 v4, v4, v8 (R * R) */
    comb[1] = 0x100a1305u; /* v_mul_f32 v5, v5, v9 (G * G) */
    comb[2] = 0x100c1506u; /* v_mul_f32 v6, v6, v10 (B * B) */
    comb[3] = 0x100e1707u; /* v_mul_f32 v7, v7, v11 (A * A) */
    comb[4] = gl_ps_s_branch(GL_PS_COMBINE_WORDS - 5u);
    for (size_t p = GL_PS_COMBINE_SLOT_TEX + 5u;
         p < GL_PS_COMBINE_SLOT_TEX + GL_PS_COMBINE_WORDS; p++) {
        ps_tex[p] = 0xbf800000u; /* s_nop 0 */
    }
    /* 144..207: the second unit's combine stage, a branch over itself until a draw sets it -
     * gl_ps_patch_tex_env_unit1, which a two-unit draw does and a one-unit draw undoes. */
    ps_tex[GL_PS_COMBINE2_SLOT_TEX] = gl_ps_s_branch(GL_PS_COMBINE_WORDS - 1u);
    for (size_t p = GL_PS_COMBINE2_SLOT_TEX + 1u;
         p < GL_PS_COMBINE2_SLOT_TEX + GL_PS_COMBINE_WORDS; p++) {
        ps_tex[p] = 0xbf800000u; /* s_nop 0 */
    }
    /* 208..219: the colour sum, after the combine and before fog as GL orders them - see
     * gl_ps_patch_sum. */
    for (size_t p = GL_PS_SUM_SLOT_TEX; p < GL_PS_SUM_SLOT_TEX + GL_PS_SUM_WORDS; p++) {
        ps_tex[p] = 0xbf800000u; /* s_nop 0 */
    }
    /* 120..131: fog, after the combine and the sum - see gl_ps_patch_fog. */
    for (size_t p = GL_PS_FOG_SLOT_TEX; p < GL_PS_FOG_SLOT_TEX + GL_PS_FOG_WORDS; p++) {
        ps_tex[p] = 0xbf800000u; /* s_nop 0 */
    }
    /* 258..273: antialiasing's coverage, between fog and the alpha test - the same place the
     * untextured shader keeps it, and the order GL specifies (1.x, 3.12). A branch over the slot
     * is no smoothing, which is what every frame that does not smooth carries, so the program a
     * frame like gl-cube's executes is the one it always was. `gl_ps_patch_coverage` writes it. */
    ps_tex[GL_PS_COVERAGE_SLOT_TEX] = gl_ps_s_branch(GL_PS_COVERAGE_WORDS - 1u);
    for (size_t p = GL_PS_COVERAGE_SLOT_TEX + 1u;
         p < GL_PS_COVERAGE_SLOT_TEX + GL_PS_COVERAGE_WORDS; p++) {
        ps_tex[p] = 0xbf800000u; /* s_nop 0 */
    }
    /* 132..135: the alpha test, as in the untextured shader. It goes *after* the texture multiply,
     * so the alpha tested is the one that reaches the framebuffer rather than the interpolated
     * one before modulation - which is what GL specifies and is the difference a texture with an
     * alpha channel makes visible. */
    for (size_t p = GL_PS_ALPHA_SLOT_TEX; p < GL_PS_ALPHA_SLOT_TEX + 4u; p++) {
        ps_tex[p] = 0xbf800000u; /* s_nop 0 */
    }
    ps_tex[GL_PS_EXPORT_TEX] = 0xf800180fu; /* exp mrt0, v4, v5, v6, v7 done vm */
    ps_tex[GL_PS_EXPORT_TEX + 1u] = 0x07060504u;
    ps_tex[GL_PS_EXPORT_TEX + 2u] = 0xbf810000u; /* s_endpgm */
    for (size_t p = GL_PS_EXPORT_TEX + 3u; p < OOPS_GL_PS_TEX_WORDS; p++) ps_tex[p] = 0xbf800000u;
}

/* **The vertex shader for a draw with a third interpolant** (since 2026-09-19). It is the
 * two-parameter program with a 64-byte vertex, whose fourth vec4 is exported as param2:
 * {secondary r, g, b, unit 0's r}. Every word is tools/shader/vs-param3.s's, assembled; the
 * instructions the two share assembled to the words gl_context.c has written since 2026-09-14.
 * The interface - SPI_VS_OUT_CONFIG 0x4, SPI_PS_IN_CONTROL 0x3, SPI_PS_INPUT_CNTL_2 0x2, the
 * unpacked form - carried a third parameter's value to the pixel shader byte for byte on a
 * console (REQ-20260919T1745Z-9c3e, sweep 20260919-212620). The packed form hung the GPU in the
 * same sweep. */
void gl_vs_build_param3(uint32_t *vs, uint64_t vbo_gpu, uint64_t canary_gpu) {
    static const uint32_t words[OOPS_GL_VS_P3_WORDS] = {
        0xbfa00001u, /* s_inst_prefetch 0x1 */
        0xbe8c037eu, /* s_mov_b32 s12, exec_lo */
        0xbefc03ffu, 0x00001003u, /* s_mov_b32 m0, 0x1003 */
        0xbf800000u, /* s_nop 0 */
        0xbf900009u, /* s_sendmsg sendmsg(MSG_GS_ALLOC_REQ) */
        0xbefe0381u, /* s_mov_b32 exec_lo, 1 */
        0x7e0202ffu, 0x20280600u, /* v_mov_b32 v1, 0x20280600 */
        0xf8000941u, 0x00000001u, /* exp prim v1, off, off, off done */
        0xbf8cff0fu, /* s_waitcnt expcnt(0) */
        0xbefe0387u, /* s_mov_b32 exec_lo, 7 */
        0xd765000eu, 0x000100c1u, /* v_mbcnt_lo_u32_b32 v14, -1, 0 */
        0x341e1c82u, /* v_lshlrev_b32 v15, 2, v14 (lane * 4, for the diagnostic store) */
        0x341c1c86u, /* v_lshlrev_b32 v14, 6, v14 (lane * 64, the vertex) */
        0x4a1c1c08u, /* v_add_nc_u32 v14, s8, v14 (+ the vertex buffer offset) */
        0xbe8203ffu, 0u, /* s_mov_b32 s2, vbo_lo (word 19) */
        0xbe8303ffu, 0u, /* s_mov_b32 s3, vbo_hi (word 21) */
        0x7e020280u, /* v_mov_b32 v1, 0 */
        0xd70f6a12u, 0x00021c02u, /* v_add_co_u32 v18, vcc_lo, s2, v14 */
        0x50260203u, /* v_add_co_ci_u32 v19, vcc_lo, s3, v1, vcc_lo */
        0xdc388000u, 0x027d0012u, /* global_load_dwordx4 v[2:5], v[18:19], off (position) */
        0xdc388010u, 0x0a7d0012u, /* global_load_dwordx4 v[10:13], v[18:19], off offset:16 (colour) */
        0xdc388020u, 0x067d0012u, /* global_load_dwordx4 v[6:9], v[18:19], off offset:32 (texture) */
        0xdc388030u, 0x147d0012u, /* global_load_dwordx4 v[20:23], v[18:19], off offset:48 (param2) */
        0xbf8c3f70u, /* s_waitcnt vmcnt(0) */
        0xbe8603ffu, 0u, /* s_mov_b32 s6, canary_lo (word 36) */
        0xbe8703ffu, 0u, /* s_mov_b32 s7, canary_hi (word 38) */
        0xd70f6a12u, 0x00021e06u, /* v_add_co_u32 v18, vcc_lo, s6, v15 */
        0x50260207u, /* v_add_co_ci_u32 v19, vcc_lo, s7, v1, vcc_lo */
        0xdc708008u, 0x007d0212u, /* global_store_dword v[18:19], v2, off offset:8 */
        0xbf8c3f70u, /* s_waitcnt vmcnt(0) */
        0xf800020fu, 0x0d0c0b0au, /* exp param0, v10, v11, v12, v13 (colour) */
        0xf800021fu, 0x09080706u, /* exp param1, v6, v7, v8, v9 (texture) */
        0xf800022fu, 0x17161514u, /* exp param2, v20, v21, v22, v23 */
        0xf80008cfu, 0x05040302u, /* exp pos0, v2, v3, v4, v5 done */
        0xbf8cff0fu, /* s_waitcnt expcnt(0) */
        0xbefe0381u, /* s_mov_b32 exec_lo, 1 */
        0x7e240206u, /* v_mov_b32 v18, s6 */
        0x7e260207u, /* v_mov_b32 v19, s7 */
        0x7e0202ffu, 0xbeef0001u, /* v_mov_b32 v1, 0xbeef0001 */
        0xdc708000u, 0x007d0112u, /* global_store_dword v[18:19], v1, off */
        0xbf8c3f70u, /* s_waitcnt vmcnt(0) */
        0xbefe030cu, /* s_mov_b32 exec_lo, s12 */
        0xbf810000u, /* s_endpgm */
    };
    memcpy(vs, words, sizeof(words));
    vs[19] = (uint32_t)vbo_gpu;
    vs[21] = (uint32_t)(vbo_gpu >> 32);
    vs[36] = (uint32_t)canary_gpu;
    vs[38] = (uint32_t)(canary_gpu >> 32);
}

/*
 * The vertex shader for a draw with two texture units: the three-parameter program above with an
 * 80-byte vertex and a fifth vec4 exported as param3 - the second unit's texture coordinate.
 * tools/shader/vs-param4.s, where every instruction it shares with the three-parameter program
 * assembled to the word written there.
 *
 * **What is measured and what is not.** obSCEne's `REQ-20260919T2258Z-8b1c` (sweep
 * `20260920-082906`, `166-agc/primitive-draw-param4`) retired a draw with `SPI_VS_OUT_CONFIG`
 * 0x6, `SPI_PS_IN_CONTROL` 0x4 and `SPI_PS_INPUT_CNTL_3` 0x3, both canaries intact - so this
 * interface is legal on the part and does not hang it, which the packed form of the *third*
 * parameter did (`-9c3e`). It did **not** show the value arriving: its three-parameter control
 * printed the same pixel as the four-parameter arm, so nothing in those rows distinguishes
 * attribute 3 being read from something else being exported. `REQ-20260920T0745Z-9a41` re-asks
 * for a control that can differ. Until it answers, this program is built and host-tested but no
 * draw runs it - see OOPS_GL_MULTITEX_MEASURED in gl_multitex.h.
 *
 * The 80-byte stride is not a shift, so the lane's offset is lane * 64 plus lane * 16.
 */
void gl_vs_build_param4(uint32_t *vs, uint64_t vbo_gpu, uint64_t canary_gpu) {
    static const uint32_t words[OOPS_GL_VS_P4_WORDS] = {
        0xbfa00001u, /* s_inst_prefetch 0x1 */
        0xbe8c037eu, /* s_mov_b32 s12, exec_lo */
        0xbefc03ffu, 0x00001003u, /* s_mov_b32 m0, 0x1003 */
        0xbf800000u, /* s_nop 0 */
        0xbf900009u, /* s_sendmsg sendmsg(MSG_GS_ALLOC_REQ) */
        0xbefe0381u, /* s_mov_b32 exec_lo, 1 */
        0x7e0202ffu, 0x20280600u, /* v_mov_b32 v1, 0x20280600 */
        0xf8000941u, 0x00000001u, /* exp prim v1, off, off, off done */
        0xbf8cff0fu, /* s_waitcnt expcnt(0) */
        0xbefe0387u, /* s_mov_b32 exec_lo, 7 */
        0xd765000eu, 0x000100c1u, /* v_mbcnt_lo_u32_b32 v14, -1, 0 */
        0x341e1c82u, /* v_lshlrev_b32 v15, 2, v14 (lane * 4, for the diagnostic store) */
        0x34201c84u, /* v_lshlrev_b32 v16, 4, v14 (lane * 16) */
        0x341c1c86u, /* v_lshlrev_b32 v14, 6, v14 (lane * 64) */
        0x4a1c1d10u, /* v_add_nc_u32 v14, v16, v14 (lane * 80, the vertex) */
        0x4a1c1c08u, /* v_add_nc_u32 v14, s8, v14 (+ the vertex buffer offset) */
        0xbe8203ffu, 0u, /* s_mov_b32 s2, vbo_lo (word 21) */
        0xbe8303ffu, 0u, /* s_mov_b32 s3, vbo_hi (word 23) */
        0x7e020280u, /* v_mov_b32 v1, 0 */
        0xd70f6a12u, 0x00021c02u, /* v_add_co_u32 v18, vcc_lo, s2, v14 */
        0x50260203u, /* v_add_co_ci_u32 v19, vcc_lo, s3, v1, vcc_lo */
        0xdc388000u, 0x027d0012u, /* global_load_dwordx4 v[2:5], off (position) */
        0xdc388010u, 0x0a7d0012u, /* global_load_dwordx4 v[10:13], off offset:16 (colour) */
        0xdc388020u, 0x067d0012u, /* global_load_dwordx4 v[6:9], off offset:32 (unit 0) */
        0xdc388030u, 0x147d0012u, /* global_load_dwordx4 v[20:23], off offset:48 (param2) */
        0xdc388040u, 0x187d0012u, /* global_load_dwordx4 v[24:27], off offset:64 (unit 1) */
        0xbf8c3f70u, /* s_waitcnt vmcnt(0) */
        0xbe8603ffu, 0u, /* s_mov_b32 s6, canary_lo (word 40) */
        0xbe8703ffu, 0u, /* s_mov_b32 s7, canary_hi (word 42) */
        0xd70f6a12u, 0x00021e06u, /* v_add_co_u32 v18, vcc_lo, s6, v15 */
        0x50260207u, /* v_add_co_ci_u32 v19, vcc_lo, s7, v1, vcc_lo */
        0xdc708008u, 0x007d0212u, /* global_store_dword v[18:19], v2, off offset:8 */
        0xbf8c3f70u, /* s_waitcnt vmcnt(0) */
        0xf800020fu, 0x0d0c0b0au, /* exp param0, v10, v11, v12, v13 (colour) */
        0xf800021fu, 0x09080706u, /* exp param1, v6, v7, v8, v9 (unit 0) */
        0xf800022fu, 0x17161514u, /* exp param2, v20, v21, v22, v23 */
        0xf800023fu, 0x1b1a1918u, /* exp param3, v24, v25, v26, v27 (unit 1) */
        0xf80008cfu, 0x05040302u, /* exp pos0, v2, v3, v4, v5 done */
        0xbf8cff0fu, /* s_waitcnt expcnt(0) */
        0xbefe0381u, /* s_mov_b32 exec_lo, 1 */
        0x7e240206u, /* v_mov_b32 v18, s6 */
        0x7e260207u, /* v_mov_b32 v19, s7 */
        0x7e0202ffu, 0xbeef0001u, /* v_mov_b32 v1, 0xbeef0001 */
        0xdc708000u, 0x007d0112u, /* global_store_dword v[18:19], v1, off */
        0xbf8c3f70u, /* s_waitcnt vmcnt(0) */
        0xbefe030cu, /* s_mov_b32 exec_lo, s12 */
        0xbf810000u, /* s_endpgm */
    };
    memcpy(vs, words, sizeof(words));
    vs[21] = (uint32_t)vbo_gpu;
    vs[23] = (uint32_t)(vbo_gpu >> 32);
    vs[40] = (uint32_t)canary_gpu;
    vs[42] = (uint32_t)(canary_gpu >> 32);
}

/* One texture unit's initial state - every unit's is the same (GL 1.3, tables 6.15-6.20). */
static void gl_tex_unit_init(gl_tex_unit_t *tu) {
    memset(tu, 0, sizeof(*tu));
    tu->tex_env_mode = GL_MODULATE;
    /* GL_COMBINE's defaults (GL 1.3, table 6.19): modulate the texture by the previous colour,
     * the third argument the constant's alpha, scales 1. */
    tu->combine.mode_rgb = GL_MODULATE;
    tu->combine.mode_alpha = GL_MODULATE;
    tu->combine.source_rgb[0] = GL_TEXTURE;
    tu->combine.source_rgb[1] = GL_PREVIOUS;
    tu->combine.source_rgb[2] = GL_CONSTANT;
    tu->combine.source_alpha[0] = GL_TEXTURE;
    tu->combine.source_alpha[1] = GL_PREVIOUS;
    tu->combine.source_alpha[2] = GL_CONSTANT;
    tu->combine.operand_rgb[0] = GL_SRC_COLOR;
    tu->combine.operand_rgb[1] = GL_SRC_COLOR;
    tu->combine.operand_rgb[2] = GL_SRC_ALPHA;
    tu->combine.operand_alpha[0] = GL_SRC_ALPHA;
    tu->combine.operand_alpha[1] = GL_SRC_ALPHA;
    tu->combine.operand_alpha[2] = GL_SRC_ALPHA;
    tu->combine.scale_rgb = 1.0f;
    tu->combine.scale_alpha = 1.0f;
    mat4_identity(&tu->texture_stack[0]);
    /* Texture generation defaults: EYE_LINEAR, with the S plane (1,0,0,0) and the T plane
     * (0,1,0,0) and R and Q all zero. Those are the specification's initial values, and they
     * are not all the same - a loop setting every plane to (1,0,0,0) would be wrong for T. */
    for (int i = 0; i < 4; i++) tu->texgen_mode[i] = GL_EYE_LINEAR;
    tu->texgen_object_plane[0][0] = 1.0f; /* S: (1, 0, 0, 0) */
    tu->texgen_eye_plane[0][0] = 1.0f;
    tu->texgen_object_plane[1][1] = 1.0f; /* T: (0, 1, 0, 0) */
    tu->texgen_eye_plane[1][1] = 1.0f;
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
    uint8_t *stencil = NULL;

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
    /* **The stencil surface is the GPU's** (since 2026-09-19): STENCIL_8 at the depth surface's
     * swizzle, 64KB_Z_X, which the hardware stencil test reads and writes. A 64 KiB block of one
     * byte a pixel is 256 x 256 - 2^16 elements split between the axes, width first, by addrlib's
     * ComputeThinBlockDimension (addrlib2.cpp:1698-1709), which gives 32-bit depth its 128 x 128 -
     * so both axes pad to 256 here, not to depth's 128:
     * 1920 x 1080 needs 8 x 5 blocks, 2.5 MiB. This was sized as depth's 128-padded extent while
     * it was CPU memory only, which a tiled 8-bit surface would have overrun. */
    size_t stencil_px = (size_t)((w + 255u) & ~255u) * (size_t)((h + 255u) & ~255u);
    stencil = (uint8_t *)oops_mem_alloc(stencil_px, 64 * 1024, OOPS_MEM_WC_GARLIC);
    if (!stencil) {
        oops_mem_free(depth);
        oops_mem_free(ctx);
        return NULL;
    }
#else
    ctx = &s_host_ctx;
    memset(ctx, 0, sizeof(*ctx));
    depth = s_host_depth;
    size_t depth_px = (size_t)w * (size_t)h;
    stencil = s_host_stencil;
    size_t stencil_px = depth_px;
#endif

    ctx->disp = disp;
    ctx->framebuffer = oops_display_get_framebuffer(disp);
    ctx->back_fb = ctx->framebuffer;
    ctx->width = w;
    ctx->height = h;
    ctx->depth_buffer = depth;
    ctx->depth_px = depth_px;
    ctx->stencil_buffer = stencil;
    ctx->stencil_px = stencil_px;

    /* Stencil defaults, all from the specification: the test off, GL_ALWAYS with reference 0 and
     * both masks all-ones, every operation GL_KEEP, and the clear value 0. An all-ones write mask
     * matters - a zero one silently makes every stencil write a no-op. */
    /* The raster position starts at the origin and **valid**: a program that calls glDrawPixels
     * without ever setting one draws at (0,0), which is what the specification says. */
    ctx->raster_pos[0] = 0.0f;
    ctx->raster_pos[1] = 0.0f;
    ctx->raster_pos[2] = 0.0f;
    ctx->raster_pos[3] = 1.0f;
    ctx->raster_color[0] = 1.0f;
    ctx->raster_color[1] = 1.0f;
    ctx->raster_color[2] = 1.0f;
    ctx->raster_color[3] = 1.0f;
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        ctx->raster_texcoord[u][0] = 0.0f;
        ctx->raster_texcoord[u][1] = 0.0f;
        ctx->raster_texcoord[u][2] = 0.0f;
        ctx->raster_texcoord[u][3] = 1.0f;
    }
    ctx->raster_distance = 0.0f;
    ctx->raster_valid = GL_TRUE;
    ctx->pixel_zoom_x = 1.0f;
    ctx->pixel_zoom_y = 1.0f;
    ctx->point_size = 1.0f;
    /* GL 1.4's point parameters at Mesa's initial values (main/points.c:211-219): no
     * attenuation, a clamp from 0 to the largest size drawn, a fade threshold of 1. */
    ctx->point_size_min = 0.0f;
    ctx->point_size_max = (float)OOPS_GL_MAX_POINT_LINE_SIZE;
    ctx->point_fade_threshold = 1.0f;
    ctx->point_atten[0] = 1.0f;
    ctx->point_atten[1] = 0.0f;
    ctx->point_atten[2] = 0.0f;
    ctx->line_width = 1.0f;
    /* Colour-index and multisample state at the specification's defaults - the current index is
     * 1 (Mesa main/context.c:269), the index mask all ones and the clear index 0
     * (main/blend.c:1139-1141), GL_MULTISAMPLE on with a coverage value of 1
     * (main/multisample.c:69-75), and GL_DITHER on. */
    ctx->cur_index = 1.0f;
    ctx->clear_index = 0.0f;
    ctx->index_mask = ~0u;
    ctx->cap_multisample = GL_TRUE;
    ctx->sample_coverage_value = 1.0f;
    ctx->sample_coverage_invert = GL_FALSE;
    ctx->cap_dither = GL_TRUE;
    ctx->hint_point_smooth = GL_DONT_CARE;
    ctx->hint_line_smooth = GL_DONT_CARE;
    ctx->hint_polygon_smooth = GL_DONT_CARE;
    ctx->hint_fog = GL_DONT_CARE;
    ctx->hint_texture_compression = GL_DONT_CARE;
    ctx->hint_generate_mipmap = GL_DONT_CARE;
    gl_eval_init(ctx);
    /* Drawing, with neither selection nor feedback buffer given yet (Mesa main/feedback.c,
     * _mesa_init_feedback). */
    ctx->render_mode = GL_RENDER;
    ctx->feedback_type = GL_2D;
    ctx->hit_min_z = 1.0f;
    ctx->hit_max_z = 0.0f;
    /* Pixel transfer that changes nothing: scales 1, biases 0, and every map one entry of 0
     * (Mesa main/pixel.c, _mesa_init_pixel). */
    for (int i = 0; i < 4; i++) {
        ctx->pixel_scale[i] = 1.0f;
        ctx->pixel_bias[i] = 0.0f;
    }
    ctx->depth_scale = 1.0f;
    ctx->depth_bias = 0.0f;
    for (int i = 0; i < OOPS_GL_PIXEL_MAPS; i++) {
        ctx->pixel_map_size[i] = 1;
        ctx->pixel_map[i][0] = 0.0f;
    }
    /* Stipples that let everything through: the line pattern all ones at factor 1, the polygon
     * mask all ones (Mesa main/context.c init_attrib_groups, main/polygon.c). */
    ctx->line_stipple_factor = 1;
    ctx->line_stipple_pattern = 0xffffu;
    for (int i = 0; i < 32; i++) ctx->polygon_stipple[i] = 0xffffffffu;

    /* Fog defaults, from the specification: GL_EXP, density 1, the range 0..1, and a **black,
     * fully transparent** fog colour - (0,0,0,0), not opaque black. */
    ctx->cap_fog = GL_FALSE;
    ctx->fog_mode = GL_EXP;
    ctx->fog_density = 1.0f;
    ctx->fog_start = 0.0f;
    ctx->fog_end = 1.0f;
    ctx->fog_color[0] = 0.0f;
    ctx->fog_color[1] = 0.0f;
    ctx->fog_color[2] = 0.0f;
    ctx->fog_color[3] = 0.0f;

    ctx->cap_stencil_test = GL_FALSE;
    ctx->stencil_func = GL_ALWAYS;
    ctx->stencil_ref = 0;
    ctx->stencil_value_mask = 0xffffffffu;
    ctx->stencil_writemask = 0xffffffffu;
    ctx->stencil_fail = GL_KEEP;
    ctx->stencil_zfail = GL_KEEP;
    ctx->stencil_zpass = GL_KEEP;
    /* GL 2.0's back face starts as the front one does, which is also what `glStencilFunc` and
     * its relatives leave it as - so a GL 1.x program never sees the split exists. */
    ctx->stencil_back_func = GL_ALWAYS;
    ctx->stencil_back_ref = 0;
    ctx->stencil_back_value_mask = 0xffffffffu;
    ctx->stencil_back_writemask = 0xffffffffu;
    ctx->stencil_back_fail = GL_KEEP;
    ctx->stencil_back_zfail = GL_KEEP;
    ctx->stencil_back_zpass = GL_KEEP;
    ctx->clear_stencil = 0;

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
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) gl_tex_unit_init(&ctx->tex_unit[u]);
    ctx->active_texture = 0u;

    ctx->depth_func = GL_LESS;
    ctx->depth_mask = GL_TRUE;
    /* GL_ONE and GL_ZERO: the specification's defaults, as Mesa initialises them
     * (main/blend.c:1148-1151). These were GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA until
     * 2026-09-19 - the factors most programs go on to ask for, which is why nothing noticed - so
     * a program that enabled GL_BLEND and never called glBlendFunc got alpha blending here and a
     * plain overwrite everywhere else, and glGetIntegerv(GL_BLEND_SRC) answered wrong before any
     * call at all. */
    ctx->blend_src = GL_ONE;
    ctx->blend_dst = GL_ZERO;
    ctx->blend_src_alpha = GL_ONE;
    ctx->blend_dst_alpha = GL_ZERO;
    ctx->blend_equation = GL_FUNC_ADD;
    ctx->blend_equation_alpha = GL_FUNC_ADD;
    ctx->blend_color[0] = 0.0f; /* main/blend.c:1155 */
    ctx->blend_color[1] = 0.0f;
    ctx->blend_color[2] = 0.0f;
    ctx->blend_color[3] = 0.0f;
    ctx->cap_color_logic_op = GL_FALSE;
    ctx->logic_op = GL_COPY; /* main/blend.c:1159 */
    ctx->perspective_hint = GL_DONT_CARE; /* the specification's default */
    ctx->cull_mode = GL_BACK;
    ctx->polygon_mode[0] = GL_FILL;
    ctx->polygon_mode[1] = GL_FILL;
    ctx->prim_raster = GL_FILL;
    ctx->prim_from_polygon = GL_FALSE;
    ctx->front_face = GL_CCW;
    ctx->shade_model = GL_SMOOTH;

    ctx->color_mask[0] = GL_TRUE;
    ctx->color_mask[1] = GL_TRUE;
    ctx->color_mask[2] = GL_TRUE;
    ctx->color_mask[3] = GL_TRUE;
    ctx->draw_buffer = GL_BACK; /* a double-buffered visual's initial buffers (GL 1.0, 4.2.1) */
    ctx->read_buffer = GL_BACK;

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
    mat4_identity(&ctx->modelview_stack[0]);
    mat4_identity(&ctx->projection_stack[0]);
    ctx->mvp_dirty = GL_TRUE;

    /* Current immediate mode attributes */
    ctx->cur_color[0] = 1.0f;
    ctx->cur_color[1] = 1.0f;
    ctx->cur_color[2] = 1.0f;
    ctx->cur_color[3] = 1.0f;
    ctx->cur_normal[0] = 0.0f;
    ctx->cur_normal[1] = 0.0f;
    ctx->cur_normal[2] = 1.0f;
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        ctx->cur_texcoord[u][0] = 0.0f;
        ctx->cur_texcoord[u][1] = 0.0f;
        ctx->cur_texcoord[u][2] = 0.0f;
        ctx->cur_texcoord[u][3] = 1.0f; /* q defaults to 1: the initial coordinate is (0, 0, 0, 1) */
    }
    ctx->cur_edge_flag = GL_TRUE; /* every edge a boundary edge until a program says otherwise */
    ctx->cur_secondary[3] = 1.0f; /* (0, 0, 0, 1), Mesa main/context.c:268 */
    /* Fog from the eye distance until a program asks for its own coordinate (main/fog.c:213);
     * the current fog coordinate and GL_FOG_INDEX start at 0 with the zeroed context. */
    ctx->fog_coord_src = GL_FRAGMENT_DEPTH;

    /* **Each array's initial size and type**, which glGet reports before any pointer call: four
     * GL_FLOATs for the vertex, colour and texture coordinate arrays, three for the normal and the
     * secondary colour, one for the index (Mesa main/varray.c:4127-4150, init_default_vao_state).
     * The zeroed context answered 0 for every one until 2026-09-19. */
    ctx->array_vertex.size = 4;   ctx->array_vertex.type = GL_FLOAT;
    ctx->array_color.size = 4;    ctx->array_color.type = GL_FLOAT;
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        ctx->array_texcoord[u].size = 4; ctx->array_texcoord[u].type = GL_FLOAT;
    }
    ctx->array_normal.size = 3;   ctx->array_normal.type = GL_FLOAT;
    ctx->array_secondary.size = 3; ctx->array_secondary.type = GL_FLOAT;
    ctx->array_fog_coord.size = 1; ctx->array_fog_coord.type = GL_FLOAT;
    ctx->array_index.size = 1;    ctx->array_index.type = GL_FLOAT;
    ctx->array_edge_flag.size = 1; ctx->array_edge_flag.type = GL_UNSIGNED_BYTE;

    /* Clip planes start all-zero and all disabled, which is the specification's initial state. */
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        ctx->clip_plane_enabled[i] = GL_FALSE;
        for (int k = 0; k < 4; k++) ctx->clip_plane[i][k] = 0.0f;
    }
    ctx->hw_clip_dirty = GL_FALSE;

    ctx->last_error = GL_NO_ERROR;

    /* GL 2.0's generic vertex attributes. **Every one's current value is (0, 0, 0, 1)** - not
     * all zeros - so an attribute whose array is disabled and which nothing has set reads as a
     * point rather than a direction, and `gl_Position = mvp * attr` of an unset attribute lands
     * at the origin instead of being degenerate. Array state starts as four floats and disabled,
     * matching the named arrays beside it. The name counter starts at 1: 0 is not an object. */
    for (int i = 0; i < OOPS_GL_MAX_VERTEX_ATTRIBS; i++) {
        gl_vertex_attrib_t *a = &ctx->vertex_attribs[i];
        a->size = 4;
        a->type = GL_FLOAT;
        a->stride = 0;
        a->pointer = (const void *)0;
        a->buffer = 0u;
        a->enabled = GL_FALSE;
        a->normalized = GL_FALSE;
        a->current[0] = 0.0f;
        a->current[1] = 0.0f;
        a->current[2] = 0.0f;
        a->current[3] = 1.0f;
    }
    ctx->gl2_next_name = 1u;
    ctx->program_current = 0u;

    /* The version this context has until the program states its own (glContextSetVersion).
     * **It gates the API as well as the badge** - see OOPS_GL_DEFAULT_VERSION_MINOR for why the
     * default is 1.5 and why it is not 2.0. */
    ctx->version_major = OOPS_GL_DEFAULT_VERSION_MAJOR;
    ctx->version_minor = OOPS_GL_DEFAULT_VERSION_MINOR;
    gl_version_string(ctx);

    /* Whether a draw may use the second texture unit (gl_multitex.h). It is a property of the
     * build rather than of the hardware init, so it is set here with the other defaults and not
     * beside the queue: the draw path reaches it only when `use_hardware` is on anyway, and a
     * host test that wants to exercise that path needs it true without a queue to talk to. */
    ctx->hw_multitex = (GLboolean)(OOPS_GL_MULTITEX_MEASURED != 0);

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
    ctx->light_model_color_control = GL_SINGLE_COLOR;
    ctx->light_model_two_side = GL_FALSE;
    ctx->cap_rescale_normal = GL_FALSE;

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
        /* 0x8000 since 2026-09-21, when the GL 2.0 uniform ring went in at 0x4000 - which is
         * where the payload used to end. gl_internal.h holds the map. */
        ctx->gpu_payload = oops_mem_alloc(OOPS_GL_PAYLOAD_BYTES, 256, OOPS_MEM_WB_ONION);
        ctx->vbo_mem = oops_mem_alloc(65536, 256, OOPS_MEM_WB_ONION);
        ctx->fence = oops_mem_alloc(0x1000, 0x1000, OOPS_MEM_WB_ONION);
        ctx->canary = oops_mem_alloc(0x1000, 0x1000, OOPS_MEM_WB_ONION);
        /* Big enough for a tiled copy too: the scanout path's buffers are padded to whole
         * 128 x 128 blocks (gl_color_words). */
        ctx->readback = (uint32_t *)oops_mem_alloc((size_t)((w + 127u) & ~127u) *
                                                       (size_t)((h + 127u) & ~127u) * 4u,
                                                   0x1000, OOPS_MEM_WB_ONION);
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
            /* The three-parameter program beside it, for a draw that needs a third interpolant
             * (gl_vs_build_param3). A frame never switches to it unless a draw does. */
            gl_vs_build_param3((uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_VS_P3_OFFSET),
                               vbo_gpu, canary_gpu);
            /*
             * And the four-parameter program, for a draw with a second texture unit.
             *
             * **This line was missing, and it cost a console run.** `gl_vs_build_param4` was
             * written, unit-tested against a scratch buffer, and wired into `gl_hw_vs_offset`,
             * which points SPI_SHADER_PGM_LO at OOPS_GL_VS_P4_OFFSET as soon as a draw needs
             * four parameters - but nothing ever wrote its seventy words into the payload. The
             * first two-unit draw on hardware jumped to 0xb00, executed whatever the allocation
             * happened to hold, and took `ILLEGAL_INST` on two waves at one PC, then a GPU
             * reset. `payload-va` plus that PC is what located it: 0x201390B04 minus
             * 0x201390000 is 0xb04, the second word of this program.
             *
             * The shape is the one the collection already knows: **a definition nothing calls
             * links and tests clean.** `app.mk`'s undefined-symbol check catches the opposite
             * case, a call with no definition; this is a definition with no call, and the only
             * thing that can see it is the hardware, once.
             */
            gl_vs_build_param4((uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_VS_P4_OFFSET),
                               vbo_gpu, canary_gpu);

            /* The NGG stage table points both the ES and GS program slots at this shader (offset 0). */

            /* 2. Hardware Textured + Gouraud Pixel Shader (OOPS_GL_PS_TEX_OFFSET, 0x400; 0x200
             * until the longer combine outgrew it) - gl_ps_build_textured, callable on the host so
             * its words can be checked there. */
            gl_ps_build_textured((uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET),
                                 canary_gpu);

            /* 3. Hardware Gouraud Barycentric Interpolating Pixel Shader (untextured, offset 0x300) */
            uint32_t *ps_untex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_UNTEX_OFFSET);
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
            /* 16..31: the polygon stipple's discard - see gl_ps_patch_stipple. The same slot in
             * the same place as the textured shader's, and for the same reason: v2 and v3 carry
             * the fragment's position in, and the interpolation below is free to use them after.
             * s_nop is no stipple. */
            for (size_t p = GL_PS_STIPPLE_SLOT; p < GL_PS_STIPPLE_SLOT + GL_PS_STIPPLE_WORDS; p++) {
                ps_untex[p] = 0xbf800000u; /* s_nop 0 */
            }
            uint32_t *const in = ps_untex + GL_PS_STIPPLE_SLOT + GL_PS_STIPPLE_WORDS;
            in[0] = 0xc8100000u; /* v_interp_p1_f32 v4, v0, attr0.x (R) */
            in[1] = 0xc8110001u; /* v_interp_p2_f32 v4, v1, attr0.x */
            in[2] = 0xc8140100u; /* v_interp_p1_f32 v5, v0, attr0.y (G) */
            in[3] = 0xc8150101u; /* v_interp_p2_f32 v5, v1, attr0.y */
            in[4] = 0xc8180200u; /* v_interp_p1_f32 v6, v0, attr0.z (B) */
            in[5] = 0xc8190201u; /* v_interp_p2_f32 v6, v1, attr0.z */
            in[6] = 0xc81c0300u; /* v_interp_p1_f32 v7, v0, attr0.w (A) */
            in[7] = 0xc81d0301u; /* v_interp_p2_f32 v7, v1, attr0.w */
            /* 40..51: fog, patched in place by gl_ps_patch_fog; s_nop is no fog. */
            for (size_t p = GL_PS_FOG_SLOT_UNTEX; p < GL_PS_FOG_SLOT_UNTEX + GL_PS_FOG_WORDS; p++) {
                ps_untex[p] = 0xbf800000u; /* s_nop 0 */
            }
            /* 52..67: antialiasing's coverage, after fog and before the alpha test - see
             * gl_ps_patch_coverage. A branch over the slot is no smoothing, which is what it
             * starts as and what a frame that never smooths keeps, so gl-cube's stream is the
             * one it was. */
            ps_untex[GL_PS_COVERAGE_SLOT_UNTEX] = gl_ps_s_branch(GL_PS_COVERAGE_WORDS - 1u);
            for (size_t p = GL_PS_COVERAGE_SLOT_UNTEX + 1u;
                 p < GL_PS_COVERAGE_SLOT_UNTEX + GL_PS_COVERAGE_WORDS; p++) {
                ps_untex[p] = 0xbf800000u; /* s_nop 0 */
            }
            /* 68..71: the alpha test, patched in place by gl_ps_patch_alpha_test. Four words,
             * which is exactly what the longest form needs (a literal load is two). Left as
             * s_nop here, which is what "no alpha test" is. */
            for (size_t p = GL_PS_ALPHA_SLOT_UNTEX; p < GL_PS_ALPHA_SLOT_UNTEX + 4u; p++) {
                ps_untex[p] = 0xbf800000u; /* s_nop 0 */
            }
            /* 72..76: the export and the end - see gl_ps_patch_export, which writes the second
             * target's when GL names both buffers. */
            ps_untex[GL_PS_EXPORT_UNTEX] = 0xf800180fu; /* exp mrt0, v4, v5, v6, v7 done vm */
            ps_untex[GL_PS_EXPORT_UNTEX + 1u] = 0x07060504u;
            ps_untex[GL_PS_EXPORT_UNTEX + 2u] = 0xbf810000u; /* s_endpgm */
            for (size_t p = GL_PS_EXPORT_UNTEX + 3u; p < OOPS_GL_PS_UNTEX_WORDS; p++) {
                ps_untex[p] = 0xbf800000u;
            }

            /* 4. Initialize active texture descriptor table (unit 0's pair; unit 1's follows it
             * at OOPS_GL_DESC_UNIT_STRIDE and is filled by nothing yet - gl_multitex.h). */
            uint32_t *desc_table =
                (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_DESC_TABLE_OFFSET);
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
            ctx->zs_tiled = GL_TRUE; /* the DB draws depth and stencil 64KB_Z_X from here on */
            /* The scanout path, once REQ-20260919T1927Z-7e21 has measured it (gl_rx.h) - before
             * the self-test, so that the test clears the buffer frames will be drawn into. */
            if (OOPS_GL_RX_MEASURED &&
                oops_display_scanout_layout(disp) == OOPS_DISPLAY_SCANOUT_RX) {
                gl_scanout_begin(ctx);
            }
            /*
             * **The payload's address, so a faulting wave's program counter names a word.**
             *
             * A GPU fault reports a PC and nothing else - `PC=0x0000000201390B04 ILLEGAL_INST`,
             * one line per wave. Every shader this library runs lives at a fixed offset inside
             * this one allocation (OOPS_GL_VS_P3_OFFSET, _VS_P4_OFFSET, OOPS_GL_PS_TEX_OFFSET
             * and the rest), so the base is the entire difference between that number and a
             * line of a `.s` file.
             *
             * It cost a console run to learn that. On 2026-09-20 the first draw with two
             * texture units faulted on two waves at the same PC, and the log had no way to say
             * which shader the address was even in - the offsets fit more than one guess, and
             * guessing at a register or an address from memory is the mistake this project
             * keeps a bus to avoid.
             */
            gl_klog_val("payload-va", (uint64_t)(uintptr_t)ctx->gpu_payload);
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
     * the buffer storage has to go first. The display lists' storage is heap memory now too. */
    gl_free_all_buffers(ctx);
    gl_free_all_shaders(ctx);
    gl_list_free_all(ctx);
    gl_eval_free(ctx);
    gl_accum_free(ctx);
    for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) gl_tex_free_mips(&ctx->textures[i]);

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
    if (ctx->stencil_buffer) oops_mem_free(ctx->stencil_buffer);
    if (ctx->front_fb && !ctx->hw_rx) oops_mem_free(ctx->front_fb); /* a scanout buffer is the display's */
    if (ctx->readback_lin) oops_mem_free(ctx->readback_lin);
    oops_mem_free(ctx);
#else
    for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
        if (ctx->textures[i].pixels) {
            free(ctx->textures[i].pixels);
            ctx->textures[i].pixels = NULL;
        }
    }
    free(ctx->readback_lin);
    ctx->readback_lin = NULL;
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

    /* **The scanout path's swap** (gl_rx.h): the back was drawn in place, so it is flipped as
     * drawn and becomes the front - it is the buffer on screen. The other scanout buffer is
     * the back once it has left the screen. Nothing is copied or tiled, and the back's old
     * contents are the frame before last, which GL allows: a back buffer is undefined after a
     * swap. The wait is here, at the swap, so a frame is never drawn into the buffer being
     * scanned. */
    if (ctx->hw_rx) {
        (void)oops_display_flip_scanout(ctx->disp);
        (void)oops_display_wait_scanout(ctx->disp);
        ctx->back_fb = oops_display_scanout(ctx->disp, 0);
        ctx->front_fb = oops_display_scanout(ctx->disp, 1);
        ctx->front_pending = GL_FALSE;
        gl_draw_targets(ctx);
        ctx->frame_count++;
        return;
    }
#endif

    /* Flip display buffer */
    uint32_t *const presented = ctx->back_fb;
    oops_display_flip(ctx->disp);

    /* **The front buffer is now what was just presented**: a swap makes the back's contents the
     * front's (GL 1.0, 2.1.1). A program that has a front surface gets it brought up to date. The
     * back is left as it was, which GL allows - its contents after a swap are undefined. */
    if (ctx->front_fb && presented) {
        memcpy(ctx->front_fb, gl_color_read_source(ctx, presented),
               (size_t)ctx->width * (size_t)ctx->height * sizeof(uint32_t));
    }
    ctx->front_pending = GL_FALSE;

    /* The display's framebuffer may be another buffer after a flip - GNM's is its scanout. */
    ctx->back_fb = oops_display_get_framebuffer(ctx->disp);
    gl_draw_targets(ctx);
    ctx->frame_count++;
}

/* The front surface - the display's size, 64 KiB aligned as a colour target, in the Garlic
 * memory the depth and stencil surfaces are measured working in. It starts as the picture on
 * screen, so a program that switches to the front after a swap draws over what it swapped. The
 * host has no screen, so there the front starts as the back. On the scanout path the front is
 * the scanout buffer on screen, set by gl_scanout_begin and every swap, and is never allocated. */
GLboolean gl_front_buffer(gl_context_t *ctx) {
    if (ctx->front_fb) return GL_TRUE;
    if (!ctx->back_fb) return GL_FALSE;
    const size_t bytes = (size_t)ctx->width * (size_t)ctx->height * sizeof(uint32_t);
#ifndef OOPS_HOST_BUILD
    uint32_t *front = (uint32_t *)oops_mem_alloc(bytes, 64 * 1024, OOPS_MEM_WC_GARLIC);
    if (!front) return GL_FALSE;
    if (oops_display_read_shown(ctx->disp, front) != 0) {
        memcpy(front, gl_color_read_source(ctx, ctx->back_fb), bytes);
    }
#else
    if (bytes > sizeof(s_host_front)) return GL_FALSE;
    uint32_t *front = s_host_front;
    memcpy(front, ctx->back_fb, bytes);
#endif
    ctx->front_fb = front;
    return GL_TRUE;
}

/* **Where a draw goes** - glDrawBuffer's buffers as the pointers every write goes through. The
 * console's colour target is set when a frame begins (gl_hw_begin_frame's CB_COLOR0_BASE), so a
 * frame open on one buffer is submitted before draws move to the other. A draw into both
 * writes `fb_also` too wherever the CPU writes pixels, and on the GPU binds it as MRT1: its
 * own CB_COLOR1 surface registers, its half of CB_TARGET_MASK, its own CB_BLEND1_CONTROL, and
 * a pixel shader exporting to both (gl_draw.c). This comment claimed the opposite - that the
 * hardware had one colour target and dropped the second buffer with a log line - until
 * 2026-09-21; it had been out of date since the second target was written, and reading it is
 * what sent a hunt for `front-and-back`'s wrong front to the tiling of MRT1 rather than to its
 * blending. GL_NONE keeps the pointer and writes no colour (gl_color_writes). */
void gl_draw_targets(gl_context_t *ctx) {
    const unsigned bits = gl_color_buffer_bits(ctx->draw_buffer);
    uint32_t *primary = ctx->back_fb;
    uint32_t *also = NULL;
    if (bits == GL_OCB_FRONT && ctx->front_fb) {
        primary = ctx->front_fb;
    } else if (bits == (GL_OCB_FRONT | GL_OCB_BACK) && ctx->front_fb) {
        also = ctx->front_fb;
    }
    if (bits & GL_OCB_FRONT) ctx->front_pending = GL_TRUE;
#ifndef OOPS_HOST_BUILD
    if (primary != ctx->framebuffer && ctx->use_hardware && ctx->hw_frame_active) {
        gl_hw_flush(ctx);
    }
#endif
    ctx->framebuffer = primary;
    ctx->fb_also = also;
}

/* **The front on screen.** Whatever has been drawn into the front since it was last shown is
 * presented - oops_display_present, which leaves the display's framebuffer, the back, as it
 * is. Called after the frame is submitted, so the front holds every draw issued. A front
 * still being drawn into is presented again at the next flush. On the scanout path the front
 * *is* the buffer on screen, so what was drawn into it is shown already, as on the console
 * itself. */
void gl_front_present(gl_context_t *ctx) {
    if (!ctx->front_fb) return;
    const GLboolean drawing = (GLboolean)((gl_color_buffer_bits(ctx->draw_buffer) & GL_OCB_FRONT) != 0u);
    if (!ctx->front_pending && !drawing) return;
#ifndef OOPS_HOST_BUILD
    if (ctx->disp && !ctx->hw_rx) {
        (void)oops_display_present(ctx->disp, gl_color_read_source(ctx, ctx->front_fb));
    }
#endif
    ctx->front_pending = GL_FALSE;
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
    gl_front_present(ctx);
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
/* **The draws already built this frame are still pointing at the words about to be rewritten.**
 *
 * On hardware a draw does not execute when it is issued; it is built into the command buffer and
 * runs at the flush. The shader payload is one buffer shared by every draw in the frame, so
 * rewriting a patch slot changes the program those built draws will run. The last thing a program
 * does with a piece of shader state is usually switch it *off*, which retroactively removed it
 * from the draws that were supposed to have it:
 *
 *     glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f);
 *     glRectf(...);              // alpha 0.25 - must be discarded
 *     glRectf(...);              // alpha 0.75 - must survive
 *     glDisable(GL_ALPHA_TEST);  // <- patches both back to s_nop, before either has run
 *
 * Nothing was discarded, because by the time the GPU read the program there was no test in it.
 * That is gl1-probe's `alpha-test` failing on hardware while passing on the host, where a draw is
 * finished when it returns. The same applies within a frame rather than at the end of one: two
 * draws wanting different texture environments cannot share one payload either.
 *
 * Submitting first is what makes the built draws keep the program they were built with, and it is
 * the rule texture storage already follows in gl_tex_storage_release_sync. Only when the words
 * actually change - an unconditional flush would split a frame on every glPopAttrib, and on the
 * glDisable that follows a test which was never enabled.
 */
void gl_ps_sync_payload_edit(gl_context_t *ctx, const uint32_t *dst,
                             const uint32_t *words, size_t n) {
#ifndef OOPS_HOST_BUILD
    if (!ctx->use_hardware || !ctx->hw_frame_active) return;
    for (size_t i = 0; i < n; i++) {
        if (dst[i] != words[i]) {
            gl_hw_flush(ctx);
            return;
        }
    }
#else
    (void)ctx; (void)dst; (void)words; (void)n;
#endif
}

/* Both pixel shaders out of this core's caches after a patch: the payload is write-combined and
 * the command processor reads what has left the core. The untextured shader sits just below the
 * textured one, so one range covers both. */
void gl_ps_flush_shaders(gl_context_t *ctx) {
#if !defined(OOPS_HOST_BUILD) && defined(__x86_64__)
    /* Two ranges since 2026-09-20: the textured shader moved to 0x1000 to have room to grow, so
     * one range over both would flush a kilobyte of payload that neither occupies. **Three since
     * 2026-09-21**, the third being the slot a compiled GL 2.0 pixel shader is copied into - a
     * payload edit the GPU has not seen flushed is the previous shader running against this
     * draw's parameters, which is the same hazard the other two are flushed for. */
    const size_t ranges[3][2] = {
        {OOPS_GL_PS_UNTEX_OFFSET, OOPS_GL_PS_UNTEX_OFFSET + OOPS_GL_PS_UNTEX_WORDS * 4u},
        {OOPS_GL_PS_TEX_OFFSET, OOPS_GL_PS_TEX_OFFSET + OOPS_GL_PS_TEX_WORDS * 4u},
        {OOPS_GL_PS_GL2_OFFSET, OOPS_GL_PS_GL2_OFFSET + OOPS_GL_PS_GL2_WORDS * 4u},
    };
    for (int r = 0; r < 3; r++) {
        for (size_t p = ranges[r][0]; p < ranges[r][1]; p += 64) {
            __builtin_ia32_clflush((const void *)((const char *)ctx->gpu_payload + p));
        }
    }
#else
    (void)ctx;
#endif
}

/* The texture environment, as the four instructions that combine the sampled texel in v4..v7 with
 * the interpolated colour in v8..v11, leaving the result in v4..v7 where the export reads it.
 *
 * **Which four depend on the bound texture's base format as well as the mode** (since
 * 2026-09-19), because GL's texture functions do: a GL_ALPHA texture leaves the fragment's colour
 * alone, and GL_LUMINANCE and GL_RGB ones its alpha - the rules gl_draw.c's gl_tex_env_apply
 * states, from Mesa's `calculate_derived_texenv`. Each channel is one word:
 *
 * - the texel's own value: `s_nop 0` - it is already where the export reads;
 * - the fragment's: `v_mov_b32` from v8..v11;
 * - GL_MODULATE: `v_mul_f32`; GL_ADD: `v_add_f32`, the colour buffer's conversion to UNORM doing
 *   the clamp. An intensity texture's added alpha can exceed 1 as the alpha test sees it.
 *
 * Every word is from `tools/shader/tex-env.s`, assembled and read back, and the three multiplies
 * the shader was built with came out of the assembler identical - the cross-check.
 *
 * GL_DECAL of an RGBA texture (a lerp by the texel's alpha) and GL_BLEND (a lerp by the texel
 * towards the environment colour) are two and three instructions a channel and do not fit; nor
 * does GL_COMBINE, whose arguments, operands and scale alone outrun four words. They modulated on
 * the console until 2026-09-19; they are the general form below now, in a slot of sixty-four.
 */
enum { GL_CH_TEX, GL_CH_FRAG, GL_CH_MUL, GL_CH_ADD };

static uint32_t gl_ps_env_word(int op, int channel, const gl_ps_stage_t *st) {
    /* Generated rather than tabulated since 2026-09-20, so that one stage's words are the other's
     * with its own registers. The result is always v4 + c; one of the two operands is already
     * there - the texel for unit 0, the stage before it for unit 1 - and the other is the one
     * this names. So "take what is already in v4" is a nop for one stage and a move for the
     * other, which is why the two cases are written by register and not by name.
     *
     * test_pm4_gl_tex_env_reaches_the_combine_slot pins unit 0's four words against the table
     * these replace, which is the check that the encoder agrees with the assembler. */
    const uint32_t c = (uint32_t)channel;
    const uint32_t r = 4u + c;
    const uint32_t in_r = (st->texel == 4u) ? st->texel : st->prev; /* already in the result */
    const uint32_t other = (st->texel == 4u) ? st->prev : st->texel;
    const uint32_t tex = st->texel + c, prev = st->prev + c;
    switch (op) {
        case GL_CH_TEX:  /* the texel alone */
            return (st->texel == in_r) ? 0xbf800000u /* s_nop 0 */
                                       : gl_ps_v_mov(r, GL_PS_SRC_V(tex));
        case GL_CH_FRAG: /* what came in - the primary colour, or the stage before */
            return (st->prev == in_r) ? 0xbf800000u : gl_ps_v_mov(r, GL_PS_SRC_V(prev));
        case GL_CH_ADD:
            return gl_ps_vop2(GL_PS_V_ADD, r, GL_PS_SRC_V(r), other + c);
        default:
            return gl_ps_vop2(GL_PS_V_MUL, r, GL_PS_SRC_V(r), other + c);
    }
}

/* # The general form (since 2026-09-19)
 *
 * GL_BLEND, GL_DECAL of an RGBA texture and GL_COMBINE are a program, not a word per channel: the
 * combiner's arguments gathered into v16..v27 (argument i's channel c in v[16 + 4i + c]), its
 * function per channel into v4..v7, the scale, and a clamp to [0, 1] - gl_draw.c's gl_tex_combine
 * step for step, Mesa's emit_texenv (main/ff_fragment_shader.c:629-735) under both. GL_BLEND and
 * GL_DECAL become combiner settings first, as Mesa's calculate_derived_texenv (main/texstate.c:173)
 * makes them: GL_BLEND is GL_INTERPOLATE(constant, fragment, texel), an RGBA GL_DECAL
 * GL_INTERPOLATE(texel, fragment, texel alpha).
 *
 * The words come from the encoder in gl_internal.h, whose every form is checked against the
 * assembler. The longest program - three interpolated arguments from the constant colour, scaled
 * - is well under the slot's sixty-four words; one that did not fit would modulate and say so. */
typedef struct {
    uint32_t *w;
    size_t n, cap;
} gl_ps_emit_t;

static void gl_ps_put(gl_ps_emit_t *e, uint32_t word) {
    if (e->n < e->cap) e->w[e->n] = word;
    e->n++;
}

static int gl_combine_arg_count(GLenum mode) {
    return (mode == GL_REPLACE) ? 1 : (mode == GL_INTERPOLATE) ? 3 : 2;
}

/* One argument's channel c (3 for alpha) into v[16 + 4 * arg + c]: the source's channel, its
 * alpha, or one minus either. A single unit, so GL_PREVIOUS and GL_PRIMARY_COLOR are both the
 * fragment's colour and GL_TEXTURE0 is GL_TEXTURE, as gl_combine_arg reads them; the constant
 * colour, one-minus already applied, is a literal. */
static void gl_ps_combine_arg(gl_ps_emit_t *e, const gl_context_t *ctx, const gl_ps_stage_t *st,
                              uint32_t arg, uint32_t c, GLenum source, GLenum operand) {
    const uint32_t dst = 16u + 4u * arg + c;
    const GLboolean alpha = (GLboolean)(operand == GL_SRC_ALPHA || operand == GL_ONE_MINUS_SRC_ALPHA);
    const GLboolean invert = (GLboolean)(operand == GL_ONE_MINUS_SRC_COLOR ||
                                         operand == GL_ONE_MINUS_SRC_ALPHA);
    const uint32_t ch = alpha ? 3u : c;
    /* This stage's own texel, by either spelling. */
    const GLboolean own = (GLboolean)(source == GL_TEXTURE ||
                                      source == (GLenum)(GL_TEXTURE0 + st->unit));
    /* GL 1.4's crossbar - GL_TEXTUREn naming a *different* unit - reads as zero, the rule a unit
     * applying no texture follows here and in Mesa. It cannot be served even when that unit was
     * sampled: its stage has already overwritten v4..v7 with its result. */
    const GLboolean other_unit =
        (GLboolean)(!own && source >= GL_TEXTURE0 && source <= GL_TEXTURE0 + 31u);
    if (source == GL_CONSTANT || other_unit) {
        const float k = other_unit ? 0.0f : ctx->tex_unit[st->unit].tex_env_color[ch];
        gl_ps_put(e, gl_ps_v_mov(dst, GL_PS_SRC_LITERAL));
        gl_ps_put(e, gl_f32_bits(invert ? 1.0f - k : k));
        return;
    }
    const uint32_t src = (own ? st->texel : (source == GL_PREVIOUS ? st->prev : 8u)) + ch;
    gl_ps_put(e, invert ? gl_ps_vop2(GL_PS_V_SUB, dst, GL_PS_SRC_ONE, src)
                        : gl_ps_v_mov(dst, GL_PS_SRC_V(src)));
}

/* One channel of a combiner function, into v[4 + c], from the arguments' channel c. */
static void gl_ps_combine_fn(gl_ps_emit_t *e, GLenum mode, uint32_t c) {
    const uint32_t r = 4u + c, a0 = 16u + c, a1 = 20u + c, a2 = 24u + c;
    switch (mode) {
        case GL_REPLACE:
            gl_ps_put(e, gl_ps_v_mov(r, GL_PS_SRC_V(a0)));
            break;
        case GL_ADD:
            gl_ps_put(e, gl_ps_vop2(GL_PS_V_ADD, r, GL_PS_SRC_V(a0), a1));
            break;
        case GL_ADD_SIGNED:
            gl_ps_put(e, gl_ps_vop2(GL_PS_V_ADD, r, GL_PS_SRC_V(a0), a1));
            gl_ps_put(e, gl_ps_vop2(GL_PS_V_ADD, r, GL_PS_SRC_NEG_HALF, r));
            break;
        case GL_SUBTRACT:
            gl_ps_put(e, gl_ps_vop2(GL_PS_V_SUB, r, GL_PS_SRC_V(a0), a1));
            break;
        case GL_INTERPOLATE: /* a0 * a2 + a1 * (1 - a2), as a1 + (a0 - a1) * a2 */
            gl_ps_put(e, gl_ps_vop2(GL_PS_V_SUB, 13u, GL_PS_SRC_V(a0), a1));
            gl_ps_put(e, gl_ps_v_mov(r, GL_PS_SRC_V(a1)));
            gl_ps_put(e, gl_ps_vop2(GL_PS_V_FMAC, r, GL_PS_SRC_V(13u), a2));
            break;
        default: /* GL_MODULATE */
            gl_ps_put(e, gl_ps_vop2(GL_PS_V_MUL, r, GL_PS_SRC_V(a0), a1));
            break;
    }
}

/* The combiner's scale for one channel: 1, 2 or 4, the only values glTexEnv accepts. */
static void gl_ps_combine_scale(gl_ps_emit_t *e, float scale, uint32_t c) {
    if (scale == 2.0f) gl_ps_put(e, gl_ps_vop2(GL_PS_V_MUL, 4u + c, GL_PS_SRC_TWO, 4u + c));
    else if (scale == 4.0f) gl_ps_put(e, gl_ps_vop2(GL_PS_V_MUL, 4u + c, GL_PS_SRC_FOUR, 4u + c));
}

/* The whole general program into `w`; answers its length, which may exceed `cap`. */
size_t gl_ps_combine_program(const gl_context_t *ctx, const gl_ps_stage_t *st,
                             const gl_combine_t *cb, uint32_t *w, size_t cap) {
    gl_ps_emit_t e = {w, 0, cap};
    const GLboolean dot = (GLboolean)(cb->mode_rgb == GL_DOT3_RGB || cb->mode_rgb == GL_DOT3_RGBA);
    const GLboolean dot_rgba = (GLboolean)(cb->mode_rgb == GL_DOT3_RGBA);
    const int n_rgb = gl_combine_arg_count(cb->mode_rgb);
    const int n_alpha = dot_rgba ? 0 : gl_combine_arg_count(cb->mode_alpha);

    /* Every argument before any function, since the functions overwrite the texel's registers. */
    for (int a = 0; a < n_rgb; a++) {
        for (uint32_t c = 0; c < 3u; c++) {
            gl_ps_combine_arg(&e, ctx, st, (uint32_t)a, c, cb->source_rgb[a], cb->operand_rgb[a]);
        }
    }
    for (int a = 0; a < n_alpha; a++) {
        gl_ps_combine_arg(&e, ctx, st, (uint32_t)a, 3u, cb->source_alpha[a], cb->operand_alpha[a]);
    }

    if (dot) {
        /* (2a - 1).(2b - 1) as 4 * sum (a - 1/2)(b - 1/2), into v4 and copied across. */
        for (uint32_t k = 0; k < 3u; k++) {
            gl_ps_put(&e, gl_ps_vop2(GL_PS_V_ADD, 13u, GL_PS_SRC_NEG_HALF, 16u + k));
            gl_ps_put(&e, gl_ps_vop2(GL_PS_V_ADD, 14u, GL_PS_SRC_NEG_HALF, 20u + k));
            gl_ps_put(&e, gl_ps_vop2(k == 0u ? GL_PS_V_MUL : GL_PS_V_FMAC, 4u, GL_PS_SRC_V(13u), 14u));
        }
        gl_ps_put(&e, gl_ps_vop2(GL_PS_V_MUL, 4u, GL_PS_SRC_FOUR, 4u));
        gl_ps_put(&e, gl_ps_v_mov(5u, GL_PS_SRC_V(4u)));
        gl_ps_put(&e, gl_ps_v_mov(6u, GL_PS_SRC_V(4u)));
        if (dot_rgba) gl_ps_put(&e, gl_ps_v_mov(7u, GL_PS_SRC_V(4u)));
    } else {
        for (uint32_t c = 0; c < 3u; c++) gl_ps_combine_fn(&e, cb->mode_rgb, c);
    }
    if (!dot_rgba) gl_ps_combine_fn(&e, cb->mode_alpha, 3u);

    for (uint32_t c = 0; c < 3u; c++) gl_ps_combine_scale(&e, cb->scale_rgb, c);
    gl_ps_combine_scale(&e, cb->scale_alpha, 3u);
    for (uint32_t c = 0; c < 4u; c++) {
        gl_ps_put(&e, gl_ps_vop2(GL_PS_V_MAX, 4u + c, GL_PS_SRC_ZERO, 4u + c));
        gl_ps_put(&e, gl_ps_vop2(GL_PS_V_MIN, 4u + c, GL_PS_SRC_ONE, 4u + c));
    }
    return e.n;
}

/* GL_BLEND and GL_DECAL as combiner settings, for the general form - gl_draw.c's
 * gl_tex_env_apply, restated. Only the modes the short form cannot express come here. */
static void gl_tex_env_as_combine(GLenum mode, GLenum base, gl_combine_t *cb) {
    const GLboolean keep_rgb = (GLboolean)(base == GL_ALPHA ||
                                           (mode == GL_DECAL && base != GL_RGB && base != GL_RGBA));
    const GLboolean keep_a = (GLboolean)(base == GL_LUMINANCE || base == GL_RGB || mode == GL_DECAL);
    memset(cb, 0, sizeof(*cb));
    cb->scale_rgb = 1.0f;
    cb->scale_alpha = 1.0f;
    for (int i = 0; i < 3; i++) {
        cb->operand_rgb[i] = GL_SRC_COLOR;
        cb->operand_alpha[i] = GL_SRC_ALPHA;
    }
    /* **GL_PREVIOUS, not GL_PRIMARY_COLOR** (corrected 2026-09-20). The fixed environment
     * functions combine the texel with "the incoming fragment colour" (GL 1.3, table 3.18), which
     * at unit n is the *result of unit n-1* - GL_PREVIOUS. The two are the same thing at unit 0,
     * which is why writing GL_PRIMARY_COLOR here was harmless while one unit was applied; at unit
     * 1 it is the difference between a lightmap modulating the base map and a lightmap
     * modulating the vertex colour with the base map thrown away. */
    cb->source_rgb[0] = GL_PREVIOUS;
    if (keep_rgb) {
        cb->mode_rgb = GL_REPLACE;
    } else if (mode == GL_DECAL) { /* RGBA: texel * texel alpha + fragment * (1 - texel alpha) */
        cb->mode_rgb = GL_INTERPOLATE;
        cb->source_rgb[0] = GL_TEXTURE;
        cb->source_rgb[1] = GL_PREVIOUS;
        cb->source_rgb[2] = GL_TEXTURE;
        cb->operand_rgb[2] = GL_SRC_ALPHA;
    } else if (mode == GL_BLEND) { /* constant * texel + fragment * (1 - texel) */
        cb->mode_rgb = GL_INTERPOLATE;
        cb->source_rgb[0] = GL_CONSTANT;
        cb->source_rgb[1] = GL_PREVIOUS;
        cb->source_rgb[2] = GL_TEXTURE;
    } else if (mode == GL_REPLACE) {
        cb->mode_rgb = GL_REPLACE;
        cb->source_rgb[0] = GL_TEXTURE;
    } else {
        cb->mode_rgb = (mode == GL_ADD) ? GL_ADD : GL_MODULATE;
        cb->source_rgb[1] = GL_TEXTURE;
    }
    cb->source_alpha[0] = GL_PREVIOUS;
    if (keep_a) {
        cb->mode_alpha = GL_REPLACE;
    } else if (mode == GL_REPLACE) {
        cb->mode_alpha = GL_REPLACE;
        cb->source_alpha[0] = GL_TEXTURE;
    } else if (mode == GL_BLEND && base == GL_INTENSITY) {
        cb->mode_alpha = GL_INTERPOLATE;
        cb->source_alpha[0] = GL_CONSTANT;
        cb->source_alpha[1] = GL_PREVIOUS;
        cb->source_alpha[2] = GL_TEXTURE;
    } else {
        cb->mode_alpha = (mode == GL_ADD && base == GL_INTENSITY) ? GL_ADD : GL_MODULATE;
        cb->source_alpha[1] = GL_TEXTURE;
    }
}

/* A combine slot written, if it is not already what it should be. Asked on every textured draw,
 * so a draw that changes nothing writes nothing; a change submits the draws built with the old
 * words first (gl_ps_sync_payload_edit). */
static void gl_ps_env_write(gl_context_t *ctx, size_t slot, const uint32_t *words) {
    uint32_t *const ps_tex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);
    if (memcmp(ps_tex + slot, words, GL_PS_COMBINE_WORDS * sizeof(uint32_t)) == 0) return;
    gl_ps_sync_payload_edit(ctx, ps_tex + slot, words, GL_PS_COMBINE_WORDS);
    for (size_t i = 0; i < GL_PS_COMBINE_WORDS; i++) ps_tex[slot + i] = words[i];
    gl_ps_flush_shaders(ctx);
}

GLboolean gl_ps_patch_tex_env(gl_context_t *ctx) {
    if (!ctx || !ctx->gpu_payload) return GL_TRUE;

    /* Unit 0's stage: its texel is the sample in v4..v7, and GL_PREVIOUS at unit 0 is the
     * primary colour (GL 1.3, 3.8.13), so both fall on v8..v11. */
    static const gl_ps_stage_t stage0 = {4u, 8u, 0u};
    const gl_ps_stage_t *const st = &stage0;
    const gl_texture_object_t *t = gl_lookup_texture(ctx, gl_effective_texture_id(ctx));
    const GLenum base = gl_tex_sample_format(t);
    const GLenum mode = ctx->tex_unit[st->unit].tex_env_mode;
    GLboolean exact = GL_TRUE;
    int rgb = GL_CH_MUL, alpha = GL_CH_MUL;
    if (base == GL_ALPHA || (mode == GL_DECAL && base != GL_RGB && base != GL_RGBA)) {
        rgb = GL_CH_FRAG;
    } else if (mode == GL_REPLACE || (mode == GL_DECAL && base == GL_RGB)) {
        rgb = GL_CH_TEX;
    } else if (mode == GL_ADD) {
        rgb = GL_CH_ADD;
    } else if (mode == GL_DECAL || mode == GL_BLEND || mode == GL_COMBINE) {
        exact = GL_FALSE;
    }
    if (base == GL_LUMINANCE || base == GL_RGB || mode == GL_DECAL) {
        alpha = GL_CH_FRAG;
    } else if (mode == GL_REPLACE) {
        alpha = GL_CH_TEX;
    } else if (mode == GL_ADD && base == GL_INTENSITY) {
        alpha = GL_CH_ADD;
    } else if ((mode == GL_BLEND && base == GL_INTENSITY) || mode == GL_COMBINE) {
        exact = GL_FALSE;
    }

    uint32_t words[GL_PS_COMBINE_WORDS];
    size_t n = 0;
    if (exact) {
        /* The short form: a word per channel, and - new with the longer slot - GL_ADD's sums
         * clamped, which the colour buffer's conversion did alone before, too late for fog and
         * the alpha test. A sum of two values in [0, 1] needs only the upper bound. */
        words[0] = gl_ps_env_word(rgb, 0, st);
        words[1] = gl_ps_env_word(rgb, 1, st);
        words[2] = gl_ps_env_word(rgb, 2, st);
        words[3] = gl_ps_env_word(alpha, 3, st);
        n = 4;
        for (uint32_t c = 0; c < 4u; c++) {
            if ((c < 3u ? rgb : alpha) == GL_CH_ADD) {
                words[n++] = gl_ps_vop2(GL_PS_V_MIN, 4u + c, GL_PS_SRC_ONE, 4u + c);
            }
        }
    } else {
        gl_combine_t derived;
        const gl_combine_t *cb = &ctx->tex_unit[st->unit].combine;
        if (mode != GL_COMBINE) {
            gl_tex_env_as_combine(mode, base, &derived);
            cb = &derived;
        }
        n = gl_ps_combine_program(ctx, st, cb, words, GL_PS_COMBINE_WORDS);
        exact = GL_TRUE;
        if (n > GL_PS_COMBINE_WORDS) { /* cannot happen with GL's argument count; modulate if so */
            for (uint32_t c = 0; c < 4u; c++) words[c] = gl_ps_env_word(GL_CH_MUL, (int)c, st);
            n = 4;
            exact = GL_FALSE;
        }
    }
    /* The rest of the slot is skipped, not executed. */
    if (n < GL_PS_COMBINE_WORDS) {
        words[n] = gl_ps_s_branch((uint32_t)(GL_PS_COMBINE_WORDS - n - 1u));
        for (size_t i = n + 1u; i < GL_PS_COMBINE_WORDS; i++) words[i] = 0xbf800000u; /* s_nop 0 */
    }
    gl_ps_env_write(ctx, GL_PS_COMBINE_SLOT_TEX, words);
    return exact;
}

/*
 * The second unit's combine stage: unit 1's texel against what unit 0's stage left in v4..v7.
 *
 * The same encoder, the same sixty-four words, one `gl_ps_stage_t` apart - which is the whole
 * point of having parameterised it. Off is a branch over the slot.
 *
 * **Hardware-validated 2026-09-20**: gl1-probe's `multitexture` passes on the console, which is
 * the first time this slot has run there - a red base under a blue `GL_ADD` unit, magenta.
 */
GLboolean gl_ps_patch_tex_env_unit1(gl_context_t *ctx, GLboolean on) {
    if (!ctx || !ctx->gpu_payload) return GL_TRUE;
    /* Unit 1's stage: its texel is where tex-prolog2.s leaves it, and GL_PREVIOUS is unit 0's
     * result in v4..v7. */
    static const gl_ps_stage_t stage1 = {28u, 4u, 1u};
    uint32_t words[GL_PS_COMBINE_WORDS];
    GLboolean exact = GL_TRUE;
    if (!on) {
        words[0] = gl_ps_s_branch(GL_PS_COMBINE_WORDS - 1u);
        for (size_t i = 1; i < GL_PS_COMBINE_WORDS; i++) words[i] = 0xbf800000u; /* s_nop 0 */
    } else {
        const gl_texture_object_t *t =
            gl_lookup_texture(ctx, gl_unit_texture_id(ctx, stage1.unit));
        gl_combine_t derived;
        const GLenum base = gl_tex_sample_format(t);
        const GLenum mode = ctx->tex_unit[stage1.unit].tex_env_mode;
        const gl_combine_t *cb = &ctx->tex_unit[stage1.unit].combine;
        if (mode != GL_COMBINE) {
            gl_tex_env_as_combine(mode, base, &derived);
            cb = &derived;
        }
        /* Always the general form here: the short form's saving is that the texel is already in
         * the result register, which is true of unit 0's stage and not of this one. */
        size_t n = gl_ps_combine_program(ctx, &stage1, cb, words, GL_PS_COMBINE_WORDS);
        if (n > GL_PS_COMBINE_WORDS) {
            for (uint32_t c = 0; c < 4u; c++) words[c] = gl_ps_env_word(GL_CH_MUL, (int)c, &stage1);
            n = 4;
            exact = GL_FALSE;
        }
        if (n < GL_PS_COMBINE_WORDS) {
            words[n] = gl_ps_s_branch((uint32_t)(GL_PS_COMBINE_WORDS - n - 1u));
            for (size_t i = n + 1u; i < GL_PS_COMBINE_WORDS; i++) words[i] = 0xbf800000u;
        }
    }
    gl_ps_env_write(ctx, GL_PS_COMBINE2_SLOT_TEX, words);
    return exact;
}

/* Fog on the console: GL's blend towards the fog colour, c * f + fog * (1 - f) on red, green and
 * blue, alpha untouched (GL 1.x, 3.10). RDNA2 has no fixed-function fog, so it is twelve words in
 * each pixel shader, between the colour becoming final and the alpha test.
 *
 * **The factor is the vertex's, carried in the texture coordinate's spare z**: gl_draw.c computes
 * it per vertex on the CPU - where the software path computes it, from the eye distance or GL
 * 1.4's fog coordinate, clamped - and the vertex shader exports that parameter as a full vec4
 * already, so the shader interface does not move: SPI_VS_OUT_CONFIG, SPI_PS_IN_CONTROL and the
 * input slots `test_pm4_gl_honours_the_gl_cube_oracle_record` pins are as they were. Both shaders
 * interpolate it as attr1.z, perspective-correct as the software path's is.
 *
 * **The fog colour is three literals**, patched in place as the alpha test's reference is, so a
 * glFog colour change mid-frame submits the draws built with the old one first
 * (gl_ps_sync_payload_edit). Every word is from `tools/shader/fog.s`, assembled for gfx1030 and
 * read back; the file also assembles the textured shader's own `attr1.x` interpolation, which came
 * out as the word already in the tree. Written 2026-09-19; `fog` and `fog-coord` both pass on a
 * console as of 2026-09-20. */
void gl_ps_patch_fog(gl_context_t *ctx) {
    if (!ctx || !ctx->gpu_payload) return;

    uint32_t words[GL_PS_FOG_WORDS];
    for (size_t i = 0; i < GL_PS_FOG_WORDS; i++) words[i] = 0xbf800000u; /* s_nop 0 */
    if (ctx->cap_fog) {
        words[0]  = 0xc8340600u; /* v_interp_p1_f32 v13, v0, attr1.z (the factor) */
        words[1]  = 0xc8350601u; /* v_interp_p2_f32 v13, v1, attr1.z */
        words[2]  = 0x081c1af2u; /* v_sub_f32 v14, 1.0, v13 */
        words[3]  = 0x10081b04u; /* v_mul_f32 v4, v4, v13 */
        words[4]  = 0x56081cffu; /* v_fmac_f32 v4, <literal>, v14 */
        words[5]  = gl_f32_bits(ctx->fog_color[0]);
        words[6]  = 0x100a1b05u; /* v_mul_f32 v5, v5, v13 */
        words[7]  = 0x560a1cffu; /* v_fmac_f32 v5, <literal>, v14 */
        words[8]  = gl_f32_bits(ctx->fog_color[1]);
        words[9]  = 0x100c1b06u; /* v_mul_f32 v6, v6, v13 */
        words[10] = 0x560c1cffu; /* v_fmac_f32 v6, <literal>, v14 */
        words[11] = gl_f32_bits(ctx->fog_color[2]);
    }

    uint32_t *ps_untex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_UNTEX_OFFSET);
    uint32_t *ps_tex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);
    if (memcmp(ps_untex + GL_PS_FOG_SLOT_UNTEX, words, sizeof(words)) == 0 &&
        memcmp(ps_tex + GL_PS_FOG_SLOT_TEX, words, sizeof(words)) == 0) {
        return;
    }
    /* A built frame still points at these words - see gl_ps_sync_payload_edit. */
    gl_ps_sync_payload_edit(ctx, ps_untex + GL_PS_FOG_SLOT_UNTEX, words, GL_PS_FOG_WORDS);
    gl_ps_sync_payload_edit(ctx, ps_tex + GL_PS_FOG_SLOT_TEX, words, GL_PS_FOG_WORDS);
    for (size_t i = 0; i < GL_PS_FOG_WORDS; i++) {
        ps_untex[GL_PS_FOG_SLOT_UNTEX + i] = words[i];
        ps_tex[GL_PS_FOG_SLOT_TEX + i] = words[i];
    }
    gl_ps_flush_shaders(ctx);
}

void gl_ps_patch_sum(gl_context_t *ctx, GLboolean on) {
    if (!ctx || !ctx->gpu_payload) return;
    uint32_t words[GL_PS_SUM_WORDS];
    for (size_t i = 0; i < GL_PS_SUM_WORDS; i++) words[i] = 0xbf800000u; /* s_nop 0 */
    if (on) {
        /* tools/shader/colour-sum.s */
        static const uint32_t sum[GL_PS_SUM_WORDS] = {
            0xc8300800u, /* v_interp_p1_f32 v12, v0, attr2.x (secondary red) */
            0xc8310801u, /* v_interp_p2_f32 v12, v1, attr2.x */
            0xc8340900u, /* v_interp_p1_f32 v13, v0, attr2.y (green) */
            0xc8350901u, /* v_interp_p2_f32 v13, v1, attr2.y */
            0xc8380a00u, /* v_interp_p1_f32 v14, v0, attr2.z (blue) */
            0xc8390a01u, /* v_interp_p2_f32 v14, v1, attr2.z */
            0xd5038004u, 0x00021904u, /* v_add_f32_e64 v4, v4, v12 clamp */
            0xd5038005u, 0x00021b05u, /* v_add_f32_e64 v5, v5, v13 clamp */
            0xd5038006u, 0x00021d06u, /* v_add_f32_e64 v6, v6, v14 clamp */
        };
        memcpy(words, sum, sizeof(words));
    }
    uint32_t *ps_tex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);
    if (memcmp(ps_tex + GL_PS_SUM_SLOT_TEX, words, sizeof(words)) == 0) return;
    /* A built frame still points at these words - see gl_ps_sync_payload_edit. */
    gl_ps_sync_payload_edit(ctx, ps_tex + GL_PS_SUM_SLOT_TEX, words, GL_PS_SUM_WORDS);
    memcpy(ps_tex + GL_PS_SUM_SLOT_TEX, words, sizeof(words));
    gl_ps_flush_shaders(ctx);
}

/*
 * **How unit 0's texture is sampled**: a 2D image, or a cube map.
 *
 * A cube map is not a sample with an extra coordinate. The texture coordinate is a *direction*,
 * and the sampler wants the face it points at and the place on that face - which RDNA2 computes
 * in four instructions of its own, `V_CUBEID_F32`, `V_CUBESC_F32`, `V_CUBETC_F32` and
 * `V_CUBEMA_F32`. The shader divides the two coordinates by twice the major axis, biases them by
 * a half, and hands the sampler `(u, v, face)` with `dim:SQ_RSRC_IMG_CUBE`.
 * `tools/shader/tex-cube.s` is the sequence, and its two cross-check instructions - the 2D sample
 * this replaces and the prolog's own `attr1.x` interpolation - assembled to the words already in
 * the tree.
 *
 * **The direction is interpolated again rather than reused.** The prolog divides s and t by q
 * before this runs, which is right for a 2D sample and wrong for a direction: dividing all three
 * components would leave the direction unchanged, but dividing two of them does not. The third
 * component is `attr2.w`, where the vertex carries unit 0's r - so a cube-textured draw runs at
 * least the three-parameter vertex shader, which gl_draw.c arranges.
 *
 * obSCEne's `REQ-20260920T0745Z-6c80` measured the receiving side: a cube sampled on this part
 * under `TYPE 0xb`, returning the texel of the face it names. The descriptor is
 * gl_pack_descriptors', the array of six faces is gl_tex_cube_upload's, and this is the sample.
 */
/*
 * **Antialiasing's coverage**, in the untextured pixel shader's slot (since 2026-09-20).
 *
 * `tools/shader/coverage.s`, whose two cross-check instructions - this shader's own
 * interpolation of the red channel and the lane kill the alpha test ends with - assembled to
 * words already in the tree. What the fifteen words do is read the offset from the primitive's
 * centre out of the texture-coordinate parameter, weigh the alpha by GL's coverage, and kill the
 * fragments the primitive misses entirely, which is what the software rasteriser does with them.
 *
 * A branch over the slot is no smoothing, and is what a frame that never smooths carries - so
 * the stream the gl-cube oracle record pins is untouched.
 */
void gl_ps_patch_coverage(gl_context_t *ctx, GLboolean on) {
    gl_ps_patch_coverage_where(ctx, on ? GL_COVERAGE_UNTEXTURED : GL_COVERAGE_OFF);
}

/*
 * **Which shader smooths, and from which interpolant.**
 *
 * The arithmetic is one program; only the parameter it reads differs, and only in the ATTR
 * field of its six interpolations. `tools/shader/coverage-tex.s` assembles both forms and its
 * seven cross-check lines came back as words already in the tree - `coverage.s`'s own six
 * interpolations and the lane kill the alpha test ends with - so the claim that the two
 * programs differ by nothing else is checked rather than asserted.
 *
 * Both slots are written on every call, so a draw that stops smoothing, or moves between
 * textured and not, leaves the other shader branching over its slot rather than weighting an
 * alpha by an offset nothing wrote.
 */
void gl_ps_patch_coverage_where(gl_context_t *ctx, gl_coverage_kind_t kind) {
    if (!ctx || !ctx->gpu_payload) return;
    /* The fifteen words, twice: `attr1` for the untextured shader, `attr3` for the textured
     * one. Every word but the six interpolations is identical, and each interpolation differs
     * by 0x800 - the ATTR field is bits [15:10], so attr1 to attr3 is +2 there. */
    static const uint32_t code_untex[15] = {
        0xc8300400u, 0xc8310401u, /* v_interp_p1/p2_f32 v12, attr1.x - the offset across */
        0xc8340500u, 0xc8350501u, /* v_interp_p1/p2_f32 v13, attr1.y - and along */
        0xc8380700u, 0xc8390701u, /* v_interp_p1/p2_f32 v14, attr1.w - the radius plus a half */
        0x1018190cu,              /* v_mul_f32_e32 v12, v12, v12 */
        0xd54b000cu, 0x04321b0du, /* v_fma_f32 v12, v13, v13, v12 */
        0x7e18670cu,              /* v_sqrt_f32_e32 v12, v12 */
        0x0818190eu,              /* v_sub_f32_e32 v12, v14, v12 */
        0x1e1818f2u,              /* v_min_f32_e32 v12, 1.0, v12 */
        0x7c021880u,              /* v_cmp_lt_f32_e32 vcc_lo, 0, v12 */
        0x877e6a7eu,              /* s_and_b32 exec_lo, exec_lo, vcc_lo */
        0x100e1907u,              /* v_mul_f32_e32 v7, v7, v12 */
    };
    static const uint32_t code_tex[15] = {
        0xc8300c00u, 0xc8310c01u, /* v_interp_p1/p2_f32 v12, attr3.x - the offset across */
        0xc8340d00u, 0xc8350d01u, /* v_interp_p1/p2_f32 v13, attr3.y - and along */
        0xc8380f00u, 0xc8390f01u, /* v_interp_p1/p2_f32 v14, attr3.w - the radius plus a half */
        0x1018190cu,              /* v_mul_f32_e32 v12, v12, v12 */
        0xd54b000cu, 0x04321b0du, /* v_fma_f32 v12, v13, v13, v12 */
        0x7e18670cu,              /* v_sqrt_f32_e32 v12, v12 */
        0x0818190eu,              /* v_sub_f32_e32 v12, v14, v12 */
        0x1e1818f2u,              /* v_min_f32_e32 v12, 1.0, v12 */
        0x7c021880u,              /* v_cmp_lt_f32_e32 vcc_lo, 0, v12 */
        0x877e6a7eu,              /* s_and_b32 exec_lo, exec_lo, vcc_lo */
        0x100e1907u,              /* v_mul_f32_e32 v7, v7, v12 */
    };

    /*
     * **A smooth polygon's coverage**, `tools/shader/coverage-poly.s`: the product of three edge
     * fades rather than one distance. The three signed distances arrive as `d*w` in `attr3`'s x,
     * y and z with `w` in its own w, and the reciprocal below turns them into the screen-linear
     * distances the fade needs - the identity the textured prolog already uses for `q`, because
     * `v_interp` is perspective-correct and a distance to a line is not.
     *
     * Twenty-six words, and the same program in both shaders: it reads `attr3` either way, so
     * where a point or a line needs one form per shader a polygon needs one at all. Its four
     * cross-check lines came back as words already in the tree - `coverage.s`'s alpha weighting
     * and lane kill, and `coverage-tex.s`'s interpolation of `attr3.x`.
     */
    static const uint32_t code_poly[26] = {
        0xc8300c00u, 0xc8310c01u, /* v_interp_p1/p2_f32 v12, attr3.x - d0 * w */
        0xc8340d00u, 0xc8350d01u, /* v_interp_p1/p2_f32 v13, attr3.y - d1 * w */
        0xc8380e00u, 0xc8390e01u, /* v_interp_p1/p2_f32 v14, attr3.z - d2 * w */
        0xc83c0f00u, 0xc83d0f01u, /* v_interp_p1/p2_f32 v15, attr3.w - w */
        0x7e1e550fu,              /* v_rcp_f32 v15, v15 */
        0x10181f0cu,              /* v_mul_f32 v12, v12, v15 - d0 */
        0x101a1f0du,              /* v_mul_f32 v13, v13, v15 - d1 */
        0x101c1f0eu,              /* v_mul_f32 v14, v14, v15 - d2 */
        0x061818f0u,              /* v_add_f32 v12, 0.5, v12 */
        0x061a1af0u,              /* v_add_f32 v13, 0.5, v13 */
        0x061c1cf0u,              /* v_add_f32 v14, 0.5, v14 */
        0xd557000cu, 0x03c9010cu, /* v_med3_f32 v12, v12, 0, 1.0 - the clamp */
        0xd557000du, 0x03c9010du, /* v_med3_f32 v13, v13, 0, 1.0 */
        0xd557000eu, 0x03c9010eu, /* v_med3_f32 v14, v14, 0, 1.0 */
        0x10181b0cu,              /* v_mul_f32 v12, v12, v13 */
        0x10181d0cu,              /* v_mul_f32 v12, v12, v14 - the three fades multiplied */
        0x7c021880u,              /* v_cmp_lt_f32_e32 vcc_lo, 0, v12 */
        0x877e6a7eu,              /* s_and_b32 exec_lo, exec_lo, vcc_lo */
        0x100e1907u,              /* v_mul_f32_e32 v7, v7, v12 */
    };

    uint32_t off_words[GL_PS_COVERAGE_WORDS];
    off_words[0] = gl_ps_s_branch(GL_PS_COVERAGE_WORDS - 1u);
    for (size_t i = 1; i < GL_PS_COVERAGE_WORDS; i++) off_words[i] = 0xbf800000u; /* s_nop 0 */

    uint32_t untex_words[GL_PS_COVERAGE_WORDS], tex_words[GL_PS_COVERAGE_WORDS];
    memcpy(untex_words, off_words, sizeof(off_words));
    memcpy(tex_words, off_words, sizeof(off_words));
    if (kind == GL_COVERAGE_UNTEXTURED) {
        memcpy(untex_words, code_untex, sizeof(code_untex));
    } else if (kind == GL_COVERAGE_TEXTURED) {
        memcpy(tex_words, code_tex, sizeof(code_tex));
    } else if (kind == GL_COVERAGE_POLYGON_UNTEX) {
        memcpy(untex_words, code_poly, sizeof(code_poly));
    } else if (kind == GL_COVERAGE_POLYGON_TEX) {
        memcpy(tex_words, code_poly, sizeof(code_poly));
    }

    uint32_t *const ps_untex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_UNTEX_OFFSET);
    uint32_t *const ps_tex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);
    uint32_t *const slot_u = ps_untex + GL_PS_COVERAGE_SLOT_UNTEX;
    uint32_t *const slot_t = ps_tex + GL_PS_COVERAGE_SLOT_TEX;
    const GLboolean u_same = (GLboolean)(memcmp(slot_u, untex_words, sizeof(untex_words)) == 0);
    const GLboolean t_same = (GLboolean)(memcmp(slot_t, tex_words, sizeof(tex_words)) == 0);
    if (u_same && t_same) return;
    if (!u_same) {
        gl_ps_sync_payload_edit(ctx, slot_u, untex_words, GL_PS_COVERAGE_WORDS);
        memcpy(slot_u, untex_words, sizeof(untex_words));
    }
    if (!t_same) {
        gl_ps_sync_payload_edit(ctx, slot_t, tex_words, GL_PS_COVERAGE_WORDS);
        memcpy(slot_t, tex_words, sizeof(tex_words));
    }
    gl_ps_flush_shaders(ctx);
}

void gl_ps_patch_sample(gl_context_t *ctx, gl_ps_sample_kind_t kind, GLenum depth_mode) {
    if (!ctx || !ctx->gpu_payload) return;
    uint32_t words[GL_PS_SAMPLE_WORDS];
    for (size_t i = 0; i < GL_PS_SAMPLE_WORDS; i++) words[i] = 0xbf800000u; /* s_nop 0 */
    if (kind == GL_PS_SAMPLE_DEPTH || kind == GL_PS_SAMPLE_SHADOW) {
        /* tools/shader/tex-shadow.s. **One value comes back, not four.** A depth texel is a
         * single float and a comparison's result is a single 0 or 1, so `dmask` is 0x1 and the
         * value lands in v4 alone; the moves below spread it across v4..v7 the way
         * GL_DEPTH_TEXTURE_MODE says, which is what the texture environment then reads as an
         * ordinary texel. The waitcnt is the slot's own because those moves read what the sample
         * wrote and the shader's next one is a word past the end of this slot - the outer one
         * then costs nothing.
         *
         * **The comparison is the sampler's.** SQ_IMG_SAMP_WORD0's DEPTH_COMPARE_FUNC already
         * carries GL_TEXTURE_COMPARE_FUNC (gl_pack_descriptors, measured by `-6c80`); what the
         * shadow form adds is the reference - r/q, clamped to [0, 1] as GL 1.4 3.8.14 and
         * softpipe both clamp it - as the **first** address register, ahead of s and t, which is
         * the VADDR order the ISA gives for the `_c` forms. */
        size_t n = 0;
        if (kind == GL_PS_SAMPLE_SHADOW) {
            words[n++] = 0xc8400b00u; /* v_interp_p1_f32 v16, v0, attr2.w (r) */
            words[n++] = 0xc8410b01u; /* v_interp_p2_f32 v16, v1, attr2.w */
            words[n++] = 0x10201910u; /* v_mul_f32_e32 v16, v16, v12 - r/q */
            words[n++] = 0x20202080u; /* v_max_f32_e32 v16, 0, v16 */
            words[n++] = 0x1e2020f2u; /* v_min_f32_e32 v16, 1.0, v16 */
            words[n++] = 0x7e220302u; /* v_mov_b32_e32 v17, v2 - s/q */
            words[n++] = 0x7e240303u; /* v_mov_b32_e32 v18, v3 - t/q */
            words[n++] = 0xf0a00108u; /* image_sample_c v4, v[16:18], ... dmask:0x1 */
            words[n++] = 0x00610410u;
        } else {
            words[n++] = 0xf0800108u; /* image_sample v4, v[2:3], ... dmask:0x1 */
            words[n++] = 0x00610402u;
        }
        words[n++] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
        if (depth_mode == GL_ALPHA) {
            /* (0, 0, 0, v) - the alpha move first, because the three zeroes overwrite v4. */
            words[n++] = 0x7e0e0304u; /* v_mov_b32_e32 v7, v4 */
            words[n++] = 0x7e080280u; /* v_mov_b32_e32 v4, 0 */
            words[n++] = 0x7e0a0280u; /* v_mov_b32_e32 v5, 0 */
            words[n++] = 0x7e0c0280u; /* v_mov_b32_e32 v6, 0 */
        } else {
            words[n++] = 0x7e0a0304u; /* v_mov_b32_e32 v5, v4 */
            words[n++] = 0x7e0c0304u; /* v_mov_b32_e32 v6, v4 */
            /* (v, v, v, v) for GL_INTENSITY, (v, v, v, 1) for GL_LUMINANCE, the default. */
            words[n++] = (depth_mode == GL_INTENSITY) ? 0x7e0e0304u  /* v_mov_b32_e32 v7, v4 */
                                                      : 0x7e0e02f2u; /* v_mov_b32_e32 v7, 1.0 */
        }
        words[n] = gl_ps_s_branch((uint32_t)(GL_PS_SAMPLE_WORDS - n - 1u));
    } else if (kind == GL_PS_SAMPLE_2D) {
        words[0] = 0xf0800f08u; /* image_sample v[4:7], v[2:3], s[4:11], s[12:15] 2D */
        words[1] = 0x00610402u;
        words[2] = gl_ps_s_branch(GL_PS_SAMPLE_WORDS - 3u);
    } else if (kind == GL_PS_SAMPLE_3D) {
        /* tools/shader/tex-3d.s. **The third coordinate is divided and the direction of a cube
         * map is not**: a volume's (s, t, r) is a position GL 1.2 divides by q like the other
         * two, and the prolog left 1/q in v12 and never overwrote it, so the divide is one
         * multiply. v2 and v3 are copied up beside it because the address registers have to be
         * consecutive and v4 is the texel's own. */
        static const uint32_t code[7] = {
            0xc8480b00u, 0xc8490b01u, /* v_interp_p1/p2_f32 v18, attr2.w (r) */
            0x10241912u,              /* v_mul_f32_e32 v18, v18, v12 - r/q */
            0x7e200302u,              /* v_mov_b32_e32 v16, v2 - s/q */
            0x7e220303u,              /* v_mov_b32_e32 v17, v3 - t/q */
            0xf0800f10u, 0x00610410u, /* image_sample v[4:7], v[16:18], ... dim:SQ_RSRC_IMG_3D */
        };
        memcpy(words, code, sizeof(code));
        words[7] = gl_ps_s_branch(GL_PS_SAMPLE_WORDS - 8u);
    } else {
        /* tools/shader/tex-cube.s. v16..v22 are the general combine's argument registers, which
         * it fills after the sample and not before, so they are free to work in here. */
        static const uint32_t code[24] = {
            0xc8400400u, 0xc8410401u, /* v_interp_p1/p2_f32 v16, attr1.x (the direction's s) */
            0xc8440500u, 0xc8450501u, /* v_interp_p1/p2_f32 v17, attr1.y (t) */
            0xc8480b00u, 0xc8490b01u, /* v_interp_p1/p2_f32 v18, attr2.w (r) */
            0xd5440013u, 0x044a2310u, /* v_cubeid_f32 v19, v16, v17, v18 - the face */
            0xd5450014u, 0x044a2310u, /* v_cubesc_f32 v20 - s on that face */
            0xd5460015u, 0x044a2310u, /* v_cubetc_f32 v21 - t on it */
            0xd5470016u, 0x044a2310u, /* v_cubema_f32 v22 - twice the major axis */
            0xd5aa0116u, 0x00000116u, /* v_rcp_f32_e64 v22, |v22| */
            0x102c2cf0u,              /* v_mul_f32_e32 v22, 0.5, v22 - 1 / (2 * |ma|) */
            0xd54b0010u, 0x03c22d14u, /* v_fma_f32 v16, v20, v22, 0.5 - u */
            0xd54b0011u, 0x03c22d15u, /* v_fma_f32 v17, v21, v22, 0.5 - v */
            0x7e240313u,              /* v_mov_b32_e32 v18, v19 - the face, third address word */
            0xf0800f18u, 0x00610410u, /* image_sample v[4:7], v[16:18], ... dim:SQ_RSRC_IMG_CUBE */
        };
        memcpy(words, code, sizeof(code));
    }
    uint32_t *const ps_tex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);
    uint32_t *const slot = ps_tex + GL_PS_SAMPLE_SLOT_TEX;
    if (memcmp(slot, words, sizeof(words)) == 0) return;
    /* A built frame still points at these words - see gl_ps_sync_payload_edit. */
    gl_ps_sync_payload_edit(ctx, slot, words, GL_PS_SAMPLE_WORDS);
    memcpy(slot, words, sizeof(words));
    gl_ps_flush_shaders(ctx);
}

/*
 * The second texture unit's sample, in the textured pixel shader.
 *
 * The software rasteriser applies every unit `glActiveTexture` names; the hardware path samples
 * one and says so in the log, so a multitextured scene comes out on the console missing its
 * second layer. This is the sampling half of closing that: unit 1's coordinate interpolated from
 * the fourth parameter the four-parameter vertex shader exports, its own descriptors loaded from
 * the second pair of the table (image at +0x40, sampler at +0x60), and the texel left in
 * **v28..v31**. `tools/shader/tex-prolog2.s`, whose three cross-check instructions - unit 0's own
 * descriptor loads and sample - assembled to the words this file already writes for them.
 *
 * **The register range is not free choice.** `gl_ps_combine_program` gathers the general combine
 * form's arguments into v16..v27, so a texel parked there would be overwritten by unit 0's own
 * combine whenever its environment is GL_COMBINE, GL_BLEND or GL_DECAL of an RGBA texture -
 * right under GL_MODULATE, wrong under the modes that need a program. The pixel shader's
 * SPI_SHADER_PGM_RSRC1 is 0x000c0010, VGPRS 0x10, which is 136 registers in wave32, so v28..v31
 * are allocated and clear of everything.
 *
 * It sits at the end of the prolog, before exec is restored from s16, because **a sample outside
 * whole-quad mode has no helper pixels**: its implicit derivatives, and so its level of detail,
 * are wrong along every quad edge. Both samples are hoisted above both combines for that reason.
 * GL's order - unit 0 combined with the fragment, then unit 1 against that result - is kept by
 * the combine slots that follow, not by where the texels are fetched.
 *
 * **Hardware-validated 2026-09-20.** Both this slot and the second combine stage ran on a
 * console for the first time when gl1-probe's `multitexture` passed, which also confirms
 * `-9a41`'s descriptor rows: the second pair really is read from s[20:27] and s[28:31], one
 * `OOPS_GL_DESC_UNIT_STRIDE` along the table.
 */
void gl_ps_patch_unit1(gl_context_t *ctx, GLboolean on) {
    if (!ctx || !ctx->gpu_payload) return;
    uint32_t words[GL_PS_UNIT1_WORDS];
    /* Off is a branch over the slot, not twenty nops: this runs per fragment of every textured
     * draw, and one taken branch is cheaper than nineteen instructions that do nothing. */
    words[0] = gl_ps_s_branch(GL_PS_UNIT1_WORDS - 1u);
    for (size_t i = 1; i < GL_PS_UNIT1_WORDS; i++) words[i] = 0xbf800000u; /* s_nop 0 */
    if (on) {
        /* tools/shader/tex-prolog2.s */
        static const uint32_t code[17] = {
            0xc8080c00u, 0xc8090c01u, /* v_interp_p1/p2_f32 v2, attr3.x (unit 1's s) */
            0xc80c0d00u, 0xc80d0d01u, /* v_interp_p1/p2_f32 v3, attr3.y (t) */
            0xc8340f00u, 0xc8350f01u, /* v_interp_p1/p2_f32 v13, attr3.w (q) */
            0x7e1a550du,              /* v_rcp_f32 v13, v13 */
            0x10041b02u,              /* v_mul_f32 v2, v2, v13 (s/q) */
            0x10061b03u,              /* v_mul_f32 v3, v3, v13 (t/q) */
            0xf40c0500u, 0xfa000040u, /* s_load_dwordx8 s[20:27], s[0:1], 0x40 (the image) */
            0xf4080700u, 0xfa000060u, /* s_load_dwordx4 s[28:31], s[0:1], 0x60 (the sampler) */
            0xbf8cc07fu,              /* s_waitcnt lgkmcnt(0) */
            0xf0800f08u, 0x00e51c02u, /* image_sample v[28:31], v[2:3], s[20:27], s[28:31] */
            0xbf8c3f70u,              /* s_waitcnt vmcnt(0) */
        };
        memcpy(words, code, sizeof(code));
        for (size_t i = 17; i < GL_PS_UNIT1_WORDS; i++) words[i] = 0xbf800000u;
    }
    uint32_t *const ps_tex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);
    uint32_t *const slot = ps_tex + GL_PS_UNIT1_SLOT_TEX;
    if (memcmp(slot, words, sizeof(words)) == 0) return;
    /* A built frame still points at these words - see gl_ps_sync_payload_edit. */
    gl_ps_sync_payload_edit(ctx, slot, words, GL_PS_UNIT1_WORDS);
    memcpy(slot, words, sizeof(words));
    gl_ps_flush_shaders(ctx);
}

/*
 * The polygon stipple, in both pixel shaders: the 32x32 mask, and the discard that reads it.
 *
 * GL applies the stipple to filled polygons in window coordinates (3.5.2). The software
 * rasteriser tests `(polygon_stipple[(height - 1 - y) & 31] >> (31 - (x & 31))) & 1` with `y`
 * counting down from the top of the framebuffer. RDNA2 has no stipple hardware, so the pixel
 * shader discards - which needs the fragment's position, and the mask somewhere it can read.
 *
 * **The position** comes from `SPI_PS_INPUT_ENA`/`_ADDR` `0x302`, which the draw sets while a
 * stipple is on: obSCEne's `REQ-20260919T2258Z-c7d4` (sweep `20260920-082906`,
 * `166-agc/ps-pos-xy`) measured POS_X arriving in v2 and POS_Y in v3, at pixel centres, with the
 * barycentrics left in v0 and v1 - and its control arm, at `0x2`, had v2 zero. Nothing else is
 * enabled there, so v4 and v5 hold nothing the shader may read, and it does not.
 *
 * **The mask** is written here, at OOPS_GL_STIPPLE_OFFSET, in the form the shader wants rather
 * than the form GL stores: row `i` holds `polygon_stipple[(height - 1 - i) & 31]` with its bits
 * reversed, so the lookup is the row at `y & 31` shifted right by `x & 31`. The rotation and the
 * reversal are the two operations the software path does per fragment; doing them once per
 * change here keeps the slot at sixteen words, which every fragment of every draw runs through.
 *
 * Patched on every draw, as the colour sum and the export are: the stipple can be turned off, or
 * the polygon mode taken off GL_FILL, between draws of one frame.
 */
void gl_ps_patch_stipple(gl_context_t *ctx, GLboolean on) {
    if (!ctx || !ctx->gpu_payload) return;
    uint32_t words[GL_PS_STIPPLE_WORDS];
    for (size_t i = 0; i < GL_PS_STIPPLE_WORDS; i++) words[i] = 0xbf800000u; /* s_nop 0 */
    uint32_t *const table = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_STIPPLE_OFFSET);
    if (on) {
        /* The payload is mapped where the GPU reads it, as every other address in these shaders
         * is taken (gl_hw_emit_draw's payload_va). */
        const uint64_t table_va = (uint64_t)(uintptr_t)table;
        /* tools/shader/polygon-stipple.s. The two literals are the table's address; the rest is
         * that file's output word for word, and its cross-check word - the wait after the canary
         * store, 0xbf8c3f70 - is one the shaders already carry. */
        const uint32_t code[GL_PS_STIPPLE_WORDS] = {
            0x7e040f02u,              /* v_cvt_u32_f32_e32 v2, v2 (x) */
            0x7e060f03u,              /* v_cvt_u32_f32_e32 v3, v3 (y) */
            0x3606069fu,              /* v_and_b32_e32 v3, 31, v3 */
            0x34060682u,              /* v_lshlrev_b32_e32 v3, 2, v3 (the row's byte offset) */
            0xbe8203ffu, (uint32_t)table_va,          /* s_mov_b32 s2, table_lo */
            0xbe8303ffu, (uint32_t)(table_va >> 32),  /* s_mov_b32 s3, table_hi */
            0xdc308000u, 0x03020003u, /* global_load_dword v3, v3, s[2:3] */
            0x3604049fu,              /* v_and_b32_e32 v2, 31, v2 */
            0xbf8c3f70u,              /* s_waitcnt vmcnt(0) */
            0x2c060702u,              /* v_lshrrev_b32_e32 v3, v2, v3 */
            0x36060681u,              /* v_and_b32_e32 v3, 1, v3 */
            0x7d8a0680u,              /* v_cmp_ne_u32_e32 vcc_lo, 0, v3 */
            0x877e6a7eu,              /* s_and_b32 exec_lo, exec_lo, vcc_lo */
        };
        memcpy(words, code, sizeof(words));
        /* The rows, rotated for the window height and bit-reversed - see above. Compared before
         * they are written, and a change submits the draws built against the old mask first: the
         * table is one buffer that every draw of a frame reads, so glPolygonStipple between two
         * draws would otherwise reach back and restipple the first. */
        uint32_t rows[OOPS_GL_STIPPLE_WORDS];
        for (uint32_t i = 0; i < OOPS_GL_STIPPLE_WORDS; i++) {
            const uint32_t src = ctx->polygon_stipple[((uint32_t)ctx->height - 1u - i) & 31u];
            uint32_t rev = 0u;
            for (int b = 0; b < 32; b++) rev |= ((src >> (31 - b)) & 1u) << b;
            rows[i] = rev;
        }
        if (memcmp(table, rows, sizeof(rows)) != 0) {
            gl_ps_sync_payload_edit(ctx, table, rows, OOPS_GL_STIPPLE_WORDS);
            memcpy(table, rows, sizeof(rows));
#if defined(__x86_64__)
            __builtin_ia32_clflush((const void *)table);
            __builtin_ia32_clflush((const void *)(table + 16));
#endif
        }
    }
    uint32_t *const ps_tex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);
    uint32_t *const ps_untex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_UNTEX_OFFSET);
    uint32_t *const slots[2] = {ps_tex + GL_PS_STIPPLE_SLOT, ps_untex + GL_PS_STIPPLE_SLOT};
    GLboolean wrote = GL_FALSE;
    for (int s = 0; s < 2; s++) {
        if (memcmp(slots[s], words, sizeof(words)) == 0) continue;
        /* A built frame still points at these words - see gl_ps_sync_payload_edit. */
        gl_ps_sync_payload_edit(ctx, slots[s], words, GL_PS_STIPPLE_WORDS);
        memcpy(slots[s], words, sizeof(words));
        wrote = GL_TRUE;
    }
    if (wrote) gl_ps_flush_shaders(ctx);
}

/*
 * The export, in both pixel shaders: one colour target or two.
 *
 * `glDrawBuffer(GL_FRONT_AND_BACK)` (and `GL_LEFT`, which names the same pair here) asks for the
 * fragment in both buffers. Until now the console path wrote the back only and said so in the
 * log; the CPU's clears and pixel rectangles reached both, so the two buffers drifted apart
 * exactly where a draw had been. The GPU can write both in one draw - obSCEne's
 * `REQ-20260919T2258Z-3f62` measured it (sweep `20260920-082906`, `166-agc/mrt-dual-target`):
 * masks `0xff` and `SPI_SHADER_COL_FORMAT 0x99` put 512 pixels in each of two targets, where the
 * one-target control left the second at its fill. The registers come from that check and
 * `gl_hw_emit_draw`; these five words are the shader's half, assembled in
 * `tools/shader/mrt1-export.s`.
 *
 * Patched on every draw, from the same place as the colour sum and for the same reason: a draw
 * after `glDrawBuffer(GL_BACK)` must stop exporting to the second target, and a stale export
 * writes a buffer GL no longer names.
 */
void gl_ps_patch_export(gl_context_t *ctx, GLboolean both) {
    if (!ctx || !ctx->gpu_payload) return;
    /* tools/shader/mrt1-export.s. The single-target form's first two words are the ones this
     * shader has always ended with, which is that file's cross-check. */
    static const uint32_t one[GL_PS_EXPORT_WORDS] = {
        0xf800180fu, /* exp mrt0, v4, v5, v6, v7 done vm */
        0x07060504u, 0xbf810000u, /* s_endpgm */
        0xbf800000u, 0xbf800000u, /* s_nop 0, past the end */
    };
    static const uint32_t two[GL_PS_EXPORT_WORDS] = {
        0xf800100fu, /* exp mrt0, v4, v5, v6, v7 vm - no done: it is not the last */
        0x07060504u, 0xf800181fu, /* exp mrt1, v4, v5, v6, v7 done vm */
        0x07060504u, 0xbf810000u, /* s_endpgm */
    };
    const uint32_t *words = both ? two : one;
    uint32_t *const ps_tex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);
    uint32_t *const ps_untex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_UNTEX_OFFSET);
    uint32_t *const slots[2] = {ps_tex + GL_PS_EXPORT_TEX, ps_untex + GL_PS_EXPORT_UNTEX};
    GLboolean wrote = GL_FALSE;
    for (int s = 0; s < 2; s++) {
        if (memcmp(slots[s], words, GL_PS_EXPORT_WORDS * sizeof(uint32_t)) == 0) continue;
        /* A built frame still points at these words - see gl_ps_sync_payload_edit. */
        gl_ps_sync_payload_edit(ctx, slots[s], words, GL_PS_EXPORT_WORDS);
        memcpy(slots[s], words, GL_PS_EXPORT_WORDS * sizeof(uint32_t));
        wrote = GL_TRUE;
    }
    if (wrote) gl_ps_flush_shaders(ctx);
}

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

    uint32_t *ps_untex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_UNTEX_OFFSET);
    uint32_t *ps_tex = (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);

    /* A built frame still points at these four words - see gl_ps_sync_payload_edit. */
    gl_ps_sync_payload_edit(ctx, ps_untex + GL_PS_ALPHA_SLOT_UNTEX, words, 4);
    gl_ps_sync_payload_edit(ctx, ps_tex + GL_PS_ALPHA_SLOT_TEX, words, 4);

    for (size_t i = 0; i < 4; i++) {
        ps_untex[GL_PS_ALPHA_SLOT_UNTEX + i] = words[i];
        ps_tex[GL_PS_ALPHA_SLOT_TEX + i] = words[i];
    }
    gl_ps_flush_shaders(ctx);
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

const GLuint *glGetFrameReadbackSampled(GLuint line_stride) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !ctx->readback || ctx->hw_frames_confirmed == 0u) return NULL;
    if (line_stride == 0u) line_stride = 1u;
    /*
     * **Which buffer holds the current pixels** - `gl_color_read_source`, the same question
     * glReadPixels asks. This read `ctx->readback` unconditionally, and that is the CP's copy
     * **as of the last submit**: a `glDrawPixels`, `glBitmap`, `glCopyPixels` or `glAccum`
     * writes the colour buffer with the CPU and builds no command stream, so no submit follows
     * and no DMA re-copies it. The caller then got the frame as it was *before* its own pixels.
     *
     * That is what gl1-probe's six pixel-rectangle failures were, measured on 2026-09-20 and
     * unchanged by a CPU store fence on 2026-09-21 - which is what finally ruled the memory
     * ordering out and pointed here. Their passing neighbours are the tell: `read-pixels`
     * passes because glReadPixels already goes through `gl_color_read_source`, and
     * `stencil-pixels` and the depth and stencil readbacks pass because those buffers are the
     * CPU's own and never travel through `readback` at all.
     *
     * `gl_raster_sync` drops `readback_of` the moment the CPU is about to write, so the choice
     * below is already made for us: the copy while it is current, the buffer itself once it is
     * not. Both are in the same layout, so the detiling loop is indifferent to which arrived.
     */
    const uint32_t *src = gl_color_read_source(ctx, ctx->framebuffer);
    if (!src) return NULL;
    /*
     * **The invalidation belongs to the copy, not to the buffer.**
     *
     * `clflush` writes a dirty line back and then invalidates it. That is what the CP's copy
     * wants - it is written by a DMA the CPU knows nothing about, and the CPU must drop what it
     * has cached of it. Doing the same to the *live* colour buffer is the opposite of harmless:
     * any line of it the CPU still holds dirty is written back **over what the GPU just drew**.
     *
     * **The signature is intermittency, and that is how it was nearly misread.** On 2026-09-21
     * `depth-range-in-frame` - two quads in one frame, the red one underneath coming back on
     * top - failed on the first run that read from the live buffer and passed on the next, from
     * the *same* binary. Whether the write-back corrupts anything depends on what the CPU
     * happens to be holding dirty, so a run that passes proves nothing either way. The first
     * failure was written up here as a deterministic regression; it is not, and the correction
     * matters more than the original note, because a hazard that only sometimes fires is the
     * kind a green run talks you out of.
     */
    const GLboolean invalidate = (GLboolean)(src == ctx->readback);
    if (!ctx->color_tiled) {
#if defined(__x86_64__)
        if (invalidate) {
            size_t bytes = (size_t)ctx->width * (size_t)ctx->height * 4u;
            size_t step = (size_t)line_stride * 64u;
            for (size_t p = 0; p < bytes; p += step) {
                __builtin_ia32_clflush((const void *)((const char *)src + p));
            }
        }
#endif
        return src;
    }

    /* **On the scanout path the copy is tiled**, like the scanout buffer it is of, and callers
     * index a linear image. So the words they read are detiled into one: every word for a full
     * read, or the first word of every `line_stride`th cache line when sampling, which is what
     * a sampling caller reads. Those words lie all over the tiled copy, so all of it is
     * invalidated first. */
    const size_t n = (size_t)ctx->width * (size_t)ctx->height;
    if (!ctx->readback_lin) {
#ifndef OOPS_HOST_BUILD
        ctx->readback_lin = (uint32_t *)oops_mem_alloc(n * 4u, 0x1000, OOPS_MEM_WB_ONION);
#else
        ctx->readback_lin = (uint32_t *)calloc(n, sizeof(uint32_t));
#endif
        if (!ctx->readback_lin) return NULL;
    }
#if defined(__x86_64__)
    if (invalidate) {
        const size_t tiled_bytes = gl_color_words(ctx) * 4u;
        for (size_t p = 0; p < tiled_bytes; p += 64u) {
            __builtin_ia32_clflush((const void *)((const char *)src + p));
        }
    }
#endif
    const size_t step = line_stride == 1u ? 1u : (size_t)line_stride * 16u;
    for (size_t i = 0; i < n; i += step) {
        const int x = (int)(i % ctx->width);
        const int y = (int)(ctx->height - 1u - (uint32_t)(i / ctx->width));
        ctx->readback_lin[i] = src[gl_color_index(ctx, x, y)];
    }
    return ctx->readback_lin;
}

const GLuint *glGetFrameReadback(void) {
    return glGetFrameReadbackSampled(1u);
}

