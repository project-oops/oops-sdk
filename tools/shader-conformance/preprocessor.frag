#define HALF 0.5
#define SCALE(x) ((x) * HALF)
#if defined(HALF) && (2 > 1)
#  define OK 1
#else
#  define OK 0
#endif
void main() {
#if OK
  gl_FragColor = vec4(SCALE(1.0), SCALE(0.5), 0.0, 1.0);
#else
  gl_FragColor = vec4(0.0);
#endif
}
