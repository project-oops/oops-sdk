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
 * RDNA2 (GFX10.3) PM4 Packet 3 Opcodes.
 * Confirmed on retail Prospero FW 12.40 hardware sweeps (obSCEne 166-agc).
 */
#define OOPS_AGC_PM4_NOP                0x10u
#define OOPS_AGC_PM4_DRAW_INDEX_AUTO    0x2Du
#define OOPS_AGC_PM4_WAIT_REG_MEM       0x3Cu
#define OOPS_AGC_PM4_RELEASE_MEM        0x49u
#define OOPS_AGC_PM4_DMA_DATA           0x50u
#define OOPS_AGC_PM4_SET_CONFIG_REG     0x68u
#define OOPS_AGC_PM4_SET_CONTEXT_REG    0x69u
#define OOPS_AGC_PM4_SET_SH_REG         0x76u
#define OOPS_AGC_PM4_SET_UCONFIG_REG    0x79u

/*
 * Hardware Register Offsets across the 4 register spaces.
 */
#define OOPS_AGC_REG_CB_COLOR0_BASE           0x200u /* Context space */
#define OOPS_AGC_REG_VGT_PRIMITIVE_TYPE       0x242u /* UConfig space */

/* RDNA2 SH (Shader) Program Address and User Data Registers */
#define OOPS_AGC_REG_SPI_SHADER_PGM_LO_PS     0x008u /* Pixel Shader */
#define OOPS_AGC_REG_SPI_SHADER_PGM_LO_ES     0x088u /* Export Shader */
#define OOPS_AGC_REG_SPI_SHADER_PGM_LO_VS     0x0c8u /* Vertex Shader */
#define OOPS_AGC_REG_SPI_SHADER_PGM_LO_HS     0x108u /* Hull Shader */
#define OOPS_AGC_REG_SPI_SHADER_PGM_LO_GS     0x148u /* Geometry Shader */
#define OOPS_AGC_REG_COMPUTE_PGM_LO           0x20cu /* Compute Shader */
#define OOPS_AGC_REG_COMPUTE_PGM_HI           0x20du
#define OOPS_AGC_REG_COMPUTE_USER_DATA_0      0x240u
#define OOPS_AGC_REG_COMPUTE_USER_DATA_1      0x241u
#define OOPS_AGC_REG_COMPUTE_USER_DATA_2      0x242u
#define OOPS_AGC_REG_COMPUTE_USER_DATA_3      0x243u
#define OOPS_AGC_REG_COMPUTE_USER_DATA_4      0x244u

/*
 * Primitive Topologies (DI_PT_*).
 */
#define OOPS_AGC_PRIM_POINTLIST               0x1u
#define OOPS_AGC_PRIM_LINELIST                0x2u
#define OOPS_AGC_PRIM_LINESTRIP               0x3u
#define OOPS_AGC_PRIM_TRILIST                 0x4u
#define OOPS_AGC_PRIM_TRISTRIP                0x5u

/*
 * Measured Hardware Shader Stages (sceAgcCreateShader 0..7).
 * Ground truth from libSceAgc jump table and silicon sweeps.
 */
#define OOPS_AGC_STAGE_COMPUTE                0u /* CS */
#define OOPS_AGC_STAGE_PIXEL                  1u /* PS */
#define OOPS_AGC_STAGE_VERTEX                 2u /* VS */
#define OOPS_AGC_STAGE_GEOMETRY               3u /* GS */
#define OOPS_AGC_STAGE_LOCAL                  4u /* LS (unfused VS half) */
#define OOPS_AGC_STAGE_HULL_HALF              5u /* HS half (unfused Hull half) */
#define OOPS_AGC_STAGE_EXPORT                 6u /* ES (Export shader) */
#define OOPS_AGC_STAGE_HULL                   7u /* HS (Hull shader) */

/*
 * Retail libSceAgc Direct PM4 Emitters and Shader Functions.
 * Calling convention: register emitters take (dcb, ((uint64_t)val << 32) | reg_offset).
 */
__attribute__((weak)) void *sceAgcDcbSetCfRegisterDirect(void *dcb, uint64_t reg_val);
__attribute__((weak)) void *sceAgcDcbSetCxRegisterDirect(void *dcb, uint64_t reg_val);
__attribute__((weak)) void *sceAgcDcbSetShRegisterDirect(void *dcb, uint64_t reg_val);
__attribute__((weak)) void *sceAgcDcbSetUcRegisterDirect(void *dcb, uint64_t reg_val);
__attribute__((weak)) void *sceAgcDcbDrawIndexAuto(void *dcb, uint32_t count, uint32_t initiator);
__attribute__((weak)) void *sceAgcCbSetShRegisterRangeDirect(void *cb, uint32_t start_reg,
                                                             const uint32_t *values,
                                                             uint32_t count);

__attribute__((weak)) int sceAgcFuseShaderHalves(void *out_stage2, const void *stage4_ls,
                                                 const void *stage6_es);
__attribute__((weak)) int sceAgcCreateInterpolantMapping(void *out_interpolants, uint32_t topology);
__attribute__((weak)) int sceAgcUpdateInterpolantMapping(void *out_interpolants, uint32_t topology);
__attribute__((weak)) int sceAgcCreatePrimState(void *out_prim_state, uint32_t topology);
__attribute__((weak)) int sceAgcUpdatePrimState(void *out_prim_state, uint32_t topology);
__attribute__((weak)) int sceAgcLinkShaders(void *out_link_state, const void *vs_shader,
                                            const void *ps_shader, const void *interpolants);

/*
 * High-level AGC compute dispatch helper. Wraps oops_gpu_dispatch() with
 * explicit grid dimensions and user data registers.
 */
static inline int oops_agc_dispatch_compute(oops_gpu_queue_t *queue,
                                            const oops_gpu_shader_t *shader,
                                            uint32_t grid_x, uint32_t grid_y,
                                            uint32_t grid_z,
                                            const uint32_t *user_data,
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

#ifdef __cplusplus
}
#endif

#endif /* OOPS_AGC_H */
