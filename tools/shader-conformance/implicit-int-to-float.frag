#version 120
void main() {
  float a = 1;
  vec2 b = vec2(2, 3);
  gl_FragColor = vec4(a * 0.5, b * 0.1, 1.0);
}
