/*
 * oops-gl: the GLSL interpreter, the reference for the programmable pipeline.
 *
 * `glsl_gen.c` compiles a shader to gfx1030 instructions; this walks the same tree on
 * the CPU and computes the same values, so a probe can compare the two. Rules are
 * GLSL 1.10's. A value is up to `EXEC_MAX_VAL_FLOATS` floats and a type; `int` and
 * `bool` are floats too, and a matrix is column-major.
 *
 * A derivative is the expression re-evaluated against the neighbouring interpolated
 * block (one pixel right, one pixel down); `gl_shader_needs_derivatives` says whether
 * a program needs them. Every invocation has a step budget, a call depth and a fixed
 * arena, so a runaway shader is a reported error, not a hang.
 */

#include "glsl_internal.h"
#include "oops/math.h"

/* -------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------- */

#define EXEC_MAX_VARS 96
#define EXEC_ARENA_FLOATS 2048
#define EXEC_MAX_DEPTH 16
/* Statements and expressions one invocation may execute: far above a real shader's,
 * small enough that a runaway costs a frame. */
#define EXEC_STEP_BUDGET 100000

/* -------------------------------------------------------------------------
 * Values and places
 * ------------------------------------------------------------------------- */

/* The width of a value in flight, larger than a `mat4` because a struct is a value
 * too. Variables live in the arena and may be any size; this is a stack cost on every
 * expression. `glsl_sema` refuses a larger struct at compile time. */
#define EXEC_MAX_VAL_FLOATS 32

typedef struct {
    glsl_type_t type;
    /* How many of `v` are live when the type cannot say: a whole array's type is its
     * element's, so the value carries its own width. Zero means "ask the type". */
    int count;
    float v[EXEC_MAX_VAL_FLOATS];
} exec_val_t;

/* An assignable location. `map` exists for a swizzle, where the components a reference
 * covers are not consecutive: `v.zx = ...` writes v[2] then v[0]. */
typedef struct {
    float *addr;
    int comps;
    glsl_type_t type;
    GLboolean swizzled;
    int map[4];
} exec_place_t;

typedef enum {
    VAR_STORE,  /* ordinary storage in the arena */
    VAR_VARY_IN /* a fragment input: read from the interpolated block, never written */
} exec_var_kind_t;

typedef struct {
    const char *name;
    size_t len;
    glsl_type_t type;
    int array; /* elements, or 0 */
    int scope;
    exec_var_kind_t kind;
    float *store; /* VAR_STORE */
    int offset;   /* VAR_VARY_IN: into the interpolated block */
} exec_var_t;

typedef enum {
    FLOW_NORMAL = 0,
    FLOW_BREAK,
    FLOW_CONTINUE,
    FLOW_RETURN,
    FLOW_DISCARD
} exec_flow_t;

typedef struct {
    gl_context_t *ctx;
    gl_program_object_t *prog;
    const glsl_unit_t *unit;
    GLenum stage;

    exec_var_t vars[EXEC_MAX_VARS];
    int var_count;
    int scope;
    float arena[EXEC_ARENA_FLOATS];
    int arena_used;

    exec_flow_t flow;
    exec_val_t ret;
    int depth;
    int steps;
    const char *error;

    /* The fragment stage's three interpolated blocks, and which one reads resolve
     * against. */
    const float *vary[3];
    const float frag_coord_src[3][4];
    int which;
    GLboolean has_neighbours;

    /* What the invocation produced. */
    float out_position[4];
    float out_point_size;
    float out_clip_vertex[4];
    GLboolean wrote_clip_vertex;
    float out_vary[GL_SHADER_VARY_FLOATS];
    float out_colour[4];
    float out_depth;
    GLboolean wrote_depth;
} exec_t;

static exec_val_t eval(exec_t *e, int32_t node);
static GLboolean exec_stmt(exec_t *e, int32_t node);

static void fail(exec_t *e, const char *why) {
    if (!e->error)
        e->error = why;
    e->flow = FLOW_RETURN;
}

static int comps_of(glsl_type_t t) {
    const int n = glsl_type_components(t);
    return (n > 0) ? n : 1;
}

/* The struct a type names in this unit, or NULL. The table was copied out of the
 * semantic pass when the unit compiled - see `struct glsl_unit`. */
static const glsl_struct_t *exec_struct(const exec_t *e, glsl_type_t t) {
    if (!e->unit || !glsl_type_is_struct(t))
        return (const glsl_struct_t *)0;
    const int i = glsl_struct_index(t);
    if (i < 0 || i >= e->unit->struct_count)
        return (const glsl_struct_t *)0;
    return &e->unit->structs[i];
}

/* The size of any value, including a struct, whose size lives in the unit's table. */
static int exec_comps(const exec_t *e, glsl_type_t t) {
    const glsl_struct_t *st = exec_struct(e, t);
    return st ? st->components : comps_of(t);
}

/* The struct a name refers to in this unit, or GLSL_TYPE_ERROR. */
static glsl_type_t exec_struct_by_name(const exec_t *e, const char *name, size_t len) {
    if (!e->unit)
        return GLSL_TYPE_ERROR;
    for (int i = 0; i < e->unit->struct_count; i++) {
        const glsl_struct_t *st = &e->unit->structs[i];
        if (st->name_len != len)
            continue;
        size_t k = 0;
        while (k < len && st->name[k] == name[k])
            k++;
        if (k == len)
            return glsl_struct_type(i);
    }
    return GLSL_TYPE_ERROR;
}

/* The type a declaration or parameter node writes, matching sema's
 * `node_declared_type`: the token, or the struct an identifier token names. */
static glsl_type_t exec_node_type(const exec_t *e, const glsl_node_t *n) {
    if (n->type_tok == GLSL_TOK_IDENTIFIER && n->type_name) {
        return exec_struct_by_name(e, n->type_name, n->type_name_len);
    }
    return glsl_type_from_token(n->type_tok);
}

/* The member of a struct type, or NULL. */
static const glsl_struct_member_t *exec_member(const exec_t *e, glsl_type_t t,
                                               const char *name, size_t len) {
    const glsl_struct_t *st = exec_struct(e, t);
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

static exec_val_t val_zero(glsl_type_t t) {
    exec_val_t v;
    v.type = t;
    v.count = 0; /* "ask the type" - only a whole array overrides this */
    for (int i = 0; i < EXEC_MAX_VAL_FLOATS; i++)
        v.v[i] = 0.0f;
    return v;
}

static exec_val_t val_float(float f) {
    exec_val_t v = val_zero(GLSL_TYPE_FLOAT);
    v.v[0] = f;
    return v;
}

static exec_val_t val_bool(GLboolean b) {
    exec_val_t v = val_zero(GLSL_TYPE_BOOL);
    v.v[0] = b ? 1.0f : 0.0f;
    return v;
}

/* -------------------------------------------------------------------------
 * The environment
 * ------------------------------------------------------------------------- */

static GLboolean name_eq(const char *a, size_t alen, const char *b, size_t blen) {
    if (alen != blen)
        return GL_FALSE;
    for (size_t i = 0; i < alen; i++) {
        if (a[i] != b[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

static size_t lit_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

static exec_var_t *lookup(exec_t *e, const char *name, size_t len) {
    for (int i = e->var_count - 1; i >= 0; i--) {
        if (name_eq(e->vars[i].name, e->vars[i].len, name, len))
            return &e->vars[i];
    }
    return (exec_var_t *)0;
}

/* Declares a name with storage of its own. The arena is a bump allocator and a scope
 * pop winds it back, so a loop body's locals cost nothing per iteration. */
static float *declare(exec_t *e, const char *name, size_t len, glsl_type_t type,
                      int array) {
    if (e->var_count >= EXEC_MAX_VARS) {
        fail(e, "too many variables live in this shader");
        return (float *)0;
    }
    const int need = exec_comps(e, type) * ((array > 0) ? array : 1);
    if (e->arena_used + need > EXEC_ARENA_FLOATS) {
        fail(e, "this shader needs more storage than an invocation has");
        return (float *)0;
    }
    float *store = &e->arena[e->arena_used];
    for (int i = 0; i < need; i++)
        store[i] = 0.0f;
    e->arena_used += need;

    exec_var_t *v = &e->vars[e->var_count++];
    v->name = name;
    v->len = len;
    v->type = type;
    v->array = array;
    v->scope = e->scope;
    v->kind = VAR_STORE;
    v->store = store;
    v->offset = 0;
    return store;
}

static void declare_vary_in(exec_t *e, const char *name, glsl_type_t type, int array,
                            int offset) {
    if (e->var_count >= EXEC_MAX_VARS)
        return;
    exec_var_t *v = &e->vars[e->var_count++];
    v->name = name;
    v->len = lit_len(name);
    v->type = type;
    v->array = array;
    v->scope = e->scope;
    v->kind = VAR_VARY_IN;
    v->store = (float *)0;
    v->offset = offset;
}

static void scope_push(exec_t *e) {
    e->scope++;
}

static void scope_pop(exec_t *e) {
    while (e->var_count > 0 && e->vars[e->var_count - 1].scope >= e->scope) {
        exec_var_t *v = &e->vars[e->var_count - 1];
        /* Only storage came out of the arena. A bump allocator winds back only its
         * most recent allocations, which is what a scope's declarations are. */
        if (v->kind == VAR_STORE && v->store) {
            const int n = comps_of(v->type) * ((v->array > 0) ? v->array : 1);
            if (v->store + n == &e->arena[e->arena_used])
                e->arena_used -= n;
        }
        e->var_count--;
    }
    e->scope--;
}

/* -------------------------------------------------------------------------
 * Small maths the language needs and the platform's header does not have
 * ------------------------------------------------------------------------- */

static float f_abs(float x) {
    return x < 0.0f ? -x : x;
}

static float f_asin(float x) {
    if (x <= -1.0f)
        return -OOPS_HALF_PI;
    if (x >= 1.0f)
        return OOPS_HALF_PI;
    return oops_atan2f(x, oops_sqrtf(1.0f - x * x));
}

static float f_acos(float x) {
    return OOPS_HALF_PI - f_asin(x);
}

/* `log2(x)` and `exp2(x)` through the natural pair, which is what the platform
 * provides. 1.4426950408889634 is 1/ln 2 and 0.6931471805599453 is ln 2. */
static float f_log2(float x) {
    return oops_logf(x) * 1.4426950408889634f;
}
static float f_exp2(float x) {
    return oops_expf(x * 0.6931471805599453f);
}

/* GLSL's `mod` is floored, not C's truncated `fmod`: `mod(-1.0, 3.0)` is 2. */
static float f_mod(float x, float y) {
    if (y == 0.0f)
        return 0.0f;
    return x - y * oops_floorf(x / y);
}

static float f_fract(float x) {
    return x - oops_floorf(x);
}

static float f_sign(float x) {
    return (x > 0.0f) ? 1.0f : ((x < 0.0f) ? -1.0f : 0.0f);
}

static float f_clamp(float x, float lo, float hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

/* -------------------------------------------------------------------------
 * Places: what an assignment writes to
 * ------------------------------------------------------------------------- */

static int swizzle_index(char c) {
    switch (c) {
    case 'x':
    case 'r':
    case 's':
        return 0;
    case 'y':
    case 'g':
    case 't':
        return 1;
    case 'z':
    case 'b':
    case 'p':
        return 2;
    case 'w':
    case 'a':
    case 'q':
        return 3;
    default:
        return -1;
    }
}

static exec_place_t place_none(void) {
    exec_place_t p;
    p.addr = (float *)0;
    p.comps = 0;
    p.type = GLSL_TYPE_ERROR;
    p.swizzled = GL_FALSE;
    for (int i = 0; i < 4; i++)
        p.map[i] = i;
    return p;
}

/* Where a value lives, for reading or for writing.
 *
 * `for_write` separates an error from a miss: a fragment input has no address, which
 * is ordinary when reading and an error only when assigning. */
static exec_place_t place_of_rw(exec_t *e, int32_t node, GLboolean for_write) {
    exec_place_t p = place_none();
    if (node == GLSL_NO_NODE)
        return p;
    const glsl_node_t *n = &e->unit->ast.nodes[node];

    if (n->kind == GLSL_NODE_IDENTIFIER) {
        exec_var_t *v = lookup(e, n->text, n->length);
        if (!v) {
            if (for_write)
                fail(e, "assignment to an unknown name");
            return p;
        }
        if (v->kind != VAR_STORE) {
            if (for_write)
                fail(e, "assignment to something that is read-only");
            return p;
        }
        p.addr = v->store;
        /* `exec_comps`, so a struct's place covers the whole struct, times the length
         * for a whole array so `v = w` copies all of it (1.20). `a[k]` takes its own
         * path below. */
        p.comps = exec_comps(e, v->type) * ((v->array > 0) ? v->array : 1);
        p.type = v->type;
        return p;
    }
    if (n->kind == GLSL_NODE_INDEX) {
        const glsl_node_t *base = &e->unit->ast.nodes[n->a];
        const exec_val_t idx = eval(e, n->b);
        const int i = (int)idx.v[0];
        if (base->kind == GLSL_NODE_IDENTIFIER) {
            exec_var_t *v = lookup(e, base->text, base->length);
            if (v && v->array > 0) {
                if (v->kind != VAR_STORE) {
                    if (for_write)
                        fail(e, "assignment to something that is read-only");
                    return p;
                }
                if (i < 0 || i >= v->array) {
                    fail(e, "array index out of range");
                    return p;
                }
                const int stride = comps_of(v->type);
                p.addr = v->store + (size_t)i * (size_t)stride;
                p.comps = stride;
                p.type = v->type;
                return p;
            }
        }
        exec_place_t bp = place_of_rw(e, n->a, for_write);
        if (!bp.addr)
            return p;
        if (glsl_type_is_matrix(bp.type)) {
            /* A `matCxR` has C columns of R floats: the bound is the column count and
             * the stride is the row count. */
            const int cols = glsl_type_matrix_cols(bp.type);
            const int rows = glsl_type_matrix_rows(bp.type);
            if (i < 0 || i >= cols) {
                fail(e, "matrix column out of range");
                return p;
            }
            p.addr = bp.addr + (size_t)i * (size_t)rows;
            p.comps = rows;
            p.type = glsl_type_vector_of(GLSL_TYPE_FLOAT, rows);
            return p;
        }
        if (i < 0 || i >= bp.comps) {
            fail(e, "vector component out of range");
            return p;
        }
        /* Through a swizzle: `v.yzw[1]` is v.z, so the map has to be followed. */
        p.addr = bp.addr + (bp.swizzled ? bp.map[i] : i);
        p.comps = 1;
        p.type = glsl_type_base(bp.type);
        return p;
    }
    if (n->kind == GLSL_NODE_FIELD) {
        exec_place_t bp = place_of_rw(e, n->a, for_write);
        if (!bp.addr)
            return p;
        /* A struct member is a run, not a map: members lie end to end in declaration
         * order, the layout sema fixed. The four-entry swizzle `map` is for vectors. */
        const glsl_struct_member_t *mem = exec_member(e, bp.type, n->text, n->length);
        if (mem) {
            p.addr = bp.addr + mem->offset;
            p.comps = exec_comps(e, mem->type) *
                      ((mem->array_size > 0) ? mem->array_size : 1);
            p.type = mem->type;
            p.swizzled = GL_FALSE;
            return p;
        }
        if (glsl_type_is_struct(bp.type)) {
            fail(e, "this struct has no member of that name");
            return place_none();
        }
        if (n->length == 0u || n->length > 4u) {
            fail(e, "a swizzle of no use");
            return p;
        }
        p.addr = bp.addr;
        p.comps = (int)n->length;
        p.swizzled = GL_TRUE;
        p.type = glsl_type_vector_of(glsl_type_base(bp.type), (int)n->length);
        for (size_t k = 0; k < n->length; k++) {
            const int c = swizzle_index(n->text[k]);
            if (c < 0 || c >= bp.comps) {
                fail(e, "a swizzle past the end of its operand");
                return place_none();
            }
            p.map[k] = bp.swizzled ? bp.map[c] : c;
        }
        /* A swizzle naming a component twice is not assignable (`v.xx = ...`); reading
         * one is ordinary, so the check is on writes only. */
        if (for_write) {
            for (int a = 0; a < p.comps; a++) {
                for (int b = a + 1; b < p.comps; b++) {
                    if (p.map[a] == p.map[b]) {
                        fail(
                            e,
                            "a swizzle naming a component twice cannot be assigned to");
                        return place_none();
                    }
                }
            }
        }
        return p;
    }
    if (for_write)
        fail(e, "assignment to something that is not a variable");
    return p;
}

/* The assignable form, which is what every write goes through. */
static exec_place_t place_of(exec_t *e, int32_t node) {
    return place_of_rw(e, node, GL_TRUE);
}

static void place_write(exec_t *e, const exec_place_t *p, const exec_val_t *v) {
    if (!p->addr)
        return;
    /* `exec_comps`, because `comps_of` reports 1 for a struct and a width of 1 is the
     * broadcast case below. A whole array carries its own width, since its type is
     * only its element's. */
    const int n = (v->count > 0) ? v->count : exec_comps(e, v->type);
    /* A scalar assigned to a wider place broadcasts, which is only reachable through a
     * constructor here; anything else the semantic stage already refused. */
    for (int i = 0; i < p->comps; i++) {
        const float f =
            (n == 1) ? v->v[0] : ((i < n && i < EXEC_MAX_VAL_FLOATS) ? v->v[i] : 0.0f);
        p->addr[p->swizzled ? p->map[i] : i] = f;
    }
}

static exec_val_t place_read(const exec_place_t *p) {
    exec_val_t v = val_zero(p->type);
    for (int i = 0; i < p->comps && i < EXEC_MAX_VAL_FLOATS; i++) {
        v.v[i] = p->addr[p->swizzled ? p->map[i] : i];
    }
    /* The place knows a whole array's width, which the type cannot state; carried on
     * the value so a later write or comparison covers all of it. */
    v.count = p->comps;
    return v;
}

/* -------------------------------------------------------------------------
 * The built-in functions
 * ------------------------------------------------------------------------- */

static GLboolean is_name(const glsl_node_t *n, const char *lit) {
    return name_eq(n->text, n->length, lit, lit_len(lit));
}

/* A sampler argument: which texture unit it names, and which target it reads.
 *
 * The unit is the value (`glUniform1i`) and the target is the declared type. The type
 * is converted to the GL enumerant the sampler bridge switches on; a raw `glsl_type_t`
 * matches no case. */
static GLenum sampler_type_of(exec_t *e, const exec_val_t *sampler_value, int *unit) {
    (void)e;
    if (unit)
        *unit = (int)sampler_value->v[0];
    return glsl_type_to_gl(sampler_value->type);
}

/* The level of detail for a texture lookup, from how the coordinate changes across the
 * screen.
 *
 * Taken by re-evaluating the coordinate against the neighbouring blocks. Without
 * neighbours (a vertex shader, or a program that needs no derivative) the base level
 * is sampled.
 */
static float lookup_lod(exec_t *e, int32_t coord_node, const exec_val_t *centre,
                        int dims, GLuint unit, GLenum sampler_type) {
    if (!e->has_neighbours || e->which != 0)
        return -1000.0f;
    const gl_context_t *ctx = e->ctx;
    if (!ctx || unit >= (GLuint)OOPS_GL_MAX_TEXTURE_UNITS)
        return -1000.0f;

    /* The image's size, so a coordinate difference becomes a texel difference. Only the
     * sampler knows which object is bound, so ask it for a texel and take the size from
     * the unit's binding the same way it does. */
    float w = 0.0f, h = 0.0f, d = 0.0f;
    {
        const gl_tex_unit_t *tu = &ctx->tex_unit[unit];
        GLuint id = 0u;
        switch (sampler_type) {
        case GL_SAMPLER_1D:
        case GL_SAMPLER_1D_SHADOW:
            id = tu->bound_texture_1d ? tu->bound_texture_1d
                                      : OOPS_GL_DEFAULT_TEXTURE_1D;
            break;
        case GL_SAMPLER_2D:
        case GL_SAMPLER_2D_SHADOW:
            id = tu->bound_texture_2d ? tu->bound_texture_2d
                                      : OOPS_GL_DEFAULT_TEXTURE_2D;
            break;
        case GL_SAMPLER_3D:
            id = tu->bound_texture_3d ? tu->bound_texture_3d
                                      : OOPS_GL_DEFAULT_TEXTURE_3D;
            break;
        case GL_SAMPLER_CUBE:
            id = tu->bound_texture_cube ? tu->bound_texture_cube
                                        : OOPS_GL_DEFAULT_TEXTURE_CUBE;
            break;
        default:
            return -1000.0f;
        }
        const gl_texture_object_t *tex = gl_lookup_texture(ctx, id);
        if (!tex)
            return -1000.0f;
        w = (float)(tex->cube ? tex->cube_hw_dim : tex->width);
        h = (float)(tex->cube ? tex->cube_hw_dim : tex->height);
        d = (float)tex->depth;
        if (w <= 0.0f)
            w = 1.0f;
        if (h <= 0.0f)
            h = 1.0f;
        if (d <= 0.0f)
            d = 1.0f;
    }

    float rho2 = 0.0f;
    for (int k = 1; k <= 2; k++) {
        const int save = e->which;
        e->which = k;
        const exec_val_t nb = eval(e, coord_node);
        e->which = save;
        const float du = (nb.v[0] - centre->v[0]) * w;
        const float dv = (dims >= 2) ? (nb.v[1] - centre->v[1]) * h : 0.0f;
        const float dw = (dims >= 3) ? (nb.v[2] - centre->v[2]) * d : 0.0f;
        const float m = du * du + dv * dv + dw * dw;
        if (m > rho2)
            rho2 = m;
    }
    if (rho2 <= 0.0f)
        return -1000.0f;
    return 0.5f * f_log2(rho2);
}

/* `dFdx(expr)` and `dFdy(expr)`: the expression one pixel over, minus the expression
 * here. */
static exec_val_t derivative(exec_t *e, int32_t arg, int which) {
    const exec_val_t here = eval(e, arg);
    if (!e->has_neighbours || e->which != 0) {
        /* No neighbour to difference against: zero, which the specification permits.
         */
        exec_val_t z = val_zero(here.type);
        return z;
    }
    const int save = e->which;
    e->which = which;
    const exec_val_t there = eval(e, arg);
    e->which = save;
    exec_val_t out = val_zero(here.type);
    for (int i = 0; i < comps_of(here.type); i++)
        out.v[i] = there.v[i] - here.v[i];
    return out;
}

/* One built-in call. `handled` is false when the name is not a built-in, so the caller
 * can look for a function the shader declared. */
static exec_val_t call_builtin(exec_t *e, const glsl_node_t *callee, int32_t first_arg,
                               GLboolean *handled) {
    *handled = GL_TRUE;

    /* The arguments, by node and by value. `dFdx` and the texture lookups need the node
     * so they can re-evaluate it against a neighbour; everything else wants the value.
     */
    int32_t anode[GLSL_MAX_PARAMS];
    exec_val_t a[GLSL_MAX_PARAMS];
    int argc = 0;
    for (int32_t x = first_arg; x != GLSL_NO_NODE && argc < GLSL_MAX_PARAMS;
         x = e->unit->ast.nodes[x].sibling) {
        anode[argc] = x;
        a[argc] = eval(e, x);
        argc++;
    }

/* --- component-wise over genType ---------------------------------------------------
 */
#define GEN1(nm, expr)                                                                 \
    if (is_name(callee, nm)) {                                                         \
        exec_val_t r = val_zero(a[0].type);                                            \
        for (int i = 0; i < comps_of(a[0].type); i++) {                                \
            const float x = a[0].v[i];                                                 \
            r.v[i] = (expr);                                                           \
        }                                                                              \
        return r;                                                                      \
    }
    GEN1("radians", x * OOPS_DEG2RAD)
    GEN1("degrees", x * OOPS_RAD2DEG)
    GEN1("sin", oops_sinf(x))
    GEN1("cos", oops_cosf(x))
    GEN1("tan", oops_tanf(x))
    GEN1("asin", f_asin(x))
    GEN1("acos", f_acos(x))
    GEN1("exp", oops_expf(x))
    GEN1("log", oops_logf(x))
    GEN1("exp2", f_exp2(x))
    GEN1("log2", f_log2(x))
    GEN1("sqrt", oops_sqrtf(x))
    GEN1("inversesqrt", (x > 0.0f) ? (1.0f / oops_sqrtf(x)) : 0.0f)
    GEN1("abs", f_abs(x))
    GEN1("sign", f_sign(x))
    GEN1("floor", oops_floorf(x))
    GEN1("ceil", oops_ceilf(x))
    GEN1("fract", f_fract(x))
#undef GEN1

    /* `atan` is the one name with two arities. */
    if (is_name(callee, "atan")) {
        exec_val_t r = val_zero(a[0].type);
        for (int i = 0; i < comps_of(a[0].type); i++) {
            r.v[i] = (argc == 2) ? oops_atan2f(a[0].v[i], a[1].v[i])
                                 : oops_atan2f(a[0].v[i], 1.0f);
        }
        return r;
    }

/* --- two operands, the second possibly a scalar ------------------------------------
 */
#define GEN2(nm, expr)                                                                 \
    if (is_name(callee, nm)) {                                                         \
        exec_val_t r = val_zero(a[0].type);                                            \
        const int scalar = (comps_of(a[1].type) == 1);                                 \
        for (int i = 0; i < comps_of(a[0].type); i++) {                                \
            const float x = a[0].v[i];                                                 \
            const float y = a[1].v[scalar ? 0 : i];                                    \
            r.v[i] = (expr);                                                           \
        }                                                                              \
        return r;                                                                      \
    }
    GEN2("pow", oops_powf(x, y))
    GEN2("mod", f_mod(x, y))
    GEN2("min", (x < y) ? x : y)
    GEN2("max", (x > y) ? x : y)
#undef GEN2

    if (is_name(callee, "clamp")) {
        exec_val_t r = val_zero(a[0].type);
        const int s1 = (comps_of(a[1].type) == 1), s2 = (comps_of(a[2].type) == 1);
        for (int i = 0; i < comps_of(a[0].type); i++) {
            r.v[i] = f_clamp(a[0].v[i], a[1].v[s1 ? 0 : i], a[2].v[s2 ? 0 : i]);
        }
        return r;
    }
    if (is_name(callee, "mix")) {
        exec_val_t r = val_zero(a[0].type);
        const int s2 = (comps_of(a[2].type) == 1);
        for (int i = 0; i < comps_of(a[0].type); i++) {
            const float t = a[2].v[s2 ? 0 : i];
            r.v[i] = a[0].v[i] * (1.0f - t) + a[1].v[i] * t;
        }
        return r;
    }
    if (is_name(callee, "step")) {
        /* The result is the second argument's shape: `step(0.5, v)` is as wide as v. */
        exec_val_t r = val_zero(a[1].type);
        const int s0 = (comps_of(a[0].type) == 1);
        for (int i = 0; i < comps_of(a[1].type); i++) {
            r.v[i] = (a[1].v[i] < a[0].v[s0 ? 0 : i]) ? 0.0f : 1.0f;
        }
        return r;
    }
    if (is_name(callee, "smoothstep")) {
        exec_val_t r = val_zero(a[2].type);
        const int s = (comps_of(a[0].type) == 1);
        for (int i = 0; i < comps_of(a[2].type); i++) {
            const float e0 = a[0].v[s ? 0 : i], e1 = a[1].v[s ? 0 : i];
            const float den = e1 - e0;
            const float t =
                (den != 0.0f) ? f_clamp((a[2].v[i] - e0) / den, 0.0f, 1.0f) : 0.0f;
            r.v[i] = t * t * (3.0f - 2.0f * t);
        }
        return r;
    }

    /* --- geometric
     * ---------------------------------------------------------------------- */
    if (is_name(callee, "length") || is_name(callee, "distance")) {
        float sum = 0.0f;
        for (int i = 0; i < comps_of(a[0].type); i++) {
            const float d = (argc == 2) ? (a[0].v[i] - a[1].v[i]) : a[0].v[i];
            sum += d * d;
        }
        return val_float(oops_sqrtf(sum));
    }
    if (is_name(callee, "dot")) {
        float sum = 0.0f;
        for (int i = 0; i < comps_of(a[0].type); i++)
            sum += a[0].v[i] * a[1].v[i];
        return val_float(sum);
    }
    if (is_name(callee, "cross")) {
        exec_val_t r = val_zero(GLSL_TYPE_VEC3);
        r.v[0] = a[0].v[1] * a[1].v[2] - a[0].v[2] * a[1].v[1];
        r.v[1] = a[0].v[2] * a[1].v[0] - a[0].v[0] * a[1].v[2];
        r.v[2] = a[0].v[0] * a[1].v[1] - a[0].v[1] * a[1].v[0];
        return r;
    }
    if (is_name(callee, "normalize")) {
        float sum = 0.0f;
        const int n = comps_of(a[0].type);
        for (int i = 0; i < n; i++)
            sum += a[0].v[i] * a[0].v[i];
        const float len = oops_sqrtf(sum);
        exec_val_t r = val_zero(a[0].type);
        /* A zero vector normalises to zero. The specification leaves it undefined;
         * zero does not propagate infinities downstream. */
        if (len > 0.0f) {
            for (int i = 0; i < n; i++)
                r.v[i] = a[0].v[i] / len;
        }
        return r;
    }
    if (is_name(callee, "faceforward")) {
        float d = 0.0f;
        const int n = comps_of(a[0].type);
        for (int i = 0; i < n; i++)
            d += a[2].v[i] * a[1].v[i];
        exec_val_t r = val_zero(a[0].type);
        for (int i = 0; i < n; i++)
            r.v[i] = (d < 0.0f) ? a[0].v[i] : -a[0].v[i];
        return r;
    }
    if (is_name(callee, "reflect")) {
        /* I - 2 * dot(N, I) * N. */
        float d = 0.0f;
        const int n = comps_of(a[0].type);
        for (int i = 0; i < n; i++)
            d += a[1].v[i] * a[0].v[i];
        exec_val_t r = val_zero(a[0].type);
        for (int i = 0; i < n; i++)
            r.v[i] = a[0].v[i] - 2.0f * d * a[1].v[i];
        return r;
    }
    if (is_name(callee, "refract")) {
        const int n = comps_of(a[0].type);
        const float eta = a[2].v[0];
        float d = 0.0f;
        for (int i = 0; i < n; i++)
            d += a[1].v[i] * a[0].v[i];
        const float k = 1.0f - eta * eta * (1.0f - d * d);
        exec_val_t r = val_zero(a[0].type);
        /* Total internal reflection returns the zero vector, as the specification
         * defines it. */
        if (k >= 0.0f) {
            const float s = eta * d + oops_sqrtf(k);
            for (int i = 0; i < n; i++)
                r.v[i] = eta * a[0].v[i] - s * a[1].v[i];
        }
        return r;
    }
    if (is_name(callee, "matrixCompMult")) {
        exec_val_t r = val_zero(a[0].type);
        for (int i = 0; i < comps_of(a[0].type); i++)
            r.v[i] = a[0].v[i] * a[1].v[i];
        return r;
    }
    if (is_name(callee, "transpose")) {
        /* Element (col, row) is `v[col * rows + row]`. `matCxR` transposes to
         * `matRxC`, so the result's stride is the source's column count. */
        const int cols = glsl_type_matrix_cols(a[0].type);
        const int rows = glsl_type_matrix_rows(a[0].type);
        exec_val_t r = val_zero(glsl_type_matrix_of(rows, cols));
        for (int c = 0; c < rows; c++) {
            for (int row = 0; row < cols; row++)
                r.v[c * cols + row] = a[0].v[row * rows + c];
        }
        return r;
    }
    if (is_name(callee, "outerProduct")) {
        /* `outerProduct(c, r)` has `c` down the columns and `r` across them: element
         * (col, row) is `c[row] * r[col]`. `outerProduct(vecR, vecC)` is a `matCxR`. */
        const int rows = comps_of(a[0].type), cols = comps_of(a[1].type);
        exec_val_t r = val_zero(glsl_type_matrix_of(cols, rows));
        for (int c = 0; c < cols; c++) {
            for (int row = 0; row < rows; row++)
                r.v[c * rows + row] = a[0].v[row] * a[1].v[c];
        }
        return r;
    }

    /* --- vector relational
     * -------------------------------------------------------------- */
    {
        int op = -1;
        if (is_name(callee, "lessThan"))
            op = 0;
        else if (is_name(callee, "lessThanEqual"))
            op = 1;
        else if (is_name(callee, "greaterThan"))
            op = 2;
        else if (is_name(callee, "greaterThanEqual"))
            op = 3;
        else if (is_name(callee, "equal"))
            op = 4;
        else if (is_name(callee, "notEqual"))
            op = 5;
        if (op >= 0) {
            const int n = comps_of(a[0].type);
            exec_val_t r = val_zero(glsl_type_vector_of(GLSL_TYPE_BOOL, n));
            for (int i = 0; i < n; i++) {
                const float x = a[0].v[i], y = a[1].v[i];
                GLboolean b = GL_FALSE;
                switch (op) {
                case 0:
                    b = (GLboolean)(x < y);
                    break;
                case 1:
                    b = (GLboolean)(x <= y);
                    break;
                case 2:
                    b = (GLboolean)(x > y);
                    break;
                case 3:
                    b = (GLboolean)(x >= y);
                    break;
                case 4:
                    b = (GLboolean)(x == y);
                    break;
                default:
                    b = (GLboolean)(x != y);
                    break;
                }
                r.v[i] = b ? 1.0f : 0.0f;
            }
            return r;
        }
    }
    if (is_name(callee, "any") || is_name(callee, "all")) {
        const GLboolean want_all = is_name(callee, "all");
        GLboolean acc = want_all ? GL_TRUE : GL_FALSE;
        for (int i = 0; i < comps_of(a[0].type); i++) {
            const GLboolean b = (GLboolean)(a[0].v[i] != 0.0f);
            if (want_all) {
                if (!b)
                    acc = GL_FALSE;
            } else if (b)
                acc = GL_TRUE;
        }
        return val_bool(acc);
    }
    if (is_name(callee, "not")) {
        exec_val_t r = val_zero(a[0].type);
        for (int i = 0; i < comps_of(a[0].type); i++)
            r.v[i] = (a[0].v[i] != 0.0f) ? 0.0f : 1.0f;
        return r;
    }

    /* --- derivatives
     * -------------------------------------------------------------------- */
    if (is_name(callee, "dFdx"))
        return derivative(e, anode[0], 1);
    if (is_name(callee, "dFdy"))
        return derivative(e, anode[0], 2);
    if (is_name(callee, "fwidth")) {
        const exec_val_t dx = derivative(e, anode[0], 1);
        const exec_val_t dy = derivative(e, anode[0], 2);
        exec_val_t r = val_zero(dx.type);
        for (int i = 0; i < comps_of(dx.type); i++)
            r.v[i] = f_abs(dx.v[i]) + f_abs(dy.v[i]);
        return r;
    }

    /* --- noise
     * -------------------------------------------------------------------------- */
    /* Noise is zero: GLSL 1.10 section 8.9 permits it, Mesa answers zero
     * (`builtin_functions.cpp:8237`), and GLSL 4.4 specifies it. The argument is
     * evaluated anyway, because it may assign. */
    if (is_name(callee, "noise1") || is_name(callee, "noise2") ||
        is_name(callee, "noise3") || is_name(callee, "noise4")) {
        if (first_arg != GLSL_NO_NODE)
            (void)eval(e, first_arg);
        const int w = callee->text[callee->length - 1u] - '0';
        return val_zero(glsl_type_vector_of(GLSL_TYPE_FLOAT, w));
    }

    /* --- ftransform
     * --------------------------------------------------------------------- */
    if (is_name(callee, "ftransform")) {
        /* The fixed-function transform of `gl_Vertex`, so depth matches a
         * fixed-function pass exactly. */
        exec_var_t *gv = lookup(e, "gl_Vertex", 9u);
        exec_val_t r = val_zero(GLSL_TYPE_VEC4);
        if (gv && gv->store && e->ctx) {
            gl_update_mvp(e->ctx);
            const float in[4] = {gv->store[0], gv->store[1], gv->store[2],
                                 gv->store[3]};
            mat4_transform_vec4(r.v, &e->ctx->mvp, in);
        }
        return r;
    }

    /* --- texture lookups
     * ---------------------------------------------------------------- */
    {
        int dims = 0;
        GLboolean proj = GL_FALSE, explicit_lod = GL_FALSE;
        GLboolean is_tex = GL_TRUE;
        if (is_name(callee, "texture1D"))
            dims = 1;
        else if (is_name(callee, "texture2D"))
            dims = 2;
        else if (is_name(callee, "texture3D"))
            dims = 3;
        else if (is_name(callee, "textureCube"))
            dims = 3;
        else if (is_name(callee, "texture1DProj")) {
            dims = 1;
            proj = GL_TRUE;
        } else if (is_name(callee, "texture2DProj")) {
            dims = 2;
            proj = GL_TRUE;
        } else if (is_name(callee, "texture3DProj")) {
            dims = 3;
            proj = GL_TRUE;
        } else if (is_name(callee, "shadow1D"))
            dims = 1;
        else if (is_name(callee, "shadow2D"))
            dims = 2;
        else if (is_name(callee, "shadow1DProj")) {
            dims = 1;
            proj = GL_TRUE;
        } else if (is_name(callee, "shadow2DProj")) {
            dims = 2;
            proj = GL_TRUE;
        } else if (is_name(callee, "texture1DLod")) {
            dims = 1;
            explicit_lod = GL_TRUE;
        } else if (is_name(callee, "texture2DLod")) {
            dims = 2;
            explicit_lod = GL_TRUE;
        } else if (is_name(callee, "texture3DLod")) {
            dims = 3;
            explicit_lod = GL_TRUE;
        } else if (is_name(callee, "textureCubeLod")) {
            dims = 3;
            explicit_lod = GL_TRUE;
        } else if (is_name(callee, "texture2DProjLod")) {
            dims = 2;
            proj = GL_TRUE;
            explicit_lod = GL_TRUE;
        } else if (is_name(callee, "shadow2DLod")) {
            dims = 2;
            explicit_lod = GL_TRUE;
        } else if (is_name(callee, "shadow2DProjLod")) {
            dims = 2;
            proj = GL_TRUE;
            explicit_lod = GL_TRUE;
        } else
            is_tex = GL_FALSE;

        if (is_tex) {
            int unit = 0;
            const GLenum stype = sampler_type_of(e, &a[0], &unit);
            const GLboolean shadow = (GLboolean)(stype == GL_SAMPLER_1D_SHADOW ||
                                                 stype == GL_SAMPLER_2D_SHADOW);
            /* A projective lookup divides by the last component passed:
             * `texture2DProj(s, vec4)` divides by w and ignores z, `texture2DProj(s,
             * vec3)` divides by z. */
            float coord[4] = {a[1].v[0], a[1].v[1], a[1].v[2], a[1].v[3]};
            if (proj) {
                const int w_index = comps_of(a[1].type) - 1;
                const float q = a[1].v[w_index];
                const float inv = (q != 0.0f) ? (1.0f / q) : 0.0f;
                for (int i = 0; i < w_index; i++)
                    coord[i] *= inv;
                for (int i = w_index; i < 4; i++)
                    coord[i] = 0.0f;
                /* A shadow lookup's reference is the third component, which the divide
                 * has just moved: `shadow2DProj` compares against r/q. */
                if (shadow && w_index >= 3)
                    coord[2] = a[1].v[2] * inv;
            } else if (shadow && dims == 2) {
                /* `shadow2D(sampler, vec3)` is (s, t) with r as the reference, which is
                 * already the layout `gl_shader_sample` expects. */
                coord[2] = a[1].v[2];
            }

            float lod;
            if (explicit_lod) {
                lod = (argc >= 3) ? a[2].v[0] : 0.0f;
            } else {
                lod = lookup_lod(e, anode[1], &a[1], dims, (GLuint)unit, stype);
                /* GLSL's optional third argument is a bias on the computed level, not
                 * the level itself. */
                if (argc >= 3 && lod > -999.0f)
                    lod += a[2].v[0];
            }
            exec_val_t r = val_zero(GLSL_TYPE_VEC4);
            (void)gl_shader_sample(e->ctx, (GLuint)unit, stype, coord, lod, r.v);
            return r;
        }
    }

    *handled = GL_FALSE;
    return val_zero(GLSL_TYPE_ERROR);
}

/* -------------------------------------------------------------------------
 * Calling a function the shader declared
 * ------------------------------------------------------------------------- */

static int32_t find_function(exec_t *e, const char *name, size_t len) {
    const glsl_ast_t *ast = &e->unit->ast;
    const int32_t root = e->unit->root;
    for (int32_t d = ast->nodes[root].a; d != GLSL_NO_NODE; d = ast->nodes[d].sibling) {
        const glsl_node_t *n = &ast->nodes[d];
        if (n->kind != GLSL_NODE_FUNCTION || n->c == GLSL_NO_NODE)
            continue;
        if (name_eq(n->text, n->length, name, len))
            return d;
    }
    return GLSL_NO_NODE;
}

static exec_val_t call_user(exec_t *e, int32_t fn, int32_t first_arg) {
    const glsl_ast_t *ast = &e->unit->ast;
    const glsl_node_t *f = &ast->nodes[fn];

    if (++e->depth > EXEC_MAX_DEPTH) {
        fail(e, "shader functions nested too deeply, or recursing");
        e->depth--;
        return val_zero(GLSL_TYPE_VOID);
    }

    /* Arguments are evaluated in the caller's scope before the parameters exist, so an
     * argument naming the same identifier as a parameter sees the caller's. */
    exec_val_t argv[GLSL_MAX_PARAMS];
    exec_place_t argp[GLSL_MAX_PARAMS];
    int32_t argn[GLSL_MAX_PARAMS];
    int argc = 0;
    for (int32_t x = first_arg; x != GLSL_NO_NODE && argc < GLSL_MAX_PARAMS;
         x = ast->nodes[x].sibling) {
        argn[argc] = x;
        argp[argc] = place_none();
        argv[argc] = eval(e, x);
        argc++;
    }

    scope_push(e);
    /* An `out` parameter starts undefined and an `inout` starts as the argument; both
     * are copied back on return. GLSL never passes by reference. */
    int pi = 0;
    for (int32_t p = f->b; p != GLSL_NO_NODE && pi < argc;
         p = ast->nodes[p].sibling, pi++) {
        const glsl_node_t *pn = &ast->nodes[p];
        if (pn->length == 0u)
            continue;
        /* `exec_node_type`, because a struct parameter names its type with an
         * identifier, for which `glsl_type_from_token` answers ERROR. */
        const glsl_type_t pt = exec_node_type(e, pn);
        /* An array parameter is a run, not a value (1.10, 6.1). Sema folded its length
         * into the tree. It is copied store to store, since an `exec_val_t` may be too
         * narrow, and a copy leaves the caller's array alone. */
        int psize = 0;
        if (pn->array_size != GLSL_NO_NODE) {
            const glsl_node_t *sz = &ast->nodes[pn->array_size];
            if (sz->kind == GLSL_NODE_INTCONST)
                psize = (int)sz->value;
        }
        float *store = declare(e, pn->text, pn->length, pt, psize);
        if (!store)
            break;
        if (psize > 0) {
            const glsl_node_t *an = &ast->nodes[argn[pi]];
            const exec_var_t *src = (an->kind == GLSL_NODE_IDENTIFIER)
                                        ? lookup(e, an->text, an->length)
                                        : (const exec_var_t *)0;
            if (!src || !src->store) {
                fail(e, "an array argument has to be an array's name");
                break;
            }
            const int total = exec_comps(e, pt) * psize;
            for (int i = 0; i < total; i++)
                store[i] = src->store[i];
            continue;
        }
        if (pn->qualifier != GLSL_TOK_KW_OUT) {
            const int w = exec_comps(e, pt);
            for (int i = 0; i < w && i < EXEC_MAX_VAL_FLOATS; i++)
                store[i] = argv[pi].v[i];
        }
        if (pn->qualifier == GLSL_TOK_KW_OUT || pn->qualifier == GLSL_TOK_KW_INOUT) {
            argp[pi] = place_of(e, argn[pi]);
        }
    }

    const exec_flow_t saved_flow = e->flow;
    e->flow = FLOW_NORMAL;
    e->ret = val_zero(exec_node_type(e, f));
    for (int32_t st = ast->nodes[f->c].a; st != GLSL_NO_NODE;
         st = ast->nodes[st].sibling) {
        if (!exec_stmt(e, st))
            break;
        if (e->flow != FLOW_NORMAL)
            break;
    }
    const exec_val_t result = e->ret;
    const GLboolean discarded = (GLboolean)(e->flow == FLOW_DISCARD);

    /* Copy `out` and `inout` parameters back before the scope goes. */
    pi = 0;
    for (int32_t p = f->b; p != GLSL_NO_NODE && pi < argc;
         p = ast->nodes[p].sibling, pi++) {
        const glsl_node_t *pn = &ast->nodes[p];
        if (pn->length == 0u)
            continue;
        if (pn->qualifier != GLSL_TOK_KW_OUT && pn->qualifier != GLSL_TOK_KW_INOUT)
            continue;
        exec_var_t *v = lookup(e, pn->text, pn->length);
        if (!v || !v->store || !argp[pi].addr)
            continue;
        exec_val_t back = val_zero(v->type);
        const int bw = exec_comps(e, v->type);
        for (int i = 0; i < bw && i < EXEC_MAX_VAL_FLOATS; i++)
            back.v[i] = v->store[i];
        place_write(e, &argp[pi], &back);
    }
    scope_pop(e);
    e->depth--;
    /* A `discard` unwinds all the way out; anything else returns to the caller's flow.
     */
    e->flow = discarded ? FLOW_DISCARD : saved_flow;
    return result;
}

/* -------------------------------------------------------------------------
 * Expressions
 * ------------------------------------------------------------------------- */

static exec_val_t construct(exec_t *e, glsl_type_t target, int32_t first_arg) {
    exec_val_t r = val_zero(target);
    const int want = comps_of(target);
    const int mcols = glsl_type_matrix_cols(target),
              mrows = glsl_type_matrix_rows(target);

    /* Gather every argument's components into one run, which is what a GLSL constructor
     * does: `vec4(v.xy, 1.0, 0.0)` is four values from three arguments. */
    float flat[32];
    int n = 0;
    int args = 0;
    for (int32_t x = first_arg; x != GLSL_NO_NODE; x = e->unit->ast.nodes[x].sibling) {
        const exec_val_t v = eval(e, x);
        args++;
        for (int i = 0; i < comps_of(v.type) && n < 32; i++)
            flat[n++] = v.v[i];
    }

    if (n == 0)
        return r;
    /* One scalar fills: a vector broadcasts it, a matrix puts it on the diagonal
     * (`mat4(1.0)` is the identity). A non-square diagonal stops at the shorter side.
     */
    if (args == 1 && n == 1) {
        if (mcols > 0) {
            const int diag = (mcols < mrows) ? mcols : mrows;
            for (int i = 0; i < diag; i++)
                r.v[i * mrows + i] = flat[0];
        } else {
            for (int i = 0; i < want; i++)
                r.v[i] = flat[0];
        }
        return r;
    }
    for (int i = 0; i < want; i++)
        r.v[i] = (i < n) ? flat[i] : 0.0f;
    /* An integer or boolean constructor converts, rather than reinterpreting:
     * `int(1.7)` is 1, truncated towards zero, and `bool(0.0)` is false. */
    const glsl_type_t base = glsl_type_base(target);
    if (base == GLSL_TYPE_INT) {
        for (int i = 0; i < want; i++)
            r.v[i] = (float)(int)r.v[i];
    } else if (base == GLSL_TYPE_BOOL) {
        for (int i = 0; i < want; i++)
            r.v[i] = (r.v[i] != 0.0f) ? 1.0f : 0.0f;
    }
    return r;
}

/* `a op b` for the arithmetic operators, with GLSL's rules for what a matrix and a
 * scalar mean. The semantic stage has already refused anything whose shapes do not
 * meet. */
static exec_val_t arith(exec_t *e, glsl_token_type_t op, const exec_val_t *l,
                        const exec_val_t *rr, glsl_type_t result) {
    exec_val_t out = val_zero(result);
    const int lc = comps_of(l->type), rc = comps_of(rr->type);

    if (op == GLSL_TOK_STAR) {
        /* `mat * vec`, `vec * mat` and `mat * mat` are linear algebra; only `mat *
         * scalar` and the component-wise cases fall through. Element (col, row) is at
         * `col * rows + row`, with the rows of the matrix it belongs to. */
        const int lcols = glsl_type_matrix_cols(l->type);
        const int lrows = glsl_type_matrix_rows(l->type);
        const int rcols = glsl_type_matrix_cols(rr->type);
        const int rrows = glsl_type_matrix_rows(rr->type);
        if (lcols > 0 && rcols > 0) {
            /* `matCxR * matPxC -> matPxR`: the sum runs over the left's columns, which
             * are the right's rows. */
            for (int c = 0; c < rcols; c++) {
                for (int row = 0; row < lrows; row++) {
                    float s = 0.0f;
                    for (int k = 0; k < lcols; k++)
                        s += l->v[k * lrows + row] * rr->v[c * rrows + k];
                    out.v[c * lrows + row] = s;
                }
            }
            return out;
        }
        if (lcols > 0 && rc == lcols) {
            /* `matCxR * vecC -> vecR`. */
            for (int row = 0; row < lrows; row++) {
                float s = 0.0f;
                for (int k = 0; k < lcols; k++)
                    s += l->v[k * lrows + row] * rr->v[k];
                out.v[row] = s;
            }
            return out;
        }
        if (rcols > 0 && lc == rrows) {
            /* A row vector times the matrix, the transpose's product: `vecR * matCxR
             * -> vecC`. */
            for (int c = 0; c < rcols; c++) {
                float s = 0.0f;
                for (int k = 0; k < rrows; k++)
                    s += l->v[k] * rr->v[c * rrows + k];
                out.v[c] = s;
            }
            return out;
        }
    }

    const int n = comps_of(result);
    for (int i = 0; i < n; i++) {
        const float x = l->v[(lc == 1) ? 0 : i];
        const float y = rr->v[(rc == 1) ? 0 : i];
        switch (op) {
        case GLSL_TOK_PLUS:
            out.v[i] = x + y;
            break;
        case GLSL_TOK_MINUS:
            out.v[i] = x - y;
            break;
        case GLSL_TOK_STAR:
            out.v[i] = x * y;
            break;
        case GLSL_TOK_SLASH:
            out.v[i] = (y != 0.0f) ? x / y : 0.0f;
            break;
        default:
            fail(e, "an operator this shader stage cannot run");
            return out;
        }
    }
    /* Integer arithmetic truncates: `7 / 2` is 3 in GLSL as it is in C. */
    if (glsl_type_base(result) == GLSL_TYPE_INT) {
        for (int i = 0; i < n; i++)
            out.v[i] = (float)(int)out.v[i];
    }
    return out;
}

/* `exec_comps`, because `comps_of` answers 1 for a struct, and GLSL 1.10 section 5.9
 * gives `==` to structs. */
static GLboolean vals_equal(const exec_t *e, const exec_val_t *a, const exec_val_t *b) {
    /* A whole array carries its own width, because its type is only its element's. */
    const int n = (a->count > 0) ? a->count : exec_comps(e, a->type);
    for (int i = 0; i < n; i++) {
        if (a->v[i] != b->v[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

static exec_val_t eval(exec_t *e, int32_t node) {
    if (node == GLSL_NO_NODE || e->error)
        return val_zero(GLSL_TYPE_ERROR);
    if (--e->steps <= 0) {
        fail(e, "this shader ran past the step budget for one invocation");
        return val_zero(GLSL_TYPE_ERROR);
    }
    const glsl_ast_t *ast = &e->unit->ast;
    const glsl_node_t *n = &ast->nodes[node];

    switch (n->kind) {
    case GLSL_NODE_INTCONST: {
        exec_val_t v = val_zero(GLSL_TYPE_INT);
        v.v[0] = (float)(int)n->value;
        return v;
    }
    case GLSL_NODE_FLOATCONST:
        return val_float((float)n->value);
    case GLSL_NODE_BOOLCONST:
        return val_bool((GLboolean)(n->value != 0.0));

    case GLSL_NODE_IDENTIFIER: {
        exec_var_t *v = lookup(e, n->text, n->length);
        if (!v) {
            /* A built-in constant is a number (7.4), the same one `glGetIntegerv`
             * answers. Asked after the lookup, so a shader may shadow one. */
            int bi = 0;
            if (glsl_builtin_const_int(n->text, n->length, &bi)) {
                exec_val_t c = val_zero(GLSL_TYPE_INT);
                c.v[0] = (float)bi;
                return c;
            }
            fail(e, "a name with no value");
            return val_zero(GLSL_TYPE_ERROR);
        }
        exec_val_t out = val_zero(v->type);
        if (v->kind == VAR_VARY_IN) {
            const float *block = e->vary[e->which] ? e->vary[e->which] : e->vary[0];
            if (block) {
                for (int i = 0; i < comps_of(v->type); i++)
                    out.v[i] = block[v->offset + i];
            }
            return out;
        }
        if (v->store) {
            /* `exec_comps`, so a struct is read whole, times the length for a whole
             * array, which GLSL 1.20's `=` and `==` need. An array value carries its
             * own `count` because its type is its element's. */
            const int n_comp = exec_comps(e, v->type) * ((v->array > 0) ? v->array : 1);
            if (n_comp > EXEC_MAX_VAL_FLOATS) {
                fail(e, "this array is wider than a value this interpreter carries");
                return val_zero(GLSL_TYPE_ERROR);
            }
            for (int i = 0; i < n_comp; i++)
                out.v[i] = v->store[i];
            if (v->array > 0)
                out.count = n_comp;
        }
        return out;
    }

    case GLSL_NODE_FIELD:
    case GLSL_NODE_INDEX: {
        /* An array-typed fragment input such as `gl_TexCoord[0]` has no address, so
         * the element is read straight out of the interpolated block. */
        if (n->kind == GLSL_NODE_INDEX) {
            const glsl_node_t *base = &ast->nodes[n->a];
            if (base->kind == GLSL_NODE_IDENTIFIER) {
                exec_var_t *v = lookup(e, base->text, base->length);
                if (v && v->kind == VAR_VARY_IN && v->array > 0) {
                    const exec_val_t idx = eval(e, n->b);
                    const int i = (int)idx.v[0];
                    if (i < 0 || i >= v->array) {
                        fail(e, "array index out of range");
                        return val_zero(GLSL_TYPE_ERROR);
                    }
                    const float *block =
                        e->vary[e->which] ? e->vary[e->which] : e->vary[0];
                    exec_val_t out = val_zero(v->type);
                    const int w = comps_of(v->type);
                    if (block) {
                        for (int k = 0; k < w; k++)
                            out.v[k] = block[v->offset + i * w + k];
                    }
                    return out;
                }
            }
        }
        /* Reading through a place, so reads and writes agree on which component a
         * swizzle or index means. */
        const exec_place_t p = place_of_rw(e, node, GL_FALSE);
        if (p.addr)
            return place_read(&p);
        if (e->error)
            return val_zero(GLSL_TYPE_ERROR);
        /* Not addressable - a fragment input, `f().xyz`, or a swizzle of a temporary.
         * Evaluate the operand and select out of the value. */
        const exec_val_t base = eval(e, n->a);
        if (n->kind == GLSL_NODE_FIELD) {
            exec_val_t out = val_zero(
                glsl_type_vector_of(glsl_type_base(base.type), (int)n->length));
            for (size_t k = 0; k < n->length && k < 4u; k++) {
                const int c = swizzle_index(n->text[k]);
                if (c < 0 || c >= comps_of(base.type)) {
                    fail(e, "a swizzle past the end of its operand");
                    return val_zero(GLSL_TYPE_ERROR);
                }
                out.v[k] = base.v[c];
            }
            return out;
        }
        {
            const exec_val_t idx = eval(e, n->b);
            const int i = (int)idx.v[0];
            const int mcols = glsl_type_matrix_cols(base.type);
            const int mrows = glsl_type_matrix_rows(base.type);
            if (mcols > 0) {
                if (i < 0 || i >= mcols) {
                    fail(e, "matrix column out of range");
                    return val_zero(GLSL_TYPE_ERROR);
                }
                exec_val_t out = val_zero(glsl_type_vector_of(GLSL_TYPE_FLOAT, mrows));
                for (int k = 0; k < mrows; k++)
                    out.v[k] = base.v[i * mrows + k];
                return out;
            }
            if (i < 0 || i >= comps_of(base.type)) {
                fail(e, "vector component out of range");
                return val_zero(GLSL_TYPE_ERROR);
            }
            exec_val_t out = val_zero(glsl_type_base(base.type));
            out.v[0] = base.v[i];
            return out;
        }
    }

    case GLSL_NODE_UNARY: {
        const exec_val_t a = eval(e, n->a);
        if (n->op == GLSL_TOK_BANG)
            return val_bool((GLboolean)(a.v[0] == 0.0f));
        exec_val_t out = a;
        if (n->op == GLSL_TOK_MINUS) {
            for (int i = 0; i < comps_of(a.type); i++)
                out.v[i] = -a.v[i];
            return out;
        }
        if (n->op == GLSL_TOK_INC || n->op == GLSL_TOK_DEC) {
            const exec_place_t p = place_of(e, n->a);
            if (!p.addr)
                return a;
            exec_val_t cur = place_read(&p);
            for (int i = 0; i < p.comps; i++) {
                cur.v[i] += (n->op == GLSL_TOK_INC) ? 1.0f : -1.0f;
            }
            place_write(e, &p, &cur);
            return cur; /* prefix: the new value */
        }
        return out; /* unary plus */
    }

    case GLSL_NODE_POSTFIX: {
        const exec_place_t p = place_of(e, n->a);
        if (!p.addr)
            return val_zero(GLSL_TYPE_ERROR);
        const exec_val_t before = place_read(&p);
        exec_val_t after = before;
        for (int i = 0; i < p.comps; i++) {
            after.v[i] += (n->op == GLSL_TOK_INC) ? 1.0f : -1.0f;
        }
        place_write(e, &p, &after);
        return before; /* postfix: the value it had */
    }

    case GLSL_NODE_BINARY: {
        /* `&&` and `||` short-circuit (1.10, 5.9). */
        if (n->op == GLSL_TOK_AND_AND) {
            const exec_val_t l = eval(e, n->a);
            if (l.v[0] == 0.0f)
                return val_bool(GL_FALSE);
            const exec_val_t r = eval(e, n->b);
            return val_bool((GLboolean)(r.v[0] != 0.0f));
        }
        if (n->op == GLSL_TOK_OR_OR) {
            const exec_val_t l = eval(e, n->a);
            if (l.v[0] != 0.0f)
                return val_bool(GL_TRUE);
            const exec_val_t r = eval(e, n->b);
            return val_bool((GLboolean)(r.v[0] != 0.0f));
        }
        const exec_val_t l = eval(e, n->a);
        const exec_val_t r = eval(e, n->b);
        switch (n->op) {
        case GLSL_TOK_XOR_XOR:
            return val_bool((GLboolean)((l.v[0] != 0.0f) != (r.v[0] != 0.0f)));
        case GLSL_TOK_EQ:
            return val_bool(vals_equal(e, &l, &r));
        case GLSL_TOK_NE:
            return val_bool((GLboolean)!vals_equal(e, &l, &r));
        case GLSL_TOK_LT:
            return val_bool((GLboolean)(l.v[0] < r.v[0]));
        case GLSL_TOK_GT:
            return val_bool((GLboolean)(l.v[0] > r.v[0]));
        case GLSL_TOK_LE:
            return val_bool((GLboolean)(l.v[0] <= r.v[0]));
        case GLSL_TOK_GE:
            return val_bool((GLboolean)(l.v[0] >= r.v[0]));
        default:
            break;
        }
        /* The result's shape, recomputed from the values so a unit that never went
         * through sema still runs: `matCxR * vecC` is a `vecR`, `vecR * matCxR` a
         * `vecC`, and a matrix product takes columns from the right, rows from the
         * left.
         */
        glsl_type_t rt = l.type;
        const int lcols = glsl_type_matrix_cols(l.type);
        const int lrows = glsl_type_matrix_rows(l.type);
        const int rcols = glsl_type_matrix_cols(r.type);
        const int rrows = glsl_type_matrix_rows(r.type);
        if (n->op == GLSL_TOK_STAR && lcols > 0 && rcols > 0 && rrows == lcols) {
            rt = glsl_type_matrix_of(rcols, lrows);
        } else if (n->op == GLSL_TOK_STAR && lcols > 0 && rcols == 0 &&
                   comps_of(r.type) == lcols) {
            rt = glsl_type_vector_of(GLSL_TYPE_FLOAT, lrows);
        } else if (n->op == GLSL_TOK_STAR && rcols > 0 && lcols == 0 &&
                   comps_of(l.type) == rrows) {
            rt = glsl_type_vector_of(GLSL_TYPE_FLOAT, rcols);
        } else if (comps_of(l.type) == 1 && comps_of(r.type) > 1) {
            rt = r.type;
        }
        /* A float operand makes an integer result a float (GLSL 1.20's implicit
         * conversion), so `2 * 0.5` is not truncated. Sema refuses the mixture in 1.10.
         */
        if (glsl_type_base(rt) == GLSL_TYPE_INT &&
            (glsl_type_base(l.type) == GLSL_TYPE_FLOAT ||
             glsl_type_base(r.type) == GLSL_TYPE_FLOAT)) {
            rt = glsl_type_vector_of(GLSL_TYPE_FLOAT, comps_of(rt));
        }
        return arith(e, n->op, &l, &r, rt);
    }

    case GLSL_NODE_ASSIGN: {
        const exec_place_t p = place_of(e, n->a);
        if (!p.addr)
            return val_zero(GLSL_TYPE_ERROR);
        exec_val_t rhs = eval(e, n->b);
        if (n->op != GLSL_TOK_ASSIGN) {
            const exec_val_t cur = place_read(&p);
            glsl_token_type_t op = GLSL_TOK_PLUS;
            if (n->op == GLSL_TOK_SUB_ASSIGN)
                op = GLSL_TOK_MINUS;
            else if (n->op == GLSL_TOK_MUL_ASSIGN)
                op = GLSL_TOK_STAR;
            else if (n->op == GLSL_TOK_DIV_ASSIGN)
                op = GLSL_TOK_SLASH;
            rhs = arith(e, op, &cur, &rhs, cur.type);
        }
        place_write(e, &p, &rhs);
        return place_read(&p);
    }

    case GLSL_NODE_CONDITIONAL: {
        const exec_val_t c = eval(e, n->a);
        return eval(e, (c.v[0] != 0.0f) ? n->b : n->c);
    }

    case GLSL_NODE_SEQUENCE:
        (void)eval(e, n->a);
        return eval(e, n->b);

    case GLSL_NODE_CALL: {
        const glsl_node_t *callee = &ast->nodes[n->a];
        /* A constructor is a type name in call position, resolved first - `vec4` is
         * never a declared function. */
        static const struct {
            const char *name;
            glsl_type_t type;
        } ctors[] = {
            {"float", GLSL_TYPE_FLOAT},
            {"int", GLSL_TYPE_INT},
            {"bool", GLSL_TYPE_BOOL},
            {"vec2", GLSL_TYPE_VEC2},
            {"vec3", GLSL_TYPE_VEC3},
            {"vec4", GLSL_TYPE_VEC4},
            {"ivec2", GLSL_TYPE_IVEC2},
            {"ivec3", GLSL_TYPE_IVEC3},
            {"ivec4", GLSL_TYPE_IVEC4},
            {"bvec2", GLSL_TYPE_BVEC2},
            {"bvec3", GLSL_TYPE_BVEC3},
            {"bvec4", GLSL_TYPE_BVEC4},
            {"mat2", GLSL_TYPE_MAT2},
            {"mat3", GLSL_TYPE_MAT3},
            {"mat4", GLSL_TYPE_MAT4},
            /* 1.20's non-square matrices and the long spellings of the square ones.
             * Sema and the generator keep matching tables; all three must agree. */
            {"mat2x2", GLSL_TYPE_MAT2},
            {"mat3x3", GLSL_TYPE_MAT3},
            {"mat4x4", GLSL_TYPE_MAT4},
            {"mat2x3", GLSL_TYPE_MAT2X3},
            {"mat2x4", GLSL_TYPE_MAT2X4},
            {"mat3x2", GLSL_TYPE_MAT3X2},
            {"mat3x4", GLSL_TYPE_MAT3X4},
            {"mat4x2", GLSL_TYPE_MAT4X2},
            {"mat4x3", GLSL_TYPE_MAT4X3},
        };
        for (size_t i = 0; i < sizeof(ctors) / sizeof(ctors[0]); i++) {
            if (is_name(callee, ctors[i].name))
                return construct(e, ctors[i].type, n->b);
        }

        /* A struct constructor writes one argument per member at that member's
         * position; sema has checked the count and the types. */
        {
            const glsl_type_t st_type =
                exec_struct_by_name(e, callee->text, callee->length);
            const glsl_struct_t *st = exec_struct(e, st_type);
            if (st) {
                exec_val_t out = val_zero(st_type);
                int i = 0;
                for (int32_t a = n->b; a != GLSL_NO_NODE && i < st->member_count;
                     a = ast->nodes[a].sibling, i++) {
                    const exec_val_t av = eval(e, a);
                    if (e->error)
                        return val_zero(GLSL_TYPE_ERROR);
                    const int w = exec_comps(e, st->member[i].type);
                    for (int k = 0; k < w; k++) {
                        const int at = st->member[i].offset + k;
                        if (at < EXEC_MAX_VAL_FLOATS)
                            out.v[at] = av.v[k];
                    }
                }
                return out;
            }
        }

        GLboolean handled = GL_FALSE;
        const exec_val_t bi = call_builtin(e, callee, n->b, &handled);
        if (handled)
            return bi;
        /* The overload sema chose, since this interpreter has values, not argument
         * types. The name lookup serves units that never went through
         * `glsl_check_unit`, such as `glsl_ps.c`'s. */
        const int32_t fn = (n->resolved != GLSL_NO_NODE)
                               ? n->resolved
                               : find_function(e, callee->text, callee->length);
        if (fn == GLSL_NO_NODE) {
            fail(e, "a call to a function with no body");
            return val_zero(GLSL_TYPE_ERROR);
        }
        return call_user(e, fn, n->b);
    }

    default:
        fail(e, "an expression this shader stage cannot run");
        return val_zero(GLSL_TYPE_ERROR);
    }
}

/* -------------------------------------------------------------------------
 * Statements
 * ------------------------------------------------------------------------- */

static GLboolean exec_stmt(exec_t *e, int32_t node) {
    if (node == GLSL_NO_NODE || e->error)
        return GL_FALSE;
    if (--e->steps <= 0) {
        fail(e, "this shader ran past the step budget for one invocation");
        return GL_FALSE;
    }
    const glsl_ast_t *ast = &e->unit->ast;
    const glsl_node_t *n = &ast->nodes[node];

    switch (n->kind) {
    case GLSL_NODE_COMPOUND:
        scope_push(e);
        for (int32_t st = n->a; st != GLSL_NO_NODE; st = ast->nodes[st].sibling) {
            if (!exec_stmt(e, st))
                break;
            if (e->flow != FLOW_NORMAL)
                break;
        }
        scope_pop(e);
        return (GLboolean)(e->error == (const char *)0);

    case GLSL_NODE_EXPR_STMT:
        if (n->a != GLSL_NO_NODE)
            (void)eval(e, n->a);
        return (GLboolean)(e->error == (const char *)0);

    case GLSL_NODE_DECL: {
        const glsl_type_t t = exec_node_type(e, n);
        int elements = 0;
        if (n->array_size != GLSL_NO_NODE) {
            const glsl_node_t *sz = &ast->nodes[n->array_size];
            elements = (sz->kind == GLSL_NODE_INTCONST) ? (int)sz->value : 0;
        }
        /* An array with an initialiser is GLSL 1.20's array constructor; its arguments
         * fill the run one element each, since `init` below holds only one value. */
        if (elements > 0 && n->a != GLSL_NO_NODE) {
            glsl_type_t ael = GLSL_TYPE_ERROR;
            int acount = 0;
            if (!glsl_array_ctor_shape(ast, n->a, &ael, &acount) ||
                acount != elements) {
                fail(e, "an array takes an array constructor of its own length here");
                return GL_FALSE;
            }
            const int w = exec_comps(e, t);
            exec_val_t args[GLSL_MAX_ARRAY_CTOR_ARGS];
            int given = 0;
            for (int32_t a = ast->nodes[n->a].b;
                 a != GLSL_NO_NODE && given < GLSL_MAX_ARRAY_CTOR_ARGS;
                 a = ast->nodes[a].sibling) {
                args[given++] = eval(e, a);
            }
            float *arr = declare(e, n->text, n->length, t, elements);
            if (arr) {
                for (int i = 0; i < given && i < elements; i++) {
                    for (int c = 0; c < w && c < EXEC_MAX_VAL_FLOATS; c++) {
                        arr[i * w + c] = args[i].v[c];
                    }
                }
            }
            return (GLboolean)(e->error == (const char *)0);
        }
        /* The initialiser is evaluated before the name exists, so `float x = x;` reads
         * an outer `x` or fails. */
        exec_val_t init = val_zero(t);
        const GLboolean has_init = (GLboolean)(n->a != GLSL_NO_NODE);
        if (has_init)
            init = eval(e, n->a);
        float *store = declare(e, n->text, n->length, t, elements);
        if (store && has_init) {
            /* `exec_comps`, so a struct initialiser copies every component. */
            const int n_comp = exec_comps(e, t);
            for (int i = 0; i < n_comp && i < EXEC_MAX_VAL_FLOATS; i++)
                store[i] = init.v[i];
        }
        return (GLboolean)(e->error == (const char *)0);
    }

    case GLSL_NODE_IF: {
        const exec_val_t c = eval(e, n->a);
        if (c.v[0] != 0.0f)
            return exec_stmt(e, n->b);
        if (n->c != GLSL_NO_NODE)
            return exec_stmt(e, n->c);
        return (GLboolean)(e->error == (const char *)0);
    }

    case GLSL_NODE_WHILE:
        scope_push(e);
        for (;;) {
            const exec_val_t c = eval(e, n->a);
            if (e->error || c.v[0] == 0.0f)
                break;
            exec_stmt(e, n->b);
            if (e->flow == FLOW_BREAK) {
                e->flow = FLOW_NORMAL;
                break;
            }
            if (e->flow == FLOW_CONTINUE)
                e->flow = FLOW_NORMAL;
            if (e->flow != FLOW_NORMAL || e->error)
                break;
        }
        scope_pop(e);
        return (GLboolean)(e->error == (const char *)0);

    case GLSL_NODE_DO_WHILE:
        scope_push(e);
        for (;;) {
            exec_stmt(e, n->a);
            if (e->flow == FLOW_BREAK) {
                e->flow = FLOW_NORMAL;
                break;
            }
            if (e->flow == FLOW_CONTINUE)
                e->flow = FLOW_NORMAL;
            if (e->flow != FLOW_NORMAL || e->error)
                break;
            const exec_val_t c = eval(e, n->b);
            if (e->error || c.v[0] == 0.0f)
                break;
        }
        scope_pop(e);
        return (GLboolean)(e->error == (const char *)0);

    case GLSL_NODE_FOR:
        /* The initialiser's scope is the loop's, so `for (int i = 0; ...)` declares an
         * `i` that ends with the loop. */
        scope_push(e);
        if (n->a != GLSL_NO_NODE)
            exec_stmt(e, n->a);
        for (;;) {
            if (n->b != GLSL_NO_NODE) {
                const exec_val_t c = eval(e, n->b);
                if (e->error || c.v[0] == 0.0f)
                    break;
            }
            exec_stmt(e, n->d);
            if (e->flow == FLOW_BREAK) {
                e->flow = FLOW_NORMAL;
                break;
            }
            if (e->flow == FLOW_CONTINUE)
                e->flow = FLOW_NORMAL;
            if (e->flow != FLOW_NORMAL || e->error)
                break;
            /* `continue` still runs the increment. */
            if (n->c != GLSL_NO_NODE)
                (void)eval(e, n->c);
        }
        scope_pop(e);
        return (GLboolean)(e->error == (const char *)0);

    case GLSL_NODE_RETURN:
        if (n->a != GLSL_NO_NODE)
            e->ret = eval(e, n->a);
        e->flow = FLOW_RETURN;
        return GL_TRUE;

    case GLSL_NODE_BREAK:
        e->flow = FLOW_BREAK;
        return GL_TRUE;

    case GLSL_NODE_CONTINUE:
        e->flow = FLOW_CONTINUE;
        return GL_TRUE;

    case GLSL_NODE_DISCARD:
        /* `discard` ends the fragment, not the function: it unwinds through every call
         * and the fragment writes no colour, depth or stencil. */
        e->flow = FLOW_DISCARD;
        return GL_TRUE;

    default:
        /* A function definition nested in a body, or anything else the grammar allows
         * at top level only. */
        return (GLboolean)(e->error == (const char *)0);
    }
}

/*
 * The shader's own unqualified globals, such as `const float pi = 3.14159265;`. They
 * run through the local-declaration arm in source order, so one may use an earlier
 * one, and into the global scope before `main`'s, so a local may shadow one.
 *
 * Attributes, varyings and uniforms are skipped: each is already bound, and running
 * its declaration would overwrite the binding.
 */
static void declare_shader_globals(exec_t *e, const glsl_unit_t *u) {
    if (!u || u->root == GLSL_NO_NODE)
        return;
    const glsl_ast_t *ast = &u->ast;
    for (int32_t d = ast->nodes[u->root].a; d != GLSL_NO_NODE;
         d = ast->nodes[d].sibling) {
        const glsl_node_t *n = &ast->nodes[d];
        if (n->kind != GLSL_NODE_DECL)
            continue;
        if (n->qualifier == GLSL_TOK_KW_UNIFORM ||
            n->qualifier == GLSL_TOK_KW_VARYING ||
            n->qualifier == GLSL_TOK_KW_ATTRIBUTE) {
            continue;
        }
        if (!exec_stmt(e, d))
            return;
    }
}

/* -------------------------------------------------------------------------
 * Setting up an invocation
 * ------------------------------------------------------------------------- */

static void set_mat4(float *dst, const gl_mat4_t *m) {
    for (int i = 0; i < 16; i++)
        dst[i] = m->m[i];
}

/* The fixed-function state a 1.10 shader may read as a built-in uniform. Declared for
 * both stages, because either may use them. */
static void bind_builtin_uniforms(exec_t *e) {
    gl_context_t *ctx = e->ctx;
    if (!ctx)
        return;
    gl_update_mvp(ctx);
    gl_update_normal_matrix(ctx);

    float *p;
    if ((p = declare(e, "gl_ModelViewMatrix", 18u, GLSL_TYPE_MAT4, 0))) {
        set_mat4(p, &ctx->modelview_stack[ctx->modelview_depth]);
    }
    if ((p = declare(e, "gl_ProjectionMatrix", 19u, GLSL_TYPE_MAT4, 0))) {
        set_mat4(p, &ctx->projection_stack[ctx->projection_depth]);
    }
    if ((p = declare(e, "gl_ModelViewProjectionMatrix", 28u, GLSL_TYPE_MAT4, 0))) {
        set_mat4(p, &ctx->mvp);
    }
    if ((p = declare(e, "gl_NormalMatrix", 15u, GLSL_TYPE_MAT3, 0))) {
        /* The context keeps the normal matrix row-major and GLSL reads a `mat3`
         * column-major, so it is transposed here. */
        for (int col = 0; col < 3; col++) {
            for (int row = 0; row < 3; row++)
                p[col * 3 + row] = ctx->normal_matrix[row * 3 + col];
        }
    }
    if ((p = declare(e, "gl_TextureMatrix", 16u, GLSL_TYPE_MAT4,
                     OOPS_GL_MAX_TEXTURE_UNITS))) {
        for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
            const gl_tex_unit_t *tu = &ctx->tex_unit[u];
            set_mat4(p + u * 16u, &tu->texture_stack[tu->texture_depth]);
        }
    }
}

/* The program's own uniforms, from the value pool the API writes. */
static void bind_program_uniforms(exec_t *e) {
    gl_program_object_t *prog = e->prog;
    if (!prog || !prog->values)
        return;
    for (int i = 0; i < prog->uniform_count; i++) {
        const gl_uniform_t *u = &prog->uniforms[i];
        /* The declared front-end type, recovered from the table's GL enumerant. A
         * sampler's type tells a lookup which target to read; see `sampler_type_of`. */
        glsl_type_t t = GLSL_TYPE_FLOAT;
        switch (u->type) {
        case GL_FLOAT:
            t = GLSL_TYPE_FLOAT;
            break;
        case GL_FLOAT_VEC2:
            t = GLSL_TYPE_VEC2;
            break;
        case GL_FLOAT_VEC3:
            t = GLSL_TYPE_VEC3;
            break;
        case GL_FLOAT_VEC4:
            t = GLSL_TYPE_VEC4;
            break;
        case GL_INT:
            t = GLSL_TYPE_INT;
            break;
        case GL_INT_VEC2:
            t = GLSL_TYPE_IVEC2;
            break;
        case GL_INT_VEC3:
            t = GLSL_TYPE_IVEC3;
            break;
        case GL_INT_VEC4:
            t = GLSL_TYPE_IVEC4;
            break;
        case GL_BOOL:
            t = GLSL_TYPE_BOOL;
            break;
        case GL_BOOL_VEC2:
            t = GLSL_TYPE_BVEC2;
            break;
        case GL_BOOL_VEC3:
            t = GLSL_TYPE_BVEC3;
            break;
        case GL_BOOL_VEC4:
            t = GLSL_TYPE_BVEC4;
            break;
        case GL_FLOAT_MAT2:
            t = GLSL_TYPE_MAT2;
            break;
        case GL_FLOAT_MAT3:
            t = GLSL_TYPE_MAT3;
            break;
        case GL_FLOAT_MAT4:
            t = GLSL_TYPE_MAT4;
            break;
        case GL_FLOAT_MAT2x3:
            t = GLSL_TYPE_MAT2X3;
            break;
        case GL_FLOAT_MAT2x4:
            t = GLSL_TYPE_MAT2X4;
            break;
        case GL_FLOAT_MAT3x2:
            t = GLSL_TYPE_MAT3X2;
            break;
        case GL_FLOAT_MAT3x4:
            t = GLSL_TYPE_MAT3X4;
            break;
        case GL_FLOAT_MAT4x2:
            t = GLSL_TYPE_MAT4X2;
            break;
        case GL_FLOAT_MAT4x3:
            t = GLSL_TYPE_MAT4X3;
            break;
        case GL_SAMPLER_1D:
            t = GLSL_TYPE_SAMPLER1D;
            break;
        case GL_SAMPLER_2D:
            t = GLSL_TYPE_SAMPLER2D;
            break;
        case GL_SAMPLER_3D:
            t = GLSL_TYPE_SAMPLER3D;
            break;
        case GL_SAMPLER_CUBE:
            t = GLSL_TYPE_SAMPLERCUBE;
            break;
        case GL_SAMPLER_1D_SHADOW:
            t = GLSL_TYPE_SAMPLER1DSHADOW;
            break;
        case GL_SAMPLER_2D_SHADOW:
            t = GLSL_TYPE_SAMPLER2DSHADOW;
            break;
        default:
            break;
        }
        float *store =
            declare(e, u->name, lit_len(u->name), t, (u->size > 1) ? u->size : 0);
        if (!store)
            return;
        const int total = u->floats * u->size;
        for (int k = 0; k < total; k++)
            store[k] = prog->values[u->offset + k];
    }
}

/* -------------------------------------------------------------------------
 * The vertex stage
 * ------------------------------------------------------------------------- */

/* The shader's own `attribute` variables, from the slot each was given at link time.
 * The values come off the vertex, which the vertex fetch filled from the enabled array
 * or from `glVertexAttrib`'s current value. */
static void bind_generic_attributes(exec_t *e, const gl_vertex_t *vtx) {
    const glsl_ast_t *ast = &e->unit->ast;
    gl_program_object_t *prog = e->prog;
    gl_context_t *ctx = e->ctx;
    if (!prog || !ctx || !vtx)
        return;
    for (int32_t d = ast->nodes[e->unit->root].a; d != GLSL_NO_NODE;
         d = ast->nodes[d].sibling) {
        const glsl_node_t *n = &ast->nodes[d];
        if (n->kind != GLSL_NODE_DECL || n->qualifier != GLSL_TOK_KW_ATTRIBUTE)
            continue;
        if (n->length >= 3u && n->text[0] == 'g' && n->text[1] == 'l' &&
            n->text[2] == '_') {
            continue; /* the fixed-function ones are bound separately */
        }
        const glsl_type_t t = glsl_type_from_token(n->type_tok);
        float *store = declare(e, n->text, n->length, t, 0);
        if (!store)
            return;
        /* Which slot: the location the linker gave this name. */
        GLint loc = -1;
        for (int i = 0; i < prog->attrib_count; i++) {
            if (name_eq(prog->attribs[i].name, lit_len(prog->attribs[i].name), n->text,
                        n->length)) {
                loc = prog->attribs[i].location;
                break;
            }
        }
        if (loc < 0 || loc >= OOPS_GL_MAX_VERTEX_ATTRIBS)
            continue;
        const int want = comps_of(t);
        const int mcols = glsl_type_matrix_cols(t), mrows = glsl_type_matrix_rows(t);
        if (mcols > 0) {
            /* A matrix attribute takes one slot per column, and a slot supplies as many
             * components as the matrix has rows. */
            for (int c = 0; c < mcols; c++) {
                const int slot = loc + c;
                if (slot >= OOPS_GL_MAX_VERTEX_ATTRIBS)
                    break;
                for (int r = 0; r < mrows; r++)
                    store[c * mrows + r] = vtx->attrib[slot][r];
            }
            continue;
        }
        for (int i = 0; i < want; i++)
            store[i] = vtx->attrib[loc][i];
    }
}

/* The fixed-function per-vertex attributes a 1.10 shader may read. */
static void bind_fixed_attributes(exec_t *e, const gl_vertex_t *v) {
    float *p;
    if ((p = declare(e, "gl_Vertex", 9u, GLSL_TYPE_VEC4, 0))) {
        p[0] = v->x;
        p[1] = v->y;
        p[2] = v->z;
        p[3] = v->w;
    }
    if ((p = declare(e, "gl_Normal", 9u, GLSL_TYPE_VEC3, 0))) {
        p[0] = v->nx;
        p[1] = v->ny;
        p[2] = v->nz;
    }
    if ((p = declare(e, "gl_Color", 8u, GLSL_TYPE_VEC4, 0))) {
        p[0] = v->r;
        p[1] = v->g;
        p[2] = v->b;
        p[3] = v->a;
    }
    if ((p = declare(e, "gl_SecondaryColor", 17u, GLSL_TYPE_VEC4, 0))) {
        p[0] = v->sr;
        p[1] = v->sg;
        p[2] = v->sb;
        p[3] = 1.0f;
    }
    if ((p = declare(e, "gl_FogCoord", 11u, GLSL_TYPE_FLOAT, 0)))
        p[0] = v->fogc;
    static const char *const MULTI[8] = {"gl_MultiTexCoord0", "gl_MultiTexCoord1",
                                         "gl_MultiTexCoord2", "gl_MultiTexCoord3",
                                         "gl_MultiTexCoord4", "gl_MultiTexCoord5",
                                         "gl_MultiTexCoord6", "gl_MultiTexCoord7"};
    for (int u = 0; u < 8; u++) {
        if (!(p = declare(e, MULTI[u], 17u, GLSL_TYPE_VEC4, 0)))
            return;
        if (u < OOPS_GL_MAX_TEXTURE_UNITS) {
            for (int k = 0; k < 4; k++)
                p[k] = v->tc[u][k];
        } else {
            /* A unit this implementation does not have reads as the attribute's
             * default, (0, 0, 0, 1). */
            p[0] = 0.0f;
            p[1] = 0.0f;
            p[2] = 0.0f;
            p[3] = 1.0f;
        }
    }
}

/* The vertex stage's outputs, each with storage that is copied into the interpolated
 * block after `main` returns. */
typedef struct {
    const char *name;
    int offset; /* into the block, or -1 for one that is written and dropped */
    int floats;
    int array;
} out_slot_t;

GLboolean gl_shader_run_vertex(gl_context_t *ctx, gl_program_object_t *p,
                               const gl_vertex_t *ff, gl_shader_vertex_out_t *out) {
    if (!ctx || !p || !p->linked || !out)
        return GL_FALSE;
    if (!p->vs)
        return GL_FALSE;

    exec_t *e = (exec_t *)gl_heap_alloc(sizeof(exec_t));
    if (!e)
        return GL_FALSE;
    for (size_t i = 0; i < sizeof(exec_t); i++)
        ((char *)e)[i] = 0;
    e->ctx = ctx;
    e->prog = p;
    e->unit = p->vs;
    e->stage = GL_VERTEX_SHADER;
    e->steps = EXEC_STEP_BUDGET;
    e->which = 0;
    e->has_neighbours = GL_FALSE;

    bind_builtin_uniforms(e);
    bind_program_uniforms(e);
    if (ff)
        bind_fixed_attributes(e, ff);
    bind_generic_attributes(e, ff);

    /* `gl_Position` starts at (0, 0, 0, 1) so a shader that writes only `.xyz` still
     * has a usable w. */
    float *pos = declare(e, "gl_Position", 11u, GLSL_TYPE_VEC4, 0);
    if (pos)
        pos[3] = 1.0f;
    float *psize = declare(e, "gl_PointSize", 12u, GLSL_TYPE_FLOAT, 0);
    if (psize)
        psize[0] = ctx->point_size;
    float *clipv = declare(e, "gl_ClipVertex", 13u, GLSL_TYPE_VEC4, 0);
    float *front = declare(e, "gl_FrontColor", 13u, GLSL_TYPE_VEC4, 0);
    float *back = declare(e, "gl_BackColor", 12u, GLSL_TYPE_VEC4, 0);
    float *fsec = declare(e, "gl_FrontSecondaryColor", 22u, GLSL_TYPE_VEC4, 0);
    float *bsec = declare(e, "gl_BackSecondaryColor", 21u, GLSL_TYPE_VEC4, 0);
    float *tc =
        declare(e, "gl_TexCoord", 11u, GLSL_TYPE_VEC4, OOPS_GL_MAX_TEXTURE_UNITS);
    float *fogf = declare(e, "gl_FogFragCoord", 15u, GLSL_TYPE_FLOAT, 0);

    /* The shader's own varyings, written here and copied into the block after `main`
     * returns. Declared from the tree rather than from the program's varying table,
     * because only the tree carries each one's type and array length. */
    {
        const glsl_ast_t *ast = &p->vs->ast;
        for (int32_t d = ast->nodes[p->vs->root].a; d != GLSL_NO_NODE;
             d = ast->nodes[d].sibling) {
            const glsl_node_t *n = &ast->nodes[d];
            if (n->kind != GLSL_NODE_DECL || n->qualifier != GLSL_TOK_KW_VARYING)
                continue;
            if (n->length >= 3u && n->text[0] == 'g' && n->text[1] == 'l' &&
                n->text[2] == '_') {
                continue;
            }
            int elements = 0;
            if (n->array_size != GLSL_NO_NODE) {
                const glsl_node_t *sz = &ast->nodes[n->array_size];
                elements = (sz->kind == GLSL_NODE_INTCONST) ? (int)sz->value : 0;
            }
            (void)declare(e, n->text, n->length, glsl_type_from_token(n->type_tok),
                          elements);
        }
    }

    declare_shader_globals(e, p->vs);

    /* Everything global is in scope; `main`'s locals go above it. */
    const int32_t main_fn = find_function(e, "main", 4u);
    if (main_fn == GLSL_NO_NODE) {
        gl_heap_free(e);
        return GL_FALSE;
    }
    scope_push(e);
    for (int32_t st = p->vs->ast.nodes[p->vs->ast.nodes[main_fn].c].a;
         st != GLSL_NO_NODE; st = p->vs->ast.nodes[st].sibling) {
        if (!exec_stmt(e, st))
            break;
        if (e->flow != FLOW_NORMAL)
            break;
    }
    scope_pop(e);

    const GLboolean ok = (GLboolean)(e->error == (const char *)0);
    if (ok) {
        for (int i = 0; i < 4; i++)
            out->position[i] = pos ? pos[i] : 0.0f;
        out->point_size = psize ? psize[0] : ctx->point_size;
        /* A `gl_ClipVertex` still all zeros is one the shader never wrote: variables
         * have no write flag, and the origin with w = 0 is no position a shader means.
         */
        out->wrote_clip_vertex = GL_FALSE;
        for (int i = 0; i < 4; i++) {
            out->clip_vertex[i] = clipv ? clipv[i] : 0.0f;
            if (out->clip_vertex[i] != 0.0f)
                out->wrote_clip_vertex = GL_TRUE;
        }
        for (int i = 0; i < GL_SHADER_VARY_FLOATS; i++)
            out->vary[i] = 0.0f;
        /* `gl_FrontColor` is what the fragment stage reads as `gl_Color`. The back
         * colours are written and dropped: the rasteriser does not choose a side per
         * primitive. */
        if (front)
            for (int i = 0; i < 4; i++)
                out->vary[GL_SHADER_VARY_COLOR + i] = front[i];
        if (fsec)
            for (int i = 0; i < 4; i++)
                out->vary[GL_SHADER_VARY_SECONDARY + i] = fsec[i];
        if (tc) {
            for (int u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
                for (int i = 0; i < 4; i++) {
                    out->vary[GL_SHADER_VARY_TEXCOORD + u * 4 + i] = tc[u * 4 + i];
                }
            }
        }
        if (fogf)
            out->vary[GL_SHADER_VARY_FOG] = fogf[0];
        /* The point sprite's coordinate outlives the vertex shader. The point expansion
         * writes it into the corner's `tc[0]`, and `gl_PointCoord` reads texture
         * coordinate 0's interpolant, which a vertex shader cannot write. Only s and t,
         * and only when the fragment stage reads `gl_PointCoord`; the link refuses a
         * program that also reads `gl_TexCoord`. */
        if (p->hw_reads_point_coord && ff) {
            out->vary[GL_SHADER_VARY_TEXCOORD + 0] = ff->tc[0][0];
            out->vary[GL_SHADER_VARY_TEXCOORD + 1] = ff->tc[0][1];
        }
        (void)back;
        (void)bsec;

        for (int i = 0; i < p->varying_count; i++) {
            exec_var_t *v =
                lookup(e, p->varyings[i].name, lit_len(p->varyings[i].name));
            if (!v || !v->store)
                continue;
            for (int k = 0; k < p->varyings[i].floats; k++) {
                out->vary[p->varyings[i].offset + k] = v->store[k];
            }
        }
    } else if (e->error) {
        oops_log_info("GL", "%s", e->error);
    }
    gl_heap_free(e);
    return ok;
}

/* -------------------------------------------------------------------------
 * The fragment stage
 * ------------------------------------------------------------------------- */

GLboolean gl_shader_run_fragment(gl_context_t *ctx, gl_program_object_t *p,
                                 const gl_shader_fragment_in_t *in,
                                 gl_shader_fragment_out_t *out) {
    if (!ctx || !p || !p->linked || !in || !out)
        return GL_FALSE;
    if (!p->fs)
        return GL_FALSE;

    out->discarded = GL_FALSE;
    out->wrote_depth = GL_FALSE;

    exec_t *e = (exec_t *)gl_heap_alloc(sizeof(exec_t));
    if (!e)
        return GL_FALSE;
    for (size_t i = 0; i < sizeof(exec_t); i++)
        ((char *)e)[i] = 0;
    e->ctx = ctx;
    e->prog = p;
    e->unit = p->fs;
    e->stage = GL_FRAGMENT_SHADER;
    e->steps = EXEC_STEP_BUDGET;
    e->vary[0] = in->vary;
    e->vary[1] = in->vary_dx;
    e->vary[2] = in->vary_dy;
    e->which = 0;
    e->has_neighbours =
        (GLboolean)(in->vary_dx != (const float *)0 && in->vary_dy != (const float *)0);

    bind_builtin_uniforms(e);
    bind_program_uniforms(e);

    /* The inputs. Each is a window into the interpolated block, so a derivative of one
     * is the same mechanism as a derivative of a varying. */
    declare_vary_in(e, "gl_Color", GLSL_TYPE_VEC4, 0, GL_SHADER_VARY_COLOR);
    declare_vary_in(e, "gl_SecondaryColor", GLSL_TYPE_VEC4, 0,
                    GL_SHADER_VARY_SECONDARY);
    declare_vary_in(e, "gl_TexCoord", GLSL_TYPE_VEC4, OOPS_GL_MAX_TEXTURE_UNITS,
                    GL_SHADER_VARY_TEXCOORD);
    declare_vary_in(e, "gl_FogFragCoord", GLSL_TYPE_FLOAT, 0, GL_SHADER_VARY_FOG);
    /* `gl_PointCoord` is the s and t of texture coordinate 0, where the point expansion
     * puts the sprite coordinate: the slot `GL_COORD_REPLACE` fills, and the hardware's
     * `SPI_PS_INPUT_CNTL.PT_SPRITE_TEX` substitution. The link refuses a shader that
     * reads both this and `gl_TexCoord[0]`. */
    declare_vary_in(e, "gl_PointCoord", GLSL_TYPE_VEC2, 0, GL_SHADER_VARY_TEXCOORD);
    {
        const glsl_ast_t *ast = &p->fs->ast;
        for (int32_t d = ast->nodes[p->fs->root].a; d != GLSL_NO_NODE;
             d = ast->nodes[d].sibling) {
            const glsl_node_t *n = &ast->nodes[d];
            if (n->kind != GLSL_NODE_DECL || n->qualifier != GLSL_TOK_KW_VARYING)
                continue;
            if (n->length >= 3u && n->text[0] == 'g' && n->text[1] == 'l' &&
                n->text[2] == '_') {
                continue;
            }
            for (int i = 0; i < p->varying_count; i++) {
                if (!name_eq(p->varyings[i].name, lit_len(p->varyings[i].name), n->text,
                             n->length)) {
                    continue;
                }
                int elements = 0;
                if (n->array_size != GLSL_NO_NODE) {
                    const glsl_node_t *sz = &ast->nodes[n->array_size];
                    elements = (sz->kind == GLSL_NODE_INTCONST) ? (int)sz->value : 0;
                }
                declare_vary_in(e, p->varyings[i].name,
                                glsl_type_from_token(n->type_tok), elements,
                                p->varyings[i].offset);
                break;
            }
        }
    }

    /* `gl_FragCoord` is the window position, not an interpolant, so it is copied in
     * directly. */
    float *fc = declare(e, "gl_FragCoord", 12u, GLSL_TYPE_VEC4, 0);
    if (fc)
        for (int i = 0; i < 4; i++)
            fc[i] = in->frag_coord[i];
    float *ff = declare(e, "gl_FrontFacing", 14u, GLSL_TYPE_BOOL, 0);
    if (ff)
        ff[0] = in->front_facing ? 1.0f : 0.0f;

    float *colour = declare(e, "gl_FragColor", 12u, GLSL_TYPE_VEC4, 0);
    float *data = declare(e, "gl_FragData", 11u, GLSL_TYPE_VEC4, 1);
    float *depth = declare(e, "gl_FragDepth", 12u, GLSL_TYPE_FLOAT, 0);
    if (depth)
        depth[0] = in->frag_coord[2];

    declare_shader_globals(e, p->fs);

    const int32_t main_fn = find_function(e, "main", 4u);
    if (main_fn == GLSL_NO_NODE) {
        gl_heap_free(e);
        return GL_FALSE;
    }
    scope_push(e);
    for (int32_t st = p->fs->ast.nodes[p->fs->ast.nodes[main_fn].c].a;
         st != GLSL_NO_NODE; st = p->fs->ast.nodes[st].sibling) {
        if (!exec_stmt(e, st))
            break;
        if (e->flow != FLOW_NORMAL)
            break;
    }
    scope_pop(e);

    const GLboolean ok = (GLboolean)(e->error == (const char *)0);
    if (ok) {
        out->discarded = (GLboolean)(e->flow == FLOW_DISCARD);
        /* A shader writes one or the other; `gl_FragData[0]` is the same buffer as
         * `gl_FragColor` on a single-target framebuffer, so whichever was written is
         * taken and the colour wins when both were. */
        const float *src = colour;
        if (colour && data) {
            const GLboolean colour_written =
                (GLboolean)(colour[0] != 0.0f || colour[1] != 0.0f ||
                            colour[2] != 0.0f || colour[3] != 0.0f);
            if (!colour_written)
                src = data;
        }
        for (int i = 0; i < 4; i++)
            out->colour[i] = src ? src[i] : 0.0f;
        if (depth) {
            out->depth = depth[0];
            out->wrote_depth = (GLboolean)(depth[0] != in->frag_coord[2]);
        }
    } else if (e->error) {
        oops_log_info("GL", "%s", e->error);
    }
    gl_heap_free(e);
    return ok;
}

/* -------------------------------------------------------------------------
 * Whether a fragment needs its neighbours
 * ------------------------------------------------------------------------- */

GLboolean gl_shader_needs_derivatives(const gl_program_object_t *p) {
    if (!p || !p->fs)
        return GL_FALSE;
    const glsl_ast_t *ast = &p->fs->ast;
    for (int32_t i = 0; i < ast->count; i++) {
        const glsl_node_t *n = &ast->nodes[i];
        if (n->kind != GLSL_NODE_IDENTIFIER)
            continue;
        /* Any name that could be a derivative-taking built-in. A false positive costs
         * two extra evaluations; a miss would cost the wrong level of detail. */
        if (name_eq(n->text, n->length, "dFdx", 4u) ||
            name_eq(n->text, n->length, "dFdy", 4u) ||
            name_eq(n->text, n->length, "fwidth", 6u)) {
            return GL_TRUE;
        }
        if (n->length >= 7u && n->text[0] == 't' && n->text[1] == 'e' &&
            n->text[2] == 'x' && n->text[3] == 't' && n->text[4] == 'u' &&
            n->text[5] == 'r' && n->text[6] == 'e') {
            return GL_TRUE; /* every `texture*` lookup without an explicit level */
        }
        if (n->length >= 6u && n->text[0] == 's' && n->text[1] == 'h' &&
            n->text[2] == 'a' && n->text[3] == 'd' && n->text[4] == 'o' &&
            n->text[5] == 'w') {
            return GL_TRUE;
        }
    }
    return GL_FALSE;
}
