uniform sampler2D tex;
varying vec2 uv;
uniform float bias;
void main() { gl_FragColor = texture2D(tex, uv, bias); }
