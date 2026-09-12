/*
 * oops-gl: Matrix operations, stacks, projection, and transformations
 */

#include "gl_internal.h"

#define GL_PI 3.14159265358979323846f
#define GL_TWO_PI 6.28318530717958647692f
#define GL_HALF_PI 1.57079632679489661923f

float gl_sin(float x) {
    while (x > GL_PI) x -= GL_TWO_PI;
    while (x < -GL_PI) x += GL_TWO_PI;
    float x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f - x2 * (1.0f / 120.0f - x2 * (1.0f / 5040.0f - x2 * (1.0f / 362880.0f)))));
}

float gl_cos(float x) {
    return gl_sin(x + GL_HALF_PI);
}

float gl_tan(float x) {
    float c = gl_cos(x);
    if (c > -1e-7f && c < 1e-7f) {
        c = (c >= 0.0f) ? 1e-7f : -1e-7f;
    }
    return gl_sin(x) / c;
}

float gl_sqrt(float val) {
    if (val <= 0.0f) return 0.0f;
    return __builtin_sqrtf(val);
}

float gl_pow(float base, float p) {
    if (p == 0.0f) return 1.0f;
    if (base <= 0.0f) return 0.0f;
    if (base >= 1.0f) return 1.0f;

    uint32_t u;
    memcpy(&u, &base, sizeof(float));
    int exp_val = (int)((u >> 23) & 0xff) - 127;
    uint32_t mant_bits = (u & 0x7fffffu) | (127u << 23);
    float m;
    memcpy(&m, &mant_bits, sizeof(float));

    float z = (m - 1.0f) / (m + 1.0f);
    float z2 = z * z;
    float ln_m = 2.0f * z * (1.0f + z2 * (1.0f / 3.0f + z2 * (1.0f / 5.0f + z2 * (1.0f / 7.0f + z2 * (1.0f / 9.0f)))));
    float ln_base = ln_m + (float)exp_val * 0.69314718056f;

    float y = p * ln_base;
    if (y < -60.0f) return 0.0f;

    int k = (int)(y * 1.4426950408889634f + (y >= 0.0f ? 0.5f : -0.5f));
    float r = y - (float)k * 0.69314718056f;
    float exp_r = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + r * (0.041666667f + r * 0.008333333f))));

    int new_exp = k + 127;
    if (new_exp <= 0) return 0.0f;
    if (new_exp >= 255) return 1.0f;

    uint32_t scale_bits = (uint32_t)new_exp << 23;
    float scale;
    memcpy(&scale, &scale_bits, sizeof(float));
    return exp_r * scale;
}

void mat4_identity(gl_mat4_t *out) {
    if (!out) return;
    for (int i = 0; i < 16; i++) {
        out->m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
}

void mat4_mult(gl_mat4_t *out, const gl_mat4_t *a, const gl_mat4_t *b) {
    gl_mat4_t res;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a->m[k * 4 + row] * b->m[col * 4 + k];
            }
            res.m[col * 4 + row] = sum;
        }
    }
    memcpy(out, &res, sizeof(gl_mat4_t));
}

void mat4_translate(gl_mat4_t *out, float x, float y, float z) {
    gl_mat4_t t;
    mat4_identity(&t);
    t.m[12] = x;
    t.m[13] = y;
    t.m[14] = z;
    mat4_mult(out, out, &t);
}

void mat4_scale(gl_mat4_t *out, float x, float y, float z) {
    gl_mat4_t s;
    mat4_identity(&s);
    s.m[0] = x;
    s.m[5] = y;
    s.m[10] = z;
    mat4_mult(out, out, &s);
}

void mat4_rotate(gl_mat4_t *out, float angle_deg, float x, float y, float z) {
    float len = gl_sqrt(x * x + y * y + z * z);
    if (len < 1e-6f) return;
    float inv_len = 1.0f / len;
    x *= inv_len;
    y *= inv_len;
    z *= inv_len;

    float rad = angle_deg * (GL_PI / 180.0f);
    float c = gl_cos(rad);
    float s = gl_sin(rad);
    float t = 1.0f - c;

    gl_mat4_t r;
    mat4_identity(&r);

    r.m[0]  = t * x * x + c;
    r.m[1]  = t * x * y + s * z;
    r.m[2]  = t * x * z - s * y;

    r.m[4]  = t * x * y - s * z;
    r.m[5]  = t * y * y + c;
    r.m[6]  = t * y * z + s * x;

    r.m[8]  = t * x * z + s * y;
    r.m[9]  = t * y * z - s * x;
    r.m[10] = t * z * z + c;

    mat4_mult(out, out, &r);
}

void mat4_frustum(gl_mat4_t *out, float l, float r, float b, float t, float n, float f) {
    if (r == l || t == b || f == n) return;
    gl_mat4_t p;
    memset(&p, 0, sizeof(p));

    p.m[0]  = (2.0f * n) / (r - l);
    p.m[5]  = (2.0f * n) / (t - b);
    p.m[8]  = (r + l) / (r - l);
    p.m[9]  = (t + b) / (t - b);
    p.m[10] = -(f + n) / (f - n);
    p.m[11] = -1.0f;
    p.m[14] = -(2.0f * f * n) / (f - n);

    mat4_mult(out, out, &p);
}

void mat4_ortho(gl_mat4_t *out, float l, float r, float b, float t, float n, float f) {
    if (r == l || t == b || f == n) return;
    gl_mat4_t o;
    mat4_identity(&o);

    o.m[0]  = 2.0f / (r - l);
    o.m[5]  = 2.0f / (t - b);
    o.m[10] = -2.0f / (f - n);
    o.m[12] = -(r + l) / (r - l);
    o.m[13] = -(t + b) / (t - b);
    o.m[14] = -(f + n) / (f - n);

    mat4_mult(out, out, &o);
}

void mat4_transform_vec4(float *out4, const gl_mat4_t *m, const float *in4) {
    float x = in4[0], y = in4[1], z = in4[2], w = in4[3];
    out4[0] = m->m[0] * x + m->m[4] * y + m->m[8]  * z + m->m[12] * w;
    out4[1] = m->m[1] * x + m->m[5] * y + m->m[9]  * z + m->m[13] * w;
    out4[2] = m->m[2] * x + m->m[6] * y + m->m[10] * z + m->m[14] * w;
    out4[3] = m->m[3] * x + m->m[7] * y + m->m[11] * z + m->m[15] * w;
}

static gl_mat4_t *get_active_matrix(gl_context_t *ctx) {
    switch (ctx->matrix_mode) {
        case GL_PROJECTION:
            return &ctx->projection_stack[ctx->projection_depth];
        case GL_TEXTURE:
            return &ctx->texture_stack[ctx->texture_depth];
        case GL_MODELVIEW:
        default:
            return &ctx->modelview_stack[ctx->modelview_depth];
    }
}

void gl_update_mvp(gl_context_t *ctx) {
    if (!ctx) return;
    if (ctx->mvp_dirty) {
        const gl_mat4_t *proj = &ctx->projection_stack[ctx->projection_depth];
        const gl_mat4_t *modv = &ctx->modelview_stack[ctx->modelview_depth];
        mat4_mult(&ctx->mvp, proj, modv);
        ctx->mvp_dirty = GL_FALSE;
    }
}

void glMatrixMode(GLenum mode) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (mode == GL_MODELVIEW || mode == GL_PROJECTION || mode == GL_TEXTURE) {
        ctx->matrix_mode = mode;
    }
}

void glPushMatrix(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    switch (ctx->matrix_mode) {
        case GL_MODELVIEW:
            if (ctx->modelview_depth < GL_MAX_MODELVIEW_STACK_DEPTH - 1) {
                ctx->modelview_stack[ctx->modelview_depth + 1] = ctx->modelview_stack[ctx->modelview_depth];
                ctx->modelview_depth++;
            } else {
                ctx->last_error = GL_STACK_OVERFLOW;
            }
            break;
        case GL_PROJECTION:
            if (ctx->projection_depth < GL_MAX_PROJECTION_STACK_DEPTH - 1) {
                ctx->projection_stack[ctx->projection_depth + 1] = ctx->projection_stack[ctx->projection_depth];
                ctx->projection_depth++;
            } else {
                ctx->last_error = GL_STACK_OVERFLOW;
            }
            break;
        case GL_TEXTURE:
            if (ctx->texture_depth < GL_MAX_TEXTURE_STACK_DEPTH - 1) {
                ctx->texture_stack[ctx->texture_depth + 1] = ctx->texture_stack[ctx->texture_depth];
                ctx->texture_depth++;
            } else {
                ctx->last_error = GL_STACK_OVERFLOW;
            }
            break;
        default: break;
    }
}

static inline void mark_matrix_dirty(gl_context_t *ctx) {
    ctx->mvp_dirty = GL_TRUE;
    if (ctx->matrix_mode == GL_MODELVIEW) {
        ctx->normal_matrix_dirty = GL_TRUE;
    }
}

void gl_update_normal_matrix(gl_context_t *ctx) {
    if (!ctx) return;
    if (!ctx->normal_matrix_dirty) return;

    const gl_mat4_t *mv = &ctx->modelview_stack[ctx->modelview_depth];
    float a00 = mv->m[0], a10 = mv->m[1], a20 = mv->m[2];
    float a01 = mv->m[4], a11 = mv->m[5], a21 = mv->m[6];
    float a02 = mv->m[8], a12 = mv->m[9], a22 = mv->m[10];

    float c00 = a11 * a22 - a12 * a21;
    float c01 = a12 * a20 - a10 * a22;
    float c02 = a10 * a21 - a11 * a20;

    float c10 = a02 * a21 - a01 * a22;
    float c11 = a00 * a22 - a02 * a20;
    float c12 = a01 * a20 - a00 * a21;

    float c20 = a01 * a12 - a02 * a11;
    float c21 = a02 * a10 - a00 * a12;
    float c22 = a00 * a11 - a01 * a10;

    float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (det > -1e-12f && det < 1e-12f) {
        ctx->normal_matrix[0] = 1.0f; ctx->normal_matrix[1] = 0.0f; ctx->normal_matrix[2] = 0.0f;
        ctx->normal_matrix[3] = 0.0f; ctx->normal_matrix[4] = 1.0f; ctx->normal_matrix[5] = 0.0f;
        ctx->normal_matrix[6] = 0.0f; ctx->normal_matrix[7] = 0.0f; ctx->normal_matrix[8] = 1.0f;
    } else {
        float inv_det = 1.0f / det;
        ctx->normal_matrix[0] = c00 * inv_det;
        ctx->normal_matrix[1] = c01 * inv_det;
        ctx->normal_matrix[2] = c02 * inv_det;
        ctx->normal_matrix[3] = c10 * inv_det;
        ctx->normal_matrix[4] = c11 * inv_det;
        ctx->normal_matrix[5] = c12 * inv_det;
        ctx->normal_matrix[6] = c20 * inv_det;
        ctx->normal_matrix[7] = c21 * inv_det;
        ctx->normal_matrix[8] = c22 * inv_det;
    }
    ctx->normal_matrix_dirty = GL_FALSE;
}

void glPopMatrix(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    switch (ctx->matrix_mode) {
        case GL_MODELVIEW:
            if (ctx->modelview_depth > 0) {
                ctx->modelview_depth--;
            } else {
                ctx->last_error = GL_STACK_UNDERFLOW;
            }
            break;
        case GL_PROJECTION:
            if (ctx->projection_depth > 0) {
                ctx->projection_depth--;
            } else {
                ctx->last_error = GL_STACK_UNDERFLOW;
            }
            break;
        case GL_TEXTURE:
            if (ctx->texture_depth > 0) {
                ctx->texture_depth--;
            } else {
                ctx->last_error = GL_STACK_UNDERFLOW;
            }
            break;
        default: break;
    }
    mark_matrix_dirty(ctx);
}

void glLoadIdentity(void) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    mat4_identity(get_active_matrix(ctx));
    mark_matrix_dirty(ctx);
}

void glLoadMatrixf(const GLfloat *m) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !m) return;
    memcpy(get_active_matrix(ctx)->m, m, 16 * sizeof(float));
    mark_matrix_dirty(ctx);
}

void glMultMatrixf(const GLfloat *m) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !m) return;
    gl_mat4_t in;
    memcpy(in.m, m, 16 * sizeof(float));
    gl_mat4_t *cur = get_active_matrix(ctx);
    mat4_mult(cur, cur, &in);
    mark_matrix_dirty(ctx);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    mat4_translate(get_active_matrix(ctx), (float)x, (float)y, (float)z);
    mark_matrix_dirty(ctx);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    mat4_rotate(get_active_matrix(ctx), (float)angle, (float)x, (float)y, (float)z);
    mark_matrix_dirty(ctx);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    mat4_scale(get_active_matrix(ctx), (float)x, (float)y, (float)z);
    mark_matrix_dirty(ctx);
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble near_val, GLdouble far_val) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    mat4_ortho(get_active_matrix(ctx), (float)left, (float)right, (float)bottom,
               (float)top, (float)near_val, (float)far_val);
    mark_matrix_dirty(ctx);
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble near_val, GLdouble far_val) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    mat4_frustum(get_active_matrix(ctx), (float)left, (float)right, (float)bottom,
                 (float)top, (float)near_val, (float)far_val);
    mark_matrix_dirty(ctx);
}

/* -------------------------------------------------------------------------
 * GLU Helper Implementations
 * ------------------------------------------------------------------------- */

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar) {
    if (aspect == 0.0 || zNear == zFar) return;
    float fov_rad = (float)(fovy * (GL_PI / 360.0)); /* fovy / 2 in radians */
    float f = gl_cos(fov_rad) / gl_sin(fov_rad);

    gl_mat4_t p;
    memset(&p, 0, sizeof(p));
    p.m[0]  = f / (float)aspect;
    p.m[5]  = f;
    p.m[10] = (float)((zFar + zNear) / (zNear - zFar));
    p.m[11] = -1.0f;
    p.m[14] = (float)((2.0 * zFar * zNear) / (zNear - zFar));

    glMultMatrixf(p.m);
}

void gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ,
               GLdouble centerX, GLdouble centerY, GLdouble centerZ,
               GLdouble upX, GLdouble upY, GLdouble upZ) {
    float fx = (float)(centerX - eyeX);
    float fy = (float)(centerY - eyeY);
    float fz = (float)(centerZ - eyeZ);
    float flen = gl_sqrt(fx * fx + fy * fy + fz * fz);
    if (flen > 1e-6f) {
        fx /= flen; fy /= flen; fz /= flen;
    }

    float ux = (float)upX;
    float uy = (float)upY;
    float uz = (float)upZ;
    float ulen = gl_sqrt(ux * ux + uy * uy + uz * uz);
    if (ulen > 1e-6f) {
        ux /= ulen; uy /= ulen; uz /= ulen;
    }

    /* s = f x u */
    float sx = fy * uz - fz * uy;
    float sy = fz * ux - fx * uz;
    float sz = fx * uy - fy * ux;
    float slen = gl_sqrt(sx * sx + sy * sy + sz * sz);
    if (slen > 1e-6f) {
        sx /= slen; sy /= slen; sz /= slen;
    }

    /* u' = s x f */
    ux = sy * fz - sz * fy;
    uy = sz * fx - sx * fz;
    uz = sx * fy - sy * fx;

    gl_mat4_t m;
    mat4_identity(&m);
    m.m[0] = sx;  m.m[4] = sy;  m.m[8]  = sz;
    m.m[1] = ux;  m.m[5] = uy;  m.m[9]  = uz;
    m.m[2] = -fx; m.m[6] = -fy; m.m[10] = -fz;

    glMultMatrixf(m.m);
    glTranslatef((GLfloat)-eyeX, (GLfloat)-eyeY, (GLfloat)-eyeZ);
}

void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top) {
    glOrtho(left, right, bottom, top, -1.0, 1.0);
}

const GLubyte *gluErrorString(GLenum error) {
    switch (error) {
        case GL_NO_ERROR:          return (const GLubyte *)"no error";
        case GL_INVALID_ENUM:      return (const GLubyte *)"invalid enumerant";
        case GL_INVALID_VALUE:     return (const GLubyte *)"invalid value";
        case GL_INVALID_OPERATION: return (const GLubyte *)"invalid operation";
        case GL_STACK_OVERFLOW:    return (const GLubyte *)"stack overflow";
        case GL_STACK_UNDERFLOW:   return (const GLubyte *)"stack underflow";
        case GL_OUT_OF_MEMORY:     return (const GLubyte *)"out of memory";
        default:                   return (const GLubyte *)"unknown error";
    }
}
