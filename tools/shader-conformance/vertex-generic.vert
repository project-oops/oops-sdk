uniform mat4 mvp;
attribute vec3 pos;
attribute vec2 uv;
varying vec2 vuv;
void main() { vuv = uv; gl_Position = mvp * vec4(pos, 1.0); }
