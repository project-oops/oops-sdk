struct S { float a; vec3 b; };
S make(float k) { S s; s.a = k; s.b = vec3(k, k*2.0, k*3.0); return s; }
void main() { S s = make(0.25); gl_FragColor = vec4(s.a, s.b.y, s.b.z, 1.0); }
