struct S { float w[3]; };
void main() {
  S s;
  s.w[0] = 0.25; s.w[1] = 0.5; s.w[2] = 0.75;
  gl_FragColor = vec4(s.w[0], s.w[1], s.w[2], 1.0);
}
