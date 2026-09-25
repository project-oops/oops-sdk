uniform int k;
void main() {
  vec4 c = vec4(0.0);
  switch (k) { case 0: c = vec4(1.0); break; default: c = vec4(0.5); }
  gl_FragColor = c;
}
