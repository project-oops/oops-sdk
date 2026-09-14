#include "oops/agc.h"
#include "oops/gpu.h"
#include "tests/test_common.h"

static void test_gpu_null_safety(void) {
  /* Safe destruction with NULL */
  oops_gpu_destroy_queue(NULL);
  oops_gpu_destroy_shader(NULL);

  /* Safe dispatch with NULL */
  int rc = oops_gpu_dispatch(NULL, NULL);
  ASSERT_EQ(rc, -1);

  oops_gpu_dispatch_t d;
  memset(&d, 0, sizeof(d));
  rc = oops_gpu_dispatch(NULL, &d);
  ASSERT_EQ(rc, -1);

  /* Safe shader creation with invalid params */
  oops_gpu_shader_t *sh = oops_gpu_create_shader(NULL, 0, NULL, 0);
  ASSERT_TRUE(sh == NULL);

  uint8_t dummy[16];
  sh = oops_gpu_create_shader(dummy, sizeof(dummy), dummy, sizeof(dummy));
  ASSERT_TRUE(sh == NULL);

  /* Last fence on NULL queue */
  uint32_t f = oops_gpu_get_last_fence(NULL);
  ASSERT_EQ(f, 0);
}

static void test_gpu_available_contract(void) {
  /* On host test runner, GPU direct hardware queue is not available */
  int avail = oops_gpu_available();
  ASSERT_EQ(avail, 0);

  oops_gpu_queue_t *cq = oops_gpu_create_compute_queue();
  ASSERT_TRUE(cq == NULL);

  oops_gpu_queue_t *gq = oops_gpu_create_graphics_queue();
  ASSERT_TRUE(gq == NULL);
}

static void test_gpu_dispatch_struct_contract(void) {
  oops_gpu_dispatch_t d;
  d.shader = NULL;
  d.user_data = NULL;
  d.user_data_count = 8;
  d.grid_x = 15;
  d.grid_y = 9;
  d.grid_z = 1;

  ASSERT_EQ(d.user_data_count, 8);
  ASSERT_EQ(d.grid_x, 15);
  ASSERT_EQ(d.grid_y, 9);
  ASSERT_EQ(d.grid_z, 1);

  int rc = oops_agc_dispatch_compute(NULL, NULL, 1, 1, 1, NULL, 0);
  ASSERT_EQ(rc, -1);
}

static void test_gpu_primitive_draw_contract(void) {
  /* Safe with NULL queue and descriptor */
  int rc = oops_agc_draw_primitive(NULL, NULL);
  ASSERT_EQ(rc, -1);

  oops_agc_draw_desc_t desc;
  memset(&desc, 0, sizeof(desc));
  rc = oops_agc_draw_primitive(NULL, &desc);
  ASSERT_EQ(rc, -1);

  /* Verify depth descriptor fields */
  uint32_t dummy_depth[64];
  desc.depth_buffer = dummy_depth;
  desc.depth_control = OOPS_AGC_DB_DEPTH_CONTROL(1, 1, OOPS_AGC_ZFUNC_LESS);
  desc.depth_format = OOPS_AGC_Z_32_FLOAT;
  ASSERT_EQ(desc.depth_control, 0x16u);
  ASSERT_EQ(desc.depth_format, 3u);
  ASSERT_TRUE(desc.depth_buffer != NULL);
}

static void test_gpu_pm4_constants(void) {
  /* Verify PM4 Packet 3 Opcodes */
  ASSERT_EQ(OOPS_AGC_PM4_NOP, 0x10u);
  ASSERT_EQ(OOPS_AGC_PM4_DRAW_INDEX_AUTO, 0x2Du);
  ASSERT_EQ(OOPS_AGC_PM4_NUM_INSTANCES, 0x2Fu);
  ASSERT_EQ(OOPS_AGC_PM4_WAIT_REG_MEM, 0x3Cu);
  ASSERT_EQ(OOPS_AGC_PM4_RELEASE_MEM, 0x49u);
  ASSERT_EQ(OOPS_AGC_PM4_DMA_DATA, 0x50u);
  ASSERT_EQ(OOPS_AGC_PM4_SET_CONFIG_REG, 0x68u);
  ASSERT_EQ(OOPS_AGC_PM4_SET_CONTEXT_REG, 0x69u);
  ASSERT_EQ(OOPS_AGC_PM4_SET_SH_REG, 0x76u);
  ASSERT_EQ(OOPS_AGC_PM4_SET_UCONFIG_REG, 0x79u);

  /* Verify Color Buffer Context Registers */
  ASSERT_EQ(OOPS_AGC_REG_CB_TARGET_MASK, 0x08eu);
  ASSERT_EQ(OOPS_AGC_REG_CB_SHADER_MASK, 0x08fu);
  ASSERT_EQ(OOPS_AGC_REG_CB_COLOR0_BASE, 0x200u);
  ASSERT_EQ(OOPS_AGC_REG_CB_COLOR0_BASE_GFX10, 0x318u);
  ASSERT_EQ(OOPS_AGC_REG_CB_COLOR0_BASE_EXT, 0x390u);
  ASSERT_EQ(OOPS_AGC_REG_CB_COLOR0_VIEW, 0x31bu);
  ASSERT_EQ(OOPS_AGC_REG_CB_COLOR0_INFO, 0x31cu);
  ASSERT_EQ(OOPS_AGC_REG_CB_COLOR0_ATTRIB, 0x31du);
  ASSERT_EQ(OOPS_AGC_REG_CB_COLOR0_DCC_CONTROL, 0x31eu);
  ASSERT_EQ(OOPS_AGC_REG_CB_COLOR0_ATTRIB2, 0x3b0u);
  ASSERT_EQ(OOPS_AGC_REG_CB_COLOR0_ATTRIB3, 0x3b8u);
  ASSERT_EQ(OOPS_AGC_REG_CB_COLOR_CONTROL, 0x202u);
  ASSERT_EQ(OOPS_AGC_REG_CB_BLEND0_CONTROL, 0x1e0u);

  /* Verify ATTRIB2 Extent Macro (width-1 in bits 27:14, height-1 in bits 13:0: the row pitch) */
  ASSERT_EQ(AGC_CB_COLOR_ATTRIB2(64, 64), (63u << 14) | 63u);
  ASSERT_EQ(AGC_CB_COLOR_ATTRIB2(1920, 1080), (1919u << 14) | 1079u);
  ASSERT_EQ(OOPS_AGC_CB_COLOR_ATTRIB2(1280, 720), (1279u << 14) | 719u);

  /* Verify Depth Block Context Registers */
  ASSERT_EQ(OOPS_AGC_REG_DB_RENDER_CONTROL, 0x000u);
  ASSERT_EQ(OOPS_AGC_REG_DB_DEPTH_VIEW, 0x002u);
  ASSERT_EQ(OOPS_AGC_REG_DB_RENDER_OVERRIDE, 0x003u);
  ASSERT_EQ(OOPS_AGC_REG_DB_DEPTH_SIZE_XY, 0x007u);
  ASSERT_EQ(OOPS_AGC_REG_DB_DEPTH_CLEAR, 0x00bu);
  ASSERT_EQ(OOPS_AGC_REG_DB_DFSM_CONTROL, 0x00eu);
  ASSERT_EQ(OOPS_AGC_REG_DB_Z_INFO, 0x010u);
  ASSERT_EQ(OOPS_AGC_REG_DB_Z_READ_BASE, 0x012u);
  ASSERT_EQ(OOPS_AGC_REG_DB_Z_WRITE_BASE, 0x014u);
  ASSERT_EQ(OOPS_AGC_REG_DB_Z_READ_BASE_HI, 0x01au);
  ASSERT_EQ(OOPS_AGC_REG_DB_Z_WRITE_BASE_HI, 0x01cu);
  ASSERT_EQ(OOPS_AGC_REG_DB_DEPTH_CONTROL, 0x200u);
  ASSERT_EQ(OOPS_AGC_REG_DB_EQAA, 0x201u);
  ASSERT_EQ(OOPS_AGC_REG_DB_SHADER_CONTROL, 0x203u);

  /* Verify Depth Extent Macro (height-1 in bits 29:16, width-1 in bits 13:0) */
  ASSERT_EQ(OOPS_AGC_DB_DEPTH_SIZE_XY(64, 64), (63u << 16) | 63u);
  ASSERT_EQ(AGC_DB_DEPTH_SIZE_XY(1920, 1080), (1079u << 16) | 1919u);
  ASSERT_EQ(OOPS_AGC_DB_DEPTH_SIZE_XY(1280, 720), (719u << 16) | 1279u);

  /* Verify Depth Compare Functions & Formats */
  ASSERT_EQ(OOPS_AGC_ZFUNC_NEVER, 0x0u);
  ASSERT_EQ(OOPS_AGC_ZFUNC_LESS, 0x1u);
  ASSERT_EQ(OOPS_AGC_ZFUNC_EQUAL, 0x2u);
  ASSERT_EQ(OOPS_AGC_ZFUNC_LEQUAL, 0x3u);
  ASSERT_EQ(OOPS_AGC_ZFUNC_GREATER, 0x4u);
  ASSERT_EQ(OOPS_AGC_ZFUNC_NOTEQUAL, 0x5u);
  ASSERT_EQ(OOPS_AGC_ZFUNC_GEQUAL, 0x6u);
  ASSERT_EQ(OOPS_AGC_ZFUNC_ALWAYS, 0x7u);
  ASSERT_EQ(OOPS_AGC_Z_32_FLOAT, 0x3u);

  /* Verify DB_DEPTH_CONTROL helper */
  ASSERT_EQ(OOPS_AGC_DB_DEPTH_CONTROL(1, 1, OOPS_AGC_ZFUNC_LESS), 0x16u);
  ASSERT_EQ(OOPS_AGC_DB_DEPTH_CONTROL(1, 0, OOPS_AGC_ZFUNC_LEQUAL), 0x32u);
  ASSERT_EQ(OOPS_AGC_DB_DEPTH_CONTROL(0, 0, OOPS_AGC_ZFUNC_ALWAYS), 0x70u);

  /* Verify SPI PS Input & Interpolant Registers */
  ASSERT_EQ(OOPS_AGC_REG_SPI_PS_INPUT_CNTL_0, 0x191u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_PS_INPUT_CNTL_31, 0x1b0u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_PS_INPUT_CNTL(0), 0x191u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_PS_INPUT_CNTL(31), 0x1b0u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_PS_INPUT_ENA, 0x1b3u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_PS_INPUT_ADDR, 0x1b4u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_INTERP_CONTROL_0, 0x1b5u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_PS_IN_CONTROL, 0x1b6u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_BARYC_CNTL, 0x1b8u);

  /* Verify UCONFIG Registers & Defaults */
  ASSERT_EQ(OOPS_AGC_REG_VGT_PRIMITIVE_TYPE, 0x242u);
  ASSERT_EQ(OOPS_AGC_REG_GE_CNTL, 0x25bu);
  ASSERT_EQ(OOPS_AGC_REG_GE_PC_ALLOC, 0x260u);
  ASSERT_EQ(OOPS_AGC_GE_CNTL_DEFAULT, 0x00008040u);
  ASSERT_EQ(OOPS_AGC_GE_PC_ALLOC_DEFAULT, 0x000003ffu);

  /* Verify RDNA2 SH Registers */
  ASSERT_EQ(OOPS_AGC_REG_SPI_SHADER_PGM_LO_PS, 0x008u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_SHADER_PGM_LO_ES, 0x088u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_SHADER_PGM_LO_VS, 0x0c8u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_SHADER_PGM_LO_HS, 0x108u);
  ASSERT_EQ(OOPS_AGC_REG_SPI_SHADER_PGM_LO_GS, 0x148u);
  ASSERT_EQ(OOPS_AGC_REG_COMPUTE_PGM_LO, 0x20cu);
  ASSERT_EQ(OOPS_AGC_REG_COMPUTE_PGM_HI, 0x20du);

  /* Verify Primitive Topologies */
  ASSERT_EQ(OOPS_AGC_PRIM_POINTLIST, 0x1u);
  ASSERT_EQ(OOPS_AGC_PRIM_LINELIST, 0x2u);
  ASSERT_EQ(OOPS_AGC_PRIM_LINESTRIP, 0x3u);
  ASSERT_EQ(OOPS_AGC_PRIM_TRILIST, 0x4u);
  ASSERT_EQ(OOPS_AGC_PRIM_TRISTRIP, 0x5u);

  /* Verify All 8 Hardware Shader Stages */
  ASSERT_EQ(OOPS_AGC_STAGE_COMPUTE, 0u);
  ASSERT_EQ(OOPS_AGC_STAGE_PIXEL, 1u);
  ASSERT_EQ(OOPS_AGC_STAGE_VERTEX, 2u);
  ASSERT_EQ(OOPS_AGC_STAGE_GEOMETRY, 3u);
  ASSERT_EQ(OOPS_AGC_STAGE_LOCAL, 4u);
  ASSERT_EQ(OOPS_AGC_STAGE_HULL_HALF, 5u);
  ASSERT_EQ(OOPS_AGC_STAGE_EXPORT, 6u);
  ASSERT_EQ(OOPS_AGC_STAGE_HULL, 7u);
}

void run_unit_tests_gpu(void);

void run_unit_tests_gpu(void) {
  TEST_SUITE_BEGIN("GPU Compute Subsystem (libSceAgc / libSceAgcDriver)");
  RUN_TEST(test_gpu_null_safety);
  RUN_TEST(test_gpu_available_contract);
  RUN_TEST(test_gpu_dispatch_struct_contract);
  RUN_TEST(test_gpu_primitive_draw_contract);
  RUN_TEST(test_gpu_pm4_constants);
}
