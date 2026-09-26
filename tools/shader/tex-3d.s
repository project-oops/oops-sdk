.text
// Sampling a 3D texture: the third texture coordinate interpolated, divided by q with the same
// reciprocal the prolog already computed, and sampled with dim:SQ_RSRC_IMG_3D against a
// descriptor whose TYPE is 0xa and whose WORD4 holds the last slice.
//
// Unlike a cube map's, this coordinate is divided. A cube map takes (s, t, r) as a
// direction, which scaling leaves alone, so tex-cube.s interpolates all three fresh; a volume
// takes them as a position in [0, 1]^3, and GL 1.2's projective texturing divides every one of
// them by q. The prolog leaves 1 / q in v12 and never overwrites it, so the divide here is one
// multiply rather than a second reciprocal.
//
// The third component is attr2.w, where the vertex carries unit 0's r - the same parameter the
// cube's direction reads, which is why a volume also forces the three-parameter vertex shader.
//
// The address registers have to be consecutive, and s/q and t/q are in v2 and v3, so the pair
// is copied up beside the new r rather than sampled in place: v4 is the texel's own first
// register and building the address across v[2:4] would name it twice. v16..v18 are the general
// combine's argument registers, which it fills after the sample and not before.
//
// The slice stride the upload writes - pitch * height, consecutive - is derived, not measured.
v_interp_p1_f32 v18, v0, attr2.w        // r
v_interp_p2_f32 v18, v1, attr2.w
v_mul_f32_e32 v18, v18, v12             // r/q
v_mov_b32_e32 v16, v2                   // s/q, already divided by the prolog
v_mov_b32_e32 v17, v3                   // t/q
image_sample v[4:7], v[16:18], s[4:11], s[12:15] dmask:0xf dim:SQ_RSRC_IMG_3D
// Cross-checks against words already in the tree: the 2D sample this replaces, the prolog's
// own divide of s by q, and its interpolation of attr1.x.
image_sample v[4:7], v[2:3], s[4:11], s[12:15] dmask:0xf dim:SQ_RSRC_IMG_2D
v_mul_f32_e32 v2, v2, v12
v_interp_p1_f32 v2, v0, attr1.x
