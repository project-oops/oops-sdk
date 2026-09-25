uniform sampler2D tex;
varying vec2 uv;
void main() {
  if (uv.x < 0.1) discard;
  gl_FragColor = texture2D(tex, uv);
}
