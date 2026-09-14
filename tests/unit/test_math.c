#include "oops/math.h"
#include "tests/test_common.h"

static void test_math_scalar_basic(void) {
  ASSERT_FLOAT_NEAR(oops_fabsf(-5.5f), 5.5f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_fabsf(5.5f), 5.5f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_fabsf(0.0f), 0.0f, 1e-6f);

  ASSERT_FLOAT_NEAR(oops_sqrtf(0.0f), 0.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_sqrtf(4.0f), 2.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_sqrtf(16.0f), 4.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_sqrtf(2.0f), 1.41421356f, 1e-5f);
  ASSERT_FLOAT_NEAR(oops_sqrtf(-5.0f), 0.0f, 1e-6f);

  ASSERT_FLOAT_NEAR(oops_clampf(-2.0f, 0.0f, 10.0f), 0.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_clampf(5.0f, 0.0f, 10.0f), 5.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_clampf(15.0f, 0.0f, 10.0f), 10.0f, 1e-6f);

  ASSERT_FLOAT_NEAR(oops_lerpf(10.0f, 20.0f, 0.0f), 10.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_lerpf(10.0f, 20.0f, 1.0f), 20.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_lerpf(10.0f, 20.0f, 0.5f), 15.0f, 1e-6f);

  ASSERT_FLOAT_NEAR(oops_floorf(3.7f), 3.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_floorf(3.0f), 3.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_floorf(-3.2f), -4.0f, 1e-6f);

  ASSERT_FLOAT_NEAR(oops_ceilf(3.2f), 4.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_ceilf(3.0f), 3.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(oops_ceilf(-3.7f), -3.0f, 1e-6f);

  ASSERT_FLOAT_NEAR(oops_fmodf(5.3f, 2.0f), 1.3f, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_fmodf(10.0f, 5.0f), 0.0f, 1e-4f);
}

static void test_math_trigonometry(void) {
  ASSERT_FLOAT_NEAR(oops_sinf(0.0f), 0.0f, 1e-5f);
  ASSERT_FLOAT_NEAR(oops_sinf(OOPS_HALF_PI), 1.0f, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_sinf(OOPS_PI), 0.0f, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_sinf(-OOPS_HALF_PI), -1.0f, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_sinf(OOPS_PI / 6.0f), 0.5f, 1e-4f);

  ASSERT_FLOAT_NEAR(oops_cosf(0.0f), 1.0f, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_cosf(OOPS_HALF_PI), 0.0f, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_cosf(OOPS_PI), -1.0f, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_cosf(OOPS_PI / 3.0f), 0.5f, 1e-4f);

  ASSERT_FLOAT_NEAR(oops_tanf(0.0f), 0.0f, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_tanf(OOPS_PI / 4.0f), 1.0f, 1e-3f);

  ASSERT_FLOAT_NEAR(oops_atan2f(0.0f, 1.0f), 0.0f, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_atan2f(1.0f, 1.0f), OOPS_PI / 4.0f, 1e-3f);
  ASSERT_FLOAT_NEAR(oops_atan2f(1.0f, 0.0f), OOPS_HALF_PI, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_atan2f(-1.0f, 0.0f), -OOPS_HALF_PI, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_atan2f(0.0f, -1.0f), OOPS_PI, 1e-3f);
}

static void test_math_exp_log_pow(void) {
  ASSERT_FLOAT_NEAR(oops_expf(0.0f), 1.0f, 1e-5f);
  ASSERT_FLOAT_NEAR(oops_expf(1.0f), 2.71828182f, 1e-3f);
  ASSERT_FLOAT_NEAR(oops_expf(2.0f), 7.389056f, 1e-2f);

  ASSERT_FLOAT_NEAR(oops_logf(1.0f), 0.0f, 1e-5f);
  ASSERT_FLOAT_NEAR(oops_logf(2.71828182f), 1.0f, 1e-3f);
  ASSERT_FLOAT_NEAR(oops_logf(10.0f), 2.302585f, 1e-3f);

  ASSERT_FLOAT_NEAR(oops_powf(2.0f, 3.0f), 8.0f, 1e-3f);
  ASSERT_FLOAT_NEAR(oops_powf(4.0f, 0.5f), 2.0f, 1e-3f);
  ASSERT_FLOAT_NEAR(oops_powf(5.0f, 0.0f), 1.0f, 1e-5f);
  ASSERT_FLOAT_NEAR(oops_powf(2.0f, -2.0f), 0.25f, 1e-4f);
  ASSERT_FLOAT_NEAR(oops_powf(-2.0f, 3.0f), -8.0f, 1e-3f);
}

static void test_math_vector3(void) {
  oops_vec3_t a = oops_vec3_make(1.0f, 2.0f, 3.0f);
  oops_vec3_t b = oops_vec3_make(4.0f, -5.0f, 6.0f);

  ASSERT_FLOAT_NEAR(oops_vec3_dot(a, b), 12.0f, 1e-5f);

  oops_vec3_t x_axis = oops_vec3_make(1.0f, 0.0f, 0.0f);
  oops_vec3_t y_axis = oops_vec3_make(0.0f, 1.0f, 0.0f);
  oops_vec3_t z_axis = oops_vec3_cross(x_axis, y_axis);
  ASSERT_FLOAT_NEAR(z_axis.x, 0.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(z_axis.y, 0.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(z_axis.z, 1.0f, 1e-6f);

  oops_vec3_t v = oops_vec3_make(3.0f, 0.0f, 4.0f);
  ASSERT_FLOAT_NEAR(oops_vec3_length(v), 5.0f, 1e-5f);

  oops_vec3_t norm = oops_vec3_normalize(v);
  ASSERT_FLOAT_NEAR(norm.x, 0.6f, 1e-5f);
  ASSERT_FLOAT_NEAR(norm.y, 0.0f, 1e-5f);
  ASSERT_FLOAT_NEAR(norm.z, 0.8f, 1e-5f);
  ASSERT_FLOAT_NEAR(oops_vec3_length(norm), 1.0f, 1e-5f);
}

static void test_math_matrix4(void) {
  oops_mat4_t id;
  oops_mat4_identity(&id);
  ASSERT_FLOAT_NEAR(id.m[0], 1.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(id.m[5], 1.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(id.m[10], 1.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(id.m[15], 1.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(id.m[1], 0.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(id.m[4], 0.0f, 1e-6f);

  oops_mat4_t res;
  oops_mat4_mul(&res, &id, &id);
  ASSERT_FLOAT_NEAR(res.m[0], 1.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(res.m[5], 1.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(res.m[10], 1.0f, 1e-6f);
  ASSERT_FLOAT_NEAR(res.m[15], 1.0f, 1e-6f);

  oops_mat4_t trans;
  oops_mat4_identity(&trans);
  oops_mat4_translate(&trans, 10.0f, 20.0f, 30.0f);
  ASSERT_FLOAT_NEAR(trans.m[12], 10.0f, 1e-5f);
  ASSERT_FLOAT_NEAR(trans.m[13], 20.0f, 1e-5f);
  ASSERT_FLOAT_NEAR(trans.m[14], 30.0f, 1e-5f);

  oops_mat4_t scale;
  oops_mat4_identity(&scale);
  oops_mat4_scale(&scale, 2.0f, 3.0f, 4.0f);
  ASSERT_FLOAT_NEAR(scale.m[0], 2.0f, 1e-5f);
  ASSERT_FLOAT_NEAR(scale.m[5], 3.0f, 1e-5f);
  ASSERT_FLOAT_NEAR(scale.m[10], 4.0f, 1e-5f);

  oops_mat4_t proj;
  oops_mat4_perspective(&proj, 60.0f * OOPS_DEG2RAD, 16.0f / 9.0f, 0.1f, 100.0f);
  ASSERT_TRUE(proj.m[0] > 0.0f);
  ASSERT_TRUE(proj.m[5] > 0.0f);
  ASSERT_FLOAT_NEAR(proj.m[11], -1.0f, 1e-5f);

  oops_mat4_t view;
  oops_mat4_lookat(&view,
                   oops_vec3_make(0.0f, 0.0f, 5.0f),
                   oops_vec3_make(0.0f, 0.0f, 0.0f),
                   oops_vec3_make(0.0f, 1.0f, 0.0f));
  ASSERT_FLOAT_NEAR(view.m[14], -5.0f, 1e-5f);
}

void run_unit_tests_math(void) {
  TEST_SUITE_BEGIN("Freestanding Math Library");
  RUN_TEST(test_math_scalar_basic);
  RUN_TEST(test_math_trigonometry);
  RUN_TEST(test_math_exp_log_pow);
  RUN_TEST(test_math_vector3);
  RUN_TEST(test_math_matrix4);
  TEST_SUITE_END();
}

