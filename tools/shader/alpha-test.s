.text
// v7 is the interpolated alpha in both pixel shaders; v12 is free in both (the untextured one
// uses v8..v10 for its canary, the textured one v8..v11 for the sample).
v_mov_b32 v12, 0x3e800000
v_cmp_lt_f32 vcc_lo, v7, v12
v_cmp_eq_f32 vcc_lo, v7, v12
v_cmp_le_f32 vcc_lo, v7, v12
v_cmp_gt_f32 vcc_lo, v7, v12
v_cmp_neq_f32 vcc_lo, v7, v12
v_cmp_ge_f32 vcc_lo, v7, v12
s_and_b32 exec_lo, exec_lo, vcc_lo
s_mov_b32 exec_lo, 0
s_nop 0
