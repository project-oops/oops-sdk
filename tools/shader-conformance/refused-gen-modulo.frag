void main() {
  int a = 7;
  int b = a % 3;
  gl_FragColor = vec4(float(b) * 0.1, 0.0, 0.0, 1.0);
}
