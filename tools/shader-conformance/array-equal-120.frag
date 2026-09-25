#version 120
uniform float k;
void main() {
  float a[3];
  float b[3];
  float c[3];
  a[0] = 0.25; a[1] = 0.5; a[2] = 0.75;
  b[0] = 0.25; b[1] = 0.5; b[2] = 0.75;
  c[0] = 0.25; c[1] = 0.5; c[2] = k;
  gl_FragColor = vec4((a == b) ? 0.75 : 0.0, (a == c) ? 1.0 : 0.25,
                      (a != c) ? 0.5 : 0.0, 1.0);
}
