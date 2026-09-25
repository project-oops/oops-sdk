float scale(float x) { return x * 0.5; }
vec3 scale(vec3 v) { return v * 0.25; }
void main() { gl_FragColor = vec4(scale(vec3(1.0)), scale(0.5)); }
