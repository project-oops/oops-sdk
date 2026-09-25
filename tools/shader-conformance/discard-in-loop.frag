varying vec2 uv;
void main() {
  float t = 0.0;
  for (int i = 0; i < 4; i++) {
    if (uv.x < float(i) * 0.1) discard;
    t += 0.2;
  }
  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);
}
