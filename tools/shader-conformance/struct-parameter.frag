struct S { float a; vec2 b; };
float sum(S s) { return s.a + s.b.x + s.b.y; }
void main() {
  S s; s.a = 0.1; s.b = vec2(0.2, 0.3);
  gl_FragColor = vec4(sum(s), 0.0, 0.0, 1.0);
}
