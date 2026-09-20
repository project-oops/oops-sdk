/*
 * oops-gl: the OpenGL Utility Library's image and coordinate functions.
 *
 * GLU is not part of GL, but a port that does not find it does not build. `gluPerspective`,
 * `gluLookAt`, `gluOrtho2D` and `gluPickMatrix` live in gl_matrix.c, beside the matrix stack they
 * multiply onto; these are the rest of what GL 1.x code actually calls:
 *
 * - `gluBuild2DMipmaps` and `gluBuild1DMipmaps` - how texture loading was written before
 *   GL_GENERATE_MIPMAP (1.4) and glGenerateMipmap (3.0) existed, which is to say in most of the
 *   code being ported;
 * - `gluScaleImage`, which they are built on and which callers use directly to fit an image to
 *   GL_MAX_TEXTURE_SIZE;
 * - `gluProject` and `gluUnProject`, for picking and for placing a label over a point;
 * - `gluGetString`.
 *
 * **The filter is a box**, as SGI's GLU is: an output pixel is the average of the input pixels
 * its footprint covers, which is what makes a minified mipmap level look like the image above it
 * rather than a sparse sample of it. Magnification by this rule replicates, which is what GLU
 * does too.
 *
 * **Errors are GLU's, not GL's**: a GLU function returns 0 for success or a `GLU_*` code, and
 * records nothing in `glGetError`. `gluErrorString` (gl_matrix.c) names them.
 */
#include "GL/glu.h"
#include "gl_internal.h"

#ifdef OOPS_HOST_BUILD
#include <stdlib.h>
#else
#include "oops/heap.h"
#endif

/* The allocator differs by build, as gl_list_alloc's does: the SDK's own heap on the target,
 * the C library's on the host. A mipmap chain allocates one level at a time. */
static void *glu_alloc(size_t bytes) {
#ifdef OOPS_HOST_BUILD
    return malloc(bytes);
#else
    return oops_malloc(bytes);
#endif
}

static void glu_release(void *p) {
    if (!p) return;
#ifdef OOPS_HOST_BUILD
    free(p);
#else
    oops_free(p);
#endif
}

/* gl_matrix.c keeps its own copy for the same reason: this is freestanding, and the value is
 * not worth a header of its own. */
#define GLU_PI 3.14159265358979323846f

/* The SDK is freestanding - there is no libc math here, and this needs one rounding rule.
 * Every value it is given is a pixel coordinate, so it is never negative. */
static int glu_ceil_i(double v) {
    const int t = (int)v;
    return (v > (double)t) ? t + 1 : t;
}

/* ---------------------------------------------------------------------------
 * The box filter
 *
 * Both directions in one routine, over tightly packed rows of a caller's choosing: `gluScaleImage`
 * gives it the pixel-store state's strides, the mipmap builder its own packed ones. Nothing here
 * reads the GL context.
 * --------------------------------------------------------------------------- */

/* One output pixel: the average of the source pixels whose centres fall inside its footprint.
 * The footprint is at least one pixel wide, so magnification takes the single pixel under the
 * output's centre - GLU's replication. */
static void glu_box_pixel(const gl_pixel_fmt_t *f, const uint8_t *src, size_t src_row,
                          GLsizei w_in, GLsizei h_in, double x0, double x1, double y0, double y1,
                          float out[4]) {
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
            gl_unpack_pixel_f(f, row + (size_t)x * f->pixel_bytes, GL_FALSE, p);
            for (int c = 0; c < 4; c++) sum[c] += p[c];
            n++;
        }
    }
    if (n == 0u) n = 1u;
    for (int c = 0; c < 4; c++) out[c] = sum[c] / (float)n;
}

/* A whole image, source strides and destination strides given. */
static void glu_box_scale(const gl_pixel_fmt_t *f, const uint8_t *src, size_t src_row,
                          GLsizei w_in, GLsizei h_in, uint8_t *dst, size_t dst_row, GLsizei w_out,
                          GLsizei h_out) {
    const double sx = (double)w_in / (double)w_out, sy = (double)h_in / (double)h_out;
    for (GLsizei y = 0; y < h_out; y++) {
        uint8_t *drow = dst + (size_t)y * dst_row;
        for (GLsizei x = 0; x < w_out; x++) {
            float rgba[4];
            glu_box_pixel(f, src, src_row, w_in, h_in, (double)x * sx, (double)(x + 1) * sx,
                          (double)y * sy, (double)(y + 1) * sy, rgba);
            /* A texture read back, not a colour buffer: luminance is R, as gl_pack_pixel_f's
             * `lum_sum` GL_FALSE means. */
            gl_pack_pixel_f(f, rgba, GL_FALSE, GL_FALSE, drow + (size_t)x * f->pixel_bytes);
        }
    }
}

/* What this can scale: a colour format with one byte-addressable pixel. GL_BITMAP has no pixel
 * to average, and the packed types would have to be unpacked and repacked through their own
 * component order - which gl_pixel.c does, but for which GLU has no caller worth the weight. */
static GLint glu_fmt(GLenum format, GLenum type, gl_pixel_fmt_t *f) {
    if (gl_pixel_fmt(format, type, f) != GL_NO_ERROR) return GLU_INVALID_ENUM;
    if (f->kind != GL_COLOR || f->bitmap || f->pixel_bytes == 0u) return GLU_INVALID_ENUM;
    return 0;
}

GLint gluScaleImage(GLenum format, GLsizei wIn, GLsizei hIn, GLenum typeIn, const void *dataIn,
                    GLsizei wOut, GLsizei hOut, GLenum typeOut, void *dataOut) {
    if (wIn <= 0 || hIn <= 0 || wOut <= 0 || hOut <= 0) return GLU_INVALID_VALUE;
    if (!dataIn || !dataOut) return GLU_INVALID_VALUE;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GLU_INVALID_OPERATION;
    gl_pixel_fmt_t fin, fout;
    GLint e = glu_fmt(format, typeIn, &fin);
    if (e) return e;
    if ((e = glu_fmt(format, typeOut, &fout)) != 0) return e;

    /* The pixel-store state applies to both ends, as the specification says: the source is read
     * through GL_UNPACK_*, the destination written through GL_PACK_*. */
    gl_pixel_src_t src;
    gl_pixel_dst_t dst;
    gl_unpack_source(ctx, &fin, dataIn, wIn, hIn, &src);
    gl_pack_dest(ctx, &fout, dataOut, wOut, hOut, &dst);
    if (!src.base || !dst.base) return GLU_INVALID_VALUE;

    /* One format in, another out, so the average is taken in the source's format and written in
     * the destination's - which is why this is not glu_box_scale on its own. */
    const double sx = (double)wIn / (double)wOut, sy = (double)hIn / (double)hOut;
    for (GLsizei y = 0; y < hOut; y++) {
        uint8_t *drow = dst.base + (size_t)y * dst.row_stride;
        for (GLsizei x = 0; x < wOut; x++) {
            float rgba[4];
            glu_box_pixel(&fin, src.base, src.row_stride, wIn, hIn, (double)x * sx,
                          (double)(x + 1) * sx, (double)y * sy, (double)(y + 1) * sy, rgba);
            gl_pack_pixel_f(&fout, rgba, GL_FALSE, dst.swap, drow + (size_t)x * fout.pixel_bytes);
        }
    }
    (void)src.swap;
    return 0;
}

/* ---------------------------------------------------------------------------
 * The mipmap builders
 * --------------------------------------------------------------------------- */

/* The power of two nearest a dimension, as GLU picks it, clamped to what this implementation
 * will accept. A texture whose sides are already powers of two is untouched, which is the case
 * every port hits. */
static GLsizei glu_pot(GLsizei n) {
    if (n <= 1) return 1;
    GLsizei lo = 1;
    while (lo * 2 < n && lo < OOPS_GL_MAX_TEXTURE_SIZE) lo *= 2;
    const GLsizei hi = (lo < OOPS_GL_MAX_TEXTURE_SIZE) ? lo * 2 : lo;
    /* Nearest, ties down - the smaller image is the safer one to have chosen. */
    const GLsizei n_lo = n - lo, n_hi = hi - n;
    GLsizei r = (n_hi < n_lo) ? hi : lo;
    if (r > OOPS_GL_MAX_TEXTURE_SIZE) r = OOPS_GL_MAX_TEXTURE_SIZE;
    return r;
}

/* The pixel-store state GLU needs while it uploads its own tightly packed levels, and the
 * caller's state put back afterwards. The caller may have set a row length for the image it
 * passed; the levels below level zero are this library's own buffers, packed tight. */
typedef struct {
    GLint row_length, skip_rows, skip_pixels, alignment;
} glu_unpack_save_t;

static void glu_unpack_tight(glu_unpack_save_t *s) {
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &s->row_length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &s->skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &s->skip_pixels);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

static void glu_unpack_restore(const glu_unpack_save_t *s) {
    glPixelStorei(GL_UNPACK_ROW_LENGTH, s->row_length);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, s->skip_rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, s->skip_pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, s->alignment);
}

/*
 * Both builders are one routine: a 1D image is a 2D image one row tall, and glTexImage1D takes
 * the width alone. Level zero is the caller's image scaled to powers of two if it is not already,
 * and each level after it is the one above halved - so the whole chain is built from the level
 * above rather than from the original, which is what makes each level the average of the last.
 */
static GLint glu_build_mipmaps(GLenum target, GLint internalFormat, GLsizei width, GLsizei height,
                               GLenum format, GLenum type, const void *data, GLboolean one_d) {
    if (width <= 0 || height <= 0) return GLU_INVALID_VALUE;
    if (!data) return GLU_INVALID_VALUE;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return GLU_INVALID_OPERATION;
    gl_pixel_fmt_t f;
    const GLint e = glu_fmt(format, type, &f);
    if (e) return e;

    GLsizei w = glu_pot(width), h = one_d ? 1 : glu_pot(height);
    uint8_t *level = (uint8_t *)glu_alloc((size_t)w * (size_t)h * f.pixel_bytes);
    if (!level) return GLU_OUT_OF_MEMORY;

    /* Level zero, through the caller's unpack state - this is their image. */
    gl_pixel_src_t src;
    gl_unpack_source(ctx, &f, data, width, height, &src);
    if (!src.base) {
        glu_release(level);
        return GLU_INVALID_VALUE;
    }
    glu_box_scale(&f, src.base, src.row_stride, width, height, level,
                  (size_t)w * f.pixel_bytes, w, h);

    glu_unpack_save_t save;
    glu_unpack_tight(&save);
    GLint lvl = 0;
    for (;;) {
        if (one_d) {
            glTexImage1D(target, lvl, internalFormat, w, 0, format, type, level);
        } else {
            glTexImage2D(target, lvl, internalFormat, w, h, 0, format, type, level);
        }
        if (w == 1 && h == 1) break;
        const GLsizei nw = (w > 1) ? w / 2 : 1, nh = (h > 1) ? h / 2 : 1;
        uint8_t *next = (uint8_t *)glu_alloc((size_t)nw * (size_t)nh * f.pixel_bytes);
        if (!next) {
            glu_release(level);
            glu_unpack_restore(&save);
            return GLU_OUT_OF_MEMORY;
        }
        glu_box_scale(&f, level, (size_t)w * f.pixel_bytes, w, h, next,
                      (size_t)nw * f.pixel_bytes, nw, nh);
        glu_release(level);
        level = next;
        w = nw;
        h = nh;
        lvl++;
    }
    glu_release(level);
    glu_unpack_restore(&save);
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

/* ---------------------------------------------------------------------------
 * Object space to the window and back
 * --------------------------------------------------------------------------- */

/* Column-major, as GL's matrices are: m[c * 4 + r]. */
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

/* A general 4x4 inverse by cofactors. GL_FALSE when the matrix is singular, which is what
 * gluUnProject answers rather than dividing by zero. */
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
    /* Normalised device coordinates, then the viewport and the depth range's default 0..1 -
     * which is what GLU maps to, taking no account of glDepthRange, as the specification says. */
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
 *
 * `gluSphere`, `gluCylinder` and `gluDisk` are how a program written against GL 1.x draws a ball,
 * a tube or a ring without carrying a mesh - the shapes half the tutorials in the world are built
 * from, and so half the code being ported.
 *
 * **The conventions are the specification's, not a choice made here**, because a texture that
 * comes out rotated or a surface that disappears under back-face culling is the kind of wrong a
 * port cannot see in the source:
 *
 * - a sphere and a cylinder are subdivided around the z axis into `slices` and along it into
 *   `stacks`; a disk lies in z = 0, divided into `slices` and `loops`;
 * - `s` runs 0 at the +y axis, 0.25 at +x, 0.5 at -y, 0.75 at -x and back to 1 at +y - so the
 *   angle is measured from +y towards +x, which is clockwise seen from +z;
 * - a sphere's `t` is 0 at z = -radius and 1 at z = +radius; a cylinder's is 0 at z = 0 and 1 at
 *   z = height; a disk's (s, t) is (1, 0.5) at (outer, 0, 0) and (0.5, 1) at (0, outer, 0);
 * - the cylinder's base is at z = 0 and its top at z = height, and neither end is capped;
 * - GLU_OUTSIDE points the normals away from the axis (the +z face, for a disk), GLU_INSIDE
 *   towards it - **and the winding follows the normals**, so a GLU_OUTSIDE surface is
 *   counter-clockwise seen from outside and survives the default `glCullFace` setup.
 *
 * Normals are unit length, which the specification does not require but every lighting setup
 * assumes; a cone's are the true surface normals, tilted by its slope, not the cylinder normals
 * a sloped side would otherwise get.
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

void gluDeleteQuadric(GLUquadric *q) { glu_release(q); }

/* A bad enumerant reaches the error callback if one is set, and is otherwise ignored - GLU has
 * no error to record, and a quadric that drew nothing would be harder to find than one that kept
 * its last good setting. */
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

/* +1 for GLU_OUTSIDE, -1 for GLU_INSIDE: the sign on every normal, and what decides which way
 * round a strip's vertices go. */
static float glu_sign(const GLUquadric *q) {
    return (q->orientation == GLU_INSIDE) ? -1.0f : 1.0f;
}

static void glu_normal(const GLUquadric *q, float x, float y, float z) {
    if (q->normals == GLU_NONE) return;
    const float s = glu_sign(q);
    glNormal3f(x * s, y * s, z * s);
}

/* The primitive a strip of the surface is drawn with. GLU_POINT and the two line styles draw the
 * same vertices; only what joins them differs, so one switch serves every shape. */
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

    /* The poles are drawn as triangle fans and the bands between them as quad strips, which is
     * how GLU does it: a band that reached a pole would be a strip of quads with two corners in
     * the same place - degenerate triangles, thrown away after being transformed. */
    for (GLint cap = 0; cap < 2 && stacks >= 2; cap++) {
        const GLboolean north = (GLboolean)(cap == 0);
        const float rho = north ? drho : (GLU_PI - drho);
        const float sr = gl_sin(rho), cr = gl_cos(rho);
        const float pz = north ? 1.0f : -1.0f;
        const float t_pole = north ? 1.0f : 0.0f;
        const float t_ring = north ? (1.0f - 1.0f / (float)stacks) : (1.0f / (float)stacks);
        /* Seen from outside the north pole, increasing theta runs clockwise - s goes +y, +x,
         * -y - so the fan is wound the other way round there. The south pole is its mirror, and
         * GLU_INSIDE reverses both. */
        const GLboolean backwards = (GLboolean)(north != (GLboolean)(inside != GL_FALSE));
        glBegin(mode == GL_QUAD_STRIP ? GL_TRIANGLE_FAN : mode);
        glu_normal(q, 0.0f, 0.0f, pz);
        if (q->texture) glTexCoord2f(0.5f, t_pole);
        glVertex3f(0.0f, 0.0f, pz * r);
        for (GLint k = 0; k <= slices; k++) {
            const GLint j = backwards ? (slices - k) : k;
            const float theta = (float)(j % slices) * dtheta;
            const float st = gl_sin(theta), ct = gl_cos(theta);
            const float nx = st * sr, ny = ct * sr, nz = cr;
            glu_normal(q, nx, ny, nz);
            if (q->texture) glTexCoord2f((float)j / (float)slices, t_ring);
            glVertex3f(nx * r, ny * r, nz * r);
        }
        glEnd();
    }
    /* The bands between the two polar rings. A sphere of two stacks is its caps alone, so this
     * runs no times; one stack has no caps at all, and is the single band it asked for. */
    const GLint band_first = (stacks >= 2) ? 1 : 0;
    const GLint band_last = (stacks >= 2) ? stacks - 1 : stacks;
    for (GLint i = band_first; i < band_last; i++) {
        /* Two rings: the one nearer +z and the one below it. They are emitted lower-first for
         * GLU_OUTSIDE, which is what makes the strip counter-clockwise seen from outside. */
        const float rho_hi = (float)i * drho, rho_lo = (float)(i + 1) * drho;
        const float t_hi = 1.0f - (float)i / (float)stacks;
        const float t_lo = 1.0f - (float)(i + 1) / (float)stacks;
        glBegin(mode);
        for (GLint j = 0; j <= slices; j++) {
            const float theta = (j == slices) ? 0.0f : (float)j * dtheta;
            const float st = gl_sin(theta), ct = gl_cos(theta);
            const float s = (float)j / (float)slices;
            for (int e = 0; e < 2; e++) {
                /* e = 0 is the vertex emitted first: the lower ring, unless inside out. */
                const int lower = inside ? (e == 1) : (e == 0);
                const float rho = lower ? rho_lo : rho_hi;
                const float tc = lower ? t_lo : t_hi;
                const float sr = gl_sin(rho), cr = gl_cos(rho);
                const float nx = st * sr, ny = ct * sr, nz = cr;
                glu_normal(q, nx, ny, nz);
                if (q->texture) glTexCoord2f(s, tc);
                glVertex3f(nx * r, ny * r, nz * r);
            }
        }
        glEnd();
    }
    /* The lines along the slices, which the strips above do not draw. A silhouette leaves them
     * out: they separate faces of the same band, which the specification's rule excludes. */
    if (q->draw_style == GLU_LINE) {
        for (GLint j = 0; j < slices; j++) {
            const float theta = (float)j * dtheta;
            const float st = gl_sin(theta), ct = gl_cos(theta);
            glBegin(GL_LINE_STRIP);
            for (GLint i = 0; i <= stacks; i++) {
                const float rho = (float)i * drho;
                const float sr = gl_sin(rho), cr = gl_cos(rho);
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
    /* The side's slope: the normal leans by (base - top) / height, and is then made unit length.
     * A cone drawn with cylinder normals lights as if its side were vertical. */
    float nz = (hh > 0.0f) ? (rb - rt) / hh : 0.0f;
    const float nlen = gl_sqrt(1.0f + nz * nz);
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
            const float st = gl_sin(theta), ct = gl_cos(theta);
            const float s = (float)j / (float)slices;
            for (int e = 0; e < 2; e++) {
                /* The upper ring first for GLU_OUTSIDE - the same reasoning as the sphere's. */
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
            const float st = gl_sin(theta), ct = gl_cos(theta);
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

/* gluDisk and gluPartialDisk in one: a full disk is a partial one swept 360 degrees from 0.
 * Angles are the specification's - degrees, 0 along +y, increasing towards +x. */
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
            const float st = gl_sin(theta), ct = gl_cos(theta);
            for (int e = 0; e < 2; e++) {
                /* The inner ring first when the +z face is out, so the strip winds
                 * counter-clockwise seen from +z. */
                const int in_ring = inside ? (e == 1) : (e == 0);
                const float rr = in_ring ? r0 : r1;
                const float x = st * rr, y = ct * rr;
                /* The normal is the face's own; glu_normal's sign would flip it a second time. */
                if (q->normals != GLU_NONE) glNormal3f(0.0f, 0.0f, nz);
                if (q->texture) glTexCoord2f(x / ro * 0.5f + 0.5f, y / ro * 0.5f + 0.5f);
                glVertex3f(x, y, 0.0f);
            }
        }
        glEnd();
    }
    /* The radial lines. A silhouette leaves out those between loops - coplanar faces - and keeps
     * the two edges of a partial disk's sweep, which bound the surface. */
    if (q->draw_style == GLU_LINE || q->draw_style == GLU_SILHOUETTE) {
        const GLboolean partial = (GLboolean)(sweep < 360.0 && sweep > -360.0);
        const GLint step = (q->draw_style == GLU_LINE) ? 1 : slices;
        if (q->draw_style == GLU_LINE || partial) {
            for (GLint j = 0; j <= slices; j += step) {
                const float theta = a0 + (float)j * dtheta;
                const float st = gl_sin(theta), ct = gl_cos(theta);
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

const GLubyte *gluGetString(GLenum name) {
    switch (name) {
        /* The version of GLU whose functions this provides. The quadrics, the tessellator and the
         * NURBS interfaces are not here; a program that needs them will not link, which is a
         * better answer than a stub that draws nothing. */
        case GLU_VERSION:    return (const GLubyte *)"1.3";
        case GLU_EXTENSIONS: return (const GLubyte *)"";
        default:             return (const GLubyte *)0;
    }
}
