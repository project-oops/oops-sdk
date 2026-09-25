void main() {
  float t = 0.0;
  int i = 0;
  do { t += 0.25; i++; } while (i < 3);
  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);
}
