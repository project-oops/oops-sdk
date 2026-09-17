/*
 * oops-gl: State management, capability switches, and clear operations
 */

#include "gl_internal.h"

/* **An integer colour component maps across the whole signed range onto [-1, 1]**, so INT_MAX
 * means 1.0 - it is not a cast. Used by the integer spellings of the lighting calls and of
 * glTexEnv, which is why it sits up here rather than beside either of them.
 *
 * The rule and the constant are Mesa's `INT_TO_FLOAT` (`src/mesa/main/macros.h`), applied to
 * exactly the colour-valued pnames in `src/mesa/main/light.c` and
 * `src/mesa/vbo/vbo_attrib_tmp.h`. */
static float gl_int_to_colour(GLint i) {
    return (float)((2.0 * (double)i + 1.0) * (1.0 / 4294967294.0));
}

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
    {
        GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (gl_list_capture(GL_LIST_OP_ENABLE, cap, 0, 0, f)) return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + OOPS_GL_LIGHT_COUNT) {
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
        case GL_POLYGON_OFFSET_FILL: ctx->cap_polygon_offset_fill = GL_TRUE; break;
        case GL_ALPHA_TEST:
            ctx->cap_alpha_test = GL_TRUE;
            gl_ps_patch_alpha_test(ctx);
            break;
        /* **A capability this subset does not have is refused, not ignored.**
         *
         * `glEnable` is the first thing a GL program does, and a silently dropped one is the
         * most expensive kind of nothing: enable GL_STENCIL_TEST or GL_FOG here and the call
         * returned clean while the feature stayed off, so the render was wrong with no error
         * anywhere to say why. The ten above and GL_LIGHT0..7 are the whole of what exists
         * (D008); everything else says so - and glIsEnabled answers from the same list, or the
         * two disagree. */
        default: gl_record_error(ctx, GL_INVALID_ENUM); break;
    }
}

void glDisable(GLenum cap) {
    {
        GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (gl_list_capture(GL_LIST_OP_DISABLE, cap, 0, 0, f)) return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + OOPS_GL_LIGHT_COUNT) {
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
        case GL_POLYGON_OFFSET_FILL: ctx->cap_polygon_offset_fill = GL_FALSE; break;
        case GL_ALPHA_TEST:
            ctx->cap_alpha_test = GL_FALSE;
            gl_ps_patch_alpha_test(ctx);
            break;
        default: gl_record_error(ctx, GL_INVALID_ENUM); break;
    }
}

GLboolean glIsEnabled(GLenum cap) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GL_FALSE;

    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + OOPS_GL_LIGHT_COUNT) {
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
        /* **These two were missing**, so glIsEnabled answered GL_FALSE for a capability
         * glEnable had just switched on - and glGetBooleanv, which forwards here, repeated it.
         * The list has to be the same list glEnable accepts; anything else is a query that
         * disagrees with the state it is querying. */
        case GL_POLYGON_OFFSET_FILL: return ctx->cap_polygon_offset_fill;
        case GL_ALPHA_TEST:     return ctx->cap_alpha_test;
        /* Refused rather than answered GL_FALSE. "Not enabled" and "there is no such thing" are
         * different answers, and a program testing for a feature needs to tell them apart. */
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return GL_FALSE;
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
    /* Same rule as everywhere else here: a target or a parameter this does not keep is
     * refused rather than dropped. Silently ignoring `pname` left a caller believing it had
     * set a texture environment that was never stored. */
    if (target != GL_TEXTURE_ENV) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (pname != GL_TEXTURE_ENV_MODE) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->tex_env_mode = (GLenum)param;
}

void glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
    glTexEnvi(target, pname, (GLint)param);
}

/* **This used to return clean having done nothing** for a target or pname it did not keep,
 * while its own sibling glTexEnvi refused the same arguments. Two spellings of one call
 * disagreeing about what is an error is worse than either answer: a program that set the
 * environment through the vector form believed it had, and the scalar form would have told it
 * otherwise. */
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (target != GL_TEXTURE_ENV) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
        case GL_TEXTURE_ENV_COLOR:
            for (int i = 0; i < 4; i++) ctx->tex_env_color[i] = params[i];
            break;
        case GL_TEXTURE_ENV_MODE:
            ctx->tex_env_mode = (GLenum)params[0];
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

/* The integer vector form. GL_TEXTURE_ENV_COLOR in integers is a colour, so it converts across
 * the signed range the way glLightiv's colours do rather than by a cast. */
void glTexEnviv(GLenum target, GLenum pname, const GLint *params) {
    if (!params) return;
    if (pname == GL_TEXTURE_ENV_COLOR) {
        GLfloat f[4];
        for (int i = 0; i < 4; i++) f[i] = gl_int_to_colour(params[i]);
        glTexEnvfv(target, pname, f);
        return;
    }
    glTexEnvi(target, pname, params[0]);
}

void glGetTexEnviv(GLenum target, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (target != GL_TEXTURE_ENV) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
        case GL_TEXTURE_ENV_MODE:
            params[0] = (GLint)ctx->tex_env_mode;
            break;
        case GL_TEXTURE_ENV_COLOR:
            /* GL converts a float colour to an integer across the signed range, which is the
             * inverse of the conversion going in. */
            for (int i = 0; i < 4; i++) {
                params[i] = (GLint)(ctx->tex_env_color[i] * 2147483647.0f);
            }
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (target != GL_TEXTURE_ENV) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
        case GL_TEXTURE_ENV_MODE:
            params[0] = (GLfloat)ctx->tex_env_mode;
            break;
        case GL_TEXTURE_ENV_COLOR:
            for (int i = 0; i < 4; i++) params[i] = ctx->tex_env_color[i];
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

/* `glHint` - advisory by definition, so ignoring one is allowed. **Naming a hint for a feature
 * that does not exist is not the same thing**, though: GL_FOG_HINT, GL_POINT_SMOOTH_HINT and
 * GL_LINE_SMOOTH_HINT are hints about fog, points and lines, none of which this draws, so
 * accepting them would tell a caller its preference had been noted about something that will
 * never happen. Only the perspective-correction hint names something real here - the rasteriser
 * does interpolate perspective-correctly - and it is recorded and reported without changing
 * anything, which is the whole of what a hint is entitled to do. */
void glHint(GLenum target, GLenum mode) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (target != GL_PERSPECTIVE_CORRECTION_HINT) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->perspective_hint = mode;
}

/* `glDrawBuffer` / `glReadBuffer` - **there is one surface here**, the one the display flips.
 * GL_BACK names it. Accepting GL_FRONT, GL_NONE or an attachment would be claiming a target
 * selection this cannot make, and GL_NONE in particular would have a program believe it had
 * switched drawing off. */
void glDrawBuffer(GLenum buf) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (buf != GL_BACK) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
}

void glReadBuffer(GLenum src) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (src != GL_BACK) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
}

void glDepthFunc(GLenum func) {
    {
        GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (gl_list_capture(GL_LIST_OP_DEPTH_FUNC, func, 0, 0, f)) return;
    }
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
    {
        GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (gl_list_capture(GL_LIST_OP_CULL_FACE, mode, 0, 0, f)) return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->cull_mode = mode;
}

void glFrontFace(GLenum mode) {
    {
        GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (gl_list_capture(GL_LIST_OP_FRONT_FACE, mode, 0, 0, f)) return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->front_face = mode;
}

void glShadeModel(GLenum mode) {
    {
        GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (gl_list_capture(GL_LIST_OP_SHADE_MODEL, mode, 0, 0, f)) return;
    }
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

/* What a program is told it is talking to.
 *
 * GL_VERSION had said "OpenGL 1.3 oops-gl 2.0", which was wrong twice over. It claimed 1.3,
 * and nothing that distinguishes 1.3 from 1.1 is implemented here (D007). It also put a word
 * in front of the number: the specification requires this string to *begin* with the version,
 * so a caller doing the usual atof() on it read 0.0 rather than any version at all. Both
 * halves of that are the badge reporting more than the code supports.
 *
 * "1.1" is the honest class, and the suffix says it is a subset so a caller that reads past
 * the number is not misled either. It is not conformant 1.1 and does not claim to be. */
const GLubyte *glGetString(GLenum name) {
    gl_context_t *ctx = gl_get_ctx();
    switch (name) {
        case GL_VENDOR:      return (const GLubyte *)"OOPS Project";
        case GL_RENDERER:    return (const GLubyte *)"Prospero-generation RDNA2, freestanding";
        case GL_VERSION:     return (const GLubyte *)"1.1 oops-gl fixed-function subset";
        /* **Empty, and that is the honest answer.**
         *
         * This used to say `GL_EXT_vertex_array`. The arrays are here - but an extension string
         * is a promise about *that extension's entry points*, and `glVertexPointerEXT`,
         * `glDrawArraysEXT`, `glArrayElementEXT` and the rest do not exist here. Only the core
         * spellings do. A program that reads the string and calls the EXT names fails to link,
         * which is a loud failure but still one this library invited.
         *
         * The same reasoning kept `GL_ARB_vertex_buffer_object` out when the buffer objects
         * landed: the core `glGenBuffers` exists, `glGenBuffersARB` does not, so the extension
         * is not supported however much of its behaviour is.
         *
         * An empty list is not a refusal and not a placeholder - it is the true statement that
         * no extension's own entry points are provided. */
        case GL_EXTENSIONS:  return (const GLubyte *)"";
        default:
            /* The specification's answer for an unrecognised name: no string, and an error
             * the caller can see. An empty string is indistinguishable from a real answer of
             * "no extensions", which is a wrong answer rather than a refused one. */
            if (ctx) gl_record_error(ctx, GL_INVALID_ENUM);
            return (const GLubyte *)0;
    }
}

/* **How many elements a query writes.** The caller sizes its buffer from the pname, so this is
 * not a convenience: a query that writes four values into a `GLint[1]` corrupts whatever the
 * caller put next to it, and does so only for the pnames nobody tested. Anything not named here
 * writes one value, which is the common case and the safe one.
 */
static int gl_query_element_count(GLenum pname) {
    switch (pname) {
        case GL_MODELVIEW_MATRIX:
        case GL_PROJECTION_MATRIX:
        case GL_TEXTURE_MATRIX:
            return 16;
        case GL_VIEWPORT:
        case GL_SCISSOR_BOX:
        case GL_CURRENT_COLOR:
        case GL_CURRENT_TEXTURE_COORDS:
        case GL_COLOR_CLEAR_VALUE:
        case GL_COLOR_WRITEMASK:
        case GL_LIGHT_MODEL_AMBIENT:
            return 4;
        case GL_CURRENT_NORMAL:
            return 3;
        case GL_DEPTH_RANGE:
        case GL_MAX_VIEWPORT_DIMS:
            return 2;
        default:
            return 1;
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

        /* The implementation limits. A program reads these to size its own work - how many
         * lights to set up, how deep it may nest glPushMatrix - so returning zero, which is
         * what the silent default below used to do, makes a correct program refuse to run. */
        case GL_MAX_LIGHTS:
            params[0] = OOPS_GL_LIGHT_COUNT;
            break;
        case GL_MAX_MODELVIEW_STACK_DEPTH:
            params[0] = OOPS_GL_MODELVIEW_STACK_CAPACITY;
            break;
        case GL_MAX_PROJECTION_STACK_DEPTH:
            params[0] = OOPS_GL_PROJECTION_STACK_CAPACITY;
            break;
        case GL_MAX_TEXTURE_STACK_DEPTH:
            params[0] = OOPS_GL_TEXTURE_STACK_CAPACITY;
            break;
        case GL_MAX_ATTRIB_STACK_DEPTH:
            params[0] = OOPS_GL_ATTRIB_STACK_CAPACITY;
            break;
        case GL_MAX_CLIENT_ATTRIB_STACK_DEPTH:
            params[0] = OOPS_GL_CLIENT_ATTRIB_STACK_CAPACITY;
            break;
        /* The widest rectangle the framebuffer copy path can carry a row of, which is the real
         * bound on a texture here - not a number picked to look generous. */
        case GL_MAX_TEXTURE_SIZE:
            params[0] = 2048;
            break;
        /* **The framebuffer's own extent, which is the honest answer.** There is one surface and
         * it is this size; a viewport larger than it has nothing to rasterise into. */
        case GL_MAX_VIEWPORT_DIMS:
            params[0] = (GLint)ctx->width;
            params[1] = (GLint)ctx->height;
            break;

        /* Where the stacks currently stand. `*_depth` counts the *saved* frames, and GL counts
         * the current matrix as one of them, so the matrix stacks report one more. */
        case GL_ATTRIB_STACK_DEPTH:
            params[0] = (GLint)ctx->attrib_depth;
            break;
        case GL_CLIENT_ATTRIB_STACK_DEPTH:
            params[0] = (GLint)ctx->client_attrib_depth;
            break;
        case GL_MODELVIEW_STACK_DEPTH:
            params[0] = ctx->modelview_depth + 1;
            break;
        case GL_PROJECTION_STACK_DEPTH:
            params[0] = ctx->projection_depth + 1;
            break;

        case GL_DEPTH_FUNC:
            params[0] = (GLint)ctx->depth_func;
            break;
        case GL_DEPTH_WRITEMASK:
            params[0] = ctx->depth_mask ? 1 : 0;
            break;
        case GL_CULL_FACE_MODE:
            params[0] = (GLint)ctx->cull_mode;
            break;
        case GL_FRONT_FACE:
            params[0] = (GLint)ctx->front_face;
            break;
        case GL_SHADE_MODEL:
            params[0] = (GLint)ctx->shade_model;
            break;
        case GL_ALPHA_TEST_FUNC:
            params[0] = (GLint)ctx->alpha_func;
            break;
        case GL_LIST_BASE:
            params[0] = (GLint)ctx->list_base;
            break;
        case GL_PERSPECTIVE_CORRECTION_HINT:
            params[0] = (GLint)ctx->perspective_hint;
            break;
        /* Both name the one surface, always. */
        case GL_DRAW_BUFFER:
        case GL_READ_BUFFER:
            params[0] = (GLint)GL_BACK;
            break;
        case GL_ARRAY_BUFFER_BINDING:
            params[0] = (GLint)ctx->bound_array_buffer;
            break;
        case GL_ELEMENT_ARRAY_BUFFER_BINDING:
            params[0] = (GLint)ctx->bound_element_array_buffer;
            break;

        default:
            /* **Refused, not ignored.** An ignored query leaves the caller's buffer holding
             * whatever it held before and raises nothing, so the program reads stack garbage
             * that looks like an answer. */
            gl_record_error(ctx, GL_INVALID_ENUM);
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
        case GL_TEXTURE_MATRIX:
            for (int i = 0; i < 16; i++) {
                params[i] = ctx->texture_stack[ctx->texture_depth].m[i];
            }
            break;

        case GL_CURRENT_COLOR:
            for (int i = 0; i < 4; i++) params[i] = ctx->cur_color[i];
            break;
        case GL_CURRENT_NORMAL:
            for (int i = 0; i < 3; i++) params[i] = ctx->cur_normal[i];
            break;
        case GL_CURRENT_TEXTURE_COORDS:
            /* GL reports four components. The two this tracks, then the specification's
             * defaults for r and q - not left as the caller found them. */
            params[0] = ctx->cur_texcoord[0];
            params[1] = ctx->cur_texcoord[1];
            params[2] = 0.0f;
            params[3] = 1.0f;
            break;
        case GL_COLOR_CLEAR_VALUE:
            for (int i = 0; i < 4; i++) params[i] = ctx->clear_color[i];
            break;
        case GL_LIGHT_MODEL_AMBIENT:
            for (int i = 0; i < 4; i++) params[i] = ctx->light_model_ambient[i];
            break;
        case GL_DEPTH_CLEAR_VALUE:
            params[0] = (GLfloat)ctx->clear_depth;
            break;
        case GL_DEPTH_RANGE:
            params[0] = (GLfloat)ctx->depth_near;
            params[1] = (GLfloat)ctx->depth_far;
            break;
        case GL_POLYGON_OFFSET_FACTOR:
            params[0] = ctx->polygon_offset_factor;
            break;
        case GL_POLYGON_OFFSET_UNITS:
            params[0] = ctx->polygon_offset_units;
            break;
        case GL_ALPHA_TEST_REF:
            params[0] = ctx->alpha_ref;
            break;
        default:
            /* Anything glGetIntegerv can answer, this can answer too - GL says every query is
             * available in every type, converted. Only a pname neither knows is refused.
             *
             * **Exactly `gl_query_element_count` of them.** The caller sized the buffer from the
             * pname, so a GLfloat[1] for GL_DEPTH_FUNC is correct and writing four would run
             * off the end of it. */
            {
                GLint iv[4] = {0, 0, 0, 0};
                GLenum before = ctx->last_error;
                ctx->last_error = GL_NO_ERROR;
                glGetIntegerv(pname, iv);
                if (ctx->last_error == GL_NO_ERROR) {
                    ctx->last_error = before;
                    int n = gl_query_element_count(pname);
                    if (n > 4) n = 4; /* nothing integer-valued is wider */
                    for (int i = 0; i < n; i++) params[i] = (GLfloat)iv[i];
                } else if (before != GL_NO_ERROR) {
                    ctx->last_error = before; /* the older error is the one GL retains */
                }
            }
            break;
    }
}

/* Every query, converted to double. GL requires each pname to be available in every type, and
 * the conversion is the whole of the difference - so this goes through glGetFloatv rather than
 * repeating the table, which is what keeps the two from disagreeing about a pname. */
void glGetDoublev(GLenum pname, GLdouble *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    GLfloat fv[16] = {0};
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    glGetFloatv(pname, fv);
    if (ctx->last_error != GL_NO_ERROR) {
        /* Refused: leave the caller's buffer alone and keep whichever error GL retains. */
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    const int n = gl_query_element_count(pname);
    for (int i = 0; i < n; i++) params[i] = (GLdouble)fv[i];
}

/* **Not every boolean query is an enable.** This used to forward everything to glIsEnabled,
 * which answers GL_FALSE for GL_COLOR_WRITEMASK - a mask of all four channels reported as all
 * four channels off, with no error to say the question was misunderstood. The two masks are
 * answered here; everything else is either an enable or reaches the general query below. */
void glGetBooleanv(GLenum pname, GLboolean *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    switch (pname) {
        case GL_COLOR_WRITEMASK:
            for (int i = 0; i < 4; i++) params[i] = ctx->color_mask[i];
            return;
        case GL_DEPTH_WRITEMASK:
            params[0] = ctx->depth_mask;
            return;
        default:
            break;
    }

    /* An enable answers directly. glIsEnabled refuses a cap it does not have, so asking it
     * first and only then falling through keeps that refusal rather than masking it. */
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    GLboolean enabled = glIsEnabled(pname);
    if (ctx->last_error == GL_NO_ERROR) {
        ctx->last_error = before;
        params[0] = enabled;
        return;
    }
    ctx->last_error = before;

    /* Not an enable: anything with a value is false when that value is zero, which is what GL
     * says the conversion is. */
    GLfloat fv[16] = {0};
    ctx->last_error = GL_NO_ERROR;
    glGetFloatv(pname, fv);
    if (ctx->last_error != GL_NO_ERROR) {
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    const int n = gl_query_element_count(pname);
    for (int i = 0; i < n; i++) params[i] = (GLboolean)(fv[i] != 0.0f);
}

/* -------------------------------------------------------------------------
 * Fixed-Function Lighting & Materials
 * ------------------------------------------------------------------------- */

void glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + OOPS_GL_LIGHT_COUNT) {
        gl_record_error(ctx, GL_INVALID_ENUM);
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
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glLightf(GLenum light, GLenum pname, GLfloat param) {
    glLightfv(light, pname, &param);
}

/* Returns whether `pname` is one this keeps; the caller records the refusal. */
static GLboolean apply_material_param(gl_material_t *m, GLenum pname, const GLfloat *params) {
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
        /* A material property this does not keep is refused rather than dropped: the six above
         * are the whole of what the lighting model reads, and a caller setting a seventh was
         * told nothing and shaded as though it had not. Reported rather than recorded here,
         * because this helper has no context - the caller owns the error flag. */
        default: return GL_FALSE;
    }
    return GL_TRUE;
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    GLboolean kept = GL_TRUE;
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK) {
        kept = apply_material_param(&ctx->mat_front, pname, params);
    }
    if (face == GL_BACK || face == GL_FRONT_AND_BACK) {
        kept = apply_material_param(&ctx->mat_back, pname, params);
    }
    if (!kept) gl_record_error(ctx, GL_INVALID_ENUM);
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
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glLightModelf(GLenum pname, GLfloat param) {
    glLightModelfv(pname, &param);
}

/* -------------------------------------------------------------------------
 * The integer spellings of the lighting calls
 *
 * **A colour is not converted the same way a scalar is.** An integer colour component is mapped
 * across the whole signed range onto [-1, 1] - so GL_AMBIENT with INT_MAX means 1.0, not
 * 2147483647.0 - while a position, a direction, an attenuation or a shininess is an ordinary
 * cast. Casting a colour would make every integer colour astronomically out of range and light
 * the scene pure white, which looks like a lighting bug anywhere but here.
 *
 * The rule and the constant are Mesa's: `INT_TO_FLOAT` in `src/mesa/main/macros.h`, applied to
 * exactly these pnames in `src/mesa/main/light.c` (`_mesa_Lightiv`) and
 * `src/mesa/vbo/vbo_attrib_tmp.h` (`Materialiv`).
 * ------------------------------------------------------------------------- */

void glLightiv(GLenum light, GLenum pname, const GLint *params) {
    if (!params) return;
    GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    switch (pname) {
        case GL_AMBIENT:
        case GL_DIFFUSE:
        case GL_SPECULAR:
            for (int i = 0; i < 4; i++) f[i] = gl_int_to_colour(params[i]);
            break;
        case GL_POSITION:
            for (int i = 0; i < 4; i++) f[i] = (GLfloat)params[i];
            break;
        case GL_SPOT_DIRECTION:
            for (int i = 0; i < 3; i++) f[i] = (GLfloat)params[i];
            break;
        default:
            /* One value, or a pname glLightfv will refuse for us. */
            f[0] = (GLfloat)params[0];
            break;
    }
    glLightfv(light, pname, f);
}

void glLighti(GLenum light, GLenum pname, GLint param) {
    glLightiv(light, pname, &param);
}

void glMaterialiv(GLenum face, GLenum pname, const GLint *params) {
    if (!params) return;
    GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    switch (pname) {
        case GL_AMBIENT:
        case GL_DIFFUSE:
        case GL_SPECULAR:
        case GL_EMISSION:
        case GL_AMBIENT_AND_DIFFUSE:
            for (int i = 0; i < 4; i++) f[i] = gl_int_to_colour(params[i]);
            break;
        default:
            f[0] = (GLfloat)params[0];
            break;
    }
    glMaterialfv(face, pname, f);
}

void glMateriali(GLenum face, GLenum pname, GLint param) {
    glMaterialiv(face, pname, &param);
}

void glLightModeliv(GLenum pname, const GLint *params) {
    if (!params) return;
    GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (pname == GL_LIGHT_MODEL_AMBIENT) {
        for (int i = 0; i < 4; i++) f[i] = gl_int_to_colour(params[i]);
    } else {
        f[0] = (GLfloat)params[0];
    }
    glLightModelfv(pname, f);
}

void glLightModeli(GLenum pname, GLint param) {
    glLightModeliv(pname, &param);
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
    for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
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

    for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
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
    {
        GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (gl_list_capture(GL_LIST_OP_BIND_TEXTURE, target, 0, texture, f)) return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
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

/* How many bytes one source pixel occupies, or zero for a combination not handled.
 *
 * **Zero is the refusal**, and it is checked before anything is allocated or copied. The upload
 * path used to accept any `format`, convert only `GL_RGBA` and `GL_RGB`, and leave the texture
 * holding whatever the allocation happened to contain for everything else - so a caller passing
 * `GL_LUMINANCE` got a texture of uninitialised memory and `glGetError` said nothing. */
static size_t gl_unpack_pixel_bytes(GLenum format, GLenum type) {
    /* Only unsigned bytes are converted below; the wider and packed types are a different
     * unpacking and are refused rather than read as bytes. */
    if (type != GL_UNSIGNED_BYTE) return 0u;
    switch (format) {
        case GL_RGBA:
        case GL_BGRA:
            return 4u;
        case GL_RGB:
        case GL_BGR:
            return 3u;
        case GL_LUMINANCE:
        case GL_ALPHA:
            return 1u;
        case GL_LUMINANCE_ALPHA:
            return 2u;
        default:
            return 0u;
    }
}

/* The stride between source rows, honouring glPixelStorei.
 *
 * `GL_UNPACK_ROW_LENGTH` gives the row width in pixels when it is not the width being uploaded -
 * which is what makes a sub-rectangle of a larger client image uploadable at all - and
 * `GL_UNPACK_ALIGNMENT` rounds each row's start up to 1, 2, 4 or 8 bytes. Both default to the
 * values the specification gives (0 and 4), so a caller that never calls glPixelStorei gets
 * exactly the behaviour this had before they existed. */
static size_t gl_unpack_row_stride(const gl_context_t *ctx, GLsizei width, size_t pixel_bytes) {
    size_t row_pixels = ctx->unpack_row_length > 0 ? (size_t)ctx->unpack_row_length : (size_t)width;
    size_t bytes = row_pixels * pixel_bytes;
    size_t align = ctx->unpack_alignment > 0 ? (size_t)ctx->unpack_alignment : 1u;
    size_t remainder = bytes % align;
    return remainder == 0u ? bytes : bytes + (align - remainder);
}

/* Converts one source row into the RGBA the sampler reads.
 *
 * Every supported format widens to RGBA because that is the one descriptor layout measured on
 * this hardware (`gl_pack_descriptors`); a narrower internal format would be a second measured
 * thing and there is only one. */
static void gl_unpack_row(uint8_t *dst, const uint8_t *src, GLsizei width, GLenum format) {
    for (size_t x = 0; x < (size_t)width; x++) {
        uint8_t *d = dst + x * 4u;
        switch (format) {
            case GL_RGBA:
                d[0] = src[x * 4 + 0]; d[1] = src[x * 4 + 1];
                d[2] = src[x * 4 + 2]; d[3] = src[x * 4 + 3];
                break;
            case GL_BGRA:
                d[0] = src[x * 4 + 2]; d[1] = src[x * 4 + 1];
                d[2] = src[x * 4 + 0]; d[3] = src[x * 4 + 3];
                break;
            case GL_RGB:
                d[0] = src[x * 3 + 0]; d[1] = src[x * 3 + 1];
                d[2] = src[x * 3 + 2]; d[3] = 255u;
                break;
            case GL_BGR:
                d[0] = src[x * 3 + 2]; d[1] = src[x * 3 + 1];
                d[2] = src[x * 3 + 0]; d[3] = 255u;
                break;
            case GL_LUMINANCE:
                d[0] = src[x]; d[1] = src[x]; d[2] = src[x]; d[3] = 255u;
                break;
            case GL_ALPHA:
                d[0] = 0u; d[1] = 0u; d[2] = 0u; d[3] = src[x];
                break;
            case GL_LUMINANCE_ALPHA:
                d[0] = src[x * 2]; d[1] = src[x * 2]; d[2] = src[x * 2];
                d[3] = src[x * 2 + 1];
                break;
            default:
                break; /* unreachable: gl_unpack_pixel_bytes refused it before this ran */
        }
    }
}

/* `glAlphaFunc(func, ref)` - discards a fragment whose alpha fails the comparison.
 *
 * RDNA2 has no fixed-function alpha test; it was removed after the fixed-function era, and a
 * modern driver implements it by discarding in the pixel shader. So this records the state and
 * `gl_ps_patch_alpha_test` writes the comparison into both shaders - which is why a function
 * this does not know is refused here rather than silently doing nothing: the shader would
 * carry no test and every fragment would pass, which is a picture rather than an error.
 */
void glAlphaFunc(GLenum func, GLclampf ref) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    switch (func) {
        case GL_NEVER:
        case GL_LESS:
        case GL_EQUAL:
        case GL_LEQUAL:
        case GL_GREATER:
        case GL_NOTEQUAL:
        case GL_GEQUAL:
        case GL_ALWAYS:
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return;
    }
    float clamped = (float)ref;
    if (clamped < 0.0f) clamped = 0.0f;
    if (clamped > 1.0f) clamped = 1.0f;
    ctx->alpha_func = func;
    ctx->alpha_ref = clamped;
    gl_ps_patch_alpha_test(ctx);
}

/* `glPolygonOffset(factor, units)` - nudges a filled polygon's depth so coplanar geometry can
 * be drawn over it without fighting.
 *
 * Both are kept as given and scaled where they are written, because the scaling depends on the
 * depth format rather than on the call: Mesa multiplies the factor by 16 for every format, and
 * the units by 4, 2 or 1 for 16-, 24- and 32-bit z-buffers. This one is 32-bit float, so the
 * units go in unscaled - see the DB_FMT_CNTL comment in `gl_draw.c` for how that was settled.
 *
 * Takes no error: the specification defines no error for this call, and any float pair is
 * meaningful - including negative, which pulls geometry towards the viewer. */
void glPolygonOffset(GLfloat factor, GLfloat units) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->polygon_offset_factor = factor;
    ctx->polygon_offset_units = units;
}

/* The float spelling of the same call. Every pixel-store parameter this has is an integer, so
 * this narrows and forwards rather than carrying a second representation. */
void glPixelStoref(GLenum pname, GLfloat param) {
    glPixelStorei(pname, (GLint)param);
}

void glPixelStorei(GLenum pname, GLint param) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    switch (pname) {
        case GL_UNPACK_ALIGNMENT:
            /* The specification's four values and no others; anything else would silently
             * misplace every row after the first. */
            if (param != 1 && param != 2 && param != 4 && param != 8) {
                gl_record_error(ctx, GL_INVALID_VALUE);
                return;
            }
            ctx->unpack_alignment = param;
            break;
        case GL_UNPACK_ROW_LENGTH:
            if (param < 0) {
                gl_record_error(ctx, GL_INVALID_VALUE);
                return;
            }
            ctx->unpack_row_length = param;
            break;
        case GL_PACK_ALIGNMENT:
            /* The other direction: how glReadPixels lays rows out in the caller's buffer. */
            if (param != 1 && param != 2 && param != 4 && param != 8) {
                gl_record_error(ctx, GL_INVALID_VALUE);
                return;
            }
            ctx->pack_alignment = param;
            break;
        default:
            /* The pack side and the other unpack parameters are not implemented, and say so
             * rather than being accepted and ignored. */
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

/* `glReadPixels(x, y, width, height, format, type, pixels)` - the framebuffer, read back.
 *
 * # The y flip, which is the whole of what makes this easy to get wrong
 *
 * **GL's window origin is the bottom-left corner; this framebuffer's row 0 is the top.** So row
 * `y` of the requested rectangle is row `height_fb - 1 - y` of memory, and a reader that forgets
 * it gets a vertically mirrored image - which looks like a plausible screenshot, so nothing
 * downstream notices. The same flip is why `glTexImage2D` does *not* flip: a texture's rows are
 * the caller's to order, a window's are not.
 *
 * # Where the pixels come from
 *
 * On target, the command processor's own copy of the render target if a frame has been
 * submitted and confirmed - it is CPU-cached, where the render target is write-combined and
 * slow to read. Otherwise the framebuffer itself, which is what a host build always uses.
 *
 * # Out of bounds is zero, not whatever was next in memory
 *
 * The specification leaves pixels outside the framebuffer undefined, and undefined is allowed
 * to be zero. Writing whatever happened to follow the framebuffer would be permitted too and is
 * the kind of plausible garbage this subsystem refuses to hand back, so the destination is
 * cleared first and only the part that overlaps is filled.
 */
/* One RGBA8 pixel, written out in the client's format. Shared by glReadPixels and
 * glGetTexImage: the conversion is the same one, and two copies of it are two chances for the
 * channel order to drift apart in exactly one of them.
 *
 * `format` has already been accepted by gl_unpack_pixel_bytes, so the default arm is
 * unreachable rather than lenient. */
static void gl_pack_pixel(uint8_t *p, GLenum format,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    switch (format) {
        case GL_RGBA: p[0] = r; p[1] = g; p[2] = b; p[3] = a; break;
        case GL_BGRA: p[0] = b; p[1] = g; p[2] = r; p[3] = a; break;
        case GL_RGB:  p[0] = r; p[1] = g; p[2] = b; break;
        case GL_BGR:  p[0] = b; p[1] = g; p[2] = r; break;
        case GL_LUMINANCE: p[0] = r; break;
        case GL_ALPHA: p[0] = a; break;
        case GL_LUMINANCE_ALPHA: p[0] = r; p[1] = a; break;
        default: break; /* unreachable: refused by the caller */
    }
}

/* The row stride glReadPixels and glGetTexImage write at: the row's own bytes, rounded up to
 * GL_PACK_ALIGNMENT. */
static size_t gl_pack_row_stride(const gl_context_t *ctx, GLsizei width, size_t pixel_bytes) {
    size_t align = ctx->pack_alignment > 0 ? (size_t)ctx->pack_alignment : 1u;
    size_t row_bytes = (size_t)width * pixel_bytes;
    size_t remainder = row_bytes % align;
    return remainder == 0u ? row_bytes : row_bytes + (align - remainder);
}

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *pixels) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (width < 0 || height < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    size_t pixel_bytes = gl_unpack_pixel_bytes(format, type);
    if (pixel_bytes == 0u) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width == 0 || height == 0 || !pixels) return;

    const uint32_t *source = ctx->readback && ctx->hw_frames_confirmed > 0u
                                 ? (const uint32_t *)ctx->readback
                                 : (const uint32_t *)ctx->framebuffer;
    if (!source) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }

    size_t stride = gl_pack_row_stride(ctx, width, pixel_bytes);

    uint8_t *dst = (uint8_t *)pixels;
    memset(dst, 0, stride * (size_t)height);

    for (GLsizei row = 0; row < height; row++) {
        GLint window_y = y + row;
        if (window_y < 0 || window_y >= (GLint)ctx->height) continue;
        /* The flip. */
        size_t source_row = (size_t)((GLint)ctx->height - 1 - window_y);
        const uint32_t *src = source + source_row * (size_t)ctx->width;
        uint8_t *out = dst + (size_t)row * stride;
        for (GLsizei col = 0; col < width; col++) {
            GLint window_x = x + col;
            if (window_x < 0 || window_x >= (GLint)ctx->width) continue;
            uint32_t argb = src[window_x];
            uint8_t r = (uint8_t)((argb >> 16) & 0xffu);
            uint8_t g = (uint8_t)((argb >> 8) & 0xffu);
            uint8_t b = (uint8_t)(argb & 0xffu);
            uint8_t a = (uint8_t)((argb >> 24) & 0xffu);
            gl_pack_pixel(out + (size_t)col * pixel_bytes, format, r, g, b, a);
        }
    }
}

/* -------------------------------------------------------------------------
 * Buffer objects (GL 1.5)
 *
 * A named block of memory the vertex arrays and the index array are read out of. The storage
 * here is ordinary process memory rather than a GPU allocation: everything that reads it is the
 * CPU-side array reader, and the hardware path copies the vertices it assembles into its own
 * allocation afterwards regardless of where they came from. A GPU-resident buffer would save
 * that copy and is a measurable optimisation rather than a correctness question.
 * ------------------------------------------------------------------------- */

/* The allocator differs by build: a freestanding target has the SDK's own heap, and the host
 * test build has the C library's. Wrapped here so the five call sites below do not each carry
 * the #if. */
static void *gl_buffer_alloc(size_t bytes) {
#ifdef OOPS_HOST_BUILD
    return malloc(bytes);
#else
    return oops_malloc(bytes);
#endif
}

static void gl_buffer_release(void *p) {
    if (!p) return;
#ifdef OOPS_HOST_BUILD
    free(p);
#else
    oops_free(p);
#endif
}

gl_buffer_object_t *gl_find_buffer(gl_context_t *ctx, GLuint name) {
    if (!ctx || name == 0u) return NULL;
    for (int i = 0; i < OOPS_GL_MAX_BUFFER_OBJECTS; i++) {
        if (ctx->buffers[i].used && ctx->buffers[i].id == name) {
            return &ctx->buffers[i];
        }
    }
    return NULL;
}

void gl_free_all_buffers(gl_context_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < OOPS_GL_MAX_BUFFER_OBJECTS; i++) {
        gl_buffer_release(ctx->buffers[i].data);
        ctx->buffers[i].data = NULL;
        ctx->buffers[i].size = 0;
        ctx->buffers[i].used = GL_FALSE;
    }
    ctx->bound_array_buffer = 0u;
    ctx->bound_element_array_buffer = 0u;
}

/* Which binding a target names. Returns NULL for a target this does not have, so the callers
 * can refuse uniformly rather than each deciding what an unknown target means. */
static GLuint *gl_buffer_binding(gl_context_t *ctx, GLenum target) {
    switch (target) {
        case GL_ARRAY_BUFFER:         return &ctx->bound_array_buffer;
        case GL_ELEMENT_ARRAY_BUFFER: return &ctx->bound_element_array_buffer;
        default:                      return NULL;
    }
}

const uint8_t *gl_array_base(const gl_context_t *ctx, const gl_client_array_t *a) {
    if (!ctx || !a) return NULL;
    if (a->buffer == 0u) return (const uint8_t *)a->pointer;

    /* **The name is resolved now, not when the pointer was set.** glBufferData may have
     * replaced the storage since - respecifying a buffer every frame is ordinary - so an
     * address captured at glVertexPointer time would be stale.
     *
     * Walked here rather than through gl_find_buffer so this can take a const context: the
     * draw path's vertex fetch has one, and it has no business being handed a mutable one. */
    for (int i = 0; i < OOPS_GL_MAX_BUFFER_OBJECTS; i++) {
        const gl_buffer_object_t *buf = &ctx->buffers[i];
        if (!buf->used || buf->id != a->buffer) continue;
        if (!buf->data) return NULL;
        return (const uint8_t *)buf->data + (uintptr_t)a->pointer;
    }
    return NULL; /* the buffer was deleted out from under the array */
}

void glGenBuffers(GLsizei n, GLuint *buffers) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !buffers) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    GLsizei made = 0;
    for (int i = 0; i < OOPS_GL_MAX_BUFFER_OBJECTS && made < n; i++) {
        if (ctx->buffers[i].used) continue;
        ctx->buffers[i].used = GL_TRUE;
        ctx->buffers[i].id = (GLuint)(i + 1);
        ctx->buffers[i].data = NULL;
        ctx->buffers[i].size = 0;
        ctx->buffers[i].usage = GL_STATIC_DRAW;
        buffers[made++] = ctx->buffers[i].id;
    }
    /* Ran out. The names already handed back are real; the rest are zeroed rather than left as
     * the caller found them, so a program that ignores the error still binds nothing instead of
     * binding whatever was in its array. */
    if (made < n) {
        for (GLsizei i = made; i < n; i++) buffers[i] = 0u;
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
    }
}

void glDeleteBuffers(GLsizei n, const GLuint *buffers) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !buffers) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    for (GLsizei i = 0; i < n; i++) {
        gl_buffer_object_t *buf = gl_find_buffer(ctx, buffers[i]);
        if (!buf) continue; /* name 0 and unknown names are silently ignored, as GL says */
        gl_buffer_release(buf->data);
        buf->data = NULL;
        buf->size = 0;
        buf->used = GL_FALSE;
        buf->id = 0u;

        /* **A deleted buffer that was bound reverts the binding to 0.** Leaving it bound would
         * have the next glBufferData look up a name nothing owns; GL specifies the unbind. */
        if (ctx->bound_array_buffer == buffers[i]) ctx->bound_array_buffer = 0u;
        if (ctx->bound_element_array_buffer == buffers[i]) ctx->bound_element_array_buffer = 0u;
    }
}

void glBindBuffer(GLenum target, GLuint buffer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* Binding 0 means "back to client memory" and is always legal. Any other name has to be one
     * glGenBuffers handed out: GL lets an implementation accept an unknown name and create the
     * object, but doing so quietly turns a typo into a working-looking buffer full of nothing. */
    if (buffer != 0u && !gl_find_buffer(ctx, buffer)) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    *binding = buffer;
}

void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    switch (usage) {
        case GL_STREAM_DRAW:
        case GL_STATIC_DRAW:
        case GL_DYNAMIC_DRAW:
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return;
    }
    if (size < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    gl_buffer_object_t *buf = gl_find_buffer(ctx, *binding);
    if (!buf) {
        /* Nothing bound. GL reserves name 0 as "no buffer", so there is no object to fill. */
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }

    /* **Replaced, not resized.** glBufferData respecifies the whole store, so the old one goes
     * even when the size is unchanged - and the arrays pointing at this buffer find the new
     * storage because they kept the name rather than an address. */
    gl_buffer_release(buf->data);
    buf->data = NULL;
    buf->size = 0;
    buf->usage = usage;
    if (size == 0) return;

    buf->data = gl_buffer_alloc((size_t)size);
    if (!buf->data) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    buf->size = size;
    if (data) {
        memcpy(buf->data, data, (size_t)size);
    } else {
        /* A null pointer reserves the store, which is how a program sets a buffer up to fill
         * with glBufferSubData. Zeroed rather than left as the allocator found it, so a draw
         * before the first update reads zeroes instead of whatever was in that memory. */
        memset(buf->data, 0, (size_t)size);
    }
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_buffer_object_t *buf = gl_find_buffer(ctx, *binding);
    if (!buf || !buf->data) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* **Bounds are an error, not a clamp**, and the addition is done in a width that cannot
     * wrap past the check: offset and size are both signed and both already known to be
     * non-negative, so `offset + size` compared against the store is safe here. */
    if (offset < 0 || size < 0 || offset + size > buf->size) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (size == 0 || !data) return;
    memcpy((uint8_t *)buf->data + offset, data, (size_t)size);
}

GLboolean glIsBuffer(GLuint buffer) {
    gl_context_t *ctx = gl_get_ctx();
    return (GLboolean)(gl_find_buffer(ctx, buffer) != NULL);
}

void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const gl_buffer_object_t *buf = gl_find_buffer(ctx, *binding);
    if (!buf) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    switch (pname) {
        case GL_BUFFER_SIZE:  params[0] = (GLint)buf->size; break;
        case GL_BUFFER_USAGE: params[0] = (GLint)buf->usage; break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

/* -------------------------------------------------------------------------
 * The per-object queries
 *
 * State belonging to a texture, a light or a material rather than to the context. Each answers
 * from the same field the matching setter writes - not from a parallel copy, which is how a
 * query comes to disagree with the state it is querying (see glIsEnabled, which did).
 * ------------------------------------------------------------------------- */

void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (target != GL_TEXTURE_2D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const gl_texture_object_t *tex =
        gl_find_texture(ctx, ctx->bound_texture_2d ? ctx->bound_texture_2d : 1);
    /* No texture object yet is not an error - GL answers with the defaults a fresh object
     * would have, which is what a program setting up one parameter at a time reads back. */
    switch (pname) {
        case GL_TEXTURE_WRAP_S:
            params[0] = (GLint)(tex ? tex->wrap_s : (GLenum)GL_REPEAT);
            break;
        case GL_TEXTURE_WRAP_T:
            params[0] = (GLint)(tex ? tex->wrap_t : (GLenum)GL_REPEAT);
            break;
        /* The defaults have to be the ones gl_find_or_create_texture actually assigns, or the
         * query answers differently before and after the first glTexImage2D. min_filter is
         * GL_NEAREST_MIPMAP_LINEAR, which is the specification's default and not GL_LINEAR. */
        case GL_TEXTURE_MIN_FILTER:
            params[0] = (GLint)(tex ? tex->min_filter : (GLenum)GL_NEAREST_MIPMAP_LINEAR);
            break;
        case GL_TEXTURE_MAG_FILTER:
            params[0] = (GLint)(tex ? tex->mag_filter : (GLenum)GL_LINEAR);
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    GLint iv = 0;
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    glGetTexParameteriv(target, pname, &iv);
    if (ctx->last_error != GL_NO_ERROR) {
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    params[0] = (GLfloat)iv;
}

/* The per-level queries: what the image actually is, as opposed to how it is sampled. A program
 * asking these has usually just uploaded and wants to know what it got. */
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params) {
    (void)level;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (target != GL_TEXTURE_2D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const gl_texture_object_t *tex =
        gl_find_texture(ctx, ctx->bound_texture_2d ? ctx->bound_texture_2d : 1);
    switch (pname) {
        case GL_TEXTURE_WIDTH:
            params[0] = tex ? tex->width : 0;
            break;
        case GL_TEXTURE_HEIGHT:
            params[0] = tex ? tex->height : 0;
            break;
        case GL_TEXTURE_INTERNAL_FORMAT:
            /* **What is stored, not what was asked for.** Every upload is converted to RGBA8
             * regardless of the internalformat the caller named, so reporting their request
             * back would be a lie a program could act on - it would believe it had a
             * single-channel texture and size its readback for one byte a pixel. */
            params[0] = (GLint)GL_RGBA;
            break;
        case GL_TEXTURE_BORDER:
            params[0] = 0; /* bordered textures are refused at upload */
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    GLint iv = 0;
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    glGetTexLevelParameteriv(target, level, pname, &iv);
    if (ctx->last_error != GL_NO_ERROR) {
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    params[0] = (GLfloat)iv;
}

/* How many floats a light or material query writes. Same reasoning as gl_query_element_count:
 * the caller sized its buffer from the pname, so four into a GLfloat[1] is an overrun. */
static int gl_light_element_count(GLenum pname) {
    switch (pname) {
        case GL_AMBIENT:
        case GL_DIFFUSE:
        case GL_SPECULAR:
        case GL_POSITION:
            return 4;
        case GL_SPOT_DIRECTION:
            return 3;
        default:
            return 1;
    }
}

void glGetLightfv(GLenum light, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + OOPS_GL_LIGHT_COUNT) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const gl_light_t *l = &ctx->lights[(size_t)(light - GL_LIGHT0)];
    switch (pname) {
        /* **Eye coordinates, which is what GL says and what is stored.** glLightfv transformed
         * the position by the modelview matrix on the way in, so returning the object-space
         * value the caller passed would need a copy of it kept and an inverse applied - and
         * would answer a different question from the one the specification asks. */
        case GL_POSITION:
            for (int i = 0; i < 4; i++) params[i] = l->position[i];
            break;
        case GL_AMBIENT:
            for (int i = 0; i < 4; i++) params[i] = l->ambient[i];
            break;
        case GL_DIFFUSE:
            for (int i = 0; i < 4; i++) params[i] = l->diffuse[i];
            break;
        case GL_SPECULAR:
            for (int i = 0; i < 4; i++) params[i] = l->specular[i];
            break;
        case GL_SPOT_DIRECTION:
            for (int i = 0; i < 3; i++) params[i] = l->spot_direction[i];
            break;
        case GL_SPOT_EXPONENT:         params[0] = l->spot_exponent; break;
        case GL_SPOT_CUTOFF:           params[0] = l->spot_cutoff; break;
        case GL_CONSTANT_ATTENUATION:  params[0] = l->const_att; break;
        case GL_LINEAR_ATTENUATION:    params[0] = l->linear_att; break;
        case GL_QUADRATIC_ATTENUATION: params[0] = l->quad_att; break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glGetLightiv(GLenum light, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    GLfloat fv[4] = {0};
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    glGetLightfv(light, pname, fv);
    if (ctx->last_error != GL_NO_ERROR) {
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    const int n = gl_light_element_count(pname);
    for (int i = 0; i < n; i++) params[i] = (GLint)fv[i];
}

void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    /* GL_FRONT_AND_BACK is refused here although glMaterialfv accepts it: setting both at once
     * is meaningful, reading "both" is not - there is no single answer to return. */
    const gl_material_t *m;
    if (face == GL_FRONT) {
        m = &ctx->mat_front;
    } else if (face == GL_BACK) {
        m = &ctx->mat_back;
    } else {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
        case GL_AMBIENT:
            for (int i = 0; i < 4; i++) params[i] = m->ambient[i];
            break;
        case GL_DIFFUSE:
            for (int i = 0; i < 4; i++) params[i] = m->diffuse[i];
            break;
        case GL_SPECULAR:
            for (int i = 0; i < 4; i++) params[i] = m->specular[i];
            break;
        case GL_EMISSION:
            for (int i = 0; i < 4; i++) params[i] = m->emission[i];
            break;
        case GL_SHININESS:
            params[0] = m->shininess;
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glGetMaterialiv(GLenum face, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    GLfloat fv[4] = {0};
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    glGetMaterialfv(face, pname, fv);
    if (ctx->last_error != GL_NO_ERROR) {
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    const int n = (pname == GL_SHININESS) ? 1 : 4;
    for (int i = 0; i < n; i++) params[i] = (GLint)fv[i];
}

/* The array pointers, read back. `glInterleavedArrays` is the reason this matters: a program
 * that let it compute the offsets may want to know what they came out as. */
void glGetPointerv(GLenum pname, GLvoid **params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    switch (pname) {
        case GL_VERTEX_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_vertex.pointer;
            break;
        case GL_COLOR_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_color.pointer;
            break;
        case GL_NORMAL_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_normal.pointer;
            break;
        case GL_TEXTURE_COORD_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_texcoord.pointer;
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

/* `glGetTexImage(...)` - the bound texture, read back into client memory.
 *
 * **Not a flip.** glReadPixels turns the framebuffer over because GL's window origin is the
 * bottom-left corner; a texture has no window and row 0 of a texture image is row 0 of what was
 * uploaded, so this walks straight through. Sharing glReadPixels' loop here would have been the
 * tidy-looking mistake.
 *
 * The stored image is RGBA8 at `tex->pitch`, so this converts out through the same
 * `gl_pack_pixel` glReadPixels uses and honours GL_PACK_ALIGNMENT the same way.
 */
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels) {
    (void)level;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    size_t pixel_bytes = gl_unpack_pixel_bytes(format, type);
    if (pixel_bytes == 0u) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!pixels) return;

    const gl_texture_object_t *tex =
        gl_find_texture(ctx, ctx->bound_texture_2d ? ctx->bound_texture_2d : 1);
    if (!tex || !tex->pixels) {
        /* No image under the query. The same error glTexSubImage2D raises for the same reason:
         * the caller is asking about something that was never uploaded. */
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (tex->width <= 0 || tex->height <= 0) return;

    const size_t stride = gl_pack_row_stride(ctx, tex->width, pixel_bytes);
    const uint8_t *src = (const uint8_t *)tex->pixels;
    uint8_t *dst = (uint8_t *)pixels;
    memset(dst, 0, stride * (size_t)tex->height);

    for (GLsizei row = 0; row < tex->height; row++) {
        const uint8_t *in = src + (size_t)row * (size_t)tex->pitch * 4u;
        uint8_t *out = dst + (size_t)row * stride;
        for (GLsizei col = 0; col < tex->width; col++) {
            const uint8_t *px = in + (size_t)col * 4u;
            gl_pack_pixel(out + (size_t)col * pixel_bytes, format, px[0], px[1], px[2], px[3]);
        }
    }
}

/* `glCopyTexSubImage2D(...)` - the framebuffer into a bound texture. Render-to-texture, the
 * 1.x way.
 *
 * Reads through `glReadPixels`, which means it inherits the y flip: the window rectangle is
 * read in GL's bottom-left orientation and lands in the texture the same way up as a
 * `glTexSubImage2D` of the same pixels would. Sharing that path rather than copying it is the
 * point - a second flip written by hand is a second chance to get the flip wrong.
 */
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLint x, GLint y, GLsizei width, GLsizei height) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (width == 0 || height == 0) return;

    /* **A bounded scratch rather than an allocation.** This is called per frame by anything
     * doing render-to-texture, and a freestanding binary has no business allocating on that
     * path; a rectangle wider than the scratch is refused rather than silently truncated. */
    enum { COPY_MAX_WIDTH = 2048 };
    static uint8_t row[COPY_MAX_WIDTH * 4];
    if (width > COPY_MAX_WIDTH) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    /* One row at a time, so the scratch is a row rather than a rectangle. Row `r` of the
     * destination is row `r` of the source read in GL orientation, which is what keeps this
     * identical to a glTexSubImage2D of the same pixels. */
    GLint saved_pack = ctx->pack_alignment;
    ctx->pack_alignment = 1; /* the scratch is tight; no padding to skip */
    for (GLsizei r = 0; r < height; r++) {
        glReadPixels(x, y + r, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
        glTexSubImage2D(target, level, xoffset, yoffset + r, width, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, row);
    }
    ctx->pack_alignment = saved_pack;
}

/* `glCopyTexImage2D(...)` - the allocating half of the copy pair.
 *
 * Sizes the texture from the window rectangle and then copies into it through
 * glCopyTexSubImage2D, so there is exactly one piece of code that decides which way up a
 * framebuffer copy lands. Writing the flip a second time here is the failure this avoids: it
 * would work for a square rectangle and be upside down for everything else.
 *
 * `border` must be zero. The bordered texture is a GL 1.0 feature no implementation has
 * supported for decades, and accepting a non-zero border would mean claiming a texture one
 * pixel larger in each direction than the one actually allocated.
 */
void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                      GLint x, GLint y, GLsizei width, GLsizei height, GLint border) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (border != 0 || width <= 0 || height <= 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    /* Allocated with no pixels: every texel is overwritten by the copy below, so uploading
     * anything here would be work thrown away. */
    glTexImage2D(target, level, (GLint)internalformat, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    /* **Checked against the texture, not against glGetError.** GL errors are sticky, so an
     * error left over from some earlier call would abort a copy that had every right to run.
     * What matters is whether the allocation above actually produced an image of this size. */
    gl_texture_object_t *tex =
        gl_find_texture(ctx, ctx->bound_texture_2d ? ctx->bound_texture_2d : 1);
    if (!tex || !tex->pixels || tex->width != width || tex->height != height) return;

    glCopyTexSubImage2D(target, level, 0, 0, x, y, width, height);
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const GLvoid *pixels) {
    (void)level;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    size_t pixel_bytes = gl_unpack_pixel_bytes(format, type);
    if (pixel_bytes == 0u) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width < 0 || height < 0 || xoffset < 0 || yoffset < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    gl_texture_object_t *tex = gl_find_texture(ctx, ctx->bound_texture_2d);
    if (!tex || !tex->pixels) {
        /* Nothing to update. The specification's error for a sub-image with no image under it. */
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* **Bounds are an error, not a clamp.** A sub-image that ran off the edge would otherwise
     * corrupt whatever followed the texture, or silently draw a different rectangle from the
     * one the caller asked for. */
    if ((size_t)xoffset + (size_t)width > (size_t)tex->width ||
        (size_t)yoffset + (size_t)height > (size_t)tex->height) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (width == 0 || height == 0 || !pixels) return;

    const uint8_t *src = (const uint8_t *)pixels;
    uint8_t *dst = (uint8_t *)tex->pixels;
    size_t stride = gl_unpack_row_stride(ctx, width, pixel_bytes);
    for (size_t y = 0; y < (size_t)height; y++) {
        uint8_t *row = dst + (((size_t)yoffset + y) * (size_t)tex->pitch + (size_t)xoffset) * 4u;
        gl_unpack_row(row, src + y * stride, width, format);
    }
#if !defined(OOPS_HOST_BUILD) && defined(__x86_64__)
    if (tex->garlic_data) {
        size_t bytes = (size_t)tex->pitch * (size_t)tex->height * 4u;
        for (size_t p = 0; p < bytes; p += 64) {
            __builtin_ia32_clflush((const void *)((const char *)tex->garlic_data + p));
        }
    }
#endif
    gl_pack_descriptors(tex);
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                 GLsizei width, GLsizei height, GLint border,
                 GLenum format, GLenum type, const GLvoid *pixels) {
    (void)level; (void)internalformat; (void)border;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (width <= 0 || height <= 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* **Checked before anything is allocated.** This used to convert only GL_RGBA and GL_RGB and
     * fall through silently for everything else, leaving the texture holding whatever the
     * allocation contained - uninitialised memory, sampled, with no error raised. */
    size_t pixel_bytes = gl_unpack_pixel_bytes(format, type);
    if (pixel_bytes == 0u) {
        gl_record_error(ctx, GL_INVALID_ENUM);
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
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    tex->garlic_va = (uint64_t)(uintptr_t)tex->garlic_data;
    tex->pixels = tex->garlic_data;
    tex->pitch = (uint32_t)pitch_px;
    (void)num_pixels;

    if (pixels) {
        const uint8_t *src = (const uint8_t *)pixels;
        uint8_t *dst = (uint8_t *)tex->garlic_data;
        size_t stride = gl_unpack_row_stride(ctx, width, pixel_bytes);
        for (size_t y = 0; y < (size_t)height; y++) {
            gl_unpack_row(dst + y * pitch_px * 4, src + y * stride, width, format);
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
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    if (pixels) {
        const uint8_t *src = (const uint8_t *)pixels;
        uint8_t *dst = (uint8_t *)tex->pixels;
        size_t stride = gl_unpack_row_stride(ctx, width, pixel_bytes);
        for (size_t y = 0; y < (size_t)height; y++) {
            gl_unpack_row(dst + y * pitch_px * 4, src + y * stride, width, format);
        }
    } else {
        /* A null pointer reserves the storage, which is legal and is how a program sets a
         * texture up to fill with glTexSubImage2D. Zeroed rather than left as the allocator
         * found it, so a draw before the first sub-image samples black instead of whatever
         * was in that memory. */
        memset(tex->pixels, 0, rgba_bytes);
    }
    (void)num_pixels;
#endif

    tex->width = width;
    tex->height = height;
    /* **Set here rather than only in the target branch**, where it used to be. On a host build
     * it stayed zero, which was harmless for exactly as long as nothing read it - and
     * glTexSubImage2D reads it to find a row, so every row landed on top of row zero. A field
     * that is only correct under one build is the kind that is discovered by the first code to
     * depend on it. */
    tex->pitch = (uint32_t)pitch_px;
    tex->format = format;
    tex->type = type;
    gl_pack_descriptors(tex);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }

    gl_texture_object_t *tex = gl_find_or_create_texture(ctx, ctx->bound_texture_2d ? ctx->bound_texture_2d : 1);
    if (!tex) return;

    switch (pname) {
        case GL_TEXTURE_WRAP_S:     tex->wrap_s = (GLenum)param; break;
        case GL_TEXTURE_WRAP_T:     tex->wrap_t = (GLenum)param; break;
        case GL_TEXTURE_MIN_FILTER: tex->min_filter = (GLenum)param; break;
        case GL_TEXTURE_MAG_FILTER: tex->mag_filter = (GLenum)param; break;
        /* The target was checked above and the parameter was not, so a caller setting one
         * this does not keep - GL_TEXTURE_WRAP_R, a LOD bias - had it dropped and went on
         * believing the texture was configured. */
        default: gl_record_error(ctx, GL_INVALID_ENUM); return;
    }
    gl_pack_descriptors(tex);
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    glTexParameteri(target, pname, (GLint)param);
}

/* The vector forms. Every texture parameter this port has is single-valued - the multi-valued
 * ones are GL_TEXTURE_BORDER_COLOR and the priority, neither of which exists here - so these
 * read one element. A null pointer is ignored rather than dereferenced. */
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params) {
    if (params) glTexParameteri(target, pname, params[0]);
}

void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) {
    if (params) glTexParameterf(target, pname, params[0]);
}

/* **Texture residency, answered honestly rather than optimistically.**
 *
 * Every texture here lives in memory the GPU can already reach and nothing evicts them, so they
 * are all resident and glAreTexturesResident returns GL_TRUE. A name that was never generated
 * is an error, not a "no" - a program asking about a texture it does not own has a bug, and
 * reporting it as merely non-resident would hide that.
 */
GLboolean glAreTexturesResident(GLsizei n, const GLuint *textures, GLboolean *residences) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !textures || !residences) return GL_FALSE;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return GL_FALSE;
    }
    for (GLsizei i = 0; i < n; i++) {
        if (!gl_find_texture(ctx, textures[i])) {
            gl_record_error(ctx, GL_INVALID_VALUE);
            return GL_FALSE;
        }
        residences[i] = GL_TRUE;
    }
    return GL_TRUE;
}

/* A hint about which textures to keep resident, on an implementation where nothing is ever
 * evicted. Accepted and ignored, which is what the specification permits a hint to be - but the
 * names are still validated, so a typo is still an error. */
void glPrioritizeTextures(GLsizei n, const GLuint *textures, const GLclampf *priorities) {
    (void)priorities;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !textures) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        /* Name 0 is silently ignored here, as GL says, rather than refused. */
        if (textures[i] != 0u && !gl_find_texture(ctx, textures[i])) {
            gl_record_error(ctx, GL_INVALID_VALUE);
            return;
        }
    }
}

GLboolean glIsTexture(GLuint texture) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || texture == 0) return GL_FALSE;
    return gl_find_texture(ctx, texture) != NULL ? GL_TRUE : GL_FALSE;
}
