attribute vec4 pos;
varying float vf;
varying vec2 v2;
varying vec3 v3;
varying vec4 v4;
void main() {
  vf = pos.x;
  v2 = pos.xy;
  v3 = pos.xyz;
  v4 = pos;
  gl_Position = pos;
}
