/*
 * oops-gl: Backend-neutral OpenGL Utility Library (GLU).
 *
 * This file provides standard GLU 1.3 functions without depending on any oops-gl
 * internal state or private functions. It calls only public OpenGL entry points
 * (declared in GL/gl.h), and uses freestanding math/memory functions.
 *
 * It can be linked against oops-gl (freestanding) or upstream Mesa (hosted).
 */

#include "GL/glu.h"
#include <string.h>

#if defined(OOPS_HOST_BUILD) || defined(USE_MESA)
#include <stdlib.h>
#include <math.h>
#define glu_sinf(x)   sinf(x)
#define glu_cosf(x)   cosf(x)
#define glu_sqrtf(x)  sqrtf(x)
#define glu_alloc(n)  malloc(n)
#define glu_free(p)   free(p)
#else
#include "oops/math.h"
#include "oops/heap.h"
#define glu_sinf(x)   oops_sinf(x)
#define glu_cosf(x)   oops_cosf(x)
#define glu_sqrtf(x)  oops_sqrtf(x)
#define glu_alloc(n)  oops_malloc(n)
#define glu_free(p)   oops_free(p)
#endif

#define GLU_PI 3.14159265358979323846f

static inline float glu_clamp(float v, float mn, float mx) {
    if (v < mn) return mn;
    if (v > mx) return mx;
    return v;
}

static int glu_ceil_i(double v) {
    const int t = (int)v;
    return (v > (double)t) ? t + 1 : t;
}

/* ---------------------------------------------------------------------------
 * Matrix and Projection Transformations
 * --------------------------------------------------------------------------- */

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar) {
    if (aspect == 0.0 || zNear == zFar) return;
    float fov_rad = (float)(fovy * ((double)GLU_PI / 360.0)); /* fovy / 2 in radians */
    float f = glu_cosf(fov_rad) / glu_sinf(fov_rad);

    float p[16];
    memset(p, 0, sizeof(p));
    p[0]  = f / (float)aspect;
    p[5]  = f;
    p[10] = (float)((zFar + zNear) / (zNear - zFar));
    p[11] = -1.0f;
    p[14] = (float)((2.0 * zFar * zNear) / (zNear - zFar));

    glMultMatrixf(p);
}

void gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ,
               GLdouble centerX, GLdouble centerY, GLdouble centerZ,
               GLdouble upX, GLdouble upY, GLdouble upZ) {
    float fx = (float)(centerX - eyeX);
    float fy = (float)(centerY - eyeY);
    float fz = (float)(centerZ - eyeZ);
    float flen = glu_sqrtf(fx * fx + fy * fy + fz * fz);
    if (flen > 1e-6f) {
        fx /= flen; fy /= flen; fz /= flen;
    }

    float ux = (float)upX;
    float uy = (float)upY;
    float uz = (float)upZ;
    float ulen = glu_sqrtf(ux * ux + uy * uy + uz * uz);
    if (ulen > 1e-6f) {
        ux /= ulen; uy /= ulen; uz /= ulen;
    }

    /* s = f x u */
    float sx = fy * uz - fz * uy;
    float sy = fz * ux - fx * uz;
    float sz = fx * uy - fy * ux;
    float slen = glu_sqrtf(sx * sx + sy * sy + sz * sz);
    if (slen > 1e-6f) {
        sx /= slen; sy /= slen; sz /= slen;
    }

    /* u' = s x f */
    ux = sy * fz - sz * fy;
    uy = sz * fx - sx * fz;
    uz = sx * fy - sy * fx;

    float m[16];
    memset(m, 0, sizeof(m));
    m[0] = sx;  m[4] = sy;  m[8]  = sz;
    m[1] = ux;  m[5] = uy;  m[9]  = uz;
    m[2] = -fx; m[6] = -fy; m[10] = -fz;
    m[15] = 1.0f;

    glMultMatrixf(m);
    glTranslatef((GLfloat)-eyeX, (GLfloat)-eyeY, (GLfloat)-eyeZ);
}

void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top) {
    glOrtho(left, right, bottom, top, -1.0, 1.0);
}

void gluPickMatrix(GLdouble x, GLdouble y, GLdouble dx, GLdouble dy, GLint *viewport) {
    if (!viewport || dx <= 0.0 || dy <= 0.0) return;
    glTranslatef((GLfloat)(((GLdouble)viewport[2] - 2.0 * (x - (GLdouble)viewport[0])) / dx),
                 (GLfloat)(((GLdouble)viewport[3] - 2.0 * (y - (GLdouble)viewport[1])) / dy),
                 0.0f);
    glScalef((GLfloat)((GLdouble)viewport[2] / dx), (GLfloat)((GLdouble)viewport[3] / dy), 1.0f);
}

const GLubyte *gluErrorString(GLenum error) {
    switch (error) {
        case GL_NO_ERROR:                   return (const GLubyte *)"no error";
        case GL_INVALID_ENUM:               return (const GLubyte *)"invalid enumerant";
        case GL_INVALID_VALUE:              return (const GLubyte *)"invalid value";
        case GL_INVALID_OPERATION:          return (const GLubyte *)"invalid operation";
        case GL_STACK_OVERFLOW:             return (const GLubyte *)"stack overflow";
        case GL_STACK_UNDERFLOW:            return (const GLubyte *)"stack underflow";
        case GL_OUT_OF_MEMORY:              return (const GLubyte *)"out of memory";
        case GLU_INVALID_ENUM:              return (const GLubyte *)"invalid enumerant";
        case GLU_INVALID_VALUE:             return (const GLubyte *)"invalid value";
        case GLU_OUT_OF_MEMORY:             return (const GLubyte *)"out of memory";
        case GLU_INVALID_OPERATION:         return (const GLubyte *)"invalid operation";
        case GLU_INCOMPATIBLE_GL_VERSION:   return (const GLubyte *)"incompatible gl version";
        default:                            return (const GLubyte *)"unknown error";
    }
}

const GLubyte *gluGetString(GLenum name) {
    switch (name) {
        case GLU_VERSION:    return (const GLubyte *)"1.3";
        case GLU_EXTENSIONS: return (const GLubyte *)"";
        default:             return (const GLubyte *)0;
    }
}

/* ---------------------------------------------------------------------------
 * Object space to the window and back
 * --------------------------------------------------------------------------- */

static void glu_mul_mat(const GLdouble *a, const GLdouble *b, GLdouble *out) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            GLdouble s = 0.0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
    }
}

static void glu_mul_vec(const GLdouble *m, const GLdouble *v, GLdouble *out) {
    for (int r = 0; r < 4; r++) {
        out[r] = m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] + m[2 * 4 + r] * v[2] +
                 m[3 * 4 + r] * v[3];
    }
}

static GLboolean glu_invert(const GLdouble *m, GLdouble *inv) {
    GLdouble a[16];
    for (int i = 0; i < 16; i++) a[i] = m[i];
    GLdouble c[16];
    c[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    c[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    c[8]  =  a[4]*a[9]*a[15]  - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    c[12] = -a[4]*a[9]*a[14]  + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    c[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    c[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    c[9]  = -a[0]*a[9]*a[15]  + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    c[13] =  a[0]*a[9]*a[14]  - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    c[2]  =  a[1]*a[6]*a[15]  - a[1]*a[7]*a[14]  - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7]  - a[13]*a[3]*a[6];
    c[6]  = -a[0]*a[6]*a[15]  + a[0]*a[7]*a[14]  + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7]  + a[12]*a[3]*a[6];
    c[10] =  a[0]*a[5]*a[15]  - a[0]*a[7]*a[13]  - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7]  - a[12]*a[3]*a[5];
    c[14] = -a[0]*a[5]*a[14]  + a[0]*a[6]*a[13]  + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6]  + a[12]*a[2]*a[5];
    c[3]  = -a[1]*a[6]*a[11]  + a[1]*a[7]*a[10]  + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7]   + a[9]*a[3]*a[6];
    c[7]  =  a[0]*a[6]*a[11]  - a[0]*a[7]*a[10]  - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7]   - a[8]*a[3]*a[6];
    c[11] = -a[0]*a[5]*a[11]  + a[0]*a[7]*a[9]   + a[4]*a[1]*a[11] - a[4]*a[3]*a[9]  - a[8]*a[1]*a[7]   + a[8]*a[3]*a[5];
    c[15] =  a[0]*a[5]*a[10]  - a[0]*a[6]*a[9]   - a[4]*a[1]*a[10] + a[4]*a[2]*a[9]  + a[8]*a[1]*a[6]   - a[8]*a[2]*a[5];
    GLdouble det = a[0] * c[0] + a[1] * c[4] + a[2] * c[8] + a[3] * c[12];
    if (det == 0.0) return GL_FALSE;
    det = 1.0 / det;
    for (int i = 0; i < 16; i++) inv[i] = c[i] * det;
    return GL_TRUE;
}

GLint gluProject(GLdouble objX, GLdouble objY, GLdouble objZ, const GLdouble *model,
                 const GLdouble *proj, const GLint *view, GLdouble *winX, GLdouble *winY,
                 GLdouble *winZ) {
    if (!model || !proj || !view || !winX || !winY || !winZ) return GL_FALSE;
    const GLdouble in[4] = {objX, objY, objZ, 1.0};
    GLdouble eye[4], clip[4];
    glu_mul_vec(model, in, eye);
    glu_mul_vec(proj, eye, clip);
    if (clip[3] == 0.0) return GL_FALSE;
    const GLdouble w = 1.0 / clip[3];
    *winX = (GLdouble)view[0] + (GLdouble)view[2] * (clip[0] * w + 1.0) * 0.5;
    *winY = (GLdouble)view[1] + (GLdouble)view[3] * (clip[1] * w + 1.0) * 0.5;
    *winZ = (clip[2] * w + 1.0) * 0.5;
    return GL_TRUE;
}

GLint gluUnProject(GLdouble winX, GLdouble winY, GLdouble winZ, const GLdouble *model,
                   const GLdouble *proj, const GLint *view, GLdouble *objX, GLdouble *objY,
                   GLdouble *objZ) {
    if (!model || !proj || !view || !objX || !objY || !objZ) return GL_FALSE;
    if (view[2] == 0 || view[3] == 0) return GL_FALSE;
    GLdouble pm[16], inv[16];
    glu_mul_mat(proj, model, pm);
    if (!glu_invert(pm, inv)) return GL_FALSE;
    const GLdouble in[4] = {
        (winX - (GLdouble)view[0]) / (GLdouble)view[2] * 2.0 - 1.0,
        (winY - (GLdouble)view[1]) / (GLdouble)view[3] * 2.0 - 1.0,
        winZ * 2.0 - 1.0,
        1.0,
    };
    GLdouble out[4];
    glu_mul_vec(inv, in, out);
    if (out[3] == 0.0) return GL_FALSE;
    *objX = out[0] / out[3];
    *objY = out[1] / out[3];
    *objZ = out[2] / out[3];
    return GL_TRUE;
}

/* ---------------------------------------------------------------------------
 * Quadrics
 * --------------------------------------------------------------------------- */

struct GLUquadric {
    GLenum draw_style;  /* GLU_FILL, GLU_LINE, GLU_SILHOUETTE, GLU_POINT */
    GLenum normals;     /* GLU_SMOOTH, GLU_FLAT, GLU_NONE */
    GLenum orientation; /* GLU_OUTSIDE, GLU_INSIDE */
    GLboolean texture;
    void (*error_cb)(GLenum);
};

GLUquadric *gluNewQuadric(void) {
    GLUquadric *q = (GLUquadric *)glu_alloc(sizeof(GLUquadric));
    if (!q) return (GLUquadric *)0;
    q->draw_style = GLU_FILL;
    q->normals = GLU_SMOOTH;
    q->orientation = GLU_OUTSIDE;
    q->texture = GL_FALSE;
    q->error_cb = (void (*)(GLenum))0;
    return q;
}

void gluDeleteQuadric(GLUquadric *q) {
    if (q) glu_free(q);
}

static void glu_quad_error(GLUquadric *q, GLenum e) {
    if (q && q->error_cb) q->error_cb(e);
}

void gluQuadricDrawStyle(GLUquadric *q, GLenum draw) {
    if (!q) return;
    if (draw != GLU_FILL && draw != GLU_LINE && draw != GLU_SILHOUETTE && draw != GLU_POINT) {
        glu_quad_error(q, GLU_INVALID_ENUM);
        return;
    }
    q->draw_style = draw;
}

void gluQuadricNormals(GLUquadric *q, GLenum normal) {
    if (!q) return;
    if (normal != GLU_SMOOTH && normal != GLU_FLAT && normal != GLU_NONE) {
        glu_quad_error(q, GLU_INVALID_ENUM);
        return;
    }
    q->normals = normal;
}

void gluQuadricOrientation(GLUquadric *q, GLenum orientation) {
    if (!q) return;
    if (orientation != GLU_OUTSIDE && orientation != GLU_INSIDE) {
        glu_quad_error(q, GLU_INVALID_ENUM);
        return;
    }
    q->orientation = orientation;
}

void gluQuadricTexture(GLUquadric *q, GLboolean texture) {
    if (q) q->texture = (GLboolean)(texture ? GL_TRUE : GL_FALSE);
}

void gluQuadricCallback(GLUquadric *q, GLenum which, void (*fn)(void)) {
    if (!q) return;
    if (which != GLU_ERROR) {
        glu_quad_error(q, GLU_INVALID_ENUM);
        return;
    }
    q->error_cb = (void (*)(GLenum))fn;
}

static float glu_sign(const GLUquadric *q) {
    return (q->orientation == GLU_INSIDE) ? -1.0f : 1.0f;
}

static void glu_normal(const GLUquadric *q, float x, float y, float z) {
    if (q->normals == GLU_NONE) return;
    const float s = glu_sign(q);
    glNormal3f(x * s, y * s, z * s);
}

static GLenum glu_strip_mode(const GLUquadric *q) {
    switch (q->draw_style) {
        case GLU_POINT: return GL_POINTS;
        case GLU_LINE:
        case GLU_SILHOUETTE: return GL_LINE_STRIP;
        default: return GL_QUAD_STRIP;
    }
}

void gluSphere(GLUquadric *q, GLdouble radius, GLint slices, GLint stacks) {
    if (!q) return;
    if (radius < 0.0 || slices < 2 || stacks < 1) {
        glu_quad_error(q, GLU_INVALID_VALUE);
        return;
    }
    const float r = (float)radius;
    const float drho = GLU_PI / (float)stacks, dtheta = 2.0f * GLU_PI / (float)slices;
    const GLenum mode = glu_strip_mode(q);
    const GLboolean inside = (GLboolean)(q->orientation == GLU_INSIDE);

    for (GLint cap = 0; cap < 2 && stacks >= 2; cap++) {
        const GLboolean north = (GLboolean)(cap == 0);
        const float rho = north ? drho : (GLU_PI - drho);
        const float sr = glu_sinf(rho), cr = glu_cosf(rho);
        const float pz = north ? 1.0f : -1.0f;
        const float t_pole = north ? 1.0f : 0.0f;
        const float t_ring = north ? (1.0f - 1.0f / (float)stacks) : (1.0f / (float)stacks);
        const GLboolean backwards = (GLboolean)(north != (GLboolean)(inside != GL_FALSE));
        glBegin(mode == GL_QUAD_STRIP ? GL_TRIANGLE_FAN : mode);
        glu_normal(q, 0.0f, 0.0f, pz);
        if (q->texture) glTexCoord2f(0.5f, t_pole);
        glVertex3f(0.0f, 0.0f, pz * r);
        for (GLint k = 0; k <= slices; k++) {
            const GLint j = backwards ? (slices - k) : k;
            const float theta = (float)(j % slices) * dtheta;
            const float st = glu_sinf(theta), ct = glu_cosf(theta);
            const float nx = st * sr, ny = ct * sr, nz = cr;
            glu_normal(q, nx, ny, nz);
            if (q->texture) glTexCoord2f((float)j / (float)slices, t_ring);
            glVertex3f(nx * r, ny * r, nz * r);
        }
        glEnd();
    }

    const GLint band_first = (stacks >= 2) ? 1 : 0;
    const GLint band_last = (stacks >= 2) ? stacks - 1 : stacks;
    for (GLint i = band_first; i < band_last; i++) {
        const float rho_hi = (float)i * drho, rho_lo = (float)(i + 1) * drho;
        const float t_hi = 1.0f - (float)i / (float)stacks;
        const float t_lo = 1.0f - (float)(i + 1) / (float)stacks;
        glBegin(mode);
        for (GLint j = 0; j <= slices; j++) {
            const float theta = (j == slices) ? 0.0f : (float)j * dtheta;
            const float st = glu_sinf(theta), ct = glu_cosf(theta);
            const float s = (float)j / (float)slices;
            for (int e = 0; e < 2; e++) {
                const int lower = inside ? (e == 1) : (e == 0);
                const float rho = lower ? rho_lo : rho_hi;
                const float tc = lower ? t_lo : t_hi;
                const float sr = glu_sinf(rho), cr = glu_cosf(rho);
                const float nx = st * sr, ny = ct * sr, nz = cr;
                glu_normal(q, nx, ny, nz);
                if (q->texture) glTexCoord2f(s, tc);
                glVertex3f(nx * r, ny * r, nz * r);
            }
        }
        glEnd();
    }

    if (q->draw_style == GLU_LINE) {
        for (GLint j = 0; j < slices; j++) {
            const float theta = (float)j * dtheta;
            const float st = glu_sinf(theta), ct = glu_cosf(theta);
            glBegin(GL_LINE_STRIP);
            for (GLint i = 0; i <= stacks; i++) {
                const float rho = (float)i * drho;
                const float sr = glu_sinf(rho), cr = glu_cosf(rho);
                glu_normal(q, st * sr, ct * sr, cr);
                if (q->texture) {
                    glTexCoord2f((float)j / (float)slices, 1.0f - (float)i / (float)stacks);
                }
                glVertex3f(st * sr * r, ct * sr * r, cr * r);
            }
            glEnd();
        }
    }
}

void gluCylinder(GLUquadric *q, GLdouble base, GLdouble top, GLdouble height, GLint slices,
                 GLint stacks) {
    if (!q) return;
    if (base < 0.0 || top < 0.0 || height < 0.0 || slices < 2 || stacks < 1) {
        glu_quad_error(q, GLU_INVALID_VALUE);
        return;
    }
    const float rb = (float)base, rt = (float)top, hh = (float)height;
    const float dtheta = 2.0f * GLU_PI / (float)slices;
    float nz = (hh > 0.0f) ? (rb - rt) / hh : 0.0f;
    const float nlen = glu_sqrtf(1.0f + nz * nz);
    const float nr = 1.0f / nlen;
    nz /= nlen;
    const GLenum mode = glu_strip_mode(q);
    const GLboolean inside = (GLboolean)(q->orientation == GLU_INSIDE);

    for (GLint i = 0; i < stacks; i++) {
        const float z0 = hh * (float)i / (float)stacks, z1 = hh * (float)(i + 1) / (float)stacks;
        const float r0 = rb + (rt - rb) * (float)i / (float)stacks;
        const float r1 = rb + (rt - rb) * (float)(i + 1) / (float)stacks;
        const float t0 = (float)i / (float)stacks, t1 = (float)(i + 1) / (float)stacks;
        glBegin(mode);
        for (GLint j = 0; j <= slices; j++) {
            const float theta = (j == slices) ? 0.0f : (float)j * dtheta;
            const float st = glu_sinf(theta), ct = glu_cosf(theta);
            const float s = (float)j / (float)slices;
            for (int e = 0; e < 2; e++) {
                const int upper = inside ? (e == 1) : (e == 0);
                const float rr = upper ? r1 : r0, zz = upper ? z1 : z0;
                glu_normal(q, st * nr, ct * nr, nz);
                if (q->texture) glTexCoord2f(s, upper ? t1 : t0);
                glVertex3f(st * rr, ct * rr, zz);
            }
        }
        glEnd();
    }
    if (q->draw_style == GLU_LINE || q->draw_style == GLU_SILHOUETTE) {
        for (GLint j = 0; j < slices; j++) {
            const float theta = (float)j * dtheta;
            const float st = glu_sinf(theta), ct = glu_cosf(theta);
            glBegin(GL_LINES);
            glu_normal(q, st * nr, ct * nr, nz);
            if (q->texture) glTexCoord2f((float)j / (float)slices, 0.0f);
            glVertex3f(st * rb, ct * rb, 0.0f);
            if (q->texture) glTexCoord2f((float)j / (float)slices, 1.0f);
            glVertex3f(st * rt, ct * rt, hh);
            glEnd();
        }
    }
}

static void glu_disk(GLUquadric *q, GLdouble inner, GLdouble outer, GLint slices, GLint loops,
                     GLdouble start, GLdouble sweep) {
    if (!q) return;
    if (inner < 0.0 || outer <= 0.0 || inner > outer || slices < 2 || loops < 1) {
        glu_quad_error(q, GLU_INVALID_VALUE);
        return;
    }
    const float ri = (float)inner, ro = (float)outer;
    const float a0 = (float)start * (GLU_PI / 180.0f), da = (float)sweep * (GLU_PI / 180.0f);
    const float dtheta = da / (float)slices;
    const GLenum mode = glu_strip_mode(q);
    const GLboolean inside = (GLboolean)(q->orientation == GLU_INSIDE);
    const float nz = inside ? -1.0f : 1.0f;

    for (GLint i = 0; i < loops; i++) {
        const float r0 = ri + (ro - ri) * (float)i / (float)loops;
        const float r1 = ri + (ro - ri) * (float)(i + 1) / (float)loops;
        glBegin(mode);
        for (GLint j = 0; j <= slices; j++) {
            const float theta = a0 + (float)j * dtheta;
            const float st = glu_sinf(theta), ct = glu_cosf(theta);
            for (int e = 0; e < 2; e++) {
                const int in_ring = inside ? (e == 1) : (e == 0);
                const float rr = in_ring ? r0 : r1;
                const float x = st * rr, y = ct * rr;
                if (q->normals != GLU_NONE) glNormal3f(0.0f, 0.0f, nz);
                if (q->texture) glTexCoord2f(x / ro * 0.5f + 0.5f, y / ro * 0.5f + 0.5f);
                glVertex3f(x, y, 0.0f);
            }
        }
        glEnd();
    }
    if (q->draw_style == GLU_LINE || q->draw_style == GLU_SILHOUETTE) {
        const GLboolean partial = (GLboolean)(sweep < 360.0 && sweep > -360.0);
        const GLint step = (q->draw_style == GLU_LINE) ? 1 : slices;
        if (q->draw_style == GLU_LINE || partial) {
            for (GLint j = 0; j <= slices; j += step) {
                const float theta = a0 + (float)j * dtheta;
                const float st = glu_sinf(theta), ct = glu_cosf(theta);
                glBegin(GL_LINES);
                if (q->normals != GLU_NONE) glNormal3f(0.0f, 0.0f, nz);
                glVertex3f(st * ri, ct * ri, 0.0f);
                glVertex3f(st * ro, ct * ro, 0.0f);
                glEnd();
            }
        }
    }
}

void gluDisk(GLUquadric *q, GLdouble inner, GLdouble outer, GLint slices, GLint loops) {
    glu_disk(q, inner, outer, slices, loops, 0.0, 360.0);
}

void gluPartialDisk(GLUquadric *q, GLdouble inner, GLdouble outer, GLint slices, GLint loops,
                    GLdouble start, GLdouble sweep) {
    glu_disk(q, inner, outer, slices, loops, start, sweep);
}

/* ---------------------------------------------------------------------------
 * Image Scaling and Mipmap Generation
 * --------------------------------------------------------------------------- */

static int glu_pixel_bytes(GLenum format, GLenum type) {
    int comp = 0;
    switch (format) {
        case GL_COLOR_INDEX:
        case GL_STENCIL_INDEX:
        case GL_DEPTH_COMPONENT:
        case GL_RED:
        case GL_GREEN:
        case GL_BLUE:
        case GL_ALPHA:
        case GL_LUMINANCE:
            comp = 1; break;
        case GL_LUMINANCE_ALPHA:
            comp = 2; break;
        case GL_RGB:
        case GL_BGR:
            comp = 3; break;
        case GL_RGBA:
        case GL_BGRA:
            comp = 4; break;
        default:
            return 0;
    }
    switch (type) {
        case GL_UNSIGNED_BYTE:
        case GL_BYTE:
            return comp * 1;
        case GL_UNSIGNED_SHORT:
        case GL_SHORT:
            return comp * 2;
        case GL_UNSIGNED_INT:
        case GL_INT:
        case GL_FLOAT:
            return comp * 4;
        default:
            return 0;
    }
}

static void glu_unpack_pixel(GLenum format, GLenum type, const uint8_t *src, float rgba[4]) {
    rgba[0] = 0.0f; rgba[1] = 0.0f; rgba[2] = 0.0f; rgba[3] = 1.0f;
    if (type == GL_UNSIGNED_BYTE) {
        switch (format) {
            case GL_RGBA:
                rgba[0] = (float)src[0] / 255.0f;
                rgba[1] = (float)src[1] / 255.0f;
                rgba[2] = (float)src[2] / 255.0f;
                rgba[3] = (float)src[3] / 255.0f;
                break;
            case GL_BGRA:
                rgba[2] = (float)src[0] / 255.0f;
                rgba[1] = (float)src[1] / 255.0f;
                rgba[0] = (float)src[2] / 255.0f;
                rgba[3] = (float)src[3] / 255.0f;
                break;
            case GL_RGB:
                rgba[0] = (float)src[0] / 255.0f;
                rgba[1] = (float)src[1] / 255.0f;
                rgba[2] = (float)src[2] / 255.0f;
                rgba[3] = 1.0f;
                break;
            case GL_BGR:
                rgba[2] = (float)src[0] / 255.0f;
                rgba[1] = (float)src[1] / 255.0f;
                rgba[0] = (float)src[2] / 255.0f;
                rgba[3] = 1.0f;
                break;
            case GL_LUMINANCE:
                rgba[0] = rgba[1] = rgba[2] = (float)src[0] / 255.0f;
                rgba[3] = 1.0f;
                break;
            case GL_ALPHA:
                rgba[0] = rgba[1] = rgba[2] = 0.0f;
                rgba[3] = (float)src[0] / 255.0f;
                break;
            case GL_LUMINANCE_ALPHA:
                rgba[0] = rgba[1] = rgba[2] = (float)src[0] / 255.0f;
                rgba[3] = (float)src[1] / 255.0f;
                break;
            default:
                rgba[0] = (float)src[0] / 255.0f;
                break;
        }
    } else if (type == GL_FLOAT) {
        const float *f = (const float *)src;
        switch (format) {
            case GL_RGBA:
                rgba[0] = f[0]; rgba[1] = f[1]; rgba[2] = f[2]; rgba[3] = f[3]; break;
            case GL_RGB:
                rgba[0] = f[0]; rgba[1] = f[1]; rgba[2] = f[2]; rgba[3] = 1.0f; break;
            case GL_LUMINANCE:
                rgba[0] = rgba[1] = rgba[2] = f[0]; rgba[3] = 1.0f; break;
            case GL_ALPHA:
                rgba[0] = rgba[1] = rgba[2] = 0.0f; rgba[3] = f[0]; break;
            case GL_LUMINANCE_ALPHA:
                rgba[0] = rgba[1] = rgba[2] = f[0]; rgba[3] = f[1]; break;
            default:
                rgba[0] = f[0]; break;
        }
    }
}

static void glu_pack_pixel(GLenum format, GLenum type, const float rgba[4], uint8_t *dst) {
    if (type == GL_UNSIGNED_BYTE) {
        uint8_t r = (uint8_t)(glu_clamp(rgba[0], 0.0f, 1.0f) * 255.0f + 0.5f);
        uint8_t g = (uint8_t)(glu_clamp(rgba[1], 0.0f, 1.0f) * 255.0f + 0.5f);
        uint8_t b = (uint8_t)(glu_clamp(rgba[2], 0.0f, 1.0f) * 255.0f + 0.5f);
        uint8_t a = (uint8_t)(glu_clamp(rgba[3], 0.0f, 1.0f) * 255.0f + 0.5f);
        switch (format) {
            case GL_RGBA: dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a; break;
            case GL_BGRA: dst[0] = b; dst[1] = g; dst[2] = r; dst[3] = a; break;
            case GL_RGB:  dst[0] = r; dst[1] = g; dst[2] = b; break;
            case GL_BGR:  dst[0] = b; dst[1] = g; dst[2] = r; break;
            case GL_LUMINANCE: dst[0] = r; break;
            case GL_ALPHA:     dst[0] = a; break;
            case GL_LUMINANCE_ALPHA: dst[0] = r; dst[1] = a; break;
            default: dst[0] = r; break;
        }
    } else if (type == GL_FLOAT) {
        float *f = (float *)dst;
        switch (format) {
            case GL_RGBA: f[0] = rgba[0]; f[1] = rgba[1]; f[2] = rgba[2]; f[3] = rgba[3]; break;
            case GL_RGB:  f[0] = rgba[0]; f[1] = rgba[1]; f[2] = rgba[2]; break;
            case GL_LUMINANCE: f[0] = rgba[0]; break;
            case GL_ALPHA:     f[0] = rgba[3]; break;
            case GL_LUMINANCE_ALPHA: f[0] = rgba[0]; f[1] = rgba[3]; break;
            default: f[0] = rgba[0]; break;
        }
    }
}

static void glu_box_pixel(GLenum format, GLenum type, const uint8_t *src, size_t src_row,
                          size_t pixel_bytes, GLsizei w_in, GLsizei h_in,
                          double x0, double x1, double y0, double y1, float out[4]) {
    int ix0 = (int)x0, ix1 = glu_ceil_i(x1), iy0 = (int)y0, iy1 = glu_ceil_i(y1);
    if (ix1 <= ix0) ix1 = ix0 + 1;
    if (iy1 <= iy0) iy1 = iy0 + 1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > w_in) ix1 = w_in;
    if (iy1 > h_in) iy1 = h_in;
    float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    unsigned n = 0u;
    for (int y = iy0; y < iy1; y++) {
        const uint8_t *row = src + (size_t)y * src_row;
        for (int x = ix0; x < ix1; x++) {
            float p[4];
            glu_unpack_pixel(format, type, row + (size_t)x * pixel_bytes, p);
            for (int c = 0; c < 4; c++) sum[c] += p[c];
            n++;
        }
    }
    if (n == 0u) n = 1u;
    for (int c = 0; c < 4; c++) out[c] = sum[c] / (float)n;
}

static void glu_box_scale(GLenum formatIn, GLenum typeIn, const uint8_t *src, size_t src_row,
                          GLsizei w_in, GLsizei h_in,
                          GLenum formatOut, GLenum typeOut, uint8_t *dst, size_t dst_row,
                          GLsizei w_out, GLsizei h_out) {
    size_t in_bytes = (size_t)glu_pixel_bytes(formatIn, typeIn);
    size_t out_bytes = (size_t)glu_pixel_bytes(formatOut, typeOut);
    if (in_bytes == 0 || out_bytes == 0) return;

    const double sx = (double)w_in / (double)w_out, sy = (double)h_in / (double)h_out;
    for (GLsizei y = 0; y < h_out; y++) {
        uint8_t *drow = dst + (size_t)y * dst_row;
        for (GLsizei x = 0; x < w_out; x++) {
            float rgba[4];
            glu_box_pixel(formatIn, typeIn, src, src_row, in_bytes, w_in, h_in,
                          (double)x * sx, (double)(x + 1) * sx,
                          (double)y * sy, (double)(y + 1) * sy, rgba);
            glu_pack_pixel(formatOut, typeOut, rgba, drow + (size_t)x * out_bytes);
        }
    }
}

GLint gluScaleImage(GLenum format, GLsizei wIn, GLsizei hIn, GLenum typeIn, const void *dataIn,
                    GLsizei wOut, GLsizei hOut, GLenum typeOut, void *dataOut) {
    if (wIn <= 0 || hIn <= 0 || wOut <= 0 || hOut <= 0) return GLU_INVALID_VALUE;
    if (!dataIn || !dataOut) return GLU_INVALID_VALUE;
    int in_bytes = glu_pixel_bytes(format, typeIn);
    int out_bytes = glu_pixel_bytes(format, typeOut);
    if (in_bytes == 0 || out_bytes == 0) return GLU_INVALID_ENUM;

    GLint unpack_row_length = 0, unpack_alignment = 4, unpack_skip_rows = 0, unpack_skip_pixels = 0;
    GLint pack_row_length = 0, pack_alignment = 4, pack_skip_rows = 0, pack_skip_pixels = 0;

    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);

    glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &pack_skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &pack_skip_pixels);

    if (unpack_alignment < 1) unpack_alignment = 1;
    if (pack_alignment < 1) pack_alignment = 1;

    size_t in_width = (unpack_row_length > 0) ? (size_t)unpack_row_length : (size_t)wIn;
    size_t in_stride = in_width * (size_t)in_bytes;
    in_stride = (in_stride + (size_t)unpack_alignment - 1u) & ~((size_t)unpack_alignment - 1u);

    size_t out_width = (pack_row_length > 0) ? (size_t)pack_row_length : (size_t)wOut;
    size_t out_stride = out_width * (size_t)out_bytes;
    out_stride = (out_stride + (size_t)pack_alignment - 1u) & ~((size_t)pack_alignment - 1u);

    const uint8_t *src = (const uint8_t *)dataIn + (size_t)unpack_skip_rows * in_stride + (size_t)unpack_skip_pixels * (size_t)in_bytes;
    uint8_t *dst = (uint8_t *)dataOut + (size_t)pack_skip_rows * out_stride + (size_t)pack_skip_pixels * (size_t)out_bytes;

    glu_box_scale(format, typeIn, src, in_stride, wIn, hIn,
                  format, typeOut, dst, out_stride, wOut, hOut);
    return 0;
}

static GLsizei glu_max_texture_size(void) {
    GLint max_tex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    if (max_tex <= 0) max_tex = 2048;
    return (GLsizei)max_tex;
}

static GLsizei glu_pot(GLsizei n) {
    if (n <= 1) return 1;
    GLsizei max_s = glu_max_texture_size();
    GLsizei lo = 1;
    while (lo * 2 <= n && lo * 2 <= max_s) lo *= 2;
    GLsizei hi = (lo * 2 <= max_s) ? lo * 2 : lo;
    GLsizei n_lo = n - lo, n_hi = (hi >= n) ? (hi - n) : (n - hi);
    GLsizei r = (n_hi < n_lo) ? hi : lo;
    if (r > max_s) r = max_s;
    return r;
}

static GLint glu_build_mipmaps(GLenum target, GLint internalFormat, GLsizei width, GLsizei height,
                               GLenum format, GLenum type, const void *data, GLboolean one_d) {
    if (width <= 0 || height <= 0) return GLU_INVALID_VALUE;
    if (!data) return GLU_INVALID_VALUE;
    int pixel_bytes = glu_pixel_bytes(format, type);
    if (pixel_bytes == 0) return GLU_INVALID_ENUM;

    GLint unpack_row_length = 0, unpack_alignment = 4, unpack_skip_rows = 0, unpack_skip_pixels = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row_length);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_alignment);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &unpack_skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &unpack_skip_pixels);

    if (unpack_alignment < 1) unpack_alignment = 1;

    GLsizei w = glu_pot(width), h = one_d ? 1 : glu_pot(height);
    uint8_t *level = (uint8_t *)glu_alloc((size_t)w * (size_t)h * (size_t)pixel_bytes);
    if (!level) return GLU_OUT_OF_MEMORY;

    /* Read level zero through caller's unpack state */
    size_t in_width = (unpack_row_length > 0) ? (size_t)unpack_row_length : (size_t)width;
    size_t in_stride = in_width * (size_t)pixel_bytes;
    in_stride = (in_stride + (size_t)unpack_alignment - 1u) & ~((size_t)unpack_alignment - 1u);
    const uint8_t *src = (const uint8_t *)data + (size_t)unpack_skip_rows * in_stride + (size_t)unpack_skip_pixels * (size_t)pixel_bytes;

    glu_box_scale(format, type, src, in_stride, width, height,
                  format, type, level, (size_t)w * (size_t)pixel_bytes, w, h);

    /* Switch unpack state to tightly packed 1-byte aligned for our internal mipmap buffers */
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    GLint lvl = 0;
    for (;;) {
        if (one_d) {
            glTexImage1D(target, lvl, internalFormat, w, 0, format, type, level);
        } else {
            glTexImage2D(target, lvl, internalFormat, w, h, 0, format, type, level);
        }
        if (w == 1 && h == 1) break;

        GLsizei nw = (w > 1) ? w / 2 : 1;
        GLsizei nh = (h > 1) ? h / 2 : 1;
        uint8_t *next = (uint8_t *)glu_alloc((size_t)nw * (size_t)nh * (size_t)pixel_bytes);
        if (!next) {
            glu_free(level);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);
            glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
            return GLU_OUT_OF_MEMORY;
        }

        glu_box_scale(format, type, level, (size_t)w * (size_t)pixel_bytes, w, h,
                      format, type, next, (size_t)nw * (size_t)pixel_bytes, nw, nh);
        glu_free(level);
        level = next;
        w = nw;
        h = nh;
        lvl++;
    }

    glu_free(level);
    /* Restore caller's unpack state */
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row_length);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, unpack_skip_rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, unpack_skip_pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_alignment);
    return 0;
}

GLint gluBuild2DMipmaps(GLenum target, GLint internalFormat, GLsizei width, GLsizei height,
                        GLenum format, GLenum type, const void *data) {
    return glu_build_mipmaps(target, internalFormat, width, height, format, type, data, GL_FALSE);
}

GLint gluBuild1DMipmaps(GLenum target, GLint internalFormat, GLsizei width, GLenum format,
                        GLenum type, const void *data) {
    return glu_build_mipmaps(target, internalFormat, width, 1, format, type, data, GL_TRUE);
}
