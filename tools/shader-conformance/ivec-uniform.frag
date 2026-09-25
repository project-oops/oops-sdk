uniform ivec4 colour;
uniform bvec3 flags;
void main() {
  gl_FragColor = vec4(colour) / 255.0 + vec4(flags.x ? 0.1 : 0.0, 0.0, 0.0, 0.0);
}
