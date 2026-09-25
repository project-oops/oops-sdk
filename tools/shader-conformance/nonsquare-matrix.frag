#version 120
uniform mat2x3 m;
void main() {
  vec3 p = m * vec2(1.0, 2.0);
  mat3x2 t = transpose(m);
  gl_FragColor = vec4(p.x, p.z, t[2][0], 1.0);
}
