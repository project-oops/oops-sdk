#define HALF 0.5
#define ADD(a, b) ((a) + (b))
#define TWICE(x) ADD(x, x)
void main() { gl_FragColor = vec4(TWICE(HALF), ADD(HALF, 0.25), 0.0, 1.0); }
