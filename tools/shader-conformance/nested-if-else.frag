uniform float k;
void main() {
  float t = 0.0;
  if (k > 0.75) { t = 1.0; }
  else if (k > 0.5) { if (k > 0.6) { t = 0.8; } else { t = 0.6; } }
  else if (k > 0.25) { t = 0.4; }
  else { t = 0.2; }
  gl_FragColor = vec4(t, 0.0, 0.0, 1.0);
}
