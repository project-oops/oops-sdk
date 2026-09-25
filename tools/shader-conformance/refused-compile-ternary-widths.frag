uniform float k;
void main() {
  vec3 v = (k > 0.5) ? vec3(1.0) : vec2(0.0);
  gl_FragColor = vec4(v, 1.0);
}
