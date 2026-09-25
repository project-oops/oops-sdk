uniform float t;
void main() {
  vec3 c = (t > 0.5) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
  gl_FragColor = vec4(c, 1.0);
}
