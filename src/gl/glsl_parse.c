/*
 * oops-gl: the GLSL expression parser
 *
 * Recursive descent over GLSL 1.10's expression grammar. One function per precedence level,
 * which is verbose and is the point: the alternative is a precedence table, and a table with one
 * wrong number in it produces a program that parses and computes something else.
 *
 * # The two things this has to get right
 *
 * **Associativity.** `a - b - c` is `(a - b) - c` and `a = b = c` is `a = (b = c)`. The binary
 * levels loop, which builds left-associatively; assignment and the conditional recurse into
 * themselves on the right, which builds right-associatively. Getting one of these backwards
 * still parses every program - it just computes a different answer, silently.
 *
 * **Precedence, including the levels nothing uses.** GLSL 1.10 has `|`, `^` and `&` between the
 * logical operators and equality even though 1.10 reserves them for integers it barely has.
 * Leaving a level out does not produce an error: it reassociates everything around it.
 *
 * The parser stops at the first error. Unlike the lexer, which reports an error token and keeps
 * going, a syntax error leaves the parser with no idea where it is - resynchronising is a real
 * feature and is not written yet, so this says so rather than producing a tree that is wrong in
 * a way nothing downstream would notice.
 */

#include "glsl_internal.h"

static void fail(glsl_parser_t *p, const char *why) {
    if (p->error) return; /* the first error is the one that means something */
    p->error = why;
    p->error_line = p->tok.line;
    p->error_column = p->tok.column;
}

static void bump(glsl_parser_t *p) {
    /* **The one place a token enters the parser**, which is what lets the preprocessor be
     * slotted in front of the lexer rather than run as a separate pass over the text. With `pp`
     * set, directives are obeyed and macros expanded before the grammar ever sees anything; with
     * it null the raw lexer is read, which is how this file's own tests drive it.
     *
     * Either source writes EOF and returns false at the end, so the lookahead stays valid and
     * every `accept` afterwards simply fails. */
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
    if (!check(p, t)) return GL_FALSE;
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
    /* **Cleared here or it is whatever the arena held.** The node arena is reused across parses
     * and is not zeroed, so a field added to this struct and set at only some of its call sites
     * is read as a stale pointer at the others. */
    n->type_name = (const char *)0;
    n->type_name_len = 0u;
    n->qualifier = GLSL_TOK_EOF;
    n->array_size = GLSL_NO_NODE;
    n->text = (const char *)0;
    n->length = 0u;
    n->value = 0.0;
    n->line = p->tok.line;
    n->column = p->tok.column;
    return at;
}

void glsl_parser_init(glsl_parser_t *p, glsl_ast_t *ast, const char *source, size_t length) {
    if (!p) return;
    p->ast = ast;
    if (ast) ast->count = 0;
    p->error = (const char *)0;
    p->error_line = 0;
    p->error_column = 0;
    p->pp = (glsl_pp_t *)0;
    p->version = 0;   /* not stated: enforce nothing - see glsl_parser_t */
    /* **Cleared, because the caller's parser is not zeroed.** `starts_declaration` walks this
     * list for every identifier it meets, so a stale count sends it through stale pointers on
     * the first shader parsed - struct or not. */
    p->struct_names = 0;
    glsl_lexer_init(&p->lx, source, length);
    bump(p);
}

void glsl_parser_init_pp(glsl_parser_t *p, glsl_ast_t *ast, glsl_pp_t *pp) {
    if (!p) return;
    p->ast = ast;
    if (ast) ast->count = 0;
    p->error = (const char *)0;
    p->error_line = 0;
    p->error_column = 0;
    p->pp = pp;
    /* The lexer is left inert: with `pp` set nothing reads it, and the preprocessor has its
     * own over the same source. */
    glsl_lexer_init(&p->lx, (const char *)0, 0);
    p->version = 0;
    p->struct_names = 0;   /* as in glsl_parser_init, and for the same reason */
    /* **This consumes every directive before the first real token**, so `#version` has already
     * been read when this returns - which is what lets a caller refuse a language it does not
     * implement before parsing a line of it. */
    bump(p);
    /* A shader that says nothing is GLSL 1.10, which the specification states outright. */
    p->version = (pp && pp->version) ? pp->version : 110;
}

static int32_t parse_assignment(glsl_parser_t *p);

/* primary: identifier, literal, or a parenthesised expression. */
static int32_t parse_primary(glsl_parser_t *p) {
    if (p->error) return GLSL_NO_NODE;

    switch (p->tok.type) {
        case GLSL_TOK_IDENTIFIER: {
            int32_t at = node_new(p, GLSL_NODE_IDENTIFIER);
            if (at == GLSL_NO_NODE) return at;
            p->ast->nodes[at].text = p->tok.text;
            p->ast->nodes[at].length = p->tok.length;
            bump(p);
            return at;
        }
        /* A type name used as a constructor - `vec4(...)` - is an identifier here. Deciding
         * that `vec4` names a type rather than a function is the semantic stage's job, and the
         * grammar cannot tell them apart anyway. */
        case GLSL_TOK_KW_FLOAT: case GLSL_TOK_KW_INT: case GLSL_TOK_KW_BOOL:
        case GLSL_TOK_KW_VEC2: case GLSL_TOK_KW_VEC3: case GLSL_TOK_KW_VEC4:
        case GLSL_TOK_KW_IVEC2: case GLSL_TOK_KW_IVEC3: case GLSL_TOK_KW_IVEC4:
        case GLSL_TOK_KW_BVEC2: case GLSL_TOK_KW_BVEC3: case GLSL_TOK_KW_BVEC4:
        case GLSL_TOK_KW_MAT2: case GLSL_TOK_KW_MAT3: case GLSL_TOK_KW_MAT4: {
            int32_t at = node_new(p, GLSL_NODE_IDENTIFIER);
            if (at == GLSL_NO_NODE) return at;
            p->ast->nodes[at].text = p->tok.text;
            p->ast->nodes[at].length = p->tok.length;
            bump(p);
            return at;
        }
        case GLSL_TOK_INTCONST:
        case GLSL_TOK_FLOATCONST: {
            GLboolean is_int = check(p, GLSL_TOK_INTCONST);
            int32_t at = node_new(p, is_int ? GLSL_NODE_INTCONST : GLSL_NODE_FLOATCONST);
            if (at == GLSL_NO_NODE) return at;
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
            if (at == GLSL_NO_NODE) return at;
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
            /* **No node for the parentheses.** They grouped the parse and have no meaning
             * afterwards; keeping one would make every consumer skip over it. */
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
 * `((a[i]).x)++` and nothing about that is right-associative. */
static int32_t parse_postfix(glsl_parser_t *p) {
    int32_t left = parse_primary(p);
    if (p->error) return GLSL_NO_NODE;

    for (;;) {
        if (check(p, GLSL_TOK_LBRACKET)) {
            bump(p);
            int32_t at = node_new(p, GLSL_NODE_INDEX);
            if (at == GLSL_NO_NODE) return at;
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
            if (at == GLSL_NO_NODE) return at;
            p->ast->nodes[at].a = left;
            /* Arguments chain through `sibling`. An empty list is legal - `f()` - so the
             * closing paren is tested before the first argument is demanded. */
            if (!check(p, GLSL_TOK_RPAREN)) {
                int32_t first = parse_assignment(p); /* not glsl_parse_expression: a comma here
                                                      * separates arguments, it is not the
                                                      * sequence operator */
                if (p->error) return GLSL_NO_NODE;
                p->ast->nodes[at].b = first;
                int32_t prev = first;
                while (accept(p, GLSL_TOK_COMMA)) {
                    int32_t next = parse_assignment(p);
                    if (p->error) return GLSL_NO_NODE;
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
            if (at == GLSL_NO_NODE) return at;
            p->ast->nodes[at].a = left;
            p->ast->nodes[at].text = p->tok.text;
            p->ast->nodes[at].length = p->tok.length;
            bump(p);
            left = at;
        } else if (check(p, GLSL_TOK_INC) || check(p, GLSL_TOK_DEC)) {
            glsl_token_type_t op = p->tok.type;
            int32_t at = node_new(p, GLSL_NODE_POSTFIX);
            if (at == GLSL_NO_NODE) return at;
            p->ast->nodes[at].op = op;
            p->ast->nodes[at].a = left;
            bump(p);
            left = at;
        } else {
            return left;
        }
        if (p->error) return GLSL_NO_NODE;
    }
}

/* unary: `++ -- + - ! ~` before the operand. Recurses into itself, so `!!x` and `- -x` work and
 * the whole chain is right-associative, which is the only way prefix operators can associate. */
static int32_t parse_unary(glsl_parser_t *p) {
    if (p->error) return GLSL_NO_NODE;
    if (check(p, GLSL_TOK_INC) || check(p, GLSL_TOK_DEC) || check(p, GLSL_TOK_PLUS) ||
        check(p, GLSL_TOK_MINUS) || check(p, GLSL_TOK_BANG) || check(p, GLSL_TOK_TILDE)) {
        glsl_token_type_t op = p->tok.type;
        int32_t at = node_new(p, GLSL_NODE_UNARY);
        if (at == GLSL_NO_NODE) return at;
        p->ast->nodes[at].op = op;
        bump(p);
        p->ast->nodes[at].a = parse_unary(p);
        if (p->error) return GLSL_NO_NODE;
        return at;
    }
    return parse_postfix(p);
}

/* Every left-associative binary level is the same loop over a different operator set, so it is
 * written once. `next` is the level below - the one that binds tighter. */
static int32_t parse_binary_level(glsl_parser_t *p,
                                  int32_t (*next)(glsl_parser_t *),
                                  const glsl_token_type_t *ops, int n_ops) {
    int32_t left = next(p);
    if (p->error) return GLSL_NO_NODE;

    for (;;) {
        int matched = -1;
        for (int i = 0; i < n_ops; i++) {
            if (p->tok.type == ops[i]) { matched = i; break; }
        }
        if (matched < 0) return left;

        glsl_token_type_t op = p->tok.type;
        int32_t at = node_new(p, GLSL_NODE_BINARY);
        if (at == GLSL_NO_NODE) return at;
        bump(p);
        int32_t right = next(p);
        if (p->error) return GLSL_NO_NODE;
        /* **`left` is the accumulated tree, not the fresh operand.** This is what makes the
         * level left-associative: `a - b - c` becomes `(a - b) - c`. Assigning the other way
         * round still parses every program and computes a different answer. */
        p->ast->nodes[at].op = op;
        p->ast->nodes[at].a = left;
        p->ast->nodes[at].b = right;
        left = at;
    }
}

/* The precedence ladder, tightest first. Each level names only its own operators; the order of
 * the functions *is* the precedence table, which is why there is no table. */
static int32_t parse_multiplicative(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_STAR, GLSL_TOK_SLASH, GLSL_TOK_PERCENT};
    return parse_binary_level(p, parse_unary, ops, 3);
}
static int32_t parse_additive(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_PLUS, GLSL_TOK_MINUS};
    return parse_binary_level(p, parse_multiplicative, ops, 2);
}
static int32_t parse_relational(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_LT, GLSL_TOK_GT, GLSL_TOK_LE, GLSL_TOK_GE};
    return parse_binary_level(p, parse_additive, ops, 4);
}
static int32_t parse_equality(glsl_parser_t *p) {
    static const glsl_token_type_t ops[] = {GLSL_TOK_EQ, GLSL_TOK_NE};
    return parse_binary_level(p, parse_relational, ops, 2);
}
/* The bitwise levels sit between equality and the logical operators. GLSL 1.10 reserves them
 * rather than using them much - but **leaving a level out does not raise an error, it silently
 * reassociates everything around it**, so they are here. */
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

/* `a ? b : c`, **right-associative**: `a ? b : c ? d : e` is `a ? b : (c ? d : e)`. The middle
 * is a full expression because the `:` closes it unambiguously; the third branch recurses here
 * rather than into the level above, which is what the associativity means. */
static int32_t parse_conditional(glsl_parser_t *p) {
    int32_t cond = parse_logical_or(p);
    if (p->error || !check(p, GLSL_TOK_QUESTION)) return cond;

    int32_t at = node_new(p, GLSL_NODE_CONDITIONAL);
    if (at == GLSL_NO_NODE) return at;
    bump(p);
    int32_t yes = glsl_parse_expression(p);
    if (!accept(p, GLSL_TOK_COLON)) {
        fail(p, "expected ':' in a conditional expression");
        return GLSL_NO_NODE;
    }
    int32_t no = parse_conditional(p);
    if (p->error) return GLSL_NO_NODE;
    p->ast->nodes[at].a = cond;
    p->ast->nodes[at].b = yes;
    p->ast->nodes[at].c = no;
    return at;
}

/* Assignment, **right-associative**: `a = b = c` is `a = (b = c)`.
 *
 * The left side is parsed as a conditional and then accepted as a target if an assignment
 * operator follows. Whether it is actually assignable - an l-value - is a semantic question,
 * not a grammatical one, and answering it here would reject `(a) = b` which is legal. */
static int32_t parse_assignment(glsl_parser_t *p) {
    int32_t left = parse_conditional(p);
    if (p->error) return GLSL_NO_NODE;

    if (check(p, GLSL_TOK_ASSIGN) || check(p, GLSL_TOK_ADD_ASSIGN) ||
        check(p, GLSL_TOK_SUB_ASSIGN) || check(p, GLSL_TOK_MUL_ASSIGN) ||
        check(p, GLSL_TOK_DIV_ASSIGN)) {
        glsl_token_type_t op = p->tok.type;
        int32_t at = node_new(p, GLSL_NODE_ASSIGN);
        if (at == GLSL_NO_NODE) return at;
        bump(p);
        int32_t right = parse_assignment(p); /* recursion on the right: right-associative */
        if (p->error) return GLSL_NO_NODE;
        p->ast->nodes[at].op = op;
        p->ast->nodes[at].a = left;
        p->ast->nodes[at].b = right;
        return at;
    }
    return left;
}

int32_t glsl_parse_expression(glsl_parser_t *p) {
    if (!p) return GLSL_NO_NODE;
    int32_t left = parse_assignment(p);
    if (p->error) return GLSL_NO_NODE;

    /* The comma operator, left-associative and lowest of all. */
    while (check(p, GLSL_TOK_COMMA)) {
        int32_t at = node_new(p, GLSL_NODE_SEQUENCE);
        if (at == GLSL_NO_NODE) return at;
        bump(p);
        int32_t right = parse_assignment(p);
        if (p->error) return GLSL_NO_NODE;
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
 * direction? These lead a declaration, so recognising them is how a declaration statement is
 * told apart from an expression statement. */
static GLboolean is_qualifier(glsl_token_type_t t) {
    return (GLboolean)(t == GLSL_TOK_KW_CONST || t == GLSL_TOK_KW_ATTRIBUTE ||
                       t == GLSL_TOK_KW_VARYING || t == GLSL_TOK_KW_UNIFORM ||
                       t == GLSL_TOK_KW_IN || t == GLSL_TOK_KW_OUT || t == GLSL_TOK_KW_INOUT);
}

/*
 * **GLSL 1.20's `invariant` and `centroid`**, which sit *before* the storage qualifier rather
 * than in place of it - `invariant centroid varying vec3 v;` is one declaration with three
 * qualifiers on it.
 *
 * Both are consumed and neither is recorded, because **neither has anything to change here**,
 * and that is a statement about this implementation rather than a shortcut:
 *
 *   - `invariant` asks that a value computed the same way in two shaders come out bit-identical.
 *     There is one code path per stage and no optimiser reordering arithmetic between them, so
 *     the guarantee already holds for everything.
 *   - `centroid` moves a varying's sample point inside the primitive under multisampling. There
 *     is no multisample buffer - `GL_SAMPLE_BUFFERS` answers 0 - so every sample is already at
 *     the pixel centre, which is where centroid sampling would put it.
 *
 * Refused in a 1.10 shader by name, because they are 1.20's and a shader that uses one has said
 * which language it is written in.
 */
static void parse_aux_qualifiers(glsl_parser_t *p) {
    while (check(p, GLSL_TOK_KW_INVARIANT) || check(p, GLSL_TOK_KW_CENTROID)) {
        if (p->version != 0 && p->version < 120) {
            fail(p, "`invariant` and `centroid` are GLSL 1.20; this shader is 1.10");
            return;
        }
        bump(p);
    }
}

/* Is this token a built-in type name? */
static GLboolean is_type_name(glsl_token_type_t t) {
    switch (t) {
        case GLSL_TOK_KW_VOID: case GLSL_TOK_KW_FLOAT: case GLSL_TOK_KW_INT:
        case GLSL_TOK_KW_BOOL:
        case GLSL_TOK_KW_VEC2: case GLSL_TOK_KW_VEC3: case GLSL_TOK_KW_VEC4:
        case GLSL_TOK_KW_IVEC2: case GLSL_TOK_KW_IVEC3: case GLSL_TOK_KW_IVEC4:
        case GLSL_TOK_KW_BVEC2: case GLSL_TOK_KW_BVEC3: case GLSL_TOK_KW_BVEC4:
        case GLSL_TOK_KW_MAT2: case GLSL_TOK_KW_MAT3: case GLSL_TOK_KW_MAT4:
        case GLSL_TOK_KW_SAMPLER1D: case GLSL_TOK_KW_SAMPLER2D: case GLSL_TOK_KW_SAMPLER3D:
        case GLSL_TOK_KW_SAMPLERCUBE:
        case GLSL_TOK_KW_SAMPLER1DSHADOW: case GLSL_TOK_KW_SAMPLER2DSHADOW:
            return GL_TRUE;
        default:
            return GL_FALSE;
    }
}

/* **Does a declaration start here?**
 *
 * This is the one genuinely ambiguous decision in the grammar as written, and it is decided by
 * one token of lookahead: a qualifier or a built-in type name begins a declaration, anything
 * else begins an expression. A user-defined type name would need a symbol table to recognise -
 * C's famous ambiguity - and GLSL 1.10 has `struct`, so this will need revisiting when structs
 * are parsed. Written as its own function so that change has one place to happen.
 */
/* Two names, both pointing into the source rather than copied. */
static GLboolean same_text(const char *a, size_t an, const char *b, size_t bn) {
    if (an != bn) return GL_FALSE;
    for (size_t i = 0; i < an; i++) {
        if (a[i] != b[i]) return GL_FALSE;
    }
    return GL_TRUE;
}

/* **Does this identifier name a struct declared earlier in this unit?** The whole of why the
 * parser keeps a list of them: see `struct_name` on `glsl_parser_t`. */
static GLboolean is_struct_name(const glsl_parser_t *p, const char *t, size_t n) {
    for (int i = 0; i < p->struct_names; i++) {
        if (same_text(p->struct_name[i], p->struct_name_len[i], t, n)) return GL_TRUE;
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

static GLboolean starts_declaration(const glsl_parser_t *p) {
    if (is_qualifier(p->tok.type) || is_type_name(p->tok.type)) return GL_TRUE;
    /* `struct S { ... } s;` declares a type and maybe a variable, and either way begins one. */
    if (p->tok.type == GLSL_TOK_KW_STRUCT) return GL_TRUE;
    /* And the case that needed the list: `S s;` where `S` is a struct. */
    return (GLboolean)(p->tok.type == GLSL_TOK_IDENTIFIER &&
                       is_struct_name(p, p->tok.text, p->tok.length));
}

/* `[ expr ]` or `[ ]` after a name. Returns the size expression, or GLSL_NO_NODE for both "not
 * an array" and "an array with no size" - the caller knows which by whether a bracket was
 * consumed, and 1.10 only allows the unsized form on a function parameter. */
static int32_t parse_array_suffix(glsl_parser_t *p, GLboolean *saw_bracket) {
    *saw_bracket = GL_FALSE;
    if (!check(p, GLSL_TOK_LBRACKET)) return GLSL_NO_NODE;
    *saw_bracket = GL_TRUE;
    bump(p);
    int32_t size = GLSL_NO_NODE;
    if (!check(p, GLSL_TOK_RBRACKET)) {
        size = glsl_parse_expression(p);
        if (p->error) return GLSL_NO_NODE;
    }
    if (!accept(p, GLSL_TOK_RBRACKET)) {
        fail(p, "expected ']' after an array size");
        return GLSL_NO_NODE;
    }
    return size;
}

/* A declaration after its type has been read: one or more declarators, comma-separated.
 *
 * `float a, b = 1.0, c[4];` is three GLSL_NODE_DECLs chained through `sibling`, **each carrying
 * the shared type**. Storing the type once on the first and leaving the rest to look backwards
 * would work until something reordered or filtered the list.
 */
static int32_t parse_declarator_list(glsl_parser_t *p, glsl_token_type_t qualifier,
                                     glsl_token_type_t type_tok, const char *type_name,
                                     size_t type_name_len);

/*
 * `struct Name { members }` - the definition, without the declarators that may follow it.
 *
 * **The name is remembered before the body is read**, which is what lets a struct contain a
 * pointer-free reference to nothing at all and still refuse `struct S { S next; };` further on:
 * sema catches that, but the parser must already agree that `S` is a type name or the member
 * line would not parse as a declaration in the first place.
 *
 * GLSL 1.10 allows no qualifiers and no initialisers on a member, and both are refused here
 * rather than parsed and dropped - a member that silently loses its `= 1.0` is worse than one
 * that will not compile.
 */
static int32_t parse_struct_definition(glsl_parser_t *p) {
    bump(p); /* `struct` */
    if (!check(p, GLSL_TOK_IDENTIFIER)) {
        /* An anonymous struct is legal in C and not in GLSL 1.10, where the name is how a
         * variable of it is ever declared. */
        fail(p, "expected a name after `struct`");
        return GLSL_NO_NODE;
    }
    const char *name = p->tok.text;
    const size_t name_len = p->tok.length;
    const int line = p->tok.line, col = p->tok.column;
    if (!remember_struct_name(p, name, name_len)) return GLSL_NO_NODE;
    bump(p);

    int32_t at = node_new(p, GLSL_NODE_STRUCT_DEF);
    if (at == GLSL_NO_NODE) return at;
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
        } else if (check(p, GLSL_TOK_IDENTIFIER) && is_struct_name(p, p->tok.text, p->tok.length)) {
            mtok = GLSL_TOK_IDENTIFIER;
            mname = p->tok.text;
            mname_len = p->tok.length;
            bump(p);
        } else {
            fail(p, "expected a type in a struct member");
            return GLSL_NO_NODE;
        }
        int32_t m = parse_declarator_list(p, GLSL_TOK_EOF, mtok, mname, mname_len);
        if (p->error) return GLSL_NO_NODE;
        /* An initialiser on a member is not GLSL 1.10, and dropping one silently would lose
         * what its author wrote. */
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
        if (first == GLSL_NO_NODE) first = m; else p->ast->nodes[prev].sibling = m;
        prev = m;
        while (p->ast->nodes[prev].sibling != GLSL_NO_NODE) prev = p->ast->nodes[prev].sibling;
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
 * A type specifier at the head of a declaration: a built-in keyword, a `struct` definition, or
 * the name of a struct already declared. Writes what the declarators will carry, and returns the
 * definition node when there was one so the caller can chain it ahead of them.
 */
static int32_t parse_type_specifier(glsl_parser_t *p, glsl_token_type_t *type_tok,
                                    const char **type_name, size_t *type_name_len) {
    *type_name = (const char *)0;
    *type_name_len = 0;
    if (check(p, GLSL_TOK_KW_STRUCT)) {
        const int32_t def = parse_struct_definition(p);
        if (p->error) return GLSL_NO_NODE;
        *type_tok = GLSL_TOK_IDENTIFIER;
        *type_name = p->ast->nodes[def].text;
        *type_name_len = p->ast->nodes[def].length;
        return def;
    }
    if (check(p, GLSL_TOK_IDENTIFIER) && is_struct_name(p, p->tok.text, p->tok.length)) {
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
        if (at == GLSL_NO_NODE) return at;
        p->ast->nodes[at].text = p->tok.text;
        p->ast->nodes[at].length = p->tok.length;
        p->ast->nodes[at].qualifier = qualifier;
        p->ast->nodes[at].type_tok = type_tok;
        p->ast->nodes[at].type_name = type_name;
        p->ast->nodes[at].type_name_len = type_name_len;
        bump(p);

        GLboolean bracketed = GL_FALSE;
        int32_t size = parse_array_suffix(p, &bracketed);
        if (p->error) return GLSL_NO_NODE;
        p->ast->nodes[at].array_size = size;

        if (accept(p, GLSL_TOK_ASSIGN)) {
            /* The initialiser is an assignment-expression, not a full expression: a comma here
             * separates declarators. `float a = 1, b = 2;` is two declarations, and parsing the
             * initialiser with the comma operator would make it one. */
            int32_t init = parse_assignment(p);
            if (p->error) return GLSL_NO_NODE;
            p->ast->nodes[at].a = init;
        }

        if (first == GLSL_NO_NODE) first = at; else p->ast->nodes[prev].sibling = at;
        prev = at;

        if (!accept(p, GLSL_TOK_COMMA)) break;
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
    if (at == GLSL_NO_NODE) return at;

    int32_t first = GLSL_NO_NODE, prev = GLSL_NO_NODE;
    while (!check(p, GLSL_TOK_RBRACE)) {
        if (check(p, GLSL_TOK_EOF) || p->error) {
            fail(p, "expected '}' to close a block");
            return GLSL_NO_NODE;
        }
        int32_t s = glsl_parse_statement(p);
        if (p->error) return GLSL_NO_NODE;
        if (first == GLSL_NO_NODE) first = s; else p->ast->nodes[prev].sibling = s;
        /* **A declaration statement may already be a chain.** `float a, b;` returns two DECLs
         * linked through `sibling`, the same field this list uses, so the tail has to be found
         * rather than assumed to be the node just returned - otherwise the next statement
         * overwrites the second declarator and `b` vanishes. The translation unit has the same
         * hazard and the same fix. */
        prev = s;
        while (p->ast->nodes[prev].sibling != GLSL_NO_NODE) prev = p->ast->nodes[prev].sibling;
    }
    bump(p); /* '}' */
    p->ast->nodes[at].a = first;
    return at;
}

int32_t glsl_parse_statement(glsl_parser_t *p) {
    if (!p || p->error) return GLSL_NO_NODE;

    if (check(p, GLSL_TOK_LBRACE)) return parse_compound(p);

    if (starts_declaration(p)) {
        glsl_token_type_t qualifier = GLSL_TOK_EOF;
        if (is_qualifier(p->tok.type)) {
            qualifier = p->tok.type;
            bump(p);
        }
        glsl_token_type_t type_tok = GLSL_TOK_EOF;
        const char *type_name = (const char *)0;
        size_t type_name_len = 0;
        const int32_t def = parse_type_specifier(p, &type_tok, &type_name, &type_name_len);
        if (p->error) return GLSL_NO_NODE;

        /* **`struct S { ... };` with no declarator is a whole statement**, and a legal one - it
         * declares the type and nothing else. Only a name after the brace starts declarators. */
        if (def != GLSL_NO_NODE && check(p, GLSL_TOK_SEMICOLON)) {
            bump(p);
            return def;
        }
        int32_t decl = parse_declarator_list(p, qualifier, type_tok, type_name, type_name_len);
        if (p->error) return GLSL_NO_NODE;
        if (!accept(p, GLSL_TOK_SEMICOLON)) {
            fail(p, "expected ';' after a declaration");
            return GLSL_NO_NODE;
        }
        /* The definition comes first and the variables hang off it, so a walk in order sees the
         * type declared before anything is declared of it. */
        if (def != GLSL_NO_NODE) {
            p->ast->nodes[def].sibling = decl;
            return def;
        }
        return decl;
    }

    if (check(p, GLSL_TOK_KW_IF)) {
        int32_t at = node_new(p, GLSL_NODE_IF);
        if (at == GLSL_NO_NODE) return at;
        bump(p);
        if (!accept(p, GLSL_TOK_LPAREN)) { fail(p, "expected '(' after 'if'"); return GLSL_NO_NODE; }
        int32_t cond = glsl_parse_expression(p);
        if (!accept(p, GLSL_TOK_RPAREN)) { fail(p, "expected ')' after an if condition"); return GLSL_NO_NODE; }
        int32_t then_s = glsl_parse_statement(p);
        if (p->error) return GLSL_NO_NODE;
        int32_t else_s = GLSL_NO_NODE;
        /* **The dangling else binds to the nearest `if`**, and recursive descent gets that for
         * free: the inner `if` is parsed by the recursive call above and consumes the `else`
         * before this frame ever sees it.
         *
         * Worth saying plainly because it looks like a decision and is not one - it is
         * structural, and an attempt to mutate it here has nothing to bite on. A future rewrite
         * to a table-driven parser would have to choose deliberately, which is why the test
         * asserting the shape is kept even though nothing here can currently break it. */
        if (accept(p, GLSL_TOK_KW_ELSE)) {
            else_s = glsl_parse_statement(p);
            if (p->error) return GLSL_NO_NODE;
        }
        p->ast->nodes[at].a = cond;
        p->ast->nodes[at].b = then_s;
        p->ast->nodes[at].c = else_s;
        return at;
    }

    if (check(p, GLSL_TOK_KW_WHILE)) {
        int32_t at = node_new(p, GLSL_NODE_WHILE);
        if (at == GLSL_NO_NODE) return at;
        bump(p);
        if (!accept(p, GLSL_TOK_LPAREN)) { fail(p, "expected '(' after 'while'"); return GLSL_NO_NODE; }
        int32_t cond = glsl_parse_expression(p);
        if (!accept(p, GLSL_TOK_RPAREN)) { fail(p, "expected ')' after a while condition"); return GLSL_NO_NODE; }
        int32_t body = glsl_parse_statement(p);
        if (p->error) return GLSL_NO_NODE;
        p->ast->nodes[at].a = cond;
        p->ast->nodes[at].b = body;
        return at;
    }

    if (check(p, GLSL_TOK_KW_DO)) {
        int32_t at = node_new(p, GLSL_NODE_DO_WHILE);
        if (at == GLSL_NO_NODE) return at;
        bump(p);
        int32_t body = glsl_parse_statement(p);
        if (p->error) return GLSL_NO_NODE;
        if (!accept(p, GLSL_TOK_KW_WHILE)) { fail(p, "expected 'while' after a do body"); return GLSL_NO_NODE; }
        if (!accept(p, GLSL_TOK_LPAREN)) { fail(p, "expected '(' after 'while'"); return GLSL_NO_NODE; }
        int32_t cond = glsl_parse_expression(p);
        if (!accept(p, GLSL_TOK_RPAREN)) { fail(p, "expected ')' after a while condition"); return GLSL_NO_NODE; }
        if (!accept(p, GLSL_TOK_SEMICOLON)) { fail(p, "expected ';' after do-while"); return GLSL_NO_NODE; }
        p->ast->nodes[at].a = body;
        p->ast->nodes[at].b = cond;
        return at;
    }

    if (check(p, GLSL_TOK_KW_FOR)) {
        int32_t at = node_new(p, GLSL_NODE_FOR);
        if (at == GLSL_NO_NODE) return at;
        bump(p);
        if (!accept(p, GLSL_TOK_LPAREN)) { fail(p, "expected '(' after 'for'"); return GLSL_NO_NODE; }
        /* The init clause is a statement, so it can be a declaration - `for (int i = 0; ...)` -
         * and it consumes its own semicolon either way. */
        int32_t init = GLSL_NO_NODE;
        if (accept(p, GLSL_TOK_SEMICOLON)) {
            init = GLSL_NO_NODE;
        } else {
            init = glsl_parse_statement(p);
            if (p->error) return GLSL_NO_NODE;
        }
        int32_t cond = GLSL_NO_NODE;
        if (!check(p, GLSL_TOK_SEMICOLON)) {
            cond = glsl_parse_expression(p);
            if (p->error) return GLSL_NO_NODE;
        }
        if (!accept(p, GLSL_TOK_SEMICOLON)) { fail(p, "expected ';' in a for header"); return GLSL_NO_NODE; }
        int32_t step = GLSL_NO_NODE;
        if (!check(p, GLSL_TOK_RPAREN)) {
            step = glsl_parse_expression(p);
            if (p->error) return GLSL_NO_NODE;
        }
        if (!accept(p, GLSL_TOK_RPAREN)) { fail(p, "expected ')' after a for header"); return GLSL_NO_NODE; }
        int32_t body = glsl_parse_statement(p);
        if (p->error) return GLSL_NO_NODE;
        p->ast->nodes[at].a = init;
        p->ast->nodes[at].b = cond;
        p->ast->nodes[at].c = step;
        p->ast->nodes[at].d = body;
        return at;
    }

    if (check(p, GLSL_TOK_KW_RETURN)) {
        int32_t at = node_new(p, GLSL_NODE_RETURN);
        if (at == GLSL_NO_NODE) return at;
        bump(p);
        if (!check(p, GLSL_TOK_SEMICOLON)) {
            p->ast->nodes[at].a = glsl_parse_expression(p);
            if (p->error) return GLSL_NO_NODE;
        }
        if (!accept(p, GLSL_TOK_SEMICOLON)) { fail(p, "expected ';' after 'return'"); return GLSL_NO_NODE; }
        return at;
    }

    if (check(p, GLSL_TOK_KW_BREAK) || check(p, GLSL_TOK_KW_CONTINUE) ||
        check(p, GLSL_TOK_KW_DISCARD)) {
        glsl_node_kind_t kind = check(p, GLSL_TOK_KW_BREAK)    ? GLSL_NODE_BREAK
                              : check(p, GLSL_TOK_KW_CONTINUE) ? GLSL_NODE_CONTINUE
                                                               : GLSL_NODE_DISCARD;
        int32_t at = node_new(p, kind);
        if (at == GLSL_NO_NODE) return at;
        bump(p);
        if (!accept(p, GLSL_TOK_SEMICOLON)) { fail(p, "expected ';' after a jump"); return GLSL_NO_NODE; }
        return at;
    }

    /* Expression statement, including the empty one. */
    {
        int32_t at = node_new(p, GLSL_NODE_EXPR_STMT);
        if (at == GLSL_NO_NODE) return at;
        if (!check(p, GLSL_TOK_SEMICOLON)) {
            p->ast->nodes[at].a = glsl_parse_expression(p);
            if (p->error) return GLSL_NO_NODE;
        }
        if (!accept(p, GLSL_TOK_SEMICOLON)) { fail(p, "expected ';' after an expression"); return GLSL_NO_NODE; }
        return at;
    }
}

/* -------------------------------------------------------------------------
 * The translation unit
 * ------------------------------------------------------------------------- */

static int32_t parse_parameter_list(glsl_parser_t *p) {
    int32_t first = GLSL_NO_NODE, prev = GLSL_NO_NODE;
    if (check(p, GLSL_TOK_RPAREN)) return GLSL_NO_NODE;

    /* `void` alone means no parameters, and is not a parameter named nothing. */
    if (check(p, GLSL_TOK_KW_VOID)) {
        glsl_lexer_t save_lx = p->lx;
        glsl_token_t save_tok = p->tok;
        bump(p);
        if (check(p, GLSL_TOK_RPAREN)) return GLSL_NO_NODE;
        p->lx = save_lx;
        p->tok = save_tok;
    }

    for (;;) {
        glsl_token_type_t qualifier = GLSL_TOK_EOF;
        if (is_qualifier(p->tok.type)) {
            qualifier = p->tok.type;
            bump(p);
        }
        /* **A parameter may be a struct**, so the same three-way type specifier the declarations
         * use applies here. `struct S { ... } f(...)` - a definition in a parameter's type - is
         * not GLSL, and `parse_type_specifier` would accept one; a definition here would also
         * have nowhere to be chained. So it is refused by name rather than half-supported. */
        if (check(p, GLSL_TOK_KW_STRUCT)) {
            fail(p, "a struct may not be defined in a parameter list");
            return GLSL_NO_NODE;
        }
        glsl_token_type_t type_tok = GLSL_TOK_EOF;
        const char *type_name = (const char *)0;
        size_t type_name_len = 0;
        (void)parse_type_specifier(p, &type_tok, &type_name, &type_name_len);
        if (p->error) return GLSL_NO_NODE;

        int32_t at = node_new(p, GLSL_NODE_PARAM);
        if (at == GLSL_NO_NODE) return at;
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
            if (p->error) return GLSL_NO_NODE;
        }

        if (first == GLSL_NO_NODE) first = at; else p->ast->nodes[prev].sibling = at;
        prev = at;

        if (!accept(p, GLSL_TOK_COMMA)) break;
    }
    return first;
}

/* One external declaration: a function definition, a function prototype, or a variable
 * declaration. All three start with a type, so they are told apart by what follows the name -
 * `(` means a function, anything else a variable. */
static int32_t parse_external_declaration(glsl_parser_t *p) {
    /* **`invariant name;` on its own is a whole declaration** in GLSL 1.20 - a restatement that
     * a variable already declared, usually `gl_Position`, is invariant. It has no type and
     * declares nothing new, so it is consumed and produces no node. Taken before the qualifier
     * loop below, which expects a type to follow. */
    parse_aux_qualifiers(p);
    if (p->error) return GLSL_NO_NODE;
    /* An identifier where a type should be, after `invariant`, is the restatement form -
     * `invariant gl_Position;` - which declares nothing and produces no node.
     *
     * **Unless it names a struct**, in which case it is a type after all and `S s;` is an
     * ordinary declaration. Before structs existed every identifier here was a restatement, and
     * this branch was allowed to be that broad; now the two forms start with the same token and
     * the name list is what tells them apart. Without this check `S s;` is consumed as a
     * restatement of `S` and then fails on `s` where a `;` was expected. */
    if (check(p, GLSL_TOK_IDENTIFIER) && !is_struct_name(p, p->tok.text, p->tok.length)) {
        bump(p);
        if (accept(p, GLSL_TOK_SEMICOLON)) return GLSL_NO_NODE;
        fail(p, "expected `;` after an invariant restatement");
        return GLSL_NO_NODE;
    }

    glsl_token_type_t qualifier = GLSL_TOK_EOF;
    if (is_qualifier(p->tok.type)) {
        qualifier = p->tok.type;
        bump(p);
    }
    glsl_token_type_t type_tok = GLSL_TOK_EOF;
    const char *type_name = (const char *)0;
    size_t type_name_len = 0;
    const int32_t struct_def = parse_type_specifier(p, &type_tok, &type_name, &type_name_len);
    if (p->error) return GLSL_NO_NODE;

    /* A struct declared at file scope with no variable after it. */
    if (struct_def != GLSL_NO_NODE && accept(p, GLSL_TOK_SEMICOLON)) return struct_def;

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
        if (at == GLSL_NO_NODE) return at;
        p->ast->nodes[at].text = name;
        p->ast->nodes[at].length = name_len;
        p->ast->nodes[at].type_tok = type_tok;
        /* The return type, which may be a struct - `S bump(S v)`. Without this the function is
         * declared with an unresolvable return type and every call to it fails. */
        p->ast->nodes[at].type_name = type_name;
        p->ast->nodes[at].type_name_len = type_name_len;
        p->ast->nodes[at].qualifier = qualifier;
        p->ast->nodes[at].line = name_line;
        p->ast->nodes[at].column = name_col;
        p->ast->nodes[at].b = parse_parameter_list(p);
        if (p->error) return GLSL_NO_NODE;
        if (!accept(p, GLSL_TOK_RPAREN)) {
            fail(p, "expected ')' to close a parameter list");
            return GLSL_NO_NODE;
        }
        if (check(p, GLSL_TOK_LBRACE)) {
            p->ast->nodes[at].c = parse_compound(p);
            if (p->error) return GLSL_NO_NODE;
        } else if (!accept(p, GLSL_TOK_SEMICOLON)) {
            fail(p, "expected a body or ';' after a function header");
            return GLSL_NO_NODE;
        }
        /* `c` left absent means a prototype. An empty body is a COMPOUND node with no
         * statements, which is a different thing and reads differently. */
        return at;
    }

    /* A variable declaration. The name has already been consumed, so the first declarator is
     * built here and any further ones come from the shared list parser. */
    int32_t at = node_new(p, GLSL_NODE_DECL);
    if (at == GLSL_NO_NODE) return at;
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
    if (p->error) return GLSL_NO_NODE;
    if (accept(p, GLSL_TOK_ASSIGN)) {
        p->ast->nodes[at].a = parse_assignment(p);
        if (p->error) return GLSL_NO_NODE;
    }
    if (accept(p, GLSL_TOK_COMMA)) {
        int32_t rest = parse_declarator_list(p, qualifier, type_tok, type_name, type_name_len);
        if (p->error) return GLSL_NO_NODE;
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
    if (!p) return GLSL_NO_NODE;
    int32_t at = node_new(p, GLSL_NODE_UNIT);
    if (at == GLSL_NO_NODE) return at;

    int32_t first = GLSL_NO_NODE, prev = GLSL_NO_NODE;
    while (!check(p, GLSL_TOK_EOF)) {
        if (p->error) return GLSL_NO_NODE;
        /* The preprocessor is a separate stage and is not written. A `#` here is refused by
         * name rather than skipped, because skipping it would silently ignore a `#version` or
         * a `#ifdef` and compile something the program did not write. */
        if (check(p, GLSL_TOK_HASH)) {
            fail(p, "preprocessor directives are not handled yet");
            return GLSL_NO_NODE;
        }
        int32_t d = parse_external_declaration(p);
        if (p->error) return GLSL_NO_NODE;
        /* **No node and no error is a declaration that declares nothing** - GLSL 1.20's
         * `invariant gl_Position;`. It is skipped rather than chained, because a
         * GLSL_NO_NODE in the chain would end the list early and lose everything after it. */
        if (d == GLSL_NO_NODE) continue;
        if (first == GLSL_NO_NODE) first = d; else p->ast->nodes[prev].sibling = d;
        /* A declarator list chains through `sibling` too, so the tail has to be found rather
         * than assumed to be the node just returned. */
        prev = d;
        while (p->ast->nodes[prev].sibling != GLSL_NO_NODE) prev = p->ast->nodes[prev].sibling;
    }
    p->ast->nodes[at].a = first;
    return at;
}
