#version 120
void main() {
  float a[2];
  float b[2];
  a[0] = 0.25; a[1] = 0.5;
  b[0] = 0.1; b[1] = 0.2;
  b += a;
  gl_FragColor = vec4(b[0], b[1], 0.0, 1.0);
}
