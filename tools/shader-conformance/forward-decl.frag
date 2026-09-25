float later(float x);
void main() { gl_FragColor = vec4(later(0.5), 0.0, 0.0, 1.0); }
float later(float x) { return x * 0.5; }
