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
    /* **Cleared for the same reason the parser's name list is**: the caller's `glsl_sema_t` is
     * often a stack local and is not zeroed, and a stale count here would have `glsl_struct_of`
     * hand back a struct made of whatever the stack held. */
    s->struct_count = 0;
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
        case GLSL_TOK_KW_MAT2X3: return GLSL_TYPE_MAT2X3;
        case GLSL_TOK_KW_MAT2X4: return GLSL_TYPE_MAT2X4;
        case GLSL_TOK_KW_MAT3X2: return GLSL_TYPE_MAT3X2;
        case GLSL_TOK_KW_MAT3X4: return GLSL_TYPE_MAT3X4;
        case GLSL_TOK_KW_MAT4X2: return GLSL_TYPE_MAT4X2;
        case GLSL_TOK_KW_MAT4X3: return GLSL_TYPE_MAT4X3;
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
    return (GLboolean)(t >= GLSL_TYPE_MAT2 && t <= GLSL_TYPE_MAT4X3);
}

/* **A matrix's shape, from a table.** `matCxR` is C columns of R rows; the square names are the
 * ones the language gives short spellings to, and `mat3` is `mat3x3`. Zero for anything that is
 * not a matrix, so a caller can ask without checking first. */
static int matrix_cols(glsl_type_t t) {
    switch (t) {
        case GLSL_TYPE_MAT2: case GLSL_TYPE_MAT2X3: case GLSL_TYPE_MAT2X4: return 2;
        case GLSL_TYPE_MAT3: case GLSL_TYPE_MAT3X2: case GLSL_TYPE_MAT3X4: return 3;
        case GLSL_TYPE_MAT4: case GLSL_TYPE_MAT4X2: case GLSL_TYPE_MAT4X3: return 4;
        default: return 0;
    }
}
static int matrix_rows(glsl_type_t t) {
    switch (t) {
        case GLSL_TYPE_MAT3X2: case GLSL_TYPE_MAT4X2: case GLSL_TYPE_MAT2: return 2;
        case GLSL_TYPE_MAT2X3: case GLSL_TYPE_MAT4X3: case GLSL_TYPE_MAT3: return 3;
        case GLSL_TYPE_MAT2X4: case GLSL_TYPE_MAT3X4: case GLSL_TYPE_MAT4: return 4;
        default: return 0;
    }
}

/* The matrix with these dimensions, or ERROR if there is none. */
static glsl_type_t matrix_of(int cols, int rows) {
    switch (cols) {
        case 2: return rows == 2 ? GLSL_TYPE_MAT2 : rows == 3 ? GLSL_TYPE_MAT2X3
                     : rows == 4 ? GLSL_TYPE_MAT2X4 : GLSL_TYPE_ERROR;
        case 3: return rows == 2 ? GLSL_TYPE_MAT3X2 : rows == 3 ? GLSL_TYPE_MAT3
                     : rows == 4 ? GLSL_TYPE_MAT3X4 : GLSL_TYPE_ERROR;
        case 4: return rows == 2 ? GLSL_TYPE_MAT4X2 : rows == 3 ? GLSL_TYPE_MAT4X3
                     : rows == 4 ? GLSL_TYPE_MAT4 : GLSL_TYPE_ERROR;
        default: return GLSL_TYPE_ERROR;
    }
}
static GLboolean is_scalar(glsl_type_t t) {
    return (GLboolean)(t == GLSL_TYPE_BOOL || t == GLSL_TYPE_INT || t == GLSL_TYPE_FLOAT);
}
static GLboolean is_sampler(glsl_type_t t) {
    return (GLboolean)(t >= GLSL_TYPE_SAMPLER1D && t <= GLSL_TYPE_SAMPLER2DSHADOW);
}

/* -------------------------------------------------------------------------
 * Structs
 * ------------------------------------------------------------------------- */

const glsl_struct_t *glsl_struct_of(const glsl_sema_t *s, glsl_type_t t) {
    if (!s || !glsl_type_is_struct(t)) return (const glsl_struct_t *)0;
    const int i = glsl_struct_index(t);
    if (i < 0 || i >= s->struct_count) return (const glsl_struct_t *)0;
    return &s->structs[i];
}

const glsl_struct_member_t *glsl_struct_member(const glsl_sema_t *s, glsl_type_t t,
                                               const char *name, size_t len) {
    const glsl_struct_t *st = glsl_struct_of(s, t);
    if (!st) return (const glsl_struct_member_t *)0;
    for (int i = 0; i < st->member_count; i++) {
        if (st->member[i].name_len != len) continue;
        size_t k = 0;
        while (k < len && st->member[i].name[k] == name[k]) k++;
        if (k == len) return &st->member[i];
    }
    return (const glsl_struct_member_t *)0;
}

/* The struct a name refers to, or GLSL_TYPE_ERROR. */
static glsl_type_t struct_type_by_name(const glsl_sema_t *s, const char *name, size_t len) {
    for (int i = 0; i < s->struct_count; i++) {
        if (s->structs[i].name_len != len) continue;
        size_t k = 0;
        while (k < len && s->structs[i].name[k] == name[k]) k++;
        if (k == len) return glsl_struct_type(i);
    }
    return GLSL_TYPE_ERROR;
}

/* **The size of anything, including a struct**, which `glsl_type_components` cannot answer on
 * its own: it takes a type and a struct's size lives in the table beside it. Every caller that
 * may see a struct asks this one instead. */
int glsl_type_components_of(const glsl_sema_t *s, glsl_type_t t) {
    const glsl_struct_t *st = glsl_struct_of(s, t);
    if (st) return st->components;
    return glsl_type_components(t);
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
        /* C columns of R rows, laid out column-major - the same run of registers everything
         * else here is, counted the way the name says. */
        case GLSL_TYPE_MAT2X3: return 6;
        case GLSL_TYPE_MAT2X4: return 8;
        case GLSL_TYPE_MAT3X2: return 6;
        case GLSL_TYPE_MAT3X4: return 12;
        case GLSL_TYPE_MAT4X2: return 8;
        case GLSL_TYPE_MAT4X3: return 12;
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

void glsl_sema_fail(glsl_sema_t *s, const char *why, int32_t node) { sema_fail(s, why, node); }
GLboolean glsl_type_is_vector(glsl_type_t t) { return is_vector(t); }
GLboolean glsl_type_is_matrix(glsl_type_t t) { return is_matrix(t); }
GLboolean glsl_type_is_sampler(glsl_type_t t) { return is_sampler(t); }
glsl_type_t glsl_type_base(glsl_type_t t) { return base_of(t); }
glsl_type_t glsl_type_vector_of(glsl_type_t base, int n) { return vector_of(base, n); }
int glsl_type_matrix_cols(glsl_type_t t) { return matrix_cols(t); }
int glsl_type_matrix_rows(glsl_type_t t) { return matrix_rows(t); }
glsl_type_t glsl_type_matrix_of(int cols, int rows) { return matrix_of(cols, rows); }

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

/*
 * **An integral constant expression** - GLSL 4.1.9, and the only place the language needs one is
 * an array's length.
 *
 * Folded here rather than demanded as a literal, because `const int N = 8;` followed by
 * `uniform vec2 offs[N];` is what shaders actually write - it is the shape mesa-demos' `vpglsl`
 * uses throughout, and refusing it was refusing the idiom rather than an edge case.
 *
 * What folds: an integer literal, a `const` integer scalar whose own initialiser folded, and the
 * arithmetic the specification allows over those. Anything else returns false and the caller
 * refuses the declaration by name - **nothing here guesses a length**, because a wrong one turns
 * `vec4 v[n]` into something indexed out of bounds at run time rather than into a diagnostic.
 *
 * Division by zero folds to false for the same reason: a constant expression the shader wrote
 * that has no value is a shader to refuse, not one to give an arbitrary length to.
 */
static GLboolean const_int_eval(const glsl_sema_t *s, int32_t node, int *out) {
    if (node == GLSL_NO_NODE) return GL_FALSE;
    const glsl_node_t *n = &s->ast->nodes[node];
    switch (n->kind) {
        case GLSL_NODE_INTCONST:
            *out = (int)n->value;
            return GL_TRUE;
        case GLSL_NODE_IDENTIFIER: {
            const glsl_symbol_t *sym = lookup(s, n->text, n->length);
            if (!sym || !sym->has_const_int) return GL_FALSE;
            *out = sym->const_int;
            return GL_TRUE;
        }
        case GLSL_NODE_UNARY: {
            int v = 0;
            if (!const_int_eval(s, n->a, &v)) return GL_FALSE;
            if (n->op == GLSL_TOK_MINUS) { *out = -v; return GL_TRUE; }
            if (n->op == GLSL_TOK_PLUS) { *out = v; return GL_TRUE; }
            return GL_FALSE;
        }
        case GLSL_NODE_BINARY: {
            int l = 0, r = 0;
            if (!const_int_eval(s, n->a, &l) || !const_int_eval(s, n->b, &r)) return GL_FALSE;
            switch (n->op) {
                case GLSL_TOK_PLUS:    *out = l + r; return GL_TRUE;
                case GLSL_TOK_MINUS:   *out = l - r; return GL_TRUE;
                case GLSL_TOK_STAR:    *out = l * r; return GL_TRUE;
                case GLSL_TOK_SLASH:   if (r == 0) return GL_FALSE; *out = l / r; return GL_TRUE;
                case GLSL_TOK_PERCENT: if (r == 0) return GL_FALSE; *out = l % r; return GL_TRUE;
                default: return GL_FALSE;
            }
        }
        default:
            return GL_FALSE;
    }
}

GLboolean glsl_declare(glsl_sema_t *s, const char *name, size_t len, glsl_type_t type,
                       GLboolean is_function) {
    if (!s) return GL_FALSE;
    if (s->count >= GLSL_MAX_SYMBOLS) {
        sema_fail(s, "too many declarations", GLSL_NO_NODE);
        return GL_FALSE;
    }
    /* **Redeclaration is only an error in the *same* scope.** Shadowing an outer name is legal
     * and common, so the check is against the current depth rather than the whole table.
     *
     * **A function is exempt, because GLSL overloads by signature.** `permute` may be declared
     * for `float`, `vec2`, `vec3` and `vec4` - that is how the built-in library is shaped and
     * how shaders are written against it. Whether two *functions* of one name are distinct is a
     * question about their parameters, which this does not have, so `glsl_declare_function`
     * asks it. What stays an error here is a function and a variable sharing a name. */
    for (int i = s->count - 1; i >= 0; i--) {
        if (s->symbols[i].scope < s->scope) break;
        if (!same_name(&s->symbols[i], name, len)) continue;
        if (is_function && s->symbols[i].is_function) continue;
        sema_fail(s, "a name declared twice in one scope", GLSL_NO_NODE);
        return GL_FALSE;
    }
    s->symbols[s->count].name = name;
    s->symbols[s->count].name_len = len;
    s->symbols[s->count].type = type;
    s->symbols[s->count].scope = s->scope;
    s->symbols[s->count].is_function = is_function;
    s->symbols[s->count].qualifier = GLSL_TOK_EOF;
    s->symbols[s->count].array_size = 0;
    /* **Cleared, because this table is reused and not zeroed.** A slot that held a `const int`
     * earlier would otherwise hand its value to whatever is declared here next, and the symptom
     * would be an array silently taking some previous constant's length. */
    s->symbols[s->count].has_const_int = GL_FALSE;
    s->symbols[s->count].const_int = 0;
    s->symbols[s->count].param_count = 0;
    s->symbols[s->count].decl_node = GLSL_NO_NODE;
    s->count++;
    return GL_TRUE;
}

/* A `const int` with a known value: an array's length may be one, and so may a loop's bound.
 * GLSL 7.4's built-in constants come in this way. */
GLboolean glsl_declare_const_int(glsl_sema_t *s, const char *name, size_t len, int value) {
    if (!glsl_declare(s, name, len, GLSL_TYPE_INT, GL_FALSE)) return GL_FALSE;
    s->symbols[s->count - 1].qualifier = GLSL_TOK_KW_CONST;
    s->symbols[s->count - 1].has_const_int = GL_TRUE;
    s->symbols[s->count - 1].const_int = value;
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

    /*
     * **`mat * vec` and `vec * mat` are linear transforms, and the two are not the same one.**
     *
     * `matCxR * vecC` is `vecR` - the vector is a column, so it has one entry per *column* of
     * the matrix and the result has one per row. `vecR * matCxR` is the product with the
     * transpose: the vector is a row, so it matches the rows and the result has one entry per
     * column. For a square matrix both read as "the dimension", which is why this worked while
     * only square ones existed.
     */
    if (op == GLSL_TOK_STAR && is_matrix(l) && is_vector(r)) {
        if (glsl_type_components(r) != matrix_cols(l) || base_of(r) != GLSL_TYPE_FLOAT) {
            sema_fail(s, "matrix and vector dimensions do not meet", node);
            return GLSL_TYPE_ERROR;
        }
        return vector_of(GLSL_TYPE_FLOAT, matrix_rows(l));
    }
    if (op == GLSL_TOK_STAR && is_vector(l) && is_matrix(r)) {
        if (glsl_type_components(l) != matrix_rows(r) || base_of(l) != GLSL_TYPE_FLOAT) {
            sema_fail(s, "vector and matrix dimensions do not meet", node);
            return GLSL_TYPE_ERROR;
        }
        return vector_of(GLSL_TYPE_FLOAT, matrix_cols(r));
    }
    /* **`matCxR * matPxC` is `matPxR`**: the left's columns have to match the right's rows, and
     * the result takes the right's columns and the left's rows. Equal types were the whole rule
     * while every matrix was square, and that is the special case where P = C = R. */
    if (op == GLSL_TOK_STAR && is_matrix(l) && is_matrix(r)) {
        if (matrix_cols(l) != matrix_rows(r)) {
            sema_fail(s, "matrix dimensions do not meet: the left's columns and the right's "
                         "rows have to be the same number", node);
            return GLSL_TYPE_ERROR;
        }
        const glsl_type_t product = matrix_of(matrix_cols(r), matrix_rows(l));
        if (product == GLSL_TYPE_ERROR) {
            sema_fail(s, "this matrix product has no type in GLSL 1.20", node);
            return GLSL_TYPE_ERROR;
        }
        return product;
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

        case GLSL_NODE_FIELD: {
            const glsl_type_t base = glsl_type_of(s, n->a);
            if (base == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
            /* **A struct member and a swizzle are the same syntax and different questions.**
             * `v.xy` asks for components of a vector; `s.a` asks for a named member. The base's
             * type decides which, and asking the wrong one produces a confusing diagnostic
             * rather than a wrong answer - `swizzle_type` would report that `a` is not a
             * component set. */
            if (glsl_type_is_struct(base)) {
                const glsl_struct_member_t *m = glsl_struct_member(s, base, n->text, n->length);
                if (!m) {
                    sema_fail(s, "this struct has no member of that name", node);
                    return GLSL_TYPE_ERROR;
                }
                /* An array member's type is its element type, exactly as an array variable's is
                 * in the symbol table - indexing is the only thing either can do. */
                return m->type;
            }
            return swizzle_type(s, base, n->text, n->length, node);
        }

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
                /* **A member that is an array is indexed the same way**, and `s.w[0]` reaches
                 * here with a field rather than a name underneath. GLSL 1.10 allows an array as
                 * a struct member (4.1.9) and the member table has carried its length all along
                 * - both back ends lay the elements out end to end from the member's offset -
                 * so the only thing missing was this rule. Without it a field's type came back
                 * as the *element* type, which is not a vector or a matrix, and the index was
                 * refused for indexing something that cannot be indexed. */
                if (base_n->kind == GLSL_NODE_FIELD) {
                    const glsl_type_t owner = glsl_type_of(s, base_n->a);
                    if (owner == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
                    if (glsl_type_is_struct(owner)) {
                        const glsl_struct_member_t *m =
                            glsl_struct_member(s, owner, base_n->text, base_n->length);
                        if (m && m->array_size > 0) {
                            glsl_type_t idx_t = glsl_type_of(s, n->b);
                            if (idx_t == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
                            if (idx_t != GLSL_TYPE_INT) {
                                sema_fail(s, "an index must be an int", node);
                                return GLSL_TYPE_ERROR;
                            }
                            const glsl_node_t *idx_n = &s->ast->nodes[n->b];
                            if (idx_n->kind == GLSL_NODE_INTCONST &&
                                ((int)idx_n->value < 0 ||
                                 (int)idx_n->value >= m->array_size)) {
                                sema_fail(s, "array index out of range", node);
                                return GLSL_TYPE_ERROR;
                            }
                            return m->type;
                        }
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
            /* Indexing a matrix gives a **column**, which has one entry per row - so `mat2x3[0]`
             * is a `vec3` and not a `vec2`. Indexing a vector gives a component. */
            if (is_matrix(base)) return vector_of(GLSL_TYPE_FLOAT, matrix_rows(base));
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
                /* 1.20's non-square matrices, and the long spellings of the square ones - the
                 * language gives `mat3` and `mat3x3` both, and they are one type. */
                {"mat2x2", GLSL_TYPE_MAT2}, {"mat3x3", GLSL_TYPE_MAT3},
                {"mat4x4", GLSL_TYPE_MAT4},
                {"mat2x3", GLSL_TYPE_MAT2X3}, {"mat2x4", GLSL_TYPE_MAT2X4},
                {"mat3x2", GLSL_TYPE_MAT3X2}, {"mat3x4", GLSL_TYPE_MAT3X4},
                {"mat4x2", GLSL_TYPE_MAT4X2}, {"mat4x3", GLSL_TYPE_MAT4X3},
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
            /* A non-square matrix constructor is 1.20's, the same as the type name is. The
             * parser refuses the declaration; this refuses `mat2x3(1.0)[0]` in an expression,
             * which never passes through a declaration and would otherwise be the one way into
             * the type in a 1.10 shader. */
            if (ctor != GLSL_TYPE_ERROR && s->version != 0 && s->version < 120 &&
                matrix_cols(ctor) != matrix_rows(ctor)) {
                sema_fail(s, "the non-square matrix types are GLSL 1.20; this shader is 1.10",
                          node);
                return GLSL_TYPE_ERROR;
            }
            if (ctor != GLSL_TYPE_ERROR) return constructor_type(s, ctor, n->b, node);

            /*
             * **A struct constructor, which is stricter than every built-in one.**
             *
             * `vec4(1.0)` fills, `vec4(v3, 1.0)` gathers, `mat3(m4)` truncates - the built-in
             * constructors are a small language of their own. A struct's is none of that: one
             * argument per member, in order, each assignable to that member (1.10, 5.4.3).
             * Writing it as another case of `constructor_type` would have meant teaching that
             * function to sometimes stop doing the thing it exists to do.
             */
            {
                const glsl_type_t st_type =
                    struct_type_by_name(s, callee->text, callee->length);
                if (st_type != GLSL_TYPE_ERROR) {
                    const glsl_struct_t *st = glsl_struct_of(s, st_type);
                    int i = 0;
                    for (int32_t a = n->b; a != GLSL_NO_NODE; a = s->ast->nodes[a].sibling, i++) {
                        const glsl_type_t at = glsl_type_of(s, a);
                        if (at == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
                        if (i >= st->member_count) {
                            sema_fail(s, "too many arguments for this struct's constructor", node);
                            return GLSL_TYPE_ERROR;
                        }
                        /* **An array member cannot be given a value here.** GLSL 1.10 has no
                         * array-valued expression, so there is nothing that could be passed for
                         * one - a constructor for such a struct cannot be written at all, and
                         * saying so beats accepting an argument that fills only the first
                         * element. */
                        if (st->member[i].array_size > 0) {
                            sema_fail(s, "a struct with an array member has no constructor", node);
                            return GLSL_TYPE_ERROR;
                        }
                        if (!glsl_type_accepts(s, st->member[i].type, at)) {
                            sema_fail(s, "this argument does not match the struct's member", a);
                            return GLSL_TYPE_ERROR;
                        }
                    }
                    if (i != st->member_count) {
                        sema_fail(s, "too few arguments for this struct's constructor", node);
                        return GLSL_TYPE_ERROR;
                    }
                    return st_type;
                }
            }

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

            /*
             * **Which overload, decided from the argument types.**
             *
             * A name no longer identifies a function, so the arguments are evaluated once and
             * then offered to every function of that name. An exact match wins outright; a
             * match that needed a conversion `glsl_type_accepts` allows - 1.20's int-to-float -
             * is taken only when nothing matched exactly, which is the rule that stops
             * `f(int)` being passed over for `f(float)` when the shader wrote an integer.
             */
            glsl_type_t args[GLSL_MAX_PARAMS];
            int given = 0;
            for (int32_t a = n->b; a != GLSL_NO_NODE; a = s->ast->nodes[a].sibling) {
                glsl_type_t at = glsl_type_of(s, a);
                if (at == GLSL_TYPE_ERROR) return GLSL_TYPE_ERROR;
                if (given >= GLSL_MAX_PARAMS) {
                    sema_fail(s, "too many arguments", node);
                    return GLSL_TYPE_ERROR;
                }
                args[given++] = at;
            }

            const glsl_symbol_t *exact = (const glsl_symbol_t *)0;
            const glsl_symbol_t *convert = (const glsl_symbol_t *)0;
            GLboolean any_name = GL_FALSE, any_non_function = GL_FALSE;
            for (int i = s->count - 1; i >= 0; i--) {
                const glsl_symbol_t *cand = &s->symbols[i];
                if (!same_name(cand, callee->text, callee->length)) continue;
                any_name = GL_TRUE;
                if (!cand->is_function) { any_non_function = GL_TRUE; continue; }
                if (cand->param_count != given) continue;
                GLboolean fits = GL_TRUE, identical = GL_TRUE;
                for (int k = 0; k < given; k++) {
                    if (cand->params[k] != args[k]) identical = GL_FALSE;
                    if (!glsl_type_accepts(s, cand->params[k], args[k])) { fits = GL_FALSE; break; }
                }
                if (!fits) continue;
                if (identical) { exact = cand; break; }
                if (!convert) convert = cand;
            }

            if (!any_name) {
                sema_fail(s, "call to an undeclared function", node);
                return GLSL_TYPE_ERROR;
            }
            const glsl_symbol_t *sym = exact ? exact : convert;
            if (!sym) {
                /* Name found, nothing it could mean. The two cases are worth separating: a
                 * variable called like a function is a different mistake from an argument list
                 * no overload takes. */
                if (any_non_function) {
                    sema_fail(s, "calling something that is not a function", node);
                } else {
                    sema_fail(s, "no version of this function takes these arguments", node);
                }
                return GLSL_TYPE_ERROR;
            }
            /* **The decision is written onto the call**, so the back ends do not repeat it. */
            s->ast->nodes[node].resolved = sym->decl_node;
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
/* Whether this node is a name - or a struct's member - standing for a whole array rather than
 * for one of its elements. Both spellings reach here: `a` for a declared array, and `s.w` for a
 * member declared with a length. */
static GLboolean names_whole_array(glsl_sema_t *s, int32_t node) {
    if (node == GLSL_NO_NODE) return GL_FALSE;
    const glsl_node_t *n = &s->ast->nodes[node];
    if (n->kind == GLSL_NODE_IDENTIFIER) {
        const glsl_symbol_t *sym = lookup(s, n->text, n->length);
        return (GLboolean)(sym && !sym->is_function && sym->array_size > 0);
    }
    if (n->kind == GLSL_NODE_FIELD) {
        const glsl_type_t owner = glsl_type_of(s, n->a);
        if (!glsl_type_is_struct(owner)) return GL_FALSE;
        const glsl_struct_member_t *m = glsl_struct_member(s, owner, n->text, n->length);
        return (GLboolean)(m && m->array_size > 0);
    }
    return GL_FALSE;
}

static GLboolean check_assign_targets(glsl_sema_t *s, int32_t node) {
    if (node == GLSL_NO_NODE || s->error) return !s->error;
    const glsl_node_t *n = &s->ast->nodes[node];

    if (n->kind == GLSL_NODE_ASSIGN) {
        if (!glsl_is_lvalue(s, n->a)) {
            sema_fail(s, "assignment to something that cannot be assigned to", node);
            return GL_FALSE;
        }
        /* **A whole array is not an l-value.** GLSL 1.10 section 5.8 lists what is - built-in
         * types, entire structures, fields, swizzles without repeats, and l-values in
         * parentheses - and an array is not among them.
         *
         * Asked here rather than in `glsl_is_lvalue`, because that function recurses *through*
         * the array's name on the way to `a[0]`: refusing an array identifier there would refuse
         * every element assignment with it. The question is only about a name standing alone as
         * the whole destination, which is exactly what this node's left child is.
         *
         * It was caught before this - by the *code generator*, which has no memory to copy an
         * array into. That is one back end refusing it and the interpreter, which has a float
         * array and would happily have copied something, never being asked. A language error
         * belongs to the front end so that both paths refuse it for the same reason. */
        if (names_whole_array(s, n->a)) {
            sema_fail(s, "an array is assigned an element at a time; GLSL 1.10 does not make a "
                         "whole array an l-value (5.8)", node);
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
/* **The type a declaration or parameter node writes**, which is its token unless the token is an
 * identifier - and then it is the struct that identifier names. Every place that used to call
 * `glsl_type_from_token` on a node calls this, so a struct is accepted wherever a type is. */
static glsl_type_t node_declared_type(const glsl_sema_t *s, const glsl_node_t *n) {
    if (n->type_tok == GLSL_TOK_IDENTIFIER && n->type_name) {
        return struct_type_by_name(s, n->type_name, n->type_name_len);
    }
    return glsl_type_from_token(n->type_tok);
}

/*
 * `struct S { ... };` - records the type, its members and their offsets.
 *
 * **Layout is members end to end, in declaration order**, and the offsets computed here are what
 * both back ends use: the interpreter indexes a float array with them and the code generator
 * adds them to a register base. One layout, decided once, or the two would disagree about where
 * `s.b` is and only one of them would be wrong at a time.
 */
static GLboolean check_struct_def(glsl_sema_t *s, int32_t d) {
    const glsl_node_t *n = &s->ast->nodes[d];
    if (struct_type_by_name(s, n->text, n->length) != GLSL_TYPE_ERROR) {
        sema_fail(s, "a struct with this name is already declared", d);
        return GL_FALSE;
    }
    if (s->struct_count >= GLSL_MAX_STRUCTS) {
        sema_fail(s, "too many struct declarations", d);
        return GL_FALSE;
    }
    glsl_struct_t *st = &s->structs[s->struct_count];
    st->name = n->text;
    st->name_len = n->length;
    st->member_count = 0;
    st->components = 0;

    for (int32_t m = n->a; m != GLSL_NO_NODE; m = s->ast->nodes[m].sibling) {
        const glsl_node_t *mn = &s->ast->nodes[m];
        if (st->member_count >= GLSL_MAX_STRUCT_MEMBERS) {
            sema_fail(s, "too many struct members", m);
            return GL_FALSE;
        }
        const glsl_type_t mt = node_declared_type(s, mn);
        if (mt == GLSL_TYPE_ERROR) {
            sema_fail(s, "struct member of an unknown type", m);
            return GL_FALSE;
        }
        if (mt == GLSL_TYPE_VOID) {
            sema_fail(s, "a struct member cannot be void", m);
            return GL_FALSE;
        }
        /* A sampler has no components and cannot be stored, so a struct holding one has no
         * layout to give it. GLSL 1.10 allows it in principle; this back end does not have
         * anywhere to put it, and says so rather than computing a size of zero. */
        if (glsl_type_is_sampler(mt)) {
            sema_fail(s, "a struct member cannot be a sampler here", m);
            return GL_FALSE;
        }
        int count = 1;
        if (mn->array_size != GLSL_NO_NODE) {
            const glsl_node_t *sz = &s->ast->nodes[mn->array_size];
            if (sz->kind != GLSL_NODE_INTCONST || (int)sz->value <= 0) {
                sema_fail(s, "a struct member array needs a positive constant size", m);
                return GL_FALSE;
            }
            count = (int)sz->value;
        }
        const int one = glsl_type_components_of(s, mt);
        for (int i = 0; i < st->member_count; i++) {
            if (st->member[i].name_len == mn->length) {
                size_t k = 0;
                while (k < mn->length && st->member[i].name[k] == mn->text[k]) k++;
                if (k == mn->length) {
                    sema_fail(s, "a struct names the same member twice", m);
                    return GL_FALSE;
                }
            }
        }
        glsl_struct_member_t *mem = &st->member[st->member_count++];
        mem->name = mn->text;
        mem->name_len = mn->length;
        mem->type = mt;
        mem->array_size = (mn->array_size != GLSL_NO_NODE) ? count : 0;
        mem->offset = st->components;
        st->components += one * count;
    }
    if (st->member_count == 0) {
        sema_fail(s, "a struct must have at least one member", d);
        return GL_FALSE;
    }
    /* Checked once the whole thing is measured, so the message is about the struct rather than
     * about whichever member happened to cross the line. */
    if (st->components > GLSL_MAX_STRUCT_COMPONENTS) {
        sema_fail(s, "this struct is larger than a value this implementation can carry", d);
        return GL_FALSE;
    }
    s->struct_count++;
    return GL_TRUE;
}

/* **`gl_` is reserved** (1.10, 3.7): a shader may not declare a name beginning with it, whether
 * or not this implementation happens to have a built-in of that name. Checked at the shader's own
 * declarations rather than inside `glsl_declare`, because that is what the built-in tables use to
 * put the real ones in. */
static GLboolean reserved_name(glsl_sema_t *s, const char *name, size_t len, int32_t node) {
    if (len >= 3u && name[0] == 'g' && name[1] == 'l' && name[2] == '_') {
        sema_fail(s, "a name beginning with `gl_` is reserved and a shader may not declare one",
                  node);
        return GL_TRUE;
    }
    return GL_FALSE;
}

static GLboolean check_declarator(glsl_sema_t *s, int32_t d) {
    const glsl_node_t *n = &s->ast->nodes[d];
    glsl_type_t t = node_declared_type(s, n);
    if (reserved_name(s, n->text, n->length, d)) return GL_FALSE;
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
    /* **An array's length has to be an integral constant expression** - GLSL 4.1.9 - which is a
     * literal, a `const` integer, or arithmetic over those. `const_int_eval` is the whole of
     * that rule; what it cannot fold is refused by name rather than assumed to be 1, which would
     * turn `vec4 v[n]` into a scalar and index it out of bounds at run time. An unsized
     * declaration - `varying vec4 v[];` - is refused for the same reason. */
    int elements = 0;
    if (n->array_size != GLSL_NO_NODE) {
        int len = 0;
        if (!const_int_eval(s, n->array_size, &len)) {
            sema_fail(s, "an array length must be a constant expression", d);
            return GL_FALSE;
        }
        if (len <= 0) {
            sema_fail(s, "an array length must be greater than zero", d);
            return GL_FALSE;
        }
        elements = len;
        /*
         * **The fold is written back into the tree, so it happens once.**
         *
         * The interpreter, the generator and the linker all read an array's length straight off
         * this node and all three accepted only `GLSL_NODE_INTCONST` - which is right, because a
         * length is a number by the time anything downstream wants it. Folding here and leaving
         * `const int N = 2; float a[N];` as an identifier in the tree made the semantic pass
         * agree the length was 3 while every consumer read 0: the shader compiled, linked, and
         * drew black, and `glGetUniformLocation` could not find `vals[3]` because the linker had
         * enumerated no elements.
         *
         * Replacing the expression with its value is the same discipline the struct layout
         * follows - decided once, in this pass, and read by everyone after it - and it means no
         * back end needs a constant folder of its own to stay in agreement with this one.
         */
        s->ast->nodes[n->array_size].kind = GLSL_NODE_INTCONST;
        s->ast->nodes[n->array_size].value = (int32_t)len;
    }
    if (!glsl_declare(s, n->text, n->length, t, GL_FALSE)) return GL_FALSE;
    s->symbols[s->count - 1].qualifier = n->qualifier;
    s->symbols[s->count - 1].array_size = elements;
    /* **A `const int` carries its value forward**, so a later array length can name it. Only an
     * unqualified-length integer scalar with a foldable initialiser qualifies, which is the set
     * 4.1.9 allows to appear in one. */
    if (n->qualifier == GLSL_TOK_KW_CONST && t == GLSL_TYPE_INT && elements == 0) {
        int v = 0;
        if (n->a != GLSL_NO_NODE && const_int_eval(s, n->a, &v)) {
            s->symbols[s->count - 1].has_const_int = GL_TRUE;
            s->symbols[s->count - 1].const_int = v;
        }
    }
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

        /* A struct declared inside a function. Its type is recorded in the unit's table rather
         * than a scoped one: GLSL 1.10 has no way to declare two structs of the same name in
         * different scopes without `check_struct_def` refusing the second, so one table holds
         * everything and the refusal is what keeps it unambiguous. */
        case GLSL_NODE_STRUCT_DEF:
            return check_struct_def(s, node);

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
    glsl_type_t ret = node_declared_type(s, n);
    if (ret == GLSL_TYPE_ERROR) {
        sema_fail(s, "function with an unknown return type", node);
        return GL_FALSE;
    }
    /* **The signature is built before the symbol**, because whether this declaration is a new
     * overload or a redefinition is a question about the parameters, and the table cannot be
     * asked until they are known. */
    glsl_type_t params[GLSL_MAX_PARAMS];
    int count = 0;
    for (int32_t p = n->b; p != GLSL_NO_NODE; p = s->ast->nodes[p].sibling) {
        if (count >= GLSL_MAX_PARAMS) {
            sema_fail(s, "too many parameters", node);
            return GL_FALSE;
        }
        glsl_type_t pt = node_declared_type(s, &s->ast->nodes[p]);
        if (pt == GLSL_TYPE_ERROR || pt == GLSL_TYPE_VOID) {
            sema_fail(s, "parameter with an unusable type", p);
            return GL_FALSE;
        }
        params[count++] = pt;
    }

    /*
     * **Two functions of one name are the same function when their parameters agree.** GLSL
     * overloads on the parameter list and *not* on the return type, so `float f(int)` and
     * `vec2 f(int)` are a redefinition rather than two overloads - a call could not choose
     * between them, since the arguments are all a call site offers.
     *
     * A prototype followed by its definition is the ordinary way to arrive here twice with the
     * same signature, so it is accepted: the second one takes over the entry, which is what
     * gives the definition's body to a call that was checked against the prototype.
     */
    for (int i = s->count - 1; i >= 0; i--) {
        glsl_symbol_t *prev = &s->symbols[i];
        if (!prev->is_function || !same_name(prev, n->text, n->length)) continue;
        if (prev->param_count != count) continue;
        GLboolean same = GL_TRUE;
        for (int k = 0; k < count; k++) {
            if (prev->params[k] != params[k]) { same = GL_FALSE; break; }
        }
        if (!same) continue;
        if (prev->type != ret) {
            sema_fail(s, "two functions differing only in return type", node);
            return GL_FALSE;
        }
        /* The body, if this one has it, is the one a call should reach. */
        if (n->c != GLSL_NO_NODE) prev->decl_node = node;
        return GL_TRUE;
    }

    if (!glsl_declare(s, n->text, n->length, ret, GL_TRUE)) return GL_FALSE;
    glsl_symbol_t *sym = &s->symbols[s->count - 1];
    for (int k = 0; k < count; k++) sym->params[k] = params[k];
    sym->param_count = count;
    sym->decl_node = node;
    return GL_TRUE;
}

GLboolean glsl_check_unit(glsl_sema_t *s, int32_t unit) {
    if (!s || !s->ast || unit == GLSL_NO_NODE) return GL_FALSE;
    const glsl_node_t *u = &s->ast->nodes[unit];
    if (u->kind != GLSL_NODE_UNIT) {
        sema_fail(s, "not a translation unit", unit);
        return GL_FALSE;
    }

    /* **Three passes over the top level**, and the order of the first two is load-bearing.
     *
     * Structs are recorded first, because a function's signature may be written in terms of one
     * - `S bump(S v)` - and the second pass resolves those types. With the struct pass second,
     * `S` was not yet a type when `bump` was declared, and a shader that passes a struct to a
     * function failed with "declaration of an unknown type" while the same struct worked
     * perfectly as a local. Found by its own test.
     *
     * Then every function is declared before any body is checked, so a function may call one
     * defined later in the file - ordinary in a shader, and otherwise an undeclared-name error
     * that depends on the order somebody happened to write them in. */
    for (int32_t d = u->a; d != GLSL_NO_NODE; d = s->ast->nodes[d].sibling) {
        if (s->ast->nodes[d].kind == GLSL_NODE_STRUCT_DEF) {
            if (!check_struct_def(s, d)) return GL_FALSE;
        }
    }

    for (int32_t d = u->a; d != GLSL_NO_NODE; d = s->ast->nodes[d].sibling) {
        if (s->ast->nodes[d].kind == GLSL_NODE_FUNCTION) {
            if (!glsl_declare_function(s, d)) return GL_FALSE;
        }
    }

    for (int32_t d = u->a; d != GLSL_NO_NODE; d = s->ast->nodes[d].sibling) {
        const glsl_node_t *n = &s->ast->nodes[d];
        /* Already recorded by the first pass above. */
        if (n->kind == GLSL_NODE_STRUCT_DEF) continue;
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
        s->current_return = node_declared_type(s, n);
        for (int32_t p = n->b; p != GLSL_NO_NODE; p = s->ast->nodes[p].sibling) {
            const glsl_node_t *pn = &s->ast->nodes[p];
            if (pn->length == 0u) continue; /* unnamed parameter in a definition: nothing to bind */
            if (reserved_name(s, pn->text, pn->length, p)) {
                glsl_scope_pop(s);
                return GL_FALSE;
            }
            glsl_type_t pt = node_declared_type(s, pn);
            /* **A parameter may be an array** (1.10, 6.1), and it binds exactly as a local array
             * does: the symbol carries the element type and the length, and indexing is the only
             * thing either can do. Declared without the length, `w[0]` inside the body asked the
             * index rule to index a `float` and was refused for indexing something that is
             * neither a vector nor a matrix. */
            int psize = 0;
            if (pn->array_size != GLSL_NO_NODE && !const_int_eval(s, pn->array_size, &psize)) {
                sema_fail(s, "an array parameter's length has to be a constant", p);
                glsl_scope_pop(s);
                return GL_FALSE;
            }
            if (psize > 0) {
                /* **Folded back into the tree, for the same reason a declarator's length is**:
                 * the interpreter and the generator both read a length straight off this node
                 * and both want a number by then. Leaving `float w[N]` as an identifier would
                 * have the semantic pass agree the length is N while every consumer read 0. */
                s->ast->nodes[pn->array_size].kind = GLSL_NODE_INTCONST;
                s->ast->nodes[pn->array_size].value = (double)psize;
                if (!glsl_declare_array(s, pn->text, pn->length, pt, psize, pn->qualifier)) {
                    glsl_scope_pop(s);
                    return GL_FALSE;
                }
                continue;
            }
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
