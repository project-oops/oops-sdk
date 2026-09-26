/*
 * AGC (Prospero GPU) definitions: PM4 opcodes, register offsets and field packers, the
 * libSceAgc entry points the SDK binds weakly, and the compute and draw helpers built
 * on `oops/gpu.h`.
 */
#ifndef OOPS_AGC_H
#define OOPS_AGC_H

#include "agc/display.h"
#include "agc/driver.h"
#include "agc/tiler.h"
#include "oops/gpu.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RDNA2 (GFX10.3) PM4 type-3 opcodes, confirmed on retail Prospero FW 12.40 by the
 * obSCEne probe 166-agc.
 */
#define OOPS_AGC_PM4_NOP 0x10u
#define OOPS_AGC_PM4_DRAW_INDEX_AUTO 0x2Du
#define OOPS_AGC_PM4_NUM_INSTANCES 0x2fu
#define OOPS_AGC_PM4_WAIT_REG_MEM 0x3Cu
#define OOPS_AGC_PM4_EVENT_WRITE 0x46u
#define OOPS_AGC_PM4_RELEASE_MEM 0x49u
#define OOPS_AGC_PM4_DMA_DATA 0x50u
#define OOPS_AGC_PM4_SET_CONFIG_REG 0x68u
#define OOPS_AGC_PM4_SET_CONTEXT_REG 0x69u
#define OOPS_AGC_PM4_SET_SH_REG 0x76u
#define OOPS_AGC_PM4_SET_UCONFIG_REG 0x79u
#define OOPS_AGC_PM4_SET_UCONFIG_REG_INDEX                                             \
    0x7Au /* bits 31:28 of the offset word carry the index */

/* Register offsets, relative to their register space. */
#define OOPS_AGC_REG_CB_TARGET_MASK 0x08eu /* Context space */
#define OOPS_AGC_REG_CB_SHADER_MASK 0x08fu
#define OOPS_AGC_REG_CB_COLOR0_BASE 0x200u
#define OOPS_AGC_REG_CB_COLOR0_BASE_GFX10 0x318u
#define OOPS_AGC_REG_CB_COLOR0_BASE_EXT 0x390u
#define OOPS_AGC_REG_CB_COLOR0_VIEW 0x31bu
#define OOPS_AGC_REG_CB_COLOR0_INFO 0x31cu
#define OOPS_AGC_REG_CB_COLOR0_ATTRIB 0x31du
#define OOPS_AGC_REG_CB_COLOR0_DCC_CONTROL 0x31eu
#define OOPS_AGC_REG_CB_COLOR0_ATTRIB2 0x3b0u
#define OOPS_AGC_REG_CB_COLOR0_ATTRIB3 0x3b8u
#define OOPS_AGC_REG_CB_COLOR_CONTROL 0x202u
#define OOPS_AGC_REG_CB_BLEND0_CONTROL 0x1e0u

/*
 * CB_COLOR0_ATTRIB2 surface extent: bits 27:14 = width - 1, bits 13:0 = height - 1.
 * Measured on Prospero: without the register the CB treats the surface as 0x0 and
 * drops every pixel write, and the colour block takes the row pitch from bits 27:14.
 */
#define OOPS_AGC_CB_COLOR_ATTRIB2(w, h)                                                \
    ((((uint32_t)(w) - 1u) << 14) | ((uint32_t)(h) - 1u))
#define AGC_CB_COLOR_ATTRIB2(w, h) OOPS_AGC_CB_COLOR_ATTRIB2(w, h)

/*
 * Depth Block (DB) Context Registers (base 0xa000).
 */
#define OOPS_AGC_REG_DB_RENDER_CONTROL 0x000u
#define OOPS_AGC_REG_DB_COUNT_CONTROL 0x001u
#define OOPS_AGC_REG_DB_DEPTH_VIEW 0x002u
#define OOPS_AGC_REG_DB_RENDER_OVERRIDE 0x003u
#define OOPS_AGC_REG_DB_RENDER_OVERRIDE2 0x004u
#define OOPS_AGC_REG_DB_HTILE_DATA_BASE 0x005u
#define OOPS_AGC_REG_DB_DEPTH_SIZE_XY 0x007u
#define OOPS_AGC_REG_DB_DEPTH_BOUNDS_MIN 0x008u
#define OOPS_AGC_REG_DB_DEPTH_BOUNDS_MAX 0x009u
#define OOPS_AGC_REG_DB_STENCIL_CLEAR 0x00au
#define OOPS_AGC_REG_DB_DEPTH_CLEAR 0x00bu
#define OOPS_AGC_REG_DB_DFSM_CONTROL 0x00eu
#define OOPS_AGC_REG_DB_Z_INFO 0x010u
#define OOPS_AGC_REG_DB_STENCIL_INFO 0x011u
#define OOPS_AGC_REG_DB_Z_READ_BASE 0x012u
#define OOPS_AGC_REG_DB_STENCIL_READ_BASE 0x013u
#define OOPS_AGC_REG_DB_Z_WRITE_BASE 0x014u
#define OOPS_AGC_REG_DB_STENCIL_WRITE_BASE 0x015u
#define OOPS_AGC_REG_DB_Z_READ_BASE_HI 0x01au
#define OOPS_AGC_REG_DB_STENCIL_READ_BASE_HI 0x01bu
#define OOPS_AGC_REG_DB_Z_WRITE_BASE_HI 0x01cu
#define OOPS_AGC_REG_DB_STENCIL_WRITE_BASE_HI 0x01du
#define OOPS_AGC_REG_DB_HTILE_DATA_BASE_HI 0x01eu
#define OOPS_AGC_REG_DB_RMI_L2_CACHE_CONTROL 0x01fu
#define OOPS_AGC_REG_DB_DEPTH_CONTROL 0x200u
#define OOPS_AGC_REG_DB_EQAA 0x201u
#define OOPS_AGC_REG_DB_SHADER_CONTROL 0x203u
#define OOPS_AGC_REG_DB_HTILE_SURFACE 0x2afu

/*
 * Depth Extent Packing Macro:
 * DB_DEPTH_SIZE_XY: bits [29:16] = (height - 1), bits [13:0] = (width - 1)
 */
#define OOPS_AGC_DB_DEPTH_SIZE_XY(w, h)                                                \
    (((((uint32_t)(h) - 1u) & 0x3fffu) << 16) | (((uint32_t)(w) - 1u) & 0x3fffu))
#define AGC_DB_DEPTH_SIZE_XY(w, h) OOPS_AGC_DB_DEPTH_SIZE_XY(w, h)

/*
 * Depth Compare Functions (CompareFrag / ZFUNC).
 */
#define OOPS_AGC_ZFUNC_NEVER 0x0u
#define OOPS_AGC_ZFUNC_LESS 0x1u
#define OOPS_AGC_ZFUNC_EQUAL 0x2u
#define OOPS_AGC_ZFUNC_LEQUAL 0x3u
#define OOPS_AGC_ZFUNC_GREATER 0x4u
#define OOPS_AGC_ZFUNC_NOTEQUAL 0x5u
#define OOPS_AGC_ZFUNC_GEQUAL 0x6u
#define OOPS_AGC_ZFUNC_ALWAYS 0x7u

/*
 * Depth Formats (ZFormat).
 */
#define OOPS_AGC_Z_INVALID 0x0u
#define OOPS_AGC_Z_16 0x1u
#define OOPS_AGC_Z_24 0x2u
#define OOPS_AGC_Z_32_FLOAT 0x3u

/*
 * DB_DEPTH_CONTROL bitmask helper:
 * Bit 1 = Z_ENABLE, Bit 2 = Z_WRITE_ENABLE, Bits [6:4] = ZFUNC.
 */
#define OOPS_AGC_DB_DEPTH_CONTROL(z_enable, z_write, zfunc)                            \
    (((z_enable) ? (1u << 1) : 0u) | ((z_write) ? (1u << 2) : 0u) |                    \
     (((uint32_t)(zfunc) & 0x7u) << 4))
#define AGC_DB_DEPTH_CONTROL(z_enable, z_write, zfunc)                                 \
    OOPS_AGC_DB_DEPTH_CONTROL(z_enable, z_write, zfunc)

/* PA SU / SC Face Culling and Polygon Control */
#define OOPS_AGC_REG_PA_SU_SC_MODE_CNTL 0x205u
#define OOPS_AGC_CULL_NONE 0x00000240u
#define OOPS_AGC_CULL_FRONT 0x00000241u
#define OOPS_AGC_CULL_BACK 0x00000242u
#define OOPS_AGC_CULL_FRONT_AND_BACK 0x00000243u
#define OOPS_AGC_FACE_CW (1u << 2)

/* SPI PS Input, Interpolant, and Barycentric Controls */
#define OOPS_AGC_REG_SPI_PS_INPUT_CNTL(i) (0x191u + (uint32_t)(i))
#define OOPS_AGC_REG_SPI_PS_INPUT_CNTL_0 0x191u
#define OOPS_AGC_REG_SPI_PS_INPUT_CNTL_31 0x1b0u
#define OOPS_AGC_REG_SPI_PS_INPUT_ENA 0x1b3u
#define OOPS_AGC_REG_SPI_PS_INPUT_ADDR 0x1b4u
#define OOPS_AGC_REG_SPI_INTERP_CONTROL_0 0x1b5u
#define OOPS_AGC_REG_SPI_PS_IN_CONTROL 0x1b6u
#define OOPS_AGC_REG_SPI_BARYC_CNTL 0x1b8u
#define OOPS_AGC_REG_SPI_SHADER_POS_FORMAT 0x1c3u
#define OOPS_AGC_REG_SPI_SHADER_COL_FORMAT 0x1c5u

/* UConfig Registers */
#define OOPS_AGC_REG_VGT_PRIMITIVE_TYPE 0x242u /* UConfig space */
#define OOPS_AGC_REG_GE_CNTL 0x25bu
#define OOPS_AGC_REG_GE_PC_ALLOC 0x260u

#define OOPS_AGC_GE_CNTL_DEFAULT 0x00008040u     /* 64-prim / 64-vert group size */
#define OOPS_AGC_GE_PC_ALLOC_DEFAULT 0x000003ffu /* Oversubscription + 511 lines */

/* RDNA2 SH (Shader) Program Address and User Data Registers */
#define OOPS_AGC_REG_SPI_SHADER_PGM_LO_PS 0x008u /* Pixel Shader */
#define OOPS_AGC_REG_SPI_SHADER_PGM_LO_ES 0x088u /* Export Shader */
#define OOPS_AGC_REG_SPI_SHADER_PGM_LO_VS 0x0c8u /* Vertex Shader */
#define OOPS_AGC_REG_SPI_SHADER_PGM_LO_HS 0x108u /* Hull Shader */
#define OOPS_AGC_REG_SPI_SHADER_PGM_LO_GS 0x148u /* Geometry Shader */
#define OOPS_AGC_REG_COMPUTE_PGM_LO 0x20cu       /* Compute Shader */
#define OOPS_AGC_REG_COMPUTE_PGM_HI 0x20du
#define OOPS_AGC_REG_COMPUTE_USER_DATA_0 0x240u
#define OOPS_AGC_REG_COMPUTE_USER_DATA_1 0x241u
#define OOPS_AGC_REG_COMPUTE_USER_DATA_2 0x242u
#define OOPS_AGC_REG_COMPUTE_USER_DATA_3 0x243u
#define OOPS_AGC_REG_COMPUTE_USER_DATA_4 0x244u

/*
 * Primitive Topologies (DI_PT_*).
 */
#define OOPS_AGC_PRIM_POINTLIST 0x1u
#define OOPS_AGC_PRIM_LINELIST 0x2u
#define OOPS_AGC_PRIM_LINESTRIP 0x3u
#define OOPS_AGC_PRIM_TRILIST 0x4u
#define OOPS_AGC_PRIM_TRISTRIP 0x5u

/*
 * Shader stages as sceAgcCreateShader numbers them (0..7), from libSceAgc's jump
 * table and measured on hardware.
 */
#define OOPS_AGC_STAGE_COMPUTE 0u   /* CS */
#define OOPS_AGC_STAGE_PIXEL 1u     /* PS */
#define OOPS_AGC_STAGE_VERTEX 2u    /* VS */
#define OOPS_AGC_STAGE_GEOMETRY 3u  /* GS */
#define OOPS_AGC_STAGE_LOCAL 4u     /* LS (unfused VS half) */
#define OOPS_AGC_STAGE_HULL_HALF 5u /* HS half (unfused Hull half) */
#define OOPS_AGC_STAGE_EXPORT 6u    /* ES (Export shader) */
#define OOPS_AGC_STAGE_HULL 7u      /* HS (Hull shader) */

/*
 * libSceAgc's direct PM4 emitters and shader functions. The register emitters take
 * (dcb, ((uint64_t)val << 32) | reg_offset).
 */
__attribute__((weak)) void *sceAgcDcbSetCfRegisterDirect(void *dcb, uint64_t reg_val);
__attribute__((weak)) void *sceAgcDcbSetCxRegisterDirect(void *dcb, uint64_t reg_val);
__attribute__((weak)) void *sceAgcDcbSetShRegisterDirect(void *dcb, uint64_t reg_val);
__attribute__((weak)) void *sceAgcDcbSetUcRegisterDirect(void *dcb, uint64_t reg_val);
__attribute__((weak)) void *sceAgcDcbDrawIndexAuto(void *dcb, uint32_t count,
                                                   uint32_t initiator);
__attribute__((weak)) void *sceAgcCbSetShRegisterRangeDirect(void *cb,
                                                             uint32_t start_reg,
                                                             const uint32_t *values,
                                                             uint32_t count);

__attribute__((weak)) int
sceAgcFuseShaderHalves(void *out_stage2, const void *stage4_ls, const void *stage6_es);
__attribute__((weak)) int sceAgcCreateInterpolantMapping(void *out_interpolants,
                                                         uint32_t topology);
__attribute__((weak)) int sceAgcUpdateInterpolantMapping(void *out_interpolants,
                                                         uint32_t topology);
__attribute__((weak)) int sceAgcCreatePrimState(void *out_prim_state,
                                                uint32_t topology);
__attribute__((weak)) int sceAgcUpdatePrimState(void *out_prim_state,
                                                uint32_t topology);
__attribute__((weak)) int sceAgcLinkShaders(void *out_link_state, const void *vs_shader,
                                            const void *ps_shader,
                                            const void *interpolants);

/* `oops_gpu_dispatch` with the grid and user data registers as arguments. */
static inline int oops_agc_dispatch_compute(oops_gpu_queue_t *queue,
                                            const oops_gpu_shader_t *shader,
                                            uint32_t grid_x, uint32_t grid_y,
                                            uint32_t grid_z, const uint32_t *user_data,
                                            uint32_t user_data_count) {
    oops_gpu_dispatch_t d;
    d.shader = shader;
    d.user_data = user_data;
    d.user_data_count = user_data_count;
    d.grid_x = grid_x;
    d.grid_y = grid_y;
    d.grid_z = grid_z;
    return oops_gpu_dispatch(queue, &d);
}

/* One draw for `oops_agc_draw_primitive`. */
typedef struct oops_agc_draw_desc {
    void *color_buffer;      /* Direct coherent / Onion memory buffer */
    uint32_t width;          /* Target width (e.g. 1920) */
    uint32_t height;         /* Target height (e.g. 1080) */
    uint64_t ngg_shader_va;  /* Virtual address of RDNA2 NGG Primitive Shader */
    uint64_t ps_shader_va;   /* Virtual address of RDNA2 Pixel Shader */
    uint32_t vertex_count;   /* Number of vertices to draw (e.g. 3 for triangle) */
    uint32_t primitive_type; /* Topology (e.g. OOPS_AGC_PRIM_TRILIST = 0x4) */
    void *depth_buffer;     /* Optional depth buffer (Direct coherent / Onion memory) */
    uint32_t depth_control; /* DB_DEPTH_CONTROL value (0 = depth disabled) */
    uint32_t depth_format;  /* ZFormat (e.g. OOPS_AGC_Z_32_FLOAT = 0x3) */
} oops_agc_draw_desc_t;

/*
 * Draws on a graphics queue (type 0): sets the fixed-function context, binds the
 * shaders, issues DRAW_INDEX_AUTO, flushes with a RELEASE_MEM end-of-pipe event and
 * waits for the fence. Returns 0 on success, -2 on a fence timeout, -1 on any other
 * error.
 */
int oops_agc_draw_primitive(oops_gpu_queue_t *queue, const oops_agc_draw_desc_t *desc);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AGC_H */
