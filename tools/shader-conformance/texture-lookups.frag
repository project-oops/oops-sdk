uniform sampler2D t2;
uniform sampler3D t3;
uniform samplerCube tc;
varying vec4 c;
void main() {
  vec4 a = texture2D(t2, c.st);
  vec4 b = texture2DProj(t2, c);
  vec4 d = texture3D(t3, c.stp);
  vec4 e = textureCube(tc, c.stp);
  gl_FragColor = a + b + d + e;
}
