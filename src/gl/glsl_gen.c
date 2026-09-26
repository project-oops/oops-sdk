/*
 * oops-gl: instruction selection - the GLSL tree becomes gfx1030 instructions.
 *
 * Sits between `glsl_sema.c`, which types every expression, and `glsl_emit.c`, which
 * encodes one instruction. A value occupies consecutive VGPRs, one per component; a
 * matrix is column-major. A bool is 0.0 or 1.0 and an int is a float kept whole, so
 * there is one register class. Only operators whose opcodes have been read out of a
 * real assembler are generated; anything else sets `error` and emits nothing, because a
 * shader built from guessed instructions is wrong only on hardware (D009).
 */

#include "glsl_internal.h"

/* -------------------------------------------------------------------------
 * The generator's own state
 * ------------------------------------------------------------------------- */

void glsl_gen_init(glsl_gen_t *g, glsl_ast_t *ast, glsl_sema_t *sema,
                   glsl_code_t *code) {
    if (!g)
        return;
    g->ast = ast;
    g->sema = sema;
    g->code = code;
    g->next_vgpr = 0u;
    g->high_water = 0u;
    g->var_count = 0;
    g->exec_depth = 0;
    g->loop_depth = 0;
    g->inline_depth = 0;
    /* Written at every call before they are read, and cleared anyway: this struct is
     * initialised field by field rather than zeroed, so every field needs a line here.
     */
    for (int i = 0; i < GLSL_GEN_MAX_INLINE_DEPTH; i++) {
        g->fn_out[i].base = 0u;
        g->fn_out[i].count = 0;
        g->fn_exec_depth[i] = 0;
        g->fn_loop_depth[i] = 0;
        g->fn_saved[i] = GL_FALSE;
    }
    g->wqm = GL_FALSE;
    g->sampler_count = 0;
    g->error = (const char *)0;
    g->error_line = 0;
    g->error_column = 0;
}

/* The first failure is the one reported, and it stops everything after it: a generator
 * that carried on would emit instructions for a tree it had already said it did not
 * understand. */
static glsl_value_t gen_fail(glsl_gen_t *g, const char *msg, int32_t node) {
    glsl_value_t none;
    none.base = 0u;
    none.count = 0;
    if (g && !g->error) {
        g->error = msg;
        if (g->ast && node != GLSL_NO_NODE) {
            g->error_line = g->ast->nodes[node].line;
            g->error_column = g->ast->nodes[node].column;
        }
    }
    return none;
}

static GLboolean is_bad(glsl_value_t v) {
    return v.count == 0 ? GL_TRUE : GL_FALSE;
}

/* A bump allocator over the VGPR file, with a mark that statements roll back to.
 * Variables are allocated below the mark and never move; temporaries come from above it
 * and are reclaimed when the statement ends. Exhaustion is an error, never a wrap,
 * which would alias a temporary onto a variable. */
static glsl_value_t gen_alloc(glsl_gen_t *g, int count, int32_t node) {
    glsl_value_t v;
    if (count <= 0 || (uint32_t)count > GLSL_MAX_VGPRS - g->next_vgpr) {
        return gen_fail(g, "the shader needs more registers than the file has", node);
    }
    v.base = g->next_vgpr;
    v.count = count;
    g->next_vgpr += (uint32_t)count;
    if (g->next_vgpr > g->high_water)
        g->high_water = g->next_vgpr;
    return v;
}

static uint32_t gen_mark(const glsl_gen_t *g) {
    return g->next_vgpr;
}
static void gen_release(glsl_gen_t *g, uint32_t mark) {
    g->next_vgpr = mark;
}

/* -------------------------------------------------------------------------
 * Variables
 * ------------------------------------------------------------------------- */

static GLboolean name_is(const glsl_gen_var_t *v, const char *name, size_t len) {
    if (v->name_len != len)
        return GL_FALSE;
    for (size_t i = 0; i < len; i++) {
        if (v->name[i] != name[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

static glsl_gen_var_t *gen_find(glsl_gen_t *g, const char *name, size_t len) {
    /* Backwards, so an inner declaration shadows an outer one the way the scope rules
     * say. */
    for (int i = g->var_count - 1; i >= 0; i--) {
        if (name_is(&g->vars[i], name, len))
            return &g->vars[i];
    }
    return (glsl_gen_var_t *)0;
}

static glsl_gen_var_t *gen_declare(glsl_gen_t *g, const char *name, size_t len,
                                   glsl_type_t type, glsl_value_t home, int32_t node) {
    if (g->var_count >= GLSL_GEN_MAX_VARS) {
        (void)gen_fail(g, "too many variables in this shader", node);
        return (glsl_gen_var_t *)0;
    }
    glsl_gen_var_t *v = &g->vars[g->var_count++];
    v->name = name;
    v->name_len = len;
    v->type = type;
    v->value = home;
    v->array_size = 0; /* the declarator sets it; everything else is a plain variable */
    v->is_const = GL_FALSE;
    v->const_val = 0.0;
    return v;
}

/* A compile-time constant, or false. Literals, a negation of one, and an unrolled
 * loop's counter, which is a different constant in each copy of the body and makes
 * `w[i]` an index this can resolve. See `is_const` for why no other variable qualifies.
 */
static GLboolean const_of(const glsl_gen_t *g, int32_t node, double *out) {
    if (node == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *n = &g->ast->nodes[node];
    if (n->kind == GLSL_NODE_INTCONST || n->kind == GLSL_NODE_FLOATCONST) {
        *out = n->value;
        return GL_TRUE;
    }
    if (n->kind == GLSL_NODE_UNARY && n->op == GLSL_TOK_MINUS) {
        double inner = 0.0;
        if (!const_of(g, n->a, &inner))
            return GL_FALSE;
        *out = -inner;
        return GL_TRUE;
    }
    if (n->kind == GLSL_NODE_IDENTIFIER) {
        /* Backwards, so an inner declaration shadows an outer one, the same rule
         * `gen_find` follows. */
        for (int i = g->var_count - 1; i >= 0; i--) {
            if (name_is(&g->vars[i], n->text, n->length)) {
                if (!g->vars[i].is_const)
                    return GL_FALSE;
                *out = g->vars[i].const_val;
                return GL_TRUE;
            }
        }
        /* A built-in constant (`const int`, GLSL 1.10 7.4), after the declared names so
         * a shader that shadows one gets its own. */
        int bi = 0;
        if (glsl_builtin_const_int(n->text, n->length, &bi)) {
            *out = (double)bi;
            return GL_TRUE;
        }
    }
    return GL_FALSE;
}

/* -------------------------------------------------------------------------
 * Types this stage will generate for
 * ------------------------------------------------------------------------- */

/* The float family. `bool` and `int` have their own tests below. */
static GLboolean is_float_family(glsl_type_t t) {
    switch (t) {
    case GLSL_TYPE_FLOAT:
    case GLSL_TYPE_VEC2:
    case GLSL_TYPE_VEC3:
    case GLSL_TYPE_VEC4:
    case GLSL_TYPE_MAT2:
    case GLSL_TYPE_MAT3:
    case GLSL_TYPE_MAT4:
    case GLSL_TYPE_MAT2X3:
    case GLSL_TYPE_MAT2X4:
    case GLSL_TYPE_MAT3X2:
    case GLSL_TYPE_MAT3X4:
    case GLSL_TYPE_MAT4X2:
    case GLSL_TYPE_MAT4X3:
        return GL_TRUE;
    default:
        return GL_FALSE;
    }
}

static GLboolean is_matrix(glsl_type_t t) {
    return glsl_type_matrix_cols(t) > 0 ? GL_TRUE : GL_FALSE;
}

/* `matCxR`'s C and R. Both 0 for anything that is not a matrix, so a caller can ask
 * first. */
static int mat_cols(glsl_type_t t) {
    return glsl_type_matrix_cols(t);
}
static int mat_rows(glsl_type_t t) {
    return glsl_type_matrix_rows(t);
}

/* A bool is a float that is 0.0 or 1.0 in a VGPR. A comparison writes its lane mask to
 * `vcc` and selects 1.0 or 0.0 out of it, costing an instruction over carrying the mask
 * but keeping one register class. With values exactly 0.0 and 1.0, `a && b` is `min`,
 * `a || b` is `max` and `!a` is `1 - a`. */
static GLboolean is_bool_family(glsl_type_t t) {
    switch (t) {
    case GLSL_TYPE_BOOL:
    /* A `bvec` is the same per component. */
    case GLSL_TYPE_BVEC2:
    case GLSL_TYPE_BVEC3:
    case GLSL_TYPE_BVEC4:
        return GL_TRUE;
    default:
        return GL_FALSE;
    }
}

/* An `int` is a float kept whole, matching `glsl_exec.c`, which writes `(float)(int)x`
 * after an integer operation; this writes the same arithmetic and a `v_trunc_f32`.
 * GLSL 1.10 requires only 16 bits of integer and allows float storage. Both paths share
 * the 24-bit mantissa ceiling. */
static GLboolean is_int_family(glsl_type_t t) {
    switch (t) {
    case GLSL_TYPE_INT:
    case GLSL_TYPE_IVEC2:
    case GLSL_TYPE_IVEC3:
    case GLSL_TYPE_IVEC4:
        return GL_TRUE;
    default:
        return GL_FALSE;
    }
}

/* Everything this stage has a register for. */
static GLboolean is_generated(glsl_type_t t) {
    /* A struct is a run of registers too: its members end to end, in the layout the
     * semantic pass fixed. */
    if (glsl_type_is_struct(t))
        return GL_TRUE;
    return (is_float_family(t) || is_bool_family(t) || is_int_family(t)) ? GL_TRUE
                                                                         : GL_FALSE;
}

/* The size of anything, including a struct, whose size lives in the semantic table
 * rather than in the type alone. */
static int gen_comps(const glsl_gen_t *g, glsl_type_t t) {
    return glsl_type_components_of(g->sema, t);
}

/* The struct a name refers to in this unit, or GLSL_TYPE_ERROR. */
static glsl_type_t gen_struct_by_name(const glsl_gen_t *g, const char *name,
                                      size_t len) {
    if (!g->sema || !name)
        return GLSL_TYPE_ERROR;
    for (int i = 0; i < g->sema->struct_count; i++) {
        const glsl_struct_t *st = &g->sema->structs[i];
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

/* The type a declaration or parameter node writes - the generator's copy of sema's
 * `node_declared_type`. The three readings of a type token have to agree, which is why
 * all three ask the same table. */
static glsl_type_t gen_node_type(const glsl_gen_t *g, const glsl_node_t *n) {
    if (n->type_tok == GLSL_TOK_IDENTIFIER && n->type_name) {
        return gen_struct_by_name(g, n->type_name, n->type_name_len);
    }
    return glsl_type_from_token(n->type_tok);
}

/* The bits of a float literal, without punning through a pointer. Every constant in
 * this file is written as a decimal and converted here, so constants such as `1/2pi`,
 * `log2 e` and `ln 2` can be checked against a table. */
static uint32_t float_bits(double d) {
    union {
        float f;
        uint32_t u;
    } cvt;
    cvt.f = (float)d;
    return cvt.u;
}

/* Component `i` of a value, broadcasting a scalar. GLSL's mixed-width rules - `v
 * * 2.0`, `min(v, 0.0)`, `mix(a, b, t)` with a float `t` - are all this one rule. */
static uint32_t comp_of(glsl_value_t v, int i) {
    return v.base + (v.count == 1 ? 0u : (uint32_t)i);
}

/* -------------------------------------------------------------------------
 * Expressions
 * ------------------------------------------------------------------------- */

static glsl_value_t gen_expr(glsl_gen_t *g, int32_t node);

/* A float constant in a register of its own. */
static glsl_value_t gen_const(glsl_gen_t *g, double k, int32_t node) {
    glsl_value_t v = gen_alloc(g, 1, node);
    if (is_bad(v))
        return v;
    glsl_emit_mov_imm(g->code, v.base, float_bits(k));
    return v;
}

/* Copy `src` into `dst`, component for component. */
static void gen_move(glsl_gen_t *g, glsl_value_t dst, glsl_value_t src) {
    for (int i = 0; i < dst.count && i < src.count; i++) {
        glsl_emit_mov(g->code, dst.base + (uint32_t)i, src.base + (uint32_t)i);
    }
}

/* One component of `src`, broadcast or indexed, into one register of `dst`. */
static void gen_binop_component(glsl_gen_t *g, glsl_token_type_t op, uint32_t d,
                                uint32_t a, uint32_t b) {
    switch (op) {
    case GLSL_TOK_PLUS:
        glsl_emit_add_f32(g->code, d, a, b);
        break;
    case GLSL_TOK_MINUS:
        glsl_emit_sub_f32(g->code, d, a, b);
        break;
    case GLSL_TOK_STAR:
        glsl_emit_mul_f32(g->code, d, a, b);
        break;
    default:
        break; /* the caller has already refused anything else */
    }
}

/* Does this subtree write to anything? Decides whether `&&` and `||` can evaluate both
 * sides - see `gen_logical`. */
static GLboolean has_side_effect(const glsl_ast_t *ast, int32_t node) {
    if (node == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *n = &ast->nodes[node];
    if (n->kind == GLSL_NODE_ASSIGN || n->kind == GLSL_NODE_POSTFIX)
        return GL_TRUE;
    if (n->kind == GLSL_NODE_UNARY &&
        (n->op == GLSL_TOK_INC || n->op == GLSL_TOK_DEC)) {
        return GL_TRUE;
    }
    if (has_side_effect(ast, n->a) || has_side_effect(ast, n->b) ||
        has_side_effect(ast, n->c)) {
        return GL_TRUE;
    }
    /* A call's arguments are a sibling chain from `b`, not reachable through the three
     * links above. Only walked for a call: elsewhere a sibling is the next statement.
     */
    if (n->kind == GLSL_NODE_CALL) {
        for (int32_t s = n->b; s != GLSL_NO_NODE; s = ast->nodes[s].sibling) {
            if (has_side_effect(ast, s))
                return GL_TRUE;
        }
        /* A call to a function this shader defines counts as one, whatever its body
         * does: the body could write a global, and it is not analysed. */
        const glsl_node_t *callee =
            (n->a != GLSL_NO_NODE) ? &ast->nodes[n->a] : (const glsl_node_t *)0;
        if (callee && callee->kind == GLSL_NODE_IDENTIFIER) {
            for (int32_t i = 0; i < ast->count; i++) {
                const glsl_node_t *f = &ast->nodes[i];
                if (f->kind != GLSL_NODE_FUNCTION || f->c == GLSL_NO_NODE)
                    continue;
                if (f->length != callee->length)
                    continue;
                GLboolean same = GL_TRUE;
                for (size_t c = 0; c < f->length; c++) {
                    if (f->text[c] != callee->text[c]) {
                        same = GL_FALSE;
                        break;
                    }
                }
                if (same)
                    return GL_TRUE;
            }
        }
    }
    return GL_FALSE;
}

/* A comparison. The ordering operators take scalars; `==` and `!=` also take vectors
 * and give one bool. `==` is the `min` of the component equalities and `!=` the `max`
 * of the component inequalities: any differing component makes `!=` true. */
static glsl_value_t gen_compare(glsl_gen_t *g, glsl_token_type_t op, glsl_value_t a,
                                glsl_value_t b, int32_t node) {
    uint32_t vopc;
    switch (op) {
    case GLSL_TOK_LT:
        vopc = GLSL_VOPC_LT_F32;
        break;
    case GLSL_TOK_GT:
        vopc = GLSL_VOPC_GT_F32;
        break;
    case GLSL_TOK_LE:
        vopc = GLSL_VOPC_LE_F32;
        break;
    case GLSL_TOK_GE:
        vopc = GLSL_VOPC_GE_F32;
        break;
    case GLSL_TOK_EQ:
        vopc = GLSL_VOPC_EQ_F32;
        break;
    case GLSL_TOK_NE:
        vopc = GLSL_VOPC_NEQ_F32;
        break;
    default:
        return gen_fail(g, "not a comparison", node);
    }
    const int w = a.count > b.count ? a.count : b.count;
    if (w > 1 && op != GLSL_TOK_EQ && op != GLSL_TOK_NE) {
        return gen_fail(
            g,
            "the ordering comparisons take scalars; lessThan and its family "
            "compare vectors, and they return a bvec this has no register for",
            node);
    }

    glsl_value_t zero = gen_const(g, 0.0, node);
    if (is_bad(zero))
        return zero;
    glsl_value_t one = gen_const(g, 1.0, node);
    if (is_bad(one))
        return one;
    glsl_value_t d = gen_alloc(g, 1, node);
    if (is_bad(d))
        return d;
    glsl_value_t t = gen_alloc(g, 1, node);
    if (is_bad(t))
        return t;

    for (int i = 0; i < w; i++) {
        glsl_emit_cmp(g->code, vopc, comp_of(a, i), comp_of(b, i));
        glsl_emit_cndmask(g->code, (i == 0 ? d.base : t.base), zero.base, one.base);
        if (i > 0) {
            /* `==` needs every component true, `!=` needs any. */
            glsl_emit_vop2_op(g->code,
                              op == GLSL_TOK_EQ ? GLSL_VOP2_MIN_F32 : GLSL_VOP2_MAX_F32,
                              d.base, d.base, t.base);
        }
    }
    return d;
}

/* `&&`, `||` and `^^` over values that are exactly 0.0 or 1.0: `min`, `max`, and the
 * absolute difference.
 *
 * With a pure right operand both sides are computed and combined, with no mask. When
 * the right side has a side effect, `&&` and `||` short-circuit through `exec`: the
 * right side runs only in lanes the left has not decided, and the result starts as the
 * left operand and is overwritten under that mask. `^^` never short-circuits in GLSL.
 */
static glsl_value_t gen_logical(glsl_gen_t *g, int32_t node) {
    const glsl_node_t *n = &g->ast->nodes[node];
    const GLboolean shortcut =
        (GLboolean)(n->op == GLSL_TOK_AND_AND || n->op == GLSL_TOK_OR_OR);

    if (shortcut && has_side_effect(g->ast, n->b)) {
        if (g->exec_depth >= GLSL_GEN_MAX_EXEC_DEPTH) {
            return gen_fail(
                g,
                "the conditionals in this shader nest deeper than the scalar "
                "registers set aside for them",
                node);
        }
        const uint32_t saved = GLSL_GEN_EXEC_SGPR_BASE + (uint32_t)g->exec_depth;
        glsl_value_t a = gen_expr(g, n->a);
        if (is_bad(a))
            return a;
        if (a.count != 1)
            return gen_fail(g, "&& and || take single bools", node);
        glsl_value_t d = gen_alloc(g, 1, node);
        if (is_bad(d))
            return d;
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return zero;
        /* The answer if the right side never runs - the left operand itself. */
        glsl_emit_mov(g->code, d.base, a.base);
        /* `&&` carries on where the left is true, `||` where it is false. */
        glsl_emit_cmp(
            g->code, (n->op == GLSL_TOK_AND_AND) ? GLSL_VOPC_NEQ_F32 : GLSL_VOPC_EQ_F32,
            a.base, zero.base);
        glsl_emit_exec_save_and_vcc(g->code, saved);
        g->exec_depth++;
        glsl_value_t b = gen_expr(g, n->b);
        g->exec_depth--;
        if (is_bad(b))
            return b;
        if (b.count != 1) {
            (void)gen_fail(g, "&& and || take single bools", node);
            return b;
        }
        /* Under the mask, so only the lanes that ran the right side take its answer. */
        glsl_emit_mov(g->code, d.base, b.base);
        glsl_emit_exec_restore(g->code, saved);
        return d;
    }

    glsl_value_t a = gen_expr(g, n->a);
    if (is_bad(a))
        return a;
    glsl_value_t b = gen_expr(g, n->b);
    if (is_bad(b))
        return b;
    if (a.count != 1 || b.count != 1) {
        return gen_fail(g, "&& and || take single bools", node);
    }
    glsl_value_t d = gen_alloc(g, 1, node);
    if (is_bad(d))
        return d;
    if (n->op == GLSL_TOK_AND_AND) {
        glsl_emit_vop2_op(g->code, GLSL_VOP2_MIN_F32, d.base, a.base, b.base);
        return d;
    }
    if (n->op == GLSL_TOK_OR_OR) {
        glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, d.base, a.base, b.base);
        return d;
    }
    /* `^^`, the exclusive or: |a - b| over two values that are 0 or 1. */
    glsl_value_t t = gen_alloc(g, 1, node);
    if (is_bad(t))
        return t;
    glsl_emit_sub_f32(g->code, d.base, a.base, b.base);
    glsl_emit_neg_f32(g->code, t.base, d.base);
    glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, d.base, d.base, t.base);
    return d;
}

/* A binary operator. `+ - *` are component for component, broadcasting a scalar;
 * matrix products are transforms, handled before the widths are compared. `/` is
 * `v_rcp_f32` then `v_mul_f32`, as ACO emits a divide not marked `precise`
 * (mesa/src/amd/compiler/aco_instruction_selection.cpp, `nir_op_fdiv`): within about 2
 * ULP, which GLSL 1.10 4.5.1 allows, while `glsl_exec.c` divides exactly. */
static glsl_value_t gen_binary(glsl_gen_t *g, int32_t node) {
    const glsl_node_t *n = &g->ast->nodes[node];
    const glsl_token_type_t op = n->op;

    if (op == GLSL_TOK_AND_AND || op == GLSL_TOK_OR_OR || op == GLSL_TOK_XOR_XOR) {
        return gen_logical(g, node);
    }

    if (op == GLSL_TOK_LT || op == GLSL_TOK_GT || op == GLSL_TOK_LE ||
        op == GLSL_TOK_GE || op == GLSL_TOK_EQ || op == GLSL_TOK_NE) {
        /* An integer comparison is the float comparison, because an int here is a
         * float kept whole; `==` is exact for whole numbers up to 2^24.
         *
         * Matrices and structs compare for equality but not order (GLSL 1.10 5.9). Both
         * are runs of registers, and two values of one type have the same layout, so
         * `gen_compare`'s element-wise reduction applies. */
        const glsl_type_t clt = glsl_type_of(g->sema, n->a);
        const glsl_type_t crt = glsl_type_of(g->sema, n->b);
        const GLboolean equality = (GLboolean)(op == GLSL_TOK_EQ || op == GLSL_TOK_NE);
        const GLboolean aggregate =
            (GLboolean)(is_matrix(clt) || is_matrix(crt) || glsl_type_is_struct(clt) ||
                        glsl_type_is_struct(crt));
        if ((!is_generated(clt) || !is_generated(crt)) || (aggregate && !equality)) {
            return gen_fail(
                g,
                "only float, int, vector and bool comparisons are generated, and "
                "a matrix or a struct compares for equality but not for order",
                node);
        }
        glsl_value_t ca = gen_expr(g, n->a);
        if (is_bad(ca))
            return ca;
        glsl_value_t cb = gen_expr(g, n->b);
        if (is_bad(cb))
            return cb;
        return gen_compare(g, op, ca, cb, node);
    }

    if (op != GLSL_TOK_PLUS && op != GLSL_TOK_MINUS && op != GLSL_TOK_STAR &&
        op != GLSL_TOK_SLASH) {
        return gen_fail(
            g,
            "this operator has no verified instruction yet: the arithmetic, the "
            "comparisons and the logical operators are generated, and the rest "
            "are refused rather than approximated",
            node);
    }

    const glsl_type_t lt = glsl_type_of(g->sema, n->a);
    const glsl_type_t rt = glsl_type_of(g->sema, n->b);
    const GLboolean int_result = (GLboolean)(is_int_family(lt) && is_int_family(rt));
    if ((!is_float_family(lt) && !is_int_family(lt)) ||
        (!is_float_family(rt) && !is_int_family(rt))) {
        return gen_fail(
            g,
            "only float, vec, mat and int arithmetic is generated; bool has no "
            "verified instruction here",
            node);
    }
    glsl_value_t a = gen_expr(g, n->a);
    if (is_bad(a))
        return a;
    glsl_value_t b = gen_expr(g, n->b);
    if (is_bad(b))
        return b;

    /* `m * v` and `v * m` are separate products: `v * m` is a dot with each column.
     * `matCxR * vec` consumes a `vecC` and produces a `vecR`; `vec * matCxR` consumes a
     * `vecR` and produces a `vecC`. */
    if (op == GLSL_TOK_STAR && is_matrix(lt) && !is_matrix(rt)) {
        const int cols = mat_cols(lt), rows = mat_rows(lt);
        if (b.count == cols) {
            glsl_value_t mv = gen_alloc(g, rows, node);
            if (is_bad(mv))
                return mv;
            glsl_emit_mat_mul_vec_cr(g->code, mv.base, a.base, b.base, (uint32_t)cols,
                                     (uint32_t)rows);
            return mv;
        }
    }
    if (op == GLSL_TOK_STAR && !is_matrix(lt) && is_matrix(rt)) {
        const int cols = mat_cols(rt), rows = mat_rows(rt);
        if (a.count == rows) {
            glsl_value_t vm = gen_alloc(g, cols, node);
            if (is_bad(vm))
                return vm;
            glsl_emit_vec_mul_mat_cr(g->code, vm.base, a.base, b.base, (uint32_t)cols,
                                     (uint32_t)rows);
            return vm;
        }
    }
    /* `m * m`: column `c` of the result is `A` times column `c` of `B`, as
     * `glsl_exec.c` computes it, and a column of `B` is already a run of registers. The
     * shape rule is `matCxR * matPxC -> matPxR`. The destination is freshly allocated
     * because the emitter writes a result's first component before reading its input's
     * last. */
    if (op == GLSL_TOK_STAR && is_matrix(lt) && is_matrix(rt)) {
        const int a_cols = mat_cols(lt), a_rows = mat_rows(lt);
        const int b_cols = mat_cols(rt), b_rows = mat_rows(rt);
        if (b_rows != a_cols) {
            return gen_fail(
                g,
                "the right matrix in a product needs as many rows as the left one "
                "has columns",
                node);
        }
        glsl_value_t mm = gen_alloc(g, b_cols * a_rows, node);
        if (is_bad(mm))
            return mm;
        for (int c = 0; c < b_cols; c++) {
            glsl_emit_mat_mul_vec_cr(g->code, mm.base + (uint32_t)(c * a_rows), a.base,
                                     b.base + (uint32_t)(c * b_rows), (uint32_t)a_cols,
                                     (uint32_t)a_rows);
        }
        return mm;
    }
    /* A matrix with a vector that missed the products above is the wrong size or an
     * operator GLSL does not define between them; it is refused rather than treated as
     * componentwise. A scalar is broadcast over every element below. */
    if (is_matrix(lt) != is_matrix(rt)) {
        const int other = is_matrix(lt) ? b.count : a.count;
        if (other != 1) {
            return gen_fail(
                g,
                "a matrix combines with a matrix of the same size, with a vector "
                "of its own width under `*`, or with a scalar - and this is none "
                "of those",
                node);
        }
    }

    const int width = a.count > b.count ? a.count : b.count;
    if (a.count != b.count && a.count != 1 && b.count != 1) {
        return gen_fail(g, "these operand widths do not combine", node);
    }

    /* Integer division, with the quotient corrected. `a * rcp(b)` can land a hair below
     * a whole quotient, which truncation turns into one less (`7 / 7` would be 0). The
     * error is at most one, so the truncated answer is multiplied back by the divisor
     * and compared with the dividend once in each direction. Done on magnitudes with
     * the sign applied at the end, since GLSL truncates toward zero. Division by zero
     * answers zero, as `glsl_exec.c` does. */
    if (int_result && op == GLSL_TOK_SLASH) {
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return zero;
        glsl_value_t one = gen_const(g, 1.0, node);
        if (is_bad(one))
            return one;
        glsl_value_t out = gen_alloc(g, width, node);
        if (is_bad(out))
            return out;
        for (int i = 0; i < width; i++) {
            const uint32_t mark = gen_mark(g);
            glsl_value_t t = gen_alloc(g, 6, node);
            if (is_bad(t))
                return t;
            const uint32_t neg = t.base + 0u, absa = t.base + 1u, absb = t.base + 2u;
            const uint32_t q = t.base + 3u, tmp = t.base + 4u, alt = t.base + 5u;
            const uint32_t A = comp_of(a, i), B = comp_of(b, i);

            glsl_emit_neg_f32(g->code, neg, A);
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, absa, A, neg);
            glsl_emit_neg_f32(g->code, neg, B);
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, absb, B, neg);

            glsl_emit_vop1_op(g->code, GLSL_VOP1_RCP_F32, tmp, absb);
            glsl_emit_mul_f32(g->code, q, absa, tmp);
            glsl_emit_vop1_op(g->code, GLSL_VOP1_TRUNC_F32, q, q);

            /* One too small: the next quotient up still fits inside the dividend. */
            glsl_emit_add_f32(g->code, alt, q, one.base);
            glsl_emit_mul_f32(g->code, tmp, alt, absb);
            glsl_emit_cmp(g->code, GLSL_VOPC_LE_F32, tmp, absa);
            glsl_emit_cndmask(g->code, q, q, alt);

            /* One too large: this quotient overshoots it. */
            glsl_emit_mul_f32(g->code, tmp, q, absb);
            glsl_emit_sub_f32(g->code, alt, q, one.base);
            glsl_emit_cmp(g->code, GLSL_VOPC_GT_F32, tmp, absa);
            glsl_emit_cndmask(g->code, q, q, alt);

            /* The quotient's sign is the sign of the operands' product. */
            glsl_emit_mul_f32(g->code, tmp, A, B);
            glsl_emit_neg_f32(g->code, alt, q);
            glsl_emit_cmp(g->code, GLSL_VOPC_LT_F32, tmp, zero.base);
            glsl_emit_cndmask(g->code, out.base + (uint32_t)i, q, alt);

            glsl_emit_cmp(g->code, GLSL_VOPC_EQ_F32, B, zero.base);
            glsl_emit_cndmask(g->code, out.base + (uint32_t)i, out.base + (uint32_t)i,
                              zero.base);
            gen_release(g, mark);
        }
        return out;
    }

    if (op == GLSL_TOK_SLASH) {
        /* One reciprocal per divisor component: `v / s` is one reciprocal and `width`
         * multiplies. */
        glsl_value_t r = gen_alloc(g, b.count, node);
        if (is_bad(r))
            return r;
        for (int i = 0; i < b.count; i++) {
            glsl_emit_vop1_op(g->code, GLSL_VOP1_RCP_F32, r.base + (uint32_t)i,
                              b.base + (uint32_t)i);
        }
        glsl_value_t out = gen_alloc(g, width, node);
        if (is_bad(out))
            return out;
        for (int i = 0; i < width; i++) {
            glsl_emit_mul_f32(g->code, out.base + (uint32_t)i, comp_of(a, i),
                              comp_of(r, i));
        }
        return out;
    }

    glsl_value_t out = gen_alloc(g, width, node);
    if (is_bad(out))
        return out;
    for (int i = 0; i < width; i++) {
        gen_binop_component(g, op, out.base + (uint32_t)i, comp_of(a, i),
                            comp_of(b, i));
    }
    /* An integer result is kept whole, as the reference's `(float)(int)x` does. Sums
     * and products of whole floats are already whole; the truncation states the
     * invariant rather than relying on it. `v_trunc_f32` rounds toward zero, as C and
     * GLSL do. */
    if (int_result) {
        for (int i = 0; i < width; i++) {
            glsl_emit_vop1_op(g->code, GLSL_VOP1_TRUNC_F32, out.base + (uint32_t)i,
                              out.base + (uint32_t)i);
        }
    }
    return out;
}

/* A swizzle read: `v.xyz`, `c.rgba`, `t.st`. The vocabularies were checked by the
 * semantic stage - mixing them, and naming a component past the end, are both already
 * refused - so this only has to map a letter to an index. A repeated component (`v.xx`)
 * is a legal read and becomes two moves from the same register. */
static glsl_value_t gen_field(glsl_gen_t *g, int32_t node) {
    const glsl_node_t *n = &g->ast->nodes[node];
    glsl_value_t src = gen_expr(g, n->a);
    if (is_bad(src))
        return src;

    /* Reading a struct member is a slice of the run and needs no copy. The swizzle path
     * below moves, because its components need not be consecutive or in order. */
    {
        const glsl_type_t bt = glsl_type_of(g->sema, n->a);
        const glsl_struct_member_t *mem =
            glsl_struct_member(g->sema, bt, n->text, n->length);
        if (mem) {
            glsl_value_t out;
            out.base = src.base + (uint32_t)mem->offset;
            out.count =
                gen_comps(g, mem->type) * ((mem->array_size > 0) ? mem->array_size : 1);
            return out;
        }
        if (glsl_type_is_struct(bt)) {
            return gen_fail(g, "this struct has no member of that name", node);
        }
    }

    const int len = (int)n->length;
    if (len < 1 || len > 4)
        return gen_fail(g, "a swizzle names one to four components", node);

    glsl_value_t out = gen_alloc(g, len, node);
    if (is_bad(out))
        return out;

    for (int i = 0; i < len; i++) {
        int idx;
        switch (n->text[i]) {
        case 'x':
        case 'r':
        case 's':
            idx = 0;
            break;
        case 'y':
        case 'g':
        case 't':
            idx = 1;
            break;
        case 'z':
        case 'b':
        case 'p':
            idx = 2;
            break;
        case 'w':
        case 'a':
        case 'q':
            idx = 3;
            break;
        default:
            return gen_fail(g, "not a component name", node);
        }
        if (idx >= src.count)
            return gen_fail(g, "a component past the end of the value", node);
        glsl_emit_mov(g->code, out.base + (uint32_t)i, src.base + (uint32_t)idx);
    }
    return out;
}

/* The type a name in call position constructs, or GLSL_TYPE_ERROR. */
static glsl_type_t constructor_target(const glsl_node_t *callee) {
    static const struct {
        const char *name;
        glsl_type_t type;
    } ctors[] = {
        {"float", GLSL_TYPE_FLOAT},
        {"vec2", GLSL_TYPE_VEC2},
        {"vec3", GLSL_TYPE_VEC3},
        {"vec4", GLSL_TYPE_VEC4},
        {"mat2", GLSL_TYPE_MAT2},
        {"mat3", GLSL_TYPE_MAT3},
        {"mat4", GLSL_TYPE_MAT4},
        /* `matNxN` is a spelling of `matN`, not a type of its own, so it constructs the
         * same thing. The six genuinely non-square shapes follow. */
        {"mat2x2", GLSL_TYPE_MAT2},
        {"mat3x3", GLSL_TYPE_MAT3},
        {"mat4x4", GLSL_TYPE_MAT4},
        {"mat2x3", GLSL_TYPE_MAT2X3},
        {"mat2x4", GLSL_TYPE_MAT2X4},
        {"mat3x2", GLSL_TYPE_MAT3X2},
        {"mat3x4", GLSL_TYPE_MAT3X4},
        {"mat4x2", GLSL_TYPE_MAT4X2},
        {"mat4x3", GLSL_TYPE_MAT4X3},
        {"bool", GLSL_TYPE_BOOL},
        {"int", GLSL_TYPE_INT},
        /* The integer and boolean vectors are the same run of registers a `vecN` is. */
        {"ivec2", GLSL_TYPE_IVEC2},
        {"ivec3", GLSL_TYPE_IVEC3},
        {"ivec4", GLSL_TYPE_IVEC4},
        {"bvec2", GLSL_TYPE_BVEC2},
        {"bvec3", GLSL_TYPE_BVEC3},
        {"bvec4", GLSL_TYPE_BVEC4},
    };
    for (size_t i = 0; i < sizeof(ctors) / sizeof(ctors[0]); i++) {
        size_t len = 0;
        while (ctors[i].name[len] != '\0')
            len++;
        if (len != callee->length)
            continue;
        GLboolean match = GL_TRUE;
        for (size_t k = 0; k < len; k++) {
            if (callee->text[k] != ctors[i].name[k]) {
                match = GL_FALSE;
                break;
            }
        }
        if (match)
            return ctors[i].type;
    }
    return GLSL_TYPE_ERROR;
}

/* A constructor. GLSL tells the forms apart by component count: `vec4(1.0)` fills every
 * component, `mat4(1.0)` fills the diagonal and zeroes the rest, and `vec4(v3, 1.0)`
 * takes components in order until the target is full. */
static glsl_value_t gen_construct(glsl_gen_t *g, glsl_type_t target, int32_t first_arg,
                                  int32_t node) {
    /* The two scalar conversions are not component copies: `bool(x)` is `x != 0` and
     * `int(x)` truncates toward zero. */
    if (target == GLSL_TYPE_BOOL || target == GLSL_TYPE_INT) {
        if (first_arg == GLSL_NO_NODE ||
            g->ast->nodes[first_arg].sibling != GLSL_NO_NODE) {
            return gen_fail(g, "bool() and int() take one argument", node);
        }
        glsl_value_t s = gen_expr(g, first_arg);
        if (is_bad(s))
            return s;
        if (s.count != 1)
            return gen_fail(g, "bool() and int() take a scalar", node);
        glsl_value_t out = gen_alloc(g, 1, node);
        if (is_bad(out))
            return out;
        if (target == GLSL_TYPE_INT) {
            glsl_emit_vop1_op(g->code, GLSL_VOP1_TRUNC_F32, out.base, s.base);
            return out;
        }
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return zero;
        glsl_value_t one = gen_const(g, 1.0, node);
        if (is_bad(one))
            return one;
        glsl_emit_cmp(g->code, GLSL_VOPC_NEQ_F32, s.base, zero.base);
        glsl_emit_cndmask(g->code, out.base, zero.base, one.base);
        return out;
    }

    const int needed = glsl_type_components(target);
    glsl_value_t out = gen_alloc(g, needed, node);
    if (is_bad(out))
        return out;

    /* Count the arguments and their components, to tell the one-scalar forms apart. */
    int args = 0, supplied = 0;
    for (int32_t a = first_arg; a != GLSL_NO_NODE; a = g->ast->nodes[a].sibling) {
        const glsl_type_t at = glsl_type_of(g->sema, a);
        /* An `int` or `bool` argument needs no conversion: an int is already a whole
         * float, and a bool is already 0.0 or 1.0, which is GLSL 1.10 5.4.1's
         * `float(b)`. */
        if (!is_float_family(at) && !is_int_family(at) && !is_bool_family(at)) {
            return gen_fail(
                g,
                "only float, vec, mat, int and bool constructor arguments are "
                "generated",
                node);
        }
        supplied += glsl_type_components(at);
        args++;
    }

    if (args == 1 && supplied == 1) {
        glsl_value_t s = gen_expr(g, first_arg);
        if (is_bad(s))
            return s;
        if (is_matrix(target)) {
            /* The diagonal takes the scalar, everything else is zero. Column-major, so
             * element `(col, row)` is at `col * rows + row`; the diagonal runs out at
             * the shorter side, so a `mat2x4` gets two diagonal entries. */
            const int cols = mat_cols(target), rows = mat_rows(target);
            for (int col = 0; col < cols; col++) {
                for (int row = 0; row < rows; row++) {
                    const uint32_t at = out.base + (uint32_t)(col * rows + row);
                    if (col == row) {
                        glsl_emit_mov(g->code, at, s.base);
                    } else {
                        glsl_emit_mov_imm(g->code, at, 0u);
                    }
                }
            }
        } else {
            for (int i = 0; i < needed; i++) {
                glsl_emit_mov(g->code, out.base + (uint32_t)i, s.base);
            }
        }
        return out;
    }

    int filled = 0;
    for (int32_t a = first_arg; a != GLSL_NO_NODE && filled < needed;
         a = g->ast->nodes[a].sibling) {
        glsl_value_t v = gen_expr(g, a);
        if (is_bad(v))
            return v;
        for (int i = 0; i < v.count && filled < needed; i++, filled++) {
            glsl_emit_mov(g->code, out.base + (uint32_t)filled, v.base + (uint32_t)i);
        }
    }
    if (filled < needed)
        return gen_fail(g, "too few components for this constructor", node);
    return out;
}

/* -------------------------------------------------------------------------
 * Places: what an assignment writes into
 *
 * A value is one VGPR a component, so writing through a swizzle is a move into each
 * register the swizzle names, not a masked move: `c.zyx = v` is three moves that cross
 * over. The semantic stage has already refused everything that is not a place (a
 * repeated component, a uniform, an attribute, a `const`).
 * ------------------------------------------------------------------------- */

/* A place can name a whole struct or a member of any type, so its width is the struct
 * ceiling the semantic pass enforces. */
#define GLSL_GEN_MAX_PLACE_REGS GLSL_MAX_STRUCT_COMPONENTS

typedef struct {
    uint32_t
        reg[GLSL_GEN_MAX_PLACE_REGS]; /* the registers this place names, in order */
    int count;
} gen_place_t;

/* `a[k]` resolved to the element's registers, for reading and assigning alike. The
 * index must be constant: an array is a run of registers, and the alternatives (a
 * select over every element, or scratch memory) are not used. An unrolled loop's
 * counter is constant, so `for (int i = 0; i < 4; i++) sum += w[i];` resolves. */
static GLboolean gen_index_of(glsl_gen_t *g, int32_t node, glsl_value_t *out) {
    const glsl_node_t *n = &g->ast->nodes[node];

    double idx = 0.0;
    if (!const_of(g, n->b, &idx)) {
        (void)gen_fail(
            g,
            "an index has to be known when the shader is compiled: an array is a "
            "run of registers here, and a register file cannot be indexed by a "
            "value that is only known while it runs. A loop that unrolls counts "
            "as known",
            node);
        return GL_FALSE;
    }
    const int k = (int)idx;
    if ((double)k != idx || k < 0) {
        (void)gen_fail(g, "an index has to be a whole number that is not negative",
                       node);
        return GL_FALSE;
    }

    /* An array name first: it has no value of its own as an expression, so a whole
     * array cannot be used where a vector is meant. Its elements are the run behind the
     * name. */
    const glsl_node_t *base = &g->ast->nodes[n->a];
    if (base->kind == GLSL_NODE_IDENTIFIER) {
        glsl_gen_var_t *v = gen_find(g, base->text, base->length);
        if (v && v->array_size > 0) {
            if (k >= v->array_size) {
                (void)gen_fail(g, "this array index is outside the array", node);
                return GL_FALSE;
            }
            out->base = v->value.base + (uint32_t)(k * v->value.count);
            out->count = v->value.count;
            return GL_TRUE;
        }
    }

    /* A struct member that is an array. Recognised here because below the stride comes
     * from the type, and a member's type is its element type: a `vec2 p[3]` would look
     * like a six-component value. The element width is the stride and the length the
     * bound. */
    if (base->kind == GLSL_NODE_FIELD) {
        const glsl_type_t owner = glsl_type_of(g->sema, base->a);
        const glsl_struct_member_t *mem =
            glsl_struct_member(g->sema, owner, base->text, base->length);
        if (mem && mem->array_size > 0) {
            glsl_value_t run = gen_expr(g, n->a);
            if (is_bad(run))
                return GL_FALSE;
            if (k >= mem->array_size) {
                (void)gen_fail(g, "this array index is outside the array", node);
                return GL_FALSE;
            }
            const int w = gen_comps(g, mem->type);
            out->base = run.base + (uint32_t)(k * w);
            out->count = w;
            return GL_TRUE;
        }
    }

    /* Otherwise a matrix's column or a vector's component, the same arithmetic with a
     * different stride; `m[c][r]` is the two composed. The base is evaluated, so a
     * computed matrix can be indexed, and a name hands back its own registers, so
     * `m[1].x = ...` is a place. */
    const glsl_type_t bt = glsl_type_of(g->sema, n->a);
    glsl_value_t bv = gen_expr(g, n->a);
    if (is_bad(bv))
        return GL_FALSE;
    /* A `matCxR` has C columns of R registers each: the stride is the row count and
     * the bound is the column count. */
    const int cols = mat_cols(bt), rows = mat_rows(bt);
    const int width = (cols > 0) ? rows : 1;
    const int count = (cols > 0) ? cols : bv.count;
    if (count < 2) {
        (void)gen_fail(
            g,
            "this is not something `[]` indexes: an array, a matrix and a vector "
            "are",
            node);
        return GL_FALSE;
    }
    if (k >= count) {
        (void)gen_fail(g, "this index is outside what it indexes", node);
        return GL_FALSE;
    }
    out->base = bv.base + (uint32_t)(k * width);
    out->count = width;
    return GL_TRUE;
}

static glsl_value_t gen_inc_dec(glsl_gen_t *g, int32_t node, GLboolean postfix);

static GLboolean gen_place_of(glsl_gen_t *g, int32_t node, gen_place_t *out) {
    if (node == GLSL_NO_NODE) {
        (void)gen_fail(g, "an assignment with no destination", node);
        return GL_FALSE;
    }
    const glsl_node_t *n = &g->ast->nodes[node];
    if (n->kind == GLSL_NODE_IDENTIFIER) {
        glsl_gen_var_t *v = gen_find(g, n->text, n->length);
        if (!v) {
            (void)gen_fail(
                g,
                "assigning to a name with no register: only locals declared in "
                "this body and the shader's own inputs are generated so far",
                node);
            return GL_FALSE;
        }
        /* A struct is assignable whole and a matrix is not. */
        if (glsl_type_is_matrix(v->type)) {
            (void)gen_fail(g, "a matrix is not assignable here", node);
            return GL_FALSE;
        }
        /* A whole array is a place under GLSL 1.20: the whole run. The semantic stage
         * has checked the other side's element type and length, and refuses array
         * assignment in a 1.10 shader (5.8). */
        const int width = v->value.count * ((v->array_size > 0) ? v->array_size : 1);
        if (width > GLSL_GEN_MAX_PLACE_REGS) {
            (void)gen_fail(g, "this value is wider than a place can name", node);
            return GL_FALSE;
        }
        out->count = width;
        for (int i = 0; i < out->count; i++)
            out->reg[i] = v->value.base + (uint32_t)i;
        return GL_TRUE;
    }
    /* `a[k] = …`, and `a[k].xy = …` through the swizzle arm below, which recurses into
     * this. */
    if (n->kind == GLSL_NODE_INDEX) {
        glsl_value_t elem;
        if (!gen_index_of(g, node, &elem))
            return GL_FALSE;
        if (elem.count > 4) {
            (void)gen_fail(g, "a matrix is not assignable here", node);
            return GL_FALSE;
        }
        out->count = elem.count;
        for (int i = 0; i < out->count; i++)
            out->reg[i] = elem.base + (uint32_t)i;
        return GL_TRUE;
    }
    if (n->kind == GLSL_NODE_FIELD) {
        gen_place_t base;
        if (!gen_place_of(g, n->a, &base))
            return GL_FALSE;
        /* A struct member is a run of consecutive registers at the base's first
         * register plus its offset, and assignable at any width. */
        {
            const glsl_type_t bt = glsl_type_of(g->sema, n->a);
            const glsl_struct_member_t *mem =
                glsl_struct_member(g->sema, bt, n->text, n->length);
            if (mem) {
                const int w = gen_comps(g, mem->type) *
                              ((mem->array_size > 0) ? mem->array_size : 1);
                if (w > GLSL_GEN_MAX_PLACE_REGS) {
                    (void)gen_fail(
                        g, "this struct member is wider than a place can name", node);
                    return GL_FALSE;
                }
                out->count = w;
                for (int i = 0; i < w; i++) {
                    out->reg[i] = base.reg[0] + (uint32_t)(mem->offset + i);
                }
                return GL_TRUE;
            }
            if (glsl_type_is_struct(bt)) {
                (void)gen_fail(g, "this struct has no member of that name", node);
                return GL_FALSE;
            }
        }
        const int len = (int)n->length;
        if (len < 1 || len > 4) {
            (void)gen_fail(g, "a swizzle names one to four components", node);
            return GL_FALSE;
        }
        out->count = len;
        for (int i = 0; i < len; i++) {
            int idx;
            switch (n->text[i]) {
            case 'x':
            case 'r':
            case 's':
                idx = 0;
                break;
            case 'y':
            case 'g':
            case 't':
                idx = 1;
                break;
            case 'z':
            case 'b':
            case 'p':
                idx = 2;
                break;
            case 'w':
            case 'a':
            case 'q':
                idx = 3;
                break;
            default:
                (void)gen_fail(g, "not a component name", node);
                return GL_FALSE;
            }
            if (idx >= base.count) {
                (void)gen_fail(g, "a component past the end of the value", node);
                return GL_FALSE;
            }
            out->reg[i] = base.reg[idx];
        }
        return GL_TRUE;
    }
    (void)gen_fail(g, "this is not something with a register to write into", node);
    return GL_FALSE;
}

/* -------------------------------------------------------------------------
 * The built-in library
 *
 * GLSL section 8, lowered onto the instructions in `tools/shader/gl2-fragment.s`: one
 * VOP1 or VOP2 per component where one exists, otherwise a short sequence with the
 * identity it uses written beside it, split into one family function each.
 *
 * `v_sin_f32` computes `sin(2*pi*x)`, so GLSL's `sin` multiplies by `1/2pi` first, as
 * ACO does (mesa/src/amd/compiler/aco_instruction_selection.cpp, `nir_op_fsin`,
 * 0x3e22f983). `v_exp_f32` and `v_log_f32` are base two: `exp` is `exp2(x * log2 e)`
 * and `log` is `log2(x) * ln 2`.
 * ------------------------------------------------------------------------- */

static GLboolean nm_is(const char *text, size_t len, const char *lit) {
    size_t n = 0;
    while (lit[n] != '\0')
        n++;
    if (n != len)
        return GL_FALSE;
    for (size_t i = 0; i < n; i++) {
        if (text[i] != lit[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

/* `text` is not NUL-terminated - it points into the shader source - so the length is
 * checked before any character is, and never after. */
static GLboolean nm_prefix(const char *text, size_t len, const char *lit) {
    size_t n = 0;
    while (lit[n] != '\0')
        n++;
    if (len < n)
        return GL_FALSE;
    for (size_t i = 0; i < n; i++) {
        if (text[i] != lit[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

/* `d[i] = op(s[i])`, one VOP1 a component. */
static void gen_map1(glsl_gen_t *g, uint32_t opcode, glsl_value_t d, glsl_value_t s) {
    for (int i = 0; i < d.count; i++) {
        glsl_emit_vop1_op(g->code, opcode, d.base + (uint32_t)i, comp_of(s, i));
    }
}

/* `d[i] = op(a[i], b[i])`, one VOP2 a component, either operand broadcastable. */
static void gen_map2(glsl_gen_t *g, uint32_t opcode, glsl_value_t d, glsl_value_t a,
                     glsl_value_t b) {
    for (int i = 0; i < d.count; i++) {
        glsl_emit_vop2_op(g->code, opcode, d.base + (uint32_t)i, comp_of(a, i),
                          comp_of(b, i));
    }
}

/* `dot(a, b)` into one register: a multiply, then a fused multiply-add a component. The
 * destination is freshly allocated, so it overlaps neither operand. */
static glsl_value_t gen_dot(glsl_gen_t *g, glsl_value_t a, glsl_value_t b,
                            int32_t node) {
    const int w = a.count > b.count ? a.count : b.count;
    glsl_value_t d = gen_alloc(g, 1, node);
    if (is_bad(d))
        return d;
    glsl_emit_mul_f32(g->code, d.base, comp_of(a, 0), comp_of(b, 0));
    for (int i = 1; i < w; i++) {
        glsl_emit_fmac_f32(g->code, d.base, comp_of(a, i), comp_of(b, i));
    }
    return d;
}

/* `1 / sqrt(dot(v, v))`, the scale `normalize` and `length` are both built on. */
static glsl_value_t gen_inv_length(glsl_gen_t *g, glsl_value_t v, int32_t node) {
    glsl_value_t d2 = gen_dot(g, v, v, node);
    if (is_bad(d2))
        return d2;
    glsl_value_t r = gen_alloc(g, 1, node);
    if (is_bad(r))
        return r;
    glsl_emit_vop1_op(g->code, GLSL_VOP1_RSQ_F32, r.base, d2.base);
    return r;
}

/* `d = (x cmp 0) ? one : zero`, component-wise, with the two constants materialised
 * once. `v_cndmask` takes its false value in src0. */
static glsl_value_t gen_select_on_sign(glsl_gen_t *g, glsl_value_t x, uint32_t vopc,
                                       double if_true, double if_false, int32_t node) {
    glsl_value_t z = gen_const(g, 0.0, node);
    if (is_bad(z))
        return z;
    glsl_value_t t = gen_const(g, if_true, node);
    if (is_bad(t))
        return t;
    glsl_value_t f = gen_const(g, if_false, node);
    if (is_bad(f))
        return f;
    glsl_value_t d = gen_alloc(g, x.count, node);
    if (is_bad(d))
        return d;
    for (int i = 0; i < x.count; i++) {
        glsl_emit_cmp(g->code, vopc, comp_of(x, i), z.base);
        glsl_emit_cndmask(g->code, d.base + (uint32_t)i, f.base, t.base);
    }
    return d;
}

#define GEN_MAX_ARGS 4

/* How many arguments a call has, counted without evaluating any: a texture lookup
 * checks its arity before its first argument, a sampler, which has no value. */
static int argc_of(const glsl_gen_t *g, int32_t first_arg) {
    int n = 0;
    for (int32_t a = first_arg; a != GLSL_NO_NODE; a = g->ast->nodes[a].sibling)
        n++;
    return n;
}

/* The built-in families after the texture lookups, one function each. A family returns
 * GEN_NOT_MINE for a name it does not generate; every other answer, an error included,
 * is final. */
typedef glsl_value_t (*gen_builtin_family_fn)(glsl_gen_t *g, const char *nm, size_t len,
                                              const glsl_value_t *arg, int argc, int w,
                                              int32_t first_arg, int32_t node);
static const glsl_value_t GEN_NOT_MINE = {0u, -1};
static glsl_value_t gen_tex_address(glsl_gen_t *g, glsl_value_t uv, GLboolean tex_cube,
                                    GLboolean projective, int div_w,
                                    GLboolean want_shadow, GLboolean want_oned,
                                    uint32_t set, uint32_t want_dim, GLboolean biased,
                                    int32_t coord_node, int32_t node);
static glsl_value_t gen_tex_sample(glsl_gen_t *g, glsl_value_t uv, uint32_t set,
                                   uint32_t want_dim, GLboolean want_shadow,
                                   GLboolean biased, int32_t coord_node, int32_t node);
static glsl_value_t gen_builtin_relational(glsl_gen_t *g, const char *nm, size_t len,
                                           const glsl_value_t *arg, int argc, int w,
                                           int32_t first_arg, int32_t node);
static glsl_value_t gen_builtin_componentwise(glsl_gen_t *g, const char *nm, size_t len,
                                              const glsl_value_t *arg, int argc, int w,
                                              int32_t first_arg, int32_t node);
static glsl_value_t gen_builtin_common(glsl_gen_t *g, const char *nm, size_t len,
                                       const glsl_value_t *arg, int argc, int w,
                                       int32_t first_arg, int32_t node);
static glsl_value_t gen_builtin_step_dot(glsl_gen_t *g, const char *nm, size_t len,
                                         const glsl_value_t *arg, int argc, int w,
                                         int32_t first_arg, int32_t node);
static glsl_value_t gen_builtin_inverse_trig(glsl_gen_t *g, const char *nm, size_t len,
                                             const glsl_value_t *arg, int argc, int w,
                                             int32_t first_arg, int32_t node);
static glsl_value_t gen_builtin_matrix(glsl_gen_t *g, const char *nm, size_t len,
                                       const glsl_value_t *arg, int argc, int w,
                                       int32_t first_arg, int32_t node);
static glsl_value_t gen_builtin_geometric(glsl_gen_t *g, const char *nm, size_t len,
                                          const glsl_value_t *arg, int argc, int w,
                                          int32_t first_arg, int32_t node);

/* The texture lookups, or GEN_NOT_MINE for a name that is not one. */
static glsl_value_t gen_builtin_texture(glsl_gen_t *g, const char *nm, size_t len,
                                        int32_t first_arg, int32_t node) {
    /* A projective lookup divides the coordinate by its last component:
     * `texture2DProj(s, vec4)` divides by `w` and ignores `z`, `texture2DProj(s, vec3)`
     * divides by `z`, as `glsl_exec.c` does. A zero divisor answers zero, as the
     * reference does; ordinary `/` does not guard this way. The dimension is one field
     * of the instruction (`tools/shader/gl2-fragment.s`). */
    const GLboolean tex_proj = nm_is(nm, len, "texture2DProj");
    const GLboolean tex_cube = nm_is(nm, len, "textureCube");
    const GLboolean tex_3d = nm_is(nm, len, "texture3D");
    const GLboolean tex_3dproj = nm_is(nm, len, "texture3DProj");
    const GLboolean tex_shadow = nm_is(nm, len, "shadow2D");
    const GLboolean tex_shadowproj = nm_is(nm, len, "shadow2DProj");
    const GLboolean tex_1d = nm_is(nm, len, "texture1D");
    const GLboolean tex_1dproj = nm_is(nm, len, "texture1DProj");
    const GLboolean tex_1dshadow = nm_is(nm, len, "shadow1D");
    const GLboolean tex_1dshadowproj = nm_is(nm, len, "shadow1DProj");
    if (!(nm_is(nm, len, "texture2D") || tex_proj || tex_cube || tex_3d || tex_3dproj ||
          tex_shadow || tex_shadowproj || tex_1d || tex_1dproj || tex_1dshadow ||
          tex_1dshadowproj)) {
        if (nm_prefix(nm, len, "texture") || nm_prefix(nm, len, "shadow")) {
            return gen_fail(
                g,
                "this texture lookup is not generated; texture1D, texture2D, "
                "texture3D and textureCube, the Proj forms of the first "
                "three, and shadow1D and shadow2D with their Proj forms are",
                node);
        }
        return GEN_NOT_MINE;
    }
    /* The dim the lookup's name asks for; the sampler's has to match it below. */
    const GLboolean want_shadow =
        (GLboolean)(tex_shadow || tex_shadowproj || tex_1dshadow || tex_1dshadowproj);
    const GLboolean want_oned =
        (GLboolean)(tex_1d || tex_1dproj || tex_1dshadow || tex_1dshadowproj);
    /* A 1D lookup samples with the 2D dim, because a 1D texture is described as one
     * row of a 2D image (TYPE 9). */
    const uint32_t want_dim =
        tex_cube ? GLSL_IMG_DIM_CUBE
                 : ((tex_3d || tex_3dproj) ? GLSL_IMG_DIM_3D : GLSL_IMG_DIM_2D);
    /* How many components the coordinate carries. A projective form carries one
     * more than it samples - the divisor - a cube carries three that become a face
     * and a place on it, and a shadow carries the reference in its third.
     * `texture1D` takes a bare float, and `texture2DProj`'s two accepted widths are
     * the one case decided from the argument. */
    const int coord_w = tex_1d                                               ? 1
                        : (tex_cube || tex_3d || tex_shadow || tex_1dshadow) ? 3
                        : (tex_3dproj || tex_shadowproj || tex_1dshadowproj) ? 4
                        : tex_1dproj                                         ? 2
                        : tex_proj                                           ? 0
                                                                             : 2;
    /* Everything before the divisor is divided, as the reference does: one component
     * for a 1D, two for a 2D, three for a volume, and a shadow's reference with them.
     */
    const GLboolean projective = (GLboolean)(tex_proj || tex_3dproj || tex_shadowproj ||
                                             tex_1dproj || tex_1dshadowproj);
    /* A third argument is a level-of-detail bias (GLSL 1.10, every fragment-stage
     * lookup): a different opcode with one more address register. Not offered for the
     * shadow forms, whose biased `image_sample_c` opcode has not been read out of an
     * assembler. */
    const int tex_argc = argc_of(g, first_arg);
    const GLboolean biased = (GLboolean)(tex_argc == 3 && !want_shadow);
    if (tex_argc != 2 && !biased) {
        return gen_fail(g,
                        want_shadow
                            ? "a shadow lookup takes a sampler and a coordinate; the "
                              "biased form goes through a different instruction and is "
                              "not generated"
                            : "a texture lookup takes a sampler and a coordinate, and "
                              "may take a level-of-detail bias after them",
                        node);
    }
    const glsl_node_t *sn = &g->ast->nodes[first_arg];
    if (sn->kind != GLSL_NODE_IDENTIFIER) {
        return gen_fail(
            g,
            "the sampler has to be named directly; there is no way to carry "
            "one in a variable here",
            node);
    }
    uint32_t set = 0u;
    GLboolean found = GL_FALSE;
    uint32_t samp_dim = GLSL_IMG_DIM_2D;
    GLboolean samp_shadow = GL_FALSE;
    GLboolean samp_oned = GL_FALSE;
    for (int i = 0; i < g->sampler_count; i++) {
        if (g->samplers[i].name_len == sn->length &&
            nm_is(sn->text, sn->length, g->samplers[i].name)) {
            set = g->samplers[i].set;
            samp_dim = g->samplers[i].dim;
            samp_shadow = g->samplers[i].shadow;
            samp_oned = g->samplers[i].oned;
            found = GL_TRUE;
            break;
        }
    }
    if (!found) {
        return gen_fail(
            g, "this name is not a sampler the draw path loads descriptors for", node);
    }
    /* The lookup and the sampler have to be the same shape: the descriptor decides
     * how memory is walked (TYPE 0xb for a cube, 0xa for a volume) and the shader how
     * many address registers it hands over. */
    if (samp_dim != want_dim || samp_shadow != want_shadow || samp_oned != want_oned) {
        return gen_fail(
            g,
            "this lookup does not match its sampler's type: texture1D takes "
            "a sampler1D, texture2D a sampler2D, textureCube a samplerCube, "
            "texture3D a sampler3D, and the shadow forms their own",
            node);
    }
    const int32_t coord_node = g->ast->nodes[first_arg].sibling;
    const glsl_type_t ct = glsl_type_of(g->sema, coord_node);
    /* `texture2DProj` and `texture1DProj` each take two widths, so theirs is decided
     * from the argument rather than the name. */
    const int want_w = (coord_w != 0) ? coord_w : (ct == GLSL_TYPE_VEC4 ? 4 : 3);
    const glsl_type_t want_t = (want_w == 4)   ? GLSL_TYPE_VEC4
                               : (want_w == 3) ? GLSL_TYPE_VEC3
                               : (want_w == 2) ? GLSL_TYPE_VEC2
                                               : GLSL_TYPE_FLOAT;
    if (ct != want_t && !(tex_proj && ct == GLSL_TYPE_VEC3) &&
        !(tex_1dproj && ct == GLSL_TYPE_VEC4)) {
        return gen_fail(
            g,
            "this texture lookup's coordinate is not the width it takes: "
            "texture1D a float, texture2D a vec2, textureCube a vec3 "
            "direction, texture3D a vec3, a shadow form the reference in its "
            "third, and a projective form one component more",
            node);
    }
    glsl_value_t uv = gen_expr(g, coord_node);
    if (is_bad(uv))
        return uv;
    if (uv.count != want_w && !(tex_1dproj && uv.count == 4)) {
        return gen_fail(g, "this texture coordinate is not the width its type says",
                        node);
    }
    /* Everything ahead of the divisor, which is the last component of whatever
     * arrived. */
    const int div_w = uv.count - 1;
    return gen_tex_address(g, uv, tex_cube, projective, div_w, want_shadow, want_oned,
                           set, want_dim, biased, coord_node, node);
}

/* A lookup's coordinate turned into the address run the sampler reads - a cube's
 * face, a projective divide, a shadow's reference - then the sample. */
static glsl_value_t gen_tex_address(glsl_gen_t *g, glsl_value_t uv, GLboolean tex_cube,
                                    GLboolean projective, int div_w,
                                    GLboolean want_shadow, GLboolean want_oned,
                                    uint32_t set, uint32_t want_dim, GLboolean biased,
                                    int32_t coord_node, int32_t node) {
    /* A cube's coordinate is a direction. `v_cubeid_f32` names the face,
     * `v_cubesc_f32` and `v_cubetc_f32` give the place on it and `v_cubema_f32` twice
     * the major axis; the shader divides by that and biases by a half to land in
     * [0, 1]. The sampler takes u, v and the face. This is ACO's sequence, and
     * `tools/shader/tex-cube.s` is the assembled copy. The direction needs no
     * normalising. `|ma|` is `max(ma, -ma)` rather than the VOP3 abs modifier. */
    if (tex_cube) {
        glsl_value_t half = gen_const(g, 0.5, node);
        if (is_bad(half))
            return half;
        glsl_value_t t = gen_alloc(g, 4, node);
        if (is_bad(t))
            return t;
        const uint32_t sc = t.base + 0u, tc = t.base + 1u;
        const uint32_t ma = t.base + 2u, neg = t.base + 3u;
        const uint32_t X = GLSL_VOP3_VGPR(uv.base + 0u);
        const uint32_t Y = GLSL_VOP3_VGPR(uv.base + 1u);
        const uint32_t Z = GLSL_VOP3_VGPR(uv.base + 2u);
        /* The three address registers, allocated as one run because `image_sample`
         * reads them consecutively from `vaddr`. */
        glsl_value_t addr = gen_alloc(g, 3, node);
        if (is_bad(addr))
            return addr;
        glsl_emit_vop3(g->code, GLSL_VOP3_CUBEID_F32, addr.base + 2u, X, Y, Z);
        glsl_emit_vop3(g->code, GLSL_VOP3_CUBESC_F32, sc, X, Y, Z);
        glsl_emit_vop3(g->code, GLSL_VOP3_CUBETC_F32, tc, X, Y, Z);
        glsl_emit_vop3(g->code, GLSL_VOP3_CUBEMA_F32, ma, X, Y, Z);
        glsl_emit_neg_f32(g->code, neg, ma);
        glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, ma, neg, ma); /* |ma| */
        glsl_emit_vop1_op(g->code, GLSL_VOP1_RCP_F32, ma, ma);
        glsl_emit_mul_f32(g->code, ma, half.base, ma); /* 1 / (2 |ma|) */
        glsl_emit_mov(g->code, addr.base + 0u, half.base);
        glsl_emit_fmac_f32(g->code, addr.base + 0u, sc, ma); /* u */
        glsl_emit_mov(g->code, addr.base + 1u, half.base);
        glsl_emit_fmac_f32(g->code, addr.base + 1u, tc, ma); /* v */
        uv = addr;
    }
    if (projective) {
        const uint32_t q = uv.base + (uint32_t)(uv.count - 1);
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return zero;
        glsl_value_t rcp = gen_alloc(g, 1, node);
        if (is_bad(rcp))
            return rcp;
        glsl_value_t inv = gen_alloc(g, 1, node);
        if (is_bad(inv))
            return inv;
        glsl_emit_vop1_op(g->code, GLSL_VOP1_RCP_F32, rcp.base, q);
        glsl_emit_cmp(g->code, GLSL_VOPC_NEQ_F32, q, zero.base);
        /* `d = vcc ? s1 : s0`, so the false value comes first: a zero divisor takes
         * the zero rather than the reciprocal's infinity. */
        glsl_emit_cndmask(g->code, inv.base, zero.base, rcp.base);
        /* Into its own run, after the coordinate, so no multiply reads a component
         * another has already overwritten. */
        glsl_value_t st = gen_alloc(g, div_w, node);
        if (is_bad(st))
            return st;
        for (int cc = 0; cc < div_w; cc++) {
            glsl_emit_mul_f32(g->code, st.base + (uint32_t)cc, uv.base + (uint32_t)cc,
                              inv.base);
        }
        uv = st;
    }

    /* A shadow lookup's reference comes first in VADDR, ahead of s and t, as
     * measured on hardware. It is clamped to [0, 1], low end first (GL 1.4 3.8.14,
     * the order `tools/shader/tex-shadow.s` follows softpipe in). The comparison is
     * the sampler's `DEPTH_COMPARE_FUNC`; the shader supplies the reference and the
     * `_c` instruction. */
    if (want_shadow || want_oned) {
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return zero;
        const int aw = want_shadow ? 3 : 2;
        glsl_value_t addr = gen_alloc(g, aw, node);
        if (is_bad(addr))
            return addr;
        int at = 0;
        if (want_shadow) {
            glsl_value_t one = gen_const(g, 1.0, node);
            if (is_bad(one))
                return one;
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, addr.base + 0u, zero.base,
                              uv.base + 2u);
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MIN_F32, addr.base + 0u, one.base,
                              addr.base + 0u);
            at = 1;
        }
        glsl_emit_mov(g->code, addr.base + (uint32_t)at, uv.base + 0u); /* s */
        /* `t` is zero for a 1D lookup: the descriptor is 2D, so the sampler reads a
         * `t` either way. */
        glsl_emit_mov(g->code, addr.base + (uint32_t)at + 1u,
                      want_oned ? zero.base : uv.base + 1u);
        uv = addr;
    }
    return gen_tex_sample(g, uv, set, want_dim, want_shadow, biased, coord_node, node);
}

/* The sample itself, from the address run in `uv`. */
static glsl_value_t gen_tex_sample(glsl_gen_t *g, glsl_value_t uv, uint32_t set,
                                   uint32_t want_dim, GLboolean want_shadow,
                                   GLboolean biased, int32_t coord_node, int32_t node) {
    /* The sample's four registers are allocated after the coordinate so `vdata`
     * cannot overlap `vaddr`. */
    glsl_value_t out = gen_alloc(g, 4, node);
    if (is_bad(out))
        return out;
    const uint32_t srsrc = GLSL_GEN_TEX_SGPR_BASE + set * GLSL_GEN_TEX_SGPR_STRIDE;
    if (want_shadow) {
        /* One value comes back (`dmask:0x1`), spread as `GL_DEPTH_TEXTURE_MODE`'s
         * default `GL_LUMINANCE`, `(v, v, v, 1)`. The other modes are per-texture and
         * chosen after compilation; the draw path logs them. */
        glsl_emit_image_sample_masked(g->code, GLSL_MIMG_SAMPLE_C, GLSL_IMG_DIM_2D,
                                      0x1u, out.base, uv.base, srsrc, srsrc + 8u);
        glsl_emit_s_waitcnt_vm(g->code);
        glsl_emit_mov(g->code, out.base + 1u, out.base + 0u);
        glsl_emit_mov(g->code, out.base + 2u, out.base + 0u);
        glsl_emit_mov_imm(g->code, out.base + 3u, float_bits(1.0f));
        return out;
    }
    /* The bias goes first in the address run, so a biased lookup copies the
     * coordinate into a fresh run behind it. */
    uint32_t addr_base = uv.base;
    if (biased) {
        const int32_t bias_node = g->ast->nodes[coord_node].sibling;
        glsl_value_t b = gen_expr(g, bias_node);
        if (is_bad(b))
            return b;
        if (b.count != 1) {
            return gen_fail(g, "a texture lookup's bias is a single number", node);
        }
        glsl_value_t run = gen_alloc(g, uv.count + 1, node);
        if (is_bad(run))
            return run;
        glsl_emit_mov(g->code, run.base, b.base);
        for (int i = 0; i < uv.count; i++) {
            glsl_emit_mov(g->code, run.base + 1u + (uint32_t)i, uv.base + (uint32_t)i);
        }
        addr_base = run.base;
    }
    glsl_emit_image_sample(g->code, biased ? GLSL_MIMG_SAMPLE_B : GLSL_MIMG_SAMPLE,
                           want_dim, out.base, addr_base, srsrc, srsrc + 8u);
    /* A sample is not in order with what follows it: without the wait the next
     * instruction reads the destination before the texture unit writes it, the hazard
     * `s_waitcnt lgkmcnt(0)` covers for the uniform block. */
    glsl_emit_s_waitcnt_vm(g->code);
    return out;
}

static glsl_value_t gen_builtin(glsl_gen_t *g, const glsl_node_t *callee,
                                int32_t first_arg, int32_t node) {
    const char *const nm = callee->text;
    const size_t len = callee->length;
    const glsl_value_t tex = gen_builtin_texture(g, nm, len, first_arg, node);
    if (tex.count >= 0)
        return tex;

    /* The arguments, left to right, evaluated once each into registers that outlive
     * the lowering, so a built-in that uses an argument twice does not evaluate it
     * twice. */
    glsl_value_t arg[GEN_MAX_ARGS];
    int argc = 0;
    /* Two built-ins take a matrix and the rest do not: a componentwise lowering walks
     * its arguments as a run of components, which is wrong for a matrix. */
    const GLboolean takes_matrix =
        (GLboolean)(nm_is(nm, len, "matrixCompMult") || nm_is(nm, len, "transpose"));
    for (int32_t a = first_arg; a != GLSL_NO_NODE; a = g->ast->nodes[a].sibling) {
        if (argc == GEN_MAX_ARGS) {
            return gen_fail(g, "more arguments than any built-in generated here takes",
                            node);
        }
        const glsl_type_t at = glsl_type_of(g->sema, a);
        /* A `bool` or a `bvec` is a float per component here, so the reductions take
         * one directly; an `int` is a whole float and compares and scales like any
         * other. */
        if ((!is_float_family(at) && !is_bool_family(at) && !is_int_family(at)) ||
            (is_matrix(at) && !takes_matrix)) {
            return gen_fail(
                g,
                "this built-in is generated for float, int and bool arguments "
                "only",
                node);
        }
        arg[argc] = gen_expr(g, a);
        if (is_bad(arg[argc]))
            return arg[argc];
        argc++;
    }
    if (argc == 0) {
        return gen_fail(g, "a built-in with no arguments is not one this generates",
                        node);
    }

    /* The widest argument: every component-wise built-in here returns that width, and
     * the rules above let a scalar stand in for any of them. */
    int w = arg[0].count;
    for (int i = 1; i < argc; i++) {
        if (arg[i].count > w)
            w = arg[i].count;
    }

    static const gen_builtin_family_fn FAMILIES[] = {
        gen_builtin_relational, gen_builtin_componentwise, gen_builtin_common,
        gen_builtin_step_dot,   gen_builtin_inverse_trig,  gen_builtin_matrix,
        gen_builtin_geometric,
    };
    for (size_t i = 0; i < sizeof(FAMILIES) / sizeof(FAMILIES[0]); i++) {
        const glsl_value_t r = FAMILIES[i](g, nm, len, arg, argc, w, first_arg, node);
        if (r.count >= 0)
            return r;
    }
    return gen_fail(
        g,
        "only constructors and the built-ins with verified instructions are "
        "generated; this name is neither, and no function of that name is "
        "defined in this shader",
        node);
}

/* The vector relational functions, the derivatives, `noise` and the reductions. */
static glsl_value_t gen_builtin_relational(glsl_gen_t *g, const char *nm, size_t len,
                                           const glsl_value_t *arg, int argc, int w,
                                           int32_t first_arg, int32_t node) {
    (void)w;
    (void)first_arg;
    /* The vector relational family. A `bvec` is 0.0 or 1.0 per component, so the
     * comparisons are the scalar compare-and-select per component, and `any` is a
     * `max`, `all` a `min` and `not` is `1 - x`. */
    {
        struct {
            const char *name;
            uint32_t op;
        } const REL[] = {
            {"lessThan", GLSL_VOPC_LT_F32},    {"lessThanEqual", GLSL_VOPC_LE_F32},
            {"greaterThan", GLSL_VOPC_GT_F32}, {"greaterThanEqual", GLSL_VOPC_GE_F32},
            {"equal", GLSL_VOPC_EQ_F32},       {"notEqual", GLSL_VOPC_NEQ_F32},
        };
        for (size_t i = 0; i < sizeof(REL) / sizeof(REL[0]); i++) {
            if (!nm_is(nm, len, REL[i].name))
                continue;
            if (argc != 2)
                return gen_fail(g, "wrong number of arguments", node);
            if (arg[0].count != arg[1].count) {
                return gen_fail(
                    g,
                    "the vector relational functions take two operands of the "
                    "same width",
                    node);
            }
            glsl_value_t rzero = gen_const(g, 0.0, node);
            if (is_bad(rzero))
                return rzero;
            glsl_value_t rone = gen_const(g, 1.0, node);
            if (is_bad(rone))
                return rone;
            glsl_value_t d = gen_alloc(g, arg[0].count, node);
            if (is_bad(d))
                return d;
            for (int cc = 0; cc < arg[0].count; cc++) {
                glsl_emit_cmp(g->code, REL[i].op, comp_of(arg[0], cc),
                              comp_of(arg[1], cc));
                glsl_emit_cndmask(g->code, d.base + (uint32_t)cc, rzero.base,
                                  rone.base);
            }
            return d;
        }
    }
    /* `noise1`..`noise4` are zero, as `glsl_builtin.c`'s `BI_NOISE` defines them. The
     * argument is already evaluated in `arg[0]`, so its side effects happen. */
    if (nm_is(nm, len, "noise1") || nm_is(nm, len, "noise2") ||
        nm_is(nm, len, "noise3") || nm_is(nm, len, "noise4")) {
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        const int noise_w = nm[len - 1u] - '0';
        glsl_value_t out = gen_alloc(g, noise_w, node);
        if (is_bad(out))
            return out;
        for (int cc = 0; cc < noise_w; cc++) {
            glsl_emit_mov_imm(g->code, out.base + (uint32_t)cc, float_bits(0.0f));
        }
        return out;
    }

    /* A derivative is the difference between this lane and its quad neighbour, which
     * exists in an uncovered lane only because whole-quad mode kept it running;
     * `glsl_ps.c` turns WQM on for a shader that names one. `dFdy` is the bottom row
     * less the top, following the window's downward y as the reference does. */
    if (nm_is(nm, len, "dFdx") || nm_is(nm, len, "dFdy") || nm_is(nm, len, "fwidth")) {
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, arg[0].count, node);
        if (is_bad(d))
            return d;
        const GLboolean want_x = (GLboolean)!nm_is(nm, len, "dFdy");
        const GLboolean want_y = (GLboolean)!nm_is(nm, len, "dFdx");
        for (int cc = 0; cc < arg[0].count; cc++) {
            const uint32_t mark = gen_mark(g);
            glsl_value_t t = gen_alloc(g, 2, node);
            if (is_bad(t))
                return t;
            const uint32_t near = t.base + 0u, acc = t.base + 1u;
            const uint32_t s = comp_of(arg[0], cc);
            if (want_x) {
                glsl_emit_dpp_mov(g->code, near, s, GLSL_DPP_QUAD_X_NEAR);
                glsl_emit_dpp_sub(g->code, acc, s, near, GLSL_DPP_QUAD_X_FAR);
                if (!want_y)
                    glsl_emit_mov(g->code, d.base + (uint32_t)cc, acc);
            }
            if (want_y) {
                const uint32_t ydst = want_x ? near : (d.base + (uint32_t)cc);
                glsl_emit_dpp_mov(g->code, t.base + 0u, s, GLSL_DPP_QUAD_Y_NEAR);
                glsl_emit_dpp_sub(g->code, ydst, s, t.base + 0u, GLSL_DPP_QUAD_Y_FAR);
                if (want_x) {
                    /* `fwidth` is |dFdx| + |dFdy|, an absolute value being
                     * `max(v, -v)`. */
                    glsl_value_t n2 = gen_alloc(g, 1, node);
                    if (is_bad(n2))
                        return n2;
                    glsl_emit_neg_f32(g->code, n2.base, acc);
                    glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, acc, acc, n2.base);
                    glsl_emit_neg_f32(g->code, n2.base, ydst);
                    glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, ydst, ydst, n2.base);
                    glsl_emit_add_f32(g->code, d.base + (uint32_t)cc, acc, ydst);
                }
            }
            gen_release(g, mark);
        }
        return d;
    }
    if (nm_is(nm, len, "any") || nm_is(nm, len, "all")) {
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        const uint32_t rop =
            nm_is(nm, len, "any") ? GLSL_VOP2_MAX_F32 : GLSL_VOP2_MIN_F32;
        glsl_value_t d = gen_alloc(g, 1, node);
        if (is_bad(d))
            return d;
        glsl_emit_mov(g->code, d.base, arg[0].base);
        for (int cc = 1; cc < arg[0].count; cc++) {
            glsl_emit_vop2_op(g->code, rop, d.base, d.base, comp_of(arg[0], cc));
        }
        return d;
    }
    if (nm_is(nm, len, "not")) {
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t rone = gen_const(g, 1.0, node);
        if (is_bad(rone))
            return rone;
        glsl_value_t d = gen_alloc(g, arg[0].count, node);
        if (is_bad(d))
            return d;
        for (int cc = 0; cc < arg[0].count; cc++) {
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)cc, rone.base,
                              comp_of(arg[0], cc));
        }
        return d;
    }
    return GEN_NOT_MINE;
}

/* One instruction a component, the constant scales, and the transcendental ones. */
static glsl_value_t gen_builtin_componentwise(glsl_gen_t *g, const char *nm, size_t len,
                                              const glsl_value_t *arg, int argc, int w,
                                              int32_t first_arg, int32_t node) {
    (void)first_arg;
    /* --- one instruction a component ------------------------------------ */
    struct {
        const char *name;
        uint32_t op;
        int args;
    } const MAP[] = {
        {"sqrt", GLSL_VOP1_SQRT_F32, 1},   {"inversesqrt", GLSL_VOP1_RSQ_F32, 1},
        {"floor", GLSL_VOP1_FLOOR_F32, 1}, {"ceil", GLSL_VOP1_CEIL_F32, 1},
        {"fract", GLSL_VOP1_FRACT_F32, 1}, {"exp2", GLSL_VOP1_EXP_F32, 1},
        {"log2", GLSL_VOP1_LOG_F32, 1},    {"min", GLSL_VOP2_MIN_F32, 2},
        {"max", GLSL_VOP2_MAX_F32, 2},
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (!nm_is(nm, len, MAP[i].name))
            continue;
        if (argc != MAP[i].args)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        if (MAP[i].args == 1) {
            gen_map1(g, MAP[i].op, d, arg[0]);
        } else {
            gen_map2(g, MAP[i].op, d, arg[0], arg[1]);
        }
        return d;
    }

    /* --- a multiply by a constant --------------------------------------- */
    struct {
        const char *name;
        double k;
    } const SCALE[] = {
        {"radians", 3.14159265358979323846 / 180.0},
        {"degrees", 180.0 / 3.14159265358979323846},
    };
    for (size_t i = 0; i < sizeof(SCALE) / sizeof(SCALE[0]); i++) {
        if (!nm_is(nm, len, SCALE[i].name))
            continue;
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t k = gen_const(g, SCALE[i].k, node);
        if (is_bad(k))
            return k;
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c),
                              k.base);
        }
        return d;
    }

    /* --- the trigonometric pair, in revolutions ------------------------- */
    if (nm_is(nm, len, "sin") || nm_is(nm, len, "cos") || nm_is(nm, len, "tan")) {
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t k = gen_const(g, 1.0 / (2.0 * 3.14159265358979323846), node);
        if (is_bad(k))
            return k;
        glsl_value_t rev = gen_alloc(g, w, node);
        if (is_bad(rev))
            return rev;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, rev.base + (uint32_t)c, comp_of(arg[0], c),
                              k.base);
        }
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        if (nm_is(nm, len, "sin")) {
            gen_map1(g, GLSL_VOP1_SIN_F32, d, rev);
            return d;
        }
        if (nm_is(nm, len, "cos")) {
            gen_map1(g, GLSL_VOP1_COS_F32, d, rev);
            return d;
        }
        /* `tan` is the quotient, so it is a sine, a cosine, a reciprocal and a
         * multiply. */
        glsl_value_t co = gen_alloc(g, w, node);
        if (is_bad(co))
            return co;
        gen_map1(g, GLSL_VOP1_SIN_F32, d, rev);
        gen_map1(g, GLSL_VOP1_COS_F32, co, rev);
        for (int c = 0; c < w; c++) {
            glsl_emit_vop1_op(g->code, GLSL_VOP1_RCP_F32, co.base + (uint32_t)c,
                              co.base + (uint32_t)c);
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, d.base + (uint32_t)c,
                              co.base + (uint32_t)c);
        }
        return d;
    }

    /* --- the base-e pair, which the hardware has in base two ------------ */
    if (nm_is(nm, len, "exp") || nm_is(nm, len, "log")) {
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        const GLboolean is_exp = nm_is(nm, len, "exp") ? GL_TRUE : GL_FALSE;
        glsl_value_t k =
            gen_const(g, is_exp ? 1.4426950408889634 : 0.6931471805599453, node);
        if (is_bad(k))
            return k;
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        if (is_exp) {
            /* exp(x) = 2^(x * log2 e) */
            for (int c = 0; c < w; c++) {
                glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c),
                                  k.base);
            }
            gen_map1(g, GLSL_VOP1_EXP_F32, d, d);
        } else {
            /* log(x) = log2(x) * ln 2 */
            gen_map1(g, GLSL_VOP1_LOG_F32, d, arg[0]);
            for (int c = 0; c < w; c++) {
                glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, d.base + (uint32_t)c,
                                  k.base);
            }
        }
        return d;
    }

    /* `pow(x, y) = 2^(y * log2 x)`, which is what the hardware's pair composes to.
     * Undefined in GLSL for a negative `x`, and `v_log_f32` of a negative is a NaN, so
     * the two agree. */
    if (nm_is(nm, len, "pow")) {
        if (argc != 2)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        gen_map1(g, GLSL_VOP1_LOG_F32, d, arg[0]);
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, d.base + (uint32_t)c,
                              comp_of(arg[1], c));
        }
        gen_map1(g, GLSL_VOP1_EXP_F32, d, d);
        return d;
    }
    return GEN_NOT_MINE;
}

/* `abs`, `sign`, `clamp`, `mix` and `mod`. */
static glsl_value_t gen_builtin_common(glsl_gen_t *g, const char *nm, size_t len,
                                       const glsl_value_t *arg, int argc, int w,
                                       int32_t first_arg, int32_t node) {
    (void)first_arg;
    /* `abs(x) = max(x, -x)`, rather than a sign-bit clear, which would need `v_and_b32`
     * and a literal mask, neither in the pinned table. */
    if (nm_is(nm, len, "abs")) {
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_neg_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c));
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, d.base + (uint32_t)c,
                              d.base + (uint32_t)c, comp_of(arg[0], c));
        }
        return d;
    }

    /* `sign(x)` is -1, 0 or 1, so it is two selects and a subtract; one select would
     * give 1 for x == 0. */
    if (nm_is(nm, len, "sign")) {
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t pos =
            gen_select_on_sign(g, arg[0], GLSL_VOPC_GT_F32, 1.0, 0.0, node);
        if (is_bad(pos))
            return pos;
        glsl_value_t neg =
            gen_select_on_sign(g, arg[0], GLSL_VOPC_LT_F32, 1.0, 0.0, node);
        if (is_bad(neg))
            return neg;
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)c, pos.base + (uint32_t)c,
                              neg.base + (uint32_t)c);
        }
        return d;
    }

    /* `clamp(x, lo, hi) = min(max(x, lo), hi)`, the usual order; it differs from the
     * other only for a NaN, where GLSL says nothing. */
    if (nm_is(nm, len, "clamp")) {
        if (argc != 3)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        gen_map2(g, GLSL_VOP2_MAX_F32, d, arg[0], arg[1]);
        gen_map2(g, GLSL_VOP2_MIN_F32, d, d, arg[2]);
        return d;
    }

    /* `mix(a, b, t) = a + (b - a) * t`, three instructions a component with the fused
     * multiply-add, and exactly `a` at t = 0. */
    if (nm_is(nm, len, "mix")) {
        if (argc != 3)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        glsl_value_t diff = gen_alloc(g, w, node);
        if (is_bad(diff))
            return diff;
        for (int c = 0; c < w; c++) {
            glsl_emit_sub_f32(g->code, diff.base + (uint32_t)c, comp_of(arg[1], c),
                              comp_of(arg[0], c));
            glsl_emit_mov(g->code, d.base + (uint32_t)c, comp_of(arg[0], c));
            glsl_emit_fmac_f32(g->code, d.base + (uint32_t)c, diff.base + (uint32_t)c,
                               comp_of(arg[2], c));
        }
        return d;
    }

    /* `mod(x, y) = x - y * floor(x / y)`, GLSL's definition. */
    if (nm_is(nm, len, "mod")) {
        if (argc != 2)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t r = gen_alloc(g, arg[1].count, node);
        if (is_bad(r))
            return r;
        gen_map1(g, GLSL_VOP1_RCP_F32, r, arg[1]);
        glsl_value_t q = gen_alloc(g, w, node);
        if (is_bad(q))
            return q;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, q.base + (uint32_t)c, comp_of(arg[0], c),
                              comp_of(r, c));
        }
        gen_map1(g, GLSL_VOP1_FLOOR_F32, q, q);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, q.base + (uint32_t)c,
                              comp_of(arg[1], c));
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c),
                              d.base + (uint32_t)c);
        }
        return d;
    }
    return GEN_NOT_MINE;
}

/* `step`, `smoothstep` and `dot`. */
static glsl_value_t gen_builtin_step_dot(glsl_gen_t *g, const char *nm, size_t len,
                                         const glsl_value_t *arg, int argc, int w,
                                         int32_t first_arg, int32_t node) {
    (void)first_arg;
    /* `step(edge, x)` is 0 below the edge and 1 at or above it: the comparison is
     * `x < edge` and the false arm, `v_cndmask`'s src0, is the 1. */
    if (nm_is(nm, len, "step")) {
        if (argc != 2)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return zero;
        glsl_value_t one = gen_const(g, 1.0, node);
        if (is_bad(one))
            return one;
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_cmp(g->code, GLSL_VOPC_LT_F32, comp_of(arg[1], c),
                          comp_of(arg[0], c));
            glsl_emit_cndmask(g->code, d.base + (uint32_t)c, one.base, zero.base);
        }
        return d;
    }

    /* `smoothstep(e0, e1, x)`: `t = clamp((x - e0) / (e1 - e0), 0, 1)`, then
     * `t*t*(3 - 2t)`, GLSL 1.10 8.3's expansion. */
    if (nm_is(nm, len, "smoothstep")) {
        if (argc != 3)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return zero;
        glsl_value_t one = gen_const(g, 1.0, node);
        if (is_bad(one))
            return one;
        glsl_value_t three = gen_const(g, 3.0, node);
        if (is_bad(three))
            return three;
        glsl_value_t two = gen_const(g, 2.0, node);
        if (is_bad(two))
            return two;
        glsl_value_t t = gen_alloc(g, w, node);
        if (is_bad(t))
            return t;
        glsl_value_t den = gen_alloc(g, w, node);
        if (is_bad(den))
            return den;
        for (int c = 0; c < w; c++) {
            glsl_emit_sub_f32(g->code, t.base + (uint32_t)c, comp_of(arg[2], c),
                              comp_of(arg[0], c));
            glsl_emit_sub_f32(g->code, den.base + (uint32_t)c, comp_of(arg[1], c),
                              comp_of(arg[0], c));
            glsl_emit_vop1_op(g->code, GLSL_VOP1_RCP_F32, den.base + (uint32_t)c,
                              den.base + (uint32_t)c);
            glsl_emit_mul_f32(g->code, t.base + (uint32_t)c, t.base + (uint32_t)c,
                              den.base + (uint32_t)c);
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, t.base + (uint32_t)c,
                              t.base + (uint32_t)c, zero.base);
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MIN_F32, t.base + (uint32_t)c,
                              t.base + (uint32_t)c, one.base);
        }
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        for (int c = 0; c < w; c++) {
            /* 3 - 2t, then t*t times it. */
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, t.base + (uint32_t)c,
                              two.base);
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)c, three.base,
                              d.base + (uint32_t)c);
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, d.base + (uint32_t)c,
                              t.base + (uint32_t)c);
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, d.base + (uint32_t)c,
                              t.base + (uint32_t)c);
        }
        return d;
    }

    /* --- the geometric ones --------------------------------------------- */

    if (nm_is(nm, len, "dot")) {
        if (argc != 2)
            return gen_fail(g, "wrong number of arguments", node);
        return gen_dot(g, arg[0], arg[1], node);
    }
    return GEN_NOT_MINE;
}

/* `atan`, `asin` and `acos`. */
static glsl_value_t gen_builtin_inverse_trig(glsl_gen_t *g, const char *nm, size_t len,
                                             const glsl_value_t *arg, int argc, int w,
                                             int32_t first_arg, int32_t node) {
    (void)w;
    (void)first_arg;
    /* `atan`, with `asin` and `acos` built on it, uses the reduction and minimax
     * polynomial of `oops_atan2f` in `src/math/math.c`, through which the software
     * rasteriser answers every `atan`, so both paths compute one function:
     *
     *     a = min(|y|, |x|) / max(|y|, |x|)
     *     r = ((-0.0464964749 s + 0.15931422) s - 0.327622764) s a + a,   s = a*a
     *     if (|y| > |x|) r = pi/2 - r
     *     if (x < 0)     r = pi - r
     *     if (y < 0)     r = -r
     *
     * The quadrant fixups are selects, not branches. `x == 0` falls out of the
     * reduction as pi/2 with the sign of `y`; only both zero needs the guard below,
     * which answers 0 as the reference does. The paths can differ only in the divide,
     * a reciprocal and a multiply good to one ULP; the tests allow 1e-5.
     */
    if (nm_is(nm, len, "atan") || nm_is(nm, len, "asin") || nm_is(nm, len, "acos")) {
        const GLboolean is_atan = nm_is(nm, len, "atan");
        if (is_atan ? (argc != 1 && argc != 2) : (argc != 1)) {
            return gen_fail(g, "wrong number of arguments", node);
        }
        const int n = arg[0].count;

        /* `asin(x)` is `atan2(x, sqrt(1 - x*x))` and `acos(x)` is `pi/2 - asin(x)`, as
         * `glsl_exec.c` defines them. The argument is clamped to [-1, 1] first, so an
         * out-of-range argument answers the endpoint as the reference does. */
        glsl_value_t y = arg[0];
        glsl_value_t x;
        glsl_value_t k_one = gen_const(g, 1.0, node);
        if (is_bad(k_one))
            return k_one;
        glsl_value_t k_neg1 = gen_const(g, -1.0, node);
        if (is_bad(k_neg1))
            return k_neg1;
        if (is_atan) {
            if (argc == 2) {
                if (arg[1].count != n) {
                    return gen_fail(g, "atan's two arguments have to be the same width",
                                    node);
                }
                x = arg[1];
            } else {
                /* One-argument `atan(y)` is `atan2(y, 1)`, as the reference does. */
                x = gen_alloc(g, n, node);
                if (is_bad(x))
                    return x;
                for (int c = 0; c < n; c++)
                    glsl_emit_mov(g->code, x.base + (uint32_t)c, k_one.base);
            }
        } else {
            glsl_value_t cl = gen_alloc(g, n, node);
            if (is_bad(cl))
                return cl;
            x = gen_alloc(g, n, node);
            if (is_bad(x))
                return x;
            for (int c = 0; c < n; c++) {
                const uint32_t d = cl.base + (uint32_t)c, q = x.base + (uint32_t)c;
                glsl_emit_vop2_op(g->code, GLSL_VOP2_MIN_F32, d, comp_of(arg[0], c),
                                  k_one.base);
                glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, d, d, k_neg1.base);
                /* 1 - x*x, which the clamp has just made non-negative. */
                glsl_emit_mul_f32(g->code, q, d, d);
                glsl_emit_sub_f32(g->code, q, k_one.base, q);
                glsl_emit_vop1_op(g->code, GLSL_VOP1_SQRT_F32, q, q);
            }
            y = cl;
        }

        glsl_value_t c3 = gen_const(g, -0.0464964749, node);
        if (is_bad(c3))
            return c3;
        glsl_value_t c2 = gen_const(g, 0.15931422, node);
        if (is_bad(c2))
            return c2;
        glsl_value_t c1 = gen_const(g, -0.327622764, node);
        if (is_bad(c1))
            return c1;
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return zero;
        glsl_value_t hpi = gen_const(g, 1.57079632679489661923, node);
        if (is_bad(hpi))
            return hpi;
        glsl_value_t pi = gen_const(g, 3.14159265358979323846, node);
        if (is_bad(pi))
            return pi;

        glsl_value_t out = gen_alloc(g, n, node);
        if (is_bad(out))
            return out;
        for (int c = 0; c < n; c++) {
            const uint32_t mark = gen_mark(g);
            glsl_value_t t = gen_alloc(g, 8, node);
            if (is_bad(t))
                return t;
            const uint32_t ax = t.base + 0u, ay = t.base + 1u, mn = t.base + 2u;
            const uint32_t mx = t.base + 3u, a = t.base + 4u, s = t.base + 5u;
            const uint32_t p = t.base + 6u, u = t.base + 7u;
            const uint32_t X = comp_of(x, c), Y = comp_of(y, c);
            const uint32_t r = out.base + (uint32_t)c;

            glsl_emit_neg_f32(g->code, ax, X);
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, ax, ax, X);
            glsl_emit_neg_f32(g->code, ay, Y);
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, ay, ay, Y);
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MIN_F32, mn, ax, ay);
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, mx, ax, ay);
            /* Both zero: `0 * rcp(0)` is NaN, so the answer is forced to 0 as the
             * reference gives. */
            glsl_emit_vop1_op(g->code, GLSL_VOP1_RCP_F32, a, mx);
            glsl_emit_mul_f32(g->code, a, mn, a);
            glsl_emit_cmp(g->code, GLSL_VOPC_NEQ_F32, mx, zero.base);
            glsl_emit_cndmask(g->code, a, zero.base, a);

            glsl_emit_mul_f32(g->code, s, a, a);
            glsl_emit_mov(g->code, p, c2.base);
            glsl_emit_fmac_f32(g->code, p, c3.base, s); /* c2 + c3 s */
            glsl_emit_mov(g->code, u, c1.base);
            glsl_emit_fmac_f32(g->code, u, p, s); /* c1 + (c2 + c3 s) s */
            glsl_emit_mul_f32(g->code, u, u, s);  /* ( ... ) s */
            glsl_emit_mov(g->code, r, a);
            glsl_emit_fmac_f32(g->code, r, u, a); /* a + ( ... ) s a */

            /* `|y| > |x|` reflects about pi/4, `x < 0` about pi/2, `y < 0` about zero,
             * in that order, each on the result of the one before. */
            glsl_emit_sub_f32(g->code, p, hpi.base, r);
            glsl_emit_cmp(g->code, GLSL_VOPC_GT_F32, ay, ax);
            glsl_emit_cndmask(g->code, r, r, p);
            glsl_emit_sub_f32(g->code, p, pi.base, r);
            glsl_emit_cmp(g->code, GLSL_VOPC_LT_F32, X, zero.base);
            glsl_emit_cndmask(g->code, r, r, p);
            glsl_emit_neg_f32(g->code, p, r);
            glsl_emit_cmp(g->code, GLSL_VOPC_LT_F32, Y, zero.base);
            glsl_emit_cndmask(g->code, r, r, p);
            gen_release(g, mark);
        }
        if (nm_is(nm, len, "acos")) {
            for (int c = 0; c < n; c++) {
                glsl_emit_sub_f32(g->code, out.base + (uint32_t)c, hpi.base,
                                  out.base + (uint32_t)c);
            }
        }
        return out;
    }
    return GEN_NOT_MINE;
}

/* `refract` and the matrix functions. */
static glsl_value_t gen_builtin_matrix(glsl_gen_t *g, const char *nm, size_t len,
                                       const glsl_value_t *arg, int argc, int w,
                                       int32_t first_arg, int32_t node) {
    (void)w;
    /* `refract`, the specification's formula:
     *
     *     k = 1 - eta^2 (1 - dot(N, I)^2)
     *     k < 0  ->  the zero vector          (total internal reflection)
     *     else   ->  eta I - (eta dot(N, I) + sqrt(k)) N
     *
     * Both arms are computed and a `v_cndmask` on the sign of `k` picks, discarding the
     * NaN `sqrt(k)` gives for a negative `k`.
     */
    if (nm_is(nm, len, "refract")) {
        if (argc != 3)
            return gen_fail(g, "refract takes I, N and eta", node);
        const int n = arg[0].count;
        if (arg[1].count != n) {
            return gen_fail(g, "refract's I and N have to be the same width", node);
        }
        if (arg[2].count != 1)
            return gen_fail(g, "refract's eta is a scalar", node);
        glsl_value_t d = gen_dot(g, arg[1], arg[0], node); /* dot(N, I) */
        if (is_bad(d))
            return d;
        glsl_value_t one = gen_const(g, 1.0, node);
        if (is_bad(one))
            return one;
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return zero;
        glsl_value_t t = gen_alloc(g, 3, node);
        if (is_bad(t))
            return t;
        const uint32_t e2 = t.base + 0u, k = t.base + 1u, sc = t.base + 2u;
        const uint32_t eta = arg[2].base;
        glsl_emit_mul_f32(g->code, e2, eta, eta);      /* eta^2 */
        glsl_emit_mul_f32(g->code, k, d.base, d.base); /* d^2 */
        glsl_emit_sub_f32(g->code, k, one.base, k);    /* 1 - d^2 */
        glsl_emit_mul_f32(g->code, k, e2, k);          /* eta^2 (1 - d^2) */
        glsl_emit_sub_f32(g->code, k, one.base, k);    /* k */
        glsl_emit_vop1_op(g->code, GLSL_VOP1_SQRT_F32, sc, k);
        glsl_emit_fmac_f32(g->code, sc, eta, d.base); /* sqrt(k) + eta d */
        glsl_value_t out = gen_alloc(g, n, node);
        if (is_bad(out))
            return out;
        /* `k >= 0` once, outside the loop: nothing in it disturbs `vcc`, and total
         * internal reflection is a property of the ray, not of a component. */
        glsl_emit_cmp(g->code, GLSL_VOPC_GE_F32, k, zero.base);
        for (int c = 0; c < n; c++) {
            const uint32_t mark = gen_mark(g);
            glsl_value_t v = gen_alloc(g, 1, node);
            if (is_bad(v))
                return v;
            glsl_emit_mul_f32(g->code, v.base, eta, comp_of(arg[0], c)); /* eta I */
            glsl_emit_neg_f32(g->code, out.base + (uint32_t)c, sc);
            glsl_emit_fmac_f32(g->code, v.base, out.base + (uint32_t)c,
                               comp_of(arg[1], c)); /* - ( ... ) N */
            glsl_emit_cndmask(g->code, out.base + (uint32_t)c, zero.base, v.base);
            gen_release(g, mark);
        }
        return out;
    }

    /* The matrix built-ins are moves and multiplies: `matrixCompMult` multiplies
     * element-wise, `transpose` moves registers, `outerProduct` multiplies per element.
     * Each destination is freshly allocated, which `transpose` relies on to not read
     * back what it has written. */
    if (nm_is(nm, len, "matrixCompMult")) {
        if (argc != 2)
            return gen_fail(g, "wrong number of arguments", node);
        if (arg[0].count != arg[1].count) {
            return gen_fail(g, "matrixCompMult takes two matrices of the same size",
                            node);
        }
        glsl_value_t d = gen_alloc(g, arg[0].count, node);
        if (is_bad(d))
            return d;
        for (int i = 0; i < arg[0].count; i++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)i, arg[0].base + (uint32_t)i,
                              arg[1].base + (uint32_t)i);
        }
        return d;
    }

    /* Element `(col, row)` is `v[col * rows + row]`. `matCxR` transposes to `matRxC`,
     * whose columns are `C` long, so the destination's stride is the source's column
     * count. The type decides, not the width: a `vec4` is also four registers. */
    if (nm_is(nm, len, "transpose")) {
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        const glsl_type_t mt = glsl_type_of(g->sema, first_arg);
        const int cols = mat_cols(mt), rows = mat_rows(mt);
        if (cols == 0)
            return gen_fail(g, "transpose takes a matrix", node);
        glsl_value_t d = gen_alloc(g, cols * rows, node);
        if (is_bad(d))
            return d;
        for (int col = 0; col < rows; col++) {
            for (int row = 0; row < cols; row++) {
                glsl_emit_mov(g->code, d.base + (uint32_t)(col * cols + row),
                              arg[0].base + (uint32_t)(row * rows + col));
            }
        }
        return d;
    }

    /* `outerProduct(c, r)` has `c` down the columns and `r` across them: element
     * `(col, row)` is `c[row] * r[col]`. `outerProduct(vecR, vecC)` is a `matCxR`. */
    if (nm_is(nm, len, "outerProduct")) {
        if (argc != 2)
            return gen_fail(g, "wrong number of arguments", node);
        const int rows = arg[0].count, cols = arg[1].count;
        if (rows < 2 || rows > 4 || cols < 2 || cols > 4) {
            return gen_fail(g, "outerProduct takes two vectors", node);
        }
        glsl_value_t d = gen_alloc(g, cols * rows, node);
        if (is_bad(d))
            return d;
        for (int col = 0; col < cols; col++) {
            for (int row = 0; row < rows; row++) {
                glsl_emit_mul_f32(g->code, d.base + (uint32_t)(col * rows + row),
                                  arg[0].base + (uint32_t)row,
                                  arg[1].base + (uint32_t)col);
            }
        }
        return d;
    }
    return GEN_NOT_MINE;
}

/* `length`, `distance`, `normalize`, `cross`, `reflect` and `faceforward`. */
static glsl_value_t gen_builtin_geometric(glsl_gen_t *g, const char *nm, size_t len,
                                          const glsl_value_t *arg, int argc, int w,
                                          int32_t first_arg, int32_t node) {
    (void)first_arg;
    if (nm_is(nm, len, "length")) {
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d2 = gen_dot(g, arg[0], arg[0], node);
        if (is_bad(d2))
            return d2;
        glsl_value_t d = gen_alloc(g, 1, node);
        if (is_bad(d))
            return d;
        glsl_emit_vop1_op(g->code, GLSL_VOP1_SQRT_F32, d.base, d2.base);
        return d;
    }

    if (nm_is(nm, len, "distance")) {
        if (argc != 2)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t diff = gen_alloc(g, w, node);
        if (is_bad(diff))
            return diff;
        for (int c = 0; c < w; c++) {
            glsl_emit_sub_f32(g->code, diff.base + (uint32_t)c, comp_of(arg[0], c),
                              comp_of(arg[1], c));
        }
        glsl_value_t d2 = gen_dot(g, diff, diff, node);
        if (is_bad(d2))
            return d2;
        glsl_value_t d = gen_alloc(g, 1, node);
        if (is_bad(d))
            return d;
        glsl_emit_vop1_op(g->code, GLSL_VOP1_SQRT_F32, d.base, d2.base);
        return d;
    }

    /* `normalize(v) = v * inversesqrt(dot(v, v))`: `v_rsq_f32` and a multiply. The
     * reciprocal square root is accurate to 1 ULP, so the length is within a couple of
     * ULP of one. */
    if (nm_is(nm, len, "normalize")) {
        if (argc != 1)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t s = gen_inv_length(g, arg[0], node);
        if (is_bad(s))
            return s;
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c),
                              s.base);
        }
        return d;
    }

    /* `cross(a, b)`, three components and six multiplies, the index pattern in `L` and
     * `R`. */
    if (nm_is(nm, len, "cross")) {
        if (argc != 2 || arg[0].count != 3 || arg[1].count != 3) {
            return gen_fail(g, "cross takes two vec3", node);
        }
        glsl_value_t d = gen_alloc(g, 3, node);
        if (is_bad(d))
            return d;
        glsl_value_t t = gen_alloc(g, 3, node);
        if (is_bad(t))
            return t;
        static const int L[3] = {1, 2, 0};
        static const int R[3] = {2, 0, 1};
        for (int c = 0; c < 3; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c,
                              arg[0].base + (uint32_t)L[c],
                              arg[1].base + (uint32_t)R[c]);
            glsl_emit_mul_f32(g->code, t.base + (uint32_t)c,
                              arg[0].base + (uint32_t)R[c],
                              arg[1].base + (uint32_t)L[c]);
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)c, d.base + (uint32_t)c,
                              t.base + (uint32_t)c);
        }
        return d;
    }

    /* `reflect(I, N) = I - 2 * dot(N, I) * N`, GLSL 1.10 8.4, with `N` assumed
     * normalised. */
    if (nm_is(nm, len, "reflect")) {
        if (argc != 2)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t dp = gen_dot(g, arg[1], arg[0], node);
        if (is_bad(dp))
            return dp;
        glsl_value_t two = gen_const(g, 2.0, node);
        if (is_bad(two))
            return two;
        glsl_emit_mul_f32(g->code, dp.base, dp.base, two.base);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, comp_of(arg[1], c),
                              dp.base);
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c),
                              d.base + (uint32_t)c);
        }
        return d;
    }

    /* `faceforward(N, I, Nref)` is `N` when `dot(Nref, I)` is negative and `-N`
     * otherwise. One comparison for the whole vector, then a select a component. */
    if (nm_is(nm, len, "faceforward")) {
        if (argc != 3)
            return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t dp = gen_dot(g, arg[2], arg[1], node);
        if (is_bad(dp))
            return dp;
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return zero;
        glsl_value_t neg = gen_alloc(g, w, node);
        if (is_bad(neg))
            return neg;
        for (int c = 0; c < w; c++) {
            glsl_emit_neg_f32(g->code, neg.base + (uint32_t)c, comp_of(arg[0], c));
        }
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d))
            return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_cmp(g->code, GLSL_VOPC_LT_F32, dp.base, zero.base);
            glsl_emit_cndmask(g->code, d.base + (uint32_t)c, neg.base + (uint32_t)c,
                              comp_of(arg[0], c));
        }
        return d;
    }
    return GEN_NOT_MINE;
}

/* -------------------------------------------------------------------------
 * User-defined functions, which are inlined
 *
 * GLSL forbids recursion, so every call is generated where it appears; there is no call
 * instruction and the bump allocator needs no frame. A value-returning body ends in its
 * `return`; an early return runs the rest of the body under a narrowed exec mask.
 * ------------------------------------------------------------------------- */

/* The function of this name with a body, or nothing. The AST is a flat array, so this
 * reads it directly. A prototype has no body and is skipped. */
static int32_t gen_find_function(const glsl_gen_t *g, const char *name, size_t length) {
    for (int32_t i = 0; i < g->ast->count; i++) {
        const glsl_node_t *n = &g->ast->nodes[i];
        if (n->kind != GLSL_NODE_FUNCTION || n->c == GLSL_NO_NODE)
            continue;
        if (n->length != length)
            continue;
        GLboolean same = GL_TRUE;
        for (size_t c = 0; c < length; c++) {
            if (n->text[c] != name[c]) {
                same = GL_FALSE;
                break;
            }
        }
        if (same)
            return i;
    }
    return GLSL_NO_NODE;
}

/* The last statement of a body, so the trailing `return` can be found without
 * generating it twice - and so a body whose last statement is not one is refused before
 * anything is emitted. */
static int32_t gen_last_stmt(const glsl_gen_t *g, int32_t compound) {
    if (compound == GLSL_NO_NODE)
        return GLSL_NO_NODE;
    int32_t last = GLSL_NO_NODE;
    for (int32_t s = g->ast->nodes[compound].a; s != GLSL_NO_NODE;
         s = g->ast->nodes[s].sibling) {
        last = s;
    }
    return last;
}

/* Whether anything in this statement is an early `return`: the caller passes the body's
 * trailing return as `skip`. A call node names a function rather than containing its
 * body, so a callee's returns are not counted. Siblings are followed in `{ ... }` only,
 * for the reason `has_loop_flow` gives. */
static GLboolean has_early_return(const glsl_gen_t *g, int32_t node, int32_t skip) {
    if (node == GLSL_NO_NODE || node == skip)
        return GL_FALSE;
    const glsl_node_t *n = &g->ast->nodes[node];
    if (n->kind == GLSL_NODE_RETURN)
        return GL_TRUE;
    if (n->kind == GLSL_NODE_COMPOUND) {
        for (int32_t s = n->a; s != GLSL_NO_NODE; s = g->ast->nodes[s].sibling) {
            if (has_early_return(g, s, skip))
                return GL_TRUE;
        }
        return GL_FALSE;
    }
    return (
        GLboolean)(has_early_return(g, n->a, skip) || has_early_return(g, n->b, skip) ||
                   has_early_return(g, n->c, skip) || has_early_return(g, n->d, skip));
}

static glsl_value_t gen_call_user(glsl_gen_t *g, int32_t fn_node, int32_t first_arg,
                                  int32_t node) {
    const glsl_node_t *fn = &g->ast->nodes[fn_node];

    if (g->inline_depth >= GLSL_GEN_MAX_INLINE_DEPTH) {
        return gen_fail(
            g,
            "user-defined calls nest deeper than this generator inlines; GLSL "
            "forbids recursion, so a shader that reaches this is calling itself",
            node);
    }

    const glsl_type_t ret = gen_node_type(g, fn);
    const GLboolean is_void = (GLboolean)(ret == GLSL_TYPE_VOID);
    if (!is_void && !is_generated(ret)) {
        return gen_fail(g,
                        "a function is generated only when it returns void, a float, a "
                        "vector, a matrix or a bool",
                        node);
    }

    /* A value-returning body has to end in its return, checked before anything is
     * emitted. A `void` body may end in anything, including a bare `return;`, which
     * generates nothing. */
    int32_t last = gen_last_stmt(g, fn->c);
    if (is_void) {
        if (last != GLSL_NO_NODE && g->ast->nodes[last].kind == GLSL_NODE_RETURN &&
            g->ast->nodes[last].a != GLSL_NO_NODE) {
            return gen_fail(g, "a void function returns a value", node);
        }
        /* A trailing bare `return;` is skipped rather than generated; anything else is
         * body. */
        if (last != GLSL_NO_NODE && g->ast->nodes[last].kind != GLSL_NODE_RETURN) {
            last =
                GLSL_NO_NODE; /* nothing to hold back - every statement is generated */
        }
    } else if (last == GLSL_NO_NODE || g->ast->nodes[last].kind != GLSL_NODE_RETURN ||
               g->ast->nodes[last].a == GLSL_NO_NODE) {
        /* The trailing return catches every lane no early return took. GLSL requires
         * every path to return, and a body not ending in one would need proving
         * exhaustive. */
        return gen_fail(
            g,
            "a function that returns a value has to end in `return <expr>;` - "
            "returns earlier in the body are generated, but one lane in every "
            "quad may reach the end and there has to be a value for it",
            node);
    }

    /* The arguments are evaluated in the caller's scope, before the parameters shadow
     * anything. */
    glsl_value_t argv[GEN_MAX_ARGS];
    int argc = 0;
    for (int32_t a = first_arg; a != GLSL_NO_NODE; a = g->ast->nodes[a].sibling) {
        if (argc >= GEN_MAX_ARGS) {
            return gen_fail(g, "more arguments than this generator carries", node);
        }
        /* An array argument is passed as the run of registers behind its name, the
         * exception GLSL 1.10 6.1 makes to a whole array not being a value; `gen_expr`
         * refuses it everywhere else. */
        const glsl_node_t *an = &g->ast->nodes[a];
        glsl_gen_var_t *av = (an->kind == GLSL_NODE_IDENTIFIER)
                                 ? gen_find(g, an->text, an->length)
                                 : (glsl_gen_var_t *)0;
        if (av && av->array_size > 0) {
            argv[argc] = av->value;
            argv[argc].count = av->value.count * av->array_size;
        } else {
            argv[argc] = gen_expr(g, a);
            if (is_bad(argv[argc]))
                return argv[argc];
        }
        argc++;
    }

    glsl_value_t out;
    out.base = 0u;
    out.count = 0;
    if (!is_void) {
        out = gen_alloc(g, gen_comps(g, ret), node);
        if (is_bad(out))
            return out;
    }

    /* Everything the call allocates from here, its parameters and its body's
     * temporaries, is given back at the end, so call sites do not accumulate registers.
     * The result is allocated before this mark so it survives. */
    const uint32_t call_mark = gen_mark(g);

    const int vars_before = g->var_count;
    glsl_scope_push(g->sema);

    /* A body with an early `return` runs under a mask; one without does not. The exec
     * at the call is saved and restored when the function ends, since a return stops
     * only the rest of its own body. A call with no early return emits no mask
     * instructions. */
    const int d = g->inline_depth;
    const GLboolean early = has_early_return(g, fn->c, last);
    if (early && g->exec_depth >= GLSL_GEN_MAX_EXEC_DEPTH) {
        (void)gen_fail(
            g,
            "the conditionals and inlined calls in this shader nest deeper than "
            "the scalar registers set aside for them",
            node);
    }
    const uint32_t fn_saved_sgpr = GLSL_GEN_EXEC_SGPR_BASE + (uint32_t)g->exec_depth;
    g->fn_saved[d] = (GLboolean)(early && !g->error);
    if (g->fn_saved[d]) {
        glsl_emit_exec_save(g->code, fn_saved_sgpr);
        g->exec_depth++;
    }
    g->fn_out[d] = out;
    g->fn_exec_depth[d] = g->exec_depth;
    g->fn_loop_depth[d] = g->loop_depth;
    g->inline_depth++;

    /* Every parameter is a copy, as GLSL says: `out` and `inout` are copied back at the
     * return, never passed by reference, so `swap(p, p)` does not alias. The copy-back
     * place comes from `gen_place_of`, resolved here, before the parameters shadow
     * anything, and applied after the body. */
    int bound = 0;
    gen_place_t writeback[GEN_MAX_ARGS];
    glsl_value_t writeback_from[GEN_MAX_ARGS];
    int writebacks = 0;
    int32_t argn[GEN_MAX_ARGS];
    {
        int k = 0;
        for (int32_t a = first_arg; a != GLSL_NO_NODE && k < GEN_MAX_ARGS;
             a = g->ast->nodes[a].sibling) {
            argn[k++] = a;
        }
    }
    for (int32_t p = fn->b; p != GLSL_NO_NODE; p = g->ast->nodes[p].sibling) {
        const glsl_node_t *pn = &g->ast->nodes[p];
        if (bound >= argc) {
            (void)gen_fail(
                g, "this call passes fewer arguments than the function takes", node);
            break;
        }
        const GLboolean writes_back = (GLboolean)(pn->qualifier == GLSL_TOK_KW_OUT ||
                                                  pn->qualifier == GLSL_TOK_KW_INOUT);
        const glsl_type_t pt = gen_node_type(g, pn);
        if (!is_generated(pt)) {
            (void)gen_fail(
                g, "only float, vec, mat, bool and struct parameters are generated",
                node);
            break;
        }
        /* An array parameter's width is its elements together; the copy below moves
         * the caller's whole run. */
        int pelems = 0;
        if (pn->array_size != GLSL_NO_NODE) {
            double sz = 0.0;
            if (!const_of(g, pn->array_size, &sz) || sz < 1.0 ||
                (double)(int)sz != sz) {
                (void)gen_fail(g, "an array parameter's length has to be a constant",
                               node);
                break;
            }
            pelems = (int)sz;
            /* Not `out` or `inout`: the copy back needs the argument to be a place, and
             * a whole array is not an l-value in GLSL 1.10 (5.8). */
            if (writes_back) {
                (void)gen_fail(
                    g,
                    "an array parameter is `in` here: copying one back needs the "
                    "argument to be assignable, and a whole array is not",
                    node);
                break;
            }
        }
        const int pwidth = gen_comps(g, pt) * ((pelems > 0) ? pelems : 1);
        if (pwidth != argv[bound].count) {
            (void)gen_fail(
                g, "an argument is a different width from the parameter it binds",
                node);
            break;
        }
        const glsl_value_t home = gen_alloc(g, argv[bound].count, node);
        if (is_bad(home))
            break;
        /* An `out` parameter starts undefined, but the argument is copied in anyway so
         * a body that reads before writing matches the reference. */
        for (int c = 0; c < home.count; c++) {
            glsl_emit_mov(g->code, home.base + (uint32_t)c,
                          argv[bound].base + (uint32_t)c);
        }
        if (writes_back) {
            /* The argument has to be a place, as for an assignment; a literal or an
             * expression is not one. */
            if (!gen_place_of(g, argn[bound], &writeback[writebacks])) {
                (void)gen_fail(
                    g,
                    "an `out` or `inout` argument has to be something that can be "
                    "assigned to",
                    node);
                break;
            }
            if (writeback[writebacks].count != home.count) {
                (void)gen_fail(
                    g, "an `out` argument is a different width from its parameter",
                    node);
                break;
            }
            writeback_from[writebacks] = home;
            writebacks++;
        }
        if (pelems > 0) {
            /* The run is the array; `value.count` stays the element width so every
             * other reader sees one element. */
            glsl_value_t elem = home;
            elem.count = gen_comps(g, pt);
            glsl_gen_var_t *pv = gen_declare(g, pn->text, pn->length, pt, elem, node);
            if (!pv)
                break;
            pv->array_size = pelems;
            if (!glsl_declare_array(g->sema, pn->text, pn->length, pt, pelems,
                                    (glsl_token_type_t)0)) {
                g->sema->error = (const char *)0;
            }
            bound++;
            continue;
        }
        if (!gen_declare(g, pn->text, pn->length, pt, home, node))
            break;
        if (!glsl_declare(g->sema, pn->text, pn->length, pt, GL_FALSE)) {
            g->sema->error = (const char *)0;
        }
        bound++;
    }
    if (!g->error && bound != argc) {
        (void)gen_fail(g, "this call passes more arguments than the function takes",
                       node);
    }

    /* The body, every statement but the trailing return, which is generated here into
     * the caller's result register. */
    if (!g->error) {
        for (int32_t s = g->ast->nodes[fn->c].a; s != GLSL_NO_NODE && !g->error;
             s = g->ast->nodes[s].sibling) {
            if (s == last)
                break;
            (void)glsl_gen_stmt(g, s);
        }
    }
    if (!g->error && !is_void) {
        const uint32_t mark = gen_mark(g);
        const glsl_value_t rv = gen_expr(g, g->ast->nodes[last].a);
        if (!is_bad(rv)) {
            if (rv.count != out.count) {
                (void)gen_fail(g,
                               "the returned expression is a different width from the "
                               "function's return type",
                               node);
            } else {
                for (int c = 0; c < out.count; c++) {
                    glsl_emit_mov(g->code, out.base + (uint32_t)c,
                                  rv.base + (uint32_t)c);
                }
            }
        }
        gen_release(g, mark);
    }

    /* Every lane back before the copy-back, which must run for the lanes that returned
     * early too; the caller then resumes with the mask it had. */
    if (g->fn_saved[d]) {
        glsl_emit_exec_restore(g->code, fn_saved_sgpr);
        g->exec_depth--;
    }

    /* The copy-back, after the body and before the parameters go out of scope. The
     * places were resolved in the caller's scope, so they name the caller's registers.
     */
    for (int i = 0; !g->error && i < writebacks; i++) {
        for (int c = 0; c < writeback[i].count; c++) {
            glsl_emit_mov(g->code, writeback[i].reg[c],
                          writeback_from[i].base + (uint32_t)c);
        }
    }

    g->inline_depth--;
    glsl_scope_pop(g->sema);
    g->var_count = vars_before;
    /* After the writeback, which read the parameters' registers, and after the return
     * move, which wrote the caller's. Nothing below is live. */
    gen_release(g, call_mark);
    if (g->error) {
        glsl_value_t none;
        none.base = 0u;
        none.count = 0;
        return none;
    }
    return out;
}

static glsl_value_t gen_expr(glsl_gen_t *g, int32_t node) {
    if (g->error) {
        glsl_value_t none;
        none.base = 0u;
        none.count = 0;
        return none;
    }
    if (node == GLSL_NO_NODE)
        return gen_fail(g, "an expression is missing", node);

    const glsl_node_t *n = &g->ast->nodes[node];
    switch (n->kind) {
    case GLSL_NODE_FLOATCONST: {
        glsl_value_t out = gen_alloc(g, 1, node);
        if (is_bad(out))
            return out;
        glsl_emit_mov_imm(g->code, out.base, float_bits(n->value));
        return out;
    }
    case GLSL_NODE_INTCONST: {
        /* An integer literal is its exact float value, in an `int` or float context
         * alike. */
        const glsl_type_t t = glsl_type_of(g->sema, node);
        if (t != GLSL_TYPE_INT && t != GLSL_TYPE_FLOAT) {
            return gen_fail(g, "unexpected type for an integer literal", node);
        }
        glsl_value_t out = gen_alloc(g, 1, node);
        if (is_bad(out))
            return out;
        glsl_emit_mov_imm(g->code, out.base, float_bits(n->value));
        return out;
    }
    case GLSL_NODE_IDENTIFIER: {
        glsl_gen_var_t *v = gen_find(g, n->text, n->length);
        if (!v) {
            /* A built-in constant (`gl_MaxDrawBuffers` and its relatives, `const int`
             * in GLSL 7.4) becomes an immediate; `const_of` answers for them too, so
             * one may be an array's length or a loop's bound. */
            int bi = 0;
            if (glsl_builtin_const_int(n->text, n->length, &bi)) {
                glsl_value_t out = gen_alloc(g, 1, node);
                if (is_bad(out))
                    return out;
                glsl_emit_mov_imm(g->code, out.base, float_bits((float)bi));
                return out;
            }
            return gen_fail(g,
                            "this name has no register: only locals declared in this "
                            "body are generated so far",
                            node);
        }
        if (v->array_size > 0) {
            /* A whole array reads as its whole run, for GLSL 1.20's `=` and `==`; sema
             * has refused every other context. `a[k]` goes through `gen_index_of`
             * instead. */
            glsl_value_t whole = v->value;
            whole.count = v->value.count * v->array_size;
            return whole;
        }
        return v->value;
    }
    /* Reading `a[k]` hands back the element's own registers, not a copy, as a variable
     * read does. */
    case GLSL_NODE_INDEX: {
        glsl_value_t elem;
        if (!gen_index_of(g, node, &elem)) {
            glsl_value_t none;
            none.base = 0u;
            none.count = 0;
            return none;
        }
        return elem;
    }
    case GLSL_NODE_FIELD:
        return gen_field(g, node);
    case GLSL_NODE_BINARY:
        return gen_binary(g, node);
    case GLSL_NODE_BOOLCONST: {
        glsl_value_t out = gen_alloc(g, 1, node);
        if (is_bad(out))
            return out;
        glsl_emit_mov_imm(g->code, out.base, float_bits(n->value != 0.0 ? 1.0 : 0.0));
        return out;
    }
    case GLSL_NODE_CONDITIONAL: {
        /* `c ? a : b`, both arms evaluated and one selected, so neither arm may
         * assign. */
        if (has_side_effect(g->ast, n->b) || has_side_effect(g->ast, n->c)) {
            return gen_fail(
                g,
                "an arm of this ?: assigns, and both arms are evaluated - so "
                "it would run when the language says it does not",
                node);
        }
        glsl_value_t c = gen_expr(g, n->a);
        if (is_bad(c))
            return c;
        if (c.count != 1)
            return gen_fail(g, "a ?: takes a single condition", node);
        glsl_value_t t = gen_expr(g, n->b);
        if (is_bad(t))
            return t;
        glsl_value_t f = gen_expr(g, n->c);
        if (is_bad(f))
            return f;
        if (t.count != f.count) {
            return gen_fail(g, "the two arms of this ?: are different widths", node);
        }
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return zero;
        glsl_value_t out = gen_alloc(g, t.count, node);
        if (is_bad(out))
            return out;
        for (int i = 0; i < t.count; i++) {
            glsl_emit_cmp(g->code, GLSL_VOPC_NEQ_F32, c.base, zero.base);
            /* The false arm is `src0`. */
            glsl_emit_cndmask(g->code, out.base + (uint32_t)i, f.base + (uint32_t)i,
                              t.base + (uint32_t)i);
        }
        return out;
    }
    case GLSL_NODE_UNARY: {
        if (n->op == GLSL_TOK_PLUS)
            return gen_expr(g, n->a);
        if (n->op == GLSL_TOK_BANG) {
            /* `!b` over a value that is 0.0 or 1.0 is `1 - b`. */
            glsl_value_t s = gen_expr(g, n->a);
            if (is_bad(s))
                return s;
            if (s.count != 1)
                return gen_fail(g, "! takes a single bool", node);
            glsl_value_t one = gen_const(g, 1.0, node);
            if (is_bad(one))
                return one;
            glsl_value_t out = gen_alloc(g, 1, node);
            if (is_bad(out))
                return out;
            glsl_emit_sub_f32(g->code, out.base, one.base, s.base);
            return out;
        }
        if (n->op == GLSL_TOK_INC || n->op == GLSL_TOK_DEC) {
            return gen_inc_dec(g, node, GL_FALSE);
        }
        if (n->op != GLSL_TOK_MINUS) {
            return gen_fail(g, "this unary operator has no verified instruction yet",
                            node);
        }
        const glsl_type_t t = glsl_type_of(g->sema, n->a);
        /* Negating a whole float leaves it whole, so an `int` needs no truncation. A
         * `bool` has no negation in GLSL. */
        if (!is_float_family(t) && !is_int_family(t)) {
            return gen_fail(g,
                            "unary minus is generated for the float and int families "
                            "only",
                            node);
        }
        glsl_value_t s = gen_expr(g, n->a);
        if (is_bad(s))
            return s;
        glsl_value_t out = gen_alloc(g, s.count, node);
        if (is_bad(out))
            return out;
        for (int i = 0; i < s.count; i++) {
            glsl_emit_neg_f32(g->code, out.base + (uint32_t)i, s.base + (uint32_t)i);
        }
        return out;
    }
    case GLSL_NODE_CALL: {
        const glsl_node_t *callee = &g->ast->nodes[n->a];
        if (callee->kind != GLSL_NODE_IDENTIFIER) {
            return gen_fail(g, "calling something that is not a name", node);
        }
        const glsl_type_t target = constructor_target(callee);
        if (target != GLSL_TYPE_ERROR)
            return gen_construct(g, target, n->b, node);

        /* A struct constructor moves one argument to each member's place in the run.
         * Sema has checked the count and the types. */
        {
            const glsl_type_t st_type =
                gen_struct_by_name(g, callee->text, callee->length);
            const glsl_struct_t *st = glsl_struct_of(g->sema, st_type);
            if (st) {
                glsl_value_t out = gen_alloc(g, st->components, node);
                if (is_bad(out))
                    return out;
                int i = 0;
                for (int32_t a = n->b; a != GLSL_NO_NODE && i < st->member_count;
                     a = g->ast->nodes[a].sibling, i++) {
                    const uint32_t mark = gen_mark(g);
                    glsl_value_t av = gen_expr(g, a);
                    if (is_bad(av))
                        return av;
                    const int w = gen_comps(g, st->member[i].type);
                    if (av.count != w) {
                        return gen_fail(g,
                                        "this argument is a different width from the "
                                        "struct's member",
                                        a);
                    }
                    for (int k = 0; k < w; k++) {
                        glsl_emit_mov(g->code,
                                      out.base + (uint32_t)(st->member[i].offset + k),
                                      av.base + (uint32_t)k);
                    }
                    gen_release(g, mark);
                }
                return out;
            }
        }
        /* A function this shader defines hides a built-in of the same name. */
        {
            /* The overload the semantic pass chose, or the name when it did not run. */
            const int32_t fn = (n->resolved != GLSL_NO_NODE)
                                   ? n->resolved
                                   : gen_find_function(g, callee->text, callee->length);
            if (fn != GLSL_NO_NODE)
                return gen_call_user(g, fn, n->b, node);
        }
        /* Not a type name and not defined here, so a built-in or a refusal. */
        return gen_builtin(g, callee, n->b, node);
    }
    /* `++i`, `--i`, `i++` and `i--` are a float add of 1.0 and a write back, an int
     * being a whole float. Prefix hands back the changed register, postfix a copy taken
     * before the change; the per-statement rewind reclaims an unused copy. */
    case GLSL_NODE_POSTFIX:
        return gen_inc_dec(g, node, GL_TRUE);
    case GLSL_NODE_ASSIGN: {
        gen_place_t place;
        if (!gen_place_of(g, n->a, &place)) {
            glsl_value_t none;
            none.base = 0u;
            none.count = 0;
            return none;
        }
        glsl_value_t r = gen_expr(g, n->b);
        if (is_bad(r))
            return r;
        /* GLSL takes a scalar on the right of any of these (`c.rgb *= 0.5`), and
         * nothing else of a different width. */
        if (r.count != place.count && r.count != 1) {
            return gen_fail(g, "the two sides of this assignment are different widths",
                            node);
        }

        switch (n->op) {
        case GLSL_TOK_ASSIGN:
            for (int i = 0; i < place.count; i++) {
                glsl_emit_mov(g->code, place.reg[i], comp_of(r, i));
            }
            break;
        case GLSL_TOK_ADD_ASSIGN:
        case GLSL_TOK_SUB_ASSIGN:
        case GLSL_TOK_MUL_ASSIGN: {
            /* The destination is its own first operand: one instruction a component. */
            const glsl_token_type_t op = n->op == GLSL_TOK_ADD_ASSIGN   ? GLSL_TOK_PLUS
                                         : n->op == GLSL_TOK_SUB_ASSIGN ? GLSL_TOK_MINUS
                                                                        : GLSL_TOK_STAR;
            for (int i = 0; i < place.count; i++) {
                gen_binop_component(g, op, place.reg[i], place.reg[i], comp_of(r, i));
            }
            break;
        }
        case GLSL_TOK_DIV_ASSIGN: {
            /* One reciprocal a divisor component, as `/` does, into a temporary because
             * the divisor may be one of the destination's own registers. */
            glsl_value_t inv = gen_alloc(g, r.count, node);
            if (is_bad(inv))
                return inv;
            for (int i = 0; i < r.count; i++) {
                glsl_emit_vop1_op(g->code, GLSL_VOP1_RCP_F32, inv.base + (uint32_t)i,
                                  r.base + (uint32_t)i);
            }
            for (int i = 0; i < place.count; i++) {
                glsl_emit_mul_f32(g->code, place.reg[i], place.reg[i], comp_of(inv, i));
            }
            break;
        }
        default:
            return gen_fail(g,
                            "this assignment operator has no instruction selection "
                            "yet",
                            node);
        }

        /* The value of an assignment is what was assigned. A place whose registers run
         * consecutively is already a value; anything else (`v.zyx = ...`) is copied
         * into one. */
        GLboolean consecutive = GL_TRUE;
        for (int i = 1; i < place.count; i++) {
            if (place.reg[i] != place.reg[i - 1] + 1u) {
                consecutive = GL_FALSE;
                break;
            }
        }
        if (consecutive) {
            glsl_value_t out;
            out.base = place.reg[0];
            out.count = place.count;
            return out;
        }
        glsl_value_t out = gen_alloc(g, place.count, node);
        if (is_bad(out))
            return out;
        for (int i = 0; i < place.count; i++) {
            glsl_emit_mov(g->code, out.base + (uint32_t)i, place.reg[i]);
        }
        return out;
    }
    default:
        return gen_fail(g, "this expression has no instruction selection yet", node);
    }
}

/* -------------------------------------------------------------------------
 * Statements
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Loops
 *
 * A `for` whose trip count is known at compile time is unrolled, the induction variable
 * a constant in each copy, so the body is straight-line code. Other loops branch, with
 * a trip guard, because a loop whose condition never goes false on some lane hangs the
 * GPU.
 * ------------------------------------------------------------------------- */

/* Whether `node` is the identifier `name`. */
static GLboolean is_name(const glsl_gen_t *g, int32_t node, const char *name,
                         size_t len) {
    if (node == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *n = &g->ast->nodes[node];
    if (n->kind != GLSL_NODE_IDENTIFIER || n->length != len)
        return GL_FALSE;
    for (size_t i = 0; i < len; i++) {
        if (n->text[i] != name[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

#define GLSL_GEN_MAX_UNROLL 64

/* Whether this statement, or anything inside it, is a `break` or a `continue` belonging
 * to it. A nested loop stops the search, since its `break` is its own. Only `{ ... }`
 * walks a sibling chain: elsewhere following siblings would leave this loop for the
 * statements after it. */
static GLboolean has_loop_flow(const glsl_gen_t *g, int32_t node) {
    if (node == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *n = &g->ast->nodes[node];
    if (n->kind == GLSL_NODE_BREAK || n->kind == GLSL_NODE_CONTINUE)
        return GL_TRUE;
    if (n->kind == GLSL_NODE_FOR || n->kind == GLSL_NODE_WHILE)
        return GL_FALSE;
    if (n->kind == GLSL_NODE_COMPOUND) {
        for (int32_t s = n->a; s != GLSL_NO_NODE; s = g->ast->nodes[s].sibling) {
            if (has_loop_flow(g, s))
                return GL_TRUE;
        }
        return GL_FALSE;
    }
    return (GLboolean)(has_loop_flow(g, n->a) || has_loop_flow(g, n->b) ||
                       has_loop_flow(g, n->c) || has_loop_flow(g, n->d));
}

/* Whether anything in this statement assigns to `name` - by `=`, by a compound
 * assignment, or by `++`/`--`. A body that moves the counter invalidates the static
 * trip count both counted paths rely on, silently, so it is refused.
 *
 * Siblings are followed in `{ ... }` only, for the reason `has_loop_flow` gives. It is
 * deliberately blunt: a nested loop declaring its own `i` is reported too, refusing a
 * valid shader rather than miscompiling one. */
static GLboolean assigns_name(const glsl_gen_t *g, int32_t node, const char *name,
                              size_t len) {
    if (node == GLSL_NO_NODE)
        return GL_FALSE;
    const glsl_node_t *n = &g->ast->nodes[node];
    if (n->kind == GLSL_NODE_ASSIGN && is_name(g, n->a, name, len))
        return GL_TRUE;
    if ((n->kind == GLSL_NODE_POSTFIX || n->kind == GLSL_NODE_UNARY) &&
        (n->op == GLSL_TOK_INC || n->op == GLSL_TOK_DEC) &&
        is_name(g, n->a, name, len)) {
        return GL_TRUE;
    }
    if (n->kind == GLSL_NODE_COMPOUND) {
        for (int32_t s = n->a; s != GLSL_NO_NODE; s = g->ast->nodes[s].sibling) {
            if (assigns_name(g, s, name, len))
                return GL_TRUE;
        }
        return GL_FALSE;
    }
    return (GLboolean)(assigns_name(g, n->a, name, len) ||
                       assigns_name(g, n->b, name, len) ||
                       assigns_name(g, n->c, name, len) ||
                       assigns_name(g, n->d, name, len));
}

/* The three scalar registers a branched loop at nesting level `d` keeps its masks in.
 */
static uint32_t loop_active_sgpr(int d) {
    return GLSL_GEN_LOOP_SGPR_BASE + (uint32_t)d * GLSL_GEN_LOOP_SGPR_COUNT;
}
static uint32_t loop_entry_sgpr(int d) {
    return loop_active_sgpr(d) + 1u;
}
static uint32_t loop_trip_sgpr(int d) {
    return loop_active_sgpr(d) + 2u;
}

/* `++`/`--` on a single-register place; see the note at `GLSL_NODE_POSTFIX`. */
static glsl_value_t gen_inc_dec(glsl_gen_t *g, int32_t node, GLboolean postfix) {
    const glsl_node_t *n = &g->ast->nodes[node];
    gen_place_t place;
    if (!gen_place_of(g, n->a, &place)) {
        glsl_value_t none;
        none.base = 0u;
        none.count = 0;
        return none;
    }
    if (place.count != 1)
        return gen_fail(g, "++ and -- take a single number", node);

    glsl_value_t before;
    before.base = 0u;
    before.count = 0;
    if (postfix) {
        before = gen_alloc(g, 1, node);
        if (is_bad(before))
            return before;
        glsl_emit_mov(g->code, before.base, place.reg[0]);
    }
    glsl_value_t one = gen_const(g, (n->op == GLSL_TOK_INC) ? 1.0 : -1.0, node);
    if (is_bad(one))
        return one;
    glsl_emit_add_f32(g->code, place.reg[0], place.reg[0], one.base);
    if (postfix)
        return before;

    glsl_value_t out;
    out.base = place.reg[0];
    out.count = 1;
    return out;
}

/* A condition into VCC, lane by lane, as `if` does it: the bool compared against zero.
 */
static GLboolean gen_cond_into_vcc(glsl_gen_t *g, int32_t cond_node) {
    glsl_value_t c = gen_expr(g, cond_node);
    if (is_bad(c))
        return GL_FALSE;
    if (c.count != 1) {
        (void)gen_fail(g, "a loop condition takes a single bool", cond_node);
        return GL_FALSE;
    }
    glsl_value_t zero = gen_const(g, 0.0, cond_node);
    if (is_bad(zero))
        return GL_FALSE;
    glsl_emit_cmp(g->code, GLSL_VOPC_NEQ_F32, c.base, zero.base);
    return GL_TRUE;
}

/*
 * The two ends of a branched loop, shared by `for`, `while` and `do`-`while`:
 *
 *       s_mov_b32  s_entry,  exec_lo   what the lanes go back to on the way out
 *       s_mov_b32  s_active, exec_lo   the lanes still going round
 *       s_mov_b32  s_trip,   0
 *   top:
 *       <condition into vcc_lo>        re-evaluated per trip, per lane
 *       s_and_b32  s_active, s_active, vcc_lo
 *       s_mov_b32  exec_lo, s_active
 *       s_cbranch_execz exit           no lane left
 *       <body>                         may narrow exec, break or continue
 *       s_mov_b32  exec_lo, s_active   undoes a `continue` before the step
 *       <step>
 *       s_add_u32  s_trip, s_trip, 1
 *       s_cmp_ge_u32 s_trip, trips
 *       s_cbranch_scc1 exit            the trip guard
 *       s_branch   top
 *   exit:
 *       s_mov_b32  exec_lo, s_entry
 *
 * `exec` is reloaded before the step so a lane that continued still counts. `break`
 * clears the lane from `s_active`; `continue` does not. The guard's ceiling is the
 * static trip count, or `GLSL_GEN_MAX_TRIPS` where there is none, so a condition that
 * never goes false ends instead of hanging the GPU.
 */
static void gen_loop_open(glsl_gen_t *g, uint32_t s_active, uint32_t s_entry,
                          uint32_t s_trip) {
    glsl_emit_exec_save(g->code, s_entry);
    glsl_emit_exec_save(g->code, s_active);
    glsl_emit_sop1(g->code, GLSL_SOP1_MOV_B32, s_trip,
                   128u); /* 128: scalar inline zero */
}

/* The tail: the trip guard, the branch back to `top`, and both exits patched to land
 * after it. `fix_empty` is the forward branch taken when no lane entered the body, or 0
 * for a loop shape that has none - a `do`-`while`, whose body always runs once. */
static GLboolean gen_loop_close(glsl_gen_t *g, int32_t node, uint32_t s_entry,
                                uint32_t s_trip, int trips, uint32_t top,
                                uint32_t fix_empty, GLboolean have_empty) {
    glsl_emit_s_inc_u32(g->code, s_trip);
    glsl_emit_s_cmp_ge_u32_imm(g->code, s_trip, (uint32_t)trips);
    const uint32_t fix_guard = glsl_emit_branch_fwd(g->code, GLSL_SOPP_CBRANCH_SCC1);
    glsl_emit_branch_back(g->code, GLSL_SOPP_BRANCH, top);
    /* Both exits land here. A failed patch means the buffer overflowed during the body,
     * and is reported rather than left as a branch to nowhere. */
    if (!glsl_patch_branch_here(g->code, fix_guard) ||
        (have_empty && !glsl_patch_branch_here(g->code, fix_empty))) {
        (void)gen_fail(
            g,
            "this loop's body is longer than the shader buffer, so the branches "
            "around it could not be resolved",
            node);
        return GL_FALSE;
    }
    glsl_emit_exec_restore(g->code, s_entry);
    return GL_TRUE;
}

/* `while`, `do`-`while` and an unrecognised `for`, on the branched-loop masks with
 * `GLSL_GEN_MAX_TRIPS` as the guard. The condition is generated from the expression
 * each trip. `do`-`while` tests at the bottom, so its body runs once and it needs no
 * branch around an empty first trip. */
static GLboolean gen_loop_branched(glsl_gen_t *g, int32_t node, int32_t init_node,
                                   int32_t cond_node, int32_t step_node,
                                   int32_t body_node, GLboolean post_test) {
    if (g->loop_depth >= GLSL_GEN_MAX_LOOP_DEPTH) {
        (void)gen_fail(g,
                       "the branched loops in this shader nest deeper than the scalar "
                       "registers set aside for them",
                       node);
        return GL_FALSE;
    }

    const int d = g->loop_depth;
    const uint32_t s_active = loop_active_sgpr(d);
    const uint32_t s_entry = loop_entry_sgpr(d);
    const uint32_t s_trip = loop_trip_sgpr(d);

    const int vars_before = g->var_count;
    /* A scope of its own, because the initialiser may declare the counter. The mark is
     * outside the body's so the counter survives every trip. */
    glsl_scope_push(g->sema);
    const uint32_t loop_mark = gen_mark(g);

    GLboolean ok = GL_TRUE;
    if (init_node != GLSL_NO_NODE)
        ok = glsl_gen_stmt(g, init_node);
    if (!ok) {
        gen_release(g, loop_mark);
        glsl_scope_pop(g->sema);
        g->var_count = vars_before;
        return GL_FALSE;
    }

    gen_loop_open(g, s_active, s_entry, s_trip);

    const uint32_t top = glsl_code_here(g->code);
    uint32_t fix_empty = 0u;
    GLboolean have_empty = GL_FALSE;

    /* A pre-tested loop narrows before the body; a post-tested one after it. No
     * condition (`for (;;)`) means true, bounded by the trip guard. */
    if (!post_test && cond_node != GLSL_NO_NODE) {
        const uint32_t mark = gen_mark(g);
        ok = gen_cond_into_vcc(g, cond_node);
        gen_release(g, mark);
        if (ok) {
            glsl_emit_sop2(g->code, GLSL_SOP2_AND_B32, s_active, s_active,
                           GLSL_SREG_VCC_LO);
            glsl_emit_exec_restore(g->code, s_active);
            fix_empty = glsl_emit_branch_fwd(g->code, GLSL_SOPP_CBRANCH_EXECZ);
            have_empty = GL_TRUE;
        }
    }

    if (ok) {
        g->loop_exec_depth[d] = g->exec_depth;
        g->loop_depth++;
        const int vars_here = g->var_count;
        const uint32_t mark = gen_mark(g);
        ok = glsl_gen_stmt(g, body_node);
        g->var_count = vars_here;
        gen_release(g, mark);
        g->loop_depth--;
    }

    if (ok) {
        /* Back to the full loop mask before the step and the condition, so a lane that
         * continued still takes the step (see `gen_loop_open`). */
        glsl_emit_exec_restore(g->code, s_active);
        if (step_node != GLSL_NO_NODE) {
            const uint32_t mark = gen_mark(g);
            glsl_value_t s = gen_expr(g, step_node);
            if (is_bad(s))
                ok = GL_FALSE;
            gen_release(g, mark);
        }
    }

    if (ok) {
        if (post_test && cond_node != GLSL_NO_NODE) {
            const uint32_t mark = gen_mark(g);
            ok = gen_cond_into_vcc(g, cond_node);
            gen_release(g, mark);
            if (ok) {
                glsl_emit_sop2(g->code, GLSL_SOP2_AND_B32, s_active, s_active,
                               GLSL_SREG_VCC_LO);
                glsl_emit_exec_restore(g->code, s_active);
                /* No lane still going round: fall out rather than branch back. */
                fix_empty = glsl_emit_branch_fwd(g->code, GLSL_SOPP_CBRANCH_EXECZ);
                have_empty = GL_TRUE;
            }
        }
    }

    if (ok) {
        ok = gen_loop_close(g, node, s_entry, s_trip, GLSL_GEN_MAX_TRIPS, top,
                            fix_empty, have_empty);
    }

    gen_release(g, loop_mark);
    glsl_scope_pop(g->sema);
    g->var_count = vars_before;
    return (GLboolean)(ok && g->error == (const char *)0);
}

static GLboolean gen_for_branched(glsl_gen_t *g, int32_t node, const glsl_node_t *decl,
                                  glsl_type_t ind_t, double start, double limit,
                                  double step, int cond_op, int trips) {
    const glsl_node_t *n = &g->ast->nodes[node];
    if (g->loop_depth >= GLSL_GEN_MAX_LOOP_DEPTH) {
        (void)gen_fail(g,
                       "the branched loops in this shader nest deeper than the scalar "
                       "registers set aside for them",
                       node);
        return GL_FALSE;
    }
    const int d = g->loop_depth;
    const uint32_t s_active = loop_active_sgpr(d);
    const uint32_t s_entry = loop_entry_sgpr(d);
    const uint32_t s_trip = loop_trip_sgpr(d);

    /* The counter outlives every trip, so it is allocated before the mark the body
     * releases to. */
    const int vars_before = g->var_count;
    glsl_scope_push(g->sema);
    if (!glsl_declare(g->sema, decl->text, decl->length, ind_t, GL_FALSE)) {
        g->sema->error = (const char *)0;
    }
    const uint32_t loop_mark = gen_mark(g);
    glsl_value_t iv = gen_alloc(g, 1, node);
    if (is_bad(iv)) {
        glsl_scope_pop(g->sema);
        g->var_count = vars_before;
        return GL_FALSE;
    }
    glsl_emit_mov_imm(g->code, iv.base, float_bits((float)start));
    if (!gen_declare(g, decl->text, decl->length, ind_t, iv, node)) {
        glsl_scope_pop(g->sema);
        g->var_count = vars_before;
        return GL_FALSE;
    }

    gen_loop_open(g, s_active, s_entry, s_trip);

    const uint32_t top = glsl_code_here(g->code);

    /* The condition is emitted from the shape the caller checked: a float compare of
     * the counter against the constant limit, cheaper per trip than generating the
     * expression. */
    GLboolean ok = GL_TRUE;
    {
        uint32_t opc = 0u;
        switch (cond_op) {
        case GLSL_TOK_LT:
            opc = GLSL_VOPC_LT_F32;
            break;
        case GLSL_TOK_LE:
            opc = GLSL_VOPC_LE_F32;
            break;
        case GLSL_TOK_GT:
            opc = GLSL_VOPC_GT_F32;
            break;
        case GLSL_TOK_GE:
            opc = GLSL_VOPC_GE_F32;
            break;
        default:
            opc = GLSL_VOPC_NEQ_F32;
            break; /* the caller allows only these */
        }
        const uint32_t mark = gen_mark(g);
        glsl_value_t k = gen_const(g, limit, node);
        if (is_bad(k))
            ok = GL_FALSE;
        else
            glsl_emit_cmp(g->code, opc, iv.base, k.base);
        gen_release(g, mark);
    }
    if (!ok) {
        glsl_scope_pop(g->sema);
        g->var_count = vars_before;
        return GL_FALSE;
    }

    glsl_emit_sop2(g->code, GLSL_SOP2_AND_B32, s_active, s_active, GLSL_SREG_VCC_LO);
    glsl_emit_exec_restore(g->code, s_active);
    const uint32_t fix_empty = glsl_emit_branch_fwd(g->code, GLSL_SOPP_CBRANCH_EXECZ);

    g->loop_exec_depth[d] = g->exec_depth;
    g->loop_depth++;
    {
        const int vars_here = g->var_count;
        const uint32_t mark = gen_mark(g);
        ok = glsl_gen_stmt(g, n->d);
        g->var_count = vars_here;
        gen_release(g, mark);
    }
    g->loop_depth--;

    if (ok) {
        /* Back to the full loop mask before the step (see `gen_loop_open`). */
        glsl_emit_exec_restore(g->code, s_active);
        /* The step is a float add of the constant the caller worked out. */
        const uint32_t mark = gen_mark(g);
        glsl_value_t k = gen_const(g, step, node);
        if (is_bad(k))
            ok = GL_FALSE;
        else
            glsl_emit_add_f32(g->code, iv.base, iv.base, k.base);
        gen_release(g, mark);
    }

    if (ok) {
        ok = gen_loop_close(g, node, s_entry, s_trip, trips, top, fix_empty, GL_TRUE);
    }

    gen_release(g, loop_mark);
    glsl_scope_pop(g->sema);
    g->var_count = vars_before;
    return (GLboolean)(ok && g->error == (const char *)0);
}

/* A `for` whose shape the unroller cannot read takes the branched path, bounded by the
 * trip guard. The shape analysis emits nothing, so arriving here leaves no half-written
 * loop behind. */
static GLboolean gen_for_generic(glsl_gen_t *g, int32_t node) {
    const glsl_node_t *n = &g->ast->nodes[node];
    return gen_loop_branched(g, node, n->a, n->b, n->c, n->d, GL_FALSE);
}

/*
 * A counted `for`, recognised so it can be unrolled: the initialiser sets the counter
 * to a constant, the condition compares it with one, and the step moves it by a
 * constant. Anything else goes to `gen_for_generic`, which is the same loop without the
 * compile-time count.
 *
 * The counter need not be declared by the loop: `for (i = 0; ...)` over a variable
 * declared above is recognised too, so `a[i]` inside it still has a constant index. A
 * counter declared outside is still readable afterwards, so it is given its final
 * value, `start + trips * step`, when the copies are done.
 */
static GLboolean gen_for(glsl_gen_t *g, int32_t node) {
    const glsl_node_t *n = &g->ast->nodes[node];

    /* The initialiser sets the induction variable to a constant, declaring it or not.
     * The init clause is parsed as a statement, so `for (i = 0; ...)` arrives as an
     * expression statement wrapping the assignment. */
    if (n->a == GLSL_NO_NODE)
        return gen_for_generic(g, node);
    const glsl_node_t *init = &g->ast->nodes[n->a];
    if (init->kind == GLSL_NODE_EXPR_STMT) {
        if (init->a == GLSL_NO_NODE)
            return gen_for_generic(g, node);
        init = &g->ast->nodes[init->a];
    }
    const glsl_node_t *decl =
        (const glsl_node_t *)0; /* null when the counter is not ours */
    const char *ind_name;
    size_t ind_len;
    glsl_type_t ind_t;
    double start = 0.0;
    if (init->kind == GLSL_NODE_DECL) {
        decl = init;
        ind_name = decl->text;
        ind_len = decl->length;
        ind_t = glsl_type_from_token(decl->type_tok);
        if (!const_of(g, decl->a, &start))
            return gen_for_generic(g, node);
    } else if (init->kind == GLSL_NODE_ASSIGN && init->op == GLSL_TOK_ASSIGN &&
               init->a != GLSL_NO_NODE &&
               g->ast->nodes[init->a].kind == GLSL_NODE_IDENTIFIER) {
        const glsl_node_t *lhs = &g->ast->nodes[init->a];
        ind_name = lhs->text;
        ind_len = lhs->length;
        ind_t = glsl_type_of(g->sema, init->a);
        if (ind_t != GLSL_TYPE_INT && ind_t != GLSL_TYPE_FLOAT) {
            return gen_for_generic(g, node);
        }
        if (!const_of(g, init->b, &start))
            return gen_for_generic(g, node);
        /* The counter has to have a register already, or the store at the end would
         * fail after the copies had been emitted. */
        if (!gen_find(g, ind_name, ind_len))
            return gen_for_generic(g, node);
    } else {
        return gen_for_generic(g, node);
    }

    /* The condition: the counter against a constant. */
    if (n->b == GLSL_NO_NODE || g->ast->nodes[n->b].kind != GLSL_NODE_BINARY) {
        return gen_for_generic(g, node);
    }
    const glsl_node_t *cond = &g->ast->nodes[n->b];
    double limit = 0.0;
    if (!is_name(g, cond->a, ind_name, ind_len) || !const_of(g, cond->b, &limit)) {
        return gen_for_generic(g, node);
    }

    /* The step: `i++`, `i--`, or `i += k`. */
    double step = 0.0;
    if (n->c == GLSL_NO_NODE)
        return gen_for_generic(g, node);
    {
        const glsl_node_t *inc = &g->ast->nodes[n->c];
        if ((inc->kind == GLSL_NODE_POSTFIX || inc->kind == GLSL_NODE_UNARY) &&
            is_name(g, inc->a, ind_name, ind_len)) {
            step = (inc->op == GLSL_TOK_INC) ? 1.0
                                             : (inc->op == GLSL_TOK_DEC ? -1.0 : 0.0);
        } else if (inc->kind == GLSL_NODE_ASSIGN &&
                   is_name(g, inc->a, ind_name, ind_len)) {
            double k = 0.0;
            if (const_of(g, inc->b, &k)) {
                if (inc->op == GLSL_TOK_ADD_ASSIGN)
                    step = k;
                else if (inc->op == GLSL_TOK_SUB_ASSIGN)
                    step = -k;
            }
        }
        if (step == 0.0)
            return gen_for_generic(g, node);
    }

    /* The trip count, counted as the reference runs it: test, body, step. */
    int trips = 0;
    for (double v = start; trips <= GLSL_GEN_MAX_TRIPS; v += step) {
        GLboolean go;
        switch (cond->op) {
        case GLSL_TOK_LT:
            go = (GLboolean)(v < limit);
            break;
        case GLSL_TOK_LE:
            go = (GLboolean)(v <= limit);
            break;
        case GLSL_TOK_GT:
            go = (GLboolean)(v > limit);
            break;
        case GLSL_TOK_GE:
            go = (GLboolean)(v >= limit);
            break;
        case GLSL_TOK_NE:
            go = (GLboolean)(v != limit);
            break;
        default:
            return gen_for_generic(g, node);
        }
        if (!go)
            break;
        trips++;
    }
    /* A known trip count above the guard's ceiling is refused rather than sent to the
     * generic loop: the guard would certainly fire and truncate the loop silently. */
    if (trips > GLSL_GEN_MAX_TRIPS) {
        (void)gen_fail(
            g,
            "this loop asks for more trips than the generator will bound, and an "
            "unbounded loop on this part is a hang rather than a wrong colour",
            node);
        return GL_FALSE;
    }

    /* A body that moves the counter invalidates the count, so it goes to the generic
     * loop, which re-evaluates the condition every trip. Checked before anything is
     * emitted. */
    if (assigns_name(g, n->d, ind_name, ind_len))
        return gen_for_generic(g, node);

    /* Unrolled where it fits, so the counter stays a constant and nothing branches. Too
     * many trips, or a `break`/`continue`, sends it to the branched path, which
     * declares the counter itself; a loop without its own declaration goes to the
     * generic form, which runs the initialiser and step as written. */
    if (trips > GLSL_GEN_MAX_UNROLL || has_loop_flow(g, n->d)) {
        if (!decl)
            return gen_for_generic(g, node);
        return gen_for_branched(g, node, decl, ind_t, start, limit, step, (int)cond->op,
                                trips);
    }

    /* One copy per trip, the counter a fresh constant each time, in the loop's own
     * scope. A counter declared outside is shadowed the same way and not touched until
     * the store below. */
    const int vars_before = g->var_count;
    glsl_scope_push(g->sema);
    if (!glsl_declare(g->sema, ind_name, ind_len, ind_t, GL_FALSE)) {
        g->sema->error = (const char *)0;
    }
    double v = start;
    for (int t = 0; t < trips && !g->error; t++, v += step) {
        const uint32_t mark = gen_mark(g);
        glsl_value_t iv = gen_alloc(g, 1, node);
        if (is_bad(iv))
            break;
        glsl_emit_mov_imm(g->code, iv.base, float_bits((float)v));
        const int vars_here = g->var_count;
        glsl_gen_var_t *ivar = gen_declare(g, ind_name, ind_len, ind_t, iv, node);
        if (!ivar)
            break;
        /* This copy's value, so `w[i]` is a constant index. The body was checked for
         * assignments to the counter, so it cannot go stale. */
        ivar->is_const = GL_TRUE;
        ivar->const_val = v;
        (void)glsl_gen_stmt(g, n->d);
        g->var_count = vars_here;
        gen_release(g, mark);
    }
    glsl_scope_pop(g->sema);
    g->var_count = vars_before;

    /* A counter declared outside the loop is still readable after it, and the copies
     * wrote only a shadow. It gets the value the loop would have left: `v`, where the
     * condition failed. */
    if (!decl && !g->error) {
        glsl_gen_var_t *outer = gen_find(g, ind_name, ind_len);
        if (outer && outer->value.count == 1) {
            glsl_emit_mov_imm(g->code, outer->value.base, float_bits((float)v));
            /* A known constant from here, so a later loop or `a[i]` folds too. */
            outer->is_const = GL_TRUE;
            outer->const_val = v;
        }
    }
    return (GLboolean)(g->error == (const char *)0);
}

GLboolean glsl_gen_stmt(glsl_gen_t *g, int32_t node) {
    if (g->error)
        return GL_FALSE;
    if (node == GLSL_NO_NODE)
        return GL_TRUE;

    const glsl_node_t *n = &g->ast->nodes[node];
    switch (n->kind) {
    case GLSL_NODE_COMPOUND: {
        /* The variables a block declares go out of scope with it. The register mark is
         * not rolled back, since something outside may have been allocated since. The
         * semantic stage's scope is pushed alongside, because it answers the type of
         * every operand. */
        const int vars_before = g->var_count;
        glsl_scope_push(g->sema);
        for (int32_t s = n->a; s != GLSL_NO_NODE; s = g->ast->nodes[s].sibling) {
            if (!glsl_gen_stmt(g, s)) {
                glsl_scope_pop(g->sema);
                return GL_FALSE;
            }
        }
        glsl_scope_pop(g->sema);
        g->var_count = vars_before;
        return GL_TRUE;
    }
    case GLSL_NODE_DECL: {
        /* Every declarator in `float a, b = 1.0;` is its own node on the sibling chain,
         * so this arm sees one name at a time and the chain is walked by the caller. */
        const glsl_type_t t = gen_node_type(g, n);
        if (!is_generated(t)) {
            (void)gen_fail(
                g, "only float, vec, mat, bool and struct locals are generated", node);
            return GL_FALSE;
        }
        /* An array is its elements end to end in one run of registers, element `k` at
         * `base + k * w`. The size is a constant, as GLSL 1.10 4.1.9 requires. */
        int elems = 0;
        if (n->array_size != GLSL_NO_NODE) {
            double sz = 0.0;
            if (!const_of(g, n->array_size, &sz)) {
                (void)gen_fail(g,
                               "an array's length has to be a constant, which the "
                               "language requires of it too",
                               node);
                return GL_FALSE;
            }
            elems = (int)sz;
            if (elems < 1 || (double)elems != sz) {
                (void)gen_fail(g, "an array's length has to be a positive whole number",
                               node);
                return GL_FALSE;
            }
            /* An initialiser must be a GLSL 1.20 array constructor of the right length,
             * `float[4](a, b, c, d)`; the declaration supplies the length. */
            if (n->a != GLSL_NO_NODE) {
                glsl_type_t ael = GLSL_TYPE_ERROR;
                int acount = 0;
                if (!glsl_array_ctor_shape(g->ast, n->a, &ael, &acount)) {
                    (void)gen_fail(g,
                                   "an array takes an array constructor here, or none; "
                                   "`float[2](a, b)` is 1.20's form",
                                   node);
                    return GL_FALSE;
                }
                if (acount != elems) {
                    (void)gen_fail(
                        g, "the array constructor's length is not the array's", node);
                    return GL_FALSE;
                }
            }
        }
        glsl_value_t home =
            gen_alloc(g, gen_comps(g, t) * (elems > 0 ? elems : 1), node);
        if (is_bad(home))
            return GL_FALSE;
        if (elems > 0) {
            /* The run is the array; `value.count` stays the element width so every
             * other reader sees one element. The constructor's arguments fill it before
             * the name is declared, so the initialiser cannot see itself. */
            if (n->a != GLSL_NO_NODE) {
                const int w = gen_comps(g, t);
                int filled = 0;
                for (int32_t arg = g->ast->nodes[n->a].b;
                     arg != GLSL_NO_NODE && filled < elems;
                     arg = g->ast->nodes[arg].sibling, filled++) {
                    const uint32_t mark = gen_mark(g);
                    glsl_value_t v = gen_expr(g, arg);
                    if (is_bad(v))
                        return GL_FALSE;
                    if (v.count != w) {
                        (void)gen_fail(g,
                                       "an array constructor's argument is a different "
                                       "width from the element",
                                       node);
                        return GL_FALSE;
                    }
                    for (int c = 0; c < w; c++) {
                        glsl_emit_mov(g->code, home.base + (uint32_t)(filled * w + c),
                                      v.base + (uint32_t)c);
                    }
                    gen_release(g, mark);
                }
                if (filled != elems) {
                    (void)gen_fail(
                        g, "the array constructor's length is not the array's", node);
                    return GL_FALSE;
                }
            }
            home.count = gen_comps(g, t);
            glsl_gen_var_t *av = gen_declare(g, n->text, n->length, t, home, node);
            if (!av)
                return GL_FALSE;
            av->array_size = elems;
            if (!glsl_declare_array(g->sema, n->text, n->length, t, elems,
                                    (glsl_token_type_t)0)) {
                (void)gen_fail(g, "this name is already declared in this scope", node);
                return GL_FALSE;
            }
            return GL_TRUE;
        }
        /* A `const int` initialised from a constant is a constant to `const_of` too, so
         * it can be an array length, a loop bound or an index. It still gets a
         * register. Read before the name is declared, so `const int a = a;` cannot see
         * itself; it cannot go stale, since assigning a `const` is refused
         * (GLSL 1.10 4.3.1). */
        double const_val = 0.0;
        const GLboolean is_const_decl =
            (GLboolean)(n->qualifier == GLSL_TOK_KW_CONST && t == GLSL_TYPE_INT &&
                        n->array_size == GLSL_NO_NODE && const_of(g, n->a, &const_val));
        if (n->a != GLSL_NO_NODE) {
            const uint32_t mark = gen_mark(g);
            glsl_value_t init = gen_expr(g, n->a);
            if (is_bad(init))
                return GL_FALSE;
            if (init.count != home.count) {
                (void)gen_fail(
                    g, "the initialiser is a different width from the variable", node);
                return GL_FALSE;
            }
            gen_move(g, home, init);
            gen_release(g, mark);
        }
        glsl_gen_var_t *dv = gen_declare(g, n->text, n->length, t, home, node);
        if (!dv)
            return GL_FALSE;
        if (is_const_decl) {
            dv->is_const = GL_TRUE;
            dv->const_val = const_val;
        }
        /* Into the semantic stage, which answers `glsl_type_of` for later mentions;
         * after the initialiser, so `float a = a;` cannot see itself. */
        if (!glsl_declare(g->sema, n->text, n->length, t, GL_FALSE)) {
            (void)gen_fail(g, "this name is already declared in this scope", node);
            return GL_FALSE;
        }
        return GL_TRUE;
    }
    case GLSL_NODE_EXPR_STMT: {
        if (n->a == GLSL_NO_NODE)
            return GL_TRUE; /* the empty statement */
        const uint32_t mark = gen_mark(g);
        glsl_value_t v = gen_expr(g, n->a);
        /* A void call has width zero, which `is_bad` would call a failure, so the
         * error flag decides here. */
        (void)v;
        if (g->error)
            return GL_FALSE;
        gen_release(g, mark);
        return GL_TRUE;
    }
    /* `if` is the exec mask, with no branch: the then arm runs with `exec` narrowed to
     * the lanes the condition holds for, the else arm with the complement, and `exec`
     * is restored. An arm no lane runs writes nothing, so `s_cbranch_execz` is omitted.
     * Scalar instructions are not exec-masked, but a nested `if` computes `exec &
     * mask`, which is zero inside a dead outer one. */
    case GLSL_NODE_IF: {
        if (g->exec_depth >= GLSL_GEN_MAX_EXEC_DEPTH) {
            (void)gen_fail(
                g,
                "the conditionals in this shader nest deeper than the scalar "
                "registers set aside for them",
                node);
            return GL_FALSE;
        }
        const uint32_t saved = GLSL_GEN_EXEC_SGPR_BASE + (uint32_t)g->exec_depth;
        const uint32_t mark = gen_mark(g);
        glsl_value_t cond = gen_expr(g, n->a);
        if (is_bad(cond))
            return GL_FALSE;
        if (cond.count != 1) {
            (void)gen_fail(g, "an if takes a single condition", node);
            return GL_FALSE;
        }
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero))
            return GL_FALSE;
        glsl_emit_cmp(g->code, GLSL_VOPC_NEQ_F32, cond.base, zero.base);
        glsl_emit_exec_save_and_vcc(g->code, saved);
        /* The condition's registers are dead the moment the mask is taken. */
        gen_release(g, mark);

        g->exec_depth++;
        GLboolean ok = glsl_gen_stmt(g, n->b);
        if (ok && n->c != GLSL_NO_NODE) {
            glsl_emit_exec_else(g->code, saved);
            ok = glsl_gen_stmt(g, n->c);
        }
        g->exec_depth--;
        glsl_emit_exec_restore(g->code, saved);
        return ok;
    }
    /* `discard` outlives the `if` it sits in: the lanes come out of each enclosing
     * saved mask before `exec` is cleared, or a restore would hand them back. The
     * export at the end still runs and carries `done`, so a wave that discarded every
     * lane retires. */
    case GLSL_NODE_DISCARD: {
        for (int d = 0; d < g->exec_depth; d++) {
            glsl_emit_exec_drop_live(g->code, GLSL_GEN_EXEC_SGPR_BASE + (uint32_t)d);
        }
        /* And the live mask, always: the export is restored from it, so a discard
         * that missed it would be undone at the end of the shader. Helper lanes were
         * never in it.
         *
         * A sample after a discard gets a level of detail from a smaller quad, since
         * the discarded lane no longer contributes to derivatives. GLSL leaves this
         * undefined; ACO keeps the quad whole with a separate demote mask
         * (`aco_insert_exec_mask.cpp`), so such a shader can differ from a desktop
         * driver in the mip level chosen. Sampling before discarding is unaffected. */
        glsl_emit_exec_drop_live(g->code, GLSL_GEN_LIVE_SGPR);
        /* And every enclosing branched loop's active and entry masks, from which
         * `exec` is reloaded each trip and after the loop. */
        for (int l = 0; l < g->loop_depth; l++) {
            glsl_emit_exec_drop_live(g->code, loop_active_sgpr(l));
            glsl_emit_exec_drop_live(g->code, loop_entry_sgpr(l));
        }
        glsl_emit_exec_clear(g->code);
        return GL_TRUE;
    }
    /* `break` and `continue` take the running lanes out of every `if` enclosing them
     * inside the loop, as `discard` does, and clear `exec`. `break` also takes them out
     * of the loop's active mask, so the next trip does not bring them back. `if`s
     * outside the loop are untouched: the loop's entry mask restores the lane after it.
     */
    case GLSL_NODE_BREAK:
    case GLSL_NODE_CONTINUE: {
        if (g->loop_depth <= 0) {
            (void)gen_fail(
                g,
                "`break` and `continue` are generated only inside a loop that "
                "branches - an unrolled loop has no mask for them to act on",
                node);
            return GL_FALSE;
        }
        const int d = g->loop_depth - 1;
        for (int e = g->loop_exec_depth[d]; e < g->exec_depth; e++) {
            glsl_emit_exec_drop_live(g->code, GLSL_GEN_EXEC_SGPR_BASE + (uint32_t)e);
        }
        if (n->kind == GLSL_NODE_BREAK) {
            glsl_emit_exec_drop_live(g->code, loop_active_sgpr(d));
        }
        glsl_emit_exec_clear(g->code);
        return GL_TRUE;
    }
    case GLSL_NODE_FOR:
        return gen_for(g, node);

    /* An early `return` (`gen_call_user` generates the trailing one). The value goes
     * into the register the trailing return writes, under the current exec, so each
     * lane keeps the value of the return it reached. Then the lanes come out of every
     * `if` and loop inside this function and `exec` is cleared. Masks enclosing the
     * call keep the lane, since returning ends only the function; that is why the
     * depths are recorded at the call. */
    case GLSL_NODE_RETURN: {
        /* A `return` in `main` is a discard that still exports: the lane leaves every
         * enclosing `if` and loop and `exec` is cleared, but `GLSL_GEN_LIVE_SGPR`,
         * which the epilogue restores from, is untouched. The lane exports whatever
         * `gl_FragColor` held. */
        if (g->inline_depth <= 0) {
            if (n->a != GLSL_NO_NODE) {
                (void)gen_fail(
                    g, "`main` returns void, so `return` there takes no value", node);
                return GL_FALSE;
            }
            for (int e = 0; e < g->exec_depth; e++) {
                glsl_emit_exec_drop_live(g->code,
                                         GLSL_GEN_EXEC_SGPR_BASE + (uint32_t)e);
            }
            for (int l = 0; l < g->loop_depth; l++) {
                glsl_emit_exec_drop_live(g->code, loop_active_sgpr(l));
                glsl_emit_exec_drop_live(g->code, loop_entry_sgpr(l));
            }
            glsl_emit_exec_clear(g->code);
            return GL_TRUE;
        }
        const int d = g->inline_depth - 1;
        if (n->a != GLSL_NO_NODE) {
            if (g->fn_out[d].count == 0) {
                (void)gen_fail(g, "a void function returns a value", node);
                return GL_FALSE;
            }
            const uint32_t mark = gen_mark(g);
            glsl_value_t rv = gen_expr(g, n->a);
            if (is_bad(rv))
                return GL_FALSE;
            if (rv.count != g->fn_out[d].count) {
                (void)gen_fail(g,
                               "the returned expression is a different width from the "
                               "function's return type",
                               node);
                return GL_FALSE;
            }
            for (int c = 0; c < g->fn_out[d].count; c++) {
                glsl_emit_mov(g->code, g->fn_out[d].base + (uint32_t)c,
                              rv.base + (uint32_t)c);
            }
            gen_release(g, mark);
        }
        for (int e = g->fn_exec_depth[d]; e < g->exec_depth; e++) {
            glsl_emit_exec_drop_live(g->code, GLSL_GEN_EXEC_SGPR_BASE + (uint32_t)e);
        }
        /* And out of any loop the function opened, whose top would otherwise reload
         * `exec` from a mask the lane is still in. */
        for (int l = g->fn_loop_depth[d]; l < g->loop_depth; l++) {
            glsl_emit_exec_drop_live(g->code, loop_active_sgpr(l));
            glsl_emit_exec_drop_live(g->code, loop_entry_sgpr(l));
        }
        glsl_emit_exec_clear(g->code);
        return GL_TRUE;
    }
    /* The two kinds hold their children in source order: `while (c) s;` has the
     * condition in `a` and the body in `b`, `do s; while (c);` the body in `a` and the
     * condition in `b`. */
    case GLSL_NODE_WHILE:
        return gen_loop_branched(g, node, GLSL_NO_NODE, n->a, GLSL_NO_NODE, n->b,
                                 GL_FALSE);
    case GLSL_NODE_DO_WHILE:
        return gen_loop_branched(g, node, GLSL_NO_NODE, n->b, GLSL_NO_NODE, n->a,
                                 GL_TRUE);
    default:
        (void)gen_fail(
            g,
            "this statement has no instruction selection yet: declarations, "
            "expressions, blocks, `if`, `for`, `while`, `do`, `break`, `continue`, "
            "`return` and `discard` are",
            node);
        return GL_FALSE;
    }
}

glsl_value_t glsl_gen_expression(glsl_gen_t *g, int32_t node) {
    return gen_expr(g, node);
}

/* One register from the bump allocator, held for the whole shader and bound to no name,
 * for the prologue's own constants (`gl_FrontFacing` selects between 0.0 and 1.0 held
 * in registers). Never released. */
uint32_t glsl_gen_scratch(glsl_gen_t *g) {
    if (!g)
        return 0u;
    const glsl_value_t v = gen_alloc(g, 1, GLSL_NO_NODE);
    return is_bad(v) ? 0u : v.base;
}

void glsl_gen_reserve(glsl_gen_t *g, uint32_t first) {
    if (!g)
        return;
    if (first > g->next_vgpr)
        g->next_vgpr = first;
    if (g->next_vgpr > g->high_water)
        g->high_water = g->next_vgpr;
}

GLboolean glsl_gen_declare_sampler(glsl_gen_t *g, const char *name, size_t len,
                                   uint32_t set, uint32_t dim, GLboolean shadow,
                                   GLboolean oned) {
    if (!g)
        return GL_FALSE;
    if (g->sampler_count >= GLSL_GEN_MAX_TEX_SETS) {
        (void)gen_fail(g,
                       "more samplers than the draw path carries descriptor sets for",
                       GLSL_NO_NODE);
        return GL_FALSE;
    }
    g->samplers[g->sampler_count].name = name;
    g->samplers[g->sampler_count].name_len = len;
    g->samplers[g->sampler_count].set = set;
    g->samplers[g->sampler_count].dim = dim;
    g->samplers[g->sampler_count].shadow = shadow;
    g->samplers[g->sampler_count].oned = oned;
    g->sampler_count++;
    /* And into the semantic stage, which types the lookup from its argument types; an
     * unknown sampler would make the whole call `GLSL_TYPE_ERROR`. */
    glsl_type_t st;
    if (shadow)
        st = oned ? GLSL_TYPE_SAMPLER1DSHADOW : GLSL_TYPE_SAMPLER2DSHADOW;
    else if (dim == GLSL_IMG_DIM_CUBE)
        st = GLSL_TYPE_SAMPLERCUBE;
    else if (dim == GLSL_IMG_DIM_3D)
        st = GLSL_TYPE_SAMPLER3D;
    else
        st = oned ? GLSL_TYPE_SAMPLER1D : GLSL_TYPE_SAMPLER2D;
    if (!glsl_declare(g->sema, name, len, st, GL_FALSE)) {
        g->sema->error =
            (const char *)0; /* already declared is the caller having done it */
    }
    /* A sampler puts the shader in whole-quad mode, decided here because the prologue
     * emits `s_wqm_b32` before the body. An unused sampler costs two instructions. */
    g->wqm = GL_TRUE;
    return GL_TRUE;
}

GLboolean glsl_gen_lookup(glsl_gen_t *g, const char *name, size_t len,
                          glsl_value_t *out) {
    if (!g)
        return GL_FALSE;
    const glsl_gen_var_t *v = gen_find(g, name, len);
    if (!v)
        return GL_FALSE;
    if (out)
        *out = v->value;
    return GL_TRUE;
}

glsl_value_t glsl_gen_declare_input(glsl_gen_t *g, const char *name, size_t len,
                                    glsl_type_t type) {
    glsl_value_t none;
    none.base = 0u;
    none.count = 0;
    if (!g || g->error)
        return none;
    /* An `int` or `bool` uniform is a float in the program's value pool, so it arrives
     * in a register like any other input. */
    if (!is_generated(type) && type != GLSL_TYPE_INT) {
        return gen_fail(g, "only float, vec, mat, int and bool inputs are generated",
                        GLSL_NO_NODE);
    }
    glsl_value_t home = gen_alloc(g, glsl_type_components(type), GLSL_NO_NODE);
    if (is_bad(home))
        return home;
    if (!gen_declare(g, name, len, type, home, GLSL_NO_NODE))
        return none;
    /* Into the semantic stage too, so it can type expressions that mention this name; a
     * caller that has already declared it there passes through. */
    if (!glsl_declare(g->sema, name, len, type, GL_FALSE)) {
        g->sema->error =
            (const char *)0; /* a redeclaration here is the caller having done it */
    }
    return home;
}

/* An input that is an array, such as `gl_TexCoord[]`: `count` elements end to end, the
 * layout `gen_index_of` reads for a declared array. */
glsl_value_t glsl_gen_declare_input_array(glsl_gen_t *g, const char *name, size_t len,
                                          glsl_type_t type, int count) {
    glsl_value_t none;
    none.base = 0u;
    none.count = 0;
    if (!g || g->error || count <= 0)
        return none;
    if (!is_generated(type)) {
        return gen_fail(g, "only float, vec and mat array inputs are generated",
                        GLSL_NO_NODE);
    }
    const int width = glsl_type_components(type);
    glsl_value_t run = gen_alloc(g, width * count, GLSL_NO_NODE);
    if (is_bad(run))
        return run;

    /* The variable's `value` is one element: the element's width and the run's base. */
    glsl_value_t elem;
    elem.base = run.base;
    elem.count = width;
    glsl_gen_var_t *v = gen_declare(g, name, len, type, elem, GLSL_NO_NODE);
    if (!v)
        return none;
    v->array_size = count;

    if (!glsl_declare_array(g->sema, name, len, type, count, GLSL_TOK_KW_VARYING)) {
        g->sema->error = (const char *)0; /* already declared by the caller */
    }
    return run;
}
