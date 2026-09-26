/*
 * oops-gl: vertex arrays, immediate mode, primitive processing and rasterisation.
 *
 * Every GL primitive reaches the draw path as triangles. Each triangle is lit, clipped
 * and projected on the CPU (the gl_tri_* stages), then either rasterised in software or
 * written to the command stream for the GPU (the gl_hw_tri_* stages).
 */

#include "gl_internal.h"
#ifndef OOPS_HOST_BUILD
#include "oops/time.h"   /* the draw path is timed - see hw_draw_ns */
#include "oops/system.h" /* oops_log_level_t, for gating the per-draw trace */
#else
/* The desktop harness's geometry dump writes through stdio; the payload has neither. */
#include <stdio.h>
#include <stdlib.h>
#endif

/*
 * The pixel shader runs wave32, and SPI_PS_IN_CONTROL.PS_W32_EN tells the hardware so.
 * Every generated shader masks with `exec_lo`/`vcc_lo` (`tools/shader/README.md`). With
 * the bit clear the shader is dispatched wave64, and lanes 32..63 (the lower 8x4 half
 * of each 8x8 block) run every masked branch unconditionally.
 *
 * Bit 15, `S_0286D8_PS_W32_EN`, gfx10/gfx103, `mesa/src/amd/common/amdgfxregs.h:12411`;
 * radv sets it from the shader's wave size at `mesa/src/amd/vulkan/radv_shader.c:2008`.
 * The register is written whole, so every write of it sets the bit.
 */
#define GL_SPI_PS_W32_EN 0x00008000u

/* -------------------------------------------------------------------------
 * Drawable modes
 *
 * Every GL 1.x mode reaches the hardware as triangles. GL_TRIANGLES, GL_TRIANGLE_STRIP,
 * GL_TRIANGLE_FAN, GL_QUADS, GL_QUAD_STRIP and GL_POLYGON triangulate directly; points
 * and lines are expanded into triangles first (see `gl_draw_line_segment` and
 * `gl_draw_point_square`). The hardware's native one- and two-vertex primitives stall
 * the pipe with this stage configuration, so they are never submitted.
 *
 * Every entry point that takes a mode checks it here first. An unsupported mode is
 * GL_INVALID_ENUM rather than a silent no-op.
 * ------------------------------------------------------------------------- */

static GLboolean gl_mode_is_drawable(GLenum mode) {
    switch (mode) {
    case GL_TRIANGLES:
    case GL_TRIANGLE_STRIP:
    case GL_TRIANGLE_FAN:
    case GL_QUADS:
    case GL_QUAD_STRIP:
    case GL_POLYGON:
    /* Points and lines reach the hardware as triangles too (`gl_draw_line_segment`,
     * `gl_draw_point_square`). */
    case GL_POINTS:
    case GL_LINES:
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        return GL_TRUE;
    default:
        return GL_FALSE;
    }
}

/* Checks a mode and records the refusal on the context. Returns whether to go ahead. */
static GLboolean gl_accept_mode(gl_context_t *ctx, GLenum mode) {
    if (gl_mode_is_drawable(mode))
        return GL_TRUE;
    gl_record_error(ctx, GL_INVALID_ENUM);
    return GL_FALSE;
}

/* The index types glDrawElements can read. Checked because `gl_element_index` treats
 * every type other than 8- and 16-bit as 32-bit, and would read past a smaller array.
 */
static GLboolean gl_index_type_is_readable(GLenum type) {
    return (GLboolean)(type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT ||
                       type == GL_UNSIGNED_INT);
}

/* -------------------------------------------------------------------------
 * Client-Side Vertex Arrays
 * ------------------------------------------------------------------------- */

void glEnableClientState(GLenum array) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    switch (array) {
    case GL_VERTEX_ARRAY:
        ctx->array_vertex.enabled = GL_TRUE;
        break;
    case GL_COLOR_ARRAY:
        ctx->array_color.enabled = GL_TRUE;
        break;
    case GL_NORMAL_ARRAY:
        ctx->array_normal.enabled = GL_TRUE;
        break;
    case GL_TEXTURE_COORD_ARRAY: /* the client active unit's (GL 1.3, 2.8) */
        ctx->array_texcoord[ctx->client_active_texture].enabled = GL_TRUE;
        break;
    case GL_EDGE_FLAG_ARRAY:
        ctx->array_edge_flag.enabled = GL_TRUE;
        break;
    case GL_INDEX_ARRAY:
        ctx->array_index.enabled = GL_TRUE;
        break;
    case GL_SECONDARY_COLOR_ARRAY:
        ctx->array_secondary.enabled = GL_TRUE;
        break;
    case GL_FOG_COORD_ARRAY:
        ctx->array_fog_coord.enabled = GL_TRUE;
        break;
    /* Same reasoning as glEnable: an array this subset does not keep is refused rather
     * than dropped, so a caller asking for one finds out from glGetError() instead of
     * from geometry that renders without it. */
    default:
        gl_record_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

void glDisableClientState(GLenum array) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    switch (array) {
    case GL_VERTEX_ARRAY:
        ctx->array_vertex.enabled = GL_FALSE;
        break;
    case GL_COLOR_ARRAY:
        ctx->array_color.enabled = GL_FALSE;
        break;
    case GL_NORMAL_ARRAY:
        ctx->array_normal.enabled = GL_FALSE;
        break;
    case GL_TEXTURE_COORD_ARRAY:
        ctx->array_texcoord[ctx->client_active_texture].enabled = GL_FALSE;
        break;
    case GL_EDGE_FLAG_ARRAY:
        ctx->array_edge_flag.enabled = GL_FALSE;
        break;
    case GL_INDEX_ARRAY:
        ctx->array_index.enabled = GL_FALSE;
        break;
    case GL_SECONDARY_COLOR_ARRAY:
        ctx->array_secondary.enabled = GL_FALSE;
        break;
    case GL_FOG_COORD_ARRAY:
        ctx->array_fog_coord.enabled = GL_FALSE;
        break;
    default:
        gl_record_error(ctx, GL_INVALID_ENUM);
        break;
    }
}

/* One GLboolean per element; a stride of zero means tightly packed, which for this type
 * is one byte. Client state, so not compiled into a list. */
void glEdgeFlagPointer(GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (stride < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    ctx->array_edge_flag.size = 1;
    ctx->array_edge_flag.type = GL_UNSIGNED_BYTE;
    ctx->array_edge_flag.stride = stride;
    ctx->array_edge_flag.pointer = pointer;
    ctx->array_edge_flag.buffer = ctx->bound_array_buffer;
}

/* The index array's element size, or 0 for a type GL does not allow here. */
static size_t gl_index_type_bytes(GLenum type) {
    switch (type) {
    case GL_UNSIGNED_BYTE:
        return sizeof(GLubyte);
    case GL_SHORT:
        return sizeof(GLshort);
    case GL_INT:
        return sizeof(GLint);
    case GL_FLOAT:
        return sizeof(GLfloat);
    case GL_DOUBLE:
        return sizeof(GLdouble);
    default:
        return 0u;
    }
}

/* The colour-index array. Client state, validated as the specification lists its types.
 * Read only when an array element is compiled into a display list, where it becomes the
 * glIndex call it stands for. */
void glIndexPointer(GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (gl_index_type_bytes(type) == 0u) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (stride < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    ctx->array_index.size = 1;
    ctx->array_index.type = type;
    ctx->array_index.stride = stride;
    ctx->array_index.pointer = pointer;
    ctx->array_index.buffer = ctx->bound_array_buffer;
}

/* Each pointer call captures the GL_ARRAY_BUFFER binding at the time it is made, as GL
 * requires, so arrays set under different bindings read from different buffers. */
/* Array types and sizes are checked as Mesa's varray.c checks them for GL 1.x: vertex
 * SHORT, INT, FLOAT or DOUBLE in 2 to 4 components (varray.c:1180-1193), normal BYTE,
 * SHORT, INT, FLOAT or DOUBLE (:1250-1262), colour any of the eight in 3 or 4
 * (:1330-1350), texture coordinate SHORT, INT, FLOAT or DOUBLE in 1 to 4 (:1615-1629).
 * A bad type is an enum error (:918); a bad size or negative stride a value error.
 * `types` is a mask of the bits below. */
enum {
    GL_AT_BYTE = 1,
    GL_AT_UBYTE = 2,
    GL_AT_SHORT = 4,
    GL_AT_USHORT = 8,
    GL_AT_INT = 16,
    GL_AT_UINT = 32,
    GL_AT_FLOAT = 64,
    GL_AT_DOUBLE = 128
};

static unsigned gl_array_type_bit(GLenum type) {
    switch (type) {
    case GL_BYTE:
        return GL_AT_BYTE;
    case GL_UNSIGNED_BYTE:
        return GL_AT_UBYTE;
    case GL_SHORT:
        return GL_AT_SHORT;
    case GL_UNSIGNED_SHORT:
        return GL_AT_USHORT;
    case GL_INT:
        return GL_AT_INT;
    case GL_UNSIGNED_INT:
        return GL_AT_UINT;
    case GL_FLOAT:
        return GL_AT_FLOAT;
    case GL_DOUBLE:
        return GL_AT_DOUBLE;
    default:
        return 0u;
    }
}

static GLboolean gl_array_pointer_ok(gl_context_t *ctx, unsigned types, GLint size,
                                     GLint size_min, GLint size_max, GLenum type,
                                     GLsizei stride) {
    if ((gl_array_type_bit(type) & types) == 0u) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return GL_FALSE;
    }
    if (size < size_min || size > size_max || stride < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return GL_FALSE;
    }
    return GL_TRUE;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (!gl_array_pointer_ok(ctx, GL_AT_SHORT | GL_AT_INT | GL_AT_FLOAT | GL_AT_DOUBLE,
                             size, 2, 4, type, stride)) {
        return;
    }
    ctx->array_vertex.size = size;
    ctx->array_vertex.type = type;
    ctx->array_vertex.stride = stride;
    ctx->array_vertex.pointer = pointer;
    ctx->array_vertex.buffer = ctx->bound_array_buffer;
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (!gl_array_pointer_ok(ctx, 0xffu, size, 3, 4, type, stride))
        return;
    ctx->array_color.size = size;
    ctx->array_color.type = type;
    ctx->array_color.stride = stride;
    ctx->array_color.pointer = pointer;
    ctx->array_color.buffer = ctx->bound_array_buffer;
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (!gl_array_pointer_ok(ctx, GL_AT_SHORT | GL_AT_INT | GL_AT_FLOAT | GL_AT_DOUBLE,
                             size, 1, 4, type, stride)) {
        return;
    }
    /* The client active unit's array (GL 1.3, 2.8). */
    gl_client_array_t *a = &ctx->array_texcoord[ctx->client_active_texture];
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->pointer = pointer;
    a->buffer = ctx->bound_array_buffer;
}

void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (!gl_array_pointer_ok(
            ctx, GL_AT_BYTE | GL_AT_SHORT | GL_AT_INT | GL_AT_FLOAT | GL_AT_DOUBLE, 3,
            3, 3, type, stride)) {
        return;
    }
    ctx->array_normal.size = 3;
    ctx->array_normal.type = type;
    ctx->array_normal.stride = stride;
    ctx->array_normal.pointer = pointer;
    ctx->array_normal.buffer = ctx->bound_array_buffer;
}

/* GL 1.4's secondary colour array: any of the eight types, normalised as the colour
 * array is. 3 or 4 components, as Mesa accepts them (main/varray.c:1536-1553), where
 * the GL 1.4 table lists only 3. Only three are read: the secondary alpha is unused. */
void glSecondaryColorPointer(GLint size, GLenum type, GLsizei stride,
                             const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (!gl_array_pointer_ok(ctx, 0xffu, size, 3, 4, type, stride))
        return;
    ctx->array_secondary.size = size;
    ctx->array_secondary.type = type;
    ctx->array_secondary.stride = stride;
    ctx->array_secondary.pointer = pointer;
    ctx->array_secondary.buffer = ctx->bound_array_buffer;
}

/* GL 1.4's fog coordinate array: one GL_FLOAT or GL_DOUBLE per element, read as a value
 * (Mesa main/varray.c:1408-1425 - its third legal type, half float, is not GL 1.x's).
 */
void glFogCoordPointer(GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (!gl_array_pointer_ok(ctx, GL_AT_FLOAT | GL_AT_DOUBLE, 1, 1, 1, type, stride))
        return;
    ctx->array_fog_coord.size = 1;
    ctx->array_fog_coord.type = type;
    ctx->array_fog_coord.stride = stride;
    ctx->array_fog_coord.pointer = pointer;
    ctx->array_fog_coord.buffer = ctx->bound_array_buffer;
}

/* `glInterleavedArrays(format, stride, pointer)` - all four arrays from one buffer.
 *
 * Implemented as the enable, disable and pointer calls the specification defines it as.
 * Arrays the format does not name are disabled, as the specification requires, so no
 * stale pointer from an earlier setup is read. A `stride` of 0 means the format's own
 * packed size.
 */
void glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;

    /* Component counts, in the order they appear in the buffer: texture coordinates
     * first, then colour, then normal, then position - which is what the format names
     * spell out backwards. A count of 0 means the format does not carry that array. */
    int tc = 0, cc = 0, nc = 0, vc = 0;
    GLboolean colour_is_bytes = GL_FALSE;

    switch (format) {
    case GL_V2F:
        vc = 2;
        break;
    case GL_V3F:
        vc = 3;
        break;
    case GL_C4UB_V2F:
        cc = 4;
        colour_is_bytes = GL_TRUE;
        vc = 2;
        break;
    case GL_C4UB_V3F:
        cc = 4;
        colour_is_bytes = GL_TRUE;
        vc = 3;
        break;
    case GL_C3F_V3F:
        cc = 3;
        vc = 3;
        break;
    case GL_N3F_V3F:
        nc = 3;
        vc = 3;
        break;
    case GL_C4F_N3F_V3F:
        cc = 4;
        nc = 3;
        vc = 3;
        break;
    case GL_T2F_V3F:
        tc = 2;
        vc = 3;
        break;
    case GL_T4F_V4F:
        tc = 4;
        vc = 4;
        break;
    case GL_T2F_C4UB_V3F:
        tc = 2;
        cc = 4;
        colour_is_bytes = GL_TRUE;
        vc = 3;
        break;
    case GL_T2F_C3F_V3F:
        tc = 2;
        cc = 3;
        vc = 3;
        break;
    case GL_T2F_N3F_V3F:
        tc = 2;
        nc = 3;
        vc = 3;
        break;
    case GL_T2F_C4F_N3F_V3F:
        tc = 2;
        cc = 4;
        nc = 3;
        vc = 3;
        break;
    case GL_T4F_C4F_N3F_V4F:
        tc = 4;
        cc = 4;
        nc = 3;
        vc = 4;
        break;
    default:
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (stride < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    /* A C4UB colour occupies four bytes rounded up to a float, because the
     * specification aligns what follows it to a float boundary. The offsets match
     * Mesa's table (`src/mesa/main/varray.c`, `_mesa_get_interleaved_layout`), which
     * computes `c = f * ((4 * sizeof(GLubyte) + (f - 1)) / f)`. */
    const size_t f = sizeof(GLfloat);
    const size_t ub4 = f * ((4u * sizeof(GLubyte) + (f - 1u)) / f);
    const size_t colour_bytes = colour_is_bytes ? ub4 : (size_t)cc * f;

    const size_t off_t = 0u;
    const size_t off_c = off_t + (size_t)tc * f;
    /* `colour_bytes` is zero when the format has no colour: `colour_is_bytes` is set
     * only in the C4UB cases. */
    const size_t off_n = off_c + colour_bytes;
    const size_t off_v = off_n + (size_t)nc * f;
    const size_t packed = off_v + (size_t)vc * f;

    const GLsizei real_stride = stride ? stride : (GLsizei)packed;
    const uint8_t *base = (const uint8_t *)pointer;

    /* No interleaved format carries edge flags or colour indices, and the specification
     * disables both arrays here too (Mesa main/varray.c:2960-2961). */
    glDisableClientState(GL_EDGE_FLAG_ARRAY);
    glDisableClientState(GL_INDEX_ARRAY);

    if (tc) {
        glTexCoordPointer(tc, GL_FLOAT, real_stride, base ? base + off_t : NULL);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    } else {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }

    if (cc) {
        glColorPointer(cc, colour_is_bytes ? GL_UNSIGNED_BYTE : GL_FLOAT, real_stride,
                       base ? base + off_c : NULL);
        glEnableClientState(GL_COLOR_ARRAY);
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
    }

    if (nc) {
        glNormalPointer(GL_FLOAT, real_stride, base ? base + off_n : NULL);
        glEnableClientState(GL_NORMAL_ARRAY);
    } else {
        glDisableClientState(GL_NORMAL_ARRAY);
    }

    glVertexPointer(vc, GL_FLOAT, real_stride, base ? base + off_v : NULL);
    glEnableClientState(GL_VERTEX_ARRAY);
}

/* -------------------------------------------------------------------------
 * Immediate Mode API
 * ------------------------------------------------------------------------- */

void glBegin(GLenum mode) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_BEGIN, gl_la_e(mode)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    /* Refused here rather than at glEnd, so the vertices in between are never collected
     * and the error is attributable to the call that caused it. `imm_active` stays
     * false, which makes glVertex*() and glEnd() no-ops for this block. */
    if (!gl_accept_mode(ctx, mode)) {
        ctx->imm_active = GL_FALSE;
        ctx->imm_count = 0;
        return;
    }
    ctx->imm_mode = mode;
    ctx->imm_active = GL_TRUE;
    ctx->imm_count = 0;
}

void glVertex2f(GLfloat x, GLfloat y) {
    glVertex4f(x, y, 0.0f, 1.0f);
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    glVertex4f(x, y, z, 1.0f);
}

void glVertex3fv(const GLfloat *v) {
    if (v)
        glVertex4f(v[0], v[1], v[2], 1.0f);
}

void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_VERTEX, gl_la_f(x), gl_la_f(y), gl_la_f(z), gl_la_f(w)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !ctx->imm_active)
        return;
    if (ctx->imm_count >= OOPS_GL_MAX_IMMEDIATE_VERTS)
        return;

    gl_vertex_t *v = &ctx->imm_verts[ctx->imm_count++];
    v->x = (float)x;
    v->y = (float)y;
    v->z = (float)z;
    v->w = (float)w;
    v->r = ctx->cur_color[0];
    v->g = ctx->cur_color[1];
    v->b = ctx->cur_color[2];
    v->a = ctx->cur_color[3];
    v->nx = ctx->cur_normal[0];
    v->ny = ctx->cur_normal[1];
    v->nz = ctx->cur_normal[2];
    v->edge = ctx->cur_edge_flag;
    v->sr = ctx->cur_secondary[0];
    v->sg = ctx->cur_secondary[1];
    v->sb = ctx->cur_secondary[2];
    v->fogc = ctx->cur_fog_coord;
    /* GL 2.0's generic attributes, latched with the vertex like every other current
     * value. In immediate mode they are always `glVertexAttrib`'s. Skipped until this
     * context has made a shader or a program (see `gl2_used`). */
    if (ctx->gl2_used) {
        for (int ai = 0; ai < OOPS_GL_MAX_VERTEX_ATTRIBS; ai++) {
            for (int k = 0; k < 4; k++)
                v->attrib[ai][k] = ctx->vertex_attribs[ai].current[k];
        }
    }
    gl_vertex_texcoord(ctx, v, ctx->cur_texcoord);
}

/* glEdgeFlag - a vertex attribute like the colour, so compiled into lists and saved
 * with GL_CURRENT_BIT. It marks whether the polygon edge starting at the next vertex is
 * a boundary edge; only glPolygonMode's GL_LINE and GL_POINT read it. */
void glEdgeFlag(GLboolean flag) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_EDGE_FLAG, gl_la_u(flag)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    ctx->cur_edge_flag = flag ? GL_TRUE : GL_FALSE;
}

void glEdgeFlagv(const GLboolean *flag) {
    if (flag)
        glEdgeFlag(*flag);
}

/* glIndex - the current colour index. A vertex attribute like the colour, so compiled
 * into lists and saved with GL_CURRENT_BIT, and in an RGBA context kept and never drawn
 * with. Every spelling is a plain cast to float, not a normalisation: an index is a
 * number, not an intensity (Mesa vbo/vbo_attrib_tmp.h:1978-1990 and :2705-2761). */
void glIndexf(GLfloat c) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_INDEX, gl_la_f(c)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    ctx->cur_index = c;
}
void glIndexd(GLdouble c) {
    glIndexf((GLfloat)c);
}
void glIndexi(GLint c) {
    glIndexf((GLfloat)c);
}
void glIndexs(GLshort c) {
    glIndexf((GLfloat)c);
}
void glIndexub(GLubyte c) {
    glIndexf((GLfloat)c);
}
void glIndexfv(const GLfloat *c) {
    if (c)
        glIndexf(c[0]);
}
void glIndexdv(const GLdouble *c) {
    if (c)
        glIndexf((GLfloat)c[0]);
}
void glIndexiv(const GLint *c) {
    if (c)
        glIndexf((GLfloat)c[0]);
}
void glIndexsv(const GLshort *c) {
    if (c)
        glIndexf((GLfloat)c[0]);
}
void glIndexubv(const GLubyte *c) {
    if (c)
        glIndexf((GLfloat)c[0]);
}

/* PA_CL_UCP_0_X and the 23 registers after it - six planes of four floats, at context
 * offset 0x16F (byte 0x0285BC, mesa/src/amd/registers/gfx103.json). Consecutive, so one
 * SET_CONTEXT_REG carries them all. A disabled plane is written as zero, so no stale
 * plane reappears when its PA_CL_CLIP_CNTL enable bit is set later. */
void gl_hw_emit_clip_planes(const gl_context_t *ctx, uint32_t **dw_ptr) {
    uint32_t *dw = *dw_ptr;
    *dw++ = 0xc0186900u; /* PACKET3_SET_CONTEXT_REG, 24 data dwords */
    *dw++ = 0x16fu;      /* mmPA_CL_UCP_0_X */
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (ctx->clip_plane_enabled[i]) {
            (void)gl_compute_clip_plane_hw(ctx, i, p);
        }
        for (int k = 0; k < 4; k++)
            *dw++ = gl_f32_bits(p[k]);
    }
    *dw_ptr = dw;
}

GLboolean gl_compute_clip_plane_hw(const gl_context_t *ctx, int i, float out[4]) {
    if (!ctx || !out || i < 0 || i >= OOPS_GL_CLIP_PLANE_COUNT)
        return GL_FALSE;
    gl_mat4_t pinv;
    if (!mat4_invert(&pinv, &ctx->projection_stack[ctx->projection_depth]))
        return GL_FALSE;
    const float *p = ctx->clip_plane[i];
    /* A plane is a row vector: p_clip = p_eye * P^-1. */
    for (int k = 0; k < 4; k++) {
        out[k] = p[0] * pinv.m[k * 4 + 0] + p[1] * pinv.m[k * 4 + 1] +
                 p[2] * pinv.m[k * 4 + 2] + p[3] * pinv.m[k * 4 + 3];
    }
    return GL_TRUE;
}

/* Texture coordinate generation, computed at the vertex where the object coordinates,
 * the normal and their modelview are all in hand. Only the coordinates whose generation
 * is enabled are replaced; the rest keep the vertex's own, so a program can generate s
 * and supply t. */
static void gl_generate_texcoord(const gl_context_t *ctx, const gl_tex_unit_t *tu,
                                 const gl_vertex_t *v, float gen[4]);

/* A unit's texture coordinate for a vertex: generated where generation is on, then
 * through the texture matrix, which GL applies to every coordinate, supplied or
 * generated. */
void gl_vertex_texcoord4(const gl_context_t *ctx, GLuint unit, const gl_vertex_t *v,
                         const float tc[4], float out[4]) {
    const gl_tex_unit_t *tu = &ctx->tex_unit[unit];
    float gen[4] = {tc[0], tc[1], tc[2], tc[3]};
    if (tu->texgen_enabled[0] || tu->texgen_enabled[1] || tu->texgen_enabled[2] ||
        tu->texgen_enabled[3]) {
        gl_generate_texcoord(ctx, tu, v, gen);
    }
    mat4_transform_vec4(out, &tu->texture_stack[tu->texture_depth], gen);
}

void gl_vertex_texcoord(const gl_context_t *ctx, gl_vertex_t *v,
                        float tcs[OOPS_GL_MAX_TEXTURE_UNITS][4]) {
    if (!ctx || !v || !tcs)
        return;
    /* Kept undivided, all four: the rasteriser interpolates s, t, r and q and divides
     * per fragment, as GL does. */
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        float gen[4];
        gl_vertex_texcoord4(ctx, u, v, tcs[u], gen);
        for (int k = 0; k < 4; k++)
            v->tc[u][k] = gen[k];
    }
}

static void gl_generate_texcoord(const gl_context_t *ctx, const gl_tex_unit_t *tu,
                                 const gl_vertex_t *v, float gen[4]) {
    const float obj[4] = {v->x, v->y, v->z, v->w};
    const gl_mat4_t *mv = &ctx->modelview_stack[ctx->modelview_depth];

    float eye[4];
    mat4_transform_vec4(eye, mv, obj);

    /* The eye-space normal - the one lighting uses, through the inverse-transpose and
     * unit length only under GL_NORMALIZE or GL_RESCALE_NORMAL, as Mesa's
     * fixed-function program takes it for every generation mode. */
    float nm[9];
    gl_normal_matrix_of(mv, nm);
    const float obj_n[3] = {v->nx, v->ny, v->nz};
    float ne[3];
    gl_eye_normal(ctx, nm, obj_n, ne);

    /* The reflection of the eye vector - from the eye to the vertex, normalised - about
     * the normal: r = u - 2n(n.u). Sphere mapping scales it into [0, 1] by the sphere's
     * own radius term; GL 1.3's reflection map is r itself. Computed once and only if
     * something asks. */
    float sphere_s = 0.0f, sphere_t = 0.0f;
    float refl[3] = {0.0f, 0.0f, 0.0f};
    GLboolean wants_reflect = GL_FALSE;
    for (int i = 0; i < 3; i++) {
        if (tu->texgen_enabled[i] && (tu->texgen_mode[i] == GL_SPHERE_MAP ||
                                      tu->texgen_mode[i] == GL_REFLECTION_MAP)) {
            wants_reflect = GL_TRUE;
        }
    }
    if (wants_reflect) {
        float u[3] = {eye[0], eye[1], eye[2]};
        float ulen = gl_sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (ulen > 0.0f) {
            u[0] /= ulen;
            u[1] /= ulen;
            u[2] /= ulen;
        }
        const float d = 2.0f * (u[0] * ne[0] + u[1] * ne[1] + u[2] * ne[2]);
        const float r0 = u[0] - ne[0] * d;
        const float r1 = u[1] - ne[1] * d;
        const float r2 = u[2] - ne[2] * d;
        refl[0] = r0;
        refl[1] = r1;
        refl[2] = r2;
        const float mterm =
            2.0f * gl_sqrt(r0 * r0 + r1 * r1 + (r2 + 1.0f) * (r2 + 1.0f));
        if (mterm != 0.0f) {
            sphere_s = r0 / mterm + 0.5f;
            sphere_t = r1 / mterm + 0.5f;
        } else {
            /* The degenerate case is the pole directly behind the eye, where the sphere
             * map has no defined coordinate. The centre is the least surprising answer.
             */
            sphere_s = 0.5f;
            sphere_t = 0.5f;
        }
    }

    for (int i = 0; i < 4; i++) {
        if (!tu->texgen_enabled[i])
            continue;
        switch (tu->texgen_mode[i]) {
        case GL_OBJECT_LINEAR: {
            const float *p = tu->texgen_object_plane[i];
            gen[i] = p[0] * obj[0] + p[1] * obj[1] + p[2] * obj[2] + p[3] * obj[3];
            break;
        }
        case GL_EYE_LINEAR: {
            /* The plane was already put through the inverse modelview when it was set.
             */
            const float *p = tu->texgen_eye_plane[i];
            gen[i] = p[0] * eye[0] + p[1] * eye[1] + p[2] * eye[2] + p[3] * eye[3];
            break;
        }
        case GL_SPHERE_MAP:
            gen[i] = (i == 0) ? sphere_s : sphere_t;
            break;
        /* GL 1.3's cube-map modes, s, t and r only (glTexGen refuses q): the reflection
         * and the eye-space normal, each a direction a cube map is looked up by. */
        case GL_REFLECTION_MAP:
            if (i < 3)
                gen[i] = refl[i];
            break;
        case GL_NORMAL_MAP:
            if (i < 3)
                gen[i] = ne[i];
            break;
        default:
            break;
        }
    }
}

void glColor3f(GLfloat red, GLfloat green, GLfloat blue) {
    glColor4f(red, green, blue, 1.0f);
}

void glColor3fv(const GLfloat *v) {
    if (v)
        glColor4f(v[0], v[1], v[2], 1.0f);
}

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_COLOR, gl_la_f(red), gl_la_f(green), gl_la_f(blue),
                    gl_la_f(alpha))) {
        return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    ctx->cur_color[0] = (float)red;
    ctx->cur_color[1] = (float)green;
    ctx->cur_color[2] = (float)blue;
    ctx->cur_color[3] = (float)alpha;
    /* GL_COLOR_MATERIAL: the new colour is the tracked material's too. */
    if (ctx->cap_color_material)
        gl_color_material_update(ctx);
}

void glColor4fv(const GLfloat *v) {
    if (v)
        glColor4f(v[0], v[1], v[2], v[3]);
}

/* Integer colours and normals are normalised: signed types onto [-1, 1] and unsigned
 * onto [0, 1], so the extreme value lands exactly on the endpoint (divide by 255, not
 * 256). From mesa/src/mesa/main/macros.h:49-104, including GLint's asymmetric divisor:
 * unsigned divides by 2^32-1 and signed by 2^32-2. Positions and texture coordinates
 * are taken as given. */
static inline GLfloat gl_b_to_f(GLbyte v) {
    return (GLfloat)((2.0 * (double)v + 1.0) / 255.0);
}
static inline GLfloat gl_s_to_f(GLshort v) {
    return (GLfloat)((2.0 * (double)v + 1.0) / 65535.0);
}
static inline GLfloat gl_i_to_f(GLint v) {
    return (GLfloat)((2.0 * (double)v + 1.0) / 4294967294.0);
}
static inline GLfloat gl_ub_to_f(GLubyte v) {
    return (GLfloat)((double)v / 255.0);
}
static inline GLfloat gl_us_to_f(GLushort v) {
    return (GLfloat)((double)v / 65535.0);
}
static inline GLfloat gl_ui_to_f(GLuint v) {
    return (GLfloat)((double)v / 4294967295.0);
}

void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) {
    glColor4f(gl_ub_to_f(red), gl_ub_to_f(green), gl_ub_to_f(blue), gl_ub_to_f(alpha));
}

/* The four-component form is the one implementation; every other arity and type
 * forwards here, so a display list records one operation and the spellings cannot drift
 * apart. */
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_TEXCOORD, gl_la_f(s), gl_la_f(t),
                                           gl_la_f(r), gl_la_f(q)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    /* Stored as given, so a query answers s, not s/q; the rasteriser divides by q per
     * fragment. Unit 0's, whichever unit is active: glTexCoord is
     * glMultiTexCoord(GL_TEXTURE0) (GL 1.3, 2.7). */
    ctx->cur_texcoord[0][0] = (float)s;
    ctx->cur_texcoord[0][1] = (float)t;
    ctx->cur_texcoord[0][2] = (float)r;
    ctx->cur_texcoord[0][3] = (float)q;
}

void glTexCoord2f(GLfloat s, GLfloat t) {
    glTexCoord4f(s, t, 0.0f, 1.0f);
}
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) {
    glTexCoord4f(s, t, r, 1.0f);
}

void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_NORMAL, gl_la_f(nx), gl_la_f(ny), gl_la_f(nz)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    ctx->cur_normal[0] = (float)nx;
    ctx->cur_normal[1] = (float)ny;
    ctx->cur_normal[2] = (float)nz;
}

void glNormal3fv(const GLfloat *v) {
    if (v)
        glNormal3f(v[0], v[1], v[2]);
}

/* -------------------------------------------------------------------------
 * The other spellings
 *
 * GL 1.x names each attribute once per C type and once per arity. Every one of these
 * converts and forwards to the `f` sibling above, so each attribute has one
 * implementation and is recorded into a display list by the sibling's `gl_list_rec`.
 *
 * A null vector pointer draws nothing rather than being dereferenced, matching the `fv`
 * forms.
 * ------------------------------------------------------------------------- */

void glVertex2d(GLdouble x, GLdouble y) {
    glVertex4f((GLfloat)x, (GLfloat)y, 0.0f, 1.0f);
}
void glVertex3d(GLdouble x, GLdouble y, GLdouble z) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, 1.0f);
}
void glVertex4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}
void glVertex2i(GLint x, GLint y) {
    glVertex4f((GLfloat)x, (GLfloat)y, 0.0f, 1.0f);
}
void glVertex3i(GLint x, GLint y, GLint z) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, 1.0f);
}
void glVertex2fv(const GLfloat *v) {
    if (v)
        glVertex4f(v[0], v[1], 0.0f, 1.0f);
}
void glVertex4fv(const GLfloat *v) {
    if (v)
        glVertex4f(v[0], v[1], v[2], v[3]);
}
void glVertex2dv(const GLdouble *v) {
    if (v)
        glVertex2d(v[0], v[1]);
}
void glVertex3dv(const GLdouble *v) {
    if (v)
        glVertex3d(v[0], v[1], v[2]);
}
void glVertex2iv(const GLint *v) {
    if (v)
        glVertex2i(v[0], v[1]);
}
void glVertex3iv(const GLint *v) {
    if (v)
        glVertex3i(v[0], v[1], v[2]);
}

void glColor3d(GLdouble red, GLdouble green, GLdouble blue) {
    glColor4f((GLfloat)red, (GLfloat)green, (GLfloat)blue, 1.0f);
}
void glColor4d(GLdouble red, GLdouble green, GLdouble blue, GLdouble alpha) {
    glColor4f((GLfloat)red, (GLfloat)green, (GLfloat)blue, (GLfloat)alpha);
}
/* Alpha is 1.0 exactly, not the converted maximum of the argument type. */
void glColor3ub(GLubyte red, GLubyte green, GLubyte blue) {
    glColor4ub(red, green, blue, 255);
}
void glColor3dv(const GLdouble *v) {
    if (v)
        glColor3d(v[0], v[1], v[2]);
}
void glColor4dv(const GLdouble *v) {
    if (v)
        glColor4d(v[0], v[1], v[2], v[3]);
}
void glColor3ubv(const GLubyte *v) {
    if (v)
        glColor4ub(v[0], v[1], v[2], 255);
}
void glColor4ubv(const GLubyte *v) {
    if (v)
        glColor4ub(v[0], v[1], v[2], v[3]);
}

/* The one-coordinate form leaves t at 0, as the specification says. */
void glTexCoord1f(GLfloat s) {
    glTexCoord2f(s, 0.0f);
}
void glTexCoord2d(GLdouble s, GLdouble t) {
    glTexCoord2f((GLfloat)s, (GLfloat)t);
}
void glTexCoord2i(GLint s, GLint t) {
    glTexCoord2f((GLfloat)s, (GLfloat)t);
}
void glTexCoord2fv(const GLfloat *v) {
    if (v)
        glTexCoord2f(v[0], v[1]);
}
void glTexCoord2dv(const GLdouble *v) {
    if (v)
        glTexCoord2d(v[0], v[1]);
}
void glTexCoord2iv(const GLint *v) {
    if (v)
        glTexCoord2i(v[0], v[1]);
}

void glNormal3d(GLdouble nx, GLdouble ny, GLdouble nz) {
    glNormal3f((GLfloat)nx, (GLfloat)ny, (GLfloat)nz);
}
void glNormal3dv(const GLdouble *v) {
    if (v)
        glNormal3d(v[0], v[1], v[2]);
}

/* -------------------------------------------------------------------------
 * The rest of the type-and-arity grid
 *
 * The payload links with `-Wl,--unresolved-symbols=ignore-all`, so a missing spelling
 * links cleanly and calls address zero at run time. The whole grid is therefore filled.
 *
 * Integer colours and normals are normalised; positions and texture coordinates are
 * not. The conversions are Mesa's (mesa/src/mesa/main/macros.h:49-104); normals use the
 * colour macros (mesa/src/mesa/vbo/vbo_attrib_tmp.h:2770-2795). The signed mapping is
 * (2c+1)/(2^b - 1), so zero does not map to zero: glNormal3b(127,0,0) is
 * (1.0, 1/255, 1/255), as Mesa produces.
 * ------------------------------------------------------------------------- */

/* Positions: the value, unconverted. */
void glVertex2s(GLshort x, GLshort y) {
    glVertex4f((GLfloat)x, (GLfloat)y, 0.0f, 1.0f);
}
void glVertex3s(GLshort x, GLshort y, GLshort z) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, 1.0f);
}
void glVertex4s(GLshort x, GLshort y, GLshort z, GLshort w) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}
void glVertex4i(GLint x, GLint y, GLint z, GLint w) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}
void glVertex2sv(const GLshort *v) {
    if (v)
        glVertex2s(v[0], v[1]);
}
void glVertex3sv(const GLshort *v) {
    if (v)
        glVertex3s(v[0], v[1], v[2]);
}
void glVertex4sv(const GLshort *v) {
    if (v)
        glVertex4s(v[0], v[1], v[2], v[3]);
}
void glVertex4iv(const GLint *v) {
    if (v)
        glVertex4i(v[0], v[1], v[2], v[3]);
}
void glVertex4dv(const GLdouble *v) {
    if (v)
        glVertex4d(v[0], v[1], v[2], v[3]);
}

/* Colours: normalised. The three-component forms set alpha to 1.0 exactly, not the
 * converted maximum of the argument type. */
void glColor3b(GLbyte r, GLbyte g, GLbyte b) {
    glColor4f(gl_b_to_f(r), gl_b_to_f(g), gl_b_to_f(b), 1.0f);
}
void glColor3s(GLshort r, GLshort g, GLshort b) {
    glColor4f(gl_s_to_f(r), gl_s_to_f(g), gl_s_to_f(b), 1.0f);
}
void glColor3i(GLint r, GLint g, GLint b) {
    glColor4f(gl_i_to_f(r), gl_i_to_f(g), gl_i_to_f(b), 1.0f);
}
void glColor3us(GLushort r, GLushort g, GLushort b) {
    glColor4f(gl_us_to_f(r), gl_us_to_f(g), gl_us_to_f(b), 1.0f);
}
void glColor3ui(GLuint r, GLuint g, GLuint b) {
    glColor4f(gl_ui_to_f(r), gl_ui_to_f(g), gl_ui_to_f(b), 1.0f);
}
void glColor4b(GLbyte r, GLbyte g, GLbyte b, GLbyte a) {
    glColor4f(gl_b_to_f(r), gl_b_to_f(g), gl_b_to_f(b), gl_b_to_f(a));
}
void glColor4s(GLshort r, GLshort g, GLshort b, GLshort a) {
    glColor4f(gl_s_to_f(r), gl_s_to_f(g), gl_s_to_f(b), gl_s_to_f(a));
}
void glColor4i(GLint r, GLint g, GLint b, GLint a) {
    glColor4f(gl_i_to_f(r), gl_i_to_f(g), gl_i_to_f(b), gl_i_to_f(a));
}
void glColor4us(GLushort r, GLushort g, GLushort b, GLushort a) {
    glColor4f(gl_us_to_f(r), gl_us_to_f(g), gl_us_to_f(b), gl_us_to_f(a));
}
void glColor4ui(GLuint r, GLuint g, GLuint b, GLuint a) {
    glColor4f(gl_ui_to_f(r), gl_ui_to_f(g), gl_ui_to_f(b), gl_ui_to_f(a));
}
void glColor3bv(const GLbyte *v) {
    if (v)
        glColor3b(v[0], v[1], v[2]);
}
void glColor3sv(const GLshort *v) {
    if (v)
        glColor3s(v[0], v[1], v[2]);
}
void glColor3iv(const GLint *v) {
    if (v)
        glColor3i(v[0], v[1], v[2]);
}
void glColor3usv(const GLushort *v) {
    if (v)
        glColor3us(v[0], v[1], v[2]);
}
void glColor3uiv(const GLuint *v) {
    if (v)
        glColor3ui(v[0], v[1], v[2]);
}
void glColor4bv(const GLbyte *v) {
    if (v)
        glColor4b(v[0], v[1], v[2], v[3]);
}
void glColor4sv(const GLshort *v) {
    if (v)
        glColor4s(v[0], v[1], v[2], v[3]);
}
void glColor4iv(const GLint *v) {
    if (v)
        glColor4i(v[0], v[1], v[2], v[3]);
}
void glColor4usv(const GLushort *v) {
    if (v)
        glColor4us(v[0], v[1], v[2], v[3]);
}
void glColor4uiv(const GLuint *v) {
    if (v)
        glColor4ui(v[0], v[1], v[2], v[3]);
}

/* GL 1.4's secondary colour: three components, normalised exactly as glColor's are
 * (Mesa vbo/vbo_attrib_tmp.h:3258-3316, through the same macros). Compiled into lists
 * and saved with GL_CURRENT_BIT like the primary colour; alpha stays at its initial 1.
 * Unlike the primary colour it is not a tracked material's source - GL_COLOR_MATERIAL
 * reads the primary. */
void glSecondaryColor3f(GLfloat r, GLfloat g, GLfloat b) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_SECONDARY_COLOR, gl_la_f(r), gl_la_f(g), gl_la_f(b)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    ctx->cur_secondary[0] = (float)r;
    ctx->cur_secondary[1] = (float)g;
    ctx->cur_secondary[2] = (float)b;
}
void glSecondaryColor3d(GLdouble r, GLdouble g, GLdouble b) {
    glSecondaryColor3f((GLfloat)r, (GLfloat)g, (GLfloat)b);
}
void glSecondaryColor3b(GLbyte r, GLbyte g, GLbyte b) {
    glSecondaryColor3f(gl_b_to_f(r), gl_b_to_f(g), gl_b_to_f(b));
}
void glSecondaryColor3s(GLshort r, GLshort g, GLshort b) {
    glSecondaryColor3f(gl_s_to_f(r), gl_s_to_f(g), gl_s_to_f(b));
}
void glSecondaryColor3i(GLint r, GLint g, GLint b) {
    glSecondaryColor3f(gl_i_to_f(r), gl_i_to_f(g), gl_i_to_f(b));
}
void glSecondaryColor3ub(GLubyte r, GLubyte g, GLubyte b) {
    glSecondaryColor3f(gl_ub_to_f(r), gl_ub_to_f(g), gl_ub_to_f(b));
}
void glSecondaryColor3us(GLushort r, GLushort g, GLushort b) {
    glSecondaryColor3f(gl_us_to_f(r), gl_us_to_f(g), gl_us_to_f(b));
}
void glSecondaryColor3ui(GLuint r, GLuint g, GLuint b) {
    glSecondaryColor3f(gl_ui_to_f(r), gl_ui_to_f(g), gl_ui_to_f(b));
}
void glSecondaryColor3fv(const GLfloat *v) {
    if (v)
        glSecondaryColor3f(v[0], v[1], v[2]);
}
void glSecondaryColor3dv(const GLdouble *v) {
    if (v)
        glSecondaryColor3d(v[0], v[1], v[2]);
}
void glSecondaryColor3bv(const GLbyte *v) {
    if (v)
        glSecondaryColor3b(v[0], v[1], v[2]);
}
void glSecondaryColor3sv(const GLshort *v) {
    if (v)
        glSecondaryColor3s(v[0], v[1], v[2]);
}
void glSecondaryColor3iv(const GLint *v) {
    if (v)
        glSecondaryColor3i(v[0], v[1], v[2]);
}
void glSecondaryColor3ubv(const GLubyte *v) {
    if (v)
        glSecondaryColor3ub(v[0], v[1], v[2]);
}
void glSecondaryColor3usv(const GLushort *v) {
    if (v)
        glSecondaryColor3us(v[0], v[1], v[2]);
}
void glSecondaryColor3uiv(const GLuint *v) {
    if (v)
        glSecondaryColor3ui(v[0], v[1], v[2]);
}

/* GL 1.4's fog coordinate: a value, not a colour, so nothing converts it. Compiled into
 * lists and saved with GL_CURRENT_BIT; read only while GL_FOG_COORD_SRC is
 * GL_FOG_COORD. */
void glFogCoordf(GLfloat coord) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_FOG_COORD, gl_la_f(coord)))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    ctx->cur_fog_coord = (float)coord;
}
void glFogCoordd(GLdouble coord) {
    glFogCoordf((GLfloat)coord);
}
void glFogCoordfv(const GLfloat *coord) {
    if (coord)
        glFogCoordf(coord[0]);
}
void glFogCoorddv(const GLdouble *coord) {
    if (coord)
        glFogCoordf((GLfloat)coord[0]);
}

/* GL_EXT_secondary_color's and GL_EXT_fog_coord's spellings, for programs that find the
 * extensions in glGetString's list. Each is the GL 1.4 core function. */
void glSecondaryColor3bEXT(GLbyte r, GLbyte g, GLbyte b) {
    glSecondaryColor3b(r, g, b);
}
void glSecondaryColor3bvEXT(const GLbyte *v) {
    glSecondaryColor3bv(v);
}
void glSecondaryColor3dEXT(GLdouble r, GLdouble g, GLdouble b) {
    glSecondaryColor3d(r, g, b);
}
void glSecondaryColor3dvEXT(const GLdouble *v) {
    glSecondaryColor3dv(v);
}
void glSecondaryColor3fEXT(GLfloat r, GLfloat g, GLfloat b) {
    glSecondaryColor3f(r, g, b);
}
void glSecondaryColor3fvEXT(const GLfloat *v) {
    glSecondaryColor3fv(v);
}
void glSecondaryColor3iEXT(GLint r, GLint g, GLint b) {
    glSecondaryColor3i(r, g, b);
}
void glSecondaryColor3ivEXT(const GLint *v) {
    glSecondaryColor3iv(v);
}
void glSecondaryColor3sEXT(GLshort r, GLshort g, GLshort b) {
    glSecondaryColor3s(r, g, b);
}
void glSecondaryColor3svEXT(const GLshort *v) {
    glSecondaryColor3sv(v);
}
void glSecondaryColor3ubEXT(GLubyte r, GLubyte g, GLubyte b) {
    glSecondaryColor3ub(r, g, b);
}
void glSecondaryColor3ubvEXT(const GLubyte *v) {
    glSecondaryColor3ubv(v);
}
void glSecondaryColor3uiEXT(GLuint r, GLuint g, GLuint b) {
    glSecondaryColor3ui(r, g, b);
}
void glSecondaryColor3uivEXT(const GLuint *v) {
    glSecondaryColor3uiv(v);
}
void glSecondaryColor3usEXT(GLushort r, GLushort g, GLushort b) {
    glSecondaryColor3us(r, g, b);
}
void glSecondaryColor3usvEXT(const GLushort *v) {
    glSecondaryColor3usv(v);
}
void glSecondaryColorPointerEXT(GLint size, GLenum type, GLsizei stride,
                                const GLvoid *pointer) {
    glSecondaryColorPointer(size, type, stride, pointer);
}
void glFogCoordfEXT(GLfloat coord) {
    glFogCoordf(coord);
}
void glFogCoordfvEXT(const GLfloat *coord) {
    glFogCoordfv(coord);
}
void glFogCoorddEXT(GLdouble coord) {
    glFogCoordd(coord);
}
void glFogCoorddvEXT(const GLdouble *coord) {
    glFogCoorddv(coord);
}
void glFogCoordPointerEXT(GLenum type, GLsizei stride, const GLvoid *pointer) {
    glFogCoordPointer(type, stride, pointer);
}

/* Normals: normalised, like colours and unlike positions. */
void glNormal3b(GLbyte nx, GLbyte ny, GLbyte nz) {
    glNormal3f(gl_b_to_f(nx), gl_b_to_f(ny), gl_b_to_f(nz));
}
void glNormal3s(GLshort nx, GLshort ny, GLshort nz) {
    glNormal3f(gl_s_to_f(nx), gl_s_to_f(ny), gl_s_to_f(nz));
}
void glNormal3i(GLint nx, GLint ny, GLint nz) {
    glNormal3f(gl_i_to_f(nx), gl_i_to_f(ny), gl_i_to_f(nz));
}
void glNormal3bv(const GLbyte *v) {
    if (v)
        glNormal3b(v[0], v[1], v[2]);
}
void glNormal3sv(const GLshort *v) {
    if (v)
        glNormal3s(v[0], v[1], v[2]);
}
void glNormal3iv(const GLint *v) {
    if (v)
        glNormal3i(v[0], v[1], v[2]);
}

/* Texture coordinates: the value, unconverted. Unspecified components take the
 * specification's defaults - t and r zero, q one - not the previous call's values. */
void glTexCoord1d(GLdouble s) {
    glTexCoord4f((GLfloat)s, 0.0f, 0.0f, 1.0f);
}
void glTexCoord1i(GLint s) {
    glTexCoord4f((GLfloat)s, 0.0f, 0.0f, 1.0f);
}
void glTexCoord1s(GLshort s) {
    glTexCoord4f((GLfloat)s, 0.0f, 0.0f, 1.0f);
}
void glTexCoord2s(GLshort s, GLshort t) {
    glTexCoord4f((GLfloat)s, (GLfloat)t, 0.0f, 1.0f);
}
void glTexCoord3d(GLdouble s, GLdouble t, GLdouble r) {
    glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, 1.0f);
}
void glTexCoord3i(GLint s, GLint t, GLint r) {
    glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, 1.0f);
}
void glTexCoord3s(GLshort s, GLshort t, GLshort r) {
    glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, 1.0f);
}
void glTexCoord4d(GLdouble s, GLdouble t, GLdouble r, GLdouble q) {
    glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, (GLfloat)q);
}
void glTexCoord4i(GLint s, GLint t, GLint r, GLint q) {
    glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, (GLfloat)q);
}
void glTexCoord4s(GLshort s, GLshort t, GLshort r, GLshort q) {
    glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, (GLfloat)q);
}
void glTexCoord1fv(const GLfloat *v) {
    if (v)
        glTexCoord1f(v[0]);
}
void glTexCoord1dv(const GLdouble *v) {
    if (v)
        glTexCoord1d(v[0]);
}
void glTexCoord1iv(const GLint *v) {
    if (v)
        glTexCoord1i(v[0]);
}
void glTexCoord1sv(const GLshort *v) {
    if (v)
        glTexCoord1s(v[0]);
}
void glTexCoord2sv(const GLshort *v) {
    if (v)
        glTexCoord2s(v[0], v[1]);
}
void glTexCoord3fv(const GLfloat *v) {
    if (v)
        glTexCoord3f(v[0], v[1], v[2]);
}
void glTexCoord3dv(const GLdouble *v) {
    if (v)
        glTexCoord3d(v[0], v[1], v[2]);
}
void glTexCoord3iv(const GLint *v) {
    if (v)
        glTexCoord3i(v[0], v[1], v[2]);
}
void glTexCoord3sv(const GLshort *v) {
    if (v)
        glTexCoord3s(v[0], v[1], v[2]);
}
void glTexCoord4fv(const GLfloat *v) {
    if (v)
        glTexCoord4f(v[0], v[1], v[2], v[3]);
}
void glTexCoord4dv(const GLdouble *v) {
    if (v)
        glTexCoord4d(v[0], v[1], v[2], v[3]);
}
void glTexCoord4iv(const GLint *v) {
    if (v)
        glTexCoord4i(v[0], v[1], v[2], v[3]);
}
void glTexCoord4sv(const GLshort *v) {
    if (v)
        glTexCoord4s(v[0], v[1], v[2], v[3]);
}

/* -------------------------------------------------------------------------
 * Multitexture (GL 1.3)
 *
 * A unit at or above GL_MAX_TEXTURE_UNITS is GL_INVALID_ENUM; for glMultiTexCoord the
 * specification leaves it undefined (2.7), and an error beats writing another unit.
 * Every spelling reaches gl_mtc with all four components, GL's defaults - t and r 0,
 * q 1 - filling the ones it leaves out.
 * ------------------------------------------------------------------------- */

/* The unit a GL_TEXTUREn names, or -1 - with GL_INVALID_ENUM recorded - for one there
 * is not. */
static int gl_mt_unit_to(GLenum target, unsigned int limit) {
    if (target >= GL_TEXTURE0 && target < GL_TEXTURE0 + limit) {
        return (int)(target - GL_TEXTURE0);
    }
    gl_context_t *ctx = gl_get_ctx();
    if (ctx)
        gl_record_error(ctx, GL_INVALID_ENUM);
    return -1;
}

/* The fixed-function limit, which bounds texture coordinates (`GL_MAX_TEXTURE_COORDS`):
 * `glMultiTexCoord` and `glClientActiveTexture` use it. `glActiveTexture` selects a
 * binding a sampler may name, so it uses the image-unit limit instead. */
static int gl_mt_unit(GLenum target) {
    return gl_mt_unit_to(target, OOPS_GL_MAX_TEXTURE_UNITS);
}

/* The one implementation: compiled into a list as named, and checked when it runs. */
static void gl_mtc(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
    if (gl_list_recording() &&
        GL_LIST_REC(GL_LIST_OP_MULTI_TEXCOORD, gl_la_e(target), gl_la_f(s), gl_la_f(t),
                    gl_la_f(r), gl_la_f(q))) {
        return;
    }
    const int u = gl_mt_unit(target);
    gl_context_t *ctx = gl_get_ctx();
    if (u < 0 || !ctx)
        return;
    ctx->cur_texcoord[u][0] = s;
    ctx->cur_texcoord[u][1] = t;
    ctx->cur_texcoord[u][2] = r;
    ctx->cur_texcoord[u][3] = q;
}

/* Server state, so compiled into a list; glClientActiveTexture below is client state
 * and is not. */
void glActiveTexture(GLenum texture) {
    if (gl_list_recording() && GL_LIST_REC(GL_LIST_OP_ACTIVE_TEXTURE, gl_la_e(texture)))
        return;
    /* Up to the image units: this selects a binding point, and a fragment shader may
     * sample any of them even though only the first few combine in the fixed-function
     * pipeline. */
    const int u = gl_mt_unit_to(texture, OOPS_GL_MAX_TEXTURE_IMAGE_UNITS);
    gl_context_t *ctx = gl_get_ctx();
    if (u >= 0 && ctx)
        ctx->active_texture = (GLuint)u;
}
void glClientActiveTexture(GLenum texture) {
    const int u = gl_mt_unit(texture);
    gl_context_t *ctx = gl_get_ctx();
    if (u >= 0 && ctx)
        ctx->client_active_texture = (GLuint)u;
}
void glMultiTexCoord1f(GLenum target, GLfloat s) {
    gl_mtc(target, s, 0.0f, 0.0f, 1.0f);
}
void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t) {
    gl_mtc(target, s, t, 0.0f, 1.0f);
}
void glMultiTexCoord3f(GLenum target, GLfloat s, GLfloat t, GLfloat r) {
    gl_mtc(target, s, t, r, 1.0f);
}
void glMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
    gl_mtc(target, s, t, r, q);
}
void glMultiTexCoord1d(GLenum target, GLdouble s) {
    gl_mtc(target, (GLfloat)s, 0.0f, 0.0f, 1.0f);
}
void glMultiTexCoord2d(GLenum target, GLdouble s, GLdouble t) {
    gl_mtc(target, (GLfloat)s, (GLfloat)t, 0.0f, 1.0f);
}
void glMultiTexCoord3d(GLenum target, GLdouble s, GLdouble t, GLdouble r) {
    gl_mtc(target, (GLfloat)s, (GLfloat)t, (GLfloat)r, 1.0f);
}
void glMultiTexCoord4d(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q) {
    gl_mtc(target, (GLfloat)s, (GLfloat)t, (GLfloat)r, (GLfloat)q);
}
void glMultiTexCoord1i(GLenum target, GLint s) {
    gl_mtc(target, (GLfloat)s, 0.0f, 0.0f, 1.0f);
}
void glMultiTexCoord2i(GLenum target, GLint s, GLint t) {
    gl_mtc(target, (GLfloat)s, (GLfloat)t, 0.0f, 1.0f);
}
void glMultiTexCoord3i(GLenum target, GLint s, GLint t, GLint r) {
    gl_mtc(target, (GLfloat)s, (GLfloat)t, (GLfloat)r, 1.0f);
}
void glMultiTexCoord4i(GLenum target, GLint s, GLint t, GLint r, GLint q) {
    gl_mtc(target, (GLfloat)s, (GLfloat)t, (GLfloat)r, (GLfloat)q);
}
void glMultiTexCoord1s(GLenum target, GLshort s) {
    gl_mtc(target, (GLfloat)s, 0.0f, 0.0f, 1.0f);
}
void glMultiTexCoord2s(GLenum target, GLshort s, GLshort t) {
    gl_mtc(target, (GLfloat)s, (GLfloat)t, 0.0f, 1.0f);
}
void glMultiTexCoord3s(GLenum target, GLshort s, GLshort t, GLshort r) {
    gl_mtc(target, (GLfloat)s, (GLfloat)t, (GLfloat)r, 1.0f);
}
void glMultiTexCoord4s(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q) {
    gl_mtc(target, (GLfloat)s, (GLfloat)t, (GLfloat)r, (GLfloat)q);
}
void glMultiTexCoord1fv(GLenum target, const GLfloat *v) {
    if (v)
        glMultiTexCoord1f(target, v[0]);
}
void glMultiTexCoord2fv(GLenum target, const GLfloat *v) {
    if (v)
        glMultiTexCoord2f(target, v[0], v[1]);
}
void glMultiTexCoord3fv(GLenum target, const GLfloat *v) {
    if (v)
        glMultiTexCoord3f(target, v[0], v[1], v[2]);
}
void glMultiTexCoord4fv(GLenum target, const GLfloat *v) {
    if (v)
        glMultiTexCoord4f(target, v[0], v[1], v[2], v[3]);
}
void glMultiTexCoord1dv(GLenum target, const GLdouble *v) {
    if (v)
        glMultiTexCoord1d(target, v[0]);
}
void glMultiTexCoord2dv(GLenum target, const GLdouble *v) {
    if (v)
        glMultiTexCoord2d(target, v[0], v[1]);
}
void glMultiTexCoord3dv(GLenum target, const GLdouble *v) {
    if (v)
        glMultiTexCoord3d(target, v[0], v[1], v[2]);
}
void glMultiTexCoord4dv(GLenum target, const GLdouble *v) {
    if (v)
        glMultiTexCoord4d(target, v[0], v[1], v[2], v[3]);
}
void glMultiTexCoord1iv(GLenum target, const GLint *v) {
    if (v)
        glMultiTexCoord1i(target, v[0]);
}
void glMultiTexCoord2iv(GLenum target, const GLint *v) {
    if (v)
        glMultiTexCoord2i(target, v[0], v[1]);
}
void glMultiTexCoord3iv(GLenum target, const GLint *v) {
    if (v)
        glMultiTexCoord3i(target, v[0], v[1], v[2]);
}
void glMultiTexCoord4iv(GLenum target, const GLint *v) {
    if (v)
        glMultiTexCoord4i(target, v[0], v[1], v[2], v[3]);
}
void glMultiTexCoord1sv(GLenum target, const GLshort *v) {
    if (v)
        glMultiTexCoord1s(target, v[0]);
}
void glMultiTexCoord2sv(GLenum target, const GLshort *v) {
    if (v)
        glMultiTexCoord2s(target, v[0], v[1]);
}
void glMultiTexCoord3sv(GLenum target, const GLshort *v) {
    if (v)
        glMultiTexCoord3s(target, v[0], v[1], v[2]);
}
void glMultiTexCoord4sv(GLenum target, const GLshort *v) {
    if (v)
        glMultiTexCoord4s(target, v[0], v[1], v[2], v[3]);
}

/* The ARB spellings: the same functions under their extension names. */
void glActiveTextureARB(GLenum texture) {
    glActiveTexture(texture);
}
void glClientActiveTextureARB(GLenum texture) {
    glClientActiveTexture(texture);
}
void glMultiTexCoord1fARB(GLenum target, GLfloat s) {
    glMultiTexCoord1f(target, s);
}
void glMultiTexCoord1fvARB(GLenum target, const GLfloat *v) {
    glMultiTexCoord1fv(target, v);
}
void glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t) {
    glMultiTexCoord2f(target, s, t);
}
void glMultiTexCoord2fvARB(GLenum target, const GLfloat *v) {
    glMultiTexCoord2fv(target, v);
}
void glMultiTexCoord3fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r) {
    glMultiTexCoord3f(target, s, t, r);
}
void glMultiTexCoord3fvARB(GLenum target, const GLfloat *v) {
    glMultiTexCoord3fv(target, v);
}
void glMultiTexCoord4fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
    glMultiTexCoord4f(target, s, t, r, q);
}
void glMultiTexCoord4fvARB(GLenum target, const GLfloat *v) {
    glMultiTexCoord4fv(target, v);
}
void glMultiTexCoord1dARB(GLenum target, GLdouble s) {
    glMultiTexCoord1d(target, s);
}
void glMultiTexCoord1dvARB(GLenum target, const GLdouble *v) {
    glMultiTexCoord1dv(target, v);
}
void glMultiTexCoord2dARB(GLenum target, GLdouble s, GLdouble t) {
    glMultiTexCoord2d(target, s, t);
}
void glMultiTexCoord2dvARB(GLenum target, const GLdouble *v) {
    glMultiTexCoord2dv(target, v);
}
void glMultiTexCoord3dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r) {
    glMultiTexCoord3d(target, s, t, r);
}
void glMultiTexCoord3dvARB(GLenum target, const GLdouble *v) {
    glMultiTexCoord3dv(target, v);
}
void glMultiTexCoord4dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r,
                          GLdouble q) {
    glMultiTexCoord4d(target, s, t, r, q);
}
void glMultiTexCoord4dvARB(GLenum target, const GLdouble *v) {
    glMultiTexCoord4dv(target, v);
}
void glMultiTexCoord1iARB(GLenum target, GLint s) {
    glMultiTexCoord1i(target, s);
}
void glMultiTexCoord1ivARB(GLenum target, const GLint *v) {
    glMultiTexCoord1iv(target, v);
}
void glMultiTexCoord2iARB(GLenum target, GLint s, GLint t) {
    glMultiTexCoord2i(target, s, t);
}
void glMultiTexCoord2ivARB(GLenum target, const GLint *v) {
    glMultiTexCoord2iv(target, v);
}
void glMultiTexCoord3iARB(GLenum target, GLint s, GLint t, GLint r) {
    glMultiTexCoord3i(target, s, t, r);
}
void glMultiTexCoord3ivARB(GLenum target, const GLint *v) {
    glMultiTexCoord3iv(target, v);
}
void glMultiTexCoord4iARB(GLenum target, GLint s, GLint t, GLint r, GLint q) {
    glMultiTexCoord4i(target, s, t, r, q);
}
void glMultiTexCoord4ivARB(GLenum target, const GLint *v) {
    glMultiTexCoord4iv(target, v);
}
void glMultiTexCoord1sARB(GLenum target, GLshort s) {
    glMultiTexCoord1s(target, s);
}
void glMultiTexCoord1svARB(GLenum target, const GLshort *v) {
    glMultiTexCoord1sv(target, v);
}
void glMultiTexCoord2sARB(GLenum target, GLshort s, GLshort t) {
    glMultiTexCoord2s(target, s, t);
}
void glMultiTexCoord2svARB(GLenum target, const GLshort *v) {
    glMultiTexCoord2sv(target, v);
}
void glMultiTexCoord3sARB(GLenum target, GLshort s, GLshort t, GLshort r) {
    glMultiTexCoord3s(target, s, t, r);
}
void glMultiTexCoord3svARB(GLenum target, const GLshort *v) {
    glMultiTexCoord3sv(target, v);
}
void glMultiTexCoord4sARB(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q) {
    glMultiTexCoord4s(target, s, t, r, q);
}
void glMultiTexCoord4svARB(GLenum target, const GLshort *v) {
    glMultiTexCoord4sv(target, v);
}

/* -------------------------------------------------------------------------
 * Points and lines, expanded to triangles
 *
 * The geometry engine stalls on a native one- or two-vertex primitive, so a line is
 * drawn as a quad and a point as a square.
 *
 * The width is in pixels, so the expansion happens after projection: each endpoint is
 * taken to normalised device coordinates, offset there by the half-width through the
 * viewport, and carried back to object space through the inverse of the combined
 * matrix, because the triangle pipeline applies that matrix again.
 *
 * A vertex behind the eye is dropped rather than expanded: there is no near-plane
 * clipper for lines, and a dropped line is better than a quad that wraps around the
 * viewer.
 * ------------------------------------------------------------------------- */

/* Projects an object-space position, returning false when it cannot be used. `out_ndc`
 * carries z too, so the expanded corners keep the endpoint's depth. */
static GLboolean gl_project_ndc(const gl_context_t *ctx, const gl_vertex_t *v,
                                float out_ndc[3], float *out_w) {
    const float obj[4] = {v->x, v->y, v->z, v->w};
    float clip[4];
    mat4_transform_vec4(clip, &ctx->mvp, obj);
    if (clip[3] <= 0.0001f)
        return GL_FALSE;
    const float inv_w = 1.0f / clip[3];
    out_ndc[0] = clip[0] * inv_w;
    out_ndc[1] = clip[1] * inv_w;
    out_ndc[2] = clip[2] * inv_w;
    *out_w = clip[3];
    return GL_TRUE;
}

/* Builds an object-space vertex at an NDC position, keeping colour, normal and texture
 * coordinate from `src`. `w` is the endpoint's own clip w, so the corner sits in the
 * plane the endpoint does, and `ndc_z` is its depth. */
static gl_vertex_t gl_vertex_at_ndc(const gl_mat4_t *inv_mvp, const gl_vertex_t *src,
                                    float ndc_x, float ndc_y, float ndc_z, float w) {
    gl_vertex_t out = *src;
    /* Back through the projection: clip = ndc * w, then object = MVP^-1 * clip, because
     * the triangle pipeline applies MVP again. */
    const float clip[4] = {ndc_x * w, ndc_y * w, ndc_z * w, w};
    float obj[4];
    mat4_transform_vec4(obj, inv_mvp, clip);
    out.x = obj[0];
    out.y = obj[1];
    out.z = obj[2];
    out.w = obj[3];
    return out;
}

static void gl_draw_triangle_pv(gl_context_t *ctx, const gl_vertex_t *v0,
                                const gl_vertex_t *v1, const gl_vertex_t *v2,
                                const gl_vertex_t *pv);

/* A point size or line width as it is drawn: the specification's rule for aliased
 * points and lines - rounded to the nearest integer, and 1 where that would be 0 -
 * clamped to the range the queries report. */
static float gl_aliased_size(float s) {
    float r = (float)(int)(s + 0.5f);
    if (r < 1.0f)
        r = 1.0f;
    if (r > (float)OOPS_GL_MAX_POINT_LINE_SIZE)
        r = (float)OOPS_GL_MAX_POINT_LINE_SIZE;
    return r;
}

/* A smooth point size or line width: not rounded to a pixel but to the granularity the
 * queries report, within the same range. */
static float gl_smooth_size(float s) {
    float r = (float)(int)(s / OOPS_GL_SMOOTH_GRANULARITY + 0.5f) *
              OOPS_GL_SMOOTH_GRANULARITY;
    if (r < 1.0f)
        r = 1.0f;
    if (r > (float)OOPS_GL_MAX_POINT_LINE_SIZE)
        r = (float)OOPS_GL_MAX_POINT_LINE_SIZE;
    return r;
}

/* An NDC position in the rasteriser's screen pixels - x right, y down from the top - by
 * the mapping gl_draw_triangle_pv applies to every vertex. */
static void gl_ndc_to_screen(const gl_context_t *ctx, const float ndc[3],
                             float out[2]) {
    const float hw = (float)ctx->vp_w * 0.5f, hh = (float)ctx->vp_h * 0.5f;
    out[0] = ndc[0] * hw + (float)ctx->vp_x + hw;
    out[1] = (float)ctx->height - (ndc[1] * hh + (float)ctx->vp_y + hh);
}

/*
 * Whether this draw smooths, and how.
 *
 * The software rasteriser weighs every fragment by its coverage, so it smooths all
 * three kinds. On the hardware path the CPU widens a point or line into a quad and
 * writes each corner's offset from the centre into a texture-coordinate parameter the
 * draw does not read (unit 0's when untextured, unit 1's when textured); the pixel
 * shader turns the interpolated offset into GL's coverage
 * (`gl_ps_patch_coverage_where`). A smooth polygon uses edge distances instead
 * (`gl_hw_polygon_smooth_setup`).
 *
 * A draw with two texture units has no spare parameter, so it is drawn aliased and
 * logged once.
 */
static GLboolean gl_smoothing(gl_context_t *ctx, GLboolean enabled, GLenum kind) {
    if (!enabled)
        return GL_FALSE;
    if (!ctx->use_hardware)
        return GL_TRUE;
    const GLboolean textured = (GLboolean)(gl_effective_texture_id(ctx) != 0u);
    /* The same condition `unit1_applied` uses at the draw, because it is the same
     * question: does this draw read the fourth parameter for a texture of its own? */
    const GLboolean two_units =
        (GLboolean)(textured && ctx->hw_multitex && gl_hw_base_unit(ctx) == 0u &&
                    gl_unit_texture_id(ctx, 1u) != 0u);
    if (!two_units) {
        /* A polygon leaves `aa_hw_on` alone: its coverage is not a corner offset, so
         * `gl_aa_stamp` has nothing to write. `ctx->aa_edges`, set by the caller around
         * the draw, tells the hardware path which of its edges fade. */
        if (kind != GL_POLYGON) {
            ctx->aa_hw_on = GL_TRUE;
            ctx->aa_hw_tex = textured;
        }
        return GL_TRUE;
    }
    if (!ctx->hw_smooth_logged) {
        oops_log_info(
            "GL",
            "a smooth primitive is aliased on this path when it uses two texture "
            "units: "
            "the coverage needs the interpolant the second unit's coordinate is in");
        ctx->hw_smooth_logged = GL_TRUE;
    }
    return GL_FALSE;
}

/*
 * One corner's offset from the primitive's centre, into the parameter the coverage slot
 * reads.
 *
 * `across` and `along` are in pixels; `radius` is the primitive's half-extent plus a
 * half, the constant GL's coverage subtracts the distance from, added here to save an
 * instruction per fragment. A line passes zero for `along`, so one shader form serves
 * both. Hardware path only: the software rasteriser computes coverage from the
 * fragment's window position.
 */
static void gl_aa_stamp(const gl_context_t *ctx, gl_vertex_t *v, float across,
                        float along, float radius) {
    if (!ctx->aa_hw_on)
        return;
    /* Unit 1's coordinate when the draw is textured, because unit 0's is read. A
     * four-parameter vertex carries `tc[1]`'s x, y and w in the fourth parameter, the
     * layout the coverage slot reads. The draw escalates to four parameters for it (see
     * where `params` is chosen in `gl_hw_tri_shader`). */
    const size_t unit = ctx->aa_hw_tex ? 1u : 0u;
    v->tc[unit][0] = across;
    v->tc[unit][1] = along;
    v->tc[unit][3] = radius;
}

/*
 * A smooth polygon's geometry for the hardware path: the triangle widened outward so
 * the rasteriser raises the pixels its edges partly cover, and the three edge distances
 * the coverage slot reads.
 *
 * Rewrites the three clip positions and fills `att[j]` with vertex j's fourth
 * parameter, `{d0*w, d1*w, d2*w, w}`. Returns false, changing nothing, when the
 * triangle has no area.
 *
 * The distance to edge i is `λ_i * h_i`, so the attribute is one-hot: at vertex j only
 * component j is non-zero, and it is that vertex's height. An edge that does not fade
 * (an internal diagonal of a triangulated polygon) carries 1.0 at all three vertices.
 * Each component is multiplied by w and w rides in the fourth slot, because `v_interp`
 * is perspective-correct and the shader divides to recover the screen-linear value.
 *
 * The widening scales about the incenter: `1 + e/r` for inradius `r` moves each edge
 * out by `e`, here one pixel. Vertices keep their own attributes, as
 * `gl_draw_line_quad` does.
 */
static GLboolean gl_hw_polygon_smooth_setup(const gl_context_t *ctx, unsigned aa_edges,
                                            float c0[4], float c1[4], float c2[4],
                                            float att[3][4]) {
    float *const c[3] = {c0, c1, c2};
    const float vw = (float)(ctx->width ? ctx->width : 1u);
    const float vh = (float)(ctx->height ? ctx->height : 1u);
    /* Window positions, and the w each came from. A vertex at or behind the eye has no
     * window position, so such a triangle keeps the aliased path. */
    float px[3], py[3], pw[3];
    for (int j = 0; j < 3; j++) {
        const float w = c[j][3];
        if (!(w > 1e-6f) && !(w < -1e-6f))
            return GL_FALSE;
        pw[j] = w;
        px[j] = (c[j][0] / w * 0.5f + 0.5f) * vw;
        py[j] = (0.5f - c[j][1] / w * 0.5f) * vh;
    }
    /* Twice the area, and the three side lengths - side `i` being the one opposite
     * vertex `i`, which is the edge the software rasteriser's `w_i` measures against.
     */
    const float a2 =
        (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
    const float area2 = a2 < 0.0f ? -a2 : a2;
    if (!(area2 > 1e-6f))
        return GL_FALSE;
    float len[3];
    len[0] =
        gl_sqrt((px[2] - px[1]) * (px[2] - px[1]) + (py[2] - py[1]) * (py[2] - py[1]));
    len[1] =
        gl_sqrt((px[0] - px[2]) * (px[0] - px[2]) + (py[0] - py[2]) * (py[0] - py[2]));
    len[2] =
        gl_sqrt((px[1] - px[0]) * (px[1] - px[0]) + (py[1] - py[0]) * (py[1] - py[0]));
    const float perim = len[0] + len[1] + len[2];
    if (!(perim > 1e-6f))
        return GL_FALSE;

    /* Vertex j's fourth parameter. Component i is this vertex's height when i is j and
     * the edge fades, 1.0 at every vertex when it does not, and zero otherwise - all
     * times w. */
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            const GLboolean fades = (GLboolean)((aa_edges >> ((i + 2) % 3)) & 1u);
            float d;
            if (!fades) {
                d = 1.0f; /* no fade: one everywhere, so the clamp is one */
            } else {
                d = (i == j) ? (area2 / len[i]) : 0.0f; /* the height, or on the edge */
            }
            att[j][i] = d * pw[j];
        }
        att[j][3] = pw[j];
    }

    /* The incenter, in window coordinates, weighted by the opposite side lengths - and
     * the inradius, twice the area over the perimeter. */
    const float ix = (len[0] * px[0] + len[1] * px[1] + len[2] * px[2]) / perim;
    const float iy = (len[0] * py[0] + len[1] * py[1] + len[2] * py[2]) / perim;
    const float r = area2 / perim;
    if (!(r > 1e-6f))
        return GL_FALSE;
    const float k = 1.0f + 1.0f / r; /* one pixel outward on every edge */

    /* Back to clip space: the same scale in normalised device coordinates, times w. z
     * and w are left alone, so each vertex keeps its depth. */
    const float inx = (ix / vw - 0.5f) * 2.0f;
    const float iny = (0.5f - iy / vh) * 2.0f;
    for (int j = 0; j < 3; j++) {
        const float nx = c[j][0] / pw[j], ny = c[j][1] / pw[j];
        c[j][0] = (inx + k * (nx - inx)) * pw[j];
        c[j][1] = (iny + k * (ny - iny)) * pw[j];
    }
    return GL_TRUE;
}

/* The quad a line occupies between two points already in NDC, `ox`/`oy` its half-width
 * offset in NDC. Each end keeps its own vertex's attributes. */
static void gl_draw_line_quad(gl_context_t *ctx, const gl_mat4_t *inv_mvp,
                              const gl_vertex_t *a, const float na[3], float wa,
                              const gl_vertex_t *b, const float nb[3], float wb,
                              float ox, float oy, const gl_vertex_t *pv) {
    gl_vertex_t a0 = gl_vertex_at_ndc(inv_mvp, a, na[0] + ox, na[1] + oy, na[2], wa);
    gl_vertex_t a1 = gl_vertex_at_ndc(inv_mvp, a, na[0] - ox, na[1] - oy, na[2], wa);
    gl_vertex_t b0 = gl_vertex_at_ndc(inv_mvp, b, nb[0] + ox, nb[1] + oy, nb[2], wb);
    gl_vertex_t b1 = gl_vertex_at_ndc(inv_mvp, b, nb[0] - ox, nb[1] - oy, nb[2], wb);
    /* Each long edge carries the across distance the coverage slot reads, the same at
     * both ends, so it is constant along the line and ramps across it. */
    {
        const float edge = ctx->aa_hw_half, rad = ctx->aa_hw_r;
        gl_aa_stamp(ctx, &a0, edge, 0.0f, rad);
        gl_aa_stamp(ctx, &b0, edge, 0.0f, rad);
        gl_aa_stamp(ctx, &a1, -edge, 0.0f, rad);
        gl_aa_stamp(ctx, &b1, -edge, 0.0f, rad);
    }

    const GLenum saved = ctx->prim_raster;
    ctx->prim_raster = GL_LINE;
    gl_draw_triangle_pv(ctx, &a0, &a1, &b1, pv);
    gl_draw_triangle_pv(ctx, &a0, &b1, &b0, pv);
    ctx->prim_raster = saved;
}

/* A point part-way along a line on screen, `t` from a to b: its NDC position is linear
 * in `t`, its 1/w is, and its attributes are perspective-correct - each weighted by its
 * end's 1/w - so a dash is coloured and textured exactly where the whole line would
 * have been. */
static void gl_line_point_at(const gl_vertex_t *a, const float na[3], float wa,
                             const gl_vertex_t *b, const float nb[3], float wb, float t,
                             gl_vertex_t *out, float n_out[3], float *w_out) {
    const float ia = (1.0f - t) / wa, ib = t / wb;
    const float iw = ia + ib;
    const float ka = ia / iw, kb = ib / iw;
    *out = *a;
    out->r = ka * a->r + kb * b->r;
    out->g = ka * a->g + kb * b->g;
    out->b = ka * a->b + kb * b->b;
    out->a = ka * a->a + kb * b->a;
    out->sr = ka * a->sr + kb * b->sr;
    out->sg = ka * a->sg + kb * b->sg;
    out->sb = ka * a->sb + kb * b->sb;
    out->fogc = ka * a->fogc + kb * b->fogc;
    for (int u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        for (int k = 0; k < 4; k++)
            out->tc[u][k] = ka * a->tc[u][k] + kb * b->tc[u][k];
    }
    out->nx = ka * a->nx + kb * b->nx;
    out->ny = ka * a->ny + kb * b->ny;
    out->nz = ka * a->nz + kb * b->nz;
    for (int i = 0; i < 3; i++)
        n_out[i] = na[i] + t * (nb[i] - na[i]);
    *w_out = 1.0f / iw;
}

/* One line segment as two triangles, marked as a line for the triangle stage (no
 * culling, no fill offset). `pv` is the vertex whose colour a flat-shaded segment
 * takes: the segment's second vertex for a GL_LINES primitive, the polygon's own for an
 * outline.
 *
 * Under GL_LINE_STIPPLE the segment is cut into dashes first. GL counts fragments along
 * the major axis and keeps fragment s when bit (s / factor) mod 16 of the pattern is
 * set, the count carrying on across a strip. Each run of kept fragments is drawn as its
 * own quad, so a wide line's dashes are square-ended blocks across its width. */
static void gl_draw_line_segment(gl_context_t *ctx, const gl_mat4_t *inv_mvp,
                                 const gl_vertex_t *a, const gl_vertex_t *b,
                                 const gl_vertex_t *pv) {
    /* Selection and feedback take the line as GL has it - two endpoints - not the quad
     * below. */
    if (gl_fb_active(ctx)) {
        gl_fb_line(ctx, a, b, pv);
        return;
    }
    float na[3], nb[3], wa, wb;
    if (!gl_project_ndc(ctx, a, na, &wa))
        return;
    if (!gl_project_ndc(ctx, b, nb, &wb))
        return;

    /* Into pixels, so the perpendicular is perpendicular on screen rather than in NDC;
     * the two differ whenever the viewport is not square. */
    const float hx = (float)ctx->vp_w * 0.5f;
    const float hy = (float)ctx->vp_h * 0.5f;
    const float px_dx = (nb[0] - na[0]) * hx;
    const float px_dy = (nb[1] - na[1]) * hy;
    const float len = gl_sqrt(px_dx * px_dx + px_dy * px_dy);
    if (len < 1e-6f)
        return; /* a zero-length segment has no direction to be perpendicular to */
    const float dx = px_dx / len;
    const float dy = px_dy / len;

    /* A smooth line is GL's rectangle of the unrounded width, centred on the segment.
     * Its quad reaches a pixel further on every side, so partly covered pixels are
     * drawn and weighed by their coverage (`aa_*`). */
    const GLboolean smooth = gl_smoothing(ctx, ctx->cap_line_smooth, GL_LINE);
    const float width =
        smooth ? gl_smooth_size(ctx->line_width) : gl_aliased_size(ctx->line_width);
    const float half = width * 0.5f + (smooth ? 1.0f : 0.0f);
    /* Perpendicular in pixels, then back into NDC through the same viewport scale. */
    const float ox = (-dy * half) / hx;
    const float oy = (dx * half) / hy;
    if (smooth) {
        ctx->aa_kind = GL_LINE;
        gl_ndc_to_screen(ctx, na, ctx->aa_a);
        gl_ndc_to_screen(ctx, nb, ctx->aa_b);
        ctx->aa_hw = width * 0.5f;
        ctx->aa_ends = (GLboolean)!ctx->cap_line_stipple;
        ctx->aa_hw_half = half;
        ctx->aa_hw_r = width * 0.5f + 0.5f;
    }

    if (!ctx->cap_line_stipple) {
        if (smooth && !ctx->aa_hw_on) {
            /* A pixel past each end, carrying the end's own attributes; the software
             * rasteriser fades those pixels by how far along the segment they are. The
             * hardware quad stops at the ends: its coverage has only the across
             * distance, and GL does not require faded ends. */
            float ea[3] = {na[0] - dx / hx, na[1] - dy / hy, na[2]};
            float eb[3] = {nb[0] + dx / hx, nb[1] + dy / hy, nb[2]};
            gl_draw_line_quad(ctx, inv_mvp, a, ea, wa, b, eb, wb, ox, oy, pv);
            ctx->aa_kind = 0u;
            return;
        }
        gl_draw_line_quad(ctx, inv_mvp, a, na, wa, b, nb, wb, ox, oy, pv);
        ctx->aa_kind = 0u;
        ctx->aa_hw_on = GL_FALSE;
        return;
    }

    if (ctx->fb_line_reset) {
        ctx->line_stipple_counter = 0;
        ctx->fb_line_reset = GL_FALSE;
    }
    const float ax = px_dx < 0.0f ? -px_dx : px_dx;
    const float ay = px_dy < 0.0f ? -px_dy : px_dy;
    int n = (int)((ax > ay ? ax : ay) + 0.5f);
    if (n < 1)
        n = 1;
    const GLint factor = ctx->line_stipple_factor;
    const GLushort pattern = ctx->line_stipple_pattern;
    GLint s = ctx->line_stipple_counter;
    int i = 0;
    while (i < n) {
        const GLboolean on = (GLboolean)((pattern >> ((s / factor) & 15)) & 1u);
        int j = i;
        while (j < n &&
               (GLboolean)((pattern >> (((s + (j - i)) / factor) & 15)) & 1u) == on)
            j++;
        if (on) {
            gl_vertex_t da, db;
            float nda[3], ndb[3], wda, wdb;
            gl_line_point_at(a, na, wa, b, nb, wb, (float)i / (float)n, &da, nda, &wda);
            gl_line_point_at(a, na, wa, b, nb, wb, (float)j / (float)n, &db, ndb, &wdb);
            gl_draw_line_quad(ctx, inv_mvp, &da, nda, wda, &db, ndb, wdb, ox, oy, pv);
        }
        s += j - i;
        i = j;
    }
    /* Kept small: only its value modulo 16 * factor matters. */
    ctx->line_stipple_counter = s % (16 * factor);
    ctx->aa_kind = 0u;
    ctx->aa_hw_on = GL_FALSE;
}

/* A point's derived size (GL 1.4, 3.3): glPointSize's, divided by sqrt(a + b d + c
 * d^2) for GL_POINT_DISTANCE_ATTENUATION's (a, b, c) and the point's eye distance d,
 * then clamped to [GL_POINT_SIZE_MIN, GL_POINT_SIZE_MAX]. The clamp applies
 * unattenuated too, as Mesa's does (state_tracker/st_atom_rasterizer.c:233-237). d is
 * the specification's eye distance; Mesa takes |z_eye|
 * (main/ffvertex_prog.c:1234-1235), which differs only off the view axis. */
static float gl_point_derived_size(const gl_context_t *ctx, const gl_vertex_t *p) {
    float size = ctx->point_size;
    const float *k = ctx->point_atten;
    if (k[0] != 1.0f || k[1] != 0.0f || k[2] != 0.0f) {
        const float obj[4] = {p->x, p->y, p->z, p->w};
        float eye[4];
        mat4_transform_vec4(eye, &ctx->modelview_stack[ctx->modelview_depth], obj);
        const float d = gl_sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
        const float den = k[0] + k[1] * d + k[2] * d * d;
        /* A denominator of zero or less has no square root; the size stays as it was.
         */
        if (den > 0.0f)
            size /= gl_sqrt(den);
    }
    if (size < ctx->point_size_min)
        size = ctx->point_size_min;
    if (size > ctx->point_size_max)
        size = ctx->point_size_max;
    return size;
}

/* One point as a screen-aligned square of its derived size in pixels, marked as a
 * point. */
static void gl_draw_point_square(gl_context_t *ctx, const gl_mat4_t *inv_mvp,
                                 const gl_vertex_t *p, const gl_vertex_t *pv) {
    if (gl_fb_active(ctx)) {
        gl_fb_point(ctx, p);
        return;
    }
    float np[3], w;
    if (!gl_project_ndc(ctx, p, np, &w))
        return;

    /* A smooth point is GL's disc of the unrounded size: its square reaches a pixel
     * further, and each pixel is weighed by how much of it the disc covers. */
    const GLboolean smooth = gl_smoothing(ctx, ctx->cap_point_smooth, GL_POINT);
    const float derived = gl_point_derived_size(ctx, p);
    const float size = smooth ? gl_smooth_size(derived) : gl_aliased_size(derived);
    const float half = size * 0.5f + (smooth ? 1.0f : 0.0f);
    const float ox = half / ((float)ctx->vp_w * 0.5f);
    const float oy = half / ((float)ctx->vp_h * 0.5f);
    if (smooth) {
        ctx->aa_kind = GL_POINT;
        gl_ndc_to_screen(ctx, np, ctx->aa_c);
        ctx->aa_r = size * 0.5f;
        ctx->aa_hw_half = half;
        ctx->aa_hw_r = size * 0.5f + 0.5f;
    }

    gl_vertex_t c0 = gl_vertex_at_ndc(inv_mvp, p, np[0] - ox, np[1] - oy, np[2], w);
    gl_vertex_t c1 = gl_vertex_at_ndc(inv_mvp, p, np[0] + ox, np[1] - oy, np[2], w);
    gl_vertex_t c2 = gl_vertex_at_ndc(inv_mvp, p, np[0] + ox, np[1] + oy, np[2], w);
    gl_vertex_t c3 = gl_vertex_at_ndc(inv_mvp, p, np[0] - ox, np[1] + oy, np[2], w);
    /* The four corners' offsets from the centre, for the hardware coverage slot. It
     * squares both, so the sign of y does not matter. */
    gl_aa_stamp(ctx, &c0, -half, -half, ctx->aa_hw_r);
    gl_aa_stamp(ctx, &c1, half, -half, ctx->aa_hw_r);
    gl_aa_stamp(ctx, &c2, half, half, ctx->aa_hw_r);
    gl_aa_stamp(ctx, &c3, -half, half, ctx->aa_hw_r);

    /* The point sprite's coordinates (ARB_point_sprite, GL 2.0). With GL_POINT_SPRITE
     * enabled, a unit whose GL_COORD_REPLACE is set takes s and t across the square
     * instead of from the vertex. c0 is lower-left, c1 lower-right, c2 upper-right, c3
     * upper-left (NDC, +y up); t runs downward from the upper corners by default,
     * because GL_POINT_SPRITE_COORD_ORIGIN is GL_UPPER_LEFT. r and q stay 0 and 1.
     *
     * A GLSL program reading `gl_PointCoord` gets unit 0's coordinate generated too,
     * without GL_POINT_SPRITE: `gl_PointCoord` is texture coordinate 0's interpolant
     * here, as `SPI_PS_INPUT_CNTL.PT_SPRITE_TEX` substitutes it on the part. The link
     * refuses a shader reading both `gl_PointCoord` and `gl_TexCoord`. */
    const gl_program_object_t *const pc_prog = gl_active_program(ctx);
    const GLboolean want_point_coord =
        (GLboolean)(pc_prog != (const gl_program_object_t *)0 &&
                    pc_prog->hw_reads_point_coord);
    if (ctx->cap_point_sprite || want_point_coord) {
        const GLboolean upper = (GLboolean)(ctx->point_sprite_origin != GL_LOWER_LEFT);
        const float t_lo = upper ? 1.0f : 0.0f; /* the two lower corners */
        const float t_hi = upper ? 0.0f : 1.0f; /* the two upper corners */
        gl_vertex_t *corner[4] = {&c0, &c1, &c2, &c3};
        const float s_of[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        const float t_of[4] = {t_lo, t_lo, t_hi, t_hi};
        for (GLuint u = 0u; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
            const GLboolean replace =
                (GLboolean)(ctx->cap_point_sprite && ctx->tex_unit[u].coord_replace);
            if (!replace && !(want_point_coord && u == 0u))
                continue;
            for (int k = 0; k < 4; k++) {
                corner[k]->tc[u][0] = s_of[k];
                corner[k]->tc[u][1] = t_of[k];
                corner[k]->tc[u][2] = 0.0f;
                corner[k]->tc[u][3] = 1.0f;
            }
        }
    }

    const GLenum saved = ctx->prim_raster;
    ctx->prim_raster = GL_POINT;
    gl_draw_triangle_pv(ctx, &c0, &c1, &c2, pv);
    gl_draw_triangle_pv(ctx, &c0, &c2, &c3, pv);
    ctx->prim_raster = saved;
    ctx->aa_kind = 0u;
    ctx->aa_hw_on = GL_FALSE;
}

/* -------------------------------------------------------------------------
 * Primitive assembly, once
 *
 * glEnd, glDrawArrays and glDrawElements share this, each supplying only how to fetch
 * vertex i.
 *
 * What a triangle of a triangulated primitive carries besides its vertices:
 *
 * - Which of its edges are the primitive's own. A quad's and a polygon's internal
 * diagonals are not edges: glPolygonMode(GL_LINE) draws a quad's four sides. Bit 0 is
 * v0->v1, bit 1 v1->v2, bit 2 v2->v0.
 * - Whether edge flags apply: only to separate triangles, quads and polygons; a strip's
 * or fan's edges are all boundary edges, as the specification says.
 * - The provoking vertex, whose colour a GL_FLAT primitive takes, from the
 * specification's table (a quad's fourth vertex, a polygon's first).
 * ------------------------------------------------------------------------- */

typedef void (*gl_vertex_fetch_fn)(gl_context_t *ctx, const void *src, int i,
                                   gl_vertex_t *out);

/* One triangle of a polygon primitive, drawn the way glPolygonMode says for the face it
 * turns out to be. With both faces GL_FILL it is a plain triangle. Otherwise the face
 * and culling are decided here (culling belongs to the polygon, not to the lines its
 * outline becomes), and the boundary edges or their starting vertices are drawn through
 * the GL_LINES and GL_POINTS expansion. */
static void gl_draw_polygon_tri(gl_context_t *ctx, const gl_vertex_t *v0,
                                const gl_vertex_t *v1, const gl_vertex_t *v2,
                                unsigned edges, GLboolean use_flags,
                                const gl_vertex_t *pv) {
    /* GL_SELECT and GL_FEEDBACK report the polygon instead of drawing it, and decide
     * its face, culling and mode themselves - on the clipped polygon. */
    if (gl_fb_active(ctx)) {
        gl_fb_polygon_tri(ctx, v0, v1, v2, edges, use_flags, pv);
        return;
    }
    /* A smooth polygon fades across its own edges only, not the internal diagonals. */
    const unsigned aa_edges =
        gl_smoothing(ctx, ctx->cap_polygon_smooth, GL_POLYGON) ? (edges & 7u) : 0u;
    if (ctx->polygon_mode[0] == GL_FILL && ctx->polygon_mode[1] == GL_FILL) {
        ctx->aa_edges = aa_edges;
        gl_draw_triangle_pv(ctx, v0, v1, v2, pv);
        ctx->aa_edges = 0u;
        return;
    }
    gl_update_mvp(ctx);
    const gl_vertex_t *vtx[3] = {v0, v1, v2};

    /* The facing, from the winding in normalised device coordinates - the same sign the
     * rasteriser's screen-space area gives, before its y flip. A vertex behind the eye
     * has no projection to wind with, and the triangle is then taken as front-facing.
     */
    GLboolean front = GL_TRUE;
    float n0[3], n1[3], n2[3], w;
    if (gl_project_ndc(ctx, v0, n0, &w) && gl_project_ndc(ctx, v1, n1, &w) &&
        gl_project_ndc(ctx, v2, n2, &w)) {
        const float area =
            (n1[0] - n0[0]) * (n2[1] - n0[1]) - (n1[1] - n0[1]) * (n2[0] - n0[0]);
        const GLboolean ccw = (GLboolean)(area > 0.0f);
        front = (ctx->front_face == GL_CW) ? (GLboolean)!ccw : ccw;
    }
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
        ctx->aa_edges = aa_edges;
        gl_draw_triangle_pv(ctx, v0, v1, v2, pv);
        ctx->aa_edges = 0u;
        return;
    }
    gl_mat4_t inv_mvp;
    if (!mat4_invert(&inv_mvp, &ctx->mvp))
        return;

    const GLboolean saved = ctx->prim_from_polygon;
    const GLboolean saved_back = ctx->prim_polygon_back;
    ctx->prim_from_polygon = GL_TRUE;
    ctx->prim_polygon_back =
        (GLboolean)!front; /* the side two-sided lighting lights it with */
    for (int e = 0; e < 3; e++) {
        if (!(edges & (1u << e)))
            continue;
        if (use_flags && !vtx[e]->edge)
            continue; /* a vertex's flag is the edge starting at it */
        if (mode == GL_LINE) {
            gl_draw_line_segment(ctx, &inv_mvp, vtx[e], vtx[(e + 1) % 3], pv);
        } else {
            /* Each vertex that starts a boundary edge, which draws every corner of a
             * quad or a polygon exactly once however it was triangulated. */
            gl_draw_point_square(ctx, &inv_mvp, vtx[e], pv);
        }
    }
    ctx->prim_from_polygon = saved;
    ctx->prim_polygon_back = saved_back;
}

/* `fb_line_reset` is set wherever GL resets the line stipple - at each independent
 * line, the start of a strip or loop, and the start of each polygon whose outline is
 * drawn - so feedback can report the next line as GL_LINE_RESET_TOKEN. Drawing never
 * reads it. */
static void gl_assemble(gl_context_t *ctx, GLenum mode, int n, gl_vertex_fetch_fn fetch,
                        const void *src) {
    gl_vertex_t a, b, c, d;
    switch (mode) {
    case GL_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3) {
            fetch(ctx, src, i, &a);
            fetch(ctx, src, i + 1, &b);
            fetch(ctx, src, i + 2, &c);
            ctx->fb_line_reset = GL_TRUE;
            gl_draw_polygon_tri(ctx, &a, &b, &c, 7u, GL_TRUE, &c);
        }
        break;
    case GL_QUADS:
        /* (0,1,2) and (0,2,3): the sides are 0-1, 1-2 in the first and 2-3, 3-0 in the
         * second, and the quad's colour is its fourth vertex's. */
        for (int i = 0; i + 3 < n; i += 4) {
            fetch(ctx, src, i, &a);
            fetch(ctx, src, i + 1, &b);
            fetch(ctx, src, i + 2, &c);
            fetch(ctx, src, i + 3, &d);
            ctx->fb_line_reset = GL_TRUE;
            gl_draw_polygon_tri(ctx, &a, &b, &c, 3u, GL_TRUE, &d);
            gl_draw_polygon_tri(ctx, &a, &c, &d, 6u, GL_TRUE, &d);
        }
        break;
    case GL_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < n; i++) {
            fetch(ctx, src, i, &a);
            fetch(ctx, src, i + 1, &b);
            fetch(ctx, src, i + 2, &c);
            ctx->fb_line_reset = GL_TRUE;
            if (i & 1) {
                gl_draw_polygon_tri(ctx, &b, &a, &c, 7u, GL_FALSE, &c);
            } else {
                gl_draw_polygon_tri(ctx, &a, &b, &c, 7u, GL_FALSE, &c);
            }
        }
        break;
    case GL_TRIANGLE_FAN:
        if (n < 3)
            break;
        fetch(ctx, src, 0, &a);
        for (int i = 1; i + 1 < n; i++) {
            fetch(ctx, src, i, &b);
            fetch(ctx, src, i + 1, &c);
            ctx->fb_line_reset = GL_TRUE;
            gl_draw_polygon_tri(ctx, &a, &b, &c, 7u, GL_FALSE, &c);
        }
        break;
    /* A polygon is a fan from vertex zero; GL requires it convex and planar. Its sides
     * are the fan's outer edges only:
     * 0-1 in the first triangle, i-(i+1) in every one, and (n-1)-0 in the last. Its
     * colour is its first vertex's. */
    case GL_POLYGON:
        if (n < 3)
            break;
        fetch(ctx, src, 0, &a);
        ctx->fb_line_reset = GL_TRUE;
        for (int i = 1; i + 1 < n; i++) {
            fetch(ctx, src, i, &b);
            fetch(ctx, src, i + 1, &c);
            const unsigned edges = 2u | (i == 1 ? 1u : 0u) | (i + 1 == n - 1 ? 4u : 0u);
            gl_draw_polygon_tri(ctx, &a, &b, &c, edges, GL_TRUE, &a);
        }
        break;
    /* A quad strip's quads share an edge, and the winding alternates the way a triangle
     * strip's does - taking them in declaration order would flip every other one. Quad
     * (i, i+1, i+3, i+2): sides i-(i+1) and (i+1)-(i+3) in the first triangle,
     * (i+3)-(i+2) and (i+2)-i in the second; its colour is vertex i+3's. */
    case GL_QUAD_STRIP:
        for (int i = 0; i + 3 < n; i += 2) {
            fetch(ctx, src, i, &a);
            fetch(ctx, src, i + 1, &b);
            fetch(ctx, src, i + 2, &c);
            fetch(ctx, src, i + 3, &d);
            ctx->fb_line_reset = GL_TRUE;
            gl_draw_polygon_tri(ctx, &a, &b, &d, 3u, GL_FALSE, &d);
            gl_draw_polygon_tri(ctx, &a, &d, &c, 6u, GL_FALSE, &d);
        }
        break;
    case GL_POINTS:
    case GL_LINES:
    case GL_LINE_STRIP:
    case GL_LINE_LOOP: {
        /* The inverse is computed once per primitive. A matrix that cannot be inverted
         * collapses the scene, and draws nothing. Selection and feedback widen nothing
         * and need no inverse. */
        gl_update_mvp(ctx);
        gl_mat4_t inv_mvp;
        if (gl_fb_active(ctx)) {
            mat4_identity(&inv_mvp);
        } else if (!mat4_invert(&inv_mvp, &ctx->mvp)) {
            break;
        }
        if (mode == GL_POINTS) {
            for (int i = 0; i < n; i++) {
                fetch(ctx, src, i, &a);
                gl_draw_point_square(ctx, &inv_mvp, &a, &a);
            }
        } else if (mode == GL_LINES) {
            for (int i = 0; i + 1 < n; i += 2) {
                fetch(ctx, src, i, &a);
                fetch(ctx, src, i + 1, &b);
                ctx->fb_line_reset = GL_TRUE;
                gl_draw_line_segment(ctx, &inv_mvp, &a, &b, &b);
            }
        } else {
            ctx->fb_line_reset = GL_TRUE;
            for (int i = 0; i + 1 < n; i++) {
                fetch(ctx, src, i, &a);
                fetch(ctx, src, i + 1, &b);
                gl_draw_line_segment(ctx, &inv_mvp, &a, &b, &b);
            }
            /* The loop closes, and the closing segment's provoking vertex is the first.
             */
            if (mode == GL_LINE_LOOP && n > 2) {
                fetch(ctx, src, n - 1, &a);
                fetch(ctx, src, 0, &b);
                gl_draw_line_segment(ctx, &inv_mvp, &a, &b, &b);
            }
        }
        break;
    }
    default:
        break;
    }
}

static void gl_fetch_immediate(gl_context_t *ctx, const void *src, int i,
                               gl_vertex_t *out) {
    (void)ctx;
    *out = ((const gl_vertex_t *)src)[i];
}

void glEnd(void) {
    if (gl_list_recording() && GL_LIST_REC0(GL_LIST_OP_END))
        return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !ctx->imm_active)
        return;
    ctx->imm_active = GL_FALSE;

    gl_assemble(ctx, ctx->imm_mode, ctx->imm_count, gl_fetch_immediate, ctx->imm_verts);
    ctx->imm_count = 0;
}

/* -------------------------------------------------------------------------
 * glRect*
 *
 * A screen-aligned rectangle in the z=0 plane, drawn with the current colour, normal
 * and texture coordinate. The specification defines it as exactly this sequence:
 *
 *     glBegin(GL_POLYGON);
 *     glVertex2(x1, y1); glVertex2(x2, y1); glVertex2(x2, y2); glVertex2(x1, y2);
 *     glEnd();
 *
 * GL_QUADS is used for GL_POLYGON: for four vertices both give the triangles (0,1,2)
 * and (0,2,3), and the vertex order keeps the specification's winding.
 *
 * Written on glBegin/glVertex/glEnd so a glRectf inside glNewList records the four
 * vertices, which is what the specification says a compiled glRect is.
 * ------------------------------------------------------------------------- */

void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) {
    glBegin(GL_QUADS);
    glVertex2f(x1, y1);
    glVertex2f(x2, y1);
    glVertex2f(x2, y2);
    glVertex2f(x1, y2);
    glEnd();
}

void glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2) {
    glRectf((GLfloat)x1, (GLfloat)y1, (GLfloat)x2, (GLfloat)y2);
}

void glRecti(GLint x1, GLint y1, GLint x2, GLint y2) {
    glRectf((GLfloat)x1, (GLfloat)y1, (GLfloat)x2, (GLfloat)y2);
}

void glRects(GLshort x1, GLshort y1, GLshort x2, GLshort y2) {
    glRectf((GLfloat)x1, (GLfloat)y1, (GLfloat)x2, (GLfloat)y2);
}

/* The vector forms take two corners in two arrays, not four scalars in one. */
void glRectfv(const GLfloat *v1, const GLfloat *v2) {
    if (v1 && v2)
        glRectf(v1[0], v1[1], v2[0], v2[1]);
}

void glRectdv(const GLdouble *v1, const GLdouble *v2) {
    if (v1 && v2)
        glRectd(v1[0], v1[1], v2[0], v2[1]);
}

void glRectiv(const GLint *v1, const GLint *v2) {
    if (v1 && v2)
        glRecti(v1[0], v1[1], v2[0], v2[1]);
}

void glRectsv(const GLshort *v1, const GLshort *v2) {
    if (v1 && v2)
        glRects(v1[0], v1[1], v2[0], v2[1]);
}

/* -------------------------------------------------------------------------
 * Triangle Pipeline & Rasterizer
 * ------------------------------------------------------------------------- */

/*
 * An occlusion query on the GPU (GL 1.5's GL_SAMPLES_PASSED).
 *
 * The depth block counts z-passing samples per render backend while DB_COUNT_CONTROL's
 * ZPASS_ENABLE is set, as the recipe's `0x11000100` has it (bits [8,11] = 1,
 * `gfx103.json:12552-12565`). `ZPASS_DONE` is an `EVENT_WRITE` that dumps every
 * backend's counter to memory: event type 21 with `EVENT_INDEX` 1 (`0x00000115`), to a
 * 16-byte-aligned destination. Two of them, sixteen bytes apart per backend, bracket
 * the query; the answer is the sum of the differences.
 *
 * While a query runs, `PERFECT_ZPASS_COUNTS` (bit 1) and
 * `DISABLE_CONSERVATIVE_ZPASS_COUNTS` (bit 2) are added, making `0x11000106`, so the
 * count is exact rather than conservative. They are not in the depth block itself, so a
 * frame with no query keeps the stream the gl-cube oracle record pins
 * (`docs/hardware/agc-gl-cube-oracle-fw1240.md`).
 *
 * ZPASS_ENABLE with no depth target bound stalls the depth block, so the arming waits
 * for `hw_z_bound`; a query whose draws never test depth does not count on this path.
 */
static void
gl_hw_begin_frame(gl_context_t *ctx); /* below; the query's ends open a frame */

static void gl_hw_emit_zpass_done(uint32_t **dw_ptr, uint64_t addr) {
    uint32_t *dw = *dw_ptr;
    *dw++ = 0xc0024600u; /* PKT3 EVENT_WRITE (0x46), three payload dwords */
    *dw++ = 0x00000115u; /* ZPASS_DONE: event type 21, EVENT_INDEX 1 */
    *dw++ = (uint32_t)addr;
    *dw++ = (uint32_t)(addr >> 32);
    *dw_ptr = dw;
}

/* The slots' base in GPU memory, and the same address as the CPU sees it - the payload
 * is one mapping, so they are the same number. */
static uint64_t gl_hw_zpass_base(const gl_context_t *ctx) {
    return (uint64_t)(uintptr_t)ctx->gpu_payload + OOPS_GL_ZPASS_OFFSET;
}

void gl_hw_query_begin(gl_context_t *ctx) {
    if (!ctx || !ctx->use_hardware || !ctx->gpu_payload)
        return;
    /* Zeroed rather than left alone: a slot a backend never writes must read as no
     * work, and the difference of two zeroes is zero whichever end is missing. */
    memset((char *)ctx->gpu_payload + OOPS_GL_ZPASS_OFFSET, 0,
           OOPS_GL_ZPASS_BACKENDS * OOPS_GL_ZPASS_STRIDE);
    ctx->hw_query_counting = GL_FALSE;
    ctx->hw_query_reg = GL_FALSE;
}

uint64_t gl_hw_query_end(gl_context_t *ctx, GLboolean *counted) {
    if (counted)
        *counted = GL_FALSE;
    if (!ctx || !ctx->use_hardware || !ctx->gpu_payload || !ctx->hw_query_counting)
        return 0u;
    if (!ctx->hw_frame_active)
        gl_hw_begin_frame(ctx);
    if (ctx->dcb_words + 8u + OOPS_GL_DCB_TRAILER_DW >= ctx->dcb_capacity_dw) {
        gl_hw_flush(ctx);
        gl_hw_begin_frame(ctx);
        /* The counters keep running across a submission - they are depth-block state,
         * not command-stream state - so the begin snapshot taken earlier is still the
         * right one. */
    }
    uint32_t *dw = ctx->dcb_mem + ctx->dcb_words;
    gl_hw_emit_zpass_done(&dw, gl_hw_zpass_base(ctx) + 8u);
    /* The recipe's DB_COUNT_CONTROL back, only when this frame raised it: writing it
     * with no depth surface bound stalls the depth block. */
    if (ctx->hw_query_reg) {
        *dw++ = 0xc0016900u;
        *dw++ = 0x001u;
        *dw++ = 0x11000100u;
    }
    ctx->dcb_words = (uint32_t)(dw - ctx->dcb_mem);
    /* The results are read by the CPU, so the frame has to have run. */
    gl_hw_flush(ctx);

    const volatile uint32_t *slots =
        (const volatile uint32_t *)((const char *)ctx->gpu_payload +
                                    OOPS_GL_ZPASS_OFFSET);
    uint64_t sum = 0u;
    for (unsigned i = 0; i < OOPS_GL_ZPASS_BACKENDS; i++) {
        const unsigned w = i * (OOPS_GL_ZPASS_STRIDE / 4u);
        /* Bit 63 is the hardware's valid marker, not part of the count. */
        const uint64_t begin =
            ((uint64_t)(slots[w + 1] & 0x7fffffffu) << 32) | slots[w];
        const uint64_t end =
            ((uint64_t)(slots[w + 3] & 0x7fffffffu) << 32) | slots[w + 2];
        if (end > begin)
            sum += end - begin;
    }
    ctx->hw_query_counting = GL_FALSE;
    ctx->hw_query_reg = GL_FALSE;
    if (counted)
        *counted = GL_TRUE;
    return sum;
}

/* The depth block: surface, extent and bases. Emitted the first time a frame draws with
 * the depth test on, so a frame that never tests depth carries exactly the measured
 * no-depth recipe. The register list and order are the ones oops_agc_draw_primitive
 * carries (AgcCompositor.elf). */
static void gl_hw_emit_depth_block(gl_context_t *ctx, uint32_t **dw_ptr) {
    uint32_t *dw = *dw_ptr;
    uint64_t depth_gpu = (uint64_t)(uintptr_t)ctx->depth_buffer;
    uint32_t w = ctx->width ? ctx->width : 1920;
    uint32_t h = ctx->height ? ctx->height : 1080;
    uint32_t z_info = OOPS_AGC_Z_32_FLOAT |
                      0x80000180u; /* Z_32_FLOAT | SW_MODE=24 | ZRANGE_PRECISION */

    static const struct {
        uint32_t reg;
        uint32_t val;
    } db_regs[] = {
        {0x000u, 0x00000000u}, /* DB_RENDER_CONTROL */
        {0x001u, 0x11000100u}, /* DB_COUNT_CONTROL */
        {0x002u, 0x00000000u}, /* DB_DEPTH_VIEW */
        {0x003u, 0x00000000u}, /* DB_RENDER_OVERRIDE */
        {0x004u, 0x00000000u}, /* DB_RENDER_OVERRIDE2 */
        {0x005u, 0x00000000u}, /* DB_HTILE_DATA_BASE */
        {0x007u, 0},           /* DB_DEPTH_SIZE_XY (patched) */
        {0x008u, 0x00000000u}, /* DB_DEPTH_BOUNDS_MIN */
        {0x009u, 0x00000000u}, /* DB_DEPTH_BOUNDS_MAX */
        {0x00au, 0x00000000u}, /* DB_STENCIL_CLEAR */
        {0x00bu, 0x00000000u}, /* DB_DEPTH_CLEAR */
        {0x010u, 0},           /* DB_Z_INFO (patched) */
        {0x011u, 0x20000180u}, /* DB_STENCIL_INFO */
        {0x012u, 0},           /* DB_Z_READ_BASE (patched) */
        {0x013u, 0x00000000u}, /* DB_STENCIL_READ_BASE */
        {0x014u, 0},           /* DB_Z_WRITE_BASE (patched) */
        {0x015u, 0x00000000u}, /* DB_STENCIL_WRITE_BASE */
        {0x01au, 0},           /* DB_Z_READ_BASE_HI (patched) */
        {0x01bu, 0x00000000u}, /* DB_STENCIL_READ_BASE_HI */
        {0x01cu, 0},           /* DB_Z_WRITE_BASE_HI (patched) */
        {0x01du, 0x00000000u}, /* DB_STENCIL_WRITE_BASE_HI */
        {0x01eu, 0x00000000u}, /* DB_HTILE_DATA_BASE_HI */
        {0x01fu, 0x00000000u}, /* DB_RMI_L2_CACHE_CONTROL */
        {0x2afu, 0x00040000u}, /* DB_HTILE_SURFACE */
    };

    for (size_t i = 0; i < sizeof(db_regs) / sizeof(db_regs[0]); i++) {
        uint32_t reg = db_regs[i].reg;
        uint32_t val = db_regs[i].val;
        if (reg == 0x007u) {
            val = OOPS_AGC_DB_DEPTH_SIZE_XY(w, h);
        } else if (reg == 0x010u) {
            val = z_info;
        } else if (reg == 0x012u || reg == 0x014u) {
            val = (uint32_t)(depth_gpu >> 8);
        } else if (reg == 0x01au || reg == 0x01cu) {
            val = (uint32_t)(depth_gpu >> 40);
        }
        *dw++ = 0xc0016900u;
        *dw++ = reg;
        *dw++ = val;
    }
    *dw_ptr = dw;
}

/* The stencil surface made live: DB_STENCIL_INFO (0x011) with FORMAT STENCIL_8, the
 * depth surface's SW_MODE 24 (64KB_Z_X) and TILE_STENCIL_DISABLE - the recipe's
 * 0x20000180 with the format field set (gfx103.json: FORMAT bit 0, SW_MODE bits 4-8,
 * TILE_STENCIL_DISABLE bit 29, as there is no HTILE) - and the four base registers the
 * depth block writes as zero. The extent is DB_DEPTH_SIZE_XY's, shared with depth, so
 * the depth block is always emitted first. */
static void gl_hw_emit_stencil_bind(gl_context_t *ctx, uint32_t **dw_ptr) {
    uint32_t *dw = *dw_ptr;
    const uint64_t s = (uint64_t)(uintptr_t)ctx->stencil_buffer;
    const struct {
        uint32_t reg, val;
    } regs[] = {
        {0x011u, 0x20000181u},         /* DB_STENCIL_INFO */
        {0x013u, (uint32_t)(s >> 8)},  /* DB_STENCIL_READ_BASE */
        {0x015u, (uint32_t)(s >> 8)},  /* DB_STENCIL_WRITE_BASE */
        {0x01bu, (uint32_t)(s >> 40)}, /* DB_STENCIL_READ_BASE_HI */
        {0x01du, (uint32_t)(s >> 40)}, /* DB_STENCIL_WRITE_BASE_HI */
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        *dw++ = 0xc0016900u;
        *dw++ = regs[i].reg;
        *dw++ = regs[i].val;
    }
    *dw_ptr = dw;
}

/* -------------------------------------------------------------------------
 * The block a compiled GL 2.0 pixel shader is handed
 * ------------------------------------------------------------------------- */

/* The texture the program's sampler for descriptor set `s` currently names.
 *
 * The sampler's value is a texture unit number (`glUniform1i`), defaulting to unit 0,
 * and the unit has a texture bound. NULL when the unit has nothing complete bound; the
 * shader then samples whatever the descriptors last held, as the fixed-function path
 * does. */
static gl_texture_object_t *
gl_gl2_sampler_texture(gl_context_t *ctx, const gl_program_object_t *prog, int s) {
    if (s < 0 || s >= prog->hw_tex_sets)
        return (gl_texture_object_t *)0;
    const int ui = prog->hw_tex_uniform[s];
    if (ui < 0 || ui >= prog->uniform_count || !prog->values)
        return (gl_texture_object_t *)0;
    /* The shader's limit, not the fixed-function one: a sampler uniform names a texture
     * image unit, bounded by `GL_MAX_TEXTURE_IMAGE_UNITS`. */
    const float unit_f = prog->values[prog->uniforms[ui].offset];
    if (unit_f < 0.0f || unit_f >= (float)OOPS_GL_MAX_TEXTURE_IMAGE_UNITS) {
        return (gl_texture_object_t *)0;
    }
    /* The sampler's type picks the target; `glEnable` does not. GL 2.0 samples whatever
     * is bound to the unit, so the fixed-function `gl_unit_texture_id` (which asks what
     * the unit is enabled for) does not apply. A `samplerCube` takes the unit's cube
     * binding, a `sampler2D` its 2D one. Nothing bound means the default texture, a
     * real object; an incomplete one samples as none. */
    const gl_tex_unit_t *tu = &ctx->tex_unit[(GLuint)unit_f];
    const GLenum st = prog->uniforms[ui].type;
    const GLboolean want_cube = (GLboolean)(st == GL_SAMPLER_CUBE);
    GLuint id;
    if (want_cube) {
        id = tu->bound_texture_cube ? tu->bound_texture_cube
                                    : OOPS_GL_DEFAULT_TEXTURE_CUBE;
    } else if (st == GL_SAMPLER_3D) {
        id = tu->bound_texture_3d ? tu->bound_texture_3d : OOPS_GL_DEFAULT_TEXTURE_3D;
    } else if (st == GL_SAMPLER_1D || st == GL_SAMPLER_1D_SHADOW) {
        id = tu->bound_texture_1d ? tu->bound_texture_1d : OOPS_GL_DEFAULT_TEXTURE_1D;
    } else {
        id = tu->bound_texture_2d ? tu->bound_texture_2d : OOPS_GL_DEFAULT_TEXTURE_2D;
    }
    {
        const gl_texture_object_t *probe = gl_lookup_texture(ctx, id);
        if (!probe || !gl_texture_complete(probe))
            return (gl_texture_object_t *)0;
        /* A cube whose six faces have not all arrived has no array to sample, so it is
         * treated as unbound, as the fixed-function path treats it. */
        if (want_cube && probe->cube_hw_dim <= 0 && !probe->cube) {
            return (gl_texture_object_t *)0;
        }
        /* A comparison's result is spread by the texture's `GL_DEPTH_TEXTURE_MODE`,
         * chosen after the shader was compiled. The compiled path emits the default
         * `GL_LUMINANCE` spread, and any other mode is logged once. */
        if (st == GL_SAMPLER_2D_SHADOW && probe->depth_mode != GL_LUMINANCE &&
            !ctx->hw_depth_mode_logged) {
            oops_log_info(
                "GL",
                "a compiled shadow lookup spreads its comparison as GL_LUMINANCE: "
                "GL_DEPTH_TEXTURE_MODE is a per-texture choice and the shader is "
                "compiled once");
            ctx->hw_depth_mode_logged = GL_TRUE;
        }
    }
    for (int ti = 0; ti < OOPS_GL_MAX_TEXTURE_OBJECTS; ti++) {
        if (ctx->textures[ti].used && ctx->textures[ti].id == id)
            return &ctx->textures[ti];
    }
    return (gl_texture_object_t *)0;
}

/* Builds the whole block: descriptors at 0x00 and 0x40, the uniform pool at 0x80.
 *
 * Zeroed and built whole every time, because the block is compared against the slot to
 * decide whether this draw needs a new one, and leftover bytes would falsify that. */
static void gl_gl2_build_block(gl_context_t *ctx, const gl_program_object_t *prog,
                               uint32_t *block) {
    memset(block, 0, OOPS_GL_GL2_SLOT_STRIDE);
    /* The shader's descriptor sets, not the fixed-function stages - the block has
     * `OOPS_GL_GL2_TEX_SETS` of them and the link assigned that many at most. */
    for (int s = 0; s < prog->hw_tex_sets && s < (int)OOPS_GL_GL2_TEX_SETS; s++) {
        gl_texture_object_t *obj = gl_gl2_sampler_texture(ctx, prog, s);
        if (!obj)
            continue;
        uint32_t *set = block + (size_t)s * (OOPS_GL_DESC_UNIT_STRIDE / 4u);
        memcpy(set, obj->img_desc, 32);
        memcpy(set + 8, obj->samp_desc, 16);
        /* GL 1.4's bias, the texture's and the unit's, joins the sampler here rather
         * than in the texture's own descriptor, since half of it is context state - the
         * same place the fixed-function path puts it. */
        const int ui = prog->hw_tex_uniform[s];
        /* `gl_gl2_sampler_texture` already refuses an out-of-range unit; the bound is
         * restated because this indexes `tex_unit` with an application-chosen value. */
        const GLuint unit = (GLuint)prog->values[prog->uniforms[ui].offset];
        if (unit >= (GLuint)OOPS_GL_MAX_TEXTURE_UNITS)
            continue;
        set[10] |= gl_hw_lod_bias_bits(gl_tex_lod_bias(&ctx->tex_unit[unit], obj));
    }
    if (prog->value_floats > 0 && prog->values) {
        /* Clamped to what the block holds. The compiler refuses a larger pool; the
         * clamp keeps this memcpy inside the slot without relying on that. */
        int n = prog->value_floats;
        if (n > OOPS_GL_GL2_UNIFORM_FLOATS)
            n = OOPS_GL_GL2_UNIFORM_FLOATS;
        memcpy((char *)block + OOPS_GL_GL2_UNIFORM_AT, prog->values,
               (size_t)n * sizeof(float));
    }
    /* The draw's own constants, which change without the program. The target height
     * turns the hardware's window y (down from the top) into `gl_FragCoord`'s (up from
     * the bottom). Always written, so it is never stale. */
    {
        float dc[OOPS_GL_GL2_DRAWCONST_FLOATS] = {0.0f, 0.0f, 0.0f, 0.0f};
        dc[OOPS_GL_GL2_DC_TARGET_H] = (float)(ctx->height ? ctx->height : 1080u);
        memcpy((char *)block + OOPS_GL_GL2_DRAWCONST_AT, dc, sizeof(dc));
    }
}

static void gl_hw_begin_frame(gl_context_t *ctx) {
    uint32_t *dw = ctx->dcb_mem;
    uint64_t color_gpu = (uint64_t)(uintptr_t)ctx->framebuffer;
    /* The second colour target's address, 0 when GL names one buffer. Both buffers are
     * allocated the same way (gl_front_buffer), so the pointer is the GPU address here
     * too. */
    const uint64_t also_gpu = (uint64_t)(uintptr_t)ctx->fb_also;

    /* An experiment's prelude (glSetHardwarePrelude) goes first, so this frame's own
     * state is written over it. */
    if (ctx->hw_prelude && ctx->hw_prelude_words) {
        for (uint32_t i = 0; i < ctx->hw_prelude_words; i++)
            *dw++ = ctx->hw_prelude[i];
    }
    uint64_t payload_va = (uint64_t)(uintptr_t)ctx->gpu_payload;

    uint32_t w = ctx->width ? ctx->width : 1920;
    uint32_t h = ctx->height ? ctx->height : 1080;
    uint32_t scissor_br = ((h & 0x7fffu) << 16) | (w & 0x7fffu);

    /* The static state is the measured recipe oops_agc_draw_primitive carries
     * (`docs/hardware/agc-gl-cube-oracle-fw1240.md`): NGG in passthrough mode, no
     * depth. Entries marked (patched) take the frame's own values below. The GL-facing
     * delta is the two parameter exports (colour, texcoord) the pixel shaders
     * interpolate: SPI_VS_OUT_CONFIG, SPI_PS_IN_CONTROL and the first two
     * SPI_PS_INPUT_CNTL slots. */
    static const struct {
        uint32_t reg;
        uint32_t val;
    } ctx_regs[] = {
        {0x318u, 0},           /* CB_COLOR0_BASE (patched) */
        {0x390u, 0},           /* CB_COLOR0_BASE_EXT (patched) */
        {0x31bu, 0x00000000u}, /* CB_COLOR0_VIEW */
        /* CB_COLOR0_INFO: COLOR_8_8_8_8, LINEAR_GENERAL, UNORM, COMP_SWAP=ALT (bytes
         * B,G,R,A: the 0xAARRGGBB framebuffer), BLEND_CLAMP set.
         *
         * BLEND_BYPASS (bit 16) is clear: it is read before the blender and disables
         * it. Mesa sets clamp for NORM/SRGB and bypass only for UINT/SINT and the
         * 8_24/24_8/X24_8_32_FLOAT formats (mesa/src/amd/common/ac_descriptors.c:
         * 1426-1438), so an 8_8_8_8 UNORM target gets clamp=1, bypass=0.
         *
         * LINEAR_GENERAL (bit 7) is clear: on this generation linearity is
         * `CB_COLOR0_ATTRIB3.COLOR_SW_MODE`, and radeonsi and radv never set
         * `S_028C70_LINEAR_GENERAL`. */
        {0x31cu, 0x00008828u},
        {0x31du, 0x00000000u}, /* CB_COLOR0_ATTRIB */
        {0x31eu, 0x00000000u}, /* CB_COLOR0_DCC_CONTROL: disabled */
        {0x3b0u, 0},           /* CB_COLOR0_ATTRIB2 (patched: extent) */
        /* CB_COLOR0_ATTRIB3: COLOR_SW_MODE=LINEAR, RESOURCE_TYPE=2D (patched to
         * OOPS_GL_RX_ATTRIB3 on the scanout path). agc_draw.c's 0x08c6c000 carries
         * 64KB_R_X, which does not describe this linear buffer.
         *
         * RESOURCE_TYPE (bits 25:24) is 2D: a render target is a 2D surface,
         * `ADDR_RSRC_TEX_2D`, which is `RADEON_RESOURCE_2D` = 1
         * (ac_surface.c:2739-2744, ac_surface.h:145). */
        {0x3b8u, 0x09c00000u},
        /* The second colour target, for glDrawBuffer(GL_FRONT_AND_BACK). Patched below
         * from `ctx->fb_also`, and left unbound - INFO 0, and out of both masks - when
         * GL names one buffer. Offsets from Mesa `src/amd/registers/gfx103.json`:
         * CB_COLOR1_BASE :5508, _VIEW :5525, _INFO :5531, _ATTRIB :5537, _DCC_CONTROL
         * :5543, _BASE_EXT :6060, _ATTRIB2 :6252, _ATTRIB3 :6300. */
        {0x327u, 0},           /* CB_COLOR1_BASE (patched) */
        {0x391u, 0},           /* CB_COLOR1_BASE_EXT (patched) */
        {0x32au, 0x00000000u}, /* CB_COLOR1_VIEW */
        {0x32bu, 0}, /* CB_COLOR1_INFO (patched: colour 0's, or 0 when unbound) */
        {0x32cu, 0x00000000u}, /* CB_COLOR1_ATTRIB */
        {0x32du, 0x00000000u}, /* CB_COLOR1_DCC_CONTROL: disabled */
        {0x3b1u, 0},           /* CB_COLOR1_ATTRIB2 (patched: extent) */
        {0x3b9u, 0},           /* CB_COLOR1_ATTRIB3 (patched: colour 0's swizzle) */

        /* The rest of each colour target's block, as Mesa emits it: fourteen
         * consecutive registers from CB_COLOR0_BASE (si_state.c:2777-2792, the GFX10
         * arm) plus the _EXT halves. The system compositor drives the same colour
         * block, so every register is written rather than inherited.
         *
         * This target has no CMASK, FMASK or DCC metadata, so their bases are zero, as
         * are the fast-clear CLEAR_WORDs and the holes Mesa emits as zero.
         *
         * The per-target stride is 0xf dwords; the _EXT registers are one dword apart
         * (R_028E60 CMASK_BASE_EXT = 0x398, R_028E80 FMASK_BASE_EXT = 0x3a0, R_028EA0
         * DCC_BASE_EXT = 0x3a8). */
        {0x319u, 0x00000000u}, /* hole, as Mesa emits it */
        {0x31au, 0x00000000u}, /* hole */
        {0x31fu, 0x00000000u}, /* CB_COLOR0_CMASK: no metadata surface */
        {0x320u, 0x00000000u}, /* hole */
        {0x321u, 0x00000000u}, /* CB_COLOR0_FMASK: no metadata surface */
        {0x322u, 0x00000000u}, /* hole */
        {0x323u, 0x00000000u}, /* CB_COLOR0_CLEAR_WORD0 */
        {0x324u, 0x00000000u}, /* CB_COLOR0_CLEAR_WORD1 */
        {0x325u, 0x00000000u}, /* CB_COLOR0_DCC_BASE */
        {0x398u, 0x00000000u}, /* CB_COLOR0_CMASK_BASE_EXT */
        {0x3a0u, 0x00000000u}, /* CB_COLOR0_FMASK_BASE_EXT */
        {0x3a8u, 0x00000000u}, /* CB_COLOR0_DCC_BASE_EXT */
        {0x328u, 0x00000000u}, /* hole */
        {0x329u, 0x00000000u}, /* hole */
        {0x32eu, 0x00000000u}, /* CB_COLOR1_CMASK */
        {0x32fu, 0x00000000u}, /* hole */
        {0x330u, 0x00000000u}, /* CB_COLOR1_FMASK */
        {0x331u, 0x00000000u}, /* hole */
        {0x332u, 0x00000000u}, /* CB_COLOR1_CLEAR_WORD0 */
        {0x333u, 0x00000000u}, /* CB_COLOR1_CLEAR_WORD1 */
        {0x334u, 0x00000000u}, /* CB_COLOR1_DCC_BASE */
        {0x399u, 0x00000000u}, /* CB_COLOR1_CMASK_BASE_EXT */
        {0x3a1u, 0x00000000u}, /* CB_COLOR1_FMASK_BASE_EXT */
        {0x3a9u, 0x00000000u}, /* CB_COLOR1_DCC_BASE_EXT */

        {0x109u, 0x00000000u}, /* CB_DCC_CONTROL: disabled */
        {0x202u,
         0x00cc0010u}, /* CB_COLOR_CONTROL: CB_NORMAL, ROP3_COPY (patched: glLogicOp) */
        {0x08eu, 0x0000000fu}, /* CB_TARGET_MASK (patched) */
        {0x08fu,
         0x0000000fu}, /* CB_SHADER_MASK: MRT0 four components (patched: MRT1 too) */
        {0x1e0u, 0x00000000u}, /* CB_BLEND0_CONTROL (patched) */
        /* Blending is per target: radeonsi writes `R_028780_CB_BLEND0_CONTROL + i * 4`
         * for all eight (`si_state.c:420`), and `CB_BLEND1_CONTROL` is `0x028784`
         * (`mesa/src/amd/common/amdgfxregs.h:12802`), context offset 0x1e1. Zero (off)
         * when the second target is unbound. */
        {0x1e1u,
         0x00000000u}, /* CB_BLEND1_CONTROL (patched: colour 0's, or 0 when unbound) */
        /*
         * RB+, written off rather than inherited. This part has RB+ (`rbplus_allowed`
         * for every `gfx_level >= GFX10_3`, `ac_gpu_info.c:1122`): a second description
         * of the blend in five registers beside `CB_BLEND0_CONTROL`, which radeonsi
         * programs on every framebuffer change (`si_state.c:162`, `:543`).
         *
         * Zero is radeonsi's own off switch: `COLOR_COMB_FCN` and `ALPHA_COMB_FCN` of
         * `OPT_COMB_NONE` (`si_state.c:490`, `:537`), with which the
         * `SRC_OPT`/`DST_OPT` fields are not read. `SX_PS_DOWNCONVERT` zero is no
         * downconversion.
         */
        /* `SX_PS_DOWNCONVERT_CONTROL` exists on gfx10.3 only. 0xff is what radeonsi
         * writes (`ac_cmdbuf.c:549`); the field is `MRT<n>_FMT_MAPPING_DISABLE`, so
         * all-ones turns every target's format mapping off. */
        {0x1d4u,
         0x000000ffu}, /* SX_PS_DOWNCONVERT_CONTROL: mapping disabled on every target */
        {0x1d5u, 0x00000000u}, /* SX_PS_DOWNCONVERT: none */
        {0x1d6u, 0x00000000u}, /* SX_BLEND_OPT_EPSILON */
        {0x1d7u, 0x00000000u}, /* SX_BLEND_OPT_CONTROL */
        /*
         * RB+'s per-target hints, all zero. The operand fields take `SX_BLEND_OPT`,
         * where 0 is `PRESERVE_NONE_IGNORE_ALL`; the `_COMB_FCN` fields take
         * `SX_OPT_COMB_FCN`, where 0 is `OPT_COMB_NONE`, which disables the operands.
         * Fields: `COLOR_SRC_OPT` [0,2], `COLOR_DST_OPT` [4,6], `COLOR_COMB_FCN`
         * [8,10], `ALPHA_SRC_OPT` [16,18], `ALPHA_DST_OPT` [20,22], `ALPHA_COMB_FCN`
         * [24,26]
         * (`mesa/src/amd/registers/gfx10.json:14940-14948`, enum :535-545). radeonsi
         * derives them per target from the blend factors (`si_state.c:461-464`,
         * `:478-486`); `test_pm4_gl_rbplus_blend_opt_is_written_off` pins 0x1d5..0x1d9.
         */
        {0x1d8u, 0x00000000u}, /* SX_MRT0_BLEND_OPT */
        {0x1d9u, 0x00000000u}, /* SX_MRT1_BLEND_OPT */
        {0x200u, 0x00000000u}, /* DB_DEPTH_CONTROL (patched) */
        {0x201u, 0x00010000u}, /* DB_EQAA */
        {0x203u, 0x00000010u}, /* DB_SHADER_CONTROL: EARLY_Z_THEN_LATE_Z */
        {0x08cu, 0xaa99aaaau}, /* PA_SC_EDGERULE */
        {0x1d4u, 0x000000ffu}, /* SX_PS_DOWNCONVERT_CONTROL */
        {0x291u,
         0x20040100u}, /* VGT_GS_ONCHIP_CNTL: ES_VERTS=256, GS_PRIMS=128,
                          GS_INST_PRIMS=128 (subgroup sizing as radeonsi programs it) */
        /* TRISTRIP: with 0 (POINTLIST) a three-vertex draw lands one pixel, as the
         * gl-cube oracle record says (`docs/hardware/agc-gl-cube-oracle-fw1240.md`). */
        {0x29bu, 0x00000002u}, /* VGT_GS_OUT_PRIM_TYPE: TRISTRIP */
        {0x2d3u, 0x00000001u}, /* GE_NGG_SUBGRP_CNTL: PRIM_AMP=1 */
        {0x2d5u,
         0x00c12010u},         /* VGT_SHADER_STAGES_EN: ES_EN=REAL | PRIMGEN_EN |
                                  MAX_PRIMGRP_IN_WAVE=2 | GS_W32 | VS_W32 (NGG, wave32) */
        {0x1ffu, 0x00000100u}, /* GE_MAX_OUTPUT_PER_SUBGROUP: 256 */
        {0x20eu, 0x00000078u}, /* PA_CL_NGG_CNTL: VERTEX_REUSE_DEPTH=30 */
        {0x2a1u, 0x00000000u}, /* VGT_PRIMITIVEID_EN */
        {0x2a6u, 0x00000040u}, /* VGT_DRAW_PAYLOAD_CNTL */
        {0x2adu, 0x00000000u}, /* VGT_REUSE_OFF */
        {0x2abu,
         0x00000001u}, /* VGT_ESGS_RING_ITEMSIZE: 1, so the vertex offsets handed to the
                          primitive thread are plain vertex indices */
        {0x2ceu, 0x00000400u}, /* VGT_GS_MAX_VERT_OUT */
        {0x2e4u, 0x00000000u}, /* VGT_GS_INSTANCE_CNT: 0 */
        {0x290u, 0x00000000u}, /* VGT_GS_MODE: off */
        {0x2d4u, 0x88101000u}, /* VGT_TESS_DISTRIBUTION */
        {0x103u, 0xffffffffu}, /* VGT_MULTI_PRIM_IB_RESET_INDX */
        {0x30eu, 0xffffffffu}, /* PA_SC_AA_MASK_X0Y0_X1Y0 */
        {0x30fu, 0xffffffffu}, /* PA_SC_AA_MASK_X0Y1_X1Y1 */
        {0x310u, 0x00000000u}, /* PA_SC_SHADER_CONTROL */
        {0x314u, 0x00000202u}, /* PA_SC_NGG_MODE_CNTL */
        {0x311u, 0x01fd2002u}, /* PA_SC_BINNER_CNTL_0 */
        {0x312u, 0x03ff0080u}, /* PA_SC_BINNER_CNTL_1 */
        {0x313u, 0x00006000u}, /* PA_SC_CONSERVATIVE_RASTERIZATION_CNTL */
        {0x00eu, 0x00000002u}, /* DB_DFSM_CONTROL */
        /* Defaults, matching an independent command stream that draws on this
         * hardware. Nothing here submits a native point or line, so they do not affect
         * a pixel. */
        {0x280u, 0x00080008u}, /* PA_SU_POINT_SIZE */
        {0x281u, 0xffff0000u}, /* PA_SU_POINT_MINMAX */
        {0x282u, 0x00000008u}, /* PA_SU_LINE_CNTL */
        /* PA_SU_POLY_OFFSET_DB_FMT_CNTL: radeonsi writes `NEG_NUM_DB_BITS(-23) |
         * DB_IS_FLOAT_FMT(1)` for a 32-bit float z-buffer
         * (`src/gallium/drivers/radeonsi/si_state.c`, the `uses_poly_offset` block):
         * 0xe9 with the float bit at 8 is 0x1e9. On that path glPolygonOffset's units
         * go in unscaled. */
        {0x2deu, 0x000001e9u}, /* PA_SU_POLY_OFFSET_DB_FMT_CNTL */
        {0x2dfu, 0},           /* PA_SU_POLY_OFFSET_CLAMP (patched) */
        {0x2e0u, 0},           /* PA_SU_POLY_OFFSET_FRONT_SCALE (patched) */
        {0x2e1u, 0},           /* PA_SU_POLY_OFFSET_FRONT_OFFSET (patched) */
        {0x2e2u, 0},           /* PA_SU_POLY_OFFSET_BACK_SCALE (patched) */
        {0x2e3u, 0},           /* PA_SU_POLY_OFFSET_BACK_OFFSET (patched) */
        {0x00cu, 0x00000000u}, /* PA_SC_SCREEN_SCISSOR_TL */
        {0x00du, 0},           /* PA_SC_SCREEN_SCISSOR_BR (patched) */
        {0x081u, 0x80000000u}, /* PA_SC_WINDOW_SCISSOR_TL: WINDOW_OFFSET_DISABLE */
        {0x082u, 0},           /* PA_SC_WINDOW_SCISSOR_BR (patched) */
        {0x090u, 0x80000000u}, /* PA_SC_GENERIC_SCISSOR_TL: WINDOW_OFFSET_DISABLE */
        {0x091u, 0},           /* PA_SC_GENERIC_SCISSOR_BR (patched) */
        {0x094u, 0x80000000u}, /* PA_SC_VPORT_SCISSOR_0_TL: WINDOW_OFFSET_DISABLE */
        {0x095u, 0},           /* PA_SC_VPORT_SCISSOR_0_BR (patched) */
        {0x0b4u, 0x00000000u}, /* PA_SC_VPORT_ZMIN_0: 0.0f */
        {0x0b5u, 0x3f800000u}, /* PA_SC_VPORT_ZMAX_0: 1.0f */
        {0x10fu, 0},           /* PA_CL_VPORT_XSCALE (patched) */
        {0x110u, 0},           /* PA_CL_VPORT_XOFFSET (patched) */
        {0x111u, 0},           /* PA_CL_VPORT_YSCALE (patched) */
        {0x112u, 0},           /* PA_CL_VPORT_YOFFSET (patched) */
        {0x113u, 0},           /* PA_CL_VPORT_ZSCALE (patched: glDepthRange) */
        {0x114u, 0},           /* PA_CL_VPORT_ZOFFSET (patched: glDepthRange) */
        {0x083u, 0x0000ffffu}, /* PA_SC_CLIPRECT_RULE */
        {0x084u, 0x00000000u}, /* PA_SC_CLIPRECT_0_TL */
        {0x085u, 0x20002000u}, /* PA_SC_CLIPRECT_0_BR */
        {0x204u,
         0x00000000u}, /* PA_CL_CLIP_CNTL: standard clipping, UCP_ENA_0..5 (patched) */
        {0x206u,
         0x0000043fu}, /* PA_CL_VTE_CNTL: viewport scale and offset on x, y, z */
        {0x207u, 0x00000000u}, /* PA_CL_VS_OUT_CNTL */
        {0x2fau, 0x40800000u}, /* PA_CL_GB_VERT_CLIP_ADJ: 4.0f */
        {0x2fbu, 0x40800000u}, /* PA_CL_GB_VERT_DISC_ADJ: 4.0f */
        {0x2fcu, 0x40800000u}, /* PA_CL_GB_HORZ_CLIP_ADJ: 4.0f */
        {0x2fdu, 0x40800000u}, /* PA_CL_GB_HORZ_DISC_ADJ: 4.0f */
        {0x205u, 0x00000240u}, /* PA_SU_SC_MODE_CNTL (patched) */
        {0x20cu, 0x00000000u}, /* PA_SU_SMALL_PRIM_FILTER_CNTL */
        {0x292u, 0x00000002u}, /* PA_SC_MODE_CNTL_0: VPORT_SCISSOR_ENABLE */
        {0x293u, 0x06020000u}, /* PA_SC_MODE_CNTL_1 */
        {0x2f8u, 0x00000000u}, /* PA_SC_AA_CONFIG: 1x */
        {0x2f9u, 0x0000002du}, /* PA_SU_VTX_CNTL */
        {0x191u, 0x00000000u}, /* SPI_PS_INPUT_CNTL_0: parameter 0 (colour), smooth */
        {0x192u, 0x00000001u}, /* SPI_PS_INPUT_CNTL_1: parameter 1 (texcoord), smooth */
        {0x1b1u, 0x00000002u}, /* SPI_VS_OUT_CONFIG: VS_EXPORT_COUNT (bits 5:1) = 1, two
                                  parameters */
        {0x1c2u, 0x00000001u}, /* SPI_SHADER_IDX_FORMAT */
        {0x1c3u, 0x00000004u}, /* SPI_SHADER_POS_FORMAT: POS0 = 4COMP */
        {0x1c4u, 0x00000000u}, /* SPI_SHADER_Z_FORMAT: no Z export */
        /* SPI_SHADER_COL_FORMAT: COL0 = FP16_ABGR (patched: COL1 too). Mesa's
           ac_choose_spi_color_formats requires the half-float format for an 8_8_8_8
           target wherever RB+ is allowed; blending is wrong without it. See
           gl_ps_patch_export. */
        {0x1c5u, 0x00000004u},
        {0x1b3u,
         0x00000002u}, /* SPI_PS_INPUT_ENA: PERSP_CENTER_ENA (patched: the stipple) */
        {0x1b4u,
         0x00000002u}, /* SPI_PS_INPUT_ADDR: PERSP_CENTER_ENA (patched: the stipple) */
        {0x1b5u, 0x00000001u}, /* SPI_INTERP_CONTROL_0: FLAT_SHADE_ENA (no parameter is
                                  flagged flat) */
        /* SPI_PS_IN_CONTROL: NUM_INTERP=2, and PS_W32_EN - see GL_SPI_PS_W32_EN. */
        {0x1b6u, GL_SPI_PS_W32_EN | 0x00000002u},
        /* `SPI_BARYC_CNTL` is zero, as radeonsi writes it (`si_state.c:4895`). Its
         * `FRONT_FACE_ALL_BITS` (bit 24) would make the face register an integer mask;
         * clear, it is a float, positive for front-facing, which is what `glsl_ps.c`'s
         * `v_cmp_gt_f32` expects (Mesa lowers `load_front_face` to `fgt(reg, 0)`,
         * `ac_nir_lower_intrinsics_to_args.c:361`). As a mask, both windings compare
         * false: 0.0 is not > 0.0, and all-ones is a NaN. */
        {0x1b8u, 0x00000000u}, /* SPI_BARYC_CNTL */
        /* TA_BC_BASE_ADDR and _HI (patched): the border colour table a sampler's
         * SQ_TEX_BORDER_COLOR_REGISTER reads, in the payload - mm 0x28080 and 0x28084
         * for gfx103 (gfx103.json:2932-2943), which radeonsi sets once in its GFX10
         * preamble (ac_cmdbuf.c:529-530). Read only by a texture whose border colour is
         * none of the three built-in ones. */
        {0x020u, 0},
        {0x021u, 0},
    };

    for (size_t i = 0; i < sizeof(ctx_regs) / sizeof(ctx_regs[0]); i++) {
        uint32_t reg = ctx_regs[i].reg;
        uint32_t val = ctx_regs[i].val;
        if (reg == 0x318u) {
            val = (uint32_t)(color_gpu >> 8);
        } else if (reg == 0x020u) {
            val = (uint32_t)((payload_va + OOPS_GL_BORDER_TABLE_OFFSET) >> 8);
        } else if (reg == 0x021u) {
            val = (uint32_t)((payload_va + OOPS_GL_BORDER_TABLE_OFFSET) >> 40) & 0xffu;
        } else if (reg == 0x390u) {
            val = (uint32_t)(color_gpu >> 40);
        } else if (reg == 0x3b0u) {
            val = OOPS_AGC_CB_COLOR_ATTRIB2(w, h);
        } else if (reg == 0x3b8u && ctx->hw_rx) {
            /* The scanout path draws the scanout buffer in its own swizzle (gl_rx.h).
             */
            val = OOPS_GL_RX_ATTRIB3;
        } else if (reg == 0x00du || reg == 0x082u || reg == 0x091u) {
            val = scissor_br;
        } else if (reg == 0x204u) {
            val = gl_compute_clip_cntl(ctx);
        } else if (reg == 0x094u || reg == 0x095u) {
            /* The viewport scissor is the only one of the four that carries GL's box.
             */
            uint32_t sc[2];
            gl_compute_scissor(ctx, w, h, sc);
            val = sc[reg - 0x094u];
        } else if (reg == 0x10fu || reg == 0x110u || reg == 0x111u || reg == 0x112u) {
            uint32_t vport[4];
            gl_compute_vport(ctx, h, vport);
            val = vport[reg - 0x10fu];
        } else if (reg == 0x113u) {
            /* Window z = z_ndc * ZSCALE + ZOFFSET, and glDepthRange(n, f) puts NDC
             * -1..1 onto n..f - so ZSCALE is (f - n) / 2 and ZOFFSET is (f + n) / 2.
             * The default range gives 0.5 and 0.5, the gl-cube oracle record's values.
             */
            val = gl_f32_bits((ctx->depth_far - ctx->depth_near) * 0.5f);
        } else if (reg == 0x114u) {
            val = gl_f32_bits((ctx->depth_far + ctx->depth_near) * 0.5f);
        } else if (reg == 0x2dfu) {
            /* glPolygonOffset has no clamp (that is EXT_polygon_offset_clamp), so zero,
             * the disabled value. */
            val = 0u;
        } else if (reg == 0x2e0u || reg == 0x2e2u) {
            /* Scale, front and back. Mesa multiplies the factor by 16 (si_state.c). */
            val = gl_f32_bits(ctx->polygon_offset_factor * 16.0f);
        } else if (reg == 0x2e1u || reg == 0x2e3u) {
            /* Offset, front and back. Unscaled on the 32-bit float z path, which the
             * DB_FMT_CNTL above establishes this is. */
            val = gl_f32_bits(ctx->polygon_offset_units);
        } else if (reg == 0x1e0u) {
            val = gl_compute_cb_blend_control(ctx);
        } else if (reg == 0x1e1u) {
            /* The second target blends as the first: GL has one blend state. Zero
             * (disabled) when there is no second target. */
            val = ctx->fb_also ? gl_compute_cb_blend_control(ctx) : 0u;
        } else if (reg == 0x202u) {
            val = gl_compute_cb_color_control(ctx);
        } else if (reg == 0x200u) {
            val = gl_compute_db_depth_control(ctx);
        } else if (reg == 0x205u) {
            val = gl_compute_pa_su_sc_mode_cntl(ctx);
        } else if (reg == 0x08eu) {
            /* Both targets take the same write mask: GL writes the same fragment to
             * both buffers, and glColorMask names channels, not buffers. */
            val = gl_compute_cb_target_mask(ctx);
            if (also_gpu)
                val |= val << 4;
        } else if (reg == 0x08fu || reg == 0x1c5u) {
            /* The shader exports to MRT1 as well (gl_ps_patch_export), so both the mask
             * and the export format carry a second copy: 0xff, and 0x44 for the two
             * half-float exports. */
            val = also_gpu ? (reg == 0x08fu ? 0x000000ffu : 0x00000044u) : val;
        } else if (reg == 0x1b3u || reg == 0x1b4u) {
            /* A stippled draw needs the fragment's window position: POS_X_FLOAT_ENA
             * (bit 8) and POS_Y_FLOAT_ENA (bit 9) beside PERSP_CENTER_ENA, which puts
             * POS_X in v2 and POS_Y in v3 after the barycentrics. Both registers carry
             * the same value. */
            val = gl_polygon_stipple_on(ctx) ? 0x00000302u : 0x00000002u;
        } else if (reg == 0x327u) {
            val = (uint32_t)(also_gpu >> 8);
        } else if (reg == 0x391u) {
            val = (uint32_t)(also_gpu >> 40);
        } else if (reg == 0x32bu) {
            /* Colour 0's format, or 0 when unbound, so a mask edit alone cannot start
             * writing memory the base register does not name. */
            val = also_gpu ? 0x00008828u : 0u;
        } else if (reg == 0x3b1u) {
            val = OOPS_AGC_CB_COLOR_ATTRIB2(w, h);
        } else if (reg == 0x3b9u) {
            /* The same swizzle as colour 0: both buffers are the same kind of surface,
             * linear here and 64KB_R_X on the scanout path. */
            val = ctx->hw_rx ? OOPS_GL_RX_ATTRIB3 : 0x09c00000u;
        }
        *dw++ = 0xc0016900u;
        *dw++ = reg;
        *dw++ = val;
    }

    for (uint32_t i = 2; i < 32; i++) {
        *dw++ = 0xc0016900u;
        *dw++ = 0x191u + i;
        *dw++ = 0u;
    }

    /* Shader stages. Every graphics stage is pointed at real code so no stage can fetch
     * from an unmapped address; on GFX10 the NGG wave takes its program counter from
     * PGM_LO_ES and its resource words from RSRC1/RSRC2_GS. */
    int textured = (gl_effective_texture_id(ctx) > 0u) ? 1 : 0;
    uint64_t ps_init_offset =
        textured ? OOPS_GL_PS_TEX_OFFSET : OOPS_GL_PS_UNTEX_OFFSET;
    uint32_t ps_rsrc2 =
        textured ? 0x00000004u
                 : 0u; /* USER_SGPR=2 (bits 5:1): s[0:1] = descriptor table */
    const struct {
        uint32_t base_reg;
        uint64_t va_offset;
        uint32_t rsrc1;
        uint32_t rsrc2;
    } stages[] = {
        {0x08u, ps_init_offset, 0x000c0010u, ps_rsrc2}, /* PS */
        {0x48u, 0x000u, 0x000c0010u,
         0x00000008u}, /* VS: USER_SGPR=4 (s0 = vbo offset) */
        {0x88u, 0x000u, 0x622c0046u,
         0x000b0008u}, /* GS/NGG: VGPRS=6 (56 in wave32), ES_VGPR_COMP_CNT=3, LDS_SIZE=1
                          granule, USER_SGPR=4 (user data 0 = vbo offset, which lands in
                          s8) */
        {0xc8u, 0x000u, 0x000c0010u, 0x00000000u},  /* ES: the NGG program counter */
        {0x108u, 0x000u, 0x000c0010u, 0x00000000u}, /* HS */
        {0x148u, 0x000u, 0x000c0010u, 0x00000000u}, /* LS */
    };
    for (size_t s = 0; s < sizeof(stages) / sizeof(stages[0]); s++) {
        uint32_t base_reg = stages[s].base_reg;
        uint64_t s_va = payload_va + stages[s].va_offset;
        *dw++ = 0xc0017600u;
        *dw++ = base_reg;
        *dw++ = (uint32_t)(s_va >> 8);
        *dw++ = 0xc0017600u;
        *dw++ = base_reg + 1u;
        *dw++ = (uint32_t)(s_va >> 40);
        *dw++ = 0xc0017600u;
        *dw++ = base_reg + 2u;
        *dw++ = stages[s].rsrc1;
        *dw++ = 0xc0017600u;
        *dw++ = base_reg + 3u;
        *dw++ = stages[s].rsrc2;
    }

    /* Compute-unit masks, as measured: CU1 stays clear for the NGG stage so pixel waves
     * can always find a home. */
    static const struct {
        uint32_t reg;
        uint32_t val;
    } spi_cu_regs[] = {
        {0x007u,
         0x003fffffu}, /* SPI_SHADER_PGM_RSRC3_PS: CU_EN=0xffff, WAVE_LIMIT=0x3f */
        {0x001u, 0x0000ffffu}, /* SPI_SHADER_PGM_RSRC4_PS: CU_EN=0xffff */
        {0x030u, 0x00000007u}, /* SPI_SHADER_REQ_CTRL_PS */
        {0x046u, 0x003fffffu}, /* SPI_SHADER_PGM_RSRC3_VS */
        {0x041u, 0x0000ffffu}, /* SPI_SHADER_PGM_RSRC4_VS */
        {0x087u, 0x003fffffu}, /* SPI_SHADER_PGM_RSRC3_GS: every CU, WAVE_LIMIT=0x3f */
        {0x081u, 0x0000ffffu}, /* SPI_SHADER_PGM_RSRC4_GS */
        {0x107u, 0x003fffffu}, /* SPI_SHADER_PGM_RSRC3_HS */
    };
    for (size_t i = 0; i < sizeof(spi_cu_regs) / sizeof(spi_cu_regs[0]); i++) {
        *dw++ = 0xc0017600u;
        *dw++ = spi_cu_regs[i].reg;
        *dw++ = spi_cu_regs[i].val;
    }

    *dw++ = 0xc0002f00u;
    *dw++ = 1u; /* PACKET3_NUM_INSTANCES */
    /* VGT_PRIMITIVE_TYPE goes through SET_UCONFIG_REG_INDEX with index 1: on GFX9 and
     * later the CP only forwards the primitive type to the geometry engine from the
     * indexed write, and a plain write leaves the GE assembling the previous client's
     * primitive type. */
    *dw++ = 0xc0017a00u;
    *dw++ = 0x10000242u;
    *dw++ = OOPS_AGC_PRIM_TRILIST; /* mmVGT_PRIMITIVE_TYPE, index 1 */
    *dw++ = 0xc0017900u;
    *dw++ = 0x25bu;
    *dw++ = 0x00020080u; /* mmGE_CNTL: VERT_GRP_SIZE=256, PRIM_GRP_SIZE=128 */
    *dw++ = 0xc0017900u;
    *dw++ = 0x260u;
    *dw++ = OOPS_AGC_GE_PC_ALLOC_DEFAULT; /* mmGE_PC_ALLOC */

    /* The user clip planes, written only when one is enabled so a frame that uses none
     * emits the recorded stream. Six planes of four floats are 24 consecutive
     * registers from PA_CL_UCP_0_X, so one packet carries all of them. */
    if (gl_compute_clip_cntl(ctx) != 0u) {
        gl_hw_emit_clip_planes(ctx, &dw);
    }

    ctx->dcb_words = (uint32_t)(dw - ctx->dcb_mem);
    ctx->hw_z_bound = GL_FALSE;
    ctx->hw_stencil_bound = GL_FALSE;
    /* The viewport, scissor and clip registers above were just written from the current
     * state. */
    ctx->hw_vport_dirty = GL_FALSE;
    ctx->hw_scissor_dirty = GL_FALSE;
    ctx->hw_clip_dirty = GL_FALSE;
    ctx->hw_depth_range_dirty = GL_FALSE;
    ctx->hw_color_control_dirty = GL_FALSE;
    ctx->hw_blend_color_dirty =
        GL_TRUE; /* not in the table above; first constant-factor draw sends it */
    ctx->hw_frame_tex = 0u; /* the new frame's descriptor slot holds nothing yet */
    ctx->hw_desc_slot =
        0u; /* and its textures start at the original table - see the ring */
    /* The slots are free again: the GPU read them in the submit that has retired. See
     * `hw_slot_tex`. */
    for (uint32_t i = 0u; i <= (uint32_t)OOPS_GL_DESC_RING_SLOTS; i++) {
        ctx->hw_slot_tex[i] = 0u;
        ctx->hw_slot_tex1[i] = 0u;
    }
    /* The cursor is back at slot 0, so the shadow no longer describes the slot the next
     * comparison is about. See `hw_desc_shadow`. */
    ctx->hw_desc_shadow_valid = 0u;
    ctx->hw_gl2_slot = 0u; /* the GL 2.0 block ring restarts with the frame ... */
    /* ... and so does the textured shader's variant ring: no queued draw points at a
     * slot, so all are free. The keys go too, so a dead slot is never matched. */
    for (uint32_t i = 0u; i < OOPS_GL_PS_RING_SLOTS; i++) {
        ctx->ps_ring_live[i] = GL_FALSE;
        ctx->ps_ring_key[i] = 0u;
    }
    ctx->ps_ring_next = 0u;
    ctx->hw_gl2_slot_program = 0u; /* ... and its first slot holds nothing */
    ctx->hw_params =
        2u; /* the stage table above bound the two-parameter vertex shader */
    /* What the table wrote into SPI_PS_INPUT_ENA/_ADDR, so only a draw needing
     * something else rewrites them. */
    ctx->hw_input_ena = gl_polygon_stipple_on(ctx) ? 0x00000302u : 0x00000002u;
    /* And the depth block's half: the table above wrote no Z export and an early test,
     * with no kill - so a draw that discards has to say so again in every frame. */
    ctx->hw_z_format = 0u;
    ctx->hw_db_shader_control = 0x00000010u;
    ctx->hw_frame_active = GL_TRUE;
}

/* -------------------------------------------------------------------------
 * The textured shader's variant ring
 *
 * Fixed-function state is implemented by patching instruction words of a pre-assembled
 * pixel shader. The copy at OOPS_GL_PS_TEX_OFFSET is a master that no draw binds;
 * draws bind a copy in the ring, and a patched master is published by copying it into a
 * free slot. A resident variant costs nothing; a new one costs a 1280-byte memcpy
 * instead of a submit and wait. See OOPS_GL_PS_RING_OFFSET.
 */

/* FNV-1a over the shader's words, recomputed only when a patch has dirtied it. A hash
 * rather than a comparison because it is matched once per draw, where a 1280-byte
 * memcmp would cost too much. */
#ifndef OOPS_HOST_BUILD
static void gl_ps_ring_hash(gl_context_t *ctx) {
    if (!ctx->ps_master_dirty)
        return;
    const uint32_t *const master =
        (const uint32_t *)((const char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0u; i < OOPS_GL_PS_TEX_WORDS; i++) {
        h ^= master[i];
        h *= 16777619u;
    }
    /* Zero is the "nothing here" key, so a shader hashing to it takes the next value.
     */
    ctx->ps_master_key = h ? h : 1u;
    ctx->ps_master_dirty = GL_FALSE;
}
#endif /* the host build binds the master directly and never rings */

/* Whether binding the current variant would need a slot the ring has not got. Asked
 * beside the descriptor ring's question, so a draw's one possible submission is decided
 * before anything of the draw is written. */
GLboolean gl_ps_ring_needs_submit(gl_context_t *ctx) {
#ifdef OOPS_HOST_BUILD
    (void)ctx;
    return GL_FALSE;
#else
    if (!ctx || !ctx->use_hardware || !ctx->gpu_payload)
        return GL_FALSE;
    gl_ps_ring_hash(ctx);
    for (uint32_t i = 0u; i < OOPS_GL_PS_RING_SLOTS; i++) {
        if (ctx->ps_ring_live[i] && ctx->ps_ring_key[i] == ctx->ps_master_key)
            return GL_FALSE;
    }
    return (GLboolean)(ctx->ps_ring_next >= OOPS_GL_PS_RING_SLOTS);
#endif
}

/* The address a textured draw should bind, as an offset into the payload. */
uint32_t gl_ps_ring_offset(gl_context_t *ctx) {
#ifdef OOPS_HOST_BUILD
    (void)ctx;
    return OOPS_GL_PS_TEX_OFFSET;
#else
    const uint32_t *const master =
        (const uint32_t *)((const char *)ctx->gpu_payload + OOPS_GL_PS_TEX_OFFSET);
    gl_ps_ring_hash(ctx);
    for (uint32_t i = 0u; i < OOPS_GL_PS_RING_SLOTS; i++) {
        if (ctx->ps_ring_live[i] && ctx->ps_ring_key[i] == ctx->ps_master_key) {
            return OOPS_GL_PS_RING_OFFSET + i * OOPS_GL_PS_TEX_WORDS * 4u;
        }
    }
    /* The submission was decided in gl_ps_ring_needs_submit, so a slot is free here.
       If none is, the last one is reused: a stale variant is wrong in one draw, where
       overwriting a slot a queued draw reads is wrong in all of them. */
    if (ctx->ps_ring_next >= OOPS_GL_PS_RING_SLOTS) {
        /* Counted and reported per flip beside the other counters, so an overrun that
         * shades draws with the wrong variant is visible. */
        ctx->hw_ps_ring_stale++;
        return OOPS_GL_PS_RING_OFFSET +
               (OOPS_GL_PS_RING_SLOTS - 1u) * OOPS_GL_PS_TEX_WORDS * 4u;
    }
    const uint32_t slot = ctx->ps_ring_next++;
    const uint32_t off = OOPS_GL_PS_RING_OFFSET + slot * OOPS_GL_PS_TEX_WORDS * 4u;
    memcpy((char *)ctx->gpu_payload + off, master, OOPS_GL_PS_TEX_WORDS * 4u);
    /* Flushed out of this core's caches, because `gl_ps_flush_shaders` covers only the
     * fixed shader ranges; unflushed, the GPU runs the previous shader. Only the slot
     * just written. */
#if defined(__x86_64__)
    for (size_t p = 0u; p < OOPS_GL_PS_TEX_WORDS * 4u; p += 64u) {
        __builtin_ia32_clflush(
            (const void *)((const char *)ctx->gpu_payload + off + p));
    }
#endif
    ctx->ps_ring_key[slot] = ctx->ps_master_key;
    ctx->ps_ring_live[slot] = GL_TRUE;
    return off;
#endif
}

/* The vertex stage's interface, switched between two, three and four parameters: the
 * NGG program (PGM_LO/HI of GS and ES, which the stage table points at the same code)
 * and the registers the parameter count lives in: SPI_VS_OUT_CONFIG's VS_EXPORT_COUNT,
 * SPI_PS_IN_CONTROL's NUM_INTERP, and SPI_PS_INPUT_CNTL_2/_3. Emitted only on a change,
 * so a two-parameter frame keeps the recorded gl-cube stream. Counted in
 * OOPS_GL_DCB_DRAW_MAX_DW. */
static void gl_hw_emit_param_count(gl_context_t *ctx, uint32_t **dw_ptr,
                                   uint32_t params) {
    if (ctx->hw_params == params)
        return;
    uint32_t *dw = *dw_ptr;
    const uint64_t off = (params >= 4u)   ? OOPS_GL_VS_P4_OFFSET
                         : (params == 3u) ? OOPS_GL_VS_P3_OFFSET
                                          : 0u;
    const uint64_t vs_va = (uint64_t)(uintptr_t)ctx->gpu_payload + off;
    static const uint32_t pgm_regs[2] = {0x88u,
                                         0xc8u}; /* SPI_SHADER_PGM_LO_GS, _LO_ES */
    for (int i = 0; i < 2; i++) {
        *dw++ = 0xc0017600u;
        *dw++ = pgm_regs[i];
        *dw++ = (uint32_t)(vs_va >> 8);
        *dw++ = 0xc0017600u;
        *dw++ = pgm_regs[i] + 1u;
        *dw++ = (uint32_t)(vs_va >> 40);
    }
    /* The measured counts: 0x4/0x3 for three parameters, 0x6/0x4 with
     * SPI_PS_INPUT_CNTL_3 0x3 for four. SPI_PS_INPUT_CNTL_3 is 0x194 (Mesa
     * src/amd/registers/gfx103.json:4203). */
    const uint32_t out_config = (params >= 4u) ? 0x6u : (params == 3u) ? 0x4u : 0x2u;
    /* PS_W32_EN rides along: the register is written whole (see GL_SPI_PS_W32_EN). */
    const uint32_t in_control = GL_SPI_PS_W32_EN | ((params >= 4u)   ? 0x4u
                                                    : (params == 3u) ? 0x3u
                                                                     : 0x2u);
    *dw++ = 0xc0016900u;
    *dw++ = 0x1b1u;
    *dw++ = out_config; /* VS_OUT_CONFIG */
    *dw++ = 0xc0016900u;
    *dw++ = 0x1b6u;
    *dw++ = in_control; /* PS_IN_CONTROL */
    *dw++ = 0xc0016900u;
    *dw++ = 0x193u;
    *dw++ = (params >= 3u) ? 0x2u : 0x0u; /* PS_INPUT_CNTL_2 */
    *dw++ = 0xc0016900u;
    *dw++ = 0x194u;
    *dw++ = (params >= 4u) ? 0x3u : 0x0u; /* PS_INPUT_CNTL_3 */
    ctx->hw_params = params;
    *dw_ptr = dw;
}

/* PACKET3_DMA_DATA: the command processor fills memory with a 32-bit pattern.
 * SRC_SEL=DATA takes the pattern from the packet, DST_SEL=TC_L2 writes through the
 * GPU's L2 (the end-of-pipe event writes it back before the CPU reads), CP_SYNC holds
 * the CP until the fill has landed so the draws that follow see it. Layout per the
 * public PM4 documentation and the open-source drivers. */
void gl_hw_emit_dma_fill(uint32_t **dw_ptr, uint64_t dst, uint32_t value,
                         uint32_t bytes) {
    uint32_t *dw = *dw_ptr;
    while (bytes > 0u) {
        uint32_t chunk = bytes > 0x3fffffcu ? 0x3fffffcu : bytes;
        *dw++ = 0xc0055000u; /* DMA_DATA, six body dwords */
        *dw++ = 0x80000000u | (2u << 29) |
                (3u << 20); /* CP_SYNC | SRC_SEL=DATA | DST_SEL=TC_L2, engine ME */
        *dw++ = value; /* the pattern (src address low when SRC_SEL is an address) */
        *dw++ = 0u;
        *dw++ = (uint32_t)dst;
        *dw++ = (uint32_t)(dst >> 32);
        *dw++ = chunk & 0x3ffffffu; /* BYTE_COUNT; source and destination in memory,
                                       both incrementing */
        dst += chunk;
        bytes -= chunk;
    }
    *dw_ptr = dw;
}

/* PACKET3_DMA_DATA memory to memory, both sides through L2. */
void gl_hw_emit_dma_copy(uint32_t **dw_ptr, uint64_t src, uint64_t dst,
                         uint32_t bytes) {
    uint32_t *dw = *dw_ptr;
    while (bytes > 0u) {
        uint32_t chunk = bytes > 0x3fffffcu ? 0x3fffffcu : bytes;
        *dw++ = 0xc0055000u;
        *dw++ =
            0x80000000u | (3u << 29) |
            (3u << 20); /* CP_SYNC | SRC_SEL=TC_L2 address | DST_SEL=TC_L2 address */
        *dw++ = (uint32_t)src;
        *dw++ = (uint32_t)(src >> 32);
        *dw++ = (uint32_t)dst;
        *dw++ = (uint32_t)(dst >> 32);
        *dw++ = chunk & 0x3ffffffu;
        src += chunk;
        dst += chunk;
        bytes -= chunk;
    }
    *dw_ptr = dw;
}

/* glClear on the hardware path: no CPU write touches the colour or depth surface. */
void gl_hw_clear(gl_context_t *ctx, GLbitfield mask, uint32_t colour, float depth) {
    if (!ctx || !ctx->use_hardware || ctx->hw_failed)
        return;
    if (!ctx->hw_frame_active) {
        gl_hw_begin_frame(ctx);
    }
    /* Up to four DMA fills of 7 dwords each - colour, the second colour buffer, depth
     * and stencil - and the flush trailer behind them. */
    if (ctx->dcb_words + 32u + OOPS_GL_DCB_TRAILER_DW >= ctx->dcb_capacity_dw) {
        gl_hw_flush(ctx);
        gl_hw_begin_frame(ctx);
    }
    uint32_t w = ctx->width ? ctx->width : 1920u;
    uint32_t h = ctx->height ? ctx->height : 1080u;
    uint32_t *dw = ctx->dcb_mem + ctx->dcb_words;
    if ((mask & GL_COLOR_BUFFER_BIT) && ctx->framebuffer) {
        /* The whole buffer - on the scanout path its padding to whole 64KB_R_X blocks
         * too, which a constant fills in any layout. */
        const uint32_t bytes =
            ctx->color_tiled ? (uint32_t)gl_color_words(ctx) * 4u : w * h * 4u;
        gl_hw_emit_dma_fill(&dw, (uint64_t)(uintptr_t)ctx->framebuffer, colour, bytes);
        /* A fill needs no colour target, so a clear reaches both buffers
         * GL_FRONT_AND_BACK names even here, where a draw reaches one. */
        if (ctx->fb_also) {
            gl_hw_emit_dma_fill(&dw, (uint64_t)(uintptr_t)ctx->fb_also, colour, bytes);
        }
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && ctx->depth_buffer) {
        uint32_t px = ctx->depth_px ? (uint32_t)ctx->depth_px : w * h;
        gl_hw_emit_dma_fill(&dw, (uint64_t)(uintptr_t)ctx->depth_buffer,
                            gl_f32_bits(depth), px * 4u);
    }
    /* The stencil surface is tiled, but a constant is the same in any layout, so a
     * whole clear is a fill of the byte four times over. The caller sends only
     * unscissored, unmasked stencil clears here. */
    if ((mask & GL_STENCIL_BUFFER_BIT) && ctx->stencil_buffer) {
        const uint32_t v = (uint32_t)(ctx->clear_stencil & 0xff) * 0x01010101u;
        uint32_t px = ctx->stencil_px ? (uint32_t)ctx->stencil_px : w * h;
        gl_hw_emit_dma_fill(&dw, (uint64_t)(uintptr_t)ctx->stencil_buffer, v,
                            (px + 3u) & ~3u);
    }
    ctx->dcb_words = (uint32_t)(dw - ctx->dcb_mem);
}

/* Unit length only when the program asks - GL_NORMALIZE, or GL 1.2's
 * GL_RESCALE_NORMAL, which undoes a uniform scale in the modelview by the length of the
 * inverse's third row (Mesa, main/light.c:1090-1103; nm is that inverse transposed, so
 * the row is nm[2], nm[5], nm[8]). A zero normal stays zero. Texture generation takes
 * the same normal, as Mesa's fixed-function program does. */
void gl_eye_normal(const gl_context_t *ctx, const float *nm, const float *n,
                   float out[3]) {
    float nx = nm[0] * n[0] + nm[1] * n[1] + nm[2] * n[2];
    float ny = nm[3] * n[0] + nm[4] * n[1] + nm[5] * n[2];
    float nz = nm[6] * n[0] + nm[7] * n[1] + nm[8] * n[2];
    if (ctx->cap_normalize) {
        const float nlen = gl_sqrt(nx * nx + ny * ny + nz * nz);
        if (nlen > 1e-12f) {
            const float inv_n = 1.0f / nlen;
            nx *= inv_n;
            ny *= inv_n;
            nz *= inv_n;
        }
    } else if (ctx->cap_rescale_normal) {
        const float f = nm[2] * nm[2] + nm[5] * nm[5] + nm[8] * nm[8];
        const float s = (f < 1e-12f) ? 1.0f : 1.0f / gl_sqrt(f);
        nx *= s;
        ny *= s;
        nz *= s;
    }
    out[0] = nx;
    out[1] = ny;
    out[2] = nz;
}

void gl_compute_lighting(gl_context_t *ctx, const float *obj_pos, const float *obj_norm,
                         const float *in_color, float *out_color) {
    float secondary[4];
    gl_compute_lighting2(ctx, obj_pos, obj_norm, in_color, out_color, secondary);
}

void gl_compute_lighting2(gl_context_t *ctx, const float *obj_pos,
                          const float *obj_norm, const float *in_color,
                          float *out_color, float *out_secondary) {
    gl_compute_lighting_side(ctx, obj_pos, obj_norm, in_color, out_color, out_secondary,
                             GL_FALSE);
}

void gl_compute_lighting_side(gl_context_t *ctx, const float *obj_pos,
                              const float *obj_norm, const float *in_color,
                              float *out_color, float *out_secondary, GLboolean back) {
    if (!ctx || !obj_pos || !obj_norm || !out_color || !out_secondary)
        return;

    /* 1. Transform vertex position to eye space: P_eye = ModelView * obj_pos */
    const gl_mat4_t *mv = &ctx->modelview_stack[ctx->modelview_depth];
    float peye[4];
    mat4_transform_vec4(peye, mv, obj_pos);
    float xe = peye[0];
    float ye = peye[1];
    float ze = peye[2];
    if (peye[3] != 0.0f && peye[3] != 1.0f) {
        float inv_w = 1.0f / peye[3];
        xe *= inv_w;
        ye *= inv_w;
        ze *= inv_w;
    }

    /* 2. Transform normal to eye space using inverse-transpose normal matrix */
    gl_update_normal_matrix(ctx);
    float en[3];
    gl_eye_normal(ctx, ctx->normal_matrix, obj_norm, en);
    float nx = en[0], ny = en[1], nz = en[2];
    /* The back of a two-sided polygon is lit as a surface facing the other way. */
    if (back) {
        nx = -nx;
        ny = -ny;
        nz = -nz;
    }

    /* 3. Material properties with GL_COLOR_MATERIAL tracking - the side's own material,
     * and the current colour tracked into it only if glColorMaterial named that side.
     */
    const gl_material_t *mat = back ? &ctx->mat_back : &ctx->mat_front;
    float mat_amb[4], mat_diff[4], mat_spec[4], mat_emis[4];
    memcpy(mat_amb, mat->ambient, 4 * sizeof(float));
    memcpy(mat_diff, mat->diffuse, 4 * sizeof(float));
    memcpy(mat_spec, mat->specular, 4 * sizeof(float));
    memcpy(mat_emis, mat->emission, 4 * sizeof(float));
    float shininess = mat->shininess;

    const GLboolean tracks =
        (GLboolean)(ctx->color_material_face == GL_FRONT_AND_BACK ||
                    ctx->color_material_face == (back ? GL_BACK : GL_FRONT));
    if (ctx->cap_color_material && in_color && tracks) {
        if (ctx->color_material_mode == GL_AMBIENT ||
            ctx->color_material_mode == GL_AMBIENT_AND_DIFFUSE) {
            memcpy(mat_amb, in_color, 4 * sizeof(float));
        }
        if (ctx->color_material_mode == GL_DIFFUSE ||
            ctx->color_material_mode == GL_AMBIENT_AND_DIFFUSE) {
            memcpy(mat_diff, in_color, 4 * sizeof(float));
        }
        if (ctx->color_material_mode == GL_SPECULAR) {
            memcpy(mat_spec, in_color, 4 * sizeof(float));
        }
        if (ctx->color_material_mode == GL_EMISSION) {
            memcpy(mat_emis, in_color, 4 * sizeof(float));
        }
    }

    /* 4. Base color: Emission + LightModelAmbient * MaterialAmbient */
    float r = mat_emis[0] + ctx->light_model_ambient[0] * mat_amb[0];
    float g = mat_emis[1] + ctx->light_model_ambient[1] * mat_amb[1];
    float b = mat_emis[2] + ctx->light_model_ambient[2] * mat_amb[2];
    float a = mat_diff[3];
    /* The specular term, summed apart so GL_SEPARATE_SPECULAR_COLOR can keep it apart.
     */
    float sr = 0.0f, sg = 0.0f, sb = 0.0f;

    /* 5. View direction vector V in eye space */
    float vx, vy, vz;
    if (ctx->light_model_local_viewer) {
        float vlen = gl_sqrt(xe * xe + ye * ye + ze * ze);
        if (vlen > 1e-6f) {
            float inv_v = 1.0f / vlen;
            vx = -xe * inv_v;
            vy = -ye * inv_v;
            vz = -ze * inv_v;
        } else {
            vx = 0.0f;
            vy = 0.0f;
            vz = 1.0f;
        }
    } else {
        vx = 0.0f;
        vy = 0.0f;
        vz = 1.0f;
    }

    /* 6. Accumulate contribution from enabled light sources */
    for (int i = 0; i < OOPS_GL_LIGHT_COUNT; i++) {
        if (!ctx->lights[i].enabled)
            continue;
        const gl_light_t *lt = &ctx->lights[i];

        float lx, ly, lz;
        float att = 1.0f;

        if (lt->position[3] == 0.0f) {
            /* Directional light: position vector already in eye space */
            float llen = gl_sqrt(lt->position[0] * lt->position[0] +
                                 lt->position[1] * lt->position[1] +
                                 lt->position[2] * lt->position[2]);
            if (llen > 1e-6f) {
                float inv_l = 1.0f / llen;
                lx = lt->position[0] * inv_l;
                ly = lt->position[1] * inv_l;
                lz = lt->position[2] * inv_l;
            } else {
                lx = 0.0f;
                ly = 0.0f;
                lz = 1.0f;
            }
        } else {
            /* Positional light: vector from vertex to light */
            float lpx = lt->position[0];
            float lpy = lt->position[1];
            float lpz = lt->position[2];
            if (lt->position[3] != 1.0f && lt->position[3] != 0.0f) {
                float inv_w = 1.0f / lt->position[3];
                lpx *= inv_w;
                lpy *= inv_w;
                lpz *= inv_w;
            }
            float dx = lpx - xe;
            float dy = lpy - ye;
            float dz = lpz - ze;
            float dist = gl_sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > 1e-6f) {
                float inv_d = 1.0f / dist;
                lx = dx * inv_d;
                ly = dy * inv_d;
                lz = dz * inv_d;
            } else {
                lx = 0.0f;
                ly = 0.0f;
                lz = 1.0f;
            }

            float denom =
                lt->const_att + lt->linear_att * dist + lt->quad_att * (dist * dist);
            att = (denom > 1e-6f) ? (1.0f / denom) : 1.0f;
        }

        /* Spotlight factor */
        float spot = 1.0f;
        if (lt->spot_cutoff < 180.0f) {
            float spot_dot = -(lx * lt->spot_direction[0] + ly * lt->spot_direction[1] +
                               lz * lt->spot_direction[2]);
            if (spot_dot >= lt->spot_cutoff_cos && spot_dot > 0.0f) {
                if (lt->spot_exponent > 0.0f) {
                    spot = gl_pow(spot_dot, lt->spot_exponent);
                }
            } else {
                spot = 0.0f;
            }
        }
        if (spot <= 0.0f)
            continue;

        float factor = att * spot;

        /* Ambient component */
        r += factor * (lt->ambient[0] * mat_amb[0]);
        g += factor * (lt->ambient[1] * mat_amb[1]);
        b += factor * (lt->ambient[2] * mat_amb[2]);

        /* Diffuse component */
        float ndotl = nx * lx + ny * ly + nz * lz;
        if (ndotl > 0.0f) {
            r += factor * (ndotl * lt->diffuse[0] * mat_diff[0]);
            g += factor * (ndotl * lt->diffuse[1] * mat_diff[1]);
            b += factor * (ndotl * lt->diffuse[2] * mat_diff[2]);

            /* Specular component (Blinn-Phong half-vector). A shininess of 0 (the
             * default) is (n.h)^0 = 1, full specular wherever n.h is positive. */
            {
                float hx = lx + vx;
                float hy = ly + vy;
                float hz = lz + vz;
                float hlen = gl_sqrt(hx * hx + hy * hy + hz * hz);
                if (hlen > 1e-6f) {
                    float inv_h = 1.0f / hlen;
                    hx *= inv_h;
                    hy *= inv_h;
                    hz *= inv_h;
                    float ndoth = nx * hx + ny * hy + nz * hz;
                    if (ndoth > 0.0f) {
                        float spec_pow = gl_pow(ndoth, shininess);
                        sr += factor * (spec_pow * lt->specular[0] * mat_spec[0]);
                        sg += factor * (spec_pow * lt->specular[1] * mat_spec[1]);
                        sb += factor * (spec_pow * lt->specular[2] * mat_spec[2]);
                    }
                }
            }
        }
    }

    /* 7. GL_SINGLE_COLOR sums the specular term into the colour;
     * GL_SEPARATE_SPECULAR_COLOR keeps it as the secondary colour, which the
     * fragment adds after texturing (GL 1.2, 2.13.1 and 3.9) - so a texture modulates
     * the diffuse light and not the highlight. Each clamped to [0, 1]. */
    if (ctx->light_model_color_control == GL_SEPARATE_SPECULAR_COLOR) {
        out_secondary[0] = (sr < 0.0f) ? 0.0f : ((sr > 1.0f) ? 1.0f : sr);
        out_secondary[1] = (sg < 0.0f) ? 0.0f : ((sg > 1.0f) ? 1.0f : sg);
        out_secondary[2] = (sb < 0.0f) ? 0.0f : ((sb > 1.0f) ? 1.0f : sb);
    } else {
        r += sr;
        g += sg;
        b += sb;
        out_secondary[0] = out_secondary[1] = out_secondary[2] = 0.0f;
    }
    out_secondary[3] = 0.0f;
    out_color[0] = (r < 0.0f) ? 0.0f : ((r > 1.0f) ? 1.0f : r);
    out_color[1] = (g < 0.0f) ? 0.0f : ((g > 1.0f) ? 1.0f : g);
    out_color[2] = (b < 0.0f) ? 0.0f : ((b > 1.0f) ? 1.0f : b);
    out_color[3] = (a < 0.0f) ? 0.0f : ((a > 1.0f) ? 1.0f : a);
}

void gl_draw_primitive_triangle(gl_context_t *ctx, const gl_vertex_t *v0,
                                const gl_vertex_t *v1, const gl_vertex_t *v2) {
    gl_draw_triangle_pv(ctx, v0, v1, v2, v2);
}

/* One triangle, whose flat-shaded colour is `pv`'s - which may be one of its own
 * vertices or not: a quad's first triangle takes the quad's fourth vertex, and an
 * expanded line's corners take the line's endpoint. See gl_assemble. */
static void gl_draw_triangle_pv_body(gl_context_t *ctx, const gl_vertex_t *v0,
                                     const gl_vertex_t *v1, const gl_vertex_t *v2,
                                     const gl_vertex_t *pv);
/* One triangle between the stages of gl_draw_triangle_pv_body: the input vertices, the
 * program, and per vertex what each stage computes - the object-space and clip
 * positions, the clip distances, the fog factor, the divided position, and the colours.
 * The arrays are gl_draw_triangle_pv_body's locals; each stage names the fields it uses
 * at its top. */
typedef struct {
    const gl_vertex_t *v0, *v1, *v2, *pv;
    gl_program_object_t *prog;
    GLboolean prog_vs;
    gl_shader_vertex_out_t *vso;
    const float *in0, *in1, *in2;
    float *c0, *c1, *c2;
    float *cd0, *cd1, *cd2;
    float fog0, fog1, fog2;
    float inv_w0, inv_w1, inv_w2;
    const float *ndc0, *ndc1, *ndc2;
    float *col0, *col1, *col2;
    float *sec0, *sec1, *sec2;
} gl_tri_t;
static GLboolean gl_tri_clip_fog(gl_context_t *ctx, gl_tri_t *tri);
static void gl_tri_colours(gl_context_t *ctx, const gl_tri_t *tri);
static void gl_tri_screen(gl_context_t *ctx, const gl_tri_t *tri,
                          gl_screen_vertex_t *out0, gl_screen_vertex_t *out1,
                          gl_screen_vertex_t *out2);
static void gl_draw_triangle_hw(gl_context_t *ctx, const gl_tri_t *tri);

/* The hardware path's state between its stages. Each stage copies the fields it reads
 * into locals of the same names, and stores what it decides for the stages after it. */
typedef struct {
    GLuint base_unit;
    GLuint eff_tex;
    gl_texture_object_t *eff_obj;
    /* The block a compiled pixel shader is handed: two texture units' descriptors and
     * the program's uniforms, built once and copied into the ring slot the same pass
     * chooses. */
    uint32_t gl2_block[OOPS_GL_GL2_SLOT_STRIDE / 4u];
    GLboolean gl2_block_used;
    GLboolean unit1_applied, volume, cube, depth_tex, shadow;
    gl_texture_object_t *unit1_obj;
    float poly_att[3][4];
    GLboolean poly_smooth;
    GLboolean p3, p3_needed, unit1;
    uint32_t params;
    size_t vsz;
    uint32_t tri_bytes;
    size_t vbo_offset;
    uint64_t desc_table_va, ps_va;
    uint32_t ps_rsrc2;
    uint32_t *dw;
    uint64_t vbo_t0, dcb_t0;
} gl_hw_draw_t;
static GLboolean gl_hw_tri_begin(gl_context_t *ctx, const gl_tri_t *tri,
                                 gl_hw_draw_t *hw);
static void gl_hw_tri_census(gl_context_t *ctx, GLuint base_unit, GLuint eff_tex,
                             gl_texture_object_t *eff_obj);
static void gl_hw_tri_sampling(gl_context_t *ctx, gl_hw_draw_t *hw);
static void gl_hw_tri_patch(gl_context_t *ctx, const gl_tri_t *tri, gl_hw_draw_t *hw);
static void gl_hw_tri_sample_slots(gl_context_t *ctx, gl_hw_draw_t *hw);
static void gl_hw_tri_desc_slot(gl_context_t *ctx, GLuint eff_tex,
                                gl_texture_object_t *eff_obj,
                                gl_texture_object_t *unit1_obj);
static void gl_hw_note_slot(gl_context_t *ctx, GLuint eff_tex,
                            const gl_texture_object_t *unit1_obj);
#ifndef OOPS_HOST_BUILD
static void gl_hw_trace_draw(gl_context_t *ctx, gl_texture_object_t *eff_obj);
#endif
static void gl_hw_tri_ring(gl_context_t *ctx, const gl_tri_t *tri, gl_hw_draw_t *hw);
static void gl_hw_tri_payload(gl_context_t *ctx, const gl_tri_t *tri, gl_hw_draw_t *hw);
static void gl_hw_tri_vertices(gl_context_t *ctx, const gl_tri_t *tri,
                               gl_hw_draw_t *hw);
static void gl_hw_tri_shader(gl_context_t *ctx, const gl_tri_t *tri, gl_hw_draw_t *hw);
static void gl_hw_tri_emit_state(gl_context_t *ctx, gl_hw_draw_t *hw);
static void gl_hw_tri_emit_dynamic(gl_context_t *ctx, gl_hw_draw_t *hw);
static void gl_hw_tri_emit_draw(gl_context_t *ctx, const gl_tri_t *tri,
                                gl_hw_draw_t *hw);

/* Times each triangle into `hw_draw_ns` and counts it in `hw_draw_calls`, so the
 * CPU time spent inside the draw path is separable from the program's own. */
static void gl_draw_triangle_pv(gl_context_t *ctx, const gl_vertex_t *v0,
                                const gl_vertex_t *v1, const gl_vertex_t *v2,
                                const gl_vertex_t *pv) {
#ifdef OOPS_HOST_BUILD
    gl_draw_triangle_pv_body(ctx, v0, v1, v2, pv);
#else
    const uint64_t t0 = oops_time_get_ns();
    gl_draw_triangle_pv_body(ctx, v0, v1, v2, pv);
    if (ctx) {
        ctx->hw_draw_ns += oops_time_get_ns() - t0;
        ctx->hw_draw_calls++;
    }
#endif
}

static void gl_draw_triangle_pv_body(gl_context_t *ctx, const gl_vertex_t *v0,
                                     const gl_vertex_t *v1, const gl_vertex_t *v2,
                                     const gl_vertex_t *pv) {
    if (!ctx || !v0 || !v1 || !v2)
        return;

    /* `hw_tnl_ns` times the transform, lighting, divide, viewport and cull in this
     * function; `hw_vbo_ns`, `hw_patch_ns` and `hw_dcb_ns` time the hardware stages. */
#ifndef OOPS_HOST_BUILD
    const uint64_t tnl_t0 = oops_time_get_ns();
#endif

    gl_update_mvp(ctx);

    /* A GL 2.0 vertex shader replaces the transform, lighting and texture coordinate
     * generation. Clipping, culling, polygon offset and rasterisation are common to
     * both. The shader runs per triangle per vertex, so a shared vertex is computed
     * more than once; this path is the reference the console path matches. */
    gl_program_object_t *prog = gl_active_program(ctx);
    const GLboolean prog_vs = (GLboolean)(prog != (gl_program_object_t *)0 && prog->vs);

    /* 1. Transform vertices to clip space */
    float c0[4], c1[4], c2[4];
    float in0[4] = {v0->x, v0->y, v0->z, v0->w};
    float in1[4] = {v1->x, v1->y, v1->z, v1->w};
    float in2[4] = {v2->x, v2->y, v2->z, v2->w};

    gl_shader_vertex_out_t vso[3];
    if (prog_vs) {
        const gl_vertex_t *vin[3] = {v0, v1, v2};
        for (int i = 0; i < 3; i++) {
            if (!gl_shader_run_vertex(ctx, prog, vin[i], &vso[i])) {
                /* Nothing is drawn, and the reason is on the log; the fixed-function
                 * transform is not a stand-in for the program's shader. */
                gl_record_error(ctx, GL_INVALID_OPERATION);
                return;
            }
        }
        for (int k = 0; k < 4; k++) {
            c0[k] = vso[0].position[k];
            c1[k] = vso[1].position[k];
            c2[k] = vso[2].position[k];
        }
    } else {
        mat4_transform_vec4(c0, &ctx->mvp, in0);
        mat4_transform_vec4(c1, &ctx->mvp, in1);
        mat4_transform_vec4(c2, &ctx->mvp, in2);
    }

    /* Simple near-plane guard: cull if completely behind camera */
    if (c0[3] <= 0.001f && c1[3] <= 0.001f && c2[3] <= 0.001f) {
        return;
    }
    /* If partially behind near plane, clamp w to prevent division by zero */
    float w0 = (c0[3] > 0.001f) ? c0[3] : 0.001f;
    float w1 = (c1[3] > 0.001f) ? c1[3] : 0.001f;
    float w2 = (c2[3] > 0.001f) ? c2[3] : 0.001f;

    /* 2. Perspective divide to NDC */
    float inv_w0 = 1.0f / w0;
    float inv_w1 = 1.0f / w1;
    float inv_w2 = 1.0f / w2;

    float ndc0[3] = {c0[0] * inv_w0, c0[1] * inv_w0, c0[2] * inv_w0};
    float ndc1[3] = {c1[0] * inv_w1, c1[1] * inv_w1, c1[2] * inv_w1};
    float ndc2[3] = {c2[0] * inv_w2, c2[1] * inv_w2, c2[2] * inv_w2};

    /* User clip distances, in eye space, one per plane per vertex, filled by
     * gl_tri_clip_fog. They are interpolated per fragment rather than the triangle
     * being clipped geometrically, which gives the same pixels without splitting it. A
     * disabled plane stays zero and is never read. */
    float cd0[OOPS_GL_CLIP_PLANE_COUNT] = {0};
    float cd1[OOPS_GL_CLIP_PLANE_COUNT] = {0};
    float cd2[OOPS_GL_CLIP_PLANE_COUNT] = {0};
    float col0[4], col1[4], col2[4], sec0[4], sec1[4], sec2[4];
    gl_tri_t tri = {.v0 = v0,
                    .v1 = v1,
                    .v2 = v2,
                    .pv = pv,
                    .prog = prog,
                    .prog_vs = prog_vs,
                    .vso = vso,
                    .in0 = in0,
                    .in1 = in1,
                    .in2 = in2,
                    .c0 = c0,
                    .c1 = c1,
                    .c2 = c2,
                    .cd0 = cd0,
                    .cd1 = cd1,
                    .cd2 = cd2,
                    .inv_w0 = inv_w0,
                    .inv_w1 = inv_w1,
                    .inv_w2 = inv_w2,
                    .ndc0 = ndc0,
                    .ndc1 = ndc1,
                    .ndc2 = ndc2,
                    .col0 = col0,
                    .col1 = col1,
                    .col2 = col2,
                    .sec0 = sec0,
                    .sec1 = sec1,
                    .sec2 = sec2};
    if (!gl_tri_clip_fog(ctx, &tri))
        return;
    gl_tri_colours(ctx, &tri);
    gl_screen_vertex_t sv0, sv1, sv2;
    gl_tri_screen(ctx, &tri, &sv0, &sv1, &sv2);

    /* 4. Backface culling via 2D signed area (screen coordinates) */
    float area =
        (sv1.sx - sv0.sx) * (sv2.sy - sv0.sy) - (sv1.sy - sv0.sy) * (sv2.sx - sv0.sx);

    /* glPolygonOffset for the software rasteriser (the hardware uses
     * `PA_SU_POLY_OFFSET_*`). The offset is `factor * m + units * r`, where `m` is the
     * triangle's largest depth slope and `r` the smallest resolvable depth difference;
     * both are constant across a triangle, so it is computed once. `m` is
     * `max(|dz/dx|, |dz/dy|)`, with the signed area as denominator. */
    float depth_bias = 0.0f;
    if (gl_prim_offsets(ctx) && area != 0.0f) {
        const float inv_area = 1.0f / area;
        const float dzdx = ((sv1.sz - sv0.sz) * (sv2.sy - sv0.sy) -
                            (sv2.sz - sv0.sz) * (sv1.sy - sv0.sy)) *
                           inv_area;
        const float dzdy = ((sv2.sz - sv0.sz) * (sv1.sx - sv0.sx) -
                            (sv1.sz - sv0.sz) * (sv2.sx - sv0.sx)) *
                           inv_area;
        float adx = dzdx < 0.0f ? -dzdx : dzdx;
        float ady = dzdy < 0.0f ? -dzdy : dzdy;
        const float slope = adx > ady ? adx : ady;
        /* `r` for a float depth buffer holding 0..1; the hardware derives its own from
         * the depth format. */
        const float resolvable = 1.0f / 16777216.0f;
        depth_bias =
            ctx->polygon_offset_factor * slope + ctx->polygon_offset_units * resolvable;
    }
    sv0.sz += depth_bias;
    sv1.sz += depth_bias;
    sv2.sz += depth_bias;

    /* Taken before the cull and degenerate returns, so every counted triangle
     * contributes its transform time. */
#ifndef OOPS_HOST_BUILD
    ctx->hw_tnl_ns += oops_time_get_ns() - tnl_t0;
#endif

    if (!ctx->use_hardware && gl_prim_culls(ctx)) {
        /* Note: with Y-flip, CCW in 3D becomes negative in screen space */
        GLboolean is_ccw = (area < 0.0f) ? GL_TRUE : GL_FALSE;
        if (ctx->front_face == GL_CW)
            is_ccw = !is_ccw;

        if (ctx->cull_mode == GL_BACK && !is_ccw)
            return;
        if (ctx->cull_mode == GL_FRONT && is_ccw)
            return;
        if (ctx->cull_mode == GL_FRONT_AND_BACK)
            return;
    }

    if (area > -1e-4f && area < 1e-4f)
        return; /* Degenerate */

    if (ctx->use_hardware) {
        gl_draw_triangle_hw(ctx, &tri);
        return;
    }

#ifdef OOPS_HOST_BUILD
    /* 6. Host builds have no GPU: the software rasterizer stands in for it there, and
     * only there. */
    gl_rasterize_triangle(ctx, &sv0, &sv1, &sv2);
    ctx->triangles_drawn++;
#else
    gl_hw_fail(ctx, "no hardware pipeline: nothing is drawn");
#endif
}

/* The clip distances from each vertex's `gl_ClipVertex`; GL_FALSE when all three are
 * outside one enabled plane. */
static GLboolean gl_tri_shader_clip(gl_context_t *ctx,
                                    const gl_shader_vertex_out_t *vso, float *cd0,
                                    float *cd1, float *cd2) {
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        if (!ctx->clip_plane_enabled[i])
            continue;
        const float *p = ctx->clip_plane[i];
        const gl_shader_vertex_out_t *s[3] = {&vso[0], &vso[1], &vso[2]};
        float *dst[3] = {&cd0[i], &cd1[i], &cd2[i]};
        for (int k = 0; k < 3; k++) {
            if (!s[k]->wrote_clip_vertex) {
                *dst[k] = 0.0f;
                continue;
            }
            const float *cv = s[k]->clip_vertex;
            *dst[k] = p[0] * cv[0] + p[1] * cv[1] + p[2] * cv[2] + p[3] * cv[3];
        }
    }
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        if (!ctx->clip_plane_enabled[i])
            continue;
        if (cd0[i] < 0.0f && cd1[i] < 0.0f && cd2[i] < 0.0f)
            return GL_FALSE;
    }
    return GL_TRUE;
}

/* The user clip distances and the fog factor of each vertex; GL_FALSE when all three
 * are outside one enabled plane. */
static GLboolean gl_tri_clip_fog(gl_context_t *ctx, gl_tri_t *tri) {
    const gl_vertex_t *const v0 = tri->v0, *const v1 = tri->v1, *const v2 = tri->v2;
    const GLboolean prog_vs = tri->prog_vs;
    const gl_shader_vertex_out_t *const vso = tri->vso;
    const float *const in0 = tri->in0, *const in1 = tri->in1, *const in2 = tri->in2;
    float *const cd0 = tri->cd0, *const cd1 = tri->cd1, *const cd2 = tri->cd2;
    /* Unfogged unless fog is on, so the rasteriser's fog blend is a no-op without a
     * branch per fragment. */
    float fog0 = 1.0f, fog1 = 1.0f, fog2 = 1.0f;
    GLboolean any_clip = GL_FALSE;
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        if (ctx->clip_plane_enabled[i]) {
            any_clip = GL_TRUE;
            break;
        }
    }
    if (prog_vs) {
        /* A vertex shader's fog coordinate is `gl_FogFragCoord`, whatever
         * GL_FOG_COORD_SRC says: only the fixed-function transform computes the
         * eye-space distance. Unwritten, it is zero, as `glFogCoord(0)`.
         *
         * User clipping uses `gl_ClipVertex`. A shader that never writes it is not
         * clipped: the distances stay zero, on the plane. The specification leaves this
         * undefined; not clipping loses no geometry. */
        if (ctx->cap_fog) {
            fog0 = gl_fog_factor(ctx, vso[0].vary[GL_SHADER_VARY_FOG]);
            fog1 = gl_fog_factor(ctx, vso[1].vary[GL_SHADER_VARY_FOG]);
            fog2 = gl_fog_factor(ctx, vso[2].vary[GL_SHADER_VARY_FOG]);
        }
        if (any_clip && !gl_tri_shader_clip(ctx, vso, cd0, cd1, cd2))
            return GL_FALSE;
    } else if (ctx->cap_fog && ctx->fog_coord_src == GL_FOG_COORD) {
        /* GL 1.4's fog coordinate in place of the distance, used as given (no absolute
         * value), as Mesa's fixed-function vertex program passes it through
         * (main/ffvertex_prog.c:1062-1064). */
        fog0 = gl_fog_factor(ctx, v0->fogc);
        fog1 = gl_fog_factor(ctx, v1->fogc);
        fog2 = gl_fog_factor(ctx, v2->fogc);
    } else if (ctx->cap_fog) {
        /* The eye-space distance to the vertex, as the specification says fog uses -
         * not the window depth, which would change with the depth range. */
        const gl_mat4_t *mv_fog = &ctx->modelview_stack[ctx->modelview_depth];
        float f0[4], f1[4], f2[4];
        mat4_transform_vec4(f0, mv_fog, in0);
        mat4_transform_vec4(f1, mv_fog, in1);
        mat4_transform_vec4(f2, mv_fog, in2);
        fog0 =
            gl_fog_factor(ctx, gl_sqrt(f0[0] * f0[0] + f0[1] * f0[1] + f0[2] * f0[2]));
        fog1 =
            gl_fog_factor(ctx, gl_sqrt(f1[0] * f1[0] + f1[1] * f1[1] + f1[2] * f1[2]));
        fog2 =
            gl_fog_factor(ctx, gl_sqrt(f2[0] * f2[0] + f2[1] * f2[1] + f2[2] * f2[2]));
    }
    if (any_clip && !prog_vs) {
        const gl_mat4_t *mv_clip = &ctx->modelview_stack[ctx->modelview_depth];
        float e0[4], e1[4], e2[4];
        mat4_transform_vec4(e0, mv_clip, in0);
        mat4_transform_vec4(e1, mv_clip, in1);
        mat4_transform_vec4(e2, mv_clip, in2);
        for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
            if (!ctx->clip_plane_enabled[i])
                continue;
            const float *p = ctx->clip_plane[i];
            cd0[i] = p[0] * e0[0] + p[1] * e0[1] + p[2] * e0[2] + p[3] * e0[3];
            cd1[i] = p[0] * e1[0] + p[1] * e1[1] + p[2] * e1[2] + p[3] * e1[3];
            cd2[i] = p[0] * e2[0] + p[1] * e2[1] + p[2] * e2[2] + p[3] * e2[3];
        }
        /* Wholly outside any single plane means nothing survives. */
        for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
            if (!ctx->clip_plane_enabled[i])
                continue;
            if (cd0[i] < 0.0f && cd1[i] < 0.0f && cd2[i] < 0.0f)
                return GL_FALSE;
        }
    }
    tri->fog0 = fog0;
    tri->fog1 = fog1;
    tri->fog2 = fog2;
    return GL_TRUE;
}

/* The colours of the three vertices: the vertex shader's, or clamped, lit and
 * flat-shaded. */
static void gl_tri_colours(gl_context_t *ctx, const gl_tri_t *tri) {
    const gl_vertex_t *const v0 = tri->v0, *const v1 = tri->v1, *const v2 = tri->v2;
    const gl_vertex_t *const pv = tri->pv;
    const GLboolean prog_vs = tri->prog_vs;
    const gl_shader_vertex_out_t *const vso = tri->vso;
    const float *const ndc0 = tri->ndc0, *const ndc1 = tri->ndc1,
                       *const ndc2 = tri->ndc2;
    /* Compute vertex colors (lighting or direct color) */
    float col0[4] = {v0->r, v0->g, v0->b, v0->a};
    float col1[4] = {v1->r, v1->g, v1->b, v1->a};
    float col2[4] = {v2->r, v2->g, v2->b, v2->a};
    /* The secondary colours: the vertices' own (glSecondaryColor, or its array) while
     * lighting is off, clamped with the primary below; lighting replaces them with the
     * specular term it keeps apart, or zero. Only read where gl_color_sum_on says the
     * sum runs. */
    float sec0[4] = {v0->sr, v0->sg, v0->sb, 0.0f};
    float sec1[4] = {v1->sr, v1->sg, v1->sb, 0.0f};
    float sec2[4] = {v2->sr, v2->sg, v2->sb, 0.0f};

    /* Which side two-sided lighting lights. A polygon's facing is its winding in
     * normalised device coordinates, the sign culling reads, taken here because
     * lighting needs it first. A polygon's outline or corners under glPolygonMode
     * take their polygon's side (gl_draw_polygon_tri sets it); a GL_LINES or GL_POINTS
     * primitive is always lit from the front, as GL lights everything but polygons. */
    GLboolean back = GL_FALSE;
    if (ctx->cap_lighting && ctx->light_model_two_side) {
        if (gl_prim_is_polygon(ctx)) {
            const float wind = (ndc1[0] - ndc0[0]) * (ndc2[1] - ndc0[1]) -
                               (ndc1[1] - ndc0[1]) * (ndc2[0] - ndc0[0]);
            const GLboolean ccw = (GLboolean)(wind > 0.0f);
            back = (ctx->front_face == GL_CW) ? ccw : (GLboolean)!ccw;
        } else if (ctx->prim_from_polygon) {
            back = ctx->prim_polygon_back;
        }
    }

    if (prog_vs) {
        /* The shader's `gl_FrontColor` and `gl_FrontSecondaryColor`, with neither
         * lighting nor flat shading applied: the vertex shader replaces both. A program
         * with no fragment shader reaches the fixed-function combiner through these. */
        const gl_shader_vertex_out_t *s[3] = {&vso[0], &vso[1], &vso[2]};
        float *dc[3] = {col0, col1, col2};
        float *ds[3] = {sec0, sec1, sec2};
        for (int k = 0; k < 3; k++) {
            for (int i = 0; i < 4; i++) {
                dc[k][i] = s[k]->vary[GL_SHADER_VARY_COLOR + i];
                ds[k][i] = s[k]->vary[GL_SHADER_VARY_SECONDARY + i];
            }
        }
    } else if (!ctx->cap_lighting) {
        /* Colours are clamped to [0, 1] before they are rasterised (GL 1.x, 2.14.9),
         * as lit ones already are. */
        for (int k = 0; k < 4; k++) {
            col0[k] = (col0[k] < 0.0f) ? 0.0f : ((col0[k] > 1.0f) ? 1.0f : col0[k]);
            col1[k] = (col1[k] < 0.0f) ? 0.0f : ((col1[k] > 1.0f) ? 1.0f : col1[k]);
            col2[k] = (col2[k] < 0.0f) ? 0.0f : ((col2[k] > 1.0f) ? 1.0f : col2[k]);
            sec0[k] = (sec0[k] < 0.0f) ? 0.0f : ((sec0[k] > 1.0f) ? 1.0f : sec0[k]);
            sec1[k] = (sec1[k] < 0.0f) ? 0.0f : ((sec1[k] > 1.0f) ? 1.0f : sec1[k]);
            sec2[k] = (sec2[k] < 0.0f) ? 0.0f : ((sec2[k] > 1.0f) ? 1.0f : sec2[k]);
        }
    }
    if (ctx->cap_lighting && !prog_vs) {
        float p0[4] = {v0->x, v0->y, v0->z, v0->w};
        float n0[3] = {v0->nx, v0->ny, v0->nz};
        float in_col0[4] = {v0->r, v0->g, v0->b, v0->a};
        gl_compute_lighting_side(ctx, p0, n0, in_col0, col0, sec0, back);

        float p1[4] = {v1->x, v1->y, v1->z, v1->w};
        float n1[3] = {v1->nx, v1->ny, v1->nz};
        float in_col1[4] = {v1->r, v1->g, v1->b, v1->a};
        gl_compute_lighting_side(ctx, p1, n1, in_col1, col1, sec1, back);

        float p2[4] = {v2->x, v2->y, v2->z, v2->w};
        float n2[3] = {v2->nx, v2->ny, v2->nz};
        float in_col2[4] = {v2->r, v2->g, v2->b, v2->a};
        gl_compute_lighting_side(ctx, p2, n2, in_col2, col2, sec2, back);
    }

    if (ctx->shade_model == GL_FLAT && !prog_vs) {
        /* The provoking vertex's colour, lit if lighting is on - computed from `pv`
         * itself when it is not one of the three, since its position and normal are
         * what lighting reads. Its secondary colour too: flat shading holds both. */
        float flat[4], flat_sec[4];
        if (!pv || pv == v2) {
            memcpy(flat, col2, sizeof(flat));
            memcpy(flat_sec, sec2, sizeof(flat_sec));
        } else if (pv == v0) {
            memcpy(flat, col0, sizeof(flat));
            memcpy(flat_sec, sec0, sizeof(flat_sec));
        } else if (pv == v1) {
            memcpy(flat, col1, sizeof(flat));
            memcpy(flat_sec, sec1, sizeof(flat_sec));
        } else {
            const float pc[4] = {pv->r, pv->g, pv->b, pv->a};
            const float ps[4] = {pv->sr, pv->sg, pv->sb, 0.0f};
            for (int k = 0; k < 4; k++) {
                flat[k] = (pc[k] < 0.0f) ? 0.0f : ((pc[k] > 1.0f) ? 1.0f : pc[k]);
                flat_sec[k] = (ps[k] < 0.0f) ? 0.0f : ((ps[k] > 1.0f) ? 1.0f : ps[k]);
            }
            if (ctx->cap_lighting) {
                const float pp[4] = {pv->x, pv->y, pv->z, pv->w};
                const float pn[3] = {pv->nx, pv->ny, pv->nz};
                gl_compute_lighting_side(ctx, pp, pn, pc, flat, flat_sec, back);
            }
        }
        memcpy(col0, flat, sizeof(flat));
        memcpy(col1, flat, sizeof(flat));
        memcpy(col2, flat, sizeof(flat));
        memcpy(sec0, flat_sec, sizeof(flat_sec));
        memcpy(sec1, flat_sec, sizeof(flat_sec));
        memcpy(sec2, flat_sec, sizeof(flat_sec));
    }
    memcpy(tri->col0, col0, sizeof(col0));
    memcpy(tri->col1, col1, sizeof(col1));
    memcpy(tri->col2, col2, sizeof(col2));
    memcpy(tri->sec0, sec0, sizeof(sec0));
    memcpy(tri->sec1, sec1, sizeof(sec1));
    memcpy(tri->sec2, sec2, sizeof(sec2));
}

/* The three screen vertices: the viewport and depth-range mapping of the divided
 * positions, with each vertex's colours, texture coordinates, clip distances, fog and,
 * under a vertex shader, what it interpolates. */
static void gl_tri_screen(gl_context_t *ctx, const gl_tri_t *tri,
                          gl_screen_vertex_t *out0, gl_screen_vertex_t *out1,
                          gl_screen_vertex_t *out2) {
    const gl_vertex_t *const v0 = tri->v0, *const v1 = tri->v1, *const v2 = tri->v2;
    const GLboolean prog_vs = tri->prog_vs;
    const gl_shader_vertex_out_t *const vso = tri->vso;
    const float *const cd0 = tri->cd0, *const cd1 = tri->cd1, *const cd2 = tri->cd2;
    const float fog0 = tri->fog0, fog1 = tri->fog1, fog2 = tri->fog2;
    const float inv_w0 = tri->inv_w0, inv_w1 = tri->inv_w1, inv_w2 = tri->inv_w2;
    const float *const ndc0 = tri->ndc0, *const ndc1 = tri->ndc1,
                       *const ndc2 = tri->ndc2;
    const float *const col0 = tri->col0, *const col1 = tri->col1,
                       *const col2 = tri->col2;
    const float *const sec0 = tri->sec0, *const sec1 = tri->sec1,
                       *const sec2 = tri->sec2;

    /* 3. Viewport mapping */
    float vp_w_half = (float)ctx->vp_w * 0.5f;
    float vp_h_half = (float)ctx->vp_h * 0.5f;
    float vp_ox = (float)ctx->vp_x + vp_w_half;
    float vp_oy = (float)ctx->vp_y + vp_h_half;

    /* glDepthRange: `z_window = ((far - near) / 2) * z_ndc + (far + near) / 2`, the
     * specification's formula and the one the hardware's 0x113/0x114 registers take. */
    const float dr_scale = (ctx->depth_far - ctx->depth_near) * 0.5f;
    const float dr_offset = (ctx->depth_far + ctx->depth_near) * 0.5f;

    gl_screen_vertex_t sv0, sv1, sv2;
    sv0.sx = ndc0[0] * vp_w_half + vp_ox;
    sv0.sy = (float)ctx->height -
             (ndc0[1] * vp_h_half + vp_oy); /* Y-flip for screen space */
    sv0.sz = ndc0[2] * dr_scale + dr_offset;
    sv0.inv_w = inv_w0;
    sv0.r = col0[0];
    sv0.g = col0[1];
    sv0.b = col0[2];
    sv0.a = col0[3];
    memcpy(sv0.tc, v0->tc, sizeof(sv0.tc));
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++)
        sv0.cd[i] = cd0[i];
    sv0.fog = fog0;
    sv0.sr = sec0[0];
    sv0.sg = sec0[1];
    sv0.sb = sec0[2];

    sv1.sx = ndc1[0] * vp_w_half + vp_ox;
    sv1.sy = (float)ctx->height - (ndc1[1] * vp_h_half + vp_oy);
    sv1.sz = ndc1[2] * dr_scale + dr_offset;
    sv1.inv_w = inv_w1;
    sv1.r = col1[0];
    sv1.g = col1[1];
    sv1.b = col1[2];
    sv1.a = col1[3];
    memcpy(sv1.tc, v1->tc, sizeof(sv1.tc));
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++)
        sv1.cd[i] = cd1[i];
    sv1.fog = fog1;
    sv1.sr = sec1[0];
    sv1.sg = sec1[1];
    sv1.sb = sec1[2];

    sv2.sx = ndc2[0] * vp_w_half + vp_ox;
    sv2.sy = (float)ctx->height - (ndc2[1] * vp_h_half + vp_oy);
    sv2.sz = ndc2[2] * dr_scale + dr_offset;
    sv2.inv_w = inv_w2;
    sv2.r = col2[0];
    sv2.g = col2[1];
    sv2.b = col2[2];
    sv2.a = col2[3];
    memcpy(sv2.tc, v2->tc, sizeof(sv2.tc));
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++)
        sv2.cd[i] = cd2[i];
    sv2.fog = fog2;
    sv2.sr = sec2[0];
    sv2.sg = sec2[1];
    sv2.sb = sec2[2];

    /* What the shader interpolates, and the texture coordinates it wrote, which a
     * program with no fragment shader passes to the fixed-function combiner. `vary` is
     * NULL without a program; the rasteriser tests that. */
    sv0.vary = sv1.vary = sv2.vary = (const float *)0;
    if (prog_vs) {
        sv0.vary = vso[0].vary;
        sv1.vary = vso[1].vary;
        sv2.vary = vso[2].vary;
        gl_screen_vertex_t *svp[3] = {&sv0, &sv1, &sv2};
        for (int k = 0; k < 3; k++) {
            for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
                const int base = GL_SHADER_VARY_TEXCOORD + (int)u * 4;
                for (int i = 0; i < 4; i++)
                    svp[k]->tc[u][i] = vso[k].vary[base + i];
            }
        }
    }
    *out0 = sv0;
    *out1 = sv1;
    *out2 = sv2;
}

/* One triangle through the hardware path (AMD RDNA2 GFX10.3): the pixel shader's slots
 * and the descriptors brought up to date, the vertices written into the ring, then the
 * command words. */
static void gl_draw_triangle_hw(gl_context_t *ctx, const gl_tri_t *tri) {
    gl_hw_draw_t hw_state;
    gl_hw_draw_t *const hw = &hw_state;
    if (!gl_hw_tri_begin(ctx, tri, hw))
        return;
    gl_hw_tri_census(ctx, hw->base_unit, hw->eff_tex, hw->eff_obj);
    gl_hw_tri_sampling(ctx, hw);
    gl_hw_tri_patch(ctx, tri, hw);
    gl_hw_tri_sample_slots(ctx, hw);
    if (hw->eff_obj && ctx->hw_frame_tex != 0u)
        gl_hw_tri_desc_slot(ctx, hw->eff_tex, hw->eff_obj, hw->unit1_obj);
    gl_hw_tri_ring(ctx, tri, hw);
    gl_hw_tri_payload(ctx, tri, hw);
    gl_hw_tri_vertices(ctx, tri, hw);
    gl_hw_tri_shader(ctx, tri, hw);
    gl_hw_tri_emit_state(ctx, hw);
    gl_hw_tri_emit_dynamic(ctx, hw);
    gl_hw_tri_emit_draw(ctx, tri, hw);
}

/* A program the back end generated for, a working pipeline, an open frame with room
 * in the stream, and the texture the sampling stage uses. GL_FALSE draws nothing. */
static GLboolean gl_hw_tri_begin(gl_context_t *ctx, const gl_tri_t *tri,
                                 gl_hw_draw_t *hw) {
    gl_program_object_t *const prog = tri->prog;
    /* A GL 2.0 program draws with its own compiled pixel shader, or not at all. The
     * fragment stage is compiled at link time (`glsl_ps.c`); a program it could not
     * generate for has `hw_ps_words` zero and is refused, rather than drawn with the
     * fixed-function shader. A program with no fragment stage uses the fixed-function
     * pixel shader, as the specification says. */
    if (prog != (gl_program_object_t *)0 && prog->fs && prog->hw_ps_words == 0u) {
        /* Logged once per program, so each refused program gives its reason. */
        if (!prog->hw_ps_logged) {
            prog->hw_ps_logged = GL_TRUE;
            oops_log_info("GL", "%s",
                          prog->hw_ps_log[0]
                              ? prog->hw_ps_log
                              : "this program's fragment shader has no console code");
        }
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return GL_FALSE;
    }
    /* The compiled shader says whether it wants the block: `hw_ps_user_sgprs` is
     * non-zero exactly when it takes the block's address. The same count decides where
     * the SPI puts the primitive mask the shader moves into `m0`. */
    const GLboolean gl2_block_used =
        (GLboolean)(prog != (gl_program_object_t *)0 && prog->fs &&
                    prog->hw_ps_words > 0u && prog->hw_ps_user_sgprs > 0u);
    if (ctx->hw_failed)
        return GL_FALSE; /* the failure is on the log and the status query */
    if (!ctx->hw_frame_active) {
        gl_hw_begin_frame(ctx);
    }

    /* Room for the most one draw can emit (OOPS_GL_DCB_DRAW_MAX_DW, which itemises
     * it) plus the trailer gl_hw_flush appends, so the stream can always be closed
     * inside the buffer. */
    if (ctx->dcb_words + OOPS_GL_DCB_DRAW_MAX_DW + OOPS_GL_DCB_TRAILER_DW >=
        ctx->dcb_capacity_dw) {
        gl_hw_flush(ctx);
        gl_hw_begin_frame(ctx);
    }

    /* Each draw points at its own slot in a ring of descriptor slots, because draws
     * are built before they run and a shared slot would be overwritten. A different
     * texture takes the next slot; the wrap submits, and so does a border colour,
     * which lives in a table a frame register names. See `OOPS_GL_DESC_RING_OFFSET`. */
    /* The unit the sampling stage draws from - the first with a texture, not
     * necessarily unit 0. Its coordinate set is the one the vertex carries
     * (gl_hw_tri_vertices). */
    const GLuint base_unit = gl_hw_base_unit(ctx);
    GLuint eff_tex = gl_unit_texture_id(ctx, base_unit);
    gl_texture_object_t *eff_obj =
        (gl_texture_object_t *)gl_lookup_texture(ctx, eff_tex);
    hw->base_unit = base_unit;
    hw->eff_tex = eff_tex;
    hw->eff_obj = eff_obj;
    hw->gl2_block_used = gl2_block_used;
    return GL_TRUE;
}

/* The per-draw texture census, and one log line for a unit whose texture cannot be
 * sampled. */
static void gl_hw_tri_census(gl_context_t *ctx, GLuint base_unit, GLuint eff_tex,
                             gl_texture_object_t *eff_obj) {
    /*
     * Texturing on, a texture bound, and GL says it cannot be sampled: GL draws
     * untextured without an error (3.8.10), which looks like textures that never
     * loaded. Logged once with the name and base level size, which separate the
     * default texture standing in for an unbound unit, an upload that stored nothing,
     * and an image the completeness rule rejects.
     */
    static GLboolean told_incomplete = GL_FALSE;
    /*
     * Read the unit's own enable and binding: `gl_unit_texture_id` and
     * `gl_effective_texture_id` both answer zero for "texturing off" and for "on but
     * unusable" alike.
     */
    const gl_tex_unit_t *u0 = &ctx->tex_unit[0];
    const GLboolean tex_on =
        (GLboolean)(u0->cap_texture_2d || u0->cap_texture_3d ||
                    u0->cap_texture_cube_map || u0->cap_texture_1d);
    const GLuint bound0 = u0->bound_texture_2d;
    /* The census this draw contributes to - see the counters' note in
     * gl_internal.h. Asked of every unit, not unit 0, because the textured unit is
     * often not unit 0 (`gl_hw_base_unit`). Split by `eff_tex`; a draw with texturing
     * off on every unit counts as neither. */
    GLboolean any_tex_on = GL_FALSE;
    for (GLuint u = 0u; u < OOPS_GL_MAX_TEXTURE_UNITS && !any_tex_on; u++) {
        const gl_tex_unit_t *tu = &ctx->tex_unit[u];
        if (tu->cap_texture_2d || tu->cap_texture_3d || tu->cap_texture_cube_map ||
            tu->cap_texture_1d) {
            any_tex_on = GL_TRUE;
        }
    }
    if (any_tex_on) {
        if (eff_tex != 0u)
            ctx->hw_draws_textured++;
        else
            ctx->hw_draws_untextured++;
    }
    /* Which texture: a scan and an increment per draw, printed once a flip (see
     * `hw_tex_census`). Unconditional, since it costs no syscall. */
    {
        /* Keyed on the pair of units, so a pass that varies only unit 1 is not one
         * row. See `id1`. */
        /* The condition `unit1_applied` states in gl_hw_tri_sampling: a second unit
         * exists only when unit 0 is the base and has something bound. */
        const uint32_t census_id1 =
            (ctx->hw_multitex && base_unit == 0u && eff_tex != 0u)
                ? gl_unit_texture_id(ctx, 1u)
                : 0u;
        uint32_t ci = 0u;
        for (; ci < ctx->hw_tex_census_n; ci++) {
            if (ctx->hw_tex_census[ci].id == eff_tex &&
                ctx->hw_tex_census[ci].id1 == census_id1)
                break;
        }
        if (ci == ctx->hw_tex_census_n && ci < OOPS_GL_TEX_CENSUS) {
            ctx->hw_tex_census_n++;
            ctx->hw_tex_census[ci].id = eff_tex;
            ctx->hw_tex_census[ci].id1 = census_id1;
            ctx->hw_tex_census[ci].draws = 0u;
        }
        if (ci < OOPS_GL_TEX_CENSUS) {
            ctx->hw_tex_census[ci].draws++;
            ctx->hw_tex_census[ci].w = (uint16_t)(eff_obj ? eff_obj->width : 0);
            ctx->hw_tex_census[ci].h = (uint16_t)(eff_obj ? eff_obj->height : 0);
            ctx->hw_tex_census[ci].slot = (uint8_t)ctx->hw_desc_slot;
            ctx->hw_tex_census[ci].unit = (uint8_t)base_unit;
            ctx->hw_tex_census[ci].env =
                (uint8_t)(uint32_t)ctx->tex_unit[base_unit].tex_env_mode;
            ctx->hw_tex_census[ci].blend = (uint8_t)(ctx->cap_blend ? 1u : 0u);
            ctx->hw_tex_census[ci].base = eff_obj ? eff_obj->base_format : 0u;
            ctx->hw_tex_census[ci].pitch = eff_obj ? eff_obj->pitch : 0u;
            /* The base level's own storage, to compare against the descriptor's
             * address: equal means the sampler reads the base level, different
             * means the mip chain. */
            ctx->hw_tex_census[ci].garlic =
                eff_obj ? (uint32_t)(eff_obj->garlic_va >> 8) : 0u;
            for (int dw_i = 0; dw_i < 8; dw_i++) {
                ctx->hw_tex_census[ci].desc[dw_i] =
                    eff_obj ? eff_obj->img_desc[dw_i] : 0u;
            }
            /* The sampler half - `s[12:15]` in `tex-prolog.s`, where the image
             * half is `s[4:11]`: wrap, filter and the LOD clamps. */
            for (int sw_i = 0; sw_i < 4; sw_i++) {
                ctx->hw_tex_census[ci].samp[sw_i] =
                    eff_obj ? eff_obj->samp_desc[sw_i] : 0u;
            }
        } else {
            ctx->hw_tex_census_over++;
        }
    }
    if (!told_incomplete && tex_on && eff_tex == 0u) {
        told_incomplete = GL_TRUE;
        const gl_texture_object_t *t0 =
            bound0 ? gl_lookup_texture(ctx, bound0)
                   : gl_lookup_texture(ctx, OOPS_GL_DEFAULT_TEXTURE_2D);
        gl_tex_view_t v;
        char msg[160];
        size_t n = 0;
        const char *head = "unusable texture on unit 0, drawn untextured: name ";
        while (head[n] && n < sizeof(msg) - 48) {
            msg[n] = head[n];
            n++;
        }
        n += obs_format_u64(msg + n, (uint64_t)bound0);
        if (t0 && gl_tex_level_view(t0, t0->base_level, &v)) {
            const char *mid = " base level ";
            for (size_t k = 0; mid[k]; k++)
                msg[n++] = mid[k];
            n += obs_format_u64(msg + n, (uint64_t)(uint32_t)v.width);
            msg[n++] = 'x';
            n += obs_format_u64(msg + n, (uint64_t)(uint32_t)v.height);
            const char *tail = " fmt ";
            for (size_t k = 0; tail[k]; k++)
                msg[n++] = tail[k];
            n += obs_format_u64(msg + n, (uint64_t)(uint32_t)v.internal_format);
        } else {
            const char *none = " has no base level image";
            for (size_t k = 0; none[k]; k++)
                msg[n++] = none[k];
        }
        msg[n] = '\0';
        oops_log_info("GL", "%s", msg);
    }
}

/* Which texture units and which sampling form this draw uses: a unit it cannot apply
 * is logged once, and an incomplete cube map draws untextured. */
static void gl_hw_tri_sampling(gl_context_t *ctx, gl_hw_draw_t *hw) {
    const GLuint base_unit = hw->base_unit;
    GLuint eff_tex = hw->eff_tex;
    gl_texture_object_t *eff_obj = hw->eff_obj;
    /*
     * Which units this path applies: the base unit, and unit 1 with unit 0
     * (gl_multitex.h). A bound unit it cannot apply - unit 1 when unit 0 has no
     * texture, since the second stage combines against the first - is logged once.
     */
    /* A second stage only when unit 0 is the base. When unit 1 is the base (unit 0
     * disabled) it is the single stage. */
    const GLboolean unit1_applied =
        (GLboolean)(ctx->hw_multitex && base_unit == 0u && eff_tex != 0u &&
                    gl_unit_texture_id(ctx, 1u) != 0u);
    for (GLuint tu = 1u; tu < OOPS_GL_MAX_TEXTURE_UNITS && !ctx->hw_unit_logged; tu++) {
        if (gl_unit_texture_id(ctx, tu) == 0u)
            continue;
        if (tu == base_unit)
            continue;
        if (tu == 1u && unit1_applied)
            continue;
        oops_log_info("GL", "a texture unit this path does not apply is bound: it is "
                            "applied by the "
                            "software rasteriser only and is left out of the draw");
        ctx->hw_unit_logged = GL_TRUE;
    }
    /* A draw under GL_FRONT_AND_BACK or GL_LEFT reaches both buffers: CB_COLOR1 is
     * bound to `fb_also`, both masks carry MRT1, and the pixel shader exports to it
     * (gl_ps_patch_export). */
    /* A volume's slices are laid out one after another by the upload; the descriptor
     * carries TYPE 0xa and the last slice in WORD4, and the sample slot interpolates
     * r, divides it by q and samples with `dim:SQ_RSRC_IMG_3D` (gl_ps_patch_sample).
     * r rides in the third parameter, so `volume` forces at least the
     * three-parameter vertex shader (gl_hw_tri_shader). */
    GLboolean volume = GL_FALSE;
    if (eff_obj && eff_obj->target == GL_TEXTURE_3D) {
        volume = GL_TRUE;
        /* A 3D mip chain is not built on this path: the level layout inside a 3D
         * image is not the 2D rule with a depth term (see `gl_tex_chain_levels`).
         * Logged once, only by a draw whose filter would use the chain. */
        if (!ctx->hw_3d_logged && gl_filter_uses_mipmaps(eff_obj->min_filter)) {
            oops_log_info(
                "GL",
                "a 3D texture's mip chain is not built on this path: minification "
                "samples the base level");
            ctx->hw_3d_logged = GL_TRUE;
        }
    }
    /* A cube map's six faces are uploaded as one array (gl_tex_cube_upload), the
     * descriptor carries TYPE 0xb, and the sample slot finds the face from the
     * direction (gl_ps_patch_sample). r rides in the third parameter, so `cube` forces
     * at least the three-parameter vertex shader. A cube map missing faces is
     * incomplete (2.1, 3.8.10) and drawn untextured. */
    GLboolean cube = GL_FALSE;
    if (eff_obj && eff_obj->target == GL_TEXTURE_CUBE_MAP) {
        if (eff_obj->cube_hw_dim > 0 || eff_obj->cube) {
            cube = GL_TRUE;
        } else {
            if (!ctx->hw_cube_logged) {
                oops_log_info("GL", "a cube map with no complete set of faces is not "
                                    "sampled on this "
                                    "path: the draw is untextured");
                ctx->hw_cube_logged = GL_TRUE;
            }
            eff_tex = 0u;
            eff_obj = (gl_texture_object_t *)0;
        }
    }
    /* A depth texture, with GL 1.4's comparison or without. The image format is
     * `32_FLOAT`, one float per texel; the sampler's DEPTH_COMPARE_FUNC carries
     * GL_TEXTURE_COMPARE_FUNC; the sample slot reads one channel and spreads it as
     * GL_DEPTH_TEXTURE_MODE says (gl_ps_patch_sample). Under GL_COMPARE_R_TO_TEXTURE
     * the reference is r, in the third parameter, so `shadow` forces one.
     *
     * The hardware compares per texel and then filters (percentage-closer); the
     * software rasteriser does the same (gl_depth_texel). */
    GLboolean depth_tex = GL_FALSE, shadow = GL_FALSE;
    if (eff_obj) {
        gl_tex_view_t dv;
        if (gl_tex_level_view(eff_obj, eff_obj->base_level, &dv) &&
            dv.base_format == GL_DEPTH_COMPONENT) {
            depth_tex = GL_TRUE;
            shadow = (GLboolean)(eff_obj->compare_mode == GL_COMPARE_R_TO_TEXTURE);
        }
    }
    hw->eff_tex = eff_tex;
    hw->eff_obj = eff_obj;
    hw->unit1_applied = unit1_applied;
    hw->volume = volume;
    hw->cube = cube;
    hw->depth_tex = depth_tex;
    hw->shadow = shadow;
}

/* The pixel shaders' slots this draw decides - the texture environment, fog, the
 * export, the stipple and the coverage - with the textures they sample made current. */
static void gl_hw_tri_patch(gl_context_t *ctx, const gl_tri_t *tri, gl_hw_draw_t *hw) {
    float *const c0 = tri->c0, *const c1 = tri->c1, *const c2 = tri->c2;
    const GLuint eff_tex = hw->eff_tex;
    gl_texture_object_t *const eff_obj = hw->eff_obj;
    const GLboolean unit1_applied = hw->unit1_applied;
    float (*const poly_att)[4] = hw->poly_att;
    /* The shader patches are timed into `hw_patch_ns`: each rebuilds its words and
     * compares them against the payload on every draw. */
#ifndef OOPS_HOST_BUILD
    const uint64_t patch_t0 = oops_time_get_ns();
#endif
    /* The second unit's texture is prepared here, beside unit 0's: the slot decision
     * in gl_hw_tri_sample_slots compares both units' descriptors, and preparing can
     * submit the frame, which must happen before a slot is chosen. */
    gl_texture_object_t *unit1_obj =
        unit1_applied ? gl_texture_slot(ctx, gl_unit_texture_id(ctx, 1u))
                      : (gl_texture_object_t *)0;
    if (eff_obj) {
        /* The texture's hardware image brought up to date - its mip chain built or
         * rebuilt, its descriptors repacked. Building may have submitted the frame
         * to free an old chain, in which case the frame is reopened here before
         * anything is written. */
        gl_tex_hw_prepare(ctx, eff_obj);
        if (!ctx->hw_frame_active)
            gl_hw_begin_frame(ctx);
        if (unit1_obj) {
            gl_tex_hw_prepare(ctx, unit1_obj);
            if (!ctx->hw_frame_active)
                gl_hw_begin_frame(ctx);
        }
        /* The combine follows the texture's base format as well as glTexEnv, so a
         * texture of another base format rewrites the shader's words, submitting the
         * draws built with the old ones first. GL_BLEND, GL_DECAL and GL_COMBINE are
         * programs in the longer slot; the log line is for one that does not fit,
         * which GL's argument counts rule out. */
        if (!gl_ps_patch_tex_env(ctx) && !ctx->hw_env_logged) {
            oops_log_info("GL", "a texture combine outgrew the pixel shader's slot: "
                                "this draw modulates on this path");
            ctx->hw_env_logged = GL_TRUE;
        }
        if (!ctx->hw_frame_active)
            gl_hw_begin_frame(ctx);
    }
    /* Fog, in both pixel shaders: the colour in the fog slot's literals, the factor
     * in the vertex (gl_hw_tri_vertices). A change submits the draws built with the
     * old words first. */
    gl_ps_patch_fog(ctx);
    if (!ctx->hw_frame_active)
        gl_hw_begin_frame(ctx);
    /* Every draw sets the export, to one target or two, so a draw after
     * glDrawBuffer(GL_BACK) stops writing the buffer GL no longer names. */
    gl_ps_patch_export(ctx, (GLboolean)(ctx->fb_also != (uint32_t *)0));
    if (!ctx->hw_frame_active)
        gl_hw_begin_frame(ctx);
    /* The polygon stipple: the discard slot and the mask it reads. Set on every draw,
     * since the stipple or glPolygonMode can change between two draws of a frame. */
    gl_ps_patch_stipple(ctx, gl_polygon_stipple_on(ctx));
    if (!ctx->hw_frame_active)
        gl_hw_begin_frame(ctx);
    /* Antialiasing's coverage, set on every draw so a triangle after a smooth point
     * stops weighing its alpha, and the slot follows the textured or untextured
     * shader. `gl_smoothing` has decided whether this draw smooths. */
    /* The polygon's widening is decided before the slot is patched, so a triangle the
     * widening refuses does not get a slot reading a fourth parameter it lacks. */
    const GLboolean poly_smooth =
        (GLboolean)(ctx->aa_edges != 0u &&
                    gl_hw_polygon_smooth_setup(ctx, ctx->aa_edges, c0, c1, c2,
                                               poly_att));
    gl_ps_patch_coverage_where(ctx, poly_smooth
                                        ? ((eff_tex != 0u) ? GL_COVERAGE_POLYGON_TEX
                                                           : GL_COVERAGE_POLYGON_UNTEX)
                                    : !ctx->aa_hw_on  ? GL_COVERAGE_OFF
                                    : (eff_tex != 0u) ? GL_COVERAGE_TEXTURED
                                                      : GL_COVERAGE_UNTEXTURED);
#ifndef OOPS_HOST_BUILD
    ctx->hw_patch_ns += oops_time_get_ns() - patch_t0;
    /* Everything from here to the command words is the descriptor slot decision and
     * the vertex ring - see `hw_vbo_ns`. */
    const uint64_t vbo_t0 = oops_time_get_ns();
    hw->vbo_t0 = vbo_t0;
#endif
    if (!ctx->hw_frame_active)
        gl_hw_begin_frame(ctx);
    hw->unit1_obj = unit1_obj;
    hw->poly_smooth = poly_smooth;
}

/* The sampling slots of a textured draw - the colour sum, the sample form and the
 * second unit - and whether its vertex needs a third parameter. */
static void gl_hw_tri_sample_slots(gl_context_t *ctx, gl_hw_draw_t *hw) {
    const GLuint eff_tex = hw->eff_tex;
    gl_texture_object_t *const eff_obj = hw->eff_obj;
    const GLboolean unit1_applied = hw->unit1_applied;
    const GLboolean volume = hw->volume, cube = hw->cube;
    const GLboolean depth_tex = hw->depth_tex, shadow = hw->shadow;
    /* The colour sum after texturing (GL 1.4, 3.9): the secondary colour rides in the
     * third parameter, and the textured shader's sum slot adds it after the combine
     * and before fog. A change of the slot submits the draws built with the old words
     * first. */
    GLboolean p3 = GL_FALSE;
    /* The third parameter a cube map's direction, a volume's r and a shadow
     * reference need, separate from the colour sum's use of the same export. */
    GLboolean p3_needed = GL_FALSE;
    /* The second texture unit (gated by gl_multitex.h): used when unit 1 has an
     * enabled texture of its own and unit 0 is textured too. It implies the third
     * parameter as well. */
    const GLboolean unit1 = unit1_applied;
    if (eff_tex != 0u) {
        /*
         * Whether the sum runs follows context state (`gl_color_sum_on`), not this
         * triangle's secondary colours: per-triangle switching rewrites the slot,
         * and every slot write flushes all shader ranges. A zero secondary colour
         * adds zero. The slot is set on every textured draw, on or off, so it never
         * adds whatever attr2 reads on a two-parameter draw.
         */
        p3 = gl_color_sum_on(ctx);
        /* A cube map's direction needs its third component, in the third
         * parameter, whether or not a colour sum wants it. gl_ps_patch_sum is told
         * `p3`, not this, so the sum stays off. */
        if (cube || volume || shadow)
            p3_needed = GL_TRUE;
        gl_ps_patch_sum(ctx, p3);
        if (!ctx->hw_frame_active)
            gl_hw_begin_frame(ctx);
        /* One of the five sampling forms, set on every textured draw so the slot
         * never keeps a previous draw's form. */
        {
            const gl_ps_sample_kind_t kind =
                shadow ? GL_PS_SAMPLE_SHADOW
                       : (depth_tex
                              ? GL_PS_SAMPLE_DEPTH
                              : (cube ? GL_PS_SAMPLE_CUBE
                                      : (volume ? GL_PS_SAMPLE_3D : GL_PS_SAMPLE_2D)));
            gl_ps_patch_sample(ctx, kind,
                               eff_obj ? eff_obj->depth_mode : (GLenum)GL_LUMINANCE);
        }
        if (!ctx->hw_frame_active)
            gl_hw_begin_frame(ctx);
        /* Both of the second unit's slots, set on every textured draw so a draw
         * back on one unit stops sampling and combining the second. */
        gl_ps_patch_unit1(ctx, unit1);
        if (!ctx->hw_frame_active)
            gl_hw_begin_frame(ctx);
        gl_ps_patch_tex_env_unit1(ctx, unit1);
        if (!ctx->hw_frame_active)
            gl_hw_begin_frame(ctx);
    }
    hw->p3 = p3;
    hw->p3_needed = p3_needed;
    hw->unit1 = unit1;
}

/* Chooses this draw's descriptor slot. Compared by content, not by texture name, so
 * a wrap or filter change mid-frame takes a new slot instead of rewriting one that
 * earlier draws will read at the flush. */
static void gl_hw_tri_desc_slot(gl_context_t *ctx, GLuint eff_tex,
                                gl_texture_object_t *eff_obj,
                                gl_texture_object_t *unit1_obj) {
    /* Compared against the CPU's shadow of the slot, not the slot: the payload is
     * uncached and flushed after every write, so reading it back costs a bus round
     * trip per draw. See `hw_desc_shadow`. */
    const uint32_t *slot = ctx->hw_desc_shadow;
    const void *border = (const char *)ctx->gpu_payload + OOPS_GL_BORDER_TABLE_OFFSET;
    uint32_t samp[4];
    memcpy(samp, eff_obj->samp_desc, 16);
    samp[2] |= gl_hw_lod_bias_bits(gl_tex_lod_bias(&ctx->tex_unit[0], eff_obj));
    GLboolean moved = (GLboolean)((ctx->hw_desc_shadow_valid & 1u) == 0u ||
                                  memcmp(slot, eff_obj->img_desc, 32) != 0 ||
                                  memcmp(slot + 8, samp, 16) != 0);
    /*
     * The second unit's half of the slot decides too. A slot is two descriptors,
     * `OOPS_GL_DESC_UNIT_STRIDE` apart; a pass that holds unit 0 constant and varies
     * unit 1 per surface must still advance the slot, or every draw in the submit
     * samples the last unit-1 descriptor written.
     */
    if (unit1_obj) {
        const uint32_t *slot1 = slot + OOPS_GL_DESC_UNIT_STRIDE / 4u;
        uint32_t samp1[4];
        memcpy(samp1, unit1_obj->samp_desc, 16);
        samp1[2] |= gl_hw_lod_bias_bits(gl_tex_lod_bias(&ctx->tex_unit[1], unit1_obj));
        if ((ctx->hw_desc_shadow_valid & 2u) == 0u ||
            memcmp(slot1, unit1_obj->img_desc, 32) != 0 ||
            memcmp(slot1 + 8, samp1, 16) != 0) {
            moved = GL_TRUE;
        }
    }
    /* A border colour change submits, whatever the ring has room for: its table is
     * named by `TA_BC_BASE_ADDR`, which the frame sets once. */
    const GLboolean border_moved =
        (GLboolean)(eff_obj->border_in_table &&
                    memcmp(border, eff_obj->border_hw, 16) != 0);
    if (moved && ctx->hw_desc_slot >= OOPS_GL_DESC_RING_SLOTS)
        ctx->hw_desc_ring_full++;
    if (border_moved || (moved && ctx->hw_desc_slot >= OOPS_GL_DESC_RING_SLOTS) ||
        gl_ps_ring_needs_submit(ctx)) {
        /* The ring is full, or the border changed, or the shader variant this
         * draw needs has nowhere to go: the frame runs, and the draws it holds
         * sample the descriptors - and bind the shader variants - they were
         * built with. */
        gl_hw_flush(ctx);
        gl_hw_begin_frame(ctx);
    } else if (moved) {
        /* The next slot, not a submission: the draws already built keep theirs. */
        ctx->hw_desc_slot++;
        /* The new slot holds an earlier frame's bytes, which the shadow does not
         * describe. Both halves go invalid and are set again as they are written, so
         * a draw using one unit leaves the other's bit clear. */
        ctx->hw_desc_shadow_valid = 0u;
    }
    gl_hw_note_slot(ctx, eff_tex, unit1_obj);
#ifndef OOPS_HOST_BUILD
    gl_hw_trace_draw(ctx, eff_obj);
#endif
}

/* Records which textures hold this draw's descriptor slot, and counts a slot that
 * changes texture within one submit. */
static void gl_hw_note_slot(gl_context_t *ctx, GLuint eff_tex,
                            const gl_texture_object_t *unit1_obj) {
    /* Whether this slot already belonged to another texture in this submit (see
     * `hw_slot_tex`). Recorded here, where the draw's slot is final, whether or not
     * the descriptor is rewritten. */
    const uint32_t s = ctx->hw_desc_slot;
    const uint32_t tex1 = unit1_obj ? unit1_obj->id : 0u;
    if (s > ctx->hw_desc_slot_high)
        ctx->hw_desc_slot_high = s;
    if (s > (uint32_t)OOPS_GL_DESC_RING_SLOTS)
        return;
    const uint32_t had = ctx->hw_slot_tex[s];
    const uint32_t had1 = ctx->hw_slot_tex1[s];
    /* Either half re-pointing is a collision; unit 0 alone can stay constant
     * across a pass. */
    const GLboolean hit = (GLboolean)((had != 0u && had != eff_tex) ||
                                      (had1 != 0u && tex1 != 0u && had1 != tex1));
    if (hit) {
        if (ctx->hw_slot_collisions == 0u) {
            ctx->hw_slot_first_slot = s;
            /* The half that moved, so the line names the two textures that
             * shared the slot. */
            if (had != 0u && had != eff_tex) {
                ctx->hw_slot_first_had = had;
                ctx->hw_slot_first_got = eff_tex;
            } else {
                ctx->hw_slot_first_had = had1;
                ctx->hw_slot_first_got = tex1;
            }
        }
        ctx->hw_slot_collisions++;
    }
    ctx->hw_slot_tex[s] = eff_tex;
    if (tex1 != 0u)
        ctx->hw_slot_tex1[s] = tex1;
}

#ifndef OOPS_HOST_BUILD
/* One trace line per draw of which texture it bound, into which slot, with the
 * environment, colour and blend that act on the texel. The first 512 draws from the
 * fourth frame on, after loading has settled. Only at OOPS_LOG_TRACE: each line is a
 * syscall. */
static void gl_hw_trace_draw(gl_context_t *ctx, gl_texture_object_t *eff_obj) {
    {
        static uint32_t drew;
        if (gl_log_level >= (int)OOPS_LOG_TRACE && ctx->frame_count >= 3u &&
            drew < 512u) {
            drew++;
            char m[200];
            size_t n = 0;
            /* The sampled unit's environment, not unit 0's: unit 0 may be
               disabled with the texture on unit 1. `gl_hw_base_unit` is what the
               shader patch asks. */
            const GLuint bu = gl_hw_base_unit(ctx);
            const char *lead = "draw tex/w/h/slot/unit/env/col/blend";
            while (lead[n] && n < 48u) {
                m[n] = lead[n];
                n++;
            }
            n = gl_msg_hex(m, sizeof(m), n, eff_obj->id);
            n = gl_msg_hex(m, sizeof(m), n, (uint32_t)eff_obj->width);
            n = gl_msg_hex(m, sizeof(m), n, (uint32_t)eff_obj->height);
            n = gl_msg_hex(m, sizeof(m), n, ctx->hw_desc_slot);
            n = gl_msg_hex(m, sizeof(m), n, bu);
            n = gl_msg_hex(m, sizeof(m), n, (uint32_t)ctx->tex_unit[bu].tex_env_mode);
            {
                /* The primary colour the combine will use, packed as eight bits
                   a channel so one word carries it. */
                uint32_t c = 0u;
                for (int k = 0; k < 4; k++) {
                    float v = ctx->cur_color[k];
                    if (v < 0.0f)
                        v = 0.0f;
                    if (v > 1.0f)
                        v = 1.0f;
                    c = (c << 8) | (uint32_t)(v * 255.0f);
                }
                n = gl_msg_hex(m, sizeof(m), n, c);
            }
            /* Whether the draw blends. */
            n = gl_msg_hex(m, sizeof(m), n, ctx->cap_blend ? 1u : 0u);
            m[n] = 0;
            oops_log_info("GL", "%s", m);

            /* Under GL_COMBINE, the RGB function, sources, operands and scale:
               what happens to the texel once fetched. */
            if (ctx->tex_unit[bu].tex_env_mode == GL_COMBINE) {
                const gl_combine_t *cb = &ctx->tex_unit[bu].combine;
                size_t k = 0;
                const char *cl = "draw comb rgb/src0-2/op0-2/scale";
                while (cl[k] && k < 40u) {
                    m[k] = cl[k];
                    k++;
                }
                k = gl_msg_hex(m, sizeof(m), k, (uint32_t)cb->mode_rgb);
                for (int s = 0; s < 3; s++) {
                    k = gl_msg_hex(m, sizeof(m), k, (uint32_t)cb->source_rgb[s]);
                }
                for (int s = 0; s < 3; s++) {
                    k = gl_msg_hex(m, sizeof(m), k, (uint32_t)cb->operand_rgb[s]);
                }
                k = gl_msg_hex(m, sizeof(m), k, (uint32_t)(cb->scale_rgb * 16.0f));
                m[k] = 0;
                oops_log_info("GL", "%s", m);
            }
        }
    }
}
#endif

/* The GL 2.0 uniform block's ring slot, and the vertex ring's room for this triangle.
 */
static void gl_hw_tri_ring(gl_context_t *ctx, const gl_tri_t *tri, gl_hw_draw_t *hw) {
    gl_program_object_t *const prog = tri->prog;
    const GLboolean prog_vs = tri->prog_vs;
    const GLuint eff_tex = hw->eff_tex;
    const GLboolean gl2_block_used = hw->gl2_block_used;
    uint32_t *const gl2_block = hw->gl2_block;
    const GLboolean poly_smooth = hw->poly_smooth;
    const GLboolean p3 = hw->p3, p3_needed = hw->p3_needed, unit1 = hw->unit1;
    /* The GL 2.0 uniform ring follows the descriptors' rule: draws already built read
     * their slot at the flush, so a draw whose uniforms differ from the current slot
     * takes the next one rather than overwrite it. The block is built once here and
     * copied where the shader's address is chosen (gl_hw_tri_shader). */
    if (gl2_block_used) {
        /* Any texture this program samples has to be uploaded and have its
         * descriptors built before they can be copied, and that can submit the
         * frame. */
        for (int s = 0; s < prog->hw_tex_sets; s++) {
            gl_texture_object_t *obj = gl_gl2_sampler_texture(ctx, prog, s);
            if (!obj)
                continue;
            gl_tex_hw_prepare(ctx, obj);
            if (!ctx->hw_frame_active)
                gl_hw_begin_frame(ctx);
        }
        if (!ctx->hw_frame_active)
            gl_hw_begin_frame(ctx);
        gl_gl2_build_block(ctx, prog, gl2_block);

        const void *slot =
            (const char *)ctx->gpu_payload + gl_hw_gl2_slot_offset(ctx->hw_gl2_slot);
        const GLboolean moved_block =
            (GLboolean)(ctx->hw_gl2_slot_program != 0u &&
                        (ctx->hw_gl2_slot_program != prog->name ||
                         memcmp(slot, gl2_block, OOPS_GL_GL2_SLOT_STRIDE) != 0));
        if (moved_block) {
            if (ctx->hw_gl2_slot + 1u >= OOPS_GL_GL2_SLOTS) {
                /* The ring is full: the frame runs, and the draws it holds read the
                 * values they were built with. `gl_hw_begin_frame` puts the slot
                 * back to 0. */
                gl_hw_flush(ctx);
                gl_hw_begin_frame(ctx);
            } else {
                ctx->hw_gl2_slot++;
            }
        }
        ctx->hw_gl2_slot_program = prog->name;
    }

    /* A full vertex ring is submitted before it is reused: the draws that read it
     * run only at the flush. The render target persists across submissions, so the
     * frame continues. The fill is per stream, in bytes (gl_hw_flush resets it). */
    /* 2, 3 or 4 parameters, and so 48, 64 or 80 bytes a vertex. The fourth shader
     * loads the third parameter too, so a two-unit vertex carries both. */
    /* A textured smooth primitive escalates to four: its coverage offset rides in the
     * second unit's parameter, which is spare because `gl_smoothing` refuses to smooth
     * when unit 1 is applied. */
    const GLboolean aa_p4 =
        (GLboolean)(ctx->aa_hw_on && ctx->aa_hw_tex && eff_tex != 0u);
    /* A smooth polygon escalates too: its edge distances and w fill the fourth
     * parameter. `poly_smooth` was decided with the coverage slot in gl_hw_tri_patch,
     * so the two agree. */
    /* A GL 2.0 program's parameter count is its varyings' (`hw_params`, from link
     * time, at least two): a compiled pixel shader reads no fixed-function colour or
     * texture coordinate. */
    const uint32_t params =
        (prog_vs && prog && prog->fs)
            ? prog->hw_params
            : ((unit1 || aa_p4 || poly_smooth) ? 4u : ((p3 || p3_needed) ? 3u : 2u));
    const size_t vsz = (params >= 4u) ? 80u : (params == 3u) ? 64u : 48u;
    const uint32_t tri_bytes = (uint32_t)(vsz * 3u);
    if (ctx->hw_vbo_cursor + tri_bytes > gl_vbo_ring_bytes) {
        gl_hw_flush(ctx);
        gl_hw_begin_frame(ctx);
    }
    hw->params = params;
    hw->vsz = vsz;
    hw->tri_bytes = tri_bytes;
}

/* A compiled pixel shader into the payload, this triangle's place in the vertex
 * ring, and an untextured draw's colour sum. */
static void gl_hw_tri_payload(gl_context_t *ctx, const gl_tri_t *tri,
                              gl_hw_draw_t *hw) {
    gl_program_object_t *const prog = tri->prog;
    float *const col0 = tri->col0, *const col1 = tri->col1, *const col2 = tri->col2;
    const float *const sec0 = tri->sec0, *const sec1 = tri->sec1,
                       *const sec2 = tri->sec2;
    const GLboolean p3 = hw->p3;
    const uint32_t tri_bytes = hw->tri_bytes;
    /* A GL 2.0 program's shader is copied into the payload before this triangle takes
     * its place in the vertex ring, because the copy can submit the frame
     * (`gl_ps_sync_payload_edit`), and a submit resets both the command stream and
     * `hw_vbo_cursor`. */
    if (prog != (gl_program_object_t *)0 && prog->fs && prog->hw_ps_words > 0u) {
        uint32_t *const ps_slot =
            (uint32_t *)((char *)ctx->gpu_payload + OOPS_GL_PS_GL2_OFFSET);
        /*
         * The compiled shader's export tail is swapped here, not by
         * `gl_ps_patch_export`, which patches only the fixed-function shaders at
         * their fixed offsets. So the target count joins the serial in deciding
         * whether the slot is stale.
         */
        const GLboolean both = (GLboolean)(ctx->fb_also != (uint32_t *)0);
        if (ctx->hw_ps_resident != prog->hw_ps_serial ||
            ctx->hw_ps_resident_both != both) {
            gl_ps_sync_payload_edit(ctx, ps_slot, prog->hw_ps, prog->hw_ps_words);
            memcpy(ps_slot, prog->hw_ps, prog->hw_ps_words * sizeof(uint32_t));
            /* `glsl_ps.c` ends every compiled shader with the one-target export
             * tail and two `s_nop`s of room, so the two-target form is the same
             * length and goes in over it. */
            memcpy(ps_slot + prog->hw_ps_words - GL_PS_EXPORT_WORDS,
                   gl_ps_export_words(both), GL_PS_EXPORT_WORDS * sizeof(uint32_t));
            /* Everything after the shader is left as it was; `s_endpgm` is the last
             * word it wrote, so nothing beyond it is reachable. */
            gl_ps_flush_shaders(ctx);
            ctx->hw_ps_resident = prog->hw_ps_serial;
            ctx->hw_ps_resident_both = both;
            if (!ctx->hw_frame_active)
                gl_hw_begin_frame(ctx);
        }
    }

    /* This triangle's place in the vertex buffer. */
    size_t vbo_offset = ctx->hw_vbo_cursor;
    ctx->hw_vbo_cursor += tri_bytes;

    /* Untextured, the secondary colour joins the primary per vertex, which is GL's
     * per-fragment sum except where it saturates between vertices. A textured draw
     * carries it in the third parameter for the pixel shader to add after the combine
     * (`p3`, gl_hw_tri_sample_slots). */
    if (gl_color_sum_on(ctx) && !p3) {
        for (int k = 0; k < 3; k++) {
            col0[k] = (col0[k] + sec0[k] > 1.0f) ? 1.0f : col0[k] + sec0[k];
            col1[k] = (col1[k] + sec1[k] > 1.0f) ? 1.0f : col1[k] + sec1[k];
            col2[k] = (col2[k] + sec2[k] > 1.0f) ? 1.0f : col2[k] + sec2[k];
        }
    }
    hw->vbo_offset = vbo_offset;
}

/* A compiled program's three vertices: the position, then the interpolated block four
 * floats a parameter, with `gl_FrontColor` and `gl_TexCoord[]` in the parameters the
 * linker set aside for them. */
static void gl_hw_write_program_vertices(char *vbo_ptr, size_t vsz, uint32_t params,
                                         const gl_program_object_t *prog,
                                         const gl_shader_vertex_out_t *vso,
                                         const float *const pos[3]) {
    for (size_t k = 0; k < 3; k++) {
        char *v = vbo_ptr + k * vsz;
        memcpy(v + 0, pos[k], 16);
        for (uint32_t pi = 0; pi < params; pi++) {
            float slot[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            /* `gl_FrontColor`, into the parameter the linker set aside for a
             * shader that names `gl_Color` (the first past the user's). */
            if (prog->hw_color_param >= 0 && pi == (uint32_t)prog->hw_color_param) {
                for (int c = 0; c < 4; c++) {
                    slot[c] = vso[k].vary[GL_SHADER_VARY_COLOR + c];
                }
            } else if (prog->hw_texcoord_param >= 0 &&
                       pi >= (uint32_t)prog->hw_texcoord_param &&
                       pi < (uint32_t)prog->hw_texcoord_param +
                                (uint32_t)OOPS_GL_MAX_TEXTURE_UNITS) {
                /* `gl_TexCoord[u]`, from its named place in the vertex stage's
                 * output rather than the user varying block. */
                const uint32_t u = pi - (uint32_t)prog->hw_texcoord_param;
                for (int c = 0; c < 4; c++) {
                    slot[c] = vso[k].vary[GL_SHADER_VARY_TEXCOORD + (int)(u * 4u) + c];
                }
            } else {
                for (int c = 0; c < 4; c++) {
                    const int at = (int)(pi * 4u) + c;
                    if (at < prog->varying_floats)
                        slot[c] = vso[k].vary[at];
                }
            }
            memcpy(v + 16 + pi * 16u, slot, 16);
        }
    }
}

/* The triangle's three vertices into its place in the vertex ring. */
static void gl_hw_tri_vertices(gl_context_t *ctx, const gl_tri_t *tri,
                               gl_hw_draw_t *hw) {
    const gl_vertex_t *const v0 = tri->v0, *const v1 = tri->v1, *const v2 = tri->v2;
    gl_program_object_t *const prog = tri->prog;
    const GLboolean prog_vs = tri->prog_vs;
    const gl_shader_vertex_out_t *const vso = tri->vso;
    const float *const c0 = tri->c0, *const c1 = tri->c1, *const c2 = tri->c2;
    const float *const col0 = tri->col0, *const col1 = tri->col1,
                       *const col2 = tri->col2;
    const float *const sec0 = tri->sec0, *const sec1 = tri->sec1,
                       *const sec2 = tri->sec2;
    const float fog0 = tri->fog0, fog1 = tri->fog1, fog2 = tri->fog2;
    const GLuint base_unit = hw->base_unit;
    const float (*const poly_att)[4] = hw->poly_att;
    const GLboolean poly_smooth = hw->poly_smooth;
    const uint32_t params = hw->params;
    const size_t vsz = hw->vsz;
    const uint32_t tri_bytes = hw->tri_bytes;
    const size_t vbo_offset = hw->vbo_offset;
    if (ctx->vbo_mem) {
        char *vbo_ptr = (char *)ctx->vbo_mem + vbo_offset;
        /* The texture parameter: s and t undivided and q in w; the textured pixel
         * shader divides per fragment. A q of 0 goes as 1, as the software
         * rasteriser's gl_q_inv does. z is the fog factor the fog slot interpolates
         * (gl_ps_patch_fog), 1 when fog is off. The coordinates are the base unit's,
         * the unit the stage samples. */
        const float *t0 = v0->tc[base_unit], *t1 = v1->tc[base_unit],
                    *t2 = v2->tc[base_unit];
        float uv0[4] = {t0[0], t0[1], fog0, (t0[3] != 0.0f) ? t0[3] : 1.0f};
        float uv1[4] = {t1[0], t1[1], fog1, (t1[3] != 0.0f) ? t1[3] : 1.0f};
        float uv2[4] = {t2[0], t2[1], fog2, (t2[3] != 0.0f) ? t2[3] : 1.0f};

        /* Each vertex: position, colour, the texture parameter - and with the third
         * parameter, {secondary r, g, b, unit 0's r}, the secondary colour for the
         * sum and r for what samples in three dimensions. */
        const float *pos[3] = {c0, c1, c2};
        const float *col[3] = {col0, col1, col2};
        const float *uvs[3] = {uv0, uv1, uv2};
        const float *sec[3] = {sec0, sec1, sec2};
        const float *tcs[3] = {t0, t1, t2};
        const float *u1[3] = {v0->tc[1], v1->tc[1], v2->tc[1]};
        /* A compiled program's vertex carries the position (`gl_Position`, from the
         * CPU-run vertex shader) and its varyings. A program with no fragment shader
         * takes the fixed-function arm below: `gl_FrontColor` and `gl_TexCoord[]`
         * already reached `col` and `uvs`. */
        if (prog_vs && prog && prog->fs) {
            gl_hw_write_program_vertices(vbo_ptr, vsz, params, prog, vso, pos);
            goto vertices_written;
        }
        for (size_t k = 0; k < 3; k++) {
            char *v = vbo_ptr + k * vsz;
            memcpy(v + 0, pos[k], 16);
            memcpy(v + 16, col[k], 16);
            memcpy(v + 32, uvs[k], 16);
            if (params >= 3u) {
                const float p2[4] = {sec[k][0], sec[k][1], sec[k][2], tcs[k][2]};
                memcpy(v + 48, p2, 16);
            }
            if (params >= 4u) {
                /* The second unit's coordinate, divided per fragment as the first
                 * is, with a q of 0 going as 1 - gl_q_inv's rule. z is unused: the
                 * fog factor is the first unit's to carry, and only one is
                 * interpolated. */
                /* A smooth polygon owns this parameter - `{d0*w, d1*w, d2*w, w}`
                 * from `gl_hw_polygon_smooth_setup` - so it is refused when the
                 * second unit wants the same four floats. */
                const float p3v[4] = {poly_smooth ? poly_att[k][0] : u1[k][0],
                                      poly_smooth ? poly_att[k][1] : u1[k][1],
                                      poly_smooth ? poly_att[k][2] : 0.0f,
                                      poly_smooth
                                          ? poly_att[k][3]
                                          : ((u1[k][3] != 0.0f) ? u1[k][3] : 1.0f)};
                memcpy(v + 64, p3v, 16);
            }
        }
        /* Both arms land here. The `(void)0` is what makes the label legal at the
         * end of a block on a target where the flush below is compiled out. */
    vertices_written:
        (void)0;
#if defined(__x86_64__)
        /* Every cache line the triangle touches: it starts 16-byte aligned, so its
         * last bytes can sit in a line of their own. */
        for (size_t p = 0; p < tri_bytes; p += 64u) {
            __builtin_ia32_clflush((const void *)(vbo_ptr + p));
        }
        __builtin_ia32_clflush((const void *)(vbo_ptr + tri_bytes - 1u));
#endif
    }
}

/* The pixel shader the draw binds and the table its user SGPRs point at: a compiled
 * program's with its uniform block, or the textured shader with this draw's
 * descriptors copied into its slot. */
static void gl_hw_tri_shader(gl_context_t *ctx, const gl_tri_t *tri, gl_hw_draw_t *hw) {
    gl_program_object_t *const prog = tri->prog;
    const GLuint eff_tex = hw->eff_tex;
    const GLboolean gl2_block_used = hw->gl2_block_used;
    const uint32_t *const gl2_block = hw->gl2_block;
    const GLboolean unit1 = hw->unit1;
    uint64_t payload_va = (uint64_t)(uintptr_t)ctx->gpu_payload;
    /* This draw's own descriptor slot; slot 0 is the original table. */
    uint64_t desc_table_va = payload_va + gl_hw_desc_slot_offset(ctx->hw_desc_slot);
    uint64_t ps_va =
        payload_va + OOPS_GL_PS_UNTEX_OFFSET; /* Default: untextured Gouraud */
    uint32_t ps_rsrc2 = 0u;

    /* A GL 2.0 program's own pixel shader, in the payload's one slot. RSRC1 is not
     * touched: the frame's stage table reserves the pixel stage's VGPRs and
     * `glsl_ps.c` refuses a shader that needs more. */
    if (prog != (gl_program_object_t *)0 && prog->fs && prog->hw_ps_words > 0u) {
        /* The copy happened in gl_hw_tri_payload, before the vertex ring; this
         * chooses the address and the block. */
        ps_va = payload_va + OOPS_GL_PS_GL2_OFFSET;
        ps_rsrc2 = 0u;

        /* The block, into the slot gl_hw_tri_ring chose, and flushed from the CPU's
         * cache like every payload edit. The uniform pool is `prog->values` verbatim,
         * the bytes `glGetUniformfv` reports. `ps_rsrc2` gains its user-SGPR pair only
         * for a shader that takes the block. */
        if (gl2_block_used) {
            const uint32_t boff = gl_hw_gl2_slot_offset(ctx->hw_gl2_slot);
            char *dst = (char *)ctx->gpu_payload + boff;
            memcpy(dst, gl2_block, OOPS_GL_GL2_SLOT_STRIDE);
#if defined(__x86_64__)
            for (uint32_t b = 0; b < OOPS_GL_GL2_SLOT_STRIDE; b += 64u) {
                __builtin_ia32_clflush((const void *)(dst + b));
            }
#endif
            desc_table_va = payload_va + boff;
            /* USER_SGPR is bits 5:1, and the count is the shader's own: s[0:1] is
             * the block's address and the primitive mask lands in s2, which is
             * where the prologue's `s_mov_b32 m0, s2` reads it from. */
            ps_rsrc2 = (prog->hw_ps_user_sgprs & 0x1fu) << 1u;
        }
    } else if (eff_tex > 0u) {
        /* Stage 5: Textured + Gouraud - the ring's copy holding this draw's patched
           words, not the master, which the GPU never reads. See
           OOPS_GL_PS_RING_OFFSET. */
        ps_va = payload_va + gl_ps_ring_offset(ctx);
        ps_rsrc2 = 0x00000004u; /* USER_SGPR=2 (bits 5:1): s[0:1] = descriptor
                                   table. 0x2 loads one SGPR and the primitive mask
                                   lands in s1 */
        /* Found by slot (`gl_texture_slot`), not by scanning the texture table, which
           is large enough to evict the rest of the draw's working set. */
        {
            gl_texture_object_t *const hit = gl_texture_slot(ctx, eff_tex);
            const int ti = hit ? (int)(hit - ctx->textures) : -1;
            if (hit) {
                uint32_t *dt = (uint32_t *)((char *)ctx->gpu_payload +
                                            gl_hw_desc_slot_offset(ctx->hw_desc_slot));
                memcpy(dt, ctx->textures[ti].img_desc, 32);
                memcpy(dt + 8, ctx->textures[ti].samp_desc, 16);
                /* GL 1.4's bias, the texture's and the unit's, joins the sampler
                 * here rather than in the texture's own descriptor, since half of
                 * it is context state. */
                dt[10] |= gl_hw_lod_bias_bits(
                    gl_tex_lod_bias(&ctx->tex_unit[0], &ctx->textures[ti]));
                /* And into the shadow, copied from `dt` so the two always agree;
                 * see `hw_desc_shadow`. */
                memcpy(ctx->hw_desc_shadow, dt, 48);
                ctx->hw_desc_shadow_valid |= 1u;
                ctx->hw_frame_tex = eff_tex;
                /* The border colour table's one entry, for a sampler whose WORD3
                 * names it. */
                float *bt =
                    (float *)((char *)ctx->gpu_payload + OOPS_GL_BORDER_TABLE_OFFSET);
                if (ctx->textures[ti].border_in_table) {
                    memcpy(bt, ctx->textures[ti].border_hw, 16);
                }
#if defined(__x86_64__)
                __builtin_ia32_clflush((const void *)dt);
                __builtin_ia32_clflush((const void *)bt);
#endif
            }
        }
        /* The second unit's pair, one stride along the table, where tex-prolog2.s
         * loads it. Written only when the draw uses the unit; otherwise the sample
         * that would read it branches over itself. */
        if (unit1) {
            const GLuint id1 = gl_unit_texture_id(ctx, 1u);
            gl_texture_object_t *const hit1 = gl_texture_slot(ctx, id1);
            const int ti = hit1 ? (int)(hit1 - ctx->textures) : -1;
            if (hit1) {
                gl_tex_hw_prepare(ctx, &ctx->textures[ti]);
                if (!ctx->hw_frame_active)
                    gl_hw_begin_frame(ctx);
                uint32_t *dt1 = (uint32_t *)((char *)ctx->gpu_payload +
                                             gl_hw_desc_slot_offset(ctx->hw_desc_slot) +
                                             OOPS_GL_DESC_UNIT_STRIDE);
                memcpy(dt1, ctx->textures[ti].img_desc, 32);
                memcpy(dt1 + 8, ctx->textures[ti].samp_desc, 16);
                dt1[10] |= gl_hw_lod_bias_bits(
                    gl_tex_lod_bias(&ctx->tex_unit[1], &ctx->textures[ti]));
                memcpy(ctx->hw_desc_shadow + OOPS_GL_DESC_UNIT_STRIDE / 4u, dt1, 48);
                ctx->hw_desc_shadow_valid |= 2u;
#if defined(__x86_64__)
                __builtin_ia32_clflush((const void *)dt1);
#endif
            }
        }
    }
    hw->desc_table_va = desc_table_va;
    hw->ps_va = ps_va;
    hw->ps_rsrc2 = ps_rsrc2;
}

/* The command stream's cursor, the depth and stencil surfaces a draw first needs, and
 * the four registers every draw sets. */
static void gl_hw_tri_emit_state(gl_context_t *ctx, gl_hw_draw_t *hw) {
#ifndef OOPS_HOST_BUILD
    const uint64_t vbo_t0 = hw->vbo_t0;
#endif
    /* Emit dynamic Depth Control, Blending, Cull Mode, and Color Target Mask state
     */
    /* The command stream's cursor is taken after every payload edit of the earlier
     * stages, because those can submit the frame (`gl_ps_sync_payload_edit`), which
     * resets `dcb_words`. */
    /* The command words are timed into `hw_dcb_ns`, apart from the descriptor and
     * vertex work before them (`hw_vbo_ns`). */
#ifndef OOPS_HOST_BUILD
    const uint64_t dcb_t0 = oops_time_get_ns();
    ctx->hw_vbo_ns += dcb_t0 - vbo_t0;
    hw->dcb_t0 = dcb_t0;
#endif
    uint32_t *dw = ctx->dcb_mem + ctx->dcb_words;

    uint32_t cur_depth_ctrl = gl_compute_db_depth_control(ctx);
    uint32_t cur_blend_ctrl = gl_compute_cb_blend_control(ctx);
    uint32_t cur_cull_ctrl = gl_compute_pa_su_sc_mode_cntl(ctx);
    uint32_t cur_target_mask = gl_compute_cb_target_mask(ctx);
    /* The same mask for the second target, when GL names both buffers - as the
     * frame's own CB_TARGET_MASK carries it. A mid-frame glColorMask must not drop
     * MRT1's half. */
    if (ctx->fb_also)
        cur_target_mask |= cur_target_mask << 4;

    /* The first depth-tested draw of a frame binds the depth surface. */
    if (cur_depth_ctrl != 0u && !ctx->hw_z_bound) {
        gl_hw_emit_depth_block(ctx, &dw);
        ctx->hw_z_bound = GL_TRUE;
    }
    /* An active occlusion query starts counting here, once the depth surface is bound
     * (counting without one stalls the depth block). The precision bits go on once a
     * frame, because a flush mid-query re-emits the depth block with the plain recipe;
     * the begin snapshot is taken once a query. */
    if (ctx->query_active != 0u && ctx->hw_z_bound && !ctx->hw_query_reg) {
        *dw++ = 0xc0016900u; /* SET_CONTEXT_REG DB_COUNT_CONTROL (0x001) */
        *dw++ = 0x001u;
        *dw++ = 0x11000106u; /* + PERFECT_ZPASS_COUNTS,
                                DISABLE_CONSERVATIVE_ZPASS_COUNTS */
        ctx->hw_query_reg = GL_TRUE;
        if (!ctx->hw_query_counting) {
            gl_hw_emit_zpass_done(&dw, gl_hw_zpass_base(ctx));
            ctx->hw_query_counting = GL_TRUE;
        }
    }
    /* The first stencil-tested draw of a frame binds the stencil surface; every
     * stencil-tested draw sets the operations and the reference. */
    if (gl_hw_stencil_on(ctx)) {
        if (!ctx->hw_stencil_bound) {
            gl_hw_emit_stencil_bind(ctx, &dw);
            ctx->hw_stencil_bound = GL_TRUE;
        }
        /* DB_STENCIL_CONTROL (0x10B), DB_STENCILREFMASK (0x10C),
         * DB_STENCILREFMASK_BF (0x10D)
         * - context registers 0x2842C, 0x28430, 0x28434 in gfx103.json -
         * consecutive, so one packet. The fields are gfx103.json's; STENCILOPVAL 1
         * is radeonsi's, the step the increment and decrement operations take
         * (si_state.c:1325-1333).
         *
         * The back face carries its own operations and reference
         * (`glStencilOpSeparate`, GL 2.0). `_BF` sits at 12, 16 and 20 in
         * `R_02842C`, the front's layout shifted by twelve. */
        const uint32_t ops = gl_hw_stencil_op(ctx->stencil_fail) |
                             (gl_hw_stencil_op(ctx->stencil_zpass) << 4) |
                             (gl_hw_stencil_op(ctx->stencil_zfail) << 8);
        const uint32_t ops_bf = gl_hw_stencil_op(ctx->stencil_back_fail) |
                                (gl_hw_stencil_op(ctx->stencil_back_zpass) << 4) |
                                (gl_hw_stencil_op(ctx->stencil_back_zfail) << 8);
        const uint32_t refmask = ((uint32_t)ctx->stencil_ref & 0xffu) |
                                 ((ctx->stencil_value_mask & 0xffu) << 8) |
                                 ((ctx->stencil_writemask & 0xffu) << 16) | (1u << 24);
        const uint32_t refmask_bf = ((uint32_t)ctx->stencil_back_ref & 0xffu) |
                                    ((ctx->stencil_back_value_mask & 0xffu) << 8) |
                                    ((ctx->stencil_back_writemask & 0xffu) << 16) |
                                    (1u << 24);
        *dw++ = 0xc0036900u; /* PACKET3_SET_CONTEXT_REG, three data dwords */
        *dw++ = 0x10bu;
        *dw++ = ops | (ops_bf << 12);
        *dw++ = refmask;
        *dw++ = refmask_bf;
    }

    *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG mmDB_DEPTH_CONTROL (0x200) */
    *dw++ = 0x200u;
    *dw++ = cur_depth_ctrl;

    /* Both targets' blend controls, in one packet, because 0x1e0 and 0x1e1 are
     * adjacent - the colour block keeps one per MRT and radeonsi writes them as
     * `R_028780_CB_BLEND0_CONTROL + i * 4` (`si_state.c:420`). MRT1's copy is the
     * same state, or zero when nothing is bound there; a mid-frame glBlendFunc must
     * not leave the second target on the frame's opening value. */
    *dw++ = 0xc0026900u; /* PACKET3_SET_CONTEXT_REG mmCB_BLEND0_CONTROL (0x1e0), two
                            dwords */
    *dw++ = 0x1e0u;
    *dw++ = cur_blend_ctrl;
    *dw++ = ctx->fb_also ? cur_blend_ctrl : 0u;

    *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG mmPA_SU_SC_MODE_CNTL (0x205) */
    *dw++ = 0x205u;
    *dw++ = cur_cull_ctrl;

    *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG mmCB_TARGET_MASK (0x08e) */
    *dw++ = 0x08eu;
    *dw++ = cur_target_mask;
    hw->dw = dw;
}

/* The registers a draw sets only when the state behind them changed since the frame
 * began: viewport, depth range, scissor, clip planes, logic op and blend constant. */
static void gl_hw_tri_emit_dynamic(gl_context_t *ctx, gl_hw_draw_t *hw) {
    uint32_t *dw = hw->dw;
    /* A viewport set after the frame's registers were written. Four consecutive
     * registers, so one packet. Nothing is emitted for a frame whose viewport was
     * already current when it began, which keeps the gl-cube oracle's stream. */
    if (ctx->hw_vport_dirty) {
        uint32_t vport[4];
        gl_compute_vport(ctx, ctx->height ? ctx->height : 1080u, vport);
        *dw++ = 0xc0046900u; /* PACKET3_SET_CONTEXT_REG, four data dwords */
        *dw++ = 0x10fu;      /* mmPA_CL_VPORT_XSCALE .. YOFFSET */
        *dw++ = vport[0];
        *dw++ = vport[1];
        *dw++ = vport[2];
        *dw++ = vport[3];
        ctx->hw_vport_dirty = GL_FALSE;
    }

    /* A depth range set after the frame's registers were written:
     * PA_CL_VPORT_ZSCALE and ZOFFSET, after the viewport's four (gfx103.json
     * 0x2844c, 0x28450), with the arithmetic of gl_hw_begin_frame's arms for 0x113
     * and 0x114. */
    if (ctx->hw_depth_range_dirty) {
        *dw++ = 0xc0026900u; /* PACKET3_SET_CONTEXT_REG, two data dwords */
        *dw++ = 0x113u;      /* mmPA_CL_VPORT_ZSCALE .. ZOFFSET */
        *dw++ = gl_f32_bits((ctx->depth_far - ctx->depth_near) * 0.5f);
        *dw++ = gl_f32_bits((ctx->depth_far + ctx->depth_near) * 0.5f);
        ctx->hw_depth_range_dirty = GL_FALSE;
    }

    /* A scissor box, or the test being switched, after the frame's registers were
     * written. Two consecutive registers, so one packet - and like the viewport,
     * nothing is emitted for a frame whose scissor was already current when it
     * began. */
    if (ctx->hw_scissor_dirty) {
        uint32_t sc[2];
        gl_compute_scissor(ctx, ctx->width ? ctx->width : 1920u,
                           ctx->height ? ctx->height : 1080u, sc);
        *dw++ = 0xc0026900u; /* PACKET3_SET_CONTEXT_REG, two data dwords */
        *dw++ = 0x094u;      /* mmPA_SC_VPORT_SCISSOR_0_TL .. _BR */
        *dw++ = sc[0];
        *dw++ = sc[1];
        ctx->hw_scissor_dirty = GL_FALSE;
    }

    /* A clip plane set, or enabled, after the frame's registers were written. Both
     * the equations and the enable bits move together - a plane written without its
     * enable does nothing, and an enable without its plane clips against whatever
     * was there before. */
    if (ctx->hw_clip_dirty) {
        gl_hw_emit_clip_planes(ctx, &dw);
        *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG mmPA_CL_CLIP_CNTL */
        *dw++ = 0x204u;
        *dw++ = gl_compute_clip_cntl(ctx);
        ctx->hw_clip_dirty = GL_FALSE;
    }

    /* A logic op switched, or its opcode changed, after the frame's registers were
     * written. */
    if (ctx->hw_color_control_dirty) {
        *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG mmCB_COLOR_CONTROL (0x202) */
        *dw++ = 0x202u;
        *dw++ = gl_compute_cb_color_control(ctx);
        ctx->hw_color_control_dirty = GL_FALSE;
    }

    /* The blend constant, only for a draw that reads it. CB_BLEND_RED, _GREEN,
     * _BLUE and _ALPHA are consecutive from context offset 0x105 (gfx103.json,
     * 0x28414..0x28420), and radeonsi writes them the same way - one sequence of
     * the four floats' bits (gallium/drivers/radeonsi/si_state.c:730-738). */
    if (ctx->hw_blend_color_dirty && gl_blend_reads_constant(ctx)) {
        /*
         * Green goes in the alpha slot when the colour constant is read: on this
         * part the green channel of a `BLEND_CONSTANT_COLOR` blend takes
         * `CB_BLEND_ALPHA` at `0x108` and ignores `CB_BLEND_GREEN` at `0x106`
         * (`docs/hardware/agc-blend-and-export-fw1240.md`).
         *
         * A draw that reads both the colour and the alpha constant needs two values
         * in `0x108`, so it records GL_INVALID_OPERATION rather than draw a silently
         * wrong channel (D009).
         */
        const GLboolean wants_color = gl_blend_reads_constant_color(ctx);
        const GLboolean wants_alpha = gl_blend_reads_constant_alpha(ctx);

        if (wants_color && wants_alpha) {
            gl_record_error(ctx, GL_INVALID_OPERATION);
        }

        *dw++ = 0xc0046900u; /* PACKET3_SET_CONTEXT_REG, four data dwords */
        *dw++ = 0x105u;      /* mmCB_BLEND_RED .. ALPHA */
        *dw++ = gl_f32_bits(ctx->blend_color[0]);
        *dw++ = gl_f32_bits(ctx->blend_color[1]);
        *dw++ = gl_f32_bits(ctx->blend_color[2]);
        /* The alpha slot carries green for a colour-constant draw, and the real
           alpha otherwise. When both were asked for, the error above has already
           been recorded and the colour constant is the one served. */
        *dw++ = gl_f32_bits(wants_color ? ctx->blend_color[1] : ctx->blend_color[3]);
        ctx->hw_blend_color_dirty = GL_FALSE;
    }
    hw->dw = dw;
}

/* The vertex stage's parameter count, the pixel shader and its inputs, the user data,
 * and the draw. */
static void gl_hw_tri_emit_draw(gl_context_t *ctx, const gl_tri_t *tri,
                                gl_hw_draw_t *hw) {
    gl_program_object_t *const prog = tri->prog;
    uint32_t *dw = hw->dw;
    const uint32_t params = hw->params;
    const uint64_t ps_va = hw->ps_va, desc_table_va = hw->desc_table_va;
    const uint32_t ps_rsrc2 = hw->ps_rsrc2;
    const size_t vbo_offset = hw->vbo_offset;
#ifndef OOPS_HOST_BUILD
    const uint64_t dcb_t0 = hw->dcb_t0;
#endif
    /* The vertex stage's parameter count, switched only on a change. */
    gl_hw_emit_param_count(ctx, &dw, params);

    /* Emit Shader & User Data */
    *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_PGM_LO_PS */
    *dw++ = 0x08u;
    *dw++ = (uint32_t)(ps_va >> 8);
    *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_PGM_HI_PS */
    *dw++ = 0x09u;
    *dw++ = (uint32_t)(ps_va >> 40);
    *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_PGM_RSRC2_PS */
    *dw++ = 0x0bu;
    *dw++ = ps_rsrc2;

    /* What the SPI hands the pixel stage: `SPI_PS_INPUT_ENA` and `_ADDR` (context
     * 0x1b3 and 0x1b4). The barycentrics always; a shader reading `gl_FragCoord`
     * also asks for the window position, which the SPI puts in v2..v5. Emitted only
     * on a change; `gl_hw_begin_frame` resets `hw_input_ena` to the frame table's
     * value. */
    {
        const uint32_t want =
            (prog != (gl_program_object_t *)0 && prog->fs && prog->hw_ps_words > 0u)
                ? prog->hw_ps_input_ena
                : (gl_polygon_stipple_on(ctx) ? 0x00000302u : 0x00000002u);
        if (want != ctx->hw_input_ena) {
            *dw++ = 0xc0016900u;
            *dw++ = 0x1b3u;
            *dw++ = want;
            *dw++ = 0xc0016900u;
            *dw++ = 0x1b4u;
            *dw++ = want;
            ctx->hw_input_ena = want;
        }
    }

    /* A shader that writes its own depth changes two registers.
     * `SPI_SHADER_Z_FORMAT` (0x1c4) says a Z is coming (`SPI_SHADER_32_R`), and
     * `DB_SHADER_CONTROL` (0x203) moves from `EARLY_Z_THEN_LATE_Z` to `LATE_Z` with
     * `Z_EXPORT_ENABLE`, since the depth is not known until the shader has run (bits
     * from `R_02880C` and `R_028710` for gfx103). Emitted only on a change;
     * `gl_hw_begin_frame` resets both to the frame table's values. */
    {
        const GLboolean compiled_ps = (GLboolean)(prog != (gl_program_object_t *)0 &&
                                                  prog->fs && prog->hw_ps_words > 0u);
        const GLboolean depth_ps =
            (GLboolean)(compiled_ps && prog->hw_ps_exports_depth);
        /* `KILL_ENABLE` for a shader that discards: clearing `exec` stops the
         * shader writing but not the depth block, which under early Z would already
         * have written the discarded fragment's depth. From `uses_discard` in
         * radeonsi (`si_state_shaders.cpp:1711`); `Z_ORDER` stays
         * `EARLY_Z_THEN_LATE_Z` (case 1 of the table at `:1730`). */
        /* The fixed-function alpha test and polygon stipple kill too. `GL_ALWAYS`
         * keeps everything, so it is not a kill. */
        const GLboolean kill_ff =
            (GLboolean)((ctx->cap_alpha_test && ctx->alpha_func != GL_ALWAYS) ||
                        gl_polygon_stipple_on(ctx));
        const GLboolean kill_ps =
            (GLboolean)(compiled_ps ? prog->hw_ps_kills : kill_ff);
        const uint32_t want_zfmt = depth_ps ? 1u : 0u;
        const uint32_t want_dbsc =
            (depth_ps ? 0x00000001u : 0x00000010u) | (kill_ps ? 0x00000040u : 0u);
        if (want_zfmt != ctx->hw_z_format) {
            *dw++ = 0xc0016900u;
            *dw++ = 0x1c4u;
            *dw++ = want_zfmt;
            ctx->hw_z_format = want_zfmt;
        }
        /* Its own comparison, not the format's: a program that discards without
         * writing depth moves this register alone. */
        if (want_dbsc != ctx->hw_db_shader_control) {
            *dw++ = 0xc0016900u;
            *dw++ = 0x203u;
            *dw++ = want_dbsc;
            ctx->hw_db_shader_control = want_dbsc;
        }
    }

    /* Pass Descriptor Table VA to PS User SGPRs 0 and 1
     * (mmSPI_SHADER_USER_DATA_PS_0 = 0x0c, 0x0d) */
    *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_USER_DATA_PS_0 */
    *dw++ = 0x0cu;
    *dw++ = (uint32_t)desc_table_va;
    *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_USER_DATA_PS_1 */
    *dw++ = 0x0du;
    *dw++ = (uint32_t)(desc_table_va >> 32);

    /* Pass VBO byte offset into GS User SGPR 0 (mmSPI_SHADER_USER_DATA_GS_0 = 0x8c)
     */
    *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_USER_DATA_GS_0 */
    *dw++ = 0x8cu;
    *dw++ = (uint32_t)vbo_offset;
    *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_USER_DATA_VS_0 (legacy
                            VS stage) */
    *dw++ = 0x4cu;
    *dw++ = (uint32_t)vbo_offset;

    /* Dispatch Hardware Draw */
    *dw++ = 0xc0002f00u; /* PACKET3_NUM_INSTANCES */
    *dw++ = 1u;
    *dw++ = 0xc0012d00u; /* DRAW_INDEX_AUTO */
    *dw++ = 3u;
    *dw++ = 2u;

    ctx->dcb_words = (uint32_t)(dw - ctx->dcb_mem);
    ctx->triangles_drawn++;
#ifndef OOPS_HOST_BUILD
    ctx->hw_dcb_ns += oops_time_get_ns() - dcb_t0;
#endif
}

/* One stencil operation, through the write mask. The mask applies to the result, not
 * the operand: GL_INVERT under 0x0f inverts all eight bits and writes back the low
 * four. GL_INCR and GL_DECR saturate; GL 1.4's GL_INCR_WRAP and GL_DECR_WRAP wrap.
 *
 * This, the blend factors, the sampler and the texture environment are compiled for the
 * target too: pixel rectangles are CPU fragments on both paths (gl_pixel_fragment).
 * Only the triangle rasteriser is host-only. */
static void gl_stencil_apply(uint8_t *sp, GLenum op, GLint ref, uint32_t wmask) {
    const uint8_t old = *sp;
    uint8_t next = old;
    switch (op) {
    case GL_KEEP:
        return;
    case GL_ZERO:
        next = 0u;
        break;
    case GL_REPLACE:
        next = (uint8_t)(ref & 0xff);
        break;
    case GL_INCR:
        next = (uint8_t)((old < 255u) ? (old + 1u) : 255u);
        break;
    case GL_DECR:
        next = (uint8_t)((old > 0u) ? (old - 1u) : 0u);
        break;
    case GL_INCR_WRAP:
        next = (uint8_t)(old + 1u);
        break;
    case GL_DECR_WRAP:
        next = (uint8_t)(old - 1u);
        break;
    case GL_INVERT:
        next = (uint8_t)(~old);
        break;
    default:
        return;
    }
    const uint8_t m = (uint8_t)(wmask & 0xffu);
    *sp = (uint8_t)((next & m) | (old & (uint8_t)~m));
}

static float get_blend_factor(GLenum factor, const float bc[4], float src_r,
                              float src_g, float src_b, float src_a, float dst_r,
                              float dst_g, float dst_b, float dst_a, int channel) {
    switch (factor) {
    /* The constant colour's alpha is what CONSTANT_COLOR contributes to the alpha
     * channel, the same as the other *_COLOR factors take their own alpha there. */
    case GL_CONSTANT_COLOR:
        return bc[channel];
    case GL_ONE_MINUS_CONSTANT_COLOR:
        return 1.0f - bc[channel];
    case GL_CONSTANT_ALPHA:
        return bc[3];
    case GL_ONE_MINUS_CONSTANT_ALPHA:
        return 1.0f - bc[3];
    case GL_ZERO:
        return 0.0f;
    case GL_ONE:
        return 1.0f;
    case GL_SRC_COLOR:
        return (channel == 0)
                   ? src_r
                   : ((channel == 1) ? src_g : ((channel == 2) ? src_b : src_a));
    case GL_ONE_MINUS_SRC_COLOR:
        return 1.0f -
               ((channel == 0)
                    ? src_r
                    : ((channel == 1) ? src_g : ((channel == 2) ? src_b : src_a)));
    case GL_SRC_ALPHA:
        return src_a;
    case GL_ONE_MINUS_SRC_ALPHA:
        return 1.0f - src_a;
    case GL_DST_ALPHA:
        return dst_a;
    case GL_ONE_MINUS_DST_ALPHA:
        return 1.0f - dst_a;
    case GL_DST_COLOR:
        return (channel == 0)
                   ? dst_r
                   : ((channel == 1) ? dst_g : ((channel == 2) ? dst_b : dst_a));
    case GL_ONE_MINUS_DST_COLOR:
        return 1.0f -
               ((channel == 0)
                    ? dst_r
                    : ((channel == 1) ? dst_g : ((channel == 2) ? dst_b : dst_a)));
    case GL_SRC_ALPHA_SATURATE: {
        /* min(As, 1 - Ad) for colour, but 1 for alpha, as softpipe has it
         * (gallium/drivers/softpipe/sp_quad_blend.c:467-470) and the colour block does
         * for BLEND_SRC_ALPHA_SATURATE. */
        if (channel == 3)
            return 1.0f;
        float f = 1.0f - dst_a;
        return (src_a < f) ? src_a : f;
    }
    default:
        return 1.0f;
    }
}

/* -------------------------------------------------------------------------
 * The software sampler
 *
 * The specification's texturing, as the reference the host tests draw against: nearest
 * and linear filtering, minification and magnification chosen by the level of detail,
 * and mipmap selection among the levels a complete texture has. Nearest is
 * `floor(u * width)`.
 * ------------------------------------------------------------------------- */

static float gl_floorf(float x) {
    const float t = (float)(int)x;
    return (t > x) ? t - 1.0f : t;
}

/* log2 for the level of detail: the exponent exactly, the mantissa by a quadratic good
 * to about 0.005 - far finer than the half-level steps a mip filter rounds to. The
 * quadratic is log2(m) + 1 over m in [1, 2), which is why the exponent's bias is 128
 * here and not 127. The triangle rasteriser's alone, so host-only with it. */
#ifdef OOPS_HOST_BUILD
static float gl_log2f(float x) {
    if (!(x > 0.0f))
        return -1000.0f;
    union {
        float f;
        uint32_t u;
    } v;
    v.f = x;
    const float e = (float)((int)((v.u >> 23) & 0xffu) - 128);
    v.u = (v.u & 0x007fffffu) | 0x3f800000u; /* the mantissa, as a float in [1, 2) */
    const float m = v.f;
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}
#endif

/* A texel index brought into the image by the wrap mode - or GL_FALSE for one that is
 * the border. GL_MIRRORED_REPEAT reflects every other repetition; GL_CLAMP and
 * GL_CLAMP_TO_BORDER leave an index outside the image outside it, and the texel there
 * is the border colour. */
static GLboolean gl_wrap_texel(int *i, int size, GLenum mode) {
    switch (mode) {
    case GL_REPEAT: {
        const int m = *i % size;
        *i = (m < 0) ? m + size : m;
        return GL_TRUE;
    }
    case GL_MIRRORED_REPEAT: {
        int m = *i % (2 * size);
        if (m < 0)
            m += 2 * size;
        *i = (m >= size) ? 2 * size - 1 - m : m;
        return GL_TRUE;
    }
    case GL_CLAMP:
    case GL_CLAMP_TO_BORDER:
        return (GLboolean)(*i >= 0 && *i < size);
    default: /* GL_CLAMP_TO_EDGE */
        *i = (*i < 0) ? 0 : ((*i >= size) ? size - 1 : *i);
        return GL_TRUE;
    }
}

/* A depth texel as it is read (GL 1.4, 3.8.14): under GL_COMPARE_R_TO_TEXTURE, 1
 * where the reference r - clamped to [0, 1] - passes GL_TEXTURE_COMPARE_FUNC against
 * the stored depth and 0 where it fails; the depth itself otherwise. Then
 * GL_DEPTH_TEXTURE_MODE: (v, v, v, 1) for luminance, (v, v, v, v) intensity, (0, 0, 0,
 * v) alpha. The reference is clamped as softpipe clamps it
 * (gallium/drivers/softpipe/sp_tex_sample.c:2803-2806). Compared per texel, before
 * filtering, so GL_LINEAR averages the comparisons - the percentage-closer filter a
 * GPU's comparison sampler gives; softpipe compares the filtered depth instead, and
 * GL 1.4 leaves the choice to the implementation. */
static void gl_depth_texel(const gl_texture_object_t *tex, float d, float ref,
                           float out[4]) {
    float v = d;
    if (tex->compare_mode == GL_COMPARE_R_TO_TEXTURE) {
        const float r = !(ref > 0.0f) ? 0.0f : ((ref > 1.0f) ? 1.0f : ref);
        GLboolean pass;
        switch (tex->compare_func) {
        case GL_NEVER:
            pass = GL_FALSE;
            break;
        case GL_LESS:
            pass = (GLboolean)(r < d);
            break;
        case GL_EQUAL:
            pass = (GLboolean)(r == d);
            break;
        case GL_GREATER:
            pass = (GLboolean)(r > d);
            break;
        case GL_NOTEQUAL:
            pass = (GLboolean)(r != d);
            break;
        case GL_GEQUAL:
            pass = (GLboolean)(r >= d);
            break;
        case GL_ALWAYS:
            pass = GL_TRUE;
            break;
        default:
            pass = (GLboolean)(r <= d);
            break; /* GL_LEQUAL */
        }
        v = pass ? 1.0f : 0.0f;
    }
    switch (tex->depth_mode) {
    case GL_INTENSITY:
        out[0] = out[1] = out[2] = out[3] = v;
        break;
    case GL_ALPHA:
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = v;
        break;
    default:
        out[0] = out[1] = out[2] = v;
        out[3] = 1.0f;
        break; /* GL_LUMINANCE */
    }
}

/* Texel (i, j) of slice `k`. A 1D or 2D level is a volume one slice deep, whose k is
 * always 0. */
static void gl_texel(const gl_texture_object_t *tex, const gl_tex_view_t *lv, int i,
                     int j, int k, float ref, float out[4]) {
    GLboolean in = gl_wrap_texel(&i, lv->width, tex->wrap_s);
    in = (GLboolean)(gl_wrap_texel(&j, lv->height, tex->wrap_t) && in);
    if (lv->depth > 1) {
        in = (GLboolean)(gl_wrap_texel(&k, lv->depth, tex->wrap_r) && in);
    } else {
        k = 0;
    }
    const GLboolean depth = (GLboolean)(lv->base_format == GL_DEPTH_COMPONENT);
    if (!in) {
        /* A depth texture's border is the border colour's red, as a depth. */
        if (depth) {
            gl_depth_texel(tex, tex->border_color[0], ref, out);
            return;
        }
        for (int n = 0; n < 4; n++)
            out[n] = tex->border_hw[n]; /* expanded as a texel is */
        return;
    }
    const uint8_t *p =
        lv->pixels + ((size_t)k * lv->slice + (size_t)j * lv->pitch + (size_t)i) * 4u;
    if (depth) {
        float d;
        memcpy(&d, p, 4u);
        gl_depth_texel(tex, d, ref, out);
        return;
    }
    out[0] = (float)p[0] / 255.0f;
    out[1] = (float)p[1] / 255.0f;
    out[2] = (float)p[2] / 255.0f;
    out[3] = (float)p[3] / 255.0f;
}

/* One level, GL_NEAREST or GL_LINEAR. Nearest is the texel containing (s * w, t * h, r
 * * d); linear weights the four around it in a slice, with texel centres at
 * half-integers - and for a volume the same four in the slice beyond, weighted by r:
 * eight texels, the specification's 3D filter. */
static void gl_sample_level(const gl_texture_object_t *tex, const gl_tex_view_t *lv,
                            float s, float t, float r, GLenum filter, float out[4]) {
    /* GL_CLAMP clamps the coordinate to [0, 1] first - so a linear filter at the
     * edge reaches half a texel into the border and takes half the border colour, and a
     * nearest one never leaves the image: the last texel at s = 1, as Mesa's software
     * sampler has it. */
    const float ref = r; /* a depth texture's comparison reference, before any wrap */
    if (tex->wrap_s == GL_CLAMP)
        s = (s < 0.0f) ? 0.0f : ((s > 1.0f) ? 1.0f : s);
    if (tex->wrap_t == GL_CLAMP)
        t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    if (tex->wrap_r == GL_CLAMP)
        r = (r < 0.0f) ? 0.0f : ((r > 1.0f) ? 1.0f : r);
    const float u = s * (float)lv->width;
    const float v = t * (float)lv->height;
    const float w = r * (float)lv->depth;
    if (filter != GL_LINEAR) {
        int i = (int)gl_floorf(u), j = (int)gl_floorf(v), k = (int)gl_floorf(w);
        if (tex->wrap_s == GL_CLAMP && i >= lv->width)
            i = lv->width - 1;
        if (tex->wrap_t == GL_CLAMP && j >= lv->height)
            j = lv->height - 1;
        if (tex->wrap_r == GL_CLAMP && k >= lv->depth)
            k = lv->depth - 1;
        gl_texel(tex, lv, i, j, k, ref, out);
        return;
    }
    const float uu = u - 0.5f, vv = v - 0.5f;
    const float fi = gl_floorf(uu), fj = gl_floorf(vv);
    const float a = uu - fi, b = vv - fj;
    const int i0 = (int)fi, j0 = (int)fj;
    const GLboolean volume = (GLboolean)(lv->depth > 1);
    int k0 = 0;
    float c = 0.0f;
    if (volume) {
        const float ww = w - 0.5f;
        const float fk = gl_floorf(ww);
        c = ww - fk;
        k0 = (int)fk;
    }
    float slice[2][4];
    for (int z = 0; z < (volume ? 2 : 1); z++) {
        float t00[4], t10[4], t01[4], t11[4];
        gl_texel(tex, lv, i0, j0, k0 + z, ref, t00);
        gl_texel(tex, lv, i0 + 1, j0, k0 + z, ref, t10);
        gl_texel(tex, lv, i0, j0 + 1, k0 + z, ref, t01);
        gl_texel(tex, lv, i0 + 1, j0 + 1, k0 + z, ref, t11);
        for (int n = 0; n < 4; n++) {
            slice[z][n] = (1.0f - a) * (1.0f - b) * t00[n] + a * (1.0f - b) * t10[n] +
                          (1.0f - a) * b * t01[n] + a * b * t11[n];
        }
    }
    for (int n = 0; n < 4; n++) {
        out[n] = volume ? (1.0f - c) * slice[0][n] + c * slice[1][n] : slice[0][n];
    }
}

/* A level of the texture being sampled - of cube face `face`, or the texture's own for
 * -1. */
static GLboolean gl_sample_view(const gl_texture_object_t *tex, int face, int level,
                                gl_tex_view_t *out) {
    return (face >= 0) ? gl_tex_face_view(tex, face, level, out)
                       : gl_tex_level_view(tex, level, out);
}

/* A cube map's face and the coordinates on it for the direction (rx, ry, rz): the
 * major axis picks the face and the other two, divided by it, the place - softpipe's
 * convert_cube (gallium/drivers/softpipe/sp_tex_sample.c:3220-3291), which restates
 * GL 1.3's table 3.19. A tie goes to x, then y, as there. */
static int gl_cube_face_coords(float rx, float ry, float rz, float *s, float *t) {
    const float ax = rx < 0.0f ? -rx : rx, ay = ry < 0.0f ? -ry : ry,
                az = rz < 0.0f ? -rz : rz;
    if (ax >= ay && ax >= az) {
        const float ima = (ax > 0.0f) ? -0.5f / ax : 0.0f;
        const float sign = (rx >= 0.0f) ? 1.0f : -1.0f;
        *s = sign * rz * ima + 0.5f;
        *t = ry * ima + 0.5f;
        return (rx >= 0.0f) ? 0 : 1;
    }
    if (ay >= ax && ay >= az) {
        const float ima = -0.5f / ay;
        const float sign = (ry >= 0.0f) ? 1.0f : -1.0f;
        *s = -rx * ima + 0.5f;
        *t = sign * -rz * ima + 0.5f;
        return (ry >= 0.0f) ? 2 : 3;
    }
    const float ima = -0.5f / az;
    const float sign = (rz >= 0.0f) ? 1.0f : -1.0f;
    *s = sign * -rx * ima + 0.5f;
    *t = ry * ima + 0.5f;
    return (rz >= 0.0f) ? 4 : 5;
}

/* The same projection onto a given face, for a neighbouring pixel's direction: the
 * level of detail compares two places on one face, as softpipe keeps a whole quad on
 * one face. The triangle rasteriser's alone, like gl_log2f. */
#ifdef OOPS_HOST_BUILD
static void gl_cube_on_face(int face, float rx, float ry, float rz, float *s,
                            float *t) {
    const float m = (face < 2) ? rx : ((face < 4) ? ry : rz);
    const float am = m < 0.0f ? -m : m;
    const float ima = (am > 0.0f) ? -0.5f / am : 0.0f;
    const float sign = (face & 1) ? -1.0f : 1.0f;
    if (face < 2) {
        *s = sign * rz * ima + 0.5f;
        *t = ry * ima + 0.5f;
    } else if (face < 4) {
        *s = -rx * ima + 0.5f;
        *t = sign * -rz * ima + 0.5f;
    } else {
        *s = sign * -rx * ima + 0.5f;
        *t = ry * ima + 0.5f;
    }
}
#endif

/* The texture at (s, t) with level of detail `lod`, on a texture the caller knows is
 * complete.
 *
 * Magnification or minification by the specification's rule: minify when lod exceeds c,
 * where c is 0.5 if the magnification filter is GL_LINEAR and the minification one is a
 * GL_NEAREST_MIPMAP_* - so the two filters meet without a seam - and 0 otherwise. A
 * *_MIPMAP_NEAREST filter reads level ceil(lod + 1/2) - 1, a *_MIPMAP_LINEAR one blends
 * floor(lod) and the level after it by the fraction, both clamped to the last level. */
static void gl_sample_texture(const gl_texture_object_t *tex, int face, float s,
                              float t, float r, float lod, float out[4]) {
    const GLenum minf = tex->min_filter, magf = tex->mag_filter;
    /* GL 1.2's parameters: the level of detail clamped to [GL_TEXTURE_MIN_LOD,
     * GL_TEXTURE_MAX_LOD] before anything reads it, and levels counted from
     * GL_TEXTURE_BASE_LEVEL up to the top level - which GL_TEXTURE_MAX_LEVEL may cut
     * short. */
    if (lod < tex->min_lod)
        lod = tex->min_lod;
    if (lod > tex->max_lod)
        lod = tex->max_lod;
    const int b = tex->base_level;
    gl_tex_view_t base;
    (void)gl_sample_view(tex, face, b, &base);
    const float c = (magf == GL_LINEAR && (minf == GL_NEAREST_MIPMAP_NEAREST ||
                                           minf == GL_NEAREST_MIPMAP_LINEAR))
                        ? 0.5f
                        : 0.0f;
    if (lod <= c) {
        gl_sample_level(tex, &base, s, t, r, magf, out);
        return;
    }
    if (!gl_filter_uses_mipmaps(minf)) {
        gl_sample_level(tex, &base, s, t, r, minf, out);
        return;
    }
    const GLenum within =
        (minf == GL_NEAREST_MIPMAP_NEAREST || minf == GL_NEAREST_MIPMAP_LINEAR)
            ? GL_NEAREST
            : GL_LINEAR;
    const int top = gl_tex_top_level(tex);
    if (minf == GL_NEAREST_MIPMAP_NEAREST || minf == GL_LINEAR_MIPMAP_NEAREST) {
        /* base + ceil(lod + 1/2) - 1, with ceil(x) written as -floor(-x). */
        int d = b + ((lod <= 0.5f) ? 0 : (int)(-gl_floorf(-(lod + 0.5f))) - 1);
        if (d > top)
            d = top;
        gl_tex_view_t lv;
        (void)gl_sample_view(tex, face, d, &lv);
        gl_sample_level(tex, &lv, s, t, r, within, out);
        return;
    }
    int d1 = b + (int)gl_floorf(lod);
    if (d1 > top)
        d1 = top;
    const int d2 = (d1 + 1 > top) ? top : d1 + 1;
    const float f = (d1 == top) ? 0.0f : lod - gl_floorf(lod);
    gl_tex_view_t l1, l2;
    (void)gl_sample_view(tex, face, d1, &l1);
    (void)gl_sample_view(tex, face, d2, &l2);
    float a[4], c2[4];
    gl_sample_level(tex, &l1, s, t, r, within, a);
    gl_sample_level(tex, &l2, s, t, r, within, c2);
    for (int k = 0; k < 4; k++)
        out[k] = (1.0f - f) * a[k] + f * c2[k];
}

/* -------------------------------------------------------------------------
 * The sampler, as a GLSL `texture2D` reaches it
 *
 * Here rather than in glsl_exec.c because the filtering rules it needs are this file's.
 *
 * The texture enables play no part: a sampler's declared type names its target, and a
 * program need not call `glEnable` (GL 2.0, 3.8.15). The binding is read directly.
 * ------------------------------------------------------------------------- */
GLboolean gl_shader_sample(const gl_context_t *ctx, GLuint unit, GLenum sampler_type,
                           const float coord[4], float lod, float out[4]) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;
    if (!ctx || !coord || unit >= (GLuint)OOPS_GL_MAX_TEXTURE_UNITS)
        return GL_FALSE;
    const gl_tex_unit_t *tu = &ctx->tex_unit[unit];

    GLuint id = 0u;
    switch (sampler_type) {
    case GL_SAMPLER_1D:
    case GL_SAMPLER_1D_SHADOW:
        id = tu->bound_texture_1d ? tu->bound_texture_1d : OOPS_GL_DEFAULT_TEXTURE_1D;
        break;
    case GL_SAMPLER_2D:
    case GL_SAMPLER_2D_SHADOW:
        id = tu->bound_texture_2d ? tu->bound_texture_2d : OOPS_GL_DEFAULT_TEXTURE_2D;
        break;
    case GL_SAMPLER_3D:
        id = tu->bound_texture_3d ? tu->bound_texture_3d : OOPS_GL_DEFAULT_TEXTURE_3D;
        break;
    case GL_SAMPLER_CUBE:
        id = tu->bound_texture_cube ? tu->bound_texture_cube
                                    : OOPS_GL_DEFAULT_TEXTURE_CUBE;
        break;
    default:
        return GL_FALSE;
    }
    const gl_texture_object_t *tex = gl_lookup_texture(ctx, id);
    if (!tex || !gl_texture_complete(tex))
        return GL_FALSE;

    /* A cube map is looked up by (s, t, r) as a direction: which face it points at, and
     * where on that face. The same projection the fixed-function path uses. */
    int face = -1;
    float s = coord[0], t = coord[1];
    const float r = coord[2];
    if (tex->cube)
        face = gl_cube_face_coords(coord[0], coord[1], coord[2], &s, &t);

    /* A shadow sampler's reference is the coordinate's third component; the
     * comparison happens in `gl_texel` when the texture's compare mode says so. */
    gl_sample_texture(tex, face, s, t, (face >= 0) ? 0.0f : r, lod, out);
    return GL_TRUE;
}

/* One combiner argument: the source's colour (or alpha), or one minus it, per the
 * operand - Mesa's get_source (main/ff_fragment_shader.c:423-466). GL_TEXTURE is this
 * unit's texel, GL 1.4's GL_TEXTUREn unit n's (zero for a unit that applies nothing,
 * :760-762), GL_PREVIOUS the colour the unit before left - the fragment's own for unit
 * 0 - and GL_PRIMARY_COLOR the fragment's own always. */
static void gl_combine_arg(const gl_tex_unit_t *tu, GLuint unit, GLenum source,
                           GLenum operand, const gl_unit_texels_t *texels,
                           const float primary[4], const float c[4], float out[4]) {
    const GLuint n = (GLuint)(source - GL_TEXTURE0);
    const float *v = (source == GL_TEXTURE)            ? texels->t[unit]
                     : (n < OOPS_GL_MAX_TEXTURE_UNITS) ? texels->t[n]
                     : (source == GL_CONSTANT)         ? tu->tex_env_color
                     : (source == GL_PRIMARY_COLOR)    ? primary
                                                       : c;
    for (int i = 0; i < 4; i++) {
        float x = (operand == GL_SRC_ALPHA || operand == GL_ONE_MINUS_SRC_ALPHA) ? v[3]
                                                                                 : v[i];
        if (operand == GL_ONE_MINUS_SRC_COLOR || operand == GL_ONE_MINUS_SRC_ALPHA)
            x = 1.0f - x;
        out[i] = x;
    }
}

/* One combiner function of its (up to three) arguments, per component - Mesa's
 * emit_combine (main/ff_fragment_shader.c:563-588). The dot products are GL_DOT3's (2a
 * - 1).(2b - 1). */
static float gl_combine_fn(GLenum mode, const float a0[4], const float a1[4],
                           const float a2[4], int i) {
    switch (mode) {
    case GL_REPLACE:
        return a0[i];
    case GL_ADD:
        return a0[i] + a1[i];
    case GL_ADD_SIGNED:
        return a0[i] + a1[i] - 0.5f;
    case GL_INTERPOLATE:
        return a0[i] * a2[i] + a1[i] * (1.0f - a2[i]);
    case GL_SUBTRACT:
        return a0[i] - a1[i];
    case GL_DOT3_RGB:
    case GL_DOT3_RGBA: {
        float d = 0.0f;
        for (int k = 0; k < 3; k++)
            d += (2.0f * a0[k] - 1.0f) * (2.0f * a1[k] - 1.0f);
        return d;
    }
    default:
        return a0[i] * a1[i]; /* GL_MODULATE */
    }
}

/* GL_COMBINE (GL 1.3): the colour and alpha each from their own function of their
 * own arguments, scaled by GL_RGB_SCALE and GL_ALPHA_SCALE and clamped to [0, 1] after
 * the scale, as Mesa's emit_texenv does (main/ff_fragment_shader.c:629-735).
 * GL_DOT3_RGBA puts the dot product in alpha too and the alpha function is not used. */
static void gl_tex_combine(const gl_tex_unit_t *tu, GLuint unit,
                           const gl_unit_texels_t *texels, const float primary[4],
                           float c[4]) {
    const gl_combine_t *cb = &tu->combine;
    float r0[4], r1[4], r2[4], q0[4], q1[4], q2[4];
    gl_combine_arg(tu, unit, cb->source_rgb[0], cb->operand_rgb[0], texels, primary, c,
                   r0);
    gl_combine_arg(tu, unit, cb->source_rgb[1], cb->operand_rgb[1], texels, primary, c,
                   r1);
    gl_combine_arg(tu, unit, cb->source_rgb[2], cb->operand_rgb[2], texels, primary, c,
                   r2);
    gl_combine_arg(tu, unit, cb->source_alpha[0], cb->operand_alpha[0], texels, primary,
                   c, q0);
    gl_combine_arg(tu, unit, cb->source_alpha[1], cb->operand_alpha[1], texels, primary,
                   c, q1);
    gl_combine_arg(tu, unit, cb->source_alpha[2], cb->operand_alpha[2], texels, primary,
                   c, q2);
    float out[4];
    for (int i = 0; i < 3; i++)
        out[i] = gl_combine_fn(cb->mode_rgb, r0, r1, r2, i) * cb->scale_rgb;
    out[3] = (cb->mode_rgb == GL_DOT3_RGBA)
                 ? gl_combine_fn(GL_DOT3_RGBA, r0, r1, r2, 0) * cb->scale_alpha
                 : gl_combine_fn(cb->mode_alpha, q0, q1, q2, 3) * cb->scale_alpha;
    for (int i = 0; i < 4; i++) {
        c[i] = !(out[i] > 0.0f) ? 0.0f : ((out[i] > 1.0f) ? 1.0f : out[i]);
    }
}

/* The texture environment: the fragment colour `c` combined with a texel `t` as the
 * texture's base format decides - Mesa's `calculate_derived_texenv`
 * (main/texstate.c:173), which restates GL's table of texture functions:
 *
 * - the colour is the fragment's own for a GL_ALPHA texture, and for GL_DECAL of
 * anything but GL_RGB and GL_RGBA (which GL leaves undefined and Mesa defines so);
 * - the alpha is the fragment's own for GL_LUMINANCE and GL_RGB textures, and always
 * for GL_DECAL;
 * - otherwise, per mode: GL_REPLACE the texel, GL_MODULATE the product, GL_ADD the sum
 * for colour and the product for alpha (the sum for GL_INTENSITY), GL_DECAL the texel's
 * colour laid over the fragment's by the texel's alpha, GL_BLEND the environment colour
 * and the fragment's mixed by the texel (alpha mixed the same way for GL_INTENSITY, and
 * multiplied otherwise).
 *
 * The texel arrives as the base format expanded it - (0, 0, 0, A), (L, L, L, 1), (I, I,
 * I, I) - and the result is clamped to [0, 1]. */
void gl_tex_env_apply(const gl_context_t *ctx, GLuint unit, GLenum base,
                      const gl_unit_texels_t *texels, const float primary[4],
                      float c[4]) {
    const gl_tex_unit_t *tu = &ctx->tex_unit[unit];
    const float *t = texels->t[unit];
    const GLenum mode = tu->tex_env_mode;
    if (mode == GL_COMBINE) {
        gl_tex_combine(tu, unit, texels, primary, c);
        return;
    }
    const float *k = tu->tex_env_color;
    const GLboolean keep_rgb =
        (GLboolean)(base == GL_ALPHA ||
                    (mode == GL_DECAL && base != GL_RGB && base != GL_RGBA));
    const GLboolean keep_a =
        (GLboolean)(base == GL_LUMINANCE || base == GL_RGB || mode == GL_DECAL);
    if (!keep_rgb) {
        for (int i = 0; i < 3; i++) {
            switch (mode) {
            case GL_REPLACE:
                c[i] = t[i];
                break;
            case GL_ADD:
                c[i] = c[i] + t[i];
                break;
            case GL_DECAL:
                c[i] = (base == GL_RGBA) ? c[i] * (1.0f - t[3]) + t[i] * t[3] : t[i];
                break;
            case GL_BLEND:
                c[i] = c[i] * (1.0f - t[i]) + k[i] * t[i];
                break;
            default:
                c[i] = c[i] * t[i];
                break; /* GL_MODULATE */
            }
        }
    }
    if (!keep_a) {
        switch (mode) {
        case GL_REPLACE:
            c[3] = t[3];
            break;
        case GL_ADD:
            c[3] = (base == GL_INTENSITY) ? c[3] + t[3] : c[3] * t[3];
            break;
        case GL_BLEND:
            c[3] = (base == GL_INTENSITY) ? c[3] * (1.0f - t[3]) + k[3] * t[3]
                                          : c[3] * t[3];
            break;
        default:
            c[3] = c[3] * t[3];
            break; /* GL_MODULATE */
        }
    }
    for (int i = 0; i < 4; i++) {
        if (!(c[i] > 0.0f))
            c[i] = 0.0f;
        else if (c[i] > 1.0f)
            c[i] = 1.0f;
    }
}

/* -------------------------------------------------------------------------
 * The per-fragment operations
 *
 * What happens to a shaded fragment, in GL's order: the alpha test, the stencil test,
 * the depth test's verdict, blending, the logic op and the write masks. One copy, for
 * triangles and for pixel rectangles (glDrawPixels, glBitmap, glCopyPixels) both.
 * ------------------------------------------------------------------------- */

/* The state the tail reads that does not change within a primitive, hoisted out of the
 * loop. The stencil state is the primitive's face's (GL 2.0, `glStencilOpSeparate`),
 * resolved once per primitive; `front` is GL_TRUE for points and lines, as the
 * specification says. */
static void gl_frag_ops_init_face(const gl_context_t *ctx, gl_frag_ops_t *o,
                                  GLboolean stencil_ok, GLboolean front) {
    o->stencil_test =
        (GLboolean)(stencil_ok && ctx->cap_stencil_test && ctx->stencil_buffer != NULL);
    o->stencil_func = front ? ctx->stencil_func : ctx->stencil_back_func;
    o->stencil_ref = front ? ctx->stencil_ref : ctx->stencil_back_ref;
    o->stencil_vmask = front ? ctx->stencil_value_mask : ctx->stencil_back_value_mask;
    o->stencil_wmask = front ? ctx->stencil_writemask : ctx->stencil_back_writemask;
    o->stencil_op_fail = front ? ctx->stencil_fail : ctx->stencil_back_fail;
    o->stencil_op_zfail = front ? ctx->stencil_zfail : ctx->stencil_back_zfail;
    o->stencil_op_zpass = front ? ctx->stencil_zpass : ctx->stencil_back_zpass;
    /* A logic op replaces blending rather than following it (Mesa's state tracker
     * leaves every blend target off when it is enabled,
     * state_tracker/st_atom_blend.c:269-273). GL_COPY is a logic op that changes
     * nothing, which is how radeonsi treats it too (si_state.c:341). */
    o->logic_mode = gl_logicop_mode(ctx->logic_op);
    o->logic_on = (GLboolean)(ctx->cap_color_logic_op && o->logic_mode != 12u);
    o->blend = (GLboolean)(ctx->cap_blend && !ctx->cap_color_logic_op);
}

/* The front face's state, for the callers that do not know a facing yet. */
static void gl_frag_ops_init(const gl_context_t *ctx, gl_frag_ops_t *o,
                             GLboolean stencil_ok) {
    gl_frag_ops_init_face(ctx, o, stencil_ok, GL_TRUE);
}

static inline GLboolean gl_depth_passes(GLenum func, float z, float cur) {
    switch (func) {
    case GL_LESS:
        return (GLboolean)(z < cur);
    case GL_LEQUAL:
        return (GLboolean)(z <= cur);
    case GL_GREATER:
        return (GLboolean)(z > cur);
    case GL_GEQUAL:
        return (GLboolean)(z >= cur);
    case GL_EQUAL:
        return (GLboolean)(z == cur);
    case GL_NOTEQUAL:
        return (GLboolean)(z != cur);
    case GL_ALWAYS:
        return GL_TRUE;
    case GL_NEVER:
        return GL_FALSE;
    default:
        return (GLboolean)(z < cur);
    }
}

static inline void gl_fragment_colour(const gl_context_t *ctx, const gl_frag_ops_t *o,
                                      uint32_t *px, float r, float g, float b, float a);

/* From the alpha test to the colour write, for one shaded fragment. `px` is its
 * colour-buffer pixel, `dz` its depth-buffer entry when the depth test ran and depth
 * writes are on (NULL otherwise), `sp` its stencil byte when the stencil test runs, and
 * `depth_failed` the depth test's verdict - taken by the caller, before shading, and
 * honoured here after stencil. */
static inline void gl_fragment_tail(gl_context_t *ctx, const gl_frag_ops_t *o,
                                    uint32_t *px, float *dz, uint8_t *sp, float z,
                                    GLboolean depth_failed, float r, float g, float b,
                                    float a) {
    /* The alpha test (the hardware patches a discard into the pixel shader). After the
     * texture environment, fog and coverage, because GL tests the final fragment
     * alpha. */
    if (ctx->cap_alpha_test) {
        GLboolean keep;
        switch (ctx->alpha_func) {
        case GL_NEVER:
            keep = GL_FALSE;
            break;
        case GL_LESS:
            keep = (GLboolean)(a < ctx->alpha_ref);
            break;
        case GL_EQUAL:
            keep = (GLboolean)(a == ctx->alpha_ref);
            break;
        case GL_LEQUAL:
            keep = (GLboolean)(a <= ctx->alpha_ref);
            break;
        case GL_GREATER:
            keep = (GLboolean)(a > ctx->alpha_ref);
            break;
        case GL_NOTEQUAL:
            keep = (GLboolean)(a != ctx->alpha_ref);
            break;
        case GL_GEQUAL:
            keep = (GLboolean)(a >= ctx->alpha_ref);
            break;
        default:
            keep = GL_TRUE;
            break; /* GL_ALWAYS */
        }
        if (!keep)
            return; /* and the deferred depth write never happens */
    }

    /* GL's order is alpha test, stencil, depth, so a fragment the alpha test discards
     * never reaches the stencil buffer. The stencil buffer is written even when the
     * stencil test fails: the fail operation is how a stencil mask is built. */
    if (o->stencil_test && sp) {
        const uint32_t masked_ref = (uint32_t)o->stencil_ref & o->stencil_vmask;
        const uint32_t masked_val = (uint32_t)(*sp) & o->stencil_vmask;
        GLboolean spass;
        switch (o->stencil_func) {
        case GL_NEVER:
            spass = GL_FALSE;
            break;
        case GL_LESS:
            spass = (GLboolean)(masked_ref < masked_val);
            break;
        case GL_LEQUAL:
            spass = (GLboolean)(masked_ref <= masked_val);
            break;
        case GL_GREATER:
            spass = (GLboolean)(masked_ref > masked_val);
            break;
        case GL_GEQUAL:
            spass = (GLboolean)(masked_ref >= masked_val);
            break;
        case GL_EQUAL:
            spass = (GLboolean)(masked_ref == masked_val);
            break;
        case GL_NOTEQUAL:
            spass = (GLboolean)(masked_ref != masked_val);
            break;
        default:
            spass = GL_TRUE;
            break; /* GL_ALWAYS */
        }
        if (!spass) {
            gl_stencil_apply(sp, o->stencil_op_fail, o->stencil_ref, o->stencil_wmask);
            return; /* no colour, and no depth - the deferred write is abandoned */
        }
        gl_stencil_apply(sp, depth_failed ? o->stencil_op_zfail : o->stencil_op_zpass,
                         o->stencil_ref, o->stencil_wmask);
    }
    /* The depth test's verdict, honoured now that stencil has had its say. */
    if (depth_failed)
        return;

    /* A sample passed - what GL 1.5's GL_SAMPLES_PASSED counts: past the alpha,
     * stencil and depth tests, one sample a fragment with no multisample buffer. */
    if (ctx->query_active)
        ctx->query_samples++;

    if (dz)
        *dz = z;

    if (!px)
        return;
    gl_fragment_colour(ctx, o, px, r, g, b, a);
    /* The second buffer glDrawBuffer(GL_FRONT_AND_BACK) names, blended and masked
     * against its own pixel - `px` is always the primary buffer's. */
    if (ctx->fb_also)
        gl_fragment_colour(ctx, o, ctx->fb_also + (px - ctx->framebuffer), r, g, b, a);
}

/* A fragment's colour into one colour buffer's pixel: blending, the logic op and the
 * colour mask (GL 1.x, 4.1.7 to 4.1.10, and 4.2.2). */
static inline void gl_fragment_colour(const gl_context_t *ctx, const gl_frag_ops_t *o,
                                      uint32_t *px, float r, float g, float b,
                                      float a) {
    uint32_t ir = (uint32_t)(r * 255.0f + 0.5f);
    uint32_t ig = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t ib = (uint32_t)(b * 255.0f + 0.5f);
    uint32_t ia = (uint32_t)(a * 255.0f + 0.5f);
    if (ir > 255)
        ir = 255;
    if (ig > 255)
        ig = 255;
    if (ib > 255)
        ib = 255;
    if (ia > 255)
        ia = 255;

    if (o->blend) {
        uint32_t dst = *px;
        float dr = (float)((dst >> 16) & 0xff) / 255.0f;
        float dg = (float)((dst >> 8) & 0xff) / 255.0f;
        float db_col = (float)(dst & 0xff) / 255.0f;
        float da = (float)((dst >> 24) & 0xff) / 255.0f;

        const float *bc = ctx->blend_color;
        float sfr =
            get_blend_factor(ctx->blend_src, bc, r, g, b, a, dr, dg, db_col, da, 0);
        float sfg =
            get_blend_factor(ctx->blend_src, bc, r, g, b, a, dr, dg, db_col, da, 1);
        float sfb =
            get_blend_factor(ctx->blend_src, bc, r, g, b, a, dr, dg, db_col, da, 2);
        float sfa = get_blend_factor(ctx->blend_src_alpha, bc, r, g, b, a, dr, dg,
                                     db_col, da, 3);

        float dfr =
            get_blend_factor(ctx->blend_dst, bc, r, g, b, a, dr, dg, db_col, da, 0);
        float dfg =
            get_blend_factor(ctx->blend_dst, bc, r, g, b, a, dr, dg, db_col, da, 1);
        float dfb =
            get_blend_factor(ctx->blend_dst, bc, r, g, b, a, dr, dg, db_col, da, 2);
        float dfa = get_blend_factor(ctx->blend_dst_alpha, bc, r, g, b, a, dr, dg,
                                     db_col, da, 3);

        /* The blend equation. GL_MIN and GL_MAX ignore the factors. The colour and the
         * alpha take their own equation (GL 2.0, `glBlendEquationSeparate`);
         * `glBlendEquation` sets both. */
        float res_r, res_g, res_b, res_a;
        switch (ctx->blend_equation) {
        case GL_FUNC_SUBTRACT:
            res_r = r * sfr - dr * dfr;
            res_g = g * sfg - dg * dfg;
            res_b = b * sfb - db_col * dfb;
            break;
        case GL_FUNC_REVERSE_SUBTRACT:
            res_r = dr * dfr - r * sfr;
            res_g = dg * dfg - g * sfg;
            res_b = db_col * dfb - b * sfb;
            break;
        case GL_MIN:
            res_r = r < dr ? r : dr;
            res_g = g < dg ? g : dg;
            res_b = b < db_col ? b : db_col;
            break;
        case GL_MAX:
            res_r = r > dr ? r : dr;
            res_g = g > dg ? g : dg;
            res_b = b > db_col ? b : db_col;
            break;
        default: /* GL_FUNC_ADD */
            res_r = r * sfr + dr * dfr;
            res_g = g * sfg + dg * dfg;
            res_b = b * sfb + db_col * dfb;
            break;
        }
        switch (ctx->blend_equation_alpha) {
        case GL_FUNC_SUBTRACT:
            res_a = a * sfa - da * dfa;
            break;
        case GL_FUNC_REVERSE_SUBTRACT:
            res_a = da * dfa - a * sfa;
            break;
        case GL_MIN:
            res_a = a < da ? a : da;
            break;
        case GL_MAX:
            res_a = a > da ? a : da;
            break;
        default:
            res_a = a * sfa + da * dfa;
            break;
        }

        if (res_r < 0.0f)
            res_r = 0.0f;
        else if (res_r > 1.0f)
            res_r = 1.0f;
        if (res_g < 0.0f)
            res_g = 0.0f;
        else if (res_g > 1.0f)
            res_g = 1.0f;
        if (res_b < 0.0f)
            res_b = 0.0f;
        else if (res_b > 1.0f)
            res_b = 1.0f;
        if (res_a < 0.0f)
            res_a = 0.0f;
        else if (res_a > 1.0f)
            res_a = 1.0f;

        ir = (uint32_t)(res_r * 255.0f + 0.5f);
        ig = (uint32_t)(res_g * 255.0f + 0.5f);
        ib = (uint32_t)(res_b * 255.0f + 0.5f);
        ia = (uint32_t)(res_a * 255.0f + 0.5f);
    }

    /* The logic op works on the stored bits, so it runs on the 8-bit values the colour
     * block would hold - after conversion, before the colour mask. */
    if (o->logic_on) {
        const uint32_t d = *px;
        ir = gl_logicop_apply(o->logic_mode, ir, (d >> 16) & 0xffu);
        ig = gl_logicop_apply(o->logic_mode, ig, (d >> 8) & 0xffu);
        ib = gl_logicop_apply(o->logic_mode, ib, d & 0xffu);
        ia = gl_logicop_apply(o->logic_mode, ia, (d >> 24) & 0xffu);
    }

    /* The colour mask. Every channel on is a plain store, which is what keeps a pixel
     * rectangle written into write-combined memory on the target from reading it back
     * for nothing. */
    const uint32_t src_px = (ia << 24) | (ir << 16) | (ig << 8) | ib;
    if (gl_color_writes(ctx, 0) && gl_color_writes(ctx, 1) && gl_color_writes(ctx, 2) &&
        gl_color_writes(ctx, 3)) {
        *px = src_px;
        return;
    }
    const uint32_t keep = (gl_color_writes(ctx, 0) ? 0u : 0x00ff0000u) |
                          (gl_color_writes(ctx, 1) ? 0u : 0x0000ff00u) |
                          (gl_color_writes(ctx, 2) ? 0u : 0x000000ffu) |
                          (gl_color_writes(ctx, 3) ? 0u : 0xff000000u);
    *px = (src_px & ~keep) | (*px & keep);
}

/* Pixel rectangles as fragments (GL 1.x, 3.6.4-3.6.5, 3.7 and 3.8). Each pixel
 * glDrawPixels, glBitmap or glCopyPixels produces is a fragment at the raster
 * position's window z, with the raster position's texture coordinate and its distance
 * for fog - and from there it meets everything a triangle's fragment does: the texture
 * environment, fog, the scissor, alpha, stencil and depth tests, blending, the logic op
 * and the masks. What is constant across the rectangle is worked out once here,
 * including the one texel every fragment samples.
 *
 * On the console the depth and stencil tests run against the GPU's tiled surfaces
 * through gl_zs_depth_ptr and gl_zs_stencil_ptr. It all runs on the CPU against the
 * frame, after the flush the caller makes. */
void gl_pixel_frags_begin(gl_context_t *ctx, gl_pixel_frags_t *pf) {
    gl_frag_ops_init(ctx, &pf->ops, GL_TRUE);
    pf->depth_test = (GLboolean)(ctx->cap_depth_test && ctx->depth_buffer != NULL);
    pf->z = ctx->raster_pos[2];

    /* The scissor box, in window coordinates, inclusive; the whole buffer without it.
     */
    pf->sc_x0 = 0;
    pf->sc_y0 = 0;
    pf->sc_x1 = (int)ctx->width - 1;
    pf->sc_y1 = (int)ctx->height - 1;
    if (ctx->cap_scissor_test) {
        if (ctx->sc_x > pf->sc_x0)
            pf->sc_x0 = ctx->sc_x;
        if (ctx->sc_y > pf->sc_y0)
            pf->sc_y0 = ctx->sc_y;
        if (ctx->sc_x + ctx->sc_w - 1 < pf->sc_x1)
            pf->sc_x1 = ctx->sc_x + ctx->sc_w - 1;
        if (ctx->sc_y + ctx->sc_h - 1 < pf->sc_y1)
            pf->sc_y1 = ctx->sc_y + ctx->sc_h - 1;
    }

    /* One texel for the whole rectangle: every fragment carries the raster
     * position's texture coordinate (GL 1.x, 3.6.5 - "the current raster position's
     * associated data"), so the sample is the same everywhere, and its level of detail
     * is that of an unchanging coordinate: magnification. */
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        pf->tex[u] = NULL;
        pf->tex_format[u] = GL_RGBA;
        for (int k = 0; k < 4; k++)
            pf->texel.t[u][k] = 0.0f;
        const GLuint tex_id = gl_unit_texture_id(ctx, u);
        const gl_texture_object_t *tex = tex_id ? gl_lookup_texture(ctx, tex_id) : NULL;
        if (!tex)
            continue;
        const float *tc = ctx->raster_texcoord[u];
        const float iq = gl_q_inv(tc[3]);
        float s = tc[0] * iq, t = tc[1] * iq, r = tc[2] * iq;
        int face = -1;
        if (tex->cube)
            face = gl_cube_face_coords(s, t, r, &s, &t);
        gl_sample_texture(tex, face, s, t, (face >= 0) ? 0.0f : r, -1000.0f,
                          pf->texel.t[u]);
        pf->tex[u] = tex;
        pf->tex_format[u] = gl_tex_sample_format(tex);
    }

    pf->fog_on = ctx->cap_fog;
    pf->fog_f = ctx->cap_fog ? gl_fog_factor(ctx, ctx->raster_distance) : 1.0f;
}

/* One fragment of a pixel rectangle, at window (x, y) - y counting up from the bottom -
 * with the colour its pixel conversion gave it. */
void gl_pixel_fragment(gl_context_t *ctx, const gl_pixel_frags_t *pf, int x, int y,
                       const float rgba[4]) {
    if (x < pf->sc_x0 || x > pf->sc_x1 || y < pf->sc_y0 || y > pf->sc_y1)
        return;
    if (!ctx->framebuffer)
        return;
    const size_t i = gl_color_index(ctx, x, y);

    GLboolean depth_failed = GL_FALSE;
    float *dz = NULL;
    if (pf->depth_test) {
        float *cur = gl_zs_depth_ptr(ctx, x, y);
        depth_failed = (GLboolean)!gl_depth_passes(ctx->depth_func, pf->z, *cur);
        if (ctx->depth_mask)
            dz = cur;
    }

    float c[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    /* Each unit in turn on what the one before it left (GL 1.3, 3.8.13). */
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        if (pf->tex[u])
            gl_tex_env_apply(ctx, u, pf->tex_format[u], &pf->texel, rgba, c);
    }
    if (pf->fog_on) {
        const float inv = 1.0f - pf->fog_f;
        for (int k = 0; k < 3; k++)
            c[k] = pf->fog_f * c[k] + inv * ctx->fog_color[k];
    }
    uint8_t *sp = pf->ops.stencil_test ? gl_zs_stencil_ptr(ctx, x, y) : NULL;
    gl_fragment_tail(ctx, &pf->ops, &ctx->framebuffer[i], dz, sp, pf->z, depth_failed,
                     c[0], c[1], c[2], c[3]);
    /* The CPU has written a colour word, so it owes a drain before anything reads the
     * buffer - gl_color_cpu_drain. Marked even when the tail wrote nothing (a failed
     * depth test, a zero colour mask), which costs only a few cache lines. */
    gl_color_cpu_touched(ctx, ctx->framebuffer, i);
}

#ifdef OOPS_HOST_BUILD
/* Whether an edge whose edge function rises by `a` a pixel right and `b` a pixel down
 * owns the pixel centres on it (gl_rasterize_triangle's tie rule), and whether a weight
 * is inside. */
static inline GLboolean gl_edge_owns_ties(float a, float b) {
    return (GLboolean)(a > 0.0f || (a == 0.0f && b > 0.0f));
}
#define GL_EDGE_IN(w, owns) ((w) > 0.0f || ((w) == 0.0f && (owns)))

/* One texture unit as a triangle samples it: the texture (NULL for none - disabled, or
 * incomplete, which gl_unit_texture_id answers as none), and what its level of detail
 * needs, worked out once per triangle. */
typedef struct {
    const gl_texture_object_t *tex;
    GLboolean needs_lod; /* the two filters differ, or one reads mipmaps */
    float bias;          /* GL 1.4's, the texture's and the unit's */
    float w, h, d;       /* the base level's size in texels */
    GLenum format;       /* the base format the environment reads it as */
} gl_unit_draw_t;

/* One unit's texel for a fragment. `pb` is the fragment's perspective-correct
 * barycentric weights, `b` its screen-space ones and `db` their slopes (d/dx, d/dy for
 * each vertex in turn), from which the neighbours' coordinates - and so the level of
 * detail - are found.
 *
 * s, t, r and q are interpolated, then divided here per fragment - GL's projective
 * texturing (GL 1.x, 3.8). */
static void gl_unit_sample(const gl_unit_draw_t *ud, GLuint unit,
                           const gl_screen_vertex_t *v0, const gl_screen_vertex_t *v1,
                           const gl_screen_vertex_t *v2, const float pb[3],
                           const float b[3], const float db[6], float texel[4]) {
    const float *t0 = v0->tc[unit], *t1 = v1->tc[unit], *t2 = v2->tc[unit];
    const float iq = gl_q_inv(pb[0] * t0[3] + pb[1] * t1[3] + pb[2] * t2[3]);
    const float u = (pb[0] * t0[0] + pb[1] * t1[0] + pb[2] * t2[0]) * iq;
    const float v = (pb[0] * t0[1] + pb[1] * t1[1] + pb[2] * t2[1]) * iq;
    /* r, which only a volume reads, and whose rate of change enters its level of detail
     * alongside s's and t's. */
    const float rc = (pb[0] * t0[2] + pb[1] * t1[2] + pb[2] * t2[2]) * iq;
    /* A cube map is looked up by (s, t, r) as a direction: the face it points at
     * and the place on that face (gl_cube_face_coords). */
    int face = -1;
    float fs = u, ft = v;
    if (ud->tex->cube)
        face = gl_cube_face_coords(u, v, rc, &fs, &ft);
    /* The level of detail from the screen-space derivatives of the texture coordinate,
     * taken by re-evaluating the perspective-correct coordinate one pixel to the right
     * and one below. Only needed when the two filters differ or one reads mipmaps;
     * otherwise any lod samples the same. A cube map's neighbours are projected onto
     * the centre's face first. */
    float lod = 0.0f;
    if (ud->needs_lod) {
        const float pxs[2] = {1.0f, 0.0f}, pys[2] = {0.0f, 1.0f};
        float rho2 = 0.0f;
        for (int k = 0; k < 2; k++) {
            float q0 = b[0] + db[0] * pxs[k] + db[1] * pys[k];
            float q1 = b[1] + db[2] * pxs[k] + db[3] * pys[k];
            float q2 = b[2] + db[4] * pxs[k] + db[5] * pys[k];
            q0 *= v0->inv_w;
            q1 *= v1->inv_w;
            q2 *= v2->inv_w;
            const float qs = q0 + q1 + q2;
            if (!(qs > 0.0f))
                continue;
            /* The neighbour's coordinate divided by its own q, as the fragment's is. */
            const float nk = gl_q_inv((q0 * t0[3] + q1 * t1[3] + q2 * t2[3]) / qs) / qs;
            float nu = (q0 * t0[0] + q1 * t1[0] + q2 * t2[0]) * nk;
            float nv = (q0 * t0[1] + q1 * t1[1] + q2 * t2[1]) * nk;
            const float nr = (q0 * t0[2] + q1 * t1[2] + q2 * t2[2]) * nk;
            if (face >= 0)
                gl_cube_on_face(face, nu, nv, nr, &nu, &nv);
            const float du = (nu - fs) * ud->w, dv = (nv - ft) * ud->h;
            float d2 = du * du + dv * dv;
            if (ud->d > 1.0f && face < 0) {
                const float dr = (nr - rc) * ud->d;
                d2 += dr * dr;
            }
            if (d2 > rho2)
                rho2 = d2;
        }
        lod = 0.5f * gl_log2f(rho2); /* log2(sqrt(rho2)) */
    }
    lod += ud->bias; /* GL 1.4's, before gl_sample_texture's LOD clamp */
    gl_sample_texture(ud->tex, face, fs, ft, (face >= 0) ? 0.0f : rc, lod, texel);
}

/* The interpolated block at one set of affine barycentric weights - the shader's
 * varyings and the fixed-function interpolants that sit beside them.
 *
 * Perspective-correct, like every other attribute here: each vertex weighted by its
 * 1/w and renormalised. Affine weights are right only for depth.
 *
 * Without a vertex shader there is no block to interpolate, so one is built from the
 * screen vertices - which is where the fixed-function stage left the same quantities.
 * That is what lets a program with only a fragment shader read `gl_Color` and
 * `gl_TexCoord[]`. */
static void gl_shader_vary_at(const gl_screen_vertex_t *v0,
                              const gl_screen_vertex_t *v1,
                              const gl_screen_vertex_t *v2, float b0, float b1,
                              float b2, GLboolean have_block,
                              float out[GL_SHADER_VARY_FLOATS]) {
    float pb0 = b0 * v0->inv_w, pb1 = b1 * v1->inv_w, pb2 = b2 * v2->inv_w;
    const float s = pb0 + pb1 + pb2;
    if (s > 0.0f) {
        const float inv = 1.0f / s;
        pb0 *= inv;
        pb1 *= inv;
        pb2 *= inv;
    } else {
        pb0 = b0;
        pb1 = b1;
        pb2 = b2;
    }

    if (have_block) {
        for (int k = 0; k < GL_SHADER_VARY_FLOATS; k++) {
            out[k] = pb0 * v0->vary[k] + pb1 * v1->vary[k] + pb2 * v2->vary[k];
        }
        return;
    }
    for (int k = 0; k < GL_SHADER_VARY_FLOATS; k++)
        out[k] = 0.0f;
    out[GL_SHADER_VARY_COLOR + 0] = pb0 * v0->r + pb1 * v1->r + pb2 * v2->r;
    out[GL_SHADER_VARY_COLOR + 1] = pb0 * v0->g + pb1 * v1->g + pb2 * v2->g;
    out[GL_SHADER_VARY_COLOR + 2] = pb0 * v0->b + pb1 * v1->b + pb2 * v2->b;
    out[GL_SHADER_VARY_COLOR + 3] = pb0 * v0->a + pb1 * v1->a + pb2 * v2->a;
    out[GL_SHADER_VARY_SECONDARY + 0] = pb0 * v0->sr + pb1 * v1->sr + pb2 * v2->sr;
    out[GL_SHADER_VARY_SECONDARY + 1] = pb0 * v0->sg + pb1 * v1->sg + pb2 * v2->sg;
    out[GL_SHADER_VARY_SECONDARY + 2] = pb0 * v0->sb + pb1 * v1->sb + pb2 * v2->sb;
    out[GL_SHADER_VARY_SECONDARY + 3] = 1.0f;
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        const int base = GL_SHADER_VARY_TEXCOORD + (int)u * 4;
        for (int i = 0; i < 4; i++) {
            out[base + i] =
                pb0 * v0->tc[u][i] + pb1 * v1->tc[u][i] + pb2 * v2->tc[u][i];
        }
    }
    /* `gl_FogFragCoord` stays zero here: the screen vertex carries the fog factor, not
     * the eye distance, so a fragment shader with no vertex shader reads zero, as
     * `glFogCoord(0)` would give. */
}

void gl_rasterize_triangle(gl_context_t *ctx, const gl_screen_vertex_t *v0,
                           const gl_screen_vertex_t *v1, const gl_screen_vertex_t *v2) {
    /* Compute triangle 2D bounding box */
    float fmin_x = v0->sx;
    if (v1->sx < fmin_x)
        fmin_x = v1->sx;
    if (v2->sx < fmin_x)
        fmin_x = v2->sx;
    float fmax_x = v0->sx;
    if (v1->sx > fmax_x)
        fmax_x = v1->sx;
    if (v2->sx > fmax_x)
        fmax_x = v2->sx;
    float fmin_y = v0->sy;
    if (v1->sy < fmin_y)
        fmin_y = v1->sy;
    if (v2->sy < fmin_y)
        fmin_y = v2->sy;
    float fmax_y = v0->sy;
    if (v1->sy > fmax_y)
        fmax_y = v1->sy;
    if (v2->sy > fmax_y)
        fmax_y = v2->sy;

    int min_x = (int)fmin_x;
    int max_x = (int)(fmax_x + 1.0f);
    int min_y = (int)fmin_y;
    int max_y = (int)(fmax_y + 1.0f);

    /* A smooth polygon's pixels reach past its edges by part of a pixel. */
    const unsigned aa_edges = (ctx->prim_raster == GL_FILL) ? ctx->aa_edges : 0u;
    if (aa_edges) {
        min_x--;
        max_x++;
        min_y--;
        max_y++;
    }

    /* Clip against scissor / screen bounds */
    int clip_min_x = 0;
    int clip_max_x = (int)ctx->width - 1;
    int clip_min_y = 0;
    int clip_max_y = (int)ctx->height - 1;

    if (ctx->cap_scissor_test) {
        if (ctx->sc_x > clip_min_x)
            clip_min_x = ctx->sc_x;
        if (ctx->sc_x + ctx->sc_w - 1 < clip_max_x)
            clip_max_x = ctx->sc_x + ctx->sc_w - 1;
        int sc_y_top = (int)ctx->height - (ctx->sc_y + ctx->sc_h);
        if (sc_y_top > clip_min_y)
            clip_min_y = sc_y_top;
        if (sc_y_top + ctx->sc_h - 1 < clip_max_y)
            clip_max_y = sc_y_top + ctx->sc_h - 1;
    }

    if (min_x < clip_min_x)
        min_x = clip_min_x;
    if (max_x > clip_max_x)
        max_x = clip_max_x;
    if (min_y < clip_min_y)
        min_y = clip_min_y;
    if (max_y > clip_max_y)
        max_y = clip_max_y;

    if (min_x > max_x || min_y > max_y)
        return;

    /* Barycentric edge equations */
    float x0 = v0->sx, y0 = v0->sy;
    float x1 = v1->sx, y1 = v1->sy;
    float x2 = v2->sx, y2 = v2->sy;

    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    float inv_area = 1.0f / area;

    /* Edge function deltas */
    float dx01 = x0 - x1, dy01 = y0 - y1;
    float dx12 = x1 - x2, dy12 = y1 - y2;
    float dx20 = x2 - x0, dy20 = y2 - y0;

    /* Sign adjust so interior evaluates positive */
    if (area < 0.0f) {
        dx01 = -dx01;
        dy01 = -dy01;
        dx12 = -dx12;
        dy12 = -dy12;
        dx20 = -dx20;
        dy20 = -dy20;
    }

    /* The tie rule: a pixel centre exactly on an edge belongs to one of the two
     * triangles that share it, not both - GL's requirement that adjacent polygons draw
     * each fragment once (GL 1.x, 3.5.1). The edge that owns its ties is a left one -
     * the interior to its +x - or a horizontal one with the interior below it in these
     * y-down screen rows, the top-left convention; the two triangles on a shared edge
     * see it with opposite normals, so exactly one owns it. The coefficients are each
     * edge function's x and y slopes, with the orientation's sign applied as the
     * weights' is below. */
    const float esgn = (area < 0.0f) ? -1.0f : 1.0f;
    const GLboolean tl0 = gl_edge_owns_ties(esgn * (y1 - y2), esgn * (x2 - x1));
    const GLboolean tl1 = gl_edge_owns_ties(esgn * (y2 - y0), esgn * (x0 - x2));
    const GLboolean tl2 = gl_edge_owns_ties(esgn * (y0 - y1), esgn * (x1 - x0));

    /* Antialiasing's coverage (GL 1.x, 3.3-3.5): the fraction of a pixel the
     * primitive covers, which multiplies the fragment's alpha after fog. A point is a
     * disc of radius r - coverage r + 1/2 minus the pixel centre's distance from its
     * centre, clamped to [0, 1]; a line a rectangle, the same across its width and, but
     * for a stipple's dashes, along its length; a polygon each of its own edges, faded
     * over the pixel either side of it. Each is the linear approximation to the pixel's
     * covered area, exact where the edge crosses a pixel straight.
     *
     * A polygon's edge function w_i is its edge's length times the pixel centre's
     * distance from it: w0 is edge v1-v2 (gl_draw_polygon_tri's bit 1), w1 v2-v0 (bit
     * 2), w2 v0-v1 (bit 0). */
    const GLenum aa_kind = ctx->aa_kind;
    const GLboolean aa_b0 = (GLboolean)((aa_edges >> 1) & 1u);
    const GLboolean aa_b1 = (GLboolean)((aa_edges >> 2) & 1u);
    const GLboolean aa_b2 = (GLboolean)(aa_edges & 1u);
    float aa_il0 = 0.0f, aa_il1 = 0.0f, aa_il2 = 0.0f;
    if (aa_edges) {
        const float l0 = gl_sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
        const float l1 = gl_sqrt((x0 - x2) * (x0 - x2) + (y0 - y2) * (y0 - y2));
        const float l2 = gl_sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
        aa_il0 = (l0 > 0.0f) ? 1.0f / l0 : 0.0f;
        aa_il1 = (l1 > 0.0f) ? 1.0f / l1 : 0.0f;
        aa_il2 = (l2 > 0.0f) ? 1.0f / l2 : 0.0f;
    }
    float aa_dx = 0.0f, aa_dy = 0.0f, aa_len = 0.0f;
    if (aa_kind == GL_LINE) {
        aa_dx = ctx->aa_b[0] - ctx->aa_a[0];
        aa_dy = ctx->aa_b[1] - ctx->aa_a[1];
        aa_len = gl_sqrt(aa_dx * aa_dx + aa_dy * aa_dy);
        if (aa_len > 0.0f) {
            aa_dx /= aa_len;
            aa_dy /= aa_len;
        }
    }

    uint32_t *fb = ctx->framebuffer;
    float *db = ctx->depth_buffer;
    uint32_t pitch = ctx->width;
    GLboolean depth_test = ctx->cap_depth_test;
    GLboolean depth_write = ctx->depth_mask;
    GLenum depth_func = ctx->depth_func;
    gl_frag_ops_t ops;
    /* Which face this triangle is, from its own winding - the same sign culling
     * reads: with the y-flip, counter-clockwise in normalised device coordinates is a
     * negative signed area here. Two things want it: GL 2.0's separate stencil state,
     * which is per face, and a fragment shader's `gl_FrontFacing`.
     *
     * Anything that is not a polygon - a point or a line expanded into triangles - is
     * front-facing, which is what the specification says of them. */
    GLboolean prim_front = GL_TRUE;
    if (gl_prim_is_polygon(ctx)) {
        const GLboolean is_ccw = (GLboolean)(area < 0.0f);
        prim_front = (ctx->front_face == GL_CCW) ? is_ccw : (GLboolean)!is_ccw;
    }
    gl_frag_ops_init_face(ctx, &ops, GL_TRUE, prim_front);
    const GLboolean stencil_test = ops.stencil_test;

    const GLboolean fog_on = ctx->cap_fog;
    const GLboolean sec_on = gl_color_sum_on(ctx);
    const float fog_r = ctx->fog_color[0];
    const float fog_g = ctx->fog_color[1];
    const float fog_b = ctx->fog_color[2];

    /* Hoisted so the fragment loop reads a local array rather than the context, and so
     * the common case - no clip plane enabled - costs one test per fragment instead of
     * six. */
    GLboolean clip_on[OOPS_GL_CLIP_PLANE_COUNT];
    GLboolean clip_any = GL_FALSE;
    for (int ci = 0; ci < OOPS_GL_CLIP_PLANE_COUNT; ci++) {
        clip_on[ci] = ctx->clip_plane_enabled[ci];
        if (clip_on[ci])
            clip_any = GL_TRUE;
    }

    /* Each unit's texture (GL 1.3), and what its level of detail needs, once per
     * triangle: the base level's size in texels -
     * GL_TEXTURE_BASE_LEVEL's image, from which the level of detail is measured - and
     * whether the filters make it matter. */
    gl_unit_draw_t ud[OOPS_GL_MAX_TEXTURE_UNITS];
    GLboolean any_tex = GL_FALSE;
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        const GLuint id = gl_unit_texture_id(ctx, u);
        const gl_texture_object_t *tex = id ? gl_lookup_texture(ctx, id) : NULL;
        ud[u].tex = tex;
        if (!tex)
            continue;
        any_tex = GL_TRUE;
        ud[u].needs_lod = (GLboolean)(tex->min_filter != tex->mag_filter ||
                                      gl_filter_uses_mipmaps(tex->min_filter));
        ud[u].bias = gl_tex_lod_bias(&ctx->tex_unit[u], tex);
        gl_tex_view_t bv;
        const GLboolean has_base = gl_tex_level_view(tex, tex->base_level, &bv);
        ud[u].w = has_base ? (float)bv.width : 1.0f;
        ud[u].h = has_base ? (float)bv.height : 1.0f;
        ud[u].d = (has_base && bv.depth > 1) ? (float)bv.depth : 1.0f;
        ud[u].format = gl_tex_sample_format(tex);
    }
    /* The barycentric weights' screen-space slopes (the edge functions' coefficients
     * over the area), which the level of detail's neighbours are found from. */
    const float dbw[6] = {
        -(y2 - y1) * inv_area, (x2 - x1) * inv_area,  -(y0 - y2) * inv_area,
        (x0 - x2) * inv_area,  -(y1 - y0) * inv_area, (x1 - x0) * inv_area,
    };

    /* The polygon stipple, for filled polygons only - not the triangles a line or
     * point becomes, nor a polygon's outline. Row `y` here counts down from the top and
     * GL's window rows count up, so the mask row is the window row's; with the stipple
     * off every row is all ones and the test below never discards. */
    const GLboolean poly_stipple =
        (GLboolean)(ctx->cap_polygon_stipple && ctx->prim_raster == GL_FILL);

    /* A GL 2.0 fragment program replaces the texturing, the colour sum and the fog. It
     * reads the interpolated block; without a vertex shader the fixed-function
     * interpolants fill it (gl_shader_vary_at).
     *
     * `needs_deriv` decides whether each fragment is evaluated once or three times: a
     * mipmapped texture lookup or a `dFdx` needs the neighbouring pixels' values. */
    gl_program_object_t *fprog = gl_active_program(ctx);
    const GLboolean prog_fs = (GLboolean)(fprog && fprog->fs);
    const GLboolean needs_deriv =
        (GLboolean)(prog_fs && gl_shader_needs_derivatives(fprog));
    /* Without a vertex shader the block is filled per fragment from the screen
     * vertices, so the fragment shader can read `gl_Color` and `gl_TexCoord[]`. */
    const GLboolean have_vary_block = (GLboolean)(v0->vary && v1->vary && v2->vary);

    /* `gl_FrontFacing` is `prim_front`, from the winding - not `prim_polygon_back`,
     * which is two-sided lighting's flag and set only while lighting is on. */

    for (int y = min_y; y <= max_y; y++) {
        float py = (float)y + 0.5f;
        uint32_t *fb_row = fb ? &fb[(uint32_t)y * pitch] : NULL;
        float *db_row = db ? &db[(uint32_t)y * pitch] : NULL;
        uint8_t *sb_row =
            stencil_test ? &ctx->stencil_buffer[(uint32_t)y * pitch] : NULL;
        const uint32_t stipple_row =
            poly_stipple ? ctx->polygon_stipple[((int)ctx->height - 1 - y) & 31]
                         : 0xffffffffu;

        for (int x = min_x; x <= max_x; x++) {
            if (!((stipple_row >> (31 - (x & 31))) & 1u))
                continue;
            float px = (float)x + 0.5f;

            /* Barycentric weights */
            float w0 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
            float w1 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);
            float w2 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);

            if (area < 0.0f) {
                w0 = -w0;
                w1 = -w1;
                w2 = -w2;
            }

            /* Inside - or, across a smooth polygon's own edge, near enough to be partly
             * covered - and how much of the pixel is covered. */
            GLboolean inside;
            float cov = 1.0f;
            if (aa_edges) {
                const float d0 = w0 * aa_il0, d1 = w1 * aa_il1, d2 = w2 * aa_il2;
                inside = (GLboolean)((aa_b0 ? d0 > -0.75f : GL_EDGE_IN(w0, tl0)) &&
                                     (aa_b1 ? d1 > -0.75f : GL_EDGE_IN(w1, tl1)) &&
                                     (aa_b2 ? d2 > -0.75f : GL_EDGE_IN(w2, tl2)));
                if (aa_b0)
                    cov *= (d0 + 0.5f > 1.0f) ? 1.0f
                                              : ((d0 + 0.5f > 0.0f) ? d0 + 0.5f : 0.0f);
                if (aa_b1)
                    cov *= (d1 + 0.5f > 1.0f) ? 1.0f
                                              : ((d1 + 0.5f > 0.0f) ? d1 + 0.5f : 0.0f);
                if (aa_b2)
                    cov *= (d2 + 0.5f > 1.0f) ? 1.0f
                                              : ((d2 + 0.5f > 0.0f) ? d2 + 0.5f : 0.0f);
            } else {
                inside = (GLboolean)(GL_EDGE_IN(w0, tl0) && GL_EDGE_IN(w1, tl1) &&
                                     GL_EDGE_IN(w2, tl2));
            }
            if (inside && aa_kind == GL_POINT) {
                const float ex = px - ctx->aa_c[0], ey = py - ctx->aa_c[1];
                const float c = ctx->aa_r + 0.5f - gl_sqrt(ex * ex + ey * ey);
                cov = (c > 1.0f) ? 1.0f : c;
            } else if (inside && aa_kind == GL_LINE) {
                const float ex = px - ctx->aa_a[0], ey = py - ctx->aa_a[1];
                const float along = ex * aa_dx + ey * aa_dy;
                const float across = ex * -aa_dy + ey * aa_dx;
                float c = ctx->aa_hw + 0.5f - (across < 0.0f ? -across : across);
                c = (c > 1.0f) ? 1.0f : c;
                if (ctx->aa_ends) {
                    const float m = (along < aa_len - along) ? along : aa_len - along;
                    const float ce = (m + 0.5f > 1.0f) ? 1.0f : m + 0.5f;
                    c *= (ce > 0.0f) ? ce : 0.0f;
                }
                cov = c;
            }
            if (!(cov > 0.0f))
                inside = GL_FALSE;

            if (inside) {
                float b0 = w0 * inv_area;
                float b1 = w1 * inv_area;
                float b2 = w2 * inv_area;
                if (area < 0.0f) {
                    b0 = -b0;
                    b1 = -b1;
                    b2 = -b2;
                }

                /* Perspective-correct weights for every attribute but depth. b0..b2
                 * are affine in screen space, right for window z and wrong for anything
                 * linear in clip space; weighting each vertex by its 1/w and
                 * renormalising corrects them, as the hardware interpolators do. */
                float pb0 = b0 * v0->inv_w, pb1 = b1 * v1->inv_w, pb2 = b2 * v2->inv_w;
                {
                    const float s = pb0 + pb1 + pb2;
                    if (s > 0.0f) {
                        const float inv_s = 1.0f / s;
                        pb0 *= inv_s;
                        pb1 *= inv_s;
                        pb2 *= inv_s;
                    } else {
                        pb0 = b0;
                        pb1 = b1;
                        pb2 = b2;
                    }
                }

                /* User clip planes, before anything touches a buffer: a clipped
                 * fragment writes neither colour nor depth. */
                if (clip_any) {
                    GLboolean clipped = GL_FALSE;
                    for (int ci = 0; ci < OOPS_GL_CLIP_PLANE_COUNT; ci++) {
                        if (!clip_on[ci])
                            continue;
                        if (pb0 * v0->cd[ci] + pb1 * v1->cd[ci] + pb2 * v2->cd[ci] <
                            0.0f) {
                            clipped = GL_TRUE;
                            break;
                        }
                    }
                    if (clipped)
                        continue;
                }

                /* Interpolate depth Z */
                float z = b0 * v0->sz + b1 * v1->sz + b2 * v2->sz;

                /* The fragment program runs ahead of the depth test, as the
                 * specification orders it: a shader may `discard` or write
                 * `gl_FragDepth`. */
                float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
                GLboolean shaded = GL_FALSE;
                if (prog_fs) {
                    float vc[GL_SHADER_VARY_FLOATS];
                    float vdx[GL_SHADER_VARY_FLOATS], vdy[GL_SHADER_VARY_FLOATS];
                    gl_shader_vary_at(v0, v1, v2, b0, b1, b2, have_vary_block, vc);
                    gl_shader_fragment_in_t in;
                    in.vary = vc;
                    in.vary_dx = (const float *)0;
                    in.vary_dy = (const float *)0;
                    if (needs_deriv) {
                        /* The neighbouring pixels' weights, from the barycentrics' own
                         * screen slopes - which the level of detail already uses. No
                         * second edge evaluation is needed: the weights are affine in x
                         * and y. */
                        gl_shader_vary_at(v0, v1, v2, b0 + dbw[0], b1 + dbw[2],
                                          b2 + dbw[4], have_vary_block, vdx);
                        gl_shader_vary_at(v0, v1, v2, b0 + dbw[1], b1 + dbw[3],
                                          b2 + dbw[5], have_vary_block, vdy);
                        in.vary_dx = vdx;
                        in.vary_dy = vdy;
                    }
                    /* `gl_FragCoord` is window coordinates with y counting up from
                     * the bottom, the opposite of this rasteriser's rows. Its w is
                     * 1/w_clip. */
                    in.frag_coord[0] = px;
                    in.frag_coord[1] = (float)ctx->height - py;
                    in.frag_coord[2] = z;
                    in.frag_coord[3] =
                        pb0 * v0->inv_w + pb1 * v1->inv_w + pb2 * v2->inv_w;
                    for (int k = 0; k < 4; k++) {
                        in.frag_coord_dx[k] = in.frag_coord[k];
                        in.frag_coord_dy[k] = in.frag_coord[k];
                    }
                    in.frag_coord_dx[0] += 1.0f;
                    in.frag_coord_dy[1] -= 1.0f;
                    in.front_facing = prim_front;

                    gl_shader_fragment_out_t fo;
                    if (!gl_shader_run_fragment(ctx, fprog, &in, &fo)) {
                        gl_record_error(ctx, GL_INVALID_OPERATION);
                        return;
                    }
                    /* `discard` writes nothing at all - no colour, no depth, no stencil
                     * - which is why it is a `continue` here and not a colour of zero.
                     */
                    if (fo.discarded)
                        continue;
                    if (fo.wrote_depth) {
                        z = fo.depth < 0.0f ? 0.0f
                                            : (fo.depth > 1.0f ? 1.0f : fo.depth);
                    }
                    r = fo.colour[0];
                    g = fo.colour[1];
                    b = fo.colour[2];
                    a = fo.colour[3];
                    shaded = GL_TRUE;
                }

                GLboolean depth_pending = GL_FALSE;

                /* Whether the depth test would reject this fragment. With stencil off
                 * this is acted on immediately below; with stencil on it has to be
                 * carried past the alpha test, because the depth result chooses between
                 * glStencilOp's second and third operations and none of them may run
                 * for a fragment alpha discards. */
                GLboolean depth_failed = GL_FALSE;

                /* Depth test */
                if (depth_test && db_row) {
                    if (!gl_depth_passes(depth_func, z, db_row[x])) {
                        /* With no stencil there is nothing further to decide. */
                        if (!stencil_test)
                            continue;
                        depth_failed = GL_TRUE;
                    }
                    /* The write is deferred past the alpha test (gl_fragment_tail):
                     * GL's order is alpha test, then depth, so a discarded fragment
                     * leaves the depth buffer alone. */
                    depth_pending = (GLboolean)depth_write;
                }

                /* Interpolate color */
                if (!shaded) {
                    r = pb0 * v0->r + pb1 * v1->r + pb2 * v2->r;
                    g = pb0 * v0->g + pb1 * v1->g + pb2 * v2->g;
                    b = pb0 * v0->b + pb1 * v1->b + pb2 * v2->b;
                    a = pb0 * v0->a + pb1 * v1->a + pb2 * v2->a;
                }

                /* Texturing, unit by unit (GL 1.3, 3.8.13): every applying unit
                 * sampled first - a GL 1.4 crossbar source can name any of them - then
                 * each unit's environment in turn on what the unit before it left. A
                 * unit applies only a complete texture; gl_unit_texture_id answers none
                 * for an incomplete one. */
                if (any_tex && !shaded) {
                    const float pb[3] = {pb0, pb1, pb2}, bs[3] = {b0, b1, b2};
                    gl_unit_texels_t texels;
                    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
                        if (ud[u].tex) {
                            gl_unit_sample(&ud[u], u, v0, v1, v2, pb, bs, dbw,
                                           texels.t[u]);
                        } else {
                            for (int k = 0; k < 4; k++)
                                texels.t[u][k] = 0.0f;
                        }
                    }
                    const float primary[4] = {r, g, b, a};
                    float frag[4] = {r, g, b, a};
                    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
                        if (ud[u].tex)
                            gl_tex_env_apply(ctx, u, ud[u].format, &texels, primary,
                                             frag);
                    }
                    r = frag[0];
                    g = frag[1];
                    b = frag[2];
                    a = frag[3];
                }

                /* The colour sum (GL 1.4, 3.9): the secondary colour - the specular
                 * term GL_SEPARATE_SPECULAR_COLOR kept apart, or unlit,
                 * glSecondaryColor's under GL_COLOR_SUM - added after texturing and
                 * before fog, clamped. */
                if (sec_on && !shaded) {
                    r += pb0 * v0->sr + pb1 * v1->sr + pb2 * v2->sr;
                    g += pb0 * v0->sg + pb1 * v1->sg + pb2 * v2->sg;
                    b += pb0 * v0->sb + pb1 * v1->sb + pb2 * v2->sb;
                    if (r > 1.0f)
                        r = 1.0f;
                    if (g > 1.0f)
                        g = 1.0f;
                    if (b > 1.0f)
                        b = 1.0f;
                }

                /* Fog: after texturing, before the alpha test, and not on alpha, as
                 * the specification orders it. A fragment shader replaces fog (GL 2.0):
                 * it applies its own from `gl_FogFragCoord` and `gl_Fog`. */
                if (fog_on && !shaded) {
                    const float ff = pb0 * v0->fog + pb1 * v1->fog + pb2 * v2->fog;
                    const float inv = 1.0f - ff;
                    r = ff * r + inv * fog_r;
                    g = ff * g + inv * fog_g;
                    b = ff * b + inv * fog_b;
                }

                /* Antialiasing's coverage, after fog and before the alpha test
                 * (GL 1.x, 3.12). */
                if (cov < 1.0f)
                    a *= cov;

                /* Alpha, stencil, the depth verdict, blending, the logic op and the
                 * masks. */
                gl_fragment_tail(ctx, &ops, fb_row ? &fb_row[x] : NULL,
                                 (depth_pending && db_row) ? &db_row[x] : NULL,
                                 (stencil_test && sb_row) ? &sb_row[x] : NULL, z,
                                 depth_failed, r, g, b, a);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Client Arrays Draw Dispatches
 * ------------------------------------------------------------------------- */

#endif /* OOPS_HOST_BUILD */

/* The bytes of one component of an array element, or 0 for a type no array takes. */
static size_t gl_array_type_bytes(GLenum type) {
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        return 1u;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
        return 2u;
    case GL_INT:
    case GL_UNSIGNED_INT:
    case GL_FLOAT:
        return 4u;
    case GL_DOUBLE:
        return 8u;
    default:
        return 0u;
    }
}

/* An array's element stride: its own, or the element's size when that is 0 (tightly
 * packed). */
static int gl_array_stride(const gl_client_array_t *a) {
    return a->stride ? a->stride : a->size * (int)gl_array_type_bytes(a->type);
}

/* Component `i` of an array element, as its type stores it. Colours and normals
 * convert as the immediate-mode calls of their type do - unsigned over 2^b - 1, signed
 * as (2c + 1) / (2^b - 1), Mesa's macros - and positions and texture coordinates as
 * plain values. Copied out rather than dereferenced, since an element need not be
 * aligned for its type. */
static float gl_array_comp(GLenum type, const uint8_t *p, int i, GLboolean normalize) {
    switch (type) {
    case GL_BYTE: {
        const GLbyte v = (GLbyte)p[i];
        return normalize ? gl_b_to_f(v) : (float)v;
    }
    case GL_UNSIGNED_BYTE:
        return normalize ? gl_ub_to_f(p[i]) : (float)p[i];
    case GL_SHORT: {
        GLshort v;
        memcpy(&v, p + (size_t)i * 2u, 2u);
        return normalize ? gl_s_to_f(v) : (float)v;
    }
    case GL_UNSIGNED_SHORT: {
        GLushort v;
        memcpy(&v, p + (size_t)i * 2u, 2u);
        return normalize ? gl_us_to_f(v) : (float)v;
    }
    case GL_INT: {
        GLint v;
        memcpy(&v, p + (size_t)i * 4u, 4u);
        return normalize ? gl_i_to_f(v) : (float)v;
    }
    case GL_UNSIGNED_INT: {
        GLuint v;
        memcpy(&v, p + (size_t)i * 4u, 4u);
        return normalize ? gl_ui_to_f(v) : (float)v;
    }
    case GL_DOUBLE: {
        GLdouble v;
        memcpy(&v, p + (size_t)i * 8u, 8u);
        return (float)v;
    }
    default: {
        GLfloat v;
        memcpy(&v, p + (size_t)i * 4u, 4u);
        return v;
    }
    }
}

/* One unit's texture coordinate array element, as s, t, r, q with GL's defaults for the
 * components the array's size leaves out - so a size-4 array's q is kept, not dropped.
 * False, and `tc` untouched, when the unit's array is not enabled. */
static GLboolean gl_array_texcoord_at(const gl_context_t *ctx, GLuint unit, int idx,
                                      float tc[4]) {
    const gl_client_array_t *a = &ctx->array_texcoord[unit];
    const uint8_t *base = gl_array_base(ctx, a);
    if (!a->enabled || !base)
        return GL_FALSE;
    const GLenum t = a->type;
    const int n = a->size;
    const uint8_t *ptr = base + (idx * gl_array_stride(a));
    tc[0] = gl_array_comp(t, ptr, 0, GL_FALSE);
    tc[1] = (n > 1) ? gl_array_comp(t, ptr, 1, GL_FALSE) : 0.0f;
    tc[2] = (n > 2) ? gl_array_comp(t, ptr, 2, GL_FALSE) : 0.0f;
    tc[3] = (n > 3) ? gl_array_comp(t, ptr, 3, GL_FALSE) : 1.0f;
    return GL_TRUE;
}

static void fetch_vertex(const gl_context_t *ctx, int idx, gl_vertex_t *out) {
    /* Each array's base comes from gl_array_base, not its `pointer` field: with a
     * buffer object bound that field is a byte offset, and offset 0 is legal. */
    const uint8_t *base_v = gl_array_base(ctx, &ctx->array_vertex);
    const uint8_t *base_c = gl_array_base(ctx, &ctx->array_color);
    const uint8_t *base_n = gl_array_base(ctx, &ctx->array_normal);

    /* Position */
    if (ctx->array_vertex.enabled && base_v) {
        const GLenum t = ctx->array_vertex.type;
        const int n = ctx->array_vertex.size;
        const uint8_t *ptr = base_v + (idx * gl_array_stride(&ctx->array_vertex));
        out->x = gl_array_comp(t, ptr, 0, GL_FALSE);
        out->y = (n > 1) ? gl_array_comp(t, ptr, 1, GL_FALSE) : 0.0f;
        out->z = (n > 2) ? gl_array_comp(t, ptr, 2, GL_FALSE) : 0.0f;
        out->w = (n > 3) ? gl_array_comp(t, ptr, 3, GL_FALSE) : 1.0f;
    } else {
        out->x = 0.0f;
        out->y = 0.0f;
        out->z = 0.0f;
        out->w = 1.0f;
    }

    /* Color */
    if (ctx->array_color.enabled && base_c) {
        const GLenum t = ctx->array_color.type;
        const uint8_t *ptr = base_c + (idx * gl_array_stride(&ctx->array_color));
        out->r = gl_array_comp(t, ptr, 0, GL_TRUE);
        out->g = gl_array_comp(t, ptr, 1, GL_TRUE);
        out->b = gl_array_comp(t, ptr, 2, GL_TRUE);
        out->a = (ctx->array_color.size > 3) ? gl_array_comp(t, ptr, 3, GL_TRUE) : 1.0f;
    } else {
        out->r = ctx->cur_color[0];
        out->g = ctx->cur_color[1];
        out->b = ctx->cur_color[2];
        out->a = ctx->cur_color[3];
    }

    /* The secondary colour, normalised as the colour array is */
    const uint8_t *base_s = gl_array_base(ctx, &ctx->array_secondary);
    if (ctx->array_secondary.enabled && base_s) {
        const GLenum t = ctx->array_secondary.type;
        const uint8_t *ptr = base_s + (idx * gl_array_stride(&ctx->array_secondary));
        out->sr = gl_array_comp(t, ptr, 0, GL_TRUE);
        out->sg = gl_array_comp(t, ptr, 1, GL_TRUE);
        out->sb = gl_array_comp(t, ptr, 2, GL_TRUE);
    } else {
        out->sr = ctx->cur_secondary[0];
        out->sg = ctx->cur_secondary[1];
        out->sb = ctx->cur_secondary[2];
    }

    /* The fog coordinate, a plain value */
    const uint8_t *base_f = gl_array_base(ctx, &ctx->array_fog_coord);
    if (ctx->array_fog_coord.enabled && base_f) {
        const uint8_t *ptr = base_f + (idx * gl_array_stride(&ctx->array_fog_coord));
        out->fogc = gl_array_comp(ctx->array_fog_coord.type, ptr, 0, GL_FALSE);
    } else {
        out->fogc = ctx->cur_fog_coord;
    }

    /* Texcoords: each unit's array element, or its current coordinate. Generated after
     * the normal is read, because sphere mapping needs it. */
    float tc[OOPS_GL_MAX_TEXTURE_UNITS][4];
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        if (!gl_array_texcoord_at(ctx, u, idx, tc[u])) {
            for (int k = 0; k < 4; k++)
                tc[u][k] = ctx->cur_texcoord[u][k];
        }
    }

    /* Normal, normalised as glNormal3b and its siblings convert */
    if (ctx->array_normal.enabled && base_n) {
        const GLenum t = ctx->array_normal.type;
        const uint8_t *ptr = base_n + (idx * gl_array_stride(&ctx->array_normal));
        out->nx = gl_array_comp(t, ptr, 0, GL_TRUE);
        out->ny = gl_array_comp(t, ptr, 1, GL_TRUE);
        out->nz = gl_array_comp(t, ptr, 2, GL_TRUE);
    } else {
        out->nx = ctx->cur_normal[0];
        out->ny = ctx->cur_normal[1];
        out->nz = ctx->cur_normal[2];
    }

    /* Generation applies to array vertices as to immediate-mode ones, overriding the
     * texture-coordinate array as it overrides glTexCoord. */
    gl_vertex_texcoord(ctx, out, tc);

    /* The edge flag: one GLboolean per element, or the current flag. */
    const uint8_t *base_e = gl_array_base(ctx, &ctx->array_edge_flag);
    if (ctx->array_edge_flag.enabled && base_e) {
        const int stride = ctx->array_edge_flag.stride ? ctx->array_edge_flag.stride
                                                       : (int)sizeof(GLboolean);
        out->edge = *(const GLboolean *)(base_e + (idx * stride)) ? GL_TRUE : GL_FALSE;
    } else {
        out->edge = ctx->cur_edge_flag;
    }

    /* GL 2.0's generic attribute arrays, one element per enabled slot, or the current
     * value where the slot's array is off. Stored on the vertex, not the context,
     * because a draw fetches every vertex before any is shaded (see
     * `gl_vertex_t::attrib`). Skipped until this context makes a shader or a program:
     * a fixed-function draw never reads them. */
    if (!ctx->gl2_used)
        return;
    for (int ai = 0; ai < OOPS_GL_MAX_VERTEX_ATTRIBS; ai++) {
        const gl_vertex_attrib_t *a = &ctx->vertex_attribs[ai];
        const uint8_t *base_a = (const uint8_t *)0;
        gl_client_array_t as;
        if (a->enabled) {
            as.size = a->size;
            as.type = a->type;
            as.stride = a->stride;
            as.pointer = a->pointer;
            as.enabled = GL_TRUE;
            as.buffer = a->buffer;
            base_a = gl_array_base(ctx, &as);
        }
        if (!base_a) {
            for (int k = 0; k < 4; k++)
                out->attrib[ai][k] = a->current[k];
            continue;
        }
        const uint8_t *ptr = base_a + (idx * gl_array_stride(&as));
        /* `normalized` is the caller's choice, not the type's, unlike the named arrays'
         * fixed per-attribute rule. A float array with GL_TRUE is not scaled. */
        const GLboolean norm =
            (GLboolean)(a->normalized && a->type != GL_FLOAT && a->type != GL_DOUBLE);
        out->attrib[ai][0] = gl_array_comp(a->type, ptr, 0, norm);
        out->attrib[ai][1] =
            (a->size > 1) ? gl_array_comp(a->type, ptr, 1, norm) : 0.0f;
        out->attrib[ai][2] =
            (a->size > 2) ? gl_array_comp(a->type, ptr, 2, norm) : 0.0f;
        out->attrib[ai][3] =
            (a->size > 3) ? gl_array_comp(a->type, ptr, 3, norm) : 1.0f;
    }
}

/* The two array-draw fetchers for gl_assemble: consecutive elements from `first`, and
 * elements named by an index array of one of the three readable types. */
static void gl_fetch_array(gl_context_t *ctx, const void *src, int i,
                           gl_vertex_t *out) {
    fetch_vertex(ctx, *(const GLint *)src + i, out);
}

typedef struct {
    const void *indices;
    GLenum type; /* checked readable by the caller before anything is fetched */
} gl_element_src_t;

static int gl_element_index(const gl_element_src_t *e, int i) {
    if (e->type == GL_UNSIGNED_SHORT)
        return (int)((const uint16_t *)e->indices)[i];
    if (e->type == GL_UNSIGNED_BYTE)
        return (int)((const uint8_t *)e->indices)[i];
    return (int)((const uint32_t *)e->indices)[i];
}

static void gl_fetch_element(gl_context_t *ctx, const void *src, int i,
                             gl_vertex_t *out) {
    fetch_vertex(ctx, gl_element_index((const gl_element_src_t *)src, i), out);
}

/* One array element as the immediate-mode calls the specification defines it to be -
 * the attribute calls for each enabled array, then the vertex - issued so that a
 * display list being compiled records them. This is how an array draw compiles: the
 * list keeps the values the arrays held at compile time.
 *
 * The texture coordinate is recorded as it sits in the array, before generation;
 * gl_vertex_texcoord applies generation when the list runs. */
static void gl_list_record_element(gl_context_t *ctx, int idx) {
    gl_vertex_t v;
    fetch_vertex(ctx, idx, &v);
    if (ctx->array_color.enabled && gl_array_base(ctx, &ctx->array_color)) {
        glColor4f(v.r, v.g, v.b, v.a);
    }
    if (ctx->array_secondary.enabled && gl_array_base(ctx, &ctx->array_secondary)) {
        glSecondaryColor3f(v.sr, v.sg, v.sb);
    }
    if (ctx->array_fog_coord.enabled && gl_array_base(ctx, &ctx->array_fog_coord)) {
        glFogCoordf(v.fogc);
    }
    if (ctx->array_normal.enabled && gl_array_base(ctx, &ctx->array_normal)) {
        glNormal3f(v.nx, v.ny, v.nz);
    }
    /* Each unit's array as the glMultiTexCoord call it stands for (GL 1.3, 2.8's
     * ArrayElement). */
    for (GLuint u = 0; u < OOPS_GL_MAX_TEXTURE_UNITS; u++) {
        float tc[4];
        if (gl_array_texcoord_at(ctx, u, idx, tc)) {
            glMultiTexCoord4f(GL_TEXTURE0 + u, tc[0], tc[1], tc[2], tc[3]);
        }
    }
    if (ctx->array_edge_flag.enabled && gl_array_base(ctx, &ctx->array_edge_flag)) {
        glEdgeFlag(v.edge);
    }
    const uint8_t *base_i = gl_array_base(ctx, &ctx->array_index);
    if (ctx->array_index.enabled && base_i) {
        const size_t bytes = gl_index_type_bytes(ctx->array_index.type);
        const int stride =
            ctx->array_index.stride ? ctx->array_index.stride : (int)bytes;
        const uint8_t *p = base_i + (idx * stride);
        switch (ctx->array_index.type) {
        case GL_UNSIGNED_BYTE:
            glIndexf((GLfloat)*p);
            break;
        case GL_SHORT:
            glIndexf((GLfloat) * (const GLshort *)p);
            break;
        case GL_INT:
            glIndexf((GLfloat) * (const GLint *)p);
            break;
        case GL_DOUBLE:
            glIndexf((GLfloat) * (const GLdouble *)p);
            break;
        default:
            glIndexf(*(const GLfloat *)p);
            break;
        }
    }
    if (ctx->array_vertex.enabled && gl_array_base(ctx, &ctx->array_vertex)) {
        glVertex4f(v.x, v.y, v.z, v.w);
    }
}

/* `glArrayElement(i)` - one vertex pulled out of the enabled arrays, inside
 * glBegin/glEnd.
 *
 * `fetch_vertex` reads the enabled arrays and falls back to the current colour, normal
 * and texture coordinate for the rest, as the specification defines. Inside a display
 * list it compiles as the attribute and vertex calls it stands for, read out of the
 * arrays now (gl_list_record_element).
 */
void glArrayElement(GLint i) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (i < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (gl_list_recording()) {
        gl_list_record_element(ctx, i);
        return;
    }
    /* Outside glBegin/glEnd this is undefined in GL, not an error; it does nothing. */
    if (!ctx->imm_active)
        return;
    if (ctx->imm_count >= OOPS_GL_MAX_IMMEDIATE_VERTS)
        return;

    fetch_vertex(ctx, i, &ctx->imm_verts[ctx->imm_count++]);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    /* A negative count is GL_INVALID_VALUE; a count of zero is legal and draws
     * nothing. */
    if (count < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!gl_accept_mode(ctx, mode))
        return;
    if (gl_draw_sources_mapped(ctx, GL_FALSE)) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }
    if (count == 0)
        return;

    /* Inside a display list, the draw compiles as what it is defined to be - glBegin,
     * one glArrayElement per index, glEnd - with the arrays read now. */
    if (gl_list_recording()) {
        glBegin(mode);
        for (GLint i = 0; i < count; i++)
            gl_list_record_element(ctx, first + i);
        glEnd();
        return;
    }

    const GLint src = first;
    gl_assemble(ctx, mode, count, gl_fetch_array, &src);
}

/* `glDrawRangeElements` (GL 1.2) is glDrawElements plus a promise about the index
 * range, which an implementation may use to pre-transform that slice of the arrays and
 * may ignore, as this one does. start > end is still GL_INVALID_VALUE, as required;
 * the indices themselves are not checked against the range. */
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count,
                         GLenum type, const GLvoid *indices) {
    gl_context_t *ctx = gl_get_ctx();
    if (ctx && start > end) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    glDrawElements(mode, count, type, indices);
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (count < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!gl_accept_mode(ctx, mode))
        return;
    /* Checked before `gl_element_index` runs: it treats any type other than 8- and
     * 16-bit as 32-bit. */
    if (!gl_index_type_is_readable(type)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (gl_draw_sources_mapped(ctx, GL_TRUE)) {
        gl_record_error(ctx, GL_INVALID_OPERATION);
        return;
    }

    /* With an element buffer bound, `indices` is a byte offset and zero is a real one,
     * so it is resolved before the null check. */
    if (ctx->bound_element_array_buffer != 0u) {
        const gl_client_array_t elements = {
            0, 0, 0, indices, GL_TRUE, ctx->bound_element_array_buffer};
        indices = gl_array_base(ctx, &elements);
        if (!indices) {
            /* Bound to a buffer with no storage. Nothing to read; the draw is skipped
             * rather than run against an address derived from NULL. */
            gl_record_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    if (count == 0 || !indices)
        return;

    const gl_element_src_t src = {indices, type};

    /* Inside a display list: glBegin, the indexed elements read now, glEnd - the
     * indices too, from the element buffer if one is bound, since a list keeps values
     * rather than names. */
    if (gl_list_recording()) {
        glBegin(mode);
        for (GLsizei i = 0; i < count; i++)
            gl_list_record_element(ctx, gl_element_index(&src, i));
        glEnd();
        return;
    }

    gl_assemble(ctx, mode, count, gl_fetch_element, &src);
}

/* GL 1.4's multi-draws: `drawcount` glDrawArrays or glDrawElements of one mode. Every
 * argument is checked before anything is drawn - a negative drawcount or count a
 * value error, the mode (and the index type) an enum error - as Mesa's validation does
 * (main/draw.c:530-547 and :303-320), so a bad count at the end does not leave the ones
 * before it drawn. Then each draw with a count above zero is issued, which is also how
 * a display list compiles them (Mesa vbo/vbo_save_api.c:1727-1760). */
void glMultiDrawArrays(GLenum mode, const GLint *first, const GLsizei *count,
                       GLsizei drawcount) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (drawcount < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!gl_accept_mode(ctx, mode))
        return;
    if (drawcount == 0 || !first || !count)
        return;
    for (GLsizei i = 0; i < drawcount; i++) {
        if (count[i] < 0) {
            gl_record_error(ctx, GL_INVALID_VALUE);
            return;
        }
    }
    for (GLsizei i = 0; i < drawcount; i++) {
        if (count[i] > 0)
            glDrawArrays(mode, first[i], count[i]);
    }
}

void glMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type,
                         const GLvoid *const *indices, GLsizei drawcount) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx)
        return;
    if (drawcount < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!gl_accept_mode(ctx, mode))
        return;
    if (!gl_index_type_is_readable(type)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }
    if (drawcount == 0 || !count || !indices)
        return;
    for (GLsizei i = 0; i < drawcount; i++) {
        if (count[i] < 0) {
            gl_record_error(ctx, GL_INVALID_VALUE);
            return;
        }
    }
    for (GLsizei i = 0; i < drawcount; i++) {
        if (count[i] > 0)
            glDrawElements(mode, count[i], type, indices[i]);
    }
}

/* GL_EXT_draw_range_elements' and GL_EXT_multi_draw_arrays' own spellings, which a
 * program written before GL 1.2 and 1.4 took them into the core calls. */
void glDrawRangeElementsEXT(GLenum mode, GLuint start, GLuint end, GLsizei count,
                            GLenum type, const GLvoid *indices) {
    glDrawRangeElements(mode, start, end, count, type, indices);
}
void glMultiDrawArraysEXT(GLenum mode, const GLint *first, const GLsizei *count,
                          GLsizei drawcount) {
    glMultiDrawArrays(mode, first, count, drawcount);
}
void glMultiDrawElementsEXT(GLenum mode, const GLsizei *count, GLenum type,
                            const GLvoid *const *indices, GLsizei drawcount) {
    glMultiDrawElements(mode, count, type, indices, drawcount);
}
