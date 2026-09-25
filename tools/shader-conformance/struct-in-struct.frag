struct Inner { float x; float y; };
struct Outer { Inner i; float z; };
void main() {
  Outer o;
  o.i.x = 0.25; o.i.y = 0.5; o.z = 0.75;
  gl_FragColor = vec4(o.i.x, o.i.y, o.z, 1.0);
}
