/*
 * oops-gl: Vertex arrays, immediate mode, primitive processing, and rasterization
 */

#include "gl_internal.h"

/* -------------------------------------------------------------------------
 * What this subset will draw, and how it refuses the rest
 *
 * Everything here reaches the hardware as triangles, so the four modes that triangulate are
 * the four that work. GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_LINE_LOOP, GL_QUAD_STRIP and
 * GL_POLYGON are declared in <GL/gl.h> and are not implemented.
 *
 * **That is now a measured refusal rather than an unmeasured one.** obSCEne's
 * `166-agc/primitive-draw-point-line` submitted both on retail hardware across four
 * independent sweeps on 2026-09-16 (`20260916-211030`, `-211622`, `-213216`, `-221711`), a
 * point with `m0 = 0x1001` (1 primitive, 1 vertex) and a line with `m0 = 0x1002` (1
 * primitive, 2 vertices), with `VGT_GS_OUT_PRIM_TYPE` 0 and 1 respectively. Every run
 * recorded `fence-hit 0` and `pixel-hit 0`: the end-of-pipe fence kept its `0x11111111`
 * sentinel, so the submission never completed at all - it is not that the primitives drew
 * wrongly, it is that the pipe stopped.
 *
 * The contrast in the same section is what makes it legible. `166-agc/primitive-draw` uses
 * the same path with `m0 = 0x1003` - **three** vertices - and its fence retires and one pixel
 * lands. So that geometry engine wanted three vertices per primitive, and one or two left it
 * waiting.
 *
 * **And it is not a property of the fixture's stage configuration.** That was the open
 * question: obSCEne's fixture programmes `VGT_SHADER_STAGES_EN` (`0x2d5`) to `0x02002000`,
 * which it reads as passthrough, where oops-gl programmes `0x00c12010` - so the stall might
 * have been something only their configuration did. Re-run with **oops-gl's own value**
 * (`REQ-...5a3e`, sweep `20260917-010918`, rows recording `vgt-shader-stages-en 0xc12010`),
 * both the point and the line stall exactly as before: `fence-hit 0`, `pixel-hit 0`, the
 * target still holding its background, `res fail`.
 *
 * So the refusal is settled on a measurement of the configuration this code actually uses,
 * rather than on an inference from somebody else's. Points and lines need a stage this does
 * not build - not a different register value - and that is a larger piece of work than the
 * rest of the 1.x surface (D008). They stay refused, and the refusal is now the correct
 * answer rather than a cautious one.
 *
 * They used to reach a `default: break;` in three separate switches, which drew nothing, set
 * no error, and left glGetError() answering GL_NO_ERROR. A caller had no way to tell that
 * from a successful draw of a degenerate mesh - it is the plausible-output failure OOPS
 * conventions section 3 forbids, and it is worse in a measuring instrument than anywhere
 * else, because a record with nothing in it still looks like a record.
 *
 * So the check lives here, once, and every entry point that takes a mode calls it before
 * doing anything. An unsupported mode is GL_INVALID_ENUM, which is the nearest thing GL has
 * to "this implementation will not do that" - there is no GL_UNSUPPORTED, and staying silent
 * is not an option.
 * ------------------------------------------------------------------------- */

static GLboolean gl_mode_is_drawable(GLenum mode) {
    switch (mode) {
        case GL_TRIANGLES:
        case GL_TRIANGLE_STRIP:
        case GL_TRIANGLE_FAN:
        case GL_QUADS:
        case GL_QUAD_STRIP:
        case GL_POLYGON:
        /* Points and lines reach the hardware as triangles too - see the expansion above glEnd.
         * They were refused here until 2026-09-17 on the strength of a measurement that closed
         * the *native* one- and two-vertex primitive, which is a different thing. */
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
    if (gl_mode_is_drawable(mode)) return GL_TRUE;
    gl_record_error(ctx, GL_INVALID_ENUM);
    return GL_FALSE;
}

/* The index types glDrawElements can read.
 *
 * Checked rather than assumed, because the reader below selects 16-bit and 8-bit explicitly
 * and treats *everything else* as 32-bit. An unchecked type therefore did not merely give a
 * wrong answer: passing GL_FLOAT, or GL_UNSIGNED_INT's neighbour by a typo, read four bytes
 * per index out of an array the caller had sized for one, running off the end of it. */
static GLboolean gl_index_type_is_readable(GLenum type) {
    return (GLboolean)(type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT ||
                       type == GL_UNSIGNED_INT);
}

/* -------------------------------------------------------------------------
 * Client-Side Vertex Arrays
 * ------------------------------------------------------------------------- */

void glEnableClientState(GLenum array) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    switch (array) {
        case GL_VERTEX_ARRAY:         ctx->array_vertex.enabled = GL_TRUE; break;
        case GL_COLOR_ARRAY:          ctx->array_color.enabled = GL_TRUE; break;
        case GL_NORMAL_ARRAY:         ctx->array_normal.enabled = GL_TRUE; break;
        case GL_TEXTURE_COORD_ARRAY:  ctx->array_texcoord.enabled = GL_TRUE; break;
        /* Same reasoning as glEnable: an array this subset does not keep is refused rather
         * than dropped, so a caller asking for one finds out from glGetError() instead of
         * from geometry that renders without it. */
        default: gl_record_error(ctx, GL_INVALID_ENUM); break;
    }
}

void glDisableClientState(GLenum array) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    switch (array) {
        case GL_VERTEX_ARRAY:         ctx->array_vertex.enabled = GL_FALSE; break;
        case GL_COLOR_ARRAY:          ctx->array_color.enabled = GL_FALSE; break;
        case GL_NORMAL_ARRAY:         ctx->array_normal.enabled = GL_FALSE; break;
        case GL_TEXTURE_COORD_ARRAY:  ctx->array_texcoord.enabled = GL_FALSE; break;
        default: gl_record_error(ctx, GL_INVALID_ENUM); break;
    }
}

/* **The buffer bound *now* is the one this array reads from later.** GL ties the array to the
 * GL_ARRAY_BUFFER binding at the moment the pointer is specified, not at the moment of the
 * draw - so a program that binds a buffer, sets four pointers, then binds a different buffer
 * and sets a fifth ends up with arrays reading from two buffers at once, which is exactly what
 * it meant. Capturing the name here is what makes that work. */
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->array_vertex.size = size;
    ctx->array_vertex.type = type;
    ctx->array_vertex.stride = stride;
    ctx->array_vertex.pointer = pointer;
    ctx->array_vertex.buffer = ctx->bound_array_buffer;
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->array_color.size = size;
    ctx->array_color.type = type;
    ctx->array_color.stride = stride;
    ctx->array_color.pointer = pointer;
    ctx->array_color.buffer = ctx->bound_array_buffer;
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->array_texcoord.size = size;
    ctx->array_texcoord.type = type;
    ctx->array_texcoord.stride = stride;
    ctx->array_texcoord.pointer = pointer;
    ctx->array_texcoord.buffer = ctx->bound_array_buffer;
}

void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->array_normal.size = 3;
    ctx->array_normal.type = type;
    ctx->array_normal.stride = stride;
    ctx->array_normal.pointer = pointer;
    ctx->array_normal.buffer = ctx->bound_array_buffer;
}

/* `glInterleavedArrays(format, stride, pointer)` - all four arrays from one buffer.
 *
 * The specification defines it as exactly the glEnableClientState / glDisableClientState and
 * pointer calls a program would otherwise write out by hand, so that is what it does: there is
 * nothing here the draw path does not already read, and nothing new to get wrong at draw time.
 *
 * **The arrays a format does not name are disabled, not left alone.** That is the part worth
 * being deliberate about: a program that sets up GL_C3F_V3F after having had a normal array
 * enabled would otherwise keep reading normals out of the old pointer, which is a stale read
 * into a buffer that may well be gone. The specification says to disable them; this does.
 *
 * `stride` of 0 means "the format's own size", which is the packed case.
 */
void glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;

    /* Component counts, in the order they appear in the buffer: texture coordinates first,
     * then colour, then normal, then position - which is what the format names spell out
     * backwards. A count of 0 means the format does not carry that array. */
    int tc = 0, cc = 0, nc = 0, vc = 0;
    GLboolean colour_is_bytes = GL_FALSE;

    switch (format) {
        case GL_V2F:             vc = 2; break;
        case GL_V3F:             vc = 3; break;
        case GL_C4UB_V2F:        cc = 4; colour_is_bytes = GL_TRUE; vc = 2; break;
        case GL_C4UB_V3F:        cc = 4; colour_is_bytes = GL_TRUE; vc = 3; break;
        case GL_C3F_V3F:         cc = 3; vc = 3; break;
        case GL_N3F_V3F:         nc = 3; vc = 3; break;
        case GL_C4F_N3F_V3F:     cc = 4; nc = 3; vc = 3; break;
        case GL_T2F_V3F:         tc = 2; vc = 3; break;
        case GL_T4F_V4F:         tc = 4; vc = 4; break;
        case GL_T2F_C4UB_V3F:    tc = 2; cc = 4; colour_is_bytes = GL_TRUE; vc = 3; break;
        case GL_T2F_C3F_V3F:     tc = 2; cc = 3; vc = 3; break;
        case GL_T2F_N3F_V3F:     tc = 2; nc = 3; vc = 3; break;
        case GL_T2F_C4F_N3F_V3F: tc = 2; cc = 4; nc = 3; vc = 3; break;
        case GL_T4F_C4F_N3F_V4F: tc = 4; cc = 4; nc = 3; vc = 4; break;
        default:
            gl_record_error(ctx, GL_INVALID_ENUM);
            return;
    }
    if (stride < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }

    /* The four unsigned bytes of a C4UB colour occupy **four bytes rounded up to a float**,
     * because the specification aligns what follows them to a float boundary. With a 4-byte
     * GLfloat the two are the same number, so nothing here can currently observe the
     * difference - the rounding is written out anyway rather than the coincidence relied on.
     *
     * Every offset below was checked against Mesa's own table
     * (`src/mesa/main/varray.c`, `_mesa_get_interleaved_layout`), which computes
     * `c = f * ((4 * sizeof(GLubyte) + (f - 1)) / f)` and lists each format's offsets and
     * default stride explicitly. All fourteen agree. */
    const size_t f = sizeof(GLfloat);
    const size_t ub4 = f * ((4u * sizeof(GLubyte) + (f - 1u)) / f);
    const size_t colour_bytes = colour_is_bytes ? ub4 : (size_t)cc * f;

    const size_t off_t = 0u;
    const size_t off_c = off_t + (size_t)tc * f;
    /* `colour_bytes` is already zero when the format has no colour - `colour_is_bytes` is only
     * set in the C4UB cases, where `cc` is 4 - so no guard is needed here. */
    const size_t off_n = off_c + colour_bytes;
    const size_t off_v = off_n + (size_t)nc * f;
    const size_t packed = off_v + (size_t)vc * f;

    const GLsizei real_stride = stride ? stride : (GLsizei)packed;
    const uint8_t *base = (const uint8_t *)pointer;

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
    {
        GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (gl_list_capture(GL_LIST_OP_BEGIN, mode, 0, 0, f)) return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* Refused here rather than at glEnd, so the vertices in between are never collected and
     * the error is attributable to the call that caused it. `imm_active` stays false, which
     * makes glVertex*() and glEnd() no-ops for this block. */
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
    if (v) glVertex4f(v[0], v[1], v[2], 1.0f);
}

void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    {
        GLfloat f[4] = {x, y, z, w};
        if (gl_list_capture(GL_LIST_OP_VERTEX, 0, 0, 0, f)) return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !ctx->imm_active) return;
    if (ctx->imm_count >= OOPS_GL_MAX_IMMEDIATE_VERTS) return;

    gl_vertex_t *v = &ctx->imm_verts[ctx->imm_count++];
    v->x = (float)x;
    v->y = (float)y;
    v->z = (float)z;
    v->w = (float)w;
    v->r = ctx->cur_color[0];
    v->g = ctx->cur_color[1];
    v->b = ctx->cur_color[2];
    v->a = ctx->cur_color[3];
    v->u = ctx->cur_texcoord[0];
    v->v = ctx->cur_texcoord[1];
    v->nx = ctx->cur_normal[0];
    v->ny = ctx->cur_normal[1];
    v->nz = ctx->cur_normal[2];
    gl_apply_texgen(ctx, v);
}

/* Texture coordinate generation, computed here because this is where the object coordinates and
 * the object-space normal are both in hand and the modelview is still the one that belongs to
 * them. Doing it later would need all three carried forward.
 *
 * Only the coordinates whose generation is enabled are replaced; the rest keep what glTexCoord
 * left, which is what lets a program generate s and supply t by hand.
 *
 * **Only s and t reach the sampler.** The texture unit here is 2D, so r and q are generated into
 * the current texture coordinate for glGetFloatv and the display list to see, and go no further.
 * That is a real limit and it is the same one glTexCoord4 has. */
/* PA_CL_UCP_0_X and the 23 registers after it - six planes of four floats, at context offset
 * 0x16F (byte 0x0285BC, mesa/src/amd/registers/gfx103.json). Consecutive, so one SET_CONTEXT_REG
 * carries the lot. A plane whose enable is off is still written, as zero: the enable bit in
 * PA_CL_CLIP_CNTL is what decides, and leaving stale values behind a disabled bit is how a plane
 * comes back to life when something else enables it later. */
void gl_hw_emit_clip_planes(const gl_context_t *ctx, uint32_t **dw_ptr) {
    uint32_t *dw = *dw_ptr;
    *dw++ = 0xc0186900u; /* PACKET3_SET_CONTEXT_REG, 24 data dwords */
    *dw++ = 0x16fu;      /* mmPA_CL_UCP_0_X */
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        float p[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (ctx->clip_plane_enabled[i]) {
            (void)gl_compute_clip_plane_hw(ctx, i, p);
        }
        for (int k = 0; k < 4; k++) *dw++ = gl_f32_bits(p[k]);
    }
    *dw_ptr = dw;
}

GLboolean gl_compute_clip_plane_hw(const gl_context_t *ctx, int i, float out[4]) {
    if (!ctx || !out || i < 0 || i >= OOPS_GL_CLIP_PLANE_COUNT) return GL_FALSE;
    gl_mat4_t pinv;
    if (!mat4_invert(&pinv, &ctx->projection_stack[ctx->projection_depth])) return GL_FALSE;
    const float *p = ctx->clip_plane[i];
    /* A plane is a row vector: p_clip = p_eye * P^-1. */
    for (int k = 0; k < 4; k++) {
        out[k] = p[0] * pinv.m[k * 4 + 0] + p[1] * pinv.m[k * 4 + 1] +
                 p[2] * pinv.m[k * 4 + 2] + p[3] * pinv.m[k * 4 + 3];
    }
    return GL_TRUE;
}

void gl_apply_texgen(const gl_context_t *ctx, gl_vertex_t *v) {
    if (!ctx || !v) return;
    if (!ctx->texgen_enabled[0] && !ctx->texgen_enabled[1] &&
        !ctx->texgen_enabled[2] && !ctx->texgen_enabled[3]) {
        return;
    }

    const float obj[4] = {v->x, v->y, v->z, v->w};
    const gl_mat4_t *mv = &ctx->modelview_stack[ctx->modelview_depth];

    float eye[4];
    mat4_transform_vec4(eye, mv, obj);

    /* The eye-space normal, for sphere mapping. The upper 3x3 of the modelview is used directly
     * rather than its inverse-transpose: the two agree for the rotations and translations a
     * sphere map is used with, and the reflection below renormalises anyway. */
    float ne[3] = {
        mv->m[0] * v->nx + mv->m[4] * v->ny + mv->m[8]  * v->nz,
        mv->m[1] * v->nx + mv->m[5] * v->ny + mv->m[9]  * v->nz,
        mv->m[2] * v->nx + mv->m[6] * v->ny + mv->m[10] * v->nz,
    };
    float nlen = gl_sqrt(ne[0] * ne[0] + ne[1] * ne[1] + ne[2] * ne[2]);
    if (nlen > 0.0f) { ne[0] /= nlen; ne[1] /= nlen; ne[2] /= nlen; }

    /* Sphere mapping, computed once and only if something asks for it. The eye vector points
     * from the eye to the vertex; u is that normalised, r is u reflected about the normal, and
     * the coordinate is r scaled into [0,1] by the sphere's own radius term. */
    float sphere_s = 0.0f, sphere_t = 0.0f;
    const GLboolean wants_sphere =
        (GLboolean)((ctx->texgen_enabled[0] && ctx->texgen_mode[0] == GL_SPHERE_MAP) ||
                    (ctx->texgen_enabled[1] && ctx->texgen_mode[1] == GL_SPHERE_MAP));
    if (wants_sphere) {
        float u[3] = {eye[0], eye[1], eye[2]};
        float ulen = gl_sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (ulen > 0.0f) { u[0] /= ulen; u[1] /= ulen; u[2] /= ulen; }
        const float d = 2.0f * (u[0] * ne[0] + u[1] * ne[1] + u[2] * ne[2]);
        const float r0 = u[0] - ne[0] * d;
        const float r1 = u[1] - ne[1] * d;
        const float r2 = u[2] - ne[2] * d;
        const float mterm = 2.0f * gl_sqrt(r0 * r0 + r1 * r1 + (r2 + 1.0f) * (r2 + 1.0f));
        if (mterm != 0.0f) {
            sphere_s = r0 / mterm + 0.5f;
            sphere_t = r1 / mterm + 0.5f;
        } else {
            /* The degenerate case is the pole directly behind the eye, where the sphere map has
             * no defined coordinate. The centre is the least surprising answer. */
            sphere_s = 0.5f;
            sphere_t = 0.5f;
        }
    }

    float gen[4] = {ctx->cur_texcoord[0], ctx->cur_texcoord[1],
                    ctx->cur_texcoord[2], ctx->cur_texcoord[3]};
    for (int i = 0; i < 4; i++) {
        if (!ctx->texgen_enabled[i]) continue;
        switch (ctx->texgen_mode[i]) {
            case GL_OBJECT_LINEAR: {
                const float *p = ctx->texgen_object_plane[i];
                gen[i] = p[0] * obj[0] + p[1] * obj[1] + p[2] * obj[2] + p[3] * obj[3];
                break;
            }
            case GL_EYE_LINEAR: {
                /* The plane was already put through the inverse modelview when it was set. */
                const float *p = ctx->texgen_eye_plane[i];
                gen[i] = p[0] * eye[0] + p[1] * eye[1] + p[2] * eye[2] + p[3] * eye[3];
                break;
            }
            case GL_SPHERE_MAP:
                gen[i] = (i == 0) ? sphere_s : sphere_t;
                break;
            default:
                break;
        }
    }

    v->u = gen[0];
    v->v = gen[1];
}

void glColor3f(GLfloat red, GLfloat green, GLfloat blue) {
    glColor4f(red, green, blue, 1.0f);
}

void glColor3fv(const GLfloat *v) {
    if (v) glColor4f(v[0], v[1], v[2], 1.0f);
}

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    {
        GLfloat f[4] = {red, green, blue, alpha};
        if (gl_list_capture(GL_LIST_OP_COLOR, 0, 0, 0, f)) return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->cur_color[0] = (float)red;
    ctx->cur_color[1] = (float)green;
    ctx->cur_color[2] = (float)blue;
    ctx->cur_color[3] = (float)alpha;
}

void glColor4fv(const GLfloat *v) {
    if (v) glColor4f(v[0], v[1], v[2], v[3]);
}

/* Integer colours and normals are *normalised*: signed types map onto [-1, 1] and unsigned onto
 * [0, 1], each so the extreme value lands exactly on the endpoint. **255 divides, it does not
 * shift** - dividing by 256 would leave full-brightness white one step short of white, which is
 * invisible in isolation and wrong in a blend.
 *
 * From Mesa rather than from memory (mesa/src/mesa/main/macros.h:49-104), including the
 * asymmetric divisor for GLint: the unsigned form divides by 2^32-1 and the signed form by
 * 2^32-2. Positions and texture coordinates use none of this - they are the value as given. */
static inline GLfloat gl_b_to_f(GLbyte v)    { return (GLfloat)((2.0 * (double)v + 1.0) / 255.0); }
static inline GLfloat gl_s_to_f(GLshort v)   { return (GLfloat)((2.0 * (double)v + 1.0) / 65535.0); }
static inline GLfloat gl_i_to_f(GLint v)     { return (GLfloat)((2.0 * (double)v + 1.0) / 4294967294.0); }
static inline GLfloat gl_ub_to_f(GLubyte v)  { return (GLfloat)((double)v / 255.0); }
static inline GLfloat gl_us_to_f(GLushort v) { return (GLfloat)((double)v / 65535.0); }
static inline GLfloat gl_ui_to_f(GLuint v)   { return (GLfloat)((double)v / 4294967295.0); }

void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) {
    glColor4f(gl_ub_to_f(red), gl_ub_to_f(green), gl_ub_to_f(blue), gl_ub_to_f(alpha));
}

/* The four-component form is the one implementation; every other arity and type forwards here,
 * so a display list records one operation and the spellings cannot drift apart.
 *
 * **q is a projective divide, not a fourth coordinate to ignore.** The specification says the
 * texture is sampled at (s/q, t/q, r/q), so dividing here is what makes glTexCoord4 mean what it
 * says at each vertex. What this does *not* do is carry q through the rasteriser and divide per
 * fragment, so a primitive whose vertices carry different q values interpolates the already
 * divided coordinates - exact wherever q is constant, which includes every q = 1 call, and an
 * affine approximation where it is not. That divergence is recorded in the roadmap rather than
 * left for someone to find in a texture that skews.
 *
 * A q of zero is left alone rather than dividing: the specification does not define it, and
 * producing an infinity here would poison the vertex instead of just this coordinate. */
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
    {
        GLfloat f[4] = {s, t, r, q};
        if (gl_list_capture(GL_LIST_OP_TEXCOORD, 0, 0, 0, f)) return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (q != 0.0f && q != 1.0f) {
        ctx->cur_texcoord[0] = (float)(s / q);
        ctx->cur_texcoord[1] = (float)(t / q);
        ctx->cur_texcoord[2] = (float)(r / q);
    } else {
        ctx->cur_texcoord[0] = (float)s;
        ctx->cur_texcoord[1] = (float)t;
        ctx->cur_texcoord[2] = (float)r;
    }
    ctx->cur_texcoord[3] = (float)q;
}

void glTexCoord2f(GLfloat s, GLfloat t) { glTexCoord4f(s, t, 0.0f, 1.0f); }
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) { glTexCoord4f(s, t, r, 1.0f); }

void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
    {
        GLfloat f[4] = {nx, ny, nz, 0.0f};
        if (gl_list_capture(GL_LIST_OP_NORMAL, 0, 0, 0, f)) return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->cur_normal[0] = (float)nx;
    ctx->cur_normal[1] = (float)ny;
    ctx->cur_normal[2] = (float)nz;
}

void glNormal3fv(const GLfloat *v) {
    if (v) glNormal3f(v[0], v[1], v[2]);
}

/* -------------------------------------------------------------------------
 * The other spellings
 *
 * GL 1.x names each attribute once per C type and once per arity. Every one of these converts
 * and forwards to the `f` sibling above, so each attribute has exactly one implementation and
 * the spellings cannot drift apart - which also means they are all recorded into a display list
 * by the one `gl_list_capture` the sibling already has.
 *
 * A null vector pointer draws nothing rather than being dereferenced, matching the `fv` forms.
 * ------------------------------------------------------------------------- */

void glVertex2d(GLdouble x, GLdouble y) { glVertex4f((GLfloat)x, (GLfloat)y, 0.0f, 1.0f); }
void glVertex3d(GLdouble x, GLdouble y, GLdouble z) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, 1.0f);
}
void glVertex4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}
void glVertex2i(GLint x, GLint y) { glVertex4f((GLfloat)x, (GLfloat)y, 0.0f, 1.0f); }
void glVertex3i(GLint x, GLint y, GLint z) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, 1.0f);
}
void glVertex2fv(const GLfloat *v) { if (v) glVertex4f(v[0], v[1], 0.0f, 1.0f); }
void glVertex4fv(const GLfloat *v) { if (v) glVertex4f(v[0], v[1], v[2], v[3]); }
void glVertex2dv(const GLdouble *v) { if (v) glVertex2d(v[0], v[1]); }
void glVertex3dv(const GLdouble *v) { if (v) glVertex3d(v[0], v[1], v[2]); }
void glVertex2iv(const GLint *v) { if (v) glVertex2i(v[0], v[1]); }
void glVertex3iv(const GLint *v) { if (v) glVertex3i(v[0], v[1], v[2]); }

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
void glColor3dv(const GLdouble *v) { if (v) glColor3d(v[0], v[1], v[2]); }
void glColor4dv(const GLdouble *v) { if (v) glColor4d(v[0], v[1], v[2], v[3]); }
void glColor3ubv(const GLubyte *v) { if (v) glColor4ub(v[0], v[1], v[2], 255); }
void glColor4ubv(const GLubyte *v) { if (v) glColor4ub(v[0], v[1], v[2], v[3]); }

/* The one-coordinate form leaves t at 0, which is what the specification says and not simply an
 * omission: a 1D-style lookup into a 2D texture wants row zero. */
void glTexCoord1f(GLfloat s) { glTexCoord2f(s, 0.0f); }
void glTexCoord2d(GLdouble s, GLdouble t) { glTexCoord2f((GLfloat)s, (GLfloat)t); }
void glTexCoord2i(GLint s, GLint t) { glTexCoord2f((GLfloat)s, (GLfloat)t); }
void glTexCoord2fv(const GLfloat *v) { if (v) glTexCoord2f(v[0], v[1]); }
void glTexCoord2dv(const GLdouble *v) { if (v) glTexCoord2d(v[0], v[1]); }
void glTexCoord2iv(const GLint *v) { if (v) glTexCoord2i(v[0], v[1]); }

void glNormal3d(GLdouble nx, GLdouble ny, GLdouble nz) {
    glNormal3f((GLfloat)nx, (GLfloat)ny, (GLfloat)nz);
}
void glNormal3dv(const GLdouble *v) { if (v) glNormal3d(v[0], v[1], v[2]); }

/* -------------------------------------------------------------------------
 * The rest of the type-and-arity grid
 *
 * **These are not cosmetic.** The payload links with `-Wl,--unresolved-symbols=ignore-all`, so a
 * program calling a spelling that does not exist here links cleanly and calls address zero at
 * run time - a SIGSEGV at `rip: 0x0000000000000000` with no build diagnostic and nothing in the
 * log naming the symbol. An absent spelling is not a missing convenience, it is a crash with the
 * evidence removed, which is why the grid is worth filling even though every entry is a
 * forwarder.
 *
 * **Integer colours and normals are normalised; positions and texture coordinates are not.**
 * glVertex3i(1,2,3) is the point (1,2,3), but glColor3i(1,2,3) is indistinguishable from black
 * and glNormal3b(127,0,0) is the x axis. Getting that backwards produces a picture that is
 * merely wrong rather than an error, so the conversions come from Mesa rather than from memory:
 * mesa/src/mesa/main/macros.h:49-104, and normals use the same macros as colours
 * (mesa/src/mesa/vbo/vbo_attrib_tmp.h:2770-2795) rather than a plain cast.
 *
 * One consequence is worth stating because it looks like a bug: the signed mapping is
 * (2c+1)/(2^b - 1), so **zero does not map to zero** - glNormal3b(127,0,0) is
 * (1.0, 1/255, 1/255), not (1, 0, 0). That is what the mapping has to do to put both -128 and
 * 127 exactly on -1 and 1, it is what Mesa produces, and it is why Mesa keeps a separate
 * BYTE_TO_FLOATZ for the cases that need an exact zero. Neither colours nor normals are such a
 * case.
 * ------------------------------------------------------------------------- */

/* Positions: the value, unconverted. */
void glVertex2s(GLshort x, GLshort y) { glVertex4f((GLfloat)x, (GLfloat)y, 0.0f, 1.0f); }
void glVertex3s(GLshort x, GLshort y, GLshort z) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, 1.0f);
}
void glVertex4s(GLshort x, GLshort y, GLshort z, GLshort w) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}
void glVertex4i(GLint x, GLint y, GLint z, GLint w) {
    glVertex4f((GLfloat)x, (GLfloat)y, (GLfloat)z, (GLfloat)w);
}
void glVertex2sv(const GLshort *v)  { if (v) glVertex2s(v[0], v[1]); }
void glVertex3sv(const GLshort *v)  { if (v) glVertex3s(v[0], v[1], v[2]); }
void glVertex4sv(const GLshort *v)  { if (v) glVertex4s(v[0], v[1], v[2], v[3]); }
void glVertex4iv(const GLint *v)    { if (v) glVertex4i(v[0], v[1], v[2], v[3]); }
void glVertex4dv(const GLdouble *v) { if (v) glVertex4d(v[0], v[1], v[2], v[3]); }

/* Colours: normalised. The three-component forms set alpha to 1.0 exactly - **not** the
 * converted maximum of the argument type, which is the same number only by coincidence. */
void glColor3b(GLbyte r, GLbyte g, GLbyte b) { glColor4f(gl_b_to_f(r), gl_b_to_f(g), gl_b_to_f(b), 1.0f); }
void glColor3s(GLshort r, GLshort g, GLshort b) { glColor4f(gl_s_to_f(r), gl_s_to_f(g), gl_s_to_f(b), 1.0f); }
void glColor3i(GLint r, GLint g, GLint b) { glColor4f(gl_i_to_f(r), gl_i_to_f(g), gl_i_to_f(b), 1.0f); }
void glColor3us(GLushort r, GLushort g, GLushort b) { glColor4f(gl_us_to_f(r), gl_us_to_f(g), gl_us_to_f(b), 1.0f); }
void glColor3ui(GLuint r, GLuint g, GLuint b) { glColor4f(gl_ui_to_f(r), gl_ui_to_f(g), gl_ui_to_f(b), 1.0f); }
void glColor4b(GLbyte r, GLbyte g, GLbyte b, GLbyte a) { glColor4f(gl_b_to_f(r), gl_b_to_f(g), gl_b_to_f(b), gl_b_to_f(a)); }
void glColor4s(GLshort r, GLshort g, GLshort b, GLshort a) { glColor4f(gl_s_to_f(r), gl_s_to_f(g), gl_s_to_f(b), gl_s_to_f(a)); }
void glColor4i(GLint r, GLint g, GLint b, GLint a) { glColor4f(gl_i_to_f(r), gl_i_to_f(g), gl_i_to_f(b), gl_i_to_f(a)); }
void glColor4us(GLushort r, GLushort g, GLushort b, GLushort a) { glColor4f(gl_us_to_f(r), gl_us_to_f(g), gl_us_to_f(b), gl_us_to_f(a)); }
void glColor4ui(GLuint r, GLuint g, GLuint b, GLuint a) { glColor4f(gl_ui_to_f(r), gl_ui_to_f(g), gl_ui_to_f(b), gl_ui_to_f(a)); }
void glColor3bv(const GLbyte *v)    { if (v) glColor3b(v[0], v[1], v[2]); }
void glColor3sv(const GLshort *v)   { if (v) glColor3s(v[0], v[1], v[2]); }
void glColor3iv(const GLint *v)     { if (v) glColor3i(v[0], v[1], v[2]); }
void glColor3usv(const GLushort *v) { if (v) glColor3us(v[0], v[1], v[2]); }
void glColor3uiv(const GLuint *v)   { if (v) glColor3ui(v[0], v[1], v[2]); }
void glColor4bv(const GLbyte *v)    { if (v) glColor4b(v[0], v[1], v[2], v[3]); }
void glColor4sv(const GLshort *v)   { if (v) glColor4s(v[0], v[1], v[2], v[3]); }
void glColor4iv(const GLint *v)     { if (v) glColor4i(v[0], v[1], v[2], v[3]); }
void glColor4usv(const GLushort *v) { if (v) glColor4us(v[0], v[1], v[2], v[3]); }
void glColor4uiv(const GLuint *v)   { if (v) glColor4ui(v[0], v[1], v[2], v[3]); }

/* Normals: normalised, like colours and unlike positions. */
void glNormal3b(GLbyte nx, GLbyte ny, GLbyte nz) { glNormal3f(gl_b_to_f(nx), gl_b_to_f(ny), gl_b_to_f(nz)); }
void glNormal3s(GLshort nx, GLshort ny, GLshort nz) { glNormal3f(gl_s_to_f(nx), gl_s_to_f(ny), gl_s_to_f(nz)); }
void glNormal3i(GLint nx, GLint ny, GLint nz) { glNormal3f(gl_i_to_f(nx), gl_i_to_f(ny), gl_i_to_f(nz)); }
void glNormal3bv(const GLbyte *v)  { if (v) glNormal3b(v[0], v[1], v[2]); }
void glNormal3sv(const GLshort *v) { if (v) glNormal3s(v[0], v[1], v[2]); }
void glNormal3iv(const GLint *v)   { if (v) glNormal3i(v[0], v[1], v[2]); }

/* Texture coordinates: the value, unconverted. The unspecified components follow the
 * specification's defaults - t and r zero, q one - rather than being left at whatever the
 * previous call set, which is the difference between glTexCoord1f resetting a coordinate and
 * silently inheriting half of the last one. */
void glTexCoord1d(GLdouble s) { glTexCoord4f((GLfloat)s, 0.0f, 0.0f, 1.0f); }
void glTexCoord1i(GLint s)    { glTexCoord4f((GLfloat)s, 0.0f, 0.0f, 1.0f); }
void glTexCoord1s(GLshort s)  { glTexCoord4f((GLfloat)s, 0.0f, 0.0f, 1.0f); }
void glTexCoord2s(GLshort s, GLshort t) { glTexCoord4f((GLfloat)s, (GLfloat)t, 0.0f, 1.0f); }
void glTexCoord3d(GLdouble s, GLdouble t, GLdouble r) { glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, 1.0f); }
void glTexCoord3i(GLint s, GLint t, GLint r) { glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, 1.0f); }
void glTexCoord3s(GLshort s, GLshort t, GLshort r) { glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, 1.0f); }
void glTexCoord4d(GLdouble s, GLdouble t, GLdouble r, GLdouble q) {
    glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, (GLfloat)q);
}
void glTexCoord4i(GLint s, GLint t, GLint r, GLint q) {
    glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, (GLfloat)q);
}
void glTexCoord4s(GLshort s, GLshort t, GLshort r, GLshort q) {
    glTexCoord4f((GLfloat)s, (GLfloat)t, (GLfloat)r, (GLfloat)q);
}
void glTexCoord1fv(const GLfloat *v)  { if (v) glTexCoord1f(v[0]); }
void glTexCoord1dv(const GLdouble *v) { if (v) glTexCoord1d(v[0]); }
void glTexCoord1iv(const GLint *v)    { if (v) glTexCoord1i(v[0]); }
void glTexCoord1sv(const GLshort *v)  { if (v) glTexCoord1s(v[0]); }
void glTexCoord2sv(const GLshort *v)  { if (v) glTexCoord2s(v[0], v[1]); }
void glTexCoord3fv(const GLfloat *v)  { if (v) glTexCoord3f(v[0], v[1], v[2]); }
void glTexCoord3dv(const GLdouble *v) { if (v) glTexCoord3d(v[0], v[1], v[2]); }
void glTexCoord3iv(const GLint *v)    { if (v) glTexCoord3i(v[0], v[1], v[2]); }
void glTexCoord3sv(const GLshort *v)  { if (v) glTexCoord3s(v[0], v[1], v[2]); }
void glTexCoord4fv(const GLfloat *v)  { if (v) glTexCoord4f(v[0], v[1], v[2], v[3]); }
void glTexCoord4dv(const GLdouble *v) { if (v) glTexCoord4d(v[0], v[1], v[2], v[3]); }
void glTexCoord4iv(const GLint *v)    { if (v) glTexCoord4i(v[0], v[1], v[2], v[3]); }
void glTexCoord4sv(const GLshort *v)  { if (v) glTexCoord4s(v[0], v[1], v[2], v[3]); }


/* -------------------------------------------------------------------------
 * Multitexture, on an implementation with one texture unit
 *
 * GL_TEXTURE0 forwards to the single-unit call of the same shape. **Any higher unit is
 * GL_INVALID_ENUM**, which is not a refusal this port invented: a unit at or above
 * GL_MAX_TEXTURE_UNITS is an invalid enum by the specification, and this reports that maximum as
 * 1. The alternative - accepting GL_TEXTURE1 and quietly applying unit 0's coordinate - would
 * draw a picture that is wrong in a way no error code mentions.
 *
 * A second unit needs a third parameter export from the vertex shader, which is an unmeasured
 * hardware fact (roadmap, and obSCEne REQ-20260917T1652Z-7c40). When that lands, the range check
 * here is the only thing that moves.
 * ------------------------------------------------------------------------- */

/* Refuses and returns false for a unit this implementation does not have. */
static GLboolean gl_mt_unit_ok(GLenum target) {
    if (target == GL_TEXTURE0) return GL_TRUE;
    gl_context_t *ctx = gl_get_ctx();
    if (ctx) gl_record_error(ctx, GL_INVALID_ENUM);
    return GL_FALSE;
}

void glActiveTexture(GLenum texture) { (void)gl_mt_unit_ok(texture); }
void glClientActiveTexture(GLenum texture) { (void)gl_mt_unit_ok(texture); }
void glMultiTexCoord1f(GLenum target, GLfloat s) { if (gl_mt_unit_ok(target)) glTexCoord1f(s); }
void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t) { if (gl_mt_unit_ok(target)) glTexCoord2f(s, t); }
void glMultiTexCoord3f(GLenum target, GLfloat s, GLfloat t, GLfloat r) { if (gl_mt_unit_ok(target)) glTexCoord3f(s, t, r); }
void glMultiTexCoord4f(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { if (gl_mt_unit_ok(target)) glTexCoord4f(s, t, r, q); }
void glMultiTexCoord1d(GLenum target, GLdouble s) { if (gl_mt_unit_ok(target)) glTexCoord1d(s); }
void glMultiTexCoord2d(GLenum target, GLdouble s, GLdouble t) { if (gl_mt_unit_ok(target)) glTexCoord2d(s, t); }
void glMultiTexCoord3d(GLenum target, GLdouble s, GLdouble t, GLdouble r) { if (gl_mt_unit_ok(target)) glTexCoord3d(s, t, r); }
void glMultiTexCoord4d(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q) { if (gl_mt_unit_ok(target)) glTexCoord4d(s, t, r, q); }
void glMultiTexCoord1i(GLenum target, GLint s) { if (gl_mt_unit_ok(target)) glTexCoord1i(s); }
void glMultiTexCoord2i(GLenum target, GLint s, GLint t) { if (gl_mt_unit_ok(target)) glTexCoord2i(s, t); }
void glMultiTexCoord3i(GLenum target, GLint s, GLint t, GLint r) { if (gl_mt_unit_ok(target)) glTexCoord3i(s, t, r); }
void glMultiTexCoord4i(GLenum target, GLint s, GLint t, GLint r, GLint q) { if (gl_mt_unit_ok(target)) glTexCoord4i(s, t, r, q); }
void glMultiTexCoord1s(GLenum target, GLshort s) { if (gl_mt_unit_ok(target)) glTexCoord1s(s); }
void glMultiTexCoord2s(GLenum target, GLshort s, GLshort t) { if (gl_mt_unit_ok(target)) glTexCoord2s(s, t); }
void glMultiTexCoord3s(GLenum target, GLshort s, GLshort t, GLshort r) { if (gl_mt_unit_ok(target)) glTexCoord3s(s, t, r); }
void glMultiTexCoord4s(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q) { if (gl_mt_unit_ok(target)) glTexCoord4s(s, t, r, q); }
void glMultiTexCoord1fv(GLenum target, const GLfloat *v) { if (gl_mt_unit_ok(target)) glTexCoord1fv(v); }
void glMultiTexCoord2fv(GLenum target, const GLfloat *v) { if (gl_mt_unit_ok(target)) glTexCoord2fv(v); }
void glMultiTexCoord3fv(GLenum target, const GLfloat *v) { if (gl_mt_unit_ok(target)) glTexCoord3fv(v); }
void glMultiTexCoord4fv(GLenum target, const GLfloat *v) { if (gl_mt_unit_ok(target)) glTexCoord4fv(v); }
void glMultiTexCoord1dv(GLenum target, const GLdouble *v) { if (gl_mt_unit_ok(target)) glTexCoord1dv(v); }
void glMultiTexCoord2dv(GLenum target, const GLdouble *v) { if (gl_mt_unit_ok(target)) glTexCoord2dv(v); }
void glMultiTexCoord3dv(GLenum target, const GLdouble *v) { if (gl_mt_unit_ok(target)) glTexCoord3dv(v); }
void glMultiTexCoord4dv(GLenum target, const GLdouble *v) { if (gl_mt_unit_ok(target)) glTexCoord4dv(v); }
void glMultiTexCoord1iv(GLenum target, const GLint *v) { if (gl_mt_unit_ok(target)) glTexCoord1iv(v); }
void glMultiTexCoord2iv(GLenum target, const GLint *v) { if (gl_mt_unit_ok(target)) glTexCoord2iv(v); }
void glMultiTexCoord3iv(GLenum target, const GLint *v) { if (gl_mt_unit_ok(target)) glTexCoord3iv(v); }
void glMultiTexCoord4iv(GLenum target, const GLint *v) { if (gl_mt_unit_ok(target)) glTexCoord4iv(v); }
void glMultiTexCoord1sv(GLenum target, const GLshort *v) { if (gl_mt_unit_ok(target)) glTexCoord1sv(v); }
void glMultiTexCoord2sv(GLenum target, const GLshort *v) { if (gl_mt_unit_ok(target)) glTexCoord2sv(v); }
void glMultiTexCoord3sv(GLenum target, const GLshort *v) { if (gl_mt_unit_ok(target)) glTexCoord3sv(v); }
void glMultiTexCoord4sv(GLenum target, const GLshort *v) { if (gl_mt_unit_ok(target)) glTexCoord4sv(v); }

/* The ARB spellings: the same functions under their extension names. */
void glActiveTextureARB(GLenum texture) { glActiveTexture(texture); }
void glClientActiveTextureARB(GLenum texture) { glClientActiveTexture(texture); }
void glMultiTexCoord1fARB(GLenum target, GLfloat s) { glMultiTexCoord1f(target, s); }
void glMultiTexCoord1fvARB(GLenum target, const GLfloat *v) { glMultiTexCoord1fv(target, v); }
void glMultiTexCoord2fARB(GLenum target, GLfloat s, GLfloat t) { glMultiTexCoord2f(target, s, t); }
void glMultiTexCoord2fvARB(GLenum target, const GLfloat *v) { glMultiTexCoord2fv(target, v); }
void glMultiTexCoord3fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r) { glMultiTexCoord3f(target, s, t, r); }
void glMultiTexCoord3fvARB(GLenum target, const GLfloat *v) { glMultiTexCoord3fv(target, v); }
void glMultiTexCoord4fARB(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { glMultiTexCoord4f(target, s, t, r, q); }
void glMultiTexCoord4fvARB(GLenum target, const GLfloat *v) { glMultiTexCoord4fv(target, v); }
void glMultiTexCoord1dARB(GLenum target, GLdouble s) { glMultiTexCoord1d(target, s); }
void glMultiTexCoord1dvARB(GLenum target, const GLdouble *v) { glMultiTexCoord1dv(target, v); }
void glMultiTexCoord2dARB(GLenum target, GLdouble s, GLdouble t) { glMultiTexCoord2d(target, s, t); }
void glMultiTexCoord2dvARB(GLenum target, const GLdouble *v) { glMultiTexCoord2dv(target, v); }
void glMultiTexCoord3dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r) { glMultiTexCoord3d(target, s, t, r); }
void glMultiTexCoord3dvARB(GLenum target, const GLdouble *v) { glMultiTexCoord3dv(target, v); }
void glMultiTexCoord4dARB(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q) { glMultiTexCoord4d(target, s, t, r, q); }
void glMultiTexCoord4dvARB(GLenum target, const GLdouble *v) { glMultiTexCoord4dv(target, v); }
void glMultiTexCoord1iARB(GLenum target, GLint s) { glMultiTexCoord1i(target, s); }
void glMultiTexCoord1ivARB(GLenum target, const GLint *v) { glMultiTexCoord1iv(target, v); }
void glMultiTexCoord2iARB(GLenum target, GLint s, GLint t) { glMultiTexCoord2i(target, s, t); }
void glMultiTexCoord2ivARB(GLenum target, const GLint *v) { glMultiTexCoord2iv(target, v); }
void glMultiTexCoord3iARB(GLenum target, GLint s, GLint t, GLint r) { glMultiTexCoord3i(target, s, t, r); }
void glMultiTexCoord3ivARB(GLenum target, const GLint *v) { glMultiTexCoord3iv(target, v); }
void glMultiTexCoord4iARB(GLenum target, GLint s, GLint t, GLint r, GLint q) { glMultiTexCoord4i(target, s, t, r, q); }
void glMultiTexCoord4ivARB(GLenum target, const GLint *v) { glMultiTexCoord4iv(target, v); }
void glMultiTexCoord1sARB(GLenum target, GLshort s) { glMultiTexCoord1s(target, s); }
void glMultiTexCoord1svARB(GLenum target, const GLshort *v) { glMultiTexCoord1sv(target, v); }
void glMultiTexCoord2sARB(GLenum target, GLshort s, GLshort t) { glMultiTexCoord2s(target, s, t); }
void glMultiTexCoord2svARB(GLenum target, const GLshort *v) { glMultiTexCoord2sv(target, v); }
void glMultiTexCoord3sARB(GLenum target, GLshort s, GLshort t, GLshort r) { glMultiTexCoord3s(target, s, t, r); }
void glMultiTexCoord3svARB(GLenum target, const GLshort *v) { glMultiTexCoord3sv(target, v); }
void glMultiTexCoord4sARB(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q) { glMultiTexCoord4s(target, s, t, r, q); }
void glMultiTexCoord4svARB(GLenum target, const GLshort *v) { glMultiTexCoord4sv(target, v); }

/* -------------------------------------------------------------------------
 * Points and lines, expanded to triangles
 *
 * The geometry engine will not accept a one- or two-vertex primitive: five obSCEne sweeps, one of
 * them on this stage, all recorded `fence-hit 0` - the pipe stops rather than drawing wrongly.
 * What that closed is the *native* primitive. A line of a given screen width is a quad and a point
 * is a square, and this file already turns `GL_QUADS` into triangles, so nothing new is asked of
 * the hardware.
 *
 * **The width is in pixels, so the expansion has to happen after projection.** A perpendicular
 * offset applied in object or eye space gives a line that thickens and thins with distance, which
 * is a different thing from what glLineWidth means. So each endpoint is transformed to normalised
 * device coordinates, offset there by the half-width converted into NDC through the viewport, and
 * carried **back to object space through the inverse of the combined matrix** - because the
 * pipeline below is going to apply that matrix again. `mat4_invert` exists for the clip planes and
 * does this too.
 *
 * A vertex behind the eye is dropped rather than expanded. Clipping a line properly means
 * splitting it at the near plane, which is a clipper this does not have; dropping is visible and
 * wrong in the same way the near-plane guard above it already is, rather than silently producing a
 * quad that wraps around the viewer.
 * ------------------------------------------------------------------------- */

/* Projects an object-space position, returning false when it cannot be used.
 *
 * `out_ndc` carries z as well as x and y: the expanded corners have to keep the endpoint's depth
 * or a wide line sinks through whatever it crosses. */
static GLboolean gl_project_ndc(const gl_context_t *ctx, const gl_vertex_t *v,
                                float out_ndc[3], float *out_w) {
    const float obj[4] = {v->x, v->y, v->z, v->w};
    float clip[4];
    mat4_transform_vec4(clip, &ctx->mvp, obj);
    if (clip[3] <= 0.0001f) return GL_FALSE;
    const float inv_w = 1.0f / clip[3];
    out_ndc[0] = clip[0] * inv_w;
    out_ndc[1] = clip[1] * inv_w;
    out_ndc[2] = clip[2] * inv_w;
    *out_w = clip[3];
    return GL_TRUE;
}

/* Builds an object-space vertex at an NDC position, keeping colour, normal and texture coordinate
 * from `src`. `w` is the endpoint's own clip w, so the corner sits in the plane the endpoint does,
 * and `ndc_z` is its depth. */
static gl_vertex_t gl_vertex_at_ndc(const gl_mat4_t *inv_mvp, const gl_vertex_t *src,
                                    float ndc_x, float ndc_y, float ndc_z, float w) {
    gl_vertex_t out = *src;
    /* Back through the projection: clip = ndc * w, then object = MVP^-1 * clip, because the
     * pipeline below applies MVP again. */
    const float clip[4] = {ndc_x * w, ndc_y * w, ndc_z * w, w};
    float obj[4];
    mat4_transform_vec4(obj, inv_mvp, clip);
    out.x = obj[0];
    out.y = obj[1];
    out.z = obj[2];
    out.w = obj[3];
    return out;
}

/* One line segment as two triangles. */
static void gl_draw_line_segment(gl_context_t *ctx, const gl_mat4_t *inv_mvp,
                                 const gl_vertex_t *a, const gl_vertex_t *b) {
    float na[3], nb[3], wa, wb;
    if (!gl_project_ndc(ctx, a, na, &wa)) return;
    if (!gl_project_ndc(ctx, b, nb, &wb)) return;

    /* Into pixels, so the perpendicular is perpendicular on screen rather than in NDC - the two
     * differ whenever the viewport is not square, which is almost always. */
    const float hx = (float)ctx->vp_w * 0.5f;
    const float hy = (float)ctx->vp_h * 0.5f;
    float dx = (nb[0] - na[0]) * hx;
    float dy = (nb[1] - na[1]) * hy;
    const float len = gl_sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) return; /* a zero-length segment has no direction to be perpendicular to */
    dx /= len;
    dy /= len;

    const float half = (ctx->line_width > 0.0f ? ctx->line_width : 1.0f) * 0.5f;
    /* Perpendicular in pixels, then back into NDC through the same viewport scale. */
    const float ox = (-dy * half) / hx;
    const float oy = (dx * half) / hy;

    const gl_vertex_t a0 = gl_vertex_at_ndc(inv_mvp, a, na[0] + ox, na[1] + oy, na[2], wa);
    const gl_vertex_t a1 = gl_vertex_at_ndc(inv_mvp, a, na[0] - ox, na[1] - oy, na[2], wa);
    const gl_vertex_t b0 = gl_vertex_at_ndc(inv_mvp, b, nb[0] + ox, nb[1] + oy, nb[2], wb);
    const gl_vertex_t b1 = gl_vertex_at_ndc(inv_mvp, b, nb[0] - ox, nb[1] - oy, nb[2], wb);

    gl_draw_primitive_triangle(ctx, &a0, &a1, &b1);
    gl_draw_primitive_triangle(ctx, &a0, &b1, &b0);
}

/* One point as a screen-aligned square of `glPointSize` pixels. */
static void gl_draw_point_square(gl_context_t *ctx, const gl_mat4_t *inv_mvp,
                                 const gl_vertex_t *p) {
    float np[3], w;
    if (!gl_project_ndc(ctx, p, np, &w)) return;

    const float half = (ctx->point_size > 0.0f ? ctx->point_size : 1.0f) * 0.5f;
    const float ox = half / ((float)ctx->vp_w * 0.5f);
    const float oy = half / ((float)ctx->vp_h * 0.5f);

    const gl_vertex_t c0 = gl_vertex_at_ndc(inv_mvp, p, np[0] - ox, np[1] - oy, np[2], w);
    const gl_vertex_t c1 = gl_vertex_at_ndc(inv_mvp, p, np[0] + ox, np[1] - oy, np[2], w);
    const gl_vertex_t c2 = gl_vertex_at_ndc(inv_mvp, p, np[0] + ox, np[1] + oy, np[2], w);
    const gl_vertex_t c3 = gl_vertex_at_ndc(inv_mvp, p, np[0] - ox, np[1] + oy, np[2], w);

    gl_draw_primitive_triangle(ctx, &c0, &c1, &c2);
    gl_draw_primitive_triangle(ctx, &c0, &c2, &c3);
}

void glEnd(void) {
    {
        GLfloat f[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        if (gl_list_capture(GL_LIST_OP_END, 0, 0, 0, f)) return;
    }
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !ctx->imm_active) return;
    ctx->imm_active = GL_FALSE;

    int n = ctx->imm_count;
    const gl_vertex_t *v = ctx->imm_verts;

    switch (ctx->imm_mode) {
        case GL_TRIANGLES:
            for (int i = 0; i + 2 < n; i += 3) {
                gl_draw_primitive_triangle(ctx, &v[i], &v[i + 1], &v[i + 2]);
            }
            break;
        case GL_QUADS:
            for (int i = 0; i + 3 < n; i += 4) {
                /* Quad as two triangles: (0, 1, 2) and (0, 2, 3) */
                gl_draw_primitive_triangle(ctx, &v[i], &v[i + 1], &v[i + 2]);
                gl_draw_primitive_triangle(ctx, &v[i], &v[i + 2], &v[i + 3]);
            }
            break;
        case GL_TRIANGLE_STRIP:
            for (int i = 0; i + 2 < n; i++) {
                if (i & 1) {
                    gl_draw_primitive_triangle(ctx, &v[i + 1], &v[i], &v[i + 2]);
                } else {
                    gl_draw_primitive_triangle(ctx, &v[i], &v[i + 1], &v[i + 2]);
                }
            }
            break;
        case GL_TRIANGLE_FAN:
            for (int i = 1; i + 1 < n; i++) {
                gl_draw_primitive_triangle(ctx, &v[0], &v[i], &v[i + 1]);
            }
            break;
        /* **A polygon is a fan.** GL requires it to be convex and planar, and a fan from vertex
         * zero is the triangulation that gives - the same one GL_QUADS already gets. */
        case GL_POLYGON:
            for (int i = 1; i + 1 < n; i++) {
                gl_draw_primitive_triangle(ctx, &v[0], &v[i], &v[i + 1]);
            }
            break;
        /* A quad strip's quads share an edge, and the winding alternates the way a triangle
         * strip's does - taking them in declaration order would flip every other one. */
        case GL_QUAD_STRIP:
            for (int i = 0; i + 3 < n; i += 2) {
                gl_draw_primitive_triangle(ctx, &v[i], &v[i + 1], &v[i + 3]);
                gl_draw_primitive_triangle(ctx, &v[i], &v[i + 3], &v[i + 2]);
            }
            break;
        case GL_POINTS:
        case GL_LINES:
        case GL_LINE_STRIP:
        case GL_LINE_LOOP: {
            /* The inverse is computed once for the whole primitive rather than per segment, and
             * a modelview or projection that cannot be inverted draws nothing - which is what a
             * matrix that collapses the scene to a plane should do. */
            gl_update_mvp(ctx);
            gl_mat4_t inv_mvp;
            if (!mat4_invert(&inv_mvp, &ctx->mvp)) break;
            if (ctx->imm_mode == GL_POINTS) {
                for (int i = 0; i < n; i++) gl_draw_point_square(ctx, &inv_mvp, &v[i]);
            } else if (ctx->imm_mode == GL_LINES) {
                for (int i = 0; i + 1 < n; i += 2) {
                    gl_draw_line_segment(ctx, &inv_mvp, &v[i], &v[i + 1]);
                }
            } else {
                for (int i = 0; i + 1 < n; i++) {
                    gl_draw_line_segment(ctx, &inv_mvp, &v[i], &v[i + 1]);
                }
                /* The loop closes; the strip does not. */
                if (ctx->imm_mode == GL_LINE_LOOP && n > 2) {
                    gl_draw_line_segment(ctx, &inv_mvp, &v[n - 1], &v[0]);
                }
            }
            break;
        }
        default: break;
    }

    ctx->imm_count = 0;
}

/* -------------------------------------------------------------------------
 * glRect*
 *
 * A screen-aligned rectangle in the z=0 plane, drawn with the current colour, normal and
 * texture coordinate. The specification defines it as exactly this sequence:
 *
 *     glBegin(GL_POLYGON);
 *     glVertex2(x1, y1); glVertex2(x2, y1); glVertex2(x2, y2); glVertex2(x1, y2);
 *     glEnd();
 *
 * **GL_POLYGON is not a primitive this accepts, and GL_QUADS is.** For four vertices those two
 * are not merely similar: GL_POLYGON triangulates a convex polygon as a fan from vertex 0, and
 * GL_QUADS splits a quad into (0,1,2) and (0,2,3), which for n=4 is the same fan. A rectangle is
 * always convex, so the substitution is exact rather than an approximation - and the vertex
 * order above is the winding the specification gives, so face culling sees what it should.
 *
 * Written on top of glBegin/glVertex/glEnd rather than reaching into the immediate-mode buffer,
 * which is what makes it compile into a display list correctly: those three capture, so a
 * glRectf inside glNewList records the four vertices, which is what the specification says a
 * compiled glRect is.
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

/* The vector forms take *two* corners in two arrays, not four scalars in one - the mistake to
 * avoid is reading v1[2] and v1[3] for the second corner. */
void glRectfv(const GLfloat *v1, const GLfloat *v2) {
    if (v1 && v2) glRectf(v1[0], v1[1], v2[0], v2[1]);
}

void glRectdv(const GLdouble *v1, const GLdouble *v2) {
    if (v1 && v2) glRectd(v1[0], v1[1], v2[0], v2[1]);
}

void glRectiv(const GLint *v1, const GLint *v2) {
    if (v1 && v2) glRecti(v1[0], v1[1], v2[0], v2[1]);
}

void glRectsv(const GLshort *v1, const GLshort *v2) {
    if (v1 && v2) glRects(v1[0], v1[1], v2[0], v2[1]);
}

/* -------------------------------------------------------------------------
 * Triangle Pipeline & Rasterizer
 * ------------------------------------------------------------------------- */


/* The depth block: surface, extent and bases. Emitted the first time a frame draws with the
 * depth test on, so a frame that never tests depth carries exactly the measured no-depth recipe.
 * The register list and order are the ones oops_agc_draw_primitive carries (AgcCompositor.elf). */
static void gl_hw_emit_depth_block(gl_context_t *ctx, uint32_t **dw_ptr) {
    uint32_t *dw = *dw_ptr;
    uint64_t depth_gpu = (uint64_t)(uintptr_t)ctx->depth_buffer;
    uint32_t w = ctx->width ? ctx->width : 1920;
    uint32_t h = ctx->height ? ctx->height : 1080;
    uint32_t z_info = OOPS_AGC_Z_32_FLOAT | 0x80000180u; /* Z_32_FLOAT | SW_MODE=24 | ZRANGE_PRECISION */

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

static void gl_hw_begin_frame(gl_context_t *ctx) {
    uint32_t *dw = ctx->dcb_mem;
    uint64_t color_gpu = (uint64_t)(uintptr_t)ctx->framebuffer;

    /* An experiment's prelude (glSetHardwarePrelude) goes first, ahead of every register this
     * frame sets, so whatever it programs is what this frame's own state is written over. */
    if (ctx->hw_prelude && ctx->hw_prelude_words) {
        for (uint32_t i = 0; i < ctx->hw_prelude_words; i++) *dw++ = ctx->hw_prelude[i];
    }
    uint64_t payload_va = (uint64_t)(uintptr_t)ctx->gpu_payload;

    uint32_t w = ctx->width ? ctx->width : 1920;
    uint32_t h = ctx->height ? ctx->height : 1080;
    uint32_t scissor_br = ((h & 0x7fffu) << 16) | (w & 0x7fffu);

    /* The static state is the recipe measured to put pixels on the screen (obSCEne
     * 166-agc/primitive-draw, the same one oops_agc_draw_primitive carries): NGG in passthrough
     * mode, no depth. Entries marked (patched) take the frame's own values below. The GL-facing
     * delta is the two parameter exports (colour, texcoord) the pixel shaders interpolate:
     * SPI_VS_OUT_CONFIG, SPI_PS_IN_CONTROL and the first two SPI_PS_INPUT_CNTL slots. */
    static const struct {
        uint32_t reg;
        uint32_t val;
    } ctx_regs[] = {
        {0x318u, 0},           /* CB_COLOR0_BASE (patched) */
        {0x390u, 0},           /* CB_COLOR0_BASE_EXT (patched) */
        {0x31bu, 0x00000000u}, /* CB_COLOR0_VIEW */
        /* CB_COLOR0_INFO: COLOR_8_8_8_8, LINEAR_GENERAL, UNORM, COMP_SWAP=ALT (bytes B,G,R,A:
         * the 0xAARRGGBB framebuffer), BLEND_CLAMP set.
         *
         * BLEND_BYPASS (bit 16) was set here until 2026-09-17, carried in from the measured
         * primitive-draw recipe, and it is the reason blending did nothing on hardware however
         * correct CB_BLEND0_CONTROL was: the bypass is read before the blender, so gl1-probe's
         * `blend` and `blend-equation` failed identically across three runs while clear, rect
         * and depth passed. Mesa derives the two bits together and never sets this combination -
         * mesa/src/amd/common/ac_descriptors.c:1426-1438 sets blend_clamp for NORM/SRGB and
         * blend_bypass *only* for UINT/SINT or the 8_24/24_8/X24_8_32_FLOAT formats, clearing
         * blend_clamp when it does. An 8_8_8_8 UNORM target gets clamp=1, bypass=0. */
        {0x31cu, 0x000088a8u},
        {0x31du, 0x00000000u}, /* CB_COLOR0_ATTRIB */
        {0x31eu, 0x00000000u}, /* CB_COLOR0_DCC_CONTROL: disabled */
        {0x3b0u, 0},           /* CB_COLOR0_ATTRIB2 (patched: extent) */
        {0x3b8u, 0x08c00000u}, /* CB_COLOR0_ATTRIB3: COLOR_SW_MODE=LINEAR (the measured 0x08c6c000 carries 64KB_R_X, the compositor's tiled surface, and streaked our linear scratch buffer) */
        {0x109u, 0x00000000u}, /* CB_DCC_CONTROL: disabled */
        {0x202u, 0x00cc0010u}, /* CB_COLOR_CONTROL: CB_NORMAL, ROP3_COPY */
        {0x08eu, 0x0000000fu}, /* CB_TARGET_MASK (patched) */
        {0x08fu, 0x0000000fu}, /* CB_SHADER_MASK: MRT0 four components */
        {0x1e0u, 0x00000000u}, /* CB_BLEND0_CONTROL (patched) */
        {0x200u, 0x00000000u}, /* DB_DEPTH_CONTROL (patched) */
        {0x201u, 0x00010000u}, /* DB_EQAA */
        {0x203u, 0x00000010u}, /* DB_SHADER_CONTROL: EARLY_Z_THEN_LATE_Z */
        {0x08cu, 0xaa99aaaau}, /* PA_SC_EDGERULE */
        {0x1d4u, 0x000000ffu}, /* SX_PS_DOWNCONVERT_CONTROL */
        {0x291u, 0x20040100u}, /* VGT_GS_ONCHIP_CNTL: ES_VERTS=256, GS_PRIMS=128, GS_INST_PRIMS=128 (subgroup sizing as radeonsi programs it) */
        /* TRISTRIP, and there is now a measured negative behind that choice rather than only
         * the positive one in the gl-cube oracle record. obSCEne's `166-agc/primitive-draw`
         * (sweep 20260916-223136) submits three vertices with this register set to 0
         * (POINTLIST) and its 64x64 target comes back with **4095 background pixels and
         * exactly one red one** - the triangle became a point, which is precisely what the
         * oracle said POINTLIST does here. Its fence retired, so this is the register's
         * effect and not a failed submission. */
        {0x29bu, 0x00000002u}, /* VGT_GS_OUT_PRIM_TYPE: TRISTRIP */
        {0x2d3u, 0x00000001u}, /* GE_NGG_SUBGRP_CNTL: PRIM_AMP=1 */
        {0x2d5u, 0x00c12010u}, /* VGT_SHADER_STAGES_EN: ES_EN=REAL | PRIMGEN_EN | MAX_PRIMGRP_IN_WAVE=2 | GS_W32 | VS_W32 (NGG, wave32) */
        {0x1ffu, 0x00000100u}, /* GE_MAX_OUTPUT_PER_SUBGROUP: 256 */
        {0x20eu, 0x00000078u}, /* PA_CL_NGG_CNTL: VERTEX_REUSE_DEPTH=30 */
        {0x2a1u, 0x00000000u}, /* VGT_PRIMITIVEID_EN */
        {0x2a6u, 0x00000040u}, /* VGT_DRAW_PAYLOAD_CNTL */
        {0x2adu, 0x00000000u}, /* VGT_REUSE_OFF */
        {0x2abu, 0x00000001u}, /* VGT_ESGS_RING_ITEMSIZE: 1, so the vertex offsets handed to the primitive thread are plain vertex indices */
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
        /* These three were defaults nobody had varied, and nothing here draws a point or a
         * line, so they had never affected a pixel. They are now **cross-checked, not
         * measured**: obSCEne's `166-agc/primitive-draw` (sweep 20260916-223136, eboot) put a
         * submission through this hardware whose fence retired, and its command stream sets
         * all three to byte-identical values. A second independent stream agreeing is worth
         * more than one, and it is still not a measurement of what they *do* - varying them
         * and watching a pixel move is, and that is filed as obSCEne REQ-...9b71. */
        {0x280u, 0x00080008u}, /* PA_SU_POINT_SIZE */
        {0x281u, 0xffff0000u}, /* PA_SU_POINT_MINMAX */
        {0x282u, 0x00000008u}, /* PA_SU_LINE_CNTL */
        /* PA_SU_POLY_OFFSET_DB_FMT_CNTL, and the value is not arbitrary: Mesa's radeonsi writes
         * `NEG_NUM_DB_BITS(-23) | DB_IS_FLOAT_FMT(1)` for a 32-bit float z-buffer
         * (`src/gallium/drivers/radeonsi/si_state.c`, the `uses_poly_offset` block), and -23 in
         * the eight-bit field is 0xe9 with the float bit at 8 giving 0x1e9 - exactly what was
         * already here. Which also settles that this depth buffer is on Mesa's 32-bit float
         * path, so glPolygonOffset's units go in unscaled. */
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
        {0x204u, 0x00000000u}, /* PA_CL_CLIP_CNTL: standard clipping, UCP_ENA_0..5 (patched) */
        {0x206u, 0x0000043fu}, /* PA_CL_VTE_CNTL: viewport scale and offset on x, y, z */
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
        {0x1b1u, 0x00000002u}, /* SPI_VS_OUT_CONFIG: VS_EXPORT_COUNT (bits 5:1) = 1, two parameters */
        {0x1c2u, 0x00000001u}, /* SPI_SHADER_IDX_FORMAT */
        {0x1c3u, 0x00000004u}, /* SPI_SHADER_POS_FORMAT: POS0 = 4COMP */
        {0x1c4u, 0x00000000u}, /* SPI_SHADER_Z_FORMAT: no Z export */
        {0x1c5u, 0x00000009u}, /* SPI_SHADER_COL_FORMAT: COL0 = 32_ABGR */
        {0x1b3u, 0x00000002u}, /* SPI_PS_INPUT_ENA: PERSP_CENTER_ENA */
        {0x1b4u, 0x00000002u}, /* SPI_PS_INPUT_ADDR: PERSP_CENTER_ENA */
        {0x1b5u, 0x00000001u}, /* SPI_INTERP_CONTROL_0: FLAT_SHADE_ENA (no parameter is flagged flat) */
        {0x1b6u, 0x00000002u}, /* SPI_PS_IN_CONTROL: NUM_INTERP=2 */
        {0x1b8u, 0x01000000u}, /* SPI_BARYC_CNTL: FRONT_FACE_ALL_BITS */
    };

    for (size_t i = 0; i < sizeof(ctx_regs) / sizeof(ctx_regs[0]); i++) {
        uint32_t reg = ctx_regs[i].reg;
        uint32_t val = ctx_regs[i].val;
        if (reg == 0x318u) {
            val = (uint32_t)(color_gpu >> 8);
        } else if (reg == 0x390u) {
            val = (uint32_t)(color_gpu >> 40);
        } else if (reg == 0x3b0u) {
            val = OOPS_AGC_CB_COLOR_ATTRIB2(w, h);
        } else if (reg == 0x00du || reg == 0x082u || reg == 0x091u) {
            val = scissor_br;
        } else if (reg == 0x204u) {
            val = gl_compute_clip_cntl(ctx);
        } else if (reg == 0x094u || reg == 0x095u) {
            /* The viewport scissor is the only one of the four that carries GL's box. */
            uint32_t sc[2];
            gl_compute_scissor(ctx, w, h, sc);
            val = sc[reg - 0x094u];
        } else if (reg == 0x10fu || reg == 0x110u || reg == 0x111u || reg == 0x112u) {
            uint32_t vport[4];
            gl_compute_vport(ctx, h, vport);
            val = vport[reg - 0x10fu];
        } else if (reg == 0x113u) {
            /* Window z = z_ndc * ZSCALE + ZOFFSET, and glDepthRange(n, f) puts NDC -1..1 onto
             * n..f - so ZSCALE is (f - n) / 2 and ZOFFSET is (f + n) / 2.
             *
             * Derived, and checked against a known-good point rather than assumed: the default
             * range 0..1 gives 0.5 and 0.5, which are exactly the two constants these registers
             * carried while the gl-cube oracle was recorded. */
            val = gl_f32_bits((ctx->depth_far - ctx->depth_near) * 0.5f);
        } else if (reg == 0x114u) {
            val = gl_f32_bits((ctx->depth_far + ctx->depth_near) * 0.5f);
        } else if (reg == 0x2dfu) {
            /* GL's own glPolygonOffset has no clamp - that is EXT_polygon_offset_clamp, which
             * this does not implement - so zero, which is the disabled value. */
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
        } else if (reg == 0x200u) {
            val = gl_compute_db_depth_control(ctx);
        } else if (reg == 0x205u) {
            val = gl_compute_pa_su_sc_mode_cntl(ctx);
        } else if (reg == 0x08eu) {
            val = gl_compute_cb_target_mask(ctx);
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

    /* Shader stages. Every graphics stage is pointed at real code so no stage can fetch from an
     * unmapped address; on GFX10 the NGG wave takes its program counter from PGM_LO_ES and its
     * resource words from RSRC1/RSRC2_GS. RSRC1_GS is the measured 0x622c0042 with VGPRS raised
     * from 2 to 0x10: the measured value allocates twelve VGPRs in wave64 and the shader uses v20. */
    int textured = (gl_effective_texture_id(ctx) > 0u) ? 1 : 0;
    uint64_t ps_init_offset = textured ? 0x200u : 0x300u;
    uint32_t ps_rsrc2 = textured ? 0x00000004u : 0u; /* USER_SGPR=2 (bits 5:1): s[0:1] = descriptor table */
    const struct {
        uint32_t base_reg;
        uint64_t va_offset;
        uint32_t rsrc1;
        uint32_t rsrc2;
    } stages[] = {
        {0x08u, ps_init_offset, 0x000c0010u, ps_rsrc2}, /* PS */
        {0x48u, 0x000u, 0x000c0010u, 0x00000008u},      /* VS: USER_SGPR=4 (s0 = vbo offset) */
        {0x88u, 0x000u, 0x622c0046u, 0x000b0008u},      /* GS/NGG: VGPRS=6 (56 in wave32), ES_VGPR_COMP_CNT=3, LDS_SIZE=1 granule, USER_SGPR=4 (user data 0 = vbo offset, which lands in s8) */
        {0xc8u, 0x000u, 0x000c0010u, 0x00000000u},      /* ES: the NGG program counter */
        {0x108u, 0x000u, 0x000c0010u, 0x00000000u},     /* HS */
        {0x148u, 0x000u, 0x000c0010u, 0x00000000u},     /* LS */
    };
    for (size_t s = 0; s < sizeof(stages) / sizeof(stages[0]); s++) {
        uint32_t base_reg = stages[s].base_reg;
        uint64_t s_va = payload_va + stages[s].va_offset;
        *dw++ = 0xc0017600u; *dw++ = base_reg;       *dw++ = (uint32_t)(s_va >> 8);
        *dw++ = 0xc0017600u; *dw++ = base_reg + 1u;  *dw++ = (uint32_t)(s_va >> 40);
        *dw++ = 0xc0017600u; *dw++ = base_reg + 2u;  *dw++ = stages[s].rsrc1;
        *dw++ = 0xc0017600u; *dw++ = base_reg + 3u;  *dw++ = stages[s].rsrc2;
    }

    /* Compute-unit masks, as measured: CU1 stays clear for the NGG stage so pixel waves can
     * always find a home. */
    static const struct {
        uint32_t reg;
        uint32_t val;
    } spi_cu_regs[] = {
        {0x007u, 0x003fffffu}, /* SPI_SHADER_PGM_RSRC3_PS: CU_EN=0xffff, WAVE_LIMIT=0x3f */
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

    *dw++ = 0xc0002f00u; *dw++ = 1u;                                            /* PACKET3_NUM_INSTANCES */
    /* VGT_PRIMITIVE_TYPE goes through SET_UCONFIG_REG_INDEX with index 1: on GFX9 and later the CP
     * only forwards the primitive type to the geometry engine from the indexed write, and a plain
     * write leaves the GE assembling whatever the previous client drew (measured here: points). */
    *dw++ = 0xc0017a00u; *dw++ = 0x10000242u; *dw++ = OOPS_AGC_PRIM_TRILIST;    /* mmVGT_PRIMITIVE_TYPE, index 1 */
    *dw++ = 0xc0017900u; *dw++ = 0x25bu; *dw++ = 0x00020080u;      /* mmGE_CNTL: VERT_GRP_SIZE=256, PRIM_GRP_SIZE=128 */
    *dw++ = 0xc0017900u; *dw++ = 0x260u; *dw++ = OOPS_AGC_GE_PC_ALLOC_DEFAULT;  /* mmGE_PC_ALLOC */

    /* The user clip planes, written only when one is enabled so a frame that uses none emits
     * the stream it always did. Six planes of four floats are 24 consecutive registers from
     * PA_CL_UCP_0_X, so one packet carries all of them. */
    if (gl_compute_clip_cntl(ctx) != 0u) {
        gl_hw_emit_clip_planes(ctx, &dw);
    }

    ctx->dcb_words = (uint32_t)(dw - ctx->dcb_mem);
    ctx->hw_z_bound = GL_FALSE;
    /* The viewport, scissor and clip registers above were just written from the current state. */
    ctx->hw_vport_dirty = GL_FALSE;
    ctx->hw_scissor_dirty = GL_FALSE;
    ctx->hw_clip_dirty = GL_FALSE;
    ctx->hw_frame_tex = 0u; /* the new frame's descriptor slot holds nothing yet */
    ctx->hw_frame_active = GL_TRUE;
}

/* PACKET3_DMA_DATA: the command processor fills memory with a 32-bit pattern. SRC_SEL=DATA takes
 * the pattern from the packet, DST_SEL=TC_L2 writes through the GPU's L2 (the end-of-pipe event
 * writes it back before the CPU reads), CP_SYNC holds the CP until the fill has landed so the
 * draws that follow see it. Layout per the public PM4 documentation and the open-source drivers. */
void gl_hw_emit_dma_fill(uint32_t **dw_ptr, uint64_t dst, uint32_t value, uint32_t bytes) {
    uint32_t *dw = *dw_ptr;
    while (bytes > 0u) {
        uint32_t chunk = bytes > 0x3fffffcu ? 0x3fffffcu : bytes;
        *dw++ = 0xc0055000u;                     /* DMA_DATA, six body dwords */
        *dw++ = 0x80000000u | (2u << 29) | (3u << 20); /* CP_SYNC | SRC_SEL=DATA | DST_SEL=TC_L2, engine ME */
        *dw++ = value;                           /* the pattern (src address low when SRC_SEL is an address) */
        *dw++ = 0u;
        *dw++ = (uint32_t)dst;
        *dw++ = (uint32_t)(dst >> 32);
        *dw++ = chunk & 0x3ffffffu;              /* BYTE_COUNT; source and destination in memory, both incrementing */
        dst += chunk;
        bytes -= chunk;
    }
    *dw_ptr = dw;
}

/* PACKET3_DMA_DATA memory to memory, both sides through L2. */
void gl_hw_emit_dma_copy(uint32_t **dw_ptr, uint64_t src, uint64_t dst, uint32_t bytes) {
    uint32_t *dw = *dw_ptr;
    while (bytes > 0u) {
        uint32_t chunk = bytes > 0x3fffffcu ? 0x3fffffcu : bytes;
        *dw++ = 0xc0055000u;
        *dw++ = 0x80000000u | (3u << 29) | (3u << 20); /* CP_SYNC | SRC_SEL=TC_L2 address | DST_SEL=TC_L2 address */
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
    if (!ctx || !ctx->use_hardware || ctx->hw_failed) return;
    if (!ctx->hw_frame_active) {
        gl_hw_begin_frame(ctx);
    }
    if (ctx->dcb_words + 32u >= ctx->dcb_capacity_dw) {
        gl_hw_flush(ctx);
        gl_hw_begin_frame(ctx);
    }
    uint32_t w = ctx->width ? ctx->width : 1920u;
    uint32_t h = ctx->height ? ctx->height : 1080u;
    uint32_t *dw = ctx->dcb_mem + ctx->dcb_words;
    if ((mask & GL_COLOR_BUFFER_BIT) && ctx->framebuffer) {
        gl_hw_emit_dma_fill(&dw, (uint64_t)(uintptr_t)ctx->framebuffer, colour, w * h * 4u);
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && ctx->depth_buffer) {
        uint32_t px = ctx->depth_px ? (uint32_t)ctx->depth_px : w * h;
        gl_hw_emit_dma_fill(&dw, (uint64_t)(uintptr_t)ctx->depth_buffer, gl_f32_bits(depth), px * 4u);
    }
    ctx->dcb_words = (uint32_t)(dw - ctx->dcb_mem);
}

void gl_compute_lighting(gl_context_t *ctx, const float *obj_pos, const float *obj_norm,
                         const float *in_color, float *out_color) {
    if (!ctx || !obj_pos || !obj_norm || !out_color) return;

    /* 1. Transform vertex position to eye space: P_eye = ModelView * obj_pos */
    const gl_mat4_t *mv = &ctx->modelview_stack[ctx->modelview_depth];
    float peye[4];
    mat4_transform_vec4(peye, mv, obj_pos);
    float xe = peye[0];
    float ye = peye[1];
    float ze = peye[2];
    if (peye[3] != 0.0f && peye[3] != 1.0f) {
        float inv_w = 1.0f / peye[3];
        xe *= inv_w; ye *= inv_w; ze *= inv_w;
    }

    /* 2. Transform normal to eye space using inverse-transpose normal matrix */
    gl_update_normal_matrix(ctx);
    const float *nm = ctx->normal_matrix;
    float nx = nm[0] * obj_norm[0] + nm[1] * obj_norm[1] + nm[2] * obj_norm[2];
    float ny = nm[3] * obj_norm[0] + nm[4] * obj_norm[1] + nm[5] * obj_norm[2];
    float nz = nm[6] * obj_norm[0] + nm[7] * obj_norm[1] + nm[8] * obj_norm[2];

    /* Normalize normal vector */
    float nlen = gl_sqrt(nx * nx + ny * ny + nz * nz);
    if (nlen > 1e-6f) {
        float inv_n = 1.0f / nlen;
        nx *= inv_n; ny *= inv_n; nz *= inv_n;
    } else {
        nx = 0.0f; ny = 0.0f; nz = 1.0f;
    }

    /* 3. Material properties with GL_COLOR_MATERIAL tracking */
    float mat_amb[4], mat_diff[4], mat_spec[4], mat_emis[4];
    memcpy(mat_amb,  ctx->mat_front.ambient,  4 * sizeof(float));
    memcpy(mat_diff, ctx->mat_front.diffuse,  4 * sizeof(float));
    memcpy(mat_spec, ctx->mat_front.specular, 4 * sizeof(float));
    memcpy(mat_emis, ctx->mat_front.emission, 4 * sizeof(float));
    float shininess = ctx->mat_front.shininess;

    if (ctx->cap_color_material && in_color) {
        if (ctx->color_material_mode == GL_AMBIENT || ctx->color_material_mode == GL_AMBIENT_AND_DIFFUSE) {
            memcpy(mat_amb, in_color, 4 * sizeof(float));
        }
        if (ctx->color_material_mode == GL_DIFFUSE || ctx->color_material_mode == GL_AMBIENT_AND_DIFFUSE) {
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

    /* 5. View direction vector V in eye space */
    float vx, vy, vz;
    if (ctx->light_model_local_viewer) {
        float vlen = gl_sqrt(xe * xe + ye * ye + ze * ze);
        if (vlen > 1e-6f) {
            float inv_v = 1.0f / vlen;
            vx = -xe * inv_v; vy = -ye * inv_v; vz = -ze * inv_v;
        } else {
            vx = 0.0f; vy = 0.0f; vz = 1.0f;
        }
    } else {
        vx = 0.0f; vy = 0.0f; vz = 1.0f;
    }

    /* 6. Accumulate contribution from enabled light sources */
    for (int i = 0; i < OOPS_GL_LIGHT_COUNT; i++) {
        if (!ctx->lights[i].enabled) continue;
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
                lx = 0.0f; ly = 0.0f; lz = 1.0f;
            }
        } else {
            /* Positional light: vector from vertex to light */
            float lpx = lt->position[0];
            float lpy = lt->position[1];
            float lpz = lt->position[2];
            if (lt->position[3] != 1.0f && lt->position[3] != 0.0f) {
                float inv_w = 1.0f / lt->position[3];
                lpx *= inv_w; lpy *= inv_w; lpz *= inv_w;
            }
            float dx = lpx - xe;
            float dy = lpy - ye;
            float dz = lpz - ze;
            float dist = gl_sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > 1e-6f) {
                float inv_d = 1.0f / dist;
                lx = dx * inv_d; ly = dy * inv_d; lz = dz * inv_d;
            } else {
                lx = 0.0f; ly = 0.0f; lz = 1.0f;
            }

            float denom = lt->const_att + lt->linear_att * dist + lt->quad_att * (dist * dist);
            att = (denom > 1e-6f) ? (1.0f / denom) : 1.0f;
        }

        /* Spotlight factor */
        float spot = 1.0f;
        if (lt->spot_cutoff < 180.0f) {
            float spot_dot = -(lx * lt->spot_direction[0] + ly * lt->spot_direction[1] + lz * lt->spot_direction[2]);
            if (spot_dot >= lt->spot_cutoff_cos && spot_dot > 0.0f) {
                if (lt->spot_exponent > 0.0f) {
                    spot = gl_pow(spot_dot, lt->spot_exponent);
                }
            } else {
                spot = 0.0f;
            }
        }
        if (spot <= 0.0f) continue;

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

            /* Specular component (Blinn-Phong half-vector) */
            if (shininess > 0.0f) {
                float hx = lx + vx;
                float hy = ly + vy;
                float hz = lz + vz;
                float hlen = gl_sqrt(hx * hx + hy * hy + hz * hz);
                if (hlen > 1e-6f) {
                    float inv_h = 1.0f / hlen;
                    hx *= inv_h; hy *= inv_h; hz *= inv_h;
                    float ndoth = nx * hx + ny * hy + nz * hz;
                    if (ndoth > 0.0f) {
                        float spec_pow = gl_pow(ndoth, shininess);
                        r += factor * (spec_pow * lt->specular[0] * mat_spec[0]);
                        g += factor * (spec_pow * lt->specular[1] * mat_spec[1]);
                        b += factor * (spec_pow * lt->specular[2] * mat_spec[2]);
                    }
                }
            }
        }
    }

    /* 7. Clamp to [0.0, 1.0] */
    out_color[0] = (r < 0.0f) ? 0.0f : ((r > 1.0f) ? 1.0f : r);
    out_color[1] = (g < 0.0f) ? 0.0f : ((g > 1.0f) ? 1.0f : g);
    out_color[2] = (b < 0.0f) ? 0.0f : ((b > 1.0f) ? 1.0f : b);
    out_color[3] = (a < 0.0f) ? 0.0f : ((a > 1.0f) ? 1.0f : a);
}

void gl_draw_primitive_triangle(gl_context_t *ctx, const gl_vertex_t *v0,
                                const gl_vertex_t *v1, const gl_vertex_t *v2) {
    if (!ctx || !v0 || !v1 || !v2) return;

    gl_update_mvp(ctx);

    /* 1. Transform vertices to clip space */
    float c0[4], c1[4], c2[4];
    float in0[4] = {v0->x, v0->y, v0->z, v0->w};
    float in1[4] = {v1->x, v1->y, v1->z, v1->w};
    float in2[4] = {v2->x, v2->y, v2->z, v2->w};

    mat4_transform_vec4(c0, &ctx->mvp, in0);
    mat4_transform_vec4(c1, &ctx->mvp, in1);
    mat4_transform_vec4(c2, &ctx->mvp, in2);

    /* User clip distances, in eye space, one per plane per vertex.
     *
     * Computed here and interpolated per fragment rather than the triangle being geometrically
     * clipped. The visible result is identical and it avoids a clipper that turns one triangle
     * into two or three - which would also have to re-derive colours, texture coordinates and
     * the winding for each piece. A plane whose enable is off is left at zero and never read. */
    float cd0[OOPS_GL_CLIP_PLANE_COUNT] = {0};
    float cd1[OOPS_GL_CLIP_PLANE_COUNT] = {0};
    float cd2[OOPS_GL_CLIP_PLANE_COUNT] = {0};
    /* Unfogged unless fog is on, so the blend below is a no-op without a branch per fragment. */
    float fog0 = 1.0f, fog1 = 1.0f, fog2 = 1.0f;
    GLboolean any_clip = GL_FALSE;
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
        if (ctx->clip_plane_enabled[i]) { any_clip = GL_TRUE; break; }
    }
    if (ctx->cap_fog) {
        /* **The distance is the eye-space distance to the vertex**, which is what the
         * specification says fog works from - not the window depth, which would make fog change
         * with the depth range, and not the object-space distance, which would ignore the
         * modelview entirely. */
        const gl_mat4_t *mv_fog = &ctx->modelview_stack[ctx->modelview_depth];
        float f0[4], f1[4], f2[4];
        mat4_transform_vec4(f0, mv_fog, in0);
        mat4_transform_vec4(f1, mv_fog, in1);
        mat4_transform_vec4(f2, mv_fog, in2);
        fog0 = gl_fog_factor(ctx, gl_sqrt(f0[0]*f0[0] + f0[1]*f0[1] + f0[2]*f0[2]));
        fog1 = gl_fog_factor(ctx, gl_sqrt(f1[0]*f1[0] + f1[1]*f1[1] + f1[2]*f1[2]));
        fog2 = gl_fog_factor(ctx, gl_sqrt(f2[0]*f2[0] + f2[1]*f2[1] + f2[2]*f2[2]));
    }
    if (any_clip) {
        const gl_mat4_t *mv_clip = &ctx->modelview_stack[ctx->modelview_depth];
        float e0[4], e1[4], e2[4];
        mat4_transform_vec4(e0, mv_clip, in0);
        mat4_transform_vec4(e1, mv_clip, in1);
        mat4_transform_vec4(e2, mv_clip, in2);
        for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
            if (!ctx->clip_plane_enabled[i]) continue;
            const float *p = ctx->clip_plane[i];
            cd0[i] = p[0] * e0[0] + p[1] * e0[1] + p[2] * e0[2] + p[3] * e0[3];
            cd1[i] = p[0] * e1[0] + p[1] * e1[1] + p[2] * e1[2] + p[3] * e1[3];
            cd2[i] = p[0] * e2[0] + p[1] * e2[1] + p[2] * e2[2] + p[3] * e2[3];
        }
        /* Wholly outside any single plane means nothing survives, and that is worth taking
         * early - it is the common case for a clip plane used to cut a scene in half. */
        for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) {
            if (!ctx->clip_plane_enabled[i]) continue;
            if (cd0[i] < 0.0f && cd1[i] < 0.0f && cd2[i] < 0.0f) return;
        }
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

    /* 3. Viewport mapping */
    float vp_w_half = (float)ctx->vp_w * 0.5f;
    float vp_h_half = (float)ctx->vp_h * 0.5f;
    float vp_ox = (float)ctx->vp_x + vp_w_half;
    float vp_oy = (float)ctx->vp_y + vp_h_half;

    /* Compute vertex colors (lighting or direct color) */
    float col0[4] = {v0->r, v0->g, v0->b, v0->a};
    float col1[4] = {v1->r, v1->g, v1->b, v1->a};
    float col2[4] = {v2->r, v2->g, v2->b, v2->a};

    if (ctx->cap_lighting) {
        float p0[4] = {v0->x, v0->y, v0->z, v0->w};
        float n0[3] = {v0->nx, v0->ny, v0->nz};
        float in_col0[4] = {v0->r, v0->g, v0->b, v0->a};
        gl_compute_lighting(ctx, p0, n0, in_col0, col0);

        float p1[4] = {v1->x, v1->y, v1->z, v1->w};
        float n1[3] = {v1->nx, v1->ny, v1->nz};
        float in_col1[4] = {v1->r, v1->g, v1->b, v1->a};
        gl_compute_lighting(ctx, p1, n1, in_col1, col1);

        float p2[4] = {v2->x, v2->y, v2->z, v2->w};
        float n2[3] = {v2->nx, v2->ny, v2->nz};
        float in_col2[4] = {v2->r, v2->g, v2->b, v2->a};
        gl_compute_lighting(ctx, p2, n2, in_col2, col2);
    }

    if (ctx->shade_model == GL_FLAT) {
        memcpy(col0, col2, 4 * sizeof(float));
        memcpy(col1, col2, 4 * sizeof(float));
    }

    /* **glDepthRange, which lived only in the hardware path until now.**
     *
     * The registers at 0x113/0x114 carried `(far-near)/2` and `(far+near)/2` while this
     * rasteriser hardcoded the 0..1 mapping, so a program that called `glDepthRange` got one
     * depth buffer on the console and a different one on the host with no error anywhere -
     * the same divergence `gl1-probe` found in the alpha test. Reversing the range is a
     * technique rather than a mistake, so it has to work in both places or neither.
     *
     * `z_window = ((far - near) / 2) * z_ndc + (far + near) / 2`, which is the specification's
     * own formula and the one the registers are computed from. */
    const float dr_scale = (ctx->depth_far - ctx->depth_near) * 0.5f;
    const float dr_offset = (ctx->depth_far + ctx->depth_near) * 0.5f;

    gl_screen_vertex_t sv0, sv1, sv2;
    sv0.sx = ndc0[0] * vp_w_half + vp_ox;
    sv0.sy = (float)ctx->height - (ndc0[1] * vp_h_half + vp_oy); /* Y-flip for screen space */
    sv0.sz = ndc0[2] * dr_scale + dr_offset;
    sv0.inv_w = inv_w0;
    sv0.r = col0[0]; sv0.g = col0[1]; sv0.b = col0[2]; sv0.a = col0[3];
    sv0.u = v0->u; sv0.v = v0->v;
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) sv0.cd[i] = cd0[i];
    sv0.fog = fog0;

    sv1.sx = ndc1[0] * vp_w_half + vp_ox;
    sv1.sy = (float)ctx->height - (ndc1[1] * vp_h_half + vp_oy);
    sv1.sz = ndc1[2] * dr_scale + dr_offset;
    sv1.inv_w = inv_w1;
    sv1.r = col1[0]; sv1.g = col1[1]; sv1.b = col1[2]; sv1.a = col1[3];
    sv1.u = v1->u; sv1.v = v1->v;
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) sv1.cd[i] = cd1[i];
    sv1.fog = fog1;

    sv2.sx = ndc2[0] * vp_w_half + vp_ox;
    sv2.sy = (float)ctx->height - (ndc2[1] * vp_h_half + vp_oy);
    sv2.sz = ndc2[2] * dr_scale + dr_offset;
    sv2.inv_w = inv_w2;
    sv2.r = col2[0]; sv2.g = col2[1]; sv2.b = col2[2]; sv2.a = col2[3];
    sv2.u = v2->u; sv2.v = v2->v;
    for (int i = 0; i < OOPS_GL_CLIP_PLANE_COUNT; i++) sv2.cd[i] = cd2[i];
    sv2.fog = fog2;

    /* 4. Backface culling via 2D signed area (screen coordinates) */
    float area = (sv1.sx - sv0.sx) * (sv2.sy - sv0.sy) - (sv1.sy - sv0.sy) * (sv2.sx - sv0.sx);

    /* **glPolygonOffset, the third piece of state that existed only on the hardware path.**
     *
     * `PA_SU_POLY_OFFSET_*` carried the factor and units while this rasteriser knew nothing of
     * them, so coplanar geometry drawn over a surface - the whole point of the call - separated
     * on the console and z-fought on the host.
     *
     * The specification's offset is `factor * m + units * r`, where `m` is the largest depth
     * slope of the triangle and `r` is the smallest resolvable depth difference. Computed here
     * rather than per fragment because **both terms are constant across a triangle**, which is
     * what makes the offset a plane shift rather than a warp.
     *
     * `m` is `max(|dz/dx|, |dz/dy|)` from the plane through the three screen vertices; the
     * signed area is its denominator, which is why this sits after it. */
    float depth_bias = 0.0f;
    if (ctx->cap_polygon_offset_fill && area != 0.0f) {
        const float inv_area = 1.0f / area;
        const float dzdx = ((sv1.sz - sv0.sz) * (sv2.sy - sv0.sy) -
                            (sv2.sz - sv0.sz) * (sv1.sy - sv0.sy)) * inv_area;
        const float dzdy = ((sv2.sz - sv0.sz) * (sv1.sx - sv0.sx) -
                            (sv1.sz - sv0.sz) * (sv2.sx - sv0.sx)) * inv_area;
        float adx = dzdx < 0.0f ? -dzdx : dzdx;
        float ady = dzdy < 0.0f ? -dzdy : dzdy;
        const float slope = adx > ady ? adx : ady;
        /* `r` for a float depth buffer holding 0..1. The hardware derives its own from the
         * depth format; this is the software path's equivalent and is deliberately small
         * enough that `units` behaves like a nudge rather than a jump. */
        const float resolvable = 1.0f / 16777216.0f;
        depth_bias = ctx->polygon_offset_factor * slope +
                     ctx->polygon_offset_units * resolvable;
    }
    sv0.sz += depth_bias;
    sv1.sz += depth_bias;
    sv2.sz += depth_bias;

    if (!ctx->use_hardware && ctx->cap_cull_face) {
        /* Note: with Y-flip, CCW in 3D becomes negative in screen space */
        GLboolean is_ccw = (area < 0.0f) ? GL_TRUE : GL_FALSE;
        if (ctx->front_face == GL_CW) is_ccw = !is_ccw;

        if (ctx->cull_mode == GL_BACK && !is_ccw) return;
        if (ctx->cull_mode == GL_FRONT && is_ccw) return;
        if (ctx->cull_mode == GL_FRONT_AND_BACK) return;
    }

    if (area > -1e-4f && area < 1e-4f) return; /* Degenerate */

    /* 5. Hardware AGC Path (AMD RDNA2 GFX10.3) */
    if (ctx->use_hardware) {
        if (ctx->hw_failed) return; /* the failure is on the log and the status query; nothing is drawn */
        if (!ctx->hw_frame_active) {
            gl_hw_begin_frame(ctx);
        }

        if (ctx->dcb_words + 160 >= ctx->dcb_capacity_dw) {
            gl_hw_flush(ctx);
            gl_hw_begin_frame(ctx);
        }

        /* **There is one descriptor slot, and every textured draw in the frame points at it.**
         *
         * The image and sampler descriptors are copied to a single address below, and each draw
         * hands the shader that same address - so a second texture bound later in the frame
         * overwrites the first, and at the flush every textured draw samples whichever texture
         * was bound last. The draws are built, not executed, so nothing notices until submit.
         *
         * Submitting the frame first is what gives the built draws the descriptors they were
         * built with. It costs a flush per texture change, which is the price of one slot; a
         * ring of slots like the vertex buffer's would avoid it and is the change to make if a
         * frame ever switches textures often enough to matter. Only on a *different* texture:
         * rebinding the same one, which a display list does constantly, changes nothing. */
        const GLuint eff_tex = gl_effective_texture_id(ctx);
        if (eff_tex > 0u && ctx->hw_frame_tex != 0u && ctx->hw_frame_tex != eff_tex) {
            gl_hw_flush(ctx);
            gl_hw_begin_frame(ctx);
        }

        /* Compute VBO buffer offset for this triangle (3 vertices * 48 bytes = 144 bytes) */
        size_t tri_idx = (size_t)ctx->triangles_drawn % 450;
        size_t vbo_offset = tri_idx * 144;

        if (ctx->vbo_mem) {
            char *vbo_ptr = (char *)ctx->vbo_mem + vbo_offset;
            float uv0[4] = {v0->u, v0->v, 0.0f, 0.0f};
            float uv1[4] = {v1->u, v1->v, 0.0f, 0.0f};
            float uv2[4] = {v2->u, v2->v, 0.0f, 0.0f};

            /* Vertex 0 (48 bytes) */
            memcpy(vbo_ptr + 0,   c0, 16);
            memcpy(vbo_ptr + 16,  col0, 16);
            memcpy(vbo_ptr + 32,  uv0, 16);
            /* Vertex 1 (48 bytes) */
            memcpy(vbo_ptr + 48,  c1, 16);
            memcpy(vbo_ptr + 64,  col1, 16);
            memcpy(vbo_ptr + 80,  uv1, 16);
            /* Vertex 2 (48 bytes) */
            memcpy(vbo_ptr + 96,  c2, 16);
            memcpy(vbo_ptr + 112, col2, 16);
            memcpy(vbo_ptr + 128, uv2, 16);
#if defined(__x86_64__)
            __builtin_ia32_clflush((const void *)(vbo_ptr + 0));
            __builtin_ia32_clflush((const void *)(vbo_ptr + 64));
            __builtin_ia32_clflush((const void *)(vbo_ptr + 128));
#endif
        }

        uint32_t *dw = ctx->dcb_mem + ctx->dcb_words;
        uint64_t payload_va = (uint64_t)(uintptr_t)ctx->gpu_payload;
        uint64_t desc_table_va = payload_va + 0x900;
        uint64_t ps_va = payload_va + 0x300; /* Default: untextured Gouraud */
        uint32_t ps_rsrc2 = 0u;

        if (eff_tex > 0u) {
            ps_va = payload_va + 0x200; /* Stage 5: Textured + Gouraud */
            ps_rsrc2 = 0x00000004u; /* USER_SGPR=2 (bits 5:1): s[0:1] = descriptor table. 0x2 loads one SGPR and the primitive mask lands in s1 (measured 2026-09-14) */
            for (int ti = 0; ti < OOPS_GL_MAX_TEXTURE_OBJECTS; ti++) {
                if (ctx->textures[ti].used && ctx->textures[ti].id == eff_tex) {
                    uint32_t *dt = (uint32_t *)((char *)ctx->gpu_payload + 0x900);
                    memcpy(dt, ctx->textures[ti].img_desc, 32);
                    memcpy(dt + 8, ctx->textures[ti].samp_desc, 16);
                    ctx->hw_frame_tex = eff_tex;
#if defined(__x86_64__)
                    __builtin_ia32_clflush((const void *)dt);
#endif
                    break;
                }
            }
        }

        /* Emit dynamic Depth Control, Blending, Cull Mode, and Color Target Mask state */
        uint32_t cur_depth_ctrl = gl_compute_db_depth_control(ctx);
        uint32_t cur_blend_ctrl = gl_compute_cb_blend_control(ctx);
        uint32_t cur_cull_ctrl = gl_compute_pa_su_sc_mode_cntl(ctx);
        uint32_t cur_target_mask = gl_compute_cb_target_mask(ctx);

        /* The first depth-tested draw of a frame binds the depth surface. */
        if (cur_depth_ctrl != 0u && !ctx->hw_z_bound) {
            gl_hw_emit_depth_block(ctx, &dw);
            ctx->hw_z_bound = GL_TRUE;
        }

        *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG mmDB_DEPTH_CONTROL (0x200) */
        *dw++ = 0x200u;
        *dw++ = cur_depth_ctrl;

        *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG mmCB_BLEND0_CONTROL (0x1e0) */
        *dw++ = 0x1e0u;
        *dw++ = cur_blend_ctrl;

        *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG mmPA_SU_SC_MODE_CNTL (0x205) */
        *dw++ = 0x205u;
        *dw++ = cur_cull_ctrl;

        *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG mmCB_TARGET_MASK (0x08e) */
        *dw++ = 0x08eu;
        *dw++ = cur_target_mask;

        /* A viewport set after the frame's registers were written. Four consecutive registers,
         * so one packet. Nothing is emitted for a frame whose viewport was already current when
         * it began, which keeps the stream gl-cube's oracle recorded byte for byte. */
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

        /* A scissor box, or the test being switched, after the frame's registers were written.
         * Two consecutive registers, so one packet - and like the viewport, nothing is emitted
         * for a frame whose scissor was already current when it began. */
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

        /* A clip plane set, or enabled, after the frame's registers were written. Both the
         * equations and the enable bits move together - a plane written without its enable does
         * nothing, and an enable without its plane clips against whatever was there before. */
        if (ctx->hw_clip_dirty) {
            gl_hw_emit_clip_planes(ctx, &dw);
            *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG mmPA_CL_CLIP_CNTL */
            *dw++ = 0x204u;
            *dw++ = gl_compute_clip_cntl(ctx);
            ctx->hw_clip_dirty = GL_FALSE;
        }

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

        /* Pass Descriptor Table VA to PS User SGPRs 0 and 1 (mmSPI_SHADER_USER_DATA_PS_0 = 0x0c, 0x0d) */
        *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_USER_DATA_PS_0 */
        *dw++ = 0x0cu;
        *dw++ = (uint32_t)desc_table_va;
        *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_USER_DATA_PS_1 */
        *dw++ = 0x0du;
        *dw++ = (uint32_t)(desc_table_va >> 32);

        /* Pass VBO byte offset into GS User SGPR 0 (mmSPI_SHADER_USER_DATA_GS_0 = 0x8c) */
        *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_USER_DATA_GS_0 */
        *dw++ = 0x8cu;
        *dw++ = (uint32_t)vbo_offset;
        *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_USER_DATA_VS_0 (legacy VS stage) */
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
        return;
    }

#ifdef OOPS_HOST_BUILD
    /* 6. Host builds have no GPU: the software rasterizer stands in for it there, and only there. */
    gl_rasterize_triangle(ctx, &sv0, &sv1, &sv2);
    ctx->triangles_drawn++;
#else
    gl_hw_fail(ctx, "no hardware pipeline: nothing is drawn");
#endif
}

#ifdef OOPS_HOST_BUILD
/* One stencil operation, through the write mask.
 *
 * **The mask is applied to the result, not to the operand**: GL_INVERT with a mask of 0x0f
 * inverts all eight bits and then writes back only the low four, which is not the same as
 * inverting four bits. Getting that backwards gives a buffer that is right whenever the mask is
 * all-ones - which is the default, and so the only case most code exercises.
 *
 * GL_INCR and GL_DECR saturate rather than wrap; the wrapping forms are GL 1.4 and refused.
 *
 * Inside the host-build guard with the rest of the software rasteriser: on the target the depth
 * block's stencil registers do this, and nothing here runs. */
static void gl_stencil_apply(uint8_t *sp, GLenum op, GLint ref, uint32_t wmask) {
    const uint8_t old = *sp;
    uint8_t next = old;
    switch (op) {
        case GL_KEEP:    return;
        case GL_ZERO:    next = 0u; break;
        case GL_REPLACE: next = (uint8_t)(ref & 0xff); break;
        case GL_INCR:    next = (uint8_t)((old < 255u) ? (old + 1u) : 255u); break;
        case GL_DECR:    next = (uint8_t)((old > 0u) ? (old - 1u) : 0u); break;
        case GL_INVERT:  next = (uint8_t)(~old); break;
        default:         return;
    }
    const uint8_t m = (uint8_t)(wmask & 0xffu);
    *sp = (uint8_t)((next & m) | (old & (uint8_t)~m));
}

static float get_blend_factor(GLenum factor, float src_r, float src_g, float src_b, float src_a,
                              float dst_r, float dst_g, float dst_b, float dst_a, int channel) {
    switch (factor) {
        case GL_ZERO: return 0.0f;
        case GL_ONE: return 1.0f;
        case GL_SRC_COLOR:
            return (channel == 0) ? src_r : ((channel == 1) ? src_g : ((channel == 2) ? src_b : src_a));
        case GL_ONE_MINUS_SRC_COLOR:
            return 1.0f - ((channel == 0) ? src_r : ((channel == 1) ? src_g : ((channel == 2) ? src_b : src_a)));
        case GL_SRC_ALPHA: return src_a;
        case GL_ONE_MINUS_SRC_ALPHA: return 1.0f - src_a;
        case GL_DST_ALPHA: return dst_a;
        case GL_ONE_MINUS_DST_ALPHA: return 1.0f - dst_a;
        case GL_DST_COLOR:
            return (channel == 0) ? dst_r : ((channel == 1) ? dst_g : ((channel == 2) ? dst_b : dst_a));
        case GL_ONE_MINUS_DST_COLOR:
            return 1.0f - ((channel == 0) ? dst_r : ((channel == 1) ? dst_g : ((channel == 2) ? dst_b : dst_a)));
        case GL_SRC_ALPHA_SATURATE: {
            float f = 1.0f - dst_a;
            return (src_a < f) ? src_a : f;
        }
        default: return 1.0f;
    }
}

void gl_rasterize_triangle(gl_context_t *ctx, const gl_screen_vertex_t *v0,
                           const gl_screen_vertex_t *v1, const gl_screen_vertex_t *v2) {
    /* Compute triangle 2D bounding box */
    float fmin_x = v0->sx; if (v1->sx < fmin_x) fmin_x = v1->sx; if (v2->sx < fmin_x) fmin_x = v2->sx;
    float fmax_x = v0->sx; if (v1->sx > fmax_x) fmax_x = v1->sx; if (v2->sx > fmax_x) fmax_x = v2->sx;
    float fmin_y = v0->sy; if (v1->sy < fmin_y) fmin_y = v1->sy; if (v2->sy < fmin_y) fmin_y = v2->sy;
    float fmax_y = v0->sy; if (v1->sy > fmax_y) fmax_y = v1->sy; if (v2->sy > fmax_y) fmax_y = v2->sy;

    int min_x = (int)fmin_x;
    int max_x = (int)(fmax_x + 1.0f);
    int min_y = (int)fmin_y;
    int max_y = (int)(fmax_y + 1.0f);

    /* Clip against scissor / screen bounds */
    int clip_min_x = 0;
    int clip_max_x = (int)ctx->width - 1;
    int clip_min_y = 0;
    int clip_max_y = (int)ctx->height - 1;

    if (ctx->cap_scissor_test) {
        if (ctx->sc_x > clip_min_x) clip_min_x = ctx->sc_x;
        if (ctx->sc_x + ctx->sc_w - 1 < clip_max_x) clip_max_x = ctx->sc_x + ctx->sc_w - 1;
        int sc_y_top = (int)ctx->height - (ctx->sc_y + ctx->sc_h);
        if (sc_y_top > clip_min_y) clip_min_y = sc_y_top;
        if (sc_y_top + ctx->sc_h - 1 < clip_max_y) clip_max_y = sc_y_top + ctx->sc_h - 1;
    }

    if (min_x < clip_min_x) min_x = clip_min_x;
    if (max_x > clip_max_x) max_x = clip_max_x;
    if (min_y < clip_min_y) min_y = clip_min_y;
    if (max_y > clip_max_y) max_y = clip_max_y;

    if (min_x > max_x || min_y > max_y) return;

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
        dx01 = -dx01; dy01 = -dy01;
        dx12 = -dx12; dy12 = -dy12;
        dx20 = -dx20; dy20 = -dy20;
    }

    uint32_t *fb = ctx->framebuffer;
    float *db = ctx->depth_buffer;
    uint32_t pitch = ctx->width;
    GLboolean depth_test = ctx->cap_depth_test;
    GLboolean depth_write = ctx->depth_mask;
    GLenum depth_func = ctx->depth_func;
    GLboolean blend = ctx->cap_blend;

    const GLboolean fog_on = ctx->cap_fog;
    const float fog_r = ctx->fog_color[0];
    const float fog_g = ctx->fog_color[1];
    const float fog_b = ctx->fog_color[2];

    GLboolean stencil_test = (GLboolean)(ctx->cap_stencil_test && ctx->stencil_buffer != NULL);
    GLenum stencil_func = ctx->stencil_func;
    GLint stencil_ref = ctx->stencil_ref;
    uint32_t stencil_vmask = ctx->stencil_value_mask;
    uint32_t stencil_wmask = ctx->stencil_writemask;
    GLenum stencil_op_fail = ctx->stencil_fail;
    GLenum stencil_op_zfail = ctx->stencil_zfail;
    GLenum stencil_op_zpass = ctx->stencil_zpass;

    /* Hoisted so the fragment loop reads a local array rather than the context, and so the
     * common case - no clip plane enabled - costs one test per fragment instead of six. */
    GLboolean clip_on[OOPS_GL_CLIP_PLANE_COUNT];
    GLboolean clip_any = GL_FALSE;
    for (int ci = 0; ci < OOPS_GL_CLIP_PLANE_COUNT; ci++) {
        clip_on[ci] = ctx->clip_plane_enabled[ci];
        if (clip_on[ci]) clip_any = GL_TRUE;
    }

    /* Check for bound and enabled texture */
    gl_texture_object_t *tex = NULL;
    const GLuint eff_tex_id = gl_effective_texture_id(ctx);
    if (eff_tex_id > 0u) {
        for (int ti = 0; ti < OOPS_GL_MAX_TEXTURE_OBJECTS; ti++) {
            if (ctx->textures[ti].used && ctx->textures[ti].id == eff_tex_id) {
                tex = &ctx->textures[ti];
                break;
            }
        }
    }

    for (int y = min_y; y <= max_y; y++) {
        float py = (float)y + 0.5f;
        uint32_t *fb_row = fb ? &fb[(uint32_t)y * pitch] : NULL;
        float *db_row = db ? &db[(uint32_t)y * pitch] : NULL;
        uint8_t *sb_row = stencil_test ? &ctx->stencil_buffer[(uint32_t)y * pitch] : NULL;

        for (int x = min_x; x <= max_x; x++) {
            float px = (float)x + 0.5f;

            /* Barycentric weights */
            float w0 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
            float w1 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);
            float w2 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);

            if (area < 0.0f) {
                w0 = -w0; w1 = -w1; w2 = -w2;
            }

            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                float b0 = w0 * inv_area;
                float b1 = w1 * inv_area;
                float b2 = w2 * inv_area;
                if (area < 0.0f) {
                    b0 = -b0; b1 = -b1; b2 = -b2;
                }

                /* **User clip planes, before anything else touches a buffer.**
                 *
                 * A clipped fragment must not write colour *or* depth, so the test sits ahead of
                 * the depth test rather than beside the colour write - a fragment that fails a
                 * clip plane was never in the primitive at all, and leaving its depth behind
                 * would hide geometry that is genuinely visible. */
                if (clip_any) {
                    GLboolean clipped = GL_FALSE;
                    for (int ci = 0; ci < OOPS_GL_CLIP_PLANE_COUNT; ci++) {
                        if (!clip_on[ci]) continue;
                        if (b0 * v0->cd[ci] + b1 * v1->cd[ci] + b2 * v2->cd[ci] < 0.0f) {
                            clipped = GL_TRUE;
                            break;
                        }
                    }
                    if (clipped) continue;
                }

                /* Interpolate depth Z */
                float z = b0 * v0->sz + b1 * v1->sz + b2 * v2->sz;
                GLboolean depth_pending = GL_FALSE;

                /* Whether the depth test would reject this fragment. With stencil off this is
                 * acted on immediately below; with stencil on it has to be carried past the
                 * alpha test, because the depth result chooses between glStencilOp's second and
                 * third operations and none of them may run for a fragment alpha discards. */
                GLboolean depth_failed = GL_FALSE;

                /* Depth test */
                if (depth_test && db_row) {
                    float cur_z = db_row[x];
                    GLboolean pass = GL_FALSE;
                    switch (depth_func) {
                        case GL_LESS:     pass = (z < cur_z); break;
                        case GL_LEQUAL:   pass = (z <= cur_z); break;
                        case GL_GREATER:  pass = (z > cur_z); break;
                        case GL_GEQUAL:   pass = (z >= cur_z); break;
                        case GL_EQUAL:    pass = (z == cur_z); break;
                        case GL_NOTEQUAL: pass = (z != cur_z); break;
                        case GL_ALWAYS:   pass = GL_TRUE; break;
                        case GL_NEVER:    pass = GL_FALSE; break;
                        default:          pass = (z < cur_z); break;
                    }
                    if (!pass) {
                        /* With no stencil there is nothing further to decide, so reject now and
                         * keep the fast path exactly as it was. */
                        if (!stencil_test) continue;
                        depth_failed = GL_TRUE;
                    }
                    /* **The write is deferred past the alpha test below.** GL's fixed-function
                     * order is alpha test, then depth - so a fragment the alpha test discards
                     * must leave the depth buffer alone. Writing here would have everything
                     * behind a discarded fragment hidden by something that is not visible,
                     * which is the classic symptom of a half-implemented alpha test. */
                    depth_pending = (GLboolean)depth_write;
                }

                /* Interpolate color */
                float r = b0 * v0->r + b1 * v1->r + b2 * v2->r;
                float g = b0 * v0->g + b1 * v1->g + b2 * v2->g;
                float b = b0 * v0->b + b1 * v1->b + b2 * v2->b;
                float a = b0 * v0->a + b1 * v1->a + b2 * v2->a;

                /* Texture application if active */
                if (tex && tex->pixels && tex->width > 0 && tex->height > 0) {
                    float u = b0 * v0->u + b1 * v1->u + b2 * v2->u;
                    float v = b0 * v0->v + b1 * v1->v + b2 * v2->v;
                    if (tex->wrap_s == GL_REPEAT) {
                        u = u - (float)(int)u;
                        if (u < 0.0f) u += 1.0f;
                    } else {
                        if (u < 0.0f) u = 0.0f;
                        if (u > 1.0f) u = 1.0f;
                    }
                    if (tex->wrap_t == GL_REPEAT) {
                        v = v - (float)(int)v;
                        if (v < 0.0f) v += 1.0f;
                    } else {
                        if (v < 0.0f) v = 0.0f;
                        if (v > 1.0f) v = 1.0f;
                    }
                    int tx = (int)(u * (float)(tex->width - 1) + 0.5f);
                    int ty = (int)(v * (float)(tex->height - 1) + 0.5f);
                    if (tx < 0) tx = 0; if (tx >= tex->width) tx = tex->width - 1;
                    if (ty < 0) ty = 0; if (ty >= tex->height) ty = tex->height - 1;

                    uint32_t *tp = (uint32_t *)tex->pixels;
                    /* Rows are `pitch` apart, which is the width rounded up - not the width.
                     * Indexing by width here read progressively further into the previous row
                     * for any texture whose width is not a multiple of 64 pixels. */
                    const size_t trow = tex->pitch ? (size_t)tex->pitch : (size_t)tex->width;
                    uint32_t tc = tp[(size_t)ty * trow + (size_t)tx];
                    float tr = (float)(tc & 0xff) / 255.0f;
                    float tg = (float)((tc >> 8) & 0xff) / 255.0f;
                    float tb = (float)((tc >> 16) & 0xff) / 255.0f;
                    float ta = (float)((tc >> 24) & 0xff) / 255.0f;

                    if (ctx->tex_env_mode == GL_REPLACE) {
                        r = tr;
                        g = tg;
                        b = tb;
                        a = ta;
                    } else if (ctx->tex_env_mode == GL_ADD) {
                        r += tr;
                        g += tg;
                        b += tb;
                        a *= ta;
                        if (r > 1.0f) r = 1.0f;
                        if (g > 1.0f) g = 1.0f;
                        if (b > 1.0f) b = 1.0f;
                    } else if (ctx->tex_env_mode == GL_DECAL) {
                        r = r * (1.0f - ta) + tr * ta;
                        g = g * (1.0f - ta) + tg * ta;
                        b = b * (1.0f - ta) + tb * ta;
                    } else { /* GL_MODULATE default */
                        r *= tr;
                        g *= tg;
                        b *= tb;
                        a *= ta;
                    }
                }

                /* **The alpha test, which lived only in the hardware path until now.**
                 *
                 * `glAlphaFunc` is implemented on the GPU by patching a discard into the pixel
                 * shader, and this rasteriser knew nothing about it - so the same program drew
                 * one picture on the console and a different one on the host, with no error
                 * anywhere. gl1-probe found that on its first run, which is the whole reason
                 * the probe runs the identical suite on both paths.
                 *
                 * Placed after the texture environment, because GL applies the test to the
                 * *final* fragment alpha and a modulated texture changes it. */

                /* **Fog: after texturing, before the alpha test, and it does not touch alpha.**
                 *
                 * That ordering is the specification's - fog is part of shading the fragment,
                 * and every per-fragment test comes after it. Leaving alpha alone matters more
                 * than it looks: fogging alpha too would make a fogged fragment fail an alpha
                 * test it passes unfogged, so turning fog on would silently change which
                 * fragments a cut-out texture keeps. */
                if (fog_on) {
                    const float ff = b0 * v0->fog + b1 * v1->fog + b2 * v2->fog;
                    const float inv = 1.0f - ff;
                    r = ff * r + inv * fog_r;
                    g = ff * g + inv * fog_g;
                    b = ff * b + inv * fog_b;
                }

                if (ctx->cap_alpha_test) {
                    GLboolean keep;
                    switch (ctx->alpha_func) {
                        case GL_NEVER:    keep = GL_FALSE; break;
                        case GL_LESS:     keep = (GLboolean)(a <  ctx->alpha_ref); break;
                        case GL_EQUAL:    keep = (GLboolean)(a == ctx->alpha_ref); break;
                        case GL_LEQUAL:   keep = (GLboolean)(a <= ctx->alpha_ref); break;
                        case GL_GREATER:  keep = (GLboolean)(a >  ctx->alpha_ref); break;
                        case GL_NOTEQUAL: keep = (GLboolean)(a != ctx->alpha_ref); break;
                        case GL_GEQUAL:   keep = (GLboolean)(a >= ctx->alpha_ref); break;
                        default:          keep = GL_TRUE; break; /* GL_ALWAYS */
                    }
                    if (!keep) continue; /* and the deferred depth write never happens */
                }

                /* **Stencil sits here because GL's order is alpha test, then stencil, then
                 * depth.** A fragment the alpha test discards never reaches the stencil buffer at
                 * all - not even the fail operation - which is why this cannot live earlier.
                 *
                 * The stencil buffer is written *even when the stencil test fails*. That is the
                 * whole point of the feature: GL_INCR or GL_REPLACE on the fail path is how a
                 * stencil mask gets built in the first place, and an implementation that skipped
                 * the write on failure would make every shadow-volume and outline technique
                 * silently do nothing. */
                if (stencil_test && sb_row) {
                    uint8_t *sp = &sb_row[x];
                    const uint32_t masked_ref = (uint32_t)stencil_ref & stencil_vmask;
                    const uint32_t masked_val = (uint32_t)(*sp) & stencil_vmask;
                    GLboolean spass;
                    switch (stencil_func) {
                        case GL_NEVER:    spass = GL_FALSE; break;
                        case GL_LESS:     spass = (GLboolean)(masked_ref <  masked_val); break;
                        case GL_LEQUAL:   spass = (GLboolean)(masked_ref <= masked_val); break;
                        case GL_GREATER:  spass = (GLboolean)(masked_ref >  masked_val); break;
                        case GL_GEQUAL:   spass = (GLboolean)(masked_ref >= masked_val); break;
                        case GL_EQUAL:    spass = (GLboolean)(masked_ref == masked_val); break;
                        case GL_NOTEQUAL: spass = (GLboolean)(masked_ref != masked_val); break;
                        default:          spass = GL_TRUE; break; /* GL_ALWAYS */
                    }
                    if (!spass) {
                        gl_stencil_apply(sp, stencil_op_fail, stencil_ref, stencil_wmask);
                        continue; /* no colour, and no depth - the deferred write is abandoned */
                    }
                    gl_stencil_apply(sp, depth_failed ? stencil_op_zfail : stencil_op_zpass,
                                     stencil_ref, stencil_wmask);
                }
                /* The depth test's verdict, honoured now that stencil has had its say. */
                if (depth_failed) continue;

                if (depth_pending && db_row) db_row[x] = z;

                uint32_t ir = (uint32_t)(r * 255.0f + 0.5f);
                uint32_t ig = (uint32_t)(g * 255.0f + 0.5f);
                uint32_t ib = (uint32_t)(b * 255.0f + 0.5f);
                uint32_t ia = (uint32_t)(a * 255.0f + 0.5f);
                if (ir > 255) ir = 255;
                if (ig > 255) ig = 255;
                if (ib > 255) ib = 255;
                if (ia > 255) ia = 255;

                if (fb_row) {
                    if (blend) {
                        uint32_t dst = fb_row[x];
                        float dr = (float)((dst >> 16) & 0xff) / 255.0f;
                        float dg = (float)((dst >> 8) & 0xff) / 255.0f;
                        float db_col = (float)(dst & 0xff) / 255.0f;
                        float da = (float)((dst >> 24) & 0xff) / 255.0f;

                        float sfr = get_blend_factor(ctx->blend_src, r, g, b, a, dr, dg, db_col, da, 0);
                        float sfg = get_blend_factor(ctx->blend_src, r, g, b, a, dr, dg, db_col, da, 1);
                        float sfb = get_blend_factor(ctx->blend_src, r, g, b, a, dr, dg, db_col, da, 2);
                        float sfa = get_blend_factor(ctx->blend_src_alpha, r, g, b, a, dr, dg, db_col, da, 3);

                        float dfr = get_blend_factor(ctx->blend_dst, r, g, b, a, dr, dg, db_col, da, 0);
                        float dfg = get_blend_factor(ctx->blend_dst, r, g, b, a, dr, dg, db_col, da, 1);
                        float dfb = get_blend_factor(ctx->blend_dst, r, g, b, a, dr, dg, db_col, da, 2);
                        float dfa = get_blend_factor(ctx->blend_dst_alpha, r, g, b, a, dr, dg, db_col, da, 3);

                        /* **The blend equation, which until now was stored and used by
                         * nothing.** `glBlendEquation` set a field that the attribute stack
                         * saved, `glGetIntegerv` reported and `glContextCreate` defaulted -
                         * and neither this rasteriser nor the hardware register emission ever
                         * read it. `glBlendEquation(GL_FUNC_SUBTRACT)` returned clean and added.
                         *
                         * That is a worse shape than the alpha test and depth range were: those
                         * at least worked on the hardware path. This worked nowhere, which is
                         * the silent lie D009 refuses everywhere else in this library.
                         *
                         * **GL_MIN and GL_MAX ignore the factors entirely**, which is the part
                         * an implementation that treats them as another sign gets wrong. */
                        float res_r, res_g, res_b, res_a;
                        switch (ctx->blend_equation) {
                            case GL_FUNC_SUBTRACT:
                                res_r = r * sfr - dr * dfr;
                                res_g = g * sfg - dg * dfg;
                                res_b = b * sfb - db_col * dfb;
                                res_a = a * sfa - da * dfa;
                                break;
                            case GL_FUNC_REVERSE_SUBTRACT:
                                res_r = dr * dfr - r * sfr;
                                res_g = dg * dfg - g * sfg;
                                res_b = db_col * dfb - b * sfb;
                                res_a = da * dfa - a * sfa;
                                break;
                            case GL_MIN:
                                res_r = r < dr ? r : dr;
                                res_g = g < dg ? g : dg;
                                res_b = b < db_col ? b : db_col;
                                res_a = a < da ? a : da;
                                break;
                            case GL_MAX:
                                res_r = r > dr ? r : dr;
                                res_g = g > dg ? g : dg;
                                res_b = b > db_col ? b : db_col;
                                res_a = a > da ? a : da;
                                break;
                            default: /* GL_FUNC_ADD */
                                res_r = r * sfr + dr * dfr;
                                res_g = g * sfg + dg * dfg;
                                res_b = b * sfb + db_col * dfb;
                                res_a = a * sfa + da * dfa;
                                break;
                        }

                        if (res_r < 0.0f) res_r = 0.0f; else if (res_r > 1.0f) res_r = 1.0f;
                        if (res_g < 0.0f) res_g = 0.0f; else if (res_g > 1.0f) res_g = 1.0f;
                        if (res_b < 0.0f) res_b = 0.0f; else if (res_b > 1.0f) res_b = 1.0f;
                        if (res_a < 0.0f) res_a = 0.0f; else if (res_a > 1.0f) res_a = 1.0f;

                        ir = (uint32_t)(res_r * 255.0f + 0.5f);
                        ig = (uint32_t)(res_g * 255.0f + 0.5f);
                        ib = (uint32_t)(res_b * 255.0f + 0.5f);
                        ia = (uint32_t)(res_a * 255.0f + 0.5f);
                    }

                    uint32_t old_dst = fb_row[x];
                    uint32_t new_px = 0;
                    new_px |= ctx->color_mask[0] ? (ir << 16) : (old_dst & 0x00ff0000u);
                    new_px |= ctx->color_mask[1] ? (ig << 8)  : (old_dst & 0x0000ff00u);
                    new_px |= ctx->color_mask[2] ? ib         : (old_dst & 0x000000ffu);
                    new_px |= ctx->color_mask[3] ? (ia << 24) : (old_dst & 0xff000000u);
                    fb_row[x] = new_px;
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Client Arrays Draw Dispatches
 * ------------------------------------------------------------------------- */

#endif /* OOPS_HOST_BUILD */

static void fetch_vertex(const gl_context_t *ctx, int idx, gl_vertex_t *out) {
    /* **Each array's base comes from gl_array_base, not from its `pointer` field.** With a
     * buffer object bound that field holds a byte *offset*, and an offset of zero is both legal
     * and indistinguishable from the null pointer the old guards tested for - so a buffer whose
     * data starts at offset 0 would have read as "no array" and drawn the default attribute. */
    const uint8_t *base_v = gl_array_base(ctx, &ctx->array_vertex);
    const uint8_t *base_c = gl_array_base(ctx, &ctx->array_color);
    const uint8_t *base_t = gl_array_base(ctx, &ctx->array_texcoord);
    const uint8_t *base_n = gl_array_base(ctx, &ctx->array_normal);

    /* Position */
    if (ctx->array_vertex.enabled && base_v) {
        int stride = ctx->array_vertex.stride ? ctx->array_vertex.stride : (ctx->array_vertex.size * (int)sizeof(float));
        const uint8_t *ptr = base_v + (idx * stride);
        const float *fp = (const float *)ptr;
        out->x = fp[0];
        out->y = (ctx->array_vertex.size > 1) ? fp[1] : 0.0f;
        out->z = (ctx->array_vertex.size > 2) ? fp[2] : 0.0f;
        out->w = (ctx->array_vertex.size > 3) ? fp[3] : 1.0f;
    } else {
        out->x = 0.0f; out->y = 0.0f; out->z = 0.0f; out->w = 1.0f;
    }

    /* Color */
    if (ctx->array_color.enabled && base_c) {
        int stride = ctx->array_color.stride ? ctx->array_color.stride :
                     (ctx->array_color.type == GL_UNSIGNED_BYTE ? ctx->array_color.size : (ctx->array_color.size * (int)sizeof(float)));
        const uint8_t *ptr = base_c + (idx * stride);
        if (ctx->array_color.type == GL_UNSIGNED_BYTE) {
            out->r = (float)ptr[0] / 255.0f;
            out->g = (float)ptr[1] / 255.0f;
            out->b = (float)ptr[2] / 255.0f;
            out->a = (ctx->array_color.size > 3) ? ((float)ptr[3] / 255.0f) : 1.0f;
        } else {
            const float *fp = (const float *)ptr;
            out->r = fp[0];
            out->g = fp[1];
            out->b = fp[2];
            out->a = (ctx->array_color.size > 3) ? fp[3] : 1.0f;
        }
    } else {
        out->r = ctx->cur_color[0];
        out->g = ctx->cur_color[1];
        out->b = ctx->cur_color[2];
        out->a = ctx->cur_color[3];
    }

    /* Texcoord */
    if (ctx->array_texcoord.enabled && base_t) {
        int stride = ctx->array_texcoord.stride ? ctx->array_texcoord.stride : (ctx->array_texcoord.size * (int)sizeof(float));
        const uint8_t *ptr = base_t + (idx * stride);
        const float *fp = (const float *)ptr;
        out->u = fp[0];
        out->v = (ctx->array_texcoord.size > 1) ? fp[1] : 0.0f;
    } else {
        out->u = ctx->cur_texcoord[0];
        out->v = ctx->cur_texcoord[1];
    }

    /* Normal */
    if (ctx->array_normal.enabled && base_n) {
        int stride = ctx->array_normal.stride ? ctx->array_normal.stride : (3 * (int)sizeof(float));
        const uint8_t *ptr = base_n + (idx * stride);
        const float *fp = (const float *)ptr;
        out->nx = fp[0]; out->ny = fp[1]; out->nz = fp[2];
    } else {
        out->nx = ctx->cur_normal[0];
        out->ny = ctx->cur_normal[1];
        out->nz = ctx->cur_normal[2];
    }

    /* **Generation applies here too.** It is a property of the coordinate, not of the way the
     * vertex arrived, so a program that enables GL_TEXTURE_GEN_S and then draws from arrays gets
     * the same generated coordinate an immediate-mode vertex would - and it overrides the
     * texture-coordinate array above, exactly as it overrides glTexCoord. Applied after the
     * normal is read because sphere mapping needs it. */
    gl_apply_texgen(ctx, out);
}

/* `glArrayElement(i)` - one vertex pulled out of the enabled arrays, inside glBegin/glEnd.
 *
 * The bridge between the two ways of feeding geometry: a program keeps its data in arrays but
 * still wants to choose the primitive assembly by hand. Everything it needs is already here -
 * `fetch_vertex` reads whichever arrays are enabled and falls back to the current colour,
 * normal and texture coordinate for the ones that are not, which is exactly what the
 * specification says this does.
 *
 * Refused inside a display list for the same reason glDrawArrays is: a compiled list must
 * dereference the arrays at *compile* time, and recording the index to read later would replay
 * whatever the array holds then.
 */
void glArrayElement(GLint i) {
    if (gl_list_refuse()) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (i < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    /* Outside glBegin/glEnd this has nowhere to put the vertex. GL calls that undefined rather
     * than an error, and doing nothing is the reading that cannot corrupt anything. */
    if (!ctx->imm_active) return;
    if (ctx->imm_count >= OOPS_GL_MAX_IMMEDIATE_VERTS) return;

    fetch_vertex(ctx, i, &ctx->imm_verts[ctx->imm_count++]);
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    // Cannot be compiled into a list: the specification has a list dereference the client
    // arrays at *compile* time, and storing the pointer to read later would draw whatever the
    // array holds then - a different picture from the one that was compiled.
    if (gl_list_refuse()) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    /* A negative count is the caller's arithmetic having gone wrong, and GL says so with
     * GL_INVALID_VALUE. A count of zero is legal and draws nothing, so the two are separated
     * here - they used to share one silent `return`. */
    if (count < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!gl_accept_mode(ctx, mode)) return;
    if (count == 0) return;

    gl_vertex_t v0, v1, v2, v3;

    switch (mode) {
        case GL_TRIANGLES:
            for (GLint i = 0; i + 2 < count; i += 3) {
                fetch_vertex(ctx, first + i, &v0);
                fetch_vertex(ctx, first + i + 1, &v1);
                fetch_vertex(ctx, first + i + 2, &v2);
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v2);
            }
            break;
        case GL_QUADS:
            for (GLint i = 0; i + 3 < count; i += 4) {
                fetch_vertex(ctx, first + i, &v0);
                fetch_vertex(ctx, first + i + 1, &v1);
                fetch_vertex(ctx, first + i + 2, &v2);
                fetch_vertex(ctx, first + i + 3, &v3);
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v2);
                gl_draw_primitive_triangle(ctx, &v0, &v2, &v3);
            }
            break;
        case GL_TRIANGLE_STRIP:
            for (GLint i = 0; i + 2 < count; i++) {
                if (i & 1) {
                    fetch_vertex(ctx, first + i + 1, &v0);
                    fetch_vertex(ctx, first + i, &v1);
                    fetch_vertex(ctx, first + i + 2, &v2);
                } else {
                    fetch_vertex(ctx, first + i, &v0);
                    fetch_vertex(ctx, first + i + 1, &v1);
                    fetch_vertex(ctx, first + i + 2, &v2);
                }
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v2);
            }
            break;
        case GL_TRIANGLE_FAN:
        case GL_POLYGON:
            fetch_vertex(ctx, first, &v0);
            for (GLint i = 1; i + 1 < count; i++) {
                fetch_vertex(ctx, first + i, &v1);
                fetch_vertex(ctx, first + i + 1, &v2);
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v2);
            }
            break;
        case GL_QUAD_STRIP:
            for (GLint i = 0; i + 3 < count; i += 2) {
                fetch_vertex(ctx, first + i, &v0);
                fetch_vertex(ctx, first + i + 1, &v1);
                fetch_vertex(ctx, first + i + 2, &v2);
                fetch_vertex(ctx, first + i + 3, &v3);
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v3);
                gl_draw_primitive_triangle(ctx, &v0, &v3, &v2);
            }
            break;
        /* **The array path expands points and lines the same way immediate mode does.** Accepting
         * the mode in `gl_mode_is_drawable` and not handling it here would draw nothing and raise
         * nothing, which is the silent-success failure this port refuses. */
        case GL_POINTS:
        case GL_LINES:
        case GL_LINE_STRIP:
        case GL_LINE_LOOP: {
            gl_update_mvp(ctx);
            gl_mat4_t inv_mvp;
            if (!mat4_invert(&inv_mvp, &ctx->mvp)) break;
            if (mode == GL_POINTS) {
                for (GLint i = 0; i < count; i++) {
                    fetch_vertex(ctx, first + i, &v0);
                    gl_draw_point_square(ctx, &inv_mvp, &v0);
                }
            } else if (mode == GL_LINES) {
                for (GLint i = 0; i + 1 < count; i += 2) {
                    fetch_vertex(ctx, first + i, &v0);
                    fetch_vertex(ctx, first + i + 1, &v1);
                    gl_draw_line_segment(ctx, &inv_mvp, &v0, &v1);
                }
            } else {
                for (GLint i = 0; i + 1 < count; i++) {
                    fetch_vertex(ctx, first + i, &v0);
                    fetch_vertex(ctx, first + i + 1, &v1);
                    gl_draw_line_segment(ctx, &inv_mvp, &v0, &v1);
                }
                if (mode == GL_LINE_LOOP && count > 2) {
                    fetch_vertex(ctx, first + count - 1, &v0);
                    fetch_vertex(ctx, first, &v1);
                    gl_draw_line_segment(ctx, &inv_mvp, &v0, &v1);
                }
            }
            break;
        }
        default: break;
    }
}

/* `glDrawRangeElements` (GL 1.2) is glDrawElements plus a promise about the index range, which
 * an implementation may use to pre-transform that slice of the arrays and may equally ignore.
 * This one ignores it - and that is the specification's own allowance, not a shortcut.
 *
 * The range is still **checked**: a program that promises indices in [start, end] and then
 * passes one outside it has a bug, and GL_INVALID_VALUE for start > end is required. Reading
 * the indices to verify every one against the range would cost more than the hint saves. */
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
    if (gl_list_refuse()) return;
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    if (count < 0) {
        gl_record_error(ctx, GL_INVALID_VALUE);
        return;
    }
    if (!gl_accept_mode(ctx, mode)) return;
    /* **Before the reader runs, not inside it.** The index reader below selects 16-bit and
     * 8-bit and falls through to 32-bit for anything else, so an unchecked type read past the
     * end of a byte-indexed array rather than merely returning nonsense. */
    if (!gl_index_type_is_readable(type)) {
        gl_record_error(ctx, GL_INVALID_ENUM);
        return;
    }

    /* **With an element buffer bound, `indices` is a byte offset, and zero is a real one.**
     * The null check below therefore cannot come first: a program drawing from the start of an
     * index buffer passes NULL and means it. */
    if (ctx->bound_element_array_buffer != 0u) {
        const gl_client_array_t elements = {
            0, 0, 0, indices, GL_TRUE, ctx->bound_element_array_buffer
        };
        indices = gl_array_base(ctx, &elements);
        if (!indices) {
            /* Bound to a buffer with no storage. Nothing to read; the draw is skipped rather
             * than run against an address derived from NULL. */
            gl_record_error(ctx, GL_INVALID_OPERATION);
            return;
        }
    }
    if (count == 0 || !indices) return;

    gl_vertex_t v0, v1, v2, v3;

    #define GET_INDEX(i) \
        ((type == GL_UNSIGNED_SHORT) ? (int)((const uint16_t *)indices)[i] : \
        ((type == GL_UNSIGNED_BYTE)  ? (int)((const uint8_t *)indices)[i] : \
                                       (int)((const uint32_t *)indices)[i]))

    switch (mode) {
        case GL_TRIANGLES:
            for (GLsizei i = 0; i + 2 < count; i += 3) {
                fetch_vertex(ctx, GET_INDEX(i), &v0);
                fetch_vertex(ctx, GET_INDEX(i + 1), &v1);
                fetch_vertex(ctx, GET_INDEX(i + 2), &v2);
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v2);
            }
            break;
        case GL_QUADS:
            for (GLsizei i = 0; i + 3 < count; i += 4) {
                fetch_vertex(ctx, GET_INDEX(i), &v0);
                fetch_vertex(ctx, GET_INDEX(i + 1), &v1);
                fetch_vertex(ctx, GET_INDEX(i + 2), &v2);
                fetch_vertex(ctx, GET_INDEX(i + 3), &v3);
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v2);
                gl_draw_primitive_triangle(ctx, &v0, &v2, &v3);
            }
            break;
        case GL_TRIANGLE_STRIP:
            for (GLsizei i = 0; i + 2 < count; i++) {
                if (i & 1) {
                    fetch_vertex(ctx, GET_INDEX(i + 1), &v0);
                    fetch_vertex(ctx, GET_INDEX(i), &v1);
                    fetch_vertex(ctx, GET_INDEX(i + 2), &v2);
                } else {
                    fetch_vertex(ctx, GET_INDEX(i), &v0);
                    fetch_vertex(ctx, GET_INDEX(i + 1), &v1);
                    fetch_vertex(ctx, GET_INDEX(i + 2), &v2);
                }
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v2);
            }
            break;
        case GL_TRIANGLE_FAN:
        case GL_POLYGON:
            fetch_vertex(ctx, GET_INDEX(0), &v0);
            for (GLsizei i = 1; i + 1 < count; i++) {
                fetch_vertex(ctx, GET_INDEX(i), &v1);
                fetch_vertex(ctx, GET_INDEX(i + 1), &v2);
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v2);
            }
            break;
        case GL_QUAD_STRIP:
            for (GLsizei i = 0; i + 3 < count; i += 2) {
                fetch_vertex(ctx, GET_INDEX(i), &v0);
                fetch_vertex(ctx, GET_INDEX(i + 1), &v1);
                fetch_vertex(ctx, GET_INDEX(i + 2), &v2);
                fetch_vertex(ctx, GET_INDEX(i + 3), &v3);
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v3);
                gl_draw_primitive_triangle(ctx, &v0, &v3, &v2);
            }
            break;
        /* The third switch. All three must agree about which modes draw, which is what the
         * comment on the refusal test means by "three switches used to disagree about this". */
        case GL_POINTS:
        case GL_LINES:
        case GL_LINE_STRIP:
        case GL_LINE_LOOP: {
            gl_update_mvp(ctx);
            gl_mat4_t inv_mvp;
            if (!mat4_invert(&inv_mvp, &ctx->mvp)) break;
            if (mode == GL_POINTS) {
                for (GLsizei i = 0; i < count; i++) {
                    fetch_vertex(ctx, GET_INDEX(i), &v0);
                    gl_draw_point_square(ctx, &inv_mvp, &v0);
                }
            } else if (mode == GL_LINES) {
                for (GLsizei i = 0; i + 1 < count; i += 2) {
                    fetch_vertex(ctx, GET_INDEX(i), &v0);
                    fetch_vertex(ctx, GET_INDEX(i + 1), &v1);
                    gl_draw_line_segment(ctx, &inv_mvp, &v0, &v1);
                }
            } else {
                for (GLsizei i = 0; i + 1 < count; i++) {
                    fetch_vertex(ctx, GET_INDEX(i), &v0);
                    fetch_vertex(ctx, GET_INDEX(i + 1), &v1);
                    gl_draw_line_segment(ctx, &inv_mvp, &v0, &v1);
                }
                if (mode == GL_LINE_LOOP && count > 2) {
                    fetch_vertex(ctx, GET_INDEX(count - 1), &v0);
                    fetch_vertex(ctx, GET_INDEX(0), &v1);
                    gl_draw_line_segment(ctx, &inv_mvp, &v0, &v1);
                }
            }
            break;
        }
        default: break;
    }
    #undef GET_INDEX
}
