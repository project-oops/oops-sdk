/*
 * oops-gl: pixel rectangles in client memory
 *
 * Every call that reads or writes an image in the program's memory - the texture uploads,
 * glDrawPixels, glReadPixels, glGetTexImage, glBitmap and the polygon stipple, and the display
 * lists that keep copies of them - describes it with a format, a type and the glPixelStorei
 * state. This is the one place that reads that description.
 *
 * # What a format and type mean
 *
 * The format names the components a pixel holds, in order: GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA,
 * GL_RGB, GL_BGR, GL_RGBA, GL_BGRA, GL_LUMINANCE, GL_LUMINANCE_ALPHA. The type says how each is
 * stored: one element per component - GL_UNSIGNED_BYTE, GL_BYTE, GL_UNSIGNED_SHORT, GL_SHORT,
 * GL_UNSIGNED_INT, GL_INT, GL_FLOAT - or, for GL 1.2's packed types, the whole pixel in one
 * 8-, 16- or 32-bit element, the format's first component in the most significant bits (the
 * _REV types: the least). Mesa's table of which packed type fits which format is the one used
 * (main/glformats.c, _mesa_format_from_format_and_type); a pair it does not have is
 * GL_INVALID_OPERATION, as GL 1.2 says.
 *
 * Integers become fractions of one as the specification converts them: an unsigned value over
 * 2^b - 1, a signed one as (2c + 1) / (2^b - 1) (the same macros the vertex attributes use,
 * gl_draw.c), a packed field over 2^bits - 1. Going out it is the inverse, rounded.
 *
 * # Where a rectangle is
 *
 * Rows are GL_*_ROW_LENGTH pixels apart (the width, when that is 0) padded to GL_*_ALIGNMENT;
 * slices of a 3D image are GL_*_IMAGE_HEIGHT rows apart (the height, when 0); and the rectangle
 * starts GL_*_SKIP_PIXELS pixels, GL_*_SKIP_ROWS rows and GL_*_SKIP_IMAGES slices in. With
 * GL_*_SWAP_BYTES each multi-byte element is stored the other way round. A bitmap's rows are
 * bits, and GL_UNPACK_LSB_FIRST puts a byte's first pixel in its lowest bit.
 *
 * Until 2026-09-19 only GL_UNSIGNED_BYTE was read, in six formats, with the alignment and the
 * unpack row length; everything else here was refused.
 */

#include "gl_internal.h"

#ifdef OOPS_HOST_BUILD
#include <stdlib.h>
#endif

/* A packed type: element size, and its fields' widths in the format's component order. */
typedef struct {
    GLenum type;
    size_t bytes;
    int n;
    int bits[4];
    GLboolean rev;
} gl_packed_t;

static const gl_packed_t k_packed[] = {
    {GL_UNSIGNED_BYTE_3_3_2,         1, 3, {3, 3, 2, 0},     GL_FALSE},
    {GL_UNSIGNED_BYTE_2_3_3_REV,     1, 3, {3, 3, 2, 0},     GL_TRUE},
    {GL_UNSIGNED_SHORT_5_6_5,        2, 3, {5, 6, 5, 0},     GL_FALSE},
    {GL_UNSIGNED_SHORT_5_6_5_REV,    2, 3, {5, 6, 5, 0},     GL_TRUE},
    {GL_UNSIGNED_SHORT_4_4_4_4,      2, 4, {4, 4, 4, 4},     GL_FALSE},
    {GL_UNSIGNED_SHORT_4_4_4_4_REV,  2, 4, {4, 4, 4, 4},     GL_TRUE},
    {GL_UNSIGNED_SHORT_5_5_5_1,      2, 4, {5, 5, 5, 1},     GL_FALSE},
    {GL_UNSIGNED_SHORT_1_5_5_5_REV,  2, 4, {5, 5, 5, 1},     GL_TRUE},
    {GL_UNSIGNED_INT_8_8_8_8,        4, 4, {8, 8, 8, 8},     GL_FALSE},
    {GL_UNSIGNED_INT_8_8_8_8_REV,    4, 4, {8, 8, 8, 8},     GL_TRUE},
    {GL_UNSIGNED_INT_10_10_10_2,     4, 4, {10, 10, 10, 2},  GL_FALSE},
    {GL_UNSIGNED_INT_2_10_10_10_REV, 4, 4, {10, 10, 10, 2},  GL_TRUE},
};

static const gl_packed_t *gl_packed_info(GLenum type) {
    for (size_t i = 0; i < sizeof(k_packed) / sizeof(k_packed[0]); i++) {
        if (k_packed[i].type == type) return &k_packed[i];
    }
    return (const gl_packed_t *)0;
}

static int gl_format_components(GLenum format) {
    switch (format) {
        case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: return 1;
        case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX: case GL_COLOR_INDEX: return 1;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_RGB: case GL_BGR: return 3;
        case GL_RGBA: case GL_BGRA: return 4;
        default: return 0;
    }
}

static size_t gl_type_bytes(GLenum type) {
    switch (type) {
        case GL_UNSIGNED_BYTE: case GL_BYTE: return 1u;
        case GL_UNSIGNED_SHORT: case GL_SHORT: return 2u;
        case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4u;
        default: return 0u;
    }
}

GLenum gl_pixel_fmt(GLenum format, GLenum type, gl_pixel_fmt_t *out) {
    /* **GL_BITMAP**: one bit a pixel, and only for indices - colour or stencil (Mesa
     * main/glformats.c:1949-1953) - addressed as glBitmap's bits are. */
    if (type == GL_BITMAP) {
        if (format != GL_COLOR_INDEX && format != GL_STENCIL_INDEX) return GL_INVALID_ENUM;
        out->format = format;
        out->type = type;
        out->kind = (format == GL_COLOR_INDEX) ? (GLenum)GL_COLOR_INDEX : (GLenum)GL_STENCIL;
        out->components = 1;
        out->packed = GL_FALSE;
        out->bitmap = GL_TRUE;
        out->elem_bytes = 0u;
        out->pixel_bytes = 0u;
        return GL_NO_ERROR;
    }
    out->bitmap = GL_FALSE;
    const int n = gl_format_components(format);
    const gl_packed_t *pk = gl_packed_info(type);
    const size_t eb = gl_type_bytes(type);
    if (n == 0 || (!pk && eb == 0u)) return GL_INVALID_ENUM;
    const GLboolean value = (GLboolean)(format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX ||
                                        format == GL_COLOR_INDEX);
    /* A depth, stencil or colour-index value is one number, which no packed type holds (Mesa
     * main/glformats.c, _mesa_error_check_format_and_type). */
    if (pk && value) return GL_INVALID_OPERATION;
    if (pk) {
        /* Which formats each packed type fits: three-field ones RGB (and BGR for 5-6-5, which
         * Mesa accepts), four-field ones RGBA or BGRA. */
        GLboolean ok;
        if (pk->n == 3) {
            ok = (GLboolean)(format == GL_RGB ||
                             (format == GL_BGR && (type == GL_UNSIGNED_SHORT_5_6_5 ||
                                                   type == GL_UNSIGNED_SHORT_5_6_5_REV)));
        } else {
            ok = (GLboolean)(format == GL_RGBA || format == GL_BGRA);
        }
        if (!ok) return GL_INVALID_OPERATION;
    }
    out->format = format;
    out->type = type;
    out->kind = (format == GL_DEPTH_COMPONENT) ? (GLenum)GL_DEPTH
              : (format == GL_STENCIL_INDEX) ? (GLenum)GL_STENCIL
              : (format == GL_COLOR_INDEX) ? (GLenum)GL_COLOR_INDEX : (GLenum)GL_COLOR;
    out->components = n;
    out->packed = (GLboolean)(pk != (const gl_packed_t *)0);
    out->elem_bytes = pk ? pk->bytes : eb;
    out->pixel_bytes = pk ? pk->bytes : eb * (size_t)n;
    return GL_NO_ERROR;
}

/* -------------------------------------------------------------------------
 * Addressing
 * ------------------------------------------------------------------------- */

static size_t gl_align_up(size_t bytes, GLint alignment) {
    const size_t a = alignment > 0 ? (size_t)alignment : 1u;
    const size_t r = bytes % a;
    return r ? bytes + (a - r) : bytes;
}

void gl_unpack_source(const gl_context_t *ctx, const gl_pixel_fmt_t *f, const void *pixels,
                      GLsizei width, GLsizei height, gl_pixel_src_t *out) {
    const size_t row_px = ctx->unpack_row_length > 0 ? (size_t)ctx->unpack_row_length
                                                     : (size_t)width;
    const size_t rows = ctx->unpack_image_height > 0 ? (size_t)ctx->unpack_image_height
                                                     : (size_t)height;
    /* A bitmap's rows are bytes of bits; its skipped pixels are bits, applied per pixel by
     * gl_unpack_bit rather than here. */
    if (f->bitmap) {
        out->row_stride = gl_align_up((row_px + 7u) / 8u, ctx->unpack_alignment);
        out->image_stride = out->row_stride * rows;
        out->swap = GL_FALSE;
        out->base = (const uint8_t *)pixels;
        if (out->base) {
            out->base += (size_t)ctx->unpack_skip_images * out->image_stride +
                         (size_t)ctx->unpack_skip_rows * out->row_stride;
        }
        return;
    }
    out->row_stride = gl_align_up(row_px * f->pixel_bytes, ctx->unpack_alignment);
    out->image_stride = out->row_stride * rows;
    out->swap = (GLboolean)(ctx->unpack_swap_bytes && f->elem_bytes > 1u);
    out->base = (const uint8_t *)pixels;
    if (out->base) {
        out->base += (size_t)ctx->unpack_skip_images * out->image_stride +
                     (size_t)ctx->unpack_skip_rows * out->row_stride +
                     (size_t)ctx->unpack_skip_pixels * f->pixel_bytes;
    }
}

void gl_pack_dest(const gl_context_t *ctx, const gl_pixel_fmt_t *f, void *pixels,
                  GLsizei width, GLsizei height, gl_pixel_dst_t *out) {
    const size_t row_px = ctx->pack_row_length > 0 ? (size_t)ctx->pack_row_length
                                                   : (size_t)width;
    const size_t rows = ctx->pack_image_height > 0 ? (size_t)ctx->pack_image_height
                                                   : (size_t)height;
    if (f->bitmap) {
        out->row_stride = gl_align_up((row_px + 7u) / 8u, ctx->pack_alignment);
        out->image_stride = out->row_stride * rows;
        out->swap = GL_FALSE;
        out->base = (uint8_t *)pixels;
        if (out->base) {
            out->base += (size_t)ctx->pack_skip_images * out->image_stride +
                         (size_t)ctx->pack_skip_rows * out->row_stride;
        }
        return;
    }
    out->row_stride = gl_align_up(row_px * f->pixel_bytes, ctx->pack_alignment);
    out->image_stride = out->row_stride * rows;
    out->swap = (GLboolean)(ctx->pack_swap_bytes && f->elem_bytes > 1u);
    out->base = (uint8_t *)pixels;
    if (out->base) {
        out->base += (size_t)ctx->pack_skip_images * out->image_stride +
                     (size_t)ctx->pack_skip_rows * out->row_stride +
                     (size_t)ctx->pack_skip_pixels * f->pixel_bytes;
    }
}

/* -------------------------------------------------------------------------
 * Elements
 * ------------------------------------------------------------------------- */

static uint32_t gl_read_elem(const uint8_t *p, size_t bytes, GLboolean swap) {
    uint8_t b[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < bytes; i++) b[i] = swap ? p[bytes - 1u - i] : p[i];
    uint32_t v = 0;
    memcpy(&v, b, bytes); /* native order, as the program wrote it */
    if (bytes == 1u) v &= 0xffu;
    else if (bytes == 2u) v &= 0xffffu;
    return v;
}

static void gl_write_elem(uint8_t *p, size_t bytes, uint32_t v, GLboolean swap) {
    uint8_t b[4];
    memcpy(b, &v, 4);
    for (size_t i = 0; i < bytes; i++) p[swap ? bytes - 1u - i : i] = b[i];
}

/* One element of a plain type, as a fraction of one (or the float itself). */
static float gl_elem_to_f(GLenum type, uint32_t v) {
    switch (type) {
        case GL_UNSIGNED_BYTE:  return (float)v / 255.0f;
        case GL_BYTE:           return (float)((2.0 * (double)(int8_t)(uint8_t)v + 1.0) / 255.0);
        case GL_UNSIGNED_SHORT: return (float)v / 65535.0f;
        case GL_SHORT:          return (float)((2.0 * (double)(int16_t)(uint16_t)v + 1.0) / 65535.0);
        case GL_UNSIGNED_INT:   return (float)((double)v / 4294967295.0);
        case GL_INT:            return (float)((2.0 * (double)(int32_t)v + 1.0) / 4294967295.0);
        case GL_FLOAT: {
            float f;
            memcpy(&f, &v, 4);
            return f;
        }
        default: return 0.0f;
    }
}

static double gl_clampd(double x, double lo, double hi) {
    if (!(x > lo)) return lo; /* also NaN */
    return x > hi ? hi : x;
}

static uint32_t gl_round_u32(double x) {
    return (uint32_t)(x + 0.5);
}

/* To the nearest integer, a tie going toward zero: 0 packs as ((2^b - 1)0 - 1) / 2 = -1/2, and
 * Mesa's FLOAT_TO_BYTE / FLOAT_TO_SHORT (main/macros.h:59, :83) make that 0 by integer division. */
static int64_t gl_round_signed(double s) {
    const double m = (s < 0.0 ? -s : s) - 0.5;
    int64_t r = (int64_t)m;
    if (m > (double)r) r++;
    return s < 0.0 ? -r : r;
}

/* A fraction of one as an element of a plain type - the inverse of gl_elem_to_f, rounded:
 * unsigned c(2^b - 1), signed ((2^b - 1)c - 1) / 2 (the specification's pack conversions). */
static uint32_t gl_f_to_elem(GLenum type, float c) {
    const double d = (double)c;
    switch (type) {
        case GL_UNSIGNED_BYTE:  return gl_round_u32(gl_clampd(d, 0.0, 1.0) * 255.0);
        case GL_UNSIGNED_SHORT: return gl_round_u32(gl_clampd(d, 0.0, 1.0) * 65535.0);
        case GL_UNSIGNED_INT:   return gl_round_u32(gl_clampd(d, 0.0, 1.0) * 4294967295.0);
        case GL_BYTE: {
            const int64_t r = gl_round_signed((gl_clampd(d, -1.0, 1.0) * 255.0 - 1.0) * 0.5);
            return (uint32_t)(uint8_t)(int8_t)(r < -128 ? -128 : (r > 127 ? 127 : r));
        }
        case GL_SHORT: {
            const int64_t r = gl_round_signed((gl_clampd(d, -1.0, 1.0) * 65535.0 - 1.0) * 0.5);
            return (uint32_t)(uint16_t)(int16_t)(r < -32768 ? -32768 : (r > 32767 ? 32767 : r));
        }
        case GL_INT: {
            const int64_t r = gl_round_signed((gl_clampd(d, -1.0, 1.0) * 4294967295.0 - 1.0) * 0.5);
            return (uint32_t)(int32_t)(r < -2147483648LL ? -2147483648LL : (r > 2147483647LL ? 2147483647LL : r));
        }
        case GL_FLOAT: {
            uint32_t v;
            memcpy(&v, &c, 4);
            return v;
        }
        default: return 0u;
    }
}

/* Where each of a format's components goes in RGBA: 0..3 for R..A, 4 for luminance. */
static void gl_format_slots(GLenum format, int slots[4]) {
    switch (format) {
        case GL_RED:   slots[0] = 0; break;
        case GL_GREEN: slots[0] = 1; break;
        case GL_BLUE:  slots[0] = 2; break;
        case GL_ALPHA: slots[0] = 3; break;
        case GL_LUMINANCE: slots[0] = 4; break;
        case GL_LUMINANCE_ALPHA: slots[0] = 4; slots[1] = 3; break;
        case GL_RGB:  slots[0] = 0; slots[1] = 1; slots[2] = 2; break;
        case GL_BGR:  slots[0] = 2; slots[1] = 1; slots[2] = 0; break;
        case GL_RGBA: slots[0] = 0; slots[1] = 1; slots[2] = 2; slots[3] = 3; break;
        case GL_BGRA: slots[0] = 2; slots[1] = 1; slots[2] = 0; slots[3] = 3; break;
        default: break;
    }
}

/* A packed pixel's fields in component order: shift and mask of each. */
static void gl_packed_fields(const gl_packed_t *pk, int shift[4], uint32_t mask[4]) {
    const int total = (int)pk->bytes * 8;
    int at = 0;
    for (int i = 0; i < pk->n; i++) {
        mask[i] = (pk->bits[i] >= 32) ? 0xffffffffu : ((1u << pk->bits[i]) - 1u);
        if (pk->rev) {
            shift[i] = at;
        } else {
            shift[i] = total - at - pk->bits[i];
        }
        at += pk->bits[i];
    }
}

void gl_unpack_pixel_f(const gl_pixel_fmt_t *f, const uint8_t *src, GLboolean swap, float out[4]) {
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (f->packed) {
        const gl_packed_t *pk = gl_packed_info(f->type);
        const uint32_t e = gl_read_elem(src, pk->bytes, swap);
        int shift[4];
        uint32_t mask[4];
        gl_packed_fields(pk, shift, mask);
        for (int i = 0; i < pk->n; i++) v[i] = (float)((e >> shift[i]) & mask[i]) / (float)mask[i];
    } else {
        for (int i = 0; i < f->components; i++) {
            v[i] = gl_elem_to_f(f->type, gl_read_elem(src + (size_t)i * f->elem_bytes,
                                                      f->elem_bytes, swap));
        }
    }
    int slots[4] = {0, 0, 0, 0};
    gl_format_slots(f->format, slots);
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f;
    for (int i = 0; i < f->components; i++) {
        if (slots[i] == 4) {
            out[0] = out[1] = out[2] = v[i];
        } else {
            out[slots[i]] = v[i];
        }
    }
}

void gl_pack_pixel_f(const gl_pixel_fmt_t *f, const float rgba[4], GLboolean lum_sum,
                     GLboolean swap, uint8_t *dst) {
    int slots[4] = {0, 0, 0, 0};
    gl_format_slots(f->format, slots);
    float v[4];
    for (int i = 0; i < f->components; i++) {
        if (slots[i] == 4) {
            const float l = lum_sum ? rgba[0] + rgba[1] + rgba[2] : rgba[0];
            v[i] = l > 1.0f ? 1.0f : l;
        } else {
            v[i] = rgba[slots[i]];
        }
    }
    if (f->packed) {
        const gl_packed_t *pk = gl_packed_info(f->type);
        int shift[4];
        uint32_t mask[4];
        gl_packed_fields(pk, shift, mask);
        uint32_t e = 0;
        for (int i = 0; i < pk->n; i++) {
            const uint32_t q = gl_round_u32(gl_clampd((double)v[i], 0.0, 1.0) * (double)mask[i]);
            e |= (q & mask[i]) << shift[i];
        }
        gl_write_elem(dst, pk->bytes, e, swap);
        return;
    }
    for (int i = 0; i < f->components; i++) {
        gl_write_elem(dst + (size_t)i * f->elem_bytes, f->elem_bytes, gl_f_to_elem(f->type, v[i]),
                      swap);
    }
}

void gl_tex_store_row(const gl_context_t *ctx, const gl_pixel_fmt_t *f, uint8_t *dst,
                      const uint8_t *src, GLsizei width, GLboolean swap, GLenum base) {
    if (base == GL_DEPTH_COMPONENT) {
        /* A depth texel is the 32-bit float itself, in the four bytes a colour texel takes. */
        for (size_t x = 0; x < (size_t)width; x++) {
            const float d = gl_unpack_value(ctx, f, src, (int)x, swap);
            memcpy(dst + x * 4u, &d, 4u);
        }
        return;
    }
    gl_unpack_row(ctx, f, dst, src, width, swap);
    gl_tex_rebase_row(dst, width, base);
}

void gl_unpack_row(const gl_context_t *ctx, const gl_pixel_fmt_t *f, uint8_t *dst,
                   const uint8_t *src, GLsizei width, GLboolean swap) {
    const GLboolean transfer = gl_pixel_transfer_active(ctx);
    for (size_t x = 0; x < (size_t)width; x++) {
        float c[4];
        if (f->kind == GL_COLOR_INDEX) {
            /* An index becomes RGBA through the I_TO_* maps, which stand in for the RGBA
             * transfer - so that is not applied as well. */
            gl_unpack_index_rgba(ctx, f, src, (int)x, swap, c);
        } else {
            gl_unpack_pixel_f(f, src + x * f->pixel_bytes, swap, c);
            if (transfer) gl_pixel_transfer_rgbaf(ctx, c);
        }
        for (int i = 0; i < 4; i++) {
            float k = c[i];
            if (!(k > 0.0f)) k = 0.0f;
            if (k > 1.0f) k = 1.0f;
            dst[x * 4u + (size_t)i] = (uint8_t)(k * 255.0f + 0.5f);
        }
    }
}

/* -------------------------------------------------------------------------
 * Depth and stencil values
 *
 * GL_DEPTH_COMPONENT and GL_STENCIL_INDEX pixels are one number each. A depth value converts as a
 * colour component does - an integer is a fraction of one - and goes through GL_DEPTH_SCALE and
 * GL_DEPTH_BIAS and a clamp to [0, 1]; a stencil index is the integer itself, shifted and offset
 * by GL_INDEX_SHIFT and GL_INDEX_OFFSET and looked up in GL_PIXEL_MAP_S_TO_S under
 * GL_MAP_STENCIL (GL 1.x, 3.6.5 and 4.3.2). The transfer is skipped while suspended, as a
 * colour's is.
 * ------------------------------------------------------------------------- */

/* Mesa's order (main/pixeltransfer.c:220-252, _mesa_apply_stencil_transfer_ops): shift - left for
 * a positive GL_INDEX_SHIFT, right for a negative - and offset, then the S_TO_S map indexed by the
 * value masked to the map's size, which is a power of two. */
static int64_t gl_index_shift_offset(const gl_context_t *ctx, int64_t s) {
    if (ctx->index_shift > 0) s = (int64_t)((uint64_t)s << ctx->index_shift);
    else if (ctx->index_shift < 0) s >>= -ctx->index_shift;
    return s + ctx->index_offset;
}

int64_t gl_stencil_transfer(const gl_context_t *ctx, int64_t s) {
    s = gl_index_shift_offset(ctx, s);
    if (ctx->map_stencil) {
        const int slot = (int)(GL_PIXEL_MAP_S_TO_S - GL_PIXEL_MAP_I_TO_I);
        const int size = ctx->pixel_map_size[slot] > 0 ? ctx->pixel_map_size[slot] : 1;
        s = (int64_t)ctx->pixel_map[slot][(uint64_t)s & (uint64_t)(size - 1)];
    }
    return s;
}

/* The integer an index element holds - a float truncated to one. */
static int64_t gl_index_elem(GLenum type, uint32_t e) {
    switch (type) {
        case GL_BYTE:  return (int8_t)(uint8_t)e;
        case GL_SHORT: return (int16_t)(uint16_t)e;
        case GL_INT:   return (int32_t)e;
        case GL_FLOAT: {
            float v;
            memcpy(&v, &e, 4);
            return (int64_t)v;
        }
        default:       return (int64_t)e;
    }
}

GLboolean gl_unpack_bit(const gl_context_t *ctx, const uint8_t *row, int x) {
    const size_t bit = (size_t)ctx->unpack_skip_pixels + (size_t)x;
    const unsigned shift = ctx->unpack_lsb_first ? (unsigned)(bit % 8u) : 7u - (unsigned)(bit % 8u);
    return (GLboolean)((row[bit / 8u] >> shift) & 1u);
}

void gl_pack_bit(const gl_context_t *ctx, uint8_t *row, int x, GLboolean v) {
    const size_t bit = (size_t)ctx->pack_skip_pixels + (size_t)x;
    const unsigned shift = ctx->pack_lsb_first ? (unsigned)(bit % 8u) : 7u - (unsigned)(bit % 8u);
    if (v) row[bit / 8u] = (uint8_t)(row[bit / 8u] | (1u << shift));
    else row[bit / 8u] = (uint8_t)(row[bit / 8u] & ~(1u << shift));
}

void gl_index_to_rgba(const gl_context_t *ctx, int64_t i, float out[4]) {
    if (!ctx->pixel_transfer_suspend) i = gl_index_shift_offset(ctx, i);
    for (int c = 0; c < 4; c++) {
        const int slot = (int)(GL_PIXEL_MAP_I_TO_R - GL_PIXEL_MAP_I_TO_I) + c;
        const int size = ctx->pixel_map_size[slot] > 0 ? ctx->pixel_map_size[slot] : 1;
        const float v = ctx->pixel_map[slot][(uint64_t)i & (uint64_t)(size - 1)];
        out[c] = !(v > 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
    }
}

/* Index `x` of a row as an integer: its bit under GL_BITMAP, its element otherwise. */
static int64_t gl_row_index(const gl_context_t *ctx, const gl_pixel_fmt_t *f, const uint8_t *row,
                            int x, GLboolean swap) {
    if (f->bitmap) return gl_unpack_bit(ctx, row, x) ? 1 : 0;
    return gl_index_elem(f->type, gl_read_elem(row + (size_t)x * f->pixel_bytes, f->elem_bytes, swap));
}

void gl_unpack_index_rgba(const gl_context_t *ctx, const gl_pixel_fmt_t *f, const uint8_t *row,
                          int x, GLboolean swap, float out[4]) {
    gl_index_to_rgba(ctx, gl_row_index(ctx, f, row, x, swap), out);
}

float gl_unpack_value(const gl_context_t *ctx, const gl_pixel_fmt_t *f, const uint8_t *row, int x,
                      GLboolean swap) {
    const GLboolean transfer = (GLboolean)!ctx->pixel_transfer_suspend;
    if (f->kind == GL_DEPTH) {
        const uint32_t e = gl_read_elem(row + (size_t)x * f->pixel_bytes, f->elem_bytes, swap);
        float d = gl_elem_to_f(f->type, e);
        if (transfer) d = d * ctx->depth_scale + ctx->depth_bias;
        return !(d > 0.0f) ? 0.0f : ((d > 1.0f) ? 1.0f : d);
    }
    /* A stencil index: the element's integer value, a float truncated to one - or its bit. */
    const int64_t s = gl_row_index(ctx, f, row, x, swap);
    return (float)(transfer ? gl_stencil_transfer(ctx, s) : s);
}

void gl_pack_value(const gl_context_t *ctx, const gl_pixel_fmt_t *f, float v, GLboolean swap,
                   uint8_t *dst) {
    const GLboolean transfer = (GLboolean)!ctx->pixel_transfer_suspend;
    uint32_t e;
    if (f->kind == GL_DEPTH) {
        float d = transfer ? v * ctx->depth_scale + ctx->depth_bias : v;
        d = !(d > 0.0f) ? 0.0f : ((d > 1.0f) ? 1.0f : d);
        e = gl_f_to_elem(f->type, d);
    } else {
        const int64_t s = transfer ? gl_stencil_transfer(ctx, (int64_t)v) : (int64_t)v;
        if (f->type == GL_FLOAT) {
            const float fs = (float)s;
            memcpy(&e, &fs, 4);
        } else {
            e = (uint32_t)s; /* the element's low bits, as a C conversion keeps them */
        }
    }
    gl_write_elem(dst, f->elem_bytes, e, swap);
}

/* -------------------------------------------------------------------------
 * Copies for display lists
 * ------------------------------------------------------------------------- */

/* **How many bytes the function below produced.** Beside it rather than at its call sites,
 * because the two must agree and the layout is this file's to know: tight rows of
 * `width * pixel_bytes`, except a GL_BITMAP image, which is packed to the bit like glBitmap's.
 * A capture has to write the blob's length down, and nothing else ever needed it. */
size_t gl_pixel_packed_bytes(const gl_pixel_fmt_t *f, GLsizei width, GLsizei height,
                             GLsizei depth) {
    if (!f || width <= 0 || height <= 0 || depth <= 0) return 0u;
    if (f->bitmap) {
        return (depth == 1) ? (((size_t)width + 7u) / 8u) * (size_t)height : 0u;
    }
    return (size_t)width * f->pixel_bytes * (size_t)height * (size_t)depth;
}

void *gl_pixel_copy_client(const gl_context_t *ctx, const gl_pixel_fmt_t *f, const void *pixels,
                           GLsizei width, GLsizei height, GLsizei depth) {
    if (!pixels || width <= 0 || height <= 0 || depth <= 0) return (void *)0;
    /* A GL_BITMAP image as glBitmap's is kept: tight, most significant bit first, which the
     * neutral replay state reads back the same way. One slice. */
    if (f->bitmap) {
        return (depth == 1) ? gl_bitmap_copy_client(ctx, (const GLubyte *)pixels, width, height)
                            : (void *)0;
    }
    gl_pixel_src_t s;
    gl_unpack_source(ctx, f, pixels, width, height, &s);
    const size_t row = (size_t)width * f->pixel_bytes;
    uint8_t *img = (uint8_t *)gl_list_alloc(row * (size_t)height * (size_t)depth);
    if (!img) {
        gl_record_error(gl_get_ctx(), GL_OUT_OF_MEMORY);
        return (void *)0;
    }
    uint8_t *out = img;
    for (size_t z = 0; z < (size_t)depth; z++) {
        for (size_t y = 0; y < (size_t)height; y++) {
            const uint8_t *in = s.base + z * s.image_stride + y * s.row_stride;
            if (!s.swap) {
                memcpy(out, in, row);
            } else {
                /* Stored native, so the replay reads it with swapping off. */
                for (size_t e = 0; e < row; e += f->elem_bytes) {
                    for (size_t b = 0; b < f->elem_bytes; b++) {
                        out[e + b] = in[e + f->elem_bytes - 1u - b];
                    }
                }
            }
            out += row;
        }
    }
    return img;
}

/* -------------------------------------------------------------------------
 * Bitmaps
 * ------------------------------------------------------------------------- */

GLboolean gl_bitmap_bit(const gl_context_t *ctx, const GLubyte *bits, GLsizei width, int x, int y) {
    const size_t row_px = ctx->unpack_row_length > 0 ? (size_t)ctx->unpack_row_length
                                                     : (size_t)width;
    const size_t stride = gl_align_up((row_px + 7u) / 8u, ctx->unpack_alignment);
    const size_t bit = (size_t)ctx->unpack_skip_pixels + (size_t)x;
    const GLubyte byte = bits[((size_t)ctx->unpack_skip_rows + (size_t)y) * stride + bit / 8u];
    const unsigned shift = ctx->unpack_lsb_first ? (unsigned)(bit % 8u) : 7u - (unsigned)(bit % 8u);
    return (GLboolean)((byte >> shift) & 1u);
}

void *gl_bitmap_copy_client(const gl_context_t *ctx, const GLubyte *bits, GLsizei width,
                            GLsizei height) {
    if (!bits || width <= 0 || height <= 0) return (void *)0;
    const size_t row = ((size_t)width + 7u) / 8u;
    uint8_t *img = (uint8_t *)gl_list_alloc(row * (size_t)height);
    if (!img) {
        gl_record_error(gl_get_ctx(), GL_OUT_OF_MEMORY);
        return (void *)0;
    }
    memset(img, 0, row * (size_t)height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (gl_bitmap_bit(ctx, bits, width, x, y)) {
                img[(size_t)y * row + (size_t)x / 8u] |= (uint8_t)(0x80u >> (x % 8));
            }
        }
    }
    return img;
}

void gl_unpack_neutral(gl_context_t *ctx, gl_unpack_saved_t *saved) {
    saved->alignment = ctx->unpack_alignment;
    saved->row_length = ctx->unpack_row_length;
    saved->image_height = ctx->unpack_image_height;
    saved->skip_rows = ctx->unpack_skip_rows;
    saved->skip_pixels = ctx->unpack_skip_pixels;
    saved->skip_images = ctx->unpack_skip_images;
    saved->swap_bytes = ctx->unpack_swap_bytes;
    saved->lsb_first = ctx->unpack_lsb_first;
    ctx->unpack_alignment = 1;
    ctx->unpack_row_length = 0;
    ctx->unpack_image_height = 0;
    ctx->unpack_skip_rows = 0;
    ctx->unpack_skip_pixels = 0;
    ctx->unpack_skip_images = 0;
    ctx->unpack_swap_bytes = GL_FALSE;
    ctx->unpack_lsb_first = GL_FALSE;
}

void gl_unpack_restore(gl_context_t *ctx, const gl_unpack_saved_t *saved) {
    ctx->unpack_alignment = saved->alignment;
    ctx->unpack_row_length = saved->row_length;
    ctx->unpack_image_height = saved->image_height;
    ctx->unpack_skip_rows = saved->skip_rows;
    ctx->unpack_skip_pixels = saved->skip_pixels;
    ctx->unpack_skip_images = saved->skip_images;
    ctx->unpack_swap_bytes = saved->swap_bytes;
    ctx->unpack_lsb_first = saved->lsb_first;
}

/* -------------------------------------------------------------------------
 * Texture internal formats
 *
 * Which formats exist and what each one's base format is are Mesa's
 * `_mesa_base_tex_format` (main/glformats.c:2408) for a compatibility context, cut to GL 1.x:
 * the sized formats of GL 1.1, the legacy 1 to 4, GL 1.3's generic compressed formats, and GL
 * 1.4's depth formats.
 * ------------------------------------------------------------------------- */

GLenum gl_tex_base_format(GLint internalformat) {
    switch (internalformat) {
        case GL_ALPHA: case GL_ALPHA4: case GL_ALPHA8: case GL_ALPHA12: case GL_ALPHA16:
        case GL_COMPRESSED_ALPHA:
            return GL_ALPHA;
        case 1: case GL_LUMINANCE: case GL_LUMINANCE4: case GL_LUMINANCE8: case GL_LUMINANCE12:
        case GL_LUMINANCE16: case GL_COMPRESSED_LUMINANCE:
            return GL_LUMINANCE;
        case 2: case GL_LUMINANCE_ALPHA: case GL_LUMINANCE4_ALPHA4: case GL_LUMINANCE6_ALPHA2:
        case GL_LUMINANCE8_ALPHA8: case GL_LUMINANCE12_ALPHA4: case GL_LUMINANCE12_ALPHA12:
        case GL_LUMINANCE16_ALPHA16: case GL_COMPRESSED_LUMINANCE_ALPHA:
            return GL_LUMINANCE_ALPHA;
        case GL_INTENSITY: case GL_INTENSITY4: case GL_INTENSITY8: case GL_INTENSITY12:
        case GL_INTENSITY16: case GL_COMPRESSED_INTENSITY:
            return GL_INTENSITY;
        case 3: case GL_RGB: case GL_R3_G3_B2: case GL_RGB4: case GL_RGB5: case GL_RGB8:
        case GL_RGB10: case GL_RGB12: case GL_RGB16: case GL_COMPRESSED_RGB:
            return GL_RGB;
        case 4: case GL_RGBA: case GL_RGBA2: case GL_RGBA4: case GL_RGB5_A1: case GL_RGBA8:
        case GL_RGB10_A2: case GL_RGBA12: case GL_RGBA16: case GL_COMPRESSED_RGBA:
            return GL_RGBA;
        /* GL 1.4's depth formats, stored as 32-bit floats whichever size is named. */
        case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24:
        case GL_DEPTH_COMPONENT32:
            return GL_DEPTH_COMPONENT;
        default:
            return 0u;
    }
}

/* The specification's conversion from RGBA to internal components takes luminance and intensity
 * from red (GL 1.3, table 3.15), so a GL_RGB image uploaded as GL_LUMINANCE keeps its red. */
void gl_tex_rebase_row(uint8_t *rgba, GLsizei width, GLenum base) {
    for (GLsizei i = 0; i < width; i++) {
        uint8_t *p = rgba + (size_t)i * 4u;
        switch (base) {
            case GL_ALPHA:           p[0] = p[1] = p[2] = 0u; break;
            case GL_LUMINANCE:       p[1] = p[2] = p[0]; p[3] = 255u; break;
            case GL_LUMINANCE_ALPHA: p[1] = p[2] = p[0]; break;
            case GL_INTENSITY:       p[1] = p[2] = p[3] = p[0]; break;
            case GL_RGB:             p[3] = 255u; break;
            default:                 break;
        }
    }
}

void gl_tex_rebase_f(float c[4], GLenum base) {
    switch (base) {
        case GL_ALPHA:           c[0] = c[1] = c[2] = 0.0f; break;
        case GL_LUMINANCE:       c[1] = c[2] = c[0]; c[3] = 1.0f; break;
        case GL_LUMINANCE_ALPHA: c[1] = c[2] = c[0]; break;
        case GL_INTENSITY:       c[1] = c[2] = c[3] = c[0]; break;
        case GL_RGB:             c[3] = 1.0f; break;
        default:                 break;
    }
}

void gl_tex_readback_row(uint8_t *rgba, GLsizei width, GLenum base) {
    if (base != GL_LUMINANCE && base != GL_LUMINANCE_ALPHA && base != GL_INTENSITY) return;
    for (GLsizei i = 0; i < width; i++) {
        uint8_t *p = rgba + (size_t)i * 4u;
        p[1] = p[2] = 0u;
        if (base != GL_LUMINANCE_ALPHA) p[3] = 255u;
    }
}
