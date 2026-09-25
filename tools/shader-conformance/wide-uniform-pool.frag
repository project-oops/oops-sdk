uniform mat4 a;
uniform mat4 b;
uniform mat4 c;
uniform vec4 d;
void main() { gl_FragColor = vec4(a[0][0], b[1][1], c[2][2], d.w); }
