float f() {
  const int N = 3;
  float w[N];
  w[0] = 0.25; w[1] = 0.5; w[2] = 0.75;
  float t = 0.0;
  for (int i = 0; i < N; i++) t += w[i];
  return t;
}
void main() { gl_FragColor = vec4(f() * 0.5, 0.0, 0.0, 1.0); }
