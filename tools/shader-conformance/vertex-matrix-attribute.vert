attribute mat4 xform;
attribute vec4 pos;
varying vec3 n;
void main() {
  n = normalize(vec3(xform * pos));
  gl_Position = xform * pos;
}
