varying vec4 c;
void main() {
  vec3 a = c.xyz;
  vec2 b = a.zy;
  gl_FragColor = vec4(b.y, b.x, c.wzyx.x, 1.0);
}
