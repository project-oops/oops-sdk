/*
 * oops-gl: the attribute stack
 *
 * `glPushAttrib` saves the state its mask names and `glPopAttrib` puts it back. Older GL code
 * leans on this heavily - a routine that changes state politely brackets itself with a push and
 * a pop - and its absence does not merely render differently: **state leaks out of the routine
 * that set it** and everything drawn afterwards is wrong, in a way that looks like a bug
 * somewhere else entirely.
 *
 * # Save everything, restore what the mask names
 *
 * A push copies all of the state below regardless of the mask, and records the mask; the pop
 * puts back only the groups the mask named. Saving selectively would be a little cheaper and a
 * lot easier to get subtly wrong - a field saved under one bit and restored under another is
 * the kind of mistake that only shows up in the one program that uses both.
 *
 * # What is not covered, and why that is an error
 *
 * The state this subset does not have - accumulation buffer, stencil, fog, evaluators, polygon
 * stipple, hints, pixel mode, points and lines - cannot be saved because it does not exist. A
 * mask naming one of those is refused with GL_INVALID_ENUM rather than quietly ignored: a
 * program that pushes GL_STENCIL_BUFFER_BIT is relying on getting stencil state back, and
 * returning without it would be a silent lie of exactly the kind a push-and-pop exists to
 * prevent.
 */

#include "gl_internal.h"

/* The attribute groups this can save. A mask outside these is refused. */
#define GL_ATTRIB_SUPPORTED                                                     \
    (GL_CURRENT_BIT | GL_POLYGON_BIT | GL_LIGHTING_BIT | GL_DEPTH_BUFFER_BIT |  \
     GL_VIEWPORT_BIT | GL_TRANSFORM_BIT | GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | \
     GL_LIST_BIT | GL_TEXTURE_BIT | GL_SCISSOR_BIT)

void glPushAttrib(GLbitfield mask) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    /* GL_ALL_ATTRIB_BITS is the common call and names groups this does not have. Narrowed to
     * what exists rather than refused, because refusing it would make the most ordinary use of
     * this function fail - and unlike a program naming GL_STENCIL_BUFFER_BIT deliberately, a
     * program saying "all" is asking for whatever there is. */
    if (mask == GL_ALL_ATTRIB_BITS) {
        mask = GL_ATTRIB_SUPPORTED;
    }
    if ((mask & ~(GLbitfield)GL_ATTRIB_SUPPORTED) != 0u) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (ctx->attrib_depth >= OOPS_GL_ATTRIB_STACK_CAPACITY) {
        gl_record_error(ctx, GL_STACK_OVERFLOW);
        return;
    }

    gl_attrib_entry_t *e = &ctx->attrib_stack[ctx->attrib_depth++];
    e->mask = mask;

    for (int i = 0; i < 4; i++) e->cur_color[i] = ctx->cur_color[i];
    for (int i = 0; i < 3; i++) e->cur_normal[i] = ctx->cur_normal[i];
    for (int i = 0; i < 2; i++) e->cur_texcoord[i] = ctx->cur_texcoord[i];

    e->cap_depth_test = ctx->cap_depth_test;
    e->cap_cull_face = ctx->cap_cull_face;
    e->cap_blend = ctx->cap_blend;
    e->cap_scissor_test = ctx->cap_scissor_test;
    e->cap_lighting = ctx->cap_lighting;
    e->cap_texture_2d = ctx->cap_texture_2d;
    e->cap_normalize = ctx->cap_normalize;
    e->cap_color_material = ctx->cap_color_material;
    e->cap_alpha_test = ctx->cap_alpha_test;
    e->cap_polygon_offset_fill = ctx->cap_polygon_offset_fill;

    e->depth_func = ctx->depth_func;
    e->depth_mask = ctx->depth_mask;
    e->clear_depth = ctx->clear_depth;

    e->blend_src = ctx->blend_src;
    e->blend_dst = ctx->blend_dst;
    e->blend_src_alpha = ctx->blend_src_alpha;
    e->blend_dst_alpha = ctx->blend_dst_alpha;
    e->blend_equation = ctx->blend_equation;
    for (int i = 0; i < 4; i++) e->color_mask[i] = ctx->color_mask[i];
    for (int i = 0; i < 4; i++) e->clear_color[i] = ctx->clear_color[i];
    e->alpha_func = ctx->alpha_func;
    e->alpha_ref = ctx->alpha_ref;

    e->cull_mode = ctx->cull_mode;
    e->front_face = ctx->front_face;
    e->polygon_offset_factor = ctx->polygon_offset_factor;
    e->polygon_offset_units = ctx->polygon_offset_units;

    e->shade_model = ctx->shade_model;
    for (int i = 0; i < OOPS_GL_LIGHT_COUNT; i++) e->lights[i] = ctx->lights[i];
    e->mat_front = ctx->mat_front;
    e->mat_back = ctx->mat_back;
    for (int i = 0; i < 4; i++) e->light_model_ambient[i] = ctx->light_model_ambient[i];

    e->bound_texture_2d = ctx->bound_texture_2d;
    e->tex_env_mode = ctx->tex_env_mode;
    for (int i = 0; i < 4; i++) e->tex_env_color[i] = ctx->tex_env_color[i];

    e->vp_x = ctx->vp_x; e->vp_y = ctx->vp_y;
    e->vp_w = ctx->vp_w; e->vp_h = ctx->vp_h;
    e->depth_near = ctx->depth_near;
    e->depth_far = ctx->depth_far;

    e->sc_x = ctx->sc_x; e->sc_y = ctx->sc_y;
    e->sc_w = ctx->sc_w; e->sc_h = ctx->sc_h;

    e->matrix_mode = ctx->matrix_mode;
    e->list_base = ctx->list_base;
}

void glPopAttrib(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (ctx->attrib_depth == 0u) {
        gl_record_error(ctx, GL_STACK_UNDERFLOW);
        return;
    }

    const gl_attrib_entry_t *e = &ctx->attrib_stack[--ctx->attrib_depth];
    const GLbitfield mask = e->mask;

    if (mask & GL_CURRENT_BIT) {
        for (int i = 0; i < 4; i++) ctx->cur_color[i] = e->cur_color[i];
        for (int i = 0; i < 3; i++) ctx->cur_normal[i] = e->cur_normal[i];
        for (int i = 0; i < 2; i++) ctx->cur_texcoord[i] = e->cur_texcoord[i];
    }

    /* **The enables belong to more than one bit.** GL_ENABLE_BIT carries all of them, and each
     * buffer bit also carries the enable for its own feature - so GL_DEPTH_BUFFER_BIT restores
     * the depth-test enable even without GL_ENABLE_BIT. Getting this wrong gives a pop that
     * restores a depth function while leaving the test off. */
    const GLboolean all_enables = (GLboolean)((mask & GL_ENABLE_BIT) != 0u);
    if (all_enables || (mask & GL_DEPTH_BUFFER_BIT)) ctx->cap_depth_test = e->cap_depth_test;
    if (all_enables || (mask & GL_POLYGON_BIT)) {
        ctx->cap_cull_face = e->cap_cull_face;
        ctx->cap_polygon_offset_fill = e->cap_polygon_offset_fill;
    }
    if (all_enables || (mask & GL_COLOR_BUFFER_BIT)) {
        ctx->cap_blend = e->cap_blend;
        ctx->cap_alpha_test = e->cap_alpha_test;
    }
    if (all_enables || (mask & GL_SCISSOR_BIT)) ctx->cap_scissor_test = e->cap_scissor_test;
    if (all_enables || (mask & GL_LIGHTING_BIT)) {
        ctx->cap_lighting = e->cap_lighting;
        ctx->cap_color_material = e->cap_color_material;
    }
    if (all_enables || (mask & GL_TEXTURE_BIT)) ctx->cap_texture_2d = e->cap_texture_2d;
    if (all_enables || (mask & GL_TRANSFORM_BIT)) ctx->cap_normalize = e->cap_normalize;

    if (mask & GL_DEPTH_BUFFER_BIT) {
        ctx->depth_func = e->depth_func;
        ctx->depth_mask = e->depth_mask;
        ctx->clear_depth = e->clear_depth;
    }

    if (mask & GL_COLOR_BUFFER_BIT) {
        ctx->blend_src = e->blend_src;
        ctx->blend_dst = e->blend_dst;
        ctx->blend_src_alpha = e->blend_src_alpha;
        ctx->blend_dst_alpha = e->blend_dst_alpha;
        ctx->blend_equation = e->blend_equation;
        for (int i = 0; i < 4; i++) ctx->color_mask[i] = e->color_mask[i];
        for (int i = 0; i < 4; i++) ctx->clear_color[i] = e->clear_color[i];
        ctx->alpha_func = e->alpha_func;
        ctx->alpha_ref = e->alpha_ref;
    }

    if (mask & GL_POLYGON_BIT) {
        ctx->cull_mode = e->cull_mode;
        ctx->front_face = e->front_face;
        ctx->polygon_offset_factor = e->polygon_offset_factor;
        ctx->polygon_offset_units = e->polygon_offset_units;
    }

    if (mask & GL_LIGHTING_BIT) {
        ctx->shade_model = e->shade_model;
        for (int i = 0; i < OOPS_GL_LIGHT_COUNT; i++) ctx->lights[i] = e->lights[i];
        ctx->mat_front = e->mat_front;
        ctx->mat_back = e->mat_back;
        for (int i = 0; i < 4; i++) ctx->light_model_ambient[i] = e->light_model_ambient[i];
    }

    if (mask & GL_TEXTURE_BIT) {
        ctx->bound_texture_2d = e->bound_texture_2d;
        ctx->tex_env_mode = e->tex_env_mode;
        for (int i = 0; i < 4; i++) ctx->tex_env_color[i] = e->tex_env_color[i];
    }

    if (mask & GL_VIEWPORT_BIT) {
        ctx->vp_x = e->vp_x; ctx->vp_y = e->vp_y;
        ctx->vp_w = e->vp_w; ctx->vp_h = e->vp_h;
        ctx->depth_near = e->depth_near;
        ctx->depth_far = e->depth_far;
    }

    if (mask & GL_SCISSOR_BIT) {
        ctx->sc_x = e->sc_x; ctx->sc_y = e->sc_y;
        ctx->sc_w = e->sc_w; ctx->sc_h = e->sc_h;
    }

    if (mask & GL_TRANSFORM_BIT) ctx->matrix_mode = e->matrix_mode;
    if (mask & GL_LIST_BIT) ctx->list_base = e->list_base;

    /* The alpha test lives in the shader, not in a register the next frame re-emits, so a
     * restore has to rewrite it. Everything else above is picked up from the context when the
     * next frame's register table is built. */
    if (mask & (GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT)) {
        gl_ps_patch_alpha_test(ctx);
    }
}

/* -------------------------------------------------------------------------
 * The client attribute stack
 *
 * Two groups, both of which live in this process rather than in a register: the array pointers
 * and the pixel-store modes. It is a second stack rather than a second mask on the first one
 * because the specification makes them independent - a helper that pushes client state and a
 * caller that pushed server state must not pop each other's frames.
 *
 * Unlike glPushAttrib there is nothing here to refuse: the two bits the specification defines
 * are both groups this has. GL_CLIENT_ALL_ATTRIB_BITS therefore needs no narrowing.
 * ------------------------------------------------------------------------- */

#define GL_CLIENT_ATTRIB_SUPPORTED (GL_CLIENT_PIXEL_STORE_BIT | GL_CLIENT_VERTEX_ARRAY_BIT)

void glPushClientAttrib(GLbitfield mask) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    if (mask == GL_CLIENT_ALL_ATTRIB_BITS) {
        mask = GL_CLIENT_ATTRIB_SUPPORTED;
    }
    /* A bit outside the two the specification defines names nothing at all, so this is an enum
     * error rather than the "state this port lacks" refusal glPushAttrib makes. */
    if ((mask & ~(GLbitfield)GL_CLIENT_ATTRIB_SUPPORTED) != 0u) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (ctx->client_attrib_depth >= OOPS_GL_CLIENT_ATTRIB_STACK_CAPACITY) {
        gl_record_error(ctx, GL_STACK_OVERFLOW);
        return;
    }

    gl_client_attrib_entry_t *e = &ctx->client_attrib_stack[ctx->client_attrib_depth++];
    e->mask = mask;

    e->array_vertex = ctx->array_vertex;
    e->array_color = ctx->array_color;
    e->array_normal = ctx->array_normal;
    e->array_texcoord = ctx->array_texcoord;

    e->unpack_alignment = ctx->unpack_alignment;
    e->unpack_row_length = ctx->unpack_row_length;
    e->pack_alignment = ctx->pack_alignment;
}

void glPopClientAttrib(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (ctx->client_attrib_depth == 0u) {
        gl_record_error(ctx, GL_STACK_UNDERFLOW);
        return;
    }

    const gl_client_attrib_entry_t *e = &ctx->client_attrib_stack[--ctx->client_attrib_depth];

    /* **The enable travels with the pointer.** GL_CLIENT_VERTEX_ARRAY_BIT covers
     * glEnableClientState as well as glVertexPointer, and restoring one without the other would
     * leave an array enabled with a pointer that was never set for it - a dangling read rather
     * than a wrong picture. `gl_client_array_t` holds both, so copying the struct keeps them
     * together by construction. */
    if (e->mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
        ctx->array_vertex = e->array_vertex;
        ctx->array_color = e->array_color;
        ctx->array_normal = e->array_normal;
        ctx->array_texcoord = e->array_texcoord;
    }

    if (e->mask & GL_CLIENT_PIXEL_STORE_BIT) {
        ctx->unpack_alignment = e->unpack_alignment;
        ctx->unpack_row_length = e->unpack_row_length;
        ctx->pack_alignment = e->pack_alignment;
    }
}
