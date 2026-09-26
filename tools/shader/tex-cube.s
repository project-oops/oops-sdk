.text
// Sampling a cube map: the texture coordinate taken as a direction, turned into a face and a
// place on it, and sampled with dim:SQ_RSRC_IMG_CUBE against a descriptor whose TYPE is 0xb.
//
// RDNA2 does the face selection in four instructions: V_CUBEID_F32 gives the face the direction
// points at, V_CUBESC_F32 and V_CUBETC_F32 the two coordinates on it, and V_CUBEMA_F32 twice the
// major axis. The shader divides sc and tc by that and biases by a half, which puts them in
// [0, 1], and hands the sampler (u, v, face) as three address registers. That is the sequence
// ACO emits and the ISA documents.
//
// The direction is not divided by q. The prolog divides s and t before this runs, which is
// right for a 2D sample and wrong for a direction: scaling all three components leaves the
// direction alone, so this interpolates them again rather than reading the divided pair. The
// third component is attr2.w, where the vertex carries unit 0's r.
//
// v16..v22 are the general combine's argument registers, which it fills after the sample and not
// before, so they are free here. The texel lands in v4..v7 as the 2D sample's does.
v_interp_p1_f32 v16, v0, attr1.x        // s
v_interp_p2_f32 v16, v1, attr1.x
v_interp_p1_f32 v17, v0, attr1.y        // t
v_interp_p2_f32 v17, v1, attr1.y
v_interp_p1_f32 v18, v0, attr2.w        // r, the third component of the direction
v_interp_p2_f32 v18, v1, attr2.w
v_cubeid_f32 v19, v16, v17, v18         // the face
v_cubesc_f32 v20, v16, v17, v18         // s on it
v_cubetc_f32 v21, v16, v17, v18         // t on it
v_cubema_f32 v22, v16, v17, v18         // twice the major axis
v_rcp_f32_e64 v22, |v22|
v_mul_f32_e32 v22, 0.5, v22             // 1 / (2 * |ma|)
v_fma_f32 v16, v20, v22, 0.5            // u
v_fma_f32 v17, v21, v22, 0.5            // v
v_mov_b32_e32 v18, v19                  // the face, as the third address register
image_sample v[4:7], v[16:18], s[4:11], s[12:15] dmask:0xf dim:SQ_RSRC_IMG_CUBE
// Cross-checks against words already in the tree: the 2D sample this replaces, and the
// interpolation of attr1.x that the prolog does.
image_sample v[4:7], v[2:3], s[4:11], s[12:15] dmask:0xf dim:SQ_RSRC_IMG_2D
v_interp_p1_f32 v2, v0, attr1.x
