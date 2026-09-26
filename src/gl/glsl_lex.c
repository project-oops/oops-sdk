/*
 * oops-gl: the GLSL lexer
 *
 * Turns GLSL 1.10 source into tokens. It owns no memory - every token points into the
 * source the caller supplied - and it never stops: an error is a token, so a compiler
 * built on this can report several problems in one pass instead of the first one and
 * nothing else.
 *
 * # Two things a hand-written lexer usually gets wrong
 *
 * **Maximal munch.** `>=` is one token and not `>` followed by `=`; `++` is not `+ +`.
 * Every operator that is a prefix of a longer one has to test for the longer one first,
 * which is why the operator switch below looks repetitive rather than clever.
 *
 * **Keyword matching is exact.** `float` is a keyword and `floatx` is an identifier.
 * Matching by prefix would make the second one lex as a keyword followed by an
 * identifier, and the resulting syntax error would point at the wrong thing entirely.
 * The identifier is scanned to its full length first and *then* compared, whole.
 */

#include "glsl_internal.h"

static GLboolean is_digit(char c) {
    return (GLboolean)(c >= '0' && c <= '9');
}
static GLboolean is_hex(char c) {
    return (GLboolean)(is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
}
static GLboolean is_ident_start(char c) {
    return (GLboolean)((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_');
}
static GLboolean is_ident_cont(char c) {
    return (GLboolean)(is_ident_start(c) || is_digit(c));
}

void glsl_lexer_init(glsl_lexer_t *lx, const char *source, size_t length) {
    if (!lx)
        return;
    lx->src = source;
    lx->length = source ? length : 0u;
    lx->pos = 0u;
    lx->line = 1;
    lx->column = 1;
}

static char peek(const glsl_lexer_t *lx, size_t ahead) {
    size_t at = lx->pos + ahead;
    return at < lx->length ? lx->src[at] : '\0';
}

/* One character forward, keeping the line and column honest.
 *
 * **A newline is counted here and nowhere else.** Comments and whitespace advance
 * through this same function precisely so that a diagnostic after a twenty-line block
 * comment still names the right line - tracking lines only in the token scanner is how
 * that goes wrong. */
static char advance(glsl_lexer_t *lx) {
    if (lx->pos >= lx->length)
        return '\0';
    char c = lx->src[lx->pos++];
    if (c == '\n') {
        lx->line++;
        lx->column = 1;
    } else {
        lx->column++;
    }
    return c;
}

/* Whitespace and comments, which are the same thing to a token stream.
 * Returns an error string if a block comment never closed, else NULL. */
static const char *skip_trivia(glsl_lexer_t *lx) {
    for (;;) {
        char c = peek(lx, 0);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lx);
        } else if (c == '/' && peek(lx, 1) == '/') {
            while (lx->pos < lx->length && peek(lx, 0) != '\n')
                advance(lx);
        } else if (c == '/' && peek(lx, 1) == '*') {
            advance(lx); /* '/' */
            advance(lx); /* '*' */
            for (;;) {
                if (lx->pos >= lx->length) {
                    /* **This is the loop's only exit when the comment never closes**,
                     * as well as the diagnostic. `advance` returns '\0' without moving
                     * once the source is exhausted, so without this test the scan spins
                     * forever rather than reaching the end - removing it hangs the
                     * build rather than failing it.
                     *
                     * The diagnostic matters too: an unterminated comment otherwise
                     * swallows the rest of the program, and the error a parser reports
                     * is "unexpected end of input" pointing at the last line rather
                     * than at the comment that ate it. */
                    return "unterminated block comment";
                }
                if (peek(lx, 0) == '*' && peek(lx, 1) == '/') {
                    advance(lx);
                    advance(lx);
                    break;
                }
                advance(lx);
            }
        } else {
            return NULL;
        }
    }
}

/* An exact, whole-word comparison against a literal. `n` is the token's full scanned
 * length, so a token longer than the keyword cannot match a prefix of it. */
static GLboolean word_is(const char *text, size_t n, const char *literal) {
    size_t i = 0;
    while (literal[i] != '\0') {
        if (i >= n || text[i] != literal[i])
            return GL_FALSE;
        i++;
    }
    return (GLboolean)(i == n);
}

static glsl_token_type_t keyword_of(const char *text, size_t n) {
    static const struct {
        const char *word;
        glsl_token_type_t type;
    } table[] = {
        {"attribute", GLSL_TOK_KW_ATTRIBUTE},
        {"const", GLSL_TOK_KW_CONST},
        {"uniform", GLSL_TOK_KW_UNIFORM},
        {"varying", GLSL_TOK_KW_VARYING},
        {"break", GLSL_TOK_KW_BREAK},
        {"continue", GLSL_TOK_KW_CONTINUE},
        {"do", GLSL_TOK_KW_DO},
        {"for", GLSL_TOK_KW_FOR},
        {"while", GLSL_TOK_KW_WHILE},
        {"if", GLSL_TOK_KW_IF},
        {"else", GLSL_TOK_KW_ELSE},
        {"discard", GLSL_TOK_KW_DISCARD},
        {"return", GLSL_TOK_KW_RETURN},
        {"struct", GLSL_TOK_KW_STRUCT},
        {"void", GLSL_TOK_KW_VOID},
        {"in", GLSL_TOK_KW_IN},
        {"out", GLSL_TOK_KW_OUT},
        {"inout", GLSL_TOK_KW_INOUT},
        {"float", GLSL_TOK_KW_FLOAT},
        {"int", GLSL_TOK_KW_INT},
        {"bool", GLSL_TOK_KW_BOOL},
        {"true", GLSL_TOK_KW_TRUE},
        {"false", GLSL_TOK_KW_FALSE},
        {"vec2", GLSL_TOK_KW_VEC2},
        {"vec3", GLSL_TOK_KW_VEC3},
        {"vec4", GLSL_TOK_KW_VEC4},
        {"ivec2", GLSL_TOK_KW_IVEC2},
        {"ivec3", GLSL_TOK_KW_IVEC3},
        {"ivec4", GLSL_TOK_KW_IVEC4},
        {"bvec2", GLSL_TOK_KW_BVEC2},
        {"bvec3", GLSL_TOK_KW_BVEC3},
        {"bvec4", GLSL_TOK_KW_BVEC4},
        {"mat2", GLSL_TOK_KW_MAT2},
        {"mat3", GLSL_TOK_KW_MAT3},
        {"mat4", GLSL_TOK_KW_MAT4},
        /* **1.20's non-square matrices, and the long spellings of the square ones.**
         * `mat2x2` is `mat2` - the same type under a second name the language also
         * gives it - so it lexes to the same token rather than to one of its own. The
         * parser then has one case per type and not one per spelling. */
        {"mat2x2", GLSL_TOK_KW_MAT2},
        {"mat3x3", GLSL_TOK_KW_MAT3},
        {"mat4x4", GLSL_TOK_KW_MAT4},
        {"mat2x3", GLSL_TOK_KW_MAT2X3},
        {"mat2x4", GLSL_TOK_KW_MAT2X4},
        {"mat3x2", GLSL_TOK_KW_MAT3X2},
        {"mat3x4", GLSL_TOK_KW_MAT3X4},
        {"mat4x2", GLSL_TOK_KW_MAT4X2},
        {"mat4x3", GLSL_TOK_KW_MAT4X3},
        {"sampler1D", GLSL_TOK_KW_SAMPLER1D},
        {"sampler2D", GLSL_TOK_KW_SAMPLER2D},
        {"sampler3D", GLSL_TOK_KW_SAMPLER3D},
        {"samplerCube", GLSL_TOK_KW_SAMPLERCUBE},
        {"sampler1DShadow", GLSL_TOK_KW_SAMPLER1DSHADOW},
        {"sampler2DShadow", GLSL_TOK_KW_SAMPLER2DSHADOW},
        /* **GLSL 1.20's two new qualifiers**, recognised whatever the shader's version.
         *
         * They are not 1.10 keywords, so a 1.10 shader could in principle use one as a
         * variable name - and every implementation anyone would port against reserves
         * them anyway, so a shader that did would already be unportable. Recognising
         * them always is one table rather than a version-dependent one, and the parser
         * refuses them by name in a 1.10 shader, which is a better diagnostic than
         * "syntax error" either way. */
        {"invariant", GLSL_TOK_KW_INVARIANT},
        {"centroid", GLSL_TOK_KW_CENTROID},
        /* ES 1.00's precision words, recognised whatever the version for the same
         * reason as the two above: a desktop shader using one as an identifier was
         * already unportable, and naming them gives a better diagnostic than a syntax
         * error further on. */
        {"precision", GLSL_TOK_KW_PRECISION},
        {"lowp", GLSL_TOK_KW_LOWP},
        {"mediump", GLSL_TOK_KW_MEDIUMP},
        {"highp", GLSL_TOK_KW_HIGHP},
        /* Reserved by GLSL 1.10. Named so a program using one is told *which* word it
         * may not use, instead of meeting a syntax error somewhere downstream. */
        {"asm", GLSL_TOK_KW_RESERVED},
        {"class", GLSL_TOK_KW_RESERVED},
        {"union", GLSL_TOK_KW_RESERVED},
        {"enum", GLSL_TOK_KW_RESERVED},
        {"typedef", GLSL_TOK_KW_RESERVED},
        {"template", GLSL_TOK_KW_RESERVED},
        {"this", GLSL_TOK_KW_RESERVED},
        {"packed", GLSL_TOK_KW_RESERVED},
        {"goto", GLSL_TOK_KW_RESERVED},
        {"switch", GLSL_TOK_KW_RESERVED},
        {"default", GLSL_TOK_KW_RESERVED},
        {"inline", GLSL_TOK_KW_RESERVED},
        {"noinline", GLSL_TOK_KW_RESERVED},
        {"volatile", GLSL_TOK_KW_RESERVED},
        {"public", GLSL_TOK_KW_RESERVED},
        {"static", GLSL_TOK_KW_RESERVED},
        {"extern", GLSL_TOK_KW_RESERVED},
        {"external", GLSL_TOK_KW_RESERVED},
        {"interface", GLSL_TOK_KW_RESERVED},
        {"long", GLSL_TOK_KW_RESERVED},
        {"short", GLSL_TOK_KW_RESERVED},
        {"double", GLSL_TOK_KW_RESERVED},
        {"half", GLSL_TOK_KW_RESERVED},
        {"fixed", GLSL_TOK_KW_RESERVED},
        {"unsigned", GLSL_TOK_KW_RESERVED},
        {"input", GLSL_TOK_KW_RESERVED},
        {"output", GLSL_TOK_KW_RESERVED},
        {"sizeof", GLSL_TOK_KW_RESERVED},
        {"cast", GLSL_TOK_KW_RESERVED},
        {"namespace", GLSL_TOK_KW_RESERVED},
        {"using", GLSL_TOK_KW_RESERVED},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (word_is(text, n, table[i].word))
            return table[i].type;
    }
    return GLSL_TOK_IDENTIFIER;
}

/* A number, which is where GLSL's grammar is fussiest.
 *
 * `1`, `017`, `0x1f` are integers; `1.0`, `1.`, `.5`, `1e5`, `1.5E-3` are floats. **The
 * distinction is made by what the scan actually found**, not by looking for a `.` up
 * front: `1e5` has no dot and is a float, and `1.` has nothing after the dot and still
 * is one.
 */
static void scan_number(glsl_lexer_t *lx, glsl_token_t *out) {
    GLboolean is_float = GL_FALSE;

    if (peek(lx, 0) == '0' && (peek(lx, 1) == 'x' || peek(lx, 1) == 'X')) {
        advance(lx);
        advance(lx);
        if (!is_hex(peek(lx, 0))) {
            out->type = GLSL_TOK_ERROR;
            out->error = "hexadecimal constant with no digits";
            return;
        }
        double v = 0.0;
        while (is_hex(peek(lx, 0))) {
            char c = advance(lx);
            int d = is_digit(c)
                        ? (c - '0')
                        : ((c | 0x20) - 'a' + 10); /* case-folded, so 0xAB == 0xab */
            v = v * 16.0 + (double)d;
        }
        out->type = GLSL_TOK_INTCONST;
        out->value = v;
        return;
    }

    double whole = 0.0;
    while (is_digit(peek(lx, 0))) {
        whole = whole * 10.0 + (double)(advance(lx) - '0');
    }

    if (peek(lx, 0) == '.') {
        is_float = GL_TRUE;
        advance(lx);
        double frac = 0.0, scale = 0.1;
        while (is_digit(peek(lx, 0))) {
            frac += (double)(advance(lx) - '0') * scale;
            scale *= 0.1;
        }
        whole += frac;
    }

    if (peek(lx, 0) == 'e' || peek(lx, 0) == 'E') {
        /* Only an exponent if something follows it that can be one. `1einvalid` is `1`
         * then the identifier `einvalid`, which is a syntax error later rather than a
         * lexing error now - consuming the `e` regardless would turn it into a
         * confusing one. */
        char after = peek(lx, 1);
        char after2 = peek(lx, 2);
        if (is_digit(after) || ((after == '+' || after == '-') && is_digit(after2))) {
            is_float = GL_TRUE;
            advance(lx);
            int sign = 1;
            if (peek(lx, 0) == '+') {
                advance(lx);
            } else if (peek(lx, 0) == '-') {
                sign = -1;
                advance(lx);
            }
            int exp = 0;
            while (is_digit(peek(lx, 0))) {
                exp = exp * 10 + (advance(lx) - '0');
                if (exp > 4096)
                    exp = 4096; /* saturate rather than overflow the counter */
            }
            for (int i = 0; i < exp; i++) {
                whole = sign > 0 ? whole * 10.0 : whole / 10.0;
            }
        }
    }

    /* **A number cannot be followed straight into an identifier.** `123abc` is not
     * `123` then `abc`: the specification has no such juxtaposition, and lexing it as
     * two tokens produces a syntax error that blames `abc`. Caught here, where the text
     * is still to hand. */
    if (is_ident_start(peek(lx, 0))) {
        out->type = GLSL_TOK_ERROR;
        out->error = "identifier immediately after a numeric constant";
        return;
    }

    out->type = is_float ? GLSL_TOK_FLOATCONST : GLSL_TOK_INTCONST;
    out->value = whole;
}

GLboolean glsl_lex_next(glsl_lexer_t *lx, glsl_token_t *out) {
    if (!lx || !out)
        return GL_FALSE;

    out->type = GLSL_TOK_EOF;
    out->text = lx->src ? lx->src + lx->pos : (const char *)0;
    out->length = 0u;
    out->value = 0.0;
    out->error = (const char *)0;

    const char *trivia_error = skip_trivia(lx);

    /* Recorded after the trivia, so a token's position is the token's own and not the
     * whitespace in front of it. */
    out->line = lx->line;
    out->column = lx->column;
    out->text = lx->src ? lx->src + lx->pos : (const char *)0;

    if (trivia_error) {
        out->type = GLSL_TOK_ERROR;
        out->error = trivia_error;
        return GL_FALSE; /* nothing can follow: the rest of the source was inside the
                            comment */
    }

    if (lx->pos >= lx->length) {
        out->type = GLSL_TOK_EOF;
        return GL_FALSE;
    }

    const size_t start = lx->pos;
    char c = peek(lx, 0);

    if (is_ident_start(c)) {
        while (is_ident_cont(peek(lx, 0)))
            advance(lx);
        out->length = lx->pos - start;
        out->type = keyword_of(out->text, out->length);
        return GL_TRUE;
    }

    if (is_digit(c) || (c == '.' && is_digit(peek(lx, 1)))) {
        scan_number(lx, out);
        out->length = lx->pos - start;
        return GL_TRUE;
    }

    advance(lx);
    switch (c) {
    /* Every operator that is a prefix of a longer one tests for the longer one first.
     */
    case '+':
        if (peek(lx, 0) == '+') {
            advance(lx);
            out->type = GLSL_TOK_INC;
        } else if (peek(lx, 0) == '=') {
            advance(lx);
            out->type = GLSL_TOK_ADD_ASSIGN;
        } else
            out->type = GLSL_TOK_PLUS;
        break;
    case '-':
        if (peek(lx, 0) == '-') {
            advance(lx);
            out->type = GLSL_TOK_DEC;
        } else if (peek(lx, 0) == '=') {
            advance(lx);
            out->type = GLSL_TOK_SUB_ASSIGN;
        } else
            out->type = GLSL_TOK_MINUS;
        break;
    case '*':
        if (peek(lx, 0) == '=') {
            advance(lx);
            out->type = GLSL_TOK_MUL_ASSIGN;
        } else
            out->type = GLSL_TOK_STAR;
        break;
    case '/':
        /* A comment was already consumed by skip_trivia, so a `/` here is division. */
        if (peek(lx, 0) == '=') {
            advance(lx);
            out->type = GLSL_TOK_DIV_ASSIGN;
        } else
            out->type = GLSL_TOK_SLASH;
        break;
    case '%':
        out->type = GLSL_TOK_PERCENT;
        break;
    case '<':
        if (peek(lx, 0) == '=') {
            advance(lx);
            out->type = GLSL_TOK_LE;
        } else
            out->type = GLSL_TOK_LT;
        break;
    case '>':
        if (peek(lx, 0) == '=') {
            advance(lx);
            out->type = GLSL_TOK_GE;
        } else
            out->type = GLSL_TOK_GT;
        break;
    case '=':
        if (peek(lx, 0) == '=') {
            advance(lx);
            out->type = GLSL_TOK_EQ;
        } else
            out->type = GLSL_TOK_ASSIGN;
        break;
    case '!':
        if (peek(lx, 0) == '=') {
            advance(lx);
            out->type = GLSL_TOK_NE;
        } else
            out->type = GLSL_TOK_BANG;
        break;
    case '&':
        if (peek(lx, 0) == '&') {
            advance(lx);
            out->type = GLSL_TOK_AND_AND;
        } else
            out->type = GLSL_TOK_AMP;
        break;
    case '|':
        if (peek(lx, 0) == '|') {
            advance(lx);
            out->type = GLSL_TOK_OR_OR;
        } else
            out->type = GLSL_TOK_PIPE;
        break;
    case '^':
        if (peek(lx, 0) == '^') {
            advance(lx);
            out->type = GLSL_TOK_XOR_XOR;
        } else
            out->type = GLSL_TOK_CARET;
        break;
    case '~':
        out->type = GLSL_TOK_TILDE;
        break;
    case '(':
        out->type = GLSL_TOK_LPAREN;
        break;
    case ')':
        out->type = GLSL_TOK_RPAREN;
        break;
    case '[':
        out->type = GLSL_TOK_LBRACKET;
        break;
    case ']':
        out->type = GLSL_TOK_RBRACKET;
        break;
    case '{':
        out->type = GLSL_TOK_LBRACE;
        break;
    case '}':
        out->type = GLSL_TOK_RBRACE;
        break;
    case ';':
        out->type = GLSL_TOK_SEMICOLON;
        break;
    case ',':
        out->type = GLSL_TOK_COMMA;
        break;
    case '.':
        out->type = GLSL_TOK_DOT;
        break;
    case ':':
        out->type = GLSL_TOK_COLON;
        break;
    case '?':
        out->type = GLSL_TOK_QUESTION;
        break;
    case '#':
        out->type = GLSL_TOK_HASH;
        break;
    default:
        out->type = GLSL_TOK_ERROR;
        out->error = "character that is not part of the GLSL alphabet";
        break;
    }

    out->length = lx->pos - start;
    return GL_TRUE;
}
