varying mat3 m;
void main() { gl_FragColor = vec4(m[0][0], m[1][1], m[2][2], 1.0); }
