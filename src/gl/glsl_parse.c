/*
 * oops-gl: the GLSL expression parser
 *
 * Recursive descent over GLSL 1.10's grammar, one function per precedence level: the
 * order of the functions is the precedence table. The binary levels loop, which builds
 * left-associatively (`a - b - c` is `(a - b) - c`); assignment and the conditional
 * recurse on the right, which builds right-associatively (`a = b = c` is
 * `a = (b = c)`). The bitwise levels `|`, `^` and `&` are present although 1.10 only
 * reserves them, since leaving a level out reassociates everything around it.
 *
 * The parser stops at the first error; it does not resynchronise.
 */

#include "glsl_internal.h"

static void fail(glsl_parser_t *p, const char *why) {
    if (p->error)
        return; /* the first error is the one that means something */
    p->error = why;
    p->error_line = p->tok.line;
    p->error_column = p->tok.column;
}

static void bump(glsl_parser_t *p) {
    /* The one place a token enters the parser. With `pp` set, directives are obeyed and
     * macros expanded before the grammar sees anything; with it null the raw lexer is
     * read, as this file's own tests do. Either source writes EOF at the end, so the
     * lookahead stays valid and every later `accept` fails. */
    if (p->pp) {
        glsl_pp_next(p->pp, &p->tok);
        if (p->tok.type == GLSL_TOK_ERROR) {
            fail(p, p->tok.error ? p->tok.error
                                 : (p->pp->error ? p->pp->error : "invalid token"));
        }
        return;
    }
    glsl_lex_next(&p->lx, &p->tok);
    if (p->tok.type == GLSL_TOK_ERROR) {
        fail(p, p->tok.error ? p->tok.error : "invalid token");
    }
}

static GLboolean check(const glsl_parser_t *p, glsl_token_type_t t) {
    return (GLboolean)(p->tok.type == t);
}

static GLboolean accept(glsl_parser_t *p, glsl_token_type_t t) {
    if (!check(p, t))
        return GL_FALSE;
    bump(p);
    return GL_TRUE;
}

static int32_t node_new(glsl_parser_t *p, glsl_node_kind_t kind) {
    if (!p->ast || p->ast->count >= GLSL_MAX_NODES) {
        fail(p, "expression too large for the node arena");
        return GLSL_NO_NODE;
    }
    int32_t at = p->ast->count++;
    glsl_node_t *n = &p->ast->nodes[at];
    n->kind = kind;
    n->op = GLSL_TOK_EOF;
    n->a = n->b = n->c = n->d = GLSL_NO_NODE;
    n->sibling = GLSL_NO_NODE;
    n->type_tok = GLSL_TOK_EOF;
    /* Every field is cleared here: the node arena is reused across parses and is not
     * zeroed. */
    n->type_name = (const char *)0;
    n->type_name_len = 0u;
    n->qualifier = GLSL_TOK_EOF;
    n->array_size = GLSL_NO_NODE;
    n->text = (const char *)0;
    n->length = 0u;
    n->value = 0.0;
    n->resolved = GLSL_NO_NODE;
    n->line = p->tok.line;
    n->column = p->tok.column;
    return at;
}

void glsl_parser_init(glsl_parser_t *p, glsl_ast_t *ast, const char *source,
                      size_t length) {
    if (!p)
        return;
    p->ast = ast;
    if (ast)
        ast->count = 0;
    p->error = (const char *)0;
    p->error_line = 0;
    p->error_column = 0;
    p->pp = (glsl_pp_t *)0;
    p->version = 0; /* not stated: enforce nothing - see glsl_parser_t */
    /* Cleared, because the caller's parser is not zeroed and `starts_declaration` walks
     * this list for every identifier. */
    p->struct_names = 0;
    glsl_lexer_init(&p->lx, source, length);
    bump(p);
}

void glsl_parser_init_pp(glsl_parser_t *p, glsl_ast_t *ast, glsl_pp_t *pp) {
    if (!p)
        return;
    p->ast = ast;
    if (ast)
        ast->count = 0;
    p->error = (const char *)0;
    p->error_line = 0;
    p->error_column = 0;
    p->pp = pp;
    /* The lexer is left inert: with `pp` set nothing reads it, and the preprocessor has
     * its own over the same source. */
    glsl_lexer_init(&p->lx, (const char *)0, 0);
    p->version = 0;
    p->struct_names = 0; /* as in glsl_parser_init, and for the same reason */
    /* This consumes every directive before the first real token, so `#version` has been
     * read when this returns and a caller can refuse the language before parsing it. */
    bump(p);
    /* A shader that says nothing is GLSL 1.10, as the specification states. */
    p->version = (pp && pp->version) ? pp->version : 110;
}

static int32_t parse_assignment(glsl_parser_t *p);

/* primary: identifier, literal, or a parenthesised expression. */
static int32_t parse_primary(glsl_parser_t *p) {
    if (p->error)
        return GLSL_NO_NODE;

    switch (p->tok.type) {
    case GLSL_TOK_IDENTIFIER: {
        int32_t at = node_new(p, GLSL_NODE_IDENTIFIER);
        if (at == GLSL_NO_NODE)
            return at;
        p->ast->nodes[at].text = p->tok.text;
        p->ast->nodes[at].length = p->tok.length;
        bump(p);
        return at;
    }
    /* A type name used as a constructor - `vec4(...)` - is an identifier here. Deciding
     * that `vec4` names a type rather than a function is the semantic stage's job, and
     * the grammar cannot tell them apart anyway. */
    case GLSL_TOK_KW_FLOAT:
    case GLSL_TOK_KW_INT:
    case GLSL_TOK_KW_BOOL:
    case GLSL_TOK_KW_VEC2:
    case GLSL_TOK_KW_VEC3:
    case GLSL_TOK_KW_VEC4:
    case GLSL_TOK_KW_IVEC2:
    case GLSL_TOK_KW_IVEC3:
    case GLSL_TOK_KW_IVEC4:
    case GLSL_TOK_KW_BVEC2:
    case GLSL_TOK_KW_BVEC3:
    case GLSL_TOK_KW_BVEC4:
    case GLSL_TOK_KW_MAT2:
    case GLSL_TOK_KW_MAT3:
    case GLSL_TOK_KW_MAT4:
    case GLSL_TOK_KW_MAT2X3:
    case GLSL_TOK_KW_MAT2X4:
    case GLSL_TOK_KW_MAT3X2:
    case GLSL_TOK_KW_MAT3X4:
    case GLSL_TOK_KW_MAT4X2:
    case GLSL_TOK_KW_MAT4X3: {
        int32_t at = node_new(p, GLSL_NODE_IDENTIFIER);
        if (at == GLSL_NO_NODE)
            return at;
        p->ast->nodes[at].text = p->tok.text;
        p->ast->nodes[at].length = p->tok.length;
        bump(p);
        return at;
    }
    case GLSL_TOK_INTCONST:
    case GLSL_TOK_FLOATCONST: {
        GLboolean is_int = check(p, GLSL_TOK_INTCONST);
        int32_t at = node_new(p, is_int ? GLSL_NODE_INTCONST : GLSL_NODE_FLOATCONST);
        if (at == GLSL_NO_NODE)
            return at;
        p->ast->nodes[at].value = p->tok.value;
        p->ast->nodes[at].text = p->tok.text;
        p->ast->nodes[at].length = p->tok.length;
        bump(p);
        return at;
    }
    case GLSL_TOK_KW_TRUE:
    case GLSL_TOK_KW_FALSE: {
        GLboolean is_true = check(p, GLSL_TOK_KW_TRUE);
        int32_t at = node_new(p, GLSL_NODE_BOOLCONST);
        if (at == GLSL_NO_NODE)
            return at;
        p->ast->nodes[at].value = is_true ? 1.0 : 0.0;
        bump(p);
        return at;
    }
    case GLSL_TOK_LPAREN: {
        bump(p);
        int32_t inner = glsl_parse_expression(p);
        if (!accept(p, GLSL_TOK_RPAREN)) {
            fail(p, "expected ')'");
            return GLSL_NO_NODE;
        }
        /* No node for the parentheses: they grouped the parse and mean nothing after.
         */
        return inner;
    }
    case GLSL_TOK_KW_RESERVED:
        fail(p, "reserved word used as an identifier");
        return GLSL_NO_NODE;
    default:
        fail(p, "expected an expression");
        return GLSL_NO_NODE;
    }
}

/* postfix: indexing, calls, field selection, and `++`/`--` after the operand.
 *
 * A loop rather than recursion, because these chain left to right: `a[i].x++` is
 * `((a[i]).x)++`. */
static int32_t parse_postfix(glsl_parser_t *p) {
    int32_t left = parse_primary(p);
    if (p->error)
        return GLSL_NO_NODE;

    for (;;) {
        if (check(p, GLSL_TOK_LBRACKET)) {
            bump(p);
            int32_t at = node_new(p, GLSL_NODE_INDEX);
            if (at == GLSL_NO_NODE)
                return at;
            /* `x[]` is never an expression, so it can only be the unsized array
             * constructor `float[](a, b)`, GLSL 1.30's form, which is named in the
             * refusal. */
            if (check(p, GLSL_TOK_RBRACKET)) {
                fail(p, "an array constructor needs a constant length here: "
                        "`float[2](a, b)`; "
                        "the unsized `float[](a, b)` is a later version's");
                return GLSL_NO_NODE;
            }
            int32_t idx = glsl_parse_expression(p);
            if (!accept(p, GLSL_TOK_RBRACKET)) {
                fail(p, "expected ']'");
                return GLSL_NO_NODE;
            }
            p->ast->nodes[at].a = left;
            p->ast->nodes[at].b = idx;
            left = at;
        } else if (check(p, GLSL_TOK_LPAREN)) {
            bump(p);
            int32_t at = node_new(p, GLSL_NODE_CALL);
            if (at == GLSL_NO_NODE)
                return at;
            p->ast->nodes[at].a = left;
            /* Arguments chain through `sibling`. An empty list is legal - `f()` - so
             * the closing paren is tested before the first argument is demanded. */
            if (!check(p, GLSL_TOK_RPAREN)) {
                int32_t first =
                    parse_assignment(p); /* not glsl_parse_expression: a comma here
                                          * separates arguments, it is not the
                                          * sequence operator */
                if (p->error)
                    return GLSL_NO_NODE;
                p->ast->nodes[at].b = first;
                int32_t prev = first;
                while (accept(p, GLSL_TOK_COMMA)) {
                    int32_t next = parse_assignment(p);
                    if (p->error)
                        return GLSL_NO_NODE;
                    p->ast->nodes[prev].sibling = next;
                    prev = next;
                }
            }
            if (!accept(p, GLSL_TOK_RPAREN)) {
                fail(p, "expected ')' to close an argument list");
                return GLSL_NO_NODE;
            }
            left = at;
        } else if (check(p, GLSL_TOK_DOT)) {
            bump(p);
            if (!check(p, GLSL_TOK_IDENTIFIER)) {
                fail(p, "expected a field name after '.'");
                return GLSL_NO_NODE;
            }
            int32_t at = node_new(p, GLSL_NODE_FIELD);
            if (at == GLSL_NO_NODE)
                return at;
            p->ast->nodes[at].a = left;
            p->ast->nodes[at].text = p->tok.text;
            p->ast->nodes[at].length = p->tok.length;
            bump(p);
            left = at;
        } else if (check(p, GLSL_TOK_INC) || check(p, GLSL_TOK_DEC)) {
            glsl_token_type_t op = p->tok.type;
            int32_t at = node_new(p, GLSL_NODE_POSTFIX);
            if (at == GLSL_NO_NODE)
                return at;
            p->ast->nodes[at].op = op;
            p->ast->nodes[at].a = left;
            bump(p);
            left = at;
        } else {
            return left;
        }
        if (p->error)
            return GLSL_NO_NODE;
    }
}

/* unary: `++ -- + - ! ~` before the operand. Recurses into itself, so `!!x` and `- -x`
 * work. */
static int32_t parse_unary(glsl_parser_t *p) {
    if (p->error)
        return GLSL_NO_NODE;
    if (check(p, GLSL_TOK_INC) || check(p, GLSL_TOK_DEC) || check(p, GLSL_TOK_PLUS) ||
        check(p, GLSL_TOK_MINUS) || check(p, GLSL_TOK_BANG) ||
        check(p, GLSL_TOK_TILDE)) {
        glsl_token_type_t op = p->tok.type;
        int32_t at = node_new(p, GLSL_NODE_UNARY);
        if (at == GLSL_NO_NODE)
            return at;
        p->ast->nodes[at].op = op;
        bump(p);
        p->ast->nodes[at].a = parse_unary(p);
        if (p->error)
            return GLSL_NO_NODE;
        return at;
    }
    return parse_postfix(p);
}

/* Every left-associative binary level is the same loop over a different operator set,
 * so it is written once. `next` is the level below - the one that binds tighter. */
static int32_t parse_binary_level(glsl_parser_t *p, int32_t (*next)(glsl_parser_t *),
                                  const glsl_token_type_t *ops, int n_ops) {
    int32_t left = next(p);
    if (p->error)
        return GLSL_NO_NODE;

    for (;;) {
        int matched = -1;
        for (int i = 0; i < n_ops; i++) {
            if (p->tok.type == ops[i]) {
                matched = i;
                break;
            }
        }
        if (matched < 0)
            return left;

        glsl_token_type_t op = p->tok.type;
        int32_t at = node_new(p, GLSL_NODE_BINARY);
        if (at == GLSL_NO_NODE)
            return at;
        bump(p);
        int32_t right = next(p);
        if (p->error)
            return GLSL_NO_NODE;
        /* `left` is the accumulated tree, not the fresh operand, which makes the level
         * left-associative: `a - b - c` becomes `(a - b) - c`. */
        p->ast->nodes[at].op = op;
        p->ast->nodes[at].a = left;
        p->ast->nodes[at].b = right;
        left = at;
    }
}

/* The precedence ladder, tightest first. Each level names only its own operators. */
static int32_t parse_multiplicative(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_STAR, GLSL_TOK_SLASH,
                                            GLSL_TOK_PERCENT};
    return parse_binary_level(p, parse_unary, ops, 3);
}
static int32_t parse_additive(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_PLUS, GLSL_TOK_MINUS};
    return parse_binary_level(p, parse_multiplicative, ops, 2);
}
static int32_t parse_relational(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_LT, GLSL_TOK_GT, GLSL_TOK_LE,
                                            GLSL_TOK_GE};
    return parse_binary_level(p, parse_additive, ops, 4);
}
static int32_t parse_equality(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_EQ, GLSL_TOK_NE};
    return parse_binary_level(p, parse_relational, ops, 2);
}
/* The bitwise levels sit between equality and the logical operators. GLSL 1.10 only
 * reserves them, but leaving a level out would reassociate everything around it. */
static int32_t parse_bit_and(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_AMP};
    return parse_binary_level(p, parse_equality, ops, 1);
}
static int32_t parse_bit_xor(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_CARET};
    return parse_binary_level(p, parse_bit_and, ops, 1);
}
static int32_t parse_bit_or(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_PIPE};
    return parse_binary_level(p, parse_bit_xor, ops, 1);
}
static int32_t parse_logical_and(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_AND_AND};
    return parse_binary_level(p, parse_bit_or, ops, 1);
}
static int32_t parse_logical_xor(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_XOR_XOR};
    return parse_binary_level(p, parse_logical_and, ops, 1);
}
static int32_t parse_logical_or(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_OR_OR};
    return parse_binary_level(p, parse_logical_xor, ops, 1);
}

/* `a ? b : c`, right-associative: `a ? b : c ? d : e` is `a ? b : (c ? d : e)`. The
 * middle is a full expression because the `:` closes it; the third branch recurses
 * here. */
static int32_t parse_conditional(glsl_parser_t *p) {
    int32_t cond = parse_logical_or(p);
    if (p->error || !check(p, GLSL_TOK_QUESTION))
        return cond;

    int32_t at = node_new(p, GLSL_NODE_CONDITIONAL);
    if (at == GLSL_NO_NODE)
        return at;
    bump(p);
    int32_t yes = glsl_parse_expression(p);
    if (!accept(p, GLSL_TOK_COLON)) {
        fail(p, "expected ':' in a conditional expression");
        return GLSL_NO_NODE;
    }
    int32_t no = parse_conditional(p);
    if (p->error)
        return GLSL_NO_NODE;
    p->ast->nodes[at].a = cond;
    p->ast->nodes[at].b = yes;
    p->ast->nodes[at].c = no;
    return at;
}

/* Assignment, right-associative: `a = b = c` is `a = (b = c)`. The left side is parsed
 * as a conditional; whether it is an l-value is the semantic stage's question, since
 * `(a) = b` is legal. */
static int32_t parse_assignment(glsl_parser_t *p) {
    int32_t left = parse_conditional(p);
    if (p->error)
        return GLSL_NO_NODE;

    if (check(p, GLSL_TOK_ASSIGN) || check(p, GLSL_TOK_ADD_ASSIGN) ||
        check(p, GLSL_TOK_SUB_ASSIGN) || check(p, GLSL_TOK_MUL_ASSIGN) ||
        check(p, GLSL_TOK_DIV_ASSIGN)) {
        glsl_token_type_t op = p->tok.type;
        int32_t at = node_new(p, GLSL_NODE_ASSIGN);
        if (at == GLSL_NO_NODE)
            return at;
        bump(p);
        int32_t right =
            parse_assignment(p); /* recursion on the right: right-associative */
        if (p->error)
            return GLSL_NO_NODE;
        p->ast->nodes[at].op = op;
        p->ast->nodes[at].a = left;
        p->ast->nodes[at].b = right;
        return at;
    }
    return left;
}

int32_t glsl_parse_expression(glsl_parser_t *p) {
    if (!p)
        return GLSL_NO_NODE;
    int32_t left = parse_assignment(p);
    if (p->error)
        return GLSL_NO_NODE;

    /* The comma operator, left-associative and lowest of all. */
    while (check(p, GLSL_TOK_COMMA)) {
        int32_t at = node_new(p, GLSL_NODE_SEQUENCE);
        if (at == GLSL_NO_NODE)
            return at;
        bump(p);
        int32_t right = parse_assignment(p);
        if (p->error)
            return GLSL_NO_NODE;
        p->ast->nodes[at].a = left;
        p->ast->nodes[at].b = right;
        left = at;
    }
    return left;
}

/* -------------------------------------------------------------------------
 * Declarations and statements
 * ------------------------------------------------------------------------- */

/* Is this token a type qualifier - const, attribute, varying, uniform, or a parameter
 * direction? These lead a declaration, so recognising them is how a declaration
 * statement is told apart from an expression statement. */
static GLboolean is_qualifier(glsl_token_type_t t) {
    return (GLboolean)(t == GLSL_TOK_KW_CONST || t == GLSL_TOK_KW_ATTRIBUTE ||
                       t == GLSL_TOK_KW_VARYING || t == GLSL_TOK_KW_UNIFORM ||
                       t == GLSL_TOK_KW_IN || t == GLSL_TOK_KW_OUT ||
                       t == GLSL_TOK_KW_INOUT);
}

/* A precision qualifier in front of a type, dropped: this GL computes in single
 * precision throughout, and the specification accepts the qualifiers with no effect. */
static void skip_precision_qualifiers(glsl_parser_t *p) {
    while (check(p, GLSL_TOK_KW_LOWP) || check(p, GLSL_TOK_KW_MEDIUMP) ||
           check(p, GLSL_TOK_KW_HIGHP)) {
        bump(p);
    }
}

/* GLSL 1.20's `invariant` and `centroid`, which sit before the storage qualifier:
 * `invariant centroid varying vec3 v;`. Both are consumed and not recorded. `invariant`
 * already holds, with one code path per stage and no reordering optimiser; `centroid`
 * changes nothing without a multisample buffer (`GL_SAMPLE_BUFFERS` is 0). Refused by
 * name in a 1.10 shader. */
static void parse_aux_qualifiers(glsl_parser_t *p) {
    skip_precision_qualifiers(p);
    while (check(p, GLSL_TOK_KW_INVARIANT) || check(p, GLSL_TOK_KW_CENTROID)) {
        if (p->version != 0 && p->version < 120) {
            fail(p, "`invariant` and `centroid` are GLSL 1.20; this shader is 1.10");
            return;
        }
        bump(p);
        skip_precision_qualifiers(p);
    }
}

/* Is this token a built-in type name? */
static GLboolean is_type_name(glsl_token_type_t t) {
    switch (t) {
    case GLSL_TOK_KW_VOID:
    case GLSL_TOK_KW_FLOAT:
    case GLSL_TOK_KW_INT:
    case GLSL_TOK_KW_BOOL:
    case GLSL_TOK_KW_VEC2:
    case GLSL_TOK_KW_VEC3:
    case GLSL_TOK_KW_VEC4:
    case GLSL_TOK_KW_IVEC2:
    case GLSL_TOK_KW_IVEC3:
    case GLSL_TOK_KW_IVEC4:
    case GLSL_TOK_KW_BVEC2:
    case GLSL_TOK_KW_BVEC3:
    case GLSL_TOK_KW_BVEC4:
    case GLSL_TOK_KW_MAT2:
    case GLSL_TOK_KW_MAT3:
    case GLSL_TOK_KW_MAT4:
    case GLSL_TOK_KW_MAT2X3:
    case GLSL_TOK_KW_MAT2X4:
    case GLSL_TOK_KW_MAT3X2:
    case GLSL_TOK_KW_MAT3X4:
    case GLSL_TOK_KW_MAT4X2:
    case GLSL_TOK_KW_MAT4X3:
    case GLSL_TOK_KW_SAMPLER1D:
    case GLSL_TOK_KW_SAMPLER2D:
    case GLSL_TOK_KW_SAMPLER3D:
    case GLSL_TOK_KW_SAMPLERCUBE:
    case GLSL_TOK_KW_SAMPLER1DSHADOW:
    case GLSL_TOK_KW_SAMPLER2DSHADOW:
        return GL_TRUE;
    default:
        return GL_FALSE;
    }
}

/* The six matrix types GLSL 1.20 added. `mat2x2` and its two siblings are not here:
 * the lexer folds them into the square tokens, so a 1.10 shader may spell them. */
static GLboolean is_nonsquare_matrix_name(glsl_token_type_t t) {
    switch (t) {
    case GLSL_TOK_KW_MAT2X3:
    case GLSL_TOK_KW_MAT2X4:
    case GLSL_TOK_KW_MAT3X2:
    case GLSL_TOK_KW_MAT3X4:
    case GLSL_TOK_KW_MAT4X2:
    case GLSL_TOK_KW_MAT4X3:
        return GL_TRUE;
    default:
        return GL_FALSE;
    }
}

/* Two names, both pointing into the source rather than copied. */
static GLboolean same_text(const char *a, size_t an, const char *b, size_t bn) {
    if (an != bn)
        return GL_FALSE;
    for (size_t i = 0; i < an; i++) {
        if (a[i] != b[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

/* Whether this identifier names a struct declared earlier in this unit; see
 * `struct_name` on `glsl_parser_t`. */
static GLboolean is_struct_name(const glsl_parser_t *p, const char *t, size_t n) {
    for (int i = 0; i < p->struct_names; i++) {
        if (same_text(p->struct_name[i], p->struct_name_len[i], t, n))
            return GL_TRUE;
    }
    return GL_FALSE;
}

static GLboolean remember_struct_name(glsl_parser_t *p, const char *t, size_t n) {
    if (is_struct_name(p, t, n)) {
        fail(p, "a struct with this name is already declared");
        return GL_FALSE;
    }
    if (p->struct_names >= GLSL_MAX_STRUCTS) {
        fail(p, "too many struct declarations");
        return GL_FALSE;
    }
    p->struct_name[p->struct_names] = t;
    p->struct_name_len[p->struct_names] = n;
    p->struct_names++;
    return GL_TRUE;
}

/* Whether a declaration starts here, by one token of lookahead: a qualifier, a type
 * name or a known struct name begins a declaration, anything else an expression. */
static GLboolean starts_declaration(const glsl_parser_t *p) {
    if (is_qualifier(p->tok.type) || is_type_name(p->tok.type))
        return GL_TRUE;
    /* A precision qualifier begins a declaration the same way a storage one does
     * (`mediump float x;`), and `precision mediump float;` is a statement of its own.
     */
    if (p->tok.type == GLSL_TOK_KW_LOWP || p->tok.type == GLSL_TOK_KW_MEDIUMP ||
        p->tok.type == GLSL_TOK_KW_HIGHP || p->tok.type == GLSL_TOK_KW_PRECISION) {
        return GL_TRUE;
    }
    /* `struct S { ... } s;` declares a type and maybe a variable, and either way begins
     * one. */
    if (p->tok.type == GLSL_TOK_KW_STRUCT)
        return GL_TRUE;
    /* `S s;` where `S` is a struct. */
    return (GLboolean)(p->tok.type == GLSL_TOK_IDENTIFIER &&
                       is_struct_name(p, p->tok.text, p->tok.length));
}

/* `[ expr ]` or `[ ]` after a name. Returns the size expression, or GLSL_NO_NODE for
 * both "not an array" and "an array with no size" - the caller knows which by whether a
 * bracket was consumed, and 1.10 only allows the unsized form on a function parameter.
 */
static int32_t parse_array_suffix(glsl_parser_t *p, GLboolean *saw_bracket) {
    *saw_bracket = GL_FALSE;
    if (!check(p, GLSL_TOK_LBRACKET))
        return GLSL_NO_NODE;
    *saw_bracket = GL_TRUE;
    bump(p);
    int32_t size = GLSL_NO_NODE;
    if (!check(p, GLSL_TOK_RBRACKET)) {
        size = glsl_parse_expression(p);
        if (p->error)
            return GLSL_NO_NODE;
    }
    if (!accept(p, GLSL_TOK_RBRACKET)) {
        fail(p, "expected ']' after an array size");
        return GLSL_NO_NODE;
    }
    return size;
}

/* A declaration after its type has been read: one or more declarators, comma-separated.
 *
 * `float a, b = 1.0, c[4];` is three GLSL_NODE_DECLs chained through `sibling`, each
 * carrying the shared type.
 */
static int32_t parse_declarator_list(glsl_parser_t *p, glsl_token_type_t qualifier,
                                     glsl_token_type_t type_tok, const char *type_name,
                                     size_t type_name_len);

/*
 * `struct Name { members }` - the definition, without the declarators that may follow
 * it.
 *
 * The name is remembered before the body is read, so `struct S { S next; };` parses
 * and the semantic stage can refuse it.
 *
 * GLSL 1.10 allows no qualifiers and no initialisers on a member; both are refused
 * rather than dropped.
 */
static int32_t parse_struct_definition(glsl_parser_t *p) {
    bump(p); /* `struct` */
    if (!check(p, GLSL_TOK_IDENTIFIER)) {
        /* An anonymous struct is legal in C and not in GLSL 1.10. */
        fail(p, "expected a name after `struct`");
        return GLSL_NO_NODE;
    }
    const char *name = p->tok.text;
    const size_t name_len = p->tok.length;
    const int line = p->tok.line, col = p->tok.column;
    if (!remember_struct_name(p, name, name_len))
        return GLSL_NO_NODE;
    bump(p);

    int32_t at = node_new(p, GLSL_NODE_STRUCT_DEF);
    if (at == GLSL_NO_NODE)
        return at;
    p->ast->nodes[at].text = name;
    p->ast->nodes[at].length = name_len;
    p->ast->nodes[at].line = line;
    p->ast->nodes[at].column = col;

    if (!accept(p, GLSL_TOK_LBRACE)) {
        fail(p, "expected `{` after a struct name");
        return GLSL_NO_NODE;
    }

    int32_t first = GLSL_NO_NODE, prev = GLSL_NO_NODE;
    while (!check(p, GLSL_TOK_RBRACE)) {
        if (check(p, GLSL_TOK_EOF) || p->error) {
            fail(p, "expected `}` to close a struct");
            return GLSL_NO_NODE;
        }
        if (is_qualifier(p->tok.type)) {
            fail(p, "a struct member may not have a storage qualifier");
            return GLSL_NO_NODE;
        }
        glsl_token_type_t mtok;
        const char *mname = (const char *)0;
        size_t mname_len = 0;
        if (is_type_name(p->tok.type)) {
            mtok = p->tok.type;
            bump(p);
        } else if (check(p, GLSL_TOK_IDENTIFIER) &&
                   is_struct_name(p, p->tok.text, p->tok.length)) {
            mtok = GLSL_TOK_IDENTIFIER;
            mname = p->tok.text;
            mname_len = p->tok.length;
            bump(p);
        } else {
            fail(p, "expected a type in a struct member");
            return GLSL_NO_NODE;
        }
        int32_t m = parse_declarator_list(p, GLSL_TOK_EOF, mtok, mname, mname_len);
        if (p->error)
            return GLSL_NO_NODE;
        /* An initialiser on a member is not GLSL 1.10. */
        for (int32_t k = m; k != GLSL_NO_NODE; k = p->ast->nodes[k].sibling) {
            if (p->ast->nodes[k].a != GLSL_NO_NODE) {
                fail(p, "a struct member may not have an initialiser");
                return GLSL_NO_NODE;
            }
        }
        if (!accept(p, GLSL_TOK_SEMICOLON)) {
            fail(p, "expected `;` after a struct member");
            return GLSL_NO_NODE;
        }
        if (first == GLSL_NO_NODE)
            first = m;
        else
            p->ast->nodes[prev].sibling = m;
        prev = m;
        while (p->ast->nodes[prev].sibling != GLSL_NO_NODE)
            prev = p->ast->nodes[prev].sibling;
    }
    bump(p); /* `}` */
    if (first == GLSL_NO_NODE) {
        fail(p, "a struct must have at least one member");
        return GLSL_NO_NODE;
    }
    p->ast->nodes[at].a = first;
    return at;
}

/*
 * A type specifier at the head of a declaration: a built-in keyword, a `struct`
 * definition, or the name of a struct already declared. Writes what the declarators
 * will carry, and returns the definition node when there was one so the caller can
 * chain it ahead of them.
 */
static int32_t parse_type_specifier(glsl_parser_t *p, glsl_token_type_t *type_tok,
                                    const char **type_name, size_t *type_name_len) {
    *type_name = (const char *)0;
    *type_name_len = 0;
    if (check(p, GLSL_TOK_KW_STRUCT)) {
        const int32_t def = parse_struct_definition(p);
        if (p->error)
            return GLSL_NO_NODE;
        *type_tok = GLSL_TOK_IDENTIFIER;
        *type_name = p->ast->nodes[def].text;
        *type_name_len = p->ast->nodes[def].length;
        return def;
    }
    if (check(p, GLSL_TOK_IDENTIFIER) &&
        is_struct_name(p, p->tok.text, p->tok.length)) {
        *type_tok = GLSL_TOK_IDENTIFIER;
        *type_name = p->tok.text;
        *type_name_len = p->tok.length;
        bump(p);
        return GLSL_NO_NODE;
    }
    if (!is_type_name(p->tok.type)) {
        fail(p, "expected a type at the start of a declaration");
        return GLSL_NO_NODE;
    }
    /* The non-square matrices are 1.20's; the lexer recognises them whatever the
     * version, and a 1.10 shader is told which word it may not use. */
    if (is_nonsquare_matrix_name(p->tok.type) && p->version != 0 && p->version < 120) {
        fail(p, "the non-square matrix types are GLSL 1.20; this shader is 1.10");
        return GLSL_NO_NODE;
    }
    *type_tok = p->tok.type;
    bump(p);
    return GLSL_NO_NODE;
}

static int32_t parse_declarator_list(glsl_parser_t *p, glsl_token_type_t qualifier,
                                     glsl_token_type_t type_tok, const char *type_name,
                                     size_t type_name_len) {
    int32_t first = GLSL_NO_NODE, prev = GLSL_NO_NODE;
    for (;;) {
        if (!check(p, GLSL_TOK_IDENTIFIER)) {
            fail(p, "expected a name in a declaration");
            return GLSL_NO_NODE;
        }
        int32_t at = node_new(p, GLSL_NODE_DECL);
        if (at == GLSL_NO_NODE)
            return at;
        p->ast->nodes[at].text = p->tok.text;
        p->ast->nodes[at].length = p->tok.length;
        p->ast->nodes[at].qualifier = qualifier;
        p->ast->nodes[at].type_tok = type_tok;
        p->ast->nodes[at].type_name = type_name;
        p->ast->nodes[at].type_name_len = type_name_len;
        bump(p);

        GLboolean bracketed = GL_FALSE;
        int32_t size = parse_array_suffix(p, &bracketed);
        if (p->error)
            return GLSL_NO_NODE;
        p->ast->nodes[at].array_size = size;

        if (accept(p, GLSL_TOK_ASSIGN)) {
            /* The initialiser is an assignment-expression, not a full expression: a
             * comma here separates declarators. */
            int32_t init = parse_assignment(p);
            if (p->error)
                return GLSL_NO_NODE;
            p->ast->nodes[at].a = init;
        }

        if (first == GLSL_NO_NODE)
            first = at;
        else
            p->ast->nodes[prev].sibling = at;
        prev = at;

        if (!accept(p, GLSL_TOK_COMMA))
            break;
    }
    return first;
}

/* -------------------------------------------------------------------------
 * Statements
 * ------------------------------------------------------------------------- */

static int32_t parse_compound(glsl_parser_t *p) {
    if (!accept(p, GLSL_TOK_LBRACE)) {
        fail(p, "expected '{'");
        return GLSL_NO_NODE;
    }
    int32_t at = node_new(p, GLSL_NODE_COMPOUND);
    if (at == GLSL_NO_NODE)
        return at;

    int32_t first = GLSL_NO_NODE, prev = GLSL_NO_NODE;
    while (!check(p, GLSL_TOK_RBRACE)) {
        if (check(p, GLSL_TOK_EOF) || p->error) {
            fail(p, "expected '}' to close a block");
            return GLSL_NO_NODE;
        }
        int32_t s = glsl_parse_statement(p);
        if (p->error)
            return GLSL_NO_NODE;
        if (first == GLSL_NO_NODE)
            first = s;
        else
            p->ast->nodes[prev].sibling = s;
        /* A declaration statement may already be a chain: `float a, b;` returns two
         * DECLs linked through `sibling`, the field this list uses, so the tail is
         * found rather than assumed. */
        prev = s;
        while (p->ast->nodes[prev].sibling != GLSL_NO_NODE)
            prev = p->ast->nodes[prev].sibling;
    }
    bump(p); /* '}' */
    p->ast->nodes[at].a = first;
    return at;
}

int32_t glsl_parse_statement(glsl_parser_t *p) {
    if (!p || p->error)
        return GLSL_NO_NODE;

    if (check(p, GLSL_TOK_LBRACE))
        return parse_compound(p);

    if (starts_declaration(p)) {
        glsl_token_type_t qualifier = GLSL_TOK_EOF;
        if (is_qualifier(p->tok.type)) {
            qualifier = p->tok.type;
            bump(p);
        }
        /* `uniform mediump float x;` - the precision word sits between the two. */
        skip_precision_qualifiers(p);
        glsl_token_type_t type_tok = GLSL_TOK_EOF;
        const char *type_name = (const char *)0;
        size_t type_name_len = 0;
        const int32_t def =
            parse_type_specifier(p, &type_tok, &type_name, &type_name_len);
        if (p->error)
            return GLSL_NO_NODE;

        /* `struct S { ... };` with no declarator is a whole statement that declares
         * only the type. */
        if (def != GLSL_NO_NODE && check(p, GLSL_TOK_SEMICOLON)) {
            bump(p);
            return def;
        }
        int32_t decl =
            parse_declarator_list(p, qualifier, type_tok, type_name, type_name_len);
        if (p->error)
            return GLSL_NO_NODE;
        if (!accept(p, GLSL_TOK_SEMICOLON)) {
            fail(p, "expected ';' after a declaration");
            return GLSL_NO_NODE;
        }
        /* The definition comes first and the variables hang off it, so a walk in order
         * sees the type declared before anything is declared of it. */
        if (def != GLSL_NO_NODE) {
            p->ast->nodes[def].sibling = decl;
            return def;
        }
        return decl;
    }

    if (check(p, GLSL_TOK_KW_IF)) {
        int32_t at = node_new(p, GLSL_NODE_IF);
        if (at == GLSL_NO_NODE)
            return at;
        bump(p);
        if (!accept(p, GLSL_TOK_LPAREN)) {
            fail(p, "expected '(' after 'if'");
            return GLSL_NO_NODE;
        }
        int32_t cond = glsl_parse_expression(p);
        if (!accept(p, GLSL_TOK_RPAREN)) {
            fail(p, "expected ')' after an if condition");
            return GLSL_NO_NODE;
        }
        int32_t then_s = glsl_parse_statement(p);
        if (p->error)
            return GLSL_NO_NODE;
        int32_t else_s = GLSL_NO_NODE;
        /* The dangling else binds to the nearest `if`: the inner `if` is parsed by the
         * recursive call above and consumes the `else` first. A test asserts the
         * shape. */
        if (accept(p, GLSL_TOK_KW_ELSE)) {
            else_s = glsl_parse_statement(p);
            if (p->error)
                return GLSL_NO_NODE;
        }
        p->ast->nodes[at].a = cond;
        p->ast->nodes[at].b = then_s;
        p->ast->nodes[at].c = else_s;
        return at;
    }

    if (check(p, GLSL_TOK_KW_WHILE)) {
        int32_t at = node_new(p, GLSL_NODE_WHILE);
        if (at == GLSL_NO_NODE)
            return at;
        bump(p);
        if (!accept(p, GLSL_TOK_LPAREN)) {
            fail(p, "expected '(' after 'while'");
            return GLSL_NO_NODE;
        }
        int32_t cond = glsl_parse_expression(p);
        if (!accept(p, GLSL_TOK_RPAREN)) {
            fail(p, "expected ')' after a while condition");
            return GLSL_NO_NODE;
        }
        int32_t body = glsl_parse_statement(p);
        if (p->error)
            return GLSL_NO_NODE;
        p->ast->nodes[at].a = cond;
        p->ast->nodes[at].b = body;
        return at;
    }

    if (check(p, GLSL_TOK_KW_DO)) {
        int32_t at = node_new(p, GLSL_NODE_DO_WHILE);
        if (at == GLSL_NO_NODE)
            return at;
        bump(p);
        int32_t body = glsl_parse_statement(p);
        if (p->error)
            return GLSL_NO_NODE;
        if (!accept(p, GLSL_TOK_KW_WHILE)) {
            fail(p, "expected 'while' after a do body");
            return GLSL_NO_NODE;
        }
        if (!accept(p, GLSL_TOK_LPAREN)) {
            fail(p, "expected '(' after 'while'");
            return GLSL_NO_NODE;
        }
        int32_t cond = glsl_parse_expression(p);
        if (!accept(p, GLSL_TOK_RPAREN)) {
            fail(p, "expected ')' after a while condition");
            return GLSL_NO_NODE;
        }
        if (!accept(p, GLSL_TOK_SEMICOLON)) {
            fail(p, "expected ';' after do-while");
            return GLSL_NO_NODE;
        }
        p->ast->nodes[at].a = body;
        p->ast->nodes[at].b = cond;
        return at;
    }

    if (check(p, GLSL_TOK_KW_FOR)) {
        int32_t at = node_new(p, GLSL_NODE_FOR);
        if (at == GLSL_NO_NODE)
            return at;
        bump(p);
        if (!accept(p, GLSL_TOK_LPAREN)) {
            fail(p, "expected '(' after 'for'");
            return GLSL_NO_NODE;
        }
        /* The init clause is a statement, so it can be a declaration - `for (int i = 0;
         * ...)` - and it consumes its own semicolon either way. */
        int32_t init = GLSL_NO_NODE;
        if (accept(p, GLSL_TOK_SEMICOLON)) {
            init = GLSL_NO_NODE;
        } else {
            init = glsl_parse_statement(p);
            if (p->error)
                return GLSL_NO_NODE;
        }
        int32_t cond = GLSL_NO_NODE;
        if (!check(p, GLSL_TOK_SEMICOLON)) {
            cond = glsl_parse_expression(p);
            if (p->error)
                return GLSL_NO_NODE;
        }
        if (!accept(p, GLSL_TOK_SEMICOLON)) {
            fail(p, "expected ';' in a for header");
            return GLSL_NO_NODE;
        }
        int32_t step = GLSL_NO_NODE;
        if (!check(p, GLSL_TOK_RPAREN)) {
            step = glsl_parse_expression(p);
            if (p->error)
                return GLSL_NO_NODE;
        }
        if (!accept(p, GLSL_TOK_RPAREN)) {
            fail(p, "expected ')' after a for header");
            return GLSL_NO_NODE;
        }
        int32_t body = glsl_parse_statement(p);
        if (p->error)
            return GLSL_NO_NODE;
        p->ast->nodes[at].a = init;
        p->ast->nodes[at].b = cond;
        p->ast->nodes[at].c = step;
        p->ast->nodes[at].d = body;
        return at;
    }

    if (check(p, GLSL_TOK_KW_RETURN)) {
        int32_t at = node_new(p, GLSL_NODE_RETURN);
        if (at == GLSL_NO_NODE)
            return at;
        bump(p);
        if (!check(p, GLSL_TOK_SEMICOLON)) {
            p->ast->nodes[at].a = glsl_parse_expression(p);
            if (p->error)
                return GLSL_NO_NODE;
        }
        if (!accept(p, GLSL_TOK_SEMICOLON)) {
            fail(p, "expected ';' after 'return'");
            return GLSL_NO_NODE;
        }
        return at;
    }

    if (check(p, GLSL_TOK_KW_BREAK) || check(p, GLSL_TOK_KW_CONTINUE) ||
        check(p, GLSL_TOK_KW_DISCARD)) {
        glsl_node_kind_t kind = check(p, GLSL_TOK_KW_BREAK)      ? GLSL_NODE_BREAK
                                : check(p, GLSL_TOK_KW_CONTINUE) ? GLSL_NODE_CONTINUE
                                                                 : GLSL_NODE_DISCARD;
        int32_t at = node_new(p, kind);
        if (at == GLSL_NO_NODE)
            return at;
        bump(p);
        if (!accept(p, GLSL_TOK_SEMICOLON)) {
            fail(p, "expected ';' after a jump");
            return GLSL_NO_NODE;
        }
        return at;
    }

    /* Expression statement, including the empty one. */
    {
        int32_t at = node_new(p, GLSL_NODE_EXPR_STMT);
        if (at == GLSL_NO_NODE)
            return at;
        if (!check(p, GLSL_TOK_SEMICOLON)) {
            p->ast->nodes[at].a = glsl_parse_expression(p);
            if (p->error)
                return GLSL_NO_NODE;
        }
        if (!accept(p, GLSL_TOK_SEMICOLON)) {
            fail(p, "expected ';' after an expression");
            return GLSL_NO_NODE;
        }
        return at;
    }
}

/* -------------------------------------------------------------------------
 * The translation unit
 * ------------------------------------------------------------------------- */

static int32_t parse_parameter_list(glsl_parser_t *p) {
    int32_t first = GLSL_NO_NODE, prev = GLSL_NO_NODE;
    if (check(p, GLSL_TOK_RPAREN))
        return GLSL_NO_NODE;

    /* `void` alone means no parameters. Anything after it is a parameter of type
     * `void`, which GLSL has no use for, so it is refused rather than rewound. */
    if (check(p, GLSL_TOK_KW_VOID)) {
        bump(p);
        if (check(p, GLSL_TOK_RPAREN))
            return GLSL_NO_NODE;
        fail(p, "a parameter may not be void");
        return GLSL_NO_NODE;
    }

    for (;;) {
        glsl_token_type_t qualifier = GLSL_TOK_EOF;
        if (is_qualifier(p->tok.type)) {
            qualifier = p->tok.type;
            bump(p);
        }
        /* `void f(in mediump float x)` - a parameter carries one the same way. */
        skip_precision_qualifiers(p);
        /* A parameter may be a struct, so the declarations' type specifier applies.
         * A struct definition in a parameter's type is not GLSL; `parse_type_specifier`
         * would accept one, so it is refused here first. */
        if (check(p, GLSL_TOK_KW_STRUCT)) {
            fail(p, "a struct may not be defined in a parameter list");
            return GLSL_NO_NODE;
        }
        glsl_token_type_t type_tok = GLSL_TOK_EOF;
        const char *type_name = (const char *)0;
        size_t type_name_len = 0;
        (void)parse_type_specifier(p, &type_tok, &type_name, &type_name_len);
        if (p->error)
            return GLSL_NO_NODE;

        int32_t at = node_new(p, GLSL_NODE_PARAM);
        if (at == GLSL_NO_NODE)
            return at;
        p->ast->nodes[at].qualifier = qualifier;
        p->ast->nodes[at].type_tok = type_tok;
        p->ast->nodes[at].type_name = type_name;
        p->ast->nodes[at].type_name_len = type_name_len;
        /* The name is optional in a prototype. */
        if (check(p, GLSL_TOK_IDENTIFIER)) {
            p->ast->nodes[at].text = p->tok.text;
            p->ast->nodes[at].length = p->tok.length;
            bump(p);
            GLboolean bracketed = GL_FALSE;
            p->ast->nodes[at].array_size = parse_array_suffix(p, &bracketed);
            if (p->error)
                return GLSL_NO_NODE;
        }

        if (first == GLSL_NO_NODE)
            first = at;
        else
            p->ast->nodes[prev].sibling = at;
        prev = at;

        if (!accept(p, GLSL_TOK_COMMA))
            break;
    }
    return first;
}

/* One external declaration: a function definition, a function prototype, or a variable
 * declaration. All three start with a type, so they are told apart by what follows the
 * name: `(` means a function, anything else a variable. */
static int32_t parse_external_declaration(glsl_parser_t *p) {
    /* `precision mediump float;` declares nothing: the qualifiers have no effect here,
     * so the statement is consumed and produces no node. Taken first, because every
     * later branch would read the type after `precision` as a declaration. */
    if (check(p, GLSL_TOK_KW_PRECISION)) {
        bump(p);
        if (!accept(p, GLSL_TOK_KW_LOWP) && !accept(p, GLSL_TOK_KW_MEDIUMP) &&
            !accept(p, GLSL_TOK_KW_HIGHP)) {
            fail(p, "expected `lowp`, `mediump` or `highp` after `precision`");
            return GLSL_NO_NODE;
        }
        /* The type it applies to; which one does not matter. */
        if (check(p, GLSL_TOK_EOF) || check(p, GLSL_TOK_SEMICOLON)) {
            fail(p, "expected a type after a precision qualifier");
            return GLSL_NO_NODE;
        }
        bump(p);
        if (accept(p, GLSL_TOK_SEMICOLON))
            return GLSL_NO_NODE;
        fail(p, "expected `;` after a precision statement");
        return GLSL_NO_NODE;
    }

    /* `invariant name;` on its own is a whole declaration in GLSL 1.20: a restatement
     * that a variable already declared, usually `gl_Position`, is invariant. It
     * declares nothing new and produces no node. */
    parse_aux_qualifiers(p);
    if (p->error)
        return GLSL_NO_NODE;
    /* An identifier where a type should be is that restatement form, unless it names a
     * struct, in which case `S s;` is an ordinary declaration. */
    if (check(p, GLSL_TOK_IDENTIFIER) &&
        !is_struct_name(p, p->tok.text, p->tok.length)) {
        bump(p);
        if (accept(p, GLSL_TOK_SEMICOLON))
            return GLSL_NO_NODE;
        fail(p, "expected `;` after an invariant restatement");
        return GLSL_NO_NODE;
    }

    glsl_token_type_t qualifier = GLSL_TOK_EOF;
    if (is_qualifier(p->tok.type)) {
        qualifier = p->tok.type;
        bump(p);
    }
    skip_precision_qualifiers(p);
    glsl_token_type_t type_tok = GLSL_TOK_EOF;
    const char *type_name = (const char *)0;
    size_t type_name_len = 0;
    const int32_t struct_def =
        parse_type_specifier(p, &type_tok, &type_name, &type_name_len);
    if (p->error)
        return GLSL_NO_NODE;

    /* A struct declared at file scope with no variable after it. */
    if (struct_def != GLSL_NO_NODE && accept(p, GLSL_TOK_SEMICOLON))
        return struct_def;

    if (!check(p, GLSL_TOK_IDENTIFIER)) {
        fail(p, "expected a name after a type");
        return GLSL_NO_NODE;
    }
    const char *name = p->tok.text;
    size_t name_len = p->tok.length;
    int name_line = p->tok.line, name_col = p->tok.column;
    bump(p);

    if (accept(p, GLSL_TOK_LPAREN)) {
        int32_t at = node_new(p, GLSL_NODE_FUNCTION);
        if (at == GLSL_NO_NODE)
            return at;
        p->ast->nodes[at].text = name;
        p->ast->nodes[at].length = name_len;
        p->ast->nodes[at].type_tok = type_tok;
        /* The return type, which may be a struct: `S bump(S v)`. */
        p->ast->nodes[at].type_name = type_name;
        p->ast->nodes[at].type_name_len = type_name_len;
        p->ast->nodes[at].qualifier = qualifier;
        p->ast->nodes[at].line = name_line;
        p->ast->nodes[at].column = name_col;
        p->ast->nodes[at].b = parse_parameter_list(p);
        if (p->error)
            return GLSL_NO_NODE;
        if (!accept(p, GLSL_TOK_RPAREN)) {
            fail(p, "expected ')' to close a parameter list");
            return GLSL_NO_NODE;
        }
        if (check(p, GLSL_TOK_LBRACE)) {
            p->ast->nodes[at].c = parse_compound(p);
            if (p->error)
                return GLSL_NO_NODE;
        } else if (!accept(p, GLSL_TOK_SEMICOLON)) {
            fail(p, "expected a body or ';' after a function header");
            return GLSL_NO_NODE;
        }
        /* `c` left absent means a prototype; an empty body is a COMPOUND node with no
         * statements. */
        return at;
    }

    /* A variable declaration. The name has already been consumed, so the first
     * declarator is built here and any further ones come from the shared list parser.
     */
    int32_t at = node_new(p, GLSL_NODE_DECL);
    if (at == GLSL_NO_NODE)
        return at;
    p->ast->nodes[at].text = name;
    p->ast->nodes[at].length = name_len;
    p->ast->nodes[at].qualifier = qualifier;
    p->ast->nodes[at].type_tok = type_tok;
    p->ast->nodes[at].type_name = type_name;
    p->ast->nodes[at].type_name_len = type_name_len;
    p->ast->nodes[at].line = name_line;
    p->ast->nodes[at].column = name_col;

    GLboolean bracketed = GL_FALSE;
    p->ast->nodes[at].array_size = parse_array_suffix(p, &bracketed);
    if (p->error)
        return GLSL_NO_NODE;
    if (accept(p, GLSL_TOK_ASSIGN)) {
        p->ast->nodes[at].a = parse_assignment(p);
        if (p->error)
            return GLSL_NO_NODE;
    }
    if (accept(p, GLSL_TOK_COMMA)) {
        int32_t rest =
            parse_declarator_list(p, qualifier, type_tok, type_name, type_name_len);
        if (p->error)
            return GLSL_NO_NODE;
        p->ast->nodes[at].sibling = rest;
    }
    if (!accept(p, GLSL_TOK_SEMICOLON)) {
        fail(p, "expected ';' after a declaration");
        return GLSL_NO_NODE;
    }
    /* As in a statement: the type is declared ahead of the variables of it. */
    if (struct_def != GLSL_NO_NODE) {
        p->ast->nodes[struct_def].sibling = at;
        return struct_def;
    }
    return at;
}

int32_t glsl_parse_translation_unit(glsl_parser_t *p) {
    if (!p)
        return GLSL_NO_NODE;
    int32_t at = node_new(p, GLSL_NODE_UNIT);
    if (at == GLSL_NO_NODE)
        return at;

    int32_t first = GLSL_NO_NODE, prev = GLSL_NO_NODE;
    while (!check(p, GLSL_TOK_EOF)) {
        if (p->error)
            return GLSL_NO_NODE;
        /* A `#` reaches here only when the raw lexer is read without the
         * preprocessor. It is refused rather than skipped, since skipping would ignore
         * a `#version` or a `#ifdef`. */
        if (check(p, GLSL_TOK_HASH)) {
            fail(p, "preprocessor directives are not handled yet");
            return GLSL_NO_NODE;
        }
        int32_t d = parse_external_declaration(p);
        if (p->error)
            return GLSL_NO_NODE;
        /* No node and no error is a declaration that declares nothing, such as
         * `invariant gl_Position;`. It is skipped, since GLSL_NO_NODE would end the
         * chain. */
        if (d == GLSL_NO_NODE)
            continue;
        if (first == GLSL_NO_NODE)
            first = d;
        else
            p->ast->nodes[prev].sibling = d;
        /* A declarator list chains through `sibling` too, so the tail has to be found
         * rather than assumed to be the node just returned. */
        prev = d;
        while (p->ast->nodes[prev].sibling != GLSL_NO_NODE)
            prev = p->ast->nodes[prev].sibling;
    }
    p->ast->nodes[at].a = first;
    return at;
}
