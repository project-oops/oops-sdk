/*
 * oops-gl: display lists
 *
 * A list records the calls made between glNewList and glEndList and replays them
 * through the same entry points on glCallList, so state bound later applies at replay.
 * Every call the specification compiles has a hook at the place its spellings converge;
 * the calls it executes immediately (queries, client state, object lifetimes, glFlush,
 * glFinish) have none. Pointer arguments are copied at compile time, images unpacked
 * through the compile-time pixel-store state into tight rows and replayed under default
 * unpacking (Mesa `main/dlist.c`, `unpack_image`). Vertex-array draws expand into their
 * `glBegin`/attribute/`glEnd` equivalents. Under GL_COMPILE_AND_EXECUTE the recorder
 * appends a command, then executes it with recording suspended, so nested recorded
 * calls record once.
 */

#include "gl_internal.h"

#ifdef OOPS_HOST_BUILD
#include <stdlib.h>
#else
/* A finished capture writes itself out, so this file needs the filesystem the swap hook
 * uses. */
#include "oops/fs.h"
#endif

/* The allocator differs by build: the SDK's own heap on the target, the C library's on
 * the host, as for buffer objects. */
void *gl_list_alloc(size_t bytes) {
    if (bytes == 0u)
        return (void *)0;
#ifdef OOPS_HOST_BUILD
    return malloc(bytes);
#else
    return oops_malloc(bytes);
#endif
}

static void gl_list_release(void *p) {
    if (!p)
        return;
#ifdef OOPS_HOST_BUILD
    free(p);
#else
    oops_free(p);
#endif
}

/* Whether a name is one this implementation can hold at all. Lists are named from 1;
 * zero is reserved by the specification and never a valid list. */
static GLboolean gl_list_nameable(GLuint list) {
    return (GLboolean)(list >= 1u && list <= GL_MAX_LISTS);
}

static gl_display_list_t *gl_list_slot(gl_context_t *ctx, GLuint list) {
    if (!gl_list_nameable(list))
        return (gl_display_list_t *)0;
    return &ctx->lists[list - 1u];
}

/* Empties a list and gives back everything it held. The slot keeps its name. */
static void gl_list_clear(gl_display_list_t *list) {
    if (!list)
        return;
    for (GLuint i = 0; i < list->count; i++)
        gl_list_release(list->cmds[i].data);
    gl_list_release(list->cmds);
    list->cmds = (gl_list_cmd_t *)0;
    list->count = 0u;
    list->capacity = 0u;
}

void gl_list_free_all(gl_context_t *ctx) {
    if (!ctx)
        return;
    for (GLuint i = 0; i < GL_MAX_LISTS; i++) {
        gl_list_clear(&ctx->lists[i]);
        ctx->lists[i].used = GL_FALSE;
        ctx->lists[i].compiled = GL_FALSE;
    }
    ctx->list_compiling = 0u;
    ctx->list_suspend = 0u;
    ctx->list_depth = 0u;
}

void *gl_list_pack_rows(const void *src, size_t row_bytes, size_t src_stride,
                        GLsizei rows) {
    if (!src || row_bytes == 0u || rows <= 0)
        return (void *)0;
    const size_t total = row_bytes * (size_t)rows;
    uint8_t *out = (total / (size_t)rows == row_bytes)
                       ? (uint8_t *)gl_list_alloc(total)
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
            pname == GL_POSITION)
            return 4;
        return (pname == GL_SPOT_DIRECTION) ? 3 : 1;
    case GL_LIST_OP_MATERIAL_FV:
    case GL_LIST_OP_MATERIAL_IV:
        if (pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR ||
            pname == GL_EMISSION || pname == GL_AMBIENT_AND_DIFFUSE)
            return 4;
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

/* -------------------------------------------------------------------------
 * Capture: the same calls, written to a stream for replay on the host's software
 * rasteriser, the reference the console path is compared against.
 *
 * The stream is flat and self-describing:
 *
 *     "OGLCAP" 0x00 0x01   magic and version
 *     u32 count            commands
 *     then, per command:
 *     u32 op, u32 a[10], u32 bytes, then `bytes` of blob padded to a multiple of four
 *
 * Little-endian, since both ends are x86-64. Arguments are written as the 32-bit words
 * `gl_list_arg_t` already holds, so a float survives exactly.
 */
#define GL_CAPTURE_MAGIC0 'O'
#define GL_CAPTURE_VERSION 1u
/* The header is 12 bytes: six of magic, one zero, one version, four of count. */
#define GL_CAPTURE_HEADER_BYTES 12u

static uint8_t *g_capture_buf;
static size_t g_capture_len;
static size_t g_capture_cap;
static uint32_t g_capture_count;
static GLboolean g_capture_overflow;

/* A ceiling, so a capture fails predictably. An array draw expands to about 144 bytes
 * per vertex, so a frame of a hundred thousand vertices is about 14 MB. */
#define GL_CAPTURE_MAX_BYTES (64u * 1024u * 1024u)

static void gl_capture_put(const void *src, size_t n) {
    if (g_capture_overflow)
        return;
    if (g_capture_len + n > g_capture_cap) {
        size_t grown = g_capture_cap ? g_capture_cap * 2u : 65536u;
        while (grown < g_capture_len + n)
            grown *= 2u;
        if (grown > GL_CAPTURE_MAX_BYTES) {
            g_capture_overflow = GL_TRUE;
            return;
        }
        uint8_t *next = (uint8_t *)gl_list_alloc(grown);
        if (!next) {
            /* Recorded, so a partial capture is never handed back as complete. */
            g_capture_overflow = GL_TRUE;
            return;
        }
        if (g_capture_len)
            memcpy(next, g_capture_buf, g_capture_len);
        gl_list_release(g_capture_buf);
        g_capture_buf = next;
        g_capture_cap = grown;
    }
    if (src)
        memcpy(g_capture_buf + g_capture_len, src, n);
    else
        memset(g_capture_buf + g_capture_len, 0, n);
    g_capture_len += n;
}

static void gl_capture_put_u32(uint32_t v) {
    gl_capture_put(&v, 4u);
}

/* One command, arguments and blob, at the end of the stream. */
static void gl_capture_append(gl_list_op_t op, const gl_list_arg_t *args, int nargs,
                              const void *blob, size_t bytes) {
    if (nargs < 0 || nargs > GL_LIST_MAX_ARGS)
        return;
    gl_capture_put_u32((uint32_t)op);
    for (int i = 0; i < GL_LIST_MAX_ARGS; i++) {
        uint32_t w = 0u;
        if (i < nargs)
            memcpy(&w, &args[i], 4u);
        gl_capture_put_u32(w);
    }
    const uint32_t n = (blob && bytes) ? (uint32_t)bytes : 0u;
    gl_capture_put_u32(n);
    if (n) {
        gl_capture_put(blob, n);
        /* Padded so every command starts word-aligned and a reader can walk the stream
           without knowing the ops. */
        const size_t pad = (4u - ((size_t)n & 3u)) & 3u;
        if (pad)
            gl_capture_put((const void *)0, pad);
    }
    g_capture_count++;
}

/* Appends one command, taking ownership of `owned`, and runs it if the list is also
 * executing. A list that cannot grow is an error, never a silent truncation. */
GLboolean gl_list_rec_owned(gl_list_op_t op, const gl_list_arg_t *args, int nargs,
                            void *owned, size_t bytes) {
    gl_context_t *ctx = gl_get_ctx();
    /* The capture sees the call before the list decides to swallow it, and whether or
       not a list is compiling. */
    if (ctx && ctx->capture_active && ctx->list_suspend == 0u) {
        gl_capture_append(op, args, nargs, owned, bytes);
    }
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
        gl_list_cmd_t *cmds =
            (gl_list_cmd_t *)gl_list_alloc((size_t)grown * sizeof(gl_list_cmd_t));
        if (!cmds || grown < list->capacity) {
            gl_list_release(cmds);
            gl_list_release(owned);
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            return (GLboolean)(ctx->list_mode == GL_COMPILE);
        }
        if (list->count)
            memcpy(cmds, list->cmds, (size_t)list->count * sizeof(gl_list_cmd_t));
        gl_list_release(list->cmds);
        list->cmds = cmds;
        list->capacity = grown;
    }

    gl_list_cmd_t *cmd = &list->cmds[list->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->op = op;
    for (int i = 0; i < nargs; i++)
        cmd->a[i] = args[i];
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
    return gl_list_rec_owned(op, args, nargs, copy, copy ? bytes : 0u);
}

GLuint glGenLists(GLsizei range) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return 0u;
    if (range < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return 0u;
    }
    if (range == 0)
        return 0u;

    /* A contiguous run, as the specification requires - a caller is entitled to treat
     * the answer as a base and add to it. */
    for (GLuint start = 1u; start + (GLuint)range - 1u <= GL_MAX_LISTS; start++) {
        GLboolean clear = GL_TRUE;
        for (GLsizei i = 0; i < range; i++) {
            if (ctx->lists[start + (GLuint)i - 1u].used) {
                clear = GL_FALSE;
                break;
            }
        }
        if (!clear)
            continue;
        for (GLsizei i = 0; i < range; i++) {
            gl_display_list_t *list = &ctx->lists[start + (GLuint)i - 1u];
            gl_list_clear(list);
            list->used = GL_TRUE;
            list->compiled = GL_FALSE;
        }
        return start;
    }
    /* No run that long. Zero is the specification's answer and means "none allocated";
     * it is not an error, so glGetError stays clear and a caller that checks the return
     * finds out. */
    return 0u;
}

GLboolean glIsList(GLuint list) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return GL_FALSE;
    gl_display_list_t *slot = gl_list_slot(ctx, list);
    return (GLboolean)(slot && slot->used && slot->compiled);
}

void glDeleteLists(GLuint list, GLsizei range) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (range < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (GLsizei i = 0; i < range; i++) {
        const GLuint name = list + (GLuint)i;
        gl_display_list_t *slot = gl_list_slot(ctx, name);
        if (!slot)
            continue; /* names outside the range were never ours; silently ignored */
        /* Not the one being recorded into: its storage is still being appended to, and
         * the specification leaves deleting it undefined. It is released when it is
         * next named. */
        if (name == ctx->list_compiling)
            continue;
        gl_list_clear(slot);
        slot->used = GL_FALSE;
        slot->compiled = GL_FALSE;
    }
}

void glNewList(GLuint list, GLenum mode) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
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
    if (!ctx)
        return;
    if (ctx->list_compiling == 0u) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    gl_display_list_t *slot = gl_list_slot(ctx, ctx->list_compiling);
    if (slot)
        slot->compiled = GL_TRUE;
    ctx->list_compiling = 0u;
}

void glListBase(GLuint base) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_LIST_BASE, gl_la_u(base)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    ctx->list_base = base;
}

/* Runs one recorded command. Pixel-carrying commands run under default unpacking,
 * because their data was packed tight when it was recorded. */
static void gl_list_execute_cmd(gl_context_t *ctx, const gl_list_cmd_t *cmd) {
    const gl_list_arg_t *a = cmd->a;
    const GLfloat v4f[4] = {a[2].f, a[3].f, a[4].f, a[5].f};
    const GLint v4i[4] = {a[2].i, a[3].i, a[4].i, a[5].i};
    const GLfloat v4f1[4] = {a[1].f, a[2].f, a[3].f, a[4].f};
    const GLint v4i1[4] = {a[1].i, a[2].i, a[3].i, a[4].i};

    switch (cmd->op) {
    case GL_LIST_OP_BEGIN:
        glBegin(a[0].e);
        break;
    case GL_LIST_OP_END:
        glEnd();
        break;
    case GL_LIST_OP_VERTEX:
        glVertex4f(a[0].f, a[1].f, a[2].f, a[3].f);
        break;
    case GL_LIST_OP_COLOR:
        glColor4f(a[0].f, a[1].f, a[2].f, a[3].f);
        break;
    case GL_LIST_OP_SECONDARY_COLOR:
        glSecondaryColor3f(a[0].f, a[1].f, a[2].f);
        break;
    case GL_LIST_OP_FOG_COORD:
        glFogCoordf(a[0].f);
        break;
    case GL_LIST_OP_NORMAL:
        glNormal3f(a[0].f, a[1].f, a[2].f);
        break;
    case GL_LIST_OP_TEXCOORD:
        glTexCoord4f(a[0].f, a[1].f, a[2].f, a[3].f);
        break;
    case GL_LIST_OP_ENABLE:
        glEnable(a[0].e);
        break;
    case GL_LIST_OP_DISABLE:
        glDisable(a[0].e);
        break;
    case GL_LIST_OP_MATRIX_MODE:
        glMatrixMode(a[0].e);
        break;
    case GL_LIST_OP_LOAD_IDENTITY:
        glLoadIdentity();
        break;
    case GL_LIST_OP_PUSH_MATRIX:
        glPushMatrix();
        break;
    case GL_LIST_OP_POP_MATRIX:
        glPopMatrix();
        break;
    case GL_LIST_OP_TRANSLATE:
        glTranslatef(a[0].f, a[1].f, a[2].f);
        break;
    case GL_LIST_OP_ROTATE:
        glRotatef(a[0].f, a[1].f, a[2].f, a[3].f);
        break;
    case GL_LIST_OP_SCALE:
        glScalef(a[0].f, a[1].f, a[2].f);
        break;
    case GL_LIST_OP_LOAD_MATRIX:
        if (cmd->data)
            glLoadMatrixf((const GLfloat *)cmd->data);
        break;
    case GL_LIST_OP_MULT_MATRIX:
        if (cmd->data)
            glMultMatrixf((const GLfloat *)cmd->data);
        break;
    case GL_LIST_OP_ORTHO:
        glOrtho(a[0].f, a[1].f, a[2].f, a[3].f, a[4].f, a[5].f);
        break;
    case GL_LIST_OP_FRUSTUM:
        glFrustum(a[0].f, a[1].f, a[2].f, a[3].f, a[4].f, a[5].f);
        break;
    case GL_LIST_OP_BIND_TEXTURE:
        glBindTexture(a[0].e, a[1].u);
        break;
    case GL_LIST_OP_SHADE_MODEL:
        glShadeModel(a[0].e);
        break;
    case GL_LIST_OP_CULL_FACE:
        glCullFace(a[0].e);
        break;
    case GL_LIST_OP_FRONT_FACE:
        glFrontFace(a[0].e);
        break;
    case GL_LIST_OP_DEPTH_FUNC:
        glDepthFunc(a[0].e);
        break;
    case GL_LIST_OP_BLEND_FUNC:
        glBlendFunc(a[0].e, a[1].e);
        break;
    case GL_LIST_OP_BLEND_FUNC_SEPARATE:
        glBlendFuncSeparate(a[0].e, a[1].e, a[2].e, a[3].e);
        break;
    case GL_LIST_OP_BLEND_EQUATION:
        glBlendEquation(a[0].e);
        break;
    case GL_LIST_OP_BLEND_COLOR:
        glBlendColor(a[0].f, a[1].f, a[2].f, a[3].f);
        break;
    case GL_LIST_OP_LOGIC_OP:
        glLogicOp(a[0].e);
        break;
    case GL_LIST_OP_ALPHA_FUNC:
        glAlphaFunc(a[0].e, a[1].f);
        break;
    case GL_LIST_OP_DEPTH_MASK:
        glDepthMask((GLboolean)a[0].u);
        break;
    case GL_LIST_OP_DEPTH_RANGE:
        glDepthRange(a[0].f, a[1].f);
        break;
    case GL_LIST_OP_COLOR_MASK:
        glColorMask((GLboolean)a[0].u, (GLboolean)a[1].u, (GLboolean)a[2].u,
                    (GLboolean)a[3].u);
        break;
    case GL_LIST_OP_CLEAR:
        glClear(a[0].u);
        break;
    case GL_LIST_OP_CLEAR_COLOR:
        glClearColor(a[0].f, a[1].f, a[2].f, a[3].f);
        break;
    case GL_LIST_OP_CLEAR_DEPTH:
        glClearDepth(a[0].f);
        break;
    case GL_LIST_OP_CLEAR_STENCIL:
        glClearStencil(a[0].i);
        break;
    case GL_LIST_OP_STENCIL_FUNC:
        glStencilFunc(a[0].e, a[1].i, a[2].u);
        break;
    case GL_LIST_OP_STENCIL_OP:
        glStencilOp(a[0].e, a[1].e, a[2].e);
        break;
    case GL_LIST_OP_STENCIL_MASK:
        glStencilMask(a[0].u);
        break;
    case GL_LIST_OP_CLIP_PLANE: {
        const GLdouble eq[4] = {a[1].f, a[2].f, a[3].f, a[4].f};
        glClipPlane(a[0].e, eq);
        break;
    }
    case GL_LIST_OP_COLOR_MATERIAL:
        glColorMaterial(a[0].e, a[1].e);
        break;
    case GL_LIST_OP_LIGHT_FV:
        glLightfv(a[0].e, a[1].e, v4f);
        break;
    case GL_LIST_OP_LIGHT_IV:
        glLightiv(a[0].e, a[1].e, v4i);
        break;
    case GL_LIST_OP_MATERIAL_FV:
        glMaterialfv(a[0].e, a[1].e, v4f);
        break;
    case GL_LIST_OP_MATERIAL_IV:
        glMaterialiv(a[0].e, a[1].e, v4i);
        break;
    case GL_LIST_OP_LIGHT_MODEL_FV:
        glLightModelfv(a[0].e, v4f1);
        break;
    case GL_LIST_OP_LIGHT_MODEL_IV:
        glLightModeliv(a[0].e, v4i1);
        break;
    case GL_LIST_OP_FOG_F:
        glFogf(a[0].e, a[1].f);
        break;
    case GL_LIST_OP_FOG_I:
        glFogi(a[0].e, a[1].i);
        break;
    case GL_LIST_OP_FOG_FV:
        glFogfv(a[0].e, v4f1);
        break;
    case GL_LIST_OP_FOG_IV:
        glFogiv(a[0].e, v4i1);
        break;
    case GL_LIST_OP_TEX_ENV_I:
        glTexEnvi(a[0].e, a[1].e, a[2].i);
        break;
    case GL_LIST_OP_TEX_ENV_FV:
        glTexEnvfv(a[0].e, a[1].e, v4f);
        break;
    case GL_LIST_OP_TEX_ENV_IV:
        glTexEnviv(a[0].e, a[1].e, v4i);
        break;
    case GL_LIST_OP_TEX_GEN_I:
        glTexGeni(a[0].e, a[1].e, a[2].i);
        break;
    case GL_LIST_OP_TEX_GEN_FV:
        glTexGenfv(a[0].e, a[1].e, v4f);
        break;
    case GL_LIST_OP_TEX_GEN_IV:
        glTexGeniv(a[0].e, a[1].e, v4i);
        break;
    case GL_LIST_OP_TEX_PARAMETER_I:
        glTexParameteri(a[0].e, a[1].e, a[2].i);
        break;
    case GL_LIST_OP_TEX_PARAMETER_F:
        glTexParameterf(a[0].e, a[1].e, a[2].f);
        break;
    case GL_LIST_OP_TEX_PARAMETER_FV:
        glTexParameterfv(a[0].e, a[1].e, v4f);
        break;
    case GL_LIST_OP_TEX_PARAMETER_IV:
        glTexParameteriv(a[0].e, a[1].e, v4i);
        break;
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
         * significant bit first: alignment 1, no row length, skips or swapping, a
         * volume's slices back to back. */
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
            glTexImage1D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].e, a[6].e,
                         cmd->data);
            break;
        case GL_LIST_OP_TEX_IMAGE_2D:
            glTexImage2D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i, a[6].e, a[7].e,
                         cmd->data);
            break;
        case GL_LIST_OP_TEX_SUB_IMAGE_1D:
            glTexSubImage1D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].e, a[5].e, cmd->data);
            break;
        case GL_LIST_OP_TEX_SUB_IMAGE_2D:
            glTexSubImage2D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i, a[6].e,
                            a[7].e, cmd->data);
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
        glCopyTexSubImage3D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i, a[6].i,
                            a[7].i, a[8].i);
        break;
    case GL_LIST_OP_COPY_TEX_IMAGE_1D:
        glCopyTexImage1D(a[0].e, a[1].i, a[2].e, a[3].i, a[4].i, a[5].i, a[6].i);
        break;
    case GL_LIST_OP_COPY_TEX_IMAGE_2D:
        glCopyTexImage2D(a[0].e, a[1].i, a[2].e, a[3].i, a[4].i, a[5].i, a[6].i,
                         a[7].i);
        break;
    case GL_LIST_OP_COPY_TEX_SUB_IMAGE_1D:
        glCopyTexSubImage1D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i);
        break;
    case GL_LIST_OP_COPY_TEX_SUB_IMAGE_2D:
        glCopyTexSubImage2D(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i, a[5].i, a[6].i,
                            a[7].i);
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
    case GL_LIST_OP_ACTIVE_TEXTURE:
        glActiveTexture(a[0].e);
        break;
    case GL_LIST_OP_MULTI_TEXCOORD:
        glMultiTexCoord4f(a[0].e, a[1].f, a[2].f, a[3].f, a[4].f);
        break;
    case GL_LIST_OP_POINT_SIZE:
        glPointSize(a[0].f);
        break;
    case GL_LIST_OP_BEGIN_QUERY:
        glBeginQuery(a[0].e, a[1].u);
        break;
    case GL_LIST_OP_END_QUERY:
        glEndQuery(a[0].e);
        break;
    case GL_LIST_OP_POINT_PARAMETER: {
        const GLfloat p[3] = {a[1].f, a[2].f, a[3].f};
        glPointParameterfv(a[0].e, p);
        break;
    }
    case GL_LIST_OP_LINE_WIDTH:
        glLineWidth(a[0].f);
        break;
    case GL_LIST_OP_POLYGON_OFFSET:
        glPolygonOffset(a[0].f, a[1].f);
        break;
    case GL_LIST_OP_SCISSOR:
        glScissor(a[0].i, a[1].i, a[2].i, a[3].i);
        break;
    case GL_LIST_OP_VIEWPORT:
        glViewport(a[0].i, a[1].i, a[2].i, a[3].i);
        break;
    case GL_LIST_OP_HINT:
        glHint(a[0].e, a[1].e);
        break;
    case GL_LIST_OP_DRAW_BUFFER:
        glDrawBuffer(a[0].e);
        break;
    case GL_LIST_OP_READ_BUFFER:
        glReadBuffer(a[0].e);
        break;
    case GL_LIST_OP_PIXEL_ZOOM:
        glPixelZoom(a[0].f, a[1].f);
        break;
    case GL_LIST_OP_RASTER_POS:
        glRasterPos4f(a[0].f, a[1].f, a[2].f, a[3].f);
        break;
    case GL_LIST_OP_WINDOW_POS:
        glWindowPos3f(a[0].f, a[1].f, a[2].f);
        break;
    case GL_LIST_OP_COPY_PIXELS:
        glCopyPixels(a[0].i, a[1].i, a[2].i, a[3].i, a[4].e);
        break;
    case GL_LIST_OP_PUSH_ATTRIB:
        glPushAttrib(a[0].u);
        break;
    case GL_LIST_OP_POP_ATTRIB:
        glPopAttrib();
        break;
    case GL_LIST_OP_LIST_BASE:
        glListBase(a[0].u);
        break;
    case GL_LIST_OP_CALL_LIST:
        glCallList(a[0].u);
        break;
    case GL_LIST_OP_POLYGON_MODE:
        glPolygonMode(a[0].e, a[1].e);
        break;
    case GL_LIST_OP_EDGE_FLAG:
        glEdgeFlag((GLboolean)a[0].u);
        break;
    case GL_LIST_OP_INDEX:
        glIndexf(a[0].f);
        break;
    case GL_LIST_OP_CLEAR_INDEX:
        glClearIndex(a[0].f);
        break;
    case GL_LIST_OP_INDEX_MASK:
        glIndexMask(a[0].u);
        break;
    case GL_LIST_OP_SAMPLE_COVERAGE:
        glSampleCoverage(a[0].f, (GLboolean)a[1].u);
        break;
    case GL_LIST_OP_MAP1:
    case GL_LIST_OP_MAP2:
        gl_eval_replay_map(cmd);
        break;
    case GL_LIST_OP_MAP_GRID1:
        glMapGrid1f(a[0].i, a[1].f, a[2].f);
        break;
    case GL_LIST_OP_MAP_GRID2:
        glMapGrid2f(a[0].i, a[1].f, a[2].f, a[3].i, a[4].f, a[5].f);
        break;
    case GL_LIST_OP_EVAL_COORD1:
        glEvalCoord1f(a[0].f);
        break;
    case GL_LIST_OP_EVAL_COORD2:
        glEvalCoord2f(a[0].f, a[1].f);
        break;
    case GL_LIST_OP_EVAL_POINT1:
        glEvalPoint1(a[0].i);
        break;
    case GL_LIST_OP_EVAL_POINT2:
        glEvalPoint2(a[0].i, a[1].i);
        break;
    case GL_LIST_OP_EVAL_MESH1:
        glEvalMesh1(a[0].e, a[1].i, a[2].i);
        break;
    case GL_LIST_OP_EVAL_MESH2:
        glEvalMesh2(a[0].e, a[1].i, a[2].i, a[3].i, a[4].i);
        break;
    case GL_LIST_OP_INIT_NAMES:
        glInitNames();
        break;
    case GL_LIST_OP_LOAD_NAME:
        glLoadName(a[0].u);
        break;
    case GL_LIST_OP_PUSH_NAME:
        glPushName(a[0].u);
        break;
    case GL_LIST_OP_POP_NAME:
        glPopName();
        break;
    case GL_LIST_OP_PASS_THROUGH:
        glPassThrough(a[0].f);
        break;
    case GL_LIST_OP_PIXEL_TRANSFER:
        glPixelTransferf(a[0].e, a[1].f);
        break;
    case GL_LIST_OP_LINE_STIPPLE:
        glLineStipple(a[0].i, (GLushort)a[1].u);
        break;
    case GL_LIST_OP_ACCUM:
        glAccum(a[0].e, a[1].f);
        break;
    case GL_LIST_OP_CLEAR_ACCUM:
        glClearAccum(a[0].f, a[1].f, a[2].f, a[3].f);
        break;
    case GL_LIST_OP_PIXEL_MAP:
        glPixelMapfv(a[0].e, a[1].i, (const GLfloat *)cmd->data);
        break;
    case GL_LIST_OP_CALL_LISTS:
        /* The names were widened to GLuint when compiled; the base is the one current
         * now, which is what the specification says a compiled glCallLists uses. */
        if (cmd->data)
            glCallLists(a[0].i, GL_UNSIGNED_INT, cmd->data);
        break;
    default:
        break;
    }
}

/* Replays one list.
 *
 * Calls the public entry points rather than reaching into the context, so a list does
 * exactly what the program would have done - including passing through whatever
 * validation those entry points do, which is where a compiled call's errors are raised:
 * when it runs, as the specification has it, not when it was recorded. */
static void gl_list_execute(gl_context_t *ctx, const gl_display_list_t *list) {
    for (GLuint i = 0; i < list->count; i++) {
        gl_list_execute_cmd(ctx, &list->cmds[i]);
    }
}

void glCallList(GLuint list) {
    /* Compiling one call of another is legal and is recorded, not followed - so a list
     * may call a list that does not exist yet, which the specification allows. */
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_CALL_LIST, gl_la_u(list)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;

    gl_display_list_t *slot = gl_list_slot(ctx, list);
    /* Calling an undefined list is ignored, not an error - the specification is
     * explicit, and it is what lets a list reference one compiled later. */
    if (!slot || !slot->used || !slot->compiled)
        return;

    if (ctx->list_depth >= GL_MAX_LIST_DEPTH) {
        /* A list that reaches itself. Bounded rather than detected exactly: the
         * specification leaves the limit implementation-defined, and a depth cap stops
         * the stack overflow a self-calling list would otherwise cause. */
        gl_record_error(ctx, GL_STACK_OVERFLOW);
        return;
    }
    ctx->list_depth++;
    gl_list_execute(ctx, slot);
    ctx->list_depth--;
}

/* Name `i` of glCallLists' array, as the offset from the list base it stands for -
 * every type GL 1.0 gives the call, decoded as Mesa decodes them
 * (main/dlist.c:13510-13575): the signed and unsigned integers as themselves, a float
 * truncated, and GL_2_BYTES to GL_4_BYTES as big-endian byte strings. */
static GLint gl_call_lists_name(GLenum type, const GLvoid *lists, GLsizei i) {
    const GLubyte *ub = (const GLubyte *)lists;
    const size_t k = (size_t)i;
    switch (type) {
    case GL_BYTE:
        return (GLint)((const GLbyte *)lists)[k];
    case GL_UNSIGNED_BYTE:
        return (GLint)ub[k];
    case GL_SHORT:
        return (GLint)((const GLshort *)lists)[k];
    case GL_UNSIGNED_SHORT:
        return (GLint)((const GLushort *)lists)[k];
    case GL_INT:
        return ((const GLint *)lists)[k];
    case GL_FLOAT:
        return (GLint)((const GLfloat *)lists)[k];
    case GL_2_BYTES:
        return (GLint)ub[2 * k] * 256 + (GLint)ub[2 * k + 1];
    case GL_3_BYTES:
        return (GLint)ub[3 * k] * 65536 + (GLint)ub[3 * k + 1] * 256 +
               (GLint)ub[3 * k + 2];
    case GL_4_BYTES:
        return (GLint)(((GLuint)ub[4 * k] << 24) | ((GLuint)ub[4 * k + 1] << 16) |
                       ((GLuint)ub[4 * k + 2] << 8) | (GLuint)ub[4 * k + 3]);
    default:
        return (GLint)((const GLuint *)lists)[k]; /* GL_UNSIGNED_INT */
    }
}

void glCallLists(GLsizei n, GLenum type, const GLvoid *lists) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    /* The type first, as Mesa checks it (main/dlist.c:13481): GL_BYTE through
     * GL_4_BYTES. */
    if (type < GL_BYTE || type > GL_4_BYTES) {
        /* Checked before any read: an unrecognised type read as the widest would run
         * off the end of a byte-sized array. */
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!lists || n == 0)
        return;

    /* Compiled as the names, not as the lists they name: the specification applies
     * the list base current when the list runs, not when it is compiled. */
    if (gl_list_recording()) {
        GLuint *names = (GLuint *)gl_list_alloc((size_t)n * sizeof(GLuint));
        if (!names) {
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            if (ctx->list_mode == GL_COMPILE)
                return;
        } else {
            /* Stored as offsets in GLuint - a negative one wraps, and wraps back when
             * the base is added, as the unsigned sum below does. */
            for (GLsizei i = 0; i < n; i++)
                names[i] = (GLuint)gl_call_lists_name(type, lists, i);
            if (gl_list_rec_owned(GL_LIST_OP_CALL_LISTS, GL_LIST_ARGV(gl_la_i(n)), 1,
                                  names, (size_t)n * sizeof(GLuint))) {
                return;
            }
        }
    }

    for (GLsizei i = 0; i < n; i++) {
        glCallList(ctx->list_base + (GLuint)gl_call_lists_name(type, lists, i));
    }
}

/* -------------------------------------------------------------------------
 * Capture: the public half
 */

/*
 * Every live 2D texture, written into the stream before the frame, because a frame
 * rarely uploads the textures it samples. Each is a bind, the base image and the
 * sampler parameters. The texels are the SDK's RGBA8 host copy, so the image goes back
 * in as `GL_RGBA`/`GL_UNSIGNED_BYTE` under the program's internal format. Appended
 * without executing, so arming a capture does not perturb the frame. Only the base
 * level is emitted; the other levels change how a minified sample looks, not which
 * texture it is.
 */
static void gl_capture_emit_texture_state(gl_context_t *ctx) {
    if (!ctx)
        return;
    GLuint restore = 0u;
    for (GLuint i = 0u; i < (GLuint)OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
        const gl_texture_object_t *t = &ctx->textures[i];
        if (!t->used || !t->pixels)
            continue;
        if (t->width <= 0 || t->height <= 0)
            continue;
        /* 2D only: a cube map's faces and a volume's slices each want their own target
           and their own emission. */
        if (t->target != GL_TEXTURE_2D)
            continue;

        gl_list_arg_t bind[2] = {gl_la_e(GL_TEXTURE_2D), gl_la_u(t->id)};
        gl_capture_append(GL_LIST_OP_BIND_TEXTURE, bind, 2, (const void *)0, 0u);
        restore = t->id;

        gl_list_arg_t img[8] = {
            gl_la_e(GL_TEXTURE_2D),
            gl_la_i(0),
            gl_la_i(t->internal_format),
            gl_la_i((GLint)t->width),
            gl_la_i((GLint)t->height),
            gl_la_i(0),
            gl_la_e(GL_RGBA),
            gl_la_e(GL_UNSIGNED_BYTE),
        };
        gl_capture_append(GL_LIST_OP_TEX_IMAGE_2D, img, 8, t->pixels,
                          (size_t)t->width * (size_t)t->height * 4u);

        /* The sampler state, since filter and wrap change every sample off a texel
           centre. */
        static const GLenum pnames[4] = {GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER,
                                         GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T};
        const GLenum pvals[4] = {t->min_filter, t->mag_filter, t->wrap_s, t->wrap_t};
        for (int p = 0; p < 4; p++) {
            if (pvals[p] == 0u)
                continue;
            gl_list_arg_t pa[3] = {gl_la_e(GL_TEXTURE_2D), gl_la_e(pnames[p]),
                                   gl_la_i((GLint)pvals[p])};
            gl_capture_append(GL_LIST_OP_TEX_PARAMETER_I, pa, 3, (const void *)0, 0u);
        }
    }
    /* Leave the binding where the frame expects to find it rather than on whichever
       texture happened to be last in the table. */
    if (restore != 0u) {
        gl_list_arg_t bind[2] = {gl_la_e(GL_TEXTURE_2D),
                                 gl_la_u(ctx->tex_unit[0].bound_texture_2d)};
        gl_capture_append(GL_LIST_OP_BIND_TEXTURE, bind, 2, (const void *)0, 0u);
    }
}

void oops_gl_capture_begin(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    /* A second begin discards the first rather than appending to it: two captures
       joined end to end would replay as one impossible program. */
    gl_list_release(g_capture_buf);
    g_capture_buf = (uint8_t *)0;
    g_capture_len = 0u;
    g_capture_cap = 0u;
    g_capture_count = 0u;
    g_capture_overflow = GL_FALSE;
    /* Room for the header, filled in by `oops_gl_capture_end` once the count is known.
     */
    gl_capture_put((const void *)0, GL_CAPTURE_HEADER_BYTES);
    /* Before `capture_active`, so these are the stream's first commands and nothing the
       frame does can land in front of them. */
    gl_capture_emit_texture_state(ctx);
    ctx->capture_active = GL_TRUE;
}

void oops_gl_capture_end(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (ctx)
        ctx->capture_active = GL_FALSE;
    if (!g_capture_buf || g_capture_len < GL_CAPTURE_HEADER_BYTES)
        return;
    static const char magic[6] = {'O', 'G', 'L', 'C', 'A', 'P'};
    memcpy(g_capture_buf, magic, sizeof(magic));
    g_capture_buf[6] = 0;
    g_capture_buf[7] = (uint8_t)GL_CAPTURE_VERSION;
    const uint32_t count = g_capture_count;
    memcpy(g_capture_buf + 8, &count, 4u);
}

const void *oops_gl_capture_data(size_t *out_bytes, unsigned *out_calls) {
    if (out_calls)
        *out_calls = g_capture_count;
    /* An overflowed capture hands back nothing: half a call stream replays as a
       different program. */
    if (g_capture_overflow) {
        if (out_bytes)
            *out_bytes = 0u;
        return (const void *)0;
    }
    if (out_bytes)
        *out_bytes = g_capture_len;
    return g_capture_buf;
}

unsigned oops_gl_capture_replay(const void *data, size_t bytes) {
    gl_context_t *ctx = gl_get_ctx();
    const uint8_t *p = (const uint8_t *)data;
    if (!ctx || !p || bytes < GL_CAPTURE_HEADER_BYTES)
        return 0u;
    if (p[0] != 'O' || p[1] != 'G' || p[2] != 'L' || p[3] != 'C' || p[4] != 'A' ||
        p[5] != 'P') {
        return 0u;
    }
    if (p[7] != (uint8_t)GL_CAPTURE_VERSION)
        return 0u;
    uint32_t count = 0u;
    memcpy(&count, p + 8, 4u);

    size_t off = GL_CAPTURE_HEADER_BYTES;
    unsigned done = 0u;
    for (uint32_t i = 0u; i < count; i++) {
        /* op + ten arguments + the blob's length. */
        const size_t fixed = 4u + (size_t)GL_LIST_MAX_ARGS * 4u + 4u;
        if (off + fixed > bytes)
            break;
        gl_list_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        uint32_t w = 0u;
        memcpy(&w, p + off, 4u);
        off += 4u;
        cmd.op = (gl_list_op_t)w;
        for (int k = 0; k < GL_LIST_MAX_ARGS; k++) {
            memcpy(&w, p + off, 4u);
            off += 4u;
            memcpy(&cmd.a[k], &w, 4u);
        }
        uint32_t blob = 0u;
        memcpy(&blob, p + off, 4u);
        off += 4u;
        if (blob) {
            if (off + blob > bytes)
                break;
            /* Pointed into the stream, not copied: the executor only reads it, and the
               caller owns the buffer for the length of the replay. */
            cmd.data = (void *)(uintptr_t)(p + off);
            off += blob;
            off += (4u - ((size_t)blob & 3u)) & 3u;
        }
        /* The same executor a display list uses, so a capture cannot drift from a list.
         */
        gl_list_execute_cmd(ctx, &cmd);
        done++;
    }
    return done;
}

/* -------------------------------------------------------------------------
 * Capturing a frame without the program's help
 *
 * The swap calls `oops_gl_capture_begin`/`end`, so a program is captured unmodified:
 * arm a frame number and a path, and the frame after that number completes is recorded
 * and written out.
 */
static uint32_t g_capture_arm_frame;
static const char *g_capture_path;

void oops_gl_capture_frame(unsigned frame, const char *path) {
    g_capture_arm_frame = (uint32_t)frame;
    g_capture_path = path;
}

void gl_capture_swap_tick(gl_context_t *ctx) {
    if (!ctx)
        return;
    if (ctx->capture_active) {
        oops_gl_capture_end();
        size_t bytes = 0u;
        unsigned calls = 0u;
        const void *data = oops_gl_capture_data(&bytes, &calls);
        {
            /* One line: calls recorded, bytes, and the write's result. */
            char msg[128];
            size_t n = 0;
            const char *head = "capture: calls";
            while (head[n] && n < sizeof(msg) - 80) {
                msg[n] = head[n];
                n++;
            }
            n = gl_msg_hex(msg, sizeof(msg), n, (uint32_t)calls);
            const char *mid = " bytes";
            size_t k = 0;
            while (mid[k] && n < sizeof(msg) - 48) {
                msg[n++] = mid[k++];
            }
            n = gl_msg_hex(msg, sizeof(msg), n, (uint32_t)bytes);
            int rc = 0;
            if (data && bytes && g_capture_path) {
#ifndef OOPS_HOST_BUILD
                rc = oops_fs_write_all(g_capture_path, data, bytes);
#endif
                const char *tail = " write-rc";
                size_t j = 0;
                while (tail[j] && n < sizeof(msg) - 14) {
                    msg[n++] = tail[j++];
                }
                n = gl_msg_hex(msg, sizeof(msg), n, (uint32_t)rc);
            }
            msg[n] = '\0';
            oops_log_info("GL", "%s", msg);
        }
        /* Each reason for writing nothing from the state that decided it. */
        if (g_capture_overflow) {
            oops_log_info("GL",
                          "capture overflowed and was discarded - nothing written");
        } else if (!g_capture_path) {
            oops_log_info("GL", "capture had no path armed - nothing written");
        } else if (!data || !bytes) {
            oops_log_info("GL", "capture recorded nothing - nothing written");
        }
        g_capture_arm_frame = 0u;
        g_capture_path = (const char *)0;
        return;
    }
    if (g_capture_arm_frame != 0u && ctx->frame_count == g_capture_arm_frame) {
        oops_gl_capture_begin();
    }
}
