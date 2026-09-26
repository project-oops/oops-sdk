/*
 * oops-gl: GLSL types and the semantic stage
 *
 * The grammar is happy with `vec3 + mat4` and with `.xyzw` on a `vec2`. Deciding those
 * are wrong happens here.
 *
 * GLSL's operators are not C's. GLSL 1.10 has no implicit conversion between int and
 * float, so `1 + 1.0` is an error. A scalar against a vector is component-wise and
 * keeps the vector's type, in either order. `mat * vec` is a linear transform, not a
 * component-wise multiply: `mat4 * vec4` is a `vec4` and `mat4 * vec3` is an error.
 */

#include "glsl_internal.h"

static void sema_fail(glsl_sema_t *s, const char *why, int32_t node) {
    if (s->error)
        return; /* the first error is the one that means something */
    s->error = why;
    if (node != GLSL_NO_NODE && s->ast) {
        s->error_line = s->ast->nodes[node].line;
        s->error_column = s->ast->nodes[node].column;
    }
}

/* The type predicates and the diagnostic sink, for the built-in library next door.
 * Wrappers over the statics, so this file keeps the short names and each rule has one
 * definition. */
void glsl_sema_fail(glsl_sema_t *s, const char *why, int32_t node);
GLboolean glsl_type_is_vector(glsl_type_t t);
GLboolean glsl_type_is_matrix(glsl_type_t t);
GLboolean glsl_type_is_sampler(glsl_type_t t);
glsl_type_t glsl_type_base(glsl_type_t t);
glsl_type_t glsl_type_vector_of(glsl_type_t base, int n);
/* Defined with the other array rules, beside the assignment ones; `==` needs it to ask
 * about whole arrays before it compares element types. */
static GLboolean both_whole_arrays(glsl_sema_t *s, int32_t a, int32_t b, int *count,
                                   GLboolean *mismatch);

void glsl_sema_init(glsl_sema_t *s, glsl_ast_t *ast) {
    if (!s)
        return;
    s->ast = ast;
    s->count = 0;
    s->scope = 0;
    s->error = (const char *)0;
    s->error_line = 0;
    s->error_column = 0;
    s->current_return = GLSL_TYPE_VOID;
    s->loop_depth = 0;
    s->stage = 0u;
    s->version = 0; /* unstated: 1.10's rules, which convert nothing */
    /* The caller's `glsl_sema_t` is often an unzeroed stack local; a stale count would
     * have `glsl_struct_of` hand back garbage. */
    s->struct_count = 0;
}

/* -------------------------------------------------------------------------
 * GLSL 1.20's implicit conversion
 *
 * int to float and `ivecN` to `vecN`, and nothing else (1.20, 4.1.10). Not float to
 * int and not bool to anything, so `int i = 1.0;` is an error in 1.20. 1.10 converts
 * nothing.
 * ------------------------------------------------------------------------- */

static glsl_type_t widen_to_float(glsl_type_t t) {
    switch (t) {
    case GLSL_TYPE_INT:
        return GLSL_TYPE_FLOAT;
    case GLSL_TYPE_IVEC2:
        return GLSL_TYPE_VEC2;
    case GLSL_TYPE_IVEC3:
        return GLSL_TYPE_VEC3;
    case GLSL_TYPE_IVEC4:
        return GLSL_TYPE_VEC4;
    default:
        return t;
    }
}

/* Whether a value of `got` may be used where `want` is expected - the test every
 * assignment, initialiser, argument and return goes through, so the rule lives in one
 * place. */
GLboolean glsl_type_accepts(const glsl_sema_t *s, glsl_type_t want, glsl_type_t got) {
    if (want == got)
        return GL_TRUE;
    if (!s || s->version < 120)
        return GL_FALSE;
    return (GLboolean)(widen_to_float(got) == want);
}

glsl_type_t glsl_type_from_token(glsl_token_type_t t) {
    switch (t) {
    case GLSL_TOK_KW_VOID:
        return GLSL_TYPE_VOID;
    case GLSL_TOK_KW_BOOL:
        return GLSL_TYPE_BOOL;
    case GLSL_TOK_KW_INT:
        return GLSL_TYPE_INT;
    case GLSL_TOK_KW_FLOAT:
        return GLSL_TYPE_FLOAT;
    case GLSL_TOK_KW_VEC2:
        return GLSL_TYPE_VEC2;
    case GLSL_TOK_KW_VEC3:
        return GLSL_TYPE_VEC3;
    case GLSL_TOK_KW_VEC4:
        return GLSL_TYPE_VEC4;
    case GLSL_TOK_KW_IVEC2:
        return GLSL_TYPE_IVEC2;
    case GLSL_TOK_KW_IVEC3:
        return GLSL_TYPE_IVEC3;
    case GLSL_TOK_KW_IVEC4:
        return GLSL_TYPE_IVEC4;
    case GLSL_TOK_KW_BVEC2:
        return GLSL_TYPE_BVEC2;
    case GLSL_TOK_KW_BVEC3:
        return GLSL_TYPE_BVEC3;
    case GLSL_TOK_KW_BVEC4:
        return GLSL_TYPE_BVEC4;
    case GLSL_TOK_KW_MAT2:
        return GLSL_TYPE_MAT2;
    case GLSL_TOK_KW_MAT3:
        return GLSL_TYPE_MAT3;
    case GLSL_TOK_KW_MAT4:
        return GLSL_TYPE_MAT4;
    case GLSL_TOK_KW_MAT2X3:
        return GLSL_TYPE_MAT2X3;
    case GLSL_TOK_KW_MAT2X4:
        return GLSL_TYPE_MAT2X4;
    case GLSL_TOK_KW_MAT3X2:
        return GLSL_TYPE_MAT3X2;
    case GLSL_TOK_KW_MAT3X4:
        return GLSL_TYPE_MAT3X4;
    case GLSL_TOK_KW_MAT4X2:
        return GLSL_TYPE_MAT4X2;
    case GLSL_TOK_KW_MAT4X3:
        return GLSL_TYPE_MAT4X3;
    case GLSL_TOK_KW_SAMPLER1D:
        return GLSL_TYPE_SAMPLER1D;
    case GLSL_TOK_KW_SAMPLER2D:
        return GLSL_TYPE_SAMPLER2D;
    case GLSL_TOK_KW_SAMPLER3D:
        return GLSL_TYPE_SAMPLER3D;
    case GLSL_TOK_KW_SAMPLERCUBE:
        return GLSL_TYPE_SAMPLERCUBE;
    case GLSL_TOK_KW_SAMPLER1DSHADOW:
        return GLSL_TYPE_SAMPLER1DSHADOW;
    case GLSL_TOK_KW_SAMPLER2DSHADOW:
        return GLSL_TYPE_SAMPLER2DSHADOW;
    default:
        return GLSL_TYPE_ERROR;
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

/* A matrix's shape. `matCxR` is C columns of R rows, and `mat3` is `mat3x3`. Zero for
 * anything that is not a matrix, so a caller can ask without checking first. */
static int matrix_cols(glsl_type_t t) {
    switch (t) {
    case GLSL_TYPE_MAT2:
    case GLSL_TYPE_MAT2X3:
    case GLSL_TYPE_MAT2X4:
        return 2;
    case GLSL_TYPE_MAT3:
    case GLSL_TYPE_MAT3X2:
    case GLSL_TYPE_MAT3X4:
        return 3;
    case GLSL_TYPE_MAT4:
    case GLSL_TYPE_MAT4X2:
    case GLSL_TYPE_MAT4X3:
        return 4;
    default:
        return 0;
    }
}
static int matrix_rows(glsl_type_t t) {
    switch (t) {
    case GLSL_TYPE_MAT3X2:
    case GLSL_TYPE_MAT4X2:
    case GLSL_TYPE_MAT2:
        return 2;
    case GLSL_TYPE_MAT2X3:
    case GLSL_TYPE_MAT4X3:
    case GLSL_TYPE_MAT3:
        return 3;
    case GLSL_TYPE_MAT2X4:
    case GLSL_TYPE_MAT3X4:
    case GLSL_TYPE_MAT4:
        return 4;
    default:
        return 0;
    }
}

/* The matrix with these dimensions, or ERROR if there is none. */
static glsl_type_t matrix_of(int cols, int rows) {
    switch (cols) {
    case 2:
        return rows == 2   ? GLSL_TYPE_MAT2
               : rows == 3 ? GLSL_TYPE_MAT2X3
               : rows == 4 ? GLSL_TYPE_MAT2X4
                           : GLSL_TYPE_ERROR;
    case 3:
        return rows == 2   ? GLSL_TYPE_MAT3X2
               : rows == 3 ? GLSL_TYPE_MAT3
               : rows == 4 ? GLSL_TYPE_MAT3X4
                           : GLSL_TYPE_ERROR;
    case 4:
        return rows == 2   ? GLSL_TYPE_MAT4X2
               : rows == 3 ? GLSL_TYPE_MAT4X3
               : rows == 4 ? GLSL_TYPE_MAT4
                           : GLSL_TYPE_ERROR;
    default:
        return GLSL_TYPE_ERROR;
    }
}
static GLboolean is_scalar(glsl_type_t t) {
    return (GLboolean)(t == GLSL_TYPE_BOOL || t == GLSL_TYPE_INT ||
                       t == GLSL_TYPE_FLOAT);
}
static GLboolean is_sampler(glsl_type_t t) {
    return (GLboolean)(t >= GLSL_TYPE_SAMPLER1D && t <= GLSL_TYPE_SAMPLER2DSHADOW);
}

/* -------------------------------------------------------------------------
 * Structs
 * ------------------------------------------------------------------------- */

const glsl_struct_t *glsl_struct_of(const glsl_sema_t *s, glsl_type_t t) {
    if (!s || !glsl_type_is_struct(t))
        return (const glsl_struct_t *)0;
    const int i = glsl_struct_index(t);
    if (i < 0 || i >= s->struct_count)
        return (const glsl_struct_t *)0;
    return &s->structs[i];
}

const glsl_struct_member_t *glsl_struct_member(const glsl_sema_t *s, glsl_type_t t,
                                               const char *name, size_t len) {
    const glsl_struct_t *st = glsl_struct_of(s, t);
    if (!st)
        return (const glsl_struct_member_t *)0;
    for (int i = 0; i < st->member_count; i++) {
        if (st->member[i].name_len != len)
            continue;
        size_t k = 0;
        while (k < len && st->member[i].name[k] == name[k])
            k++;
        if (k == len)
            return &st->member[i];
    }
    return (const glsl_struct_member_t *)0;
}

/* The struct a name refers to, or GLSL_TYPE_ERROR. */
static glsl_type_t struct_type_by_name(const glsl_sema_t *s, const char *name,
                                       size_t len) {
    for (int i = 0; i < s->struct_count; i++) {
        if (s->structs[i].name_len != len)
            continue;
        size_t k = 0;
        while (k < len && s->structs[i].name[k] == name[k])
            k++;
        if (k == len)
            return glsl_struct_type(i);
    }
    return GLSL_TYPE_ERROR;
}

/* The component count of any type, including a struct, whose size lives in the struct
 * table that `glsl_type_components` cannot see. Every caller that may see a struct asks
 * this one. */
int glsl_type_components_of(const glsl_sema_t *s, glsl_type_t t) {
    const glsl_struct_t *st = glsl_struct_of(s, t);
    if (st)
        return st->components;
    return glsl_type_components(t);
}

int glsl_type_components(glsl_type_t t) {
    switch (t) {
    case GLSL_TYPE_BOOL:
    case GLSL_TYPE_INT:
    case GLSL_TYPE_FLOAT:
        return 1;
    case GLSL_TYPE_VEC2:
    case GLSL_TYPE_IVEC2:
    case GLSL_TYPE_BVEC2:
        return 2;
    case GLSL_TYPE_VEC3:
    case GLSL_TYPE_IVEC3:
    case GLSL_TYPE_BVEC3:
        return 3;
    case GLSL_TYPE_VEC4:
    case GLSL_TYPE_IVEC4:
    case GLSL_TYPE_BVEC4:
        return 4;
    case GLSL_TYPE_MAT2:
        return 4;
    case GLSL_TYPE_MAT3:
        return 9;
    case GLSL_TYPE_MAT4:
        return 16;
    /* C columns of R rows, laid out column-major. */
    case GLSL_TYPE_MAT2X3:
        return 6;
    case GLSL_TYPE_MAT2X4:
        return 8;
    case GLSL_TYPE_MAT3X2:
        return 6;
    case GLSL_TYPE_MAT3X4:
        return 12;
    case GLSL_TYPE_MAT4X2:
        return 8;
    case GLSL_TYPE_MAT4X3:
        return 12;
    default:
        return 0;
    }
}

/* The scalar a vector is made of: `vec3` is float, `ivec2` is int, `bvec4` is bool. */
static glsl_type_t base_of(glsl_type_t t) {
    if (t >= GLSL_TYPE_VEC2 && t <= GLSL_TYPE_VEC4)
        return GLSL_TYPE_FLOAT;
    if (t >= GLSL_TYPE_IVEC2 && t <= GLSL_TYPE_IVEC4)
        return GLSL_TYPE_INT;
    if (t >= GLSL_TYPE_BVEC2 && t <= GLSL_TYPE_BVEC4)
        return GLSL_TYPE_BOOL;
    if (is_matrix(t))
        return GLSL_TYPE_FLOAT;
    return t;
}

/* The vector of `base` with `n` components; `n == 1` gives the scalar back. */
static glsl_type_t vector_of(glsl_type_t base, int n) {
    if (n == 1)
        return base;
    if (base == GLSL_TYPE_FLOAT)
        return (glsl_type_t)(GLSL_TYPE_VEC2 + (n - 2));
    if (base == GLSL_TYPE_INT)
        return (glsl_type_t)(GLSL_TYPE_IVEC2 + (n - 2));
    if (base == GLSL_TYPE_BOOL)
        return (glsl_type_t)(GLSL_TYPE_BVEC2 + (n - 2));
    return GLSL_TYPE_ERROR;
}

void glsl_sema_fail(glsl_sema_t *s, const char *why, int32_t node) {
    sema_fail(s, why, node);
}
GLboolean glsl_type_is_vector(glsl_type_t t) {
    return is_vector(t);
}
GLboolean glsl_type_is_matrix(glsl_type_t t) {
    return is_matrix(t);
}
GLboolean glsl_type_is_sampler(glsl_type_t t) {
    return is_sampler(t);
}
glsl_type_t glsl_type_base(glsl_type_t t) {
    return base_of(t);
}
glsl_type_t glsl_type_vector_of(glsl_type_t base, int n) {
    return vector_of(base, n);
}
int glsl_type_matrix_cols(glsl_type_t t) {
    return matrix_cols(t);
}
int glsl_type_matrix_rows(glsl_type_t t) {
    return matrix_rows(t);
}
glsl_type_t glsl_type_matrix_of(int cols, int rows) {
    return matrix_of(cols, rows);
}

/* -------------------------------------------------------------------------
 * Scopes
 * ------------------------------------------------------------------------- */

void glsl_scope_push(glsl_sema_t *s) {
    if (s)
        s->scope++;
}

void glsl_scope_pop(glsl_sema_t *s) {
    if (!s || s->scope == 0)
        return;
    /* Everything declared at this depth goes. The table is a stack, so dropping the
     * tail is enough. */
    while (s->count > 0 && s->symbols[s->count - 1].scope >= s->scope)
        s->count--;
    s->scope--;
}

static GLboolean same_name(const glsl_symbol_t *sym, const char *name, size_t len) {
    if (sym->name_len != len)
        return GL_FALSE;
    for (size_t i = 0; i < len; i++) {
        if (sym->name[i] != name[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

static const glsl_symbol_t *lookup(const glsl_sema_t *s, const char *name, size_t len) {
    /* Backwards, so an inner declaration shadows an outer one of the same name. */
    for (int i = s->count - 1; i >= 0; i--) {
        if (same_name(&s->symbols[i], name, len))
            return &s->symbols[i];
    }
    return (const glsl_symbol_t *)0;
}

/*
 * An integral constant expression (GLSL 4.1.9), which an array's length is. Folded
 * rather than demanded as a literal, because `const int N = 8; uniform vec2 offs[N];`
 * is common in shaders (mesa-demos' `vpglsl`).
 *
 * What folds: an integer literal, a `const` integer scalar whose own initialiser
 * folded, and arithmetic over those. Anything else, division by zero included, returns
 * false and the caller refuses the declaration: a guessed length would index out of
 * bounds at run time instead of producing a diagnostic.
 */
static GLboolean const_int_eval(const glsl_sema_t *s, int32_t node, int *out) {
    if (node == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *n = &s->ast->nodes[node];
    switch (n->kind) {
    case GLSL_NODE_INTCONST:
        *out = (int)n->value;
        return GL_TRUE;
    case GLSL_NODE_IDENTIFIER: {
        const glsl_symbol_t *sym = lookup(s, n->text, n->length);
        if (!sym || !sym->has_const_int)
            return GL_FALSE;
        *out = sym->const_int;
        return GL_TRUE;
    }
    case GLSL_NODE_UNARY: {
        int v = 0;
        if (!const_int_eval(s, n->a, &v))
            return GL_FALSE;
        if (n->op == GLSL_TOK_MINUS) {
            *out = -v;
            return GL_TRUE;
        }
        if (n->op == GLSL_TOK_PLUS) {
            *out = v;
            return GL_TRUE;
        }
        return GL_FALSE;
    }
    case GLSL_NODE_BINARY: {
        int l = 0, r = 0;
        if (!const_int_eval(s, n->a, &l) || !const_int_eval(s, n->b, &r))
            return GL_FALSE;
        switch (n->op) {
        case GLSL_TOK_PLUS:
            *out = l + r;
            return GL_TRUE;
        case GLSL_TOK_MINUS:
            *out = l - r;
            return GL_TRUE;
        case GLSL_TOK_STAR:
            *out = l * r;
            return GL_TRUE;
        case GLSL_TOK_SLASH:
            if (r == 0)
                return GL_FALSE;
            *out = l / r;
            return GL_TRUE;
        case GLSL_TOK_PERCENT:
            if (r == 0)
                return GL_FALSE;
            *out = l % r;
            return GL_TRUE;
        default:
            return GL_FALSE;
        }
    }
    default:
        return GL_FALSE;
    }
}

GLboolean glsl_declare(glsl_sema_t *s, const char *name, size_t len, glsl_type_t type,
                       GLboolean is_function) {
    if (!s)
        return GL_FALSE;
    if (s->count >= GLSL_MAX_SYMBOLS) {
        sema_fail(s, "too many declarations", GLSL_NO_NODE);
        return GL_FALSE;
    }
    /* Redeclaration is an error only in the same scope; shadowing an outer name is
     * legal. Two functions of one name are exempt, because GLSL overloads by signature
     * and `glsl_declare_function` compares the parameters. A function and a variable
     * sharing a name stay an error. */
    for (int i = s->count - 1; i >= 0; i--) {
        if (s->symbols[i].scope < s->scope)
            break;
        if (!same_name(&s->symbols[i], name, len))
            continue;
        if (is_function && s->symbols[i].is_function)
            continue;
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
    /* Cleared, because the table is reused and not zeroed: a slot that held a
     * `const int` would otherwise hand its value to the next declaration. */
    s->symbols[s->count].has_const_int = GL_FALSE;
    s->symbols[s->count].const_int = 0;
    s->symbols[s->count].param_count = 0;
    s->symbols[s->count].decl_node = GLSL_NO_NODE;
    s->count++;
    return GL_TRUE;
}

/* A `const int` with a known value: an array's length may be one, and so may a loop's
 * bound. GLSL 7.4's built-in constants come in this way. */
GLboolean glsl_declare_const_int(glsl_sema_t *s, const char *name, size_t len,
                                 int value) {
    if (!glsl_declare(s, name, len, GLSL_TYPE_INT, GL_FALSE))
        return GL_FALSE;
    s->symbols[s->count - 1].qualifier = GLSL_TOK_KW_CONST;
    s->symbols[s->count - 1].has_const_int = GL_TRUE;
    s->symbols[s->count - 1].const_int = value;
    return GL_TRUE;
}

GLboolean glsl_declare_array(glsl_sema_t *s, const char *name, size_t len,
                             glsl_type_t type, int count, glsl_token_type_t qualifier) {
    if (!glsl_declare(s, name, len, type, GL_FALSE))
        return GL_FALSE;
    s->symbols[s->count - 1].array_size = count;
    s->symbols[s->count - 1].qualifier = qualifier;
    return GL_TRUE;
}

/* -------------------------------------------------------------------------
 * Swizzles
 *
 * `.xyz`, `.rgb` and `.stp` name the same components through three vocabularies, for
 * position, colour and texture coordinates. They may not be mixed: `v.xg` is an error.
 * ------------------------------------------------------------------------- */

/* Returns the component index 0..3, or -1. `set` receives 0/1/2 for xyzw/rgba/stpq. */
static int swizzle_component(char c, int *set) {
    switch (c) {
    case 'x':
        *set = 0;
        return 0;
    case 'y':
        *set = 0;
        return 1;
    case 'z':
        *set = 0;
        return 2;
    case 'w':
        *set = 0;
        return 3;
    case 'r':
        *set = 1;
        return 0;
    case 'g':
        *set = 1;
        return 1;
    case 'b':
        *set = 1;
        return 2;
    case 'a':
        *set = 1;
        return 3;
    case 's':
        *set = 2;
        return 0;
    case 't':
        *set = 2;
        return 1;
    case 'p':
        *set = 2;
        return 2;
    case 'q':
        *set = 2;
        return 3;
    default:
        return -1;
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
        /* Past the end of the operand: `.w` on a vec3. */
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

/* The type a name in call position constructs, or ERROR if it names no type. `matNxN`
 * is the long spelling of `matN`. */
glsl_type_t glsl_type_by_ctor_name(const char *name, size_t len);
static glsl_type_t type_by_ctor_name(const char *name, size_t len) {
    static const struct {
        const char *name;
        glsl_type_t type;
    } ctors[] = {
        {"float", GLSL_TYPE_FLOAT},   {"int", GLSL_TYPE_INT},
        {"bool", GLSL_TYPE_BOOL},     {"vec2", GLSL_TYPE_VEC2},
        {"vec3", GLSL_TYPE_VEC3},     {"vec4", GLSL_TYPE_VEC4},
        {"ivec2", GLSL_TYPE_IVEC2},   {"ivec3", GLSL_TYPE_IVEC3},
        {"ivec4", GLSL_TYPE_IVEC4},   {"bvec2", GLSL_TYPE_BVEC2},
        {"bvec3", GLSL_TYPE_BVEC3},   {"bvec4", GLSL_TYPE_BVEC4},
        {"mat2", GLSL_TYPE_MAT2},     {"mat3", GLSL_TYPE_MAT3},
        {"mat4", GLSL_TYPE_MAT4},     {"mat2x2", GLSL_TYPE_MAT2},
        {"mat3x3", GLSL_TYPE_MAT3},   {"mat4x4", GLSL_TYPE_MAT4},
        {"mat2x3", GLSL_TYPE_MAT2X3}, {"mat2x4", GLSL_TYPE_MAT2X4},
        {"mat3x2", GLSL_TYPE_MAT3X2}, {"mat3x4", GLSL_TYPE_MAT3X4},
        {"mat4x2", GLSL_TYPE_MAT4X2}, {"mat4x3", GLSL_TYPE_MAT4X3},
    };
    for (size_t i = 0; i < sizeof(ctors) / sizeof(ctors[0]); i++) {
        size_t k = 0;
        while (ctors[i].name[k] != '\0')
            k++;
        if (k != len)
            continue;
        GLboolean match = GL_TRUE;
        for (size_t c = 0; c < len; c++) {
            if (name[c] != ctors[i].name[c]) {
                match = GL_FALSE;
                break;
            }
        }
        if (match)
            return ctors[i].type;
    }
    return GLSL_TYPE_ERROR;
}

/*
 * A GLSL 1.20 array constructor, `T[N](a, b, ...)`. The parser produces it as a call
 * whose callee is an index, `CALL(INDEX(IDENTIFIER "float", 2), args)`.
 *
 * Answers the element type and the length, not an array type: this type system has
 * none. A symbol carries `(type, array_size)` and an expression carries a type alone,
 * so an array constructor is usable only as a declaration's initialiser, where the
 * declaration receives the length. As an argument or a return value it is refused.
 */
GLboolean glsl_array_ctor_of(glsl_sema_t *s, int32_t node, glsl_type_t *elem,
                             int *count) {
    if (!s || node == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *n = &s->ast->nodes[node];
    if (n->kind != GLSL_NODE_CALL || n->a == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *idx = &s->ast->nodes[n->a];
    if (idx->kind != GLSL_NODE_INDEX || idx->a == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *name = &s->ast->nodes[idx->a];
    if (name->kind != GLSL_NODE_IDENTIFIER)
        return GL_FALSE;
    const glsl_type_t t = type_by_ctor_name(name->text, name->length);
    if (t == GLSL_TYPE_ERROR)
        return GL_FALSE;
    int len = 0;
    if (idx->b == GLSL_NO_NODE || !const_int_eval(s, idx->b, &len) || len < 1) {
        /* The unsized `float[](a, b)`, or a length that does not fold. */
        sema_fail(
            s,
            "an array constructor needs a constant length: `float[2](a, b)`, not an "
            "empty or computed one",
            node);
        return GL_FALSE;
    }
    /* Folded back into the tree, as a declarator's length is: both back ends read this
     * node, and it lets `glsl_array_ctor_shape` answer from the AST alone. */
    s->ast->nodes[idx->b].kind = GLSL_NODE_INTCONST;
    s->ast->nodes[idx->b].value = (double)len;
    *elem = t;
    *count = len;
    return GL_TRUE;
}

/* The same shape, asked of the tree alone, which is what both back ends have. The
 * semantic pass has already folded the length to an `INTCONST`. */
GLboolean glsl_array_ctor_shape(const glsl_ast_t *ast, int32_t node, glsl_type_t *elem,
                                int *count) {
    if (!ast || node == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *n = &ast->nodes[node];
    if (n->kind != GLSL_NODE_CALL || n->a == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *idx = &ast->nodes[n->a];
    if (idx->kind != GLSL_NODE_INDEX || idx->a == GLSL_NO_NODE ||
        idx->b == GLSL_NO_NODE) {
        return GL_FALSE;
    }
    const glsl_node_t *name = &ast->nodes[idx->a];
    const glsl_node_t *len = &ast->nodes[idx->b];
    if (name->kind != GLSL_NODE_IDENTIFIER || len->kind != GLSL_NODE_INTCONST)
        return GL_FALSE;
    const glsl_type_t t = type_by_ctor_name(name->text, name->length);
    if (t == GLSL_TYPE_ERROR || (int)len->value < 1)
        return GL_FALSE;
    *elem = t;
    *count = (int)len->value;
    return GL_TRUE;
}

/* Whether this is an array constructor's syntax, whatever its length folds to: a call
 * on an index into a type name. Used to pick a diagnostic that names an array
 * constructor rather than "calling something that is not a name". */
static GLboolean is_array_ctor_syntax(const glsl_sema_t *s, int32_t node) {
    const glsl_node_t *n = &s->ast->nodes[node];
    if (n->kind != GLSL_NODE_CALL || n->a == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *idx = &s->ast->nodes[n->a];
    if (idx->kind != GLSL_NODE_INDEX || idx->a == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *name = &s->ast->nodes[idx->a];
    return (GLboolean)(name->kind == GLSL_NODE_IDENTIFIER &&
                       type_by_ctor_name(name->text, name->length) != GLSL_TYPE_ERROR);
}

/* The arity and element types of an array constructor. Unlike `vec4(1.0)` there is no
 * filling rule: one argument per element (1.20, 5.4.4). */
static GLboolean check_array_ctor(glsl_sema_t *s, int32_t node, glsl_type_t elem,
                                  int count) {
    const glsl_node_t *n = &s->ast->nodes[node];
    if (s->version < 120) {
        sema_fail(s, "an array constructor is GLSL 1.20; this shader is 1.10", node);
        return GL_FALSE;
    }
    int given = 0;
    for (int32_t a = n->b; a != GLSL_NO_NODE; a = s->ast->nodes[a].sibling) {
        const glsl_type_t at = glsl_type_of(s, a);
        if (at == GLSL_TYPE_ERROR)
            return GL_FALSE;
        if (!glsl_type_accepts(s, elem, at)) {
            sema_fail(s,
                      "an array constructor's argument does not match its element type",
                      a);
            return GL_FALSE;
        }
        given++;
    }
    if (given != count) {
        sema_fail(s, "an array constructor takes exactly one argument per element",
                  node);
        return GL_FALSE;
    }
    return GL_TRUE;
}

/* A constructor: `vec3(1.0)`, `vec4(v, 1.0)`, `mat4(1.0)`.
 *
 * GLSL counts components, not arguments: `vec4(v3, 1.0)` is legal, `vec4(1.0)` fills
 * all four, and `vec4(1.0, 2.0)` is too few. */
static glsl_type_t constructor_type(glsl_sema_t *s, glsl_type_t target,
                                    int32_t first_arg, int32_t node) {
    int supplied = 0;
    int args = 0;
    for (int32_t a = first_arg; a != GLSL_NO_NODE; a = s->ast->nodes[a].sibling) {
        glsl_type_t at = glsl_type_of(s, a);
        if (at == GLSL_TYPE_ERROR)
            return GLSL_TYPE_ERROR;
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
    /* One scalar fills everything: `vec4(0.0)`, and `mat4(1.0)`, the identity. */
    if (args == 1 && supplied == 1)
        return target;
    if (supplied < needed) {
        sema_fail(s, "too few components for this constructor", node);
        return GLSL_TYPE_ERROR;
    }
    /* A single larger argument drops its trailing components; several arguments summing
     * past the target are an error. */
    if (supplied > needed && args > 1) {
        sema_fail(s, "too many components for this constructor", node);
        return GLSL_TYPE_ERROR;
    }
    return target;
}

static glsl_type_t binary_type(glsl_sema_t *s, glsl_token_type_t op, glsl_type_t l,
                               glsl_type_t r, int32_t node) {
    if (l == GLSL_TYPE_ERROR || r == GLSL_TYPE_ERROR)
        return GLSL_TYPE_ERROR;

    /* One side float and the other int widens the int one (GLSL 1.20). Done once here,
     * so every rule below sees a pair that agrees on its base type. `ivec2 == vec3`
     * widens and is still a width mismatch. */
    if (s && s->version >= 120) {
        const glsl_type_t lb = base_of(l), rb = base_of(r);
        if (lb == GLSL_TYPE_FLOAT && rb == GLSL_TYPE_INT)
            r = widen_to_float(r);
        else if (rb == GLSL_TYPE_FLOAT && lb == GLSL_TYPE_INT)
            l = widen_to_float(l);
    }

    switch (op) {
    case GLSL_TOK_AND_AND:
    case GLSL_TOK_OR_OR:
    case GLSL_TOK_XOR_XOR:
        if (l != GLSL_TYPE_BOOL || r != GLSL_TYPE_BOOL) {
            sema_fail(s, "the logical operators take bool", node);
            return GLSL_TYPE_ERROR;
        }
        return GLSL_TYPE_BOOL;

    case GLSL_TOK_EQ:
    case GLSL_TOK_NE: {
        /* Arrays are asked about first: `glsl_type_of` answers a whole array with its
         * element type, so two arrays of different lengths would otherwise compare as
         * FLOAT against FLOAT. */
        const glsl_node_t *bn = &s->ast->nodes[node];
        int count = 0;
        GLboolean mismatch = GL_FALSE;
        if (both_whole_arrays(s, bn->a, bn->b, &count, &mismatch)) {
            if (s->version < 120) {
                sema_fail(s, "GLSL 1.10 does not compare arrays (5.9); 1.20 does",
                          node);
                return GLSL_TYPE_ERROR;
            }
            return GLSL_TYPE_BOOL;
        }
        if (mismatch) {
            sema_fail(s,
                      "== and != on arrays need the same element type and the same "
                      "length",
                      node);
            return GLSL_TYPE_ERROR;
        }
        if (l != r) {
            sema_fail(s, "== and != need both sides to be the same type", node);
            return GLSL_TYPE_ERROR;
        }
        return GLSL_TYPE_BOOL;
    }

    case GLSL_TOK_LT:
    case GLSL_TOK_GT:
    case GLSL_TOK_LE:
    case GLSL_TOK_GE:
        /* Ordering is scalars only; vectors use `lessThan()`. */
        if (!(l == r && (l == GLSL_TYPE_INT || l == GLSL_TYPE_FLOAT))) {
            sema_fail(s, "< > <= >= compare int with int or float with float", node);
            return GLSL_TYPE_ERROR;
        }
        return GLSL_TYPE_BOOL;

    default:
        break;
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

    /* `matCxR * vecC` is `vecR`: the vector is a column. `vecR * matCxR` is the product
     * with the transpose: the vector is a row, and the result has one entry per
     * column. */
    if (op == GLSL_TOK_STAR && is_matrix(l) && is_vector(r)) {
        if (glsl_type_components(r) != matrix_cols(l) ||
            base_of(r) != GLSL_TYPE_FLOAT) {
            sema_fail(s, "matrix and vector dimensions do not meet", node);
            return GLSL_TYPE_ERROR;
        }
        return vector_of(GLSL_TYPE_FLOAT, matrix_rows(l));
    }
    if (op == GLSL_TOK_STAR && is_vector(l) && is_matrix(r)) {
        if (glsl_type_components(l) != matrix_rows(r) ||
            base_of(l) != GLSL_TYPE_FLOAT) {
            sema_fail(s, "vector and matrix dimensions do not meet", node);
            return GLSL_TYPE_ERROR;
        }
        return vector_of(GLSL_TYPE_FLOAT, matrix_cols(r));
    }
    /* `matCxR * matPxC` is `matPxR`: the left's columns match the right's rows, and the
     * result takes the right's columns and the left's rows. */
    if (op == GLSL_TOK_STAR && is_matrix(l) && is_matrix(r)) {
        if (matrix_cols(l) != matrix_rows(r)) {
            sema_fail(
                s,
                "matrix dimensions do not meet: the left's columns and the right's "
                "rows have to be the same number",
                node);
            return GLSL_TYPE_ERROR;
        }
        const glsl_type_t product = matrix_of(matrix_cols(r), matrix_rows(l));
        if (product == GLSL_TYPE_ERROR) {
            sema_fail(s, "this matrix product has no type in GLSL 1.20", node);
            return GLSL_TYPE_ERROR;
        }
        return product;
    }

    /* A scalar against a vector or matrix is component-wise and keeps the larger type,
     * in either order. */
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

    /* No implicit conversion: `1 + 1.0` is an error in GLSL 1.10. */
    if (l != r) {
        sema_fail(s, "operands of different types, and GLSL 1.10 converts neither",
                  node);
        return GLSL_TYPE_ERROR;
    }
    return l;
}

glsl_type_t glsl_type_of(glsl_sema_t *s, int32_t node) {
    if (!s || !s->ast || node == GLSL_NO_NODE)
        return GLSL_TYPE_ERROR;
    if (s->error)
        return GLSL_TYPE_ERROR;

    const glsl_node_t *n = &s->ast->nodes[node];
    switch (n->kind) {
    case GLSL_NODE_INTCONST:
        return GLSL_TYPE_INT;
    case GLSL_NODE_FLOATCONST:
        return GLSL_TYPE_FLOAT;
    case GLSL_NODE_BOOLCONST:
        return GLSL_TYPE_BOOL;

    case GLSL_NODE_IDENTIFIER: {
        /* A type name used as an identifier is a constructor being named; it only has a
         * type when it is called, which the CALL arm below handles. */
        const glsl_symbol_t *sym = lookup(s, n->text, n->length);
        if (!sym) {
            /* A real `gl_` name this implementation does not provide gets a diagnostic
             * that says why, rather than "undeclared name", which suggests a typo. */
            const char *why = glsl_builtin_refusal(n->text, n->length);
            sema_fail(s, why ? why : "use of an undeclared name", node);
            return GLSL_TYPE_ERROR;
        }
        return sym->type;
    }

    case GLSL_NODE_FIELD: {
        const glsl_type_t base = glsl_type_of(s, n->a);
        if (base == GLSL_TYPE_ERROR)
            return GLSL_TYPE_ERROR;
        /* A struct member and a swizzle share the syntax; the base's type decides
         * which `v.xy` or `s.a` is. */
        if (glsl_type_is_struct(base)) {
            const glsl_struct_member_t *m =
                glsl_struct_member(s, base, n->text, n->length);
            if (!m) {
                sema_fail(s, "this struct has no member of that name", node);
                return GLSL_TYPE_ERROR;
            }
            /* An array member's type is its element type, as an array variable's is in
             * the symbol table. */
            return m->type;
        }
        return swizzle_type(s, base, n->text, n->length, node);
    }

    case GLSL_NODE_INDEX: {
        /* An array name is resolved before its type: `vec4 v[4]` has type `vec4` in
         * the table, and typing the operand first would index the vec4 and answer a
         * float. An array is only ever indexed through its own name or member. */
        {
            const glsl_node_t *base_n = &s->ast->nodes[n->a];
            if (base_n->kind == GLSL_NODE_IDENTIFIER) {
                const glsl_symbol_t *sym = lookup(s, base_n->text, base_n->length);
                if (sym && sym->array_size > 0 && !sym->is_function) {
                    glsl_type_t idx_t = glsl_type_of(s, n->b);
                    if (idx_t == GLSL_TYPE_ERROR)
                        return GLSL_TYPE_ERROR;
                    if (idx_t != GLSL_TYPE_INT) {
                        sema_fail(s, "an index must be an int", node);
                        return GLSL_TYPE_ERROR;
                    }
                    /* A constant index past the end is a compile error (1.10, 4.1.9).
                     * A variable one is checked when it runs. */
                    const glsl_node_t *idx_n = &s->ast->nodes[n->b];
                    if (idx_n->kind == GLSL_NODE_INTCONST &&
                        ((int)idx_n->value < 0 ||
                         (int)idx_n->value >= sym->array_size)) {
                        sema_fail(s, "array index out of range", node);
                        return GLSL_TYPE_ERROR;
                    }
                    return sym->type;
                }
            }
            /* An array struct member (1.10, 4.1.9) is indexed the same way: `s.w[0]`
             * has a field underneath. Both back ends lay the elements out end to end
             * from the member's offset. */
            if (base_n->kind == GLSL_NODE_FIELD) {
                const glsl_type_t owner = glsl_type_of(s, base_n->a);
                if (owner == GLSL_TYPE_ERROR)
                    return GLSL_TYPE_ERROR;
                if (glsl_type_is_struct(owner)) {
                    const glsl_struct_member_t *m =
                        glsl_struct_member(s, owner, base_n->text, base_n->length);
                    if (m && m->array_size > 0) {
                        glsl_type_t idx_t = glsl_type_of(s, n->b);
                        if (idx_t == GLSL_TYPE_ERROR)
                            return GLSL_TYPE_ERROR;
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
        if (base == GLSL_TYPE_ERROR || idx == GLSL_TYPE_ERROR)
            return GLSL_TYPE_ERROR;
        if (idx != GLSL_TYPE_INT) {
            sema_fail(s, "an index must be an int", node);
            return GLSL_TYPE_ERROR;
        }
        /* Indexing a matrix gives a column, which has one entry per row: `mat2x3[0]`
         * is a `vec3`. Indexing a vector gives a component. */
        if (is_matrix(base))
            return vector_of(GLSL_TYPE_FLOAT, matrix_rows(base));
        if (is_vector(base))
            return base_of(base);
        sema_fail(s, "indexing something that is neither a vector nor a matrix", node);
        return GLSL_TYPE_ERROR;
    }

    case GLSL_NODE_CALL: {
        const glsl_node_t *callee = &s->ast->nodes[n->a];
        if (callee->kind != GLSL_NODE_IDENTIFIER) {
            /* An array constructor used as a general expression. It is legal only as a
             * declaration's initialiser, which `check_declarator` handles without
             * asking this function; as an argument or a return value it would need an
             * array type, which this type system does not have. */
            glsl_type_t ael = GLSL_TYPE_ERROR;
            int acount = 0;
            if (glsl_array_ctor_shape(s->ast, node, &ael, &acount) ||
                is_array_ctor_syntax(s, node)) {
                sema_fail(s,
                          "an array constructor is a declaration's initialiser here - "
                          "1.20 also allows one as an argument and a return value, and "
                          "those are not implemented",
                          node);
                return GLSL_TYPE_ERROR;
            }
            sema_fail(s, "calling something that is not a name", node);
            return GLSL_TYPE_ERROR;
        }
        /* A constructor is a type name in call position. Checked before the symbol
         * table, because `vec4` is never declared as a function. */
        const glsl_type_t ctor = type_by_ctor_name(callee->text, callee->length);
        /* A non-square matrix constructor is 1.20's. The parser refuses the
         * declaration; this refuses `mat2x3(1.0)[0]` in an expression. */
        if (ctor != GLSL_TYPE_ERROR && s->version != 0 && s->version < 120 &&
            matrix_cols(ctor) != matrix_rows(ctor)) {
            sema_fail(s,
                      "the non-square matrix types are GLSL 1.20; this shader is 1.10",
                      node);
            return GLSL_TYPE_ERROR;
        }
        if (ctor != GLSL_TYPE_ERROR)
            return constructor_type(s, ctor, n->b, node);

        /* A struct constructor takes one argument per member, in order, each assignable
         * to that member (1.10, 5.4.3), with none of the built-in constructors'
         * filling, gathering or truncating. */
        {
            const glsl_type_t st_type =
                struct_type_by_name(s, callee->text, callee->length);
            if (st_type != GLSL_TYPE_ERROR) {
                const glsl_struct_t *st = glsl_struct_of(s, st_type);
                int i = 0;
                for (int32_t a = n->b; a != GLSL_NO_NODE;
                     a = s->ast->nodes[a].sibling, i++) {
                    const glsl_type_t at = glsl_type_of(s, a);
                    if (at == GLSL_TYPE_ERROR)
                        return GLSL_TYPE_ERROR;
                    if (i >= st->member_count) {
                        sema_fail(s, "too many arguments for this struct's constructor",
                                  node);
                        return GLSL_TYPE_ERROR;
                    }
                    /* GLSL 1.10 has no array-valued expression, so a struct with an
                     * array member has no constructor that can be written. */
                    if (st->member[i].array_size > 0) {
                        sema_fail(s, "a struct with an array member has no constructor",
                                  node);
                        return GLSL_TYPE_ERROR;
                    }
                    if (!glsl_type_accepts(s, st->member[i].type, at)) {
                        sema_fail(s, "this argument does not match the struct's member",
                                  a);
                        return GLSL_TYPE_ERROR;
                    }
                }
                if (i != st->member_count) {
                    sema_fail(s, "too few arguments for this struct's constructor",
                              node);
                    return GLSL_TYPE_ERROR;
                }
                return st_type;
            }
        }

        /* The built-in library, before the symbol table. `sin`, `dot` and `texture2D`
         * are overloaded over genType and resolved by rule, like the constructors. A
         * shader function that reuses a built-in's name (legal in GLSL 1.10) loses to
         * the built-in here; the rare case is traded for not searching the table on
         * every built-in call. */
        {
            glsl_type_t bargs[GLSL_MAX_PARAMS];
            int bargc = 0;
            GLboolean too_many = GL_FALSE;
            for (int32_t a = n->b; a != GLSL_NO_NODE; a = s->ast->nodes[a].sibling) {
                glsl_type_t at = glsl_type_of(s, a);
                if (at == GLSL_TYPE_ERROR)
                    return GLSL_TYPE_ERROR;
                if (bargc >= GLSL_MAX_PARAMS) {
                    too_many = GL_TRUE;
                    break;
                }
                /* Every integer argument widens under 1.20, so `mod(x, 2)` and
                 * `clamp(v, 0, 1)` resolve. No 1.10 or 1.20 built-in takes an integer,
                 * so no overload is lost. */
                bargs[bargc++] = (s->version >= 120) ? widen_to_float(at) : at;
            }
            if (!too_many) {
                GLboolean found = GL_FALSE;
                glsl_type_t bt = glsl_builtin_call_type(s, callee->text, callee->length,
                                                        bargs, bargc, node, &found);
                if (found)
                    return bt;
            }
        }

        /* Overload resolution. The arguments are typed once and offered to every
         * function of that name. An exact match wins; a match needing 1.20's
         * int-to-float is taken only when nothing matched exactly, so `f(int)` is not
         * passed over for `f(float)`. */
        glsl_type_t args[GLSL_MAX_PARAMS];
        int given = 0;
        for (int32_t a = n->b; a != GLSL_NO_NODE; a = s->ast->nodes[a].sibling) {
            glsl_type_t at = glsl_type_of(s, a);
            if (at == GLSL_TYPE_ERROR)
                return GLSL_TYPE_ERROR;
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
            if (!same_name(cand, callee->text, callee->length))
                continue;
            any_name = GL_TRUE;
            if (!cand->is_function) {
                any_non_function = GL_TRUE;
                continue;
            }
            if (cand->param_count != given)
                continue;
            GLboolean fits = GL_TRUE, identical = GL_TRUE;
            for (int k = 0; k < given; k++) {
                if (cand->params[k] != args[k])
                    identical = GL_FALSE;
                if (!glsl_type_accepts(s, cand->params[k], args[k])) {
                    fits = GL_FALSE;
                    break;
                }
            }
            if (!fits)
                continue;
            if (identical) {
                exact = cand;
                break;
            }
            if (!convert)
                convert = cand;
        }

        if (!any_name) {
            sema_fail(s, "call to an undeclared function", node);
            return GLSL_TYPE_ERROR;
        }
        const glsl_symbol_t *sym = exact ? exact : convert;
        if (!sym) {
            /* Name found, nothing it could mean: a variable called like a function, or
             * an argument list no overload takes. */
            if (any_non_function) {
                sema_fail(s, "calling something that is not a function", node);
            } else {
                sema_fail(s, "no version of this function takes these arguments", node);
            }
            return GLSL_TYPE_ERROR;
        }
        /* The decision is written onto the call, so the back ends do not repeat it. */
        s->ast->nodes[node].resolved = sym->decl_node;
        return sym->type;
    }

    case GLSL_NODE_UNARY: {
        glsl_type_t t = glsl_type_of(s, n->a);
        if (t == GLSL_TYPE_ERROR)
            return GLSL_TYPE_ERROR;
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
        if (t == GLSL_TYPE_ERROR)
            return GLSL_TYPE_ERROR;
        if (base_of(t) == GLSL_TYPE_BOOL || is_sampler(t)) {
            sema_fail(s, "++ and -- need a numeric operand", node);
            return GLSL_TYPE_ERROR;
        }
        return t;
    }

    case GLSL_NODE_BINARY:
        return binary_type(s, n->op, glsl_type_of(s, n->a), glsl_type_of(s, n->b),
                           node);

    case GLSL_NODE_ASSIGN: {
        glsl_type_t l = glsl_type_of(s, n->a);
        glsl_type_t r = glsl_type_of(s, n->b);
        if (l == GLSL_TYPE_ERROR || r == GLSL_TYPE_ERROR)
            return GLSL_TYPE_ERROR;
        if (n->op != GLSL_TOK_ASSIGN) {
            /* `a += b` is `a = a + b`, so the arithmetic rules decide first. */
            glsl_token_type_t arith = n->op == GLSL_TOK_ADD_ASSIGN   ? GLSL_TOK_PLUS
                                      : n->op == GLSL_TOK_SUB_ASSIGN ? GLSL_TOK_MINUS
                                      : n->op == GLSL_TOK_MUL_ASSIGN ? GLSL_TOK_STAR
                                                                     : GLSL_TOK_SLASH;
            r = binary_type(s, arith, l, r, node);
            if (r == GLSL_TYPE_ERROR)
                return GLSL_TYPE_ERROR;
        }
        /* The conversion goes one way, into the variable's type: `float f; f = 1;`
         * is legal in 1.20 and `int i; i = 1.0;` is not, in either version. */
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
        /* Either branch may widen to the other's type, so `b ? 1 : 1.0` is a float
         * in 1.20 and an error in 1.10. */
        if (glsl_type_accepts(s, y, no))
            return y;
        if (glsl_type_accepts(s, no, y))
            return no;
        sema_fail(s, "the two branches of ?: have different types", node);
        return GLSL_TYPE_ERROR;
    }

    case GLSL_NODE_SEQUENCE: {
        glsl_type_t a = glsl_type_of(s, n->a);
        if (a == GLSL_TYPE_ERROR)
            return GLSL_TYPE_ERROR;
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
 * What may sit on the left of an assignment. A swizzle with a repeated component is not
 * assignable: `v.xx = ...` writes two values into one component. A uniform, an
 * attribute or a const is read-only, which is why a symbol carries its storage
 * qualifier.
 * ------------------------------------------------------------------------- */

GLboolean glsl_is_lvalue(glsl_sema_t *s, int32_t node) {
    if (!s || !s->ast || node == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *n = &s->ast->nodes[node];

    switch (n->kind) {
    case GLSL_NODE_IDENTIFIER: {
        const glsl_symbol_t *sym = lookup(s, n->text, n->length);
        if (!sym)
            return GL_FALSE;
        if (sym->is_function)
            return GL_FALSE;
        if (sym->qualifier == GLSL_TOK_KW_UNIFORM ||
            sym->qualifier == GLSL_TOK_KW_ATTRIBUTE ||
            sym->qualifier == GLSL_TOK_KW_CONST) {
            sema_fail(s, "assignment to a read-only variable", node);
            return GL_FALSE;
        }
        return GL_TRUE;
    }
    case GLSL_NODE_INDEX:
        return glsl_is_lvalue(s, n->a);
    case GLSL_NODE_FIELD: {
        if (!glsl_is_lvalue(s, n->a))
            return GL_FALSE;
        /* A repeated component cannot be written. */
        for (size_t i = 0; i < n->length; i++) {
            for (size_t k = i + 1; k < n->length; k++) {
                if (n->text[i] == n->text[k]) {
                    sema_fail(s, "a swizzle that repeats a component is not assignable",
                              node);
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

/* The element type and length of a node that names a whole array, or false: `a` for a
 * declared array, `s.w` for a struct member declared with a length. The type system has
 * no array type, so this is how a rule asks for both halves. */
GLboolean glsl_whole_array_info(glsl_sema_t *s, int32_t node, glsl_type_t *elem,
                                int *size) {
    if (!s || node == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *n = &s->ast->nodes[node];
    if (n->kind == GLSL_NODE_IDENTIFIER) {
        const glsl_symbol_t *sym = lookup(s, n->text, n->length);
        if (!sym || sym->is_function || sym->array_size <= 0)
            return GL_FALSE;
        *elem = sym->type;
        *size = sym->array_size;
        return GL_TRUE;
    }
    if (n->kind == GLSL_NODE_FIELD) {
        const glsl_type_t owner = glsl_type_of(s, n->a);
        if (!glsl_type_is_struct(owner))
            return GL_FALSE;
        const glsl_struct_member_t *m =
            glsl_struct_member(s, owner, n->text, n->length);
        if (!m || m->array_size <= 0)
            return GL_FALSE;
        *elem = m->type;
        *size = m->array_size;
        return GL_TRUE;
    }
    return GL_FALSE;
}

static GLboolean names_whole_array(glsl_sema_t *s, int32_t node) {
    glsl_type_t elem = GLSL_TYPE_ERROR;
    int size = 0;
    return glsl_whole_array_info(s, node, &elem, &size);
}

/* Two whole arrays of the same element type and length, which 1.20 lets `=`, `==` and
 * `!=` take. False when neither side is an array, so a caller can fall through to the
 * ordinary rules; `mismatch` separates "not arrays" from "arrays that do not match". */
static GLboolean both_whole_arrays(glsl_sema_t *s, int32_t a, int32_t b, int *count,
                                   GLboolean *mismatch) {
    glsl_type_t ea = GLSL_TYPE_ERROR, eb = GLSL_TYPE_ERROR;
    int na = 0, nb = 0;
    const GLboolean la = glsl_whole_array_info(s, a, &ea, &na);
    const GLboolean lb = glsl_whole_array_info(s, b, &eb, &nb);
    *mismatch = GL_FALSE;
    if (!la && !lb)
        return GL_FALSE;
    if (!la || !lb || ea != eb || na != nb) {
        *mismatch = GL_TRUE;
        return GL_FALSE;
    }
    *count = na;
    return GL_TRUE;
}

/* An assignment's target is checked here rather than in glsl_type_of, because only
 * statements ask whether an expression may be written to. Walks the whole expression so
 * a nested assignment is caught too. */
static GLboolean check_assign_targets(glsl_sema_t *s, int32_t node) {
    if (node == GLSL_NO_NODE || s->error)
        return !s->error;
    const glsl_node_t *n = &s->ast->nodes[node];

    if (n->kind == GLSL_NODE_ASSIGN) {
        if (!glsl_is_lvalue(s, n->a)) {
            sema_fail(s, "assignment to something that cannot be assigned to", node);
            return GL_FALSE;
        }
        /* A whole array as the destination. Asked here rather than in `glsl_is_lvalue`,
         * which recurses through the array's name on the way to `a[0]` and so cannot
         * refuse the name itself. */
        if (names_whole_array(s, n->a)) {
            /* GLSL 1.10 5.8 does not list an array as an l-value; 1.20 allows
             * whole-array assignment. */
            if (s->version < 120) {
                sema_fail(
                    s,
                    "an array is assigned an element at a time; GLSL 1.10 does not "
                    "make a whole array an l-value (5.8)",
                    node);
                return GL_FALSE;
            }
            /* Only plain `=`: 1.20 defines no arithmetic on arrays, so `a += b` has no
             * meaning. */
            if (n->op != GLSL_TOK_ASSIGN) {
                sema_fail(
                    s,
                    "a compound assignment on a whole array is not GLSL: 1.20 gives an "
                    "array `=` and not arithmetic",
                    node);
                return GL_FALSE;
            }
            int count = 0;
            GLboolean mismatch = GL_FALSE;
            if (!both_whole_arrays(s, n->a, n->b, &count, &mismatch)) {
                sema_fail(s,
                          mismatch
                              ? "both sides of a whole-array assignment need the same "
                                "element type and the same length"
                              : "a whole array is assigned from a whole array",
                          node);
                return GL_FALSE;
            }
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

    if (!check_assign_targets(s, n->a))
        return GL_FALSE;
    if (!check_assign_targets(s, n->b))
        return GL_FALSE;
    if (!check_assign_targets(s, n->c))
        return GL_FALSE;
    for (int32_t a = n->kind == GLSL_NODE_CALL ? n->b : GLSL_NO_NODE; a != GLSL_NO_NODE;
         a = s->ast->nodes[a].sibling) {
        if (!check_assign_targets(s, a))
            return GL_FALSE;
    }
    return GL_TRUE;
}

/* An expression in statement position: type it, then check what it writes to. */
static GLboolean check_expression(glsl_sema_t *s, int32_t node) {
    if (node == GLSL_NO_NODE)
        return GL_TRUE;
    if (glsl_type_of(s, node) == GLSL_TYPE_ERROR)
        return GL_FALSE;
    return check_assign_targets(s, node);
}

/* A condition has to be a bool. GLSL does not take "non-zero is true" from C, so an
 * `int` here is an error rather than a conversion. */
static GLboolean check_condition(glsl_sema_t *s, int32_t node, const char *where) {
    if (node == GLSL_NO_NODE)
        return GL_TRUE;
    glsl_type_t t = glsl_type_of(s, node);
    if (t == GLSL_TYPE_ERROR)
        return GL_FALSE;
    if (t != GLSL_TYPE_BOOL) {
        sema_fail(s, where, node);
        return GL_FALSE;
    }
    return check_assign_targets(s, node);
}

/* The type a declaration or parameter node writes: its token, or the struct an
 * identifier token names. Every node-to-type question goes through this, so a struct is
 * accepted wherever a type is. */
static glsl_type_t node_declared_type(const glsl_sema_t *s, const glsl_node_t *n) {
    if (n->type_tok == GLSL_TOK_IDENTIFIER && n->type_name) {
        return struct_type_by_name(s, n->type_name, n->type_name_len);
    }
    return glsl_type_from_token(n->type_tok);
}

/*
 * `struct S { ... };` - records the type, its members and their offsets.
 *
 * Layout is members end to end, in declaration order. Both back ends use these offsets:
 * the interpreter indexes a float array with them and the code generator adds them to a
 * register base.
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
        /* A sampler has no components and cannot be stored, so a struct holding one has
         * no layout. GLSL 1.10 allows it; this implementation refuses it by name. */
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
                while (k < mn->length && st->member[i].name[k] == mn->text[k])
                    k++;
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
    /* Checked once the whole struct is measured, so the message names the struct. */
    if (st->components > GLSL_MAX_STRUCT_COMPONENTS) {
        sema_fail(s, "this struct is larger than a value this implementation can carry",
                  d);
        return GL_FALSE;
    }
    s->struct_count++;
    return GL_TRUE;
}

/* `gl_` is reserved (1.10, 3.7): a shader may not declare a name beginning with it.
 * Checked at the shader's own declarations rather than inside `glsl_declare`, which the
 * built-in tables use to declare the real ones. */
static GLboolean reserved_name(glsl_sema_t *s, const char *name, size_t len,
                               int32_t node) {
    if (len >= 3u && name[0] == 'g' && name[1] == 'l' && name[2] == '_') {
        sema_fail(
            s,
            "a name beginning with `gl_` is reserved and a shader may not declare one",
            node);
        return GL_TRUE;
    }
    return GL_FALSE;
}

/* One declarator, not the chain. `float a, b;` is two DECL nodes linked through
 * `sibling`, which the enclosing list walk already follows; following it here too would
 * declare every name twice. */
static GLboolean check_declarator(glsl_sema_t *s, int32_t d) {
    const glsl_node_t *n = &s->ast->nodes[d];
    glsl_type_t t = node_declared_type(s, n);
    if (reserved_name(s, n->text, n->length, d))
        return GL_FALSE;
    if (t == GLSL_TYPE_ERROR) {
        sema_fail(s, "declaration of an unknown type", d);
        return GL_FALSE;
    }
    if (t == GLSL_TYPE_VOID) {
        sema_fail(s, "a variable cannot be void", d);
        return GL_FALSE;
    }
    /* The initialiser is typed before the name is declared, so `float x = x;` is an
     * error rather than a variable initialised from itself. */
    if (n->a != GLSL_NO_NODE) {
        /* An array constructor is handled here and nowhere else: a declaration is the
         * one context that supplies the length its value needs. */
        glsl_type_t ael = GLSL_TYPE_ERROR;
        int acount = 0;
        if (is_array_ctor_syntax(s, n->a)) {
            if (n->array_size == GLSL_NO_NODE) {
                sema_fail(
                    s, "an array constructor initialises an array, and this is not one",
                    d);
                return GL_FALSE;
            }
            if (!glsl_array_ctor_of(s, n->a, &ael, &acount))
                return GL_FALSE;
            if (!check_array_ctor(s, n->a, ael, acount))
                return GL_FALSE;
            if (!glsl_type_accepts(s, t, ael)) {
                sema_fail(s, "the array constructor's element type is not the array's",
                          d);
                return GL_FALSE;
            }
            int want = 0;
            if (!const_int_eval(s, n->array_size, &want) || want != acount) {
                sema_fail(s, "the array constructor's length is not the array's", d);
                return GL_FALSE;
            }
        } else {
            glsl_type_t init = glsl_type_of(s, n->a);
            if (init == GLSL_TYPE_ERROR)
                return GL_FALSE;
            if (!glsl_type_accepts(s, t, init)) {
                sema_fail(s, "initialiser of a different type from the variable", d);
                return GL_FALSE;
            }
            if (n->array_size != GLSL_NO_NODE) {
                sema_fail(s,
                          "an array takes an array constructor here, or no initialiser",
                          d);
                return GL_FALSE;
            }
        }
    }
    /* An array's length is an integral constant expression (GLSL 4.1.9), folded by
     * `const_int_eval`. What does not fold is refused rather than assumed, and so is an
     * unsized declaration such as `varying vec4 v[];`. */
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
        /* The fold is written back into the tree. The interpreter, the generator and
         * the linker read an array's length straight off this node and accept only
         * `GLSL_NODE_INTCONST`, so none of them needs a constant folder of its own. */
        s->ast->nodes[n->array_size].kind = GLSL_NODE_INTCONST;
        s->ast->nodes[n->array_size].value = (int32_t)len;
    }
    if (!glsl_declare(s, n->text, n->length, t, GL_FALSE))
        return GL_FALSE;
    s->symbols[s->count - 1].qualifier = n->qualifier;
    s->symbols[s->count - 1].array_size = elements;
    /* A `const int` carries its value forward, so a later array length can name it.
     * Only a non-array integer scalar with a foldable initialiser qualifies, the set
     * 4.1.9 allows in one. */
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
    if (node == GLSL_NO_NODE || s->error)
        return !s->error;
    const glsl_node_t *n = &s->ast->nodes[node];

    switch (n->kind) {
    case GLSL_NODE_COMPOUND: {
        glsl_scope_push(s);
        for (int32_t st = n->a; st != GLSL_NO_NODE; st = s->ast->nodes[st].sibling) {
            if (!check_statement(s, st)) {
                glsl_scope_pop(s);
                return GL_FALSE;
            }
        }
        glsl_scope_pop(s);
        return GL_TRUE;
    }

    case GLSL_NODE_DECL:
        return check_declarator(s, node);

    /* A struct declared inside a function goes into the unit's one struct table, not a
     * scoped one; `check_struct_def` refuses a second struct of the same name in any
     * scope, which keeps that table unambiguous. */
    case GLSL_NODE_STRUCT_DEF:
        return check_struct_def(s, node);

    case GLSL_NODE_EXPR_STMT:
        return check_expression(s, n->a);

    case GLSL_NODE_IF:
        if (!check_condition(s, n->a, "the condition of an if must be a bool"))
            return GL_FALSE;
        if (!check_statement(s, n->b))
            return GL_FALSE;
        return check_statement(s, n->c);

    case GLSL_NODE_WHILE:
        if (!check_condition(s, n->a, "the condition of a while must be a bool"))
            return GL_FALSE;
        s->loop_depth++;
        {
            GLboolean ok = check_statement(s, n->b);
            s->loop_depth--;
            return ok;
        }

    case GLSL_NODE_DO_WHILE:
        s->loop_depth++;
        {
            GLboolean ok = check_statement(s, n->a);
            s->loop_depth--;
            if (!ok)
                return GL_FALSE;
        }
        return check_condition(s, n->b, "the condition of a do-while must be a bool");

    case GLSL_NODE_FOR: {
        /* The init clause declares into the loop's own scope, so `for (int i = 0; ...)`
         * does not leave `i` behind and two such loops do not collide. */
        glsl_scope_push(s);
        GLboolean ok = check_statement(s, n->a);
        if (ok)
            ok = check_condition(s, n->b, "the condition of a for must be a bool");
        if (ok)
            ok = check_expression(s, n->c);
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
                sema_fail(s, "return with no value in a function that returns one",
                          node);
                return GL_FALSE;
            }
            return GL_TRUE;
        }
        if (s->current_return == GLSL_TYPE_VOID) {
            sema_fail(s, "return with a value in a void function", node);
            return GL_FALSE;
        }
        glsl_type_t t = glsl_type_of(s, n->a);
        if (t == GLSL_TYPE_ERROR)
            return GL_FALSE;
        if (!glsl_type_accepts(s, s->current_return, t)) {
            sema_fail(s, "return of a different type from the function's", node);
            return GL_FALSE;
        }
        return check_assign_targets(s, n->a);
    }

    case GLSL_NODE_BREAK:
    case GLSL_NODE_CONTINUE:
        /* Outside a loop these name nothing to break out of; the parser cannot tell. */
        if (s->loop_depth == 0) {
            sema_fail(s, "break or continue outside a loop", node);
            return GL_FALSE;
        }
        return GL_TRUE;

    case GLSL_NODE_DISCARD:
        return GL_TRUE;

    default:
        /* A bare expression node in statement position comes from a hand-built tree
         * (the parser wraps one). Typed rather than ignored. */
        return check_expression(s, node);
    }
}

/* -------------------------------------------------------------------------
 * The translation unit
 * ------------------------------------------------------------------------- */

/* Records a function's signature so calls can be checked against it.
 *
 * Exported because the pixel-shader back end rebuilds a scoped symbol table of its own
 * and has to put the unit's functions back into it, or it cannot type a call. */
GLboolean glsl_declare_function(glsl_sema_t *s, int32_t node) {
    const glsl_node_t *n = &s->ast->nodes[node];
    glsl_type_t ret = node_declared_type(s, n);
    if (ret == GLSL_TYPE_ERROR) {
        sema_fail(s, "function with an unknown return type", node);
        return GL_FALSE;
    }
    /* The signature is built before the symbol: whether this is a new overload or a
     * redefinition depends on the parameters. */
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

    /* Two functions of one name are the same function when their parameters agree.
     * GLSL overloads on parameters, not the return type, so `float f(int)` and
     * `vec2 f(int)` are an error. A prototype followed by its definition is accepted,
     * and the definition takes over the entry so calls reach its body. */
    for (int i = s->count - 1; i >= 0; i--) {
        glsl_symbol_t *prev = &s->symbols[i];
        if (!prev->is_function || !same_name(prev, n->text, n->length))
            continue;
        if (prev->param_count != count)
            continue;
        GLboolean same = GL_TRUE;
        for (int k = 0; k < count; k++) {
            if (prev->params[k] != params[k]) {
                same = GL_FALSE;
                break;
            }
        }
        if (!same)
            continue;
        if (prev->type != ret) {
            sema_fail(s, "two functions differing only in return type", node);
            return GL_FALSE;
        }
        /* The body, if this one has it, is the one a call should reach. */
        if (n->c != GLSL_NO_NODE)
            prev->decl_node = node;
        return GL_TRUE;
    }

    if (!glsl_declare(s, n->text, n->length, ret, GL_TRUE))
        return GL_FALSE;
    glsl_symbol_t *sym = &s->symbols[s->count - 1];
    for (int k = 0; k < count; k++)
        sym->params[k] = params[k];
    sym->param_count = count;
    sym->decl_node = node;
    return GL_TRUE;
}

GLboolean glsl_check_unit(glsl_sema_t *s, int32_t unit) {
    if (!s || !s->ast || unit == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *u = &s->ast->nodes[unit];
    if (u->kind != GLSL_NODE_UNIT) {
        sema_fail(s, "not a translation unit", unit);
        return GL_FALSE;
    }

    /* Three passes over the top level. Structs come first, because a function's
     * signature may name one (`S bump(S v)`). Then every function is declared before
     * any body is checked, so a function may call one defined later in the file. */
    for (int32_t d = u->a; d != GLSL_NO_NODE; d = s->ast->nodes[d].sibling) {
        if (s->ast->nodes[d].kind == GLSL_NODE_STRUCT_DEF) {
            if (!check_struct_def(s, d))
                return GL_FALSE;
        }
    }

    for (int32_t d = u->a; d != GLSL_NO_NODE; d = s->ast->nodes[d].sibling) {
        if (s->ast->nodes[d].kind == GLSL_NODE_FUNCTION) {
            if (!glsl_declare_function(s, d))
                return GL_FALSE;
        }
    }

    for (int32_t d = u->a; d != GLSL_NO_NODE; d = s->ast->nodes[d].sibling) {
        const glsl_node_t *n = &s->ast->nodes[d];
        /* Already recorded by the first pass above. */
        if (n->kind == GLSL_NODE_STRUCT_DEF)
            continue;
        if (n->kind == GLSL_NODE_DECL) {
            if (!check_declarator(s, d))
                return GL_FALSE;
            continue;
        }
        if (n->kind != GLSL_NODE_FUNCTION)
            continue;
        if (n->c == GLSL_NO_NODE)
            continue; /* a prototype has no body to check */

        /* The parameters live in the body's scope. Pushed here rather than inside the
         * compound statement, so a parameter and a top-level local of the same name
         * collide, as GLSL requires. */
        glsl_scope_push(s);
        s->current_return = node_declared_type(s, n);
        for (int32_t p = n->b; p != GLSL_NO_NODE; p = s->ast->nodes[p].sibling) {
            const glsl_node_t *pn = &s->ast->nodes[p];
            if (pn->length == 0u)
                continue; /* unnamed parameter in a definition: nothing to bind */
            if (reserved_name(s, pn->text, pn->length, p)) {
                glsl_scope_pop(s);
                return GL_FALSE;
            }
            glsl_type_t pt = node_declared_type(s, pn);
            /* A parameter may be an array (1.10, 6.1), and it binds as a local array
             * does: the symbol carries the element type and the length. */
            int psize = 0;
            if (pn->array_size != GLSL_NO_NODE &&
                !const_int_eval(s, pn->array_size, &psize)) {
                sema_fail(s, "an array parameter's length has to be a constant", p);
                glsl_scope_pop(s);
                return GL_FALSE;
            }
            if (psize > 0) {
                /* Folded back into the tree, as a declarator's length is: the back
                 * ends read the length straight off this node. */
                s->ast->nodes[pn->array_size].kind = GLSL_NODE_INTCONST;
                s->ast->nodes[pn->array_size].value = (double)psize;
                if (!glsl_declare_array(s, pn->text, pn->length, pt, psize,
                                        pn->qualifier)) {
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
        /* The body's statements are walked here, not through the COMPOUND case, which
         * would push a second scope and let a local shadow a parameter. GLSL puts both
         * in one scope, so `float f(float x){ float x; }` is a redefinition. */
        GLboolean ok = GL_TRUE;
        const int32_t body = n->c;
        for (int32_t st = s->ast->nodes[body].a; st != GLSL_NO_NODE;
             st = s->ast->nodes[st].sibling) {
            if (!check_statement(s, st)) {
                ok = GL_FALSE;
                break;
            }
        }
        glsl_scope_pop(s);
        s->current_return = GLSL_TYPE_VOID;
        if (!ok)
            return GL_FALSE;
    }
    return (GLboolean)(s->error == (const char *)0);
}
