/*
 * oops-gl: the attribute stack
 *
 * `glPushAttrib` saves the state its mask names and `glPopAttrib` puts it back.
 *
 * A push copies all of the state below regardless of the mask, and records the mask;
 * the pop puts back only the groups the mask named. Saving everything keeps a field
 * from being saved under one bit and restored under another.
 *
 * Every GL 1.x group exists. A bit outside them names nothing and is refused with
 * GL_INVALID_ENUM rather than ignored, since a program that pushes a group relies on
 * getting that state back.
 */

#include "gl_internal.h"

/* The targets whose bound texture's parameters GL_TEXTURE_BIT saves, in the entry's
 * order. */
static const GLenum k_attrib_tex_targets[4] = {GL_TEXTURE_1D, GL_TEXTURE_2D,
                                               GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP};

/* The attribute groups this can save. A mask outside these is refused. */
#define GL_ATTRIB_SUPPORTED                                                            \
    (GL_CURRENT_BIT | GL_POLYGON_BIT | GL_LIGHTING_BIT | GL_DEPTH_BUFFER_BIT |         \
     GL_VIEWPORT_BIT | GL_TRANSFORM_BIT | GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT |        \
     GL_LIST_BIT | GL_TEXTURE_BIT | GL_SCISSOR_BIT | GL_STENCIL_BUFFER_BIT |           \
     GL_FOG_BIT | GL_POINT_BIT | GL_LINE_BIT | GL_HINT_BIT | GL_MULTISAMPLE_BIT |      \
     GL_EVAL_BIT | GL_PIXEL_MODE_BIT | GL_POLYGON_STIPPLE_BIT | GL_ACCUM_BUFFER_BIT)

void glPushAttrib(GLbitfield mask) {
    /* The server stack is compiled into lists; the client one below is not. */
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_PUSH_ATTRIB, gl_la_u(mask)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;

    /* "All" names bits beyond the defined groups, so it is narrowed to what exists
     * rather than refused. Both spellings count: the Khronos 0xFFFFFFFF and GL 1.0's
     * 0x000FFFFF. */
    if (mask == GL_ALL_ATTRIB_BITS || mask == 0x000FFFFFu) {
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

    for (int i = 0; i < 4; i++)
        e->cur_color[i] = ctx->cur_color[i];
    for (int i = 0; i < 3; i++)
        e->cur_normal[i] = ctx->cur_normal[i];
    memcpy(e->cur_texcoord, ctx->cur_texcoord,
           sizeof(e->cur_texcoord)); /* every unit's */
    memcpy(e->raster_pos, ctx->raster_pos, sizeof(e->raster_pos));
    memcpy(e->raster_color, ctx->raster_color, sizeof(e->raster_color));
    memcpy(e->raster_texcoord, ctx->raster_texcoord, sizeof(e->raster_texcoord));
    e->raster_distance = ctx->raster_distance;
    e->raster_valid = ctx->raster_valid;
    e->cur_edge_flag = ctx->cur_edge_flag;
    e->cur_index = ctx->cur_index;
    for (int i = 0; i < 4; i++)
        e->cur_secondary[i] = ctx->cur_secondary[i];
    e->cur_fog_coord = ctx->cur_fog_coord;

    e->cap_dither = ctx->cap_dither;
    e->cap_index_logic_op = ctx->cap_index_logic_op;
    e->cap_multisample = ctx->cap_multisample;
    e->cap_sample_alpha_to_coverage = ctx->cap_sample_alpha_to_coverage;
    e->cap_sample_alpha_to_one = ctx->cap_sample_alpha_to_one;
    e->cap_sample_coverage = ctx->cap_sample_coverage;
    e->sample_coverage_value = ctx->sample_coverage_value;
    e->sample_coverage_invert = ctx->sample_coverage_invert;
    e->point_size = ctx->point_size;
    e->point_size_min = ctx->point_size_min;
    e->point_size_max = ctx->point_size_max;
    e->point_fade_threshold = ctx->point_fade_threshold;
    for (int i = 0; i < 3; i++)
        e->point_atten[i] = ctx->point_atten[i];
    e->line_width = ctx->line_width;
    e->cap_line_stipple = ctx->cap_line_stipple;
    e->cap_point_smooth = ctx->cap_point_smooth;
    /* GL_POINT_BIT's, like the smoothing enable beside it, and GL_ENABLE_BIT's. The
       sprite origin goes with it: the specification files GL_POINT_SPRITE_COORD_ORIGIN
       under the same group as the rest of the point state. GL_COORD_REPLACE is per unit
       and belongs to GL_TEXTURE_BIT instead, saved with the rest of the unit below. */
    e->cap_point_sprite = ctx->cap_point_sprite;
    e->point_sprite_origin = ctx->point_sprite_origin;
    e->cap_line_smooth = ctx->cap_line_smooth;
    e->cap_polygon_smooth = ctx->cap_polygon_smooth;
    e->line_stipple_factor = ctx->line_stipple_factor;
    e->line_stipple_pattern = ctx->line_stipple_pattern;
    e->cap_polygon_stipple = ctx->cap_polygon_stipple;
    for (int i = 0; i < 32; i++)
        e->polygon_stipple[i] = ctx->polygon_stipple[i];
    for (int i = 0; i < 4; i++)
        e->accum_clear[i] = ctx->accum_clear[i];
    for (int i = 0; i < OOPS_GL_EVAL_MAPS; i++) {
        e->cap_map1[i] = ctx->cap_map1[i];
        e->cap_map2[i] = ctx->cap_map2[i];
    }
    e->cap_auto_normal = ctx->cap_auto_normal;
    e->grid1_un = ctx->grid1_un;
    e->grid1_u1 = ctx->grid1_u1;
    e->grid1_u2 = ctx->grid1_u2;
    e->grid2_un = ctx->grid2_un;
    e->grid2_vn = ctx->grid2_vn;
    e->grid2_u1 = ctx->grid2_u1;
    e->grid2_u2 = ctx->grid2_u2;
    e->grid2_v1 = ctx->grid2_v1;
    e->grid2_v2 = ctx->grid2_v2;
    for (int i = 0; i < 4; i++) {
        e->pixel_scale[i] = ctx->pixel_scale[i];
        e->pixel_bias[i] = ctx->pixel_bias[i];
    }
    e->depth_scale = ctx->depth_scale;
    e->depth_bias = ctx->depth_bias;
    e->index_shift = ctx->index_shift;
    e->index_offset = ctx->index_offset;
    e->map_color = ctx->map_color;
    e->map_stencil = ctx->map_stencil;
    e->pixel_zoom_x = ctx->pixel_zoom_x;
    e->pixel_zoom_y = ctx->pixel_zoom_y;
    e->perspective_hint = ctx->perspective_hint;
    e->hint_point_smooth = ctx->hint_point_smooth;
    e->hint_line_smooth = ctx->hint_line_smooth;
    e->hint_polygon_smooth = ctx->hint_polygon_smooth;
    e->hint_fog = ctx->hint_fog;
    e->hint_texture_compression = ctx->hint_texture_compression;
    e->hint_generate_mipmap = ctx->hint_generate_mipmap;
    e->clear_index = ctx->clear_index;
    e->index_mask = ctx->index_mask;

    e->cap_depth_test = ctx->cap_depth_test;
    e->cap_cull_face = ctx->cap_cull_face;
    e->cap_blend = ctx->cap_blend;
    e->cap_scissor_test = ctx->cap_scissor_test;
    e->cap_lighting = ctx->cap_lighting;
    e->cap_normalize = ctx->cap_normalize;
    e->cap_rescale_normal = ctx->cap_rescale_normal;
    e->cap_color_material = ctx->cap_color_material;
    e->cap_alpha_test = ctx->cap_alpha_test;
    e->cap_polygon_offset_fill = ctx->cap_polygon_offset_fill;
    e->cap_polygon_offset_line = ctx->cap_polygon_offset_line;
    e->cap_polygon_offset_point = ctx->cap_polygon_offset_point;

    e->depth_func = ctx->depth_func;
    e->depth_mask = ctx->depth_mask;
    e->clear_depth = ctx->clear_depth;

    e->blend_src = ctx->blend_src;
    e->blend_dst = ctx->blend_dst;
    e->blend_src_alpha = ctx->blend_src_alpha;
    e->blend_dst_alpha = ctx->blend_dst_alpha;
    e->blend_equation = ctx->blend_equation;
    e->blend_equation_alpha = ctx->blend_equation_alpha;
    for (int i = 0; i < 4; i++)
        e->blend_color[i] = ctx->blend_color[i];
    e->cap_color_logic_op = ctx->cap_color_logic_op;
    e->logic_op = ctx->logic_op;
    for (int i = 0; i < 4; i++)
        e->color_mask[i] = ctx->color_mask[i];
    e->draw_buffer = ctx->draw_buffer; /* GL_COLOR_BUFFER_BIT's (GL 1.3, table 6.21) */
    e->read_buffer = ctx->read_buffer; /* GL_PIXEL_MODE_BIT's (table 6.18) */
    for (int i = 0; i < 4; i++)
        e->clear_color[i] = ctx->clear_color[i];
    e->alpha_func = ctx->alpha_func;
    e->alpha_ref = ctx->alpha_ref;

    e->cull_mode = ctx->cull_mode;
    e->front_face = ctx->front_face;
    e->polygon_offset_factor = ctx->polygon_offset_factor;
    e->polygon_offset_units = ctx->polygon_offset_units;
    e->polygon_mode[0] = ctx->polygon_mode[0];
    e->polygon_mode[1] = ctx->polygon_mode[1];

    e->shade_model = ctx->shade_model;
    for (int i = 0; i < OOPS_GL_LIGHT_COUNT; i++)
        e->lights[i] = ctx->lights[i];
    e->mat_front = ctx->mat_front;
    e->mat_back = ctx->mat_back;
    for (int i = 0; i < 4; i++)
        e->light_model_ambient[i] = ctx->light_model_ambient[i];
    e->light_model_local_viewer = ctx->light_model_local_viewer;
    e->light_model_color_control = ctx->light_model_color_control;
    e->light_model_two_side = ctx->light_model_two_side;
    e->color_material_face = ctx->color_material_face;
    e->color_material_mode = ctx->color_material_mode;

    /* Every unit, whole - the texture matrix stack is copied too, and ignored by the
     * pop. */
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        const gl_tex_unit_t *tu = &ctx->tex_unit[u];
        e->tex_units[u] = *tu;
        for (int i = 0; i < 4; i++) {
            const GLuint bound = (i == 0)   ? tu->bound_texture_1d
                                 : (i == 1) ? tu->bound_texture_2d
                                 : (i == 2) ? tu->bound_texture_3d
                                            : tu->bound_texture_cube;
            const GLuint id =
                bound ? bound : gl_default_texture_id(k_attrib_tex_targets[i]);
            const gl_texture_object_t *t = gl_lookup_texture(ctx, id);
            e->tex_param_id[u][i] = t ? id : 0u;
            if (!t)
                continue;
            e->tex_params[u][i].wrap_s = t->wrap_s;
            e->tex_params[u][i].wrap_t = t->wrap_t;
            e->tex_params[u][i].wrap_r = t->wrap_r;
            e->tex_params[u][i].min_filter = t->min_filter;
            e->tex_params[u][i].mag_filter = t->mag_filter;
            for (int k = 0; k < 4; k++)
                e->tex_params[u][i].border_color[k] = t->border_color[k];
            e->tex_params[u][i].priority = t->priority;
            e->tex_params[u][i].base_level = t->base_level;
            e->tex_params[u][i].max_level = t->max_level;
            e->tex_params[u][i].min_lod = t->min_lod;
            e->tex_params[u][i].max_lod = t->max_lod;
            e->tex_params[u][i].lod_bias = t->lod_bias;
            e->tex_params[u][i].generate_mipmap = t->generate_mipmap;
            e->tex_params[u][i].compare_mode = t->compare_mode;
            e->tex_params[u][i].compare_func = t->compare_func;
            e->tex_params[u][i].depth_mode = t->depth_mode;
        }
    }
    e->active_texture = ctx->active_texture;

    e->vp_x = ctx->vp_x;
    e->vp_y = ctx->vp_y;
    e->vp_w = ctx->vp_w;
    e->vp_h = ctx->vp_h;
    e->depth_near = ctx->depth_near;
    e->depth_far = ctx->depth_far;

    e->sc_x = ctx->sc_x;
    e->sc_y = ctx->sc_y;
    e->sc_w = ctx->sc_w;
    e->sc_h = ctx->sc_h;

    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        e->clip_plane_enabled[i] = ctx->clip_plane_enabled[i];
        for (int k = 0; k < 4; k++)
            e->clip_plane[i][k] = ctx->clip_plane[i][k];
    }

    e->cap_fog = ctx->cap_fog;
    e->cap_color_sum = ctx->cap_color_sum;
    e->fog_coord_src = ctx->fog_coord_src;
    e->fog_index = ctx->fog_index;
    e->fog_mode = ctx->fog_mode;
    e->fog_density = ctx->fog_density;
    e->fog_start = ctx->fog_start;
    e->fog_end = ctx->fog_end;
    for (int i = 0; i < 4; i++)
        e->fog_color[i] = ctx->fog_color[i];

    e->cap_stencil_test = ctx->cap_stencil_test;
    e->stencil_func = ctx->stencil_func;
    e->stencil_ref = ctx->stencil_ref;
    e->stencil_value_mask = ctx->stencil_value_mask;
    e->stencil_writemask = ctx->stencil_writemask;
    e->stencil_fail = ctx->stencil_fail;
    e->stencil_zfail = ctx->stencil_zfail;
    e->stencil_zpass = ctx->stencil_zpass;
    e->stencil_back_func = ctx->stencil_back_func;
    e->stencil_back_ref = ctx->stencil_back_ref;
    e->stencil_back_value_mask = ctx->stencil_back_value_mask;
    e->stencil_back_writemask = ctx->stencil_back_writemask;
    e->stencil_back_fail = ctx->stencil_back_fail;
    e->stencil_back_zfail = ctx->stencil_back_zfail;
    e->stencil_back_zpass = ctx->stencil_back_zpass;
    e->clear_stencil = ctx->clear_stencil;

    e->matrix_mode = ctx->matrix_mode;
    e->list_base = ctx->list_base;
}

void glPopAttrib(void) {
    if (gl_list_recording() && GL_LIST_REC0(GL_LIST_OP_POP_ATTRIB))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (ctx->attrib_depth == 0u) {
        gl_record_error(ctx, GL_STACK_UNDERFLOW);
        return;
    }

    const gl_attrib_entry_t *e = &ctx->attrib_stack[--ctx->attrib_depth];
    const GLbitfield mask = e->mask;

    if (mask & GL_CURRENT_BIT) {
        for (int i = 0; i < 4; i++)
            ctx->cur_color[i] = e->cur_color[i];
        for (int i = 0; i < 3; i++)
            ctx->cur_normal[i] = e->cur_normal[i];
        memcpy(ctx->cur_texcoord, e->cur_texcoord, sizeof(ctx->cur_texcoord));
        memcpy(ctx->raster_pos, e->raster_pos, sizeof(ctx->raster_pos));
        memcpy(ctx->raster_color, e->raster_color, sizeof(ctx->raster_color));
        memcpy(ctx->raster_texcoord, e->raster_texcoord, sizeof(ctx->raster_texcoord));
        ctx->raster_distance = e->raster_distance;
        ctx->raster_valid = e->raster_valid;
        ctx->cur_edge_flag = e->cur_edge_flag;
        ctx->cur_index = e->cur_index;
        for (int i = 0; i < 4; i++)
            ctx->cur_secondary[i] = e->cur_secondary[i];
        ctx->cur_fog_coord = e->cur_fog_coord;
    }

    /* The enables belong to more than one bit. GL_ENABLE_BIT carries all of them, and
     * each buffer bit also carries the enable for its own feature - so
     * GL_DEPTH_BUFFER_BIT restores the depth-test enable even without GL_ENABLE_BIT. */
    const GLboolean all_enables = (GLboolean)((mask & GL_ENABLE_BIT) != 0u);
    if (all_enables || (mask & GL_DEPTH_BUFFER_BIT))
        ctx->cap_depth_test = e->cap_depth_test;
    if (all_enables || (mask & GL_POLYGON_BIT)) {
        ctx->cap_cull_face = e->cap_cull_face;
        ctx->cap_polygon_offset_fill = e->cap_polygon_offset_fill;
        ctx->cap_polygon_offset_line = e->cap_polygon_offset_line;
        ctx->cap_polygon_offset_point = e->cap_polygon_offset_point;
    }
    if (all_enables || (mask & GL_COLOR_BUFFER_BIT)) {
        ctx->cap_blend = e->cap_blend;
        ctx->cap_alpha_test = e->cap_alpha_test;
        ctx->cap_color_logic_op = e->cap_color_logic_op;
        ctx->hw_color_control_dirty = GL_TRUE;
        ctx->cap_dither = e->cap_dither;
        ctx->cap_index_logic_op = e->cap_index_logic_op;
    }
    if (all_enables || (mask & GL_MULTISAMPLE_BIT)) {
        ctx->cap_multisample = e->cap_multisample;
        ctx->cap_sample_alpha_to_coverage = e->cap_sample_alpha_to_coverage;
        ctx->cap_sample_alpha_to_one = e->cap_sample_alpha_to_one;
        ctx->cap_sample_coverage = e->cap_sample_coverage;
    }
    if (mask & GL_MULTISAMPLE_BIT) {
        ctx->sample_coverage_value = e->sample_coverage_value;
        ctx->sample_coverage_invert = e->sample_coverage_invert;
    }
    if (mask & GL_POINT_BIT) {
        ctx->point_size = e->point_size;
        /* GL 1.4's parameters are the point group's too (Mesa main/attrib.c:936-939).
         */
        ctx->point_size_min = e->point_size_min;
        ctx->point_size_max = e->point_size_max;
        ctx->point_fade_threshold = e->point_fade_threshold;
        for (int i = 0; i < 3; i++)
            ctx->point_atten[i] = e->point_atten[i];
    }
    if (mask & GL_LINE_BIT) {
        ctx->line_width = e->line_width;
        ctx->line_stipple_factor = e->line_stipple_factor;
        ctx->line_stipple_pattern = e->line_stipple_pattern;
    }
    /* Each stipple's enable belongs to its own group as well as GL_ENABLE_BIT - the
     * line's to GL_LINE_BIT, the polygon's to GL_POLYGON_BIT - and the polygon's mask
     * to a group of its own. */
    if (all_enables || (mask & GL_LINE_BIT))
        ctx->cap_line_stipple = e->cap_line_stipple;
    if (all_enables || (mask & GL_POLYGON_BIT))
        ctx->cap_polygon_stipple = e->cap_polygon_stipple;
    if (all_enables || (mask & GL_POINT_BIT))
        ctx->cap_point_smooth = e->cap_point_smooth;
    if (all_enables || (mask & GL_POINT_BIT))
        ctx->cap_point_sprite = e->cap_point_sprite;
    if (mask & GL_POINT_BIT)
        ctx->point_sprite_origin = e->point_sprite_origin;
    if (all_enables || (mask & GL_LINE_BIT))
        ctx->cap_line_smooth = e->cap_line_smooth;
    if (all_enables || (mask & GL_POLYGON_BIT))
        ctx->cap_polygon_smooth = e->cap_polygon_smooth;
    if (mask & GL_POLYGON_STIPPLE_BIT) {
        for (int i = 0; i < 32; i++)
            ctx->polygon_stipple[i] = e->polygon_stipple[i];
    }
    if (mask & GL_ACCUM_BUFFER_BIT) {
        for (int i = 0; i < 4; i++)
            ctx->accum_clear[i] = e->accum_clear[i];
    }
    /* The evaluator enables belong to GL_EVAL_BIT as well as GL_ENABLE_BIT; the grid to
     * GL_EVAL_BIT alone. The maps themselves are in no attribute group. */
    if (all_enables || (mask & GL_EVAL_BIT)) {
        for (int i = 0; i < OOPS_GL_EVAL_MAPS; i++) {
            ctx->cap_map1[i] = e->cap_map1[i];
            ctx->cap_map2[i] = e->cap_map2[i];
        }
        ctx->cap_auto_normal = e->cap_auto_normal;
    }
    /* The transfer state and the zoom; the read buffer this group also names is the one
     * surface there is, so it has nothing to restore. */
    if (mask & GL_PIXEL_MODE_BIT) {
        for (int i = 0; i < 4; i++) {
            ctx->pixel_scale[i] = e->pixel_scale[i];
            ctx->pixel_bias[i] = e->pixel_bias[i];
        }
        ctx->depth_scale = e->depth_scale;
        ctx->depth_bias = e->depth_bias;
        ctx->index_shift = e->index_shift;
        ctx->index_offset = e->index_offset;
        ctx->map_color = e->map_color;
        ctx->map_stencil = e->map_stencil;
        ctx->pixel_zoom_x = e->pixel_zoom_x;
        ctx->read_buffer = e->read_buffer;
        ctx->pixel_zoom_y = e->pixel_zoom_y;
    }
    if (mask & GL_EVAL_BIT) {
        ctx->grid1_un = e->grid1_un;
        ctx->grid1_u1 = e->grid1_u1;
        ctx->grid1_u2 = e->grid1_u2;
        ctx->grid2_un = e->grid2_un;
        ctx->grid2_vn = e->grid2_vn;
        ctx->grid2_u1 = e->grid2_u1;
        ctx->grid2_u2 = e->grid2_u2;
        ctx->grid2_v1 = e->grid2_v1;
        ctx->grid2_v2 = e->grid2_v2;
    }
    if (mask & GL_HINT_BIT) {
        ctx->perspective_hint = e->perspective_hint;
        ctx->hint_point_smooth = e->hint_point_smooth;
        ctx->hint_line_smooth = e->hint_line_smooth;
        ctx->hint_polygon_smooth = e->hint_polygon_smooth;
        ctx->hint_fog = e->hint_fog;
        ctx->hint_texture_compression = e->hint_texture_compression;
        ctx->hint_generate_mipmap = e->hint_generate_mipmap;
    }
    if (all_enables || (mask & GL_SCISSOR_BIT)) {
        ctx->cap_scissor_test = e->cap_scissor_test;
        ctx->hw_scissor_dirty = GL_TRUE;
    }
    if (all_enables || (mask & GL_STENCIL_BUFFER_BIT)) {
        ctx->cap_stencil_test = e->cap_stencil_test;
    }
    /* The fog enable belongs to both GL_ENABLE_BIT and GL_FOG_BIT, like every other
     * feature's enable here - so a pop of GL_FOG_BIT alone brings fog back on, not just
     * its parameters. */
    if (all_enables || (mask & GL_FOG_BIT)) {
        ctx->cap_fog = e->cap_fog;
        /* GL_COLOR_SUM too: the GL 1.4 state table puts it in the fog and enable
         * groups (Mesa main/enable.c:1084-1085). Mesa saves it but does not restore it
         * (main/attrib.c:858-866); this restores it. */
        ctx->cap_color_sum = e->cap_color_sum;
    }
    if (mask & GL_FOG_BIT) {
        ctx->fog_mode = e->fog_mode;
        ctx->fog_density = e->fog_density;
        ctx->fog_start = e->fog_start;
        ctx->fog_end = e->fog_end;
        for (int i = 0; i < 4; i++)
            ctx->fog_color[i] = e->fog_color[i];
        ctx->fog_index = e->fog_index;
        /* GL_FOG_COORD_SRC is the fog group's as well; Mesa's pop leaves it out with
         * GL_COLOR_SUM (main/attrib.c:858-866). */
        ctx->fog_coord_src = e->fog_coord_src;
    }
    if (mask & GL_STENCIL_BUFFER_BIT) {
        ctx->stencil_func = e->stencil_func;
        ctx->stencil_ref = e->stencil_ref;
        ctx->stencil_value_mask = e->stencil_value_mask;
        ctx->stencil_writemask = e->stencil_writemask;
        ctx->stencil_fail = e->stencil_fail;
        ctx->stencil_zfail = e->stencil_zfail;
        ctx->stencil_zpass = e->stencil_zpass;
        ctx->stencil_back_func = e->stencil_back_func;
        ctx->stencil_back_ref = e->stencil_back_ref;
        ctx->stencil_back_value_mask = e->stencil_back_value_mask;
        ctx->stencil_back_writemask = e->stencil_back_writemask;
        ctx->stencil_back_fail = e->stencil_back_fail;
        ctx->stencil_back_zfail = e->stencil_back_zfail;
        ctx->stencil_back_zpass = e->stencil_back_zpass;
        ctx->clear_stencil = e->clear_stencil;
    }
    if (all_enables || (mask & GL_LIGHTING_BIT)) {
        ctx->cap_lighting = e->cap_lighting;
        ctx->cap_color_material = e->cap_color_material;
    }
    if (all_enables || (mask & GL_TEXTURE_BIT)) {
        /* Every unit's target enables and generation enables - the texture group's and
         * the enable group's both (GL 1.3, table 6.20: "texture/enable"), which is
         * where Mesa saves them (main/attrib.c:189-192, restored :486-510). */
        for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
            gl_tex_unit_t *tu = &ctx->tex_unit[u];
            const gl_tex_unit_t *s = &e->tex_units[u];
            tu->cap_texture_1d = s->cap_texture_1d;
            tu->cap_texture_2d = s->cap_texture_2d;
            tu->cap_texture_3d = s->cap_texture_3d;
            tu->cap_texture_cube_map = s->cap_texture_cube_map;
            for (int i = 0; i < 4; i++)
                tu->texgen_enabled[i] = s->texgen_enabled[i];
        }
    }
    if (all_enables || (mask & GL_TRANSFORM_BIT)) {
        ctx->cap_normalize = e->cap_normalize;
        ctx->cap_rescale_normal = e->cap_rescale_normal;
        /* The clip planes are already in eye space, so they restore as stored - putting
         * them back through a modelview inverse would transform them a second time. */
        for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
            ctx->clip_plane_enabled[i] = e->clip_plane_enabled[i];
            for (int k = 0; k < 4; k++)
                ctx->clip_plane[i][k] = e->clip_plane[i][k];
        }
        ctx->hw_clip_dirty = GL_TRUE;
    }

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
        ctx->blend_equation_alpha = e->blend_equation_alpha;
        for (int i = 0; i < 4; i++)
            ctx->blend_color[i] = e->blend_color[i];
        ctx->hw_blend_color_dirty = GL_TRUE;
        ctx->logic_op = e->logic_op;
        ctx->hw_color_control_dirty = GL_TRUE;
        for (int i = 0; i < 4; i++)
            ctx->color_mask[i] = e->color_mask[i];
        ctx->draw_buffer = e->draw_buffer;
        gl_draw_targets(ctx); /* a front it names was allocated when it was first set */
        for (int i = 0; i < 4; i++)
            ctx->clear_color[i] = e->clear_color[i];
        ctx->alpha_func = e->alpha_func;
        ctx->alpha_ref = e->alpha_ref;
        ctx->clear_index = e->clear_index;
        ctx->index_mask = e->index_mask;
    }

    if (mask & GL_POLYGON_BIT) {
        ctx->cull_mode = e->cull_mode;
        ctx->front_face = e->front_face;
        ctx->polygon_offset_factor = e->polygon_offset_factor;
        ctx->polygon_offset_units = e->polygon_offset_units;
        ctx->polygon_mode[0] = e->polygon_mode[0];
        ctx->polygon_mode[1] = e->polygon_mode[1];
    }

    if (mask & GL_LIGHTING_BIT) {
        ctx->shade_model = e->shade_model;
        for (int i = 0; i < OOPS_GL_LIGHT_COUNT; i++)
            ctx->lights[i] = e->lights[i];
        ctx->mat_front = e->mat_front;
        ctx->mat_back = e->mat_back;
        for (int i = 0; i < 4; i++)
            ctx->light_model_ambient[i] = e->light_model_ambient[i];
        /* The rest of the light model. */
        ctx->light_model_local_viewer = e->light_model_local_viewer;
        ctx->light_model_color_control = e->light_model_color_control;
        ctx->light_model_two_side = e->light_model_two_side;
        /* glColorMaterial's face and property are GL_LIGHTING_BIT's too. */
        ctx->color_material_face = e->color_material_face;
        ctx->color_material_mode = e->color_material_mode;
    }

    if (mask & GL_TEXTURE_BIT) {
        for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
            gl_tex_unit_t *tu = &ctx->tex_unit[u];
            const gl_tex_unit_t *s = &e->tex_units[u];
            tu->bound_texture_1d = s->bound_texture_1d;
            tu->bound_texture_2d = s->bound_texture_2d;
            tu->bound_texture_3d = s->bound_texture_3d;
            tu->bound_texture_cube = s->bound_texture_cube;
            /* GL_COORD_REPLACE is per unit and part of the texture environment, so it
               belongs to this group rather than to GL_POINT_BIT. */
            tu->coord_replace = s->coord_replace;
            /* The bound textures' own parameters come back too, as Mesa restores them.
             * A texture deleted in between is not brought back. */
            for (int i = 0; i < 4; i++) {
                if (e->tex_param_id[u][i] == 0u)
                    continue;
                gl_texture_object_t *t = (gl_texture_object_t *)0;
                for (int k = 0; k < OOPS_GL_MAX_TEXTURE_OBJECTS; k++) {
                    if (ctx->textures[k].used &&
                        ctx->textures[k].id == e->tex_param_id[u][i]) {
                        t = &ctx->textures[k];
                        break;
                    }
                }
                if (!t)
                    continue;
                t->wrap_s = e->tex_params[u][i].wrap_s;
                t->wrap_t = e->tex_params[u][i].wrap_t;
                t->wrap_r = e->tex_params[u][i].wrap_r;
                t->min_filter = e->tex_params[u][i].min_filter;
                t->mag_filter = e->tex_params[u][i].mag_filter;
                for (int k = 0; k < 4; k++)
                    t->border_color[k] = e->tex_params[u][i].border_color[k];
                t->priority = e->tex_params[u][i].priority;
                t->base_level = e->tex_params[u][i].base_level;
                t->max_level = e->tex_params[u][i].max_level;
                t->min_lod = e->tex_params[u][i].min_lod;
                t->max_lod = e->tex_params[u][i].max_lod;
                t->lod_bias = e->tex_params[u][i].lod_bias;
                t->generate_mipmap = e->tex_params[u][i].generate_mipmap;
                t->compare_mode = e->tex_params[u][i].compare_mode;
                t->compare_func = e->tex_params[u][i].compare_func;
                t->depth_mode = e->tex_params[u][i].depth_mode;
                gl_tex_repack(t);
            }
            tu->tex_env_mode = s->tex_env_mode;
            tu->tex_lod_bias = s->tex_lod_bias;
            tu->combine = s->combine;
            for (int i = 0; i < 4; i++)
                tu->tex_env_color[i] = s->tex_env_color[i];
            /* The generation planes are already in eye space, so they restore as stored
             * - putting them back through a modelview inverse here would transform them
             * a second time. The texture matrix stack is in no attribute group and
             * stays as it is. */
            for (int i = 0; i < 4; i++) {
                tu->texgen_mode[i] = s->texgen_mode[i];
                for (int k = 0; k < 4; k++) {
                    tu->texgen_object_plane[i][k] = s->texgen_object_plane[i][k];
                    tu->texgen_eye_plane[i][k] = s->texgen_eye_plane[i][k];
                }
            }
        }
        ctx->active_texture = e->active_texture;
        gl_ps_patch_tex_env(ctx);
    }

    if (mask & GL_VIEWPORT_BIT) {
        ctx->vp_x = e->vp_x;
        ctx->vp_y = e->vp_y;
        ctx->vp_w = e->vp_w;
        ctx->vp_h = e->vp_h;
        ctx->depth_near = e->depth_near;
        ctx->depth_far = e->depth_far;
        /* Flagged so a pop inside a frame reaches the next draw. */
        ctx->hw_vport_dirty = GL_TRUE;
        ctx->hw_depth_range_dirty = GL_TRUE;
    }

    if (mask & GL_SCISSOR_BIT) {
        ctx->sc_x = e->sc_x;
        ctx->sc_y = e->sc_y;
        ctx->sc_w = e->sc_w;
        ctx->sc_h = e->sc_h;
        ctx->hw_scissor_dirty = GL_TRUE;
    }

    if (mask & GL_TRANSFORM_BIT)
        ctx->matrix_mode = e->matrix_mode;
    if (mask & GL_LIST_BIT)
        ctx->list_base = e->list_base;

    /* The alpha test lives in the shader, not in a register the next frame re-emits, so
     * a restore has to rewrite it. Everything else above is picked up from the context
     * when the next frame's register table is built. */
    if (mask & (GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT)) {
        gl_ps_patch_alpha_test(ctx);
    }
}

/* -------------------------------------------------------------------------
 * The client attribute stack
 *
 * Two groups: the array pointers and the pixel-store modes. A separate stack, because
 * the specification makes client and server pushes independent.
 * ------------------------------------------------------------------------- */

#define GL_CLIENT_ATTRIB_SUPPORTED                                                     \
    (GL_CLIENT_PIXEL_STORE_BIT | GL_CLIENT_VERTEX_ARRAY_BIT)

void glPushClientAttrib(GLbitfield mask) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;

    if (mask == GL_CLIENT_ALL_ATTRIB_BITS) {
        mask = GL_CLIENT_ATTRIB_SUPPORTED;
    }
    /* A bit outside the two the specification defines names nothing. */
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
    memcpy(e->array_texcoord, ctx->array_texcoord,
           sizeof(e->array_texcoord)); /* every unit's */
    e->client_active_texture = ctx->client_active_texture;
    e->array_edge_flag = ctx->array_edge_flag;
    e->array_index = ctx->array_index;
    e->array_secondary = ctx->array_secondary;
    e->array_fog_coord = ctx->array_fog_coord;

    e->unpack_alignment = ctx->unpack_alignment;
    e->unpack_row_length = ctx->unpack_row_length;
    e->pack_alignment = ctx->pack_alignment;
    e->unpack_image_height = ctx->unpack_image_height;
    e->pack_image_height = ctx->pack_image_height;
    e->unpack_skip_rows = ctx->unpack_skip_rows;
    e->unpack_skip_pixels = ctx->unpack_skip_pixels;
    e->unpack_skip_images = ctx->unpack_skip_images;
    e->unpack_swap_bytes = ctx->unpack_swap_bytes;
    e->unpack_lsb_first = ctx->unpack_lsb_first;
    e->pack_row_length = ctx->pack_row_length;
    e->pack_skip_rows = ctx->pack_skip_rows;
    e->pack_skip_pixels = ctx->pack_skip_pixels;
    e->pack_skip_images = ctx->pack_skip_images;
    e->pack_swap_bytes = ctx->pack_swap_bytes;
    e->pack_lsb_first = ctx->pack_lsb_first;
}

void glPopClientAttrib(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (ctx->client_attrib_depth == 0u) {
        gl_record_error(ctx, GL_STACK_UNDERFLOW);
        return;
    }

    const gl_client_attrib_entry_t *e =
        &ctx->client_attrib_stack[--ctx->client_attrib_depth];

    /* The enable travels with the pointer: GL_CLIENT_VERTEX_ARRAY_BIT covers both, and
     * `gl_client_array_t` holds both, so copying the struct keeps them together. */
    if (e->mask & GL_CLIENT_VERTEX_ARRAY_BIT) {
        ctx->array_vertex = e->array_vertex;
        ctx->array_color = e->array_color;
        ctx->array_normal = e->array_normal;
        memcpy(ctx->array_texcoord, e->array_texcoord, sizeof(ctx->array_texcoord));
        ctx->client_active_texture = e->client_active_texture;
        ctx->array_edge_flag = e->array_edge_flag;
        ctx->array_index = e->array_index;
        ctx->array_secondary = e->array_secondary;
        ctx->array_fog_coord = e->array_fog_coord;
    }

    if (e->mask & GL_CLIENT_PIXEL_STORE_BIT) {
        ctx->unpack_alignment = e->unpack_alignment;
        ctx->unpack_row_length = e->unpack_row_length;
        ctx->pack_alignment = e->pack_alignment;
        ctx->unpack_image_height = e->unpack_image_height;
        ctx->pack_image_height = e->pack_image_height;
        ctx->unpack_skip_rows = e->unpack_skip_rows;
        ctx->unpack_skip_pixels = e->unpack_skip_pixels;
        ctx->unpack_skip_images = e->unpack_skip_images;
        ctx->unpack_swap_bytes = e->unpack_swap_bytes;
        ctx->unpack_lsb_first = e->unpack_lsb_first;
        ctx->pack_row_length = e->pack_row_length;
        ctx->pack_skip_rows = e->pack_skip_rows;
        ctx->pack_skip_pixels = e->pack_skip_pixels;
        ctx->pack_skip_images = e->pack_skip_images;
        ctx->pack_swap_bytes = e->pack_swap_bytes;
        ctx->pack_lsb_first = e->pack_lsb_first;
    }
}
