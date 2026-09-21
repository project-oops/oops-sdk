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
 * # Their pixels are fragments
 *
 * There is no primitive assembly here and nothing goes through the triangle rasteriser, but each
 * pixel of a rectangle is a **fragment**, and it meets what any fragment does - the texture
 * environment, fog, the scissor, alpha, stencil and depth tests, blending, the logic op and the
 * masks - through gl_pixel_fragment, the triangle rasteriser's own tail. (Until 2026-09-19 they
 * were written into the colour buffer directly, past all of it.) On the hardware path that means
 * the CPU touches the render target, so a frame that has draws built but not submitted must be
 * flushed first - the same rule the shader payload and texture storage follow.
 */

#include "gl_internal.h"

#ifdef OOPS_HOST_BUILD
#include <stdlib.h>
#endif

/* GL's window origin is bottom-left and the framebuffer's row 0 is the top. */
static int gl_raster_row(const gl_context_t *ctx, int y_from_bottom) {
    return (int)ctx->height - 1 - y_from_bottom;
}

/* Scratch memory for a pixel operation that has to hold a rectangle - glCopyPixels' source. */
static void *gl_raster_scratch(size_t bytes) {
#ifdef OOPS_HOST_BUILD
    return malloc(bytes ? bytes : 1u);
#else
    return oops_malloc(bytes ? bytes : 1u);
#endif
}

static void gl_raster_scratch_free(void *p) {
#ifdef OOPS_HOST_BUILD
    free(p);
#else
    oops_free(p);
#endif
}

static int gl_ceil_i(float v) {
    const int i = (int)v;
    return ((float)i < v) ? i + 1 : i;
}

/* **The window columns (or rows) pixel `n` of a rectangle covers**, zoomed by `z` from the raster
 * coordinate `r`: those whose centres lie in [r + z n, r + z (n + 1)), GL 1.x 3.6.5's rule. A zoom
 * of 1 is one fragment a pixel, a larger one a block, a fractional one sometimes none, and a
 * negative one a mirror. `*lo > *hi` when it covers none. This truncated the raster position and
 * stepped by whole pixels until 2026-09-19, which put a mirrored image one column over. */
static void gl_zoom_span(float r, float z, int n, int *lo, int *hi) {
    float a = r + z * (float)n, b = r + z * (float)(n + 1);
    if (a > b) { const float t = a; a = b; b = t; }
    *lo = gl_ceil_i(a - 0.5f);
    *hi = gl_ceil_i(b - 0.5f) - 1;
}

/* One pixel of a rectangle, as the fragments its zoomed block covers. */
static void gl_zoomed_fragments(gl_context_t *ctx, const gl_pixel_frags_t *pf, int sx, int sy,
                                const float c[4]) {
    int x0, x1, y0, y1;
    gl_zoom_span(ctx->raster_pos[0], ctx->pixel_zoom_x, sx, &x0, &x1);
    gl_zoom_span(ctx->raster_pos[1], ctx->pixel_zoom_y, sy, &y0, &y1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) gl_pixel_fragment(ctx, pf, x, y, c);
    }
}

/* One stencil index of a rectangle, into the stencil buffer across its zoomed block. A stencil
 * rectangle is not a set of fragments: its values are written directly, through the scissor and
 * GL_STENCIL_WRITEMASK and nothing else (GL 1.x, 4.3.1; Mesa swrast's draw_stencil_pixels writes
 * the span through the mask). The stencil buffer is the CPU's on both paths. */
static void gl_zoomed_stencil(gl_context_t *ctx, const gl_pixel_frags_t *pf, int sx, int sy,
                              int64_t s) {
    uint8_t *sb = ctx->stencil_buffer;
    if (!sb) return;
    int x0, x1, y0, y1;
    gl_zoom_span(ctx->raster_pos[0], ctx->pixel_zoom_x, sx, &x0, &x1);
    gl_zoom_span(ctx->raster_pos[1], ctx->pixel_zoom_y, sy, &y0, &y1);
    const uint8_t m = (uint8_t)(ctx->stencil_writemask & 0xffu);
    for (int y = y0; y <= y1; y++) {
        if (y < pf->sc_y0 || y > pf->sc_y1) continue;
        for (int x = x0; x <= x1; x++) {
            if (x < pf->sc_x0 || x > pf->sc_x1) continue;
            uint8_t *sp = gl_zs_stencil_ptr(ctx, x, y);
            *sp = (uint8_t)(((uint8_t)s & m) | (*sp & (uint8_t)~m));
        }
    }
}

/* The raster colour as a fragment's: what a depth rectangle's fragments are coloured, clamped as
 * glBitmap's are - the raster position keeps it unclamped when unlit. */
static void gl_raster_frag_colour(const gl_context_t *ctx, float c[4]) {
    for (int i = 0; i < 4; i++) {
        const float k = ctx->raster_color[i];
        c[i] = !(k > 0.0f) ? 0.0f : ((k > 1.0f) ? 1.0f : k);
    }
}

/* The transform, shared by every spelling.
 *
 * The clip test is the ordinary one: a point is inside when each of its clip coordinates is
 * within +/- w. Failing it sets the position invalid rather than recording an error - GL has no
 * error for a clipped raster position, it simply stops drawing. */
void glRasterPos4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_RASTER_POS, gl_la_f(x), gl_la_f(y), gl_la_f(z), gl_la_f(w))) return;
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
     * specification says they are always updated, and a program may read them back.
     *
     * **They are a vertex's, processed as a vertex's are**: the colour lit when lighting is on,
     * the texture coordinate through generation and the texture matrix (Mesa main/rastpos.c,
     * shade_rastpos and the TRANSFORM_POINT after compute_texgen). Both were the current values
     * copied raw until 2026-09-19, so a glBitmap label under lighting came out in its unlit
     * colour. */
    if (ctx->cap_lighting) {
        gl_compute_lighting(ctx, obj, ctx->cur_normal, ctx->cur_color, ctx->raster_color);
    } else {
        for (int i = 0; i < 4; i++) ctx->raster_color[i] = ctx->cur_color[i];
    }
    gl_vertex_t rv;
    memset(&rv, 0, sizeof(rv));
    rv.x = x; rv.y = y; rv.z = z; rv.w = w;
    rv.nx = ctx->cur_normal[0]; rv.ny = ctx->cur_normal[1]; rv.nz = ctx->cur_normal[2];
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        gl_vertex_texcoord4(ctx, u, &rv, ctx->cur_texcoord[u], ctx->raster_texcoord[u]);
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
    /* The clip w itself, as Mesa keeps it (main/rastpos.c:454) and GL_CURRENT_RASTER_POSITION
     * reports it. This held 1/w until 2026-09-19, which nothing read but the query. */
    ctx->raster_pos[3] = cw;
    /* The eye-space distance, which is what GL_CURRENT_RASTER_DISTANCE reports and what fog
     * would use. Computed from the modelview alone, not the combined matrix. Under GL 1.4's
     * GL_FOG_COORD source it is the current fog coordinate instead (Mesa main/rastpos.c:475-479). */
    float eye[4];
    mat4_transform_vec4(eye, &ctx->modelview_stack[ctx->modelview_depth], obj);
    ctx->raster_distance = (ctx->fog_coord_src == GL_FOG_COORD)
                               ? ctx->cur_fog_coord
                               : gl_sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    ctx->raster_valid = GL_TRUE;
    /* In GL_SELECT a valid raster position is a hit, like a point (Mesa main/rastpos.c:523). */
    gl_fb_raster_hit(ctx);
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

/* **glWindowPos** (GL 1.4): the raster position in window coordinates, set rather than
 * transformed - Mesa's window_pos3f (main/rastpos.c). Always valid; z clamped to [0, 1] and put
 * through the depth range; w 1; the colour and texture coordinate the current ones, neither lit
 * nor generated nor put through the texture matrix; the raster distance 0 (without fog
 * coordinates). A valid raster position is a hit in GL_SELECT, as glRasterPos's is. */
void glWindowPos3f(GLfloat x, GLfloat y, GLfloat z) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_WINDOW_POS, gl_la_f(x), gl_la_f(y), gl_la_f(z))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    const float zc = (z < 0.0f) ? 0.0f : ((z > 1.0f) ? 1.0f : z);
    ctx->raster_pos[0] = x;
    ctx->raster_pos[1] = y;
    ctx->raster_pos[2] = zc * (ctx->depth_far - ctx->depth_near) + ctx->depth_near;
    ctx->raster_pos[3] = 1.0f;
    ctx->raster_valid = GL_TRUE;
    /* 0, or the fog coordinate under GL_FOG_COORD (Mesa main/rastpos.c:730-733). */
    ctx->raster_distance = (ctx->fog_coord_src == GL_FOG_COORD) ? ctx->cur_fog_coord : 0.0f;
    for (int i = 0; i < 4; i++) {
        const float c = ctx->cur_color[i];
        ctx->raster_color[i] = (c < 0.0f) ? 0.0f : ((c > 1.0f) ? 1.0f : c);
    }
    memcpy(ctx->raster_texcoord, ctx->cur_texcoord, sizeof(ctx->raster_texcoord)); /* every unit's */
    gl_fb_raster_hit(ctx);
}

void glWindowPos2f(GLfloat x, GLfloat y) { glWindowPos3f(x, y, 0.0f); }
void glWindowPos2d(GLdouble x, GLdouble y) { glWindowPos3f((GLfloat)x, (GLfloat)y, 0.0f); }
void glWindowPos2i(GLint x, GLint y) { glWindowPos3f((GLfloat)x, (GLfloat)y, 0.0f); }
void glWindowPos2s(GLshort x, GLshort y) { glWindowPos3f((GLfloat)x, (GLfloat)y, 0.0f); }
void glWindowPos3d(GLdouble x, GLdouble y, GLdouble z) {
    glWindowPos3f((GLfloat)x, (GLfloat)y, (GLfloat)z);
}
void glWindowPos3i(GLint x, GLint y, GLint z) {
    glWindowPos3f((GLfloat)x, (GLfloat)y, (GLfloat)z);
}
void glWindowPos3s(GLshort x, GLshort y, GLshort z) {
    glWindowPos3f((GLfloat)x, (GLfloat)y, (GLfloat)z);
}
void glWindowPos2fv(const GLfloat *v)  { if (v) glWindowPos2f(v[0], v[1]); }
void glWindowPos2dv(const GLdouble *v) { if (v) glWindowPos2d(v[0], v[1]); }
void glWindowPos2iv(const GLint *v)    { if (v) glWindowPos2i(v[0], v[1]); }
void glWindowPos2sv(const GLshort *v)  { if (v) glWindowPos2s(v[0], v[1]); }
void glWindowPos3fv(const GLfloat *v)  { if (v) glWindowPos3f(v[0], v[1], v[2]); }
void glWindowPos3dv(const GLdouble *v) { if (v) glWindowPos3d(v[0], v[1], v[2]); }
void glWindowPos3iv(const GLint *v)    { if (v) glWindowPos3i(v[0], v[1], v[2]); }
void glWindowPos3sv(const GLshort *v)  { if (v) glWindowPos3s(v[0], v[1], v[2]); }

/* **GL_ARB_window_pos' own spellings** (2026-09-19): the extension came before GL 1.4 took it
 * into the core, and a program of that era - one drawing a bitmap font, typically - calls these
 * names after finding the extension in glGetString's list. Each is the core function. */
void glWindowPos2dARB(GLdouble x, GLdouble y) { glWindowPos2d(x, y); }
void glWindowPos2dvARB(const GLdouble *p) { glWindowPos2dv(p); }
void glWindowPos2fARB(GLfloat x, GLfloat y) { glWindowPos2f(x, y); }
void glWindowPos2fvARB(const GLfloat *p) { glWindowPos2fv(p); }
void glWindowPos2iARB(GLint x, GLint y) { glWindowPos2i(x, y); }
void glWindowPos2ivARB(const GLint *p) { glWindowPos2iv(p); }
void glWindowPos2sARB(GLshort x, GLshort y) { glWindowPos2s(x, y); }
void glWindowPos2svARB(const GLshort *p) { glWindowPos2sv(p); }
void glWindowPos3dARB(GLdouble x, GLdouble y, GLdouble z) { glWindowPos3d(x, y, z); }
void glWindowPos3dvARB(const GLdouble *p) { glWindowPos3dv(p); }
void glWindowPos3fARB(GLfloat x, GLfloat y, GLfloat z) { glWindowPos3f(x, y, z); }
void glWindowPos3fvARB(const GLfloat *p) { glWindowPos3fv(p); }
void glWindowPos3iARB(GLint x, GLint y, GLint z) { glWindowPos3i(x, y, z); }
void glWindowPos3ivARB(const GLint *p) { glWindowPos3iv(p); }
void glWindowPos3sARB(GLshort x, GLshort y, GLshort z) { glWindowPos3s(x, y, z); }
void glWindowPos3svARB(const GLshort *p) { glWindowPos3sv(p); }

void glPixelZoom(GLfloat xfactor, GLfloat yfactor) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_PIXEL_ZOOM, gl_la_f(xfactor), gl_la_f(yfactor))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->pixel_zoom_x = xfactor;
    ctx->pixel_zoom_y = yfactor;
}

/* The colour buffer is about to be written by the CPU, so a frame with draws built into it has
 * to be submitted first - the same rule glDeleteTextures and the shader payload follow. **And
 * the CP's copy of it goes stale.** A read after this one would otherwise take a copy made
 * before the write (gl_color_read_source). glDrawPixels and glReadPixels with no draw between
 * them read the old pixels on the console until 2026-09-19. */
static void gl_raster_sync(gl_context_t *ctx) {
#ifndef OOPS_HOST_BUILD
    if (ctx->use_hardware && ctx->hw_frame_active) {
        gl_hw_flush(ctx);
    }
#endif
    ctx->readback_of = NULL;
}

/*
 * **The other end of gl_raster_sync**: the CPU has finished putting pixels into the colour
 * buffer, so the frame is no longer the clear it started as, and the words owe a drain before
 * anything reads them back.
 *
 * On the scanout path the colour buffer is write-combined display memory, and a WC store is not
 * ordered against a later load, so the CP's DMA in `gl_hw_flush` can read a store that has not
 * drained. **That is a real hazard and it was not the one that made six of these checks fail** -
 * the console returned the same eight pixels with this in place and without it. The reason was
 * `glGetFrameReadbackSampled` handing back a copy taken at the last submit; see gl_context.c.
 *
 * Every path in this file that wrote a fragment ends here. That is six places rather than four,
 * because glDrawPixels and glCopyPixels each have a depth-or-stencil arm that returns early.
 */
static void gl_raster_wrote(gl_context_t *ctx) {
    ctx->fb_cleared = GL_TRUE;
    gl_color_cpu_drain(ctx);
}

void glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type,
                  const GLvoid *pixels) {
    if (gl_list_recording() &&
        gl_list_rec_image(GL_LIST_OP_DRAW_PIXELS,
                          GL_LIST_ARGV(gl_la_i(width), gl_la_i(height), gl_la_e(format),
                                       gl_la_e(type)),
                          4, width, height, format, type, pixels)) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (width < 0 || height < 0) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
    /* Every format and type gl_pixel.c reads - GL_RGBA, GL_RGB and GL_LUMINANCE bytes were the
     * only ones until 2026-09-19, so a BGRA, alpha or float image was refused here while the
     * same image uploaded as a texture. */
    gl_pixel_fmt_t f;
    const GLenum fmt_err = gl_pixel_fmt(format, type, &f);
    if (fmt_err != GL_NO_ERROR) { gl_record_error(ctx, fmt_err); return; }
    /* Selection and feedback draw nothing; feedback reports the rectangle's position, whatever
     * its size (Mesa main/drawpix.c:164-171). */
    if (gl_fb_active(ctx)) {
        gl_fb_pixel_token(ctx, GL_DRAW_PIXEL_TOKEN);
        return;
    }
    if (!pixels || width == 0 || height == 0) return;
    /* Not an error - the specification says an invalid raster position simply draws nothing. */
    if (!ctx->raster_valid || !ctx->framebuffer) return;

    gl_raster_sync(ctx);

    gl_pixel_src_t s;
    gl_unpack_source(ctx, &f, pixels, width, height, &s);
    const GLboolean transfer = gl_pixel_transfer_active(ctx);
    gl_pixel_frags_t pf;
    gl_pixel_frags_begin(ctx, &pf);

    /* **Depth and stencil rectangles** (GL 1.0; refused until 2026-09-19). A depth pixel is a
     * fragment in the raster colour at its own z, through every fragment operation - so the depth
     * test decides whether it lands, and a disabled one writes no depth at all, as GL says. A
     * stencil index is written straight into the stencil buffer. */
    if (f.kind == GL_DEPTH || f.kind == GL_STENCIL) {
        float colour[4];
        gl_raster_frag_colour(ctx, colour);
        for (int sy = 0; sy < height; sy++) {
            const uint8_t *row = s.base + (size_t)sy * s.row_stride;
            for (int sx = 0; sx < width; sx++) {
                const float v = gl_unpack_value(ctx, &f, row, sx, s.swap);
                if (f.kind == GL_DEPTH) {
                    pf.z = v;
                    gl_zoomed_fragments(ctx, &pf, sx, sy, colour);
                } else {
                    gl_zoomed_stencil(ctx, &pf, sx, sy, (int64_t)v);
                }
            }
        }
        gl_raster_wrote(ctx);
        return;
    }

    for (int sy = 0; sy < height; sy++) {
        const uint8_t *row = s.base + (size_t)sy * s.row_stride;
        for (int sx = 0; sx < width; sx++) {
            float c[4];
            if (f.kind == GL_COLOR_INDEX) {
                /* A colour index, through the index maps - GL 1.0 in an RGBA context, refused
                 * until 2026-09-19. The maps stand in for the RGBA transfer. */
                gl_unpack_index_rgba(ctx, &f, row, sx, s.swap, c);
            } else {
                gl_unpack_pixel_f(&f, row + (size_t)sx * f.pixel_bytes, s.swap, c);
                /* After expansion to RGBA, so an image with no alpha has its 1.0 scaled and
                 * biased like any other - the specification's order, and Mesa's. Then clamped,
                 * as a fragment's colour is for a fixed-point colour buffer. */
                if (transfer) gl_pixel_transfer_rgbaf(ctx, c);
            }
            for (int i = 0; i < 4; i++) {
                if (!(c[i] > 0.0f)) c[i] = 0.0f;
                if (c[i] > 1.0f) c[i] = 1.0f;
            }
            gl_zoomed_fragments(ctx, &pf, sx, sy, c);
        }
    }
    gl_raster_wrote(ctx);
}

void glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_COPY_PIXELS, gl_la_i(x), gl_la_i(y), gl_la_i(width), gl_la_i(height),
                    gl_la_e(type))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (width < 0 || height < 0) { gl_record_error(ctx, GL_INVALID_VALUE); return; }
    /* GL_COLOR, GL_DEPTH or GL_STENCIL - the last two refused until 2026-09-19, though they are
     * GL 1.0's. */
    if (type != GL_COLOR && type != GL_DEPTH && type != GL_STENCIL) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if ((type == GL_DEPTH && !ctx->depth_buffer) || (type == GL_STENCIL && !ctx->stencil_buffer)) {
        gl_record_error(ctx, GL_INVALID_OPERATION); /* no such buffer */
        return;
    }
    if (!ctx->raster_valid || width == 0 || height == 0) return;
    if (gl_fb_active(ctx)) {
        gl_fb_pixel_token(ctx, GL_COPY_PIXEL_TOKEN);
        return;
    }
    if (!ctx->framebuffer) return;

    gl_raster_sync(ctx);

    /* **A depth or stencil copy**: the source rectangle read whole first, as the colour copy is,
     * then each value through its transfer once - GL_DEPTH_SCALE/BIAS and the clamp, or the
     * index shift, offset and map - and drawn as glDrawPixels draws that format. */
    if (type != GL_COLOR) {
        float *vals = (float *)gl_raster_scratch((size_t)width * (size_t)height * sizeof(float));
        if (!vals) {
            gl_record_error(ctx, GL_OUT_OF_MEMORY);
            return;
        }
        for (int sy = 0; sy < height; sy++) {
            const int srow = gl_raster_row(ctx, y + sy);
            for (int sx = 0; sx < width; sx++) {
                const int scol = x + sx;
                const GLboolean in = (GLboolean)(srow >= 0 && (unsigned int)srow < ctx->height &&
                                                 scol >= 0 && (unsigned int)scol < ctx->width);
                vals[(size_t)sy * (size_t)width + (size_t)sx] =
                    !in ? 0.0f : (type == GL_DEPTH ? *gl_zs_depth_ptr(ctx, scol, y + sy)
                                                   : (float)*gl_zs_stencil_ptr(ctx, scol, y + sy));
            }
        }
        gl_pixel_frags_t vf;
        gl_pixel_frags_begin(ctx, &vf);
        float colour[4];
        gl_raster_frag_colour(ctx, colour);
        const GLboolean xfer = (GLboolean)!ctx->pixel_transfer_suspend;
        for (int sy = 0; sy < height; sy++) {
            const int srow = gl_raster_row(ctx, y + sy);
            if (srow < 0 || (unsigned int)srow >= ctx->height) continue;
            for (int sx = 0; sx < width; sx++) {
                const int scol = x + sx;
                if (scol < 0 || (unsigned int)scol >= ctx->width) continue;
                float v = vals[(size_t)sy * (size_t)width + (size_t)sx];
                if (type == GL_DEPTH) {
                    if (xfer) v = v * ctx->depth_scale + ctx->depth_bias;
                    vf.z = !(v > 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
                    gl_zoomed_fragments(ctx, &vf, sx, sy, colour);
                } else {
                    const int64_t st = xfer ? gl_stencil_transfer(ctx, (int64_t)v) : (int64_t)v;
                    gl_zoomed_stencil(ctx, &vf, sx, sy, st);
                }
            }
        }
        gl_raster_scratch_free(vals);
        gl_raster_wrote(ctx);
        return;
    }

    /* **The whole source is read before anything is written.** GL defines the copy as a
     * glReadPixels followed by a glDrawPixels, and the two rectangles may overlap: this read and
     * wrote pixel by pixel until 2026-09-19, so a copy one row up - or one column right - read
     * back pixels it had just written and smeared the first row or column across the rest.
     * Source pixels outside the buffer are undefined in GL; they are left out here. The source
     * is the buffer glReadBuffer names, the destination the ones glDrawBuffer does. */
    const size_t n = (size_t)width * (size_t)height;
    uint32_t *src = (uint32_t *)gl_raster_scratch(n * sizeof(uint32_t));
    if (!src) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return;
    }
    const uint32_t *const from = gl_read_target(ctx);
    for (int sy = 0; sy < height; sy++) {
        const int srow = gl_raster_row(ctx, y + sy);
        for (int sx = 0; sx < width; sx++) {
            const int scol = x + sx;
            const GLboolean in = (GLboolean)(srow >= 0 && (unsigned int)srow < ctx->height &&
                                             scol >= 0 && (unsigned int)scol < ctx->width);
            src[(size_t)sy * (size_t)width + (size_t)sx] =
                in ? from[gl_color_index(ctx, scol, y + sy)] : 0u;
        }
    }

    const GLboolean transfer = gl_pixel_transfer_active(ctx);
    gl_pixel_frags_t pf;
    gl_pixel_frags_begin(ctx, &pf);
    for (int sy = 0; sy < height; sy++) {
        const int srow = gl_raster_row(ctx, y + sy);
        if (srow < 0 || (unsigned int)srow >= ctx->height) continue;
        for (int sx = 0; sx < width; sx++) {
            const int scol = x + sx;
            if (scol < 0 || (unsigned int)scol >= ctx->width) continue;
            uint32_t v = src[(size_t)sy * (size_t)width + (size_t)sx];
            /* A copy is a read and a draw, and the transfer applies once between them. */
            if (transfer) {
                uint8_t c8[4] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v, (uint8_t)(v >> 24)};
                gl_pixel_transfer_rgba8(ctx, c8);
                v = ((uint32_t)c8[3] << 24) | ((uint32_t)c8[0] << 16) | ((uint32_t)c8[1] << 8) | c8[2];
            }
            const float c[4] = {(float)((v >> 16) & 0xffu) / 255.0f, (float)((v >> 8) & 0xffu) / 255.0f,
                                (float)(v & 0xffu) / 255.0f, (float)((v >> 24) & 0xffu) / 255.0f};
            gl_zoomed_fragments(ctx, &pf, sx, sy, c);
        }
    }
    gl_raster_scratch_free(src);
    gl_raster_wrote(ctx);
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
    /* Compiled with the bits copied - one bit per pixel, read through the unpack state of now,
     * packed tight and most significant bit first for the replay. This is how bitmap fonts in
     * display lists work, which is most of what glBitmap is used for. */
    if (gl_list_recording()) {
        void *bits = (void *)0;
        GLboolean packed = GL_TRUE;
        if (bitmap && width > 0 && height > 0) {
            bits = gl_bitmap_copy_client(ctx, bitmap, width, height);
            packed = (GLboolean)(bits != (void *)0);
        }
        if (!packed) {
            /* Out of memory, already recorded. Nothing is kept; C&E still draws below. */
            if (ctx->list_mode == GL_COMPILE) return;
        } else if (gl_list_rec_owned(GL_LIST_OP_BITMAP,
                                     GL_LIST_ARGV(gl_la_i(width), gl_la_i(height), gl_la_f(xorig),
                                                  gl_la_f(yorig), gl_la_f(xmove), gl_la_f(ymove)),
                                     6, bits)) {
            return;
        }
    }
    if (width < 0 || height < 0) { gl_record_error(ctx, GL_INVALID_VALUE); return; }

    if (gl_fb_active(ctx)) {
        /* Nothing drawn; feedback reports the position. The move below still happens. */
        gl_fb_pixel_token(ctx, GL_BITMAP_TOKEN);
    } else if (bitmap && width > 0 && height > 0 && ctx->raster_valid && ctx->framebuffer) {
        gl_raster_sync(ctx);

        /* The raster colour, clamped as a fragment's is - the raster position keeps it unclamped
         * when unlit, as Mesa's does (main/rastpos.c:497-499). A negative channel came out at
         * full intensity here until 2026-09-19. */
        float c[4];
        for (int i = 0; i < 4; i++) {
            const float k = ctx->raster_color[i];
            c[i] = !(k > 0.0f) ? 0.0f : ((k > 1.0f) ? 1.0f : k);
        }

        /* One bit per pixel, addressed through the unpack state - row length, alignment, skips
         * and GL_UNPACK_LSB_FIRST (gl_bitmap_bit) - at floor(raster - origin), GL 1.x 3.6.6.
         * Unzoomed: glPixelZoom does not apply to bitmaps. */
        const float fx = ctx->raster_pos[0] - xorig, fy = ctx->raster_pos[1] - yorig;
        const int ox = (int)fx - (((float)(int)fx > fx) ? 1 : 0);
        const int oy = (int)fy - (((float)(int)fy > fy) ? 1 : 0);

        gl_pixel_frags_t pf;
        gl_pixel_frags_begin(ctx, &pf);
        for (int sy = 0; sy < height; sy++) {
            for (int sx = 0; sx < width; sx++) {
                if (gl_bitmap_bit(ctx, bitmap, width, sx, sy)) {
                    gl_pixel_fragment(ctx, &pf, ox + sx, oy + sy, c);
                }
            }
        }
        gl_raster_wrote(ctx);
    }

    /* The move happens whatever was drawn, including for a null or empty bitmap. */
    ctx->raster_pos[0] += xmove;
    ctx->raster_pos[1] += ymove;
}

/* -------------------------------------------------------------------------
 * The accumulation buffer
 *
 * RGBA as signed 16-bit fractions of one - Mesa's MESA_FORMAT_RGBA_SNORM16 (main/accum.c) - on
 * the CPU, allocated on first use. Every operation reads or writes the colour buffer through the
 * same doors glReadPixels and glDrawPixels use: a flush first, so what the GPU drew has landed,
 * then the readback copy (or, on the host, the framebuffer) to read and the framebuffer to write.
 * That makes it work on the hardware path as it stands, with no register or shader involved.
 *
 * Mesa's arithmetic, with two differences: products are rounded rather than truncated, and
 * every result is clamped to the buffer's range where Mesa's GLshort arithmetic wraps - a
 * GL_ACCUM that overflowed used to come back as a large negative value, which is the one answer
 * nothing could want.
 * ------------------------------------------------------------------------- */

static void *gl_accum_alloc(size_t bytes) {
#ifdef OOPS_HOST_BUILD
    return malloc(bytes);
#else
    return oops_malloc(bytes);
#endif
}

void gl_accum_free(gl_context_t *ctx) {
    if (!ctx || !ctx->accum_buffer) return;
#ifdef OOPS_HOST_BUILD
    free(ctx->accum_buffer);
#else
    oops_free(ctx->accum_buffer);
#endif
    ctx->accum_buffer = (int16_t *)0;
}

static GLboolean gl_accum_ensure(gl_context_t *ctx) {
    if (ctx->accum_buffer) return GL_TRUE;
    const size_t n = (size_t)ctx->width * (size_t)ctx->height * 4u;
    ctx->accum_buffer = (int16_t *)gl_accum_alloc(n * sizeof(int16_t));
    if (!ctx->accum_buffer) {
        gl_record_error(ctx, GL_OUT_OF_MEMORY);
        return GL_FALSE;
    }
    memset(ctx->accum_buffer, 0, n * sizeof(int16_t));
    return GL_TRUE;
}

static int16_t gl_accum_snorm(float v) {
    const float s = v * 32767.0f;
    if (!(s > -32767.0f)) return (int16_t)-32767; /* also NaN */
    if (s >= 32767.0f) return (int16_t)32767;
    return (int16_t)(s < 0.0f ? s - 0.5f : s + 0.5f);
}

/* The rectangle every operation covers, in window coordinates: the whole buffer, or the scissor
 * box within it when the scissor test is on (Mesa bounds them by the draw buffer's _Xmin.._Ymax). */
static GLboolean gl_accum_region(const gl_context_t *ctx, int *x0, int *y0, int *x1, int *y1) {
    *x0 = 0; *y0 = 0; *x1 = (int)ctx->width; *y1 = (int)ctx->height;
    if (ctx->cap_scissor_test) {
        if (ctx->sc_x > *x0) *x0 = ctx->sc_x;
        if (ctx->sc_y > *y0) *y0 = ctx->sc_y;
        if (ctx->sc_x + ctx->sc_w < *x1) *x1 = ctx->sc_x + ctx->sc_w;
        if (ctx->sc_y + ctx->sc_h < *y1) *y1 = ctx->sc_y + ctx->sc_h;
    }
    return (GLboolean)(*x0 < *x1 && *y0 < *y1);
}

void gl_accum_clear(gl_context_t *ctx) {
    int x0, y0, x1, y1;
    if (!gl_accum_region(ctx, &x0, &y0, &x1, &y1) || !gl_accum_ensure(ctx)) return;
    int16_t c[4];
    for (int i = 0; i < 4; i++) c[i] = gl_accum_snorm(ctx->accum_clear[i]);
    for (int y = y0; y < y1; y++) {
        int16_t *row = ctx->accum_buffer + ((size_t)y * ctx->width) * 4u;
        for (int x = x0; x < x1; x++) {
            for (int i = 0; i < 4; i++) row[(size_t)x * 4u + (size_t)i] = c[i];
        }
    }
}

/* Clamped to -1..1 as it is set, as Mesa clamps it (main/accum.c, _mesa_ClearAccum). */
void glClearAccum(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_CLEAR_ACCUM, gl_la_f(red), gl_la_f(green), gl_la_f(blue),
                    gl_la_f(alpha))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    const GLfloat v[4] = {red, green, blue, alpha};
    for (int i = 0; i < 4; i++) {
        float c = v[i];
        if (!(c > -1.0f)) c = -1.0f;
        if (c > 1.0f) c = 1.0f;
        ctx->accum_clear[i] = c;
    }
}

void glAccum(GLenum op, GLfloat value) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_ACCUM, gl_la_e(op), gl_la_f(value))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (op != GL_ACCUM && op != GL_LOAD && op != GL_RETURN && op != GL_MULT && op != GL_ADD) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (ctx->imm_active) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    /* GL_RETURN writes fragments, and selection and feedback write none. */
    if (op == GL_RETURN && ctx->render_mode != GL_RENDER) return;
    int x0, y0, x1, y1;
    if (!gl_accum_region(ctx, &x0, &y0, &x1, &y1) || !gl_accum_ensure(ctx)) return;
    const size_t w = ctx->width;

    if (op == GL_ADD || op == GL_MULT) {
        for (int y = y0; y < y1; y++) {
            int16_t *row = ctx->accum_buffer + ((size_t)y * w) * 4u;
            for (size_t i = (size_t)x0 * 4u; i < (size_t)x1 * 4u; i++) {
                const float a = (float)row[i] / 32767.0f;
                row[i] = gl_accum_snorm(op == GL_ADD ? a + value : a * value);
            }
        }
        return;
    }

    if (op == GL_ACCUM || op == GL_LOAD) {
        /* Everything drawn so far has to have landed before it is read - glReadPixels' rule.
         * Both operations read the buffer glReadBuffer names (GL 1.0, 4.2.4). */
        glFlush();
        const uint32_t *src = gl_color_read_source(ctx, gl_read_target(ctx));
        if (!src) return;
        for (int y = y0; y < y1; y++) {
            int16_t *row = ctx->accum_buffer + ((size_t)y * w) * 4u;
            for (int x = x0; x < x1; x++) {
                const uint32_t p = src[gl_color_index(ctx, x, y)];
                const float c[4] = {(float)((p >> 16) & 0xffu) / 255.0f,
                                    (float)((p >> 8) & 0xffu) / 255.0f,
                                    (float)(p & 0xffu) / 255.0f,
                                    (float)((p >> 24) & 0xffu) / 255.0f};
                int16_t *acc = row + (size_t)x * 4u;
                for (int i = 0; i < 4; i++) {
                    const float prior = (op == GL_ACCUM) ? (float)acc[i] / 32767.0f : 0.0f;
                    acc[i] = gl_accum_snorm(prior + c[i] * value);
                }
            }
        }
        return;
    }

    /* GL_RETURN: the buffer times `value`, clamped, into the colour buffer through the colour
     * mask - read-modify-write where a channel is masked, as Mesa does (accum_return) - and into
     * each buffer glDrawBuffer names. */
    if (!ctx->framebuffer) return;
    gl_raster_sync(ctx);
    for (int b = 0; b < 2; b++) {
        uint32_t *const target = b == 0 ? ctx->framebuffer : ctx->fb_also;
        if (!target) continue;
        for (int y = y0; y < y1; y++) {
            const int16_t *row = ctx->accum_buffer + ((size_t)y * w) * 4u;
            for (int x = x0; x < x1; x++) {
                const int16_t *acc = row + (size_t)x * 4u;
                uint32_t *const pxp = &target[gl_color_index(ctx, x, y)];
                const uint32_t old = *pxp;
                const uint32_t prior[4] = {(old >> 16) & 0xffu, (old >> 8) & 0xffu, old & 0xffu,
                                           (old >> 24) & 0xffu};
                uint32_t out[4];
                for (int i = 0; i < 4; i++) {
                    if (!gl_color_writes(ctx, i)) { out[i] = prior[i]; continue; }
                    float c = (float)acc[i] / 32767.0f * value;
                    if (!(c > 0.0f)) c = 0.0f;
                    if (c > 1.0f) c = 1.0f;
                    out[i] = (uint32_t)(c * 255.0f + 0.5f);
                }
                *pxp = (out[3] << 24) | (out[0] << 16) | (out[1] << 8) | out[2];
            }
        }
    }
    gl_raster_wrote(ctx);
}

/* The polygon stipple: a 32x32 bitmap, unpacked as glBitmap unpacks one - rows of four bytes,
 * most significant bit leftmost, padded to GL_UNPACK_ALIGNMENT, the first row the bottom - and
 * kept a row to a word, byte 0 in the top bits, as Mesa keeps it (main/pack.c,
 * _mesa_unpack_polygon_stipple). Compiled with the mask packed at compile time. A null mask
 * changes nothing. */
void glPolygonStipple(const GLubyte *mask) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (gl_list_recording()) {
        void *bits = mask ? gl_bitmap_copy_client(ctx, mask, 32, 32) : (void *)0;
        if (mask && !bits) {
            if (ctx->list_mode == GL_COMPILE) return; /* out of memory, already recorded */
        } else if (gl_list_rec_owned(GL_LIST_OP_POLYGON_STIPPLE, (const gl_list_arg_t *)0, 0,
                                     bits)) {
            return;
        }
    }
    if (!mask) return;
    /* Bit by bit through the unpack state, so the skips and GL_UNPACK_LSB_FIRST apply as they do
     * to glBitmap. */
    for (int r = 0; r < 32; r++) {
        uint32_t row = 0;
        for (int c = 0; c < 32; c++) {
            if (gl_bitmap_bit(ctx, mask, 32, c, r)) row |= 0x80000000u >> c;
        }
        ctx->polygon_stipple[r] = row;
    }
}

/* Read back the way it was given: rows of four bytes, padded to GL_PACK_ALIGNMENT. */
void glGetPolygonStipple(GLubyte *mask) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !mask) return;
    /* Written as the pack state lays a bitmap out: row length (in bits), alignment, skips and
     * GL_PACK_LSB_FIRST. Only the mask's own bits are touched. */
    const size_t row_px = ctx->pack_row_length > 0 ? (size_t)ctx->pack_row_length : 32u;
    const size_t align = ctx->pack_alignment > 0 ? (size_t)ctx->pack_alignment : 1u;
    const size_t stride = ((row_px + 7u) / 8u + align - 1u) / align * align;
    for (int r = 0; r < 32; r++) {
        GLubyte *p = mask + ((size_t)ctx->pack_skip_rows + (size_t)r) * stride;
        for (int c = 0; c < 32; c++) {
            const size_t bit = (size_t)ctx->pack_skip_pixels + (size_t)c;
            const unsigned shift = ctx->pack_lsb_first ? (unsigned)(bit % 8u) : 7u - (unsigned)(bit % 8u);
            if ((ctx->polygon_stipple[r] >> (31 - c)) & 1u) p[bit / 8u] |= (GLubyte)(1u << shift);
            else p[bit / 8u] &= (GLubyte)~(1u << shift);
        }
    }
}

/* -------------------------------------------------------------------------
 * Pixel transfer and pixel maps
 *
 * Every colour a pixel rectangle carries in - glDrawPixels, glCopyPixels, the texture uploads and
 * copies - or out through glReadPixels is scaled and biased per component, then with GL_MAP_COLOR
 * replaced from its component's table, then clamped: Mesa's order (main/pixeltransfer.c,
 * _mesa_apply_rgba_transfer_ops). The table entry is the component times (size - 1), rounded to
 * the nearest with ties to even as Mesa rounds it (_mesa_map_rgba, _mesa_lroundevenf).
 *
 * The other half of the state - the index shift and offset, the colour-index and stencil maps,
 * GL_MAP_STENCIL, the depth scale and bias - is kept, compiled, saved and queried, and acts on
 * nothing: it applies to colour-index, stencil and depth rectangles, and those formats are
 * refused by every pixel entry point here.
 * ------------------------------------------------------------------------- */

GLboolean gl_pixel_transfer_active(const gl_context_t *ctx) {
    if (!ctx || ctx->pixel_transfer_suspend) return GL_FALSE;
    if (ctx->map_color) return GL_TRUE;
    for (int i = 0; i < 4; i++) {
        if (ctx->pixel_scale[i] != 1.0f || ctx->pixel_bias[i] != 0.0f) return GL_TRUE;
    }
    return GL_FALSE;
}

static int gl_round_half_even(float x) {
    /* Past what an int holds the value is its own rounding, and converting it would be undefined. */
    if (!(x == x)) return 0;
    if (x >= 1073741824.0f) return 1073741824;
    if (x <= -1073741824.0f) return -1073741824;
    const float fl = (float)(int)x - ((x < 0.0f && (float)(int)x != x) ? 1.0f : 0.0f);
    const float frac = x - fl;
    int n = (int)fl;
    if (frac > 0.5f || (frac == 0.5f && (n & 1))) n++;
    return n;
}

static float gl_clamp01(float x) {
    if (!(x > 0.0f)) return 0.0f; /* also NaN */
    return x > 1.0f ? 1.0f : x;
}

/* On floats, so a source wider than eight bits - a GL_FLOAT or 16-bit image - is scaled before
 * it is quantised rather than after. Clamped at the end, as Mesa clamps. */
void gl_pixel_transfer_rgbaf(const gl_context_t *ctx, float rgba[4]) {
    for (int i = 0; i < 4; i++) {
        float c = rgba[i] * ctx->pixel_scale[i] + ctx->pixel_bias[i];
        if (ctx->map_color) {
            /* R_TO_R, G_TO_G, B_TO_B, A_TO_A are slots 6..9. */
            const int m = 6 + i;
            const int size = ctx->pixel_map_size[m];
            int idx = gl_round_half_even(gl_clamp01(c) * (float)(size - 1));
            if (idx < 0) idx = 0;
            if (idx > size - 1) idx = size - 1;
            c = ctx->pixel_map[m][idx];
        }
        rgba[i] = gl_clamp01(c);
    }
}

void gl_pixel_transfer_rgba8(const gl_context_t *ctx, uint8_t rgba[4]) {
    float c[4];
    for (int i = 0; i < 4; i++) c[i] = (float)rgba[i] / 255.0f;
    gl_pixel_transfer_rgbaf(ctx, c);
    for (int i = 0; i < 4; i++) rgba[i] = (uint8_t)(c[i] * 255.0f + 0.5f);
}

/* Recorded in lists; glPixelTransferi forwards here, so one operation covers both. */
void glPixelTransferf(GLenum pname, GLfloat param) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_PIXEL_TRANSFER, gl_la_e(pname), gl_la_f(param))) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    const GLint rounded = (GLint)(param < 0.0f ? param - 0.5f : param + 0.5f);
    switch (pname) {
        case GL_MAP_COLOR:     ctx->map_color = (GLboolean)(param != 0.0f); break;
        case GL_MAP_STENCIL:   ctx->map_stencil = (GLboolean)(param != 0.0f); break;
        case GL_INDEX_SHIFT:   ctx->index_shift = rounded; break;
        case GL_INDEX_OFFSET:  ctx->index_offset = rounded; break;
        case GL_RED_SCALE:     ctx->pixel_scale[0] = param; break;
        case GL_GREEN_SCALE:   ctx->pixel_scale[1] = param; break;
        case GL_BLUE_SCALE:    ctx->pixel_scale[2] = param; break;
        case GL_ALPHA_SCALE:   ctx->pixel_scale[3] = param; break;
        case GL_RED_BIAS:      ctx->pixel_bias[0] = param; break;
        case GL_GREEN_BIAS:    ctx->pixel_bias[1] = param; break;
        case GL_BLUE_BIAS:     ctx->pixel_bias[2] = param; break;
        case GL_ALPHA_BIAS:    ctx->pixel_bias[3] = param; break;
        case GL_DEPTH_SCALE:   ctx->depth_scale = param; break;
        case GL_DEPTH_BIAS:    ctx->depth_bias = param; break;
        default:               gl_record_error(ctx, GL_INVALID_ENUM); break;
    }
}

void glPixelTransferi(GLenum pname, GLint param) { glPixelTransferf(pname, (GLfloat)param); }

/* The slot for a map, or -1. */
static int gl_pixel_map_slot(GLenum map) {
    if (map >= GL_PIXEL_MAP_I_TO_I && map <= GL_PIXEL_MAP_A_TO_A) return (int)(map - GL_PIXEL_MAP_I_TO_I);
    return -1;
}

/* Maps looked up by an index hold indices, and every other map holds colour components - which
 * decides both how the integer forms convert and how stored values are clamped. */
static GLboolean gl_pixel_map_is_index(GLenum map) {
    return (GLboolean)(map == GL_PIXEL_MAP_I_TO_I || map == GL_PIXEL_MAP_S_TO_S);
}

/* Checked in Mesa's order - the size, then the map, then a size that must be a power of two
 * (main/pixel.c, _mesa_PixelMapfv). **Every map indexed by a colour index or a stencil index
 * must be a power of two in size, GL_PIXEL_MAP_I_TO_I included**: Mesa's range check starts at
 * GL_PIXEL_MAP_S_TO_S and so lets an I_TO_I of any size through. A null array with a valid size
 * does nothing, as it does in Mesa without a pixel buffer bound.
 *
 * Stored as Mesa stores them: colour components clamped to 0..1, stencil indices rounded, colour
 * indices as given (store_pixelmap). */
void glPixelMapfv(GLenum map, GLsizei mapsize, const GLfloat *values) {
    if (gl_list_recording()) {
        const GLboolean keep = (GLboolean)(values && mapsize >= 1 &&
                                           mapsize <= OOPS_GL_MAX_PIXEL_MAP_TABLE);
        void *copy = (void *)0;
        if (keep) {
            copy = gl_list_alloc((size_t)mapsize * sizeof(GLfloat));
            if (!copy) {
                gl_context_t *c = gl_get_ctx();
                gl_record_error(c, GL_OUT_OF_MEMORY);
                if (c && c->list_mode == GL_COMPILE) return;
            } else {
                for (GLsizei i = 0; i < mapsize; i++) ((GLfloat *)copy)[i] = values[i];
            }
        }
        if ((!keep || copy) &&
            gl_list_rec_owned(GL_LIST_OP_PIXEL_MAP,
                              GL_LIST_ARGV(gl_la_e(map), gl_la_i(mapsize)), 2, copy)) {
            return;
        }
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (mapsize < 1 || mapsize > OOPS_GL_MAX_PIXEL_MAP_TABLE) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    const int slot = gl_pixel_map_slot(map);
    if (slot < 0) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (map <= GL_PIXEL_MAP_I_TO_A && (mapsize & (mapsize - 1)) != 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!values) return;
    for (GLsizei i = 0; i < mapsize; i++) {
        float v = values[i];
        if (map == GL_PIXEL_MAP_S_TO_S) v = (float)gl_round_half_even(v);
        else if (map != GL_PIXEL_MAP_I_TO_I) v = gl_clamp01(v);
        ctx->pixel_map[slot][i] = v;
    }
    ctx->pixel_map_size[slot] = mapsize;
}

/* The integer forms convert and forward, so glPixelMapfv's checks and its list record cover them
 * too. An index map takes the integers as they are; a colour map takes them normalised, the
 * whole unsigned range onto 0..1 (Mesa main/pixel.c, UINT_TO_FLOAT and USHORT_TO_FLOAT). */
void glPixelMapuiv(GLenum map, GLsizei mapsize, const GLuint *values) {
    GLfloat f[OOPS_GL_MAX_PIXEL_MAP_TABLE];
    if (!values || mapsize < 1 || mapsize > OOPS_GL_MAX_PIXEL_MAP_TABLE) {
        glPixelMapfv(map, mapsize, (const GLfloat *)0);
        return;
    }
    const GLboolean index = gl_pixel_map_is_index(map);
    for (GLsizei i = 0; i < mapsize; i++) {
        f[i] = index ? (GLfloat)values[i] : (GLfloat)((double)values[i] / 4294967295.0);
    }
    glPixelMapfv(map, mapsize, f);
}

void glPixelMapusv(GLenum map, GLsizei mapsize, const GLushort *values) {
    GLfloat f[OOPS_GL_MAX_PIXEL_MAP_TABLE];
    if (!values || mapsize < 1 || mapsize > OOPS_GL_MAX_PIXEL_MAP_TABLE) {
        glPixelMapfv(map, mapsize, (const GLfloat *)0);
        return;
    }
    const GLboolean index = gl_pixel_map_is_index(map);
    for (GLsizei i = 0; i < mapsize; i++) {
        f[i] = index ? (GLfloat)values[i] : (GLfloat)values[i] / 65535.0f;
    }
    glPixelMapfv(map, mapsize, f);
}

/* Read back the way they were set: an index map as integers, a colour map normalised over the
 * type's range and rounded. Mesa's integer readbacks do otherwise for the index maps -
 * glGetPixelMapuiv copies S_TO_S's float bits and normalises I_TO_I (main/pixel.c) - which does
 * not give back what glPixelMapuiv put in; these do. */
static const float *gl_pixel_map_read(GLenum map, GLint *size) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return (const float *)0;
    const int slot = gl_pixel_map_slot(map);
    if (slot < 0) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return (const float *)0;
    }
    *size = ctx->pixel_map_size[slot];
    return ctx->pixel_map[slot];
}

void glGetPixelMapfv(GLenum map, GLfloat *values) {
    GLint n = 0;
    const float *m = gl_pixel_map_read(map, &n);
    if (!m || !values) return;
    for (GLint i = 0; i < n; i++) values[i] = m[i];
}

void glGetPixelMapuiv(GLenum map, GLuint *values) {
    GLint n = 0;
    const float *m = gl_pixel_map_read(map, &n);
    if (!m || !values) return;
    const GLboolean index = gl_pixel_map_is_index(map);
    for (GLint i = 0; i < n; i++) {
        const double v = index ? (double)m[i] : (double)m[i] * 4294967295.0;
        values[i] = v <= 0.0 ? 0u : (v >= 4294967295.0 ? 0xffffffffu : (GLuint)(v + 0.5));
    }
}

void glGetPixelMapusv(GLenum map, GLushort *values) {
    GLint n = 0;
    const float *m = gl_pixel_map_read(map, &n);
    if (!m || !values) return;
    const GLboolean index = gl_pixel_map_is_index(map);
    for (GLint i = 0; i < n; i++) {
        const float v = index ? m[i] : m[i] * 65535.0f;
        values[i] = v <= 0.0f ? (GLushort)0 : (v >= 65535.0f ? (GLushort)0xffff : (GLushort)(v + 0.5f));
    }
}
