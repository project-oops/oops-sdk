uniform float t;
void main() {
  float a = atan(t, 1.0) + asin(clamp(t,-1.0,1.0)) + acos(clamp(t,-1.0,1.0));
  gl_FragColor = vec4(fract(a), mod(a, 1.0), sign(a) * 0.5 + 0.5, 1.0);
}
