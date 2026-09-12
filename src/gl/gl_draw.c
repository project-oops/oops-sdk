/*
 * oops-gl: Vertex arrays, immediate mode, primitive processing, and rasterization
 */

#include "gl_internal.h"

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
        default: break;
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
        default: break;
    }
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->array_vertex.size = size;
    ctx->array_vertex.type = type;
    ctx->array_vertex.stride = stride;
    ctx->array_vertex.pointer = pointer;
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->array_color.size = size;
    ctx->array_color.type = type;
    ctx->array_color.stride = stride;
    ctx->array_color.pointer = pointer;
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->array_texcoord.size = size;
    ctx->array_texcoord.type = type;
    ctx->array_texcoord.stride = stride;
    ctx->array_texcoord.pointer = pointer;
}

void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->array_normal.size = 3;
    ctx->array_normal.type = type;
    ctx->array_normal.stride = stride;
    ctx->array_normal.pointer = pointer;
}

/* -------------------------------------------------------------------------
 * Immediate Mode API
 * ------------------------------------------------------------------------- */

void glBegin(GLenum mode) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
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
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || !ctx->imm_active) return;
    if (ctx->imm_count >= GL_MAX_IMMEDIATE_VERTS) return;

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
}

void glColor3f(GLfloat red, GLfloat green, GLfloat blue) {
    glColor4f(red, green, blue, 1.0f);
}

void glColor3fv(const GLfloat *v) {
    if (v) glColor4f(v[0], v[1], v[2], 1.0f);
}

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
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

void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) {
    glColor4f((float)red / 255.0f, (float)green / 255.0f,
              (float)blue / 255.0f, (float)alpha / 255.0f);
}

void glTexCoord2f(GLfloat s, GLfloat t) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->cur_texcoord[0] = (float)s;
    ctx->cur_texcoord[1] = (float)t;
}

void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx) return;
    ctx->cur_normal[0] = (float)nx;
    ctx->cur_normal[1] = (float)ny;
    ctx->cur_normal[2] = (float)nz;
}

void glNormal3fv(const GLfloat *v) {
    if (v) glNormal3f(v[0], v[1], v[2]);
}

void glEnd(void) {
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
        default: break;
    }

    ctx->imm_count = 0;
}

/* -------------------------------------------------------------------------
 * Triangle Pipeline & Rasterizer
 * ------------------------------------------------------------------------- */


#ifndef OOPS_HOST_BUILD
static void gl_hw_begin_frame(gl_context_t *ctx) {
    uint32_t *dw = ctx->dcb_mem;
    uint64_t color_gpu = (uint64_t)(uintptr_t)ctx->framebuffer;
    uint64_t payload_va = (uint64_t)(uintptr_t)ctx->gpu_payload;

    uint32_t w = ctx->width ? ctx->width : 1920;
    uint32_t h = ctx->height ? ctx->height : 1080;

    static const struct {
        uint32_t reg;
        uint32_t val;
    } ctx_regs[] = {
        {0x318u, 0}, /* CB_COLOR0_BASE */
        {0x390u, 0}, /* CB_COLOR0_BASE_EXT */
        {0x31bu, 0x00000000u}, /* CB_COLOR0_VIEW */
        {0x31cu, 0x000180a8u}, /* CB_COLOR0_INFO: COLOR_8_8_8_8, LINEAR_GENERAL, UNORM */
        {0x31du, 0x00000000u}, /* CB_COLOR0_ATTRIB: 0 */
        {0x31eu, 0x00000000u}, /* CB_COLOR0_DCC_CONTROL: disabled */
        {0x3b0u, (1919u << 14) | 1079u}, /* CB_COLOR0_ATTRIB2 */
        {0x202u, 0x00cc0010u}, /* CB_COLOR_CONTROL: CB_NORMAL, ROP3_COPY */
        {0x08eu, 0x0000000fu}, /* CB_TARGET_MASK */
        {0x08fu, 0x0000000fu}, /* CB_SHADER_MASK */
        {0x1e0u, 0x00000000u}, /* CB_BLEND0_CONTROL: blending disabled */
        {0x000u, 0x00000000u}, /* DB_RENDER_CONTROL */
        {0x002u, 0x00000000u}, /* DB_DEPTH_VIEW */
        {0x003u, 0x00000000u}, /* DB_RENDER_OVERRIDE */
        {0x004u, 0x08000000u}, /* DB_RENDER_OVERRIDE2 */
        {0x005u, 0x00000000u}, /* DB_HTILE_DATA_BASE */
        {0x007u, 0},           /* DB_DEPTH_SIZE_XY */
        {0x008u, 0x00000000u}, /* DB_DEPTH_BOUNDS_MIN */
        {0x009u, 0x00000000u}, /* DB_DEPTH_BOUNDS_MAX */
        {0x00au, 0x00000000u}, /* DB_STENCIL_CLEAR */
        {0x00bu, 0x00000000u}, /* DB_DEPTH_CLEAR */
        {0x00eu, 0x00000002u}, /* DB_DFSM_CONTROL */
        {0x010u, 0},           /* DB_Z_INFO */
        {0x011u, 0x20000180u}, /* DB_STENCIL_INFO */
        {0x012u, 0},           /* DB_Z_READ_BASE */
        {0x013u, 0x00000000u}, /* DB_STENCIL_READ_BASE */
        {0x014u, 0},           /* DB_Z_WRITE_BASE */
        {0x015u, 0x00000000u}, /* DB_STENCIL_WRITE_BASE */
        {0x01au, 0},           /* DB_Z_READ_BASE_HI */
        {0x01bu, 0x00000000u}, /* DB_STENCIL_READ_BASE_HI */
        {0x01cu, 0},           /* DB_Z_WRITE_BASE_HI */
        {0x01du, 0x00000000u}, /* DB_STENCIL_WRITE_BASE_HI */
        {0x01eu, 0x00000000u}, /* DB_HTILE_DATA_BASE_HI */
        {0x01fu, 0x00000000u}, /* DB_RMI_L2_CACHE_CONTROL */
        {0x200u, 0},           /* DB_DEPTH_CONTROL */
        {0x201u, 0x00130000u}, /* DB_EQAA */
        {0x203u, 0x00000000u}, /* DB_SHADER_CONTROL: LATE_Z */
        {0x2afu, 0x00040000u}, /* DB_HTILE_SURFACE */
        {0x08cu, 0xaa99aaaau}, /* PA_SC_EDGERULE */
        {0x1d4u, 0x000000ffu}, /* SX_PS_DOWNCONVERT_CONTROL */
        {0x291u, (128u << 22) | (128u << 11) | 256u}, /* VGT_GS_ONCHIP_CNTL */
        {0x29bu, 0x00000002u}, /* VGT_GS_OUT_PRIM_TYPE: TRILIST */
        {0x2d3u, 0x00000001u}, /* GE_NGG_SUBGRP_CNTL: PRIM_AMP=1 */
        {0x2d5u, 0x00c12010u}, /* VGT_SHADER_STAGES_EN: ES_EN | PRIMGEN_EN | GS_W32 | VS_W32 */
        {0x1ffu, 0x00000100u}, /* GE_MAX_OUTPUT_PER_SUBGROUP */
        {0x20eu, 0x00000078u}, /* PA_CL_NGG_CNTL */
        {0x2a1u, 0x00000000u}, /* VGT_PRIMITIVEID_EN */
        {0x2a6u, 0x00000000u}, /* VGT_DRAW_PAYLOAD_CNTL */
        {0x2adu, 0x00000000u}, /* VGT_REUSE_OFF */
        {0x2ceu, 0x00000400u}, /* VGT_GS_MAX_VERT_OUT */
        {0x2e4u, 0x00000004u}, /* VGT_GS_INSTANCE_CNT */
        {0x2d4u, 0x88101010u}, /* VGT_TESS_DISTRIBUTION */
        {0x103u, 0xffffffffu}, /* VGT_MULTI_PRIM_IB_RESET_INDX */
        {0x30eu, 0xffffffffu}, /* PA_SC_AA_MASK_X0Y0_X1Y0 */
        {0x30fu, 0xffffffffu}, /* PA_SC_AA_MASK_X0Y1_X1Y1 */
        {0x310u, 0x00000000u}, /* PA_SC_SHADER_CONTROL */
        {0x314u, 0x00000200u}, /* PA_SC_NGG_MODE_CNTL */
        {0x311u, 0x19fc0122u}, /* PA_SC_BINNER_CNTL_0 */
        {0x312u, 0x03ff0080u}, /* PA_SC_BINNER_CNTL_1 */
        {0x313u, 0x00100000u}, /* PA_SC_CONSERVATIVE_RASTERIZATION_CNTL */
        {0x00eu, 0x00000002u}, /* DB_DFSM_CONTROL */
        {0x280u, 0x00080008u}, /* PA_SU_POINT_SIZE */
        {0x281u, 0xffff0000u}, /* PA_SU_POINT_MINMAX */
        {0x282u, 0x00000008u}, /* PA_SU_LINE_CNTL */
        {0x2deu, 0x000001e9u}, /* PA_SU_POLY_OFFSET_DB_FMT_CNTL */
        {0x00cu, 0x00000000u}, /* PA_SC_SCREEN_SCISSOR_TL */
        {0x00du, 0x04380780u}, /* PA_SC_SCREEN_SCISSOR_BR (1920x1080) */
        {0x081u, 0x80000000u}, /* PA_SC_WINDOW_SCISSOR_TL (WINDOW_OFFSET_DISABLE) */
        {0x082u, 0x04380780u}, /* PA_SC_WINDOW_SCISSOR_BR (1920x1080) */
        {0x090u, 0x80000000u}, /* PA_SC_GENERIC_SCISSOR_TL (WINDOW_OFFSET_DISABLE) */
        {0x091u, 0x04380780u}, /* PA_SC_GENERIC_SCISSOR_BR (1920x1080) */
        {0x094u, 0x80000000u}, /* PA_SC_VPORT_SCISSOR_0_TL (WINDOW_OFFSET_DISABLE) */
        {0x095u, 0x04380780u}, /* PA_SC_VPORT_SCISSOR_0_BR (1920x1080) */
        {0x0b4u, 0x00000000u}, /* PA_SC_VPORT_ZMIN_0: 0.0f */
        {0x0b5u, 0x3f800000u}, /* PA_SC_VPORT_ZMAX_0: 1.0f */
        {0x10fu, 0x44700000u}, /* PA_CL_VPORT_XSCALE: 960.0f */
        {0x110u, 0x44700000u}, /* PA_CL_VPORT_XOFFSET: 960.0f */
        {0x111u, 0x44070000u}, /* PA_CL_VPORT_YSCALE: 540.0f */
        {0x112u, 0x44070000u}, /* PA_CL_VPORT_YOFFSET: 540.0f */
        {0x113u, 0x3f000000u}, /* PA_CL_VPORT_ZSCALE: 0.5f */
        {0x114u, 0x3f000000u}, /* PA_CL_VPORT_ZOFFSET: 0.5f */
        {0x083u, 0x0000ffffu}, /* PA_SC_CLIPRECT_RULE */
        {0x084u, 0x00000000u}, /* PA_SC_CLIPRECT_0_TL */
        {0x085u, 0x20002000u}, /* PA_SC_CLIPRECT_0_BR (8192x8192) */
        {0x204u, 0x00000000u}, /* PA_CL_CLIP_CNTL: standard clipping */
        {0x206u, 0x0000043fu}, /* PA_CL_VTE_CNTL */
        {0x207u, 0x00000000u}, /* PA_CL_VS_OUT_CNTL */
        {0x2fau, 0x40800000u}, /* PA_CL_GB_VERT_CLIP_ADJ: 4.0f */
        {0x2fbu, 0x40800000u}, /* PA_CL_GB_VERT_DISC_ADJ: 4.0f */
        {0x2fcu, 0x40800000u}, /* PA_CL_GB_HORZ_CLIP_ADJ: 4.0f */
        {0x2fdu, 0x40800000u}, /* PA_CL_GB_HORZ_DISC_ADJ: 4.0f */
        {0x205u, 0x00000240u}, /* PA_SU_SC_MODE_CNTL: no cull, face=0, poly=trilist */
        {0x20cu, 0x00000000u}, /* PA_SU_SMALL_PRIM_FILTER_CNTL: disabled */
        {0x292u, 0x00000022u}, /* PA_SC_MODE_CNTL_0 */
        {0x293u, 0x760201b5u}, /* PA_SC_MODE_CNTL_1 */
        {0x2f8u, 0x20000000u}, /* PA_SC_AA_CONFIG */
        {0x2f9u, 0x0000002du}, /* PA_SU_VTX_CNTL */
        {0x191u, 0x00000000u}, /* SPI_PS_INPUT_CNTL_0: Offset 0, smooth (Color) */
        {0x192u, 0x00000001u}, /* SPI_PS_INPUT_CNTL_1: Offset 1, smooth (UV) */
        {0x1b1u, 0x00000001u}, /* SPI_VS_OUT_CONFIG: 2 PC Exports (param0, param1), NO_PC_EXPORT=0 */
        {0x1c2u, 0x00000001u}, /* SPI_SHADER_IDX_FORMAT: IDX0 = 1COMP */
        {0x1c3u, 0x00000004u}, /* SPI_SHADER_POS_FORMAT: POS0 = 4COMP */
        {0x1c4u, 0x00000000u}, /* SPI_SHADER_Z_FORMAT */
        {0x1c5u, 0x00000009u}, /* SPI_SHADER_COL_FORMAT: COL0 = 32_ABGR */
        {0x1b3u, 0x00000002u}, /* SPI_PS_INPUT_ENA: PERSP_CENTER_ENA */
        {0x1b4u, 0x00000002u}, /* SPI_PS_INPUT_ADDR: PERSP_CENTER_ENA */
        {0x1b5u, 0x00000000u}, /* SPI_INTERP_CONTROL_0: 0 */
        {0x1b6u, 0x00008002u}, /* SPI_PS_IN_CONTROL: PS_W32_EN, NUM_INTERP=2 */
        {0x1b8u, 0x01000000u}, /* SPI_BARYC_CNTL: FRONT_FACE_ALL_BITS */
    };

    uint64_t depth_gpu = (uint64_t)(uintptr_t)ctx->depth_buffer;
    int depth_on = (ctx->cap_depth_test && ctx->depth_buffer) ? 1 : 0;
    uint32_t zfunc = (ctx->depth_func >= 0x0200 && ctx->depth_func <= 0x0207) ? (ctx->depth_func - 0x0200) : 1u;
    uint32_t z_write = ctx->depth_mask ? 1u : 0u;
    uint32_t depth_ctrl = (1u << 1) | (z_write << 2) | ((zfunc & 0x7u) << 4);
    uint32_t z_info = 3u | 0x80000180u; /* Z_32_FLOAT | SW_MODE=24 | ZRANGE_PRECISION */

    for (size_t i = 0; i < sizeof(ctx_regs) / sizeof(ctx_regs[0]); i++) {
        uint32_t reg = ctx_regs[i].reg;
        uint32_t val = ctx_regs[i].val;
        if (reg == 0x318u) {
            val = (uint32_t)(color_gpu >> 8);
        } else if (reg == 0x390u) {
            val = (uint32_t)(color_gpu >> 40);
        } else if (reg == 0x3b0u) {
            val = ((w - 1) << 14) | (h - 1);
        } else if (reg == 0x007u) {
            val = ((h - 1) << 16) | (w - 1);
        } else if (reg == 0x010u) {
            val = depth_on ? z_info : 0u;
        } else if (reg == 0x012u || reg == 0x014u) {
            val = depth_on ? (uint32_t)(depth_gpu >> 8) : 0u;
        } else if (reg == 0x01au || reg == 0x01cu) {
            val = depth_on ? (uint32_t)(depth_gpu >> 40) : 0u;
        } else if (reg == 0x1e0u) {
            val = ctx->cap_blend ? 0x00002504u : 0u;
        } else if (reg == 0x200u) {
            val = depth_on ? depth_ctrl : 0u;
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

    static const struct {
        uint32_t base_reg;
        uint64_t va_offset;
        uint32_t rsrc1;
        uint32_t rsrc2;
    } stages[] = {
        {0x08u, 0x200u, 0x000c0010u, 0x00000002u},  /* PS: 2 User SGPRs (s[0:1] = desc_table) */
        {0x48u, 0x000u, 0x000c0010u, 0x00000008u},  /* VS: 4 User SGPRs */
        {0x88u, 0x000u, 0x200c0010u, 0x00000008u},  /* GS / NGG: 4 User SGPRs (s0 = vbo_offset) */
        {0xc8u, 0x000u, 0x000c0010u, 0x00000008u},  /* ES */
        {0x108u, 0x000u, 0x000c0010u, 0x00000008u}, /* HS */
        {0x148u, 0x000u, 0x000c0010u, 0x00000008u}, /* LS */
    };
    uint64_t ps_init_offset = (ctx->cap_texture_2d && ctx->bound_texture_2d > 0) ? 0x200u : 0x300u;
    for (size_t s = 0; s < sizeof(stages) / sizeof(stages[0]); s++) {
        uint32_t base_reg = stages[s].base_reg;
        uint64_t s_va = payload_va + ((s == 0) ? ps_init_offset : stages[s].va_offset);
        *dw++ = 0xc0017600u; *dw++ = base_reg;       *dw++ = (uint32_t)(s_va >> 8);
        *dw++ = 0xc0017600u; *dw++ = base_reg + 1u;  *dw++ = (uint32_t)(s_va >> 40);
        *dw++ = 0xc0017600u; *dw++ = base_reg + 2u;  *dw++ = stages[s].rsrc1;
        *dw++ = 0xc0017600u; *dw++ = base_reg + 3u;  *dw++ = stages[s].rsrc2;
    }

    *dw++ = 0xc0017600u; *dw++ = 0x007u; *dw++ = 0x003fffffu;
    *dw++ = 0xc0017600u; *dw++ = 0x001u; *dw++ = 0x0000ffffu;
    *dw++ = 0xc0017600u; *dw++ = 0x030u; *dw++ = 0x00000007u;
    *dw++ = 0xc0017600u; *dw++ = 0x046u; *dw++ = 0x003fffffu;
    *dw++ = 0xc0017600u; *dw++ = 0x041u; *dw++ = 0x0000ffffu;
    *dw++ = 0xc0017600u; *dw++ = 0x087u; *dw++ = 0x003fffffu;
    *dw++ = 0xc0017600u; *dw++ = 0x081u; *dw++ = 0x0000ffffu;
    *dw++ = 0xc0017600u; *dw++ = 0x107u; *dw++ = 0x003fffffu;

    *dw++ = 0xc0002f00u; *dw++ = 1u;                          /* PACKET3_NUM_INSTANCES */
    *dw++ = 0xc0017900u; *dw++ = 0x242u; *dw++ = 0x4u;        /* mmVGT_PRIMITIVE_TYPE: TRILIST */
    *dw++ = 0xc0017900u; *dw++ = 0x25bu; *dw++ = 0x00020080u; /* mmGE_CNTL */
    *dw++ = 0xc0017900u; *dw++ = 0x260u; *dw++ = 0x000001ffu; /* mmGE_PC_ALLOC */

    ctx->dcb_words = (uint32_t)(dw - ctx->dcb_mem);
    ctx->hw_frame_active = GL_TRUE;
}
#endif

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
    for (int i = 0; i < GL_MAX_LIGHTS; i++) {
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

    gl_screen_vertex_t sv0, sv1, sv2;
    sv0.sx = ndc0[0] * vp_w_half + vp_ox;
    sv0.sy = (float)ctx->height - (ndc0[1] * vp_h_half + vp_oy); /* Y-flip for screen space */
    sv0.sz = ndc0[2] * 0.5f + 0.5f;
    sv0.inv_w = inv_w0;
    sv0.r = col0[0]; sv0.g = col0[1]; sv0.b = col0[2]; sv0.a = col0[3];
    sv0.u = v0->u; sv0.v = v0->v;

    sv1.sx = ndc1[0] * vp_w_half + vp_ox;
    sv1.sy = (float)ctx->height - (ndc1[1] * vp_h_half + vp_oy);
    sv1.sz = ndc1[2] * 0.5f + 0.5f;
    sv1.inv_w = inv_w1;
    sv1.r = col1[0]; sv1.g = col1[1]; sv1.b = col1[2]; sv1.a = col1[3];
    sv1.u = v1->u; sv1.v = v1->v;

    sv2.sx = ndc2[0] * vp_w_half + vp_ox;
    sv2.sy = (float)ctx->height - (ndc2[1] * vp_h_half + vp_oy);
    sv2.sz = ndc2[2] * 0.5f + 0.5f;
    sv2.inv_w = inv_w2;
    sv2.r = col2[0]; sv2.g = col2[1]; sv2.b = col2[2]; sv2.a = col2[3];
    sv2.u = v2->u; sv2.v = v2->v;

    /* 4. Backface culling via 2D signed area (screen coordinates) */
    float area = (sv1.sx - sv0.sx) * (sv2.sy - sv0.sy) - (sv1.sy - sv0.sy) * (sv2.sx - sv0.sx);

    if (ctx->cap_cull_face) {
        /* Note: with Y-flip, CCW in 3D becomes negative in screen space */
        GLboolean is_ccw = (area < 0.0f) ? GL_TRUE : GL_FALSE;
        if (ctx->front_face == GL_CW) is_ccw = !is_ccw;

        if (ctx->cull_mode == GL_BACK && !is_ccw) return;
        if (ctx->cull_mode == GL_FRONT && is_ccw) return;
        if (ctx->cull_mode == GL_FRONT_AND_BACK) return;
    }

    if (area > -1e-4f && area < 1e-4f) return; /* Degenerate */

#ifndef OOPS_HOST_BUILD
    /* 5. Hardware AGC Path (AMD RDNA2 GFX10.3) */
    if (ctx->use_hardware) {
        if (!ctx->hw_frame_active) {
            gl_hw_begin_frame(ctx);
        }

        if (ctx->dcb_words + 64 >= ctx->dcb_capacity_dw) {
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

        if (ctx->cap_texture_2d) {
            ps_va = payload_va + 0x200; /* Stage 5: Textured + Gouraud */
            if (ctx->bound_texture_2d > 0) {
                for (int ti = 0; ti < GL_MAX_TEXTURE_OBJECTS; ti++) {
                    if (ctx->textures[ti].used && ctx->textures[ti].id == ctx->bound_texture_2d) {
                        uint32_t *dt = (uint32_t *)((char *)ctx->gpu_payload + 0x900);
                        memcpy(dt, ctx->textures[ti].img_desc, 32);
                        memcpy(dt + 8, ctx->textures[ti].samp_desc, 16);
#if defined(__x86_64__)
                        __builtin_ia32_clflush((const void *)dt);
#endif
                        break;
                    }
                }
            }
        }

        *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_PGM_LO_PS */
        *dw++ = 0x08u;
        *dw++ = (uint32_t)(ps_va >> 8);
        *dw++ = 0xc0017600u; /* PACKET3_SET_SH_REG mmSPI_SHADER_PGM_HI_PS */
        *dw++ = 0x09u;
        *dw++ = (uint32_t)(ps_va >> 40);

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
#endif

    /* 6. Software Fallback Rasterizer */
    gl_rasterize_triangle(ctx, &sv0, &sv1, &sv2);
    ctx->triangles_drawn++;
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

    /* Check for bound and enabled texture */
    gl_texture_object_t *tex = NULL;
    if (ctx->cap_texture_2d && ctx->bound_texture_2d > 0) {
        for (int ti = 0; ti < GL_MAX_TEXTURE_OBJECTS; ti++) {
            if (ctx->textures[ti].used && ctx->textures[ti].id == ctx->bound_texture_2d) {
                tex = &ctx->textures[ti];
                break;
            }
        }
    }

    for (int y = min_y; y <= max_y; y++) {
        float py = (float)y + 0.5f;
        uint32_t *fb_row = fb ? &fb[(uint32_t)y * pitch] : NULL;
        float *db_row = db ? &db[(uint32_t)y * pitch] : NULL;

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

                /* Interpolate depth Z */
                float z = b0 * v0->sz + b1 * v1->sz + b2 * v2->sz;

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
                    if (!pass) continue;
                    if (depth_write) db_row[x] = z;
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
                    uint32_t tc = tp[(size_t)ty * (size_t)tex->width + (size_t)tx];
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

                        float res_r = r * sfr + dr * dfr;
                        float res_g = g * sfg + dg * dfg;
                        float res_b = b * sfb + db_col * dfb;
                        float res_a = a * sfa + da * dfa;

                        if (res_r < 0.0f) res_r = 0.0f; else if (res_r > 1.0f) res_r = 1.0f;
                        if (res_g < 0.0f) res_g = 0.0f; else if (res_g > 1.0f) res_g = 1.0f;
                        if (res_b < 0.0f) res_b = 0.0f; else if (res_b > 1.0f) res_b = 1.0f;
                        if (res_a < 0.0f) res_a = 0.0f; else if (res_a > 1.0f) res_a = 1.0f;

                        ir = (uint32_t)(res_r * 255.0f + 0.5f);
                        ig = (uint32_t)(res_g * 255.0f + 0.5f);
                        ib = (uint32_t)(res_b * 255.0f + 0.5f);
                        ia = (uint32_t)(res_a * 255.0f + 0.5f);
                    }
                    fb_row[x] = (ia << 24) | (ir << 16) | (ig << 8) | ib;
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Client Arrays Draw Dispatches
 * ------------------------------------------------------------------------- */

static void fetch_vertex(const gl_context_t *ctx, int idx, gl_vertex_t *out) {
    /* Position */
    if (ctx->array_vertex.enabled && ctx->array_vertex.pointer) {
        int stride = ctx->array_vertex.stride ? ctx->array_vertex.stride : (ctx->array_vertex.size * (int)sizeof(float));
        const uint8_t *ptr = (const uint8_t *)ctx->array_vertex.pointer + (idx * stride);
        const float *fp = (const float *)ptr;
        out->x = fp[0];
        out->y = (ctx->array_vertex.size > 1) ? fp[1] : 0.0f;
        out->z = (ctx->array_vertex.size > 2) ? fp[2] : 0.0f;
        out->w = (ctx->array_vertex.size > 3) ? fp[3] : 1.0f;
    } else {
        out->x = 0.0f; out->y = 0.0f; out->z = 0.0f; out->w = 1.0f;
    }

    /* Color */
    if (ctx->array_color.enabled && ctx->array_color.pointer) {
        int stride = ctx->array_color.stride ? ctx->array_color.stride :
                     (ctx->array_color.type == GL_UNSIGNED_BYTE ? ctx->array_color.size : (ctx->array_color.size * (int)sizeof(float)));
        const uint8_t *ptr = (const uint8_t *)ctx->array_color.pointer + (idx * stride);
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
    if (ctx->array_texcoord.enabled && ctx->array_texcoord.pointer) {
        int stride = ctx->array_texcoord.stride ? ctx->array_texcoord.stride : (ctx->array_texcoord.size * (int)sizeof(float));
        const uint8_t *ptr = (const uint8_t *)ctx->array_texcoord.pointer + (idx * stride);
        const float *fp = (const float *)ptr;
        out->u = fp[0];
        out->v = (ctx->array_texcoord.size > 1) ? fp[1] : 0.0f;
    } else {
        out->u = ctx->cur_texcoord[0];
        out->v = ctx->cur_texcoord[1];
    }

    /* Normal */
    if (ctx->array_normal.enabled && ctx->array_normal.pointer) {
        int stride = ctx->array_normal.stride ? ctx->array_normal.stride : (3 * (int)sizeof(float));
        const uint8_t *ptr = (const uint8_t *)ctx->array_normal.pointer + (idx * stride);
        const float *fp = (const float *)ptr;
        out->nx = fp[0]; out->ny = fp[1]; out->nz = fp[2];
    } else {
        out->nx = ctx->cur_normal[0];
        out->ny = ctx->cur_normal[1];
        out->nz = ctx->cur_normal[2];
    }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || count <= 0) return;

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
            fetch_vertex(ctx, first, &v0);
            for (GLint i = 1; i + 1 < count; i++) {
                fetch_vertex(ctx, first + i, &v1);
                fetch_vertex(ctx, first + i + 1, &v2);
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v2);
            }
            break;
        default: break;
    }
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
    gl_context_t *ctx = gl_get_ctx();
    if (!ctx || count <= 0 || !indices) return;

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
            fetch_vertex(ctx, GET_INDEX(0), &v0);
            for (GLsizei i = 1; i + 1 < count; i++) {
                fetch_vertex(ctx, GET_INDEX(i), &v1);
                fetch_vertex(ctx, GET_INDEX(i + 1), &v2);
                gl_draw_primitive_triangle(ctx, &v0, &v1, &v2);
            }
            break;
        default: break;
    }
    #undef GET_INDEX
}
