/*
 * oops-gl: Internal context and pipeline structures
 */

#ifndef __GL_INTERNAL_H__
#define __GL_INTERNAL_H__

#include "GL/gl.h"
#include "GL/glu.h"
#include "oops/freestd.h"
#include "oops/display.h"
#include "oops/agc.h"

#ifndef OOPS_HOST_BUILD
#include "oops/memory.h"
#include "oops/syscall.h"
#include "agc/driver.h"
#endif

#define GL_MAX_MODELVIEW_STACK_DEPTH 16
#define GL_MAX_PROJECTION_STACK_DEPTH 4
#define GL_MAX_TEXTURE_STACK_DEPTH 2
#define GL_MAX_IMMEDIATE_VERTS 2048
#define GL_MAX_TEXTURE_OBJECTS 32
#define GL_MAX_LIGHTS 8

typedef struct {
    GLboolean enabled;
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float position[4];       /* Stored in Eye Coordinates */
    float spot_direction[3]; /* Stored in Eye Coordinates */
    float spot_exponent;
    float spot_cutoff;
    float spot_cutoff_cos;
    float const_att;
    float linear_att;
    float quad_att;
} gl_light_t;

typedef struct {
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float emission[4];
    float shininess;
} gl_material_t;

typedef struct gl_texture_object {
    GLuint id;
    GLboolean used;
    GLsizei width;
    GLsizei height;
    GLenum format;
    GLenum type;
    GLenum wrap_s;
    GLenum wrap_t;
    GLenum min_filter;
    GLenum mag_filter;
    void *pixels;          /* Host software copy */
    void *garlic_data;     /* 256-byte aligned Garlic allocation on PS5 */
    uint64_t garlic_va;    /* GPU Virtual Address */
    uint32_t pitch;      /* pixels per row in memory: equals the width, which is where the sampler takes the pitch from */
    uint32_t img_desc[8];  /* SQ_IMG_RSRC_WORD0..7 */
    uint32_t samp_desc[4]; /* SQ_IMG_SAMP_WORD0..3 */
    GLboolean desc_dirty;
} gl_texture_object_t;

typedef struct {
    float m[16]; /* Column-major: m[col*4 + row] */
} gl_mat4_t;

typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float u, v;
    float nx, ny, nz;
} gl_vertex_t;

typedef struct {
    float sx, sy, sz; /* Screen x, screen y, and depth z in [0.0, 1.0] */
    float inv_w;
    float r, g, b, a;
    float u, v;
} gl_screen_vertex_t;

typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    const void *pointer;
    GLboolean enabled;
} gl_client_array_t;

typedef struct gl_context {
    struct oops_display *disp;
    uint32_t *framebuffer;
    uint32_t width;
    uint32_t height;
    float *depth_buffer;

    /* Viewport & Scissor */
    GLint vp_x, vp_y;
    GLsizei vp_w, vp_h;
    GLint sc_x, sc_y;
    GLsizei sc_w, sc_h;

    /* Clear values */
    float clear_color[4];
    float clear_depth;

    /* Capabilities */
    GLboolean cap_depth_test;
    GLboolean cap_cull_face;
    GLboolean cap_blend;
    GLboolean cap_scissor_test;
    GLboolean cap_lighting;
    GLboolean cap_texture_2d;

    /* State settings */
    GLenum depth_func;
    GLboolean depth_mask;
    GLenum blend_src;
    GLenum blend_dst;
    GLenum blend_src_alpha;
    GLenum blend_dst_alpha;
    GLenum blend_equation;
    GLenum tex_env_mode;
    float tex_env_color[4];
    GLenum cull_mode;     /* GL_BACK, GL_FRONT, etc. */
    GLenum front_face;    /* GL_CCW, GL_CW */
    GLenum shade_model;   /* GL_SMOOTH, GL_FLAT */
    GLboolean color_mask[4];

    /* Matrix stacks */
    GLenum matrix_mode;
    gl_mat4_t modelview_stack[GL_MAX_MODELVIEW_STACK_DEPTH];
    int modelview_depth;
    gl_mat4_t projection_stack[GL_MAX_PROJECTION_STACK_DEPTH];
    int projection_depth;
    gl_mat4_t texture_stack[GL_MAX_TEXTURE_STACK_DEPTH];
    int texture_depth;

    /* Combined MVP cache */
    gl_mat4_t mvp;
    GLboolean mvp_dirty;

    /* Normal Matrix Cache (inverse-transpose of upper 3x3 of ModelView) */
    float normal_matrix[9];
    GLboolean normal_matrix_dirty;

    /* Lighting state */
    GLboolean cap_normalize;
    GLboolean cap_color_material;
    GLenum color_material_face;
    GLenum color_material_mode;
    gl_light_t lights[GL_MAX_LIGHTS];
    gl_material_t mat_front;
    gl_material_t mat_back;
    float light_model_ambient[4];
    GLboolean light_model_local_viewer;
    GLboolean light_model_two_side;

    /* Client arrays */
    gl_client_array_t array_vertex;
    gl_client_array_t array_color;
    gl_client_array_t array_normal;
    gl_client_array_t array_texcoord;

    /* Current attributes for immediate mode */
    float cur_color[4];
    float cur_normal[3];
    float cur_texcoord[2];

    /* Immediate mode buffer */
    GLenum imm_mode;
    GLboolean imm_active;
    gl_vertex_t imm_verts[GL_MAX_IMMEDIATE_VERTS];
    int imm_count;

    /* Texture Management */
    GLuint bound_texture_2d;
    gl_texture_object_t textures[GL_MAX_TEXTURE_OBJECTS];

    /* Error tracking */
    GLenum last_error;

    /* Hardware AGC backend handles (for Prospero/Trinity) */
    void *agc_queue;
    void *gpu_payload;
    void *vbo_mem;
    void *fence;
    void *canary;
    uint32_t *dcb_mem;
    uint32_t dcb_capacity_dw;
    uint32_t dcb_words;
    GLboolean hw_frame_active;
    GLboolean hw_z_bound; /* the depth surface is bound in the open frame */
    size_t depth_px;      /* floats in depth_buffer: the 64KB_Z_X tiled extent, both axes padded to 128 px */
    GLboolean use_hardware;
    uint32_t canary_vs;
    uint32_t canary_ps;
    uint32_t canary_vs_s0;
    uint32_t canary_ps_s0;
    GLboolean fb_cleared;

    /* The hardware proof: what the last submission measured, never which path was picked. */
    GLboolean hw_failed;          /* a submit or fence failure: drawing has stopped for good */
    const char *hw_failure;       /* why, for the log and the HUD */
    uint32_t hw_last_fence;       /* the end-of-pipe fence word read back after the last submission */
    uint64_t hw_last_timestamp;   /* the GPU clock counter the end-of-pipe RELEASE_MEM wrote */
    uint64_t hw_prev_timestamp;
    uint32_t hw_frames_confirmed; /* submissions whose fence and clock both arrived */
    GLboolean hw_clear_verified;  /* the GPU-only clear at context creation survived readback */
    uint32_t hw_clear_colour;
    uint32_t hw_clear_matched;
    uint32_t hw_clear_expected;
    GLboolean hw_dump_pending;    /* log the next submission's stream, shaders and fence: the oracle record */
    const uint32_t *hw_prelude;   /* glSetHardwarePrelude: words every frame's stream opens with, or NULL */
    uint32_t hw_prelude_words;
    uint32_t *readback;           /* CPU-cached copy of the render target, made by the CP at the end of every submission */

    /* Performance telemetry */
    uint64_t frame_count;
    uint64_t triangles_drawn;
} gl_context_t;

extern gl_context_t *g_gl_ctx;

static inline gl_context_t *gl_get_ctx(void) {
    return g_gl_ctx;
}

/* Freestanding math primitives */
float gl_sin(float rad);
float gl_cos(float rad);
float gl_tan(float rad);
float gl_sqrt(float val);
float gl_pow(float base, float exp);

/* Matrix operations */
void mat4_identity(gl_mat4_t *out);
void mat4_mult(gl_mat4_t *out, const gl_mat4_t *a, const gl_mat4_t *b);
void mat4_translate(gl_mat4_t *out, float x, float y, float z);
void mat4_rotate(gl_mat4_t *out, float angle_deg, float x, float y, float z);
void mat4_scale(gl_mat4_t *out, float x, float y, float z);
void mat4_frustum(gl_mat4_t *out, float l, float r, float b, float t, float n, float f);
void mat4_ortho(gl_mat4_t *out, float l, float r, float b, float t, float n, float f);
void mat4_transform_vec4(float *out4, const gl_mat4_t *m, const float *in4);
void gl_update_mvp(gl_context_t *ctx);
void gl_update_normal_matrix(gl_context_t *ctx);
void gl_compute_lighting(gl_context_t *ctx, const float *obj_pos, const float *obj_norm,
                         const float *in_color, float *out_color);

static inline uint32_t gl_depth_func_to_zfunc(GLenum func) {
    switch (func) {
        case GL_NEVER:    return OOPS_AGC_ZFUNC_NEVER;
        case GL_LESS:     return OOPS_AGC_ZFUNC_LESS;
        case GL_EQUAL:    return OOPS_AGC_ZFUNC_EQUAL;
        case GL_LEQUAL:   return OOPS_AGC_ZFUNC_LEQUAL;
        case GL_GREATER:  return OOPS_AGC_ZFUNC_GREATER;
        case GL_NOTEQUAL: return OOPS_AGC_ZFUNC_NOTEQUAL;
        case GL_GEQUAL:   return OOPS_AGC_ZFUNC_GEQUAL;
        case GL_ALWAYS:   return OOPS_AGC_ZFUNC_ALWAYS;
        default:          return OOPS_AGC_ZFUNC_LESS;
    }
}

static inline uint32_t gl_compute_db_depth_control(const gl_context_t *ctx) {
    if (!ctx) return 0u;
    int z_enable = (ctx->cap_depth_test && ctx->depth_buffer != NULL) ? 1 : 0;
    int z_write = (z_enable && ctx->depth_mask) ? 1 : 0;
    uint32_t zfunc = gl_depth_func_to_zfunc(ctx->depth_func);
    return z_enable ? OOPS_AGC_DB_DEPTH_CONTROL(1, z_write, zfunc) : 0u;
}

static inline uint32_t gl_compute_pa_su_sc_mode_cntl(const gl_context_t *ctx) {
    if (!ctx) return OOPS_AGC_CULL_NONE;
    uint32_t cull_bits = 0;
    if (ctx->cap_cull_face) {
        if (ctx->cull_mode == GL_FRONT) {
            cull_bits |= 1u; /* CULL_FRONT */
        } else if (ctx->cull_mode == GL_BACK) {
            cull_bits |= 2u; /* CULL_BACK */
        } else if (ctx->cull_mode == GL_FRONT_AND_BACK) {
            cull_bits |= 3u; /* CULL_FRONT_AND_BACK */
        }
    }
    if (ctx->front_face == GL_CW) {
        cull_bits |= OOPS_AGC_FACE_CW; /* FACE = CW */
    }
    return OOPS_AGC_CULL_NONE | cull_bits;
}

static inline uint32_t gl_compute_cb_target_mask(const gl_context_t *ctx) {
    if (!ctx) return 0x0000000fu;
    uint32_t mask = 0;
    if (ctx->color_mask[0]) mask |= 0x1u; /* Red */
    if (ctx->color_mask[1]) mask |= 0x2u; /* Green */
    if (ctx->color_mask[2]) mask |= 0x4u; /* Blue */
    if (ctx->color_mask[3]) mask |= 0x8u; /* Alpha */
    return mask;
}

/* Rendering pipeline */
void gl_rasterize_triangle(gl_context_t *ctx, const gl_screen_vertex_t *v0,
                           const gl_screen_vertex_t *v1, const gl_screen_vertex_t *v2);
void gl_hw_flush(gl_context_t *ctx);
void gl_hw_fail(gl_context_t *ctx, const char *reason);
void gl_hw_emit_dma_fill(uint32_t **dw_ptr, uint64_t dst, uint32_t value, uint32_t bytes);
void gl_hw_emit_dma_copy(uint32_t **dw_ptr, uint64_t src, uint64_t dst, uint32_t bytes);
void gl_hw_clear(gl_context_t *ctx, GLbitfield mask, uint32_t colour, float depth);
void gl_draw_primitive_triangle(gl_context_t *ctx, const gl_vertex_t *v0,
                                const gl_vertex_t *v1, const gl_vertex_t *v2);

#endif /* __GL_INTERNAL_H__ */
