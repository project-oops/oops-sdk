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
/* Six, which is the specification's minimum and exactly what the hardware clipper has:
 * PA_CL_CLIP_CNTL carries UCP_ENA_0..5 and there is no seventh bit. */
#define OOPS_GL_CLIP_PLANE_COUNT 6

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

/* The default texture of each target - the object a glTex* call acts on when nothing is bound.
 *
 * Reserved ids rather than object 1, which is what this used for GL_TEXTURE_2D. glGenTextures
 * counts up from 1, so object 1 was both the default 2D texture *and* the first id handed out: a
 * program that generated a texture and then uploaded with nothing bound wrote into its own. These
 * are above anything that counting will reach, and there is one per target because GL has one per
 * target - sharing them would let a 1D upload overwrite the default 2D image.
 */
#define OOPS_GL_DEFAULT_TEXTURE_1D 0xfffffe01u
#define OOPS_GL_DEFAULT_TEXTURE_2D 0xfffffe02u

typedef struct gl_texture_object {
    GLuint id;
    GLboolean used;
    /* The target this object was first bound to, and the only one it may be bound to afterwards.
     * Zero until it is first bound. GL calls rebinding to a different target an error, and it is
     * one worth reporting: the object's image has a shape the other target cannot read. */
    GLenum target;
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
    /* Signed distance from each user clip plane, in eye space. Interpolated across the triangle
     * and tested per fragment rather than the triangle being geometrically clipped: the visible
     * result is the same and it does not need a clipper that turns one triangle into several. */
    float cd[OOPS_GL_CLIP_PLANE_COUNT];
    /* The fog factor: 1 means fully the fragment's own colour, 0 fully the fog colour. Computed
     * per vertex from the eye-space distance and interpolated, which is where the per-fragment
     * quality comes from - folding fog into the vertex colour instead is exact only when the
     * factor is constant across the primitive. */
    float fog;
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
    /* Four, not two: glTexCoord3 and glTexCoord4 exist and q is a projective divide, so a
     * two-component current texture coordinate cannot hold what they set. Defaults (0,0,0,1). */
    float cur_texcoord[4];

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
    GLuint bound_texture_1d;
    GLboolean cap_texture_1d;
    GLenum tex_env_mode;
    GLenum texgen_mode[4];
    float texgen_object_plane[4][4];
    float texgen_eye_plane[4][4];
    GLboolean texgen_enabled[4];
    /* GL_TRANSFORM_BIT */
    float clip_plane[OOPS_GL_CLIP_PLANE_COUNT][4];
    GLboolean clip_plane_enabled[OOPS_GL_CLIP_PLANE_COUNT];
    /* GL_FOG_BIT */
    GLboolean cap_fog;
    GLenum fog_mode;
    float fog_density;
    float fog_start;
    float fog_end;
    float fog_color[4];
    /* GL_STENCIL_BUFFER_BIT */
    GLboolean cap_stencil_test;
    GLenum stencil_func;
    GLint stencil_ref;
    GLuint stencil_value_mask;
    GLuint stencil_writemask;
    GLenum stencil_fail;
    GLenum stencil_zfail;
    GLenum stencil_zpass;
    GLint clear_stencil;
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
    GLboolean cap_texture_1d;

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

    /* Client arrays */
    gl_client_array_t array_vertex;
    gl_client_array_t array_color;
    gl_client_array_t array_normal;
    gl_client_array_t array_texcoord;

    /* Current attributes for immediate mode */
    float cur_color[4];
    float cur_normal[3];
    float cur_texcoord[4]; /* s, t, r, q - see the attribute-stack copy of this field */

    /* Immediate mode buffer */
    GLenum imm_mode;
    GLboolean imm_active;
    gl_vertex_t imm_verts[OOPS_GL_MAX_IMMEDIATE_VERTS];
    int imm_count;

    /* Texture Management. One binding per target, both live at once - that is what makes
     * GL_TEXTURE_1D a target rather than a shape of 2D texture. */
    GLuint bound_texture_2d;
    GLuint bound_texture_1d;
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
    /* `glViewport` was called after this frame's registers were written, so the next draw has to
     * re-emit them. Frames that set the viewport before drawing - which is every frame gl1-cube
     * renders - never raise it, so their command stream is unchanged. */
    /* Texture coordinate generation, one entry per coordinate in the order S, T, R, Q.
     *
     * `eye_plane` is stored **already multiplied by the inverse modelview of the moment
     * glTexGen was called**, which is what the specification says and is the only reason
     * GL_EYE_LINEAR differs from GL_OBJECT_LINEAR. Keeping the caller's numbers instead and
     * transforming later would silently turn every eye-linear plane into an object-linear one.
     * glGetTexGen returns what is stored, as the specification also says. */
    GLenum texgen_mode[4];
    float texgen_object_plane[4][4];
    float texgen_eye_plane[4][4];
    GLboolean texgen_enabled[4];

    /* Fog. The distance it works from is the eye-space distance to the fragment, so the factor is
     * computed per vertex and interpolated - see the fog field on the screen vertex. */
    GLboolean cap_fog;
    GLenum fog_mode;
    float fog_density;
    float fog_start;
    float fog_end;
    float fog_color[4];

    /* Point size and line width, in pixels. Both are screen-space quantities, which is why the
     * expansion that honours them has to happen after projection. */
    float point_size;
    float line_width;

    /* Raster position, in window coordinates, plus the colour and texture coordinate latched
     * with it. `raster_valid` is false when the position clipped, and an invalid position draws
     * nothing - clamping it to the edge instead would put a bitmap somewhere the program never
     * asked for, which is worse than drawing nothing. */
    float raster_pos[4];
    float raster_color[4];
    float raster_texcoord[4];
    float raster_distance;
    GLboolean raster_valid;
    float pixel_zoom_x;
    float pixel_zoom_y;

    /* Stencil. Eight bits per pixel in its own buffer, because on this hardware the stencil
     * surface is separate from the depth one - DB_STENCIL_INFO and DB_STENCIL_READ/WRITE_BASE
     * are their own registers, so Z_32_FLOAT having no stencil plane costs nothing. */
    uint8_t *stencil_buffer;
    size_t stencil_px;
    GLboolean cap_stencil_test;
    GLenum stencil_func;
    GLint stencil_ref;
    GLuint stencil_value_mask;
    GLuint stencil_writemask;
    GLenum stencil_fail;
    GLenum stencil_zfail;
    GLenum stencil_zpass;
    GLint clear_stencil;

    /* User clip planes, in **eye** coordinates - stored through the inverse modelview of the
     * moment glClipPlane was called, the same rule the eye-linear texgen plane follows. */
    float clip_plane[OOPS_GL_CLIP_PLANE_COUNT][4];
    GLboolean clip_plane_enabled[OOPS_GL_CLIP_PLANE_COUNT];
    GLboolean hw_clip_dirty;

    GLboolean hw_vport_dirty;
    GLboolean hw_scissor_dirty;
    /* Which texture's descriptors are in this frame's one descriptor slot, 0 for none. */
    GLuint hw_frame_tex;
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
/* The four instructions combining the sampled texel with the interpolated colour. */
#define GL_PS_COMBINE_SLOT_TEX 36u
void gl_ps_patch_alpha_test(gl_context_t *ctx);
void gl_ps_patch_tex_env(gl_context_t *ctx);

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
float gl_exp(float x);

/* The fog factor for an eye-space distance: 1 is unfogged, 0 is fully the fog colour.
 *
 * The three modes are the specification's. Linear divides by (end - start), so an end equal to
 * the start would divide by zero - that case gives 0, which is the fully-fogged answer the limit
 * approaches from either side, rather than an infinity that poisons the colour. */
static inline float gl_fog_factor(const gl_context_t *ctx, float dist) {
    float f;
    switch (ctx->fog_mode) {
        case GL_EXP:
            f = gl_exp(-ctx->fog_density * dist);
            break;
        case GL_EXP2: {
            const float d = ctx->fog_density * dist;
            f = gl_exp(-(d * d));
            break;
        }
        default: { /* GL_LINEAR */
            const float span = ctx->fog_end - ctx->fog_start;
            f = (span == 0.0f) ? 0.0f : (ctx->fog_end - dist) / span;
            break;
        }
    }
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f;
}

/* Matrix operations */
void mat4_identity(gl_mat4_t *out);
void mat4_mult(gl_mat4_t *out, const gl_mat4_t *a, const gl_mat4_t *b);
void mat4_translate(gl_mat4_t *out, float x, float y, float z);
void mat4_rotate(gl_mat4_t *out, float angle_deg, float x, float y, float z);
void mat4_scale(gl_mat4_t *out, float x, float y, float z);
void mat4_frustum(gl_mat4_t *out, float l, float r, float b, float t, float n, float f);
void mat4_ortho(gl_mat4_t *out, float l, float r, float b, float t, float n, float f);
void mat4_transform_vec4(float *out4, const gl_mat4_t *m, const float *in4);
GLboolean mat4_invert(gl_mat4_t *out, const gl_mat4_t *in);
void gl_apply_texgen(const gl_context_t *ctx, gl_vertex_t *v);

/* The UCP_ENA_0..5 bits of PA_CL_CLIP_CNTL (0x204), bits 0..5 per
 * mesa/src/amd/registers/gfx103.json. */
static inline uint32_t gl_compute_clip_cntl(const gl_context_t *ctx) {
    uint32_t v = 0u;
    if (!ctx) return 0u;
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        if (ctx->clip_plane_enabled[i]) v |= (1u << i);
    }
    return v;
}

/* The plane the hardware clipper wants, which is **clip space, not eye space**.
 *
 * "Clip-Space Plane = Eye-Space Plane * Projection Matrix" - mesa/src/mesa/main/clip.c:40-51,
 * which multiplies the eye plane by the projection's *inverse* because a plane transforms by the
 * inverse of the transform its points take. Mesa passes the eye-space form only when a vertex
 * shader writes a clip vertex (st_atom_clip.c:52-55); these shaders do not, so the fixed-function
 * clipper applies and it reads clip space.
 *
 * Returns false when the projection cannot be inverted, leaving `out` alone - the caller then
 * writes nothing rather than a plane made of infinities. */
GLboolean gl_compute_clip_plane_hw(const gl_context_t *ctx, int i, float out[4]);
void gl_hw_emit_clip_planes(const gl_context_t *ctx, uint32_t **dw_ptr);
/* The binding a glTex* call names, as a pointer into the context so callers can read and write
 * it. NULL for a target this implementation does not have; the caller reports GL_INVALID_ENUM. */
static inline GLuint *gl_binding_slot(gl_context_t *ctx, GLenum target) {
    if (!ctx) return NULL;
    if (target == GL_TEXTURE_2D) return &ctx->bound_texture_2d;
    if (target == GL_TEXTURE_1D) return &ctx->bound_texture_1d;
    return NULL;
}

static inline GLuint gl_default_texture_id(GLenum target) {
    return (target == GL_TEXTURE_1D) ? OOPS_GL_DEFAULT_TEXTURE_1D : OOPS_GL_DEFAULT_TEXTURE_2D;
}

/* **The texture a draw samples**, which is not the same question as the one above.
 *
 * When more than one target is enabled the specification says the highest dimensionality wins, so
 * 2D beats 1D and a program can leave a 1D texture bound while drawing with a 2D one. Answering
 * this with "whatever is bound to 2D" would give the wrong texture to a program using 1D, and
 * answering it with "whichever was bound last" would give a different wrong one. */
static inline GLuint gl_effective_texture_id(const gl_context_t *ctx) {
    if (!ctx) return 0u;
    if (ctx->cap_texture_2d && ctx->bound_texture_2d > 0u) return ctx->bound_texture_2d;
    if (ctx->cap_texture_1d && ctx->bound_texture_1d > 0u) return ctx->bound_texture_1d;
    return 0u;
}

size_t gl_unpack_row_stride(const gl_context_t *ctx, GLsizei width, size_t pixel_bytes);
size_t gl_unpack_row_stride_bytes(const gl_context_t *ctx, size_t bytes);
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

/*
 * One GL blend factor as the colour block's `BlendOp`.
 *
 * Values from `oops-mesa/mesa/src/amd/registers/gfx10.json`, enum `BlendOp` - they are a closed
 * table, so a factor with no entry is refused by falling back rather than being approximated.
 */
static inline uint32_t gl_blend_op(GLenum factor, uint32_t fallback) {
    switch (factor) {
        case GL_ZERO:                     return 0u;  /* BLEND_ZERO */
        case GL_ONE:                      return 1u;  /* BLEND_ONE */
        case GL_SRC_COLOR:                return 2u;  /* BLEND_SRC_COLOR */
        case GL_ONE_MINUS_SRC_COLOR:      return 3u;  /* BLEND_ONE_MINUS_SRC_COLOR */
        case GL_SRC_ALPHA:                return 4u;  /* BLEND_SRC_ALPHA */
        case GL_ONE_MINUS_SRC_ALPHA:      return 5u;  /* BLEND_ONE_MINUS_SRC_ALPHA */
        case GL_DST_ALPHA:                return 6u;  /* BLEND_DST_ALPHA */
        case GL_ONE_MINUS_DST_ALPHA:      return 7u;  /* BLEND_ONE_MINUS_DST_ALPHA */
        case GL_DST_COLOR:                return 8u;  /* BLEND_DST_COLOR */
        case GL_ONE_MINUS_DST_COLOR:      return 9u;  /* BLEND_ONE_MINUS_DST_COLOR */
        case GL_SRC_ALPHA_SATURATE:       return 10u; /* BLEND_SRC_ALPHA_SATURATE */
        default:                          return fallback;
    }
}

/* A GL blend equation as the colour block's `CombFunc`, same file, enum `CombFunc`. The two
 * subtractions are not interchangeable: `COMB_SRC_MINUS_DST` is GL's `GL_FUNC_SUBTRACT` and
 * `COMB_DST_MINUS_SRC` is `GL_FUNC_REVERSE_SUBTRACT`, and swapping them negates the result. */
static inline uint32_t gl_blend_comb(GLenum equation) {
    switch (equation) {
        case GL_FUNC_SUBTRACT:         return 1u; /* COMB_SRC_MINUS_DST */
        case GL_MIN:                   return 2u; /* COMB_MIN_DST_SRC */
        case GL_MAX:                   return 3u; /* COMB_MAX_DST_SRC */
        case GL_FUNC_REVERSE_SUBTRACT: return 4u; /* COMB_DST_MINUS_SRC */
        default:                       return 0u; /* COMB_DST_PLUS_SRC, i.e. GL_FUNC_ADD */
    }
}

/*
 * `CB_BLEND0_CONTROL` from the GL blend state.
 *
 * **This was the constant `0x00002504` whenever blending was enabled**, so `glBlendFunc` and
 * `glBlendEquation` reached the software rasteriser and never the GPU. Decoding that constant
 * against the field layout says more than that: `COLOR_SRCBLEND` 4 and `COLOR_DESTBLEND` 5 are
 * right - they are GL's default `SRC_ALPHA` / `ONE_MINUS_SRC_ALPHA` - but **bit 30, `ENABLE`,
 * is clear**, and bit 13 is set where the register has no field at all. So the colour block was
 * being told the right factors and never told to blend, which is why gl1-probe's `blend` and
 * `blend-equation` both fail on hardware while passing on the host.
 *
 * Field positions and both enums from `oops-mesa/mesa/src/amd/registers/gfx103.json` and its
 * `gfx10.json` base, which agree: `COLOR_SRCBLEND` [0,4], `COLOR_COMB_FCN` [5,7],
 * `COLOR_DESTBLEND` [8,12], `ALPHA_SRCBLEND` [16,20], `ALPHA_COMB_FCN` [21,23],
 * `ALPHA_DESTBLEND` [24,28], `SEPARATE_ALPHA_BLEND` [29], `ENABLE` [30].
 *
 * The alpha channel is always programmed and `SEPARATE_ALPHA_BLEND` always set, because oops-gl
 * tracks a separate alpha factor pair (`glBlendFuncSeparate`) and letting the block infer alpha
 * from the colour fields would quietly ignore it.
 */
static inline uint32_t gl_compute_cb_blend_control(const gl_context_t *ctx) {
    if (!ctx || !ctx->cap_blend) return 0u;
    const uint32_t comb = gl_blend_comb(ctx->blend_equation);
    /* GL_MIN and GL_MAX ignore the factors entirely - the hardware takes them from the operands,
     * so the factor fields are set to ONE to keep them from contributing. */
    const GLboolean minmax = (ctx->blend_equation == GL_MIN || ctx->blend_equation == GL_MAX)
                                 ? GL_TRUE : GL_FALSE;
    const uint32_t csrc = minmax ? 1u : gl_blend_op(ctx->blend_src, 4u);
    const uint32_t cdst = minmax ? 1u : gl_blend_op(ctx->blend_dst, 5u);
    const uint32_t asrc = minmax ? 1u : gl_blend_op(ctx->blend_src_alpha, 4u);
    const uint32_t adst = minmax ? 1u : gl_blend_op(ctx->blend_dst_alpha, 5u);
    return (csrc & 0x1fu) | ((comb & 0x7u) << 5) | ((cdst & 0x1fu) << 8) |
           ((asrc & 0x1fu) << 16) | ((comb & 0x7u) << 21) | ((adst & 0x1fu) << 24) |
           (1u << 29) | /* SEPARATE_ALPHA_BLEND */
           (1u << 30);  /* ENABLE - the bit the constant never set */
}

/*
 * `PA_CL_VPORT_XSCALE`, `XOFFSET`, `YSCALE`, `YOFFSET` from the GL viewport.
 *
 * **The hardware path ignored `glViewport` entirely until 2026-09-17**: these four registers were
 * computed from the render target's width and height, so the GPU mapped NDC across the whole
 * surface whatever the viewport said. The software rasteriser has always honoured it, so the two
 * paths disagreed and nothing on the host could see it - gl1-cube sets the viewport to exactly
 * the framebuffer size, which is the one case where the hardcoded values are right. gl1-probe's
 * first full hardware run found it: every check that sampled a pixel and compared it to an
 * expected colour failed, while the two that compare one frame against another passed, because
 * both frames were displaced identically.
 *
 * The formulas generalise what was there rather than replacing it. For a viewport covering the
 * whole target they reduce to `w/2`, `w/2`, `-h/2`, `h/2` - the previous constants exactly - so
 * the gl-cube oracle frame is unchanged.
 *
 * `fb_h` is the render target's height, not the viewport's: GL measures the viewport from the
 * bottom-left and the rows grow downward, so the offset is the distance from the top of the
 * target to the middle of the viewport.
 */
static inline void gl_compute_vport(const gl_context_t *ctx, uint32_t fb_h, uint32_t out[4]) {
    const float vx = (float)ctx->vp_x;
    const float vy = (float)ctx->vp_y;
    const float vw = (float)ctx->vp_w;
    const float vh = (float)ctx->vp_h;
    out[0] = gl_f32_bits(vw * 0.5f);                    /* XSCALE  */
    out[1] = gl_f32_bits(vx + vw * 0.5f);               /* XOFFSET */
    out[2] = gl_f32_bits(-vh * 0.5f);                   /* YSCALE: NDC +y is up, rows grow down */
    out[3] = gl_f32_bits((float)fb_h - vy - vh * 0.5f); /* YOFFSET */
}

/* PA_SC_VPORT_SCISSOR_0_TL / _BR from the GL scissor box.
 *
 * The scan converter's rectangle is y-down over the render target; GL measures its box from the
 * bottom-left, so the same flip gl_compute_vport applies applies here. The box is clamped to the
 * target because the register fields are unsigned and a box hanging off the left or top would
 * otherwise wrap to an enormous coordinate.
 *
 * With the test disabled - and for a box covering the whole target - this is the full extent,
 * which is what the measured recipe wrote unconditionally. A title that never calls glScissor
 * emits the same stream it did before.
 *
 * Fields TL_X / BR_X [0,14], TL_Y / BR_Y [16,30], WINDOW_OFFSET_DISABLE [31], from
 * mesa/src/amd/registers/gfx103.json (types PA_SC_WINDOW_SCISSOR_TL and _BR; the VPORT_SCISSOR
 * registers at 0x028250 reference them). Only the viewport scissor takes the GL box: the screen,
 * window and generic rectangles are the surface bounds and stay at the full extent.
 */
static inline void gl_compute_scissor(const gl_context_t *ctx, uint32_t fb_w, uint32_t fb_h,
                                      uint32_t out[2]) {
    int32_t l = 0, t = 0;
    int32_t r = (int32_t)fb_w, b = (int32_t)fb_h;
    if (ctx && ctx->cap_scissor_test) {
        const int32_t sw = ctx->sc_w > 0 ? (int32_t)ctx->sc_w : 0;
        const int32_t sh = ctx->sc_h > 0 ? (int32_t)ctx->sc_h : 0;
        l = ctx->sc_x;
        r = ctx->sc_x + sw;
        t = (int32_t)fb_h - (ctx->sc_y + sh);
        b = (int32_t)fb_h - ctx->sc_y;
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r > (int32_t)fb_w) r = (int32_t)fb_w;
        if (b > (int32_t)fb_h) b = (int32_t)fb_h;
        if (r < l) r = l;
        if (b < t) b = t;
    }
    out[0] = 0x80000000u | (((uint32_t)t & 0x7fffu) << 16) | ((uint32_t)l & 0x7fffu);
    out[1] = (((uint32_t)b & 0x7fffu) << 16) | ((uint32_t)r & 0x7fffu);
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
