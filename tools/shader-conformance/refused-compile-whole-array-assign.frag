void main() {
  float a[2];
  float b[2];
  a[0] = 1.0; a[1] = 2.0;
  b = a;
  gl_FragColor = vec4(b[0], b[1], 0.0, 1.0);
}
