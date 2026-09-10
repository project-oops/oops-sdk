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

void run_unit_tests_gpu(void);

void run_unit_tests_gpu(void) {
  TEST_SUITE_BEGIN("GPU Compute Subsystem (libSceAgc / libSceAgcDriver)");
  RUN_TEST(test_gpu_null_safety);
  RUN_TEST(test_gpu_available_contract);
  RUN_TEST(test_gpu_dispatch_struct_contract);
}
