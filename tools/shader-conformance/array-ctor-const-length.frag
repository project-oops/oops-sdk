#version 120
const int N = 3;
void main() {
  float w[N] = float[N](0.25, 0.5, 0.75);
  float t = 0.0;
  int i;
  for (i = 0; i < N; i++) { t += w[i]; }
  gl_FragColor = vec4(t * 0.5, w[0], w[2], 1.0);
}
