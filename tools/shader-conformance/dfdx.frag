#extension GL_OES_standard_derivatives : enable
varying vec2 uv;
void main() {
  gl_FragColor = vec4(abs(dFdx(uv.x)), abs(dFdy(uv.y)), fwidth(uv.x), 1.0);
}
