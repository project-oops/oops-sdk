float total(float w[4]) {
  return w[0] + w[1] + w[2] + w[3];
}
void main() {
  float a[4];
  a[0] = 0.1; a[1] = 0.2; a[2] = 0.3; a[3] = 0.4;
  gl_FragColor = vec4(total(a), 0.0, 0.0, 1.0);
}
