.text
// Fog on the console (gl_ps_patch_fog). The per-vertex factor f - clamped to 0..1 on the CPU,
// where the software path computes it - rides in the texture parameter's spare z, which the
// vertex shader already exports; both pixel shaders interpolate it here and blend the colour in
// v4..v6 towards the fog colour, c * f + fog * (1 - f), leaving alpha alone. The colour is a
// literal per channel, patched in place like the alpha test's reference; 0.3 stands in for it.
// Not 0.5, or 0, or 1: those have inline encodings, and the assembler uses them - no literal
// word, and so nowhere to patch the colour into. v13 and v14 are free in both shaders.
v_interp_p1_f32 v13, v0, attr1.z
v_interp_p2_f32 v13, v1, attr1.z
v_sub_f32 v14, 1.0, v13
v_mul_f32 v4, v4, v13
v_fmac_f32 v4, 0x3e99999a, v14
v_mul_f32 v5, v5, v13
v_fmac_f32 v5, 0x3e99999a, v14
v_mul_f32 v6, v6, v13
v_fmac_f32 v6, 0x3e99999a, v14
// The cross-check: the textured shader's own interpolation of attr1.x, in the tree as 0xc8080400.
v_interp_p1_f32 v2, v0, attr1.x
s_nop 0
