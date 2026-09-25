varying vec3 n;
varying vec3 l;
void main() {
  vec3 nn = normalize(n);
  float d = dot(nn, normalize(l));
  vec3 r = reflect(-l, nn);
  vec3 f = faceforward(nn, l, nn);
  gl_FragColor = vec4(d, length(r), distance(n, l), f.x);
}
