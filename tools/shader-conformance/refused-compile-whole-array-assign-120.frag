#version 120
void main() {
  float w[2];
  float v[2];
  w[0] = 0.25; w[1] = 0.5;
  v = w;
  gl_FragColor = vec4(v[0], v[1], 0.0, 1.0);
}
