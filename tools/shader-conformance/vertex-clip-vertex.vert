void main() {
  gl_ClipVertex = gl_ModelViewMatrix * gl_Vertex;
  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
}
