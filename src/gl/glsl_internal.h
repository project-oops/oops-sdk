/*
 * oops-gl: the GLSL front end - tokens
 *
 * GL 2.0 is GLSL, and GLSL is a compiler. This is its first stage and the only one that can be
 * finished today: the lexer is pure text handling, so it is testable to the same standard as
 * everything else here, while the code generator behind it needs a shader interface that is
 * still waiting on a hardware measurement (obSCEne REQ-...-3a91).
 *
 * Scoped to **GLSL 1.10**, which is the language GL 2.0 defines. Later versions add keywords
 * and drop others; a token stream that quietly accepted `in`/`out` as 1.10 storage qualifiers
 * when the program meant the 1.30 ones would be a compiler that agreed with the wrong
 * specification.
 */

#ifndef __GLSL_INTERNAL_H__
#define __GLSL_INTERNAL_H__

#include "gl_internal.h"

typedef enum {
    GLSL_TOK_EOF = 0,
    GLSL_TOK_ERROR,

    GLSL_TOK_IDENTIFIER,
    GLSL_TOK_INTCONST,
    GLSL_TOK_FLOATCONST,

    /* Keywords. GLSL 1.10's reserved words are a closed set; anything outside it is an
     * identifier, which is why these are listed rather than matched by prefix. */
    GLSL_TOK_KW_ATTRIBUTE, GLSL_TOK_KW_CONST, GLSL_TOK_KW_UNIFORM, GLSL_TOK_KW_VARYING,
    GLSL_TOK_KW_BREAK, GLSL_TOK_KW_CONTINUE, GLSL_TOK_KW_DO, GLSL_TOK_KW_FOR,
    GLSL_TOK_KW_WHILE, GLSL_TOK_KW_IF, GLSL_TOK_KW_ELSE, GLSL_TOK_KW_DISCARD,
    GLSL_TOK_KW_RETURN, GLSL_TOK_KW_STRUCT, GLSL_TOK_KW_VOID,
    GLSL_TOK_KW_IN, GLSL_TOK_KW_OUT, GLSL_TOK_KW_INOUT,
    GLSL_TOK_KW_FLOAT, GLSL_TOK_KW_INT, GLSL_TOK_KW_BOOL,
    GLSL_TOK_KW_TRUE, GLSL_TOK_KW_FALSE,
    GLSL_TOK_KW_VEC2, GLSL_TOK_KW_VEC3, GLSL_TOK_KW_VEC4,
    GLSL_TOK_KW_IVEC2, GLSL_TOK_KW_IVEC3, GLSL_TOK_KW_IVEC4,
    GLSL_TOK_KW_BVEC2, GLSL_TOK_KW_BVEC3, GLSL_TOK_KW_BVEC4,
    GLSL_TOK_KW_MAT2, GLSL_TOK_KW_MAT3, GLSL_TOK_KW_MAT4,
    GLSL_TOK_KW_SAMPLER1D, GLSL_TOK_KW_SAMPLER2D, GLSL_TOK_KW_SAMPLER3D,
    GLSL_TOK_KW_SAMPLERCUBE, GLSL_TOK_KW_SAMPLER1DSHADOW, GLSL_TOK_KW_SAMPLER2DSHADOW,
    /* Reserved by 1.10 for future use. Recognised so they can be **refused with their own
     * name** rather than parsed as identifiers and failing later as a mysterious syntax
     * error. */
    GLSL_TOK_KW_RESERVED,

    /* Punctuation and operators */
    GLSL_TOK_LPAREN, GLSL_TOK_RPAREN, GLSL_TOK_LBRACKET, GLSL_TOK_RBRACKET,
    GLSL_TOK_LBRACE, GLSL_TOK_RBRACE, GLSL_TOK_SEMICOLON, GLSL_TOK_COMMA,
    GLSL_TOK_DOT, GLSL_TOK_COLON, GLSL_TOK_QUESTION,
    GLSL_TOK_PLUS, GLSL_TOK_MINUS, GLSL_TOK_STAR, GLSL_TOK_SLASH, GLSL_TOK_PERCENT,
    GLSL_TOK_BANG, GLSL_TOK_ASSIGN,
    GLSL_TOK_LT, GLSL_TOK_GT, GLSL_TOK_LE, GLSL_TOK_GE, GLSL_TOK_EQ, GLSL_TOK_NE,
    GLSL_TOK_AND_AND, GLSL_TOK_OR_OR, GLSL_TOK_XOR_XOR,
    GLSL_TOK_INC, GLSL_TOK_DEC,
    GLSL_TOK_ADD_ASSIGN, GLSL_TOK_SUB_ASSIGN, GLSL_TOK_MUL_ASSIGN, GLSL_TOK_DIV_ASSIGN,
    GLSL_TOK_AMP, GLSL_TOK_PIPE, GLSL_TOK_CARET, GLSL_TOK_TILDE,

    /* `#`, which starts a preprocessor directive. The preprocessor is a separate stage and is
     * not written yet; the lexer names the token so that stage has something to consume, and
     * so a `#version` line is not mistaken for an operator. */
    GLSL_TOK_HASH
} glsl_token_type_t;

typedef struct {
    glsl_token_type_t type;
    const char *text;   /* into the source, not a copy: the lexer owns no memory */
    size_t length;
    int line;           /* 1-based, as a compiler diagnostic quotes them */
    int column;         /* 1-based, counted in bytes */
    /* Only for GLSL_TOK_INTCONST and GLSL_TOK_FLOATCONST. */
    double value;
    /* Only for GLSL_TOK_ERROR: why, as a literal, so there is nothing to free. */
    const char *error;
} glsl_token_t;

typedef struct {
    const char *src;
    size_t length;
    size_t pos;
    int line;
    int column;
} glsl_lexer_t;

void glsl_lexer_init(glsl_lexer_t *lx, const char *source, size_t length);
/* Writes the next token and returns GL_FALSE once GLSL_TOK_EOF has been produced, so a caller
 * can drive it with `while (glsl_lex_next(&lx, &t))`. An error token stops nothing: the caller
 * decides whether to keep going, which is what lets a compiler report more than one problem. */
GLboolean glsl_lex_next(glsl_lexer_t *lx, glsl_token_t *out);

#endif /* __GLSL_INTERNAL_H__ */
