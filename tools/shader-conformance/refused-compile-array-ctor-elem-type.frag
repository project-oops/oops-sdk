#version 120
void main() {
  vec2 p[2] = vec2[2](vec2(0.25, 0.5), 0.75);
  gl_FragColor = vec4(p[0], p[1]);
}
