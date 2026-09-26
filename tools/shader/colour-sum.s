.text
// The colour sum after texturing (GL 1.4, 3.9): the secondary colour -
// lighting's separate specular term, or glSecondaryColor's under GL_COLOR_SUM - interpolated
// from the third parameter's x, y and z, added to the combined colour in v4..v6, and the sum
// clamped to [0, 1]. The textured pixel shader's slot at GL_PS_SUM_SLOT_TEX: after the combine,
// before fog, which is GL's order. v12..v14 are free there - v12 was q, used by then - and fog
// reuses v13 and v14 afterwards. Alpha is the primary colour's: GL leaves the secondary alpha
// out of the sum.
v_interp_p1_f32 v12, v0, attr2.x
v_interp_p2_f32 v12, v1, attr2.x
v_interp_p1_f32 v13, v0, attr2.y
v_interp_p2_f32 v13, v1, attr2.y
v_interp_p1_f32 v14, v0, attr2.z
v_interp_p2_f32 v14, v1, attr2.z
v_add_f32_e64 v4, v4, v12 clamp
v_add_f32_e64 v5, v5, v13 clamp
v_add_f32_e64 v6, v6, v14 clamp
// Cross-check against a word already in the tree: fog's attr1.z interpolation, 0xc8340600.
v_interp_p1_f32 v13, v0, attr1.z
