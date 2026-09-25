void main() {
  float t = 0.0;
  int i;
  for (i = 0; i < gl_MaxDrawBuffers; i++) { t += 0.5; }
  gl_FragData[0] = vec4(t, float(gl_MaxDrawBuffers) * 0.5, 0.0, 1.0);
}
