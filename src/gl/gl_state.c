/*
 * oops-gl: State management, capability switches, and clear operations
 */

#include "gl_internal.h"

/* **An integer colour component maps across the whole signed range onto [-1, 1]**, so INT_MAX
 * means 1.0 - it is not a cast. Used by the integer spellings of the lighting calls and of
 * glTexEnv, which is why it sits up here rather than beside either of them.
 *
 * The rule and the constant are Mesa's `INT_TO_FLOAT` (`src/mesa/main/macros.h`), applied to
 * exactly the colour-valued pnames in `src/mesa/main/light.c` and
 * `src/mesa/vbo/vbo_attrib_tmp.h`. */
static float gl_int_to_colour(GLint i) {
    return (float)((2.0 * (double)i + 1.0) * (1.0 / 4294967294.0));
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_VIEWPORT, gl_la_i(x), gl_la_i(y), gl_la_i(width), gl_la_i(height))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->vp_x = x;
    ctx->vp_y = y;
    ctx->vp_w = width;
    ctx->vp_h = height;
    /* The hardware carries the viewport in registers written when a frame opens, so a change
     * after that has to be re-emitted before the next draw. */
    ctx->hw_vport_dirty = GL_TRUE;
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_SCISSOR, gl_la_i(x), gl_la_i(y), gl_la_i(width), gl_la_i(height))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->sc_x = x;
    ctx->sc_y = y;
    ctx->sc_w = width;
    ctx->sc_h = height;
    /* Same as the viewport: the box lives in registers written when a frame opens. */
    ctx->hw_scissor_dirty = GL_TRUE;
}

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_CLEAR_COLOR, gl_la_f(red), gl_la_f(green), gl_la_f(blue),
                    gl_la_f(alpha))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->clear_color[0] = (float)red;
    ctx->clear_color[1] = (float)green;
    ctx->clear_color[2] = (float)blue;
    ctx->clear_color[3] = (float)alpha;
}

void glClearDepth(GLclampd depth) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_CLEAR_DEPTH, gl_la_f((GLfloat)depth))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->clear_depth = (float)depth;
}

/* A clear of colour or depth that has to keep to a scissor box or go through a colour mask,
 * done as GL defines a clear: a rectangle of fragments at the clear values, with only the scissor
 * test and the write masks applied. So it is drawn - one quad over the whole window, at the clear
 * colour and the clear depth, through the ordinary pipeline - with every other per-fragment
 * effect set aside for it and put back after it: depth test ALWAYS (and off, with depth writes
 * off, when depth is not being cleared), no blending, logic op, alpha test, texture, lighting,
 * fog, stencil test, culling, offset, stipple or clip planes, filled polygons, identity
 * matrices, the whole window as the viewport and the full depth range.
 *
 * Why draw rather than fill: the hardware clears with a DMA fill of the whole allocation, which
 * cannot keep to a rectangle of a tiled surface or leave a channel alone. A draw can, the
 * scissor registers and CB_TARGET_MASK already do both, and the same draw on the host goes
 * through the software rasteriser's scissor and mask - so the two paths clear identically. An
 * unscissored, unmasked clear still takes the fill, which leaves every frame that clears the
 * whole surface - gl-cube's recorded one included - exactly as it was. */
static void gl_clear_by_draw(gl_context_t *ctx, GLboolean colour, GLboolean depth,
                             GLboolean sten) {
    gl_mat4_t *mv = &ctx->modelview_stack[ctx->modelview_depth];
    gl_mat4_t *pr = &ctx->projection_stack[ctx->projection_depth];
    const gl_mat4_t saved_mv = *mv, saved_pr = *pr;
    const GLint vx = ctx->vp_x, vy = ctx->vp_y;
    const GLsizei vw = ctx->vp_w, vh = ctx->vp_h;
    const float dn = ctx->depth_near, df = ctx->depth_far;
    float cc[4];
    GLboolean cm[4], clip[OOPS_GL_CLIP_PLANE_COUNT];
    for (int i = 0; i < 4; i++) { cc[i] = ctx->cur_color[i]; cm[i] = ctx->color_mask[i]; }
    GLboolean any_clip = GL_FALSE;
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        clip[i] = ctx->clip_plane_enabled[i];
        if (clip[i]) any_clip = GL_TRUE;
    }
    const GLboolean depth_test = ctx->cap_depth_test, depth_mask = ctx->depth_mask;
    const GLenum depth_func = ctx->depth_func;
    const GLboolean blend = ctx->cap_blend, logic = ctx->cap_color_logic_op;
    const GLboolean alpha = ctx->cap_alpha_test;
    const GLboolean light = ctx->cap_lighting, fog = ctx->cap_fog;
    /* Every texture target on every unit. Only 1D and 2D were set aside until 2026-09-19, so an
     * enabled 3D texture or cube map textured the drawn clear. */
    GLboolean tex_caps[OOPS_GL_MAX_TEXTURE_UNITS][4];
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        gl_tex_unit_t *tu = &ctx->tex_unit[u];
        tex_caps[u][0] = tu->cap_texture_1d; tex_caps[u][1] = tu->cap_texture_2d;
        tex_caps[u][2] = tu->cap_texture_3d; tex_caps[u][3] = tu->cap_texture_cube_map;
    }
    const GLboolean stencil = ctx->cap_stencil_test, cull = ctx->cap_cull_face;
    const GLboolean offset = ctx->cap_polygon_offset_fill, stipple = ctx->cap_polygon_stipple;
    const GLenum pm0 = ctx->polygon_mode[0], pm1 = ctx->polygon_mode[1];
    /* The stencil state the stencil pass borrows. */
    const GLenum sf = ctx->stencil_func, s_fail = ctx->stencil_fail;
    const GLenum s_zfail = ctx->stencil_zfail, s_zpass = ctx->stencil_zpass;
    const GLint sref = ctx->stencil_ref;
    const GLuint svm = ctx->stencil_value_mask;

    mat4_identity(mv);
    mat4_identity(pr);
    ctx->mvp_dirty = GL_TRUE;
    ctx->vp_x = 0; ctx->vp_y = 0;
    ctx->vp_w = (GLsizei)ctx->width; ctx->vp_h = (GLsizei)ctx->height;
    ctx->hw_vport_dirty = GL_TRUE;
    ctx->depth_near = 0.0f; ctx->depth_far = 1.0f;
    ctx->hw_depth_range_dirty = GL_TRUE;
    for (int i = 0; i < 4; i++) {
        ctx->cur_color[i] = ctx->clear_color[i];
        if (!colour) ctx->color_mask[i] = GL_FALSE;
    }
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) ctx->clip_plane_enabled[i] = GL_FALSE;
    if (any_clip) ctx->hw_clip_dirty = GL_TRUE;
    ctx->cap_depth_test = depth;
    ctx->depth_func = GL_ALWAYS;
    ctx->depth_mask = depth;
    ctx->cap_blend = GL_FALSE;
    ctx->cap_color_logic_op = GL_FALSE;
    if (logic) ctx->hw_color_control_dirty = GL_TRUE;
    ctx->cap_alpha_test = GL_FALSE;
    if (alpha) gl_ps_patch_alpha_test(ctx);
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        gl_tex_unit_t *tu = &ctx->tex_unit[u];
        tu->cap_texture_1d = tu->cap_texture_2d = GL_FALSE;
        tu->cap_texture_3d = tu->cap_texture_cube_map = GL_FALSE;
    }
    ctx->cap_lighting = GL_FALSE;
    ctx->cap_fog = GL_FALSE;
    /* **A stencil clear the console cannot fill** (boxed or masked; since 2026-09-19): the stencil
     * test on, passing always, every surviving fragment's stencil REPLACEd by the clear value -
     * through the stencil write mask, which is left as it is because GL applies it to a clear. */
    ctx->cap_stencil_test = sten;
    if (sten) {
        ctx->stencil_func = GL_ALWAYS;
        ctx->stencil_ref = ctx->clear_stencil;
        ctx->stencil_value_mask = 0xffffffffu;
        ctx->stencil_fail = GL_KEEP;
        ctx->stencil_zfail = GL_REPLACE;
        ctx->stencil_zpass = GL_REPLACE;
    }
    ctx->cap_cull_face = GL_FALSE;
    ctx->cap_polygon_offset_fill = GL_FALSE;
    ctx->cap_polygon_stipple = GL_FALSE;
    ctx->polygon_mode[0] = GL_FILL;
    ctx->polygon_mode[1] = GL_FILL;

    /* Window z = NDC z / 2 + 1/2 under the full depth range, so NDC z = 2 * depth - 1. */
    const float z = 2.0f * ctx->clear_depth - 1.0f;
    glBegin(GL_QUADS);
    glVertex4f(-1.0f, -1.0f, z, 1.0f);
    glVertex4f(1.0f, -1.0f, z, 1.0f);
    glVertex4f(1.0f, 1.0f, z, 1.0f);
    glVertex4f(-1.0f, 1.0f, z, 1.0f);
    glEnd();

    *mv = saved_mv;
    *pr = saved_pr;
    ctx->mvp_dirty = GL_TRUE;
    ctx->vp_x = vx; ctx->vp_y = vy; ctx->vp_w = vw; ctx->vp_h = vh;
    ctx->hw_vport_dirty = GL_TRUE;
    ctx->depth_near = dn; ctx->depth_far = df;
    ctx->hw_depth_range_dirty = GL_TRUE;
    for (int i = 0; i < 4; i++) { ctx->cur_color[i] = cc[i]; ctx->color_mask[i] = cm[i]; }
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) ctx->clip_plane_enabled[i] = clip[i];
    if (any_clip) ctx->hw_clip_dirty = GL_TRUE;
    ctx->cap_depth_test = depth_test;
    ctx->depth_func = depth_func;
    ctx->depth_mask = depth_mask;
    ctx->cap_blend = blend;
    ctx->cap_color_logic_op = logic;
    if (logic) ctx->hw_color_control_dirty = GL_TRUE;
    ctx->cap_alpha_test = alpha;
    if (alpha) gl_ps_patch_alpha_test(ctx);
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        gl_tex_unit_t *tu = &ctx->tex_unit[u];
        tu->cap_texture_1d = tex_caps[u][0]; tu->cap_texture_2d = tex_caps[u][1];
        tu->cap_texture_3d = tex_caps[u][2]; tu->cap_texture_cube_map = tex_caps[u][3];
    }
    ctx->cap_lighting = light;
    ctx->cap_fog = fog;
    ctx->cap_stencil_test = stencil;
    ctx->stencil_func = sf;
    ctx->stencil_ref = sref;
    ctx->stencil_value_mask = svm;
    ctx->stencil_fail = s_fail;
    ctx->stencil_zfail = s_zfail;
    ctx->stencil_zpass = s_zpass;
    ctx->cap_cull_face = cull;
    ctx->cap_polygon_offset_fill = offset;
    ctx->cap_polygon_stipple = stipple;
    ctx->polygon_mode[0] = pm0;
    ctx->polygon_mode[1] = pm1;
}

void glClear(GLbitfield mask) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_CLEAR, gl_la_u(mask))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* A bit that names no buffer is GL_INVALID_VALUE, and nothing is cleared. These were
     * ignored until 2026-09-19. */
    if ((mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT |
                              GL_ACCUM_BUFFER_BIT)) != 0u) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* In GL_SELECT and GL_FEEDBACK nothing reaches the framebuffer, a clear included (Mesa
     * main/clear.c:185). A pick pass that begins with the program's usual glClear would
     * otherwise wipe the frame it is picking in. */
    if (ctx->render_mode != GL_RENDER) return;
    if (ctx->imm_active) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* The accumulation buffer lives on the CPU on both paths, so it is cleared here, ahead of the
     * hardware path's return. It keeps to the scissor box, as Mesa's does. */
    if (mask & GL_ACCUM_BUFFER_BIT) gl_accum_clear(ctx);

    /* **A clear keeps to the scissor box and goes through the write masks** - GL's rule, which
     * this ignored on both paths until 2026-09-19: every clear filled the whole surface, so a
     * program clearing one viewport of a split screen wiped the others, and one clearing colour
     * with a channel masked lost that channel too. The depth mask drops the depth clear
     * outright (Mesa main/clear.c: "don't clear depth buffer if depth writing disabled"). */
    if (!ctx->depth_mask) mask &= ~(GLbitfield)GL_DEPTH_BUFFER_BIT;
    int x0 = 0, y0 = 0, x1 = (int)ctx->width, y1 = (int)ctx->height;
    if (ctx->cap_scissor_test) {
        if (ctx->sc_x > x0) x0 = ctx->sc_x;
        if (ctx->sc_y > y0) y0 = ctx->sc_y;
        if (ctx->sc_x + ctx->sc_w < x1) x1 = ctx->sc_x + ctx->sc_w;
        if (ctx->sc_y + ctx->sc_h < y1) y1 = ctx->sc_y + ctx->sc_h;
    }
    if (x0 >= x1 || y0 >= y1) return; /* a scissor box outside the surface clears nothing */
    const GLboolean partial = (GLboolean)(x0 > 0 || y0 > 0 || x1 < (int)ctx->width ||
                                          y1 < (int)ctx->height);

    /* The stencil buffer, cleared inside the box and through the write mask. This ignored the
     * mask on the claim that GL does, and it does not - the specification applies every buffer's
     * write mask to a clear. On the console the stencil buffer is the GPU's surface, tiled, since
     * 2026-09-19: a whole clear is a fill in the command stream (a constant is the same in any
     * layout), and a boxed or masked one is drawn. */
    if ((mask & GL_STENCIL_BUFFER_BIT) && ctx->stencil_buffer) {
        const uint8_t wm = (uint8_t)(ctx->stencil_writemask & 0xffu);
        const uint8_t v = (uint8_t)(ctx->clear_stencil & 0xff);
#ifndef OOPS_HOST_BUILD
        if (ctx->use_hardware) {
            if (!partial && wm == 0xffu) {
                gl_hw_clear(ctx, GL_STENCIL_BUFFER_BIT, 0u, 0.0f);
            } else if (wm != 0u) {
                gl_clear_by_draw(ctx, GL_FALSE, GL_FALSE, GL_TRUE);
            }
        } else
#endif
        if (!partial && wm == 0xffu) {
            const size_t n = ctx->stencil_px ? ctx->stencil_px
                                             : (size_t)ctx->width * (size_t)ctx->height;
            memset(ctx->stencil_buffer, v, n);
        } else if (wm != 0u) {
            for (int y = y0; y < y1; y++) {
                uint8_t *row = ctx->stencil_buffer + (size_t)((int)ctx->height - 1 - y) * ctx->width;
                for (int x = x0; x < x1; x++) row[x] = (uint8_t)((row[x] & ~wm) | (v & wm));
            }
        }
    }
    mask &= (GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    /* glDrawBuffer(GL_NONE): there is no colour buffer to clear. */
    if (ctx->draw_buffer == GL_NONE) mask &= ~(GLbitfield)GL_COLOR_BUFFER_BIT;
    if (mask == 0u) return;

    /* Colour and depth through a box or a mask are cleared by drawing: see gl_clear_by_draw. */
    const GLboolean colour_masked =
        (GLboolean)((mask & GL_COLOR_BUFFER_BIT) &&
                    !(ctx->color_mask[0] && ctx->color_mask[1] && ctx->color_mask[2] &&
                      ctx->color_mask[3]));
    if (partial || colour_masked) {
        gl_clear_by_draw(ctx, (GLboolean)((mask & GL_COLOR_BUFFER_BIT) != 0u),
                         (GLboolean)((mask & GL_DEPTH_BUFFER_BIT) != 0u), GL_FALSE);
        if (mask & GL_COLOR_BUFFER_BIT) ctx->fb_cleared = GL_TRUE;
        return;
    }

    size_t total_px = (size_t)ctx->width * (size_t)ctx->height;

    uint32_t ir = (uint32_t)(ctx->clear_color[0] * 255.0f + 0.5f);
    uint32_t ig = (uint32_t)(ctx->clear_color[1] * 255.0f + 0.5f);
    uint32_t ib = (uint32_t)(ctx->clear_color[2] * 255.0f + 0.5f);
    uint32_t ia = (uint32_t)(ctx->clear_color[3] * 255.0f + 0.5f);
    if (ir > 255) ir = 255;
    if (ig > 255) ig = 255;
    if (ib > 255) ib = 255;
    if (ia > 255) ia = 255;
    uint32_t col = (ia << 24) | (ir << 16) | (ig << 8) | ib;

#ifndef OOPS_HOST_BUILD
    if (ctx->use_hardware) {
        /* The GPU clears its own targets; the CPU never writes them on this path. */
        gl_hw_clear(ctx, mask, col, ctx->clear_depth);
        if (mask & GL_COLOR_BUFFER_BIT) ctx->fb_cleared = GL_TRUE;
        return;
    }
#endif

    if (mask & GL_COLOR_BUFFER_BIT) {
        /* Every word of the buffer, a tiled one's padding included: a constant is the same in
         * any layout. */
        const size_t words = gl_color_words(ctx);
        uint32_t *fb = ctx->framebuffer;
        if (fb) {
            for (size_t i = 0; i < words; i++) {
                fb[i] = col;
            }
            ctx->fb_cleared = GL_TRUE;
        }
        /* And the other buffer glDrawBuffer(GL_FRONT_AND_BACK) names. */
        if (ctx->fb_also) {
            for (size_t i = 0; i < words; i++) ctx->fb_also[i] = col;
        }
    }

    if (mask & GL_DEPTH_BUFFER_BIT) {
        float *db = ctx->depth_buffer;
        float cd = ctx->clear_depth;
        if (db) {
            uint32_t cd_raw;
            memcpy(&cd_raw, &cd, 4);
            uint64_t cd_raw64 = ((uint64_t)cd_raw << 32) | cd_raw;
            uint64_t *db64 = (uint64_t *)db;
            size_t depth_px = ctx->depth_px ? ctx->depth_px : total_px; /* the whole tiled extent */
            size_t total_qwords = depth_px / 2;
            for (size_t i = 0; i < total_qwords; i++) {
                db64[i] = cd_raw64;
            }
            if (depth_px & 1) {
                db[depth_px - 1] = cd;
            }
        }
    }
}

void glEnable(GLenum cap) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_ENABLE, gl_la_e(cap))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + OOPS_GL_LIGHT_COUNT) {
        ctx->lights[(size_t)(cap - GL_LIGHT0)].enabled = GL_TRUE;
        return;
    }
    GLboolean *eval_cap = gl_eval_cap(ctx, cap);
    if (eval_cap) {
        *eval_cap = GL_TRUE;
        return;
    }

    switch (cap) {
        case GL_DEPTH_TEST:     ctx->cap_depth_test = GL_TRUE; break;
        case GL_CULL_FACE:      ctx->cap_cull_face = GL_TRUE; break;
        case GL_BLEND:          ctx->cap_blend = GL_TRUE; break;
        case GL_SCISSOR_TEST:   ctx->cap_scissor_test = GL_TRUE; ctx->hw_scissor_dirty = GL_TRUE; break;
        case GL_TEXTURE_GEN_S:  gl_tu(ctx)->texgen_enabled[0] = GL_TRUE; break;
        case GL_TEXTURE_GEN_T:  gl_tu(ctx)->texgen_enabled[1] = GL_TRUE; break;
        case GL_TEXTURE_GEN_R:  gl_tu(ctx)->texgen_enabled[2] = GL_TRUE; break;
        case GL_TEXTURE_GEN_Q:  gl_tu(ctx)->texgen_enabled[3] = GL_TRUE; break;
        case GL_CLIP_PLANE0: case GL_CLIP_PLANE1: case GL_CLIP_PLANE2:
        case GL_CLIP_PLANE3: case GL_CLIP_PLANE4: case GL_CLIP_PLANE5:
            ctx->clip_plane_enabled[(int)cap - (int)GL_CLIP_PLANE0] = GL_TRUE;
            ctx->hw_clip_dirty = GL_TRUE;
            break;
        case GL_STENCIL_TEST:   ctx->cap_stencil_test = GL_TRUE; break;
        case GL_LIGHTING:       ctx->cap_lighting = GL_TRUE; break;
        case GL_TEXTURE_2D:     gl_tu(ctx)->cap_texture_2d = GL_TRUE; break;
        case GL_TEXTURE_1D:     gl_tu(ctx)->cap_texture_1d = GL_TRUE; break;
        case GL_TEXTURE_3D:     gl_tu(ctx)->cap_texture_3d = GL_TRUE; break;
        case GL_TEXTURE_CUBE_MAP: gl_tu(ctx)->cap_texture_cube_map = GL_TRUE; break;
        case GL_POINT_SMOOTH:   ctx->cap_point_smooth = GL_TRUE; break;
        case GL_POINT_SPRITE:   ctx->cap_point_sprite = GL_TRUE; break;
        case GL_LINE_SMOOTH:    ctx->cap_line_smooth = GL_TRUE; break;
        case GL_POLYGON_SMOOTH: ctx->cap_polygon_smooth = GL_TRUE; break;
        case GL_FOG:            ctx->cap_fog = GL_TRUE; break;
        case GL_COLOR_SUM:      ctx->cap_color_sum = GL_TRUE; break;
        case GL_NORMALIZE:      ctx->cap_normalize = GL_TRUE; break;
        case GL_RESCALE_NORMAL: ctx->cap_rescale_normal = GL_TRUE; break;
        case GL_COLOR_MATERIAL:
            ctx->cap_color_material = GL_TRUE;
            gl_color_material_update(ctx); /* the current colour is tracked from now */
            break;
        case GL_POLYGON_OFFSET_FILL: ctx->cap_polygon_offset_fill = GL_TRUE; break;
        case GL_POLYGON_OFFSET_LINE: ctx->cap_polygon_offset_line = GL_TRUE; break;
        case GL_POLYGON_OFFSET_POINT: ctx->cap_polygon_offset_point = GL_TRUE; break;
        case GL_LINE_STIPPLE:         ctx->cap_line_stipple = GL_TRUE; break;
        case GL_POLYGON_STIPPLE:      ctx->cap_polygon_stipple = GL_TRUE; break;
        /* State that changes no pixel here, each for the reason given where it is declared. */
        case GL_DITHER:               ctx->cap_dither = GL_TRUE; break;
        case GL_INDEX_LOGIC_OP:       ctx->cap_index_logic_op = GL_TRUE; break;
        case GL_MULTISAMPLE:          ctx->cap_multisample = GL_TRUE; break;
        case GL_SAMPLE_ALPHA_TO_COVERAGE: ctx->cap_sample_alpha_to_coverage = GL_TRUE; break;
        case GL_SAMPLE_ALPHA_TO_ONE:  ctx->cap_sample_alpha_to_one = GL_TRUE; break;
        case GL_SAMPLE_COVERAGE:      ctx->cap_sample_coverage = GL_TRUE; break;
        case GL_COLOR_LOGIC_OP:
            ctx->cap_color_logic_op = GL_TRUE;
            ctx->hw_color_control_dirty = GL_TRUE;
            break;
        case GL_ALPHA_TEST:
            ctx->cap_alpha_test = GL_TRUE;
            gl_ps_patch_alpha_test(ctx);
            break;
        /* **A capability this subset does not have is refused, not ignored.**
         *
         * `glEnable` is the first thing a GL program does, and a silently dropped one is the
         * most expensive kind of nothing: enable GL_STENCIL_TEST or GL_FOG here and the call
         * returned clean while the feature stayed off, so the render was wrong with no error
         * anywhere to say why. The cases above and GL_LIGHT0..7 are the whole of what exists
         * (D008); everything else says so - and glIsEnabled answers from the same list, or the
         * two disagree.
         *
         * **The refusal carries the capability**, because the name of the call is only half an
         * answer: GL_POINT_SPRITE was found here by a log that said `glEnable` and then needed
         * the port's source read to learn which enable it meant. `glEnable 0x8861` needs
         * nothing read. */
        default: gl_record_error_val(ctx, GL_INVALID_ENUM, cap); break;
    }
}

void glDisable(GLenum cap) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_DISABLE, gl_la_e(cap))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + OOPS_GL_LIGHT_COUNT) {
        ctx->lights[(size_t)(cap - GL_LIGHT0)].enabled = GL_FALSE;
        return;
    }
    GLboolean *eval_cap = gl_eval_cap(ctx, cap);
    if (eval_cap) {
        *eval_cap = GL_FALSE;
        return;
    }

    switch (cap) {
        case GL_DEPTH_TEST:     ctx->cap_depth_test = GL_FALSE; break;
        case GL_CULL_FACE:      ctx->cap_cull_face = GL_FALSE; break;
        case GL_BLEND:          ctx->cap_blend = GL_FALSE; break;
        case GL_SCISSOR_TEST:   ctx->cap_scissor_test = GL_FALSE; ctx->hw_scissor_dirty = GL_TRUE; break;
        case GL_TEXTURE_GEN_S:  gl_tu(ctx)->texgen_enabled[0] = GL_FALSE; break;
        case GL_TEXTURE_GEN_T:  gl_tu(ctx)->texgen_enabled[1] = GL_FALSE; break;
        case GL_TEXTURE_GEN_R:  gl_tu(ctx)->texgen_enabled[2] = GL_FALSE; break;
        case GL_TEXTURE_GEN_Q:  gl_tu(ctx)->texgen_enabled[3] = GL_FALSE; break;
        case GL_CLIP_PLANE0: case GL_CLIP_PLANE1: case GL_CLIP_PLANE2:
        case GL_CLIP_PLANE3: case GL_CLIP_PLANE4: case GL_CLIP_PLANE5:
            ctx->clip_plane_enabled[(int)cap - (int)GL_CLIP_PLANE0] = GL_FALSE;
            ctx->hw_clip_dirty = GL_TRUE;
            break;
        case GL_STENCIL_TEST:   ctx->cap_stencil_test = GL_FALSE; break;
        case GL_LIGHTING:       ctx->cap_lighting = GL_FALSE; break;
        case GL_TEXTURE_2D:     gl_tu(ctx)->cap_texture_2d = GL_FALSE; break;
        case GL_TEXTURE_1D:     gl_tu(ctx)->cap_texture_1d = GL_FALSE; break;
        case GL_TEXTURE_3D:     gl_tu(ctx)->cap_texture_3d = GL_FALSE; break;
        case GL_TEXTURE_CUBE_MAP: gl_tu(ctx)->cap_texture_cube_map = GL_FALSE; break;
        case GL_POINT_SMOOTH:   ctx->cap_point_smooth = GL_FALSE; break;
        case GL_POINT_SPRITE:   ctx->cap_point_sprite = GL_FALSE; break;
        case GL_LINE_SMOOTH:    ctx->cap_line_smooth = GL_FALSE; break;
        case GL_POLYGON_SMOOTH: ctx->cap_polygon_smooth = GL_FALSE; break;
        case GL_FOG:            ctx->cap_fog = GL_FALSE; break;
        case GL_COLOR_SUM:      ctx->cap_color_sum = GL_FALSE; break;
        case GL_NORMALIZE:      ctx->cap_normalize = GL_FALSE; break;
        case GL_RESCALE_NORMAL: ctx->cap_rescale_normal = GL_FALSE; break;
        case GL_COLOR_MATERIAL: ctx->cap_color_material = GL_FALSE; break;
        case GL_POLYGON_OFFSET_FILL: ctx->cap_polygon_offset_fill = GL_FALSE; break;
        case GL_POLYGON_OFFSET_LINE: ctx->cap_polygon_offset_line = GL_FALSE; break;
        case GL_POLYGON_OFFSET_POINT: ctx->cap_polygon_offset_point = GL_FALSE; break;
        case GL_LINE_STIPPLE:         ctx->cap_line_stipple = GL_FALSE; break;
        case GL_POLYGON_STIPPLE:      ctx->cap_polygon_stipple = GL_FALSE; break;
        case GL_DITHER:               ctx->cap_dither = GL_FALSE; break;
        case GL_INDEX_LOGIC_OP:       ctx->cap_index_logic_op = GL_FALSE; break;
        case GL_MULTISAMPLE:          ctx->cap_multisample = GL_FALSE; break;
        case GL_SAMPLE_ALPHA_TO_COVERAGE: ctx->cap_sample_alpha_to_coverage = GL_FALSE; break;
        case GL_SAMPLE_ALPHA_TO_ONE:  ctx->cap_sample_alpha_to_one = GL_FALSE; break;
        case GL_SAMPLE_COVERAGE:      ctx->cap_sample_coverage = GL_FALSE; break;
        case GL_COLOR_LOGIC_OP:
            ctx->cap_color_logic_op = GL_FALSE;
            ctx->hw_color_control_dirty = GL_TRUE;
            break;
        case GL_ALPHA_TEST:
            ctx->cap_alpha_test = GL_FALSE;
            gl_ps_patch_alpha_test(ctx);
            break;
        default: gl_record_error_val(ctx, GL_INVALID_ENUM, cap); break;
    }
}

GLboolean glIsEnabled(GLenum cap) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GL_FALSE;

    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + OOPS_GL_LIGHT_COUNT) {
        return ctx->lights[(size_t)(cap - GL_LIGHT0)].enabled;
    }
    const GLboolean *eval_cap = gl_eval_cap(ctx, cap);
    if (eval_cap) return *eval_cap;

    switch (cap) {
        case GL_DEPTH_TEST:     return ctx->cap_depth_test;
        case GL_CULL_FACE:      return ctx->cap_cull_face;
        case GL_BLEND:          return ctx->cap_blend;
        case GL_SCISSOR_TEST:   return ctx->cap_scissor_test;
        case GL_TEXTURE_GEN_S:  return gl_tu(ctx)->texgen_enabled[0];
        case GL_TEXTURE_GEN_T:  return gl_tu(ctx)->texgen_enabled[1];
        case GL_TEXTURE_GEN_R:  return gl_tu(ctx)->texgen_enabled[2];
        case GL_TEXTURE_GEN_Q:  return gl_tu(ctx)->texgen_enabled[3];
        case GL_CLIP_PLANE0: case GL_CLIP_PLANE1: case GL_CLIP_PLANE2:
        case GL_CLIP_PLANE3: case GL_CLIP_PLANE4: case GL_CLIP_PLANE5:
            return ctx->clip_plane_enabled[(int)cap - (int)GL_CLIP_PLANE0];
        case GL_STENCIL_TEST:   return ctx->cap_stencil_test;
        case GL_LIGHTING:       return ctx->cap_lighting;
        case GL_TEXTURE_2D:     return gl_tu(ctx)->cap_texture_2d;
        case GL_TEXTURE_1D:     return gl_tu(ctx)->cap_texture_1d;
        case GL_TEXTURE_3D:     return gl_tu(ctx)->cap_texture_3d;
        case GL_TEXTURE_CUBE_MAP: return gl_tu(ctx)->cap_texture_cube_map;
        case GL_POINT_SMOOTH:   return ctx->cap_point_smooth;
        case GL_POINT_SPRITE:   return ctx->cap_point_sprite;
        case GL_LINE_SMOOTH:    return ctx->cap_line_smooth;
        case GL_POLYGON_SMOOTH: return ctx->cap_polygon_smooth;
        case GL_FOG:            return ctx->cap_fog;
        case GL_COLOR_SUM:      return ctx->cap_color_sum;
        case GL_NORMALIZE:      return ctx->cap_normalize;
        case GL_RESCALE_NORMAL: return ctx->cap_rescale_normal;
        case GL_COLOR_MATERIAL: return ctx->cap_color_material;
        /* **These two were missing**, so glIsEnabled answered GL_FALSE for a capability
         * glEnable had just switched on - and glGetBooleanv, which forwards here, repeated it.
         * The list has to be the same list glEnable accepts; anything else is a query that
         * disagrees with the state it is querying. */
        case GL_POLYGON_OFFSET_FILL: return ctx->cap_polygon_offset_fill;
        case GL_POLYGON_OFFSET_LINE: return ctx->cap_polygon_offset_line;
        case GL_POLYGON_OFFSET_POINT: return ctx->cap_polygon_offset_point;
        case GL_LINE_STIPPLE:        return ctx->cap_line_stipple;
        case GL_POLYGON_STIPPLE:     return ctx->cap_polygon_stipple;
        case GL_DITHER:              return ctx->cap_dither;
        case GL_INDEX_LOGIC_OP:      return ctx->cap_index_logic_op;
        case GL_MULTISAMPLE:         return ctx->cap_multisample;
        case GL_SAMPLE_ALPHA_TO_COVERAGE: return ctx->cap_sample_alpha_to_coverage;
        case GL_SAMPLE_ALPHA_TO_ONE: return ctx->cap_sample_alpha_to_one;
        case GL_SAMPLE_COVERAGE:     return ctx->cap_sample_coverage;
        case GL_INDEX_ARRAY:         return ctx->array_index.enabled;
        /* **The client arrays are capabilities too**, as far as glIsEnabled is concerned - the
         * specification lists them, and these were refused as unknown until 2026-09-19. */
        case GL_VERTEX_ARRAY:        return ctx->array_vertex.enabled;
        case GL_NORMAL_ARRAY:        return ctx->array_normal.enabled;
        case GL_COLOR_ARRAY:         return ctx->array_color.enabled;
        case GL_TEXTURE_COORD_ARRAY: return ctx->array_texcoord[ctx->client_active_texture].enabled;
        case GL_EDGE_FLAG_ARRAY:     return ctx->array_edge_flag.enabled;
        case GL_SECONDARY_COLOR_ARRAY: return ctx->array_secondary.enabled;
        case GL_FOG_COORD_ARRAY:     return ctx->array_fog_coord.enabled;
        case GL_ALPHA_TEST:     return ctx->cap_alpha_test;
        /* The RGBA logic op. GL_INDEX_LOGIC_OP (GL 1.0's GL_LOGIC_OP) is the colour-index one:
         * stored as state and never applied, like the rest of colour-index state here. */
        case GL_COLOR_LOGIC_OP: return ctx->cap_color_logic_op;
        /* Refused rather than answered GL_FALSE. "Not enabled" and "there is no such thing" are
         * different answers, and a program testing for a feature needs to tell them apart. */
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return GL_FALSE;
    }
}

/* The factors each side of the blend may name, as Mesa's `legal_src_factor` and
 * `legal_dst_factor` list them for desktop GL without ARB_blend_func_extended
 * (main/blend.c:49-118). The two lists differ in one entry: GL_SRC_ALPHA_SATURATE is a source
 * factor only.
 *
 * **These were not checked at all until 2026-09-19.** Any enum was stored, reported back by
 * glGetIntegerv, and then turned into BLEND_SRC_ALPHA / BLEND_ONE_MINUS_SRC_ALPHA by
 * gl_blend_op's fallback on the way to the register - so a misspelt factor blended as the GL
 * default with no error anywhere, and so did the four constant-colour factors before they
 * existed. */
static GLboolean gl_blend_factor_ok(GLenum f, GLboolean is_src) {
    switch (f) {
        case GL_ZERO: case GL_ONE:
        case GL_SRC_COLOR: case GL_ONE_MINUS_SRC_COLOR:
        case GL_DST_COLOR: case GL_ONE_MINUS_DST_COLOR:
        case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA:
        case GL_DST_ALPHA: case GL_ONE_MINUS_DST_ALPHA:
        case GL_CONSTANT_COLOR: case GL_ONE_MINUS_CONSTANT_COLOR:
        case GL_CONSTANT_ALPHA: case GL_ONE_MINUS_CONSTANT_ALPHA:
            return GL_TRUE;
        case GL_SRC_ALPHA_SATURATE:
            return is_src;
        default:
            return GL_FALSE;
    }
}

void glBlendFunc(GLenum sfactor, GLenum dfactor) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_BLEND_FUNC, gl_la_e(sfactor), gl_la_e(dfactor))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (!gl_blend_factor_ok(sfactor, GL_TRUE) || !gl_blend_factor_ok(dfactor, GL_FALSE)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->blend_src = sfactor;
    ctx->blend_dst = dfactor;
    ctx->blend_src_alpha = sfactor;
    ctx->blend_dst_alpha = dfactor;
    /*
     * **The constant has to go out again, because which value lands in `0x108` depends on these
     * factors and not only on `glBlendColor`.** A colour-constant draw puts green there and an
     * alpha-constant draw puts alpha there - see `gl_draw.c` and
     * `docs/hardware/agc-blend-and-export-fw1240.md`.
     *
     * Without this, switching families between draws reuses whatever the last emission left, and
     * the second draw blends against the first one's channel. A host test caught exactly that.
     */
    ctx->hw_blend_color_dirty = GL_TRUE;
}

void glBlendFuncSeparate(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_BLEND_FUNC_SEPARATE, gl_la_e(sfactorRGB), gl_la_e(dfactorRGB),
                    gl_la_e(sfactorAlpha), gl_la_e(dfactorAlpha))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (!gl_blend_factor_ok(sfactorRGB, GL_TRUE) || !gl_blend_factor_ok(dfactorRGB, GL_FALSE) ||
        !gl_blend_factor_ok(sfactorAlpha, GL_TRUE) || !gl_blend_factor_ok(dfactorAlpha, GL_FALSE)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->blend_src = sfactorRGB;
    ctx->blend_dst = dfactorRGB;
    ctx->blend_src_alpha = sfactorAlpha;
    ctx->blend_dst_alpha = dfactorAlpha;
    /* Same reason as `glBlendFunc` above: `0x108` carries green or alpha depending on which
       constant family these factors name. */
    ctx->hw_blend_color_dirty = GL_TRUE;
}

/* The five simple equations (Mesa main/blend.c:435-447, legal_simple_blend_equation). Anything
 * else used to be stored and then blended as GL_FUNC_ADD by gl_blend_comb's default. */
static GLboolean gl_blend_equation_ok(GLenum mode) {
    return (GLboolean)(mode == GL_FUNC_ADD || mode == GL_FUNC_SUBTRACT ||
                       mode == GL_FUNC_REVERSE_SUBTRACT || mode == GL_MIN || mode == GL_MAX);
}

/* The work, shared by the GL 1.4 call and the GL 2.0 one - `glBlendEquation` sets both groups
 * and must not reach through the 2.0 entry point, which is gated on 2.0 having been claimed. */
static void blend_equation_set(gl_context_t *ctx, GLenum modeRGB, GLenum modeAlpha) {
    if (!gl_blend_equation_ok(modeRGB) || !gl_blend_equation_ok(modeAlpha)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->blend_equation = modeRGB;
    ctx->blend_equation_alpha = modeAlpha;
}

/* **Not gated on 1.4**, although that is the version that took it into the core:
 * `GL_EXT_blend_minmax` and `GL_EXT_blend_subtract` are both advertised, and an extension is
 * available whatever the core version. See the note on `gl_require_version`. */
void glBlendEquation(GLenum mode) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_BLEND_EQUATION, gl_la_e(mode))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    blend_equation_set(ctx, mode, mode);
}

/* GL 2.0: an equation per channel group - GL_FUNC_ADD for the colour and GL_FUNC_SUBTRACT for
 * the alpha, say. `glBlendEquation` above is the both-the-same case of this, exactly as the
 * specification defines it from 2.0 onwards.
 *
 * **GL_MIN and GL_MAX ignore the blend factors**, so an equation that is one of them on one
 * group and not the other is two different treatments in one blend. The software path below and
 * the console's `CB_BLEND0_CONTROL` both already carry `ALPHA_COMB_FCN` separately from
 * `COLOR_COMB_FCN`, which is why this costs nothing beyond the second field. */
void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !gl_require_version(ctx, 2u, 0u)) return;
    blend_equation_set(ctx, modeRGB, modeAlpha);
}

void glBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_BLEND_COLOR, gl_la_f(red), gl_la_f(green), gl_la_f(blue),
                    gl_la_f(alpha))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* Clamped to [0, 1] on the way in, as Mesa does (main/blend.c:788-791). */
    const GLfloat in[4] = {red, green, blue, alpha};
    for (int i = 0; i < 4; i++) {
        GLfloat c = in[i];
        if (!(c >= 0.0f)) c = 0.0f; /* also catches NaN */
        if (c > 1.0f) c = 1.0f;
        ctx->blend_color[i] = c;
    }
    ctx->hw_blend_color_dirty = GL_TRUE;
}

/* GL_EXT_blend_color and GL_EXT_blend_minmax, whose entry points a program of that era calls.
 * GL_EXT_blend_subtract adds no entry point of its own: its two equations go through
 * glBlendEquationEXT. */
void glBlendColorEXT(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    glBlendColor(red, green, blue, alpha);
}
void glBlendEquationEXT(GLenum mode) { glBlendEquation(mode); }

void glLogicOp(GLenum opcode) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_LOGIC_OP, gl_la_e(opcode))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* The sixteen opcodes are exactly GL_CLEAR..GL_SET (Mesa main/blend.c:860-883 lists them
     * one by one, and they are the contiguous range 0x1500..0x150F). */
    if (opcode < GL_CLEAR || opcode > GL_SET) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->logic_op = opcode;
    ctx->hw_color_control_dirty = GL_TRUE;
}

/* -------------------------------------------------------------------------
 * Stencil
 * ------------------------------------------------------------------------- */
static GLboolean gl_stencil_func_ok(GLenum f) {
    return (f == GL_NEVER || f == GL_LESS || f == GL_LEQUAL || f == GL_GREATER ||
            f == GL_GEQUAL || f == GL_EQUAL || f == GL_NOTEQUAL || f == GL_ALWAYS)
               ? GL_TRUE : GL_FALSE;
}

/* GL 1.4's GL_INCR_WRAP and GL_DECR_WRAP included since 2026-09-19 (Mesa main/stencil.c:70-71),
 * each its own rule in gl_stencil_apply - wrapping, where GL_INCR and GL_DECR saturate. */
static GLboolean gl_stencil_op_ok(GLenum o) {
    return (o == GL_KEEP || o == GL_ZERO || o == GL_REPLACE || o == GL_INCR ||
            o == GL_DECR || o == GL_INVERT || o == GL_INCR_WRAP || o == GL_DECR_WRAP)
               ? GL_TRUE : GL_FALSE;
}

/* -------------------------------------------------------------------------
 * Fog
 * ------------------------------------------------------------------------- */
static void gl_fog_set(gl_context_t *ctx, GLenum pname, const GLfloat *v) {
    switch (pname) {
        case GL_FOG_MODE: {
            const GLenum mode = (GLenum)v[0];
            if (mode != GL_LINEAR && mode != GL_EXP && mode != GL_EXP2) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            ctx->fog_mode = mode;
            return;
        }
        case GL_FOG_DENSITY:
            /* Negative density is an error rather than a fog that brightens with distance. */
            if (v[0] < 0.0f) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
            ctx->fog_density = v[0];
            return;
        case GL_FOG_START: ctx->fog_start = v[0]; return;
        case GL_FOG_END:   ctx->fog_end = v[0]; return;
        case GL_FOG_COLOR:
            for (int i = 0; i < 4; i++) ctx->fog_color[i] = v[i];
            return;
        /* Colour-index fog: state in an RGBA context, kept and never drawn with, as Mesa keeps it
         * (main/fog.c:136-142). It was refused until 2026-09-19, which a conforming program
         * setting up both modes' fog saw as an error. */
        case GL_FOG_INDEX:
            ctx->fog_index = v[0];
            return;
        /* GL 1.4: where fog reads its distance from - the eye (GL_FRAGMENT_DEPTH) or the vertex's
         * fog coordinate (GL_FOG_COORD). Nothing else (main/fog.c:157-168). */
        case GL_FOG_COORD_SRC: {
            const GLenum src = (GLenum)v[0];
            if (src != GL_FOG_COORD && src != GL_FRAGMENT_DEPTH) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            ctx->fog_coord_src = src;
            return;
        }
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return;
    }
}

void glFogfv(GLenum pname, const GLfloat *params) {
    if (params && gl_list_recording() &&
        gl_list_rec_fv(GL_LIST_OP_FOG_FV, pname, 0u, GL_FALSE, pname, params)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    gl_fog_set(ctx, pname, params);
}

void glFogf(GLenum pname, GLfloat param) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_FOG_F, gl_la_e(pname), gl_la_f(param))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* Only the colour reads four; everything else reads one, so a scalar call must not be
     * turned into a four-element read of a single float. */
    if (pname == GL_FOG_COLOR) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    GLfloat v[4] = {param, 0.0f, 0.0f, 0.0f};
    gl_fog_set(ctx, pname, v);
}

void glFogi(GLenum pname, GLint param) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_FOG_I, gl_la_e(pname), gl_la_i(param))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (pname == GL_FOG_COLOR) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    GLfloat v[4] = {(GLfloat)param, 0.0f, 0.0f, 0.0f};
    gl_fog_set(ctx, pname, v);
}

/* **An integer fog colour is normalised, like every other integer colour here.** The distances
 * are plain casts; only GL_FOG_COLOR converts by range. Getting that backwards gives a fog that
 * is white whenever the caller asked for anything at all. */
void glFogiv(GLenum pname, const GLint *params) {
    if (params && gl_list_recording() &&
        gl_list_rec_iv(GL_LIST_OP_FOG_IV, pname, 0u, GL_FALSE, pname, params)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (pname == GL_FOG_COLOR) {
        GLfloat v[4];
        for (int i = 0; i < 4; i++) {
            v[i] = (GLfloat)((2.0 * (double)params[i] + 1.0) / 4294967294.0);
        }
        gl_fog_set(ctx, pname, v);
        return;
    }
    GLfloat v[4] = {(GLfloat)params[0], 0.0f, 0.0f, 0.0f};
    gl_fog_set(ctx, pname, v);
}

/* Both are screen-space widths in pixels, and both are refused at zero or below - a zero-width
 * line is not a thin line, it is a request the specification calls an error. */
void glPointSize(GLfloat size) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_POINT_SIZE, gl_la_f(size))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (size <= 0.0f) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
    ctx->point_size = size;
}

/* GL 1.4's point parameters: the size clamp, the fade threshold, and the distance attenuation -
 * as Mesa's _mesa_PointParameterfv sets and checks them (main/points.c:117-195): a negative
 * clamp or threshold is a value error, a name outside the four an enum error (GL 2.0's
 * GL_POINT_SPRITE_COORD_ORIGIN among them). The scalar forms fill the rest of the three
 * components with zero, as Mesa's do; only GL_POINT_DISTANCE_ATTENUATION reads them. Compiled
 * into lists and saved with GL_POINT_BIT. */
void glPointParameterfv(GLenum pname, const GLfloat *params) {
    if (!params) return;
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_POINT_PARAMETER, gl_la_e(pname), gl_la_f(params[0]),
                    gl_la_f(pname == GL_POINT_DISTANCE_ATTENUATION ? params[1] : 0.0f),
                    gl_la_f(pname == GL_POINT_DISTANCE_ATTENUATION ? params[2] : 0.0f))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    switch (pname) {
        case GL_POINT_DISTANCE_ATTENUATION:
            for (int i = 0; i < 3; i++) ctx->point_atten[i] = params[i];
            return;
        case GL_POINT_SIZE_MIN:
        case GL_POINT_SIZE_MAX:
        case GL_POINT_FADE_THRESHOLD_SIZE:
            if (params[0] < 0.0f) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
            if (pname == GL_POINT_SIZE_MIN) ctx->point_size_min = params[0];
            else if (pname == GL_POINT_SIZE_MAX) ctx->point_size_max = params[0];
            else ctx->point_fade_threshold = params[0];
            return;
        /* GL 2.0's fifth parameter, now that point sprites exist here to need it. Only the two
         * corners are values; anything else is an enum error, as it is for the pname itself. */
        case GL_POINT_SPRITE_COORD_ORIGIN: {
            const GLenum e = (GLenum)(GLint)params[0];
            if (e != GL_LOWER_LEFT && e != GL_UPPER_LEFT) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            ctx->point_sprite_origin = e;
            return;
        }
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return;
    }
}

void glPointParameterf(GLenum pname, GLfloat param) {
    const GLfloat p[3] = {param, 0.0f, 0.0f};
    glPointParameterfv(pname, p);
}

void glPointParameteri(GLenum pname, GLint param) {
    const GLfloat p[3] = {(GLfloat)param, 0.0f, 0.0f};
    glPointParameterfv(pname, p);
}

/* GL_ARB_point_parameters and GL_EXT_point_parameters: the same two entry points under both
 * suffixes, which is how the two extensions were published. */
void glPointParameterfARB(GLenum pname, GLfloat param) { glPointParameterf(pname, param); }
void glPointParameterfvARB(GLenum pname, const GLfloat *params) { glPointParameterfv(pname, params); }
void glPointParameterfEXT(GLenum pname, GLfloat param) { glPointParameterf(pname, param); }
void glPointParameterfvEXT(GLenum pname, const GLfloat *params) { glPointParameterfv(pname, params); }

void glPointParameteriv(GLenum pname, const GLint *params) {
    if (!params) return;
    GLfloat p[3] = {(GLfloat)params[0], 0.0f, 0.0f};
    if (pname == GL_POINT_DISTANCE_ATTENUATION) {
        p[1] = (GLfloat)params[1];
        p[2] = (GLfloat)params[2];
    }
    glPointParameterfv(pname, p);
}

void glLineWidth(GLfloat width) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_LINE_WIDTH, gl_la_f(width))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (width <= 0.0f) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
    ctx->line_width = width;
}

/* The factor is clamped to 1..256 rather than refused, as Mesa clamps it (main/lines.c:112). */
void glLineStipple(GLint factor, GLushort pattern) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_LINE_STIPPLE, gl_la_i(factor), gl_la_u(pattern))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->line_stipple_factor = factor < 1 ? 1 : (factor > 256 ? 256 : factor);
    ctx->line_stipple_pattern = pattern;
}

/* The GL 1.0 calls. Each sets **both faces** - which is how the specification defines them from
 * 2.0 onwards - by going to the shared body below, not through the 2.0 entry point: that one
 * is gated on the context having claimed 2.0, and these have been here since 1.0. */
static void stencil_func_set(gl_context_t *ctx, GLenum face, GLenum func, GLint ref,
                             GLuint mask);
static void stencil_op_set(gl_context_t *ctx, GLenum face, GLenum sfail, GLenum dpfail,
                           GLenum dppass);
static void stencil_mask_set(gl_context_t *ctx, GLenum face, GLuint mask);

void glStencilFunc(GLenum func, GLint ref, GLuint mask) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_STENCIL_FUNC, gl_la_e(func), gl_la_i(ref), gl_la_u(mask))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    stencil_func_set(ctx, GL_FRONT_AND_BACK, func, ref, mask);
}

void glStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_STENCIL_OP, gl_la_e(sfail), gl_la_e(dpfail), gl_la_e(dppass))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    stencil_op_set(ctx, GL_FRONT_AND_BACK, sfail, dpfail, dppass);
}

void glStencilMask(GLuint mask) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_STENCIL_MASK, gl_la_u(mask))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    stencil_mask_set(ctx, GL_FRONT_AND_BACK, mask);
}

/* -------------------------------------------------------------------------
 * GL 2.0's separate stencil state
 *
 * **The three GL 1.x calls above are the `GL_FRONT_AND_BACK` case of these**, which is exactly
 * how the specification defines them from 2.0 onwards - so there is one implementation rather
 * than two that have to agree, and a GL 1.x program keeps working because setting both faces to
 * the same thing is what it always did.
 *
 * `face` is GL_FRONT, GL_BACK or GL_FRONT_AND_BACK; anything else is GL_INVALID_ENUM.
 * ------------------------------------------------------------------------- */

static GLboolean gl_stencil_face_ok(GLenum face) {
    return (GLboolean)(face == GL_FRONT || face == GL_BACK || face == GL_FRONT_AND_BACK);
}

/* **The work, shared by the GL 1.x call and the GL 2.0 one.**
 *
 * `glStencilFunc` is the `GL_FRONT_AND_BACK` case of `glStencilFuncSeparate` and is implemented
 * as one - but the 2.0 entry point is gated on the context having claimed 2.0, and the 1.0 one
 * must not be. So the body lives here and each entry point brings its own gate, rather than the
 * older call reaching through the newer one and being refused by it. */
static void stencil_func_set(gl_context_t *ctx, GLenum face, GLenum func, GLint ref,
                             GLuint mask) {
    if (!gl_stencil_face_ok(face) || !gl_stencil_func_ok(func)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* The reference is clamped to the buffer's range, which the specification requires and which
     * also keeps the comparison honest: an unclamped 300 would never equal anything an 8-bit
     * buffer can hold, so GL_EQUAL would silently never pass. */
    const GLint clamped = (ref < 0) ? 0 : (ref > 255 ? 255 : ref);
    if (face != GL_BACK) {
        ctx->stencil_func = func;
        ctx->stencil_ref = clamped;
        ctx->stencil_value_mask = mask;
    }
    if (face != GL_FRONT) {
        ctx->stencil_back_func = func;
        ctx->stencil_back_ref = clamped;
        ctx->stencil_back_value_mask = mask;
    }
}

static void stencil_op_set(gl_context_t *ctx, GLenum face, GLenum sfail, GLenum dpfail,
                           GLenum dppass) {
    if (!gl_stencil_face_ok(face)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!gl_stencil_op_ok(sfail) || !gl_stencil_op_ok(dpfail) || !gl_stencil_op_ok(dppass)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (face != GL_BACK) {
        ctx->stencil_fail = sfail;
        ctx->stencil_zfail = dpfail;
        ctx->stencil_zpass = dppass;
    }
    if (face != GL_FRONT) {
        ctx->stencil_back_fail = sfail;
        ctx->stencil_back_zfail = dpfail;
        ctx->stencil_back_zpass = dppass;
    }
}

static void stencil_mask_set(gl_context_t *ctx, GLenum face, GLuint mask) {
    if (!gl_stencil_face_ok(face)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (face != GL_BACK) ctx->stencil_writemask = mask;
    if (face != GL_FRONT) ctx->stencil_back_writemask = mask;
}

/* The GL 2.0 entry points. Each is the shared body above plus the one thing that makes it 2.0's
 * rather than 1.0's: a context that has not claimed 2.0 does not have it. */
void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !gl_require_version(ctx, 2u, 0u)) return;
    stencil_func_set(ctx, face, func, ref, mask);
}

void glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !gl_require_version(ctx, 2u, 0u)) return;
    stencil_op_set(ctx, face, sfail, dpfail, dppass);
}

void glStencilMaskSeparate(GLenum face, GLuint mask) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !gl_require_version(ctx, 2u, 0u)) return;
    stencil_mask_set(ctx, face, mask);
}

void glClearStencil(GLint s) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_CLEAR_STENCIL, gl_la_i(s))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->clear_stencil = s & 0xff;
}

/* -------------------------------------------------------------------------
 * User clip planes
 * ------------------------------------------------------------------------- */
static int gl_clip_plane_index(GLenum plane) {
    const int i = (int)plane - (int)GL_CLIP_PLANE0;
    return (i >= 0 && i < OOPS_GL_CLIP_PLANE_COUNT) ? i : -1;
}

/* The plane arrives in object coordinates and is stored in eye coordinates, multiplied by the
 * inverse modelview of this moment. That is the specification's rule and it is what makes a clip
 * plane stay put in the world while the modelview moves afterwards - storing the caller's numbers
 * and using them directly would make the plane follow the object instead, which is a different
 * feature that happens to look right in any scene that never moves after setting it. */
void glClipPlane(GLenum plane, const GLdouble *equation) {
    /* Recorded in object coordinates, as passed - the transform into eye space belongs to the
     * modelview current when the list *runs*, which is the whole point of recording the call.
     * Narrowed to float as the plane is stored anyway (Mesa's save_ClipPlane does the same). */
    if (equation && gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_CLIP_PLANE, gl_la_e(plane), gl_la_f((GLfloat)equation[0]),
                    gl_la_f((GLfloat)equation[1]), gl_la_f((GLfloat)equation[2]),
                    gl_la_f((GLfloat)equation[3]))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !equation) return;
    const int i = gl_clip_plane_index(plane);
    if (i < 0) { gl_record_error(ctx, GL_INVALID_ENUM); return; }

    const float p[4] = {(float)equation[0], (float)equation[1],
                        (float)equation[2], (float)equation[3]};
    gl_mat4_t inv;
    if (mat4_invert(&inv, &ctx->modelview_stack[ctx->modelview_depth])) {
        /* A plane is a row vector: p' = p * M^-1. */
        for (int k = 0; k < 4; k++) {
            ctx->clip_plane[i][k] = p[0] * inv.m[k * 4 + 0] + p[1] * inv.m[k * 4 + 1] +
                                    p[2] * inv.m[k * 4 + 2] + p[3] * inv.m[k * 4 + 3];
        }
    } else {
        for (int k = 0; k < 4; k++) ctx->clip_plane[i][k] = p[k];
        gl_record_error(ctx, GL_INVALID_OPERATION);
    }
    ctx->hw_clip_dirty = GL_TRUE;
}

/* Returns what is stored, which is the eye-space plane - as the specification says. */
void glGetClipPlane(GLenum plane, GLdouble *equation) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !equation) return;
    const int i = gl_clip_plane_index(plane);
    if (i < 0) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    for (int k = 0; k < 4; k++) equation[k] = (GLdouble)ctx->clip_plane[i][k];
}

/* -------------------------------------------------------------------------
 * Texture coordinate generation
 *
 * The coordinate index, or -1 for a coord this does not know. S, T, R and Q are consecutive but
 * that is not relied on - a table keeps the mapping visible.
 * ------------------------------------------------------------------------- */
static int gl_texgen_index(GLenum coord) {
    switch (coord) {
        case GL_S: return 0;
        case GL_T: return 1;
        case GL_R: return 2;
        case GL_Q: return 3;
        default:   return -1;
    }
}

/* The five generation modes. GL_NORMAL_MAP and GL_REFLECTION_MAP, GL 1.3's cube-map modes, were
 * refused while there was no cube map here (until 2026-09-19). */
static GLboolean gl_texgen_mode_ok(GLenum mode) {
    return (mode == GL_OBJECT_LINEAR || mode == GL_EYE_LINEAR || mode == GL_SPHERE_MAP ||
            mode == GL_REFLECTION_MAP || mode == GL_NORMAL_MAP)
               ? GL_TRUE : GL_FALSE;
}

static void gl_texgen_set(gl_context_t *ctx, GLenum coord, GLenum pname, const GLfloat *v) {
    const int i = gl_texgen_index(coord);
    if (i < 0) { gl_record_error(ctx, GL_INVALID_ENUM); return; }

    if (pname == GL_TEXTURE_GEN_MODE) {
        const GLenum mode = (GLenum)v[0];
        if (!gl_texgen_mode_ok(mode)) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
        /* GL_SPHERE_MAP generates s and t only; asking for it on r or q is an error rather
         * than a coordinate that quietly never changes. */
        if (mode == GL_SPHERE_MAP && i > 1) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
        /* And the cube-map modes s, t and r (Mesa, main/texgen.c:113-121): q is no direction. */
        if ((mode == GL_REFLECTION_MAP || mode == GL_NORMAL_MAP) && i == 3) {
            gl_record_error(ctx, GL_INVALID_ENUM);
            return;
        }
        gl_tu(ctx)->texgen_mode[i] = mode;
        return;
    }
    if (pname == GL_OBJECT_PLANE) {
        for (int k = 0; k < 4; k++) gl_tu(ctx)->texgen_object_plane[i][k] = v[k];
        return;
    }
    if (pname == GL_EYE_PLANE) {
        /* **Stored through the inverse modelview of this moment**, which is what makes an
         * eye-linear plane stay where it was put while the modelview moves afterwards. */
        gl_mat4_t inv;
        if (mat4_invert(&inv, &ctx->modelview_stack[ctx->modelview_depth])) {
            /* The plane is a row vector: p' = p * M^-1, which is M^-T applied as a column. */
            for (int k = 0; k < 4; k++) {
                gl_tu(ctx)->texgen_eye_plane[i][k] = v[0] * inv.m[k * 4 + 0] + v[1] * inv.m[k * 4 + 1] +
                                              v[2] * inv.m[k * 4 + 2] + v[3] * inv.m[k * 4 + 3];
            }
        } else {
            /* A singular modelview has no inverse; the plane is kept as given rather than
             * filled with infinities, and the call is refused so the caller knows. */
            for (int k = 0; k < 4; k++) gl_tu(ctx)->texgen_eye_plane[i][k] = v[k];
            gl_record_error(ctx, GL_INVALID_OPERATION);
        }
        return;
    }
    gl_record_error(ctx, GL_INVALID_ENUM);
}

void glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params) {
    /* Recorded in the coordinates passed; an eye plane is put through the modelview current
     * when the list runs, as glClipPlane's is. */
    if (params && gl_list_recording() &&
        gl_list_rec_fv(GL_LIST_OP_TEX_GEN_FV, coord, pname, GL_TRUE, pname, params)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    gl_texgen_set(ctx, coord, pname, params);
}

void glTexGeniv(GLenum coord, GLenum pname, const GLint *params) {
    if (params && gl_list_recording() &&
        gl_list_rec_iv(GL_LIST_OP_TEX_GEN_IV, coord, pname, GL_TRUE, pname, params)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    /* Four are read for a plane and one for a mode, so only the first is touched unless the
     * parameter is a plane - reading four from a one-element array would be a fault. */
    GLfloat f[4] = {(GLfloat)params[0], 0.0f, 0.0f, 0.0f};
    if (pname == GL_OBJECT_PLANE || pname == GL_EYE_PLANE) {
        for (int k = 1; k < 4; k++) f[k] = (GLfloat)params[k];
    }
    gl_texgen_set(ctx, coord, pname, f);
}

void glTexGendv(GLenum coord, GLenum pname, const GLdouble *params) {
    if (!params) return;
    GLfloat f[4] = {(GLfloat)params[0], 0.0f, 0.0f, 0.0f};
    if (pname == GL_OBJECT_PLANE || pname == GL_EYE_PLANE) {
        for (int k = 1; k < 4; k++) f[k] = (GLfloat)params[k];
    }
    /* Narrowed first, so the list holds what glTexGenfv would. */
    if (gl_list_recording() &&
        gl_list_rec_fv(GL_LIST_OP_TEX_GEN_FV, coord, pname, GL_TRUE, pname, f)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    gl_texgen_set(ctx, coord, pname, f);
}

/* The scalar forms set only the mode: a plane needs four values, and the specification says so
 * by giving GL_OBJECT_PLANE and GL_EYE_PLANE no scalar spelling. */
void glTexGeni(GLenum coord, GLenum pname, GLint param) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_TEX_GEN_I, gl_la_e(coord), gl_la_e(pname), gl_la_i(param))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (pname != GL_TEXTURE_GEN_MODE) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    GLfloat f[4] = {(GLfloat)param, 0.0f, 0.0f, 0.0f};
    gl_texgen_set(ctx, coord, pname, f);
}
void glTexGenf(GLenum coord, GLenum pname, GLfloat param) {
    glTexGeni(coord, pname, (GLint)param);
}
void glTexGend(GLenum coord, GLenum pname, GLdouble param) {
    glTexGeni(coord, pname, (GLint)param);
}

void glGetTexGenfv(GLenum coord, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    const int i = gl_texgen_index(coord);
    if (i < 0) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    switch (pname) {
        case GL_TEXTURE_GEN_MODE: params[0] = (GLfloat)gl_tu(ctx)->texgen_mode[i]; break;
        case GL_OBJECT_PLANE:
            for (int k = 0; k < 4; k++) params[k] = gl_tu(ctx)->texgen_object_plane[i][k];
            break;
        case GL_EYE_PLANE:
            for (int k = 0; k < 4; k++) params[k] = gl_tu(ctx)->texgen_eye_plane[i][k];
            break;
        default: gl_record_error(ctx, GL_INVALID_ENUM); break;
    }
}

void glGetTexGeniv(GLenum coord, GLenum pname, GLint *params) {
    GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    glGetTexGenfv(coord, pname, f);
    const int n = (pname == GL_TEXTURE_GEN_MODE) ? 1 : 4;
    for (int k = 0; k < n; k++) params[k] = (GLint)f[k];
}

void glGetTexGendv(GLenum coord, GLenum pname, GLdouble *params) {
    GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    glGetTexGenfv(coord, pname, f);
    const int n = (pname == GL_TEXTURE_GEN_MODE) ? 1 : 4;
    for (int k = 0; k < n; k++) params[k] = (GLdouble)f[k];
}

/* The environment modes: Mesa's legal set (main/texenv.c, set_env_mode). Anything else was
 * stored until 2026-09-19 and drawn as GL_MODULATE, with no error; GL_COMBINE was refused until
 * the combiner landed, the same day. */
static GLboolean gl_tex_env_mode_ok(GLenum m) {
    return (GLboolean)(m == GL_MODULATE || m == GL_DECAL || m == GL_BLEND || m == GL_REPLACE ||
                       m == GL_ADD || m == GL_COMBINE);
}

/* **The one texture-environment setter**, every form reaching it with floats. The combiner's
 * parameters are checked as Mesa checks them (main/texenv.c:107-370): a colour or alpha
 * function GL 1.3 has, GL_DOT3_RGB and GL_DOT3_RGBA for the colour only; a source among
 * GL_TEXTURE, GL_CONSTANT, GL_PRIMARY_COLOR, GL_PREVIOUS and GL 1.4's crossbar GL_TEXTUREn for
 * each unit there is; an operand among the colour and alpha ones for a colour argument,
 * the alpha ones for an alpha argument - each an enum error otherwise - and a scale of 1, 2 or
 * 4, a value error otherwise. */
static void gl_tex_env_set(gl_context_t *ctx, GLenum pname, const GLfloat *p) {
    const GLenum e = (GLenum)(GLint)p[0];
    gl_combine_t *cb = &gl_tu(ctx)->combine;
    switch (pname) {
        case GL_TEXTURE_ENV_COLOR:
            for (int i = 0; i < 4; i++) gl_tu(ctx)->tex_env_color[i] = p[i];
            return;
        case GL_TEXTURE_ENV_MODE:
            if (!gl_tex_env_mode_ok(e)) break;
            gl_tu(ctx)->tex_env_mode = e;
            gl_ps_patch_tex_env(ctx);
            return;
        case GL_COMBINE_RGB:
        case GL_COMBINE_ALPHA: {
            const GLboolean common = (GLboolean)(e == GL_REPLACE || e == GL_MODULATE ||
                                                 e == GL_ADD || e == GL_ADD_SIGNED ||
                                                 e == GL_INTERPOLATE || e == GL_SUBTRACT);
            const GLboolean dot3 = (GLboolean)(e == GL_DOT3_RGB || e == GL_DOT3_RGBA);
            if (!common && !(dot3 && pname == GL_COMBINE_RGB)) break;
            if (pname == GL_COMBINE_RGB) cb->mode_rgb = e;
            else cb->mode_alpha = e;
            return;
        }
        case GL_SOURCE0_RGB: case GL_SOURCE1_RGB: case GL_SOURCE2_RGB:
        case GL_SOURCE0_ALPHA: case GL_SOURCE1_ALPHA: case GL_SOURCE2_ALPHA:
            if (e != GL_TEXTURE && e != GL_CONSTANT && e != GL_PRIMARY_COLOR && e != GL_PREVIOUS &&
                !(e >= GL_TEXTURE0 && e < GL_TEXTURE0 + OOPS_GL_MAX_TEXTURE_UNITS)) {
                break;
            }
            if (pname <= GL_SOURCE2_RGB) cb->source_rgb[pname - GL_SOURCE0_RGB] = e;
            else cb->source_alpha[pname - GL_SOURCE0_ALPHA] = e;
            return;
        case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB:
            if (e != GL_SRC_COLOR && e != GL_ONE_MINUS_SRC_COLOR && e != GL_SRC_ALPHA &&
                e != GL_ONE_MINUS_SRC_ALPHA) {
                break;
            }
            cb->operand_rgb[pname - GL_OPERAND0_RGB] = e;
            return;
        case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA:
            if (e != GL_SRC_ALPHA && e != GL_ONE_MINUS_SRC_ALPHA) break;
            cb->operand_alpha[pname - GL_OPERAND0_ALPHA] = e;
            return;
        case GL_RGB_SCALE:
        case GL_ALPHA_SCALE:
            if (p[0] != 1.0f && p[0] != 2.0f && p[0] != 4.0f) {
                gl_record_error(ctx, GL_INVALID_VALUE);
                return;
            }
            if (pname == GL_RGB_SCALE) cb->scale_rgb = p[0];
            else cb->scale_alpha = p[0];
            return;
        default:
            break;
    }
    /* The pname, not the value: the switch above is on the parameter name, and a pname this
       does not keep is the common refusal. A value it does not accept for a pname it does
       lands here too, and the pname is still the more useful half. */
    gl_record_error_val(ctx, GL_INVALID_ENUM, pname);
}

/* The two targets: GL_TEXTURE_ENV, and GL 1.4's GL_TEXTURE_FILTER_CONTROL, whose one parameter is
 * the unit's level-of-detail bias (Mesa main/texenv.c:448-460). Same rule as everywhere else
 * here: a target or a parameter this does not keep is refused rather than dropped - silently
 * ignoring one left a caller believing it had set an environment that was never stored. */
static void gl_tex_env_target_set(gl_context_t *ctx, GLenum target, GLenum pname,
                                  const GLfloat *p) {
    if (target == GL_TEXTURE_ENV) {
        gl_tex_env_set(ctx, pname, p);
    } else if (target == GL_TEXTURE_FILTER_CONTROL && pname == GL_TEXTURE_LOD_BIAS) {
        gl_tu(ctx)->tex_lod_bias = p[0];
    } else if (target == GL_POINT_SPRITE && pname == GL_COORD_REPLACE) {
        /* The third target. A boolean, so anything non-zero is true - the specification takes it
         * as a GLboolean and Mesa compares against zero rather than against GL_TRUE. */
        gl_tu(ctx)->coord_replace = (GLboolean)(p[0] != 0.0f);
    } else {
        gl_record_error_val(ctx, GL_INVALID_ENUM, target);
    }
}

/* The scalar forms go through the vector one with the value first and zeros after, as Mesa's do
 * (main/texenv.c, _mesa_TexEnvi). */
void glTexEnvi(GLenum target, GLenum pname, GLint param) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_TEX_ENV_I, gl_la_e(target), gl_la_e(pname), gl_la_i(param))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    const GLfloat p[4] = {(GLfloat)param, 0.0f, 0.0f, 0.0f};
    gl_tex_env_target_set(ctx, target, pname, p);
}

/* **Its own path, not glTexEnvi's**, which truncated: glTexEnvf(GL_RGB_SCALE, 1.5f) is a value
 * error, not a scale of 1. */
void glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
    const GLfloat p[4] = {param, 0.0f, 0.0f, 0.0f};
    glTexEnvfv(target, pname, p);
}

/* **This used to return clean having done nothing** for a target or pname it did not keep,
 * while its own sibling glTexEnvi refused the same arguments. Two spellings of one call
 * disagreeing about what is an error is worse than either answer: a program that set the
 * environment through the vector form believed it had, and the scalar form would have told it
 * otherwise. */
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params) {
    if (params && gl_list_recording() &&
        gl_list_rec_fv(GL_LIST_OP_TEX_ENV_FV, target, pname, GL_TRUE, pname, params)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    GLfloat p[4] = {params[0], 0.0f, 0.0f, 0.0f};
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_COLOR) {
        for (int i = 1; i < 4; i++) p[i] = params[i];
    }
    gl_tex_env_target_set(ctx, target, pname, p);
}

/* The integer vector form. GL_TEXTURE_ENV_COLOR in integers is a colour, so it converts across
 * the signed range the way glLightiv's colours do rather than by a cast. */
void glTexEnviv(GLenum target, GLenum pname, const GLint *params) {
    if (!params) return;
    if (pname == GL_TEXTURE_ENV_COLOR) {
        GLfloat f[4];
        for (int i = 0; i < 4; i++) f[i] = gl_int_to_colour(params[i]);
        glTexEnvfv(target, pname, f);
        return;
    }
    glTexEnvi(target, pname, params[0]);
}

/* A texture-environment parameter that is one enum or scale: the combiner's, and the mode. False
 * for any other pname. */
static GLboolean gl_tex_env_scalar(const gl_context_t *ctx, GLenum pname, GLfloat *out) {
    const gl_combine_t *cb = &gl_tu(ctx)->combine;
    switch (pname) {
        case GL_TEXTURE_ENV_MODE: *out = (GLfloat)gl_tu(ctx)->tex_env_mode; return GL_TRUE;
        case GL_COMBINE_RGB:      *out = (GLfloat)cb->mode_rgb; return GL_TRUE;
        case GL_COMBINE_ALPHA:    *out = (GLfloat)cb->mode_alpha; return GL_TRUE;
        case GL_SOURCE0_RGB: case GL_SOURCE1_RGB: case GL_SOURCE2_RGB:
            *out = (GLfloat)cb->source_rgb[pname - GL_SOURCE0_RGB];
            return GL_TRUE;
        case GL_SOURCE0_ALPHA: case GL_SOURCE1_ALPHA: case GL_SOURCE2_ALPHA:
            *out = (GLfloat)cb->source_alpha[pname - GL_SOURCE0_ALPHA];
            return GL_TRUE;
        case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB:
            *out = (GLfloat)cb->operand_rgb[pname - GL_OPERAND0_RGB];
            return GL_TRUE;
        case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA:
            *out = (GLfloat)cb->operand_alpha[pname - GL_OPERAND0_ALPHA];
            return GL_TRUE;
        case GL_RGB_SCALE:   *out = cb->scale_rgb; return GL_TRUE;
        case GL_ALPHA_SCALE: *out = cb->scale_alpha; return GL_TRUE;
        default: return GL_FALSE;
    }
}

static GLint gl_float_to_int_color(GLfloat x);

void glGetTexEnviv(GLenum target, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    /* The unit's bias as an integer by a plain cast, as Mesa's is (main/texenv.c:798). */
    if (target == GL_TEXTURE_FILTER_CONTROL && pname == GL_TEXTURE_LOD_BIAS) {
        params[0] = (GLint)gl_tu(ctx)->tex_lod_bias;
        return;
    }
    /* The setter learned a third target, so this has to as well - a query that refuses what the
     * matching set accepts is the disagreement `glTexEnvfv` above was already fixed for once. */
    if (target == GL_POINT_SPRITE && pname == GL_COORD_REPLACE) {
        params[0] = gl_tu(ctx)->coord_replace ? 1 : 0;
        return;
    }
    if (target != GL_TEXTURE_ENV) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    GLfloat scalar = 0.0f;
    if (gl_tex_env_scalar(ctx, pname, &scalar)) {
        params[0] = (GLint)scalar; /* an enum, or a scale of 1, 2 or 4 */
        return;
    }
    switch (pname) {
        case GL_TEXTURE_ENV_COLOR:
            /* GL converts a float colour to an integer across the signed range, which is the
             * inverse of the conversion going in. **In double** - this multiplied by
             * 2147483647.0f, which is 2^31 as a float, so a channel of 1.0 overflowed GLint and
             * read back as -2147483648 until 2026-09-19. */
            for (int i = 0; i < 4; i++) {
                params[i] = gl_float_to_int_color(gl_tu(ctx)->tex_env_color[i]);
            }
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (target == GL_TEXTURE_FILTER_CONTROL && pname == GL_TEXTURE_LOD_BIAS) {
        params[0] = gl_tu(ctx)->tex_lod_bias;
        return;
    }
    if (target == GL_POINT_SPRITE && pname == GL_COORD_REPLACE) {
        params[0] = gl_tu(ctx)->coord_replace ? 1.0f : 0.0f;
        return;
    }
    if (target != GL_TEXTURE_ENV) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (gl_tex_env_scalar(ctx, pname, params)) return;
    switch (pname) {
        case GL_TEXTURE_ENV_COLOR:
            for (int i = 0; i < 4; i++) params[i] = gl_tu(ctx)->tex_env_color[i];
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

/* The stored mode for one hint target, or NULL for a target GL 1.x does not have. */
static GLenum *gl_hint_slot(gl_context_t *ctx, GLenum target) {
    switch (target) {
        case GL_PERSPECTIVE_CORRECTION_HINT: return &ctx->perspective_hint;
        case GL_POINT_SMOOTH_HINT:           return &ctx->hint_point_smooth;
        case GL_LINE_SMOOTH_HINT:            return &ctx->hint_line_smooth;
        case GL_POLYGON_SMOOTH_HINT:         return &ctx->hint_polygon_smooth;
        case GL_FOG_HINT:                    return &ctx->hint_fog;
        case GL_TEXTURE_COMPRESSION_HINT:    return &ctx->hint_texture_compression;
        case GL_GENERATE_MIPMAP_HINT:        return &ctx->hint_generate_mipmap;
        default:                             return (GLenum *)0;
    }
}

/* `glHint` - advisory by definition: each target is recorded and reported, and none of them
 * changes a pixel, which is the whole of what a hint is entitled to do.
 *
 * **Only the perspective hint was accepted until 2026-09-19**, on the grounds that the fog, point
 * and line hints named features this did not draw. It draws all three now - and even then, a hint
 * is a preference about *how*, which an implementation is free to ignore; refusing it made the
 * `glHint(GL_FOG_HINT, GL_NICEST)` a great deal of 1.x start-up code carries an error. */
void glHint(GLenum target, GLenum mode) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_HINT, gl_la_e(target), gl_la_e(mode))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    GLenum *slot = gl_hint_slot(ctx, target);
    if (!slot) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    *slot = mode;
}

/* `glDrawBuffer` / `glReadBuffer` (GL 1.0, 4.2.1 and 4.3.2). The visual is double-buffered and
 * mono with no auxiliary buffers, so GL's names sort four ways (gl_color_buffer_bits):
 *
 * - GL_BACK and GL_BACK_LEFT name the back buffer, the display's framebuffer, which
 *   glSwapBuffers presents.
 * - GL_FRONT and GL_FRONT_LEFT name the front, the picture on screen: oops-gl's own surface,
 *   allocated the first time a program names it, filled with what is on screen, and put on
 *   screen by glFlush and glFinish once drawn into (gl_front_buffer, gl_front_present). No
 *   memory for it is GL_OUT_OF_MEMORY, the state unchanged.
 * - GL_LEFT and GL_FRONT_AND_BACK name both: drawn, both are written; read, the front is.
 * - The right buffers and GL_AUX0..3 name buffers this visual does not have:
 *   GL_INVALID_OPERATION, as the specification says. Anything else is GL_INVALID_ENUM, and so
 *   is glReadBuffer(GL_NONE); glDrawBuffer(GL_NONE) draws into no colour buffer (colour writes
 *   off, the other buffers still written).
 *
 * Only GL_BACK was accepted until 2026-09-19, everything else an enum error. Later that day the
 * other names were accepted, but the front ones were refused, because the display handed out
 * its back buffer only. That evening the front became a surface of its own
 * (oops_display_present puts it on screen). */
static GLboolean gl_color_buffer_check(gl_context_t *ctx, unsigned bits) {
    if (bits == 0u) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return GL_FALSE;
    }
    if (bits & GL_OCB_ABSENT) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    if ((bits & GL_OCB_FRONT) && !gl_front_buffer(ctx)) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return GL_FALSE;
    }
    return GL_TRUE;
}

void glDrawBuffer(GLenum buf) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_DRAW_BUFFER, gl_la_e(buf))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* The console follows through CB_TARGET_MASK, which every draw emits (gl_color_writes). */
    if (buf != GL_NONE && !gl_color_buffer_check(ctx, gl_color_buffer_bits(buf))) return;
    ctx->draw_buffer = buf;
    gl_draw_targets(ctx);
}

/*
 * GL 2.0's `glDrawBuffers`: several colour buffers named at once.
 *
 * **On a window-system framebuffer this means the same fragment colour to each of them**, not a
 * different one per buffer - true multiple render targets are framebuffer objects and their
 * GL_COLOR_ATTACHMENT names, which are GL 3.0 and are not here. So this is `glDrawBuffer` of the
 * union, which is what the two existing colour targets already do under GL_FRONT_AND_BACK.
 *
 * The specification's three errors, and each is a mistake worth naming rather than accepting:
 * `n` outside [0, GL_MAX_DRAW_BUFFERS] is GL_INVALID_VALUE; GL_FRONT, GL_BACK,
 * GL_FRONT_AND_BACK and GL_LEFT name more than one buffer each and may not appear in a list at
 * all (2.0, 4.2.1), which is GL_INVALID_OPERATION; and a buffer named twice is the same error.
 */
void glDrawBuffers(GLsizei n, const GLenum *bufs) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !gl_require_version(ctx, 2u, 0u)) return;
    GLint limit = 0;
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &limit);
    if (n < 0 || n > limit) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (n > 0 && !bufs) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    unsigned union_bits = 0u;
    for (GLsizei i = 0; i < n; i++) {
        if (bufs[i] == GL_NONE) continue;
        const unsigned bits = gl_color_buffer_bits(bufs[i]);
        if (bits == 0u) {
            gl_record_error(ctx, GL_INVALID_ENUM);
            return;
        }
        if (bits & GL_OCB_ABSENT) {
            gl_record_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        /* A name covering more than one buffer, or one already named. */
        if ((bits & (bits - 1u)) != 0u || (union_bits & bits) != 0u) {
            gl_record_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        union_bits |= bits;
    }

    /* Back through `glDrawBuffer`, so the union goes through the one place that checks a front
     * buffer can be allocated and re-derives the console's target mask. */
    GLenum single = GL_NONE;
    if (union_bits == (GL_OCB_FRONT | GL_OCB_BACK)) single = GL_FRONT_AND_BACK;
    else if (union_bits == GL_OCB_FRONT) single = GL_FRONT;
    else if (union_bits == GL_OCB_BACK) single = GL_BACK;
    glDrawBuffer(single);
}

void glReadBuffer(GLenum src) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_READ_BUFFER, gl_la_e(src))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* GL_NONE is not a buffer name here: GL 1.x has no reading from no buffer. */
    if (!gl_color_buffer_check(ctx, gl_color_buffer_bits(src))) return;
    ctx->read_buffer = src;
}

void glDepthFunc(GLenum func) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_DEPTH_FUNC, gl_la_e(func))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->depth_func = func;
}

void glDepthMask(GLboolean flag) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_DEPTH_MASK, gl_la_u(flag))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->depth_mask = flag;
}

void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_COLOR_MASK, gl_la_u(red), gl_la_u(green), gl_la_u(blue),
                    gl_la_u(alpha))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->color_mask[0] = red;
    ctx->color_mask[1] = green;
    ctx->color_mask[2] = blue;
    ctx->color_mask[3] = alpha;
}

void glCullFace(GLenum mode) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_CULL_FACE, gl_la_e(mode))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->cull_mode = mode;
}

void glFrontFace(GLenum mode) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_FRONT_FACE, gl_la_e(mode))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->front_face = mode;
}

void glShadeModel(GLenum mode) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_SHADE_MODEL, gl_la_e(mode))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->shade_model = mode;
}

GLenum glGetError(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GL_NO_ERROR;
    GLenum err = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    return err;
}

/*
 * The version string, built into the context because glGetString hands out a pointer that has to
 * outlive the call. "<major>.<minor> oops-gl fixed-function subset" - the suffix always, so a
 * caller reading past the number learns what this is whatever version it asked for.
 */
void gl_version_string(gl_context_t *ctx) {
    char *p = ctx->version_string;
    *p++ = (char)('0' + (ctx->version_major % 10u));
    *p++ = '.';
    *p++ = (char)('0' + (ctx->version_minor % 10u));
    /* **The suffix follows the number**, because "fixed-function" is the wrong word for a badge
     * that says 2.0 - the whole of what 2.0 adds is the pipeline that is not fixed-function.
     * Both still say `subset`, which is the part that must never come off. */
    static const char tail_1x[] = " oops-gl fixed-function subset";
    static const char tail_2x[] = " oops-gl programmable subset";
    const char *tail = (ctx->version_major >= 2u) ? tail_2x : tail_1x;
    const size_t n = (ctx->version_major >= 2u) ? sizeof(tail_2x) : sizeof(tail_1x);
    for (size_t i = 0; i < n; i++) p[i] = tail[i];
}

/*
 * **The version is the caller's to state** (2026-09-20), because the badge is about the program's
 * expectations, not this library's opinion of itself.
 *
 * The default is 1.1: the honest class of what is implemented everywhere, and what GL_VERSION
 * said unconditionally until now. It had said "OpenGL 1.3 oops-gl 2.0" before that, which was
 * wrong twice over - it claimed a 1.3 nothing here distinguished from 1.1, and it put a word in
 * front of the number, so a caller doing the usual atof() read 0.0 rather than any version.
 *
 * A port written against a later 1.x checks the badge before calling something this library
 * *does* have, and refuses to run when it reads lower. Setting the version lets that port run.
 * It changes nothing else: no call becomes implemented, and the suffix keeps saying subset. What
 * it does do is put the claim where it belongs - a program asserting what it targets, with a log
 * line recording that it asked, rather than this library asserting a conformance it has not got.
 */
GLboolean glContextSetVersion(GLuint major, GLuint minor) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GL_FALSE;
    /* **1.0 through 1.5, and 2.0 and 2.1** (2026-09-21). This refused anything but 1.x, and
     * said "nothing above it is implemented at all" - which stopped being true the day the
     * programmable pipeline ran: `glCreateShader` through `glUseProgram` exist, both stages
     * run, and `gl2-probe` measures forty-odd checks against them.
     *
     * **2.1 is the GLSL 1.20 one**, and is claimable because the front end takes that dialect:
     * implicit int-to-float conversion, `invariant` and `centroid`, `transpose` and
     * `outerProduct`. It was refused for exactly as long as that was untrue.
     *
     * 3.x and above are refused because none of them is implemented, which is the reason this
     * gate exists at all.
     *
     * A badge is still only a badge: what it changes is what `glGetString` answers, not what
     * any call does. An app that claims 1.1 and then calls `glCreateShader` gets a shader,
     * exactly as a desktop driver would - a context exposes what it exposes, and the number is
     * the program stating what it targets. */
    const GLboolean ok = (GLboolean)((major == 1u && minor <= 5u) ||
                                     (major == 2u && minor <= 1u));
    if (!ok) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return GL_FALSE;
    }
    ctx->version_major = major;
    ctx->version_minor = minor;
    gl_version_string(ctx);
    gl_log_line("the program set the reported GL version; behaviour is unchanged by it");
    return GL_TRUE;
}

void glContextGetVersion(GLuint *major, GLuint *minor) {
    gl_context_t *ctx = gl_get_ctx();
    if (major) *major = ctx ? ctx->version_major : 0u;
    if (minor) *minor = ctx ? ctx->version_minor : 0u;
}

/* What a program is told it is talking to. */
const GLubyte *glGetString(GLenum name) {
    gl_context_t *ctx = gl_get_ctx();
    switch (name) {
        case GL_VENDOR:      return (const GLubyte *)"OOPS Project";
        case GL_RENDERER:    return (const GLubyte *)"Prospero-generation RDNA2, freestanding";
        /* The context's, which glContextSetVersion writes and context creation defaults. */
        case GL_VERSION:
            if (!ctx || ctx->version_string[0] == '\0') {
                return (const GLubyte *)"1.1 oops-gl fixed-function subset";
            }
            return (const GLubyte *)ctx->version_string;
        /* **The shading language has its own version and its own string**, and it answers the
         * *highest* dialect the front end takes rather than the one the GL badge pairs with -
         * which is what the specification asks for and is why the two numbers are allowed to
         * differ. A shader may still say `#version 110`, and it is then held to 1.10's rules:
         * the number here is a ceiling, not a mode. Anything above 1.20 is refused by number at
         * compile rather than taken as one of them. */
        case GL_SHADING_LANGUAGE_VERSION:
            /* GL 2.0's enumerant, so a context that has not claimed 2.0 has never heard of it. */
            if (!ctx || !gl_require_version_enum(ctx, 2u, 0u)) return (const GLubyte *)0;
            return (const GLubyte *)"1.20 oops-gl";
        /* **Empty, and that is the honest answer.**
         *
         * This used to say `GL_EXT_vertex_array`. The arrays are here - but an extension string
         * is a promise about *that extension's entry points*, and `glVertexPointerEXT`,
         * `glDrawArraysEXT`, `glArrayElementEXT` and the rest do not exist here. Only the core
         * spellings do. A program that reads the string and calls the EXT names fails to link,
         * which is a loud failure but still one this library invited.
         *
         * The same reasoning kept `GL_ARB_vertex_buffer_object` out when the buffer objects
         * landed: the core `glGenBuffers` exists, `glGenBuffersARB` does not, so the extension
         * is not supported however much of its behaviour is.
         *
         * An empty list is not a refusal and not a placeholder - it is the true statement that
         * no extension's own entry points are provided. */
        /* **What this library implements, under the names a program of that era looks for**
         * (2026-09-19; empty until then).
         *
         * The rule that kept it empty stands: an extension is a promise about *its own entry
         * points*, so none was listed while only the core spellings existed. The entry points
         * are here now - `glGenBuffersARB`, `glSecondaryColor3fEXT`, `glWindowPos2iARB` and the
         * rest, each the core function under its published name (gl.h's compatibility
         * section) - and the extensions that need no entry point at all, only state this
         * library keeps, are listed beside them.
         *
         * **The list is what the path in use does.** GL_ARB_multitexture was on it for the
         * software rasteriser and off on the console, where a draw sampled unit 0 alone; it is
         * on both since 2026-09-20, the console path having gained the second unit that
         * `REQ-20260920T0745Z-9a41` measured (gl_multitex.h). The console still applies two
         * units where the software rasteriser applies every one `glActiveTexture` names, and
         * `GL_MAX_TEXTURE_UNITS` says so on each: the extension promises two, which is what the
         * name is about, and a program asking for more is asking the query, not the list.
         *
         * **GL_ARB_texture_cube_map joined both lists on 2026-09-20**, when the console gained
         * the sample: it adds no entry point of its own, only targets, enums and the
         * GL_NORMAL_MAP and GL_REFLECTION_MAP generation modes, and every one of those is kept
         * on both paths now. A cube map whose six faces have not all arrived is not sampled on
         * either - GL does not sample an incomplete one (2.1, 3.8.10) - so the promise holds.
         *
         * **GL_EXT_texture3D joined both lists on 2026-09-20**, the same day the console gained
         * the volume sample. It has two entry points of its own, `glTexImage3DEXT` and
         * `glTexSubImage3DEXT`, and they arrived with it (gl.h's compatibility section) - a list
         * entry without them would be the broken promise this library refuses to make. The
         * console samples the base level only, which is a level-of-detail difference and not a
         * missing feature; the extension says nothing about mip chains that GL 1.2 does not.
         *
         * **GL_ARB_depth_texture and GL_ARB_shadow joined both lists on 2026-09-20**, when the
         * console gained the comparison sample. Like the cube-map extension neither adds an
         * entry point - depth textures are `glTexImage2D` with a `GL_DEPTH_COMPONENT` internal
         * format, and the comparison is `glTexParameteri` - so what they promise is enums and
         * behaviour, and both paths have all of it. GL_ARB_shadow is listed only beside
         * GL_ARB_depth_texture, which is the pair a program looks for.
         *
         * **GL_ARB_occlusion_query joined both lists on 2026-09-20**, when the GPU started
         * counting: two `ZPASS_DONE` events bracket the query and the answer is the sum over the
         * sixteen render backends (`REQ-20260919T2048Z-4d19`). Its eight entry points arrived
         * with it - `glBeginQueryARB` and the rest, each the core function under its published
         * name - because the extension predates GL 1.5 and a program using it calls those. The
         * one case the console still will not count is a query whose draws never test depth,
         * which the log says; the extension is about GL_SAMPLES_PASSED, which is the depth test's
         * own count, so the promise holds.
         *
         * Nothing is left off either list. */
        case GL_EXTENSIONS:
            return (const GLubyte *)((ctx && ctx->use_hardware)
                ? "GL_ARB_depth_texture GL_ARB_multitexture GL_ARB_occlusion_query "
                  "GL_ARB_point_parameters GL_ARB_shadow GL_ARB_texture_border_clamp "
                  "GL_ARB_texture_cube_map GL_ARB_texture_env_add "
                  "GL_ARB_texture_env_combine GL_ARB_texture_env_dot3 "
                  "GL_ARB_texture_mirrored_repeat GL_ARB_transpose_matrix "
                  "GL_ARB_vertex_buffer_object GL_ARB_window_pos GL_EXT_bgra GL_EXT_blend_color "
                  "GL_EXT_blend_minmax GL_EXT_blend_subtract GL_EXT_draw_range_elements "
                  "GL_EXT_fog_coord GL_EXT_multi_draw_arrays GL_EXT_point_parameters "
                  "GL_EXT_rescale_normal GL_EXT_secondary_color "
                  "GL_EXT_separate_specular_color GL_EXT_stencil_wrap "
                  "GL_EXT_texture3D "
                  "GL_EXT_texture_edge_clamp GL_EXT_texture_env_add GL_EXT_texture_env_combine "
                  "GL_EXT_texture_env_dot3 GL_EXT_texture_lod_bias GL_SGIS_texture_edge_clamp"
                : "GL_ARB_depth_texture GL_ARB_multitexture GL_ARB_occlusion_query "
                  "GL_ARB_point_parameters GL_ARB_shadow GL_ARB_texture_border_clamp "
                  "GL_ARB_texture_cube_map "
                  "GL_ARB_texture_env_add GL_ARB_texture_env_combine GL_ARB_texture_env_dot3 "
                  "GL_ARB_texture_mirrored_repeat GL_ARB_transpose_matrix "
                  "GL_ARB_vertex_buffer_object GL_ARB_window_pos GL_EXT_bgra GL_EXT_blend_color "
                  "GL_EXT_blend_minmax GL_EXT_blend_subtract GL_EXT_draw_range_elements "
                  "GL_EXT_fog_coord GL_EXT_multi_draw_arrays GL_EXT_point_parameters "
                  "GL_EXT_rescale_normal GL_EXT_secondary_color "
                  "GL_EXT_separate_specular_color GL_EXT_stencil_wrap "
                  "GL_EXT_texture3D "
                  "GL_EXT_texture_edge_clamp GL_EXT_texture_env_add GL_EXT_texture_env_combine "
                  "GL_EXT_texture_env_dot3 GL_EXT_texture_lod_bias GL_SGIS_texture_edge_clamp");
        default:
            /* The specification's answer for an unrecognised name: no string, and an error
             * the caller can see. An empty string is indistinguishable from a real answer of
             * "no extensions", which is a wrong answer rather than a refused one. */
            if (ctx) gl_record_error(ctx, GL_INVALID_ENUM);
            return (const GLubyte *)0;
    }
}

/* **How many elements a query writes.** The caller sizes its buffer from the pname, so this is
 * not a convenience: a query that writes four values into a `GLint[1]` corrupts whatever the
 * caller put next to it, and does so only for the pnames nobody tested. Anything not named here
 * writes one value, which is the common case and the safe one.
 */
static int gl_query_element_count(GLenum pname) {
    switch (pname) {
        case GL_MODELVIEW_MATRIX:
        case GL_PROJECTION_MATRIX:
        case GL_TEXTURE_MATRIX:
        case GL_TRANSPOSE_MODELVIEW_MATRIX:
        case GL_TRANSPOSE_PROJECTION_MATRIX:
        case GL_TRANSPOSE_TEXTURE_MATRIX:
            return 16;
        case GL_VIEWPORT:
        case GL_SCISSOR_BOX:
        case GL_CURRENT_COLOR:
        case GL_CURRENT_SECONDARY_COLOR:
        case GL_CURRENT_TEXTURE_COORDS:
        case GL_COLOR_CLEAR_VALUE:
        case GL_COLOR_WRITEMASK:
        case GL_LIGHT_MODEL_AMBIENT:
        /* **These five were missing**, so glGetDoublev - which copies exactly this many out of
         * glGetFloatv's answer - returned one component of each four-component vector and left
         * the other three as the caller's buffer had them. */
        case GL_BLEND_COLOR:
        case GL_FOG_COLOR:
        case GL_ACCUM_CLEAR_VALUE:
        case GL_CURRENT_RASTER_POSITION:
        case GL_CURRENT_RASTER_COLOR:
        case GL_CURRENT_RASTER_TEXTURE_COORDS:
            return 4;
        case GL_CURRENT_NORMAL:
        case GL_POINT_DISTANCE_ATTENUATION:
            return 3;
        case GL_MAP2_GRID_DOMAIN:
            return 4;
        case GL_DEPTH_RANGE:
        case GL_MAX_VIEWPORT_DIMS:
        case GL_POLYGON_MODE:
        case GL_MAP1_GRID_DOMAIN:
        case GL_MAP2_GRID_SEGMENTS:
        case GL_POINT_SIZE_RANGE:
        case GL_LINE_WIDTH_RANGE:
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_ALIASED_LINE_WIDTH_RANGE:
            return 2;
        default:
            return 1;
    }
}

/* A float state value asked for as an integer: rounded to the nearest, which is the
 * specification's conversion for everything that is not a normalised colour. */
static GLint gl_round_to_int(GLfloat x) {
    if (!(x == x)) return 0; /* NaN */
    if (x >= 2147483647.0f) return 2147483647;
    if (x <= -2147483648.0f) return (GLint)(-2147483647 - 1);
    return (GLint)(x < 0.0f ? x - 0.5f : x + 0.5f);
}

/* Mesa's FLOAT_TO_INT (main/macros.h:111), `(GLint)(2147483647.0 * X)`. Clamped to [-1, 1]
 * first: the current colour is not clamped when it is set, and converting an out-of-range
 * double to int is undefined in C rather than merely saturating. */
static GLint gl_float_to_int_color(GLfloat x) {
    if (!(x >= -1.0f)) x = -1.0f; /* also NaN */
    if (x > 1.0f) x = 1.0f;
    return (GLint)(2147483647.0 * (double)x);
}

/* The float-only states glGetIntegerv answers through glGetFloatv, mapped linearly rather than
 * rounded: GL 1.5's 6.1.2 names colour components, depth values and normals, and these are
 * Mesa's TYPE_FLOATN / TYPE_DOUBLEN rows among them (main/get_hash_params.py:10, :159, :163,
 * :175, :786). */
static GLboolean gl_get_normalised(GLenum pname) {
    switch (pname) {
        case GL_ALPHA_TEST_REF:
        case GL_CURRENT_NORMAL:
        case GL_CURRENT_RASTER_COLOR:
        case GL_DEPTH_CLEAR_VALUE:
        case GL_LIGHT_MODEL_AMBIENT:
            return GL_TRUE;
        default:
            return GL_FALSE;
    }
}

/* Nonzero while glGetIntegerv is asking glGetFloatv, whose own fallback asks glGetIntegerv: a
 * pname neither knows then stops at the second step instead of recursing. */
static int s_get_float_fallback;

void glGetIntegerv(GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    switch (pname) {
        case GL_VIEWPORT:
            params[0] = ctx->vp_x;
            params[1] = ctx->vp_y;
            params[2] = ctx->vp_w;
            params[3] = ctx->vp_h;
            break;
        case GL_SCISSOR_BOX:
            params[0] = ctx->sc_x;
            params[1] = ctx->sc_y;
            params[2] = ctx->sc_w;
            params[3] = ctx->sc_h;
            break;
        case GL_MATRIX_MODE:
            params[0] = (GLint)ctx->matrix_mode;
            break;
        case GL_TEXTURE_BINDING_2D:
            params[0] = (GLint)gl_tu(ctx)->bound_texture_2d;
            break;
        case GL_FRAMEBUFFER_BINDING:
            params[0] = (GLint)ctx->bound_framebuffer;
            break;
        case GL_RENDERBUFFER_BINDING:
            params[0] = (GLint)ctx->bound_renderbuffer;
            break;
        case GL_MAX_RENDERBUFFER_SIZE:
            params[0] = (GLint)OOPS_GL_MAX_TEXTURE_SIZE;
            break;
        /* ES 2.0's shader-compiler queries. This GL compiles online and has no binary format,
         * so the pair is GL_TRUE and zero - and the zero is what makes glShaderBinary's refusal
         * the correct answer rather than an arbitrary one. */
        case GL_SHADER_COMPILER:
            params[0] = (GLint)GL_TRUE;
            break;
        case GL_NUM_SHADER_BINARY_FORMATS:
            params[0] = 0;
            break;
        case GL_TEXTURE_BINDING_1D:
            params[0] = (GLint)gl_tu(ctx)->bound_texture_1d;
            break;
        case GL_TEXTURE_BINDING_CUBE_MAP:
            params[0] = (GLint)gl_tu(ctx)->bound_texture_cube;
            break;
        case GL_MAX_CUBE_MAP_TEXTURE_SIZE: params[0] = OOPS_GL_MAX_CUBE_MAP_TEXTURE_SIZE; break;
        case GL_TEXTURE_BINDING_3D:
            params[0] = (GLint)gl_tu(ctx)->bound_texture_3d;
            break;
        case GL_MAX_3D_TEXTURE_SIZE:   params[0] = OOPS_GL_MAX_3D_TEXTURE_SIZE; break;
        /* The pixel-store state, every parameter of it - none was answered until 2026-09-19,
         * GL_UNPACK_ALIGNMENT included, so a helper that saves and restores it by hand read an
         * error instead. */
        case GL_UNPACK_ALIGNMENT:      params[0] = ctx->unpack_alignment; break;
        case GL_UNPACK_ROW_LENGTH:     params[0] = ctx->unpack_row_length; break;
        case GL_UNPACK_IMAGE_HEIGHT:   params[0] = ctx->unpack_image_height; break;
        case GL_UNPACK_SKIP_ROWS:      params[0] = ctx->unpack_skip_rows; break;
        case GL_UNPACK_SKIP_PIXELS:    params[0] = ctx->unpack_skip_pixels; break;
        case GL_UNPACK_SKIP_IMAGES:    params[0] = ctx->unpack_skip_images; break;
        case GL_UNPACK_SWAP_BYTES:     params[0] = ctx->unpack_swap_bytes ? 1 : 0; break;
        case GL_UNPACK_LSB_FIRST:      params[0] = ctx->unpack_lsb_first ? 1 : 0; break;
        case GL_PACK_ALIGNMENT:        params[0] = ctx->pack_alignment; break;
        case GL_PACK_ROW_LENGTH:       params[0] = ctx->pack_row_length; break;
        case GL_PACK_IMAGE_HEIGHT:     params[0] = ctx->pack_image_height; break;
        case GL_PACK_SKIP_ROWS:        params[0] = ctx->pack_skip_rows; break;
        case GL_PACK_SKIP_PIXELS:      params[0] = ctx->pack_skip_pixels; break;
        case GL_PACK_SKIP_IMAGES:      params[0] = ctx->pack_skip_images; break;
        case GL_PACK_SWAP_BYTES:       params[0] = ctx->pack_swap_bytes ? 1 : 0; break;
        case GL_PACK_LSB_FIRST:        params[0] = ctx->pack_lsb_first ? 1 : 0; break;
        case GL_BLEND_SRC:
        case GL_BLEND_SRC_RGB:
            params[0] = (GLint)ctx->blend_src;
            break;
        case GL_BLEND_DST:
        case GL_BLEND_DST_RGB:
            params[0] = (GLint)ctx->blend_dst;
            break;
        case GL_BLEND_SRC_ALPHA:
            params[0] = (GLint)ctx->blend_src_alpha;
            break;
        case GL_BLEND_DST_ALPHA:
            params[0] = (GLint)ctx->blend_dst_alpha;
            break;
        case GL_BLEND_EQUATION:
            params[0] = (GLint)ctx->blend_equation;
            break;
        case GL_LOGIC_OP_MODE:
            params[0] = (GLint)ctx->logic_op;
            break;
        /* Colours as integers map [0, 1] onto [0, INT_MAX] - Mesa's FLOAT_TO_INT
         * (main/macros.h:111), which get.c applies to every TYPE_FLOATN_4 query
         * (get.c:2084-2089). A plain cast would answer 0 for anything short of full intensity. */
        case GL_BLEND_COLOR:
            for (int i = 0; i < 4; i++) params[i] = gl_float_to_int_color(ctx->blend_color[i]);
            break;
        case GL_COLOR_CLEAR_VALUE:
            for (int i = 0; i < 4; i++) params[i] = gl_float_to_int_color(ctx->clear_color[i]);
            break;
        case GL_CURRENT_COLOR:
            for (int i = 0; i < 4; i++) params[i] = gl_float_to_int_color(ctx->cur_color[i]);
            break;
        case GL_CURRENT_SECONDARY_COLOR:
            for (int i = 0; i < 4; i++) params[i] = gl_float_to_int_color(ctx->cur_secondary[i]);
            break;
        case GL_FOG_COLOR:
            for (int i = 0; i < 4; i++) params[i] = gl_float_to_int_color(ctx->fog_color[i]);
            break;
        case GL_TEXTURE_ENV_MODE:
            params[0] = (GLint)gl_tu(ctx)->tex_env_mode;
            break;

        /* The implementation limits. A program reads these to size its own work - how many
         * lights to set up, how deep it may nest glPushMatrix - so returning zero, which is
         * what the silent default below used to do, makes a correct program refuse to run. */
        case GL_MAX_LIGHTS:
            params[0] = OOPS_GL_LIGHT_COUNT;
            break;
        case GL_MAX_MODELVIEW_STACK_DEPTH:
            params[0] = OOPS_GL_MODELVIEW_STACK_CAPACITY;
            break;
        case GL_MAX_PROJECTION_STACK_DEPTH:
            params[0] = OOPS_GL_PROJECTION_STACK_CAPACITY;
            break;
        case GL_MAX_TEXTURE_STACK_DEPTH:
            params[0] = OOPS_GL_TEXTURE_STACK_CAPACITY;
            break;
        case GL_MAX_ATTRIB_STACK_DEPTH:
            params[0] = OOPS_GL_ATTRIB_STACK_CAPACITY;
            break;
        case GL_MAX_CLIENT_ATTRIB_STACK_DEPTH:
            params[0] = OOPS_GL_CLIENT_ATTRIB_STACK_CAPACITY;
            break;
        /* The widest rectangle the framebuffer copy path can carry a row of, which is the real
         * bound on a texture here - not a number picked to look generous. */
        case GL_MAX_TEXTURE_SIZE:
            params[0] = OOPS_GL_MAX_TEXTURE_SIZE;
            break;
        /* **Two, GL 1.3's minimum** (section 2.6, table 6.29). This answered 1 until 2026-09-19 -
         * honestly, with glActiveTexture refusing a second unit, but short of what GL 1.3
         * requires. The console applies unit 0 alone, and logs once when a draw uses more. */
        case GL_MAX_TEXTURE_UNITS:
            params[0] = OOPS_GL_MAX_TEXTURE_UNITS;
            break;
        case GL_MAX_CLIP_PLANES:
            params[0] = OOPS_GL_CLIP_PLANE_COUNT;
            break;

        /* -----------------------------------------------------------------
         * GL 2.0's limits
         *
         * Every one is this implementation's own number and every one meets the
         * specification's minimum. **None of them is rounded up to look generous**: a program
         * that asks GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS and is told 4 will take a path that then
         * samples nothing, where being told 0 sends it down the one that works.
         * ----------------------------------------------------------------- */
        case GL_MAX_VERTEX_ATTRIBS:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = OOPS_GL_MAX_VERTEX_ATTRIBS;   /* 16, GL 2.0's minimum */
            break;
        case GL_MAX_VARYING_FLOATS:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = OOPS_GL_MAX_VARYING_FLOATS;   /* 32, the minimum: eight vec4 slots */
            break;
        /* The samplers a fragment shader may use, which is the texture units that exist. */
        case GL_MAX_TEXTURE_IMAGE_UNITS:
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
        case GL_MAX_TEXTURE_COORDS:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = OOPS_GL_MAX_TEXTURE_UNITS;
            break;
        /* **Zero, which is legal and is true.** GL 2.0's minimum is 0 and a vertex shader here
         * has no sampler - the vertex stage on this hardware is not wired to the texture
         * pipe. */
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = 0;
            break;
        /* The uniform storage a stage may declare, in floats. A mat4 is sixteen of them. */
        case GL_MAX_VERTEX_UNIFORM_COMPONENTS:
        case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = OOPS_GL_MAX_PROGRAM_UNIFORMS * 4;
            break;
        /* One colour buffer per `glDrawBuffers` entry. Two exist - the front surface and the
         * back one - and both receive the same fragment colour, which is what the call means
         * for a window-system framebuffer. */
        case GL_MAX_DRAW_BUFFERS:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = 2;
            break;
        case GL_CURRENT_PROGRAM:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = (GLint)ctx->program_current;
            break;
        /* **Zero, and the list that follows it is empty.** The specification allows the set of
         * compressed formats to be empty and expects a program to ask; answering honestly is
         * what sends it down its uncompressed path instead of into a refusal it did not plan
         * for. GL_COMPRESSED_TEXTURE_FORMATS writes nothing, because there is nothing. */
        case GL_NUM_COMPRESSED_TEXTURE_FORMATS:
            params[0] = 0;
            break;
        case GL_COMPRESSED_TEXTURE_FORMATS:
            break;
        case GL_CURRENT_RASTER_POSITION_VALID: params[0] = ctx->raster_valid ? 1 : 0; break;
        case GL_FOG_MODE:            params[0] = (GLint)ctx->fog_mode; break;
        case GL_FOG_COORD_SRC:       params[0] = (GLint)ctx->fog_coord_src; break;
        /* Floats asked for as integers, rounded (Mesa get.c's TYPE_FLOAT is IROUND). */
        case GL_FOG_INDEX:           params[0] = gl_round_to_int(ctx->fog_index); break;
        case GL_CURRENT_FOG_COORD:   params[0] = gl_round_to_int(ctx->cur_fog_coord); break;
        case GL_FOG_COORD_ARRAY_TYPE:   params[0] = (GLint)ctx->array_fog_coord.type; break;
        case GL_FOG_COORD_ARRAY_STRIDE: params[0] = ctx->array_fog_coord.stride; break;
        case GL_FOG_COORD_ARRAY_BUFFER_BINDING: params[0] = (GLint)ctx->array_fog_coord.buffer; break;
        /* The distances as plain integers; the colour normalised back, the inverse of glFogiv. */
        case GL_FOG_START:           params[0] = (GLint)ctx->fog_start; break;
        case GL_FOG_END:             params[0] = (GLint)ctx->fog_end; break;
        case GL_FOG_DENSITY:         params[0] = (GLint)ctx->fog_density; break;
        case GL_STENCIL_BITS:        params[0] = 8; break;
        case GL_STENCIL_FUNC:        params[0] = (GLint)ctx->stencil_func; break;
        case GL_STENCIL_REF:         params[0] = ctx->stencil_ref; break;
        case GL_STENCIL_VALUE_MASK:  params[0] = (GLint)ctx->stencil_value_mask; break;
        case GL_STENCIL_WRITEMASK:   params[0] = (GLint)ctx->stencil_writemask; break;
        case GL_STENCIL_FAIL:        params[0] = (GLint)ctx->stencil_fail; break;
        case GL_STENCIL_PASS_DEPTH_FAIL: params[0] = (GLint)ctx->stencil_zfail; break;
        case GL_STENCIL_PASS_DEPTH_PASS: params[0] = (GLint)ctx->stencil_zpass; break;
        /* GL 2.0's back face. Answered even by a context nothing has split, where they read the
         * same as the front ones - which is what makes them safe to query unconditionally. */
        case GL_STENCIL_BACK_FUNC:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = (GLint)ctx->stencil_back_func;
            break;
        case GL_STENCIL_BACK_REF:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = ctx->stencil_back_ref;
            break;
        case GL_STENCIL_BACK_VALUE_MASK:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = (GLint)ctx->stencil_back_value_mask;
            break;
        case GL_STENCIL_BACK_WRITEMASK:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = (GLint)ctx->stencil_back_writemask;
            break;
        case GL_STENCIL_BACK_FAIL:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = (GLint)ctx->stencil_back_fail;
            break;
        case GL_STENCIL_BACK_PASS_DEPTH_FAIL:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = (GLint)ctx->stencil_back_zfail;
            break;
        case GL_STENCIL_BACK_PASS_DEPTH_PASS:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = (GLint)ctx->stencil_back_zpass;
            break;
        /* **GL_BLEND_EQUATION_RGB has the same value as GL_BLEND_EQUATION**, which is why there
         * is no case for it: the 2.0 name is the 1.4 name, renamed rather than added, so it is
         * answered by the 1.4 one and is gated with it. */
        case GL_BLEND_EQUATION_ALPHA:
            if (!gl_require_version_enum(ctx, 2u, 0u)) break;
            params[0] = (GLint)ctx->blend_equation_alpha;
            break;
        case GL_STENCIL_CLEAR_VALUE: params[0] = ctx->clear_stencil; break;
        case GL_ACTIVE_TEXTURE:
            params[0] = (GLint)(GL_TEXTURE0 + ctx->active_texture);
            break;
        case GL_CLIENT_ACTIVE_TEXTURE:
            params[0] = (GLint)(GL_TEXTURE0 + ctx->client_active_texture);
            break;
        /* **The framebuffer's own extent, which is the honest answer.** There is one surface and
         * it is this size; a viewport larger than it has nothing to rasterise into. */
        case GL_MAX_VIEWPORT_DIMS:
            params[0] = (GLint)ctx->width;
            params[1] = (GLint)ctx->height;
            break;

        /* Where the stacks currently stand. `*_depth` counts the *saved* frames, and GL counts
         * the current matrix as one of them, so the matrix stacks report one more. */
        case GL_ATTRIB_STACK_DEPTH:
            params[0] = (GLint)ctx->attrib_depth;
            break;
        case GL_CLIENT_ATTRIB_STACK_DEPTH:
            params[0] = (GLint)ctx->client_attrib_depth;
            break;
        case GL_MODELVIEW_STACK_DEPTH:
            params[0] = ctx->modelview_depth + 1;
            break;
        case GL_PROJECTION_STACK_DEPTH:
            params[0] = ctx->projection_depth + 1;
            break;
        /* The active unit's (GL 1.3). Not answered at all - nor its enum declared - until
         * 2026-09-19, though the two above were. */
        case GL_TEXTURE_STACK_DEPTH:
            params[0] = gl_tu(ctx)->texture_depth + 1;
            break;
        /* **Ten table names nothing answered** (nor declared) until 2026-09-19, found by querying
         * every name in GL 1.5's state tables through every getter. Mesa's answers where they are
         * constants (main/get_hash_params.py:779, :788, :846; config.h:131, :134). */
        case GL_LIST_INDEX:          params[0] = (GLint)ctx->list_compiling; break;
        case GL_LIST_MODE:           params[0] = ctx->list_compiling ? (GLint)ctx->list_mode : 0; break;
        case GL_MAX_LIST_NESTING:    params[0] = GL_MAX_LIST_DEPTH; break;
        case GL_CURRENT_RASTER_INDEX: params[0] = 1; break; /* an RGBA context's, as Mesa's */
        case GL_AUX_BUFFERS:         params[0] = 0; break;
        case GL_DOUBLEBUFFER:        params[0] = 1; break; /* glSwapBuffers flips a back buffer */
        case GL_STEREO:              params[0] = 0; break;
        case GL_SUBPIXEL_BITS:       params[0] = 4; break; /* GL's minimum, Mesa's SUB_PIXEL_BITS */
        /* Recommendations for glDrawRangeElements, not limits - it has none here. The
         * immediate-mode buffer's size, the one bound glBegin/glEnd and glArrayElement meet. */
        case GL_MAX_ELEMENTS_VERTICES:
        case GL_MAX_ELEMENTS_INDICES:
            params[0] = OOPS_GL_MAX_IMMEDIATE_VERTS;
            break;

        case GL_DEPTH_FUNC:
            params[0] = (GLint)ctx->depth_func;
            break;
        case GL_DEPTH_WRITEMASK:
            params[0] = ctx->depth_mask ? 1 : 0;
            break;
        case GL_CULL_FACE_MODE:
            params[0] = (GLint)ctx->cull_mode;
            break;
        case GL_FRONT_FACE:
            params[0] = (GLint)ctx->front_face;
            break;
        case GL_SHADE_MODEL:
            params[0] = (GLint)ctx->shade_model;
            break;
        /* The light model's scalars, neither answered until 2026-09-19. */
        case GL_LIGHT_MODEL_LOCAL_VIEWER:
            params[0] = ctx->light_model_local_viewer ? 1 : 0;
            break;
        case GL_LIGHT_MODEL_COLOR_CONTROL:
            params[0] = (GLint)ctx->light_model_color_control;
            break;
        case GL_LIGHT_MODEL_TWO_SIDE:
            params[0] = ctx->light_model_two_side ? 1 : 0;
            break;
        case GL_COLOR_MATERIAL_FACE:
            params[0] = (GLint)ctx->color_material_face;
            break;
        case GL_COLOR_MATERIAL_PARAMETER:
            params[0] = (GLint)ctx->color_material_mode;
            break;
        case GL_ALPHA_TEST_FUNC:
            params[0] = (GLint)ctx->alpha_func;
            break;
        case GL_LIST_BASE:
            params[0] = (GLint)ctx->list_base;
            break;
        case GL_PERSPECTIVE_CORRECTION_HINT:
            params[0] = (GLint)ctx->perspective_hint;
            break;
        /* As glDrawBuffer and glReadBuffer last set them - GL_BACK, GL_BACK_LEFT, or for drawing
         * GL_NONE. */
        case GL_DRAW_BUFFER:
            params[0] = (GLint)ctx->draw_buffer;
            break;
        case GL_READ_BUFFER:
            params[0] = (GLint)ctx->read_buffer;
            break;
        case GL_ARRAY_BUFFER_BINDING:
            params[0] = (GLint)ctx->bound_array_buffer;
            break;
        case GL_ELEMENT_ARRAY_BUFFER_BINDING:
            params[0] = (GLint)ctx->bound_element_array_buffer;
            break;

        /* The remaining hints, colour-index state and multisampling - state only, all of it. */
        case GL_POINT_SMOOTH_HINT:        params[0] = (GLint)ctx->hint_point_smooth; break;
        case GL_LINE_SMOOTH_HINT:         params[0] = (GLint)ctx->hint_line_smooth; break;
        case GL_POLYGON_SMOOTH_HINT:      params[0] = (GLint)ctx->hint_polygon_smooth; break;
        case GL_FOG_HINT:                 params[0] = (GLint)ctx->hint_fog; break;
        case GL_TEXTURE_COMPRESSION_HINT: params[0] = (GLint)ctx->hint_texture_compression; break;
        case GL_GENERATE_MIPMAP_HINT:     params[0] = (GLint)ctx->hint_generate_mipmap; break;
        /* GL 1.4's bias limit, a float asked for as an integer. */
        case GL_MAX_TEXTURE_LOD_BIAS:     params[0] = (GLint)OOPS_GL_MAX_TEXTURE_LOD_BIAS; break;
        case GL_CURRENT_INDEX:            params[0] = gl_round_to_int(ctx->cur_index); break;
        case GL_INDEX_CLEAR_VALUE:        params[0] = gl_round_to_int(ctx->clear_index); break;
        case GL_POINT_SIZE:               params[0] = gl_round_to_int(ctx->point_size); break;
        case GL_POINT_SIZE_MIN:           params[0] = gl_round_to_int(ctx->point_size_min); break;
        case GL_POINT_SIZE_MAX:           params[0] = gl_round_to_int(ctx->point_size_max); break;
        case GL_POINT_FADE_THRESHOLD_SIZE: params[0] = gl_round_to_int(ctx->point_fade_threshold); break;
        /* An enum, so it reads back as itself rather than through the rounding above. */
        case GL_POINT_SPRITE_COORD_ORIGIN: params[0] = (GLint)ctx->point_sprite_origin; break;
        case GL_POINT_DISTANCE_ATTENUATION:
            for (int i = 0; i < 3; i++) params[i] = gl_round_to_int(ctx->point_atten[i]);
            break;
        case GL_LINE_WIDTH:               params[0] = gl_round_to_int(ctx->line_width); break;
        case GL_POINT_SIZE_RANGE:
        case GL_LINE_WIDTH_RANGE:
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_ALIASED_LINE_WIDTH_RANGE:
            params[0] = 1;
            params[1] = OOPS_GL_MAX_POINT_LINE_SIZE;
            break;
        case GL_POINT_SIZE_GRANULARITY:
        case GL_LINE_WIDTH_GRANULARITY:
            /* The smooth step, 1/8, rounded as an integer query rounds every float: 0. */
            params[0] = gl_round_to_int(OOPS_GL_SMOOTH_GRANULARITY);
            break;
        case GL_INDEX_WRITEMASK:          params[0] = (GLint)ctx->index_mask; break;
        case GL_INDEX_MODE:               params[0] = 0; break; /* an RGBA context */
        case GL_RGBA_MODE:                params[0] = 1; break;
        case GL_INDEX_BITS:               params[0] = 0; break;
        case GL_INDEX_ARRAY_TYPE:         params[0] = (GLint)ctx->array_index.type; break;
        case GL_INDEX_ARRAY_STRIDE:       params[0] = ctx->array_index.stride; break;
        case GL_INDEX_ARRAY_BUFFER_BINDING: params[0] = (GLint)ctx->array_index.buffer; break;
        case GL_SAMPLE_BUFFERS:           params[0] = 0; break; /* no multisample buffer */
        case GL_SAMPLES:                  params[0] = 0; break;
        case GL_SAMPLE_COVERAGE_INVERT:   params[0] = ctx->sample_coverage_invert ? 1 : 0; break;
        case GL_SAMPLE_COVERAGE_VALUE:    params[0] = gl_round_to_int(ctx->sample_coverage_value); break;

        case GL_POLYGON_MODE: /* two values: front, then back */
            params[0] = (GLint)ctx->polygon_mode[0];
            params[1] = (GLint)ctx->polygon_mode[1];
            break;
        case GL_EDGE_FLAG:
            params[0] = ctx->cur_edge_flag ? 1 : 0;
            break;

        /* **Each array's description.** None of these was answered until 2026-09-19 - they were
         * refused as unknown - though every one is state glVertexPointer and its siblings set,
         * and a program that saves and restores arrays by hand reads them. */
        case GL_VERTEX_ARRAY_SIZE:           params[0] = ctx->array_vertex.size; break;
        case GL_VERTEX_ARRAY_TYPE:           params[0] = (GLint)ctx->array_vertex.type; break;
        case GL_VERTEX_ARRAY_STRIDE:         params[0] = ctx->array_vertex.stride; break;
        case GL_NORMAL_ARRAY_TYPE:           params[0] = (GLint)ctx->array_normal.type; break;
        case GL_NORMAL_ARRAY_STRIDE:         params[0] = ctx->array_normal.stride; break;
        case GL_COLOR_ARRAY_SIZE:            params[0] = ctx->array_color.size; break;
        case GL_COLOR_ARRAY_TYPE:            params[0] = (GLint)ctx->array_color.type; break;
        case GL_COLOR_ARRAY_STRIDE:          params[0] = ctx->array_color.stride; break;
        case GL_TEXTURE_COORD_ARRAY_SIZE:    params[0] = ctx->array_texcoord[ctx->client_active_texture].size; break;
        case GL_TEXTURE_COORD_ARRAY_TYPE:    params[0] = (GLint)ctx->array_texcoord[ctx->client_active_texture].type; break;
        case GL_TEXTURE_COORD_ARRAY_STRIDE:  params[0] = ctx->array_texcoord[ctx->client_active_texture].stride; break;
        case GL_EDGE_FLAG_ARRAY_STRIDE:      params[0] = ctx->array_edge_flag.stride; break;
        case GL_VERTEX_ARRAY_BUFFER_BINDING:        params[0] = (GLint)ctx->array_vertex.buffer; break;
        case GL_NORMAL_ARRAY_BUFFER_BINDING:        params[0] = (GLint)ctx->array_normal.buffer; break;
        case GL_COLOR_ARRAY_BUFFER_BINDING:         params[0] = (GLint)ctx->array_color.buffer; break;
        case GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING: params[0] = (GLint)ctx->array_texcoord[ctx->client_active_texture].buffer; break;
        case GL_EDGE_FLAG_ARRAY_BUFFER_BINDING:     params[0] = (GLint)ctx->array_edge_flag.buffer; break;
        case GL_SECONDARY_COLOR_ARRAY_SIZE:   params[0] = ctx->array_secondary.size; break;
        case GL_SECONDARY_COLOR_ARRAY_TYPE:   params[0] = (GLint)ctx->array_secondary.type; break;
        case GL_SECONDARY_COLOR_ARRAY_STRIDE: params[0] = ctx->array_secondary.stride; break;
        case GL_SECONDARY_COLOR_ARRAY_BUFFER_BINDING: params[0] = (GLint)ctx->array_secondary.buffer; break;

        /* The evaluator grid. The domains are floats, rounded here like every non-colour float
         * asked for as an integer. */
        /* The buffers' sizes: eight bits a colour channel, the 32-bit float depth buffer both
         * paths keep (DB_Z_INFO's Z_32_FLOAT), eight stencil bits (above), sixteen-bit
         * accumulation channels. None of these was answered until 2026-09-19. */
        case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS:
            params[0] = 8;
            break;
        case GL_DEPTH_BITS:             params[0] = 32; break;
        case GL_ACCUM_RED_BITS: case GL_ACCUM_GREEN_BITS:
        case GL_ACCUM_BLUE_BITS: case GL_ACCUM_ALPHA_BITS:
            params[0] = 16;
            break;
        case GL_ACCUM_CLEAR_VALUE:
            for (int i = 0; i < 4; i++) params[i] = gl_float_to_int_color(ctx->accum_clear[i]);
            break;
        case GL_LINE_STIPPLE_PATTERN:   params[0] = (GLint)ctx->line_stipple_pattern; break;
        case GL_LINE_STIPPLE_REPEAT:    params[0] = ctx->line_stipple_factor; break;

        /* Pixel transfer. The scales and biases are floats, rounded here. */
        case GL_MAP_COLOR:              params[0] = ctx->map_color ? 1 : 0; break;
        case GL_MAP_STENCIL:            params[0] = ctx->map_stencil ? 1 : 0; break;
        case GL_INDEX_SHIFT:            params[0] = ctx->index_shift; break;
        case GL_INDEX_OFFSET:           params[0] = ctx->index_offset; break;
        case GL_RED_SCALE:              params[0] = gl_round_to_int(ctx->pixel_scale[0]); break;
        case GL_GREEN_SCALE:            params[0] = gl_round_to_int(ctx->pixel_scale[1]); break;
        case GL_BLUE_SCALE:             params[0] = gl_round_to_int(ctx->pixel_scale[2]); break;
        case GL_ALPHA_SCALE:            params[0] = gl_round_to_int(ctx->pixel_scale[3]); break;
        case GL_RED_BIAS:               params[0] = gl_round_to_int(ctx->pixel_bias[0]); break;
        case GL_GREEN_BIAS:             params[0] = gl_round_to_int(ctx->pixel_bias[1]); break;
        case GL_BLUE_BIAS:              params[0] = gl_round_to_int(ctx->pixel_bias[2]); break;
        case GL_ALPHA_BIAS:             params[0] = gl_round_to_int(ctx->pixel_bias[3]); break;
        case GL_DEPTH_SCALE:            params[0] = gl_round_to_int(ctx->depth_scale); break;
        case GL_DEPTH_BIAS:             params[0] = gl_round_to_int(ctx->depth_bias); break;
        case GL_MAX_PIXEL_MAP_TABLE:    params[0] = OOPS_GL_MAX_PIXEL_MAP_TABLE; break;
        case GL_PIXEL_MAP_I_TO_I_SIZE: case GL_PIXEL_MAP_S_TO_S_SIZE:
        case GL_PIXEL_MAP_I_TO_R_SIZE: case GL_PIXEL_MAP_I_TO_G_SIZE:
        case GL_PIXEL_MAP_I_TO_B_SIZE: case GL_PIXEL_MAP_I_TO_A_SIZE:
        case GL_PIXEL_MAP_R_TO_R_SIZE: case GL_PIXEL_MAP_G_TO_G_SIZE:
        case GL_PIXEL_MAP_B_TO_B_SIZE: case GL_PIXEL_MAP_A_TO_A_SIZE:
            params[0] = ctx->pixel_map_size[pname - GL_PIXEL_MAP_I_TO_I_SIZE];
            break;

        /* Selection and feedback. */
        case GL_RENDER_MODE:            params[0] = (GLint)ctx->render_mode; break;
        case GL_NAME_STACK_DEPTH:       params[0] = (GLint)ctx->name_depth; break;
        case GL_MAX_NAME_STACK_DEPTH:   params[0] = OOPS_GL_MAX_NAME_STACK_DEPTH; break;
        case GL_SELECTION_BUFFER_SIZE:  params[0] = ctx->select_size; break;
        case GL_FEEDBACK_BUFFER_SIZE:   params[0] = ctx->feedback_size; break;
        case GL_FEEDBACK_BUFFER_TYPE:   params[0] = (GLint)ctx->feedback_type; break;

        case GL_MAX_EVAL_ORDER:      params[0] = OOPS_GL_MAX_EVAL_ORDER; break;
        case GL_MAP1_GRID_SEGMENTS:  params[0] = ctx->grid1_un; break;
        case GL_MAP2_GRID_SEGMENTS:
            params[0] = ctx->grid2_un;
            params[1] = ctx->grid2_vn;
            break;
        case GL_MAP1_GRID_DOMAIN:
            params[0] = gl_round_to_int(ctx->grid1_u1);
            params[1] = gl_round_to_int(ctx->grid1_u2);
            break;
        case GL_MAP2_GRID_DOMAIN:
            params[0] = gl_round_to_int(ctx->grid2_u1);
            params[1] = gl_round_to_int(ctx->grid2_u2);
            params[2] = gl_round_to_int(ctx->grid2_v1);
            params[3] = gl_round_to_int(ctx->grid2_v2);
            break;

        default: {
            /* **An enable is a query too.** GL answers every capability glIsEnabled knows through
             * each glGet as well - glGetIntegerv(GL_DEPTH_TEST) is 1 or 0 - and until 2026-09-19
             * only glGetBooleanv did; the integer, float and double forms refused them all. */
            const GLenum before = ctx->last_error;
            ctx->last_error = GL_NO_ERROR;
            const GLboolean enabled = glIsEnabled(pname);
            if (ctx->last_error == GL_NO_ERROR) {
                ctx->last_error = before;
                params[0] = enabled ? 1 : 0;
                break;
            }
            ctx->last_error = before;
            /* **So is a float-valued state** (GL 1.5, 6.1.2). The raster position, colour and
             * distance, the alpha reference, the current normal, the depth clear value, the light
             * model's ambient colour, the polygon offset and the pixel zoom - eleven - answered
             * glGetFloatv and refused this until 2026-09-19, found by querying every name in the
             * specification's state tables through every getter. Rounded, or mapped linearly for
             * the normalised ones. */
            if (s_get_float_fallback == 0) {
                GLfloat fv[16];
                s_get_float_fallback++;
                ctx->last_error = GL_NO_ERROR;
                glGetFloatv(pname, fv);
                s_get_float_fallback--;
                if (ctx->last_error == GL_NO_ERROR) {
                    ctx->last_error = before;
                    const int n = gl_query_element_count(pname);
                    const GLboolean norm = gl_get_normalised(pname);
                    for (int i = 0; i < n; i++) {
                        params[i] = norm ? gl_float_to_int_color(fv[i]) : gl_round_to_int(fv[i]);
                    }
                    break;
                }
                ctx->last_error = before;
            }
            /* **Refused, not ignored.** An ignored query leaves the caller's buffer holding
             * whatever it held before and raises nothing, so the program reads stack garbage
             * that looks like an answer. */
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
        }
    }
}

void glGetFloatv(GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    switch (pname) {
        case GL_CURRENT_RASTER_POSITION:
            for (int i = 0; i < 4; i++) params[i] = ctx->raster_pos[i];
            break;
        case GL_CURRENT_RASTER_COLOR:
            for (int i = 0; i < 4; i++) params[i] = ctx->raster_color[i];
            break;
        case GL_CURRENT_RASTER_TEXTURE_COORDS:
            for (int i = 0; i < 4; i++) params[i] = ctx->raster_texcoord[ctx->active_texture][i];
            break;
        case GL_CURRENT_RASTER_DISTANCE: params[0] = ctx->raster_distance; break;
        case GL_CURRENT_RASTER_POSITION_VALID:
            params[0] = ctx->raster_valid ? 1.0f : 0.0f;
            break;
        case GL_ZOOM_X: params[0] = ctx->pixel_zoom_x; break;
        case GL_ZOOM_Y: params[0] = ctx->pixel_zoom_y; break;
        case GL_FOG_DENSITY: params[0] = ctx->fog_density; break;
        case GL_FOG_START:   params[0] = ctx->fog_start; break;
        case GL_FOG_END:     params[0] = ctx->fog_end; break;
        case GL_FOG_COLOR:
            for (int i = 0; i < 4; i++) params[i] = ctx->fog_color[i];
            break;
        case GL_FOG_MODE:    params[0] = (GLfloat)ctx->fog_mode; break;
        case GL_FOG_INDEX:   params[0] = ctx->fog_index; break;
        case GL_CURRENT_FOG_COORD: params[0] = ctx->cur_fog_coord; break;
        case GL_MODELVIEW_MATRIX:
            for (int i = 0; i < 16; i++) {
                params[i] = ctx->modelview_stack[ctx->modelview_depth].m[i];
            }
            break;
        case GL_PROJECTION_MATRIX:
            for (int i = 0; i < 16; i++) {
                params[i] = ctx->projection_stack[ctx->projection_depth].m[i];
            }
            break;
        case GL_TEXTURE_MATRIX:
            for (int i = 0; i < 16; i++) {
                params[i] = gl_tu(ctx)->texture_stack[gl_tu(ctx)->texture_depth].m[i];
            }
            break;
        /* GL 1.3's row-major views of the same three, which nothing answered (nor declared)
         * until 2026-09-19 though glLoadTransposeMatrix existed. Element (row, col) of the
         * column-major store is m[col * 4 + row]. */
        case GL_TRANSPOSE_MODELVIEW_MATRIX:
        case GL_TRANSPOSE_PROJECTION_MATRIX:
        case GL_TRANSPOSE_TEXTURE_MATRIX: {
            const gl_mat4_t *m = (pname == GL_TRANSPOSE_MODELVIEW_MATRIX)
                                     ? &ctx->modelview_stack[ctx->modelview_depth]
                               : (pname == GL_TRANSPOSE_PROJECTION_MATRIX)
                                     ? &ctx->projection_stack[ctx->projection_depth]
                                     : &gl_tu(ctx)->texture_stack[gl_tu(ctx)->texture_depth];
            for (int i = 0; i < 16; i++) params[i] = m->m[(i % 4) * 4 + i / 4];
            break;
        }

        case GL_CURRENT_COLOR:
            for (int i = 0; i < 4; i++) params[i] = ctx->cur_color[i];
            break;
        case GL_CURRENT_SECONDARY_COLOR:
            for (int i = 0; i < 4; i++) params[i] = ctx->cur_secondary[i];
            break;
        case GL_CURRENT_NORMAL:
            for (int i = 0; i < 3; i++) params[i] = ctx->cur_normal[i];
            break;
        case GL_CURRENT_TEXTURE_COORDS:
            /* All four. This answered the constants 0 and 1 for r and q from when only s and t
             * were tracked, and went on doing it after glTexCoord3/4 started setting them - so a
             * program reading back a projective coordinate got q = 1 whatever it had set. */
            for (int i = 0; i < 4; i++) params[i] = ctx->cur_texcoord[ctx->active_texture][i];
            break;
        case GL_COLOR_CLEAR_VALUE:
            for (int i = 0; i < 4; i++) params[i] = ctx->clear_color[i];
            break;
        case GL_BLEND_COLOR:
            for (int i = 0; i < 4; i++) params[i] = ctx->blend_color[i];
            break;
        case GL_LIGHT_MODEL_AMBIENT:
            for (int i = 0; i < 4; i++) params[i] = ctx->light_model_ambient[i];
            break;
        case GL_DEPTH_CLEAR_VALUE:
            params[0] = (GLfloat)ctx->clear_depth;
            break;
        case GL_DEPTH_RANGE:
            params[0] = (GLfloat)ctx->depth_near;
            params[1] = (GLfloat)ctx->depth_far;
            break;
        case GL_POLYGON_OFFSET_FACTOR:
            params[0] = ctx->polygon_offset_factor;
            break;
        case GL_POLYGON_OFFSET_UNITS:
            params[0] = ctx->polygon_offset_units;
            break;
        /* Answered for the first time on 2026-09-19 - declared in the header since points and
         * lines landed, and refused by every query. The sizes are what was set; the ranges are
         * what is drawn (see OOPS_GL_MAX_POINT_LINE_SIZE). */
        case GL_POINT_SIZE: params[0] = ctx->point_size; break;
        case GL_POINT_SIZE_MIN: params[0] = ctx->point_size_min; break;
        case GL_POINT_SIZE_MAX: params[0] = ctx->point_size_max; break;
        case GL_POINT_FADE_THRESHOLD_SIZE: params[0] = ctx->point_fade_threshold; break;
        case GL_POINT_SPRITE_COORD_ORIGIN: params[0] = (GLfloat)ctx->point_sprite_origin; break;
        case GL_POINT_DISTANCE_ATTENUATION:
            for (int i = 0; i < 3; i++) params[i] = ctx->point_atten[i];
            break;
        case GL_LINE_WIDTH: params[0] = ctx->line_width; break;
        case GL_POINT_SIZE_RANGE:
        case GL_LINE_WIDTH_RANGE:
        case GL_ALIASED_POINT_SIZE_RANGE:
        case GL_ALIASED_LINE_WIDTH_RANGE:
            params[0] = 1.0f;
            params[1] = (GLfloat)OOPS_GL_MAX_POINT_LINE_SIZE;
            break;
        /* The smooth sizes' step (GL 1.2 names these GL_SMOOTH_*_GRANULARITY): an aliased size is
         * rounded to a whole pixel, a smooth one to an eighth. It was 1 while nothing smoothed. */
        case GL_POINT_SIZE_GRANULARITY:
        case GL_LINE_WIDTH_GRANULARITY:
            params[0] = OOPS_GL_SMOOTH_GRANULARITY;
            break;
        /* The colour-index and coverage values are floats; the integer path would truncate. */
        case GL_CURRENT_INDEX:          params[0] = ctx->cur_index; break;
        case GL_INDEX_CLEAR_VALUE:      params[0] = ctx->clear_index; break;
        case GL_SAMPLE_COVERAGE_VALUE:  params[0] = ctx->sample_coverage_value; break;
        case GL_ACCUM_CLEAR_VALUE:
            for (int i = 0; i < 4; i++) params[i] = ctx->accum_clear[i];
            break;
        case GL_RED_SCALE:    params[0] = ctx->pixel_scale[0]; break;
        case GL_GREEN_SCALE:  params[0] = ctx->pixel_scale[1]; break;
        case GL_BLUE_SCALE:   params[0] = ctx->pixel_scale[2]; break;
        case GL_ALPHA_SCALE:  params[0] = ctx->pixel_scale[3]; break;
        case GL_RED_BIAS:     params[0] = ctx->pixel_bias[0]; break;
        case GL_GREEN_BIAS:   params[0] = ctx->pixel_bias[1]; break;
        case GL_BLUE_BIAS:    params[0] = ctx->pixel_bias[2]; break;
        case GL_ALPHA_BIAS:   params[0] = ctx->pixel_bias[3]; break;
        case GL_DEPTH_SCALE:  params[0] = ctx->depth_scale; break;
        case GL_DEPTH_BIAS:   params[0] = ctx->depth_bias; break;
        case GL_MAP1_GRID_DOMAIN:
            params[0] = ctx->grid1_u1;
            params[1] = ctx->grid1_u2;
            break;
        case GL_MAP2_GRID_DOMAIN:
            params[0] = ctx->grid2_u1;
            params[1] = ctx->grid2_u2;
            params[2] = ctx->grid2_v1;
            params[3] = ctx->grid2_v2;
            break;
        case GL_ALPHA_TEST_REF:
            params[0] = ctx->alpha_ref;
            break;
        default:
            /* Anything glGetIntegerv can answer, this can answer too - GL says every query is
             * available in every type, converted. Only a pname neither knows is refused.
             *
             * **Exactly `gl_query_element_count` of them.** The caller sized the buffer from the
             * pname, so a GLfloat[1] for GL_DEPTH_FUNC is correct and writing four would run
             * off the end of it. */
            {
                GLint iv[4] = {0, 0, 0, 0};
                GLenum before = ctx->last_error;
                ctx->last_error = GL_NO_ERROR;
                glGetIntegerv(pname, iv);
                if (ctx->last_error == GL_NO_ERROR) {
                    ctx->last_error = before;
                    int n = gl_query_element_count(pname);
                    if (n > 4) n = 4; /* nothing integer-valued is wider */
                    for (int i = 0; i < n; i++) params[i] = (GLfloat)iv[i];
                } else if (before != GL_NO_ERROR) {
                    ctx->last_error = before; /* the older error is the one GL retains */
                }
            }
            break;
    }
}

/* Every query, converted to double. GL requires each pname to be available in every type, and
 * the conversion is the whole of the difference - so this goes through glGetFloatv rather than
 * repeating the table, which is what keeps the two from disagreeing about a pname. */
void glGetDoublev(GLenum pname, GLdouble *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    GLfloat fv[16] = {0};
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    glGetFloatv(pname, fv);
    if (ctx->last_error != GL_NO_ERROR) {
        /* Refused: leave the caller's buffer alone and keep whichever error GL retains. */
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    const int n = gl_query_element_count(pname);
    for (int i = 0; i < n; i++) params[i] = (GLdouble)fv[i];
}

/* **Not every boolean query is an enable.** This used to forward everything to glIsEnabled,
 * which answers GL_FALSE for GL_COLOR_WRITEMASK - a mask of all four channels reported as all
 * four channels off, with no error to say the question was misunderstood. The two masks are
 * answered here; everything else is either an enable or reaches the general query below. */
void glGetBooleanv(GLenum pname, GLboolean *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    switch (pname) {
        case GL_COLOR_WRITEMASK:
            for (int i = 0; i < 4; i++) params[i] = ctx->color_mask[i];
            return;
        case GL_DEPTH_WRITEMASK:
            params[0] = ctx->depth_mask;
            return;
        default:
            break;
    }

    /* An enable answers directly. glIsEnabled refuses a cap it does not have, so asking it
     * first and only then falling through keeps that refusal rather than masking it. */
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    GLboolean enabled = glIsEnabled(pname);
    if (ctx->last_error == GL_NO_ERROR) {
        ctx->last_error = before;
        params[0] = enabled;
        return;
    }
    ctx->last_error = before;

    /* Not an enable: anything with a value is false when that value is zero, which is what GL
     * says the conversion is. */
    GLfloat fv[16] = {0};
    ctx->last_error = GL_NO_ERROR;
    glGetFloatv(pname, fv);
    if (ctx->last_error != GL_NO_ERROR) {
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    const int n = gl_query_element_count(pname);
    for (int i = 0; i < n; i++) params[i] = (GLboolean)(fv[i] != 0.0f);
}

/* -------------------------------------------------------------------------
 * Fixed-Function Lighting & Materials
 * ------------------------------------------------------------------------- */

/* Recorded in the coordinates passed: a light's position and spot direction are put through the
 * modelview current when the list *runs*, which is what lets a list place a light relative to
 * whatever it is drawn inside. The integer forms convert and forward here, so they record the
 * normalised values. */
void glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
    if (params && gl_list_recording() &&
        gl_list_rec_fv(GL_LIST_OP_LIGHT_FV, light, pname, GL_TRUE, pname, params)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + OOPS_GL_LIGHT_COUNT) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    size_t idx = (size_t)(light - GL_LIGHT0);
    gl_light_t *l = &ctx->lights[idx];

    switch (pname) {
        case GL_AMBIENT:
            memcpy(l->ambient, params, 4 * sizeof(float));
            break;
        case GL_DIFFUSE:
            memcpy(l->diffuse, params, 4 * sizeof(float));
            break;
        case GL_SPECULAR:
            memcpy(l->specular, params, 4 * sizeof(float));
            break;
        case GL_POSITION: {
            /* Position is transformed by current ModelView matrix into eye space */
            const gl_mat4_t *mv = &ctx->modelview_stack[ctx->modelview_depth];
            mat4_transform_vec4(l->position, mv, params);
            break;
        }
        case GL_SPOT_DIRECTION: {
            const gl_mat4_t *mv = &ctx->modelview_stack[ctx->modelview_depth];
            float dx = params[0], dy = params[1], dz = params[2];
            l->spot_direction[0] = mv->m[0] * dx + mv->m[4] * dy + mv->m[8] * dz;
            l->spot_direction[1] = mv->m[1] * dx + mv->m[5] * dy + mv->m[9] * dz;
            l->spot_direction[2] = mv->m[2] * dx + mv->m[6] * dy + mv->m[10] * dz;
            float len = gl_sqrt(l->spot_direction[0] * l->spot_direction[0] +
                                l->spot_direction[1] * l->spot_direction[1] +
                                l->spot_direction[2] * l->spot_direction[2]);
            if (len > 1e-6f) {
                float inv = 1.0f / len;
                l->spot_direction[0] *= inv;
                l->spot_direction[1] *= inv;
                l->spot_direction[2] *= inv;
            }
            break;
        }
        case GL_SPOT_EXPONENT:
            l->spot_exponent = params[0];
            break;
        case GL_SPOT_CUTOFF:
            l->spot_cutoff = params[0];
            if (params[0] <= 90.0f) {
                float rad = params[0] * (3.14159265f / 180.0f);
                l->spot_cutoff_cos = gl_cos(rad);
            } else {
                l->spot_cutoff_cos = -1.0f;
            }
            break;
        case GL_CONSTANT_ATTENUATION:
            l->const_att = params[0];
            break;
        case GL_LINEAR_ATTENUATION:
            l->linear_att = params[0];
            break;
        case GL_QUADRATIC_ATTENUATION:
            l->quad_att = params[0];
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glLightf(GLenum light, GLenum pname, GLfloat param) {
    glLightfv(light, pname, &param);
}

/* Returns whether `pname` is one this keeps; the caller records the refusal. */
static GLboolean apply_material_param(gl_material_t *m, GLenum pname, const GLfloat *params) {
    switch (pname) {
        case GL_AMBIENT:
            memcpy(m->ambient, params, 4 * sizeof(float));
            break;
        case GL_DIFFUSE:
            memcpy(m->diffuse, params, 4 * sizeof(float));
            break;
        case GL_SPECULAR:
            memcpy(m->specular, params, 4 * sizeof(float));
            break;
        case GL_EMISSION:
            memcpy(m->emission, params, 4 * sizeof(float));
            break;
        case GL_SHININESS:
            m->shininess = params[0];
            if (m->shininess < 0.0f) m->shininess = 0.0f;
            if (m->shininess > 128.0f) m->shininess = 128.0f;
            break;
        case GL_AMBIENT_AND_DIFFUSE:
            memcpy(m->ambient, params, 4 * sizeof(float));
            memcpy(m->diffuse, params, 4 * sizeof(float));
            break;
        /* A material property this does not keep is refused rather than dropped: the six above
         * are the whole of what the lighting model reads, and a caller setting a seventh was
         * told nothing and shaded as though it had not. Reported rather than recorded here,
         * because this helper has no context - the caller owns the error flag. */
        default: return GL_FALSE;
    }
    return GL_TRUE;
}

/* Whether GL_COLOR_MATERIAL drives material property `prop` - GL_AMBIENT, GL_DIFFUSE,
 * GL_SPECULAR or GL_EMISSION - of the front or back material now. */
static GLboolean gl_cm_tracks(const gl_context_t *ctx, GLboolean back, GLenum prop) {
    if (!ctx->cap_color_material) return GL_FALSE;
    const GLenum f = ctx->color_material_face;
    if (f != GL_FRONT_AND_BACK && f != (back ? GL_BACK : GL_FRONT)) return GL_FALSE;
    const GLenum m = ctx->color_material_mode;
    return (GLboolean)(m == prop ||
                       (m == GL_AMBIENT_AND_DIFFUSE && (prop == GL_AMBIENT || prop == GL_DIFFUSE)));
}

/* **The current colour written into every property GL_COLOR_MATERIAL tracks** - Mesa's
 * _mesa_update_color_material (main/light.c:759-772), run where Mesa runs it: when the colour
 * changes (vbo/vbo_exec_api.c:237-240), when the enable goes on (main/enable.c:568-577) and when
 * glColorMaterial changes what is tracked while it is on (main/light.c:800-803). Until 2026-09-19
 * the colour was only substituted while lighting, so glGetMaterial read back the material as last
 * set, and once GL_COLOR_MATERIAL went off the material was that stale value again rather than the
 * last colour it had tracked. */
void gl_color_material_update(gl_context_t *ctx) {
    if (!ctx || !ctx->cap_color_material) return;
    for (int side = 0; side < 2; side++) {
        gl_material_t *m = side ? &ctx->mat_back : &ctx->mat_front;
        const GLboolean back = (GLboolean)(side == 1);
        if (gl_cm_tracks(ctx, back, GL_AMBIENT)) memcpy(m->ambient, ctx->cur_color, sizeof(m->ambient));
        if (gl_cm_tracks(ctx, back, GL_DIFFUSE)) memcpy(m->diffuse, ctx->cur_color, sizeof(m->diffuse));
        if (gl_cm_tracks(ctx, back, GL_SPECULAR)) memcpy(m->specular, ctx->cur_color, sizeof(m->specular));
        if (gl_cm_tracks(ctx, back, GL_EMISSION)) memcpy(m->emission, ctx->cur_color, sizeof(m->emission));
    }
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {
    if (params && gl_list_recording() &&
        gl_list_rec_fv(GL_LIST_OP_MATERIAL_FV, face, pname, GL_TRUE, pname, params)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* **A property GL_COLOR_MATERIAL tracks is left alone** - the colour owns it while the enable
     * is on (Mesa, vbo/vbo_exec_api.c:590-598). Set on a copy, and the tracked ones put back. */
    GLboolean kept = GL_TRUE;
    for (int side = 0; side < 2; side++) {
        const GLboolean back = (GLboolean)(side == 1);
        if (back ? (face == GL_FRONT) : (face == GL_BACK)) continue;
        gl_material_t *m = back ? &ctx->mat_back : &ctx->mat_front;
        const gl_material_t before = *m;
        kept = apply_material_param(m, pname, params);
        if (gl_cm_tracks(ctx, back, GL_AMBIENT)) memcpy(m->ambient, before.ambient, sizeof(m->ambient));
        if (gl_cm_tracks(ctx, back, GL_DIFFUSE)) memcpy(m->diffuse, before.diffuse, sizeof(m->diffuse));
        if (gl_cm_tracks(ctx, back, GL_SPECULAR)) memcpy(m->specular, before.specular, sizeof(m->specular));
        if (gl_cm_tracks(ctx, back, GL_EMISSION)) memcpy(m->emission, before.emission, sizeof(m->emission));
    }
    if (!kept) gl_record_error(ctx, GL_INVALID_ENUM);
}

void glMaterialf(GLenum face, GLenum pname, GLfloat param) {
    glMaterialfv(face, pname, &param);
}

void glLightModelfv(GLenum pname, const GLfloat *params) {
    if (params && gl_list_recording() &&
        gl_list_rec_fv(GL_LIST_OP_LIGHT_MODEL_FV, pname, 0u, GL_FALSE, pname, params)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;

    switch (pname) {
        case GL_LIGHT_MODEL_AMBIENT:
            memcpy(ctx->light_model_ambient, params, 4 * sizeof(float));
            break;
        case GL_LIGHT_MODEL_LOCAL_VIEWER:
            ctx->light_model_local_viewer = (params[0] != 0.0f) ? GL_TRUE : GL_FALSE;
            break;
        /* GL 1.2: whether the specular term joins the rest of the lit colour or is kept apart
         * and added after texturing (gl_compute_lighting2). Only the two values. */
        case GL_LIGHT_MODEL_COLOR_CONTROL: {
            const GLenum e = (GLenum)(GLint)params[0];
            if (e != GL_SINGLE_COLOR && e != GL_SEPARATE_SPECULAR_COLOR) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            ctx->light_model_color_control = e;
            break;
        }
        /* **Two-sided lighting** (2026-09-19): a polygon facing away is lit with the back
         * material and its normal reversed. It was refused - it needs the polygon's facing,
         * and lighting was thought to run before that existed - and before it was refused it
         * set a field nothing read. The triangle stage lights all three vertices together, so
         * it winds them first (gl_draw_triangle_pv). */
        case GL_LIGHT_MODEL_TWO_SIDE:
            ctx->light_model_two_side = (params[0] != 0.0f) ? GL_TRUE : GL_FALSE;
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glLightModelf(GLenum pname, GLfloat param) {
    glLightModelfv(pname, &param);
}

/* -------------------------------------------------------------------------
 * The integer spellings of the lighting calls
 *
 * **A colour is not converted the same way a scalar is.** An integer colour component is mapped
 * across the whole signed range onto [-1, 1] - so GL_AMBIENT with INT_MAX means 1.0, not
 * 2147483647.0 - while a position, a direction, an attenuation or a shininess is an ordinary
 * cast. Casting a colour would make every integer colour astronomically out of range and light
 * the scene pure white, which looks like a lighting bug anywhere but here.
 *
 * The rule and the constant are Mesa's: `INT_TO_FLOAT` in `src/mesa/main/macros.h`, applied to
 * exactly these pnames in `src/mesa/main/light.c` (`_mesa_Lightiv`) and
 * `src/mesa/vbo/vbo_attrib_tmp.h` (`Materialiv`).
 * ------------------------------------------------------------------------- */

void glLightiv(GLenum light, GLenum pname, const GLint *params) {
    if (!params) return;
    GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    switch (pname) {
        case GL_AMBIENT:
        case GL_DIFFUSE:
        case GL_SPECULAR:
            for (int i = 0; i < 4; i++) f[i] = gl_int_to_colour(params[i]);
            break;
        case GL_POSITION:
            for (int i = 0; i < 4; i++) f[i] = (GLfloat)params[i];
            break;
        case GL_SPOT_DIRECTION:
            for (int i = 0; i < 3; i++) f[i] = (GLfloat)params[i];
            break;
        default:
            /* One value, or a pname glLightfv will refuse for us. */
            f[0] = (GLfloat)params[0];
            break;
    }
    glLightfv(light, pname, f);
}

void glLighti(GLenum light, GLenum pname, GLint param) {
    glLightiv(light, pname, &param);
}

void glMaterialiv(GLenum face, GLenum pname, const GLint *params) {
    if (!params) return;
    GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    switch (pname) {
        case GL_AMBIENT:
        case GL_DIFFUSE:
        case GL_SPECULAR:
        case GL_EMISSION:
        case GL_AMBIENT_AND_DIFFUSE:
            for (int i = 0; i < 4; i++) f[i] = gl_int_to_colour(params[i]);
            break;
        default:
            f[0] = (GLfloat)params[0];
            break;
    }
    glMaterialfv(face, pname, f);
}

void glMateriali(GLenum face, GLenum pname, GLint param) {
    glMaterialiv(face, pname, &param);
}

void glLightModeliv(GLenum pname, const GLint *params) {
    if (!params) return;
    GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (pname == GL_LIGHT_MODEL_AMBIENT) {
        for (int i = 0; i < 4; i++) f[i] = gl_int_to_colour(params[i]);
    } else {
        f[0] = (GLfloat)params[0];
    }
    glLightModelfv(pname, f);
}

void glLightModeli(GLenum pname, GLint param) {
    glLightModeliv(pname, &param);
}

void glColorMaterial(GLenum face, GLenum mode) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_COLOR_MATERIAL, gl_la_e(face), gl_la_e(mode))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* Checked, as Mesa checks them (main/light.c, _mesa_ColorMaterial): any value used to be
     * stored, and one that named no side or no material property tracked nothing. */
    const GLboolean face_ok = (GLboolean)(face == GL_FRONT || face == GL_BACK ||
                                          face == GL_FRONT_AND_BACK);
    const GLboolean mode_ok = (GLboolean)(mode == GL_EMISSION || mode == GL_AMBIENT ||
                                          mode == GL_DIFFUSE || mode == GL_SPECULAR ||
                                          mode == GL_AMBIENT_AND_DIFFUSE);
    if (!face_ok || !mode_ok) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->color_material_face = face;
    ctx->color_material_mode = mode;
    gl_color_material_update(ctx);
}

/* -------------------------------------------------------------------------
 * Texture Object Management
 * ------------------------------------------------------------------------- */

#ifdef OOPS_HOST_BUILD
#include <stdlib.h>
#endif

/* The buffer-object allocator, defined with the buffer objects below; mip levels use it too. */
static void *gl_buffer_alloc(size_t bytes);
static void gl_buffer_release(void *p);

static gl_texture_object_t *gl_find_texture(gl_context_t *ctx, GLuint id) {
    if (!ctx || id == 0) return NULL;
    /* Slot `id - 1` first, for the reason `gl_lookup_texture` gives at length. */
    if (id <= (GLuint)OOPS_GL_MAX_TEXTURE_OBJECTS) {
        gl_texture_object_t *t = &ctx->textures[id - 1u];
        if (t->used && t->id == id) return t;
    }
    for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
        if (ctx->textures[i].used && ctx->textures[i].id == id) {
            return &ctx->textures[i];
        }
    }
    return NULL;
}

static gl_texture_object_t *gl_find_or_create_texture(gl_context_t *ctx, GLuint id) {
    if (!ctx || id == 0) return NULL;
    gl_texture_object_t *tex = gl_find_texture(ctx, id);
    if (tex) return tex;

    for (int i = 0; i < OOPS_GL_MAX_TEXTURE_OBJECTS; i++) {
        if (!ctx->textures[i].used) {
            tex = &ctx->textures[i];
            memset(tex, 0, sizeof(*tex));
            tex->id = id;
            tex->used = GL_TRUE;
            tex->wrap_s = GL_REPEAT;
            tex->wrap_t = GL_REPEAT;
            tex->wrap_r = GL_REPEAT;
            tex->depth = 1;
            tex->min_filter = GL_NEAREST_MIPMAP_LINEAR;
            tex->mag_filter = GL_LINEAR;
            tex->priority = 1.0f; /* border colour (0, 0, 0, 0) from the memset */
            tex->max_level = 1000; /* GL 1.2's defaults; the base level 0 */
            tex->min_lod = -1000.0f;
            tex->max_lod = 1000.0f;
            /* GL 1.4's depth-texture defaults: no comparison, GL_LEQUAL, read as luminance. */
            tex->compare_mode = GL_NONE;
            tex->compare_func = GL_LEQUAL;
            tex->depth_mode = GL_LUMINANCE;
            return tex;
        }
    }
    return NULL;
}

/*
 * **GPU-visible texture storage may not be freed while a built frame still points at it.**
 *
 * The hardware path is deferred: `glDrawArrays` writes PM4 into the command buffer and returns,
 * and nothing executes until the flush. A texture's pixels live in GARLIC memory and the draw
 * carries their *address*, not a copy - so freeing that memory between the draw call and the
 * submission hands the sampler pages that are no longer mapped.
 *
 * The specification is on the caller's side here: deleting a texture that is still in use is
 * legal, and it is the implementation that owes the storage a lifetime long enough for the draws
 * referencing it. gl1-probe's texture check does exactly that - draws, deletes, then reads the
 * pixels back - and on 2026-09-17 it took the console's GPU down with it:
 *
 *     GPU Protection fault. client:TCP(8) access:Read permission:0x3
 *     reason: Unmapped page access, Protection fault addr(VA): 0x0000000203190000
 *     504 wavefronts ... XNACK_ERROR MEMVIOL
 *
 * `TCP` is the texture cache. 504 wavefronts faulted, the GPU was reset and the user interface
 * restarted. Flushing first is the blunt fix and the correct one: it costs a submission only
 * when a program changes texture storage mid-frame, and it makes the free safe by making the
 * draws that reference it finish. A deferred-free list keyed on the fence would cost less and
 * is worth having when something needs it.
 */
static void gl_tex_storage_release_sync(gl_context_t *ctx) {
#ifndef OOPS_HOST_BUILD
    if (ctx && ctx->use_hardware && ctx->hw_frame_active) {
        gl_hw_flush(ctx);
    }
#else
    (void)ctx;
#endif
}

/* A wrap mode as SQ_IMG_SAMP_WORD0's CLAMP_X/Y/Z value: radeonsi's si_tex_wrap
 * (si_state.c:1927), the values from gfx8.json:646-652. */
static uint32_t gl_hw_wrap(GLenum w) {
    switch (w) {
        case GL_MIRRORED_REPEAT: return 1u; /* SQ_TEX_MIRROR */
        case GL_CLAMP_TO_EDGE:   return 2u; /* SQ_TEX_CLAMP_LAST_TEXEL */
        case GL_CLAMP:           return 4u; /* SQ_TEX_CLAMP_HALF_BORDER */
        case GL_CLAMP_TO_BORDER: return 6u; /* SQ_TEX_CLAMP_BORDER */
        default:                 return 0u; /* SQ_TEX_WRAP: GL_REPEAT */
    }
}

/*
 * GL's depth comparison as the sampler's DEPTH_COMPARE_FUNC. The order is the hardware's own
 * (Mesa `ac_descriptors.c:119` writes GL's/Gallium's function straight into the field, and the
 * two enumerations agree): NEVER 0, LESS 1, EQUAL 2, LEQUAL 3, GREATER 4, NOTEQUAL 5, GEQUAL 6,
 * ALWAYS 7. obSCEne's `-6c80` ran function 3 and reported a pass and a fail either side of the
 * stored depth, which anchors the one value the whole ordering hangs on.
 */
static uint32_t gl_hw_depth_compare(GLenum func) {
    switch (func) {
        case GL_NEVER:    return 0u;
        case GL_LESS:     return 1u;
        case GL_EQUAL:    return 2u;
        case GL_LEQUAL:   return 3u;
        case GL_GREATER:  return 4u;
        case GL_NOTEQUAL: return 5u;
        case GL_GEQUAL:   return 6u;
        default:          return 7u; /* GL_ALWAYS */
    }
}

static void gl_pack_descriptors(gl_texture_object_t *tex) {
    if (!tex) return;
    /* The mip chain when there is one to describe (gl_tex_hw_prepare decides), the base level's
     * own storage otherwise - which is every texture that does not read mipmaps, and so gets
     * exactly the descriptors it always had. */
    /* **`mipchain=off` in `/app0/oops-gl` takes the chain out of the descriptor**, leaving the
     * base level's own storage - which is the allocation `tex->pixels` names and the one a frame
     * capture carries, so it is the configuration a replay on the software rasteriser reproduces
     * exactly. Any difference between the console and that replay is then a difference about the
     * chain and nothing else. Default on; this is a question, not a setting. */
    const GLboolean chain =
        (GLboolean)(tex->desc_chain && tex->chain_data && gl_mipchain_enabled);
    uint64_t va = chain ? tex->chain_va : tex->garlic_va;
    uint32_t w = tex->width ? (uint32_t)tex->width : 1u;
    uint32_t h = tex->height ? (uint32_t)tex->height : 1u;
    /* A chain starts at the base level it was built from, whose size the descriptor carries. */
    gl_tex_view_t cb;
    if (chain && gl_tex_level_view(tex, tex->chain_base, &cb)) {
        w = (uint32_t)cb.width;
        h = (uint32_t)cb.height;
    }
    /* A cube map's own image fields stay empty - its faces are six images - so the size comes
     * from the array `gl_tex_cube_upload` built out of them. */
    if (tex->target == GL_TEXTURE_CUBE_MAP && tex->cube_hw_dim > 0) {
        w = (uint32_t)tex->cube_hw_dim;
        h = w;
    }

    /* **A depth texture's texels are one 32-bit float each**, not four bytes of colour, so its
     * image format is a different one (since 2026-09-20). `GFX10_FORMAT_32_FLOAT` is 22 and
     * `GFX10_FORMAT_8_8_8_8_UNORM` is 56 - `mesa/src/amd/registers/gfx10-rsrc.json:27` and `:61`,
     * the second being the value this descriptor has always carried, which is what says the
     * table being read is the right one. FORMAT is `SQ_IMG_RSRC_WORD1` bits [20,28] (`:371`).
     * Both are four bytes a texel, so the row pitch, the chain layout and the slice stride are
     * all unchanged by this. */
    gl_tex_view_t fv;
    const GLboolean is_depth =
        (GLboolean)(gl_tex_level_view(tex, tex->base_level, &fv) &&
                    fv.base_format == GL_DEPTH_COMPONENT);
    const uint32_t img_format = is_depth ? 22u : 56u;

    /* RDNA2 SQ_IMG_RSRC_WORD0..7 (32 bytes), laid out as the public RDNA ISA reference gives them.
     * The base address is in 256-byte units: the unshifted address sent the sampler 256 times too
     * far and drew a wavefront fault on 2026-09-14. */
    tex->img_desc[0] = (uint32_t)(va >> 8);                                   /* BASE_ADDRESS[39:8] */
    tex->img_desc[1] = (uint32_t)((va >> 40) & 0xffu) | (img_format << 20) | (((w - 1u) & 3u) << 30); /* BASE_ADDRESS_HI, MIN_LOD=0, FORMAT, WIDTH_LO */
    tex->img_desc[2] = (((w - 1u) >> 2) & 0x3fffu) | (((h - 1u) & 0x3fffu) << 14) | (1u << 31); /* WIDTH_HI, HEIGHT, RESOURCE_LEVEL */
    /* TYPE=2D (9), SW_MODE=0, DST_SEL X,Y,Z,W = channels 0,1,2,3 (4,5,6,7).
     *
     * SW_MODE 0 is `ADDR_SW_LINEAR`, **not** `ADDR_SW_LINEAR_GENERAL`, which this comment used
     * to call it. The two behave differently and the distinction decides the row pitch:
     * addrlib gives LINEAR_GENERAL a pitch alignment of one element and LINEAR an alignment of
     * `256 / elementBytes` (mesa/src/amd/addrlib/src/gfx9/gfx9addrlib.cpp:5117-5127). It cannot
     * be the former here in any case - `ADDR_SW_LINEAR_GENERAL` is 32
     * (mesa/src/amd/addrlib/inc/addrtypes.h:259) and SW_MODE is five bits wide,
     * mesa/src/amd/registers/gfx10-rsrc.json:388, so 32 does not fit in the field. It is an
     * addrlib-internal mode, not a value the hardware takes. */
    /*
     * **TYPE is the target's** since 2026-09-20: 9 for 2D, 0xa for 3D, 0xb for a cube map
     * (`S_00A00C_TYPE`, Mesa `ac_descriptors.c:372`). obSCEne's `REQ-20260920T0745Z-6c80`
     * (sweep `20260920-103636`, `166-agc/texture-extended`) sampled all three on this part and
     * reports the words: `0xa0000fac` for the 3D arm, `0xb0000fac` for the cube, `0x90000fac`
     * for the 2D control over the same memory - which is what says the field, and not something
     * else about the arm, is what changed the result.
     */
    uint32_t img_type = 9u;
    if (tex->target == GL_TEXTURE_3D) img_type = 0xau;
    else if (tex->target == GL_TEXTURE_CUBE_MAP) img_type = 0xbu;
    tex->img_desc[3] = (img_type << 28) | 0xfacu;
    /* WORD4. For a 2D image DEPTH holds the low 13 bits of the row pitch and PITCH_MSB bit 13
     * holds the top one, both as pitch - 1, and the hardware reads them only when the pitch
     * exceeds the width - "1D, 2D, 2D_MSAA: the pitch if pitch > width, the low bits are in
     * DEPTH", mesa/src/amd/registers/gfx10-rsrc.json:401-406 (type SQ_IMG_RSRC_WORD4_gfx103).
     * Mesa encodes it the same way at ac_descriptors.c:711-712.
     *
     * Left at zero for a texture whose rows are exactly as wide as the image, which is every
     * width that is a multiple of 64 pixels - so the descriptor gl-cube gets is unchanged. */
    const uint32_t pitch = tex->pitch ? tex->pitch : w;
    if (tex->target == GL_TEXTURE_3D || tex->target == GL_TEXTURE_CUBE_MAP) {
        /* **For a 3D image and a cube map the field is the last slice, not a pitch** - DEPTH is
         * the slice count less one, which is what `-6c80` set: `0x1` for its two-slice volume
         * and `0x5` for the cube's six faces. The pitch reading below is the 2D one, and writing
         * it here would describe a volume one row wide. */
        /* A chain's depth is the **base level's**, as its width and height above are: the
         * descriptor's level 0 is GL's base level, and the hardware halves all three from
         * there. `tex->depth` is level 0's, which is the same number whenever the base level is
         * 0 and the wrong one when it is not. */
        uint32_t volume_slices = (uint32_t)(tex->depth > 0 ? tex->depth : 1);
        if (chain && tex->target == GL_TEXTURE_3D && cb.depth > 0) {
            volume_slices = (uint32_t)cb.depth;
        }
        const uint32_t slices = (tex->target == GL_TEXTURE_CUBE_MAP) ? 6u : volume_slices;
        tex->img_desc[4] = (slices - 1u) & 0x1fffu;
    } else if (pitch > w && !chain) {
        const uint32_t p1 = pitch - 1u;
        tex->img_desc[4] = (p1 & 0x1fffu) | (((p1 >> 13) & 1u) << 13);
    } else {
        tex->img_desc[4] = 0u;
    }
    /* WORD5 PERF_MOD [20,22] (gfx10-rsrc.json:413, and :424 for the gfx10.3 form this part
     * uses). **Mesa writes 4 here unconditionally** for every gfx10 texture it builds -
     * `S_00A014_PERF_MOD(4)`, ac_descriptors.c:543 - and this wrote zero, which is the one field
     * of the eight where the descriptor differs from the reference implementation for an ordinary
     * 2D image. Zero is not a documented "default" so much as the value nobody chose.
     *
     * ARRAY_PITCH [0,3] stays 0: it is meaningful only for a 3D image, where 0 selects the
     * read-only reading of DEPTH that the sampler wants. MAX_MIP [4,7] is the chain's, ORed in
     * below rather than assigned over this. */
    tex->img_desc[5] = 4u << 20;
    tex->img_desc[6] = 0u;
    tex->img_desc[7] = 0u;
    /* **What a texture actually got**, for the first few packed in a run.
     *
     * A port whose textures render as blocky, banded, wrongly-coloured surfaces has a sampling
     * fault, and the candidates - the row pitch, whether the chain path or the plain one was
     * taken, what WORD4 ended up as - are all decided right here and none of them are visible
     * from outside. Guessing between them has cost several hardware runs; this prints them.
     *
     * Eight, because a title's first textures are the ones a title screen draws, and because a
     * line per texture for eighty-five of them would bury what it is competing with. */
#ifndef OOPS_HOST_BUILD
    {
        /* **Every distinct texture once**, the default excluded. Filtering by width was a guess
           about which textures were interesting, and the guess is what needs testing: a port whose
           *text* draws correctly while its level art does not already proves the sampling path
           works for some textures, so the useful comparison is a working one beside a broken one,
           not a preselected set. The default texture is skipped because it is repacked on every
           bind and ate the whole window when it was not. */
        static uint32_t told;
        /* **Every real image, keyed on the size rather than the name.** Two earlier versions of
           this filter each spent the whole window before anything interesting loaded: first on
           the default texture, which is repacked on every bind, and then on skipping a repeat of
           the last id - which turned out to discard every picture this port has, because its
           loader uploads a 1x1 placeholder and immediately re-specifies the same texture at its
           true size. The call worth seeing is always the one straight after the one that got
           printed. Eighty-four lines of `0x1 0x1` from a title that plainly draws pictures was
           what said so. A hundred and twenty-eight of these is nothing next to the six thousand
           lines a frame of this port already emits. */
        if (told < 128u && tex->id > 1u && (w > 1u || h > 1u)) {
            told++;
            /* Two lines. The first is the descriptor as packed; the second is what the texels
               actually are in GPU memory, which is the half of the question the descriptor
               cannot answer.
               `gl_klog_val` belongs to gl_context.c, so this builds its own from the hex helper
               every refusal message already uses. */
            char m[200];
            size_t n = 0;
            const char *lead = "tex id/w/h/pitch/levels/word4/valo";
            while (lead[n] && n < 48u) { m[n] = lead[n]; n++; }
            n = gl_msg_hex(m, sizeof(m), n, tex->id);
            n = gl_msg_hex(m, sizeof(m), n, w);
            n = gl_msg_hex(m, sizeof(m), n, h);
            n = gl_msg_hex(m, sizeof(m), n, pitch);
            n = gl_msg_hex(m, sizeof(m), n, (uint32_t)tex->chain_levels);
            n = gl_msg_hex(m, sizeof(m), n, tex->img_desc[4]);
            /* The low byte of the address the descriptor could not carry: `img_desc[0]` is the
               address in 256-byte units, so anything here is a texture the sampler reads from
               the wrong place. Expected zero - `oops_mem_alloc` is asked for 256. */
            n = gl_msg_hex(m, sizeof(m), n, (uint32_t)(va & 0xffu));
            m[n] = 0;
            gl_log_line(m);

            /* **The first four texels of row 0, and the first of row 1.** This is the one
               measurement that splits the problem in half whichever theory is right: if these
               bytes are the image, the upload is sound and the fault is in how the sampler is
               told to read them; if they are not, nothing about the descriptor matters yet.
               Row 1 comes along because it is where a wrong pitch shows up first - it should be
               the image's second row, not more of the first. */
            const uint32_t *t0 = (const uint32_t *)tex->garlic_data;
            if (t0) {
                char t[200];
                size_t k = 0;
                const char *tl = "tex texels id/r0x4/r1";
                while (tl[k] && k < 32u) { t[k] = tl[k]; k++; }
                k = gl_msg_hex(t, sizeof(t), k, tex->id);
                k = gl_msg_hex(t, sizeof(t), k, t0[0]);
                k = gl_msg_hex(t, sizeof(t), k, t0[1]);
                k = gl_msg_hex(t, sizeof(t), k, t0[2]);
                k = gl_msg_hex(t, sizeof(t), k, t0[3]);
                /* Guarded: the allocation is `pitch * height` texels, so row 1 exists only when
                   there is a second row to read. A one-pixel-high texture would otherwise read
                   one texel past the end. */
                k = gl_msg_hex(t, sizeof(t), k, h > 1u ? t0[pitch] : 0u);
                t[k] = 0;
                gl_log_line(t);
            }
        }
    }
#endif
    if (chain) {
        /* WORD3 LAST_LEVEL [16,19] and WORD5 MAX_MIP [4,7] (gfx10-rsrc.json, SQ_IMG_RSRC_WORD3 and
         * _WORD5) both the chain's last level, BASE_LEVEL [12,15] left at 0 - as radeonsi fills
         * them (ac_descriptors.c:528-551). No custom pitch in WORD4: a naturally laid-out chain
         * has each level's pitch derived by the hardware, and Mesa writes that field only for a
         * surface created with a pitch of its own (ac_descriptors.c:697-713). */
        const uint32_t top = (uint32_t)(tex->chain_levels > 0 ? tex->chain_levels - 1 : 0);
        tex->img_desc[3] |= (top & 0xfu) << 16;
        tex->img_desc[5] |= (top & 0xfu) << 4;
    }

    /* RDNA2 SQ_IMG_SAMP_WORD0..3 (16 bytes). CLAMP_X [0,2] and CLAMP_Y [3,5] as radeonsi's
     * si_tex_wrap chooses them (si_state.c:1927). GL_CLAMP was CLAMP_LAST_TEXEL here until
     * 2026-09-19, which is GL_CLAMP_TO_EDGE; it is the half-border clamp, whose linear filter at
     * the edge takes half the border colour. */
    uint32_t cx = gl_hw_wrap(tex->wrap_s);
    uint32_t cy = gl_hw_wrap(tex->wrap_t);
    /* **CLAMP_Z [6,8] for the third coordinate**, which only a 3D image has - `-6c80`'s 3D arm
     * carried `0x92` where its 2D control carried `0x12`, the difference being this field set to
     * the same clamp as the other two. A 2D image leaves it zero, as every descriptor here has.
     *
     * **DEPTH_COMPARE_FUNC [12,14]** is GL's `GL_TEXTURE_COMPARE_FUNC` when
     * `GL_TEXTURE_COMPARE_MODE` asks for it (`S_008F30_DEPTH_COMPARE_FUNC`, Mesa
     * `ac_descriptors.c:119`). `-6c80` ran two arms with one reference either side of the stored
     * depth under function 3, `LEQUAL`, and they came back `0xffffffff` and `0xff000000` - the
     * comparison passing and failing, which is what says the field works rather than the sample
     * returning nothing. */
    const uint32_t cz = (tex->target == GL_TEXTURE_3D) ? gl_hw_wrap(tex->wrap_r) : 0u;
    uint32_t cmp = 0u;
    if (tex->compare_mode == GL_COMPARE_R_TO_TEXTURE) cmp = gl_hw_depth_compare(tex->compare_func);
    tex->samp_desc[0] = cx | (cy << 3) | (cz << 6) | (cmp << 12);
    /* WORD1: MIN_LOD [0,11] and MAX_LOD [12,23], unsigned 4.8 fixed point clamped to [0, 15], as
     * radeonsi encodes GL_TEXTURE_MIN_LOD and _MAX_LOD before GFX12 (ac_descriptors.c:139-140) -
     * in the chain's own levels, whose level 0 is GL's base level, where GL measures from too.
     * **Except the default maximum**, which stays the field's own 0xfff rather than radeonsi's
     * 15.0: that is what every descriptor here has carried, gl-cube's included, and no texture
     * here has 15 levels for the difference to reach. */
    const float lo = (tex->min_lod > 0.0f) ? ((tex->min_lod < 15.0f) ? tex->min_lod : 15.0f) : 0.0f;
    const float hi = (tex->max_lod > 0.0f) ? ((tex->max_lod < 15.0f) ? tex->max_lod : 15.0f) : 0.0f;
    const uint32_t min_lod = (uint32_t)(lo * 256.0f);
    const uint32_t max_lod = (tex->max_lod >= 15.0f) ? 0xfffu : (uint32_t)(hi * 256.0f);
    tex->samp_desc[1] = (min_lod & 0xfffu) | ((max_lod & 0xfffu) << 12);
    uint32_t mag = (tex->mag_filter == GL_LINEAR) ? 1u : 0u;
    uint32_t min = (tex->min_filter == GL_LINEAR || tex->min_filter == GL_LINEAR_MIPMAP_NEAREST || tex->min_filter == GL_LINEAR_MIPMAP_LINEAR) ? 1u : 0u;
    /* MIP_FILTER [26,27]: NONE 0, POINT 1, LINEAR 2 (gfx8.json:668-673, the same field at
     * gfx10-rsrc.json:493), chosen as radeonsi's si_tex_mipfilter does (si_state.c:1950-1961).
     * NONE without a chain, which samples the base level whatever the filter - what the hardware
     * has always done here - and for a chain of one level, which a base level other than 0 or a
     * non-mipmap filter gives. */
    uint32_t mip = 0u;
    if (chain && tex->chain_levels > 1 && gl_filter_uses_mipmaps(tex->min_filter)) {
        mip = (tex->min_filter == GL_NEAREST_MIPMAP_LINEAR || tex->min_filter == GL_LINEAR_MIPMAP_LINEAR)
                  ? 2u : 1u;
    }
    tex->samp_desc[2] = (mag << 20) | (min << 22) | (mip << 26);

    /* WORD3: BORDER_COLOR_TYPE [30,31] and BORDER_COLOR_PTR [0,11], as radeonsi's
     * si_translate_border_color picks them (si_state.c:4010-4074) - transparent black when no
     * axis reads the border (GL_CLAMP only does under a linear filter), one of the three built-in
     * colours when the border is one, and otherwise entry 0 of the border colour table
     * (SQ_TEX_BORDER_COLOR_REGISTER; the values are gfx8.json:638-641), which the draw fills from
     * `border_hw` and TA_BC_BASE_ADDR points at. The colour is expanded by the base format first,
     * as the texels are. */
    for (int i = 0; i < 4; i++) tex->border_hw[i] = tex->border_color[i];
    if (is_depth) {
        /* **A depth texture's border is the border colour's red, as a depth** - the software
         * path's rule, and the one the comparison needs: the border is a *stored depth* that
         * `GL_TEXTURE_COMPARE_FUNC` still runs against, and what GL_DEPTH_TEXTURE_MODE describes
         * is what the comparison's result becomes afterwards. Expanding by that mode here would
         * be applying it a step too early - and for `GL_ALPHA` it zeroes red, which is the one
         * channel a 32_FLOAT image reads. */
        tex->border_hw[1] = tex->border_hw[2] = tex->border_hw[3] = tex->border_hw[0];
    } else {
        gl_tex_rebase_f(tex->border_hw, gl_tex_sample_format(tex));
    }
    const GLboolean linear = (GLboolean)(mag != 0u || min != 0u);
    const GLboolean uses_border = (GLboolean)(cx == 6u || cy == 6u || (linear && (cx == 4u || cy == 4u)));
    const float *b = tex->border_hw;
    uint32_t type = 0u; /* SQ_TEX_BORDER_COLOR_TRANS_BLACK */
    tex->border_in_table = GL_FALSE;
    if (uses_border) {
        if (b[0] == 0.0f && b[1] == 0.0f && b[2] == 0.0f && b[3] == 0.0f) {
            type = 0u;
        } else if (b[0] == 0.0f && b[1] == 0.0f && b[2] == 0.0f && b[3] == 1.0f) {
            type = 1u; /* OPAQUE_BLACK */
        } else if (b[0] == 1.0f && b[1] == 1.0f && b[2] == 1.0f && b[3] == 1.0f) {
            type = 2u; /* OPAQUE_WHITE */
        } else {
            type = 3u; /* REGISTER: the table, entry 0 */
            tex->border_in_table = GL_TRUE;
        }
    }
    tex->samp_desc[3] = type << 30;
    tex->desc_dirty = GL_TRUE;
}

/* The object a glTex* call acts on: the named target's binding, or that target's *own* default
 * texture when nothing is bound. `create` distinguishes an upload, which may bring the default
 * into existence, from a query, which should not.
 *
 * Returns NULL for a target this does not have; the caller reports GL_INVALID_ENUM. */
/* The targets that name a binding point here. The `glTex*2D` entry points stay 2D-only - a
 * GL_TEXTURE_1D passed to glTexImage2D is an error in GL, not a shorthand. This is for the calls
 * that are shared between targets: the parameter setters and the queries. */
/* Forward declarations: the copy helper is defined above the upload helper it calls, and the
 * 1D entry points below use all three. */
static void gl_tex_image_common(gl_context_t *ctx, GLenum target, GLint level,
                                GLint internalformat, GLsizei width, GLsizei height,
                                GLsizei depth, GLenum format, GLenum type, const GLvoid *pixels);
static void gl_tex_sub_image_common(gl_context_t *ctx, GLenum target, GLint level,
                                    GLint xoffset, GLint yoffset, GLint zoffset,
                                    GLsizei width, GLsizei height, GLsizei depth,
                                    GLenum format, GLenum type, const GLvoid *pixels);
static void gl_copy_tex_sub_common(gl_context_t *ctx, GLenum target, GLint level,
                                   GLint xoffset, GLint yoffset, GLint zoffset,
                                   GLint x, GLint y, GLsizei width, GLsizei height);
static GLboolean gl_copy_internal_format_ok(GLenum internalformat);
static gl_tex_level_t *gl_proxy_level(gl_context_t *ctx, GLenum target, GLint level);
static GLboolean gl_is_proxy_target(GLenum t);

/* The targets a texture *object* is named by - glBindTexture, glTexParameter, glGetTexParameter,
 * glEnable. */
static GLboolean gl_texture_target_ok(GLenum t) {
    return (t == GL_TEXTURE_2D || t == GL_TEXTURE_1D || t == GL_TEXTURE_3D ||
            t == GL_TEXTURE_CUBE_MAP) ? GL_TRUE : GL_FALSE;
}

/* The targets an *image* is named by - the uploads, glGetTexImage, glGetTexLevelParameter: a cube
 * map's are its six faces, never GL_TEXTURE_CUBE_MAP itself. */
static GLboolean gl_tex_image_target_ok(GLenum t) {
    return (t == GL_TEXTURE_2D || t == GL_TEXTURE_1D || t == GL_TEXTURE_3D ||
            GL_CUBE_FACE_INDEX(t) >= 0) ? GL_TRUE : GL_FALSE;
}

/* The level an image target names: a face's for a face target, the texture's own otherwise. */
static GLboolean gl_tex_target_view(const gl_texture_object_t *tex, GLenum target, int level,
                                    gl_tex_view_t *out) {
    const int face = GL_CUBE_FACE_INDEX(target);
    return (face >= 0) ? gl_tex_face_view(tex, face, level, out)
                       : gl_tex_level_view(tex, level, out);
}

static gl_texture_object_t *gl_texture_for_target(gl_context_t *ctx, GLenum target,
                                                  GLboolean create) {
    GLuint *slot = gl_binding_slot(ctx, target);
    if (!slot) return NULL;
    const GLuint id = *slot ? *slot : gl_default_texture_id(target);
    gl_texture_object_t *tex = create ? gl_find_or_create_texture(ctx, id)
                                      : gl_find_texture(ctx, id);
    /* A face target belongs to the cube map the object is. */
    if (tex && tex->target == 0u) {
        tex->target = (GL_CUBE_FACE_INDEX(target) >= 0) ? (GLenum)GL_TEXTURE_CUBE_MAP : target;
    }
    return tex;
}

/* The mean of 2^shift eight-bit samples, rounded to nearest with halves to even - what Mesa's
 * software mipmap generation gives, averaging in float and packing through float_to_ubyte
 * (main/mipmap.c:149-180). */
static uint8_t gl_mip_mean(unsigned sum, unsigned shift) {
    unsigned q = sum >> shift;
    const unsigned r = sum & ((1u << shift) - 1u), half = 1u << (shift - 1u);
    if (r > half || (r == half && (q & 1u))) q++;
    return (uint8_t)q;
}

/* **GL_GENERATE_MIPMAP** (GL 1.4, 3.8.8): the levels above the base rebuilt from it, each the box
 * filter of the one below - every 2x2 texel block (2x2x2 in a volume) averaged, a side of 1 reused
 * rather than halved, and a leftover odd row or column dropped, as Mesa's do_row does. As far as
 * GL_TEXTURE_MAX_LEVEL or a 1x1 level, whichever comes first; one cube face at a time, the face
 * whose base changed. The levels land where uploaded ones would, so the software sampler and
 * the hardware's mip chain both see them. */
static void gl_tex_generate_mipmap(gl_context_t *ctx, gl_texture_object_t *tex, int face) {
    const int base = tex->base_level;
    gl_tex_view_t src;
    const GLboolean have = (face >= 0) ? gl_tex_face_view(tex, face, base, &src)
                                       : gl_tex_level_view(tex, base, &src);
    if (!have) return;
    const GLboolean vol = (GLboolean)(tex->target == GL_TEXTURE_3D);
    const GLint internal_format = src.internal_format;
    const GLenum base_format = src.base_format;
    GLsizei w = src.width, h = src.height, d = vol ? src.depth : 1;
    const uint8_t *sp = src.pixels;
    size_t spitch = src.pitch, sslice = src.slice;
    for (int level = base + 1; level < OOPS_GL_MAX_TEXTURE_LEVELS && level <= tex->max_level;
         level++) {
        if (w == 1 && h == 1 && d == 1) break;
        const GLsizei nw = (w > 1) ? w / 2 : 1, nh = (h > 1) ? h / 2 : 1, nd = (d > 1) ? d / 2 : 1;
        uint8_t *dst = (uint8_t *)gl_buffer_alloc((size_t)nw * (size_t)nh * (size_t)nd * 4u);
        if (!dst) {
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        const unsigned shift = vol ? 3u : 2u;
        for (GLsizei z = 0; z < nd; z++) {
            const size_t z0 = (size_t)(vol ? 2 * z : 0);
            const size_t z1 = (size_t)(vol ? ((2 * z + 1 < d) ? 2 * z + 1 : d - 1) : 0);
            for (GLsizei y = 0; y < nh; y++) {
                const size_t y0 = (size_t)(2 * y < h ? 2 * y : h - 1);
                const size_t y1 = (size_t)(2 * y + 1 < h ? 2 * y + 1 : h - 1);
                for (GLsizei x = 0; x < nw; x++) {
                    const size_t x0 = (size_t)(2 * x < w ? 2 * x : w - 1);
                    const size_t x1 = (size_t)(2 * x + 1 < w ? 2 * x + 1 : w - 1);
                    const size_t at[8] = {
                        z0 * sslice + y0 * spitch + x0, z0 * sslice + y0 * spitch + x1,
                        z0 * sslice + y1 * spitch + x0, z0 * sslice + y1 * spitch + x1,
                        z1 * sslice + y0 * spitch + x0, z1 * sslice + y0 * spitch + x1,
                        z1 * sslice + y1 * spitch + x0, z1 * sslice + y1 * spitch + x1};
                    uint8_t *out = dst + (((size_t)z * (size_t)nh + (size_t)y) * (size_t)nw +
                                          (size_t)x) * 4u;
                    /* A depth texture's texel is one float: its levels average depths. */
                    if (base_format == GL_DEPTH_COMPONENT) {
                        float sum = 0.0f;
                        for (unsigned k = 0; k < (1u << shift); k++) {
                            float dv;
                            memcpy(&dv, sp + at[k] * 4u, 4u);
                            sum += dv;
                        }
                        const float mean = sum / (float)(1u << shift);
                        memcpy(out, &mean, 4u);
                        continue;
                    }
                    for (int c = 0; c < 4; c++) {
                        unsigned sum = 0u;
                        for (unsigned k = 0; k < (1u << shift); k++) sum += sp[at[k] * 4u + (size_t)c];
                        out[c] = gl_mip_mean(sum, shift);
                    }
                }
            }
        }
        gl_tex_level_t *lv = (face >= 0) ? &tex->cube[face * OOPS_GL_MAX_TEXTURE_LEVELS + level]
                                         : &tex->mips[level];
        gl_buffer_release(lv->pixels);
        lv->pixels = dst;
        lv->width = nw;
        lv->height = nh;
        lv->depth = nd;
        lv->internal_format = internal_format;
        lv->base_format = base_format;
        sp = dst;
        spitch = (size_t)nw;
        sslice = (size_t)nw * (size_t)nh;
        w = nw;
        h = nh;
        d = nd;
    }
    tex->chain_dirty = GL_TRUE;
}

/* After a level of `target` changed: generate the levels above it when GL_GENERATE_MIPMAP is on
 * and it is the base level below GL_TEXTURE_MAX_LEVEL, Mesa's condition (main/teximage.c:2889-
 * 2897). Held back while a copy uploads row by row (`gen_mipmap_suspend`), which asks once at
 * the end instead. */
static void gl_tex_gen_mipmap_check(gl_context_t *ctx, GLenum target, GLint level) {
    if (ctx->gen_mipmap_suspend > 0 || gl_is_proxy_target(target)) return;
    gl_texture_object_t *tex = gl_texture_for_target(ctx, target, GL_FALSE);
    if (!tex || !tex->generate_mipmap) return;
    if (level != tex->base_level || level >= tex->max_level) return;
    gl_tex_generate_mipmap(ctx, tex, GL_CUBE_FACE_INDEX(target));
}

void glGenTextures(GLsizei n, GLuint *textures) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !textures || n <= 0) return;

    GLuint next_id = 1;
    for (int i = 0; i < n; i++) {
        while (gl_find_texture(ctx, next_id) != NULL) {
            next_id++;
        }
        gl_texture_object_t *tex = gl_find_or_create_texture(ctx, next_id);
        if (tex) {
            textures[i] = next_id;
            gl_pack_descriptors(tex);
            ctx->hw_tex_created++;
            next_id++;
        } else {
            /* **Running out of names is silent on a desktop driver, so nothing checks for it.**
             * A real GL has 2^32 texture names and this has `OOPS_GL_MAX_TEXTURE_OBJECTS`, so
             * exhaustion is a failure mode unique to this implementation - and until now it was
             * reported the way the specification allows and no program reads: a zero in the
             * output array. A port then binds 0, uploads into the default texture, draws, and
             * gets flat surfaces with no error anywhere. That is a day of looking in the wrong
             * place, and it costs three lines to make it say so.
             *
             * GL_OUT_OF_MEMORY is the right code: the specification has no "out of names"
             * because it does not anticipate a fixed pool, and out-of-memory is the general
             * arm for an implementation that cannot satisfy a request. The log line is what
             * actually gets read, though, because the same programs that ignore the zero also
             * never call glGetError(). Once per context - after that the count carries it. */
            textures[i] = 0;
            ctx->hw_tex_failed++;
            if (ctx->hw_tex_failed == 1u) {
                gl_log_line("glGenTextures has no names left: this GL has a fixed pool where a "
                            "desktop driver has 2^32, and a name of 0 is what a caller past the "
                            "end receives - it will upload into the default texture and draw "
                            "untextured. Raise OOPS_GL_MAX_TEXTURE_OBJECTS");
            }
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
        }
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !textures || n <= 0) return;

    for (int i = 0; i < n; i++) {
        GLuint id = textures[i];
        if (id == 0) continue;
        gl_texture_object_t *tex = gl_find_texture(ctx, id);
        if (tex) {
            gl_tex_storage_release_sync(ctx);
#ifndef OOPS_HOST_BUILD
            if (tex->garlic_data) {
                oops_mem_free(tex->garlic_data);
                tex->garlic_data = NULL;
                tex->pixels = NULL;
            }
#else
            if (tex->pixels) {
                free(tex->pixels);
                tex->pixels = NULL;
            }
#endif
            gl_tex_free_mips(tex);
            /* Deleting a bound texture binds 0 in its place - on every target it was bound to,
             * in every unit, as Mesa's unbind_texobj_from_texunits does (main/texobj.c:1381). Only the
             * 2D binding was reset until 2026-09-19, so a deleted 1D texture stayed "bound" by a
             * name that no longer existed. */
            for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
                gl_tex_unit_t *tu = &ctx->tex_unit[u];
                if (tu->bound_texture_2d == id) tu->bound_texture_2d = 0;
                if (tu->bound_texture_1d == id) tu->bound_texture_1d = 0;
                if (tu->bound_texture_3d == id) tu->bound_texture_3d = 0;
                if (tu->bound_texture_cube == id) tu->bound_texture_cube = 0;
            }
            memset(tex, 0, sizeof(*tex));
        }
    }
}

/* The hardware chain's storage: GPU-visible on the target, process memory on the host, where
 * nothing reads it but the descriptor tests. */
static void *gl_chain_alloc(size_t bytes) {
#ifndef OOPS_HOST_BUILD
    return oops_mem_alloc(bytes, 256, OOPS_MEM_WC_GARLIC);
#else
    return gl_buffer_alloc(bytes);
#endif
}

static void gl_chain_release(void *p) {
    if (!p) return;
#ifndef OOPS_HOST_BUILD
    oops_mem_free(p);
#else
    gl_buffer_release(p);
#endif
}

void gl_tex_repack(gl_texture_object_t *tex) {
    gl_pack_descriptors(tex);
}

/* Every mip level's storage and the hardware chain, given back. The base level is the caller's,
 * because it lives in a different allocator on the target. */
void gl_tex_free_mips(gl_texture_object_t *tex) {
    if (!tex) return;
    for (int i = 1; i < OOPS_GL_MAX_TEXTURE_LEVELS; i++) {
        gl_buffer_release(tex->mips[i].pixels);
        tex->mips[i].pixels = NULL;
        tex->mips[i].width = 0;
        tex->mips[i].height = 0;
        tex->mips[i].depth = 0;
    }
    /* A cube map's faces, every level of each, and the table that held them. */
    if (tex->cube) {
        for (int i = 0; i < 6 * OOPS_GL_MAX_TEXTURE_LEVELS; i++) {
            gl_buffer_release(tex->cube[i].pixels);
        }
        gl_buffer_release(tex->cube);
        tex->cube = NULL;
    }
    gl_chain_release(tex->chain_data);
    tex->chain_data = NULL;
    tex->chain_va = 0u;
    tex->desc_chain = GL_FALSE;
}

/* How many levels the hardware should sample through a chain, from the base level - 0 for none,
 * when the base level's own storage serves.
 *
 * A mipmapped texture that is complete and a power of two on both axes takes every level from
 * GL_TEXTURE_BASE_LEVEL to the top level (GL_TEXTURE_MAX_LEVEL may cut it short). **Power of two
 * because the two rounding rules agree only there**: GL halves a level by floor, addrlib by ceil
 * (GetMipSize), and a 3-wide base would be 1 wide at level 1 to GL and 2 wide to the hardware.
 * GL 1.x requires power-of-two textures anyway; one that is not samples its base level alone.
 *
 * **A base level other than 0 is a chain even of one level**, since the image the hardware
 * otherwise samples is level 0's; the descriptor's level 0 is then GL's base level, which is also
 * where GL measures the level of detail from. */
static int gl_tex_chain_levels(const gl_texture_object_t *tex) {
    /* **A volume is sampled from level 0 only, and the reason is now measured rather than
     * structural.** The layout was two-dimensional until 2026-09-21, which was reason enough;
     * `gl_tex_chain_layout_3d` halves depth with width and height and the copy walks slices, so
     * that reason is gone. The hardware still reads the base level: gl1-probe's `volume-mipmap`
     * minifies a 4x4x4 red volume whose level 1 is green, and the console answered
     * `saw 0xffff0000` - red - with the descriptor carrying `LAST_LEVEL` 1 and `MAX_MIP` 1, which
     * `test_pm4_gl_volume_and_cube_sample_on_hardware` pins.
     *
     * So **where a 3D image's mip levels sit is not the 2D rule with a depth term**, and this
     * library does not know what it is. `texture-3d` measures the slice stride *within* level 0
     * and `mipmap-levels` measures the level placement of a *2D* chain; neither measures the
     * placement of a level in a volume, which is what was extrapolated and what the part
     * rejected. `REQ-20260921T1300Z-9b73` asks for it. Until it answers, a volume keeps the
     * behaviour it has always had here and says so in the log, rather than pointing the
     * descriptor at a chain the hardware reads the wrong end of. */
    if (tex->target == GL_TEXTURE_3D) return 0;
    if (!gl_texture_complete(tex)) return 0;
    const int b = tex->base_level;
    gl_tex_view_t bv;
    if (!gl_tex_level_view(tex, b, &bv)) return 0;
    int levels = gl_filter_uses_mipmaps(tex->min_filter) ? gl_tex_top_level(tex) - b + 1 : 1;
    /* **A volume's depth is a third axis to round**, and it goes through the same test as the
     * other two: the two halving rules agree only on a power of two, so a 2-deep volume chains
     * and a 3-deep one samples its base level alone. This read `target == GL_TEXTURE_3D` and
     * returned 0 until 2026-09-21 - no chain at all for a volume, because the layout was two
     * dimensional. It is three now (`gl_tex_chain_layout_3d`). */
    if (levels > 1 && ((bv.width & (bv.width - 1)) != 0 || (bv.height & (bv.height - 1)) != 0)) {
        levels = 1;
    }
    if (levels > 1 && tex->target == GL_TEXTURE_3D &&
        (bv.depth <= 0 || (bv.depth & (bv.depth - 1)) != 0)) {
        levels = 1;
    }
    return (levels == 1 && b == 0) ? 0 : levels;
}

/*
 * **A cube map's six faces uploaded as one array** (since 2026-09-20).
 *
 * The faces arrive one at a time, each its own tight-packed image in process memory
 * (`gl_texture_object_t::cube`), because that is what the software rasteriser samples. The
 * hardware wants one image of six slices: face f at slice f in GL's order - +X, -X, +Y, -Y, +Z,
 * -Z, which is the order the hardware's cube addressing uses too - each slice laid out exactly
 * as a 2D image, with the same 256-byte row pitch every other image here has.
 *
 * Built here rather than as each face arrives, because a face is not a texture: a program gives
 * six of them, and allocating and copying on every `glTexImage2D` would do the work six times
 * and hold a half-built cube in between.
 *
 * **The slice stride is derived, not measured.** obSCEne's `-6c80` sampled a 3D image and a cube
 * on this part, but each reported one texel: nothing in those rows says where the second slice
 * begins. Consecutive slices of `pitch * height` is what addrlib computes for a linear array and
 * what this library's own 3D upload already writes, and the pitch is a multiple of 256 bytes so
 * no slice needs padding to reach an alignment. A stride that is wrong anyway would leave face 0
 * right and the other five wrong, which is the shape this project keeps catching, so it is asked
 * about in `REQ-20260920T1050Z-5d7c` and said out loud here until that answers.
 *
 * Only level 0 of each face: the mip chain for a cube map is a second question, and a chain
 * built from one face would be wrong for the other five.
 */
static void gl_tex_cube_upload(gl_context_t *ctx, gl_texture_object_t *tex) {
    if (!tex->cube) return;
    const gl_tex_level_t *f0 = &tex->cube[0];
    const GLsizei dim = f0->width;
    if (dim <= 0 || f0->height != dim) return; /* no +X face yet, or not square */
    for (int f = 1; f < 6; f++) {
        const gl_tex_level_t *lv = &tex->cube[(size_t)f * OOPS_GL_MAX_TEXTURE_LEVELS];
        /* Every face, all the same size: an incomplete cube map is not sampled by GL at all
         * (2.1, 3.8.10), so there is nothing to describe until the sixth arrives. */
        if (!lv->pixels || lv->width != dim || lv->height != dim) return;
    }

    const size_t pitch_px = ((size_t)dim + 63u) & ~(size_t)63u;
    const size_t slice_px = pitch_px * (size_t)dim;
    const size_t bytes = slice_px * 6u * 4u;

    /* A draw already built into this frame may name the old image; it has to run first. */
    gl_tex_storage_release_sync(ctx);
#ifndef OOPS_HOST_BUILD
    if (tex->garlic_data) {
        oops_mem_free(tex->garlic_data);
        tex->garlic_data = NULL;
    }
    tex->garlic_data = oops_mem_alloc(bytes, 256, OOPS_MEM_WC_GARLIC);
#else
    free(tex->garlic_data);
    tex->garlic_data = malloc(bytes);
#endif
    if (!tex->garlic_data) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    tex->garlic_va = (uint64_t)(uintptr_t)tex->garlic_data;
    tex->pitch = (uint32_t)pitch_px;
    /* Zeroed first: the padding between each row's pixels and the pitch is written by nothing
     * below, and the sampler would otherwise read whatever the allocator left there. */
    memset(tex->garlic_data, 0, bytes);
    for (int f = 0; f < 6; f++) {
        const gl_tex_level_t *lv = &tex->cube[(size_t)f * OOPS_GL_MAX_TEXTURE_LEVELS];
        const uint8_t *src = (const uint8_t *)lv->pixels;
        uint8_t *dst = (uint8_t *)tex->garlic_data + (size_t)f * slice_px * 4u;
        for (size_t y = 0; y < (size_t)dim; y++) {
            memcpy(dst + y * pitch_px * 4u, src + y * (size_t)dim * 4u, (size_t)dim * 4u);
        }
    }
#if !defined(OOPS_HOST_BUILD) && defined(__x86_64__)
    for (size_t p = 0; p < bytes; p += 64) {
        __builtin_ia32_clflush((const void *)((const char *)tex->garlic_data + p));
    }
#endif
    tex->cube_hw_dim = dim;
    tex->cube_hw_dirty = GL_FALSE;
    gl_pack_descriptors(tex);
}

void gl_tex_hw_prepare(gl_context_t *ctx, gl_texture_object_t *tex) {
    if (!ctx || !tex) return;
    if (tex->target == GL_TEXTURE_CUBE_MAP && (tex->cube_hw_dirty || !tex->garlic_data)) {
        gl_tex_cube_upload(ctx, tex);
        return; /* a cube map has no mip chain here - see gl_tex_cube_upload */
    }
    const int levels = gl_tex_chain_levels(tex);
    const GLboolean want = (GLboolean)(levels > 0);
    const int b = tex->base_level;
    if (want && (tex->chain_dirty || !tex->chain_data || tex->chain_base != b ||
                 tex->chain_levels != levels)) {
        /* A draw already built into this frame may name the old chain; it has to run first. */
        gl_tex_storage_release_sync(ctx);
        gl_chain_release(tex->chain_data);
        tex->chain_data = NULL;

        gl_tex_view_t bv;
        (void)gl_tex_level_view(tex, b, &bv);
        size_t offsets[OOPS_GL_MAX_TEXTURE_LEVELS];
        uint32_t pitches[OOPS_GL_MAX_TEXTURE_LEVELS];
        const GLsizei bdepth = (tex->target == GL_TEXTURE_3D && bv.depth > 0) ? bv.depth : 1;
        const size_t total =
            gl_tex_chain_layout_3d(bv.width, bv.height, bdepth, levels, offsets, pitches);
        uint8_t *chain = (uint8_t *)gl_chain_alloc(total);
        if (!chain) {
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            tex->desc_chain = GL_FALSE;
            gl_pack_descriptors(tex);
            return;
        }
        memset(chain, 0, total);
        for (int i = 0; i < levels; i++) {
            gl_tex_view_t lv;
            if (!gl_tex_level_view(tex, b + i, &lv)) continue; /* complete, so never */
            /* **Slice by slice, and a 2D texture is one slice.** The source is tight-packed at
             * `width * height` a slice, which is how every level is stored here; the destination
             * pads each row to the level's pitch and puts the slices one after another at
             * `pitch * height`, which is where `texture-3d` measured the hardware reading
             * slice 1. A 2D level runs this loop once and copies exactly the rows it always
             * did. */
            const GLsizei slices = (tex->target == GL_TEXTURE_3D && lv.depth > 0) ? lv.depth : 1;
            const size_t dst_slice = (size_t)pitches[i] * (size_t)lv.height * 4u;
            const size_t src_slice = (size_t)lv.pitch * (size_t)lv.height * 4u;
            for (GLsizei z = 0; z < slices; z++) {
                for (GLsizei y = 0; y < lv.height; y++) {
                    memcpy(chain + offsets[i] + (size_t)z * dst_slice + (size_t)y * pitches[i] * 4u,
                           lv.pixels + (size_t)z * src_slice + (size_t)y * lv.pitch * 4u,
                           (size_t)lv.width * 4u);
                }
            }
        }
#if !defined(OOPS_HOST_BUILD) && defined(__x86_64__)
        for (size_t p = 0; p < total; p += 64) {
            __builtin_ia32_clflush((const void *)(chain + p));
        }
#endif
        tex->chain_data = chain;
        tex->chain_va = (uint64_t)(uintptr_t)chain;
        tex->chain_dirty = GL_FALSE;
        tex->chain_base = b;
        tex->chain_levels = levels;
        tex->desc_chain = GL_TRUE;
        gl_pack_descriptors(tex);
        return;
    }
    if (want != tex->desc_chain) {
        tex->desc_chain = want;
        gl_pack_descriptors(tex);
    }
}

void glBindTexture(GLenum target, GLuint texture) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_BIND_TEXTURE, gl_la_e(target), gl_la_u(texture))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* An object's target, never one of a cube map's faces. */
    GLuint *slot = gl_texture_target_ok(target) ? gl_binding_slot(ctx, target) : (GLuint *)0;
    if (!slot) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }

    if (texture == 0) {
        *slot = 0;
        return;
    }

    gl_texture_object_t *tex = gl_find_or_create_texture(ctx, texture);
    if (!tex) return;

    /* **An object belongs to the target it was first bound to.** Rebinding it elsewhere is
     * GL_INVALID_OPERATION, not a silent reinterpretation: the image inside has a shape the other
     * target cannot read, and handing it over would sample a 1D image as a 2D one. */
    if (tex->target != 0u && tex->target != target) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    tex->target = target;
    *slot = texture;
}

/* The stride between source rows, honouring glPixelStorei.
 *
 * `GL_UNPACK_ROW_LENGTH` gives the row width in pixels when it is not the width being uploaded -
 * which is what makes a sub-rectangle of a larger client image uploadable at all - and
 * `GL_UNPACK_ALIGNMENT` rounds each row's start up to 1, 2, 4 or 8 bytes. Both default to the
 * values the specification gives (0 and 4), so a caller that never calls glPixelStorei gets
 * exactly the behaviour this had before they existed. */
size_t gl_unpack_row_stride(const gl_context_t *ctx, GLsizei width, size_t pixel_bytes) {
    size_t row_pixels = ctx->unpack_row_length > 0 ? (size_t)ctx->unpack_row_length : (size_t)width;
    return gl_unpack_row_stride_bytes(ctx, row_pixels * pixel_bytes);
}

/* The same alignment rule for a row whose size is already in bytes, which is what a bitmap has:
 * one bit per pixel does not divide into a pixel size. */
size_t gl_unpack_row_stride_bytes(const gl_context_t *ctx, size_t bytes) {
    size_t align = ctx->unpack_alignment > 0 ? (size_t)ctx->unpack_alignment : 1u;
    size_t remainder = bytes % align;
    return remainder == 0u ? bytes : bytes + (align - remainder);
}

/* A client image copied into a list through the unpack state of now - format, type, skips,
 * swapping and all (gl_pixel_copy_client) - and replayed under neutral state. A format and type
 * the call would refuse keep no pixels, and the replay refuses exactly as the call would have. */
GLboolean gl_list_rec_image(gl_list_op_t op, const gl_list_arg_t *args, int nargs,
                            GLsizei width, GLsizei height, GLenum format, GLenum type,
                            const GLvoid *pixels) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GL_FALSE;
    gl_pixel_fmt_t f;
    void *img = (void *)0;
    size_t img_bytes = 0u;
    if (pixels && gl_pixel_fmt(format, type, &f) == GL_NO_ERROR && width > 0 && height > 0) {
        img = gl_pixel_copy_client(ctx, &f, pixels, width, height, 1);
        /* Out of memory, already recorded. Nothing is kept - a command replayed without its
         * image would upload garbage - and under GL_COMPILE_AND_EXECUTE the call still runs. */
        if (!img) return (GLboolean)(ctx->list_mode == GL_COMPILE);
        img_bytes = gl_pixel_packed_bytes(&f, width, height, 1);
    }
    return gl_list_rec_owned(op, args, nargs, img, img_bytes);
}

/* `glAlphaFunc(func, ref)` - discards a fragment whose alpha fails the comparison.
 *
 * RDNA2 has no fixed-function alpha test; it was removed after the fixed-function era, and a
 * modern driver implements it by discarding in the pixel shader. So this records the state and
 * `gl_ps_patch_alpha_test` writes the comparison into both shaders - which is why a function
 * this does not know is refused here rather than silently doing nothing: the shader would
 * carry no test and every fragment would pass, which is a picture rather than an error.
 */
void glAlphaFunc(GLenum func, GLclampf ref) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_ALPHA_FUNC, gl_la_e(func), gl_la_f(ref))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    switch (func) {
        case GL_NEVER:
        case GL_LESS:
        case GL_EQUAL:
        case GL_LEQUAL:
        case GL_GREATER:
        case GL_NOTEQUAL:
        case GL_GEQUAL:
        case GL_ALWAYS:
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return;
    }
    float clamped = (float)ref;
    if (clamped < 0.0f) clamped = 0.0f;
    if (clamped > 1.0f) clamped = 1.0f;
    ctx->alpha_func = func;
    ctx->alpha_ref = clamped;
    gl_ps_patch_alpha_test(ctx);
}

/* `glPolygonOffset(factor, units)` - nudges a filled polygon's depth so coplanar geometry can
 * be drawn over it without fighting.
 *
 * Both are kept as given and scaled where they are written, because the scaling depends on the
 * depth format rather than on the call: Mesa multiplies the factor by 16 for every format, and
 * the units by 4, 2 or 1 for 16-, 24- and 32-bit z-buffers. This one is 32-bit float, so the
 * units go in unscaled - see the DB_FMT_CNTL comment in `gl_draw.c` for how that was settled.
 *
 * Takes no error: the specification defines no error for this call, and any float pair is
 * meaningful - including negative, which pulls geometry towards the viewer. */
/* The colour-index clear value and write mask: RGBA-context state, kept and reported. The clear
 * value is not clamped - an index is a number, not an intensity. */
void glClearIndex(GLfloat c) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_CLEAR_INDEX, gl_la_f(c))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->clear_index = c;
}

void glIndexMask(GLuint mask) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_INDEX_MASK, gl_la_u(mask))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->index_mask = mask;
}

/* The coverage value is clamped to [0, 1] as Mesa does (main/multisample.c:49). With no
 * multisample buffer it has no effect, which the specification says is what it then does. */
void glSampleCoverage(GLclampf value, GLboolean invert) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_SAMPLE_COVERAGE, gl_la_f(value), gl_la_u(invert))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (!(value >= 0.0f)) value = 0.0f; /* also NaN */
    if (value > 1.0f) value = 1.0f;
    ctx->sample_coverage_value = value;
    ctx->sample_coverage_invert = invert ? GL_TRUE : GL_FALSE;
}

/* How each face is drawn. Validated as Mesa does (main/polygon.c:159-187): the three modes, the
 * three face names, and anything else GL_INVALID_ENUM with the state untouched. The drawing
 * itself is gl_draw_polygon_tri's. */
void glPolygonMode(GLenum face, GLenum mode) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_POLYGON_MODE, gl_la_e(face), gl_la_e(mode))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    switch (face) {
        case GL_FRONT:          ctx->polygon_mode[0] = mode; break;
        case GL_BACK:           ctx->polygon_mode[1] = mode; break;
        case GL_FRONT_AND_BACK: ctx->polygon_mode[0] = mode; ctx->polygon_mode[1] = mode; break;
        default: gl_record_error(ctx, GL_INVALID_ENUM); break;
    }
}

void glPolygonOffset(GLfloat factor, GLfloat units) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_POLYGON_OFFSET, gl_la_f(factor), gl_la_f(units))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->polygon_offset_factor = factor;
    ctx->polygon_offset_units = units;
}

/* The float spelling of the same call. Every pixel-store parameter this has is an integer, so
 * this narrows and forwards rather than carrying a second representation. */
void glPixelStoref(GLenum pname, GLfloat param) {
    glPixelStorei(pname, (GLint)param);
}

void glPixelStorei(GLenum pname, GLint param) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    switch (pname) {
        case GL_UNPACK_ALIGNMENT:
            /* The specification's four values and no others; anything else would silently
             * misplace every row after the first. */
            if (param != 1 && param != 2 && param != 4 && param != 8) {
                gl_record_error(ctx, GL_INVALID_VALUE);
                return;
            }
            ctx->unpack_alignment = param;
            break;
        case GL_UNPACK_ROW_LENGTH:
            if (param < 0) {
                gl_record_error(ctx, GL_INVALID_VALUE);
                return;
            }
            ctx->unpack_row_length = param;
            break;
        case GL_PACK_ALIGNMENT:
            /* The other direction: how glReadPixels lays rows out in the caller's buffer. */
            if (param != 1 && param != 2 && param != 4 && param != 8) {
                gl_record_error(ctx, GL_INVALID_VALUE);
                return;
            }
            ctx->pack_alignment = param;
            break;
        /* Rows per slice of a volume in the caller's memory (GL 1.2), 0 meaning the image's own
         * height; the skips; the pack side's row length. All counts, so none may be negative.
         * Refused until 2026-09-19 - and a refused skip is worse than a missing feature: the
         * program's glTexSubImage2D then reads its sub-rectangle from the image's corner. */
        case GL_UNPACK_IMAGE_HEIGHT:
        case GL_PACK_IMAGE_HEIGHT:
        case GL_UNPACK_SKIP_ROWS:
        case GL_UNPACK_SKIP_PIXELS:
        case GL_UNPACK_SKIP_IMAGES:
        case GL_PACK_ROW_LENGTH:
        case GL_PACK_SKIP_ROWS:
        case GL_PACK_SKIP_PIXELS:
        case GL_PACK_SKIP_IMAGES:
            if (param < 0) {
                gl_record_error(ctx, GL_INVALID_VALUE);
                return;
            }
            switch (pname) {
                case GL_UNPACK_IMAGE_HEIGHT: ctx->unpack_image_height = param; break;
                case GL_PACK_IMAGE_HEIGHT:   ctx->pack_image_height = param; break;
                case GL_UNPACK_SKIP_ROWS:    ctx->unpack_skip_rows = param; break;
                case GL_UNPACK_SKIP_PIXELS:  ctx->unpack_skip_pixels = param; break;
                case GL_UNPACK_SKIP_IMAGES:  ctx->unpack_skip_images = param; break;
                case GL_PACK_ROW_LENGTH:     ctx->pack_row_length = param; break;
                case GL_PACK_SKIP_ROWS:      ctx->pack_skip_rows = param; break;
                case GL_PACK_SKIP_PIXELS:    ctx->pack_skip_pixels = param; break;
                default:                     ctx->pack_skip_images = param; break;
            }
            break;
        case GL_UNPACK_SWAP_BYTES: ctx->unpack_swap_bytes = (GLboolean)(param != 0); break;
        case GL_UNPACK_LSB_FIRST:  ctx->unpack_lsb_first = (GLboolean)(param != 0); break;
        case GL_PACK_SWAP_BYTES:   ctx->pack_swap_bytes = (GLboolean)(param != 0); break;
        case GL_PACK_LSB_FIRST:    ctx->pack_lsb_first = (GLboolean)(param != 0); break;
        default:
            /* Every GL 1.2 parameter is above; anything else names nothing. */
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

/* `glReadPixels(x, y, width, height, format, type, pixels)` - the framebuffer, read back.
 *
 * # The y flip, which is the whole of what makes this easy to get wrong
 *
 * **GL's window origin is the bottom-left corner; this framebuffer's row 0 is the top.** So row
 * `y` of the requested rectangle is row `height_fb - 1 - y` of memory, and a reader that forgets
 * it gets a vertically mirrored image - which looks like a plausible screenshot, so nothing
 * downstream notices. The same flip is why `glTexImage2D` does *not* flip: a texture's rows are
 * the caller's to order, a window's are not.
 *
 * # Where the pixels come from
 *
 * On target, the command processor's own copy of the render target if a frame has been
 * submitted and confirmed - it is CPU-cached, where the render target is write-combined and
 * slow to read. Otherwise the framebuffer itself, which is what a host build always uses.
 *
 * # Out of bounds is zero, not whatever was next in memory
 *
 * The specification leaves pixels outside the framebuffer undefined, and undefined is allowed
 * to be zero. Writing whatever happened to follow the framebuffer would be permitted too and is
 * the kind of plausible garbage this subsystem refuses to hand back, so the destination is
 * cleared first and only the part that overlaps is filled.
 */
/* Every format and type gl_pixel.c knows, through every pack parameter - GL 1.2's skips, row
 * length, image height and byte swapping as well as the alignment. Only the unsigned-byte
 * formats were written until 2026-09-19. */
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *pixels) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (width < 0 || height < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    gl_pixel_fmt_t f;
    const GLenum fmt_err = gl_pixel_fmt(format, type, &f);
    if (fmt_err != GL_NO_ERROR) {
        gl_record_error(ctx, fmt_err);
        return;
    }
    /* An RGBA colour buffer holds no indices to read (GL 1.x, 4.3.2). */
    if (f.kind == GL_COLOR_INDEX) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (width == 0 || height == 0 || !pixels) return;

    /* **Depth and stencil** (GL_DEPTH_COMPONENT, GL_STENCIL_INDEX), refused until 2026-09-19 -
     * though they are GL 1.0's, and reading the depth under the cursor is how a program unprojects
     * a click. Each value through its own transfer (gl_pack_value); pixels outside the window are
     * undefined in GL and written as 0. On the console the flush finishes the frame, and the
     * values are read out of the GPU's tiled surfaces (gl_zs_depth_ptr). */
    if (f.kind != GL_COLOR) {
        glFlush();
        const float *zb = ctx->depth_buffer;
        const uint8_t *sb = ctx->stencil_buffer;
        if ((f.kind == GL_DEPTH && !zb) || (f.kind == GL_STENCIL && !sb)) {
            gl_record_error(ctx, GL_INVALID_OPERATION); /* no such buffer */
            return;
        }
        gl_pixel_dst_t vd;
        gl_pack_dest(ctx, &f, pixels, width, height, &vd);
        for (GLsizei row = 0; row < height; row++) {
            const GLint wy = y + row;
            for (GLsizei col = 0; col < width; col++) {
                const GLint wx = x + col;
                float v = 0.0f;
                if (wy >= 0 && wy < (GLint)ctx->height && wx >= 0 && wx < (GLint)ctx->width) {
                    v = (f.kind == GL_DEPTH) ? *gl_zs_depth_ptr(ctx, wx, wy)
                                             : (float)*gl_zs_stencil_ptr(ctx, wx, wy);
                }
                uint8_t *rowp = vd.base + (size_t)row * vd.row_stride;
                if (f.bitmap) {
                    /* A stencil index as one bit - its lowest, after the transfer. */
                    const int64_t st = ctx->pixel_transfer_suspend
                                           ? (int64_t)v : gl_stencil_transfer(ctx, (int64_t)v);
                    gl_pack_bit(ctx, rowp, col, (GLboolean)(st & 1));
                    continue;
                }
                gl_pack_value(ctx, &f, v, vd.swap, rowp + (size_t)col * f.pixel_bytes);
            }
        }
        return;
    }

    /* **Everything issued before this call has to have happened.** The specification requires it,
     * and on the hardware path it is not free: drawing builds a command stream and returns, so
     * without the flush this would read a target the GPU has not been told to draw into yet and
     * report the previous frame - or, before any frame, the buffer as the display left it. The
     * readback the source below prefers is filled by that same submission, so the flush is also
     * what makes it current rather than one frame stale. On the host the flush costs nothing
     * beyond closing an open glBegin, which the specification also wants. */
    glFlush();

    /* The buffer glReadBuffer names - through the CP's cached copy when that is current. */
    const uint32_t *source = gl_color_read_source(ctx, gl_read_target(ctx));
    if (!source) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }

    gl_pixel_dst_t d;
    gl_pack_dest(ctx, &f, pixels, width, height, &d);
    const GLboolean transfer = gl_pixel_transfer_active(ctx);
    /* **Luminance read from colour is R + G + B**, clamped, not R alone - the specification's
     * conversion, which Mesa applies whenever an RGB buffer is read as luminance
     * (main/readpix.c:484, pack.c:1297). A grey pixel reads the same either way; pure blue read
     * as 0 with R alone, where GL reads it as 1. gl_pack_pixel_f's `lum_sum`. */
    static const float outside[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    for (GLsizei row = 0; row < height; row++) {
        /* **Only the row's own pixels are written** - out-of-window ones as zero - never the
         * alignment padding after them. This zeroed `stride * height` up front until
         * 2026-09-19, which wrote the *last* row's padding too: a 1x1 GL_RGB read at the default
         * alignment of 4 wrote four bytes into the three the caller had sized for it, and
         * AddressSanitizer caught it in this file's own test. The padding between rows is the
         * caller's memory as well; GL leaves it alone and so does this. */
        uint8_t *out = d.base + (size_t)row * d.row_stride;
        GLint window_y = y + row;
        const GLboolean row_in = (GLboolean)(window_y >= 0 && window_y < (GLint)ctx->height);
        for (GLsizei col = 0; col < width; col++) {
            GLint window_x = x + col;
            uint8_t *px = out + (size_t)col * f.pixel_bytes;
            if (!row_in || window_x < 0 || window_x >= (GLint)ctx->width) {
                gl_pack_pixel_f(&f, outside, GL_FALSE, d.swap, px);
                continue;
            }
            /* The flip, and on the scanout path the swizzle, are gl_color_index's. */
            const size_t at = gl_color_index(ctx, window_x, window_y);
            /* **And when `source` is the CP's copy, the line has to be dropped first**, because
             * a DMA filled it behind the CPU's back. Without this the read returns whatever the
             * CPU had cached of an earlier frame - see gl_color_copy_invalidate_word, and the
             * drifting bytes that sent two suites chasing a blend. */
            gl_color_copy_invalidate_word(ctx, source, at);
            const uint32_t argb = source[at];
            float c[4] = {(float)((argb >> 16) & 0xffu) / 255.0f,
                          (float)((argb >> 8) & 0xffu) / 255.0f,
                          (float)(argb & 0xffu) / 255.0f,
                          (float)((argb >> 24) & 0xffu) / 255.0f};
            if (transfer) gl_pixel_transfer_rgbaf(ctx, c);
            gl_pack_pixel_f(&f, c, GL_TRUE, d.swap, px);
        }
    }
}

/* -------------------------------------------------------------------------
 * Buffer objects (GL 1.5)
 *
 * A named block of memory the vertex arrays and the index array are read out of. The storage
 * here is ordinary process memory rather than a GPU allocation: everything that reads it is the
 * CPU-side array reader, and the hardware path copies the vertices it assembles into its own
 * allocation afterwards regardless of where they came from. A GPU-resident buffer would save
 * that copy and is a measurable optimisation rather than a correctness question.
 * ------------------------------------------------------------------------- */

/* The allocator differs by build: a freestanding target has the SDK's own heap, and the host
 * test build has the C library's. Wrapped here so the five call sites below do not each carry
 * the #if. */
void *gl_heap_alloc(size_t bytes) {
#ifdef OOPS_HOST_BUILD
    return malloc(bytes);
#else
    return oops_malloc(bytes);
#endif
}

void gl_heap_free(void *p) {
    if (!p) return;
#ifdef OOPS_HOST_BUILD
    free(p);
#else
    oops_free(p);
#endif
}

/* The buffer objects' own names for the pair, kept so the call sites below read as they did and
 * so there is one place that knows which heap this build has. */
static void *gl_buffer_alloc(size_t bytes) { return gl_heap_alloc(bytes); }
static void gl_buffer_release(void *p) { gl_heap_free(p); }

gl_buffer_object_t *gl_find_buffer(gl_context_t *ctx, GLuint name) {
    if (!ctx || name == 0u) return NULL;
    for (int i = 0; i < OOPS_GL_MAX_BUFFER_OBJECTS; i++) {
        if (ctx->buffers[i].used && ctx->buffers[i].id == name) {
            return &ctx->buffers[i];
        }
    }
    return NULL;
}

void gl_free_all_buffers(gl_context_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < OOPS_GL_MAX_BUFFER_OBJECTS; i++) {
        gl_buffer_release(ctx->buffers[i].data);
        ctx->buffers[i].data = NULL;
        ctx->buffers[i].size = 0;
        ctx->buffers[i].used = GL_FALSE;
    }
    ctx->bound_array_buffer = 0u;
    ctx->bound_element_array_buffer = 0u;
}

/* Which binding a target names. Returns NULL for a target this does not have, so the callers
 * can refuse uniformly rather than each deciding what an unknown target means. */
static GLuint *gl_buffer_binding(gl_context_t *ctx, GLenum target) {
    switch (target) {
        case GL_ARRAY_BUFFER:         return &ctx->bound_array_buffer;
        case GL_ELEMENT_ARRAY_BUFFER: return &ctx->bound_element_array_buffer;
        default:                      return NULL;
    }
}

const uint8_t *gl_array_base(const gl_context_t *ctx, const gl_client_array_t *a) {
    if (!ctx || !a) return NULL;
    if (a->buffer == 0u) {
        /* **An offset with no buffer bound is not an address, and dereferencing it faults.**
         * With a buffer bound `pointer` holds a byte offset; with none, GL says it is a client
         * pointer and this has to honour that. But a program that ignored a failed
         * `glGenBuffers` - Neverball, 2026-09-23 - binds 0 and then passes its offsets anyway,
         * and the first page is never mapped, so what arrives here is a small integer and the
         * array reader took a SIGSEGV on address 8 rather than drawing anything.
         *
         * Treating that as "no array" gives the draw its default attribute instead, which is
         * wrong on screen and diagnosable off it: `glGenBuffers` has already raised
         * GL_OUT_OF_MEMORY, and a log with that error and a strange-looking frame is a better
         * place to start than a fault address. No real client array lives in the first page. */
        if ((uintptr_t)a->pointer != 0u && (uintptr_t)a->pointer < 4096u) return NULL;
        return (const uint8_t *)a->pointer;
    }

    /* **The name is resolved now, not when the pointer was set.** glBufferData may have
     * replaced the storage since - respecifying a buffer every frame is ordinary - so an
     * address captured at glVertexPointer time would be stale.
     *
     * **Indexed, not searched.** `glGenBuffers` is the only thing that creates a buffer and it
     * names slot `i` as `i + 1`; `glBindBuffer` refuses a name it did not hand out, so the name
     * is the slot and always has been. This walked all 1024 entries per array per vertex until
     * 2026-09-23, which is why the table could not grow. The identity is still checked rather
     * than assumed, so a future creator that breaks it fails a draw instead of reading the
     * wrong buffer. */
    const GLuint id = a->buffer;
    if (id >= 1u && id <= (GLuint)OOPS_GL_MAX_BUFFER_OBJECTS) {
        const gl_buffer_object_t *buf = &ctx->buffers[id - 1u];
        if (buf->used && buf->id == id) {
            if (!buf->data) return NULL;
            return (const uint8_t *)buf->data + (uintptr_t)a->pointer;
        }
    }
    return NULL; /* the buffer was deleted out from under the array */
}

void glGenBuffers(GLsizei n, GLuint *buffers) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !buffers) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    GLsizei made = 0;
    for (int i = 0; i < OOPS_GL_MAX_BUFFER_OBJECTS && made < n; i++) {
        if (ctx->buffers[i].used) continue;
        ctx->buffers[i].used = GL_TRUE;
        ctx->buffers[i].id = (GLuint)(i + 1);
        ctx->buffers[i].data = NULL;
        ctx->buffers[i].size = 0;
        ctx->buffers[i].usage = GL_STATIC_DRAW;
        ctx->buffers[i].mapped = GL_FALSE;
        ctx->buffers[i].access = GL_READ_WRITE; /* GL 1.5's initial GL_BUFFER_ACCESS */
        buffers[made++] = ctx->buffers[i].id;
    }
    /* Ran out. The names already handed back are real; the rest are zeroed rather than left as
     * the caller found them, so a program that ignores the error still binds nothing instead of
     * binding whatever was in its array. */
    if (made < n) {
        for (GLsizei i = made; i < n; i++) buffers[i] = 0u;
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
    }
}

void glDeleteBuffers(GLsizei n, const GLuint *buffers) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !buffers) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    for (GLsizei i = 0; i < n; i++) {
        gl_buffer_object_t *buf = gl_find_buffer(ctx, buffers[i]);
        if (!buf) continue; /* name 0 and unknown names are silently ignored, as GL says */
        gl_buffer_release(buf->data);
        buf->data = NULL;
        buf->size = 0;
        buf->used = GL_FALSE;
        buf->id = 0u;
        buf->mapped = GL_FALSE; /* a mapped buffer is unmapped by its deletion */

        /* **A deleted buffer that was bound reverts the binding to 0.** Leaving it bound would
         * have the next glBufferData look up a name nothing owns; GL specifies the unbind. */
        if (ctx->bound_array_buffer == buffers[i]) ctx->bound_array_buffer = 0u;
        if (ctx->bound_element_array_buffer == buffers[i]) ctx->bound_element_array_buffer = 0u;
    }
}

void glBindBuffer(GLenum target, GLuint buffer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* Binding 0 means "back to client memory" and is always legal. Any other name has to be one
     * glGenBuffers handed out: GL lets an implementation accept an unknown name and create the
     * object, but doing so quietly turns a typo into a working-looking buffer full of nothing. */
    if (buffer != 0u && !gl_find_buffer(ctx, buffer)) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    *binding = buffer;
}

void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* All nine of GL 1.5's usages - the _READ and _COPY ones were refused until 2026-09-19. A
     * hint, which process memory has no use for; kept and reported. */
    switch (usage) {
        case GL_STREAM_DRAW: case GL_STREAM_READ: case GL_STREAM_COPY:
        case GL_STATIC_DRAW: case GL_STATIC_READ: case GL_STATIC_COPY:
        case GL_DYNAMIC_DRAW: case GL_DYNAMIC_READ: case GL_DYNAMIC_COPY:
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return;
    }
    if (size < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    gl_buffer_object_t *buf = gl_find_buffer(ctx, *binding);
    if (!buf) {
        /* Nothing bound. GL reserves name 0 as "no buffer", so there is no object to fill. */
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }

    /* **Replaced, not resized.** glBufferData respecifies the whole store, so the old one goes
     * even when the size is unchanged - and the arrays pointing at this buffer find the new
     * storage because they kept the name rather than an address. */
    gl_buffer_release(buf->data);
    buf->data = NULL;
    buf->size = 0;
    buf->usage = usage;
    /* A new store unmaps the old one - the pointer it handed out is gone with it. */
    buf->mapped = GL_FALSE;
    buf->access = GL_READ_WRITE;
    if (size == 0) return;

    buf->data = gl_buffer_alloc((size_t)size);
    if (!buf->data) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    buf->size = size;
    if (data) {
        memcpy(buf->data, data, (size_t)size);
    } else {
        /* A null pointer reserves the store, which is how a program sets a buffer up to fill
         * with glBufferSubData. Zeroed rather than left as the allocator found it, so a draw
         * before the first update reads zeroes instead of whatever was in that memory. */
        memset(buf->data, 0, (size_t)size);
    }
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_buffer_object_t *buf = gl_find_buffer(ctx, *binding);
    if (!buf || !buf->data) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* **Bounds are an error, not a clamp**, and the addition is done in a width that cannot
     * wrap past the check: offset and size are both signed and both already known to be
     * non-negative, so `offset + size` compared against the store is safe here. */
    if (offset < 0 || size < 0 || offset + size > buf->size) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* Not while mapped (GL 1.5, 2.9; Mesa's buffer_object_subdata_range_good). */
    if (buf->mapped) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (size == 0 || !data) return;
    memcpy((uint8_t *)buf->data + offset, data, (size_t)size);
}

/* GL 1.5's read back of a buffer's store, checked as glBufferSubData is - the target, a bound
 * buffer, the range, and not while mapped (Mesa main/bufferobj.c:2671-2687). */
void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, GLvoid *data) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const gl_buffer_object_t *buf = gl_find_buffer(ctx, *binding);
    if (!buf) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (offset < 0 || size < 0 || offset + size > buf->size) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (buf->mapped) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (size == 0 || !data || !buf->data) return;
    memcpy(data, (const uint8_t *)buf->data + offset, (size_t)size);
}

/* **GL 1.5's mapping.** The store is process memory, so the pointer handed out is the store
 * itself - reads see the data, writes are the data, with nothing to copy on unmap. Checked in
 * Mesa's order (main/bufferobj.c:3876-3897): the access (an enum error), the target, a bound
 * buffer, and not already mapped (each GL_INVALID_OPERATION). A buffer with no store maps to
 * NULL. */
GLvoid *glMapBuffer(GLenum target, GLenum access) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return NULL;
    if (access != GL_READ_ONLY && access != GL_WRITE_ONLY && access != GL_READ_WRITE) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return NULL;
    }
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return NULL;
    }
    gl_buffer_object_t *buf = gl_find_buffer(ctx, *binding);
    if (!buf || buf->mapped) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return NULL;
    }
    buf->mapped = GL_TRUE;
    buf->access = access;
    return buf->data;
}

/* GL_TRUE, since process memory cannot lose its contents while mapped; GL_FALSE with
 * GL_INVALID_OPERATION for a buffer that was not mapped (Mesa main/bufferobj.c:3059-3069). */
GLboolean glUnmapBuffer(GLenum target) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GL_FALSE;
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return GL_FALSE;
    }
    gl_buffer_object_t *buf = gl_find_buffer(ctx, *binding);
    if (!buf || !buf->mapped) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    buf->mapped = GL_FALSE;
    return GL_TRUE;
}

/* The mapped pointer, NULL while unmapped; GL_BUFFER_MAP_POINTER the only name (Mesa
 * main/bufferobj.c:3270-3286). */
void glGetBufferPointerv(GLenum target, GLenum pname, GLvoid **params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (pname != GL_BUFFER_MAP_POINTER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const gl_buffer_object_t *buf = gl_find_buffer(ctx, *binding);
    if (!buf) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    params[0] = buf->mapped ? buf->data : NULL;
}

/* **GL_ARB_vertex_buffer_object's own spellings** (2026-09-19). The extension came before GL 1.5
 * took it into the core, so a program of that era calls these names. Each is the core function;
 * the extension's types are the core's (`GLsizeiptrARB` is `GLsizeiptr`). This is what lets
 * glGetString advertise the extension, which it would not for behaviour alone. */
void glBindBufferARB(GLenum target, GLuint buffer) { glBindBuffer(target, buffer); }
void glDeleteBuffersARB(GLsizei n, const GLuint *buffers) { glDeleteBuffers(n, buffers); }
void glGenBuffersARB(GLsizei n, GLuint *buffers) { glGenBuffers(n, buffers); }
GLboolean glIsBufferARB(GLuint buffer) { return glIsBuffer(buffer); }
void glBufferDataARB(GLenum target, GLsizeiptrARB size, const GLvoid *data, GLenum usage) {
    glBufferData(target, size, data, usage);
}
void glBufferSubDataARB(GLenum target, GLintptrARB offset, GLsizeiptrARB size, const GLvoid *data) {
    glBufferSubData(target, offset, size, data);
}
void glGetBufferSubDataARB(GLenum target, GLintptrARB offset, GLsizeiptrARB size, GLvoid *data) {
    glGetBufferSubData(target, offset, size, data);
}
void *glMapBufferARB(GLenum target, GLenum access) { return glMapBuffer(target, access); }
GLboolean glUnmapBufferARB(GLenum target) { return glUnmapBuffer(target); }
void glGetBufferParameterivARB(GLenum target, GLenum pname, GLint *params) {
    glGetBufferParameteriv(target, pname, params);
}
void glGetBufferPointervARB(GLenum target, GLenum pname, GLvoid **params) {
    glGetBufferPointerv(target, pname, params);
}

/* Whether a draw would source a mapped buffer - an enabled array's, or the bound element buffer
 * when `elements` - which GL 1.5 makes GL_INVALID_OPERATION (2.9; Mesa's draw validation checks
 * every bound array buffer, main/draw.c). */
GLboolean gl_draw_sources_mapped(gl_context_t *ctx, GLboolean elements) {
    const gl_client_array_t *arrays[7 + OOPS_GL_MAX_TEXTURE_UNITS] = {
        &ctx->array_vertex, &ctx->array_color, &ctx->array_normal,
        &ctx->array_edge_flag, &ctx->array_index, &ctx->array_secondary, &ctx->array_fog_coord};
    for (int u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) arrays[7 + u] = &ctx->array_texcoord[u];
    for (int i = 0; i < 7 + OOPS_GL_MAX_TEXTURE_UNITS; i++) {
        if (!arrays[i]->enabled || arrays[i]->buffer == 0u) continue;
        const gl_buffer_object_t *buf = gl_find_buffer(ctx, arrays[i]->buffer);
        if (buf && buf->mapped) return GL_TRUE;
    }
    if (elements && ctx->bound_element_array_buffer != 0u) {
        const gl_buffer_object_t *buf = gl_find_buffer(ctx, ctx->bound_element_array_buffer);
        if (buf && buf->mapped) return GL_TRUE;
    }
    return GL_FALSE;
}

GLboolean glIsBuffer(GLuint buffer) {
    gl_context_t *ctx = gl_get_ctx();
    return (GLboolean)(gl_find_buffer(ctx, buffer) != NULL);
}

/* -------------------------------------------------------------------------
 * Occlusion queries (GL 1.5)
 *
 * GL_SAMPLES_PASSED between glBeginQuery and glEndQuery: gl_fragment_tail counts every fragment
 * past the depth test while one is active, so the result is exact and known the moment the
 * query ends - always available. The rules are Mesa's (main/queryobj.c): names from
 * glGenQueries, or a new name at glBeginQuery in a compatibility context; a query object only
 * once begun; one active query a target.
 *
 * **On the hardware path the GPU counts them** since 2026-09-20: two `ZPASS_DONE` events bracket
 * the query, each dumping all sixteen render backends' counters, and the result is the sum of
 * the differences (`gl_hw_query_begin`/`gl_hw_query_end`). The count is exact rather than
 * conservative because DB_COUNT_CONTROL gains `PERFECT_ZPASS_COUNTS` while a query runs. It was
 * the CPU's fragments alone before that, with GL_QUERY_COUNTER_BITS 0 to say so.
 *
 * **A query whose draws never test depth still is not counted there**, and keeps the CPU's zero
 * with one log line: the counters need a bound depth surface, and setting ZPASS_ENABLE without
 * one stalls the depth block and never retires the fence.
 * ------------------------------------------------------------------------- */

static gl_query_object_t *gl_find_query(gl_context_t *ctx, GLuint id) {
    if (id == 0u) return NULL;
    for (int i = 0; i < OOPS_GL_MAX_QUERY_OBJECTS; i++) {
        if (ctx->queries[i].used && ctx->queries[i].id == id) return &ctx->queries[i];
    }
    return NULL;
}

/* A slot for name `id`, or NULL when every slot is taken. */
static gl_query_object_t *gl_new_query(gl_context_t *ctx, GLuint id) {
    for (int i = 0; i < OOPS_GL_MAX_QUERY_OBJECTS; i++) {
        gl_query_object_t *q = &ctx->queries[i];
        if (q->used) continue;
        memset(q, 0, sizeof(*q));
        q->used = GL_TRUE;
        q->id = id;
        return q;
    }
    return NULL;
}

void glGenQueries(GLsizei n, GLuint *ids) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ids) return;
    GLuint next = 1u;
    for (GLsizei k = 0; k < n; k++) {
        while (gl_find_query(ctx, next)) next++;
        if (!gl_new_query(ctx, next)) {
            for (GLsizei r = k; r < n; r++) ids[r] = 0u;
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        ids[k] = next++;
    }
}

/* An active query deleted ends first; unknown names and 0 are ignored (Mesa
 * main/queryobj.c:661-692). */
void glDeleteQueries(GLsizei n, const GLuint *ids) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!ids) return;
    for (GLsizei k = 0; k < n; k++) {
        gl_query_object_t *q = gl_find_query(ctx, ids[k]);
        if (!q) continue;
        if (q->active) ctx->query_active = 0u;
        memset(q, 0, sizeof(*q));
    }
}

/* Only a name that has been begun is a query object (Mesa main/queryobj.c:695-710). */
GLboolean glIsQuery(GLuint id) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GL_FALSE;
    const gl_query_object_t *q = gl_find_query(ctx, id);
    return (GLboolean)(q && q->ever_bound);
}

void glBeginQuery(GLenum target, GLuint id) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_BEGIN_QUERY, gl_la_e(target), gl_la_u(id))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* Mesa's order (main/queryobj.c:745-826): the target, an active query on it, the name 0, a
     * new name made an object. */
    if (target != GL_SAMPLES_PASSED) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (ctx->query_active != 0u || id == 0u) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    gl_query_object_t *q = gl_find_query(ctx, id);
    if (!q) q = gl_new_query(ctx, id);
    if (!q) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    q->ever_bound = GL_TRUE;
    q->active = GL_TRUE;
    q->result = 0u;
    ctx->query_active = id;
    ctx->query_samples = 0u;
    /* The counter slots cleared and the draw path armed. It starts counting at the first draw
     * that binds the depth surface, which is where a depth target exists to count against. */
    gl_hw_query_begin(ctx);
}

void glEndQuery(GLenum target) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_END_QUERY, gl_la_e(target))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_SAMPLES_PASSED) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_query_object_t *q = gl_find_query(ctx, ctx->query_active);
    ctx->query_active = 0u;
    if (!q) {
        gl_record_error(ctx, GL_INVALID_OPERATION); /* no matching glBeginQuery */
        return;
    }
    q->active = GL_FALSE;
    q->result = ctx->query_samples;
    /* **The GPU's count replaces the CPU's on the hardware path**, where the CPU draws nothing
     * and `query_samples` is zero anyway. A query none of whose draws tested depth never armed,
     * and keeps the software count with one line saying so - GL_SAMPLES_PASSED with the depth
     * test off is legal and common, and the alternative is setting ZPASS_ENABLE against no depth
     * target, which is what wedged the GPU in `REQ-20260919T1600Z-e3a7`. */
    if (ctx->use_hardware) {
        GLboolean counted = GL_FALSE;
        const uint64_t gpu = gl_hw_query_end(ctx, &counted);
        if (counted) {
            q->result = gpu;
        } else if (!ctx->hw_query_logged) {
            gl_log_line("an occlusion query whose draws never test depth is not counted on this "
                        "path: there is no depth surface for the counters to run against");
            ctx->hw_query_logged = GL_TRUE;
        }
    }
}

void glGetQueryiv(GLenum target, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (target != GL_SAMPLES_PASSED) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
        /* **32 on both paths** since 2026-09-20, when the console started counting: it was 0
         * there, which GL 1.5 allows and which tells a program the count carries no information
         * (4.1.6). 32 rather than the counters' own 63 usable bits because a result leaves here
         * through `glGetQueryObjectiv` and `glGetQueryObjectuiv` and nothing wider, so 32 is what
         * either path can promise. */
        case GL_QUERY_COUNTER_BITS: params[0] = 32; break;
        case GL_CURRENT_QUERY:      params[0] = (GLint)ctx->query_active; break;
        default:                    gl_record_error(ctx, GL_INVALID_ENUM); break;
    }
}

/* A result of a query that has been begun and has ended - GL_INVALID_OPERATION otherwise (Mesa
 * main/queryobj.c:1107-1119) - clamped to the parameter's type as Mesa clamps it. */
static GLboolean gl_query_value(gl_context_t *ctx, GLuint id, GLenum pname, uint64_t *out) {
    const gl_query_object_t *q = gl_find_query(ctx, id);
    if (!q || q->active || !q->ever_bound) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    switch (pname) {
        case GL_QUERY_RESULT:           *out = q->result; return GL_TRUE;
        case GL_QUERY_RESULT_AVAILABLE: *out = GL_TRUE; return GL_TRUE;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return GL_FALSE;
    }
}

void glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    uint64_t v = 0u;
    if (gl_query_value(ctx, id, pname, &v)) params[0] = (v > 0x7fffffffu) ? 0x7fffffff : (GLint)v;
}

void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    uint64_t v = 0u;
    if (gl_query_value(ctx, id, pname, &v)) params[0] = (v > 0xffffffffu) ? 0xffffffffu : (GLuint)v;
}

/* **GL_ARB_occlusion_query's own spellings** (since 2026-09-20, with the extension itself). The
 * extension predates GL 1.5 and a program written against it calls these; gl.h's compatibility
 * section says why the names and the list entry have to arrive together. */
void glGenQueriesARB(GLsizei n, GLuint *ids) { glGenQueries(n, ids); }
void glDeleteQueriesARB(GLsizei n, const GLuint *ids) { glDeleteQueries(n, ids); }
GLboolean glIsQueryARB(GLuint id) { return glIsQuery(id); }
void glBeginQueryARB(GLenum target, GLuint id) { glBeginQuery(target, id); }
void glEndQueryARB(GLenum target) { glEndQuery(target); }
void glGetQueryivARB(GLenum target, GLenum pname, GLint *params) {
    glGetQueryiv(target, pname, params);
}
void glGetQueryObjectivARB(GLuint id, GLenum pname, GLint *params) {
    glGetQueryObjectiv(id, pname, params);
}
void glGetQueryObjectuivARB(GLuint id, GLenum pname, GLuint *params) {
    glGetQueryObjectuiv(id, pname, params);
}

void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    GLuint *binding = gl_buffer_binding(ctx, target);
    if (!binding) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const gl_buffer_object_t *buf = gl_find_buffer(ctx, *binding);
    if (!buf) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    switch (pname) {
        case GL_BUFFER_SIZE:  params[0] = (GLint)buf->size; break;
        case GL_BUFFER_USAGE: params[0] = (GLint)buf->usage; break;
        case GL_BUFFER_ACCESS: params[0] = (GLint)buf->access; break;
        case GL_BUFFER_MAPPED: params[0] = buf->mapped ? GL_TRUE : GL_FALSE; break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

/* -------------------------------------------------------------------------
 * The per-object queries
 *
 * State belonging to a texture, a light or a material rather than to the context. Each answers
 * from the same field the matching setter writes - not from a parallel copy, which is how a
 * query comes to disagree with the state it is querying (see glIsEnabled, which did).
 * ------------------------------------------------------------------------- */

void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (!gl_texture_target_ok(target)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const gl_texture_object_t *tex =
        gl_texture_for_target(ctx, target, GL_FALSE);
    /* No texture object yet is not an error - GL answers with the defaults a fresh object
     * would have, which is what a program setting up one parameter at a time reads back. */
    switch (pname) {
        /* The float parameters as integers, as Mesa converts them (main/texparam.c, the iv
         * getter): a colour and the priority across the signed range by FLOAT_TO_INT, and
         * residency as the boolean it is - every texture here is resident. */
        case GL_TEXTURE_BORDER_COLOR:
            for (int i = 0; i < 4; i++) {
                params[i] = gl_float_to_int_color(tex ? tex->border_color[i] : 0.0f);
            }
            break;
        case GL_TEXTURE_PRIORITY:
            params[0] = gl_float_to_int_color(tex ? tex->priority : 1.0f);
            break;
        case GL_TEXTURE_RESIDENT:
            params[0] = GL_TRUE;
            break;
        case GL_TEXTURE_BASE_LEVEL:
            params[0] = tex ? tex->base_level : 0;
            break;
        case GL_TEXTURE_MAX_LEVEL:
            params[0] = tex ? tex->max_level : 1000;
            break;
        /* Rounded to the nearest, as Mesa's LCLAMPF does (main/texparam.c:2663-2676). */
        case GL_TEXTURE_MIN_LOD:
            params[0] = gl_round_to_int(tex ? tex->min_lod : -1000.0f);
            break;
        case GL_TEXTURE_MAX_LOD:
            params[0] = gl_round_to_int(tex ? tex->max_lod : 1000.0f);
            break;
        case GL_TEXTURE_LOD_BIAS:
            params[0] = gl_round_to_int(tex ? tex->lod_bias : 0.0f);
            break;
        case GL_GENERATE_MIPMAP:
            params[0] = (tex && tex->generate_mipmap) ? GL_TRUE : GL_FALSE;
            break;
        case GL_TEXTURE_COMPARE_MODE:
            params[0] = (GLint)(tex ? tex->compare_mode : (GLenum)GL_NONE);
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            params[0] = (GLint)(tex ? tex->compare_func : (GLenum)GL_LEQUAL);
            break;
        case GL_DEPTH_TEXTURE_MODE:
            params[0] = (GLint)(tex ? tex->depth_mode : (GLenum)GL_LUMINANCE);
            break;
        case GL_TEXTURE_WRAP_S:
            params[0] = (GLint)(tex ? tex->wrap_s : (GLenum)GL_REPEAT);
            break;
        case GL_TEXTURE_WRAP_T:
            params[0] = (GLint)(tex ? tex->wrap_t : (GLenum)GL_REPEAT);
            break;
        case GL_TEXTURE_WRAP_R:
            params[0] = (GLint)(tex ? tex->wrap_r : (GLenum)GL_REPEAT);
            break;
        /* The defaults have to be the ones gl_find_or_create_texture actually assigns, or the
         * query answers differently before and after the first glTexImage2D. min_filter is
         * GL_NEAREST_MIPMAP_LINEAR, which is the specification's default and not GL_LINEAR. */
        case GL_TEXTURE_MIN_FILTER:
            params[0] = (GLint)(tex ? tex->min_filter : (GLenum)GL_NEAREST_MIPMAP_LINEAR);
            break;
        case GL_TEXTURE_MAG_FILTER:
            params[0] = (GLint)(tex ? tex->mag_filter : (GLenum)GL_LINEAR);
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    /* The float parameters as themselves; everything else is an enum, through the integer
     * query. */
    if (gl_texture_target_ok(target) &&
        (pname == GL_TEXTURE_BORDER_COLOR || pname == GL_TEXTURE_PRIORITY ||
         pname == GL_TEXTURE_MIN_LOD || pname == GL_TEXTURE_MAX_LOD ||
         pname == GL_TEXTURE_LOD_BIAS)) {
        const gl_texture_object_t *tex = gl_texture_for_target(ctx, target, GL_FALSE);
        if (pname == GL_TEXTURE_PRIORITY) {
            params[0] = tex ? tex->priority : 1.0f;
        } else if (pname == GL_TEXTURE_LOD_BIAS) {
            params[0] = tex ? tex->lod_bias : 0.0f;
        } else if (pname == GL_TEXTURE_MIN_LOD) {
            params[0] = tex ? tex->min_lod : -1000.0f;
        } else if (pname == GL_TEXTURE_MAX_LOD) {
            params[0] = tex ? tex->max_lod : 1000.0f;
        } else {
            for (int i = 0; i < 4; i++) params[i] = tex ? tex->border_color[i] : 0.0f;
        }
        return;
    }
    GLint iv = 0;
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    glGetTexParameteriv(target, pname, &iv);
    if (ctx->last_error != GL_NO_ERROR) {
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    params[0] = (GLfloat)iv;
}

/* The per-level queries: what the image actually is, as opposed to how it is sampled. A program
 * asking these has usually just uploaded and wants to know what it got. */
void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (!gl_tex_image_target_ok(target) && !gl_is_proxy_target(target)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (level < 0 || level >= OOPS_GL_MAX_TEXTURE_LEVELS) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* **The level asked about.** `level` was ignored, so every level answered with the base
     * image's size. A level never specified answers zero, as the specification says. A proxy's
     * level is what its last glTexImage asked for, or zeros if that would not have fitted. */
    gl_tex_view_t lv;
    GLboolean has;
    const gl_tex_level_t *proxy = gl_proxy_level(ctx, target, level);
    if (proxy) {
        memset(&lv, 0, sizeof(lv));
        lv.width = proxy->width;
        lv.height = proxy->height;
        lv.depth = proxy->depth;
        lv.internal_format = proxy->internal_format;
        lv.base_format = proxy->base_format;
        has = (GLboolean)(proxy->width > 0);
    } else {
        has = gl_tex_target_view(gl_texture_for_target(ctx, target, GL_FALSE), target, level, &lv);
    }
    /* Which components the base format keeps (Mesa, _mesa_base_format_has_channel in
     * main/glformats.c); each is stored in eight bits. */
    const GLenum b = has ? lv.base_format : 0u;
    GLboolean channel = GL_FALSE;
    switch (pname) {
        case GL_TEXTURE_RED_SIZE:
        case GL_TEXTURE_GREEN_SIZE:
        case GL_TEXTURE_BLUE_SIZE:
            channel = (GLboolean)(b == GL_RGB || b == GL_RGBA);
            break;
        case GL_TEXTURE_ALPHA_SIZE:
            channel = (GLboolean)(b == GL_RGBA || b == GL_ALPHA || b == GL_LUMINANCE_ALPHA);
            break;
        case GL_TEXTURE_LUMINANCE_SIZE:
            channel = (GLboolean)(b == GL_LUMINANCE || b == GL_LUMINANCE_ALPHA);
            break;
        case GL_TEXTURE_INTENSITY_SIZE:
            channel = (GLboolean)(b == GL_INTENSITY);
            break;
        default:
            break;
    }
    switch (pname) {
        case GL_TEXTURE_RED_SIZE:
        case GL_TEXTURE_GREEN_SIZE:
        case GL_TEXTURE_BLUE_SIZE:
        case GL_TEXTURE_ALPHA_SIZE:
        case GL_TEXTURE_LUMINANCE_SIZE:
        case GL_TEXTURE_INTENSITY_SIZE:
            params[0] = channel ? 8 : 0;
            break;
        /* GL 1.4: a depth texture's depth is a 32-bit float, whichever size was named. */
        case GL_TEXTURE_DEPTH_SIZE:
            params[0] = (b == GL_DEPTH_COMPONENT) ? 32 : 0;
            break;
        case GL_TEXTURE_WIDTH:
            params[0] = has ? lv.width : 0;
            break;
        case GL_TEXTURE_HEIGHT:
            params[0] = has ? lv.height : 0;
            break;
        case GL_TEXTURE_DEPTH:
            params[0] = has ? lv.depth : 0;
            break;
        /* No texture here is compressed, which is a fact about all of them rather than a
         * refusal - so this answers rather than erroring, and the size query answers zero. */
        case GL_TEXTURE_COMPRESSED:
            params[0] = GL_FALSE;
            break;
        case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
            params[0] = 0;
            break;
        case GL_TEXTURE_INTERNAL_FORMAT:
            /* **What was asked for**, as Mesa answers (main/texparam.c:1807) - but a generic
             * compressed format, which nothing here compresses, as its base format, which is
             * what GL 1.3 says it is replaced by. This answered GL_RGBA for everything while
             * the internal format was ignored; a level never specified still does. */
            if (!has) {
                params[0] = (GLint)GL_RGBA;
            } else if (lv.internal_format >= (GLint)GL_COMPRESSED_ALPHA &&
                       lv.internal_format <= (GLint)GL_COMPRESSED_RGBA) {
                params[0] = (GLint)lv.base_format;
            } else {
                params[0] = lv.internal_format;
            }
            break;
        case GL_TEXTURE_BORDER:
            params[0] = 0; /* bordered textures are refused at upload */
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    GLint iv = 0;
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    glGetTexLevelParameteriv(target, level, pname, &iv);
    if (ctx->last_error != GL_NO_ERROR) {
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    params[0] = (GLfloat)iv;
}

/* How many floats a light or material query writes. Same reasoning as gl_query_element_count:
 * the caller sized its buffer from the pname, so four into a GLfloat[1] is an overrun. */
static int gl_light_element_count(GLenum pname) {
    switch (pname) {
        case GL_AMBIENT:
        case GL_DIFFUSE:
        case GL_SPECULAR:
        case GL_POSITION:
            return 4;
        case GL_SPOT_DIRECTION:
            return 3;
        default:
            return 1;
    }
}

void glGetLightfv(GLenum light, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + OOPS_GL_LIGHT_COUNT) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const gl_light_t *l = &ctx->lights[(size_t)(light - GL_LIGHT0)];
    switch (pname) {
        /* **Eye coordinates, which is what GL says and what is stored.** glLightfv transformed
         * the position by the modelview matrix on the way in, so returning the object-space
         * value the caller passed would need a copy of it kept and an inverse applied - and
         * would answer a different question from the one the specification asks. */
        case GL_POSITION:
            for (int i = 0; i < 4; i++) params[i] = l->position[i];
            break;
        case GL_AMBIENT:
            for (int i = 0; i < 4; i++) params[i] = l->ambient[i];
            break;
        case GL_DIFFUSE:
            for (int i = 0; i < 4; i++) params[i] = l->diffuse[i];
            break;
        case GL_SPECULAR:
            for (int i = 0; i < 4; i++) params[i] = l->specular[i];
            break;
        case GL_SPOT_DIRECTION:
            for (int i = 0; i < 3; i++) params[i] = l->spot_direction[i];
            break;
        case GL_SPOT_EXPONENT:         params[0] = l->spot_exponent; break;
        case GL_SPOT_CUTOFF:           params[0] = l->spot_cutoff; break;
        case GL_CONSTANT_ATTENUATION:  params[0] = l->const_att; break;
        case GL_LINEAR_ATTENUATION:    params[0] = l->linear_att; break;
        case GL_QUADRATIC_ATTENUATION: params[0] = l->quad_att; break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glGetLightiv(GLenum light, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    GLfloat fv[4] = {0};
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    glGetLightfv(light, pname, fv);
    if (ctx->last_error != GL_NO_ERROR) {
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    const int n = gl_light_element_count(pname);
    for (int i = 0; i < n; i++) params[i] = (GLint)fv[i];
}

void glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    /* GL_FRONT_AND_BACK is refused here although glMaterialfv accepts it: setting both at once
     * is meaningful, reading "both" is not - there is no single answer to return. */
    const gl_material_t *m;
    if (face == GL_FRONT) {
        m = &ctx->mat_front;
    } else if (face == GL_BACK) {
        m = &ctx->mat_back;
    } else {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
        case GL_AMBIENT:
            for (int i = 0; i < 4; i++) params[i] = m->ambient[i];
            break;
        case GL_DIFFUSE:
            for (int i = 0; i < 4; i++) params[i] = m->diffuse[i];
            break;
        case GL_SPECULAR:
            for (int i = 0; i < 4; i++) params[i] = m->specular[i];
            break;
        case GL_EMISSION:
            for (int i = 0; i < 4; i++) params[i] = m->emission[i];
            break;
        case GL_SHININESS:
            params[0] = m->shininess;
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glGetMaterialiv(GLenum face, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    GLfloat fv[4] = {0};
    GLenum before = ctx->last_error;
    ctx->last_error = GL_NO_ERROR;
    glGetMaterialfv(face, pname, fv);
    if (ctx->last_error != GL_NO_ERROR) {
        if (before != GL_NO_ERROR) ctx->last_error = before;
        return;
    }
    ctx->last_error = before;
    const int n = (pname == GL_SHININESS) ? 1 : 4;
    for (int i = 0; i < n; i++) params[i] = (GLint)fv[i];
}

/* The array pointers, read back. `glInterleavedArrays` is the reason this matters: a program
 * that let it compute the offsets may want to know what they came out as. */
void glGetPointerv(GLenum pname, GLvoid **params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    switch (pname) {
        case GL_VERTEX_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_vertex.pointer;
            break;
        case GL_COLOR_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_color.pointer;
            break;
        case GL_NORMAL_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_normal.pointer;
            break;
        case GL_TEXTURE_COORD_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_texcoord[ctx->client_active_texture].pointer;
            break;
        case GL_EDGE_FLAG_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_edge_flag.pointer;
            break;
        /* Answered since 2026-09-19; the index array had a pointer and no way to read it back. */
        case GL_INDEX_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_index.pointer;
            break;
        case GL_SECONDARY_COLOR_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_secondary.pointer;
            break;
        case GL_FOG_COORD_ARRAY_POINTER:
            params[0] = (GLvoid *)(uintptr_t)ctx->array_fog_coord.pointer;
            break;
        case GL_SELECTION_BUFFER_POINTER:
            params[0] = (GLvoid *)ctx->select_buffer;
            break;
        case GL_FEEDBACK_BUFFER_POINTER:
            params[0] = (GLvoid *)ctx->feedback_buffer;
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

/* `glGetTexImage(...)` - the bound texture, read back into client memory.
 *
 * **Not a flip.** glReadPixels turns the framebuffer over because GL's window origin is the
 * bottom-left corner; a texture has no window and row 0 of a texture image is row 0 of what was
 * uploaded, so this walks straight through. Sharing glReadPixels' loop here would have been the
 * tidy-looking mistake.
 *
 * The stored image is RGBA8 at `tex->pitch`, so this converts out through the same
 * `gl_pack_pixel` glReadPixels uses and honours GL_PACK_ALIGNMENT the same way.
 */
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (!gl_tex_image_target_ok(target)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_pixel_fmt_t f;
    const GLenum fmt_err = gl_pixel_fmt(format, type, &f);
    if (fmt_err != GL_NO_ERROR) {
        gl_record_error(ctx, fmt_err);
        return;
    }
    /* Colour indices are no format a texture reads back as. */
    if (f.kind == GL_COLOR_INDEX) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (level < 0 || level >= OOPS_GL_MAX_TEXTURE_LEVELS) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!pixels) return;

    /* The level asked for - `level` was ignored and every level read back the base image. */
    const gl_texture_object_t *tex =
        gl_texture_for_target(ctx, target, GL_FALSE);
    gl_tex_view_t lv;
    if (!gl_tex_target_view(tex, target, level, &lv)) {
        /* No image under the query. The same error glTexSubImage2D raises for the same reason:
         * the caller is asking about something that was never uploaded. */
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* A depth texture reads back as GL_DEPTH_COMPONENT and only so, a colour one never so (Mesa
     * main/texgetimage.c's format checks) - GL_INVALID_OPERATION either way. */
    if ((lv.base_format == GL_DEPTH_COMPONENT) != (f.kind == GL_DEPTH) || f.kind == GL_STENCIL) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }

    /* A volume comes back slice after slice, GL_PACK_IMAGE_HEIGHT rows apart when that is set -
     * and every pack parameter applies, as for glReadPixels. Luminance is the red channel, the
     * texture's own luminance, not a sum - and a luminance or intensity texture reports it in red
     * alone, so asking for GL_RGB of one gives (L, 0, 0). */
    gl_pixel_dst_t d;
    gl_pack_dest(ctx, &f, pixels, lv.width, lv.height, &d);
    const uint8_t *src = lv.pixels;

    for (GLsizei z = 0; z < lv.depth; z++) {
        for (GLsizei row = 0; row < lv.height; row++) {
            const uint8_t *in = src + ((size_t)z * lv.slice + (size_t)row * lv.pitch) * 4u;
            uint8_t *out = d.base + (size_t)z * d.image_stride + (size_t)row * d.row_stride;
            /* Only the row's own pixels. This zeroed `stride * height` first, which wrote the
             * last row's alignment padding past the end of a buffer sized as GL sizes it - the
             * same overrun glReadPixels had. */
            for (GLsizei col = 0; col < lv.width; col++) {
                if (f.kind == GL_DEPTH) {
                    float dv;
                    memcpy(&dv, in + (size_t)col * 4u, 4u);
                    gl_pack_value(ctx, &f, dv, d.swap, out + (size_t)col * f.pixel_bytes);
                    continue;
                }
                uint8_t px[4];
                memcpy(px, in + (size_t)col * 4u, 4u);
                gl_tex_readback_row(px, 1, lv.base_format);
                const float c[4] = {(float)px[0] / 255.0f, (float)px[1] / 255.0f,
                                    (float)px[2] / 255.0f, (float)px[3] / 255.0f};
                gl_pack_pixel_f(&f, c, GL_FALSE, d.swap, out + (size_t)col * f.pixel_bytes);
            }
        }
    }
}

/* `glCopyTexSubImage2D(...)` - the framebuffer into a bound texture. Render-to-texture, the
 * 1.x way.
 *
 * Reads through `glReadPixels`, which means it inherits the y flip: the window rectangle is
 * read in GL's bottom-left orientation and lands in the texture the same way up as a
 * `glTexSubImage2D` of the same pixels would. Sharing that path rather than copying it is the
 * point - a second flip written by hand is a second chance to get the flip wrong.
 */
/* The framebuffer-to-texture copy, target already validated - shared with glCopyTexSubImage1D. */
static void gl_copy_tex_sub_common(gl_context_t *ctx, GLenum target, GLint level,
                                   GLint xoffset, GLint yoffset, GLint zoffset,
                                   GLint x, GLint y, GLsizei width, GLsizei height) {
    if (width < 0 || height < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (width == 0 || height == 0) return;

    /* **A bounded scratch rather than an allocation.** This is called per frame by anything
     * doing render-to-texture, and a freestanding binary has no business allocating on that
     * path; a rectangle wider than the scratch is refused rather than silently truncated. */
    enum { COPY_MAX_WIDTH = 2048 };
    static uint8_t row[COPY_MAX_WIDTH * 4];
    if (width > COPY_MAX_WIDTH) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    /* One row at a time, so the scratch is a row rather than a rectangle. Row `r` of the
     * destination is row `r` of the source read in GL orientation, which is what keeps this
     * identical to a glTexSubImage2D of the same pixels. */
    /* The scratch is tight and native, so neither the program's pack state (reading into it)
     * nor its unpack state (uploading from it) may apply: no padding, row length, skips or byte
     * swapping on either side. Only the alignment was set aside before the other parameters
     * existed. */
    const GLint p_align = ctx->pack_alignment, p_row = ctx->pack_row_length;
    const GLint p_rows = ctx->pack_skip_rows, p_px = ctx->pack_skip_pixels;
    const GLint p_img = ctx->pack_skip_images, p_ih = ctx->pack_image_height;
    const GLboolean p_swap = ctx->pack_swap_bytes;
    ctx->pack_alignment = 1;
    ctx->pack_row_length = 0;
    ctx->pack_skip_rows = 0;
    ctx->pack_skip_pixels = 0;
    ctx->pack_skip_images = 0;
    ctx->pack_image_height = 0;
    ctx->pack_swap_bytes = GL_FALSE;
    gl_unpack_saved_t saved_unpack;
    /* **Into a depth texture the copy reads the depth buffer** (GL 1.4), as floats, through
     * glReadPixels like any other copy. */
    const gl_texture_object_t *dtex = gl_texture_for_target(ctx, target, GL_FALSE);
    gl_tex_view_t dlv;
    const GLboolean depth = (GLboolean)(gl_tex_target_view(dtex, target, level, &dlv) &&
                                        dlv.base_format == GL_DEPTH_COMPONENT);
    const GLenum rd_format = depth ? (GLenum)GL_DEPTH_COMPONENT : (GLenum)GL_RGBA;
    const GLenum rd_type = depth ? (GLenum)GL_FLOAT : (GLenum)GL_UNSIGNED_BYTE;
    ctx->gen_mipmap_suspend++; /* GL_GENERATE_MIPMAP once, after the last row */
    for (GLsizei r = 0; r < height; r++) {
        glReadPixels(x, y + r, width, 1, rd_format, rd_type, row);
        /* The shared helper, not glTexSubImage2D: `target` may be GL_TEXTURE_1D here and the
         * public 2D entry point refuses that - correctly, which is why it cannot be used.
         *
         * **glReadPixels has already applied the pixel transfer**, which a copy undergoes once;
         * the upload would apply it a second time, so it is suspended for the upload. */
        ctx->pixel_transfer_suspend = GL_TRUE;
        gl_unpack_neutral(ctx, &saved_unpack);
        gl_tex_sub_image_common(ctx, target, level, xoffset, yoffset + r, zoffset, width, 1, 1,
                                rd_format, rd_type, row);
        gl_unpack_restore(ctx, &saved_unpack);
        ctx->pixel_transfer_suspend = GL_FALSE;
    }
    ctx->gen_mipmap_suspend--;
    ctx->pack_alignment = p_align;
    ctx->pack_row_length = p_row;
    ctx->pack_skip_rows = p_rows;
    ctx->pack_skip_pixels = p_px;
    ctx->pack_skip_images = p_img;
    ctx->pack_image_height = p_ih;
    ctx->pack_swap_bytes = p_swap;
    gl_tex_gen_mipmap_check(ctx, target, level);
}

void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLint x, GLint y, GLsizei width, GLsizei height) {
    /* A copy reads the framebuffer when it *runs*, so there is nothing to capture but the call. */
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_COPY_TEX_SUB_IMAGE_2D, gl_la_e(target), gl_la_i(level),
                    gl_la_i(xoffset), gl_la_i(yoffset), gl_la_i(x), gl_la_i(y), gl_la_i(width),
                    gl_la_i(height))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D && GL_CUBE_FACE_INDEX(target) < 0) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_copy_tex_sub_common(ctx, target, level, xoffset, yoffset, 0, x, y, width, height);
}

/* `glCopyTexImage2D(...)` - the allocating half of the copy pair.
 *
 * Sizes the texture from the window rectangle and then copies into it through
 * glCopyTexSubImage2D, so there is exactly one piece of code that decides which way up a
 * framebuffer copy lands. Writing the flip a second time here is the failure this avoids: it
 * would work for a square rectangle and be upside down for everything else.
 *
 * `border` must be zero. The bordered texture is a GL 1.0 feature no implementation has
 * supported for decades, and accepting a non-zero border would mean claiming a texture one
 * pixel larger in each direction than the one actually allocated.
 */
void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                      GLint x, GLint y, GLsizei width, GLsizei height, GLint border) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_COPY_TEX_IMAGE_2D, gl_la_e(target), gl_la_i(level),
                    gl_la_e(internalformat), gl_la_i(x), gl_la_i(y), gl_la_i(width),
                    gl_la_i(height), gl_la_i(border))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if ((target != GL_TEXTURE_2D && GL_CUBE_FACE_INDEX(target) < 0) ||
        !gl_copy_internal_format_ok(internalformat)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (border != 0 || width <= 0 || height <= 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    /* Allocated with no pixels: every texel is overwritten by the copy below, so uploading
     * anything here would be work thrown away. A depth format is allocated as depth, which is
     * what the copy then reads the depth buffer into. */
    const GLboolean depth = (GLboolean)(gl_tex_base_format((GLint)internalformat) == GL_DEPTH_COMPONENT);
    glTexImage2D(target, level, (GLint)internalformat, width, height, 0,
                 depth ? (GLenum)GL_DEPTH_COMPONENT : (GLenum)GL_RGBA,
                 depth ? (GLenum)GL_FLOAT : (GLenum)GL_UNSIGNED_BYTE, NULL);

    /* **Checked against the texture, not against glGetError.** GL errors are sticky, so an
     * error left over from some earlier call would abort a copy that had every right to run.
     * What matters is whether the allocation above actually produced an image of this size. */
    gl_texture_object_t *tex =
        gl_texture_for_target(ctx, target, GL_FALSE);
    gl_tex_view_t lv;
    /* The level just specified, not the base one - this compared against the base level's size
     * until 2026-09-19, so a copy into any mip level found a mismatch and silently copied
     * nothing. */
    if (!gl_tex_target_view(tex, target, level, &lv) || lv.width != width || lv.height != height) {
        return;
    }

    glCopyTexSubImage2D(target, level, 0, 0, x, y, width, height);
}

/* The sub-image upload, target already validated - shared with glTexSubImage1D, which passes
 * yoffset 0 and height 1. */
static void gl_tex_sub_image_common(gl_context_t *ctx, GLenum target, GLint level,
                                    GLint xoffset, GLint yoffset, GLint zoffset,
                                    GLsizei width, GLsizei height, GLsizei depth,
                                    GLenum format, GLenum type, const GLvoid *pixels) {
    gl_pixel_fmt_t f;
    const GLenum fmt_err = gl_pixel_fmt(format, type, &f);
    if (fmt_err != GL_NO_ERROR) {
        gl_record_error(ctx, fmt_err);
        return;
    }
    if (width < 0 || height < 0 || depth < 0 || xoffset < 0 || yoffset < 0 || zoffset < 0 ||
        level < 0 || level >= OOPS_GL_MAX_TEXTURE_LEVELS) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    gl_pixel_src_t s;
    gl_unpack_source(ctx, &f, pixels, width, height, &s);

    /* A mip level, or any level of a cube map's face: its own tight rows, with the same bounds
     * rule as the base level below. */
    const int face = GL_CUBE_FACE_INDEX(target);
    if (level > 0 || face >= 0) {
        gl_texture_object_t *mt = gl_texture_for_target(ctx, target, GL_FALSE);
        gl_tex_level_t *lv = (gl_tex_level_t *)0;
        if (mt && face >= 0) {
            lv = mt->cube ? &mt->cube[face * OOPS_GL_MAX_TEXTURE_LEVELS + level]
                          : (gl_tex_level_t *)0;
        } else if (mt) {
            lv = &mt->mips[level];
        }
        if (!lv || !lv->pixels) {
            gl_record_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        /* Depth data for a depth level and colour for a colour one, as the full upload checks. */
        if ((lv->base_format == GL_DEPTH_COMPONENT) != (f.kind == GL_DEPTH) || f.kind == GL_STENCIL) {
            gl_record_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        const GLsizei ld = lv->depth > 0 ? lv->depth : 1;
        if ((size_t)xoffset + (size_t)width > (size_t)lv->width ||
            (size_t)yoffset + (size_t)height > (size_t)lv->height ||
            (size_t)zoffset + (size_t)depth > (size_t)ld) {
            gl_record_error(ctx, GL_INVALID_VALUE);
            return;
        }
        if (width == 0 || height == 0 || depth == 0 || !pixels) return;
        uint8_t *dst = (uint8_t *)lv->pixels;
        const size_t lslice = (size_t)lv->width * (size_t)lv->height;
        for (size_t z = 0; z < (size_t)depth; z++) {
            for (size_t y = 0; y < (size_t)height; y++) {
                uint8_t *row = dst + (((size_t)zoffset + z) * lslice +
                                      ((size_t)yoffset + y) * (size_t)lv->width + (size_t)xoffset) * 4u;
                gl_tex_store_row(ctx, &f, row, s.base + z * s.image_stride + y * s.row_stride,
                                 width, s.swap, lv->base_format);
            }
        }
        mt->chain_dirty = GL_TRUE;
        gl_tex_gen_mipmap_check(ctx, target, level);
        return;
    }

    gl_texture_object_t *tex = gl_texture_for_target(ctx, target, GL_FALSE);
    if (!tex || !tex->pixels) {
        /* Nothing to update. The specification's error for a sub-image with no image under it. */
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if ((tex->base_format == GL_DEPTH_COMPONENT) != (f.kind == GL_DEPTH) || f.kind == GL_STENCIL) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* **Bounds are an error, not a clamp.** A sub-image that ran off the edge would otherwise
     * corrupt whatever followed the texture, or silently draw a different rectangle from the
     * one the caller asked for. */
    const GLsizei td = tex->depth > 0 ? tex->depth : 1;
    if ((size_t)xoffset + (size_t)width > (size_t)tex->width ||
        (size_t)yoffset + (size_t)height > (size_t)tex->height ||
        (size_t)zoffset + (size_t)depth > (size_t)td) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (width == 0 || height == 0 || depth == 0 || !pixels) return;

    uint8_t *dst = (uint8_t *)tex->pixels;
    const size_t tslice = (size_t)tex->pitch * (size_t)tex->height;
    for (size_t z = 0; z < (size_t)depth; z++) {
        for (size_t y = 0; y < (size_t)height; y++) {
            uint8_t *row = dst + (((size_t)zoffset + z) * tslice +
                                  ((size_t)yoffset + y) * (size_t)tex->pitch + (size_t)xoffset) * 4u;
            gl_tex_store_row(ctx, &f, row, s.base + z * s.image_stride + y * s.row_stride, width,
                             s.swap, tex->base_format);
        }
    }
    tex->chain_dirty = GL_TRUE; /* the chain holds its own copy of the base level */
#if !defined(OOPS_HOST_BUILD) && defined(__x86_64__)
    if (tex->garlic_data) {
        size_t bytes = (size_t)tex->pitch * (size_t)tex->height * (size_t)td * 4u;
        for (size_t p = 0; p < bytes; p += 64) {
            __builtin_ia32_clflush((const void *)((const char *)tex->garlic_data + p));
        }
    }
#endif
    gl_pack_descriptors(tex);
    gl_tex_gen_mipmap_check(ctx, target, level);
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format, GLenum type,
                     const GLvoid *pixels) {
    if (gl_list_recording() &&
        gl_list_rec_image(GL_LIST_OP_TEX_SUB_IMAGE_2D,
                          GL_LIST_ARGV(gl_la_e(target), gl_la_i(level), gl_la_i(xoffset),
                                       gl_la_i(yoffset), gl_la_i(width), gl_la_i(height),
                                       gl_la_e(format), gl_la_e(type)),
                          8, width, height, format, type, pixels)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D && GL_CUBE_FACE_INDEX(target) < 0) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_tex_sub_image_common(ctx, target, level, xoffset, yoffset, 0, width, height, 1,
                            format, type, pixels);
}

/* The upload, with the target already validated by the caller.
 *
 * Everything below this point is the same for a 1D and a 2D image - a 1D one is stored as a
 * height-1 2D one, which is what the sampler reads and what the descriptor describes. The only
 * thing the two do not share is *which binding* the texture comes from, and that arrives as
 * `target`. */
/* A copy's internal format: any glTexImage takes, but for the legacy 1 to 4, and a refusal here is
 * an enum error rather than glTexImage's value error (Mesa, main/teximage.c:2462-2471). */
static GLboolean gl_copy_internal_format_ok(GLenum internalformat) {
    if (internalformat >= 1u && internalformat <= 4u) return GL_FALSE;
    return (GLboolean)(gl_tex_base_format((GLint)internalformat) != 0u);
}

/* The proxy level a proxy target names, or NULL for any other target. */
static gl_tex_level_t *gl_proxy_level(gl_context_t *ctx, GLenum target, GLint level) {
    int i;
    switch (target) {
        case GL_PROXY_TEXTURE_1D: i = 0; break;
        case GL_PROXY_TEXTURE_2D: i = 1; break;
        case GL_PROXY_TEXTURE_3D: i = 2; break;
        case GL_PROXY_TEXTURE_CUBE_MAP: i = 3; break;
        default: return (gl_tex_level_t *)0;
    }
    if (level < 0 || level >= OOPS_GL_MAX_TEXTURE_LEVELS) return (gl_tex_level_t *)0;
    return &ctx->proxy[i][level];
}

static GLboolean gl_is_proxy_target(GLenum t) {
    return (GLboolean)(t == GL_PROXY_TEXTURE_1D || t == GL_PROXY_TEXTURE_2D ||
                       t == GL_PROXY_TEXTURE_3D || t == GL_PROXY_TEXTURE_CUBE_MAP);
}

static void gl_tex_image_common(gl_context_t *ctx, GLenum target, GLint level,
                                GLint internalformat, GLsizei width, GLsizei height,
                                GLsizei depth, GLenum format, GLenum type, const GLvoid *pixels) {
    /* In Mesa's order (main/teximage.c, texture_error_check): the level, a negative size, the
     * format and type, the internal format - each an error for a proxy too. */
    if (level < 0 || level >= OOPS_GL_MAX_TEXTURE_LEVELS || width < 0 || height < 0 || depth < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* **Checked before anything is allocated.** This used to convert only GL_RGBA and GL_RGB and
     * fall through silently for everything else, leaving the texture holding whatever the
     * allocation contained - uninitialised memory, sampled, with no error raised. */
    gl_pixel_fmt_t f;
    const GLenum fmt_err = gl_pixel_fmt(format, type, &f);
    if (fmt_err != GL_NO_ERROR) {
        gl_record_error(ctx, fmt_err);
        return;
    }
    /* **The internal format was ignored until 2026-09-19** - every texture was RGBA whatever it
     * was asked to be, so a GL_ALPHA texture drawn GL_MODULATE painted its black RGB over the
     * fragment's colour, and GL_INTENSITY did not exist. One GL 1.x does not define is a value
     * error (Mesa, main/teximage.c:1953). */
    const GLenum base = gl_tex_base_format(internalformat);
    if (base == 0u) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* **Depth textures** (GL 1.4): 1D and 2D only in GL 1.x - cube maps are GL 3.0's (Mesa
     * main/teximage.c:1744-1790, :2021-2025) - and depth data for a depth format, colour data for
     * a colour one, never a stencil index (texture_formats_agree, :1795-1832). Each is
     * GL_INVALID_OPERATION. */
    if (base == GL_DEPTH_COMPONENT &&
        !(target == GL_TEXTURE_1D || target == GL_TEXTURE_2D || target == GL_PROXY_TEXTURE_1D ||
          target == GL_PROXY_TEXTURE_2D)) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if ((base == GL_DEPTH_COMPONENT) != (f.kind == GL_DEPTH) || f.kind == GL_STENCIL) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* An image larger than its level can be: the specification's limit is the maximum size halved
     * once per level - GL_MAX_3D_TEXTURE_SIZE on all three sides of a volume, and
     * GL_MAX_CUBE_MAP_TEXTURE_SIZE for a cube map's face, which must also be square (Mesa,
     * main/teximage.c:1079-1100). */
    const GLboolean is3d = (GLboolean)(target == GL_TEXTURE_3D || target == GL_PROXY_TEXTURE_3D);
    const int face = GL_CUBE_FACE_INDEX(target);
    const GLboolean cube = (GLboolean)(face >= 0 || target == GL_PROXY_TEXTURE_CUBE_MAP);
    const GLsizei max = is3d ? OOPS_GL_MAX_3D_TEXTURE_SIZE
                             : (cube ? OOPS_GL_MAX_CUBE_MAP_TEXTURE_SIZE : OOPS_GL_MAX_TEXTURE_SIZE);
    /* **A zero in any dimension is a legal size**, and means no image at all - which is how a
     * program releases a level it no longer wants. It was refused with GL_INVALID_VALUE here
     * until 2026-09-20, which was this library's one behavioural difference from the
     * specification; the release is handled below, after the texture is found. */
    const GLboolean empty = (GLboolean)(width == 0 || height == 0 || depth == 0);
    const GLboolean fits = (GLboolean)(empty ||
                                       (width <= (max >> level) && height <= (max >> level) &&
                                        depth <= (max >> level) && (is3d || depth == 1) &&
                                        (!cube || width == height)));

    /* **A proxy allocates nothing and raises no size error**: it records what it was asked and
     * whether that would have worked - a level all zeros when it would not - for
     * glGetTexLevelParameter to report. Errors in the enums are still errors. */
    gl_tex_level_t *proxy = gl_proxy_level(ctx, target, level);
    if (proxy) {
        const gl_tex_level_t none = {0, 0, 0, (void *)0, 0, 0u};
        *proxy = none;
        if (fits) {
            proxy->width = width;
            proxy->height = height;
            proxy->depth = depth;
            proxy->internal_format = internalformat;
            proxy->base_format = base;
        }
        return;
    }
    if (!fits) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    gl_texture_object_t *tex = gl_texture_for_target(ctx, target, GL_TRUE);
    if (!tex) return;

    /*
     * **A zero-sized image releases the level and allocates nothing.**
     *
     * `gl_tex_level_view` already reports a level with no pixels or no extent as absent, so
     * releasing one is the whole of what a zero size has to do: the texture becomes incomplete
     * if that level was needed, which is a draw with no texture, and a mip chain built from it
     * is rebuilt without it.
     *
     * The base level frees its GPU-visible storage rather than its process memory, because that
     * is where a level-0 image lives; a built frame may still name the old address, so the frame
     * is submitted first, as every other release of it does.
     */
    if (empty) {
        if (face >= 0) {
            if (tex->cube) {
                gl_tex_level_t *lv = &tex->cube[face * OOPS_GL_MAX_TEXTURE_LEVELS + level];
                gl_buffer_release(lv->pixels);
                memset(lv, 0, sizeof(*lv));
                tex->cube_hw_dirty = GL_TRUE;
            }
        } else if (level > 0) {
            gl_tex_level_t *lv = &tex->mips[level];
            gl_buffer_release(lv->pixels);
            memset(lv, 0, sizeof(*lv));
        } else {
            gl_tex_storage_release_sync(ctx);
#ifndef OOPS_HOST_BUILD
            if (tex->garlic_data) {
                oops_mem_free(tex->garlic_data);
                tex->garlic_data = NULL;
            }
            tex->garlic_va = 0u;
#else
            free(tex->pixels);
#endif
            tex->pixels = NULL;
            tex->width = 0;
            tex->height = 0;
            tex->depth = 0;
            tex->pitch = 0;
            tex->internal_format = internalformat;
            tex->base_format = base;
        }
        tex->chain_dirty = GL_TRUE;
        gl_pack_descriptors(tex);
        return;
    }

    /* **A mip level is its own image**, not the base one - see `mips` in gl_texture_object_t.
     * It lives in process memory with its rows packed tight; the hardware does not read it. So
     * does every level of a cube map's face, whose table is made the first time one is given. */
    gl_pixel_src_t s;
    gl_unpack_source(ctx, &f, pixels, width, height, &s);
    if (face >= 0 && !tex->cube) {
        const size_t table = 6u * OOPS_GL_MAX_TEXTURE_LEVELS * sizeof(gl_tex_level_t);
        tex->cube = (gl_tex_level_t *)gl_buffer_alloc(table);
        if (!tex->cube) {
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        memset(tex->cube, 0, table);
    }
    if (level > 0 || face >= 0) {
        gl_tex_level_t *lv = (face >= 0) ? &tex->cube[face * OOPS_GL_MAX_TEXTURE_LEVELS + level]
                                         : &tex->mips[level];
        const size_t slice = (size_t)width * (size_t)height;
        const size_t bytes = slice * (size_t)depth * 4u;
        uint8_t *dst = (uint8_t *)gl_buffer_alloc(bytes);
        if (!dst) {
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        memset(dst, 0, bytes);
        if (pixels) {
            for (size_t z = 0; z < (size_t)depth; z++) {
                for (size_t y = 0; y < (size_t)height; y++) {
                    uint8_t *row = dst + (z * slice + y * (size_t)width) * 4u;
                    gl_tex_store_row(ctx, &f, row, s.base + z * s.image_stride + y * s.row_stride,
                                     width, s.swap, base);
                }
            }
        }
        gl_buffer_release(lv->pixels);
        lv->pixels = dst;
        lv->width = width;
        lv->height = height;
        lv->depth = depth;
        lv->internal_format = internalformat;
        lv->base_format = base;
        tex->chain_dirty = GL_TRUE;
        /* A new face means the array built from the six is out of date (gl_tex_cube_upload). */
        if (face >= 0) tex->cube_hw_dirty = GL_TRUE;
        gl_tex_gen_mipmap_check(ctx, target, level);
        return;
    }

    size_t num_pixels = (size_t)width * (size_t)height;
    /* A linear image's rows are stored at a 256-byte pitch - 64 pixels at four bytes each.
     *
     * The note this replaces said the sampler takes the pitch from the width, measured
     * 2026-09-14, and closed with "widths that are not a multiple of 64 pixels are unmeasured".
     * That caveat was the whole answer: at a width of 64 pixels the row pitch *is* 256 bytes,
     * so a texture that wide cannot tell "pitch = width" apart from "pitch = width rounded up",
     * and the texture that measurement used was 64 wide - as gl-cube's still is.
     *
     * gl1-probe's textures are 2x2, where the two differ, and the split in its results says
     * which is right: `texture-wrap` passes on hardware while `texture-2d`, `tex-env-modes`,
     * `copy-tex` and `tex-sub-image` fail. The one that passes is the one that samples only
     * row 0 - it holds t at 0.25 throughout and varies s. Every check that needs a second row
     * fails, which is what a row pitch wider than the rows were written gives you.
     *
     * Mesa aligns a linear pitch the same way and carries it explicitly when it exceeds the
     * width (ac_descriptors.c:697-713, "GFX10.3+ can set a custom pitch for 1D and 2D
     * non-array, but it must be a multiple of 256B"); gl_pack_descriptors writes it.
     *
     * Rounding up cannot disturb what already works: for any width that is a multiple of 64
     * the pitch is the width, the descriptor field stays inert, and the bytes are laid out
     * exactly as before. */
    size_t pitch_px = ((size_t)width + 63u) & ~(size_t)63u;
    /* A volume's slices follow one another, each `pitch_px * height` pixels. */
    const size_t slice_px = pitch_px * (size_t)height;
    size_t rgba_bytes = slice_px * (size_t)depth * 4;

    /* Re-specifying a texture frees its old storage, and a draw already built into this frame
     * may still name that address. A no-op on the host, where there is no deferred frame. */
    gl_tex_storage_release_sync(ctx);
#ifndef OOPS_HOST_BUILD
    if (tex->garlic_data) {
        oops_mem_free(tex->garlic_data);
        tex->garlic_data = NULL;
    }
    tex->garlic_data = oops_mem_alloc(rgba_bytes, 256, OOPS_MEM_WC_GARLIC);
    if (!tex->garlic_data) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    tex->garlic_va = (uint64_t)(uintptr_t)tex->garlic_data;
    tex->pixels = tex->garlic_data;
    tex->pitch = (uint32_t)pitch_px;
    (void)num_pixels;

    /* Zeroed first, and unconditionally. A null `pixels` reserves storage to be filled by
     * glTexSubImage2D, and without this the sampler reads whatever the allocator last left in
     * GARLIC until that happens - the host branch below has always zeroed for exactly that
     * reason, and the two should not disagree. It also covers the padding between the rows and
     * the pitch, which no upload ever writes. */
    memset(tex->garlic_data, 0, rgba_bytes);
    if (pixels) {
        uint8_t *dst = (uint8_t *)tex->garlic_data;
        for (size_t z = 0; z < (size_t)depth; z++) {
            for (size_t y = 0; y < (size_t)height; y++) {
                uint8_t *row = dst + (z * slice_px + y * pitch_px) * 4;
                gl_tex_store_row(ctx, &f, row, s.base + z * s.image_stride + y * s.row_stride,
                                 width, s.swap, base);
            }
        }
    }
#if defined(__x86_64__)
    /* Outside the `if`: the zeroing above is a CPU write to write-combined memory too, and the
     * GPU has to see it whether or not an upload followed. */
    for (size_t p = 0; p < rgba_bytes; p += 64) {
        __builtin_ia32_clflush((const void *)((const char *)tex->garlic_data + p));
    }
#endif
#else
    if (tex->pixels) {
        free(tex->pixels);
    }
    tex->pixels = malloc(rgba_bytes);
    if (!tex->pixels) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    /* Zeroed first, and unconditionally - the same as the target branch above.
     *
     * A null pointer reserves the storage, which is legal and is how a program sets a texture
     * up to fill with glTexSubImage2D; zeroing means a draw before the first sub-image samples
     * black instead of whatever the allocator found. That much was always true here. What made
     * it unconditional is the row padding: an upload writes `width` pixels into a row `pitch`
     * wide, so with a `pixels` argument the gap between them was left as malloc returned it.
     * test_gl_copy_tex_sub_image_matches_a_read_then_upload caught it immediately - two
     * textures holding identical images compared unequal in the padding between their rows. */
    memset(tex->pixels, 0, rgba_bytes);
    if (pixels) {
        uint8_t *dst = (uint8_t *)tex->pixels;
        for (size_t z = 0; z < (size_t)depth; z++) {
            for (size_t y = 0; y < (size_t)height; y++) {
                uint8_t *row = dst + (z * slice_px + y * pitch_px) * 4;
                gl_tex_store_row(ctx, &f, row, s.base + z * s.image_stride + y * s.row_stride,
                                 width, s.swap, base);
            }
        }
    }
    (void)num_pixels;
#endif

    tex->width = width;
    tex->height = height;
    tex->depth = depth;
    /* **Set here rather than only in the target branch**, where it used to be. On a host build
     * it stayed zero, which was harmless for exactly as long as nothing read it - and
     * glTexSubImage2D reads it to find a row, so every row landed on top of row zero. A field
     * that is only correct under one build is the kind that is discovered by the first code to
     * depend on it. */
    tex->pitch = (uint32_t)pitch_px;
    tex->format = format;
    tex->type = type;
    tex->internal_format = internalformat;
    tex->base_format = base;
    tex->chain_dirty = GL_TRUE;
    gl_pack_descriptors(tex);
    gl_tex_gen_mipmap_check(ctx, target, level);
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                 GLsizei width, GLsizei height, GLint border,
                 GLenum format, GLenum type, const GLvoid *pixels) {
    /* The image is copied into the list now, through the unpack state of now - the program may
     * free or reuse its buffer the moment this returns, and a list replayed later must still
     * upload what was passed. A proxy is never compiled - it runs now, as the specification
     * says and Mesa does (main/dlist.c:4163). */
    if (gl_list_recording() && !gl_is_proxy_target(target) &&
        gl_list_rec_image(GL_LIST_OP_TEX_IMAGE_2D,
                          GL_LIST_ARGV(gl_la_e(target), gl_la_i(level), gl_la_i(internalformat),
                                       gl_la_i(width), gl_la_i(height), gl_la_i(border),
                                       gl_la_e(format), gl_la_e(type)),
                          8, width, height, format, type, pixels)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* 2D only: GL_TEXTURE_1D here is an error, not a shorthand for a height-1 image. A cube map's
     * faces are 2D images too, each through its own target (GL 1.3). */
    if (target != GL_TEXTURE_2D && target != GL_PROXY_TEXTURE_2D &&
        GL_CUBE_FACE_INDEX(target) < 0 && target != GL_PROXY_TEXTURE_CUBE_MAP) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* Refused as the 1D and 3D uploads refuse it - this one alone accepted a border and stored an
     * image without it until 2026-09-19. */
    if (border != 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    gl_tex_image_common(ctx, target, level, internalformat, width, height, 1, format, type, pixels);
}

/* -------------------------------------------------------------------------
 * Three-dimensional textures (GL 1.2)
 *
 * A volume is stored slice after slice, each slice laid out exactly as a 2D image is - so the
 * upload, sub-upload and copy helpers above serve it with a depth and a z offset, and slice z of
 * the caller's image starts GL_UNPACK_IMAGE_HEIGHT rows after slice z - 1 (or the image's own
 * height, when that is 0). Its binding, enable and default texture are its own, as 1D's are.
 *
 * **Sampled on the console too since 2026-09-20**, and this said it was the software
 * rasteriser's alone until 2026-09-21: it listed the three things the hardware path needed - a
 * 3D image descriptor, r carried to the pixel shader, a shader sampling with three coordinates -
 * and all three landed the day after it was written. `texture-3d` does not fail there; it
 * passes, and it is what measures the slice stride the mip chain relies on. The chain itself
 * followed on 2026-09-21 (`gl_tex_chain_layout_3d`); what a volume still lacks there is a chain
 * when its **depth** is not a power of two, which is the rule a width that is not has always
 * had.
 * ------------------------------------------------------------------------- */

/* A volume packed tight for a list, through the unpack state of now - the 2D recorder with one
 * more dimension. */
static GLboolean gl_list_rec_volume(gl_list_op_t op, const gl_list_arg_t *args, int nargs,
                                    GLsizei width, GLsizei height, GLsizei depth, GLenum format,
                                    GLenum type, const GLvoid *pixels) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GL_FALSE;
    gl_pixel_fmt_t f;
    void *img = (void *)0;
    size_t img_bytes = 0u;
    if (pixels && gl_pixel_fmt(format, type, &f) == GL_NO_ERROR &&
        width > 0 && height > 0 && depth > 0) {
        img = gl_pixel_copy_client(ctx, &f, pixels, width, height, depth);
        if (!img) return (GLboolean)(ctx->list_mode == GL_COMPILE);
        img_bytes = gl_pixel_packed_bytes(&f, width, height, depth);
    }
    return gl_list_rec_owned(op, args, nargs, img, img_bytes);
}

void glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                  GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type,
                  const GLvoid *pixels) {
    if (gl_list_recording() && !gl_is_proxy_target(target) &&
        gl_list_rec_volume(GL_LIST_OP_TEX_IMAGE_3D,
                           GL_LIST_ARGV(gl_la_e(target), gl_la_i(level), gl_la_i(internalformat),
                                        gl_la_i(width), gl_la_i(height), gl_la_i(depth),
                                        gl_la_i(border), gl_la_e(format), gl_la_e(type)),
                           9, width, height, depth, format, type, pixels)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_3D && target != GL_PROXY_TEXTURE_3D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (border != 0) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
    gl_tex_image_common(ctx, target, level, internalformat, width, height, depth, format, type,
                        pixels);
}

void glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                     GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                     const GLvoid *pixels) {
    if (gl_list_recording() &&
        gl_list_rec_volume(GL_LIST_OP_TEX_SUB_IMAGE_3D,
                           GL_LIST_ARGV(gl_la_e(target), gl_la_i(level), gl_la_i(xoffset),
                                        gl_la_i(yoffset), gl_la_i(zoffset), gl_la_i(width),
                                        gl_la_i(height), gl_la_i(depth), gl_la_e(format),
                                        gl_la_e(type)),
                           10, width, height, depth, format, type, pixels)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_3D) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    gl_tex_sub_image_common(ctx, target, level, xoffset, yoffset, zoffset, width, height, depth,
                            format, type, pixels);
}

/* **GL_EXT_texture3D's own spellings** (since 2026-09-20). A program written before GL 1.2 calls
 * these, not the core names, and the extension string promises them - gl.h's compatibility
 * section says why the two have to arrive together. The extension has exactly these two entry
 * points: `glCopyTexSubImage3D` is GL 1.2's, not this extension's. */
void glTexImage3DEXT(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                     GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type,
                     const GLvoid *pixels) {
    glTexImage3D(target, level, (GLint)internalformat, width, height, depth, border, format,
                 type, pixels);
}

void glTexSubImage3DEXT(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                        GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                        const GLvoid *pixels) {
    glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type,
                    pixels);
}

/* A framebuffer rectangle into one slice of a volume - there is no copy that makes a volume. */
void glCopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_COPY_TEX_SUB_IMAGE_3D, gl_la_e(target), gl_la_i(level),
                    gl_la_i(xoffset), gl_la_i(yoffset), gl_la_i(zoffset), gl_la_i(x), gl_la_i(y),
                    gl_la_i(width), gl_la_i(height))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_3D) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    if (zoffset < 0) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
    gl_copy_tex_sub_common(ctx, target, level, xoffset, yoffset, zoffset, x, y, width, height);
}

/* -------------------------------------------------------------------------
 * Compressed textures, of which there are none
 *
 * The specification allows the set of compressed formats to be empty, and this one is.
 * `GL_NUM_COMPRESSED_TEXTURE_FORMATS` reports 0, so a program that asks - which is what a program
 * using compressed textures is supposed to do - takes its uncompressed path. A program that does
 * not ask gets `GL_INVALID_ENUM`, which is the specification's answer for an internal format that
 * is not a supported compressed one.
 *
 * **These are conformant, not stubs.** The distinction matters here: the payload links with
 * `-Wl,--unresolved-symbols=ignore-all`, so leaving them out is not "an absent feature is an
 * absent symbol" (D009) in practice - it is a call to address zero with no diagnostic. A refusal
 * a program can read is strictly better than a crash it cannot.
 * ------------------------------------------------------------------------- */
static void gl_compressed_refuse(gl_context_t *ctx, GLenum target) {
    /* An unknown target is reported as such; a known one fails on the format, because there is
     * no compressed format this accepts. Both are GL_INVALID_ENUM, but getting the reason right
     * matters to anyone reading the code later. */
    (void)target;
    gl_record_error(ctx, GL_INVALID_ENUM);
}

void glCompressedTexImage1D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                            GLint border, GLsizei imageSize, const GLvoid *data) {
    (void)level; (void)internalformat; (void)width; (void)border; (void)imageSize; (void)data;
    gl_context_t *ctx = gl_get_ctx();
    if (ctx) gl_compressed_refuse(ctx, target);
}

void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                            GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data) {
    (void)level; (void)internalformat; (void)width; (void)height;
    (void)border; (void)imageSize; (void)data;
    gl_context_t *ctx = gl_get_ctx();
    if (ctx) gl_compressed_refuse(ctx, target);
}

void glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                            GLsizei height, GLsizei depth, GLint border, GLsizei imageSize,
                            const GLvoid *data) {
    (void)level; (void)internalformat; (void)width; (void)height; (void)depth;
    (void)border; (void)imageSize; (void)data;
    gl_context_t *ctx = gl_get_ctx();
    if (ctx) gl_compressed_refuse(ctx, target);
}

void glCompressedTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width,
                               GLenum format, GLsizei imageSize, const GLvoid *data) {
    (void)level; (void)xoffset; (void)width; (void)format; (void)imageSize; (void)data;
    gl_context_t *ctx = gl_get_ctx();
    if (ctx) gl_compressed_refuse(ctx, target);
}

void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                               GLsizei width, GLsizei height, GLenum format, GLsizei imageSize,
                               const GLvoid *data) {
    (void)level; (void)xoffset; (void)yoffset; (void)width; (void)height;
    (void)format; (void)imageSize; (void)data;
    gl_context_t *ctx = gl_get_ctx();
    if (ctx) gl_compressed_refuse(ctx, target);
}

void glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                               GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                               GLenum format, GLsizei imageSize, const GLvoid *data) {
    (void)level; (void)xoffset; (void)yoffset; (void)zoffset;
    (void)width; (void)height; (void)depth; (void)format; (void)imageSize; (void)data;
    gl_context_t *ctx = gl_get_ctx();
    if (ctx) gl_compressed_refuse(ctx, target);
}

/* No texture here is compressed, so this is GL_INVALID_OPERATION - which is what the
 * specification says for a query against a texture whose image is uncompressed, and is a
 * different answer from the format refusals above. */
void glGetCompressedTexImage(GLenum target, GLint level, GLvoid *img) {
    (void)level; (void)img;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (!gl_tex_image_target_ok(target)) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    gl_record_error(ctx, GL_INVALID_OPERATION);
}

/* -------------------------------------------------------------------------
 * One-dimensional textures
 *
 * A 1D image is stored as a height-1 2D one, because that is what the sampler reads and what the
 * descriptor describes - the row pitch rule and the upload path are unchanged. **What is not
 * shared is the binding**: GL_TEXTURE_1D has its own binding point, its own enable and its own
 * default texture, which is the whole reason these are separate entry points rather than callers
 * passing height = 1.
 *
 * `border` must be zero. GL 1.x allows a one-texel border and this has never stored one; a
 * non-zero border is refused rather than ignored, because a program that asks for one and is
 * given a texture without it samples the wrong texels at the edges.
 * ------------------------------------------------------------------------- */
void glTexImage1D(GLenum target, GLint level, GLint internalFormat, GLsizei width,
                  GLint border, GLenum format, GLenum type, const GLvoid *pixels) {
    if (gl_list_recording() && !gl_is_proxy_target(target) &&
        gl_list_rec_image(GL_LIST_OP_TEX_IMAGE_1D,
                          GL_LIST_ARGV(gl_la_e(target), gl_la_i(level), gl_la_i(internalFormat),
                                       gl_la_i(width), gl_la_i(border), gl_la_e(format),
                                       gl_la_e(type)),
                          7, width, 1, format, type, pixels)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_1D && target != GL_PROXY_TEXTURE_1D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (border != 0) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
    /* The 2D path does the work, against the 1D binding - which is why the binding is resolved
     * from the target rather than assumed. */
    gl_tex_image_common(ctx, target, level, internalFormat, width, 1, 1, format, type, pixels);
}

void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width,
                     GLenum format, GLenum type, const GLvoid *pixels) {
    if (gl_list_recording() &&
        gl_list_rec_image(GL_LIST_OP_TEX_SUB_IMAGE_1D,
                          GL_LIST_ARGV(gl_la_e(target), gl_la_i(level), gl_la_i(xoffset),
                                       gl_la_i(width), gl_la_e(format), gl_la_e(type)),
                          6, width, 1, format, type, pixels)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_1D) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    gl_tex_sub_image_common(ctx, target, level, xoffset, 0, 0, width, 1, 1, format, type, pixels);
}

void glCopyTexImage1D(GLenum target, GLint level, GLenum internalFormat,
                      GLint x, GLint y, GLsizei width, GLint border) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_COPY_TEX_IMAGE_1D, gl_la_e(target), gl_la_i(level),
                    gl_la_e(internalFormat), gl_la_i(x), gl_la_i(y), gl_la_i(width),
                    gl_la_i(border))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_1D || !gl_copy_internal_format_ok(internalFormat)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (border != 0) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
    const GLboolean depth = (GLboolean)(gl_tex_base_format((GLint)internalFormat) == GL_DEPTH_COMPONENT);
    glTexImage1D(target, level, (GLint)internalFormat, width, 0,
                 depth ? (GLenum)GL_DEPTH_COMPONENT : (GLenum)GL_RGBA,
                 depth ? (GLenum)GL_FLOAT : (GLenum)GL_UNSIGNED_BYTE, NULL);
    /* **Checked against the level, not with glGetError** - which this used, and which *clears*
     * the error flag: an error from the allocation above was consumed here and never reached the
     * program, and an older unrelated error aborted a copy that had every right to run. The 2D
     * version's comment warns of exactly this. */
    gl_tex_view_t lv;
    if (!gl_tex_level_view(gl_texture_for_target(ctx, target, GL_FALSE), level, &lv) ||
        lv.width != width) {
        return;
    }
    glCopyTexSubImage1D(target, level, 0, x, y, width);
}

void glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset,
                         GLint x, GLint y, GLsizei width) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_COPY_TEX_SUB_IMAGE_1D, gl_la_e(target), gl_la_i(level),
                    gl_la_i(xoffset), gl_la_i(x), gl_la_i(y), gl_la_i(width))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_1D) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    gl_copy_tex_sub_common(ctx, target, level, xoffset, 0, 0, x, y, width, 1);
}

/* **The one texture-parameter setter**, every form reaching it with floats - an enum as its value
 * (exact in a float), the border colour as four. Mesa's rules (main/texparam.c,
 * set_tex_parameteri and set_tex_parameterf):
 *
 * - a wrap mode is GL_REPEAT, GL_CLAMP, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER or GL_MIRRORED_REPEAT
 *   (validate_texture_wrap_mode, :64), a minification filter one of the six and a magnification
 *   filter GL_NEAREST or GL_LINEAR; anything else is GL_INVALID_ENUM and changes nothing. Until
 *   2026-09-19 any value was stored, and a mistyped filter sampled as GL_NEAREST;
 * - GL_TEXTURE_PRIORITY is clamped to [0, 1] (:828) and GL_TEXTURE_BORDER_COLOR's four channels
 *   too (:877, without float textures). Both were refused before;
 * - GL_TEXTURE_RESIDENT is a query, not a parameter;
 * - GL 1.2's GL_TEXTURE_BASE_LEVEL and GL_TEXTURE_MAX_LEVEL are non-negative integers, and
 *   GL_TEXTURE_MIN_LOD and GL_TEXTURE_MAX_LOD any float. All four were refused until
 *   2026-09-19. */
static GLboolean gl_tex_wrap_ok(GLenum w) {
    return (GLboolean)(w == GL_REPEAT || w == GL_CLAMP || w == GL_CLAMP_TO_EDGE ||
                       w == GL_CLAMP_TO_BORDER || w == GL_MIRRORED_REPEAT);
}

static void gl_tex_parameter(GLenum target, GLenum pname, const GLfloat *p) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (!gl_texture_target_ok(target)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_texture_object_t *tex = gl_texture_for_target(ctx, target, GL_TRUE);
    if (!tex) return;
    const GLenum e = (GLenum)(GLint)p[0];
    switch (pname) {
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
        case GL_TEXTURE_WRAP_R:
            if (!gl_tex_wrap_ok(e)) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            if (pname == GL_TEXTURE_WRAP_S) tex->wrap_s = e;
            else if (pname == GL_TEXTURE_WRAP_T) tex->wrap_t = e;
            else tex->wrap_r = e;
            break;
        case GL_TEXTURE_MIN_FILTER:
            if (e != GL_NEAREST && e != GL_LINEAR && !gl_filter_uses_mipmaps(e)) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            tex->min_filter = e;
            break;
        case GL_TEXTURE_MAG_FILTER:
            if (e != GL_NEAREST && e != GL_LINEAR) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            tex->mag_filter = e;
            break;
        case GL_TEXTURE_PRIORITY:
            tex->priority = (p[0] > 1.0f) ? 1.0f : ((p[0] > 0.0f) ? p[0] : 0.0f);
            return; /* nothing the sampler reads */
        case GL_TEXTURE_BORDER_COLOR:
            for (int i = 0; i < 4; i++) {
                tex->border_color[i] = (p[i] > 1.0f) ? 1.0f : ((p[i] > 0.0f) ? p[i] : 0.0f);
            }
            break;
        /* GL 1.2's levels: an integer each, negative a value error (Mesa, main/texparam.c:389).
         * A float given for one is truncated, as Mesa's float setter converts it. */
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_MAX_LEVEL: {
            const GLint v = (p[0] >= 2147483647.0f) ? 2147483647 : (GLint)p[0];
            if (v < 0) {
                gl_record_error(ctx, GL_INVALID_VALUE);
                return;
            }
            if (pname == GL_TEXTURE_BASE_LEVEL) tex->base_level = v;
            else tex->max_level = v;
            break;
        }
        /* And the clamp on the level of detail: any float. */
        case GL_TEXTURE_MIN_LOD:
            tex->min_lod = p[0];
            break;
        case GL_TEXTURE_MAX_LOD:
            tex->max_lod = p[0];
            break;
        /* GL 1.4's bias: any float, clamped with the unit's where it is used (Mesa
         * main/texparam.c:861-872). The draw adds it to the sampler word, not the descriptor
         * here, because the unit's half is context state. */
        case GL_TEXTURE_LOD_BIAS:
            tex->lod_bias = p[0];
            return;
        /* GL 1.4's automatic mipmaps: a boolean, acted on at the next change to the base level,
         * not now. */
        case GL_GENERATE_MIPMAP:
            tex->generate_mipmap = (p[0] != 0.0f) ? GL_TRUE : GL_FALSE;
            return;
        /* GL 1.4's depth-texture parameters, checked as Mesa checks them (main/texparam.c): the
         * mode GL_NONE or GL_COMPARE_R_TO_TEXTURE, the function any of the eight (GL 1.5 widened
         * 1.4's two), the depth mode luminance, intensity or alpha - an enum error otherwise.
         *
         * **The first two reach the hardware sampler since 2026-09-20**: obSCEne's `-6c80`
         * measured DEPTH_COMPARE_FUNC working on the part, so they break to the repack below
         * rather than returning. They were state for the software sampler alone while the
         * hardware drew a depth texture untextured, and a descriptor that did not follow them
         * would have compared against whatever the field last held. `GL_DEPTH_TEXTURE_MODE`
         * still returns: it says which channels the comparison's result fills, which the shader
         * does and the sampler does not. */
        case GL_TEXTURE_COMPARE_MODE:
            if (e != GL_NONE && e != GL_COMPARE_R_TO_TEXTURE) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            tex->compare_mode = e;
            break;
        case GL_TEXTURE_COMPARE_FUNC:
            if (e < GL_NEVER || e > GL_ALWAYS) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            tex->compare_func = e;
            break;
        case GL_DEPTH_TEXTURE_MODE:
            if (e != GL_LUMINANCE && e != GL_INTENSITY && e != GL_ALPHA) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            tex->depth_mode = e;
            return;
        /* The target was checked above and the parameter was not, so a caller setting one
         * this does not keep - a later version's swizzle, say - had it dropped and went on
         * believing the texture was configured. */
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return;
    }
    gl_pack_descriptors(tex);
}

/* The scalar forms refuse the one vector parameter, as Mesa does ("non-scalar pname"). */
void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_TEX_PARAMETER_I, gl_la_e(target), gl_la_e(pname), gl_la_i(param))) {
        return;
    }
    if (pname == GL_TEXTURE_BORDER_COLOR) {
        gl_context_t *ctx = gl_get_ctx();
        if (ctx) gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const GLfloat f[4] = {(GLfloat)param, 0.0f, 0.0f, 0.0f};
    gl_tex_parameter(target, pname, f);
}

/* **Its own path, not glTexParameteri's.** This truncated to an integer on the way through, so
 * glTexParameterf(GL_TEXTURE_PRIORITY, 0.5f) would have set 0 - and a list compiled it as the
 * integer form too. */
void glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_TEX_PARAMETER_F, gl_la_e(target), gl_la_e(pname), gl_la_f(param))) {
        return;
    }
    if (pname == GL_TEXTURE_BORDER_COLOR) {
        gl_context_t *ctx = gl_get_ctx();
        if (ctx) gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const GLfloat f[4] = {param, 0.0f, 0.0f, 0.0f};
    gl_tex_parameter(target, pname, f);
}

/* The vector forms read four values for GL_TEXTURE_BORDER_COLOR and one for anything else. An
 * integer border colour converts by range (INT_TO_FLOAT, main/texparam.c:1170), anything else by
 * a plain cast. A null pointer is ignored rather than dereferenced. */
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) {
    if (!params) return;
    if (gl_list_recording() &&
        gl_list_rec_fv(GL_LIST_OP_TEX_PARAMETER_FV, target, pname, GL_TRUE, pname, params)) {
        return;
    }
    GLfloat f[4] = {params[0], 0.0f, 0.0f, 0.0f};
    if (pname == GL_TEXTURE_BORDER_COLOR) {
        for (int i = 1; i < 4; i++) f[i] = params[i];
    }
    gl_tex_parameter(target, pname, f);
}

void glTexParameteriv(GLenum target, GLenum pname, const GLint *params) {
    if (!params) return;
    if (gl_list_recording() &&
        gl_list_rec_iv(GL_LIST_OP_TEX_PARAMETER_IV, target, pname, GL_TRUE, pname, params)) {
        return;
    }
    GLfloat f[4] = {(GLfloat)params[0], 0.0f, 0.0f, 0.0f};
    if (pname == GL_TEXTURE_BORDER_COLOR) {
        for (int i = 0; i < 4; i++) f[i] = gl_int_to_colour(params[i]);
    }
    gl_tex_parameter(target, pname, f);
}

/* **Texture residency, answered honestly rather than optimistically.**
 *
 * Every texture here lives in memory the GPU can already reach and nothing evicts them, so they
 * are all resident and glAreTexturesResident returns GL_TRUE. A name that was never generated
 * is an error, not a "no" - a program asking about a texture it does not own has a bug, and
 * reporting it as merely non-resident would hide that.
 */
GLboolean glAreTexturesResident(GLsizei n, const GLuint *textures, GLboolean *residences) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !textures || !residences) return GL_FALSE;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return GL_FALSE;
    }
    for (GLsizei i = 0; i < n; i++) {
        if (!gl_find_texture(ctx, textures[i])) {
            gl_record_error(ctx, GL_INVALID_VALUE);
            return GL_FALSE;
        }
        residences[i] = GL_TRUE;
    }
    return GL_TRUE;
}

/* A hint about which textures to keep resident, on an implementation where nothing is ever
 * evicted. Accepted and ignored, which is what the specification permits a hint to be - but the
 * names are still validated, so a typo is still an error. */
void glPrioritizeTextures(GLsizei n, const GLuint *textures, const GLclampf *priorities) {
    /* Compiled with both arrays copied, names first and priorities after, in one block. */
    if (textures && n > 0 && gl_list_recording()) {
        const size_t names_bytes = (size_t)n * sizeof(GLuint);
        uint8_t *block = (uint8_t *)gl_list_alloc(names_bytes + (size_t)n * sizeof(GLclampf));
        gl_context_t *rctx = gl_get_ctx();
        if (!block) {
            gl_record_error(rctx, GL_OUT_OF_MEMORY);
            if (rctx->list_mode == GL_COMPILE) return;
        } else {
            memcpy(block, textures, names_bytes);
            GLclampf *pri = (GLclampf *)(block + names_bytes);
            for (GLsizei i = 0; i < n; i++) pri[i] = priorities ? priorities[i] : 0.0f;
            if (gl_list_rec_owned(GL_LIST_OP_PRIORITIZE_TEXTURES, GL_LIST_ARGV(gl_la_i(n)), 1,
                                  block, names_bytes + (size_t)n * sizeof(GLclampf))) {
                return;
            }
        }
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !textures) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        /* Name 0 is silently ignored here, as GL says, rather than refused. */
        if (textures[i] != 0u && !gl_find_texture(ctx, textures[i])) {
            gl_record_error(ctx, GL_INVALID_VALUE);
            return;
        }
    }
    /* **Kept now**, clamped as glTexParameter clamps it, so GL_TEXTURE_PRIORITY reads it back.
     * Until 2026-09-19 the priorities were validated and dropped. */
    for (GLsizei i = 0; i < n && priorities; i++) {
        gl_texture_object_t *t = gl_find_texture(ctx, textures[i]);
        if (t) t->priority = (priorities[i] > 1.0f) ? 1.0f : ((priorities[i] > 0.0f) ? priorities[i] : 0.0f);
    }
}

GLboolean glIsTexture(GLuint texture) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || texture == 0) return GL_FALSE;
    return gl_find_texture(ctx, texture) != NULL ? GL_TRUE : GL_FALSE;
}

/* -------------------------------------------------------------------------
 * Framebuffer objects
 *
 * The object layer: names, storage, attachments and the completeness rules. What it does *not*
 * yet do is redirect a draw - see glCheckFramebufferStatus, which says so in the only way a
 * caller can act on.
 * ------------------------------------------------------------------------- */

/* The lookups themselves are in `gl_internal.h`, because `gl_draw_targets` needs them from
 * another file. These are the names the entry points below were written against. */
static gl_framebuffer_object_t *gl_find_framebuffer(gl_context_t *ctx, GLuint id) {
    return gl_framebuffer_slot(ctx, id);
}

static gl_renderbuffer_object_t *gl_find_renderbuffer(gl_context_t *ctx, GLuint id) {
    return gl_renderbuffer_slot(ctx, id);
}

/* The attachment point a name selects, or NULL for one this GL does not have. GL_COLOR_ATTACHMENT0
 * only: ES 2.0 has exactly one colour attachment and GL_MAX_COLOR_ATTACHMENTS is 1. */
static gl_fb_attachment_t *gl_fb_attachment_for(gl_framebuffer_object_t *fb, GLenum attachment) {
    if (!fb) return NULL;
    switch (attachment) {
        case GL_COLOR_ATTACHMENT0: return &fb->color0;
        case GL_DEPTH_ATTACHMENT: return &fb->depth;
        case GL_STENCIL_ATTACHMENT: return &fb->stencil;
        default: return NULL;
    }
}

/* An attachment's size, and whether it has storage at all. A texture level that was never given
 * an image, and a renderbuffer with no glRenderbufferStorage, are both attachments that exist
 * and are not renderable - which is the difference between INCOMPLETE_ATTACHMENT and
 * INCOMPLETE_MISSING_ATTACHMENT. */
static GLboolean gl_fb_attachment_size(gl_context_t *ctx, const gl_fb_attachment_t *at,
                                       GLsizei *w, GLsizei *h) {
    *w = 0;
    *h = 0;
    if (!at || at->kind == GL_FB_ATTACH_NONE) return GL_FALSE;
    if (at->kind == GL_FB_ATTACH_RENDERBUFFER) {
        const gl_renderbuffer_object_t *rb = gl_find_renderbuffer(ctx, at->name);
        if (!rb || !rb->pixels) return GL_FALSE;
        *w = rb->width;
        *h = rb->height;
        return GL_TRUE;
    }
    const gl_texture_object_t *tex = gl_find_texture(ctx, at->name);
    if (!tex) return GL_FALSE;
    gl_tex_view_t view;
    if (!gl_tex_level_view(tex, at->level, &view)) return GL_FALSE;
    *w = view.width;
    *h = view.height;
    return GL_TRUE;
}

void glGenFramebuffers(GLsizei n, GLuint *framebuffers) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !framebuffers) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    GLuint next_id = 1;
    for (GLsizei i = 0; i < n; i++) {
        while (next_id <= (GLuint)OOPS_GL_MAX_FRAMEBUFFER_OBJECTS &&
               ctx->framebuffers[next_id - 1u].used) {
            next_id++;
        }
        if (next_id > (GLuint)OOPS_GL_MAX_FRAMEBUFFER_OBJECTS) {
            framebuffers[i] = 0;
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            continue;
        }
        gl_framebuffer_object_t *fb = &ctx->framebuffers[next_id - 1u];
        memset(fb, 0, sizeof(*fb));
        fb->id = next_id;
        fb->used = GL_TRUE;
        framebuffers[i] = next_id;
        next_id++;
    }
}

void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !framebuffers) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        const GLuint id = framebuffers[i];
        if (id == 0) continue;
        gl_framebuffer_object_t *fb = gl_find_framebuffer(ctx, id);
        if (!fb) continue;
        /* Deleting the bound framebuffer binds 0 in its place, as GL says - otherwise the
         * binding names a slot that has been cleared and every later draw reads it. */
        if (ctx->bound_framebuffer == id) ctx->bound_framebuffer = 0;
        memset(fb, 0, sizeof(*fb));
    }
    gl_draw_targets(ctx);
}

void glBindFramebuffer(GLenum target, GLuint framebuffer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_FRAMEBUFFER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (framebuffer != 0 && !gl_find_framebuffer(ctx, framebuffer)) {
        /* **A name glGenFramebuffers never gave.** ES 2.0 refuses it; desktop GL before 3.0 let
         * a bind create one. This takes the ES rule, because that is the surface being answered
         * here and the other spelling is not offered. */
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    ctx->bound_framebuffer = framebuffer;
    gl_draw_targets(ctx);
}

GLboolean glIsFramebuffer(GLuint framebuffer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || framebuffer == 0) return GL_FALSE;
    return gl_find_framebuffer(ctx, framebuffer) ? GL_TRUE : GL_FALSE;
}

void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !renderbuffers) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    GLuint next_id = 1;
    for (GLsizei i = 0; i < n; i++) {
        while (next_id <= (GLuint)OOPS_GL_MAX_RENDERBUFFER_OBJECTS &&
               ctx->renderbuffers[next_id - 1u].used) {
            next_id++;
        }
        if (next_id > (GLuint)OOPS_GL_MAX_RENDERBUFFER_OBJECTS) {
            renderbuffers[i] = 0;
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            continue;
        }
        gl_renderbuffer_object_t *rb = &ctx->renderbuffers[next_id - 1u];
        memset(rb, 0, sizeof(*rb));
        rb->id = next_id;
        rb->used = GL_TRUE;
        renderbuffers[i] = next_id;
        next_id++;
    }
}

void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !renderbuffers) return;
    if (n < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        const GLuint id = renderbuffers[i];
        if (id == 0) continue;
        gl_renderbuffer_object_t *rb = gl_find_renderbuffer(ctx, id);
        if (!rb) continue;
        gl_buffer_release(rb->pixels);
        if (ctx->bound_renderbuffer == id) ctx->bound_renderbuffer = 0;
        /* **Every attachment naming it goes too**, on every framebuffer and not only the bound
         * one, which is what GL says happens and what keeps gl_fb_attachment_size from looking
         * up a name that no longer exists. */
        for (int f = 0; f < OOPS_GL_MAX_FRAMEBUFFER_OBJECTS; f++) {
            gl_framebuffer_object_t *fb = &ctx->framebuffers[f];
            if (!fb->used) continue;
            gl_fb_attachment_t *slots[3] = {&fb->color0, &fb->depth, &fb->stencil};
            for (int s = 0; s < 3; s++) {
                if (slots[s]->kind == GL_FB_ATTACH_RENDERBUFFER && slots[s]->name == id) {
                    memset(slots[s], 0, sizeof(*slots[s]));
                }
            }
        }
        memset(rb, 0, sizeof(*rb));
    }
    gl_draw_targets(ctx);
}

void glBindRenderbuffer(GLenum target, GLuint renderbuffer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_RENDERBUFFER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (renderbuffer != 0 && !gl_find_renderbuffer(ctx, renderbuffer)) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    ctx->bound_renderbuffer = renderbuffer;
}

GLboolean glIsRenderbuffer(GLuint renderbuffer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || renderbuffer == 0) return GL_FALSE;
    return gl_find_renderbuffer(ctx, renderbuffer) ? GL_TRUE : GL_FALSE;
}

void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_RENDERBUFFER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_renderbuffer_object_t *rb = gl_find_renderbuffer(ctx, ctx->bound_renderbuffer);
    if (!rb) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (width < 0 || height < 0 ||
        width > OOPS_GL_MAX_TEXTURE_SIZE || height > OOPS_GL_MAX_TEXTURE_SIZE) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    switch (internalformat) {
        case GL_RGBA4: case GL_RGB5_A1: case GL_RGB565: case GL_RGBA8: case GL_RGB8:
        case GL_DEPTH_COMPONENT16_ARB: case GL_DEPTH_COMPONENT24: case GL_STENCIL_INDEX8:
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return;
    }
    /* **One word a sample whatever the format.** A renderbuffer is not sampled, so nothing reads
     * it in its declared layout - the format is kept to answer the queries and to decide
     * completeness, and the storage is the width this GL's colour and depth buffers already are.
     * Packing RGB565 tightly would save memory a render target does not have a shortage of, and
     * would need a second addressing path through the rasteriser. */
    gl_buffer_release(rb->pixels);
    rb->pixels = NULL;
    rb->internal_format = internalformat;
    rb->width = width;
    rb->height = height;
    if (width > 0 && height > 0) {
        const size_t bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
        rb->pixels = (uint32_t *)gl_buffer_alloc(bytes);
        if (!rb->pixels) {
            rb->width = 0;
            rb->height = 0;
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        memset(rb->pixels, 0, bytes);
    }
    /* Storage is what made this attachment renderable, or what moved it. */
    gl_draw_targets(ctx);
}

void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (target != GL_RENDERBUFFER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const gl_renderbuffer_object_t *rb = gl_find_renderbuffer(ctx, ctx->bound_renderbuffer);
    if (!rb) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    const GLenum fmt = rb->internal_format;
    switch (pname) {
        case GL_RENDERBUFFER_WIDTH: *params = (GLint)rb->width; break;
        case GL_RENDERBUFFER_HEIGHT: *params = (GLint)rb->height; break;
        case GL_RENDERBUFFER_INTERNAL_FORMAT: *params = (GLint)fmt; break;
        /* The sizes the *declared* format promises, not the storage above - a program choosing a
         * format reads these back to find out what it got, and answering 8888 for an RGB565
         * renderbuffer would report a precision it did not ask for and cannot rely on. */
        case GL_RENDERBUFFER_RED_SIZE:
            *params = (fmt == GL_RGBA4) ? 4 : (fmt == GL_RGB5_A1 || fmt == GL_RGB565) ? 5
                    : (fmt == GL_RGBA8 || fmt == GL_RGB8) ? 8 : 0;
            break;
        case GL_RENDERBUFFER_GREEN_SIZE:
            *params = (fmt == GL_RGBA4) ? 4 : (fmt == GL_RGB5_A1) ? 5 : (fmt == GL_RGB565) ? 6
                    : (fmt == GL_RGBA8 || fmt == GL_RGB8) ? 8 : 0;
            break;
        case GL_RENDERBUFFER_BLUE_SIZE:
            *params = (fmt == GL_RGBA4) ? 4 : (fmt == GL_RGB5_A1 || fmt == GL_RGB565) ? 5
                    : (fmt == GL_RGBA8 || fmt == GL_RGB8) ? 8 : 0;
            break;
        case GL_RENDERBUFFER_ALPHA_SIZE:
            *params = (fmt == GL_RGBA4) ? 4 : (fmt == GL_RGB5_A1) ? 1 : (fmt == GL_RGBA8) ? 8 : 0;
            break;
        case GL_RENDERBUFFER_DEPTH_SIZE:
            *params = (fmt == GL_DEPTH_COMPONENT16_ARB) ? 16
                    : (fmt == GL_DEPTH_COMPONENT24) ? 24 : 0;
            break;
        case GL_RENDERBUFFER_STENCIL_SIZE:
            *params = (fmt == GL_STENCIL_INDEX8) ? 8 : 0;
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                            GLuint texture, GLint level) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_FRAMEBUFFER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* **Attaching to the window-system framebuffer is an error, not a no-op.** Name 0 is the
     * display and has no attachment points; a program that gets here has forgotten its bind. */
    gl_framebuffer_object_t *fb = gl_find_framebuffer(ctx, ctx->bound_framebuffer);
    if (!fb) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    gl_fb_attachment_t *at = gl_fb_attachment_for(fb, attachment);
    if (!at) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (textarget != GL_TEXTURE_2D &&
        !(textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
          textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (level < 0 || level >= OOPS_GL_MAX_TEXTURE_LEVELS) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (texture == 0) {
        memset(at, 0, sizeof(*at));
        gl_draw_targets(ctx);
        return;
    }
    if (!gl_find_texture(ctx, texture)) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    at->kind = GL_FB_ATTACH_TEXTURE;
    at->name = texture;
    at->textarget = textarget;
    at->level = level;
    /* The target the draw path holds was chosen from the attachments as they were. */
    gl_draw_targets(ctx);
}

void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                               GLuint renderbuffer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_FRAMEBUFFER || renderbuffertarget != GL_RENDERBUFFER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_framebuffer_object_t *fb = gl_find_framebuffer(ctx, ctx->bound_framebuffer);
    if (!fb) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    gl_fb_attachment_t *at = gl_fb_attachment_for(fb, attachment);
    if (!at) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (renderbuffer == 0) {
        memset(at, 0, sizeof(*at));
        gl_draw_targets(ctx);
        return;
    }
    if (!gl_find_renderbuffer(ctx, renderbuffer)) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    at->kind = GL_FB_ATTACH_RENDERBUFFER;
    at->name = renderbuffer;
    at->textarget = 0;
    at->level = 0;
    gl_draw_targets(ctx);
}

void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname,
                                           GLint *params) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !params) return;
    if (target != GL_FRAMEBUFFER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_framebuffer_object_t *fb = gl_find_framebuffer(ctx, ctx->bound_framebuffer);
    if (!fb) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    const gl_fb_attachment_t *at = gl_fb_attachment_for(fb, attachment);
    if (!at) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    switch (pname) {
        case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
            *params = (at->kind == GL_FB_ATTACH_TEXTURE) ? (GLint)GL_TEXTURE
                    : (at->kind == GL_FB_ATTACH_RENDERBUFFER) ? (GLint)GL_RENDERBUFFER
                    : (GLint)GL_NONE;
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
            /* **Only when something is attached.** GL says this query is an error against an
             * empty attachment point rather than answering zero - the caller is expected to have
             * asked for the type first. */
            if (at->kind == GL_FB_ATTACH_NONE) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            *params = (GLint)at->name;
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
            if (at->kind != GL_FB_ATTACH_TEXTURE) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            *params = at->level;
            break;
        case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
            if (at->kind != GL_FB_ATTACH_TEXTURE) {
                gl_record_error(ctx, GL_INVALID_ENUM);
                return;
            }
            *params = (at->textarget == GL_TEXTURE_2D) ? 0 : (GLint)at->textarget;
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

GLenum glCheckFramebufferStatus(GLenum target) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return 0;
    if (target != GL_FRAMEBUFFER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    /* The window-system framebuffer is always complete - there is nothing to attach to it. */
    gl_framebuffer_object_t *fb = gl_find_framebuffer(ctx, ctx->bound_framebuffer);
    if (!fb) return GL_FRAMEBUFFER_COMPLETE;

    GLsizei cw = 0, ch = 0, dw = 0, dh = 0, sw = 0, sh = 0;
    const GLboolean has_c = gl_fb_attachment_size(ctx, &fb->color0, &cw, &ch);
    const GLboolean has_d = gl_fb_attachment_size(ctx, &fb->depth, &dw, &dh);
    const GLboolean has_s = gl_fb_attachment_size(ctx, &fb->stencil, &sw, &sh);

    /* An attachment point that names something with no storage behind it. */
    if ((fb->color0.kind != GL_FB_ATTACH_NONE && !has_c) ||
        (fb->depth.kind != GL_FB_ATTACH_NONE && !has_d) ||
        (fb->stencil.kind != GL_FB_ATTACH_NONE && !has_s)) {
        return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
    }
    if (!has_c && !has_d && !has_s) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;

    /* ES 2.0 requires every attachment to share one size; desktop GL 3.0 dropped the rule. */
    const GLsizei w = has_c ? cw : has_d ? dw : sw;
    const GLsizei h = has_c ? ch : has_d ? dh : sh;
    if ((has_c && (cw != w || ch != h)) || (has_d && (dw != w || dh != h)) ||
        (has_s && (sw != w || sh != h))) {
        return GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS;
    }

    /*
     * **Complete by the rules, and only if a draw can actually reach it.**
     *
     * `gl_fbo_bound_target` is the function `gl_draw_targets` uses to point the colour buffer at
     * the attachment, so asking it here is asking the draw path directly rather than restating
     * its conditions - the two cannot drift, and a COMPLETE from this function is a promise the
     * next draw keeps. What it refuses is the scanout and hardware paths, whose colour buffers
     * are addressed in a swizzle an attachment is not in, and a base-level texture whose rows
     * are further apart than they are wide.
     *
     * GL_FRAMEBUFFER_UNSUPPORTED is the specification's answer for exactly that, and callers
     * must handle it. Reporting COMPLETE instead would be the flattering answer and a false one:
     * the program would draw, the pixels would land on the display, and a suite reading the
     * attachment afterwards would get whatever was there before - a pass in some cases and an
     * unexplained failure in others. A refusal makes dEQP report NotSupported, which is true.
     */
    gl_fb_storage_t colour;
    float *depth = NULL;
    const GLuint saved = ctx->bound_framebuffer;
    ctx->bound_framebuffer = fb->id;
    const GLboolean renderable = gl_fbo_bound_target(ctx, &colour, &depth);
    ctx->bound_framebuffer = saved;
    return renderable ? (GLenum)GL_FRAMEBUFFER_COMPLETE : (GLenum)GL_FRAMEBUFFER_UNSUPPORTED;
}

/*
 * **ES 2.0's explicit call, over GL 1.4's automatic one.**
 *
 * `gl_tex_generate_mipmap` already builds the chain, because GL_GENERATE_MIPMAP (GL 1.4, 3.8.8)
 * has needed it since long before this - it is the same box filter over the same levels, and it
 * already handles a volume, a cube face, GL_TEXTURE_BASE_LEVEL and GL_TEXTURE_MAX_LEVEL. The
 * only thing ES adds is asking for it by hand rather than on every upload.
 *
 * So this is the entry point and the validation, and none of the filtering. A second reduction
 * written here would have been a second answer to "what is the level above this one", and the
 * two would have drifted.
 */
void glGenerateMipmap(GLenum target) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP &&
        target != GL_TEXTURE_1D && target != GL_TEXTURE_3D) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    gl_texture_object_t *tex = gl_texture_for_target(ctx, target, GL_FALSE);
    if (!tex) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }

    if (target == GL_TEXTURE_CUBE_MAP) {
        /* **Every face, and only if the cube is complete.** GL says a cube map whose six faces
         * do not agree in size and format is an error here rather than six independent chains -
         * a sampler reading such a map has no defined level to read from. */
        gl_tex_view_t f0;
        if (!tex->cube || !gl_tex_face_view(tex, 0, tex->base_level, &f0)) {
            gl_record_error(ctx, GL_INVALID_OPERATION);
            return;
        }
        for (int face = 1; face < 6; face++) {
            gl_tex_view_t fv;
            if (!gl_tex_face_view(tex, face, tex->base_level, &fv) ||
                fv.width != f0.width || fv.height != f0.height ||
                fv.internal_format != f0.internal_format) {
                gl_record_error(ctx, GL_INVALID_OPERATION);
                return;
            }
        }
        for (int face = 0; face < 6; face++) gl_tex_generate_mipmap(ctx, tex, face);
        return;
    }

    /* No base image is an error, not a texture with one empty chain - and it has to be checked
     * here, because the generator answers an absent base by returning quietly. */
    gl_tex_view_t base;
    if (!gl_tex_level_view(tex, tex->base_level, &base)) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    gl_tex_generate_mipmap(ctx, tex, -1);
}

/* -------------------------------------------------------------------------
 * The ES 2.0 spellings, and the calls only ES has
 * ------------------------------------------------------------------------- */

void glClearDepthf(GLclampf depth) { glClearDepth((GLclampd)depth); }

void glDepthRangef(GLclampf zNear, GLclampf zFar) {
    glDepthRange((GLclampd)zNear, (GLclampd)zFar);
}

void glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype,
                                GLint *range, GLint *precision) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (shadertype != GL_VERTEX_SHADER && shadertype != GL_FRAGMENT_SHADER) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    /* **Every precision here is IEEE single.** ES allows a fragment shader to carry less, and
     * the values below are what the specification names for an implementation that does not:
     * the range and precision of binary32, and 24 bits for the integer types because that is
     * what a float holds exactly. Nothing in this GL narrows anything to mediump. */
    switch (precisiontype) {
        case GL_LOW_FLOAT: case GL_MEDIUM_FLOAT: case GL_HIGH_FLOAT:
            if (range) { range[0] = 127; range[1] = 127; }
            if (precision) *precision = 23;
            break;
        case GL_LOW_INT: case GL_MEDIUM_INT: case GL_HIGH_INT:
            if (range) { range[0] = 24; range[1] = 24; }
            if (precision) *precision = 0;
            break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            break;
    }
}

/* A hint that the compiler's memory may be reclaimed. This compiler is part of the library and
 * has nothing to release, and the specification allows ignoring it - a later glCompileShader
 * must work regardless, which is what makes ignoring it correct rather than lazy. */
void glReleaseShaderCompiler(void) { }

void glShaderBinary(GLsizei count, const GLuint *shaders, GLenum binaryformat,
                    const void *binary, GLsizei length) {
    gl_context_t *ctx = gl_get_ctx();
    (void)shaders;
    (void)binary;
    (void)binaryformat;
    if (!ctx) return;
    if (count < 0 || length < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* GL_NUM_SHADER_BINARY_FORMATS is zero here, so no value of `binaryformat` is valid and
     * GL_INVALID_ENUM is the required answer rather than a stub's silence. */
    gl_record_error(ctx, GL_INVALID_ENUM);
}
