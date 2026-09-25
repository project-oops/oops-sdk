void bump(inout float x, in float k) { x += k; }
void main() {
  float v = 0.25;
  bump(v, 0.5);
  gl_FragColor = vec4(v, 0.0, 0.0, 1.0);
}
