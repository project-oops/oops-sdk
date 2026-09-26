/*
 * Compile a corpus of real shaders with oops-gl's own front end and code generator, and
 * histogram what each refuses. It calls glCreateShader/glShaderSource/glCompileShader -
 * the entry points a title calls - so it measures what a port would hit.
 *
 * Built and driven by `tools/shader-survey.sh`, which holds the usage. Takes a list of
 * shader files; `--per-file` prints one status line per file instead.
 */
#include <GL/gl.h>
#include <oops/display.h>
#include <oops/memory.h>

/* The generator is reached through the same internal entry the unit tests use. A shader
 * that compiles can still be refused for the console, and only the generator decides
 * whether a port runs on hardware. */
#include "gl_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host stubs the SDK's display and allocator need, same as gl2-probe's own runner.
 */
static uint32_t *s_fb;
static int s_dummy = 1;
static unsigned int s_w = 256, s_h = 256;

oops_display_t *oops_display_open(oops_display_backend_t backend, unsigned int width,
                                  unsigned int height) {
    (void)backend;
    s_w = width ? width : 256;
    s_h = height ? height : 256;
    if (!s_fb) {
        s_fb = (uint32_t *)calloc((size_t)s_w * (size_t)s_h, sizeof(uint32_t));
        if (!s_fb)
            return (oops_display_t *)0;
    }
    return (oops_display_t *)&s_dummy;
}
void oops_display_close(oops_display_t *d) {
    (void)d;
}
int oops_display_flip(oops_display_t *d) {
    (void)d;
    return 0;
}
uint32_t *oops_display_get_framebuffer(oops_display_t *d) {
    (void)d;
    return s_fb;
}
unsigned int oops_display_get_width(const oops_display_t *d) {
    (void)d;
    return s_w;
}
unsigned int oops_display_get_height(const oops_display_t *d) {
    (void)d;
    return s_h;
}
int oops_display_is_gpu_accelerated(const oops_display_t *d) {
    (void)d;
    return 0;
}
int oops_display_is_ready(const oops_display_t *d) {
    return d != (const oops_display_t *)0;
}

void *oops_mem_alloc(size_t size, size_t alignment, oops_mem_type_t type) {
    (void)alignment;
    (void)type;
    return malloc(size);
}
void oops_mem_free(void *p) {
    free(p);
}

#define MAX_REASONS 256
#define REASON_LEN 220

typedef struct {
    char text[REASON_LEN];
    int count;
    char first_file[256];
    /* The first message in this bucket, untruncated: the key has the shader's own
     * identifiers stripped, and this keeps the name the message was about. */
    char first_full[REASON_LEN];
} reason_t;

static reason_t g_reason[MAX_REASONS];
static int g_reasons;

/* The generator's refusals, histogrammed separately. A shader in this column compiles
 * and runs on the software reference and is refused for the console. */
static reason_t g_genreason[MAX_REASONS];
static int g_genreasons;

/* The reason, with the shader's own identifiers taken out of it, so a diagnostic naming
 * a variable makes one bucket rather than one per variable. Everything from the first
 * ` - ` or `'` onward is dropped, which is where this compiler puts the specifics. */
static void tally_into(const char *log, const char *file, int which);

static void tally(const char *log, const char *file) {
    tally_into(log, file, 0);
}
static void tally_gen(const char *log, const char *file) {
    tally_into(log, file, 1);
}

static void tally_into(const char *log, const char *file, int which) {
    char key[REASON_LEN];
    size_t k = 0;
    for (const char *p = log; *p && k + 1 < sizeof(key); p++) {
        if (*p == '\n')
            break;
        if (*p == '\'')
            break;
        if (p[0] == ' ' && p[1] == '-' && p[2] == ' ')
            break;
        key[k++] = *p;
    }
    while (k > 0 && (key[k - 1] == ' ' || key[k - 1] == ':'))
        k--;
    key[k] = '\0';
    if (k == 0) {
        snprintf(key, sizeof(key), "(refused with an empty log)");
    }

    int *n = which ? &g_genreasons : &g_reasons;
    reason_t *tab = which ? g_genreason : g_reason;
    for (int i = 0; i < *n; i++) {
        if (strcmp(tab[i].text, key) == 0) {
            tab[i].count++;
            return;
        }
    }
    if (*n >= MAX_REASONS)
        return;
    snprintf(tab[*n].text, REASON_LEN, "%s", key);
    snprintf(tab[*n].first_file, sizeof(tab[*n].first_file), "%s", file);
    snprintf(tab[*n].first_full, REASON_LEN, "%s", log);
    tab[*n].count = 1;
    (*n)++;
}

static char *slurp(const char *path, long *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return (char *)0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (8 << 20)) {
        fclose(f);
        return (char *)0;
    }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return (char *)0;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *len_out = (long)got;
    return buf;
}

static int ends_with(const char *s, const char *suf) {
    const size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

/*
 * A vertex shader that links against this fragment shader: a fragment shader's
 * `varying` has to be declared by the vertex stage too, so the declarations are copied
 * out of the fragment source. They are left unwritten - GL leaves an unassigned varying
 * undefined rather than making it a link error - since the question is whether the
 * fragment shader generates.
 */
static void make_matching_vs(const char *fs, char *out, size_t cap) {
    size_t w = 0;
    const char *ver = strstr(fs, "#version");
    if (ver) {
        const char *end = strchr(ver, '\n');
        const size_t n = end ? (size_t)(end - ver + 1) : strlen(ver);
        if (w + n < cap) {
            memcpy(out + w, ver, n);
            w += n;
        }
    }
    /* Each `varying` declaration, restated verbatim - including any precision
     * qualifier, which has to agree between the stages in ES. */
    for (const char *p = fs; *p; p++) {
        if (p != fs && p[-1] != '\n')
            continue;
        const char *q = p;
        while (*q == ' ' || *q == '\t')
            q++;
        if (strncmp(q, "varying", 7) != 0)
            continue;
        const char *end = strchr(q, ';');
        if (!end)
            continue;
        const size_t n = (size_t)(end - q + 1);
        if (w + n + 1 >= cap)
            break;
        memcpy(out + w, q, n);
        w += n;
        out[w++] = '\n';
    }
    const char *tail =
        "attribute vec3 pos;\nvoid main() { gl_Position = vec4(pos, 1.0); }\n";
    const size_t tn = strlen(tail);
    if (w + tn < cap) {
        memcpy(out + w, tail, tn);
        w += tn;
    }
    out[w < cap ? w : cap - 1] = '\0';
}

/*
 * Whether this is a standalone GLSL translation unit. A corpus directory holds things
 * that are not: Prism templates that open `@prism(type='fragment', ...)`, libraries of
 * functions meant to be included, deliberately invalid test shaders. They are skipped
 * and counted separately, so the compile column is about shaders.
 */
static int is_translation_unit(const char *src) {
    const char *p = src;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (*p == '@')
        return 0; /* a template, not a shader */
    if (!strstr(src, "void main"))
        return 0; /* an include, or not a shader */
    return 1;
}

/* The stage, sniffed from the source because ports name vertex shaders `*_vertex.glsl`
 * and the like. Writing `gl_Position` or declaring an `attribute` makes it a vertex
 * shader; writing `gl_FragColor` or `gl_FragData` a fragment one. The extension is the
 * tie-break. */
static GLenum sniff_stage(const char *src, const char *path) {
    if (strstr(src, "gl_Position") || strstr(src, "attribute "))
        return GL_VERTEX_SHADER;
    if (strstr(src, "gl_FragColor") || strstr(src, "gl_FragData"))
        return GL_FRAGMENT_SHADER;
    if (ends_with(path, ".vert") || ends_with(path, ".vs"))
        return GL_VERTEX_SHADER;
    return GL_FRAGMENT_SHADER;
}

int main(int argc, char **argv) {
    oops_display_t *disp = oops_display_open(OOPS_DISPLAY_BACKEND_AUTO, 256, 256);
    void *ctx = glContextCreate(disp);
    if (!ctx) {
        printf("no context\n");
        return 1;
    }
    glContextMakeCurrent(ctx);
    glContextSetVersion(2, 0);

    /* `--per-file` prints one status line per file, so a corpus with an expected
     * outcome per file (tools/shader-conformance) can assert each one. */
    int per_file = 0;
    int first_arg = 1;
    if (argc > 1 && strcmp(argv[1], "--per-file") == 0) {
        per_file = 1;
        first_arg = 2;
    }

    int total = 0, ok = 0, unreadable = 0, not_a_unit = 0;
    int gen_total = 0, gen_ok = 0, gen_link_failed = 0;
    for (int a = first_arg; a < argc; a++) {
        long len = 0;
        char *src = slurp(argv[a], &len);
        if (!src || len == 0) {
            unreadable++;
            if (per_file)
                printf("UNREADABLE %s\n", argv[a]);
            free(src);
            continue;
        }
        if (!is_translation_unit(src)) {
            not_a_unit++;
            if (per_file)
                printf("NOT-A-UNIT %s\n", argv[a]);
            free(src);
            continue;
        }
        const GLenum stage = sniff_stage(src, argv[a]);
        const GLuint sh = glCreateShader(stage);
        const char *srcs[1] = {src};
        glShaderSource(sh, 1, srcs, (const GLint *)0);
        glCompileShader(sh);
        GLint status = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
        total++;
        if (status) {
            ok++;
            /* Then the generator, which decides console support. Only a fragment
             * shader: the vertex stage runs on the CPU, so the generator has nothing
             * to refuse about one. */
            if (stage == GL_FRAGMENT_SHADER) {
                gen_total++;
                const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
                static char vbuf[8192];
                make_matching_vs(src, vbuf, sizeof(vbuf));
                const char *vsrc = vbuf;
                glShaderSource(vs, 1, &vsrc, (const GLint *)0);
                glCompileShader(vs);
                const GLuint prog = glCreateProgram();
                glAttachShader(prog, vs);
                glAttachShader(prog, sh);
                glLinkProgram(prog);
                GLint linked = 0;
                glGetProgramiv(prog, GL_LINK_STATUS, &linked);
                if (!linked) {
                    gen_link_failed++;
                    if (per_file)
                        printf("LINK-FAIL  %s\n", argv[a]);
                } else {
                    const gl_program_object_t *po =
                        gl_find_program((gl_context_t *)ctx, prog);
                    static uint32_t words[4096];
                    uint32_t count = 0u, vgprs = 0u, ena = 0u;
                    char glog[256] = {0};
                    if (po &&
                        gl_program_compile_fragment(po, words, 4096u, &count, &vgprs,
                                                    NULL, &ena, glog, sizeof(glog))) {
                        gen_ok++;
                        if (per_file)
                            printf("GENERATES  %s\n", argv[a]);
                    } else {
                        tally_gen(glog, argv[a]);
                        if (per_file)
                            printf("GEN-FAIL   %s: %s\n", argv[a], glog);
                    }
                }
                glDeleteProgram(prog);
                glDeleteShader(vs);
            } else if (per_file) {
                /* The vertex stage runs on the CPU, so compiling is a vertex shader's
                 * whole answer. */
                printf("COMPILES   %s\n", argv[a]);
            }
        } else {
            char log[1024];
            GLsizei got = 0;
            glGetShaderInfoLog(sh, (GLsizei)sizeof(log), &got, log);
            log[(got > 0 && got < (GLsizei)sizeof(log)) ? got : 0] = '\0';
            tally(log, argv[a]);
            if (per_file)
                printf("COMP-FAIL  %s: %s\n", argv[a], log);
        }
        glDeleteShader(sh);
        free(src);
    }

    if (per_file) {
        /* No histogram: a per-file caller takes the whole of stdout as status lines. */
        glContextDestroy(ctx);
        return 0;
    }

    printf("\n%d of %d shaders compile (%d unreadable, %d not a standalone shader)\n",
           ok, total, unreadable, not_a_unit);
    printf("\n%-6s %s\n", "count",
           "refusal (identifiers stripped), and the first file it hit");
    /* Descending, so the row worth acting on is first. */
    for (int printed = 0; printed < g_reasons; printed++) {
        int best = -1;
        for (int i = 0; i < g_reasons; i++) {
            if (g_reason[i].count < 0)
                continue;
            if (best < 0 || g_reason[i].count > g_reason[best].count)
                best = i;
        }
        if (best < 0)
            break;
        printf("%-6d %s\n           %s\n", g_reason[best].count, g_reason[best].text,
               g_reason[best].first_file);
        g_reason[best].count = -1;
    }

    printf("\n%d of %d fragment shaders generate for the console (%d did not link)\n",
           gen_ok, gen_total, gen_link_failed);
    printf("\n%-6s %s\n", "count",
           "the generator's refusal, and the first file it hit");
    for (int printed = 0; printed < g_genreasons; printed++) {
        int best = -1;
        for (int i = 0; i < g_genreasons; i++) {
            if (g_genreason[i].count < 0)
                continue;
            if (best < 0 || g_genreason[i].count > g_genreason[best].count)
                best = i;
        }
        if (best < 0)
            break;
        printf("%-6d %s\n           %s\n           first: %s\n",
               g_genreason[best].count, g_genreason[best].text,
               g_genreason[best].first_file, g_genreason[best].first_full);
        g_genreason[best].count = -1;
    }
    glContextDestroy(ctx);
    oops_display_close(disp);
    free(s_fb);
    return 0;
}
