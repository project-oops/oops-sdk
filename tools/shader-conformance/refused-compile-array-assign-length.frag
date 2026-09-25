#version 120
void main() {
  float a[2];
  float b[3];
  a[0] = 0.25; a[1] = 0.5;
  b = a;
  gl_FragColor = vec4(b[0], b[1], b[2], 1.0);
}
