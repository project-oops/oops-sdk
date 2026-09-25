uniform float t;
void main() {
  float a[4];
  a[0] = 0.1; a[1] = 0.2; a[2] = 0.3; a[3] = 0.4;
  int k = int(t);
  gl_FragColor = vec4(a[k], 0.0, 0.0, 1.0);
}
