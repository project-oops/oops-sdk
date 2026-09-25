#version 120
uniform float k;
void main() {
  vec2 p[3] = vec2[3](vec2(0.1, 0.2), vec2(0.3, 0.4), vec2(k, 0.6));
  gl_FragColor = vec4(p[0].x, p[1].y, p[2].y, 1.0);
}
