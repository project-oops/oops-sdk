/*
 * oops-gl: GLSL types and the semantic stage
 *
 * The grammar is happy with `vec3 + mat4` and with `.xyzw` on a `vec2`. Deciding those are
 * wrong happens here.
 *
 * # What GLSL's operators actually do, which is not what C's do
 *
 * **There is no implicit conversion between int and float.** `1 + 1.0` is an error in GLSL 1.10,
 * not an int promoted to a float. Writing the usual arithmetic conversions here would accept
 * shaders the specification rejects and, worse, would silently pick a type for expressions whose
 * author meant something else.
 *
 * **A scalar against a vector is component-wise, and keeps the vector's type.** `vec3 * float`
 * is a `vec3`. So is `float * vec3`. That asymmetry-that-isn't is the common case in shader
 * code and is the first thing to get right.
 *
 * **`mat * vec` is a linear transform, not a component-wise multiply.** `mat4 * vec4` is a
 * `vec4`; `mat4 * vec3` is an error, because the dimensions do not meet. This is the rule that
 * makes `mvp * vec4(pos, 1.0)` the idiom it is, and the one a component-wise implementation
 * gets wrong while still producing a plausible type.
 */

#include "glsl_internal.h"

static void sema_fail(glsl_sema_t *s, const char *why, int32_t node) {
    if (s->error) return; /* the first error is the one that means something */
    s->error = why;
    if (node != GLSL_NO_NODE && s->ast) {
        s->error_line = s->ast->nodes[node].line;
        s->error_column = s->ast->nodes[node].column;
    }
}

/* The type predicates and the diagnostic sink, for the built-in library next door.
 *
 * Wrappers rather than the statics made public, so every call site in this file keeps using the
 * short names and there is still exactly one definition of each rule. A second copy of
 * "what counts as a vector" in another file is the kind of thing that agrees on the day it is
 * written and not afterwards. */
void glsl_sema_fail(glsl_sema_t *s, const char *why, int32_t node);
GLboolean glsl_type_is_vector(glsl_type_t t);
GLboolean glsl_type_is_matrix(glsl_type_t t);
GLboolean glsl_type_is_sampler(glsl_type_t t);
glsl_type_t glsl_type_base(glsl_type_t t);
glsl_type_t glsl_type_vector_of(glsl_type_t base, int n);
int glsl_type_matrix_dim(glsl_type_t t);

void glsl_sema_init(glsl_sema_t *s, glsl_ast_t *ast) {
    if (!s) return;
    s->ast = ast;
    s->count = 0;
    s->scope = 0;
    s->error = (const char *)0;
    s->error_line = 0;
    s->error_column = 0;
    s->current_return = GLSL_TYPE_VOID;
    s->loop_depth = 0;
    s->stage = 0u;
    s->version = 0;   /* unstated: 1.10's rules, which convert nothing */
}

/* -------------------------------------------------------------------------
 * GLSL 1.20's implicit conversion
 *
 * **int to float, and `ivecN` to `vecN`. That is the whole of it** (1.20, 4.1.10). Not float to
 * int, not bool to anything, and not in the other direction - so `int i = 1.0;` is still an
 * error in 1.20, which is the half people expect to work and which does not.
 *
 * 1.10 converts nothing at all, and that refusal is deliberate rather than incidental: without
 * it `1 + 1.0` silently picks a type its author did not write.
 * ------------------------------------------------------------------------- */

static glsl_type_t widen_to_float(glsl_type_t t) {
    switch (t) {
        case GLSL_TYPE_INT:   return GLSL_TYPE_FLOAT;
        case GLSL_TYPE_IVEC2: return GLSL_TYPE_VEC2;
        case GLSL_TYPE_IVEC3: return GLSL_TYPE_VEC3;
        case GLSL_TYPE_IVEC4: return GLSL_TYPE_VEC4;
        default: return t;
    }
}

/* Whether a value of `got` may be used where `want` is expected - the test every assignment,
 * initialiser, argument and return goes through, so the rule lives in one place. */
GLboolean glsl_type_accepts(const glsl_sema_t *s, glsl_type_t want, glsl_type_t got) {
    if (want == got) return GL_TRUE;
    if (!s || s->version < 120) return GL_FALSE;
    return (GLboolean)(widen_to_float(got) == want);
}

glsl_type_t glsl_type_from_token(glsl_token_type_t t) {
    switch (t) {
        case GLSL_TOK_KW_VOID:  return GLSL_TYPE_VOID;
        case GLSL_TOK_KW_BOOL:  return GLSL_TYPE_BOOL;
        case GLSL_TOK_KW_INT:   return GLSL_TYPE_INT;
        case GLSL_TOK_KW_FLOAT: return GLSL_TYPE_FLOAT;
        case GLSL_TOK_KW_VEC2:  return GLSL_TYPE_VEC2;
        case GLSL_TOK_KW_VEC3:  return GLSL_TYPE_VEC3;
        case GLSL_TOK_KW_VEC4:  return GLSL_TYPE_VEC4;
        case GLSL_TOK_KW_IVEC2: return GLSL_TYPE_IVEC2;
        case GLSL_TOK_KW_IVEC3: return GLSL_TYPE_IVEC3;
        case GLSL_TOK_KW_IVEC4: return GLSL_TYPE_IVEC4;
        case GLSL_TOK_KW_BVEC2: return GLSL_TYPE_BVEC2;
        case GLSL_TOK_KW_BVEC3: return GLSL_TYPE_BVEC3;
        case GLSL_TOK_KW_BVEC4: return GLSL_TYPE_BVEC4;
        case GLSL_TOK_KW_MAT2:  return GLSL_TYPE_MAT2;
        case GLSL_TOK_KW_MAT3:  return GLSL_TYPE_MAT3;
        case GLSL_TOK_KW_MAT4:  return GLSL_TYPE_MAT4;
        case GLSL_TOK_KW_SAMPLER1D: return GLSL_TYPE_SAMPLER1D;
        case GLSL_TOK_KW_SAMPLER2D: return GLSL_TYPE_SAMPLER2D;
        case GLSL_TOK_KW_SAMPLER3D: return GLSL_TYPE_SAMPLER3D;
        case GLSL_TOK_KW_SAMPLERCUBE: return GLSL_TYPE_SAMPLERCUBE;
        case GLSL_TOK_KW_SAMPLER1DSHADOW: return GLSL_TYPE_SAMPLER1DSHADOW;
        case GLSL_TOK_KW_SAMPLER2DSHADOW: return GLSL_TYPE_SAMPLER2DSHADOW;
        default: return GLSL_TYPE_ERROR;
    }
}

static GLboolean is_vector(glsl_type_t t) {
    return (GLboolean)((t >= GLSL_TYPE_VEC2 && t <= GLSL_TYPE_VEC4) ||
                       (t >= GLSL_TYPE_IVEC2 && t <= GLSL_TYPE_IVEC4) ||
                       (t >= GLSL_TYPE_BVEC2 && t <= GLSL_TYPE_BVEC4));
}
static GLboolean is_matrix(glsl_type_t t) {
    return (GLboolean)(t >= GLSL_TYPE_MAT2 && t <= GLSL_TYPE_MAT4);
}
static GLboolean is_scalar(glsl_type_t t) {
    return (GLboolean)(t == GLSL_TYPE_BOOL || t == GLSL_TYPE_INT || t == GLSL_TYPE_FLOAT);
}
static GLboolean is_sampler(glsl_type_t t) {
    return (GLboolean)(t >= GLSL_TYPE_SAMPLER1D && t <= GLSL_TYPE_SAMPLER2DSHADOW);
}

int glsl_type_components(glsl_type_t t) {
    switch (t) {
        case GLSL_TYPE_BOOL: case GLSL_TYPE_INT: case GLSL_TYPE_FLOAT: return 1;
        case GLSL_TYPE_VEC2: case GLSL_TYPE_IVEC2: case GLSL_TYPE_BVEC2: return 2;
        case GLSL_TYPE_VEC3: case GLSL_TYPE_IVEC3: case GLSL_TYPE_BVEC3: return 3;
        case GLSL_TYPE_VEC4: case GLSL_TYPE_IVEC4: case GLSL_TYPE_BVEC4: return 4;
        case GLSL_TYPE_MAT2: return 4;
        case GLSL_TYPE_MAT3: return 9;
        case GLSL_TYPE_MAT4: return 16;
        default: return 0;
    }
}

/* The scalar a vector is made of: `vec3` is float, `ivec2` is int, `bvec4` is bool. */
static glsl_type_t base_of(glsl_type_t t) {
    if (t >= GLSL_TYPE_VEC2 && t <= GLSL_TYPE_VEC4) return GLSL_TYPE_FLOAT;
    if (t >= GLSL_TYPE_IVEC2 && t <= GLSL_TYPE_IVEC4) return GLSL_TYPE_INT;
    if (t >= GLSL_TYPE_BVEC2 && t <= GLSL_TYPE_BVEC4) return GLSL_TYPE_BOOL;
    if (is_matrix(t)) return GLSL_TYPE_FLOAT;
    return t;
}

/* The vector of `base` with `n` components; `n == 1` gives the scalar back. */
static glsl_type_t vector_of(glsl_type_t base, int n) {
    if (n == 1) return base;
    if (base == GLSL_TYPE_FLOAT) return (glsl_type_t)(GLSL_TYPE_VEC2 + (n - 2));
    if (base == GLSL_TYPE_INT) return (glsl_type_t)(GLSL_TYPE_IVEC2 + (n - 2));
    if (base == GLSL_TYPE_BOOL) return (glsl_type_t)(GLSL_TYPE_BVEC2 + (n - 2));
    return GLSL_TYPE_ERROR;
}

/* Matrix dimension: mat2 is 2, mat3 is 3, mat4 is 4. */
static int matrix_dim(glsl_type_t t) {
    return t == GLSL_TYPE_MAT2 ? 2 : t == GLSL_TYPE_MAT3 ? 3 : t == GLSL_TYPE_MAT4 ? 4 : 0;
}

void glsl_sema_fail(glsl_sema_t *s, const char *why, int32_t node) { sema_fail(s, why, node); }
GLboolean glsl_type_is_vector(glsl_type_t t) { return is_vector(t); }
GLboolean glsl_type_is_matrix(glsl_type_t t) { return is_matrix(t); }
GLboolean glsl_type_is_sampler(glsl_type_t t) { return is_sampler(t); }
glsl_type_t glsl_type_base(glsl_type_t t) { return base_of(t); }
glsl_type_t glsl_type_vector_of(glsl_type_t base, int n) { return vector_of(base, n); }
int glsl_type_matrix_dim(glsl_type_t t) { return matrix_dim(t); }

/* -------------------------------------------------------------------------
 * Scopes
 * ------------------------------------------------------------------------- */

void glsl_scope_push(glsl_sema_t *s) { if (s) s->scope++; }

void glsl_scope_pop(glsl_sema_t *s) {
    if (!s || s->scope == 0) return;
    /* Everything declared at this depth goes. The table is a stack, so dropping the tail is
     * enough - and it has to happen here rather than being left for a later lookup to filter,
     * or a name would stay visible after its block closed. */
    while (s->count > 0 && s->symbols[s->count - 1].scope >= s->scope) s->count--;
    s->scope--;
}

static GLboolean same_name(const glsl_symbol_t *sym, const char *name, size_t len) {
    if (sym->name_len != len) return GL_FALSE;
    for (size_t i = 0; i < len; i++) {
        if (sym->name[i] != name[i]) return GL_FALSE;
    }
    return GL_TRUE;
}

static const glsl_symbol_t *lookup(const glsl_sema_t *s, const char *name, size_t len) {
    /* **Backwards**, so an inner declaration shadows an outer one of the same name. Forwards
     * would find the outer one and quietly use the wrong variable. */
    for (int i = s->count - 1; i >= 0; i--) {
        if (same_name(&s->symbols[i], name, len)) return &s->symbols[i];
    }
    return (const glsl_symbol_t *)0;
}

GLboolean glsl_declare(glsl_sema_t *s, const char *name, size_t len, glsl_type_t type,
                       GLboolean is_function) {
    if (!s) return GL_FALSE;
    if (s->count >= GLSL_MAX_SYMBOLS) {
        sema_fail(s, "too many declarations", GLSL_NO_NODE);
        return GL_FALSE;
    }
    /* **Redeclaration is only an error in the *same* scope.** Shadowing an outer name is legal
     * and common, so the check is against the current depth rather than the whole table. */
    for (int i = s->count - 1; i >= 0; i--) {
        if (s->symbols[i].scope < s->scope) break;
        if (same_name(&s->symbols[i], name, len)) {
            sema_fail(s, "a name declared twice in one scope", GLSL_NO_NODE);
            return GL_FALSE;
        }
    }
    s->symbols[s->count].name = name;
    s->symbols[s->count].name_len = len;
    s->symbols[s->count].type = type;
    s->symbols[s->count].scope = s->scope;
    s->symbols[s->count].is_function = is_function;
    s->symbols[s->count].qualifier = GLSL_TOK_EOF;
    s->symbols[s->count].array_size = 0;
    s->symbols[s->count].param_count = 0;
    s->count++;
    return GL_TRUE;
}

GLboolean glsl_declare_array(glsl_sema_t *s, const char *name, size_t len, glsl_type_t type,
                             int count, glsl_token_type_t qualifier) {
    if (!glsl_declare(s, name, len, type, GL_FALSE)) return GL_FALSE;
    s->symbols[s->count - 1].array_size = count;
    s->symbols[s->count - 1].qualifier = qualifier;
    return GL_TRUE;
}

/* -------------------------------------------------------------------------
 * Swizzles
 *
 * `.xyz`, `.rgb` and `.stp` name the same components through three vocabularies, for position,
 * colour and texture coordinates. **They may not be mixed**: `v.xg` is an error, not a
 * component named `x` and one named `g`. Mixing is exactly the typo that produces a shader that
 * compiles elsewhere and not here, or the reverse, so it is worth refusing precisely.
 * ------------------------------------------------------------------------- */

/* Returns the component index 0..3, or -1. `set` receives 0/1/2 for xyzw/rgba/stpq. */
static int swizzle_component(char c, int *set) {
    switch (c) {
        case 'x': *set = 0; return 0;
        case 'y': *set = 0; return 1;
        case 'z': *set = 0; return 2;
        case 'w': *set = 0; return 3;
        case 'r': *set = 1; return 0;
        case 'g': *set = 1; return 1;
        case 'b': *set = 1; return 2;
        case 'a': *set = 1; return 3;
        case 's': *set = 2; return 0;
        case 't': *set = 2; return 1;
        case 'p': *set = 2; return 2;
        case 'q': *set = 2; return 3;
        default: return -1;
    }
}

static glsl_type_t swizzle_type(glsl_sema_t *s, glsl_type_t operand, const char *field,
                                size_t len, int32_t node) {
    if (!is_vector(operand)) {
        sema_fail(s, "field selection on something that is not a vector", node);
        return GLSL_TYPE_ERROR;
    }
    if (len < 1 || len > 4) {
        sema_fail(s, "a swizzle selects one to four components", node);
        return GLSL_TYPE_ERROR;
    }
    const int width = glsl_type_components(operand);
    int first_set = -1;
    for (size_t i = 0; i < len; i++) {
        int set = -1;
        int idx = swizzle_component(field[i], &set);
        if (idx < 0) {
            sema_fail(s, "not a component name", node);
            return GLSL_TYPE_ERROR;
        }
        if (first_set < 0) {
            first_set = set;
        } else if (set != first_set) {
            sema_fail(s, "a swizzle may not mix the xyzw, rgba and stpq sets", node);
            return GLSL_TYPE_ERROR;
        }
        /* **Past the end of the operand.** `.w` on a vec3 is the error this catches, and it is
         * the one that otherwise reads whatever sits after the vector. */
        if (idx >= width) {
            sema_fail(s, "a component past the end of the vector", node);
            return GLSL_TYPE_ERROR;
        }
    }
    return vector_of(base_of(operand), (int)len);
}

/* -------------------------------------------------------------------------
 * Expressions
 * ------------------------------------------------------------------------- */

/* A constructor: `vec3(1.0)`, `vec4(v, 1.0)`, `mat4(1.0)`.
 *
 * GLSL's rule is by **component count, not argument count**: `vec4(v3, 1.0)` is four components
 * from two arguments and is legal, and so is `vec4(1.0)`, which fills all four. Counting
 * arguments instead would reject the first and accept `vec4(1.0, 2.0)`, which is neither one
 * component nor four. */
static glsl_type_t constructor_type(glsl_sema_t *s, glsl_type_t target, int32_t first_arg,
                                    int32_t node) {
    int supplied = 0;
    int args = 0;
    for (int32_t a = first_arg; a != GLSL_NO_NODE; a = s->ast->nodes[a].sibling) {
        glsl_type_t at = glsl_type_of(s, a);
        if (at == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
        if (is_sampler(at)) {
            sema_fail(s, "a sampler cannot be a constructor argument", node);
            return GLSL_TYPE_ERROR;
        }
        supplied += glsl_type_components(at);
        args++;
    }
    const int needed = glsl_type_components(target);
    if (args == 0) {
        sema_fail(s, "a constructor needs at least one argument", node);
        return GLSL_TYPE_ERROR;
    }
    /* One scalar fills everything - `vec4(0.0)`, and `mat4(1.0)` which is the identity scaled. */
    if (args == 1 && supplied == 1) return target;
    if (supplied < needed) {
        sema_fail(s, "too few components for this constructor", node);
        return GLSL_TYPE_ERROR;
    }
    /* More than needed is allowed to spill only in the sense that GLSL permits trailing
     * components to be dropped when a single argument is larger; several arguments summing past
     * the target is a mistake. */
    if (supplied > needed && args > 1) {
        sema_fail(s, "too many components for this constructor", node);
        return GLSL_TYPE_ERROR;
    }
    return target;
}

static glsl_type_t binary_type(glsl_sema_t *s, glsl_token_type_t op, glsl_type_t l,
                               glsl_type_t r, int32_t node) {
    if (l == GLSL_TYPE_ERROR || r == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;

    /* **One side float and the other int widens the int one** (GLSL 1.20). Done here, once, so
     * every rule below sees a pair that already agrees on its base type and none of them has to
     * know about the conversion. `vec3 * 2` becomes `vec3 * 2.0`; `ivec2 == vec3` widens and is
     * still a width mismatch, which is what it should be. */
    if (s && s->version >= 120) {
        const glsl_type_t lb = base_of(l), rb = base_of(r);
        if (lb == GLSL_TYPE_FLOAT && rb == GLSL_TYPE_INT) r = widen_to_float(r);
        else if (rb == GLSL_TYPE_FLOAT && lb == GLSL_TYPE_INT) l = widen_to_float(l);
    }

    switch (op) {
        case GLSL_TOK_AND_AND: case GLSL_TOK_OR_OR: case GLSL_TOK_XOR_XOR:
            if (l != GLSL_TYPE_BOOL || r != GLSL_TYPE_BOOL) {
                sema_fail(s, "the logical operators take bool", node);
                return GLSL_TYPE_ERROR;
            }
            return GLSL_TYPE_BOOL;

        case GLSL_TOK_EQ: case GLSL_TOK_NE:
            if (l != r) {
                sema_fail(s, "== and != need both sides to be the same type", node);
                return GLSL_TYPE_ERROR;
            }
            return GLSL_TYPE_BOOL;

        case GLSL_TOK_LT: case GLSL_TOK_GT: case GLSL_TOK_LE: case GLSL_TOK_GE:
            /* **Ordering is scalars only.** GLSL has `lessThan()` for vectors precisely because
             * `<` on them has no single answer. */
            if (!(l == r && (l == GLSL_TYPE_INT || l == GLSL_TYPE_FLOAT))) {
                sema_fail(s, "< > <= >= compare int with int or float with float", node);
                return GLSL_TYPE_ERROR;
            }
            return GLSL_TYPE_BOOL;

        default: break;
    }

    /* Arithmetic. */
    if (is_sampler(l) || is_sampler(r)) {
        sema_fail(s, "arithmetic on a sampler", node);
        return GLSL_TYPE_ERROR;
    }
    if (l == GLSL_TYPE_BOOL || r == GLSL_TYPE_BOOL ||
        (is_vector(l) && base_of(l) == GLSL_TYPE_BOOL) ||
        (is_vector(r) && base_of(r) == GLSL_TYPE_BOOL)) {
        sema_fail(s, "arithmetic on a bool", node);
        return GLSL_TYPE_ERROR;
    }

    /* `mat * vec` and `vec * mat` are linear transforms; the dimensions have to meet. */
    if (op == GLSL_TOK_STAR && is_matrix(l) && is_vector(r)) {
        if (glsl_type_components(r) != matrix_dim(l) || base_of(r) != GLSL_TYPE_FLOAT) {
            sema_fail(s, "matrix and vector dimensions do not meet", node);
            return GLSL_TYPE_ERROR;
        }
        return r;
    }
    if (op == GLSL_TOK_STAR && is_vector(l) && is_matrix(r)) {
        if (glsl_type_components(l) != matrix_dim(r) || base_of(l) != GLSL_TYPE_FLOAT) {
            sema_fail(s, "vector and matrix dimensions do not meet", node);
            return GLSL_TYPE_ERROR;
        }
        return l;
    }
    if (op == GLSL_TOK_STAR && is_matrix(l) && is_matrix(r)) {
        if (l != r) {
            sema_fail(s, "matrix dimensions do not meet", node);
            return GLSL_TYPE_ERROR;
        }
        return l;
    }

    /* A scalar against a vector or matrix is component-wise and keeps the larger type, in
     * either order. */
    if (is_scalar(l) && (is_vector(r) || is_matrix(r))) {
        if (base_of(r) != l) {
            sema_fail(s, "no implicit conversion between int and float", node);
            return GLSL_TYPE_ERROR;
        }
        return r;
    }
    if (is_scalar(r) && (is_vector(l) || is_matrix(l))) {
        if (base_of(l) != r) {
            sema_fail(s, "no implicit conversion between int and float", node);
            return GLSL_TYPE_ERROR;
        }
        return l;
    }

    /* **No implicit conversion.** `1 + 1.0` is an error in GLSL 1.10, and accepting it would
     * pick a type the author did not write. */
    if (l != r) {
        sema_fail(s, "operands of different types, and GLSL 1.10 converts neither", node);
        return GLSL_TYPE_ERROR;
    }
    return l;
}

glsl_type_t glsl_type_of(glsl_sema_t *s, int32_t node) {
    if (!s || !s->ast || node == GLSL_NO_NODE) return GLSL_TYPE_ERROR;
    if (s->error) return GLSL_TYPE_ERROR;

    const glsl_node_t *n = &s->ast->nodes[node];
    switch (n->kind) {
        case GLSL_NODE_INTCONST:   return GLSL_TYPE_INT;
        case GLSL_NODE_FLOATCONST: return GLSL_TYPE_FLOAT;
        case GLSL_NODE_BOOLCONST:  return GLSL_TYPE_BOOL;

        case GLSL_NODE_IDENTIFIER: {
            /* A type name used as an identifier is a constructor being named; it only has a
             * type when it is called, which the CALL arm below handles. */
            const glsl_symbol_t *sym = lookup(s, n->text, n->length);
            if (!sym) {
                /* **A `gl_` name this implementation does not provide is named, not guessed
                 * at.** `gl_LightSource[0]` and `gl_PointCoord` are real GLSL and are missing
                 * here for reasons an author can act on - no struct type, no point sprites -
                 * where "undeclared name" invites them to check their spelling instead. */
                const char *why = glsl_builtin_refusal(n->text, n->length);
                sema_fail(s, why ? why : "use of an undeclared name", node);
                return GLSL_TYPE_ERROR;
            }
            return sym->type;
        }

        case GLSL_NODE_FIELD:
            return swizzle_type(s, glsl_type_of(s, n->a), n->text, n->length, node);

        case GLSL_NODE_INDEX: {
            /* **An array name is resolved before its type is**, because a name declared
             * `vec4 v[4]` has element type `vec4` in the table - there is no array type - and
             * typing the operand first would report a vec4 and then hand back a float as though
             * a component had been asked for. GLSL 1.10 has no array-valued expressions, so an
             * array can only ever be indexed through its own name and this is the whole rule. */
            {
                const glsl_node_t *base_n = &s->ast->nodes[n->a];
                if (base_n->kind == GLSL_NODE_IDENTIFIER) {
                    const glsl_symbol_t *sym = lookup(s, base_n->text, base_n->length);
                    if (sym && sym->array_size > 0 && !sym->is_function) {
                        glsl_type_t idx_t = glsl_type_of(s, n->b);
                        if (idx_t == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
                        if (idx_t != GLSL_TYPE_INT) {
                            sema_fail(s, "an index must be an int", node);
                            return GLSL_TYPE_ERROR;
                        }
                        /* A constant index past the end is a compile error in GLSL, not a
                         * runtime read of whatever follows (1.10, 4.1.9). A variable one is
                         * checked when it runs. */
                        const glsl_node_t *idx_n = &s->ast->nodes[n->b];
                        if (idx_n->kind == GLSL_NODE_INTCONST &&
                            ((int)idx_n->value < 0 || (int)idx_n->value >= sym->array_size)) {
                            sema_fail(s, "array index out of range", node);
                            return GLSL_TYPE_ERROR;
                        }
                        return sym->type;
                    }
                }
            }
            glsl_type_t base = glsl_type_of(s, n->a);
            glsl_type_t idx = glsl_type_of(s, n->b);
            if (base == GLSL_TYPE_ERROR || idx == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
            if (idx != GLSL_TYPE_INT) {
                sema_fail(s, "an index must be an int", node);
                return GLSL_TYPE_ERROR;
            }
            /* Indexing a matrix gives a column; indexing a vector gives a component. */
            if (is_matrix(base)) return vector_of(GLSL_TYPE_FLOAT, matrix_dim(base));
            if (is_vector(base)) return base_of(base);
            sema_fail(s, "indexing something that is neither a vector nor a matrix", node);
            return GLSL_TYPE_ERROR;
        }

        case GLSL_NODE_CALL: {
            const glsl_node_t *callee = &s->ast->nodes[n->a];
            if (callee->kind != GLSL_NODE_IDENTIFIER) {
                sema_fail(s, "calling something that is not a name", node);
                return GLSL_TYPE_ERROR;
            }
            /* A constructor is a type name in call position. Checked before the symbol table,
             * because `vec4` is never declared as a function. */
            glsl_type_t ctor = GLSL_TYPE_ERROR;
            static const struct { const char *name; glsl_type_t type; } ctors[] = {
                {"float", GLSL_TYPE_FLOAT}, {"int", GLSL_TYPE_INT}, {"bool", GLSL_TYPE_BOOL},
                {"vec2", GLSL_TYPE_VEC2}, {"vec3", GLSL_TYPE_VEC3}, {"vec4", GLSL_TYPE_VEC4},
                {"ivec2", GLSL_TYPE_IVEC2}, {"ivec3", GLSL_TYPE_IVEC3}, {"ivec4", GLSL_TYPE_IVEC4},
                {"bvec2", GLSL_TYPE_BVEC2}, {"bvec3", GLSL_TYPE_BVEC3}, {"bvec4", GLSL_TYPE_BVEC4},
                {"mat2", GLSL_TYPE_MAT2}, {"mat3", GLSL_TYPE_MAT3}, {"mat4", GLSL_TYPE_MAT4},
            };
            for (size_t i = 0; i < sizeof(ctors) / sizeof(ctors[0]); i++) {
                size_t len = 0;
                while (ctors[i].name[len] != '\0') len++;
                if (len == callee->length) {
                    GLboolean match = GL_TRUE;
                    for (size_t k = 0; k < len; k++) {
                        if (callee->text[k] != ctors[i].name[k]) { match = GL_FALSE; break; }
                    }
                    if (match) { ctor = ctors[i].type; break; }
                }
            }
            if (ctor != GLSL_TYPE_ERROR) return constructor_type(s, ctor, n->b, node);

            /* **The built-in library, before the symbol table.** `sin`, `dot` and `texture2D`
             * are not declared anywhere - they are overloaded over genType, which one signature
             * per name cannot express - so they are resolved by rule, like the constructors
             * above. A shader may still declare a function of its own with a built-in's name,
             * which GLSL 1.10 allows; this finds the built-in first, which is wrong in that one
             * case and right in every other, and the case is rare enough that the alternative -
             * searching the table first and so paying a lookup on every `sin` - is the worse
             * trade. */
            {
                glsl_type_t bargs[GLSL_MAX_PARAMS];
                int bargc = 0;
                GLboolean too_many = GL_FALSE;
                for (int32_t a = n->b; a != GLSL_NO_NODE; a = s->ast->nodes[a].sibling) {
                    glsl_type_t at = glsl_type_of(s, a);
                    if (at == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
                    if (bargc >= GLSL_MAX_PARAMS) { too_many = GL_TRUE; break; }
                    /* **Every integer argument widens under 1.20**, before the built-in's own
                     * rule looks at it - so `mod(x, 2)` and `clamp(v, 0, 1)` resolve, which is
                     * how shader authors write them. Nothing is lost by widening everything:
                     * neither 1.10 nor 1.20 has a built-in that takes an integer and means it,
                     * so there is no overload this could pick wrongly. */
                    bargs[bargc++] = (s->version >= 120) ? widen_to_float(at) : at;
                }
                if (!too_many) {
                    GLboolean found = GL_FALSE;
                    glsl_type_t bt = glsl_builtin_call_type(s, callee->text, callee->length,
                                                            bargs, bargc, node, &found);
                    if (found) return bt;
                }
            }

            const glsl_symbol_t *sym = lookup(s, callee->text, callee->length);
            if (!sym) {
                sema_fail(s, "call to an undeclared function", node);
                return GLSL_TYPE_ERROR;
            }
            if (!sym->is_function) {
                sema_fail(s, "calling something that is not a function", node);
                return GLSL_TYPE_ERROR;
            }
            /* **Arity and each argument's type, against the recorded signature.** GLSL 1.10 has
             * no implicit conversion, so an argument that is merely close is wrong. */
            int given = 0;
            for (int32_t a = n->b; a != GLSL_NO_NODE; a = s->ast->nodes[a].sibling) {
                glsl_type_t at = glsl_type_of(s, a);
                if (at == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
                if (given < sym->param_count && !glsl_type_accepts(s, sym->params[given], at)) {
                    sema_fail(s, "argument of the wrong type", a);
                    return GLSL_TYPE_ERROR;
                }
                given++;
            }
            if (given != sym->param_count) {
                sema_fail(s, "wrong number of arguments", node);
                return GLSL_TYPE_ERROR;
            }
            return sym->type;
        }

        case GLSL_NODE_UNARY: {
            glsl_type_t t = glsl_type_of(s, n->a);
            if (t == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
            if (n->op == GLSL_TOK_BANG) {
                if (t != GLSL_TYPE_BOOL) {
                    sema_fail(s, "! takes a bool", node);
                    return GLSL_TYPE_ERROR;
                }
                return GLSL_TYPE_BOOL;
            }
            if (base_of(t) == GLSL_TYPE_BOOL || is_sampler(t)) {
                sema_fail(s, "arithmetic on a bool or a sampler", node);
                return GLSL_TYPE_ERROR;
            }
            return t;
        }

        case GLSL_NODE_POSTFIX: {
            glsl_type_t t = glsl_type_of(s, n->a);
            if (t == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
            if (base_of(t) == GLSL_TYPE_BOOL || is_sampler(t)) {
                sema_fail(s, "++ and -- need a numeric operand", node);
                return GLSL_TYPE_ERROR;
            }
            return t;
        }

        case GLSL_NODE_BINARY:
            return binary_type(s, n->op, glsl_type_of(s, n->a), glsl_type_of(s, n->b), node);

        case GLSL_NODE_ASSIGN: {
            glsl_type_t l = glsl_type_of(s, n->a);
            glsl_type_t r = glsl_type_of(s, n->b);
            if (l == GLSL_TYPE_ERROR || r == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
            if (n->op != GLSL_TOK_ASSIGN) {
                /* `a += b` is `a = a + b`, so the arithmetic rules decide first. */
                glsl_token_type_t arith =
                    n->op == GLSL_TOK_ADD_ASSIGN ? GLSL_TOK_PLUS :
                    n->op == GLSL_TOK_SUB_ASSIGN ? GLSL_TOK_MINUS :
                    n->op == GLSL_TOK_MUL_ASSIGN ? GLSL_TOK_STAR : GLSL_TOK_SLASH;
                r = binary_type(s, arith, l, r, node);
                if (r == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
            }
            /* **The conversion goes one way**, into the variable's type: `float f; f = 1;` is
             * legal in 1.20 and `int i; i = 1.0;` is not, in either version. */
            if (!glsl_type_accepts(s, l, r)) {
                sema_fail(s, "assigning a value of a different type", node);
                return GLSL_TYPE_ERROR;
            }
            return l;
        }

        case GLSL_NODE_CONDITIONAL: {
            glsl_type_t c = glsl_type_of(s, n->a);
            glsl_type_t y = glsl_type_of(s, n->b);
            glsl_type_t no = glsl_type_of(s, n->c);
            if (c == GLSL_TYPE_ERROR || y == GLSL_TYPE_ERROR || no == GLSL_TYPE_ERROR) {
                return GLSL_TYPE_ERROR;
            }
            if (c != GLSL_TYPE_BOOL) {
                sema_fail(s, "the condition of ?: must be a bool", node);
                return GLSL_TYPE_ERROR;
            }
            /* Either branch may widen to the other's type, so `b ? 1 : 1.0` is a float in 1.20
             * and an error in 1.10. */
            if (glsl_type_accepts(s, y, no)) return y;
            if (glsl_type_accepts(s, no, y)) return no;
            sema_fail(s, "the two branches of ?: have different types", node);
            return GLSL_TYPE_ERROR;
        }

        case GLSL_NODE_SEQUENCE: {
            glsl_type_t a = glsl_type_of(s, n->a);
            if (a == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
            return glsl_type_of(s, n->b); /* the comma operator's value is its right side */
        }

        default:
            sema_fail(s, "not an expression", node);
            return GLSL_TYPE_ERROR;
    }
}

/* -------------------------------------------------------------------------
 * L-values
 *
 * What may sit on the left of an assignment. The interesting cases are not the obvious ones:
 *
 * **A swizzle with a repeated component is not assignable.** `v.xx = vec2(1.0, 2.0)` would have
 * to write two different values into one component. GLSL says so explicitly, and an
 * implementation that only checks "is this a field selection" accepts it and produces whichever
 * value happened to land last.
 *
 * **A uniform, an attribute or a const is read-only**, so its name is an l-value syntactically
 * and not semantically. That check needs the storage qualifier, which is why the symbol carries
 * one.
 * ------------------------------------------------------------------------- */

GLboolean glsl_is_lvalue(glsl_sema_t *s, int32_t node) {
    if (!s || !s->ast || node == GLSL_NO_NODE) return GL_FALSE;
    const glsl_node_t *n = &s->ast->nodes[node];

    switch (n->kind) {
        case GLSL_NODE_IDENTIFIER: {
            const glsl_symbol_t *sym = lookup(s, n->text, n->length);
            if (!sym) return GL_FALSE;
            if (sym->is_function) return GL_FALSE;
            if (sym->qualifier == GLSL_TOK_KW_UNIFORM || sym->qualifier == GLSL_TOK_KW_ATTRIBUTE ||
                sym->qualifier == GLSL_TOK_KW_CONST) {
                sema_fail(s, "assignment to a read-only variable", node);
                return GL_FALSE;
            }
            return GL_TRUE;
        }
        case GLSL_NODE_INDEX:
            return glsl_is_lvalue(s, n->a);
        case GLSL_NODE_FIELD: {
            if (!glsl_is_lvalue(s, n->a)) return GL_FALSE;
            /* **A repeated component cannot be written.** Two values, one place. */
            for (size_t i = 0; i < n->length; i++) {
                for (size_t k = i + 1; k < n->length; k++) {
                    if (n->text[i] == n->text[k]) {
                        sema_fail(s, "a swizzle that repeats a component is not assignable", node);
                        return GL_FALSE;
                    }
                }
            }
            return GL_TRUE;
        }
        default:
            return GL_FALSE;
    }
}

/* -------------------------------------------------------------------------
 * Statements
 * ------------------------------------------------------------------------- */

static GLboolean check_statement(glsl_sema_t *s, int32_t node);

/* An assignment's target is checked here rather than in glsl_type_of, because typing an
 * expression and deciding it may be written to are different questions and only statements ask
 * the second one. Walks the whole expression so a nested assignment is caught too. */
static GLboolean check_assign_targets(glsl_sema_t *s, int32_t node) {
    if (node == GLSL_NO_NODE || s->error) return !s->error;
    const glsl_node_t *n = &s->ast->nodes[node];

    if (n->kind == GLSL_NODE_ASSIGN) {
        if (!glsl_is_lvalue(s, n->a)) {
            sema_fail(s, "assignment to something that cannot be assigned to", node);
            return GL_FALSE;
        }
    }
    /* `++x` and `x++` write to their operand too. */
    if ((n->kind == GLSL_NODE_UNARY || n->kind == GLSL_NODE_POSTFIX) &&
        (n->op == GLSL_TOK_INC || n->op == GLSL_TOK_DEC)) {
        if (!glsl_is_lvalue(s, n->a)) {
            sema_fail(s, "++ or -- on something that cannot be assigned to", node);
            return GL_FALSE;
        }
    }

    if (!check_assign_targets(s, n->a)) return GL_FALSE;
    if (!check_assign_targets(s, n->b)) return GL_FALSE;
    if (!check_assign_targets(s, n->c)) return GL_FALSE;
    for (int32_t a = n->kind == GLSL_NODE_CALL ? n->b : GLSL_NO_NODE;
         a != GLSL_NO_NODE; a = s->ast->nodes[a].sibling) {
        if (!check_assign_targets(s, a)) return GL_FALSE;
    }
    return GL_TRUE;
}

/* An expression in statement position: type it, then check what it writes to. */
static GLboolean check_expression(glsl_sema_t *s, int32_t node) {
    if (node == GLSL_NO_NODE) return GL_TRUE;
    if (glsl_type_of(s, node) == GLSL_TYPE_ERROR) return GL_FALSE;
    return check_assign_targets(s, node);
}

/* A condition has to be a bool. GLSL does not take "non-zero is true" from C, so an `int` here
 * is an error rather than a conversion. */
static GLboolean check_condition(glsl_sema_t *s, int32_t node, const char *where) {
    if (node == GLSL_NO_NODE) return GL_TRUE;
    glsl_type_t t = glsl_type_of(s, node);
    if (t == GLSL_TYPE_ERROR) return GL_FALSE;
    if (t != GLSL_TYPE_BOOL) {
        sema_fail(s, where, node);
        return GL_FALSE;
    }
    return check_assign_targets(s, node);
}

/* **One declarator, not the chain.** `float a, b;` is two DECL nodes linked through `sibling`,
 * and `sibling` is also how the enclosing statement or unit list is walked - so whoever is
 * walking that list reaches the second declarator on its own. Following the chain here as well
 * would declare every name twice and report a redeclaration that the source does not contain. */
static GLboolean check_declarator(glsl_sema_t *s, int32_t d) {
    const glsl_node_t *n = &s->ast->nodes[d];
    glsl_type_t t = glsl_type_from_token(n->type_tok);
    if (t == GLSL_TYPE_ERROR) {
        sema_fail(s, "declaration of an unknown type", d);
        return GL_FALSE;
    }
    if (t == GLSL_TYPE_VOID) {
        sema_fail(s, "a variable cannot be void", d);
        return GL_FALSE;
    }
    /* **The initialiser is typed before the name is declared**, so `float x = x;` is an error
     * rather than a variable initialised from itself. */
    if (n->a != GLSL_NO_NODE) {
        glsl_type_t init = glsl_type_of(s, n->a);
        if (init == GLSL_TYPE_ERROR) return GL_FALSE;
        if (!glsl_type_accepts(s, t, init)) {
            sema_fail(s, "initialiser of a different type from the variable", d);
            return GL_FALSE;
        }
    }
    /* **An array's length has to be a constant**, and in GLSL 1.10 that means a literal or a
     * `const` initialised from one (4.1.9). A literal is what shaders write and is all that is
     * accepted here: anything else is refused by name rather than assumed to be 1, which would
     * turn `vec4 v[n]` into a scalar and index it out of bounds at run time. An unsized
     * declaration - `varying vec4 v[];` - is refused for the same reason. */
    int elements = 0;
    if (n->array_size != GLSL_NO_NODE) {
        const glsl_node_t *sz = &s->ast->nodes[n->array_size];
        if (sz->kind != GLSL_NODE_INTCONST || (int)sz->value <= 0) {
            sema_fail(s, "an array length must be a positive integer literal", d);
            return GL_FALSE;
        }
        elements = (int)sz->value;
    }
    if (!glsl_declare(s, n->text, n->length, t, GL_FALSE)) return GL_FALSE;
    s->symbols[s->count - 1].qualifier = n->qualifier;
    s->symbols[s->count - 1].array_size = elements;
    return GL_TRUE;
}

static GLboolean check_statement(glsl_sema_t *s, int32_t node) {
    if (node == GLSL_NO_NODE || s->error) return !s->error;
    const glsl_node_t *n = &s->ast->nodes[node];

    switch (n->kind) {
        case GLSL_NODE_COMPOUND: {
            glsl_scope_push(s);
            for (int32_t st = n->a; st != GLSL_NO_NODE; st = s->ast->nodes[st].sibling) {
                if (!check_statement(s, st)) { glsl_scope_pop(s); return GL_FALSE; }
            }
            glsl_scope_pop(s);
            return GL_TRUE;
        }

        case GLSL_NODE_DECL:
            return check_declarator(s, node);

        case GLSL_NODE_EXPR_STMT:
            return check_expression(s, n->a);

        case GLSL_NODE_IF:
            if (!check_condition(s, n->a, "the condition of an if must be a bool")) return GL_FALSE;
            if (!check_statement(s, n->b)) return GL_FALSE;
            return check_statement(s, n->c);

        case GLSL_NODE_WHILE:
            if (!check_condition(s, n->a, "the condition of a while must be a bool")) return GL_FALSE;
            s->loop_depth++;
            { GLboolean ok = check_statement(s, n->b); s->loop_depth--; return ok; }

        case GLSL_NODE_DO_WHILE:
            s->loop_depth++;
            { GLboolean ok = check_statement(s, n->a); s->loop_depth--; if (!ok) return GL_FALSE; }
            return check_condition(s, n->b, "the condition of a do-while must be a bool");

        case GLSL_NODE_FOR: {
            /* **The init clause declares into the loop's own scope**, not the enclosing one -
             * `for (int i = 0; ...)` must not leave `i` behind, and two such loops in a row
             * must not collide. */
            glsl_scope_push(s);
            GLboolean ok = check_statement(s, n->a);
            if (ok) ok = check_condition(s, n->b, "the condition of a for must be a bool");
            if (ok) ok = check_expression(s, n->c);
            if (ok) {
                s->loop_depth++;
                ok = check_statement(s, n->d);
                s->loop_depth--;
            }
            glsl_scope_pop(s);
            return ok;
        }

        case GLSL_NODE_RETURN: {
            if (n->a == GLSL_NO_NODE) {
                if (s->current_return != GLSL_TYPE_VOID) {
                    sema_fail(s, "return with no value in a function that returns one", node);
                    return GL_FALSE;
                }
                return GL_TRUE;
            }
            if (s->current_return == GLSL_TYPE_VOID) {
                sema_fail(s, "return with a value in a void function", node);
                return GL_FALSE;
            }
            glsl_type_t t = glsl_type_of(s, n->a);
            if (t == GLSL_TYPE_ERROR) return GL_FALSE;
            if (!glsl_type_accepts(s, s->current_return, t)) {
                sema_fail(s, "return of a different type from the function's", node);
                return GL_FALSE;
            }
            return check_assign_targets(s, n->a);
        }

        case GLSL_NODE_BREAK:
        case GLSL_NODE_CONTINUE:
            /* Outside a loop these name nothing to break out of. The parser cannot tell, since
             * it has no idea where it is. */
            if (s->loop_depth == 0) {
                sema_fail(s, "break or continue outside a loop", node);
                return GL_FALSE;
            }
            return GL_TRUE;

        case GLSL_NODE_DISCARD:
            return GL_TRUE;

        default:
            /* An expression node reaching statement position, which the parser wraps - so this
             * is a tree built by hand rather than parsed. Typed anyway rather than ignored. */
            return check_expression(s, node);
    }
}

/* -------------------------------------------------------------------------
 * The translation unit
 * ------------------------------------------------------------------------- */

/* Records a function's signature so calls can be checked against it.
 *
 * Exported because the pixel-shader back end rebuilds a scoped symbol table of its own and has
 * to put the unit's functions back into it: a call is typed against the recorded signature, and
 * a table that holds only the globals cannot type one at all - which read as
 * "only float, vec and mat constructor arguments are generated" the first time a helper's
 * result was passed to a constructor. */
GLboolean glsl_declare_function(glsl_sema_t *s, int32_t node) {
    const glsl_node_t *n = &s->ast->nodes[node];
    glsl_type_t ret = glsl_type_from_token(n->type_tok);
    if (ret == GLSL_TYPE_ERROR) {
        sema_fail(s, "function with an unknown return type", node);
        return GL_FALSE;
    }
    if (!glsl_declare(s, n->text, n->length, ret, GL_TRUE)) return GL_FALSE;
    glsl_symbol_t *sym = &s->symbols[s->count - 1];

    int count = 0;
    for (int32_t p = n->b; p != GLSL_NO_NODE; p = s->ast->nodes[p].sibling) {
        if (count >= GLSL_MAX_PARAMS) {
            sema_fail(s, "too many parameters", node);
            return GL_FALSE;
        }
        glsl_type_t pt = glsl_type_from_token(s->ast->nodes[p].type_tok);
        if (pt == GLSL_TYPE_ERROR || pt == GLSL_TYPE_VOID) {
            sema_fail(s, "parameter with an unusable type", p);
            return GL_FALSE;
        }
        sym->params[count++] = pt;
    }
    sym->param_count = count;
    return GL_TRUE;
}

GLboolean glsl_check_unit(glsl_sema_t *s, int32_t unit) {
    if (!s || !s->ast || unit == GLSL_NO_NODE) return GL_FALSE;
    const glsl_node_t *u = &s->ast->nodes[unit];
    if (u->kind != GLSL_NODE_UNIT) {
        sema_fail(s, "not a translation unit", unit);
        return GL_FALSE;
    }

    /* **Two passes over the top level.** Every function is declared before any body is
     * checked, so a function may call one defined later in the file - which is ordinary in a
     * shader and would otherwise be an undeclared-name error depending on the order somebody
     * happened to write them in. */
    for (int32_t d = u->a; d != GLSL_NO_NODE; d = s->ast->nodes[d].sibling) {
        if (s->ast->nodes[d].kind == GLSL_NODE_FUNCTION) {
            if (!glsl_declare_function(s, d)) return GL_FALSE;
        }
    }

    for (int32_t d = u->a; d != GLSL_NO_NODE; d = s->ast->nodes[d].sibling) {
        const glsl_node_t *n = &s->ast->nodes[d];
        if (n->kind == GLSL_NODE_DECL) {
            if (!check_declarator(s, d)) return GL_FALSE;
            continue;
        }
        if (n->kind != GLSL_NODE_FUNCTION) continue;
        if (n->c == GLSL_NO_NODE) continue; /* a prototype has no body to check */

        /* The parameters live in the body's scope. Pushed here rather than inside the compound
         * statement, so a parameter and a top-level local of the same name collide - which is
         * what GLSL says. */
        glsl_scope_push(s);
        s->current_return = glsl_type_from_token(n->type_tok);
        for (int32_t p = n->b; p != GLSL_NO_NODE; p = s->ast->nodes[p].sibling) {
            const glsl_node_t *pn = &s->ast->nodes[p];
            if (pn->length == 0u) continue; /* unnamed parameter in a definition: nothing to bind */
            glsl_type_t pt = glsl_type_from_token(pn->type_tok);
            if (!glsl_declare(s, pn->text, pn->length, pt, GL_FALSE)) {
                glsl_scope_pop(s);
                return GL_FALSE;
            }
            s->symbols[s->count - 1].qualifier = pn->qualifier;
        }
        /* **The body's statements are walked here, not through the COMPOUND case**, because
         * that would push a second scope and make a local merely *shadow* a parameter. GLSL
         * puts the parameters and the body's outermost block in one scope, so
         * `float f(float x){ float x; }` is a redefinition - which is what glslang reports and
         * what the comment above this loop claims. Going through check_statement would have
         * quietly accepted it. */
        GLboolean ok = GL_TRUE;
        const int32_t body = n->c;
        for (int32_t st = s->ast->nodes[body].a; st != GLSL_NO_NODE;
             st = s->ast->nodes[st].sibling) {
            if (!check_statement(s, st)) { ok = GL_FALSE; break; }
        }
        glsl_scope_pop(s);
        s->current_return = GLSL_TYPE_VOID;
        if (!ok) return GL_FALSE;
    }
    return (GLboolean)(s->error == (const char *)0);
}
