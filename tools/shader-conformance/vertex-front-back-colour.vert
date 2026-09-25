void main() {
  gl_FrontColor = gl_Color;
  gl_BackColor = vec4(1.0) - gl_Color;
  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
}
