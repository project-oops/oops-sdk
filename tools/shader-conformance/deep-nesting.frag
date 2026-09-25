float a(float x) { return x + 0.01; }
float b(float x) { return a(x) + 0.01; }
float c(float x) { return b(x) + 0.01; }
float d(float x) { return c(x) + 0.01; }
void main() { gl_FragColor = vec4(d(0.5), 0.0, 0.0, 1.0); }
