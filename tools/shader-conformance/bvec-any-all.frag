uniform vec3 v;
void main() {
  bvec3 b = greaterThan(v, vec3(0.5));
  gl_FragColor = vec4(any(b) ? 1.0 : 0.0, all(b) ? 1.0 : 0.0,
                      any(not(b)) ? 1.0 : 0.0, 1.0);
}
