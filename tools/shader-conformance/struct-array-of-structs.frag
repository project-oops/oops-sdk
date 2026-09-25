struct Inner { float x; float y; };
struct Outer { Inner i[2]; };
void main() {
  Outer o;
  o.i[0].x = 0.25; o.i[0].y = 0.5;
  o.i[1].x = 0.75; o.i[1].y = 1.0;
  gl_FragColor = vec4(o.i[0].x, o.i[1].x, o.i[0].y, o.i[1].y);
}
