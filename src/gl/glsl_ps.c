/*
 * oops-gl: a GLSL fragment shader compiled into a gfx1030 pixel shader
 *
 * The vertex stage is not compiled: the hardware vertex shader is a passthrough
 * (`tools/shader/vs-param3.s`), and a GL 2.0 vertex shader runs on the CPU in
 * `glsl_exec.c`, writing the same vertex the fixed-function path writes.
 *
 * A compiled pixel shader interpolates the varyings (`v_interp` pairs), runs the body,
 * moves the colour into v4..v7 and exports it. Encodings come from
 * `tools/shader/gl2-fragment.s`; `test_glsl_ps_compiles_a_fragment_shader` asserts
 * them. v0 and v1 hold the barycentrics and v4..v7 are the export registers, so
 * allocation starts at v8.
 *
 * Varyings are limited to four parameters, sixteen floats: the pipeline exports two to
 * four (`hw_params` in the draw path), and a program needing more is refused.
 */

#include "glsl_internal.h"

/* The barycentrics are v0 and v1; the colour is exported from v4..v7. Everything the
 * compiler allocates sits above them. */
#define GL_PS_EXPORT_BASE 4u
#define GL_PS_FIRST_FREE_VGPR 8u

/* Four parameters of four components, which is what the vertex stage can export. */
#define GL_PS_MAX_PARAM_FLOATS 16

/* What the frame's stage table allocates for the pixel stage.
 * `SPI_SHADER_PGM_RSRC1_PS` is 0x000c0010 in `gl_hw_begin_frame`'s table, and its VGPRS
 * field (bits 5:0) is 0x10; wave32 allocates `(VGPRS + 1) * 8` registers, so 136. A
 * register past that is another wave's. */
#define GL_PS_MAX_VGPRS 136u

/* Where the uniform window lands in the scalar file. `glsl_internal.h` has the map
 * below it; s72 is the first multiple of four clear of it and of the draw constants,
 * as a scalar load of four dwords or more needs. Thirty-two floats from s72 end at
 * s103, below the s105 this part allows. */
#define GL_PS_UNIFORM_SGPR_BASE 72u

/* How many floats of the block are resident at once: a staging size, not a budget. A
 * uniform is moved into a VGPR at the top of the shader, so the prologue slides this
 * window along the block, one pass per 32 floats (see `ps_load_uniforms`). The block
 * is walked by `s_load_dwordx16`, whose destination must be four-aligned. */
#define GL_PS_UNIFORM_WINDOW_FLOATS 32

/* The draw's own constants, loaded into s68..s71: the last 4-aligned group below the
 * uniforms and above the loop masks. */
#define GL_PS_DRAWCONST_SGPR_BASE 68u

/* The top of the scalar map, continued from `glsl_internal.h`, which asserts the ranges
 * it owns. The draw constants and the uniform window sit above them and meet the
 * 106-register ceiling here. */
typedef char gl_ps_sgpr_map_fits
    [(GLSL_GEN_LOOP_SGPR_BASE +
              (unsigned)GLSL_GEN_MAX_LOOP_DEPTH * GLSL_GEN_LOOP_SGPR_COUNT <=
          GL_PS_DRAWCONST_SGPR_BASE &&
      GL_PS_DRAWCONST_SGPR_BASE % 4u == 0u &&
      GL_PS_DRAWCONST_SGPR_BASE + (unsigned)OOPS_GL_GL2_DRAWCONST_FLOATS <=
          GL_PS_UNIFORM_SGPR_BASE &&
      GL_PS_UNIFORM_SGPR_BASE % 4u == 0u &&
      /* The window, not the block: only this much is ever resident. */
      GL_PS_UNIFORM_WINDOW_FLOATS % 16 == 0 &&
      GL_PS_UNIFORM_SGPR_BASE + (unsigned)GL_PS_UNIFORM_WINDOW_FLOATS <= 106u)
         ? 1
         : -1];

/* Where the hardware puts the fragment's window position when `SPI_PS_INPUT_ENA` asks
 * for it. The VGPRs are packed in the order Mesa enumerates them
 * (`ac_get_fs_input_vgpr_cnt`, `ac_shader_util.c`): the perspective-centre barycentrics
 * take v0 and v1, and x, y, z and w follow. The payload's polygon stipple reads the
 * same pair at v2 and v3 with `ENA` 0x302. The prologue copies them into registers of
 * their own. */
#define GL_PS_FRAGPOS_VGPR 2u

/* `SPI_PS_INPUT_ENA` bits, from `R_0286CC_SPI_PS_INPUT_ENA` for gfx103; `R_02865C` is
 * that register only from gfx12 and is `SPI_PS_INPUT_CNTL_6` here. */
#define GL_PS_INPUT_PERSP_CENTER 0x00000002u
#define GL_PS_INPUT_POS_XYZW 0x00000f00u
#define GL_PS_INPUT_FRONT_FACE 0x00001000u

static size_t lit_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0')
        n++;
    return n;
}

/* The front-end type a GL enumerant names, for the varying table - which holds the
 * enumerant because that is what `glGetActiveUniform` reports, and the generator wants
 * the other one. */
static glsl_type_t type_from_gl(GLenum t) {
    switch (t) {
    case GL_FLOAT:
        return GLSL_TYPE_FLOAT;
    case GL_FLOAT_VEC2:
        return GLSL_TYPE_VEC2;
    case GL_FLOAT_VEC3:
        return GLSL_TYPE_VEC3;
    case GL_FLOAT_VEC4:
        return GLSL_TYPE_VEC4;
    /* An `int` or `bool` uniform is carried as a float: the value pool has one
     * representation for every uniform (`gl_internal.h`), so `glUniform1i(u, 1)` is
     * already 1.0f. The type stays `int`, and the generator refuses integer arithmetic
     * on it. */
    case GL_INT:
        return GLSL_TYPE_INT;
    case GL_BOOL:
        return GLSL_TYPE_BOOL;
    /* And their vectors: an `ivec4` is four floats in the pool and four registers here,
     * as a `vec4` is. */
    case GL_INT_VEC2:
        return GLSL_TYPE_IVEC2;
    case GL_INT_VEC3:
        return GLSL_TYPE_IVEC3;
    case GL_INT_VEC4:
        return GLSL_TYPE_IVEC4;
    case GL_BOOL_VEC2:
        return GLSL_TYPE_BVEC2;
    case GL_BOOL_VEC3:
        return GLSL_TYPE_BVEC3;
    case GL_BOOL_VEC4:
        return GLSL_TYPE_BVEC4;
    /* Matrices. The generator stores one as `cols * rows` consecutive registers,
     * column-major, and the value pool holds it the same way (the order
     * `glUniformMatrix*fv` writes without `transpose`), so the copy into VGPRs needs no
     * reordering. */
    case GL_FLOAT_MAT2:
        return GLSL_TYPE_MAT2;
    case GL_FLOAT_MAT3:
        return GLSL_TYPE_MAT3;
    case GL_FLOAT_MAT4:
        return GLSL_TYPE_MAT4;
    case GL_FLOAT_MAT2x3:
        return GLSL_TYPE_MAT2X3;
    case GL_FLOAT_MAT2x4:
        return GLSL_TYPE_MAT2X4;
    case GL_FLOAT_MAT3x2:
        return GLSL_TYPE_MAT3X2;
    case GL_FLOAT_MAT3x4:
        return GLSL_TYPE_MAT3X4;
    case GL_FLOAT_MAT4x2:
        return GLSL_TYPE_MAT4X2;
    case GL_FLOAT_MAT4x3:
        return GLSL_TYPE_MAT4X3;
    default:
        return GLSL_TYPE_ERROR;
    }
}

/* Whether this unit can throw a fragment away, by node kind: `discard` is a keyword,
 * not an identifier. Every `discard` counts, including ones in functions `main` never
 * calls; over-reporting only gives up early Z. */
GLboolean glsl_unit_discards(const glsl_unit_t *u) {
    if (!u)
        return GL_FALSE;
    for (int32_t i = 0; i < u->ast.count; i++) {
        if (u->ast.nodes[i].kind == GLSL_NODE_DISCARD)
            return GL_TRUE;
    }
    return GL_FALSE;
}

/* Whether this shader names `name` anywhere, for built-ins, which have no declaration
 * to find. Every node is read once, so a mention in a branch never taken still counts:
 * the prologue is emitted before the branches are known. */
GLboolean glsl_unit_mentions(const glsl_unit_t *u, const char *name, size_t len) {
    if (!u)
        return GL_FALSE;
    for (int32_t i = 0; i < u->ast.count; i++) {
        const glsl_node_t *n = &u->ast.nodes[i];
        if (n->kind != GLSL_NODE_IDENTIFIER || (size_t)n->length != len)
            continue;
        GLboolean same = GL_TRUE;
        for (size_t c = 0; c < len; c++) {
            if (n->text[c] != name[c]) {
                same = GL_FALSE;
                break;
            }
        }
        if (same)
            return GL_TRUE;
    }
    return GL_FALSE;
}

/* Whether this unit declares `name` as a uniform of its own. The program's pool holds
 * both stages' uniforms, and one this shader never names must not cost it VGPRs. */
static GLboolean unit_declares_uniform(const glsl_unit_t *u, const char *name,
                                       size_t len) {
    if (!u || u->root == GLSL_NO_NODE)
        return GL_FALSE;
    for (int32_t d = u->ast.nodes[u->root].a; d != GLSL_NO_NODE;
         d = u->ast.nodes[d].sibling) {
        const glsl_node_t *n = &u->ast.nodes[d];
        if (n->kind != GLSL_NODE_DECL || n->qualifier != GLSL_TOK_KW_UNIFORM)
            continue;
        if (n->length != len)
            continue;
        GLboolean same = GL_TRUE;
        for (size_t i = 0; i < len; i++) {
            if (n->text[i] != name[i]) {
                same = GL_FALSE;
                break;
            }
        }
        if (same)
            return GL_TRUE;
    }
    return GL_FALSE;
}

/* Finds `main`'s definition in a unit. */
static int32_t find_main(const glsl_unit_t *u) {
    if (!u || u->root == GLSL_NO_NODE)
        return GLSL_NO_NODE;
    for (int32_t d = u->ast.nodes[u->root].a; d != GLSL_NO_NODE;
         d = u->ast.nodes[d].sibling) {
        const glsl_node_t *n = &u->ast.nodes[d];
        if (n->kind != GLSL_NODE_FUNCTION || n->c == GLSL_NO_NODE)
            continue;
        if (n->length == 4u && n->text[0] == 'm' && n->text[1] == 'a' &&
            n->text[2] == 'i' && n->text[3] == 'n') {
            return d;
        }
    }
    return GLSL_NO_NODE;
}

/* The stages of gl_program_compile_fragment, in the order it runs them. Each returns
 * GL_FALSE with the reason in `log`. */
static GLboolean ps_declare_samplers(const gl_program_object_t *p,
                                     const glsl_unit_t *fs, glsl_gen_t *gen, char *log,
                                     size_t log_size);
static GLboolean ps_emit_prologue(const glsl_unit_t *fs, glsl_gen_t *gen,
                                  glsl_code_t *code, int tex_sets,
                                  GLboolean takes_block, GLboolean wants_fragcoord,
                                  uint32_t user_sgprs);
static GLboolean ps_load_uniforms(const gl_program_object_t *p, const glsl_unit_t *fs,
                                  glsl_gen_t *gen, glsl_code_t *code, char *log,
                                  size_t log_size);
static GLboolean ps_declare_window_inputs(glsl_gen_t *gen, glsl_code_t *code,
                                          GLboolean wants_fragcoord,
                                          GLboolean wants_frontfacing,
                                          uint32_t frontface_vgpr, char *log,
                                          size_t log_size);
static GLboolean ps_interpolate_inputs(const gl_program_object_t *p, glsl_gen_t *gen,
                                       glsl_code_t *code, char *log, size_t log_size);
static GLboolean ps_declare_globals_outputs(const glsl_unit_t *fs, glsl_gen_t *gen,
                                            glsl_code_t *code, GLboolean wants_fragdata,
                                            GLboolean wants_fragdepth, char *log,
                                            size_t log_size);
static GLboolean ps_gen_main(const glsl_unit_t *fs, glsl_gen_t *gen, char *log,
                             size_t log_size);
static GLboolean ps_emit_epilogue(glsl_gen_t *gen, glsl_code_t *code,
                                  GLboolean wants_fragdata, GLboolean wants_fragdepth,
                                  char *log, size_t log_size);

GLboolean gl_program_compile_fragment(const gl_program_object_t *p, uint32_t *words,
                                      uint32_t capacity, uint32_t *out_count,
                                      uint32_t *out_vgprs, uint32_t *out_user_sgprs,
                                      uint32_t *out_input_ena, char *log,
                                      size_t log_size) {
    if (log && log_size)
        log[0] = '\0';
    if (out_count)
        *out_count = 0u;
    if (out_vgprs)
        *out_vgprs = 0u;
    if (out_user_sgprs)
        *out_user_sgprs = 0u;
    /* The barycentrics whatever happens, so a caller that ignores a failure still
     * configures a stage the fixed-function shaders can run in. */
    if (out_input_ena)
        *out_input_ena = GL_PS_INPUT_PERSP_CENTER;
    if (!p || !p->linked || !words) {
        glsl_log_write(log, log_size, "no linked fragment stage to compile", 0, 0);
        return GL_FALSE;
    }
    const glsl_unit_t *fs = p->fs;
    if (!fs) {
        /* A program with only a vertex shader runs the payload's fixed-function
         * fragment stage: not a failure, and `out_count` stays zero. */
        return GL_TRUE;
    }
    if (p->varying_floats > GL_PS_MAX_PARAM_FLOATS) {
        oops_snprintf(log, log_size,
                      "this program interpolates %d floats and the pipeline exports %d",
                      p->varying_floats, GL_PS_MAX_PARAM_FLOATS);
        return GL_FALSE;
    }

    /* On the heap: both are too large for a freestanding thread's stack. */
    glsl_sema_t *sema = (glsl_sema_t *)gl_heap_alloc(sizeof(glsl_sema_t));
    glsl_gen_t *gen = (glsl_gen_t *)gl_heap_alloc(sizeof(glsl_gen_t));
    if (!sema || !gen) {
        gl_heap_free(sema);
        gl_heap_free(gen);
        glsl_log_write(log, log_size, "out of memory", 0, 0);
        return GL_FALSE;
    }

    glsl_code_t code_buf;
    glsl_code_t *const code = &code_buf;
    glsl_code_init(code, words, capacity);

    /* The semantic stage is rebuilt rather than kept from the compile; the generator
     * asks it for the type of every operand as it walks `main`. */
    glsl_sema_init(sema, (glsl_ast_t *)&fs->ast);
    sema->stage = GL_FRAGMENT_SHADER;
    sema->version = fs->version ? fs->version : 110;
    /* The unit's struct table, back into this fresh sema, which never ran
     * `glsl_check_unit`. The generator and the interpreter read the same layout. */
    sema->struct_count = fs->struct_count;
    for (int i = 0; i < fs->struct_count; i++)
        sema->structs[i] = fs->structs[i];
    GLboolean ok = glsl_declare_builtins(sema, GL_FRAGMENT_SHADER);

    /* The unit's own functions, back into the table, so a call to a helper the shader
     * defines has a signature to be typed against. */
    if (ok) {
        for (int32_t d = fs->ast.nodes[fs->root].a; d != GLSL_NO_NODE;
             d = fs->ast.nodes[d].sibling) {
            if (fs->ast.nodes[d].kind != GLSL_NODE_FUNCTION)
                continue;
            if (!glsl_declare_function(sema, d)) {
                glsl_log_write(
                    log, log_size,
                    sema->error ? sema->error : "a function has no signature", 0, 0);
                ok = GL_FALSE;
                break;
            }
        }
    }

    glsl_gen_init(gen, (glsl_ast_t *)&fs->ast, sema, code);
    glsl_gen_reserve(gen, GL_PS_FIRST_FREE_VGPR);

    if (ok)
        ok = ps_declare_samplers(p, fs, gen, log, log_size);

    /* Whether this shader is handed the block decides both the scalar loads and how
     * many user SGPRs the draw configures, which is where the SPI puts the primitive
     * mask. The draw path reads it back off the program (`hw_ps_user_sgprs`).
     * `gl_FragCoord` needs the block for the viewport height that flips its y. */
    const int tex_sets = p->hw_tex_sets;
    const GLboolean wants_fragcoord = glsl_unit_mentions(fs, "gl_FragCoord", 12u);
    /* `gl_FrontFacing` needs no block - the SPI hands it over in a register of its own.
     */
    const GLboolean wants_frontfacing = glsl_unit_mentions(fs, "gl_FrontFacing", 14u);
    /* `gl_FragDepth` needs the window position too, for the interpolated z it starts
     * at. */
    const GLboolean wants_fragdepth = glsl_unit_mentions(fs, "gl_FragDepth", 12u);
    const GLboolean wants_fragdata = glsl_unit_mentions(fs, "gl_FragData", 11u);
    const GLboolean takes_block =
        (GLboolean)(tex_sets > 0 || p->value_floats > 0 || wants_fragcoord);
    const uint32_t user_sgprs = takes_block ? 2u : 0u;
    const uint32_t input_ena =
        GL_PS_INPUT_PERSP_CENTER |
        ((wants_fragcoord || wants_fragdepth) ? GL_PS_INPUT_POS_XYZW : 0u) |
        (wants_frontfacing ? GL_PS_INPUT_FRONT_FACE : 0u);
    /* The SPI packs the enabled inputs in Mesa's order, so the front-face register
     * follows the position when that is enabled and the barycentrics when it is not. */
    const uint32_t frontface_vgpr =
        GL_PS_FRAGPOS_VGPR + ((wants_fragcoord || wants_fragdepth) ? 4u : 0u);

    if (ok)
        ok = ps_emit_prologue(fs, gen, code, tex_sets, takes_block, wants_fragcoord,
                              user_sgprs);
    if (ok)
        ok = ps_load_uniforms(p, fs, gen, code, log, log_size);
    if (ok)
        ok = ps_declare_window_inputs(gen, code, wants_fragcoord, wants_frontfacing,
                                      frontface_vgpr, log, log_size);
    if (ok)
        ok = ps_interpolate_inputs(p, gen, code, log, log_size);
    if (ok)
        ok = ps_declare_globals_outputs(fs, gen, code, wants_fragdata, wants_fragdepth,
                                        log, log_size);
    if (ok)
        ok = ps_gen_main(fs, gen, log, log_size);
    if (ok)
        ok =
            ps_emit_epilogue(gen, code, wants_fragdata, wants_fragdepth, log, log_size);

    if (ok && code->overflow) {
        oops_snprintf(log, log_size, "this shader needs more than %u instructions",
                      capacity);
        ok = GL_FALSE;
    }

    if (ok && gen->high_water > GL_PS_MAX_VGPRS) {
        oops_snprintf(
            log, log_size,
            "this shader needs %u registers and the pixel stage is allocated %u",
            gen->high_water, GL_PS_MAX_VGPRS);
        ok = GL_FALSE;
    }

    if (ok) {
        if (out_count)
            *out_count = code->count;
        /* What the shader's resource register has to reserve: the allocator's
         * high-water mark, which lies above the export registers. */
        if (out_vgprs)
            *out_vgprs = gen->high_water;
        if (out_user_sgprs)
            *out_user_sgprs = user_sgprs;
        if (out_input_ena)
            *out_input_ena = input_ena;
    }
    gl_heap_free(sema);
    gl_heap_free(gen);
    return ok;
}

/* The samplers, each declared against its descriptor set, and the refusals for a
 * sampler or a uniform pool the draw does not carry. */
static GLboolean ps_declare_samplers(const gl_program_object_t *p,
                                     const glsl_unit_t *fs, glsl_gen_t *gen, char *log,
                                     size_t log_size) {
    GLboolean ok = GL_TRUE;
    /* ---------------------------------------------------------------------
     * The block, before anything else touches a register.
     *
     * The draw puts one block in the payload and its address in the pixel shader's
     * first user SGPR pair, so everything below is at an offset from `s[0:1]`: a
     * texture unit's descriptors at 0x00 and 0x40, this program's whole value pool at
     * 0x80.
     *
     * The samplers are found first, because whether this shader samples decides
     * whether it runs in whole-quad mode, which is emitted before the body.
     * --------------------------------------------------------------------- */
    const int tex_sets = p->hw_tex_sets;
    for (int s = 0; ok && s < tex_sets; s++) {
        const gl_uniform_t *u = &p->uniforms[p->hw_tex_uniform[s]];
        const uint32_t dim =
            (u->type == GL_SAMPLER_CUBE)
                ? GLSL_IMG_DIM_CUBE
                : ((u->type == GL_SAMPLER_3D) ? GLSL_IMG_DIM_3D : GLSL_IMG_DIM_2D);
        /* A shadow sampler's dim is still 2D; what differs is that it compares. A 1D
         * texture's is 2D as well - `glTexImage1D` stores one row and the descriptor
         * says TYPE 9 - so what differs there is the width of the coordinate. */
        const GLboolean shadow = (GLboolean)(u->type == GL_SAMPLER_2D_SHADOW ||
                                             u->type == GL_SAMPLER_1D_SHADOW);
        const GLboolean oned =
            (GLboolean)(u->type == GL_SAMPLER_1D || u->type == GL_SAMPLER_1D_SHADOW);
        if (!glsl_gen_declare_sampler(gen, u->name, lit_len(u->name), (uint32_t)s, dim,
                                      shadow, oned)) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "a sampler has no set", 0, 0);
            ok = GL_FALSE;
        }
    }
    /* A sampler the linker could not give a set to is named here, at its declaration,
     * rather than at the lookup. */
    for (int i = 0; ok && i < p->uniform_count; i++) {
        const gl_uniform_t *u = &p->uniforms[i];
        if (!gl_type_is_sampler(u->type))
            continue;
        if (!unit_declares_uniform(fs, u->name, lit_len(u->name)))
            continue;
        GLboolean has_set = GL_FALSE;
        for (int s = 0; s < tex_sets; s++) {
            if (p->hw_tex_uniform[s] == i) {
                has_set = GL_TRUE;
                break;
            }
        }
        if (has_set)
            continue;
        if (u->type != GL_SAMPLER_2D) {
            oops_snprintf(
                log, log_size,
                "uniform '%s' is a sampler this path does not carry; only sampler2D "
                "is generated",
                u->name);
        } else {
            oops_snprintf(
                log, log_size,
                "this shader samples through more than %d textures, and a draw "
                "carries that many descriptor sets",
                GLSL_GEN_MAX_TEX_SETS);
        }
        ok = GL_FALSE;
    }

    if (ok && p->value_floats > OOPS_GL_GL2_UNIFORM_FLOATS) {
        oops_snprintf(log, log_size,
                      "this program's uniforms are %d floats and a draw carries %d",
                      p->value_floats, OOPS_GL_GL2_UNIFORM_FLOATS);
        ok = GL_FALSE;
    }
    return ok;
}

/* The prologue: the mask `m0` holds, the block's scalar loads, whole-quad mode and the
 * saved live mask. */
static GLboolean ps_emit_prologue(const glsl_unit_t *fs, glsl_gen_t *gen,
                                  glsl_code_t *code, int tex_sets,
                                  GLboolean takes_block, GLboolean wants_fragcoord,
                                  uint32_t user_sgprs) {
    GLboolean ok = GL_TRUE;
    /* `m0` first, because every interpolation reads it; without it a shader reads
     * another primitive's parameters. See `glsl_emit_s_mov_m0`. The mask sits just past
     * the user data: s2 with the block, s0 without. */
    if (ok)
        glsl_emit_s_mov_m0(code, user_sgprs);

    /* One wait covers every load below, because `lgkmcnt(0)` waits for all of them.
     * Without it the shader reads zeros. */
    if (ok && takes_block) {
        for (int s = 0; s < tex_sets; s++) {
            const uint32_t base =
                GLSL_GEN_TEX_SGPR_BASE + (uint32_t)s * GLSL_GEN_TEX_SGPR_STRIDE;
            const uint32_t at = (uint32_t)s * OOPS_GL_DESC_UNIT_STRIDE;
            glsl_emit_s_load(code, GLSL_SMEM_LOAD_DWORDX8, base, 0u, at);
            glsl_emit_s_load(code, GLSL_SMEM_LOAD_DWORDX4, base + 8u, 0u, at + 32u);
        }
        /* The uniforms are not loaded here but a window at a time in
         * `ps_load_uniforms`. The descriptors stay resident for the whole shader,
         * because a sample reads them wherever it is. */
        /* The draw's constants, under the one wait below with everything else. */
        if (wants_fragcoord) {
            glsl_emit_s_load(code, GLSL_SMEM_LOAD_DWORDX4, GL_PS_DRAWCONST_SGPR_BASE,
                             0u, OOPS_GL_GL2_DRAWCONST_AT);
        }
        glsl_emit_s_waitcnt_lgkm(code);
    }

    /* Whole-quad mode if this shader samples or takes a derivative, entered here rather
     * than just before the lookup: the level of detail comes from how the coordinate
     * changes across the 2x2 quad, so every step that produced it must run in the
     * helper lanes too. Decided from the source, since the mode is entered before the
     * body. */
    if (ok &&
        (glsl_unit_mentions(fs, "dFdx", 4u) || glsl_unit_mentions(fs, "dFdy", 4u) ||
         glsl_unit_mentions(fs, "fwidth", 6u))) {
        gen->wqm = GL_TRUE;
    }
    /* The live mask - the lanes that should reach the export - is kept for every
     * shader. Whole-quad mode restores it before the export; `discard` takes a lane out
     * of it; a `return` in `main` does not, so a lane that returns early still exports
     * what `gl_FragColor` held. */
    if (ok) {
        glsl_emit_exec_save(code, GLSL_GEN_LIVE_SGPR);
        if (gen->wqm)
            glsl_emit_wqm(code);
    }
    return ok;
}

/* Every uniform the shader names, given VGPRs and loaded from the block a scalar
 * window at a time. */
static GLboolean ps_load_uniforms(const gl_program_object_t *p, const glsl_unit_t *fs,
                                  glsl_gen_t *gen, glsl_code_t *code, char *log,
                                  size_t log_size) {
    GLboolean ok = GL_TRUE;
    /* Each uniform this shader names is moved into a VGPR of its own and declared like
     * any other input, so the generator has one register class. The scalar registers
     * that carried it are then free, so the window slides: each pass loads 32 floats of
     * the block, waits, and moves that range into the registers reserved for it.
     *
     * Registers are assigned to every uniform first and the block is copied a float at
     * a time after, so a uniform wider than the window (`vec4 KernelValue[9]`, 36
     * floats) may span passes. What bounds a shader is VGPRs, one per uniform float. */
    struct {
        int at;
        int floats;
        uint32_t home;
    } resident[OOPS_GL_MAX_PROGRAM_UNIFORMS];
    int resident_count = 0;
    int pool_lo = OOPS_GL_GL2_UNIFORM_FLOATS, pool_hi = 0;

    for (int i = 0; ok && i < p->uniform_count; i++) {
        const gl_uniform_t *u = &p->uniforms[i];
        const size_t ulen = lit_len(u->name);
        if (!unit_declares_uniform(fs, u->name, ulen))
            continue;
        if (gl_type_is_sampler(u->type))
            continue; /* its descriptors are in the scalar file */
        const glsl_type_t t = type_from_gl(u->type);
        if (t == GLSL_TYPE_ERROR) {
            /* A type `type_from_gl` does not carry. Refused, since a skipped uniform
             * would read as zero. */
            oops_snprintf(
                log, log_size,
                "uniform '%s' is not a float, a float vector or a matrix, and the "
                "compiled path carries nothing else yet",
                u->name);
            ok = GL_FALSE;
            break;
        }
        const int size = (u->size > 0) ? u->size : 1;
        /* An array of uniforms is a run of registers, as `gl_TexCoord[]` is;
         * `gen_index_of` slices it by a constant index. A non-constant index is
         * refused: there is no addressable memory behind a run of VGPRs. */
        const glsl_value_t home =
            (size > 1) ? glsl_gen_declare_input_array(gen, u->name, ulen, t, size)
                       : glsl_gen_declare_input(gen, u->name, ulen, t);
        if (home.count == 0) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "a uniform has no register", 0, 0);
            ok = GL_FALSE;
            break;
        }
        if (resident_count >= OOPS_GL_MAX_PROGRAM_UNIFORMS) {
            glsl_log_write(log, log_size, "too many uniforms in one shader", 0, 0);
            ok = GL_FALSE;
            break;
        }
        resident[resident_count].at = u->offset;
        /* `floats` is per element, so the whole run is `floats * size`. */
        resident[resident_count].floats = u->floats * size;
        resident[resident_count].home = home.base;
        resident_count++;
        if (u->offset < pool_lo)
            pool_lo = u->offset;
        if (u->offset + u->floats * size > pool_hi)
            pool_hi = u->offset + u->floats * size;
    }

    /* The block, a window at a time. Aligned down to sixteen because that is the load's
     * own granularity and the destination has to stay four-aligned. */
    for (int base = pool_lo & ~15; ok && base < pool_hi;
         base += GL_PS_UNIFORM_WINDOW_FLOATS) {
        for (int off = 0; off < GL_PS_UNIFORM_WINDOW_FLOATS; off += 16) {
            /* Only the chunks that are inside the block. No uniform reaches past its
             * end, so a chunk left out is one no move below reads. */
            if (base + off >= OOPS_GL_GL2_UNIFORM_FLOATS)
                break;
            glsl_emit_s_load(code, GLSL_SMEM_LOAD_DWORDX16,
                             GL_PS_UNIFORM_SGPR_BASE + (uint32_t)off, 0u,
                             OOPS_GL_GL2_UNIFORM_AT + (uint32_t)(base + off) * 4u);
        }
        glsl_emit_s_waitcnt_lgkm(code);

        /* Relative to this pass's base: `s72` holds float `base` of the block, not
         * float 0. */
        for (int r = 0; r < resident_count; r++) {
            for (int c = 0; c < resident[r].floats; c++) {
                const int at = resident[r].at + c;
                if (at < base || at >= base + GL_PS_UNIFORM_WINDOW_FLOATS)
                    continue;
                glsl_emit_vop1(
                    code, GLSL_VOP1_MOV_B32, resident[r].home + (uint32_t)c,
                    glsl_sgpr(GL_PS_UNIFORM_SGPR_BASE + (uint32_t)(at - base)));
            }
        }
    }
    return ok;
}

/* `gl_FragCoord` and `gl_FrontFacing`, from the registers the SPI fills. */
static GLboolean ps_declare_window_inputs(glsl_gen_t *gen, glsl_code_t *code,
                                          GLboolean wants_fragcoord,
                                          GLboolean wants_frontfacing,
                                          uint32_t frontface_vgpr, char *log,
                                          size_t log_size) {
    GLboolean ok = GL_TRUE;
    /* `gl_FragCoord`, copied out of the registers the SPI filled. x, z and w are the
     * hardware's values; y is `height - y`, because GL measures it from the bottom of
     * the window and the hardware from the top (`gl_ps_patch_stipple` rotates its mask
     * for the same reason). Copied because v4 and v5 are also export registers. */
    if (ok && wants_fragcoord) {
        const glsl_value_t fc =
            glsl_gen_declare_input(gen, "gl_FragCoord", 12u, GLSL_TYPE_VEC4);
        if (fc.count != 4) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "gl_FragCoord has no register", 0,
                           0);
            ok = GL_FALSE;
        } else {
            glsl_emit_mov(code, fc.base + 0u, GL_PS_FRAGPOS_VGPR + 0u);
            /* `v_sub_f32` directly, not `glsl_emit_sub_f32`, which biases its first
             * operand as a VGPR: the height is an SGPR, and only `src0` can name one.
             */
            glsl_emit_vop2(
                code, GLSL_VOP2_SUB_F32, fc.base + 1u,
                glsl_sgpr(GL_PS_DRAWCONST_SGPR_BASE + OOPS_GL_GL2_DC_TARGET_H),
                GL_PS_FRAGPOS_VGPR + 1u);
            glsl_emit_mov(code, fc.base + 2u, GL_PS_FRAGPOS_VGPR + 2u);
            glsl_emit_mov(code, fc.base + 3u, GL_PS_FRAGPOS_VGPR + 3u);
        }
    }

    /* `gl_FrontFacing` is a sign, not a flag: the SPI hands over a float that is
     * positive for a front-facing primitive (Mesa lowers `load_front_face` as
     * `fgt(reg, 0)`). A bool here is 0.0 or 1.0, so this is a compare-and-select. */
    if (ok && wants_frontfacing) {
        const glsl_value_t ff =
            glsl_gen_declare_input(gen, "gl_FrontFacing", 14u, GLSL_TYPE_BOOL);
        if (ff.count != 1) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "gl_FrontFacing has no register",
                           0, 0);
            ok = GL_FALSE;
        } else {
            const uint32_t zero = glsl_gen_scratch(gen);
            const uint32_t one = glsl_gen_scratch(gen);
            if (gen->error) {
                glsl_log_write(log, log_size, gen->error, 0, 0);
                ok = GL_FALSE;
            } else {
                glsl_emit_mov_imm(code, zero, 0x00000000u);
                glsl_emit_mov_imm(code, one, 0x3f800000u); /* 1.0f */
                glsl_emit_cmp(code, GLSL_VOPC_GT_F32, frontface_vgpr, zero);
                glsl_emit_cndmask(code, ff.base, zero, one);
            }
        }
    }
    return ok;
}

/* The varyings, `gl_Color`, `gl_TexCoord[]` and `gl_PointCoord`, each interpolated into
 * registers of its own. */
static GLboolean ps_interpolate_inputs(const gl_program_object_t *p, glsl_gen_t *gen,
                                       glsl_code_t *code, char *log, size_t log_size) {
    GLboolean ok = GL_TRUE;
    /* ---------------------------------------------------------------------
     * Every varying interpolated into registers of its own: the interp pair writes
     * exactly the registers `glsl_gen_declare_input` returned, so the allocator and the
     * interpolation agree.
     * --------------------------------------------------------------------- */
    for (int i = 0; ok && i < p->varying_count; i++) {
        const gl_varying_t *v = &p->varyings[i];
        const glsl_type_t t = type_from_gl(v->type);
        if (t == GLSL_TYPE_ERROR) {
            oops_snprintf(
                log, log_size,
                "varying '%s' is not a float or a float vector, and the parameter "
                "interpolators carry nothing else",
                v->name);
            ok = GL_FALSE;
            break;
        }
        const glsl_value_t home =
            glsl_gen_declare_input(gen, v->name, lit_len(v->name), t);
        if (home.count == 0) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "a varying has no register", 0, 0);
            ok = GL_FALSE;
            break;
        }
        for (int c = 0; c < home.count; c++) {
            /* The linker laid the varyings out as a flat block of floats; float `n` is
             * component `n % 4` of parameter `n / 4`. */
            const int slot = v->offset + c;
            glsl_emit_interp_pair(code, home.base + (uint32_t)c, (uint32_t)(slot / 4),
                                  (uint32_t)(slot % 4));
        }
    }

    /* `gl_Color`, from the parameter the linker set aside (`hw_color_param`): past the
     * user's varyings with a vertex shader, parameter 0 without one. */
    if (ok && p->hw_color_param >= 0) {
        const glsl_value_t home =
            glsl_gen_declare_input(gen, "gl_Color", 8u, GLSL_TYPE_VEC4);
        if (home.count != 4) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "gl_Color has no register", 0, 0);
            ok = GL_FALSE;
        } else {
            for (int c = 0; c < 4; c++) {
                glsl_emit_interp_pair(code, home.base + (uint32_t)c,
                                      (uint32_t)p->hw_color_param, (uint32_t)c);
            }
        }
    }

    /* `gl_TexCoord[]`, one parameter an element from `hw_texcoord_param`: a run of
     * `OOPS_GL_MAX_TEXTURE_UNITS` vec4s that a constant index slices. */
    if (ok && p->hw_texcoord_param >= 0) {
        const glsl_value_t home = glsl_gen_declare_input_array(
            gen, "gl_TexCoord", 11u, GLSL_TYPE_VEC4, OOPS_GL_MAX_TEXTURE_UNITS);
        if (home.count != 4 * OOPS_GL_MAX_TEXTURE_UNITS) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "gl_TexCoord has no registers", 0,
                           0);
            ok = GL_FALSE;
        } else {
            for (int u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
                for (int c = 0; c < 4; c++) {
                    glsl_emit_interp_pair(code, home.base + (uint32_t)(u * 4 + c),
                                          (uint32_t)(p->hw_texcoord_param + u),
                                          (uint32_t)c);
                }
            }
        }
    }

    /* `gl_PointCoord`, texture coordinate 0's interpolant: the point expansion writes
     * the sprite coordinate into `tc[0]` (the part's own mechanism is
     * `SPI_PS_INPUT_CNTL.PT_SPRITE_TEX`). The linker refuses it alongside a vertex
     * shader, so the vertex has the fixed-function layout and the texture parameter is
     * 1 (`gl_draw.c`'s `memcpy(v + 32, uvs[k], 16)`). */
    if (ok && p->hw_reads_point_coord) {
        const glsl_value_t home =
            glsl_gen_declare_input(gen, "gl_PointCoord", 13u, GLSL_TYPE_VEC2);
        if (home.count != 2) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "gl_PointCoord has no register", 0,
                           0);
            ok = GL_FALSE;
        } else {
            for (int c = 0; c < 2; c++) {
                glsl_emit_interp_pair(code, home.base + (uint32_t)c, 1u, (uint32_t)c);
            }
        }
    }
    return ok;
}

/* The shader's own globals, then the outputs it writes. */
static GLboolean ps_declare_globals_outputs(const glsl_unit_t *fs, glsl_gen_t *gen,
                                            glsl_code_t *code, GLboolean wants_fragdata,
                                            GLboolean wants_fragdepth, char *log,
                                            size_t log_size) {
    GLboolean ok = GL_TRUE;
    /* ---------------------------------------------------------------------
     * The shader's own globals - `const float pi = 3.14159;` and the rest.
     *
     * Generated by the same arm as a local, in source order, so one `const` may be
     * written in terms of an earlier one. A `const` is not folded: that would need a
     * second constant evaluator beside `glsl_exec.c`'s, able to disagree with it.
     *
     * Uniforms, varyings and attributes are skipped; each has its own arm above.
     * --------------------------------------------------------------------- */
    if (ok && fs->root != GLSL_NO_NODE) {
        for (int32_t d = fs->ast.nodes[fs->root].a; ok && d != GLSL_NO_NODE;
             d = fs->ast.nodes[d].sibling) {
            const glsl_node_t *gn = &fs->ast.nodes[d];
            if (gn->kind != GLSL_NODE_DECL)
                continue;
            if (gn->qualifier == GLSL_TOK_KW_UNIFORM ||
                gn->qualifier == GLSL_TOK_KW_VARYING ||
                gn->qualifier == GLSL_TOK_KW_ATTRIBUTE) {
                continue;
            }
            if (!glsl_gen_stmt(gen, d)) {
                glsl_log_write(log, log_size,
                               gen->error ? gen->error
                                          : "this global has no instruction selection",
                               gen->error_line, gen->error_column);
                ok = GL_FALSE;
            }
        }
    }

    /* `gl_FragColor` is a variable like any other, moved into the export registers at
     * the end. `gl_FragData[0]` is the same buffer under GLSL 1.10's other name (1.10,
     * 7.2), a one-element array because there is one draw buffer. */
    if (ok) {
        const glsl_value_t colour =
            glsl_gen_declare_input(gen, "gl_FragColor", 12u, GLSL_TYPE_VEC4);
        if (colour.count == 0) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "gl_FragColor has no register", 0,
                           0);
            ok = GL_FALSE;
        }
    }
    if (ok && wants_fragdata) {
        const glsl_value_t data =
            glsl_gen_declare_input_array(gen, "gl_FragData", 11u, GLSL_TYPE_VEC4, 1);
        if (data.count != 4) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "gl_FragData has no register", 0,
                           0);
            ok = GL_FALSE;
        }
    }

    /* `gl_FragDepth`, declared only for a shader that names it, and seeded with the
     * interpolated z. GLSL leaves an unwritten path undefined; `glsl_exec.c` carries
     * `gl_FragCoord.z` there too, so the two paths agree. */
    if (ok && wants_fragdepth) {
        const glsl_value_t depth =
            glsl_gen_declare_input(gen, "gl_FragDepth", 12u, GLSL_TYPE_FLOAT);
        if (depth.count != 1) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "gl_FragDepth has no register", 0,
                           0);
            ok = GL_FALSE;
        } else {
            glsl_emit_mov(code, depth.base,
                          GL_PS_FRAGPOS_VGPR + 2u); /* the interpolated z */
        }
    }
    return ok;
}

/* The body of `main`. */
static GLboolean ps_gen_main(const glsl_unit_t *fs, glsl_gen_t *gen, char *log,
                             size_t log_size) {
    GLboolean ok = GL_TRUE;
    if (ok) {
        const int32_t main_fn = find_main(fs);
        if (main_fn == GLSL_NO_NODE) {
            glsl_log_write(log, log_size, "the fragment shader has no main", 0, 0);
            ok = GL_FALSE;
        } else {
            const int32_t body = fs->ast.nodes[main_fn].c;
            for (int32_t st = fs->ast.nodes[body].a; st != GLSL_NO_NODE;
                 st = fs->ast.nodes[st].sibling) {
                if (!glsl_gen_stmt(gen, st)) {
                    glsl_log_write(log, log_size,
                                   gen->error
                                       ? gen->error
                                       : "this shader has no instruction selection",
                                   gen->error_line, gen->error_column);
                    ok = GL_FALSE;
                    break;
                }
            }
        }
    }
    return ok;
}

/* The epilogue: the live mask back, the exports, and room for a second colour
 * export. */
static GLboolean ps_emit_epilogue(glsl_gen_t *gen, glsl_code_t *code,
                                  GLboolean wants_fragdata, GLboolean wants_fragdepth,
                                  char *log, size_t log_size) {
    GLboolean ok = GL_TRUE;
    if (ok) {
        /* Whichever name this shader wrote. A shader that named neither exports
         * `gl_FragColor` unassigned. */
        const char *out_name = wants_fragdata ? "gl_FragData" : "gl_FragColor";
        const size_t out_len = wants_fragdata ? 11u : 12u;
        glsl_value_t colour;
        if (!glsl_gen_lookup(gen, out_name, out_len, &colour) || colour.count != 4) {
            glsl_log_write(log, log_size,
                           "the fragment colour did not survive to the export", 0, 0);
            ok = GL_FALSE;
        } else {
            /* Back to the lanes that should export: this leaves whole-quad mode, keeps
             * discarded lanes out, and brings back lanes that returned early from
             * `main`. */
            glsl_emit_exec_restore(code, GLSL_GEN_LIVE_SGPR);
            for (uint32_t i = 0; i < 4u; i++) {
                /* Never a move onto itself: the export registers lie below everything
                 * the allocator hands out. */
                glsl_emit_mov(code, GL_PS_EXPORT_BASE + i, colour.base + i);
            }
            /* The depth goes first, because the colour export is the one that says
             * `done`. */
            if (wants_fragdepth) {
                glsl_value_t depth;
                if (glsl_gen_lookup(gen, "gl_FragDepth", 12u, &depth) &&
                    depth.count == 1) {
                    glsl_emit_export_mrtz(code, depth.base);
                } else {
                    glsl_log_write(log, log_size,
                                   "gl_FragDepth did not survive to the export", 0, 0);
                    ok = GL_FALSE;
                }
            }
            glsl_emit_export_mrt0(code, GL_PS_EXPORT_BASE);
            glsl_emit_endpgm(code);
            /* Room for a second export, for a draw into two colour buffers. These five
             * words match `gl_ps_export_words(GL_FALSE)`'s first five, and the two
             * `s_nop`s take the tail to `GL_PS_EXPORT_WORDS`, so the draw path can
             * write the two-target form over it in place when `fb_also` is bound, as it
             * does for the payload's fixed-function shaders. That form is what
             * `CB_SHADER_MASK` 0xff and `SPI_SHADER_COL_FORMAT` 0x44 expect
             * (gl2-probe's `two-draw-buffers`). On a one-target draw nothing runs after
             * `s_endpgm`. */
            glsl_emit_nop(code);
            glsl_emit_nop(code);
        }
    }
    return ok;
}
