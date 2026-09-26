/*
 * Freestanding math and 3D matrix/vector library implementation.
 * Zero libc dependencies.
 */

#include "oops/math.h"
#include "oops/freestd.h"
#ifndef OOPS_HOST_BUILD
#include "libc/math.h"
#endif

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

/* ===================================================================
 * Double-precision scalar math
 *
 * See the note above the declarations in <oops/math.h> for why these exist rather than widening
 * the float kernels above. The reduction scheme and coefficients are the classic fdlibm ones,
 * which every libc's double kernels descend from.
 * =================================================================== */

/* `sqrtsd` is exact for doubles and is one instruction. `__builtin_sqrt` is deliberately not used:
 * on a freestanding target it is free to lower to a call to `sqrt`, which is this file's own
 * caller - the self-naming relocation that has bitten this SDK before. */
double oops_sqrt(double x) {
#if defined(__x86_64__)
  double r;
  __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
  return r;
#else
  if (!(x > 0.0)) return (x == 0.0) ? x : (x - x) / (x - x);
  double g = x;
  for (int i = 0; i < 60; i++) {
    double n = 0.5 * (g + x / g);
    if (n == g) break;
    g = n;
  }
  return g;
#endif
}

double oops_floor(double x) {
  if (!(x > -9.0e15 && x < 9.0e15)) return x; /* already integral, or not a number */
  double t = (double)(long long)x;
  return (t > x) ? t - 1.0 : t;
}

double oops_ceil(double x) {
  if (!(x > -9.0e15 && x < 9.0e15)) return x;
  double t = (double)(long long)x;
  return (t < x) ? t + 1.0 : t;
}

/* Exact: the quotient is removed a power of two at a time, so nothing is lost to a huge `x/y`.
 * The outer loop runs once per exponent of difference. */
double oops_fmod(double x, double y) {
  if (y == 0.0 || x != x || y != y) return (x - x) / (x - x);
  double ax = (x < 0.0) ? -x : x;
  double ay = (y < 0.0) ? -y : y;
  if (ax < ay) return x;
  while (ax >= ay) {
    double d = ay;
    while (d * 2.0 <= ax) d *= 2.0;
    ax -= d;
  }
  return (x < 0.0) ? -ax : ax;
}

/* --- sin/cos: Cody-Waite reduction onto [-pi/4, pi/4], then the fdlibm kernels --------------- */

static const double OOPS_PIO2_HI = 1.57079632673412561417e+00;
static const double OOPS_PIO2_MID = 6.07710050650619224932e-11;
static const double OOPS_PIO2_LO = 2.02226624879595063154e-21;

static double oops_kernel_sin(double x, double y) {
  static const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                      S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                      S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
  const double z = x * x;
  const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
  const double v = z * x;
  return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

static double oops_kernel_cos(double x, double y) {
  static const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                      C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                      C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
  const double z = x * x;
  const double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
  const double hz = 0.5 * z;
  const double w = 1.0 - hz;
  return w + (((1.0 - w) - hz) + (z * r - x * y));
}

/* Leaves the reduced argument in *r with its low part in *rl, and returns the quadrant. */
static int oops_rem_pio2(double x, double *r, double *rl) {
  const double inv_pio2 = 6.36619772367581382433e-01;
  double fn = x * inv_pio2;
  fn = (fn >= 0.0) ? oops_floor(fn + 0.5) : oops_ceil(fn - 0.5);
  const double hi = x - fn * OOPS_PIO2_HI;
  const double lo = fn * OOPS_PIO2_MID;
  const double y = hi - lo;
  *r = y;
  *rl = (hi - y) - lo - fn * OOPS_PIO2_LO;
  const long long n = (long long)fn;
  return (int)(n & 3);
}

double oops_sin(double x) {
  if (x != x || x > 1.0e308 || x < -1.0e308) return x - x;
  if (x < 0.7853981633974483 && x > -0.7853981633974483) return oops_kernel_sin(x, 0.0);
  double r, rl;
  switch (oops_rem_pio2(x, &r, &rl)) {
    case 0: return oops_kernel_sin(r, rl);
    case 1: return oops_kernel_cos(r, rl);
    case 2: return -oops_kernel_sin(r, rl);
    default: return -oops_kernel_cos(r, rl);
  }
}

double oops_cos(double x) {
  if (x != x || x > 1.0e308 || x < -1.0e308) return x - x;
  if (x < 0.7853981633974483 && x > -0.7853981633974483) return oops_kernel_cos(x, 0.0);
  double r, rl;
  switch (oops_rem_pio2(x, &r, &rl)) {
    case 0: return oops_kernel_cos(r, rl);
    case 1: return -oops_kernel_sin(r, rl);
    case 2: return -oops_kernel_cos(r, rl);
    default: return oops_kernel_sin(r, rl);
  }
}

double oops_tan(double x) {
  const double c = oops_cos(x);
  if (c == 0.0) return (x > 0.0) ? 1.0e308 : -1.0e308;
  return oops_sin(x) / c;
}

/* --- atan: reduce to |t| <= tan(pi/8), then one odd polynomial ------------------------------- */

static double oops_atan_kernel(double x) {
  static const double T0 = 3.33333333333329318027e-01, T1 = -1.99999999998764832476e-01,
                      T2 = 1.42857142725034663711e-01, T3 = -1.11111104054623557880e-01,
                      T4 = 9.09088713343650656196e-02, T5 = -7.69187620504482999495e-02,
                      T6 = 6.66107313738753120669e-02, T7 = -5.83357013379057348645e-02,
                      T8 = 4.97687799461593236017e-02, T9 = -3.65315727442169155270e-02,
                      T10 = 1.62858201153657823623e-02;
  const double z = x * x, w = z * z;
  const double s1 = z * (T0 + w * (T2 + w * (T4 + w * (T6 + w * (T8 + w * T10)))));
  const double s2 = w * (T1 + w * (T3 + w * (T5 + w * (T7 + w * T9))));
  return x - x * (s1 + s2);
}

/*
 * atan on [0, 1], which is the only interval the kernel above is fitted for (|t| <= 0.4375).
 *
 * The pi/4 constant is split into a head and a tail because the identity adds it to a value of
 * similar size: with a single rounded constant the sum loses the last three digits, which the
 * comparison against the host libm in `tests/unit/test_math.c` fails on.
 */
static double oops_atan01(double x) {
  static const double PIO4_HI = 7.85398163397448278999e-01;
  static const double PIO4_LO = 3.06161699786838301793e-17;
  if (x > 0.41421356237309503) {
    return PIO4_HI + (oops_atan_kernel((x - 1.0) / (x + 1.0)) + PIO4_LO);
  }
  return oops_atan_kernel(x);
}

double oops_atan(double x) {
  static const double PIO2_HI = 1.57079632679489655800e+00;
  static const double PIO2_LO = 6.12323399573676603587e-17;
  if (x != x) return x;
  int neg = 0;
  if (x < 0.0) {
    x = -x;
    neg = 1;
  }
  double r;
  if (x > 1.0e17) {
    r = PIO2_HI + PIO2_LO;
  } else if (x > 1.0) {
    /* Inverting lands in [0,1), which still needs the second reduction - going straight to the
       kernel here was wrong for x just above 1, where 1/x is far outside its fitted range. */
    r = PIO2_HI - (oops_atan01(1.0 / x) - PIO2_LO);
  } else {
    r = oops_atan01(x);
  }
  return neg ? -r : r;
}

double oops_atan2(double y, double x) {
  const double pi = 3.14159265358979323846;
  const double pio2 = 1.57079632679489661923;
  if (x != x || y != y) return x + y;
  if (y == 0.0) return (x >= 0.0) ? 0.0 : pi;
  if (x == 0.0) return (y > 0.0) ? pio2 : -pio2;
  const double a = oops_atan(y / x);
  if (x > 0.0) return a;
  return (y > 0.0) ? a + pi : a - pi;
}

/*
 * The half-angle forms, which are the whole point of this file.
 *
 * `acos(x) = 2*atan2(sqrt(1-x), sqrt(1+x))` stays well conditioned as x approaches 1, where the
 * naive `atan2(sqrt(1-x*x), x)` loses the answer to cancellation - and where a float kernel has
 * already rounded the argument to exactly 1 and returns a flat zero.
 */
double oops_acos(double x) {
  if (x != x) return x;
  if (x >= 1.0) return 0.0;
  if (x <= -1.0) return 3.14159265358979323846;
  return 2.0 * oops_atan2(oops_sqrt(1.0 - x), oops_sqrt(1.0 + x));
}

double oops_asin(double x) {
  if (x != x) return x;
  if (x >= 1.0) return 1.57079632679489661923;
  if (x <= -1.0) return -1.57079632679489661923;
  return oops_atan2(x, oops_sqrt((1.0 - x) * (1.0 + x)));
}

/* --- exp/log/pow ----------------------------------------------------------------------------- */

double oops_exp(double x) {
  static const double LN2_HI = 6.93147180369123816490e-01, LN2_LO = 1.90821492927058770002e-10;
  static const double P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03,
                      P3 = 6.61375632143793436117e-05, P4 = -1.65339022054652515390e-06,
                      P5 = 4.13813679705723846039e-08;
  if (x != x) return x;
  if (x > 709.782712893384) return 1.0e308 * 10.0;
  if (x < -745.133219101941) return 0.0;

  const double invln2 = 1.44269504088896338700;
  double fn = x * invln2;
  fn = (fn >= 0.0) ? oops_floor(fn + 0.5) : oops_ceil(fn - 0.5);
  const double hi = x - fn * LN2_HI;
  const double lo = fn * LN2_LO;
  const double r = hi - lo;

  const double z = r * r;
  const double c = r - z * (P1 + z * (P2 + z * (P3 + z * (P4 + z * P5))));
  const double y = 1.0 + (r * c / (2.0 - c) + r);

  /* 2^k by building the exponent field, in steps so a large k cannot overflow it. */
  int k = (int)fn;
  double scale = 1.0;
  while (k > 1000) {
    scale *= 8.98846567431158e307;
    k -= 1000;
  }
  while (k < -1000) {
    scale *= 1.1125369292536007e-308;
    k += 1000;
  }
  uint64_t bits = (uint64_t)(k + 1023) << 52;
  double p;
  memcpy(&p, &bits, sizeof(p));
  return y * p * scale;
}

double oops_ln(double x) {
  static const double LG1 = 6.666666666666735130e-01, LG2 = 3.999999999940941908e-01,
                      LG3 = 2.857142874366239149e-01, LG4 = 2.222219843214978396e-01,
                      LG5 = 1.818357216161805012e-01, LG6 = 1.531383769920937332e-01,
                      LG7 = 1.479819860511658591e-01;
  static const double LN2_HI = 6.93147180369123816490e-01, LN2_LO = 1.90821492927058770002e-10;
  if (x != x) return x;
  if (x < 0.0) return (x - x) / (x - x);
  if (x == 0.0) return -1.0e308 * 10.0;

  uint64_t bits;
  memcpy(&bits, &x, sizeof(bits));
  int k = (int)((bits >> 52) & 0x7FF) - 1023;
  if (k == -1023) { /* subnormal: scale up by 2^54 and take it off the exponent */
    x *= 18014398509481984.0;
    memcpy(&bits, &x, sizeof(bits));
    k = (int)((bits >> 52) & 0x7FF) - 1023 - 54;
  }
  bits = (bits & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
  double m;
  memcpy(&m, &bits, sizeof(m));
  if (m > 1.4142135623730951) {
    m *= 0.5;
    k++;
  }

  const double f = m - 1.0;
  const double s = f / (2.0 + f);
  const double z = s * s;
  const double w = z * z;
  const double t1 = w * (LG2 + w * (LG4 + w * LG6));
  const double t2 = z * (LG1 + w * (LG3 + w * (LG5 + w * LG7)));
  const double R = t2 + t1;
  const double hfsq = 0.5 * f * f;
  return (double)k * LN2_HI - ((hfsq - (s * (hfsq + R) + (double)k * LN2_LO)) - f);
}

double oops_pow(double base, double exp_) {
  if (exp_ == 0.0) return 1.0;
  if (base != base || exp_ != exp_) return base + exp_;
  if (base == 1.0) return 1.0;
  if (base == 0.0) return (exp_ > 0.0) ? 0.0 : 1.0e308 * 10.0;
  if (base < 0.0) {
    /* Real only for an integral exponent. */
    const double r = oops_floor(exp_);
    if (r != exp_) return (base - base) / (base - base);
    const double mag = oops_exp(exp_ * oops_ln(-base));
    return (oops_fmod(r, 2.0) == 0.0) ? mag : -mag;
  }
  return oops_exp(exp_ * oops_ln(base));
}

#ifndef OOPS_HOST_BUILD
/* Hyperbolic functions and extensions */
double sinh(double x) {
  double e = oops_exp(x);
  return 0.5 * (e - 1.0 / e);
}
float sinhf(float x) { return (float)sinh((double)x); }

double cosh(double x) {
  double e = oops_exp(x);
  return 0.5 * (e + 1.0 / e);
}
float coshf(float x) { return (float)cosh((double)x); }

double tanh(double x) {
  double e = oops_exp(2.0 * x);
  return (e - 1.0) / (e + 1.0);
}
float tanhf(float x) { return (float)tanh((double)x); }

double asinh(double x) {
  return oops_ln(x + oops_sqrt(x * x + 1.0));
}
float asinhf(float x) { return (float)asinh((double)x); }

double acosh(double x) {
  if (x < 1.0) return (x - x) / (x - x);
  return oops_ln(x + oops_sqrt(x * x - 1.0));
}
float acoshf(float x) { return (float)acosh((double)x); }

double atanh(double x) {
  return 0.5 * oops_ln((1.0 + x) / (1.0 - x));
}
float atanhf(float x) { return (float)atanh((double)x); }

double cbrt(double x) {
  return x < 0.0 ? -oops_pow(-x, 1.0 / 3.0) : oops_pow(x, 1.0 / 3.0);
}
float cbrtf(float x) { return (float)cbrt((double)x); }

double log1p(double x) {
  return oops_ln(1.0 + x);
}
float log1pf(float x) { return (float)log1p((double)x); }

double expm1(double x) {
  return oops_exp(x) - 1.0;
}
float expm1f(float x) { return (float)expm1((double)x); }
#endif

