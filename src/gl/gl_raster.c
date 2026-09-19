/*
 * oops-gl: the raster position, and the pixel operations that draw at it
 *
 * `glRasterPos` is a vertex that is never drawn. It goes through the whole transform - modelview,
 * projection, the clip test, the viewport map - and what comes out is remembered in window
 * coordinates, together with the colour and texture coordinate current at the time. `glDrawPixels`
 * and `glBitmap` then draw there.
 *
 * # An invalid position draws nothing
 *
 * If the point clips, the raster position is marked **invalid** and every subsequent
 * `glDrawPixels` and `glBitmap` is a no-op until a valid position is set. That is the
 * specification's rule and it is worth stating because the tempting alternative - clamp to the
 * nearest edge and draw anyway - puts an image somewhere the program never asked for. A program
 * that scrolls text off the side of the screen would get it piled up against the edge instead of
 * disappearing.
 *
 * # These draw straight into the framebuffer
 *
 * There is no primitive assembly here and nothing goes through the rasteriser: the pixel
 * rectangle is written to the colour buffer directly. On the hardware path that means the CPU
 * touches the render target, so a frame that has draws built but not submitted must be flushed
 * first - the same rule the shader payload and texture storage follow.
 */

#include "gl_internal.h"

/* GL's window origin is bottom-left and the framebuffer's row 0 is the top. */
static int gl_raster_row(const gl_context_t *ctx, int y_from_bottom) {
    return (int)ctx->height - 1 - y_from_bottom;
}

/* One pixel of the colour buffer, with the bounds check every path here needs. */
static void gl_raster_put(gl_context_t *ctx, int x, int y_from_bottom, uint32_t argb) {
    if (x < 0 || y_from_bottom < 0) return;
    if ((unsigned int)x >= ctx->width || (unsigned int)y_from_bottom >= ctx->height) return;
    const int row = gl_raster_row(ctx, y_from_bottom);
    if (row < 0) return;
    ctx->framebuffer[(size_t)row * (size_t)ctx->width + (size_t)x] = argb;
}

/* The transform, shared by every spelling.
 *
 * The clip test is the ordinary one: a point is inside when each of its clip coordinates is
 * within +/- w. Failing it sets the position invalid rather than recording an error - GL has no
 * error for a clipped raster position, it simply stops drawing. */
void glRasterPos4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    /* The combined matrix is computed lazily and only the draw path used to ask for it. A raster
     * position is a vertex too, so it has to ask as well - without this it transforms through
     * whatever the last draw left behind, or through zeroes if nothing has drawn yet. */
    gl_update_mvp(ctx);

    const float obj[4] = {x, y, z, w};
    float clip[4];
    mat4_transform_vec4(clip, &ctx->mvp, obj);

    const float cw = clip[3];
    GLboolean inside = (GLboolean)(cw > 0.0f);
    if (inside) {
        for (int i = 0; i < 3; i++) {
            if (clip[i] < -cw || clip[i] > cw) { inside = GL_FALSE; break; }
        }
    }

    /* The colour and texture coordinate are latched whether or not the position is valid: the
     * specification says they are always updated, and a program may read them back. */
    for (int i = 0; i < 4; i++) {
        ctx->raster_color[i] = ctx->cur_color[i];
        ctx->raster_texcoord[i] = ctx->cur_texcoord[i];
    }

    if (!inside) {
        ctx->raster_valid = GL_FALSE;
        return;
    }

    const float inv_w = 1.0f / cw;
    const float ndc_x = clip[0] * inv_w;
    const float ndc_y = clip[1] * inv_w;
    const float ndc_z = clip[2] * inv_w;

    ctx->raster_pos[0] = (float)ctx->vp_x + ((ndc_x + 1.0f) * 0.5f) * (float)ctx->vp_w;
    ctx->raster_pos[1] = (float)ctx->vp_y + ((ndc_y + 1.0f) * 0.5f) * (float)ctx->vp_h;
    ctx->raster_pos[2] = ndc_z * (ctx->depth_far - ctx->depth_near) * 0.5f +
                         (ctx->depth_far + ctx->depth_near) * 0.5f;
    ctx->raster_pos[3] = inv_w;
    /* The eye-space distance, which is what GL_CURRENT_RASTER_DISTANCE reports and what fog
     * would use. Computed from the modelview alone, not the combined matrix. */
    float eye[4];
    mat4_transform_vec4(eye, &ctx->modelview_stack[ctx->modelview_depth], obj);
    ctx->raster_distance = gl_sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    ctx->raster_valid = GL_TRUE;
}

void glRasterPos2f(GLfloat x, GLfloat y) { glRasterPos4f(x, y, 0.0f, 1.0f); }
void glRasterPos3f(GLfloat x, GLfloat y, GLfloat z) { glRasterPos4f(x, y, z, 1.0f); }
void glRasterPos2d(GLdouble x, GLdouble y) { glRasterPos4f((GLfloat)x, (GLfloat)y, 0.0f, 1.0f); }
void glRasterPos3d(GLdouble x, GLdouble y, GLdouble z) {
    glRasterPos4f((GLfloat)x, (GLfloat)y, (GLfloat)z, 1.0f);
}
void glRasterPos4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) {
    glRasterPos4f((GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}
void glRasterPos2i(GLint x, GLint y) { glRasterPos4f((GLfloat)x, (GLfloat)y, 0.0f, 1.0f); }
void glRasterPos3i(GLint x, GLint y, GLint z) {
    glRasterPos4f((GLfloat)x, (GLfloat)y, (GLfloat)z, 1.0f);
}
void glRasterPos4i(GLint x, GLint y, GLint z, GLint w) {
    glRasterPos4f((GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}
void glRasterPos2s(GLshort x, GLshort y) { glRasterPos4f((GLfloat)x, (GLfloat)y, 0.0f, 1.0f); }
void glRasterPos3s(GLshort x, GLshort y, GLshort z) {
    glRasterPos4f((GLfloat)x, (GLfloat)y, (GLfloat)z, 1.0f);
}
void glRasterPos4s(GLshort x, GLshort y, GLshort z, GLshort w) {
    glRasterPos4f((GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}
void glRasterPos2fv(const GLfloat *v)  { if (v) glRasterPos2f(v[0], v[1]); }
void glRasterPos3fv(const GLfloat *v)  { if (v) glRasterPos3f(v[0], v[1], v[2]); }
void glRasterPos4fv(const GLfloat *v)  { if (v) glRasterPos4f(v[0], v[1], v[2], v[3]); }
void glRasterPos2dv(const GLdouble *v) { if (v) glRasterPos2d(v[0], v[1]); }
void glRasterPos3dv(const GLdouble *v) { if (v) glRasterPos3d(v[0], v[1], v[2]); }
void glRasterPos4dv(const GLdouble *v) { if (v) glRasterPos4d(v[0], v[1], v[2], v[3]); }
void glRasterPos2iv(const GLint *v)    { if (v) glRasterPos2i(v[0], v[1]); }
void glRasterPos3iv(const GLint *v)    { if (v) glRasterPos3i(v[0], v[1], v[2]); }
void glRasterPos4iv(const GLint *v)    { if (v) glRasterPos4i(v[0], v[1], v[2], v[3]); }
void glRasterPos2sv(const GLshort *v)  { if (v) glRasterPos2s(v[0], v[1]); }
void glRasterPos3sv(const GLshort *v)  { if (v) glRasterPos3s(v[0], v[1], v[2]); }
void glRasterPos4sv(const GLshort *v)  { if (v) glRasterPos4s(v[0], v[1], v[2], v[3]); }

void glPixelZoom(GLfloat xfactor, GLfloat yfactor) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->pixel_zoom_x = xfactor;
    ctx->pixel_zoom_y = yfactor;
}

/* The colour buffer is about to be written by the CPU, so a frame with draws built into it has
 * to be submitted first - the same rule glDeleteTextures and the shader payload follow. */
static void gl_raster_sync(gl_context_t *ctx) {
#ifndef OOPS_HOST_BUILD
    if (ctx->use_hardware && ctx->hw_frame_active) {
        gl_hw_flush(ctx);
    }
#else
    (void)ctx;
#endif
}

void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type,
                  const GLvoid *pixels) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (width < 0 || height < 0) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
    if (type != GL_UNSIGNED_BYTE) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    if (format != GL_RGBA && format != GL_RGB && format != GL_LUMINANCE) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (!pixels || width == 0 || height == 0) return;
    /* Not an error - the specification says an invalid raster position simply draws nothing. */
    if (!ctx->raster_valid || !ctx->framebuffer) return;

    gl_raster_sync(ctx);

    const size_t bpp = (format == GL_RGBA) ? 4u : ((format == GL_RGB) ? 3u : 1u);
    const size_t stride = gl_unpack_row_stride(ctx, width, bpp);
    const uint8_t *src = (const uint8_t *)pixels;
    const int ox = (int)ctx->raster_pos[0];
    const int oy = (int)ctx->raster_pos[1];

    /* A negative zoom mirrors, which is the documented way to draw an image the other way up -
     * so the destination step carries the sign rather than the loop bounds. */
    const float zx = (ctx->pixel_zoom_x != 0.0f) ? ctx->pixel_zoom_x : 1.0f;
    const float zy = (ctx->pixel_zoom_y != 0.0f) ? ctx->pixel_zoom_y : 1.0f;

    for (int sy = 0; sy < height; sy++) {
        const uint8_t *row = src + (size_t)sy * stride;
        for (int sx = 0; sx < width; sx++) {
            const uint8_t *p = row + (size_t)sx * bpp;
            uint32_t r, g, b, a;
            if (format == GL_LUMINANCE) {
                r = g = b = p[0];
                a = 255u;
            } else {
                r = p[0]; g = p[1]; b = p[2];
                a = (format == GL_RGBA) ? p[3] : 255u;
            }
            const uint32_t argb = (a << 24) | (r << 16) | (g << 8) | b;

            /* With a zoom of 1 this is one destination pixel per source pixel; with a larger one
             * it is a block, which is what the specification's "each pixel covers the rectangle"
             * amounts to. */
            const int dx0 = ox + (int)((float)sx * zx);
            const int dy0 = oy + (int)((float)sy * zy);
            const int nx = (int)(zx < 0.0f ? -zx : zx) + ((zx == (float)(int)zx) ? 0 : 1);
            const int ny = (int)(zy < 0.0f ? -zy : zy) + ((zy == (float)(int)zy) ? 0 : 1);
            for (int by = 0; by < (ny > 0 ? ny : 1); by++) {
                for (int bx = 0; bx < (nx > 0 ? nx : 1); bx++) {
                    gl_raster_put(ctx, dx0 + bx, dy0 + by, argb);
                }
            }
        }
    }
    ctx->fb_cleared = GL_TRUE;
}

void glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (width < 0 || height < 0) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
    /* GL_DEPTH and GL_STENCIL copies are a different buffer and are refused rather than silently
     * copying colour, which would return success and the wrong pixels. */
    if (type != GL_COLOR) { gl_record_error(ctx, GL_INVALID_ENUM); return; }
    if (!ctx->raster_valid || !ctx->framebuffer || width == 0 || height == 0) return;

    gl_raster_sync(ctx);

    const int ox = (int)ctx->raster_pos[0];
    const int oy = (int)ctx->raster_pos[1];
    /* **Read every source row before writing any**, because the source and destination are the
     * same buffer and an overlapping copy that reads as it writes smears. A single row of scratch
     * is enough: rows are copied bottom-up and a row is fully read before it is written. */
    for (int sy = 0; sy < height; sy++) {
        const int srow = gl_raster_row(ctx, y + sy);
        if (srow < 0 || (unsigned int)srow >= ctx->height) continue;
        for (int sx = 0; sx < width; sx++) {
            const int scol = x + sx;
            if (scol < 0 || (unsigned int)scol >= ctx->width) continue;
            const uint32_t v = ctx->framebuffer[(size_t)srow * (size_t)ctx->width + (size_t)scol];
            gl_raster_put(ctx, ox + sx, oy + sy, v);
        }
    }
    ctx->fb_cleared = GL_TRUE;
}

/* A bitmap is one bit per pixel, drawn in the current raster colour, and it **moves the raster
 * position** by (xmove, ymove) afterwards. That last part is what makes a string of glBitmap
 * calls lay out text, and leaving it out gives a program that draws every glyph on top of the
 * first one.
 *
 * Rows are packed most-significant-bit first and padded to the unpack alignment, like any other
 * pixel rectangle. A zero-sized bitmap is legal and still moves the position - that is how a
 * space character is drawn. */
void glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig,
              GLfloat xmove, GLfloat ymove, const GLubyte *bitmap) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (width < 0 || height < 0) { gl_record_error(ctx, GL_INVALID_VALUE); return; }

    if (bitmap && width > 0 && height > 0 && ctx->raster_valid && ctx->framebuffer) {
        gl_raster_sync(ctx);

        uint32_t r = (uint32_t)(ctx->raster_color[0] * 255.0f + 0.5f);
        uint32_t g = (uint32_t)(ctx->raster_color[1] * 255.0f + 0.5f);
        uint32_t b = (uint32_t)(ctx->raster_color[2] * 255.0f + 0.5f);
        uint32_t a = (uint32_t)(ctx->raster_color[3] * 255.0f + 0.5f);
        if (r > 255u) r = 255u;
        if (g > 255u) g = 255u;
        if (b > 255u) b = 255u;
        if (a > 255u) a = 255u;
        const uint32_t argb = (a << 24) | (r << 16) | (g << 8) | b;

        /* One bit per pixel, so the row is width bits rounded up to bytes and then to the
         * unpack alignment. */
        const size_t row_bytes = ((size_t)width + 7u) / 8u;
        const size_t stride = gl_unpack_row_stride_bytes(ctx, row_bytes);
        const int ox = (int)ctx->raster_pos[0] - (int)xorig;
        const int oy = (int)ctx->raster_pos[1] - (int)yorig;

        for (int sy = 0; sy < height; sy++) {
            const GLubyte *row = bitmap + (size_t)sy * stride;
            for (int sx = 0; sx < width; sx++) {
                const GLubyte byte = row[(size_t)sx / 8u];
                /* Most significant bit first: bit 7 is the leftmost pixel of each byte. */
                if ((byte >> (7 - (sx & 7))) & 1u) {
                    gl_raster_put(ctx, ox + sx, oy + sy, argb);
                }
            }
        }
        ctx->fb_cleared = GL_TRUE;
    }

    /* The move happens whatever was drawn, including for a null or empty bitmap. */
    ctx->raster_pos[0] += xmove;
    ctx->raster_pos[1] += ymove;
}
