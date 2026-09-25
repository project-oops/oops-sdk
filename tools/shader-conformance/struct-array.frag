struct S { float a; vec2 b; };
void main() {
  S s[2];
  s[0].a = 0.25; s[1].a = 0.5;
  gl_FragColor = vec4(s[0].a, s[1].a, 0.0, 1.0);
}
