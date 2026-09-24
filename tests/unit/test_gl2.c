/*
 * OpenGL 2.0: shader objects, program objects and generic vertex attributes.
 *
 * Separate from `test_gl.c` because it is a separate API - the fixed-function tests next door
 * are about state and pixels, and these are about an object model with a lifetime.
 *
 * **What these check is the behaviour a port will actually lean on**, which is not the happy
 * path. A program that compiles a shader, links it and draws is served by almost any
 * implementation; the ones that go wrong are the deferred delete, the location of `-1`, the
 * attribute bound after the link rather than before it, and the uniform set with the wrong
 * command. Each of those has a test here and each of them was written against the
 * specification's own wording rather than against what this implementation happened to do.
 */

#include "oops/display.h"
#include "src/gl/gl_internal.h"
#include "src/gl/glsl_internal.h"
#include "oops/math.h"
#include "tests/test_common.h"
#include <math.h>

/* A fresh context per test. The object tables live in it, so nothing leaks between tests and
 * the name counter starts at 1 every time - which is what lets a test assert on a name.
 *
 * **`glContextSetVersion(2, 0)` is not decoration.** A context has the entry points its version
 * defines and no others, and the default is 1.1 - so without this line every call below is
 * GL_INVALID_OPERATION and does nothing. That is the point of the claim, and
 * `test_gl2_version_gating` is the check that it still bites. */
static void *gl2_context(void) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
    void *ctx = glContextCreate(disp);
    glContextMakeCurrent(ctx);
    glContextSetVersion(2, 0);
    return ctx;
}

static void source_of(GLuint sh, const char *src) {
    const GLchar *strings[1];
    strings[0] = src;
    glShaderSource(sh, 1, strings, NULL);
}

/* Compiles a shader and returns it, requiring the compile to have succeeded - so a test about
 * linking fails on the link rather than on a typo three lines up. */
static GLuint compiled(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    ASSERT_TRUE(sh != 0u);
    source_of(sh, src);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = {0};
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        printf("\n    compile failed: %s\n", log);
    }
    ASSERT_EQ(ok, GL_TRUE);
    return sh;
}

static const char *const VS_SIMPLE =
    "uniform mat4 mvp;\n"
    "attribute vec3 pos;\n"
    "attribute vec3 colour;\n"
    "varying vec3 vcolour;\n"
    "void main() {\n"
    "  vcolour = colour;\n"
    "  gl_Position = mvp * vec4(pos, 1.0);\n"
    "}\n";

static const char *const FS_SIMPLE =
    "varying vec3 vcolour;\n"
    "void main() { gl_FragColor = vec4(vcolour, 1.0); }\n";

/* -------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------- */

static void test_gl2_shaders_and_programs_share_one_name_space(void) {
    void *ctx = gl2_context();

    const GLuint a = glCreateShader(GL_VERTEX_SHADER);
    const GLuint p = glCreateProgram();
    const GLuint b = glCreateShader(GL_FRAGMENT_SHADER);
    ASSERT_TRUE(a != 0u && p != 0u && b != 0u);
    /* **The three names are distinct**, which two independent counters would not give: with
     * one counter per kind the shader and the program would both be 1. */
    ASSERT_TRUE(a != p && p != b && a != b);

    ASSERT_EQ(glIsShader(a), GL_TRUE);
    ASSERT_EQ(glIsProgram(a), GL_FALSE);
    ASSERT_EQ(glIsProgram(p), GL_TRUE);
    ASSERT_EQ(glIsShader(p), GL_FALSE);
    /* A name nothing owns is neither, and asking is not an error. */
    ASSERT_EQ(glIsShader(9999u), GL_FALSE);
    ASSERT_EQ(glIsProgram(9999u), GL_FALSE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* A shader operation on a program name is GL_INVALID_OPERATION - the object exists and is
     * the wrong kind - while a dead name is GL_INVALID_VALUE. */
    glCompileShader(p);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glCompileShader(9999u);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

    glCreateShader(GL_TEXTURE_2D);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    glContextDestroy(ctx);
}

static void test_gl2_a_name_is_never_reused(void) {
    void *ctx = gl2_context();
    const GLuint first = glCreateShader(GL_VERTEX_SHADER);
    glDeleteShader(first);
    ASSERT_EQ(glIsShader(first), GL_FALSE);
    const GLuint second = glCreateShader(GL_VERTEX_SHADER);
    /* The slot is free again; the *name* is not. A stale `first` held by a caller now finds
     * nothing rather than this new object. */
    ASSERT_TRUE(second != first);
    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * Compiling
 * ------------------------------------------------------------------------- */

static void test_gl2_compile_reports_status_and_a_log(void) {
    void *ctx = gl2_context();

    GLuint sh = glCreateShader(GL_VERTEX_SHADER);
    GLint status = -1;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);
    glGetShaderiv(sh, GL_SHADER_TYPE, &status);
    ASSERT_EQ(status, (GLint)GL_VERTEX_SHADER);

    /* Compiling with no source fails and says why. It is **not** a GL error: the call has a
     * status and a log to report through, which is the whole point of having them. */
    glCompileShader(sh);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);
    GLint log_len = 0;
    glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &log_len);
    ASSERT_TRUE(log_len > 1);

    source_of(sh, VS_SIMPLE);
    glGetShaderiv(sh, GL_SHADER_SOURCE_LENGTH, &log_len);
    ASSERT_EQ(log_len, (GLint)(strlen(VS_SIMPLE) + 1u));
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);
    glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &log_len);
    ASSERT_EQ(log_len, 0);  /* nothing to say */

    /* The source comes back as it went in, and `bufSize` is honoured to the byte: at most
     * `bufSize` written including the terminator, `length` excluding it. */
    char buf[8];
    GLsizei written = -1;
    memset(buf, 'x', sizeof(buf));
    glGetShaderSource(sh, 8, &written, buf);
    ASSERT_EQ(written, 7);
    ASSERT_EQ(buf[7], '\0');
    ASSERT_TRUE(strncmp(buf, VS_SIMPLE, 7) == 0);

    /* A zero buffer writes nothing at all, not even a terminator - which is what lets a caller
     * measure with a null pointer. */
    written = -1;
    glGetShaderSource(sh, 0, &written, NULL);
    ASSERT_EQ(written, 0);

    /* New source un-compiles it: a status left over from text that no longer exists would be a
     * lie about the shader that is there now. */
    source_of(sh, FS_SIMPLE);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    glContextDestroy(ctx);
}

static void test_gl2_compile_refuses_and_explains(void) {
    void *ctx = gl2_context();

    /* A type error the grammar accepts: `vec3 * mat4` is a well-formed binary expression whose
     * dimensions do not meet. */
    GLuint bad = glCreateShader(GL_VERTEX_SHADER);
    source_of(bad,
              "attribute vec3 pos;\n"
              "void main() { gl_Position = vec4(pos * mat4(1.0), 1.0); }\n");
    glCompileShader(bad);
    GLint status = -1;
    glGetShaderiv(bad, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);
    char log[256] = {0};
    GLsizei n = 0;
    glGetShaderInfoLog(bad, (GLsizei)sizeof(log), &n, log);
    ASSERT_TRUE(n > 0);
    /* The diagnostic carries the line it happened on, which is the difference between a message
     * and a message somebody can act on. */
    ASSERT_TRUE(log[0] == '2');

    /* **1.10 and 1.20 are the two dialects**, each stated explicitly. */
    GLuint v110 = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(v110, "#version 110\nvoid main() { gl_FragColor = vec4(1.0); }\n");
    glCompileShader(v110);
    glGetShaderiv(v110, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    GLuint v120 = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(v120, "#version 120\nvoid main() { gl_FragColor = vec4(1.0); }\n");
    glCompileShader(v120);
    glGetShaderiv(v120, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    /* Anything later is refused **by version** rather than compiled as one of them: a 1.30
     * shader takes `in`/`out` in place of `attribute`/`varying` and means its integer
     * arithmetic, and accepting it as 1.20 would take those rules somewhere else silently. */
    GLuint v130 = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(v130, "#version 130\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n");
    glCompileShader(v130);
    glGetShaderiv(v130, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* A `gl_` name that is real GLSL and is missing here is **named**, with the reason. "Use of
     * an undeclared name" reads as a typo and sends its author to check their spelling.
     *
     * This asserted on `gl_PointCoord` until 2026-09-23, when that stopped being missing - the
     * refusal had claimed point sprites were not implemented and they had been since 2026-09-20.
     * `gl_Fog` is the example now because it is still true: there is no struct type here. */
    GLuint pc = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(pc, "#version 120\nvoid main() { gl_FragColor = vec4(gl_Fog.color); }\n");
    glCompileShader(pc);
    glGetShaderiv(pc, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);
    char pclog[256] = {0};
    glGetShaderInfoLog(pc, (GLsizei)sizeof(pclog), NULL, pclog);
    ASSERT_TRUE(strstr(pclog, "structs") != NULL);

    /* **And `gl_PointCoord` compiles**, because it is an input now. */
    GLuint ok_pc = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(ok_pc, "void main() { gl_FragColor = vec4(gl_PointCoord, 0.0, 1.0); }\n");
    glCompileShader(ok_pc);
    glGetShaderiv(ok_pc, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    /*
     * **Its two restrictions are refused at link, and each says why.** Both are real properties
     * of this implementation rather than omissions:
     *
     *   - `gl_PointCoord` is texture coordinate 0's interpolant, here and on the part (where the
     *     substitution is `SPI_PS_INPUT_CNTL.PT_SPRITE_TEX`), so it cannot share a program with
     *     `gl_TexCoord`;
     *   - a point is expanded into its square *before* the vertex stage, in object space through
     *     the inverse MVP, so a vertex shader recomputes four corners from identical attributes
     *     and collapses the square. A point that silently vanishes is what this refuses.
     */
    {
        /* The vertex-shader refusal first, because it is the one that says whether
         * `hw_reads_point_coord` is being seen at all. */
        GLuint vs_first = glCreateShader(GL_VERTEX_SHADER);
        source_of(vs_first, "#version 120\nvoid main() { gl_Position = gl_Vertex; }\n");
        glCompileShader(vs_first);
        GLuint pc_own = glCreateShader(GL_FRAGMENT_SHADER);
        source_of(pc_own, "void main() { gl_FragColor = vec4(gl_PointCoord, 0.0, 1.0); }\n");
        glCompileShader(pc_own);
        glGetShaderiv(pc_own, GL_COMPILE_STATUS, &status);
        ASSERT_EQ(status, GL_TRUE);
        GLuint probe_vs = glCreateProgram();
        glAttachShader(probe_vs, vs_first);
        glAttachShader(probe_vs, pc_own);
        glLinkProgram(probe_vs);
        GLint lk0 = 0;
        glGetProgramiv(probe_vs, GL_LINK_STATUS, &lk0);
        ASSERT_EQ(lk0, GL_FALSE);

        GLuint both = glCreateProgram();
        glAttachShader(both, ok_pc);
        GLuint clash = glCreateShader(GL_FRAGMENT_SHADER);
        source_of(clash, "#version 120\nvoid main() {\n"
                         "  gl_FragColor = vec4(gl_PointCoord, 0.0, 1.0) * gl_TexCoord[0];\n"
                         "}\n");
        glCompileShader(clash);
        /* **Asserted, because a shader that failed to compile links as a program with no
         * fragment stage** - which succeeds, and would read as the refusal not firing. That is
         * exactly how this test first failed. */
        glGetShaderiv(clash, GL_COMPILE_STATUS, &status);
        ASSERT_EQ(status, GL_TRUE);
        GLuint pc_tc = glCreateProgram();
        glAttachShader(pc_tc, clash);
        glLinkProgram(pc_tc);
        GLint lk = 0;
        glGetProgramiv(pc_tc, GL_LINK_STATUS, &lk);
        ASSERT_EQ(lk, GL_FALSE);
        char lg[256] = {0};
        glGetProgramInfoLog(pc_tc, (GLsizei)sizeof(lg), NULL, lg);
        ASSERT_TRUE(strstr(lg, "gl_TexCoord") != NULL);

        /* A fragment shader on its own links: the fixed-function vertex stage transforms the
         * expansion as it was built. */
        glLinkProgram(both);
        glGetProgramiv(both, GL_LINK_STATUS, &lk);
        ASSERT_EQ(lk, GL_TRUE);

        /* With a vertex shader it does not, and the log names the expansion. */
        GLuint vs_pc = glCreateShader(GL_VERTEX_SHADER);
        source_of(vs_pc, "#version 120\nvoid main() { gl_Position = gl_Vertex; }\n");
        glCompileShader(vs_pc);
        GLuint with_vs = glCreateProgram();
        glAttachShader(with_vs, vs_pc);
        glAttachShader(with_vs, ok_pc);
        glLinkProgram(with_vs);
        glGetProgramiv(with_vs, GL_LINK_STATUS, &lk);
        ASSERT_EQ(lk, GL_FALSE);
        char lg2[256] = {0};
        glGetProgramInfoLog(with_vs, (GLsizei)sizeof(lg2), NULL, lg2);
        ASSERT_TRUE(strstr(lg2, "vertex stage") != NULL);
    }

    GLuint ls = glCreateShader(GL_VERTEX_SHADER);
    source_of(ls, "void main() { gl_Position = vec4(gl_LightSource[0].diffuse); }\n");
    glCompileShader(ls);
    glGetShaderiv(ls, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);
    memset(pclog, 0, sizeof(pclog));
    glGetShaderInfoLog(ls, (GLsizei)sizeof(pclog), NULL, pclog);
    ASSERT_TRUE(strstr(pclog, "struct") != NULL);

    glContextDestroy(ctx);
}

static void test_gl2_builtins_are_known_to_the_compiler(void) {
    void *ctx = gl2_context();

    /* The genType overloads, the geometric functions and a texture lookup, in one shader - none
     * of which is declared anywhere and all of which have to type. */
    GLuint fs = compiled(GL_FRAGMENT_SHADER,
                         "uniform sampler2D tex;\n"
                         "varying vec2 uv;\n"
                         "varying vec3 n;\n"
                         "void main() {\n"
                         "  vec3 l = normalize(n);\n"
                         "  float d = max(dot(l, vec3(0.0, 0.0, 1.0)), 0.0);\n"
                         "  vec4 t = texture2D(tex, uv);\n"
                         "  float f = smoothstep(0.0, 1.0, d);\n"
                         "  gl_FragColor = mix(t, vec4(1.0), f) * clamp(d, 0.0, 1.0);\n"
                         "}\n");
    (void)fs;

    /* `dFdx` exists in a fragment shader and does not exist in a vertex shader - the
     * specification makes the second an error rather than a function that returns zero. */
    GLuint ok = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(ok, "varying float v;\nvoid main() { gl_FragColor = vec4(dFdx(v)); }\n");
    glCompileShader(ok);
    GLint status = -1;
    glGetShaderiv(ok, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    GLuint wrong = glCreateShader(GL_VERTEX_SHADER);
    source_of(wrong, "attribute float v;\nvoid main() { gl_Position = vec4(dFdx(v)); }\n");
    glCompileShader(wrong);
    glGetShaderiv(wrong, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* The asymmetry that matters: `min(genType, float)` is legal and `min(float, genType)` is
     * not. An implementation that allowed a scalar anywhere would accept the second and be
     * wrong everywhere else. */
    GLuint scalar_ok = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(scalar_ok, "varying vec3 v;\nvoid main(){ gl_FragColor = vec4(min(v, 0.5), 1.0); }\n");
    glCompileShader(scalar_ok);
    glGetShaderiv(scalar_ok, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    GLuint scalar_bad = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(scalar_bad, "varying vec3 v;\nvoid main(){ gl_FragColor = vec4(min(0.5, v), 1.0); }\n");
    glCompileShader(scalar_bad);
    glGetShaderiv(scalar_bad, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* `cross` is vec3 only. */
    GLuint cross_bad = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(cross_bad, "varying vec2 v;\nvoid main(){ gl_FragColor = vec4(cross(v, v), 0.0, 1.0); }\n");
    glCompileShader(cross_bad);
    glGetShaderiv(cross_bad, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* The fixed-function built-in uniforms, which is how a port mixes a shader with the matrix
     * stack it already sets. */
    GLuint ff = glCreateShader(GL_VERTEX_SHADER);
    source_of(ff, "void main() { gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }\n");
    glCompileShader(ff);
    glGetShaderiv(ff, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    glContextDestroy(ctx);
}

static void test_gl2_glsl_120_converts_int_to_float(void) {
    void *ctx = gl2_context();
    GLint status = -1;

    /* **The rule GLSL 1.20 exists for, from a port's point of view.** `vec3 * 2` and
     * `clamp(v, 0, 1)` are how shader authors write, and 1.10 refuses both - which is correct
     * for 1.10 and is the wall a `#version 120` shader hits on its first line of arithmetic. */
    static const char *const MIXED =
        "attribute vec3 pos;\n"
        "varying vec3 v;\n"
        "float half_of(float x) { return x * 0.5; }\n"
        "void main() {\n"
        "  float f = 1;\n"                       /* initialiser */
        "  f = 2;\n"                             /* assignment */
        "  vec3 scaled = pos * 2;\n"             /* operator, scalar against a vector */
        "  float mixed = 1 + 1.0;\n"             /* operator, scalar against a scalar */
        "  float called = half_of(3);\n"         /* argument */
        "  float chosen = (f > 0.0) ? 1 : 2.0;\n"/* the two branches of ?: */
        "  v = scaled * clamp(f, 0, 1) * mixed * called * chosen;\n" /* a built-in */
        "  gl_Position = vec4(pos, 1.0);\n"
        "}\n";

    GLuint ok120 = glCreateShader(GL_VERTEX_SHADER);
    {
        char src[1024];
        strcpy(src, "#version 120\n");
        strcat(src, MIXED);
        const GLchar *s[1];
        s[0] = src;
        glShaderSource(ok120, 1, s, NULL);
    }
    glCompileShader(ok120);
    glGetShaderiv(ok120, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[256] = {0};
        glGetShaderInfoLog(ok120, (GLsizei)sizeof(log), NULL, log);
        printf("\n    1.20 compile failed: %s\n", log);
    }
    ASSERT_EQ(status, GL_TRUE);

    /* **The same source is an error in 1.10**, which is the half that has to keep working: a
     * front end that converted for both would accept shaders the specification rejects, and the
     * author would find out on somebody else's driver. */
    GLuint bad110 = glCreateShader(GL_VERTEX_SHADER);
    source_of(bad110, MIXED);
    glCompileShader(bad110);
    glGetShaderiv(bad110, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* **The conversion goes one way.** float to int is not implicit in either version, which is
     * the half people expect to work and which does not. */
    GLuint wrong = glCreateShader(GL_VERTEX_SHADER);
    source_of(wrong,
              "#version 120\n"
              "attribute vec3 pos;\n"
              "void main() { int i = 1.0; gl_Position = vec4(pos, float(i)); }\n");
    glCompileShader(wrong);
    glGetShaderiv(wrong, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* 1.20's qualifiers and its two matrix built-ins, and both refused in a 1.10 shader. */
    GLuint quals = glCreateShader(GL_VERTEX_SHADER);
    source_of(quals,
              "#version 120\n"
              "invariant centroid varying vec3 v;\n"
              "attribute vec3 pos;\n"
              "invariant gl_Position;\n"
              "void main() {\n"
              "  mat3 m = transpose(outerProduct(pos, pos));\n"
              "  v = m * pos;\n"
              "  gl_Position = vec4(pos, 1.0);\n"
              "}\n");
    glCompileShader(quals);
    glGetShaderiv(quals, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[256] = {0};
        glGetShaderInfoLog(quals, (GLsizei)sizeof(log), NULL, log);
        printf("\n    1.20 qualifiers failed: %s\n", log);
    }
    ASSERT_EQ(status, GL_TRUE);

    GLuint quals110 = glCreateShader(GL_VERTEX_SHADER);
    source_of(quals110,
              "#version 110\n"
              "centroid varying vec3 v;\n"
              "attribute vec3 pos;\n"
              "void main() { v = pos; gl_Position = vec4(pos, 1.0); }\n");
    glCompileShader(quals110);
    glGetShaderiv(quals110, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    GLuint tr110 = glCreateShader(GL_VERTEX_SHADER);
    source_of(tr110,
              "#version 110\n"
              "uniform mat3 m;\n"
              "attribute vec3 pos;\n"
              "void main() { gl_Position = vec4(transpose(m) * pos, 1.0); }\n");
    glCompileShader(tr110);
    glGetShaderiv(tr110, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    glContextDestroy(ctx);
}

static void test_gl2_arrays_are_typed_and_bounded(void) {
    void *ctx = gl2_context();

    GLuint ok = glCreateShader(GL_VERTEX_SHADER);
    source_of(ok,
              "uniform vec4 palette[4];\n"
              "attribute float which;\n"
              "void main() { gl_Position = palette[1] * which; }\n");
    glCompileShader(ok);
    GLint status = -1;
    glGetShaderiv(ok, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    /* **An element keeps the array's element type.** Before arrays were understood, indexing a
     * `vec4` array read as indexing a vec4 and produced a float - which then failed several
     * lines away with a message about the wrong thing. */
    GLuint mistyped = glCreateShader(GL_VERTEX_SHADER);
    source_of(mistyped,
              "uniform vec4 palette[4];\n"
              "void main() { float f = palette[1]; gl_Position = vec4(f); }\n");
    glCompileShader(mistyped);
    glGetShaderiv(mistyped, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* A constant index past the end is a compile error, not a read of whatever follows. */
    GLuint over = glCreateShader(GL_VERTEX_SHADER);
    source_of(over,
              "uniform vec4 palette[4];\n"
              "void main() { gl_Position = palette[4]; }\n");
    glCompileShader(over);
    glGetShaderiv(over, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * Linking
 * ------------------------------------------------------------------------- */

static void test_gl2_link_builds_the_interface(void) {
    void *ctx = gl2_context();

    const GLuint vs = compiled(GL_VERTEX_SHADER, VS_SIMPLE);
    const GLuint fs = compiled(GL_FRAGMENT_SHADER, FS_SIMPLE);
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* Attaching twice is GL_INVALID_OPERATION, and leaves the count alone. */
    glAttachShader(prog, vs);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    GLint n = 0;
    glGetProgramiv(prog, GL_ATTACHED_SHADERS, &n);
    ASSERT_EQ(n, 2);

    /* Before the link there are no locations to ask for, and asking is an error rather than a
     * -1 that looks like "no such uniform". */
    ASSERT_EQ(glGetUniformLocation(prog, "mvp"), -1);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {0};
        glGetProgramInfoLog(prog, (GLsizei)sizeof(log), NULL, log);
        printf("\n    link failed: %s\n", log);
    }
    ASSERT_EQ(linked, GL_TRUE);

    glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
    ASSERT_EQ(n, 1);
    glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &n);
    ASSERT_EQ(n, 2);

    GLint size = 0;
    GLenum type = 0;
    char name[32] = {0};
    GLsizei len = 0;
    glGetActiveUniform(prog, 0, (GLsizei)sizeof(name), &len, &size, &type, name);
    ASSERT_EQ(type, (GLenum)GL_FLOAT_MAT4);
    ASSERT_EQ(size, 1);
    ASSERT_TRUE(strcmp(name, "mvp") == 0);
    ASSERT_EQ(len, 3);

    ASSERT_TRUE(glGetUniformLocation(prog, "mvp") >= 0);
    /* A name nothing declares is -1 and **not** an error: the specification defines -1 so a
     * program need not branch on a uniform the linker removed. */
    ASSERT_EQ(glGetUniformLocation(prog, "nothing"), -1);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    const GLint pos = glGetAttribLocation(prog, "pos");
    const GLint col = glGetAttribLocation(prog, "colour");
    ASSERT_TRUE(pos >= 0 && col >= 0 && pos != col);
    ASSERT_EQ(glGetAttribLocation(prog, "gl_Vertex"), -1);

    glUseProgram(prog);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLint current = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current);
    ASSERT_EQ(current, (GLint)prog);

    glContextDestroy(ctx);
}

static void test_gl2_link_refuses_what_cannot_run(void) {
    void *ctx = gl2_context();

    /* A vertex shader that never writes gl_Position. The specification leaves the result
     * undefined; a blank screen is the worst diagnostic there is, so this is a link error. */
    {
        const GLuint vs = compiled(GL_VERTEX_SHADER,
                                   "attribute vec4 pos;\n"
                                   "varying vec4 v;\n"
                                   "void main() { v = pos; }\n");
        const GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glLinkProgram(prog);
        GLint linked = -1;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_FALSE);
        GLint log_len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &log_len);
        ASSERT_TRUE(log_len > 1);
        /* A program that did not link cannot be made current, and the failure does not drop the
         * caller back to fixed function behind its back. */
        glUseProgram(prog);
        ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    }

    /* gl_Position written through a helper still counts: a whole-unit sweep rather than a walk
     * of `main`, because that is how shaders are actually written. */
    {
        const GLuint vs = compiled(GL_VERTEX_SHADER,
                                   "attribute vec4 pos;\n"
                                   "void place(vec4 p) { gl_Position = p; }\n"
                                   "void main() { place(pos); }\n");
        const GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glLinkProgram(prog);
        GLint linked = -1;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_TRUE);
    }

    /* A varying the fragment shader reads and the vertex shader does not write is a link
     * error, not a varying that interpolates zero. */
    {
        const GLuint vs = compiled(GL_VERTEX_SHADER,
                                   "attribute vec4 pos;\nvoid main() { gl_Position = pos; }\n");
        const GLuint fs = compiled(GL_FRAGMENT_SHADER,
                                   "varying vec3 missing;\n"
                                   "void main() { gl_FragColor = vec4(missing, 1.0); }\n");
        const GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint linked = -1;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_FALSE);
    }

    /* And one whose type differs between the stages. */
    {
        const GLuint vs = compiled(GL_VERTEX_SHADER,
                                   "attribute vec4 pos;\n"
                                   "varying vec3 v;\n"
                                   "void main() { v = pos.xyz; gl_Position = pos; }\n");
        const GLuint fs = compiled(GL_FRAGMENT_SHADER,
                                   "varying vec2 v;\n"
                                   "void main() { gl_FragColor = vec4(v, 0.0, 1.0); }\n");
        const GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint linked = -1;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_FALSE);
    }

    /* An attached shader that has not compiled fails the link rather than being left out of
     * it - which would run the fixed-function half in its place and report success. */
    {
        const GLuint vs = compiled(GL_VERTEX_SHADER,
                                   "attribute vec4 pos;\nvoid main() { gl_Position = pos; }\n");
        GLuint broken = glCreateShader(GL_FRAGMENT_SHADER);
        source_of(broken, "void main() { this is not glsl }\n");
        glCompileShader(broken);
        const GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, broken);
        glLinkProgram(prog);
        GLint linked = -1;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_FALSE);
    }

    glContextDestroy(ctx);
}

static void test_gl2_bind_attrib_location_applies_at_the_next_link(void) {
    void *ctx = gl2_context();

    const GLuint vs = compiled(GL_VERTEX_SHADER, VS_SIMPLE);
    const GLuint fs = compiled(GL_FRAGMENT_SHADER, FS_SIMPLE);
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);

    glBindAttribLocation(prog, 5, "pos");
    glLinkProgram(prog);
    ASSERT_EQ(glGetAttribLocation(prog, "pos"), 5);
    /* The other attribute takes the lowest free slot, which is not the one that was claimed. */
    const GLint col = glGetAttribLocation(prog, "colour");
    ASSERT_TRUE(col >= 0 && col != 5);

    /* **A binding after the link does not take effect until the next one**, which is the
     * mistake this API most invites: a program that binds, draws, and wonders why. */
    glBindAttribLocation(prog, 7, "pos");
    ASSERT_EQ(glGetAttribLocation(prog, "pos"), 5);
    glLinkProgram(prog);
    ASSERT_EQ(glGetAttribLocation(prog, "pos"), 7);

    /* A binding naming something this shader does not declare is not an error and simply does
     * not appear - which is what lets one binding table serve several programs. */
    glBindAttribLocation(prog, 9, "absent");
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glLinkProgram(prog);
    ASSERT_EQ(glGetAttribLocation(prog, "absent"), -1);

    /* The language's own prefix cannot be bound: the fixed-function attributes have fixed
     * homes. */
    glBindAttribLocation(prog, 3, "gl_Vertex");
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glBindAttribLocation(prog, OOPS_GL_MAX_VERTEX_ATTRIBS, "pos");
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * Deferred deletion
 * ------------------------------------------------------------------------- */

static void test_gl2_deletion_is_deferred_and_observable(void) {
    void *ctx = gl2_context();

    const GLuint vs = compiled(GL_VERTEX_SHADER, VS_SIMPLE);
    const GLuint fs = compiled(GL_FRAGMENT_SHADER, FS_SIMPLE);
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    ASSERT_EQ(linked, GL_TRUE);

    /* The idiom every GL 2.0 program uses: delete the shaders the moment they are linked. */
    glDeleteShader(vs);
    glDeleteShader(fs);

    /* **`glIsShader` says no and `glGetShaderiv` still answers.** Both are required and an
     * implementation with one of them passes half the tests that exist for this. */
    ASSERT_EQ(glIsShader(vs), GL_FALSE);
    GLint flagged = -1;
    glGetShaderiv(vs, GL_DELETE_STATUS, &flagged);
    ASSERT_EQ(flagged, GL_TRUE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* And the program is still linked and still usable, which is the point of the deferral. */
    glUseProgram(prog);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    ASSERT_TRUE(glGetUniformLocation(prog, "mvp") >= 0);

    /* Detaching the last holder is what finally reaps it - after which the name is gone and a
     * query on it is GL_INVALID_VALUE rather than a read of a freed object. */
    glDetachShader(prog, vs);
    glGetShaderiv(vs, GL_DELETE_STATUS, &flagged);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

    /* Detaching something that is not attached is GL_INVALID_OPERATION. */
    glDetachShader(prog, fs);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDetachShader(prog, fs);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);  /* fs has now been reaped too */

    glContextDestroy(ctx);
}

static void test_gl2_a_deleted_program_in_use_keeps_running(void) {
    void *ctx = gl2_context();

    const GLuint vs = compiled(GL_VERTEX_SHADER, VS_SIMPLE);
    const GLuint fs = compiled(GL_FRAGMENT_SHADER, FS_SIMPLE);
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);

    glDeleteProgram(prog);
    ASSERT_EQ(glIsProgram(prog), GL_FALSE);
    /* Still current, still linked - the deletion waits for something else to be made current. */
    GLint current = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current);
    ASSERT_EQ(current, (GLint)prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    ASSERT_EQ(linked, GL_TRUE);

    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * Uniforms
 * ------------------------------------------------------------------------- */

static void test_gl2_uniforms_take_the_matching_command(void) {
    void *ctx = gl2_context();

    const GLuint vs = compiled(GL_VERTEX_SHADER,
                               "uniform mat4 mvp;\n"
                               "uniform float scale;\n"
                               "uniform int count;\n"
                               "attribute vec4 pos;\n"
                               "void main() { gl_Position = mvp * pos * scale * float(count); }\n");
    const GLuint fs = compiled(GL_FRAGMENT_SHADER,
                               "uniform sampler2D tex;\n"
                               "uniform vec4 tint;\n"
                               "void main() { gl_FragColor = texture2D(tex, vec2(0.0)) * tint; }\n");
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    ASSERT_EQ(linked, GL_TRUE);

    /* Setting a uniform with no program in use is an error, not a value kept for later. */
    const GLint scale = glGetUniformLocation(prog, "scale");
    glUniform1f(scale, 2.0f);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

    glUseProgram(prog);
    glUniform1f(scale, 2.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLfloat got = 0.0f;
    glGetUniformfv(prog, scale, &got);
    ASSERT_TRUE(fabsf(got - 2.0f) < 1e-6f);

    /* **The command has to match the declared type.** `glUniform1i` on a float is an error, not
     * a conversion - which is what stops a `glUniform1i(loc, 1)` meant for a sampler from
     * quietly setting a float somewhere else. */
    glUniform1i(scale, 3);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    const GLint count = glGetUniformLocation(prog, "count");
    glUniform1f(count, 3.0f);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glUniform1i(count, 3);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* The width has to match too: a vec4 uniform takes glUniform4f and nothing else. */
    const GLint tint = glGetUniformLocation(prog, "tint");
    glUniform3f(tint, 1.0f, 0.0f, 0.0f);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glUniform4f(tint, 0.25f, 0.5f, 0.75f, 1.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLfloat rgba[4] = {0};
    glGetUniformfv(prog, tint, rgba);
    ASSERT_TRUE(fabsf(rgba[2] - 0.75f) < 1e-6f);

    /* A matrix takes only glUniformMatrix, and `transpose` is applied once, here - so the
     * stored value is column-major whichever way the caller had it. */
    const GLint mvp = glGetUniformLocation(prog, "mvp");
    glUniform4f(mvp, 0.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    GLfloat m[16];
    for (int i = 0; i < 16; i++) m[i] = (GLfloat)i;
    glUniformMatrix4fv(mvp, 1, GL_FALSE, m);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLfloat back[16] = {0};
    glGetUniformfv(prog, mvp, back);
    ASSERT_TRUE(fabsf(back[1] - 1.0f) < 1e-6f);
    glUniformMatrix4fv(mvp, 1, GL_TRUE, m);
    glGetUniformfv(prog, mvp, back);
    /* Element [1] of the stored matrix is now element [4] of what was passed. */
    ASSERT_TRUE(fabsf(back[1] - 4.0f) < 1e-6f);

    /* A sampler takes the integer form only: setting one with a float would be a unit number
     * that is nearly an integer. */
    const GLint tex = glGetUniformLocation(prog, "tex");
    glUniform1f(tex, 1.0f);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glUniform1i(tex, 1);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLint unit = -1;
    glGetUniformiv(prog, tex, &unit);
    ASSERT_EQ(unit, 1);

    /* **A location of -1 is silently ignored**, by definition - so a program need not branch on
     * a uniform the linker removed. */
    glUniform1f(-1, 5.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* Relinking resets every uniform to zero, which is the rule programs forget. */
    glLinkProgram(prog);
    glUseProgram(prog);
    glGetUniformfv(prog, glGetUniformLocation(prog, "scale"), &got);
    ASSERT_TRUE(fabsf(got) < 1e-6f);

    glContextDestroy(ctx);
}

static void test_gl2_uniform_arrays_step_by_location(void) {
    void *ctx = gl2_context();

    const GLuint vs = compiled(GL_VERTEX_SHADER,
                               "uniform vec4 palette[3];\n"
                               "uniform float after;\n"
                               "attribute vec4 pos;\n"
                               "void main() { gl_Position = pos * palette[2] * after; }\n");
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    ASSERT_EQ(linked, GL_TRUE);
    glUseProgram(prog);

    const GLint base = glGetUniformLocation(prog, "palette");
    ASSERT_TRUE(base >= 0);
    /* The unbracketed name is element zero, and the bracketed forms are consecutive. */
    ASSERT_EQ(glGetUniformLocation(prog, "palette[0]"), base);
    ASSERT_EQ(glGetUniformLocation(prog, "palette[2]"), base + 2);
    /* Past the end is -1, not a location into whatever comes next. */
    ASSERT_EQ(glGetUniformLocation(prog, "palette[3]"), -1);

    /* **The uniform declared after the array must not collide with its elements**, which is
     * exactly what numbering locations per uniform rather than per element would do. */
    const GLint after = glGetUniformLocation(prog, "after");
    ASSERT_TRUE(after >= base + 3);

    const GLfloat three[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    glUniform4fv(base, 3, three);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLfloat got[4] = {0};
    glGetUniformfv(prog, base + 1, got);
    ASSERT_TRUE(fabsf(got[1] - 1.0f) < 1e-6f);

    /* Starting part-way and running past the end: the excess is ignored, and what was in range
     * still landed. The uniform after the array is untouched. */
    const GLfloat two[8] = {9, 9, 9, 9, 8, 8, 8, 8};
    glUniform4fv(base + 2, 2, two);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glGetUniformfv(prog, base + 2, got);
    ASSERT_TRUE(fabsf(got[0] - 9.0f) < 1e-6f);
    GLfloat tail = -1.0f;
    glGetUniformfv(prog, after, &tail);
    ASSERT_TRUE(fabsf(tail) < 1e-6f);

    /* A count above one on something that is not an array is an error. */
    glUniform1fv(after, 2, two);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * Generic vertex attributes
 * ------------------------------------------------------------------------- */

static void test_gl2_vertex_attrib_state(void) {
    void *ctx = gl2_context();

    /* The initial current value is (0, 0, 0, 1) - a point, not a direction - so an attribute
     * nothing has set lands at the origin under a transform rather than being degenerate. */
    GLfloat cur[4] = {9, 9, 9, 9};
    glGetVertexAttribfv(3, GL_CURRENT_VERTEX_ATTRIB, cur);
    ASSERT_TRUE(cur[0] == 0.0f && cur[1] == 0.0f && cur[2] == 0.0f && cur[3] == 1.0f);

    /* And the short forms fill rather than leave: a 2f after a 4f gives z = 0 and w = 1, not
     * whatever the 4f left behind. */
    glVertexAttrib4f(3, 1.0f, 2.0f, 3.0f, 4.0f);
    glVertexAttrib2f(3, 5.0f, 6.0f);
    glGetVertexAttribfv(3, GL_CURRENT_VERTEX_ATTRIB, cur);
    ASSERT_TRUE(cur[0] == 5.0f && cur[1] == 6.0f && cur[2] == 0.0f && cur[3] == 1.0f);

    /* The normalised forms scale and the plain ones convert - the difference people reach for
     * the wrong one over. */
    const GLubyte bytes[4] = {255, 128, 0, 255};
    glVertexAttrib4ubv(4, bytes);
    glGetVertexAttribfv(4, GL_CURRENT_VERTEX_ATTRIB, cur);
    ASSERT_TRUE(fabsf(cur[0] - 255.0f) < 1e-6f);
    glVertexAttrib4Nubv(4, bytes);
    glGetVertexAttribfv(4, GL_CURRENT_VERTEX_ATTRIB, cur);
    ASSERT_TRUE(fabsf(cur[0] - 1.0f) < 1e-6f);

    /* A signed byte of -128 divides by 127 and is clamped to -1, which is the specification's
     * rule and is why it is not simply a divide by 128. */
    const GLbyte sbytes[4] = {-128, 127, 0, 0};
    glVertexAttrib4Nbv(5, sbytes);
    glGetVertexAttribfv(5, GL_CURRENT_VERTEX_ATTRIB, cur);
    ASSERT_TRUE(fabsf(cur[0] + 1.0f) < 1e-6f);
    ASSERT_TRUE(fabsf(cur[1] - 1.0f) < 1e-6f);

    static const GLfloat data[6] = {0, 0, 1, 0, 0, 1};
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8, data);
    glEnableVertexAttribArray(2);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLint iv = 0;
    glGetVertexAttribiv(2, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &iv);
    ASSERT_EQ(iv, GL_TRUE);
    glGetVertexAttribiv(2, GL_VERTEX_ATTRIB_ARRAY_SIZE, &iv);
    ASSERT_EQ(iv, 2);
    glGetVertexAttribiv(2, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &iv);
    ASSERT_EQ(iv, 8);
    glGetVertexAttribiv(2, GL_VERTEX_ATTRIB_ARRAY_TYPE, &iv);
    ASSERT_EQ(iv, (GLint)GL_FLOAT);
    GLvoid *ptr = NULL;
    glGetVertexAttribPointerv(2, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
    ASSERT_TRUE(ptr == (GLvoid *)data);

    /* An index past the limit is GL_INVALID_VALUE rather than a write past the table. */
    glEnableVertexAttribArray(OOPS_GL_MAX_VERTEX_ATTRIBS);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
    glVertexAttribPointer(0, 5, GL_FLOAT, GL_FALSE, 0, data);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
    glVertexAttribPointer(0, 4, GL_TEXTURE_2D, GL_FALSE, 0, data);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * What the implementation says about itself
 * ------------------------------------------------------------------------- */

static void test_gl2_limits_and_version_are_answered(void) {
    void *ctx = gl2_context();

    GLint v = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &v);
    ASSERT_EQ(v, OOPS_GL_MAX_VERTEX_ATTRIBS);
    ASSERT_TRUE(v >= 16);          /* GL 2.0's own minimum */
    glGetIntegerv(GL_MAX_VARYING_FLOATS, &v);
    ASSERT_EQ(v, OOPS_GL_MAX_VARYING_FLOATS);
    ASSERT_TRUE(v >= 32);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &v);
    ASSERT_EQ(v, OOPS_GL_MAX_TEXTURE_UNITS);
    ASSERT_TRUE(v >= 2);
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &v);
    /* Zero is legal and is what this is: GL 2.0's minimum, and a vertex shader here has no
     * sampler. Reporting more than exists would make a program take a path that then fails. */
    ASSERT_EQ(v, 0);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &v);
    ASSERT_TRUE(v >= 1);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* The shading language's version is its own string, separate from GL_VERSION. */
    const GLubyte *sl = glGetString(GL_SHADING_LANGUAGE_VERSION);
    ASSERT_TRUE(sl != NULL);
    /* **The highest dialect the front end takes**, which is what the specification asks this to
     * report - not the one the GL badge pairs with. A shader may still declare `#version 110`
     * and be held to 1.10's rules; this number is a ceiling, not a mode. */
    ASSERT_TRUE(strncmp((const char *)sl, "1.20", 4) == 0);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * Drawing
 *
 * From here on the checks are pixels. **A program that links and reports every location
 * correctly and then draws the wrong colour has passed everything above**, which is why these
 * exist: the object model is the half that is easy to get right.
 *
 * The reference these measure is the software path, which is oops-gl's definition of the
 * answer. The console path for a GL 2.0 program does not exist yet and refuses the draw rather
 * than running the fixed-function shaders in its place, so there is nothing here that can
 * quietly differ between the two.
 * ------------------------------------------------------------------------- */

#define GL2_W 64
#define GL2_H 64

typedef struct {
    oops_display_t *disp;
    void *ctx;
    uint32_t *fb;
} gl2_target_t;

static gl2_target_t gl2_target(void) {
    gl2_target_t t;
    t.disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, GL2_W, GL2_H);
    t.ctx = glContextCreate(t.disp);
    glContextMakeCurrent(t.ctx);
    glContextSetVersion(2, 0);   /* see gl2_context: without this there is no GL 2.0 here */
    t.fb = oops_display_get_framebuffer(t.disp);
    glViewport(0, 0, GL2_W, GL2_H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    return t;
}

/* The framebuffer's row 0 is the top; GL's window y counts up from the bottom. These take the
 * rasteriser's own orientation, which is what `fb` is. */
static uint32_t px(const gl2_target_t *t, int x, int y) { return t->fb[y * GL2_W + x]; }
static int px_r(uint32_t p) { return (int)((p >> 16) & 0xffu); }
static int px_g(uint32_t p) { return (int)((p >> 8) & 0xffu); }
static int px_b(uint32_t p) { return (int)(p & 0xffu); }

static GLuint linked_program(const char *vs_src, const char *fs_src) {
    const GLuint vs = compiled(GL_VERTEX_SHADER, vs_src);
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    if (fs_src) {
        const GLuint fs = compiled(GL_FRAGMENT_SHADER, fs_src);
        glAttachShader(prog, fs);
    }
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[256] = {0};
        glGetProgramInfoLog(prog, (GLsizei)sizeof(log), NULL, log);
        printf("\n    link failed: %s\n", log);
    }
    ASSERT_EQ(linked, GL_TRUE);
    return prog;
}

/* A screen-filling quad through generic attribute slot `loc`, as two triangles. */
static void draw_quad(GLint loc, float z) {
    const GLfloat corners[4][3] = {
        {-0.8f, -0.8f, 0.0f}, {0.8f, -0.8f, 0.0f}, {0.8f, 0.8f, 0.0f}, {-0.8f, 0.8f, 0.0f}};
    const int tri[6] = {0, 1, 2, 0, 2, 3};
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 6; i++) {
        const int c = tri[i];
        glVertexAttrib3f((GLuint)loc, corners[c][0], corners[c][1], z);
        /* The position is the attribute; glVertex is what pushes the vertex. Its own value is
         * unused by these shaders and is here because immediate mode needs it. */
        glVertex3f(corners[c][0], corners[c][1], z);
    }
    glEnd();
}

/*
 * **Structs, run rather than type-checked.** The front end accepting one says nothing about the
 * interpreter placing its members where sema said they were - and the two agreeing is the whole
 * contract, because a member read from the wrong place gives a colour rather than an error.
 *
 * Each shader below puts a different member into a different channel, so a layout that is off by
 * one names itself: the channels come back rotated rather than merely wrong.
 */
static void test_gl2_structs_run(void) {
    gl2_target_t t = gl2_target();

    /* Construct, then read each member back into its own channel. */
    {
        const GLuint prog = linked_program(
            "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
            "struct C { float r; float g; float b; };\n"
            "void main() {\n"
            "  C c = C(0.25, 0.5, 0.75);\n"
            "  gl_FragColor = vec4(c.r, c.g, c.b, 1.0);\n"
            "}\n");
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);     /* 0.25 */
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136);   /* 0.50 */
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200);   /* 0.75 */
    }

    /* A member that is a vector, and a swizzle of it - the two meanings of `.` in one line. */
    {
        const GLuint prog = linked_program(
            "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
            "struct M { float a; vec3 v; };\n"
            "void main() {\n"
            "  M m = M(0.0, vec3(0.25, 0.5, 0.75));\n"
            "  gl_FragColor = vec4(m.v.x, m.v.y, m.v.z, 1.0);\n"
            "}\n");
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136);
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200);
    }

    /* **Assignment copies the whole struct**, not its first component. Writing to the copy must
     * not disturb the original, which is what a shared pointer would do. */
    {
        const GLuint prog = linked_program(
            "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
            "struct C { float r; float g; float b; };\n"
            "void main() {\n"
            "  C a = C(0.25, 0.5, 0.75);\n"
            "  C b = a;\n"
            "  b.r = 1.0;\n"
            "  gl_FragColor = vec4(a.r, b.g, b.b, 1.0);\n"
            "}\n");
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);     /* a.r still 0.25 */
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136);
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200);
    }

    /* A nested struct, where the inner one's position is added to the outer one's. */
    {
        const GLuint prog = linked_program(
            "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
            "struct In { float x; float y; };\n"
            "struct Out { float lead; In in2; };\n"
            "void main() {\n"
            "  Out o = Out(0.25, In(0.5, 0.75));\n"
            "  gl_FragColor = vec4(o.lead, o.in2.x, o.in2.y, 1.0);\n"
            "}\n");
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136);
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200);
    }

    /* Through a function, by value in and by value out. */
    {
        const GLuint prog = linked_program(
            "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
            "struct C { float r; float g; float b; };\n"
            "C darken(C c) { return C(c.r * 0.5, c.g * 0.5, c.b * 0.5); }\n"
            "void main() {\n"
            "  C c = darken(C(0.5, 1.0, 1.5));\n"
            "  gl_FragColor = vec4(c.r, c.g, c.b, 1.0);\n"
            "}\n");
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);     /* 0.5  * 0.5 = 0.25 */
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136);   /* 1.0  * 0.5 = 0.50 */
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200);   /* 1.5  * 0.5 = 0.75 */
    }

    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_a_program_draws(void) {
    gl2_target_t t = gl2_target();

    const GLuint prog = linked_program(
        "attribute vec3 pos;\n"
        "void main() { gl_Position = vec4(pos, 1.0); }\n",
        "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    ASSERT_TRUE(loc >= 0);
    glUseProgram(prog);

    draw_quad(loc, 0.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* The middle is the shader's green, and a corner outside the quad is still the clear. */
    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(mid) < 8 && px_g(mid) > 247 && px_b(mid) < 8);
    ASSERT_EQ(px(&t, 1, 1), 0xff000000u);

    /* **glUseProgram(0) goes back to the fixed-function pipeline**, which is what a program
     * that draws its HUD with glBegin after its scene depends on. */
    glUseProgram(0);
    glColor3f(1.0f, 0.0f, 0.0f);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.8f, -0.8f, 0.0f);
    glVertex3f(0.8f, -0.8f, 0.0f);
    glVertex3f(0.0f, 0.8f, 0.0f);
    glEnd();
    const uint32_t red = px(&t, GL2_W / 2, GL2_H / 2 + 8);
    ASSERT_TRUE(px_r(red) > 247 && px_g(red) < 8);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_uniforms_and_varyings_reach_the_pixels(void) {
    gl2_target_t t = gl2_target();

    /* A varying carrying the position, and a uniform scaling it - so the picture is a gradient
     * whose two ends differ, which a constant colour could not fake. */
    const GLuint prog = linked_program(
        "uniform float scale;\n"
        "attribute vec3 pos;\n"
        "varying vec2 uv;\n"
        "void main() {\n"
        "  uv = pos.xy * scale + vec2(0.5);\n"
        "  gl_Position = vec4(pos, 1.0);\n"
        "}\n",
        "varying vec2 uv;\n"
        "void main() { gl_FragColor = vec4(uv.x, uv.y, 0.0, 1.0); }\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    glUseProgram(prog);
    glUniform1f(glGetUniformLocation(prog, "scale"), 0.625f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    draw_quad(loc, 0.0f);

    /* uv.x runs 0 at the left edge of the quad to 1 at the right, so the left half is darker in
     * red than the right half and the middle is about half. */
    const uint32_t left = px(&t, GL2_W / 4, GL2_H / 2);
    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2);
    const uint32_t right = px(&t, 3 * GL2_W / 4, GL2_H / 2);
    ASSERT_TRUE(px_r(left) < px_r(mid));
    ASSERT_TRUE(px_r(mid) < px_r(right));
    ASSERT_TRUE(px_r(mid) > 110 && px_r(mid) < 145);
    /* uv.y is the other axis, so the same three pixels share a green. */
    ASSERT_TRUE(px_g(left) == px_g(mid) && px_g(mid) == px_g(right));
    ASSERT_TRUE(px_b(mid) == 0);

    /* **Changing the uniform changes the picture without relinking.** */
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform1f(glGetUniformLocation(prog, "scale"), 0.0f);
    draw_quad(loc, 0.0f);
    const uint32_t flat = px(&t, GL2_W / 4, GL2_H / 2);
    const uint32_t flat2 = px(&t, 3 * GL2_W / 4, GL2_H / 2);
    ASSERT_EQ(px_r(flat), px_r(flat2));
    ASSERT_TRUE(px_r(flat) > 120 && px_r(flat) < 135);  /* the constant 0.5 */

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_a_matrix_uniform_transforms(void) {
    gl2_target_t t = gl2_target();

    /* The idiom every GL 2.0 program is built on. A translation in the last column moves the
     * quad; taking the matrix as rows instead would move it along the wrong axis, which is the
     * failure a symmetric test scene hides. */
    const GLuint prog = linked_program(
        "uniform mat4 mvp;\n"
        "attribute vec3 pos;\n"
        "void main() { gl_Position = mvp * vec4(pos, 1.0); }\n",
        "void main() { gl_FragColor = vec4(1.0); }\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    glUseProgram(prog);

    /* Column-major: element 12 is the x translation. */
    GLfloat m[16] = {0.25f, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1};
    glUniformMatrix4fv(glGetUniformLocation(prog, "mvp"), 1, GL_FALSE, m);
    draw_quad(loc, 0.0f);

    /* A quarter-size quad shifted right: lit to the right of centre, dark to the left. */
    ASSERT_TRUE(px_r(px(&t, GL2_W / 2 + 12, GL2_H / 2)) > 247);
    ASSERT_EQ(px(&t, GL2_W / 2 - 12, GL2_H / 2), 0xff000000u);

    /* And with `transpose` the same numbers mean the other matrix, which moves it in y - the
     * check that the flag is applied rather than ignored. */
    glClear(GL_COLOR_BUFFER_BIT);
    glUniformMatrix4fv(glGetUniformLocation(prog, "mvp"), 1, GL_TRUE, m);
    draw_quad(loc, 0.0f);
    ASSERT_EQ(px(&t, GL2_W / 2 + 12, GL2_H / 2), 0xff000000u);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_discard_writes_nothing(void) {
    gl2_target_t t = gl2_target();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Discards the left half. **A discarded fragment writes no depth either**, which is what
     * the second draw below measures: something further away must still appear there. */
    const GLuint prog = linked_program(
        "attribute vec3 pos;\n"
        "varying float side;\n"
        "void main() { side = pos.x; gl_Position = vec4(pos, 1.0); }\n",
        "varying float side;\n"
        "void main() {\n"
        "  if (side < 0.0) discard;\n"
        "  gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0);\n"
        "}\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    glUseProgram(prog);
    draw_quad(loc, -0.5f);

    ASSERT_TRUE(px_b(px(&t, 3 * GL2_W / 4, GL2_H / 2)) > 247);
    ASSERT_EQ(px(&t, GL2_W / 4, GL2_H / 2), 0xff000000u);

    /* A second, further quad. On the right the first one's depth blocks it; on the left the
     * discarded fragments left the depth buffer alone, so it draws. */
    const GLuint flat = linked_program(
        "attribute vec3 pos;\n"
        "void main() { gl_Position = vec4(pos, 1.0); }\n",
        "void main() { gl_FragColor = vec4(1.0, 1.0, 0.0, 1.0); }\n");
    const GLint loc2 = glGetAttribLocation(flat, "pos");
    glUseProgram(flat);
    draw_quad(loc2, 0.5f);

    ASSERT_TRUE(px_b(px(&t, 3 * GL2_W / 4, GL2_H / 2)) > 247);   /* still blue */
    const uint32_t left = px(&t, GL2_W / 4, GL2_H / 2);
    ASSERT_TRUE(px_r(left) > 247 && px_g(left) > 247 && px_b(left) < 8);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_control_flow_and_functions_run(void) {
    gl2_target_t t = gl2_target();

    /* A loop, a user function with an `out` parameter, and the built-ins - all of which the
     * interpreter has to get right for any real shader. The sum below is 1+2+3+4 = 10, scaled
     * to 0.5, so the answer is a mid grey rather than a value any single mistake would land
     * on. */
    const GLuint prog = linked_program(
        "attribute vec3 pos;\n"
        "varying float v;\n"
        "void accumulate(out float total) {\n"
        "  total = 0.0;\n"
        "  for (int i = 1; i <= 4; i++) {\n"
        "    if (i == 3) { total += float(i); continue; }\n"
        "    total += float(i);\n"
        "  }\n"
        "}\n"
        "void main() {\n"
        "  float sum;\n"
        "  accumulate(sum);\n"
        "  v = sum * 0.05;\n"
        "  gl_Position = vec4(pos, 1.0);\n"
        "}\n",
        "varying float v;\n"
        "void main() {\n"
        "  float k = clamp(v, 0.0, 1.0);\n"
        "  gl_FragColor = vec4(k, sqrt(k * k), mix(0.0, k, 1.0), 1.0);\n"
        "}\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    glUseProgram(prog);
    draw_quad(loc, 0.0f);

    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(mid) > 120 && px_r(mid) < 135);
    ASSERT_EQ(px_r(mid), px_g(mid));
    ASSERT_EQ(px_r(mid), px_b(mid));

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_a_sampler_reads_its_own_unit(void) {
    gl2_target_t t = gl2_target();

    /* Two 2x2 textures on two units, and a shader that mixes them. **The texture enables are
     * never called**: a sampler's declared type names its target, and a program that relies on
     * glEnable would be relying on a rule GL 2.0 removed. */
    GLuint tex[2];
    glGenTextures(2, tex);
    const uint32_t red[4] = {0xff0000ffu, 0xff0000ffu, 0xff0000ffu, 0xff0000ffu};
    const uint32_t blue[4] = {0xffff0000u, 0xffff0000u, 0xffff0000u, 0xffff0000u};
    for (int i = 0; i < 2; i++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     (i == 0) ? red : blue);
    }
    glActiveTexture(GL_TEXTURE0);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    const GLuint prog = linked_program(
        "attribute vec3 pos;\n"
        "varying vec2 uv;\n"
        "void main() { uv = pos.xy * 0.5 + vec2(0.5); gl_Position = vec4(pos, 1.0); }\n",
        "uniform sampler2D first;\n"
        "uniform sampler2D second;\n"
        "varying vec2 uv;\n"
        "void main() {\n"
        "  gl_FragColor = mix(texture2D(first, uv), texture2D(second, uv), 0.5);\n"
        "}\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "first"), 0);
    glUniform1i(glGetUniformLocation(prog, "second"), 1);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    draw_quad(loc, 0.0f);

    /* Half red and half blue, with no green anywhere. Swapping the two samplers' units would
     * give the same answer here, so the second half of this test separates them. */
    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(mid) > 120 && px_r(mid) < 135);
    ASSERT_TRUE(px_b(mid) > 120 && px_b(mid) < 135);
    ASSERT_TRUE(px_g(mid) < 8);

    /* Both samplers on unit 1 is all blue, which only reads if `glUniform1i` really chose the
     * unit rather than the declaration order deciding it. */
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform1i(glGetUniformLocation(prog, "first"), 1);
    draw_quad(loc, 0.0f);
    const uint32_t all_blue = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_b(all_blue) > 247 && px_r(all_blue) < 8);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_attribute_arrays_feed_the_shader(void) {
    gl2_target_t t = gl2_target();

    /* `glVertexAttribPointer` and `glDrawArrays`, which is how a real port draws. **Every
     * vertex is fetched before any is shaded**, so an implementation that kept one current
     * value per slot would give every vertex the last one's - a triangle collapsed to a point
     * rather than the one below. */
    static const GLfloat verts[9] = {
        -0.8f, -0.8f, 0.0f,
         0.8f, -0.8f, 0.0f,
         0.0f,  0.8f, 0.0f};
    static const GLubyte cols[12] = {
        255, 0, 0, 255,
        0, 255, 0, 255,
        0, 0, 255, 255};

    const GLuint prog = linked_program(
        "attribute vec3 pos;\n"
        "attribute vec4 colour;\n"
        "varying vec4 vcol;\n"
        "void main() { vcol = colour; gl_Position = vec4(pos, 1.0); }\n",
        "varying vec4 vcol;\n"
        "void main() { gl_FragColor = vcol; }\n");
    const GLint pos = glGetAttribLocation(prog, "pos");
    const GLint col = glGetAttribLocation(prog, "colour");
    ASSERT_TRUE(pos >= 0 && col >= 0);
    glUseProgram(prog);

    glVertexAttribPointer((GLuint)pos, 3, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray((GLuint)pos);
    /* **Normalised**, so 255 is 1.0 and not 255.0 - the difference this flag exists for. */
    glVertexAttribPointer((GLuint)col, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, cols);
    glEnableVertexAttribArray((GLuint)col);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* Each corner takes its own vertex's colour, so the three are different and each is
     * dominated by a different channel. */
    const uint32_t bl = px(&t, 8, GL2_H - 8);
    const uint32_t br = px(&t, GL2_W - 9, GL2_H - 8);
    const uint32_t top = px(&t, GL2_W / 2, 8);
    ASSERT_TRUE(px_r(bl) > px_g(bl) && px_r(bl) > px_b(bl));
    ASSERT_TRUE(px_g(br) > px_r(br) && px_g(br) > px_b(br));
    ASSERT_TRUE(px_b(top) > px_r(top) && px_b(top) > px_g(top));
    /* The middle is a mix of all three, which an unnormalised read would have saturated to
     * white. */
    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2 + 6);
    ASSERT_TRUE(px_r(mid) > 20 && px_r(mid) < 200);
    ASSERT_TRUE(px_g(mid) > 20 && px_g(mid) < 200);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* **The same two features on the path that defines the answer.**
 *
 * `glsl_gen.c` compiles file-scope `const`s and `discard` for the console, and the console is
 * meant to agree with this rasteriser rather than the other way round - so a construct the
 * compiler accepts and the reference cannot run is the one divergence that would never show up
 * as a wrong pixel anywhere a test could look. Until 2026-09-21 the interpreter declared
 * attributes, varyings and uniforms and nothing else, so a shader opening with
 * `const float pi = 3.14159;` - which is how most real ones open - failed here while compiling
 * there.
 */
static void test_gl2_the_reference_runs_globals_and_discard(void) {
    gl2_target_t t = gl2_target();
    static const GLfloat verts[9] = {
        -0.9f, -0.9f, 0.0f,
         0.9f, -0.9f, 0.0f,
         0.0f,  0.9f, 0.0f};

    /* Three globals, the third written in terms of the first two - so they have to be declared
     * in source order and not merely all declared. */
    const GLuint prog = linked_program(
        "attribute vec3 pos;\n"
        "void main() { gl_Position = vec4(pos, 1.0); }\n",
        "const float half_on = 0.5;\n"
        "const vec3 warm = vec3(1.0, 0.5, 0.0);\n"
        "const vec3 dim = warm * half_on;\n"
        "void main() { gl_FragColor = vec4(dim, 1.0); }\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    ASSERT_TRUE(loc >= 0);
    glUseProgram(prog);
    glVertexAttribPointer((GLuint)loc, 3, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray((GLuint)loc);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* `dim` is `warm * half_on`, so (0.5, 0.25, 0.0) - and the third global being right is the
     * whole point: a `const` declared but not initialised would give black here, and one
     * declared out of order would give zero for `warm` and black again. */
    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2 + 6);
    ASSERT_TRUE(px_r(mid) > 115 && px_r(mid) < 140);
    ASSERT_TRUE(px_g(mid) > 54 && px_g(mid) < 76);
    ASSERT_TRUE(px_b(mid) < 12);

    /* And `discard`, where the check is that the background survives - a fragment that was
     * thrown away must leave what was under it. */
    const GLuint killer = linked_program(
        "attribute vec3 pos;\n"
        "void main() { gl_Position = vec4(pos, 1.0); }\n",
        "const float always = 1.0;\n"
        "void main() {\n"
        "  if (always > 0.5) { discard; }\n"
        "  gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
        "}\n");
    glUseProgram(killer);
    glVertexAttribPointer((GLuint)glGetAttribLocation(killer, "pos"), 3, GL_FLOAT, GL_FALSE, 0,
                          verts);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    /* Still the previous shader's colour, not white. */
    const uint32_t after = px(&t, GL2_W / 2, GL2_H / 2 + 6);
    ASSERT_EQ(after, mid);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_frag_coord_and_derivatives(void) {
    gl2_target_t t = gl2_target();

    /* `gl_FragCoord.y` counts **up from the bottom**, which is the opposite of the
     * rasteriser's rows - a shader that gets the row index instead draws this gradient upside
     * down, and that is exactly what this measures. */
    const GLuint prog = linked_program(
        "attribute vec3 pos;\n"
        "void main() { gl_Position = vec4(pos, 1.0); }\n",
        "void main() { gl_FragColor = vec4(gl_FragCoord.y / 64.0, 0.0, 0.0, 1.0); }\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    glUseProgram(prog);
    draw_quad(loc, 0.0f);

    const uint32_t high_row = px(&t, GL2_W / 2, 10);            /* near the top of the image */
    const uint32_t low_row = px(&t, GL2_W / 2, GL2_H - 11);     /* near the bottom */
    ASSERT_TRUE(px_r(high_row) > px_r(low_row));

    /* A derivative across the screen. `dFdx` of a varying that runs 0..1 over the quad's width
     * is its slope per pixel - about 1/51 here - so scaling by 64 gives something visible and
     * constant, which a derivative taken as zero would not. */
    glClear(GL_COLOR_BUFFER_BIT);
    const GLuint dprog = linked_program(
        "attribute vec3 pos;\n"
        "varying float v;\n"
        "void main() { v = pos.x * 0.625 + 0.5; gl_Position = vec4(pos, 1.0); }\n",
        "varying float v;\n"
        "void main() { gl_FragColor = vec4(dFdx(v) * 64.0, 0.0, 0.0, 1.0); }\n");
    const GLint dloc = glGetAttribLocation(dprog, "pos");
    glUseProgram(dprog);
    draw_quad(dloc, 0.0f);

    const uint32_t a = px(&t, GL2_W / 2 - 8, GL2_H / 2);
    const uint32_t b = px(&t, GL2_W / 2 + 8, GL2_H / 2);
    ASSERT_TRUE(px_r(a) > 0);
    ASSERT_EQ(px_r(a), px_r(b));   /* constant across a linear varying */
    /* 0.625 * 2 over 51.2 pixels, times 64, is about 1.56 - clamped to 1.0 in the framebuffer.
     * What matters is that it is not zero, which is what no derivative at all would give. */
    ASSERT_TRUE(px_r(a) > 200);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_a_vertex_shader_alone_feeds_fixed_function(void) {
    gl2_target_t t = gl2_target();

    /* **A program may have one stage.** With only a vertex shader the fixed-function fragment
     * stage runs, reading `gl_FrontColor` and `gl_TexCoord[]` - which is what a port that
     * replaces its transform and keeps its texture combiner does. */
    const GLuint prog = linked_program(
        "attribute vec3 pos;\n"
        "void main() {\n"
        "  gl_FrontColor = vec4(1.0, 0.0, 1.0, 1.0);\n"
        "  gl_Position = vec4(pos, 1.0);\n"
        "}\n",
        NULL);
    const GLint loc = glGetAttribLocation(prog, "pos");
    glUseProgram(prog);
    draw_quad(loc, 0.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(mid) > 247 && px_g(mid) < 8 && px_b(mid) > 247);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_glsl_120_runs_what_it_compiles(void) {
    gl2_target_t t = gl2_target();

    /* Compiling is half of it. **An integer that widened has to arrive as a float at run time
     * too**: an interpreter that kept the left operand's type would make `2 * 0.25` an integer
     * expression and truncate the answer to 0, which is a black channel where a half-lit one
     * was meant. */
    const GLuint prog = linked_program(
        "#version 120\n"
        "attribute vec3 pos;\n"
        "void main() { gl_Position = vec4(pos, 1.0); }\n",
        "#version 120\n"
        "void main() {\n"
        "  float half_v = 2 * 0.25;\n"             /* 0.5, not 0 */
        "  float quarter = 1 / 4.0;\n"             /* 0.25, not 0 */
        "  float three_q = clamp(3, 0, 1) * 0.75;\n"
        "  gl_FragColor = vec4(half_v, quarter, three_q, 1.0);\n"
        "}\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    glUseProgram(prog);
    draw_quad(loc, 0.0f);

    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(mid) > 120 && px_r(mid) < 135);
    ASSERT_TRUE(px_g(mid) > 58 && px_g(mid) < 70);
    ASSERT_TRUE(px_b(mid) > 185 && px_b(mid) < 198);

    /* `transpose` and `outerProduct` computing what they say. `outerProduct(c, r)` puts `c`
     * down the columns, so element (col 0, row 1) is `c.y * r.x` - and the transpose swaps it
     * with (col 1, row 0). Getting the two the wrong way round gives a matrix that is still a
     * matrix, and still draws. */
    glClear(GL_COLOR_BUFFER_BIT);
    const GLuint mprog = linked_program(
        "#version 120\n"
        "attribute vec3 pos;\n"
        "void main() { gl_Position = vec4(pos, 1.0); }\n",
        "#version 120\n"
        "void main() {\n"
        "  mat2 o = outerProduct(vec2(1.0, 0.5), vec2(0.25, 1.0));\n"
        "  mat2 tr = transpose(o);\n"
        "  gl_FragColor = vec4(o[0][1], tr[0][1], o[1][0], 1.0);\n"
        "}\n");
    glUseProgram(mprog);
    draw_quad(glGetAttribLocation(mprog, "pos"), 0.0f);

    /* o[0][1]  = c.y * r.x = 0.5 * 0.25 = 0.125 -> 32
     * tr[0][1] = o[1][0]   = c.x * r.y  = 1.0   -> 255
     * o[1][0]  = 1.0                            -> 255 */
    const uint32_t m = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(m) > 26 && px_r(m) < 38);
    ASSERT_TRUE(px_g(m) > 247);
    ASSERT_TRUE(px_b(m) > 247);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_a_runaway_shader_is_stopped(void) {
    gl2_target_t t = gl2_target();

    /* A shader is a program somebody else wrote, and `while (true) {}` compiles. The invocation
     * carries a step budget; running out ends the draw with GL_INVALID_OPERATION rather than
     * taking the frame - and nothing is drawn, because a half-run shader has no colour. */
    const GLuint prog = linked_program(
        "attribute vec3 pos;\n"
        "void main() {\n"
        "  float k = 0.0;\n"
        "  for (int i = 0; i < 1000000; i++) { k += 0.000001; }\n"
        "  gl_Position = vec4(pos, 1.0) * k;\n"
        "}\n",
        "void main() { gl_FragColor = vec4(1.0); }\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    glUseProgram(prog);
    draw_quad(loc, 0.0f);

    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    ASSERT_EQ(px(&t, GL2_W / 2, GL2_H / 2), 0xff000000u);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* -------------------------------------------------------------------------
 * The rest of GL 2.0, which is not about shaders
 * ------------------------------------------------------------------------- */

static void test_gl2_separate_stencil_and_blend_state(void) {
    void *ctx = gl2_context();

    /* **`glStencilFunc` sets both faces**, which is how the specification defines it from 2.0
     * onwards - so a GL 1.x program is unaffected by the split existing. */
    glStencilFunc(GL_EQUAL, 3, 0x0fu);
    GLint v = 0;
    glGetIntegerv(GL_STENCIL_FUNC, &v);
    ASSERT_EQ(v, (GLint)GL_EQUAL);
    glGetIntegerv(GL_STENCIL_BACK_FUNC, &v);
    ASSERT_EQ(v, (GLint)GL_EQUAL);
    glGetIntegerv(GL_STENCIL_BACK_REF, &v);
    ASSERT_EQ(v, 3);

    /* And the separate form touches only the face it names. */
    glStencilFuncSeparate(GL_BACK, GL_NOTEQUAL, 7, 0xffu);
    glGetIntegerv(GL_STENCIL_FUNC, &v);
    ASSERT_EQ(v, (GLint)GL_EQUAL);
    glGetIntegerv(GL_STENCIL_BACK_FUNC, &v);
    ASSERT_EQ(v, (GLint)GL_NOTEQUAL);
    glGetIntegerv(GL_STENCIL_REF, &v);
    ASSERT_EQ(v, 3);
    glGetIntegerv(GL_STENCIL_BACK_REF, &v);
    ASSERT_EQ(v, 7);

    /* The shadow-volume idiom: increment on one face and decrement on the other. */
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &v);
    ASSERT_EQ(v, (GLint)GL_INCR_WRAP);
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS, &v);
    ASSERT_EQ(v, (GLint)GL_DECR_WRAP);

    glStencilMaskSeparate(GL_BACK, 0x0fu);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &v);
    ASSERT_EQ(v, (GLint)0xffffffffu);
    glGetIntegerv(GL_STENCIL_BACK_WRITEMASK, &v);
    ASSERT_EQ(v, 0x0f);

    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glStencilFuncSeparate(GL_LEFT, GL_ALWAYS, 0, 0xffu);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_TEXTURE_2D);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    /* **A push and pop carries both faces.** GL 2.0 puts the back-face state in the same
     * attribute group, so a push that saved only the front would drop half of it silently. */
    glPushAttrib(GL_STENCIL_BUFFER_BIT);
    glStencilFuncSeparate(GL_BACK, GL_LESS, 1, 0xffu);
    glPopAttrib();
    glGetIntegerv(GL_STENCIL_BACK_FUNC, &v);
    ASSERT_EQ(v, (GLint)GL_NOTEQUAL);

    /* A blend equation per channel group, and `glBlendEquation` setting both. */
    glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_MAX);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &v);
    ASSERT_EQ(v, (GLint)GL_FUNC_SUBTRACT);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &v);
    ASSERT_EQ(v, (GLint)GL_MAX);
    glBlendEquation(GL_FUNC_ADD);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &v);
    ASSERT_EQ(v, (GLint)GL_FUNC_ADD);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_TEXTURE_2D);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    glContextDestroy(ctx);
}

static void test_gl2_separate_blend_equation_blends(void) {
    gl2_target_t t = gl2_target();

    /* The colour subtracts and the alpha adds, in one blend. An implementation carrying one
     * equation for both would either subtract the alpha too or add the colour. */
    glClearColor(0.5f, 0.5f, 0.5f, 0.25f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);

    glColor4f(0.25f, 0.25f, 0.25f, 0.5f);
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.9f, -0.9f, 0.0f);
    glVertex3f(0.9f, -0.9f, 0.0f);
    glVertex3f(0.9f, 0.9f, 0.0f);
    glEnd();

    /* Colour: destination minus source, 0.5 - 0.25 = 0.25, about 64.
     * Alpha:   destination plus source, 0.25 + 0.5 = 0.75, about 192.
     *
     * Not exact numbers: the destination came back out of eight-bit storage, so 0.25 is really
     * 64/255. What separates a pass from a failure here is the *sign* - a shared equation gives
     * either 0.75 in the colour or 0.25 in the alpha, both a long way from these. */
    const uint32_t p = px(&t, GL2_W / 2 + 8, GL2_H / 2 + 8);
    const int alpha = (int)((p >> 24) & 0xffu);
    ASSERT_TRUE(px_r(p) > 58 && px_r(p) < 70);
    ASSERT_TRUE(alpha > 185 && alpha < 198);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

static void test_gl2_draw_buffers(void) {
    void *ctx = gl2_context();

    GLint v = 0;
    const GLenum both[2] = {GL_FRONT, GL_BACK};
    glDrawBuffers(2, both);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glGetIntegerv(GL_DRAW_BUFFER, &v);
    ASSERT_EQ(v, (GLint)GL_FRONT_AND_BACK);

    const GLenum one[1] = {GL_BACK};
    glDrawBuffers(1, one);
    glGetIntegerv(GL_DRAW_BUFFER, &v);
    ASSERT_EQ(v, (GLint)GL_BACK);

    /* None at all is no colour buffer, which is legal and is what a depth-only pass asks for. */
    glDrawBuffers(0, NULL);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glGetIntegerv(GL_DRAW_BUFFER, &v);
    ASSERT_EQ(v, (GLint)GL_NONE);

    /* **A name covering more than one buffer may not appear in the list** (GL 2.0, 4.2.1), and
     * neither may a buffer named twice - both GL_INVALID_OPERATION, not a silent union. */
    const GLenum wide[1] = {GL_FRONT_AND_BACK};
    glDrawBuffers(1, wide);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    const GLenum twice[2] = {GL_BACK, GL_BACK};
    glDrawBuffers(2, twice);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    const GLenum absent[1] = {GL_FRONT_RIGHT};
    glDrawBuffers(1, absent);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

    GLint limit = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &limit);
    glDrawBuffers(limit + 1, both);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

    glContextDestroy(ctx);
}

static void test_gl2_version_gating(void) {
    /* **A context that has not claimed 2.0 does not have 2.0.** Deliberately not through
     * `gl2_context`, which claims it - this one is the default, 1.1. */
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
    void *ctx = glContextCreate(disp);
    glContextMakeCurrent(ctx);

    GLuint maj = 0u, min = 9u;
    glContextGetVersion(&maj, &min);
    ASSERT_EQ(maj, 1u);
    ASSERT_EQ(min, 1u);

    /* A name-returning call answers 0, a location -1, a predicate GL_FALSE - each the value it
     * returns on failure - and every one of them records GL_INVALID_OPERATION. */
    ASSERT_EQ(glCreateShader(GL_VERTEX_SHADER), 0u);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    ASSERT_EQ(glCreateProgram(), 0u);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    ASSERT_EQ(glIsShader(1u), GL_FALSE);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    ASSERT_EQ(glGetUniformLocation(1u, "x"), -1);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glUseProgram(0);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glEnableVertexAttribArray(0);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_KEEP);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    const GLenum bufs[1] = {GL_BACK};
    glDrawBuffers(1, bufs);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

    /* **And the call did nothing**, which is the half an error code does not prove: the back
     * stencil state is untouched after a refused `glStencilOpSeparate`. */
    glContextSetVersion(2, 0);
    GLint v = 0;
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS, &v);
    ASSERT_EQ(v, (GLint)GL_KEEP);
    glContextSetVersion(1, 1);

    /* **An enumerant a later version added is GL_INVALID_ENUM**, not GL_INVALID_OPERATION -
     * "I have never heard of this" rather than "not from here". */
    v = 1234;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &v);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    ASSERT_EQ(v, 1234);
    glGetIntegerv(GL_CURRENT_PROGRAM, &v);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    ASSERT_TRUE(glGetString(GL_SHADING_LANGUAGE_VERSION) == NULL);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    /* **GL 1.x is untouched**, and so are the extension spellings. Every 1.2-1.5 feature here
     * is advertised as an ARB or EXT extension, and an extension is available to a context
     * whatever its core version - so a GL 1.1 context genuinely has buffer objects, and
     * refusing them would be a rule about spelling rather than about capability. */
    GLuint buf = 0u;
    glGenBuffers(1, &buf);
    ASSERT_TRUE(buf != 0u);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glActiveTexture(GL_TEXTURE1);
    glSecondaryColor3f(1.0f, 0.0f, 0.0f);
    glFogCoordf(1.0f);
    glBlendEquation(GL_FUNC_SUBTRACT);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    /* The GL 1.0 stencil call set both faces even here, which is what it means from 2.0
     * onwards - the state exists whatever version is claimed; only the entry point that
     * addresses one face is 2.0's. */
    glContextSetVersion(2, 0);
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS, &v);
    ASSERT_EQ(v, (GLint)GL_INCR);

    /* Claiming it turns the whole surface on. */
    const GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
    ASSERT_TRUE(sh != 0u);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* And narrowing again turns it back off, which is what makes this a property of the
     * context rather than a one-way switch. */
    glContextSetVersion(1, 5);
    ASSERT_EQ(glCreateShader(GL_FRAGMENT_SHADER), 0u);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

/* -------------------------------------------------------------------------
 * The console back end
 *
 * **These assert instruction words, and every one of them came out of clang.**
 * `tools/shader/gl2-fragment.s` holds the source; the words below are what
 * `clang -target amdgcn-amd-amdhsa -mcpu=gfx1030` assembled it to. A wrong encoding in a
 * hand-written shader is wrong once; a wrong encoding in a compiler is wrong in every shader it
 * ever emits, which is why this is checked against an assembler rather than against itself.
 *
 * Nothing here runs on a console. What it establishes is that the words are the right words -
 * which is the half that can be established without one.
 * ------------------------------------------------------------------------- */

static void test_gl2_pixel_shader_encodings_match_the_assembler(void) {
    uint32_t words[64];
    glsl_code_t c;

    /* **The parameter cache address**, which every interpolation below reads out of `m0`. Both
     * source registers, because the back end emits whichever the draw's user-SGPR count puts
     * the primitive mask in - and these are the same two words the payload's hand-written pixel
     * shaders carry (`gl_context.c`, `ps_untex[1]` and `ps_tex[1]`), which is the cross-check
     * that the generated prologue and the fixed-function one mean the same thing. */
    glsl_code_init(&c, words, 64);
    glsl_emit_s_mov_m0(&c, 0u);
    glsl_emit_s_mov_m0(&c, 2u);
    ASSERT_EQ(c.count, 2u);
    ASSERT_EQ(words[0], 0xbefc0300u); /* s_mov_b32 m0, s0 */
    ASSERT_EQ(words[1], 0xbefc0302u); /* s_mov_b32 m0, s2 */

    /* **DPP, the eight-byte form a derivative reads its neighbour through.** `src0` is 0xfa in
     * the first word - the marker - and the real source register, the permute and the masks are
     * in the second. A reader that stopped at the first word would take the next instruction's
     * words for operands. */
    glsl_code_init(&c, words, 64);
    glsl_emit_dpp_mov(&c, 4u, 5u, GLSL_DPP_QUAD_X_NEAR);
    glsl_emit_dpp_sub(&c, 4u, 5u, 5u, GLSL_DPP_QUAD_X_FAR);
    ASSERT_EQ(c.count, 4u);
    ASSERT_EQ(words[0], 0x7e0802fau); /* v_mov_b32_dpp v4, v5 quad_perm:[0,0,2,2] */
    ASSERT_EQ(words[1], 0xff00a005u); /* [0x05,0xa0,0x00,0xff] - source, permute, then masks */
    ASSERT_EQ(words[2], 0x08080afau); /* v_sub_f32_dpp v4, v5, v5 quad_perm:[1,1,3,3] */
    ASSERT_EQ(words[3], 0xff00f505u);

    /* **The depth export.** Target 8, one channel, and **no `done`** - the colour export that
     * follows is the one that says it, and two exports both claiming to be last is a shader
     * that does not retire. Compare with `exp mrt0 ... done vm` below, which differs in every
     * one of those. */
    glsl_code_init(&c, words, 64);
    glsl_emit_export_mrtz(&c, 4u);
    ASSERT_EQ(c.count, 2u);
    ASSERT_EQ(words[0], 0xf8000081u); /* exp mrtz v4, off, off, off */
    ASSERT_EQ(words[1], 0x00000004u);
    ASSERT_EQ((words[0] >> 11) & 1u, 0u); /* done is not set */

    /* **A scalar operand in `src0`**, which is how `gl_FragCoord.y` gets flipped: the viewport
     * height is a per-draw constant in the scalar file and the hardware's row is a VGPR. VOP2's
     * `src0` is nine bits and names either; `vsrc1` is eight and names a VGPR - so the order is
     * forced, and `glsl_emit_sub_f32` cannot be used because it puts its first operand through
     * `glsl_vgpr` and would encode s44 as v44. */
    glsl_code_init(&c, words, 64);
    glsl_emit_vop2(&c, GLSL_VOP2_SUB_F32, 8u, glsl_sgpr(44u), 3u);
    ASSERT_EQ(c.count, 1u);
    ASSERT_EQ(words[0], 0x0810062cu); /* v_sub_f32_e32 v8, s44, v3 */

    /* Interpolation, across all four channels and a high attribute - so the attribute's field
     * is pinned apart from its channel's, which one example would not have separated. */
    glsl_code_init(&c, words, 64);
    glsl_emit_interp_pair(&c, 4u, 0u, 0u);
    glsl_emit_interp_pair(&c, 5u, 0u, 1u);
    glsl_emit_interp_pair(&c, 6u, 0u, 2u);
    glsl_emit_interp_pair(&c, 7u, 0u, 3u);
    glsl_emit_interp_pair(&c, 20u, 3u, 2u);
    ASSERT_EQ(c.count, 10u);
    ASSERT_EQ(words[0], 0xc8100000u); /* v_interp_p1_f32 v4, v0, attr0.x */
    ASSERT_EQ(words[1], 0xc8110001u); /* v_interp_p2_f32 v4, v1, attr0.x */
    ASSERT_EQ(words[2], 0xc8140100u); /* v_interp_p1_f32 v5, v0, attr0.y */
    ASSERT_EQ(words[3], 0xc8150101u);
    ASSERT_EQ(words[4], 0xc8180200u); /* attr0.z */
    ASSERT_EQ(words[5], 0xc8190201u);
    ASSERT_EQ(words[6], 0xc81c0300u); /* attr0.w */
    ASSERT_EQ(words[7], 0xc81d0301u);
    ASSERT_EQ(words[8], 0xc8500e00u); /* v_interp_p1_f32 v20, v0, attr3.z */
    ASSERT_EQ(words[9], 0xc8510e01u);

    /* The export. Four dwords since 2026-09-23: two `v_cvt_pkrtz_f16_f32` that pack the four
     * floats into two registers, then the export itself - the first of its dwords carrying
     * `compr`, `done` and `vm`, the second the two packed registers as two bytes. An 8_8_8_8
     * target on a part with RB+ requires the half-float format, and the four-float export this
     * used to emit is what broke blending; glsl_emit_export_mrt0 carries the account. */
    glsl_code_init(&c, words, 64);
    glsl_emit_export_mrt0(&c, 4u);
    ASSERT_EQ(c.count, 4u);
    ASSERT_EQ(words[0], 0x5e080b04u); /* v_cvt_pkrtz_f16_f32 v4, v4, v5 */
    ASSERT_EQ(words[1], 0x5e0a0f06u); /* v_cvt_pkrtz_f16_f32 v5, v6, v7 */
    ASSERT_EQ(words[2], 0xf8001c0fu); /* exp mrt0 ... done compr vm */
    ASSERT_EQ(words[3], 0x00000504u); /* v4, v5 */

    /* **From v0, this is LLVM's own output, word for word.** `llc -mcpu=gfx1030` on the pair of
     * `llvm.amdgcn.cvt.pkrtz` feeding `llvm.amdgcn.exp.compr.v2f16` assembles to
     * `[0x00,0x03,0x00,0x5e]`, `[0x02,0x07,0x02,0x5e]` and
     * `[0x0f,0x1c,0x00,0xf8],[0x00,0x01,0x00,0x00]`. The encodings here were taken from it
     * rather than derived, and this is the case that says so. */
    glsl_code_init(&c, words, 64);
    glsl_emit_export_mrt0(&c, 0u);
    ASSERT_EQ(words[0], 0x5e000300u); /* v_cvt_pkrtz_f16_f32 v0, v0, v1 */
    ASSERT_EQ(words[1], 0x5e020702u); /* v_cvt_pkrtz_f16_f32 v1, v2, v3 */
    ASSERT_EQ(words[2], 0xf8001c0fu);
    ASSERT_EQ(words[3], 0x00000100u); /* v0, v1 */

    /* The one-operand instructions. */
    glsl_code_init(&c, words, 64);
    glsl_emit_vop1_op(&c, GLSL_VOP1_RCP_F32, 4u, 5u);
    glsl_emit_vop1_op(&c, GLSL_VOP1_SQRT_F32, 4u, 5u);
    glsl_emit_vop1_op(&c, GLSL_VOP1_RSQ_F32, 4u, 5u);
    glsl_emit_vop1_op(&c, GLSL_VOP1_FRACT_F32, 4u, 5u);
    glsl_emit_vop1_op(&c, GLSL_VOP1_FLOOR_F32, 4u, 5u);
    glsl_emit_vop1_op(&c, GLSL_VOP1_CEIL_F32, 4u, 5u);
    glsl_emit_vop1_op(&c, GLSL_VOP1_TRUNC_F32, 4u, 5u);
    glsl_emit_vop1_op(&c, GLSL_VOP1_SIN_F32, 4u, 5u);
    glsl_emit_vop1_op(&c, GLSL_VOP1_COS_F32, 4u, 5u);
    glsl_emit_vop1_op(&c, GLSL_VOP1_EXP_F32, 4u, 5u);
    glsl_emit_vop1_op(&c, GLSL_VOP1_LOG_F32, 4u, 5u);
    ASSERT_EQ(words[0], 0x7e085505u);  /* v_rcp_f32_e32 v4, v5 */
    ASSERT_EQ(words[1], 0x7e086705u);  /* v_sqrt_f32_e32 */
    ASSERT_EQ(words[2], 0x7e085d05u);  /* v_rsq_f32_e32 */
    ASSERT_EQ(words[3], 0x7e084105u);  /* v_fract_f32_e32 */
    ASSERT_EQ(words[4], 0x7e084905u);  /* v_floor_f32_e32 */
    ASSERT_EQ(words[5], 0x7e084505u);  /* v_ceil_f32_e32 */
    ASSERT_EQ(words[6], 0x7e084305u);  /* v_trunc_f32_e32 */
    ASSERT_EQ(words[7], 0x7e086b05u);  /* v_sin_f32_e32 */
    ASSERT_EQ(words[8], 0x7e086d05u);  /* v_cos_f32_e32 */
    ASSERT_EQ(words[9], 0x7e084b05u);  /* v_exp_f32_e32 - base two */
    ASSERT_EQ(words[10], 0x7e084f05u); /* v_log_f32_e32 - base two */

    /* Two operands, and the conditional move whose false value comes first. */
    glsl_code_init(&c, words, 64);
    glsl_emit_vop2_op(&c, GLSL_VOP2_MAX_F32, 4u, 5u, 6u);
    glsl_emit_vop2_op(&c, GLSL_VOP2_MIN_F32, 4u, 5u, 6u);
    glsl_emit_cndmask(&c, 4u, 5u, 6u);
    ASSERT_EQ(words[0], 0x20080d05u); /* v_max_f32_e32 v4, v5, v6 */
    ASSERT_EQ(words[1], 0x1e080d05u); /* v_min_f32_e32 v4, v5, v6 */
    ASSERT_EQ(words[2], 0x02080d05u); /* v_cndmask_b32_e32 v4, v5, v6, vcc_lo */

    /* The comparisons and the lane kill `discard` ends with. */
    glsl_code_init(&c, words, 64);
    glsl_emit_cmp(&c, GLSL_VOPC_LT_F32, 4u, 5u);
    glsl_emit_cmp(&c, GLSL_VOPC_EQ_F32, 4u, 5u);
    glsl_emit_cmp(&c, GLSL_VOPC_LE_F32, 4u, 5u);
    glsl_emit_cmp(&c, GLSL_VOPC_GT_F32, 4u, 5u);
    glsl_emit_cmp(&c, GLSL_VOPC_GE_F32, 4u, 5u);
    glsl_emit_cmp(&c, GLSL_VOPC_NEQ_F32, 4u, 5u);
    glsl_emit_kill_from_vcc(&c);
    ASSERT_EQ(words[0], 0x7c020b04u); /* v_cmp_lt_f32_e32 vcc_lo, v4, v5 */
    ASSERT_EQ(words[1], 0x7c040b04u); /* eq */
    ASSERT_EQ(words[2], 0x7c060b04u); /* le */
    ASSERT_EQ(words[3], 0x7c080b04u); /* gt */
    ASSERT_EQ(words[4], 0x7c0c0b04u); /* ge */
    ASSERT_EQ(words[5], 0x7c1a0b04u); /* neq - the unordered one */
    /* **The cross-check that this pipeline is right rather than self-consistent**: this exact
     * word is already in the tree behind glAlphaFunc and the polygon stipple. */
    ASSERT_EQ(words[6], 0x877e6a7eu); /* s_and_b32 exec_lo, exec_lo, vcc_lo */

    glsl_code_init(&c, words, 64);
    glsl_emit_nop(&c);
    glsl_emit_endpgm(&c);
    ASSERT_EQ(words[0], 0xbf800000u); /* s_nop 0 */
    ASSERT_EQ(words[1], 0xbf810000u); /* s_endpgm */
}

static void test_gl2_compiles_a_whole_pixel_shader(void) {
    void *ctx = gl2_context();

    /* **gl2-cube's own fragment shader**, which is the one a console oracle would be recorded
     * with. Three varying components interpolated, a constructor, and the colour exported. */
    const GLuint prog = linked_program(
        "uniform mat4 mvp;\n"
        "attribute vec3 pos;\n"
        "attribute vec3 colour;\n"
        "varying vec3 vcolour;\n"
        "void main() { vcolour = colour; gl_Position = mvp * vec4(pos, 1.0); }\n",
        "varying vec3 vcolour;\n"
        "void main() { gl_FragColor = vec4(vcolour, 1.0); }\n");

    gl_context_t *c = (gl_context_t *)ctx;
    const gl_program_object_t *p = gl_find_program(c, prog);
    ASSERT_TRUE(p != NULL);

    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};
    uint32_t user_sgprs = 99u;
    uint32_t input_ena = 0u;
    const GLboolean ok = gl_program_compile_fragment(p, words, 256u, &count, &vgprs,
                                                     &user_sgprs, &input_ena, log, sizeof(log));
    if (!ok) printf("\n    compile failed: %s\n", log);
    ASSERT_EQ(ok, GL_TRUE);

    /* **`m0` before anything else**, because every interpolation below reads the parameter
     * cache through it and a shader without it interpolates whatever the previous wave was
     * pointed at. s2 because this program is handed the block in s[0:1], which puts the SPI's
     * primitive mask in the register after them. */
    ASSERT_EQ(words[0], 0xbefc0302u); /* s_mov_b32 m0, s2 */

    /* **Then the uniforms**, because they are a memory load and the wait for it wants as much
     * between it and the first read as possible. This program's pool is the vertex shader's
     * `mat4 mvp` - sixteen floats, one `s_load_dwordx16` into s48..s63 - and the fragment
     * shader names none of them, so nothing is moved into a VGPR for it. */
    ASSERT_EQ(words[1], 0xf4100c00u); /* s_load_dwordx16 s[48:63], s[0:1], 0x80 */
    /* **0x80, not 0.** The block's first two 0x40 are the texture units' descriptors; the
     * uniforms start after them, and a shader loading from 0 would compute with an image
     * descriptor read as floats. */
    ASSERT_EQ(words[2], 0xfa000080u);
    ASSERT_EQ(words[3], 0xbf8cc07fu); /* s_waitcnt lgkmcnt(0) */

    /* Then three components of one varying, each a `p1`/`p2` pair, into v8, v9, v10 - the first
     * registers above the ones the hardware owns. */
    ASSERT_EQ(words[4], 0xc8200000u); /* v_interp_p1_f32 v8, v0, attr0.x */
    ASSERT_EQ(words[5], 0xc8210001u); /* v_interp_p2_f32 v8, v1, attr0.x */
    ASSERT_EQ(words[6], 0xc8240100u); /* v9, attr0.y */
    ASSERT_EQ(words[7], 0xc8250101u);
    ASSERT_EQ(words[8], 0xc8280200u); /* v10, attr0.z */
    ASSERT_EQ(words[9], 0xc8290201u);

    /* **The block's address and the mask register are one decision.** Two user SGPRs is what
     * the draw configures into `SPI_SHADER_PGM_RSRC2_PS`, and it is also what puts the mask in
     * s2 - so a shader that reported a different count would be moving the wrong register into
     * `m0` on the very draw that configured it. */
    ASSERT_EQ(user_sgprs, 2u);

    /* **And the pixel stage is asked for exactly what this shader reads**: the perspective
     * centre barycentrics and nothing else, because nothing here names `gl_FragCoord`. Asking
     * for the window position as well would cost four VGPRs of the stage's allocation on every
     * shader that never looks at it. */
    ASSERT_EQ(input_ena, 0x00000002u);

    /* The epilogue, whatever the body did in between: the colour into v4..v7, the export,
     * `s_endpgm`, and two `s_nop`s of room past it.
     *
     * **The room is what lets a second colour target have an export.** The tail is exactly
     * `GL_PS_EXPORT_WORDS` long and byte-identical to `gl_ps_export_words(GL_FALSE)`, so the
     * draw path can write the two-target form over it in place when `fb_also` is bound. Nothing
     * runs after `s_endpgm`, so the padding costs a one-target draw nothing. */
    ASSERT_EQ(words[count - 1u], 0xbf800000u); /* s_nop 0, past the end */
    ASSERT_EQ(words[count - 2u], 0xbf800000u); /* s_nop 0, past the end */
    ASSERT_EQ(words[count - 3u], 0xbf810000u); /* s_endpgm */
    ASSERT_EQ(words[count - 4u], 0x00000504u); /* v4, v5 - the two packed registers */
    ASSERT_EQ(words[count - 5u], 0xf8001c0fu); /* exp mrt0 ... done compr vm */
    ASSERT_EQ(words[count - 6u], 0x5e0a0f06u); /* v_cvt_pkrtz_f16_f32 v5, v6, v7 */
    ASSERT_EQ(words[count - 7u], 0x5e080b04u); /* v_cvt_pkrtz_f16_f32 v4, v4, v5 */
    /* And the whole tail is the shared one, so the swap is a copy and not a translation. */
    for (uint32_t i = 0u; i < GL_PS_EXPORT_WORDS; i++) {
        ASSERT_EQ(words[count - GL_PS_EXPORT_WORDS + i], gl_ps_export_words(GL_FALSE)[i]);
    }
    for (uint32_t i = 0; i < 4u; i++) {
        /* v_mov_b32 v4+i, <colour>+i - the opcode and destination are what matter here. They sit
           just above the export tail, which is `GL_PS_EXPORT_WORDS` long however many targets
           the draw ends up having. */
        const uint32_t w = words[count - GL_PS_EXPORT_WORDS - 4u + i];
        ASSERT_EQ(w >> 25, 0x3fu);                   /* VOP1 */
        ASSERT_EQ((w >> 17) & 0xffu, 4u + i);        /* into v4..v7 */
        ASSERT_EQ((w >> 9) & 0xffu, 1u);             /* v_mov_b32 */
    }

    /* **The register count is what the resource register has to reserve.** Everything below v8
     * is the hardware's, so a count that did not include them would under-reserve the file. */
    ASSERT_TRUE(vgprs >= 12u);
    ASSERT_TRUE(count > 8u && count < 64u);

    /* A program with no fragment stage compiles to nothing and is not a failure: the
     * fixed-function pixel shader in the payload is what runs for it. */
    const GLuint vs_only = linked_program(
        "attribute vec3 pos;\n"
        "void main() { gl_FrontColor = vec4(1.0); gl_Position = vec4(pos, 1.0); }\n",
        NULL);
    const gl_program_object_t *p2 = gl_find_program(c, vs_only);
    count = 99u;
    user_sgprs = 99u;
    ASSERT_EQ(gl_program_compile_fragment(p2, words, 256u, &count, &vgprs, &user_sgprs, NULL,
                                          log, sizeof(log)),
              GL_TRUE);
    ASSERT_EQ(count, 0u);
    /* Nothing was compiled, so the draw configures nothing: a program on this arm runs the
     * fixed-function pixel shader, which brings its own user data. */
    ASSERT_EQ(user_sgprs, 0u);

    glContextDestroy(ctx);
}

static void test_gl2_the_back_end_refuses_what_it_cannot_encode(void) {
    void *ctx = gl2_context();
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};

    /* **What is left refused is the one-dimensional family.** `texture2D`, `texture2DProj`,
     * `textureCube`, `texture3D`, `texture3DProj`, `shadow2D` and `shadow2DProj` are all
     * generated: each reads a descriptor obSCEne has measured on this part, and the shadow pair
     * needed one instruction - `image_sample_c` - whose register order was measured too.
     *
     * **There is no longer a refused lookup to list here.** Every form a fragment shader may
     * call is generated, and the two families that used to be named are gone for different
     * reasons: 1D was never a different kind of thing, only unwired - a 1D texture is a 2D
     * image one row high and is *described* to the hardware as TYPE 9, so its lookup is a 2D
     * sample with a zero beside the coordinate - and the explicit-level forms are rejected by
     * the front end, which is right: `texture2DLod` exists only in a vertex shader in GLSL
     * 1.10, and it says so.
     *
     * Also not here: a `samplerCube` sampled through `texture2D`, or any other mismatch. The
     * back end checks it - two address registers where the hardware reads three would leave the
     * third holding whatever the allocator last put there - but the semantic stage types a
     * lookup by its sampler and refuses the mismatch first ("must be its own sampler type"), so
     * no shader can carry one that far and a case here would be testing the front end by
     * proxy. */
    gl_context_t *c = (gl_context_t *)ctx;

    /* **And a third sampler**, which is one more than a draw carries descriptor sets for. The
     * message names the number rather than saying "too many", because the number is the thing
     * to check against. */
    memset(log, 0, sizeof(log));
    const GLuint three = linked_program(
        "attribute vec3 pos;\n"
        "varying vec2 uv;\n"
        "void main() { uv = pos.xy; gl_Position = vec4(pos, 1.0); }\n",
        "uniform sampler2D a;\nuniform sampler2D b;\nuniform sampler2D d;\n"
        "varying vec2 uv;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(a, uv) + texture2D(b, uv) + texture2D(d, uv);\n"
        "}\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, three), words, 256u, &count,
                                          &vgprs, NULL, NULL, log, sizeof(log)),
              GL_FALSE);
    ASSERT_TRUE(strstr(log, "2") != NULL);

    /* **The hardware's own limit**, named with its number: four parameters, sixteen floats.
     * Five has never run on this part, so a program needing a fifth is refused here rather than
     * compiled into a shader that reads a parameter the vertex stage never exported. */
    const GLuint wide = linked_program(
        "attribute vec4 pos;\n"
        "varying vec4 a;\nvarying vec4 b;\nvarying vec4 d;\nvarying vec4 e;\nvarying vec4 f;\n"
        "void main() {\n"
        "  a = pos; b = pos; d = pos; e = pos; f = pos;\n"
        "  gl_Position = pos;\n"
        "}\n",
        "varying vec4 a;\nvarying vec4 b;\nvarying vec4 d;\nvarying vec4 e;\nvarying vec4 f;\n"
        "void main() { gl_FragColor = a + b + d + e + f; }\n");
    memset(log, 0, sizeof(log));
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, wide), words, 256u, &count, &vgprs,
                                          NULL, NULL, log, sizeof(log)),
              GL_FALSE);
    ASSERT_TRUE(strstr(log, "16") != NULL);

    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * A simulator for the compiled pixel shader
 *
 * The tests above assert *words*, which catches a wrong encoding. They cannot catch a wrong
 * **lowering**: `sin` compiled to `v_sin_f32` with no scale is a correctly encoded instruction
 * computing the wrong function, and every word of it looks right. So this decodes the shader the
 * back end just emitted and runs it, and the tests compare the colour it exports against the
 * same arithmetic written in C.
 *
 * **The hardware's semantics, not the language's.** `v_sin_f32` here computes `sin(2*pi*x)`,
 * because that is what the instruction does - so a lowering that forgot the `1/2pi` fails,
 * which is the entire point of simulating rather than asserting words. Likewise `v_exp_f32` is
 * base two and `v_sub_f32` subtracts `vsrc1` from `src0` and not the other way round.
 *
 * One lane, and `v_interp_p1_f32` loads the parameter straight out of `attr` - there are no
 * barycentrics to interpolate with, and the value at *a* fragment is all these tests need.
 * `v_interp_p2_f32` then adds nothing, which is what it does at that fragment.
 * ------------------------------------------------------------------------- */

typedef struct {
    float v[256];
    float s[128];           /* the scalar file as floats, which is where a uniform lands */
    /* **The same file again, as lane masks.** One wave, one lane, so a mask is a boolean. The
     * two arrays do not overlap in practice - `glsl_ps.c` loads uniforms from s16 up and
     * `glsl_gen.c` saves exec masks into s4..s15 - and keeping them apart here means a shader
     * that confused the two would read a zero rather than a plausible float. */
    GLboolean smask[128];
    /* **And a third view of the same file, as unsigned integers.** A branched loop keeps a trip
     * counter in a scalar register and compares it with `s_cmp_ge_u32`; that register is never
     * also a mask, and no mask register is ever also a counter, so the views never disagree
     * about one register - they are separate because a counter of 0 and a mask of "no lanes"
     * are the same bit pattern and reading one as the other would look like it worked. */
    uint32_t scount[128];
    /* The scalar condition code, which is what `s_cbranch_scc1` reads. */
    GLboolean scc;
    /* **The whole block `s[0:1]` points at**, laid out the way `gl_gl2_build_block` lays it
     * out: two texture units' descriptors, then the uniforms at 0x80. Built here rather than
     * pointed at `p->values` directly, so that a shader loading its uniforms from the wrong
     * offset reads a descriptor rather than the right answer. */
    float ublock[OOPS_GL_GL2_SLOT_STRIDE / 4];
    int ublock_floats;
    /* **Set by a sample and cleared by `s_waitcnt vmcnt(0)`.** Reading one of these before the
     * wait is a shader computing with what the register held, which is the hazard obSCEne
     * measured for the scalar loads (`-6c0d`, arm 5) and the same one applies here. */
    GLboolean vpending[256];
    int samples;            /* how many `image_sample`s ran */
    uint32_t last_tex_set;  /* which descriptor set the last one used */
    GLboolean vcc;
    GLboolean exec;
    float out[4];
    GLboolean exported;
    GLboolean lane_survived; /* exec at the export: whether this fragment is written */
    GLboolean ended;
    /* **Set by a scalar load and cleared by its wait.** A read of an SGPR while this is set is
     * a shader reading a register the load has not delivered into - which on hardware is
     * whatever it held, and here is a test failure. */
    GLboolean lgkm_pending;
    /* **Whether `m0` has been pointed at the parameter cache.** The hardware reads it on every
     * `v_interp`, and a shader that never sets it interpolates against whatever the previous
     * wave left - which is not a blank screen or a fault but a surface speckled, wave by wave,
     * with another primitive's parameters. That was gl2-cube's first frame on hardware
     * (2026-09-22). The interpolation arm below refuses to run without it, so removing the
     * prologue's `s_mov_b32 m0` fails here instead of on a console. */
    GLboolean m0_set;
    /* **Which of the hardware's own registers this shader actually asked for**, from the
     * `SPI_PS_INPUT_ENA` the compiler reported. Reading one that is not live is reading what
     * the previous wave left. */
    GLboolean hw_vgpr_live[8];
    /* **What a `v_cvt_pkrtz_f16_f32` put in a register**, which a float cannot hold: the
       instruction packs two half-floats into one 32-bit register and the colour export reads
       them back as a pair. `s->v` models a register as one float, so the pair lives beside it
       and the compressed export reads this instead. Written by opcode 47 and by nothing else,
       so a register that was never packed and is exported compressed reads as zero rather than
       as whatever its float happened to be. */
    float vpack[256][2];
    GLboolean vpacked[256];
} sim_t;

/* **A float as `v_cvt_pkrtz_f16_f32` leaves it**: half precision, round toward zero.
 *
 * The colour a shader computes in 32 bits does not survive to the colour block intact - an
 * 8_8_8_8 target on this part takes half-floats, so ten mantissa bits is what a fragment gets.
 * That is more than an 8-bit channel needs and the loss is invisible in a rendered frame, but a
 * simulator that carried full precision through a half-precision instruction would be claiming
 * an exactness the hardware does not have, and the next thing that depends on the low bits
 * would find out on a console instead of here.
 *
 * Half's subnormal range ends below 6.1e-5, which is a quarter of one 8-bit level, so anything
 * that small is flushed to zero rather than modelled. */
static float sim_f16_rtz(float f) {
    union { float f; uint32_t u; } c;
    c.f = f;
    const uint32_t sign = c.u & 0x80000000u;
    const uint32_t biased = (c.u >> 23) & 0xffu;
    if (biased == 0xffu) return f; /* inf and nan pass through */
    {
        const int32_t e = (int32_t)biased - 127;
        if (e > 15) { c.u = sign | 0x7f800000u; return c.f; } /* beyond half's range */
        if (e < -14) { c.u = sign; return c.f; }              /* below its normals */
        c.u = (c.u & ~0x1fffu); /* ten mantissa bits, the low thirteen dropped - toward zero */
        c.u |= sign;
        return c.f;
    }
}

/* The first register the allocator owns; everything below it is the SPI's. Mirrors
 * `GL_PS_FIRST_FREE_VGPR` in `glsl_ps.c`, which is not a header constant. */
#define GL_PS_FIRST_FREE_VGPR_SIM 8u

static float sim_f32(uint32_t bits) {
    union { uint32_t u; float f; } cvt;
    cvt.u = bits;
    return cvt.f;
}

/* A source operand: a VGPR, an SGPR, one of the two inline constants this back end emits, or a
 * literal dword that follows the instruction. */
static float sim_src(sim_t *s, uint32_t src0, const uint32_t *w, uint32_t *i) {
    if (src0 >= 256u) {
        const uint32_t r = src0 - 256u;
        ASSERT_EQ(s->vpending[r], GL_FALSE);
        /* **A register the SPI was never asked to fill is not a register to read.** Below
         * `GL_PS_FIRST_FREE_VGPR` the file belongs to the hardware, and which of it is live
         * depends entirely on `SPI_PS_INPUT_ENA`: the barycentrics always, the window position
         * only for a shader that names `gl_FragCoord`, the face only for one that names
         * `gl_FrontFacing` - and they are packed, so enabling one moves the next.
         *
         * Reading an unasked one is not a fault on hardware. It returns whatever the previous
         * wave left, which is the same shape of bug as the missing `m0` and just as quiet. The
         * simulator knows what was asked for, so here it is an assertion instead. */
        if (r < GL_PS_FIRST_FREE_VGPR_SIM) ASSERT_TRUE(s->hw_vgpr_live[r]);
        return s->v[r];
    }
    if (src0 == 128u) return 0.0f;
    if (src0 == 242u) return 1.0f;
    if (src0 == 255u) return sim_f32(w[++(*i)]);
    if (src0 == 250u) {
        /* **DPP: a read of the lane next door, and this simulator has one lane.**
         *
         * The extra dword carries the real source register and the permute. With a single lane
         * every permute selects that lane, so the value is its own - which makes a derivative
         * come out exactly zero here. That is the honest answer for one lane rather than a
         * convenient one, and it is why the derivative tests assert instructions and whole-quad
         * mode rather than a slope: a slope needs four lanes and this has one. */
        const uint32_t tail = w[++(*i)];
        return s->v[tail & 0xffu];
    }
    if (src0 < 102u) {
        /* **A scalar read with a load still in flight is the bug this models.** The hardware
         * would return whatever the register held; there is nothing to see on a host and
         * nothing to see in the words. */
        ASSERT_EQ(s->lgkm_pending, GL_FALSE);
        return s->s[src0];
    }
    ASSERT_TRUE(0); /* an operand encoding no test has taught this simulator */
    return 0.0f;
}

/* A scalar operand read as a lane mask: `exec`, `vcc`, the inline zero, or a saved mask. */
static GLboolean sim_mask(const sim_t *s, uint32_t reg) {
    if (reg == 126u) return s->exec;
    if (reg == 106u) return s->vcc;
    if (reg == 128u) return GL_FALSE;
    ASSERT_TRUE(reg < 102u);
    return s->smask[reg];
}

static void sim_set_mask(sim_t *s, uint32_t reg, GLboolean value) {
    if (reg == 126u) { s->exec = value; return; }
    if (reg == 106u) { s->vcc = value; return; }
    ASSERT_TRUE(reg < 102u);
    s->smask[reg] = value;
}

/* **How many instructions a shader may run here before this calls it a hang.**
 *
 * Every other failure in this simulator is an assertion on a value. A loop whose condition never
 * goes false has no wrong value to assert on - it simply does not stop, and on the part it takes
 * the GPU with it. Bounding the run is what turns that into a failing test at a line number.
 * Generous: the largest loop these tests write is a few thousand trips of a few dozen
 * instructions, and nothing legitimate comes near this. */
#define SIM_MAX_STEPS 2000000

static void sim_run(sim_t *s, const uint32_t *w, uint32_t count, const float attr[4][4]) {
    const double PI = 3.14159265358979323846;
    long steps = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t x = w[i];
        if (++steps > SIM_MAX_STEPS) {
            printf("\n    the shader ran %ld instructions without ending - a loop that does not "
                   "terminate\n", steps);
            ASSERT_TRUE(0);
        }

        if (x == 0xbf810000u) { s->ended = GL_TRUE; break; }
        if (x == 0xbf800000u) continue;          /* s_nop */
        if (x == 0xbf8cc07fu) { s->lgkm_pending = GL_FALSE; continue; } /* s_waitcnt lgkmcnt(0) */
        if (x == 0xbf8c3f70u) {                                        /* s_waitcnt vmcnt(0) */
            for (int k = 0; k < 256; k++) s->vpending[k] = GL_FALSE;
            continue;
        }

        if ((x >> 26) == 0x3cu) {                /* MIMG: image_sample */
            const uint32_t w1 = w[++i];
            const uint32_t vdata = (w1 >> 8) & 0xffu;
            const uint32_t vaddr = w1 & 0xffu;
            const uint32_t srsrc = ((w1 >> 16) & 0x1fu) * 4u;
            const uint32_t ssamp = ((w1 >> 21) & 0x1fu) * 4u;
            const uint32_t dim = (x >> 3) & 0x7u;
            const uint32_t mimg_op = (x >> 18) & 0x7fu;
            const uint32_t dmask = (x >> 8) & 0xfu;
            /* `image_sample` returns a texel in four registers; `image_sample_c` compares and
             * returns one. Never `_lz`, which would give up the mip chain and the LOD bias. */
            ASSERT_TRUE(mimg_op == 32u || mimg_op == 40u);
            ASSERT_EQ(dmask, (mimg_op == 40u) ? 0x1u : 0xfu);
            ASSERT_TRUE(dim == 1u || dim == 2u || dim == 3u); /* 2D, volume or cube */
            /* The sampler's four registers sit eight above the image's eight - the layout
             * `glsl_internal.h` sets out and the prologue loads into. */
            ASSERT_EQ(ssamp, srsrc + 8u);
            ASSERT_TRUE(srsrc >= 4u);
            const uint32_t set = (srsrc - 4u) / 12u;
            ASSERT_TRUE(set < 2u);
            /* **A texture whose texel is its own coordinate**, plus the set it came through.
             * That is not a real filter and does not need to be: what these tests check is that
             * the right coordinate reached the right descriptor set, and a texel derived from
             * both says so in one value. */
            /* **A comparing sample returns the comparison, in one register.** The stored depth
             * is 0.5 and the function is less-or-equal, which is the fixture obSCEne measured
             * this against - a reference either side of 0.5 came back 0xffffffff and
             * 0xff000000. The reference is the **first** address register, so a lowering that
             * put it last would compare against `s` and this would read 0 where 1 is due. */
            if (mimg_op == 40u) {
                if (s->exec) s->v[vdata] = (s->v[vaddr] <= 0.5f) ? 1.0f : 0.0f;
                for (int k = 0; k < 1; k++) s->vpending[vdata + (uint32_t)k] = GL_TRUE;
                continue;
            }
            if (s->exec) {
                s->v[vdata + 0u] = s->v[vaddr];
                s->v[vdata + 1u] = s->v[vaddr + 1u];
                /* **A three-address lookup reports its third register instead of the set.** For
                 * a cube that is the face the direction resolved to and for a volume the slice
                 * coordinate, and either is the thing worth reading back: both are the failure
                 * those lookups have that a 2D one does not, and behind a texel carrying only
                 * u and v both would be invisible. */
                s->v[vdata + 2u] = (dim == 2u || dim == 3u) ? s->v[vaddr + 2u] : (float)set;
                s->v[vdata + 3u] = 1.0f;
            }
            for (uint32_t k = 0; k < 4u; k++) s->vpending[vdata + k] = GL_TRUE;
            s->samples++;
            s->last_tex_set = set;
            continue;
        }

        /* SOP1, which has to be tested before SOP2: its top two bits are SOP2's as well. */
        if ((x >> 23) == 0x17du) {
            const uint32_t sdst = (x >> 16) & 0x7fu;
            const uint32_t op = (x >> 8) & 0xffu;
            const uint32_t ssrc0 = x & 0xffu;
            if (op == 3u && sdst == 124u) {      /* s_mov_b32 m0, s<n> */
                /* The parameter cache address. Which scalar register it comes from is the
                 * draw's user-SGPR count - s0 with none, s2 with the block's address in
                 * s[0:1] - and both are legal; what is not legal is interpolating without it. */
                ASSERT_TRUE(ssrc0 == 0u || ssrc0 == 2u);
                s->m0_set = GL_TRUE;
            } else if (op == 3u) {               /* s_mov_b32 */
                sim_set_mask(s, sdst, sim_mask(s, ssrc0));
                /* A loop's trip counter is started with this same instruction, so the integer
                 * view is zeroed alongside the mask view. Only the inline zero: nothing else
                 * this generator emits moves an integer between scalar registers. */
                if (ssrc0 == 128u && sdst < 128u) s->scount[sdst] = 0u;
            } else if (op == 9u) {
                /* `s_wqm_b32`. One lane is modelled, so the helper lanes it would turn on are
                 * not here to turn on and this is the identity - including for a zero mask,
                 * which whole-quad mode leaves zero. What the tests can still see is that the
                 * live mask was saved before it and restored after. */
                sim_set_mask(s, sdst, sim_mask(s, ssrc0));
            } else if (op == 60u) {              /* s_and_saveexec_b32 */
                sim_set_mask(s, sdst, s->exec);
                s->exec = (GLboolean)(s->exec && s->vcc);
            } else {
                ASSERT_TRUE(0);
            }
            continue;
        }

        if ((x >> 26) == 0x3du) {                /* SMEM: a scalar load from s[0:1] */
            const uint32_t op = (x >> 18) & 0xffu;
            const uint32_t sdata = (x >> 6) & 0x7fu;
            const uint32_t sbase = x & 0x3fu;
            const uint32_t offset = w[++i] & 0x1fffffu;
            static const uint32_t WIDTH[5] = {1u, 2u, 4u, 8u, 16u};
            ASSERT_TRUE(op < 5u);
            ASSERT_EQ(sbase, 0u);                          /* s[0:1] - the block's address */
            /* **The destination's alignment, which is the assembler's rule and not the width.**
             * A single dword goes anywhere, a pair is 2-aligned, and everything four dwords and
             * wider is **4**-aligned - so `s_load_dwordx16 s[52:67]` is legal and
             * `s_load_dwordx8 s[6:13]` is not. Read out of clang by trying them. */
            {
                const uint32_t align = WIDTH[op] >= 4u ? 4u : WIDTH[op];
                ASSERT_EQ(sdata % align, 0u);
            }
            ASSERT_EQ(offset % 4u, 0u);
            for (uint32_t k = 0; k < WIDTH[op]; k++) {
                const uint32_t f = offset / 4u + k;
                /* Past the block is whatever the payload slot holds; the shader loads a whole
                 * sixteen and uses what it declared, so this is normal and reads as zero. */
                s->s[sdata + k] = (f < (uint32_t)s->ublock_floats) ? s->ublock[f] : 0.0f;
            }
            s->lgkm_pending = GL_TRUE;
            continue;
        }

        if ((x >> 26) == 0x3eu) {                /* EXP - the second dword names the registers */
            const uint32_t regs = w[++i];
            if ((x >> 10) & 0x1u) {
                /* **Compressed: two registers, four halves.** The first holds (R,G) and the
                   second (B,A), which is what `glsl_emit_export_mrt0` packs and what an
                   8_8_8_8 target requires - see that function. */
                const uint32_t lo = regs & 0xffu, hi = (regs >> 8) & 0xffu;
                /* **Only a surviving lane has to have packed anything.** A wave that discarded
                   every lane still reaches the export and still carries `done`, because that is
                   what retires it - but the packing above it is a VALU write and was skipped,
                   exactly as the hardware would skip it. Asserting unconditionally here failed
                   every shader that discards, which is a property of the simulator and not of
                   the shader. */
                if (s->exec) {
                    ASSERT_TRUE(s->vpacked[lo]);
                    ASSERT_TRUE(s->vpacked[hi]);
                }
                s->out[0] = s->vpack[lo][0];
                s->out[1] = s->vpack[lo][1];
                s->out[2] = s->vpack[hi][0];
                s->out[3] = s->vpack[hi][1];
            } else {
                for (int c = 0; c < 4; c++) s->out[c] = s->v[(regs >> (8 * c)) & 0xffu];
            }
            s->exported = GL_TRUE;
            /* **The export runs whatever exec says; the *pixel* is what exec decides.** A wave
             * that discarded every lane still exports, and still carries `done`, because that
             * is what retires it - so "did it export" and "did this lane survive" are two
             * different questions and the tests ask both. */
            s->lane_survived = s->exec;
            continue;
        }
        if ((x >> 26) == 0x32u) {                /* VINTRP */
            const uint32_t vdst = (x >> 18) & 0xffu;
            const uint32_t op = (x >> 16) & 0x3u;
            const uint32_t at = (x >> 10) & 0x3fu;
            const uint32_t ch = (x >> 8) & 0x3u;
            /* **The parameter cache has to have been addressed.** On hardware this is not a
             * fault: the interpolation reads through whatever `m0` happens to hold and returns
             * another primitive's parameters. Modelled as a failure because a simulator whose
             * every answer stayed right would be the one thing that could not have caught it. */
            ASSERT_TRUE(s->m0_set);
            if (op == 0u && s->exec) s->v[vdst] = attr[at][ch]; /* p1 loads; p2 adds nothing */
            continue;
        }
        if ((x >> 25) == 0x3fu) {                /* VOP1 */
            const uint32_t vdst = (x >> 17) & 0xffu;
            const uint32_t op = (x >> 9) & 0xffu;
            const float a = sim_src(s, x & 0x1ffu, w, &i);
            float r = 0.0f;
            switch (op) {
                case 1u:  r = a; break;                              /* v_mov_b32 */
                case 32u: r = a - floorf(a); break;                  /* v_fract_f32 */
                case 33u: r = truncf(a); break;
                case 34u: r = ceilf(a); break;
                case 36u: r = floorf(a); break;
                case 37u: r = powf(2.0f, a); break;                  /* base two */
                case 39u: r = logf(a) / logf(2.0f); break;           /* base two */
                /* **`v_rcp_f32` is accurate to one unit in the last place, not correctly
                 * rounded**, and a host divide is correctly rounded - so simulating it as
                 * `1/a` models something better than the part and hides every bug that lives
                 * in that gap. The worst case for a truncating consumer is a reciprocal a
                 * shade low, which turns `7 * rcp(7)` into a hair under 1.0 and `7 / 7` into
                 * 0, so that is what is modelled: one ULP down, every time.
                 *
                 * Deliberately pessimistic rather than random. A simulator that sometimes
                 * reproduced the hazard would make a test that sometimes passed. */
                case 42u: r = nextafterf(1.0f / a, (a > 0.0f) ? 0.0f : -3.0e38f); break;
                case 46u: r = 1.0f / sqrtf(a); break;
                case 51u: r = sqrtf(a); break;
                case 53u: r = sinf((float)(2.0 * PI) * a); break;    /* revolutions */
                case 54u: r = cosf((float)(2.0 * PI) * a); break;
                default: ASSERT_TRUE(0); break;
            }
            if (s->exec) s->v[vdst] = r;
            continue;
        }
        if ((x >> 25) == 0x3eu) {                /* VOPC, into vcc_lo */
            const uint32_t op = (x >> 17) & 0xffu;
            const float b = s->v[(x >> 9) & 0xffu];
            const float a = sim_src(s, x & 0x1ffu, w, &i);
            GLboolean r = GL_FALSE;
            switch (op) {
                case 1u:  r = (GLboolean)(a < b); break;
                case 2u:  r = (GLboolean)(a == b); break;
                case 3u:  r = (GLboolean)(a <= b); break;
                case 4u:  r = (GLboolean)(a > b); break;
                case 6u:  r = (GLboolean)(a >= b); break;
                case 13u: r = (GLboolean)(a != b); break;
                default: ASSERT_TRUE(0); break;
            }
            /* **A comparison writes zero for an inactive lane**, which is what makes
             * `s_and_saveexec_b32` inside a dead branch narrow to nothing rather than to
             * whatever the arithmetic in that branch happened to produce. */
            s->vcc = (GLboolean)(s->exec && r);
            continue;
        }
        /* **SOPP: the branches, and the only way this simulator's program counter moves.**
         * Tested before SOP2, whose top two bits it shares - falling through to that arm is how
         * an unhandled branch would present, which is an assertion rather than a jump.
         *
         * `simm16` counts from the word *after* the branch, so the target is `i + 1 + simm` and
         * the loop's own `i++` is what the -1 accounts for. */
        if ((x >> 23) == 0x17fu) {
            const uint32_t op = (x >> 16) & 0x7fu;
            const int32_t simm = (int32_t)(int16_t)(uint16_t)(x & 0xffffu);
            GLboolean take;
            if (op == 2u) take = GL_TRUE;                      /* s_branch */
            else if (op == 5u) take = s->scc;                  /* s_cbranch_scc1 */
            else if (op == 8u) take = (GLboolean)!s->exec;     /* s_cbranch_execz */
            else { ASSERT_TRUE(0); take = GL_FALSE; }
            if (take) {
                const int32_t target = (int32_t)i + 1 + simm;
                ASSERT_TRUE(target >= 0 && (uint32_t)target <= count);
                i = (uint32_t)target - 1u;   /* the loop's `i++` lands on `target` */
            }
            continue;
        }
        /* SOPC: the scalar compare that sets SCC. Also before SOP2, for the same reason. */
        if ((x >> 23) == 0x17eu) {
            const uint32_t op = (x >> 16) & 0x7fu;
            const uint32_t ssrc1 = (x >> 8) & 0xffu;
            const uint32_t ssrc0 = x & 0xffu;
            uint32_t rhs;
            ASSERT_EQ(op, 9u);                                 /* s_cmp_ge_u32 */
            ASSERT_TRUE(ssrc0 < 128u);
            if (ssrc1 == 255u) rhs = w[++i];                   /* the trailing literal */
            else { ASSERT_TRUE(ssrc1 >= 128u && ssrc1 <= 192u); rhs = ssrc1 - 128u; }
            s->scc = (GLboolean)(s->scount[ssrc0] >= rhs);
            continue;
        }
        if ((x >> 30) == 0x2u) {                 /* SOP2 */
            const uint32_t op = (x >> 23) & 0x7fu;
            const uint32_t sdst = (x >> 16) & 0x7fu;
            const uint32_t ssrc1 = (x >> 8) & 0xffu;
            const uint32_t ssrc0 = x & 0xffu;
            /* `s_add_u32` first: its operands are an integer and an inline constant, neither of
             * which `sim_mask` can read. **Scalar arithmetic is not exec-masked** - it runs
             * whatever the lanes are doing, which is exactly what makes a trip guard a guard. */
            if (op == 0u) {
                ASSERT_TRUE(sdst < 128u && ssrc0 < 128u);
                ASSERT_TRUE(ssrc1 >= 128u && ssrc1 <= 192u);
                s->scount[sdst] = s->scount[ssrc0] + (ssrc1 - 128u);
                continue;
            }
            const GLboolean a = sim_mask(s, ssrc0);
            const GLboolean b = sim_mask(s, ssrc1);
            if (op == 14u) {                     /* s_and_b32 */
                sim_set_mask(s, sdst, (GLboolean)(a && b));
            } else if (op == 20u) {              /* s_andn2_b32: a & ~b */
                sim_set_mask(s, sdst, (GLboolean)(a && !b));
            } else {
                ASSERT_TRUE(0);
            }
            continue;
        }
        /* **VOP3, which here is only the cube face selection.** Three sources at once is what
         * puts these in VOP3 at all, and the four of them are the whole of what this back end
         * emits in that encoding - so an opcode arriving here that is not one of them is a
         * change this simulator has not been told about, and says so rather than guessing.
         *
         * The mapping is the ISA's: the largest component picks the axis, its sign picks which
         * of the pair, and the other two become `sc` and `tc` with the signs that keep every
         * face oriented the same way round. `ma` is twice the major axis, which is why the
         * shader divides by `2|ma|` rather than by `|ma|`. */
        if ((x >> 26) == 0x35u) {
            const uint32_t op = (x >> 16) & 0x3ffu;
            const uint32_t vdst = x & 0xffu;
            const uint32_t w1 = w[++i];
            const float X = s->v[(w1 & 0x1ffu) - 256u];
            const float Y = s->v[((w1 >> 9) & 0x1ffu) - 256u];
            const float Z = s->v[((w1 >> 18) & 0x1ffu) - 256u];
            const float ax = X < 0.0f ? -X : X;
            const float ay = Y < 0.0f ? -Y : Y;
            const float az = Z < 0.0f ? -Z : Z;
            float id, sc, tc, ma;
            if (az >= ax && az >= ay) {
                ma = 2.0f * Z; id = (Z < 0.0f) ? 5.0f : 4.0f;
                sc = (Z < 0.0f) ? -X : X; tc = -Y;
            } else if (ay >= ax) {
                ma = 2.0f * Y; id = (Y < 0.0f) ? 3.0f : 2.0f;
                sc = X; tc = (Y < 0.0f) ? -Z : Z;
            } else {
                ma = 2.0f * X; id = (X < 0.0f) ? 1.0f : 0.0f;
                sc = (X < 0.0f) ? Z : -Z; tc = -Y;
            }
            if (s->exec) {
                switch (op) {
                    case 0x144u: s->v[vdst] = id; break;
                    case 0x145u: s->v[vdst] = sc; break;
                    case 0x146u: s->v[vdst] = tc; break;
                    case 0x147u: s->v[vdst] = ma; break;
                    default: ASSERT_TRUE(0); break;
                }
            }
            continue;
        }
        {                                        /* VOP2 */
            const uint32_t op = (x >> 25) & 0x3fu;
            const uint32_t vdst = (x >> 17) & 0xffu;
            if (op == 47u) { /* v_cvt_pkrtz_f16_f32 - the colour export's packing */
                /* **Read straight out of the file, as the export itself always has.** These
                   two instructions are the export's epilogue and they read exactly the
                   registers the uncompressed export used to name in its second dword, which
                   were never put through `sim_src`. They are registers the shader wrote, so
                   the hardware-liveness question `sim_src` asks - was the SPI ever asked to
                   fill this - is not about them and answering it would fail every shader that
                   keeps its colour low in the file.

                   The result is not a float, so it goes in the pack table and `s->v[vdst]` is
                   left alone: reading a packed register as a float should not be plausible. */
                const uint32_t s0 = x & 0x1ffu;
                const float lo = s0 >= 256u ? s->v[s0 - 256u] : 0.0f;
                const float hi = s->v[(x >> 9) & 0xffu];
                if (s->exec) {
                    s->vpack[vdst][0] = sim_f16_rtz(lo);
                    s->vpack[vdst][1] = sim_f16_rtz(hi);
                    s->vpacked[vdst] = GL_TRUE;
                }
                continue;
            }
            const float b = s->v[(x >> 9) & 0xffu];
            const float a = sim_src(s, x & 0x1ffu, w, &i);
            float r = 0.0f;
            switch (op) {
                case 1u:  r = s->vcc ? b : a; break;                 /* false value is src0 */
                case 3u:  r = a + b; break;
                case 4u:  r = a - b; break;                          /* src0 - vsrc1 */
                case 8u:  r = a * b; break;
                case 15u: r = a < b ? a : b; break;
                case 16u: r = a > b ? a : b; break;
                case 43u: r = s->v[vdst] + a * b; break;             /* v_fmac_f32 */
                default: ASSERT_TRUE(0); break;
            }
            if (s->exec) s->v[vdst] = r;
        }
    }
}

/* Compiles an already-linked program for the console, runs the words and hands back the exported
 * colour.
 *
 * **The uniform block is `p->values`** - the pool `glUniform*` writes and the draw path copies
 * into the payload verbatim - so a test that sets a uniform through the API and reads the colour
 * back out has been through the same bytes the hardware would. */
/* The window position the simulated SPI hands the shader, and the viewport it hands the draw.
 * Chosen so no two are equal and none is 0 or 1: a shader reading the wrong one of the four, or
 * skipping the y flip, lands on a number no other component could have produced. `y` is the
 * hardware's - counted down from the top - so `gl_FragCoord.y` has to come out
 * SIM_TARGET_H - SIM_FRAG_Y = 1059.5.
 *
 * **The render target's height, not the viewport's.** `gl_FragCoord` is window-relative, so a
 * shader drawing into a corner of a 1080-row target still counts from the bottom of the target.
 * Seeding this with a viewport-sized number would agree with a back end that flipped by the
 * viewport - which is the bug gl2-probe found, and a simulator that shared it would have kept
 * quiet about it. */
#define SIM_TARGET_H 1080.0f
#define SIM_FRAG_X     10.5f
#define SIM_FRAG_Y     20.5f
#define SIM_FRAG_Z     0.25f
#define SIM_FRAG_W     2.0f
/* Positive, so `gl_FrontFacing` is true; distinct from every other seeded value so reading it
 * by mistake shows up as a number that could have come from nowhere else. */
#define SIM_FRONT_FACE 7.5f

static GLboolean compile_and_run_prog(void *ctx, GLuint prog, const float attr[4][4],
                                      float out[4]) {
    const gl_program_object_t *p = gl_find_program((gl_context_t *)ctx, prog);
    ASSERT_TRUE(p != NULL);

    static uint32_t words[512];
    uint32_t count = 0u, vgprs = 0u, ena = 0u;
    char log[256] = {0};
    const GLboolean ok =
        gl_program_compile_fragment(p, words, 512u, &count, &vgprs, NULL, &ena, log,
                                    sizeof(log));
    if (!ok) printf("\n    compile failed: %s\n", log);
    ASSERT_EQ(ok, GL_TRUE);
    /* Whatever it emitted has to fit the file the stage table allocated. */
    ASSERT_TRUE(vgprs <= 136u);

    sim_t s;
    memset(&s, 0, sizeof(s));
    s.exec = GL_TRUE;            /* the lane starts live */
    s.ublock_floats = (int)(sizeof(s.ublock) / sizeof(s.ublock[0]));
    /* The block as the draw path builds it: descriptors first, the draw's own constants in the
     * second set's tail, the value pool at 0x80. */
    for (int i = 0; i < p->value_floats; i++) {
        s.ublock[OOPS_GL_GL2_UNIFORM_AT / 4 + i] = p->values[i];
    }
    s.ublock[OOPS_GL_GL2_DRAWCONST_AT / 4 + OOPS_GL_GL2_DC_TARGET_H] = SIM_TARGET_H;

    /* **The SPI fills exactly what it was asked for, packed in order**, and this models that
     * rather than filling the low registers and hoping. `input_ena` came out of the compile, so
     * the two cannot disagree: the barycentrics are always live, the window position follows
     * when the shader named `gl_FragCoord`, and the face follows whatever is there.
     *
     * Everything else below v8 stays dead, and `sim_src` refuses to read a dead one. That is
     * what makes a wrong register number a failure here rather than a plausible value - which
     * is the whole hazard, since the packing moves the face from v2 to v6 depending on a bit
     * set somewhere else entirely. */
    s.hw_vgpr_live[0] = GL_TRUE; /* the i barycentric */
    s.hw_vgpr_live[1] = GL_TRUE; /* and j */
    {
        uint32_t next = 2u;
        if ((ena & 0x00000f00u) != 0u) {
            s.v[next] = SIM_FRAG_X; s.hw_vgpr_live[next++] = GL_TRUE;
            s.v[next] = SIM_FRAG_Y; s.hw_vgpr_live[next++] = GL_TRUE;
            s.v[next] = SIM_FRAG_Z; s.hw_vgpr_live[next++] = GL_TRUE;
            s.v[next] = SIM_FRAG_W; s.hw_vgpr_live[next++] = GL_TRUE;
        }
        if ((ena & 0x00001000u) != 0u) {
            /* Positive is front-facing; the sign is the answer, not the value. */
            s.v[next] = SIM_FRONT_FACE; s.hw_vgpr_live[next++] = GL_TRUE;
        }
    }
    sim_run(&s, words, count, attr);
    ASSERT_EQ(s.exported, GL_TRUE);
    ASSERT_EQ(s.ended, GL_TRUE);
    for (int i = 0; i < 4; i++) out[i] = s.out[i];
    return s.lane_survived;
}

/* Links, then the above. The return is whether the lane survived to the export - false when the
 * shader discarded it. */
static GLboolean compile_and_run(void *ctx, const char *vs_src, const char *fs_src,
                                 const float attr[4][4], float out[4]) {
    return compile_and_run_prog(ctx, linked_program(vs_src, fs_src), attr, out);
}

/* **The tolerance carries the colour export's own precision, on top of whatever is asked for.**
 *
 * Everything compared here reached the test through `gl_FragColor`, and since 2026-09-23 that
 * leaves the shader as two packed half-floats: an 8_8_8_8 target on a part with RB+ takes
 * `SPI_SHADER_FP16_ABGR` and nothing else (`glsl_emit_export_mrt0`). Ten mantissa bits truncated
 * toward zero is up to one part in 1024 of the value, so an assertion written at 1e-6 was
 * asking the export for an exactness the hardware has never had - it only used to pass because
 * the simulator carried 32 bits through an instruction that does not.
 *
 * Scaling by the expected magnitude rather than loosening to a flat number keeps the assertions
 * sharp where it matters: these tests separate a component from its neighbours, a flipped y from
 * an unflipped one, a uniform from a descriptor. Those differ by far more than a part in 1024,
 * and anything that does not was never going to survive an 8-bit channel either. */
#define ASSERT_NEAR(a, b, tol)                                                                 \
    do {                                                                                       \
        const float _a = (float)(a), _b = (float)(b);                                          \
        const float _d = _a > _b ? _a - _b : _b - _a;                                          \
        const float _mag = _b > 0.0f ? _b : -_b;                                               \
        const float _lim = (float)(tol) + _mag * (1.0f / 1024.0f);                             \
        if (!(_d <= _lim)) {                                                                   \
            printf("\n    %s = %f, expected %f\n", #a, (double)_a, (double)_b);                \
        }                                                                                      \
        ASSERT_TRUE(_d <= _lim);                                                               \
    } while (0)

/* The vertex shader every simulation below pairs with: one vec4 varying, which lands in
 * parameter 0 and is what `attr[0]` fills. */
static const char *const VS_ONE_VARYING =
    "attribute vec4 pos;\n"
    "varying vec4 vin;\n"
    "void main() { vin = pos; gl_Position = pos; }\n";

static void test_gl2_frag_coord_comes_from_the_window_position(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* **Every component, in one export**, so reading the wrong register shows up as the wrong
     * channel rather than as a value that could be anything. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() { gl_FragColor = gl_FragCoord; }\n", attr, o);
    ASSERT_NEAR(o[0], SIM_FRAG_X, 1e-6f);
    /* **y is flipped and the others are not.** GL counts `gl_FragCoord.y` up from the bottom of
     * the window and the hardware counts down from the top, so this is the viewport height less
     * what the SPI supplied. A back end that passed the hardware value straight through would
     * draw every gradient upside down - and would pass a test that only checked x. */
    ASSERT_NEAR(o[1], SIM_TARGET_H - SIM_FRAG_Y, 1e-6f);
    ASSERT_NEAR(o[2], SIM_FRAG_Z, 1e-6f);
    ASSERT_NEAR(o[3], SIM_FRAG_W, 1e-6f);

    /* A swizzle of it is the same registers read in another order, which is what almost every
     * real shader does with it. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() { gl_FragColor = vec4(gl_FragCoord.yx, 0.0, 1.0); }\n", attr,
                    o);
    ASSERT_NEAR(o[0], SIM_TARGET_H - SIM_FRAG_Y, 1e-6f);
    ASSERT_NEAR(o[1], SIM_FRAG_X, 1e-6f);

    /* **A shader that never names it is not charged for it.** The declaration, the scalar load
     * and the four registers all hang off the mention, so this is the arm that says the cost is
     * conditional - and `input_ena` is what the draw configures the stage with. */
    gl_context_t *c = (gl_context_t *)ctx;
    const GLuint plain = linked_program(VS_ONE_VARYING,
                                        "void main() { gl_FragColor = vec4(1.0); }\n");
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u, ena = 0u, usg = 0u;
    char log[256] = {0};
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, plain), words, 256u, &count,
                                          &vgprs, &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    ASSERT_EQ(ena, 0x00000002u);  /* the barycentrics only */
    ASSERT_EQ(usg, 0u);           /* and no block, so no user SGPRs */

    const GLuint uses = linked_program(
        VS_ONE_VARYING, "void main() { gl_FragColor = vec4(gl_FragCoord.xyz, 1.0); }\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, uses), words, 256u, &count, &vgprs,
                                          &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    /* PERSP_CENTER plus POS_X/Y/Z/W - bits 8..11 of SPI_PS_INPUT_ENA for gfx103, which is
     * R_0286CC and not R_02865C (that address is this register only from gfx12). */
    ASSERT_EQ(ena, 0x00000f02u);
    /* **And it takes the block**, though it declares no uniform and samples nothing: the
     * viewport height that flips y lives there. A shader handed no block would have read the
     * flip out of a scalar register nothing loaded. */
    ASSERT_EQ(usg, 2u);

    glContextDestroy(ctx);
}

static void test_gl2_loops_are_unrolled_when_the_count_is_known(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* 1+2+3+4+5 = 15, which is a sum no single iteration produces - so a loop that ran once,
     * or ran with the counter stuck, gives a different answer rather than a near one. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 1; i <= 5; i++) { total += float(i); }\n"
                    "  gl_FragColor = vec4(total * 0.01, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.15f, 1e-6f);

    /* Counting down, and a step that is not one - the trip count is computed the way the
     * reference runs the loop, test then body then step, so an off-by-one shows up as a
     * different sum. 10 + 8 + 6 + 4 + 2 = 30. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 10; i > 0; i -= 2) { total += float(i); }\n"
                    "  gl_FragColor = vec4(total * 0.01, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.30f, 1e-6f);

    /* A condition that is false at the start runs the body no times, which an unroller that
     * always emitted one copy would get wrong. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 7.0;\n"
                    "  for (int i = 0; i < 0; i++) { total = 0.0; }\n"
                    "  gl_FragColor = vec4(total * 0.1, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.7f, 1e-6f);

    /* The counter is a fresh value each trip, so a body that assigns to it does not carry the
     * change into the next one - the loop's own step decides that, as the language says. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 0; i < 3; i++) { int j = i; j = j + 10; total += float(j); }\n"
                    "  gl_FragColor = vec4(total * 0.01, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.33f, 1e-6f); /* 10 + 11 + 12 */

    /* **A `break` belongs to the loop it is in, and to no other.** Two loops here, one of them
     * unrollable and one not - and the unrollable one has to compile. A check that looked for
     * `break` anywhere in the shader rather than inside this loop's own body refuses both, and
     * names the wrong one while doing it. A nested loop's `break` is its own for the same
     * reason, which is why the search stops at one. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "float other(float s) {\n"
                    "  for (int k = 0; k < 2; k++) { s += 1.0; }\n"
                    "  return s;\n"
                    "}\n"
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 0; i < 3; i++) { total += 1.0; }\n"
                    "  gl_FragColor = vec4(other(total) * 0.1, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-6f); /* 3 from one loop, 2 from the other */

    glContextDestroy(ctx);
}

/* **The loops that branch**, which is the only backward jump this back end emits.
 *
 * Every value below is one the unrolled path cannot produce: either the loop runs more times
 * than the unroller copies out, or it leaves early. The single lane this simulator runs cannot
 * show divergence - two lanes leaving a loop on different trips is a hardware question - but it
 * shows the whole of the per-lane semantics, which is where the trip counts and the masks are.
 */
static void test_gl2_loops_that_branch_run_break_and_continue(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* More trips than the unroller writes out. 200 of them, which no copy count reaches. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 0; i < 200; i++) { total += 1.0; }\n"
                    "  gl_FragColor = vec4(total * 0.001, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.2f, 1e-6f);

    /* `break` leaves on the sixth trip, so the answer is 5 and not 200. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 0; i < 200; i++) { if (total >= 5.0) break; total += 1.0; }\n"
                    "  gl_FragColor = vec4(total * 0.1, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-6f);

    /* **`continue` skips the rest of the body and still counts the trip**, which is the one
     * thing about it that can be got wrong silently.
     *
     * The step runs under the loop's own mask, not the body's - a `continue` that left `exec`
     * cleared over the step would stop the counter for that lane while `n` kept going, so the
     * loop would take three extra trips to reach 100 and `total` would come out at 100 instead
     * of 97. Both are plausible numbers; only one of them is this loop. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  float n = 0.0;\n"
                    "  for (int i = 0; i < 100; i++) {\n"
                    "    n += 1.0;\n"
                    "    if (n < 3.5) continue;\n"
                    "    total += 1.0;\n"
                    "  }\n"
                    "  gl_FragColor = vec4(total * 0.01, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.97f, 1e-6f); /* 100 trips, three of them skipped */

    /* A `break` two `if`s deep. It has to take the lane out of both saved masks on the way out,
     * or the inner restore hands it back and the loop carries on to 200. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 0; i < 200; i++) {\n"
                    "    total += 1.0;\n"
                    "    if (total > 2.0) { if (total > 4.0) { break; } }\n"
                    "  }\n"
                    "  gl_FragColor = vec4(total * 0.1, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-6f);

    /* **The inner `break` belongs to the inner loop and to no other.** Each pass of the outer
     * loop runs the inner one to its own `break` at j = 91, adding 91; the outer stops once the
     * total passes 200, which takes three passes. A `break` that reached the outer loop's mask
     * as well would leave after the first pass with 91 - a plausible number, and not this one. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 0; i < 100; i++) {\n"
                    "    for (int j = 0; j < 100; j++) {\n"
                    "      if (float(j) > 90.0) break;\n"
                    "      total += 1.0;\n"
                    "    }\n"
                    "    if (total > 200.0) break;\n"
                    "  }\n"
                    "  gl_FragColor = vec4(total * 0.001, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.273f, 1e-6f); /* three passes of 91 */

    /* A condition false on arrival runs the body no times - the `s_cbranch_execz` exit, which
     * is the one path out of the loop that is taken before anything in it has run. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 7.0;\n"
                    "  for (int i = 0; i < 0; i++) { if (total > 0.0) break; total = 0.0; }\n"
                    "  gl_FragColor = vec4(total * 0.1, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.7f, 1e-6f);

    /* **A `discard` inside a loop has to survive the next trip.** The loop reloads `exec` from
     * its active mask at the top of every trip, so a discarded lane left in that mask is handed
     * straight back and reaches the export alive - the fragment would be written rather than
     * thrown away, and the colour would be whatever the loop finished with. */
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING,
                              "void main() {\n"
                              "  float total = 0.0;\n"
                              "  for (int i = 0; i < 200; i++) {\n"
                              "    total += 1.0;\n"
                              "    if (total > 3.5) discard;\n"
                              "  }\n"
                              "  gl_FragColor = vec4(total * 0.1, 0.0, 0.0, 1.0);\n"
                              "}\n",
                              attr, o),
              GL_FALSE);

    glContextDestroy(ctx);
}

/* **The two shaders gl2-probe runs on the console for `control-flow` and `short-circuit`.**
 *
 * Both were refused by the compiled back end until now - the first for its trip count, the
 * second for its right operand - so the probe reported `0x0502` for each on hardware while the
 * software reference ran them. These are those exact sources, compiled and run here, so the
 * host says what the console is about to. */
static void test_gl2_the_probes_control_flow_shaders_compile_and_run(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* `control-flow`: 99 trips with a `break` at 5 and a `continue` on the way, over integer
     * comparisons - which are the float comparison of the same two registers, an `int` here
     * being a float kept whole. 1+2+3+4+5 = 15, and 15 * 0.05 = 0.75. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 1; i < 100; i++) {\n"
                    "    if (i > 5) break;\n"
                    "    if (i == 3) { total += float(i); continue; }\n"
                    "    total += float(i);\n"
                    "  }\n"
                    "  gl_FragColor = vec4(total * 0.05, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.75f, 1e-6f);

    /* `short-circuit`: the right operand writes through an `out` parameter, so whether it ran
     * is visible in the answer rather than only in the timing. `never && mark(a)` must leave
     * `a` at zero and `always || mark(b)` must leave `b` at zero; an implementation that
     * evaluated both sides sets each to one and gives a different colour. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "bool mark(out float touched) { touched = 1.0; return true; }\n"
                    "void main() {\n"
                    "  float a = 0.0;\n"
                    "  float b = 0.0;\n"
                    "  bool never = false;\n"
                    "  bool always = true;\n"
                    "  if (never && mark(a)) { a = 1.0; }\n"
                    "  if (always || mark(b)) { b = 0.0; }\n"
                    "  gl_FragColor = vec4(a, b, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, 1e-6f);
    ASSERT_NEAR(o[1], 0.0f, 1e-6f);

    /* **And the other way round, so the test is not passed by never running the right side at
     * all.** Here the left operand does not decide, so the right one must run and its mark must
     * land. A back end that dropped the right side would give zero for both. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "bool mark(out float touched) { touched = 1.0; return true; }\n"
                    "void main() {\n"
                    "  float a = 0.0;\n"
                    "  float b = 0.0;\n"
                    "  bool always = true;\n"
                    "  bool never = false;\n"
                    "  bool r = always && mark(a);\n"
                    "  bool s = never || mark(b);\n"
                    "  gl_FragColor = vec4(a, b, (r && s) ? 1.0 : 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f);
    ASSERT_NEAR(o[1], 1.0f, 1e-6f);
    ASSERT_NEAR(o[2], 1.0f, 1e-6f);

    /* **gl2-probe's `loop-divergence` shader, at both ends of its gradient.**
     *
     * On the console this runs with every column of the quad breaking on a different trip, which
     * is the thing a one-lane simulator cannot reproduce - what it can do is run the same shader
     * twice with the varying at each end and check the trip count follows it. If these two came
     * out the same the probe's gradient would be flat for a reason that has nothing to do with
     * the masks, and the hardware result would be unreadable. */
    {
        static const char *const FS_DIVERGE =
            "varying vec4 vin;\n"
            "void main() {\n"
            "  float total = 0.0;\n"
            "  for (int i = 0; i < 40; i++) {\n"
            "    if (float(i) > vin.x * 32.0) break;\n"
            "    total += 1.0;\n"
            "  }\n"
            "  gl_FragColor = vec4(total * 0.03, 0.0, 0.0, 1.0);\n"
            "}\n";
        const float lo[4][4] = {{0.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        const float hi[4][4] = {{1.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        compile_and_run(ctx, VS_ONE_VARYING, FS_DIVERGE, lo, o);
        ASSERT_NEAR(o[0], 0.03f, 1e-6f);  /* one trip: i = 0 is not > 0 */
        compile_and_run(ctx, VS_ONE_VARYING, FS_DIVERGE, hi, o);
        ASSERT_NEAR(o[0], 0.99f, 1e-6f);  /* thirty-three: i = 0..32 */
    }

    /* And `discard-in-loop`, on both sides of its threshold. The discarding side must not come
     * back - the loop reloads `exec` from its own mask every trip, and a lane left in that mask
     * is handed straight back.
     *
     * **A hundred trips, so the loop branches.** At twenty-four it unrolls, and an unrolled
     * loop has no reload to get wrong - the check would pass without reaching the path it is
     * for. It also would not fit: twenty-four copies of this body is over the 512-instruction
     * limit, which is how the difference first showed up. */
    {
        static const char *const FS_DISCARD_LOOP =
            "varying vec4 vin;\n"
            "void main() {\n"
            "  float total = 0.0;\n"
            "  for (int i = 0; i < 100; i++) {\n"
            "    total += 1.0;\n"
            "    if (vin.x > 0.5 && total > 4.0) discard;\n"
            "  }\n"
            "  gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);\n"
            "}\n";
        const float keep[4][4] = {{0.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        const float kill[4][4] = {{1.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, FS_DISCARD_LOOP, keep, o), GL_TRUE);
        ASSERT_NEAR(o[1], 1.0f, 1e-6f);
        ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, FS_DISCARD_LOOP, kill, o), GL_FALSE);
    }

    /* **`early-return` and `local-arrays`, the probe's own sources.** Both features are new
     * enough that the software reference running them says nothing about the compiled path -
     * which is exactly how `control-flow` and `short-circuit` sat as `0x0502` refusals on
     * hardware while the host reported them passing. */
    {
        static const char *const FS_EARLY =
            "varying vec4 vin;\n"
            "float pick(float a) {\n"
            "  if (a < 0.5) { return 0.25; }\n"
            "  return 1.0;\n"
            "}\n"
            "void main() {\n"
            "  float r = pick(vin.x);\n"
            "  gl_FragColor = vec4(r, 1.0, 0.0, 1.0);\n"
            "}\n";
        const float lo[4][4] = {{0.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        const float hi[4][4] = {{1.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        compile_and_run(ctx, VS_ONE_VARYING, FS_EARLY, lo, o);
        ASSERT_NEAR(o[0], 0.25f, 1e-6f);
        ASSERT_NEAR(o[1], 1.0f, 1e-6f); /* the caller's green, after the call */
        compile_and_run(ctx, VS_ONE_VARYING, FS_EARLY, hi, o);
        ASSERT_NEAR(o[0], 1.0f, 1e-6f);
        ASSERT_NEAR(o[1], 1.0f, 1e-6f);
    }
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float w[4];\n"
                    "  for (int i = 0; i < 4; i++) { w[i] = float(i) + 1.0; }\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 0; i < 4; i++) { total += w[i]; }\n"
                    "  vec3 v[2];\n"
                    "  v[0] = vec3(0.0, 0.25, 0.5);\n"
                    "  v[1] = vec3(0.75, 1.0, 0.0);\n"
                    "  gl_FragColor = vec4(total * 0.1, v[1].x, v[0].z, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f);   /* 1+2+3+4 = 10 */
    ASSERT_NEAR(o[1], 0.75f, 1e-6f);
    ASSERT_NEAR(o[2], 0.5f, 1e-6f);

    /* **gl2-probe's `texture-cube` shader**, whose shape differs from the tests above: the
     * direction arrives as a `varying vec3` rather than a literal, so the three components the
     * face selection reads are interpolated registers. A vec3 varying is also the case where
     * the parameter packing could hand over the wrong third component. */
    {
        const float hi[4][4] = {{1.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        compile_and_run(ctx, VS_ONE_VARYING,
                        "uniform samplerCube sky;\n"
                        "varying vec4 vin;\n"
                        "void main() {\n"
                        "  vec3 dir = vec3(vin.x, 0.0, 0.0);\n"
                        "  gl_FragColor = textureCube(sky, dir);\n"
                        "}\n",
                        hi, o);
        ASSERT_NEAR(o[2], 0.0f, 1e-6f); /* +X is face 0 */
    }

    /* `^^` has no short-circuit in the language, so a right side that assigns is correct rather
     * than a problem - both sides always run and the mark always lands. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "bool mark(out float touched) { touched = 1.0; return true; }\n"
                    "void main() {\n"
                    "  float a = 0.0;\n"
                    "  bool never = false;\n"
                    "  bool r = never ^^ mark(a);\n"
                    "  gl_FragColor = vec4(a, r ? 1.0 : 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f);
    ASSERT_NEAR(o[1], 1.0f, 1e-6f);

    glContextDestroy(ctx);
}

/* Integer comparisons, which are the float comparison of the same registers. */
static void test_gl2_integer_comparisons_are_the_float_ones(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Each channel is a different operator over values that make the wrong answer a different
     * colour. `==` on integers is exact - whole numbers have one representation each - which is
     * the thing `==` on floats cannot promise. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  int a = 7;\n"
                    "  int b = 3;\n"
                    "  float r = (a > b) ? 1.0 : 0.0;\n"
                    "  float g = (a - 4 == b) ? 1.0 : 0.0;\n"
                    "  float bl = (b >= a) ? 1.0 : 0.0;\n"
                    "  gl_FragColor = vec4(r, g, bl, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f);
    ASSERT_NEAR(o[1], 1.0f, 1e-6f);
    ASSERT_NEAR(o[2], 0.0f, 1e-6f);

    /* A negative integer, where truncation towards zero and the comparison have to agree - the
     * division below is -7/2 = -3, not -4, and -3 > -4. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  int q = -7 / 2;\n"
                    "  float r = (q == -3) ? 1.0 : 0.0;\n"
                    "  float g = (q < 0) ? 1.0 : 0.0;\n"
                    "  gl_FragColor = vec4(r, g, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f);
    ASSERT_NEAR(o[1], 1.0f, 1e-6f);

    glContextDestroy(ctx);
}

/* **The trip guard ships in the words**, which no value test can show.
 *
 * The guard is unreachable from GLSL by construction: the trip count is known when the shader
 * is compiled, a body that moves the counter is refused, and a loop whose bound is not constant
 * never gets here. So nothing a shader can write makes it fire - it is there for a bug in this
 * generator, and the only way to check it is present is to look. */
static void test_gl2_a_branched_loop_carries_its_trip_guard(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[512];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};

    const GLuint prog = linked_program(VS_ONE_VARYING,
                                       "void main() {\n"
                                       "  float total = 0.0;\n"
                                       "  for (int i = 0; i < 200; i++) { total += 1.0; }\n"
                                       "  gl_FragColor = vec4(total * 0.001, 0.0, 0.0, 1.0);\n"
                                       "}\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, prog), words, 512u, &count, &vgprs,
                                          NULL, NULL, log, sizeof(log)),
              GL_TRUE);

    int backward = 0, guards = 0, compares = 0;
    uint32_t back_at = 0u, back_target = 0u;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t x = words[i];
        if ((x >> 23) == 0x17fu) {
            const uint32_t op = (x >> 16) & 0x7fu;
            const int32_t simm = (int32_t)(int16_t)(uint16_t)(x & 0xffffu);
            if (op == 2u && simm < 0) {
                backward++;
                back_at = i;
                back_target = (uint32_t)((int32_t)i + 1 + simm);
            }
            if (op == 5u) guards++;                      /* s_cbranch_scc1 */
        }
        if ((x >> 23) == 0x17eu && ((x >> 16) & 0x7fu) == 9u) {
            compares++;                                  /* s_cmp_ge_u32 */
            i++;                                         /* its literal: 200 does not inline */
        }
    }
    ASSERT_EQ(backward, 1);   /* one loop, one way round */
    ASSERT_EQ(compares, 1);
    ASSERT_EQ(guards, 1);
    /* The jump goes back into the shader, not past its start, and not forward. */
    ASSERT_TRUE(back_target < back_at);
    /* And the ceiling is the trip count this loop was measured to have, not a round number. */
    /* Still ends properly: `s_endpgm` with the export tail's two words of room after it. */
    ASSERT_EQ(words[count - 3u], 0xbf810000u);
    {
        GLboolean found = GL_FALSE;
        for (uint32_t i = 0; i + 1u < count; i++) {
            if ((words[i] >> 23) == 0x17eu && (words[i] & 0xff00u) == 0xff00u) {
                ASSERT_EQ(words[i + 1u], 200u);
                found = GL_TRUE;
            }
        }
        ASSERT_EQ(found, GL_TRUE);
    }

    glContextDestroy(ctx);
}

/* The loops that are still refused, each by name.
 *
 * The list is shorter than it was: a loop with more trips than the unroller writes out, and one
 * with a `break` or a `continue`, both used to be here and are now generated as real branches.
 * What is left are the loops where the refusal is not about the lowering but about the **trip
 * count not being knowable** - and that number is what the branched loop's guard is made of, so
 * a loop without one cannot be bounded and is the case that would hang the part. */
static void test_gl2_the_back_end_refuses_the_loops_it_cannot_bound(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};

    static const struct { const char *fs; const char *wants; } cases[] = {
        /* A bound that is not known when the shader is compiled. **This is the one the guard
         * cannot be built for**, and so the one that would genuinely hang. */
        {"uniform float lim;\n"
         "void main() {\n"
         "  float t = 0.0;\n"
         "  for (int i = 0; float(i) < lim; i++) { t += 1.0; }\n"
         "  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);\n"
         "}\n",
         "constant"},
        /* More trips than the generator will put a ceiling on. Branching does not make this one
         * safe: a guard has to hold a number, and past some size the number stops being a
         * bound worth having. */
        {"void main() {\n"
         "  float t = 0.0;\n"
         "  for (int i = 0; i < 100000; i++) { t += 1.0; }\n"
         "  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);\n"
         "}\n",
         "bound"},
        /* A body that moves its own counter. The trip count is worked out at compile time and
         * this makes it wrong - silently, in both lowerings, which is why it is refused in
         * neither one of them but before the choice between them. */
        {"void main() {\n"
         "  float t = 0.0;\n"
         "  for (int i = 0; i < 4; i++) { t += 1.0; i = i + 2; }\n"
         "  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);\n"
         "}\n",
         "assigns its own counter"},
        /* **And the loop that is named is the one at fault.** The clean loop on line 3
         * compiles; the one on line 4 does not, so the message begins "4:". A check that swept
         * the whole shader rather than this loop's own body refuses line 3 first and reports
         * that - a true sentence about the wrong loop, which is worse than no sentence. */
        {"void main() {\n"
         "  float t = 0.0;\n"
         "  for (int i = 0; i < 2; i++) { t += 1.0; }\n"
         "  for (int j = 0; j < 2; j++) { t += 1.0; j = j - 1; }\n"
         "  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);\n"
         "}\n",
         "4:"},
        /* Branched loops nested deeper than the scalar registers set aside for their masks.
         * Each of these three has a `break`, so each one branches; three loops that unrolled
         * would cost nothing here at all. */
        {"void main() {\n"
         "  float t = 0.0;\n"
         "  for (int i = 0; i < 2; i++) { if (t > 9.0) break;\n"
         "    for (int j = 0; j < 2; j++) { if (t > 9.0) break;\n"
         "      for (int k = 0; k < 2; k++) { if (t > 9.0) break; t += 1.0; } } }\n"
         "  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);\n"
         "}\n",
         "nest deeper"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const GLuint prog = linked_program(VS_ONE_VARYING, cases[i].fs);
        log[0] = '\0';
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, prog), words, 256u, &count,
                                              &vgprs, NULL, NULL, log, sizeof(log)),
                  GL_FALSE);
        if (strstr(log, cases[i].wants) == NULL) {
            printf("\n    case %d: expected a message about '%s', got '%s'\n", (int)i,
                   cases[i].wants, log);
        }
        ASSERT_TRUE(strstr(log, cases[i].wants) != NULL);
    }

    glContextDestroy(ctx);
}

static void test_gl2_derivatives_are_quad_reads_under_whole_quad_mode(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u, ena = 0u, usg = 0u;
    char log[256] = {0};

    /* **The permutes are the whole of a derivative**, and the four of them differ only in that
     * byte - so the test names the byte. `dFdx` takes the right-hand column less the left,
     * `dFdy` the bottom row less the top; using an x permute for `dFdy` gives a slope along
     * the wrong axis and nothing else changes. */
    const GLuint dx = linked_program(
        VS_ONE_VARYING,
        "varying vec4 vin;\n"
        "void main() { gl_FragColor = vec4(dFdx(vin.x), 0.0, 0.0, 1.0); }\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, dx), words, 256u, &count, &vgprs,
                                          &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    {
        GLboolean far_x = GL_FALSE, near_x = GL_FALSE, any_y = GL_FALSE, wqm = GL_FALSE;
        for (uint32_t i = 0; i + 1 < count; i++) {
            if ((words[i] & 0x1ffu) != 0xfau) continue;
            const uint32_t ctrl = (words[i + 1] >> 8) & 0xffu;
            if (ctrl == GLSL_DPP_QUAD_X_FAR) far_x = GL_TRUE;
            if (ctrl == GLSL_DPP_QUAD_X_NEAR) near_x = GL_TRUE;
            if (ctrl == GLSL_DPP_QUAD_Y_FAR || ctrl == GLSL_DPP_QUAD_Y_NEAR) any_y = GL_TRUE;
        }
        /* `s_wqm_b32 exec_lo, exec_lo` - the mode a quad read needs, because the lane next door
         * may be one the primitive does not cover and would otherwise not be running. */
        for (uint32_t i = 0; i < count; i++) {
            if (words[i] == 0xbefe097eu) wqm = GL_TRUE;
        }
        ASSERT_TRUE(far_x);
        ASSERT_TRUE(near_x);
        ASSERT_TRUE(!any_y);  /* dFdx reaches for no row permute */
        ASSERT_TRUE(wqm);
    }

    /* `dFdy` is the same shape on the other axis - and asserts the x permutes are absent, so a
     * lowering that used one set for both fails one of the two arms. */
    const GLuint dy = linked_program(
        VS_ONE_VARYING,
        "varying vec4 vin;\n"
        "void main() { gl_FragColor = vec4(dFdy(vin.x), 0.0, 0.0, 1.0); }\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, dy), words, 256u, &count, &vgprs,
                                          &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    {
        GLboolean far_y = GL_FALSE, near_y = GL_FALSE, any_x = GL_FALSE;
        for (uint32_t i = 0; i + 1 < count; i++) {
            if ((words[i] & 0x1ffu) != 0xfau) continue;
            const uint32_t ctrl = (words[i + 1] >> 8) & 0xffu;
            if (ctrl == GLSL_DPP_QUAD_Y_FAR) far_y = GL_TRUE;
            if (ctrl == GLSL_DPP_QUAD_Y_NEAR) near_y = GL_TRUE;
            if (ctrl == GLSL_DPP_QUAD_X_FAR || ctrl == GLSL_DPP_QUAD_X_NEAR) any_x = GL_TRUE;
        }
        ASSERT_TRUE(far_y);
        ASSERT_TRUE(near_y);
        ASSERT_TRUE(!any_x);
    }

    /* `fwidth` is both, so all four permutes appear in the one shader. */
    const GLuint fw = linked_program(
        VS_ONE_VARYING,
        "varying vec4 vin;\n"
        "void main() { gl_FragColor = vec4(fwidth(vin.x), 0.0, 0.0, 1.0); }\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, fw), words, 256u, &count, &vgprs,
                                          &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    {
        int seen = 0;
        for (uint32_t i = 0; i + 1 < count; i++) {
            if ((words[i] & 0x1ffu) != 0xfau) continue;
            const uint32_t ctrl = (words[i + 1] >> 8) & 0xffu;
            if (ctrl == GLSL_DPP_QUAD_X_FAR || ctrl == GLSL_DPP_QUAD_X_NEAR ||
                ctrl == GLSL_DPP_QUAD_Y_FAR || ctrl == GLSL_DPP_QUAD_Y_NEAR) {
                seen++;
            }
        }
        ASSERT_TRUE(seen >= 4);
    }

    /* A shader that names none of them does not enter whole-quad mode and pays nothing. */
    const GLuint plain = linked_program(
        VS_ONE_VARYING, "void main() { gl_FragColor = vec4(1.0); }\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, plain), words, 256u, &count,
                                          &vgprs, &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    for (uint32_t i = 0; i < count; i++) ASSERT_TRUE(words[i] != 0xbefe097eu);

    glContextDestroy(ctx);
}

static void test_gl2_frag_depth_exports_before_the_colour(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u, ena = 0u, usg = 0u;
    char log[256] = {0};

    const GLuint d = linked_program(
        VS_ONE_VARYING,
        "void main() {\n"
        "  gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
        "  gl_FragDepth = 0.99;\n"
        "}\n");
    const gl_program_object_t *pd = gl_find_program(c, d);
    ASSERT_TRUE(pd != NULL);
    ASSERT_EQ(pd->hw_ps_exports_depth, GL_TRUE);
    ASSERT_EQ(gl_program_compile_fragment(pd, words, 256u, &count, &vgprs, &usg, &ena, log,
                                          sizeof(log)),
              GL_TRUE);
    /* It asks for the window position, because the depth it starts from is the interpolated z. */
    ASSERT_EQ(ena & 0x00000f00u, 0x00000f00u);

    /* **Order and `done` are the whole of what can go wrong here.** The depth export comes
     * first and does not claim to be last; the colour export comes second and does. A shader
     * with two `done` exports, or with the colour first, does not retire - and nothing on the
     * host would show it. */
    {
        int z_at = -1, c_at = -1;
        for (uint32_t i = 0; i + 1 < count; i++) {
            if ((words[i] >> 26) != 0x3eu) continue;
            const uint32_t target = (words[i] >> 4) & 0x3fu;
            if (target == 8u) z_at = (int)i;
            if (target == 0u) c_at = (int)i;
        }
        ASSERT_TRUE(z_at >= 0);
        ASSERT_TRUE(c_at >= 0);
        ASSERT_TRUE(z_at < c_at);
        ASSERT_EQ((words[z_at] >> 11) & 1u, 0u); /* the depth does not say done */
        ASSERT_EQ((words[c_at] >> 11) & 1u, 1u); /* the colour does */
    }

    /* A shader that never names it exports no depth and is charged no register for one. */
    const GLuint plain = linked_program(
        VS_ONE_VARYING, "void main() { gl_FragColor = vec4(1.0); }\n");
    const gl_program_object_t *pp = gl_find_program(c, plain);
    ASSERT_TRUE(pp != NULL);
    ASSERT_EQ(pp->hw_ps_exports_depth, GL_FALSE);
    ASSERT_EQ(gl_program_compile_fragment(pp, words, 256u, &count, &vgprs, &usg, &ena, log,
                                          sizeof(log)),
              GL_TRUE);
    for (uint32_t i = 0; i < count; i++) {
        if ((words[i] >> 26) == 0x3eu) ASSERT_TRUE(((words[i] >> 4) & 0x3fu) != 8u);
    }

    glContextDestroy(ctx);
}

static void test_gl2_vector_relationals_reduce_a_bvec(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* **`any` and `all` are a max and a min**, which is only true because a bvec component is
     * exactly 0.0 or 1.0. `lessThan((0,1,2), (1,1,1))` is `(true, false, false)`, so `any` is
     * true, `all` is false, and `all(not(c))` is false as well - three different reductions of
     * one comparison, and a back end that confused the two reductions gets the middle one
     * wrong while the first still looks right. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  bvec3 c = lessThan(vec3(0.0, 1.0, 2.0), vec3(1.0, 1.0, 1.0));\n"
                    "  gl_FragColor = vec4(any(c) ? 1.0 : 0.0, all(c) ? 1.0 : 0.0,\n"
                    "                      all(not(c)) ? 1.0 : 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f); /* any: the first component is true */
    ASSERT_NEAR(o[1], 0.0f, 1e-6f); /* all: the other two are not */
    ASSERT_NEAR(o[2], 0.0f, 1e-6f); /* all(not): the first is false once inverted */

    /* `not` on its own, and a comparison whose answer differs per component - so a lowering
     * that compared once and broadcast gives the same value three times and fails. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  bvec3 c = greaterThan(vec3(2.0, 0.0, 3.0), vec3(1.0, 1.0, 1.0));\n"
                    "  bvec3 n = not(c);\n"
                    "  gl_FragColor = vec4(n.x ? 1.0 : 0.0, n.y ? 1.0 : 0.0,\n"
                    "                      n.z ? 1.0 : 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, 1e-6f); /* 2 > 1, so not is false */
    ASSERT_NEAR(o[1], 1.0f, 1e-6f); /* 0 > 1 is false, so not is true */
    ASSERT_NEAR(o[2], 0.0f, 1e-6f);

    glContextDestroy(ctx);
}

static void test_gl2_gl_color_lands_where_the_link_put_it(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;

    /* **With a vertex shader, the user's varyings own the parameters and the colour follows
     * them.** `VS_ONE_VARYING` carries one `vec4`, which is parameter 0 - so `gl_Color` is
     * parameter 1, and the parameter count grows to carry it. */
    const GLuint withvs = linked_program(
        VS_ONE_VARYING,
        "varying vec4 vin;\n"
        "void main() { gl_FragColor = gl_Color * vin.x; }\n");
    const gl_program_object_t *pv = gl_find_program(c, withvs);
    ASSERT_TRUE(pv != NULL);
    ASSERT_EQ(pv->hw_color_param, 1);
    ASSERT_TRUE(pv->hw_params >= 2u);

    /* **Without one, the fixed-function vertex path runs and has always written the colour into
     * parameter 0** - so the slot already exists and nothing is added. A back end that used one
     * number for both cases would read the user's first varying as the colour in one of them. */
    const GLuint fsonly = glCreateProgram();
    {
        const GLchar *src[1] = {
            "void main() { gl_FragColor = vec4(gl_Color.rgb * 0.5, 1.0); }\n"};
        const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, src, NULL);
        glCompileShader(fs);
        glAttachShader(fsonly, fs);
        glLinkProgram(fsonly);
        GLint linked = 0;
        glGetProgramiv(fsonly, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_TRUE);
    }
    const gl_program_object_t *pn = gl_find_program(c, fsonly);
    ASSERT_TRUE(pn != NULL);
    ASSERT_EQ(pn->hw_color_param, 0);

    /* And a shader that never names it is charged nothing. */
    const GLuint plain = linked_program(VS_ONE_VARYING,
                                        "varying vec4 vin;\n"
                                        "void main() { gl_FragColor = vin; }\n");
    const gl_program_object_t *pp = gl_find_program(c, plain);
    ASSERT_TRUE(pp != NULL);
    ASSERT_EQ(pp->hw_color_param, -1);

    /* The interpolation reads the parameter the link chose. `attr[1]` is the colour for the
     * first program, so a prologue reading parameter 0 would return the varying instead - and
     * the two are deliberately different values here. */
    float o[4];
    const float attr[4][4] = {{0.5f, 0, 0, 0},      /* param0: the user varying */
                              {0.25f, 0.75f, 1.0f, 1.0f}, /* param1: gl_Color */
                              {0, 0, 0, 0},
                              {0, 0, 0, 0}};
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() { gl_FragColor = gl_Color; }\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, 1e-6f);
    ASSERT_NEAR(o[1], 0.75f, 1e-6f);
    ASSERT_NEAR(o[2], 1.0f, 1e-6f);

    glContextDestroy(ctx);
}

static void test_gl2_matrix_products_are_two_products(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* **`m * v` and `v * m` are different answers.** The second is the product with the
     * transpose, and a matrix with one off-diagonal term is what separates them: with
     * col0 = (1,2) and col1 = (0,1),
     *
     *   m * (1,0) = col0          = (1, 2)
     *   (1,0) * m = (v.col0, v.col1) = (1, 0)
     *
     * so the `y` of the two differs, 2 against 0. A back end that folded the two together
     * would give the same pair twice and would be right for a symmetric matrix - which is
     * exactly the matrix a careless test uses. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  mat2 m = mat2(1.0, 2.0, 0.0, 1.0);\n"
                    "  vec2 a = m * vec2(1.0, 0.0);\n"
                    "  vec2 b = vec2(1.0, 0.0) * m;\n"
                    "  gl_FragColor = vec4(a.x, a.y, b.x, b.y);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f);
    ASSERT_NEAR(o[1], 2.0f, 1e-6f); /* m * v picks the column */
    ASSERT_NEAR(o[2], 1.0f, 1e-6f);
    ASSERT_NEAR(o[3], 0.0f, 1e-6f); /* v * m dots with the columns */

    /* `mat2(0.5)` is a diagonal and not four halves - the constructor people get wrong. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  mat2 m = mat2(0.5);\n"
                    "  vec2 v = m * vec2(1.0, 1.0);\n"
                    "  gl_FragColor = vec4(v, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-6f); /* not 1.0, which four halves would give */
    ASSERT_NEAR(o[1], 0.5f, 1e-6f);

    /* Three dimensions, so the column stride is exercised at more than one value: a walk that
     * hard-coded four would read past the end of a mat3 and a walk that hard-coded two would
     * stop short. col0 = (1,0,0), col1 = (0,2,0), col2 = (3,0,4). */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  mat3 m = mat3(1.0, 0.0, 0.0,  0.0, 2.0, 0.0,  3.0, 0.0, 4.0);\n"
                    "  vec3 a = m * vec3(1.0, 1.0, 1.0);\n"
                    "  gl_FragColor = vec4(a, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 4.0f, 1e-6f); /* 1 + 0 + 3 */
    ASSERT_NEAR(o[1], 2.0f, 1e-6f); /* 0 + 2 + 0 */
    ASSERT_NEAR(o[2], 4.0f, 1e-6f); /* 0 + 0 + 4 */

    glContextDestroy(ctx);
}

static void test_gl2_integers_are_floats_kept_whole(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* **The representation is the reference's**: an int is a float and every integer operation
     * is followed by a truncation towards zero, which is what `glsl_exec.c` writes as
     * `(float)(int)x`. These are exact in a float, so what the arm proves is that the values
     * arrive at all - int locals are generated now, where they were refused. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  int a = 7;\n"
                    "  int b = 3;\n"
                    "  gl_FragColor = vec4(float(a + b), float(a - b), float(a * b), 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 10.0f, 1e-6f);
    ASSERT_NEAR(o[1], 4.0f, 1e-6f);
    ASSERT_NEAR(o[2], 21.0f, 1e-6f);

    /* **Where truncation is the whole answer.** `int(7.9)` is 7 and `int(-7.9)` is -7 - toward
     * zero, not toward minus infinity - so a lowering that reached for `floor` gets the second
     * one wrong and only the second one. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  gl_FragColor = vec4(float(int(7.9)), float(int(-7.9)),\n"
                    "                      float(int(2.5) * int(3.5)), 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 7.0f, 1e-6f);
    ASSERT_NEAR(o[1], -7.0f, 1e-6f);
    ASSERT_NEAR(o[2], 6.0f, 1e-6f); /* 2 * 3, not 8.75 */

    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t w2[256];
    uint32_t n2 = 0u, vg = 0u;
    char lg[256] = {0};

    /* **The truncation after an integer operation is asserted on the instructions, not on the
     * value**, because on these values it changes nothing: addition, subtraction and
     * multiplication of whole floats are whole already. It is emitted so the representation
     * holds by construction rather than by luck, and the only way to see it is to look. The
     * float arm below is the control - the same expression with a float type emits none. */
    {
        const GLuint ip = linked_program(
            VS_ONE_VARYING,
            "void main() { int a = 7; int b = 3; gl_FragColor = vec4(float(a * b)); }\n");
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, ip), w2, 256u, &n2, &vg, NULL,
                                              NULL, lg, sizeof(lg)),
                  GL_TRUE);
        int truncs = 0;
        for (uint32_t i = 0; i < n2; i++) {
            if ((w2[i] >> 25) == 0x3fu && ((w2[i] >> 9) & 0xffu) == GLSL_VOP1_TRUNC_F32) {
                truncs++;
            }
        }
        ASSERT_TRUE(truncs > 0);

        const GLuint fp = linked_program(
            VS_ONE_VARYING,
            "void main() { float a = 7.0; float b = 3.0; gl_FragColor = vec4(a * b); }\n");
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, fp), w2, 256u, &n2, &vg, NULL,
                                              NULL, lg, sizeof(lg)),
                  GL_TRUE);
        int float_truncs = 0;
        for (uint32_t i = 0; i < n2; i++) {
            if ((w2[i] >> 25) == 0x3fu && ((w2[i] >> 9) & 0xffu) == GLSL_VOP1_TRUNC_F32) {
                float_truncs++;
            }
        }
        ASSERT_EQ(float_truncs, 0);
    }

    /* **Integer division, against a reciprocal that is deliberately a shade low.** The
     * simulator models `v_rcp_f32` as one unit in the last place below the true value, because
     * that is the part's accuracy and a host divide's is better - so `a * rcp(a)` lands under
     * 1.0 and an uncorrected `trunc` gives zero. Every exact case below is one a naive
     * lowering gets wrong by one. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  gl_FragColor = vec4(float(7 / 7), float(49 / 7),\n"
                    "                      float(100 / 10), float(6 / 3)) * 0.01;\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.01f, 1e-6f);  /* 1, not 0 */
    ASSERT_NEAR(o[1], 0.07f, 1e-6f);  /* 7, not 6 */
    ASSERT_NEAR(o[2], 0.10f, 1e-6f);
    ASSERT_NEAR(o[3], 0.02f, 1e-6f);

    /* **Truncation is toward zero on both signs**, which is C's rule and GLSL's. A lowering
     * that took the floor gets every negative quotient wrong by one, and only the negative
     * ones - so the positive arm above would still look right. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  gl_FragColor = vec4(float(7 / 2), float(-7 / 2),\n"
                    "                      float(7 / -2), float(-7 / -2)) * 0.1;\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.3f, 1e-6f);   /* 3 */
    ASSERT_NEAR(o[1], -0.3f, 1e-6f);  /* -3, not -4 */
    ASSERT_NEAR(o[2], -0.3f, 1e-6f);
    ASSERT_NEAR(o[3], 0.3f, 1e-6f);

    /* **A large quotient, because the one correction has a range.** The reciprocal's relative
     * error is about 2^-23, so the truncated quotient is out by `q * 2^-23` - under one for any
     * `q` below roughly eight million, which is where a single correction is enough. Above
     * that the integers themselves stop being exactly representable in a float, so the
     * representation runs out before the correction does. This pins the working range rather
     * than an edge nobody reaches: 999999 / 3 is 333333 exactly. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  gl_FragColor = vec4(float(999999 / 3) * 0.000001,\n"
                    "                      float(1000000 / 8) * 0.000001,\n"
                    "                      float(-999999 / 3) * 0.000001, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.333333f, 1e-5f);
    ASSERT_NEAR(o[1], 0.125f, 1e-5f);
    ASSERT_NEAR(o[2], -0.333333f, 1e-5f);

    /* Division by zero answers zero, which is what the reference answers. The language calls it
     * undefined; the two paths still have to agree on something. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  int z = 0;\n"
                    "  gl_FragColor = vec4(float(5 / z), 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, 1e-6f);

    glContextDestroy(ctx);
}

static void test_gl2_front_facing_is_a_sign_not_a_flag(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u, ena = 0u, usg = 0u;
    char log[256] = {0};

    /* **The register is a float whose sign is the answer**, not a zero-or-one flag: Mesa lowers
     * `load_front_face` as `fgt(reg, 0)` and `load_front_face_fsign` as the register itself. So
     * the prologue compares and selects, and what this pins is that it compares at all - a back
     * end that moved the register straight into a bool would answer "front" for a negative
     * number, which is every back-facing fragment. */
    const GLuint ff = linked_program(
        VS_ONE_VARYING,
        "void main() {\n"
        "  gl_FragColor = gl_FrontFacing ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);\n"
        "}\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, ff), words, 256u, &count, &vgprs,
                                          &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    /* PERSP_CENTER and FRONT_FACE, and **not** the window position: asking for four registers
     * of `gl_FragCoord` that this shader never reads would cost the stage its allocation for
     * nothing. */
    ASSERT_EQ(ena, 0x00001002u);
    /* No block either - the face arrives in a register, not through the payload. */
    ASSERT_EQ(usg, 0u);
    {
        /* Somewhere in the prologue there is a float compare against zero. Its opcode is
         * `GT_F32`, and it is what makes the sign the answer. */
        GLboolean saw_cmp = GL_FALSE;
        for (uint32_t i = 0; i < count; i++) {
            if ((words[i] >> 25) == 0x3eu && ((words[i] >> 17) & 0xffu) == GLSL_VOPC_GT_F32 &&
                (words[i] & 0x1ffu) == 256u + 2u) { /* v2: straight after the barycentrics */
                saw_cmp = GL_TRUE;
                break;
            }
        }
        ASSERT_TRUE(saw_cmp);
    }

    /* **Both together move the face register**, because the SPI packs what it was asked for in
     * order: with the position enabled the face follows it, and the compare has to read the
     * later register. A back end that fixed the face at one number would compare the window's
     * w against zero here and answer "front" for every fragment in front of the eye. */
    const GLuint both = linked_program(
        VS_ONE_VARYING,
        "void main() {\n"
        "  float d = gl_FrontFacing ? 1.0 : 0.0;\n"
        "  gl_FragColor = vec4(d, gl_FragCoord.y, 0.0, 1.0);\n"
        "}\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, both), words, 256u, &count, &vgprs,
                                          &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    ASSERT_EQ(ena, 0x00001f02u); /* PERSP_CENTER | POS_XYZW | FRONT_FACE */
    ASSERT_EQ(usg, 2u);          /* and the block, for the height that flips y */
    {
        /* v6, not v2: two barycentrics, then x, y, z, w, then the face. The face is the
         * compare's **src0** - the nine-bit operand, so a VGPR reads as 256 + its number - and
         * `vsrc1` is the register holding zero. */
        GLboolean saw_v6 = GL_FALSE;
        for (uint32_t i = 0; i < count; i++) {
            if ((words[i] >> 25) == 0x3eu && ((words[i] >> 17) & 0xffu) == GLSL_VOPC_GT_F32 &&
                (words[i] & 0x1ffu) == 256u + 6u) {
                saw_v6 = GL_TRUE;
                break;
            }
        }
        ASSERT_TRUE(saw_v6);
    }

    /* **All three at once**, which is the combination the packing makes fragile: `gl_FragDepth`
     * asks for the window position as well, so a shader naming it and the face - but not
     * `gl_FragCoord` - still has the face at v6 rather than v2. A back end that keyed the
     * offset on `gl_FragCoord` alone reads the position's w as the face here, which is positive
     * for everything in front of the eye and so answers "front" for every fragment.
     *
     * It is also run below, so the simulator's liveness check sees it. */
    {
        const GLuint both2 = linked_program(
            VS_ONE_VARYING,
            "void main() {\n"
            "  gl_FragDepth = 0.5;\n"
            "  gl_FragColor = vec4(gl_FrontFacing ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n"
            "}\n");
        uint32_t w3[256];
        uint32_t n3 = 0u, v3 = 0u, e3 = 0u, u3 = 0u;
        char l3[256] = {0};
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, both2), w3, 256u, &n3, &v3,
                                              &u3, &e3, l3, sizeof(l3)),
                  GL_TRUE);
        ASSERT_EQ(e3, 0x00001f02u); /* PERSP_CENTER | POS_XYZW | FRONT_FACE */
        GLboolean face_at_v6 = GL_FALSE;
        for (uint32_t i = 0; i < n3; i++) {
            if ((w3[i] >> 25) == 0x3eu && ((w3[i] >> 17) & 0xffu) == GLSL_VOPC_GT_F32 &&
                (w3[i] & 0x1ffu) == 256u + 6u) {
                face_at_v6 = GL_TRUE;
            }
        }
        ASSERT_TRUE(face_at_v6);
    }

    /* **And run, not only inspected.** The simulator fills exactly the registers
     * `input_ena` asked for and refuses to read any other, so this arm fails if the prologue
     * reaches for the face at the wrong number - which is the failure the packing invites,
     * since enabling `gl_FragCoord` moves it. Both shapes are run for that reason. */
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  gl_FragColor = vec4(gl_FrontFacing ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f); /* the seeded face is positive, so front */

    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  gl_FragColor = vec4(gl_FrontFacing ? 1.0 : 0.0, gl_FragCoord.y,\n"
                    "                      gl_FragCoord.x, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f);
    ASSERT_NEAR(o[1], SIM_TARGET_H - SIM_FRAG_Y, 1e-6f);
    ASSERT_NEAR(o[2], SIM_FRAG_X, 1e-6f);

    /* The face beside `gl_FragDepth` and without `gl_FragCoord` - the shape above, run, so the
     * simulator's liveness check sees whether anything reads a register the SPI never filled. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  gl_FragDepth = 0.5;\n"
                    "  gl_FragColor = vec4(gl_FrontFacing ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f);

    glContextDestroy(ctx);
}

static void test_gl2_user_functions_are_inlined(void) {
    void *ctx = gl2_context();
    float o[4];
    const float x = 0.75f, y = 0.25f, z = 3.0f, w = 2.0f;
    const float attr[4][4] = {{x, y, z, w}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* **Arguments bind by position, not by name.** `sub(a, b)` and `sub(b, a)` differ only in
     * order, so a generator that bound them by name - or that evaluated the parameters in the
     * callee's scope, where `a` and `b` mean something else - returns the same value for both
     * and this fails on the second. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "float sub(float a, float b) { return a - b; }\n"
                    "void main() {\n"
                    "  gl_FragColor = vec4(sub(vin.x, vin.y), sub(vin.y, vin.x), 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], x - y, 1e-6f);
    ASSERT_NEAR(o[1], y - x, 1e-6f);

    /* **A parameter shadows a caller's variable of the same name and gives it back.** The
     * argument is evaluated before the parameter is bound, so `f(a)` where the parameter is
     * also `a` passes the caller's - and after the call the caller's `a` is untouched. A
     * generator that bound first would pass the parameter's own uninitialised register. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "float twice(float a) { a = a * 2.0; return a; }\n"
                    "void main() {\n"
                    "  float a = vin.x;\n"
                    "  float t = twice(a);\n"
                    "  gl_FragColor = vec4(t, a, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], x * 2.0f, 1e-6f);
    ASSERT_NEAR(o[1], x, 1e-6f); /* the caller's `a`, not the parameter's */

    /* Nesting, locals inside a body, and a vector return - one call feeding another, so a
     * result register reused across the two would show up here. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "float add(float a, float b) { float s = a + b; return s; }\n"
                    "vec3 scale(vec3 v, float k) { return v * k; }\n"
                    "void main() {\n"
                    "  gl_FragColor = vec4(scale(vec3(vin.x, vin.y, add(vin.z, vin.w)), 2.0),\n"
                    "                      1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], x * 2.0f, 1e-6f);
    ASSERT_NEAR(o[1], y * 2.0f, 1e-6f);
    ASSERT_NEAR(o[2], (z + w) * 2.0f, 1e-6f);

    /* **`inout` is pass-by-value-and-copy-back, not pass-by-reference.** The distinction is the
     * language's and it is visible: with references `swap(p, p)` aliases and leaves `p` alone;
     * with copies it writes `p` twice and the second write wins. The swap below is the ordinary
     * case, and the aliased call after it is the one that tells the two apart. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void swap(inout float a, inout float b) { float t = a; a = b; b = t; }\n"
                    "void main() {\n"
                    "  float p = 1.0;\n"
                    "  float q = 0.0;\n"
                    "  swap(p, q);\n"
                    "  gl_FragColor = vec4(p, q, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, 1e-6f);
    ASSERT_NEAR(o[1], 1.0f, 1e-6f);

    /* An `out` parameter, and a void function called as a statement - which produces no value,
     * and is the one place a result of no width is the expected outcome rather than a failure. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void twice(float a, out float r) { r = a + a; }\n"
                    "void main() {\n"
                    "  float d = 0.0;\n"
                    "  twice(vin.x, d);\n"
                    "  gl_FragColor = vec4(d, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], x + x, 1e-6f);

    /* **A call gives its registers back.** An inlined call's parameters and body temporaries are
     * live only while the body is being generated; keeping them costs one copy of the helper's
     * locals per call site, and a shader with a handful of calls is then refused for a budget it
     * never needed at any one moment. Ten calls to a helper with three locals is the shape - it
     * stays near one call's cost, and without the release it climbs past ten times it. */
    {
        gl_context_t *cc = (gl_context_t *)ctx;
        const GLuint many = linked_program(
            VS_ONE_VARYING,
            "varying vec4 vin;\n"
            "float h(float a) { float b = a * 2.0; float c = b + 1.0; return c * 0.5; }\n"
            "void main() {\n"
            "  float t = h(vin.x) + h(vin.y) + h(vin.z) + h(vin.w) + h(vin.x)\n"
            "          + h(vin.y) + h(vin.z) + h(vin.w) + h(vin.x) + h(vin.y);\n"
            "  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);\n"
            "}\n");
        uint32_t wm[512];
        uint32_t nm = 0u, vm = 0u;
        char lm[256] = {0};
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(cc, many), wm, 512u, &nm, &vm,
                                              NULL, NULL, lm, sizeof(lm)),
                  GL_TRUE);
        /* **Measured, not guessed: 50 with the release and 77 without it.** The remainder is
         * not the calls - it is the ten results and the sum's own temporaries, which are all
         * genuinely live at once - so this pins the improvement rather than an ideal. A
         * threshold between the two catches the release going away again; 136 is what the
         * stage allocates, and both numbers are under it, so the release is register pressure
         * and not a shader that would have been refused. */
        ASSERT_TRUE(vm < 64u);
    }

    /* **Not tested here: a user function that hides a built-in of the same name.** GLSL 1.10
     * allows it and the generator would inline the user's, because it looks for a definition in
     * this shader before it reaches the built-in table. The front end does not get that far -
     * `float min(float, float)` fails to compile, ahead of any of this - so the case cannot
     * reach the back end and a test of it here would be testing the front end by proxy. */

    glContextDestroy(ctx);
}

/* **Matrix arithmetic, where a wrong answer is still a matrix.**
 *
 * Every value below is chosen so that the transposed result, the componentwise result and the
 * product are three different numbers. A matrix test built on symmetric operands passes with
 * the rows and columns swapped, which is the one mistake column-major storage invites.
 */
static void test_gl2_matrix_by_matrix_and_by_scalar(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* **`m * m` is a product and not componentwise.** `a` is a shear and `b` a scale, so
     * `a * b` and `b * a` differ - which is what says the columns were walked in the right
     * order rather than merely all multiplied.
     *
     * Column-major: `mat2(1, 2, 0, 1)` is col0 = (1,2), col1 = (0,1).
     *   a = [[1,0],[2,1]] as (row, col);  b = [[3,0],[0,4]]
     *   a*b: col0 = a * (3,0) = (3, 6);   col1 = a * (0,4) = (0, 4)
     *   b*a: col0 = b * (1,2) = (3, 8);   col1 = b * (0,1) = (0, 4)
     * The (1,0) element is 6 one way and 8 the other. Componentwise would give 3. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  mat2 a = mat2(1.0, 2.0, 0.0, 1.0);\n"
                    "  mat2 b = mat2(3.0, 0.0, 0.0, 4.0);\n"
                    "  mat2 ab = a * b;\n"
                    "  mat2 ba = b * a;\n"
                    "  gl_FragColor = vec4(ab[0][1] * 0.1, ba[0][1] * 0.1, ab[1][1] * 0.1, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.6f, 1e-6f); /* a*b at (1,0) */
    ASSERT_NEAR(o[1], 0.8f, 1e-6f); /* b*a at (1,0) - the other product */
    ASSERT_NEAR(o[2], 0.4f, 1e-6f);

    /* `mat3 * mat3`, so the size is not baked in anywhere. The identity times anything is that
     * thing, and a scale down the diagonal multiplies each column by its own factor. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  mat3 s = mat3(2.0, 0.0, 0.0,  0.0, 3.0, 0.0,  0.0, 0.0, 4.0);\n"
                    "  mat3 t = mat3(1.0, 1.0, 1.0,  0.0, 1.0, 0.0,  0.0, 0.0, 1.0);\n"
                    "  mat3 st = s * t;\n"
                    "  gl_FragColor = vec4(st[0][0] * 0.1, st[0][1] * 0.1, st[0][2] * 0.1, 1.0);\n"
                    "}\n",
                    attr, o);
    /* col0 of t is (1,1,1); s * that is (2,3,4). */
    ASSERT_NEAR(o[0], 0.2f, 1e-6f);
    ASSERT_NEAR(o[1], 0.3f, 1e-6f);
    ASSERT_NEAR(o[2], 0.4f, 1e-6f);

    /* **A matrix with a scalar is componentwise and broadcast**, both ways round, and a matrix
     * with a matrix under `+` is componentwise too - which is why `*` had to be special. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  mat2 a = mat2(1.0, 2.0, 3.0, 4.0);\n"
                    "  mat2 h = a * 0.5;\n"
                    "  mat2 k = 2.0 * a;\n"
                    "  mat2 s = a + a;\n"
                    "  gl_FragColor = vec4(h[1][1] * 0.1, k[0][1] * 0.1, s[1][0] * 0.1, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.2f, 1e-6f); /* 4 * 0.5 */
    ASSERT_NEAR(o[1], 0.4f, 1e-6f); /* 2 * 2 */
    ASSERT_NEAR(o[2], 0.6f, 1e-6f); /* 3 + 3 */

    /* `matrixCompMult` is the componentwise product GLSL spells out precisely because `*` does
     * not mean it - so it must differ from `a * b` on the same operands. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  mat2 a = mat2(1.0, 2.0, 0.0, 1.0);\n"
                    "  mat2 b = mat2(3.0, 0.0, 0.0, 4.0);\n"
                    "  mat2 c = matrixCompMult(a, b);\n"
                    "  mat2 p = a * b;\n"
                    "  gl_FragColor = vec4(c[0][1] * 0.1, p[0][1] * 0.1, c[0][0] * 0.1, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, 1e-6f); /* 2 * 0 componentwise */
    ASSERT_NEAR(o[1], 0.6f, 1e-6f); /* 6 as a product */
    ASSERT_NEAR(o[2], 0.3f, 1e-6f); /* 1 * 3 */

    /* `transpose` swaps the indices, so an asymmetric matrix is the only useful witness.
     * **1.20, which is where the language put it** - the front end gates it and is right to. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "#version 120\n"
                    "void main() {\n"
                    "  mat2 a = mat2(1.0, 2.0, 3.0, 4.0);\n"
                    "  mat2 t = transpose(a);\n"
                    "  gl_FragColor = vec4(t[0][1] * 0.1, t[1][0] * 0.1, t[0][0] * 0.1, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.3f, 1e-6f); /* was a[1][0] */
    ASSERT_NEAR(o[1], 0.2f, 1e-6f); /* was a[0][1] */
    ASSERT_NEAR(o[2], 0.1f, 1e-6f); /* the diagonal does not move */

    /* `outerProduct(c, r)` puts `c` down the columns; the other order is the transpose of this
     * and would still be a matrix, so the two off-diagonal elements are the test. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "#version 120\n"
                    "void main() {\n"
                    "  mat2 m = outerProduct(vec2(1.0, 2.0), vec2(3.0, 5.0));\n"
                    "  gl_FragColor = vec4(m[0][1] * 0.1, m[1][0] * 0.1, m[1][1] * 0.1, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.6f, 1e-6f); /* (col 0, row 1) = c[1] * r[0] = 2 * 3 */
    ASSERT_NEAR(o[1], 0.5f, 1e-6f); /* (col 1, row 0) = c[0] * r[1] = 1 * 5 */
    ASSERT_NEAR(o[2], 1.0f, 1e-6f); /* 2 * 5 */

    glContextDestroy(ctx);
}

/* **A local array is a run of registers**, indexed where the shader is compiled.
 *
 * There is no addressable memory behind one, so the index has to be known at compile time. The
 * case that makes that worth having rather than merely legal is an unrolled loop: its counter
 * holds a different constant in each copy of the body, so `w[i]` resolves element by element -
 * which is how a shader actually writes this.
 */
static void test_gl2_local_arrays_are_indexed_where_the_shader_is_compiled(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Written by literal index, read by literal index, and the elements must not overlap -
     * 0.1, 0.2, 0.3, 0.4 into four slots, read back out of order so a stride mistake shows as
     * a different number rather than the same one twice. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float w[4];\n"
                    "  w[0] = 0.1; w[1] = 0.2; w[2] = 0.3; w[3] = 0.4;\n"
                    "  gl_FragColor = vec4(w[2], w[0], w[3], 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.3f, 1e-6f);
    ASSERT_NEAR(o[1], 0.1f, 1e-6f);
    ASSERT_NEAR(o[2], 0.4f, 1e-6f);

    /* **A `vec3` array, where the stride is three and not one.** An implementation that gave
     * every element one register would read `v[1].x` out of `v[0].y` and the answer would be
     * plausible - 0.2 instead of 0.4 - so the values are chosen to tell those apart. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  vec3 v[2];\n"
                    "  v[0] = vec3(0.1, 0.2, 0.3);\n"
                    "  v[1] = vec3(0.4, 0.5, 0.6);\n"
                    "  gl_FragColor = vec4(v[1].x, v[0].y, v[1].z, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.4f, 1e-6f);
    ASSERT_NEAR(o[1], 0.2f, 1e-6f);
    ASSERT_NEAR(o[2], 0.6f, 1e-6f);

    /* **The one this exists for: an unrolled loop's counter as the index.** 1+2+3+4 = 10, and
     * 10 * 0.05 is 0.5 - a sum no single element produces, so a loop that read the same element
     * four times gives a different answer rather than a near one. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float w[4];\n"
                    "  w[0] = 1.0; w[1] = 2.0; w[2] = 3.0; w[3] = 4.0;\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 0; i < 4; i++) { total += w[i]; }\n"
                    "  gl_FragColor = vec4(total * 0.05, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-6f);

    /* Written through the counter too, then read back the other way round, so the write and
     * the read have to agree about which element is which. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float w[4];\n"
                    "  for (int i = 0; i < 4; i++) { w[i] = float(i) * 0.1; }\n"
                    "  gl_FragColor = vec4(w[3], w[1], w[0], 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.3f, 1e-6f);
    ASSERT_NEAR(o[1], 0.1f, 1e-6f);
    ASSERT_NEAR(o[2], 0.0f, 1e-6f);

    /* An element assigned through a swizzle, which goes through the same place. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  vec3 v[2];\n"
                    "  v[0] = vec3(0.0, 0.0, 0.0);\n"
                    "  v[1] = vec3(0.0, 0.0, 0.0);\n"
                    "  v[1].xz = vec2(0.25, 0.75);\n"
                    "  gl_FragColor = vec4(v[1].x, v[1].y, v[1].z, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, 1e-6f);
    ASSERT_NEAR(o[1], 0.0f, 1e-6f);
    ASSERT_NEAR(o[2], 0.75f, 1e-6f);

    glContextDestroy(ctx);
}

/* What an array cannot do here, each said in its own words. */
static void test_gl2_arrays_refuse_what_a_register_file_cannot_do(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[512];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};

    static const struct { const char *fs; const char *wants; } cases[] = {
        /* **An index only known while the shader runs.** An array is a run of registers and a
         * register file cannot be indexed by a value the shader computes - the alternatives are
         * a select chain costing the whole array per access, or memory this back end has not
         * got, and neither is something to do quietly. */
        {"uniform float k;\n"
         "void main() {\n"
         "  float w[4];\n"
         "  w[0] = 0.1; w[1] = 0.2; w[2] = 0.3; w[3] = 0.4;\n"
         "  gl_FragColor = vec4(w[int(k)], 0.0, 0.0, 1.0);\n"
         "}\n",
         "known when the shader is compiled"},
        /* A whole array as a value, which the language has no expression for either. */
        {"void main() {\n"
         "  float w[4];\n"
         "  float v[4];\n"
         "  w[0] = 0.1;\n"
         "  v = w;\n"
         "  gl_FragColor = vec4(v[0], 0.0, 0.0, 1.0);\n"
         "}\n",
         "element at a time"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const GLuint prog = linked_program(VS_ONE_VARYING, cases[i].fs);
        log[0] = '\0';
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, prog), words, 512u, &count,
                                              &vgprs, NULL, NULL, log, sizeof(log)),
                  GL_FALSE);
        if (strstr(log, cases[i].wants) == NULL) {
            printf("\n    case %d: expected a message about '%s', got '%s'\n", (int)i,
                   cases[i].wants, log);
        }
        ASSERT_TRUE(strstr(log, cases[i].wants) != NULL);
    }

    glContextDestroy(ctx);
}

/* **An early `return`, which ends the function and nothing else.**
 *
 * The value goes into the caller's result under the exec the lanes have at that point, so each
 * lane takes the value from whichever return it reached. Then the lanes come out of every `if`
 * and loop inside the function - the same drop `break` and `discard` do - and `exec` is
 * cleared, so the rest of the body writes nothing for them. What it must **not** touch is
 * anything around the call: the lane still finishes the caller's statement and still goes round
 * the caller's loop.
 */
static void test_gl2_an_early_return_ends_the_function_and_nothing_else(void) {
    void *ctx = gl2_context();
    float o[4];
    const float lo[4][4] = {{0.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float hi[4][4] = {{1.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* The shape the refusal test used to carry: a guard clause, then the real work. Both arms
     * over the same varying, so one run takes the early return and the other does not. */
    static const char *const FS_GUARD =
        "varying vec4 vin;\n"
        "float f(float a) {\n"
        "  if (a > 0.5) { return 0.25; }\n"
        "  return 0.75;\n"
        "}\n"
        "void main() { gl_FragColor = vec4(f(vin.x), 0.0, 0.0, 1.0); }\n";
    compile_and_run(ctx, VS_ONE_VARYING, FS_GUARD, hi, o);
    ASSERT_NEAR(o[0], 0.25f, 1e-6f);
    compile_and_run(ctx, VS_ONE_VARYING, FS_GUARD, lo, o);
    ASSERT_NEAR(o[0], 0.75f, 1e-6f);

    /* **The statements after the return must not run for the lane that took it.** If `exec` is
     * not cleared, `t` is written twice and the answer is the second value - so 0.25 against
     * 0.9 is the difference between returning early and merely computing a value early. */
    static const char *const FS_SKIPS =
        "varying vec4 vin;\n"
        "float f(float a) {\n"
        "  float t = 0.25;\n"
        "  if (a > 0.5) { return t; }\n"
        "  t = 0.9;\n"
        "  return t;\n"
        "}\n"
        "void main() { gl_FragColor = vec4(f(vin.x), 0.0, 0.0, 1.0); }\n";
    compile_and_run(ctx, VS_ONE_VARYING, FS_SKIPS, hi, o);
    ASSERT_NEAR(o[0], 0.25f, 1e-6f);

    /* **And the caller carries on.** The statement the call sits in still runs for a lane that
     * returned early - the green channel is written after the call and must be there whichever
     * way the function went. A return that cleared `exec` and did not put it back gives green
     * zero on exactly the lanes that returned. */
    static const char *const FS_CALLER_GOES_ON =
        "varying vec4 vin;\n"
        "float f(float a) {\n"
        "  if (a > 0.5) { return 0.25; }\n"
        "  return 0.75;\n"
        "}\n"
        "void main() {\n"
        "  float r = f(vin.x);\n"
        "  gl_FragColor = vec4(r, 1.0, 0.0, 1.0);\n"
        "}\n";
    compile_and_run(ctx, VS_ONE_VARYING, FS_CALLER_GOES_ON, hi, o);
    ASSERT_NEAR(o[0], 0.25f, 1e-6f);
    ASSERT_NEAR(o[1], 1.0f, 1e-6f);
    compile_and_run(ctx, VS_ONE_VARYING, FS_CALLER_GOES_ON, lo, o);
    ASSERT_NEAR(o[0], 0.75f, 1e-6f);
    ASSERT_NEAR(o[1], 1.0f, 1e-6f);

    /* **An `out` parameter is copied back even for a lane that returned early.** The copy-back
     * runs after the mask is restored; if it ran before, the caller's variable would keep what
     * it held and the blue channel would be zero. */
    static const char *const FS_OUT_PARAM =
        "varying vec4 vin;\n"
        "float f(float a, out float mark) {\n"
        "  mark = 0.5;\n"
        "  if (a > 0.5) { return 0.25; }\n"
        "  mark = 1.0;\n"
        "  return 0.75;\n"
        "}\n"
        "void main() {\n"
        "  float m = 0.0;\n"
        "  float r = f(vin.x, m);\n"
        "  gl_FragColor = vec4(r, 0.0, m, 1.0);\n"
        "}\n";
    compile_and_run(ctx, VS_ONE_VARYING, FS_OUT_PARAM, hi, o);
    ASSERT_NEAR(o[0], 0.25f, 1e-6f);
    ASSERT_NEAR(o[2], 0.5f, 1e-6f);  /* the value it had when it returned, not the later one */
    compile_and_run(ctx, VS_ONE_VARYING, FS_OUT_PARAM, lo, o);
    ASSERT_NEAR(o[0], 0.75f, 1e-6f);
    ASSERT_NEAR(o[2], 1.0f, 1e-6f);

    /* **A return out of a loop inside the function.** The loop reloads `exec` from its own mask
     * at the top of every trip, so a returning lane left in that mask goes round again - and
     * would then run the statements after the loop as well. Returns at 3, so 0.3; a lane that
     * kept going reaches the trailing `return 0.9`. */
    static const char *const FS_RETURN_FROM_LOOP =
        "varying vec4 vin;\n"
        "float f(float a) {\n"
        "  float t = 0.0;\n"
        "  for (int i = 0; i < 200; i++) {\n"
        "    t += 1.0;\n"
        "    if (t > 2.5) { return t * 0.1; }\n"
        "  }\n"
        "  return 0.9;\n"
        "}\n"
        "void main() { gl_FragColor = vec4(f(vin.x), 1.0, 0.0, 1.0); }\n";
    compile_and_run(ctx, VS_ONE_VARYING, FS_RETURN_FROM_LOOP, hi, o);
    ASSERT_NEAR(o[0], 0.3f, 1e-6f);
    ASSERT_NEAR(o[1], 1.0f, 1e-6f); /* and the caller still ran */

    /* **A bare `return;` from a void function**, which has no value to write and still has to
     * stop the body. Without the mask, `mark` is overwritten and comes back 1.0. */
    static const char *const FS_VOID_RETURN =
        "varying vec4 vin;\n"
        "void g(float a, out float mark) {\n"
        "  mark = 0.25;\n"
        "  if (a > 0.5) { return; }\n"
        "  mark = 1.0;\n"
        "}\n"
        "void main() {\n"
        "  float m = 0.0;\n"
        "  g(vin.x, m);\n"
        "  gl_FragColor = vec4(m, 0.0, 0.0, 1.0);\n"
        "}\n";
    compile_and_run(ctx, VS_ONE_VARYING, FS_VOID_RETURN, hi, o);
    ASSERT_NEAR(o[0], 0.25f, 1e-6f);
    compile_and_run(ctx, VS_ONE_VARYING, FS_VOID_RETURN, lo, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f);

    /* **Nested calls**, so the two functions' masks are distinct. The inner returns early and
     * the outer must carry on to its own arithmetic - an inner return that reached the outer
     * function's mask gives 0.2 instead of 0.5. */
    static const char *const FS_NESTED =
        "varying vec4 vin;\n"
        "float inner(float a) { if (a > 0.5) { return 0.2; } return 0.4; }\n"
        "float outer(float a) {\n"
        "  float v = inner(a);\n"
        "  if (a > 9.0) { return 0.0; }\n"
        "  return v + 0.3;\n"
        "}\n"
        "void main() { gl_FragColor = vec4(outer(vin.x), 0.0, 0.0, 1.0); }\n";
    compile_and_run(ctx, VS_ONE_VARYING, FS_NESTED, hi, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-6f);
    compile_and_run(ctx, VS_ONE_VARYING, FS_NESTED, lo, o);
    ASSERT_NEAR(o[0], 0.7f, 1e-6f);

    /* **A function with no early return emits no mask**, which is what keeps every shader that
     * had none costing exactly what it did. Two words fewer than the same body with a guard. */
    {
        gl_context_t *c = (gl_context_t *)ctx;
        uint32_t plain[512], guarded[512];
        uint32_t n_plain = 0u, n_guarded = 0u, vgprs = 0u;
        char log[256] = {0};
        const GLuint p1 = linked_program(
            VS_ONE_VARYING, "varying vec4 vin;\n"
                            "float f(float a) { return a * 0.5; }\n"
                            "void main() { gl_FragColor = vec4(f(vin.x), 0.0, 0.0, 1.0); }\n");
        const GLuint p2 = linked_program(VS_ONE_VARYING, FS_GUARD);
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, p1), plain, 512u, &n_plain,
                                              &vgprs, NULL, NULL, log, sizeof(log)),
                  GL_TRUE);
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, p2), guarded, 512u, &n_guarded,
                                              &vgprs, NULL, NULL, log, sizeof(log)),
                  GL_TRUE);
        /* `s_and_saveexec_b32` is SOP1 op 60; the guarded one has the `if`'s and the
         * function's, the plain one has neither. */
        int saves_plain = 0, saves_guarded = 0;
        for (uint32_t i = 0; i < n_plain; i++) {
            if ((plain[i] >> 23) == 0x17du && ((plain[i] >> 8) & 0xffu) == 60u) saves_plain++;
        }
        for (uint32_t i = 0; i < n_guarded; i++) {
            if ((guarded[i] >> 23) == 0x17du && ((guarded[i] >> 8) & 0xffu) == 60u) {
                saves_guarded++;
            }
        }
        ASSERT_EQ(saves_plain, 0);
        ASSERT_TRUE(saves_guarded >= 1);
    }

    glContextDestroy(ctx);
}

/* The shapes that are refused rather than generated, each with the reason named. A call
 * mechanism that quietly did something else for these is the failure this guards. */
static void test_gl2_the_back_end_refuses_the_calls_it_cannot_inline(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};

    static const struct { const char *fs; const char *wants; } cases[] = {
        /* **A return in `main`, which is the one that is still refused.** An early return
         * inside a function hands its lanes back at the call; `main` has no call to hand them
         * back at, and the export that retires the wave runs after the body rather than inside
         * it - so the lanes would have to be held off across a boundary this generator does not
         * reach. */
        {"varying vec4 vin;\n"
         "void main() {\n"
         "  gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
         "  if (vin.x > 0.0) { return; }\n"
         "  gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);\n"
         "}\n",
         "`main`"},
        /* A value-returning function still has to end in a return: GLSL requires every path to
         * return, and the trailing one is what catches the lanes no earlier return took. */
        {"varying vec4 vin;\n"
         "float f(float a) { if (a > 0.0) { return 1.0; } }\n"
         "void main() { gl_FragColor = vec4(f(vin.x), 0.0, 0.0, 1.0); }\n",
         "has to end in"},
        /* Not here: an `out` argument that is not a place. The back end checks it, because the
         * copy-back has to have somewhere to write - but the semantic stage owns l-value
         * validity and refuses `f(x, q.xx)` and `f(x, 1.0)` before the back end sees either, so
         * no shader can carry one this far. */
        /* Recursion is invalid GLSL; what matters is that it ends in a message. */
        {"varying vec4 vin;\n"
         "float f(float a) { return f(a); }\n"
         "void main() { gl_FragColor = vec4(f(vin.x), 0.0, 0.0, 1.0); }\n",
         "recursion"},
        /* Not here: a call with the wrong number of arguments. The back end checks it, because
         * a mismatch there would bind a parameter to a register nothing wrote - but the front
         * end rejects it first ("wrong number of arguments"), so no shader can carry one this
         * far and a case for it would be testing the front end by proxy. */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const GLuint prog = linked_program(VS_ONE_VARYING, cases[i].fs);
        log[0] = '\0';
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, prog), words, 256u, &count,
                                              &vgprs, NULL, NULL, log, sizeof(log)),
                  GL_FALSE);
        if (strstr(log, cases[i].wants) == NULL) {
            printf("\n    case %d: expected a message about '%s', got '%s'\n", (int)i,
                   cases[i].wants, log);
        }
        ASSERT_TRUE(strstr(log, cases[i].wants) != NULL);
    }

    glContextDestroy(ctx);
}

/*
 * **Structs compiled to gfx1030 and simulated**, which is a different question from the software
 * path's: there a struct is a float array and here it is a run of registers, and the two agree
 * only because both read the layout the semantic pass fixed. A member resolved to the wrong
 * register is a colour, not an error - so each case puts a different member in a different
 * channel and a layout off by one comes back rotated.
 */
static void test_gl2_compiled_structs(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0.25f, 0.5f, 0.75f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float tol = 2e-3f;

    /* Construct and read each member back into its own channel. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "struct C { float r; float g; float b; };\n"
                    "void main() {\n"
                    "  C c = C(0.25, 0.5, 0.75);\n"
                    "  gl_FragColor = vec4(c.r, c.g, c.b, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.75f, tol);

    /* A vector member, and a swizzle of it - both meanings of `.` in one expression. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "struct M { float a; vec3 v; };\n"
                    "void main() {\n"
                    "  M m = M(0.0, vec3(0.25, 0.5, 0.75));\n"
                    "  gl_FragColor = vec4(m.v.z, m.v.y, m.v.x, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.75f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.25f, tol);

    /* **A member is writable**, and writing one must not disturb its neighbours. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "struct C { float r; float g; float b; };\n"
                    "void main() {\n"
                    "  C c = C(0.25, 0.5, 0.75);\n"
                    "  c.g = 0.125;\n"
                    "  gl_FragColor = vec4(c.r, c.g, c.b, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.125f, tol);
    ASSERT_NEAR(o[2], 0.75f, tol);

    /* Whole-struct assignment, where writing the copy must leave the original alone. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "struct C { float r; float g; float b; };\n"
                    "void main() {\n"
                    "  C a = C(0.25, 0.5, 0.75);\n"
                    "  C b = a;\n"
                    "  b.r = 1.0;\n"
                    "  gl_FragColor = vec4(a.r, b.g, b.b, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.75f, tol);

    /* Nested, so the inner struct's layout is added to the outer one's. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "struct In { float x; float y; };\n"
                    "struct Out { float lead; In in2; };\n"
                    "void main() {\n"
                    "  Out s = Out(0.25, In(0.5, 0.75));\n"
                    "  gl_FragColor = vec4(s.lead, s.in2.x, s.in2.y, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.75f, tol);

    /* Built from a varying, so the members carry interpolated values rather than constants -
     * a constant-folded layout would pass the cases above and fail this one. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "struct C { float r; float g; float b; };\n"
                    "void main() {\n"
                    "  C c = C(vin.x, vin.y, vin.z);\n"
                    "  gl_FragColor = vec4(c.b, c.g, c.r, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.75f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.25f, tol);

    /* Through a function, by value both ways. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "struct C { float r; float g; float b; };\n"
                    "C half_of(C c) { return C(c.r * 0.5, c.g * 0.5, c.b * 0.5); }\n"
                    "void main() {\n"
                    "  C c = half_of(C(0.5, 1.0, 1.5));\n"
                    "  gl_FragColor = vec4(c.r, c.g, c.b, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.75f, tol);

    glContextDestroy(ctx);
}

static void test_gl2_compiled_arithmetic_matches_the_language(void) {
    void *ctx = gl2_context();
    /* The reciprocal is a 1-ULP instruction and the transcendentals are worse, so these compare
     * to a tolerance - and the tolerance is loose enough to pass a correct lowering and nowhere
     * near loose enough to pass a wrong one. `sin` without its scale is out by 0.9 here. */
    const float tol = 2e-5f;
    float o[4];
    const float x = 0.7f, y = 2.5f, z = -1.25f, w = 4.0f;
    const float attr[4][4] = {{x, y, z, w}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* **The four lowerings with a constant in them.** `sin` and `cos` are in revolutions, `exp`
     * and `log` are base two - each of these fails by a clean factor if its constant is
     * dropped, which is what makes them worth simulating. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = vec4(sin(vin.x), cos(vin.x), exp(vin.x), log(vin.y));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], sinf(x), tol);
    ASSERT_NEAR(o[1], cosf(x), tol);
    ASSERT_NEAR(o[2], expf(x), 1e-4f);
    ASSERT_NEAR(o[3], logf(y), tol);

    /* Division, which is a reciprocal and a multiply - and the operand order, which `v_sub_f32`
     * makes easy to reverse. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = vec4(vin.w / vin.y, vin.y - vin.x, vin.x - vin.y,\n"
                    "                      pow(vin.y, vin.x));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], w / y, tol);
    ASSERT_NEAR(o[1], y - x, tol);
    ASSERT_NEAR(o[2], x - y, tol);
    ASSERT_NEAR(o[3], powf(y, x), 1e-4f);

    /* The common functions, including the two whose definition has a case in it: `sign` is
     * three-valued, and `mod` is `x - y * floor(x/y)` and so is positive for a negative `x`. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = vec4(abs(vin.z), sign(vin.z), mod(vin.z, vin.y),\n"
                    "                      fract(vin.y));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.25f, tol);
    ASSERT_NEAR(o[1], -1.0f, tol);
    ASSERT_NEAR(o[2], z - y * floorf(z / y), tol);  /* 1.25, not -1.25 */
    ASSERT_NEAR(o[3], 0.5f, tol);

    /* `sign(0.0)` is zero and not one, which is the case a single select gets wrong. */
    const float zeros[4][4] = {{0, 1, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() { gl_FragColor = vec4(sign(vin.x), sign(vin.y), 0.0, 1.0); }\n",
                    zeros, o);
    ASSERT_NEAR(o[0], 0.0f, tol);
    ASSERT_NEAR(o[1], 1.0f, tol);

    /* Interpolating, clamping and stepping. `mix` at t = 0 and t = 1 has to be exactly its
     * endpoints, which `a + (b-a)*t` is and `a*(1-t) + b*t` is not. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = vec4(mix(vin.z, vin.w, 0.0), mix(vin.z, vin.w, 1.0),\n"
                    "                      clamp(vin.w, 0.0, 1.0), step(vin.x, vin.y));\n"
                    "}\n",
                    attr, o);
    ASSERT_EQ(o[0] == z, GL_TRUE);
    ASSERT_EQ(o[1] == w, GL_TRUE);
    ASSERT_NEAR(o[2], 1.0f, tol);
    ASSERT_NEAR(o[3], 1.0f, tol);   /* y >= x, so 1 */

    /* `smoothstep`, transcribed from the specification's own expansion. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  float t = smoothstep(0.0, 4.0, vin.y);\n"
                    "  gl_FragColor = vec4(t, smoothstep(0.0, 4.0, -1.0),\n"
                    "                      smoothstep(0.0, 4.0, 9.0), 1.0);\n"
                    "}\n",
                    attr, o);
    {
        const float t = y / 4.0f;              /* clamped, and inside the range here */
        ASSERT_NEAR(o[0], t * t * (3.0f - 2.0f * t), 1e-4f);
        ASSERT_NEAR(o[1], 0.0f, tol);          /* clamped below */
        ASSERT_NEAR(o[2], 1.0f, tol);          /* clamped above */
    }

    glContextDestroy(ctx);
}

static void test_gl2_compiled_geometry_matches_the_language(void) {
    void *ctx = gl2_context();
    const float tol = 2e-5f;
    float o[4];
    const float attr[4][4] = {{3.0f, 4.0f, 12.0f, 2.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  vec3 v = vin.xyz;\n"
                    "  gl_FragColor = vec4(length(v), dot(v, v), normalize(v).z,\n"
                    "                      distance(v, vec3(0.0, 0.0, 0.0)));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 13.0f, 1e-3f);           /* 3, 4, 12 */
    ASSERT_NEAR(o[1], 169.0f, 1e-3f);
    ASSERT_NEAR(o[2], 12.0f / 13.0f, 1e-4f);
    ASSERT_NEAR(o[3], 13.0f, 1e-3f);

    /* `cross`, where the index pattern is the whole of the correctness. x cross y is z. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  vec3 c = cross(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0));\n"
                    "  gl_FragColor = vec4(c, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, tol);
    ASSERT_NEAR(o[1], 0.0f, tol);
    ASSERT_NEAR(o[2], 1.0f, tol);

    /* `reflect(I, N)` off a surface whose normal is +y turns a downward ray upward. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  vec3 r = reflect(vec3(1.0, -1.0, 0.0), vec3(0.0, 1.0, 0.0));\n"
                    "  gl_FragColor = vec4(r, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, tol);
    ASSERT_NEAR(o[1], 1.0f, tol);
    ASSERT_NEAR(o[2], 0.0f, tol);

    glContextDestroy(ctx);
}

static void test_gl2_compiled_swizzle_writes_land_where_they_are_named(void) {
    void *ctx = gl2_context();
    const float tol = 1e-6f;
    float o[4];
    const float attr[4][4] = {{0.1f, 0.2f, 0.3f, 0.4f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* **A swizzle write is not a masked move here**, it is a move into each register the
     * swizzle names - and `.zyx` crossing over is the case that tells a correct implementation
     * from one that writes the components in the order it read them. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor.rgb = vin.xyz;\n"
                    "  gl_FragColor.a = 1.0;\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.1f, tol);
    ASSERT_NEAR(o[1], 0.2f, tol);
    ASSERT_NEAR(o[2], 0.3f, tol);
    ASSERT_NEAR(o[3], 1.0f, tol);

    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  vec4 c = vec4(0.0);\n"
                    "  c.zyx = vin.xyz;\n"
                    "  c.w = 1.0;\n"
                    "  gl_FragColor = c;\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.3f, tol);   /* c.x took vin.z */
    ASSERT_NEAR(o[1], 0.2f, tol);
    ASSERT_NEAR(o[2], 0.1f, tol);
    ASSERT_NEAR(o[3], 1.0f, tol);

    /* The compound forms, which read the destination and write it back. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  vec4 c = vin;\n"
                    "  c.rgb *= 2.0;\n"
                    "  c.r += 1.0;\n"
                    "  c.g -= 0.1;\n"
                    "  c.a /= 4.0;\n"
                    "  gl_FragColor = c;\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.1f * 2.0f + 1.0f, tol);
    ASSERT_NEAR(o[1], 0.2f * 2.0f - 0.1f, 1e-5f);
    ASSERT_NEAR(o[2], 0.3f * 2.0f, tol);
    ASSERT_NEAR(o[3], 0.4f / 4.0f, 1e-5f);

    glContextDestroy(ctx);
}

static void test_gl2_compiled_uniforms_come_from_the_scalar_file(void) {
    void *ctx = gl2_context();
    const float tol = 1e-6f;
    float o[4];
    const float attr[4][4] = {{0.25f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* **A uniform is the same in every lane, so it lives in an SGPR** - loaded from a block the
     * draw puts in the payload, not interpolated. This sets the values through the API and
     * reads the colour back out of the simulated shader, so what it checks is the whole path:
     * the pool the linker laid out, the offsets the compiler loaded from, and the wait between
     * the load and the first read. */
    const GLuint prog = linked_program(
        "attribute vec4 pos;\n"
        "varying vec4 vin;\n"
        "void main() { vin = pos; gl_Position = pos; }\n",
        "uniform float amount;\n"
        "uniform vec3 tint;\n"
        "varying vec4 vin;\n"
        "void main() { gl_FragColor = vec4(tint * amount, vin.x); }\n");
    glUseProgram(prog);
    glUniform1f(glGetUniformLocation(prog, "amount"), 2.0f);
    glUniform3f(glGetUniformLocation(prog, "tint"), 0.1f, 0.2f, 0.3f);

    compile_and_run_prog(ctx, prog, attr, o);
    ASSERT_NEAR(o[0], 0.2f, 1e-5f);
    ASSERT_NEAR(o[1], 0.4f, 1e-5f);
    ASSERT_NEAR(o[2], 0.6f, 1e-5f);
    ASSERT_NEAR(o[3], 0.25f, tol);   /* still the varying, so the two sources do not collide */

    /* **A changed uniform changes the picture with no recompile.** The words are the same; the
     * block they load from is not. That is the whole point of the scalar path - the alternative,
     * baking the values in as literals, would need this shader compiled again. */
    glUniform1f(glGetUniformLocation(prog, "amount"), 0.5f);
    compile_and_run_prog(ctx, prog, attr, o);
    ASSERT_NEAR(o[0], 0.05f, 1e-5f);
    ASSERT_NEAR(o[2], 0.15f, 1e-5f);

    /* **A uniform the fragment shader never names costs it nothing.** `mvp` is sixteen floats
     * in the shared pool and the vertex stage's business; a compiler that moved every uniform
     * into a VGPR would spend sixteen registers on it here. The pool is still loaded whole -
     * scalar loads are cheap and the offsets have to stay the ones the linker chose. */
    const GLuint shared = linked_program(
        "uniform mat4 mvp;\n"
        "attribute vec4 pos;\n"
        "varying vec4 vin;\n"
        "void main() { vin = pos; gl_Position = mvp * pos; }\n",
        "uniform float k;\n"
        "varying vec4 vin;\n"
        "void main() { gl_FragColor = vec4(k, k, k, vin.x); }\n");
    glUseProgram(shared);
    glUniform1f(glGetUniformLocation(shared, "k"), 0.75f);

    const gl_program_object_t *sp = gl_find_program((gl_context_t *)ctx, shared);
    static uint32_t words[512];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};
    ASSERT_EQ(gl_program_compile_fragment(sp, words, 512u, &count, &vgprs, NULL, NULL, log,
                                          sizeof(log)),
              GL_TRUE);
    /* v8 and v9 for the one varying component and `k`, plus `gl_FragColor`'s four and the
     * eight the hardware owns: nowhere near the seventeen a materialised `mvp` would add. */
    ASSERT_TRUE(vgprs < 24u);

    compile_and_run_prog(ctx, shared, attr, o);
    ASSERT_NEAR(o[0], 0.75f, tol);
    ASSERT_NEAR(o[3], 0.25f, tol);

    glContextDestroy(ctx);
}

static void test_gl2_compiled_control_flow_runs_the_right_arm(void) {
    void *ctx = gl2_context();
    const float tol = 1e-6f;
    float o[4];

    /* `vin.x` is the condition's input, so the same shader is run twice with different values
     * and has to take different arms. **A single lane is enough to see the mask arithmetic go
     * wrong**: both arms execute either way, and what is being checked is which one wrote. */
    const char *const IF_ELSE =
        "varying vec4 vin;\n"
        "void main() {\n"
        "  vec4 c = vec4(0.0);\n"
        "  if (vin.x > 0.5) { c = vec4(1.0, 0.0, 0.0, 1.0); }\n"
        "  else { c = vec4(0.0, 1.0, 0.0, 1.0); }\n"
        "  gl_FragColor = c;\n"
        "}\n";
    const float hi[4][4] = {{0.9f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float lo[4][4] = {{0.1f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, IF_ELSE, hi, o), GL_TRUE);
    ASSERT_NEAR(o[0], 1.0f, tol);
    ASSERT_NEAR(o[1], 0.0f, tol);
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, IF_ELSE, lo, o), GL_TRUE);
    ASSERT_NEAR(o[0], 0.0f, tol);
    ASSERT_NEAR(o[1], 1.0f, tol);

    /* Nested, and an `if` with no `else`. The inner one saves into the next scalar register
     * along, and an outer arm no lane is running must not let the inner one write. */
    const char *const NESTED =
        "varying vec4 vin;\n"
        "void main() {\n"
        "  float r = 0.0;\n"
        "  if (vin.x > 0.5) {\n"
        "    r = 0.25;\n"
        "    if (vin.y > 0.5) { r = 0.5; }\n"
        "  }\n"
        "  gl_FragColor = vec4(r, 0.0, 0.0, 1.0);\n"
        "}\n";
    const float both[4][4] = {{0.9f, 0.9f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float outer[4][4] = {{0.9f, 0.1f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float neither[4][4] = {{0.1f, 0.9f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    compile_and_run(ctx, VS_ONE_VARYING, NESTED, both, o);
    ASSERT_NEAR(o[0], 0.5f, tol);
    compile_and_run(ctx, VS_ONE_VARYING, NESTED, outer, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    /* **The one that catches a wrong inner mask.** The outer arm is dead, so the inner `if`'s
     * condition is true and its body must still write nothing. */
    compile_and_run(ctx, VS_ONE_VARYING, NESTED, neither, o);
    ASSERT_NEAR(o[0], 0.0f, tol);

    /* The logical operators, which over values that are exactly 0.0 or 1.0 are min, max and
     * `1 - x` - no comparison and no branch among them. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  bool a = vin.x > 0.5;\n"
                    "  bool b = vin.y > 0.5;\n"
                    "  float p = (a && b) ? 1.0 : 0.0;\n"
                    "  float q = (a || b) ? 1.0 : 0.0;\n"
                    "  float r = (!a) ? 1.0 : 0.0;\n"
                    "  gl_FragColor = vec4(p, q, r, 1.0);\n"
                    "}\n",
                    outer, o);   /* a true, b false */
    ASSERT_NEAR(o[0], 0.0f, tol);
    ASSERT_NEAR(o[1], 1.0f, tol);
    ASSERT_NEAR(o[2], 0.0f, tol);

    /* A vector `==`, which is true only when **every** component agrees - the case a
     * per-component answer combined with the wrong operator gets backwards. */
    const float magenta[4][4] = {{1.0f, 0.0f, 1.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float nearly[4][4] = {{1.0f, 0.5f, 1.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const char *const VEC_EQ =
        "varying vec4 vin;\n"
        "void main() {\n"
        "  float m = (vin.xyz == vec3(1.0, 0.0, 1.0)) ? 1.0 : 0.0;\n"
        "  gl_FragColor = vec4(m, 0.0, 0.0, 1.0);\n"
        "}\n";
    compile_and_run(ctx, VS_ONE_VARYING, VEC_EQ, magenta, o);
    ASSERT_NEAR(o[0], 1.0f, tol);
    compile_and_run(ctx, VS_ONE_VARYING, VEC_EQ, nearly, o);
    ASSERT_NEAR(o[0], 0.0f, tol);

    glContextDestroy(ctx);
}

static void test_gl2_compiled_discard_kills_the_lane_for_good(void) {
    void *ctx = gl2_context();
    float o[4];
    const float hi[4][4] = {{0.9f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float lo[4][4] = {{0.1f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* **This is the shape craft's block shader opens with**, and the one that matters: a
     * conditional discard, then work afterwards that the surviving lanes still do. */
    const char *const KEY =
        "varying vec4 vin;\n"
        "void main() {\n"
        "  if (vin.x > 0.5) { discard; }\n"
        "  gl_FragColor = vec4(0.25, 0.5, 0.75, 1.0);\n"
        "}\n";

    /* **The lane comes back if the discard only narrowed `exec`.** The `if` restores the mask
     * it saved on the way in, so a discard that did not also take the lane out of that save
     * would be undone three instructions later - and the shader would export a colour for a
     * fragment it had just thrown away. */
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, KEY, hi, o), GL_FALSE);
    /* It still exports, and still with `done`: a wave that discarded every lane must retire. */
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, KEY, lo, o), GL_TRUE);
    ASSERT_NEAR(o[0], 0.25f, 1e-6f);
    ASSERT_NEAR(o[2], 0.75f, 1e-6f);

    /* The same from two levels in, where the lane has to come out of both saved masks. */
    const char *const NESTED_KEY =
        "varying vec4 vin;\n"
        "void main() {\n"
        "  if (vin.x > 0.5) {\n"
        "    if (vin.y > 0.5) { discard; }\n"
        "  }\n"
        "  gl_FragColor = vec4(1.0);\n"
        "}\n";
    const float both[4][4] = {{0.9f, 0.9f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float one[4][4] = {{0.9f, 0.1f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, NESTED_KEY, both, o), GL_FALSE);
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, NESTED_KEY, one, o), GL_TRUE);

    /* **A discard with an `else` after it**, which is where the order of the two instructions
     * the discard emits stops being a detail.
     *
     * The `else` is `exec = saved & ~exec`, and a discard has just set `exec` to zero - so that
     * reads `saved & ~0`, which is `saved` entire. It is only correct because the discard took
     * the lane out of `saved` *first*. Drop that step and a discarded lane reappears in the
     * `else`, runs it, and is live again at the `if`'s restore: the shader then exports a
     * colour for a fragment it threw away, and the wrong one at that.
     *
     * One lane, so this is two runs: the lane that discards must not survive, and the lane that
     * does not must come out with the else's value and not the earlier one. */
    const char *const ELSE_KEY =
        "varying vec4 vin;\n"
        "void main() {\n"
        "  gl_FragColor = vec4(0.125);\n"
        "  if (vin.x > 0.5) { discard; } else { gl_FragColor = vec4(0.75); }\n"
        "}\n";
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, ELSE_KEY, hi, o), GL_FALSE);
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, ELSE_KEY, lo, o), GL_TRUE);
    ASSERT_NEAR(o[0], 0.75f, 1e-6f);

    /* An unconditional one, which kills the lane whatever the inputs are. */
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING,
                              "varying vec4 vin;\n"
                              "void main() { discard; gl_FragColor = vec4(1.0); }\n",
                              lo, o),
              GL_FALSE);

    glContextDestroy(ctx);
}

static void test_gl2_compiled_globals_are_in_scope(void) {
    void *ctx = gl2_context();
    const float tol = 1e-5f;
    float o[4];
    const float attr[4][4] = {{0.5f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* **A `const` at file scope is what a real shader opens with** - craft's block shader
     * declares `pi`, `light_color` and `ambient_color` before `main` and uses all three. They
     * are generated in source order, so one may be written in terms of an earlier one. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "const float half_turn = 0.5;\n"
                    "const vec3 tint = vec3(0.2, 0.4, 0.6);\n"
                    "const vec3 doubled = tint + tint;\n"
                    "varying vec4 vin;\n"
                    "void main() { gl_FragColor = vec4(doubled * half_turn, vin.x); }\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.2f, tol);
    ASSERT_NEAR(o[1], 0.4f, tol);
    ASSERT_NEAR(o[2], 0.6f, tol);
    ASSERT_NEAR(o[3], 0.5f, tol);

    /* A plain global, which GLSL 1.10 allows and which is a variable like any other. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "float scale = 3.0;\n"
                    "varying vec4 vin;\n"
                    "void main() { scale = scale + 1.0; gl_FragColor = vec4(scale * 0.1); }\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.4f, tol);

    glContextDestroy(ctx);
}

/*
 * **A whole shader of the shape a port actually has**, rather than one feature at a time.
 *
 * This is craft's block shader with the texture lookups taken out: file-scope `const`s, uniforms
 * the API set, varyings, a conditional `discard`, `min`, `clamp`, `mix`, and a vector times a
 * scalar in three different places. The point is that the pieces compose - each has its own test
 * above, and a back end can pass all of those and still fall over on the first shader that uses
 * six of them at once, usually by running out of registers or by getting the allocator's
 * per-statement mark wrong.
 */
static void test_gl2_a_realistic_shader_compiles_and_computes(void) {
    void *ctx = gl2_context();
    float o[4];

    const float ao_in = 0.6f, light_in = 0.1f, fog_in = 0.25f, diffuse_in = 0.7f;
    const float attr[4][4] = {{ao_in, light_in, fog_in, diffuse_in},
                              {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    const GLuint prog = linked_program(
        "attribute vec4 pos;\n"
        "varying vec4 vin;\n"
        "void main() { vin = pos; gl_Position = pos; }\n",
        "uniform float daylight;\n"
        "uniform vec3 fog_color;\n"
        "varying vec4 vin;\n"
        "const vec3 light_color = vec3(0.6);\n"
        "const vec3 ambient_color = vec3(0.4);\n"
        "void main() {\n"
        "  vec3 color = vec3(0.8, 0.7, 0.5);\n"
        "  if (color.r < 0.0) { discard; }\n"
        "  float ao = min(1.0, vin.x + vin.y);\n"
        "  float df = min(1.0, vin.w + vin.y);\n"
        "  vec3 light = ambient_color + light_color * df;\n"
        "  color = clamp(color * light * ao, vec3(0.0), vec3(1.0));\n"
        "  color = mix(color, fog_color * daylight, vin.z);\n"
        "  gl_FragColor = vec4(color, 1.0);\n"
        "}\n");
    glUseProgram(prog);
    glUniform1f(glGetUniformLocation(prog, "daylight"), 0.8f);
    glUniform3f(glGetUniformLocation(prog, "fog_color"), 0.5f, 0.6f, 0.7f);

    /* The lane survives: the discard's condition is false. */
    ASSERT_EQ(compile_and_run_prog(ctx, prog, attr, o), GL_TRUE);

    /* The same arithmetic, written the way the shader writes it. */
    const float base[3] = {0.8f, 0.7f, 0.5f};
    const float fogc[3] = {0.5f, 0.6f, 0.7f};
    const float ao = 1.0f < (ao_in + light_in) ? 1.0f : (ao_in + light_in);
    const float df = 1.0f < (diffuse_in + light_in) ? 1.0f : (diffuse_in + light_in);
    const float light = 0.4f + 0.6f * df;
    for (int i = 0; i < 3; i++) {
        float c = base[i] * light * ao;
        if (c < 0.0f) c = 0.0f;
        if (c > 1.0f) c = 1.0f;
        const float f = fogc[i] * 0.8f;
        ASSERT_NEAR(o[i], c + (f - c) * fog_in, 1e-5f);
    }
    ASSERT_NEAR(o[3], 1.0f, 1e-6f);

    glContextDestroy(ctx);
}

/*
 * **Sampling a texture from a compiled shader.**
 *
 * The simulator's texture returns its own coordinate in x and y and the descriptor set it came
 * through in z, which is not a filter and does not need to be: what these check is that the
 * right coordinate reached the right set, and a texel made of both says so in one value.
 */
static void test_gl2_compiled_texture_lookups_reach_the_right_set(void) {
    void *ctx = gl2_context();
    const float tol = 1e-6f;
    float o[4];
    const float attr[4][4] = {{0.25f, 0.75f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2D tex;\n"
                    "varying vec4 vin;\n"
                    "void main() { gl_FragColor = texture2D(tex, vin.xy); }\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);   /* the coordinate arrived ... */
    ASSERT_NEAR(o[1], 0.75f, tol);
    ASSERT_NEAR(o[2], 0.0f, tol);    /* ... through set 0 */
    ASSERT_NEAR(o[3], 1.0f, tol);

    /* **`texture2DProj` is the same lookup with a divide in front**, by the coordinate's last
     * component - and because this simulator's texel *is* the coordinate, the division is
     * visible in the answer rather than inferred. 0.25/2 and 0.75/2. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2D tex;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = texture2DProj(tex, vec3(vin.x, vin.y, 2.0));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.125f, tol);
    ASSERT_NEAR(o[1], 0.375f, tol);

    /* **The vec4 form divides by `w` and ignores `z`**, which is the specification's rule and
     * not "the last component of the vector" - a 99.0 in `z` must change nothing. 0.25/4. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2D tex;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = texture2DProj(tex, vec4(vin.x, vin.y, 99.0, 4.0));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0625f, tol);
    ASSERT_NEAR(o[1], 0.1875f, tol);

    /* **A cube lookup hands over three address registers, and the third is the face.**
     *
     * The direction picks an axis by its largest component and a face by that component's sign,
     * and the other two become the place on it. `+X` is face 0 dead centre; `-Z` is face 5, and
     * a lowering that lost the sign would give 4. The simulator reports the face where a 2D
     * sample reports the descriptor set, because a wrong face is the failure a cube has that a
     * 2D does not - and behind a texel carrying only u and v it would be invisible. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform samplerCube tex;\n"
                    "void main() { gl_FragColor = textureCube(tex, vec3(1.0, 0.0, 0.0)); }\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, tol);   /* dead centre of the face ... */
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.0f, tol);   /* ... which is +X, face 0 */

    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform samplerCube tex;\n"
                    "void main() { gl_FragColor = textureCube(tex, vec3(0.0, 0.0, -1.0)); }\n",
                    attr, o);
    ASSERT_NEAR(o[2], 5.0f, tol);   /* -Z, not +Z */

    /* **Off-centre, and not normalised.** A direction is a direction: scaling all three
     * components leaves the face and the place on it alone, so `(2, 1, 0)` must land exactly
     * where `(1, 0.5, 0)` does. `tc` is `-y`, so a positive y moves v *down* - and a lowering
     * that dropped that sign gives 0.625 instead of 0.375. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform samplerCube tex;\n"
                    "void main() {\n"
                    "  vec4 a = textureCube(tex, vec3(1.0, 0.5, 0.0));\n"
                    "  vec4 b = textureCube(tex, vec3(2.0, 1.0, 0.0));\n"
                    "  gl_FragColor = vec4(a.y, b.y, a.z, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.375f, tol);
    ASSERT_NEAR(o[1], 0.375f, tol); /* the same, because the direction is the same */
    ASSERT_NEAR(o[2], 0.0f, tol);

    /* **A volume takes its three coordinates straight through**, with no face selection and no
     * divide - which is exactly what separates it from the cube it is one bit away from in the
     * instruction. All three must arrive, in order: a lowering that sent only `s` and `t` would
     * leave the slice holding whatever the allocator last put there. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler3D vol;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = texture3D(vol, vec3(vin.x, vin.y, 0.625));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.75f, tol);
    ASSERT_NEAR(o[2], 0.625f, tol);   /* the slice, not the descriptor set */

    /* And its projective form divides **all three** by `w`, where the 2D one divides two. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler3D vol;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = texture3DProj(vol, vec4(vin.x, vin.y, 1.0, 2.0));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.125f, tol);
    ASSERT_NEAR(o[1], 0.375f, tol);
    ASSERT_NEAR(o[2], 0.5f, tol);     /* 1.0 / 2.0 - the slice is divided too */

    /* **A 1D lookup is a 2D sample with a zero beside the coordinate**, because a 1D texture is
     * one row of a 2D image and is described to the hardware as exactly that. The simulator
     * reports the two address registers, so the zero is visible rather than assumed: a
     * lowering that left `t` unwritten would report whatever the allocator had, and one that
     * sampled `dim:SQ_RSRC_IMG_1D` would be telling the hardware something the descriptor does
     * not say. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler1D ramp;\n"
                    "varying vec4 vin;\n"
                    "void main() { gl_FragColor = texture1D(ramp, vin.x); }\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);   /* the one coordinate */
    ASSERT_NEAR(o[1], 0.0f, tol);    /* and the zero the descriptor still reads */

    /* Its projective form divides the one coordinate by the last component, whichever width
     * arrived - `vec2` divides by `t` and `vec4` by `q`, which is the same rule the 2D pair
     * follow. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler1D ramp;\n"
                    "varying vec4 vin;\n"
                    "void main() { gl_FragColor = texture1DProj(ramp, vec2(vin.x, 2.0)); }\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.125f, tol);
    ASSERT_NEAR(o[1], 0.0f, tol);

    /* And the 1D shadow, where the address is the reference, the coordinate and the zero. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler1DShadow depth;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = shadow1D(depth, vec3(vin.x, 0.0, 0.25));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, tol);    /* 0.25 <= 0.5 */

    /* **A shadow lookup compares instead of returning a texel**, and the reference it compares
     * is the coordinate's *third* component handed over as the sampler's *first* address
     * register. The simulator stores depth 0.5 and compares less-or-equal, which is the fixture
     * obSCEne measured against - so 0.25 passes and 0.75 fails.
     *
     * **The register order is the thing this is really for.** A lowering that left the
     * reference in place would compare `s` against the stored depth: here `s` is 0.25, so the
     * passing case would still pass and only the failing one would give it away. Both are
     * checked for that reason. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2DShadow depth;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = shadow2D(depth, vec3(vin.x, vin.y, 0.25));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, tol);   /* 0.25 <= 0.5 */
    ASSERT_NEAR(o[1], 1.0f, tol);   /* GL_LUMINANCE spreads it across rgb ... */
    ASSERT_NEAR(o[2], 1.0f, tol);
    ASSERT_NEAR(o[3], 1.0f, tol);   /* ... with alpha 1 */

    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2DShadow depth;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = shadow2D(depth, vec3(vin.x, vin.y, 0.75));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, tol);   /* 0.75 > 0.5, and `s` is 0.25 - so this is the order test */
    ASSERT_NEAR(o[3], 1.0f, tol);

    /* **The reference is clamped to [0, 1]** before it is compared, which GL 1.4 requires. An
     * unclamped -1 compares less-or-equal just as 0 does, so the clamp is invisible here; 2.0
     * is the one that shows it, clamping to 1.0 and still failing against 0.5. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2DShadow depth;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = shadow2D(depth, vec3(vin.x, vin.y, -1.0));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, tol);

    /* And the projective form divides the reference by `q` along with s and t. 0.5/2 is 0.25,
     * which passes where the undivided 0.5... also passes - so the divisor is 4, making the
     * reference 0.125 and `s` 0.0625, and only a divided reference gives 1 here while an
     * undivided 0.5 sits exactly on the boundary. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2DShadow depth;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = shadow2DProj(depth, vec4(vin.x, vin.y, 3.0, 4.0));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, tol);   /* 3/4 = 0.75 > 0.5 */

    /* **A zero divisor answers zero, not an infinity.** The language calls it undefined and the
     * reference picks zero; the two paths agreeing is worth the compare and the select. A
     * reciprocal left unguarded gives `inf`, and `inf * 0.25` is `inf` rather than anything a
     * texture unit can clamp into a sensible texel. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2D tex;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = texture2DProj(tex, vec3(vin.x, vin.y, 0.0));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, tol);
    ASSERT_NEAR(o[1], 0.0f, tol);

    /* **Two samplers take two sets, in declaration order.** A shader that sampled both through
     * set 0 would read one texture twice - which looks like a texture-binding bug and is a
     * compiler one. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2D first;\n"
                    "uniform sampler2D second;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  vec4 a = texture2D(first, vin.xy);\n"
                    "  vec4 b = texture2D(second, vin.xy);\n"
                    "  gl_FragColor = vec4(a.z, b.z, a.x, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, tol);    /* `first` is set 0 ... */
    ASSERT_NEAR(o[1], 1.0f, tol);    /* ... and `second` is set 1 */
    ASSERT_NEAR(o[2], 0.25f, tol);

    /* The result composes with everything else: a sample scaled by a uniform, added to a const,
     * behind a discard. */
    const GLuint prog = linked_program(
        "attribute vec4 pos;\n"
        "varying vec4 vin;\n"
        "void main() { vin = pos; gl_Position = pos; }\n",
        "uniform sampler2D tex;\n"
        "uniform float amount;\n"
        "const vec3 lift = vec3(0.1, 0.0, 0.0);\n"
        "varying vec4 vin;\n"
        "void main() {\n"
        "  vec3 c = texture2D(tex, vin.xy).rgb;\n"
        "  if (c.r > 0.9) { discard; }\n"
        "  gl_FragColor = vec4(c * amount + lift, 1.0);\n"
        "}\n");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    glUniform1f(glGetUniformLocation(prog, "amount"), 2.0f);
    ASSERT_EQ(compile_and_run_prog(ctx, prog, attr, o), GL_TRUE);
    ASSERT_NEAR(o[0], 0.25f * 2.0f + 0.1f, 1e-5f);
    ASSERT_NEAR(o[1], 0.75f * 2.0f, 1e-5f);

    /* And the discard still bites when the sampled value asks for it. */
    const float bright[4][4] = {{0.95f, 0.5f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    ASSERT_EQ(compile_and_run_prog(ctx, prog, bright, o), GL_FALSE);

    glContextDestroy(ctx);
}

/*
 * **The inverse trigonometric functions, measured against the reference rather than trusted.**
 *
 * These were refused because "a polynomial of unmeasured accuracy is not generated". The
 * objection was to a polynomial chosen *here*; the one now emitted is the one `oops_atan2f`
 * already ships, and the software rasteriser answers every `atan` in this SDK through that
 * function. So the expected values below are computed by calling it - not by a second
 * approximation that would have to be right for this test to mean anything.
 *
 * **The tolerance is 1e-5 and it is the reciprocal's.** There is no divide instruction on this
 * part, so `min/max` is `v_rcp_f32` and a multiply, good to one unit in the last place where
 * the reference does a true divide. Everything else - the coefficients, the reduction, the
 * order of the quadrant fixups - is identical, so this is the whole of the difference.
 */
static float ref_asin(float x) {
    if (x <= -1.0f) return -1.57079632679489661923f;
    if (x >= 1.0f) return 1.57079632679489661923f;
    return oops_atan2f(x, oops_sqrtf(1.0f - x * x));
}

static void test_gl2_the_inverse_trig_agrees_with_the_reference(void) {
    void *ctx = gl2_context();
    float o[4];
    const float tol = 1e-5f;

    /* **Across the reduction's seam and both sides of it.** `|y| > |x|` swaps which of the two
     * is the numerator, so 1.0 is the value that has to come out right from either direction,
     * and the signs cover all four quadrant fixups. */
    static const float XS[] = {-8.0f,  -1.5f, -1.0f, -0.6f, -0.25f, 0.0f,
                               0.25f,  0.6f,  1.0f,  1.5f,  8.0f};
    for (size_t i = 0; i < sizeof(XS) / sizeof(XS[0]); i++) {
        const float attr[4][4] = {{XS[i], 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        compile_and_run(ctx, VS_ONE_VARYING,
                        "varying vec4 vin;\n"
                        "void main() { gl_FragColor = vec4(atan(vin.x), 0.0, 0.0, 1.0); }\n",
                        attr, o);
        ASSERT_NEAR(o[0], oops_atan2f(XS[i], 1.0f), tol);
    }

    /* `asin` and `acos`, including **outside the domain**: the language and the reference both
     * answer the endpoint, where an unclamped `sqrt(1 - x*x)` is not a number. */
    for (size_t i = 0; i < sizeof(XS) / sizeof(XS[0]); i++) {
        const float attr[4][4] = {{XS[i], 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        compile_and_run(ctx, VS_ONE_VARYING,
                        "varying vec4 vin;\n"
                        "void main() {\n"
                        "  gl_FragColor = vec4(asin(vin.x), acos(vin.x), 0.0, 1.0);\n"
                        "}\n",
                        attr, o);
        ASSERT_NEAR(o[0], ref_asin(XS[i]), tol);
        ASSERT_NEAR(o[1], 1.57079632679489661923f - ref_asin(XS[i]), tol);
    }

    /* **Two-argument `atan`, which is the one that needs the quadrants.** `atan(y, x)` and
     * `atan(y/x)` differ everywhere `x` is negative, so a lowering that quietly used the
     * one-argument form would pass the sweep above and fail here. */
    static const float YS2[] = {1.0f, 1.0f, -1.0f, -1.0f, 0.0f,  1.0f, 0.0f};
    static const float XS2[] = {1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 0.0f};
    for (size_t i = 0; i < sizeof(YS2) / sizeof(YS2[0]); i++) {
        const float attr[4][4] = {
            {YS2[i], XS2[i], 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        compile_and_run(ctx, VS_ONE_VARYING,
                        "varying vec4 vin;\n"
                        "void main() {\n"
                        "  gl_FragColor = vec4(atan(vin.x, vin.y), 0.0, 0.0, 1.0);\n"
                        "}\n",
                        attr, o);
        ASSERT_NEAR(o[0], oops_atan2f(YS2[i], XS2[i]), tol);
    }

    glContextDestroy(ctx);
}

/* **`refract`, where the interesting half is the ray that does not refract at all.**
 *
 * Total internal reflection returns the zero vector - the specification's own wording, and
 * something a shader leans on to darken a grazing angle. The lowering computes both arms and
 * selects, so the square root of a negative is produced and then discarded: a `v_cndmask` moves
 * a register rather than evaluating anything, and the NaN goes with the arm it belongs to.
 */
static void test_gl2_refract_returns_zero_under_total_internal_reflection(void) {
    void *ctx = gl2_context();
    float o[4];
    const float tol = 1e-6f;
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Straight down onto a flat surface with eta 0.5: d = -1, k = 1 - 0.25*(1-1) = 1, so the
     * ray bends and the answer is `0.5*I - (0.5*(-1) + 1)*N` = (0, -1, 0) for I = (0,-1,0). */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  vec3 r = refract(vec3(0.0, -1.0, 0.0), vec3(0.0, 1.0, 0.0), 0.5);\n"
                    "  gl_FragColor = vec4(r * 0.5 + 0.5, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, tol);
    ASSERT_NEAR(o[1], 0.0f, tol);   /* -1 encoded as 0 */
    ASSERT_NEAR(o[2], 0.5f, tol);

    /* **A grazing ray with eta 2.0, which is total internal reflection**: d is near zero, so
     * `k = 1 - 4*(1 - d*d)` is negative and the whole vector is zero. Encoded the same way, so
     * zero comes back as 0.5 in every channel - and a lowering that let the NaN through would
     * give something that is not 0.5 and not anything else either. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  vec3 i = normalize(vec3(1.0, -0.05, 0.0));\n"
                    "  vec3 r = refract(i, vec3(0.0, 1.0, 0.0), 2.0);\n"
                    "  gl_FragColor = vec4(r * 0.5 + 0.5, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.5f, tol);

    glContextDestroy(ctx);
}

void run_unit_tests_gl2(void) {
    TEST_SUITE_BEGIN("OpenGL 2.0: shaders, programs and generic attributes");
    RUN_TEST(test_gl2_version_gating);
    RUN_TEST(test_gl2_shaders_and_programs_share_one_name_space);
    RUN_TEST(test_gl2_a_name_is_never_reused);
    RUN_TEST(test_gl2_compile_reports_status_and_a_log);
    RUN_TEST(test_gl2_compile_refuses_and_explains);
    RUN_TEST(test_gl2_builtins_are_known_to_the_compiler);
    RUN_TEST(test_gl2_arrays_are_typed_and_bounded);
    RUN_TEST(test_gl2_glsl_120_converts_int_to_float);
    RUN_TEST(test_gl2_link_builds_the_interface);
    RUN_TEST(test_gl2_link_refuses_what_cannot_run);
    RUN_TEST(test_gl2_bind_attrib_location_applies_at_the_next_link);
    RUN_TEST(test_gl2_deletion_is_deferred_and_observable);
    RUN_TEST(test_gl2_a_deleted_program_in_use_keeps_running);
    RUN_TEST(test_gl2_uniforms_take_the_matching_command);
    RUN_TEST(test_gl2_uniform_arrays_step_by_location);
    RUN_TEST(test_gl2_vertex_attrib_state);
    RUN_TEST(test_gl2_limits_and_version_are_answered);
    RUN_TEST(test_gl2_a_program_draws);
    RUN_TEST(test_gl2_structs_run);
    RUN_TEST(test_gl2_uniforms_and_varyings_reach_the_pixels);
    RUN_TEST(test_gl2_a_matrix_uniform_transforms);
    RUN_TEST(test_gl2_discard_writes_nothing);
    RUN_TEST(test_gl2_control_flow_and_functions_run);
    RUN_TEST(test_gl2_a_sampler_reads_its_own_unit);
    RUN_TEST(test_gl2_attribute_arrays_feed_the_shader);
    RUN_TEST(test_gl2_the_reference_runs_globals_and_discard);
    RUN_TEST(test_gl2_frag_coord_and_derivatives);
    RUN_TEST(test_gl2_a_vertex_shader_alone_feeds_fixed_function);
    RUN_TEST(test_gl2_glsl_120_runs_what_it_compiles);
    RUN_TEST(test_gl2_a_runaway_shader_is_stopped);
    RUN_TEST(test_gl2_pixel_shader_encodings_match_the_assembler);
    RUN_TEST(test_gl2_frag_coord_comes_from_the_window_position);
    RUN_TEST(test_gl2_loops_are_unrolled_when_the_count_is_known);
    RUN_TEST(test_gl2_loops_that_branch_run_break_and_continue);
    RUN_TEST(test_gl2_the_probes_control_flow_shaders_compile_and_run);
    RUN_TEST(test_gl2_integer_comparisons_are_the_float_ones);
    RUN_TEST(test_gl2_a_branched_loop_carries_its_trip_guard);
    RUN_TEST(test_gl2_the_back_end_refuses_the_loops_it_cannot_bound);
    RUN_TEST(test_gl2_derivatives_are_quad_reads_under_whole_quad_mode);
    RUN_TEST(test_gl2_frag_depth_exports_before_the_colour);
    RUN_TEST(test_gl2_vector_relationals_reduce_a_bvec);
    RUN_TEST(test_gl2_gl_color_lands_where_the_link_put_it);
    RUN_TEST(test_gl2_matrix_products_are_two_products);
    RUN_TEST(test_gl2_integers_are_floats_kept_whole);
    RUN_TEST(test_gl2_front_facing_is_a_sign_not_a_flag);
    RUN_TEST(test_gl2_user_functions_are_inlined);
    RUN_TEST(test_gl2_matrix_by_matrix_and_by_scalar);
    RUN_TEST(test_gl2_local_arrays_are_indexed_where_the_shader_is_compiled);
    RUN_TEST(test_gl2_arrays_refuse_what_a_register_file_cannot_do);
    RUN_TEST(test_gl2_an_early_return_ends_the_function_and_nothing_else);
    RUN_TEST(test_gl2_the_back_end_refuses_the_calls_it_cannot_inline);
    RUN_TEST(test_gl2_compiles_a_whole_pixel_shader);
    RUN_TEST(test_gl2_the_back_end_refuses_what_it_cannot_encode);
    RUN_TEST(test_gl2_compiled_structs);
    RUN_TEST(test_gl2_compiled_arithmetic_matches_the_language);
    RUN_TEST(test_gl2_compiled_geometry_matches_the_language);
    RUN_TEST(test_gl2_compiled_swizzle_writes_land_where_they_are_named);
    RUN_TEST(test_gl2_compiled_uniforms_come_from_the_scalar_file);
    RUN_TEST(test_gl2_compiled_control_flow_runs_the_right_arm);
    RUN_TEST(test_gl2_compiled_discard_kills_the_lane_for_good);
    RUN_TEST(test_gl2_compiled_globals_are_in_scope);
    RUN_TEST(test_gl2_compiled_texture_lookups_reach_the_right_set);
    RUN_TEST(test_gl2_a_realistic_shader_compiles_and_computes);
    RUN_TEST(test_gl2_the_inverse_trig_agrees_with_the_reference);
    RUN_TEST(test_gl2_refract_returns_zero_under_total_internal_reflection);
    RUN_TEST(test_gl2_separate_stencil_and_blend_state);
    RUN_TEST(test_gl2_separate_blend_equation_blends);
    RUN_TEST(test_gl2_draw_buffers);
    TEST_SUITE_END();
}
