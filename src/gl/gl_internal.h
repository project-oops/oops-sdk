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
#include "oops/system.h"

#ifndef OOPS_HOST_BUILD
#include "oops/memory.h"
#include "oops/heap.h"
#include "oops/syscall.h"
#include "agc/driver.h"
#endif
#include "agc/tiler.h"
#include "gl_rx.h"
#include "gl_multitex.h"
#include "gl_zs_tiling.h"

/* The capacities are `OOPS_GL_`, not `GL_`: `GL_MAX_LIGHTS` and the like are the
 * enumerants a program passes to glGetIntegerv to ask for these numbers. Anything here
 * is a size this port chose; anything in <GL/gl.h> is a number the specification
 * assigned. */
#define OOPS_GL_MODELVIEW_STACK_CAPACITY 16
#define OOPS_GL_PROJECTION_STACK_CAPACITY 4
#define OOPS_GL_TEXTURE_STACK_CAPACITY 2
/* The texture units: two, GL 1.3's minimum (section 2.6, table 6.29). */
#define OOPS_GL_MAX_TEXTURE_UNITS 2
/*
 * The units a shader may sample, which GL keeps separate from the fixed-function count.
 *
 * `GL_MAX_TEXTURE_UNITS` is how many `glEnable(GL_TEXTURE_2D)` stages combine, and
 * `GL_MAX_TEXTURE_IMAGE_UNITS` how many samplers a fragment shader may name. Different
 * things limit them here:
 *
 *   - the fixed-function count by `OOPS_GL_DESC_SLOT_STRIDE`, 0x80 for two units at
 * 0x40 each, 63 slots from 0x1800. At 0x100 the ring would reach 0x5700 and land on the
 *     GL 2.0 slot region at 0x4000.
 *   - the shader count by `GLSL_GEN_MAX_TEX_SETS` and the GL 2.0 payload slot, a
 *     different region with room above it.
 */
#define OOPS_GL_MAX_TEXTURE_IMAGE_UNITS 4

/*
 * How many independent colour outputs a fragment shader has: one.
 *
 * The fragment stage exports MRT0 and nothing else (`glsl_emit_export_mrt0`).
 * `glGetIntegerv(GL_MAX_DRAW_BUFFERS)`, the length of `gl_FragData` and the
 * `gl_MaxDrawBuffers` constant are one fact (GLSL 1.10, 7.2), so they share this value.
 *
 * Front and back surfaces are double buffering, not two draw buffers: GL 2.0 4.2.1
 * keeps `GL_FRONT` and `GL_BACK` out of `glDrawBuffers`' list, and both together is
 * `glDrawBuffer(GL_FRONT_AND_BACK)`. `glDrawBuffers(2, ...)` is GL_INVALID_VALUE.
 *
 * One deviation: `glDrawBuffers(1, {GL_FRONT})` is accepted here. 4.2.1 excludes
 * `GL_FRONT` from the list because on a stereo framebuffer it covers left and right;
 * this implementation has no stereo, so the name covers one surface and refusing it
 * would report a stereo constraint a program cannot have hit.
 */
#define OOPS_GL_MAX_DRAW_BUFFERS 1

/* How many sampler descriptor sets a compiled pixel shader carries, which is one
 * fact in two layouts: the payload slot holds this many 0x40 descriptor sets before the
 * uniform block, and the scalar file holds this many at `GLSL_GEN_TEX_SGPR_BASE`.
 * `glsl_internal.h` derives `GLSL_GEN_MAX_TEX_SETS` from it rather than restating it,
 * because the two drifting apart would put the shader's descriptors and the draw's at
 * different offsets - which is a wrong texture rather than a diagnostic. Here because
 * that header includes this one. */
#define OOPS_GL_GL2_TEX_SETS OOPS_GL_MAX_TEXTURE_IMAGE_UNITS

/* Distinct textures a frame's draw census holds before it starts counting overflow -
 * see `hw_tex_census`. */
#define OOPS_GL_TEX_CENSUS 24u

/* The patchable slots in the two master pixel shaders, as an index into
 * `hw_patch_writes` - a count per slot of how many times a frame it actually changed.
 * Here for the same reason the descriptor ring's size is: `gl_context_t` sizes an array
 * by it. */
enum {
    GL_PATCH_SLOT_ENV,      /* the texture environment's combine */
    GL_PATCH_SLOT_ENV1,     /* and the second unit's */
    GL_PATCH_SLOT_SUM,      /* the secondary colour added after texturing */
    GL_PATCH_SLOT_SAMPLE,   /* 2D, cube, volume, depth or shadow */
    GL_PATCH_SLOT_UNIT1,    /* the second unit's sample */
    GL_PATCH_SLOT_COVERAGE, /* antialiasing */
    GL_PATCH_SLOT_EXPORT,   /* one colour target or two */
    GL_PATCH_SLOT_FOG,
    GL_PATCH_SLOT_STIPPLE,
    GL_PATCH_SLOT_COUNT
};

/* Slots in the texture descriptor ring. Here rather than with the rest of the
 * descriptor map further down, because `gl_context_t` sizes an array by it; what it
 * means is documented there. */
#define OOPS_GL_DESC_RING_SLOTS 63u
#define OOPS_GL_MAX_IMMEDIATE_VERTS 2048
/* How many list names exist. A list's contents are not fixed: they grow from the SDK
 * heap as they are recorded, like a texture's image or a buffer object's store, so an
 * unused list costs no context memory. */
#define GL_MAX_LISTS 256
/* How deep one list may call another before it is refused as recursion. GL leaves the
 * limit implementation-defined and requires at least 64. */
#define GL_MAX_LIST_DEPTH 64
/* The specification's minimum, which is what this provides. */
#define OOPS_GL_ATTRIB_STACK_CAPACITY 16
#define OOPS_GL_CLIENT_ATTRIB_STACK_CAPACITY 16
/* Texture names. SuperTux's menu alone exhausted 256; its tile and sprite images number
 * in the thousands. 648 bytes each, in the context's host memory. */
#define OOPS_GL_MAX_TEXTURE_OBJECTS 4096
/* Framebuffer and renderbuffer objects. Fewer than textures because a program has a
 * handful of render targets where it has hundreds of images - Mesa sizes neither,
 * having no fixed pool, so these are this implementation's own and reported as
 * GL_OUT_OF_MEMORY when exhausted, the way glGenTextures does. */
#define OOPS_GL_MAX_FRAMEBUFFER_OBJECTS 64
#define OOPS_GL_MAX_RENDERBUFFER_OBJECTS 64
/* Distinct sites that may submit a frame, for the per-site flush census. The table has
   slack so a new site is counted under its own name rather than folded into the unnamed
   remainder. Here rather than beside the other DCB sizes because the context struct
   below needs it. */
#define OOPS_GL_FLUSH_SITES 20u
/* Copies of the textured pixel shader held at once, so a patched variant can be
   published without waiting for the GPU. The rationale is at OOPS_GL_PS_RING_OFFSET;
   the count is here because the context struct below sizes an array with it. */
#define OOPS_GL_PS_RING_SLOTS 6u
/* Buffer object names. A program may allocate two per mesh (a vertex and an index
 * buffer, as Neverball's `share/solid_draw.c:363-369` does) and keep two levels loaded
 * at once, and one that ignores GL_OUT_OF_MEMORY goes on to pass byte offsets as client
 * pointers. 1024 is 40 KB of context; `gl_array_base` indexes this table directly. */
#define OOPS_GL_MAX_BUFFER_OBJECTS 1024
#define OOPS_GL_MAX_QUERY_OBJECTS 64
/* GL 2.0's generic vertex attribute slots. 16 is the specification's minimum for
 * GL_MAX_VERTEX_ATTRIBS, so a program that asks the limit and packs to it gets what it
 * asked for. Up here with the other capacities rather than beside the shader objects
 * because a vertex carries one value per slot - see gl_vertex_t. */
#define OOPS_GL_MAX_VERTEX_ATTRIBS 16
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
 * What is recorded is the call, not its effect. A list replays the same calls in the
 * same order, so a list compiled before a texture is bound and executed after it draws
 * with the later texture, as GL specifies.
 *
 * Scalar arguments are inline (GL_LIST_MAX_ARGS), and anything passed by pointer is
 * copied into `data`, which the list owns: a matrix, an image unpacked at compile time,
 * a list of names. The copy is the specification's rule: a list must not see what the
 * pointer holds later. */
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
    GL_LIST_OP_ORTHO,
    GL_LIST_OP_FRUSTUM,
    GL_LIST_OP_BIND_TEXTURE,
    GL_LIST_OP_SHADE_MODEL,
    GL_LIST_OP_CULL_FACE,
    GL_LIST_OP_FRONT_FACE,
    GL_LIST_OP_DEPTH_FUNC,
    GL_LIST_OP_BLEND_FUNC,
    GL_LIST_OP_BLEND_FUNC_SEPARATE,
    GL_LIST_OP_BLEND_EQUATION,
    GL_LIST_OP_BLEND_COLOR,
    GL_LIST_OP_LOGIC_OP,
    GL_LIST_OP_ALPHA_FUNC,
    GL_LIST_OP_DEPTH_MASK,
    GL_LIST_OP_DEPTH_RANGE,
    GL_LIST_OP_COLOR_MASK,
    GL_LIST_OP_CLEAR,
    GL_LIST_OP_CLEAR_COLOR,
    GL_LIST_OP_CLEAR_DEPTH,
    GL_LIST_OP_CLEAR_STENCIL,
    GL_LIST_OP_STENCIL_FUNC,
    GL_LIST_OP_STENCIL_OP,
    GL_LIST_OP_STENCIL_MASK,
    GL_LIST_OP_CLIP_PLANE,
    GL_LIST_OP_COLOR_MATERIAL,
    GL_LIST_OP_LIGHT_FV,
    GL_LIST_OP_LIGHT_IV,
    GL_LIST_OP_MATERIAL_FV,
    GL_LIST_OP_MATERIAL_IV,
    GL_LIST_OP_LIGHT_MODEL_FV,
    GL_LIST_OP_LIGHT_MODEL_IV,
    GL_LIST_OP_FOG_F,
    GL_LIST_OP_FOG_I,
    GL_LIST_OP_FOG_FV,
    GL_LIST_OP_FOG_IV,
    GL_LIST_OP_TEX_ENV_I,
    GL_LIST_OP_TEX_ENV_FV,
    GL_LIST_OP_TEX_ENV_IV,
    GL_LIST_OP_TEX_GEN_I,
    GL_LIST_OP_TEX_GEN_FV,
    GL_LIST_OP_TEX_GEN_IV,
    GL_LIST_OP_TEX_PARAMETER_I,
    GL_LIST_OP_TEX_PARAMETER_F,
    GL_LIST_OP_TEX_PARAMETER_FV,
    GL_LIST_OP_TEX_PARAMETER_IV,
    GL_LIST_OP_TEX_IMAGE_1D,
    GL_LIST_OP_TEX_IMAGE_2D,
    GL_LIST_OP_TEX_SUB_IMAGE_1D,
    GL_LIST_OP_TEX_SUB_IMAGE_2D,
    GL_LIST_OP_COPY_TEX_IMAGE_1D,
    GL_LIST_OP_COPY_TEX_IMAGE_2D,
    GL_LIST_OP_COPY_TEX_SUB_IMAGE_1D,
    GL_LIST_OP_COPY_TEX_SUB_IMAGE_2D,
    GL_LIST_OP_PRIORITIZE_TEXTURES,
    GL_LIST_OP_ACTIVE_TEXTURE,
    GL_LIST_OP_POINT_SIZE,
    GL_LIST_OP_LINE_WIDTH,
    GL_LIST_OP_POLYGON_OFFSET,
    GL_LIST_OP_SCISSOR,
    GL_LIST_OP_VIEWPORT,
    GL_LIST_OP_HINT,
    GL_LIST_OP_DRAW_BUFFER,
    GL_LIST_OP_READ_BUFFER,
    GL_LIST_OP_PIXEL_ZOOM,
    GL_LIST_OP_RASTER_POS,
    GL_LIST_OP_WINDOW_POS,
    GL_LIST_OP_BITMAP,
    GL_LIST_OP_DRAW_PIXELS,
    GL_LIST_OP_COPY_PIXELS,
    GL_LIST_OP_PUSH_ATTRIB,
    GL_LIST_OP_POP_ATTRIB,
    GL_LIST_OP_LIST_BASE,
    GL_LIST_OP_CALL_LIST,
    GL_LIST_OP_CALL_LISTS,
    GL_LIST_OP_POLYGON_MODE,
    GL_LIST_OP_EDGE_FLAG,
    GL_LIST_OP_INDEX,
    GL_LIST_OP_CLEAR_INDEX,
    GL_LIST_OP_INDEX_MASK,
    GL_LIST_OP_SAMPLE_COVERAGE,
    GL_LIST_OP_MAP1, /* the control points, packed at compile time, are the data */
    GL_LIST_OP_MAP2,
    GL_LIST_OP_MAP_GRID1,
    GL_LIST_OP_MAP_GRID2,
    GL_LIST_OP_EVAL_COORD1,
    GL_LIST_OP_EVAL_COORD2,
    GL_LIST_OP_EVAL_POINT1,
    GL_LIST_OP_EVAL_POINT2,
    GL_LIST_OP_EVAL_MESH1,
    GL_LIST_OP_EVAL_MESH2,
    GL_LIST_OP_INIT_NAMES,
    GL_LIST_OP_LOAD_NAME,
    GL_LIST_OP_PUSH_NAME,
    GL_LIST_OP_POP_NAME,
    GL_LIST_OP_PASS_THROUGH,
    GL_LIST_OP_PIXEL_TRANSFER,
    GL_LIST_OP_PIXEL_MAP, /* the values, as floats, are the data */
    GL_LIST_OP_LINE_STIPPLE,
    GL_LIST_OP_POLYGON_STIPPLE, /* the mask, packed tight at compile time, is the data
                                 */
    GL_LIST_OP_ACCUM,
    GL_LIST_OP_CLEAR_ACCUM,
    GL_LIST_OP_TEX_IMAGE_3D, /* the volume, packed tight at compile time, is the data */
    GL_LIST_OP_TEX_SUB_IMAGE_3D,
    GL_LIST_OP_COPY_TEX_SUB_IMAGE_3D,
    GL_LIST_OP_SECONDARY_COLOR,
    GL_LIST_OP_FOG_COORD,
    GL_LIST_OP_POINT_PARAMETER, /* the pname, then three floats */
    GL_LIST_OP_BEGIN_QUERY,
    GL_LIST_OP_END_QUERY,
    GL_LIST_OP_MULTI_TEXCOORD, /* the unit as named (GL_TEXTUREn), then s, t, r, q */
} gl_list_op_t;

typedef union {
    GLint i;
    GLuint u;
    GLenum e;
    GLfloat f;
} gl_list_arg_t;

/* Ten, for glTexSubImage3D - the longest compiled call, with ten scalar arguments. */
#define GL_LIST_MAX_ARGS 10

typedef struct {
    gl_list_op_t op;
    gl_list_arg_t a[GL_LIST_MAX_ARGS];
    void *data; /* owned by the list; NULL when the call passed no pointer, or a NULL
                   one */
} gl_list_cmd_t;

typedef struct {
    GLboolean used;     /* named by glGenLists or glNewList */
    GLboolean compiled; /* glEndList has closed it, so it may be called */
    GLuint count;
    GLuint capacity;
    gl_list_cmd_t *cmds; /* grown as it is recorded into; freed with the list */
} gl_display_list_t;

static inline gl_list_arg_t gl_la_f(GLfloat v) {
    gl_list_arg_t a;
    a.f = v;
    return a;
}
static inline gl_list_arg_t gl_la_i(GLint v) {
    gl_list_arg_t a;
    a.i = v;
    return a;
}
static inline gl_list_arg_t gl_la_u(GLuint v) {
    gl_list_arg_t a;
    a.u = v;
    return a;
}
static inline gl_list_arg_t gl_la_e(GLenum v) {
    gl_list_arg_t a;
    a.e = v;
    return a;
}

/* The default texture of each target - the object a glTex* call acts on when nothing is
 * bound.
 *
 * Reserved ids, above anything glGenTextures' count from 1 will reach, so a default
 * texture is never also a generated one. There is one per target because GL has one per
 * target - sharing them would let a 1D upload overwrite the default 2D image.
 */
#define OOPS_GL_DEFAULT_TEXTURE_1D 0xfffffe01u
#define OOPS_GL_DEFAULT_TEXTURE_2D 0xfffffe02u
#define OOPS_GL_DEFAULT_TEXTURE_3D 0xfffffe03u
#define OOPS_GL_DEFAULT_TEXTURE_CUBE 0xfffffe04u

/* The largest 3D texture on a side. The specification's minimum is 16; this holds the
 * volume on the CPU, and 256 on a side is 64 MB at RGBA8 - the most a program should be
 * allowed to ask of process memory for one texture. */
#define OOPS_GL_MAX_3D_TEXTURE_SIZE 256

/* The largest cube map face. The specification's minimum is 16; the six faces live on
 * the CPU, and 1024 on a side is 24 MB for the base levels at RGBA8. */
#define OOPS_GL_MAX_CUBE_MAP_TEXTURE_SIZE 1024

/* A cube map face target's index, +X -X +Y -Y +Z -Z being 0..5 as GL numbers them - or
 * -1. */
#define GL_CUBE_FACE_INDEX(t)                                                          \
    (((t) >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && (t) <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)  \
         ? (int)((t) - GL_TEXTURE_CUBE_MAP_POSITIVE_X)                                 \
         : -1)

/* The largest texture, and so the mip levels a texture can have: 2048 halves to 1 in
 * eleven steps, levels 0..11. */
#define OOPS_GL_MAX_TEXTURE_SIZE 2048
#define OOPS_GL_MAX_TEXTURE_LEVELS 12
/* Samples per pixel, which is one. This rasterises one sample and the hardware path's
 * colour target is single-sampled, so `GL_MAX_SAMPLES` is 1 and
 * `glRenderbufferStorageMultisample` refuses more rather than quietly giving one. */
#define OOPS_GL_MAX_SAMPLES 1

/* One mip level above the base: RGBA8 rows packed tight, in process memory. The base
 * level is the texture object's own fields, because that is the image the hardware
 * samples - see gl_texture_object_t. */
typedef struct {
    GLsizei width;
    GLsizei height;
    GLsizei depth; /* 1 but for a 3D texture's level; its slices follow one another */
    void *pixels;  /* NULL when the level was never specified */
    GLint internal_format; /* as the program named it */
    GLenum base_format;    /* what it means - see gl_tex_base_format */
} gl_tex_level_t;

typedef struct gl_texture_object {
    GLuint id;
    GLboolean used;
    /* The target this object was first bound to, and the only one it may be bound to
     * afterwards. Zero until it is first bound. GL calls rebinding to a different
     * target an error, and it is one worth reporting: the object's image has a shape
     * the other target cannot read. */
    GLenum target;
    GLsizei width;
    GLsizei height;
    /* 1 but for a 3D texture, whose base level's slices follow one another in `pixels`,
     * each `pitch * height` pixels. */
    GLsizei depth;
    GLenum format;
    GLenum type;
    /* The base level's internal format, as named and as reduced to its base format. The
     * texels are RGBA8 whatever it is, kept as the base format expands them for
     * sampling - see gl_tex_base_format. */
    GLint internal_format;
    GLenum base_format;
    /* GL_TEXTURE_BORDER_COLOR, clamped to [0, 1] as Mesa keeps it without float
     * textures, and GL_TEXTURE_PRIORITY, which nothing evicts by but a program may set
     * and read back. */
    float border_color[4];
    float priority;
    /* The border colour as the sampler meets it - expanded by the base format as a
     * texel is - and whether the hardware reads it from the border colour table rather
     * than naming one of its three built-in colours (gl_pack_descriptors). */
    float border_hw[4];
    GLboolean border_in_table;
    /* GL 1.2's level-of-detail parameters: the level sampling starts from, the last it
     * may reach, and the clamp on the level of detail - 0, 1000, -1000 and 1000 by
     * default. */
    GLint base_level;
    GLint max_level;
    float min_lod;
    float max_lod;
    /* GL 1.4: this texture's level-of-detail bias, added to the unit's
     * (`tex_lod_bias`), and GL_GENERATE_MIPMAP - rebuild the levels above the base
     * whenever the base changes. */
    float lod_bias;
    GLboolean generate_mipmap;
    /* GL 1.4's depth-texture parameters: GL_NONE or GL_COMPARE_R_TO_TEXTURE, the
     * comparison, and whether the result reads as GL_LUMINANCE, GL_INTENSITY or
     * GL_ALPHA. */
    GLenum compare_mode, compare_func, depth_mode;
    GLenum wrap_s;
    GLenum wrap_t;
    GLenum wrap_r;
    GLenum min_filter;
    GLenum mag_filter;
    void *pixels;       /* Host software copy */
    void *garlic_data;  /* 256-byte aligned Garlic allocation on Prospero */
    uint64_t garlic_va; /* GPU Virtual Address */
    uint32_t pitch; /* pixels per row in memory: equals the width, which is where the
                       sampler takes the pitch from */
    uint32_t img_desc[8];  /* SQ_IMG_RSRC_WORD0..7 */
    uint32_t samp_desc[4]; /* SQ_IMG_SAMP_WORD0..3 */
    GLboolean desc_dirty;
    /* Mip levels 1 and up; the software rasteriser selects among them, and the hardware
     * reads them through `chain_data` below. `mips[0]` is unused. */
    gl_tex_level_t mips[OOPS_GL_MAX_TEXTURE_LEVELS];
    /* The mip chain as the hardware reads it - built by gl_tex_hw_prepare when the
     * texture is complete, power-of-two and filtered through its mipmaps, and otherwise
     * NULL. Laid out as addrlib lays out a linear GFX10 surface: smallest level first
     * and the base level last, each level's rows at its own width rounded up to 256
     * bytes (gfx10addrlib.cpp:5082-5104). `chain_dirty` marks a level changed since it
     * was built; `desc_chain` whether the descriptors currently describe it rather than
     * the base level alone. */
    void *chain_data;
    uint64_t chain_va;
    GLboolean chain_dirty;
    GLboolean desc_chain;
    /* The levels the chain holds: `chain_levels` of them from `chain_base`, the base
     * level at the time it was built. */
    int chain_base;
    int chain_levels;
    /* A cube map's six faces, each with its own levels - face f's level l at
     * `cube[f * OOPS_GL_MAX_TEXTURE_LEVELS + l]`, RGBA8 rows packed tight in process
     * memory. Allocated the first time a face is specified; NULL for every other
     * texture. The object's own image fields stay empty for a cube map. */
    gl_tex_level_t *cube;
    /* The hardware image built from those faces (gl_tex_cube_upload): the
     * face size it was built at, and whether a face has been given since. The object's
     * own `width` and `height` stay empty for a cube map, so the descriptor takes the
     * size from here. */
    GLsizei cube_hw_dim;
    GLboolean cube_hw_dirty;
} gl_texture_object_t;

/*
 * Framebuffer objects. A renderbuffer is an image with no sampling behind it:
 * storage, a size and a format, existing only to be drawn into. A framebuffer is three
 * attachment points naming one of those or a texture level.
 *
 * `pixels` is RGBA8 for a colour renderbuffer and 32 bits a sample for a depth or
 * stencil one, which is what the rest of this GL already carries them as - a
 * renderbuffer introduces no new pixel layout, only a second place one can live.
 */
typedef struct gl_renderbuffer_object {
    GLuint id;
    GLboolean used;
    GLenum internal_format;
    GLsizei width;
    GLsizei height;
    uint32_t *pixels;
    /* Whether `pixels` came from the GPU allocator, which is what decides both how it
     * is freed and whether the command processor can be pointed at it. False on a build
     * machine, where there is no GPU and the heap is the only allocator. */
    GLboolean gpu_resident;
} gl_renderbuffer_object_t;

typedef enum {
    GL_FB_ATTACH_NONE = 0,
    GL_FB_ATTACH_TEXTURE,
    GL_FB_ATTACH_RENDERBUFFER
} gl_fb_attach_kind_t;

typedef struct gl_fb_attachment {
    gl_fb_attach_kind_t kind;
    /* The texture or renderbuffer name. Kept as a name rather than a pointer because
     * either may be deleted while attached, and GL says the attachment goes with it - a
     * pointer would outlive its object and this has to notice. */
    GLuint name;
    GLenum textarget;
    GLint level;
} gl_fb_attachment_t;

typedef struct gl_framebuffer_object {
    GLuint id;
    GLboolean used;
    gl_fb_attachment_t color0;
    gl_fb_attachment_t depth;
    gl_fb_attachment_t stencil;
} gl_framebuffer_object_t;

/* Evaluators. The largest order a map may have - the specification's minimum is 8, and
 * 30 is Mesa's MAX_EVAL_ORDER (main/config.h:75). */
#define OOPS_GL_MAX_EVAL_ORDER 30
/* The name stack's depth in GL_SELECT: the specification's minimum, and Mesa's
 * MAX_NAME_STACK_DEPTH (main/config.h:78). */
#define OOPS_GL_MAX_NAME_STACK_DEPTH 64

/* Pixel maps: ten tables, indexed by map minus GL_PIXEL_MAP_I_TO_I in the order the
 * enums run (I_TO_I, S_TO_S, I_TO_R/G/B/A, R_TO_R, G_TO_G, B_TO_B, A_TO_A), each up to
 * Mesa's MAX_PIXEL_MAP_TABLE entries (main/config.h:69). */
#define OOPS_GL_PIXEL_MAPS 10
#define OOPS_GL_MAX_PIXEL_MAP_TABLE 256

/* Nine maps per dimension, indexed by target minus GL_MAP1_COLOR_4 (or
 * GL_MAP2_COLOR_4): colour, index, normal, texture coordinates 1-4, vertex 3 and 4 -
 * the order the enums run in. */
#define OOPS_GL_EVAL_MAPS 9

/* One map: its order and domain in each dimension (a 1D map has vorder 1 and ignores
 * v), and its control points packed tight - `uorder * vorder` points of the target's
 * component count, u outermost. `points` is NULL until glMap first defines the map,
 * standing for the one initial control point every map starts with; see gl_eval_points.
 */
typedef struct {
    GLint uorder, vorder;
    float u1, u2, v1, v2;
    float *points;
} gl_eval_map_t;

typedef struct {
    float m[16]; /* Column-major: m[col*4 + row] */
} gl_mat4_t;

typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float nx, ny, nz;
    /* glEdgeFlag's value when this vertex was issued: whether the polygon edge that
     * starts here is a boundary edge, drawn by GL_LINE and GL_POINT polygon modes. */
    GLboolean edge;
    /* Each unit's texture coordinate, s, t, r and q, after generation and the texture
     * matrix and undivided - the rasteriser interpolates all four and divides by q per
     * fragment (the hardware vertex, which carries unit 0's s and t, divides at the
     * corner). r is what a 3D or cube texture reads, and a depth texture's comparison
     * reference. */
    float tc[OOPS_GL_MAX_TEXTURE_UNITS][4];
    /* The secondary colour (GL 1.4) - glSecondaryColor's, or the secondary colour
     * array's. What the colour sum adds when lighting is off; lighting replaces it with
     * the specular term. */
    float sr, sg, sb;
    /* The fog coordinate (GL 1.4) - glFogCoord's, or the fog coordinate array's. What
     * fog reads in place of the eye distance when GL_FOG_COORD_SRC is GL_FOG_COORD. */
    float fogc;
    /* GL 2.0's generic vertex attributes, one four-component value per slot. Filled by
     * the vertex fetch from each enabled array, or from `glVertexAttrib`'s current
     * value where the array is off.
     *
     * Carried on the vertex rather than read from the context when the shader runs,
     * because a draw fetches every vertex into a buffer before any of them is shaded,
     * so the context's current values would be the last vertex's for all of them. It is
     * the largest thing on a vertex. */
    float attrib[OOPS_GL_MAX_VERTEX_ATTRIBS][4];
} gl_vertex_t;

typedef struct {
    float sx, sy, sz; /* Screen x, screen y, and depth z in [0.0, 1.0] */
    float inv_w;
    float r, g, b, a;
    /* Each unit's s, t, r, q, undivided: s, t and r are divided by q per fragment. */
    float tc[OOPS_GL_MAX_TEXTURE_UNITS][4];
    /* Signed distance from each user clip plane, in eye space. Interpolated across the
     * triangle and tested per fragment rather than the triangle being geometrically
     * clipped: the visible result is the same and it does not need a clipper that turns
     * one triangle into several. */
    float cd[OOPS_GL_CLIP_PLANE_COUNT];
    /* The fog factor: 1 means fully the fragment's own colour, 0 fully the fog colour.
     * Computed per vertex from the eye-space distance and interpolated, which is where
     * the per-fragment quality comes from - folding fog into the vertex colour instead
     * is exact only when the factor is constant across the primitive. */
    float fog;
    /* The secondary colour - the lit specular term under GL_SEPARATE_SPECULAR_COLOR,
     * zero otherwise - interpolated and added after texturing, before fog. */
    float sr, sg, sb;
    /* What a vertex shader wrote, GL_SHADER_VARY_FLOATS of it, or NULL when the
     * fixed-function pipeline produced this vertex. A pointer rather than the block
     * itself: the fixed-function path is the hot one and would otherwise carry fifty
     * floats per vertex it never reads. The block lives in the caller's frame and
     * outlives the rasterise. */
    const float *vary;
} gl_screen_vertex_t;

/* GL_COMBINE's state (GL 1.3): the colour and alpha functions, each argument's source
 * and operand, and the two scales. GL_TEXTURE_BIT's, with the rest of the texture
 * environment. */
typedef struct {
    GLenum mode_rgb, mode_alpha;
    GLenum source_rgb[3], source_alpha[3];
    GLenum operand_rgb[3], operand_alpha[3];
    float scale_rgb, scale_alpha;
} gl_combine_t;

/* One texture unit's server state: everything glActiveTexture selects between. The
 * enables and bindings per target, the environment and combiner, GL 1.4's per-unit LOD
 * bias, the texture matrix stack, and coordinate generation, S, T, R, Q.
 *
 * `texgen_eye_plane` is stored already multiplied by the inverse modelview of the
 * moment glTexGen was called, as the specification says; that is what makes
 * GL_EYE_LINEAR differ from GL_OBJECT_LINEAR. glGetTexGen returns what is stored, as
 * the specification also says. */
typedef struct {
    GLboolean cap_texture_1d, cap_texture_2d, cap_texture_3d, cap_texture_cube_map;
    /* One binding per target, all live at once - that is what makes GL_TEXTURE_1D a
     * target rather than a shape of 2D texture. */
    GLuint bound_texture_1d, bound_texture_2d, bound_texture_3d, bound_texture_cube;
    GLenum tex_env_mode;
    float tex_env_color[4];
    gl_combine_t combine; /* GL_COMBINE's functions, arguments and scales */
    /* GL 1.4's level-of-detail bias (glTexEnv, GL_TEXTURE_FILTER_CONTROL), added to the
     * texture's own. */
    float tex_lod_bias;
    gl_mat4_t texture_stack[OOPS_GL_TEXTURE_STACK_CAPACITY];
    int texture_depth;
    GLenum texgen_mode[4];
    float texgen_object_plane[4][4];
    float texgen_eye_plane[4][4];
    GLboolean texgen_enabled[4];
    /* GL_COORD_REPLACE (ARB_point_sprite, GL 2.0): with GL_POINT_SPRITE enabled, this
     * unit takes its s and t across the point's own square instead of from the vertex,
     * so a point becomes a textured sprite. Per unit, set through
     * `glTexEnvi(GL_POINT_SPRITE, ...)` - a third target for glTexEnv beside
     * GL_TEXTURE_ENV and GL_TEXTURE_FILTER_CONTROL. */
    GLboolean coord_replace;
} gl_tex_unit_t;

/* Every unit's texel for one fragment - zero for a unit applying none, which is what a
 * GL_TEXTUREn combiner source reads for it (Mesa, main/ff_fragment_shader.c:760-762). A
 * struct so that it passes as const: C11 will not convert float (*)[4] to const float
 * (*)[4] implicitly. */
typedef struct {
    float t[OOPS_GL_MAX_TEXTURE_UNITS][4];
} gl_unit_texels_t;

/* One entry of the attribute stack: the GL state `glPushAttrib` can save.
 *
 * A struct of its own rather than a copy of the context, because the context also holds
 * the display lists, the textures and the matrix stacks, and snapshotting those would
 * be a large copy per push. What is here is the state a program can change and expect
 * back.
 */
typedef struct {
    GLbitfield mask; /* what this push asked to save, and so what the pop restores */

    /* GL_CURRENT_BIT */
    float cur_color[4];
    float cur_normal[3];
    /* And the current raster position with what was latched with it - the current
     * group's too (GL 1.3, table 6.5), as Mesa's push copies them
     * (main/attrib.c:122-125, mtypes.h:347-355). */
    float raster_pos[4];
    float raster_color[4];
    float raster_texcoord[OOPS_GL_MAX_TEXTURE_UNITS][4];
    float raster_distance;
    GLboolean raster_valid;
    /* Four, not two: glTexCoord3 and glTexCoord4 exist and q is a projective divide, so
     * a two-component current texture coordinate cannot hold what they set. Defaults
     * (0,0,0,1). One per unit (GL 1.3: the current bit holds every unit's). */
    float cur_texcoord[OOPS_GL_MAX_TEXTURE_UNITS][4];
    GLboolean cur_edge_flag;
    float cur_index;
    float cur_secondary[4];
    float cur_fog_coord;

    /* GL_ENABLE_BIT, and the individual buffer bits that also carry an enable */
    GLboolean cap_depth_test, cap_cull_face, cap_blend, cap_scissor_test;
    GLboolean cap_lighting, cap_normalize,
        cap_color_material;       /* the texture enables: tex_units */
    GLboolean cap_rescale_normal; /* also GL_TRANSFORM_BIT, with GL_NORMALIZE */
    GLboolean cap_alpha_test, cap_polygon_offset_fill;
    GLboolean cap_polygon_offset_line,
        cap_polygon_offset_point;             /* also GL_POLYGON_BIT */
    GLboolean cap_dither, cap_index_logic_op; /* also GL_COLOR_BUFFER_BIT */

    /* GL_MULTISAMPLE_BIT (the enables also GL_ENABLE_BIT) */
    GLboolean cap_multisample, cap_sample_alpha_to_coverage, cap_sample_alpha_to_one;
    GLboolean cap_sample_coverage;
    float sample_coverage_value;
    GLboolean sample_coverage_invert;

    /* GL_POINT_BIT, GL_LINE_BIT */
    float point_size;
    float point_size_min, point_size_max, point_fade_threshold, point_atten[3];
    float line_width;
    GLboolean cap_line_stipple; /* also GL_ENABLE_BIT */
    GLint line_stipple_factor;
    GLushort line_stipple_pattern;
    /* The smoothing enables: GL_POINT_BIT's, GL_LINE_BIT's and GL_POLYGON_BIT's, and
     * all GL_ENABLE_BIT's. */
    GLboolean cap_point_smooth, cap_line_smooth, cap_polygon_smooth;
    /* GL_POINT_BIT's too: the sprite enable and which corner its generated t starts
     * from. */
    GLboolean cap_point_sprite;
    GLenum point_sprite_origin;

    /* GL_POLYGON_STIPPLE_BIT; the enable is GL_POLYGON_BIT's and GL_ENABLE_BIT's */
    GLboolean cap_polygon_stipple;
    uint32_t polygon_stipple[32];

    /* GL_ACCUM_BUFFER_BIT */
    float accum_clear[4];

    /* GL_EVAL_BIT (the enables also GL_ENABLE_BIT). The maps' control points are not
     * saved - the specification leaves them out of every attribute group. */
    GLboolean cap_map1[OOPS_GL_EVAL_MAPS], cap_map2[OOPS_GL_EVAL_MAPS];
    GLboolean cap_auto_normal;
    GLint grid1_un, grid2_un, grid2_vn;
    float grid1_u1, grid1_u2, grid2_u1, grid2_u2, grid2_v1, grid2_v2;

    /* GL_PIXEL_MODE_BIT. The pixel maps are in no attribute group. */
    float pixel_scale[4], pixel_bias[4];
    float depth_scale, depth_bias;
    GLint index_shift, index_offset;
    GLboolean map_color, map_stencil;
    float pixel_zoom_x, pixel_zoom_y;

    /* GL_HINT_BIT */
    GLenum perspective_hint, hint_point_smooth, hint_line_smooth, hint_polygon_smooth,
        hint_fog;
    GLenum hint_texture_compression, hint_generate_mipmap;

    /* GL_DEPTH_BUFFER_BIT */
    GLenum depth_func;
    GLboolean depth_mask;
    float clear_depth;

    /* GL_COLOR_BUFFER_BIT */
    GLenum blend_src, blend_dst, blend_src_alpha, blend_dst_alpha, blend_equation;
    GLenum blend_equation_alpha;
    float blend_color[4];
    GLboolean cap_color_logic_op; /* also GL_ENABLE_BIT */
    GLenum logic_op;
    GLboolean color_mask[4];
    GLenum draw_buffer; /* GL_COLOR_BUFFER_BIT's */
    GLenum read_buffer; /* GL_PIXEL_MODE_BIT's, kept here beside it */
    float clear_color[4];
    GLenum alpha_func;
    float alpha_ref;
    float clear_index;
    GLuint index_mask;

    /* GL_POLYGON_BIT */
    GLenum cull_mode, front_face;
    float polygon_offset_factor, polygon_offset_units;
    GLenum polygon_mode[2];

    /* GL_LIGHTING_BIT */
    GLenum shade_model;
    gl_light_t lights[OOPS_GL_LIGHT_COUNT];
    gl_material_t mat_front, mat_back;
    float light_model_ambient[4];
    GLboolean light_model_local_viewer;
    GLenum light_model_color_control;
    GLboolean light_model_two_side;
    GLenum color_material_face, color_material_mode;

    /* GL_TEXTURE_BIT: every unit's state, pushed TEXTURE0 first (GL 1.3, 6.1.14) - all
     * of it but the texture matrix stack, which no attribute group holds - and the
     * active unit selector, which is the texture group's too (table 6.20). The target
     * enables and the generation enables in it are GL_ENABLE_BIT's as well. */
    gl_tex_unit_t tex_units[OOPS_GL_MAX_TEXTURE_UNITS];
    GLuint active_texture;
    /* And the parameters of the texture each unit's target had bound (or its default),
     * 1D, 2D, 3D, cube map - which Mesa saves with the group too
     * (main/attrib.c:251-275, copy_texture_attribs). The id is 0 when there was no such
     * object. */
    GLuint tex_param_id[OOPS_GL_MAX_TEXTURE_UNITS][4];
    struct {
        GLenum wrap_s, wrap_t, wrap_r, min_filter, mag_filter;
        float border_color[4];
        float priority;
        GLint base_level, max_level;
        float min_lod, max_lod;
        float lod_bias;
        GLboolean generate_mipmap;
        GLenum compare_mode, compare_func, depth_mode;
    } tex_params[OOPS_GL_MAX_TEXTURE_UNITS][4];
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
    GLenum fog_coord_src;
    float fog_index;
    GLboolean cap_color_sum; /* also GL_ENABLE_BIT */
    /* GL_STENCIL_BUFFER_BIT. Both faces: GL 2.0 puts the back-face state in this same
     * attribute group. */
    GLboolean cap_stencil_test;
    GLenum stencil_func;
    GLint stencil_ref;
    GLuint stencil_value_mask;
    GLuint stencil_writemask;
    GLenum stencil_fail;
    GLenum stencil_zfail;
    GLenum stencil_zpass;
    GLenum stencil_back_func;
    GLint stencil_back_ref;
    GLuint stencil_back_value_mask;
    GLuint stencil_back_writemask;
    GLenum stencil_back_fail;
    GLenum stencil_back_zfail;
    GLenum stencil_back_zpass;
    GLint clear_stencil;

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
    /* Client memory, or - when `buffer` is non-zero - a byte offset into that buffer
     * object. The same field carries both, as the GL 1.5 API's parameter does. */
    const void *pointer;
    GLboolean enabled;
    /* The buffer bound to GL_ARRAY_BUFFER when this array was specified, or 0 for
     * client memory. The name, not an address: glBufferData may reallocate the storage,
     * and a program may respecify a buffer every frame. */
    GLuint buffer;
} gl_client_array_t;

/* One buffer object. The storage is GPU-mapped where it can be, because the resident
 * draw path points the vertex stage's attribute fetch straight at it - a heap pointer
 * there is a GPU page fault, not a slow draw. The CPU-side array reader works either
 * way, so a GPU allocation that fails falls back to the heap and only costs the
 * resident path: `gpu_visible` is what `gl_hw_can_resident_draw` asks, and it decides
 * which allocator frees the store. */
typedef struct {
    GLuint id;
    GLboolean used;
    void *data;
    GLboolean gpu_visible;
    GLsizeiptr size;
    GLenum usage;
    /* GL 1.5's mapping: whether glMapBuffer has handed the store out, and with what
     * access. */
    GLboolean mapped;
    GLenum access;
} gl_buffer_object_t;

/* One GL 1.5 query object: named by glGenQueries (or a first glBeginQuery), a query
 * object only once it has been begun (`ever_bound`, as Mesa's IsQuery answers), and its
 * last result. */
typedef struct {
    GLuint id;
    GLboolean used, ever_bound, active;
    uint64_t result;
} gl_query_object_t;

/* -------------------------------------------------------------------------
 * GL 2.0: shader objects, program objects and generic vertex attributes
 *
 * The objects `glCreateShader` and `glCreateProgram` hand out. Their bulky parts - the
 * source text, the compiled tree, the uniform values - are allocated, like a buffer
 * object's store and a texture's pixels, so the slot tables stay small enough to live
 * in the context.
 * ------------------------------------------------------------------------- */

/* A compiled translation unit, shared and counted rather than copied.
 *
 * The specification lets a shader be deleted and detached the moment a program has
 * linked it, and the program keeps working (GL 2.0, 2.15.2). Copying the tree into the
 * program at link would cost far more than a reference count. The count is the shader
 * object that compiled it plus every program that linked it.
 *
 * Opaque here: its definition needs `glsl_ast_t`, and `glsl_internal.h` includes this
 * file rather than the other way round. `gl_glsl_unit_release` is the only thing
 * anything outside the front end does to one. */
typedef struct glsl_unit glsl_unit_t;
void gl_glsl_unit_retain(glsl_unit_t *u);
void gl_glsl_unit_release(glsl_unit_t *u);

/* This build's heap: the C library's on a host test build, the SDK's on the target.
 * Defined once in gl_state.c so nothing else has to carry the `#if`. */
void *gl_heap_alloc(size_t bytes);
void gl_heap_free(void *p);

/* The longest name a uniform, attribute or varying may have, with its terminator. The
 * specification sets no minimum; Mesa's is 256. A longer name is refused at link with
 * a diagnostic rather than silently truncated to collide with another. */
#define OOPS_GL_MAX_GLSL_NAME 64
/* One object's info log. A diagnostic here is one line - the front end reports the
 * first problem and stops - so this is the length of the longest such line plus its
 * line and column. */
#define OOPS_GL_INFO_LOG_SIZE 256

#define OOPS_GL_MAX_SHADER_OBJECTS 64
#define OOPS_GL_MAX_PROGRAM_OBJECTS 32
/* GL 2.0 allows several shaders of one type on a program; the linker refuses past this
 * rather than losing one quietly. */
#define OOPS_GL_MAX_ATTACHED_SHADERS 8
#define OOPS_GL_MAX_PROGRAM_UNIFORMS 64
/* `OOPS_GL_MAX_VERTEX_ATTRIBS` is with the other capacities at the top of this file,
 * because `gl_vertex_t` carries one value per slot and is declared long before this
 * section. */
/* Varyings between the stages: eight four-component slots, so GL_MAX_VARYING_FLOATS is
 * 32 - the specification's minimum, and what the interpolator carries per fragment. */
#define OOPS_GL_MAX_PROGRAM_VARYINGS 8
#define OOPS_GL_MAX_VARYING_FLOATS (OOPS_GL_MAX_PROGRAM_VARYINGS * 4)

/* One entry of a linked program's uniform table.
 *
 * `type` is the GL enumerant `glGetActiveUniform` reports - GL_FLOAT_VEC4,
 * GL_FLOAT_MAT4, GL_SAMPLER_2D - rather than the front end's own type, because this
 * table is read by the API and by the back end, and only the front end has the other.
 *
 * The value is always floats, even for an `int`, a `bool` and a sampler: GLSL 1.10's
 * integers fit, the shading hardware has one register file, and `glGetUniformiv`
 * converts on the way out. A sampler's value is its texture unit number,
 * which is what `glUniform1i` on a sampler means. */
typedef struct {
    char name[OOPS_GL_MAX_GLSL_NAME];
    GLenum type;
    GLint size;     /* array elements; 1 for a scalar declaration */
    GLint location; /* what glGetUniformLocation answers, and what glUniform takes */
    int offset;     /* into the program's value pool */
    int floats;     /* how many floats one element occupies: 1, 2, 3, 4, 4, 9 or 16 */
} gl_uniform_t;

/* GLSL 1.10's six sampler types, which are consecutive enumerants (GL_SAMPLER_1D 0x8B5D
 * through GL_SAMPLER_2D_SHADOW 0x8B62). A sampler is not a value the way the others are
 * - its float slot holds a texture unit number - so several places need to tell one
 * apart from a vec. */
static inline GLboolean gl_type_is_sampler(GLenum t) {
    return (t >= GL_SAMPLER_1D && t <= GL_SAMPLER_2D_SHADOW) ? GL_TRUE : GL_FALSE;
}

/* One attribute the linker bound, or one binding `glBindAttribLocation` asked for and
 * the next link will honour. The two are the same shape and are kept in separate
 * tables, because a binding that names no attribute in the shader is not an error and
 * must not appear in `glGetActiveAttrib`'s list. */
typedef struct {
    char name[OOPS_GL_MAX_GLSL_NAME];
    GLenum type;
    GLint size;
    GLint location;
} gl_attrib_binding_t;

/* One varying, as the linker matched it between the two stages: where it starts in the
 * interpolated block and how wide it is. */
typedef struct {
    char name[OOPS_GL_MAX_GLSL_NAME];
    GLenum type;
    int offset; /* into the fragment's float block */
    int floats;
} gl_varying_t;

typedef struct {
    GLuint name; /* 0 when the slot is free */
    GLenum type; /* GL_VERTEX_SHADER or GL_FRAGMENT_SHADER */
    GLboolean compiled;
    /* `glDeleteShader` was called while something still referred to this object. It
     * stops being a shader to `glIsShader` at that moment and keeps working until the
     * last program detaches it - which is the observable half of deferred deletion. */
    GLboolean flagged;
    char *source; /* the glShaderSource strings joined, NUL-terminated; NULL if never
                     given */
    size_t source_len; /* without the terminator, which is what GL_SHADER_SOURCE_LENGTH
                          is +1 of */
    glsl_unit_t
        *unit; /* the compiled tree, a reference held; NULL until a compile succeeds */
    char info_log[OOPS_GL_INFO_LOG_SIZE];
} gl_shader_object_t;

typedef struct {
    GLuint name;
    GLboolean linked;
    GLboolean flagged;
    GLboolean validated; /* glValidateProgram's last answer */
    GLboolean valid_run; /* whether glValidateProgram has ever been called on it */
    GLuint attached[OOPS_GL_MAX_ATTACHED_SHADERS];
    int attached_count;

    /* The two stages the last successful link took references to. They survive their
     * shader objects being detached and deleted, which is why they are counted. */
    glsl_unit_t *vs;
    glsl_unit_t *fs;

    gl_uniform_t uniforms[OOPS_GL_MAX_PROGRAM_UNIFORMS];
    int uniform_count;
    /* Which sampler uniform each of the compiled shader's descriptor sets belongs to,
     * as an index into `uniforms`, and how many sets it uses. Decided once at link
     * time: the shader loads set `n` from a fixed offset in the block and the draw
     * fills that offset, so the two have to mean the same thing by `n`. */
    int hw_tex_uniform[OOPS_GL_GL2_TEX_SETS];
    int hw_tex_sets;
    /* The first parameter `gl_TexCoord[]` occupies, or -1 when the fragment shader
     * never names it. One parameter an element, `OOPS_GL_MAX_TEXTURE_UNITS` of them,
     * laid out consecutively from here - decided at link time for the same reason
     * `hw_color_param` is, since the compiler reads it to emit the interpolation and
     * the draw reads it to fill the slot, and the two must not each work it out. */
    int hw_texcoord_param;
    float *values; /* the value pool the uniforms' offsets index */
    int value_floats;

    /* Attributes the link found in the vertex shader, and the bindings requested for
     * the next one - see glBindAttribLocation's note in GL/gl.h. */
    gl_attrib_binding_t attribs[OOPS_GL_MAX_VERTEX_ATTRIBS];
    int attrib_count;
    gl_attrib_binding_t bindings[OOPS_GL_MAX_VERTEX_ATTRIBS];
    int binding_count;

    gl_varying_t varyings[OOPS_GL_MAX_PROGRAM_VARYINGS];
    int varying_count;
    int varying_floats;

    /* The console's half, filled at link time by `gl_program_compile_fragment`.
     *
     * A compiled pixel shader, or nothing. `hw_ps_words` of zero is not a link failure:
     * a program with no fragment stage has nothing to compile, and a program the back
     * end will not generate for still links and draws on the software path, but not on
     * a console; `hw_ps_log` is what the draw path says once when it refuses.
     *
     * Kept per program rather than compiled on demand, because compiling needs a symbol
     * table and a generator - a quarter of a megabyte of scratch. */
    uint32_t *hw_ps;
    uint32_t hw_ps_words;
    uint32_t hw_ps_vgprs;
    /* How many user SGPRs the compiled shader was built to be handed: two when it is
     * given the block's address in s[0:1], none when it needs no block. The draw path
     * configures `SPI_SHADER_PGM_RSRC2_PS` from this rather than deciding a second
     * time, because the count also fixes where the SPI puts the primitive mask, which
     * the shader moves into `m0`. */
    uint32_t hw_ps_user_sgprs;
    /* Which compiled shader this is, as an identity that is never handed out twice.
     *
     * A program's name cannot answer that: `glDeleteProgram` frees it and the next
     * `glCreateProgram` returns the same number. The draw path uploads a compiled
     * shader into the payload's one GL 2.0 slot only when the slot holds something
     * else, so asking by name would run a deleted program's shader against a new
     * program's draw.
     *
     * Issued from `hw_ps_next_serial` at each successful compile and never reused. Zero
     * means this program has no compiled shader. */
    uint64_t hw_ps_serial;
    /* What the pixel stage has to be handed for this shader to run, as
     * `SPI_PS_INPUT_ENA` and `_ADDR` - the barycentrics always, and the fragment's
     * window position when the shader reads `gl_FragCoord`. Decided by the compiler
     * because only it knows what the shader names, and written by the draw, which is
     * the same division as `hw_ps_user_sgprs`: a shader reading a register the SPI was
     * not told to supply reads whatever was in it. */
    uint32_t hw_ps_input_ena;
    /* Which parameter carries `gl_Color`, or -1 for a shader that does not read it.
     *
     * The fixed-function colour is not a user varying, so where it lands depends on
     * whether there is a vertex shader. With one, the user's varyings own the
     * parameters from 0 and this is the first free slot after them. Without one, the
     * fixed-function vertex path writes the colour into parameter 0.
     *
     * Decided at link because the draw sizes the vertex from it and the compiler
     * interpolates from it, and the two must agree. */
    int hw_color_param;
    /* Whether the compiled shader exports a depth of its own, which changes two
     * registers the draw writes: `SPI_SHADER_Z_FORMAT` has to say a Z is coming, and
     * `DB_SHADER_CONTROL` has to stop testing early - a depth the shader computes is
     * not known until the shader has run, and early Z would have tested the
     * interpolated one instead. */
    GLboolean hw_ps_exports_depth;
    /* Whether the compiled shader can kill a fragment, which the depth block has to be
     * told in `DB_SHADER_CONTROL.KILL_ENABLE` - clearing `exec` does not tell it.
     *
     * With early Z and the bit clear, the block tests, writes and retires a pixel
     * before the shader runs; the export's mask arrives too late, so a discarded
     * fragment keeps its depth and rejects the draws behind it.
     *
     * radeonsi sets this bit from `uses_discard` and leaves `Z_ORDER` at
     * `EARLY_Z_THEN_LATE_Z` (`si_state_shaders.cpp:1711` and `:1758`) - so the kill is
     * this one bit, not a switch to late Z. */
    GLboolean hw_ps_kills;
    /* Whether the fragment stage reads `gl_PointCoord`. The point expansion generates
     * the sprite coordinate for a unit whose `GL_COORD_REPLACE` is set, and for texture
     * coordinate 0 as well when this is true - because `gl_PointCoord` is that
     * interpolant and a GLSL program has no `GL_COORD_REPLACE` to set. The link refuses
     * a fragment shader that reads both this and `gl_TexCoord`, so the slot has one
     * meaning. */
    GLboolean hw_reads_point_coord;
    /* Whether this program's refusal has been logged. Cleared at every link,
     * so a relinked program that is still refused says so again - the source may have
     * changed and the reason with it. On the program rather than the context because
     * program names are recycled; see the note by `hw_ps_serial`. */
    GLboolean hw_ps_logged;
    /* How many four-component parameters this program's varyings occupy, which is what
     * the vertex stage exports and the pixel shader interpolates. Two at minimum,
     * because the pipeline's smallest configuration exports two. */
    uint32_t hw_params;
    char hw_ps_log[OOPS_GL_INFO_LOG_SIZE];

    /* The compiled vertex shader for this program, or NULL. */
    uint32_t *hw_vs;
    uint32_t hw_vs_words;
    uint32_t hw_vs_vgprs;
    uint32_t hw_vs_user_sgprs;
    uint64_t hw_vs_serial;
    GLboolean hw_vs_logged;
    char hw_vs_log[OOPS_GL_INFO_LOG_SIZE];

    char info_log[OOPS_GL_INFO_LOG_SIZE];
} gl_program_object_t;

/* One generic vertex attribute array, and the current value used when it is disabled.
 *
 * The same shape as `gl_client_array_t` with `normalized` added, rather than that
 * structure reused: a generic array normalises on the caller's word and a named one
 * normalises by its type's own rule, and folding the two would make
 * `glVertexAttribPointer(.., GL_FLOAT, GL_TRUE, ..)` mean something. */
typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    const void *pointer;
    GLuint
        buffer; /* GL_ARRAY_BUFFER at the time it was specified, 0 for client memory */
    GLboolean enabled;
    GLboolean normalized;
    float current[4]; /* glVertexAttrib's value; (0, 0, 0, 1) at first */
} gl_vertex_attrib_t;

/* One frame of the client attribute stack. Separate from gl_attrib_entry_t because
 * the specification keeps the two stacks separate: a push of client state must not pop
 * server state, and a program that brackets a helper with both is relying on that. */
typedef struct {
    GLbitfield mask;

    /* GL_CLIENT_VERTEX_ARRAY_BIT */
    gl_client_array_t array_vertex;
    gl_client_array_t array_color;
    gl_client_array_t array_normal;
    gl_client_array_t array_texcoord[OOPS_GL_MAX_TEXTURE_UNITS];
    GLuint client_active_texture; /* the vertex-array group's (GL 1.3, table 6.6) */
    gl_client_array_t array_edge_flag;
    gl_client_array_t array_index;
    gl_client_array_t array_secondary;
    gl_client_array_t array_fog_coord;

    /* GL_CLIENT_PIXEL_STORE_BIT */
    GLint unpack_alignment;
    GLint unpack_row_length;
    GLint pack_alignment;
    GLint unpack_image_height, pack_image_height;
    GLint unpack_skip_rows, unpack_skip_pixels, unpack_skip_images;
    GLboolean unpack_swap_bytes, unpack_lsb_first;
    GLint pack_row_length, pack_skip_rows, pack_skip_pixels, pack_skip_images;
    GLboolean pack_swap_bytes, pack_lsb_first;
} gl_client_attrib_entry_t;

typedef struct gl_context {
    struct oops_display *disp;
    /* The colour buffers, and which of them a draw writes (gl_draw_targets).
     * `framebuffer` is where every write goes: the back, or the front after
     * glDrawBuffer(GL_FRONT). `fb_also` is the second buffer that GL_FRONT_AND_BACK and
     * GL_LEFT add, or NULL. The back is the display's framebuffer. The front is
     * oops-gl's own surface, allocated the first time a program names it
     * (gl_front_buffer) and put on screen by glFlush and glFinish while it has been
     * drawn into (`front_pending`). */
    uint32_t *framebuffer;
    uint32_t *fb_also;
    uint32_t *back_fb;
    uint32_t *front_fb;
    GLboolean front_pending;
    /* The scanout path (gl_rx.h): the colour buffers are the display's scanout
     * buffers, in the GPU's 64KB_R_X swizzle, drawn in place and flipped as drawn - the
     * back the next one, the front the one on screen - rather than a linear buffer the
     * display re-tiles. `color_tiled` is what the CPU's colour addressing follows
     * (gl_color_index). Both are set by gl_scanout_begin when OOPS_GL_RX_MEASURED. */
    GLboolean hw_rx;
    GLboolean color_tiled;
    /*
     * The span of a colour buffer the CPU has written and not yet drained, as word
     * indices into `cpu_color_buf`; `cpu_color_lo >= cpu_color_hi` means there is
     * nothing outstanding.
     *
     * On the scanout path the colour buffer is the display's own memory, which the CPU
     * maps write-combined. WC writes are weakly ordered on x86 and a later read may be
     * served before them, so the CPU, or the CP's DMA copy into `readback`, could read
     * what was there before.
     *
     * gl_color_cpu_drain drains the span. See gl_rx.h.
     */
    size_t cpu_color_lo;
    size_t cpu_color_hi;
    uint32_t *cpu_color_buf;
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

    /* The texture units (GL 1.3), and which one glActiveTexture selected - an index, 0
     * for GL_TEXTURE0. State calls act on `tex_unit[active_texture]` (gl_tu); a draw
     * reads each unit. */
    /* Sized by the image units, because a sampler may name any of them and a
     * binding has to exist there for it to sample. The fixed-function pipeline still
     * combines only the first `OOPS_GL_MAX_TEXTURE_UNITS` of them - that limit is the
     * descriptor ring's, and the units past it are binding points a shader reaches
     * rather than stages that combine. */
    gl_tex_unit_t tex_unit[OOPS_GL_MAX_TEXTURE_IMAGE_UNITS];
    GLuint active_texture;

    /* State settings */
    GLenum depth_func;
    GLboolean depth_mask;
    GLenum blend_src;
    GLenum blend_dst;
    GLenum blend_src_alpha;
    GLenum blend_dst_alpha;
    GLenum blend_equation;
    /* GL 2.0's second equation, for the alpha channel. `glBlendEquation` sets both. */
    GLenum blend_equation_alpha;
    /* glBlendColor, clamped on the way in (Mesa main/blend.c:788-791). The unclamped
     * copy Mesa also keeps only matters with a floating-point colour buffer, and this
     * one is 8-bit fixed point, so it is the clamped value glGetFloatv answers with
     * (get.c:1173-1177). */
    float blend_color[4];
    /* glEnable(GL_COLOR_LOGIC_OP) and glLogicOp. When enabled it replaces blending
     * outright rather than following it - Mesa's state tracker checks it first and
     * leaves every blend target disabled (state_tracker/st_atom_blend.c:269-273). */
    GLboolean cap_color_logic_op;
    GLenum logic_op;
    /* Nonzero while a copy uploads a level a row at a time: GL_GENERATE_MIPMAP waits
     * for the last row rather than rebuilding the levels once per row. */
    int gen_mipmap_suspend;
    /* glHint, one per target (GL_HINT_BIT). Recorded and reported; none changes
     * anything - the rasteriser interpolates perspective-correctly either way, and the
     * others are preferences an implementation may ignore. */
    GLenum perspective_hint;
    GLenum hint_point_smooth, hint_line_smooth, hint_polygon_smooth, hint_fog;
    GLenum hint_texture_compression;
    GLenum hint_generate_mipmap; /* GL 1.4's */

    /* Colour-index state, kept and reported and never drawn with: this is an RGBA
     * context, and in one the specification keeps all of it as state only.
     * `index_logic_op` is GL_INDEX_LOGIC_OP's enable, stored as Mesa stores it
     * (main/enable.c:672-678). */
    float cur_index;
    float clear_index;
    GLuint index_mask;
    GLboolean cap_index_logic_op;
    gl_client_array_t array_index;

    /* Multisampling, with no multisample buffer: state only (GL_MULTISAMPLE_BIT). */
    GLboolean cap_multisample, cap_sample_alpha_to_coverage, cap_sample_alpha_to_one;
    GLboolean cap_sample_coverage;
    float sample_coverage_value;
    GLboolean sample_coverage_invert;

    /* Evaluators (gl_eval.c): the eighteen maps, their enables, GL_AUTO_NORMAL, and the
     * grid glMapGrid sets for glEvalMesh and glEvalPoint. */
    gl_eval_map_t map1[OOPS_GL_EVAL_MAPS], map2[OOPS_GL_EVAL_MAPS];
    GLboolean cap_map1[OOPS_GL_EVAL_MAPS], cap_map2[OOPS_GL_EVAL_MAPS];
    GLboolean cap_auto_normal;
    GLint grid1_un, grid2_un, grid2_vn;
    float grid1_u1, grid1_u2, grid2_u1, grid2_u2, grid2_v1, grid2_v2;

    /* Selection and feedback (gl_select.c). `render_mode` is GL_RENDER, GL_SELECT or
     * GL_FEEDBACK; in the other two, primitive assembly hands each primitive to gl_fb_*
     * instead of the rasteriser. The counts run past the buffer's size on overflow -
     * the writes stop, the count does not - which is how glRenderMode knows to answer
     * -1. `fb_line_reset` marks the next line as starting a new stipple pattern:
     * reported as GL_LINE_RESET_TOKEN rather than GL_LINE_TOKEN in feedback, and drawn
     * from the pattern's first bit under GL_LINE_STIPPLE. Whichever of the two reads it
     * clears it. */
    GLenum render_mode;
    GLuint *select_buffer;
    GLsizei select_size;
    GLuint select_count;
    GLuint select_hits;
    GLboolean select_buffer_set;
    GLboolean hit_flag;
    float hit_min_z, hit_max_z;
    GLuint name_stack[OOPS_GL_MAX_NAME_STACK_DEPTH];
    GLuint name_depth;
    GLfloat *feedback_buffer;
    GLsizei feedback_size;
    GLuint feedback_count;
    GLenum feedback_type;
    GLboolean feedback_buffer_set;
    GLboolean fb_line_reset;

    /* Pixel transfer (gl_raster.c): scale and bias per colour component, red to alpha,
     * the colour and index maps, and the state that acts on index, stencil and depth
     * pixels - kept, since those formats are refused. `pixel_transfer_suspend` is set
     * while a texture copy uploads what glReadPixels already transferred, so the copy
     * is transferred once, not twice. */
    float pixel_scale[4], pixel_bias[4];
    float depth_scale, depth_bias;
    GLint index_shift, index_offset;
    GLboolean map_color, map_stencil;
    GLboolean pixel_transfer_suspend;
    float pixel_map[OOPS_GL_PIXEL_MAPS][OOPS_GL_MAX_PIXEL_MAP_TABLE];
    GLint pixel_map_size[OOPS_GL_PIXEL_MAPS];

    /* Stipple. `line_stipple_counter` is GL's s: the pixels drawn since the pattern
     * last started, carried across a strip's segments and reset where `fb_line_reset`
     * says a new line begins. A polygon stipple row is one window row, bit 31 its
     * column 0. */
    GLboolean cap_line_stipple;
    GLint line_stipple_factor;
    GLushort line_stipple_pattern;
    GLint line_stipple_counter;
    GLboolean cap_polygon_stipple;
    uint32_t polygon_stipple[32];

    GLboolean cap_point_smooth, cap_line_smooth, cap_polygon_smooth;
    /* GL_POINT_SPRITE, and which corner its generated t starts from. The origin is
     * GL_UPPER_LEFT by default, which is the specification's default and the opposite
     * of the window's y. GL_LOWER_LEFT is the other. */
    GLboolean cap_point_sprite;
    GLenum point_sprite_origin;
    /* Antialiasing (GL_POINT_SMOOTH, GL_LINE_SMOOTH, GL_POLYGON_SMOOTH). The
     * triangles a smooth point or line becomes carry, for the rasteriser, the shape
     * they stand for in screen pixels: a point's centre and radius, or a line's ends
     * and half-width - `aa_ends` false for a stipple's dash, whose ends are not the
     * line's. `aa_edges` is the polygon triangle's boundary edges
     * (gl_draw_polygon_tri's bits), the only ones a smooth polygon's coverage fades
     * across. The hardware draws all three aliased, and says so once. */
    GLenum aa_kind; /* GL_POINT, GL_LINE, or 0 */
    float aa_c[2], aa_r;
    float aa_a[2], aa_b[2], aa_hw;
    GLboolean aa_ends;
    unsigned aa_edges;
    GLboolean hw_smooth_logged;

    /* The accumulation buffer (gl_raster.c): RGBA as signed 16-bit fractions of one, a
     * pixel to four, rows bottom-up in GL's window order. NULL until first used - at
     * 1080p it is 16 MB, which a program that never accumulates should not pay for. */
    int16_t *accum_buffer;
    float accum_clear[4];

    /* GL_DITHER: a flag with no effect, on by default. */
    GLboolean cap_dither;
    GLenum cull_mode;   /* GL_BACK, GL_FRONT, etc. */
    GLenum front_face;  /* GL_CCW, GL_CW */
    GLenum shade_model; /* GL_SMOOTH, GL_FLAT */
    GLboolean color_mask[4];
    /* glDrawBuffer's and glReadBuffer's choices: GL_BACK (or GL_BACK_LEFT, the same
     * buffer on this mono visual), and for drawing GL_NONE. */
    GLenum draw_buffer, read_buffer;

    /* Matrix stacks */
    GLenum matrix_mode;
    gl_mat4_t modelview_stack[OOPS_GL_MODELVIEW_STACK_CAPACITY];
    int modelview_depth;
    gl_mat4_t projection_stack[OOPS_GL_PROJECTION_STACK_CAPACITY];
    int projection_depth;
    /* The texture matrix stacks are per unit - gl_tex_unit_t. */

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
    GLenum
        light_model_color_control; /* GL_SINGLE_COLOR or GL_SEPARATE_SPECULAR_COLOR */
    GLboolean light_model_two_side;
    GLboolean cap_rescale_normal;

    /* Client arrays */
    gl_client_array_t array_vertex;
    gl_client_array_t array_color;
    gl_client_array_t array_normal;
    /* One texture coordinate array per unit, and which one glClientActiveTexture
     * selected for glTexCoordPointer, GL_TEXTURE_COORD_ARRAY and their queries
     * (GL 1.3, 2.8). */
    gl_client_array_t array_texcoord[OOPS_GL_MAX_TEXTURE_UNITS];
    GLuint client_active_texture;
    gl_client_array_t
        array_edge_flag; /* GLboolean elements; `size` and `type` unused */
    gl_client_array_t
        array_secondary; /* GL 1.4; three components read, whatever the size */
    gl_client_array_t
        array_fog_coord; /* GL 1.4; one GL_FLOAT or GL_DOUBLE per element */

    /* Current attributes for immediate mode */
    float cur_color[4];
    float cur_normal[3];
    /* Each unit's s, t, r, q - glTexCoord sets unit 0's, glMultiTexCoord any unit's.
     * See the attribute-stack copy of this field. */
    float cur_texcoord[OOPS_GL_MAX_TEXTURE_UNITS][4];
    GLboolean cur_edge_flag;
    /* glSecondaryColor's (GL 1.4), (0, 0, 0, 1) at first; alpha is never set and never
     * read. */
    float cur_secondary[4];
    /* GL_COLOR_SUM: the secondary colour added to the primary after texturing. Lighting
     * applies the sum whatever this says - see gl_color_sum_on. */
    GLboolean cap_color_sum;
    /* glFogCoord's (GL 1.4), 0 at first, and GL_FOG_COORD_SRC: GL_FRAGMENT_DEPTH (the
     * eye distance, the default) or GL_FOG_COORD (this value, per vertex). */
    float cur_fog_coord;
    GLenum fog_coord_src;
    /* GL_FOG_INDEX: colour-index fog, kept and reported, never drawn with. */
    float fog_index;

    /* Immediate mode buffer */
    GLenum imm_mode;
    GLboolean imm_active;
    gl_vertex_t imm_verts[OOPS_GL_MAX_IMMEDIATE_VERTS];
    int imm_count;

    /* Texture Management. The bindings and enables are per unit - gl_tex_unit_t. */
    GLboolean
        hw_3d_logged; /* the one log line a mipmapped volume earns on the hardware */
    GLboolean hw_cube_logged; /* and a cube-mapped one */
    GLboolean
        hw_env_logged; /* and one for an environment the pixel shader cannot combine */
    GLboolean hw_unit_logged; /* and one for a texture unit above 0, which the console
                                 leaves out */
    /* And one for a compiled shadow lookup whose texture is not `GL_LUMINANCE`: the
     * spread a comparison's single value takes is that texture's choice, made after the
     * shader was compiled, and the compiled path emits the default. */
    GLboolean hw_depth_mode_logged;
    /* A census of the frame's draws, by whether one sampled a texture at all: the
     * proportion tells a HUD from an untextured world. Counted per submit and reported
     * by the same gate as the submit's other values. */
    uint32_t hw_draws_textured;
    uint32_t hw_draws_untextured;
    /*
     * Which textures a frame drew with, and how many draws each got: when a surface
     * comes out wrong, which texture a draw reached for.
     *
     * Counted here and printed once a flip rather than traced per draw, because a log
     * line per draw slows a busy frame by orders of magnitude. A draw costs a scan of
     * at most `OOPS_GL_TEX_CENSUS` entries and an increment; a frame costs one line per
     * distinct texture. `id` 0 is the untextured draws, and `over` counts draws whose
     * texture found no free entry.
     */
    struct {
        uint32_t id; /* the texture name, 0 for an untextured draw */
        /* The second unit's texture. A row is keyed on the pair because `id` is the
         * base unit's: a program that keeps one shadow texture on unit 0 and the
         * per-surface material on unit 1 would otherwise collapse every draw into one
         * row. */
        uint32_t id1;
        uint32_t draws; /* how many draws sampled it this frame */
        uint16_t w,
            h; /* its dimensions, so a mosaic of two sizes is visible at a glance */
        uint8_t slot;  /* the descriptor slot it last went into */
        uint8_t unit;  /* the unit that sampled it - often 1, not 0 */
        uint8_t env;   /* GL_REPLACE/MODULATE/COMBINE, low byte of the enum */
        uint8_t blend; /* whether the draw blended */
        /* The base format, because the environment's answer depends on it: a combine
         * reading `GL_ONE_MINUS_SRC_ALPHA` from a texture with no alpha channel gets 1,
         * and `previous x (1 - 1)` is black. */
        GLenum base;
        /* The descriptor the sampler was handed: everything above says what the library
         * meant, these eight words what the hardware was told. Width and height are
         * split across words 1 and 2, and word 4 carries the row pitch, which is read
         * only when the pitch exceeds the width. */
        uint32_t desc[8];
        uint32_t samp[4]; /* the sampler half: wrap, filter, LOD clamps */
        uint32_t pitch;   /* what the library believes the row pitch is, in pixels */
        uint32_t garlic;  /* the base level storage >> 8; differs from desc[0] when the
                           * descriptor points at the mip chain instead */
    } hw_tex_census[OOPS_GL_TEX_CENSUS];
    uint32_t hw_tex_census_n;    /* entries used */
    uint32_t hw_tex_census_over; /* draws that found no entry */
    /*
     * Whether one descriptor slot served two textures inside one submit.
     *
     * Every textured draw hands the shader `payload_va + slot_offset(hw_desc_slot)` and
     * the GPU reads that slot when it executes the draw, after the CPU has moved on. So
     * a slot may hold one texture's descriptor for the whole of a submit and no more:
     * otherwise every draw in that submit pointing there samples whichever was written
     * last. The ring is built to prevent it - a draw takes the next slot whenever its
     * descriptor differs, and the frame is submitted when the ring is full - and this
     * checks that it does.
     *
     * `slot_tex[s]` is the texture the slot was given since the last flush, 0 for
     * untouched; `slot_tex1[s]` is the second unit's half, since a slot holds two
     * descriptors. A collision records the slot and both textures.
     */
    uint32_t hw_slot_tex[OOPS_GL_DESC_RING_SLOTS + 1u];
    uint32_t hw_slot_tex1[OOPS_GL_DESC_RING_SLOTS + 1u];
    uint32_t
        hw_slot_collisions; /* how many times a slot was re-pointed within one submit */
    /* Draws that ran the last shader slot's variant instead of their own. The pixel
     * shader ring has OOPS_GL_PS_RING_SLOTS slots; a draw that finds none free reuses
     * the last rather than overwrite a slot a queued draw is reading. Per flip, cleared
     * with the rest. */
    uint32_t hw_ps_ring_stale;
    /* How far the descriptor slot index travels in a submit, and how often the ring
     * filled. The ring has `OOPS_GL_DESC_RING_SLOTS` slots and flushes when a draw
     * needs a new one and none is free. A high-water mark at the last slot says the
     * ring runs at its limit, and `ring-full` how many times a frame it submitted to
     * get room. Per flip, cleared with the rest. */
    uint32_t hw_desc_slot_high;
    uint32_t hw_desc_ring_full;
    uint32_t hw_slot_first_slot; /* and the first one, with the two textures that shared
                                    it */
    uint32_t hw_slot_first_had;
    uint32_t hw_slot_first_got;
    /* Texture names handed out, names refused, and errors raised - cumulative, not per
     * submit. `glGenTextures` has a fixed pool here (`OOPS_GL_MAX_TEXTURE_OBJECTS`)
     * where a desktop GL has 2^32 names, and a program that does not check gets name 0,
     * uploads to the default texture and samples whatever was there. Read these before
     * the draw census. */
    uint32_t hw_tex_created;
    uint32_t hw_tex_failed;
    uint32_t hw_gl_errors;
    /* What the census last printed for the three above, so it prints them only when
     * they have moved. The gate they sit behind opens on every submit while frames are
     * not confirming, when the title's own output matters most. */
    uint32_t hw_tex_created_said;
    uint32_t hw_tex_failed_said;
    uint32_t hw_gl_errors_said;
    /* The most recent code raised, which `last_error` cannot give: the first error
     * stands until `glGetError` clears it, and a program that never calls it leaves
     * `last_error` on the first one. With the count, this separates one repeating fault
     * from a scatter. */
    uint32_t hw_gl_error_last;
    /* Submits since the last flip, counted per site (`hw_flush_site`): a submit is a
     * GPU round-trip, and the per-site breakdown says where the frame's cost comes
     * from.
     *
     * Keyed on the `__func__` pointer, not on the string: every call site in a
     * translation unit hands over the same literal, so a pointer compare is correct and
     * cheap. A site beyond the table is counted in `hw_flushes` and reported unnamed.
     */
    /* A capture is running: every hookable entry point is written to a byte stream
     * that can be replayed elsewhere - on the host's reference rasteriser, which is the
     * point. See `oops_gl_capture_begin` in GL/gl.h. */
    /* The textured shader's variant ring - see OOPS_GL_PS_RING_OFFSET. `key` is a hash
     * of the master's words, `live` says the slot is referenced by a draw already in
     * this batch and so cannot be rewritten, and `dirty` says the master has been
     * patched since it was hashed. */
    uint32_t ps_ring_key[OOPS_GL_PS_RING_SLOTS];
    GLboolean ps_ring_live[OOPS_GL_PS_RING_SLOTS];
    uint32_t ps_ring_next;
    uint32_t ps_master_key;
    GLboolean ps_master_dirty;
    GLboolean capture_active;
    uint32_t hw_flushes;
    /* Nanoseconds spent inside a submit, per frame: subtracted from the frame time,
     * what is left is this library's own CPU work. Measured at the one place submits
     * happen.
     */
    uint64_t hw_flush_ns;
    /* And the nanoseconds inside the draw path, with the triangles that spent them - so
     * the CPU's share of a frame can be split between this library and the program
     * above it. */
    uint64_t hw_draw_ns;
    uint32_t hw_draw_calls;
    /* And, inside that, the per-draw shader patching - the block that rebuilds its
     * words and compares them against the payload on every draw whether or not anything
     * changed. */
    uint64_t hw_patch_ns;
    /* And the command words themselves, so the rest of a draw can be attributed to the
     * texture and descriptor preparation that sits between the two. */
    uint64_t hw_dcb_ns;
    /*
     * The remaining phases of the draw path, so a frame's time is attributed by
     * measurement rather than by assumption.
     *
     * `hw_tnl_ns` is the software transform and lighting: every vertex through the
     * modelview and projection, the lights, texture generation, the divide, the
     * viewport and the cull. It runs per triangle, so a vertex shared by six of them
     * is transformed six times.
     *
     * `hw_vbo_ns` is the descriptor slot decision and writing the three vertices into
     * the ring.
     *
     * What neither covers, between the hardware branch and the patch block, is the
     * state evaluation: blend, depth, cull and target masks recomputed and compared per
     * draw.
     */
    uint64_t hw_tnl_ns;
    uint64_t hw_vbo_ns;
    /*
     * What the current descriptor slot holds, in cached memory.
     *
     * The slot decision - does this draw's descriptor differ from the one already in
     * the slot? - is answered from this shadow, not from `gpu_payload`: that is ONION
     * and every write to a slot is followed by a `clflush`, so reading it back is a
     * miss to the slowest bus on the part per triangle. `valid` says which halves of
     * the shadow describe the slot: bit 0 for unit 0's descriptor and sampler, bit 1
     * for unit 1's. A slot change clears both, because the new slot's contents were
     * written by some earlier frame and are not this shadow's; each half is set again
     * when it is written.
     *
     * Laid out as the slot is - unit 0's image at word 0, its sampler at word 8, unit
     * 1's at word 16 and 24 - so a comparison is against the same 48 bytes the copy
     * writes, in the same order, and the two cannot drift into describing different
     * things.
     */
    uint32_t hw_desc_shadow[32];
    uint32_t hw_desc_shadow_valid;
    /*
     * And the same for the two pixel shaders, for the same reason.
     *
     * The patch slots in the two master shaders (GL_PATCH_SLOT_*) each decide per draw
     * whether they have anything to do by comparing against the words already there,
     * and `gl_ps_flush_shaders` evicts whatever it writes, so reading the payload would
     * miss to ONION every time. The words are usually already right, so the comparison
     * is the cost.
     *
     * Mirrors of the two regions exactly, seeded from the payload once the masters are
     * built - so the first comparison of a slot is against what is really there rather
     * than against zeroes, which would answer "same" for any slot whose words are zero.
     */
    uint32_t hw_ps_untex_shadow[128]; /* OOPS_GL_PS_UNTEX_WORDS */
    uint32_t hw_ps_tex_shadow[320];   /* OOPS_GL_PS_TEX_WORDS */
    /*
     * How often each patch slot changes, and how much cache eviction that costs. A
     * write calls `gl_ps_flush_shaders`, which evicts all three shader ranges whichever
     * slot moved.
     *
     * A count per slot says which one is moving: a slot that changes a few times a
     * frame is a material switch, one that changes per draw is an answer that
     * alternates per draw. Counted rather than timed, because a clock read per call
     * would cost more than the call.
     */
    uint32_t hw_patch_writes[GL_PATCH_SLOT_COUNT];
    uint32_t hw_ps_flush_calls;
    const char *hw_flush_site[OOPS_GL_FLUSH_SITES];
    uint32_t hw_flush_site_n[OOPS_GL_FLUSH_SITES];
    uint32_t hw_flush_unnamed;
    /* The entry point that raised it, from `__func__` at the call site. A pointer to a
     * string literal in the payload, so it outlives every frame and costs nothing to
     * keep. */
    const char *hw_gl_error_fn;
    /* The enum or value the latest refusal was about, where its site passed one. */
    uint32_t hw_gl_error_val;
    GLboolean hw_gl_error_has_val;
    /* The GL 2.0 refusal's log-once flag lives on the program object - `hw_ps_logged` -
     * and not here, because program names are recycled: a context-side marker keyed on
     * the name would swallow a later program's reason for being refused. */
    /* Which compiled pixel shader is in the payload's one GL 2.0 slot, by the serial
     * issued below, 0 for none. A frame that draws with one program uploads it once;
     * one that alternates pays an upload and a cache flush per switch.
     *
     * By serial and not by name - see `hw_ps_serial` on the program. */
    uint64_t hw_ps_resident;
    /* Which export tail the resident compiled shader was uploaded with. One program
     * drawn into one colour buffer and then into two is the same serial and needs
     * different words, so the serial alone cannot say whether the slot is current. */
    GLboolean hw_ps_resident_both;
    /* The next serial to issue, so no two compiled shaders are ever confused for each
     * other. Starts at 1; 0 is "no shader". */
    uint64_t hw_ps_next_serial;
    uint64_t hw_vs_resident;
    uint64_t hw_vs_next_serial;
    /* What `SPI_PS_INPUT_ENA` and `_ADDR` currently hold, so a draw emits them only
     * when it wants something else. Set by `gl_hw_begin_frame` to whatever its table
     * wrote. */
    uint32_t hw_input_ena;
    /* What `SPI_SHADER_Z_FORMAT` currently holds - 0 for no depth export, 1 for one. */
    uint32_t hw_z_format;
    /* And what `DB_SHADER_CONTROL` currently holds, cached separately: a program that
     * discards without writing depth changes `KILL_ENABLE` here and not the Z format.
     */
    uint32_t hw_db_shader_control;
    /* The depth and stencil surfaces are the GPU's, 64KB_Z_X tiled (see
     * gl_zs_depth_ptr): true once the hardware path is up on the console, never on a
     * host build. */
    GLboolean zs_tiled;
    gl_texture_object_t textures[OOPS_GL_MAX_TEXTURE_OBJECTS];
    /* Framebuffer objects, and which one a draw goes to. `bound_framebuffer` of 0 is
     * the window-system framebuffer - the display; a non-zero name redirects the colour
     * target in `gl_draw_targets`. */
    gl_framebuffer_object_t framebuffers[OOPS_GL_MAX_FRAMEBUFFER_OBJECTS];
    gl_renderbuffer_object_t renderbuffers[OOPS_GL_MAX_RENDERBUFFER_OBJECTS];
    GLuint bound_framebuffer;
    /*
     * The read binding; `bound_framebuffer` is the draw binding.
     *
     * GL 3.0 split one binding into two: draws go to `GL_DRAW_FRAMEBUFFER` and reads
     * come from `GL_READ_FRAMEBUFFER`, and `GL_FRAMEBUFFER` sets both. It exists for
     * `glBlitFramebuffer`, the only entry point that reads one framebuffer while
     * writing another; a program that binds only `GL_FRAMEBUFFER` moves both together.
     */
    GLuint bound_read_framebuffer;
    GLuint bound_renderbuffer;
    /* Said-once flags for the two halves of `glBlitFramebuffer` that are not applied -
     * the depth/stencil bits and a scaled `GL_LINEAR`. Reported rather than silently
     * skipped, and reported once rather than per blit, because a blit is a per-frame
     * call. */
    GLboolean hw_blit_ds_logged;
    GLboolean hw_blit_linear_logged;
    /* The window-system framebuffer's size, kept because `width` and `height` stop
     * being it. Everything addresses the colour buffer through `gl_color_index`,
     * which reads those two, so binding a framebuffer object of a different size means
     * changing them - and changing them means remembering what they were. Set once
     * beside them at context creation. */
    uint32_t fb0_width;
    uint32_t fb0_height;
    /* The display's depth buffer, parked for the same reason. A framebuffer object with
     * no depth attachment leaves `depth_buffer` NULL, which the draw path already reads
     * as "no depth test here" - so an attachment-less depth needs no special case. */
    float *fb0_depth_buffer;
    /* And the display's tiling, which an attachment does not share. On the console
     * the scanout buffers are in a 64KB_R_X swizzle and `hw_rx`/`color_tiled` say so;
     * an attachment is linear whatever the display is. Both are parked here and cleared
     * while a framebuffer object is bound, because `gl_color_index` addresses with one
     * and `gl_hw_begin_frame` programs `CB_COLOR0_ATTRIB3` from the other - so leaving
     * either set would have the CPU and the command processor read a linear attachment
     * as a tiled one. */
    GLboolean fb0_hw_rx;
    GLboolean fb0_color_tiled;
    /* The proxy targets' levels - 1D, 2D, 3D, cube map: sizes and formats only, never
     * pixels. A level that would not have fitted is all zeros, which is how a proxy
     * says no. */
    gl_tex_level_t proxy[4][OOPS_GL_MAX_TEXTURE_LEVELS];

    /* Buffer objects. Two binding points, because GL_ARRAY_BUFFER and
     * GL_ELEMENT_ARRAY_BUFFER are independent - a program binds one of each and draws
     * from both at once. */
    gl_buffer_object_t buffers[OOPS_GL_MAX_BUFFER_OBJECTS];
    /* GL 1.5's occlusion queries: the objects, the one active on GL_SAMPLES_PASSED (0
     * for none), and the samples counted since it began - bumped by gl_fragment_tail
     * for every fragment that passes the depth test. */
    gl_query_object_t queries[OOPS_GL_MAX_QUERY_OBJECTS];
    GLuint query_active;
    uint64_t query_samples;
    GLboolean hw_query_logged;
    GLuint bound_array_buffer;
    GLuint bound_element_array_buffer;

    /* GL 2.0's programmable pipeline - gl_shader.c.
     *
     * One counter for both tables, because the specification gives shader and program
     * objects a single name space (GL 2.0, 2.15.1). It only ever goes up: a
     * name is never reused within a context, so a stale name held by a program is an
     * error rather than a silent hit on whatever took the slot. */
    gl_shader_object_t shaders[OOPS_GL_MAX_SHADER_OBJECTS];
    gl_program_object_t programs[OOPS_GL_MAX_PROGRAM_OBJECTS];
    GLuint gl2_next_name;
    /* Whether this context has ever made a shader or a program.
     *
     * A GL 1.x program pays nothing for GL 2.0: the vertex fetch skips filling the
     * generic attribute slots until `glCreateShader` or `glCreateProgram` is called.
     * It is never cleared, which is the conservative direction. */
    GLboolean gl2_used;
    /* glUseProgram's, and 0 for the fixed-function pipeline. A program flagged for
     * deletion while in use stays here and keeps drawing until another is made current.
     */
    GLuint program_current;
    gl_vertex_attrib_t vertex_attribs[OOPS_GL_MAX_VERTEX_ATTRIBS];

    /* Error tracking */
    GLenum last_error;

    /* What glGetString(GL_VERSION) answers - the caller's to state,
     * glContextSetVersion. The string is built here because glGetString hands out a
     * pointer that must outlive the call. "1.5 oops-gl fixed-function subset" is 33
     * bytes with its terminator. */
    GLuint version_major, version_minor;
    char version_string[48];

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
    /* GL 1.5's occlusion queries on the GPU. `hw_query_counting` is
     * query-scoped: the begin snapshot has been dumped and the count is running.
     * `hw_query_reg` is frame-scoped, like `hw_z_bound`: this frame has raised
     * DB_COUNT_CONTROL's precision bits, which a flush mid-query undoes by re-emitting
     * the depth block. */
    GLboolean hw_query_counting;
    GLboolean hw_query_reg;
    /* Antialiasing on the console. `aa_hw_on` says this
     * primitive carries its coverage geometry in the texture-coordinate parameter for
     * the untextured pixel shader's slot; `aa_hw_half` is the widened quad's
     * half-extent in pixels and `aa_hw_r` the primitive's own half-extent plus a half,
     * which is the constant the shader subtracts the distance from. Set and cleared
     * around one primitive, like `aa_kind`. (`aa_hw` above is the software path's line
     * half-width and is a different thing entirely.) */
    GLboolean aa_hw_on;
    /* Whether the smoothing draw is textured, which decides both the interpolant the
     * offset rides in and the shader whose slot carries the coverage: the second
     * texture unit's parameter and the textured shader when it is, the texture
     * coordinate's parameter and the untextured shader when it is not. */
    GLboolean aa_hw_tex;
    float aa_hw_half;
    float aa_hw_r;
    GLboolean
        hw_stencil_bound; /* and the stencil surface, live, for a frame that tests it */
    /* `glViewport` was called after this frame's registers were written, so the next
     * draw has to re-emit them. Frames that set the viewport before drawing - which is
     * every frame gl1-cube renders - never raise it, so their command stream is
     * unchanged. */
    /* Texture coordinate generation is per unit - gl_tex_unit_t. */

    /* Fog. The distance it works from is the eye-space distance to the fragment, so the
     * factor is computed per vertex and interpolated - see the fog field on the screen
     * vertex. */
    GLboolean cap_fog;
    GLenum fog_mode;
    float fog_density;
    float fog_start;
    float fog_end;
    float fog_color[4];

    /* Point size and line width, in pixels. Both are screen-space quantities, which is
     * why the expansion that honours them has to happen after projection. */
    float point_size;
    float line_width;
    /* GL 1.4's point parameters: the clamp every point's size meets, the distance
     * attenuation (a, b, c) that divides it by sqrt(a + b d + c d^2), and the fade
     * threshold - state only, since the fade applies to multisampled points and there
     * is no multisample buffer. */
    float point_size_min, point_size_max, point_fade_threshold, point_atten[3];

    /* Raster position, in window coordinates, plus the colour and texture coordinate
     * latched with it. `raster_valid` is false when the position clipped, and an
     * invalid position draws nothing - clamping it to the edge instead would put a
     * bitmap somewhere the program never asked for, which is worse than drawing
     * nothing. */
    float raster_pos[4];
    float raster_color[4];
    float raster_texcoord[OOPS_GL_MAX_TEXTURE_UNITS][4]; /* per unit */
    float raster_distance;
    GLboolean raster_valid;
    float pixel_zoom_x;
    float pixel_zoom_y;

    /* Stencil. Eight bits per pixel in its own buffer, because on this hardware the
     * stencil surface is separate from the depth one - DB_STENCIL_INFO and
     * DB_STENCIL_READ/WRITE_BASE are their own registers, so Z_32_FLOAT having no
     * stencil plane costs nothing. */
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
    /* GL 2.0's back face. One stencil state served both faces through GL 1.x; 2.0
     * splits them, which is what a single-pass stencil shadow volume needs - increment
     * on the front faces and decrement on the back, in one draw rather than two.
     *
     * `glStencilFunc`, `glStencilOp` and `glStencilMask` set both, which is how the
     * specification defines them from 2.0 onwards - so a GL 1.x program is unaffected
     * and these fields simply track the front ones. The face is a property of the
     * primitive rather than of the fragment, so the choice between the two sets is made
     * once per triangle; see `gl_frag_ops_init`. */
    GLenum stencil_back_func;
    GLint stencil_back_ref;
    GLuint stencil_back_value_mask;
    GLuint stencil_back_writemask;
    GLenum stencil_back_fail;
    GLenum stencil_back_zfail;
    GLenum stencil_back_zpass;
    GLint clear_stencil;

    /* User clip planes, in eye coordinates - stored through the inverse modelview
     * of the moment glClipPlane was called, the same rule the eye-linear texgen plane
     * follows. */
    float clip_plane[OOPS_GL_CLIP_PLANE_COUNT][4];
    GLboolean clip_plane_enabled[OOPS_GL_CLIP_PLANE_COUNT];
    GLboolean hw_clip_dirty;

    GLboolean hw_vport_dirty;
    GLboolean hw_scissor_dirty;
    /* PA_CL_VPORT_ZSCALE / ZOFFSET, for a glDepthRange after the frame's registers went
     * out. */
    GLboolean hw_depth_range_dirty;
    /* CB_COLOR_CONTROL, for a logic op switched or changed after the frame's registers
     * went out. */
    GLboolean hw_color_control_dirty;
    /* CB_BLEND_RED..ALPHA are not part of the frame's register table at all - gl-cube's
     * recorded stream has no constant colour in it - so they go out at the first draw
     * that blends with a constant factor, and again after every glBlendColor. Set at
     * the start of each frame, since nothing says another submission left them alone.
     */
    GLboolean hw_blend_color_dirty;
    /* Which texture's descriptors are in this frame's one descriptor slot, 0 for none.
     */
    GLuint hw_frame_tex;
    /* Which descriptor slot the textures of this frame have reached. 0 is the original
     * table, and a frame that never changes texture stays there - see the ring's note
     * above. */
    uint32_t hw_desc_slot;
    /* Which block this frame's GL 2.0 draws have reached, and whose contents are in it
     * - the uniforms and the texture descriptors together, since they share one block.
     * The program name is part of the question: two programs' pools are different
     * lengths, so comparing the bytes alone could call one unchanged from the other. 0
     * is "nothing yet". */
    uint32_t hw_gl2_slot;
    GLuint hw_gl2_slot_program;
    uint32_t hw_attrib_slot;
    size_t depth_px; /* floats in depth_buffer: the 64KB_Z_X tiled extent, both axes
                        padded to 128 px */
    GLboolean use_hardware;
    uint32_t canary_vs;
    uint32_t canary_ps;
    uint32_t canary_vs_s0;
    uint32_t canary_ps_s0;
    GLboolean fb_cleared;

    /* The hardware proof: what the last submission measured, never which path was
     * picked. */
    GLboolean hw_failed; /* a submit or fence failure: drawing has stopped for good */
    const char *hw_failure; /* why, for the log and the HUD */
    uint32_t hw_last_fence; /* the end-of-pipe fence word read back after the last
                               submission */
    uint64_t
        hw_last_timestamp; /* the GPU clock counter the end-of-pipe RELEASE_MEM wrote */
    uint64_t hw_prev_timestamp;
    uint32_t hw_frames_confirmed; /* submissions whose fence and clock both arrived */
    GLboolean hw_clear_verified;  /* the GPU-only clear at context creation survived
                                     readback */
    uint32_t hw_clear_colour;
    uint32_t hw_clear_matched;
    uint32_t hw_clear_expected;
    GLboolean hw_dump_pending;  /* log the next submission's stream, shaders and fence:
                                   the oracle record */
    const uint32_t *hw_prelude; /* glSetHardwarePrelude: words every frame's stream
                                   opens with, or NULL */
    uint32_t hw_prelude_words;
    uint32_t *readback; /* CPU-cached copy of the render target, made by the CP at the
                           end of every submission */
    /* The colour buffer that copy is of, or NULL once the CPU has written into it since
     * (gl_raster.c's gl_raster_sync) - see gl_color_read_source. */
    const uint32_t *readback_of;
    /* The second colour target's copy, and the buffer it is of. A draw under
     * GL_FRONT_AND_BACK writes two surfaces, and `glReadPixels` of the one in `fb_also`
     * needs its own copy rather than the CPU's view of memory the GPU just wrote.
     * Allocated the first time a second target is bound. */
    uint32_t *readback_also;
    const uint32_t *readback_also_of;
    /* On the scanout path the copy is tiled like its buffer; glGetFrameReadback detiles
     * it into this, the linear image its callers index. */
    uint32_t *readback_lin;

    /* glPushAttrib. The specification requires at least 16 deep. */
    gl_attrib_entry_t attrib_stack[OOPS_GL_ATTRIB_STACK_CAPACITY];
    GLuint attrib_depth;

    /* glPushClientAttrib. Its own stack and its own depth; the specification's minimum
     * is 16. */
    gl_client_attrib_entry_t client_attrib_stack[OOPS_GL_CLIENT_ATTRIB_STACK_CAPACITY];
    GLuint client_attrib_depth;

    /* glAlphaFunc. A fragment whose alpha fails the comparison is discarded. */
    GLboolean cap_alpha_test;
    GLenum alpha_func;
    float alpha_ref;

    /* glPolygonOffset, and an enable per polygon mode: the filled polygons, their
     * outlines, and their corner points. */
    GLboolean cap_polygon_offset_fill;
    GLboolean cap_polygon_offset_line;
    GLboolean cap_polygon_offset_point;
    float polygon_offset_factor;
    float polygon_offset_units;

    /* glPolygonMode: [0] for front faces, [1] for back. */
    GLenum polygon_mode[2];

    /* What the triangles now being drawn stand for. Everything reaches the
     * rasteriser - and the hardware - as triangles, including a line (a screen-width
     * quad) and a point (a square). But culling applies to polygons only, and each
     * polygon-offset enable to one kind, so the triangle stage has to know: GL_FILL for
     * a polygon, GL_LINE or GL_POINT for an expanded line or point, and
     * `prim_from_polygon` when that line or point is a polygon's outline or corner
     * under glPolygonMode rather than a GL_LINES or GL_POINTS primitive. Set around
     * each expansion and GL_FILL otherwise. */
    GLenum prim_raster;
    GLboolean prim_from_polygon;
    /* Whether that polygon faces away - the side two-sided lighting lights its outline
     * with. */
    GLboolean prim_polygon_back;

    /* glDepthRange: where NDC z lands in the depth buffer. Defaults to the
     * specification's 0..1, which is the pair the viewport registers were already
     * carrying as constants. */
    float depth_near;
    float depth_far;

    /* glPixelStorei: how a client's pixel rectangle is laid out in its own memory. */
    GLint unpack_alignment;  /* row start alignment in bytes: 1, 2, 4 or 8 */
    GLint unpack_row_length; /* pixels per source row, or 0 meaning "the width being
                                uploaded" */
    GLint pack_alignment;    /* the same, for rows glReadPixels writes back */
    /* Rows per slice of a 3D image, or 0 meaning "the height being uploaded" - how far
     * apart a volume's slices are in the caller's memory. */
    GLint unpack_image_height, pack_image_height;
    /* The rest of glPixelStorei (gl_pixel.c): where in the caller's image the rectangle
     * starts, whether its multi-byte elements are the other way round, bit order within
     * a bitmap byte, and the pack side's row length. */
    GLint unpack_skip_rows, unpack_skip_pixels, unpack_skip_images;
    GLboolean unpack_swap_bytes, unpack_lsb_first;
    GLint pack_row_length, pack_skip_rows, pack_skip_pixels, pack_skip_images;
    GLboolean pack_swap_bytes, pack_lsb_first;

    /* Display lists: what was recorded, and what is being recorded now. */
    gl_display_list_t lists[GL_MAX_LISTS];
    GLuint list_compiling; /* the name being recorded into, or 0 for none */
    GLenum list_mode;      /* GL_COMPILE or GL_COMPILE_AND_EXECUTE */
    GLuint list_base;      /* glListBase, added to every name glCallLists reads */
    GLuint list_depth;     /* nested glCallList, to stop a list that calls itself */
    /* Nonzero while a recorded command is being executed on the recorder's own behalf -
     * the GL_COMPILE_AND_EXECUTE half of a call, or a list replayed inside one - so the
     * entry points it runs through do not record themselves a second time. */
    GLuint list_suspend;

    /* Performance telemetry */
    uint64_t frame_count;
    uint64_t triangles_drawn;

    /* How many parameters the bound vertex shader exports: 2, 3 (the colour sum's,
     * gl_vs_build_param3) or 4 (the second texture unit's), with SPI_VS_OUT_CONFIG,
     * SPI_PS_IN_CONTROL and SPI_PS_INPUT_CNTL_n set for it. Reset when a frame begins,
     * which binds the two-parameter shader. */
    uint32_t hw_params;
    /* Whether a draw may use the second texture unit on this path.
     * OOPS_GL_MULTITEX_MEASURED until a draw or a test says otherwise - gl_multitex.h.
     */
    GLboolean hw_multitex;
    /* The vertex buffer's fill in bytes, reset with the stream. */
    uint32_t hw_vbo_cursor;
} gl_context_t;

extern gl_context_t *g_gl_ctx;

/* The texture unit glActiveTexture selected: what every texture state call - binding,
 * enabling, parameters of the environment, generation, the texture matrix - acts on. A
 * macro so that it keeps the constness of the context it is given. */
#define gl_tu(ctx) (&(ctx)->tex_unit[(ctx)->active_texture])

static inline gl_context_t *gl_get_ctx(void) {
    return g_gl_ctx;
}

/* Records an error the way GL says to: the first one wins. Once the flag is set, no
 * further errors are recorded until glGetError() reads and clears it; glGetError()
 * itself assigns, because clearing the flag is its job. */

/* Appends "0x" and `v` in hex to `buf` at `n`, and returns the new length. */
static inline size_t gl_msg_hex(char *buf, size_t cap, size_t n, uint32_t v) {
    if (n + 3u >= cap)
        return n;
    buf[n++] = ' ';
    buf[n++] = '0';
    buf[n++] = 'x';
    int top = 28;
    while (top > 0 && ((v >> top) & 0xfu) == 0u)
        top -= 4;
    while (top >= 0 && n < cap - 1u) {
        const uint8_t d = (uint8_t)((v >> top) & 0xfu);
        buf[n++] = (char)(d < 10u ? ('0' + d) : ('a' + d - 10u));
        top -= 4;
    }
    return n;
}

/* The value that was refused, when the call site knows it, so the log names both the
 * entry point and the enum: `glEnable 0x8861`.
 *
 * `val` is a GLenum at almost every site and an index or a bitfield at a couple, so it
 * prints as hex. Sites with no single culprit use `gl_record_error`. */
static inline void gl_record_error_val_at(gl_context_t *ctx, GLenum error, uint32_t val,
                                          GLboolean have_val, const char *fn);

static inline void gl_record_error_at(gl_context_t *ctx, GLenum error, const char *fn) {
    gl_record_error_val_at(ctx, error, 0u, GL_FALSE, fn);
}

static inline void gl_record_error_val_at(gl_context_t *ctx, GLenum error, uint32_t val,
                                          GLboolean have_val, const char *fn) {
    if (!ctx)
        return;
    /* Counted on every raise, not only on the one that sets the flag: the flag says
     * what went wrong first, the count how much, for a port that never calls
     * glGetError(). Every error in this library passes through here. */
    ctx->hw_gl_errors++;
    ctx->hw_gl_error_last = (uint32_t)error;
    /* Which entry point refused, by name: this library raises GL_INVALID_ENUM from
     * dozens of calls, and `__func__` at the call site costs only a string literal
     * already in the binary. Logged once, because this sits on a path some programs
     * take per frame. */
    ctx->hw_gl_error_fn = fn;
    ctx->hw_gl_error_val = have_val ? val : 0u;
    ctx->hw_gl_error_has_val = have_val;
    if (ctx->hw_gl_errors == 1u && fn) {
        char msg[128];
        size_t n = 0;
        const char *head = "first GL error ";
        while (head[n] && n < sizeof(msg) - 64) {
            msg[n] = head[n];
            n++;
        }
        n = gl_msg_hex(msg, sizeof(msg), n, (uint32_t)error);
        const char *mid = " raised by ";
        size_t k = 0;
        while (mid[k] && n < sizeof(msg) - 48) {
            msg[n++] = mid[k++];
        }
        size_t m = 0;
        while (fn[m] && n < sizeof(msg) - 16) {
            msg[n++] = fn[m++];
        }
        /* The value it refused. */
        if (have_val) {
            const char *tail = " on";
            size_t j = 0;
            while (tail[j] && n < sizeof(msg) - 14) {
                msg[n++] = tail[j++];
            }
            n = gl_msg_hex(msg, sizeof(msg), n, val);
        }
        msg[n] = '\0';
        oops_log_info("GL", "%s", msg);
    }
    if (ctx->last_error == GL_NO_ERROR) {
        ctx->last_error = error;
    }
}

/* The name comes from the call site, so the macro has to expand there - `__func__`
 * inside the inline above would be the inline's own name for every caller in the
 * library. */
#define gl_record_error(ctx, err) gl_record_error_at((ctx), (err), __func__)
/* The same, where the site knows what it is refusing. Prefer it. */
#define gl_record_error_val(ctx, err, val)                                             \
    gl_record_error_val_at((ctx), (err), (uint32_t)(val), GL_TRUE, __func__)

/* -------------------------------------------------------------------------
 * The version an app claimed, and what it gets for claiming it
 *
 * A context has the entry points its version defines and no others. Everything here is
 * one archive, so where a desktop driver's linker would refuse, this is a runtime check
 * with the specification's answer for a call not in the context: GL_INVALID_OPERATION,
 * and the call does nothing. A function that returns a value returns its failure value:
 * 0 for a name, -1 for a location, GL_FALSE for a predicate.
 *
 * The gate covers GL 2.0's entry points, not GL 1.x's. Every GL 1.2-1.5 feature here is
 * also advertised as an extension (`GL_ARB_multitexture`, `GL_ARB_vertex_buffer_object`
 * and the rest), and an extension is available whatever the core version, so refusing
 * `glBindBuffer` beside `glBindBufferARB` would be a rule about spelling. Nothing
 * advertises the programmable pipeline as an extension, so claiming 2.0 is the only way
 * to it.
 * ------------------------------------------------------------------------- */

static inline GLboolean gl_version_at_least(const gl_context_t *ctx, GLuint major,
                                            GLuint minor) {
    if (!ctx)
        return GL_FALSE;
    return (GLboolean)(ctx->version_major > major ||
                       (ctx->version_major == major && ctx->version_minor >= minor));
}

/* The gate every entry point a version added begins with. False means the call must
 * do nothing; the error is already recorded. */
static inline GLboolean gl_require_version(gl_context_t *ctx, GLuint major,
                                           GLuint minor) {
    if (gl_version_at_least(ctx, major, minor))
        return GL_TRUE;
    gl_record_error(ctx, GL_INVALID_OPERATION);
    return GL_FALSE;
}

/* The same for a `glGet` enumerant a later version added. An enum a context does not
 * know is GL_INVALID_ENUM, not GL_INVALID_OPERATION, as the specification distinguishes
 * them. */
static inline GLboolean gl_require_version_enum(gl_context_t *ctx, GLuint major,
                                                GLuint minor) {
    if (gl_version_at_least(ctx, major, minor))
        return GL_TRUE;
    gl_record_error(ctx, GL_INVALID_ENUM);
    return GL_FALSE;
}

/* A float's bits, for the registers and shader literals that take one. */
static inline uint32_t gl_f32_bits(float f) {
    union {
        float f;
        uint32_t u;
    } v;
    v.f = f;
    return v.u;
}

/* Where the alpha test sits in each pixel shader, as a word index. Four words are
 * reserved, which is what the longest form needs: a literal load (two words), a
 * compare, and the mask update. `gl_ps_patch_alpha_test` writes them; anything else
 * leaves them as `s_nop`.
 *
 * Patched in place rather than rebuilt, because the shaders are laid into the GPU
 * payload once at context creation. The instruction encodings are assembled for gfx1030
 * - see the table in `gl_ps_patch_alpha_test`.
 */
#define GL_PS_ALPHA_SLOT_UNTEX 80u
#define GL_PS_ALPHA_SLOT_TEX                                                           \
    286u /* +16 stipple, +28 the sample slot, +20 unit 1, +64 combine 2, +28 coverage  \
          */
/* The texture combine: the instructions combining the sampled texel with the
 * interpolated colour. Sixty-four words, for the general form GL_BLEND, GL_DECAL of
 * RGBA and GL_COMBINE need. A program shorter than the slot ends in an `s_branch` over
 * the rest. */
/* The second texture unit's sample: twenty words at the end of the prolog, before exec
 * is restored from s16 - a sample outside whole-quad mode has no helper pixels, so its
 * level of detail is wrong along every quad edge. Both samples are hoisted above both
 * combines for that reason; GL's order is kept by the combine slots, not by where the
 * texels are fetched. `gl_ps_patch_unit1` writes them from tools/shader/tex-prolog2.s,
 * and a branch over the slot is one unit, which every one-texture draw writes.
 * `gl_multitex.h` is the gate. */
/* How unit 0 is sampled: the prolog's descriptor loads are shared, and this slot is the
 * sample itself, in one of five forms. A 2D texture is the two-word `image_sample`,
 * followed by a branch over the rest. A volume is `tools/shader/tex-3d.s`: r
 * interpolated and divided by the q the prolog already reciprocated, the divided pair
 * copied up beside it, and `dim:SQ_RSRC_IMG_3D`. A cube map is
 * `tools/shader/tex-cube.s`: the coordinate taken as a direction, turned into a face
 * and a place on it by the four `v_cube*` instructions, and sampled with
 * `dim:SQ_RSRC_IMG_CUBE` - the longest of them, which is why the slot is this size. A
 * depth texture is `tools/shader/tex-shadow.s`: `dmask:0x1`, because one value comes
 * back, spread across v4..v7 as GL_DEPTH_TEXTURE_MODE says - and with GL 1.4's
 * comparison, `image_sample_c` with the clamped reference ahead of s and t.
 * `gl_ps_patch_sample` writes it. */
#define GL_PS_SAMPLE_SLOT_TEX 56u
#define GL_PS_SAMPLE_WORDS 28u
#define GL_PS_UNIT1_SLOT_TEX 85u
#define GL_PS_UNIT1_WORDS 20u
#define GL_PS_COMBINE_SLOT_TEX 106u
#define GL_PS_COMBINE_WORDS 64u
/* The second unit's combine stage, the same sixty-four words for
 * unit 1's environment: its texel against what unit 0's stage left in v4..v7. A branch
 * over the slot is one unit, and is what a one-unit draw writes back into it. */
#define GL_PS_COMBINE2_SLOT_TEX 170u
/* Fog: twelve words in each shader, after the colour is final and before the alpha
 * test, which GL puts after fog. `gl_ps_patch_fog` writes them from
 * `tools/shader/fog.s`; `s_nop` is no fog. */
#define GL_PS_FOG_SLOT_UNTEX 40u
#define GL_PS_FOG_SLOT_TEX                                                             \
    246u /* +16 stipple, +28 the sample slot, +20 unit 1, +64 combine 2 */
#define GL_PS_FOG_WORDS 12u
/* The colour sum: twelve words in the textured shader between the combine and fog, GL's
 * order, adding the secondary colour the third parameter carries
 * (tools/shader/colour-sum.s); `s_nop` is no sum. The untextured shader has no such
 * slot: without a
 * texture between them, adding the secondary colour to the primary per vertex is the
 * same sum, except where it saturates between the vertices. */
#define GL_PS_SUM_SLOT_TEX 234u
#define GL_PS_SUM_WORDS 12u
/* The polygon stipple's discard: sixteen words in both shaders,
 * between the canary block and the interpolation - the fragment's position arrives in
 * v2 and v3, which the interpolation uses as scratch, so the lookup has to run before
 * it, and before whole-quad mode so that the mask the sample restores is the one the
 * discard left. `gl_ps_patch_stipple` writes them from
 * `tools/shader/polygon-stipple.s`; `s_nop` is no stipple. */
#define GL_PS_STIPPLE_SLOT 16u
#define GL_PS_STIPPLE_WORDS 16u
#define GL_PS_EXPORT_TEX 290u
/* The export and the end of the program, patched together by `gl_ps_patch_export`.
 * One colour target is `exp mrt0 ... done compr vm`, `s_endpgm` and
 * a nop; both buffers is `exp mrt0 ... compr vm`, `exp mrt1 ... done compr vm`,
 * `s_endpgm` - `done` belongs to the last export a wave raises, so the single-target
 * word cannot simply be repeated (tools/shader/mrt1-export.s).
 *
 * Seven words, because the export is preceded by the two `v_cvt_pkrtz_f16_f32` that
 * pack four floats into two registers: an 8_8_8_8 target on a part with RB+ requires
 * the half-float export format (see `gl_ps_patch_export`). */
#define GL_PS_EXPORT_WORDS 7u
#define GL_PS_EXPORT_UNTEX 84u
/*
 * Antialiasing's coverage: a slot between fog and the alpha test in both pixel shaders,
 * which is where GL applies it (1.x, 3.12) and where the software rasteriser applies
 * it. `tools/shader/coverage.s`; a branch over the slot is no smoothing, and
 * `gl_ps_patch_coverage_where` writes it.
 *
 * The geometry is interpolated rather than computed. A smooth point's coverage is
 * `r + 1/2 - |p - c|` and a smooth line's `w/2 + 1/2 - |across|`, and the offset those
 * need is linear across the quad the CPU widened the primitive into - so the CPU writes
 * it at the corners and the interpolator carries it in. A line puts zero in the second
 * component, which makes one form serve both.
 *
 * In the untextured shader the offset rides in the texture coordinate's parameter,
 * which that shader does not otherwise read. The textured shader's slot reads `attr3`
 * instead of `attr1` (`tools/shader/coverage-tex.s`): a textured draw reads all four
 * components of `attr1` - s and t, fog's factor, q - so the offset goes in the second
 * texture unit's parameter, and a one-unit draw escalates to four parameters for it. A
 * draw using two units has only `attr3.z` spare, one float where three are needed, so
 * that case is aliased and says so in the log.
 *
 * GL_POLYGON_SMOOTH lives in the same slot, which is why it is twenty-eight words: its
 * coverage is the product of three edge
 * fades instead of one distance, and the three distances arrive as `d*w` with `w`
 * beside them, to be divided per fragment the way a projected texture coordinate is.
 * See `tools/shader/coverage-poly.s`. It always reads `attr3`, textured or not, so one
 * program serves both shaders - a smooth polygon escalates to four parameters for it
 * and, like a smooth textured point, is refused when the second texture unit wants that
 * parameter.
 */
#define GL_PS_COVERAGE_SLOT_UNTEX 52u
#define GL_PS_COVERAGE_SLOT_TEX 258u
#define GL_PS_COVERAGE_WORDS 28u
/* Where the pixel shaders sit in the GPU payload. The textured one has 320 words at
 * 0x1000 (to 0x1500) with nothing above it, room for its sampling and combine slots;
 * the untextured one has 128 words at 0x200. */
/* The textured shader's variant ring, OOPS_GL_PS_RING_SLOTS copies of the 0x500-byte
 * shader from 0x6000, each holding one set of patched words, so a draw binds the
 * variant it needs instead of the one slot being rewritten underneath draws already
 * queued.
 *
 * Fixed-function state - the texture combine, fog, the alpha test - is implemented by
 * patching instruction words of a pre-assembled shader. Editing words that queued draws
 * still point at would change what those draws do, so without the ring every edit would
 * submit the frame and wait. The ring makes an edit cost a memcpy, and a variant still
 * resident costs nothing - the common case, since a program alternates between a few
 * arrangements.
 *
 * The shader is relocatable: it holds no reference to its own address. The one absolute
 * address in it is the canary's, which is data and is the same for every copy. */
#define OOPS_GL_PS_RING_OFFSET 0x6000u

#define OOPS_GL_PS_TEX_OFFSET 0x1000u
#define OOPS_GL_PS_TEX_WORDS 320u
#define OOPS_GL_PS_UNTEX_OFFSET 0x200u
#define OOPS_GL_PS_UNTEX_WORDS 128u
/* The three-parameter vertex shader: 64 words at 0x700, between the
 * textured pixel shader's end (0x6c0) and the descriptors (0x900). */
#define OOPS_GL_VS_P3_OFFSET 0x700u
#define OOPS_GL_VS_P3_WORDS 64u
/* The four-parameter vertex shader: 70 words at 0xb00, clear of the border colour table
 * at 0xa00. It carries a second texture unit's coordinate in an 80-byte vertex
 * (gl_multitex.h). */
#define OOPS_GL_VS_P4_OFFSET 0xb00u
#define OOPS_GL_VS_P4_WORDS 70u
/* The polygon stipple's 32x32 mask, as the pixel shader reads it: 32 rows of one dword,
 * at 0x800 in the gap the three-parameter vertex shader leaves before the descriptors.
 * Written by `gl_ps_patch_stipple`, rotated for the window height and bit-reversed, so
 * the shader's lookup is a load and a shift. */
#define OOPS_GL_STIPPLE_OFFSET 0x800u
#define OOPS_GL_STIPPLE_WORDS 32u
/* The texture descriptor table, whose address goes in the pixel shader's first user
 * SGPR pair. Two pairs: unit 0's image at +0x00 and sampler at +0x20, unit 1's at +0x40
 * and +0x60, where tex-prolog2.s loads them (gl_multitex.h). Both pairs are filled when
 * a draw uses two units, and both decide when the ring's slot advances - see the
 * comparison in `gl_draw.c`. */
#define OOPS_GL_DESC_TABLE_OFFSET 0x900u
#define OOPS_GL_DESC_UNIT_STRIDE 0x40u

/*
 * A ring of descriptor slots, so a frame that draws several textures submits once
 * instead of once per texture: with one slot, a second texture written before the frame
 * runs would be sampled by every draw built against the first.
 *
 * The shader needs no change. The descriptor table's address is per-draw user data
 * (`SPI_SHADER_USER_DATA_PS_0`/`_1`), and `tex-prolog.s` and `tex-prolog2.s` load their
 * pairs at `s[0:1]+0x00` and `s[0:1]+0x40`, relative to that base. Pointing the base at
 * a different slot is the whole mechanism.
 *
 * Slot 0 is the table at 0x900, so a frame that never changes texture emits the stream
 * the gl-cube oracle record (docs/hardware/agc-gl-cube-oracle-fw1240.md) pins. Slots 1
 * and up live in their own region, clear of everything else in the payload.
 *
 * The ring wraps into a flush, as the vertex ring does. A border colour still flushes
 * on a change whatever the ring says, because `TA_BC_BASE_ADDR` is a frame register
 * rather than something the slot carries.
 */
/* Where a compiled GL 2.0 pixel shader sits: 512 words at 0x3800, running to 0x4000,
 * above the descriptor ring and below the GL 2.0 uniform ring.
 *
 * One slot, not one per program. A compiled shader lives in the program object and is
 * copied here when a draw needs it, the way a texture's descriptors are copied into
 * their slot; a frame that switches between two programs pays an upload and a cache
 * flush per switch (`hw_ps_resident`). */
#define OOPS_GL_PS_GL2_OFFSET 0x3800u
#define OOPS_GL_PS_GL2_WORDS 512u

/*
 * A ring of blocks for GL 2.0 draws, at 0x4000 to the end of the payload.
 *
 * One block holds everything a compiled pixel shader is handed, and the shader is given
 * its address in `SPI_SHADER_USER_DATA_PS_0`/`_1`:
 *
 *     +0x00   texture unit 0: the image descriptor (8 dwords), then the sampler (4)
 *     +0x40   texture unit n at n * 0x40, the same, OOPS_GL_GL2_TEX_SETS of them
 *     +0x100  the uniform block (OOPS_GL_GL2_UNIFORM_AT)
 *
 * The two halves are one block on purpose. `OOPS_GL_DESC_UNIT_STRIDE` is the same
 * 0x40 the fixed-function descriptor table uses, so a unit's descriptors sit where
 * `tex-prolog.s` would look for them - and a compiled shader takes one base address and
 * finds both, rather than needing a second user SGPR pair for the second thing.
 *
 * A uniform is the same value in every lane and every fragment, so it belongs in an
 * SGPR; an SGPR is loaded from memory, so the values need an address. The block is
 * `p->values` copied verbatim - the linker already lays the uniforms out as a flat pool
 * of floats and every `glUniform*` writes into it, so what the shader loads and what
 * `glGetUniformfv` reads back are the same bytes rather than two layouts to keep in
 * step.
 *
 * A ring and not a slot, for the reason the descriptors needed one: uniforms change
 * between draws, and one slot would mean every draw in a frame reading whatever the
 * last one set.
 *
 * Its own ring, and not a wider descriptor slot, so the stride of the ring the GL 1.x
 * console path runs through - and its oracle record - does not move. obSCEne measured
 * the block arriving intact through this wiring: its address in the pixel shader's
 * first user SGPR pair and `s_load_dwordx16` from it. Without the
 * `s_waitcnt lgkmcnt(0)` the shader reads zeros, so the wait is load-bearing.
 *
 * A shader loads a window of the uniform block into registers (see
 * OOPS_GL_GL2_UNIFORM_FLOATS); a program past it is refused by the compiler with the
 * number.
 */
#define OOPS_GL_GL2_SLOT_OFFSET 0x4000u
#define OOPS_GL_GL2_SLOT_STRIDE 0x200u /* four descriptor sets, then the uniforms */
#define OOPS_GL_GL2_SLOTS 32u          /* 0x4000 .. 0x8000, the rest of the payload */
#define OOPS_GL_GL2_UNIFORM_AT 0x100u  /* the uniform block's offset within a slot */
/* How many floats the block carries, which is not how many a shader can hold in
 * registers. The slot has room for 64 between `OOPS_GL_GL2_UNIFORM_AT` and its end; the
 * scalar file has room for 32 above the loop masks. `glsl_ps.c` loads a window of the
 * block, so a program's pool may be this big as long as the uniforms one fragment
 * shader names fall inside one window - which is the thing the register file actually
 * limits. */
#define OOPS_GL_GL2_UNIFORM_FLOATS 64

/* The draw's own constants, four floats the shader may need that are not the program's
 * uniforms: the render target's height, which turns the hardware's window y into
 * `gl_FragCoord`'s.
 *
 * The target's height and not the viewport's: `gl_FragCoord` is window-relative, and
 * every other y flip in `gl_draw.c` uses `ctx->height` for the same reason.
 *
 * It lives in the last descriptor set's tail. A set is `OOPS_GL_DESC_UNIT_STRIDE` =
 * 0x40 and holds a 32-byte image descriptor and a 16-byte sampler, so the last sixteen
 * bytes of each are padding, on an offset a scalar load of four dwords can name; the
 * assertion below holds it clear of every descriptor. */
#define OOPS_GL_GL2_DRAWCONST_AT 0xF0u
#define OOPS_GL_GL2_DRAWCONST_FLOATS 4
/* Which float is which. Room for three more before the set above it. */
#define OOPS_GL_GL2_DC_TARGET_H 0

static inline uint32_t gl_hw_gl2_slot_offset(uint32_t slot) {
    return OOPS_GL_GL2_SLOT_OFFSET + slot * OOPS_GL_GL2_SLOT_STRIDE;
}

#define OOPS_GL_VS_GL2_OFFSET 0x8000u
#define OOPS_GL_VS_GL2_WORDS 1024u

#define OOPS_GL_ATTRIB_SLOT_OFFSET 0x9000u
#define OOPS_GL_ATTRIB_SLOT_STRIDE 0x100u /* 16 attributes * 16 bytes */
#define OOPS_GL_ATTRIB_SLOTS 32u          /* 32 * 256 = 8192 bytes, ends at 0xb000u */

static inline uint32_t gl_hw_attrib_slot_offset(uint32_t slot) {
    return OOPS_GL_ATTRIB_SLOT_OFFSET + slot * OOPS_GL_ATTRIB_SLOT_STRIDE;
}

#define OOPS_GL_PAYLOAD_BYTES 0x10000u

#define OOPS_GL_DESC_RING_OFFSET 0x1800u
#define OOPS_GL_DESC_SLOT_STRIDE 0x80u /* two units, 0x40 each */
/* `OOPS_GL_DESC_RING_SLOTS` is defined near the top of this file, beside the other
 * capacities, because `gl_context_t` sizes an array by it - see `hw_slot_tex`. Its
 * meaning belongs here with the rest of the descriptor map: 63 slots, plus slot 0 at
 * the table above, so 64 textures a frame before the ring is full and the frame is
 * submitted. */

/* Where slot `n` of the ring begins, as a byte offset into the GPU payload. Slot 0 is
 * the table at OOPS_GL_DESC_TABLE_OFFSET. */
static inline uint32_t gl_hw_desc_slot_offset(uint32_t slot) {
    return slot == 0u
               ? OOPS_GL_DESC_TABLE_OFFSET
               : OOPS_GL_DESC_RING_OFFSET + (slot - 1u) * OOPS_GL_DESC_SLOT_STRIDE;
}

/* The compiler checks the map closes. The regions above are separate constants, and one
 * growing past the next would put a shader over a descriptor or a uniform block over a
 * shader, neither of which faults. */
typedef char oops_gl_payload_map_closes
    [(OOPS_GL_DESC_RING_OFFSET + OOPS_GL_DESC_RING_SLOTS * OOPS_GL_DESC_SLOT_STRIDE <=
          OOPS_GL_PS_GL2_OFFSET &&
      /* `hw_desc_shadow` mirrors a whole slot and is sized by a literal, because the
       * struct is declared above these constants. The two have to stay the same size or
       * the shadow describes less of the slot than the copy writes. */
      OOPS_GL_DESC_SLOT_STRIDE == 32u * 4u &&
      /* And the two shader mirrors, sized by literals for the same reason. */
      OOPS_GL_PS_UNTEX_WORDS == 128u && OOPS_GL_PS_TEX_WORDS == 320u &&
      OOPS_GL_PS_GL2_OFFSET + OOPS_GL_PS_GL2_WORDS * 4u <= OOPS_GL_GL2_SLOT_OFFSET &&
      OOPS_GL_GL2_SLOT_OFFSET + OOPS_GL_GL2_SLOTS * OOPS_GL_GL2_SLOT_STRIDE <=
          OOPS_GL_VS_GL2_OFFSET &&
      OOPS_GL_VS_GL2_OFFSET + OOPS_GL_VS_GL2_WORDS * 4u <= OOPS_GL_ATTRIB_SLOT_OFFSET &&
      OOPS_GL_ATTRIB_SLOT_OFFSET + OOPS_GL_ATTRIB_SLOTS * OOPS_GL_ATTRIB_SLOT_STRIDE <=
          OOPS_GL_PAYLOAD_BYTES &&
      /* The uniform block has to start after every descriptor set and end inside the
         slot. */
      (uint32_t)OOPS_GL_GL2_TEX_SETS * OOPS_GL_DESC_UNIT_STRIDE <=
          OOPS_GL_GL2_UNIFORM_AT &&
      OOPS_GL_GL2_UNIFORM_AT + (uint32_t)OOPS_GL_GL2_UNIFORM_FLOATS * 4u <=
          OOPS_GL_GL2_SLOT_STRIDE &&
      /* The draw constants sit in the last set's padding: after its 48 bytes of
       * descriptors, before the uniforms, and 4-aligned so a scalar load of four dwords
       * can name them. */
      ((uint32_t)OOPS_GL_GL2_TEX_SETS - 1u) * OOPS_GL_DESC_UNIT_STRIDE + 48u <=
          OOPS_GL_GL2_DRAWCONST_AT &&
      OOPS_GL_GL2_DRAWCONST_AT % 16u == 0u &&
      OOPS_GL_GL2_DRAWCONST_AT + (uint32_t)OOPS_GL_GL2_DRAWCONST_FLOATS * 4u <=
          OOPS_GL_GL2_UNIFORM_AT)
         ? 1
         : -1];

/* The general combine's encoder: the three instruction formats it emits, laid out field
 * by field as the RDNA2 ISA lays them out and checked, one line per form and operand
 * kind, against the words `tools/shader/combine.s` assembles to
 * (test_pm4_gl_combine_encoder_matches_the_assembler). A source is a VGPR as 256 + n,
 * an inline constant, or GL_PS_SRC_LITERAL with the literal's word after the
 * instruction. `vsrc1` is always a VGPR number. */
#define GL_PS_SRC_V(n) (256u + (uint32_t)(n))
#define GL_PS_SRC_ZERO 0x80u     /* 0 */
#define GL_PS_SRC_NEG_HALF 0xf1u /* -0.5 */
#define GL_PS_SRC_ONE 0xf2u      /* 1.0 */
#define GL_PS_SRC_TWO 0xf4u      /* 2.0 */
#define GL_PS_SRC_FOUR 0xf6u     /* 4.0 */
#define GL_PS_SRC_LITERAL 0xffu
enum {
    GL_PS_V_ADD = 0x03u,
    GL_PS_V_SUB = 0x04u,
    GL_PS_V_MUL = 0x08u,
    GL_PS_V_MIN = 0x0fu,
    GL_PS_V_MAX = 0x10u,
    GL_PS_V_FMAC = 0x2bu
};
static inline uint32_t gl_ps_vop2(uint32_t op, uint32_t vdst, uint32_t src0,
                                  uint32_t vsrc1) {
    return (op << 25) | (vdst << 17) | (vsrc1 << 9) | src0;
}
static inline uint32_t gl_ps_v_mov(uint32_t vdst, uint32_t src0) {
    return 0x7e000200u | (vdst << 17) | src0; /* VOP1 v_mov_b32 */
}
static inline uint32_t gl_ps_s_branch(uint32_t skip_words) {
    return 0xbf820000u |
           (skip_words & 0xffffu); /* SOPP, the offset counted from the next word */
}
/* The border colour table in the payload: one 16-byte entry of four floats, 256-byte
 * aligned as TA_BC_BASE_ADDR's 256-byte units need - a texture's own border colour when
 * it is none of the sampler's three built-in ones (gl_pack_descriptors). */
#define OOPS_GL_BORDER_TABLE_OFFSET 0xa00u
/*
 * The occlusion-query counters: sixteen 16-byte slots, one per render backend, each
 * holding the begin count at +0 and the end count at +8. That is radeonsi's layout
 * (`si_query.c`: the start event writes the slot, the stop event writes it plus eight),
 * and obSCEne measured it on this part: sixteen slots written, `enabled-rb-mask
 * 0xffff`, and the sum of `end - begin` equal to the pixels drawn.
 *
 * Sixteen is this part's number, not a general one. Some slots carry a zero difference
 * when the work does not reach them, so the answer is the sum and never one slot. Bit
 * 63 of each count is the hardware's own valid marker and is masked off.
 *
 * It lives in the payload, which is Onion (write-back, GPU-coherent), so the CPU reads
 * the results back without the uncached penalty a Garlic buffer would carry - and 0xd00
 * is clear of the four-parameter vertex shader above it, which ends at 0xc18.
 */
#define OOPS_GL_ZPASS_OFFSET 0xd00u
#define OOPS_GL_ZPASS_BACKENDS 16u
#define OOPS_GL_ZPASS_STRIDE 16u
void gl_ps_patch_alpha_test(gl_context_t *ctx);
/* The textured pixel shader, laid into `ps_tex` (OOPS_GL_PS_TEX_WORDS words) with its
 * canary store aimed at `canary_gpu` - what context creation writes at
 * OOPS_GL_PS_TEX_OFFSET. */
void gl_ps_build_textured(uint32_t *ps_tex, uint64_t canary_gpu);
/* The combine for the environment mode and the effective texture's base format: a word
 * a channel where that says it, and a generated program for GL_BLEND, GL_DECAL of RGBA
 * and GL_COMBINE. GL_FALSE only if a program outgrew the slot and modulates instead,
 * which GL's argument counts rule out. */
GLboolean gl_ps_patch_tex_env(gl_context_t *ctx);
GLboolean gl_ps_patch_tex_env_unit1(gl_context_t *ctx, GLboolean on);
/*
 * Which registers a combine stage reads, and whose state it combines with.
 *
 * Unit 0's stage takes its texel from the sample in v4..v7 and has no stage before it -
 * GL says GL_PREVIOUS at unit 0 is the primary colour - so its previous is v8..v11.
 * Unit 1's takes its texel from v28..v31, where tex-prolog2.s leaves it, and its
 * previous from v4..v7, where unit 0's stage left its result. Every stage writes
 * v4..v7, so they chain by running in order.
 *
 * The one thing neither stage can offer is GL 1.4's crossbar: after unit 0's stage, v4
 * holds its result, not its texel, so GL_TEXTURE0 named from unit 1 has nowhere to
 * read. It reads zero, which is what a unit applying no texture reads here and in Mesa.
 */
typedef struct {
    uint32_t texel; /* the unit's own sampled texel: 4 for unit 0, 28 for unit 1 */
    uint32_t prev;  /* GL_PREVIOUS: 8 for unit 0 (the primary colour), 4 for unit 1 */
    unsigned unit;  /* whose GL_TEXTURE_ENV_COLOR and combiner state */
} gl_ps_stage_t;
/* The general form's program for a combiner description, into `w`; answers its length
 * in words, which may exceed `cap` (nothing is written past it). */
size_t gl_ps_combine_program(const gl_context_t *ctx, const gl_ps_stage_t *st,
                             const gl_combine_t *cb, uint32_t *w, size_t cap);
/* The software rasteriser's texture environment for one unit: the colour `c` the unit
 * before left (the fragment's own for unit 0) combined, in place, with this unit's
 * texel `texels[unit]` as expanded for its base format `base`. `texels` holds every
 * unit's - zero for a unit applying none - for GL 1.4's GL_TEXTUREn sources, and
 * `primary` the fragment's own colour for GL_PRIMARY_COLOR. Public so the console's
 * combine programs can be checked against it. */
void gl_tex_env_apply(const gl_context_t *ctx, GLuint unit, GLenum base,
                      const gl_unit_texels_t *texels, const float primary[4],
                      float c[4]);
/* Fog into both pixel shaders, or out: the blend towards the fog colour by the factor
 * the vertex carries in its texture coordinate's z. Asked on every hardware draw;
 * writes only on a change. */
void gl_ps_patch_fog(gl_context_t *ctx);
/* The colour sum after texturing into the textured pixel shader's slot, or out
 * (tools/shader/colour-sum.s). A draw that sums reads the secondary colour from the
 * third parameter, so it runs the three-parameter vertex shader. Asked on every
 * textured hardware draw; writes only on a change. */
void gl_ps_patch_sum(gl_context_t *ctx, GLboolean on);
void gl_ps_patch_export(gl_context_t *ctx, GLboolean both);
/* The `GL_PS_EXPORT_WORDS`-long export tail for one colour target or two. Both forms
 * are the same length so either can be written over the other in place - see the
 * definition. */
const uint32_t *gl_ps_export_words(GLboolean both);
void gl_ps_patch_stipple(gl_context_t *ctx, GLboolean on);
void gl_ps_patch_unit1(gl_context_t *ctx, GLboolean on);
/* Which of the five sample forms the slot holds - the enumerators are not hardware
 * values; the `dim:` field and the instruction they choose are written out in
 * gl_ps_patch_sample. The last two are a depth texture read as itself and one read
 * through GL 1.4's comparison; both return a single value and spread it across v4..v7,
 * which is what `depth_mode` decides. */
typedef enum {
    GL_PS_SAMPLE_2D = 0,
    GL_PS_SAMPLE_3D = 1,
    GL_PS_SAMPLE_CUBE = 2,
    GL_PS_SAMPLE_DEPTH = 3,
    GL_PS_SAMPLE_SHADOW = 4
} gl_ps_sample_kind_t;
/* `depth_mode` is GL_TEXTURE_DEPTH_MODE's value and is read by the last two kinds only;
 * the other three ignore it. */
void gl_ps_patch_sample(gl_context_t *ctx, gl_ps_sample_kind_t kind, GLenum depth_mode);
/* Antialiasing's coverage in the untextured pixel shader, on or off. Asked on every
 * untextured hardware draw; writes only on a change. */
/* Which of the two pixel shaders smooths. The arithmetic is the same program either
 * way; only the interpolant it reads differs - `attr1` where there is no texture to
 * want it, `attr3`, the second unit's coordinate, where there is. */
typedef enum {
    GL_COVERAGE_OFF = 0,
    GL_COVERAGE_UNTEXTURED,
    GL_COVERAGE_TEXTURED,
    /* A smooth polygon, in whichever shader the draw uses: its three edge distances
     * always ride in `attr3`, so one program serves both and the kind does not name a
     * shader. */
    GL_COVERAGE_POLYGON_UNTEX,
    GL_COVERAGE_POLYGON_TEX
} gl_coverage_kind_t;
void gl_ps_patch_coverage(gl_context_t *ctx, GLboolean on);
void gl_ps_patch_coverage_where(gl_context_t *ctx, gl_coverage_kind_t kind);
/* Whether this draw is stippled: GL applies the polygon stipple to filled polygons only
 * - not to a polygon's outline, nor to the lines and points a primitive becomes
 * (3.5.2). The software rasteriser asks the same question in gl_raster_triangle. */
static inline GLboolean gl_polygon_stipple_on(const gl_context_t *ctx) {
    return (GLboolean)(ctx->cap_polygon_stipple && ctx->prim_raster == GL_FILL);
}
/* The three-parameter vertex shader, laid into `vs` (OOPS_GL_VS_P3_WORDS words) with
 * its vertex buffer and canary addresses - tools/shader/vs-param3.s's words. */
void gl_vs_build_param3(uint32_t *vs, uint64_t vbo_gpu, uint64_t canary_gpu);
void gl_vs_build_param4(uint32_t *vs, uint64_t vbo_gpu, uint64_t canary_gpu);
/* The version string rebuilt from the context's major and minor (gl_state.c). */
void gl_version_string(gl_context_t *ctx);
/* The version a context has before the program states its own. 1.1 is what is
 * implemented everywhere, and a build serving ports that all expect a later one can
 * move it rather than patching each of them.
 *
 * 2.0 is never the default: the version gates the API, and the opt-in keeps a GL 1.x
 * program from reaching the programmable pipeline by accident. A GL 2.0 program calls
 * `glContextSetVersion(2, 0)`. */
#ifndef OOPS_GL_DEFAULT_VERSION_MAJOR
#define OOPS_GL_DEFAULT_VERSION_MAJOR 1u
#endif
#ifndef OOPS_GL_DEFAULT_VERSION_MINOR
#define OOPS_GL_DEFAULT_VERSION_MINOR 1u
#endif

/* Buffer objects. `gl_find_buffer` answers NULL for name 0 and for a name never
 * generated.
 *
 * `gl_array_base` is the one place an array's effective address is worked out: client
 * memory straight through, or the buffer's storage plus the offset the `pointer` field
 * is carrying. It answers NULL when the named buffer has gone or has no storage, which
 * stops the reader rather than letting it walk an address computed from a freed
 * pointer. */
gl_buffer_object_t *gl_find_buffer(gl_context_t *ctx, GLuint name);
/* Releases every buffer object's storage. Called from glContextDestroy, and living
 * beside the allocation in gl_state.c so the choice of allocator stays in one file. */
void gl_free_all_buffers(gl_context_t *ctx);
const uint8_t *gl_array_base(const gl_context_t *ctx, const gl_client_array_t *a);

/* Display lists.
 *
 * Every entry point the specification compiles into a list begins
 *
 *     if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_..., args...)) return;
 *
 * `gl_list_recording` is the ordinary path's whole cost - one load, and the arguments
 * are not even built when it is false. `gl_list_rec` appends the call and answers
 * whether the caller should stop: under GL_COMPILE always, and under
 * GL_COMPILE_AND_EXECUTE too, because it has already executed the call by replaying
 * the command it just recorded with recording suspended. That one rule is what makes
 * the recorder safe to put anywhere: an entry point that is recorded and then calls
 * another recorded entry point in its body records once, not twice, because the body
 * only ever runs suspended or with no list open.
 *
 * `gl_list_rec` copies `bytes` of `data` into the list; `gl_list_rec_owned` takes a
 * block the caller already allocated with `gl_list_alloc` (an image unpacked at compile
 * time), and frees it if the record fails. Either answers false, having recorded
 * nothing, when it cannot allocate - GL_OUT_OF_MEMORY is set, and under
 * GL_COMPILE_AND_EXECUTE the caller then executes normally. */
/* True while a list is compiling, and also while a capture is running.
 *
 * The two want the same hooks and differ in what happens afterwards: a compiling
 * list swallows the call, a capture records it and lets it through. `gl_list_rec_owned`
 * decides which by looking at `list_compiling` again, so the only thing this gate has
 * to do is let the recorder see the call at all. */
static inline GLboolean gl_list_recording(void) {
    const gl_context_t *ctx = g_gl_ctx;
    return (GLboolean)(ctx && (ctx->list_compiling != 0u || ctx->capture_active) &&
                       ctx->list_suspend == 0u);
}
GLboolean gl_list_rec(gl_list_op_t op, const gl_list_arg_t *args, int nargs,
                      const void *data, size_t bytes);
/* `bytes` describes `owned`, because a capture has to write the blob down. The list
   itself does not need it - replay recomputes what it wants from the arguments. */
GLboolean gl_list_rec_owned(gl_list_op_t op, const gl_list_arg_t *args, int nargs,
                            void *owned, size_t bytes);
/* Called by the swap, once per frame: starts an armed capture and finishes a running
 * one. */
void gl_capture_swap_tick(gl_context_t *ctx);
void *gl_list_alloc(size_t bytes);
/* An image's rows as `glPixelStorei` lays them out now, packed tight into a new block -
 * what a list must keep, because the unpack state it replays under will be different.
 * NULL for no pixels or no rows, and also when it cannot allocate (GL_OUT_OF_MEMORY
 * set). */
void *gl_list_pack_rows(const void *src, size_t row_bytes, size_t src_stride,
                        GLsizei rows);
/* Records a call carrying a `format`/`type` image - the texture uploads and
 * glDrawPixels - with the image packed by gl_list_pack_rows. A format this cannot read
 * records no pixels, and the replay then refuses exactly as the call would have; a
 * failed allocation records nothing. */
GLboolean gl_list_rec_image(gl_list_op_t op, const gl_list_arg_t *args, int nargs,
                            GLsizei width, GLsizei height, GLenum format, GLenum type,
                            const GLvoid *pixels);
/* Frees every list's storage; glContextDestroy. */
void gl_list_free_all(gl_context_t *ctx);

/* Evaluators (gl_eval.c). `gl_eval_cap` answers the enable flag behind an evaluator
 * capability - a map of either dimension, or GL_AUTO_NORMAL - and NULL for any other,
 * so glEnable, glDisable and glIsEnabled share one list. `gl_eval_replay_map` runs a
 * compiled glMap1 or glMap2. */
GLboolean *gl_eval_cap(gl_context_t *ctx, GLenum cap);
void gl_eval_init(gl_context_t *ctx);
void gl_eval_free(gl_context_t *ctx);
void gl_eval_replay_map(const gl_list_cmd_t *cmd);

/* Selection and feedback (gl_select.c): what primitive assembly calls instead of
 * drawing when the render mode is not GL_RENDER. Each takes the vertices as assembled -
 * object coordinates, before any line or point is widened into triangles - and does its
 * own transform, lighting, clipping and culling. `pv` is the provoking vertex, as for
 * drawing. */
static inline GLboolean gl_fb_active(const gl_context_t *ctx) {
    return (GLboolean)(ctx->render_mode != GL_RENDER);
}
void gl_fb_polygon_tri(gl_context_t *ctx, const gl_vertex_t *v0, const gl_vertex_t *v1,
                       const gl_vertex_t *v2, unsigned edges, GLboolean use_flags,
                       const gl_vertex_t *pv);
void gl_fb_line(gl_context_t *ctx, const gl_vertex_t *a, const gl_vertex_t *b,
                const gl_vertex_t *pv);
void gl_fb_point(gl_context_t *ctx, const gl_vertex_t *p);
/* glRasterPos in GL_SELECT: a valid position is a hit at its depth. */
void gl_fb_raster_hit(gl_context_t *ctx);
/* glBitmap, glDrawPixels and glCopyPixels in GL_FEEDBACK: the token, then the raster
 * position as a vertex. */
void gl_fb_pixel_token(gl_context_t *ctx, GLenum token);

/* Pixel transfer on an RGBA8 colour (gl_raster.c): scale and bias, the colour maps, the
 * clamp. `gl_pixel_transfer_active` is false for the default state - scales 1, biases
 * 0, no map - and while suspended, and the callers test it once per rectangle so the
 * untransferred path costs nothing per pixel. */
GLboolean gl_pixel_transfer_active(const gl_context_t *ctx);
void gl_pixel_transfer_rgba8(const gl_context_t *ctx, uint8_t rgba[4]);

/* Pixel rectangles in client memory (gl_pixel.c): what a format and type describe,
 * where a rectangle's rows and slices are under glPixelStorei, and each pixel converted
 * to and from RGBA floats.
 *
 * `gl_pixel_fmt` answers GL_NO_ERROR, GL_INVALID_ENUM for a format or type this does
 * not read, or GL_INVALID_OPERATION for a packed type with a format it does not fit -
 * GL 1.2's rule. */
typedef struct {
    GLenum format, type;
    /* GL_COLOR for the colour formats, GL_DEPTH for GL_DEPTH_COMPONENT, GL_STENCIL for
     * GL_STENCIL_INDEX, GL_COLOR_INDEX for colour indices - which an RGBA context takes
     * in, through the index maps, and never gives out. A call that cannot take one
     * refuses it itself. */
    GLenum kind;
    int components;     /* values per pixel the format names */
    size_t elem_bytes;  /* one element: a component, or a whole packed pixel */
    size_t pixel_bytes; /* one pixel - 0 under GL_BITMAP, whose pixels are bits */
    GLboolean packed;
    GLboolean bitmap; /* GL_BITMAP: an index a bit, rows of bytes as glBitmap's */
} gl_pixel_fmt_t;
GLenum gl_pixel_fmt(GLenum format, GLenum type, gl_pixel_fmt_t *out);

typedef struct {
    const uint8_t *base; /* pixel (0, 0) of slice 0, the skips applied */
    size_t row_stride, image_stride;
    GLboolean swap;
} gl_pixel_src_t;
typedef struct {
    uint8_t *base;
    size_t row_stride, image_stride;
    GLboolean swap;
} gl_pixel_dst_t;
void gl_unpack_source(const gl_context_t *ctx, const gl_pixel_fmt_t *f,
                      const void *pixels, GLsizei width, GLsizei height,
                      gl_pixel_src_t *out);
void gl_pack_dest(const gl_context_t *ctx, const gl_pixel_fmt_t *f, void *pixels,
                  GLsizei width, GLsizei height, gl_pixel_dst_t *out);
/* One pixel to RGBA, expanded by the format (absent colour 0, absent alpha 1). */
void gl_unpack_pixel_f(const gl_pixel_fmt_t *f, const uint8_t *src, GLboolean swap,
                       float out[4]);
/* One pixel from RGBA. Luminance is R + G + B clamped when `lum_sum` - a colour buffer
 * read - and R otherwise - a texture read back. */
void gl_pack_pixel_f(const gl_pixel_fmt_t *f, const float rgba[4], GLboolean lum_sum,
                     GLboolean swap, uint8_t *dst);
/* One depth or stencil value (GL_DEPTH_COMPONENT, GL_STENCIL_INDEX) in and out: a depth
 * as a fraction of one through GL_DEPTH_SCALE/BIAS and clamped, a stencil index as the
 * integer through the index shift and offset and GL_MAP_STENCIL's map -
 * gl_stencil_transfer. */
float gl_unpack_value(const gl_context_t *ctx, const gl_pixel_fmt_t *f,
                      const uint8_t *row, int x, GLboolean swap);
void gl_pack_value(const gl_context_t *ctx, const gl_pixel_fmt_t *f, float v,
                   GLboolean swap, uint8_t *dst);
int64_t gl_stencil_transfer(const gl_context_t *ctx, int64_t s);
/* A GL_COLOR_INDEX pixel as RGBA, as Mesa's _mesa_unpack_color_index_to_rgba_float
 * converts one (main/pack.c:1500-1548): the index through GL_INDEX_SHIFT and
 * GL_INDEX_OFFSET, then each component from its GL_PIXEL_MAP_I_TO_R/G/B/A table -
 * indexed by the value masked to the table's size - and clamped. No RGBA scale, bias or
 * colour map after: the index maps are that. */
void gl_unpack_index_rgba(const gl_context_t *ctx, const gl_pixel_fmt_t *f,
                          const uint8_t *row, int x, GLboolean swap, float out[4]);
/* One index's RGBA from its integer, the same steps; and a GL_BITMAP pixel's bit in and
 * out, under the unpack and pack skip-pixels and LSB-first state. */
void gl_index_to_rgba(const gl_context_t *ctx, int64_t i, float out[4]);
GLboolean gl_unpack_bit(const gl_context_t *ctx, const uint8_t *row, int x);
void gl_pack_bit(const gl_context_t *ctx, uint8_t *row, int x, GLboolean v);
/* Whether a draw would read a mapped buffer object - an enabled array's, or the element
 * buffer when `elements` - which GL 1.5 refuses with GL_INVALID_OPERATION (gl_state.c).
 */
GLboolean gl_draw_sources_mapped(gl_context_t *ctx, GLboolean elements);
/* One source row stored as texels of base format `base`: RGBA8 through the pixel
 * transfer and rebased for a colour texture (gl_unpack_row, gl_tex_rebase_row), 32-bit
 * floats through the depth transfer for a depth texture. What every upload stores. */
void gl_tex_store_row(const gl_context_t *ctx, const gl_pixel_fmt_t *f, uint8_t *dst,
                      const uint8_t *src, GLsizei width, GLboolean swap, GLenum base);
/* One source row into RGBA8 texels through the pixel transfer - what every upload
 * stores. */
void gl_unpack_row(const gl_context_t *ctx, const gl_pixel_fmt_t *f, uint8_t *dst,
                   const uint8_t *src, GLsizei width, GLboolean swap);
/* A client image copied for a display list: `depth` slices of `height` rows, tight,
 * bytes in native order - so it replays under neutral unpack state. NULL for no pixels;
 * NULL with GL_OUT_OF_MEMORY set when it cannot allocate. */
void *gl_pixel_copy_client(const gl_context_t *ctx, const gl_pixel_fmt_t *f,
                           const void *pixels, GLsizei width, GLsizei height,
                           GLsizei depth);
/* The size of what that produced, for a caller that has to write the blob down. */
size_t gl_pixel_packed_bytes(const gl_pixel_fmt_t *f, GLsizei width, GLsizei height,
                             GLsizei depth);
/* Bit (x, y) of a client bitmap - glBitmap, glPolygonStipple - under the unpack state:
 * row length, alignment, skips, and GL_UNPACK_LSB_FIRST. */
GLboolean gl_bitmap_bit(const gl_context_t *ctx, const GLubyte *bits, GLsizei width,
                        int x, int y);
/* A bitmap copied for a display list, tight and most significant bit first. */
void *gl_bitmap_copy_client(const gl_context_t *ctx, const GLubyte *bits, GLsizei width,
                            GLsizei height);
/* The unpack state set neutral for a list's replay - its images were stored tight and
 * native - and put back after. */
typedef struct {
    GLint alignment, row_length, image_height, skip_rows, skip_pixels, skip_images;
    GLboolean swap_bytes, lsb_first;
} gl_unpack_saved_t;
void gl_unpack_neutral(gl_context_t *ctx, gl_unpack_saved_t *saved);
void gl_unpack_restore(gl_context_t *ctx, const gl_unpack_saved_t *saved);
/* Pixel transfer on floats: the path an RGBA8 colour takes too (gl_raster.c). */
void gl_pixel_transfer_rgbaf(const gl_context_t *ctx, float rgba[4]);

/* A texture internal format's base format - GL_ALPHA, GL_LUMINANCE, GL_LUMINANCE_ALPHA,
 * GL_INTENSITY, GL_RGB or GL_RGBA - or 0 for one GL 1.x does not define (gl_pixel.c).
 */
GLenum gl_tex_base_format(GLint internalformat);
/* RGBA8 texels as a base format keeps them, in place: the components it drops set to
 * what sampling reads for them - (0, 0, 0, A), (L, L, L, 1), (L, L, L, A), (I, I, I,
 * I), (R, G, B, 1) - which is also what a shader's texture lookup of one returns. */
void gl_tex_rebase_row(uint8_t *rgba, GLsizei width, GLenum base);
/* The same for one colour in floats: a border colour, which the sampler meets as a
 * texel. */
void gl_tex_rebase_f(float c[4], GLenum base);
/* Stored texels as glGetTexImage reports them: luminance and intensity in red alone,
 * green and blue zero (Mesa, main/texgetimage.c:289-301). In place. */
void gl_tex_readback_row(uint8_t *rgba, GLsizei width, GLenum base);

/* The accumulation buffer (gl_raster.c): glClear's GL_ACCUM_BUFFER_BIT, and the
 * storage's release at glContextDestroy. */
void gl_accum_clear(gl_context_t *ctx);
void gl_accum_free(gl_context_t *ctx);
/* How many values a `*v` call reads for this `pname` - 4 for a colour, position or
 * plane, 3 for a direction, 1 for a scalar - so compiling one copies exactly what the
 * caller passed and never reads past the end of a one-element array. The same per-pname
 * sizing Mesa's `save_*v` functions use (`main/dlist.c`). */
int gl_list_param_count(gl_list_op_t op, GLenum pname);

/* Records a `*v` call whose leading enums are `e0` (and `e1`, for the two-enum forms)
 * and whose values follow them in the command, copying `gl_list_param_count` of them.
 */
static inline GLboolean gl_list_rec_fv(gl_list_op_t op, GLenum e0, GLenum e1,
                                       GLboolean two, GLenum pname, const GLfloat *v) {
    gl_list_arg_t a[6];
    int n = 0;
    a[n++] = gl_la_e(e0);
    if (two)
        a[n++] = gl_la_e(e1);
    const int count = v ? gl_list_param_count(op, pname) : 0;
    for (int i = 0; i < 4; i++)
        a[n + i] = gl_la_f((v && i < count) ? v[i] : 0.0f);
    return gl_list_rec(op, a, n + 4, (const void *)0, 0u);
}
static inline GLboolean gl_list_rec_iv(gl_list_op_t op, GLenum e0, GLenum e1,
                                       GLboolean two, GLenum pname, const GLint *v) {
    gl_list_arg_t a[6];
    int n = 0;
    a[n++] = gl_la_e(e0);
    if (two)
        a[n++] = gl_la_e(e1);
    const int count = v ? gl_list_param_count(op, pname) : 0;
    for (int i = 0; i < 4; i++)
        a[n + i] = gl_la_i((v && i < count) ? v[i] : 0);
    return gl_list_rec(op, a, n + 4, (const void *)0, 0u);
}

#define GL_LIST_ARGV(...) ((const gl_list_arg_t[]){__VA_ARGS__})
#define GL_LIST_ARGC(...)                                                              \
    ((int)(sizeof((gl_list_arg_t[]){__VA_ARGS__}) / sizeof(gl_list_arg_t)))
#define GL_LIST_REC(op, ...)                                                           \
    gl_list_rec((op), GL_LIST_ARGV(__VA_ARGS__), GL_LIST_ARGC(__VA_ARGS__),            \
                (const void *)0, 0u)
#define GL_LIST_REC0(op)                                                               \
    gl_list_rec((op), (const gl_list_arg_t *)0, 0, (const void *)0, 0u)

/* Freestanding math primitives */
float gl_sin(float rad);
float gl_cos(float rad);
float gl_tan(float rad);
float gl_sqrt(float val);
float gl_pow(float base, float exp);
float gl_exp(float x);

/* The per-fragment operations' state that is fixed for a primitive (gl_draw.c,
 * gl_frag_ops_init): the stencil test, blending and the logic op. */
typedef struct {
    GLboolean stencil_test;
    GLenum stencil_func;
    GLint stencil_ref;
    uint32_t stencil_vmask, stencil_wmask;
    GLenum stencil_op_fail, stencil_op_zfail, stencil_op_zpass;
    GLboolean blend, logic_on;
    uint32_t logic_mode;
} gl_frag_ops_t;

/* A pixel rectangle's fragments (glDrawPixels, glBitmap, glCopyPixels): what is
 * constant across the rectangle - the raster z, the one texel its fixed texture
 * coordinate samples, the fog factor of its raster distance, the scissor box in window
 * coordinates - set by gl_pixel_frags_begin; gl_pixel_fragment then takes each fragment
 * through the texture environment, fog and the per-fragment operations. */
typedef struct {
    gl_frag_ops_t ops;
    GLboolean depth_test;
    float z;
    /* Per unit: the texture the unit applies (NULL for none), its base format, and the
     * one texel the raster position's coordinate samples. */
    const gl_texture_object_t *tex[OOPS_GL_MAX_TEXTURE_UNITS];
    GLenum tex_format[OOPS_GL_MAX_TEXTURE_UNITS];
    gl_unit_texels_t texel;
    GLboolean fog_on;
    float fog_f;
    int sc_x0, sc_y0, sc_x1, sc_y1;
} gl_pixel_frags_t;

void gl_pixel_frags_begin(gl_context_t *ctx, gl_pixel_frags_t *pf);
void gl_pixel_fragment(gl_context_t *ctx, const gl_pixel_frags_t *pf, int x, int y,
                       const float rgba[4]);

/* 1/q for the projective divide - 1 for a q of zero, whose divide GL leaves undefined
 * and which is kinder left undivided than sent to infinity. */
static inline float gl_q_inv(float q) {
    return (q != 0.0f) ? 1.0f / q : 1.0f;
}

/* The level-of-detail bias a texture is sampled with (GL 1.4, 3.8.8): its own plus the
 * unit's, clamped to GL_MAX_TEXTURE_LOD_BIAS - the sum Mesa's state tracker forms
 * (state_tracker/st_atom_sampler.c:97). Added to the level of detail before the
 * GL_TEXTURE_MIN_LOD/MAX_LOD clamp. `tu` is the unit sampling it. */
static inline float gl_tex_lod_bias(const gl_tex_unit_t *tu,
                                    const gl_texture_object_t *tex) {
    float b = tex->lod_bias + tu->tex_lod_bias;
    if (b > OOPS_GL_MAX_TEXTURE_LOD_BIAS)
        b = OOPS_GL_MAX_TEXTURE_LOD_BIAS;
    if (b < -OOPS_GL_MAX_TEXTURE_LOD_BIAS)
        b = -OOPS_GL_MAX_TEXTURE_LOD_BIAS;
    return b;
}

/* That bias as SQ_IMG_SAMP_WORD2's LOD_BIAS field: bits 0-13 (gfx10-rsrc.json:488),
 * signed fixed point with eight fraction bits, as radeonsi encodes it for GFX10
 * (ac_descriptors.c:144- 145, util_signed_fixed(bias, 8)). The clamp above keeps it
 * inside the field's [-32, 32). A bias of 0 is no bits, which leaves every descriptor
 * drawn before it as it was. */
static inline uint32_t gl_hw_lod_bias_bits(float bias) {
    return (uint32_t)(int32_t)(bias * 256.0f) & 0x3fffu;
}

/* Whether the colour sum runs - the secondary colour added to the primary after
 * texturing, before fog. GL_COLOR_SUM turns it on, and lighting applies it regardless
 * (GL 1.4, 3.9), which is also Mesa's test (main/ff_fragment_shader.c:69-78). Under
 * lighting the secondary colour is the specular term GL_SEPARATE_SPECULAR_COLOR keeps
 * apart, and zero under GL_SINGLE_COLOR - so the sum is only worth doing when one of
 * the two holds. */
static inline GLboolean gl_color_sum_on(const gl_context_t *ctx) {
    if (ctx->cap_lighting) {
        return (GLboolean)(ctx->light_model_color_control ==
                           GL_SEPARATE_SPECULAR_COLOR);
    }
    return ctx->cap_color_sum;
}

/* The fog factor for an eye-space distance: 1 is unfogged, 0 is fully the fog colour.
 *
 * The three modes are the specification's. Linear divides by (end - start), so an end
 * equal to the start would divide by zero - that case gives 0, which is the
 * fully-fogged answer the limit approaches from either side, rather than an infinity
 * that poisons the colour. */
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
    if (f < 0.0f)
        f = 0.0f;
    if (f > 1.0f)
        f = 1.0f;
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
/* Sets each unit's `v->tc` from the vertex's own texture coordinates `tcs` (s, t, r, q
 * as they arrived - glTexCoord's and glMultiTexCoord's current values, or the arrays'
 * elements), after that unit's generation and texture matrix. `gl_vertex_texcoord4` is
 * one unit's, into `out` - for a caller that keeps them elsewhere, like the raster
 * position. */
void gl_vertex_texcoord(const gl_context_t *ctx, gl_vertex_t *v,
                        float tcs[OOPS_GL_MAX_TEXTURE_UNITS][4]);
void gl_vertex_texcoord4(const gl_context_t *ctx, GLuint unit, const gl_vertex_t *v,
                         const float tc[4], float out[4]);

/* The UCP_ENA_0..5 bits of PA_CL_CLIP_CNTL (0x204), bits 0..5 per
 * mesa/src/amd/registers/gfx103.json. */
static inline uint32_t gl_compute_clip_cntl(const gl_context_t *ctx) {
    uint32_t v = 0u;
    if (!ctx)
        return 0u;
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        if (ctx->clip_plane_enabled[i])
            v |= (1u << i);
    }
    return v;
}

/* The plane the hardware clipper wants, which is clip space, not eye space.
 *
 * mesa/src/mesa/main/clip.c:40-51 multiplies the eye plane by the projection's inverse,
 * because a plane transforms by the inverse of the transform its points take.
 * Mesa passes the eye-space form only when a vertex shader writes a clip vertex
 * (st_atom_clip.c:52-55); these shaders do not, so the fixed-function clipper applies
 * and it reads clip space.
 *
 * Returns false when the projection cannot be inverted, leaving `out` alone - the
 * caller then writes nothing rather than a plane made of infinities. */
GLboolean gl_compute_clip_plane_hw(const gl_context_t *ctx, int i, float out[4]);
void gl_hw_emit_clip_planes(const gl_context_t *ctx, uint32_t **dw_ptr);
/* The binding a glTex* call names, in the active unit, as a pointer into the context so
 * callers can read and write it. NULL for a target this implementation does not have;
 * the caller reports GL_INVALID_ENUM. */
static inline GLuint *gl_binding_slot(gl_context_t *ctx, GLenum target) {
    if (!ctx)
        return NULL;
    gl_tex_unit_t *tu = gl_tu(ctx);
    if (target == GL_TEXTURE_2D)
        return &tu->bound_texture_2d;
    if (target == GL_TEXTURE_1D)
        return &tu->bound_texture_1d;
    if (target == GL_TEXTURE_3D)
        return &tu->bound_texture_3d;
    /* A cube map's faces are images of the one object GL_TEXTURE_CUBE_MAP binds. */
    if (target == GL_TEXTURE_CUBE_MAP || GL_CUBE_FACE_INDEX(target) >= 0) {
        return &tu->bound_texture_cube;
    }
    return NULL;
}

static inline GLuint gl_default_texture_id(GLenum target) {
    if (target == GL_TEXTURE_1D)
        return OOPS_GL_DEFAULT_TEXTURE_1D;
    if (target == GL_TEXTURE_3D)
        return OOPS_GL_DEFAULT_TEXTURE_3D;
    if (target == GL_TEXTURE_CUBE_MAP || GL_CUBE_FACE_INDEX(target) >= 0) {
        return OOPS_GL_DEFAULT_TEXTURE_CUBE;
    }
    return OOPS_GL_DEFAULT_TEXTURE_2D;
}

/* The texture a draw samples, which is not the same question as the one above.
 *
 * When more than one target is enabled the specification says the highest
 * dimensionality wins, so 2D beats 1D and a program can leave a 1D texture bound while
 * drawing with a 2D one. Answering this with "whatever is bound to 2D" would give the
 * wrong texture to a program using 1D, and answering it with "whichever was bound last"
 * would give a different wrong one. */
/* One level of a texture as the sampler sees it: the base level from the object's own
 * fields (rows `pitch` pixels apart), a mip level from `mips` (rows packed tight).
 * False when that level holds no image. */
typedef struct {
    GLsizei width, height, depth;
    size_t pitch; /* pixels */
    size_t slice; /* pixels from one slice of a 3D level to the next: pitch * height */
    const uint8_t *pixels;
    GLint internal_format;
    GLenum base_format;
} gl_tex_view_t;

/* Level `level` of cube face `face` (0..5), RGBA8 rows packed tight. */
static inline GLboolean gl_tex_face_view(const gl_texture_object_t *tex, int face,
                                         int level, gl_tex_view_t *out) {
    if (!tex || !tex->cube || face < 0 || face > 5 || level < 0 ||
        level >= OOPS_GL_MAX_TEXTURE_LEVELS) {
        return GL_FALSE;
    }
    const gl_tex_level_t *lv = &tex->cube[face * OOPS_GL_MAX_TEXTURE_LEVELS + level];
    out->width = lv->width;
    out->height = lv->height;
    out->depth = 1;
    out->pitch = (size_t)lv->width;
    out->pixels = (const uint8_t *)lv->pixels;
    out->internal_format = lv->internal_format;
    out->base_format = lv->base_format;
    out->slice = out->pitch * (size_t)(out->height > 0 ? out->height : 0);
    return (GLboolean)(out->pixels && out->width > 0 && out->height > 0);
}

/* A level of any texture; for a cube map, of its +X face, which the questions asked
 * without a face - the size a level of detail is measured by, the format sampling meets
 * - are answered from, the six being alike in a complete cube map. */
static inline GLboolean gl_tex_level_view(const gl_texture_object_t *tex, int level,
                                          gl_tex_view_t *out) {
    if (!tex || level < 0 || level >= OOPS_GL_MAX_TEXTURE_LEVELS)
        return GL_FALSE;
    if (tex->cube)
        return gl_tex_face_view(tex, 0, level, out);
    if (level == 0) {
        out->width = tex->width;
        out->height = tex->height;
        out->depth = tex->depth > 0 ? tex->depth : 1;
        out->pitch = tex->pitch ? (size_t)tex->pitch : (size_t)tex->width;
        out->pixels = (const uint8_t *)tex->pixels;
        out->internal_format = tex->internal_format;
        out->base_format = tex->base_format;
    } else {
        out->width = tex->mips[level].width;
        out->height = tex->mips[level].height;
        out->depth = tex->mips[level].depth > 0 ? tex->mips[level].depth : 1;
        out->pitch = (size_t)tex->mips[level].width;
        out->pixels = (const uint8_t *)tex->mips[level].pixels;
        out->internal_format = tex->mips[level].internal_format;
        out->base_format = tex->mips[level].base_format;
    }
    out->slice = out->pitch * (size_t)(out->height > 0 ? out->height : 0);
    return (GLboolean)(out->pixels && out->width > 0 && out->height > 0);
}

static inline GLboolean gl_filter_uses_mipmaps(GLenum f) {
    return (GLboolean)(f == GL_NEAREST_MIPMAP_NEAREST ||
                       f == GL_LINEAR_MIPMAP_NEAREST || f == GL_NEAREST_MIPMAP_LINEAR ||
                       f == GL_LINEAR_MIPMAP_LINEAR);
}

/* The last level a complete texture samples: q = min(p, GL_TEXTURE_MAX_LEVEL), where
 * p = GL_TEXTURE_BASE_LEVEL + floor(log2(max(w, h, d))) of the base level's image
 * (GL 1.2, 3.8.8) - and no further than the levels there are. Below the base level when
 * MAX_LEVEL is, which only a mipmapped texture minds (gl_texture_complete). */
static inline int gl_tex_top_level(const gl_texture_object_t *tex) {
    const int b = tex->base_level;
    gl_tex_view_t bv;
    if (!gl_tex_level_view(tex, b, &bv))
        return b;
    int m = (bv.width > bv.height) ? bv.width : bv.height;
    if (bv.depth > m)
        m = bv.depth;
    int q = b;
    while (m > 1) {
        m >>= 1;
        q++;
    }
    if (q > tex->max_level)
        q = tex->max_level;
    if (q > OOPS_GL_MAX_TEXTURE_LEVELS - 1)
        q = OOPS_GL_MAX_TEXTURE_LEVELS - 1;
    return q;
}

/* The base format sampling meets: the base level's (GL_TEXTURE_BASE_LEVEL's, not level
 * 0's), GL_RGBA when there is none. */
static inline GLenum gl_tex_sample_format(const gl_texture_object_t *tex) {
    gl_tex_view_t bv;
    if (tex && gl_tex_level_view(tex, tex->base_level, &bv) && bv.base_format) {
        /* A depth texture's texels read as GL_DEPTH_TEXTURE_MODE says (GL 1.4, 3.8.14).
         */
        if (bv.base_format == GL_DEPTH_COMPONENT)
            return tex->depth_mode;
        return bv.base_format;
    }
    return (GLenum)GL_RGBA;
}

/* Texture completeness, with GL 1.2's base and maximum levels (Mesa,
 * main/texobj.c:729-760 and :881). A texture is complete when its base level -
 * GL_TEXTURE_BASE_LEVEL, which must name a level there is - holds an image and, if its
 * minification filter reads mipmaps, the maximum level is not below the base and every
 * level from the base to the top level (gl_tex_top_level) holds one of exactly the
 * halved size and the base level's internal format. An incomplete texture is as if
 * texturing were off - the fragment takes the untextured colour - which is GL's
 * answer and the reason a program that forgets to set a non-mipmap filter draws
 * untextured on every implementation. Setting GL_TEXTURE_MAX_LEVEL to the last level it
 * uploaded is how a program keeps a short chain complete. */
static inline GLboolean gl_texture_complete(const gl_texture_object_t *tex) {
    const int b = tex->base_level;
    if (b < 0 || b >= OOPS_GL_MAX_TEXTURE_LEVELS)
        return GL_FALSE;
    /* A cube map is cube complete when all six faces' base images exist, square, of
     * one size and one internal format (Mesa, main/texobj.c:800-830) - and each face
     * mipmap complete in the same way when the filter reads mipmaps. The +X face stands
     * for the rest below. */
    const int faces = tex->cube ? 6 : 1;
    gl_tex_view_t base;
    if (!gl_tex_level_view(tex, b, &base))
        return GL_FALSE;
    for (int f = 1; f < faces; f++) {
        gl_tex_view_t fv;
        if (!gl_tex_face_view(tex, f, b, &fv) || fv.width != base.width ||
            fv.height != base.height || fv.internal_format != base.internal_format) {
            return GL_FALSE;
        }
    }
    if (!gl_filter_uses_mipmaps(tex->min_filter))
        return GL_TRUE;
    if (tex->max_level < b)
        return GL_FALSE;
    const int top = gl_tex_top_level(tex);
    for (int f = 0; f < faces; f++) {
        for (int i = b + 1; i <= top; i++) {
            gl_tex_view_t lv;
            const int k = i - b;
            const GLboolean has = tex->cube ? gl_tex_face_view(tex, f, i, &lv)
                                            : gl_tex_level_view(tex, i, &lv);
            if (!has)
                return GL_FALSE;
            const GLsizei ew = (base.width >> k) > 0 ? (base.width >> k) : 1;
            const GLsizei eh = (base.height >> k) > 0 ? (base.height >> k) : 1;
            const GLsizei ed = (base.depth >> k) > 0 ? (base.depth >> k) : 1;
            if (lv.width != ew || lv.height != eh || lv.depth != ed)
                return GL_FALSE;
            if (lv.internal_format != base.internal_format)
                return GL_FALSE;
        }
    }
    return GL_TRUE;
}

/* Gives back a texture's mip levels and its hardware chain (not its base level);
 * glDeleteTextures, glContextDestroy. The caller has synchronised with any built frame.
 */
void gl_tex_free_mips(gl_texture_object_t *tex);

/* Repacks a texture's descriptors after its parameters changed outside glTexParameter -
 * the attribute stack's pop. */
void gl_tex_repack(gl_texture_object_t *tex);

/* Brings a texture's hardware image up to date before a draw samples it: builds or
 * rebuilds the mip chain when the texture reads mipmaps, and repacks the descriptors
 * when what they should describe changed. May submit the frame (to free a chain a built
 * draw still names); the caller reopens it. */
void gl_tex_hw_prepare(gl_context_t *ctx, gl_texture_object_t *tex);

/* The linear GFX10 mip layout, addrlib's (gfx10addrlib.cpp:5082-5104, GetMipSize at
 * gfx10addrlib.h:367-383): level i is ceil(w / 2^i) by ceil(h / 2^i) by ceil(d / 2^i),
 * its rows padded to 64 RGBA8 texels (256 bytes), its slices one after another at
 * `pitch * height`, and the levels placed smallest first. Fills each level's byte
 * offset and row pitch in texels, and answers the whole chain's size in bytes.
 *
 * `d` is 1 for a 2D texture, which reduces this to the two-dimensional form. A volume's
 * depth halves with its width and height. gl1-probe's `texture-3d` covers the slice
 * stride on hardware (slice 1 of a 2x2x2 volume at `pitch * height` from slice 0), and
 * `mipmap-levels` the smallest-first level placement. */
static inline size_t gl_tex_chain_layout_3d(GLsizei w, GLsizei h, GLsizei d, int levels,
                                            size_t offsets[], uint32_t pitches[]) {
    size_t total = 0;
    for (int i = levels - 1; i >= 0; i--) {
        const uint32_t mw = ((uint32_t)w + (1u << i) - 1u) >> i;
        const uint32_t mh = ((uint32_t)h + (1u << i) - 1u) >> i;
        const uint32_t md = ((uint32_t)d + (1u << i) - 1u) >> i;
        const uint32_t pitch = ((mw ? mw : 1u) + 63u) & ~63u;
        offsets[i] = total;
        pitches[i] = pitch;
        total += (size_t)pitch * (size_t)(mh ? mh : 1u) * (size_t)(md ? md : 1u) * 4u;
    }
    return total;
}

static inline size_t gl_tex_chain_layout(GLsizei w, GLsizei h, int levels,
                                         size_t offsets[], uint32_t pitches[]) {
    return gl_tex_chain_layout_3d(w, h, 1, levels, offsets, pitches);
}

/* Slot `id - 1` first, then the scan.
 *
 * `glGenTextures` hands out the lowest free name and `gl_find_or_create_texture` takes
 * the lowest free slot, so a texture called `n` is in slot `n - 1` unless deletion has
 * shuffled things - which makes the guess right almost always and wrong harmlessly,
 * since the same `used && id ==` test that ends the scan also validates it. No cache,
 * so nothing to invalidate when a texture is created, deleted or renamed. This runs
 * several times per draw. */
/* The writable form of `gl_lookup_texture`, for the draw path's own scans. Same rule:
   slot `id - 1` first, then the sweep that validates it. */
static inline gl_texture_object_t *gl_texture_slot(gl_context_t *ctx, GLuint id) {
    if (!ctx || id == 0u)
        return (gl_texture_object_t *)0;
    if (id <= (GLuint)OOPS_GL_MAX_TEXTURE_OBJECTS) {
        gl_texture_object_t *t = &ctx->textures[id - 1u];
        if (t->used && t->id == id)
            return t;
    }
    for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
        if (ctx->textures[i].used && ctx->textures[i].id == id)
            return &ctx->textures[i];
    }
    return (gl_texture_object_t *)0;
}

/* The framebuffer and renderbuffer a name refers to. Here rather than beside the entry
 * points in `gl_state.c` because `gl_draw_targets` needs them too, and it lives in
 * `gl_context.c`. Slot `id - 1` first, then the sweep that validates it -
 * `gl_texture_slot`'s rule. */
static inline gl_framebuffer_object_t *gl_framebuffer_slot(gl_context_t *ctx,
                                                           GLuint id) {
    if (!ctx || id == 0u)
        return (gl_framebuffer_object_t *)0;
    if (id <= (GLuint)OOPS_GL_MAX_FRAMEBUFFER_OBJECTS) {
        gl_framebuffer_object_t *f = &ctx->framebuffers[id - 1u];
        if (f->used && f->id == id)
            return f;
    }
    for (int i = 0; i < OOPS_GL_MAX_FRAMEBUFFER_OBJECTS; i++) {
        if (ctx->framebuffers[i].used && ctx->framebuffers[i].id == id)
            return &ctx->framebuffers[i];
    }
    return (gl_framebuffer_object_t *)0;
}

static inline gl_renderbuffer_object_t *gl_renderbuffer_slot(gl_context_t *ctx,
                                                             GLuint id) {
    if (!ctx || id == 0u)
        return (gl_renderbuffer_object_t *)0;
    if (id <= (GLuint)OOPS_GL_MAX_RENDERBUFFER_OBJECTS) {
        gl_renderbuffer_object_t *r = &ctx->renderbuffers[id - 1u];
        if (r->used && r->id == id)
            return r;
    }
    for (int i = 0; i < OOPS_GL_MAX_RENDERBUFFER_OBJECTS; i++) {
        if (ctx->renderbuffers[i].used && ctx->renderbuffers[i].id == id)
            return &ctx->renderbuffers[i];
    }
    return (gl_renderbuffer_object_t *)0;
}

/*
 * Whether the path can draw into a framebuffer object at all, before asking anything
 * about the attachments.
 *
 * The software rasteriser can, always. So can the hardware path: `gl_hw_begin_frame`
 * takes `CB_COLOR0_BASE` from `ctx->framebuffer` and the extent from
 * `ctx->width`/`ctx->height`, and `CB_COLOR0_INFO` describes a LINEAR_GENERAL surface,
 * so a linear attachment with a GPU address is the same shape of target. That it is GPU
 * memory is checked per attachment by `gl_fbo_bound_target`.
 *
 * The display's swizzle is not a reason to refuse: `gl_draw_targets` clears the tiling
 * while a framebuffer object is bound and puts it back after, so the addressing follows
 * the target rather than the display.
 */
static inline GLboolean gl_fbo_path_can_render(const gl_context_t *ctx) {
    return (GLboolean)(ctx != (const gl_context_t *)0);
}

/*
 * Where an attachment's pixels are, and how wide its rows are.
 *
 * One resolver for both readers - `glCheckFramebufferStatus` and `gl_draw_targets` - so
 * a status never says COMPLETE about storage the draw path cannot find.
 *
 * `pitch` is in pixels. A renderbuffer is packed tight; a texture's base level has the
 * object's own pitch, and a level above it is packed tight - which is
 * `gl_tex_level_view`'s rule, so it is read from there rather than restated.
 */
typedef struct {
    uint32_t *pixels;
    GLsizei width;
    GLsizei height;
    size_t pitch;
    /* Whether the command processor can be pointed at these pixels. A renderbuffer says
     * so itself; a texture's base level is GPU memory on the console and heap memory on
     * a build machine, which `gl_tex_level_view` does not carry, so it is asked
     * separately below. */
    GLboolean gpu_resident;
} gl_fb_storage_t;

static inline GLboolean gl_fb_attachment_storage(gl_context_t *ctx,
                                                 const gl_fb_attachment_t *at,
                                                 gl_fb_storage_t *out) {
    out->pixels = (uint32_t *)0;
    out->width = 0;
    out->height = 0;
    out->pitch = 0;
    out->gpu_resident = GL_FALSE;
    if (!ctx || !at || at->kind == GL_FB_ATTACH_NONE)
        return GL_FALSE;

    if (at->kind == GL_FB_ATTACH_RENDERBUFFER) {
        gl_renderbuffer_object_t *rb = gl_renderbuffer_slot(ctx, at->name);
        if (!rb || !rb->pixels || rb->width <= 0 || rb->height <= 0)
            return GL_FALSE;
        out->pixels = rb->pixels;
        out->width = rb->width;
        out->height = rb->height;
        out->pitch = (size_t)rb->width;
        out->gpu_resident = rb->gpu_resident;
        return GL_TRUE;
    }

    gl_texture_object_t *tex = gl_texture_slot(ctx, at->name);
    if (!tex)
        return GL_FALSE;
    gl_tex_view_t view;
    const GLboolean have =
        (at->textarget == GL_TEXTURE_2D)
            ? gl_tex_level_view(tex, at->level, &view)
            : gl_tex_face_view(tex, GL_CUBE_FACE_INDEX(at->textarget), at->level,
                               &view);
    if (!have)
        return GL_FALSE;
    /* `gl_tex_view_t` is the sampler's read-only view and this is a write. The cast
     * is back to the storage the texture object owns, which is the same memory - the
     * constness belongs to the view, not to the level. */
    out->pixels = (uint32_t *)(void *)(uintptr_t)view.pixels;
    out->width = view.width;
    out->height = view.height;
    out->pitch = view.pitch;
    /* Only the base level is GPU memory. `garlic_data` is the image the sampler reads
     * and the one a render target could be pointed at; the levels above it are built on
     * the heap by `gl_tex_generate_mipmap`, so rendering into them is a software-path
     * operation. */
    out->gpu_resident = (GLboolean)(at->level == 0 && tex->garlic_data != (void *)0 &&
                                    (const uint8_t *)tex->garlic_data == view.pixels);
    return GL_TRUE;
}

/*
 * The bound framebuffer object's colour and depth, when a draw can go there.
 *
 * `gl_color_index` multiplies by `ctx->width` to step a row, so an attachment whose
 * rows are further apart than they are wide - a base-level texture with a padded pitch
 * - is refused rather than written past the end of each row. The status query asks the
 * same function, so a framebuffer this refuses never reports itself complete.
 *
 * `depth` may come back NULL with a true result: a framebuffer object with a colour
 * attachment and no depth one is legal, and the draw path already treats a null depth
 * buffer as no depth test rather than as an error.
 */
static inline GLboolean gl_fbo_bound_target(gl_context_t *ctx, gl_fb_storage_t *colour,
                                            float **depth) {
    *depth = (float *)0;
    if (!gl_fbo_path_can_render(ctx))
        return GL_FALSE;
    gl_framebuffer_object_t *fb = gl_framebuffer_slot(ctx, ctx->bound_framebuffer);
    if (!fb)
        return GL_FALSE;
    if (!gl_fb_attachment_storage(ctx, &fb->color0, colour))
        return GL_FALSE;
    if (colour->pitch != (size_t)colour->width)
        return GL_FALSE;
    /* On the console the command processor has to be able to reach it. The software
     * path writes with the CPU and does not care. */
    if (ctx->use_hardware && !colour->gpu_resident)
        return GL_FALSE;

    gl_fb_storage_t ds;
    if (gl_fb_attachment_storage(ctx, &fb->depth, &ds)) {
        if (ds.pitch != (size_t)ds.width || ds.width != colour->width ||
            ds.height != colour->height) {
            return GL_FALSE;
        }
        /* A depth attachment is software-path only. The console's depth surface is
         * 64KB_Z_X tiled and programmed through its own registers, so pointing the
         * depth test at a linear renderbuffer would have the rasteriser and the command
         * processor addressing different memory. The whole framebuffer is refused
         * rather than drawn with its depth attachment ignored. */
        if (ctx->use_hardware)
            return GL_FALSE;
        /* The depth renderbuffer is one word a sample, which is what the software
         * rasteriser's depth buffer is - it reads them as floats, so this names the
         * same words that way. */
        *depth = (float *)(void *)ds.pixels;
    }
    return GL_TRUE;
}

static inline const gl_texture_object_t *gl_lookup_texture(const gl_context_t *ctx,
                                                           GLuint id) {
    if (id != 0u && id <= (GLuint)OOPS_GL_MAX_TEXTURE_OBJECTS) {
        const gl_texture_object_t *t = &ctx->textures[id - 1u];
        if (t->used && t->id == id)
            return t;
    }
    for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
        if (ctx->textures[i].used && ctx->textures[i].id == id)
            return &ctx->textures[i];
    }
    return (const gl_texture_object_t *)0;
}

/* The texture a draw samples, by id, or 0 for none.
 *
 * Nothing bound means the target's default texture, not no texture: GL 1.x's texture 0
 * is a real texture, and a program in the 1.0 style uploads with glTexImage2D and draws
 * without ever calling glBindTexture. An incomplete texture samples as none. */
static inline GLuint gl_unit_texture_id(const gl_context_t *ctx, GLuint unit) {
    if (!ctx || unit >= OOPS_GL_MAX_TEXTURE_UNITS)
        return 0u;
    const gl_tex_unit_t *tu = &ctx->tex_unit[unit];
    GLuint id = 0u;
    /* A cube map outranks them all (GL 1.3, 3.8.15). */
    if (tu->cap_texture_cube_map) {
        id = tu->bound_texture_cube ? tu->bound_texture_cube
                                    : OOPS_GL_DEFAULT_TEXTURE_CUBE;
    } else if (tu->cap_texture_3d) {
        id = tu->bound_texture_3d ? tu->bound_texture_3d : OOPS_GL_DEFAULT_TEXTURE_3D;
    } else if (tu->cap_texture_2d) {
        id = tu->bound_texture_2d ? tu->bound_texture_2d : OOPS_GL_DEFAULT_TEXTURE_2D;
    } else if (tu->cap_texture_1d) {
        id = tu->bound_texture_1d ? tu->bound_texture_1d : OOPS_GL_DEFAULT_TEXTURE_1D;
    }
    if (id == 0u)
        return 0u;
    const gl_texture_object_t *tex = gl_lookup_texture(ctx, id);
    /* Zero here means "sampled nothing", and that covers two different situations:
     * texturing off, and texturing on with a texture GL says is unusable. The draw path
     * tells them apart and reports the second - see the note beside `eff_tex` in
     * gl_draw.c. */
    return (tex && gl_texture_complete(tex)) ? id : 0u;
}

/* Depth and stencil at a window pixel, for the CPU - `y` counting up from the bottom,
 * as GL's window coordinates do. The software rasteriser's buffers are rows, top first;
 * the console's are the GPU's surfaces, 64KB_Z_X tiled, addressed through
 * gl_zs_tiling.h's vectors. `zs_tiled` says which: set where the hardware path comes
 * up, never on a host build, whose "hardware" is the PM4 tests' mock over the same row
 * buffers. A caller on the console submits the open frame first, as every pixel
 * operation does (glFlush, or gl_raster.c's gl_raster_sync). No HTILE is bound
 * (DB_HTILE_DATA_BASE is 0), so the surface is the whole of the state. The frame's
 * closing RELEASE_MEM writes the DB's caches back and invalidates GL2 (its GCR field,
 * 0x603, is GLM_WB | GLM_INV | GL2_INV | GL2_WB). That makes both directions coherent:
 * the CPU reads what the GPU wrote, and the next frame reads what the CPU wrote. The
 * CPU reads and writes the write-combined mapping uncached. */
static inline float *gl_zs_depth_ptr(gl_context_t *ctx, int x, int y) {
    const uint32_t row = ctx->height - 1u - (uint32_t)y;
    if (ctx->zs_tiled) {
        const uint32_t pitch = (ctx->width + 127u) & ~127u;
        return (float *)(void *)((char *)ctx->depth_buffer +
                                 gl_zs_depth_offset(pitch, (uint32_t)x, row));
    }
    return &ctx->depth_buffer[(size_t)row * ctx->width + (size_t)x];
}

static inline uint8_t *gl_zs_stencil_ptr(gl_context_t *ctx, int x, int y) {
    const uint32_t row = ctx->height - 1u - (uint32_t)y;
    if (ctx->zs_tiled) {
        const uint32_t pitch = (ctx->width + 255u) & ~255u;
        return ctx->stencil_buffer + gl_zs_stencil_offset(pitch, (uint32_t)x, row);
    }
    return &ctx->stencil_buffer[(size_t)row * ctx->width + (size_t)x];
}

/* A colour pixel's index at a window position, for the CPU - `y` counting up from
 * the bottom. A colour buffer is rows, top first, in the software rasteriser and on the
 * console's linear path. On the scanout path it is a scanout buffer in the GPU's
 * 64KB_R_X swizzle: 128 x 128 blocks row by row, each addressed through the display
 * tiler's own vectors (agc_tile_pixel). The CP's copy of such a buffer is the same
 * bytes, so it is addressed the same way. `color_tiled` says which, for every colour
 * buffer of the context at once. */
/* Drains the CPU's outstanding colour writes, so that a later read - by this
 * CPU, by the CP's DMA, or by the display - sees them. Nothing to do when the span is
 * empty, which is every frame that draws only with the GPU (gl_context.c). */
void gl_color_cpu_drain(gl_context_t *ctx);

/* One word of a colour buffer the CPU has just written, added to the outstanding span.
 * One range rather than a list: a pixel rectangle is contiguous in `y` and its
 * fragments land in a band, so one range costs a few extra cache lines at the ends and
 * no bookkeeping. A write to a different buffer drains the one before it, because the
 * span names a single buffer. `gl_color_cpu_drain` drains it; see the fields' comment.
 */
static inline void gl_color_cpu_touched(gl_context_t *ctx, uint32_t *buf, size_t i) {
    if (ctx->cpu_color_buf != buf) {
        gl_color_cpu_drain(ctx);
        ctx->cpu_color_buf = buf;
        ctx->cpu_color_lo = i;
        ctx->cpu_color_hi = i + 1u;
        return;
    }
    if (i < ctx->cpu_color_lo)
        ctx->cpu_color_lo = i;
    if (i + 1u > ctx->cpu_color_hi)
        ctx->cpu_color_hi = i + 1u;
}

static inline size_t gl_color_index(const gl_context_t *ctx, int x, int y) {
    const uint32_t row = ctx->height - 1u - (uint32_t)y;
    if (ctx->color_tiled) {
        const size_t block = (size_t)(row >> 7) * (size_t)((ctx->width + 127u) >> 7) +
                             (size_t)((uint32_t)x >> 7);
        return block * 16384u + agc_tile_pixel((uint32_t)x & 127u, row & 127u);
    }
    return (size_t)row * ctx->width + (size_t)x;
}

/* The words a colour buffer spans - what a whole clear fills and the CP's copy holds:
 * the width and height padded to whole blocks when tiled. */
static inline size_t gl_color_words(const gl_context_t *ctx) {
    if (ctx->color_tiled) {
        return (size_t)((ctx->width + 127u) & ~127u) *
               (size_t)((ctx->height + 127u) & ~127u);
    }
    return (size_t)ctx->width * (size_t)ctx->height;
}

/*
 * The unit the console's first sampling stage takes its texture from: the first that
 * has one.
 *
 * A unit with texturing disabled passes the fragment colour through, so a later unit's
 * `GL_PREVIOUS` is the primary colour (GL 1.3, 3.8.13), and unit 0 does not have to be
 * textured for unit 1 to apply. gl1-probe's `texture-unit1-alone` covers it.
 */
static inline GLuint gl_hw_base_unit(const gl_context_t *ctx) {
    for (GLuint u = 0u; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        if (gl_unit_texture_id(ctx, u) != 0u)
            return u;
    }
    return 0u;
}

/* What the console's one sampled texture is - the base unit's, which is usually unit
 * 0's. */
static inline GLuint gl_effective_texture_id(const gl_context_t *ctx) {
    return gl_unit_texture_id(ctx, gl_hw_base_unit(ctx));
}

size_t gl_unpack_row_stride(const gl_context_t *ctx, GLsizei width, size_t pixel_bytes);
size_t gl_unpack_row_stride_bytes(const gl_context_t *ctx, size_t bytes);
void gl_update_mvp(gl_context_t *ctx);
void gl_update_normal_matrix(gl_context_t *ctx);
void gl_normal_matrix_of(const gl_mat4_t *mv, float *normal_matrix);
/* An object-space normal in eye space as lighting and texture generation both take it:
 * through the normal matrix `nm`, then unit length under GL_NORMALIZE or rescaled under
 * GL_RESCALE_NORMAL, and left alone otherwise. */
void gl_eye_normal(const gl_context_t *ctx, const float *nm, const float *n,
                   float out[3]);
/* The lit primary colour - with the specular term in it, or not under
 * GL_SEPARATE_SPECULAR_COLOR - and, from the second form, the secondary colour: that
 * specular term, clamped, or zero. */
void gl_compute_lighting(gl_context_t *ctx, const float *obj_pos, const float *obj_norm,
                         const float *in_color, float *out_color);
/* GL_COLOR_MATERIAL's write-through: the current colour into every material property it
 * tracks (gl_state.c). */
void gl_color_material_update(gl_context_t *ctx);
void gl_compute_lighting2(gl_context_t *ctx, const float *obj_pos,
                          const float *obj_norm, const float *in_color,
                          float *out_color, float *out_secondary);
/* The same for one side: `back` lights with the back material and the normal reversed,
 * as GL_LIGHT_MODEL_TWO_SIDE lights a polygon that faces away. */
void gl_compute_lighting_side(gl_context_t *ctx, const float *obj_pos,
                              const float *obj_norm, const float *in_color,
                              float *out_color, float *out_secondary, GLboolean back);

static inline uint32_t gl_depth_func_to_zfunc(GLenum func) {
    switch (func) {
    case GL_NEVER:
        return OOPS_AGC_ZFUNC_NEVER;
    case GL_LESS:
        return OOPS_AGC_ZFUNC_LESS;
    case GL_EQUAL:
        return OOPS_AGC_ZFUNC_EQUAL;
    case GL_LEQUAL:
        return OOPS_AGC_ZFUNC_LEQUAL;
    case GL_GREATER:
        return OOPS_AGC_ZFUNC_GREATER;
    case GL_NOTEQUAL:
        return OOPS_AGC_ZFUNC_NOTEQUAL;
    case GL_GEQUAL:
        return OOPS_AGC_ZFUNC_GEQUAL;
    case GL_ALWAYS:
        return OOPS_AGC_ZFUNC_ALWAYS;
    default:
        return OOPS_AGC_ZFUNC_LESS;
    }
}

/* Whether the stencil test runs on the console: the enable, and a stencil surface to
 * test. */
static inline GLboolean gl_hw_stencil_on(const gl_context_t *ctx) {
    return (GLboolean)(ctx->cap_stencil_test && ctx->stencil_buffer != NULL);
}

static inline uint32_t gl_compute_db_depth_control(const gl_context_t *ctx) {
    if (!ctx)
        return 0u;
    int z_enable = (ctx->cap_depth_test && ctx->depth_buffer != NULL) ? 1 : 0;
    int z_write = (z_enable && ctx->depth_mask) ? 1 : 0;
    uint32_t zfunc = gl_depth_func_to_zfunc(ctx->depth_func);
    uint32_t v = z_enable ? OOPS_AGC_DB_DEPTH_CONTROL(1, z_write, zfunc) : 0u;
    /* The stencil test: STENCIL_ENABLE (bit 0) and STENCILFUNC (bits 8-10),
     * gfx103.json's DB_DEPTH_CONTROL fields, radeonsi's programming
     * (si_state.c:1418-1428). The compare functions are CompareFrag's, in GL's own
     * order, so the depth function's mapping serves.
     *
     * BACKFACE_ENABLE is set and the back face gets its own function, for GL 2.0's
     * `glStencilFuncSeparate` and `glStencilOpSeparate`; with the bit clear the
     * hardware applies the front state to both. gl2-probe's `separate-stencil` covers
     * it.
     *
     * `STENCILFUNC_BF` is bits 22:20 (`R_028800`), and the back state mirrors the front
     * when a program never calls the separate entry points - `glStencilFunc` writes
     * both. */
    if (gl_hw_stencil_on(ctx)) {
        v |= 1u | (gl_depth_func_to_zfunc(ctx->stencil_func) << 8) | (1u << 7) |
             (gl_depth_func_to_zfunc(ctx->stencil_back_func) << 20);
    }
    return v;
}

/* GL's stencil operation as DB_STENCIL_CONTROL's StencilOp (gfx103.json's enum;
 * radeonsi's si_translate_stencil_op, si_state.c:1353-1378): GL_REPLACE is
 * REPLACE_TEST, the reference value, and GL_INCR/GL_DECR the clamping forms. */
static inline uint32_t gl_hw_stencil_op(GLenum op) {
    switch (op) {
    case GL_ZERO:
        return 1u;
    case GL_REPLACE:
        return 3u;
    case GL_INCR:
        return 5u;
    case GL_DECR:
        return 6u;
    case GL_INVERT:
        return 7u;
    case GL_INCR_WRAP:
        return 8u;
    case GL_DECR_WRAP:
        return 9u;
    default:
        return 0u; /* GL_KEEP */
    }
}

/* Whether the triangles being drawn are a polygon (see `prim_raster`). Anything but an
 * expanded line or point is. */
static inline GLboolean gl_prim_is_polygon(const gl_context_t *ctx) {
    return (GLboolean)(ctx->prim_raster != GL_LINE && ctx->prim_raster != GL_POINT);
}

/* Culling is for polygons. A line expanded into a quad has a winding like any triangle,
 * and culling it would drop whichever lines happened to wind the wrong way. */
static inline GLboolean gl_prim_culls(const gl_context_t *ctx) {
    return (GLboolean)(ctx->cap_cull_face && gl_prim_is_polygon(ctx));
}

/* Which polygon-offset enable, if any, covers the triangles being drawn. A GL_LINES or
 * GL_POINTS primitive has none, so an expanded line does not take the fill offset. */
static inline GLboolean gl_prim_offsets(const gl_context_t *ctx) {
    if (gl_prim_is_polygon(ctx))
        return ctx->cap_polygon_offset_fill;
    if (!ctx->prim_from_polygon)
        return GL_FALSE;
    return (ctx->prim_raster == GL_LINE) ? ctx->cap_polygon_offset_line
                                         : ctx->cap_polygon_offset_point;
}

static inline uint32_t gl_compute_pa_su_sc_mode_cntl(const gl_context_t *ctx) {
    if (!ctx)
        return OOPS_AGC_CULL_NONE;
    uint32_t cull_bits = 0;
    if (gl_prim_culls(ctx)) {
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
    if (gl_prim_offsets(ctx)) {
        /* Front, back and para together: GL has one enable for filled polygons and does
         * not distinguish the faces, so enabling one and not the other would offset
         * half a mesh.
         *
         * Bit positions from Mesa's generated register header,
         * `src/amd/common/amdgfxregs.h`: S_028814_POLY_OFFSET_FRONT_ENABLE is bit 11,
         * BACK_ENABLE bit 12, PARA_ENABLE bit 13. */
        cull_bits |= (1u << 11) | (1u << 12) | (1u << 13);
    }
    return OOPS_AGC_CULL_NONE | cull_bits;
}

/* What a colour buffer name names, in this visual - double-buffered, mono, no
 * auxiliary buffers: GL_OCB_BACK, GL_OCB_FRONT, both, GL_OCB_ABSENT for the right and
 * auxiliary buffers it does not have, or 0 for anything that is not a buffer name.
 * GL_LEFT and GL_FRONT_AND_BACK draw into the front and the back, as Mesa's
 * draw_buffer_enum_to_bitmask has them (`main/buffers.c:145-170`). Read from, every
 * name that includes the front reads the front (`:209-226`). GL_NONE is 0 here;
 * glDrawBuffer takes it separately. */
#define GL_OCB_BACK 1u
#define GL_OCB_FRONT 2u
#define GL_OCB_ABSENT 4u
static inline unsigned gl_color_buffer_bits(GLenum b) {
    switch (b) {
    case GL_BACK:
    case GL_BACK_LEFT:
        return GL_OCB_BACK;
    case GL_FRONT:
    case GL_FRONT_LEFT:
        return GL_OCB_FRONT;
    case GL_LEFT:
    case GL_FRONT_AND_BACK:
        return GL_OCB_FRONT | GL_OCB_BACK;
    case GL_FRONT_RIGHT:
    case GL_BACK_RIGHT:
    case GL_RIGHT:
    case GL_AUX0:
    case GL_AUX1:
    case GL_AUX2:
    case GL_AUX3:
        return GL_OCB_ABSENT;
    default:
        return 0u;
    }
}

/* The front buffer, allocated and filled with the picture on screen, or GL_FALSE when
 * there is no memory for it (gl_context.c). */
GLboolean gl_front_buffer(gl_context_t *ctx);
/* `framebuffer` and `fb_also` made what glDrawBuffer says, the open frame submitted
 * first on the console when the buffer it draws into changes (gl_context.c). */
void gl_draw_targets(gl_context_t *ctx);
/* The front put on screen, if it has been drawn into since it last was - glFlush and
 * glFinish. */
void gl_front_present(gl_context_t *ctx);
/* The buffer glReadBuffer names - or the bound framebuffer object's colour attachment,
 * which takes precedence over it for the reason glDrawBuffer is ignored while one is
 * bound: GL_FRONT and GL_BACK name the window system's buffers, and a framebuffer
 * object has neither. Reading has to follow drawing here, or a program draws into an
 * attachment and reads the display. */
static inline const uint32_t *gl_read_target(const gl_context_t *ctx) {
    if (ctx->bound_framebuffer != 0u && ctx->framebuffer)
        return ctx->framebuffer;
    return (gl_color_buffer_bits(ctx->read_buffer) & GL_OCB_FRONT) && ctx->front_fb
               ? ctx->front_fb
               : ctx->back_fb;
}
/* A colour buffer's pixels, for the CPU to read, after the caller's flush. On the
 * console this is the CP's CPU-cached copy, when the last submission made one of this
 * very buffer and the CPU has not written into the buffer since. Otherwise it is the
 * buffer itself. */
static inline const uint32_t *gl_color_read_source(const gl_context_t *ctx,
                                                   const uint32_t *buf) {
    if (!buf || ctx->hw_frames_confirmed == 0u)
        return buf;
    if (ctx->readback && ctx->readback_of == buf)
        return ctx->readback;
    /* Either target answers here, not just the one the last draw called primary.
     * The tags are distinct pointers, so the second copy can only be returned for the
     * buffer it was made of, and a target with no copy still falls through to itself.
     */
    if (ctx->readback_also && ctx->readback_also_of == buf)
        return ctx->readback_also;
    return buf;
}

/* Whether `src` is one of the CP's copies rather than a colour buffer itself. */
static inline GLboolean gl_color_is_copy(const gl_context_t *ctx, const uint32_t *src) {
    return (GLboolean)(src && (src == ctx->readback || src == ctx->readback_also));
}

/*
 * Drop what the CPU has cached of the CP's copy, before reading it.
 *
 * A copy is filled by a DMA the CPU knows nothing about, so any line of it the CPU
 * still holds is from an earlier frame. Every reader of a copy - `glGetFrameReadback`,
 * `glReadPixels` - invalidates first.
 *
 * `clflush` writes a dirty line back before invalidating it, which is why this is only
 * ever pointed at a copy: the CPU never writes one, so every line is clean. Aimed at a
 * live colour buffer it would write a stale line back over what the GPU just drew.
 *
 * Per word, because a read is usually a rectangle and often one pixel: invalidating the
 * whole copy for a 1x1 read would touch nine megabytes. A large read repeats the
 * instruction on lines it has already dropped, which is still cheaper.
 */
static inline void gl_color_copy_invalidate_word(const gl_context_t *ctx,
                                                 const uint32_t *src, size_t i) {
#if defined(__x86_64__) && !defined(OOPS_HOST_BUILD)
    if (gl_color_is_copy(ctx, src)) {
        __builtin_ia32_clflush((const void *)(src + i));
    }
#else
    (void)ctx;
    (void)src;
    (void)i;
#endif
}

/* The same, for a caller that reads the whole copy in one go - a swap's memcpy, or the
 * present. `words` is the copy's own extent (gl_color_words), not the visible image's.
 */
static inline void gl_color_copy_invalidate_all(const gl_context_t *ctx,
                                                const uint32_t *src, size_t words) {
#if defined(__x86_64__) && !defined(OOPS_HOST_BUILD)
    if (gl_color_is_copy(ctx, src)) {
        for (size_t p = 0; p < words * 4u; p += 64u) {
            __builtin_ia32_clflush((const void *)((const char *)src + p));
        }
    }
#else
    (void)ctx;
    (void)src;
    (void)words;
#endif
}

/* Whether colour channel `i` is written: its glColorMask bit, and a draw buffer that is
 * not GL_NONE (GL 1.0, 4.2.1). */
static inline GLboolean gl_color_writes(const gl_context_t *ctx, int i) {
    return (GLboolean)(ctx->color_mask[i] && ctx->draw_buffer != GL_NONE);
}

static inline uint32_t gl_compute_cb_target_mask(const gl_context_t *ctx) {
    if (!ctx)
        return 0x0000000fu;
    uint32_t mask = 0;
    if (gl_color_writes(ctx, 0))
        mask |= 0x1u; /* Red */
    if (gl_color_writes(ctx, 1))
        mask |= 0x2u; /* Green */
    if (gl_color_writes(ctx, 2))
        mask |= 0x4u; /* Blue */
    if (gl_color_writes(ctx, 3))
        mask |= 0x8u; /* Alpha */
    return mask;
}

/*
 * One GL blend factor as the colour block's `BlendOp`.
 *
 * Values from `oops-mesa/mesa/src/amd/registers/gfx10.json`, enum `BlendOp` - they are
 * a closed table, so a factor with no entry is refused by falling back rather than
 * being approximated.
 */
static inline uint32_t gl_blend_op(GLenum factor, uint32_t fallback) {
    switch (factor) {
    case GL_ZERO:
        return 0u; /* BLEND_ZERO */
    case GL_ONE:
        return 1u; /* BLEND_ONE */
    case GL_SRC_COLOR:
        return 2u; /* BLEND_SRC_COLOR */
    case GL_ONE_MINUS_SRC_COLOR:
        return 3u; /* BLEND_ONE_MINUS_SRC_COLOR */
    case GL_SRC_ALPHA:
        return 4u; /* BLEND_SRC_ALPHA */
    case GL_ONE_MINUS_SRC_ALPHA:
        return 5u; /* BLEND_ONE_MINUS_SRC_ALPHA */
    case GL_DST_ALPHA:
        return 6u; /* BLEND_DST_ALPHA */
    case GL_ONE_MINUS_DST_ALPHA:
        return 7u; /* BLEND_ONE_MINUS_DST_ALPHA */
    case GL_DST_COLOR:
        return 8u; /* BLEND_DST_COLOR */
    case GL_ONE_MINUS_DST_COLOR:
        return 9u; /* BLEND_ONE_MINUS_DST_COLOR */
    case GL_SRC_ALPHA_SATURATE:
        return 10u; /* BLEND_SRC_ALPHA_SATURATE */
    /* Not contiguous with the rest: 11 and 12 are the BOTH_ factors and 15-18 the
     * dual-source ones. radeonsi picks the same four through its _GFX6 names below
     * GFX11 (gallium/drivers/radeonsi/si_state.c, si_translate_blend_factor). */
    case GL_CONSTANT_COLOR:
        return 13u; /* BLEND_CONSTANT_COLOR */
    case GL_ONE_MINUS_CONSTANT_COLOR:
        return 14u; /* BLEND_ONE_MINUS_CONSTANT_COLOR */
    case GL_CONSTANT_ALPHA:
        return 19u; /* BLEND_CONSTANT_ALPHA */
    case GL_ONE_MINUS_CONSTANT_ALPHA:
        return 20u; /* BLEND_ONE_MINUS_CONSTANT_ALPHA */
    default:
        return fallback;
    }
}

static inline GLboolean gl_blend_factor_is_constant(GLenum factor) {
    return (factor == GL_CONSTANT_COLOR || factor == GL_ONE_MINUS_CONSTANT_COLOR ||
            factor == GL_CONSTANT_ALPHA || factor == GL_ONE_MINUS_CONSTANT_ALPHA)
               ? GL_TRUE
               : GL_FALSE;
}

/*
 * The two families are not interchangeable on this part, and the next two predicates
 * tell them apart.
 *
 * The green channel of a `BLEND_CONSTANT_COLOR` blend reads `CB_BLEND_ALPHA` at `0x108`
 * and ignores `CB_BLEND_GREEN` at `0x106`; red and blue take their own registers.
 * Packet shape, write order and the RB+ registers do not change it
 * (docs/hardware/agc-blend-and-export-fw1240.md, section 1).
 *
 * A draw reading the colour constant therefore needs green placed in the alpha slot;
 * one reading both families cannot be satisfied at all, because both want `0x108`.
 * `gl_draw.c` does the first and refuses the second.
 */
static inline GLboolean gl_blend_factor_is_constant_color(GLenum factor) {
    return (factor == GL_CONSTANT_COLOR || factor == GL_ONE_MINUS_CONSTANT_COLOR)
               ? GL_TRUE
               : GL_FALSE;
}

static inline GLboolean gl_blend_factor_is_constant_alpha(GLenum factor) {
    return (factor == GL_CONSTANT_ALPHA || factor == GL_ONE_MINUS_CONSTANT_ALPHA)
               ? GL_TRUE
               : GL_FALSE;
}

/* Whether a draw with this state reads CB_BLEND_RED..ALPHA - blending on, not
 * overridden by a logic op, and one of the four factors a constant one. */
static inline GLboolean gl_blend_reads_constant(const gl_context_t *ctx) {
    if (!ctx->cap_blend || ctx->cap_color_logic_op)
        return GL_FALSE;
    /* Both equations have to ignore the factors for the constant to go unread. With
     * separate equations (GL 2.0) a GL_MIN colour and a GL_FUNC_ADD alpha still reads
     * it for the alpha, and testing only the colour's equation would leave the register
     * unwritten. */
    const GLboolean c_minmax =
        (GLboolean)(ctx->blend_equation == GL_MIN || ctx->blend_equation == GL_MAX);
    const GLboolean a_minmax = (GLboolean)(ctx->blend_equation_alpha == GL_MIN ||
                                           ctx->blend_equation_alpha == GL_MAX);
    if (c_minmax && a_minmax)
        return GL_FALSE;
    return ((!c_minmax && (gl_blend_factor_is_constant(ctx->blend_src) ||
                           gl_blend_factor_is_constant(ctx->blend_dst))) ||
            (!a_minmax && (gl_blend_factor_is_constant(ctx->blend_src_alpha) ||
                           gl_blend_factor_is_constant(ctx->blend_dst_alpha))))
               ? GL_TRUE
               : GL_FALSE;
}

/*
 * The same question, split by family, for the `0x108` conflict described above. Both
 * mirror `gl_blend_reads_constant`'s equation masking rather than repeating a simpler
 * test: a `GL_MIN` equation ignores its factors, so a constant named there is not read
 * and must not count.
 */
static inline GLboolean gl_blend_reads_constant_color(const gl_context_t *ctx) {
    if (!ctx->cap_blend || ctx->cap_color_logic_op)
        return GL_FALSE;
    const GLboolean c_minmax =
        (GLboolean)(ctx->blend_equation == GL_MIN || ctx->blend_equation == GL_MAX);
    const GLboolean a_minmax = (GLboolean)(ctx->blend_equation_alpha == GL_MIN ||
                                           ctx->blend_equation_alpha == GL_MAX);
    return ((!c_minmax && (gl_blend_factor_is_constant_color(ctx->blend_src) ||
                           gl_blend_factor_is_constant_color(ctx->blend_dst))) ||
            (!a_minmax && (gl_blend_factor_is_constant_color(ctx->blend_src_alpha) ||
                           gl_blend_factor_is_constant_color(ctx->blend_dst_alpha))))
               ? GL_TRUE
               : GL_FALSE;
}

static inline GLboolean gl_blend_reads_constant_alpha(const gl_context_t *ctx) {
    if (!ctx->cap_blend || ctx->cap_color_logic_op)
        return GL_FALSE;
    const GLboolean c_minmax =
        (GLboolean)(ctx->blend_equation == GL_MIN || ctx->blend_equation == GL_MAX);
    const GLboolean a_minmax = (GLboolean)(ctx->blend_equation_alpha == GL_MIN ||
                                           ctx->blend_equation_alpha == GL_MAX);
    return ((!c_minmax && (gl_blend_factor_is_constant_alpha(ctx->blend_src) ||
                           gl_blend_factor_is_constant_alpha(ctx->blend_dst))) ||
            (!a_minmax && (gl_blend_factor_is_constant_alpha(ctx->blend_src_alpha) ||
                           gl_blend_factor_is_constant_alpha(ctx->blend_dst_alpha))))
               ? GL_TRUE
               : GL_FALSE;
}

/*
 * A GL logic op as the four-bit truth table both Mesa and the colour block use.
 *
 * GL's own enum is a truth table too, but with its bits in the opposite order: GL_COPY
 * is 0x1503 where the table value is 12. So it is not `opcode - GL_CLEAR`, and it is
 * not something to re-derive by hand - this is Mesa's `color_logicop_mapping`
 * (main/blend.c:835-852), indexed by `opcode & 0xf` as `logic_op` does at line 888,
 * into `enum gl_logicop_mode` (main/menums.h:107-124), whose values are defined as
 * `result_bit = mode & (1 << (2 * src_bit + dst_bit))`.
 */
static inline uint32_t gl_logicop_mode(GLenum opcode) {
    static const uint8_t mapping[16] = {
        0u,  /* GL_CLEAR         -> COLOR_LOGICOP_CLEAR */
        8u,  /* GL_AND           -> COLOR_LOGICOP_AND */
        4u,  /* GL_AND_REVERSE   -> COLOR_LOGICOP_AND_REVERSE */
        12u, /* GL_COPY          -> COLOR_LOGICOP_COPY */
        2u,  /* GL_AND_INVERTED  -> COLOR_LOGICOP_AND_INVERTED */
        10u, /* GL_NOOP          -> COLOR_LOGICOP_NOOP */
        6u,  /* GL_XOR           -> COLOR_LOGICOP_XOR */
        14u, /* GL_OR            -> COLOR_LOGICOP_OR */
        1u,  /* GL_NOR           -> COLOR_LOGICOP_NOR */
        9u,  /* GL_EQUIV         -> COLOR_LOGICOP_EQUIV */
        5u,  /* GL_INVERT        -> COLOR_LOGICOP_INVERT */
        13u, /* GL_OR_REVERSE    -> COLOR_LOGICOP_OR_REVERSE */
        3u,  /* GL_COPY_INVERTED -> COLOR_LOGICOP_COPY_INVERTED */
        11u, /* GL_OR_INVERTED   -> COLOR_LOGICOP_OR_INVERTED */
        7u,  /* GL_NAND          -> COLOR_LOGICOP_NAND */
        15u, /* GL_SET           -> COLOR_LOGICOP_SET */
    };
    return mapping[opcode & 0x0fu];
}

/* One logic op applied to one byte, straight from the truth-table definition above. */
static inline uint32_t gl_logicop_apply(uint32_t mode, uint32_t s, uint32_t d) {
    uint32_t r = 0u;
    if (mode & 1u)
        r |= ~s & ~d; /* src 0, dst 0 */
    if (mode & 2u)
        r |= ~s & d; /* src 0, dst 1 */
    if (mode & 4u)
        r |= s & ~d; /* src 1, dst 0 */
    if (mode & 8u)
        r |= s & d; /* src 1, dst 1 */
    return r & 0xffu;
}

/*
 * `CB_COLOR_CONTROL` from the GL logic-op state.
 *
 * Off, this is exactly the 0x00cc0010 the frame table carried as a constant - `MODE`
 * CB_NORMAL and `ROP3` 0xcc, ROP3_COPY - so gl-cube's stream is unchanged. On, it is
 * radeonsi's recipe (gallium/drivers/radeonsi/si_state.c:341 and :365-368): the
 * four-bit mode repeated into both halves of the eight-bit ROP3, and GL_COPY treated as
 * off. It also sets `DISABLE_DUAL_QUAD` (si_state.c:545-549), which radeonsi does for a
 * logic op whenever `rbplus_allowed`, and that is every GFX10_3 part
 * (amd/common/ac_gpu_info.c:1117 and :1122-1125).
 *
 * Fields from `oops-mesa/mesa/src/amd/registers/gfx103.json`: `DISABLE_DUAL_QUAD` [0],
 * `MODE` [4,6], `ROP3` [16,23].
 */
/* `DISABLE_DUAL_QUAD` (bit 0) turns off the render backend's RB+ path, which packs
 * several pixels into one wide transaction. It stays clear for blending: Mesa disables
 * RB+ for dual-source blending and logic op on every generation, and for all blending
 * only on GFX11; on GFX10_3 it leaves it on, so this does too. A blend needs the
 * half-float export format instead (`gl_ps_patch_export`). */
static inline uint32_t gl_compute_cb_color_control(const gl_context_t *ctx) {
    const uint32_t mode_normal = 1u << 4; /* CBMode CB_NORMAL */
    if (ctx && ctx->cap_color_logic_op) {
        const uint32_t op = gl_logicop_mode(ctx->logic_op);
        if (op != 12u) {                                        /* COLOR_LOGICOP_COPY */
            return mode_normal | ((op | (op << 4)) << 16) | 1u; /* DISABLE_DUAL_QUAD */
        }
    }
    return mode_normal | (0xccu << 16); /* ROP3_COPY */
}

/* A GL blend equation as the colour block's `CombFunc`, same file, enum `CombFunc`. The
 * two subtractions are not interchangeable: `COMB_SRC_MINUS_DST` is GL's
 * `GL_FUNC_SUBTRACT` and `COMB_DST_MINUS_SRC` is `GL_FUNC_REVERSE_SUBTRACT`, and
 * swapping them negates the result. */
static inline uint32_t gl_blend_comb(GLenum equation) {
    switch (equation) {
    case GL_FUNC_SUBTRACT:
        return 1u; /* COMB_SRC_MINUS_DST */
    case GL_MIN:
        return 2u; /* COMB_MIN_DST_SRC */
    case GL_MAX:
        return 3u; /* COMB_MAX_DST_SRC */
    case GL_FUNC_REVERSE_SUBTRACT:
        return 4u; /* COMB_DST_MINUS_SRC */
    default:
        return 0u; /* COMB_DST_PLUS_SRC, i.e. GL_FUNC_ADD */
    }
}

/*
 * `CB_BLEND0_CONTROL` from the GL blend state.
 *
 * Field positions and both enums from `oops-mesa/mesa/src/amd/registers/gfx103.json`
 * and its `gfx10.json` base, which agree: `COLOR_SRCBLEND` [0,4], `COLOR_COMB_FCN`
 * [5,7], `COLOR_DESTBLEND` [8,12], `ALPHA_SRCBLEND` [16,20], `ALPHA_COMB_FCN` [21,23],
 * `ALPHA_DESTBLEND` [24,28], `SEPARATE_ALPHA_BLEND` [29], `ENABLE` [30].
 *
 * `SEPARATE_ALPHA_BLEND` is set only when the alpha state differs from the colour
 * state, as radeonsi does (`si_state.c:499`: `if (srcA != srcRGB || dstA != dstRGB ||
 * eqA != eqRGB)`); with the two sets equal the block's inference is exact.
 *
 * The bit is not free on this part. With it set, the colour block applies
 * `COLOR_COMB_FCN` to channels 0 and 2 and `ALPHA_COMB_FCN` to channels 1 and 3 -
 * alternating by position, not by channel identity (obSCEne, `166-agc/compiled-ps`:
 * swapping the two combines inverts the pattern, and masking to one channel leaves
 * each channel's combine unchanged). Setting it with equal state would hand green an
 * equation GL never asked it to use.
 */
/* Set by `oops_gl_set_log_level`; see GL/gl.h for what each level covers. */
extern int gl_log_level;

/* Whether a texture's descriptor may point at its mip chain, from `/app0/oops-gl`'s
 * `mipchain` key at context creation. Default 1. See the note where it is read: with it
 * off, the descriptor names the base level's own storage, which is what a frame capture
 * carries and what a replay on the software rasteriser draws - so the two become
 * comparable. */
extern int gl_mipchain_enabled;

static inline uint32_t gl_compute_cb_blend_control(const gl_context_t *ctx) {
    if (!ctx || !ctx->cap_blend)
        return 0u;
    /* A logic op replaces blending, so the blender is left off and CB_COLOR_CONTROL's
     * ROP3 does the work (see gl_compute_cb_color_control). */
    if (ctx->cap_color_logic_op)
        return 0u;
    /* Each channel group's own equation (GL 2.0, `glBlendEquationSeparate`), into the
     * register's separate `COLOR_COMB_FCN` and `ALPHA_COMB_FCN` fields. */
    const uint32_t comb = gl_blend_comb(ctx->blend_equation);
    const uint32_t acomb = gl_blend_comb(ctx->blend_equation_alpha);
    /* GL_MIN and GL_MAX ignore the factors entirely - the hardware takes them from the
     * operands, so the factor fields are set to ONE to keep them from contributing. Per
     * group, because the two equations may differ. */
    const GLboolean minmax =
        (ctx->blend_equation == GL_MIN || ctx->blend_equation == GL_MAX) ? GL_TRUE
                                                                         : GL_FALSE;
    const GLboolean aminmax =
        (ctx->blend_equation_alpha == GL_MIN || ctx->blend_equation_alpha == GL_MAX)
            ? GL_TRUE
            : GL_FALSE;
    const uint32_t csrc = minmax ? 1u : gl_blend_op(ctx->blend_src, 4u);
    const uint32_t cdst = minmax ? 1u : gl_blend_op(ctx->blend_dst, 5u);
    const uint32_t asrc = aminmax ? 1u : gl_blend_op(ctx->blend_src_alpha, 4u);
    const uint32_t adst = aminmax ? 1u : gl_blend_op(ctx->blend_dst_alpha, 5u);
    /* Compared as the register's own fields rather than as the GL enums, so two
     * spellings of the same hardware factor - which `gl_blend_op` maps together - do
     * not read as different. */
    const GLboolean separate =
        (GLboolean)(asrc != csrc || adst != cdst || acomb != comb);
    return (csrc & 0x1fu) | ((comb & 0x7u) << 5) | ((cdst & 0x1fu) << 8) |
           ((asrc & 0x1fu) << 16) | ((acomb & 0x7u) << 21) | ((adst & 0x1fu) << 24) |
           (separate ? (1u << 29) : 0u) | /* SEPARATE_ALPHA_BLEND - see above */
           (1u << 30);                    /* ENABLE */
}

/*
 * `PA_CL_VPORT_XSCALE`, `XOFFSET`, `YSCALE`, `YOFFSET` from the GL viewport.
 *
 * For a viewport covering the whole target these reduce to `w/2`, `w/2`, `-h/2`, `h/2`,
 * the values the gl-cube oracle frame carries.
 *
 * `fb_h` is the render target's height, not the viewport's: GL measures the viewport
 * from the bottom-left and the rows grow downward, so the offset is the distance from
 * the top of the target to the middle of the viewport.
 */
static inline void gl_compute_vport(const gl_context_t *ctx, uint32_t fb_h,
                                    uint32_t out[4]) {
    const float vx = (float)ctx->vp_x;
    const float vy = (float)ctx->vp_y;
    const float vw = (float)ctx->vp_w;
    const float vh = (float)ctx->vp_h;
    out[0] = gl_f32_bits(vw * 0.5f);      /* XSCALE  */
    out[1] = gl_f32_bits(vx + vw * 0.5f); /* XOFFSET */
    out[2] = gl_f32_bits(-vh * 0.5f);     /* YSCALE: NDC +y is up, rows grow down */
    out[3] = gl_f32_bits((float)fb_h - vy - vh * 0.5f); /* YOFFSET */
}

/* PA_SC_VPORT_SCISSOR_0_TL / _BR from the GL scissor box.
 *
 * The scan converter's rectangle is y-down over the render target; GL measures its box
 * from the bottom-left, so the same flip gl_compute_vport applies applies here. The box
 * is clamped to the target because the register fields are unsigned and a box hanging
 * off the left or top would otherwise wrap to an enormous coordinate.
 *
 * With the test disabled - and for a box covering the whole target - this is the full
 * extent, the measured recipe's value, so a title that never calls glScissor emits the
 * oracle stream.
 *
 * Fields TL_X / BR_X [0,14], TL_Y / BR_Y [16,30], WINDOW_OFFSET_DISABLE [31], from
 * mesa/src/amd/registers/gfx103.json (types PA_SC_WINDOW_SCISSOR_TL and _BR; the
 * VPORT_SCISSOR registers at 0x028250 reference them). Only the viewport scissor takes
 * the GL box: the screen, window and generic rectangles are the surface bounds and stay
 * at the full extent.
 */
static inline void gl_compute_scissor(const gl_context_t *ctx, uint32_t fb_w,
                                      uint32_t fb_h, uint32_t out[2]) {
    int32_t l = 0, t = 0;
    int32_t r = (int32_t)fb_w, b = (int32_t)fb_h;
    if (ctx && ctx->cap_scissor_test) {
        const int32_t sw = ctx->sc_w > 0 ? (int32_t)ctx->sc_w : 0;
        const int32_t sh = ctx->sc_h > 0 ? (int32_t)ctx->sc_h : 0;
        l = ctx->sc_x;
        r = ctx->sc_x + sw;
        t = (int32_t)fb_h - (ctx->sc_y + sh);
        b = (int32_t)fb_h - ctx->sc_y;
        if (l < 0)
            l = 0;
        if (t < 0)
            t = 0;
        if (r > (int32_t)fb_w)
            r = (int32_t)fb_w;
        if (b > (int32_t)fb_h)
            b = (int32_t)fb_h;
        if (r < l)
            r = l;
        if (b < t)
            b = t;
    }
    out[0] = 0x80000000u | (((uint32_t)t & 0x7fffu) << 16) | ((uint32_t)l & 0x7fffu);
    out[1] = (((uint32_t)b & 0x7fffu) << 16) | ((uint32_t)r & 0x7fffu);
}

/* Rendering pipeline */
void gl_rasterize_triangle(gl_context_t *ctx, const gl_screen_vertex_t *v0,
                           const gl_screen_vertex_t *v1, const gl_screen_vertex_t *v2);
/* Which site submitted the frame, by name. A submit is a GPU round-trip, the most
 * expensive thing this layer does, and many places can cause one: a full command
 * buffer, a full vertex ring, a full descriptor ring, a query read, a pixel-shader
 * payload edit, a flip. The same `__func__`-at-the-call-site macro as
 * `gl_record_error`. */
void gl_hw_flush_at(gl_context_t *ctx, const char *fn);
#define gl_hw_flush(ctx) gl_hw_flush_at((ctx), __func__)
/* An occlusion query's two ends on the hardware path (gl_draw.c). `gl_hw_query_begin`
 * clears the counter slots and arms the draw path, which takes the begin snapshot at
 * the first draw that has a depth surface bound - never before one, because
 * ZPASS_ENABLE with no depth target stalls the depth block and never retires the fence.
 * `gl_hw_query_end` takes the closing snapshot, puts
 * DB_COUNT_CONTROL back, submits, and returns the sum over the render backends - or 0
 * with `*counted` false when no draw ever armed it. */
void gl_hw_query_begin(gl_context_t *ctx);
uint64_t gl_hw_query_end(gl_context_t *ctx, GLboolean *counted);

/* What gl_hw_flush appends after the last packet of a stream: two RELEASE_MEM events of
 * 8 dwords, a WAIT_REG_MEM of 7, the readback DMA copy of 7 (one chunk - the largest
 * render target is far under DMA_DATA's 64 MiB), and 16 dwords of padding - 46, rounded
 * up. Every check that decides whether a packet still fits has to leave this much
 * behind it, or the stream that fitted is closed past the end of the command buffer. */
#define OOPS_GL_DCB_TRAILER_DW 64u

/* The most one triangle can add to the stream; the sum is itemised where it is used, in
 * gl_draw.c, and anything added per draw has to be added to it. */
#define OOPS_GL_DCB_DRAW_MAX_DW 232u /* itemised in gl_draw.c */

/*
 * The vertex ring, and the command stream, sized so a frame fits in one submit.
 *
 * `gl_draw_triangle_pv_body` flushes when either would overflow, and a flush submits
 * the stream and waits for the GPU to finish it.
 *
 * One number, in bytes, used by both the allocation and the bound, so the ring cannot
 * be raised without the allocation.
 */
#define OOPS_GL_VBO_RING_BYTES                                                         \
    0x400000u /* 4 MiB: 29,127 triangles at the 144-byte worst case */
#define OOPS_GL_VBO_RING_TRIANGLES (OOPS_GL_VBO_RING_BYTES / 144u)

/*
 * How much of that ring a frame may use before it submits, from `/app0/oops-gl`'s
 * `vbobytes` key. The allocation stays `OOPS_GL_VBO_RING_BYTES`; this only moves the
 * bound that triggers a flush, so a smaller value is always safe and only means
 * flushing more often.
 *
 * A flush is also where the descriptor ring, the shader variant ring and the vertex
 * cursor all start again, so this knob separates a fault that does not survive a whole
 * frame from one that does, without a rebuild.
 */
extern uint32_t gl_vbo_ring_bytes;

/* The command stream, in dwords, sized to hold a frame. `OOPS_GL_DCB_DRAW_MAX_DW`
 * above is what each check reserves, and it is the worst case rather than the typical
 * one. */
#define OOPS_GL_DCB_CAPACITY_DW 0x80000u /* 512 Ki dwords, 2 MiB */
void gl_hw_fail(gl_context_t *ctx, const char *reason);
void gl_hw_emit_dma_fill(uint32_t **dw_ptr, uint64_t dst, uint32_t value,
                         uint32_t bytes);
void gl_hw_emit_dma_copy(uint32_t **dw_ptr, uint64_t src, uint64_t dst, uint32_t bytes);
void gl_hw_clear(gl_context_t *ctx, GLbitfield mask, uint32_t colour, float depth);
void gl_draw_primitive_triangle(gl_context_t *ctx, const gl_vertex_t *v0,
                                const gl_vertex_t *v1, const gl_vertex_t *v2);

/* -------------------------------------------------------------------------
 * GL 2.0's object model (gl_shader.c, glsl_link.c)
 * ------------------------------------------------------------------------- */

/* Links a program from two compiled stages, either of which may be NULL - a program
 * with only a vertex shader leaves the fragment stage fixed-function, and the reverse.
 * False with `info_log` written is a link failure and the caller sets GL_LINK_STATUS
 * from it. References to the units are taken on success; the previous link's are
 * dropped either way, so a failed relink leaves a program that is not linked rather
 * than one still running the old code. */
GLboolean gl_program_link(gl_context_t *ctx, gl_program_object_t *p, glsl_unit_t *vs,
                          glsl_unit_t *fs);
/* The slot a name occupies, or NULL. Names are never reused within a context, so a name
 * from a destroyed object finds nothing rather than whatever took its place. */
gl_shader_object_t *gl_find_shader(gl_context_t *ctx, GLuint name);
gl_program_object_t *gl_find_program(gl_context_t *ctx, GLuint name);
/* The program `glUseProgram` made current and that has linked, or NULL - which is the
 * one question the draw path asks. A current program that failed its last relink draws
 * nothing and is an error at draw time, not a silent fall back to fixed function. */
gl_program_object_t *gl_active_program(gl_context_t *ctx);
/* Frees every shader and program a context owns, at glContextDestroy. */
void gl_free_all_shaders(gl_context_t *ctx);

/* -------------------------------------------------------------------------
 * The console back end (glsl_ps.c)
 *
 * Only the fragment stage is compiled. oops-gl's hardware vertex shader is a
 * passthrough - the CPU builds each vertex already in clip space and the shader loads
 * and exports it - so a GL 2.0 vertex shader runs on the CPU in `glsl_exec.c`, writing
 * the same vertex the fixed-function path writes. The fragment stage has no CPU
 * standing in for it on the console, which is why it is the half that needs a compiler.
 * ------------------------------------------------------------------------- */

/* Compiles a linked program's fragment stage into a complete gfx1030 pixel shader: the
 * varyings interpolated, the body, and the colour exported. `out_count` is how many
 * words it wrote, `out_vgprs` how much of the register file it needs, for the shader's
 * resource register, and `out_user_sgprs` how many user SGPRs the draw has to configure
 * for it - see `hw_ps_user_sgprs`. `out_user_sgprs` may be null.
 *
 * A program with no fragment stage succeeds with `out_count` zero - the
 * fixed-function pixel shader in the payload is what runs for it, and there is nothing
 * to compile. False is a real failure, with `log` saying what the compiler would not
 * generate. */
GLboolean gl_program_compile_fragment(const gl_program_object_t *p, uint32_t *words,
                                      uint32_t capacity, uint32_t *out_count,
                                      uint32_t *out_vgprs, uint32_t *out_user_sgprs,
                                      uint32_t *out_input_ena, char *log,
                                      size_t log_size);
GLboolean gl_program_compile_vertex(const gl_program_object_t *p, uint32_t *words,
                                    uint32_t capacity, uint32_t *out_count,
                                    uint32_t *out_vgprs, uint32_t *out_user_sgprs,
                                    char *log, size_t log_size);

/* Submit before editing a shader the GPU may not have read yet. The draws already in
 * the stream were built against the words that are there now; changing them first
 * would have the GPU run the new program for the old draws. Only when the words
 * change. */
void gl_ps_sync_payload_edit(gl_context_t *ctx, const uint32_t *dst,
                             const uint32_t *words, size_t n);
/* A pixel-shader slot compared and recorded through the cached mirror rather than read
   back out of ONION - see `hw_ps_untex_shadow`. `gl_ps_shadow_seed` fills the mirror
   from the masters once they are built, so the first comparison is against what is
   really in the payload. */
GLboolean gl_ps_slot_same(gl_context_t *ctx, const uint32_t *dst, const uint32_t *words,
                          size_t n);
void gl_ps_slot_wrote(gl_context_t *ctx, const uint32_t *dst, const uint32_t *words,
                      size_t n);
void gl_ps_shadow_seed(gl_context_t *ctx);
/* Which copy of the textured shader a draw should bind, as a payload offset, and
   whether binding it would first need the frame submitted to free a slot. */
uint32_t gl_ps_ring_offset(gl_context_t *ctx);
GLboolean gl_ps_ring_needs_submit(gl_context_t *ctx);
/* Every pixel shader out of this core's caches after an edit: the payload is
 * write-combined and the command processor reads what has left the core. */
void gl_ps_flush_shaders(gl_context_t *ctx);

/* -------------------------------------------------------------------------
 * The interpolated block between the two programmable stages (glsl_exec.c)
 *
 * A vertex invocation writes it, the rasteriser interpolates it, a fragment invocation
 * reads it. It carries the fixed-function interpolants as well as the program's own
 * varyings, laid out after them at fixed offsets - `gl_Color`, `gl_TexCoord[]` and
 * the rest.
 *
 * Keeping them in the same block is what makes a half-programmable pipeline work: a
 * program with only a vertex shader writes `gl_FrontColor` and `gl_TexCoord[0]` and the
 * fixed-function fragment stage reads them, and a program with only a fragment shader
 * reads what the fixed-function vertex stage wrote. It is also what makes the
 * derivatives uniform - one block is interpolated at the pixel, one pixel right and one
 * pixel down, so `dFdx` of anything that came through here is exact rather than
 * approximated.
 * ------------------------------------------------------------------------- */
#define GL_SHADER_VARY_FF_BASE OOPS_GL_MAX_VARYING_FLOATS
#define GL_SHADER_VARY_COLOR (GL_SHADER_VARY_FF_BASE + 0)
#define GL_SHADER_VARY_SECONDARY (GL_SHADER_VARY_FF_BASE + 4)
#define GL_SHADER_VARY_TEXCOORD (GL_SHADER_VARY_FF_BASE + 8)
#define GL_SHADER_VARY_FOG (GL_SHADER_VARY_TEXCOORD + 4 * OOPS_GL_MAX_TEXTURE_UNITS)
#define GL_SHADER_VARY_FLOATS (GL_SHADER_VARY_FOG + 1)

/* What one vertex invocation produces. */
typedef struct {
    float position[4];    /* gl_Position, in clip coordinates */
    float point_size;     /* gl_PointSize, or the context's glPointSize */
    float clip_vertex[4]; /* gl_ClipVertex, for the user clip planes */
    GLboolean wrote_clip_vertex;
    float vary[GL_SHADER_VARY_FLOATS];
} gl_shader_vertex_out_t;

/* Runs a program's vertex shader for one vertex. `ff` supplies the fixed-function
 * attributes - `gl_Vertex`, `gl_Normal`, `gl_Color`, `gl_MultiTexCoord*` - which a
 * shader may read alongside its own generic ones. False with nothing written when the
 * shader could not be run, which is an error at the draw rather than a frame that
 * quietly loses its geometry. */
GLboolean gl_shader_run_vertex(gl_context_t *ctx, gl_program_object_t *p,
                               const gl_vertex_t *ff, gl_shader_vertex_out_t *out);

/* What one fragment invocation reads and writes. The three interpolated blocks are the
 * pixel, one pixel right and one pixel down - which is how a derivative is taken. */
typedef struct {
    const float *vary; /* GL_SHADER_VARY_FLOATS, at the pixel */
    const float
        *vary_dx; /* the same, one pixel right; NULL when nothing needs a derivative */
    const float *vary_dy;
    float frag_coord[4]; /* gl_FragCoord: window x, y, depth, and 1/w */
    float frag_coord_dx[4];
    float frag_coord_dy[4];
    GLboolean front_facing;
} gl_shader_fragment_in_t;

typedef struct {
    float colour[4]; /* gl_FragColor */
    float depth;     /* gl_FragDepth, when the shader wrote one */
    GLboolean wrote_depth;
    GLboolean discarded;
} gl_shader_fragment_out_t;

GLboolean gl_shader_run_fragment(gl_context_t *ctx, gl_program_object_t *p,
                                 const gl_shader_fragment_in_t *in,
                                 gl_shader_fragment_out_t *out);

/* Whether a program's fragment stage reads anything whose value depends on a
 * neighbouring pixel - a mipmapped texture lookup, `dFdx`, `dFdy` or `fwidth`. A
 * fragment that needs none of them is evaluated once instead of three times, which is
 * most of them. */
GLboolean gl_shader_needs_derivatives(const gl_program_object_t *p);

/* One texel for a GLSL texture lookup: the texture `target` names on `unit`, sampled at
 * `coord` with `bias` added to the level of detail. `lod` is the level the derivatives
 * gave, or -1000 for "the base level" when nothing needed one.
 *
 * The texture enables do not apply. With a program in use the sampler's declared
 * type names the target and `glEnable(GL_TEXTURE_2D)` means nothing (GL 2.0, 3.8.15) -
 * which is why this is not `gl_unit_texture_id`. False when nothing complete is bound,
 * in which case the specification leaves the result undefined and this writes opaque
 * black. */
GLboolean gl_shader_sample(const gl_context_t *ctx, GLuint unit, GLenum sampler_type,
                           const float coord[4], float lod, float out[4]);

#endif /* __GL_INTERNAL_H__ */
