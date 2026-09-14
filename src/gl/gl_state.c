/*
 * oops-gl: State management, capability switches, and clear operations
 */

#include "gl_internal.h"

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->vp_x = x;
    ctx->vp_y = y;
    ctx->vp_w = width;
    ctx->vp_h = height;
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->sc_x = x;
    ctx->sc_y = y;
    ctx->sc_w = width;
    ctx->sc_h = height;
}

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->clear_color[0] = (float)red;
    ctx->clear_color[1] = (float)green;
    ctx->clear_color[2] = (float)blue;
    ctx->clear_color[3] = (float)alpha;
}

void glClearDepth(GLclampd depth) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->clear_depth = (float)depth;
}

void glClear(GLbitfield mask) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    size_t total_px = (size_t)ctx->width * (size_t)ctx->height;

    uint32_t ir = (uint32_t)(ctx->clear_color[0] * 255.0f + 0.5f);
    uint32_t ig = (uint32_t)(ctx->clear_color[1] * 255.0f + 0.5f);
    uint32_t ib = (uint32_t)(ctx->clear_color[2] * 255.0f + 0.5f);
    uint32_t ia = (uint32_t)(ctx->clear_color[3] * 255.0f + 0.5f);
    if (ir > 255) ir = 255;
    if (ig > 255) ig = 255;
    if (ib > 255) ib = 255;
    if (ia > 255) ia = 255;
    uint32_t col = (ia << 24) | (ir << 16) | (ig << 8) | ib;

#ifndef OOPS_HOST_BUILD
    if (ctx->use_hardware) {
        /* The GPU clears its own targets; the CPU never writes them on this path. */
        gl_hw_clear(ctx, mask, col, ctx->clear_depth);
        if (mask & GL_COLOR_BUFFER_BIT) ctx->fb_cleared = GL_TRUE;
        return;
    }
#endif

    if (mask & GL_COLOR_BUFFER_BIT) {
        uint32_t *fb = ctx->framebuffer;
        if (fb) {
            for (size_t i = 0; i < total_px; i++) {
                fb[i] = col;
            }
            ctx->fb_cleared = GL_TRUE;
        }
    }

    if (mask & GL_DEPTH_BUFFER_BIT) {
        float *db = ctx->depth_buffer;
        float cd = ctx->clear_depth;
        if (db) {
            uint32_t cd_raw;
            memcpy(&cd_raw, &cd, 4);
            uint64_t cd_raw64 = ((uint64_t)cd_raw << 32) | cd_raw;
            uint64_t *db64 = (uint64_t *)db;
            size_t depth_px = ctx->depth_px ? ctx->depth_px : total_px; /* the whole tiled extent */
            size_t total_qwords = depth_px / 2;
            for (size_t i = 0; i < total_qwords; i++) {
                db64[i] = cd_raw64;
            }
            if (depth_px & 1) {
                db[depth_px - 1] = cd;
            }
        }
    }
}

void glEnable(GLenum cap) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + GL_MAX_LIGHTS) {
        ctx->lights[(size_t)(cap - GL_LIGHT0)].enabled = GL_TRUE;
        return;
    }

    switch (cap) {
        case GL_DEPTH_TEST:     ctx->cap_depth_test = GL_TRUE; break;
        case GL_CULL_FACE:      ctx->cap_cull_face = GL_TRUE; break;
        case GL_BLEND:          ctx->cap_blend = GL_TRUE; break;
        case GL_SCISSOR_TEST:   ctx->cap_scissor_test = GL_TRUE; break;
        case GL_LIGHTING:       ctx->cap_lighting = GL_TRUE; break;
        case GL_TEXTURE_2D:     ctx->cap_texture_2d = GL_TRUE; break;
        case GL_NORMALIZE:      ctx->cap_normalize = GL_TRUE; break;
        case GL_COLOR_MATERIAL: ctx->cap_color_material = GL_TRUE; break;
        default: break;
    }
}

void glDisable(GLenum cap) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + GL_MAX_LIGHTS) {
        ctx->lights[(size_t)(cap - GL_LIGHT0)].enabled = GL_FALSE;
        return;
    }

    switch (cap) {
        case GL_DEPTH_TEST:     ctx->cap_depth_test = GL_FALSE; break;
        case GL_CULL_FACE:      ctx->cap_cull_face = GL_FALSE; break;
        case GL_BLEND:          ctx->cap_blend = GL_FALSE; break;
        case GL_SCISSOR_TEST:   ctx->cap_scissor_test = GL_FALSE; break;
        case GL_LIGHTING:       ctx->cap_lighting = GL_FALSE; break;
        case GL_TEXTURE_2D:     ctx->cap_texture_2d = GL_FALSE; break;
        case GL_NORMALIZE:      ctx->cap_normalize = GL_FALSE; break;
        case GL_COLOR_MATERIAL: ctx->cap_color_material = GL_FALSE; break;
        default: break;
    }
}

GLboolean glIsEnabled(GLenum cap) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GL_FALSE;

    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + GL_MAX_LIGHTS) {
        return ctx->lights[(size_t)(cap - GL_LIGHT0)].enabled;
    }

    switch (cap) {
        case GL_DEPTH_TEST:     return ctx->cap_depth_test;
        case GL_CULL_FACE:      return ctx->cap_cull_face;
        case GL_BLEND:          return ctx->cap_blend;
        case GL_SCISSOR_TEST:   return ctx->cap_scissor_test;
        case GL_LIGHTING:       return ctx->cap_lighting;
        case GL_TEXTURE_2D:     return ctx->cap_texture_2d;
        case GL_NORMALIZE:      return ctx->cap_normalize;
        case GL_COLOR_MATERIAL: return ctx->cap_color_material;
        default: return GL_FALSE;
    }
}

void glBlendFunc(GLenum sfactor, GLenum dfactor) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->blend_src = sfactor;
    ctx->blend_dst = dfactor;
    ctx->blend_src_alpha = sfactor;
    ctx->blend_dst_alpha = dfactor;
}

void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->blend_src = sfactorRGB;
    ctx->blend_dst = dfactorRGB;
    ctx->blend_src_alpha = sfactorAlpha;
    ctx->blend_dst_alpha = dfactorAlpha;
}

void glBlendEquation(GLenum mode) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->blend_equation = mode;
}

void glTexEnvi(GLenum target, GLenum pname, GLint param) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE) {
        ctx->tex_env_mode = (GLenum)param;
    }
}

void glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
    glTexEnvi(target, pname, (GLint)param);
}

void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (target == GL_TEXTURE_ENV) {
        if (pname == GL_TEXTURE_ENV_COLOR) {
            ctx->tex_env_color[0] = params[0];
            ctx->tex_env_color[1] = params[1];
            ctx->tex_env_color[2] = params[2];
            ctx->tex_env_color[3] = params[3];
        } else if (pname == GL_TEXTURE_ENV_MODE) {
            ctx->tex_env_mode = (GLenum)params[0];
        }
    }
}

void glDepthFunc(GLenum func) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->depth_func = func;
}

void glDepthMask(GLboolean flag) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->depth_mask = flag;
}

void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->color_mask[0] = red;
    ctx->color_mask[1] = green;
    ctx->color_mask[2] = blue;
    ctx->color_mask[3] = alpha;
}

void glCullFace(GLenum mode) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->cull_mode = mode;
}

void glFrontFace(GLenum mode) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->front_face = mode;
}

void glShadeModel(GLenum mode) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->shade_model = mode;
}

GLenum glGetError(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GL_NO_ERROR;
    GLenum err = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    return err;
}

const GLubyte *glGetString(GLenum name) {
    switch (name) {
        case GL_VENDOR:      return (const GLubyte *)"OOPS Project";
        case GL_RENDERER:    return (const GLubyte *)"Sony PlayStation 5 RDNA2 Freestanding";
        case GL_VERSION:     return (const GLubyte *)"OpenGL 1.3 oops-gl 2.0";
        case GL_EXTENSIONS:  return (const GLubyte *)"GL_EXT_vertex_array";
        default: return (const GLubyte *)"";
    }
}

void glGetIntegerv(GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    switch (pname) {
        case GL_VIEWPORT:
            params[0] = ctx->vp_x;
            params[1] = ctx->vp_y;
            params[2] = ctx->vp_w;
            params[3] = ctx->vp_h;
            break;
        case GL_SCISSOR_BOX:
            params[0] = ctx->sc_x;
            params[1] = ctx->sc_y;
            params[2] = ctx->sc_w;
            params[3] = ctx->sc_h;
            break;
        case GL_MATRIX_MODE:
            params[0] = (GLint)ctx->matrix_mode;
            break;
        case GL_TEXTURE_BINDING_2D:
            params[0] = (GLint)ctx->bound_texture_2d;
            break;
        case GL_BLEND_SRC:
        case GL_BLEND_SRC_RGB:
            params[0] = (GLint)ctx->blend_src;
            break;
        case GL_BLEND_DST:
        case GL_BLEND_DST_RGB:
            params[0] = (GLint)ctx->blend_dst;
            break;
        case GL_BLEND_SRC_ALPHA:
            params[0] = (GLint)ctx->blend_src_alpha;
            break;
        case GL_BLEND_DST_ALPHA:
            params[0] = (GLint)ctx->blend_dst_alpha;
            break;
        case GL_BLEND_EQUATION:
            params[0] = (GLint)ctx->blend_equation;
            break;
        case GL_TEXTURE_ENV_MODE:
            params[0] = (GLint)ctx->tex_env_mode;
            break;
        default:
            break;
    }
}

void glGetFloatv(GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    switch (pname) {
        case GL_MODELVIEW_MATRIX:
            for (int i = 0; i < 16; i++) {
                params[i] = ctx->modelview_stack[ctx->modelview_depth].m[i];
            }
            break;
        case GL_PROJECTION_MATRIX:
            for (int i = 0; i < 16; i++) {
                params[i] = ctx->projection_stack[ctx->projection_depth].m[i];
            }
            break;
        default:
            break;
    }
}

void glGetBooleanv(GLenum pname, GLboolean *params) {
    if (!params) return;
    *params = glIsEnabled(pname);
}

/* -------------------------------------------------------------------------
 * Fixed-Function Lighting & Materials
 * ------------------------------------------------------------------------- */

void glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + GL_MAX_LIGHTS) {
        ctx->last_error = GL_INVALID_ENUM;
        return;
    }
    size_t idx = (size_t)(light - GL_LIGHT0);
    gl_light_t *l = &ctx->lights[idx];

    switch (pname) {
        case GL_AMBIENT:
            memcpy(l->ambient, params, 4 * sizeof(float));
            break;
        case GL_DIFFUSE:
            memcpy(l->diffuse, params, 4 * sizeof(float));
            break;
        case GL_SPECULAR:
            memcpy(l->specular, params, 4 * sizeof(float));
            break;
        case GL_POSITION: {
            /* Position is transformed by current ModelView matrix into eye space */
            const gl_mat4_t *mv = &ctx->modelview_stack[ctx->modelview_depth];
            mat4_transform_vec4(l->position, mv, params);
            break;
        }
        case GL_SPOT_DIRECTION: {
            const gl_mat4_t *mv = &ctx->modelview_stack[ctx->modelview_depth];
            float dx = params[0], dy = params[1], dz = params[2];
            l->spot_direction[0] = mv->m[0] * dx + mv->m[4] * dy + mv->m[8] * dz;
            l->spot_direction[1] = mv->m[1] * dx + mv->m[5] * dy + mv->m[9] * dz;
            l->spot_direction[2] = mv->m[2] * dx + mv->m[6] * dy + mv->m[10] * dz;
            float len = gl_sqrt(l->spot_direction[0] * l->spot_direction[0] +
                                l->spot_direction[1] * l->spot_direction[1] +
                                l->spot_direction[2] * l->spot_direction[2]);
            if (len > 1e-6f) {
                float inv = 1.0f / len;
                l->spot_direction[0] *= inv;
                l->spot_direction[1] *= inv;
                l->spot_direction[2] *= inv;
            }
            break;
        }
        case GL_SPOT_EXPONENT:
            l->spot_exponent = params[0];
            break;
        case GL_SPOT_CUTOFF:
            l->spot_cutoff = params[0];
            if (params[0] <= 90.0f) {
                float rad = params[0] * (3.14159265f / 180.0f);
                l->spot_cutoff_cos = gl_cos(rad);
            } else {
                l->spot_cutoff_cos = -1.0f;
            }
            break;
        case GL_CONSTANT_ATTENUATION:
            l->const_att = params[0];
            break;
        case GL_LINEAR_ATTENUATION:
            l->linear_att = params[0];
            break;
        case GL_QUADRATIC_ATTENUATION:
            l->quad_att = params[0];
            break;
        default:
            ctx->last_error = GL_INVALID_ENUM;
            break;
    }
}

void glLightf(GLenum light, GLenum pname, GLfloat param) {
    glLightfv(light, pname, &param);
}

static void apply_material_param(gl_material_t *m, GLenum pname, const GLfloat *params) {
    switch (pname) {
        case GL_AMBIENT:
            memcpy(m->ambient, params, 4 * sizeof(float));
            break;
        case GL_DIFFUSE:
            memcpy(m->diffuse, params, 4 * sizeof(float));
            break;
        case GL_SPECULAR:
            memcpy(m->specular, params, 4 * sizeof(float));
            break;
        case GL_EMISSION:
            memcpy(m->emission, params, 4 * sizeof(float));
            break;
        case GL_SHININESS:
            m->shininess = params[0];
            if (m->shininess < 0.0f) m->shininess = 0.0f;
            if (m->shininess > 128.0f) m->shininess = 128.0f;
            break;
        case GL_AMBIENT_AND_DIFFUSE:
            memcpy(m->ambient, params, 4 * sizeof(float));
            memcpy(m->diffuse, params, 4 * sizeof(float));
            break;
        default: break;
    }
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) {
        apply_material_param(&ctx->mat_front, pname, params);
    }
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) {
        apply_material_param(&ctx->mat_back, pname, params);
    }
}

void glMaterialf(GLenum face, GLenum pname, GLfloat param) {
    glMaterialfv(face, pname, &param);
}

void glLightModelfv(GLenum pname, const GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    switch (pname) {
        case GL_LIGHT_MODEL_AMBIENT:
            memcpy(ctx->light_model_ambient, params, 4 * sizeof(float));
            break;
        case GL_LIGHT_MODEL_LOCAL_VIEWER:
            ctx->light_model_local_viewer = (params[0] != 0.0f) ? GL_TRUE : GL_FALSE;
            break;
        case GL_LIGHT_MODEL_TWO_SIDE:
            ctx->light_model_two_side = (params[0] != 0.0f) ? GL_TRUE : GL_FALSE;
            break;
        default:
            ctx->last_error = GL_INVALID_ENUM;
            break;
    }
}

void glLightModelf(GLenum pname, GLfloat param) {
    glLightModelfv(pname, &param);
}

void glColorMaterial(GLenum face, GLenum mode) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->color_material_face = face;
    ctx->color_material_mode = mode;
}

/* -------------------------------------------------------------------------
 * Texture Object Management
 * ------------------------------------------------------------------------- */

#ifdef OOPS_HOST_BUILD
#include <stdlib.h>
#endif

static gl_texture_object_t *gl_find_texture(gl_context_t *ctx, GLuint id) {
    if (!ctx || id == 0) return NULL;
    for (int i = 0; i < GL_MAX_TEXTURE_OBJECTS; i++) {
        if (ctx->textures[i].used && ctx->textures[i].id == id) {
            return &ctx->textures[i];
        }
    }
    return NULL;
}

static gl_texture_object_t *gl_find_or_create_texture(gl_context_t *ctx, GLuint id) {
    if (!ctx || id == 0) return NULL;
    gl_texture_object_t *tex = gl_find_texture(ctx, id);
    if (tex) return tex;

    for (int i = 0; i < GL_MAX_TEXTURE_OBJECTS; i++) {
        if (!ctx->textures[i].used) {
            tex = &ctx->textures[i];
            memset(tex, 0, sizeof(*tex));
            tex->id = id;
            tex->used = GL_TRUE;
            tex->wrap_s = GL_REPEAT;
            tex->wrap_t = GL_REPEAT;
            tex->min_filter = GL_NEAREST_MIPMAP_LINEAR;
            tex->mag_filter = GL_LINEAR;
            return tex;
        }
    }
    return NULL;
}

static void gl_pack_descriptors(gl_texture_object_t *tex) {
    if (!tex) return;
    uint64_t va = tex->garlic_va;
    uint32_t w = tex->width ? (uint32_t)tex->width : 1u;
    uint32_t h = tex->height ? (uint32_t)tex->height : 1u;

    /* RDNA2 SQ_IMG_RSRC_WORD0..7 (32 bytes), laid out as the public RDNA ISA reference gives them.
     * The base address is in 256-byte units: the unshifted address sent the sampler 256 times too
     * far and drew a wavefront fault on 2026-09-14. */
    tex->img_desc[0] = (uint32_t)(va >> 8);                                   /* BASE_ADDRESS[39:8] */
    tex->img_desc[1] = (uint32_t)((va >> 40) & 0xffu) | (56u << 20) | (((w - 1u) & 3u) << 30); /* BASE_ADDRESS_HI, MIN_LOD=0, FORMAT=8_8_8_8_UNORM, WIDTH_LO */
    tex->img_desc[2] = (((w - 1u) >> 2) & 0x3fffu) | (((h - 1u) & 0x3fffu) << 14) | (1u << 31); /* WIDTH_HI, HEIGHT, RESOURCE_LEVEL */
    tex->img_desc[3] = 0x90000000u | 0xfacu; /* TYPE=2D, SW_MODE=LINEAR_GENERAL, DST_SEL X,Y,Z,W = channels 0,1,2,3 (4,5,6,7) */
    tex->img_desc[4] = 0u; /* DEPTH: unused for a 2D image; it does not carry the linear pitch on this GPU (measured 2026-09-14) */
    tex->img_desc[5] = 0u;
    tex->img_desc[6] = 0u;
    tex->img_desc[7] = 0u;

    /* RDNA2 SQ_IMG_SAMP_WORD0..3 (16 bytes) */
    uint32_t cx = (tex->wrap_s == GL_CLAMP_TO_EDGE || tex->wrap_s == GL_CLAMP) ? 2u : 0u; /* CLAMP_LAST_TEXEL : WRAP */
    uint32_t cy = (tex->wrap_t == GL_CLAMP_TO_EDGE || tex->wrap_t == GL_CLAMP) ? 2u : 0u;
    tex->samp_desc[0] = cx | (cy << 3);
    tex->samp_desc[1] = 0x00fff000u;
    uint32_t mag = (tex->mag_filter == GL_LINEAR) ? 1u : 0u;
    uint32_t min = (tex->min_filter == GL_LINEAR || tex->min_filter == GL_LINEAR_MIPMAP_NEAREST || tex->min_filter == GL_LINEAR_MIPMAP_LINEAR) ? 1u : 0u;
    tex->samp_desc[2] = (mag << 20) | (min << 22);
    tex->samp_desc[3] = 0u;
    tex->desc_dirty = GL_TRUE;
}

void glGenTextures(GLsizei n, GLuint *textures) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !textures || n <= 0) return;

    GLuint next_id = 1;
    for (int i = 0; i < n; i++) {
        while (gl_find_texture(ctx, next_id) != NULL) {
            next_id++;
        }
        gl_texture_object_t *tex = gl_find_or_create_texture(ctx, next_id);
        if (tex) {
            textures[i] = next_id;
            gl_pack_descriptors(tex);
            next_id++;
        } else {
            textures[i] = 0;
        }
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !textures || n <= 0) return;

    for (int i = 0; i < n; i++) {
        GLuint id = textures[i];
        if (id == 0) continue;
        gl_texture_object_t *tex = gl_find_texture(ctx, id);
        if (tex) {
#ifndef OOPS_HOST_BUILD
            if (tex->garlic_data) {
                oops_mem_free(tex->garlic_data);
                tex->garlic_data = NULL;
                tex->pixels = NULL;
            }
#else
            if (tex->pixels) {
                free(tex->pixels);
                tex->pixels = NULL;
            }
#endif
            if (ctx->bound_texture_2d == id) {
                ctx->bound_texture_2d = 0;
            }
            memset(tex, 0, sizeof(*tex));
        }
    }
}

void glBindTexture(GLenum target, GLuint texture) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) {
        ctx->last_error = GL_INVALID_ENUM;
        return;
    }

    if (texture == 0) {
        ctx->bound_texture_2d = 0;
        return;
    }

    gl_texture_object_t *tex = gl_find_or_create_texture(ctx, texture);
    if (tex) {
        ctx->bound_texture_2d = texture;
    }
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                 GLsizei width, GLsizei height, GLint border,
                 GLenum format, GLenum type, const GLvoid *pixels) {
    (void)level; (void)internalformat; (void)border;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) {
        ctx->last_error = GL_INVALID_ENUM;
        return;
    }
    if (width <= 0 || height <= 0) {
        ctx->last_error = GL_INVALID_VALUE;
        return;
    }

    gl_texture_object_t *tex = gl_find_or_create_texture(ctx, ctx->bound_texture_2d ? ctx->bound_texture_2d : 1);
    if (!tex) return;

    size_t num_pixels = (size_t)width * (size_t)height;
    /* The sampler takes a linear image's row pitch from its width (measured 2026-09-14: rows laid
     * out twice as far apart, with the DEPTH field carrying that pitch or left at zero, sampled
     * identically wrong), so rows are stored at exactly the width. Widths that are not a multiple
     * of 64 pixels are unmeasured. */
    size_t pitch_px = (size_t)width;
    size_t rgba_bytes = pitch_px * (size_t)height * 4;

#ifndef OOPS_HOST_BUILD
    if (tex->garlic_data) {
        oops_mem_free(tex->garlic_data);
        tex->garlic_data = NULL;
    }
    tex->garlic_data = oops_mem_alloc(rgba_bytes, 256, OOPS_MEM_WC_GARLIC);
    if (!tex->garlic_data) {
        ctx->last_error = GL_OUT_OF_MEMORY;
        return;
    }
    tex->garlic_va = (uint64_t)(uintptr_t)tex->garlic_data;
    tex->pixels = tex->garlic_data;
    tex->pitch = (uint32_t)pitch_px;
    (void)num_pixels;

    if (pixels) {
        const uint8_t *src = (const uint8_t *)pixels;
        uint8_t *dst = (uint8_t *)tex->garlic_data;
        for (size_t y = 0; y < (size_t)height; y++) {
            uint8_t *row = dst + y * pitch_px * 4;
            if (format == GL_RGBA) {
                memcpy(row, src + y * (size_t)width * 4, (size_t)width * 4);
            } else if (format == GL_RGB) {
                for (size_t x = 0; x < (size_t)width; x++) {
                    const uint8_t *s = src + (y * (size_t)width + x) * 3;
                    row[x * 4 + 0] = s[0];
                    row[x * 4 + 1] = s[1];
                    row[x * 4 + 2] = s[2];
                    row[x * 4 + 3] = 255;
                }
            }
        }
#if defined(__x86_64__)
        for (size_t p = 0; p < rgba_bytes; p += 64) {
            __builtin_ia32_clflush((const void *)((const char *)tex->garlic_data + p));
        }
#endif
    }
#else
    if (tex->pixels) {
        free(tex->pixels);
    }
    tex->pixels = malloc(rgba_bytes);
    if (!tex->pixels) {
        ctx->last_error = GL_OUT_OF_MEMORY;
        return;
    }
    if (pixels) {
        if (format == GL_RGBA) {
            memcpy(tex->pixels, pixels, rgba_bytes);
        } else if (format == GL_RGB) {
            const uint8_t *src = (const uint8_t *)pixels;
            uint8_t *dst = (uint8_t *)tex->pixels;
            for (size_t p = 0; p < num_pixels; p++) {
                dst[p * 4 + 0] = src[p * 3 + 0];
                dst[p * 4 + 1] = src[p * 3 + 1];
                dst[p * 4 + 2] = src[p * 3 + 2];
                dst[p * 4 + 3] = 255;
            }
        }
    }
#endif

    tex->width = width;
    tex->height = height;
    tex->format = format;
    tex->type = type;
    gl_pack_descriptors(tex);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) {
        ctx->last_error = GL_INVALID_ENUM;
        return;
    }

    gl_texture_object_t *tex = gl_find_or_create_texture(ctx, ctx->bound_texture_2d ? ctx->bound_texture_2d : 1);
    if (!tex) return;

    switch (pname) {
        case GL_TEXTURE_WRAP_S:     tex->wrap_s = (GLenum)param; break;
        case GL_TEXTURE_WRAP_T:     tex->wrap_t = (GLenum)param; break;
        case GL_TEXTURE_MIN_FILTER: tex->min_filter = (GLenum)param; break;
        case GL_TEXTURE_MAG_FILTER: tex->mag_filter = (GLenum)param; break;
        default: break;
    }
    gl_pack_descriptors(tex);
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    glTexParameteri(target, pname, (GLint)param);
}

GLboolean glIsTexture(GLuint texture) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || texture == 0) return GL_FALSE;
    return gl_find_texture(ctx, texture) != NULL ? GL_TRUE : GL_FALSE;
}
