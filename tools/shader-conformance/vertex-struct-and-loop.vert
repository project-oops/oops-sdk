struct Light { vec3 dir; float k; };
attribute vec4 pos;
attribute vec3 nrm;
varying float lit;
void main() {
  Light l; l.dir = vec3(0.0, 0.0, 1.0); l.k = 0.75;
  float t = 0.0;
  for (int i = 0; i < 3; i++) { t += max(dot(nrm, l.dir), 0.0) * l.k * 0.25; }
  lit = t;
  gl_Position = pos;
}
