void main() {
  float a[2];
  float b[2];
  a[0] = 0.25; a[1] = 0.5;
  b[0] = 0.25; b[1] = 0.5;
  gl_FragColor = (a == b) ? vec4(1.0) : vec4(0.0);
}
