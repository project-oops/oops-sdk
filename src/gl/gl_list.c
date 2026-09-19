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
 * # What is not recorded, and why that is an error rather than a silence
 *
 * The specification splits GL calls in two: most are compiled into a list, and a named few
 * always execute immediately because they return something - glGenLists, glIsList, every
 * glGet*, glIsEnabled. Those are handled by simply not having a capture hook, so they run as
 * they always did.
 *
 * A third group is neither: calls this subset can make, that a list cannot yet carry - the
 * vertex-array draws, whose semantics require dereferencing client pointers at *compile* time
 * and copying what they hold. Compiling one is refused with GL_INVALID_OPERATION rather than
 * dropped, so a program using them inside a list finds out. Storing the pointer and reading it
 * at execute time would be the quiet wrong answer: it reads whatever the array holds later,
 * which is a different picture from the one the program compiled.
 */

#include "gl_internal.h"

/* Whether a name is one this implementation can hold at all. Lists are named from 1; zero is
 * reserved by the specification and never a valid list. */
static GLboolean gl_list_nameable(GLuint list) {
    return (GLboolean)(list >= 1u && list <= GL_MAX_LISTS);
}

static gl_display_list_t *gl_list_slot(gl_context_t *ctx, GLuint list) {
    if (!gl_list_nameable(list)) return (gl_display_list_t *)0;
    return &ctx->lists[list - 1u];
}

/* Appends one command to the list being compiled.
 *
 * Returns whether the caller should return immediately - true under GL_COMPILE, false under
 * GL_COMPILE_AND_EXECUTE (where the call is both recorded and performed) and false when no list
 * is being compiled at all, which is the ordinary path.
 *
 * **A full list is an error, not a truncation.** Dropping commands would give a list that draws
 * part of what was compiled into it, which looks like a modelling mistake rather than a limit
 * being hit and is the kind of wrong answer this subsystem refuses to produce.
 */
GLboolean gl_list_capture(gl_list_op_t op, GLenum e0, GLenum e1, GLuint u0, const GLfloat *f) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || ctx->list_compiling == 0u) return GL_FALSE;

    gl_display_list_t *list = gl_list_slot(ctx, ctx->list_compiling);
    if (!list) return GL_FALSE;

    if (list->count >= GL_MAX_LIST_COMMANDS) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return (GLboolean)(ctx->list_mode == GL_COMPILE);
    }

    gl_list_cmd_t *cmd = &list->cmds[list->count++];
    cmd->op = op;
    cmd->e0 = e0;
    cmd->e1 = e1;
    cmd->u0 = u0;
    for (int i = 0; i < 4; i++) cmd->f[i] = f ? f[i] : 0.0f;

    return (GLboolean)(ctx->list_mode == GL_COMPILE);
}

/* Refuses a call that cannot be compiled, when one is being compiled.
 *
 * Returns whether the caller should return immediately. Separate from `gl_list_capture` because
 * the answer is different: a refused call is not performed either, since performing it under
 * GL_COMPILE would draw something the program expected to be deferred. */
GLboolean gl_list_refuse(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || ctx->list_compiling == 0u) return GL_FALSE;
    gl_record_error(ctx, GL_INVALID_OPERATION);
    return GL_TRUE;
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
            list->used = GL_TRUE;
            list->compiled = GL_FALSE;
            list->count = 0u;
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
        gl_display_list_t *slot = gl_list_slot(ctx, list + (GLuint)i);
        if (!slot) continue; /* names outside the range were never ours; silently ignored */
        slot->used = GL_FALSE;
        slot->compiled = GL_FALSE;
        slot->count = 0u;
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
    slot->used = GL_TRUE;
    slot->compiled = GL_FALSE; /* not callable until glEndList closes it */
    slot->count = 0u;
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
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->list_base = base;
}

/* Replays one list.
 *
 * Calls the public entry points rather than reaching into the context, so a list does exactly
 * what the program would have done - including passing through whatever validation those
 * entry points do. */
static void gl_list_execute(const gl_display_list_t *list) {
    for (GLuint i = 0; i < list->count; i++) {
        const gl_list_cmd_t *cmd = &list->cmds[i];
        const GLfloat *f = cmd->f;
        switch (cmd->op) {
            case GL_LIST_OP_BEGIN:         glBegin(cmd->e0); break;
            case GL_LIST_OP_END:           glEnd(); break;
            case GL_LIST_OP_VERTEX:        glVertex4f(f[0], f[1], f[2], f[3]); break;
            case GL_LIST_OP_COLOR:         glColor4f(f[0], f[1], f[2], f[3]); break;
            case GL_LIST_OP_NORMAL:        glNormal3f(f[0], f[1], f[2]); break;
            case GL_LIST_OP_TEXCOORD:      glTexCoord4f(f[0], f[1], f[2], f[3]); break;
            case GL_LIST_OP_ENABLE:        glEnable(cmd->e0); break;
            case GL_LIST_OP_DISABLE:       glDisable(cmd->e0); break;
            case GL_LIST_OP_MATRIX_MODE:   glMatrixMode(cmd->e0); break;
            case GL_LIST_OP_LOAD_IDENTITY: glLoadIdentity(); break;
            case GL_LIST_OP_PUSH_MATRIX:   glPushMatrix(); break;
            case GL_LIST_OP_POP_MATRIX:    glPopMatrix(); break;
            case GL_LIST_OP_TRANSLATE:     glTranslatef(f[0], f[1], f[2]); break;
            case GL_LIST_OP_ROTATE:        glRotatef(f[0], f[1], f[2], f[3]); break;
            case GL_LIST_OP_SCALE:         glScalef(f[0], f[1], f[2]); break;
            case GL_LIST_OP_BIND_TEXTURE:  glBindTexture(cmd->e0, cmd->u0); break;
            case GL_LIST_OP_SHADE_MODEL:   glShadeModel(cmd->e0); break;
            case GL_LIST_OP_CULL_FACE:     glCullFace(cmd->e0); break;
            case GL_LIST_OP_FRONT_FACE:    glFrontFace(cmd->e0); break;
            case GL_LIST_OP_DEPTH_FUNC:    glDepthFunc(cmd->e0); break;
            case GL_LIST_OP_BLEND_FUNC:    glBlendFunc(cmd->e0, cmd->e1); break;
            case GL_LIST_OP_LOAD_MATRIX:
            case GL_LIST_OP_MULT_MATRIX:
                /* The sixteen floats do not fit one command, so a matrix is four commands: this
                 * one carries the first row and the three after it carry the rest. */
                if (i + 3u < list->count) {
                    GLfloat m[16];
                    for (int row = 0; row < 4; row++) {
                        for (int col = 0; col < 4; col++) {
                            m[row * 4 + col] = list->cmds[i + (GLuint)row].f[col];
                        }
                    }
                    if (cmd->op == GL_LIST_OP_LOAD_MATRIX) {
                        glLoadMatrixf(m);
                    } else {
                        glMultMatrixf(m);
                    }
                    i += 3u;
                }
                break;
            case GL_LIST_OP_CALL_LIST:     glCallList(cmd->u0); break;
            default: break;
        }
    }
}

void glCallList(GLuint list) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    /* Compiling one call of another is legal and is recorded, not followed - so a list may call
     * a list that does not exist yet, which the specification allows. */
    if (ctx->list_compiling != 0u) {
        GLfloat none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (gl_list_capture(GL_LIST_OP_CALL_LIST, 0, 0, list, none)) return;
    }

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
    gl_list_execute(slot);
    ctx->list_depth--;
}

void glCallLists(GLsizei n, GLenum type, const GLvoid *lists) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!lists || n == 0) return;

    for (GLsizei i = 0; i < n; i++) {
        GLuint name;
        switch (type) {
            case GL_UNSIGNED_BYTE:  name = ((const GLubyte *)lists)[i]; break;
            case GL_UNSIGNED_SHORT: name = ((const GLushort *)lists)[i]; break;
            case GL_UNSIGNED_INT:   name = ((const GLuint *)lists)[i]; break;
            default:
                /* Checked before any read, not inside the loop's arithmetic: an unrecognised
                 * type read as the widest would run off the end of a byte-sized array. */
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
        }
        glCallList(ctx->list_base + name);
    }
}
