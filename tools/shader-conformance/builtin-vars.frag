void main() {
  float f = gl_FrontFacing ? 1.0 : 0.0;
  gl_FragColor = vec4(gl_FragCoord.x * 0.001, gl_FragCoord.y * 0.001, f, 1.0);
}
