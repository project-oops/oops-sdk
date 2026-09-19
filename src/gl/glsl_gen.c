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

/* The float family, and nothing else. `int` and `bool` are refused rather than run through the
 * float instructions: `v_add_f32` on a pair of integers is not integer addition, and the result
 * would be wrong in a way no test on the host would see. */
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

/* The bits of a float literal, without punning through a pointer. */
static uint32_t float_bits(double d) {
    union { float f; uint32_t u; } cvt;
    cvt.f = (float)d;
    return cvt.u;
}

/* -------------------------------------------------------------------------
 * Expressions
 * ------------------------------------------------------------------------- */

static glsl_value_t gen_expr(glsl_gen_t *g, int32_t node);

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
 */
static glsl_value_t gen_binary(glsl_gen_t *g, int32_t node) {
    const glsl_node_t *n = &g->ast->nodes[node];
    const glsl_token_type_t op = n->op;

    if (op != GLSL_TOK_PLUS && op != GLSL_TOK_MINUS && op != GLSL_TOK_STAR) {
        return gen_fail(g, "this operator has no verified instruction yet: only + - * are "
                           "generated, and / is refused rather than approximated", node);
    }

    const glsl_type_t lt = glsl_type_of(g->sema, n->a);
    const glsl_type_t rt = glsl_type_of(g->sema, n->b);
    if (!is_float_family(lt) || !is_float_family(rt)) {
        return gen_fail(g, "only float, vec and mat arithmetic is generated; int and bool have "
                           "no verified instruction here", node);
    }

    glsl_value_t a = gen_expr(g, n->a);
    if (is_bad(a)) return a;
    glsl_value_t b = gen_expr(g, n->b);
    if (is_bad(b)) return b;

    /* `mat4 * vec4`. The encoder owns the column-major walk. */
    if (op == GLSL_TOK_STAR && lt == GLSL_TYPE_MAT4 && rt == GLSL_TYPE_VEC4) {
        glsl_value_t out = gen_alloc(g, 4, node);
        if (is_bad(out)) return out;
        glsl_emit_mat4_mul_vec4(g->code, out.base, a.base, b.base);
        return out;
    }
    if (is_matrix(lt) || is_matrix(rt)) {
        return gen_fail(g, "the only matrix arithmetic generated so far is mat4 * vec4", node);
    }

    const int width = a.count > b.count ? a.count : b.count;
    if (a.count != b.count && a.count != 1 && b.count != 1) {
        return gen_fail(g, "these operand widths do not combine", node);
    }

    glsl_value_t out = gen_alloc(g, width, node);
    if (is_bad(out)) return out;
    for (int i = 0; i < width; i++) {
        const uint32_t ai = a.base + (a.count == 1 ? 0u : (uint32_t)i);
        const uint32_t bi = b.base + (b.count == 1 ? 0u : (uint32_t)i);
        gen_binop_component(g, op, out.base + (uint32_t)i, ai, bi);
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
    const int needed = glsl_type_components(target);
    glsl_value_t out = gen_alloc(g, needed, node);
    if (is_bad(out)) return out;

    /* Count the arguments and their components, so the one-scalar forms are told apart. */
    int args = 0, supplied = 0;
    for (int32_t a = first_arg; a != GLSL_NO_NODE; a = g->ast->nodes[a].sibling) {
        const glsl_type_t at = glsl_type_of(g->sema, a);
        if (!is_float_family(at)) {
            return gen_fail(g, "only float, vec and mat constructor arguments are generated",
                            node);
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
        case GLSL_NODE_UNARY: {
            if (n->op == GLSL_TOK_PLUS) return gen_expr(g, n->a);
            if (n->op != GLSL_TOK_MINUS) {
                return gen_fail(g, "this unary operator has no verified instruction yet", node);
            }
            const glsl_type_t t = glsl_type_of(g->sema, n->a);
            if (!is_float_family(t)) {
                return gen_fail(g, "unary minus is generated for the float family only", node);
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
            if (target == GLSL_TYPE_ERROR) {
                return gen_fail(g, "only constructors are generated; there is no call mechanism "
                                   "yet, so a function call is refused rather than inlined",
                                node);
            }
            return gen_construct(g, target, n->b, node);
        }
        case GLSL_NODE_ASSIGN: {
            if (n->op != GLSL_TOK_ASSIGN) {
                return gen_fail(g, "only plain assignment is generated; the compound forms "
                                   "would need a read of the destination first", node);
            }
            const glsl_node_t *lhs = &g->ast->nodes[n->a];
            if (lhs->kind != GLSL_NODE_IDENTIFIER) {
                return gen_fail(g, "only a whole variable is assignable here; writing through a "
                                   "swizzle needs a masked move this has not measured", node);
            }
            glsl_gen_var_t *v = gen_find(g, lhs->text, lhs->length);
            if (!v) return gen_fail(g, "assigning to a name with no register", node);
            glsl_value_t r = gen_expr(g, n->b);
            if (is_bad(r)) return r;
            if (r.count != v->value.count) {
                return gen_fail(g, "the two sides of this assignment are different widths", node);
            }
            gen_move(g, v->value, r);
            return v->value;
        }
        default:
            return gen_fail(g, "this expression has no instruction selection yet", node);
    }
}

/* -------------------------------------------------------------------------
 * Statements
 * ------------------------------------------------------------------------- */

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
            if (!is_float_family(t)) {
                (void)gen_fail(g, "only float, vec and mat locals are generated", node);
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
            if (is_bad(v)) return GL_FALSE;
            gen_release(g, mark);
            return GL_TRUE;
        }
        default:
            (void)gen_fail(g, "this statement has no instruction selection yet: the generator "
                              "handles declarations, expressions and blocks", node);
            return GL_FALSE;
    }
}

glsl_value_t glsl_gen_expression(glsl_gen_t *g, int32_t node) {
    return gen_expr(g, node);
}

glsl_value_t glsl_gen_declare_input(glsl_gen_t *g, const char *name, size_t len,
                                    glsl_type_t type) {
    glsl_value_t none; none.base = 0u; none.count = 0;
    if (!g || g->error) return none;
    if (!is_float_family(type)) {
        return gen_fail(g, "only float, vec and mat inputs are generated", GLSL_NO_NODE);
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
