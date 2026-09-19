/*
 * oops-gl: the GLSL preprocessor
 *
 * Sits between the lexer and the parser as a token filter. The parser asks for a token; this
 * obeys any directives in the way, expands any macro it finds, and hands back the next real
 * one.
 *
 * # The two things a preprocessor has to get right
 *
 * **Skipping still has to parse.** Inside a false `#ifdef`, the tokens are discarded - but the
 * *directives* are not, because `#ifdef A` ... `#ifdef B` ... `#endif` ... `#endif` must still
 * pair correctly. A skipper that throws away everything until the next `#endif` closes the
 * wrong one, and the error surfaces hundreds of lines later as a brace mismatch.
 *
 * **A macro must not expand itself.** `#define A A` is legal and means "A is A", and expanding
 * the body without guarding loops until something runs out.
 *
 * The guard is a flag per macro, and **when it is cleared is the whole of the difficulty**.
 * Clearing it as the expansion queue drains is too early: the drain happens on the very read
 * that yields the token which would re-trigger the expansion, so `#define A A` still loops.
 * Clearing it when the reader goes back to the *lexer* is right - that is the moment the
 * expansion is genuinely finished - and it makes mutual recursion (`#define A B`,
 * `#define B A`) terminate too, because both flags are still set when the second name comes
 * round.
 */

#include "glsl_internal.h"

static void pp_fail(glsl_pp_t *pp, const char *why, int line) {
    if (pp->error) return;
    pp->error = why;
    pp->error_line = line;
}

static GLboolean same_word(const char *a, size_t an, const char *b, size_t bn) {
    if (an != bn) return GL_FALSE;
    for (size_t i = 0; i < an; i++) {
        if (a[i] != b[i]) return GL_FALSE;
    }
    return GL_TRUE;
}

/* Matches a token's text against a literal - used for directive names, which the lexer hands
 * back as ordinary identifiers or keywords. */
static GLboolean tok_is(const glsl_token_t *t, const char *word) {
    size_t n = 0;
    while (word[n] != '\0') n++;
    return same_word(t->text, t->length, word, n);
}

static glsl_macro_t *find_macro(glsl_pp_t *pp, const char *name, size_t len) {
    for (int i = 0; i < pp->macro_count; i++) {
        if (pp->macros[i].in_use && same_word(pp->macros[i].name, pp->macros[i].name_len, name, len)) {
            return &pp->macros[i];
        }
    }
    return (glsl_macro_t *)0;
}

void glsl_pp_init(glsl_pp_t *pp, const char *source, size_t length) {
    if (!pp) return;
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
        if (!pp->emitting[i]) return GL_FALSE;
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
    if (pp->pending_head < pp->pending_tail) {
        *out = pp->pending[pp->pending_head++];
        if (pp->pending_head == pp->pending_tail) {
            pp->pending_head = pp->pending_tail = 0;
        }
        return GL_TRUE;
    }
    /* **Here, not in the drain above.** Clearing the flags as the queue empties is one read too
     * early - that read is the one that yields the token which would re-trigger the expansion,
     * so `#define A A` loops forever rather than failing. Going back to the lexer is the moment
     * an expansion is genuinely over. */
    for (int i = 0; i < pp->macro_count; i++) pp->macros[i].expanding = GL_FALSE;
    return glsl_lex_next(&pp->lx, out);
}

/* Consumes the rest of the directive line. The lexer does not produce newline tokens, so a
 * directive's end is found by watching the line number change - which is why every token
 * carries one. */
static void skip_to_end_of_line(glsl_pp_t *pp, glsl_token_t *tok, int line) {
    while (tok->type != GLSL_TOK_EOF && tok->line == line) {
        if (!raw_next(pp, tok)) return;
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

    GLboolean have = raw_next(pp, tok);
    /* **A `(` straight after the name makes it function-like**, which this does not do. Caught
     * here rather than silently treated as an object-like macro whose body starts with a
     * parenthesis - that would expand to something that compiles and is wrong. */
    if (have && tok->line == line && tok->type == GLSL_TOK_LPAREN &&
        tok->text == name + name_len) {
        pp_fail(pp, "function-like macros are not handled yet", line);
        return;
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
    if (!have) tok->type = GLSL_TOK_EOF;
}

static void do_undef(glsl_pp_t *pp, glsl_token_t *tok, int line) {
    if (!raw_next(pp, tok) || tok->line != line || tok->type != GLSL_TOK_IDENTIFIER) {
        pp_fail(pp, "#undef with no name", line);
        return;
    }
    glsl_macro_t *m = find_macro(pp, tok->text, tok->length);
    if (m) m->in_use = GL_FALSE;
    /* `#undef` of something never defined is not an error, which is what the specification
     * says and what every header relies on. */
    if (!raw_next(pp, tok)) tok->type = GLSL_TOK_EOF;
    skip_to_end_of_line(pp, tok, line);
}

static void do_version(glsl_pp_t *pp, glsl_token_t *tok, int line) {
    if (!raw_next(pp, tok) || tok->line != line || tok->type != GLSL_TOK_INTCONST) {
        pp_fail(pp, "#version with no number", line);
        return;
    }
    pp->version = (int)tok->value;
    if (!raw_next(pp, tok)) tok->type = GLSL_TOK_EOF;
    skip_to_end_of_line(pp, tok, line);
}

/* Reads one directive. `tok` holds the token after `#` on entry and the first token of the next
 * line on exit. */
static void do_directive(glsl_pp_t *pp, glsl_token_t *tok) {
    const int line = tok->line;

    /* A `#` on a line of its own is the null directive and is legal. */
    if (tok->type == GLSL_TOK_EOF || tok->line != line) return;

    /* **Conditionals are read even while skipping**, so nesting stays balanced. Everything
     * else is obeyed only when output is live. */
    if (tok_is(tok, "ifdef") || tok_is(tok, "ifndef")) {
        GLboolean want_defined = tok_is(tok, "ifdef");
        if (pp->cond_depth >= GLSL_MAX_COND_DEPTH) {
            pp_fail(pp, "conditionals nested too deeply", line);
            return;
        }
        GLboolean live = emitting(pp);
        if (!raw_next(pp, tok) || tok->line != line || tok->type != GLSL_TOK_IDENTIFIER) {
            pp_fail(pp, "#ifdef with no name", line);
            return;
        }
        GLboolean defined = (GLboolean)(find_macro(pp, tok->text, tok->length) != (glsl_macro_t *)0);
        GLboolean on = (GLboolean)(defined == want_defined);
        /* A branch inside a dark region stays dark however its condition reads. */
        pp->emitting[pp->cond_depth] = (GLboolean)(live && on);
        pp->taken[pp->cond_depth] = (GLboolean)(live && on);
        pp->cond_depth++;
        if (!raw_next(pp, tok)) tok->type = GLSL_TOK_EOF;
        skip_to_end_of_line(pp, tok, line);
        return;
    }

    if (tok_is(tok, "else")) {
        if (pp->cond_depth == 0) {
            pp_fail(pp, "#else with no #ifdef", line);
            return;
        }
        int top = pp->cond_depth - 1;
        /* Live only if no branch here has been taken *and* the enclosing region is live. */
        GLboolean outer_live = GL_TRUE;
        for (int i = 0; i < top; i++) {
            if (!pp->emitting[i]) outer_live = GL_FALSE;
        }
        pp->emitting[top] = (GLboolean)(outer_live && !pp->taken[top]);
        if (pp->emitting[top]) pp->taken[top] = GL_TRUE;
        if (!raw_next(pp, tok)) tok->type = GLSL_TOK_EOF;
        skip_to_end_of_line(pp, tok, line);
        return;
    }

    if (tok_is(tok, "endif")) {
        if (pp->cond_depth == 0) {
            pp_fail(pp, "#endif with no #ifdef", line);
            return;
        }
        pp->cond_depth--;
        if (!raw_next(pp, tok)) tok->type = GLSL_TOK_EOF;
        skip_to_end_of_line(pp, tok, line);
        return;
    }

    if (!emitting(pp)) {
        /* Any other directive inside a dark region is ignored entirely - including ones this
         * would otherwise refuse, because a `#extension` in a branch that is not compiled has
         * not been asked for. */
        skip_to_end_of_line(pp, tok, line);
        return;
    }

    if (tok_is(tok, "define")) { do_define(pp, tok, line); return; }
    if (tok_is(tok, "undef"))  { do_undef(pp, tok, line); return; }
    if (tok_is(tok, "version")) { do_version(pp, tok, line); return; }
    if (tok_is(tok, "error")) {
        pp_fail(pp, "#error directive in the shader source", line);
        return;
    }

    /* **Refused by name, not skipped.** A skipped `#extension` compiles a shader that asked for
     * something it did not get; a skipped `#if` takes the wrong branch. Both are worse than a
     * compile error that names the directive. */
    if (tok_is(tok, "if") || tok_is(tok, "elif")) {
        pp_fail(pp, "#if and #elif are not handled yet; use #ifdef", line);
        return;
    }
    if (tok_is(tok, "extension")) {
        pp_fail(pp, "#extension is not handled yet", line);
        return;
    }
    if (tok_is(tok, "pragma") || tok_is(tok, "line")) {
        pp_fail(pp, "#pragma and #line are not handled yet", line);
        return;
    }
    pp_fail(pp, "unknown preprocessor directive", line);
}

/* Queues a macro's body for the next reads. */
static GLboolean expand(glsl_pp_t *pp, glsl_macro_t *m) {
    int room = GLSL_MAX_PENDING - pp->pending_tail;
    if (m->token_count > room) {
        pp_fail(pp, "macro expansion too large", pp->error_line);
        return GL_FALSE;
    }
    for (int32_t i = 0; i < m->token_count; i++) {
        pp->pending[pp->pending_tail++] = pp->pool[m->first_token + i];
    }
    m->expanding = GL_TRUE;
    return GL_TRUE;
}

GLboolean glsl_pp_next(glsl_pp_t *pp, glsl_token_t *out) {
    if (!pp || !out) return GL_FALSE;

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
            if (!raw_next(pp, &after)) after.type = GLSL_TOK_EOF;
            /* A directive name on the same line as its `#`. A `#` alone on a line is the null
             * directive and is legal, in which case `after` already belongs to the next line. */
            if (after.line == tok.line && after.type != GLSL_TOK_EOF) {
                do_directive(pp, &after);
            }
            /* **Whatever the directive stopped on belongs to the stream.** It is the first
             * token of the next line, already taken off the lexer, and dropping it here loses
             * the first token after every directive. */
            if (!pp->error && after.type != GLSL_TOK_EOF) {
                pp->held = after;
                pp->has_held = GL_TRUE;
            }
            continue;
        }

        if (!emitting(pp)) continue;

        if (tok.type == GLSL_TOK_IDENTIFIER) {
            glsl_macro_t *m = find_macro(pp, tok.text, tok.length);
            /* **Not while it is already expanding**: `#define A A` would otherwise loop. */
            if (m && !m->expanding) {
                if (m->token_count == 0) {
                    /* An empty macro expands to nothing, so read on rather than emitting it. */
                    m->expanding = GL_TRUE;
                    continue;
                }
                if (!expand(pp, m)) {
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
