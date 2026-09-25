#define TWO 2
#define DOUBLE(x) ((x) * 2)
#if DOUBLE(TWO) == 4
#  define OK 1
#endif
void main() {
#ifdef OK
  gl_FragColor = vec4(0.75, 0.0, 0.0, 1.0);
#else
  gl_FragColor = vec4(0.0);
#endif
}
