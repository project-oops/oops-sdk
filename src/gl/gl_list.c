/*
 * oops-gl: display lists
 *
 * A list records the calls made between glNewList and glEndList and replays them on glCallList.
 * This is the oldest feature in OpenGL and the one GL 1.x programs lean on hardest: static
 * geometry is compiled once and drawn by name for the rest of the run.
 *
 * # What is recorded is the call, not its effect
 *
 * Each entry point that can be compiled appends a `gl_list_cmd_t` and, under GL_COMPILE,
 * returns without doing anything. Replay calls the same entry points again in order. So a list
 * compiled before a texture is bound and executed after it draws with the *later* texture,
 * which is what the specification says happens and would not if the effect were baked in.
 *
 * # Everything the specification compiles, is compiled
 *
 * **Until 2026-09-19 this recorded 21 operations** - the vertex attributes, the enables, the
 * matrix-stack calls and a handful of others - and every other call ran at compile time. A
 * `glMaterialfv` inside a GL_COMPILE list therefore changed the material *while the list was being
 * built* and was absent when it was called, so a program that gave each object its own material
 * in its own list drew every object in whichever material was set last. That is the gears demo,
 * and the shape of a great deal of GL 1.x code.
 *
 * The specification names the calls that are *not* compiled - they execute immediately even
 * inside glNewList: everything that returns a value (glGen*, glIs*, glGet*, glRenderMode,
 * glReadPixels), the client-side state (the array pointers, glEnable/DisableClientState,
 * glInterleavedArrays, glPixelStore, glPush/PopClientAttrib, glClientActiveTexture), the object
 * lifetimes (glDeleteTextures, glDeleteLists, the buffer-object calls), glNewList and glEndList
 * themselves, and glFlush and glFinish. Those simply have no hook. Every other entry point has
 * one, at the one place its spellings converge - `glColor3ub` forwards to `glColor4f`, which
 * records.
 *
 * # What a list keeps
 *
 * Arguments passed by pointer are copied when the call is compiled, not when it is replayed: a
 * matrix, a list of names, a light's position - and an image, which is unpacked through the
 * **compile-time** `glPixelStorei` state into tight rows. Replay then runs it under default
 * unpacking, which is what makes a list independent of whatever the pixel-store state has become
 * by the time it is called. That is Mesa's arrangement too (`main/dlist.c`, `unpack_image`).
 *
 * The vertex-array draws compile the same way: the specification says a list records what the
 * arrays held when the draw was compiled, so `glDrawArrays` and friends expand, at compile time,
 * into the `glBegin`/attribute/`glEnd` sequence they are defined to be equivalent to. They were
 * refused with GL_INVALID_OPERATION before, which was honest but not GL.
 *
 * # GL_COMPILE_AND_EXECUTE
 *
 * Handled in one place: the recorder appends the command and then *executes the command it just
 * recorded*, with recording suspended, and tells the caller to stop. So an entry point that is
 * recorded and whose body calls other recorded entry points - `glCopyTexImage2D` does - records
 * once rather than once per level of nesting, and a list called while another is being compiled
 * this way runs without being copied into it. Both of those recorded twice before.
 */

#include "gl_internal.h"

#ifdef OOPS_HOST_BUILD
#include <stdlib.h>
#endif

/* The allocator differs by build: the SDK's own heap on the target, the C library's on the
 * host, as for buffer objects. */
void *gl_list_alloc(size_t bytes) {
    if (bytes == 0u) return (void *)0;
#ifdef OOPS_HOST_BUILD
    return malloc(bytes);
#else
    return oops_malloc(bytes);
#endif
}

static void gl_list_release(void *p) {
    if (!p) return;
#ifdef OOPS_HOST_BUILD
    free(p);
#else
    oops_free(p);
#endif
}

/* Whether a name is one this implementation can hold at all. Lists are named from 1; zero is
 * reserved by the specification and never a valid list. */
static GLboolean gl_list_nameable(GLuint list) {
    return (GLboolean)(list >= 1u && list <= GL_MAX_LISTS);
}

static gl_display_list_t *gl_list_slot(gl_context_t *ctx, GLuint list) {
    if (!gl_list_nameable(list)) return (gl_display_list_t *)0;
    return &ctx->lists[list - 1u];
}

/* Empties a list and gives back everything it held. The slot keeps its name. */
static void gl_list_clear(gl_display_list_t *list) {
    if (!list) return;
    for (GLuint i = 0; i < list->count; i++) gl_list_release(list->cmds[i].data);
    gl_list_release(list->cmds);
    list->cmds = (gl_list_cmd_t *)0;
    list->count = 0u;
    list->capacity = 0u;
}

void gl_list_free_all(gl_context_t *ctx) {
    if (!ctx) return;
    for (GLuint i = 0; i < GL_MAX_LISTS; i++) {
        gl_list_clear(&ctx->lists[i]);
        ctx->lists[i].used = GL_FALSE;
        ctx->lists[i].compiled = GL_FALSE;
    }
    ctx->list_compiling = 0u;
    ctx->list_suspend = 0u;
    ctx->list_depth = 0u;
}

void *gl_list_pack_rows(const void *src, size_t row_bytes, size_t src_stride, GLsizei rows) {
    if (!src || row_bytes == 0u || rows <= 0) return (void *)0;
    const size_t total = row_bytes * (size_t)rows;
    uint8_t *out = (total / (size_t)rows == row_bytes) ? (uint8_t *)gl_list_alloc(total)
                                                       : (uint8_t *)0; /* would overflow */
    if (!out) {
        gl_record_error(gl_get_ctx(), GL_OUT_OF_MEMORY);
        return (void *)0;
    }
    const uint8_t *in = (const uint8_t *)src;
    for (GLsizei r = 0; r < rows; r++) {
        memcpy(out + (size_t)r * row_bytes, in + (size_t)r * src_stride, row_bytes);
    }
    return out;
}

int gl_list_param_count(gl_list_op_t op, GLenum pname) {
    switch (op) {
        case GL_LIST_OP_LIGHT_FV:
        case GL_LIST_OP_LIGHT_IV:
            if (pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR ||
                pname == GL_POSITION) return 4;
            return (pname == GL_SPOT_DIRECTION) ? 3 : 1;
        case GL_LIST_OP_MATERIAL_FV:
        case GL_LIST_OP_MATERIAL_IV:
            if (pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR ||
                pname == GL_EMISSION || pname == GL_AMBIENT_AND_DIFFUSE) return 4;
            return (pname == GL_COLOR_INDEXES) ? 3 : 1;
        case GL_LIST_OP_LIGHT_MODEL_FV:
        case GL_LIST_OP_LIGHT_MODEL_IV:
            return (pname == GL_LIGHT_MODEL_AMBIENT) ? 4 : 1;
        case GL_LIST_OP_FOG_FV:
        case GL_LIST_OP_FOG_IV:
            return (pname == GL_FOG_COLOR) ? 4 : 1;
        case GL_LIST_OP_TEX_ENV_FV:
        case GL_LIST_OP_TEX_ENV_IV:
            return (pname == GL_TEXTURE_ENV_COLOR) ? 4 : 1;
        case GL_LIST_OP_TEX_GEN_FV:
        case GL_LIST_OP_TEX_GEN_IV:
            return (pname == GL_OBJECT_PLANE || pname == GL_EYE_PLANE) ? 4 : 1;
        case GL_LIST_OP_TEX_PARAMETER_FV:
        case GL_LIST_OP_TEX_PARAMETER_IV:
            return (pname == GL_TEXTURE_BORDER_COLOR) ? 4 : 1;
        default:
            return 1;
    }
}

static void gl_list_execute_cmd(gl_context_t *ctx, const gl_list_cmd_t *cmd);

/* Appends one command, taking ownership of `owned`, and runs it if the list is also executing.
 *
 * **A list that cannot grow is an error, not a truncation.** Dropping commands silently would
 * give a list that draws part of what was compiled into it, which looks like a modelling mistake
 * rather than a limit being hit. */
GLboolean gl_list_rec_owned(gl_list_op_t op, const gl_list_arg_t *args, int nargs, void *owned) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || ctx->list_compiling == 0u || ctx->list_suspend != 0u) {
        gl_list_release(owned);
        return GL_FALSE;
    }
    gl_display_list_t *list = gl_list_slot(ctx, ctx->list_compiling);
    if (!list || nargs < 0 || nargs > GL_LIST_MAX_ARGS) {
        gl_list_release(owned);
        return GL_FALSE;
    }

    if (list->count == list->capacity) {
        const GLuint grown = list->capacity ? list->capacity * 2u : 64u;
        gl_list_cmd_t *cmds = (gl_list_cmd_t *)gl_list_alloc((size_t)grown * sizeof(gl_list_cmd_t));
        if (!cmds || grown < list->capacity) {
            gl_list_release(cmds);
            gl_list_release(owned);
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            return (GLboolean)(ctx->list_mode == GL_COMPILE);
        }
        if (list->count) memcpy(cmds, list->cmds, (size_t)list->count * sizeof(gl_list_cmd_t));
        gl_list_release(list->cmds);
        list->cmds = cmds;
        list->capacity = grown;
    }

    gl_list_cmd_t *cmd = &list->cmds[list->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->op = op;
    for (int i = 0; i < nargs; i++) cmd->a[i] = args[i];
    cmd->data = owned;

    if (ctx->list_mode == GL_COMPILE_AND_EXECUTE) {
        ctx->list_suspend++;
        gl_list_execute_cmd(ctx, cmd);
        ctx->list_suspend--;
    }
    return GL_TRUE;
}

GLboolean gl_list_rec(gl_list_op_t op, const gl_list_arg_t *args, int nargs,
                      const void *data, size_t bytes) {
    void *copy = (void *)0;
    if (data && bytes) {
        copy = gl_list_alloc(bytes);
        if (!copy) {
            gl_context_t *ctx = gl_get_ctx();
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            return (GLboolean)(ctx && ctx->list_mode == GL_COMPILE);
        }
        memcpy(copy, data, bytes);
    }
    return gl_list_rec_owned(op, args, nargs, copy);
}

GLuint glGenLists(GLsizei range) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return 0u;
    if (range < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return 0u;
    }
    if (range == 0) return 0u;

    /* A contiguous run, as the specification requires - a caller is entitled to treat the
     * answer as a base and add to it. */
    for (GLuint start = 1u; start + (GLuint)range - 1u <= GL_MAX_LISTS; start++) {
        GLboolean clear = GL_TRUE;
        for (GLsizei i = 0; i < range; i++) {
            if (ctx->lists[start + (GLuint)i - 1u].used) {
                clear = GL_FALSE;
                break;
            }
        }
        if (!clear) continue;
        for (GLsizei i = 0; i < range; i++) {
            gl_display_list_t *list = &ctx->lists[start + (GLuint)i - 1u];
            gl_list_clear(list);
            list->used = GL_TRUE;
            list->compiled = GL_FALSE;
        }
        return start;
    }
    /* No run that long. Zero is the specification's answer and means "none allocated"; it is
     * not an error, so glGetError stays clear and a caller that checks the return finds out. */
    return 0u;
}

GLboolean glIsList(GLuint list) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GL_FALSE;
    gl_display_list_t *slot = gl_list_slot(ctx, list);
    return (GLboolean)(slot && slot->used && slot->compiled);
}

void glDeleteLists(GLuint list, GLsizei range) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (range < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (GLsizei i = 0; i < range; i++) {
        const GLuint name = list + (GLuint)i;
        gl_display_list_t *slot = gl_list_slot(ctx, name);
        if (!slot) continue; /* names outside the range were never ours; silently ignored */
        /* Not the one being recorded into: its storage is still being appended to, and the
         * specification leaves deleting it undefined. It is released when it is next named. */
        if (name == ctx->list_compiling) continue;
        gl_list_clear(slot);
        slot->used = GL_FALSE;
        slot->compiled = GL_FALSE;
    }
}

void glNewList(GLuint list, GLenum mode) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (list == 0u || !gl_list_nameable(list)) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (ctx->list_compiling != 0u) {
        /* Lists do not nest. The specification says so, and allowing it would make the
         * recursion guard in glCallList meaningless. */
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }

    gl_display_list_t *slot = gl_list_slot(ctx, list);
    gl_list_clear(slot); /* a redefinition replaces the old contents */
    slot->used = GL_TRUE;
    slot->compiled = GL_FALSE; /* not callable until glEndList closes it */
    ctx->list_compiling = list;
    ctx->list_mode = mode;
}

void glEndList(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (ctx->list_compiling == 0u) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    gl_display_list_t *slot = gl_list_slot(ctx, ctx->list_compiling);
    if (slot) slot->compiled = GL_TRUE;
    ctx->list_compiling = 0u;
}

void glListBase(GLuint base) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_LIST_BASE, gl_la_u(base))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->list_base = base;
}

/* Runs one recorded command. Pixel-carrying commands run under default unpacking, because
 * their data was packed tight when it was recorded. */
static void gl_list_execute_cmd(gl_context_t *ctx, const gl_list_cmd_t *cmd) {
    const gl_list_arg_t *a = cmd->a;
    const GLfloat v4f[4] = {a[2].f, a[3].f, a[4].f, a[5].f};
    const GLint v4i[4] = {a[2].i, a[3].i, a[4].i, a[5].i};
    const GLfloat v4f1[4] = {a[1].f, a[2].f, a[3].f, a[4].f};
    const GLint v4i1[4] = {a[1].i, a[2].i, a[3].i, a[4].i};

    switch (cmd->op) {
        case GL_LIST_OP_BEGIN:         glBegin(a[0].e); break;
        case GL_LIST_OP_END:           glEnd(); break;
        case GL_LIST_OP_VERTEX:        glVertex4f(a[0].f, a[1].f, a[2].f, a[3].f); break;
        case GL_LIST_OP_COLOR:         glColor4f(a[0].f, a[1].f, a[2].f, a[3].f); break;
        case GL_LIST_OP_SECONDARY_COLOR: glSecondaryColor3f(a[0].f, a[1].f, a[2].f); break;
        case GL_LIST_OP_FOG_COORD:     glFogCoordf(a[0].f); break;
        case GL_LIST_OP_NORMAL:        glNormal3f(a[0].f, a[1].f, a[2].f); break;
        case GL_LIST_OP_TEXCOORD:      glTexCoord4f(a[0].f, a[1].f, a[2].f, a[3].f); break;
        case GL_LIST_OP_ENABLE:        glEnable(a[0].e); break;
        case GL_LIST_OP_DISABLE:       glDisable(a[0].e); break;
        case GL_LIST_OP_MATRIX_MODE:   glMatrixMode(a[0].e); break;
        case GL_LIST_OP_LOAD_IDENTITY: glLoadIdentity(); break;
        case GL_LIST_OP_PUSH_MATRIX:   glPushMatrix(); break;
        case GL_LIST_OP_POP_MATRIX:    glPopMatrix(); break;
        case GL_LIST_OP_TRANSLATE:     glTranslatef(a[0].f, a[1].f, a[2].f); break;
        case GL_LIST_OP_ROTATE:        glRotatef(a[0].f, a[1].f, a[2].f, a[3].f); break;
        case GL_LIST_OP_SCALE:         glScalef(a[0].f, a[1].f, a[2].f); break;
        case GL_LIST_OP_LOAD_MATRIX:   if (cmd->data) glLoadMatrixf((const GLfloat *)cmd->data); break;
        case GL_LIST_OP_MULT_MATRIX:   if (cmd->data) glMultMatrixf((const GLfloat *)cmd->data); break;
        case GL_LIST_OP_ORTHO:
            glOrtho(a[0].f, a[1].f, a[2].f, a[3].f, a[4].f, a[5].f);
            break;
        case GL_LIST_OP_FRUSTUM:
            glFrustum(a[0].f, a[1].f, a[2].f, a[3].f, a[4].f, a[5].f);
            break;
        case GL_LIST_OP_BIND_TEXTURE:  glBindTexture(a[0].e, a[1].u); break;
        case GL_LIST_OP_SHADE_MODEL:   glShadeModel(a[0].e); break;
        case GL_LIST_OP_CULL_FACE:     glCullFace(a[0].e); break;
        case GL_LIST_OP_FRONT_FACE:    glFrontFace(a[0].e); break;
        case GL_LIST_OP_DEPTH_FUNC:    glDepthFunc(a[0].e); break;
        case GL_LIST_OP_BLEND_FUNC:    glBlendFunc(a[0].e, a[1].e); break;
        case GL_LIST_OP_BLEND_FUNC_SEPARATE:
            glBlendFuncSeparate(a[0].e, a[1].e, a[2].e, a[3].e);
            break;
        case GL_LIST_OP_BLEND_EQUATION: glBlendEquation(a[0].e); break;
        case GL_LIST_OP_BLEND_COLOR:   glBlendColor(a[0].f, a[1].f, a[2].f, a[3].f); break;
        case GL_LIST_OP_LOGIC_OP:      glLogicOp(a[0].e); break;
        case GL_LIST_OP_ALPHA_FUNC:    glAlphaFunc(a[0].e, a[1].f); break;
        case GL_LIST_OP_DEPTH_MASK:    glDepthMask((GLboolean)a[0].u); break;
        case GL_LIST_OP_DEPTH_RANGE:   glDepthRange(a[0].f, a[1].f); break;
        case GL_LIST_OP_COLOR_MASK:
            glColorMask((GLboolean)a[0].u, (GLboolean)a[1].u, (GLboolean)a[2].u, (GLboolean)a[3].u);
            break;
        case GL_LIST_OP_CLEAR:         glClear(a[0].u); break;
        case GL_LIST_OP_CLEAR_COLOR:   glClearColor(a[0].f, a[1].f, a[2].f, a[3].f); break;
        case GL_LIST_OP_CLEAR_DEPTH:   glClearDepth(a[0].f); break;
        case GL_LIST_OP_CLEAR_STENCIL: glClearStencil(a[0].i); break;
        case GL_LIST_OP_STENCIL_FUNC:  glStencilFunc(a[0].e, a[1].i, a[2].u); break;
        case GL_LIST_OP_STENCIL_OP:    glStencilOp(a[0].e, a[1].e, a[2].e); break;
        case GL_LIST_OP_STENCIL_MASK:  glStencilMask(a[0].u); break;
        case GL_LIST_OP_CLIP_PLANE: {
            const GLdouble eq[4] = {a[1].f, a[2].f, a[3].f, a[4].f};
            glClipPlane(a[0].e, eq);
            break;
        }
        case GL_LIST_OP_COLOR_MATERIAL: glColorMaterial(a[0].e, a[1].e); break;
        case GL_LIST_OP_LIGHT_FV:      glLightfv(a[0].e, a[1].e, v4f); break;
        case GL_LIST_OP_LIGHT_IV:      glLightiv(a[0].e, a[1].e, v4i); break;
        case GL_LIST_OP_MATERIAL_FV:   glMaterialfv(a[0].e, a[1].e, v4f); break;
        case GL_LIST_OP_MATERIAL_IV:   glMaterialiv(a[0].e, a[1].e, v4i); break;
        case GL_LIST_OP_LIGHT_MODEL_FV: glLightModelfv(a[0].e, v4f1); break;
        case GL_LIST_OP_LIGHT_MODEL_IV: glLightModeliv(a[0].e, v4i1); break;
        case GL_LIST_OP_FOG_F:         glFogf(a[0].e, a[1].f); break;
        case GL_LIST_OP_FOG_I:         glFogi(a[0].e, a[1].i); break;
        case GL_LIST_OP_FOG_FV:        glFogfv(a[0].e, v4f1); break;
        case GL_LIST_OP_FOG_IV:        glFogiv(a[0].e, v4i1); break;
        case GL_LIST_OP_TEX_ENV_I:     glTexEnvi(a[0].e, a[1].e, a[2].i); break;
        case GL_LIST_OP_TEX_ENV_FV:    glTexEnvfv(a[0].e, a[1].e, v4f); break;
        case GL_LIST_OP_TEX_ENV_IV:    glTexEnviv(a[0].e, a[1].e, v4i); break;
        case GL_LIST_OP_TEX_GEN_I:     glTexGeni(a[0].e, a[1].e, a[2].i); break;
        case GL_LIST_OP_TEX_GEN_FV:    glTexGenfv(a[0].e, a[1].e, v4f); break;
        case GL_LIST_OP_TEX_GEN_IV:    glTexGeniv(a[0].e, a[1].e, v4i); break;
        case GL_LIST_OP_TEX_PARAMETER_I: glTexParameteri(a[0].e, a[1].e, a[2].i); break;
        case GL_LIST_OP_TEX_PARAMETER_F: glTexParameterf(a[0].e, a[1].e, a[2].f); break;
        case GL_LIST_OP_TEX_PARAMETER_FV: glTexParameterfv(a[0].e, a[1].e, v4f); break;
        case GL_LIST_OP_TEX_PARAMETER_IV: glTexParameteriv(a[0].e, a[1].e, v4i); break;
        case GL_LIST_OP_TEX_IMAGE_1D:
        case GL_LIST_OP_TEX_IMAGE_2D:
        case GL_LIST_OP_TEX_SUB_IMAGE_1D:
        case GL_LIST_OP_TEX_SUB_IMAGE_2D:
        case GL_LIST_OP_BITMAP:
        case GL_LIST_OP_POLYGON_STIPPLE:
        case GL_LIST_OP_TEX_IMAGE_3D:
        case GL_LIST_OP_TEX_SUB_IMAGE_3D:
        case GL_LIST_OP_DRAW_PIXELS: {
            /* The rows were packed tight and native at compile time, and a bitmap most
             * significant bit first: alignment 1, no row length, skips or swapping, a volume's
             * slices back to back. */
            gl_unpack_saved_t saved_unpack;
            gl_unpack_neutral(ctx, &saved_unpack);
            switch (cmd->op) {
                case GL_LIST_OP_TEX_IMAGE_3D:
                    glTexImage3D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i, a[6].i, a[7].e,
                                 a[8].e, cmd->data);
                    break;
                case GL_LIST_OP_TEX_SUB_IMAGE_3D:
                    glTexSubImage3D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i, a[6].i,
                                    a[7].i, a[8].e, a[9].e, cmd->data);
                    break;
                case GL_LIST_OP_TEX_IMAGE_1D:
                    glTexImage1D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].e, a[6].e, cmd->data);
                    break;
                case GL_LIST_OP_TEX_IMAGE_2D:
                    glTexImage2D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i, a[6].e, a[7].e,
                                 cmd->data);
                    break;
                case GL_LIST_OP_TEX_SUB_IMAGE_1D:
                    glTexSubImage1D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].e, a[5].e, cmd->data);
                    break;
                case GL_LIST_OP_TEX_SUB_IMAGE_2D:
                    glTexSubImage2D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i, a[6].e, a[7].e,
                                    cmd->data);
                    break;
                case GL_LIST_OP_BITMAP:
                    glBitmap(a[0].i, a[1].i, a[2].f, a[3].f, a[4].f, a[5].f,
                             (const GLubyte *)cmd->data);
                    break;
                case GL_LIST_OP_POLYGON_STIPPLE:
                    glPolygonStipple((const GLubyte *)cmd->data);
                    break;
                default: /* GL_LIST_OP_DRAW_PIXELS */
                    glDrawPixels(a[0].i, a[1].i, a[2].e, a[3].e, cmd->data);
                    break;
            }
            gl_unpack_restore(ctx, &saved_unpack);
            break;
        }
        case GL_LIST_OP_COPY_TEX_SUB_IMAGE_3D:
            glCopyTexSubImage3D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i, a[6].i, a[7].i,
                                a[8].i);
            break;
        case GL_LIST_OP_COPY_TEX_IMAGE_1D:
            glCopyTexImage1D(a[0].e, a[1].i, a[2].e, a[3].i, a[4].i, a[5].i, a[6].i);
            break;
        case GL_LIST_OP_COPY_TEX_IMAGE_2D:
            glCopyTexImage2D(a[0].e, a[1].i, a[2].e, a[3].i, a[4].i, a[5].i, a[6].i, a[7].i);
            break;
        case GL_LIST_OP_COPY_TEX_SUB_IMAGE_1D:
            glCopyTexSubImage1D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i);
            break;
        case GL_LIST_OP_COPY_TEX_SUB_IMAGE_2D:
            glCopyTexSubImage2D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i, a[6].i, a[7].i);
            break;
        case GL_LIST_OP_PRIORITIZE_TEXTURES:
            /* n names, then n priorities, in one block. */
            if (cmd->data && a[0].i > 0) {
                const GLuint *names = (const GLuint *)cmd->data;
                glPrioritizeTextures(a[0].i, names, (const GLclampf *)(names + a[0].i));
            } else {
                glPrioritizeTextures(a[0].i, (const GLuint *)0, (const GLclampf *)0);
            }
            break;
        case GL_LIST_OP_ACTIVE_TEXTURE: glActiveTexture(a[0].e); break;
        case GL_LIST_OP_MULTI_TEXCOORD:
            glMultiTexCoord4f(a[0].e, a[1].f, a[2].f, a[3].f, a[4].f);
            break;
        case GL_LIST_OP_POINT_SIZE:    glPointSize(a[0].f); break;
        case GL_LIST_OP_BEGIN_QUERY:   glBeginQuery(a[0].e, a[1].u); break;
        case GL_LIST_OP_END_QUERY:     glEndQuery(a[0].e); break;
        case GL_LIST_OP_POINT_PARAMETER: {
            const GLfloat p[3] = {a[1].f, a[2].f, a[3].f};
            glPointParameterfv(a[0].e, p);
            break;
        }
        case GL_LIST_OP_LINE_WIDTH:    glLineWidth(a[0].f); break;
        case GL_LIST_OP_POLYGON_OFFSET: glPolygonOffset(a[0].f, a[1].f); break;
        case GL_LIST_OP_SCISSOR:       glScissor(a[0].i, a[1].i, a[2].i, a[3].i); break;
        case GL_LIST_OP_VIEWPORT:      glViewport(a[0].i, a[1].i, a[2].i, a[3].i); break;
        case GL_LIST_OP_HINT:          glHint(a[0].e, a[1].e); break;
        case GL_LIST_OP_DRAW_BUFFER:   glDrawBuffer(a[0].e); break;
        case GL_LIST_OP_READ_BUFFER:   glReadBuffer(a[0].e); break;
        case GL_LIST_OP_PIXEL_ZOOM:    glPixelZoom(a[0].f, a[1].f); break;
        case GL_LIST_OP_RASTER_POS:    glRasterPos4f(a[0].f, a[1].f, a[2].f, a[3].f); break;
        case GL_LIST_OP_WINDOW_POS:    glWindowPos3f(a[0].f, a[1].f, a[2].f); break;
        case GL_LIST_OP_COPY_PIXELS:   glCopyPixels(a[0].i, a[1].i, a[2].i, a[3].i, a[4].e); break;
        case GL_LIST_OP_PUSH_ATTRIB:   glPushAttrib(a[0].u); break;
        case GL_LIST_OP_POP_ATTRIB:    glPopAttrib(); break;
        case GL_LIST_OP_LIST_BASE:     glListBase(a[0].u); break;
        case GL_LIST_OP_CALL_LIST:     glCallList(a[0].u); break;
        case GL_LIST_OP_POLYGON_MODE:  glPolygonMode(a[0].e, a[1].e); break;
        case GL_LIST_OP_EDGE_FLAG:     glEdgeFlag((GLboolean)a[0].u); break;
        case GL_LIST_OP_INDEX:         glIndexf(a[0].f); break;
        case GL_LIST_OP_CLEAR_INDEX:   glClearIndex(a[0].f); break;
        case GL_LIST_OP_INDEX_MASK:    glIndexMask(a[0].u); break;
        case GL_LIST_OP_SAMPLE_COVERAGE: glSampleCoverage(a[0].f, (GLboolean)a[1].u); break;
        case GL_LIST_OP_MAP1:
        case GL_LIST_OP_MAP2:          gl_eval_replay_map(cmd); break;
        case GL_LIST_OP_MAP_GRID1:     glMapGrid1f(a[0].i, a[1].f, a[2].f); break;
        case GL_LIST_OP_MAP_GRID2:     glMapGrid2f(a[0].i, a[1].f, a[2].f, a[3].i, a[4].f, a[5].f); break;
        case GL_LIST_OP_EVAL_COORD1:   glEvalCoord1f(a[0].f); break;
        case GL_LIST_OP_EVAL_COORD2:   glEvalCoord2f(a[0].f, a[1].f); break;
        case GL_LIST_OP_EVAL_POINT1:   glEvalPoint1(a[0].i); break;
        case GL_LIST_OP_EVAL_POINT2:   glEvalPoint2(a[0].i, a[1].i); break;
        case GL_LIST_OP_EVAL_MESH1:    glEvalMesh1(a[0].e, a[1].i, a[2].i); break;
        case GL_LIST_OP_EVAL_MESH2:    glEvalMesh2(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i); break;
        case GL_LIST_OP_INIT_NAMES:    glInitNames(); break;
        case GL_LIST_OP_LOAD_NAME:     glLoadName(a[0].u); break;
        case GL_LIST_OP_PUSH_NAME:     glPushName(a[0].u); break;
        case GL_LIST_OP_POP_NAME:      glPopName(); break;
        case GL_LIST_OP_PASS_THROUGH:  glPassThrough(a[0].f); break;
        case GL_LIST_OP_PIXEL_TRANSFER: glPixelTransferf(a[0].e, a[1].f); break;
        case GL_LIST_OP_LINE_STIPPLE:  glLineStipple(a[0].i, (GLushort)a[1].u); break;
        case GL_LIST_OP_ACCUM:         glAccum(a[0].e, a[1].f); break;
        case GL_LIST_OP_CLEAR_ACCUM:   glClearAccum(a[0].f, a[1].f, a[2].f, a[3].f); break;
        case GL_LIST_OP_PIXEL_MAP:     glPixelMapfv(a[0].e, a[1].i, (const GLfloat *)cmd->data); break;
        case GL_LIST_OP_CALL_LISTS:
            /* The names were widened to GLuint when compiled; the base is the one current now,
             * which is what the specification says a compiled glCallLists uses. */
            if (cmd->data) glCallLists(a[0].i, GL_UNSIGNED_INT, cmd->data);
            break;
        default: break;
    }
}

/* Replays one list.
 *
 * Calls the public entry points rather than reaching into the context, so a list does exactly
 * what the program would have done - including passing through whatever validation those
 * entry points do, which is where a compiled call's errors are raised: when it runs, as the
 * specification has it, not when it was recorded. */
static void gl_list_execute(gl_context_t *ctx, const gl_display_list_t *list) {
    for (GLuint i = 0; i < list->count; i++) {
        gl_list_execute_cmd(ctx, &list->cmds[i]);
    }
}

void glCallList(GLuint list) {
    /* Compiling one call of another is legal and is recorded, not followed - so a list may call
     * a list that does not exist yet, which the specification allows. */
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_CALL_LIST, gl_la_u(list))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    gl_display_list_t *slot = gl_list_slot(ctx, list);
    /* Calling an undefined list is *ignored*, not an error - the specification is explicit, and
     * it is what lets a list reference one compiled later. */
    if (!slot || !slot->used || !slot->compiled) return;

    if (ctx->list_depth >= GL_MAX_LIST_DEPTH) {
        /* A list that reaches itself. Bounded rather than detected exactly: the specification
         * leaves the limit implementation-defined, and a depth cap stops the stack overflow a
         * self-calling list would otherwise cause. */
        gl_record_error(ctx, GL_STACK_OVERFLOW);
        return;
    }
    ctx->list_depth++;
    gl_list_execute(ctx, slot);
    ctx->list_depth--;
}

/* Name `i` of glCallLists' array, as the offset from the list base it stands for - every type
 * GL 1.0 gives the call, decoded as Mesa decodes them (main/dlist.c:13510-13575): the signed and
 * unsigned integers as themselves, a float truncated, and GL_2_BYTES to GL_4_BYTES as big-endian
 * byte strings. Only the three unsigned types were accepted until 2026-09-19. */
static GLint gl_call_lists_name(GLenum type, const GLvoid *lists, GLsizei i) {
    const GLubyte *ub = (const GLubyte *)lists;
    const size_t k = (size_t)i;
    switch (type) {
        case GL_BYTE:           return (GLint)((const GLbyte *)lists)[k];
        case GL_UNSIGNED_BYTE:  return (GLint)ub[k];
        case GL_SHORT:          return (GLint)((const GLshort *)lists)[k];
        case GL_UNSIGNED_SHORT: return (GLint)((const GLushort *)lists)[k];
        case GL_INT:            return ((const GLint *)lists)[k];
        case GL_FLOAT:          return (GLint)((const GLfloat *)lists)[k];
        case GL_2_BYTES:        return (GLint)ub[2 * k] * 256 + (GLint)ub[2 * k + 1];
        case GL_3_BYTES:
            return (GLint)ub[3 * k] * 65536 + (GLint)ub[3 * k + 1] * 256 + (GLint)ub[3 * k + 2];
        case GL_4_BYTES:
            return (GLint)(((GLuint)ub[4 * k] << 24) | ((GLuint)ub[4 * k + 1] << 16) |
                           ((GLuint)ub[4 * k + 2] << 8) | (GLuint)ub[4 * k + 3]);
        default:                return (GLint)((const GLuint *)lists)[k]; /* GL_UNSIGNED_INT */
    }
}

void glCallLists(GLsizei n, GLenum type, const GLvoid *lists) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* The type first, as Mesa checks it (main/dlist.c:13481): GL_BYTE through GL_4_BYTES. */
    if (type < GL_BYTE || type > GL_4_BYTES) {
        /* Checked before any read: an unrecognised type read as the widest would run off the
         * end of a byte-sized array. */
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!lists || n == 0) return;

    /* **Compiled as the names, not as the lists they name.** This used to call glCallList per
     * name while compiling, which recorded `base + name` with the base current at *compile*
     * time; the specification applies the base current when the list runs. */
    if (gl_list_recording()) {
        GLuint *names = (GLuint *)gl_list_alloc((size_t)n * sizeof(GLuint));
        if (!names) {
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            if (ctx->list_mode == GL_COMPILE) return;
        } else {
            /* Stored as offsets in GLuint - a negative one wraps, and wraps back when the base
             * is added, as the unsigned sum below does. */
            for (GLsizei i = 0; i < n; i++) names[i] = (GLuint)gl_call_lists_name(type, lists, i);
            if (gl_list_rec_owned(GL_LIST_OP_CALL_LISTS, GL_LIST_ARGV(gl_la_i(n)), 1, names)) {
                return;
            }
        }
    }

    for (GLsizei i = 0; i < n; i++) {
        glCallList(ctx->list_base + (GLuint)gl_call_lists_name(type, lists, i));
    }
}
