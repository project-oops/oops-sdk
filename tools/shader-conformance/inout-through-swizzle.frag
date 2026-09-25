void bump(inout float x) { x += 0.25; }
void main() {
  vec4 c = vec4(0.0);
  bump(c.g);
  gl_FragColor = vec4(c.r, c.g, c.b, 1.0);
}
