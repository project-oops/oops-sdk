/*
 * oops-gl: selection and feedback
 *
 * GL has three render modes. GL_RENDER draws. In GL_SELECT and GL_FEEDBACK every
 * primitive goes through the same transform, lighting and clipping, and then **is
 * reported instead of drawn**:
 *
 * - GL_SELECT keeps a stack of names the program pushes around each object, and records
 * a *hit*
 *   - the names on the stack, with the nearest and farthest window depth - whenever a
 * primitive survives clipping and culling. Drawn again under a pick matrix that shrinks
 * the view to a few pixels around the cursor, the scene's hits are what is under the
 * mouse. That is how a great many GL 1.x programs pick.
 * - GL_FEEDBACK writes each primitive as a token and its clipped vertices - window
 * coordinates, and optionally colour and texture coordinate - into a float buffer. It
 * is how a program turns what it would draw into vectors: PostScript and PDF export are
 * built on it.
 *
 * Nothing reaches the framebuffer in either mode, glClear included (Mesa
 * main/clear.c:185).
 *
 * # Where this sits
 *
 * Primitive assembly (gl_assemble) calls in here instead of the rasteriser, per
 * triangle of a polygon and per line and point **before** a line or point is widened
 * into the triangles that draw it. So the geometry reported is GL's - a line is two
 * endpoints, not a quad - and the one path serves glBegin/glEnd, the vertex arrays,
 * display lists and evaluators alike.
 *
 * # Clipping is real here
 *
 * The drawing path does not clip geometrically: it interpolates clip distances and
 * rejects per fragment, which gives the same pixels. Selection and feedback have no
 * pixels - they report geometry, and a hit's depth range and a feedback polygon's
 * vertices are those *after clipping*. So this clips properly, in homogeneous clip
 * space against the six view-volume planes and every enabled user plane (in eye space),
 * carrying the colour and texture coordinate along: a polygon with Sutherland-Hodgman,
 * a line by its entry and exit parameters.
 *
 * # What follows Mesa, and what cannot
 *
 * The semantics are Mesa's main/feedback.c: the hit record's layout and its depths
 * scaled to 2^32-1, the counts that run on past a full buffer so glRenderMode can
 * answer -1, the name-stack calls ignored outside GL_SELECT, and a triangle at a time
 * reported as GL_POLYGON_TOKEN (Mesa's state_tracker/st_cb_feedback.c does the same).
 * Two things differ, both from how vertices are kept here: a feedback texture
 * coordinate is (s/q, t/q, 0, 1) - the vertex carries its coordinate already divided,
 * which is the same point projectively and loses only r, which no texture here reads -
 * and the pixel-rectangle tokens report the raster colour and coordinate as glRasterPos
 * latched them.
 */

#include "gl_internal.h"

/* -------------------------------------------------------------------------
 * The render mode and its buffers
 * ------------------------------------------------------------------------- */

/* One hit record, if there is a hit to record: the name-stack depth, the depth range,
 * then the names bottom to top. Written as far as the buffer goes and counted in full.
 * Depths are window z in 0..1 scaled to 2^32-1 and rounded, as the specification says;
 * Mesa multiplies in float, where 1.0 * 4294967295.0f rounds up to 2^32 and does not
 * fit, so this multiplies in double. */
static void gl_select_put(gl_context_t *ctx, GLuint value) {
    if (ctx->select_count < (GLuint)ctx->select_size && ctx->select_buffer) {
        ctx->select_buffer[ctx->select_count] = value;
    }
    ctx->select_count++;
}

static GLuint gl_select_depth(float z) {
    if (!(z > 0.0f))
        return 0u;
    if (z >= 1.0f)
        return 0xffffffffu;
    return (GLuint)((double)z * 4294967295.0 + 0.5);
}

static void gl_select_flush_hit(gl_context_t *ctx) {
    if (!ctx->hit_flag)
        return;
    gl_select_put(ctx, ctx->name_depth);
    gl_select_put(ctx, gl_select_depth(ctx->hit_min_z));
    gl_select_put(ctx, gl_select_depth(ctx->hit_max_z));
    for (GLuint i = 0; i < ctx->name_depth; i++)
        gl_select_put(ctx, ctx->name_stack[i]);
    ctx->select_hits++;
    ctx->hit_flag = GL_FALSE;
    ctx->hit_min_z = 1.0f;
    ctx->hit_max_z = 0.0f;
}

static void gl_select_hit(gl_context_t *ctx, float z) {
    ctx->hit_flag = GL_TRUE;
    if (z < ctx->hit_min_z)
        ctx->hit_min_z = z;
    if (z > ctx->hit_max_z)
        ctx->hit_max_z = z;
}

static void gl_fb_put(gl_context_t *ctx, float value) {
    if (ctx->feedback_count < (GLuint)ctx->feedback_size && ctx->feedback_buffer) {
        ctx->feedback_buffer[ctx->feedback_count] = value;
    }
    ctx->feedback_count++;
}

/* glRenderMode answers for the mode being left: the hit count, the number of feedback
 * values, or 0 from GL_RENDER - and -1 for a buffer that overflowed. **A refused call
 * changes nothing**: the new mode is checked before the old one is left, so GL_SELECT
 * without a selection buffer is GL_INVALID_OPERATION and the program stays where it
 * was. (Mesa raises the error and switches anyway, main/feedback.c _mesa_RenderMode.)
 */
GLint glRenderMode(GLenum mode) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return 0;
    if (ctx->imm_active) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return 0;
    }
    if (mode != GL_RENDER && mode != GL_SELECT && mode != GL_FEEDBACK) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return 0;
    }
    if ((mode == GL_SELECT && !ctx->select_buffer_set) ||
        (mode == GL_FEEDBACK && !ctx->feedback_buffer_set)) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return 0;
    }

    GLint result = 0;
    if (ctx->render_mode == GL_SELECT) {
        gl_select_flush_hit(ctx);
        result = (ctx->select_count > (GLuint)ctx->select_size)
                     ? -1
                     : (GLint)ctx->select_hits;
        ctx->select_count = 0u;
        ctx->select_hits = 0u;
        ctx->name_depth = 0u;
    } else if (ctx->render_mode == GL_FEEDBACK) {
        result = (ctx->feedback_count > (GLuint)ctx->feedback_size)
                     ? -1
                     : (GLint)ctx->feedback_count;
        ctx->feedback_count = 0u;
    }

    /* Entering a mode starts it empty. */
    ctx->hit_flag = GL_FALSE;
    ctx->hit_min_z = 1.0f;
    ctx->hit_max_z = 0.0f;
    ctx->name_depth = 0u;
    ctx->select_count = 0u;
    ctx->select_hits = 0u;
    ctx->feedback_count = 0u;
    ctx->render_mode = mode;
    return result;
}

void glSelectBuffer(GLsizei size, GLuint *buffer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (size < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (ctx->render_mode == GL_SELECT) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    ctx->select_buffer = buffer;
    ctx->select_size = buffer ? size : 0;
    ctx->select_buffer_set = GL_TRUE;
    ctx->select_count = 0u;
}

void glFeedbackBuffer(GLsizei size, GLenum type, GLfloat *buffer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (ctx->render_mode == GL_FEEDBACK) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (size < 0 || (!buffer && size > 0)) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    switch (type) {
    case GL_2D:
    case GL_3D:
    case GL_3D_COLOR:
    case GL_3D_COLOR_TEXTURE:
    case GL_4D_COLOR_TEXTURE:
        break;
    default:
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    ctx->feedback_buffer = buffer;
    ctx->feedback_size = size;
    ctx->feedback_type = type;
    ctx->feedback_buffer_set = GL_TRUE;
    ctx->feedback_count = 0u;
}

/* -------------------------------------------------------------------------
 * The name stack. Compiled into lists; ignored outside GL_SELECT, as Mesa ignores them.
 * Every change to the stack first records the hit the old stack has earned, if it has
 * one.
 * ------------------------------------------------------------------------- */

void glInitNames(void) {
    if (gl_list_recording() && GL_LIST_REC0(GL_LIST_OP_INIT_NAMES))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || ctx->render_mode != GL_SELECT)
        return;
    gl_select_flush_hit(ctx);
    ctx->name_depth = 0u;
}

void glLoadName(GLuint name) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_LOAD_NAME, gl_la_u(name)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || ctx->render_mode != GL_SELECT)
        return;
    if (ctx->name_depth == 0u) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    gl_select_flush_hit(ctx);
    ctx->name_stack[ctx->name_depth - 1u] = name;
}

void glPushName(GLuint name) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_PUSH_NAME, gl_la_u(name)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || ctx->render_mode != GL_SELECT)
        return;
    if (ctx->name_depth >= OOPS_GL_MAX_NAME_STACK_DEPTH) {
        gl_record_error(ctx, GL_STACK_OVERFLOW);
        return;
    }
    gl_select_flush_hit(ctx);
    ctx->name_stack[ctx->name_depth++] = name;
}

void glPopName(void) {
    if (gl_list_recording() && GL_LIST_REC0(GL_LIST_OP_POP_NAME))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || ctx->render_mode != GL_SELECT)
        return;
    if (ctx->name_depth == 0u) {
        gl_record_error(ctx, GL_STACK_UNDERFLOW);
        return;
    }
    gl_select_flush_hit(ctx);
    ctx->name_depth--;
}

/* A marker a program drops into the feedback stream - a label for what follows. */
void glPassThrough(GLfloat token) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_PASS_THROUGH, gl_la_f(token)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || ctx->render_mode != GL_FEEDBACK)
        return;
    gl_fb_put(ctx, (float)GL_PASS_THROUGH_TOKEN);
    gl_fb_put(ctx, token);
}

/* -------------------------------------------------------------------------
 * Vertices, clipping, and the window
 * ------------------------------------------------------------------------- */

/* A vertex as selection and feedback need it: clip coordinates to clip in, eye
 * coordinates for the user planes, and the colour and texture coordinate to report. All
 * four interpolate linearly along an edge in clip space, which is what makes clipping
 * them one lerp. */
typedef struct {
    float clip[4];
    float eye[4];
    float col[4];
    float tc[4];
} gl_fb_vtx_t;

/* Three corners, and at most one more per plane: six view-volume planes and six user
 * planes. */
#define GL_FB_MAX_POLY (3 + 6 + OOPS_GL_CLIP_PLANE_COUNT)

/* The colour a vertex is reported in: lit if lighting is on, exactly as the drawing
 * path lights it (gl_compute_lighting). */
static void gl_fb_colour(gl_context_t *ctx, const gl_vertex_t *v, float out[4]) {
    const float in[4] = {v->r, v->g, v->b, v->a};
    if (ctx->cap_lighting) {
        const float obj[4] = {v->x, v->y, v->z, v->w};
        const float n[3] = {v->nx, v->ny, v->nz};
        gl_compute_lighting(ctx, obj, n, in, out);
    } else {
        for (int i = 0; i < 4; i++)
            out[i] = in[i];
    }
}

/* `flat` is the provoking vertex's colour under GL_FLAT, or NULL for the vertex's own.
 */
static void gl_fb_vertex(gl_context_t *ctx, const gl_vertex_t *v, const float *flat,
                         gl_fb_vtx_t *out) {
    const float obj[4] = {v->x, v->y, v->z, v->w};
    mat4_transform_vec4(out->clip, &ctx->mvp, obj);
    mat4_transform_vec4(out->eye, &ctx->modelview_stack[ctx->modelview_depth], obj);
    if (flat) {
        for (int i = 0; i < 4; i++)
            out->col[i] = flat[i];
    } else {
        gl_fb_colour(ctx, v, out->col);
    }
    /* The vertex's texture coordinate, all four and undivided - what GL's feedback
     * reports. This was s/q and t/q with r 0 and q 1 until the rasteriser took the
     * divide over (2026-09-19). Unit 0's, as Mesa's feedback takes it
     * (state_tracker/st_cb_feedback.c:114-118). */
    for (int i = 0; i < 4; i++)
        out->tc[i] = v->tc[0][i];
}

/* The provoking colour for this primitive, into `buf`, or NULL under smooth shading. */
static const float *gl_fb_flat(gl_context_t *ctx, const gl_vertex_t *pv, float buf[4]) {
    if (ctx->shade_model != GL_FLAT || !pv)
        return (const float *)0;
    gl_fb_colour(ctx, pv, buf);
    return buf;
}

/* The planes to clip against: 0..5 the view volume (-w <= x, y, z <= w), 6 + i user
 * plane i. */
static int gl_fb_planes(const gl_context_t *ctx,
                        int ids[6 + OOPS_GL_CLIP_PLANE_COUNT]) {
    int n = 0;
    for (int p = 0; p < 6; p++)
        ids[n++] = p;
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        if (ctx->clip_plane_enabled[i])
            ids[n++] = 6 + i;
    }
    return n;
}

/* A vertex's distance inside a plane: nonnegative is kept. */
static float gl_fb_dist(const gl_context_t *ctx, int plane, const gl_fb_vtx_t *v) {
    const float *c = v->clip;
    switch (plane) {
    case 0:
        return c[3] + c[0];
    case 1:
        return c[3] - c[0];
    case 2:
        return c[3] + c[1];
    case 3:
        return c[3] - c[1];
    case 4:
        return c[3] + c[2];
    case 5:
        return c[3] - c[2];
    default: {
        const float *p = ctx->clip_plane[plane - 6];
        return p[0] * v->eye[0] + p[1] * v->eye[1] + p[2] * v->eye[2] +
               p[3] * v->eye[3];
    }
    }
}

static void gl_fb_lerp(const gl_fb_vtx_t *a, const gl_fb_vtx_t *b, float t,
                       gl_fb_vtx_t *out) {
    for (int i = 0; i < 4; i++) {
        out->clip[i] = a->clip[i] + t * (b->clip[i] - a->clip[i]);
        out->eye[i] = a->eye[i] + t * (b->eye[i] - a->eye[i]);
        out->col[i] = a->col[i] + t * (b->col[i] - a->col[i]);
        out->tc[i] = a->tc[i] + t * (b->tc[i] - a->tc[i]);
    }
}

/* Sutherland-Hodgman, one plane at a time. Answers the clipped polygon's vertex count
 * in `out`, fewer than 3 when nothing is left. */
static int gl_fb_clip_polygon(const gl_context_t *ctx, const gl_fb_vtx_t *in, int n,
                              gl_fb_vtx_t *out) {
    gl_fb_vtx_t bufs[2][GL_FB_MAX_POLY];
    int planes[6 + OOPS_GL_CLIP_PLANE_COUNT];
    const int np = gl_fb_planes(ctx, planes);
    for (int i = 0; i < n; i++)
        bufs[0][i] = in[i];
    int cur = 0;
    for (int k = 0; k < np && n > 0; k++) {
        const gl_fb_vtx_t *src = bufs[cur];
        gl_fb_vtx_t *dst = bufs[cur ^ 1];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const gl_fb_vtx_t *a = &src[i];
            const gl_fb_vtx_t *b = &src[(i + 1) % n];
            const float da = gl_fb_dist(ctx, planes[k], a);
            const float db = gl_fb_dist(ctx, planes[k], b);
            if (da >= 0.0f && m < GL_FB_MAX_POLY)
                dst[m++] = *a;
            if ((da >= 0.0f) != (db >= 0.0f) && m < GL_FB_MAX_POLY) {
                gl_fb_lerp(a, b, da / (da - db), &dst[m++]);
            }
        }
        n = m;
        cur ^= 1;
    }
    for (int i = 0; i < n; i++)
        out[i] = bufs[cur][i];
    return n;
}

/* A segment's entry and exit parameters across every plane; false when none of it is
 * inside. */
static GLboolean gl_fb_clip_line(const gl_context_t *ctx, gl_fb_vtx_t *a,
                                 gl_fb_vtx_t *b) {
    int planes[6 + OOPS_GL_CLIP_PLANE_COUNT];
    const int np = gl_fb_planes(ctx, planes);
    float t0 = 0.0f, t1 = 1.0f;
    for (int k = 0; k < np; k++) {
        const float da = gl_fb_dist(ctx, planes[k], a);
        const float db = gl_fb_dist(ctx, planes[k], b);
        if (da < 0.0f && db < 0.0f)
            return GL_FALSE;
        if (da < 0.0f) {
            const float t = da / (da - db);
            if (t > t0)
                t0 = t;
        } else if (db < 0.0f) {
            const float t = da / (da - db);
            if (t < t1)
                t1 = t;
        }
        if (t0 > t1)
            return GL_FALSE;
    }
    const gl_fb_vtx_t oa = *a, ob = *b;
    if (t0 > 0.0f)
        gl_fb_lerp(&oa, &ob, t0, a);
    if (t1 < 1.0f)
        gl_fb_lerp(&oa, &ob, t1, b);
    return GL_TRUE;
}

static GLboolean gl_fb_inside(const gl_context_t *ctx, const gl_fb_vtx_t *v) {
    int planes[6 + OOPS_GL_CLIP_PLANE_COUNT];
    const int np = gl_fb_planes(ctx, planes);
    for (int k = 0; k < np; k++) {
        if (gl_fb_dist(ctx, planes[k], v) < 0.0f)
            return GL_FALSE;
    }
    return GL_TRUE;
}

/* Window coordinates, GL's way up - origin bottom-left, as a feedback buffer reports
 * them - with z through the depth range and w the clip w (Mesa st_cb_feedback.c
 * feedback_vertex). */
static void gl_fb_window(const gl_context_t *ctx, const gl_fb_vtx_t *v, float win[4]) {
    const float w = v->clip[3];
    const float inv = (w != 0.0f) ? 1.0f / w : 0.0f;
    win[0] = (float)ctx->vp_x + (v->clip[0] * inv + 1.0f) * 0.5f * (float)ctx->vp_w;
    win[1] = (float)ctx->vp_y + (v->clip[1] * inv + 1.0f) * 0.5f * (float)ctx->vp_h;
    win[2] = v->clip[2] * inv * (ctx->depth_far - ctx->depth_near) * 0.5f +
             (ctx->depth_far + ctx->depth_near) * 0.5f;
    win[3] = w;
}

/* One vertex into the feedback buffer, in the layout glFeedbackBuffer's type names. */
static void gl_fb_put_vertex(gl_context_t *ctx, const float win[4], const float col[4],
                             const float tc[4]) {
    const GLenum type = ctx->feedback_type;
    gl_fb_put(ctx, win[0]);
    gl_fb_put(ctx, win[1]);
    if (type != GL_2D)
        gl_fb_put(ctx, win[2]);
    if (type == GL_4D_COLOR_TEXTURE)
        gl_fb_put(ctx, win[3]);
    if (type == GL_3D_COLOR || type == GL_3D_COLOR_TEXTURE ||
        type == GL_4D_COLOR_TEXTURE) {
        for (int i = 0; i < 4; i++)
            gl_fb_put(ctx, col[i]);
    }
    if (type == GL_3D_COLOR_TEXTURE || type == GL_4D_COLOR_TEXTURE) {
        for (int i = 0; i < 4; i++)
            gl_fb_put(ctx, tc[i]);
    }
}

/* A clipped vertex reported: a hit at its depth, or its values into the feedback
 * buffer. */
static void gl_fb_report(gl_context_t *ctx, const gl_fb_vtx_t *v) {
    float win[4];
    gl_fb_window(ctx, v, win);
    if (ctx->render_mode == GL_SELECT) {
        gl_select_hit(ctx, win[2]);
    } else {
        gl_fb_put_vertex(ctx, win, v->col, v->tc);
    }
}

/* -------------------------------------------------------------------------
 * The primitives
 * ------------------------------------------------------------------------- */

static void gl_fb_line_coloured(gl_context_t *ctx, const gl_vertex_t *a,
                                const gl_vertex_t *b, const float *flat) {
    gl_fb_vtx_t fa, fb;
    gl_fb_vertex(ctx, a, flat, &fa);
    gl_fb_vertex(ctx, b, flat, &fb);
    if (!gl_fb_clip_line(ctx, &fa, &fb))
        return;
    if (ctx->render_mode == GL_FEEDBACK) {
        gl_fb_put(ctx,
                  (float)(ctx->fb_line_reset ? GL_LINE_RESET_TOKEN : GL_LINE_TOKEN));
        ctx->fb_line_reset = GL_FALSE;
    }
    gl_fb_report(ctx, &fa);
    gl_fb_report(ctx, &fb);
}

static void gl_fb_point_coloured(gl_context_t *ctx, const gl_vertex_t *p,
                                 const float *flat) {
    gl_fb_vtx_t fp;
    gl_fb_vertex(ctx, p, flat, &fp);
    if (!gl_fb_inside(ctx, &fp))
        return;
    if (ctx->render_mode == GL_FEEDBACK)
        gl_fb_put(ctx, (float)GL_POINT_TOKEN);
    gl_fb_report(ctx, &fp);
}

void gl_fb_line(gl_context_t *ctx, const gl_vertex_t *a, const gl_vertex_t *b,
                const gl_vertex_t *pv) {
    gl_update_mvp(ctx);
    float buf[4];
    gl_fb_line_coloured(ctx, a, b, gl_fb_flat(ctx, pv, buf));
}

void gl_fb_point(gl_context_t *ctx, const gl_vertex_t *p) {
    gl_update_mvp(ctx);
    gl_fb_point_coloured(ctx, p, (const float *)0);
}

/* One triangle of a polygon primitive. Clipped first, because the facing is decided on
 * what is left of it in window space and a triangle wholly outside is no hit at all;
 * then culled, since
 * **a culled polygon is no hit** either; then reported the way glPolygonMode says for
 * its face - as a polygon, as its boundary edges, or as the vertices that start them.
 */
void gl_fb_polygon_tri(gl_context_t *ctx, const gl_vertex_t *v0, const gl_vertex_t *v1,
                       const gl_vertex_t *v2, unsigned edges, GLboolean use_flags,
                       const gl_vertex_t *pv) {
    gl_update_mvp(ctx);
    float buf[4];
    const float *flat = gl_fb_flat(ctx, pv, buf);
    gl_fb_vtx_t tri[3];
    gl_fb_vertex(ctx, v0, flat, &tri[0]);
    gl_fb_vertex(ctx, v1, flat, &tri[1]);
    gl_fb_vertex(ctx, v2, flat, &tri[2]);
    gl_fb_vtx_t poly[GL_FB_MAX_POLY];
    const int n = gl_fb_clip_polygon(ctx, tri, 3, poly);
    if (n < 3)
        return;

    float win[GL_FB_MAX_POLY][4];
    for (int i = 0; i < n; i++)
        gl_fb_window(ctx, &poly[i], win[i]);
    float area = 0.0f;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        area += win[i][0] * win[j][1] - win[j][0] * win[i][1];
    }
    const GLboolean ccw = (GLboolean)(area > 0.0f);
    const GLboolean front = (ctx->front_face == GL_CW) ? (GLboolean)!ccw : ccw;
    if (ctx->cap_cull_face) {
        if (ctx->cull_mode == GL_FRONT_AND_BACK)
            return;
        if (ctx->cull_mode == GL_FRONT && front)
            return;
        if (ctx->cull_mode == GL_BACK && !front)
            return;
    }

    const GLenum mode = ctx->polygon_mode[front ? 0 : 1];
    if (mode == GL_FILL) {
        if (ctx->render_mode == GL_SELECT) {
            for (int i = 0; i < n; i++)
                gl_select_hit(ctx, win[i][2]);
        } else {
            gl_fb_put(ctx, (float)GL_POLYGON_TOKEN);
            gl_fb_put(ctx, (float)n);
            for (int i = 0; i < n; i++)
                gl_fb_put_vertex(ctx, win[i], poly[i].col, poly[i].tc);
        }
        return;
    }
    const gl_vertex_t *vtx[3] = {v0, v1, v2};
    for (int e = 0; e < 3; e++) {
        if (!(edges & (1u << e)))
            continue;
        if (use_flags && !vtx[e]->edge)
            continue;
        if (mode == GL_LINE) {
            gl_fb_line_coloured(ctx, vtx[e], vtx[(e + 1) % 3], flat);
        } else {
            gl_fb_point_coloured(ctx, vtx[e], flat);
        }
    }
}

/* -------------------------------------------------------------------------
 * The raster position and the pixel rectangles
 * ------------------------------------------------------------------------- */

void gl_fb_raster_hit(gl_context_t *ctx) {
    if (ctx->render_mode == GL_SELECT && ctx->raster_valid)
        gl_select_hit(ctx, ctx->raster_pos[2]);
}

void gl_fb_pixel_token(gl_context_t *ctx, GLenum token) {
    if (ctx->render_mode != GL_FEEDBACK || !ctx->raster_valid)
        return;
    gl_fb_put(ctx, (float)token);
    gl_fb_put_vertex(ctx, ctx->raster_pos, ctx->raster_color, ctx->raster_texcoord[0]);
}
