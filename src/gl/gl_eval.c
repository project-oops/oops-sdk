/*
 * oops-gl: evaluators
 *
 * A map is a Bezier curve (glMap1) or surface (glMap2) for one vertex attribute.
 * glEvalCoord evaluates every enabled map and issues the glVertex, glNormal, glColor
 * and glTexCoord calls it stands for, on the CPU through the immediate-mode path;
 * glMapGrid and glEvalMesh walk a grid. The current attributes are put back after each
 * evaluated vertex, as Mesa does (vbo/vbo_exec_api.c:712-770); map precedence follows
 * vbo/vbo_exec_eval.c:65-119, and colour-index maps evaluate to nothing in this RGBA
 * context. De Casteljau's construction gives the derivative GL_AUTO_NORMAL needs.
 */

#include "gl_internal.h"

#ifdef OOPS_HOST_BUILD
#include <stdlib.h>
#endif

/* Components per map, in the order the targets run (GL_MAP1_COLOR_4 + slot). */
static const int k_eval_components[OOPS_GL_EVAL_MAPS] = {4, 1, 3, 1, 2, 3, 4, 3, 4};

/* The one control point each map starts with, before any glMap: white for colour, index
 * 1, the normal (0,0,1), and (0,0,0,1) for texture coordinates and vertices, each cut
 * to the map's component count (Mesa main/eval.c, _mesa_init_eval). */
static const float k_eval_initial[OOPS_GL_EVAL_MAPS][4] = {
    {1.0f, 1.0f, 1.0f, 1.0f}, /* colour */
    {1.0f, 0.0f, 0.0f, 0.0f}, /* index */
    {0.0f, 0.0f, 1.0f, 0.0f}, /* normal */
    {0.0f, 0.0f, 0.0f, 1.0f}, /* texture coordinate 1 */
    {0.0f, 0.0f, 0.0f, 1.0f}, /* 2 */
    {0.0f, 0.0f, 0.0f, 1.0f}, /* 3 */
    {0.0f, 0.0f, 0.0f, 1.0f}, /* 4 */
    {0.0f, 0.0f, 0.0f, 1.0f}, /* vertex 3 */
    {0.0f, 0.0f, 0.0f, 1.0f}, /* vertex 4 */
};

enum {
    EVAL_SLOT_COLOR = 0,
    EVAL_SLOT_INDEX = 1,
    EVAL_SLOT_NORMAL = 2,
    EVAL_SLOT_TEX1 = 3,
    EVAL_SLOT_TEX4 = 6,
    EVAL_SLOT_VERTEX3 = 7,
    EVAL_SLOT_VERTEX4 = 8,
};

static void *gl_eval_alloc(size_t bytes) {
#ifdef OOPS_HOST_BUILD
    return malloc(bytes);
#else
    return oops_malloc(bytes);
#endif
}

static void gl_eval_release(void *p) {
    if (!p)
        return;
#ifdef OOPS_HOST_BUILD
    free(p);
#else
    oops_free(p);
#endif
}

/* The map a target names, and its dimension and slot; NULL for anything that is not a
 * map. */
static gl_eval_map_t *gl_eval_map(gl_context_t *ctx, GLenum target, int *dim,
                                  int *slot) {
    if (target >= GL_MAP1_COLOR_4 && target <= GL_MAP1_VERTEX_4) {
        *dim = 1;
        *slot = (int)(target - GL_MAP1_COLOR_4);
        return &ctx->map1[*slot];
    }
    if (target >= GL_MAP2_COLOR_4 && target <= GL_MAP2_VERTEX_4) {
        *dim = 2;
        *slot = (int)(target - GL_MAP2_COLOR_4);
        return &ctx->map2[*slot];
    }
    return (gl_eval_map_t *)0;
}

/* The component count of a map of dimension `dim`, or 0 when the target is not one. */
static int gl_eval_target_components(GLenum target, int dim) {
    if (dim == 1 && target >= GL_MAP1_COLOR_4 && target <= GL_MAP1_VERTEX_4) {
        return k_eval_components[target - GL_MAP1_COLOR_4];
    }
    if (dim == 2 && target >= GL_MAP2_COLOR_4 && target <= GL_MAP2_VERTEX_4) {
        return k_eval_components[target - GL_MAP2_COLOR_4];
    }
    return 0;
}

static const float *gl_eval_points(const gl_eval_map_t *m, int slot) {
    return m->points ? m->points : k_eval_initial[slot];
}

GLboolean *gl_eval_cap(gl_context_t *ctx, GLenum cap) {
    if (!ctx)
        return (GLboolean *)0;
    if (cap == GL_AUTO_NORMAL)
        return &ctx->cap_auto_normal;
    if (cap >= GL_MAP1_COLOR_4 && cap <= GL_MAP1_VERTEX_4)
        return &ctx->cap_map1[cap - GL_MAP1_COLOR_4];
    if (cap >= GL_MAP2_COLOR_4 && cap <= GL_MAP2_VERTEX_4)
        return &ctx->cap_map2[cap - GL_MAP2_COLOR_4];
    return (GLboolean *)0;
}

void gl_eval_init(gl_context_t *ctx) {
    if (!ctx)
        return;
    for (int i = 0; i < OOPS_GL_EVAL_MAPS; i++) {
        gl_eval_map_t *maps[2] = {&ctx->map1[i], &ctx->map2[i]};
        for (int d = 0; d < 2; d++) {
            maps[d]->uorder = 1;
            maps[d]->vorder = 1;
            maps[d]->u1 = 0.0f;
            maps[d]->u2 = 1.0f;
            maps[d]->v1 = 0.0f;
            maps[d]->v2 = 1.0f;
            maps[d]->points = (float *)0;
        }
        ctx->cap_map1[i] = GL_FALSE;
        ctx->cap_map2[i] = GL_FALSE;
    }
    ctx->cap_auto_normal = GL_FALSE;
    ctx->grid1_un = 1;
    ctx->grid1_u1 = 0.0f;
    ctx->grid1_u2 = 1.0f;
    ctx->grid2_un = 1;
    ctx->grid2_vn = 1;
    ctx->grid2_u1 = 0.0f;
    ctx->grid2_u2 = 1.0f;
    ctx->grid2_v1 = 0.0f;
    ctx->grid2_v2 = 1.0f;
}

void gl_eval_free(gl_context_t *ctx) {
    if (!ctx)
        return;
    for (int i = 0; i < OOPS_GL_EVAL_MAPS; i++) {
        gl_eval_release(ctx->map1[i].points);
        gl_eval_release(ctx->map2[i].points);
        ctx->map1[i].points = (float *)0;
        ctx->map2[i].points = (float *)0;
    }
}

/* -------------------------------------------------------------------------
 * Defining a map
 * ------------------------------------------------------------------------- */

/* Copies `uorder` x `vorder` control points of `k` components out of the caller's array
 * - `fp` or `dp`, whichever is not NULL - into `out`, packed tight with u outermost.
 * The strides are the caller's, in values, and may leave gaps between points: that is
 * what lets a program keep its control points inside larger records. */
static void gl_eval_pack(float *out, const GLfloat *fp, const GLdouble *dp,
                         GLint ustride, GLint uorder, GLint vstride, GLint vorder,
                         int k) {
    for (GLint i = 0; i < uorder; i++) {
        for (GLint j = 0; j < vorder; j++) {
            const size_t at = (size_t)i * (size_t)ustride + (size_t)j * (size_t)vstride;
            for (int c = 0; c < k; c++) {
                *out++ = fp ? fp[at + (size_t)c] : (float)dp[at + (size_t)c];
            }
        }
    }
}

/* glMap1 and glMap2, both spellings. Checked in the order that gives each wrong
 * argument its own error: the target (GL_INVALID_ENUM), then the domain, the orders,
 * the strides and the points (GL_INVALID_VALUE) - Mesa's checks, with the target first
 * (main/eval.c map1/map2). */
static void gl_eval_define(GLenum target, GLfloat u1, GLfloat u2, GLint ustride,
                           GLint uorder, GLfloat v1, GLfloat v2, GLint vstride,
                           GLint vorder, const GLfloat *fp, const GLdouble *dp,
                           int want_dim) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    int dim = 0, slot = 0;
    gl_eval_map_t *m = gl_eval_map(ctx, target, &dim, &slot);
    if (!m || dim != want_dim) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    const int k = k_eval_components[slot];
    if (u1 == u2 || (dim == 2 && v1 == v2) || uorder < 1 ||
        uorder > OOPS_GL_MAX_EVAL_ORDER || vorder < 1 ||
        vorder > OOPS_GL_MAX_EVAL_ORDER || ustride < k || (dim == 2 && vstride < k) ||
        (!fp && !dp)) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    float *pts = (float *)gl_eval_alloc((size_t)uorder * (size_t)vorder * (size_t)k *
                                        sizeof(float));
    if (!pts) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    gl_eval_pack(pts, fp, dp, ustride, uorder, vstride, vorder, k);
    gl_eval_release(m->points);
    m->points = pts;
    m->uorder = uorder;
    m->vorder = vorder;
    m->u1 = u1;
    m->u2 = u2;
    m->v1 = (dim == 2) ? v1 : 0.0f;
    m->v2 = (dim == 2) ? v2 : 1.0f;
}

/* Compiles a glMap: the control points are packed now, as the specification requires -
 * the list must not see what the caller's array holds later - and replayed with the
 * tight strides that packing produced. A call that will fail when it runs keeps no
 * points and replays with strides of zero, which fails the same way: the target is
 * checked before the strides, and every other failure is GL_INVALID_VALUE either way.
 * (Mesa records the tight stride even for a call whose own stride was too small,
 * main/dlist.c save_Map1f, so the replay succeeds where the call itself would not
 * have.) */
static GLboolean gl_eval_rec_map(gl_list_op_t op, GLenum target, GLfloat u1, GLfloat u2,
                                 GLint ustride, GLint uorder, GLfloat v1, GLfloat v2,
                                 GLint vstride, GLint vorder, const GLfloat *fp,
                                 const GLdouble *dp) {
    const GLboolean two = (GLboolean)(op == GL_LIST_OP_MAP2);
    const int k = gl_eval_target_components(target, two ? 2 : 1);
    const GLboolean packable =
        (GLboolean)(k > 0 && (fp || dp) && uorder >= 1 &&
                    uorder <= OOPS_GL_MAX_EVAL_ORDER && ustride >= k &&
                    (!two || (vorder >= 1 && vorder <= OOPS_GL_MAX_EVAL_ORDER &&
                              vstride >= k)));
    void *pts = (void *)0;
    size_t pack_bytes = 0u;
    if (packable) {
        const GLint vo = two ? vorder : 1;
        pack_bytes = (size_t)uorder * (size_t)vo * (size_t)k * sizeof(float);
        pts = gl_list_alloc(pack_bytes);
        if (!pts) {
            gl_context_t *ctx = gl_get_ctx();
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            return (GLboolean)(ctx && ctx->list_mode == GL_COMPILE);
        }
        gl_eval_pack((float *)pts, fp, dp, ustride, uorder, two ? vstride : 0, vo, k);
    }
    if (two) {
        const gl_list_arg_t a[7] = {gl_la_e(target), gl_la_f(u1), gl_la_f(u2),
                                    gl_la_i(uorder), gl_la_f(v1), gl_la_f(v2),
                                    gl_la_i(vorder)};
        return gl_list_rec_owned(op, a, 7, pts, pts ? pack_bytes : 0u);
    }
    const gl_list_arg_t a[4] = {gl_la_e(target), gl_la_f(u1), gl_la_f(u2),
                                gl_la_i(uorder)};
    return gl_list_rec_owned(op, a, 4, pts, pts ? pack_bytes : 0u);
}

void gl_eval_replay_map(const gl_list_cmd_t *cmd) {
    const gl_list_arg_t *a = cmd->a;
    const GLfloat *pts = (const GLfloat *)cmd->data;
    const int k = gl_eval_target_components(a[0].e, cmd->op == GL_LIST_OP_MAP2 ? 2 : 1);
    if (cmd->op == GL_LIST_OP_MAP2) {
        glMap2f(a[0].e, a[1].f, a[2].f, pts ? a[6].i * k : 0, a[3].i, a[4].f, a[5].f,
                pts ? k : 0, a[6].i, pts);
    } else {
        glMap1f(a[0].e, a[1].f, a[2].f, pts ? k : 0, a[3].i, pts);
    }
}

void glMap1f(GLenum target, GLfloat u1, GLfloat u2, GLint stride, GLint order,
             const GLfloat *points) {
    if (gl_list_recording() &&
        gl_eval_rec_map(GL_LIST_OP_MAP1, target, u1, u2, stride, order, 0.0f, 0.0f, 0,
                        1, points, (const GLdouble *)0)) {
        return;
    }
    gl_eval_define(target, u1, u2, stride, order, 0.0f, 1.0f, 0, 1, points,
                   (const GLdouble *)0, 1);
}

void glMap1d(GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order,
             const GLdouble *points) {
    if (gl_list_recording() &&
        gl_eval_rec_map(GL_LIST_OP_MAP1, target, (GLfloat)u1, (GLfloat)u2, stride,
                        order, 0.0f, 0.0f, 0, 1, (const GLfloat *)0, points)) {
        return;
    }
    gl_eval_define(target, (GLfloat)u1, (GLfloat)u2, stride, order, 0.0f, 1.0f, 0, 1,
                   (const GLfloat *)0, points, 1);
}

void glMap2f(GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder,
             GLfloat v1, GLfloat v2, GLint vstride, GLint vorder,
             const GLfloat *points) {
    if (gl_list_recording() &&
        gl_eval_rec_map(GL_LIST_OP_MAP2, target, u1, u2, ustride, uorder, v1, v2,
                        vstride, vorder, points, (const GLdouble *)0)) {
        return;
    }
    gl_eval_define(target, u1, u2, ustride, uorder, v1, v2, vstride, vorder, points,
                   (const GLdouble *)0, 2);
}

void glMap2d(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder,
             GLdouble v1, GLdouble v2, GLint vstride, GLint vorder,
             const GLdouble *points) {
    if (gl_list_recording() &&
        gl_eval_rec_map(GL_LIST_OP_MAP2, target, (GLfloat)u1, (GLfloat)u2, ustride,
                        uorder, (GLfloat)v1, (GLfloat)v2, vstride, vorder,
                        (const GLfloat *)0, points)) {
        return;
    }
    gl_eval_define(target, (GLfloat)u1, (GLfloat)u2, ustride, uorder, (GLfloat)v1,
                   (GLfloat)v2, vstride, vorder, (const GLfloat *)0, points, 2);
}

/* -------------------------------------------------------------------------
 * Querying a map
 * ------------------------------------------------------------------------- */

/* Where a glGetMap answer goes: exactly one of the three is set. Each value is
 * converted as it is written, straight into the caller's array - GL_COEFF can be 3600
 * values, and a staging copy of that on the stack is a poor trade for a query. Integers
 * round to the nearest, as Mesa rounds them (main/eval.c, lroundf); nothing here is a
 * colour to normalise. */
typedef struct {
    GLfloat *f;
    GLdouble *d;
    GLint *i;
} gl_eval_sink_t;

static void gl_eval_put(const gl_eval_sink_t *s, size_t at, float x) {
    if (s->f)
        s->f[at] = x;
    else if (s->d)
        s->d[at] = (GLdouble)x;
    else
        s->i[at] = (GLint)(x < 0.0f ? x - 0.5f : x + 0.5f);
}

/* A map never defined answers its initial state: order 1, the domain 0..1, and the one
 * initial control point. GL_COEFF writes the points in the order glMap took them, u
 * outermost. */
static void gl_eval_query(GLenum target, GLenum query, const gl_eval_sink_t *s) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    int dim = 0, slot = 0;
    const gl_eval_map_t *m = gl_eval_map(ctx, target, &dim, &slot);
    if (!m) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    switch (query) {
    case GL_COEFF: {
        const size_t n =
            (size_t)m->uorder * (size_t)m->vorder * (size_t)k_eval_components[slot];
        const float *pts = gl_eval_points(m, slot);
        for (size_t i = 0; i < n; i++)
            gl_eval_put(s, i, pts[i]);
        break;
    }
    case GL_ORDER:
        gl_eval_put(s, 0, (float)m->uorder);
        if (dim == 2)
            gl_eval_put(s, 1, (float)m->vorder);
        break;
    case GL_DOMAIN:
        gl_eval_put(s, 0, m->u1);
        gl_eval_put(s, 1, m->u2);
        if (dim == 2) {
            gl_eval_put(s, 2, m->v1);
            gl_eval_put(s, 3, m->v2);
        }
        break;
    default:
        gl_record_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

void glGetMapfv(GLenum target, GLenum query, GLfloat *v) {
    if (!v)
        return;
    const gl_eval_sink_t s = {v, (GLdouble *)0, (GLint *)0};
    gl_eval_query(target, query, &s);
}

void glGetMapdv(GLenum target, GLenum query, GLdouble *v) {
    if (!v)
        return;
    const gl_eval_sink_t s = {(GLfloat *)0, v, (GLint *)0};
    gl_eval_query(target, query, &s);
}

void glGetMapiv(GLenum target, GLenum query, GLint *v) {
    if (!v)
        return;
    const gl_eval_sink_t s = {(GLfloat *)0, (GLdouble *)0, v};
    gl_eval_query(target, query, &s);
}

/* -------------------------------------------------------------------------
 * Evaluating
 * ------------------------------------------------------------------------- */

/* The Bezier curve through `n` control points of `k` components, `stride` values apart,
 * at parameter `t` (0..1 across the domain; outside it the polynomial simply
 * continues). Writes the point, and when `deriv` is not NULL its derivative with
 * respect to t: (n-1) times the difference of the two points left before the
 * construction's last step. */
static void gl_bezier(const float *p, int n, int k, int stride, float t, float *out,
                      float *deriv) {
    float b[OOPS_GL_MAX_EVAL_ORDER][4];
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < k; c++)
            b[i][c] = p[(size_t)i * (size_t)stride + (size_t)c];
    }
    const float s = 1.0f - t;
    if (deriv) {
        for (int c = 0; c < k; c++)
            deriv[c] = 0.0f;
    }
    for (int r = n - 1; r >= 1; r--) {
        if (r == 1 && deriv) {
            for (int c = 0; c < k; c++)
                deriv[c] = (float)(n - 1) * (b[1][c] - b[0][c]);
        }
        for (int i = 0; i < r; i++) {
            for (int c = 0; c < k; c++)
                b[i][c] = s * b[i][c] + t * b[i + 1][c];
        }
    }
    for (int c = 0; c < k; c++)
        out[c] = b[0][c];
}

/* A 1D map at domain value u, into `out` (whose components past the map's own are left
 * as they are, so the caller's defaults stand). */
static void gl_eval_map1(const gl_eval_map_t *m, int slot, float u, float *out) {
    const float t = (u - m->u1) / (m->u2 - m->u1);
    const int k = k_eval_components[slot];
    gl_bezier(gl_eval_points(m, slot), m->uorder, k, k, t, out, (float *)0);
}

/* A 2D map at (u, v). With `du` and `dv`, also the partial derivatives with respect to
 * the domain values themselves - not the 0..1 parameters - so a domain given backwards
 * turns them round, as it turns the surface round. */
static void gl_eval_map2(const gl_eval_map_t *m, int slot, float u, float v, float *out,
                         float *du, float *dv) {
    const float su = 1.0f / (m->u2 - m->u1);
    const float sv = 1.0f / (m->v2 - m->v1);
    const float tu = (u - m->u1) * su;
    const float tv = (v - m->v1) * sv;
    const int k = k_eval_components[slot];
    const float *pts = gl_eval_points(m, slot);
    float row[OOPS_GL_MAX_EVAL_ORDER][4];
    float drow[OOPS_GL_MAX_EVAL_ORDER][4];
    for (int i = 0; i < m->uorder; i++) {
        gl_bezier(pts + (size_t)i * (size_t)m->vorder * (size_t)k, m->vorder, k, k, tv,
                  row[i], dv ? drow[i] : (float *)0);
    }
    gl_bezier(&row[0][0], m->uorder, k, 4, tu, out, du);
    if (du) {
        for (int c = 0; c < k; c++)
            du[c] *= su;
    }
    if (dv) {
        gl_bezier(&drow[0][0], m->uorder, k, 4, tu, dv, (float *)0);
        for (int c = 0; c < k; c++)
            dv[c] *= sv;
    }
}

/* Issues one evaluated vertex: the evaluated attributes stand in for the current ones
 * for this glVertex only, and the current ones are put back after it. The texture
 * coordinate maps feed unit 0, as Mesa's evaluator feeds its TEX0 attribute
 * (vbo/vbo_exec_eval.c:88-92). */
static void gl_eval_emit(gl_context_t *ctx, const float pos[4], const float color[4],
                         const float normal[3], const float tc[4]) {
    float saved_color[4], saved_normal[3], saved_tc[4];
    for (int i = 0; i < 4; i++)
        saved_color[i] = ctx->cur_color[i];
    for (int i = 0; i < 3; i++)
        saved_normal[i] = ctx->cur_normal[i];
    for (int i = 0; i < 4; i++)
        saved_tc[i] = ctx->cur_texcoord[0][i];
    for (int i = 0; i < 4; i++)
        ctx->cur_color[i] = color[i];
    for (int i = 0; i < 3; i++)
        ctx->cur_normal[i] = normal[i];
    for (int i = 0; i < 4; i++)
        ctx->cur_texcoord[0][i] = tc[i];
    glVertex4f(pos[0], pos[1], pos[2], pos[3]);
    for (int i = 0; i < 4; i++)
        ctx->cur_color[i] = saved_color[i];
    for (int i = 0; i < 3; i++)
        ctx->cur_normal[i] = saved_normal[i];
    for (int i = 0; i < 4; i++)
        ctx->cur_texcoord[0][i] = saved_tc[i];
}

/* The enabled vertex map of a dimension - 4 over 3 - or -1 for none. */
static int gl_eval_vertex_slot(const GLboolean *caps) {
    if (caps[EVAL_SLOT_VERTEX4])
        return EVAL_SLOT_VERTEX4;
    if (caps[EVAL_SLOT_VERTEX3])
        return EVAL_SLOT_VERTEX3;
    return -1;
}

/* The highest enabled texture-coordinate map, or -1. */
static int gl_eval_tex_slot(const GLboolean *caps) {
    for (int s = EVAL_SLOT_TEX4; s >= EVAL_SLOT_TEX1; s--) {
        if (caps[s])
            return s;
    }
    return -1;
}

static void gl_eval_coord1(gl_context_t *ctx, float u) {
    const int vs = gl_eval_vertex_slot(ctx->cap_map1);
    if (vs < 0)
        return;

    float color[4], normal[3];
    float tc[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float pos[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 4; i++)
        color[i] = ctx->cur_color[i];
    for (int i = 0; i < 3; i++)
        normal[i] = ctx->cur_normal[i];

    if (ctx->cap_map1[EVAL_SLOT_COLOR])
        gl_eval_map1(&ctx->map1[EVAL_SLOT_COLOR], EVAL_SLOT_COLOR, u, color);
    if (ctx->cap_map1[EVAL_SLOT_NORMAL])
        gl_eval_map1(&ctx->map1[EVAL_SLOT_NORMAL], EVAL_SLOT_NORMAL, u, normal);
    const int ts = gl_eval_tex_slot(ctx->cap_map1);
    if (ts >= 0) {
        gl_eval_map1(&ctx->map1[ts], ts, u, tc);
    } else {
        for (int i = 0; i < 4; i++)
            tc[i] = ctx->cur_texcoord[0][i];
    }
    gl_eval_map1(&ctx->map1[vs], vs, u, pos);
    gl_eval_emit(ctx, pos, color, normal, tc);
}

static void gl_eval_coord2(gl_context_t *ctx, float u, float v) {
    const int vs = gl_eval_vertex_slot(ctx->cap_map2);
    if (vs < 0)
        return;

    float color[4], normal[3];
    float tc[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float pos[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < 4; i++)
        color[i] = ctx->cur_color[i];
    for (int i = 0; i < 3; i++)
        normal[i] = ctx->cur_normal[i];

    if (ctx->cap_map2[EVAL_SLOT_COLOR]) {
        gl_eval_map2(&ctx->map2[EVAL_SLOT_COLOR], EVAL_SLOT_COLOR, u, v, color,
                     (float *)0, (float *)0);
    }
    const int ts = gl_eval_tex_slot(ctx->cap_map2);
    if (ts >= 0) {
        gl_eval_map2(&ctx->map2[ts], ts, u, v, tc, (float *)0, (float *)0);
    } else {
        for (int i = 0; i < 4; i++)
            tc[i] = ctx->cur_texcoord[0][i];
    }

    if (ctx->cap_auto_normal) {
        /* GL_AUTO_NORMAL is the surface's own normal: the cross product of its
         * partial derivatives, which takes precedence over a normal map. For a
         * four-component vertex map it is the normal of the projected surface (x/w,
         * y/w, z/w); each partial derivative of that is (dp w - dw p) / w^2, and the
         * w^2 is dropped because only the direction survives the normalisation (Mesa
         * vbo/vbo_exec_eval.c:196-209). A degenerate point - a patch's corner collapsed
         * to a pole, as at the top of the teapot's lid - has no normal, and gets the
         * zero vector rather than a direction made up for it. */
        float du[4] = {0.0f, 0.0f, 0.0f, 0.0f}, dv[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        gl_eval_map2(&ctx->map2[vs], vs, u, v, pos, du, dv);
        if (vs == EVAL_SLOT_VERTEX4) {
            for (int c = 0; c < 3; c++) {
                du[c] = du[c] * pos[3] - du[3] * pos[c];
                dv[c] = dv[c] * pos[3] - dv[3] * pos[c];
            }
        }
        normal[0] = du[1] * dv[2] - du[2] * dv[1];
        normal[1] = du[2] * dv[0] - du[0] * dv[2];
        normal[2] = du[0] * dv[1] - du[1] * dv[0];
        const float len = gl_sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                                  normal[2] * normal[2]);
        if (len > 0.0f) {
            for (int c = 0; c < 3; c++)
                normal[c] /= len;
        }
    } else {
        if (ctx->cap_map2[EVAL_SLOT_NORMAL]) {
            gl_eval_map2(&ctx->map2[EVAL_SLOT_NORMAL], EVAL_SLOT_NORMAL, u, v, normal,
                         (float *)0, (float *)0);
        }
        gl_eval_map2(&ctx->map2[vs], vs, u, v, pos, (float *)0, (float *)0);
    }
    gl_eval_emit(ctx, pos, color, normal, tc);
}

void glEvalCoord1f(GLfloat u) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_EVAL_COORD1, gl_la_f(u)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    gl_eval_coord1(ctx, u);
}

void glEvalCoord2f(GLfloat u, GLfloat v) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_EVAL_COORD2, gl_la_f(u), gl_la_f(v)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    gl_eval_coord2(ctx, u, v);
}

void glEvalCoord1d(GLdouble u) {
    glEvalCoord1f((GLfloat)u);
}
void glEvalCoord2d(GLdouble u, GLdouble v) {
    glEvalCoord2f((GLfloat)u, (GLfloat)v);
}
void glEvalCoord1fv(const GLfloat *u) {
    if (u)
        glEvalCoord1f(u[0]);
}
void glEvalCoord1dv(const GLdouble *u) {
    if (u)
        glEvalCoord1f((GLfloat)u[0]);
}
void glEvalCoord2fv(const GLfloat *u) {
    if (u)
        glEvalCoord2f(u[0], u[1]);
}
void glEvalCoord2dv(const GLdouble *u) {
    if (u)
        glEvalCoord2f((GLfloat)u[0], (GLfloat)u[1]);
}

/* -------------------------------------------------------------------------
 * The grid
 * ------------------------------------------------------------------------- */

void glMapGrid1f(GLint un, GLfloat u1, GLfloat u2) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_MAP_GRID1, gl_la_i(un), gl_la_f(u1), gl_la_f(u2)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (un < 1) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    ctx->grid1_un = un;
    ctx->grid1_u1 = u1;
    ctx->grid1_u2 = u2;
}

void glMapGrid2f(GLint un, GLfloat u1, GLfloat u2, GLint vn, GLfloat v1, GLfloat v2) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_MAP_GRID2, gl_la_i(un), gl_la_f(u1), gl_la_f(u2),
                    gl_la_i(vn), gl_la_f(v1), gl_la_f(v2))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (un < 1 || vn < 1) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    ctx->grid2_un = un;
    ctx->grid2_u1 = u1;
    ctx->grid2_u2 = u2;
    ctx->grid2_vn = vn;
    ctx->grid2_v1 = v1;
    ctx->grid2_v2 = v2;
}

void glMapGrid1d(GLint un, GLdouble u1, GLdouble u2) {
    glMapGrid1f(un, (GLfloat)u1, (GLfloat)u2);
}

void glMapGrid2d(GLint un, GLdouble u1, GLdouble u2, GLint vn, GLdouble v1,
                 GLdouble v2) {
    glMapGrid2f(un, (GLfloat)u1, (GLfloat)u2, vn, (GLfloat)v1, (GLfloat)v2);
}

/* Grid line `i` of `n` across [a, b], computed from i each time rather than
 * accumulated, and line n is b exactly: patches meeting along an edge then share
 * vertices to the bit and the surface does not crack. */
static float gl_grid_at(GLint i, GLint n, float a, float b) {
    if (i == n)
        return b;
    return a + (float)i * ((b - a) / (float)n);
}

void glEvalPoint1(GLint i) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_EVAL_POINT1, gl_la_i(i)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    gl_eval_coord1(ctx, gl_grid_at(i, ctx->grid1_un, ctx->grid1_u1, ctx->grid1_u2));
}

void glEvalPoint2(GLint i, GLint j) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_EVAL_POINT2, gl_la_i(i), gl_la_i(j)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    gl_eval_coord2(ctx, gl_grid_at(i, ctx->grid2_un, ctx->grid2_u1, ctx->grid2_u2),
                   gl_grid_at(j, ctx->grid2_vn, ctx->grid2_v1, ctx->grid2_v2));
}

/* glEvalMesh is glBegin, a run of glEvalCoord and glEnd - so inside an open glBegin it
 * would start a primitive within a primitive and throw the open one's vertices away.
 * The specification makes that GL_INVALID_OPERATION, and it is refused before anything
 * is drawn. */
void glEvalMesh1(GLenum mode, GLint i1, GLint i2) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_EVAL_MESH1, gl_la_e(mode), gl_la_i(i1), gl_la_i(i2)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    GLenum prim;
    switch (mode) {
    case GL_POINT:
        prim = GL_POINTS;
        break;
    case GL_LINE:
        prim = GL_LINE_STRIP;
        break;
    default:
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (ctx->imm_active) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    glBegin(prim);
    for (GLint i = i1; i <= i2; i++) {
        gl_eval_coord1(ctx, gl_grid_at(i, ctx->grid1_un, ctx->grid1_u1, ctx->grid1_u2));
    }
    glEnd();
}

/* GL_FILL is a quad strip per row of the grid, as the specification writes it - not a
 * triangle strip, which Mesa substitutes (main/draw.c _mesa_EvalMesh2) and which draws
 * the same filled area but outlines each quad's diagonal under glPolygonMode(GL_LINE).
 */
void glEvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_EVAL_MESH2, gl_la_e(mode), gl_la_i(i1), gl_la_i(i2),
                    gl_la_i(j1), gl_la_i(j2))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (ctx->imm_active) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    const GLint un = ctx->grid2_un, vn = ctx->grid2_vn;
    const float ua = ctx->grid2_u1, ub = ctx->grid2_u2;
    const float va = ctx->grid2_v1, vb = ctx->grid2_v2;
    switch (mode) {
    case GL_POINT:
        glBegin(GL_POINTS);
        for (GLint j = j1; j <= j2; j++) {
            for (GLint i = i1; i <= i2; i++) {
                gl_eval_coord2(ctx, gl_grid_at(i, un, ua, ub),
                               gl_grid_at(j, vn, va, vb));
            }
        }
        glEnd();
        break;
    case GL_LINE:
        for (GLint j = j1; j <= j2; j++) {
            glBegin(GL_LINE_STRIP);
            for (GLint i = i1; i <= i2; i++) {
                gl_eval_coord2(ctx, gl_grid_at(i, un, ua, ub),
                               gl_grid_at(j, vn, va, vb));
            }
            glEnd();
        }
        for (GLint i = i1; i <= i2; i++) {
            glBegin(GL_LINE_STRIP);
            for (GLint j = j1; j <= j2; j++) {
                gl_eval_coord2(ctx, gl_grid_at(i, un, ua, ub),
                               gl_grid_at(j, vn, va, vb));
            }
            glEnd();
        }
        break;
    default: /* GL_FILL */
        for (GLint j = j1; j < j2; j++) {
            const float v0 = gl_grid_at(j, vn, va, vb);
            const float v1 = gl_grid_at(j + 1, vn, va, vb);
            glBegin(GL_QUAD_STRIP);
            for (GLint i = i1; i <= i2; i++) {
                const float u = gl_grid_at(i, un, ua, ub);
                gl_eval_coord2(ctx, u, v0);
                gl_eval_coord2(ctx, u, v1);
            }
            glEnd();
        }
        break;
    }
}
