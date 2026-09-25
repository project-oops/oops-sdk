#define N 4
#if (N * 2 - 3) == 5 && !(N < 2) || 0
#  define OK 1
#endif
#if N / 2 != 2
#  define BAD 1
#endif
void main() {
#if defined(OK) && !defined(BAD)
  gl_FragColor = vec4(0.5, 0.25, 0.125, 1.0);
#else
  gl_FragColor = vec4(0.0);
#endif
}
