/*
 * oops-gl: GLSL 1.10's built-in library
 *
 * `sin`, `dot`, `texture2D` and the sixty-odd others the language provides, and the
 * `gl_` variables and constants a shader may use without declaring them.
 *
 * Every arithmetic built-in is overloaded over genType, and `glsl_symbol_t` holds one
 * signature per name, so built-ins are resolved by rule - a table of names against the
 * shape of their signature - ahead of the symbol table, as constructors are. The rules
 * keep the specification's asymmetries (GLSL 1.10, section 8): `min(genType, float)` is
 * legal and `min(float, genType)` is not, `step(float, genType)` is legal, and
 * `smoothstep` takes both edges as scalars or neither.
 *
 * The built-in uniform structures (`gl_LightSource[]`, `gl_Fog`, ...) are refused by
 * name, since this front end has no struct type; see `glsl_builtin_refusal`.
 */

#include "glsl_internal.h"

/* -------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------- */

static GLboolean name_is(const char *text, size_t len, const char *lit) {
    size_t i = 0;
    for (; lit[i] != '\0'; i++) {
        if (i >= len || text[i] != lit[i])
            return GL_FALSE;
    }
    return (GLboolean)(i == len);
}

/* -------------------------------------------------------------------------
 * genType
 *
 * A genType argument is a float or a float vector. An int vector is not one: GLSL 1.10
 * has no `sin(ivec2)` and no implicit conversion to reach it.
 * ------------------------------------------------------------------------- */

static GLboolean is_gen(glsl_type_t t) {
    return (GLboolean)(t == GLSL_TYPE_FLOAT || t == GLSL_TYPE_VEC2 ||
                       t == GLSL_TYPE_VEC3 || t == GLSL_TYPE_VEC4);
}

/* -------------------------------------------------------------------------
 * The rules
 * ------------------------------------------------------------------------- */

typedef enum {
    BI_GEN1, /* (genType) -> genType                          sin, abs, normalize */
    BI_GEN2, /* (genType, genType) -> genType                 pow, atan(y,x), reflect */
    BI_GEN2_SCALAR,  /* (genType, genType|float) -> genType           mod, min, max */
    BI_GEN3_SCALAR,  /* (genType, genType|float, genType|float)       clamp */
    BI_MIX,          /* (genType, genType, genType|float)             mix */
    BI_STEP,         /* (genType|float, genType) -> the second's type step */
    BI_SMOOTHSTEP,   /* (genType|float, same, genType)                smoothstep */
    BI_GEN3,         /* three genType -> genType                      faceforward */
    BI_REFRACT,      /* (genType, genType, float) -> genType */
    BI_TO_FLOAT1,    /* (genType) -> float                            length */
    BI_TO_FLOAT2,    /* (genType, genType) -> float                   distance, dot */
    BI_CROSS,        /* (vec3, vec3) -> vec3 */
    BI_MATCOMP,      /* (matN, matN) -> matN */
    BI_TRANSPOSE,    /* (matN) -> matN                                1.20 */
    BI_OUTERPRODUCT, /* (vecN, vecN) -> matN                          1.20 */
    BI_RELATIONAL,   /* (vecN|ivecN, same) -> bvecN                   lessThan and the
                        rest */
    BI_EQUALITY,     /* the above, and bvecN too                      equal, notEqual */
    BI_ANY_ALL,      /* (bvecN) -> bool */
    BI_NOT,          /* (bvecN) -> bvecN */
    BI_TEXTURE,      /* (sampler, coord [, bias]) -> vec4 */
    BI_FTRANSFORM,   /* () -> vec4 */
    /* The noise functions, which return zero: `(genType) -> float|vec2|vec3|vec4`, the
     * width fixed by the name. GLSL 1.10 section 8.9 does not forbid a constant, Mesa
     * returns 0 (`builtin_functions.cpp:8237`), and GLSL 4.4 specifies it. */
    BI_NOISE,
    BI_REFUSED /* a name that is a built-in and is not implemented */
} bi_rule_t;

typedef struct {
    const char *name;
    bi_rule_t rule;
    /* BI_TEXTURE only: the sampler the first argument must be, how many components the
     * coordinate has, and whether a level-of-detail argument replaces the bias. */
    glsl_type_t sampler;
    int coord;
    /* Which stage may call it. 0 for either; GL_VERTEX_SHADER or GL_FRAGMENT_SHADER for
     * one. `dFdx` in a vertex shader and `texture2DLod` in a fragment shader are both
     * errors the specification names. */
    GLenum stage;
    /* BI_REFUSED only: what to say instead of "undeclared". */
    const char *refusal;
} bi_entry_t;

static const bi_entry_t BUILTINS[] = {
    /* 8.1 Angle and trigonometry */
    {"radians", BI_GEN1, 0, 0, 0, 0},
    {"degrees", BI_GEN1, 0, 0, 0, 0},
    {"sin", BI_GEN1, 0, 0, 0, 0},
    {"cos", BI_GEN1, 0, 0, 0, 0},
    {"tan", BI_GEN1, 0, 0, 0, 0},
    {"asin", BI_GEN1, 0, 0, 0, 0},
    {"acos", BI_GEN1, 0, 0, 0, 0},
    /* `atan` is the one name with two arities: one argument is y/x, two are y and x in
     * that order. Handled where the arity is counted rather than as two entries. */
    {"atan", BI_GEN2, 0, 0, 0, 0},

    /* 8.2 Exponential */
    {"pow", BI_GEN2, 0, 0, 0, 0},
    {"exp", BI_GEN1, 0, 0, 0, 0},
    {"log", BI_GEN1, 0, 0, 0, 0},
    {"exp2", BI_GEN1, 0, 0, 0, 0},
    {"log2", BI_GEN1, 0, 0, 0, 0},
    {"sqrt", BI_GEN1, 0, 0, 0, 0},
    {"inversesqrt", BI_GEN1, 0, 0, 0, 0},

    /* 8.3 Common */
    {"abs", BI_GEN1, 0, 0, 0, 0},
    {"sign", BI_GEN1, 0, 0, 0, 0},
    {"floor", BI_GEN1, 0, 0, 0, 0},
    {"ceil", BI_GEN1, 0, 0, 0, 0},
    {"fract", BI_GEN1, 0, 0, 0, 0},
    {"mod", BI_GEN2_SCALAR, 0, 0, 0, 0},
    {"min", BI_GEN2_SCALAR, 0, 0, 0, 0},
    {"max", BI_GEN2_SCALAR, 0, 0, 0, 0},
    {"clamp", BI_GEN3_SCALAR, 0, 0, 0, 0},
    {"mix", BI_MIX, 0, 0, 0, 0},
    {"step", BI_STEP, 0, 0, 0, 0},
    {"smoothstep", BI_SMOOTHSTEP, 0, 0, 0, 0},

    /* 8.4 Geometric */
    {"length", BI_TO_FLOAT1, 0, 0, 0, 0},
    {"distance", BI_TO_FLOAT2, 0, 0, 0, 0},
    {"dot", BI_TO_FLOAT2, 0, 0, 0, 0},
    {"cross", BI_CROSS, 0, 0, 0, 0},
    {"normalize", BI_GEN1, 0, 0, 0, 0},
    {"faceforward", BI_GEN3, 0, 0, 0, 0},
    {"reflect", BI_GEN2, 0, 0, 0, 0},
    {"refract", BI_REFRACT, 0, 0, 0, 0},
    {"ftransform", BI_FTRANSFORM, 0, 0, GL_VERTEX_SHADER, 0},

    /* 8.5 Matrix. `transpose` and `outerProduct` are GLSL 1.20's, refused by version
     * in the resolver rather than by stage. */
    {"matrixCompMult", BI_MATCOMP, 0, 0, 0, 0},
    {"transpose", BI_TRANSPOSE, 0, 0, 0, 0},
    {"outerProduct", BI_OUTERPRODUCT, 0, 0, 0, 0},

    /* 8.6 Vector relational */
    {"lessThan", BI_RELATIONAL, 0, 0, 0, 0},
    {"lessThanEqual", BI_RELATIONAL, 0, 0, 0, 0},
    {"greaterThan", BI_RELATIONAL, 0, 0, 0, 0},
    {"greaterThanEqual", BI_RELATIONAL, 0, 0, 0, 0},
    {"equal", BI_EQUALITY, 0, 0, 0, 0},
    {"notEqual", BI_EQUALITY, 0, 0, 0, 0},
    {"any", BI_ANY_ALL, 0, 0, 0, 0},
    {"all", BI_ANY_ALL, 0, 0, 0, 0},
    {"not", BI_NOT, 0, 0, 0, 0},

    /* 8.7 Texture lookup.
     *
     * The `Proj` forms take one component more than the plain ones - the coordinate is
     * divided by its last - and `texture2DProj` accepts either a vec3 or a vec4, the
     * vec4's third component being ignored. So `coord` is a minimum width. */
    {"texture1D", BI_TEXTURE, GLSL_TYPE_SAMPLER1D, 1, 0, 0},
    {"texture1DProj", BI_TEXTURE, GLSL_TYPE_SAMPLER1D, 2, 0, 0},
    {"texture2D", BI_TEXTURE, GLSL_TYPE_SAMPLER2D, 2, 0, 0},
    {"texture2DProj", BI_TEXTURE, GLSL_TYPE_SAMPLER2D, 3, 0, 0},
    {"texture3D", BI_TEXTURE, GLSL_TYPE_SAMPLER3D, 3, 0, 0},
    {"texture3DProj", BI_TEXTURE, GLSL_TYPE_SAMPLER3D, 4, 0, 0},
    {"textureCube", BI_TEXTURE, GLSL_TYPE_SAMPLERCUBE, 3, 0, 0},
    {"shadow1D", BI_TEXTURE, GLSL_TYPE_SAMPLER1DSHADOW, 3, 0, 0},
    {"shadow2D", BI_TEXTURE, GLSL_TYPE_SAMPLER2DSHADOW, 3, 0, 0},
    {"shadow1DProj", BI_TEXTURE, GLSL_TYPE_SAMPLER1DSHADOW, 4, 0, 0},
    {"shadow2DProj", BI_TEXTURE, GLSL_TYPE_SAMPLER2DSHADOW, 4, 0, 0},
    /* The `Lod` forms are vertex-shader only (1.10, 8.7); a fragment shader has
     * implicit derivatives. */
    {"texture1DLod", BI_TEXTURE, GLSL_TYPE_SAMPLER1D, 1, GL_VERTEX_SHADER, 0},
    {"texture1DProjLod", BI_TEXTURE, GLSL_TYPE_SAMPLER1D, 2, GL_VERTEX_SHADER, 0},
    {"texture2DLod", BI_TEXTURE, GLSL_TYPE_SAMPLER2D, 2, GL_VERTEX_SHADER, 0},
    {"texture2DProjLod", BI_TEXTURE, GLSL_TYPE_SAMPLER2D, 3, GL_VERTEX_SHADER, 0},
    {"texture3DLod", BI_TEXTURE, GLSL_TYPE_SAMPLER3D, 3, GL_VERTEX_SHADER, 0},
    {"texture3DProjLod", BI_TEXTURE, GLSL_TYPE_SAMPLER3D, 4, GL_VERTEX_SHADER, 0},
    {"textureCubeLod", BI_TEXTURE, GLSL_TYPE_SAMPLERCUBE, 3, GL_VERTEX_SHADER, 0},
    {"shadow1DLod", BI_TEXTURE, GLSL_TYPE_SAMPLER1DSHADOW, 3, GL_VERTEX_SHADER, 0},
    {"shadow2DLod", BI_TEXTURE, GLSL_TYPE_SAMPLER2DSHADOW, 3, GL_VERTEX_SHADER, 0},
    {"shadow1DProjLod", BI_TEXTURE, GLSL_TYPE_SAMPLER1DSHADOW, 4, GL_VERTEX_SHADER, 0},
    {"shadow2DProjLod", BI_TEXTURE, GLSL_TYPE_SAMPLER2DSHADOW, 4, GL_VERTEX_SHADER, 0},

    /* 8.8 Fragment processing. Derivatives exist only where there is a neighbouring
     * fragment to take them against. */
    {"dFdx", BI_GEN1, 0, 0, GL_FRAGMENT_SHADER, 0},
    {"dFdy", BI_GEN1, 0, 0, GL_FRAGMENT_SHADER, 0},
    {"fwidth", BI_GEN1, 0, 0, GL_FRAGMENT_SHADER, 0},

    /* 8.9 Noise; see BI_NOISE. The `coord` column carries the result width here, since
     * the name fixes it. */
    {"noise1", BI_NOISE, 0, 1, 0, 0},
    {"noise2", BI_NOISE, 0, 2, 0, 0},
    {"noise3", BI_NOISE, 0, 3, 0, 0},
    {"noise4", BI_NOISE, 0, 4, 0, 0},
};

/* -------------------------------------------------------------------------
 * Resolution
 * ------------------------------------------------------------------------- */

/* `a` and `b` are the same genType, or `b` is a float where the rule allows a scalar.
 */
static GLboolean gen_or_scalar(glsl_type_t a, glsl_type_t b) {
    return (GLboolean)(b == a || b == GLSL_TYPE_FLOAT);
}

static glsl_type_t texture_type(glsl_sema_t *s, const bi_entry_t *e,
                                const glsl_type_t *args, int argc, int32_t node) {
    if (argc < 2 || argc > 3) {
        glsl_sema_fail(
            s, "a texture lookup takes a sampler, a coordinate and an optional bias",
            node);
        return GLSL_TYPE_ERROR;
    }
    if (args[0] != e->sampler) {
        glsl_sema_fail(
            s, "a texture lookup's first argument must be its own sampler type", node);
        return GLSL_TYPE_ERROR;
    }
    const int given = glsl_type_components(args[1]);
    if (!is_gen(args[1]) || given < e->coord || given > 4) {
        glsl_sema_fail(s, "a texture coordinate of the wrong width", node);
        return GLSL_TYPE_ERROR;
    }
    if (argc == 3 && args[2] != GLSL_TYPE_FLOAT) {
        glsl_sema_fail(s, "a texture lookup's bias or level must be a float", node);
        return GLSL_TYPE_ERROR;
    }
    /* Every lookup in 1.10 returns a vec4, the shadow ones included: the comparison
     * result is broadcast rather than returned as a float. */
    return GLSL_TYPE_VEC4;
}

glsl_type_t glsl_builtin_call_type(glsl_sema_t *s, const char *name, size_t len,
                                   const glsl_type_t *args, int argc, int32_t node,
                                   GLboolean *found) {
    if (found)
        *found = GL_FALSE;
    if (!s || !name || len == 0u)
        return GLSL_TYPE_ERROR;

    const bi_entry_t *e = (const bi_entry_t *)0;
    for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++) {
        if (name_is(name, len, BUILTINS[i].name)) {
            e = &BUILTINS[i];
            break;
        }
    }
    if (!e)
        return GLSL_TYPE_ERROR;
    if (found)
        *found = GL_TRUE;

    if (e->rule == BI_REFUSED) {
        glsl_sema_fail(s, e->refusal, node);
        return GLSL_TYPE_ERROR;
    }
    /* The stage decides whether this name exists at all. `s->stage` is 0 when a caller
     * checks a unit without naming its stage, as the front end's own tests do, and then
     * nothing is refused on this ground. */
    if (e->stage != 0u && s->stage != 0u && e->stage != s->stage) {
        glsl_sema_fail(s,
                       e->stage == GL_VERTEX_SHADER
                           ? "this built-in exists only in a vertex shader"
                           : "this built-in exists only in a fragment shader",
                       node);
        return GLSL_TYPE_ERROR;
    }

    switch (e->rule) {
    case BI_GEN1:
        if (argc != 1 || !is_gen(args[0]))
            break;
        return args[0];

    case BI_GEN2:
        /* `atan` with one argument is the one-operand form, which is a genType rule. */
        if (argc == 1 && name_is(name, len, "atan") && is_gen(args[0]))
            return args[0];
        if (argc != 2 || !is_gen(args[0]) || args[1] != args[0])
            break;
        return args[0];

    case BI_GEN2_SCALAR:
        if (argc != 2 || !is_gen(args[0]) || !gen_or_scalar(args[0], args[1]))
            break;
        return args[0];

    case BI_GEN3_SCALAR:
        if (argc != 3 || !is_gen(args[0]) || !gen_or_scalar(args[0], args[1]) ||
            args[2] != args[1]) {
            break;
        }
        return args[0];

    case BI_MIX:
        if (argc != 3 || !is_gen(args[0]) || args[1] != args[0] ||
            !gen_or_scalar(args[0], args[2])) {
            break;
        }
        return args[0];

    case BI_STEP:
        /* The result is the second argument's type: `step(0.5, v)` is a vec of v's
         * width. */
        if (argc != 2 || !is_gen(args[1]) || !gen_or_scalar(args[1], args[0]))
            break;
        return args[1];

    case BI_SMOOTHSTEP:
        if (argc != 3 || !is_gen(args[2]) || args[0] != args[1] ||
            !gen_or_scalar(args[2], args[0])) {
            break;
        }
        return args[2];

    case BI_GEN3:
        if (argc != 3 || !is_gen(args[0]) || args[1] != args[0] || args[2] != args[0])
            break;
        return args[0];

    case BI_REFRACT:
        if (argc != 3 || !is_gen(args[0]) || args[1] != args[0] ||
            args[2] != GLSL_TYPE_FLOAT) {
            break;
        }
        return args[0];

    case BI_TO_FLOAT1:
        if (argc != 1 || !is_gen(args[0]))
            break;
        return GLSL_TYPE_FLOAT;

    case BI_TO_FLOAT2:
        if (argc != 2 || !is_gen(args[0]) || args[1] != args[0])
            break;
        return GLSL_TYPE_FLOAT;

    case BI_CROSS:
        /* Only vec3 has a cross product. */
        if (argc != 2 || args[0] != GLSL_TYPE_VEC3 || args[1] != GLSL_TYPE_VEC3)
            break;
        return GLSL_TYPE_VEC3;

    case BI_MATCOMP:
        if (argc != 2 || !glsl_type_is_matrix(args[0]) || args[1] != args[0])
            break;
        return args[0];

    case BI_TRANSPOSE:
    case BI_OUTERPRODUCT:
        /* GLSL 1.20's, and refused by version in a 1.10 shader. */
        if (s->version != 0 && s->version < 120) {
            glsl_sema_fail(s, "this built-in is GLSL 1.20; this shader is 1.10", node);
            return GLSL_TYPE_ERROR;
        }
        if (e->rule == BI_TRANSPOSE) {
            /* `transpose(matCxR)` is `matRxC`. */
            if (argc != 1 || !glsl_type_is_matrix(args[0]))
                break;
            return glsl_type_matrix_of(glsl_type_matrix_rows(args[0]),
                                       glsl_type_matrix_cols(args[0]));
        }
        /* `outerProduct(vecR, vecC)` is `matCxR`: the first is a column and the second
         * a row, so the result has one column per entry of the second and one row per
         * entry of the first. */
        if (argc != 2 || !glsl_type_is_vector(args[0]) || !glsl_type_is_vector(args[1]))
            break;
        if (glsl_type_base(args[0]) != GLSL_TYPE_FLOAT ||
            glsl_type_base(args[1]) != GLSL_TYPE_FLOAT) {
            break;
        }
        {
            const glsl_type_t m = glsl_type_matrix_of(glsl_type_components(args[1]),
                                                      glsl_type_components(args[0]));
            if (m != GLSL_TYPE_ERROR)
                return m;
        }
        break;

    case BI_RELATIONAL:
    case BI_EQUALITY: {
        if (argc != 2 || args[1] != args[0])
            break;
        if (!glsl_type_is_vector(args[0]))
            break;
        const glsl_type_t base = glsl_type_base(args[0]);
        /* `equal` and `notEqual` accept bvecs; the ordering comparisons do not, because
         * `true > false` means nothing. */
        if (base == GLSL_TYPE_BOOL && e->rule == BI_RELATIONAL)
            break;
        if (base != GLSL_TYPE_FLOAT && base != GLSL_TYPE_INT && base != GLSL_TYPE_BOOL)
            break;
        return glsl_type_vector_of(GLSL_TYPE_BOOL, glsl_type_components(args[0]));
    }

    case BI_ANY_ALL:
        if (argc != 1 || !glsl_type_is_vector(args[0]) ||
            glsl_type_base(args[0]) != GLSL_TYPE_BOOL) {
            break;
        }
        return GLSL_TYPE_BOOL;

    case BI_NOT:
        if (argc != 1 || !glsl_type_is_vector(args[0]) ||
            glsl_type_base(args[0]) != GLSL_TYPE_BOOL) {
            break;
        }
        return args[0];

    case BI_TEXTURE:
        return texture_type(s, e, args, argc, node);

    case BI_FTRANSFORM:
        if (argc != 0)
            break;
        return GLSL_TYPE_VEC4;

    /* One argument of any float width; the result's width is the name's. */
    case BI_NOISE:
        if (argc != 1)
            break;
        if (!is_gen(args[0]))
            break;
        return glsl_type_vector_of(GLSL_TYPE_FLOAT, e->coord);

    case BI_REFUSED:
        break;
    }

    glsl_sema_fail(s, "a built-in called with arguments it does not take", node);
    return GLSL_TYPE_ERROR;
}

/* -------------------------------------------------------------------------
 * Built-in variables
 *
 * The `gl_` names a 1.10 shader may use without declaring. Split three ways, so that
 * writing `gl_FragColor` from a vertex shader is an error. The fixed-function uniforms
 * let a shader use the matrix stack the program already sets; the `uniform` qualifier
 * makes them read-only, as the specification says.
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    glsl_type_t type;
    int array; /* elements, or 0 */
    glsl_token_type_t
        qualifier; /* what may be done to it: uniform and attribute are read-only */
} bi_var_t;

/* Both stages. */
static const bi_var_t BUILTIN_COMMON[] = {
    /* The matrix stack, as a shader sees it. `gl_NormalMatrix` is the inverse transpose
     * of the modelview's upper 3x3, the one lighting uses. */
    {"gl_ModelViewMatrix", GLSL_TYPE_MAT4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_ProjectionMatrix", GLSL_TYPE_MAT4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_ModelViewProjectionMatrix", GLSL_TYPE_MAT4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_NormalMatrix", GLSL_TYPE_MAT3, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_TextureMatrix", GLSL_TYPE_MAT4, OOPS_GL_MAX_TEXTURE_UNITS,
     GLSL_TOK_KW_UNIFORM},
    {"gl_ModelViewMatrixInverse", GLSL_TYPE_MAT4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_ProjectionMatrixInverse", GLSL_TYPE_MAT4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_ModelViewProjectionMatrixInverse", GLSL_TYPE_MAT4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_ModelViewMatrixTranspose", GLSL_TYPE_MAT4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_ProjectionMatrixTranspose", GLSL_TYPE_MAT4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_ModelViewProjectionMatrixTranspose", GLSL_TYPE_MAT4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_NormalScale", GLSL_TYPE_FLOAT, 0, GLSL_TOK_KW_UNIFORM},
};

/* A vertex shader's inputs are the fixed-function per-vertex attributes; its outputs
 * are what a fragment shader or the fixed-function fragment stage will read. */
static const bi_var_t BUILTIN_VERTEX[] = {
    {"gl_Vertex", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_Normal", GLSL_TYPE_VEC3, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_Color", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_SecondaryColor", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_FogCoord", GLSL_TYPE_FLOAT, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_MultiTexCoord0", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_MultiTexCoord1", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_MultiTexCoord2", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_MultiTexCoord3", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_MultiTexCoord4", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_MultiTexCoord5", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_MultiTexCoord6", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_ATTRIBUTE},
    {"gl_MultiTexCoord7", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_ATTRIBUTE},

    /* Outputs are written, not qualified. A vertex shader that leaves `gl_Position`
     * unwritten is undefined by the specification; the linker refuses it. */
    {"gl_Position", GLSL_TYPE_VEC4, 0, GLSL_TOK_EOF},
    {"gl_PointSize", GLSL_TYPE_FLOAT, 0, GLSL_TOK_EOF},
    {"gl_ClipVertex", GLSL_TYPE_VEC4, 0, GLSL_TOK_EOF},
    {"gl_FrontColor", GLSL_TYPE_VEC4, 0, GLSL_TOK_EOF},
    {"gl_BackColor", GLSL_TYPE_VEC4, 0, GLSL_TOK_EOF},
    {"gl_FrontSecondaryColor", GLSL_TYPE_VEC4, 0, GLSL_TOK_EOF},
    {"gl_BackSecondaryColor", GLSL_TYPE_VEC4, 0, GLSL_TOK_EOF},
    {"gl_TexCoord", GLSL_TYPE_VEC4, OOPS_GL_MAX_TEXTURE_UNITS, GLSL_TOK_EOF},
    {"gl_FogFragCoord", GLSL_TYPE_FLOAT, 0, GLSL_TOK_EOF},
};

static const bi_var_t BUILTIN_FRAGMENT[] = {
    /* `gl_FragCoord` is in window coordinates, and its w is 1/w_clip, not the clip w.
     */
    {"gl_FragCoord", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_FrontFacing", GLSL_TYPE_BOOL, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_Color", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_SecondaryColor", GLSL_TYPE_VEC4, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_TexCoord", GLSL_TYPE_VEC4, OOPS_GL_MAX_TEXTURE_UNITS, GLSL_TOK_KW_UNIFORM},
    {"gl_FogFragCoord", GLSL_TYPE_FLOAT, 0, GLSL_TOK_KW_UNIFORM},

    /* `gl_PointCoord` (GLSL 1.20): where the fragment sits inside the point being
     * drawn, (0,0) to (1,1), with the origin `GL_POINT_SPRITE_COORD_ORIGIN` names
     * (`GL_UPPER_LEFT` by default).
     *
     * It is texture coordinate 0's interpolant: a point is expanded into two triangles
     * whose corners carry the sprite coordinates (gl_draw.c), as `GL_COORD_REPLACE`
     * does for the fixed-function path; the part's own mechanism is
     * `SPI_PS_INPUT_CNTL.PT_SPRITE_TEX`. So `gl_PointCoord` and `gl_TexCoord[0]` are
     * one slot, and a fragment shader reading both is refused at link time. */
    {"gl_PointCoord", GLSL_TYPE_VEC2, 0, GLSL_TOK_KW_UNIFORM},
    {"gl_FragColor", GLSL_TYPE_VEC4, 0, GLSL_TOK_EOF},
    {"gl_FragDepth", GLSL_TYPE_FLOAT, 0, GLSL_TOK_EOF},
    /* One draw buffer, so one element: the same number `GL_MAX_DRAW_BUFFERS` answers
     * and `gl_MaxDrawBuffers` reads, because GLSL declares this array as
     * `gl_FragData[gl_MaxDrawBuffers]`. */
    {"gl_FragData", GLSL_TYPE_VEC4, OOPS_GL_MAX_DRAW_BUFFERS, GLSL_TOK_EOF},
};

/* The built-in constants (1.10, 7.4): this implementation's own limits, each from the
 * constant the matching `glGetIntegerv` answers with, so a shader and the API agree.
 * They are `const int`, so both back ends fold a use into a literal, and one may be an
 * array's length (4.1.9 requires an integral constant expression). */
typedef struct {
    const char *name;
    int value;
} bi_const_t;

static const bi_const_t BUILTIN_CONSTS[] = {
    {"gl_MaxLights", OOPS_GL_LIGHT_COUNT},
    {"gl_MaxClipPlanes", OOPS_GL_CLIP_PLANE_COUNT},
    {"gl_MaxTextureUnits", OOPS_GL_MAX_TEXTURE_UNITS},
    /* The fixed-function stage count, because `gl_TexCoord[]` is a fixed-function
     * array: the answer `GL_MAX_TEXTURE_COORDS` gives, not the sampler count. */
    {"gl_MaxTextureCoords", OOPS_GL_MAX_TEXTURE_UNITS},
    {"gl_MaxVertexAttribs", OOPS_GL_MAX_VERTEX_ATTRIBS},
    {"gl_MaxVertexUniformComponents", OOPS_GL_MAX_PROGRAM_UNIFORMS * 4},
    {"gl_MaxVaryingFloats", OOPS_GL_MAX_VARYING_FLOATS},
    /* Zero, which is legal: the vertex stage here does not sample textures. */
    {"gl_MaxVertexTextureImageUnits", 0},
    {"gl_MaxCombinedTextureImageUnits", OOPS_GL_MAX_TEXTURE_IMAGE_UNITS},
    {"gl_MaxTextureImageUnits", OOPS_GL_MAX_TEXTURE_IMAGE_UNITS},
    {"gl_MaxFragmentUniformComponents", OOPS_GL_MAX_PROGRAM_UNIFORMS * 4},
    /* GLSL declares `gl_FragData[gl_MaxDrawBuffers]`, so this constant, that array's
     * length and `GL_MAX_DRAW_BUFFERS` are one fact; see `OOPS_GL_MAX_DRAW_BUFFERS`. */
    {"gl_MaxDrawBuffers", OOPS_GL_MAX_DRAW_BUFFERS},
};

static GLboolean declare_table(glsl_sema_t *s, const bi_var_t *t, size_t n) {
    for (size_t i = 0; i < n; i++) {
        size_t len = 0;
        while (t[i].name[len] != '\0')
            len++;
        if (!glsl_declare_array(s, t[i].name, len, t[i].type, t[i].array,
                                t[i].qualifier)) {
            return GL_FALSE;
        }
    }
    return GL_TRUE;
}

/* The `gl_` names that are real GLSL and are not here, each with the reason. A shader
 * that uses one is told what is missing rather than "use of an undeclared name", which
 * reads as a typo. */
const char *glsl_builtin_refusal(const char *name, size_t len) {
    static const struct {
        const char *name;
        const char *why;
    } REFUSED[] = {
        /* The built-in uniform structures. This front end has no struct type at all, so
         * there is nothing for a member selection to select from. */
        {"gl_LightSource",
         "gl_LightSource is a struct array, and structs are not implemented"},
        {"gl_LightModel", "gl_LightModel is a struct, and structs are not implemented"},
        {"gl_FrontMaterial",
         "gl_FrontMaterial is a struct, and structs are not implemented"},
        {"gl_BackMaterial",
         "gl_BackMaterial is a struct, and structs are not implemented"},
        {"gl_FrontLightProduct",
         "gl_FrontLightProduct is a struct array, and structs are not implemented"},
        {"gl_BackLightProduct",
         "gl_BackLightProduct is a struct array, and structs are not implemented"},
        {"gl_Fog", "gl_Fog is a struct, and structs are not implemented"},
        {"gl_DepthRange", "gl_DepthRange is a struct, and structs are not implemented"},
        /* GLSL 1.30 and later, named so a shader that meant to be a later version is
         * told which version it is written in rather than which word is unknown. */
        {"gl_InstanceID",
         "gl_InstanceID is GLSL 1.40; this front end takes 1.10 and 1.20"},
        {"gl_VertexID", "gl_VertexID is GLSL 1.30; this front end takes 1.10 and 1.20"},
        {"gl_ClipDistance", "gl_ClipDistance is GLSL 1.30; use gl_ClipVertex"},
    };
    if (!name || len == 0u)
        return (const char *)0;
    for (size_t i = 0; i < sizeof(REFUSED) / sizeof(REFUSED[0]); i++) {
        if (name_is(name, len, REFUSED[i].name))
            return REFUSED[i].why;
    }
    return (const char *)0;
}

/* The value of a built-in constant, or false for any other name. Both back ends ask, so
 * a use of one becomes a literal in each rather than a register neither declared. */
GLboolean glsl_builtin_const_int(const char *name, size_t len, int *out) {
    if (!name || len == 0u || !out)
        return GL_FALSE;
    for (size_t i = 0; i < sizeof(BUILTIN_CONSTS) / sizeof(BUILTIN_CONSTS[0]); i++) {
        if (name_is(name, len, BUILTIN_CONSTS[i].name)) {
            *out = BUILTIN_CONSTS[i].value;
            return GL_TRUE;
        }
    }
    return GL_FALSE;
}

GLboolean glsl_declare_builtins(glsl_sema_t *s, GLenum stage) {
    if (!s)
        return GL_FALSE;
    /* The constants first, and in every stage: 7.4 makes them available to both. */
    for (size_t i = 0; i < sizeof(BUILTIN_CONSTS) / sizeof(BUILTIN_CONSTS[0]); i++) {
        size_t len = 0;
        while (BUILTIN_CONSTS[i].name[len] != '\0')
            len++;
        if (!glsl_declare_const_int(s, BUILTIN_CONSTS[i].name, len,
                                    BUILTIN_CONSTS[i].value)) {
            return GL_FALSE;
        }
    }
    if (!declare_table(s, BUILTIN_COMMON,
                       sizeof(BUILTIN_COMMON) / sizeof(BUILTIN_COMMON[0]))) {
        return GL_FALSE;
    }
    if (stage == GL_VERTEX_SHADER) {
        return declare_table(s, BUILTIN_VERTEX,
                             sizeof(BUILTIN_VERTEX) / sizeof(BUILTIN_VERTEX[0]));
    }
    if (stage == GL_FRAGMENT_SHADER) {
        return declare_table(s, BUILTIN_FRAGMENT,
                             sizeof(BUILTIN_FRAGMENT) / sizeof(BUILTIN_FRAGMENT[0]));
    }
    return GL_TRUE;
}
