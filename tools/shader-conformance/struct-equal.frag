struct S { float a; vec2 b; };
void main() {
  S p; p.a = 1.0; p.b = vec2(2.0, 3.0);
  S q; q.a = 1.0; q.b = vec2(2.0, 3.0);
  gl_FragColor = (p == q) ? vec4(1.0) : vec4(0.0);
}
