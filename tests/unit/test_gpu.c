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

  oops_gpu_queue_t *q = oops_gpu_create_compute_queue();
  ASSERT_TRUE(q == NULL);
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

static void test_gpu_pm4_constants(void) {
  /* Verify PM4 Packet 3 Opcodes */
  ASSERT_EQ(OOPS_AGC_PM4_NOP, 0x10u);
  ASSERT_EQ(OOPS_AGC_PM4_DRAW_INDEX_AUTO, 0x2Du);
  ASSERT_EQ(OOPS_AGC_PM4_WAIT_REG_MEM, 0x3Cu);
  ASSERT_EQ(OOPS_AGC_PM4_RELEASE_MEM, 0x49u);
  ASSERT_EQ(OOPS_AGC_PM4_DMA_DATA, 0x50u);
  ASSERT_EQ(OOPS_AGC_PM4_SET_CONFIG_REG, 0x68u);
  ASSERT_EQ(OOPS_AGC_PM4_SET_CONTEXT_REG, 0x69u);
  ASSERT_EQ(OOPS_AGC_PM4_SET_SH_REG, 0x76u);
  ASSERT_EQ(OOPS_AGC_PM4_SET_UCONFIG_REG, 0x79u);

  /* Verify Register Offsets */
  ASSERT_EQ(OOPS_AGC_REG_CB_COLOR0_BASE, 0x200u);
  ASSERT_EQ(OOPS_AGC_REG_VGT_PRIMITIVE_TYPE, 0x242u);
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
  RUN_TEST(test_gpu_pm4_constants);
}
