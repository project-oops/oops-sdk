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
 * the texture descriptors take s4..s27 and the mask registers s28..s40 - `glsl_internal.h` has
 * the map. s48 is the first multiple of four clear of all of it, and a multiple of four is what
 * a scalar load of four dwords or more needs; the loads march on by sixteen from here, so every
 * one of them lands on one too. */
#define GL_PS_UNIFORM_SGPR_BASE 48u

/* **The draw's own constants**, loaded into s44..s47 - the last 4-aligned group below the
 * uniforms and above the exec masks, which end at s40. Four dwords, so a 4-aligned destination
 * is what the load needs and s44 is one. */
#define GL_PS_DRAWCONST_SGPR_BASE 44u

/* **Where the hardware puts the fragment's window position**, when `SPI_PS_INPUT_ENA` asks for
 * it. The VGPRs are packed in the order Mesa enumerates them (`ac_get_fs_input_vgpr_cnt`,
 * `ac_shader_util.c`): the perspective-centre barycentrics take v0 and v1, and x, y, z and w
 * follow one register each. The payload's polygon stipple reads the same pair at v2 and v3 with
 * `ENA` 0x302, which is the in-tree confirmation of the order.
 *
 * These are read in the prologue and copied into registers of their own, so nothing downstream
 * depends on the layout - the same treatment a uniform gets, and for the same reason. */
#define GL_PS_FRAGPOS_VGPR 2u

/* `SPI_PS_INPUT_ENA` bits, from `R_0286CC_SPI_PS_INPUT_ENA` for gfx103 - not `R_02865C`, which
 * is that register only from gfx12 and is `SPI_PS_INPUT_CNTL_6` here. */
#define GL_PS_INPUT_PERSP_CENTER 0x00000002u
#define GL_PS_INPUT_POS_XYZW     0x00000f00u
#define GL_PS_INPUT_FRONT_FACE   0x00001000u

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
        /* **An `int` or `bool` uniform is carried as the float it already is.** The program's
         * value pool has one representation for every uniform (`gl_internal.h` says why), so
         * `glUniform1i(ortho, 1)` has already become 1.0f before this sees it - and a shader
         * that reads such a uniform reads it through `bool()` or a comparison, both of which
         * work on that float unchanged. Integer *arithmetic* on it is still refused: the type
         * stays `int` here, and the generator has no verified instruction for it. */
        case GL_INT: return GLSL_TYPE_INT;
        case GL_BOOL: return GLSL_TYPE_BOOL;
        default: return GLSL_TYPE_ERROR;
    }
}

/* Whether this unit declares `name` as a uniform of its own.
 *
 * **The program's uniform pool is both stages' uniforms together**, which is what lets one
 * `mat4 mvp` be one uniform - so the pool holds names this shader never mentions. They are
 * loaded into SGPRs regardless, because the block is copied whole and a scalar load is cheap;
 * what they must not cost is a **VGPR each**, and a vertex-only `mat4` would cost sixteen. */
/* **Does this shader name it anywhere?** A built-in is not declared, so there is no declaration
 * list to walk and the question is about the whole body. The AST is a flat array, so this reads
 * every node once rather than walking the tree - which also means a mention inside a branch the
 * generator will never take still counts, and that is the right answer: the prologue has to be
 * emitted before anything knows which branches there are. */
/* **Whether this unit can throw a fragment away.**
 *
 * Not `glsl_unit_mentions("discard")`: that walks the identifiers, and `discard` is a keyword -
 * it parses to a statement node and never to a name, so the search would look in the one place
 * it cannot be and answer no every time. The node kind is the only thing that says it.
 *
 * Every `discard` in the unit counts, including ones in functions `main` never calls. That
 * over-reports, and the cost of over-reporting is early Z given up on a draw that did not need
 * to - while under-reporting is a depth block that never hears about the kill. */
GLboolean glsl_unit_discards(const glsl_unit_t *u) {
    if (!u) return GL_FALSE;
    for (int32_t i = 0; i < u->ast.count; i++) {
        if (u->ast.nodes[i].kind == GLSL_NODE_DISCARD) return GL_TRUE;
    }
    return GL_FALSE;
}

GLboolean glsl_unit_mentions(const glsl_unit_t *u, const char *name, size_t len) {
    if (!u) return GL_FALSE;
    for (int32_t i = 0; i < u->ast.count; i++) {
        const glsl_node_t *n = &u->ast.nodes[i];
        if (n->kind != GLSL_NODE_IDENTIFIER || (size_t)n->length != len) continue;
        GLboolean same = GL_TRUE;
        for (size_t c = 0; c < len; c++) {
            if (n->text[c] != name[c]) { same = GL_FALSE; break; }
        }
        if (same) return GL_TRUE;
    }
    return GL_FALSE;
}

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
                                      uint32_t *out_vgprs, uint32_t *out_user_sgprs,
                                      uint32_t *out_input_ena, char *log, size_t log_size) {
    if (log && log_size) log[0] = '\0';
    if (out_count) *out_count = 0u;
    if (out_vgprs) *out_vgprs = 0u;
    if (out_user_sgprs) *out_user_sgprs = 0u;
    /* The barycentrics whatever happens, so a caller that ignores a failure still configures a
     * stage the fixed-function shaders can run in. */
    if (out_input_ena) *out_input_ena = GL_PS_INPUT_PERSP_CENTER;
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

    /* **The unit's own functions, back into the table.** `glsl_check_unit` recorded them during
     * the compile and this symbol table is a fresh one, so without this a call to a function the
     * shader defines has no signature to be typed against - and the generator, which asks this
     * table for the type of every operand, cannot tell what a helper returns. It read as
     * "only float, vec and mat constructor arguments are generated" the first time a helper's
     * result was passed to `vec4`, which is a message about the wrong thing entirely. */
    if (ok) {
        for (int32_t d = fs->ast.nodes[fs->root].a; d != GLSL_NO_NODE;
             d = fs->ast.nodes[d].sibling) {
            if (fs->ast.nodes[d].kind != GLSL_NODE_FUNCTION) continue;
            if (!glsl_declare_function(sema, d)) {
                log_say(log, log_size, sema->error ? sema->error : "a function has no signature",
                        0, 0);
                ok = GL_FALSE;
                break;
            }
        }
    }

    glsl_gen_init(gen, (glsl_ast_t *)&fs->ast, sema, &code);
    glsl_gen_reserve(gen, GL_PS_FIRST_FREE_VGPR);

    /* ---------------------------------------------------------------------
     * The block, before anything else touches a register.
     *
     * The draw puts one block in the payload and its address in the pixel shader's first user
     * SGPR pair, so everything below is at an offset from `s[0:1]`: a texture unit's descriptors
     * at 0x00 and 0x40, this program's whole value pool at 0x80.
     *
     * **The samplers are found first**, because whether this shader samples decides whether it
     * runs in whole-quad mode - and that has to be emitted before any of the body, by which time
     * finding out would be too late.
     * --------------------------------------------------------------------- */
    const int tex_sets = p->hw_tex_sets;
    for (int s = 0; ok && s < tex_sets; s++) {
        const gl_uniform_t *u = &p->uniforms[p->hw_tex_uniform[s]];
        const uint32_t dim = (u->type == GL_SAMPLER_CUBE)
                                 ? GLSL_IMG_DIM_CUBE
                                 : ((u->type == GL_SAMPLER_3D) ? GLSL_IMG_DIM_3D
                                                               : GLSL_IMG_DIM_2D);
        /* A shadow sampler's dim is still 2D; what differs is that it compares. A 1D texture's
         * is 2D as well - `glTexImage1D` stores one row and the descriptor says TYPE 9 - so
         * what differs there is the width of the coordinate. */
        const GLboolean shadow = (GLboolean)(u->type == GL_SAMPLER_2D_SHADOW ||
                                             u->type == GL_SAMPLER_1D_SHADOW);
        const GLboolean oned = (GLboolean)(u->type == GL_SAMPLER_1D ||
                                           u->type == GL_SAMPLER_1D_SHADOW);
        if (!glsl_gen_declare_sampler(gen, u->name, lit_len(u->name), (uint32_t)s, dim, shadow,
                                      oned)) {
            log_say(log, log_size, gen->error ? gen->error : "a sampler has no set", 0, 0);
            ok = GL_FALSE;
        }
    }
    /* A sampler the linker could not give a set to - because there were more than the draw
     * carries, or because it is not a 2D one - is named here rather than at the lookup. The
     * lookup's message would be about the function; this one is about the declaration, and a
     * shader declaring a `samplerCube` was never going to sample it with `texture2D`. */
    for (int i = 0; ok && i < p->uniform_count; i++) {
        const gl_uniform_t *u = &p->uniforms[i];
        if (!gl_type_is_sampler(u->type)) continue;
        if (!unit_declares_uniform(fs, u->name, lit_len(u->name))) continue;
        GLboolean has_set = GL_FALSE;
        for (int s = 0; s < tex_sets; s++) {
            if (p->hw_tex_uniform[s] == i) { has_set = GL_TRUE; break; }
        }
        if (has_set) continue;
        if (u->type != GL_SAMPLER_2D) {
            oops_snprintf(log, log_size,
                          "uniform '%s' is a sampler this path does not carry; only sampler2D "
                          "is generated", u->name);
        } else {
            oops_snprintf(log, log_size,
                          "this shader samples through more than %d textures, and a draw "
                          "carries that many descriptor sets", GLSL_GEN_MAX_TEX_SETS);
        }
        ok = GL_FALSE;
    }

    if (ok && p->value_floats > OOPS_GL_GL2_UNIFORM_FLOATS) {
        oops_snprintf(log, log_size,
                      "this program's uniforms are %d floats and a draw carries %d",
                      p->value_floats, OOPS_GL_GL2_UNIFORM_FLOATS);
        ok = GL_FALSE;
    }

    /* **Whether this shader is handed the block at all**, which decides two things together and
     * so is decided once: the scalar loads below, and how many user SGPRs the draw configures -
     * which in turn is where the SPI puts the primitive mask. The draw path reads the answer
     * back off the program (`hw_ps_user_sgprs`) rather than working it out a second time, so
     * the shader and the register that feeds it cannot come to different conclusions. */
    /* **`gl_FragCoord` needs the block too**, for the viewport height that flips its y - so it
     * joins the two things that already decide whether this shader is handed one. */
    const GLboolean wants_fragcoord = glsl_unit_mentions(fs, "gl_FragCoord", 12u);
    /* `gl_FrontFacing` needs no block - the SPI hands it over in a register of its own. */
    const GLboolean wants_frontfacing = glsl_unit_mentions(fs, "gl_FrontFacing", 14u);
    /* **`gl_FragDepth` needs the window position too**, for the interpolated z it starts at -
     * so it asks for the same registers `gl_FragCoord` does, and a shader naming either gets
     * them. */
    const GLboolean wants_fragdepth = glsl_unit_mentions(fs, "gl_FragDepth", 12u);
    const GLboolean takes_block =
        (GLboolean)(tex_sets > 0 || p->value_floats > 0 || wants_fragcoord);
    const uint32_t user_sgprs = takes_block ? 2u : 0u;
    const uint32_t input_ena = GL_PS_INPUT_PERSP_CENTER |
                               ((wants_fragcoord || wants_fragdepth) ? GL_PS_INPUT_POS_XYZW
                                                                     : 0u) |
                               (wants_frontfacing ? GL_PS_INPUT_FRONT_FACE : 0u);
    /* **Where the front-face register lands, which depends on what else was asked for.** The
     * SPI packs the enabled inputs in the order Mesa enumerates them, so the face follows the
     * position when the position is there and sits straight after the barycentrics when it is
     * not. Computed rather than fixed, because pinning it would mean asking for four registers
     * of window position that the shader never reads just to keep this one in place. */
    const uint32_t frontface_vgpr =
        GL_PS_FRAGPOS_VGPR + ((wants_fragcoord || wants_fragdepth) ? 4u : 0u);

    /* **`m0` first, because every interpolation reads it** - and because a shader that skips it
     * still runs, still exports, and draws a surface speckled with another primitive's
     * parameters. See `glsl_emit_s_mov_m0`. The mask sits just past the user data: s2 with the
     * block, s0 without. */
    if (ok) glsl_emit_s_mov_m0(&code, user_sgprs);

    /* **One wait covers every load below**, because `lgkmcnt(0)` waits for all of them and not
     * for one. Leaving it out is not a slower shader but a wrong one: obSCEne's `-6c0d` ran
     * exactly that arm and the shader read all zeros. */
    if (ok && takes_block) {
        for (int s = 0; s < tex_sets; s++) {
            const uint32_t base =
                GLSL_GEN_TEX_SGPR_BASE + (uint32_t)s * GLSL_GEN_TEX_SGPR_STRIDE;
            const uint32_t at = (uint32_t)s * OOPS_GL_DESC_UNIT_STRIDE;
            glsl_emit_s_load(&code, GLSL_SMEM_LOAD_DWORDX8, base, 0u, at);
            glsl_emit_s_load(&code, GLSL_SMEM_LOAD_DWORDX4, base + 8u, 0u, at + 32u);
        }
        for (int base = 0; base < p->value_floats; base += 16) {
            glsl_emit_s_load(&code, GLSL_SMEM_LOAD_DWORDX16,
                             GL_PS_UNIFORM_SGPR_BASE + (uint32_t)base, 0u,
                             OOPS_GL_GL2_UNIFORM_AT + (uint32_t)base * 4u);
        }
        /* The draw's constants, under the one wait below with everything else. */
        if (wants_fragcoord) {
            glsl_emit_s_load(&code, GLSL_SMEM_LOAD_DWORDX4, GL_PS_DRAWCONST_SGPR_BASE, 0u,
                             OOPS_GL_GL2_DRAWCONST_AT);
        }
        glsl_emit_s_waitcnt_lgkm(&code);
    }

    /* **Whole-quad mode, if this shader samples**, and from here rather than from just before
     * the lookup: `image_sample` takes its level of detail from how the coordinate changes
     * across the 2x2 quad, so every step that *produced* that coordinate has to have run in the
     * helper lanes too - which means the interpolation below and whatever the body does to it.
     * The live mask is kept and put back before the export. */
    /* **A derivative needs whole-quad mode too**, and for the same reason a sample does: it
     * reads the lane next door, and in a lane the primitive does not cover there is a value
     * there only because WQM kept that lane running. Decided from the source rather than from
     * generation, because the mode has to be entered before any of the body runs and by then
     * it would be too late to find out. */
    if (ok && (glsl_unit_mentions(fs, "dFdx", 4u) || glsl_unit_mentions(fs, "dFdy", 4u) ||
               glsl_unit_mentions(fs, "fwidth", 6u))) {
        gen->wqm = GL_TRUE;
    }
    if (ok && gen->wqm) {
        glsl_emit_exec_save(&code, GLSL_GEN_LIVE_SGPR);
        glsl_emit_wqm(&code);
    }

    /* Each uniform this shader actually names is moved into a VGPR of its own and declared like
     * any other input, so the rest of the generator sees a variable and needs no notion of an
     * SGPR at all. That costs one register and one instruction a float; the alternative -
     * teaching every operand path that a value might live in an SGPR - would buy those back at
     * the price of a second register class in a back end that has one. */
    for (int i = 0; ok && i < p->uniform_count; i++) {
        const gl_uniform_t *u = &p->uniforms[i];
        const size_t ulen = lit_len(u->name);
        if (!unit_declares_uniform(fs, u->name, ulen)) continue;
        if (gl_type_is_sampler(u->type)) continue; /* its descriptors are in the scalar file */
        const glsl_type_t t = type_from_gl(u->type);
        if (t == GLSL_TYPE_ERROR || u->size != 1) {
            /* A matrix or an array: the first would need a wider `type_from_gl`, the second an
             * index this back end cannot generate. Named rather than silently skipped, because
             * a skipped uniform reads as zero and draws. */
            oops_snprintf(log, log_size,
                          "uniform '%s' is not a float or a float vector, and the compiled "
                          "path carries nothing else yet", u->name);
            ok = GL_FALSE;
            break;
        }
        const glsl_value_t home = glsl_gen_declare_input(gen, u->name, ulen, t);
        if (home.count == 0) {
            log_say(log, log_size, gen->error ? gen->error : "a uniform has no register", 0, 0);
            ok = GL_FALSE;
            break;
        }
        for (int c = 0; c < home.count; c++) {
            glsl_emit_vop1(&code, GLSL_VOP1_MOV_B32, home.base + (uint32_t)c,
                           glsl_sgpr(GL_PS_UNIFORM_SGPR_BASE + (uint32_t)(u->offset + c)));
        }
    }

    /* **`gl_FragCoord`, copied out of the registers the SPI filled and into the allocator's.**
     *
     * x, z and w are the hardware's values. **y is not**: GL measures `gl_FragCoord.y` from the
     * bottom of the window and the hardware hands down the row from the top, so this is
     * `height - y`. The payload's polygon stipple is the in-tree witness - `gl_ps_patch_stipple`
     * rotates its mask for the window height precisely because the two count opposite ways, and
     * a shader that skipped the flip would draw every gradient upside down.
     *
     * Copied rather than used where they lie, because v4 and v5 are also the export registers:
     * the epilogue writes them last, so reading them in place would work and would be one
     * reordering away from not working. */
    if (ok && wants_fragcoord) {
        const glsl_value_t fc = glsl_gen_declare_input(gen, "gl_FragCoord", 12u, GLSL_TYPE_VEC4);
        if (fc.count != 4) {
            log_say(log, log_size, gen->error ? gen->error : "gl_FragCoord has no register", 0,
                    0);
            ok = GL_FALSE;
        } else {
            glsl_emit_mov(&code, fc.base + 0u, GL_PS_FRAGPOS_VGPR + 0u);
            /* **`v_sub_f32` straight**, not through `glsl_emit_sub_f32`: that helper puts its
             * first operand through `glsl_vgpr`, and this one is a scalar register. VOP2's
             * `src0` is the nine-bit operand field that takes either, while `vsrc1` is a VGPR
             * number and nothing else - so the height has to be the first operand, which is
             * also the order the subtraction wants. */
            glsl_emit_vop2(&code, GLSL_VOP2_SUB_F32, fc.base + 1u,
                           glsl_sgpr(GL_PS_DRAWCONST_SGPR_BASE + OOPS_GL_GL2_DC_TARGET_H),
                           GL_PS_FRAGPOS_VGPR + 1u);
            glsl_emit_mov(&code, fc.base + 2u, GL_PS_FRAGPOS_VGPR + 2u);
            glsl_emit_mov(&code, fc.base + 3u, GL_PS_FRAGPOS_VGPR + 3u);
        }
    }

    /* **`gl_FrontFacing`, which is a sign and not a flag.** The SPI hands over a float that is
     * positive for a front-facing primitive - Mesa lowers `load_front_face` as `fgt(reg, 0)`
     * and `load_front_face_fsign` as the register itself, which is what says it is a float and
     * not a zero/one integer. A back end that treated it as a boolean directly would read a
     * negative number as true and answer "front" for every fragment.
     *
     * A bool here is a float 0.0 or 1.0 like any other, so this is the same compare-and-select
     * the language's own comparisons use. */
    if (ok && wants_frontfacing) {
        const glsl_value_t ff =
            glsl_gen_declare_input(gen, "gl_FrontFacing", 14u, GLSL_TYPE_BOOL);
        if (ff.count != 1) {
            log_say(log, log_size, gen->error ? gen->error : "gl_FrontFacing has no register", 0,
                    0);
            ok = GL_FALSE;
        } else {
            const uint32_t zero = glsl_gen_scratch(gen);
            const uint32_t one = glsl_gen_scratch(gen);
            if (gen->error) {
                log_say(log, log_size, gen->error, 0, 0);
                ok = GL_FALSE;
            } else {
                glsl_emit_mov_imm(&code, zero, 0x00000000u);
                glsl_emit_mov_imm(&code, one, 0x3f800000u); /* 1.0f */
                glsl_emit_cmp(&code, GLSL_VOPC_GT_F32, frontface_vgpr, zero);
                glsl_emit_cndmask(&code, ff.base, zero, one);
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

    /* **`gl_Color`, interpolated from the parameter the linker set aside for it.**
     *
     * The fixed-function colour is not a user varying and owns no slot until a fragment shader
     * asks for one, so `hw_color_param` is where it ended up - the first parameter past the
     * user's when there is a vertex shader, and parameter 0 when there is not, because the
     * fixed-function vertex path has always written it there. The two cases differ, which is
     * why the number comes from the link and is not worked out again here. */
    if (ok && p->hw_color_param >= 0) {
        const glsl_value_t home =
            glsl_gen_declare_input(gen, "gl_Color", 8u, GLSL_TYPE_VEC4);
        if (home.count != 4) {
            log_say(log, log_size, gen->error ? gen->error : "gl_Color has no register", 0, 0);
            ok = GL_FALSE;
        } else {
            for (int c = 0; c < 4; c++) {
                glsl_emit_interp_pair(&code, home.base + (uint32_t)c,
                                      (uint32_t)p->hw_color_param, (uint32_t)c);
            }
        }
    }

    /*
     * **`gl_PointCoord`, interpolated from the texture parameter** (since 2026-09-23).
     *
     * It is texture coordinate 0's interpolant - the point expansion writes the sprite
     * coordinate into `tc[0]`, the vertex assembly copies that unit's set into the texture
     * parameter, and the part's own name for the same substitution is
     * `SPI_PS_INPUT_CNTL.PT_SPRITE_TEX`.
     *
     * **Parameter 1, and the link is what makes that a constant rather than a guess.** A program
     * reading `gl_PointCoord` is refused if it has a vertex shader, so the vertex always takes
     * the fixed-function layout - position, then the colour at parameter 0, then the texture
     * parameter at 1 (`gl_draw.c`'s `memcpy(v + 32, uvs[k], 16)`). With a vertex shader the
     * parameters are the program's own varyings and 1 would mean something else entirely, which
     * is exactly why that case is not allowed to reach here.
     *
     * Without this the front end knew the name, the interpreter had a value for it and the
     * linker had an opinion about it, and the console had no register - which is what the
     * hardware said, in as many words: "this name has no register". The host passed throughout,
     * because the host *is* the interpreter.
     */
    if (ok && p->hw_reads_point_coord) {
        const glsl_value_t home =
            glsl_gen_declare_input(gen, "gl_PointCoord", 13u, GLSL_TYPE_VEC2);
        if (home.count != 2) {
            log_say(log, log_size, gen->error ? gen->error : "gl_PointCoord has no register", 0,
                    0);
            ok = GL_FALSE;
        } else {
            for (int c = 0; c < 2; c++) {
                glsl_emit_interp_pair(&code, home.base + (uint32_t)c, 1u, (uint32_t)c);
            }
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

    /* **`gl_FragDepth` is the same thing for depth**, declared only for a shader that names it
     * so nothing is charged for a register or an export it never uses.
     *
     * Seeded with the interpolated depth rather than left as whatever the allocator held. GLSL
     * says a shader that writes it on one path and not another leaves the value undefined on
     * the other, and `glsl_exec.c` carries `gl_FragCoord.z` there - so this reads the same
     * value, which is the one arrangement where the two paths agree about a shader the language
     * does not pin down. It needs the window position for that, which is why the mention
     * enables it. */
    if (ok && wants_fragdepth) {
        const glsl_value_t depth =
            glsl_gen_declare_input(gen, "gl_FragDepth", 12u, GLSL_TYPE_FLOAT);
        if (depth.count != 1) {
            log_say(log, log_size, gen->error ? gen->error : "gl_FragDepth has no register", 0,
                    0);
            ok = GL_FALSE;
        } else {
            glsl_emit_mov(&code, depth.base, GL_PS_FRAGPOS_VGPR + 2u); /* the interpolated z */
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
            /* **Out of whole-quad mode before anything is written.** The helper lanes were on
             * so the sample's derivatives would exist; letting them reach the export would put
             * fragments on screen that the primitive does not cover. */
            if (gen->wqm) glsl_emit_exec_restore(&code, GLSL_GEN_LIVE_SGPR);
            for (uint32_t i = 0; i < 4u; i++) {
                /* A move onto itself would be a wasted instruction rather than a wrong one, and
                 * the export registers are below everything the allocator hands out - so this
                 * never is one, and the check is left out rather than written and never taken. */
                glsl_emit_mov(&code, GL_PS_EXPORT_BASE + i, colour.base + i);
            }
            /* **The depth goes first, because the colour export is the one that says `done`.**
             * Two exports both claiming to be the last is a shader that does not retire. */
            if (wants_fragdepth) {
                glsl_value_t depth;
                if (glsl_gen_lookup(gen, "gl_FragDepth", 12u, &depth) && depth.count == 1) {
                    glsl_emit_export_mrtz(&code, depth.base);
                } else {
                    log_say(log, log_size, "gl_FragDepth did not survive to the export", 0, 0);
                    ok = GL_FALSE;
                }
            }
            glsl_emit_export_mrt0(&code, GL_PS_EXPORT_BASE);
            glsl_emit_endpgm(&code);
            /*
             * **Room for a second export, so a draw into two colour buffers can have one.**
             *
             * These five words are byte-identical to `gl_ps_export_words(GL_FALSE)`'s first
             * five, and the two `s_nop`s take the tail to `GL_PS_EXPORT_WORDS` - which is what
             * lets the draw path write the two-target form over it in place when `fb_also` is
             * bound, exactly as it already does for the payload's fixed-function shaders.
             *
             * Without the room, `glDrawBuffer(GL_FRONT_AND_BACK)` under a compiled program set
             * `CB_SHADER_MASK` to 0xff and `SPI_SHADER_COL_FORMAT` to 0x44 - telling the colour
             * block to expect two exports - while the shader made one. gl2-probe's
             * `two-draw-buffers` measured the consequence as a front buffer holding exactly its
             * pre-draw colour: not a wrong blend, an absent write.
             *
             * Nothing runs after `s_endpgm`, so on a one-target draw these are never reached.
             */
            glsl_emit_nop(&code);
            glsl_emit_nop(&code);
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
        if (out_user_sgprs) *out_user_sgprs = user_sgprs;
        if (out_input_ena) *out_input_ena = input_ena;
    }
    gl_heap_free(sema);
    gl_heap_free(gen);
    return ok;
}
