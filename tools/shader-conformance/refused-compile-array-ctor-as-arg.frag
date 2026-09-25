#version 120
float total(float w[2]) { return w[0] + w[1]; }
void main() { gl_FragColor = vec4(total(float[2](0.25, 0.5)), 0.0, 0.0, 1.0); }
