/*
 * oops-gl: the GLSL compiler's internal interface - lexer, preprocessor, parser,
 * semantic checks, built-in library, RDNA2 encoder and instruction selection.
 *
 * Scoped to GLSL 1.10, the language GL 2.0 defines, with 1.20 where a unit asks for it.
 * Later versions add keywords and drop others; accepting `in`/`out` as 1.30 storage
 * qualifiers in a 1.10 shader would agree with the wrong specification.
 */

#ifndef __GLSL_INTERNAL_H__
#define __GLSL_INTERNAL_H__

#include "gl_internal.h"

typedef enum {
    GLSL_TOK_EOF = 0,
    GLSL_TOK_ERROR,

    /* The preprocessor's own marker, which never reaches the lexer's consumers. It is
     * queued behind a macro's expansion and consumed by `raw_next` to mark where that
     * expansion ends; see `expand` in glsl_pp.c for why a flag will not do. `value`
     * carries the macro's index. */
    GLSL_TOK_PP_MACRO_END,

    GLSL_TOK_IDENTIFIER,
    GLSL_TOK_INTCONST,
    GLSL_TOK_FLOATCONST,

    /* Keywords. GLSL 1.10's reserved words are a closed set; anything outside it is an
     * identifier, which is why these are listed rather than matched by prefix. */
    GLSL_TOK_KW_ATTRIBUTE,
    GLSL_TOK_KW_CONST,
    GLSL_TOK_KW_UNIFORM,
    GLSL_TOK_KW_VARYING,
    GLSL_TOK_KW_BREAK,
    GLSL_TOK_KW_CONTINUE,
    GLSL_TOK_KW_DO,
    GLSL_TOK_KW_FOR,
    GLSL_TOK_KW_WHILE,
    GLSL_TOK_KW_IF,
    GLSL_TOK_KW_ELSE,
    GLSL_TOK_KW_DISCARD,
    GLSL_TOK_KW_RETURN,
    GLSL_TOK_KW_STRUCT,
    GLSL_TOK_KW_VOID,
    GLSL_TOK_KW_IN,
    GLSL_TOK_KW_OUT,
    GLSL_TOK_KW_INOUT,
    /* GLSL 1.20's qualifiers. `invariant` asks that a value computed the same way in
     * two shaders come out bit-identical; `centroid` moves a varying's sample point
     * inside the primitive under multisampling. Both lex in any version, and the parser
     * refuses them in a 1.10 shader by name. */
    GLSL_TOK_KW_INVARIANT,
    GLSL_TOK_KW_CENTROID,
    /* OpenGL ES 1.00's precision words. Desktop GL accepts them with no effect, so they
     * lex here for an ES shader to parse, and the parser drops them rather than
     * carrying them into the type system, where nothing could act on them. */
    GLSL_TOK_KW_PRECISION,
    GLSL_TOK_KW_LOWP,
    GLSL_TOK_KW_MEDIUMP,
    GLSL_TOK_KW_HIGHP,
    GLSL_TOK_KW_FLOAT,
    GLSL_TOK_KW_INT,
    GLSL_TOK_KW_BOOL,
    GLSL_TOK_KW_TRUE,
    GLSL_TOK_KW_FALSE,
    GLSL_TOK_KW_VEC2,
    GLSL_TOK_KW_VEC3,
    GLSL_TOK_KW_VEC4,
    GLSL_TOK_KW_IVEC2,
    GLSL_TOK_KW_IVEC3,
    GLSL_TOK_KW_IVEC4,
    GLSL_TOK_KW_BVEC2,
    GLSL_TOK_KW_BVEC3,
    GLSL_TOK_KW_BVEC4,
    GLSL_TOK_KW_MAT2,
    GLSL_TOK_KW_MAT3,
    GLSL_TOK_KW_MAT4,
    /* GLSL 1.20's non-square spellings. `mat2x2`, `mat3x3` and `mat4x4` are the same
     * words as the three above and lex to them, so there are six tokens rather than
     * nine. */
    GLSL_TOK_KW_MAT2X3,
    GLSL_TOK_KW_MAT2X4,
    GLSL_TOK_KW_MAT3X2,
    GLSL_TOK_KW_MAT3X4,
    GLSL_TOK_KW_MAT4X2,
    GLSL_TOK_KW_MAT4X3,
    GLSL_TOK_KW_SAMPLER1D,
    GLSL_TOK_KW_SAMPLER2D,
    GLSL_TOK_KW_SAMPLER3D,
    GLSL_TOK_KW_SAMPLERCUBE,
    GLSL_TOK_KW_SAMPLER1DSHADOW,
    GLSL_TOK_KW_SAMPLER2DSHADOW,
    /* Reserved by 1.10 for future use. Recognised so they are refused by name rather
     * than parsed as identifiers and failing later as an unexplained syntax error. */
    GLSL_TOK_KW_RESERVED,

    /* Punctuation and operators */
    GLSL_TOK_LPAREN,
    GLSL_TOK_RPAREN,
    GLSL_TOK_LBRACKET,
    GLSL_TOK_RBRACKET,
    GLSL_TOK_LBRACE,
    GLSL_TOK_RBRACE,
    GLSL_TOK_SEMICOLON,
    GLSL_TOK_COMMA,
    GLSL_TOK_DOT,
    GLSL_TOK_COLON,
    GLSL_TOK_QUESTION,
    GLSL_TOK_PLUS,
    GLSL_TOK_MINUS,
    GLSL_TOK_STAR,
    GLSL_TOK_SLASH,
    GLSL_TOK_PERCENT,
    GLSL_TOK_BANG,
    GLSL_TOK_ASSIGN,
    GLSL_TOK_LT,
    GLSL_TOK_GT,
    GLSL_TOK_LE,
    GLSL_TOK_GE,
    GLSL_TOK_EQ,
    GLSL_TOK_NE,
    GLSL_TOK_AND_AND,
    GLSL_TOK_OR_OR,
    GLSL_TOK_XOR_XOR,
    GLSL_TOK_INC,
    GLSL_TOK_DEC,
    GLSL_TOK_ADD_ASSIGN,
    GLSL_TOK_SUB_ASSIGN,
    GLSL_TOK_MUL_ASSIGN,
    GLSL_TOK_DIV_ASSIGN,
    GLSL_TOK_AMP,
    GLSL_TOK_PIPE,
    GLSL_TOK_CARET,
    GLSL_TOK_TILDE,

    /* `#`, which starts a preprocessor directive. The preprocessor is a separate stage
     * that consumes it, so a `#version` line is never mistaken for an operator. */
    GLSL_TOK_HASH
} glsl_token_type_t;

typedef struct {
    glsl_token_type_t type;
    const char *text; /* into the source, not a copy: the lexer owns no memory */
    size_t length;
    int line;   /* 1-based, as a compiler diagnostic quotes them */
    int column; /* 1-based, counted in bytes */
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
 * Nodes live in a caller-supplied arena and refer to each other by index, not pointer.
 * Indices survive the arena being copied or moved, are half the size, and a stale one
 * is a bounds check rather than a wild read in a freestanding binary.
 *
 * The arena is fixed and refuses when full: a shader needing more nodes is refused with
 * a diagnostic.
 * ------------------------------------------------------------------------- */

#define GLSL_MAX_NODES 2048

/* Struct limits, defined ahead of the parser because the grammar must know which words
 * are struct names before the type system runs; see `struct_name` on `glsl_parser_t`.
 */
#define GLSL_MAX_STRUCTS 16
#define GLSL_MAX_STRUCT_MEMBERS 16

/* How many arguments an array constructor may have, which is the longest array it can
 * build. The interpreter holds them all on the stack before declaring the array. */
#define GLSL_MAX_ARRAY_CTOR_ARGS 32
/* How large a struct may be, in components. The interpreter carries every value in a
 * fixed stack array of this width, so an oversized struct is refused at compile time
 * rather than copied partway. Twice a `mat4`, the largest built-in type. */
#define GLSL_MAX_STRUCT_COMPONENTS 32
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
    GLSL_NODE_COMPOUND, /* { ... }, statements chained from `a` through `sibling` */
    GLSL_NODE_IF,       /* if (`a`) `b` else `c` */
    GLSL_NODE_WHILE,    /* while (`a`) `b` */
    GLSL_NODE_DO_WHILE, /* do `a` while (`b`) */
    GLSL_NODE_FOR,      /* for (`a`; `b`; `c`) `d` */
    GLSL_NODE_RETURN,   /* return `a`, or `a` absent */
    GLSL_NODE_BREAK,
    GLSL_NODE_CONTINUE,
    GLSL_NODE_DISCARD,
    GLSL_NODE_EXPR_STMT, /* `a` ;  - `a` absent for the empty statement */

    /* Declarations */
    GLSL_NODE_DECL,  /* one declarator: name in text, `a` = initialiser, `array_size` =
                      * the size expression. `float a, b;` is two of these chained
                      * through `sibling`. */
    GLSL_NODE_PARAM, /* one function parameter */
    GLSL_NODE_FUNCTION, /* name in text, `b` = parameter chain, `c` = body.
                         * `c` absent means a prototype, not an empty body. */
    /* `struct Name { members };` - name in text, members chained from `a` as
     * GLSL_NODE_DECL. A declaration may follow the closing brace (`struct S { ... }
     * s;`), in which case the declarators are the siblings of this node, exactly as for
     * any other type. */
    GLSL_NODE_STRUCT_DEF,
    GLSL_NODE_UNIT /* the whole source: declarations chained from `a` */
} glsl_node_kind_t;

typedef struct {
    glsl_node_kind_t kind;
    glsl_token_type_t op; /* for UNARY/POSTFIX/BINARY/ASSIGN; GLSL_TOK_EOF otherwise */
    int32_t a, b, c, d;   /* children, GLSL_NO_NODE when absent. `d` exists for `for`,
                           * the one construct with four. */
    int32_t sibling;      /* next in a list - call arguments, statements, declarators,
                           * parameters, top-level declarations. GLSL_NO_NODE at the end. */
    /* Declarations and parameters: the type as written, as a token. Resolving it, and
     * deciding whether an identifier names a struct, is the semantic stage's job. */
    glsl_token_type_t type_tok;
    /* The type's name when the type is a struct (`type_tok` is then
     * `GLSL_TOK_IDENTIFIER`). Separate from `text`, which holds the declarator's own
     * name: `S s;` has two names. NULL for every built-in type. */
    const char *type_name;
    size_t type_name_len;
    glsl_token_type_t
        qualifier;      /* const/attribute/varying/uniform/in/out/inout, or EOF */
    int32_t array_size; /* the size expression, GLSL_NO_NODE if not an array */
    const char *text;   /* identifier or field name: into the source, never a copy */
    size_t length;
    double value; /* for INTCONST/FLOATCONST/BOOLCONST */
    /* Which function a CALL binds to, as a FUNCTION node index, decided by the semantic
     * pass. GLSL_NO_NODE on every other node and on a call to a built-in. Overloads
     * share a name, and only the semantic pass has the argument types to choose;
     * recording the choice keeps both back ends from resolving overloads again and
     * disagreeing. */
    int32_t resolved;
    int line, column;
} glsl_node_t;

typedef struct {
    glsl_node_t nodes[GLSL_MAX_NODES];
    int32_t count;
} glsl_ast_t;

/* Declared ahead of the parser, which may read its tokens through one. The structure
 * itself is below, with the rest of the preprocessor. */
struct glsl_pp;

typedef struct {
    glsl_lexer_t lx;
    glsl_token_t tok; /* one token of lookahead, which is all this grammar needs */
    glsl_ast_t *ast;
    const char *error; /* a literal, so there is nothing to free */
    int error_line, error_column;
    /* Where tokens come from. NULL reads the lexer above directly, as the parser's own
     * tests do; set, the preprocessor is read instead, so directives are obeyed and
     * macros expanded first. See `glsl_parser_init_pp`. */
    struct glsl_pp *pp;
    /* The shading language the source asked for: 110, 120, or 0 for not stated, which
     * enforces nothing so the front end's tests can parse either dialect. A real
     * compile always has a number: `glsl_parser_init_pp` reads `#version` before the
     * first real token. */
    int version;
    /* The struct names seen so far. `Foo bar;` and `foo * bar;` differ only in whether
     * `Foo` names a type, which no lookahead settles; `starts_declaration` asks this
     * list while parsing, before any semantic pass has run. The names point into the
     * source. Sema builds its own table afterwards and owns members and layout. */
    const char *struct_name[GLSL_MAX_STRUCTS];
    size_t struct_name_len[GLSL_MAX_STRUCTS];
    int struct_names;
} glsl_parser_t;

/* -------------------------------------------------------------------------
 * The preprocessor
 *
 * A token filter, not a text-to-text pass: the parser pulls tokens through it, so no
 * rewritten copy of the source is allocated and every token points into the original
 * text for diagnostics.
 *
 * GLSL 1.10's preprocessor: `#version`, object-like and function-like `#define` and
 * `#undef`, `#ifdef`/`#ifndef`/`#if`/`#elif`/`#else`/`#endif` with constant
 * expressions, `#error`, `#extension`, `#pragma` and `#line`.
 *
 * What is refused is refused by name, never skipped. `#extension ... : require` fails
 * because no extensions are implemented. `<<` and `>>` in a `#if` fail because
 * GLSL 1.10 has no shift token and they would otherwise read as two `<`.
 * ------------------------------------------------------------------------- */

#define GLSL_MAX_MACROS 64
#define GLSL_MAX_MACRO_TOKENS 1024
#define GLSL_MAX_COND_DEPTH 32
#define GLSL_MAX_PENDING 128

/* A function-like macro's parameter names; a body token matching one is replaced by the
 * argument at the same index. A ninth parameter is refused, not dropped. */
#define GLSL_MAX_MACRO_PARAMS 8

typedef struct {
    const char *name;
    size_t name_len;
    int32_t first_token; /* into the token pool */
    int32_t token_count;
    GLboolean in_use;    /* defined at all */
    GLboolean expanding; /* currently being expanded: stops `#define A A` looping */
    /* Function-like, which is not the same as taking no arguments: `#define F() x` has
     * zero parameters and still expands only as `F()`, while `#define F x` expands on
     * sight. So this cannot be inferred from `param_count`. */
    GLboolean function_like;
    int param_count;
    const char *param_name[GLSL_MAX_MACRO_PARAMS];
    size_t param_len[GLSL_MAX_MACRO_PARAMS];
} glsl_macro_t;

/* Tagged, so the parser above can hold a pointer to one before it is defined. */
typedef struct glsl_pp {
    glsl_lexer_t lx;

    glsl_macro_t macros[GLSL_MAX_MACROS];
    int macro_count;
    glsl_token_t pool[GLSL_MAX_MACRO_TOKENS];
    int pool_count;

    /* Tokens produced by expansion, served before the lexer is read again. A straight
     * buffer drained front to back, not a ring. */
    glsl_token_t pending[GLSL_MAX_PENDING];
    int pending_head, pending_tail;

    /* The token a directive read past its own line. A directive ends when the line
     * number changes, so the first token of the next line has already been lexed; it is
     * parked here and served before anything else. */
    glsl_token_t held;
    GLboolean has_held;

    /* One entry per open conditional. `emitting` says whether this level's branch is
     * the live one; `taken` remembers whether any branch here has already been taken,
     * so `#else` after a true `#ifdef` stays dark. */
    GLboolean emitting[GLSL_MAX_COND_DEPTH];
    GLboolean taken[GLSL_MAX_COND_DEPTH];
    int cond_depth;

    int version; /* from `#version`, or 0 if absent */
    const char *error;
    int error_line;
} glsl_pp_t;

void glsl_pp_init(glsl_pp_t *pp, const char *source, size_t length);
/* The next token after directives are obeyed and macros expanded. Returns GL_FALSE at
 * EOF or on error, having written GLSL_TOK_EOF or GLSL_TOK_ERROR. */
GLboolean glsl_pp_next(glsl_pp_t *pp, glsl_token_t *out);

void glsl_lexer_init(glsl_lexer_t *lx, const char *source, size_t length);
/* Writes the next token and returns GL_FALSE once GLSL_TOK_EOF has been produced, so a
 * caller can drive it with `while (glsl_lex_next(&lx, &t))`. An error token stops
 * nothing: the caller decides whether to keep going, which is what lets a compiler
 * report more than one problem. */
GLboolean glsl_lex_next(glsl_lexer_t *lx, glsl_token_t *out);

/* Primes the lookahead, so the parser is ready to read immediately. `ast` is reset to
 * empty. */
void glsl_parser_init(glsl_parser_t *p, glsl_ast_t *ast, const char *source,
                      size_t length);
/* The same, reading through a preprocessor the caller has initialised over the source,
 * so directives are obeyed rather than reaching the grammar as `#` tokens. It consumes
 * the directives before the first real token, so `pp->version` is known on return and
 * a caller may refuse a language before parsing it. The preprocessor must outlive the
 * parse; the parser does not own it. */
void glsl_parser_init_pp(glsl_parser_t *p, glsl_ast_t *ast, glsl_pp_t *pp);
/* Parses one expression, including the comma operator, and returns its root node index,
 * or GLSL_NO_NODE with `p->error` set. It does not require the whole source to be
 * consumed; a caller wanting that checks `p->tok.type == GLSL_TOK_EOF` afterwards. */
int32_t glsl_parse_expression(glsl_parser_t *p);
/* One statement: compound, selection, iteration, jump, declaration or expression. */
int32_t glsl_parse_statement(glsl_parser_t *p);
/* A whole shader: external declarations until the source runs out. Returns a
 * GLSL_NODE_UNIT, or GLSL_NO_NODE with `p->error` set. */
int32_t glsl_parse_translation_unit(glsl_parser_t *p);

/* -------------------------------------------------------------------------
 * Types and the semantic stage
 *
 * The grammar accepts `vec3 + mat4` and `v.xyzw` on a `vec2`, which are well-formed
 * expressions. Rejecting them is this stage's job, and it produces most diagnostics.
 * ------------------------------------------------------------------------- */

typedef enum {
    GLSL_TYPE_ERROR = 0, /* already reported; propagates without reporting again */
    GLSL_TYPE_VOID,
    GLSL_TYPE_BOOL,
    GLSL_TYPE_INT,
    GLSL_TYPE_FLOAT,
    GLSL_TYPE_VEC2,
    GLSL_TYPE_VEC3,
    GLSL_TYPE_VEC4,
    GLSL_TYPE_IVEC2,
    GLSL_TYPE_IVEC3,
    GLSL_TYPE_IVEC4,
    GLSL_TYPE_BVEC2,
    GLSL_TYPE_BVEC3,
    GLSL_TYPE_BVEC4,
    /* `matCxR` is C columns of R rows, GLSL's order: `mat2x3` is two columns of three,
     * and `matCxR * vecC -> vecR`. The square types keep their short names, as the
     * language does (`mat3` is `mat3x3`). The six non-square ones are contiguous with
     * them so `is_matrix` is a range check; sizes come from a table, not enum
     * arithmetic, so inserting a type cannot break them. */
    GLSL_TYPE_MAT2,
    GLSL_TYPE_MAT3,
    GLSL_TYPE_MAT4,
    GLSL_TYPE_MAT2X3,
    GLSL_TYPE_MAT2X4,
    GLSL_TYPE_MAT3X2,
    GLSL_TYPE_MAT3X4,
    GLSL_TYPE_MAT4X2,
    GLSL_TYPE_MAT4X3,
    GLSL_TYPE_SAMPLER1D,
    GLSL_TYPE_SAMPLER2D,
    GLSL_TYPE_SAMPLER3D,
    GLSL_TYPE_SAMPLERCUBE,
    GLSL_TYPE_SAMPLER1DSHADOW,
    GLSL_TYPE_SAMPLER2DSHADOW,

    /* A user-defined struct is a type value in the same enum: `GLSL_TYPE_STRUCT_BASE +
     * i` for the `i`th struct the unit declared. Reserving a range keeps a type one
     * integer everywhere it is carried, and only code that must distinguish a struct
     * asks. The base leaves room, so adding a built-in type never renumbers a struct.
     */
    GLSL_TYPE_STRUCT_BASE = 64
} glsl_type_t;

static inline GLboolean glsl_type_is_struct(glsl_type_t t) {
    return (GLboolean)(t >= GLSL_TYPE_STRUCT_BASE &&
                       t < (glsl_type_t)(GLSL_TYPE_STRUCT_BASE + GLSL_MAX_STRUCTS));
}
static inline int glsl_struct_index(glsl_type_t t) {
    return (int)t - (int)GLSL_TYPE_STRUCT_BASE;
}
static inline glsl_type_t glsl_struct_type(int index) {
    return (glsl_type_t)((int)GLSL_TYPE_STRUCT_BASE + index);
}

/* Writes `why` into a GL info log as "line:column: why", or bare when `line` is 0. A
 * NULL `why` leaves the log empty. */
static inline void glsl_log_write(char *log, size_t cap, const char *why, int line,
                                  int column) {
    if (!log || cap == 0u)
        return;
    if (!why) {
        log[0] = '\0';
        return;
    }
    if (line > 0) {
        oops_snprintf(log, cap, "%d:%d: %s", line, column, why);
    } else {
        oops_snprintf(log, cap, "%s", why);
    }
}

/* One member of a struct. `array_size` is 0 for a plain member, mirroring
 * `glsl_symbol_t`. */
typedef struct {
    const char *name;
    size_t name_len;
    glsl_type_t type;
    int array_size;
    /* Where this member starts inside the struct, in components. Both back ends lay a
     * struct out as its members end to end, so one offset serves the interpreter's
     * float array and the code generator's register run. */
    int offset;
} glsl_struct_member_t;

typedef struct {
    const char *name;
    size_t name_len;
    glsl_struct_member_t member[GLSL_MAX_STRUCT_MEMBERS];
    int member_count;
    int components; /* the whole struct, in components */
} glsl_struct_t;

#define GLSL_MAX_SYMBOLS 256

#define GLSL_MAX_PARAMS 8

typedef struct {
    const char *name;
    size_t name_len;
    glsl_type_t type; /* a variable's type, or a function's return type */
    int scope;        /* the block depth it was declared at */
    GLboolean is_function;
    /* The storage qualifier, which decides assignability: `uniform`, `attribute` and
     * `const` are read-only, and the l-value check reads it here. */
    glsl_token_type_t qualifier;
    /* How many elements, or 0 for a name that is not an array. GLSL 1.10 has one level
     * of array and no array-valued expressions, so a length and an element type in
     * `type` say everything; only indexing reads it. */
    int array_size;
    /* A `const int`'s value, for an array's length, which GLSL 4.1.9 requires to be an
     * integral constant expression (`const int N = 8; uniform vec2 offs[N];`). Set only
     * for `const` integer scalars with a foldable initialiser, the set the
     * specification allows there. */
    GLboolean has_const_int;
    int const_int;
    glsl_type_t params[GLSL_MAX_PARAMS];
    int param_count;
    /* Which FUNCTION node this symbol is, so a resolved call can name it among
     * overloads. GLSL_NO_NODE for a variable. */
    int32_t decl_node;
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
    int loop_depth; /* so `break` and `continue` can be refused outside a loop */
    /* Which stage this unit is, GL_VERTEX_SHADER or GL_FRAGMENT_SHADER, or 0 for a
     * caller that does not say. It decides whether `dFdx` and `texture2DLod` exist;
     * each is an error in the wrong stage, and 0 refuses neither, as the front end's
     * tests need. */
    GLenum stage;
    /* The shading language this unit asked for: 110 or 120, or 0 for a caller that does
     * not say, which behaves as the stricter 1.10. It decides implicit conversion:
     * 1.10 converts nothing (`1 + 1.0` is an error); 1.20 converts int to float and
     * `ivecN` to `vecN`, and nothing else. */
    int version;
    /* The structs this unit declared, in declaration order: type value
     * `GLSL_TYPE_STRUCT_BASE + i` names entry `i`. Not in the symbol table, because a
     * struct type is only looked up for a member's type and offset. */
    glsl_struct_t structs[GLSL_MAX_STRUCTS];
    int struct_count;
} glsl_sema_t;

/* The struct a type names, or NULL when it names anything else. */
const glsl_struct_t *glsl_struct_of(const glsl_sema_t *s, glsl_type_t t);
/* The member `name` of struct type `t`, or NULL. */
const glsl_struct_member_t *glsl_struct_member(const glsl_sema_t *s, glsl_type_t t,
                                               const char *name, size_t len);
/* The size of any type in components, including a struct, whose size lives in the
 * sema's table. Every caller that may see a struct asks this, not
 * `glsl_type_components`. */
int glsl_type_components_of(const glsl_sema_t *s, glsl_type_t t);

void glsl_sema_init(glsl_sema_t *s, glsl_ast_t *ast);
/* The type a token names, or GLSL_TYPE_ERROR if it names no type. */
glsl_type_t glsl_type_from_token(glsl_token_type_t t);
/* How many components a type has: 1 for a scalar, 2-4 for a vector, 4/9/16 for a
 * matrix. */
int glsl_type_components(glsl_type_t t);
/* The diagnostic sink and type predicates, shared with the built-in library so each
 * rule has one definition, in glsl_sema.c. */
void glsl_sema_fail(glsl_sema_t *s, const char *why, int32_t node);
GLboolean glsl_type_is_vector(glsl_type_t t);
GLboolean glsl_type_is_matrix(glsl_type_t t);
GLboolean glsl_type_is_sampler(glsl_type_t t);
/* A vector or matrix's component type; a scalar's own type. */
glsl_type_t glsl_type_base(glsl_type_t t);
/* The n-component vector of a base type; `vector_of(FLOAT, 1)` is FLOAT itself. */
glsl_type_t glsl_type_vector_of(glsl_type_t base, int n);
/* A matrix's two sizes: `matCxR` is C columns of R rows. Both are 0 for a type that is
 * not a matrix. Stride and bound are separate questions unless C == R. */
int glsl_type_matrix_cols(glsl_type_t t);
int glsl_type_matrix_rows(glsl_type_t t);
/* The matrix type with these dimensions, or ERROR. */
glsl_type_t glsl_type_matrix_of(int cols, int rows);
/* Whether a value of `got` may be used where `want` is expected: the one place the
 * implicit-conversion rule lives. Exact equality always, plus int to float and `ivecN`
 * to `vecN` in GLSL 1.20. Every assignment, initialiser, argument and return uses it.
 */
GLboolean glsl_type_accepts(const glsl_sema_t *s, glsl_type_t want, glsl_type_t got);
/* The type of an expression node, recording the first error on the way. */
glsl_type_t glsl_type_of(glsl_sema_t *s, int32_t node);
/* Declares a name in the current scope. False, with an error recorded, on a
 * redeclaration. */
GLboolean glsl_declare(glsl_sema_t *s, const char *name, size_t len, glsl_type_t type,
                       GLboolean is_function);
/* The same, for a name that is an array of `count` elements of `type`. `count` of 0
 * declares a plain variable, so a caller building a table does not need two calls. */
GLboolean glsl_declare_array(glsl_sema_t *s, const char *name, size_t len,
                             glsl_type_t type, int count, glsl_token_type_t qualifier);
/* The same, for a `const int` whose value is known, which an array's length and a
 * loop's bound may both be. GLSL 7.4's built-in constants come in this way. */
GLboolean glsl_declare_const_int(glsl_sema_t *s, const char *name, size_t len,
                                 int value);
/* Whether this CALL node is a GLSL 1.20 array constructor `T[N](a, b, ...)` (a call
 * whose callee indexes a type name), and if so its element type and length. It has no
 * expression type, since a symbol carries an array's length, so it is usable only as a
 * declaration's initialiser. */
GLboolean glsl_array_ctor_of(glsl_sema_t *s, int32_t node, glsl_type_t *elem,
                             int *count);
/* The same question asked of the tree alone, for the back ends: sema has already folded
 * the length to an `INTCONST`. */
GLboolean glsl_array_ctor_shape(const glsl_ast_t *ast, int32_t node, glsl_type_t *elem,
                                int *count);
/* The element type and length of a node naming a whole array (`a`, or a struct member
 * `s.w` with a length), or false. The type system has no array type, so any rule
 * needing both halves asks this. */
GLboolean glsl_whole_array_info(glsl_sema_t *s, int32_t node, glsl_type_t *elem,
                                int *size);

/* -------------------------------------------------------------------------
 * The built-in library
 *
 * GLSL's built-in functions are overloaded over `genType` (`sin` takes and returns a
 * float or any vec), which one signature per name in the symbol table cannot express.
 * They are resolved by rule ahead of the table, as constructors are.
 *
 * `glsl_builtin_call_type` answers the result type of a call to a built-in. When the
 * name is not a built-in it sets `*found` false and records nothing, so the caller
 * tries the symbol table; when a built-in is called wrongly it sets `*found` true and
 * returns GLSL_TYPE_ERROR with a diagnostic recorded.
 * ------------------------------------------------------------------------- */
glsl_type_t glsl_builtin_call_type(glsl_sema_t *s, const char *name, size_t len,
                                   const glsl_type_t *args, int argc, int32_t node,
                                   GLboolean *found);
/* Declares GLSL 1.10's built-in variables and uniforms for one stage: `gl_Position`,
 * `gl_FragColor` and the fixed-function state a 1.10 shader may read. Called before
 * `glsl_check_unit`, since a shader may use one without declaring it. */
GLboolean glsl_declare_builtins(glsl_sema_t *s, GLenum stage);
/* Records one `GLSL_NODE_FUNCTION`'s return and parameter types, so a call to it can be
 * typed. `glsl_check_unit` does this for every function; a caller building its own
 * symbol table, such as the pixel-shader back end, must do it too. */
GLboolean glsl_declare_function(glsl_sema_t *s, int32_t node);

/* Whether a unit names this identifier anywhere, including untaken branches and
 * uncalled functions; it reads every node of the flat AST once. The over-answer is
 * deliberate: the linker sizes the parameter block and the back end emits its prologue
 * before generating, and an unneeded input costs one parameter where a missing one
 * reads a register nothing filled. */
GLboolean glsl_unit_mentions(const glsl_unit_t *u, const char *name, size_t len);
/* Whether the unit contains a `discard`, a keyword `glsl_unit_mentions` cannot find. */
GLboolean glsl_unit_discards(const glsl_unit_t *u);
/* Why a real GLSL `gl_` name is not declared here, or NULL for a name this does not
 * know. It turns "undeclared name" into a message naming the missing feature. */
const char *glsl_builtin_refusal(const char *name, size_t len);
/* The value of a built-in constant (7.4), or false for any other name. Both back ends
 * make it a literal, and each value is what the matching `glGetIntegerv` answers. */
GLboolean glsl_builtin_const_int(const char *name, size_t len, int *out);
/* -------------------------------------------------------------------------
 * The back end: RDNA2 instruction encoding
 *
 * Opcodes and field positions are read out of a real assembler, never written from
 * memory: `tools/shader/gl2-transform.s` holds the source and the tests assert the
 * exact words clang produced from it. A wrong encoding does not fail loudly.
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t *words;
    uint32_t capacity;
    uint32_t count;
    GLboolean
        overflow; /* recorded, never wrapped: a truncated shader is a valid stream */
} glsl_code_t;

/* VOP2 opcodes, from `v_mul_f32` = 0x10080108, `v_add_f32` = 0x06080908,
 * `v_fmac_f32` = 0x56080308; the opcode is bits [30:25] of each. */
#define GLSL_VOP2_ADD_F32 3u
/* `v_sub_f32 v4, v8, v9` = 0x08081308. The next opcode, 5, is `v_subrev_f32` with the
 * operands the other way round, so an off-by-one here computes the negation of what was
 * asked. */
#define GLSL_VOP2_SUB_F32 4u
#define GLSL_VOP2_MUL_F32 8u
#define GLSL_VOP2_FMAC_F32 43u
#define GLSL_VOP2_MIN_F32 15u
#define GLSL_VOP2_MAX_F32 16u
/* `d = vcc_lo ? vsrc1 : src0`: the false value comes first. Boolean `mix` and `?:` both
 * use it. From `v_cndmask_b32_e32 v4, v5, v6, vcc_lo` = 0x02080d05. */
#define GLSL_VOP2_CNDMASK 1u

/* VOP1 opcode, from `v_mov_b32 v14, v4` = 0x7e1c0304, bits [16:9]. */
#define GLSL_VOP1_MOV_B32 1u
/* The transcendentals, from `tools/shader/gl2-fragment.s`. `v_rcp_f32` is a
 * reciprocal, not a divide: GLSL's `/` is a reciprocal and a multiply. `v_exp_f32` and
 * `v_log_f32` are base two, not base e: `exp(x)` is `exp2(x * log2 e)`. */
#define GLSL_VOP1_FRACT_F32 32u
#define GLSL_VOP1_TRUNC_F32 33u
#define GLSL_VOP1_CEIL_F32 34u
#define GLSL_VOP1_FLOOR_F32 36u
#define GLSL_VOP1_EXP_F32 37u /* 2^x */
#define GLSL_VOP1_LOG_F32 39u /* log2 x */
#define GLSL_VOP1_RCP_F32 42u
#define GLSL_VOP1_RSQ_F32 46u
#define GLSL_VOP1_SQRT_F32 51u
#define GLSL_VOP1_SIN_F32 53u
#define GLSL_VOP1_COS_F32 54u

/* VOPC opcodes, from the same file. `v_cmp_neq_f32` is the unordered not-equal, which
 * is GLSL's `!=`: a NaN is not equal to anything, including itself. */
#define GLSL_VOPC_LT_F32 1u
#define GLSL_VOPC_EQ_F32 2u
#define GLSL_VOPC_LE_F32 3u
#define GLSL_VOPC_GT_F32 4u
#define GLSL_VOPC_GE_F32 6u
#define GLSL_VOPC_NEQ_F32 13u

/* SMEM opcodes: the five load widths, which are consecutive. From
 * `tools/shader/gl2-fragment.s`: `s_load_dword s4, s[0:1], 0x0` = 0xf4000100
 * 0xfa000000, and each wider form is the opcode field (bits 25:18) one higher. Uniforms
 * reach a compiled pixel shader this way: the draw puts a block's address in the first
 * user SGPR pair and the shader loads from it. */
#define GLSL_SMEM_LOAD_DWORD 0u
#define GLSL_SMEM_LOAD_DWORDX2 1u
#define GLSL_SMEM_LOAD_DWORDX4 2u
#define GLSL_SMEM_LOAD_DWORDX8 3u
#define GLSL_SMEM_LOAD_DWORDX16 4u

void glsl_code_init(glsl_code_t *c, uint32_t *words, uint32_t capacity);
uint32_t glsl_vgpr(uint32_t n);
/* An SGPR as a source operand: the identity, spelled out so a call site says which file
 * it means. See `glsl_emit.c`. */
uint32_t glsl_sgpr(uint32_t n);
/* `sbase_pair` is an SGPR pair index: the address in s[0:1] is 0. `offset` is in
 * bytes. */
void glsl_emit_s_load(glsl_code_t *c, uint32_t op, uint32_t sdata, uint32_t sbase_pair,
                      uint32_t offset);
void glsl_emit_s_waitcnt_lgkm(glsl_code_t *c);

/* Scalar registers outside the general file. Wave32, so the mask registers are the low
 * halves; `vcc` and `exec` are the 64-bit names and encode differently. */
#define GLSL_SREG_VCC_LO 106u
#define GLSL_SREG_EXEC_LO 126u
/* `m0`, which a pixel shader sets once and never reads: see `glsl_emit_s_mov_m0`. */
#define GLSL_SREG_M0 124u

/* SOP1 and SOP2 opcodes, from `tools/shader/gl2-fragment.s`:
 * `s_mov_b32 exec_lo, s4` = 0xbefe0304, `s_and_saveexec_b32 s4, vcc_lo` = 0xbe843c6a,
 * `s_andn2_b32 exec_lo, s4, exec_lo` = 0x8a7e7e04, and `s_and_b32` from the word
 * already in the tree, 0x877e6a7e. */
#define GLSL_SOP1_MOV_B32 3u
/* `s_wqm_b32 exec_lo, exec_lo` = 0xbefe097e: whole-quad mode, for a sample's
 * derivatives. */
#define GLSL_SOP1_WQM_B32 9u
#define GLSL_SOP1_AND_SAVEEXEC_B32 60u
#define GLSL_SOP2_AND_B32 14u
#define GLSL_SOP2_ANDN2_B32 20u
/* `s_add_u32 s20, s20, 1` = 0x80148114, from `tools/shader/branch.s`: the trip
 * counter's step. */
#define GLSL_SOP2_ADD_U32 0u

void glsl_emit_sop1(glsl_code_t *c, uint32_t op, uint32_t sdst, uint32_t ssrc0);
void glsl_emit_sop2(glsl_code_t *c, uint32_t op, uint32_t sdst, uint32_t ssrc0,
                    uint32_t ssrc1);
/* An `if` uses no branch (see `glsl_emit.c`): a body run with `exec` zero writes
 * nothing. Only loops need the branches below. */

/* -------------------------------------------------------------------------
 * Branches
 *
 * SOPP: `101111111 op[22:16] simm16[15:0]`, the family `s_nop` and `s_endpgm` are
 * already in. Words from `tools/shader/branch.s`: `s_branch` back over one instruction
 * = 0xbf82fffe, `s_cbranch_execz` +4 = 0xbf880004, `s_cbranch_scc1` +3 = 0xbf850003.
 * ------------------------------------------------------------------------- */
#define GLSL_SOPP_BRANCH 2u
#define GLSL_SOPP_CBRANCH_SCC1 5u
/* Taken when every lane has left, so a narrowed loop does not run its body for nobody.
 */
#define GLSL_SOPP_CBRANCH_EXECZ 8u

/* SOPC: `101111110 op[22:16] ssrc1[15:8] ssrc0[7:0]`, from `s_cmp_ge_u32 s20, 0x100`
 * = 0xbf09ff14 + literal. Sets SCC, which `s_cbranch_scc1` reads. */
#define GLSL_SOPC_CMP_GE_U32 9u

void glsl_emit_sopp(glsl_code_t *c, uint32_t op, uint32_t simm16);
void glsl_emit_sopc(glsl_code_t *c, uint32_t op, uint32_t ssrc0, uint32_t ssrc1);
/* The index the next emitted word will take: a label. */
uint32_t glsl_code_here(const glsl_code_t *c);
/* A branch to a word already emitted. `target` comes from an earlier `glsl_code_here`.
 */
void glsl_emit_branch_back(glsl_code_t *c, uint32_t op, uint32_t target);
/* A branch to a word not emitted yet: returns the index to pass to
 * `glsl_patch_branch_here`. */
uint32_t glsl_emit_branch_fwd(glsl_code_t *c, uint32_t op);
/* Points a forward branch at the next word. False means the buffer overflowed in
 * between, so the offset cannot be trusted and the compile fails. */
GLboolean glsl_patch_branch_here(glsl_code_t *c, uint32_t at);
/* The trip guard: a counter that ends a loop whatever the lanes are doing. */
void glsl_emit_s_inc_u32(glsl_code_t *c, uint32_t sreg);
void glsl_emit_s_cmp_ge_u32_imm(glsl_code_t *c, uint32_t sreg, uint32_t imm);
/* MIMG opcodes, from `tools/shader/gl2-fragment.s`: `image_sample` = 0xf0800f08 and
 * `image_sample_lz` = 0xf09c0f08, the opcode being bits 24:18 of the first word.
 * `_lz` samples level zero only, ignoring the mip chain, the minification filter and
 * GL 1.4's LOD bias. */
#define GLSL_MIMG_SAMPLE 32u
/* `image_sample_b`: clang 21 for gfx1030 assembles `image_sample_b v[4:7], v[16:18],
 * s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D` to 0xF0940F08, against the plain
 * sample's 0xF0800F08. At `SQ_RSRC_IMG_CUBE` it takes `v[16:19]`, one more than the
 * cube's three coordinates: the bias goes first in the address run. */
#define GLSL_MIMG_SAMPLE_B 37u
#define GLSL_MIMG_SAMPLE_LZ 39u
/* The comparing form, which returns one value rather than a texel: the sampler's
 * `DEPTH_COMPARE_FUNC` is applied per texel against a reference in the first address
 * register, ahead of s and t. From `tools/shader/tex-shadow.s`, where
 * `image_sample_c ... dmask:0x1` is 0xf0a00108. */
#define GLSL_MIMG_SAMPLE_C 40u

/* Cube face selection: `v_cubeid_f32` names the face, `v_cubesc_f32` and
 * `v_cubetc_f32` give the two coordinates on it, and `v_cubema_f32` gives twice the
 * major axis to divide them by. All four read x, y and z, so they are VOP3 only.
 * Opcodes from `tools/shader/tex-cube.s`: 0xd5440013, 0xd5450014, 0xd5460015,
 * 0xd5470016, the opcode being bits 25:16; the sequence ACO emits. */
#define GLSL_VOP3_CUBEID_F32 0x144u
#define GLSL_VOP3_CUBESC_F32 0x145u
#define GLSL_VOP3_CUBETC_F32 0x146u
#define GLSL_VOP3_CUBEMA_F32 0x147u
/* A VOP3 source names the whole operand space, so a VGPR is 256 plus its number. */
#define GLSL_VOP3_VGPR(n) (256u + (n))
void glsl_emit_vop3(glsl_code_t *c, uint32_t op, uint32_t vdst, uint32_t src0,
                    uint32_t src1, uint32_t src2);

/* The `dim` field, bits 5:3 of the first word. */
#define GLSL_IMG_DIM_1D 0u
#define GLSL_IMG_DIM_2D 1u
#define GLSL_IMG_DIM_3D 2u
#define GLSL_IMG_DIM_CUBE 3u

/* `srsrc` and `ssamp` are the first SGPR of the descriptor group; the encoder
 * divides by four. `vaddr` is the first of a consecutive run holding the coordinate,
 * `vdata` the first of the four the sample returns. */
void glsl_emit_image_sample_masked(glsl_code_t *c, uint32_t opcode, uint32_t dim,
                                   uint32_t dmask, uint32_t vdata, uint32_t vaddr,
                                   uint32_t srsrc, uint32_t ssamp);
void glsl_emit_image_sample(glsl_code_t *c, uint32_t opcode, uint32_t dim,
                            uint32_t vdata, uint32_t vaddr, uint32_t srsrc,
                            uint32_t ssamp);
void glsl_emit_s_waitcnt_vm(glsl_code_t *c);
void glsl_emit_wqm(glsl_code_t *c);
void glsl_emit_exec_save(glsl_code_t *c, uint32_t saved);

void glsl_emit_exec_save_and_vcc(glsl_code_t *c, uint32_t saved);
void glsl_emit_exec_else(glsl_code_t *c, uint32_t saved);
void glsl_emit_exec_restore(glsl_code_t *c, uint32_t saved);
void glsl_emit_exec_drop_live(glsl_code_t *c, uint32_t saved);
void glsl_emit_exec_clear(glsl_code_t *c);
void glsl_emit_vop2(glsl_code_t *c, uint32_t opcode, uint32_t vdst, uint32_t src0,
                    uint32_t vsrc1);
void glsl_emit_vop1(glsl_code_t *c, uint32_t opcode, uint32_t vdst, uint32_t src0);
void glsl_emit_mul_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1);
void glsl_emit_add_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1);
/* `d = s0 - s1`, in that order; see the definition. */
void glsl_emit_sub_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1);
void glsl_emit_neg_f32(glsl_code_t *c, uint32_t d, uint32_t s);
void glsl_emit_fmac_f32(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1);
void glsl_emit_mov(glsl_code_t *c, uint32_t d, uint32_t s);
void glsl_emit_mov_imm(glsl_code_t *c, uint32_t d, uint32_t bits);
void glsl_emit_endpgm(glsl_code_t *c);
void glsl_emit_nop(glsl_code_t *c);
/* `dst[0..rows-1] = m * v` for a `cols` x `rows` matrix, column-major, where `v` has
 * `cols` components. `dst` must not overlap `m` or `v`. */
void glsl_emit_mat_mul_vec_cr(glsl_code_t *c, uint32_t dst, uint32_t m, uint32_t v,
                              uint32_t cols, uint32_t rows);
/* `dst[0..n-1] = m * v` for an n x n matrix, column-major. `dst` must not overlap `m`
 * or `v`. */
void glsl_emit_mat_mul_vec(glsl_code_t *c, uint32_t dst, uint32_t m, uint32_t v,
                           uint32_t n);
/* `dst[0..3] = m * v`, column-major. `dst` must not overlap `v`. */
void glsl_emit_mat4_mul_vec4(glsl_code_t *c, uint32_t dst, uint32_t m, uint32_t v);
/* `dst[0..cols-1] = v * m`, the product with the transpose, not `m * v`: `v` has `rows`
 * components and the result has one per column. */
void glsl_emit_vec_mul_mat_cr(glsl_code_t *c, uint32_t dst, uint32_t v, uint32_t m,
                              uint32_t cols, uint32_t rows);
/* `dst[0..n-1] = v * m`, the product with the transpose, not `m * v`. */
void glsl_emit_vec_mul_mat(glsl_code_t *c, uint32_t dst, uint32_t v, uint32_t m,
                           uint32_t n);

/* One operand, one result: `v_sqrt_f32` and the rest of the VOP1 table above. */
void glsl_emit_vop1_op(glsl_code_t *c, uint32_t opcode, uint32_t d, uint32_t s);
/* Two operands: `v_min_f32`, `v_max_f32` and the rest of VOP2. */
void glsl_emit_vop2_op(glsl_code_t *c, uint32_t opcode, uint32_t d, uint32_t s0,
                       uint32_t s1);
/* `d = vcc_lo ? s1 : s0`. The false value is the first one. */
void glsl_emit_cndmask(glsl_code_t *c, uint32_t d, uint32_t s0, uint32_t s1);
/* A comparison into `vcc_lo`, and the lane kill that reads it. `discard` is the pair,
 * as are the alpha test and the polygon stipple. */
void glsl_emit_cmp(glsl_code_t *c, uint32_t opcode, uint32_t s0, uint32_t s1);
void glsl_emit_kill_from_vcc(glsl_code_t *c);

/* -------------------------------------------------------------------------
 * The pixel shader's two ends
 *
 * A varying reaches a fragment as a parameter, interpolated by two instructions against
 * the barycentrics the hardware puts in v0 and v1. Exporting the result takes one.
 * Neither depends on the GLSL; they frame the compiled body.
 * ------------------------------------------------------------------------- */

/* `m0` addresses the parameter cache, and every `v_interp` reads it. The SPI hands the
 * wave its primitive mask in the scalar register just past the user data, so `ssrc` is
 * s2 for a shader taking the block's address in s[0:1] and s0 for one with no user
 * data. Without it `m0` holds whatever the slot's last wave left, and some waves
 * interpolate another primitive's parameters: a fine per-wave stipple. The hand-written
 * pixel shaders in `gl_context.c` (`ps_untex`, `ps_tex`) set it the same way. */
void glsl_emit_s_mov_m0(glsl_code_t *c, uint32_t ssrc);
/* One component of one parameter into `vdst`: `p2` false for the first half of the pair
 * and true for the second, which must follow it immediately. `attr` is 0..31 and `chan`
 * 0..3. `glsl_emit_s_mov_m0` has to have run first. */
void glsl_emit_interp(glsl_code_t *c, uint32_t vdst, uint32_t attr, uint32_t chan,
                      GLboolean p2);
/* Both halves for one component, which is what a caller always wants. */
void glsl_emit_interp_pair(glsl_code_t *c, uint32_t vdst, uint32_t attr, uint32_t chan);
/* `exp mrt0 v[base..base+3] done vm`: the colour, and the end of the shader's exports.
 */
void glsl_emit_export_mrt0(glsl_code_t *c, uint32_t base);
/* The depth a shader wrote, exported before the colour; see the definition for why it
 * carries neither `done` nor `vm`. */
void glsl_emit_export_mrtz(glsl_code_t *c, uint32_t reg);

/* The quad permutes a derivative needs, as `quad_perm` control values. A quad is laid
 * out (0,0) (1,0) / (0,1) (1,1), so the x pairs are lanes 0-1 and 2-3 and the y pairs
 * 0-2 and 1-3. Each broadcasts one side of a pair across the quad; the difference of
 * two is the derivative. All four are checked against the assembler. */
#define GLSL_DPP_QUAD_X_FAR 0xf5u  /* [1,1,3,3], the right-hand column */
#define GLSL_DPP_QUAD_X_NEAR 0xa0u /* [0,0,2,2], the left-hand column */
#define GLSL_DPP_QUAD_Y_FAR 0xeeu  /* [2,3,2,3], the bottom row */
#define GLSL_DPP_QUAD_Y_NEAR 0x44u /* [0,1,0,1], the top row */

/* `dst = src`, read through a quad permute. */
void glsl_emit_dpp_mov(glsl_code_t *c, uint32_t dst, uint32_t src, uint32_t ctrl);
/* `dst = perm(src0) - vsrc1`. Only `src0` can be permuted, which is why a derivative
 * takes a move and a subtract rather than one instruction. */
void glsl_emit_dpp_sub(glsl_code_t *c, uint32_t dst, uint32_t src0, uint32_t vsrc1,
                       uint32_t ctrl);

/* -------------------------------------------------------------------------
 * Instruction selection: the tree becomes instructions
 *
 * A value occupies consecutive VGPRs, one per component: a `float` is one, a `vec4`
 * four, a `mat4` sixteen laid out column-major. No packing and no component aliasing,
 * so a swizzle is a move.
 *
 * Only the float family is generated, with opcodes read out of a real assembler.
 * Anything else sets `error` and emits nothing; see `glsl_gen.c`.
 * ------------------------------------------------------------------------- */

#define GLSL_MAX_VGPRS 256u
#define GLSL_GEN_MAX_VARS 64

/*
 * The scalar file, as a compiled pixel shader divides it:
 *
 *   s0..s1    the uniform block's address, handed over as user data
 *   s2        the primitive mask, moved into `m0` by the prologue (s0 when the shader
 *             takes no block, and then s[0:1] is not an address)
 *   s4..      texture sets, 12 each: image descriptor at +0, sampler at +8
 *   LIVE      the live-lane mask, kept across a whole-quad section
 *   EXEC..    an `if`'s saved exec mask, one per nesting level
 *   LOOP..    a branched loop's three masks, one set per nesting level
 *   then      the draw constants and the uniforms, placed by `glsl_ps.c`
 *
 * A scalar load of four dwords or more needs a 4-aligned destination whatever its
 * width, and MIMG's `srsrc` and `ssamp` hold the register number divided by four, so
 * every descriptor and load group starts on a multiple of four.
 */
#define GLSL_GEN_TEX_SGPR_BASE 4u /* set n: image at +12n, sampler at +12n+8 */
#define GLSL_GEN_TEX_SGPR_STRIDE 12u
/* Four sampler sets, which is what the scalar file has room for: sets s4..s51, masks
 * s52..s66, draw constants s68..s71 and uniforms s72..s103, against a ceiling of s105
 * (Mesa `ac_gpu_info.c:260`, `max_sgpr_alloc = 108` with VCC at s[106-107]). The
 * static assertions in `gl_internal.h` and below stop a fifth set running past the end.
 */
#define GLSL_GEN_MAX_TEX_SETS OOPS_GL_GL2_TEX_SETS
#define GLSL_GEN_LIVE_SGPR 52u
#define GLSL_GEN_EXEC_SGPR_BASE 53u
/* Eight levels of nested `if`, what fits below the ceiling beside four sampler sets. A
 * ninth is a diagnostic rather than a corrupted register. */
#define GLSL_GEN_MAX_EXEC_DEPTH 8

/*
 * A branched loop needs three masks, where an `if` needs one:
 *
 *   `active`  the lanes still going round. The condition narrows it each trip and
 *             `break` removes lanes for good; `exec` is reloaded from it at the top of
 *             every trip, which also undoes a `continue`.
 *   `entry`   the mask the loop was entered with, restored on the way out: a lane that
 *             left `active` still runs the statements after the loop.
 *   `trip`    the trip guard's counter.
 *
 * See `gen_for_branched`. The depth counts only loops the unroller could not finish;
 * unrolled loops nested inside cost nothing here. The masks sit after the exec stack.
 */
#define GLSL_GEN_LOOP_SGPR_BASE 61u
#define GLSL_GEN_LOOP_SGPR_COUNT 3u
#define GLSL_GEN_MAX_LOOP_DEPTH 2
/* A branched loop always ends. The guard's ceiling is the statically counted trip
 * count, so it fires only on a shader that does not do what it says. A loop asking for
 * more trips than this is refused, since an unbounded ceiling would permit the hang. */
#define GLSL_GEN_MAX_TRIPS 65536
/* How deep user-defined calls may nest before the generator refuses, so a shader
 * calling itself is a message rather than a hang. */
#define GLSL_GEN_MAX_INLINE_DEPTH 8

/*
 * The scalar map, asserted as a sequence: every range in order, each starting after the
 * last ends. The general SGPRs are s0..s105 (Mesa `ac_gpu_info.c:260`,
 * `max_sgpr_alloc = 108` with VCC at s[106-107]); an overlap or overrun gives no
 * run-time error, only a wrong pixel. `glsl_ps.c` continues the sequence with the draw
 * constants and the uniforms.
 */
typedef char glsl_gen_sgpr_map_fits
    [(GLSL_GEN_TEX_SGPR_BASE +
              (unsigned)GLSL_GEN_MAX_TEX_SETS * GLSL_GEN_TEX_SGPR_STRIDE <=
          GLSL_GEN_LIVE_SGPR &&
      GLSL_GEN_LIVE_SGPR + 1u <= GLSL_GEN_EXEC_SGPR_BASE &&
      GLSL_GEN_EXEC_SGPR_BASE + (unsigned)GLSL_GEN_MAX_EXEC_DEPTH <=
          GLSL_GEN_LOOP_SGPR_BASE &&
      GLSL_GEN_LOOP_SGPR_BASE +
              (unsigned)GLSL_GEN_MAX_LOOP_DEPTH * GLSL_GEN_LOOP_SGPR_COUNT <=
          106u)
         ? 1
         : -1];

typedef struct {
    uint32_t base; /* the first VGPR of the run */
    int count;     /* how many consecutive registers, 0 when the value is an error */
} glsl_value_t;

typedef struct {
    const char *name;
    size_t name_len;
    glsl_value_t value;
    glsl_type_t type;
    /* How many elements, or 0 for a name that is not an array, mirroring
     * `glsl_symbol_t`. The registers are the elements end to end (element `k` of a
     * `vec3 v[4]` is `value.base + 3k`), which is all indexing needs. */
    int array_size;
    /* Set only for an unrolled loop's counter, which holds a known value in each copy
     * of the body, so `w[i]` resolves to a register. Not set for locals in general:
     * that would need dataflow, and a body assigning to the counter is refused before
     * emission. */
    GLboolean is_const;
    double const_val;
} glsl_gen_var_t;

typedef struct {
    glsl_ast_t *ast;
    glsl_sema_t *sema;
    glsl_code_t *code;
    uint32_t next_vgpr;  /* the bump allocator's cursor */
    uint32_t high_water; /* the most registers ever live, which is what the stage must
                            reserve */
    glsl_gen_var_t vars[GLSL_GEN_MAX_VARS];
    int var_count;
    /* How many enclosing `if`s have saved the exec mask. `discard` reads it: a
     * discarded lane has to come out of every one of those saves, or the innermost
     * restore brings it back. */
    int exec_depth;
    /* How many branched loops enclose this point, and the `exec_depth` each was entered
     * at. `break` and `continue` take the lane out of the `if`s from the loop's entry
     * depth up to here, not from zero: an `if` outside the loop must still restore the
     * lane once the loop is over. */
    int loop_depth;
    int loop_exec_depth[GLSL_GEN_MAX_LOOP_DEPTH];
    /* How many user-defined calls are being inlined around this point. Calls are
     * inlined, so this bounds a shader that calls itself, which GLSL forbids but
     * invalid input may still do. */
    int inline_depth;
    /* What an early `return` needs about the function it leaves, one entry per inlined
     * call. `out` is the caller's result register. `exec_depth` and `loop_depth` are
     * where the function started, so a return takes its lanes out of the `if`s and
     * loops inside the function only: the lane still runs the rest of the caller.
     * `saved` is false when the body has no early return, and then no mask is taken. */
    glsl_value_t fn_out[GLSL_GEN_MAX_INLINE_DEPTH];
    int fn_exec_depth[GLSL_GEN_MAX_INLINE_DEPTH];
    int fn_loop_depth[GLSL_GEN_MAX_INLINE_DEPTH];
    GLboolean fn_saved[GLSL_GEN_MAX_INLINE_DEPTH];
    /* Whether this shader runs in whole-quad mode, which it does exactly when it
     * samples. Then `discard` also takes the lane out of the live mask at
     * `GLSL_GEN_LIVE_SGPR`, which the export is restored from. */
    GLboolean wqm;
    /* The sampler uniforms this shader named, in the order it named them, each with the
     * descriptor set the prologue loaded for it. */
    struct {
        const char *name;
        size_t name_len;
        uint32_t set;
        /* Which lookup this sampler answers to, as the `GLSL_IMG_DIM_*` the sample
         * carries. It must match the descriptor's shape (cube and 3D take three address
         * registers, 2D two), so `texture2D` on a `samplerCube` is refused. */
        uint32_t dim;
        /* Whether it compares rather than returns: a `sampler2DShadow`. Its dim is
         * still 2D; the instruction, the mask and a reference in the coordinate change.
         */
        GLboolean shadow;
        /* A 1D texture is a 2D image one row high, as `glTexImage1D` stores it and
         * `gl_state.c` describes it (TYPE 9, the 2D one). Its `dim` is 2D, and this
         * records that the coordinate is one component with a zero beside it. */
        GLboolean oned;
    } samplers[GLSL_GEN_MAX_TEX_SETS];
    int sampler_count;
    const char *error; /* the first failure, which stops everything after it */
    int error_line, error_column;
} glsl_gen_t;

void glsl_gen_init(glsl_gen_t *g, glsl_ast_t *ast, glsl_sema_t *sema,
                   glsl_code_t *code);
/* One statement: a declaration, an expression statement, or a block of them. */
GLboolean glsl_gen_stmt(glsl_gen_t *g, int32_t node);
/* One expression, leaving its value in the registers the returned value names. */
glsl_value_t glsl_gen_expression(glsl_gen_t *g, int32_t node);
/* Gives a name a register home without a declaration in the tree: how an attribute, a
 * uniform or a varying arrives. */
glsl_value_t glsl_gen_declare_input(glsl_gen_t *g, const char *name, size_t len,
                                    glsl_type_t type);
/* The same, for an input that is an array of `count` of `type`, such as
 * `gl_TexCoord[]`. The registers are the elements end to end, the layout `[]` indexes.
 * Returns the whole run. */
glsl_value_t glsl_gen_declare_input_array(glsl_gen_t *g, const char *name, size_t len,
                                          glsl_type_t type, int count);
/* Puts the allocator's cursor at `first`, so the registers below it are the caller's: a
 * pixel shader's v0 and v1 hold the barycentrics and v4..v7 are what it exports from.
 */
void glsl_gen_reserve(glsl_gen_t *g, uint32_t first);
/* One unnamed register for the prologue's own constants; see the definition. 0, with
 * `g->error` set, when the file is full. */
uint32_t glsl_gen_scratch(glsl_gen_t *g);
/* Where a declared name lives. False when nothing of that name has a register. */
GLboolean glsl_gen_lookup(glsl_gen_t *g, const char *name, size_t len,
                          glsl_value_t *out);
/* Tells the generator that `name` is a sampler whose descriptors the prologue loaded
 * into set `set`; sampling any other name is refused. `dim` is one of the
 * `GLSL_IMG_DIM_*` above, what this sampler's lookups carry. */
GLboolean glsl_gen_declare_sampler(glsl_gen_t *g, const char *name, size_t len,
                                   uint32_t set, uint32_t dim, GLboolean shadow,
                                   GLboolean oned);

/* -------------------------------------------------------------------------
 * A compiled unit
 *
 * What `glCompileShader` produces and `glLinkProgram` takes references to. It is opaque
 * in gl_internal.h; only the front end and the linker see this shape.
 *
 * The source is owned and kept, because every `text` pointer in the tree points into
 * it. `glGetShaderSource` also answers from it.
 * ------------------------------------------------------------------------- */
struct glsl_unit {
    int refs; /* the shader object, plus every program that linked it */
    GLenum stage;
    char *source;
    size_t source_len;
    int version;  /* what `#version` said, or 0 when there was none */
    int32_t root; /* the GLSL_NODE_UNIT */
    glsl_ast_t ast;
    /* The struct table, carried out of the transient semantic pass so the interpreter
     * uses the same layout rather than re-deriving one. Member names point into
     * `source`, which this unit owns. */
    glsl_struct_t structs[GLSL_MAX_STRUCTS];
    int struct_count;
};

/* Preprocesses, parses and checks `src`, returning a unit with one reference, or NULL
 * having written a diagnostic into `log`. Nothing is left allocated on failure. */
glsl_unit_t *glsl_unit_compile(GLenum stage, const char *src, size_t len, char *log,
                               size_t log_size);

/* The GL enumerant `glGetActiveUniform` reports for a front-end type, and how many
 * floats one element of it occupies. A sampler is one float, holding its texture unit
 * number. */
GLenum glsl_type_to_gl(glsl_type_t t);
int glsl_type_floats(glsl_type_t t);

void glsl_scope_push(glsl_sema_t *s);
void glsl_scope_pop(glsl_sema_t *s);
/* Checks a whole translation unit: declarations, function bodies, statements and
 * l-values. Returns GL_FALSE with `s->error` set on the first problem. */
GLboolean glsl_check_unit(glsl_sema_t *s, int32_t unit);
/* Whether an expression may appear on the left of an assignment. */
GLboolean glsl_is_lvalue(glsl_sema_t *s, int32_t node);

#endif /* __GLSL_INTERNAL_H__ */
