#version 120
float f(float x) { return x * 0.5; }
vec2 f(vec2 v) { return v * 0.25; }
void main() { gl_FragColor = vec4(f(vec2(1.0, 2.0)), f(1), 1.0); }
