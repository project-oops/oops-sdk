/*
 * oops-gl: OpenGL 2.0's shader and program objects
 *
 * The entry points. The compiler behind them is `glsl_*`; the link is `glsl_link.c`.
 * This file is the object model: names, attachment, deletion, the uniform and attribute
 * queries, and the generic vertex attribute arrays.
 *
 * Shaders and programs share one name space (GL 2.0, 2.15.1), drawn from one counter
 * that only goes up, so a stale name finds nothing rather than a later object.
 * Deleting an attached shader or the program in use sets a flag and the object goes
 * with its last reference; meanwhile `glIsShader` answers GL_FALSE and
 * `glGetShaderiv(GL_DELETE_STATUS)` answers GL_TRUE without error. No call here goes
 * through the display-list recorder. Section 5.4 keeps the object calls, the queries
 * and the attribute arrays out of a list; glUseProgram, glUniform* and glVertexAttrib*,
 * which it compiles into one, execute at once as well.
 */

#include "gl_internal.h"
#include "glsl_internal.h"

/* -------------------------------------------------------------------------
 * Names and slots
 * ------------------------------------------------------------------------- */

static size_t str_len(const char *s) {
    size_t n = 0;
    if (!s)
        return 0;
    while (s[n] != '\0')
        n++;
    return n;
}

static GLboolean str_eq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

/*
 * The context, only if it has claimed GL 2.0. Every entry point here gets its context
 * through this, so a context of a lower version gets NULL with GL_INVALID_OPERATION
 * recorded, and each function returns its failure value (0 for a name, -1 for a
 * location, GL_FALSE for a predicate). The internal helpers take their context as a
 * parameter and are not gated: the draw path and `glContextDestroy` call them whatever
 * the version.
 */
static gl_context_t *gl2_ctx(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return (gl_context_t *)0;
    if (!gl_require_version(ctx, 2u, 0u))
        return (gl_context_t *)0;
    return ctx;
}

gl_shader_object_t *gl_find_shader(gl_context_t *ctx, GLuint name) {
    if (!ctx || name == 0u)
        return (gl_shader_object_t *)0;
    for (int i = 0; i < OOPS_GL_MAX_SHADER_OBJECTS; i++) {
        if (ctx->shaders[i].name == name)
            return &ctx->shaders[i];
    }
    return (gl_shader_object_t *)0;
}

gl_program_object_t *gl_find_program(gl_context_t *ctx, GLuint name) {
    if (!ctx || name == 0u)
        return (gl_program_object_t *)0;
    for (int i = 0; i < OOPS_GL_MAX_PROGRAM_OBJECTS; i++) {
        if (ctx->programs[i].name == name)
            return &ctx->programs[i];
    }
    return (gl_program_object_t *)0;
}

gl_program_object_t *gl_active_program(gl_context_t *ctx) {
    if (!ctx || ctx->program_current == 0u)
        return (gl_program_object_t *)0;
    gl_program_object_t *p = gl_find_program(ctx, ctx->program_current);
    return (p && p->linked) ? p : (gl_program_object_t *)0;
}

/* The next name, from the counter both tables share. Making any object turns the
 * generic attribute fetch on (`gl2_used`), so a fixed-function context does not pay for
 * it per vertex. */
static GLuint next_name(gl_context_t *ctx) {
    if (ctx->gl2_next_name == 0u)
        ctx->gl2_next_name = 1u;
    ctx->gl2_used = GL_TRUE;
    return ctx->gl2_next_name++;
}

/* Whether anything still refers to a shader: the program table, and `flagged` being
 * unset. */
static GLboolean shader_is_attached(gl_context_t *ctx, GLuint name) {
    for (int i = 0; i < OOPS_GL_MAX_PROGRAM_OBJECTS; i++) {
        const gl_program_object_t *p = &ctx->programs[i];
        if (p->name == 0u)
            continue;
        for (int a = 0; a < p->attached_count; a++) {
            if (p->attached[a] == name)
                return GL_TRUE;
        }
    }
    return GL_FALSE;
}

static void shader_destroy(gl_shader_object_t *s) {
    gl_heap_free(s->source);
    gl_glsl_unit_release(s->unit);
    s->name = 0u;
    s->source = (char *)0;
    s->source_len = 0;
    s->unit = (glsl_unit_t *)0;
    s->compiled = GL_FALSE;
    s->flagged = GL_FALSE;
    s->info_log[0] = '\0';
}

static void program_destroy(gl_program_object_t *p) {
    gl_glsl_unit_release(p->vs);
    gl_glsl_unit_release(p->fs);
    gl_heap_free(p->values);
    gl_heap_free(p->hw_ps);
    p->name = 0u;
    p->vs = (glsl_unit_t *)0;
    p->fs = (glsl_unit_t *)0;
    p->values = (float *)0;
    p->hw_ps = (uint32_t *)0;
    p->hw_ps_words = 0u;
    p->hw_ps_vgprs = 0u;
    p->hw_ps_user_sgprs = 0u;
    p->hw_ps_logged = GL_FALSE;
    p->hw_ps_serial = 0u;
    p->hw_color_param = -1;
    p->hw_texcoord_param = -1;
    p->hw_ps_exports_depth = GL_FALSE;
    p->hw_ps_kills = GL_FALSE;
    p->hw_params = 0u;
    p->hw_ps_log[0] = '\0';
    p->linked = GL_FALSE;
    p->flagged = GL_FALSE;
    p->validated = GL_FALSE;
    p->valid_run = GL_FALSE;
    p->attached_count = 0;
    p->uniform_count = 0;
    p->value_floats = 0;
    p->attrib_count = 0;
    p->binding_count = 0;
    p->varying_count = 0;
    p->varying_floats = 0;
    p->info_log[0] = '\0';
}

/* A flagged shader whose last program has just let go of it. Called from
 * glDetachShader, glDeleteProgram and the link teardown. */
static void shader_reap(gl_context_t *ctx, GLuint name) {
    gl_shader_object_t *s = gl_find_shader(ctx, name);
    if (s && s->flagged && !shader_is_attached(ctx, name))
        shader_destroy(s);
}

void gl_free_all_shaders(gl_context_t *ctx) {
    if (!ctx)
        return;
    for (int i = 0; i < OOPS_GL_MAX_PROGRAM_OBJECTS; i++) {
        if (ctx->programs[i].name != 0u)
            program_destroy(&ctx->programs[i]);
    }
    for (int i = 0; i < OOPS_GL_MAX_SHADER_OBJECTS; i++) {
        if (ctx->shaders[i].name != 0u)
            shader_destroy(&ctx->shaders[i]);
    }
    ctx->program_current = 0u;
}

/* -------------------------------------------------------------------------
 * Writing a string back to a caller
 *
 * `glGetShaderInfoLog` and its three relatives all take `bufSize` and `length` and all
 * mean the same thing by them: at most `bufSize` bytes are written including the
 * terminator, and `length` receives what was written not counting it. A `bufSize` of 0
 * writes nothing, not even the terminator, so a caller may pass a null buffer.
 * ------------------------------------------------------------------------- */

static void return_string(const char *src, GLsizei bufSize, GLsizei *length,
                          GLchar *dst) {
    const size_t have = str_len(src);
    size_t wrote = 0;
    if (dst && bufSize > 0) {
        const size_t room = (size_t)bufSize - 1u;
        wrote = (have < room) ? have : room;
        for (size_t i = 0; i < wrote; i++)
            dst[i] = src[i];
        dst[wrote] = '\0';
    }
    if (length)
        *length = (GLsizei)wrote;
}

/* -------------------------------------------------------------------------
 * Shader objects
 * ------------------------------------------------------------------------- */

GLuint glCreateShader(GLenum type) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return 0u;
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return 0u;
    }
    for (int i = 0; i < OOPS_GL_MAX_SHADER_OBJECTS; i++) {
        if (ctx->shaders[i].name != 0u)
            continue;
        gl_shader_object_t *s = &ctx->shaders[i];
        s->name = next_name(ctx);
        s->type = type;
        s->compiled = GL_FALSE;
        s->flagged = GL_FALSE;
        s->source = (char *)0;
        s->source_len = 0;
        s->unit = (glsl_unit_t *)0;
        s->info_log[0] = '\0';
        return s->name;
    }
    /* Zero, with GL_OUT_OF_MEMORY: the specification's answer for a creation that
     * cannot be served. */
    gl_record_error(ctx, GL_OUT_OF_MEMORY);
    return 0u;
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                    const GLint *length) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    if (count < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    gl_shader_object_t *s = gl_find_shader(ctx, shader);
    if (!s) {
        /* A name that is a program rather than a shader is GL_INVALID_OPERATION; one
         * that is neither is GL_INVALID_VALUE, as the specification distinguishes them.
         */
        gl_record_error(ctx, gl_find_program(ctx, shader) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    if (count > 0 && !string) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    /* The strings are stored concatenated, the only form anything reads
     * (`glGetShaderSource` returns it). A negative `length` entry, or a null `length`,
     * means that string is NUL-terminated.
     */
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++) {
        if (!string[i])
            continue;
        total += (length && length[i] >= 0) ? (size_t)length[i] : str_len(string[i]);
    }
    char *joined = (char *)gl_heap_alloc(total + 1u);
    if (!joined) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    size_t at = 0;
    for (GLsizei i = 0; i < count; i++) {
        if (!string[i])
            continue;
        const size_t n =
            (length && length[i] >= 0) ? (size_t)length[i] : str_len(string[i]);
        for (size_t k = 0; k < n; k++)
            joined[at++] = string[i][k];
    }
    joined[at] = '\0';

    gl_heap_free(s->source);
    s->source = joined;
    s->source_len = at;
    /* New source un-compiles the object, as every implementation does, so
     * GL_COMPILE_STATUS never describes replaced text. */
    s->compiled = GL_FALSE;
    gl_glsl_unit_release(s->unit);
    s->unit = (glsl_unit_t *)0;
    s->info_log[0] = '\0';
}

void glCompileShader(GLuint shader) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    gl_shader_object_t *s = gl_find_shader(ctx, shader);
    if (!s) {
        gl_record_error(ctx, gl_find_program(ctx, shader) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    gl_glsl_unit_release(s->unit);
    s->unit = (glsl_unit_t *)0;
    s->compiled = GL_FALSE;
    s->info_log[0] = '\0';

    if (!s->source) {
        /* No source is a failed compile, not an error: GL_COMPILE_STATUS and the log
         * are the channel `glCompileShader` has. */
        oops_snprintf(s->info_log, sizeof(s->info_log), "no source has been given");
        return;
    }
    s->unit = glsl_unit_compile(s->type, s->source, s->source_len, s->info_log,
                                sizeof(s->info_log));
    s->compiled = (GLboolean)(s->unit != (glsl_unit_t *)0);
}

void glDeleteShader(GLuint shader) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || shader == 0u)
        return; /* 0 is silently ignored, as everywhere in GL */
    gl_shader_object_t *s = gl_find_shader(ctx, shader);
    if (!s) {
        gl_record_error(ctx, gl_find_program(ctx, shader) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    s->flagged = GL_TRUE;
    if (!shader_is_attached(ctx, shader))
        shader_destroy(s);
}

GLboolean glIsShader(GLuint shader) {
    gl_context_t *ctx = gl2_ctx();
    const gl_shader_object_t *s = gl_find_shader(ctx, shader);
    /* Flagged for deletion means it is no longer a shader object, although its queries
     * still answer - see the file comment. */
    return (GLboolean)(s != (const gl_shader_object_t *)0 && !s->flagged);
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || !params)
        return;
    const gl_shader_object_t *s = gl_find_shader(ctx, shader);
    if (!s) {
        gl_record_error(ctx, gl_find_program(ctx, shader) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    switch (pname) {
    case GL_SHADER_TYPE:
        *params = (GLint)s->type;
        break;
    case GL_DELETE_STATUS:
        *params = s->flagged ? GL_TRUE : GL_FALSE;
        break;
    case GL_COMPILE_STATUS:
        *params = s->compiled ? GL_TRUE : GL_FALSE;
        break;
    /* The length includes the terminator, and is 0 when there is no log at all. The
     * same for the source. */
    case GL_INFO_LOG_LENGTH:
        *params = s->info_log[0] ? (GLint)(str_len(s->info_log) + 1u) : 0;
        break;
    case GL_SHADER_SOURCE_LENGTH:
        *params = s->source ? (GLint)(s->source_len + 1u) : 0;
        break;
    default:
        gl_record_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length,
                        GLchar *infoLog) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    if (bufSize < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    const gl_shader_object_t *s = gl_find_shader(ctx, shader);
    if (!s) {
        gl_record_error(ctx, gl_find_program(ctx, shader) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    return_string(s->info_log, bufSize, length, infoLog);
}

void glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei *length,
                       GLchar *source) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    if (bufSize < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    const gl_shader_object_t *s = gl_find_shader(ctx, shader);
    if (!s) {
        gl_record_error(ctx, gl_find_program(ctx, shader) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    return_string(s->source ? s->source : "", bufSize, length, source);
}

/* -------------------------------------------------------------------------
 * Program objects
 * ------------------------------------------------------------------------- */

GLuint glCreateProgram(void) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return 0u;
    for (int i = 0; i < OOPS_GL_MAX_PROGRAM_OBJECTS; i++) {
        if (ctx->programs[i].name != 0u)
            continue;
        gl_program_object_t *p = &ctx->programs[i];
        program_destroy(p); /* clears every field; the slot is known free */
        p->name = next_name(ctx);
        return p->name;
    }
    gl_record_error(ctx, GL_OUT_OF_MEMORY);
    return 0u;
}

void glAttachShader(GLuint program, GLuint shader) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    gl_program_object_t *p = gl_find_program(ctx, program);
    gl_shader_object_t *s = gl_find_shader(ctx, shader);
    if (!p || !s) {
        gl_record_error(ctx,
                        (gl_find_shader(ctx, program) || gl_find_program(ctx, shader))
                            ? GL_INVALID_OPERATION
                            : GL_INVALID_VALUE);
        return;
    }
    for (int i = 0; i < p->attached_count; i++) {
        if (p->attached[i] == shader) {
            gl_record_error(ctx, GL_INVALID_OPERATION); /* already attached */
            return;
        }
    }
    if (p->attached_count >= OOPS_GL_MAX_ATTACHED_SHADERS) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    p->attached[p->attached_count++] = shader;
}

void glDetachShader(GLuint program, GLuint shader) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p || !gl_find_shader(ctx, shader)) {
        gl_record_error(ctx,
                        (gl_find_shader(ctx, program) || gl_find_program(ctx, shader))
                            ? GL_INVALID_OPERATION
                            : GL_INVALID_VALUE);
        return;
    }
    for (int i = 0; i < p->attached_count; i++) {
        if (p->attached[i] != shader)
            continue;
        for (int k = i; k + 1 < p->attached_count; k++)
            p->attached[k] = p->attached[k + 1];
        p->attached_count--;
        shader_reap(ctx, shader);
        return;
    }
    gl_record_error(ctx, GL_INVALID_OPERATION); /* not attached to this program */
}

void glLinkProgram(GLuint program) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }

    /* Every attached shader must have compiled, or the link fails rather than running
     * the fixed-function stage in its place. */
    glsl_unit_t *vs = (glsl_unit_t *)0;
    glsl_unit_t *fs = (glsl_unit_t *)0;
    for (int i = 0; i < p->attached_count; i++) {
        const gl_shader_object_t *s = gl_find_shader(ctx, p->attached[i]);
        if (!s)
            continue;
        if (!s->compiled || !s->unit) {
            gl_glsl_unit_release(p->vs);
            gl_glsl_unit_release(p->fs);
            p->vs = (glsl_unit_t *)0;
            p->fs = (glsl_unit_t *)0;
            p->linked = GL_FALSE;
            oops_snprintf(p->info_log, sizeof(p->info_log),
                          "an attached shader has not compiled");
            return;
        }
        if (s->type == GL_VERTEX_SHADER && !vs)
            vs = s->unit;
        if (s->type == GL_FRAGMENT_SHADER && !fs)
            fs = s->unit;
    }
    (void)gl_program_link(ctx, p, vs, fs);
}

void glUseProgram(GLuint program) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    if (program == 0u) {
        ctx->program_current = 0u;
        return;
    }
    gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    if (!p->linked) {
        /* A program that has not linked cannot be made current, and the one already
         * current is left alone. */
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    ctx->program_current = program;
}

void glValidateProgram(GLuint program) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    p->valid_run = GL_TRUE;
    p->info_log[0] = '\0';
    if (!p->linked) {
        p->validated = GL_FALSE;
        oops_snprintf(p->info_log, sizeof(p->info_log), "the program has not linked");
        return;
    }
    /* Whether the program could run against the current state. The check is a sampler
     * pointing at a unit whose bound texture is the wrong target, the specification's
     * own example. */
    p->validated = GL_TRUE;
    for (int i = 0; i < p->uniform_count; i++) {
        const gl_uniform_t *u = &p->uniforms[i];
        if (u->type != GL_SAMPLER_1D && u->type != GL_SAMPLER_2D &&
            u->type != GL_SAMPLER_3D && u->type != GL_SAMPLER_CUBE &&
            u->type != GL_SAMPLER_1D_SHADOW && u->type != GL_SAMPLER_2D_SHADOW) {
            continue;
        }
        for (int e = 0; e < u->size; e++) {
            const int unit = p->values ? (int)p->values[u->offset + e] : 0;
            if (unit < 0 || unit >= OOPS_GL_MAX_TEXTURE_UNITS) {
                p->validated = GL_FALSE;
                oops_snprintf(p->info_log, sizeof(p->info_log),
                              "sampler '%s' names texture unit %d, and there are %d",
                              u->name, unit, OOPS_GL_MAX_TEXTURE_UNITS);
                return;
            }
        }
    }
}

void glDeleteProgram(GLuint program) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || program == 0u)
        return;
    gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    p->flagged = GL_TRUE;
    if (ctx->program_current == program)
        return; /* in use: it goes when something else is */

    /* Its shaders lose a holder, which may be the last one keeping a flagged shader
     * alive. The names are taken first because destroying the program clears the list.
     */
    GLuint held[OOPS_GL_MAX_ATTACHED_SHADERS];
    const int n = p->attached_count;
    for (int i = 0; i < n; i++)
        held[i] = p->attached[i];
    program_destroy(p);
    for (int i = 0; i < n; i++)
        shader_reap(ctx, held[i]);
}

GLboolean glIsProgram(GLuint program) {
    gl_context_t *ctx = gl2_ctx();
    const gl_program_object_t *p = gl_find_program(ctx, program);
    return (GLboolean)(p != (const gl_program_object_t *)0 && !p->flagged);
}

void glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || !params)
        return;
    const gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    switch (pname) {
    case GL_DELETE_STATUS:
        *params = p->flagged ? GL_TRUE : GL_FALSE;
        break;
    case GL_LINK_STATUS:
        *params = p->linked ? GL_TRUE : GL_FALSE;
        break;
    case GL_VALIDATE_STATUS:
        *params = p->validated ? GL_TRUE : GL_FALSE;
        break;
    case GL_ATTACHED_SHADERS:
        *params = p->attached_count;
        break;
    case GL_ACTIVE_UNIFORMS:
        *params = p->uniform_count;
        break;
    case GL_ACTIVE_ATTRIBUTES:
        *params = p->attrib_count;
        break;
    /* oops-gl's own queries: what the console back end made of the fragment stage. A
     * refused fragment shader fails the draw with GL_INVALID_OPERATION; zero words is a
     * refusal, and `glGetProgramHardwareLog` says why. */
    case GL_PROGRAM_HW_PS_WORDS:
        *params = (GLint)p->hw_ps_words;
        break;
    case GL_PROGRAM_HW_PS_VGPRS:
        *params = (GLint)p->hw_ps_vgprs;
        break;
    case GL_PROGRAM_HW_PARAMS:
        *params = (GLint)p->hw_params;
        break;
    case GL_INFO_LOG_LENGTH:
        *params = p->info_log[0] ? (GLint)(str_len(p->info_log) + 1u) : 0;
        break;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH: {
        /* The longest name plus its terminator, over the active uniforms - 0 when there
         * are none. */
        size_t longest = 0;
        for (int i = 0; i < p->uniform_count; i++) {
            const size_t n = str_len(p->uniforms[i].name);
            if (n > longest)
                longest = n;
        }
        *params = p->uniform_count ? (GLint)(longest + 1u) : 0;
        break;
    }
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH: {
        size_t longest = 0;
        for (int i = 0; i < p->attrib_count; i++) {
            const size_t n = str_len(p->attribs[i].name);
            if (n > longest)
                longest = n;
        }
        *params = p->attrib_count ? (GLint)(longest + 1u) : 0;
        break;
    }
    default:
        gl_record_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length,
                         GLchar *infoLog) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    if (bufSize < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    const gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    return_string(p->info_log, bufSize, length, infoLog);
}

/* Why the console back end would not generate code for this program, separate from the
 * link log: a linked program's fragment shader can still be refused by `glsl_ps.c`, and
 * the draw then fails. Empty when the program has console code or no fragment stage. */
void glGetProgramHardwareLog(GLuint program, GLsizei bufSize, GLsizei *length,
                             GLchar *infoLog) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    if (bufSize < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    const gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    return_string(p->hw_ps_log, bufSize, length, infoLog);
}

void glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei *count,
                          GLuint *shaders) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    if (maxCount < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    const gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    GLsizei n = 0;
    for (int i = 0; i < p->attached_count && n < maxCount; i++) {
        if (shaders)
            shaders[n] = p->attached[i];
        n++;
    }
    if (count)
        *count = n;
}

/* -------------------------------------------------------------------------
 * Uniforms
 * ------------------------------------------------------------------------- */

/* `name`, `name[0]` and `name[3]` all name a location; the specification makes the
 * unbracketed name element zero. */
static GLint uniform_location_of(const gl_program_object_t *p, const char *name) {
    if (!name)
        return -1;
    size_t len = str_len(name);
    int element = 0;
    if (len > 2u && name[len - 1u] == ']') {
        size_t open = len - 1u;
        while (open > 0u && name[open] != '[')
            open--;
        if (name[open] != '[')
            return -1;
        /* A non-numeric or negative subscript names nothing, rather than being read as
         * 0. */
        int v = 0;
        for (size_t i = open + 1u; i < len - 1u; i++) {
            if (name[i] < '0' || name[i] > '9')
                return -1;
            v = v * 10 + (name[i] - '0');
        }
        element = v;
        len = open;
    }
    for (int i = 0; i < p->uniform_count; i++) {
        if (str_len(p->uniforms[i].name) != len)
            continue;
        if (!str_eq_n(p->uniforms[i].name, name, len))
            continue;
        if (element >= p->uniforms[i].size)
            return -1;
        return p->uniforms[i].location + element;
    }
    return -1;
}

/* The uniform a location falls in, and which of its elements. NULL for a location that
 * is not this program's - which includes -1, the location `glUniform` is defined to
 * ignore. */
static gl_uniform_t *uniform_at(gl_program_object_t *p, GLint location, int *element) {
    if (!p || location < 0)
        return (gl_uniform_t *)0;
    for (int i = 0; i < p->uniform_count; i++) {
        gl_uniform_t *u = &p->uniforms[i];
        if (location >= u->location && location < u->location + u->size) {
            if (element)
                *element = location - u->location;
            return u;
        }
    }
    return (gl_uniform_t *)0;
}

GLint glGetUniformLocation(GLuint program, const GLchar *name) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return -1;
    const gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return -1;
    }
    if (!p->linked) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return -1;
    }
    return uniform_location_of(p, name);
}

void glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length,
                        GLint *size, GLenum *type, GLchar *name) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    if (bufSize < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    const gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    if ((GLint)index >= p->uniform_count) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    const gl_uniform_t *u = &p->uniforms[index];
    if (size)
        *size = u->size;
    if (type)
        *type = u->type;
    return_string(u->name, bufSize, length, name);
}

/* How many floats one element of a uniform type holds, and which family it belongs to.
 * The families decide which `glUniform` may set it, since the specification requires
 * the command to match the declared type. */
static int type_floats(GLenum t) {
    switch (t) {
    case GL_FLOAT:
    case GL_INT:
    case GL_BOOL:
        return 1;
    case GL_FLOAT_VEC2:
    case GL_INT_VEC2:
    case GL_BOOL_VEC2:
        return 2;
    case GL_FLOAT_VEC3:
    case GL_INT_VEC3:
    case GL_BOOL_VEC3:
        return 3;
    case GL_FLOAT_VEC4:
    case GL_INT_VEC4:
    case GL_BOOL_VEC4:
        return 4;
    case GL_FLOAT_MAT2:
        return 4;
    case GL_FLOAT_MAT3:
        return 9;
    case GL_FLOAT_MAT4:
        return 16;
    case GL_FLOAT_MAT2x3:
    case GL_FLOAT_MAT3x2:
        return 6;
    case GL_FLOAT_MAT2x4:
    case GL_FLOAT_MAT4x2:
        return 8;
    case GL_FLOAT_MAT3x4:
    case GL_FLOAT_MAT4x3:
        return 12;
    default:
        return 1; /* every sampler: its unit number */
    }
}

static GLboolean type_is_sampler(GLenum t) {
    return (GLboolean)(t == GL_SAMPLER_1D || t == GL_SAMPLER_2D || t == GL_SAMPLER_3D ||
                       t == GL_SAMPLER_CUBE || t == GL_SAMPLER_1D_SHADOW ||
                       t == GL_SAMPLER_2D_SHADOW);
}

static GLboolean type_is_bool(GLenum t) {
    return (GLboolean)(t == GL_BOOL || t == GL_BOOL_VEC2 || t == GL_BOOL_VEC3 ||
                       t == GL_BOOL_VEC4);
}

static GLboolean type_is_int(GLenum t) {
    return (GLboolean)(t == GL_INT || t == GL_INT_VEC2 || t == GL_INT_VEC3 ||
                       t == GL_INT_VEC4);
}

static GLboolean type_is_matrix(GLenum t) {
    return (GLboolean)(t == GL_FLOAT_MAT2 || t == GL_FLOAT_MAT3 || t == GL_FLOAT_MAT4 ||
                       t == GL_FLOAT_MAT2x3 || t == GL_FLOAT_MAT2x4 ||
                       t == GL_FLOAT_MAT3x2 || t == GL_FLOAT_MAT3x4 ||
                       t == GL_FLOAT_MAT4x2 || t == GL_FLOAT_MAT4x3);
}

/* A matrix uniform's shape. Both 0 for anything that is not a matrix, so a caller asks
 * once. */
static void type_matrix_shape(GLenum t, int *cols, int *rows) {
    switch (t) {
    case GL_FLOAT_MAT2:
        *cols = 2;
        *rows = 2;
        return;
    case GL_FLOAT_MAT3:
        *cols = 3;
        *rows = 3;
        return;
    case GL_FLOAT_MAT4:
        *cols = 4;
        *rows = 4;
        return;
    case GL_FLOAT_MAT2x3:
        *cols = 2;
        *rows = 3;
        return;
    case GL_FLOAT_MAT2x4:
        *cols = 2;
        *rows = 4;
        return;
    case GL_FLOAT_MAT3x2:
        *cols = 3;
        *rows = 2;
        return;
    case GL_FLOAT_MAT3x4:
        *cols = 3;
        *rows = 4;
        return;
    case GL_FLOAT_MAT4x2:
        *cols = 4;
        *rows = 2;
        return;
    case GL_FLOAT_MAT4x3:
        *cols = 4;
        *rows = 3;
        return;
    default:
        *cols = 0;
        *rows = 0;
        return;
    }
}

/* The shared body of every `glUniform`. `comps` is the command's own width, `integer`
 * whether it is an `i` form, and `matrix_type` the GL enum the matrix forms name - 0
 * for the rest.
 *
 * The matrix check is by type, not float count: `glUniformMatrix2x3fv` and
 * `glUniformMatrix3x2fv` both carry six floats.
 *
 * Returns the destination to write `count` elements into, or NULL when the call does
 * nothing: the error cases, and a location of -1, which is ignored silently. */
static float *uniform_dest(GLint location, int comps, GLboolean integer,
                           GLenum matrix_type, GLsizei count, int *out_elements) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return (float *)0;
    if (count < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return (float *)0;
    }
    gl_program_object_t *p = gl_find_program(ctx, ctx->program_current);
    if (!p || !p->linked) {
        gl_record_error(ctx, GL_INVALID_OPERATION); /* no program in use */
        return (float *)0;
    }
    if (location == -1)
        return (float *)0;
    int element = 0;
    gl_uniform_t *u = uniform_at(p, location, &element);
    if (!u) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return (float *)0;
    }

    if (matrix_type) {
        if (u->type != matrix_type) {
            gl_record_error(ctx, GL_INVALID_OPERATION);
            return (float *)0;
        }
    } else {
        if (type_is_matrix(u->type) || type_floats(u->type) != comps) {
            gl_record_error(ctx, GL_INVALID_OPERATION);
            return (float *)0;
        }
        /* A sampler takes only the integer forms; a bool takes either. */
        if (type_is_sampler(u->type) && !integer) {
            gl_record_error(ctx, GL_INVALID_OPERATION);
            return (float *)0;
        }
        if (!type_is_bool(u->type)) {
            if (type_is_int(u->type) && !integer) {
                gl_record_error(ctx, GL_INVALID_OPERATION);
                return (float *)0;
            }
            if (u->type == GL_FLOAT || u->type == GL_FLOAT_VEC2 ||
                u->type == GL_FLOAT_VEC3 || u->type == GL_FLOAT_VEC4) {
                if (integer) {
                    gl_record_error(ctx, GL_INVALID_OPERATION);
                    return (float *)0;
                }
            }
        }
    }

    /* `count` over 1 on something that is not an array is an error; past the end of an
     * array, the excess is ignored, as the specification says. */
    if (count > 1 && u->size == 1) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return (float *)0;
    }
    int room = u->size - element;
    if (room < 0)
        room = 0;
    int n = (int)count;
    if (n > room)
        n = room;
    if (out_elements)
        *out_elements = n;
    if (n <= 0)
        return (float *)0;
    return &p->values[(size_t)u->offset + (size_t)element * (size_t)u->floats];
}

static void uniform_write_f(GLint location, int comps, GLsizei count,
                            const GLfloat *v) {
    int n = 0;
    float *dst = uniform_dest(location, comps, GL_FALSE, 0, count, &n);
    if (!dst || !v)
        return;
    for (int i = 0; i < n * comps; i++)
        dst[i] = v[i];
}

static void uniform_write_i(GLint location, int comps, GLsizei count, const GLint *v) {
    int n = 0;
    float *dst = uniform_dest(location, comps, GL_TRUE, 0, count, &n);
    if (!dst || !v)
        return;
    for (int i = 0; i < n * comps; i++)
        dst[i] = (float)v[i];
}

void glUniform1f(GLint location, GLfloat v0) {
    uniform_write_f(location, 1, 1, &v0);
}
void glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
    const GLfloat v[2] = {v0, v1};
    uniform_write_f(location, 2, 1, v);
}
void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
    const GLfloat v[3] = {v0, v1, v2};
    uniform_write_f(location, 3, 1, v);
}
void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    const GLfloat v[4] = {v0, v1, v2, v3};
    uniform_write_f(location, 4, 1, v);
}
void glUniform1i(GLint location, GLint v0) {
    uniform_write_i(location, 1, 1, &v0);
}
void glUniform2i(GLint location, GLint v0, GLint v1) {
    const GLint v[2] = {v0, v1};
    uniform_write_i(location, 2, 1, v);
}
void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2) {
    const GLint v[3] = {v0, v1, v2};
    uniform_write_i(location, 3, 1, v);
}
void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3) {
    const GLint v[4] = {v0, v1, v2, v3};
    uniform_write_i(location, 4, 1, v);
}
void glUniform1fv(GLint location, GLsizei count, const GLfloat *v) {
    uniform_write_f(location, 1, count, v);
}
void glUniform2fv(GLint location, GLsizei count, const GLfloat *v) {
    uniform_write_f(location, 2, count, v);
}
void glUniform3fv(GLint location, GLsizei count, const GLfloat *v) {
    uniform_write_f(location, 3, count, v);
}
void glUniform4fv(GLint location, GLsizei count, const GLfloat *v) {
    uniform_write_f(location, 4, count, v);
}
void glUniform1iv(GLint location, GLsizei count, const GLint *v) {
    uniform_write_i(location, 1, count, v);
}
void glUniform2iv(GLint location, GLsizei count, const GLint *v) {
    uniform_write_i(location, 2, count, v);
}
void glUniform3iv(GLint location, GLsizei count, const GLint *v) {
    uniform_write_i(location, 3, count, v);
}
void glUniform4iv(GLint location, GLsizei count, const GLint *v) {
    uniform_write_i(location, 4, count, v);
}

/* The matrices, stored column-major whatever the caller passed, as the shading
 * language reads them, so `transpose` is applied once here. The source of a transposed
 * `mat2x3` is three columns of two, so the read stride is the destination's column
 * count, not its row count. */
static void uniform_write_matrix(GLint location, GLenum type, GLsizei count,
                                 GLboolean transpose, const GLfloat *v) {
    int cols = 0, rows = 0;
    type_matrix_shape(type, &cols, &rows);
    int n = 0;
    const int per = cols * rows;
    float *dst = uniform_dest(location, per, GL_FALSE, type, count, &n);
    if (!dst || !v)
        return;
    for (int e = 0; e < n; e++) {
        const GLfloat *src = v + (size_t)e * (size_t)per;
        float *out = dst + (size_t)e * (size_t)per;
        if (!transpose) {
            for (int i = 0; i < per; i++)
                out[i] = src[i];
        } else {
            for (int col = 0; col < cols; col++) {
                for (int row = 0; row < rows; row++) {
                    out[col * rows + row] = src[row * cols + col];
                }
            }
        }
    }
}

void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *v) {
    uniform_write_matrix(location, GL_FLOAT_MAT2, count, transpose, v);
}
void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *v) {
    uniform_write_matrix(location, GL_FLOAT_MAT3, count, transpose, v);
}
void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *v) {
    uniform_write_matrix(location, GL_FLOAT_MAT4, count, transpose, v);
}
void glUniformMatrix2x3fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *v) {
    uniform_write_matrix(location, GL_FLOAT_MAT2x3, count, transpose, v);
}
void glUniformMatrix3x2fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *v) {
    uniform_write_matrix(location, GL_FLOAT_MAT3x2, count, transpose, v);
}
void glUniformMatrix2x4fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *v) {
    uniform_write_matrix(location, GL_FLOAT_MAT2x4, count, transpose, v);
}
void glUniformMatrix4x2fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *v) {
    uniform_write_matrix(location, GL_FLOAT_MAT4x2, count, transpose, v);
}
void glUniformMatrix3x4fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *v) {
    uniform_write_matrix(location, GL_FLOAT_MAT3x4, count, transpose, v);
}
void glUniformMatrix4x3fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *v) {
    uniform_write_matrix(location, GL_FLOAT_MAT4x3, count, transpose, v);
}

/* The read-back pair. Unlike `glUniform` these name a program rather than acting on the
 * current one, so a caller may read a program it is not drawing with. */
static const float *uniform_read(GLuint program, GLint location, int *comps) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return (const float *)0;
    gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return (const float *)0;
    }
    if (!p->linked) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return (const float *)0;
    }
    int element = 0;
    const gl_uniform_t *u = uniform_at(p, location, &element);
    if (!u || !p->values) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return (const float *)0;
    }
    if (comps)
        *comps = u->floats;
    return &p->values[(size_t)u->offset + (size_t)element * (size_t)u->floats];
}

void glGetUniformfv(GLuint program, GLint location, GLfloat *params) {
    int comps = 0;
    const float *src = uniform_read(program, location, &comps);
    if (!src || !params)
        return;
    for (int i = 0; i < comps; i++)
        params[i] = src[i];
}

void glGetUniformiv(GLuint program, GLint location, GLint *params) {
    int comps = 0;
    const float *src = uniform_read(program, location, &comps);
    if (!src || !params)
        return;
    /* Truncated towards zero, which is what the specification says a float uniform read
     * as an int gives - not rounded. */
    for (int i = 0; i < comps; i++)
        params[i] = (GLint)src[i];
}

/* -------------------------------------------------------------------------
 * Generic vertex attributes
 * ------------------------------------------------------------------------- */

static GLboolean attrib_index_ok(gl_context_t *ctx, GLuint index) {
    if (index >= (GLuint)OOPS_GL_MAX_VERTEX_ATTRIBS) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return GL_FALSE;
    }
    return GL_TRUE;
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                           GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || !attrib_index_ok(ctx, index))
        return;
    if (size < 1 || size > 4) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (stride < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
    case GL_INT:
    case GL_UNSIGNED_INT:
    case GL_FLOAT:
    case GL_DOUBLE:
        break;
    default:
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_vertex_attrib_t *a = &ctx->vertex_attribs[index];
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->pointer = pointer;
    a->normalized = normalized;
    /* The buffer bound now, by name, so a later `glBufferData` that reallocates the
     * store is followed, as for the named arrays. */
    a->buffer = ctx->bound_array_buffer;
}

void glEnableVertexAttribArray(GLuint index) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || !attrib_index_ok(ctx, index))
        return;
    ctx->vertex_attribs[index].enabled = GL_TRUE;
}

void glDisableVertexAttribArray(GLuint index) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || !attrib_index_ok(ctx, index))
        return;
    ctx->vertex_attribs[index].enabled = GL_FALSE;
}

void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || !attrib_index_ok(ctx, index))
        return;
    gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    if (!name)
        return;
    /* `gl_` is the language's own prefix and cannot be bound - the fixed-function
     * attributes have fixed homes. */
    if (str_len(name) >= 3u && name[0] == 'g' && name[1] == 'l' && name[2] == '_') {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    const size_t len = str_len(name);
    if (len + 1u > OOPS_GL_MAX_GLSL_NAME) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* Re-binding a name replaces its entry, so the later binding wins. */
    for (int i = 0; i < p->binding_count; i++) {
        if (str_len(p->bindings[i].name) == len &&
            str_eq_n(p->bindings[i].name, name, len)) {
            p->bindings[i].location = (GLint)index;
            return;
        }
    }
    if (p->binding_count >= OOPS_GL_MAX_VERTEX_ATTRIBS) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    gl_attrib_binding_t *b = &p->bindings[p->binding_count++];
    for (size_t i = 0; i < len; i++)
        b->name[i] = name[i];
    b->name[len] = '\0';
    b->location = (GLint)index;
    b->type = 0u;
    b->size = 1;
}

GLint glGetAttribLocation(GLuint program, const GLchar *name) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return -1;
    const gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return -1;
    }
    if (!p->linked) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return -1;
    }
    if (!name)
        return -1;
    const size_t len = str_len(name);
    for (int i = 0; i < p->attrib_count; i++) {
        if (str_len(p->attribs[i].name) == len &&
            str_eq_n(p->attribs[i].name, name, len)) {
            return p->attribs[i].location;
        }
    }
    return -1;
}

void glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length,
                       GLint *size, GLenum *type, GLchar *name) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx)
        return;
    if (bufSize < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    const gl_program_object_t *p = gl_find_program(ctx, program);
    if (!p) {
        gl_record_error(ctx, gl_find_shader(ctx, program) ? GL_INVALID_OPERATION
                                                          : GL_INVALID_VALUE);
        return;
    }
    if ((GLint)index >= p->attrib_count) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    const gl_attrib_binding_t *a = &p->attribs[index];
    if (size)
        *size = a->size;
    if (type)
        *type = a->type;
    return_string(a->name, bufSize, length, name);
}

void glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || !params || !attrib_index_ok(ctx, index))
        return;
    const gl_vertex_attrib_t *a = &ctx->vertex_attribs[index];
    switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
        params[0] = a->enabled ? 1.0f : 0.0f;
        break;
    case GL_VERTEX_ATTRIB_ARRAY_SIZE:
        params[0] = (float)a->size;
        break;
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
        params[0] = (float)a->stride;
        break;
    case GL_VERTEX_ATTRIB_ARRAY_TYPE:
        params[0] = (float)a->type;
        break;
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
        params[0] = a->normalized ? 1.0f : 0.0f;
        break;
    case GL_CURRENT_VERTEX_ATTRIB:
        for (int i = 0; i < 4; i++)
            params[i] = a->current[i];
        break;
    default:
        gl_record_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

void glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || !params || !attrib_index_ok(ctx, index))
        return;
    const gl_vertex_attrib_t *a = &ctx->vertex_attribs[index];
    switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
        params[0] = a->enabled ? GL_TRUE : GL_FALSE;
        break;
    case GL_VERTEX_ATTRIB_ARRAY_SIZE:
        params[0] = a->size;
        break;
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
        params[0] = (GLint)a->stride;
        break;
    case GL_VERTEX_ATTRIB_ARRAY_TYPE:
        params[0] = (GLint)a->type;
        break;
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
        params[0] = a->normalized ? GL_TRUE : GL_FALSE;
        break;
    case GL_CURRENT_VERTEX_ATTRIB:
        for (int i = 0; i < 4; i++)
            params[i] = (GLint)a->current[i];
        break;
    default:
        gl_record_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

void glGetVertexAttribdv(GLuint index, GLenum pname, GLdouble *params) {
    GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glGetVertexAttribfv(index, pname, f);
    if (!params)
        return;
    const int n = (pname == GL_CURRENT_VERTEX_ATTRIB) ? 4 : 1;
    for (int i = 0; i < n; i++)
        params[i] = (GLdouble)f[i];
}

void glGetVertexAttribPointerv(GLuint index, GLenum pname, GLvoid **pointer) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || !pointer || !attrib_index_ok(ctx, index))
        return;
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    *pointer = (GLvoid *)ctx->vertex_attribs[index].pointer;
}

void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    gl_context_t *ctx = gl2_ctx();
    if (!ctx || !attrib_index_ok(ctx, index))
        return;
    float *c = ctx->vertex_attribs[index].current;
    c[0] = x;
    c[1] = y;
    c[2] = z;
    c[3] = w;
}

/* The short forms fill the missing components from (0, 0, 0, 1): `glVertexAttrib2f`
 * gives z = 0 and w = 1 whatever was set before, as the specification says. */
void glVertexAttrib1f(GLuint index, GLfloat x) {
    glVertexAttrib4f(index, x, 0.0f, 0.0f, 1.0f);
}
void glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y) {
    glVertexAttrib4f(index, x, y, 0.0f, 1.0f);
}
void glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z) {
    glVertexAttrib4f(index, x, y, z, 1.0f);
}
void glVertexAttrib1fv(GLuint i, const GLfloat *v) {
    if (v)
        glVertexAttrib1f(i, v[0]);
}
void glVertexAttrib2fv(GLuint i, const GLfloat *v) {
    if (v)
        glVertexAttrib2f(i, v[0], v[1]);
}
void glVertexAttrib3fv(GLuint i, const GLfloat *v) {
    if (v)
        glVertexAttrib3f(i, v[0], v[1], v[2]);
}
void glVertexAttrib4fv(GLuint i, const GLfloat *v) {
    if (v)
        glVertexAttrib4f(i, v[0], v[1], v[2], v[3]);
}

void glVertexAttrib1d(GLuint i, GLdouble x) {
    glVertexAttrib1f(i, (GLfloat)x);
}
void glVertexAttrib2d(GLuint i, GLdouble x, GLdouble y) {
    glVertexAttrib2f(i, (GLfloat)x, (GLfloat)y);
}
void glVertexAttrib3d(GLuint i, GLdouble x, GLdouble y, GLdouble z) {
    glVertexAttrib3f(i, (GLfloat)x, (GLfloat)y, (GLfloat)z);
}
void glVertexAttrib4d(GLuint i, GLdouble x, GLdouble y, GLdouble z, GLdouble w) {
    glVertexAttrib4f(i, (GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}
void glVertexAttrib1dv(GLuint i, const GLdouble *v) {
    if (v)
        glVertexAttrib1d(i, v[0]);
}
void glVertexAttrib2dv(GLuint i, const GLdouble *v) {
    if (v)
        glVertexAttrib2d(i, v[0], v[1]);
}
void glVertexAttrib3dv(GLuint i, const GLdouble *v) {
    if (v)
        glVertexAttrib3d(i, v[0], v[1], v[2]);
}
void glVertexAttrib4dv(GLuint i, const GLdouble *v) {
    if (v)
        glVertexAttrib4d(i, v[0], v[1], v[2], v[3]);
}

void glVertexAttrib1s(GLuint i, GLshort x) {
    glVertexAttrib1f(i, (GLfloat)x);
}
void glVertexAttrib2s(GLuint i, GLshort x, GLshort y) {
    glVertexAttrib2f(i, (GLfloat)x, (GLfloat)y);
}
void glVertexAttrib3s(GLuint i, GLshort x, GLshort y, GLshort z) {
    glVertexAttrib3f(i, (GLfloat)x, (GLfloat)y, (GLfloat)z);
}
void glVertexAttrib4s(GLuint i, GLshort x, GLshort y, GLshort z, GLshort w) {
    glVertexAttrib4f(i, (GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}
void glVertexAttrib1sv(GLuint i, const GLshort *v) {
    if (v)
        glVertexAttrib1s(i, v[0]);
}
void glVertexAttrib2sv(GLuint i, const GLshort *v) {
    if (v)
        glVertexAttrib2s(i, v[0], v[1]);
}
void glVertexAttrib3sv(GLuint i, const GLshort *v) {
    if (v)
        glVertexAttrib3s(i, v[0], v[1], v[2]);
}
void glVertexAttrib4sv(GLuint i, const GLshort *v) {
    if (v)
        glVertexAttrib4s(i, v[0], v[1], v[2], v[3]);
}

/* The plain integer forms convert; the `N` forms scale:
 * `glVertexAttrib4ubv({255,...})` gives 255.0, `glVertexAttrib4Nubv` gives 1.0. */
void glVertexAttrib4bv(GLuint i, const GLbyte *v) {
    if (v)
        glVertexAttrib4f(i, (GLfloat)v[0], (GLfloat)v[1], (GLfloat)v[2], (GLfloat)v[3]);
}
void glVertexAttrib4iv(GLuint i, const GLint *v) {
    if (v)
        glVertexAttrib4f(i, (GLfloat)v[0], (GLfloat)v[1], (GLfloat)v[2], (GLfloat)v[3]);
}
void glVertexAttrib4ubv(GLuint i, const GLubyte *v) {
    if (v)
        glVertexAttrib4f(i, (GLfloat)v[0], (GLfloat)v[1], (GLfloat)v[2], (GLfloat)v[3]);
}
void glVertexAttrib4uiv(GLuint i, const GLuint *v) {
    if (v)
        glVertexAttrib4f(i, (GLfloat)v[0], (GLfloat)v[1], (GLfloat)v[2], (GLfloat)v[3]);
}
void glVertexAttrib4usv(GLuint i, const GLushort *v) {
    if (v)
        glVertexAttrib4f(i, (GLfloat)v[0], (GLfloat)v[1], (GLfloat)v[2], (GLfloat)v[3]);
}

/* The signed normalisations divide by the type's largest positive value, so -128 of a
 * byte maps just past -1 and is clamped to it (GL 2.0, table 2.9). */
static GLfloat norm_b(GLbyte v) {
    const GLfloat f = (GLfloat)v / 127.0f;
    return f < -1.0f ? -1.0f : f;
}
static GLfloat norm_s(GLshort v) {
    const GLfloat f = (GLfloat)v / 32767.0f;
    return f < -1.0f ? -1.0f : f;
}
static GLfloat norm_i(GLint v) {
    const GLfloat f = (GLfloat)v / 2147483647.0f;
    return f < -1.0f ? -1.0f : f;
}

void glVertexAttrib4Nub(GLuint i, GLubyte x, GLubyte y, GLubyte z, GLubyte w) {
    glVertexAttrib4f(i, (GLfloat)x / 255.0f, (GLfloat)y / 255.0f, (GLfloat)z / 255.0f,
                     (GLfloat)w / 255.0f);
}
void glVertexAttrib4Nubv(GLuint i, const GLubyte *v) {
    if (v)
        glVertexAttrib4Nub(i, v[0], v[1], v[2], v[3]);
}
void glVertexAttrib4Nbv(GLuint i, const GLbyte *v) {
    if (v)
        glVertexAttrib4f(i, norm_b(v[0]), norm_b(v[1]), norm_b(v[2]), norm_b(v[3]));
}
void glVertexAttrib4Nsv(GLuint i, const GLshort *v) {
    if (v)
        glVertexAttrib4f(i, norm_s(v[0]), norm_s(v[1]), norm_s(v[2]), norm_s(v[3]));
}
void glVertexAttrib4Niv(GLuint i, const GLint *v) {
    if (v)
        glVertexAttrib4f(i, norm_i(v[0]), norm_i(v[1]), norm_i(v[2]), norm_i(v[3]));
}
void glVertexAttrib4Nusv(GLuint i, const GLushort *v) {
    if (v) {
        glVertexAttrib4f(i, (GLfloat)v[0] / 65535.0f, (GLfloat)v[1] / 65535.0f,
                         (GLfloat)v[2] / 65535.0f, (GLfloat)v[3] / 65535.0f);
    }
}
void glVertexAttrib4Nuiv(GLuint i, const GLuint *v) {
    if (v) {
        glVertexAttrib4f(i, (GLfloat)v[0] / 4294967295.0f,
                         (GLfloat)v[1] / 4294967295.0f, (GLfloat)v[2] / 4294967295.0f,
                         (GLfloat)v[3] / 4294967295.0f);
    }
}
