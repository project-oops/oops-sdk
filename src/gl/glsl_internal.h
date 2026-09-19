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

/* -------------------------------------------------------------------------
 * The AST
 *
 * Nodes live in a caller-supplied arena and refer to each other by **index, not pointer**.
 * Indices survive the arena being copied or moved, they are half the size, and a stale one is a
 * bounds check rather than a wild read - which matters in a freestanding binary where there is
 * nothing to catch the alternative.
 *
 * The arena is fixed and refuses when full, like every other buffer here. A shader that needs
 * more nodes than this is refused with a diagnostic rather than served by an allocation nobody
 * asked for.
 * ------------------------------------------------------------------------- */

#define GLSL_MAX_NODES 2048
#define GLSL_NO_NODE (-1)

typedef enum {
    GLSL_NODE_IDENTIFIER,
    GLSL_NODE_INTCONST,
    GLSL_NODE_FLOATCONST,
    GLSL_NODE_BOOLCONST,
    GLSL_NODE_UNARY,       /* op applied to `a`, prefix */
    GLSL_NODE_POSTFIX,     /* op applied to `a`, postfix: ++ and -- only */
    GLSL_NODE_BINARY,      /* `a` op `b` */
    GLSL_NODE_ASSIGN,      /* `a` op `b`, op being = += -= *= /= */
    GLSL_NODE_CONDITIONAL, /* `a` ? `b` : `c` */
    GLSL_NODE_SEQUENCE,    /* `a` , `b` */
    GLSL_NODE_INDEX,       /* `a` [ `b` ] */
    GLSL_NODE_FIELD,       /* `a` . name, the name in text/length */
    GLSL_NODE_CALL,        /* `a` ( args ), args chained through `sibling` */

    /* Statements */
    GLSL_NODE_COMPOUND,    /* { ... }, statements chained from `a` through `sibling` */
    GLSL_NODE_IF,          /* if (`a`) `b` else `c` */
    GLSL_NODE_WHILE,       /* while (`a`) `b` */
    GLSL_NODE_DO_WHILE,    /* do `a` while (`b`) */
    GLSL_NODE_FOR,         /* for (`a`; `b`; `c`) `d` */
    GLSL_NODE_RETURN,      /* return `a`, or `a` absent */
    GLSL_NODE_BREAK,
    GLSL_NODE_CONTINUE,
    GLSL_NODE_DISCARD,
    GLSL_NODE_EXPR_STMT,   /* `a` ;  - `a` absent for the empty statement */

    /* Declarations */
    GLSL_NODE_DECL,        /* one declarator: name in text, `a` = initialiser, `array_size` =
                            * the size expression. `float a, b;` is two of these chained
                            * through `sibling`. */
    GLSL_NODE_PARAM,       /* one function parameter */
    GLSL_NODE_FUNCTION,    /* name in text, `b` = parameter chain, `c` = body.
                            * **`c` absent means a prototype**, not an empty body. */
    GLSL_NODE_UNIT         /* the whole source: declarations chained from `a` */
} glsl_node_kind_t;

typedef struct {
    glsl_node_kind_t kind;
    glsl_token_type_t op;   /* for UNARY/POSTFIX/BINARY/ASSIGN; GLSL_TOK_EOF otherwise */
    int32_t a, b, c, d;     /* children, GLSL_NO_NODE when absent. `d` exists for `for`, which
                             * is the one construct here with four of them. */
    int32_t sibling;        /* next in a list - call arguments, statements, declarators,
                             * parameters, top-level declarations. GLSL_NO_NODE at the end. */
    /* Declarations and parameters: the type as written. A type is a token rather than a
     * resolved type here, because resolving `vec4` to a type - and deciding whether an
     * identifier names a struct - is the semantic stage's job, not the grammar's. */
    glsl_token_type_t type_tok;
    glsl_token_type_t qualifier;  /* const/attribute/varying/uniform/in/out/inout, or EOF */
    int32_t array_size;           /* the size expression, GLSL_NO_NODE if not an array */
    const char *text;       /* identifier or field name: into the source, never a copy */
    size_t length;
    double value;           /* for INTCONST/FLOATCONST/BOOLCONST */
    int line, column;
} glsl_node_t;

typedef struct {
    glsl_node_t nodes[GLSL_MAX_NODES];
    int32_t count;
} glsl_ast_t;

typedef struct {
    glsl_lexer_t lx;
    glsl_token_t tok;       /* one token of lookahead, which is all this grammar needs */
    glsl_ast_t *ast;
    const char *error;      /* a literal, so there is nothing to free */
    int error_line, error_column;
} glsl_parser_t;

/* -------------------------------------------------------------------------
 * The preprocessor
 *
 * A **token filter**, not a text-to-text pass: the parser pulls tokens through it, so nothing
 * has to allocate a rewritten copy of the source and every token keeps pointing into the
 * original text for diagnostics.
 *
 * Scoped to what GLSL 1.10 shaders actually use: `#version`, object-like `#define` and
 * `#undef`, `#ifdef`/`#ifndef`/`#else`/`#endif`, and `#error`. Function-like macros, `#if` with
 * an expression, `#extension`, `#pragma` and `#line` are **refused by name** rather than
 * skipped - a skipped `#extension` would compile a shader that asked for something it did not
 * get.
 * ------------------------------------------------------------------------- */

#define GLSL_MAX_MACROS 64
#define GLSL_MAX_MACRO_TOKENS 1024
#define GLSL_MAX_COND_DEPTH 32
#define GLSL_MAX_PENDING 128

typedef struct {
    const char *name;
    size_t name_len;
    int32_t first_token;   /* into the token pool */
    int32_t token_count;
    GLboolean in_use;      /* defined at all */
    GLboolean expanding;   /* **currently being expanded**: stops `#define A A` looping */
} glsl_macro_t;

typedef struct {
    glsl_lexer_t lx;

    glsl_macro_t macros[GLSL_MAX_MACROS];
    int macro_count;
    glsl_token_t pool[GLSL_MAX_MACRO_TOKENS];
    int pool_count;

    /* Tokens produced by expansion, served before the lexer is read again. A ring would be
     * tidier; a straight buffer drained front to back is easier to be sure of. */
    glsl_token_t pending[GLSL_MAX_PENDING];
    int pending_head, pending_tail;

    /* **The token a directive read past its own line.** A directive's end is found by watching
     * the line number change, which means one token of the *next* line has already been taken
     * off the lexer by the time the directive is finished. It is parked here and served before
     * anything else, or it is silently dropped - which loses the first token after every
     * directive. */
    glsl_token_t held;
    GLboolean has_held;

    /* One entry per open conditional. `emitting` says whether this level's branch is the live
     * one; `taken` remembers whether any branch here has already been taken, so `#else` after
     * a true `#ifdef` stays dark. */
    GLboolean emitting[GLSL_MAX_COND_DEPTH];
    GLboolean taken[GLSL_MAX_COND_DEPTH];
    int cond_depth;

    int version;           /* from `#version`, or 0 if absent */
    const char *error;
    int error_line;
} glsl_pp_t;

void glsl_pp_init(glsl_pp_t *pp, const char *source, size_t length);
/* The next token after directives are obeyed and macros expanded. Returns GL_FALSE at EOF or
 * on error, having written GLSL_TOK_EOF or GLSL_TOK_ERROR. */
GLboolean glsl_pp_next(glsl_pp_t *pp, glsl_token_t *out);

void glsl_lexer_init(glsl_lexer_t *lx, const char *source, size_t length);
/* Writes the next token and returns GL_FALSE once GLSL_TOK_EOF has been produced, so a caller
 * can drive it with `while (glsl_lex_next(&lx, &t))`. An error token stops nothing: the caller
 * decides whether to keep going, which is what lets a compiler report more than one problem. */
GLboolean glsl_lex_next(glsl_lexer_t *lx, glsl_token_t *out);

/* Primes the lookahead, so the parser is ready to read immediately. `ast` is reset to empty. */
void glsl_parser_init(glsl_parser_t *p, glsl_ast_t *ast, const char *source, size_t length);
/* Parses one expression, including the comma operator, and returns its root node index -
 * or GLSL_NO_NODE with `p->error` set. It does not require the whole source to be consumed;
 * a caller wanting that checks `p->tok.type == GLSL_TOK_EOF` afterwards. */
int32_t glsl_parse_expression(glsl_parser_t *p);
/* One statement - compound, selection, iteration, jump, declaration or expression. */
int32_t glsl_parse_statement(glsl_parser_t *p);
/* A whole shader: external declarations until the source runs out. Returns a GLSL_NODE_UNIT,
 * or GLSL_NO_NODE with `p->error` set. */
int32_t glsl_parse_translation_unit(glsl_parser_t *p);

/* -------------------------------------------------------------------------
 * Types and the semantic stage
 *
 * The grammar accepts `vec3 + mat4` and `v.xyzw` on a `vec2` quite happily - they are both
 * well-formed binary expressions and field selections. Deciding that they are wrong is this
 * stage's job, and it is where a shader compiler earns most of its diagnostics.
 * ------------------------------------------------------------------------- */

typedef enum {
    GLSL_TYPE_ERROR = 0,   /* already reported; propagates without reporting again */
    GLSL_TYPE_VOID,
    GLSL_TYPE_BOOL, GLSL_TYPE_INT, GLSL_TYPE_FLOAT,
    GLSL_TYPE_VEC2, GLSL_TYPE_VEC3, GLSL_TYPE_VEC4,
    GLSL_TYPE_IVEC2, GLSL_TYPE_IVEC3, GLSL_TYPE_IVEC4,
    GLSL_TYPE_BVEC2, GLSL_TYPE_BVEC3, GLSL_TYPE_BVEC4,
    GLSL_TYPE_MAT2, GLSL_TYPE_MAT3, GLSL_TYPE_MAT4,
    GLSL_TYPE_SAMPLER1D, GLSL_TYPE_SAMPLER2D, GLSL_TYPE_SAMPLER3D,
    GLSL_TYPE_SAMPLERCUBE, GLSL_TYPE_SAMPLER1DSHADOW, GLSL_TYPE_SAMPLER2DSHADOW
} glsl_type_t;

#define GLSL_MAX_SYMBOLS 256

#define GLSL_MAX_PARAMS 8

typedef struct {
    const char *name;
    size_t name_len;
    glsl_type_t type;      /* a variable's type, or a function's return type */
    int scope;             /* the block depth it was declared at */
    GLboolean is_function;
    /* **The storage qualifier decides assignability.** `uniform`, `attribute` and `const` are
     * read-only to a shader, and the l-value check is the only place that matters - so it is
     * kept here rather than re-derived from the declaration node. */
    glsl_token_type_t qualifier;
    glsl_type_t params[GLSL_MAX_PARAMS];
    int param_count;
} glsl_symbol_t;

typedef struct {
    glsl_ast_t *ast;
    glsl_symbol_t symbols[GLSL_MAX_SYMBOLS];
    int count;
    int scope;
    const char *error;
    int error_line, error_column;
    /* Set while checking a function body, so `return` can be checked against it. */
    glsl_type_t current_return;
    int loop_depth;        /* so `break` and `continue` can be refused outside a loop */
} glsl_sema_t;

void glsl_sema_init(glsl_sema_t *s, glsl_ast_t *ast);
/* The type a token names, or GLSL_TYPE_ERROR if it names no type. */
glsl_type_t glsl_type_from_token(glsl_token_type_t t);
/* How many components a type has: 1 for a scalar, 2-4 for a vector, 4/9/16 for a matrix. */
int glsl_type_components(glsl_type_t t);
/* The type of an expression node, recording the first error on the way. */
glsl_type_t glsl_type_of(glsl_sema_t *s, int32_t node);
/* Declares a name in the current scope. False, with an error recorded, on a redeclaration. */
GLboolean glsl_declare(glsl_sema_t *s, const char *name, size_t len, glsl_type_t type,
                       GLboolean is_function);
/* -------------------------------------------------------------------------
 * The back end: RDNA2 instruction encoding
 *
 * Opcodes and field positions are read out of a real assembler, never written from memory -
 * `tools/shader/gl2-transform.s` holds the source and the tests assert the exact words clang
 * produced from it. A wrong encoding assembles into the payload and the hardware does something
 * else, so it cannot fail loudly on its own.
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t *words;
    uint32_t capacity;
    uint32_t count;
    GLboolean overflow;   /* recorded, never wrapped: a truncated shader is a valid stream */
} glsl_code_t;

/* VOP2 opcodes, from `v_mul_f32` = 0x10080108, `v_add_f32` = 0x06080908,
 * `v_fmac_f32` = 0x56080308 - the opcode is bits [30:25] of each. */
#define GLSL_VOP2_ADD_F32   3u
/* `v_sub_f32 v4, v8, v9` = 0x08081308. The next opcode, 5, is `v_subrev_f32` with the operands
 * the other way round, so an off-by-one here computes the negation of what was asked. */
#define GLSL_VOP2_SUB_F32   4u
#define GLSL_VOP2_MUL_F32   8u
#define GLSL_VOP2_FMAC_F32 43u
/* VOP1 opcode, from `v_mov_b32 v14, v4` = 0x7e1c0304 - bits [16:9]. */
#define GLSL_VOP1_MOV_B32   1u

void glsl_code_init(glsl_code_t *c, uint32_t *words, uint32_t capacity);
uint32_t glsl_vgpr(uint32_t n);
void glsl_emit_vop2(glsl_code_t *c, uint32_t opcode, uint32_t vdst, uint32_t src0,
                    uint32_t vsrc1);
void glsl_emit_vop1(glsl_code_t *c, uint32_t opcode, uint32_t vdst, uint32_t src0);
void glsl_emit_mul_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1);
void glsl_emit_add_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1);
/* `d = s0 - s1`, in that order - see the definition. */
void glsl_emit_sub_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1);
void glsl_emit_neg_f32(glsl_code_t *c, uint32_t d, uint32_t s);
void glsl_emit_fmac_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1);
void glsl_emit_mov(glsl_code_t *c, uint32_t d, uint32_t s);
void glsl_emit_mov_imm(glsl_code_t *c, uint32_t d, uint32_t bits);
void glsl_emit_endpgm(glsl_code_t *c);
/* `dst[0..3] = m * v`, column-major. `dst` must not overlap `v`. */
void glsl_emit_mat4_mul_vec4(glsl_code_t *c, uint32_t dst, uint32_t m, uint32_t v);

/* -------------------------------------------------------------------------
 * Instruction selection: the tree becomes instructions
 *
 * A value occupies consecutive VGPRs, one per component - a `float` is one, a `vec4` four, a
 * `mat4` sixteen laid out column-major. No packing and no component aliasing, so a swizzle is a
 * move rather than a reinterpretation of a register.
 *
 * Only the float family is generated, and only the operators whose opcodes have been read out of
 * a real assembler. Anything else sets `error` and emits nothing - see `glsl_gen.c`.
 * ------------------------------------------------------------------------- */

#define GLSL_MAX_VGPRS 256u
#define GLSL_GEN_MAX_VARS 64

typedef struct {
    uint32_t base;  /* the first VGPR of the run */
    int count;      /* how many consecutive registers, 0 when the value is an error */
} glsl_value_t;

typedef struct {
    const char *name;
    size_t name_len;
    glsl_value_t value;
    glsl_type_t type;
} glsl_gen_var_t;

typedef struct {
    glsl_ast_t *ast;
    glsl_sema_t *sema;
    glsl_code_t *code;
    uint32_t next_vgpr;   /* the bump allocator's cursor */
    uint32_t high_water;  /* the most registers ever live, which is what the stage must reserve */
    glsl_gen_var_t vars[GLSL_GEN_MAX_VARS];
    int var_count;
    const char *error;    /* the **first** failure, which stops everything after it */
    int error_line, error_column;
} glsl_gen_t;

void glsl_gen_init(glsl_gen_t *g, glsl_ast_t *ast, glsl_sema_t *sema, glsl_code_t *code);
/* One statement: a declaration, an expression statement, or a block of them. */
GLboolean glsl_gen_stmt(glsl_gen_t *g, int32_t node);
/* One expression, leaving its value in the registers the returned value names. */
glsl_value_t glsl_gen_expression(glsl_gen_t *g, int32_t node);
/* Give a name a register home without a declaration in the tree - how an attribute, a uniform or
 * a varying arrives once the shader interface is settled. */
glsl_value_t glsl_gen_declare_input(glsl_gen_t *g, const char *name, size_t len,
                                    glsl_type_t type);

void glsl_scope_push(glsl_sema_t *s);
void glsl_scope_pop(glsl_sema_t *s);
/* Checks a whole translation unit: declarations, function bodies, statements and l-values.
 * Returns GL_FALSE with `s->error` set on the first problem. */
GLboolean glsl_check_unit(glsl_sema_t *s, int32_t unit);
/* Whether an expression may appear on the left of an assignment. */
GLboolean glsl_is_lvalue(glsl_sema_t *s, int32_t node);

#endif /* __GLSL_INTERNAL_H__ */
