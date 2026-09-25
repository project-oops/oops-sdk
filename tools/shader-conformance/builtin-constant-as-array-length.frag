void main() {
  vec2 offs[gl_MaxTextureCoords];
  int i;
  for (i = 0; i < gl_MaxTextureCoords; i++) { offs[i] = vec2(float(i) * 0.25); }
  gl_FragColor = vec4(offs[0].x, offs[1].x, 0.0, 1.0);
}
