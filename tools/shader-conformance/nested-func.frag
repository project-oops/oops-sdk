float sq(float x) { return x * x; }
float quad(float x) { return sq(sq(x)); }
void main() { gl_FragColor = vec4(quad(0.5), 0.0, 0.0, 1.0); }
