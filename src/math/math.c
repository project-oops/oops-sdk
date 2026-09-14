/*
 * Freestanding math and 3D matrix/vector library implementation.
 * Zero libc dependencies.
 */

#include "oops/math.h"
#include "oops/freestd.h"

float oops_floorf(float x) {
  int i = (int)x;
  return (x < (float)i) ? (float)(i - 1) : (float)i;
}

float oops_ceilf(float x) {
  int i = (int)x;
  return (x > (float)i) ? (float)(i + 1) : (float)i;
}

float oops_fmodf(float x, float y) {
  if (y == 0.0f) return 0.0f;
  return x - y * oops_floorf(x / y);
}

static inline float sin_poly(float r) {
  float r2 = r * r;
  return r * (1.0f - r2 * (1.0f / 6.0f - r2 * (1.0f / 120.0f - r2 * (1.0f / 5040.0f))));
}

static inline float cos_poly(float r) {
  float r2 = r * r;
  return 1.0f - r2 * (0.5f - r2 * (1.0f / 24.0f - r2 * (1.0f / 720.0f - r2 * (1.0f / 40320.0f))));
}

float oops_sinf(float x) {
  float k = oops_floorf(x * (2.0f / OOPS_PI) + 0.5f);
  float r = x - k * OOPS_HALF_PI;
  int q = (int)k & 3;
  if (q < 0) q += 4;
  switch (q) {
    case 0: return sin_poly(r);
    case 1: return cos_poly(r);
    case 2: return -sin_poly(r);
    default: return -cos_poly(r);
  }
}

float oops_cosf(float x) {
  float k = oops_floorf(x * (2.0f / OOPS_PI) + 0.5f);
  float r = x - k * OOPS_HALF_PI;
  int q = (int)k & 3;
  if (q < 0) q += 4;
  switch (q) {
    case 0: return cos_poly(r);
    case 1: return -sin_poly(r);
    case 2: return -cos_poly(r);
    default: return sin_poly(r);
  }
}

float oops_tanf(float x) {
  float c = oops_cosf(x);
  if (oops_fabsf(c) < 1e-6f) {
    return (c < 0.0f) ? -1e6f : 1e6f;
  }
  return oops_sinf(x) / c;
}

float oops_atan2f(float y, float x) {
  if (x == 0.0f) {
    if (y > 0.0f) return OOPS_HALF_PI;
    if (y < 0.0f) return -OOPS_HALF_PI;
    return 0.0f;
  }
  float ax = oops_fabsf(x);
  float ay = oops_fabsf(y);
  float a = (ax > ay) ? (ay / ax) : (ax / ay);
  float s = a * a;
  /* Minimax polynomial for arctan(a) on [0, 1] */
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = OOPS_HALF_PI - r;
  if (x < 0.0f) r = OOPS_PI - r;
  if (y < 0.0f) r = -r;
  return r;
}

float oops_expf(float x) {
  if (x > 88.0f) return 1e38f;
  if (x < -88.0f) return 0.0f;
  /* Range reduction: exp(x) = 2^(x * log2(e)) */
  float k = oops_floorf(x * 1.4426950408889634f + 0.5f);
  float r = x - k * 0.6931471805599453f;
  /* Minimax polynomial for exp(r) on [-ln2/2, ln2/2] */
  float r2 = r * r;
  float p = 1.0f + r + r2 * (0.5f + r * (0.16666667f + r * (0.041666668f + r * 0.008333333f)));
  /* Scale by 2^k via bit manipulation */
  int ik = (int)k;
  uint32_t bits = (uint32_t)(ik + 127) << 23;
  float scale;
  memcpy(&scale, &bits, sizeof(float));
  return p * scale;
}

float oops_logf(float x) {
  if (x <= 0.0f) return -1e38f;
  /* Extract exponent and mantissa */
  uint32_t bits;
  memcpy(&bits, &x, sizeof(float));
  int exp = (int)((bits >> 23) & 0xFF) - 127;
  bits = (bits & 0x007FFFFF) | 0x3F800000;
  float m;
  memcpy(&m, &bits, sizeof(float));

  /* Align m to [sqrt(2)/2, sqrt(2)] */
  if (m > 1.41421356f) {
    m *= 0.5f;
    exp++;
  }
  float f = (m - 1.0f) / (m + 1.0f);
  float f2 = f * f;
  float poly = f * (2.0f + f2 * (0.66666667f + f2 * (0.4f + f2 * 0.28571429f)));
  return (float)exp * 0.69314718f + poly;
}

float oops_powf(float base, float exp) {
  if (base == 0.0f) return (exp == 0.0f) ? 1.0f : 0.0f;
  if (base < 0.0f) {
    /* Handle integer exponent */
    int ie = (int)exp;
    if ((float)ie == exp) {
      float res = oops_expf(exp * oops_logf(-base));
      return (ie & 1) ? -res : res;
    }
    return 0.0f; /* NaN */
  }
  return oops_expf(exp * oops_logf(base));
}

oops_vec3_t oops_vec3_normalize(oops_vec3_t v) {
  float len = oops_vec3_length(v);
  if (len > 1e-6f) {
    float inv = 1.0f / len;
    oops_vec3_t r = {v.x * inv, v.y * inv, v.z * inv};
    return r;
  }
  oops_vec3_t zero = {0.0f, 0.0f, 0.0f};
  return zero;
}

void oops_mat4_identity(oops_mat4_t *out_mat) {
  if (!out_mat) return;
  for (int i = 0; i < 16; i++) {
    out_mat->m[i] = 0.0f;
  }
  out_mat->m[0] = 1.0f;
  out_mat->m[5] = 1.0f;
  out_mat->m[10] = 1.0f;
  out_mat->m[15] = 1.0f;
}

void oops_mat4_mul(oops_mat4_t *out_mat, const oops_mat4_t *a, const oops_mat4_t *b) {
  if (!out_mat || !a || !b) return;
  oops_mat4_t r;
  for (int c = 0; c < 4; c++) {
    for (int row = 0; row < 4; row++) {
      r.m[c * 4 + row] = a->m[0 * 4 + row] * b->m[c * 4 + 0] +
                         a->m[1 * 4 + row] * b->m[c * 4 + 1] +
                         a->m[2 * 4 + row] * b->m[c * 4 + 2] +
                         a->m[3 * 4 + row] * b->m[c * 4 + 3];
    }
  }
  memcpy(out_mat, &r, sizeof(oops_mat4_t));
}

void oops_mat4_perspective(oops_mat4_t *out_mat, float fovy_rad, float aspect, float z_near, float z_far) {
  if (!out_mat || aspect == 0.0f || z_near == z_far) return;
  float f = 1.0f / oops_tanf(fovy_rad * 0.5f);
  for (int i = 0; i < 16; i++) out_mat->m[i] = 0.0f;

  out_mat->m[0] = f / aspect;
  out_mat->m[5] = f;
  out_mat->m[10] = (z_far + z_near) / (z_near - z_far);
  out_mat->m[11] = -1.0f;
  out_mat->m[14] = (2.0f * z_far * z_near) / (z_near - z_far);
}

void oops_mat4_ortho(oops_mat4_t *out_mat, float left, float right, float bottom, float top, float z_near, float z_far) {
  if (!out_mat || left == right || bottom == top || z_near == z_far) return;
  for (int i = 0; i < 16; i++) out_mat->m[i] = 0.0f;

  out_mat->m[0] = 2.0f / (right - left);
  out_mat->m[5] = 2.0f / (top - bottom);
  out_mat->m[10] = -2.0f / (z_far - z_near);
  out_mat->m[12] = -(right + left) / (right - left);
  out_mat->m[13] = -(top + bottom) / (top - bottom);
  out_mat->m[14] = -(z_far + z_near) / (z_far - z_near);
  out_mat->m[15] = 1.0f;
}

void oops_mat4_lookat(oops_mat4_t *out_mat, oops_vec3_t eye, oops_vec3_t center, oops_vec3_t up) {
  if (!out_mat) return;
  oops_vec3_t f = oops_vec3_make(center.x - eye.x, center.y - eye.y, center.z - eye.z);
  f = oops_vec3_normalize(f);

  oops_vec3_t u = oops_vec3_normalize(up);
  oops_vec3_t s = oops_vec3_cross(f, u);
  s = oops_vec3_normalize(s);
  u = oops_vec3_cross(s, f);

  oops_mat4_t rot;
  oops_mat4_identity(&rot);
  rot.m[0] = s.x;  rot.m[4] = s.y;  rot.m[8]  = s.z;
  rot.m[1] = u.x;  rot.m[5] = u.y;  rot.m[9]  = u.z;
  rot.m[2] = -f.x; rot.m[6] = -f.y; rot.m[10] = -f.z;

  oops_mat4_t trans;
  oops_mat4_identity(&trans);
  trans.m[12] = -eye.x;
  trans.m[13] = -eye.y;
  trans.m[14] = -eye.z;

  oops_mat4_mul(out_mat, &rot, &trans);
}

void oops_mat4_translate(oops_mat4_t *out_mat, float tx, float ty, float tz) {
  if (!out_mat) return;
  oops_mat4_t t;
  oops_mat4_identity(&t);
  t.m[12] = tx;
  t.m[13] = ty;
  t.m[14] = tz;
  oops_mat4_mul(out_mat, out_mat, &t);
}

void oops_mat4_rotate(oops_mat4_t *out_mat, float angle_rad, float rx, float ry, float rz) {
  if (!out_mat) return;
  oops_vec3_t axis = oops_vec3_normalize(oops_vec3_make(rx, ry, rz));
  float c = oops_cosf(angle_rad);
  float s = oops_sinf(angle_rad);
  float omc = 1.0f - c;

  oops_mat4_t r;
  oops_mat4_identity(&r);
  r.m[0] = axis.x * axis.x * omc + c;
  r.m[1] = axis.x * axis.y * omc + axis.z * s;
  r.m[2] = axis.x * axis.z * omc - axis.y * s;

  r.m[4] = axis.y * axis.x * omc - axis.z * s;
  r.m[5] = axis.y * axis.y * omc + c;
  r.m[6] = axis.y * axis.z * omc + axis.x * s;

  r.m[8] = axis.z * axis.x * omc + axis.y * s;
  r.m[9] = axis.z * axis.y * omc - axis.x * s;
  r.m[10] = axis.z * axis.z * omc + c;

  oops_mat4_mul(out_mat, out_mat, &r);
}

void oops_mat4_scale(oops_mat4_t *out_mat, float sx, float sy, float sz) {
  if (!out_mat) return;
  oops_mat4_t sc;
  oops_mat4_identity(&sc);
  sc.m[0] = sx;
  sc.m[5] = sy;
  sc.m[10] = sz;
  oops_mat4_mul(out_mat, out_mat, &sc);
}
