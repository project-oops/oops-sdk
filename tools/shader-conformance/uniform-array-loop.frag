const int N = 4;
uniform vec4 K[N];
void main() {
  int i;
  vec4 sum = vec4(0.0);
  for (i = 0; i < N; ++i) { sum += K[i]; }
  gl_FragColor = sum;
}
