/*
 * oops-gl: from the GLSL front end to the GL object model
 *
 * `glsl_unit_compile` drives the preprocessor, parser and semantic stage over a source
 * string and keeps the result. `gl_program_link` matches the two stages' varyings,
 * gathers uniforms and attributes, and gives each a location.
 *
 * Every name in the tree points into the source, so a unit owns a copy of the source
 * and frees the two together. A linked program keeps working after its shaders are
 * detached and deleted (GL 2.0, 2.15.2), so a link takes a reference on each unit.
 *
 * A varying the fragment shader reads and the vertex shader does not write, or one
 * whose type differs between them, is a link error.
 */

#include "glsl_internal.h"

/* -------------------------------------------------------------------------
 * Small string helpers
 *
 * Local rather than the C library's: a name in the tree is a pointer and a length into
 * the source, not a NUL-terminated string.
 * ------------------------------------------------------------------------- */

static size_t lit_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

static GLboolean name_eq(const char *a, size_t alen, const char *b, size_t blen) {
    if (alen != blen)
        return GL_FALSE;
    for (size_t i = 0; i < alen; i++) {
        if (a[i] != b[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

/* Copies a name out of the source into a fixed field, returning false when it does not
 * fit - which is refused at link rather than truncated, since two long names that
 * differ past the cut would become one. */
static GLboolean name_copy(char *dst, size_t cap, const char *src, size_t len) {
    if (len + 1u > cap)
        return GL_FALSE;
    for (size_t i = 0; i < len; i++)
        dst[i] = src[i];
    dst[len] = '\0';
    return GL_TRUE;
}

/* Whether a name is one of the language's own. GLSL reserves the `gl_` prefix, and the
 * API keeps such names out of the active uniform and attribute lists (GL 2.0, 2.15.3).
 */
static GLboolean is_reserved_name(const char *n, size_t len) {
    return (GLboolean)(len >= 3u && n[0] == 'g' && n[1] == 'l' && n[2] == '_');
}

/* -------------------------------------------------------------------------
 * Types, as the API reports them
 * ------------------------------------------------------------------------- */

GLenum glsl_type_to_gl(glsl_type_t t) {
    switch (t) {
    case GLSL_TYPE_FLOAT:
        return GL_FLOAT;
    case GLSL_TYPE_VEC2:
        return GL_FLOAT_VEC2;
    case GLSL_TYPE_VEC3:
        return GL_FLOAT_VEC3;
    case GLSL_TYPE_VEC4:
        return GL_FLOAT_VEC4;
    case GLSL_TYPE_INT:
        return GL_INT;
    case GLSL_TYPE_IVEC2:
        return GL_INT_VEC2;
    case GLSL_TYPE_IVEC3:
        return GL_INT_VEC3;
    case GLSL_TYPE_IVEC4:
        return GL_INT_VEC4;
    case GLSL_TYPE_BOOL:
        return GL_BOOL;
    case GLSL_TYPE_BVEC2:
        return GL_BOOL_VEC2;
    case GLSL_TYPE_BVEC3:
        return GL_BOOL_VEC3;
    case GLSL_TYPE_BVEC4:
        return GL_BOOL_VEC4;
    case GLSL_TYPE_MAT2:
        return GL_FLOAT_MAT2;
    case GLSL_TYPE_MAT3:
        return GL_FLOAT_MAT3;
    case GLSL_TYPE_MAT4:
        return GL_FLOAT_MAT4;
    /* 1.20's non-square matrices. A type of 0 here would leave a uniform no `glUniform`
     * command can write. */
    case GLSL_TYPE_MAT2X3:
        return GL_FLOAT_MAT2x3;
    case GLSL_TYPE_MAT2X4:
        return GL_FLOAT_MAT2x4;
    case GLSL_TYPE_MAT3X2:
        return GL_FLOAT_MAT3x2;
    case GLSL_TYPE_MAT3X4:
        return GL_FLOAT_MAT3x4;
    case GLSL_TYPE_MAT4X2:
        return GL_FLOAT_MAT4x2;
    case GLSL_TYPE_MAT4X3:
        return GL_FLOAT_MAT4x3;
    case GLSL_TYPE_SAMPLER1D:
        return GL_SAMPLER_1D;
    case GLSL_TYPE_SAMPLER2D:
        return GL_SAMPLER_2D;
    case GLSL_TYPE_SAMPLER3D:
        return GL_SAMPLER_3D;
    case GLSL_TYPE_SAMPLERCUBE:
        return GL_SAMPLER_CUBE;
    case GLSL_TYPE_SAMPLER1DSHADOW:
        return GL_SAMPLER_1D_SHADOW;
    case GLSL_TYPE_SAMPLER2DSHADOW:
        return GL_SAMPLER_2D_SHADOW;
    default:
        return 0u;
    }
}

int glsl_type_floats(glsl_type_t t) {
    if (glsl_type_is_sampler(t))
        return 1; /* the unit number glUniform1i set */
    if (t == GLSL_TYPE_VOID || t == GLSL_TYPE_ERROR)
        return 0;
    return glsl_type_components(t);
}

/* -------------------------------------------------------------------------
 * Compiling one shader
 * ------------------------------------------------------------------------- */

void gl_glsl_unit_retain(glsl_unit_t *u) {
    if (u)
        u->refs++;
}

void gl_glsl_unit_release(glsl_unit_t *u) {
    if (!u)
        return;
    if (--u->refs > 0)
        return;
    gl_heap_free(u->source);
    gl_heap_free(u);
}

glsl_unit_t *glsl_unit_compile(GLenum stage, const char *src, size_t len, char *log,
                               size_t log_size) {
    if (log && log_size)
        log[0] = '\0';
    if (!src) {
        glsl_log_write(log, log_size, "no source", 0, 0);
        return (glsl_unit_t *)0;
    }

    /* The unit is allocated first and the source copied into it, because everything
     * after this point points into that copy. The node arena makes it too large for the
     * stack. */
    glsl_unit_t *u = (glsl_unit_t *)gl_heap_alloc(sizeof(glsl_unit_t));
    if (!u) {
        glsl_log_write(log, log_size, "out of memory", 0, 0);
        return (glsl_unit_t *)0;
    }
    u->refs = 1;
    u->stage = stage;
    u->source = (char *)0;
    u->source_len = len;
    u->version = 0;
    u->root = GLSL_NO_NODE;
    u->ast.count = 0;

    u->source = (char *)gl_heap_alloc(len + 1u);
    if (!u->source) {
        gl_heap_free(u);
        glsl_log_write(log, log_size, "out of memory", 0, 0);
        return (glsl_unit_t *)0;
    }
    for (size_t i = 0; i < len; i++)
        u->source[i] = src[i];
    u->source[len] = '\0';

    /* The parser reads through the preprocessor, so directives, macros and
     * conditionals never reach the grammar. On the heap: a `glsl_pp_t` carries a macro
     * token pool of tens of kilobytes. */
    glsl_pp_t *pp = (glsl_pp_t *)gl_heap_alloc(sizeof(glsl_pp_t));
    glsl_parser_t *p = (glsl_parser_t *)gl_heap_alloc(sizeof(glsl_parser_t));
    if (!pp || !p) {
        gl_heap_free(pp);
        gl_heap_free(p);
        glsl_log_write(log, log_size, "out of memory", 0, 0);
        gl_glsl_unit_release(u);
        return (glsl_unit_t *)0;
    }
    glsl_pp_init(pp, u->source, len);
    /* This consumes the directives ahead of the first real token, so the version is
     * known before a line of the shader has been parsed. */
    glsl_parser_init_pp(p, &u->ast, pp);
    u->version = pp->version;

    /* GLSL 1.10 and 1.20; a later `#version` is refused by number. 0 means the shader
     * said nothing, which the specification defines as 1.10. The dialects differ: 1.20
     * converts int to float implicitly and 1.10 converts nothing.
     *
     * `#version 100` is OpenGL ES 1.00, which is derived from GLSL 1.10 and compiles as
     * it: no implicit conversion, `attribute` and `varying`, the same built-in library.
     * Its precision qualifiers are dropped by the parser, since this GL computes in
     * single precision throughout. */
    if (u->version == 100)
        u->version = 110;

    if (u->version != 0 && u->version != 110 && u->version != 120) {
        glsl_log_write(
            log, log_size,
            "only GLSL 1.10, 1.20 and ES 1.00 are implemented; this shader asks "
            "for another",
            1, 1);
        gl_heap_free(pp);
        gl_heap_free(p);
        gl_glsl_unit_release(u);
        return (glsl_unit_t *)0;
    }

    const int32_t root = glsl_parse_translation_unit(p);
    const GLboolean parse_failed = (GLboolean)(p->error || root == GLSL_NO_NODE);
    if (parse_failed || pp->error) {
        /* The preprocessor's error is preferred when both are set, because a directive
         * that went wrong is what made the grammar see nonsense. */
        if (pp->error) {
            glsl_log_write(log, log_size, pp->error, pp->error_line, 1);
        } else {
            glsl_log_write(log, log_size, p->error ? p->error : "parse failed",
                           p->error_line, p->error_column);
        }
        gl_heap_free(pp);
        gl_heap_free(p);
        gl_glsl_unit_release(u);
        return (glsl_unit_t *)0;
    }
    gl_heap_free(pp);
    gl_heap_free(p);
    u->root = root;

    /* The semantic stage runs against a fresh table with this stage's built-ins, and is
     * not kept: what the linker wants, the top-level declarations, is in the tree. */
    glsl_sema_t *sema = (glsl_sema_t *)gl_heap_alloc(sizeof(glsl_sema_t));
    if (!sema) {
        glsl_log_write(log, log_size, "out of memory", 0, 0);
        gl_glsl_unit_release(u);
        return (glsl_unit_t *)0;
    }
    glsl_sema_init(sema, &u->ast);
    sema->stage = stage;
    /* Unstated is 1.10, as the specification says. */
    sema->version = u->version ? u->version : 110;
    GLboolean ok = glsl_declare_builtins(sema, stage);
    if (ok)
        ok = glsl_check_unit(sema, root);
    if (!ok) {
        glsl_log_write(log, log_size,
                       sema->error ? sema->error : "semantic check failed",
                       sema->error_line, sema->error_column);
        gl_heap_free(sema);
        gl_glsl_unit_release(u);
        return (glsl_unit_t *)0;
    }
    /* The struct layouts are copied out before the semantic pass is freed: the
     * interpreter needs them, and they are decided once in `check_struct_def`. The
     * names inside point into `u->source`. */
    u->struct_count = sema->struct_count;
    for (int i = 0; i < sema->struct_count; i++)
        u->structs[i] = sema->structs[i];

    gl_heap_free(sema);
    return u;
}

/* -------------------------------------------------------------------------
 * Linking
 * ------------------------------------------------------------------------- */

/* Whether a unit assigns to `name` anywhere, following an assignment's target through
 * swizzles and indices to the identifier underneath. A whole-tree sweep rather than a
 * walk of `main`, because a helper function may do the write. */
static GLboolean assigns_to(const glsl_ast_t *ast, const char *name) {
    const size_t nlen = lit_len(name);
    for (int32_t i = 0; i < ast->count; i++) {
        const glsl_node_t *n = &ast->nodes[i];
        if (n->kind != GLSL_NODE_ASSIGN)
            continue;
        int32_t target = n->a;
        /* Down through `.xyz` and `[i]` to whatever is being written. */
        while (target != GLSL_NO_NODE) {
            const glsl_node_t *tn = &ast->nodes[target];
            if (tn->kind == GLSL_NODE_FIELD || tn->kind == GLSL_NODE_INDEX) {
                target = tn->a;
                continue;
            }
            if (tn->kind == GLSL_NODE_IDENTIFIER &&
                name_eq(tn->text, tn->length, name, nlen)) {
                return GL_TRUE;
            }
            break;
        }
    }
    return GL_FALSE;
}

/* How many elements a declarator declares: its array length, or 1. The semantic stage
 * has already refused anything that is not a positive literal. */
static int decl_elements(const glsl_ast_t *ast, const glsl_node_t *n) {
    if (n->array_size == GLSL_NO_NODE)
        return 1;
    const glsl_node_t *sz = &ast->nodes[n->array_size];
    return (sz->kind == GLSL_NODE_INTCONST && (int)sz->value > 0) ? (int)sz->value : 1;
}

static gl_uniform_t *find_uniform(gl_program_object_t *p, const char *name,
                                  size_t len) {
    for (int i = 0; i < p->uniform_count; i++) {
        if (name_eq(p->uniforms[i].name, lit_len(p->uniforms[i].name), name, len)) {
            return &p->uniforms[i];
        }
    }
    return (gl_uniform_t *)0;
}

/* Every top-level declaration of one qualifier, handed to `visit`. Anything the
 * semantic stage accepted is here; nothing needs rechecking. */
typedef GLboolean (*decl_fn)(gl_program_object_t *p, const glsl_unit_t *u,
                             const glsl_node_t *n, void *arg);

static GLboolean walk_globals(gl_program_object_t *p, const glsl_unit_t *u,
                              glsl_token_type_t qualifier, decl_fn visit, void *arg) {
    if (!u || u->root == GLSL_NO_NODE)
        return GL_TRUE;
    for (int32_t d = u->ast.nodes[u->root].a; d != GLSL_NO_NODE;
         d = u->ast.nodes[d].sibling) {
        const glsl_node_t *n = &u->ast.nodes[d];
        if (n->kind != GLSL_NODE_DECL || n->qualifier != qualifier)
            continue;
        if (!visit(p, u, n, arg))
            return GL_FALSE;
    }
    return GL_TRUE;
}

static GLboolean add_uniform(gl_program_object_t *p, const glsl_unit_t *u,
                             const glsl_node_t *n, void *arg) {
    (void)arg;
    const glsl_type_t t = glsl_type_from_token(n->type_tok);
    const int elements = decl_elements(&u->ast, n);

    /* A uniform declared in both stages is one uniform with one location and one
     * value; declarations that disagree are a link error. */
    gl_uniform_t *existing = find_uniform(p, n->text, n->length);
    if (existing) {
        if (existing->type != glsl_type_to_gl(t) || existing->size != elements) {
            oops_snprintf(p->info_log, sizeof(p->info_log),
                          "uniform '%s' is declared differently in the two stages",
                          existing->name);
            return GL_FALSE;
        }
        return GL_TRUE;
    }
    /* `gl_ModelViewProjectionMatrix` and the rest are fixed-function state, not the
     * program's uniforms: the API does not list them and `glUniform` cannot set them.
     */
    if (is_reserved_name(n->text, n->length))
        return GL_TRUE;

    if (p->uniform_count >= OOPS_GL_MAX_PROGRAM_UNIFORMS) {
        oops_snprintf(p->info_log, sizeof(p->info_log), "more than %d uniforms",
                      OOPS_GL_MAX_PROGRAM_UNIFORMS);
        return GL_FALSE;
    }
    gl_uniform_t *slot = &p->uniforms[p->uniform_count];
    if (!name_copy(slot->name, sizeof(slot->name), n->text, n->length)) {
        oops_snprintf(p->info_log, sizeof(p->info_log),
                      "a uniform name longer than %d bytes", OOPS_GL_MAX_GLSL_NAME - 1);
        return GL_FALSE;
    }
    slot->type = glsl_type_to_gl(t);
    slot->size = elements;
    slot->floats = glsl_type_floats(t);
    /* A location per array element, not per uniform: `glGetUniformLocation("v[2]")`
     * answers, and an array's elements occupy consecutive locations (GL 2.0, 2.15.3).
     */
    slot->location = (p->uniform_count == 0)
                         ? 0
                         : (p->uniforms[p->uniform_count - 1].location +
                            p->uniforms[p->uniform_count - 1].size);
    slot->offset = p->value_floats;
    p->value_floats += slot->floats * elements;
    p->uniform_count++;
    return GL_TRUE;
}

static GLboolean add_attribute(gl_program_object_t *p, const glsl_unit_t *u,
                               const glsl_node_t *n, void *arg) {
    (void)arg;
    if (is_reserved_name(n->text, n->length))
        return GL_TRUE; /* gl_Vertex and friends */
    if (p->attrib_count >= OOPS_GL_MAX_VERTEX_ATTRIBS) {
        oops_snprintf(p->info_log, sizeof(p->info_log),
                      "more than %d vertex attributes", OOPS_GL_MAX_VERTEX_ATTRIBS);
        return GL_FALSE;
    }
    const glsl_type_t t = glsl_type_from_token(n->type_tok);
    gl_attrib_binding_t *slot = &p->attribs[p->attrib_count];
    if (!name_copy(slot->name, sizeof(slot->name), n->text, n->length)) {
        oops_snprintf(p->info_log, sizeof(p->info_log),
                      "an attribute name longer than %d bytes",
                      OOPS_GL_MAX_GLSL_NAME - 1);
        return GL_FALSE;
    }
    slot->type = glsl_type_to_gl(t);
    slot->size = decl_elements(&u->ast, n);
    slot->location = -1; /* assigned below, once every requested binding is known */
    p->attrib_count++;
    return GL_TRUE;
}

static GLboolean add_varying(gl_program_object_t *p, const glsl_unit_t *u,
                             const glsl_node_t *n, void *arg) {
    (void)arg;
    if (is_reserved_name(n->text, n->length))
        return GL_TRUE;
    const glsl_type_t t = glsl_type_from_token(n->type_tok);
    const int elements = decl_elements(&u->ast, n);
    const int floats = glsl_type_floats(t) * elements;

    for (int i = 0; i < p->varying_count; i++) {
        if (name_eq(p->varyings[i].name, lit_len(p->varyings[i].name), n->text,
                    n->length)) {
            if (p->varyings[i].type != glsl_type_to_gl(t) ||
                p->varyings[i].floats != floats) {
                oops_snprintf(p->info_log, sizeof(p->info_log),
                              "varying '%s' has a different type in each stage",
                              p->varyings[i].name);
                return GL_FALSE;
            }
            return GL_TRUE;
        }
    }
    if (p->varying_count >= OOPS_GL_MAX_PROGRAM_VARYINGS) {
        oops_snprintf(p->info_log, sizeof(p->info_log), "more than %d varyings",
                      OOPS_GL_MAX_PROGRAM_VARYINGS);
        return GL_FALSE;
    }
    if (p->varying_floats + floats > OOPS_GL_MAX_VARYING_FLOATS) {
        oops_snprintf(p->info_log, sizeof(p->info_log),
                      "the varyings need more than %d interpolated floats",
                      OOPS_GL_MAX_VARYING_FLOATS);
        return GL_FALSE;
    }
    gl_varying_t *slot = &p->varyings[p->varying_count];
    if (!name_copy(slot->name, sizeof(slot->name), n->text, n->length)) {
        oops_snprintf(p->info_log, sizeof(p->info_log),
                      "a varying name longer than %d bytes", OOPS_GL_MAX_GLSL_NAME - 1);
        return GL_FALSE;
    }
    slot->type = glsl_type_to_gl(t);
    slot->offset = p->varying_floats;
    slot->floats = floats;
    p->varying_floats += floats;
    p->varying_count++;
    return GL_TRUE;
}

/* Every varying the fragment stage declares must be one the vertex stage declares too.
 * The other direction is fine: a vertex shader may compute more than this fragment
 * shader reads. */
static GLboolean check_fragment_varyings(gl_program_object_t *p, const glsl_unit_t *vs,
                                         const glsl_unit_t *fs) {
    if (!vs || !fs)
        return GL_TRUE;
    for (int32_t d = fs->ast.nodes[fs->root].a; d != GLSL_NO_NODE;
         d = fs->ast.nodes[d].sibling) {
        const glsl_node_t *n = &fs->ast.nodes[d];
        if (n->kind != GLSL_NODE_DECL || n->qualifier != GLSL_TOK_KW_VARYING)
            continue;
        if (is_reserved_name(n->text, n->length))
            continue;
        GLboolean seen = GL_FALSE;
        for (int32_t e = vs->ast.nodes[vs->root].a; e != GLSL_NO_NODE;
             e = vs->ast.nodes[e].sibling) {
            const glsl_node_t *m = &vs->ast.nodes[e];
            if (m->kind != GLSL_NODE_DECL || m->qualifier != GLSL_TOK_KW_VARYING)
                continue;
            if (name_eq(m->text, m->length, n->text, n->length)) {
                seen = GL_TRUE;
                break;
            }
        }
        if (!seen) {
            char name[OOPS_GL_MAX_GLSL_NAME];
            if (!name_copy(name, sizeof(name), n->text, n->length))
                name[0] = '\0';
            oops_snprintf(
                p->info_log, sizeof(p->info_log),
                "the fragment shader reads varying '%s', which the vertex shader does "
                "not write",
                name);
            return GL_FALSE;
        }
    }
    return GL_TRUE;
}

/* How many attribute slots one declaration takes: one per column, so `matCxR` takes C
 * and a `mat3x2` takes three. */
static int attrib_slot_span(GLenum type) {
    switch (type) {
    case GL_FLOAT_MAT2:
    case GL_FLOAT_MAT2x3:
    case GL_FLOAT_MAT2x4:
        return 2;
    case GL_FLOAT_MAT3:
    case GL_FLOAT_MAT3x2:
    case GL_FLOAT_MAT3x4:
        return 3;
    case GL_FLOAT_MAT4:
    case GL_FLOAT_MAT4x2:
    case GL_FLOAT_MAT4x3:
        return 4;
    default:
        return 1;
    }
}

/* A slot nothing has claimed, for an attribute the caller did not bind. */
static GLint lowest_free_slot(const gl_program_object_t *p, int need) {
    for (GLint base = 0; base + need <= OOPS_GL_MAX_VERTEX_ATTRIBS; base++) {
        GLboolean clash = GL_FALSE;
        for (int i = 0; i < p->attrib_count && !clash; i++) {
            const GLint loc = p->attribs[i].location;
            if (loc < 0)
                continue;
            /* A matrix attribute takes one slot per column, which is why this is a
             * range overlap rather than an equality. */
            const int span = attrib_slot_span(p->attribs[i].type);
            if (base < loc + span && loc < base + need)
                clash = GL_TRUE;
        }
        if (!clash)
            return base;
    }
    return -1;
}

/* Links what is attached. Returns false with `info_log` written; the caller sets
 * GL_LINK_STATUS.
 *
 * The program's previous link is torn down first, references and all, so a failed
 * relink leaves a program that is not linked (GL 2.0, 2.15.3). */
GLboolean gl_program_link(gl_context_t *ctx, gl_program_object_t *p, glsl_unit_t *vs,
                          glsl_unit_t *fs) {
    if (!ctx || !p)
        return GL_FALSE;

    gl_glsl_unit_release(p->vs);
    gl_glsl_unit_release(p->fs);
    p->vs = (glsl_unit_t *)0;
    p->fs = (glsl_unit_t *)0;
    p->linked = GL_FALSE;
    p->uniform_count = 0;
    p->value_floats = 0;
    p->attrib_count = 0;
    p->varying_count = 0;
    p->varying_floats = 0;
    p->info_log[0] = '\0';
    gl_heap_free(p->values);
    p->values = (float *)0;
    gl_heap_free(p->hw_ps);
    p->hw_ps = (uint32_t *)0;
    p->hw_ps_words = 0u;
    p->hw_ps_vgprs = 0u;
    p->hw_ps_user_sgprs = 0u;
    gl_heap_free(p->hw_vs);
    p->hw_vs = (uint32_t *)0;
    p->hw_vs_words = 0u;
    p->hw_vs_vgprs = 0u;
    p->hw_vs_user_sgprs = 0u;
    p->hw_vs_logged = GL_FALSE;
    p->hw_vs_serial = 0u;
    p->hw_vs_log[0] = '\0';
    /* A relink is a new answer, so a program refused again says why again: the source
     * may have changed and the reason with it. */
    p->hw_ps_logged = GL_FALSE;
    /* The old words are gone, so the old identity goes with them: a relink issues a new
     * serial below, and the slot uploads again rather than trusting what it holds. */
    p->hw_ps_serial = 0u;
    p->hw_params = 0u;
    p->hw_color_param = -1;
    p->hw_texcoord_param = -1;
    p->hw_ps_exports_depth = GL_FALSE;
    p->hw_ps_kills = GL_FALSE;
    p->hw_ps_log[0] = '\0';
    p->hw_tex_sets = 0;
    for (size_t i = 0; i < sizeof(p->hw_tex_uniform) / sizeof(p->hw_tex_uniform[0]);
         i++) {
        p->hw_tex_uniform[i] = -1;
    }

    if (!vs && !fs) {
        oops_snprintf(p->info_log, sizeof(p->info_log), "no shaders attached");
        return GL_FALSE;
    }
    /* A vertex shader that never writes `gl_Position` is undefined by the
     * specification; it is a link error here. */
    if (vs && !assigns_to(&vs->ast, "gl_Position")) {
        oops_snprintf(p->info_log, sizeof(p->info_log),
                      "the vertex shader never writes gl_Position");
        return GL_FALSE;
    }

    if (!walk_globals(p, vs, GLSL_TOK_KW_UNIFORM, add_uniform, (void *)0))
        return GL_FALSE;
    if (!walk_globals(p, fs, GLSL_TOK_KW_UNIFORM, add_uniform, (void *)0))
        return GL_FALSE;
    if (!walk_globals(p, vs, GLSL_TOK_KW_ATTRIBUTE, add_attribute, (void *)0))
        return GL_FALSE;
    if (!walk_globals(p, vs, GLSL_TOK_KW_VARYING, add_varying, (void *)0))
        return GL_FALSE;
    if (!walk_globals(p, fs, GLSL_TOK_KW_VARYING, add_varying, (void *)0))
        return GL_FALSE;
    if (!check_fragment_varyings(p, vs, fs))
        return GL_FALSE;

    /* `gl_PointCoord`'s two restrictions. They hold on both paths, so they are link
     * errors and sit above `p->linked = GL_TRUE`; nothing after that line fails a link.
     *
     * `gl_PointCoord` and `gl_TexCoord[0]` are one interpolant: the point expansion
     * writes the sprite coordinate into texture coordinate 0, as `GL_COORD_REPLACE`
     * does and as the part does with `SPI_PS_INPUT_CNTL.PT_SPRITE_TEX`.
     *
     * Not alongside a vertex shader: `gl_draw_point_square` sizes the square in object
     * space through the inverse of the fixed-function MVP, and a vertex shader would
     * collapse its four corners, which carry the same attributes, onto one point.
     *
     * The flag is recorded on the program because the draw needs it too: it makes the
     * expansion generate the coordinate for a unit whose `GL_COORD_REPLACE` is clear.
     */
    p->hw_reads_point_coord = (GLboolean)(fs != (glsl_unit_t *)0 &&
                                          glsl_unit_mentions(fs, "gl_PointCoord", 13u));
    if (p->hw_reads_point_coord && glsl_unit_mentions(fs, "gl_TexCoord", 11u)) {
        oops_snprintf(
            p->info_log, sizeof(p->info_log),
            "a fragment shader reading gl_PointCoord cannot also read gl_TexCoord: "
            "they are the same interpolant here, as they are on the hardware");
        return GL_FALSE;
    }
    if (p->hw_reads_point_coord && vs) {
        oops_snprintf(
            p->info_log, sizeof(p->info_log),
            "gl_PointCoord needs the fixed-function vertex stage: points are expanded "
            "into their square before the vertex stage here, and a vertex shader "
            "collapses the square. Attach only a fragment shader");
        return GL_FALSE;
    }

    /* Requested bindings first, then the rest into the lowest free slots. A binding
     * that names no attribute in this program is not an error. */
    for (int i = 0; i < p->attrib_count; i++) {
        for (int b = 0; b < p->binding_count; b++) {
            if (name_eq(p->attribs[i].name, lit_len(p->attribs[i].name),
                        p->bindings[b].name, lit_len(p->bindings[b].name))) {
                p->attribs[i].location = p->bindings[b].location;
                break;
            }
        }
    }
    for (int i = 0; i < p->attrib_count; i++) {
        if (p->attribs[i].location >= 0)
            continue;
        const GLint slot = lowest_free_slot(p, attrib_slot_span(p->attribs[i].type));
        if (slot < 0) {
            oops_snprintf(p->info_log, sizeof(p->info_log),
                          "the attributes do not fit in %d slots",
                          OOPS_GL_MAX_VERTEX_ATTRIBS);
            return GL_FALSE;
        }
        p->attribs[i].location = slot;
    }

    /* A freshly linked program's uniforms hold zero, and a sampler texture unit 0; a
     * relink resets them, as the specification says. */
    if (p->value_floats > 0) {
        p->values = (float *)gl_heap_alloc((size_t)p->value_floats * sizeof(float));
        if (!p->values) {
            oops_snprintf(p->info_log, sizeof(p->info_log), "out of memory");
            return GL_FALSE;
        }
        for (int i = 0; i < p->value_floats; i++)
            p->values[i] = 0.0f;
    }

    p->vs = vs;
    p->fs = fs;
    gl_glsl_unit_retain(vs);
    gl_glsl_unit_retain(fs);
    p->linked = GL_TRUE;

    /* -----------------------------------------------------------------
     * The console's half, compiled here because this is the last point that knows the
     * whole program and a draw is the wrong place for compiler scratch.
     *
     * A failure here does not fail the link, so GL_LINK_STATUS answers the same on a
     * host and on hardware. The program still draws on the software path; `hw_ps_log`
     * is what the draw path reports once when it refuses.
     * ----------------------------------------------------------------- */
    /* Which sampler gets which descriptor set, in the fragment shader's declaration
     * order - decided once, because the compiled shader and the draw path must agree.
     * A vertex shader cannot sample here. */
    if (fs) {
        const int max_sets =
            (int)(sizeof(p->hw_tex_uniform) / sizeof(p->hw_tex_uniform[0]));
        for (int32_t d = fs->ast.nodes[fs->root].a;
             d != GLSL_NO_NODE && p->hw_tex_sets < max_sets;
             d = fs->ast.nodes[d].sibling) {
            const glsl_node_t *n = &fs->ast.nodes[d];
            if (n->kind != GLSL_NODE_DECL || n->qualifier != GLSL_TOK_KW_UNIFORM)
                continue;
            for (int i = 0; i < p->uniform_count; i++) {
                /* A cube sampler takes a set too: the faces upload as one array and
                 * the descriptor carries TYPE 0xb. */
                if (p->uniforms[i].type != GL_SAMPLER_2D &&
                    p->uniforms[i].type != GL_SAMPLER_CUBE &&
                    p->uniforms[i].type != GL_SAMPLER_3D &&
                    p->uniforms[i].type != GL_SAMPLER_2D_SHADOW &&
                    p->uniforms[i].type != GL_SAMPLER_1D &&
                    p->uniforms[i].type != GL_SAMPLER_1D_SHADOW) {
                    continue;
                }
                if (!name_eq(p->uniforms[i].name, lit_len(p->uniforms[i].name), n->text,
                             n->length)) {
                    continue;
                }
                p->hw_tex_uniform[p->hw_tex_sets++] = i;
                break;
            }
        }
    }

    p->hw_params = (uint32_t)((p->varying_floats + 3) / 4);

    /* Whether the fragment stage writes its own depth: `SPI_SHADER_Z_FORMAT` has to
     * expect a Z, and `DB_SHADER_CONTROL` has to stop testing early. Decided here
     * because the draw and the compiler both read it. */
    p->hw_ps_exports_depth = (GLboolean)(fs != (glsl_unit_t *)0 &&
                                         glsl_unit_mentions(fs, "gl_FragDepth", 12u));
    /* Whether it can kill, for `DB_SHADER_CONTROL.KILL_ENABLE`. By node kind, since
     * `discard` is a keyword and not an identifier. */
    p->hw_ps_kills = glsl_unit_discards(fs);

    /* `gl_Color` costs a parameter only when a fragment shader reads it. With a vertex
     * shader the user's varyings own the parameters from 0, so the colour takes the
     * next one. Without one, the fixed-function vertex path writes the colour into
     * parameter 0. */
    p->hw_color_param = -1;
    if (fs && glsl_unit_mentions(fs, "gl_Color", 8u)) {
        if (vs) {
            p->hw_color_param = (int)p->hw_params;
            p->hw_params++;
        } else {
            p->hw_color_param = 0;
        }
    }

    /* `gl_TexCoord[]`, one parameter an element, carried from the fixed-function vertex
     * stage's block (`GL_SHADER_VARY_TEXCOORD`) to a compiled fragment shader. Every
     * element is reserved, not only the ones indexed; the cost is bounded by
     * `OOPS_GL_MAX_TEXTURE_UNITS` and paid only by a shader that names the array. */
    p->hw_texcoord_param = -1;
    if (fs && glsl_unit_mentions(fs, "gl_TexCoord", 11u)) {
        p->hw_texcoord_param = (int)p->hw_params;
        p->hw_params += (uint32_t)OOPS_GL_MAX_TEXTURE_UNITS;
    }

    if (p->hw_params < 2u)
        p->hw_params = 2u; /* the pipeline's smallest configuration */
    if (fs) {
        p->hw_ps = (uint32_t *)gl_heap_alloc(OOPS_GL_PS_GL2_WORDS * sizeof(uint32_t));
        if (!p->hw_ps) {
            oops_snprintf(p->hw_ps_log, sizeof(p->hw_ps_log),
                          "no memory for a compiled pixel shader");
        } else if (!gl_program_compile_fragment(
                       p, p->hw_ps, OOPS_GL_PS_GL2_WORDS, &p->hw_ps_words,
                       &p->hw_ps_vgprs, &p->hw_ps_user_sgprs, &p->hw_ps_input_ena,
                       p->hw_ps_log, sizeof(p->hw_ps_log))) {
            gl_heap_free(p->hw_ps);
            p->hw_ps = (uint32_t *)0;
            p->hw_ps_words = 0u;
            p->hw_ps_user_sgprs = 0u;
        } else {
            /* An identity for these words, never reissued. The draw path uses it to
             * decide whether the payload's slot already holds them; a program name
             * would not do, because `glDeleteProgram` gives it back. */
            if (ctx->hw_ps_next_serial == 0u)
                ctx->hw_ps_next_serial = 1u;
            p->hw_ps_serial = ctx->hw_ps_next_serial++;
        }
    } else {
        /* No fragment stage: the fixed-function pixel shader in the payload is what
         * runs, and there is nothing to compile or to refuse. */
        p->hw_ps_log[0] = '\0';
    }

    if (vs) {
        p->hw_vs = (uint32_t *)gl_heap_alloc(OOPS_GL_VS_GL2_WORDS * sizeof(uint32_t));
        if (!p->hw_vs) {
            oops_snprintf(p->hw_vs_log, sizeof(p->hw_vs_log),
                          "no memory for a compiled vertex shader");
        } else if (!gl_program_compile_vertex(p, p->hw_vs, OOPS_GL_VS_GL2_WORDS,
                                              &p->hw_vs_words, &p->hw_vs_vgprs,
                                              &p->hw_vs_user_sgprs, p->hw_vs_log,
                                              sizeof(p->hw_vs_log))) {
            gl_heap_free(p->hw_vs);
            p->hw_vs = (uint32_t *)0;
            p->hw_vs_words = 0u;
            p->hw_vs_vgprs = 0u;
            p->hw_vs_user_sgprs = 0u;
        } else {
            if (ctx->hw_vs_next_serial == 0u)
                ctx->hw_vs_next_serial = 1u;
            p->hw_vs_serial = ctx->hw_vs_next_serial++;
        }
    } else {
        p->hw_vs_log[0] = '\0';
    }
    return GL_TRUE;
}
