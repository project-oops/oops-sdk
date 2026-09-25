#ifndef OOPS_MATH_H
#define OOPS_MATH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard mathematical constants */
#define OOPS_PI         3.14159265358979323846f
#define OOPS_TWO_PI     6.28318530717958647692f
#define OOPS_HALF_PI    1.57079632679489661923f
#define OOPS_DEG2RAD    (OOPS_PI / 180.0f)
#define OOPS_RAD2DEG    (180.0f / OOPS_PI)

/* Scalar float math */
static inline float oops_fabsf(float x) {
  return __builtin_fabsf(x);
}

static inline float oops_sqrtf(float x) {
  if (x <= 0.0f) return 0.0f;
  return __builtin_sqrtf(x);
}

static inline float oops_clampf(float x, float min_val, float max_val) {
  if (x < min_val) return min_val;
  if (x > max_val) return max_val;
  return x;
}

static inline float oops_lerpf(float a, float b, float t) {
  return a + t * (b - a);
}

/*
 * **Double-precision scalar math, and why it is not the float set widened.**
 *
 * `<libc/math.h>`'s `sin`, `acos` and their siblings used to be one line each: cast the argument
 * to `float`, call the float kernel, widen the result. That is right for a shader-adjacent
 * calculation and wrong for anything that reasons about its own precision, because it silently
 * moves the smallest representable step from 1e-16 to 6e-8.
 *
 * Extreme Tux Racer is the measurement. Its quaternion interpolation guards a singularity the way
 * a double program should - `if (1.0 - cosphi > 1e-13)`, then divide by `sin(acos(cosphi))`. On a
 * double `acos`, `cosphi = 1 - 1e-13` gives `4.5e-7` and the division is ordinary. Through a float
 * `acos` the argument *rounds to exactly 1*, `acos` returns 0, and the division is `0/0`. Every
 * frame in which the player's orientation barely changed produced a NaN quaternion, which reached
 * the course lookup as a NaN position and faulted two layers later. Nothing in between was wrong.
 *
 * So these are real double kernels - argument reduction and polynomials carrying the full 53 bits
 * - and `<libc/math.h>`'s double entry points are one line each on top of them. `tests/unit/
 * test_math.c` checks them against the host's own libm, including the near-1 case above.
 */
double oops_sqrt(double x);
double oops_sin(double x);
double oops_cos(double x);
double oops_tan(double x);
double oops_atan(double x);
double oops_atan2(double y, double x);
double oops_asin(double x);
double oops_acos(double x);
double oops_exp(double x);
double oops_ln(double x);
double oops_pow(double base, double exp_);
double oops_floor(double x);
double oops_ceil(double x);
double oops_fmod(double x, double y);

float oops_floorf(float x);
float oops_ceilf(float x);
float oops_fmodf(float x, float y);
float oops_sinf(float x);
float oops_cosf(float x);
float oops_tanf(float x);
float oops_atan2f(float y, float x);
float oops_expf(float x);
float oops_logf(float x);
float oops_powf(float base, float exp);

/* 3D Vector Primitives */
typedef struct oops_vec3 {
  float x;
  float y;
  float z;
} oops_vec3_t;

static inline oops_vec3_t oops_vec3_make(float x, float y, float z) {
  oops_vec3_t v = {x, y, z};
  return v;
}

static inline float oops_vec3_dot(oops_vec3_t a, oops_vec3_t b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline oops_vec3_t oops_vec3_cross(oops_vec3_t a, oops_vec3_t b) {
  oops_vec3_t r;
  r.x = a.y * b.z - a.z * b.y;
  r.y = a.z * b.x - a.x * b.z;
  r.z = a.x * b.y - a.y * b.x;
  return r;
}

static inline float oops_vec3_length(oops_vec3_t v) {
  return oops_sqrtf(oops_vec3_dot(v, v));
}

oops_vec3_t oops_vec3_normalize(oops_vec3_t v);

/* 4x4 Matrix (Column-major layout matching OpenGL / RDNA2 PM4 uniform buffers) */
typedef struct oops_mat4 {
  float m[16];
} oops_mat4_t;

void oops_mat4_identity(oops_mat4_t *out_mat);
void oops_mat4_mul(oops_mat4_t *out_mat, const oops_mat4_t *a, const oops_mat4_t *b);
void oops_mat4_perspective(oops_mat4_t *out_mat, float fovy_rad, float aspect, float z_near, float z_far);
void oops_mat4_ortho(oops_mat4_t *out_mat, float left, float right, float bottom, float top, float z_near, float z_far);
void oops_mat4_lookat(oops_mat4_t *out_mat, oops_vec3_t eye, oops_vec3_t center, oops_vec3_t up);
void oops_mat4_translate(oops_mat4_t *out_mat, float tx, float ty, float tz);
void oops_mat4_rotate(oops_mat4_t *out_mat, float angle_rad, float rx, float ry, float rz);
void oops_mat4_scale(oops_mat4_t *out_mat, float sx, float sy, float sz);

#ifdef __cplusplus
}
#endif

#endif /* OOPS_MATH_H */

