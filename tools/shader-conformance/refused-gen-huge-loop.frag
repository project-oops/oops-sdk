void main() {
  float t = 0.0;
  for (int i = 0; i < 1000000; i++) { t += 0.000001; }
  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);
}
