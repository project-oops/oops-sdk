/*
 * oops-gl: instruction selection - the GLSL tree becomes gfx1030 instructions.
 *
 * The stage between the semantic checker and the encoder. `glsl_sema.c` has already decided what
 * every expression means and what type it has; `glsl_emit.c` knows how to spell one instruction.
 * This walks the tree and decides *which* instructions, and into which registers.
 *
 * # What a value is
 *
 * A value occupies **consecutive VGPRs, one per component**: a `float` is one register, a `vec4`
 * is four, a `mat4` is sixteen laid out column-major - `m[0..3]` is the first column, which is
 * how GL stores a matrix and how `glsl_emit_mat4_mul_vec4` reads one. There is no packing and no
 * component aliasing. That is not the tightest allocation possible and it is the one whose
 * mistakes are visible: a swizzle is a move, not a change of meaning attached to a register.
 *
 * # What it refuses, and why refusing is the point
 *
 * Only the float family is generated - `float`, `vec2/3/4`, `mat2/3/4` - and only the operators
 * whose opcodes have been read out of a real assembler. **Everything else sets `error` and emits
 * nothing.** Integer arithmetic has no verified opcode here yet, so `int` maths is refused rather
 * than approximated with the float ones; division is refused rather than emitted as a reciprocal
 * whose precision nobody has measured. A shader that will not compile is a build failure. A
 * shader compiled out of guessed instructions is a frame that is subtly wrong on hardware and
 * correct everywhere else, which is the failure this whole port is arranged to avoid (D009).
 */

#include "glsl_internal.h"

/* -------------------------------------------------------------------------
 * The generator's own state
 * ------------------------------------------------------------------------- */

void glsl_gen_init(glsl_gen_t *g, glsl_ast_t *ast, glsl_sema_t *sema, glsl_code_t *code) {
    if (!g) return;
    g->ast = ast;
    g->sema = sema;
    g->code = code;
    g->next_vgpr = 0u;
    g->high_water = 0u;
    g->var_count = 0;
    g->exec_depth = 0;
    g->inline_depth = 0;
    g->wqm = GL_FALSE;
    g->sampler_count = 0;
    g->error = (const char *)0;
    g->error_line = 0;
    g->error_column = 0;
}

/* The first failure is the one reported, and it stops everything after it: a generator that
 * carried on would emit instructions for a tree it had already said it did not understand. */
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

static GLboolean is_bad(glsl_value_t v) { return v.count == 0 ? GL_TRUE : GL_FALSE; }

/*
 * A bump allocator over the VGPR file, with a mark that statements roll back to.
 *
 * Straight-line code with no branches, so a value is live from where it is produced to where it
 * is consumed and nothing outlives the statement that made it. Variables are allocated below the
 * mark and never move; temporaries come from above it and are reclaimed when the statement ends.
 *
 * **Exhaustion is an error, never a wrap.** Wrapping would silently alias a temporary onto a
 * variable, and the shader would compute something plausible.
 */
static glsl_value_t gen_alloc(glsl_gen_t *g, int count, int32_t node) {
    glsl_value_t v;
    if (count <= 0 || (uint32_t)count > GLSL_MAX_VGPRS - g->next_vgpr) {
        return gen_fail(g, "the shader needs more registers than the file has", node);
    }
    v.base = g->next_vgpr;
    v.count = count;
    g->next_vgpr += (uint32_t)count;
    if (g->next_vgpr > g->high_water) g->high_water = g->next_vgpr;
    return v;
}

static uint32_t gen_mark(const glsl_gen_t *g) { return g->next_vgpr; }
static void gen_release(glsl_gen_t *g, uint32_t mark) { g->next_vgpr = mark; }

/* -------------------------------------------------------------------------
 * Variables
 * ------------------------------------------------------------------------- */

static GLboolean name_is(const glsl_gen_var_t *v, const char *name, size_t len) {
    if (v->name_len != len) return GL_FALSE;
    for (size_t i = 0; i < len; i++) {
        if (v->name[i] != name[i]) return GL_FALSE;
    }
    return GL_TRUE;
}

static glsl_gen_var_t *gen_find(glsl_gen_t *g, const char *name, size_t len) {
    /* Backwards, so an inner declaration shadows an outer one the way the scope rules say. */
    for (int i = g->var_count - 1; i >= 0; i--) {
        if (name_is(&g->vars[i], name, len)) return &g->vars[i];
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
    return v;
}

/* -------------------------------------------------------------------------
 * Types this stage will generate for
 * ------------------------------------------------------------------------- */

/* The float family. `bool` is its own thing below; `int` is a float that is kept whole. */
static GLboolean is_float_family(glsl_type_t t) {
    switch (t) {
        case GLSL_TYPE_FLOAT:
        case GLSL_TYPE_VEC2: case GLSL_TYPE_VEC3: case GLSL_TYPE_VEC4:
        case GLSL_TYPE_MAT2: case GLSL_TYPE_MAT3: case GLSL_TYPE_MAT4:
            return GL_TRUE;
        default:
            return GL_FALSE;
    }
}

static GLboolean is_matrix(glsl_type_t t) {
    return (t == GLSL_TYPE_MAT2 || t == GLSL_TYPE_MAT3 || t == GLSL_TYPE_MAT4) ? GL_TRUE : GL_FALSE;
}

/* The side of a square matrix, or 0 for anything else. */
static int mat_dim(glsl_type_t t) {
    switch (t) {
        case GLSL_TYPE_MAT2: return 2;
        case GLSL_TYPE_MAT3: return 3;
        case GLSL_TYPE_MAT4: return 4;
        default: return 0;
    }
}

/*
 * **A bool is a float that is 0.0 or 1.0, in a register of its own.**
 *
 * The hardware's own answer to a comparison is a *lane mask* in an SGPR, which is the right
 * representation for a condition and the wrong one for a value: `bool b = x < y;` has to live
 * somewhere a variable lives, and every variable here is VGPRs. So a comparison writes the mask
 * to `vcc` and immediately selects 1.0 or 0.0 out of it, and everything downstream - `&&`, the
 * `?:`, an `if`'s condition - works on that.
 *
 * It costs an instruction and a register over carrying the mask around. What it buys is that
 * there is exactly **one** register class in this back end, which is the thing that makes the
 * allocator, the swizzles and the places all as simple as they are.
 *
 * Because the values are exactly 0.0 and 1.0: `a && b` is `min`, `a || b` is `max`, and `!a` is
 * `1 - a`. No comparison is needed for any of them.
 */
static GLboolean is_bool_family(glsl_type_t t) {
    switch (t) {
        case GLSL_TYPE_BOOL:
        /* **A `bvec` is the same thing per component**, which costs nothing extra: the
         * comparisons write one per component and the reductions are a `min` or a `max` over
         * values that are only ever 0.0 or 1.0. */
        case GLSL_TYPE_BVEC2: case GLSL_TYPE_BVEC3: case GLSL_TYPE_BVEC4:
            return GL_TRUE;
        default:
            return GL_FALSE;
    }
}

/*
 * **An `int` is a float that is kept whole**, which is the reference's representation and
 * therefore the one this has to match.
 *
 * `glsl_exec.c` holds every value in a `float v[16]` and writes `(float)(int)x` after an
 * integer operation; this writes the same arithmetic and a `v_trunc_f32` after it. That is not
 * a shortcut taken for convenience - GLSL 1.10 requires only that an integer hold 16 bits and
 * explicitly allows an implementation to store one in a float - and it is the only
 * representation under which the console and the host can agree, which is what the whole
 * arrangement is for.
 *
 * What it cannot do is arithmetic that overflows 24 bits of mantissa, where a float stops being
 * able to count. The reference has the same ceiling, so the two still agree; they are simply
 * both wrong about numbers no fragment shader has.
 */
static GLboolean is_int_family(glsl_type_t t) {
    switch (t) {
        case GLSL_TYPE_INT:
        case GLSL_TYPE_IVEC2: case GLSL_TYPE_IVEC3: case GLSL_TYPE_IVEC4:
            return GL_TRUE;
        default:
            return GL_FALSE;
    }
}

/* Everything this stage has a register for. */
static GLboolean is_generated(glsl_type_t t) {
    return (is_float_family(t) || is_bool_family(t) || is_int_family(t)) ? GL_TRUE : GL_FALSE;
}

/* The bits of a float literal, without punning through a pointer.
 *
 * **Every constant this file needs is written as a decimal and converted here**, never as the
 * hexadecimal it comes out as. A wrong bit pattern is a number nobody can read back, and the
 * lowerings below turn on constants that are easy to get subtly wrong - `1/2pi`, `log2 e`,
 * `ln 2`. Written as decimals they are checkable against a table; written as hex they are not. */
static uint32_t float_bits(double d) {
    union { float f; uint32_t u; } cvt;
    cvt.f = (float)d;
    return cvt.u;
}

/* Component `i` of a value, broadcasting a scalar. GLSL's mixed-width rules - `v * 2.0`,
 * `min(v, 0.0)`, `mix(a, b, t)` with a float `t` - are all this one rule. */
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
    if (is_bad(v)) return v;
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
        case GLSL_TOK_PLUS:  glsl_emit_add_f32(g->code, d, a, b); break;
        case GLSL_TOK_MINUS: glsl_emit_sub_f32(g->code, d, a, b); break;
        case GLSL_TOK_STAR:  glsl_emit_mul_f32(g->code, d, a, b); break;
        default: break; /* the caller has already refused anything else */
    }
}

/*
 * `a op b`, where op is one of `+ - *`.
 *
 * Three shapes, and the third is not the other two:
 *
 *   - same width: component for component.
 *   - scalar with a vector, either way round: the scalar is broadcast. **`*` is not commutative
 *     in its encoding** even though it is in its meaning - VOP2 takes one biased source and one
 *     bare one - so the operand that is a register block and the operand that is a single
 *     register are told apart here rather than in the encoder.
 *   - `mat4 * vec4`: a transform, not sixteen component-wise multiplies. Handled before the
 *     widths are compared, because the widths do not match and the natural reading of that is
 *     wrong rather than merely unsupported.
 *
 * **`/` is a reciprocal and a multiply**, which is the only division this instruction set has:
 * `v_rcp_f32` and then `v_mul_f32`. That is what ACO emits for a GLSL divide that is not marked
 * `precise` (mesa/src/amd/compiler/aco_instruction_selection.cpp, `nir_op_fdiv`), and the
 * reciprocal is accurate to 1 ULP, so the quotient is within about 2. GLSL 1.10 requires no
 * better - section 4.5.1 leaves division's precision to the implementation - and the software
 * rasteriser in `glsl_exec.c` divides exactly, so the two paths can differ in the last bit.
 * That is the one divergence between them, and it is recorded here rather than discovered.
 */
/* Does this subtree write to anything? What decides whether `&&` and `||` can be evaluated on
 * both sides - see `gen_logical`. */
static GLboolean has_side_effect(const glsl_ast_t *ast, int32_t node) {
    if (node == GLSL_NO_NODE) return GL_FALSE;
    const glsl_node_t *n = &ast->nodes[node];
    if (n->kind == GLSL_NODE_ASSIGN || n->kind == GLSL_NODE_POSTFIX) return GL_TRUE;
    if (n->kind == GLSL_NODE_UNARY &&
        (n->op == GLSL_TOK_INC || n->op == GLSL_TOK_DEC)) {
        return GL_TRUE;
    }
    if (has_side_effect(ast, n->a) || has_side_effect(ast, n->b) ||
        has_side_effect(ast, n->c)) {
        return GL_TRUE;
    }
    /* A call's arguments are a sibling chain from `b`, so the second one and after are not
     * reachable through the three links above. Only walked for a call: elsewhere a sibling is
     * the *next statement*, and following it would make every expression in a block look like
     * every other one. */
    if (n->kind == GLSL_NODE_CALL) {
        for (int32_t s = n->b; s != GLSL_NO_NODE; s = ast->nodes[s].sibling) {
            if (has_side_effect(ast, s)) return GL_TRUE;
        }
        /* **A call to a function this shader defines counts as one**, whatever its body does.
         *
         * `&&` and `||` are a `min` and a `max` here, so both sides are always evaluated - which
         * is indistinguishable from short-circuiting for a pure expression and wrong for one
         * that writes something. A user function's body could write to a global, and deciding
         * whether it does means analysing it; assuming it does not is the mistake that draws.
         * So `a && f(b)` is refused, and a shader that wants it inlines the call itself. */
        const glsl_node_t *callee = (n->a != GLSL_NO_NODE) ? &ast->nodes[n->a]
                                                           : (const glsl_node_t *)0;
        if (callee && callee->kind == GLSL_NODE_IDENTIFIER) {
            for (int32_t i = 0; i < ast->count; i++) {
                const glsl_node_t *f = &ast->nodes[i];
                if (f->kind != GLSL_NODE_FUNCTION || f->c == GLSL_NO_NODE) continue;
                if (f->length != callee->length) continue;
                GLboolean same = GL_TRUE;
                for (size_t c = 0; c < f->length; c++) {
                    if (f->text[c] != callee->text[c]) { same = GL_FALSE; break; }
                }
                if (same) return GL_TRUE;
            }
        }
    }
    return GL_FALSE;
}

/*
 * A comparison. `a < b` and the rest, on scalars; `==` and `!=` additionally on vectors, where
 * GLSL's answer is a single bool that is true only if **every** component agrees.
 *
 * The per-component answers are 0.0 or 1.0, so combining them needs no comparison of its own:
 * `==` over a vector is the `min` of the component equalities, and `!=` is the `max` of the
 * component inequalities. Which of those two it is matters - taking the min for both would make
 * `a != b` mean "every component differs", which is true of far fewer pairs and is wrong in the
 * direction that draws.
 */
static glsl_value_t gen_compare(glsl_gen_t *g, glsl_token_type_t op, glsl_value_t a,
                                glsl_value_t b, int32_t node) {
    uint32_t vopc;
    switch (op) {
        case GLSL_TOK_LT: vopc = GLSL_VOPC_LT_F32; break;
        case GLSL_TOK_GT: vopc = GLSL_VOPC_GT_F32; break;
        case GLSL_TOK_LE: vopc = GLSL_VOPC_LE_F32; break;
        case GLSL_TOK_GE: vopc = GLSL_VOPC_GE_F32; break;
        case GLSL_TOK_EQ: vopc = GLSL_VOPC_EQ_F32; break;
        case GLSL_TOK_NE: vopc = GLSL_VOPC_NEQ_F32; break;
        default: return gen_fail(g, "not a comparison", node);
    }
    const int w = a.count > b.count ? a.count : b.count;
    if (w > 1 && op != GLSL_TOK_EQ && op != GLSL_TOK_NE) {
        return gen_fail(g, "the ordering comparisons take scalars; lessThan and its family "
                           "compare vectors, and they return a bvec this has no register for",
                        node);
    }

    glsl_value_t zero = gen_const(g, 0.0, node);
    if (is_bad(zero)) return zero;
    glsl_value_t one = gen_const(g, 1.0, node);
    if (is_bad(one)) return one;
    glsl_value_t d = gen_alloc(g, 1, node);
    if (is_bad(d)) return d;
    glsl_value_t t = gen_alloc(g, 1, node);
    if (is_bad(t)) return t;

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

/* `&&`, `||` and `^^` over values that are exactly 0.0 or 1.0: `min`, `max`, and the absolute
 * difference. No comparison, and no branch.
 *
 * **GLSL short-circuits `&&` and `||`, and this does not** - both sides are evaluated. That is
 * indistinguishable as long as the right-hand side does nothing, which for a fragment shader
 * means it does not assign; so a right-hand side that assigns is refused rather than quietly
 * evaluated when the language says it would not be. Division by zero on the dead side is not a
 * reason to refuse: it produces an infinity that is then discarded, exactly as on any other
 * implementation that vectorises this. */
static glsl_value_t gen_logical(glsl_gen_t *g, int32_t node) {
    const glsl_node_t *n = &g->ast->nodes[node];
    if (has_side_effect(g->ast, n->b)) {
        return gen_fail(g, "the right of a && or || assigns, and this evaluates both sides - "
                           "so it would run when the language says it does not", node);
    }
    glsl_value_t a = gen_expr(g, n->a);
    if (is_bad(a)) return a;
    glsl_value_t b = gen_expr(g, n->b);
    if (is_bad(b)) return b;
    if (a.count != 1 || b.count != 1) {
        return gen_fail(g, "&& and || take single bools", node);
    }
    glsl_value_t d = gen_alloc(g, 1, node);
    if (is_bad(d)) return d;
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
    if (is_bad(t)) return t;
    glsl_emit_sub_f32(g->code, d.base, a.base, b.base);
    glsl_emit_neg_f32(g->code, t.base, d.base);
    glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, d.base, d.base, t.base);
    return d;
}

static glsl_value_t gen_binary(glsl_gen_t *g, int32_t node) {
    const glsl_node_t *n = &g->ast->nodes[node];
    const glsl_token_type_t op = n->op;

    if (op == GLSL_TOK_AND_AND || op == GLSL_TOK_OR_OR || op == GLSL_TOK_XOR_XOR) {
        return gen_logical(g, node);
    }

    if (op == GLSL_TOK_LT || op == GLSL_TOK_GT || op == GLSL_TOK_LE || op == GLSL_TOK_GE ||
        op == GLSL_TOK_EQ || op == GLSL_TOK_NE) {
        const glsl_type_t clt = glsl_type_of(g->sema, n->a);
        const glsl_type_t crt = glsl_type_of(g->sema, n->b);
        if ((!is_float_family(clt) && !is_bool_family(clt)) ||
            (!is_float_family(crt) && !is_bool_family(crt)) ||
            is_matrix(clt) || is_matrix(crt)) {
            return gen_fail(g, "only float, vector and bool comparisons are generated; the "
                                "integer ones have no verified instruction here", node);
        }
        glsl_value_t ca = gen_expr(g, n->a);
        if (is_bad(ca)) return ca;
        glsl_value_t cb = gen_expr(g, n->b);
        if (is_bad(cb)) return cb;
        return gen_compare(g, op, ca, cb, node);
    }

    if (op != GLSL_TOK_PLUS && op != GLSL_TOK_MINUS && op != GLSL_TOK_STAR &&
        op != GLSL_TOK_SLASH) {
        return gen_fail(g, "this operator has no verified instruction yet: the arithmetic, the "
                           "comparisons and the logical operators are generated, and the rest "
                           "are refused rather than approximated", node);
    }

    const glsl_type_t lt = glsl_type_of(g->sema, n->a);
    const glsl_type_t rt = glsl_type_of(g->sema, n->b);
    const GLboolean int_result = (GLboolean)(is_int_family(lt) && is_int_family(rt));
    if ((!is_float_family(lt) && !is_int_family(lt)) ||
        (!is_float_family(rt) && !is_int_family(rt))) {
        return gen_fail(g, "only float, vec, mat and int arithmetic is generated; bool has no "
                           "verified instruction here", node);
    }
    /* **Integer division is refused, and the reason is the reciprocal.** There is no float
     * divide on this part: `a / b` is `a * rcp(b)`, and `v_rcp_f32` is accurate to one unit in
     * the last place. That is invisible under a float result and decisive under an integer one,
     * where the truncation that follows turns a result a hair below `n` into `n - 1`. `7 / 7`
     * coming out 0 is the shape of it.
     *
     * Getting this right means a quotient fixup - one multiply and a compare to check whether
     * the truncated answer times the divisor has overshot - which is a handful of instructions
     * this does not yet emit. Until it does, the shader is told rather than given an answer
     * that is right for most divisors. */
    glsl_value_t a = gen_expr(g, n->a);
    if (is_bad(a)) return a;
    glsl_value_t b = gen_expr(g, n->b);
    if (is_bad(b)) return b;

    /* **`m * v` and `v * m`, at every square size.** The encoder owns the column-major walk,
     * and the two are separate calls because they are separate products: `v * m` is the one
     * with the transpose, so it is a dot with each *column* rather than a sum over columns. A
     * back end that folded them together would give the same answer twice and be right only
     * for a symmetric matrix. */
    if (op == GLSL_TOK_STAR && is_matrix(lt) && !is_matrix(rt)) {
        const int dim = mat_dim(lt);
        if (b.count == dim) {
            glsl_value_t mv = gen_alloc(g, dim, node);
            if (is_bad(mv)) return mv;
            glsl_emit_mat_mul_vec(g->code, mv.base, a.base, b.base, (uint32_t)dim);
            return mv;
        }
    }
    if (op == GLSL_TOK_STAR && !is_matrix(lt) && is_matrix(rt)) {
        const int dim = mat_dim(rt);
        if (a.count == dim) {
            glsl_value_t vm = gen_alloc(g, dim, node);
            if (is_bad(vm)) return vm;
            glsl_emit_vec_mul_mat(g->code, vm.base, a.base, b.base, (uint32_t)dim);
            return vm;
        }
    }
    if (is_matrix(lt) || is_matrix(rt)) {
        /* What is left is matrix times matrix, and matrix times scalar. The first is n of the
         * products above and the second is componentwise; neither is written yet, and both are
         * named rather than approximated. */
        return gen_fail(g, "the matrix arithmetic generated is a square matrix times a vector "
                           "and a vector times one; matrix by matrix and matrix by scalar are "
                           "not", node);
    }

    const int width = a.count > b.count ? a.count : b.count;
    if (a.count != b.count && a.count != 1 && b.count != 1) {
        return gen_fail(g, "these operand widths do not combine", node);
    }

    /* **Integer division, with the quotient corrected.**
     *
     * There is no divide instruction here, so `a / b` is `a * rcp(b)` and `v_rcp_f32` is
     * accurate to one unit in the last place. Under a float result that is invisible; under an
     * integer one the truncation that follows turns a quotient a hair below `n` into `n - 1`,
     * and `7 / 7` comes out 0. The error is at most one, so one correction settles it: multiply
     * the truncated answer back by the divisor and compare it against the dividend, once in
     * each direction. Both corrections cannot apply at once.
     *
     * Done on magnitudes with the sign applied at the end, because GLSL truncates toward zero
     * and a `trunc` on a negative quotient rounds the wrong way. Division by zero answers zero,
     * which is what `glsl_exec.c` answers - the language calls it undefined and the two paths
     * still have to agree on something. */
    if (int_result && op == GLSL_TOK_SLASH) {
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero)) return zero;
        glsl_value_t one = gen_const(g, 1.0, node);
        if (is_bad(one)) return one;
        glsl_value_t out = gen_alloc(g, width, node);
        if (is_bad(out)) return out;
        for (int i = 0; i < width; i++) {
            const uint32_t mark = gen_mark(g);
            glsl_value_t t = gen_alloc(g, 6, node);
            if (is_bad(t)) return t;
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
        /* One reciprocal **per divisor component**, not per result component: `v / s` with a
         * scalar divisor is one reciprocal and `width` multiplies, which is the shape this is
         * written for and is also the common case. */
        glsl_value_t r = gen_alloc(g, b.count, node);
        if (is_bad(r)) return r;
        for (int i = 0; i < b.count; i++) {
            glsl_emit_vop1_op(g->code, GLSL_VOP1_RCP_F32, r.base + (uint32_t)i,
                              b.base + (uint32_t)i);
        }
        glsl_value_t out = gen_alloc(g, width, node);
        if (is_bad(out)) return out;
        for (int i = 0; i < width; i++) {
            glsl_emit_mul_f32(g->code, out.base + (uint32_t)i, comp_of(a, i), comp_of(r, i));
        }
        return out;
    }

    glsl_value_t out = gen_alloc(g, width, node);
    if (is_bad(out)) return out;
    for (int i = 0; i < width; i++) {
        gen_binop_component(g, op, out.base + (uint32_t)i, comp_of(a, i), comp_of(b, i));
    }
    /* **An integer result is kept whole**, which is what makes an int an int here: the
     * reference writes `(float)(int)x` after an integer operation and this writes the
     * instruction that does the same thing. Addition, subtraction and multiplication of whole
     * floats are already whole, so this is not correcting them - it is the one place the
     * representation is stated, and it costs an instruction on operations that are exact
     * anyway rather than leaving the invariant to hold by luck. `v_trunc_f32` rounds toward
     * zero, which is the direction C and GLSL both take. */
    if (int_result) {
        for (int i = 0; i < width; i++) {
            glsl_emit_vop1_op(g->code, GLSL_VOP1_TRUNC_F32, out.base + (uint32_t)i,
                              out.base + (uint32_t)i);
        }
    }
    return out;
}

/* A swizzle read: `v.xyz`, `c.rgba`, `t.st`. The vocabularies were checked by the semantic
 * stage - mixing them, and naming a component past the end, are both already refused - so this
 * only has to map a letter to an index. A repeated component (`v.xx`) is a legal read and
 * becomes two moves from the same register. */
static glsl_value_t gen_field(glsl_gen_t *g, int32_t node) {
    const glsl_node_t *n = &g->ast->nodes[node];
    glsl_value_t src = gen_expr(g, n->a);
    if (is_bad(src)) return src;

    const int len = (int)n->length;
    if (len < 1 || len > 4) return gen_fail(g, "a swizzle names one to four components", node);

    glsl_value_t out = gen_alloc(g, len, node);
    if (is_bad(out)) return out;

    for (int i = 0; i < len; i++) {
        int idx;
        switch (n->text[i]) {
            case 'x': case 'r': case 's': idx = 0; break;
            case 'y': case 'g': case 't': idx = 1; break;
            case 'z': case 'b': case 'p': idx = 2; break;
            case 'w': case 'a': case 'q': idx = 3; break;
            default: return gen_fail(g, "not a component name", node);
        }
        if (idx >= src.count) return gen_fail(g, "a component past the end of the value", node);
        glsl_emit_mov(g->code, out.base + (uint32_t)i, src.base + (uint32_t)idx);
    }
    return out;
}

/* The type a name in call position constructs, or GLSL_TYPE_ERROR if it names no type. */
static glsl_type_t constructor_target(const glsl_node_t *callee) {
    static const struct { const char *name; glsl_type_t type; } ctors[] = {
        {"float", GLSL_TYPE_FLOAT},
        {"vec2", GLSL_TYPE_VEC2}, {"vec3", GLSL_TYPE_VEC3}, {"vec4", GLSL_TYPE_VEC4},
        {"mat2", GLSL_TYPE_MAT2}, {"mat3", GLSL_TYPE_MAT3}, {"mat4", GLSL_TYPE_MAT4},
        {"bool", GLSL_TYPE_BOOL}, {"int", GLSL_TYPE_INT},
    };
    for (size_t i = 0; i < sizeof(ctors) / sizeof(ctors[0]); i++) {
        size_t len = 0;
        while (ctors[i].name[len] != '\0') len++;
        if (len != callee->length) continue;
        GLboolean match = GL_TRUE;
        for (size_t k = 0; k < len; k++) {
            if (callee->text[k] != ctors[i].name[k]) { match = GL_FALSE; break; }
        }
        if (match) return ctors[i].type;
    }
    return GLSL_TYPE_ERROR;
}

/*
 * A constructor.
 *
 * Two forms, and GLSL distinguishes them by component count rather than argument count:
 *
 *   `vec4(1.0)`  - one scalar fills every component.
 *   `mat4(1.0)`  - one scalar fills the *diagonal*, and the rest is zero. Not the same rule, and
 *                  filling a matrix the way a vector is filled produces a matrix of ones, which
 *                  transforms everything to the same point - visible on hardware, invisible here.
 *   `vec4(v3, 1.0)` - components taken in order until the target is full.
 */
static glsl_value_t gen_construct(glsl_gen_t *g, glsl_type_t target, int32_t first_arg,
                                  int32_t node) {
    /* **The two scalar conversions, which are not component copies.**
     *
     * `bool(x)` is `x != 0` and `int(x)` truncates toward zero - neither is the "take components
     * until the target is full" rule below, and running them through it would copy the float
     * across unchanged. `bool(2.0)` would then be 2.0, which is true in a condition and wrong
     * everywhere the value itself is read; craft's `bool(ortho)` is exactly that call. */
    if (target == GLSL_TYPE_BOOL || target == GLSL_TYPE_INT) {
        if (first_arg == GLSL_NO_NODE || g->ast->nodes[first_arg].sibling != GLSL_NO_NODE) {
            return gen_fail(g, "bool() and int() take one argument", node);
        }
        glsl_value_t s = gen_expr(g, first_arg);
        if (is_bad(s)) return s;
        if (s.count != 1) return gen_fail(g, "bool() and int() take a scalar", node);
        glsl_value_t out = gen_alloc(g, 1, node);
        if (is_bad(out)) return out;
        if (target == GLSL_TYPE_INT) {
            glsl_emit_vop1_op(g->code, GLSL_VOP1_TRUNC_F32, out.base, s.base);
            return out;
        }
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero)) return zero;
        glsl_value_t one = gen_const(g, 1.0, node);
        if (is_bad(one)) return one;
        glsl_emit_cmp(g->code, GLSL_VOPC_NEQ_F32, s.base, zero.base);
        glsl_emit_cndmask(g->code, out.base, zero.base, one.base);
        return out;
    }

    const int needed = glsl_type_components(target);
    glsl_value_t out = gen_alloc(g, needed, node);
    if (is_bad(out)) return out;

    /* Count the arguments and their components, so the one-scalar forms are told apart. */
    int args = 0, supplied = 0;
    for (int32_t a = first_arg; a != GLSL_NO_NODE; a = g->ast->nodes[a].sibling) {
        const glsl_type_t at = glsl_type_of(g->sema, a);
        /* An `int` argument needs no conversion: it is already a whole float in a register, so
         * `float(i)` is a move and `vec3(i, x, y)` packs it like any other component. A `bool`
         * is 0.0 or 1.0 and would behave the same, but GLSL's `float(b)` is a conversion this
         * has not been asked for yet, so it stays refused and named. */
        if (!is_float_family(at) && !is_int_family(at)) {
            return gen_fail(g, "only float, vec, mat and int constructor arguments are "
                               "generated", node);
        }
        supplied += glsl_type_components(at);
        args++;
    }

    if (args == 1 && supplied == 1) {
        glsl_value_t s = gen_expr(g, first_arg);
        if (is_bad(s)) return s;
        if (is_matrix(target)) {
            /* The diagonal takes the scalar, everything else is zero. Column-major, so the
             * diagonal of an n x n matrix is at 0, n+1, 2n+2 ... */
            int n = target == GLSL_TYPE_MAT2 ? 2 : (target == GLSL_TYPE_MAT3 ? 3 : 4);
            for (int i = 0; i < needed; i++) {
                if (i % (n + 1) == 0) {
                    glsl_emit_mov(g->code, out.base + (uint32_t)i, s.base);
                } else {
                    glsl_emit_mov_imm(g->code, out.base + (uint32_t)i, 0u);
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
        if (is_bad(v)) return v;
        for (int i = 0; i < v.count && filled < needed; i++, filled++) {
            glsl_emit_mov(g->code, out.base + (uint32_t)filled, v.base + (uint32_t)i);
        }
    }
    if (filled < needed) return gen_fail(g, "too few components for this constructor", node);
    return out;
}

/* -------------------------------------------------------------------------
 * Places: what an assignment writes into
 *
 * A value here is one VGPR a component with no packing, so **writing through a swizzle is not a
 * masked move** - it is a move into each of the registers the swizzle names. `c.rgb = v` is
 * three moves into the first three of `c`'s registers, `c.a = 1.0` is one into the fourth, and
 * `c.zyx = v` is three moves that cross over. It is only this simple because of the
 * representation: a back end that packed four floats into one register would need a write mask,
 * and there is no measured one here.
 *
 * The semantic stage has already refused everything that is not a place - a repeated component
 * (`v.xx = ...`), a uniform, an attribute, a `const` - so this maps letters to indices and
 * nothing more.
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t reg[4];   /* the registers this place names, in the order it names them */
    int count;
} gen_place_t;

static GLboolean gen_place_of(glsl_gen_t *g, int32_t node, gen_place_t *out) {
    if (node == GLSL_NO_NODE) {
        (void)gen_fail(g, "an assignment with no destination", node);
        return GL_FALSE;
    }
    const glsl_node_t *n = &g->ast->nodes[node];
    if (n->kind == GLSL_NODE_IDENTIFIER) {
        glsl_gen_var_t *v = gen_find(g, n->text, n->length);
        if (!v) {
            (void)gen_fail(g, "assigning to a name with no register: only locals declared in "
                              "this body and the shader's own inputs are generated so far",
                           node);
            return GL_FALSE;
        }
        if (v->value.count > 4) {
            (void)gen_fail(g, "a matrix is not assignable here", node);
            return GL_FALSE;
        }
        out->count = v->value.count;
        for (int i = 0; i < out->count; i++) out->reg[i] = v->value.base + (uint32_t)i;
        return GL_TRUE;
    }
    if (n->kind == GLSL_NODE_FIELD) {
        gen_place_t base;
        if (!gen_place_of(g, n->a, &base)) return GL_FALSE;
        const int len = (int)n->length;
        if (len < 1 || len > 4) {
            (void)gen_fail(g, "a swizzle names one to four components", node);
            return GL_FALSE;
        }
        out->count = len;
        for (int i = 0; i < len; i++) {
            int idx;
            switch (n->text[i]) {
                case 'x': case 'r': case 's': idx = 0; break;
                case 'y': case 'g': case 't': idx = 1; break;
                case 'z': case 'b': case 'p': idx = 2; break;
                case 'w': case 'a': case 'q': idx = 3; break;
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
    (void)gen_fail(g, "this is not something with a register to write into; an array element "
                      "has no instruction selection yet", node);
    return GL_FALSE;
}

/* -------------------------------------------------------------------------
 * The built-in library
 *
 * GLSL section 8, lowered onto the instructions in `tools/shader/gl2-fragment.s`. Three groups,
 * and the boundary between them is what has a verified encoding rather than what is easy:
 *
 *   - **One instruction.** `sqrt`, `inversesqrt`, `floor`, `ceil`, `fract`, `min`, `max` -
 *     VOP1 or VOP2 a component and nothing else.
 *   - **A short sequence.** `abs`, `sign`, `clamp`, `mix`, `step`, `smoothstep`, `mod`, `pow`,
 *     `exp`, `log`, `dot`, `length`, `distance`, `normalize`, `cross`, `reflect`,
 *     `faceforward`, `radians`, `degrees`, `sin`, `cos`, `tan` - each built from those, with
 *     the identity it uses written beside it.
 *   - **Refused.** `asin`, `acos`, `atan`, `refract`, the matrix functions and the vector
 *     relational ones. There is no instruction for them and no lowering that is not a
 *     polynomial somebody chose; approximating a transcendental to an unmeasured accuracy is
 *     exactly the failure this back end is arranged around (D009), so the shader does not
 *     compile and the message says which function stopped it.
 *
 * # The two traps in here
 *
 * **`v_sin_f32` does not take radians.** It computes `sin(2*pi*x)`, so GLSL's `sin` is a
 * multiply by `1/2pi` and then the instruction - which is what ACO emits
 * (mesa/src/amd/compiler/aco_instruction_selection.cpp, `nir_op_fsin`, the 0x3e22f983 it
 * multiplies by). Feeding radians straight in gives a smooth periodic function of the right
 * shape and the wrong period, which looks like a shader that works until something has to line
 * up with it.
 *
 * **`v_exp_f32` and `v_log_f32` are base two.** `exp` is `exp2(x * log2 e)` and `log` is
 * `log2(x) * ln 2`. Taking them for the natural pair is wrong by a factor of 1.44 - a number
 * small enough to look like a tuning problem rather than a compiler bug.
 * ------------------------------------------------------------------------- */

static GLboolean nm_is(const char *text, size_t len, const char *lit) {
    size_t n = 0;
    while (lit[n] != '\0') n++;
    if (n != len) return GL_FALSE;
    for (size_t i = 0; i < n; i++) {
        if (text[i] != lit[i]) return GL_FALSE;
    }
    return GL_TRUE;
}

/* `text` is not NUL-terminated - it points into the shader source - so the length is checked
 * before any character is, and never after. */
static GLboolean nm_prefix(const char *text, size_t len, const char *lit) {
    size_t n = 0;
    while (lit[n] != '\0') n++;
    if (len < n) return GL_FALSE;
    for (size_t i = 0; i < n; i++) {
        if (text[i] != lit[i]) return GL_FALSE;
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
        glsl_emit_vop2_op(g->code, opcode, d.base + (uint32_t)i, comp_of(a, i), comp_of(b, i));
    }
}

/* `dot(a, b)` into one register: a multiply and then a fused multiply-add a component, which is
 * the shortest form this instruction set has for it. The destination is freshly allocated and so
 * sits above both operands - the accumulate reads `d` and would otherwise need to. */
static glsl_value_t gen_dot(glsl_gen_t *g, glsl_value_t a, glsl_value_t b, int32_t node) {
    const int w = a.count > b.count ? a.count : b.count;
    glsl_value_t d = gen_alloc(g, 1, node);
    if (is_bad(d)) return d;
    glsl_emit_mul_f32(g->code, d.base, comp_of(a, 0), comp_of(b, 0));
    for (int i = 1; i < w; i++) {
        glsl_emit_fmac_f32(g->code, d.base, comp_of(a, i), comp_of(b, i));
    }
    return d;
}

/* `1 / sqrt(dot(v, v))`, the scale `normalize` and `length` are both built on. */
static glsl_value_t gen_inv_length(glsl_gen_t *g, glsl_value_t v, int32_t node) {
    glsl_value_t d2 = gen_dot(g, v, v, node);
    if (is_bad(d2)) return d2;
    glsl_value_t r = gen_alloc(g, 1, node);
    if (is_bad(r)) return r;
    glsl_emit_vop1_op(g->code, GLSL_VOP1_RSQ_F32, r.base, d2.base);
    return r;
}

/* `d = (x cmp 0) ? one : zero`, component-wise, with the two constants materialised once.
 * `v_cndmask` takes its **false** value in src0, which is why `zero` is passed there. */
static glsl_value_t gen_select_on_sign(glsl_gen_t *g, glsl_value_t x, uint32_t vopc,
                                       double if_true, double if_false, int32_t node) {
    glsl_value_t z = gen_const(g, 0.0, node);
    if (is_bad(z)) return z;
    glsl_value_t t = gen_const(g, if_true, node);
    if (is_bad(t)) return t;
    glsl_value_t f = gen_const(g, if_false, node);
    if (is_bad(f)) return f;
    glsl_value_t d = gen_alloc(g, x.count, node);
    if (is_bad(d)) return d;
    for (int i = 0; i < x.count; i++) {
        glsl_emit_cmp(g->code, vopc, comp_of(x, i), z.base);
        glsl_emit_cndmask(g->code, d.base + (uint32_t)i, f.base, t.base);
    }
    return d;
}

#define GEN_MAX_ARGS 4

/* How many arguments a call has, counted without evaluating any - `texture2D` has to check its
 * arity before it looks at the first one, because the first one is a sampler and evaluating a
 * sampler is not a thing. */
static int argc_of(const glsl_gen_t *g, int32_t first_arg) {
    int n = 0;
    for (int32_t a = first_arg; a != GLSL_NO_NODE; a = g->ast->nodes[a].sibling) n++;
    return n;
}

static glsl_value_t gen_builtin(glsl_gen_t *g, const glsl_node_t *callee, int32_t first_arg,
                                int32_t node) {
    const char *const nm = callee->text;
    const size_t len = callee->length;

    /* The names that are real GLSL and have no instruction here. Named one at a time, because
     * "this built-in has no instruction selection" over a call to `atan` reads as a gap in the
     * compiler, which is what it is - and the alternative, a polynomial of somebody's choosing,
     * would read as working. */
    if (nm_is(nm, len, "asin") || nm_is(nm, len, "acos") || nm_is(nm, len, "atan")) {
        return gen_fail(g, "the inverse trigonometric functions have no instruction on this "
                           "part, and a polynomial of unmeasured accuracy is not generated in "
                           "their place", node);
    }
    if (nm_is(nm, len, "refract")) {
        return gen_fail(g, "refract needs a square root of a value that may be negative and a "
                           "select on it, which this has no lowering for yet", node);
    }
    if (nm_is(nm, len, "matrixCompMult") || nm_is(nm, len, "transpose") ||
        nm_is(nm, len, "outerProduct")) {
        return gen_fail(g, "the matrix built-ins are not generated; only mat4 * vec4 is", node);
    }
    /* `texture2D` is generated; the rest of section 8.7 is not. Each of the others needs
     * something this has no measurement for - a cube's coordinate is a direction the hardware
     * resolves to a face, a volume's is three components, a `Proj` form divides by its last,
     * and a shadow lookup compares rather than returns. The dimension is one field in the
     * instruction (`tools/shader/gl2-fragment.s` pins all four), so these are a short step
     * rather than a different problem - but a step nobody has taken. */
    if (nm_is(nm, len, "texture2D")) {
        if (argc_of(g, first_arg) != 2) {
            return gen_fail(g, "texture2D takes a sampler and a vec2", node);
        }
        const glsl_node_t *sn = &g->ast->nodes[first_arg];
        if (sn->kind != GLSL_NODE_IDENTIFIER) {
            return gen_fail(g, "the sampler has to be named directly; there is no way to carry "
                               "one in a variable here", node);
        }
        uint32_t set = 0u;
        GLboolean found = GL_FALSE;
        for (int i = 0; i < g->sampler_count; i++) {
            if (g->samplers[i].name_len == sn->length &&
                nm_is(sn->text, sn->length, g->samplers[i].name)) {
                set = g->samplers[i].set;
                found = GL_TRUE;
                break;
            }
        }
        if (!found) {
            return gen_fail(g, "this name is not a sampler the draw path loads descriptors for",
                            node);
        }
        const int32_t coord_node = g->ast->nodes[first_arg].sibling;
        const glsl_type_t ct = glsl_type_of(g->sema, coord_node);
        if (ct != GLSL_TYPE_VEC2) {
            return gen_fail(g, "texture2D's coordinate is a vec2", node);
        }
        glsl_value_t uv = gen_expr(g, coord_node);
        if (is_bad(uv)) return uv;
        if (uv.count != 2) return gen_fail(g, "texture2D's coordinate is a vec2", node);

        /* **The sample's four registers are allocated after the coordinate**, so they cannot
         * overlap it - `image_sample` reads `vaddr` and writes `vdata`, and an overlap would
         * have it read back what it had just written for the second component. */
        glsl_value_t out = gen_alloc(g, 4, node);
        if (is_bad(out)) return out;
        const uint32_t srsrc = GLSL_GEN_TEX_SGPR_BASE + set * GLSL_GEN_TEX_SGPR_STRIDE;
        glsl_emit_image_sample(g->code, GLSL_MIMG_SAMPLE, GLSL_IMG_DIM_2D, out.base, uv.base,
                               srsrc, srsrc + 8u);
        /* **A sample is not in order with what follows it.** Without this the next instruction
         * reads the destination before the texture unit has written it, which is the same
         * hazard `s_waitcnt lgkmcnt(0)` covers for the uniform block - and obSCEne measured
         * that one returning all zeros when the wait was left out (`-6c0d`). */
        glsl_emit_s_waitcnt_vm(g->code);
        return out;
    }
    if (nm_prefix(nm, len, "texture") || nm_prefix(nm, len, "shadow")) {
        return gen_fail(g, "only texture2D is generated; the projective, cube, volume and "
                           "shadow lookups each need something this has not measured", node);
    }

    /* The arguments, left to right. Evaluated once each and into registers that outlive the
     * lowering below - a built-in that used an argument twice (`normalize`, `dot(v, v)`) must
     * not evaluate its expression twice. */
    glsl_value_t arg[GEN_MAX_ARGS];
    int argc = 0;
    for (int32_t a = first_arg; a != GLSL_NO_NODE; a = g->ast->nodes[a].sibling) {
        if (argc == GEN_MAX_ARGS) {
            return gen_fail(g, "more arguments than any built-in generated here takes", node);
        }
        const glsl_type_t at = glsl_type_of(g->sema, a);
        /* A `bool` or a `bvec` is a float per component here, so the reductions take one
         * directly; an `int` is a whole float and compares and scales like any other. What is
         * still refused is a matrix, whose components are a square and not a run. */
        if ((!is_float_family(at) && !is_bool_family(at) && !is_int_family(at)) ||
            is_matrix(at)) {
            return gen_fail(g, "this built-in is generated for float, int and bool arguments "
                               "only",
                            node);
        }
        arg[argc] = gen_expr(g, a);
        if (is_bad(arg[argc])) return arg[argc];
        argc++;
    }
    if (argc == 0) {
        return gen_fail(g, "a built-in with no arguments is not one this generates", node);
    }

    /* The widest argument: every component-wise built-in here returns that width, and the rules
     * above let a scalar stand in for any of them. */
    int w = arg[0].count;
    for (int i = 1; i < argc; i++) {
        if (arg[i].count > w) w = arg[i].count;
    }

    /* --- the vector relational family ------------------------------------
     *
     * **A `bvec` is what a `bool` already was, one per component**: a float that is 0.0 or 1.0.
     * So the comparisons are the scalar compare-and-select run down the two operands, and the
     * reductions need no comparison at all - `any` is the `max` of values that are only ever 0
     * or 1, `all` is the `min`, and `not` is `1 - x`. The representation is what makes that
     * true, and it is the same reason `&&` is a `min` here. */
    {
        struct { const char *name; uint32_t op; } const REL[] = {
            {"lessThan",         GLSL_VOPC_LT_F32},
            {"lessThanEqual",    GLSL_VOPC_LE_F32},
            {"greaterThan",      GLSL_VOPC_GT_F32},
            {"greaterThanEqual", GLSL_VOPC_GE_F32},
            {"equal",            GLSL_VOPC_EQ_F32},
            {"notEqual",         GLSL_VOPC_NEQ_F32},
        };
        for (size_t i = 0; i < sizeof(REL) / sizeof(REL[0]); i++) {
            if (!nm_is(nm, len, REL[i].name)) continue;
            if (argc != 2) return gen_fail(g, "wrong number of arguments", node);
            if (arg[0].count != arg[1].count) {
                return gen_fail(g, "the vector relational functions take two operands of the "
                                   "same width", node);
            }
            glsl_value_t rzero = gen_const(g, 0.0, node);
            if (is_bad(rzero)) return rzero;
            glsl_value_t rone = gen_const(g, 1.0, node);
            if (is_bad(rone)) return rone;
            glsl_value_t d = gen_alloc(g, arg[0].count, node);
            if (is_bad(d)) return d;
            for (int cc = 0; cc < arg[0].count; cc++) {
                glsl_emit_cmp(g->code, REL[i].op, comp_of(arg[0], cc), comp_of(arg[1], cc));
                glsl_emit_cndmask(g->code, d.base + (uint32_t)cc, rzero.base, rone.base);
            }
            return d;
        }
    }
    /* --- the derivatives -------------------------------------------------
     *
     * **A derivative is the difference between this lane and its neighbour in the quad**, which
     * is the whole of what makes it a quad operation: the value has to exist in the lane next
     * door, and in a lane the primitive does not cover it only exists because whole-quad mode
     * kept that lane running. `glsl_ps.c` turns WQM on for a shader that names one, the same
     * way it does for a shader that samples, and for the same reason.
     *
     * `dFdy`'s sign follows the window's y, which counts down the screen - so this is the
     * bottom row less the top, matching the reference's own finite difference rather than
     * GL's bottom-left convention. */
    if (nm_is(nm, len, "dFdx") || nm_is(nm, len, "dFdy") || nm_is(nm, len, "fwidth")) {
        if (argc != 1) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, arg[0].count, node);
        if (is_bad(d)) return d;
        const GLboolean want_x = (GLboolean)!nm_is(nm, len, "dFdy");
        const GLboolean want_y = (GLboolean)!nm_is(nm, len, "dFdx");
        for (int cc = 0; cc < arg[0].count; cc++) {
            const uint32_t mark = gen_mark(g);
            glsl_value_t t = gen_alloc(g, 2, node);
            if (is_bad(t)) return t;
            const uint32_t near = t.base + 0u, acc = t.base + 1u;
            const uint32_t s = comp_of(arg[0], cc);
            if (want_x) {
                glsl_emit_dpp_mov(g->code, near, s, GLSL_DPP_QUAD_X_NEAR);
                glsl_emit_dpp_sub(g->code, acc, s, near, GLSL_DPP_QUAD_X_FAR);
                if (!want_y) glsl_emit_mov(g->code, d.base + (uint32_t)cc, acc);
            }
            if (want_y) {
                const uint32_t ydst = want_x ? near : (d.base + (uint32_t)cc);
                glsl_emit_dpp_mov(g->code, t.base + 0u, s, GLSL_DPP_QUAD_Y_NEAR);
                glsl_emit_dpp_sub(g->code, ydst, s, t.base + 0u, GLSL_DPP_QUAD_Y_FAR);
                if (want_x) {
                    /* `fwidth` is |dFdx| + |dFdy|, and an absolute value here is `max(v, -v)`
                     * - the same two instructions the integer divide uses. */
                    glsl_value_t n2 = gen_alloc(g, 1, node);
                    if (is_bad(n2)) return n2;
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
        if (argc != 1) return gen_fail(g, "wrong number of arguments", node);
        const uint32_t rop = nm_is(nm, len, "any") ? GLSL_VOP2_MAX_F32 : GLSL_VOP2_MIN_F32;
        glsl_value_t d = gen_alloc(g, 1, node);
        if (is_bad(d)) return d;
        glsl_emit_mov(g->code, d.base, arg[0].base);
        for (int cc = 1; cc < arg[0].count; cc++) {
            glsl_emit_vop2_op(g->code, rop, d.base, d.base, comp_of(arg[0], cc));
        }
        return d;
    }
    if (nm_is(nm, len, "not")) {
        if (argc != 1) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t rone = gen_const(g, 1.0, node);
        if (is_bad(rone)) return rone;
        glsl_value_t d = gen_alloc(g, arg[0].count, node);
        if (is_bad(d)) return d;
        for (int cc = 0; cc < arg[0].count; cc++) {
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)cc, rone.base, comp_of(arg[0], cc));
        }
        return d;
    }

    /* --- one instruction a component ------------------------------------ */
    struct { const char *name; uint32_t op; int args; } const MAP[] = {
        {"sqrt",        GLSL_VOP1_SQRT_F32,  1},
        {"inversesqrt", GLSL_VOP1_RSQ_F32,   1},
        {"floor",       GLSL_VOP1_FLOOR_F32, 1},
        {"ceil",        GLSL_VOP1_CEIL_F32,  1},
        {"fract",       GLSL_VOP1_FRACT_F32, 1},
        {"exp2",        GLSL_VOP1_EXP_F32,   1},
        {"log2",        GLSL_VOP1_LOG_F32,   1},
        {"min",         GLSL_VOP2_MIN_F32,   2},
        {"max",         GLSL_VOP2_MAX_F32,   2},
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (!nm_is(nm, len, MAP[i].name)) continue;
        if (argc != MAP[i].args) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        if (MAP[i].args == 1) {
            gen_map1(g, MAP[i].op, d, arg[0]);
        } else {
            gen_map2(g, MAP[i].op, d, arg[0], arg[1]);
        }
        return d;
    }

    /* --- a multiply by a constant --------------------------------------- */
    struct { const char *name; double k; } const SCALE[] = {
        {"radians", 3.14159265358979323846 / 180.0},
        {"degrees", 180.0 / 3.14159265358979323846},
    };
    for (size_t i = 0; i < sizeof(SCALE) / sizeof(SCALE[0]); i++) {
        if (!nm_is(nm, len, SCALE[i].name)) continue;
        if (argc != 1) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t k = gen_const(g, SCALE[i].k, node);
        if (is_bad(k)) return k;
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c), k.base);
        }
        return d;
    }

    /* --- the trigonometric pair, in revolutions ------------------------- */
    if (nm_is(nm, len, "sin") || nm_is(nm, len, "cos") || nm_is(nm, len, "tan")) {
        if (argc != 1) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t k = gen_const(g, 1.0 / (2.0 * 3.14159265358979323846), node);
        if (is_bad(k)) return k;
        glsl_value_t rev = gen_alloc(g, w, node);
        if (is_bad(rev)) return rev;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, rev.base + (uint32_t)c, comp_of(arg[0], c), k.base);
        }
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        if (nm_is(nm, len, "sin")) {
            gen_map1(g, GLSL_VOP1_SIN_F32, d, rev);
            return d;
        }
        if (nm_is(nm, len, "cos")) {
            gen_map1(g, GLSL_VOP1_COS_F32, d, rev);
            return d;
        }
        /* `tan` is the quotient, so it is a sine, a cosine, a reciprocal and a multiply. */
        glsl_value_t co = gen_alloc(g, w, node);
        if (is_bad(co)) return co;
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
        if (argc != 1) return gen_fail(g, "wrong number of arguments", node);
        const GLboolean is_exp = nm_is(nm, len, "exp") ? GL_TRUE : GL_FALSE;
        glsl_value_t k = gen_const(g, is_exp ? 1.4426950408889634 : 0.6931471805599453, node);
        if (is_bad(k)) return k;
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        if (is_exp) {
            /* exp(x) = 2^(x * log2 e) */
            for (int c = 0; c < w; c++) {
                glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c), k.base);
            }
            gen_map1(g, GLSL_VOP1_EXP_F32, d, d);
        } else {
            /* log(x) = log2(x) * ln 2 */
            gen_map1(g, GLSL_VOP1_LOG_F32, d, arg[0]);
            for (int c = 0; c < w; c++) {
                glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, d.base + (uint32_t)c, k.base);
            }
        }
        return d;
    }

    /* `pow(x, y) = 2^(y * log2 x)`, which is what the hardware's pair composes to. Undefined in
     * GLSL for a negative `x`, and `v_log_f32` of a negative is a NaN, so the two agree. */
    if (nm_is(nm, len, "pow")) {
        if (argc != 2) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        gen_map1(g, GLSL_VOP1_LOG_F32, d, arg[0]);
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, d.base + (uint32_t)c,
                              comp_of(arg[1], c));
        }
        gen_map1(g, GLSL_VOP1_EXP_F32, d, d);
        return d;
    }

    /* `abs(x) = max(x, -x)`. Two instructions a component and no constant, rather than the
     * sign-bit clear - which would need `v_and_b32` and a literal mask, neither of which is in
     * the pinned table. */
    if (nm_is(nm, len, "abs")) {
        if (argc != 1) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_neg_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c));
            glsl_emit_vop2_op(g->code, GLSL_VOP2_MAX_F32, d.base + (uint32_t)c,
                              d.base + (uint32_t)c, comp_of(arg[0], c));
        }
        return d;
    }

    /* `sign(x)` is -1, 0 or 1, and zero is its own case - so it is two selects and a subtract
     * rather than one select, which would give 1 for x == 0. */
    if (nm_is(nm, len, "sign")) {
        if (argc != 1) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t pos = gen_select_on_sign(g, arg[0], GLSL_VOPC_GT_F32, 1.0, 0.0, node);
        if (is_bad(pos)) return pos;
        glsl_value_t neg = gen_select_on_sign(g, arg[0], GLSL_VOPC_LT_F32, 1.0, 0.0, node);
        if (is_bad(neg)) return neg;
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)c, pos.base + (uint32_t)c,
                              neg.base + (uint32_t)c);
        }
        return d;
    }

    /* `clamp(x, lo, hi) = min(max(x, lo), hi)`, in that order: the other order gives `lo` for a
     * NaN where this gives `hi`, and GLSL says nothing about either, but `min(max(...))` is what
     * every other implementation does. */
    if (nm_is(nm, len, "clamp")) {
        if (argc != 3) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        gen_map2(g, GLSL_VOP2_MAX_F32, d, arg[0], arg[1]);
        gen_map2(g, GLSL_VOP2_MIN_F32, d, d, arg[2]);
        return d;
    }

    /* `mix(a, b, t) = a + (b - a) * t`. Three instructions a component with the fused
     * multiply-add, and exactly `a` when t is 0 and exactly `b` when it is 1 - which the other
     * form, `a*(1-t) + b*t`, is not. */
    if (nm_is(nm, len, "mix")) {
        if (argc != 3) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        glsl_value_t diff = gen_alloc(g, w, node);
        if (is_bad(diff)) return diff;
        for (int c = 0; c < w; c++) {
            glsl_emit_sub_f32(g->code, diff.base + (uint32_t)c, comp_of(arg[1], c),
                              comp_of(arg[0], c));
            glsl_emit_mov(g->code, d.base + (uint32_t)c, comp_of(arg[0], c));
            glsl_emit_fmac_f32(g->code, d.base + (uint32_t)c, diff.base + (uint32_t)c,
                               comp_of(arg[2], c));
        }
        return d;
    }

    /* `mod(x, y) = x - y * floor(x / y)`, which is GLSL's definition verbatim. */
    if (nm_is(nm, len, "mod")) {
        if (argc != 2) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t r = gen_alloc(g, arg[1].count, node);
        if (is_bad(r)) return r;
        gen_map1(g, GLSL_VOP1_RCP_F32, r, arg[1]);
        glsl_value_t q = gen_alloc(g, w, node);
        if (is_bad(q)) return q;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, q.base + (uint32_t)c, comp_of(arg[0], c), comp_of(r, c));
        }
        gen_map1(g, GLSL_VOP1_FLOOR_F32, q, q);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, q.base + (uint32_t)c,
                              comp_of(arg[1], c));
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c),
                              d.base + (uint32_t)c);
        }
        return d;
    }

    /* `step(edge, x)` is 0 below the edge and 1 at or above it - so the comparison is `x < edge`
     * and the **false** arm is the 1, which is the arm `v_cndmask` takes from `vsrc1`. */
    if (nm_is(nm, len, "step")) {
        if (argc != 2) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero)) return zero;
        glsl_value_t one = gen_const(g, 1.0, node);
        if (is_bad(one)) return one;
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_cmp(g->code, GLSL_VOPC_LT_F32, comp_of(arg[1], c), comp_of(arg[0], c));
            glsl_emit_cndmask(g->code, d.base + (uint32_t)c, one.base, zero.base);
        }
        return d;
    }

    /* `smoothstep(e0, e1, x)`: `t = clamp((x - e0) / (e1 - e0), 0, 1)`, then `t*t*(3 - 2t)`.
     * GLSL 1.10 section 8.3 gives this expansion, so it is transcribed rather than chosen. */
    if (nm_is(nm, len, "smoothstep")) {
        if (argc != 3) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero)) return zero;
        glsl_value_t one = gen_const(g, 1.0, node);
        if (is_bad(one)) return one;
        glsl_value_t three = gen_const(g, 3.0, node);
        if (is_bad(three)) return three;
        glsl_value_t two = gen_const(g, 2.0, node);
        if (is_bad(two)) return two;
        glsl_value_t t = gen_alloc(g, w, node);
        if (is_bad(t)) return t;
        glsl_value_t den = gen_alloc(g, w, node);
        if (is_bad(den)) return den;
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
        if (is_bad(d)) return d;
        for (int c = 0; c < w; c++) {
            /* 3 - 2t, then t*t times it. */
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, t.base + (uint32_t)c, two.base);
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)c, three.base, d.base + (uint32_t)c);
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, d.base + (uint32_t)c,
                              t.base + (uint32_t)c);
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, d.base + (uint32_t)c,
                              t.base + (uint32_t)c);
        }
        return d;
    }

    /* --- the geometric ones --------------------------------------------- */

    if (nm_is(nm, len, "dot")) {
        if (argc != 2) return gen_fail(g, "wrong number of arguments", node);
        return gen_dot(g, arg[0], arg[1], node);
    }

    if (nm_is(nm, len, "length")) {
        if (argc != 1) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t d2 = gen_dot(g, arg[0], arg[0], node);
        if (is_bad(d2)) return d2;
        glsl_value_t d = gen_alloc(g, 1, node);
        if (is_bad(d)) return d;
        glsl_emit_vop1_op(g->code, GLSL_VOP1_SQRT_F32, d.base, d2.base);
        return d;
    }

    if (nm_is(nm, len, "distance")) {
        if (argc != 2) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t diff = gen_alloc(g, w, node);
        if (is_bad(diff)) return diff;
        for (int c = 0; c < w; c++) {
            glsl_emit_sub_f32(g->code, diff.base + (uint32_t)c, comp_of(arg[0], c),
                              comp_of(arg[1], c));
        }
        glsl_value_t d2 = gen_dot(g, diff, diff, node);
        if (is_bad(d2)) return d2;
        glsl_value_t d = gen_alloc(g, 1, node);
        if (is_bad(d)) return d;
        glsl_emit_vop1_op(g->code, GLSL_VOP1_SQRT_F32, d.base, d2.base);
        return d;
    }

    /* `normalize(v) = v * inversesqrt(dot(v, v))`, which is the hardware's `v_rsq_f32` and a
     * multiply rather than a square root and a divide. The reciprocal square root is accurate to
     * 1 ULP, so a normalised vector's length is within a couple of ULP of one - not exactly one,
     * which is true of every implementation and is why nothing should compare it to one. */
    if (nm_is(nm, len, "normalize")) {
        if (argc != 1) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t s = gen_inv_length(g, arg[0], node);
        if (is_bad(s)) return s;
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c), s.base);
        }
        return d;
    }

    /* `cross(a, b)`, three components and six multiplies. Written out rather than looped,
     * because the index pattern is the thing to get right and a loop hides it. */
    if (nm_is(nm, len, "cross")) {
        if (argc != 2 || arg[0].count != 3 || arg[1].count != 3) {
            return gen_fail(g, "cross takes two vec3", node);
        }
        glsl_value_t d = gen_alloc(g, 3, node);
        if (is_bad(d)) return d;
        glsl_value_t t = gen_alloc(g, 3, node);
        if (is_bad(t)) return t;
        static const int L[3] = {1, 2, 0};
        static const int R[3] = {2, 0, 1};
        for (int c = 0; c < 3; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, arg[0].base + (uint32_t)L[c],
                              arg[1].base + (uint32_t)R[c]);
            glsl_emit_mul_f32(g->code, t.base + (uint32_t)c, arg[0].base + (uint32_t)R[c],
                              arg[1].base + (uint32_t)L[c]);
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)c, d.base + (uint32_t)c,
                              t.base + (uint32_t)c);
        }
        return d;
    }

    /* `reflect(I, N) = I - 2 * dot(N, I) * N`, GLSL 1.10 section 8.4 verbatim - and `N` is
     * assumed normalised there, which is the caller's business and not this one's. */
    if (nm_is(nm, len, "reflect")) {
        if (argc != 2) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t dp = gen_dot(g, arg[1], arg[0], node);
        if (is_bad(dp)) return dp;
        glsl_value_t two = gen_const(g, 2.0, node);
        if (is_bad(two)) return two;
        glsl_emit_mul_f32(g->code, dp.base, dp.base, two.base);
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_mul_f32(g->code, d.base + (uint32_t)c, comp_of(arg[1], c), dp.base);
            glsl_emit_sub_f32(g->code, d.base + (uint32_t)c, comp_of(arg[0], c),
                              d.base + (uint32_t)c);
        }
        return d;
    }

    /* `faceforward(N, I, Nref)` is `N` when `dot(Nref, I)` is negative and `-N` otherwise. One
     * comparison for the whole vector, then a select a component. */
    if (nm_is(nm, len, "faceforward")) {
        if (argc != 3) return gen_fail(g, "wrong number of arguments", node);
        glsl_value_t dp = gen_dot(g, arg[2], arg[1], node);
        if (is_bad(dp)) return dp;
        glsl_value_t zero = gen_const(g, 0.0, node);
        if (is_bad(zero)) return zero;
        glsl_value_t neg = gen_alloc(g, w, node);
        if (is_bad(neg)) return neg;
        for (int c = 0; c < w; c++) {
            glsl_emit_neg_f32(g->code, neg.base + (uint32_t)c, comp_of(arg[0], c));
        }
        glsl_value_t d = gen_alloc(g, w, node);
        if (is_bad(d)) return d;
        for (int c = 0; c < w; c++) {
            glsl_emit_cmp(g->code, GLSL_VOPC_LT_F32, dp.base, zero.base);
            glsl_emit_cndmask(g->code, d.base + (uint32_t)c, neg.base + (uint32_t)c,
                              comp_of(arg[0], c));
        }
        return d;
    }

    return gen_fail(g, "only constructors and the built-ins with verified instructions are "
                       "generated; this name is neither, and no function of that name is "
                       "defined in this shader", node);
}

/* -------------------------------------------------------------------------
 * User-defined functions, which are inlined
 *
 * There is no call instruction here and there does not need to be. GLSL forbids recursion, so
 * every call graph is finite and every call can be generated where it appears - which is also
 * what the register allocator wants, since a bump allocator has no notion of a frame to save
 * and restore across a branch.
 *
 * What is supported is the shape almost every helper in a fragment shader has: value
 * parameters, and a single `return` as the last statement of the body. An **early** return is
 * refused rather than generated, because on this machine leaving a function early is an exec
 * mask operation - the lanes that returned have to stop executing the rest of the body while
 * the others carry on - and that mask would have to be threaded through every statement after
 * it. `discard` does the same thing and can afford to, because it never comes back.
 * ------------------------------------------------------------------------- */

/* The function of this name with a body, or nothing. The AST is a flat array, so this reads it
 * directly rather than walking the unit's declaration chain - which the generator is not given
 * and does not otherwise need. A prototype has no body and is skipped: it is not the
 * definition, and inlining it would produce nothing. */
static int32_t gen_find_function(const glsl_gen_t *g, const char *name, size_t length) {
    for (int32_t i = 0; i < g->ast->count; i++) {
        const glsl_node_t *n = &g->ast->nodes[i];
        if (n->kind != GLSL_NODE_FUNCTION || n->c == GLSL_NO_NODE) continue;
        if (n->length != length) continue;
        GLboolean same = GL_TRUE;
        for (size_t c = 0; c < length; c++) {
            if (n->text[c] != name[c]) { same = GL_FALSE; break; }
        }
        if (same) return i;
    }
    return GLSL_NO_NODE;
}

/* The last statement of a body, so the trailing `return` can be found without generating it
 * twice - and so a body whose last statement is not one is refused before anything is emitted. */
static int32_t gen_last_stmt(const glsl_gen_t *g, int32_t compound) {
    if (compound == GLSL_NO_NODE) return GLSL_NO_NODE;
    int32_t last = GLSL_NO_NODE;
    for (int32_t s = g->ast->nodes[compound].a; s != GLSL_NO_NODE;
         s = g->ast->nodes[s].sibling) {
        last = s;
    }
    return last;
}

static glsl_value_t gen_call_user(glsl_gen_t *g, int32_t fn_node, int32_t first_arg,
                                  int32_t node) {
    const glsl_node_t *fn = &g->ast->nodes[fn_node];

    if (g->inline_depth >= GLSL_GEN_MAX_INLINE_DEPTH) {
        return gen_fail(g, "user-defined calls nest deeper than this generator inlines; GLSL "
                           "forbids recursion, so a shader that reaches this is calling itself",
                        node);
    }

    const glsl_type_t ret = glsl_type_from_token(fn->type_tok);
    const GLboolean is_void = (GLboolean)(ret == GLSL_TYPE_VOID);
    if (!is_void && !is_generated(ret)) {
        return gen_fail(g, "a function is generated only when it returns void, a float, a "
                           "vector, a matrix or a bool", node);
    }

    /* **A value-returning body has to end in its return**, checked before a single instruction
     * is emitted so a refusal leaves nothing half-generated behind it. A `void` body may end in
     * anything, including a bare `return;` - which is generated as the nothing it is, since it
     * is the last statement and there is nothing after it to skip. */
    int32_t last = gen_last_stmt(g, fn->c);
    if (is_void) {
        if (last != GLSL_NO_NODE && g->ast->nodes[last].kind == GLSL_NODE_RETURN &&
            g->ast->nodes[last].a != GLSL_NO_NODE) {
            return gen_fail(g, "a void function returns a value", node);
        }
        /* A trailing bare `return;` is skipped rather than generated; anything else is body. */
        if (last != GLSL_NO_NODE && g->ast->nodes[last].kind != GLSL_NODE_RETURN) {
            last = GLSL_NO_NODE; /* nothing to hold back - every statement is generated */
        }
    } else if (last == GLSL_NO_NODE || g->ast->nodes[last].kind != GLSL_NODE_RETURN ||
               g->ast->nodes[last].a == GLSL_NO_NODE) {
        return gen_fail(g, "a function is generated only when its body ends in `return <expr>;` "
                           "- an early or missing return would need the exec mask carried "
                           "through the statements after it", node);
    }

    /* **The arguments are evaluated in the caller's scope**, before the parameters shadow
     * anything: `f(x)` where the parameter is also called `x` has to read the caller's. */
    glsl_value_t argv[GEN_MAX_ARGS];
    int argc = 0;
    for (int32_t a = first_arg; a != GLSL_NO_NODE; a = g->ast->nodes[a].sibling) {
        if (argc >= GEN_MAX_ARGS) {
            return gen_fail(g, "more arguments than this generator carries", node);
        }
        argv[argc] = gen_expr(g, a);
        if (is_bad(argv[argc])) return argv[argc];
        argc++;
    }

    glsl_value_t out; out.base = 0u; out.count = 0;
    if (!is_void) {
        out = gen_alloc(g, glsl_type_components(ret), node);
        if (is_bad(out)) return out;
    }

    /* **Everything the call allocates from here is given back at the end**, which for an
     * inlined call is the whole of its parameters and its body's temporaries. Without this each
     * call site keeps its registers for the life of the shader, so ten calls to one helper cost
     * ten copies of its locals - and a shader is refused for a budget it never actually needed
     * at any one moment. The result is allocated *above* this mark so it survives, because it
     * is the one thing the caller goes on to read. */
    const uint32_t call_mark = gen_mark(g);

    const int vars_before = g->var_count;
    glsl_scope_push(g->sema);
    g->inline_depth++;

    /* **Every parameter is a copy, which is what GLSL says and not an implementation detail.**
     * An `out` or `inout` is passed by value and copied back at the return - never by reference
     * - so `swap(p, p)` leaves `p` alone rather than aliasing, and a body assigning to an `in`
     * parameter changes nothing the caller can see.
     *
     * The copy-back needs the argument to be somewhere to write, which is the same question an
     * assignment asks, so `gen_place_of` answers it. Resolved here and applied after the body:
     * a place taken before the parameters shadow anything is the caller's. */
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
            (void)gen_fail(g, "this call passes fewer arguments than the function takes", node);
            break;
        }
        const GLboolean writes_back = (GLboolean)(pn->qualifier == GLSL_TOK_KW_OUT ||
                                                  pn->qualifier == GLSL_TOK_KW_INOUT);
        const glsl_type_t pt = glsl_type_from_token(pn->type_tok);
        if (!is_generated(pt)) {
            (void)gen_fail(g, "only float, vec, mat and bool parameters are generated", node);
            break;
        }
        if (glsl_type_components(pt) != argv[bound].count) {
            (void)gen_fail(g, "an argument is a different width from the parameter it binds",
                           node);
            break;
        }
        const glsl_value_t home = gen_alloc(g, argv[bound].count, node);
        if (is_bad(home)) break;
        /* An `out` parameter starts undefined by the language's own rule, but copying the
         * argument in costs one move and makes a body that reads before writing behave the way
         * the reference does rather than reading whatever the allocator last held. */
        for (int c = 0; c < home.count; c++) {
            glsl_emit_mov(g->code, home.base + (uint32_t)c, argv[bound].base + (uint32_t)c);
        }
        if (writes_back) {
            /* **The argument has to be a place**, which is the same question an assignment
             * asks. A literal or an expression is not one, and GLSL refuses it - this says so
             * from the side that would otherwise write the value into a temporary and drop it. */
            if (!gen_place_of(g, argn[bound], &writeback[writebacks])) {
                (void)gen_fail(g, "an `out` or `inout` argument has to be something that can be "
                                  "assigned to", node);
                break;
            }
            if (writeback[writebacks].count != home.count) {
                (void)gen_fail(g, "an `out` argument is a different width from its parameter",
                               node);
                break;
            }
            writeback_from[writebacks] = home;
            writebacks++;
        }
        if (!gen_declare(g, pn->text, pn->length, pt, home, node)) break;
        if (!glsl_declare(g->sema, pn->text, pn->length, pt, GL_FALSE)) {
            g->sema->error = (const char *)0;
        }
        bound++;
    }
    if (!g->error && bound != argc) {
        (void)gen_fail(g, "this call passes more arguments than the function takes", node);
    }

    /* The body, every statement but the trailing return - which is generated here instead, into
     * the caller's result register, because that is the whole of what returning means. */
    if (!g->error) {
        for (int32_t s = g->ast->nodes[fn->c].a; s != GLSL_NO_NODE && !g->error;
             s = g->ast->nodes[s].sibling) {
            if (s == last) break;
            (void)glsl_gen_stmt(g, s);
        }
    }
    if (!g->error && !is_void) {
        const uint32_t mark = gen_mark(g);
        const glsl_value_t rv = gen_expr(g, g->ast->nodes[last].a);
        if (!is_bad(rv)) {
            if (rv.count != out.count) {
                (void)gen_fail(g, "the returned expression is a different width from the "
                                  "function's return type", node);
            } else {
                for (int c = 0; c < out.count; c++) {
                    glsl_emit_mov(g->code, out.base + (uint32_t)c, rv.base + (uint32_t)c);
                }
            }
        }
        gen_release(g, mark);
    }

    /* **The copy-back, after the body and before the parameters go out of scope.**
     *
     * This is what makes `out` and `inout` pass-by-value-and-copy-back rather than
     * pass-by-reference, which is the language's rule and not a detail: with references,
     * `swap(p, p)` aliases and leaves both unchanged; with copies it writes `p` twice and the
     * second write wins. The places were resolved in the caller's scope before the parameters
     * shadowed anything, so they name the caller's registers. */
    for (int i = 0; !g->error && i < writebacks; i++) {
        for (int c = 0; c < writeback[i].count; c++) {
            glsl_emit_mov(g->code, writeback[i].reg[c], writeback_from[i].base + (uint32_t)c);
        }
    }

    g->inline_depth--;
    glsl_scope_pop(g->sema);
    g->var_count = vars_before;
    /* After the writeback, which read the parameters' registers, and after the return move,
     * which wrote the caller's. Nothing below is live. */
    gen_release(g, call_mark);
    if (g->error) { glsl_value_t none; none.base = 0u; none.count = 0; return none; }
    return out;
}

static glsl_value_t gen_expr(glsl_gen_t *g, int32_t node) {
    if (g->error) {
        glsl_value_t none; none.base = 0u; none.count = 0; return none;
    }
    if (node == GLSL_NO_NODE) return gen_fail(g, "an expression is missing", node);

    const glsl_node_t *n = &g->ast->nodes[node];
    switch (n->kind) {
        case GLSL_NODE_FLOATCONST: {
            glsl_value_t out = gen_alloc(g, 1, node);
            if (is_bad(out)) return out;
            glsl_emit_mov_imm(g->code, out.base, float_bits(n->value));
            return out;
        }
        case GLSL_NODE_INTCONST: {
            /* An integer literal in a float context is the one integer this stage will take,
             * because converting it is exact and happens here rather than on the hardware. An
             * integer *variable* is still refused - there is nothing to convert at compile time
             * and no verified integer instruction to use at run time. */
            const glsl_type_t t = glsl_type_of(g->sema, node);
            if (t != GLSL_TYPE_INT && t != GLSL_TYPE_FLOAT) {
                return gen_fail(g, "unexpected type for an integer literal", node);
            }
            glsl_value_t out = gen_alloc(g, 1, node);
            if (is_bad(out)) return out;
            glsl_emit_mov_imm(g->code, out.base, float_bits(n->value));
            return out;
        }
        case GLSL_NODE_IDENTIFIER: {
            glsl_gen_var_t *v = gen_find(g, n->text, n->length);
            if (!v) return gen_fail(g, "this name has no register: only locals declared in this "
                                       "body are generated so far", node);
            return v->value;
        }
        case GLSL_NODE_FIELD:
            return gen_field(g, node);
        case GLSL_NODE_BINARY:
            return gen_binary(g, node);
        case GLSL_NODE_BOOLCONST: {
            glsl_value_t out = gen_alloc(g, 1, node);
            if (is_bad(out)) return out;
            glsl_emit_mov_imm(g->code, out.base, float_bits(n->value != 0.0 ? 1.0 : 0.0));
            return out;
        }
        case GLSL_NODE_CONDITIONAL: {
            /* `c ? a : b`, both arms evaluated and one selected - which is what the hardware
             * does anyway with a mask, and is why the operands must not assign. */
            if (has_side_effect(g->ast, n->b) || has_side_effect(g->ast, n->c)) {
                return gen_fail(g, "an arm of this ?: assigns, and both arms are evaluated - so "
                                   "it would run when the language says it does not", node);
            }
            glsl_value_t c = gen_expr(g, n->a);
            if (is_bad(c)) return c;
            if (c.count != 1) return gen_fail(g, "a ?: takes a single condition", node);
            glsl_value_t t = gen_expr(g, n->b);
            if (is_bad(t)) return t;
            glsl_value_t f = gen_expr(g, n->c);
            if (is_bad(f)) return f;
            if (t.count != f.count) {
                return gen_fail(g, "the two arms of this ?: are different widths", node);
            }
            glsl_value_t zero = gen_const(g, 0.0, node);
            if (is_bad(zero)) return zero;
            glsl_value_t out = gen_alloc(g, t.count, node);
            if (is_bad(out)) return out;
            for (int i = 0; i < t.count; i++) {
                glsl_emit_cmp(g->code, GLSL_VOPC_NEQ_F32, c.base, zero.base);
                /* **The false arm is `src0`.** The other way round compiles every `?:` in every
                 * shader to the opposite branch, and nothing anywhere complains. */
                glsl_emit_cndmask(g->code, out.base + (uint32_t)i, f.base + (uint32_t)i,
                                  t.base + (uint32_t)i);
            }
            return out;
        }
        case GLSL_NODE_UNARY: {
            if (n->op == GLSL_TOK_PLUS) return gen_expr(g, n->a);
            if (n->op == GLSL_TOK_BANG) {
                /* `!b` over a value that is 0.0 or 1.0 is `1 - b`. */
                glsl_value_t s = gen_expr(g, n->a);
                if (is_bad(s)) return s;
                if (s.count != 1) return gen_fail(g, "! takes a single bool", node);
                glsl_value_t one = gen_const(g, 1.0, node);
                if (is_bad(one)) return one;
                glsl_value_t out = gen_alloc(g, 1, node);
                if (is_bad(out)) return out;
                glsl_emit_sub_f32(g->code, out.base, one.base, s.base);
                return out;
            }
            if (n->op != GLSL_TOK_MINUS) {
                return gen_fail(g, "this unary operator has no verified instruction yet", node);
            }
            const glsl_type_t t = glsl_type_of(g->sema, n->a);
            /* Negating a whole float leaves it whole, so an `int` needs no truncation after it
             * and goes through the same instruction. A `bool` has no negation in GLSL. */
            if (!is_float_family(t) && !is_int_family(t)) {
                return gen_fail(g, "unary minus is generated for the float and int families "
                                   "only", node);
            }
            glsl_value_t s = gen_expr(g, n->a);
            if (is_bad(s)) return s;
            glsl_value_t out = gen_alloc(g, s.count, node);
            if (is_bad(out)) return out;
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
            if (target != GLSL_TYPE_ERROR) return gen_construct(g, target, n->b, node);
            /* **A function this shader defines wins over the built-in table**, which is GLSL's
             * own rule: a user function may share a name with a built-in and hides it. */
            {
                const int32_t fn = gen_find_function(g, callee->text, callee->length);
                if (fn != GLSL_NO_NODE) return gen_call_user(g, fn, n->b, node);
            }
            /* Not a type name and not defined here, so a built-in or a refusal. */
            return gen_builtin(g, callee, n->b, node);
        }
        case GLSL_NODE_ASSIGN: {
            gen_place_t place;
            if (!gen_place_of(g, n->a, &place)) {
                glsl_value_t none; none.base = 0u; none.count = 0; return none;
            }
            glsl_value_t r = gen_expr(g, n->b);
            if (is_bad(r)) return r;
            /* GLSL takes a scalar on the right of any of these - `c.rgb *= 0.5` - and nothing
             * else of a different width. */
            if (r.count != place.count && r.count != 1) {
                return gen_fail(g, "the two sides of this assignment are different widths", node);
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
                    /* The destination is its own first operand, which is what makes these one
                     * instruction a component rather than a read, an operate and a write. */
                    const glsl_token_type_t op = n->op == GLSL_TOK_ADD_ASSIGN ? GLSL_TOK_PLUS
                                                 : n->op == GLSL_TOK_SUB_ASSIGN ? GLSL_TOK_MINUS
                                                                                : GLSL_TOK_STAR;
                    for (int i = 0; i < place.count; i++) {
                        gen_binop_component(g, op, place.reg[i], place.reg[i], comp_of(r, i));
                    }
                    break;
                }
                case GLSL_TOK_DIV_ASSIGN: {
                    /* One reciprocal a divisor component, as `/` does - and into a temporary,
                     * because the divisor may be one of the destination's own registers. */
                    glsl_value_t inv = gen_alloc(g, r.count, node);
                    if (is_bad(inv)) return inv;
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
                    return gen_fail(g, "this assignment operator has no instruction selection "
                                       "yet", node);
            }

            /* The value of an assignment is what was assigned. A place whose registers run
             * consecutively - which every whole variable and every leading swizzle does - is
             * already a value; anything else (`v.zyx = ...` used for its result) is copied into
             * one, rather than a `glsl_value_t` being made to mean something it does not. */
            GLboolean consecutive = GL_TRUE;
            for (int i = 1; i < place.count; i++) {
                if (place.reg[i] != place.reg[i - 1] + 1u) { consecutive = GL_FALSE; break; }
            }
            if (consecutive) {
                glsl_value_t out;
                out.base = place.reg[0];
                out.count = place.count;
                return out;
            }
            glsl_value_t out = gen_alloc(g, place.count, node);
            if (is_bad(out)) return out;
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
 * `for`, unrolled
 *
 * **There is no branch in anything this generator emits, and a loop does not change that.** A
 * `for` whose trip count is known when the shader is compiled is written out iteration by
 * iteration, with the induction variable a constant in each - so the body meets the same
 * instruction selection as any other straight-line code and nothing new has to be true about
 * the machine.
 *
 * The alternative is a real backward branch, and the reason not to reach for it yet is the
 * failure mode: a loop whose condition never goes false on some lane does not draw the wrong
 * colour, it hangs the GPU, and that is the one failure this repository cannot afford to guess
 * at. An unrolled loop cannot hang. What it can do is not fit, and that is a message.
 *
 * So the loops that are generated are the ones real shaders mostly have - a constant count of
 * taps, of samples, of octaves - and the ones that are refused are refused by name with their
 * trip count in the message.
 * ------------------------------------------------------------------------- */

/* A compile-time constant, or false. Only what a loop header needs: a literal, or a negated
 * one. Anything else is a loop whose count this cannot know. */
static GLboolean const_of(const glsl_gen_t *g, int32_t node, double *out) {
    if (node == GLSL_NO_NODE) return GL_FALSE;
    const glsl_node_t *n = &g->ast->nodes[node];
    if (n->kind == GLSL_NODE_INTCONST || n->kind == GLSL_NODE_FLOATCONST) {
        *out = n->value;
        return GL_TRUE;
    }
    if (n->kind == GLSL_NODE_UNARY && n->op == GLSL_TOK_MINUS) {
        double inner = 0.0;
        if (!const_of(g, n->a, &inner)) return GL_FALSE;
        *out = -inner;
        return GL_TRUE;
    }
    return GL_FALSE;
}

/* Whether `node` is the identifier `name`. */
static GLboolean is_name(const glsl_gen_t *g, int32_t node, const char *name, size_t len) {
    if (node == GLSL_NO_NODE) return GL_FALSE;
    const glsl_node_t *n = &g->ast->nodes[node];
    if (n->kind != GLSL_NODE_IDENTIFIER || n->length != len) return GL_FALSE;
    for (size_t i = 0; i < len; i++) {
        if (n->text[i] != name[i]) return GL_FALSE;
    }
    return GL_TRUE;
}

#define GLSL_GEN_MAX_UNROLL 64

/* Whether this statement, or anything inside it, is a `break` or a `continue` **belonging to
 * it**.
 *
 * A nested loop stops the search: its `break` is its own, and refusing the outer loop for it
 * would be refusing the wrong thing. Only `{ ... }` walks a sibling chain, because that is the
 * one place a sibling is the next statement rather than the next element of something else -
 * following siblings everywhere would wander out of this loop and into the statements after
 * it, which is how a first attempt at this refused every loop in a shader that had one
 * `break` anywhere. */
static GLboolean has_loop_flow(const glsl_gen_t *g, int32_t node) {
    if (node == GLSL_NO_NODE) return GL_FALSE;
    const glsl_node_t *n = &g->ast->nodes[node];
    if (n->kind == GLSL_NODE_BREAK || n->kind == GLSL_NODE_CONTINUE) return GL_TRUE;
    if (n->kind == GLSL_NODE_FOR || n->kind == GLSL_NODE_WHILE) return GL_FALSE;
    if (n->kind == GLSL_NODE_COMPOUND) {
        for (int32_t s = n->a; s != GLSL_NO_NODE; s = g->ast->nodes[s].sibling) {
            if (has_loop_flow(g, s)) return GL_TRUE;
        }
        return GL_FALSE;
    }
    return (GLboolean)(has_loop_flow(g, n->a) || has_loop_flow(g, n->b) ||
                       has_loop_flow(g, n->c) || has_loop_flow(g, n->d));
}

static GLboolean gen_for(glsl_gen_t *g, int32_t node) {
    const glsl_node_t *n = &g->ast->nodes[node];

    /* The initialiser has to declare the induction variable with a constant. `for (i = 0; ...)`
     * over a variable declared outside is a different shape and is not read here. */
    if (n->a == GLSL_NO_NODE || g->ast->nodes[n->a].kind != GLSL_NODE_DECL) {
        (void)gen_fail(g, "a loop is unrolled only when its initialiser declares the counter - "
                          "`for (int i = 0; ...)` - because that is what makes the trip count "
                          "knowable here", node);
        return GL_FALSE;
    }
    const glsl_node_t *decl = &g->ast->nodes[n->a];
    const glsl_type_t ind_t = glsl_type_from_token(decl->type_tok);
    double start = 0.0;
    if (!const_of(g, decl->a, &start)) {
        (void)gen_fail(g, "a loop's counter has to start at a constant for the trip count to "
                          "be known when the shader is compiled", node);
        return GL_FALSE;
    }

    /* The condition: the counter against a constant. */
    if (n->b == GLSL_NO_NODE || g->ast->nodes[n->b].kind != GLSL_NODE_BINARY) {
        (void)gen_fail(g, "a loop's condition has to compare the counter with a constant", node);
        return GL_FALSE;
    }
    const glsl_node_t *cond = &g->ast->nodes[n->b];
    double limit = 0.0;
    if (!is_name(g, cond->a, decl->text, decl->length) || !const_of(g, cond->b, &limit)) {
        (void)gen_fail(g, "a loop's condition has to be the counter compared with a constant, "
                          "in that order", node);
        return GL_FALSE;
    }

    /* The step: `i++`, `i--`, or `i += k`. */
    double step = 0.0;
    if (n->c == GLSL_NO_NODE) {
        (void)gen_fail(g, "a loop with no increment has no trip count this can know", node);
        return GL_FALSE;
    }
    {
        const glsl_node_t *inc = &g->ast->nodes[n->c];
        if ((inc->kind == GLSL_NODE_POSTFIX || inc->kind == GLSL_NODE_UNARY) &&
            is_name(g, inc->a, decl->text, decl->length)) {
            step = (inc->op == GLSL_TOK_INC) ? 1.0 : (inc->op == GLSL_TOK_DEC ? -1.0 : 0.0);
        } else if (inc->kind == GLSL_NODE_ASSIGN &&
                   is_name(g, inc->a, decl->text, decl->length)) {
            double k = 0.0;
            if (const_of(g, inc->b, &k)) {
                if (inc->op == GLSL_TOK_ADD_ASSIGN) step = k;
                else if (inc->op == GLSL_TOK_SUB_ASSIGN) step = -k;
            }
        }
        if (step == 0.0) {
            (void)gen_fail(g, "a loop's increment has to move the counter by a constant - `i++`,"
                              " `i--` or `i += k` - and by something other than nothing", node);
            return GL_FALSE;
        }
    }

    /* **The trip count, counted the way the reference runs it**: test, then body, then step. */
    int trips = 0;
    for (double v = start; trips <= GLSL_GEN_MAX_UNROLL; v += step) {
        GLboolean go;
        switch (cond->op) {
            case GLSL_TOK_LT: go = (GLboolean)(v < limit); break;
            case GLSL_TOK_LE: go = (GLboolean)(v <= limit); break;
            case GLSL_TOK_GT: go = (GLboolean)(v > limit); break;
            case GLSL_TOK_GE: go = (GLboolean)(v >= limit); break;
            case GLSL_TOK_NE: go = (GLboolean)(v != limit); break;
            default:
                (void)gen_fail(g, "a loop's condition is compared with <, <=, >, >= or != here",
                               node);
                return GL_FALSE;
        }
        if (!go) break;
        trips++;
    }
    if (trips > GLSL_GEN_MAX_UNROLL) {
        (void)gen_fail(g, "this loop runs more times than the generator unrolls, and a loop is "
                          "unrolled rather than branched because a branch that never goes false "
                          "hangs the part rather than drawing the wrong colour", node);
        return GL_FALSE;
    }

    /* `break` and `continue` would each need a mask carried through the rest of the loop, which
     * is the same thing an early `return` needs and is refused for the same reason. Checked
     * before anything is emitted. */
    if (has_loop_flow(g, n->d)) {
        (void)gen_fail(g, "`break` and `continue` are not generated: each needs the exec mask "
                          "carried through the rest of the loop, which this generator does not "
                          "do", node);
        return GL_FALSE;
    }

    /* Out it goes, one copy per trip, with the counter a fresh constant each time. The scope is
     * the loop's - `for (int i = ...)` ends with it - and each body gets its own on top. */
    const int vars_before = g->var_count;
    glsl_scope_push(g->sema);
    if (!glsl_declare(g->sema, decl->text, decl->length, ind_t, GL_FALSE)) {
        g->sema->error = (const char *)0;
    }
    double v = start;
    for (int t = 0; t < trips && !g->error; t++, v += step) {
        const uint32_t mark = gen_mark(g);
        glsl_value_t iv = gen_alloc(g, 1, node);
        if (is_bad(iv)) break;
        glsl_emit_mov_imm(g->code, iv.base, float_bits((float)v));
        const int vars_here = g->var_count;
        if (!gen_declare(g, decl->text, decl->length, ind_t, iv, node)) break;
        (void)glsl_gen_stmt(g, n->d);
        g->var_count = vars_here;
        gen_release(g, mark);
    }
    glsl_scope_pop(g->sema);
    g->var_count = vars_before;
    return (GLboolean)(g->error == (const char *)0);
}

GLboolean glsl_gen_stmt(glsl_gen_t *g, int32_t node) {
    if (g->error) return GL_FALSE;
    if (node == GLSL_NO_NODE) return GL_TRUE;

    const glsl_node_t *n = &g->ast->nodes[node];
    switch (n->kind) {
        case GLSL_NODE_COMPOUND: {
            /* The variables a block declares go out of scope with it, and so do their
             * registers - but only the ones declared *here*, which is what the saved count is
             * for. The register mark is not rolled back with them: a nested block's variables
             * sit above this block's, and releasing them would be correct only if nothing
             * outside had been allocated since, which is not something to rely on.
             *
             * The semantic stage's scope is pushed alongside, because this stage asks it for the
             * type of every operand and it can only answer for names it currently holds. */
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
            /* Every declarator in `float a, b = 1.0;` is its own node on the sibling chain, so
             * this arm sees one name at a time and the chain is walked by the caller. */
            const glsl_type_t t = glsl_type_from_token(n->type_tok);
            if (!is_generated(t)) {
                (void)gen_fail(g, "only float, vec, mat and bool locals are generated", node);
                return GL_FALSE;
            }
            if (n->array_size != GLSL_NO_NODE) {
                (void)gen_fail(g, "arrays have no instruction selection yet", node);
                return GL_FALSE;
            }
            glsl_value_t home = gen_alloc(g, glsl_type_components(t), node);
            if (is_bad(home)) return GL_FALSE;
            if (n->a != GLSL_NO_NODE) {
                const uint32_t mark = gen_mark(g);
                glsl_value_t init = gen_expr(g, n->a);
                if (is_bad(init)) return GL_FALSE;
                if (init.count != home.count) {
                    (void)gen_fail(g, "the initialiser is a different width from the variable",
                                   node);
                    return GL_FALSE;
                }
                gen_move(g, home, init);
                gen_release(g, mark);
            }
            if (!gen_declare(g, n->text, n->length, t, home, node)) return GL_FALSE;
            /* And into the semantic stage, which is what answers `glsl_type_of` for every later
             * mention of this name. Declared *after* the initialiser is generated, so
             * `float a = a;` cannot see itself. */
            if (!glsl_declare(g->sema, n->text, n->length, t, GL_FALSE)) {
                (void)gen_fail(g, "this name is already declared in this scope", node);
                return GL_FALSE;
            }
            return GL_TRUE;
        }
        case GLSL_NODE_EXPR_STMT: {
            if (n->a == GLSL_NO_NODE) return GL_TRUE; /* the empty statement */
            const uint32_t mark = gen_mark(g);
            glsl_value_t v = gen_expr(g, n->a);
            /* **A void call produces no value, and that is not a failure.** `is_bad` is a
             * width of zero, which a call to a `void` function has by definition - so the error
             * flag is what says whether anything went wrong here, and a statement is the one
             * place a result of no width is the expected outcome. */
            (void)v;
            if (g->error) return GL_FALSE;
            gen_release(g, mark);
            return GL_TRUE;
        }
        /*
         * **`if` is the exec mask, and there is no branch in it.**
         *
         * Every lane runs every instruction; which lanes *write* is what `exec` says. So the
         * then arm runs with `exec` narrowed to the lanes the condition holds for, the else arm
         * with the complement, and then `exec` goes back to what came in. A body that no lane
         * is running still executes and writes nothing, which is why `s_cbranch_execz` is a
         * saving rather than a requirement - and leaving it out is what lets this emit straight
         * through with no labels and no offsets to backpatch.
         *
         * Scalar instructions inside a body are **not** exec-masked, and a nested `if` emits
         * some. They stay correct because what they compute is `exec & mask`, and `0 & mask` is
         * zero: an inner `if` inside a dead outer one narrows nothing and restores nothing.
         */
        case GLSL_NODE_IF: {
            if (g->exec_depth >= GLSL_GEN_MAX_EXEC_DEPTH) {
                (void)gen_fail(g, "the conditionals in this shader nest deeper than the scalar "
                                  "registers set aside for them", node);
                return GL_FALSE;
            }
            const uint32_t saved = GLSL_GEN_EXEC_SGPR_BASE + (uint32_t)g->exec_depth;
            const uint32_t mark = gen_mark(g);
            glsl_value_t cond = gen_expr(g, n->a);
            if (is_bad(cond)) return GL_FALSE;
            if (cond.count != 1) {
                (void)gen_fail(g, "an if takes a single condition", node);
                return GL_FALSE;
            }
            glsl_value_t zero = gen_const(g, 0.0, node);
            if (is_bad(zero)) return GL_FALSE;
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
        /*
         * **`discard` has to outlive the `if` it sits in.**
         *
         * Clearing `exec` kills the running lanes, and the enclosing `if`'s restore would hand
         * every one of them straight back - so the lanes come out of each enclosing saved mask
         * first, and only then is `exec` cleared. At the outermost level there is nothing to
         * take them out of and it is the one instruction.
         *
         * Everything after this in the same arm runs with `exec` zero and writes nothing, which
         * is what the specification asks for. The export at the end of the shader still runs,
         * and still carries `done`: a wave that discarded every lane must still retire.
         */
        case GLSL_NODE_DISCARD: {
            for (int d = 0; d < g->exec_depth; d++) {
                glsl_emit_exec_drop_live(g->code, GLSL_GEN_EXEC_SGPR_BASE + (uint32_t)d);
            }
            /* **And the live mask, when the shader samples.** In whole-quad mode the export is
             * restored from that mask rather than from `exec`, so a discard that did not reach
             * it would be undone at the end of the shader instead of at the end of the `if` -
             * the same bug one step further out. Taking helper lanes out of it along the way is
             * harmless: they were never in it.
             *
             * **The known limit is a sample that comes *after* a discard.** A discarded lane is
             * off from here on, so it no longer contributes to a neighbour's derivative, and a
             * `texture2D` further down the shader gets a level of detail computed from a
             * smaller quad. GLSL leaves derivatives undefined in non-uniform control flow, so
             * this is within the specification - but ACO keeps a separate mask so the quad
             * stays whole (`aco_insert_exec_mask.cpp`, the demote path), and a shader that
             * samples after discarding will differ from a desktop driver in the last mip level
             * it picks. Sampling and then discarding - which is what craft's block shader does,
             * and the common shape - is unaffected. */
            if (g->wqm) glsl_emit_exec_drop_live(g->code, GLSL_GEN_LIVE_SGPR);
            glsl_emit_exec_clear(g->code);
            return GL_TRUE;
        }
        case GLSL_NODE_FOR:
            return gen_for(g, node);

        case GLSL_NODE_RETURN:
            /* **A return reaching here is one that is not the last statement of a function
             * body.** `gen_call_user` generates the trailing one itself, into the caller's
             * result, and never walks as far as this arm. Anything else - a return inside an
             * `if`, or one in `main` - would have to stop the lanes that took it from executing
             * the rest of the body while the others carry on, and that mask would need
             * threading through every statement after it. */
            (void)gen_fail(g, "a return is generated only as the last statement of a function "
                              "body; an early return is an exec mask this generator does not "
                              "carry through the statements that follow it", node);
            return GL_FALSE;
        default:
            (void)gen_fail(g, "this statement has no instruction selection yet: the generator "
                              "handles declarations, expressions, blocks, if and discard - a "
                              "loop would need a branch and a label mechanism it has not got",
                           node);
            return GL_FALSE;
    }
}

glsl_value_t glsl_gen_expression(glsl_gen_t *g, int32_t node) {
    return gen_expr(g, node);
}

/* One register from the bump allocator, held for the whole shader and bound to no name.
 *
 * The prologue needs a constant or two of its own - `gl_FrontFacing` selects between 0.0 and
 * 1.0, and a select's second operand has to be a register - and those are not variables, so
 * declaring them would put a name in the table that no GLSL can refer to. Never released,
 * because the prologue runs once and a register it holds is one the body never sees. */
uint32_t glsl_gen_scratch(glsl_gen_t *g) {
    if (!g) return 0u;
    const glsl_value_t v = gen_alloc(g, 1, GLSL_NO_NODE);
    return is_bad(v) ? 0u : v.base;
}

void glsl_gen_reserve(glsl_gen_t *g, uint32_t first) {
    if (!g) return;
    if (first > g->next_vgpr) g->next_vgpr = first;
    if (g->next_vgpr > g->high_water) g->high_water = g->next_vgpr;
}

GLboolean glsl_gen_declare_sampler(glsl_gen_t *g, const char *name, size_t len, uint32_t set) {
    if (!g) return GL_FALSE;
    if (g->sampler_count >= GLSL_GEN_MAX_TEX_SETS) {
        (void)gen_fail(g, "more samplers than the draw path carries descriptor sets for",
                       GLSL_NO_NODE);
        return GL_FALSE;
    }
    g->samplers[g->sampler_count].name = name;
    g->samplers[g->sampler_count].name_len = len;
    g->samplers[g->sampler_count].set = set;
    g->sampler_count++;
    /* **And into the semantic stage, which is what types the lookup.** `texture2D(s, uv)` is
     * resolved by rule from its argument types, so a `s` the symbol table has never heard of
     * makes the whole call `GLSL_TYPE_ERROR` - and then `vec3(texture2D(...))` is refused for
     * having an argument that is not a float or a vector, which is a true sentence about a
     * false premise and sends the reader to the wrong place entirely. */
    if (!glsl_declare(g->sema, name, len, GLSL_TYPE_SAMPLER2D, GL_FALSE)) {
        g->sema->error = (const char *)0; /* already declared is the caller having done it */
    }
    /* **Sampling is what puts the shader in whole-quad mode**, and it is decided here rather
     * than when the first `texture2D` is generated - the prologue has to emit `s_wqm_b32`
     * before any of the body, and by then it is too late to find out. A shader that declares a
     * sampler and never uses it therefore runs in whole-quad mode for nothing, which costs the
     * two instructions and no correctness. */
    g->wqm = GL_TRUE;
    return GL_TRUE;
}

GLboolean glsl_gen_lookup(glsl_gen_t *g, const char *name, size_t len, glsl_value_t *out) {
    if (!g) return GL_FALSE;
    const glsl_gen_var_t *v = gen_find(g, name, len);
    if (!v) return GL_FALSE;
    if (out) *out = v->value;
    return GL_TRUE;
}

glsl_value_t glsl_gen_declare_input(glsl_gen_t *g, const char *name, size_t len,
                                    glsl_type_t type) {
    glsl_value_t none; none.base = 0u; none.count = 0;
    if (!g || g->error) return none;
    /* `int` and `bool` join the float family here and nowhere else: a uniform of either is a
     * float in the program's value pool, so it arrives in a register like any other and can be
     * compared, converted with `bool()`, or used as a condition. What it cannot do is
     * arithmetic - `glsl_type_of` still says `int`, and `gen_binary` still refuses it. */
    if (!is_generated(type) && type != GLSL_TYPE_INT) {
        return gen_fail(g, "only float, vec, mat, int and bool inputs are generated",
                        GLSL_NO_NODE);
    }
    glsl_value_t home = gen_alloc(g, glsl_type_components(type), GLSL_NO_NODE);
    if (is_bad(home)) return home;
    if (!gen_declare(g, name, len, type, home, GLSL_NO_NODE)) return none;
    /* Into the semantic stage too, so it can type expressions that mention this name. A caller
     * that has already declared it there passes through: the register home is what this adds. */
    if (!glsl_declare(g->sema, name, len, type, GL_FALSE)) {
        g->sema->error = (const char *)0; /* a redeclaration here is the caller having done it */
    }
    return home;
}
