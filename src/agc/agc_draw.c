#include "oops/agc.h"
#include "oops/gpu.h"
#include "oops/memory.h"
#include "oops/target.h"
#include "agc_internal.h"

#if OOPS_TARGET_IS_PROSPERO

__attribute__((weak)) int sceKernelUsleep(unsigned int microseconds);
__attribute__((weak)) int sceAgcDriverSubmitCommandBuffer(void *queue, const void *dcb);

static inline uint32_t float_as_u32(float f) {
  union { float f; uint32_t u; } val;
  val.f = f;
  return val.u;
}

int oops_agc_draw_primitive(oops_gpu_queue_t *queue, const oops_agc_draw_desc_t *desc) {
  if (!queue || !desc || !desc->color_buffer || desc->width == 0 || desc->height == 0) {
    return -1;
  }
  if (!queue->agc_queue_handle || !queue->dcb_mem || !queue->fence) {
    return -1;
  }

  uint32_t *dw = (uint32_t *)queue->dcb_mem;
  uint64_t color_gpu = (uint64_t)(uintptr_t)desc->color_buffer;
  uint64_t fence_gpu = (uint64_t)(uintptr_t)queue->fence;
  uint64_t ngg_va = desc->ngg_shader_va;
  uint64_t ps_va = desc->ps_shader_va;
  uint32_t prim_type = desc->primitive_type ? desc->primitive_type : OOPS_AGC_PRIM_TRILIST;
  uint32_t vcount = desc->vertex_count ? desc->vertex_count : 3;

  /* Reset fence */
  *queue->fence = 0x11111111u;

  /* 1. Context register setup for Color Target, Rasterizer, Viewport, Scissor */
  static const struct {
    uint32_t reg;
    uint32_t val;
  } base_ctx_regs[] = {
    {0x318u, 0}, /* CB_COLOR0_BASE (GFX10, patched below) */
    {0x390u, 0}, /* CB_COLOR0_BASE_EXT (GFX10, patched below) */
    {0x31bu, 0x00000000u}, /* CB_COLOR0_VIEW */
    {0x31cu, 0x000180a8u}, /* CB_COLOR0_INFO: COLOR_8_8_8_8, LINEAR_GENERAL, UNORM */
    {0x31du, 0x00000000u}, /* CB_COLOR0_ATTRIB: 0 */
    {0x31eu, 0x00000000u}, /* CB_COLOR0_DCC_CONTROL: disabled */
    {0x3b0u, 0},           /* CB_COLOR0_ATTRIB2 (patched below with AGC_CB_COLOR_ATTRIB2) */
    {0x3b8u, 0x08c6c000u}, /* CB_COLOR0_ATTRIB3: 2D linear buffer */
    {0x109u, 0x00000000u}, /* CB_DCC_CONTROL: disabled */
    {0x202u, 0x00cc0010u}, /* CB_COLOR_CONTROL: CB_NORMAL, ROP3_COPY */
    {0x08eu, 0x0000000fu}, /* CB_TARGET_MASK: MRT0 4 components enabled */
    {0x08fu, 0x0000000fu}, /* CB_SHADER_MASK: MRT0 4 components export enabled */
    {0x1e0u, 0x20010001u}, /* CB_BLEND0_CONTROL: SRC=ONE, DST=ZERO, ADD */
    {0x200u, 0x00000000u}, /* DB_DEPTH_CONTROL: disabled */
    {0x201u, 0x00010000u}, /* DB_EQAA */
    {0x203u, 0x00000010u}, /* DB_SHADER_CONTROL: EARLY_Z_THEN_LATE_Z */
    {0x08cu, 0xaa99aaaau}, /* PA_SC_EDGERULE: standard edge rule */
    {0x1d4u, 0x000000ffu}, /* SX_PS_DOWNCONVERT_CONTROL */
    {0x291u, 0x10020040u}, /* VGT_GS_ONCHIP_CNTL: ES_VERTS=64, GS_PRIMS=64, GS_INST_PRIMS=64 */
    {0x29bu, 0x00000000u}, /* VGT_GS_OUT_PRIM_TYPE: POINTLIST/PASSTHRU */
    {0x2d3u, 0x00000001u}, /* GE_NGG_SUBGRP_CNTL: PRIM_AMP=1, THDS_PER_SUBGRP=0 */
    {0x2d5u, 0x02002000u}, /* VGT_SHADER_STAGES_EN: PRIMGEN_EN | PRIMGEN_PASSTHRU_EN */
    {0x1ffu, 0x00000040u}, /* GE_MAX_OUTPUT_PER_SUBGROUP: MAX_VERTS=64 */
    {0x20eu, 0x00000078u}, /* PA_CL_NGG_CNTL: VERTEX_REUSE_DEPTH=30 */
    {0x2a1u, 0x00000000u}, /* VGT_PRIMITIVEID_EN: disabled */
    {0x2a6u, 0x00000040u}, /* VGT_DRAW_PAYLOAD_CNTL */
    {0x2adu, 0x00000000u}, /* VGT_REUSE_OFF */
    {0x2abu, 0x00000004u}, /* VGT_ESGS_RING_ITEMSIZE: 4 */
    {0x2ceu, 0x00000000u}, /* VGT_GS_MAX_VERT_OUT: 0 */
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
    {0x280u, 0x00080008u}, /* PA_SU_POINT_SIZE */
    {0x281u, 0xffff0000u}, /* PA_SU_POINT_MINMAX */
    {0x282u, 0x00000008u}, /* PA_SU_LINE_CNTL */
    {0x2deu, 0x000001e9u}, /* PA_SU_POLY_OFFSET_DB_FMT_CNTL */
    {0x00cu, 0x00000000u}, /* PA_SC_SCREEN_SCISSOR_TL */
    {0x00du, 0x40004000u}, /* PA_SC_SCREEN_SCISSOR_BR */
    {0x081u, 0x80000000u}, /* PA_SC_WINDOW_SCISSOR_TL */
    {0x082u, 0x40004000u}, /* PA_SC_WINDOW_SCISSOR_BR */
    {0x090u, 0x80000000u}, /* PA_SC_GENERIC_SCISSOR_TL */
    {0x091u, 0x40004000u}, /* PA_SC_GENERIC_SCISSOR_BR */
    {0x094u, 0x80000000u}, /* PA_SC_VPORT_SCISSOR_0_TL */
    {0x095u, 0x40004000u}, /* PA_SC_VPORT_SCISSOR_0_BR */
    {0x0b4u, 0x00000000u}, /* PA_SC_VPORT_ZMIN_0: 0.0f */
    {0x0b5u, 0x3f800000u}, /* PA_SC_VPORT_ZMAX_0: 1.0f */
    {0x10fu, 0},           /* PA_CL_VPORT_XSCALE (patched below) */
    {0x110u, 0},           /* PA_CL_VPORT_XOFFSET (patched below) */
    {0x111u, 0},           /* PA_CL_VPORT_YSCALE (patched below) */
    {0x112u, 0},           /* PA_CL_VPORT_YOFFSET (patched below) */
    {0x113u, 0x3f000000u}, /* PA_CL_VPORT_ZSCALE: 0.5f */
    {0x114u, 0x3f000000u}, /* PA_CL_VPORT_ZOFFSET: 0.5f */
    {0x083u, 0x0000ffffu}, /* PA_SC_CLIPRECT_RULE */
    {0x084u, 0x00000000u}, /* PA_SC_CLIPRECT_0_TL */
    {0x085u, 0x20002000u}, /* PA_SC_CLIPRECT_0_BR (8192x8192) */
    {0x204u, 0x00000000u}, /* PA_CL_CLIP_CNTL */
    {0x206u, 0x0000043fu}, /* PA_CL_VTE_CNTL */
    {0x207u, 0x00000000u}, /* PA_CL_VS_OUT_CNTL */
    {0x2fau, 0x3f800000u}, /* PA_CL_GB_VERT_CLIP_ADJ: 1.0f */
    {0x2fbu, 0x3f800000u}, /* PA_CL_GB_VERT_DISC_ADJ: 1.0f */
    {0x2fcu, 0x3f800000u}, /* PA_CL_GB_HORZ_CLIP_ADJ: 1.0f */
    {0x2fdu, 0x3f800000u}, /* PA_CL_GB_HORZ_DISC_ADJ: 1.0f */
    {0x205u, 0x00000240u}, /* PA_SU_SC_MODE_CNTL: no cull, trilist */
    {0x20cu, 0x00000000u}, /* PA_SU_SMALL_PRIM_FILTER_CNTL */
    {0x292u, 0x00000002u}, /* PA_SC_MODE_CNTL_0 */
    {0x293u, 0x06020000u}, /* PA_SC_MODE_CNTL_1 */
    {0x2f8u, 0x00000000u}, /* PA_SC_AA_CONFIG */
    {0x2f9u, 0x0000002du}, /* PA_SU_VTX_CNTL */
    {0x1b1u, 0x00000080u}, /* SPI_VS_OUT_CONFIG: NO_PC_EXPORT */
    {0x1c2u, 0x00000001u}, /* SPI_SHADER_IDX_FORMAT */
    {0x1c3u, 0x00000004u}, /* SPI_SHADER_POS_FORMAT */
    {0x1c5u, 0x00000009u}, /* SPI_SHADER_COL_FORMAT */
    {0x1b3u, 0x00000002u}, /* SPI_PS_INPUT_ENA: PERSP_CENTER_ENA */
    {0x1b4u, 0x00000002u}, /* SPI_PS_INPUT_ADDR: PERSP_CENTER_ENA */
    {0x1b5u, 0x00000001u}, /* SPI_INTERP_CONTROL_0: FLAT_SHADE_ENA */
    {0x1b6u, 0x00000000u}, /* SPI_PS_IN_CONTROL */
    {0x1b8u, 0x01000000u}, /* SPI_BARYC_CNTL: FRONT_FACE_ALL_BITS */
  };

  float half_w = (float)desc->width * 0.5f;
  float half_h = (float)desc->height * 0.5f;

  for (size_t i = 0; i < sizeof(base_ctx_regs) / sizeof(base_ctx_regs[0]); i++) {
    uint32_t reg = base_ctx_regs[i].reg;
    uint32_t val = base_ctx_regs[i].val;
    if (reg == 0x318u) {
      val = (uint32_t)(color_gpu >> 8);
    } else if (reg == 0x390u) {
      val = (uint32_t)(color_gpu >> 40);
    } else if (reg == 0x3b0u) {
      val = OOPS_AGC_CB_COLOR_ATTRIB2(desc->width, desc->height);
    } else if (reg == 0x200u) {
      val = (desc->depth_buffer && desc->depth_control) ? desc->depth_control : 0u;
    } else if (reg == 0x203u) {
      val = 0x00000000u; /* DB_SHADER_CONTROL: LATE_Z default matching AgcCompositor.elf */
    } else if (reg == 0x10fu || reg == 0x110u) {
      val = float_as_u32(half_w);
    } else if (reg == 0x111u || reg == 0x112u) {
      val = float_as_u32(half_h);
    }
    *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG, count 1 */
    *dw++ = reg;
    *dw++ = val;
  }

  /* Optional Depth Buffer and Depth Test setup (aligned with AgcCompositor.elf) */
  if (desc->depth_buffer && desc->depth_control) {
    uint64_t depth_gpu = (uint64_t)(uintptr_t)desc->depth_buffer;
    uint32_t z_format = desc->depth_format ? (desc->depth_format & 0x3u) : OOPS_AGC_Z_32_FLOAT;
    uint32_t z_info = z_format | 0x80000180u; /* SW_MODE=24 (0x180), ZRANGE_PRECISION (0x80000000) */

    static const struct {
      uint32_t reg;
      uint32_t val;
    } db_regs[] = {
      {OOPS_AGC_REG_DB_RENDER_CONTROL,        0x00000000u},
      {OOPS_AGC_REG_DB_COUNT_CONTROL,         0x11000100u},
      {OOPS_AGC_REG_DB_DEPTH_VIEW,            0x00000000u},
      {OOPS_AGC_REG_DB_RENDER_OVERRIDE,       0x00000000u},
      {OOPS_AGC_REG_DB_RENDER_OVERRIDE2,      0x00000000u},
      {OOPS_AGC_REG_DB_HTILE_DATA_BASE,       0x00000000u},
      {OOPS_AGC_REG_DB_DEPTH_SIZE_XY,         0},
      {OOPS_AGC_REG_DB_DEPTH_BOUNDS_MIN,      0x00000000u},
      {OOPS_AGC_REG_DB_DEPTH_BOUNDS_MAX,      0x00000000u},
      {OOPS_AGC_REG_DB_STENCIL_CLEAR,         0x00000000u},
      {OOPS_AGC_REG_DB_DEPTH_CLEAR,           0x00000000u},
      {OOPS_AGC_REG_DB_Z_INFO,                0},
      {OOPS_AGC_REG_DB_STENCIL_INFO,          0x20000180u},
      {OOPS_AGC_REG_DB_Z_READ_BASE,           0},
      {OOPS_AGC_REG_DB_STENCIL_READ_BASE,     0x00000000u},
      {OOPS_AGC_REG_DB_Z_WRITE_BASE,          0},
      {OOPS_AGC_REG_DB_STENCIL_WRITE_BASE,    0x00000000u},
      {OOPS_AGC_REG_DB_Z_READ_BASE_HI,        0},
      {OOPS_AGC_REG_DB_STENCIL_READ_BASE_HI,  0x00000000u},
      {OOPS_AGC_REG_DB_Z_WRITE_BASE_HI,       0},
      {OOPS_AGC_REG_DB_STENCIL_WRITE_BASE_HI, 0x00000000u},
      {OOPS_AGC_REG_DB_HTILE_DATA_BASE_HI,    0x00000000u},
      {OOPS_AGC_REG_DB_RMI_L2_CACHE_CONTROL,  0x00000000u},
      {OOPS_AGC_REG_DB_HTILE_SURFACE,         0x00040000u},
    };
    for (size_t i = 0; i < sizeof(db_regs) / sizeof(db_regs[0]); i++) {
      uint32_t reg = db_regs[i].reg;
      uint32_t val = db_regs[i].val;
      if (reg == OOPS_AGC_REG_DB_DEPTH_SIZE_XY) {
        val = OOPS_AGC_DB_DEPTH_SIZE_XY(desc->width, desc->height);
      } else if (reg == OOPS_AGC_REG_DB_Z_INFO) {
        val = z_info;
      } else if (reg == OOPS_AGC_REG_DB_Z_READ_BASE || reg == OOPS_AGC_REG_DB_Z_WRITE_BASE) {
        val = (uint32_t)(depth_gpu >> 8);
      } else if (reg == OOPS_AGC_REG_DB_Z_READ_BASE_HI || reg == OOPS_AGC_REG_DB_Z_WRITE_BASE_HI) {
        val = (uint32_t)(depth_gpu >> 40);
      }
      *dw++ = 0xc0016900u; /* PACKET3_SET_CONTEXT_REG, count 1 */
      *dw++ = reg;
      *dw++ = val;
    }
  }

  /* Clear all 32 SPI_PS_INPUT_CNTL registers to 0 */
  for (uint32_t i = 0; i < 32; i++) {
    *dw++ = 0xc0016900u;
    *dw++ = 0x191u + i;
    *dw++ = 0u;
  }

  /* 2. Shader program binding */
  struct {
    uint32_t base_reg;
    uint64_t va;
    uint32_t rsrc1;
    uint32_t rsrc2;
  } stages[] = {
    {0x08u, ps_va, 0x000c0010u, 0x00000000u}, /* PS */
    {0x48u, ngg_va, 0x000c0010u, 0x00000000u}, /* VS */
    {0x88u, ngg_va, 0x622c0042u, 0x00030000u}, /* GS/NGG */
    {0xc8u, ngg_va, 0x000c0010u, 0x00000000u}, /* ES */
    {0x108u, ngg_va, 0x000c0010u, 0x00000000u}, /* HS */
    {0x148u, ngg_va, 0x000c0010u, 0x00000000u}, /* LS */
  };
  for (size_t s = 0; s < sizeof(stages) / sizeof(stages[0]); s++) {
    uint32_t base_reg = stages[s].base_reg;
    uint64_t s_va = stages[s].va;
    *dw++ = 0xc0017600u; /* SET_SH_REG */
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

  /* 3. SPI Compute Unit Enable masks */
  static const struct {
    uint32_t reg;
    uint32_t val;
  } spi_cu_regs[] = {
    {0x007u, 0x0000ffffu}, /* PS CUs 0-15 */
    {0x001u, 0x00000003u}, /* PS CUs 16-17 */
    {0x087u, 0x0000fffdu}, /* GS CUs (CU1 disabled to prevent deadlock) */
    {0x081u, 0x00000003u}, /* GS CUs 16-17 */
    {0x107u, 0xffff0000u}, /* HS CUs */
  };
  for (size_t i = 0; i < sizeof(spi_cu_regs) / sizeof(spi_cu_regs[0]); i++) {
    *dw++ = 0xc0017600u;
    *dw++ = spi_cu_regs[i].reg;
    *dw++ = spi_cu_regs[i].val;
  }

  /* 4. Primitive topology and GE Parameter Cache setup via SET_UCONFIG_REG */
  *dw++ = 0xc0002f00u; /* PACKET3_NUM_INSTANCES */
  *dw++ = 1u;
  *dw++ = 0xc0017900u; /* mmVGT_PRIMITIVE_TYPE */
  *dw++ = 0x242u;
  *dw++ = prim_type;
  *dw++ = 0xc0017900u; /* mmGE_CNTL */
  *dw++ = 0x25bu;
  *dw++ = OOPS_AGC_GE_CNTL_DEFAULT;
  *dw++ = 0xc0017900u; /* mmGE_PC_ALLOC */
  *dw++ = 0x260u;
  *dw++ = OOPS_AGC_GE_PC_ALLOC_DEFAULT;

  /* 5. Draw: DRAW_INDEX_AUTO */
  *dw++ = 0xc0012d00u;
  *dw++ = vcount;
  *dw++ = 2u; /* DI_SRC_SEL_AUTO_INDEX */

  /* 6. Flush & Fence: RELEASE_MEM */
  *dw++ = 0xc0064900u;
  *dw++ = 0x06603514u;
  *dw++ = 0x20000000u;
  *dw++ = (uint32_t)fence_gpu;
  *dw++ = (uint32_t)(fence_gpu >> 32);
  *dw++ = 0xbeefcafeu;
  *dw++ = 0u;
  *dw++ = 0u;

  /* Trailing PM4 NOPs */
  for (int p = 0; p < 16; p++) {
    dw[p] = 0xffff1000u;
  }
  dw += 16;

  uint32_t words = (uint32_t)(dw - (uint32_t *)queue->dcb_mem);

  oops_agc_dcb_desc desc_cmd;
  desc_cmd.gpu_addr = (uint64_t)(uintptr_t)queue->dcb_mem;
  desc_cmd.size = words;
  desc_cmd.flags = 0u;
  desc_cmd.pad = 0u;

#if defined(__x86_64__)
  __builtin_ia32_clflush((const void *)queue->fence);
  for (size_t p = 0; p < (size_t)words * sizeof(uint32_t); p += 64) {
    __builtin_ia32_clflush((const void *)((const char *)queue->dcb_mem + p));
  }
#endif

  int submit_rc = -1;
  if (sceAgcDriverSubmitCommandBuffer) {
    submit_rc = sceAgcDriverSubmitCommandBuffer(queue->agc_queue_handle, &desc_cmd);
  } else if (sceAgcDriverSubmitDcb) {
    submit_rc = sceAgcDriverSubmitDcb(&desc_cmd);
  }

  if (submit_rc != 0) {
    return -1;
  }

  /* Poll fence */
  for (int iter = 0; iter < 10000; iter++) {
#if defined(__x86_64__)
    __builtin_ia32_clflush((const void *)queue->fence);
#endif
    if (*queue->fence == 0xbeefcafeu) {
      queue->last_fence = 0xbeefcafeu;
      return 0;
    }
    if (sceKernelUsleep) {
      sceKernelUsleep(10);
    }
  }

  return -2; /* fence timeout */
}

#else

/* Host / Non-PS5 Stubs */
int oops_agc_draw_primitive(oops_gpu_queue_t *queue, const oops_agc_draw_desc_t *desc) {
  if (!queue || !desc) {
    return -1;
  }
  return -1; /* Hardware GPU queue unavailable on host */
}

#endif
