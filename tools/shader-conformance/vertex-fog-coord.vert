void main() {
  gl_FogFragCoord = abs((gl_ModelViewMatrix * gl_Vertex).z);
  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
}
