/*
 * oops-gl: the GLSL preprocessor
 *
 * Sits between the lexer and the parser as a token filter. The parser asks for a token;
 * this obeys any directives in the way, expands any macro it finds, and hands back the
 * next real one.
 *
 * Inside a false conditional the tokens are discarded but the conditional directives
 * are still read, so nesting pairs correctly.
 *
 * A macro does not expand within its own expansion: `#define A A` leaves an `A`. The
 * guard is a flag per macro, set when its body is queued and cleared by an end marker
 * queued behind the body (see `expand`), so mutual recursion terminates too.
 */

#include "glsl_internal.h"

static void pp_fail(glsl_pp_t *pp, const char *why, int line) {
    if (pp->error)
        return;
    pp->error = why;
    pp->error_line = line;
}

static GLboolean same_word(const char *a, size_t an, const char *b, size_t bn) {
    if (an != bn)
        return GL_FALSE;
    for (size_t i = 0; i < an; i++) {
        if (a[i] != b[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

/* Matches a token's text against a literal - used for directive names, which the lexer
 * hands back as ordinary identifiers or keywords. */
static GLboolean tok_is(const glsl_token_t *t, const char *word) {
    size_t n = 0;
    while (word[n] != '\0')
        n++;
    return same_word(t->text, t->length, word, n);
}

static glsl_macro_t *find_macro(glsl_pp_t *pp, const char *name, size_t len) {
    for (int i = 0; i < pp->macro_count; i++) {
        if (pp->macros[i].in_use &&
            same_word(pp->macros[i].name, pp->macros[i].name_len, name, len)) {
            return &pp->macros[i];
        }
    }
    return (glsl_macro_t *)0;
}

void glsl_pp_init(glsl_pp_t *pp, const char *source, size_t length) {
    if (!pp)
        return;
    glsl_lexer_init(&pp->lx, source, length);
    pp->macro_count = 0;
    pp->pool_count = 0;
    pp->pending_head = pp->pending_tail = 0;
    pp->has_held = GL_FALSE;
    pp->cond_depth = 0;
    pp->version = 0;
    pp->error = (const char *)0;
    pp->error_line = 0;
    for (int i = 0; i < GLSL_MAX_MACROS; i++) {
        pp->macros[i].in_use = GL_FALSE;
        pp->macros[i].expanding = GL_FALSE;
    }
}

/* Whether output is live: every open conditional must be emitting. */
static GLboolean emitting(const glsl_pp_t *pp) {
    for (int i = 0; i < pp->cond_depth; i++) {
        if (!pp->emitting[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

/* Reads the raw next token, which is the lexer's unless expansion has queued some. */
static GLboolean raw_next(glsl_pp_t *pp, glsl_token_t *out) {
    if (pp->has_held) {
        *out = pp->held;
        pp->has_held = GL_FALSE;
        return (GLboolean)(out->type != GLSL_TOK_EOF);
    }
    while (pp->pending_head < pp->pending_tail) {
        *out = pp->pending[pp->pending_head++];
        if (pp->pending_head == pp->pending_tail) {
            pp->pending_head = pp->pending_tail = 0;
        }
        /* An end marker is where one macro's expansion stops; reading it clears that
         * macro's flag (see `expand`). It is swallowed here. */
        if (out->type == GLSL_TOK_PP_MACRO_END) {
            const int mi = (int)out->value;
            if (mi >= 0 && mi < pp->macro_count)
                pp->macros[mi].expanding = GL_FALSE;
            continue;
        }
        return GL_TRUE;
    }
    /* Going back to the lexer means every expansion is over, including an empty
     * macro's, which queues no marker. Clearing as the queue empties would be one read
     * too early and `#define A A` would loop. */
    for (int i = 0; i < pp->macro_count; i++)
        pp->macros[i].expanding = GL_FALSE;
    return glsl_lex_next(&pp->lx, out);
}

/* Consumes the rest of the directive line. The lexer does not produce newline tokens,
 * so a directive's end is found by watching the line number change - which is why every
 * token carries one. */
static void skip_to_end_of_line(glsl_pp_t *pp, glsl_token_t *tok, int line) {
    while (tok->type != GLSL_TOK_EOF && tok->line == line) {
        if (!raw_next(pp, tok))
            return;
    }
}

/* `#define name tokens...` - object-like only. */
static void do_define(glsl_pp_t *pp, glsl_token_t *tok, int line) {
    if (!raw_next(pp, tok) || tok->line != line) {
        pp_fail(pp, "#define with no name", line);
        return;
    }
    if (tok->type != GLSL_TOK_IDENTIFIER) {
        pp_fail(pp, "#define of something that is not a name", line);
        return;
    }
    const char *name = tok->text;
    size_t name_len = tok->length;

    glsl_macro_t *existing = find_macro(pp, name, name_len);
    glsl_macro_t *m = existing;
    if (!m) {
        if (pp->macro_count >= GLSL_MAX_MACROS) {
            pp_fail(pp, "too many macros", line);
            return;
        }
        m = &pp->macros[pp->macro_count++];
    }
    m->name = name;
    m->name_len = name_len;
    m->in_use = GL_TRUE;
    m->expanding = GL_FALSE;
    m->first_token = pp->pool_count;
    m->token_count = 0;
    m->function_like = GL_FALSE;
    m->param_count = 0;

    GLboolean have = raw_next(pp, tok);
    /* A `(` straight after the name makes it function-like: `#define F(x) x` takes an
     * argument, `#define F (x)` is object-like with a body that begins with a
     * parenthesis. */
    if (have && tok->line == line && tok->type == GLSL_TOK_LPAREN &&
        tok->text == name + name_len) {
        m->function_like = GL_TRUE;
        have = raw_next(pp, tok);
        if (have && tok->line == line && tok->type == GLSL_TOK_RPAREN) {
            have = raw_next(pp, tok); /* `#define F() ...` - zero parameters */
        } else {
            for (;;) {
                if (!have || tok->line != line || tok->type != GLSL_TOK_IDENTIFIER) {
                    pp_fail(pp, "#define parameter list wants a name", line);
                    return;
                }
                if (m->param_count >= GLSL_MAX_MACRO_PARAMS) {
                    pp_fail(pp, "too many macro parameters", line);
                    return;
                }
                /* A parameter named twice would make substitution ambiguous. */
                for (int i = 0; i < m->param_count; i++) {
                    if (same_word(m->param_name[i], m->param_len[i], tok->text,
                                  tok->length)) {
                        pp_fail(pp, "#define names the same parameter twice", line);
                        return;
                    }
                }
                m->param_name[m->param_count] = tok->text;
                m->param_len[m->param_count] = tok->length;
                m->param_count++;
                have = raw_next(pp, tok);
                if (have && tok->line == line && tok->type == GLSL_TOK_COMMA) {
                    have = raw_next(pp, tok);
                    continue;
                }
                if (have && tok->line == line && tok->type == GLSL_TOK_RPAREN) {
                    have = raw_next(pp, tok);
                    break;
                }
                pp_fail(pp, "#define parameter list wants `,` or `)`", line);
                return;
            }
        }
    }
    while (have && tok->line == line && tok->type != GLSL_TOK_EOF) {
        if (pp->pool_count >= GLSL_MAX_MACRO_TOKENS) {
            pp_fail(pp, "macro bodies too large", line);
            return;
        }
        pp->pool[pp->pool_count++] = *tok;
        m->token_count++;
        have = raw_next(pp, tok);
    }
    if (!have)
        tok->type = GLSL_TOK_EOF;
}

static void do_undef(glsl_pp_t *pp, glsl_token_t *tok, int line) {
    if (!raw_next(pp, tok) || tok->line != line || tok->type != GLSL_TOK_IDENTIFIER) {
        pp_fail(pp, "#undef with no name", line);
        return;
    }
    glsl_macro_t *m = find_macro(pp, tok->text, tok->length);
    if (m)
        m->in_use = GL_FALSE;
    /* `#undef` of something never defined is not an error. */
    if (!raw_next(pp, tok))
        tok->type = GLSL_TOK_EOF;
    skip_to_end_of_line(pp, tok, line);
}

static void do_version(glsl_pp_t *pp, glsl_token_t *tok, int line) {
    if (!raw_next(pp, tok) || tok->line != line || tok->type != GLSL_TOK_INTCONST) {
        pp_fail(pp, "#version with no number", line);
        return;
    }
    pp->version = (int)tok->value;
    if (!raw_next(pp, tok))
        tok->type = GLSL_TOK_EOF;
    skip_to_end_of_line(pp, tok, line);
}

/* -------------------------------------------------------------------------
 * `#if` and `#elif`: a constant expression over integers
 *
 * The grammar is C's, as the specification says, minus the parts GLSL 1.10 has no
 * tokens for (see `pp_eval_shift_guard`). Three passes over the directive's line:
 *
 *   1. `defined X` and `defined(X)` become 1 or 0, before expansion, so `defined FOO`
 *      asks about FOO itself.
 *   2. Macros expand. An identifier left over is 0, as the specification requires.
 *   3. The result is evaluated by precedence.
 *
 * All of it happens in fixed arrays. An expression longer than `GLSL_PP_MAX_EXPR`
 * tokens is refused rather than truncated.
 * ------------------------------------------------------------------------- */

#define GLSL_PP_MAX_EXPR 128
#define GLSL_PP_MAX_EXPANSIONS 256

/* Declared here and defined below: a `#if` expression may call a function-like macro,
 * and one substitution serves both it and the token stream. */
#define GLSL_PP_MAX_ARG_TOKENS 128

typedef struct pp_args {
    glsl_token_t tok[GLSL_PP_MAX_ARG_TOKENS];
    int start[GLSL_MAX_MACRO_PARAMS];
    int len[GLSL_MAX_MACRO_PARAMS];
    int count; /* arguments actually supplied */
    int used;  /* tokens in `tok` */
} pp_args_t;

static GLboolean pp_args_begin(pp_args_t *a);
static GLboolean pp_args_open(glsl_pp_t *pp, pp_args_t *a, int line);
static GLboolean pp_args_push(glsl_pp_t *pp, pp_args_t *a, const glsl_token_t *t,
                              int line);
static GLboolean pp_args_check(glsl_pp_t *pp, const glsl_macro_t *m, const pp_args_t *a,
                               int line);
static int pp_subst(glsl_pp_t *pp, const glsl_macro_t *m, const pp_args_t *a,
                    glsl_token_t *out, int max, int line);

/* Collects what is left of a directive's line. `tok` holds the directive name on entry
 * and the first token of the next line on exit, as with every `do_*` here. */
static int pp_collect_line(glsl_pp_t *pp, glsl_token_t *tok, int line,
                           glsl_token_t *buf, int max) {
    int n = 0;
    GLboolean have = raw_next(pp, tok);
    while (have && tok->line == line && tok->type != GLSL_TOK_EOF) {
        if (n >= max) {
            pp_fail(pp, "preprocessor expression too long", line);
            return -1;
        }
        buf[n++] = *tok;
        have = raw_next(pp, tok);
    }
    if (!have)
        tok->type = GLSL_TOK_EOF;
    return n;
}

/* Makes an integer token out of thin air. `text`/`length` are left pointing at the
 * token it replaces, so a diagnostic still names something that exists in the source.
 */
static glsl_token_t pp_int_token(const glsl_token_t *like, int32_t v) {
    glsl_token_t t = *like;
    t.type = GLSL_TOK_INTCONST;
    t.value = (double)v;
    return t;
}

/* Pass 1: `defined X` and `defined ( X )`. */
static int pp_resolve_defined(glsl_pp_t *pp, const glsl_token_t *in, int n,
                              glsl_token_t *out, int line) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!(in[i].type == GLSL_TOK_IDENTIFIER &&
              same_word(in[i].text, in[i].length, "defined", 7))) {
            out[m++] = in[i];
            continue;
        }
        int j = i + 1;
        GLboolean paren = GL_FALSE;
        if (j < n && in[j].type == GLSL_TOK_LPAREN) {
            paren = GL_TRUE;
            j++;
        }
        if (j >= n || in[j].type != GLSL_TOK_IDENTIFIER) {
            pp_fail(pp, "`defined` without a name", line);
            return -1;
        }
        const GLboolean d =
            (GLboolean)(find_macro(pp, in[j].text, in[j].length) != (glsl_macro_t *)0);
        glsl_token_t v = pp_int_token(&in[j], d ? 1 : 0);
        j++;
        if (paren) {
            if (j >= n || in[j].type != GLSL_TOK_RPAREN) {
                pp_fail(pp, "`defined(` without `)`", line);
                return -1;
            }
            j++;
        }
        out[m++] = v;
        i = j - 1;
    }
    return m;
}

/* Pass 2: expand macros. Bounded rather than recursive - a macro that expands to itself
 * stops at the budget with a named failure instead of running out of stack. */
static int pp_expand_expr(glsl_pp_t *pp, glsl_token_t *buf, int n, int line) {
    int budget = GLSL_PP_MAX_EXPANSIONS;
    for (int i = 0; i < n; i++) {
        if (buf[i].type != GLSL_TOK_IDENTIFIER)
            continue;
        glsl_macro_t *m = find_macro(pp, buf[i].text, buf[i].length);
        if (!m)
            continue;
        if (--budget < 0) {
            pp_fail(pp,
                    "macro expansion in a preprocessor expression does not terminate",
                    line);
            return -1;
        }

        /* How much of `buf` this call occupies, and what it turns into. For an
         * object-like macro that is the name and the body; for a function-like one it
         * is the whole `F(a, b)` and the body with its arguments substituted. */
        glsl_token_t body[GLSL_PP_MAX_EXPR];
        int consumed = 1, produced;
        if (m->function_like) {
            /* A function-like macro's name with no `(` after it is not a call; it stays
             * an identifier, which the evaluator reads as 0. */
            if (i + 1 >= n || buf[i + 1].type != GLSL_TOK_LPAREN)
                continue;
            pp_args_t args;
            pp_args_begin(&args);
            if (!pp_args_open(pp, &args, line))
                return -1;
            int depth = 0, j = i + 2;
            for (;; j++) {
                if (j >= n) {
                    pp_fail(pp, "macro call without a closing `)`", line);
                    return -1;
                }
                if (buf[j].type == GLSL_TOK_LPAREN)
                    depth++;
                if (buf[j].type == GLSL_TOK_RPAREN) {
                    if (depth == 0)
                        break;
                    depth--;
                }
                if (buf[j].type == GLSL_TOK_COMMA && depth == 0) {
                    if (!pp_args_open(pp, &args, line))
                        return -1;
                    continue;
                }
                if (!pp_args_push(pp, &args, &buf[j], line))
                    return -1;
                args.len[args.count - 1]++;
            }
            if (!pp_args_check(pp, m, &args, line))
                return -1;
            consumed = j - i + 1;
            produced = pp_subst(pp, m, &args, body, GLSL_PP_MAX_EXPR, line);
            if (produced < 0)
                return -1;
        } else {
            produced = (int)m->token_count;
            for (int k = 0; k < produced; k++)
                body[k] = pp->pool[m->first_token + k];
        }

        const int grow = produced - consumed;
        if (n + grow > GLSL_PP_MAX_EXPR) {
            pp_fail(pp, "preprocessor expression too long", line);
            return -1;
        }
        if (grow > 0) {
            for (int k = n - 1; k >= i + consumed; k--)
                buf[k + grow] = buf[k];
        } else if (grow < 0) {
            for (int k = i + consumed; k < n; k++)
                buf[k + grow] = buf[k];
        }
        for (int k = 0; k < produced; k++)
            buf[i + k] = body[k];
        n += grow;
        i--; /* re-examine from here: the body may itself start with a macro */
    }
    return n;
}

typedef struct {
    glsl_pp_t *pp;
    const glsl_token_t *t;
    int n, i, line;
    GLboolean bad;
} pp_eval_t;

static int32_t pp_eval_ternary(pp_eval_t *e);

static glsl_token_type_t pp_peek(const pp_eval_t *e) {
    return e->i < e->n ? e->t[e->i].type : GLSL_TOK_EOF;
}

/* `<<` and `>>` are refused rather than mis-read. GLSL 1.10 has no shift operators, so
 * `1 << 2` arrives as two adjacent `<` tokens; they are detected by adjacency in the
 * source text and named. */
static GLboolean pp_eval_shift_guard(pp_eval_t *e) {
    if (e->i + 1 >= e->n)
        return GL_FALSE;
    const glsl_token_t *a = &e->t[e->i], *b = &e->t[e->i + 1];
    const GLboolean both_lt =
        (GLboolean)(a->type == GLSL_TOK_LT && b->type == GLSL_TOK_LT);
    const GLboolean both_gt =
        (GLboolean)(a->type == GLSL_TOK_GT && b->type == GLSL_TOK_GT);
    if ((both_lt || both_gt) && b->text == a->text + 1) {
        pp_fail(e->pp, "<< and >> are not preprocessor operators this front end has",
                e->line);
        e->bad = GL_TRUE;
        return GL_TRUE;
    }
    return GL_FALSE;
}

static int32_t pp_eval_primary(pp_eval_t *e) {
    if (e->bad)
        return 0;
    if (e->i >= e->n) {
        pp_fail(e->pp, "preprocessor expression ends early", e->line);
        e->bad = GL_TRUE;
        return 0;
    }
    const glsl_token_t *t = &e->t[e->i];
    switch (t->type) {
    case GLSL_TOK_INTCONST:
        e->i++;
        return (int32_t)t->value;
    case GLSL_TOK_KW_TRUE:
        e->i++;
        return 1;
    case GLSL_TOK_KW_FALSE:
        e->i++;
        return 0;
    case GLSL_TOK_LPAREN: {
        e->i++;
        const int32_t v = pp_eval_ternary(e);
        if (!e->bad && pp_peek(e) != GLSL_TOK_RPAREN) {
            pp_fail(e->pp, "preprocessor expression missing `)`", e->line);
            e->bad = GL_TRUE;
        } else if (!e->bad) {
            e->i++;
        }
        return v;
    }
    case GLSL_TOK_IDENTIFIER:
        /* `__VERSION__` is predefined, as the specification requires. */
        if (same_word(t->text, t->length, "__VERSION__", 11)) {
            e->i++;
            return (int32_t)(e->pp->version ? e->pp->version : 110);
        }
        /* Anything else still an identifier here is a macro that was never defined, and
         * the specification says that is 0. */
        e->i++;
        return 0;
    case GLSL_TOK_FLOATCONST:
        pp_fail(e->pp, "a preprocessor expression is integer only", e->line);
        e->bad = GL_TRUE;
        return 0;
    default:
        pp_fail(e->pp, "this is not allowed in a preprocessor expression", e->line);
        e->bad = GL_TRUE;
        return 0;
    }
}

static int32_t pp_eval_unary(pp_eval_t *e) {
    if (e->bad)
        return 0;
    switch (pp_peek(e)) {
    case GLSL_TOK_PLUS:
        e->i++;
        return pp_eval_unary(e);
    case GLSL_TOK_MINUS:
        e->i++;
        return -pp_eval_unary(e);
    case GLSL_TOK_BANG:
        e->i++;
        return pp_eval_unary(e) ? 0 : 1;
    case GLSL_TOK_TILDE:
        e->i++;
        return ~pp_eval_unary(e);
    default:
        return pp_eval_primary(e);
    }
}

static int32_t pp_eval_mul(pp_eval_t *e) {
    int32_t v = pp_eval_unary(e);
    for (;;) {
        if (e->bad)
            return 0;
        const glsl_token_type_t op = pp_peek(e);
        if (op != GLSL_TOK_STAR && op != GLSL_TOK_SLASH && op != GLSL_TOK_PERCENT)
            return v;
        e->i++;
        const int32_t r = pp_eval_unary(e);
        if (e->bad)
            return 0;
        if ((op == GLSL_TOK_SLASH || op == GLSL_TOK_PERCENT) && r == 0) {
            pp_fail(e->pp, "division by zero in a preprocessor expression", e->line);
            e->bad = GL_TRUE;
            return 0;
        }
        v = op == GLSL_TOK_STAR ? v * r : op == GLSL_TOK_SLASH ? v / r : v % r;
    }
}

static int32_t pp_eval_add(pp_eval_t *e) {
    int32_t v = pp_eval_mul(e);
    for (;;) {
        if (e->bad)
            return 0;
        const glsl_token_type_t op = pp_peek(e);
        if (op != GLSL_TOK_PLUS && op != GLSL_TOK_MINUS)
            return v;
        e->i++;
        const int32_t r = pp_eval_mul(e);
        v = op == GLSL_TOK_PLUS ? v + r : v - r;
    }
}

static int32_t pp_eval_rel(pp_eval_t *e) {
    int32_t v = pp_eval_add(e);
    for (;;) {
        if (e->bad || pp_eval_shift_guard(e))
            return 0;
        const glsl_token_type_t op = pp_peek(e);
        if (op != GLSL_TOK_LT && op != GLSL_TOK_GT && op != GLSL_TOK_LE &&
            op != GLSL_TOK_GE) {
            return v;
        }
        e->i++;
        const int32_t r = pp_eval_add(e);
        v = op == GLSL_TOK_LT   ? (v < r)
            : op == GLSL_TOK_GT ? (v > r)
            : op == GLSL_TOK_LE ? (v <= r)
                                : (v >= r);
    }
}

static int32_t pp_eval_eq(pp_eval_t *e) {
    int32_t v = pp_eval_rel(e);
    for (;;) {
        if (e->bad)
            return 0;
        const glsl_token_type_t op = pp_peek(e);
        if (op != GLSL_TOK_EQ && op != GLSL_TOK_NE)
            return v;
        e->i++;
        const int32_t r = pp_eval_rel(e);
        v = op == GLSL_TOK_EQ ? (v == r) : (v != r);
    }
}

static int32_t pp_eval_band(pp_eval_t *e) {
    int32_t v = pp_eval_eq(e);
    while (!e->bad && pp_peek(e) == GLSL_TOK_AMP) {
        e->i++;
        v &= pp_eval_eq(e);
    }
    return e->bad ? 0 : v;
}

static int32_t pp_eval_bxor(pp_eval_t *e) {
    int32_t v = pp_eval_band(e);
    while (!e->bad && pp_peek(e) == GLSL_TOK_CARET) {
        e->i++;
        v ^= pp_eval_band(e);
    }
    return e->bad ? 0 : v;
}

static int32_t pp_eval_bor(pp_eval_t *e) {
    int32_t v = pp_eval_bxor(e);
    while (!e->bad && pp_peek(e) == GLSL_TOK_PIPE) {
        e->i++;
        v |= pp_eval_bxor(e);
    }
    return e->bad ? 0 : v;
}

/* Both sides are evaluated. A preprocessor expression has no side effects, and
 * evaluating the right of a false `&&` still reports a malformed operand there. */
static int32_t pp_eval_land(pp_eval_t *e) {
    int32_t v = pp_eval_bor(e);
    while (!e->bad && pp_peek(e) == GLSL_TOK_AND_AND) {
        e->i++;
        const int32_t r = pp_eval_bor(e);
        v = (v && r);
    }
    return e->bad ? 0 : v;
}

static int32_t pp_eval_lor(pp_eval_t *e) {
    int32_t v = pp_eval_land(e);
    while (!e->bad && pp_peek(e) == GLSL_TOK_OR_OR) {
        e->i++;
        const int32_t r = pp_eval_land(e);
        v = (v || r);
    }
    return e->bad ? 0 : v;
}

static int32_t pp_eval_ternary(pp_eval_t *e) {
    const int32_t c = pp_eval_lor(e);
    if (e->bad || pp_peek(e) != GLSL_TOK_QUESTION)
        return c;
    e->i++;
    const int32_t a = pp_eval_ternary(e);
    if (!e->bad && pp_peek(e) != GLSL_TOK_COLON) {
        pp_fail(e->pp, "`?` without `:` in a preprocessor expression", e->line);
        e->bad = GL_TRUE;
        return 0;
    }
    if (e->bad)
        return 0;
    e->i++;
    const int32_t b = pp_eval_ternary(e);
    return c ? a : b;
}

/* The whole of it: collect the line, resolve `defined`, expand, evaluate. Returns the
 * value, or 0 with `pp->error` set. */
static int32_t pp_eval_condition(glsl_pp_t *pp, glsl_token_t *tok, int line) {
    glsl_token_t raw[GLSL_PP_MAX_EXPR], work[GLSL_PP_MAX_EXPR];
    const int n = pp_collect_line(pp, tok, line, raw, GLSL_PP_MAX_EXPR);
    if (n < 0)
        return 0;
    if (n == 0) {
        pp_fail(pp, "#if with no expression", line);
        return 0;
    }
    int m = pp_resolve_defined(pp, raw, n, work, line);
    if (m < 0)
        return 0;
    m = pp_expand_expr(pp, work, m, line);
    if (m < 0)
        return 0;

    pp_eval_t e;
    e.pp = pp;
    e.t = work;
    e.n = m;
    e.i = 0;
    e.line = line;
    e.bad = GL_FALSE;
    const int32_t v = pp_eval_ternary(&e);
    if (e.bad)
        return 0;
    if (e.i != m) {
        pp_fail(pp, "trailing rubbish in a preprocessor expression", line);
        return 0;
    }
    return v;
}

/* Reads one directive. `tok` holds the token after `#` on entry and the first token of
 * the next line on exit. */
static void do_directive(glsl_pp_t *pp, glsl_token_t *tok) {
    const int line = tok->line;

    /* A `#` on a line of its own is the null directive and is legal. */
    if (tok->type == GLSL_TOK_EOF || tok->line != line)
        return;

    /* Conditionals are read even while skipping, so nesting stays balanced; everything
     * else is obeyed only when output is live. An `#if` expression in a dark region is
     * not evaluated, since it may divide by zero or name a macro only the other arm
     * defines. */
    if (tok_is(tok, "if")) {
        if (pp->cond_depth >= GLSL_MAX_COND_DEPTH) {
            pp_fail(pp, "conditionals nested too deeply", line);
            return;
        }
        const GLboolean live = emitting(pp);
        GLboolean on = GL_FALSE;
        if (live) {
            on = (GLboolean)(pp_eval_condition(pp, tok, line) != 0);
            if (pp->error)
                return;
        } else {
            skip_to_end_of_line(pp, tok, line);
            if (!raw_next(pp, tok))
                tok->type = GLSL_TOK_EOF;
        }
        pp->emitting[pp->cond_depth] = (GLboolean)(live && on);
        pp->taken[pp->cond_depth] = (GLboolean)(live && on);
        pp->cond_depth++;
        if (live)
            skip_to_end_of_line(pp, tok, line);
        return;
    }

    if (tok_is(tok, "elif")) {
        if (pp->cond_depth == 0) {
            pp_fail(pp, "#elif with no #if", line);
            return;
        }
        const int top = pp->cond_depth - 1;
        GLboolean outer_live = GL_TRUE;
        for (int i = 0; i < top; i++) {
            if (!pp->emitting[i])
                outer_live = GL_FALSE;
        }
        /* Only evaluated when the enclosing region is live and no earlier arm of this
         * conditional has been taken. */
        const GLboolean could = (GLboolean)(outer_live && !pp->taken[top]);
        GLboolean on = GL_FALSE;
        if (could) {
            on = (GLboolean)(pp_eval_condition(pp, tok, line) != 0);
            if (pp->error)
                return;
        } else {
            skip_to_end_of_line(pp, tok, line);
            if (!raw_next(pp, tok))
                tok->type = GLSL_TOK_EOF;
        }
        pp->emitting[top] = (GLboolean)(could && on);
        if (pp->emitting[top])
            pp->taken[top] = GL_TRUE;
        if (could)
            skip_to_end_of_line(pp, tok, line);
        return;
    }

    if (tok_is(tok, "ifdef") || tok_is(tok, "ifndef")) {
        GLboolean want_defined = tok_is(tok, "ifdef");
        if (pp->cond_depth >= GLSL_MAX_COND_DEPTH) {
            pp_fail(pp, "conditionals nested too deeply", line);
            return;
        }
        GLboolean live = emitting(pp);
        if (!raw_next(pp, tok) || tok->line != line ||
            tok->type != GLSL_TOK_IDENTIFIER) {
            pp_fail(pp, "#ifdef with no name", line);
            return;
        }
        GLboolean defined =
            (GLboolean)(find_macro(pp, tok->text, tok->length) != (glsl_macro_t *)0);
        GLboolean on = (GLboolean)(defined == want_defined);
        /* A branch inside a dark region stays dark however its condition reads. */
        pp->emitting[pp->cond_depth] = (GLboolean)(live && on);
        pp->taken[pp->cond_depth] = (GLboolean)(live && on);
        pp->cond_depth++;
        if (!raw_next(pp, tok))
            tok->type = GLSL_TOK_EOF;
        skip_to_end_of_line(pp, tok, line);
        return;
    }

    if (tok_is(tok, "else")) {
        if (pp->cond_depth == 0) {
            pp_fail(pp, "#else with no #ifdef", line);
            return;
        }
        int top = pp->cond_depth - 1;
        /* Live only if no branch here has been taken and the enclosing region is
         * live. */
        GLboolean outer_live = GL_TRUE;
        for (int i = 0; i < top; i++) {
            if (!pp->emitting[i])
                outer_live = GL_FALSE;
        }
        pp->emitting[top] = (GLboolean)(outer_live && !pp->taken[top]);
        if (pp->emitting[top])
            pp->taken[top] = GL_TRUE;
        if (!raw_next(pp, tok))
            tok->type = GLSL_TOK_EOF;
        skip_to_end_of_line(pp, tok, line);
        return;
    }

    if (tok_is(tok, "endif")) {
        if (pp->cond_depth == 0) {
            pp_fail(pp, "#endif with no #ifdef", line);
            return;
        }
        pp->cond_depth--;
        if (!raw_next(pp, tok))
            tok->type = GLSL_TOK_EOF;
        skip_to_end_of_line(pp, tok, line);
        return;
    }

    if (!emitting(pp)) {
        /* Any other directive inside a dark region is ignored entirely, including ones
         * this would otherwise refuse. */
        skip_to_end_of_line(pp, tok, line);
        return;
    }

    if (tok_is(tok, "define")) {
        do_define(pp, tok, line);
        return;
    }
    if (tok_is(tok, "undef")) {
        do_undef(pp, tok, line);
        return;
    }
    if (tok_is(tok, "version")) {
        do_version(pp, tok, line);
        return;
    }
    if (tok_is(tok, "error")) {
        pp_fail(pp, "#error directive in the shader source", line);
        return;
    }

    /* `#extension`. This front end implements no extensions. The specification makes
     * `require` on an unsupported extension an error and `enable` a warning that
     * compiles on; `warn` and `disable` ask for nothing. So `require` is refused and
     * the rest are accepted and ignored. */
    if (tok_is(tok, "extension")) {
        glsl_token_t name = *tok;
        if (!raw_next(pp, tok) || tok->line != line ||
            tok->type != GLSL_TOK_IDENTIFIER) {
            pp_fail(pp, "#extension with no extension name", line);
            return;
        }
        name = *tok;
        (void)name;
        if (!raw_next(pp, tok) || tok->line != line || tok->type != GLSL_TOK_COLON) {
            pp_fail(pp, "#extension without `: behaviour`", line);
            return;
        }
        if (!raw_next(pp, tok) || tok->line != line ||
            tok->type != GLSL_TOK_IDENTIFIER) {
            pp_fail(pp, "#extension without a behaviour", line);
            return;
        }
        if (same_word(tok->text, tok->length, "require", 7)) {
            pp_fail(pp, "#extension ... : require - this front end has no extensions",
                    line);
            return;
        }
        if (!same_word(tok->text, tok->length, "enable", 6) &&
            !same_word(tok->text, tok->length, "warn", 4) &&
            !same_word(tok->text, tok->length, "disable", 7)) {
            pp_fail(pp, "#extension behaviour must be require, enable, warn or disable",
                    line);
            return;
        }
        if (!raw_next(pp, tok))
            tok->type = GLSL_TOK_EOF;
        skip_to_end_of_line(pp, tok, line);
        return;
    }

    /* `#pragma` is ignored, as the specification says to do with one not recognised;
     * `optimize`, `debug` and `STDGL` change nothing here. */
    if (tok_is(tok, "pragma")) {
        if (!raw_next(pp, tok))
            tok->type = GLSL_TOK_EOF;
        skip_to_end_of_line(pp, tok, line);
        return;
    }

    /* `#line` is accepted and does not move the line numbers. Only diagnostics read
     * them, so a diagnostic names the physical line; no shader compiles differently. */
    if (tok_is(tok, "line")) {
        if (!raw_next(pp, tok))
            tok->type = GLSL_TOK_EOF;
        skip_to_end_of_line(pp, tok, line);
        return;
    }

    pp_fail(pp, "unknown preprocessor directive", line);
}

/* -------------------------------------------------------------------------
 * Function-like macros
 *
 * One argument reader per source - the token stream, and the flat array a `#if`
 * expression lives in - and one substitution shared between them, so `#define MAX(a,b)
 * ((a)>(b)?(a):(b))` means the same thing in code as it does in a condition.
 * ------------------------------------------------------------------------- */

static GLboolean pp_args_begin(pp_args_t *a) {
    a->count = 0;
    a->used = 0;
    return GL_TRUE;
}

static GLboolean pp_args_push(glsl_pp_t *pp, pp_args_t *a, const glsl_token_t *t,
                              int line) {
    if (a->used >= GLSL_PP_MAX_ARG_TOKENS) {
        pp_fail(pp, "macro argument too long", line);
        return GL_FALSE;
    }
    a->tok[a->used++] = *t;
    return GL_TRUE;
}

static GLboolean pp_args_open(glsl_pp_t *pp, pp_args_t *a, int line) {
    if (a->count >= GLSL_MAX_MACRO_PARAMS) {
        pp_fail(pp, "too many macro arguments", line);
        return GL_FALSE;
    }
    a->start[a->count] = a->used;
    a->len[a->count] = 0;
    a->count++;
    return GL_TRUE;
}

/* Checks the count once the list is closed. An arity mismatch is an error, not a pad
 * or a drop. */
static GLboolean pp_args_check(glsl_pp_t *pp, const glsl_macro_t *m, const pp_args_t *a,
                               int line) {
    int supplied = a->count;
    /* `F()` for a one-parameter macro is one empty argument, which is what C and GLSL
     * both say; `F()` for a zero-parameter macro is none. */
    if (m->param_count == 0 && supplied == 1 && a->len[0] == 0)
        supplied = 0;
    if (supplied != m->param_count) {
        pp_fail(pp, "macro called with the wrong number of arguments", line);
        return GL_FALSE;
    }
    return GL_TRUE;
}

/* Substitutes the arguments into the body. A body token that names a parameter becomes
 * that argument's tokens; everything else is copied. */
static int pp_subst(glsl_pp_t *pp, const glsl_macro_t *m, const pp_args_t *a,
                    glsl_token_t *out, int max, int line) {
    int n = 0;
    for (int32_t i = 0; i < m->token_count; i++) {
        const glsl_token_t *bt = &pp->pool[m->first_token + i];
        int p = -1;
        if (bt->type == GLSL_TOK_IDENTIFIER) {
            for (int k = 0; k < m->param_count; k++) {
                if (same_word(m->param_name[k], m->param_len[k], bt->text,
                              bt->length)) {
                    p = k;
                    break;
                }
            }
        }
        if (p < 0) {
            if (n >= max) {
                pp_fail(pp, "macro expansion too large", line);
                return -1;
            }
            out[n++] = *bt;
            continue;
        }
        for (int k = 0; k < a->len[p]; k++) {
            if (n >= max) {
                pp_fail(pp, "macro expansion too large", line);
                return -1;
            }
            out[n++] = a->tok[a->start[p] + k];
        }
    }
    return n;
}

/* Reads `( arg , arg )` off the stream; the caller has already consumed the `(`.
 * Nesting is counted and commas inside it are not separators, so `F(g(a,b), c)` is two
 * arguments. */
static GLboolean pp_read_args_stream(glsl_pp_t *pp, glsl_macro_t *m, pp_args_t *a,
                                     int line) {
    pp_args_begin(a);
    if (!pp_args_open(pp, a, line))
        return GL_FALSE;
    int depth = 0;
    for (;;) {
        glsl_token_t t;
        if (!raw_next(pp, &t) || t.type == GLSL_TOK_EOF) {
            pp_fail(pp, "macro call without a closing `)`", line);
            return GL_FALSE;
        }
        if (t.type == GLSL_TOK_LPAREN)
            depth++;
        if (t.type == GLSL_TOK_RPAREN) {
            if (depth == 0)
                break;
            depth--;
        }
        if (t.type == GLSL_TOK_COMMA && depth == 0) {
            if (!pp_args_open(pp, a, line))
                return GL_FALSE;
            continue;
        }
        if (!pp_args_push(pp, a, &t, line))
            return GL_FALSE;
        a->len[a->count - 1]++;
    }
    return pp_args_check(pp, m, a, line);
}

/* Queues a macro's body for the next reads. For a function-like macro the caller has
 * already confirmed and consumed the `(`. */
static GLboolean expand(glsl_pp_t *pp, glsl_macro_t *m, const pp_args_t *args) {
    glsl_token_t body[GLSL_PP_MAX_ARG_TOKENS];
    const glsl_token_t *src;
    int count;
    if (m->function_like) {
        count = pp_subst(pp, m, args, body, GLSL_PP_MAX_ARG_TOKENS, pp->error_line);
        if (count < 0)
            return GL_FALSE;
        src = body;
    } else {
        src = &pp->pool[m->first_token];
        count = (int)m->token_count;
    }
    /* The body goes in front of what is still queued, not after it: when a macro
     * inside a macro expands, the queue still holds the rest of the outer body, and the
     * inner expansion belongs before it. `#define SCALE(x) ((x) * HALF)` depends on
     * this.
     *
     * An end marker follows the body and clears `expanding` when read. Because splices
     * go in front, an expansion nested inside this one lands before the marker, so the
     * flag means "not within itself": `DUP(HALF)` with `#define DUP(a) ((a) + (a))`
     * expands both `HALF`s, and `#define A A` emits one `A` and stops. */
    const int left = pp->pending_tail - pp->pending_head;
    if (count + 1 + left > GLSL_MAX_PENDING) {
        pp_fail(pp, "macro expansion too large", pp->error_line);
        return GL_FALSE;
    }
    /* The tail moves over itself, from `pending_head` to `count + 1`, and either may be
     * larger; the copy direction is chosen as `memmove` would, so no token is
     * overwritten before it moves. */
    if (count + 1 >= pp->pending_head) {
        for (int i = left - 1; i >= 0; i--) {
            pp->pending[count + 1 + i] = pp->pending[pp->pending_head + i];
        }
    } else {
        for (int i = 0; i < left; i++) {
            pp->pending[count + 1 + i] = pp->pending[pp->pending_head + i];
        }
    }
    for (int i = 0; i < count; i++)
        pp->pending[i] = src[i];
    glsl_token_t end;
    end.type = GLSL_TOK_PP_MACRO_END;
    end.text = (const char *)0;
    end.length = 0;
    end.value = (double)(int)(m - pp->macros);
    end.line = pp->error_line;
    end.column = 0;
    end.error = (const char *)0;
    pp->pending[count] = end;
    pp->pending_head = 0;
    pp->pending_tail = count + 1 + left;
    m->expanding = GL_TRUE;
    return GL_TRUE;
}

GLboolean glsl_pp_next(glsl_pp_t *pp, glsl_token_t *out) {
    if (!pp || !out)
        return GL_FALSE;

    for (;;) {
        if (pp->error) {
            out->type = GLSL_TOK_ERROR;
            out->error = pp->error;
            return GL_FALSE;
        }

        glsl_token_t tok;
        GLboolean have = raw_next(pp, &tok);
        if (!have && tok.type != GLSL_TOK_ERROR) {
            if (pp->cond_depth != 0) {
                pp_fail(pp, "#ifdef with no #endif", tok.line);
                out->type = GLSL_TOK_ERROR;
                out->error = pp->error;
                return GL_FALSE;
            }
            *out = tok;
            return GL_FALSE;
        }
        if (tok.type == GLSL_TOK_ERROR) {
            pp_fail(pp, tok.error ? tok.error : "invalid token", tok.line);
            *out = tok;
            return GL_FALSE;
        }

        if (tok.type == GLSL_TOK_HASH) {
            glsl_token_t after = tok;
            if (!raw_next(pp, &after))
                after.type = GLSL_TOK_EOF;
            /* A directive name on the same line as its `#`. A `#` alone on a line is
             * the null directive and is legal, in which case `after` already belongs to
             * the next line. */
            if (after.line == tok.line && after.type != GLSL_TOK_EOF) {
                do_directive(pp, &after);
            }
            /* Whatever the directive stopped on belongs to the stream: it is the first
             * token of the next line, already taken off the lexer. */
            if (!pp->error && after.type != GLSL_TOK_EOF) {
                pp->held = after;
                pp->has_held = GL_TRUE;
            }
            continue;
        }

        if (!emitting(pp))
            continue;

        if (tok.type == GLSL_TOK_IDENTIFIER) {
            glsl_macro_t *m = find_macro(pp, tok.text, tok.length);
            /* Not while it is already expanding: `#define A A` would otherwise loop. */
            if (m && !m->expanding) {
                pp_args_t args;
                if (m->function_like) {
                    /* A function-like macro's name on its own is not a call and is
                     * emitted unchanged. The next token is read to look for `(` and put
                     * back if it is not one; `held` is free, as `raw_next` above has
                     * just drained it. */
                    glsl_token_t after;
                    const GLboolean got = raw_next(pp, &after);
                    if (!got || after.type != GLSL_TOK_LPAREN) {
                        if (got) {
                            pp->held = after;
                            pp->has_held = GL_TRUE;
                        }
                        *out = tok;
                        return GL_TRUE;
                    }
                    if (!pp_read_args_stream(pp, m, &args, tok.line)) {
                        out->type = GLSL_TOK_ERROR;
                        out->error = pp->error;
                        return GL_FALSE;
                    }
                } else {
                    pp_args_begin(&args);
                }
                if (m->token_count == 0) {
                    /* An empty macro expands to nothing, so read on rather than
                     * emitting it. */
                    m->expanding = GL_TRUE;
                    continue;
                }
                if (!expand(pp, m, &args)) {
                    out->type = GLSL_TOK_ERROR;
                    out->error = pp->error;
                    return GL_FALSE;
                }
                continue;
            }
        }

        *out = tok;
        return GL_TRUE;
    }
}
