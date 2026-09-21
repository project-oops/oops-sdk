/*
 * oops-gl: a GLSL fragment shader compiled into a gfx1030 pixel shader
 *
 * The first half of GL 2.0's console back end, and the half that is actually a compiler.
 *
 * # Why the vertex stage is not here
 *
 * **It does not need to be.** oops-gl's hardware vertex shader is a passthrough: the CPU builds
 * each vertex - transformed into clip space, with its colour and texture coordinates already
 * computed - and the shader loads it from the vertex buffer and exports it. `tools/shader/
 * vs-param3.s` is the whole of it, and `exp pos0 v2, v3, v4, v5` exports the position exactly as
 * it was loaded.
 *
 * So a GL 2.0 vertex shader runs where every other vertex computation in this library runs: on
 * the CPU, in `glsl_exec.c`. It writes `gl_Position` and its varyings into the same vertex the
 * fixed-function path writes, and the same passthrough shader carries them. There is nothing for
 * a vertex-shader compiler to do that the interpreter is not already doing correctly.
 *
 * The fragment stage is different, because **the fragment stage is the hardware**. A pixel
 * shader runs per fragment on the GPU and there is no CPU standing in for it on the console
 * path. That is what this compiles.
 *
 * # What a compiled pixel shader looks like
 *
 * Three parts, and only the middle depends on the GLSL:
 *
 *     v_interp_p1_f32 / v_interp_p2_f32 ...   the varyings, one pair per component
 *     <the body>                              whatever the shader computes
 *     v_mov_b32 v4..v7                        the colour into the export registers
 *     exp mrt0 v4, v5, v6, v7 done vm
 *     s_endpgm
 *
 * Every encoding comes from `tools/shader/gl2-fragment.s`, assembled by clang, and
 * `test_glsl_ps_compiles_a_fragment_shader` asserts the words. **A wrong encoding in a compiler
 * is wrong in every shader it ever emits**, which is why that file exists.
 *
 * # The register convention, which is the hardware's and not ours
 *
 * `v0` and `v1` hold the barycentrics the rasteriser wrote, and every `v_interp` reads them.
 * `v4` through `v7` are what the existing pixel shaders export from, and this keeps that so the
 * export instruction is the same one. The allocator therefore starts at `v8`: a compiler that
 * handed out `v0` would have the shader compute over the coordinates it was given.
 *
 * # The limit that is the hardware's
 *
 * Four parameters, sixteen floats. The pipeline exports two, three or four of them - `hw_params`
 * in the draw path - and a fifth has never run on this part (`REQ-20260921T1210Z-4f16`). A
 * program whose varyings need more than sixteen floats is refused here with that number in the
 * message, rather than compiled into a shader that reads a parameter the vertex stage never
 * exported.
 */

#include "glsl_internal.h"

/* The barycentrics are v0 and v1; the colour is exported from v4..v7. Everything the compiler
 * allocates sits above them. */
#define GL_PS_EXPORT_BASE 4u
#define GL_PS_FIRST_FREE_VGPR 8u

/* Four parameters of four components, which is what the vertex stage can export. */
#define GL_PS_MAX_PARAM_FLOATS 16

/* **What the frame's stage table allocated for the pixel stage.** `SPI_SHADER_PGM_RSRC1_PS` is
 * 0x000c0010 in `gl_hw_begin_frame`'s table, and its VGPRS field - bits 5:0 - is 0x10; wave32
 * allocates `(VGPRS + 1) * 8` registers, so 136. A shader that touched `v136` would be reading
 * and writing outside its own allocation, which on this part is another wave's file: not a
 * wrong pixel but a wrong pixel somewhere else, in a draw that has nothing to do with this one.
 *
 * Raising RSRC1 for a hungry shader is the other way to do this and is a measurement away - the
 * register is written once a frame and the value has never been anything but the one above. */
#define GL_PS_MAX_VGPRS 136u

/* **Where the uniform block lands in the scalar file.** s0 and s1 are the block's own address,
 * handed over as user data, and the system SGPRs follow them - so the first safe destination is
 * well above both. s16 is also **16-aligned**, which `s_load_dwordx16` requires of its
 * destination and which s2 or s4 would not satisfy: the loads march on by sixteen from here, so
 * every one of them is aligned for its width. */
#define GL_PS_UNIFORM_SGPR_BASE 16u

static size_t lit_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static void log_say(char *log, size_t cap, const char *why, int line, int column) {
    if (!log || cap == 0u) return;
    if (!why) { log[0] = '\0'; return; }
    if (line > 0) {
        oops_snprintf(log, cap, "%d:%d: %s", line, column, why);
    } else {
        oops_snprintf(log, cap, "%s", why);
    }
}

/* The front-end type a GL enumerant names, for the varying table - which holds the enumerant
 * because that is what `glGetActiveUniform` reports, and the generator wants the other one. */
static glsl_type_t type_from_gl(GLenum t) {
    switch (t) {
        case GL_FLOAT: return GLSL_TYPE_FLOAT;
        case GL_FLOAT_VEC2: return GLSL_TYPE_VEC2;
        case GL_FLOAT_VEC3: return GLSL_TYPE_VEC3;
        case GL_FLOAT_VEC4: return GLSL_TYPE_VEC4;
        default: return GLSL_TYPE_ERROR;
    }
}

/* Whether this unit declares `name` as a uniform of its own.
 *
 * **The program's uniform pool is both stages' uniforms together**, which is what lets one
 * `mat4 mvp` be one uniform - so the pool holds names this shader never mentions. They are
 * loaded into SGPRs regardless, because the block is copied whole and a scalar load is cheap;
 * what they must not cost is a **VGPR each**, and a vertex-only `mat4` would cost sixteen. */
static GLboolean unit_declares_uniform(const glsl_unit_t *u, const char *name, size_t len) {
    if (!u || u->root == GLSL_NO_NODE) return GL_FALSE;
    for (int32_t d = u->ast.nodes[u->root].a; d != GLSL_NO_NODE; d = u->ast.nodes[d].sibling) {
        const glsl_node_t *n = &u->ast.nodes[d];
        if (n->kind != GLSL_NODE_DECL || n->qualifier != GLSL_TOK_KW_UNIFORM) continue;
        if (n->length != len) continue;
        GLboolean same = GL_TRUE;
        for (size_t i = 0; i < len; i++) {
            if (n->text[i] != name[i]) { same = GL_FALSE; break; }
        }
        if (same) return GL_TRUE;
    }
    return GL_FALSE;
}

/* Finds `main`'s definition in a unit. */
static int32_t find_main(const glsl_unit_t *u) {
    if (!u || u->root == GLSL_NO_NODE) return GLSL_NO_NODE;
    for (int32_t d = u->ast.nodes[u->root].a; d != GLSL_NO_NODE; d = u->ast.nodes[d].sibling) {
        const glsl_node_t *n = &u->ast.nodes[d];
        if (n->kind != GLSL_NODE_FUNCTION || n->c == GLSL_NO_NODE) continue;
        if (n->length == 4u && n->text[0] == 'm' && n->text[1] == 'a' && n->text[2] == 'i' &&
            n->text[3] == 'n') {
            return d;
        }
    }
    return GLSL_NO_NODE;
}

GLboolean gl_program_compile_fragment(const gl_program_object_t *p, uint32_t *words,
                                      uint32_t capacity, uint32_t *out_count,
                                      uint32_t *out_vgprs, char *log, size_t log_size) {
    if (log && log_size) log[0] = '\0';
    if (out_count) *out_count = 0u;
    if (out_vgprs) *out_vgprs = 0u;
    if (!p || !p->linked || !words) {
        log_say(log, log_size, "no linked fragment stage to compile", 0, 0);
        return GL_FALSE;
    }
    const glsl_unit_t *fs = p->fs;
    if (!fs) {
        /* A program with only a vertex shader leaves the fixed-function fragment stage to run,
         * and that stage is already in the payload - so there is nothing to compile and this is
         * not a failure. The caller distinguishes them by `out_count` being zero. */
        return GL_TRUE;
    }
    if (p->varying_floats > GL_PS_MAX_PARAM_FLOATS) {
        oops_snprintf(log, log_size,
                      "this program interpolates %d floats and the pipeline exports %d",
                      p->varying_floats, GL_PS_MAX_PARAM_FLOATS);
        return GL_FALSE;
    }

    /* Both of these are over a hundred kilobytes - the node arena and the symbol table - which
     * is more than a freestanding thread's stack should be asked for. */
    glsl_sema_t *sema = (glsl_sema_t *)gl_heap_alloc(sizeof(glsl_sema_t));
    glsl_gen_t *gen = (glsl_gen_t *)gl_heap_alloc(sizeof(glsl_gen_t));
    if (!sema || !gen) {
        gl_heap_free(sema);
        gl_heap_free(gen);
        log_say(log, log_size, "out of memory", 0, 0);
        return GL_FALSE;
    }

    glsl_code_t code;
    glsl_code_init(&code, words, capacity);

    /* The semantic stage is rebuilt rather than kept from the compile: it is scoped, so what
     * survived `glsl_check_unit` is the globals, and the generator asks it for the type of every
     * operand as it walks `main`. Cheaper to redo than to keep a hundred kilobytes per shader
     * object alive for it. */
    glsl_sema_init(sema, (glsl_ast_t *)&fs->ast);
    sema->stage = GL_FRAGMENT_SHADER;
    sema->version = fs->version ? fs->version : 110;
    GLboolean ok = glsl_declare_builtins(sema, GL_FRAGMENT_SHADER);

    glsl_gen_init(gen, (glsl_ast_t *)&fs->ast, sema, &code);
    glsl_gen_reserve(gen, GL_PS_FIRST_FREE_VGPR);

    /* ---------------------------------------------------------------------
     * The uniforms, before anything else touches a register.
     *
     * The draw puts this program's value pool in the payload and its address in the pixel
     * shader's first user SGPR pair, so the block is `s[0:1] + 0`. Two scalar loads bring up to
     * thirty-two floats into s16..s47, and **one wait** covers both - `lgkmcnt(0)` waits for
     * every outstanding scalar load, not for one.
     *
     * Then each uniform this shader actually names is moved into a VGPR of its own and declared
     * like any other input, so the rest of the generator sees a variable and needs no notion of
     * an SGPR at all. That costs one register and one instruction a float, and the alternative -
     * teaching every operand path that a value might live in an SGPR - would buy those back at
     * the price of a second register class in a back end that has one.
     * --------------------------------------------------------------------- */
    if (ok && p->value_floats > OOPS_GL_GL2_UNIFORM_FLOATS) {
        oops_snprintf(log, log_size,
                      "this program's uniforms are %d floats and a draw carries %d",
                      p->value_floats, OOPS_GL_GL2_UNIFORM_FLOATS);
        ok = GL_FALSE;
    }
    if (ok && p->value_floats > 0) {
        for (int base = 0; base < p->value_floats; base += 16) {
            glsl_emit_s_load(&code, GLSL_SMEM_LOAD_DWORDX16,
                             GL_PS_UNIFORM_SGPR_BASE + (uint32_t)base, 0u,
                             (uint32_t)base * 4u);
        }
        glsl_emit_s_waitcnt_lgkm(&code);

        for (int i = 0; ok && i < p->uniform_count; i++) {
            const gl_uniform_t *u = &p->uniforms[i];
            const size_t ulen = lit_len(u->name);
            if (!unit_declares_uniform(fs, u->name, ulen)) continue;
            const glsl_type_t t = type_from_gl(u->type);
            if (t == GLSL_TYPE_ERROR || u->size != 1) {
                /* A sampler, a matrix or an array. The first is refused where the lookup is;
                 * the other two would need a wider `type_from_gl` and, for the array, an index
                 * this back end cannot generate. Named rather than silently skipped, because a
                 * skipped uniform reads as zero and draws. */
                oops_snprintf(log, log_size,
                              "uniform '%s' is not a float or a float vector, and the compiled "
                              "path carries nothing else yet", u->name);
                ok = GL_FALSE;
                break;
            }
            const glsl_value_t home = glsl_gen_declare_input(gen, u->name, ulen, t);
            if (home.count == 0) {
                log_say(log, log_size, gen->error ? gen->error : "a uniform has no register", 0,
                        0);
                ok = GL_FALSE;
                break;
            }
            for (int c = 0; c < home.count; c++) {
                glsl_emit_vop1(&code, GLSL_VOP1_MOV_B32, home.base + (uint32_t)c,
                               glsl_sgpr(GL_PS_UNIFORM_SGPR_BASE +
                                         (uint32_t)(u->offset + c)));
            }
        }
    }

    /* ---------------------------------------------------------------------
     * The prologue: every varying interpolated into registers of its own.
     *
     * **The allocator's order and the interpolation's order have to agree**, which is why the
     * value comes back from `glsl_gen_declare_input` and the interp pair is emitted into
     * exactly the registers it named. Emitting into a register computed separately would work
     * until the first shader whose varyings were declared in a different order from the
     * linker's table.
     * --------------------------------------------------------------------- */
    for (int i = 0; ok && i < p->varying_count; i++) {
        const gl_varying_t *v = &p->varyings[i];
        const glsl_type_t t = type_from_gl(v->type);
        if (t == GLSL_TYPE_ERROR) {
            oops_snprintf(log, log_size,
                          "varying '%s' is not a float or a float vector, and the parameter "
                          "interpolators carry nothing else", v->name);
            ok = GL_FALSE;
            break;
        }
        const glsl_value_t home = glsl_gen_declare_input(gen, v->name, lit_len(v->name), t);
        if (home.count == 0) {
            log_say(log, log_size, gen->error ? gen->error : "a varying has no register", 0, 0);
            ok = GL_FALSE;
            break;
        }
        for (int c = 0; c < home.count; c++) {
            /* The linker laid the varyings out as a flat block of floats; the hardware carries
             * them as four-component parameters. So float `n` of the block is component `n % 4`
             * of parameter `n / 4`, and the two layouts are the same thing counted differently. */
            const int slot = v->offset + c;
            glsl_emit_interp_pair(&code, home.base + (uint32_t)c, (uint32_t)(slot / 4),
                                  (uint32_t)(slot % 4));
        }
    }

    /* ---------------------------------------------------------------------
     * The shader's own globals - `const float pi = 3.14159;` and the rest.
     *
     * These are ordinary declarations that happen to sit outside `main`, so they are generated
     * by the same arm that generates a local: a home, the initialiser into it, the name
     * declared. **In source order**, which is what lets one `const` be written in terms of an
     * earlier one.
     *
     * A `const` is not folded. It could be - every initialiser here is a constant expression by
     * definition - and folding would save a register and a move each. It is not done because
     * the semantic stage does not evaluate constant expressions today, so folding would mean a
     * second evaluator beside `glsl_exec.c`'s, able to disagree with it. A move is cheaper than
     * that.
     *
     * Uniforms, varyings and attributes are skipped: each has its own arm above, and generating
     * them here would declare the name twice.
     * --------------------------------------------------------------------- */
    if (ok && fs->root != GLSL_NO_NODE) {
        for (int32_t d = fs->ast.nodes[fs->root].a; ok && d != GLSL_NO_NODE;
             d = fs->ast.nodes[d].sibling) {
            const glsl_node_t *gn = &fs->ast.nodes[d];
            if (gn->kind != GLSL_NODE_DECL) continue;
            if (gn->qualifier == GLSL_TOK_KW_UNIFORM || gn->qualifier == GLSL_TOK_KW_VARYING ||
                gn->qualifier == GLSL_TOK_KW_ATTRIBUTE) {
                continue;
            }
            if (!glsl_gen_stmt(gen, d)) {
                log_say(log, log_size,
                        gen->error ? gen->error : "this global has no instruction selection",
                        gen->error_line, gen->error_column);
                ok = GL_FALSE;
            }
        }
    }

    /* `gl_FragColor` is a variable like any other and is moved into the export registers at the
     * end. Pinning it to v4 instead would save four moves and would mean every temporary the
     * body allocated had to dodge it. */
    if (ok) {
        const glsl_value_t colour =
            glsl_gen_declare_input(gen, "gl_FragColor", 12u, GLSL_TYPE_VEC4);
        if (colour.count == 0) {
            log_say(log, log_size, gen->error ? gen->error : "gl_FragColor has no register", 0, 0);
            ok = GL_FALSE;
        }
    }

    /* ---------------------------------------------------------------------
     * The body
     * --------------------------------------------------------------------- */
    if (ok) {
        const int32_t main_fn = find_main(fs);
        if (main_fn == GLSL_NO_NODE) {
            log_say(log, log_size, "the fragment shader has no main", 0, 0);
            ok = GL_FALSE;
        } else {
            const int32_t body = fs->ast.nodes[main_fn].c;
            for (int32_t st = fs->ast.nodes[body].a; st != GLSL_NO_NODE;
                 st = fs->ast.nodes[st].sibling) {
                if (!glsl_gen_stmt(gen, st)) {
                    log_say(log, log_size,
                            gen->error ? gen->error : "this shader has no instruction selection",
                            gen->error_line, gen->error_column);
                    ok = GL_FALSE;
                    break;
                }
            }
        }
    }

    /* ---------------------------------------------------------------------
     * The epilogue
     * --------------------------------------------------------------------- */
    if (ok) {
        glsl_value_t colour;
        if (!glsl_gen_lookup(gen, "gl_FragColor", 12u, &colour) || colour.count != 4) {
            log_say(log, log_size, "gl_FragColor did not survive to the export", 0, 0);
            ok = GL_FALSE;
        } else {
            for (uint32_t i = 0; i < 4u; i++) {
                /* A move onto itself would be a wasted instruction rather than a wrong one, and
                 * the export registers are below everything the allocator hands out - so this
                 * never is one, and the check is left out rather than written and never taken. */
                glsl_emit_mov(&code, GL_PS_EXPORT_BASE + i, colour.base + i);
            }
            glsl_emit_export_mrt0(&code, GL_PS_EXPORT_BASE);
            glsl_emit_endpgm(&code);
        }
    }

    if (ok && code.overflow) {
        oops_snprintf(log, log_size, "this shader needs more than %u instructions", capacity);
        ok = GL_FALSE;
    }

    if (ok && gen->high_water > GL_PS_MAX_VGPRS) {
        oops_snprintf(log, log_size,
                      "this shader needs %u registers and the pixel stage is allocated %u",
                      gen->high_water, GL_PS_MAX_VGPRS);
        ok = GL_FALSE;
    }

    if (ok) {
        if (out_count) *out_count = code.count;
        /* What the shader's resource register has to reserve. The high-water mark is what the
         * allocator ever held live, and the export registers sit below it - so it is the whole
         * of the file this shader touches. */
        if (out_vgprs) *out_vgprs = gen->high_water;
    }
    gl_heap_free(sema);
    gl_heap_free(gen);
    return ok;
}
