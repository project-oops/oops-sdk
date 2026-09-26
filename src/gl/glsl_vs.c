/*
 * oops-gl: a GLSL vertex shader compiled into native gfx1030 instructions
 *
 * Implements the hardware vertex pipeline (D014) for OpenGL 2.0.
 *
 * A compiled vertex shader:
 * - Loads uniforms from the GL 2.0 uniform block in s[8:9] (user SGPRs 0..1 in NGG/GS
 * stage).
 * - Declares vertex attributes and loads them into VGPRs.
 * - Declares built-ins (gl_Position, gl_FrontColor) and user varyings.
 * - Lowers the vertex shader AST statements in main().
 * - Epilogue: exports varyings to SPI parameters (param0..paramN), exports gl_Position
 *   to POS0 with done=GL_TRUE, waits for export completion, and terminates (s_endpgm).
 */

#include "glsl_internal.h"

#define GL_VS_EXPORT_POS_BASE 4u
#define GL_VS_FIRST_FREE_VGPR 16u
#define GL_VS_MAX_PARAM_FLOATS 16
#define GL_VS_MAX_VGPRS 136u
#define GL_VS_UNIFORM_SGPR_BASE 72u
#define GL_VS_UNIFORM_WINDOW_FLOATS 32
#define GL_VS_UNIFORM_BLOCK_SBASE 4u /* s8 / 2 */

static size_t lit_len(const char *s) {
    if (!s)
        return 0;
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

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
    case GL_FLOAT_MAT2:
        return GLSL_TYPE_MAT2;
    case GL_FLOAT_MAT3:
        return GLSL_TYPE_MAT3;
    case GL_FLOAT_MAT4:
        return GLSL_TYPE_MAT4;
    case GL_INT:
        return GLSL_TYPE_INT;
    case GL_BOOL:
        return GLSL_TYPE_BOOL;
    default:
        return GLSL_TYPE_ERROR;
    }
}

static GLboolean name_matches(const char *a, size_t alen, const char *b, size_t blen) {
    if (alen != blen || !a || !b)
        return GL_FALSE;
    for (size_t i = 0; i < alen; i++) {
        if (a[i] != b[i])
            return GL_FALSE;
    }
    return GL_TRUE;
}

static GLboolean unit_declares_uniform(const glsl_unit_t *u, const char *name,
                                       size_t len) {
    if (!u || u->root == GLSL_NO_NODE)
        return GL_FALSE;
    for (int32_t d = u->ast.nodes[u->root].a; d != GLSL_NO_NODE;
         d = u->ast.nodes[d].sibling) {
        const glsl_node_t *n = &u->ast.nodes[d];
        if (n->kind == GLSL_NODE_DECL && n->qualifier == GLSL_TOK_KW_UNIFORM) {
            if (name_matches(n->text, (size_t)n->length, name, len))
                return GL_TRUE;
        }
    }
    return GL_FALSE;
}

static GLboolean unit_declares_attribute(const glsl_unit_t *u, const char *name,
                                         size_t len) {
    if (!u || u->root == GLSL_NO_NODE)
        return GL_FALSE;
    for (int32_t d = u->ast.nodes[u->root].a; d != GLSL_NO_NODE;
         d = u->ast.nodes[d].sibling) {
        const glsl_node_t *n = &u->ast.nodes[d];
        if (n->kind == GLSL_NODE_DECL && n->qualifier == GLSL_TOK_KW_ATTRIBUTE) {
            if (name_matches(n->text, (size_t)n->length, name, len))
                return GL_TRUE;
        }
    }
    return GL_FALSE;
}

static int32_t find_main(const glsl_unit_t *u) {
    if (!u || u->root == GLSL_NO_NODE)
        return GLSL_NO_NODE;
    for (int32_t d = u->ast.nodes[u->root].a; d != GLSL_NO_NODE;
         d = u->ast.nodes[d].sibling) {
        const glsl_node_t *n = &u->ast.nodes[d];
        if (n->kind == GLSL_NODE_FUNCTION && n->length == 4 && n->text[0] == 'm' &&
            n->text[1] == 'a' && n->text[2] == 'i' && n->text[3] == 'n')
            return d;
    }
    return GLSL_NO_NODE;
}

static void vs_emit_ngg_preamble(glsl_code_t *code) {
    /* NGG primitive allocation and export sequence (D005, D014). */
    glsl_code_put(code, 0xbfa00001u); /* s_inst_prefetch 0x1 */
    glsl_code_put(code, 0xbe8c037eu); /* s_mov_b32 s12, exec_lo */
    glsl_code_put(code, 0xbefc03ffu); /* s_mov_b32 m0, 0x1003 */
    glsl_code_put(code, 0x00001003u);
    glsl_code_put(code, 0xbf800000u); /* s_nop 0 */
    glsl_code_put(code, 0xbf900009u); /* s_sendmsg sendmsg(MSG_GS_ALLOC_REQ) */
    glsl_code_put(code, 0xbefe0381u); /* s_mov_b32 exec_lo, 1 */
    glsl_code_put(code, 0x7e0202ffu); /* v_mov_b32 v1, 0x20280600 */
    glsl_code_put(code, 0x20280600u);
    glsl_code_put(code, 0xf8000941u); /* exp prim v1, off, off, off done */
    glsl_code_put(code, 0x00000001u);
    glsl_code_put(code, 0xbf8cff0fu); /* s_waitcnt expcnt(0) */
    glsl_code_put(code, 0xbefe0387u); /* s_mov_b32 exec_lo, 7 */
    glsl_code_put(code, 0xd765000eu); /* v_mbcnt_lo_u32_b32 v14, -1, 0 */
    glsl_code_put(code, 0x000100c1u);

    /* Ordered wave ID from s2 (gs_tg_info): v14 = (ordered_wave_id * 3) + lane */
    glsl_code_put(code, 0x8704ff02u); /* s_and_b32 s4, s2, 0xfff */
    glsl_code_put(code, 0x00000fffu);
    glsl_code_put(code, 0x8f058104u); /* s_lshl_b32 s5, s4, 1 */
    glsl_code_put(code, 0x80040504u); /* s_add_u32 s4, s4, s5 */
    glsl_code_put(code, 0x4a1c1c04u); /* v_add_nc_u32 v14, s4, v14 */

    glsl_code_put(code, 0x7e020280u); /* v_mov_b32 v1, 0 */
}

static GLboolean vs_load_uniforms(const gl_program_object_t *p, const glsl_unit_t *vs,
                                  glsl_gen_t *gen, glsl_code_t *code, char *log,
                                  size_t log_size) {
    GLboolean ok = GL_TRUE;
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
        if (!unit_declares_uniform(vs, u->name, ulen))
            continue;
        if (gl_type_is_sampler(u->type))
            continue;
        const glsl_type_t t = type_from_gl(u->type);
        if (t == GLSL_TYPE_ERROR) {
            oops_snprintf(log, log_size,
                          "uniform '%s' is not a supported type on the vertex path",
                          u->name);
            ok = GL_FALSE;
            break;
        }
        const int size = (u->size > 0) ? u->size : 1;
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
        resident[resident_count].floats = u->floats * size;
        resident[resident_count].home = home.base;
        resident_count++;
        if (u->offset < pool_lo)
            pool_lo = u->offset;
        if (u->offset + u->floats * size > pool_hi)
            pool_hi = u->offset + u->floats * size;
    }

    /* Load from uniform block in s[8:9] (sbase_pair = 4) */
    for (int base = pool_lo & ~15; ok && base < pool_hi;
         base += GL_VS_UNIFORM_WINDOW_FLOATS) {
        for (int off = 0; off < GL_VS_UNIFORM_WINDOW_FLOATS; off += 16) {
            if (base + off >= OOPS_GL_GL2_UNIFORM_FLOATS)
                break;
            glsl_emit_s_load(code, GLSL_SMEM_LOAD_DWORDX16,
                             GL_VS_UNIFORM_SGPR_BASE + (uint32_t)off,
                             GL_VS_UNIFORM_BLOCK_SBASE,
                             OOPS_GL_GL2_UNIFORM_AT + (uint32_t)(base + off) * 4u);
        }
        glsl_emit_s_waitcnt_lgkm(code);

        for (int r = 0; r < resident_count; r++) {
            for (int c = 0; c < resident[r].floats; c++) {
                const int at = resident[r].at + c;
                if (at < base || at >= base + GL_VS_UNIFORM_WINDOW_FLOATS)
                    continue;
                glsl_emit_vop1(
                    code, GLSL_VOP1_MOV_B32, resident[r].home + (uint32_t)c,
                    glsl_sgpr(GL_VS_UNIFORM_SGPR_BASE + (uint32_t)(at - base)));
            }
        }
    }
    return ok;
}

static GLboolean vs_load_attributes(const gl_program_object_t *p, const glsl_unit_t *vs,
                                    glsl_gen_t *gen, glsl_code_t *code, char *log,
                                    size_t log_size) {
    GLboolean ok = GL_TRUE;
    int loaded = 0;
    for (int i = 0; ok && i < p->attrib_count; i++) {
        const gl_attrib_binding_t *attr = &p->attribs[i];
        const size_t alen = lit_len(attr->name);
        if (!unit_declares_attribute(vs, attr->name, alen))
            continue;
        const glsl_type_t t = type_from_gl(attr->type);
        if (t == GLSL_TYPE_ERROR) {
            oops_snprintf(log, log_size, "attribute '%s' has an unsupported type",
                          attr->name);
            ok = GL_FALSE;
            break;
        }
        const glsl_value_t home = glsl_gen_declare_input(gen, attr->name, alen, t);
        if (home.count == 0) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "an attribute has no register", 0,
                           0);
            ok = GL_FALSE;
            break;
        }
        const int loc = attr->location;
        if (loc < 0 || loc >= OOPS_GL_MAX_VERTEX_ATTRIBS)
            continue;

        /* s_load_dwordx4 s[4:7], s[10:11], loc * 16 (sbase_pair = 5) */
        glsl_emit_s_load(code, GLSL_SMEM_LOAD_DWORDX4, 4u, 5u, (uint32_t)(loc * 16));
        glsl_emit_s_waitcnt_lgkm(code);

        /* v_mul_lo_u32 v4, s6, v14 (v4 = stride * idx) */
        glsl_code_put(code, 0xd5690004u);
        glsl_code_put(code, 0x00021c06u);

        /* v_add_co_u32 v2, vcc_lo, s4, v4 */
        glsl_code_put(code, 0xd70f6a02u);
        glsl_code_put(code, 0x00020804u);

        /* v_add_co_ci_u32_e32 v3, vcc_lo, s5, v1, vcc_lo (v1 = 0) */
        glsl_code_put(code, 0x50060205u);

        if (home.count == 4) {
            /* If component count in buffer is 4, load vec4; else set w=1.0 and load
             * vec3 */
            glsl_code_put(code, 0xbf068407u); /* s_cmp_eq_u32 s7, 4 */
            glsl_code_put(code, 0xbf850004u); /* s_cbranch_scc1 4 */
            glsl_code_put(code, 0x7e0002f2u | (((home.base + 3u) & 0xffu)
                                               << 8u)); /* v_mov_b32 v[w], 1.0 */
            glsl_emit_global_load_dwordx3(code, home.base, 2u, 0u);
            glsl_code_put(code, 0xbf820002u); /* s_branch 2 */
            glsl_emit_global_load_dwordx4(code, home.base, 2u, 0u);
        } else if (home.count == 3) {
            glsl_emit_global_load_dwordx3(code, home.base, 2u, 0u);
        } else if (home.count == 2) {
            glsl_emit_global_load_dwordx2(code, home.base, 2u, 0u);
        } else if (home.count == 1) {
            glsl_emit_global_load_dword(code, home.base, 2u, 0u);
        }
        loaded++;
    }
    if (loaded > 0) {
        glsl_code_put(code, 0xbf8c3f70u); /* s_waitcnt vmcnt(0) */
    }
    return ok;
}

static GLboolean vs_declare_outputs(const gl_program_object_t *p, const glsl_unit_t *vs,
                                    glsl_gen_t *gen, glsl_code_t *code, char *log,
                                    size_t log_size) {
    GLboolean ok = GL_TRUE;
    /* gl_Position (vec4) initialized to (0, 0, 0, 1.0) */
    glsl_value_t pos = glsl_gen_declare_input(gen, "gl_Position", 11u, GLSL_TYPE_VEC4);
    if (pos.count != 4) {
        glsl_log_write(log, log_size, "gl_Position has no register", 0, 0);
        return GL_FALSE;
    }
    glsl_emit_mov_imm(code, pos.base + 0u, 0u);
    glsl_emit_mov_imm(code, pos.base + 1u, 0u);
    glsl_emit_mov_imm(code, pos.base + 2u, 0u);
    glsl_emit_mov_imm(code, pos.base + 3u, 0x3f800000u /* 1.0f */);

    /* gl_FrontColor if mentioned */
    if (glsl_unit_mentions(vs, "gl_FrontColor", 13u)) {
        glsl_value_t col =
            glsl_gen_declare_input(gen, "gl_FrontColor", 13u, GLSL_TYPE_VEC4);
        if (col.count != 4) {
            glsl_log_write(log, log_size, "gl_FrontColor has no register", 0, 0);
            return GL_FALSE;
        }
        glsl_emit_mov_imm(code, col.base + 0u, 0x3f800000u);
        glsl_emit_mov_imm(code, col.base + 1u, 0x3f800000u);
        glsl_emit_mov_imm(code, col.base + 2u, 0x3f800000u);
        glsl_emit_mov_imm(code, col.base + 3u, 0x3f800000u);
    }

    /* User varyings */
    for (int i = 0; ok && i < p->varying_count; i++) {
        const gl_varying_t *v = &p->varyings[i];
        const size_t vlen = lit_len(v->name);
        const glsl_type_t t = type_from_gl(v->type);
        if (t == GLSL_TYPE_ERROR) {
            oops_snprintf(log, log_size, "varying '%s' has an unsupported type",
                          v->name);
            ok = GL_FALSE;
            break;
        }
        const glsl_value_t home = glsl_gen_declare_input(gen, v->name, vlen, t);
        if (home.count == 0) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error : "a varying has no register", 0, 0);
            ok = GL_FALSE;
            break;
        }
    }
    return ok;
}

static GLboolean vs_declare_globals(const glsl_unit_t *vs, glsl_gen_t *gen, char *log,
                                    size_t log_size) {
    if (vs->root == GLSL_NO_NODE)
        return GL_TRUE;
    for (int32_t d = vs->ast.nodes[vs->root].a; d != GLSL_NO_NODE;
         d = vs->ast.nodes[d].sibling) {
        const glsl_node_t *gn = &vs->ast.nodes[d];
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
            return GL_FALSE;
        }
    }
    return GL_TRUE;
}

static GLboolean vs_gen_main(const glsl_unit_t *vs, glsl_gen_t *gen, char *log,
                             size_t log_size) {
    const int32_t main_fn = find_main(vs);
    if (main_fn == GLSL_NO_NODE) {
        glsl_log_write(log, log_size, "the vertex shader has no main", 0, 0);
        return GL_FALSE;
    }
    const int32_t body = vs->ast.nodes[main_fn].c;
    for (int32_t st = vs->ast.nodes[body].a; st != GLSL_NO_NODE;
         st = vs->ast.nodes[st].sibling) {
        if (!glsl_gen_stmt(gen, st)) {
            glsl_log_write(log, log_size,
                           gen->error ? gen->error
                                      : "this shader has no instruction selection",
                           gen->error_line, gen->error_column);
            return GL_FALSE;
        }
    }
    return GL_TRUE;
}

static GLboolean vs_emit_epilogue(const gl_program_object_t *p, const glsl_unit_t *vs,
                                  glsl_gen_t *gen, glsl_code_t *code, char *log,
                                  size_t log_size) {
    (void)vs;
    uint32_t param_regs[GL_VS_MAX_PARAM_FLOATS];
    for (uint32_t i = 0; i < (uint32_t)GL_VS_MAX_PARAM_FLOATS; i++)
        param_regs[i] = GL_VS_EXPORT_POS_BASE;

    /* Map user varyings to parameters */
    for (int i = 0; i < p->varying_count; i++) {
        const gl_varying_t *v = &p->varyings[i];
        glsl_value_t val;
        if (!glsl_gen_lookup(gen, v->name, lit_len(v->name), &val))
            continue;
        for (int c = 0; c < val.count; c++) {
            const int slot = v->offset + c;
            if (slot >= 0 && slot < GL_VS_MAX_PARAM_FLOATS) {
                param_regs[slot] = val.base + (uint32_t)c;
            }
        }
    }

    /* gl_FrontColor -> hw_color_param */
    if (p->hw_color_param >= 0) {
        glsl_value_t col;
        if (glsl_gen_lookup(gen, "gl_FrontColor", 13u, &col) && col.count == 4) {
            for (int c = 0; c < 4; c++) {
                const int slot = p->hw_color_param * 4 + c;
                if (slot >= 0 && slot < GL_VS_MAX_PARAM_FLOATS) {
                    param_regs[slot] = col.base + (uint32_t)c;
                }
            }
        }
    }

    /* Emit SPI parameter exports */
    for (uint32_t k = 0; k < p->hw_params; k++) {
        const uint32_t base = k * 4u;
        glsl_emit_export_param(code, k, param_regs[base + 0], param_regs[base + 1],
                               param_regs[base + 2], param_regs[base + 3]);
    }

    /* Export gl_Position to POS0 with done=GL_TRUE */
    glsl_value_t pos;
    if (!glsl_gen_lookup(gen, "gl_Position", 11u, &pos) || pos.count != 4) {
        glsl_log_write(log, log_size, "gl_Position did not survive to export", 0, 0);
        return GL_FALSE;
    }
    glsl_emit_export_pos(code, pos.base, pos.base + 1u, pos.base + 2u, pos.base + 3u,
                         GL_TRUE);

    glsl_emit_s_waitcnt_exp(code);
    glsl_code_put(code, 0xbefe030cu); /* s_mov_b32 exec_lo, s12 */
    glsl_emit_endpgm(code);

    return GL_TRUE;
}

GLboolean gl_program_compile_vertex(const gl_program_object_t *p, uint32_t *words,
                                    uint32_t capacity, uint32_t *out_count,
                                    uint32_t *out_vgprs, uint32_t *out_user_sgprs,
                                    char *log, size_t log_size) {
    if (log && log_size)
        log[0] = '\0';
    if (out_count)
        *out_count = 0u;
    if (out_vgprs)
        *out_vgprs = 0u;
    if (out_user_sgprs)
        *out_user_sgprs = 0u;

    if (!p || !p->linked || !words) {
        glsl_log_write(log, log_size, "no linked vertex stage to compile", 0, 0);
        return GL_FALSE;
    }
    const glsl_unit_t *vs = p->vs;
    if (!vs) {
        return GL_TRUE;
    }
    if (p->varying_floats > GL_VS_MAX_PARAM_FLOATS) {
        oops_snprintf(
            log, log_size,
            "this program exports %d varying floats and the pipeline exports %d",
            p->varying_floats, GL_VS_MAX_PARAM_FLOATS);
        return GL_FALSE;
    }

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

    glsl_sema_init(sema, (glsl_ast_t *)&vs->ast);
    sema->stage = GL_VERTEX_SHADER;
    sema->version = vs->version ? vs->version : 110;
    sema->struct_count = vs->struct_count;
    for (int i = 0; i < vs->struct_count; i++)
        sema->structs[i] = vs->structs[i];

    GLboolean ok = glsl_declare_builtins(sema, GL_VERTEX_SHADER);

    if (ok) {
        for (int32_t d = vs->ast.nodes[vs->root].a; d != GLSL_NO_NODE;
             d = vs->ast.nodes[d].sibling) {
            if (vs->ast.nodes[d].kind != GLSL_NODE_FUNCTION)
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

    glsl_gen_init(gen, (glsl_ast_t *)&vs->ast, sema, code);
    glsl_gen_reserve(gen, GL_VS_FIRST_FREE_VGPR);

    const uint32_t user_sgprs = 4u;

    if (ok)
        vs_emit_ngg_preamble(code);
    if (ok)
        ok = vs_load_uniforms(p, vs, gen, code, log, log_size);
    if (ok)
        ok = vs_load_attributes(p, vs, gen, code, log, log_size);
    if (ok)
        ok = vs_declare_outputs(p, vs, gen, code, log, log_size);
    if (ok)
        ok = vs_declare_globals(vs, gen, log, log_size);
    if (ok)
        ok = vs_gen_main(vs, gen, log, log_size);
    if (ok)
        ok = vs_emit_epilogue(p, vs, gen, code, log, log_size);

    if (ok && code->overflow) {
        oops_snprintf(log, log_size,
                      "this vertex shader needs more than %u instructions", capacity);
        ok = GL_FALSE;
    }

    if (ok && gen->high_water > GL_VS_MAX_VGPRS) {
        oops_snprintf(log, log_size,
                      "this vertex shader needs %u registers and the vertex stage is "
                      "allocated %u",
                      gen->high_water, GL_VS_MAX_VGPRS);
        ok = GL_FALSE;
    }

    if (ok) {
        if (out_count)
            *out_count = code->count;
        if (out_vgprs)
            *out_vgprs = gen->high_water;
        if (out_user_sgprs)
            *out_user_sgprs = user_sgprs;
    }

    gl_heap_free(sema);
    gl_heap_free(gen);
    return ok;
}
