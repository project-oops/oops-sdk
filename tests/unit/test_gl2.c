/*
 * OpenGL 2.0: shader objects, program objects and generic vertex attributes.
 *
 * Separate from `test_gl.c`, which covers fixed-function state and pixels; these cover
 * an object model with a lifetime.
 *
 * The tests target the behaviour ports lean on beyond the happy path: the deferred
 * delete, the location of `-1`, the attribute bound after the link, and the uniform set
 * with the wrong command. Each is written against the specification's wording.
 */

#include "oops/display.h"
#include "src/gl/gl_internal.h"
#include "src/gl/glsl_internal.h"
#include "oops/math.h"
#include "tests/test_common.h"
#include <math.h>

/* A fresh context per test. The object tables live in it, so nothing leaks between
 * tests and the name counter starts at 1 every time, which lets a test assert on a
 * name.
 *
 * A context has the entry points its version defines and no others, and the default is
 * 1.1, so without `glContextSetVersion(2, 0)` every call below is GL_INVALID_OPERATION.
 * `test_gl2_version_gating` checks that gate. */
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

/* Compiles a shader and returns it, requiring the compile to succeed, so a test about
 * linking fails on the link rather than on a typo in the source. */
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

/* Whether a shader compiles, without requiring that it does, for tests about a refusal.
 */
static GLboolean compiles(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    source_of(sh, src);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    return (GLboolean)(ok == GL_TRUE);
}

static const char *const VS_SIMPLE = "uniform mat4 mvp;\n"
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

/* Shaders and programs draw names from one counter, and each call checks the kind. */
static void test_gl2_shaders_and_programs_share_one_name_space(void) {
    void *ctx = gl2_context();

    const GLuint a = glCreateShader(GL_VERTEX_SHADER);
    const GLuint p = glCreateProgram();
    const GLuint b = glCreateShader(GL_FRAGMENT_SHADER);
    ASSERT_TRUE(a != 0u && p != 0u && b != 0u);
    /* The three names are distinct; one counter per kind would make the shader and the
     * program both 1. */
    ASSERT_TRUE(a != p && p != b && a != b);

    ASSERT_EQ(glIsShader(a), GL_TRUE);
    ASSERT_EQ(glIsProgram(a), GL_FALSE);
    ASSERT_EQ(glIsProgram(p), GL_TRUE);
    ASSERT_EQ(glIsShader(p), GL_FALSE);
    /* A name nothing owns is neither, and asking is not an error. */
    ASSERT_EQ(glIsShader(9999u), GL_FALSE);
    ASSERT_EQ(glIsProgram(9999u), GL_FALSE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* A shader operation on a program name is GL_INVALID_OPERATION - the object exists
     * and is the wrong kind - while a dead name is GL_INVALID_VALUE. */
    glCompileShader(p);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glCompileShader(9999u);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

    glCreateShader(GL_TEXTURE_2D);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    glContextDestroy(ctx);
}

/* A deleted object's name is not handed out again. */
static void test_gl2_a_name_is_never_reused(void) {
    void *ctx = gl2_context();
    const GLuint first = glCreateShader(GL_VERTEX_SHADER);
    glDeleteShader(first);
    ASSERT_EQ(glIsShader(first), GL_FALSE);
    const GLuint second = glCreateShader(GL_VERTEX_SHADER);
    /* The slot is free again; the name is not. A stale `first` held by a caller now
     * finds nothing rather than this new object. */
    ASSERT_TRUE(second != first);
    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * Compiling
 * ------------------------------------------------------------------------- */

/* A compile reports through its status and info log, and source queries honour bufSize.
 */
static void test_gl2_compile_reports_status_and_a_log(void) {
    void *ctx = gl2_context();

    GLuint sh = glCreateShader(GL_VERTEX_SHADER);
    GLint status = -1;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);
    glGetShaderiv(sh, GL_SHADER_TYPE, &status);
    ASSERT_EQ(status, (GLint)GL_VERTEX_SHADER);

    /* Compiling with no source fails and says why. It is not a GL error: the call has a
     * status and a log to report through. */
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
    ASSERT_EQ(log_len, 0); /* nothing to say */

    /* The source comes back as it went in, and `bufSize` is honoured to the byte: at
     * most `bufSize` written including the terminator, `length` excluding it. */
    char buf[8];
    GLsizei written = -1;
    memset(buf, 'x', sizeof(buf));
    glGetShaderSource(sh, 8, &written, buf);
    ASSERT_EQ(written, 7);
    ASSERT_EQ(buf[7], '\0');
    ASSERT_TRUE(strncmp(buf, VS_SIMPLE, 7) == 0);

    /* A zero buffer writes nothing, not even a terminator, so a caller can measure with
     * a null pointer. */
    written = -1;
    glGetShaderSource(sh, 0, &written, NULL);
    ASSERT_EQ(written, 0);

    /* New source clears the compile status, which describes the old text. */
    source_of(sh, FS_SIMPLE);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    glContextDestroy(ctx);
}

/* `void` followed by anything but `)` in a parameter list is refused as a void
 * parameter, with every later token still where it was. */
static void test_gl2_a_void_parameter_is_refused(void) {
    void *ctx = gl2_context();
    GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(sh, "float f(void x) { return 1.0; }\n"
                  "void main() { gl_FragColor = vec4(f()); }\n");
    glCompileShader(sh);
    GLint status = -1;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);
    char log[256] = {0};
    glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
    ASSERT_TRUE(strstr(log, "void") != NULL);
    /* `(void)` alone is still an empty list. */
    ASSERT_TRUE(compiles(GL_FRAGMENT_SHADER,
                         "float f(void) { return 1.0; }\n"
                         "void main() { gl_FragColor = vec4(f()); }\n"));
    glContextDestroy(ctx);
}

/* A compile or link refusal fails with a log that names the line or the reason. */
static void test_gl2_compile_refuses_and_explains(void) {
    void *ctx = gl2_context();

    /* A type error the grammar accepts: `vec3 * mat4` is a well-formed binary
     * expression whose dimensions do not meet. */
    GLuint bad = glCreateShader(GL_VERTEX_SHADER);
    source_of(bad, "attribute vec3 pos;\n"
                   "void main() { gl_Position = vec4(pos * mat4(1.0), 1.0); }\n");
    glCompileShader(bad);
    GLint status = -1;
    glGetShaderiv(bad, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);
    char log[256] = {0};
    GLsizei n = 0;
    glGetShaderInfoLog(bad, (GLsizei)sizeof(log), &n, log);
    ASSERT_TRUE(n > 0);
    /* The diagnostic carries the line it happened on. */
    ASSERT_TRUE(log[0] == '2');

    /* 1.10 and 1.20 are the two dialects, each stated explicitly. */
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

    /* Anything later is refused by version rather than compiled as one of them: a 1.30
     * shader uses `in`/`out` and means its integer arithmetic, and compiling it as 1.20
     * would silently change those rules. */
    GLuint v130 = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(v130, "#version 130\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n");
    glCompileShader(v130);
    glGetShaderiv(v130, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* A `gl_` name that is real GLSL and is missing here is named, with the reason;
     * "use of an undeclared name" would read as a typo. `gl_Fog` needs a struct type,
     * which this compiler does not have. */
    GLuint pc = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(pc, "#version 120\nvoid main() { gl_FragColor = vec4(gl_Fog.color); }\n");
    glCompileShader(pc);
    glGetShaderiv(pc, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);
    char pclog[256] = {0};
    glGetShaderInfoLog(pc, (GLsizei)sizeof(pclog), NULL, pclog);
    ASSERT_TRUE(strstr(pclog, "structs") != NULL);

    /* `gl_PointCoord` is a fragment input and compiles. */
    GLuint ok_pc = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(ok_pc, "void main() { gl_FragColor = vec4(gl_PointCoord, 0.0, 1.0); }\n");
    glCompileShader(ok_pc);
    glGetShaderiv(ok_pc, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    /*
     * Its two restrictions are refused at link, and each says why:
     *
     *   - `gl_PointCoord` is texture coordinate 0's interpolant, here and on the part
     * (where the substitution is `SPI_PS_INPUT_CNTL.PT_SPRITE_TEX`), so it cannot share
     * a program with `gl_TexCoord`;
     *   - a point is expanded into its square before the vertex stage, in object space
     * through the inverse MVP, so a vertex shader would recompute four corners from
     * identical attributes and collapse the square.
     */
    {
        /* The vertex-shader refusal first: it shows whether `hw_reads_point_coord` is
         * seen at all. */
        GLuint vs_first = glCreateShader(GL_VERTEX_SHADER);
        source_of(vs_first, "#version 120\nvoid main() { gl_Position = gl_Vertex; }\n");
        glCompileShader(vs_first);
        GLuint pc_own = glCreateShader(GL_FRAGMENT_SHADER);
        source_of(pc_own,
                  "void main() { gl_FragColor = vec4(gl_PointCoord, 0.0, 1.0); }\n");
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
        source_of(clash,
                  "#version 120\nvoid main() {\n"
                  "  gl_FragColor = vec4(gl_PointCoord, 0.0, 1.0) * gl_TexCoord[0];\n"
                  "}\n");
        glCompileShader(clash);
        /* A shader that failed to compile links as a program with no fragment stage,
         * which succeeds and would hide the refusal. */
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

        /* A fragment shader on its own links: the fixed-function vertex stage
         * transforms the expansion as it was built. */
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

/* The built-in functions type-check with the specification's overloads and stage rules.
 */
static void test_gl2_builtins_are_known_to_the_compiler(void) {
    void *ctx = gl2_context();

    /* The genType overloads, the geometric functions and a texture lookup, in one
     * shader. */
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

    /* `dFdx` exists in a fragment shader only; in a vertex shader the specification
     * makes it an error rather than a function that returns zero. */
    GLuint ok = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(ok, "varying float v;\nvoid main() { gl_FragColor = vec4(dFdx(v)); }\n");
    glCompileShader(ok);
    GLint status = -1;
    glGetShaderiv(ok, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    GLuint wrong = glCreateShader(GL_VERTEX_SHADER);
    source_of(wrong,
              "attribute float v;\nvoid main() { gl_Position = vec4(dFdx(v)); }\n");
    glCompileShader(wrong);
    glGetShaderiv(wrong, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* `min(genType, float)` is legal and `min(float, genType)` is not. */
    GLuint scalar_ok = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(
        scalar_ok,
        "varying vec3 v;\nvoid main(){ gl_FragColor = vec4(min(v, 0.5), 1.0); }\n");
    glCompileShader(scalar_ok);
    glGetShaderiv(scalar_ok, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    GLuint scalar_bad = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(
        scalar_bad,
        "varying vec3 v;\nvoid main(){ gl_FragColor = vec4(min(0.5, v), 1.0); }\n");
    glCompileShader(scalar_bad);
    glGetShaderiv(scalar_bad, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* `cross` is vec3 only. */
    GLuint cross_bad = glCreateShader(GL_FRAGMENT_SHADER);
    source_of(cross_bad, "varying vec2 v;\nvoid main(){ gl_FragColor = vec4(cross(v, "
                         "v), 0.0, 1.0); }\n");
    glCompileShader(cross_bad);
    glGetShaderiv(cross_bad, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* The fixed-function built-in uniforms let a shader use the matrix stack. */
    GLuint ff = glCreateShader(GL_VERTEX_SHADER);
    source_of(
        ff,
        "void main() { gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex; }\n");
    glCompileShader(ff);
    glGetShaderiv(ff, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    glContextDestroy(ctx);
}

/* GLSL 1.20 converts int to float implicitly in every context, and 1.10 does not. */
static void test_gl2_glsl_120_converts_int_to_float(void) {
    void *ctx = gl2_context();
    GLint status = -1;

    /* `vec3 * 2` and `clamp(v, 0, 1)` are legal 1.20 and refused by 1.10. */
    static const char *const MIXED =
        "attribute vec3 pos;\n"
        "varying vec3 v;\n"
        "float half_of(float x) { return x * 0.5; }\n"
        "void main() {\n"
        "  float f = 1;\n"               /* initialiser */
        "  f = 2;\n"                     /* assignment */
        "  vec3 scaled = pos * 2;\n"     /* operator, scalar against a vector */
        "  float mixed = 1 + 1.0;\n"     /* operator, scalar against a scalar */
        "  float called = half_of(3);\n" /* argument */
        "  float chosen = (f > 0.0) ? 1 : 2.0;\n" /* the two branches of ?: */
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

    /* The same source is an error in 1.10; converting there would accept shaders the
     * specification rejects. */
    GLuint bad110 = glCreateShader(GL_VERTEX_SHADER);
    source_of(bad110, MIXED);
    glCompileShader(bad110);
    glGetShaderiv(bad110, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* The conversion goes one way: float to int is not implicit in either version. */
    GLuint wrong = glCreateShader(GL_VERTEX_SHADER);
    source_of(wrong,
              "#version 120\n"
              "attribute vec3 pos;\n"
              "void main() { int i = 1.0; gl_Position = vec4(pos, float(i)); }\n");
    glCompileShader(wrong);
    glGetShaderiv(wrong, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* 1.20's qualifiers and its two matrix built-ins, and both refused in a 1.10
     * shader. */
    GLuint quals = glCreateShader(GL_VERTEX_SHADER);
    source_of(quals, "#version 120\n"
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
    source_of(quals110, "#version 110\n"
                        "centroid varying vec3 v;\n"
                        "attribute vec3 pos;\n"
                        "void main() { v = pos; gl_Position = vec4(pos, 1.0); }\n");
    glCompileShader(quals110);
    glGetShaderiv(quals110, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    GLuint tr110 = glCreateShader(GL_VERTEX_SHADER);
    source_of(tr110, "#version 110\n"
                     "uniform mat3 m;\n"
                     "attribute vec3 pos;\n"
                     "void main() { gl_Position = vec4(transpose(m) * pos, 1.0); }\n");
    glCompileShader(tr110);
    glGetShaderiv(tr110, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    glContextDestroy(ctx);
}

/* An array element has the element type, and a constant index past the end is refused.
 */
static void test_gl2_arrays_are_typed_and_bounded(void) {
    void *ctx = gl2_context();

    GLuint ok = glCreateShader(GL_VERTEX_SHADER);
    source_of(ok, "uniform vec4 palette[4];\n"
                  "attribute float which;\n"
                  "void main() { gl_Position = palette[1] * which; }\n");
    glCompileShader(ok);
    GLint status = -1;
    glGetShaderiv(ok, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_TRUE);

    /* An element of a `vec4` array is a vec4, not a float from indexing a vector. */
    GLuint mistyped = glCreateShader(GL_VERTEX_SHADER);
    source_of(mistyped,
              "uniform vec4 palette[4];\n"
              "void main() { float f = palette[1]; gl_Position = vec4(f); }\n");
    glCompileShader(mistyped);
    glGetShaderiv(mistyped, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    /* A constant index past the end is a compile error, not a read of whatever follows.
     */
    GLuint over = glCreateShader(GL_VERTEX_SHADER);
    source_of(over, "uniform vec4 palette[4];\n"
                    "void main() { gl_Position = palette[4]; }\n");
    glCompileShader(over);
    glGetShaderiv(over, GL_COMPILE_STATUS, &status);
    ASSERT_EQ(status, GL_FALSE);

    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * Linking
 * ------------------------------------------------------------------------- */

/* A link builds the active uniform and attribute tables and their locations. */
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

    /* Before the link there are no locations to ask for, and asking is an error rather
     * than a -1 that looks like "no such uniform". */
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
    /* A name nothing declares is -1 and not an error: the specification defines -1
     * so a program need not branch on a uniform the linker removed. */
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

/* A link fails, with a log, on a program whose result would be undefined. */
static void test_gl2_link_refuses_what_cannot_run(void) {
    void *ctx = gl2_context();

    /* A vertex shader that never writes gl_Position. The specification leaves the
     * result undefined; this makes it a link error rather than a blank screen. */
    {
        const GLuint vs = compiled(GL_VERTEX_SHADER, "attribute vec4 pos;\n"
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
        /* A program that did not link cannot be made current, and the failure does not
         * drop the caller back to fixed function behind its back. */
        glUseProgram(prog);
        ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    }

    /* gl_Position written through a helper still counts: the check covers the whole
     * unit, not only `main`. */
    {
        const GLuint vs =
            compiled(GL_VERTEX_SHADER, "attribute vec4 pos;\n"
                                       "void place(vec4 p) { gl_Position = p; }\n"
                                       "void main() { place(pos); }\n");
        const GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glLinkProgram(prog);
        GLint linked = -1;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_TRUE);
    }

    /* A varying the fragment shader reads and the vertex shader does not write is a
     * link error, not a varying that interpolates zero. */
    {
        const GLuint vs =
            compiled(GL_VERTEX_SHADER,
                     "attribute vec4 pos;\nvoid main() { gl_Position = pos; }\n");
        const GLuint fs = compiled(
            GL_FRAGMENT_SHADER, "varying vec3 missing;\n"
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
        const GLuint fs = compiled(
            GL_FRAGMENT_SHADER, "varying vec2 v;\n"
                                "void main() { gl_FragColor = vec4(v, 0.0, 1.0); }\n");
        const GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        GLint linked = -1;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_FALSE);
    }

    /* An attached shader that has not compiled fails the link rather than being left
     * out, which would run the fixed-function stage in its place and report success. */
    {
        const GLuint vs =
            compiled(GL_VERTEX_SHADER,
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

/* glBindAttribLocation takes effect at the next link, not immediately. */
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
    /* The other attribute takes the lowest free slot, which is not the one that was
     * claimed. */
    const GLint col = glGetAttribLocation(prog, "colour");
    ASSERT_TRUE(col >= 0 && col != 5);

    /* A binding after the link does not take effect until the next one. */
    glBindAttribLocation(prog, 7, "pos");
    ASSERT_EQ(glGetAttribLocation(prog, "pos"), 5);
    glLinkProgram(prog);
    ASSERT_EQ(glGetAttribLocation(prog, "pos"), 7);

    /* A binding naming something this shader does not declare is not an error and does
     * not appear, so one binding table can serve several programs. */
    glBindAttribLocation(prog, 9, "absent");
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glLinkProgram(prog);
    ASSERT_EQ(glGetAttribLocation(prog, "absent"), -1);

    /* The language's own prefix cannot be bound: the fixed-function attributes have
     * fixed homes. */
    glBindAttribLocation(prog, 3, "gl_Vertex");
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glBindAttribLocation(prog, OOPS_GL_MAX_VERTEX_ATTRIBS, "pos");
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * Deferred deletion
 * ------------------------------------------------------------------------- */

/* A deleted shader still attached to a program stays queryable until it is detached. */
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

    /* The idiom every GL 2.0 program uses: delete the shaders the moment they are
     * linked. */
    glDeleteShader(vs);
    glDeleteShader(fs);

    /* `glIsShader` says no and `glGetShaderiv` still answers; both are required. */
    ASSERT_EQ(glIsShader(vs), GL_FALSE);
    GLint flagged = -1;
    glGetShaderiv(vs, GL_DELETE_STATUS, &flagged);
    ASSERT_EQ(flagged, GL_TRUE);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* The program is still linked and usable. */
    glUseProgram(prog);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    ASSERT_TRUE(glGetUniformLocation(prog, "mvp") >= 0);

    /* Detaching the last holder reaps it, after which the name is gone
     * and a query on it is GL_INVALID_VALUE rather than a read of a freed object. */
    glDetachShader(prog, vs);
    glGetShaderiv(vs, GL_DELETE_STATUS, &flagged);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

    /* Detaching something that is not attached is GL_INVALID_OPERATION. */
    glDetachShader(prog, fs);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glDetachShader(prog, fs);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE); /* fs has now been reaped too */

    glContextDestroy(ctx);
}

/* A deleted program that is current stays current and linked. */
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
    /* Still current, still linked - the deletion waits for something else to be made
     * current. */
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

/* A uniform accepts only the glUniform command matching its declared type and width. */
static void test_gl2_uniforms_take_the_matching_command(void) {
    void *ctx = gl2_context();

    const GLuint vs =
        compiled(GL_VERTEX_SHADER,
                 "uniform mat4 mvp;\n"
                 "uniform float scale;\n"
                 "uniform int count;\n"
                 "attribute vec4 pos;\n"
                 "void main() { gl_Position = mvp * pos * scale * float(count); }\n");
    const GLuint fs =
        compiled(GL_FRAGMENT_SHADER,
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

    /* Setting a uniform with no program in use is an error, not a value kept for later.
     */
    const GLint scale = glGetUniformLocation(prog, "scale");
    glUniform1f(scale, 2.0f);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

    glUseProgram(prog);
    glUniform1f(scale, 2.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLfloat got = 0.0f;
    glGetUniformfv(prog, scale, &got);
    ASSERT_TRUE(fabsf(got - 2.0f) < 1e-6f);

    /* `glUniform1i` on a float is an error, not a conversion, so a `glUniform1i(loc,
     * 1)` meant for a sampler cannot quietly set a float. */
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

    /* A matrix takes only glUniformMatrix, and `transpose` is applied once, here - so
     * the stored value is column-major whichever way the caller had it. */
    const GLint mvp = glGetUniformLocation(prog, "mvp");
    glUniform4f(mvp, 0.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    GLfloat m[16];
    for (int i = 0; i < 16; i++)
        m[i] = (GLfloat)i;
    glUniformMatrix4fv(mvp, 1, GL_FALSE, m);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLfloat back[16] = {0};
    glGetUniformfv(prog, mvp, back);
    ASSERT_TRUE(fabsf(back[1] - 1.0f) < 1e-6f);
    glUniformMatrix4fv(mvp, 1, GL_TRUE, m);
    glGetUniformfv(prog, mvp, back);
    /* Element [1] of the stored matrix is now element [4] of what was passed. */
    ASSERT_TRUE(fabsf(back[1] - 4.0f) < 1e-6f);

    /* A sampler takes the integer form only: setting one with a float would be a unit
     * number that is nearly an integer. */
    const GLint tex = glGetUniformLocation(prog, "tex");
    glUniform1f(tex, 1.0f);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    glUniform1i(tex, 1);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLint unit = -1;
    glGetUniformiv(prog, tex, &unit);
    ASSERT_EQ(unit, 1);

    /* A location of -1 is silently ignored, by definition, so a program need not
     * branch on a uniform the linker removed. */
    glUniform1f(-1, 5.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* Relinking resets every uniform to zero. */
    glLinkProgram(prog);
    glUseProgram(prog);
    glGetUniformfv(prog, glGetUniformLocation(prog, "scale"), &got);
    ASSERT_TRUE(fabsf(got) < 1e-6f);

    glContextDestroy(ctx);
}

/* A uniform array takes one location per element, and writes stop at its end. */
static void test_gl2_uniform_arrays_step_by_location(void) {
    void *ctx = gl2_context();

    const GLuint vs = compiled(
        GL_VERTEX_SHADER, "uniform vec4 palette[3];\n"
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

    /* The uniform declared after the array does not collide with its elements, as
     * numbering locations per uniform rather than per element would make it. */
    const GLint after = glGetUniformLocation(prog, "after");
    ASSERT_TRUE(after >= base + 3);

    const GLfloat three[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    glUniform4fv(base, 3, three);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    GLfloat got[4] = {0};
    glGetUniformfv(prog, base + 1, got);
    ASSERT_TRUE(fabsf(got[1] - 1.0f) < 1e-6f);

    /* Starting part-way and running past the end: the excess is ignored, and what was
     * in range still landed. The uniform after the array is untouched. */
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

/* Current generic attribute values: initial value, short forms, normalisation, limits.
 */
static void test_gl2_vertex_attrib_state(void) {
    void *ctx = gl2_context();

    /* The initial current value is (0, 0, 0, 1), a point rather than a direction, so an
     * attribute nothing has set lands at the origin under a transform rather than being
     * degenerate. */
    GLfloat cur[4] = {9, 9, 9, 9};
    glGetVertexAttribfv(3, GL_CURRENT_VERTEX_ATTRIB, cur);
    ASSERT_TRUE(cur[0] == 0.0f && cur[1] == 0.0f && cur[2] == 0.0f && cur[3] == 1.0f);

    /* And the short forms fill rather than leave: a 2f after a 4f gives z = 0 and w =
     * 1, not whatever the 4f left behind. */
    glVertexAttrib4f(3, 1.0f, 2.0f, 3.0f, 4.0f);
    glVertexAttrib2f(3, 5.0f, 6.0f);
    glGetVertexAttribfv(3, GL_CURRENT_VERTEX_ATTRIB, cur);
    ASSERT_TRUE(cur[0] == 5.0f && cur[1] == 6.0f && cur[2] == 0.0f && cur[3] == 1.0f);

    /* The normalised forms scale and the plain ones convert. */
    const GLubyte bytes[4] = {255, 128, 0, 255};
    glVertexAttrib4ubv(4, bytes);
    glGetVertexAttribfv(4, GL_CURRENT_VERTEX_ATTRIB, cur);
    ASSERT_TRUE(fabsf(cur[0] - 255.0f) < 1e-6f);
    glVertexAttrib4Nubv(4, bytes);
    glGetVertexAttribfv(4, GL_CURRENT_VERTEX_ATTRIB, cur);
    ASSERT_TRUE(fabsf(cur[0] - 1.0f) < 1e-6f);

    /* A signed byte of -128 divides by 127 and is clamped to -1, the specification's
     * rule, not a divide by 128. */
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

    /* An index past the limit is GL_INVALID_VALUE rather than a write past the table.
     */
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

/* The GL 2.0 limits and the shading language version report what exists. */
static void test_gl2_limits_and_version_are_answered(void) {
    void *ctx = gl2_context();

    GLint v = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &v);
    ASSERT_EQ(v, OOPS_GL_MAX_VERTEX_ATTRIBS);
    ASSERT_TRUE(v >= 16); /* GL 2.0's own minimum */
    glGetIntegerv(GL_MAX_VARYING_FLOATS, &v);
    ASSERT_EQ(v, OOPS_GL_MAX_VARYING_FLOATS);
    ASSERT_TRUE(v >= 32);
    /* The samplers a shader may name, which is not the fixed-function stage count. */
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &v);
    ASSERT_EQ(v, OOPS_GL_MAX_TEXTURE_IMAGE_UNITS);
    ASSERT_TRUE(v >= 2);
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &v);
    /* Zero is GL 2.0's minimum, and a vertex shader here has no sampler. Reporting more
     * would send a program down a path that then fails. */
    ASSERT_EQ(v, 0);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &v);
    ASSERT_TRUE(v >= 1);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* The shading language's version is its own string, separate from GL_VERSION. */
    const GLubyte *sl = glGetString(GL_SHADING_LANGUAGE_VERSION);
    ASSERT_TRUE(sl != NULL);
    /* The highest dialect the front end takes, as the specification asks, not the one
     * the GL version pairs with. It is a ceiling, not a mode: a shader may still
     * declare
     * `#version 110` and be held to 1.10's rules. */
    ASSERT_TRUE(strncmp((const char *)sl, "1.20", 4) == 0);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * Drawing
 *
 * From here on the checks are pixels: a program that links and reports every location
 * correctly can still draw the wrong colour. The reference is the software path, which
 * is oops-gl's definition of the answer.
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
    glContextSetVersion(2,
                        0); /* see gl2_context: without this there is no GL 2.0 here */
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

/* The framebuffer's row 0 is the top; GL's window y counts up from the bottom. These
 * take the rasteriser's own orientation, which is what `fb` is. */
static uint32_t px(const gl2_target_t *t, int x, int y) {
    return t->fb[y * GL2_W + x];
}
static int px_r(uint32_t p) {
    return (int)((p >> 16) & 0xffu);
}
static int px_g(uint32_t p) {
    return (int)((p >> 8) & 0xffu);
}
static int px_b(uint32_t p) {
    return (int)(p & 0xffu);
}

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
    const GLfloat corners[4][3] = {{-0.8f, -0.8f, 0.0f},
                                   {0.8f, -0.8f, 0.0f},
                                   {0.8f, 0.8f, 0.0f},
                                   {-0.8f, 0.8f, 0.0f}};
    const int tri[6] = {0, 1, 2, 0, 2, 3};
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 6; i++) {
        const int c = tri[i];
        glVertexAttrib3f((GLuint)loc, corners[c][0], corners[c][1], z);
        /* The position is the attribute; glVertex pushes the vertex, and its value is
         * unused by these shaders. */
        glVertex3f(corners[c][0], corners[c][1], z);
    }
    glEnd();
}

/* A struct local in a loop body gives its storage back each iteration, so 300
 * iterations of an 8-float struct fit in an invocation's arena. */
static void test_gl2_struct_locals_are_freed_at_scope_end(void) {
    gl2_target_t t = gl2_target();
    const GLuint prog = linked_program(
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
        "struct S { vec4 a; vec4 b; };\n"
        "void main() {\n"
        "  float c = 0.0;\n"
        "  for (int i = 0; i < 300; i++) { S s; s.a = vec4(0.001); c += s.a.x; }\n"
        "  gl_FragColor = vec4(c, 0.0, 0.0, 1.0);\n"
        "}\n");
    ASSERT_TRUE(prog != 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog);
    draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
    const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(p) > 70 && px_r(p) < 84); /* 0.3 */
    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/*
 * Struct members run from where sema placed them; a misplaced member gives a colour,
 * not an error. Each shader puts a different member into a different channel, so a
 * layout off by one comes back rotated.
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
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);   /* 0.25 */
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136); /* 0.50 */
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200); /* 0.75 */
    }

    /* A member that is a vector, and a swizzle of it: the two meanings of `.` in one
     * line. */
    {
        const GLuint prog = linked_program(
            "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
            "struct M { float a; vec3 v; };\n"
            "void main() {\n"
            "  M m = M(0.0, vec3(0.25, 0.5, 0.75));\n"
            "  gl_FragColor = vec4(m.v.x, m.v.y, m.v.z, 1.0);\n"
            "}\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136);
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200);
    }

    /* Assignment copies the whole struct, not its first component, and writing to the
     * copy does not disturb the original. */
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
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72); /* a.r still 0.25 */
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
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);   /* 0.5  * 0.5 = 0.25 */
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136); /* 1.0  * 0.5 = 0.50 */
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200); /* 1.5  * 0.5 = 0.75 */
    }

    /* A struct parameter, the shape gl2-probe's `structs/function` arm draws. Declaring
     * it needs the struct-aware type; otherwise `declare` refuses it and `main` draws
     * nothing. The clear matters: every case here draws the same colour, so a case that
     * drew nothing would pass on the previous case's pixel. */
    {
        const GLuint prog = linked_program(
            "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
            "struct C { float r; float g; float b; };\n"
            "uniform float src;\n"
            "C half_of(C c) { return C(c.r * 0.5, c.g * 0.5, c.b * 0.5); }\n"
            "void main() {\n"
            "  C c = half_of(C(src, src * 2.0, src * 3.0));\n"
            "  gl_FragColor = vec4(c.r, c.g, c.b, 1.0);\n"
            "}\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        glUniform1f(glGetUniformLocation(prog, "src"), 0.5f);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136);
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200);
    }

    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* Framebuffer and renderbuffer objects: names, attachments, queries and completeness.
 */
static void test_gl2_framebuffer_objects(void) {
    gl2_target_t t = gl2_target();

    /* Names: distinct, non-zero, live until deleted. */
    GLuint fb[2] = {0, 0};
    glGenFramebuffers(2, fb);
    ASSERT_TRUE(fb[0] != 0 && fb[1] != 0 && fb[0] != fb[1]);
    ASSERT_TRUE(glIsFramebuffer(fb[0]) == GL_TRUE);
    ASSERT_TRUE(glIsFramebuffer(fb[0] + 1000u) == GL_FALSE);

    /* A name that was never generated is refused, the ES rule: binding it does not
     * create one, so a program with a stale name cannot draw somewhere it does not own.
     */
    while (glGetError() != GL_NO_ERROR) {
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fb[0] + 1000u);
    ASSERT_TRUE(glGetError() == GL_INVALID_OPERATION);

    glBindFramebuffer(GL_FRAMEBUFFER, fb[0]);
    GLint bound = -1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    ASSERT_TRUE((GLuint)bound == fb[0]);

    /* Nothing attached yet. */
    ASSERT_TRUE(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);

    /* A renderbuffer with a name but no storage: attached, and not renderable. */
    GLuint rb[2] = {0, 0};
    glGenRenderbuffers(2, rb);
    ASSERT_TRUE(rb[0] != 0 && rb[1] != 0 && rb[0] != rb[1]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              rb[0]);
    ASSERT_TRUE(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);

    /* The queries answer the declared format, not the storage: RGB565 is one word a
     * sample here like everything else, but reports 5/6/5 bits. */
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGB565, 32, 32);
    GLint v = -1;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &v);
    ASSERT_TRUE(v == 32);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_RED_SIZE, &v);
    ASSERT_TRUE(v == 5);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_GREEN_SIZE, &v);
    ASSERT_TRUE(v == 6);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_BLUE_SIZE, &v);
    ASSERT_TRUE(v == 5);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_ALPHA_SIZE, &v);
    ASSERT_TRUE(v == 0);

    /* The attachment reads back as what it is. */
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &v);
    ASSERT_TRUE(v == GL_RENDERBUFFER);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &v);
    ASSERT_TRUE((GLuint)v == rb[0]);

    /* An empty attachment point has a type and no name, and asking for the name is an
     * error rather than a zero; a program asks the type first. */
    while (glGetError() != GL_NO_ERROR) {
    }
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &v);
    ASSERT_TRUE(v == GL_NONE);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &v);
    ASSERT_TRUE(glGetError() == GL_INVALID_ENUM);

    /* ES 2.0 requires every attachment to be the same size. */
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16_ARB, 16, 16);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              rb[1]);
    ASSERT_TRUE(glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS);

    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16_ARB, 32, 32);
    ASSERT_TRUE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    /* Deleting a renderbuffer detaches it everywhere. */
    glDeleteRenderbuffers(1, &rb[0]);
    ASSERT_TRUE(glIsRenderbuffer(rb[0]) == GL_FALSE);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &v);
    ASSERT_TRUE(v == GL_NONE);

    /* Deleting the bound framebuffer binds 0, the display, in its place. */
    glDeleteFramebuffers(1, &fb[0]);
    ASSERT_TRUE(glIsFramebuffer(fb[0]) == GL_FALSE);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    ASSERT_TRUE(bound == 0);
    /* And the window-system framebuffer is always complete. */
    ASSERT_TRUE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glDeleteFramebuffers(1, &fb[1]);
    glDeleteRenderbuffers(1, &rb[1]);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* A cube-face attachment is sized from the face it names, not from face +X. */
static void test_gl2_cube_face_attachment_is_sized_from_its_face(void) {
    gl2_target_t t = gl2_target();
    GLuint tex = 0, fb = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, GL_RGBA, 4, 4, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_Y, 0, GL_RGBA, 8, 8, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_Y, tex, 0);
    GLuint rb = 0;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16_ARB, 8, 8);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
    ASSERT_TRUE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteRenderbuffers(1, &rb);
    glDeleteTextures(1, &tex);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/*
 * Function overloading (GLSL 1.10) resolves each call to the right signature, as in
 * mesa-demos' `simplex-noise.glsl`, which declares `permute` four times. Every overload
 * returns a different value, so a wrong resolution gives a wrong colour.
 */
static void test_gl2_function_overloading(void) {
    gl2_target_t t = gl2_target();

    const GLuint prog = linked_program(
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
        /* By parameter type, by parameter count, and a struct against a vector. */
        "struct S { float v; };\n"
        "float pick(float a) { return 0.25; }\n"
        "float pick(vec2 a) { return 0.5; }\n"
        "float pick(vec2 a, float b) { return 0.75; }\n"
        "float pick(S a) { return 1.0; }\n"
        "void main() {\n"
        "  gl_FragColor = vec4(pick(1.0), pick(vec2(1.0)), pick(vec2(1.0), 1.0),\n"
        "                      pick(S(1.0)));\n"
        "}\n");
    ASSERT_TRUE(prog != 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog);
    draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
    const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);   /* pick(float) */
    ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136); /* pick(vec2) */
    ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200); /* pick(vec2, float) */

    /* An exact match beats one that needs a conversion. Under 1.20 both overloads below
     * accept `two(1)`, and the `int` one is chosen. */
    const GLuint prog2 = linked_program(
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
        "#version 120\n"
        "float two(int a) { return 0.25; }\n"
        "float two(float a) { return 0.5; }\n"
        "void main() { gl_FragColor = vec4(two(1), two(1.0), 0.75, 1.0); }\n");
    ASSERT_TRUE(prog2 != 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog2);
    draw_quad(glGetAttribLocation(prog2, "pos"), 0.0f);
    const uint32_t q = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(q) > 55 && px_r(q) < 72);   /* two(int) */
    ASSERT_TRUE(px_g(q) > 120 && px_g(q) < 136); /* two(float) */

    /* What overloading must not let through. */
    const char *const bad[] = {
        /* Differing only in return type: a call could not choose, since the arguments
         * are all it offers. */
        "float f(float a) { return 1.0; }\n"
        "vec2 f(float a) { return vec2(1.0); }\n"
        "void main() { gl_FragColor = vec4(f(1.0)); }\n",
        /* A function and a variable of one name is still a redeclaration. */
        "float g;\nfloat g(float a) { return a; }\n"
        "void main() { gl_FragColor = vec4(g(1.0)); }\n",
        /* No overload takes these arguments. */
        "float h(float a) { return a; }\n"
        "float h(vec2 a) { return a.x; }\n"
        "void main() { gl_FragColor = vec4(h(vec3(1.0))); }\n",
    };
    for (int i = 0; i < 3; i++) {
        const GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(sh, 1, &bad[i], (const GLint *)0);
        glCompileShader(sh);
        GLint status = 1;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
        ASSERT_TRUE(status == 0);
        glDeleteShader(sh);
    }

    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/*
 * An array's length is an integral constant expression, not only a literal
 * (`const int N = 8; uniform vec2 offs[N];`, as in mesa-demos' `vpglsl`). A length that
 * cannot be folded is refused rather than guessed, and each accepted case reads a value
 * that depends on the length, so a wrongly folded length fails.
 */
static void test_gl2_array_length_constant_expressions(void) {
    gl2_target_t t = gl2_target();

    /* A `const int`, and arithmetic over one. `v[3]` is the last element of a 4-long
     * array and out of bounds for a 3-long one, so the length must be exact. */
    const GLuint prog = linked_program(
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
        "const int N = 2;\n"
        "const int WIDE = N * 2;\n"
        "uniform float vals[WIDE];\n"
        "void main() {\n"
        "  float a[N + 1];\n"
        "  a[0] = 0.25; a[1] = 0.5; a[2] = 0.75;\n"
        "  gl_FragColor = vec4(a[0], a[1], a[2] * vals[3], 1.0);\n"
        "}\n");
    ASSERT_TRUE(prog != 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog);
    glUniform1f(glGetUniformLocation(prog, "vals[3]"), 1.0f);
    draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
    const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
    /* The uniform's element resolves by name, which only happens if the linker saw a
     * length. */
    ASSERT_TRUE(glGetUniformLocation(prog, "vals[3]") >= 0);
    ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);
    ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136);
    ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200);

    /* Refused: a non-constant length, and a length that folds to zero. */
    const char *const bad[] = {
        "uniform int n;\nvoid main() { float a[n]; a[0] = 1.0; gl_FragColor = "
        "vec4(a[0]); }\n",
        "const int Z = 2 - 2;\nfloat a[Z];\nvoid main() { gl_FragColor = vec4(a[0]); "
        "}\n",
    };
    for (int i = 0; i < 2; i++) {
        const GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(sh, 1, &bad[i], (const GLint *)0);
        glCompileShader(sh);
        GLint status = 1;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
        ASSERT_TRUE(status == 0);
        glDeleteShader(sh);
    }

    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/*
 * GLSL ES 1.00 (`#version 100`) compiles, and dropping its precision qualifiers leaves
 * the shader's meaning intact. This GL computes in single precision throughout, so the
 * qualifiers have no effect. All three spellings are covered because each is parsed in
 * a different place: the `precision` statement, a qualifier between storage qualifier
 * and type, and one on a function parameter.
 */
static void test_gl2_es_100_shaders(void) {
    gl2_target_t t = gl2_target();

    const GLuint prog =
        linked_program("#version 100\n"
                       "precision highp float;\n"
                       "attribute mediump vec3 pos;\n"
                       "void main() { gl_Position = vec4(pos, 1.0); }\n",
                       "#version 100\n"
                       "precision mediump float;\n"
                       "uniform lowp float scale;\n"
                       "mediump float doubled(lowp float v) { return v * 2.0; }\n"
                       "void main() {\n"
                       "  mediump float g = doubled(0.25);\n"
                       "  gl_FragColor = vec4(scale, g, 0.75, 1.0);\n"
                       "}\n");
    ASSERT_TRUE(prog != 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog);
    glUniform1f(glGetUniformLocation(prog, "scale"), 0.25f);
    draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
    const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);   /* the uniform, 0.25 */
    ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136); /* 0.25 doubled through a parameter */
    ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200); /* 0.75 */

    /* A version this compiler does not implement is still refused. 3.30 is the one
     * SuperTux ships beside its ES shaders. */
    const GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
    const char *src330 = "#version 330\nout vec4 c;\nvoid main() { c = vec4(1.0); }\n";
    glShaderSource(sh, 1, &src330, (const GLint *)0);
    glCompileShader(sh);
    GLint status = 1;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    ASSERT_TRUE(status == 0);
    glDeleteShader(sh);

    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/*
 * A draw into a framebuffer object lands in the attachment and not on the display. Both
 * halves are asserted: the attachment holds the drawn colour and the display keeps its
 * clear. The attachment is a different size from the display, so a redirection that
 * moved the pointer but not the width would write into the wrong rows.
 */
static void test_gl2_draw_into_a_framebuffer_object(void) {
    gl2_target_t t = gl2_target();

    /* The display, cleared to a colour nothing else uses. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ASSERT_TRUE((px(&t, GL2_W / 2, GL2_H / 2) & 0xffffffu) == 0x0000ffu);

    GLuint fb = 0, rb = 0;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &rb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 32, 32);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              rb);
    ASSERT_TRUE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    /* Clear the attachment green, and read it back through glReadPixels, which reads
     * the bound colour buffer. */
    glViewport(0, 0, 32, 32);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    uint8_t got[4] = {0, 0, 0, 0};
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
    ASSERT_TRUE(got[0] == 0 && got[1] == 255 && got[2] == 0);

    /* The display was not touched. */
    ASSERT_TRUE((px(&t, GL2_W / 2, GL2_H / 2) & 0xffffffu) == 0x0000ffu);

    /* Unbinding puts the display back, at its own size. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, GL2_W, GL2_H);
    ASSERT_TRUE((px(&t, GL2_W / 2, GL2_H / 2) & 0xffffffu) == 0x0000ffu);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ASSERT_TRUE((px(&t, GL2_W / 2, GL2_H / 2) & 0xffffffu) == 0xff0000u);
    /* The corner furthest from the origin, which only a full-size target reaches. */
    ASSERT_TRUE((px(&t, GL2_W - 1, GL2_H - 1) & 0xffffffu) == 0xff0000u);

    glDeleteFramebuffers(1, &fb);
    glDeleteRenderbuffers(1, &rb);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/*
 * `glBlitFramebuffer` reads the read binding and writes the draw binding, as
 * libultraship uses it (`gfx_opengl.cpp:907`). Two colours in the source catch a copy
 * that smears one pixel; an inverted destination catches a dropped flip; and a read
 * binding distinct from the draw binding catches a blit that reads the draw target.
 */
static void test_gl2_blit_framebuffer_reads_the_read_binding(void) {
    gl2_target_t t = gl2_target();

    GLuint src_fb = 0, src_rb = 0, dst_fb = 0, dst_rb = 0;
    glGenFramebuffers(1, &src_fb);
    glGenRenderbuffers(1, &src_rb);
    glBindFramebuffer(GL_FRAMEBUFFER, src_fb);
    glBindRenderbuffer(GL_RENDERBUFFER, src_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 8, 8);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              src_rb);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              (GLenum)GL_FRAMEBUFFER_COMPLETE);

    /* Bottom half red, top half green, so a flip and a smeared copy are both visible.
     * Scissored clears rather than draws, since clearing an attachment is covered by
     * `test_gl2_draw_into_a_framebuffer_object`. */
    glViewport(0, 0, 8, 8);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 4, 8, 4);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    glGenFramebuffers(1, &dst_fb);
    glGenRenderbuffers(1, &dst_rb);
    glBindFramebuffer(GL_FRAMEBUFFER, dst_fb);
    glBindRenderbuffer(GL_RENDERBUFFER, dst_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 8, 8);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              dst_rb);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              (GLenum)GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, 8, 8);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* The split: read from one, draw to the other. */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fb);
    glBlitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    uint8_t lo[4] = {0, 0, 0, 0}, hi[4] = {0, 0, 0, 0};
    glReadPixels(4, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lo);
    glReadPixels(4, 6, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, hi);
    /* Blue anywhere means the blit did not happen; both the same means it smeared. */
    ASSERT_TRUE(lo[0] == 255 && lo[1] == 0 && lo[2] == 0);
    ASSERT_TRUE(hi[0] == 0 && hi[1] == 255 && hi[2] == 0);

    /* Inverted destination y flips the image. */
    glBlitFramebuffer(0, 0, 8, 8, 0, 8, 8, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadPixels(4, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lo);
    glReadPixels(4, 6, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, hi);
    ASSERT_TRUE(lo[0] == 0 && lo[1] == 255 && lo[2] == 0);
    ASSERT_TRUE(hi[0] == 255 && hi[1] == 0 && hi[2] == 0);

    /* `GL_FRAMEBUFFER` binds both read and draw: after this a blit reads the
     * destination onto itself, so the picture survives unchanged. */
    glBindFramebuffer(GL_FRAMEBUFFER, dst_fb);
    glBlitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadPixels(4, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, lo);
    ASSERT_TRUE(lo[0] == 0 && lo[1] == 255 && lo[2] == 0);

    /* Multisampling is refused rather than downgraded. One sample is all this
     * rasterises, so a request for four is `GL_INVALID_OPERATION` and the storage is
     * left alone. */
    while (glGetError() != GL_NO_ERROR) {
    }
    glBindRenderbuffer(GL_RENDERBUFFER, dst_rb);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_RGBA8, 8, 8);
    ASSERT_EQ(glGetError(), (GLenum)GL_INVALID_OPERATION);
    /* One sample is the request `glRenderbufferStorage` answers, and is accepted. */
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, 1, GL_RGBA8, 8, 8);
    ASSERT_EQ(glGetError(), (GLenum)GL_NO_ERROR);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &src_fb);
    glDeleteFramebuffers(1, &dst_fb);
    glDeleteRenderbuffers(1, &src_rb);
    glDeleteRenderbuffers(1, &dst_rb);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/*
 * `glGenerateMipmap` builds each level by averaging, checked by reading the levels
 * back. The 4x4 image is four uniform 2x2 blocks of different red, so a wrong row
 * stride shows in level 1; the 2x2 image's single level-1 texel is the mean of four
 * unequal values.
 */
static void test_gl2_generate_mipmap(void) {
    gl2_target_t t = gl2_target();

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    uint8_t img[4 * 4 * 4];
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            const uint8_t r = (uint8_t)(40 + 40 * ((y / 2) * 2 + (x / 2)));
            uint8_t *p = &img[(y * 4 + x) * 4];
            p[0] = r;
            p[1] = 0;
            p[2] = 0;
            p[3] = 255;
        }
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);

    uint8_t lvl1[2 * 2 * 4];
    memset(lvl1, 0xab, sizeof(lvl1));
    glGetTexImage(GL_TEXTURE_2D, 1, GL_RGBA, GL_UNSIGNED_BYTE, lvl1);
    ASSERT_TRUE(lvl1[0 * 4] == 40);
    ASSERT_TRUE(lvl1[1 * 4] == 80);
    ASSERT_TRUE(lvl1[2 * 4] == 120);
    ASSERT_TRUE(lvl1[3 * 4] == 160);
    /* The chain runs to 1x1, and that texel is the mean of the four above. */
    uint8_t lvl2[4];
    memset(lvl2, 0xab, sizeof(lvl2));
    glGetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, lvl2);
    ASSERT_TRUE(lvl2[0] == 100); /* (40 + 80 + 120 + 160 + 2) / 4 */

    /* The averaging arm: four unequal texels, one result. */
    GLuint tex2 = 0;
    glGenTextures(1, &tex2);
    glBindTexture(GL_TEXTURE_2D, tex2);
    const uint8_t quad[2 * 2 * 4] = {
        10, 0, 0, 255, 20, 0, 0, 255, 30, 0, 0, 255, 41, 0, 0, 255,
    };
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, quad);
    glGenerateMipmap(GL_TEXTURE_2D);
    uint8_t one[4];
    memset(one, 0xab, sizeof(one));
    glGetTexImage(GL_TEXTURE_2D, 1, GL_RGBA, GL_UNSIGNED_BYTE, one);
    ASSERT_TRUE(one[0] == 25); /* (10 + 20 + 30 + 41 + 2) / 4 = 25 */
    ASSERT_TRUE(one[3] == 255);

    /* No base image is an error, not an empty chain. */
    GLuint tex3 = 0;
    glGenTextures(1, &tex3);
    glBindTexture(GL_TEXTURE_2D, tex3);
    while (glGetError() != GL_NO_ERROR) {
    }
    glGenerateMipmap(GL_TEXTURE_2D);
    ASSERT_TRUE(glGetError() == GL_INVALID_OPERATION);

    glDeleteTextures(1, &tex);
    glDeleteTextures(1, &tex2);
    glDeleteTextures(1, &tex3);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* A linked program draws its colour, and glUseProgram(0) returns to fixed function. */
static void test_gl2_a_program_draws(void) {
    gl2_target_t t = gl2_target();

    const GLuint prog =
        linked_program("attribute vec3 pos;\n"
                       "void main() { gl_Position = vec4(pos, 1.0); }\n",
                       "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    ASSERT_TRUE(loc >= 0);
    glUseProgram(prog);

    draw_quad(loc, 0.0f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* The middle is the shader's green, and a corner outside the quad is still the
     * clear. */
    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(mid) < 8 && px_g(mid) > 247 && px_b(mid) < 8);
    ASSERT_EQ(px(&t, 1, 1), 0xff000000u);

    /* glUseProgram(0) goes back to the fixed-function pipeline, which a program that
     * draws its HUD with glBegin after its scene depends on. */
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

/* Uniform values and interpolated varyings reach the pixels. */
static void test_gl2_uniforms_and_varyings_reach_the_pixels(void) {
    gl2_target_t t = gl2_target();

    /* A varying carrying the position and a uniform scaling it, so the picture is a
     * gradient whose two ends differ. */
    const GLuint prog =
        linked_program("uniform float scale;\n"
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

    /* uv.x runs 0 at the left edge of the quad to 1 at the right, so the left half is
     * darker in red than the right half and the middle is about half. */
    const uint32_t left = px(&t, GL2_W / 4, GL2_H / 2);
    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2);
    const uint32_t right = px(&t, 3 * GL2_W / 4, GL2_H / 2);
    ASSERT_TRUE(px_r(left) < px_r(mid));
    ASSERT_TRUE(px_r(mid) < px_r(right));
    ASSERT_TRUE(px_r(mid) > 110 && px_r(mid) < 145);
    /* uv.y is the other axis, so the same three pixels share a green. */
    ASSERT_TRUE(px_g(left) == px_g(mid) && px_g(mid) == px_g(right));
    ASSERT_TRUE(px_b(mid) == 0);

    /* Changing the uniform changes the picture without relinking. */
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform1f(glGetUniformLocation(prog, "scale"), 0.0f);
    draw_quad(loc, 0.0f);
    const uint32_t flat = px(&t, GL2_W / 4, GL2_H / 2);
    const uint32_t flat2 = px(&t, 3 * GL2_W / 4, GL2_H / 2);
    ASSERT_EQ(px_r(flat), px_r(flat2));
    ASSERT_TRUE(px_r(flat) > 120 && px_r(flat) < 135); /* the constant 0.5 */

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* A mat4 uniform is column-major, and the transpose flag is applied. */
static void test_gl2_a_matrix_uniform_transforms(void) {
    gl2_target_t t = gl2_target();

    /* A translation in the last column moves the quad; taking the matrix as rows would
     * move it along the wrong axis. */
    const GLuint prog =
        linked_program("uniform mat4 mvp;\n"
                       "attribute vec3 pos;\n"
                       "void main() { gl_Position = mvp * vec4(pos, 1.0); }\n",
                       "void main() { gl_FragColor = vec4(1.0); }\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    glUseProgram(prog);

    /* Column-major: element 12 is the x translation. */
    GLfloat m[16] = {0.25f, 0, 0, 0, 0, 0.25f, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1};
    glUniformMatrix4fv(glGetUniformLocation(prog, "mvp"), 1, GL_FALSE, m);
    draw_quad(loc, 0.0f);

    /* A quarter-size quad shifted right: lit right of centre, dark to the left. */
    ASSERT_TRUE(px_r(px(&t, GL2_W / 2 + 12, GL2_H / 2)) > 247);
    ASSERT_EQ(px(&t, GL2_W / 2 - 12, GL2_H / 2), 0xff000000u);

    /* With `transpose` the same numbers mean the other matrix, which moves it in y. */
    glClear(GL_COLOR_BUFFER_BIT);
    glUniformMatrix4fv(glGetUniformLocation(prog, "mvp"), 1, GL_TRUE, m);
    draw_quad(loc, 0.0f);
    ASSERT_EQ(px(&t, GL2_W / 2 + 12, GL2_H / 2), 0xff000000u);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* A discarded fragment writes neither colour nor depth. */
static void test_gl2_discard_writes_nothing(void) {
    gl2_target_t t = gl2_target();

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Discards the left half. The second draw below checks that no depth was written
     * there either. */
    const GLuint prog =
        linked_program("attribute vec3 pos;\n"
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

    /* A second, further quad. On the right the first one's depth blocks it; on the left
     * the discarded fragments left the depth buffer alone, so it draws. */
    const GLuint flat =
        linked_program("attribute vec3 pos;\n"
                       "void main() { gl_Position = vec4(pos, 1.0); }\n",
                       "void main() { gl_FragColor = vec4(1.0, 1.0, 0.0, 1.0); }\n");
    const GLint loc2 = glGetAttribLocation(flat, "pos");
    glUseProgram(flat);
    draw_quad(loc2, 0.5f);

    ASSERT_TRUE(px_b(px(&t, 3 * GL2_W / 4, GL2_H / 2)) > 247); /* still blue */
    const uint32_t left = px(&t, GL2_W / 4, GL2_H / 2);
    ASSERT_TRUE(px_r(left) > 247 && px_g(left) > 247 && px_b(left) < 8);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* Loops, `continue`, `out` parameters and built-ins run in the interpreter. */
static void test_gl2_control_flow_and_functions_run(void) {
    gl2_target_t t = gl2_target();

    /* The sum below is 1+2+3+4 = 10, scaled to 0.5, a mid grey no single mistake would
     * land on. */
    const GLuint prog =
        linked_program("attribute vec3 pos;\n"
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

/* A sampler reads the unit its uniform names, with no texture enable. */
static void test_gl2_a_sampler_reads_its_own_unit(void) {
    gl2_target_t t = gl2_target();

    /* Two 2x2 textures on two units, and a shader that mixes them. The texture enables
     * are never called: a sampler's declared type names its target. */
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
        "void main() { uv = pos.xy * 0.5 + vec2(0.5); gl_Position = vec4(pos, 1.0); "
        "}\n",
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

    /* Half red and half blue, with no green anywhere. Swapping the two samplers' units
     * would give the same answer, so the second half of this test separates them. */
    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(mid) > 120 && px_r(mid) < 135);
    ASSERT_TRUE(px_b(mid) > 120 && px_b(mid) < 135);
    ASSERT_TRUE(px_g(mid) < 8);

    /* Both samplers on unit 1 give all blue only if `glUniform1i` chose the unit rather
     * than declaration order. */
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform1i(glGetUniformLocation(prog, "first"), 1);
    draw_quad(loc, 0.0f);
    const uint32_t all_blue = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_b(all_blue) > 247 && px_r(all_blue) < 8);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* Attribute arrays drawn with glDrawArrays give each vertex its own values. */
static void test_gl2_attribute_arrays_feed_the_shader(void) {
    gl2_target_t t = gl2_target();

    /* Every vertex is fetched before any is shaded, so keeping one current value per
     * slot would give every vertex the last one's colour. */
    static const GLfloat verts[9] = {-0.8f, -0.8f, 0.0f, 0.8f, -0.8f,
                                     0.0f,  0.0f,  0.8f, 0.0f};
    static const GLubyte cols[12] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255};

    const GLuint prog =
        linked_program("attribute vec3 pos;\n"
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
    /* Normalised, so 255 is 1.0 and not 255.0. */
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
    /* The middle is a mix of all three, which an unnormalised read would have saturated
     * to white. */
    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2 + 6);
    ASSERT_TRUE(px_r(mid) > 20 && px_r(mid) < 200);
    ASSERT_TRUE(px_g(mid) > 20 && px_g(mid) < 200);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* The reference interpreter runs file-scope `const`s and `discard`, as `glsl_gen.c`
 * compiles them for the console; the console is meant to agree with this rasteriser. */
static void test_gl2_the_reference_runs_globals_and_discard(void) {
    gl2_target_t t = gl2_target();
    static const GLfloat verts[9] = {-0.9f, -0.9f, 0.0f, 0.9f, -0.9f,
                                     0.0f,  0.0f,  0.9f, 0.0f};

    /* Three globals, the third written in terms of the first two, so they must be
     * declared in source order. */
    const GLuint prog =
        linked_program("attribute vec3 pos;\n"
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

    /* `dim` is `warm * half_on`, so (0.5, 0.25, 0.0); an uninitialised or out-of-order
     * `const` would give black. */
    const uint32_t mid = px(&t, GL2_W / 2, GL2_H / 2 + 6);
    ASSERT_TRUE(px_r(mid) > 115 && px_r(mid) < 140);
    ASSERT_TRUE(px_g(mid) > 54 && px_g(mid) < 76);
    ASSERT_TRUE(px_b(mid) < 12);

    /* `discard` leaves the background under the fragment. */
    const GLuint killer =
        linked_program("attribute vec3 pos;\n"
                       "void main() { gl_Position = vec4(pos, 1.0); }\n",
                       "const float always = 1.0;\n"
                       "void main() {\n"
                       "  if (always > 0.5) { discard; }\n"
                       "  gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
                       "}\n");
    glUseProgram(killer);
    glVertexAttribPointer((GLuint)glGetAttribLocation(killer, "pos"), 3, GL_FLOAT,
                          GL_FALSE, 0, verts);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    /* Still the previous shader's colour, not white. */
    const uint32_t after = px(&t, GL2_W / 2, GL2_H / 2 + 6);
    ASSERT_EQ(after, mid);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* `gl_FragCoord.y` counts up from the bottom, and `dFdx` is the per-pixel slope. */
static void test_gl2_frag_coord_and_derivatives(void) {
    gl2_target_t t = gl2_target();

    /* `gl_FragCoord.y` counts up from the bottom, the opposite of the rasteriser's
     * rows, so the row index would draw this gradient upside down. */
    const GLuint prog = linked_program(
        "attribute vec3 pos;\n"
        "void main() { gl_Position = vec4(pos, 1.0); }\n",
        "void main() { gl_FragColor = vec4(gl_FragCoord.y / 64.0, 0.0, 0.0, 1.0); }\n");
    const GLint loc = glGetAttribLocation(prog, "pos");
    glUseProgram(prog);
    draw_quad(loc, 0.0f);

    const uint32_t high_row = px(&t, GL2_W / 2, 10); /* near the top of the image */
    const uint32_t low_row = px(&t, GL2_W / 2, GL2_H - 11); /* near the bottom */
    ASSERT_TRUE(px_r(high_row) > px_r(low_row));

    /* A derivative across the screen. `dFdx` of a varying that runs 0..1 over the
     * quad's width is its slope per pixel, about 1/51 here, so scaling by 64 gives
     * something visible and constant. */
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
    ASSERT_EQ(px_r(a), px_r(b)); /* constant across a linear varying */
    /* 0.625 * 2 over 51.2 pixels, times 64, is about 1.56, clamped to 1.0 in the
     * framebuffer. */
    ASSERT_TRUE(px_r(a) > 200);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* A program with only a vertex shader feeds the fixed-function fragment stage. */
static void test_gl2_a_vertex_shader_alone_feeds_fixed_function(void) {
    gl2_target_t t = gl2_target();

    /* The fixed-function fragment stage reads `gl_FrontColor` and `gl_TexCoord[]`, as a
     * port that replaces its transform and keeps its texture combiner relies on. */
    const GLuint prog = linked_program("attribute vec3 pos;\n"
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

/* GLSL 1.20 conversions and matrix built-ins compute the right values at run time. */
static void test_gl2_glsl_120_runs_what_it_compiles(void) {
    gl2_target_t t = gl2_target();

    /* An integer that widened arrives as a float at run time: keeping the left
     * operand's type would make `2 * 0.25` an integer expression and truncate it to 0.
     */
    const GLuint prog =
        linked_program("#version 120\n"
                       "attribute vec3 pos;\n"
                       "void main() { gl_Position = vec4(pos, 1.0); }\n",
                       "#version 120\n"
                       "void main() {\n"
                       "  float half_v = 2 * 0.25;\n" /* 0.5, not 0 */
                       "  float quarter = 1 / 4.0;\n" /* 0.25, not 0 */
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

    /* `outerProduct(c, r)` puts `c` down the columns, so element (col 0, row 1) is
     * `c.y * r.x`, and the transpose swaps it with (col 1, row 0). */
    glClear(GL_COLOR_BUFFER_BIT);
    const GLuint mprog =
        linked_program("#version 120\n"
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

/* A shader that exceeds its step budget ends the draw with an error and draws nothing.
 */
static void test_gl2_a_runaway_shader_is_stopped(void) {
    gl2_target_t t = gl2_target();

    /* Running out of the step budget ends the draw with GL_INVALID_OPERATION rather
     * than taking the frame; a half-run shader has no colour, so nothing is drawn. */
    const GLuint prog =
        linked_program("attribute vec3 pos;\n"
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

/* Separate stencil and blend-equation state per face and channel group, with the
 * unsuffixed calls setting both. */
static void test_gl2_separate_stencil_and_blend_state(void) {
    void *ctx = gl2_context();

    /* `glStencilFunc` sets both faces, as GL 2.0 defines it, so a GL 1.x program is
     * unaffected by the split. */
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

    /* A push and pop carries both faces: GL 2.0 puts the back-face state in the same
     * attribute group. */
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

/* glBlendEquationSeparate applies one equation to colour and another to alpha. */
static void test_gl2_separate_blend_equation_blends(void) {
    gl2_target_t t = gl2_target();

    /* The colour subtracts and the alpha adds, in one blend. */
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
     * Not exact: the destination came out of eight-bit storage, so 0.25 is 64/255. A
     * shared equation would give 0.75 in the colour or 0.25 in the alpha. */
    const uint32_t p = px(&t, GL2_W / 2 + 8, GL2_H / 2 + 8);
    const int alpha = (int)((p >> 24) & 0xffu);
    ASSERT_TRUE(px_r(p) > 58 && px_r(p) < 70);
    ASSERT_TRUE(alpha > 185 && alpha < 198);

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* glDrawBuffers accepts one buffer, the fragment stage's single colour output. */
static void test_gl2_draw_buffers(void) {
    void *ctx = gl2_context();

    GLint v = 0;

    /* GLSL declares `gl_FragData[gl_MaxDrawBuffers]` and the fragment stage exports one
     * colour target, so the limit is one; front and back are not independent outputs
     * (GL 2.0, 4.2.1). Both together is `glDrawBuffer(GL_FRONT_AND_BACK)`. */
    const GLenum both[2] = {GL_FRONT, GL_BACK};
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &v);
    ASSERT_EQ(v, 1);
    glDrawBuffers(2, both);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

    /* The singular call names both. */
    glDrawBuffer(GL_FRONT_AND_BACK);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glGetIntegerv(GL_DRAW_BUFFER, &v);
    ASSERT_EQ(v, (GLint)GL_FRONT_AND_BACK);

    const GLenum one[1] = {GL_BACK};
    glDrawBuffers(1, one);
    glGetIntegerv(GL_DRAW_BUFFER, &v);
    ASSERT_EQ(v, (GLint)GL_BACK);

    /* None at all is no colour buffer, which is legal and is what a depth-only pass
     * asks for. */
    glDrawBuffers(0, NULL);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    glGetIntegerv(GL_DRAW_BUFFER, &v);
    ASSERT_EQ(v, (GLint)GL_NONE);

    /* A name covering more than one buffer may not appear in the list (GL 2.0, 4.2.1):
     * GL_INVALID_OPERATION, not a silent union. */
    const GLenum wide[1] = {GL_FRONT_AND_BACK};
    glDrawBuffers(1, wide);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    /* A buffer named twice needs two entries, which the limit of one already refuses
     * with GL_INVALID_VALUE. */
    const GLenum twice[2] = {GL_BACK, GL_BACK};
    glDrawBuffers(2, twice);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);
    const GLenum absent[1] = {GL_FRONT_RIGHT};
    glDrawBuffers(1, absent);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

    GLint limit = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &limit);
    glDrawBuffers(limit + 1, both);
    ASSERT_EQ(glGetError(), GL_INVALID_VALUE);

    glContextDestroy(ctx);
}

/* A context has only the GL 2.0 entry points and enumerants when it claims 2.0. */
static void test_gl2_version_gating(void) {
    /* Not through `gl2_context`, which claims 2.0; this one is the default, 1.1. */
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 64, 64);
    void *ctx = glContextCreate(disp);
    glContextMakeCurrent(ctx);

    GLuint maj = 0u, min = 9u;
    glContextGetVersion(&maj, &min);
    ASSERT_EQ(maj, 1u);
    ASSERT_EQ(min, 1u);

    /* A name-returning call answers 0, a location -1, a predicate GL_FALSE, and every
     * one records GL_INVALID_OPERATION. */
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

    /* The refused call did nothing: the back stencil state is untouched after
     * `glStencilOpSeparate`. */
    glContextSetVersion(2, 0);
    GLint v = 0;
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS, &v);
    ASSERT_EQ(v, (GLint)GL_KEEP);
    glContextSetVersion(1, 1);

    /* An enumerant a later version added is GL_INVALID_ENUM, not GL_INVALID_OPERATION.
     */
    v = 1234;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &v);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    ASSERT_EQ(v, 1234);
    glGetIntegerv(GL_CURRENT_PROGRAM, &v);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);
    ASSERT_TRUE(glGetString(GL_SHADING_LANGUAGE_VERSION) == NULL);
    ASSERT_EQ(glGetError(), GL_INVALID_ENUM);

    /* GL 1.x is untouched, and so are the extension spellings: every 1.2-1.5 feature is
     * advertised as an ARB or EXT extension, available whatever the core version. */
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
    /* The GL 1.0 stencil call set both faces even here: the state exists whatever
     * version is claimed, and only the per-face entry point is 2.0's. */
    glContextSetVersion(2, 0);
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS, &v);
    ASSERT_EQ(v, (GLint)GL_INCR);

    /* Claiming it turns the whole surface on. */
    const GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
    ASSERT_TRUE(sh != 0u);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    /* Narrowing again turns it back off. */
    glContextSetVersion(1, 5);
    ASSERT_EQ(glCreateShader(GL_FRAGMENT_SHADER), 0u);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);

    glContextDestroy(ctx);
    oops_display_close(disp);
}

/* -------------------------------------------------------------------------
 * The console back end
 *
 * These assert instruction words against an assembler: `tools/shader/gl2-fragment.s`
 * holds the source, and the words are what `clang -target amdgcn-amd-amdhsa
 * -mcpu=gfx1030` assembles it to. A wrong encoding in a compiler is wrong in every
 * shader it emits. Nothing here runs on a console.
 * ------------------------------------------------------------------------- */

/* Each emitter produces the instruction words clang assembles for gfx1030. */
static void test_gl2_pixel_shader_encodings_match_the_assembler(void) {
    uint32_t words[64];
    glsl_code_t c;

    /* The parameter cache address, which every interpolation reads out of `m0`. Both
     * source registers, because the draw's user-SGPR count decides where the primitive
     * mask is; these are the words the fixed-function pixel shaders carry
     * (`gl_context.c`, `ps_untex[1]` and `ps_tex[1]`). */
    glsl_code_init(&c, words, 64);
    glsl_emit_s_mov_m0(&c, 0u);
    glsl_emit_s_mov_m0(&c, 2u);
    ASSERT_EQ(c.count, 2u);
    ASSERT_EQ(words[0], 0xbefc0300u); /* s_mov_b32 m0, s0 */
    ASSERT_EQ(words[1], 0xbefc0302u); /* s_mov_b32 m0, s2 */

    /* DPP, the eight-byte form a derivative reads its neighbour through. `src0` is the
     * 0xfa marker in the first word; the real source register, the permute and the
     * masks are in the second. */
    glsl_code_init(&c, words, 64);
    glsl_emit_dpp_mov(&c, 4u, 5u, GLSL_DPP_QUAD_X_NEAR);
    glsl_emit_dpp_sub(&c, 4u, 5u, 5u, GLSL_DPP_QUAD_X_FAR);
    ASSERT_EQ(c.count, 4u);
    ASSERT_EQ(words[0], 0x7e0802fau); /* v_mov_b32_dpp v4, v5 quad_perm:[0,0,2,2] */
    ASSERT_EQ(words[1],
              0xff00a005u); /* [0x05,0xa0,0x00,0xff] - source, permute, then masks */
    ASSERT_EQ(words[2], 0x08080afau); /* v_sub_f32_dpp v4, v5, v5 quad_perm:[1,1,3,3] */
    ASSERT_EQ(words[3], 0xff00f505u);

    /* The depth export: target 8, one channel, and no `done`. The colour export that
     * follows carries `done`; two exports both claiming to be last do not retire. */
    glsl_code_init(&c, words, 64);
    glsl_emit_export_mrtz(&c, 4u);
    ASSERT_EQ(c.count, 2u);
    ASSERT_EQ(words[0], 0xf8000081u); /* exp mrtz v4, off, off, off */
    ASSERT_EQ(words[1], 0x00000004u);
    ASSERT_EQ((words[0] >> 11) & 1u, 0u); /* done is not set */

    /* A scalar operand in `src0`, which is how `gl_FragCoord.y` is flipped: the
     * viewport height is an SGPR and the row a VGPR. VOP2's `src0` is nine bits and
     * names either; `vsrc1` names only a VGPR. `glsl_emit_sub_f32` would encode s44 as
     * v44. */
    glsl_code_init(&c, words, 64);
    glsl_emit_vop2(&c, GLSL_VOP2_SUB_F32, 8u, glsl_sgpr(44u), 3u);
    ASSERT_EQ(c.count, 1u);
    ASSERT_EQ(words[0], 0x0810062cu); /* v_sub_f32_e32 v8, s44, v3 */

    /* Interpolation across all four channels and a high attribute, so the attribute
     * field is pinned apart from the channel field. */
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

    /* The colour export is four dwords: two `v_cvt_pkrtz_f16_f32` that pack the four
     * floats into two registers, then the export, its first dword carrying `compr`,
     * `done` and `vm`. An 8_8_8_8 target on a part with RB+ requires the half-float
     * format; see glsl_emit_export_mrt0. */
    glsl_code_init(&c, words, 64);
    glsl_emit_export_mrt0(&c, 4u);
    ASSERT_EQ(c.count, 4u);
    ASSERT_EQ(words[0], 0x5e080b04u); /* v_cvt_pkrtz_f16_f32 v4, v4, v5 */
    ASSERT_EQ(words[1], 0x5e0a0f06u); /* v_cvt_pkrtz_f16_f32 v5, v6, v7 */
    ASSERT_EQ(words[2], 0xf8001c0fu); /* exp mrt0 ... done compr vm */
    ASSERT_EQ(words[3], 0x00000504u); /* v4, v5 */

    /* From v0, this is LLVM's output word for word: `llc -mcpu=gfx1030` on two
     * `llvm.amdgcn.cvt.pkrtz` feeding `llvm.amdgcn.exp.compr.v2f16` gives
     * `[0x00,0x03,0x00,0x5e]`, `[0x02,0x07,0x02,0x5e]` and
     * `[0x0f,0x1c,0x00,0xf8],[0x00,0x01,0x00,0x00]`. */
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
    /* The same word glAlphaFunc and the polygon stipple use. */
    ASSERT_EQ(words[6], 0x877e6a7eu); /* s_and_b32 exec_lo, exec_lo, vcc_lo */

    glsl_code_init(&c, words, 64);
    glsl_emit_nop(&c);
    glsl_emit_endpgm(&c);
    ASSERT_EQ(words[0], 0xbf800000u); /* s_nop 0 */
    ASSERT_EQ(words[1], 0xbf810000u); /* s_endpgm */
}

/* The link refuses a missing context before it compiles the fragment stage, whose
 * serial lives in the context. */
static void test_gl2_link_refuses_a_null_context(void) {
    void *ctx = gl2_context();
    const GLuint prog = linked_program(
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
        "void main() { gl_FragColor = vec4(1.0); }\n");
    gl_context_t *c = (gl_context_t *)ctx;
    gl_program_object_t *p = gl_find_program(c, prog);
    ASSERT_TRUE(p != NULL);
    glsl_unit_t *vs = NULL, *fs = NULL;
    for (int i = 0; i < p->attached_count; i++) {
        const gl_shader_object_t *s = gl_find_shader(c, p->attached[i]);
        if (s && s->type == GL_VERTEX_SHADER)
            vs = s->unit;
        if (s && s->type == GL_FRAGMENT_SHADER)
            fs = s->unit;
    }
    ASSERT_TRUE(vs != NULL && fs != NULL);
    ASSERT_EQ(gl_program_link(NULL, p, vs, fs), GL_FALSE);
    glDeleteProgram(prog);
    glContextDestroy(ctx);
}

/* gl2-cube's fragment shader compiles to the expected console pixel shader. */
static void test_gl2_compiles_a_whole_pixel_shader(void) {
    void *ctx = gl2_context();

    /* gl2-cube's fragment shader: three varying components interpolated, a constructor,
     * and the colour exported. */
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
    const GLboolean ok = gl_program_compile_fragment(
        p, words, 256u, &count, &vgprs, &user_sgprs, &input_ena, log, sizeof(log));
    if (!ok)
        printf("\n    compile failed: %s\n", log);
    ASSERT_EQ(ok, GL_TRUE);

    /* `m0` first: every interpolation reads the parameter cache through it. s2 because
     * the block's address is in s[0:1], which puts the primitive mask after them. */
    ASSERT_EQ(words[0], 0xbefc0302u); /* s_mov_b32 m0, s2 */

    /* No uniform load: the loaded window spans only the uniforms this shader names, and
     * it names none of the vertex shader's `mvp`, so `s_waitcnt` follows `m0` directly.
     * The load is pinned in `test_gl2_compiled_uniform_window`. */
    ASSERT_EQ(words[1], 0xbf8cc07fu); /* s_waitcnt lgkmcnt(0) */

    /* Then the live mask is saved, for every shader: it holds the lanes that reach the
     * export, so `discard` keeps a lane out and a `return` in `main` brings one back
     * (`glsl_ps.c`). clang 21 for gfx1030 assembles `s_mov_b32 s52, exec_lo` to
     * 0xbeb4037e. */
    ASSERT_EQ(words[2], 0xbeb4037eu);

    /* Then three components of one varying, each a `p1`/`p2` pair, into v8, v9, v10,
     * the first registers above the ones the hardware owns. */
    ASSERT_EQ(words[3], 0xc8200000u); /* v_interp_p1_f32 v8, v0, attr0.x */
    ASSERT_EQ(words[4], 0xc8210001u); /* v_interp_p2_f32 v8, v1, attr0.x */
    ASSERT_EQ(words[5], 0xc8240100u); /* v9, attr0.y */
    ASSERT_EQ(words[6], 0xc8250101u);
    ASSERT_EQ(words[7], 0xc8280200u); /* v10, attr0.z */
    ASSERT_EQ(words[8], 0xc8290201u);

    /* Two user SGPRs is what the draw configures into `SPI_SHADER_PGM_RSRC2_PS`, and it
     * is also what puts the mask in s2, so the count and the `m0` source must agree. */
    ASSERT_EQ(user_sgprs, 2u);

    /* The pixel stage is asked for exactly what this shader reads: the perspective
     * centre barycentrics, since nothing names `gl_FragCoord`. The window position
     * would cost four VGPRs. */
    ASSERT_EQ(input_ena, 0x00000002u);

    /* The epilogue: the colour into v4..v7, the export, `s_endpgm`, and two `s_nop`s of
     * room past it. The tail is `GL_PS_EXPORT_WORDS` long and identical to
     * `gl_ps_export_words(GL_FALSE)`, so the draw path can write the two-target form
     * over it in place when `fb_also` is bound. */
    ASSERT_EQ(words[count - 1u], 0xbf800000u); /* s_nop 0, past the end */
    ASSERT_EQ(words[count - 2u], 0xbf800000u); /* s_nop 0, past the end */
    ASSERT_EQ(words[count - 3u], 0xbf810000u); /* s_endpgm */
    ASSERT_EQ(words[count - 4u], 0x00000504u); /* v4, v5 - the two packed registers */
    ASSERT_EQ(words[count - 5u], 0xf8001c0fu); /* exp mrt0 ... done compr vm */
    ASSERT_EQ(words[count - 6u], 0x5e0a0f06u); /* v_cvt_pkrtz_f16_f32 v5, v6, v7 */
    ASSERT_EQ(words[count - 7u], 0x5e080b04u); /* v_cvt_pkrtz_f16_f32 v4, v4, v5 */
    /* The whole tail is the shared one, so the swap is a copy. */
    for (uint32_t i = 0u; i < GL_PS_EXPORT_WORDS; i++) {
        ASSERT_EQ(words[count - GL_PS_EXPORT_WORDS + i],
                  gl_ps_export_words(GL_FALSE)[i]);
    }
    for (uint32_t i = 0; i < 4u; i++) {
        /* v_mov_b32 v4+i, <colour>+i, just above the export tail; the opcode and
           destination are what matter. */
        const uint32_t w = words[count - GL_PS_EXPORT_WORDS - 4u + i];
        ASSERT_EQ(w >> 25, 0x3fu);            /* VOP1 */
        ASSERT_EQ((w >> 17) & 0xffu, 4u + i); /* into v4..v7 */
        ASSERT_EQ((w >> 9) & 0xffu, 1u);      /* v_mov_b32 */
    }

    /* The register count is what the resource register reserves, so it includes
     * everything below v8, which is the hardware's. */
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
    ASSERT_EQ(gl_program_compile_fragment(p2, words, 256u, &count, &vgprs, &user_sgprs,
                                          NULL, log, sizeof(log)),
              GL_TRUE);
    ASSERT_EQ(count, 0u);
    /* Nothing was compiled, so the draw configures nothing: a program on this arm runs
     * the fixed-function pixel shader, which brings its own user data. */
    ASSERT_EQ(user_sgprs, 0u);

    glContextDestroy(ctx);
}

/* The back end refuses, with the limit in the log, what exceeds the hardware's limits.
 */
static void test_gl2_the_back_end_refuses_what_it_cannot_encode(void) {
    void *ctx = gl2_context();
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};

    /* Every lookup a fragment shader may call is generated, so no lookup is refused
     * here. A sampler-type mismatch is refused by the front end first ("must be its own
     * sampler type"), so it cannot reach the back end. */
    gl_context_t *c = (gl_context_t *)ctx;

    /* One sampler more than a draw carries descriptor sets for. The message names the
     * number, and the shader is built from the limit so it follows it. */
    memset(log, 0, sizeof(log));
    char too_many_fs[512];
    int at = 0;
    for (int s = 0; s <= OOPS_GL_GL2_TEX_SETS; s++) {
        at += oops_snprintf(too_many_fs + at, sizeof(too_many_fs) - (size_t)at,
                            "uniform sampler2D s%d;\n", s);
    }
    at += oops_snprintf(too_many_fs + at, sizeof(too_many_fs) - (size_t)at,
                        "varying vec2 uv;\nvoid main() {\n  gl_FragColor = vec4(0.0)");
    for (int s = 0; s <= OOPS_GL_GL2_TEX_SETS; s++) {
        at += oops_snprintf(too_many_fs + at, sizeof(too_many_fs) - (size_t)at,
                            " + texture2D(s%d, uv)", s);
    }
    (void)oops_snprintf(too_many_fs + at, sizeof(too_many_fs) - (size_t)at, ";\n}\n");

    const GLuint too_many =
        linked_program("attribute vec3 pos;\n"
                       "varying vec2 uv;\n"
                       "void main() { uv = pos.xy; gl_Position = vec4(pos, 1.0); }\n",
                       too_many_fs);
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, too_many), words, 256u,
                                          &count, &vgprs, NULL, NULL, log, sizeof(log)),
              GL_FALSE);
    /* The set count, as the message spells it. */
    char want[32];
    (void)oops_snprintf(want, sizeof(want), "more than %d", OOPS_GL_GL2_TEX_SETS);
    ASSERT_TRUE(strstr(log, want) != NULL);

    /* The same count of cube samplers is refused for the count too: a cube takes a set
     * like a 2D sampler does, so its type is not the reason. */
    memset(log, 0, sizeof(log));
    at = 0;
    for (int s = 0; s <= OOPS_GL_GL2_TEX_SETS; s++) {
        at += oops_snprintf(too_many_fs + at, sizeof(too_many_fs) - (size_t)at,
                            "uniform samplerCube s%d;\n", s);
    }
    at += oops_snprintf(too_many_fs + at, sizeof(too_many_fs) - (size_t)at,
                        "varying vec2 uv;\nvoid main() {\n  gl_FragColor = vec4(0.0)");
    for (int s = 0; s <= OOPS_GL_GL2_TEX_SETS; s++) {
        at += oops_snprintf(too_many_fs + at, sizeof(too_many_fs) - (size_t)at,
                            " + textureCube(s%d, vec3(uv, 1.0))", s);
    }
    (void)oops_snprintf(too_many_fs + at, sizeof(too_many_fs) - (size_t)at, ";\n}\n");
    const GLuint too_many_cubes =
        linked_program("attribute vec3 pos;\n"
                       "varying vec2 uv;\n"
                       "void main() { uv = pos.xy; gl_Position = vec4(pos, 1.0); }\n",
                       too_many_fs);
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, too_many_cubes), words,
                                          256u, &count, &vgprs, NULL, NULL, log,
                                          sizeof(log)),
              GL_FALSE);
    ASSERT_TRUE(strstr(log, want) != NULL);

    /* The varying limit, named with its number: four parameters, sixteen floats. A
     * program needing a fifth is refused rather than compiled into a shader that reads
     * a parameter the vertex stage never exported. */
    const GLuint wide =
        linked_program("attribute vec4 pos;\n"
                       "varying vec4 a;\nvarying vec4 b;\nvarying vec4 d;\nvarying "
                       "vec4 e;\nvarying vec4 f;\n"
                       "void main() {\n"
                       "  a = pos; b = pos; d = pos; e = pos; f = pos;\n"
                       "  gl_Position = pos;\n"
                       "}\n",
                       "varying vec4 a;\nvarying vec4 b;\nvarying vec4 d;\nvarying "
                       "vec4 e;\nvarying vec4 f;\n"
                       "void main() { gl_FragColor = a + b + d + e + f; }\n");
    memset(log, 0, sizeof(log));
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, wide), words, 256u, &count,
                                          &vgprs, NULL, NULL, log, sizeof(log)),
              GL_FALSE);
    ASSERT_TRUE(strstr(log, "16") != NULL);

    glContextDestroy(ctx);
}

/* -------------------------------------------------------------------------
 * A simulator for the compiled pixel shader
 *
 * Asserting words catches a wrong encoding but not a wrong lowering: `sin` compiled to
 * an unscaled `v_sin_f32` is correctly encoded and computes the wrong function. This
 * decodes the emitted shader, runs it, and compares the exported colour with the same
 * arithmetic in C.
 *
 * It models the hardware's semantics: `v_sin_f32` computes `sin(2*pi*x)`, `v_exp_f32`
 * is base two, and `v_sub_f32` subtracts `vsrc1` from `src0`.
 *
 * One lane. `v_interp_p1_f32` loads the parameter straight out of `attr` and
 * `v_interp_p2_f32` adds nothing: the value at one fragment is all the tests need.
 * ------------------------------------------------------------------------- */

typedef struct {
    float v[256];
    float s[128]; /* the scalar file as floats, which is where a uniform lands */
    /* The same file as lane masks; one lane, so a mask is a boolean. `glsl_ps.c` loads
     * uniforms from s16 up and `glsl_gen.c` saves exec masks into s4..s15; keeping the
     * views apart makes a shader that confused them read zero, not a plausible float.
     */
    GLboolean smask[128];
    /* A third view, as unsigned integers: a branched loop's trip counter, compared with
     * `s_cmp_ge_u32`. A counter register is never a mask; the views are separate
     * because a counter of 0 and an empty mask share a bit pattern. */
    uint32_t scount[128];
    /* The scalar condition code, which is what `s_cbranch_scc1` reads. */
    GLboolean scc;
    /* The block `s[0:1]` points at, laid out as `gl_gl2_build_block` does: texture
     * descriptors, then the uniforms at 0x80. Built rather than aliased to `p->values`,
     * so a load from the wrong offset reads a descriptor. */
    float ublock[OOPS_GL_GL2_SLOT_STRIDE / 4];
    int ublock_floats;
    /* Set by a sample and cleared by `s_waitcnt vmcnt(0)`. Reading one before the wait
     * computes with what the register held before the sample. */
    GLboolean vpending[256];
    int samples;           /* how many `image_sample`s ran */
    uint32_t last_tex_set; /* which descriptor set the last one used */
    GLboolean vcc;
    GLboolean exec;
    float out[4];
    GLboolean exported;
    GLboolean lane_survived; /* exec at the export: whether this fragment is written */
    GLboolean ended;
    /* Set by a scalar load and cleared by its wait. Reading an SGPR while it is set
     * reads a register the load has not delivered into; here that fails the test. */
    GLboolean lgkm_pending;
    /* Whether `m0` points at the parameter cache. The hardware reads it on every
     * `v_interp`; without it a shader interpolates another primitive's parameters. The
     * interpolation arm refuses to run without it. */
    GLboolean m0_set;
    /* Which hardware-owned registers this shader asked for, from the `SPI_PS_INPUT_ENA`
     * the compiler reported. One not asked for holds what the previous wave left. */
    GLboolean hw_vgpr_live[8];
    /* The two half-floats a `v_cvt_pkrtz_f16_f32` packed into a register, which the
       compressed export reads. Written only by opcode 47, so a register exported
       compressed without being packed reads as zero. */
    float vpack[256][2];
    GLboolean vpacked[256];
} sim_t;

/* A float as `v_cvt_pkrtz_f16_f32` leaves it: half precision, round toward zero. An
 * 8_8_8_8 target on this part takes half-floats, so a fragment's colour keeps ten
 * mantissa bits. Half's subnormals, below 6.1e-5 (a quarter of one 8-bit level), are
 * flushed to zero rather than modelled. */
static float sim_f16_rtz(float f) {
    union {
        float f;
        uint32_t u;
    } c;
    c.f = f;
    const uint32_t sign = c.u & 0x80000000u;
    const uint32_t biased = (c.u >> 23) & 0xffu;
    if (biased == 0xffu)
        return f; /* inf and nan pass through */
    {
        const int32_t e = (int32_t)biased - 127;
        if (e > 15) {
            c.u = sign | 0x7f800000u;
            return c.f;
        } /* beyond half's range */
        if (e < -14) {
            c.u = sign;
            return c.f;
        } /* below its normals */
        c.u = (c.u &
               ~0x1fffu); /* ten mantissa bits, the low thirteen dropped: toward zero */
        c.u |= sign;
        return c.f;
    }
}

/* The first register the allocator owns; everything below it is the SPI's. Mirrors
 * `GL_PS_FIRST_FREE_VGPR` in `glsl_ps.c`, which is not a header constant. */
#define GL_PS_FIRST_FREE_VGPR_SIM 8u

static float sim_f32(uint32_t bits) {
    union {
        uint32_t u;
        float f;
    } cvt;
    cvt.u = bits;
    return cvt.f;
}

/* A source operand: a VGPR, an SGPR, one of the two inline constants this back end
 * emits, or a literal dword that follows the instruction. */
static float sim_src(sim_t *s, uint32_t src0, const uint32_t *w, uint32_t *i) {
    if (src0 >= 256u) {
        const uint32_t r = src0 - 256u;
        ASSERT_EQ(s->vpending[r], GL_FALSE);
        /* Below `GL_PS_FIRST_FREE_VGPR` the file belongs to the hardware, and which of
         * it is live depends on `SPI_PS_INPUT_ENA`: the barycentrics always, the window
         * position only for `gl_FragCoord`, the face only for `gl_FrontFacing`, packed
         * so enabling one moves the next. On hardware an unasked register returns what
         * the previous wave left; here it is an assertion. */
        if (r < GL_PS_FIRST_FREE_VGPR_SIM)
            ASSERT_TRUE(s->hw_vgpr_live[r]);
        return s->v[r];
    }
    if (src0 == 128u)
        return 0.0f;
    if (src0 == 242u)
        return 1.0f;
    if (src0 == 255u)
        return sim_f32(w[++(*i)]);
    if (src0 == 250u) {
        /* DPP reads a neighbouring lane; the extra dword carries the real source
         * register and the permute. With one lane every permute selects that lane, so a
         * derivative is zero here, and the derivative tests assert instructions and
         * whole-quad mode rather than a slope. */
        const uint32_t tail = w[++(*i)];
        return s->v[tail & 0xffu];
    }
    /* SGPRs run to s105 on GFX10; `flat_scratch` at 102 is GFX9's encoding. clang
     * assembles `v_mov_b32 v0, s105` for gfx1030 to `7E000269`, and `vcc_lo` is 106
     * (Mesa `ac_gpu_info.c:260`, `max_sgpr_alloc` 108 with VCC at s[106-107]). */
    if (src0 < 106u) {
        /* A scalar read with a load still in flight returns whatever the register held
         * on hardware; here it fails. */
        ASSERT_EQ(s->lgkm_pending, GL_FALSE);
        return s->s[src0];
    }
    ASSERT_TRUE(0); /* an operand encoding no test has taught this simulator */
    return 0.0f;
}

/* A scalar operand read as a lane mask: `exec`, `vcc`, the inline zero, or a saved
 * mask. */
static GLboolean sim_mask(const sim_t *s, uint32_t reg) {
    if (reg == 126u)
        return s->exec;
    if (reg == 106u)
        return s->vcc;
    if (reg == 128u)
        return GL_FALSE;
    ASSERT_TRUE(reg < 102u);
    return s->smask[reg];
}

static void sim_set_mask(sim_t *s, uint32_t reg, GLboolean value) {
    if (reg == 126u) {
        s->exec = value;
        return;
    }
    if (reg == 106u) {
        s->vcc = value;
        return;
    }
    ASSERT_TRUE(reg < 102u);
    s->smask[reg] = value;
}

/* How many instructions a shader may run before the simulator calls it a hang, so a
 * loop that never ends fails at a line number. Generous: the largest loop these tests
 * write is a few thousand trips of a few dozen instructions. */
#define SIM_MAX_STEPS 2000000

/* The depth a comparing sample compares against, less-or-equal. A test moves it to
 * make a reference's exact value decide the result. */
static float s_sim_shadow_depth = 0.5f;

static void sim_run(sim_t *s, const uint32_t *w, uint32_t count,
                    const float attr[4][4]) {
    const double PI = 3.14159265358979323846;
    long steps = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t x = w[i];
        if (++steps > SIM_MAX_STEPS) {
            printf("\n    the shader ran %ld instructions without ending - a loop that "
                   "does not "
                   "terminate\n",
                   steps);
            ASSERT_TRUE(0);
        }

        if (x == 0xbf810000u) {
            s->ended = GL_TRUE;
            break;
        }
        if (x == 0xbf800000u)
            continue; /* s_nop */
        if (x == 0xbf8cc07fu) {
            s->lgkm_pending = GL_FALSE;
            continue;
        } /* s_waitcnt lgkmcnt(0) */
        if (x == 0xbf8c3f70u) { /* s_waitcnt vmcnt(0) */
            for (int k = 0; k < 256; k++)
                s->vpending[k] = GL_FALSE;
            continue;
        }

        if ((x >> 26) == 0x3cu) { /* MIMG: image_sample */
            const uint32_t w1 = w[++i];
            const uint32_t vdata = (w1 >> 8) & 0xffu;
            const uint32_t vaddr = w1 & 0xffu;
            const uint32_t srsrc = ((w1 >> 16) & 0x1fu) * 4u;
            const uint32_t ssamp = ((w1 >> 21) & 0x1fu) * 4u;
            const uint32_t dim = (x >> 3) & 0x7u;
            const uint32_t mimg_op = (x >> 18) & 0x7fu;
            const uint32_t dmask = (x >> 8) & 0xfu;
            /* `image_sample` returns a texel in four registers; `image_sample_c`
             * compares and returns one. Never `_lz`, which would give up the mip chain
             * and the LOD bias. */
            ASSERT_TRUE(mimg_op == 32u || mimg_op == 40u);
            ASSERT_EQ(dmask, (mimg_op == 40u) ? 0x1u : 0xfu);
            ASSERT_TRUE(dim == 1u || dim == 2u || dim == 3u); /* 2D, volume or cube */
            /* The sampler's four registers sit eight above the image's eight, the
             * layout `glsl_internal.h` sets out and the prologue loads into. */
            ASSERT_EQ(ssamp, srsrc + 8u);
            ASSERT_TRUE(srsrc >= 4u);
            const uint32_t set = (srsrc - 4u) / 12u;
            ASSERT_TRUE(set < 2u);
            /* A texture whose texel is its own coordinate plus the set it came through,
             * which shows the right coordinate reached the right descriptor set. */
            /* A comparing sample returns the comparison in one register, against
             * `s_sim_shadow_depth`. The reference is the first address register, so
             * putting it last would compare against `s`. */
            if (mimg_op == 40u) {
                if (s->exec)
                    s->v[vdata] = (s->v[vaddr] <= s_sim_shadow_depth) ? 1.0f : 0.0f;
                for (int k = 0; k < 1; k++)
                    s->vpending[vdata + (uint32_t)k] = GL_TRUE;
                continue;
            }
            if (s->exec) {
                s->v[vdata + 0u] = s->v[vaddr];
                s->v[vdata + 1u] = s->v[vaddr + 1u];
                /* A three-address lookup reports its third register instead of the
                 * set: the face a cube direction resolved to, or a volume's slice
                 * coordinate. */
                s->v[vdata + 2u] =
                    (dim == 2u || dim == 3u) ? s->v[vaddr + 2u] : (float)set;
                s->v[vdata + 3u] = 1.0f;
            }
            for (uint32_t k = 0; k < 4u; k++)
                s->vpending[vdata + k] = GL_TRUE;
            s->samples++;
            s->last_tex_set = set;
            continue;
        }

        /* SOP1, which has to be tested before SOP2: its top two bits are SOP2's as
         * well. */
        if ((x >> 23) == 0x17du) {
            const uint32_t sdst = (x >> 16) & 0x7fu;
            const uint32_t op = (x >> 8) & 0xffu;
            const uint32_t ssrc0 = x & 0xffu;
            if (op == 3u && sdst == 124u) { /* s_mov_b32 m0, s<n> */
                /* The parameter cache address. Its source is set by the draw's
                 * user-SGPR count: s0 with none, s2 with the block's address in s[0:1].
                 */
                ASSERT_TRUE(ssrc0 == 0u || ssrc0 == 2u);
                s->m0_set = GL_TRUE;
            } else if (op == 3u) { /* s_mov_b32 */
                sim_set_mask(s, sdst, sim_mask(s, ssrc0));
                /* A loop's trip counter is started with this same instruction, so the
                 * integer view is zeroed alongside the mask view. Only the inline zero:
                 * nothing else this generator emits moves an integer between scalar
                 * registers. */
                if (ssrc0 == 128u && sdst < 128u)
                    s->scount[sdst] = 0u;
            } else if (op == 9u) {
                /* `s_wqm_b32`. With one lane there are no helper lanes, so this is the
                 * identity; a zero mask stays zero. */
                sim_set_mask(s, sdst, sim_mask(s, ssrc0));
            } else if (op == 60u) { /* s_and_saveexec_b32 */
                sim_set_mask(s, sdst, s->exec);
                s->exec = (GLboolean)(s->exec && s->vcc);
            } else {
                ASSERT_TRUE(0);
            }
            continue;
        }

        if ((x >> 26) == 0x3du) { /* SMEM: a scalar load from s[0:1] */
            const uint32_t op = (x >> 18) & 0xffu;
            const uint32_t sdata = (x >> 6) & 0x7fu;
            const uint32_t sbase = x & 0x3fu;
            const uint32_t offset = w[++i] & 0x1fffffu;
            static const uint32_t WIDTH[5] = {1u, 2u, 4u, 8u, 16u};
            ASSERT_TRUE(op < 5u);
            ASSERT_EQ(sbase, 0u); /* s[0:1], the block's address */
            /* The destination's alignment is the assembler's rule, not the width: one
             * dword anywhere, a pair 2-aligned, four dwords and wider 4-aligned. clang
             * accepts `s_load_dwordx16 s[52:67]` and refuses `s_load_dwordx8 s[6:13]`.
             */
            {
                const uint32_t align = WIDTH[op] >= 4u ? 4u : WIDTH[op];
                ASSERT_EQ(sdata % align, 0u);
            }
            ASSERT_EQ(offset % 4u, 0u);
            for (uint32_t k = 0; k < WIDTH[op]; k++) {
                const uint32_t f = offset / 4u + k;
                /* Past the block is whatever the payload slot holds; a shader may load
                 * more than it declared, so this reads as zero. */
                s->s[sdata + k] =
                    (f < (uint32_t)s->ublock_floats) ? s->ublock[f] : 0.0f;
            }
            s->lgkm_pending = GL_TRUE;
            continue;
        }

        if ((x >> 26) == 0x3eu) { /* EXP: the second dword names the registers */
            const uint32_t regs = w[++i];
            if ((x >> 10) & 0x1u) {
                /* Compressed: two registers, four halves, (R,G) then (B,A), as
                   `glsl_emit_export_mrt0` packs them for an 8_8_8_8 target. */
                const uint32_t lo = regs & 0xffu, hi = (regs >> 8) & 0xffu;
                /* Only a surviving lane has packed anything: a wave that discarded
                   every lane still exports with `done` to retire, but its VALU packing
                   was skipped. */
                if (s->exec) {
                    ASSERT_TRUE(s->vpacked[lo]);
                    ASSERT_TRUE(s->vpacked[hi]);
                }
                s->out[0] = s->vpack[lo][0];
                s->out[1] = s->vpack[lo][1];
                s->out[2] = s->vpack[hi][0];
                s->out[3] = s->vpack[hi][1];
            } else {
                for (int c = 0; c < 4; c++)
                    s->out[c] = s->v[(regs >> (8 * c)) & 0xffu];
            }
            s->exported = GL_TRUE;
            /* The export runs whatever exec says; exec decides the pixel. "Did it
             * export" and "did this lane survive" are separate, and the tests ask both.
             */
            s->lane_survived = s->exec;
            continue;
        }
        if ((x >> 26) == 0x32u) { /* VINTRP */
            const uint32_t vdst = (x >> 18) & 0xffu;
            const uint32_t op = (x >> 16) & 0x3u;
            const uint32_t at = (x >> 10) & 0x3fu;
            const uint32_t ch = (x >> 8) & 0x3u;
            /* The parameter cache must have been addressed. On hardware an unset `m0`
             * returns another primitive's parameters without a fault. */
            ASSERT_TRUE(s->m0_set);
            if (op == 0u && s->exec)
                s->v[vdst] = attr[at][ch]; /* p1 loads; p2 adds nothing */
            continue;
        }
        if ((x >> 25) == 0x3fu) { /* VOP1 */
            const uint32_t vdst = (x >> 17) & 0xffu;
            const uint32_t op = (x >> 9) & 0xffu;
            const float a = sim_src(s, x & 0x1ffu, w, &i);
            float r = 0.0f;
            switch (op) {
            case 1u:
                r = a;
                break; /* v_mov_b32 */
            case 32u:
                r = a - floorf(a);
                break; /* v_fract_f32 */
            case 33u:
                r = truncf(a);
                break;
            case 34u:
                r = ceilf(a);
                break;
            case 36u:
                r = floorf(a);
                break;
            case 37u:
                r = powf(2.0f, a);
                break; /* base two */
            case 39u:
                r = logf(a) / logf(2.0f);
                break; /* base two */
            /* `v_rcp_f32` is accurate to one ULP, not correctly rounded. The worst case
             * for a truncating consumer is a reciprocal a shade low, which turns `7 /
             * 7` into 0, so this models one ULP down every time rather than randomly.
             */
            case 42u:
                r = nextafterf(1.0f / a, (a > 0.0f) ? 0.0f : -3.0e38f);
                break;
            case 46u:
                r = 1.0f / sqrtf(a);
                break;
            case 51u:
                r = sqrtf(a);
                break;
            case 53u:
                r = sinf((float)(2.0 * PI) * a);
                break; /* revolutions */
            case 54u:
                r = cosf((float)(2.0 * PI) * a);
                break;
            default:
                ASSERT_TRUE(0);
                break;
            }
            if (s->exec)
                s->v[vdst] = r;
            continue;
        }
        if ((x >> 25) == 0x3eu) { /* VOPC, into vcc_lo */
            const uint32_t op = (x >> 17) & 0xffu;
            const float b = s->v[(x >> 9) & 0xffu];
            const float a = sim_src(s, x & 0x1ffu, w, &i);
            GLboolean r = GL_FALSE;
            switch (op) {
            case 1u:
                r = (GLboolean)(a < b);
                break;
            case 2u:
                r = (GLboolean)(a == b);
                break;
            case 3u:
                r = (GLboolean)(a <= b);
                break;
            case 4u:
                r = (GLboolean)(a > b);
                break;
            case 6u:
                r = (GLboolean)(a >= b);
                break;
            case 13u:
                r = (GLboolean)(a != b);
                break;
            default:
                ASSERT_TRUE(0);
                break;
            }
            /* A comparison writes zero for an inactive lane, so `s_and_saveexec_b32`
             * inside a dead branch narrows to nothing. */
            s->vcc = (GLboolean)(s->exec && r);
            continue;
        }
        /* SOPP: the branches. Tested before SOP2, whose top two bits it shares.
         * `simm16` counts from the word after the branch, so the target is
         * `i + 1 + simm`; the -1 below accounts for the loop's own `i++`. */
        if ((x >> 23) == 0x17fu) {
            const uint32_t op = (x >> 16) & 0x7fu;
            const int32_t simm = (int32_t)(int16_t)(uint16_t)(x & 0xffffu);
            GLboolean take;
            if (op == 2u)
                take = GL_TRUE; /* s_branch */
            else if (op == 5u)
                take = s->scc; /* s_cbranch_scc1 */
            else if (op == 8u)
                take = (GLboolean)!s->exec; /* s_cbranch_execz */
            else {
                ASSERT_TRUE(0);
                take = GL_FALSE;
            }
            if (take) {
                const int32_t target = (int32_t)i + 1 + simm;
                ASSERT_TRUE(target >= 0 && (uint32_t)target <= count);
                i = (uint32_t)target - 1u; /* the loop's `i++` lands on `target` */
            }
            continue;
        }
        /* SOPC: the scalar compare that sets SCC. Also before SOP2, for the same
         * reason. */
        if ((x >> 23) == 0x17eu) {
            const uint32_t op = (x >> 16) & 0x7fu;
            const uint32_t ssrc1 = (x >> 8) & 0xffu;
            const uint32_t ssrc0 = x & 0xffu;
            uint32_t rhs;
            ASSERT_EQ(op, 9u); /* s_cmp_ge_u32 */
            ASSERT_TRUE(ssrc0 < 128u);
            if (ssrc1 == 255u)
                rhs = w[++i]; /* the trailing literal */
            else {
                ASSERT_TRUE(ssrc1 >= 128u && ssrc1 <= 192u);
                rhs = ssrc1 - 128u;
            }
            s->scc = (GLboolean)(s->scount[ssrc0] >= rhs);
            continue;
        }
        if ((x >> 30) == 0x2u) { /* SOP2 */
            const uint32_t op = (x >> 23) & 0x7fu;
            const uint32_t sdst = (x >> 16) & 0x7fu;
            const uint32_t ssrc1 = (x >> 8) & 0xffu;
            const uint32_t ssrc0 = x & 0xffu;
            /* `s_add_u32` first: its operands are an integer and an inline constant,
             * neither of which `sim_mask` can read. Scalar arithmetic is not
             * exec-masked, which is what makes a trip guard a guard. */
            if (op == 0u) {
                ASSERT_TRUE(sdst < 128u && ssrc0 < 128u);
                ASSERT_TRUE(ssrc1 >= 128u && ssrc1 <= 192u);
                s->scount[sdst] = s->scount[ssrc0] + (ssrc1 - 128u);
                continue;
            }
            const GLboolean a = sim_mask(s, ssrc0);
            const GLboolean b = sim_mask(s, ssrc1);
            if (op == 14u) { /* s_and_b32 */
                sim_set_mask(s, sdst, (GLboolean)(a && b));
            } else if (op == 20u) { /* s_andn2_b32: a & ~b */
                sim_set_mask(s, sdst, (GLboolean)(a && !b));
            } else {
                ASSERT_TRUE(0);
            }
            continue;
        }
        /* VOP3, which this back end emits only for the four cube face instructions.
         * The mapping is the ISA's: the largest component picks the axis, its sign
         * picks which of the pair, and the other two become `sc` and `tc` with the
         * signs that keep every face oriented alike. `ma` is twice the major axis, so
         * the shader divides by `2|ma|`. */
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
                ma = 2.0f * Z;
                id = (Z < 0.0f) ? 5.0f : 4.0f;
                sc = (Z < 0.0f) ? -X : X;
                tc = -Y;
            } else if (ay >= ax) {
                ma = 2.0f * Y;
                id = (Y < 0.0f) ? 3.0f : 2.0f;
                sc = X;
                tc = (Y < 0.0f) ? -Z : Z;
            } else {
                ma = 2.0f * X;
                id = (X < 0.0f) ? 1.0f : 0.0f;
                sc = (X < 0.0f) ? Z : -Z;
                tc = -Y;
            }
            if (s->exec) {
                switch (op) {
                case 0x144u:
                    s->v[vdst] = id;
                    break;
                case 0x145u:
                    s->v[vdst] = sc;
                    break;
                case 0x146u:
                    s->v[vdst] = tc;
                    break;
                case 0x147u:
                    s->v[vdst] = ma;
                    break;
                default:
                    ASSERT_TRUE(0);
                    break;
                }
            }
            continue;
        }
        { /* VOP2 */
            const uint32_t op = (x >> 25) & 0x3fu;
            const uint32_t vdst = (x >> 17) & 0xffu;
            if (op == 47u) { /* v_cvt_pkrtz_f16_f32, the colour export's packing */
                /* Read straight out of the file, not through `sim_src`: these are
                   registers the shader wrote, so `sim_src`'s hardware-liveness check
                   does not apply. The result goes in the pack table and `s->v[vdst]` is
                   left alone, so a packed register read as a float is not plausible. */
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
            case 1u:
                r = s->vcc ? b : a;
                break; /* false value is src0 */
            case 3u:
                r = a + b;
                break;
            case 4u:
                r = a - b;
                break; /* src0 - vsrc1 */
            case 8u:
                r = a * b;
                break;
            case 15u:
                r = a < b ? a : b;
                break;
            case 16u:
                r = a > b ? a : b;
                break;
            case 43u:
                r = s->v[vdst] + a * b;
                break; /* v_fmac_f32 */
            default:
                ASSERT_TRUE(0);
                break;
            }
            if (s->exec)
                s->v[vdst] = r;
        }
    }
}

/* The window position the simulated SPI hands the shader, and the render target height.
 * No two are equal and none is 0 or 1, so reading the wrong component or skipping the y
 * flip gives a number no other component could. `y` is the hardware's, counted down
 * from the top, so `gl_FragCoord.y` is SIM_TARGET_H - SIM_FRAG_Y = 1059.5.
 *
 * The flip uses the render target's height, not the viewport's: `gl_FragCoord` is
 * window-relative. A viewport-sized number here would agree with a back end that
 * flipped by the viewport. */
#define SIM_TARGET_H 1080.0f
#define SIM_FRAG_X 10.5f
#define SIM_FRAG_Y 20.5f
#define SIM_FRAG_Z 0.25f
#define SIM_FRAG_W 2.0f
/* Positive, so `gl_FrontFacing` is true; distinct from every other seeded value. */
#define SIM_FRONT_FACE 7.5f

/* Compiles an already-linked program for the console, runs the words and returns
 * whether the lane survived, with the exported colour in `out`. The uniform block is
 * `p->values`, the pool `glUniform*` writes and the draw path copies verbatim. */
static GLboolean compile_and_run_prog(void *ctx, GLuint prog, const float attr[4][4],
                                      float out[4]) {
    const gl_program_object_t *p = gl_find_program((gl_context_t *)ctx, prog);
    ASSERT_TRUE(p != NULL);

    static uint32_t words[512];
    uint32_t count = 0u, vgprs = 0u, ena = 0u;
    char log[256] = {0};
    const GLboolean ok = gl_program_compile_fragment(p, words, 512u, &count, &vgprs,
                                                     NULL, &ena, log, sizeof(log));
    if (!ok)
        printf("\n    compile failed: %s\n", log);
    ASSERT_EQ(ok, GL_TRUE);
    /* Whatever it emitted has to fit the file the stage table allocated. */
    ASSERT_TRUE(vgprs <= 136u);

    sim_t s;
    memset(&s, 0, sizeof(s));
    s.exec = GL_TRUE; /* the lane starts live */
    s.ublock_floats = (int)(sizeof(s.ublock) / sizeof(s.ublock[0]));
    /* The block as the draw path builds it: descriptors first, the draw's own constants
     * in the second set's tail, the value pool at 0x80. */
    for (int i = 0; i < p->value_floats; i++) {
        s.ublock[OOPS_GL_GL2_UNIFORM_AT / 4 + i] = p->values[i];
    }
    s.ublock[OOPS_GL_GL2_DRAWCONST_AT / 4 + OOPS_GL_GL2_DC_TARGET_H] = SIM_TARGET_H;

    /* The SPI fills exactly what `input_ena` asked for, packed in order: the
     * barycentrics always, then the window position if named, then the face. Everything
     * else below v8 stays dead and `sim_src` refuses to read it, since the packing
     * moves the face between v2 and v6. */
    s.hw_vgpr_live[0] = GL_TRUE; /* the i barycentric */
    s.hw_vgpr_live[1] = GL_TRUE; /* and j */
    {
        uint32_t next = 2u;
        if ((ena & 0x00000f00u) != 0u) {
            s.v[next] = SIM_FRAG_X;
            s.hw_vgpr_live[next++] = GL_TRUE;
            s.v[next] = SIM_FRAG_Y;
            s.hw_vgpr_live[next++] = GL_TRUE;
            s.v[next] = SIM_FRAG_Z;
            s.hw_vgpr_live[next++] = GL_TRUE;
            s.v[next] = SIM_FRAG_W;
            s.hw_vgpr_live[next++] = GL_TRUE;
        }
        if ((ena & 0x00001000u) != 0u) {
            /* Positive is front-facing; the sign is the answer, not the value. */
            s.v[next] = SIM_FRONT_FACE;
            s.hw_vgpr_live[next++] = GL_TRUE;
        }
    }
    sim_run(&s, words, count, attr);
    ASSERT_EQ(s.exported, GL_TRUE);
    ASSERT_EQ(s.ended, GL_TRUE);
    for (int i = 0; i < 4; i++)
        out[i] = s.out[i];
    return s.lane_survived;
}

/* Links, then the above. The return is whether the lane survived to the export - false
 * when the shader discarded it. */
static GLboolean compile_and_run(void *ctx, const char *vs_src, const char *fs_src,
                                 const float attr[4][4], float out[4]) {
    return compile_and_run_prog(ctx, linked_program(vs_src, fs_src), attr, out);
}

/* The tolerance adds the colour export's own precision to what is asked for. The export
 * is two packed half-floats: an 8_8_8_8 target on a part with RB+ takes
 * `SPI_SHADER_FP16_ABGR` (`glsl_emit_export_mrt0`), and ten mantissa bits truncated is
 * up to one part in 1024. Scaling by the expected magnitude keeps the assertions sharp.
 */
#define ASSERT_NEAR(a, b, tol)                                                         \
    do {                                                                               \
        const float _a = (float)(a), _b = (float)(b);                                  \
        const float _d = _a > _b ? _a - _b : _b - _a;                                  \
        const float _mag = _b > 0.0f ? _b : -_b;                                       \
        const float _lim = (float)(tol) + _mag * (1.0f / 1024.0f);                     \
        if (!(_d <= _lim)) {                                                           \
            printf("\n    %s = %f, expected %f\n", #a, (double)_a, (double)_b);        \
        }                                                                              \
        ASSERT_TRUE(_d <= _lim);                                                       \
    } while (0)

/* The vertex shader every simulation below pairs with: one vec4 varying, which lands in
 * parameter 0 and is what `attr[0]` fills. */
static const char *const VS_ONE_VARYING =
    "attribute vec4 pos;\n"
    "varying vec4 vin;\n"
    "void main() { vin = pos; gl_Position = pos; }\n";

/* Compiled `gl_FragCoord` reads the SPI window position, with y flipped by the target.
 */
static void test_gl2_frag_coord_comes_from_the_window_position(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Every component in one export, so a wrong register shows as a wrong channel. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() { gl_FragColor = gl_FragCoord; }\n", attr, o);
    ASSERT_NEAR(o[0], SIM_FRAG_X, 1e-6f);
    /* y is flipped and the others are not: GL counts `gl_FragCoord.y` up from the
     * bottom and the hardware counts down from the top. */
    ASSERT_NEAR(o[1], SIM_TARGET_H - SIM_FRAG_Y, 1e-6f);
    ASSERT_NEAR(o[2], SIM_FRAG_Z, 1e-6f);
    ASSERT_NEAR(o[3], SIM_FRAG_W, 1e-6f);

    /* A swizzle reads the same registers in another order. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() { gl_FragColor = vec4(gl_FragCoord.yx, 0.0, 1.0); }\n",
                    attr, o);
    ASSERT_NEAR(o[0], SIM_TARGET_H - SIM_FRAG_Y, 1e-6f);
    ASSERT_NEAR(o[1], SIM_FRAG_X, 1e-6f);

    /* A shader that never names it is not charged for it: no scalar load, no four
     * registers, and `input_ena`, which configures the stage, asks only for
     * barycentrics. */
    gl_context_t *c = (gl_context_t *)ctx;
    const GLuint plain =
        linked_program(VS_ONE_VARYING, "void main() { gl_FragColor = vec4(1.0); }\n");
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u, ena = 0u, usg = 0u;
    char log[256] = {0};
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, plain), words, 256u,
                                          &count, &vgprs, &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    ASSERT_EQ(ena, 0x00000002u); /* the barycentrics only */
    ASSERT_EQ(usg, 0u);          /* and no block, so no user SGPRs */

    const GLuint uses =
        linked_program(VS_ONE_VARYING,
                       "void main() { gl_FragColor = vec4(gl_FragCoord.xyz, 1.0); }\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, uses), words, 256u, &count,
                                          &vgprs, &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    /* PERSP_CENTER plus POS_X/Y/Z/W - bits 8..11 of SPI_PS_INPUT_ENA for gfx103, which
     * is R_0286CC and not R_02865C (that address is this register only from gfx12). */
    ASSERT_EQ(ena, 0x00000f02u);
    /* It takes the block though it declares no uniform and samples nothing: the target
     * height that flips y lives there. */
    ASSERT_EQ(usg, 2u);

    glContextDestroy(ctx);
}

/* A loop with a constant trip count unrolls to the right number of bodies. */
static void test_gl2_loops_are_unrolled_when_the_count_is_known(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* 1+2+3+4+5 = 15, a sum no single iteration or stuck counter produces. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 1; i <= 5; i++) { total += float(i); }\n"
                    "  gl_FragColor = vec4(total * 0.01, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.15f, 1e-6f);

    /* Counting down with a step that is not one. The trip count follows the reference's
     * order, test then body then step: 10 + 8 + 6 + 4 + 2 = 30. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 10; i > 0; i -= 2) { total += float(i); }\n"
                    "  gl_FragColor = vec4(total * 0.01, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.30f, 1e-6f);

    /* A condition that is false at the start runs the body no times, which an unroller
     * that always emitted one copy would get wrong. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 7.0;\n"
                    "  for (int i = 0; i < 0; i++) { total = 0.0; }\n"
                    "  gl_FragColor = vec4(total * 0.1, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.7f, 1e-6f);

    /* The counter is a fresh value each trip, so a body that assigns to it does not
     * carry the change into the next one. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "void main() {\n"
        "  float total = 0.0;\n"
        "  for (int i = 0; i < 3; i++) { int j = i; j = j + 10; total += float(j); }\n"
        "  gl_FragColor = vec4(total * 0.01, 0.0, 0.0, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.33f, 1e-6f); /* 10 + 11 + 12 */

    /* A `break` belongs to the loop it is in: the search for one stays inside this
     * loop's own body and stops at a nested loop, so both loops here unroll. */
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

/* Branched loops run the right trips with `break`, `continue` and `discard`. Every
 * value is one the unrolled path cannot produce. One lane shows the per-lane semantics,
 * not divergence between lanes. */
static void test_gl2_loops_that_branch_run_break_and_continue(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* 200 trips, more than the unroller writes out. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float total = 0.0;\n"
                    "  for (int i = 0; i < 200; i++) { total += 1.0; }\n"
                    "  gl_FragColor = vec4(total * 0.001, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.2f, 1e-6f);

    /* `break` leaves on the sixth trip, so the answer is 5 and not 200. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "void main() {\n"
        "  float total = 0.0;\n"
        "  for (int i = 0; i < 200; i++) { if (total >= 5.0) break; total += 1.0; }\n"
        "  gl_FragColor = vec4(total * 0.1, 0.0, 0.0, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-6f);

    /* `continue` skips the rest of the body and still counts the trip: the step runs
     * under the loop's mask, not the body's. A step skipped with the body would give
     * 100 instead of 97. */
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

    /* A `break` two `if`s deep. It has to take the lane out of both saved masks on the
     * way out, or the inner restore hands it back and the loop carries on to 200. */
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

    /* The inner `break` belongs to the inner loop. Each outer pass adds 91 and the
     * outer loop stops once the total passes 200, after three passes; a `break`
     * reaching the outer mask would stop at 91. */
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

    /* A condition false on arrival runs the body no times: the `s_cbranch_execz` exit.
     */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "void main() {\n"
        "  float total = 7.0;\n"
        "  for (int i = 0; i < 0; i++) { if (total > 0.0) break; total = 0.0; }\n"
        "  gl_FragColor = vec4(total * 0.1, 0.0, 0.0, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.7f, 1e-6f);

    /* A `discard` inside a loop survives the next trip. The loop reloads `exec` from
     * its active mask every trip, so the discarded lane must leave that mask too. */
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

/* gl2-probe's `control-flow` and `short-circuit` shaders compile and run on the back
 * end. These are the probe's exact sources. */
static void test_gl2_the_probes_control_flow_shaders_compile_and_run(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* `control-flow`: 99 trips with a `break` at 5 and a `continue` on the way, over
     * integer comparisons (an `int` here is a float kept whole). 15 * 0.05 = 0.75. */
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

    /* `short-circuit`: the right operand writes through an `out` parameter, so whether
     * it ran shows in the answer. `never && mark(a)` leaves `a` at zero and
     * `always || mark(b)` leaves `b` at zero. */
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

    /* The other way round: the left operand does not decide, so the right one runs and
     * its mark lands. */
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

    /* gl2-probe's `loop-divergence` shader at both ends of its gradient. One lane
     * cannot diverge, but the trip count must follow the varying, or the probe's
     * gradient would be flat for a reason unrelated to the masks. */
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
        const float lo[4][4] = {
            {0.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        const float hi[4][4] = {
            {1.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        compile_and_run(ctx, VS_ONE_VARYING, FS_DIVERGE, lo, o);
        ASSERT_NEAR(o[0], 0.03f, 1e-6f); /* one trip: i = 0 is not > 0 */
        compile_and_run(ctx, VS_ONE_VARYING, FS_DIVERGE, hi, o);
        ASSERT_NEAR(o[0], 0.99f, 1e-6f); /* thirty-three: i = 0..32 */
    }

    /* `discard-in-loop` on both sides of its threshold: the discarding side does not
     * come back when the loop reloads `exec`. A hundred trips so the loop branches; an
     * unrolled loop has no reload, and 24 copies of this body exceed 512 instructions.
     */
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
        const float keep[4][4] = {
            {0.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        const float kill[4][4] = {
            {1.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, FS_DISCARD_LOOP, keep, o),
                  GL_TRUE);
        ASSERT_NEAR(o[1], 1.0f, 1e-6f);
        ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, FS_DISCARD_LOOP, kill, o),
                  GL_FALSE);
    }

    /* `early-return` and `local-arrays`, the probe's own sources, through the compiled
     * path; the software reference running them says nothing about it. */
    {
        static const char *const FS_EARLY = "varying vec4 vin;\n"
                                            "float pick(float a) {\n"
                                            "  if (a < 0.5) { return 0.25; }\n"
                                            "  return 1.0;\n"
                                            "}\n"
                                            "void main() {\n"
                                            "  float r = pick(vin.x);\n"
                                            "  gl_FragColor = vec4(r, 1.0, 0.0, 1.0);\n"
                                            "}\n";
        const float lo[4][4] = {
            {0.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        const float hi[4][4] = {
            {1.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
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
    ASSERT_NEAR(o[0], 1.0f, 1e-6f); /* 1+2+3+4 = 10 */
    ASSERT_NEAR(o[1], 0.75f, 1e-6f);
    ASSERT_NEAR(o[2], 0.5f, 1e-6f);

    /* gl2-probe's `texture-cube` shader: the direction comes from a varying rather than
     * a literal, so the components the face selection reads are interpolated
     * registers. */
    {
        const float hi[4][4] = {
            {1.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
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

    /* `^^` has no short-circuit in the language: both sides run and the mark lands. */
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

/* Integer comparisons compile to the float comparison of the same registers. */
static void test_gl2_integer_comparisons_are_the_float_ones(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Each channel is a different operator. `==` on integers is exact, since whole
     * numbers have one representation each. */
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

    /* A negative integer, where truncation towards zero and the comparison agree:
     * -7/2 = -3, not -4. */
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

/* A branched loop carries its trip guard in the emitted words. The guard guards against
 * a generator bug and no shader with a known trip count makes it fire, so its presence
 * is checked by reading the words. */
static void test_gl2_a_branched_loop_carries_its_trip_guard(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[512];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};

    const GLuint prog = linked_program(
        VS_ONE_VARYING, "void main() {\n"
                        "  float total = 0.0;\n"
                        "  for (int i = 0; i < 200; i++) { total += 1.0; }\n"
                        "  gl_FragColor = vec4(total * 0.001, 0.0, 0.0, 1.0);\n"
                        "}\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, prog), words, 512u, &count,
                                          &vgprs, NULL, NULL, log, sizeof(log)),
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
            if (op == 5u)
                guards++; /* s_cbranch_scc1 */
        }
        if ((x >> 23) == 0x17eu && ((x >> 16) & 0x7fu) == 9u) {
            compares++; /* s_cmp_ge_u32 */
            i++;        /* its literal: 200 does not inline */
        }
    }
    ASSERT_EQ(backward, 1); /* one loop, one way round */
    ASSERT_EQ(compares, 1);
    ASSERT_EQ(guards, 1);
    /* The jump goes back into the shader, not past its start, and not forward. */
    ASSERT_TRUE(back_target < back_at);
    /* It ends with `s_endpgm` and the export tail's two words of room after it. */
    ASSERT_EQ(words[count - 3u], 0xbf810000u);
    /* The ceiling is this loop's trip count, not a round number. */
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

/* The back end refuses, by name, the loops its trip guard cannot bound. */
static void test_gl2_the_back_end_refuses_the_loops_it_cannot_bound(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};

    static const struct {
        const char *fs;
        const char *wants;
    } cases[] = {
        /*
         * A known trip count larger than the guard's ceiling: the loop would run 65536
         * times instead of 100000 and draw a wrong colour with no error. Loops with an
         * unknown count are bounded by the guard and covered in
         * `test_gl2_compiled_unbounded_for_loops`.
         */
        {"void main() {\n"
         "  float t = 0.0;\n"
         "  for (int i = 0; i < 100000; i++) { t += 1.0; }\n"
         "  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);\n"
         "}\n",
         "bound"},
        /* Branched loops nested deeper than the scalar registers set aside for their
         * masks. Each of these three has a `break`, so each one branches. */
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
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, prog), words, 256u,
                                              &count, &vgprs, NULL, NULL, log,
                                              sizeof(log)),
                  GL_FALSE);
        if (strstr(log, cases[i].wants) == NULL) {
            printf("\n    case %d: expected a message about '%s', got '%s'\n", (int)i,
                   cases[i].wants, log);
        }
        ASSERT_TRUE(strstr(log, cases[i].wants) != NULL);
    }

    glContextDestroy(ctx);
}

/* Derivatives compile to DPP quad reads on the right axis under whole-quad mode. */
static void test_gl2_derivatives_are_quad_reads_under_whole_quad_mode(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u, ena = 0u, usg = 0u;
    char log[256] = {0};

    /* The four permutes differ only in the DPP control byte, so the test names it.
     * `dFdx` takes the right-hand column less the left, `dFdy` the bottom row less the
     * top. */
    const GLuint dx = linked_program(
        VS_ONE_VARYING,
        "varying vec4 vin;\n"
        "void main() { gl_FragColor = vec4(dFdx(vin.x), 0.0, 0.0, 1.0); }\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, dx), words, 256u, &count,
                                          &vgprs, &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    {
        GLboolean far_x = GL_FALSE, near_x = GL_FALSE, any_y = GL_FALSE, wqm = GL_FALSE;
        for (uint32_t i = 0; i + 1 < count; i++) {
            if ((words[i] & 0x1ffu) != 0xfau)
                continue;
            const uint32_t ctrl = (words[i + 1] >> 8) & 0xffu;
            if (ctrl == GLSL_DPP_QUAD_X_FAR)
                far_x = GL_TRUE;
            if (ctrl == GLSL_DPP_QUAD_X_NEAR)
                near_x = GL_TRUE;
            if (ctrl == GLSL_DPP_QUAD_Y_FAR || ctrl == GLSL_DPP_QUAD_Y_NEAR)
                any_y = GL_TRUE;
        }
        /* `s_wqm_b32 exec_lo, exec_lo`: a quad read needs it because the neighbouring
         * lane may be one the primitive does not cover. */
        for (uint32_t i = 0; i < count; i++) {
            if (words[i] == 0xbefe097eu)
                wqm = GL_TRUE;
        }
        ASSERT_TRUE(far_x);
        ASSERT_TRUE(near_x);
        ASSERT_TRUE(!any_y); /* dFdx reaches for no row permute */
        ASSERT_TRUE(wqm);
    }

    /* `dFdy` is the same shape on the other axis, with the x permutes absent. */
    const GLuint dy = linked_program(
        VS_ONE_VARYING,
        "varying vec4 vin;\n"
        "void main() { gl_FragColor = vec4(dFdy(vin.x), 0.0, 0.0, 1.0); }\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, dy), words, 256u, &count,
                                          &vgprs, &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    {
        GLboolean far_y = GL_FALSE, near_y = GL_FALSE, any_x = GL_FALSE;
        for (uint32_t i = 0; i + 1 < count; i++) {
            if ((words[i] & 0x1ffu) != 0xfau)
                continue;
            const uint32_t ctrl = (words[i + 1] >> 8) & 0xffu;
            if (ctrl == GLSL_DPP_QUAD_Y_FAR)
                far_y = GL_TRUE;
            if (ctrl == GLSL_DPP_QUAD_Y_NEAR)
                near_y = GL_TRUE;
            if (ctrl == GLSL_DPP_QUAD_X_FAR || ctrl == GLSL_DPP_QUAD_X_NEAR)
                any_x = GL_TRUE;
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
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, fw), words, 256u, &count,
                                          &vgprs, &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    {
        int seen = 0;
        for (uint32_t i = 0; i + 1 < count; i++) {
            if ((words[i] & 0x1ffu) != 0xfau)
                continue;
            const uint32_t ctrl = (words[i + 1] >> 8) & 0xffu;
            if (ctrl == GLSL_DPP_QUAD_X_FAR || ctrl == GLSL_DPP_QUAD_X_NEAR ||
                ctrl == GLSL_DPP_QUAD_Y_FAR || ctrl == GLSL_DPP_QUAD_Y_NEAR) {
                seen++;
            }
        }
        ASSERT_TRUE(seen >= 4);
    }

    /* A shader that names none of them does not enter whole-quad mode. */
    const GLuint plain =
        linked_program(VS_ONE_VARYING, "void main() { gl_FragColor = vec4(1.0); }\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, plain), words, 256u,
                                          &count, &vgprs, &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    for (uint32_t i = 0; i < count; i++)
        ASSERT_TRUE(words[i] != 0xbefe097eu);

    glContextDestroy(ctx);
}

/* `gl_FragDepth` exports to mrtz before the colour, and only the colour says `done`. */
static void test_gl2_frag_depth_exports_before_the_colour(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u, ena = 0u, usg = 0u;
    char log[256] = {0};

    const GLuint d =
        linked_program(VS_ONE_VARYING, "void main() {\n"
                                       "  gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
                                       "  gl_FragDepth = 0.99;\n"
                                       "}\n");
    const gl_program_object_t *pd = gl_find_program(c, d);
    ASSERT_TRUE(pd != NULL);
    ASSERT_EQ(pd->hw_ps_exports_depth, GL_TRUE);
    ASSERT_EQ(gl_program_compile_fragment(pd, words, 256u, &count, &vgprs, &usg, &ena,
                                          log, sizeof(log)),
              GL_TRUE);
    /* It asks for the window position, because the depth it starts from is the
     * interpolated z. */
    ASSERT_EQ(ena & 0x00000f00u, 0x00000f00u);

    /* The depth export comes first without `done`; the colour export second with it. A
     * shader with two `done` exports, or the colour first, does not retire. */
    {
        int z_at = -1, c_at = -1;
        for (uint32_t i = 0; i + 1 < count; i++) {
            if ((words[i] >> 26) != 0x3eu)
                continue;
            const uint32_t target = (words[i] >> 4) & 0x3fu;
            if (target == 8u)
                z_at = (int)i;
            if (target == 0u)
                c_at = (int)i;
        }
        ASSERT_TRUE(z_at >= 0);
        ASSERT_TRUE(c_at >= 0);
        ASSERT_TRUE(z_at < c_at);
        ASSERT_EQ((words[z_at] >> 11) & 1u, 0u); /* the depth does not say done */
        ASSERT_EQ((words[c_at] >> 11) & 1u, 1u); /* the colour does */
    }

    /* A shader that never names it exports no depth. */
    const GLuint plain =
        linked_program(VS_ONE_VARYING, "void main() { gl_FragColor = vec4(1.0); }\n");
    const gl_program_object_t *pp = gl_find_program(c, plain);
    ASSERT_TRUE(pp != NULL);
    ASSERT_EQ(pp->hw_ps_exports_depth, GL_FALSE);
    ASSERT_EQ(gl_program_compile_fragment(pp, words, 256u, &count, &vgprs, &usg, &ena,
                                          log, sizeof(log)),
              GL_TRUE);
    for (uint32_t i = 0; i < count; i++) {
        if ((words[i] >> 26) == 0x3eu)
            ASSERT_TRUE(((words[i] >> 4) & 0x3fu) != 8u);
    }

    glContextDestroy(ctx);
}

/* Vector relationals compare per component, and `any`/`all`/`not` reduce a bvec. */
static void test_gl2_vector_relationals_reduce_a_bvec(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* `any` and `all` are a max and a min, since a bvec component is exactly 0.0
     * or 1.0. `lessThan((0,1,2), (1,1,1))` is `(true, false, false)`: `any` is true,
     * `all` is false, and `all(not(c))` is false. */
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

    /* `not` on its own, and a comparison whose answer differs per component, so a
     * comparison done once and broadcast fails. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
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

/* `gl_Color` in a fragment shader reads the parameter the link assigned it. */
static void test_gl2_gl_color_lands_where_the_link_put_it(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;

    /* With a vertex shader the user's varyings own the parameters and the colour
     * follows them: `VS_ONE_VARYING`'s `vec4` is parameter 0, so `gl_Color` is
     * parameter 1. */
    const GLuint withvs = linked_program(
        VS_ONE_VARYING, "varying vec4 vin;\n"
                        "void main() { gl_FragColor = gl_Color * vin.x; }\n");
    const gl_program_object_t *pv = gl_find_program(c, withvs);
    ASSERT_TRUE(pv != NULL);
    ASSERT_EQ(pv->hw_color_param, 1);
    ASSERT_TRUE(pv->hw_params >= 2u);

    /* Without one, the fixed-function vertex path writes the colour into parameter 0,
     * so the slot already exists. */
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
    const GLuint plain =
        linked_program(VS_ONE_VARYING, "varying vec4 vin;\n"
                                       "void main() { gl_FragColor = vin; }\n");
    const gl_program_object_t *pp = gl_find_program(c, plain);
    ASSERT_TRUE(pp != NULL);
    ASSERT_EQ(pp->hw_color_param, -1);

    /* The interpolation reads the parameter the link chose: `attr[1]` is the colour and
     * `attr[0]` a different value in the varying. */
    float o[4];
    const float attr[4][4] = {{0.5f, 0, 0, 0},            /* param0: the user varying */
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

/* `m * v` and `v * m` compile to different products, and matrix built-ins compute. */
static void test_gl2_matrix_products_are_two_products(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* `v * m` is the product with the transpose. An asymmetric matrix separates them:
     * with col0 = (1,2) and col1 = (0,1),
     *
     *   m * (1,0) = col0          = (1, 2)
     *   (1,0) * m = (v.col0, v.col1) = (1, 0) */
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

    /* `mat2(0.5)` is a diagonal, not four halves. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  mat2 m = mat2(0.5);\n"
                    "  vec2 v = m * vec2(1.0, 1.0);\n"
                    "  gl_FragColor = vec4(v, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-6f); /* not 1.0, which four halves would give */
    ASSERT_NEAR(o[1], 0.5f, 1e-6f);

    /* Three dimensions, so the column stride is exercised at a second value.
     * col0 = (1,0,0), col1 = (0,2,0), col2 = (3,0,4). */
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

/* Integers are floats truncated toward zero after every operation, as in the reference.
 */
static void test_gl2_integers_are_floats_kept_whole(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* The representation is the reference's: an int is a float and every integer
     * operation is followed by a truncation towards zero, `(float)(int)x` in
     * `glsl_exec.c`. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "void main() {\n"
        "  int a = 7;\n"
        "  int b = 3;\n"
        "  gl_FragColor = vec4(float(a + b), float(a - b), float(a * b), 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 10.0f, 1e-6f);
    ASSERT_NEAR(o[1], 4.0f, 1e-6f);
    ASSERT_NEAR(o[2], 21.0f, 1e-6f);

    /* `int(7.9)` is 7 and `int(-7.9)` is -7: toward zero, not `floor`. */
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

    /* The truncation after an integer operation is asserted on the instructions, since
     * on whole values it changes nothing. The float arm is the control: the same
     * expression with a float type emits none. */
    {
        const GLuint ip =
            linked_program(VS_ONE_VARYING, "void main() { int a = 7; int b = 3; "
                                           "gl_FragColor = vec4(float(a * b)); }\n");
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, ip), w2, 256u, &n2,
                                              &vg, NULL, NULL, lg, sizeof(lg)),
                  GL_TRUE);
        int truncs = 0;
        for (uint32_t i = 0; i < n2; i++) {
            if ((w2[i] >> 25) == 0x3fu &&
                ((w2[i] >> 9) & 0xffu) == GLSL_VOP1_TRUNC_F32) {
                truncs++;
            }
        }
        ASSERT_TRUE(truncs > 0);

        const GLuint fp =
            linked_program(VS_ONE_VARYING, "void main() { float a = 7.0; float b = "
                                           "3.0; gl_FragColor = vec4(a * b); }\n");
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, fp), w2, 256u, &n2,
                                              &vg, NULL, NULL, lg, sizeof(lg)),
                  GL_TRUE);
        int float_truncs = 0;
        for (uint32_t i = 0; i < n2; i++) {
            if ((w2[i] >> 25) == 0x3fu &&
                ((w2[i] >> 9) & 0xffu) == GLSL_VOP1_TRUNC_F32) {
                float_truncs++;
            }
        }
        ASSERT_EQ(float_truncs, 0);
    }

    /* Integer division against a reciprocal one ULP low, as the simulator models
     * `v_rcp_f32`: `a * rcp(a)` lands under 1.0, so an uncorrected `trunc` is off by
     * one on every exact case below. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  gl_FragColor = vec4(float(7 / 7), float(49 / 7),\n"
                    "                      float(100 / 10), float(6 / 3)) * 0.01;\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.01f, 1e-6f); /* 1, not 0 */
    ASSERT_NEAR(o[1], 0.07f, 1e-6f); /* 7, not 6 */
    ASSERT_NEAR(o[2], 0.10f, 1e-6f);
    ASSERT_NEAR(o[3], 0.02f, 1e-6f);

    /* Truncation is toward zero on both signs, C's rule and GLSL's; a floor would be
     * off by one on every negative quotient. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  gl_FragColor = vec4(float(7 / 2), float(-7 / 2),\n"
                    "                      float(7 / -2), float(-7 / -2)) * 0.1;\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.3f, 1e-6f);  /* 3 */
    ASSERT_NEAR(o[1], -0.3f, 1e-6f); /* -3, not -4 */
    ASSERT_NEAR(o[2], -0.3f, 1e-6f);
    ASSERT_NEAR(o[3], 0.3f, 1e-6f);

    /* A large quotient. The reciprocal's relative error is about 2^-23, so one
     * correction suffices below roughly eight million, where integers stop being exact
     * in a float anyway. 999999 / 3 is 333333 exactly. */
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

    /* Division by zero answers zero, as the reference does. The language leaves it
     * undefined; the two paths agree. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  int z = 0;\n"
                    "  gl_FragColor = vec4(float(5 / z), 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, 1e-6f);

    glContextDestroy(ctx);
}

/* `gl_FrontFacing` compares the SPI face register's sign, read from where it is packed.
 */
static void test_gl2_front_facing_is_a_sign_not_a_flag(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u, ena = 0u, usg = 0u;
    char log[256] = {0};

    /* The register is a float whose sign is the answer, not a zero-or-one flag: Mesa
     * lowers `load_front_face` as `fgt(reg, 0)`. So the prologue compares against zero
     * rather than moving the register into a bool. */
    const GLuint ff =
        linked_program(VS_ONE_VARYING, "void main() {\n"
                                       "  gl_FragColor = gl_FrontFacing ? vec4(0.0, "
                                       "1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0);\n"
                                       "}\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, ff), words, 256u, &count,
                                          &vgprs, &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    /* PERSP_CENTER and FRONT_FACE, and not the window position, which this shader never
     * reads. */
    ASSERT_EQ(ena, 0x00001002u);
    /* No block either: the face arrives in a register, not through the payload. */
    ASSERT_EQ(usg, 0u);
    {
        /* The prologue holds a `GT_F32` compare of the face against zero. */
        GLboolean saw_cmp = GL_FALSE;
        for (uint32_t i = 0; i < count; i++) {
            if ((words[i] >> 25) == 0x3eu &&
                ((words[i] >> 17) & 0xffu) == GLSL_VOPC_GT_F32 &&
                (words[i] & 0x1ffu) ==
                    256u + 2u) { /* v2: straight after the barycentrics */
                saw_cmp = GL_TRUE;
                break;
            }
        }
        ASSERT_TRUE(saw_cmp);
    }

    /* Both together move the face register: the SPI packs what it was asked for in
     * order, so with the position enabled the face follows it. A fixed face register
     * would compare the window's w against zero. */
    const GLuint both = linked_program(
        VS_ONE_VARYING, "void main() {\n"
                        "  float d = gl_FrontFacing ? 1.0 : 0.0;\n"
                        "  gl_FragColor = vec4(d, gl_FragCoord.y, 0.0, 1.0);\n"
                        "}\n");
    ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, both), words, 256u, &count,
                                          &vgprs, &usg, &ena, log, sizeof(log)),
              GL_TRUE);
    ASSERT_EQ(ena, 0x00001f02u); /* PERSP_CENTER | POS_XYZW | FRONT_FACE */
    ASSERT_EQ(usg, 2u);          /* and the block, for the height that flips y */
    {
        /* v6, not v2: two barycentrics, then x, y, z, w, then the face. The face is the
         * compare's `src0`, the nine-bit operand where a VGPR reads as 256 + its
         * number; `vsrc1` holds zero. */
        GLboolean saw_v6 = GL_FALSE;
        for (uint32_t i = 0; i < count; i++) {
            if ((words[i] >> 25) == 0x3eu &&
                ((words[i] >> 17) & 0xffu) == GLSL_VOPC_GT_F32 &&
                (words[i] & 0x1ffu) == 256u + 6u) {
                saw_v6 = GL_TRUE;
                break;
            }
        }
        ASSERT_TRUE(saw_v6);
    }

    /* `gl_FragDepth` asks for the window position too, so a shader naming it and the
     * face but not `gl_FragCoord` still has the face at v6. Keying the offset on
     * `gl_FragCoord` alone would read the position's w as the face. Also run below. */
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
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, both2), w3, 256u, &n3,
                                              &v3, &u3, &e3, l3, sizeof(l3)),
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

    /* Run, not only inspected: the simulator fills exactly the registers `input_ena`
     * asked for and refuses to read any other, so a face read from the wrong register
     * fails. Both shapes are run. */
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "void main() {\n"
        "  gl_FragColor = vec4(gl_FrontFacing ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f); /* the seeded face is positive, so front */

    compile_and_run(
        ctx, VS_ONE_VARYING,
        "void main() {\n"
        "  gl_FragColor = vec4(gl_FrontFacing ? 1.0 : 0.0, gl_FragCoord.y,\n"
        "                      gl_FragCoord.x, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f);
    ASSERT_NEAR(o[1], SIM_TARGET_H - SIM_FRAG_Y, 1e-6f);
    ASSERT_NEAR(o[2], SIM_FRAG_X, 1e-6f);

    /* The face beside `gl_FragDepth` and without `gl_FragCoord`, the shape above, run
     * through the simulator's liveness check. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "void main() {\n"
        "  gl_FragDepth = 0.5;\n"
        "  gl_FragColor = vec4(gl_FrontFacing ? 1.0 : 0.0, 0.0, 0.0, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 1.0f, 1e-6f);

    glContextDestroy(ctx);
}

/* User functions inline with GLSL's parameter semantics and release their registers. */
static void test_gl2_user_functions_are_inlined(void) {
    void *ctx = gl2_context();
    float o[4];
    const float x = 0.75f, y = 0.25f, z = 3.0f, w = 2.0f;
    const float attr[4][4] = {{x, y, z, w}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Arguments bind by position, not by name, and are evaluated in the caller's
     * scope. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "varying vec4 vin;\n"
        "float sub(float a, float b) { return a - b; }\n"
        "void main() {\n"
        "  gl_FragColor = vec4(sub(vin.x, vin.y), sub(vin.y, vin.x), 0.0, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], x - y, 1e-6f);
    ASSERT_NEAR(o[1], y - x, 1e-6f);

    /* A parameter shadows a caller's variable of the same name and gives it back. The
     * argument is evaluated before the parameter is bound, and after the call the
     * caller's `a` is untouched. */
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

    /* Nesting, locals inside a body, and a vector return: one call feeding another, so
     * a result register reused across the two shows. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
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

    /* `inout` is pass-by-value-and-copy-back, not pass-by-reference. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
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

    /* An `out` parameter, and a void function called as a statement, whose result of no
     * width is expected rather than a failure. */
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

    /* A call gives its registers back: an inlined call's parameters and temporaries are
     * live only while its body is generated. Ten calls to a helper with three locals
     * stay near one call's cost. */
    {
        gl_context_t *cc = (gl_context_t *)ctx;
        const GLuint many = linked_program(
            VS_ONE_VARYING,
            "varying vec4 vin;\n"
            "float h(float a) { float b = a * 2.0; float c = b + 1.0; return c * 0.5; "
            "}\n"
            "void main() {\n"
            "  float t = h(vin.x) + h(vin.y) + h(vin.z) + h(vin.w) + h(vin.x)\n"
            "          + h(vin.y) + h(vin.z) + h(vin.w) + h(vin.x) + h(vin.y);\n"
            "  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);\n"
            "}\n");
        uint32_t wm[512];
        uint32_t nm = 0u, vm = 0u;
        char lm[256] = {0};
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(cc, many), wm, 512u, &nm,
                                              &vm, NULL, NULL, lm, sizeof(lm)),
                  GL_TRUE);
        /* 50 with the release and 77 without it; the remainder is the ten results and
         * the sum's temporaries, all live at once. The threshold sits between the two.
         */
        ASSERT_TRUE(vm < 64u);
    }

    /* A user function hiding a built-in cannot reach the back end: the front end
     * refuses `float min(float, float)`. The generator would prefer the user's
     * definition. */

    glContextDestroy(ctx);
}

/* Matrix-by-matrix products, scalar broadcasts and matrix built-ins compute correctly.
 * Every value makes the transposed, componentwise and product results three different
 * numbers, since symmetric operands would pass with rows and columns swapped. */
static void test_gl2_matrix_by_matrix_and_by_scalar(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* `m * m` is a product and not componentwise. `a` is a shear and `b` a scale, so
     * `a * b` and `b * a` differ.
     *
     * Column-major: `mat2(1, 2, 0, 1)` is col0 = (1,2), col1 = (0,1).
     *   a = [[1,0],[2,1]] as (row, col);  b = [[3,0],[0,4]]
     *   a*b: col0 = a * (3,0) = (3, 6);   col1 = a * (0,4) = (0, 4)
     *   b*a: col0 = b * (1,2) = (3, 8);   col1 = b * (0,1) = (0, 4)
     * The (1,0) element is 6 one way and 8 the other. Componentwise would give 3. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "void main() {\n"
        "  mat2 a = mat2(1.0, 2.0, 0.0, 1.0);\n"
        "  mat2 b = mat2(3.0, 0.0, 0.0, 4.0);\n"
        "  mat2 ab = a * b;\n"
        "  mat2 ba = b * a;\n"
        "  gl_FragColor = vec4(ab[0][1] * 0.1, ba[0][1] * 0.1, ab[1][1] * 0.1, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.6f, 1e-6f); /* a*b at (1,0) */
    ASSERT_NEAR(o[1], 0.8f, 1e-6f); /* b*a at (1,0), the other product */
    ASSERT_NEAR(o[2], 0.4f, 1e-6f);

    /* `mat3 * mat3`, so the size is not baked in. A scale down the diagonal multiplies
     * each column by its own factor. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
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

    /* A matrix with a scalar is componentwise and broadcast, both ways round, and a
     * matrix with a matrix under `+` is componentwise too. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
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

    /* `matrixCompMult` is the componentwise product, so it differs from `a * b` on the
     * same operands. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
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

    /* `transpose` swaps the indices, so the matrix is asymmetric. It is GLSL 1.20. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
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

    /* `outerProduct(c, r)` puts `c` down the columns; the other order is the transpose,
     * so the two off-diagonal elements are the test. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
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

/*
 * GLSL 1.20's non-square matrices use the column count and the row count each where it
 * belongs. On a square matrix the two are the same number, so these shapes disagree: a
 * `mat2x3` has two columns of three, `m * vec2` is a `vec3` and `vec3 * m` a `vec2`,
 * and a stride from the wrong side reads the next column or past the end.
 */
static void test_gl2_non_square_matrices(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* `mat2x3 * vec2` is a `vec3`; the vector's width matches the columns.
     *
     * m = mat2x3(1,2,3, 4,5,6): col0 = (1,2,3), col1 = (4,5,6).
     * m * (1, 10) = col0 * 1 + col1 * 10 = (41, 52, 63).
     *
     * A stride of 2 would read col1 from element 2, giving (3,4,5). */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "#version 120\n"
                    "void main() {\n"
                    "  mat2x3 m = mat2x3(1.0, 2.0, 3.0,  4.0, 5.0, 6.0);\n"
                    "  vec3 p = m * vec2(1.0, 10.0);\n"
                    "  gl_FragColor = vec4(p.x * 0.01, p.y * 0.01, p.z * 0.01, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.41f, 1e-5f);
    ASSERT_NEAR(o[1], 0.52f, 1e-5f);
    ASSERT_NEAR(o[2], 0.63f, 1e-5f);

    /* `vec3 * mat2x3` is a `vec2`: the vector matches the rows and the result has one
     * entry per column.
     *
     * (1, 10, 100) * m = (dot with col0, dot with col1) = (1+20+300, 4+50+600) = (321,
     * 654). */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "#version 120\n"
                    "void main() {\n"
                    "  mat2x3 m = mat2x3(1.0, 2.0, 3.0,  4.0, 5.0, 6.0);\n"
                    "  vec2 q = vec3(1.0, 10.0, 100.0) * m;\n"
                    "  gl_FragColor = vec4(q.x * 0.001, q.y * 0.001, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.321f, 1e-5f);
    ASSERT_NEAR(o[1], 0.654f, 1e-5f);

    /* `[]` gives a column, whose length is the row count. `m[1]` is (4,5,6), so
     * `m[1][2]` is 6 and `m[0][2]` is 3; `m[2]` is out of range, with two columns. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#version 120\n"
        "void main() {\n"
        "  mat2x3 m = mat2x3(1.0, 2.0, 3.0,  4.0, 5.0, 6.0);\n"
        "  gl_FragColor = vec4(m[1][2] * 0.1, m[0][2] * 0.1, m[1][0] * 0.1, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.6f, 1e-6f);
    ASSERT_NEAR(o[1], 0.3f, 1e-6f);
    ASSERT_NEAR(o[2], 0.4f, 1e-6f);

    /* `transpose` changes the shape: a `mat2x3` transposes to a `mat3x2`. t has
     * col0 = (1,4), col1 = (2,5), col2 = (3,6), so `t[2]` exists and `m[2]` does not.
     */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#version 120\n"
        "void main() {\n"
        "  mat2x3 m = mat2x3(1.0, 2.0, 3.0,  4.0, 5.0, 6.0);\n"
        "  mat3x2 t = transpose(m);\n"
        "  gl_FragColor = vec4(t[0][1] * 0.1, t[2][0] * 0.1, t[1][1] * 0.1, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.4f, 1e-6f);
    ASSERT_NEAR(o[1], 0.3f, 1e-6f);
    ASSERT_NEAR(o[2], 0.5f, 1e-6f);

    /* `matCxR * matPxC` is a `matPxR`, neither operand's shape.
     *
     * A = mat3x2(1,2, 3,4, 5,6) is 2 rows by 3 columns; B = mat2x3(1,0,2, 0,1,3) is 3
     * by 2. A * B is a `mat2` with col0 = (11, 14) and col1 = (18, 22). */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#version 120\n"
        "void main() {\n"
        "  mat3x2 a = mat3x2(1.0, 2.0,  3.0, 4.0,  5.0, 6.0);\n"
        "  mat2x3 b = mat2x3(1.0, 0.0, 2.0,  0.0, 1.0, 3.0);\n"
        "  mat2 p = a * b;\n"
        "  gl_FragColor = vec4(p[0][0] * 0.01, p[0][1] * 0.01, p[1][1] * 0.01, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.11f, 1e-5f);
    ASSERT_NEAR(o[1], 0.14f, 1e-5f);
    ASSERT_NEAR(o[2], 0.22f, 1e-5f);

    /* The same two matrices the other way round give a `mat3`, larger than either.
     * B * A has col0 = (1,2,8), col1 = (3,4,18), col2 = (5,6,28). */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#version 120\n"
        "void main() {\n"
        "  mat3x2 a = mat3x2(1.0, 2.0,  3.0, 4.0,  5.0, 6.0);\n"
        "  mat2x3 b = mat2x3(1.0, 0.0, 2.0,  0.0, 1.0, 3.0);\n"
        "  mat3 q = b * a;\n"
        "  gl_FragColor = vec4(q[0][2] * 0.01, q[2][2] * 0.01, q[1][1] * 0.01, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.08f, 1e-5f);
    ASSERT_NEAR(o[1], 0.28f, 1e-5f);
    ASSERT_NEAR(o[2], 0.04f, 1e-5f);

    /* `outerProduct(vecR, vecC)` is a `matCxR`, so a `vec3` and a `vec2` give a
     * `mat2x3`: col0 = (10,20,30), col1 = (20,40,60). */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#version 120\n"
        "void main() {\n"
        "  mat2x3 o = outerProduct(vec3(1.0, 2.0, 3.0), vec2(10.0, 20.0));\n"
        "  gl_FragColor = vec4(o[1][2] * 0.01, o[0][1] * 0.01, o[1][0] * 0.01, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.6f, 1e-5f);
    ASSERT_NEAR(o[1], 0.2f, 1e-5f);
    ASSERT_NEAR(o[2], 0.2f, 1e-5f);

    /* A scalar fills the diagonal, which runs out at the shorter side: `mat2x4` has
     * only (0,0) and (1,1) on it. A square-case stride of `n + 1` would reach element
     * 10, past the eight this matrix has. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#version 120\n"
        "void main() {\n"
        "  mat2x4 d = mat2x4(0.5);\n"
        "  gl_FragColor = vec4(d[0][0], d[1][1], d[1][0] + d[0][3] + d[1][3], 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-6f);
    ASSERT_NEAR(o[1], 0.5f, 1e-6f);
    ASSERT_NEAR(o[2], 0.0f, 1e-6f);

    /* `matNxN` is a spelling of `matN`, one type, so this assigns without a
     * conversion. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#version 120\n"
        "void main() {\n"
        "  mat3x3 a = mat3x3(2.0);\n"
        "  mat3 b = a;\n"
        "  gl_FragColor = vec4(b[1][1] * 0.25, b[1][0], b[2][2] * 0.25, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-6f);
    ASSERT_NEAR(o[1], 0.0f, 1e-6f);
    ASSERT_NEAR(o[2], 0.5f, 1e-6f);

    glContextDestroy(ctx);
}

/*
 * The non-square shapes through the software reference, `glsl_exec.c`, which is a
 * separate implementation the compiled-path tests cannot see. The values are the ones
 * above scaled into the unit range for an eight-bit channel; a wrong stride moves them
 * by whole components.
 */
static void test_gl2_non_square_matrices_run(void) {
    gl2_target_t t = gl2_target();
    static const char *const VS =
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n";

    /* `mat2x3 * vec2 -> vec3`, the interpreter's own walk. m * (1, 10) = (41, 52, 63)
     * as above, scaled to (0.41, 0.52, 0.63). */
    {
        const GLuint prog =
            linked_program(VS, "#version 120\n"
                               "void main() {\n"
                               "  mat2x3 m = mat2x3(1.0, 2.0, 3.0,  4.0, 5.0, 6.0);\n"
                               "  vec3 p = m * vec2(1.0, 10.0);\n"
                               "  gl_FragColor = vec4(p * 0.01, 1.0);\n"
                               "}\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 100 && px_r(p) < 110); /* 0.41 */
        ASSERT_TRUE(px_g(p) > 128 && px_g(p) < 138); /* 0.52 */
        ASSERT_TRUE(px_b(p) > 156 && px_b(p) < 166); /* 0.63 */
    }

    /* `vec3 * mat2x3 -> vec2`, the other width. (321, 654) scaled to (0.321, 0.654). */
    {
        const GLuint prog =
            linked_program(VS, "#version 120\n"
                               "void main() {\n"
                               "  mat2x3 m = mat2x3(1.0, 2.0, 3.0,  4.0, 5.0, 6.0);\n"
                               "  vec2 q = vec3(1.0, 10.0, 100.0) * m;\n"
                               "  gl_FragColor = vec4(q * 0.001, 0.0, 1.0);\n"
                               "}\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 77 && px_r(p) < 87);   /* 0.321 */
        ASSERT_TRUE(px_g(p) > 162 && px_g(p) < 172); /* 0.654 */
        ASSERT_TRUE(px_b(p) < 8);
    }

    /* `matCxR * matPxC -> matPxR`: `mat2x3 * mat3x2` is a `mat3`, whose (row 2, col 2)
     * is 28, scaled to 0.28. The other two channels take (row 2, col 0) = 8 and
     * (row 1, col 1) = 4. */
    {
        const GLuint prog =
            linked_program(VS, "#version 120\n"
                               "void main() {\n"
                               "  mat3x2 a = mat3x2(1.0, 2.0,  3.0, 4.0,  5.0, 6.0);\n"
                               "  mat2x3 b = mat2x3(1.0, 0.0, 2.0,  0.0, 1.0, 3.0);\n"
                               "  mat3 q = b * a;\n"
                               "  gl_FragColor = vec4(q[0][2] * 0.01, q[2][2] * 0.01, "
                               "q[1][1] * 0.01, 1.0);\n"
                               "}\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 16 && px_r(p) < 25); /* 0.08 */
        ASSERT_TRUE(px_g(p) > 67 && px_g(p) < 76); /* 0.28 */
        ASSERT_TRUE(px_b(p) > 6 && px_b(p) < 15);  /* 0.04 */
    }

    /* `transpose` of a `mat2x3` is a `mat3x2`, so `t[2]` exists. (t[0][1], t[2][0],
     * t[1][1]) is (4, 3, 5), scaled to (0.4, 0.3, 0.5). */
    {
        const GLuint prog =
            linked_program(VS, "#version 120\n"
                               "void main() {\n"
                               "  mat2x3 m = mat2x3(1.0, 2.0, 3.0,  4.0, 5.0, 6.0);\n"
                               "  mat3x2 tr = transpose(m);\n"
                               "  gl_FragColor = vec4(tr[0][1] * 0.1, tr[2][0] * 0.1, "
                               "tr[1][1] * 0.1, 1.0);\n"
                               "}\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 97 && px_r(p) < 107);  /* 0.4 */
        ASSERT_TRUE(px_g(p) > 71 && px_g(p) < 81);   /* 0.3 */
        ASSERT_TRUE(px_b(p) > 123 && px_b(p) < 133); /* 0.5 */
    }

    /* The diagonal constructor, which runs out at the shorter side: `mat2x4(0.5)` has
     * 0.5 at (0,0) and (1,1) and zero elsewhere. */
    {
        const GLuint prog =
            linked_program(VS, "#version 120\n"
                               "void main() {\n"
                               "  mat2x4 d = mat2x4(0.5);\n"
                               "  gl_FragColor = vec4(d[0][0], d[1][1], d[1][0] + "
                               "d[0][3] + d[1][3], 1.0);\n"
                               "}\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 123 && px_r(p) < 133);
        ASSERT_TRUE(px_g(p) > 123 && px_g(p) < 133);
        ASSERT_TRUE(px_b(p) < 8);
    }

    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* The non-square types are GLSL 1.20's and a 1.10 shader using them is refused. Both
 * ways in are checked: a declaration goes through the parser's type rule, and a
 * constructor in an expression does not. */
static void test_gl2_non_square_matrices_are_refused_in_110(void) {
    void *ctx = gl2_context();

    ASSERT_EQ(compiles(GL_FRAGMENT_SHADER, "void main() {\n"
                                           "  mat2x3 m = mat2x3(1.0);\n"
                                           "  gl_FragColor = vec4(m[0], 1.0);\n"
                                           "}\n"),
              GL_FALSE);

    /* No declaration: the type appears only in call position, which the parser reads as
     * an identifier and hands to the semantic stage. */
    ASSERT_EQ(compiles(GL_FRAGMENT_SHADER,
                       "void main() { gl_FragColor = vec4(mat2x3(1.0)[0], 1.0); }\n"),
              GL_FALSE);

    /* The same two compile in 1.20, so the refusal is the version, not the feature. */
    ASSERT_EQ(compiles(GL_FRAGMENT_SHADER, "#version 120\n"
                                           "void main() {\n"
                                           "  mat2x3 m = mat2x3(1.0);\n"
                                           "  gl_FragColor = vec4(m[0], 1.0);\n"
                                           "}\n"),
              GL_TRUE);
    ASSERT_EQ(compiles(GL_FRAGMENT_SHADER,
                       "#version 120\n"
                       "void main() { gl_FragColor = vec4(mat2x3(1.0)[0], 1.0); }\n"),
              GL_TRUE);

    glContextDestroy(ctx);
}

/* A local array is a run of registers indexed at compile time, including by an
 * unrolled loop's counter, which is a constant in each copy of the body. */
static void test_gl2_local_arrays_are_indexed_where_the_shader_is_compiled(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Written and read by literal index, out of order, so overlapping elements show. */
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

    /* A `vec3` array, where the stride is three: a stride of one would read `v[1].x`
     * out of `v[0].y`, 0.2 instead of 0.4. */
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

    /* An unrolled loop's counter as the index. 1+2+3+4 = 10, and 10 * 0.05 is 0.5, a
     * sum no single element produces. */
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

    /* Written through the counter too, then read back the other way round, so the write
     * and the read have to agree about which element is which. */
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

/* A runtime array index is refused with its own message, and whole-array assignment is
 * a front-end error. */
static void test_gl2_arrays_refuse_what_a_register_file_cannot_do(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[512];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};

    static const struct {
        const char *fs;
        const char *wants;
    } cases[] = {
        /* An index known only while the shader runs. A register file cannot be indexed
         * by a computed value; the alternatives are a select chain costing the whole
         * array per access, or memory this back end does not have. */
        {"uniform float k;\n"
         "void main() {\n"
         "  float w[4];\n"
         "  w[0] = 0.1; w[1] = 0.2; w[2] = 0.3; w[3] = 0.4;\n"
         "  gl_FragColor = vec4(w[int(k)], 0.0, 0.0, 1.0);\n"
         "}\n",
         "known when the shader is compiled"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const GLuint prog = linked_program(VS_ONE_VARYING, cases[i].fs);
        log[0] = '\0';
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, prog), words, 512u,
                                              &count, &vgprs, NULL, NULL, log,
                                              sizeof(log)),
                  GL_FALSE);
        if (strstr(log, cases[i].wants) == NULL) {
            printf("\n    case %d: expected a message about '%s', got '%s'\n", (int)i,
                   cases[i].wants, log);
        }
        ASSERT_TRUE(strstr(log, cases[i].wants) != NULL);
    }

    /* A whole array as a destination is a language error, so the front end refuses it
     * for both paths: GLSL 1.10 section 5.8 does not list an array as an l-value.
     * Element assignment through the same name, `v[0] = w[0]`, still compiles. */
    ASSERT_EQ(compiles(GL_FRAGMENT_SHADER,
                       "void main() {\n"
                       "  float w[4];\n"
                       "  float v[4];\n"
                       "  w[0] = 0.1;\n"
                       "  v = w;\n"
                       "  gl_FragColor = vec4(v[0], 0.0, 0.0, 1.0);\n"
                       "}\n"),
              GL_FALSE);
    ASSERT_EQ(compiles(GL_FRAGMENT_SHADER,
                       "void main() {\n"
                       "  float w[4];\n"
                       "  float v[4];\n"
                       "  w[0] = 0.1;\n"
                       "  v[0] = w[0];\n"
                       "  gl_FragColor = vec4(v[0], 0.0, 0.0, 1.0);\n"
                       "}\n"),
              GL_TRUE);
    /* A struct's array member, the other spelling of the same name. */
    ASSERT_EQ(compiles(GL_FRAGMENT_SHADER,
                       "struct S { float w[3]; };\n"
                       "void main() {\n"
                       "  S a; S b;\n"
                       "  a.w[0] = 0.5;\n"
                       "  b.w = a.w;\n"
                       "  gl_FragColor = vec4(b.w[0], 0.0, 0.0, 1.0);\n"
                       "}\n"),
              GL_FALSE);
    /* A whole struct is assignable: 5.8 names entire structures. */
    ASSERT_EQ(compiles(GL_FRAGMENT_SHADER, "struct S { float a; vec2 b; };\n"
                                           "void main() {\n"
                                           "  S p; p.a = 0.5; p.b = vec2(1.0, 2.0);\n"
                                           "  S q; q = p;\n"
                                           "  gl_FragColor = vec4(q.a, q.b, 1.0);\n"
                                           "}\n"),
              GL_TRUE);

    glContextDestroy(ctx);
}

/* An early `return` ends the function and nothing else. Each lane takes the value of
 * the return it reached, leaves every `if` and loop inside the function, and writes
 * nothing further there; the caller's statement and loop still run for it. */
static void test_gl2_an_early_return_ends_the_function_and_nothing_else(void) {
    void *ctx = gl2_context();
    float o[4];
    const float lo[4][4] = {{0.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float hi[4][4] = {{1.0f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* A guard clause, then the real work; one run takes the early return and the other
     * does not. */
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

    /* The statements after the return do not run for the lane that took it; otherwise
     * `t` is written again and the answer is 0.9. */
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

    /* The caller carries on: the green channel, written after the call, is there
     * whichever way the function went. */
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

    /* An `out` parameter is copied back even for a lane that returned early: the
     * copy-back runs after the mask is restored. */
    static const char *const FS_OUT_PARAM = "varying vec4 vin;\n"
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
    ASSERT_NEAR(o[2], 0.5f,
                1e-6f); /* the value it had when it returned, not the later one */
    compile_and_run(ctx, VS_ONE_VARYING, FS_OUT_PARAM, lo, o);
    ASSERT_NEAR(o[0], 0.75f, 1e-6f);
    ASSERT_NEAR(o[2], 1.0f, 1e-6f);

    /* A return out of a loop inside the function. The loop reloads `exec` from its mask
     * every trip, so the returning lane must leave that mask. Returns at 3, so 0.3, not
     * the trailing `return 0.9`. */
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

    /* A bare `return;` from a void function still stops the body; otherwise `mark`
     * comes back 1.0. */
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

    /* Nested calls have distinct masks: the inner returns early and the outer carries
     * on to its own arithmetic, 0.5 rather than 0.2. */
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

    /* A function with no early return emits no mask: two words fewer than the same body
     * with a guard. */
    {
        gl_context_t *c = (gl_context_t *)ctx;
        uint32_t plain[512], guarded[512];
        uint32_t n_plain = 0u, n_guarded = 0u, vgprs = 0u;
        char log[256] = {0};
        const GLuint p1 = linked_program(
            VS_ONE_VARYING,
            "varying vec4 vin;\n"
            "float f(float a) { return a * 0.5; }\n"
            "void main() { gl_FragColor = vec4(f(vin.x), 0.0, 0.0, 1.0); }\n");
        const GLuint p2 = linked_program(VS_ONE_VARYING, FS_GUARD);
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, p1), plain, 512u,
                                              &n_plain, &vgprs, NULL, NULL, log,
                                              sizeof(log)),
                  GL_TRUE);
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, p2), guarded, 512u,
                                              &n_guarded, &vgprs, NULL, NULL, log,
                                              sizeof(log)),
                  GL_TRUE);
        /* `s_and_saveexec_b32` is SOP1 op 60; the guarded one has the `if`'s and the
         * function's, the plain one has neither. */
        int saves_plain = 0, saves_guarded = 0;
        for (uint32_t i = 0; i < n_plain; i++) {
            if ((plain[i] >> 23) == 0x17du && ((plain[i] >> 8) & 0xffu) == 60u)
                saves_plain++;
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

/* The call shapes the back end cannot inline are refused, each with the reason named.
 */
static void test_gl2_the_back_end_refuses_the_calls_it_cannot_inline(void) {
    void *ctx = gl2_context();
    gl_context_t *c = (gl_context_t *)ctx;
    uint32_t words[256];
    uint32_t count = 0u, vgprs = 0u;
    char log[256] = {0};

    static const struct {
        const char *fs;
        const char *wants;
    } cases[] = {
        /* A value-returning function has to end in a return: the trailing one catches
         * the lanes no earlier return took. (A `return` in `main` is generated; see
         * `test_gl2_compiled_early_return`.) */
        {"varying vec4 vin;\n"
         "float f(float a) { if (a > 0.0) { return 1.0; } }\n"
         "void main() { gl_FragColor = vec4(f(vin.x), 0.0, 0.0, 1.0); }\n",
         "has to end in"},
        /* Not here: an `out` argument that is not an l-value. The semantic stage
         * refuses `f(x, q.xx)` and `f(x, 1.0)` before the back end sees them. */
        /* Recursion is invalid GLSL; what matters is that it ends in a message. */
        {"varying vec4 vin;\n"
         "float f(float a) { return f(a); }\n"
         "void main() { gl_FragColor = vec4(f(vin.x), 0.0, 0.0, 1.0); }\n",
         "recursion"},
        /* Not here: a call with the wrong number of arguments, which the front end
         * rejects first ("wrong number of arguments"). */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const GLuint prog = linked_program(VS_ONE_VARYING, cases[i].fs);
        log[0] = '\0';
        ASSERT_EQ(gl_program_compile_fragment(gl_find_program(c, prog), words, 256u,
                                              &count, &vgprs, NULL, NULL, log,
                                              sizeof(log)),
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
 * An early `return` in `main` exports whatever `gl_FragColor` held when it left: the
 * lane leaves every enclosing `if` and loop, and the epilogue restores `exec` from the
 * live mask, which the return leaves alone. A `discard` would export nothing and a
 * fall-through the later value, so the test reads the colour.
 */
static void test_gl2_compiled_early_return(void) {
    void *ctx = gl2_context();
    float o[4];
    /* The varying is 1.0 at this fragment, so the condition below is taken. */
    const float attr[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float tol = 2e-3f;

    /* Returns before the second write: the exported colour is the first one. Falling
     * through would give 0.75 and a discard would give nothing at all. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = vec4(0.25, 0.5, 0.0, 1.0);\n"
                    "  if (vin.x > 0.5) { return; }\n"
                    "  gl_FragColor = vec4(0.75, 0.75, 0.75, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);

    /* The condition not taken runs on, so the return is a branch and not a stop. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = vec4(0.25, 0.5, 0.0, 1.0);\n"
                    "  if (vin.x > 2.0) { return; }\n"
                    "  gl_FragColor = vec4(0.75, 0.75, 0.75, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.75f, tol);

    /* A `return` out of a loop leaves the loop as well as the body; otherwise the
     * loop's top reloads `exec` from a mask the lane is still in. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  float t = 0.0;\n"
                    "  for (int i = 0; i < 8; i++) {\n"
                    "    t += 0.125;\n"
                    "    gl_FragColor = vec4(t, 0.0, 0.0, 1.0);\n"
                    "    if (t > 0.3) { return; }\n"
                    "  }\n"
                    "  gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.375f, tol); /* the third trip, and no further */
    ASSERT_NEAR(o[1], 0.0f, tol);   /* and not the line after the loop */

    glContextDestroy(ctx);
}

/* `noise`, `ivec`/`bvec` constructors and matrix `==` compute in the compiled path. */
static void test_gl2_compiled_glsl110_corners(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float tol = 2e-3f;

    /* `noise` is zero, the answer desktop drivers give and GLSL 4.4 specifies. The
     * argument is still evaluated, so a side effect in it happens. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float n1 = noise1(0.5);\n"
                    "  vec2 n2 = noise2(1.5);\n"
                    "  gl_FragColor = vec4(n1 + 0.25, n2.x + 0.5, n2.y + 0.75, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.75f, tol);

    /* `ivec` and `bvec` constructors. The integer one truncates, so 2.9 arrives as 2.
     */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  ivec2 iv = ivec2(1, 3);\n"
                    "  bvec2 bv = bvec2(true, false);\n"
                    "  gl_FragColor = vec4(float(iv.x) * 0.25, float(iv.y) * 0.25,\n"
                    "                      bv.x ? 0.75 : 0.0, bv.y ? 1.0 : 0.5);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.75f, tol);
    ASSERT_NEAR(o[2], 0.75f, tol); /* true  */
    ASSERT_NEAR(o[3], 0.5f, tol);  /* false */

    /* A matrix compares for equality over every element, so one differing component
     * makes them unequal. Both directions are checked. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  mat2 a = mat2(1.0, 2.0, 3.0, 4.0);\n"
                    "  mat2 b = mat2(1.0, 2.0, 3.0, 4.0);\n"
                    "  mat2 c = mat2(1.0, 2.0, 3.0, 5.0);\n"
                    "  gl_FragColor = vec4((a == b) ? 0.25 : 0.0,\n"
                    "                      (a == c) ? 0.0 : 0.5,\n"
                    "                      (a != c) ? 0.75 : 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol); /* equal matrices are equal */
    ASSERT_NEAR(o[1], 0.5f, tol);  /* and one differing element is enough */
    ASSERT_NEAR(o[2], 0.75f, tol);

    glContextDestroy(ctx);
}

/*
 * A shader's uniform window holds the span it names, not the whole pool of both stages'
 * uniforms. The pool carries `OOPS_GL_GL2_UNIFORM_FLOATS` and the register file holds
 * `GL_PS_UNIFORM_WINDOW_FLOATS`. The value is checked, since wrong offsets would return
 * a different uniform's value.
 */
static void test_gl2_compiled_uniform_window(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float tol = 2e-3f;

    /* `pad` is a vertex-stage `mat4`, sixteen floats the fragment shader never names;
     * `tint` follows it in the pool, 19 floats in all. */
    const GLuint prog =
        linked_program("attribute vec4 pos;\n"
                       "uniform mat4 pad;\n"
                       "void main() { gl_Position = pad * pos; }\n",
                       "uniform vec3 tint;\n"
                       "void main() { gl_FragColor = vec4(tint, 1.0); }\n");
    ASSERT_TRUE(prog != 0);
    glUseProgram(prog);
    glUniform3f(glGetUniformLocation(prog, "tint"), 0.25f, 0.5f, 0.75f);

    compile_and_run_prog(ctx, prog, attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.75f, tol);

    /* A shader naming uniforms on both sides of a window boundary reads both. */
    const GLuint two =
        linked_program("attribute vec4 pos;\n"
                       "uniform mat4 pad;\n"
                       "void main() { gl_Position = pad * pos; }\n",
                       "uniform float a;\nuniform vec2 b;\n"
                       "void main() { gl_FragColor = vec4(a, b, 1.0); }\n");
    ASSERT_TRUE(two != 0);
    glUseProgram(two);
    glUniform1f(glGetUniformLocation(two, "a"), 0.125f);
    glUniform2f(glGetUniformLocation(two, "b"), 0.375f, 0.625f);
    compile_and_run_prog(ctx, two, attr, o);
    ASSERT_NEAR(o[0], 0.125f, tol);
    ASSERT_NEAR(o[1], 0.375f, tol);
    ASSERT_NEAR(o[2], 0.625f, tol);

    glUseProgram(0);
    glContextDestroy(ctx);
}

/*
 * `gl_TexCoord[]` reaches a compiled fragment shader across the parameter interface.
 * The two elements hold different values, so an off-by-one or aliasing shows. With no
 * user varyings, `gl_TexCoord[0]` is parameter 0 and `[1]` parameter 1.
 */
static void test_gl2_compiled_texcoord_builtin(void) {
    void *ctx = gl2_context();
    float o[4];
    const float tol = 2e-3f;
    const float attr[4][4] = {
        {0.25f, 0.5f, 0.0f, 1.0f},   /* gl_TexCoord[0] */
        {0.75f, 0.125f, 0.0f, 1.0f}, /* gl_TexCoord[1] */
        {0, 0, 0, 0},
        {0, 0, 0, 0},
    };

    compile_and_run(ctx, "attribute vec4 pos;\nvoid main() { gl_Position = pos; }\n",
                    "void main() {\n"
                    "  gl_FragColor = vec4(gl_TexCoord[0].x, gl_TexCoord[0].y,\n"
                    "                      gl_TexCoord[1].x, gl_TexCoord[1].y);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.75f, tol); /* the other element, not the first again */
    ASSERT_NEAR(o[3], 0.125f, tol);

    /* `.st` is the same swizzle by its texture-coordinate name. */
    compile_and_run(ctx, "attribute vec4 pos;\nvoid main() { gl_Position = pos; }\n",
                    "void main() {\n"
                    "  vec2 c = gl_TexCoord[0].st;\n"
                    "  gl_FragColor = vec4(c.s, c.t, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.5f, tol);

    glContextDestroy(ctx);
}

/* Prefix `++`/`--` yield the new value and postfix the old; both move the variable. */
static void test_gl2_compiled_increment(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float tol = 2e-3f;

    /* `++i` is the new value, `i++` the old one, and `i` ends at the same place either
     * way. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float a = 1.0;\n"
                    "  float pre = ++a;\n"
                    "  float b = 1.0;\n"
                    "  float post = b++;\n"
                    "  gl_FragColor = vec4(pre, post, a, b);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 2.0f, tol); /* ++a is the new value */
    ASSERT_NEAR(o[1], 1.0f, tol); /* b++ is the old one */
    ASSERT_NEAR(o[2], 2.0f, tol); /* and both variables moved */
    ASSERT_NEAR(o[3], 2.0f, tol);

    /* `--` the same way, so a dropped sign shows. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float a = 1.0;\n"
                    "  float pre = --a;\n"
                    "  float b = 1.0;\n"
                    "  float post = b--;\n"
                    "  gl_FragColor = vec4(pre + 1.0, post, a + 1.0, b + 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, tol); /* --a is 0 */
    ASSERT_NEAR(o[1], 1.0f, tol); /* b-- is the old 1 */
    ASSERT_NEAR(o[2], 1.0f, tol);
    ASSERT_NEAR(o[3], 1.0f, tol);

    glContextDestroy(ctx);
}

/*
 * A `for` the unroller cannot read takes the branched path with a trip guard. Each
 * result counts the trips, and the counter's final value is checked too, since a step
 * applied twice or not at all is invisible in the sum.
 */
static void test_gl2_compiled_unbounded_for_loops(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float tol = 2e-3f;

    /* A bound that is a uniform, so the trip count cannot be known when the shader
     * compiles. */
    const GLuint prog = linked_program(
        VS_ONE_VARYING, "uniform float lim;\n"
                        "varying vec4 vin;\n"
                        "void main() {\n"
                        "  float t = 0.0;\n"
                        "  for (float i = 0.0; i < lim; i += 1.0) { t += 1.0; }\n"
                        "  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);\n"
                        "}\n");
    ASSERT_TRUE(prog != 0);
    glUseProgram(prog);
    glUniform1f(glGetUniformLocation(prog, "lim"), 3.0f);
    compile_and_run_prog(ctx, prog, attr, o);
    ASSERT_NEAR(o[0], 3.0f, tol);

    /* A body that moves its own counter: the condition and the step are evaluated every
     * trip. i goes 0, 3, 6, so the body runs twice and the counter ends at 6. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float t = 0.0;\n"
                    "  float i = 0.0;\n"
                    "  for (; i < 4.0; i += 1.0) { t += 1.0; i += 2.0; }\n"
                    "  gl_FragColor = vec4(t, i, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 2.0f, tol);
    ASSERT_NEAR(o[1], 6.0f, tol);

    /* An initialiser that declares nothing, over a counter from outside the loop. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float t = 0.0;\n"
                    "  float i = 1.0;\n"
                    "  for (i = 0.0; i < 3.0; i += 1.0) { t += 0.25; }\n"
                    "  gl_FragColor = vec4(t, i, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.75f, tol);
    ASSERT_NEAR(o[1], 3.0f, tol);

    /* `for (;;)` with a `break`: GLSL makes an absent condition true, and the guard
     * bounds it. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float t = 0.0;\n"
                    "  for (;;) { t += 0.2; if (t > 0.5) break; }\n"
                    "  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.6f, tol);

    glUseProgram(0);
    glContextDestroy(ctx);
}

/*
 * `while` and `do`-`while` compile as branched loops bounded by `GLSL_GEN_MAX_TRIPS`.
 * Each result depends on how many times the loop ran.
 */
static void test_gl2_compiled_while_loops(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float tol = 2e-3f;

    /* A pre-tested loop: five trips, each adding a tenth. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float acc = 0.0;\n"
                    "  float i = 0.0;\n"
                    "  while (i < 5.0) { acc += 0.1; i += 1.0; }\n"
                    "  gl_FragColor = vec4(acc, i, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, tol);
    ASSERT_NEAR(o[1], 5.0f, tol);

    /* A `do`-`while` whose condition is false at the top still runs once. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float acc = 0.0;\n"
                    "  do { acc += 0.25; } while (acc < 0.0);\n"
                    "  gl_FragColor = vec4(acc, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);

    /* A `do`-`while` that does loop. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float acc = 0.0;\n"
                    "  do { acc += 0.25; } while (acc < 0.7);\n"
                    "  gl_FragColor = vec4(acc, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.75f, tol);

    /* A `while` whose condition is false on entry runs not at all. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float acc = 0.5;\n"
                    "  while (acc > 1.0) { acc += 1.0; }\n"
                    "  gl_FragColor = vec4(acc, 0.0, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, tol);

    /* `break` reaches a `while` the same way it reaches a `for`: the accumulator counts
     * three trips and the counter stops at 4. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  float acc = 0.0;\n"
                    "  float i = 0.0;\n"
                    "  while (i < 10.0) {\n"
                    "    i += 1.0;\n"
                    "    if (i > 3.0) { break; }\n"
                    "    acc += 0.1;\n"
                    "  }\n"
                    "  gl_FragColor = vec4(acc, i, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.3f, tol);
    ASSERT_NEAR(o[1], 4.0f, tol);

    glContextDestroy(ctx);
}

/*
 * A matrix uniform (SuperTux's `mat3 fragcoord2uv`) is laid out column-major in the
 * pool and copied from the right offsets. The matrix is asymmetric, so a row-major copy
 * gives different values.
 */
static void test_gl2_compiled_matrix_uniform(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    const GLuint prog =
        linked_program("attribute vec4 pos;\n"
                       "varying vec4 vin;\n"
                       "void main() { vin = pos; gl_Position = pos; }\n",
                       "uniform mat3 m;\n"
                       "varying vec4 vin;\n"
                       "void main() {\n"
                       "  vec3 r = m * vec3(1.0, 2.0, 3.0);\n"
                       "  gl_FragColor = vec4(r, 1.0);\n"
                       "}\n");
    ASSERT_TRUE(prog != 0);
    glUseProgram(prog);
    /* Column-major: the first three floats are column 0. */
    const GLfloat m[9] = {1.0f,  2.0f,   3.0f,   10.0f, 20.0f,
                          30.0f, 100.0f, 200.0f, 300.0f};
    glUniformMatrix3fv(glGetUniformLocation(prog, "m"), 1, GL_FALSE, m);

    compile_and_run_prog(ctx, prog, attr, o);
    /* row i = m[0][i]*1 + m[1][i]*2 + m[2][i]*3 */
    ASSERT_NEAR(o[0], 1.0f + 20.0f + 300.0f, 1e-3f); /* 321 */
    ASSERT_NEAR(o[1], 2.0f + 40.0f + 600.0f, 1e-3f); /* 642 */
    ASSERT_NEAR(o[2], 3.0f + 60.0f + 900.0f, 1e-3f); /* 963 */

    glUseProgram(0);
    glContextDestroy(ctx);
}

/*
 * A non-square matrix uniform is set through GL 2.1's `glUniformMatrix2x3fv`, and the
 * command must match the declared type, not the float count: `mat2x3` and `mat3x2` are
 * both six floats, and the wrong one would silently transpose.
 */
static void test_gl2_non_square_matrix_uniform(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    const GLuint prog =
        linked_program("attribute vec4 pos;\n"
                       "varying vec4 vin;\n"
                       "void main() { vin = pos; gl_Position = pos; }\n",
                       "#version 120\n"
                       "uniform mat2x3 m;\n"
                       "varying vec4 vin;\n"
                       "void main() {\n"
                       "  vec3 r = m * vec2(1.0, 10.0);\n"
                       "  gl_FragColor = vec4(r * 0.01, 1.0);\n"
                       "}\n");
    ASSERT_TRUE(prog != 0);

    /* The linker reports the type and the size the shader declared. */
    {
        GLint size = 0;
        GLenum type = 0;
        char name[32] = {0};
        glGetActiveUniform(prog, 0, (GLsizei)sizeof(name), NULL, &size, &type, name);
        ASSERT_EQ(type, (GLenum)GL_FLOAT_MAT2x3);
        ASSERT_EQ(size, 1);
    }

    glUseProgram(prog);
    const GLint loc = glGetUniformLocation(prog, "m");
    ASSERT_TRUE(loc >= 0);

    /* Column-major: the first three floats are column 0. m * (1, 10) = (41, 52, 63). */
    const GLfloat m[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    glUniformMatrix2x3fv(loc, 1, GL_FALSE, m);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    compile_and_run_prog(ctx, prog, attr, o);
    ASSERT_NEAR(o[0], 0.41f, 1e-4f);
    ASSERT_NEAR(o[1], 0.52f, 1e-4f);
    ASSERT_NEAR(o[2], 0.63f, 1e-4f);

    /* The other six-float command is refused and does not disturb the value. */
    const GLfloat wrong[6] = {9.0f, 9.0f, 9.0f, 9.0f, 9.0f, 9.0f};
    glUniformMatrix3x2fv(loc, 1, GL_FALSE, wrong);
    ASSERT_EQ(glGetError(), GL_INVALID_OPERATION);
    compile_and_run_prog(ctx, prog, attr, o);
    ASSERT_NEAR(o[0], 0.41f, 1e-4f);

    /* `transpose` reads the source as the other shape, three columns of two, and writes
     * the `mat2x3` this uniform is, so the transposed floats give the same matrix. */
    const GLfloat t[6] = {1.0f, 4.0f, 2.0f, 5.0f, 3.0f, 6.0f};
    glUniformMatrix2x3fv(loc, 1, GL_TRUE, t);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    compile_and_run_prog(ctx, prog, attr, o);
    ASSERT_NEAR(o[0], 0.41f, 1e-4f);
    ASSERT_NEAR(o[1], 0.52f, 1e-4f);
    ASSERT_NEAR(o[2], 0.63f, 1e-4f);

    glUseProgram(0);
    glContextDestroy(ctx);
}

/*
 * Uniforms spanning more than the 32-float scalar window load in several passes, each
 * reloading the same registers from further along; a uniform lives in a VGPR once
 * moved. Three `mat4`s and a `vec4` is 52 floats, two passes, and every value is
 * distinct, so a wrong base returns a different uniform's number.
 */
static void test_gl2_uniforms_wider_than_the_scalar_window(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    const GLuint prog =
        linked_program("attribute vec4 pos;\n"
                       "varying vec4 vin;\n"
                       "void main() { vin = pos; gl_Position = pos; }\n",
                       "uniform mat4 a;\n"
                       "uniform mat4 b;\n"
                       "uniform mat4 c;\n"
                       "uniform vec4 d;\n"
                       "varying vec4 vin;\n"
                       "void main() {\n"
                       "  gl_FragColor = vec4(a[0][0], b[1][1], c[2][2], d.w);\n"
                       "}\n");
    ASSERT_TRUE(prog != 0);
    glUseProgram(prog);

    /* Each matrix is a scale, so `m[i][i]` is its own value and every off-diagonal is
     * zero; a read one column off gives 0.0. */
    GLfloat m[16];
    for (int k = 0; k < 16; k++)
        m[k] = 0.0f;

    m[0] = 0.125f;
    glUniformMatrix4fv(glGetUniformLocation(prog, "a"), 1, GL_FALSE, m);
    m[0] = 0.0f;

    m[5] = 0.375f;
    glUniformMatrix4fv(glGetUniformLocation(prog, "b"), 1, GL_FALSE, m);
    m[5] = 0.0f;

    m[10] = 0.625f;
    glUniformMatrix4fv(glGetUniformLocation(prog, "c"), 1, GL_FALSE, m);

    glUniform4f(glGetUniformLocation(prog, "d"), 0.0f, 0.0f, 0.0f, 0.875f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    compile_and_run_prog(ctx, prog, attr, o);
    ASSERT_NEAR(o[0], 0.125f, 1e-3f);
    ASSERT_NEAR(o[1], 0.375f, 1e-3f);
    ASSERT_NEAR(o[2], 0.625f, 1e-3f);
    ASSERT_NEAR(o[3], 0.875f, 1e-3f);

    /* The same uniforms named in the other order: the passes follow the uniforms'
     * offsets in the pool, not the order the shader mentions them. */
    const GLuint prog2 =
        linked_program("attribute vec4 pos;\n"
                       "varying vec4 vin;\n"
                       "void main() { vin = pos; gl_Position = pos; }\n",
                       "uniform mat4 a;\n"
                       "uniform mat4 b;\n"
                       "uniform mat4 c;\n"
                       "uniform vec4 d;\n"
                       "varying vec4 vin;\n"
                       "void main() {\n"
                       "  gl_FragColor = vec4(d.w, c[2][2], b[1][1], a[0][0]);\n"
                       "}\n");
    ASSERT_TRUE(prog2 != 0);
    glUseProgram(prog2);
    for (int k = 0; k < 16; k++)
        m[k] = 0.0f;
    m[0] = 0.125f;
    glUniformMatrix4fv(glGetUniformLocation(prog2, "a"), 1, GL_FALSE, m);
    m[0] = 0.0f;
    m[5] = 0.375f;
    glUniformMatrix4fv(glGetUniformLocation(prog2, "b"), 1, GL_FALSE, m);
    m[5] = 0.0f;
    m[10] = 0.625f;
    glUniformMatrix4fv(glGetUniformLocation(prog2, "c"), 1, GL_FALSE, m);
    glUniform4f(glGetUniformLocation(prog2, "d"), 0.0f, 0.0f, 0.0f, 0.875f);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    compile_and_run_prog(ctx, prog2, attr, o);
    ASSERT_NEAR(o[0], 0.875f, 1e-3f);
    ASSERT_NEAR(o[1], 0.625f, 1e-3f);
    ASSERT_NEAR(o[2], 0.375f, 1e-3f);
    ASSERT_NEAR(o[3], 0.125f, 1e-3f);

    glUseProgram(0);
    glContextDestroy(ctx);
}

/*
 * mesa-demos' `convolution.frag` shape: a uniform array, indexed by the counter of a
 * `for` whose counter is declared before the loop, bounded by a `const int`. All three
 * are needed for `K[i]` to be a compile-time index; each is also tested on its own.
 */
static void test_gl2_uniform_arrays_indexed_by_an_unrolled_counter(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    const GLuint prog =
        linked_program("attribute vec4 pos;\n"
                       "varying vec4 vin;\n"
                       "void main() { vin = pos; gl_Position = pos; }\n",
                       "const int N = 4;\n"
                       "uniform vec4 K[N];\n"
                       "varying vec4 vin;\n"
                       "void main() {\n"
                       "  int i;\n"
                       "  vec4 sum = vec4(0.0);\n"
                       "  for (i = 0; i < N; ++i) { sum += K[i]; }\n"
                       "  gl_FragColor = sum;\n"
                       "}\n");
    ASSERT_TRUE(prog != 0);
    glUseProgram(prog);

    /* Each channel comes from exactly one element, so an index off by one moves a
     * channel to zero. */
    const GLfloat k[16] = {0.5f, 0.0f, 0.0f,   0.0f, 0.0f, 0.25f, 0.0f, 0.0f,
                           0.0f, 0.0f, 0.125f, 0.0f, 0.0f, 0.0f,  0.0f, 1.0f};
    glUniform4fv(glGetUniformLocation(prog, "K"), 4, k);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    compile_and_run_prog(ctx, prog, attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-3f);
    ASSERT_NEAR(o[1], 0.25f, 1e-3f);
    ASSERT_NEAR(o[2], 0.125f, 1e-3f);
    ASSERT_NEAR(o[3], 1.0f, 1e-3f);

    /* A counter declared outside the loop holds where it stopped afterwards. The
     * unrolled copies write a shadow, so this reads the store the unroller leaves
     * behind: three trips, so `i` is 3. */
    const GLuint prog2 =
        linked_program("attribute vec4 pos;\n"
                       "varying vec4 vin;\n"
                       "void main() { vin = pos; gl_Position = pos; }\n",
                       "varying vec4 vin;\n"
                       "void main() {\n"
                       "  int i;\n"
                       "  float t = 0.0;\n"
                       "  for (i = 0; i < 3; ++i) { t += 0.25; }\n"
                       "  gl_FragColor = vec4(t, float(i) * 0.1, 0.0, 1.0);\n"
                       "}\n");
    ASSERT_TRUE(prog2 != 0);
    glUseProgram(prog2);
    compile_and_run_prog(ctx, prog2, attr, o);
    ASSERT_NEAR(o[0], 0.75f, 1e-3f); /* three trips ran */
    ASSERT_NEAR(o[1], 0.3f, 1e-3f);  /* and `i` says so afterwards */

    /* A `const int` as a local array's length and as a loop's bound: the generator
     * reads the name as a constant. */
    const GLuint prog3 =
        linked_program("attribute vec4 pos;\n"
                       "varying vec4 vin;\n"
                       "void main() { vin = pos; gl_Position = pos; }\n",
                       "const int W = 3;\n"
                       "varying vec4 vin;\n"
                       "void main() {\n"
                       "  float a[W];\n"
                       "  int i;\n"
                       "  for (i = 0; i < W; ++i) { a[i] = float(i) * 0.25; }\n"
                       "  gl_FragColor = vec4(a[2], a[1], a[0], 1.0);\n"
                       "}\n");
    ASSERT_TRUE(prog3 != 0);
    glUseProgram(prog3);
    compile_and_run_prog(ctx, prog3, attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-3f);
    ASSERT_NEAR(o[1], 0.25f, 1e-3f);
    ASSERT_NEAR(o[2], 0.0f, 1e-3f);

    glUseProgram(0);
    glContextDestroy(ctx);
}

/* The uniform array sum through the software reference, which runs the loop rather
 * than unrolling it; it shares the answer with the compiled path. */
static void test_gl2_uniform_arrays_run(void) {
    gl2_target_t t = gl2_target();
    static const char *const VS =
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n";

    const GLuint prog =
        linked_program(VS, "const int N = 4;\n"
                           "uniform vec4 K[N];\n"
                           "void main() {\n"
                           "  int i;\n"
                           "  vec4 sum = vec4(0.0);\n"
                           "  for (i = 0; i < N; ++i) { sum += K[i]; }\n"
                           "  gl_FragColor = vec4(sum.rgb, 1.0);\n"
                           "}\n");
    glUseProgram(prog);
    const GLfloat k[16] = {0.25f, 0.0f, 0.0f,  0.0f, 0.0f, 0.5f, 0.0f, 0.0f,
                           0.0f,  0.0f, 0.75f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    glUniform4fv(glGetUniformLocation(prog, "K"), 4, k);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
    const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);   /* 0.25 */
    ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136); /* 0.50 */
    ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200); /* 0.75 */

    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/*
 * `ivec` and `bvec` uniforms in a compiled shader. The value pool has one
 * representation, so `glUniform4iv` has already turned ints into floats and an `ivec4`
 * is what a `vec4` is. SuperTuxKart's `coloredquad.frag` is the shape.
 */
static void test_gl2_compiled_integer_vector_uniform(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* SuperTuxKart's shader, in 1.10 spelling: `gl_FragColor` for its `out vec4`. */
    const GLuint prog =
        linked_program("attribute vec4 pos;\n"
                       "varying vec4 vin;\n"
                       "void main() { vin = pos; gl_Position = pos; }\n",
                       "uniform ivec4 color;\n"
                       "varying vec4 vin;\n"
                       "void main() { gl_FragColor = vec4(color) / 255.0; }\n");
    ASSERT_TRUE(prog != 0);
    glUseProgram(prog);

    const GLint loc = glGetUniformLocation(prog, "color");
    ASSERT_TRUE(loc >= 0);
    const GLint rgba[4] = {51, 102, 204, 255};
    glUniform4iv(loc, 1, rgba);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    compile_and_run_prog(ctx, prog, attr, o);
    ASSERT_NEAR(o[0], 51.0f / 255.0f, 1e-3f);
    ASSERT_NEAR(o[1], 102.0f / 255.0f, 1e-3f);
    ASSERT_NEAR(o[2], 204.0f / 255.0f, 1e-3f);

    /* A `bvec` reads as a condition: `false` is 0.0 and `true` 1.0 in the pool, so `?:`
     * works on the float unchanged. */
    const GLuint prog2 =
        linked_program("attribute vec4 pos;\n"
                       "varying vec4 vin;\n"
                       "void main() { vin = pos; gl_Position = pos; }\n",
                       "uniform bvec3 flags;\n"
                       "varying vec4 vin;\n"
                       "void main() {\n"
                       "  gl_FragColor = vec4(flags.x ? 0.25 : 0.0,\n"
                       "                      flags.y ? 0.5 : 0.0,\n"
                       "                      flags.z ? 0.75 : 0.0, 1.0);\n"
                       "}\n");
    ASSERT_TRUE(prog2 != 0);
    glUseProgram(prog2);
    const GLint fl = glGetUniformLocation(prog2, "flags");
    ASSERT_TRUE(fl >= 0);
    /* The middle one off, so ignoring the uniform would light three channels. */
    const GLint flags[3] = {1, 0, 1};
    glUniform3iv(fl, 1, flags);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);

    compile_and_run_prog(ctx, prog2, attr, o);
    ASSERT_NEAR(o[0], 0.25f, 1e-3f);
    ASSERT_NEAR(o[1], 0.0f, 1e-3f);
    ASSERT_NEAR(o[2], 0.75f, 1e-3f);

    glUseProgram(0);
    glContextDestroy(ctx);
}

/*
 * An array as a struct member (GLSL 1.10, 4.1.9) and struct `==`/`!=` (5.9) compile.
 * A member that is an array is a run of registers inside the struct's run, and struct
 * equality reduces over the whole run as matrix equality does.
 */
static void test_gl2_compiled_struct_arrays_and_equality(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Read back in reverse order, so an index that runs the wrong way is a rotation. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "struct S { float w[3]; };\n"
                    "void main() {\n"
                    "  S s;\n"
                    "  s.w[0] = 0.25; s.w[1] = 0.5; s.w[2] = 0.75;\n"
                    "  gl_FragColor = vec4(s.w[2], s.w[1], s.w[0], 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.75f, 1e-3f);
    ASSERT_NEAR(o[1], 0.5f, 1e-3f);
    ASSERT_NEAR(o[2], 0.25f, 1e-3f);

    /* A member in front of the array, so the array does not start at the struct's base;
     * ignoring the leading member would shift every channel by one. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "struct S { float a; float w[3]; };\n"
                    "void main() {\n"
                    "  S s;\n"
                    "  s.a = 0.125;\n"
                    "  s.w[0] = 0.25; s.w[1] = 0.5; s.w[2] = 0.75;\n"
                    "  gl_FragColor = vec4(s.a, s.w[0], s.w[2], 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.125f, 1e-3f);
    ASSERT_NEAR(o[1], 0.25f, 1e-3f);
    ASSERT_NEAR(o[2], 0.75f, 1e-3f);

    /* An array of vectors inside a struct, where the element is wider than one register
     * and the stride is what an index multiplies by. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "struct S { vec2 p[3]; };\n"
                    "void main() {\n"
                    "  S s;\n"
                    "  s.p[0] = vec2(0.1, 0.2);\n"
                    "  s.p[1] = vec2(0.3, 0.4);\n"
                    "  s.p[2] = vec2(0.5, 0.6);\n"
                    "  gl_FragColor = vec4(s.p[2].y, s.p[1].x, s.p[0].y, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.6f, 1e-3f);
    ASSERT_NEAR(o[1], 0.3f, 1e-3f);
    ASSERT_NEAR(o[2], 0.2f, 1e-3f);

    /* Equality over the whole run: `r` differs from `p` in the last component of the
     * last member, so a reduction that stopped early would call them equal. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "struct S { float a; vec2 b; };\n"
                    "void main() {\n"
                    "  S p; p.a = 1.0; p.b = vec2(2.0, 3.0);\n"
                    "  S q; q.a = 1.0; q.b = vec2(2.0, 3.0);\n"
                    "  S r; r.a = 1.0; r.b = vec2(2.0, 4.0);\n"
                    "  gl_FragColor = vec4((p == q) ? 0.75 : 0.0,\n"
                    "                      (p == r) ? 1.0 : 0.25,\n"
                    "                      (p != r) ? 0.5 : 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.75f, 1e-3f);
    ASSERT_NEAR(o[1], 0.25f, 1e-3f);
    ASSERT_NEAR(o[2], 0.5f, 1e-3f);

    /* A difference in the first member, so the reduction is not only reading the tail.
     */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "struct S { float a; vec2 b; };\n"
        "void main() {\n"
        "  S p; p.a = 1.0; p.b = vec2(2.0, 3.0);\n"
        "  S r; r.a = 9.0; r.b = vec2(2.0, 3.0);\n"
        "  gl_FragColor = vec4((p == r) ? 1.0 : 0.25, (p != r) ? 0.5 : 0.0,\n"
        "                      0.0, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.25f, 1e-3f);
    ASSERT_NEAR(o[1], 0.5f, 1e-3f);

    /* Ordering is refused: 5.9 gives a struct no `<`. */
    ASSERT_EQ(compiles(GL_FRAGMENT_SHADER,
                       "struct S { float a; };\n"
                       "void main() {\n"
                       "  S p; p.a = 1.0;\n"
                       "  S q; q.a = 2.0;\n"
                       "  gl_FragColor = (p < q) ? vec4(1.0) : vec4(0.0);\n"
                       "}\n"),
              GL_FALSE);

    glContextDestroy(ctx);
}

/* Macro re-expansion, array parameters, and `gl_FragData[0]` (GLSL 1.10's other name
 * for the colour) compile and compute. */
static void test_gl2_compiled_spec_corners(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* A macro parameter used twice with a macro as the argument: both uses of `HALF` in
     * `((HALF) + (HALF))` expand, not only the first. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#define HALF 0.5\n"
        "#define DUP(a) ((a) + (a))\n"
        "void main() { gl_FragColor = vec4(DUP(HALF) * 0.5, 0.25, 0.125, 1.0); }\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-3f);

    /* Nested function-like macros, and one whose body continues after the nested call,
     * so the expansion is spliced where reading had got to, not appended. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#define Q 0.25\n"
        "#define ADD(a, b) ((a) + (b))\n"
        "#define TWICE(x) ADD(x, x) + 0.0\n"
        "void main() {\n"
        "  gl_FragColor = vec4(TWICE(Q), ADD(Q, Q) + Q, ADD(Q, 0.5), 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.5f, 1e-3f);
    ASSERT_NEAR(o[1], 0.75f, 1e-3f);
    ASSERT_NEAR(o[2], 0.75f, 1e-3f);

    /* An array as a function parameter (6.1): the parameter keeps its length. The
     * elements are distinct powers of two, so a copy shifted by one gives a different
     * sum. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "float total(float w[4]) { return w[0] + w[1] + w[2] + w[3]; }\n"
                    "void main() {\n"
                    "  float a[4];\n"
                    "  a[0] = 0.0625; a[1] = 0.125; a[2] = 0.25; a[3] = 0.5;\n"
                    "  gl_FragColor = vec4(total(a), a[0], a[3], 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.9375f, 1e-3f);
    ASSERT_NEAR(o[1], 0.0625f, 1e-3f);
    ASSERT_NEAR(o[2], 0.5f, 1e-3f);

    /* The parameter is a copy: a body that writes it changes nothing the caller sees,
     * for the whole run. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "float wipe(float w[3]) { w[0] = 9.0; w[2] = 9.0; return w[1]; }\n"
                    "void main() {\n"
                    "  float a[3];\n"
                    "  a[0] = 0.25; a[1] = 0.5; a[2] = 0.75;\n"
                    "  float got = wipe(a);\n"
                    "  gl_FragColor = vec4(a[0], got, a[2], 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, 1e-3f);
    ASSERT_NEAR(o[1], 0.5f, 1e-3f);
    ASSERT_NEAR(o[2], 0.75f, 1e-3f);

    /* An `out` array is refused rather than silently dropped: copying back needs an
     * l-value, and 5.8 does not make a whole array one. */
    {
        const GLuint prog = linked_program(
            VS_ONE_VARYING, "void fill(out float w[2]) { w[0] = 1.0; w[1] = 1.0; }\n"
                            "void main() {\n"
                            "  float a[2];\n"
                            "  fill(a);\n"
                            "  gl_FragColor = vec4(a[0], a[1], 0.0, 1.0);\n"
                            "}\n");
        uint32_t words[512];
        uint32_t count = 0u, vgprs = 0u;
        char log[256] = {0};
        ASSERT_EQ(gl_program_compile_fragment(
                      gl_find_program((gl_context_t *)ctx, prog), words, 512u, &count,
                      &vgprs, NULL, NULL, log, sizeof(log)),
                  GL_FALSE);
        ASSERT_TRUE(strstr(log, "array parameter is `in` here") != NULL);
    }

    /* `gl_FragData[0]` is the same buffer as `gl_FragColor` (1.10, 7.2), and the
     * compiled path exports it. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() { gl_FragData[0] = vec4(0.25, 0.5, 0.75, 1.0); }\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, 1e-3f);
    ASSERT_NEAR(o[1], 0.5f, 1e-3f);
    ASSERT_NEAR(o[2], 0.75f, 1e-3f);

    glContextDestroy(ctx);
}

/*
 * The built-in constants (1.10, 7.4) equal what the API reports, since a program sizes
 * from `glGetIntegerv` and a shader from the constant. They fold at compile time, and
 * `const_of` answers for them, so one can be an array's length.
 */
static void test_gl2_builtin_constants(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Read from the API, then from a shader, and compare, rather than against
     * literals. */
    GLint api_units = 0, api_attribs = 0, api_images = 0;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &api_units);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &api_attribs);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &api_images);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    ASSERT_TRUE(api_units > 0 && api_attribs > 0 && api_images > 0);

    compile_and_run(
        ctx, VS_ONE_VARYING,
        "void main() {\n"
        "  gl_FragColor = vec4(float(gl_MaxTextureUnits) * 0.01,\n"
        "                      float(gl_MaxVertexAttribs) * 0.01,\n"
        "                      float(gl_MaxTextureImageUnits) * 0.01, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], (float)api_units * 0.01f, 2e-3f);
    ASSERT_NEAR(o[1], (float)api_attribs * 0.01f, 2e-3f);
    ASSERT_NEAR(o[2], (float)api_images * 0.01f, 2e-3f);

    /* One as an array's length and a loop's bound, an integral constant expression
     * (4.1.9) the generator folds. Read back in reverse. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "void main() {\n"
                    "  vec2 offs[gl_MaxTextureCoords];\n"
                    "  int i;\n"
                    "  for (i = 0; i < gl_MaxTextureCoords; i++) {\n"
                    "    offs[i] = vec2(float(i) * 0.25 + 0.25);\n"
                    "  }\n"
                    "  gl_FragColor = vec4(offs[1].x, offs[0].y, 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.5f, 2e-3f);
    ASSERT_NEAR(o[1], 0.25f, 2e-3f);

    /* A shader may not redeclare one: GLSL 1.10 3.7 reserves every `gl_` name. The back
     * ends look a constant up after the declared names, so this refusal matters. */
    ASSERT_EQ(
        compiles(GL_FRAGMENT_SHADER,
                 "void main() {\n"
                 "  const int gl_MaxTextureUnits = 7;\n"
                 "  gl_FragColor = vec4(float(gl_MaxTextureUnits), 0.0, 0.0, 1.0);\n"
                 "}\n"),
        GL_FALSE);

    /* `gl_MaxDrawBuffers` is one number: the glGet, the constant a shader reads, and
     * the bound of `gl_FragData[gl_MaxDrawBuffers]` as the front end enforces it. */
    GLint api_draw = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &api_draw);
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "void main() { gl_FragColor = vec4(float(gl_MaxDrawBuffers) * 0.25,\n"
        "                                  0.0, 0.0, 1.0); }\n",
        attr, o);
    ASSERT_NEAR(o[0], (float)api_draw * 0.25f, 2e-3f);

    /* The array's bound agrees: element 0 exists and element `api_draw` (1) does not.
     */
    ASSERT_EQ(api_draw, 1);
    ASSERT_EQ(
        compiles(GL_FRAGMENT_SHADER, "void main() { gl_FragData[0] = vec4(1.0); }\n"),
        GL_TRUE);
    ASSERT_EQ(
        compiles(GL_FRAGMENT_SHADER, "void main() { gl_FragData[1] = vec4(1.0); }\n"),
        GL_FALSE);

    glContextDestroy(ctx);
}

/*
 * GLSL 1.20's array constructors, `float[2](a, b)`, parsed as
 * `CALL(INDEX(IDENTIFIER "float", 2), args)`. They work as a declaration's initialiser,
 * the one context that supplies a length (an expression carries a type, only a symbol
 * a length); as an argument or return value they are refused by name.
 */
static void test_gl2_array_constructors(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Read back out of order, so a fill that ran the wrong way is a rotation. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "#version 120\n"
                    "void main() {\n"
                    "  float w[3] = float[3](0.25, 0.5, 0.75);\n"
                    "  gl_FragColor = vec4(w[2], w[0], w[1], 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.75f, 2e-3f);
    ASSERT_NEAR(o[1], 0.25f, 2e-3f);
    ASSERT_NEAR(o[2], 0.5f, 2e-3f);

    /* An element wider than one register, so the stride is what an index multiplies. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#version 120\n"
        "void main() {\n"
        "  vec2 p[3] = vec2[3](vec2(0.1, 0.2), vec2(0.3, 0.4), vec2(0.5, 0.6));\n"
        "  gl_FragColor = vec4(p[2].y, p[1].x, p[0].y, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.6f, 2e-3f);
    ASSERT_NEAR(o[1], 0.3f, 2e-3f);
    ASSERT_NEAR(o[2], 0.2f, 2e-3f);

    /* The arguments are expressions, not literals, so they are generated. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "#version 120\n"
                    "void main() {\n"
                    "  float a = 0.5;\n"
                    "  float w[2] = float[2](a * 0.5, a + 0.25);\n"
                    "  gl_FragColor = vec4(w[0], w[1], 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, 2e-3f);
    ASSERT_NEAR(o[1], 0.75f, 2e-3f);

    /* A `const int` length on both sides, 4.1.9's integral constant expression. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "#version 120\n"
                    "const int N = 3;\n"
                    "void main() {\n"
                    "  float w[N] = float[N](0.125, 0.25, 0.5);\n"
                    "  float t = 0.0;\n"
                    "  int i;\n"
                    "  for (i = 0; i < N; i++) { t += w[i]; }\n"
                    "  gl_FragColor = vec4(t, w[0], w[2], 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.875f, 2e-3f);
    ASSERT_NEAR(o[1], 0.125f, 2e-3f);
    ASSERT_NEAR(o[2], 0.5f, 2e-3f);

    /* The refusals, each naming its reason. */
    struct {
        const char *src;
        const char *wants;
    } bad[] = {
        /* 1.10 does not have them at all. */
        {"void main() {\n"
         "  float w[2] = float[2](0.25, 0.5);\n"
         "  gl_FragColor = vec4(w[0], w[1], 0.0, 1.0);\n"
         "}\n",
         "GLSL 1.20"},
        /* One argument per element, with no filling rule, unlike `vec4(1.0)`. */
        {"#version 120\n"
         "void main() {\n"
         "  float w[3] = float[3](0.25, 0.5);\n"
         "  gl_FragColor = vec4(w[0], w[1], w[2], 1.0);\n"
         "}\n",
         "one argument per element"},
        /* The unsized form, which is a later version's and has no length to take. */
        {"#version 120\n"
         "void main() {\n"
         "  float w[2] = float[](0.25, 0.5);\n"
         "  gl_FragColor = vec4(w[0], w[1], 0.0, 1.0);\n"
         "}\n",
         "constant length"},
        /* As an argument, which 1.20 allows and this does not; the message says so
         * rather than reporting a width mismatch. */
        {"#version 120\n"
         "float total(float w[2]) { return w[0] + w[1]; }\n"
         "void main() { gl_FragColor = vec4(total(float[2](0.25, 0.5)), 0.0, 0.0, "
         "1.0); }\n",
         "not implemented"},
        /* A length that disagrees with the array's. */
        {"#version 120\n"
         "void main() {\n"
         "  float w[2] = float[3](0.25, 0.5, 0.75);\n"
         "  gl_FragColor = vec4(w[0], w[1], 0.0, 1.0);\n"
         "}\n",
         "length is not the array's"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
        source_of(sh, bad[i].src);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        char log[256] = {0};
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        if (ok != GL_FALSE || strstr(log, bad[i].wants) == NULL) {
            printf(
                "\n    case %d: wanted a refusal mentioning '%s', got ok=%d log='%s'\n",
                (int)i, bad[i].wants, (int)ok, log);
        }
        ASSERT_EQ(ok, GL_FALSE);
        ASSERT_TRUE(strstr(log, bad[i].wants) != NULL);
    }

    glContextDestroy(ctx);
}

/*
 * GLSL 1.20's whole-array assignment and comparison, `v = w` and `v == w` (1.10 has
 * neither: 5.8, 5.9). An expression carries a type and only a symbol a length, so the
 * semantic stage checks the arrays' lengths before comparing element types.
 */
static void test_gl2_whole_array_assign_and_compare(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Assignment copies every element, read back out of order so a partial copy shows.
     */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "#version 120\n"
                    "void main() {\n"
                    "  float a[3];\n"
                    "  float b[3];\n"
                    "  a[0] = 0.25; a[1] = 0.5; a[2] = 0.75;\n"
                    "  b[0] = 0.0; b[1] = 0.0; b[2] = 0.0;\n"
                    "  b = a;\n"
                    "  gl_FragColor = vec4(b[2], b[0], b[1], 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.75f, 2e-3f);
    ASSERT_NEAR(o[1], 0.25f, 2e-3f);
    ASSERT_NEAR(o[2], 0.5f, 2e-3f);

    /* The copy is not an alias: writing the destination afterwards leaves the source.
     */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "#version 120\n"
                    "void main() {\n"
                    "  float a[2];\n"
                    "  float b[2];\n"
                    "  a[0] = 0.25; a[1] = 0.5;\n"
                    "  b = a;\n"
                    "  b[0] = 1.0;\n"
                    "  gl_FragColor = vec4(a[0], b[0], a[1], 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, 2e-3f); /* the source is untouched */
    ASSERT_NEAR(o[1], 1.0f, 2e-3f);
    ASSERT_NEAR(o[2], 0.5f, 2e-3f);

    /* An element wider than one register, so the run is elements times their width. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#version 120\n"
        "void main() {\n"
        "  vec2 p[3];\n"
        "  vec2 q[3];\n"
        "  p[0] = vec2(0.1, 0.2); p[1] = vec2(0.3, 0.4); p[2] = vec2(0.5, 0.6);\n"
        "  q = p;\n"
        "  gl_FragColor = vec4(q[2].y, q[1].x, q[0].y, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.6f, 2e-3f);
    ASSERT_NEAR(o[1], 0.3f, 2e-3f);
    ASSERT_NEAR(o[2], 0.2f, 2e-3f);

    /* Comparison reduces over the whole run: the difference is in the last element. The
     * third arm checks `!=` agrees. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "#version 120\n"
                    "void main() {\n"
                    "  float a[3];\n"
                    "  float b[3];\n"
                    "  float c[3];\n"
                    "  a[0] = 0.25; a[1] = 0.5; a[2] = 0.75;\n"
                    "  b[0] = 0.25; b[1] = 0.5; b[2] = 0.75;\n"
                    "  c[0] = 0.25; c[1] = 0.5; c[2] = 9.0;\n"
                    "  gl_FragColor = vec4((a == b) ? 0.75 : 0.0,\n"
                    "                      (a == c) ? 1.0 : 0.25,\n"
                    "                      (a != c) ? 0.5 : 0.0, 1.0);\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.75f, 2e-3f);
    ASSERT_NEAR(o[1], 0.25f, 2e-3f);
    ASSERT_NEAR(o[2], 0.5f, 2e-3f);

    /* A difference in the first element, so the reduction is not only reading the tail.
     */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "#version 120\n"
        "void main() {\n"
        "  float a[3];\n"
        "  float c[3];\n"
        "  a[0] = 0.25; a[1] = 0.5; a[2] = 0.75;\n"
        "  c[0] = 9.0; c[1] = 0.5; c[2] = 0.75;\n"
        "  gl_FragColor = vec4((a == c) ? 1.0 : 0.25, (a != c) ? 0.5 : 0.0,\n"
        "                      0.0, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.25f, 2e-3f);
    ASSERT_NEAR(o[1], 0.5f, 2e-3f);

    /* The refusals, each naming its reason. */
    struct {
        const char *src;
        const char *wants;
    } bad[] = {
        /* 1.10 has neither. */
        {"void main() {\n"
         "  float a[2]; float b[2];\n"
         "  a[0] = 0.25; a[1] = 0.5;\n"
         "  b = a;\n"
         "  gl_FragColor = vec4(b[0], b[1], 0.0, 1.0);\n"
         "}\n",
         "1.10 does not"},
        {"void main() {\n"
         "  float a[2]; float b[2];\n"
         "  a[0] = 0.25; a[1] = 0.5; b[0] = 0.25; b[1] = 0.5;\n"
         "  gl_FragColor = (a == b) ? vec4(1.0) : vec4(0.0);\n"
         "}\n",
         "1.10 does not compare arrays"},
        /* Lengths have to match, which the element types alone cannot see. */
        {"#version 120\n"
         "void main() {\n"
         "  float a[2]; float b[3];\n"
         "  a[0] = 0.25; a[1] = 0.5;\n"
         "  b = a;\n"
         "  gl_FragColor = vec4(b[0], b[1], b[2], 1.0);\n"
         "}\n",
         "same length"},
        {"#version 120\n"
         "void main() {\n"
         "  float a[2]; float b[3];\n"
         "  a[0] = 0.25; a[1] = 0.5;\n"
         "  gl_FragColor = (a == b) ? vec4(1.0) : vec4(0.0);\n"
         "}\n",
         "same length"},
        /* A compound assignment is arithmetic; 1.20 gives arrays only `=`. */
        {"#version 120\n"
         "void main() {\n"
         "  float a[2]; float b[2];\n"
         "  a[0] = 0.25; a[1] = 0.5; b[0] = 0.1; b[1] = 0.2;\n"
         "  b += a;\n"
         "  gl_FragColor = vec4(b[0], b[1], 0.0, 1.0);\n"
         "}\n",
         "compound assignment"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
        source_of(sh, bad[i].src);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        char log[256] = {0};
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        if (ok != GL_FALSE || strstr(log, bad[i].wants) == NULL) {
            printf(
                "\n    case %d: wanted a refusal mentioning '%s', got ok=%d log='%s'\n",
                (int)i, bad[i].wants, (int)ok, log);
        }
        ASSERT_EQ(ok, GL_FALSE);
        ASSERT_TRUE(strstr(log, bad[i].wants) != NULL);
    }

    glContextDestroy(ctx);
}

/* Whole-array assignment and comparison through the software reference, whose values
 * carry their own width since an array's type is its element's. */
static void test_gl2_whole_array_assign_and_compare_run(void) {
    gl2_target_t t = gl2_target();
    const GLuint prog = linked_program(
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
        "#version 120\n"
        "void main() {\n"
        "  vec2 p[3];\n"
        "  vec2 q[3];\n"
        "  vec2 r[3];\n"
        "  p[0] = vec2(0.25, 0.1); p[1] = vec2(0.5, 0.2); p[2] = vec2(0.75, 0.3);\n"
        "  q = p;\n"
        "  r = p; r[2] = vec2(9.0, 9.0);\n"
        "  gl_FragColor = vec4(q[0].x, (q == p) ? 0.5 : 0.0, (r == p) ? 0.0 : 0.75, "
        "1.0);\n"
        "}\n");
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog);
    draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
    const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);   /* 0.25: the copy landed */
    ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136); /* 0.50: equal to its source */
    ASSERT_TRUE(px_b(p) > 185 &&
                px_b(p) < 200); /* 0.75: and differs in the last element */

    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* Array constructors through the software reference, which fills the run from each
 * argument, since an `exec_val_t` holds only one value's floats. */
static void test_gl2_array_constructors_run(void) {
    gl2_target_t t = gl2_target();
    const GLuint prog = linked_program(
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
        "#version 120\n"
        "const int N = 3;\n"
        "void main() {\n"
        "  vec2 p[N] = vec2[N](vec2(0.25, 0.1), vec2(0.5, 0.2), vec2(0.75, 0.3));\n"
        "  gl_FragColor = vec4(p[0].x, p[1].x, p[2].x, 1.0);\n"
        "}\n");
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog);
    draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
    const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);   /* 0.25 */
    ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136); /* 0.50 */
    ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200); /* 0.75 */

    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/*
 * The vertex stage computes correctly. A wrong position would hide under a quad that
 * covers the sampled pixel, so the vertex shader's working is carried into a varying
 * and read back as colour.
 */
static void test_gl2_vertex_stage_computes(void) {
    gl2_target_t t = gl2_target();

    /* A transform that is not the identity, or every comparison below would compare the
     * vertex with itself. */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.25f, 0.0f, 0.0f);
    glScalef(2.0f, 1.0f, 1.0f);

    /* `ftransform()` is `gl_ModelViewProjectionMatrix * gl_Vertex` (1.10, 8.10), the
     * fixed-function answer. Red is the disagreement; green says the transform was
     * applied at all. */
    {
        const GLuint prog = linked_program(
            "attribute vec3 pos;\n"
            "varying vec4 diff;\n"
            "varying float moved;\n"
            "void main() {\n"
            "  vec4 a = ftransform();\n"
            "  vec4 b = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
            "  diff = abs(a - b);\n"
            "  moved = abs(b.x - gl_Vertex.x);\n"
            "  gl_Position = vec4(pos, 1.0);\n"
            "}\n",
            "varying vec4 diff;\n"
            "varying float moved;\n"
            "void main() {\n"
            "  float m = max(max(diff.x, diff.y), max(diff.z, diff.w));\n"
            "  gl_FragColor = vec4(m > 0.0001 ? 1.0 : 0.0,\n"
            "                      moved > 0.01 ? 1.0 : 0.0, 0.0, 1.0);\n"
            "}\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) < 8);   /* they agree */
        ASSERT_TRUE(px_g(p) > 247); /* and the transform was not the identity */
    }

    /* `gl_NormalMatrix` is the inverse transpose of the modelview's upper 3x3 (1.10,
     * 7.4), the one derived built-in matrix. With scale (2, 1, 1), element (0,0) is
     * 0.5, where the upper 3x3 itself would give 2.0. */
    {
        const GLuint prog = linked_program(
            "attribute vec3 pos;\n"
            "varying float nm;\n"
            "void main() { nm = gl_NormalMatrix[0][0]; gl_Position = vec4(pos, 1.0); "
            "}\n",
            "varying float nm;\n"
            "void main() { gl_FragColor = vec4(nm * 0.5, 0.0, 0.0, 1.0); }\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 &&
                    px_r(p) < 72); /* 0.5 * 0.5 = 0.25, not 2.0 clamped to 1 */
    }

    /* A varying of every width reaches the fragment stage intact through the linker's
     * parameter packing; each carries a value only it has. */
    {
        const GLuint prog = linked_program(
            "attribute vec3 pos;\n"
            "varying float a;\n"
            "varying vec2 b;\n"
            "varying vec3 c;\n"
            "void main() {\n"
            "  a = 0.125; b = vec2(0.25, 0.375); c = vec3(0.5, 0.625, 0.75);\n"
            "  gl_Position = vec4(pos, 1.0);\n"
            "}\n",
            "varying float a;\n"
            "varying vec2 b;\n"
            "varying vec3 c;\n"
            "void main() { gl_FragColor = vec4(a + b.y, c.x, c.z, 1.0); }\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 120 && px_r(p) < 136); /* 0.125 + 0.375 */
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136); /* 0.5 */
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200); /* 0.75 */
    }

    /* A `struct` and a loop in the vertex stage, a different interpreter entry from the
     * fragment one. */
    {
        const GLuint prog = linked_program(
            "struct K { float g; float s; };\n"
            "attribute vec3 pos;\n"
            "varying float lit;\n"
            "void main() {\n"
            "  K k; k.g = 0.1; k.s = 0.05;\n"
            "  float t = k.g;\n"
            "  for (int i = 0; i < 3; i++) { t += k.s; }\n"
            "  lit = t;\n"
            "  gl_Position = vec4(pos, 1.0);\n"
            "}\n",
            "varying float lit;\n"
            "void main() { gl_FragColor = vec4(lit, 0.0, 0.0, 1.0); }\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72); /* 0.1 + 3 * 0.05 = 0.25 */
    }

    glUseProgram(0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* The built-in constants through the software reference's own identifier lookup. */
static void test_gl2_builtin_constants_run(void) {
    gl2_target_t t = gl2_target();
    const GLuint prog = linked_program(
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n",
        "void main() {\n"
        "  vec2 offs[gl_MaxTextureCoords];\n"
        "  int i;\n"
        "  for (i = 0; i < gl_MaxTextureCoords; i++) { offs[i] = vec2(float(i) * 0.5 + "
        "0.25); }\n"
        "  gl_FragColor = vec4(offs[0].x, offs[1].x, float(gl_MaxTextureUnits) * 0.25, "
        "1.0);\n"
        "}\n");
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog);
    draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
    const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
    ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);   /* 0.25 */
    ASSERT_TRUE(px_g(p) > 185 && px_g(p) < 200); /* 0.75 */
    ASSERT_TRUE(px_b(p) > 120 && px_b(p) < 136); /* 2 * 0.25 */

    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* The spec corners through the software reference. The preprocessor is shared, so the
 * macro case checks the whole pipeline agrees. */
static void test_gl2_spec_corners_run(void) {
    gl2_target_t t = gl2_target();
    static const char *const VS =
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n";

    {
        const GLuint prog =
            linked_program(VS, "#define Q 0.25\n"
                               "#define DUP(a) ((a) + (a))\n"
                               "void main() { gl_FragColor = vec4(DUP(Q), DUP(Q) + Q, "
                               "DUP(Q) * 3.0, 1.0); }\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 120 && px_r(p) < 136); /* 0.50 */
        ASSERT_TRUE(px_g(p) > 185 && px_g(p) < 200); /* 0.75 */
        ASSERT_TRUE(px_b(p) > 248);                  /* 1.50, clamped */
    }

    {
        const GLuint prog = linked_program(
            VS, "float total(float w[4]) { return w[0] + w[1] + w[2] + w[3]; }\n"
                "void main() {\n"
                "  float a[4];\n"
                "  a[0] = 0.0625; a[1] = 0.125; a[2] = 0.25; a[3] = 0.5;\n"
                "  gl_FragColor = vec4(total(a), a[0], a[3], 1.0);\n"
                "}\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 231 && px_r(p) < 247); /* 0.9375 */
        ASSERT_TRUE(px_g(p) > 8 && px_g(p) < 24);    /* 0.0625 */
        ASSERT_TRUE(px_b(p) > 120 && px_b(p) < 136); /* 0.50 */
    }

    {
        const GLuint prog = linked_program(
            VS, "void main() { gl_FragData[0] = vec4(0.25, 0.5, 0.75, 1.0); }\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136);
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200);
    }

    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/* Struct arrays and struct equality through the software reference, which lays a struct
 * out as floats rather than registers. */
static void test_gl2_struct_arrays_and_equality_run(void) {
    gl2_target_t t = gl2_target();
    static const char *const VS =
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n";

    {
        const GLuint prog =
            linked_program(VS, "struct S { float a; float w[3]; };\n"
                               "void main() {\n"
                               "  S s;\n"
                               "  s.a = 0.125;\n"
                               "  s.w[0] = 0.25; s.w[1] = 0.5; s.w[2] = 0.75;\n"
                               "  gl_FragColor = vec4(s.w[0], s.w[1], s.w[2], 1.0);\n"
                               "}\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72); /* 0.25, not 0.125 */
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136);
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200);
    }

    {
        const GLuint prog =
            linked_program(VS, "struct S { float a; vec2 b; };\n"
                               "void main() {\n"
                               "  S p; p.a = 1.0; p.b = vec2(2.0, 3.0);\n"
                               "  S q; q.a = 1.0; q.b = vec2(2.0, 3.0);\n"
                               "  S r; r.a = 1.0; r.b = vec2(2.0, 4.0);\n"
                               "  gl_FragColor = vec4((p == q) ? 0.25 : 0.0,\n"
                               "                      (p == r) ? 1.0 : 0.5,\n"
                               "                      (p != r) ? 0.75 : 0.0, 1.0);\n"
                               "}\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);
        draw_quad(glGetAttribLocation(prog, "pos"), 0.0f);
        const uint32_t p = px(&t, GL2_W / 2, GL2_H / 2);
        ASSERT_TRUE(px_r(p) > 55 && px_r(p) < 72);   /* 0.25: equal */
        ASSERT_TRUE(px_g(p) > 120 && px_g(p) < 136); /* 0.50: not equal */
        ASSERT_TRUE(px_b(p) > 185 && px_b(p) < 200); /* 0.75: and != agrees */
    }

    glUseProgram(0);
    glContextDestroy(t.ctx);
    oops_display_close(t.disp);
}

/*
 * Structs compiled to gfx1030 are a run of registers laid out as the semantic pass
 * fixed. Each case puts a different member in a different channel, so a layout off by
 * one comes back rotated.
 */
static void test_gl2_compiled_structs(void) {
    void *ctx = gl2_context();
    float o[4];
    const float attr[4][4] = {
        {0.25f, 0.5f, 0.75f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
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

    /* A vector member, and a swizzle of it: both meanings of `.` in one expression. */
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

    /* A member is writable, and writing one leaves its neighbours alone. */
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

    /* Built from a varying, so the members carry interpolated values rather than
     * constants a folded layout could pass on. */
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

/* Compiled arithmetic and common functions compute what the language defines. */
static void test_gl2_compiled_arithmetic_matches_the_language(void) {
    void *ctx = gl2_context();
    /* The reciprocal is a 1-ULP instruction and the transcendentals are worse, so these
     * compare to a tolerance far tighter than a wrong lowering: `sin` without its scale
     * is out by 0.9 here. */
    const float tol = 2e-5f;
    float o[4];
    const float x = 0.7f, y = 2.5f, z = -1.25f, w = 4.0f;
    const float attr[4][4] = {{x, y, z, w}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* The four lowerings with a constant in them: the hardware's `sin` and `cos` are in
     * revolutions and `exp` and `log` base two. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "varying vec4 vin;\n"
        "void main() {\n"
        "  gl_FragColor = vec4(sin(vin.x), cos(vin.x), exp(vin.x), log(vin.y));\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], sinf(x), tol);
    ASSERT_NEAR(o[1], cosf(x), tol);
    ASSERT_NEAR(o[2], expf(x), 1e-4f);
    ASSERT_NEAR(o[3], logf(y), tol);

    /* Division, which is a reciprocal and a multiply, and subtraction's operand order.
     */
    compile_and_run(
        ctx, VS_ONE_VARYING,
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

    /* The common functions, including the two whose definition has a case in it: `sign`
     * is three-valued, and `mod` is `x - y * floor(x/y)` and so is positive for a
     * negative `x`. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "varying vec4 vin;\n"
        "void main() {\n"
        "  gl_FragColor = vec4(abs(vin.z), sign(vin.z), mod(vin.z, vin.y),\n"
        "                      fract(vin.y));\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 1.25f, tol);
    ASSERT_NEAR(o[1], -1.0f, tol);
    ASSERT_NEAR(o[2], z - y * floorf(z / y), tol); /* 1.25, not -1.25 */
    ASSERT_NEAR(o[3], 0.5f, tol);

    /* `sign(0.0)` is zero, not one. */
    const float zeros[4][4] = {{0, 1, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "varying vec4 vin;\n"
        "void main() { gl_FragColor = vec4(sign(vin.x), sign(vin.y), 0.0, 1.0); }\n",
        zeros, o);
    ASSERT_NEAR(o[0], 0.0f, tol);
    ASSERT_NEAR(o[1], 1.0f, tol);

    /* Interpolating, clamping and stepping. `mix` at t = 0 and t = 1 has to be exactly
     * its endpoints, which `a + (b-a)*t` is and `a*(1-t) + b*t` is not. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "varying vec4 vin;\n"
        "void main() {\n"
        "  gl_FragColor = vec4(mix(vin.z, vin.w, 0.0), mix(vin.z, vin.w, 1.0),\n"
        "                      clamp(vin.w, 0.0, 1.0), step(vin.x, vin.y));\n"
        "}\n",
        attr, o);
    ASSERT_EQ(o[0] == z, GL_TRUE);
    ASSERT_EQ(o[1] == w, GL_TRUE);
    ASSERT_NEAR(o[2], 1.0f, tol);
    ASSERT_NEAR(o[3], 1.0f, tol); /* y >= x, so 1 */

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
        const float t = y / 4.0f; /* clamped, and inside the range here */
        ASSERT_NEAR(o[0], t * t * (3.0f - 2.0f * t), 1e-4f);
        ASSERT_NEAR(o[1], 0.0f, tol); /* clamped below */
        ASSERT_NEAR(o[2], 1.0f, tol); /* clamped above */
    }

    glContextDestroy(ctx);
}

/* Compiled geometric functions compute what the language defines. */
static void test_gl2_compiled_geometry_matches_the_language(void) {
    void *ctx = gl2_context();
    const float tol = 2e-5f;
    float o[4];
    const float attr[4][4] = {
        {3.0f, 4.0f, 12.0f, 2.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    compile_and_run(ctx, VS_ONE_VARYING,
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  vec3 v = vin.xyz;\n"
                    "  gl_FragColor = vec4(length(v), dot(v, v), normalize(v).z,\n"
                    "                      distance(v, vec3(0.0, 0.0, 0.0)));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 13.0f, 1e-3f); /* 3, 4, 12 */
    ASSERT_NEAR(o[1], 169.0f, 1e-3f);
    ASSERT_NEAR(o[2], 12.0f / 13.0f, 1e-4f);
    ASSERT_NEAR(o[3], 13.0f, 1e-3f);

    /* `cross`, where the index pattern is the whole of the correctness: x cross y is z.
     */
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

/* A swizzle write moves into each register the swizzle names, in the named order. */
static void test_gl2_compiled_swizzle_writes_land_where_they_are_named(void) {
    void *ctx = gl2_context();
    const float tol = 1e-6f;
    float o[4];
    const float attr[4][4] = {
        {0.1f, 0.2f, 0.3f, 0.4f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* A swizzle write is a move into each register the swizzle names; `.zyx` crossing
     * over separates that from writing in read order. */
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
    ASSERT_NEAR(o[0], 0.3f, tol); /* c.x took vin.z */
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

/* Compiled uniforms load from the draw's block into SGPRs, and follow later glUniform
 * calls without a recompile. */
static void test_gl2_compiled_uniforms_come_from_the_scalar_file(void) {
    void *ctx = gl2_context();
    const float tol = 1e-6f;
    float o[4];
    const float attr[4][4] = {
        {0.25f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* A uniform is the same in every lane, so it lives in an SGPR loaded from a block
     * the draw puts in the payload. Set through the API and read back, this covers the
     * pool layout, the load offsets, and the wait before the first read. */
    const GLuint prog =
        linked_program("attribute vec4 pos;\n"
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
    ASSERT_NEAR(o[3], 0.25f,
                tol); /* still the varying: the two sources do not collide */

    /* A changed uniform changes the picture with no recompile: the words are the same
     * and the block they load from is not. */
    glUniform1f(glGetUniformLocation(prog, "amount"), 0.5f);
    compile_and_run_prog(ctx, prog, attr, o);
    ASSERT_NEAR(o[0], 0.05f, 1e-5f);
    ASSERT_NEAR(o[2], 0.15f, 1e-5f);

    /* A uniform the fragment shader never names costs it no VGPRs: `mvp` is sixteen
     * floats in the shared pool that only the vertex stage reads. */
    const GLuint shared =
        linked_program("uniform mat4 mvp;\n"
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
    ASSERT_EQ(gl_program_compile_fragment(sp, words, 512u, &count, &vgprs, NULL, NULL,
                                          log, sizeof(log)),
              GL_TRUE);
    /* v8 and v9 for the varying component and `k`, plus `gl_FragColor`'s four and the
     * eight the hardware owns; a materialised `mvp` would add sixteen. */
    ASSERT_TRUE(vgprs < 24u);

    compile_and_run_prog(ctx, shared, attr, o);
    ASSERT_NEAR(o[0], 0.75f, tol);
    ASSERT_NEAR(o[3], 0.25f, tol);

    glContextDestroy(ctx);
}

/* Compiled `if`/`else`, nesting, logical operators and vector `==` take the right arm.
 */
static void test_gl2_compiled_control_flow_runs_the_right_arm(void) {
    void *ctx = gl2_context();
    const float tol = 1e-6f;
    float o[4];

    /* `vin.x` is the condition's input, so the same shader runs twice and takes
     * different arms. Both arms execute under a mask; the check is which one wrote. */
    const char *const IF_ELSE = "varying vec4 vin;\n"
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

    /* Nested, and an `if` with no `else`. The inner one saves into the next scalar
     * register along, and an outer arm no lane is running must not let the inner one
     * write. */
    const char *const NESTED = "varying vec4 vin;\n"
                               "void main() {\n"
                               "  float r = 0.0;\n"
                               "  if (vin.x > 0.5) {\n"
                               "    r = 0.25;\n"
                               "    if (vin.y > 0.5) { r = 0.5; }\n"
                               "  }\n"
                               "  gl_FragColor = vec4(r, 0.0, 0.0, 1.0);\n"
                               "}\n";
    const float both[4][4] = {
        {0.9f, 0.9f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float outer[4][4] = {
        {0.9f, 0.1f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float neither[4][4] = {
        {0.1f, 0.9f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    compile_and_run(ctx, VS_ONE_VARYING, NESTED, both, o);
    ASSERT_NEAR(o[0], 0.5f, tol);
    compile_and_run(ctx, VS_ONE_VARYING, NESTED, outer, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    /* The outer arm is dead, so the inner body writes nothing though its own condition
     * is true. */
    compile_and_run(ctx, VS_ONE_VARYING, NESTED, neither, o);
    ASSERT_NEAR(o[0], 0.0f, tol);

    /* The logical operators, which over values exactly 0.0 or 1.0 are min, max and
     * `1 - x`. */
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
                    outer, o); /* a true, b false */
    ASSERT_NEAR(o[0], 0.0f, tol);
    ASSERT_NEAR(o[1], 1.0f, tol);
    ASSERT_NEAR(o[2], 0.0f, tol);

    /* A vector `==` is true only when every component agrees. */
    const float magenta[4][4] = {
        {1.0f, 0.0f, 1.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float nearly[4][4] = {
        {1.0f, 0.5f, 1.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
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

/* A compiled `discard` removes the lane from every saved mask, so no restore revives
 * it. */
static void test_gl2_compiled_discard_kills_the_lane_for_good(void) {
    void *ctx = gl2_context();
    float o[4];
    const float hi[4][4] = {{0.9f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float lo[4][4] = {{0.1f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* The shape craft's block shader opens with: a conditional discard, then work the
     * surviving lanes still do. */
    const char *const KEY = "varying vec4 vin;\n"
                            "void main() {\n"
                            "  if (vin.x > 0.5) { discard; }\n"
                            "  gl_FragColor = vec4(0.25, 0.5, 0.75, 1.0);\n"
                            "}\n";

    /* The `if` restores the mask it saved on the way in, so the discard takes the lane
     * out of that save as well as `exec`. */
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, KEY, hi, o), GL_FALSE);
    /* A wave that discarded every lane still exports with `done`, so it retires. */
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, KEY, lo, o), GL_TRUE);
    ASSERT_NEAR(o[0], 0.25f, 1e-6f);
    ASSERT_NEAR(o[2], 0.75f, 1e-6f);

    /* The same from two levels in, where the lane comes out of both saved masks. */
    const char *const NESTED_KEY = "varying vec4 vin;\n"
                                   "void main() {\n"
                                   "  if (vin.x > 0.5) {\n"
                                   "    if (vin.y > 0.5) { discard; }\n"
                                   "  }\n"
                                   "  gl_FragColor = vec4(1.0);\n"
                                   "}\n";
    const float both[4][4] = {
        {0.9f, 0.9f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const float one[4][4] = {
        {0.9f, 0.1f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, NESTED_KEY, both, o), GL_FALSE);
    ASSERT_EQ(compile_and_run(ctx, VS_ONE_VARYING, NESTED_KEY, one, o), GL_TRUE);

    /* A discard with an `else` after it. The `else` is `exec = saved & ~exec`, and
     * after a discard `exec` is zero, so it is correct only because the discard took
     * the lane out of `saved` first. Two runs: the discarding lane does not survive,
     * and the other takes the else's value. */
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

/* File-scope `const`s and plain globals are in scope in compiled shaders. */
static void test_gl2_compiled_globals_are_in_scope(void) {
    void *ctx = gl2_context();
    const float tol = 1e-5f;
    float o[4];
    const float attr[4][4] = {
        {0.5f, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* File-scope `const`s are generated in source order, so one may be written in terms
     * of an earlier one (craft's block shader declares three before `main`). */
    compile_and_run(
        ctx, VS_ONE_VARYING,
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
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "float scale = 3.0;\n"
        "varying vec4 vin;\n"
        "void main() { scale = scale + 1.0; gl_FragColor = vec4(scale * 0.1); }\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.4f, tol);

    glContextDestroy(ctx);
}

/*
 * The features compose in one realistic shader: craft's block shader without its
 * texture lookups, with file-scope `const`s, API-set uniforms, varyings, a conditional
 * `discard`, `min`, `clamp` and `mix`. Composition stresses the register allocator.
 */
static void test_gl2_a_realistic_shader_compiles_and_computes(void) {
    void *ctx = gl2_context();
    float o[4];

    const float ao_in = 0.6f, light_in = 0.1f, fog_in = 0.25f, diffuse_in = 0.7f;
    const float attr[4][4] = {{ao_in, light_in, fog_in, diffuse_in},
                              {0, 0, 0, 0},
                              {0, 0, 0, 0},
                              {0, 0, 0, 0}};

    const GLuint prog =
        linked_program("attribute vec4 pos;\n"
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
        if (c < 0.0f)
            c = 0.0f;
        if (c > 1.0f)
            c = 1.0f;
        const float f = fogc[i] * 0.8f;
        ASSERT_NEAR(o[i], c + (f - c) * fog_in, 1e-5f);
    }
    ASSERT_NEAR(o[3], 1.0f, 1e-6f);

    glContextDestroy(ctx);
}

/*
 * Compiled texture lookups pass the right coordinates to the right descriptor set. The
 * simulator's texel is its own coordinate in x and y and the set (or a cube's face, a
 * volume's slice) in z.
 */
static void test_gl2_compiled_texture_lookups_reach_the_right_set(void) {
    void *ctx = gl2_context();
    const float tol = 1e-6f;
    float o[4];
    const float attr[4][4] = {
        {0.25f, 0.75f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2D tex;\n"
                    "varying vec4 vin;\n"
                    "void main() { gl_FragColor = texture2D(tex, vin.xy); }\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol); /* the coordinate arrived */
    ASSERT_NEAR(o[1], 0.75f, tol);
    ASSERT_NEAR(o[2], 0.0f, tol); /* through set 0 */
    ASSERT_NEAR(o[3], 1.0f, tol);

    /* `texture2DProj` divides by the coordinate's last component first; the texel is
     * the coordinate, so the division shows. 0.25/2 and 0.75/2. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2D tex;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = texture2DProj(tex, vec3(vin.x, vin.y, 2.0));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.125f, tol);
    ASSERT_NEAR(o[1], 0.375f, tol);

    /* The vec4 form divides by `w` and ignores `z`, so a 99.0 in `z` changes nothing.
     * 0.25/4. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "uniform sampler2D tex;\n"
        "varying vec4 vin;\n"
        "void main() {\n"
        "  gl_FragColor = texture2DProj(tex, vec4(vin.x, vin.y, 99.0, 4.0));\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.0625f, tol);
    ASSERT_NEAR(o[1], 0.1875f, tol);

    /* A cube lookup hands over three address registers, the third the face. The largest
     * component picks the axis and its sign the face; `+X` is face 0 dead centre and
     * `-Z` is face 5, where a lost sign would give 4. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "uniform samplerCube tex;\n"
        "void main() { gl_FragColor = textureCube(tex, vec3(1.0, 0.0, 0.0)); }\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.5f, tol); /* dead centre of the face, */
    ASSERT_NEAR(o[1], 0.5f, tol);
    ASSERT_NEAR(o[2], 0.0f, tol); /* which is +X, face 0 */

    compile_and_run(
        ctx, VS_ONE_VARYING,
        "uniform samplerCube tex;\n"
        "void main() { gl_FragColor = textureCube(tex, vec3(0.0, 0.0, -1.0)); }\n",
        attr, o);
    ASSERT_NEAR(o[2], 5.0f, tol); /* -Z, not +Z */

    /* Off-centre and not normalised: `(2, 1, 0)` lands exactly where `(1, 0.5, 0)`
     * does. `tc` is `-y`, so a positive y moves v down, 0.375 rather than 0.625. */
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

    /* A volume takes its three coordinates straight through, in order, with no face
     * selection and no divide. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler3D vol;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = texture3D(vol, vec3(vin.x, vin.y, 0.625));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol);
    ASSERT_NEAR(o[1], 0.75f, tol);
    ASSERT_NEAR(o[2], 0.625f, tol); /* the slice, not the descriptor set */

    /* Its projective form divides all three by `w`. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "uniform sampler3D vol;\n"
        "varying vec4 vin;\n"
        "void main() {\n"
        "  gl_FragColor = texture3DProj(vol, vec4(vin.x, vin.y, 1.0, 2.0));\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.125f, tol);
    ASSERT_NEAR(o[1], 0.375f, tol);
    ASSERT_NEAR(o[2], 0.5f, tol); /* 1.0 / 2.0: the slice is divided too */

    /* A 1D lookup is a 2D sample with a zero beside the coordinate: a 1D texture is
     * described to the hardware as one row of a 2D image. The simulator reports both
     * address registers, so the zero is visible. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler1D ramp;\n"
                    "varying vec4 vin;\n"
                    "void main() { gl_FragColor = texture1D(ramp, vin.x); }\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.25f, tol); /* the one coordinate */
    ASSERT_NEAR(o[1], 0.0f, tol);  /* and the zero the descriptor still reads */

    /* Its projective form divides the coordinate by the last component: `vec2` by `t`,
     * `vec4` by `q`. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "uniform sampler1D ramp;\n"
        "varying vec4 vin;\n"
        "void main() { gl_FragColor = texture1DProj(ramp, vec2(vin.x, 2.0)); }\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.125f, tol);
    ASSERT_NEAR(o[1], 0.0f, tol);

    /* The 1D shadow, whose address is the reference, the coordinate and the zero. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler1DShadow depth;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = shadow1D(depth, vec3(vin.x, 0.0, 0.25));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, tol); /* 0.25 <= 0.5 */

    /* A shadow lookup compares instead of returning a texel. The reference, the
     * coordinate's third component, is the sampler's first address register. With
     * stored depth 0.5 and less-or-equal, 0.25 passes and 0.75 fails; `s` is 0.25, so
     * only the failing case shows a reference left in place. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2DShadow depth;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = shadow2D(depth, vec3(vin.x, vin.y, 0.25));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 1.0f, tol); /* 0.25 <= 0.5 */
    ASSERT_NEAR(o[1], 1.0f, tol); /* GL_LUMINANCE spreads it across rgb, */
    ASSERT_NEAR(o[2], 1.0f, tol);
    ASSERT_NEAR(o[3], 1.0f, tol); /* with alpha 1 */

    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2DShadow depth;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = shadow2D(depth, vec3(vin.x, vin.y, 0.75));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, tol); /* 0.75 > 0.5, and `s` is 0.25: the order test */
    ASSERT_NEAR(o[3], 1.0f, tol);

    /* The reference is clamped to [0, 1] before the compare (GL 1.4). Against a stored
     * 1.0, a clamped 2.0 passes and an unclamped one fails. Less-or-equal cannot show
     * the low end: a negative reference passes clamped or not. */
    s_sim_shadow_depth = 1.0f;
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2DShadow depth;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = shadow2D(depth, vec3(vin.x, vin.y, 2.0));\n"
                    "}\n",
                    attr, o);
    s_sim_shadow_depth = 0.5f;
    ASSERT_NEAR(o[0], 1.0f, tol);

    /* The projective form divides the reference by `q` along with s and t: 1/4 passes
     * against 0.5 where an undivided 1.0 fails. */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "uniform sampler2DShadow depth;\n"
        "varying vec4 vin;\n"
        "void main() {\n"
        "  gl_FragColor = shadow2DProj(depth, vec4(vin.x, vin.y, 1.0, 4.0));\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 1.0f, tol);

    /* A zero divisor answers zero, as the reference does, not the `inf` an unguarded
     * reciprocal gives. */
    compile_and_run(ctx, VS_ONE_VARYING,
                    "uniform sampler2D tex;\n"
                    "varying vec4 vin;\n"
                    "void main() {\n"
                    "  gl_FragColor = texture2DProj(tex, vec3(vin.x, vin.y, 0.0));\n"
                    "}\n",
                    attr, o);
    ASSERT_NEAR(o[0], 0.0f, tol);
    ASSERT_NEAR(o[1], 0.0f, tol);

    /* Two samplers take two sets, in declaration order. */
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
    ASSERT_NEAR(o[0], 0.0f, tol); /* `first` is set 0 */
    ASSERT_NEAR(o[1], 1.0f, tol); /* and `second` is set 1 */
    ASSERT_NEAR(o[2], 0.25f, tol);

    /* A sample scaled by a uniform, added to a const, behind a discard. */
    const GLuint prog =
        linked_program("attribute vec4 pos;\n"
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

    /* The discard fires when the sampled value asks for it. */
    const float bright[4][4] = {
        {0.95f, 0.5f, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    ASSERT_EQ(compile_and_run_prog(ctx, prog, bright, o), GL_FALSE);

    glContextDestroy(ctx);
}

/* `asin` as the reference computes it, through `oops_atan2f`, clamped at the domain. */
static float ref_asin(float x) {
    if (x <= -1.0f)
        return -1.57079632679489661923f;
    if (x >= 1.0f)
        return 1.57079632679489661923f;
    return oops_atan2f(x, oops_sqrtf(1.0f - x * x));
}

/*
 * The compiled inverse trig functions use `oops_atan2f`'s polynomial, so they are
 * checked against that function. The 1e-5 tolerance is the reciprocal's: `min/max` is
 * `v_rcp_f32` and a multiply where the reference divides; all else is identical.
 */
static void test_gl2_the_inverse_trig_agrees_with_the_reference(void) {
    void *ctx = gl2_context();
    float o[4];
    const float tol = 1e-5f;

    /* Across the reduction's seam: `|y| > |x|` swaps the numerator, so 1.0 comes out
     * right from either direction, and the signs cover all four quadrant fixups. */
    static const float XS[] = {-8.0f, -1.5f, -1.0f, -0.6f, -0.25f, 0.0f,
                               0.25f, 0.6f,  1.0f,  1.5f,  8.0f};
    for (size_t i = 0; i < sizeof(XS) / sizeof(XS[0]); i++) {
        const float attr[4][4] = {
            {XS[i], 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        compile_and_run(
            ctx, VS_ONE_VARYING,
            "varying vec4 vin;\n"
            "void main() { gl_FragColor = vec4(atan(vin.x), 0.0, 0.0, 1.0); }\n",
            attr, o);
        ASSERT_NEAR(o[0], oops_atan2f(XS[i], 1.0f), tol);
    }

    /* `asin` and `acos`, including outside the domain, where both answer the endpoint
     * and an unclamped `sqrt(1 - x*x)` is not a number. */
    for (size_t i = 0; i < sizeof(XS) / sizeof(XS[0]); i++) {
        const float attr[4][4] = {
            {XS[i], 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        compile_and_run(ctx, VS_ONE_VARYING,
                        "varying vec4 vin;\n"
                        "void main() {\n"
                        "  gl_FragColor = vec4(asin(vin.x), acos(vin.x), 0.0, 1.0);\n"
                        "}\n",
                        attr, o);
        ASSERT_NEAR(o[0], ref_asin(XS[i]), tol);
        ASSERT_NEAR(o[1], 1.57079632679489661923f - ref_asin(XS[i]), tol);
    }

    /* Two-argument `atan` needs the quadrants: `atan(y, x)` and `atan(y/x)` differ
     * wherever `x` is negative. */
    static const float YS2[] = {1.0f, 1.0f, -1.0f, -1.0f, 0.0f, 1.0f, 0.0f};
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

/* `refract` returns the zero vector under total internal reflection, as specified. The
 * lowering computes both arms and selects with `v_cndmask`, so the NaN from the
 * negative square root stays in the arm not taken. */
static void test_gl2_refract_returns_zero_under_total_internal_reflection(void) {
    void *ctx = gl2_context();
    float o[4];
    const float tol = 1e-6f;
    const float attr[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};

    /* Straight down onto a flat surface with eta 0.5: d = -1, k = 1 - 0.25*(1-1) = 1,
     * so the ray bends and the answer is `0.5*I - (0.5*(-1) + 1)*N` = (0, -1, 0) for I
     * = (0,-1,0). */
    compile_and_run(
        ctx, VS_ONE_VARYING,
        "void main() {\n"
        "  vec3 r = refract(vec3(0.0, -1.0, 0.0), vec3(0.0, 1.0, 0.0), 0.5);\n"
        "  gl_FragColor = vec4(r * 0.5 + 0.5, 1.0);\n"
        "}\n",
        attr, o);
    ASSERT_NEAR(o[0], 0.5f, tol);
    ASSERT_NEAR(o[1], 0.0f, tol); /* -1 encoded as 0 */
    ASSERT_NEAR(o[2], 0.5f, tol);

    /* A grazing ray with eta 2.0 is total internal reflection: d is near zero, so
     * `k = 1 - 4*(1 - d*d)` is negative and the vector is zero, encoded as 0.5. */
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
    RUN_TEST(test_gl2_a_void_parameter_is_refused);
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
    RUN_TEST(test_gl2_struct_locals_are_freed_at_scope_end);
    RUN_TEST(test_gl2_function_overloading);
    RUN_TEST(test_gl2_array_length_constant_expressions);
    RUN_TEST(test_gl2_es_100_shaders);
    RUN_TEST(test_gl2_framebuffer_objects);
    RUN_TEST(test_gl2_cube_face_attachment_is_sized_from_its_face);
    RUN_TEST(test_gl2_draw_into_a_framebuffer_object);
    RUN_TEST(test_gl2_blit_framebuffer_reads_the_read_binding);
    RUN_TEST(test_gl2_generate_mipmap);
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
    RUN_TEST(test_gl2_non_square_matrices);
    RUN_TEST(test_gl2_non_square_matrices_run);
    RUN_TEST(test_gl2_non_square_matrices_are_refused_in_110);
    RUN_TEST(test_gl2_local_arrays_are_indexed_where_the_shader_is_compiled);
    RUN_TEST(test_gl2_arrays_refuse_what_a_register_file_cannot_do);
    RUN_TEST(test_gl2_an_early_return_ends_the_function_and_nothing_else);
    RUN_TEST(test_gl2_the_back_end_refuses_the_calls_it_cannot_inline);
    RUN_TEST(test_gl2_compiles_a_whole_pixel_shader);
    RUN_TEST(test_gl2_link_refuses_a_null_context);
    RUN_TEST(test_gl2_the_back_end_refuses_what_it_cannot_encode);
    RUN_TEST(test_gl2_compiled_while_loops);
    RUN_TEST(test_gl2_compiled_unbounded_for_loops);
    RUN_TEST(test_gl2_compiled_increment);
    RUN_TEST(test_gl2_compiled_texcoord_builtin);
    RUN_TEST(test_gl2_compiled_uniform_window);
    RUN_TEST(test_gl2_compiled_glsl110_corners);
    RUN_TEST(test_gl2_compiled_early_return);
    RUN_TEST(test_gl2_compiled_matrix_uniform);
    RUN_TEST(test_gl2_non_square_matrix_uniform);
    RUN_TEST(test_gl2_uniforms_wider_than_the_scalar_window);
    RUN_TEST(test_gl2_uniform_arrays_indexed_by_an_unrolled_counter);
    RUN_TEST(test_gl2_uniform_arrays_run);
    RUN_TEST(test_gl2_compiled_integer_vector_uniform);
    RUN_TEST(test_gl2_compiled_structs);
    RUN_TEST(test_gl2_compiled_struct_arrays_and_equality);
    RUN_TEST(test_gl2_struct_arrays_and_equality_run);
    RUN_TEST(test_gl2_compiled_spec_corners);
    RUN_TEST(test_gl2_spec_corners_run);
    RUN_TEST(test_gl2_builtin_constants);
    RUN_TEST(test_gl2_builtin_constants_run);
    RUN_TEST(test_gl2_array_constructors);
    RUN_TEST(test_gl2_whole_array_assign_and_compare);
    RUN_TEST(test_gl2_whole_array_assign_and_compare_run);
    RUN_TEST(test_gl2_array_constructors_run);
    RUN_TEST(test_gl2_vertex_stage_computes);
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
