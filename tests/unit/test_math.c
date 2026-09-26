#include "oops/math.h"
#include "tests/test_common.h"
#include <math.h>

/* Unit tests for `oops/math.h` and the libc math replacements. */

/* The scalar helpers (abs, min/max, clamp, rounding, sqrt) give exact answers. */
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

/* The float trigonometric functions agree with known values. */
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

/* exp, log and pow agree with known values. */
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

/* Vector arithmetic, dot, cross and normalise give exact results. */
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

/* Matrix construction, multiplication and transforms give exact results. */
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
    oops_mat4_lookat(&view, oops_vec3_make(0.0f, 0.0f, 5.0f),
                     oops_vec3_make(0.0f, 0.0f, 0.0f),
                     oops_vec3_make(0.0f, 1.0f, 0.0f));
    ASSERT_FLOAT_NEAR(view.m[14], -5.0f, 1e-5f);
}

/*
 * `ldexp` and `frexp` (written out in `src/system/libc.c`) round-trip: `frexp` gives a
 * significand in [0.5, 1) and an exponent, and `ldexp` puts them back. The builtins
 * lower to a call to the function itself on the target, which a host test cannot see;
 * that recursion is caught by sweeping the built objects instead.
 */
static void test_math_ldexp_and_frexp_round_trip(void) {
    int e = 12345;

    /* The significand is in [0.5, 1) and carries the sign; the exponent reassembles the
     * value. */
    ASSERT_FLOAT_NEAR(frexp(48.0, &e), 0.75, 1e-12);
    ASSERT_EQ(e, 6);
    ASSERT_FLOAT_NEAR(ldexp(0.75, 6), 48.0, 1e-12);

    ASSERT_FLOAT_NEAR(frexp(-0.125, &e), -0.5, 1e-12);
    ASSERT_EQ(e, -2);
    ASSERT_FLOAT_NEAR(ldexp(-0.5, -2), -0.125, 1e-12);

    /* Zero is the case with no exponent to report, and it must not be invented. */
    e = 12345;
    ASSERT_FLOAT_NEAR(frexp(0.0, &e), 0.0, 0.0);
    ASSERT_EQ(e, 0);

    /* Exact powers of two, up and down, where any double rounding would show. */
    ASSERT_FLOAT_NEAR(ldexp(1.0, 0), 1.0, 0.0);
    ASSERT_FLOAT_NEAR(ldexp(1.0, 10), 1024.0, 0.0);
    ASSERT_FLOAT_NEAR(ldexp(1.0, -10), 1.0 / 1024.0, 0.0);

    /* Round trip across the range, including the staged scaling for large exponents. */
    const double values[] = {1.0,  3.14159265358979,       1e-300, 1e300,
                             -7.5, 2.2250738585072014e-308};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        int ex = 0;
        const double m = frexp(values[i], &ex);
        ASSERT_TRUE(m == 0.0 || (m > -1.0 && m <= -0.5) || (m >= 0.5 && m < 1.0));
        const double back = ldexp(m, ex);
        ASSERT_TRUE(back == values[i]);
    }

    /* The float forms go through the double ones and must agree exactly. */
    int ef = 0;
    ASSERT_FLOAT_NEAR(frexpf(48.0f, &ef), 0.75f, 1e-6f);
    ASSERT_EQ(ef, 6);
    ASSERT_FLOAT_NEAR(ldexpf(0.75f, 6), 48.0f, 1e-6f);

    /* Round to nearest, ties to even - the property that separates rint from round. */
    ASSERT_FLOAT_NEAR(rint(0.5), 0.0, 0.0);
    ASSERT_FLOAT_NEAR(rint(1.5), 2.0, 0.0);
    ASSERT_FLOAT_NEAR(rint(2.5), 2.0, 0.0);
    ASSERT_FLOAT_NEAR(rint(-2.5), -2.0, 0.0);
    ASSERT_FLOAT_NEAR(rintf(2.5f), 2.0f, 0.0f);
    ASSERT_FLOAT_NEAR(nearbyint(2.5), 2.0, 0.0);
}

/*
 * The double kernels are double precision and agree with the host's libm. `acos` of an
 * argument a hair below 1 must not be 0: quaternion interpolation guards its
 * singularity at 1e-13, and a float-backed `acos` turns `sin(acos(x))` into a zero
 * divisor there.
 */
static void test_math_double_precision(void) {
    /* Just below 1, where float precision rounds to 0. */
    const double near_one = 1.0 - 1e-13;
    ASSERT_TRUE(oops_acos(near_one) > 1e-7);
    ASSERT_TRUE(fabs(oops_acos(near_one) - acos(near_one)) < 1e-9);
    ASSERT_TRUE(oops_sin(oops_acos(near_one)) > 1e-7); /* the divisor, not zero */

    /* sqrt is exact, so it is compared exactly. */
    ASSERT_TRUE(oops_sqrt(2.0) == sqrt(2.0));
    ASSERT_TRUE(oops_sqrt(1e300) == sqrt(1e300));
    ASSERT_TRUE(oops_sqrt(0.0) == 0.0);

    static const double xs[] = {-8.75, -3.0,    -1.0,    -0.5,  -1e-9, 0.0,
                                1e-9,  0.3,     0.5,     1.0,   1.5,   2.5,
                                3.0,   6.28318, 12.5664, 100.0, 1e4,   1e6};
    for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        const double x = xs[i];
        ASSERT_TRUE(fabs(oops_sin(x) - sin(x)) < 1e-12);
        ASSERT_TRUE(fabs(oops_cos(x) - cos(x)) < 1e-12);
        ASSERT_TRUE(fabs(oops_atan(x) - atan(x)) < 1e-12);
        if (x > 0.0) {
            ASSERT_TRUE(fabs(oops_ln(x) - log(x)) < 1e-12);
            ASSERT_TRUE(fabs(oops_sqrt(x) - sqrt(x)) < 1e-12);
        }
        if (x > -20.0 && x < 20.0) {
            ASSERT_TRUE(fabs(oops_exp(x) - exp(x)) < 1e-9 * (exp(x) + 1.0));
        }
    }

    /* asin/acos across the range, including both endpoints. */
    for (int i = -20; i <= 20; i++) {
        const double x = (double)i / 20.0;
        ASSERT_TRUE(fabs(oops_asin(x) - asin(x)) < 1e-12);
        ASSERT_TRUE(fabs(oops_acos(x) - acos(x)) < 1e-12);
    }

    /* atan2 in all four quadrants and on the axes. */
    static const double q[] = {-3.0, -1.0, 0.0, 1.0, 3.0};
    for (unsigned a = 0; a < 5; a++) {
        for (unsigned b = 0; b < 5; b++) {
            if (q[a] == 0.0 && q[b] == 0.0)
                continue;
            ASSERT_TRUE(fabs(oops_atan2(q[a], q[b]) - atan2(q[a], q[b])) < 1e-12);
        }
    }

    ASSERT_TRUE(fabs(oops_pow(2.0, 10.0) - 1024.0) < 1e-9);
    ASSERT_TRUE(fabs(oops_pow(10.0, -3.0) - 0.001) < 1e-15);
    ASSERT_TRUE(fabs(oops_pow(-2.0, 3.0) + 8.0) < 1e-9);
    ASSERT_TRUE(fabs(oops_fmod(7.5, 2.0) - 1.5) < 1e-15);
    ASSERT_TRUE(fabs(oops_fmod(-7.5, 2.0) + 1.5) < 1e-15);
    ASSERT_TRUE(oops_floor(-2.5) == -3.0 && oops_ceil(-2.5) == -2.0);

    /* A NaN in is a NaN out rather than a plausible number: every guard downstream
     * tests for it. */
    const double nan_in = 0.0 / 0.0;
    ASSERT_TRUE(oops_acos(nan_in) != oops_acos(nan_in));
    ASSERT_TRUE(oops_sin(nan_in) != oops_sin(nan_in));
}

void run_unit_tests_math(void) {
    TEST_SUITE_BEGIN("Freestanding Math Library");
    RUN_TEST(test_math_ldexp_and_frexp_round_trip);
    RUN_TEST(test_math_scalar_basic);
    RUN_TEST(test_math_trigonometry);
    RUN_TEST(test_math_exp_log_pow);
    RUN_TEST(test_math_vector3);
    RUN_TEST(test_math_matrix4);
    RUN_TEST(test_math_double_precision);
    TEST_SUITE_END();
}
