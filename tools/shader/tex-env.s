.text
// The texture environment's four words (gl_ps_patch_tex_env). The textured pixel shader samples
// the texel into v4..v7 and interpolates the fragment colour into v8..v11; the export reads
// v4..v7, so each word leaves a result there.
v_mul_f32 v4, v4, v8
v_mul_f32 v5, v5, v9
v_mul_f32 v6, v6, v10
v_mul_f32 v7, v7, v11
v_add_f32 v4, v4, v8
v_add_f32 v5, v5, v9
v_add_f32 v6, v6, v10
v_add_f32 v7, v7, v11
v_mov_b32 v4, v8
v_mov_b32 v5, v9
v_mov_b32 v6, v10
v_mov_b32 v7, v11
s_nop 0
