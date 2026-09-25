void main() {
  float a = float(gl_MaxTextureUnits) * 0.25;
  float b = float(gl_MaxVertexAttribs) * 0.01;
  float c = float(gl_MaxTextureImageUnits) * 0.1;
  gl_FragColor = vec4(a, b, c, 1.0);
}
