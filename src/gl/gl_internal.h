/*
 * oops-gl: Internal context and pipeline structures
 */

#ifndef __GL_INTERNAL_H__
#define __GL_INTERNAL_H__

#include "GL/gl.h"
#include "GL/glu.h"
#include "oops/freestd.h"
#include "oops/display.h"

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
    GLboolean use_hardware;
    uint32_t canary_vs;
    uint32_t canary_ps;
    uint32_t canary_vs_s0;
    uint32_t canary_ps_s0;
    GLboolean fb_cleared;

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

/* Rendering pipeline */
void gl_rasterize_triangle(gl_context_t *ctx, const gl_screen_vertex_t *v0,
                           const gl_screen_vertex_t *v1, const gl_screen_vertex_t *v2);
#ifndef OOPS_HOST_BUILD
void gl_hw_flush(gl_context_t *ctx);
#endif
void gl_draw_primitive_triangle(gl_context_t *ctx, const gl_vertex_t *v0,
                                const gl_vertex_t *v1, const gl_vertex_t *v2);

#endif /* __GL_INTERNAL_H__ */
