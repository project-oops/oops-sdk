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
#include "oops/heap.h"
#include "oops/syscall.h"
#include "agc/driver.h"
#endif

/* **The capacities are `OOPS_GL_`, not `GL_`.** They used to be spelled `GL_MAX_LIGHTS`,
 * `GL_MAX_ATTRIB_STACK_DEPTH` and so on - which are the names of the *GL enumerants a program
 * passes to glGetIntegerv to ask for these very numbers*. Holding those names internally made
 * the enums impossible to define, so `glGetIntegerv(GL_MAX_LIGHTS, ...)` could not be written
 * at all. The prefix is what keeps the two apart: anything here is a size this port chose,
 * anything in <GL/gl.h> is a number the specification assigned. */
#define OOPS_GL_MODELVIEW_STACK_CAPACITY 16
#define OOPS_GL_PROJECTION_STACK_CAPACITY 4
#define OOPS_GL_TEXTURE_STACK_CAPACITY 2
#define OOPS_GL_MAX_IMMEDIATE_VERTS 2048
/* Display lists, sized like everything else here: fixed, so nothing allocates on a guest's
 * behalf in a freestanding binary. A program that wants more is refused loudly rather than
 * silently dropping geometry - see `gl_list_record`. */
#define GL_MAX_LISTS 256
#define GL_MAX_LIST_COMMANDS 4096
/* How deep one list may call another before it is refused as recursion. GL leaves the limit
 * implementation-defined and requires at least 64. */
#define GL_MAX_LIST_DEPTH 64
/* The specification's minimum, which is what this provides. */
#define OOPS_GL_ATTRIB_STACK_CAPACITY 16
#define OOPS_GL_CLIENT_ATTRIB_STACK_CAPACITY 16
#define OOPS_GL_MAX_TEXTURE_OBJECTS 32
#define OOPS_GL_MAX_BUFFER_OBJECTS 64
#define OOPS_GL_LIGHT_COUNT 8

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

/* One recorded call inside a display list.
 *
 * A tagged union flattened into fixed fields rather than a real union, because every operation
 * this records fits in an enum, four floats and two integers, and a flat record is one memcpy
 * to append and needs no per-op sizing. `GL_MAX_LIST_COMMANDS` of these is the cost.
 *
 * **What is recorded is the call, not its effect.** A list replays the same calls in the same
 * order, so a list compiled before a texture is bound and executed after it draws with the
 * later texture - which is what GL says happens, and would not if the effect were baked. */
typedef enum {
    GL_LIST_OP_BEGIN,
    GL_LIST_OP_END,
    GL_LIST_OP_VERTEX,
    GL_LIST_OP_COLOR,
    GL_LIST_OP_NORMAL,
    GL_LIST_OP_TEXCOORD,
    GL_LIST_OP_ENABLE,
    GL_LIST_OP_DISABLE,
    GL_LIST_OP_MATRIX_MODE,
    GL_LIST_OP_LOAD_IDENTITY,
    GL_LIST_OP_PUSH_MATRIX,
    GL_LIST_OP_POP_MATRIX,
    GL_LIST_OP_TRANSLATE,
    GL_LIST_OP_ROTATE,
    GL_LIST_OP_SCALE,
    GL_LIST_OP_LOAD_MATRIX,
    GL_LIST_OP_MULT_MATRIX,
    GL_LIST_OP_BIND_TEXTURE,
    GL_LIST_OP_SHADE_MODEL,
    GL_LIST_OP_CULL_FACE,
    GL_LIST_OP_FRONT_FACE,
    GL_LIST_OP_DEPTH_FUNC,
    GL_LIST_OP_BLEND_FUNC,
    GL_LIST_OP_CALL_LIST,
} gl_list_op_t;

typedef struct {
    gl_list_op_t op;
    GLenum e0;
    GLenum e1;
    GLuint u0;
    GLfloat f[4];
} gl_list_cmd_t;

typedef struct {
    GLboolean used;       /* named by glGenLists or glNewList */
    GLboolean compiled;   /* glEndList has closed it, so it may be called */
    GLuint count;
    gl_list_cmd_t cmds[GL_MAX_LIST_COMMANDS];
} gl_display_list_t;

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

/* One entry of the attribute stack: the GL state `glPushAttrib` can save.
 *
 * A struct of its own rather than a copy of the context, because the context also holds the
 * display lists - 256 of them, 4096 commands each - and snapshotting that would be tens of
 * megabytes per push. What is here is the state a program can change and expect back.
 */
typedef struct {
    GLbitfield mask; /* what this push asked to save, and so what the pop restores */

    /* GL_CURRENT_BIT */
    float cur_color[4];
    float cur_normal[3];
    float cur_texcoord[2];

    /* GL_ENABLE_BIT, and the individual buffer bits that also carry an enable */
    GLboolean cap_depth_test, cap_cull_face, cap_blend, cap_scissor_test;
    GLboolean cap_lighting, cap_texture_2d, cap_normalize, cap_color_material;
    GLboolean cap_alpha_test, cap_polygon_offset_fill;

    /* GL_DEPTH_BUFFER_BIT */
    GLenum depth_func;
    GLboolean depth_mask;
    float clear_depth;

    /* GL_COLOR_BUFFER_BIT */
    GLenum blend_src, blend_dst, blend_src_alpha, blend_dst_alpha, blend_equation;
    GLboolean color_mask[4];
    float clear_color[4];
    GLenum alpha_func;
    float alpha_ref;

    /* GL_POLYGON_BIT */
    GLenum cull_mode, front_face;
    float polygon_offset_factor, polygon_offset_units;

    /* GL_LIGHTING_BIT */
    GLenum shade_model;
    gl_light_t lights[OOPS_GL_LIGHT_COUNT];
    gl_material_t mat_front, mat_back;
    float light_model_ambient[4];

    /* GL_TEXTURE_BIT */
    GLuint bound_texture_2d;
    GLenum tex_env_mode;
    float tex_env_color[4];

    /* GL_VIEWPORT_BIT */
    GLint vp_x, vp_y;
    GLsizei vp_w, vp_h;
    float depth_near, depth_far;

    /* GL_SCISSOR_BIT */
    GLint sc_x, sc_y;
    GLsizei sc_w, sc_h;

    /* GL_TRANSFORM_BIT */
    GLenum matrix_mode;

    /* GL_LIST_BIT */
    GLuint list_base;
} gl_attrib_entry_t;


typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    /* Client memory, or - when `buffer` is non-zero - a **byte offset** into that buffer
     * object. The same field carries both, which is what the GL 1.5 API does with the same
     * parameter. */
    const void *pointer;
    GLboolean enabled;
    /* The buffer bound to GL_ARRAY_BUFFER when this array was specified, or 0 for client
     * memory. The *name*, not an address: glBufferData may reallocate the storage, and a
     * program respecifying a buffer every frame is ordinary rather than exotic. */
    GLuint buffer;
} gl_client_array_t;

/* One buffer object. The storage is ordinary process memory: everything that reads it here is
 * the CPU-side array reader, and the hardware path copies the vertices it builds into its own
 * GPU allocation afterwards regardless of where they came from. */
typedef struct {
    GLuint id;
    GLboolean used;
    void *data;
    GLsizeiptr size;
    GLenum usage;
} gl_buffer_object_t;

/* One frame of the *client* attribute stack. Separate from gl_attrib_entry_t because the
 * specification keeps the two stacks separate: a push of client state must not pop server
 * state, and a program that brackets a helper with both is relying on that. */
typedef struct {
    GLbitfield mask;

    /* GL_CLIENT_VERTEX_ARRAY_BIT */
    gl_client_array_t array_vertex;
    gl_client_array_t array_color;
    gl_client_array_t array_normal;
    gl_client_array_t array_texcoord;

    /* GL_CLIENT_PIXEL_STORE_BIT */
    GLint unpack_alignment;
    GLint unpack_row_length;
    GLint pack_alignment;
} gl_client_attrib_entry_t;

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
    /* glHint(GL_PERSPECTIVE_CORRECTION_HINT). Recorded and reported; it changes nothing,
     * because the rasteriser interpolates perspective-correctly either way. */
    GLenum perspective_hint;
    GLenum cull_mode;     /* GL_BACK, GL_FRONT, etc. */
    GLenum front_face;    /* GL_CCW, GL_CW */
    GLenum shade_model;   /* GL_SMOOTH, GL_FLAT */
    GLboolean color_mask[4];

    /* Matrix stacks */
    GLenum matrix_mode;
    gl_mat4_t modelview_stack[OOPS_GL_MODELVIEW_STACK_CAPACITY];
    int modelview_depth;
    gl_mat4_t projection_stack[OOPS_GL_PROJECTION_STACK_CAPACITY];
    int projection_depth;
    gl_mat4_t texture_stack[OOPS_GL_TEXTURE_STACK_CAPACITY];
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
    gl_light_t lights[OOPS_GL_LIGHT_COUNT];
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
    gl_vertex_t imm_verts[OOPS_GL_MAX_IMMEDIATE_VERTS];
    int imm_count;

    /* Texture Management */
    GLuint bound_texture_2d;
    gl_texture_object_t textures[OOPS_GL_MAX_TEXTURE_OBJECTS];

    /* Buffer objects. Two binding points, because GL_ARRAY_BUFFER and GL_ELEMENT_ARRAY_BUFFER
     * are independent - a program binds one of each and draws from both at once. */
    gl_buffer_object_t buffers[OOPS_GL_MAX_BUFFER_OBJECTS];
    GLuint bound_array_buffer;
    GLuint bound_element_array_buffer;

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

    /* glPushAttrib. The specification requires at least 16 deep. */
    gl_attrib_entry_t attrib_stack[OOPS_GL_ATTRIB_STACK_CAPACITY];
    GLuint attrib_depth;

    /* glPushClientAttrib. Its own stack and its own depth; the specification's minimum is 16. */
    gl_client_attrib_entry_t client_attrib_stack[OOPS_GL_CLIENT_ATTRIB_STACK_CAPACITY];
    GLuint client_attrib_depth;

    /* glAlphaFunc. A fragment whose alpha fails the comparison is discarded. */
    GLboolean cap_alpha_test;
    GLenum alpha_func;
    float alpha_ref;

    /* glPolygonOffset. `enabled` is GL_POLYGON_OFFSET_FILL; the wireframe and point variants
     * need glPolygonMode, which needs primitives this cannot draw. */
    GLboolean cap_polygon_offset_fill;
    float polygon_offset_factor;
    float polygon_offset_units;

    /* glDepthRange: where NDC z lands in the depth buffer. Defaults to the specification's
     * 0..1, which is the pair the viewport registers were already carrying as constants. */
    float depth_near;
    float depth_far;

    /* glPixelStorei: how a client's pixel rectangle is laid out in its own memory. Only the
     * unpack side exists, because nothing here reads pixels back yet. */
    GLint unpack_alignment;  /* row start alignment in bytes: 1, 2, 4 or 8 */
    GLint unpack_row_length; /* pixels per source row, or 0 meaning "the width being uploaded" */
    GLint pack_alignment;    /* the same, for rows glReadPixels writes back */

    /* Display lists: what was recorded, and what is being recorded now. */
    gl_display_list_t lists[GL_MAX_LISTS];
    GLuint list_compiling;   /* the name being recorded into, or 0 for none */
    GLenum list_mode;        /* GL_COMPILE or GL_COMPILE_AND_EXECUTE */
    GLuint list_base;        /* glListBase, added to every name glCallLists reads */
    GLuint list_depth;       /* nested glCallList, to stop a list that calls itself */

    /* Performance telemetry */
    uint64_t frame_count;
    uint64_t triangles_drawn;
} gl_context_t;

extern gl_context_t *g_gl_ctx;

static inline gl_context_t *gl_get_ctx(void) {
    return g_gl_ctx;
}

/* Records an error the way GL says to: **the first one wins.**
 *
 * The specification is explicit that once the flag is set, no further errors are recorded
 * until glGetError() reads and clears it. Every site here used to assign `last_error`
 * directly, which is the opposite rule - the *last* error won - so a caller doing several
 * calls before one glGetError() was shown the most recent failure and never the one that
 * started it. That sends somebody to the wrong call, which is the only thing an error code is
 * for.
 *
 * glGetError() itself still assigns, because clearing the flag is its job. */
static inline void gl_record_error(gl_context_t *ctx, GLenum error) {
    if (ctx && ctx->last_error == GL_NO_ERROR) {
        ctx->last_error = error;
    }
}

/* A float's bits, for the registers and shader literals that take one. */
static inline uint32_t gl_f32_bits(float f) {
    union { float f; uint32_t u; } v;
    v.f = f;
    return v.u;
}

/* Where the alpha test sits in each pixel shader, as a word index. Four words are reserved,
 * which is what the longest form needs: a literal load (two words), a compare, and the mask
 * update. `gl_ps_patch_alpha_test` writes them; anything else leaves them as `s_nop`.
 *
 * Patched in place rather than rebuilt, because the shaders are laid into the GPU payload once
 * at context creation and a rebuild would have to redo the descriptors and the cache flush with
 * them. The instruction encodings were produced by assembling for gfx1030 rather than written
 * from memory - see the table in `gl_ps_patch_alpha_test`. */
#define GL_PS_ALPHA_SLOT_UNTEX 24u
#define GL_PS_ALPHA_SLOT_TEX   40u
void gl_ps_patch_alpha_test(gl_context_t *ctx);

/* Buffer objects. `gl_find_buffer` answers NULL for name 0 and for a name never generated.
 *
 * `gl_array_base` is the one place an array's effective address is worked out: client memory
 * straight through, or the buffer's storage plus the offset the `pointer` field is carrying.
 * It answers NULL when the named buffer has gone or has no storage, which stops the reader
 * rather than letting it walk an address computed from a freed pointer. */
gl_buffer_object_t *gl_find_buffer(gl_context_t *ctx, GLuint name);
/* Releases every buffer object's storage. Called from glContextDestroy, and living beside the
 * allocation in gl_state.c so the choice of allocator stays in one file. */
void gl_free_all_buffers(gl_context_t *ctx);
const uint8_t *gl_array_base(const gl_context_t *ctx, const gl_client_array_t *a);

/* Display lists. `gl_list_capture` appends the call being made to the list being compiled and
 * returns whether the caller should stop there; `gl_list_refuse` does the same for a call that
 * cannot be compiled. Both answer false when no list is being compiled, which is the ordinary
 * path and costs one load. */
GLboolean gl_list_capture(gl_list_op_t op, GLenum e0, GLenum e1, GLuint u0, const GLfloat *f);
GLboolean gl_list_refuse(void);

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
    if (ctx->cap_polygon_offset_fill) {
        /* Front, back and para together: GL has one enable for filled polygons and does not
         * distinguish the faces, so enabling one and not the other would offset half a mesh.
         *
         * Bit positions from Mesa's generated register header,
         * `src/amd/common/amdgfxregs.h`: S_028814_POLY_OFFSET_FRONT_ENABLE is bit 11,
         * BACK_ENABLE bit 12, PARA_ENABLE bit 13. */
        cull_bits |= (1u << 11) | (1u << 12) | (1u << 13);
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
