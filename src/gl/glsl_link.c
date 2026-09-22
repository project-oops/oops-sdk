/*
 * oops-gl: from the GLSL front end to the GL object model
 *
 * The bridge between a compiler that turns text into a tree and an API that hands out names.
 * Two jobs:
 *
 *   - **Compiling one shader.** `glsl_unit_compile` drives the preprocessor, the parser and the
 *     semantic stage over a source string and keeps what comes out, source and all.
 *   - **Linking a program.** Matching the two stages' varyings, gathering their uniforms and the
 *     vertex stage's attributes, and giving each a location - which is what makes
 *     `glGetUniformLocation` answer anything.
 *
 * # Why the source is kept
 *
 * The lexer copies nothing: every token's `text` points into the source string, and so does
 * every name in the tree. That is what makes a diagnostic able to quote the line. It also means
 * **the tree is only valid while the source is**, so a unit owns a copy of it and frees the two
 * together. A shader object that freed the caller's string after compiling and kept the tree
 * would work until the memory was reused.
 *
 * # Why a link takes references rather than copies
 *
 * The specification lets a program be linked and its shaders then detached and deleted, and the
 * program keeps working (GL 2.0, 2.15.2) - so the tree has to outlive the shader object. A copy
 * would cost the whole arena per stage; a reference count costs a word and cannot get out of
 * step with itself.
 *
 * # What the linker refuses
 *
 * A varying the fragment shader reads and the vertex shader does not write is a **link error**,
 * not a varying that interpolates zero. So is a mismatch in its type. Both are what the
 * specification says, and both are the shape of mistake that otherwise produces a black screen
 * with nothing to read.
 */

#include "glsl_internal.h"

/* -------------------------------------------------------------------------
 * Small string helpers
 *
 * Local rather than the C library's, because this file is compiled into a freestanding target
 * where `strncmp` is whatever `freestd` provides and a NUL-terminated name is not always what is
 * being compared - a name in the tree is a pointer and a length into the source.
 * ------------------------------------------------------------------------- */

static size_t lit_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static GLboolean name_eq(const char *a, size_t alen, const char *b, size_t blen) {
    if (alen != blen) return GL_FALSE;
    for (size_t i = 0; i < alen; i++) {
        if (a[i] != b[i]) return GL_FALSE;
    }
    return GL_TRUE;
}

/* Copies a name out of the source into a fixed field, returning false when it does not fit -
 * which is refused at link rather than truncated, since two long names that differ past the cut
 * would become one. */
static GLboolean name_copy(char *dst, size_t cap, const char *src, size_t len) {
    if (len + 1u > cap) return GL_FALSE;
    for (size_t i = 0; i < len; i++) dst[i] = src[i];
    dst[len] = '\0';
    return GL_TRUE;
}

/* Whether a name is one of the language's own. GLSL reserves the `gl_` prefix, and the API
 * keeps such names out of the active uniform and attribute lists (GL 2.0, 2.15.3) - so a
 * program enumerating its uniforms does not have to filter the fixed-function state out. */
static GLboolean is_reserved_name(const char *n, size_t len) {
    return (GLboolean)(len >= 3u && n[0] == 'g' && n[1] == 'l' && n[2] == '_');
}

/* -------------------------------------------------------------------------
 * Types, as the API reports them
 * ------------------------------------------------------------------------- */

GLenum glsl_type_to_gl(glsl_type_t t) {
    switch (t) {
        case GLSL_TYPE_FLOAT: return GL_FLOAT;
        case GLSL_TYPE_VEC2:  return GL_FLOAT_VEC2;
        case GLSL_TYPE_VEC3:  return GL_FLOAT_VEC3;
        case GLSL_TYPE_VEC4:  return GL_FLOAT_VEC4;
        case GLSL_TYPE_INT:   return GL_INT;
        case GLSL_TYPE_IVEC2: return GL_INT_VEC2;
        case GLSL_TYPE_IVEC3: return GL_INT_VEC3;
        case GLSL_TYPE_IVEC4: return GL_INT_VEC4;
        case GLSL_TYPE_BOOL:  return GL_BOOL;
        case GLSL_TYPE_BVEC2: return GL_BOOL_VEC2;
        case GLSL_TYPE_BVEC3: return GL_BOOL_VEC3;
        case GLSL_TYPE_BVEC4: return GL_BOOL_VEC4;
        case GLSL_TYPE_MAT2:  return GL_FLOAT_MAT2;
        case GLSL_TYPE_MAT3:  return GL_FLOAT_MAT3;
        case GLSL_TYPE_MAT4:  return GL_FLOAT_MAT4;
        case GLSL_TYPE_SAMPLER1D:       return GL_SAMPLER_1D;
        case GLSL_TYPE_SAMPLER2D:       return GL_SAMPLER_2D;
        case GLSL_TYPE_SAMPLER3D:       return GL_SAMPLER_3D;
        case GLSL_TYPE_SAMPLERCUBE:     return GL_SAMPLER_CUBE;
        case GLSL_TYPE_SAMPLER1DSHADOW: return GL_SAMPLER_1D_SHADOW;
        case GLSL_TYPE_SAMPLER2DSHADOW: return GL_SAMPLER_2D_SHADOW;
        default: return 0u;
    }
}

int glsl_type_floats(glsl_type_t t) {
    if (glsl_type_is_sampler(t)) return 1;   /* the unit number glUniform1i set */
    if (t == GLSL_TYPE_VOID || t == GLSL_TYPE_ERROR) return 0;
    return glsl_type_components(t);
}

/* -------------------------------------------------------------------------
 * Compiling one shader
 * ------------------------------------------------------------------------- */

static void log_write(char *log, size_t cap, const char *why, int line, int column) {
    if (!log || cap == 0u) return;
    if (!why) { log[0] = '\0'; return; }
    if (line > 0) {
        oops_snprintf(log, cap, "%d:%d: %s", line, column, why);
    } else {
        oops_snprintf(log, cap, "%s", why);
    }
}

void gl_glsl_unit_retain(glsl_unit_t *u) {
    if (u) u->refs++;
}

void gl_glsl_unit_release(glsl_unit_t *u) {
    if (!u) return;
    if (--u->refs > 0) return;
    gl_heap_free(u->source);
    gl_heap_free(u);
}

glsl_unit_t *glsl_unit_compile(GLenum stage, const char *src, size_t len, char *log,
                               size_t log_size) {
    if (log && log_size) log[0] = '\0';
    if (!src) {
        log_write(log, log_size, "no source", 0, 0);
        return (glsl_unit_t *)0;
    }

    /* The unit is allocated first and the source copied into it, because everything after this
     * point points into that copy. It is over a hundred kilobytes - the node arena - which is
     * why it is on the heap and not a local. */
    glsl_unit_t *u = (glsl_unit_t *)gl_heap_alloc(sizeof(glsl_unit_t));
    if (!u) {
        log_write(log, log_size, "out of memory", 0, 0);
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
        log_write(log, log_size, "out of memory", 0, 0);
        return (glsl_unit_t *)0;
    }
    for (size_t i = 0; i < len; i++) u->source[i] = src[i];
    u->source[len] = '\0';

    /* **The parser reads through the preprocessor**, rather than the two being separate passes
     * over the text. A `#version` line, a `#define` or a conditional would otherwise reach the
     * grammar as a stray `#` - and running the preprocessor first and then the parser over the
     * same raw source would find the directive errors and still mis-parse the shader.
     *
     * On the heap, not the stack: a `glsl_pp_t` carries a thousand-token macro pool and is some
     * tens of kilobytes, which is more than a freestanding thread's stack should be asked for. */
    glsl_pp_t *pp = (glsl_pp_t *)gl_heap_alloc(sizeof(glsl_pp_t));
    glsl_parser_t *p = (glsl_parser_t *)gl_heap_alloc(sizeof(glsl_parser_t));
    if (!pp || !p) {
        gl_heap_free(pp);
        gl_heap_free(p);
        log_write(log, log_size, "out of memory", 0, 0);
        gl_glsl_unit_release(u);
        return (glsl_unit_t *)0;
    }
    glsl_pp_init(pp, u->source, len);
    /* This consumes the directives ahead of the first real token, so the version is known
     * before a line of the shader has been parsed. */
    glsl_parser_init_pp(p, &u->ast, pp);
    u->version = pp->version;

    /* **GLSL 1.10 and 1.20, and a later `#version` is refused by number.** 0 means the shader
     * said nothing, which the specification defines as 1.10.
     *
     * The two dialects are not compiled the same way and the difference is not cosmetic: 1.20
     * converts int to float implicitly and 1.10 converts nothing, so `vec3 * 2` is a shader in
     * one and an error in the other. Compiling a 1.30 shader as either would take *its* rules -
     * `in`/`out` in place of `attribute`/`varying`, integer arithmetic that means it - somewhere
     * else silently, which is why the number is checked rather than hoped about. */
    if (u->version != 0 && u->version != 110 && u->version != 120) {
        log_write(log, log_size,
                  "only GLSL 1.10 and 1.20 are implemented; this shader asks for another", 1, 1);
        gl_heap_free(pp);
        gl_heap_free(p);
        gl_glsl_unit_release(u);
        return (glsl_unit_t *)0;
    }

    const int32_t root = glsl_parse_translation_unit(p);
    const GLboolean parse_failed = (GLboolean)(p->error || root == GLSL_NO_NODE);
    if (parse_failed || pp->error) {
        /* A directive error is reported at its own line; the parser's carries its column. The
         * preprocessor's is preferred when both are set, because a directive that went wrong is
         * what made the grammar see nonsense. */
        if (pp->error) {
            log_write(log, log_size, pp->error, pp->error_line, 1);
        } else {
            log_write(log, log_size, p->error ? p->error : "parse failed", p->error_line,
                      p->error_column);
        }
        gl_heap_free(pp);
        gl_heap_free(p);
        gl_glsl_unit_release(u);
        return (glsl_unit_t *)0;
    }
    gl_heap_free(pp);
    gl_heap_free(p);
    u->root = root;

    /* The semantic stage runs against a fresh table each time, with this stage's built-ins in
     * it. It is a hundred kilobytes of its own and is not kept: nothing after the check needs
     * the scoped symbol table, and what the linker wants - the top-level declarations - is in
     * the tree. */
    glsl_sema_t *sema = (glsl_sema_t *)gl_heap_alloc(sizeof(glsl_sema_t));
    if (!sema) {
        log_write(log, log_size, "out of memory", 0, 0);
        gl_glsl_unit_release(u);
        return (glsl_unit_t *)0;
    }
    glsl_sema_init(sema, &u->ast);
    sema->stage = stage;
    /* Unstated is 1.10, which converts nothing - the stricter of the two, so a shader that says
     * no version is held to the rules its author most likely meant. */
    sema->version = u->version ? u->version : 110;
    GLboolean ok = glsl_declare_builtins(sema, stage);
    if (ok) ok = glsl_check_unit(sema, root);
    if (!ok) {
        log_write(log, log_size, sema->error ? sema->error : "semantic check failed",
                  sema->error_line, sema->error_column);
        gl_heap_free(sema);
        gl_glsl_unit_release(u);
        return (glsl_unit_t *)0;
    }
    gl_heap_free(sema);
    return u;
}

/* -------------------------------------------------------------------------
 * Linking
 * ------------------------------------------------------------------------- */

/* Whether a unit assigns to `name` anywhere, following an assignment's target through swizzles
 * and indices to the identifier underneath. Used for the one thing a vertex shader must do.
 *
 * A whole-tree sweep rather than a walk of `main`, because `gl_Position` is as often written by
 * a helper the shader calls. It is linear in the node count and runs once per link. */
static GLboolean assigns_to(const glsl_ast_t *ast, const char *name) {
    const size_t nlen = lit_len(name);
    for (int32_t i = 0; i < ast->count; i++) {
        const glsl_node_t *n = &ast->nodes[i];
        if (n->kind != GLSL_NODE_ASSIGN) continue;
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

/* How many elements a declarator declares: its array length, or 1. The semantic stage has
 * already refused anything that is not a positive literal, so this cannot be surprised. */
static int decl_elements(const glsl_ast_t *ast, const glsl_node_t *n) {
    if (n->array_size == GLSL_NO_NODE) return 1;
    const glsl_node_t *sz = &ast->nodes[n->array_size];
    return (sz->kind == GLSL_NODE_INTCONST && (int)sz->value > 0) ? (int)sz->value : 1;
}

static gl_uniform_t *find_uniform(gl_program_object_t *p, const char *name, size_t len) {
    for (int i = 0; i < p->uniform_count; i++) {
        if (name_eq(p->uniforms[i].name, lit_len(p->uniforms[i].name), name, len)) {
            return &p->uniforms[i];
        }
    }
    return (gl_uniform_t *)0;
}

/* Every top-level declaration of one qualifier, handed to `visit`. Anything the semantic stage
 * accepted is here; nothing needs rechecking. */
typedef GLboolean (*decl_fn)(gl_program_object_t *p, const glsl_unit_t *u,
                             const glsl_node_t *n, void *arg);

static GLboolean walk_globals(gl_program_object_t *p, const glsl_unit_t *u,
                              glsl_token_type_t qualifier, decl_fn visit, void *arg) {
    if (!u || u->root == GLSL_NO_NODE) return GL_TRUE;
    for (int32_t d = u->ast.nodes[u->root].a; d != GLSL_NO_NODE; d = u->ast.nodes[d].sibling) {
        const glsl_node_t *n = &u->ast.nodes[d];
        if (n->kind != GLSL_NODE_DECL || n->qualifier != qualifier) continue;
        if (!visit(p, u, n, arg)) return GL_FALSE;
    }
    return GL_TRUE;
}

static GLboolean add_uniform(gl_program_object_t *p, const glsl_unit_t *u,
                             const glsl_node_t *n, void *arg) {
    (void)arg;
    const glsl_type_t t = glsl_type_from_token(n->type_tok);
    const int elements = decl_elements(&u->ast, n);

    /* **A uniform declared in both stages is one uniform.** The specification requires the two
     * declarations to agree and gives them a single location and a single value - which is how
     * a shared `mat4 mvp` works at all. Disagreeing is a link error, not a pair of uniforms. */
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
    /* `gl_ModelViewProjectionMatrix` and the rest are fixed-function state, not the program's
     * uniforms: the API does not list them and `glUniform` cannot set them. */
    if (is_reserved_name(n->text, n->length)) return GL_TRUE;

    if (p->uniform_count >= OOPS_GL_MAX_PROGRAM_UNIFORMS) {
        oops_snprintf(p->info_log, sizeof(p->info_log), "more than %d uniforms",
                      OOPS_GL_MAX_PROGRAM_UNIFORMS);
        return GL_FALSE;
    }
    gl_uniform_t *slot = &p->uniforms[p->uniform_count];
    if (!name_copy(slot->name, sizeof(slot->name), n->text, n->length)) {
        oops_snprintf(p->info_log, sizeof(p->info_log), "a uniform name longer than %d bytes",
                      OOPS_GL_MAX_GLSL_NAME - 1);
        return GL_FALSE;
    }
    slot->type = glsl_type_to_gl(t);
    slot->size = elements;
    slot->floats = glsl_type_floats(t);
    /* **A location per array element**, not per uniform, because the specification requires
     * `glGetUniformLocation("v[2]")` to answer and requires the elements of an array to occupy
     * consecutive locations a caller may step through (GL 2.0, 2.15.3). Numbering by uniform
     * instead would make `v[2]` collide with whatever uniform was declared next, and the collision
     * would only show in a shader that had both an array and something after it. */
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
    if (is_reserved_name(n->text, n->length)) return GL_TRUE;  /* gl_Vertex and friends */
    if (p->attrib_count >= OOPS_GL_MAX_VERTEX_ATTRIBS) {
        oops_snprintf(p->info_log, sizeof(p->info_log), "more than %d vertex attributes",
                      OOPS_GL_MAX_VERTEX_ATTRIBS);
        return GL_FALSE;
    }
    const glsl_type_t t = glsl_type_from_token(n->type_tok);
    gl_attrib_binding_t *slot = &p->attribs[p->attrib_count];
    if (!name_copy(slot->name, sizeof(slot->name), n->text, n->length)) {
        oops_snprintf(p->info_log, sizeof(p->info_log), "an attribute name longer than %d bytes",
                      OOPS_GL_MAX_GLSL_NAME - 1);
        return GL_FALSE;
    }
    slot->type = glsl_type_to_gl(t);
    slot->size = decl_elements(&u->ast, n);
    slot->location = -1;   /* assigned below, once every requested binding is known */
    p->attrib_count++;
    return GL_TRUE;
}

static GLboolean add_varying(gl_program_object_t *p, const glsl_unit_t *u,
                             const glsl_node_t *n, void *arg) {
    (void)arg;
    if (is_reserved_name(n->text, n->length)) return GL_TRUE;
    const glsl_type_t t = glsl_type_from_token(n->type_tok);
    const int elements = decl_elements(&u->ast, n);
    const int floats = glsl_type_floats(t) * elements;

    for (int i = 0; i < p->varying_count; i++) {
        if (name_eq(p->varyings[i].name, lit_len(p->varyings[i].name), n->text, n->length)) {
            if (p->varyings[i].type != glsl_type_to_gl(t) || p->varyings[i].floats != floats) {
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
        oops_snprintf(p->info_log, sizeof(p->info_log), "a varying name longer than %d bytes",
                      OOPS_GL_MAX_GLSL_NAME - 1);
        return GL_FALSE;
    }
    slot->type = glsl_type_to_gl(t);
    slot->offset = p->varying_floats;
    slot->floats = floats;
    p->varying_floats += floats;
    p->varying_count++;
    return GL_TRUE;
}

/* Every varying the fragment stage declares must be one the vertex stage declares too. The other
 * direction is fine: a vertex shader may compute more than this fragment shader reads. */
static GLboolean check_fragment_varyings(gl_program_object_t *p, const glsl_unit_t *vs,
                                         const glsl_unit_t *fs) {
    if (!vs || !fs) return GL_TRUE;
    for (int32_t d = fs->ast.nodes[fs->root].a; d != GLSL_NO_NODE;
         d = fs->ast.nodes[d].sibling) {
        const glsl_node_t *n = &fs->ast.nodes[d];
        if (n->kind != GLSL_NODE_DECL || n->qualifier != GLSL_TOK_KW_VARYING) continue;
        if (is_reserved_name(n->text, n->length)) continue;
        GLboolean seen = GL_FALSE;
        for (int32_t e = vs->ast.nodes[vs->root].a; e != GLSL_NO_NODE;
             e = vs->ast.nodes[e].sibling) {
            const glsl_node_t *m = &vs->ast.nodes[e];
            if (m->kind != GLSL_NODE_DECL || m->qualifier != GLSL_TOK_KW_VARYING) continue;
            if (name_eq(m->text, m->length, n->text, n->length)) { seen = GL_TRUE; break; }
        }
        if (!seen) {
            char name[OOPS_GL_MAX_GLSL_NAME];
            if (!name_copy(name, sizeof(name), n->text, n->length)) name[0] = '\0';
            oops_snprintf(p->info_log, sizeof(p->info_log),
                          "the fragment shader reads varying '%s', which the vertex shader does "
                          "not write", name);
            return GL_FALSE;
        }
    }
    return GL_TRUE;
}

/* A slot nothing has claimed, for an attribute the caller did not bind. */
static GLint lowest_free_slot(const gl_program_object_t *p, int need) {
    for (GLint base = 0; base + need <= OOPS_GL_MAX_VERTEX_ATTRIBS; base++) {
        GLboolean clash = GL_FALSE;
        for (int i = 0; i < p->attrib_count && !clash; i++) {
            const GLint loc = p->attribs[i].location;
            if (loc < 0) continue;
            /* A matrix attribute takes one slot per column, which is why this is a range
             * overlap rather than an equality. */
            const int span = p->attribs[i].type == GL_FLOAT_MAT2   ? 2
                             : p->attribs[i].type == GL_FLOAT_MAT3 ? 3
                             : p->attribs[i].type == GL_FLOAT_MAT4 ? 4
                                                                   : 1;
            if (base < loc + span && loc < base + need) clash = GL_TRUE;
        }
        if (!clash) return base;
    }
    return -1;
}

/* Links what is attached. Returns false with `info_log` written; the caller sets GL_LINK_STATUS.
 *
 * The program's previous link is torn down first, references and all, so a failed relink leaves
 * a program that is *not* linked rather than one still holding the previous stages - which is
 * the specification's rule (GL 2.0, 2.15.3) and the one a program that relinks on a setting
 * change depends on. */
GLboolean gl_program_link(gl_context_t *ctx, gl_program_object_t *p, glsl_unit_t *vs,
                          glsl_unit_t *fs) {
    (void)ctx;
    if (!p) return GL_FALSE;

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
    p->hw_params = 0u;
    p->hw_ps_log[0] = '\0';
    p->hw_tex_sets = 0;
    for (size_t i = 0; i < sizeof(p->hw_tex_uniform) / sizeof(p->hw_tex_uniform[0]); i++) {
        p->hw_tex_uniform[i] = -1;
    }

    if (!vs && !fs) {
        oops_snprintf(p->info_log, sizeof(p->info_log), "no shaders attached");
        return GL_FALSE;
    }
    /* **A vertex shader that never writes `gl_Position` draws nothing**, and the specification
     * leaves the result undefined rather than requiring a diagnostic. A blank screen is the
     * worst diagnostic there is, so it is one here. */
    if (vs && !assigns_to(&vs->ast, "gl_Position")) {
        oops_snprintf(p->info_log, sizeof(p->info_log),
                      "the vertex shader never writes gl_Position");
        return GL_FALSE;
    }

    if (!walk_globals(p, vs, GLSL_TOK_KW_UNIFORM, add_uniform, (void *)0)) return GL_FALSE;
    if (!walk_globals(p, fs, GLSL_TOK_KW_UNIFORM, add_uniform, (void *)0)) return GL_FALSE;
    if (!walk_globals(p, vs, GLSL_TOK_KW_ATTRIBUTE, add_attribute, (void *)0)) return GL_FALSE;
    if (!walk_globals(p, vs, GLSL_TOK_KW_VARYING, add_varying, (void *)0)) return GL_FALSE;
    if (!walk_globals(p, fs, GLSL_TOK_KW_VARYING, add_varying, (void *)0)) return GL_FALSE;
    if (!check_fragment_varyings(p, vs, fs)) return GL_FALSE;

    /* **Requested bindings first, then the rest into the lowest free slots.** A binding the
     * caller asked for that names no attribute in this shader is not an error and simply does
     * not appear - which is what lets one binding table serve several programs. */
    for (int i = 0; i < p->attrib_count; i++) {
        for (int b = 0; b < p->binding_count; b++) {
            if (name_eq(p->attribs[i].name, lit_len(p->attribs[i].name), p->bindings[b].name,
                        lit_len(p->bindings[b].name))) {
                p->attribs[i].location = p->bindings[b].location;
                break;
            }
        }
    }
    for (int i = 0; i < p->attrib_count; i++) {
        if (p->attribs[i].location >= 0) continue;
        const int span = p->attribs[i].type == GL_FLOAT_MAT2   ? 2
                         : p->attribs[i].type == GL_FLOAT_MAT3 ? 3
                         : p->attribs[i].type == GL_FLOAT_MAT4 ? 4
                                                               : 1;
        const GLint slot = lowest_free_slot(p, span);
        if (slot < 0) {
            oops_snprintf(p->info_log, sizeof(p->info_log),
                          "the attributes do not fit in %d slots", OOPS_GL_MAX_VERTEX_ATTRIBS);
            return GL_FALSE;
        }
        p->attribs[i].location = slot;
    }

    /* The uniform values start at zero, which is what the specification says a freshly linked
     * program's uniforms hold - and, for a sampler, is texture unit 0. A relink resets them,
     * which is also the rule and is the thing programs forget. */
    if (p->value_floats > 0) {
        p->values = (float *)gl_heap_alloc((size_t)p->value_floats * sizeof(float));
        if (!p->values) {
            oops_snprintf(p->info_log, sizeof(p->info_log), "out of memory");
            return GL_FALSE;
        }
        for (int i = 0; i < p->value_floats; i++) p->values[i] = 0.0f;
    }

    p->vs = vs;
    p->fs = fs;
    gl_glsl_unit_retain(vs);
    gl_glsl_unit_retain(fs);
    p->linked = GL_TRUE;

    /* -----------------------------------------------------------------
     * The console's half, compiled here because this is the last moment that knows the whole
     * program - and because a draw is the wrong place to ask for a quarter of a megabyte of
     * compiler scratch.
     *
     * **A failure here does not fail the link.** The program still draws on the software path,
     * which is the reference; what it cannot do is draw on a console, and `hw_ps_log` is what
     * the draw path says once when it refuses. Failing the link instead would make
     * GL_LINK_STATUS answer differently on a host and on hardware, which is the one divergence
     * a probe could never see past.
     * ----------------------------------------------------------------- */
    /* **Which sampler gets which descriptor set**, in the order the fragment shader declares
     * them - decided here because the compiled shader and the draw path both have to agree on
     * it, and one decision is the only way to be sure they do. Only the fragment shader's
     * samplers get a set: a vertex shader cannot sample here. */
    if (fs) {
        const int max_sets =
            (int)(sizeof(p->hw_tex_uniform) / sizeof(p->hw_tex_uniform[0]));
        for (int32_t d = fs->ast.nodes[fs->root].a;
             d != GLSL_NO_NODE && p->hw_tex_sets < max_sets;
             d = fs->ast.nodes[d].sibling) {
            const glsl_node_t *n = &fs->ast.nodes[d];
            if (n->kind != GLSL_NODE_DECL || n->qualifier != GLSL_TOK_KW_UNIFORM) continue;
            for (int i = 0; i < p->uniform_count; i++) {
                if (p->uniforms[i].type != GL_SAMPLER_2D) continue;
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
    if (p->hw_params < 2u) p->hw_params = 2u;   /* the pipeline's smallest configuration */
    if (fs) {
        p->hw_ps = (uint32_t *)gl_heap_alloc(OOPS_GL_PS_GL2_WORDS * sizeof(uint32_t));
        if (!p->hw_ps) {
            oops_snprintf(p->hw_ps_log, sizeof(p->hw_ps_log),
                          "no memory for a compiled pixel shader");
        } else if (!gl_program_compile_fragment(p, p->hw_ps, OOPS_GL_PS_GL2_WORDS,
                                                &p->hw_ps_words, &p->hw_ps_vgprs,
                                                &p->hw_ps_user_sgprs, p->hw_ps_log,
                                                sizeof(p->hw_ps_log))) {
            gl_heap_free(p->hw_ps);
            p->hw_ps = (uint32_t *)0;
            p->hw_ps_words = 0u;
            p->hw_ps_user_sgprs = 0u;
        }
    } else {
        /* No fragment stage: the fixed-function pixel shader in the payload is what runs, and
         * there is nothing to compile or to refuse. */
        p->hw_ps_log[0] = '\0';
    }
    return GL_TRUE;
}
