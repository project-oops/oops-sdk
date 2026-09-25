uniform float size;
void main() {
  gl_PointSize = size;
  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
}
