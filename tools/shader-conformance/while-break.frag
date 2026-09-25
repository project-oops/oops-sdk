void main() {
  float t = 0.0;
  int i = 0;
  while (i < 10) { t += 0.1; i++; if (t > 0.35) break; }
  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);
}
