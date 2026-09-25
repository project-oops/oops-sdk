varying vec3 n;
void main() {
  n = normalize(gl_NormalMatrix * gl_Normal);
  vec4 eye = gl_ModelViewMatrix * gl_Vertex;
  vec4 back = gl_ModelViewMatrixInverse * eye;
  gl_Position = gl_ProjectionMatrix * eye + back * 0.0;
}
