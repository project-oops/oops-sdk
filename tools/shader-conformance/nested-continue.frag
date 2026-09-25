void main() {
  float t = 0.0;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) { if (j == 1) continue; t += 0.05; }
  }
  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);
}
