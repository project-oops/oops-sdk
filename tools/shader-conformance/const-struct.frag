struct S { float a; vec2 b; };
void main() {
  const float k = 0.25;
  S s; s.a = k; s.b = vec2(k, k * 2.0);
  gl_FragColor = vec4(s.a, s.b.x, s.b.y, 1.0);
}
