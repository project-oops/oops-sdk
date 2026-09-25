float f(float gl_x) { return gl_x * 0.5; }
void main() { gl_FragColor = vec4(f(1.0), 0.0, 0.0, 1.0); }
