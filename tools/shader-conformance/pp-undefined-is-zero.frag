#if NOT_DEFINED_ANYWHERE
#  define BAD 1
#endif
void main() {
#ifndef BAD
  gl_FragColor = vec4(0.5, 0.0, 0.0, 1.0);
#else
  gl_FragColor = vec4(0.0);
#endif
}
