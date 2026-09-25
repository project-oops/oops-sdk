#version 120
uniform mat3 m;
void main() {
  mat3 t = transpose(m);
  mat3 p = m * t;
  gl_FragColor = vec4(p[0][0], p[1][1], p[2][2], 1.0);
}
