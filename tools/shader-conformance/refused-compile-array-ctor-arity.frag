#version 120
void main() {
  float w[3] = float[3](0.25, 0.5);
  gl_FragColor = vec4(w[0], w[1], w[2], 1.0);
}
