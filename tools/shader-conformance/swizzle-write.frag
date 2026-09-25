void main() {
  vec4 c = vec4(0.0);
  c.rgb = vec3(0.25, 0.5, 0.75);
  c.a = 1.0;
  c.zyx = c.xyz;
  gl_FragColor = c;
}
