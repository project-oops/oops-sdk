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
#else
#include "oops/syscall.h"
__attribute__((weak)) int sceKernelUsleep(unsigned int microseconds);
__attribute__((weak)) int sceAgcDriverSubmitCommandBuffer(void *queue, const void *dcb);

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

void gl_hw_flush(gl_context_t *ctx) {
    if (!ctx || !ctx->use_hardware || !ctx->hw_frame_active || ctx->dcb_words == 0) {
        return;
    }

    /* Reset canary words before frame execution */
    if (ctx->canary) {
        volatile uint32_t *c = (volatile uint32_t *)ctx->canary;
        c[0] = 0xaaaaaaaa;
        c[1] = 0xaaaaaaaa;
        c[2] = 0xaaaaaaaa;
        c[3] = 0xaaaaaaaa;
#if defined(__x86_64__)
        __builtin_ia32_clflush((const void *)ctx->canary);
#endif
    }

    uint32_t *dw = ctx->dcb_mem + ctx->dcb_words;
    uint64_t fence_gpu = (uint64_t)(uintptr_t)ctx->fence;

    /* Release Mem with EOP Fence */
    *dw++ = 0xc0064900u;
    *dw++ = 0x06603514u;
    *dw++ = 0x20000000u;
    *dw++ = (uint32_t)fence_gpu;
    *dw++ = (uint32_t)(fence_gpu >> 32);
    *dw++ = 0xbeefcafeu;
    *dw++ = 0u;
    *dw++ = 0u;

    for (int p = 0; p < 16; p++) {
        dw[p] = 0xffff1000u;
    }
    dw += 16;

    uint32_t total_words = (uint32_t)(dw - ctx->dcb_mem);

#if defined(__x86_64__)
    *(volatile uint32_t *)ctx->fence = 0x11111111u;
    __builtin_ia32_clflush((const void *)ctx->fence);
    for (size_t p = 0; p < (size_t)total_words * sizeof(uint32_t); p += 64) {
        __builtin_ia32_clflush((const void *)((const char *)ctx->dcb_mem + p));
    }
#endif

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
            if (*(volatile uint32_t *)ctx->fence == 0xbeefcafeu) {
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

    int fence_hit = (*(volatile uint32_t *)ctx->fence == 0xbeefcafeu) ? 1 : 0;
    uint32_t fence_val = *(volatile uint32_t *)ctx->fence;
    if (ctx->frame_count % 60 == 0 || ctx->frame_count < 5) {
        if (ctx->canary) {
            volatile uint32_t *c = (volatile uint32_t *)ctx->canary;
            gl_klog_val("vert0-x", (uint64_t)c[2]);
            gl_klog_val("vert1-x", (uint64_t)c[3]);
            gl_klog_val("vert2-x", (uint64_t)c[4]);
        }
        gl_klog_val("flush-words", (uint64_t)total_words);
        gl_klog_val("submit-rc", (uint64_t)(uint32_t)rc);
        gl_klog_val("fence-hit", (uint64_t)fence_hit);
        gl_klog_val("fence-val", (uint64_t)fence_val);
        gl_klog_val("canary-vs", (uint64_t)ctx->canary_vs);
        gl_klog_val("canary-ps", (uint64_t)ctx->canary_ps);
        gl_klog_val("canary-vs-s0", (uint64_t)ctx->canary_vs_s0);
        gl_klog_val("canary-ps-s0", (uint64_t)ctx->canary_ps_s0);
    }

    ctx->hw_frame_active = GL_FALSE;
    ctx->dcb_words = 0;
    ctx->triangles_drawn = 0;
}
#endif

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

    depth = (float *)oops_mem_alloc((size_t)w * (size_t)h * sizeof(float), 64 * 1024, OOPS_MEM_WC_GARLIC);
    if (!depth) {
        oops_mem_free(ctx);
        return NULL;
    }
#else
    ctx = &s_host_ctx;
    memset(ctx, 0, sizeof(*ctx));
    depth = s_host_depth;
#endif

    ctx->disp = disp;
    ctx->framebuffer = oops_display_get_framebuffer(disp);
    ctx->width = w;
    ctx->height = h;
    ctx->depth_buffer = depth;

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

    for (int i = 0; i < GL_MAX_LIGHTS; i++) {
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

            vs[ 0] = 0xbefc03ffu; /* s_mov_b32 m0, 0x1003 */
            vs[ 1] = 0x00001003u;
            vs[ 2] = 0xbf900009u; /* s_sendmsg sendmsg(MSG_GS_ALLOC_REQ) */
            vs[ 3] = 0xbe84037eu; /* s_mov_b32 s4, exec_lo */
            vs[ 4] = 0xbefe0387u; /* s_mov_b32 exec_lo, 7 */
            vs[ 5] = 0xd765000eu; /* v_mbcnt_lo_u32_b32 v14, -1, 0 */
            vs[ 6] = 0x000100c1u;
            vs[ 7] = 0x341e1c84u; /* v_lshlrev_b32 v15, 4, v14 (lane * 16) */
            vs[ 8] = 0x341c1c85u; /* v_lshlrev_b32 v14, 5, v14 (lane * 32) */
            vs[ 9] = 0x4a1c1d0fu; /* v_add_nc_u32 v14, v15, v14 (lane * 48) */
            vs[10] = 0x4a1c1c00u; /* v_add_nc_u32 v14, s0, v14 (vbo_offset + lane * 48) */
            vs[11] = 0xbe8203ffu; /* s_mov_b32 s2, vbo_lo */
            vs[12] = (uint32_t)vbo_gpu;
            vs[13] = 0xbe8303ffu; /* s_mov_b32 s3, vbo_hi */
            vs[14] = (uint32_t)(vbo_gpu >> 32);
            vs[15] = 0x7e020280u; /* v_mov_b32 v1, 0 */
            vs[16] = 0xd70f6a12u; /* v_add_co_u32 v18, vcc_lo, s2, v14 */
            vs[17] = 0x00021c02u;
            vs[18] = 0x50260203u; /* v_add_co_ci_u32 v19, vcc_lo, s3, v1, vcc_lo */
            vs[19] = 0xdc388000u; /* global_load_dwordx4 v[2:5], v[18:19], off offset:0 (pos) */
            vs[20] = 0x027d0012u;
            vs[21] = 0xdc388010u; /* global_load_dwordx4 v[10:13], v[18:19], off offset:16 (color) */
            vs[22] = 0x0a7d0012u;
            vs[23] = 0xdc388020u; /* global_load_dwordx4 v[6:9], v[18:19], off offset:32 (uv) */
            vs[24] = 0x067d0012u;
            vs[25] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
            vs[26] = 0xbe8603ffu; /* s_mov_b32 s6, canary_lo */
            vs[27] = (uint32_t)canary_gpu;
            vs[28] = 0xbe8703ffu; /* s_mov_b32 s7, canary_hi */
            vs[29] = (uint32_t)(canary_gpu >> 32);
            vs[30] = 0xbefe0381u; /* s_mov_b32 exec_lo, 1 */
            vs[31] = 0x7e240206u; /* v_mov_b32 v18, s6 */
            vs[32] = 0x7e260207u; /* v_mov_b32 v19, s7 */
            vs[33] = 0x7e0202ffu; /* v_mov_b32 v1, 0xbeef0001 */
            vs[34] = 0xbeef0001u;
            vs[35] = 0xdc708000u; /* global_store_dword v[18:19], v1, off offset:0 */
            vs[36] = 0x007d0112u;
            vs[37] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
            vs[38] = 0x7e0202ffu; /* v_mov_b32 v1, 0x20280600 */
            vs[39] = 0x20280600u;
            vs[40] = 0xf8000941u; /* exp prim, v1, off, off, off done */
            vs[41] = 0x00000001u;
            vs[42] = 0xbefe0387u; /* s_mov_b32 exec_lo, 7 */
            vs[43] = 0xf800020fu; /* exp param0, v10, v11, v12, v13 (Color) */
            vs[44] = 0x0d0c0b0au;
            vs[45] = 0xf800021fu; /* exp param1, v6, v7, v8, v9 (UV) */
            vs[46] = 0x09080706u;
            vs[47] = 0xf80008cfu; /* exp pos0, v2, v3, v4, v5 done (Pos) */
            vs[48] = 0x05040302u;
            vs[49] = 0xbefe0304u; /* s_mov_b32 exec_lo, s4 */
            vs[50] = 0xbf810000u; /* s_endpgm */
            for (size_t p = 51; p < 64; p++) vs[p] = 0xbf800000u;

            /* Mirror to GS stage at offset 0x100 */
            uint32_t *gs = (uint32_t *)((char *)ctx->gpu_payload + 0x100);
            for (size_t p = 0; p < 64; p++) gs[p] = vs[p];

            /* 2. Hardware Textured + Gouraud Pixel Shader (offset 0x200) */
            uint32_t *ps_tex = (uint32_t *)((char *)ctx->gpu_payload + 0x200);
            ps_tex[ 0] = 0xbf8c0000u; /* s_waitcnt 0 */
            ps_tex[ 1] = 0xbe84037eu; /* s_mov_b32 s4, exec_lo */
            ps_tex[ 2] = 0xbefe0381u; /* s_mov_b32 exec_lo, 1 */
            ps_tex[ 3] = 0xbe8203ffu; /* s_mov_b32 s2, canary_lo */
            ps_tex[ 4] = (uint32_t)canary_gpu;
            ps_tex[ 5] = 0xbe8303ffu; /* s_mov_b32 s3, canary_hi */
            ps_tex[ 6] = (uint32_t)(canary_gpu >> 32);
            ps_tex[ 7] = 0x7e100202u; /* v_mov_b32 v8, s2 */
            ps_tex[ 8] = 0x7e120203u; /* v_mov_b32 v9, s3 */
            ps_tex[ 9] = 0x7e1402ffu; /* v_mov_b32 v10, 0xbeef0002 */
            ps_tex[10] = 0xbeef0002u;
            ps_tex[11] = 0xdc708004u; /* global_store_dword v[8:9], v10, off offset:4 */
            ps_tex[12] = 0x007d0a08u;
            ps_tex[13] = 0xbefe0304u; /* s_mov_b32 exec_lo, s4 */
            ps_tex[14] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
            ps_tex[15] = 0xc8080400u; /* v_interp_p1_f32 v2, v0, attr1.x (U) */
            ps_tex[16] = 0xc8090401u; /* v_interp_p2_f32 v2, v1, attr1.x */
            ps_tex[17] = 0xc80c0500u; /* v_interp_p1_f32 v3, v0, attr1.y (V) */
            ps_tex[18] = 0xc80d0501u; /* v_interp_p2_f32 v3, v1, attr1.y */
            ps_tex[19] = 0xc8200000u; /* v_interp_p1_f32 v8, v0, attr0.x (R) */
            ps_tex[20] = 0xc8210001u; /* v_interp_p2_f32 v8, v1, attr0.x */
            ps_tex[21] = 0xc8240100u; /* v_interp_p1_f32 v9, v0, attr0.y (G) */
            ps_tex[22] = 0xc8250101u; /* v_interp_p2_f32 v9, v1, attr0.y */
            ps_tex[23] = 0xc8280200u; /* v_interp_p1_f32 v10, v0, attr0.z (B) */
            ps_tex[24] = 0xc8290201u; /* v_interp_p2_f32 v10, v1, attr0.z */
            ps_tex[25] = 0xc82c0300u; /* v_interp_p1_f32 v11, v0, attr0.w (A) */
            ps_tex[26] = 0xc82d0301u; /* v_interp_p2_f32 v11, v1, attr0.w */
            ps_tex[27] = 0xf40c0100u; /* s_load_dwordx8 s[4:11], s[0:1], 0x00 */
            ps_tex[28] = 0xfa000000u;
            ps_tex[29] = 0xf4080300u; /* s_load_dwordx4 s[12:15], s[0:1], 0x20 */
            ps_tex[30] = 0xfa000020u;
            ps_tex[31] = 0xbf8cc07fu; /* s_waitcnt lgkmcnt(0) */
            ps_tex[32] = 0xf09c0f08u; /* image_sample_lz v[4:7], v[2:3], s[4:11], s[12:15] dmask:0xf dim:SQ_RSRC_IMG_2D */
            ps_tex[33] = 0x00610402u;
            ps_tex[34] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
            ps_tex[35] = 0x10081104u; /* v_mul_f32 v4, v4, v8 (R * R) */
            ps_tex[36] = 0x100a1305u; /* v_mul_f32 v5, v5, v9 (G * G) */
            ps_tex[37] = 0x100c1506u; /* v_mul_f32 v6, v6, v10 (B * B) */
            ps_tex[38] = 0x100e1707u; /* v_mul_f32 v7, v7, v11 (A * A) */
            ps_tex[39] = 0xf800180fu; /* exp mrt0, v4, v5, v6, v7 done vm */
            ps_tex[40] = 0x07060504u;
            ps_tex[41] = 0xbf810000u; /* s_endpgm */
            for (size_t p = 42; p < 64; p++) ps_tex[p] = 0xbf800000u;

            /* 3. Hardware Gouraud Barycentric Interpolating Pixel Shader (untextured, offset 0x300) */
            uint32_t *ps_untex = (uint32_t *)((char *)ctx->gpu_payload + 0x300);
            ps_untex[ 0] = 0xbf8c0000u; /* s_waitcnt 0 */
            ps_untex[ 1] = 0xbe84037eu; /* s_mov_b32 s4, exec_lo */
            ps_untex[ 2] = 0xbefe0381u; /* s_mov_b32 exec_lo, 1 */
            ps_untex[ 3] = 0xbe8003ffu; /* s_mov_b32 s0, canary_lo */
            ps_untex[ 4] = (uint32_t)canary_gpu;
            ps_untex[ 5] = 0xbe8103ffu; /* s_mov_b32 s1, canary_hi */
            ps_untex[ 6] = (uint32_t)(canary_gpu >> 32);
            ps_untex[ 7] = 0x7e100200u; /* v_mov_b32 v8, s0 */
            ps_untex[ 8] = 0x7e120201u; /* v_mov_b32 v9, s1 */
            ps_untex[ 9] = 0x7e1402ffu; /* v_mov_b32 v10, 0xbeef0002 */
            ps_untex[10] = 0xbeef0002u;
            ps_untex[11] = 0xdc708004u; /* global_store_dword v[8:9], v10, off offset:4 */
            ps_untex[12] = 0x007d0a08u;
            ps_untex[13] = 0xbefe0304u; /* s_mov_b32 exec_lo, s4 */
            ps_untex[14] = 0xbf8c3f70u; /* s_waitcnt vmcnt(0) */
            ps_untex[15] = 0xc8100000u; /* v_interp_p1_f32 v4, v0, attr0.x (R) */
            ps_untex[16] = 0xc8110001u; /* v_interp_p2_f32 v4, v1, attr0.x */
            ps_untex[17] = 0xc8140100u; /* v_interp_p1_f32 v5, v0, attr0.y (G) */
            ps_untex[18] = 0xc8150101u; /* v_interp_p2_f32 v5, v1, attr0.y */
            ps_untex[19] = 0xc8180200u; /* v_interp_p1_f32 v6, v0, attr0.z (B) */
            ps_untex[20] = 0xc8190201u; /* v_interp_p2_f32 v6, v1, attr0.z */
            ps_untex[21] = 0xc81c0300u; /* v_interp_p1_f32 v7, v0, attr0.w (A) */
            ps_untex[22] = 0xc81d0301u; /* v_interp_p2_f32 v7, v1, attr0.w */
            ps_untex[23] = 0xf800180fu; /* exp mrt0, v4, v5, v6, v7 done vm */
            ps_untex[24] = 0x07060504u;
            ps_untex[25] = 0xbf810000u; /* s_endpgm */
            for (size_t p = 26; p < 64; p++) ps_untex[p] = 0xbf800000u;

            /* 4. Fallback shader at 0x800 */
            uint32_t *fb = (uint32_t *)((char *)ctx->gpu_payload + 0x800);
            fb[0] = 0xbefc0380u; /* s_mov_b32 m0, 0 */
            fb[1] = 0xbf900009u; /* s_sendmsg sendmsg(MSG_GS_ALLOC_REQ) */
            fb[2] = 0xbf810000u; /* s_endpgm */
            for (size_t p = 3; p < 64; p++) fb[p] = 0xbf800000u;

            /* 5. Initialize active texture descriptor table at 0x900 */
            uint32_t *desc_table = (uint32_t *)((char *)ctx->gpu_payload + 0x900);
            memset(desc_table, 0, 64);
            desc_table[1] = 56u << 20; /* FORMAT = 56 (FMT_8_8_8_8_UNORM) */
            desc_table[2] = (1u << 31); /* RESOURCE_LEVEL = 1 */
            desc_table[3] = 0x90000688u; /* SQ_RSRC_IMG_2D */
            desc_table[9] = 0x00fff000u; /* Sampler MAX_LOD */

#if defined(__x86_64__)
            for (size_t p = 0; p < 0x1000; p += 64) {
                __builtin_ia32_clflush((const void *)((const char *)ctx->gpu_payload + p));
            }
#endif
            ctx->use_hardware = GL_TRUE;
            gl_klog_line("hardware AGC RDNA2 pipeline initialized successfully");
        }
    }
#endif

    g_gl_ctx = ctx;
    return (void *)ctx;
}

void glContextDestroy(void *ctx_handle) {
    gl_context_t *ctx = (gl_context_t *)ctx_handle;
    if (!ctx) return;

#ifndef OOPS_HOST_BUILD
    if (ctx->agc_queue && sceAgcDriverDestroyQueue) {
        sceAgcDriverDestroyQueue(ctx->agc_queue);
        ctx->agc_queue = NULL;
    }
    for (int i = 0; i < GL_MAX_TEXTURE_OBJECTS; i++) {
        if (ctx->textures[i].garlic_data) {
            oops_mem_free(ctx->textures[i].garlic_data);
            ctx->textures[i].garlic_data = NULL;
        }
    }
    if (ctx->gpu_payload) oops_mem_free(ctx->gpu_payload);
    if (ctx->fence) oops_mem_free(ctx->fence);
    if (ctx->canary) oops_mem_free(ctx->canary);
    if (ctx->dcb_mem) oops_mem_free(ctx->dcb_mem);
    if (ctx->depth_buffer) oops_mem_free(ctx->depth_buffer);
    oops_mem_free(ctx);
#else
    for (int i = 0; i < GL_MAX_TEXTURE_OBJECTS; i++) {
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

