#define PICK(a, b) (a)
void main() { gl_FragColor = vec4(PICK(vec2(1.0, 0.5).x, 0.0), 0.0, 0.0, 1.0); }
