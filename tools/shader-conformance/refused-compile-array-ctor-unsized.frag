#version 120
void main() {
  float w[2] = float[](0.25, 0.5);
  gl_FragColor = vec4(w[0], w[1], 0.0, 1.0);
}
